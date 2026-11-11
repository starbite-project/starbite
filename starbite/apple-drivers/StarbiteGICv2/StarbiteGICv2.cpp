/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteGICv2: GIC-400 interrupt controller for the Starbite S9 platform (see StarbiteGICv2.h, NOTES.md).
 * Register logic: hw/sb_gicv2.c (host-tested). Addresses: the provider nub's "reg" (GICD index 0, GICC index 1).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/c++/OSData.h>
#include <libkern/OSAtomic.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include "StarbiteGICv2.h"

extern "C" {
/* osfmk/arm64/machine_routines.c: sets the calling CPU's cpu_data interrupt handler (what sleh_irq calls for an
 * INTID that is neither the timer nor an SGI). Kernel-internal: fine built in; a KC build needs it exported. */
void ml_install_interrupt_handler(void *nub, int source, void *target, IOInterruptHandler handler, void *refCon);
int cpu_number(void);
}

#define super IOInterruptController
OSDefineMetaClassAndStructors(StarbiteGICv2, IOInterruptController);

#define GICLOG(fmt, ...) IOLog("StarbiteGICv2: " fmt "\n", ##__VA_ARGS__)

static __attribute__((unused)) uint32_t
gic_mmio_rd(void *ctx, uint32_t off)
{
	return *(volatile uint32_t *)((uintptr_t)ctx + off);
}

static __attribute__((unused)) void
gic_mmio_wr(void *ctx, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)((uintptr_t)ctx + off) = value;
}

static inline void
dsb_ishst(void)
{
	__asm__ volatile ("dsb ishst" ::: "memory");
}

bool
StarbiteGICv2::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	fProvider = provider;
#if !USE_APPLEARMSMP
	/* Today's kernels route interrupts through IOCPUInterruptController (SweetCPU); two owners would fight. */
	GICLOG("this kernel is not built with USE_APPLEARMSMP: not taking the GIC (see INTEGRATION.md)");
	return false;
