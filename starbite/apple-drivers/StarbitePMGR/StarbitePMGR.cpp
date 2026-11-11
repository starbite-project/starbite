/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbitePMGR: CPU bring-up for the Starbite S9 platform (see StarbitePMGR.h, NOTES.md). Sequencing and register
 * facts: hw/sb_smp.c and hw/sb_psci.c (host-tested). Addresses and IDs: the ADT only (/psci, /cpus/cpuN,
 * /arm-io/pmu "reg").
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include "StarbitePMGR.h"

extern "C" {
/* osfmk/arm64/start.s (SWEET): the PSCI CPU_ON entry, physical, MMU off; picks its cpu_data by MPIDR */
void sweet_secondary_entry(void);
int cpu_number(void);
/* kernel-internal (sched_prim.h, MACH_KERNEL_PRIVATE / cpu_common.c): the self-test only */
processor_t thread_bind(processor_t processor);
processor_t cpu_to_processor(int cpu);
typedef void (*sb_broadcast_fn)(void *);
kern_return_t cpu_xcall(int cpu_number, sb_broadcast_fn func, void *param);
#if defined(ARM64_BOARD_CONFIG_STARLTE)
/* bsd/dev/herolte_ufs.c, patch 0423: the milestone self-stop (dumps the console to pmsg, resets into TWRP) */
int starlte_sbstop_at(const char *stage);
void starlte_sbstop_now(const char *stage);
#endif
}

#define super IOPMGR
OSDefineMetaClassAndStructors(StarbitePMGR, IOPMGR);

#define PMLOG(fmt, ...) IOLog("StarbitePMGR: " fmt "\n", ##__VA_ARGS__)

/* SMCCC 1.0 fast call: x0 = result; x4-x17 may be clobbered. HVC is UNDEFINED at EL1 on the S9. */
static __attribute__((unused)) int64_t
smc_conduit(void *ctx __unused, uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	        "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
	        "cc", "memory");
	return (int64_t)x0;
}

static uint32_t
pmu_rd(void *ctx, uint32_t off)
{
	return *(volatile uint32_t *)((uintptr_t)ctx + off);
}