#else
	if (provider->getDeviceMemoryCount() < 2) {
		GICLOG("provider has %u reg ranges, need GICD and GICC", (unsigned)provider->getDeviceMemoryCount());
		return false;
	}
	fDistMap = provider->mapDeviceMemoryWithIndex(0);
	fCpuMap = provider->mapDeviceMemoryWithIndex(1);
	fDistLock = IOSimpleLockAlloc();
	if (fDistMap == nullptr || fCpuMap == nullptr || fDistLock == nullptr) {
		GICLOG("cannot map GICD/GICC");
		return false;
	}
	fGic.d.ctx = (void *)fDistMap->getVirtualAddress();
	fGic.d.rd = gic_mmio_rd;
	fGic.d.wr = gic_mmio_wr;
	fGic.c.ctx = (void *)fCpuMap->getVirtualAddress();
	fGic.c.rd = gic_mmio_rd;
	fGic.c.wr = gic_mmio_wr;
	if (sb_gic_probe(&fGic) != SB_GIC_OK) {
		GICLOG("GICD_TYPER reads 0x%08x at %#llx: no GIC there", fGic.typer,
		    (unsigned long long)fDistMap->getPhysicalAddress());
		return false;
	}
	fNumVectors = fGic.nlines;

	vectors = IONewZero(IOInterruptVector, fNumVectors);
	if (vectors == nullptr) {
		return false;
	}
	for (uint32_t i = GIC_FIRST_SPI; i < fNumVectors; i++) {
		vectors[i].interruptLock = IOLockAlloc();
		if (vectors[i].interruptLock == nullptr) {
			return false;
		}
	}

	/* The CPU running start() is the boot CPU (the secondaries start only after this registers below). */
	fHandler = OSMemberFunctionCast(IOInterruptHandler, this, &StarbiteGICv2::handleInterrupt);
	boolean_t ints = ml_set_interrupts_enabled(FALSE);
	unsigned boot = (unsigned)cpu_number();
	cpuBooted(boot);
	ml_set_interrupts_enabled(ints);
	if (fCPU[boot].maskError != SB_GIC_OK) {
		GICLOG("boot CPU %u: ITARGETSR0 reads 0x%02x (error %d)", boot, fCPU[boot].ifaceMask, fCPU[boot].maskError);
		return false;
	}
	fBootMask = fCPU[boot].ifaceMask;

	uint32_t ctlr = fGic.d.rd(fGic.d.ctx, GICD_CTLR);
	IOSimpleLockLock(fDistLock);
	int e = sb_gic_dist_init_spis(&fGic, fBootMask);
	IOSimpleLockUnlock(fDistLock);
	if (e != SB_GIC_OK) {
		GICLOG("SPI set-up failed: %d", e);
		return false;
	}
	GICLOG("GIC-400 at %#llx/%#llx: IIDR 0x%08x TYPER 0x%08x, %u INTIDs, %u CPU interfaces, security ext %s, "
	    "GICD_CTLR was 0x%x; boot CPU %u = interface bit 0x%02x; all %u SPIs off, level, prio 0x%02x, to the boot CPU",
	    (unsigned long long)fDistMap->getPhysicalAddress(), (unsigned long long)fCpuMap->getPhysicalAddress(),
	    fGic.iidr, fGic.typer, fNumVectors, fGic.ncpu_ifaces, (fGic.typer & GICD_TYPER_SECEXT) ? "yes" : "no",
	    ctlr, boot, fBootMask, fNumVectors - GIC_FIRST_SPI, SB_GIC_DEFAULT_PRIO);

	/* every later CPU reports in on itself (AppleARMSMP PE_cpu_machine_init -> ml_broadcast_cpu_event) */
	cpu_event_register_callback(&StarbiteGICv2::cpuEvent, this);

	/* the name the IODT gave our children's interrupt-parent (IODTInterruptControllerName's format) */
	OSData *ph = OSDynamicCast(OSData, provider->getProperty("AAPL,phandle"));
	if (ph == nullptr || ph->getLength() != sizeof(uint32_t)) {
		GICLOG("gic node has no AAPL,phandle: no device can name it as interrupt-parent");
		return false;
	}
	char buf[48];
	snprintf(buf, sizeof(buf), "IOInterruptController%08X", *(const uint32_t *)ph->getBytesNoCopy());
	fName = const_cast<OSSymbol *>(OSSymbol::withCString(buf));
	if (fName == nullptr) {
		return false;
	}
	if (provider->getProperty("#interrupt-cells") == nullptr) {
		GICLOG("WARNING: gic node has no #interrupt-cells: IODT maps no interrupt-parent to it (INTEGRATION.md)");
	}
	setProperty("InterruptControllerName", fName);
	getPlatform()->registerInterruptController(fName, this);
	registerService();

	/* hand ourselves to AppleARMSMP's PassthruInterruptController (it may not exist yet: wait off this thread) */
	thread_t t;
	retain();
	if (kernel_thread_start(&StarbiteGICv2::attachThread, this, &t) != KERN_SUCCESS) {
		release();
		GICLOG("cannot start the attach thread");
		return false;
	}
	thread_deallocate(t);
	return true;
#endif /* USE_APPLEARMSMP */
}

void
StarbiteGICv2::attachThread(void *arg, wait_result_t wr __unused)
{
	StarbiteGICv2 *self = (StarbiteGICv2 *)arg;
	IOInterruptController *passthru = getPlatform()->lookUpInterruptController(gPlatformInterruptControllerName);
	/* AppleARMSMP's cpu_boot_thread: waitForChildController() returns `target` as the controller it calls
	 * sendIPI() on. The handler is never called on this platform (sleh_irq does not use PE_handle_ext_interrupt). */
	IOReturn r = passthru->registerInterrupt(self, 0, self, self->fHandler, nullptr);
	GICLOG("attached to the platform interrupt controller (%s): %x", passthru->getMetaClass()->getClassName(), r);

	/* one summary once every CPU has run its CPU_BOOTED (AppleARMSMP publishes IOAllCPUInitialized after) */
	OSDictionary *m = serviceMatching("IOResources");
	if (m != nullptr) {
		m = propertyMatching(gIOAllCPUInitializedKey, kOSBooleanTrue, m);
	}
	IOService *res = m ? waitForMatchingService(m, 60ULL * NSEC_PER_SEC) : nullptr;
	OSSafeReleaseNULL(m);
	char line[160];
	int n = 0;
	for (unsigned i = 0; i < SB_GIC_MAX_CPUS && n < (int)sizeof(line) - 24; i++) {
		if (self->fCPU[i].booted) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %u:0x%02x%s", i, self->fCPU[i].ifaceMask,
			    (self->fCPU[i].giccCtlr & GICC_CTLR_ENABLE) ? "" : "(GICC OFF)");
		}
	}
	GICLOG("%s; CPU interface bits:%s; SGI registrations %u, strays %u",
	    res ? "all CPUs initialized" : "IOAllCPUInitialized NOT seen in 60 s", line, self->fSgiRegistrations,
	    self->fStray);
	OSSafeReleaseNULL(res);
	self->release();
	thread_terminate(current_thread());
}

bool
StarbiteGICv2::cpuEvent(void *param, enum cpu_event event, unsigned int cpu_or_cluster)
{
	if (event == CPU_BOOTED) {
		((StarbiteGICv2 *)param)->cpuBooted(cpu_or_cluster);
	}
	return true;
}

/* On the CPU itself, interrupts off, may not block (CPU_BOOTED). */
void
StarbiteGICv2::cpuBooted(unsigned int cpu)
{
	unsigned self = (unsigned)cpu_number();
	if (cpu != self || cpu >= SB_GIC_MAX_CPUS) {
		panic("StarbiteGICv2: CPU_BOOTED for cpu %u delivered on cpu %u", cpu, self);
	}
	PerCPU *p = &fCPU[cpu];
	p->maskError = sb_gic_record_own_mask(&fGic, cpu, &p->ifaceMask);
	if (p->maskError != SB_GIC_OK && fBootMask != 0) {
		/* without its bit no other CPU can signal this one: the scheduler would hang on it */
		panic("StarbiteGICv2: cpu %u GICD_ITARGETSR0 reads 0x%02x (error %d)", cpu, p->ifaceMask, p->maskError);
	}
	p->giccCtlr = fGic.c.rd(fGic.c.ctx, GICC_CTLR);
	p->sgiPpiEnable = fGic.d.rd(fGic.d.ctx, GICD_ISENABLER);
	ml_install_interrupt_handler(fProvider, 0, this, fHandler, nullptr);
	__atomic_store_n(&p->booted, 1, __ATOMIC_RELEASE);
}

bool
StarbiteGICv2::vectorOf(IOService *nub, int source, uint32_t *intid)
{
	if (nub == nullptr || nub->_interruptSources == nullptr || source < 0 ||
	    source >= nub->_numInterruptSources) {
		return false;
	}
	OSData *d = nub->_interruptSources[source].vectorData;
	if (d == nullptr || d->getLength() < sizeof(uint32_t)) {
		return false;
	}
	*intid = *(const uint32_t *)d->getBytesNoCopy();
	return true;
}

IOReturn
StarbiteGICv2::registerInterrupt(IOService *nub, int source, void *target, IOInterruptHandler handler,
    void *refCon)
{
	uint32_t intid;
	if (!vectorOf(nub, source, &intid)) {
		return kIOReturnBadArgument;
	}
	switch (sb_gic_register_policy(&fGic, intid)) {
	case SB_GIC_POL_PEXPERT:
		/* a cpu node's IPI entry (AppleARMSMP register_aic_handlers): sleh_irq dispatches SGIs itself */
		__atomic_fetch_add(&fSgiRegistrations, 1, __ATOMIC_RELAXED);
		return kIOReturnSuccess;
	case SB_GIC_POL_VECTOR:
		return super::registerInterrupt(nub, source, target, handler, refCon);
	case SB_GIC_POL_REFUSE:
	default:
		GICLOG("%s: interrupt %d = INTID %u refused (only SPIs 32..%u, and SGIs for CPU nodes)", nub->getName(),
		    source, intid, fNumVectors - 1);
		return kIOReturnUnsupported;
	}
}