static void
pmu_wr(void *ctx, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)((uintptr_t)ctx + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
prop_u32(IORegistryEntry *e, const char *key, uint32_t *v)
{
	OSData *d = OSDynamicCast(OSData, e->getProperty(key));
	if (d == nullptr || d->getLength() != sizeof(uint32_t)) {
		return false;
	}
	*v = *(const uint32_t *)d->getBytesNoCopy();
	return true;
}

static bool
prop_u64(IORegistryEntry *e, const char *key, uint64_t *v)
{
	OSData *d = OSDynamicCast(OSData, e->getProperty(key));
	if (d == nullptr) {
		return false;
	}
	if (d->getLength() == sizeof(uint64_t)) {
		*v = *(const uint64_t *)d->getBytesNoCopy();
		return true;
	}
	if (d->getLength() == sizeof(uint32_t)) {
		*v = *(const uint32_t *)d->getBytesNoCopy();
		return true;
	}
	return false;
}

static const char *
smp_err(int e)
{
	switch (e) {
	case SB_SMP_OK: return "ok";
	case SB_SMP_E_INVAL: return "no such CPU";
	case SB_SMP_E_STATE: return "wrong state (boot CPU, or started twice)";
	case SB_SMP_E_DESC: return "ADT facts disagree";
	case SB_SMP_E_NOPMU: return "M3 but no PMU mapping";
	case SB_SMP_E_PMU: return "PMU CAL value did not read back";
	case SB_SMP_E_PSCI: return "PSCI CPU_ON failed";
	case SB_SMP_E_IDENT: return "the core that booted is not the one described";
	default: return "?";
	}
}

bool
StarbitePMGR::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
#if !USE_APPLEARMSMP
	PMLOG("this kernel is not built with USE_APPLEARMSMP: nobody would call an IOPMGR (see INTEGRATION.md)");
	(void)fPSCI.ready;
	return false;
#else
	/* PSCI 0.1: function IDs and the conduit from /psci; never PSCI_VERSION (it answers -1 here) */
	OSData *method = OSDynamicCast(OSData, provider->getProperty("method"));
	uint32_t on = 0, off = 0, susp = 0;
	prop_u32(provider, "cpu_on", &on);
	prop_u32(provider, "cpu_off", &off);
	prop_u32(provider, "cpu_suspend", &susp);
	int e = sb_psci_init(&fPSCI, method ? (const char *)method->getBytesNoCopy() : nullptr,
	    method ? method->getLength() : 0, on, off, susp, smc_conduit, nullptr);
	if (e != PSCI_SUCCESS) {
		/* AppleARMSMP would wait for an IOPMGR forever: say why, loudly, and stop */
		panic("StarbitePMGR: /psci unusable (error %d: method must be \"smc\", cpu_on 0x%x)", e, on);
	}
	fSMP.psci = &fPSCI;
	if (!readTopology() || !setUpEntry(provider)) {
		panic("StarbitePMGR: CPU topology / entry set-up failed (see the lines above)");
	}
	bool needPMU = false;
	for (unsigned i = 0; i < fSMP.ncpus; i++) {
		needPMU |= fSMP.cpu[i].kind == 'P';
	}
	if (needPMU && !mapPMU()) {
		PMLOG("WARNING: no /arm-io/pmu mapping: the M3 cores will not be started");
	}
	PMLOG("PSCI 0.1 over SMC, CPU_ON 0x%08x; %u CPUs selected; entry %#llx%s; PMU %s", on, fSMP.ncpus,
	    (unsigned long long)fEntryPA, fTrampolinePA ? " via trampoline" : "",
	    fPMUMap ? "mapped" : (needPMU ? "MISSING" : "not needed (no M3 selected)"));

	cpu_event_register_callback(&StarbitePMGR::cpuEvent, this);
	registerService();

	thread_t t;
	retain();
	if (kernel_thread_start(&StarbitePMGR::selfTestThread, this, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	} else {
		release();
	}
	return true;
#endif
}

/* /cpus/cpuN for every CPU XNU selected (cpus=N), matched by "reg" = ml_topology_cpu.phys_id */
bool
StarbitePMGR::readTopology(void)
{
	const ml_topology_info_t *topo = ml_get_topology_info();
	IORegistryEntry *cpus = IORegistryEntry::fromPath("/cpus", gIODTPlane);
	if (cpus == nullptr || topo->num_cpus > SB_SMP_MAX_CPUS) {
		PMLOG("no /cpus, or %u CPUs > %u", topo->num_cpus, SB_SMP_MAX_CPUS);
		OSSafeReleaseNULL(cpus);
		return false;
	}
	bool ok = true;
	for (unsigned i = 0; i < topo->num_cpus; i++) {
		const ml_topology_cpu_t *tc = &topo->cpus[i];
		IORegistryEntry *node = nullptr;
		OSIterator *it = cpus->getChildIterator(gIODTPlane);
		while (it != nullptr && (node = OSDynamicCast(IORegistryEntry, it->getNextObject())) != nullptr) {
			uint32_t reg;
			if (prop_u32(node, "reg", &reg) && reg == tc->phys_id) {
				break;
			}
		}
		struct sb_cpu_desc d = {};
		d.reg = tc->phys_id;
		d.is_boot = tc == topo->boot_cpu;
		OSData *kind = node ? OSDynamicCast(OSData, node->getProperty("starbite,cluster-kind")) : nullptr;
		if (node == nullptr || !prop_u64(node, "starbite,mpidr", &d.mpidr) ||
		    !prop_u64(node, "starbite,psci-target", &d.psci_target) || kind == nullptr || kind->getLength() < 1) {
			PMLOG("cpu %u (reg %#x): no ADT node with starbite,mpidr / psci-target / cluster-kind "
			    "(tools/board/mkadt-board.py output needed)", tc->cpu_id, tc->phys_id);
			OSSafeReleaseNULL(it);
			ok = false;
			continue;
		}
		d.kind = *(const char *)kind->getBytesNoCopy();
		OSSafeReleaseNULL(it);
		int e = sb_smp_add(&fSMP, tc->cpu_id, &d);
		const char *xnu = tc->cluster_type == CLUSTER_TYPE_E ? "E" : tc->cluster_type == CLUSTER_TYPE_P ? "P" : "SMP";
		PMLOG("cpu %u: reg %#x mpidr %#llx psci %#llx kind %c, XNU cluster %u type %s%s%s", tc->cpu_id,
		    tc->phys_id, (unsigned long long)d.mpidr, (unsigned long long)d.psci_target, d.kind, tc->cluster_id,
		    xnu, d.is_boot ? " (boot)" : "", e == SB_SMP_OK ? "" : ": INCONSISTENT, will not be started");
#if __ARM_AMP__
		if ((d.kind == 'E') != (tc->cluster_type == CLUSTER_TYPE_E)) {
			PMLOG("cpu %u: ADT cluster-type does not match starbite,cluster-kind %c", tc->cpu_id, d.kind);
		}
#endif
	}
	cpus->release();
	return ok;
}

bool
StarbitePMGR::mapPMU(void)
{
	OSDictionary *m = nameMatching("starbite,pmu");
	IOService *nub = m ? waitForMatchingService(m, 5ULL * NSEC_PER_SEC) : nullptr;
	OSSafeReleaseNULL(m);
	if (nub == nullptr) {
		return false;
	}
	fPMUMap = nub->mapDeviceMemoryWithIndex(0);
	nub->release();
	if (fPMUMap == nullptr || fPMUMap->getLength() < 0x3000) {
		OSSafeReleaseNULL(fPMUMap);
		return false;
	}
	fPMUIo.ctx = (void *)fPMUMap->getVirtualAddress();
	fPMUIo.rd = pmu_rd;
	fPMUIo.wr = pmu_wr;
	fSMP.pmu = &fPMUIo;
	PMLOG("PMU at %#llx (+%#llx)", (unsigned long long)fPMUMap->getPhysicalAddress(),
	    (unsigned long long)fPMUMap->getLength());
	return true;
}

/*
 * The entry CPU_ON gets. Open question (NOTES.md Q1): the S7's firmware kept only 32 bits of it (patch 0223);
 * the S9's is unmeasured above 4 GiB. If the kernel is above 4 GiB and the ADT /psci node names a free page
 * below (starbite,entry-trampoline), a 16-byte trampoline goes there.
 */
bool
StarbitePMGR::setUpEntry(IOService *provider)
{
	fEntryPA = ml_static_vtop((vm_offset_t)&sweet_secondary_entry);
	if (fEntryPA == 0) {
		PMLOG("no physical address for sweet_secondary_entry");
		return false;
	}
	uint64_t tramp = 0;
	if ((fEntryPA >> 32) == 0 || !prop_u64(provider, "starbite,entry-trampoline", &tramp) || tramp == 0) {
		if (fEntryPA >> 32) {
			PMLOG("WARNING: entry %#llx is above 4 GiB and the ADT names no trampoline page; if the cores do not "
			    "check in, the firmware truncated it (NOTES.md Q1)", (unsigned long long)fEntryPA);
		}
		return true;
	}
	if ((tramp >> 32) != 0 || (tramp & 0xf) != 0) {
		PMLOG("starbite,entry-trampoline %#llx unusable (must be below 4 GiB, 16-byte aligned)",
		    (unsigned long long)tramp);
		return true;
	}
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)tramp, 16,
	    kIODirectionInOut);
	fTrampMap = md ? md->map(kIOMapInhibitCache) : nullptr;
	OSSafeReleaseNULL(md);
	if (fTrampMap == nullptr) {
		PMLOG("cannot map the trampoline page %#llx", (unsigned long long)tramp);
		return true;
	}
	uint32_t words[4];
	sb_smp_trampoline(words, fEntryPA);
	volatile uint32_t *code = (volatile uint32_t *)fTrampMap->getVirtualAddress();
	for (unsigned i = 0; i < 4; i++) {
		code[i] = words[i];
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	fTrampolinePA = tramp;
	return true;
}