#define SB_GIC_FORWARD(call, sgiResult)                                         \
	uint32_t intid;                                                         \
	if (!vectorOf(nub, source, &intid)) {                                   \
	        return kIOReturnBadArgument;                                    \
	}                                                                       \
	switch (sb_gic_register_policy(&fGic, intid)) {                         \
	case SB_GIC_POL_PEXPERT:                                                \
	        return sgiResult;                                               \
	case SB_GIC_POL_VECTOR:                                                 \
	        return call;                                                    \
	default:                                                                \
	        return kIOReturnUnsupported;                                    \
	}

IOReturn
StarbiteGICv2::unregisterInterrupt(IOService *nub, int source)
{
	SB_GIC_FORWARD(super::unregisterInterrupt(nub, source), kIOReturnSuccess)
}

IOReturn
StarbiteGICv2::getInterruptType(IOService *nub, int source, int *interruptType)
{
	uint32_t intid;
	if (interruptType == nullptr || !vectorOf(nub, source, &intid)) {
		return kIOReturnBadArgument;
	}
	switch (sb_gic_register_policy(&fGic, intid)) {
	case SB_GIC_POL_PEXPERT:
		*interruptType = kIOInterruptTypeEdge;  /* SGIs are edge-triggered */
		return kIOReturnSuccess;
	case SB_GIC_POL_VECTOR:
		return super::getInterruptType(nub, source, interruptType);
	default:
		return kIOReturnUnsupported;
	}
}

IOReturn
StarbiteGICv2::enableInterrupt(IOService *nub, int source)
{
	/* SGI 1 is enabled by pexpert on every CPU (banked); nothing to do */
	SB_GIC_FORWARD(super::enableInterrupt(nub, source), kIOReturnSuccess)
}

IOReturn
StarbiteGICv2::disableInterrupt(IOService *nub, int source)
{
	/* never mask the IPI: XNU cannot run without it */
	SB_GIC_FORWARD(super::disableInterrupt(nub, source), kIOReturnUnsupported)
}

IOReturn
StarbiteGICv2::causeInterrupt(IOService *nub, int source)
{
	SB_GIC_FORWARD(super::causeInterrupt(nub, source), kIOReturnUnsupported)
}

IOInterruptAction
StarbiteGICv2::getInterruptHandlerAddress(void)
{
	return OSMemberFunctionCast(IOInterruptAction, this, &StarbiteGICv2::handleInterrupt);
}

/* sleh_irq, after GICC_IAR, for any INTID that is not PPI 27 or an SGI; sleh writes GICC_EOIR after we return. */
IOReturn
StarbiteGICv2::handleInterrupt(void *refCon __unused, IOService *nub __unused, int source)
{
	uint32_t intid = (uint32_t)source;
	IOInterruptVector *vector = intid < fNumVectors ? &vectors[intid] : nullptr;
	bool registered = vector != nullptr && vector->interruptRegistered;
	bool soft = vector != nullptr && vector->interruptDisabledSoft;

	switch (sb_gic_handle_action(&fGic, intid, registered, soft)) {
	case SB_GIC_ACT_CALL:
		vector->interruptActive = 1;
		OSMemoryBarrier();
		if (!vector->interruptDisabledSoft) {
			vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
		} else {
			vector->interruptDisabledHard = 1;      /* disabled while we looked: lazy disable */
			disableVectorHard(intid, vector);
		}
		vector->interruptActive = 0;
		break;
	case SB_GIC_ACT_MASK:
		vector->interruptDisabledHard = 1;
		disableVectorHard(intid, vector);
		break;
	case SB_GIC_ACT_STRAY:
	default:
		__atomic_fetch_add(&fStray, 1, __ATOMIC_RELAXED);
		if (sb_gic_kind(&fGic, intid) == SB_GIC_KIND_SPI) {
			sb_gic_spi_disable(&fGic, intid);       /* set-enable bits: no lock needed */
		}
		if (fStrayLogged < 16) {
			fStrayLogged++;
			kprintf("StarbiteGICv2: stray INTID %u on cpu %d (nobody registered it): masked\n", intid, cpu_number());
		}
		break;
	}
	return kIOReturnSuccess;
}

bool
StarbiteGICv2::vectorCanBeShared(IOInterruptVectorNumber, IOInterruptVector *)
{
	return false;   /* the board gives every block its own SPIs (tools/board/starlte.board) */
}