/* AppleARMSMP PE_cpu_start_internal (processor_boot -> cpu_start), on the boot CPU, for each secondary */
void
StarbitePMGR::enableCPUCore(unsigned int cpu_id, uint64_t entry_pa)
{
	if (entry_pa != 0 && entry_pa != fEntryPA) {
		kprintf("StarbitePMGR: cpu %u: XNU passed entry %#llx, using sweet_secondary_entry %#llx\n", cpu_id,
		    (unsigned long long)entry_pa, (unsigned long long)fEntryPA);
	}
	int32_t rc = 0;
	uint64_t entry = sb_smp_entry(fEntryPA, fTrampolinePA);
	int e = sb_smp_cpu_on(&fSMP, cpu_id, entry, &rc);
	if (e != SB_SMP_OK) {
		/*
		 * No panic: the core simply stays off and the self-test reports it ("cluster P: 3 up of 4"), which says
		 * more than a panic would. XNU never schedules on a core that did not check in.
		 */
		IOLog("StarbitePMGR: cpu %u NOT started: %s (PSCI %d %s%s%#x)\n", cpu_id, smp_err(e), rc,
		    sb_psci_strerror(rc), e == SB_SMP_E_PMU ? ", PMU offset " : ", -", e == SB_SMP_E_PMU ?
		    fSMP.pmu_bad_offset : 0);
		return;
	}
	kprintf("StarbitePMGR: cpu %u CPU_ON target %#llx entry %#llx: rc 0%s\n", cpu_id,
	    (unsigned long long)fSMP.cpu[cpu_id].psci_target, (unsigned long long)entry,
	    fSMP.cpu[cpu_id].kind == 'P' ? " (PMU CAL written and read back first)" : "");
}

void
StarbitePMGR::enableCPUCore(unsigned int cpu_id)
{
	enableCPUCore(cpu_id, 0);
}

void
StarbitePMGR::disableCPUCore(unsigned int cpu_id)
{
	/* only reached for runtime CPU exit (boot-arg) or sleep, neither of which this platform offers yet */
	panic("StarbitePMGR: cpu %u offline / sleep is not supported (no CPU_OFF path yet)", cpu_id);
}

void
StarbitePMGR::enableCPUCluster(unsigned int cluster_id)
{
	/* both clusters are powered at boot and never cut (the ADT has no cluster-power-down) */
	kprintf("StarbitePMGR: enableCPUCluster(%u): already on\n", cluster_id);
}

void
StarbitePMGR::disableCPUCluster(unsigned int cluster_id)
{
	kprintf("StarbitePMGR: disableCPUCluster(%u): ignored, clusters stay powered\n", cluster_id);
}

void
StarbitePMGR::initCPUIdle(ml_processor_info_t *info)
{
	info->supports_nap = FALSE;     /* idle = plain WFI, done by cpu_idle() */
}

void
StarbitePMGR::enterCPUIdle(UInt64 *newIdleTimeoutTicks)
{
	if (newIdleTimeoutTicks != nullptr) {
		*newIdleTimeoutTicks = 0;       /* no idle timer */
	}
}

void
StarbitePMGR::exitCPUIdle(UInt64 *newIdleTimeoutTicks)
{
	if (newIdleTimeoutTicks != nullptr) {
		*newIdleTimeoutTicks = 0;
	}
}

void
StarbitePMGR::updateCPUIdle(UInt64 *newIdleTimeoutTicks)
{
	if (newIdleTimeoutTicks != nullptr) {
		*newIdleTimeoutTicks = 0;
	}
}

/* On the core itself, interrupts off, may not block. */
bool
StarbitePMGR::cpuEvent(void *param, enum cpu_event event, unsigned int cpu)
{
	if (event != CPU_BOOTED) {
		return true;
	}
	StarbitePMGR *self = (StarbitePMGR *)param;
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1"), midr = __builtin_arm_rsr64("MIDR_EL1");
	int e = sb_smp_booted(&self->fSMP, cpu, mpidr, midr);
	__atomic_fetch_add(&self->fBootedCount, 1, __ATOMIC_RELEASE);
	if (e != SB_SMP_OK) {
		kprintf("StarbitePMGR: cpu %u checked in as mpidr %#llx midr %#llx: %s\n", cpu,
		    (unsigned long long)mpidr, (unsigned long long)midr, smp_err(e));
	}
	return true;
}

void
StarbitePMGR::selfTestThread(void *arg, wait_result_t wr __unused)
{
	StarbitePMGR *self = (StarbitePMGR *)arg;
	self->selfTest();
	self->release();
	thread_terminate(current_thread());
}

struct sb_ipi_probe {
	volatile uint32_t ran;
	volatile int cpu;
};

static void
sb_ipi_probe_fn(void *arg)
{
	struct sb_ipi_probe *p = (struct sb_ipi_probe *)arg;
	p->cpu = cpu_number();
	__atomic_store_n(&p->ran, 1, __ATOMIC_RELEASE);
}