void
StarbiteGICv2::initVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
	/* priority, boot-CPU target and level trigger as at start(); a client can re-route with setSPITarget */
	IOInterruptState is = IOSimpleLockLockDisableInterrupt(fDistLock);
	sb_gic_spi_set_priority(&fGic, (uint32_t)vectorNumber, SB_GIC_DEFAULT_PRIO);
	sb_gic_spi_set_target(&fGic, (uint32_t)vectorNumber, fBootMask);
	IOSimpleLockUnlockEnableInterrupt(fDistLock, is);
}

int
StarbiteGICv2::getVectorType(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
	return sb_gic_is_edge(&fGic, (uint32_t)vectorNumber) ? kIOInterruptTypeEdge : kIOInterruptTypeLevel;
}

void
StarbiteGICv2::disableVectorHard(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
	sb_gic_spi_disable(&fGic, (uint32_t)vectorNumber);
}

void
StarbiteGICv2::enableVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
	sb_gic_spi_enable(&fGic, (uint32_t)vectorNumber);
}

void
StarbiteGICv2::causeVector(IOInterruptVectorNumber vectorNumber, IOInterruptVector *)
{
	sb_gic_spi_set_pending(&fGic, (uint32_t)vectorNumber);
}

/* AppleARMSMP PE_cpu_signal / _deferred (cpu_signal_internal runs it with interrupts off). */
void
StarbiteGICv2::sendIPI(unsigned int cpu_id, bool deferred)
{
	boolean_t ints = ml_set_interrupts_enabled(FALSE);
	unsigned self = (unsigned)cpu_number();
	dsb_ishst();            /* the target must see its cpu_signal bits before the SGI */
	int e = sb_gic_send_ipi(&fGic, cpu_id, self, SB_GIC_IPI_SGI);
	ml_set_interrupts_enabled(ints);
	if (e != SB_GIC_OK) {
		panic("StarbiteGICv2: IPI cpu %u -> cpu %u: error %d (interface bit not known: cpu %u never booted?)",
		    self, cpu_id, e, cpu_id);
	}
	if (cpu_id < SB_GIC_MAX_CPUS) {
		/* GICv2 has no deferred IPIs: a deferred one is sent now (it only arrives early) */
		fCPU[cpu_id].ipisSent++;
		if (deferred) {
			fCPU[cpu_id].deferredSent++;
		}
	}
}

void
StarbiteGICv2::cancelDeferredIPI(unsigned int cpu_id __unused)
{
	/* already delivered (sendIPI sends deferred IPIs at once); a spurious wake-up is harmless */
}

IOReturn
StarbiteGICv2::setSPITarget(uint32_t intid, unsigned int logicalCPU)
{
	if (logicalCPU >= SB_GIC_MAX_CPUS || !__atomic_load_n(&fCPU[logicalCPU].booted, __ATOMIC_ACQUIRE)) {
		return kIOReturnNotReady;
	}
	IOInterruptState is = IOSimpleLockLockDisableInterrupt(fDistLock);
	int e = sb_gic_spi_set_target(&fGic, intid, fCPU[logicalCPU].ifaceMask);
	IOSimpleLockUnlockEnableInterrupt(fDistLock, is);
	return e == SB_GIC_OK ? kIOReturnSuccess : kIOReturnBadArgument;
}

void
StarbiteGICv2::free(void)
{
	/* only reached when start() failed: nothing was registered with the platform yet unless fName is set */
	if (vectors != nullptr) {
		for (uint32_t i = 0; i < fNumVectors; i++) {
			if (vectors[i].interruptLock != nullptr) {
				IOLockFree(vectors[i].interruptLock);
			}
		}
		IODelete(vectors, IOInterruptVector, fNumVectors);
		vectors = nullptr;
	}
	OSSafeReleaseNULL(fDistMap);
	OSSafeReleaseNULL(fCpuMap);
	if (fDistLock != nullptr) {
		IOSimpleLockFree(fDistLock);
		fDistLock = nullptr;
	}
	OSSafeReleaseNULL(fName);
	super::free();
}