void
StarbitePMGR::selfTest(void)
{
	OSDictionary *m = serviceMatching("IOResources");
	if (m != nullptr) {
		m = propertyMatching(gIOAllCPUInitializedKey, kOSBooleanTrue, m);
	}
	IOService *res = m ? waitForMatchingService(m, 60ULL * NSEC_PER_SEC) : nullptr;
	OSSafeReleaseNULL(m);
	if (res == nullptr) {
		PMLOG("IOAllCPUInitialized not published in 60 s");
	}
	OSSafeReleaseNULL(res);

	/* every started core must check in (CPU_BOOTED on itself) within 5 s */
	unsigned n = fSMP.ncpus;
	for (unsigned waited = 0; waited < 5000; waited += 10) {
		unsigned pending = 0;
		for (unsigned i = 0; i < n; i++) {
			pending += fSMP.state[i] == SB_CPU_ON_SENT;
		}
		if (pending == 0) {
			break;
		}
		IOSleep(10);
	}

	struct sb_smp_tally t = {};
	t.ipi_ok = t.timer_ok = true;
	t.timebase_hz = gPEClockFrequencyInfo.timebase_frequency_hz;
	bool ipi[SB_SMP_MAX_CPUS] = {}, tick[SB_SMP_MAX_CPUS] = {};
	uint64_t tick_us[SB_SMP_MAX_CPUS] = {};

	/* IPIs: from CPU 0 to every other core (cpu_xcall: GICD_SGIR -> sleh SGI -> cpu_signal_handler) */
	processor_t p0 = cpu_to_processor(0);
	thread_bind(p0);
	thread_block(THREAD_CONTINUE_NULL);
	for (unsigned i = 1; i < n; i++) {
		if (fSMP.state[i] != SB_CPU_UP) {
			continue;
		}
		struct sb_ipi_probe probe = { 0, -1 };
		kern_return_t kr = KERN_FAILURE;
		for (unsigned tries = 0; tries < 100 && kr != KERN_SUCCESS; tries++) {
			kr = cpu_xcall((int)i, sb_ipi_probe_fn, &probe);    /* fails while SIGPdisabled */
			if (kr != KERN_SUCCESS) {
				IOSleep(1);
			}
		}
		for (unsigned us = 0; kr == KERN_SUCCESS && us < 100000 && !__atomic_load_n(&probe.ran, __ATOMIC_ACQUIRE);
		    us += 10) {
			IODelay(10);
		}
		ipi[i] = kr == KERN_SUCCESS && probe.ran && probe.cpu == (int)i;
	}
	ipi[0] = fSMP.state[0] == SB_CPU_UP;    /* its self-SGI cleared SIGPdisabled before this thread ran */

	/* the timer: a 20 ms sleep bound to each core wakes on that core (its PPI 27 fires) */
	for (unsigned i = 0; i < n; i++) {
		if (fSMP.state[i] != SB_CPU_UP) {
			continue;
		}
		thread_bind(cpu_to_processor((int)i));
		thread_block(THREAD_CONTINUE_NULL);
		int before = cpu_number();
		uint64_t a = mach_absolute_time(), ns;
		IOSleep(20);
		absolutetime_to_nanoseconds(mach_absolute_time() - a, &ns);
		tick_us[i] = ns / 1000;
		tick[i] = before == (int)i && cpu_number() == (int)i && ns >= 19000000ULL && ns <= 500000000ULL;
	}
	thread_bind(PROCESSOR_NULL);
	thread_block(THREAD_CONTINUE_NULL);

	for (unsigned i = 0; i < n; i++) {
		unsigned k = fSMP.cpu[i].kind == 'P';
		bool up = fSMP.state[i] == SB_CPU_UP;
		t.selected[k]++;
		t.up[k] += up;
		t.ipi_ok &= ipi[i];
		t.timer_ok &= tick[i];
		PMLOG("cpu %u %c mpidr %#llx midr %#llx (%s): %s, IPI %s, timer %llu us", i, fSMP.cpu[i].kind,
		    (unsigned long long)fSMP.seen_mpidr[i], (unsigned long long)fSMP.seen_midr[i],
		    sb_midr_name(fSMP.seen_midr[i]),
		    up ? "up" : fSMP.state[i] == SB_CPU_ON_SENT ? "CPU_ON ok but NEVER CHECKED IN" : "NOT STARTED",
		    ipi[i] ? "ok" : "FAILED", (unsigned long long)tick_us[i]);
	}
	char line[96];
	sb_smp_pass_line(line, sizeof(line), &t);
	PMLOG("M2 %s: %s", sb_smp_tally_pass(&t) ? "PASS" : "FAIL", line);
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (starlte_sbstop_at("smp")) {
		starlte_sbstop_now("smp");
	}
#endif
}
