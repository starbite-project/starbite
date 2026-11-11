/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteUFS: IOKit glue for the fenced UFS host (see StarbiteUFS.h, NOTES.md, PHONE-TEST.md).
 *
 * Register facts come from Starbite's own build/xnu-12377/bsd/dev/herolte_ufs.c (patches 0204/0300, proven on the
 * S9 since 2026-10-25) and, as facts only, from an earlier port's APSL-2.0 drivers/storage/ufs/ufshc.c (an earlier port
 * Project, "an earlier port"): credit in NOTES.md. No GPL code (starbite/libsb, Linux) is used.
 *
 * Addresses come only from the provider nub (ADT "reg" of /arm-io/ufs: HCI, vendor HCI, UniPro, UFSP), the
 * interrupt only from its "interrupts". The allow-list, deny-list and write switch come from the nub's
 * "starbite,allowed-ranges", "starbite,denied-ranges", "starbite,writes-allowed" (format: NOTES.md).
 *
 * Boot-args (all optional):
 *   sbufsrw=1       writes, if the ADT also says starbite,writes-allowed = 1 and the range is WRITABLE (default 0)
 *   sbufsirq=0      polled only (default 1: the nub's interrupt, with a 20 ms safety poll)
 *   sbufsmulti=0    one 4 KiB block per command (default 1: 16 after the read-only self test passes)
 *   sbufsqd=N       slots in use, 1..8 (default 4)
 *   sbufsbounce=1   every transfer through the slot bounce buffers (default 0: direct DMA where it fits)
 *   sbufs64=1       allow DMA above 4 GiB if CAP.64AS (default 0)
 *   sbufscached=1   cacheable descriptor memory with explicit maintenance, one slot (default 0: write-combined)
 *   sbufsreport=S   S s after start: IOMedia tree, root line, fence summary again (default 0: off). The tree is
 *                   always printed once the media settle, the root line once the root is mounted.
 *   sbstop=ufs      (0423) after the first IOMedia tree: stop and reset into TWRP
 *   sbstop=ufsroot  after the sbufsreport report: stop and reset into TWRP
 */
#include "StarbiteUFS.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/vnode.h>
#include <sys/mount.h>

#ifndef STARBITEUFS_SBSTOP
#define STARBITEUFS_SBSTOP 1            /* built into a STARLTE kernel: herolte_ufs.c (0423) provides these */
#endif
#if STARBITEUFS_SBSTOP
extern "C" int starlte_sbstop_at(const char *stage);
extern "C" void starlte_sbstop_now(const char *stage);
#endif

#define LOG(fmt, ...)   IOLog("StarbiteUFS: " fmt "\n", ##__VA_ARGS__)

static const uint32_t kPollUsPolled = 250;      /* polled mode, while commands are in flight */
static const uint32_t kPollMsIrq = 20;          /* interrupt mode: lost-interrupt safety net and timeouts */
static const uint32_t kTreeSettleMs = 3000;
static const unsigned kMaxUARetries = 3;

OSDefineMetaClassAndStructors(StarbiteUFS, IOService);
OSDefineMetaClassAndStructors(StarbiteUFSRange, IOBlockStorageDevice);

/* ---------------------------------------------------------------------------------------------------------------- */
/* sbufs_hw_ops                                                                                                      */

uint32_t
StarbiteUFS::hwRead(void *ctx, unsigned bank, uint32_t off)
{
	StarbiteUFS *self = (StarbiteUFS *)ctx;
	volatile uint32_t *b = bank == SBUFS_BANK_HCI ? self->fHci : bank == SBUFS_BANK_VS ? self->fVs : NULL;
	return (b != NULL && off < 0x200) ? b[off / 4] : 0xffffffffu;
}

void
StarbiteUFS::hwWrite(void *ctx, unsigned bank, uint32_t off, uint32_t v)
{
	StarbiteUFS *self = (StarbiteUFS *)ctx;
	/* the UniPro and UFSP banks are never written */
	volatile uint32_t *b = bank == SBUFS_BANK_HCI ? self->fHci : bank == SBUFS_BANK_VS ? self->fVs : NULL;
	if (b != NULL && off < 0x200) {
		b[off / 4] = v;
	}
}

void
StarbiteUFS::hwBarrier(void *ctx)
{
	(void)ctx;
	__builtin_arm_dsb(0xf);
}

uint64_t
StarbiteUFS::hwNowUs(void *ctx)
{
	(void)ctx;
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

void
StarbiteUFS::hwDelayUs(void *ctx, uint32_t us)
{
	(void)ctx;
	IODelay(us);
}

void
StarbiteUFS::hwLog(void *ctx, const char *line)
{
	(void)ctx;
	IOLog("StarbiteUFS: %s\n", line);
}

void
StarbiteUFS::hwClean(void *ctx, uint32_t off, uint32_t len)
{
	StarbiteUFS *self = (StarbiteUFS *)ctx;
	self->fDma->performOperation(kIOMemoryIncoherentIOStore, off, len);
}

void
StarbiteUFS::hwInval(void *ctx, uint32_t off, uint32_t len)
{
	StarbiteUFS *self = (StarbiteUFS *)ctx;
	self->fDma->performOperation(kIOMemoryIncoherentIOFlush, off, len);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* start-up                                                                                                          */

static uint32_t
bootArg(const char *name, uint32_t dflt)
{
	uint32_t v = dflt;
	(void)PE_parse_boot_argn(name, &v, sizeof(v));
	return v;
}

bool
StarbiteUFS::readConfig(IOService *provider)
{
	char rd[32] = "";
	char err[160] = "";
	uint32_t probe = 0;

	/* herolte_ufs.c drives the same host for rd=ufsroot0 / ufsprobe=1: never both */
	if ((PE_parse_boot_argn("rd", rd, sizeof(rd) - 1) && strncmp(rd, "ufsroot", 7) == 0) ||
	    (PE_parse_boot_argn("ufsprobe", &probe, sizeof(probe)) && probe != 0)) {
		LOG("rd=%s ufsprobe=%u: herolte_ufs owns the host in this boot, not starting", rd, probe);
		return false;
	}
	fArgRW = bootArg("sbufsrw", 0);
	fArgIrq = bootArg("sbufsirq", 1);
	fArgMulti = bootArg("sbufsmulti", 1);
	fArgQD = bootArg("sbufsqd", 4);
	fArgBounce = bootArg("sbufsbounce", 0);
	fArg64 = bootArg("sbufs64", 0);
	fArgCached = bootArg("sbufscached", 0);
	fArgReport = bootArg("sbufsreport", 0);
	if (!PE_parse_boot_argn("sbstop", fStop, sizeof(fStop) - 1)) {
		fStop[0] = '\0';
	}
	if (fArgQD < 1) {
		fArgQD = 1;
	}
	if (fArgQD > SBUFS_MAX_SLOTS) {
		fArgQD = SBUFS_MAX_SLOTS;
	}
	if (fArgCached) {
		fArgQD = 1;     /* cached descriptors: two slots share a cache line, so only one slot (as herolte_ufs) */
	}

	OSData *allow = OSDynamicCast(OSData, provider->getProperty("starbite,allowed-ranges"));
	OSData *deny = OSDynamicCast(OSData, provider->getProperty("starbite,denied-ranges"));
	OSData *wa = OSDynamicCast(OSData, provider->getProperty("starbite,writes-allowed"));
	if (allow == NULL) {
		LOG("no starbite,allowed-ranges on the nub: nothing may be exposed, not starting");
		return false;
	}
	if (sbufs_fence_parse(&fFence, allow->getBytesNoCopy(), allow->getLength(),
	    deny ? deny->getBytesNoCopy() : NULL, deny ? deny->getLength() : 0, err, sizeof(err)) != SBUFS_OK) {
		LOG("fence properties refused (%s): not starting", err);
		return false;
	}
	fAdtWrites = wa != NULL && wa->getLength() >= 4 && *(const uint32_t *)wa->getBytesNoCopy() == 1;
	fFence.writes_enabled = fAdtWrites && fArgRW == 1;
	LOG("fence: %u allowed ranges, %u denied extents%s; writes %s (ADT %u, sbufsrw=%u)", fFence.nallow,
	    fFence.ndeny, deny ? "" : " (no deny-list: allow-list + built-in GPT floors)",
	    fFence.writes_enabled ? "ON" : "off", fAdtWrites, fArgRW);
	return fFence.nallow != 0;
}

bool
StarbiteUFS::mapRegisters(IOService *provider)
{
	if (provider->getDeviceMemoryCount() < 2) {
		LOG("nub has %u reg ranges, need HCI and vendor HCI", provider->getDeviceMemoryCount());
		return false;
	}
	fHciMap = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
	fVsMap = provider->mapDeviceMemoryWithIndex(1, kIOMapInhibitCache);
	if (fHciMap == NULL || fVsMap == NULL || fHciMap->getLength() < 0x200 || fVsMap->getLength() < 0x70) {
		LOG("cannot map the HCI / vendor HCI registers");
		return false;
	}
	fHci = (volatile uint32_t *)fHciMap->getVirtualAddress();
	fVs = (volatile uint32_t *)fVsMap->getVirtualAddress();
	if (provider->getDeviceMemoryCount() >= 4) {
		fUfspMap = provider->mapDeviceMemoryWithIndex(SBUFS_BANK_UFSP, kIOMapInhibitCache);  /* read only */
		if (fUfspMap != NULL && fUfspMap->getLength() >= 0x100) {
			fUfsp = (volatile uint32_t *)fUfspMap->getVirtualAddress();
		}
	}
	LOG("HCI at 0x%llx, vendor HCI at 0x%llx, UFSP %s", (unsigned long long)fHciMap->getPhysicalAddress(),
	    (unsigned long long)fVsMap->getPhysicalAddress(), fUfsp ? "mapped (read only)" : "not mapped");
	return true;
}

bool
StarbiteUFS::allocDMA(void)
{
	IOByteCount seglen = 0;
	size_t size = (SBUFS_DMA_SIZE(fArgQD) + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
	IOOptionBits opts = kIODirectionInOut | kIOMemoryPhysicallyContiguous |
	    (fArgCached ? 0 : kIOMapWriteCombineCache);
	mach_vm_address_t mask = fArg64 ? 0x0000000FFFFFF000ULL : 0x00000000FFFFF000ULL;

	fDma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, opts, size, mask);
	if (fDma == NULL || fDma->prepare() != kIOReturnSuccess) {
		LOG("no %zu bytes of contiguous DMA memory", size);
		OSSafeReleaseNULL(fDma);
		return false;
	}
	addr64_t pa = fDma->getPhysicalSegment(0, &seglen, kIOMemoryMapperNone);
	if (pa == 0 || seglen < size) {
		LOG("DMA memory not contiguous (0x%llx, %llu of %zu bytes)", (unsigned long long)pa,
		    (unsigned long long)seglen, size);
		return false;
	}
	fDmaCached = fArgCached != 0;
	bzero(fDma->getBytesNoCopy(), size);
	if (fDmaCached) {
		fDma->performOperation(kIOMemoryIncoherentIOFlush, 0, size);
	}
	struct sbufs_hw_ops ops = {
		.ctx = this, .rd = hwRead, .wr = hwWrite, .barrier = hwBarrier, .now_us = hwNowUs,
		.delay_us = hwDelayUs, .log = hwLog,
		.dma_clean = fDmaCached ? hwClean : NULL, .dma_inval = fDmaCached ? hwInval : NULL,
	};
	struct sbufs_cfg cfg = {
		.nslots = fArgQD, .clr_one = true, .nexus = true, .dma64 = fArg64 != 0, .irq = false,
		.timeout_us = 5 * 1000 * 1000,
	};
	if (sbufs_hc_init(&fHc, &ops, &cfg, (uint8_t *)fDma->getBytesNoCopy(), pa, size, &fFence) != SBUFS_OK) {
		LOG("core refused the DMA area at 0x%llx", (unsigned long long)pa);
		return false;
	}
	LOG("DMA area 0x%llx, %zu bytes, %s", (unsigned long long)pa, size,
	    fDmaCached ? "cacheable (explicit maintenance)" : "write-combined");
	return true;
}

void
StarbiteUFS::logUFSP(void)
{
	if (fUfsp == NULL) {
		return;
	}
	/* S-Boot leaves region 0 open (0..0xffffffff, all LUs, ctrl 0xf1: phone, 2026-10-25); only read here */
	LOG("ufsp: rctrl 0x%x rsecurity 0x%x region0 begin 0x%x end 0x%x lun 0x%x ctrl 0x%x",
	    fUfsp[SBUFS_UFSP_RCTRL / 4], fUfsp[SBUFS_UFSP_RSECURITY / 4], fUfsp[SBUFS_UFSP_REGION(0) / 4],
	    fUfsp[SBUFS_UFSP_REGION(0) / 4 + 1], fUfsp[SBUFS_UFSP_REGION(0) / 4 + 2], fUfsp[SBUFS_UFSP_REGION(0) / 4 + 3]);
}

bool
StarbiteUFS::bringUp(void)
{
	bool probed[SBUFS_MAX_GENERAL_LU] = { false };
	bool inquired = false;

	if (sbufs_hc_take_over(&fHc) != SBUFS_OK) {
		return false;
	}
	logUFSP();
	(void)sbufs_hc_wake(&fHc);              /* as herolte_ufs: a failed wake shows up in the LU probes */
	/* only the LUs the allow-list names are spoken to at all */
	for (unsigned i = 0; i < fFence.nallow; i++) {
		uint32_t lun = fFence.allow[i].lun;
		uint64_t blocks = 0;
		uint32_t bs = 0;
		if (probed[lun]) {
			continue;
		}
		probed[lun] = true;
		if (sbufs_hc_probe_lu(&fHc, (uint8_t)lun, &blocks, &bs) != SBUFS_OK) {
			LOG("lun %u: not ready: its ranges are not exposed", lun);
			continue;
		}
		if (bs != SBUFS_BLOCK) {
			LOG("lun %u: %u-byte blocks, not 4096: its ranges are not exposed", lun, bs);
			continue;
		}
		sbufs_fence_set_capacity(&fFence, lun, blocks);
		if (!inquired && sbufs_hc_inquiry(&fHc, (uint8_t)lun, fVendor, fProduct, fRevision) == SBUFS_OK) {
			inquired = true;
			LOG("device: %s %s rev %s", fVendor, fProduct, fRevision);
		}
	}
	if (!inquired) {
		strlcpy(fVendor, "SAMSUNG", sizeof(fVendor));
		strlcpy(fProduct, "UFS", sizeof(fProduct));
		strlcpy(fRevision, "?", sizeof(fRevision));
	}
	unsigned usable = sbufs_fence_seal(&fFence);
	logRanges();
	if (usable == 0) {
		LOG("no usable range: nothing exposed");
		return false;
	}
	if (fArgMulti) {
		for (unsigned i = 0; i < fFence.nallow; i++) {
			const struct sbufs_range *r = &fFence.allow[i];
			if (r->usable && r->count >= SBUFS_MULTI_BLOCKS) {
				(void)sbufs_hc_multi_selftest(&fHc, (uint8_t)r->lun, r->first);
				break;
			}
		}
	}
	return true;
}

static void
uuidString(const uint8_t u[16], char out[37])
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned o = 0;
	for (unsigned i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10) {
			out[o++] = '-';
		}
		out[o++] = hex[u[i] >> 4];
		out[o++] = hex[u[i] & 15];
	}
	out[o] = '\0';
}

void
StarbiteUFS::logRanges(void)
{
	for (unsigned i = 0; i < fFence.nallow; i++) {
		const struct sbufs_range *r = &fFence.allow[i];
		char u[37];
		uuidString(r->uuid, u);
		LOG("range %u \"%s\" lun %u lba %llu +%llu (%llu MiB) %s%s uuid %s: %s", i, r->name, r->lun,
		    (unsigned long long)r->first, (unsigned long long)r->count, (unsigned long long)(r->count >> 8),
		    sbufs_fence_range_writable(&fFence, i) ? "rw" : "ro", (r->flags & SBUFS_RANGE_ROOT) ? " root" : "", u,
		    r->usable ? "EXPOSED" : r->why);
	}
}

bool
StarbiteUFS::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	fNub = provider;
	fStartAbs = mach_absolute_time();
	for (unsigned i = 0; i < kPool; i++) {
		fPool[i].pooled = true;
		fPool[i].dma = IODMACommand::withSpecification(IODMACommand::OutputHost64, 64, SBUFS_PRD_MAX_BYTES,
		    IODMACommand::kMapped, 0, 1);
		if (fPool[i].dma == NULL) {
			goto fail;
		}
	}
	if (!readConfig(provider) || !mapRegisters(provider) || !allocDMA() || !bringUp()) {
		goto fail;
	}

	fWorkLoop = IOWorkLoop::workLoop();
	fGate = IOCommandGate::commandGate(this);
	fPollTimer = IOTimerEventSource::timerEventSource(this,
	    OSMemberFunctionCast(IOTimerEventSource::Action, this, &StarbiteUFS::handlePollTimer));
	fTreeTimer = IOTimerEventSource::timerEventSource(this,
	    OSMemberFunctionCast(IOTimerEventSource::Action, this, &StarbiteUFS::handleTreeTimer));
	fReportTimer = IOTimerEventSource::timerEventSource(this,
	    OSMemberFunctionCast(IOTimerEventSource::Action, this, &StarbiteUFS::handleReportTimer));
	if (fWorkLoop == NULL || fGate == NULL || fPollTimer == NULL || fTreeTimer == NULL || fReportTimer == NULL ||
	    fWorkLoop->addEventSource(fGate) != kIOReturnSuccess ||
	    fWorkLoop->addEventSource(fPollTimer) != kIOReturnSuccess ||
	    fWorkLoop->addEventSource(fTreeTimer) != kIOReturnSuccess ||
	    fWorkLoop->addEventSource(fReportTimer) != kIOReturnSuccess) {
		LOG("no work loop / event sources");
		goto fail;
	}
	if (fArgIrq) {
		fIrq = IOInterruptEventSource::interruptEventSource(this,
		    OSMemberFunctionCast(IOInterruptEventAction, this, &StarbiteUFS::handleInterrupt), provider, 0);
		if (fIrq != NULL && fWorkLoop->addEventSource(fIrq) == kIOReturnSuccess) {
			sbufs_hc_enable_irq(&fHc, true);
			fIrq->enable();
			LOG("interrupt: the nub's interrupt 0, %u ms safety poll", kPollMsIrq);
		} else {
			OSSafeReleaseNULL(fIrq);
			LOG("interrupt: none from the nub, polled every %u us", kPollUsPolled);
		}
	} else {
		LOG("interrupt: off (sbufsirq=0), polled every %u us", kPollUsPolled);
	}

	fMediaNotifier = addMatchingNotification(gIOPublishNotification, serviceMatching("IOMedia"),
	    OSMemberFunctionCast(IOServiceMatchingNotificationHandler, this, &StarbiteUFS::mediaPublished), this);
	fReportTimer->setTimeoutMS(1000);    /* the root watch, and the sbufsreport= report */
	publishRanges();
	registerService();
	LOG("up: %u slot(s), %u block(s) per command, %s DMA, %s", fHc.cfg.nslots, maxBlocksPerCommand(),
	    fArgBounce ? "bounce" : "direct", fIrq ? "interrupts" : "polled");
	return true;

fail:
	LOG("not started: Apple's software sees no UFS in this boot");
	stop(provider);
	return false;
}

void
StarbiteUFS::publishRanges(void)
{
	for (unsigned i = 0; i < fFence.nallow; i++) {
		if (!fFence.allow[i].usable) {
			continue;
		}
		StarbiteUFSRange *r = OSTypeAlloc(StarbiteUFSRange);
		if (r == NULL || !r->initRange(this, i) || !r->attach(this)) {
			OSSafeReleaseNULL(r);
			LOG("range %u: could not publish", i);
			continue;
		}
		fRanges[i] = r;
		r->registerService();
	}
}

void
StarbiteUFS::stop(IOService *provider)
{
	if (fMediaNotifier != NULL) {
		fMediaNotifier->remove();
		fMediaNotifier = NULL;
	}
	if (fIrq != NULL) {
		fIrq->disable();
		sbufs_hc_enable_irq(&fHc, false);
	}
	if (fWorkLoop != NULL) {
		if (fIrq != NULL) {
			fWorkLoop->removeEventSource(fIrq);
		}
		IOTimerEventSource *t[3] = { fPollTimer, fTreeTimer, fReportTimer };
		for (unsigned i = 0; i < 3; i++) {
			if (t[i] != NULL) {
				t[i]->cancelTimeout();
				fWorkLoop->removeEventSource(t[i]);
			}
		}
		if (fGate != NULL) {
			fWorkLoop->removeEventSource(fGate);
		}
	}
	for (unsigned i = 0; i < SBUFS_MAX_RANGES; i++) {
		OSSafeReleaseNULL(fRanges[i]);
	}
	IOService::stop(provider);
}

void
StarbiteUFS::free(void)
{
	OSSafeReleaseNULL(fIrq);
	OSSafeReleaseNULL(fPollTimer);
	OSSafeReleaseNULL(fTreeTimer);
	OSSafeReleaseNULL(fReportTimer);
	OSSafeReleaseNULL(fGate);
	OSSafeReleaseNULL(fWorkLoop);
	for (unsigned i = 0; i < kPool; i++) {
		OSSafeReleaseNULL(fPool[i].dma);
	}
	if (fDma != NULL) {
		fDma->complete();
		OSSafeReleaseNULL(fDma);
	}
	OSSafeReleaseNULL(fHciMap);
	OSSafeReleaseNULL(fVsMap);
	OSSafeReleaseNULL(fUfspMap);
	IOService::free();
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* requests                                                                                                          */

const struct sbufs_range *
StarbiteUFS::rangeInfo(unsigned range) const
{
	return range < fFence.nallow ? &fFence.allow[range] : NULL;
}

bool
StarbiteUFS::rangeWritable(unsigned range) const
{
	return sbufs_fence_range_writable(&fFence, range);
}

uint32_t
StarbiteUFS::maxBlocksPerCommand() const
{
	return sbufs_hc_max_xfer(&fHc) / SBUFS_BLOCK;
}

StarbiteUFSXfer *
StarbiteUFS::getXfer(void)
{
	for (unsigned i = 0; i < kPool; i++) {
		if (!fPool[i].used) {
			IODMACommand *d = fPool[i].dma;
			bzero(&fPool[i], sizeof(fPool[i]));
			fPool[i].dma = d;
			fPool[i].pooled = true;
			fPool[i].used = true;
			return &fPool[i];
		}
	}
	/* never sleep for one: a completion running on the work loop may submit the next request */
	StarbiteUFSXfer *x = IONewZero(StarbiteUFSXfer, 1);
	if (x != NULL) {
		x->dma = IODMACommand::withSpecification(IODMACommand::OutputHost64, 64, SBUFS_PRD_MAX_BYTES,
		    IODMACommand::kMapped, 0, 1);
		if (x->dma == NULL) {
			IODelete(x, StarbiteUFSXfer, 1);
			return NULL;
		}
		x->used = true;
	}
	return x;
}

void
StarbiteUFS::putXfer(StarbiteUFSXfer *x)
{
	if (x->pooled) {
		x->used = false;
		x->buffer = NULL;
	} else {
		OSSafeReleaseNULL(x->dma);
		IODelete(x, StarbiteUFSXfer, 1);
	}
}

void
StarbiteUFS::fenceRefusal(int verdict, unsigned range, uint64_t block, uint64_t nblks, bool write)
{
	/* the fence log: the first 32 in full, then every 1000th */
	if (fRefusalsLogged < 32 || fFence.refused % 1000 == 0) {
		fRefusalsLogged++;
		LOG("fence: REFUSED %s range %u block %llu +%llu: %s (refusal #%llu)", write ? "write" : "read", range,
		    (unsigned long long)block, (unsigned long long)nblks, sbufs_fence_verdict_name(verdict),
		    (unsigned long long)fFence.refused);
	}
}

struct SubmitArgs {
	int phase;
	unsigned range;
	IOMemoryDescriptor *buffer;
	UInt64 block, nblks;
	IOStorageAttributes *attributes;
	IOStorageCompletion *completion;
	StarbiteUFSXfer *x;
};

IOReturn
StarbiteUFS::gatedSubmit(OSObject *owner, void *a0, void *, void *, void *)
{
	StarbiteUFS *self = (StarbiteUFS *)owner;
	SubmitArgs *a = (SubmitArgs *)a0;
	uint32_t lun = 0;
	uint64_t lba = 0;

	switch (a->phase) {
	case 0: {
		bool write = a->buffer->getDirection() == kIODirectionOut;
		int v = sbufs_fence_map(&self->fFence, a->range, a->block, a->nblks, write, &lun, &lba);
		if (v != SBUFS_FENCE_OK) {
			self->fenceRefusal(v, a->range, a->block, a->nblks, write);
			return v == SBUFS_FENCE_READONLY ? kIOReturnNotWritable : kIOReturnNotPermitted;
		}
		if (self->fHc.dead) {
			return kIOReturnNoDevice;
		}
		a->x = self->getXfer();
		if (a->x == NULL) {
			return kIOReturnNoMemory;
		}
		a->x->range = a->range;
		a->x->lun = lun;
		a->x->lba = lba;
		a->x->nblks = a->nblks;
		a->x->write = write;
		a->x->fua = a->attributes != NULL && (a->attributes->options & kIOStorageOptionForceUnitAccess);
		a->x->buffer = a->buffer;
		a->x->completion = *a->completion;
		self->fActive++;
		self->fReqs++;
		return kIOReturnSuccess;
	}
	case 1:
		a->x->next = NULL;
		if (self->fReadyTail != NULL) {
			self->fReadyTail->next = a->x;
		} else {
			self->fReadyHead = a->x;
		}
		self->fReadyTail = a->x;
		self->pump();
		return kIOReturnSuccess;
	default:
		self->fActive--;
		self->putXfer(a->x);
		return kIOReturnSuccess;
	}
}

IOReturn
StarbiteUFS::submit(unsigned range, IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
    IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
	SubmitArgs a = { 0, range, buffer, block, nblks, attributes, completion, NULL };
	IOReturn ret;

	if (fGate == NULL || buffer == NULL || completion == NULL) {
		return kIOReturnNotReady;
	}
	if (nblks > UINT64_MAX / SBUFS_BLOCK || buffer->getLength() < nblks * SBUFS_BLOCK) {
		return kIOReturnBadArgument;
	}
	ret = fGate->runAction(gatedSubmit, &a);
	if (ret != kIOReturnSuccess) {
		return ret;
	}
	/* wire the buffer outside the gate: prepare() may page */
	ret = a.x->dma->setMemoryDescriptor(buffer, true);
	if (ret != kIOReturnSuccess) {
		LOG("buffer prepare failed 0x%x", ret);
		a.phase = 2;
		fGate->runAction(gatedSubmit, &a);
		return ret;
	}
	a.phase = 1;
	return fGate->runAction(gatedSubmit, &a);
}

void
StarbiteUFS::pump(void)
{
	while (fReadyHead != NULL) {
		StarbiteUFSXfer *x = fReadyHead;
		fReadyHead = x->next;
		if (fReadyHead == NULL) {
			fReadyTail = NULL;
		}
		if (!startChunk(x)) {
			x->next = fReadyHead;           /* no free slot: back to the head, wait for a completion */
			fReadyHead = x;
			if (fReadyTail == NULL) {
				fReadyTail = x;
			}
			break;
		}
	}
	armPoll();
}

static IOReturn
errReturn(int err)
{
	switch (err) {
	case SBUFS_OK: return kIOReturnSuccess;
	case SBUFS_E_TIMEOUT: return kIOReturnTimeout;
	case SBUFS_E_NODEV: return kIOReturnNoDevice;
	case SBUFS_E_FENCE: case SBUFS_E_GATE: return kIOReturnNotPermitted;
	case SBUFS_E_ROFS: return kIOReturnNotWritable;
	default: return kIOReturnIOError;
	}
}

/* Returns false only when no slot is free (the xfer stays queued); every other outcome consumes it. */
bool
StarbiteUFS::startChunk(StarbiteUFSXfer *x)
{
	struct sbufs_cmd c;
	struct sbufs_seg segs[SBUFS_MULTI_BLOCKS];
	unsigned nsegs = 0;
	int s = sbufs_hc_alloc_slot(&fHc);

	if (s == SBUFS_E_BUSY) {
		return false;
	}
	if (s < 0) {
		finish(x, kIOReturnNoDevice);
		return true;
	}
	if (x->sync) {
		(void)sbufs_cdb_sync(&c, (uint8_t)x->lun, x->lba, (uint32_t)x->nblks);
		x->chunk = (uint32_t)x->nblks;
		fSlotXfer[s] = x;
		int e = sbufs_hc_issue(&fHc, (unsigned)s, &c, NULL, 0, x);
		if (e != SBUFS_OK) {
			finish(x, errReturn(e));
		}
		return true;
	}

	uint64_t left = x->nblks - x->done;
	uint32_t n = (uint32_t)(left < maxBlocksPerCommand() ? left : maxBlocksPerCommand());
	uint64_t off = x->done * SBUFS_BLOCK;
	uint32_t bytes = n * SBUFS_BLOCK;
	if (x->write) {
		(void)sbufs_cdb_write(&c, (uint8_t)x->lun, x->lba + x->done, n, x->fua);
	} else {
		(void)sbufs_cdb_read(&c, (uint8_t)x->lun, x->lba + x->done, n);
	}

	/* direct DMA: the buffer's physical segments, if each is cache-line aligned and they cover the chunk */
	bool direct = fArgBounce == 0;
	if (direct) {
		IODMACommand::Segment64 seg[SBUFS_MULTI_BLOCKS];
		UInt32 num = n;
		UInt64 o = off;
		uint32_t covered = 0;
		if (x->dma->genIOVMSegments(&o, seg, &num) != kIOReturnSuccess) {
			num = 0;
		}
		for (UInt32 i = 0; i < num && covered < bytes; i++) {
			uint32_t len = (uint32_t)seg[i].fLength;
			if (len > bytes - covered) {
				len = bytes - covered;
			}
			if ((seg[i].fIOVMAddr & 63) != 0 || (len & 63) != 0) {
				break;
			}
			segs[nsegs].pa = seg[i].fIOVMAddr;
			segs[nsegs].len = len;
			nsegs++;
			covered += len;
		}
		direct = covered == bytes;
	}
	if (direct) {
		x->buffer->performOperation(x->write ? kIOMemoryIncoherentIOStore : kIOMemoryIncoherentIOFlush, off, bytes);
		int e = sbufs_hc_issue(&fHc, (unsigned)s, &c, segs, nsegs, x);
		if (e == SBUFS_OK) {
			x->chunk = n;
			x->bounce = false;
			fSlotXfer[s] = x;
			return true;
		}
		if (e != SBUFS_E_DMA) {
			finish(x, errReturn(e));
			return true;
		}
		s = sbufs_hc_alloc_slot(&fHc);  /* issue freed it: the same slot comes back */
		if (s < 0) {
			finish(x, kIOReturnNoDevice);
			return true;
		}
	}
	fBounced++;
	if (x->write && x->buffer->readBytes(off, sbufs_hc_bounce_va(&fHc, (unsigned)s), bytes) != bytes) {
		sbufs_hc_free_slot(&fHc, (unsigned)s);
		finish(x, kIOReturnIOError);
		return true;
	}
	int e = sbufs_hc_issue(&fHc, (unsigned)s, &c, NULL, 0, x);
	if (e != SBUFS_OK) {
		finish(x, errReturn(e));
		return true;
	}
	x->chunk = n;
	x->bounce = true;
	fSlotXfer[s] = x;
	return true;
}

void
StarbiteUFS::doneThunk(void *ctx, unsigned slot, void *cookie, const struct sbufs_result *r)
{
	StarbiteUFS *self = (StarbiteUFS *)ctx;
	self->fSlotXfer[slot] = NULL;
	self->chunkDone((StarbiteUFSXfer *)cookie, slot, r);
}

static void
pushHead(StarbiteUFSXfer **head, StarbiteUFSXfer **tail, StarbiteUFSXfer *x)
{
	x->next = *head;
	*head = x;
	if (*tail == NULL) {
		*tail = x;
	}
}

void
StarbiteUFS::chunkDone(StarbiteUFSXfer *x, unsigned slot, const struct sbufs_result *r)
{
	uint64_t off = x->done * SBUFS_BLOCK;
	uint32_t bytes = x->chunk * SBUFS_BLOCK;

	if (r->err == SBUFS_OK) {
		if (x->sync) {
			finish(x, kIOReturnSuccess);
			return;
		}
		if (!x->write) {
			if (x->bounce) {
				x->buffer->writeBytes(off, sbufs_hc_bounce_va(&fHc, slot), bytes);
			} else {
				x->buffer->performOperation(kIOMemoryIncoherentIOFlush, off, bytes);
			}
		}
		x->done += x->chunk;
		x->retries = 0;
		if (x->done == x->nblks) {
			finish(x, kIOReturnSuccess);
		} else {
			pushHead(&fReadyHead, &fReadyTail, x);
		}
		return;
	}
	bool attention = r->err == SBUFS_E_IO && (r->sense_key == SBUFS_SENSE_UNIT_ATTN ||
	    (r->sense_key == SBUFS_SENSE_NOT_READY && r->asc == 0x04));
	if (attention && x->retries < kMaxUARetries) {
		x->retries++;
		fUARetries++;
		pushHead(&fReadyHead, &fReadyTail, x);  /* the same chunk again */
		return;
	}
	LOG("%s lun %u lba %llu +%u failed (%d)", x->sync ? "sync" : x->write ? "write" : "read", x->lun,
	    (unsigned long long)(x->lba + x->done), x->chunk, r->err);
	finish(x, errReturn(r->err));
}

void
StarbiteUFS::finish(StarbiteUFSXfer *x, IOReturn status)
{
	if (x->sync) {
		x->status = status;
		x->finished = true;
		fGate->commandWakeup(x);
		return;
	}
	IOStorageCompletion completion = x->completion;
	UInt64 bytes = x->done * SBUFS_BLOCK;
	x->dma->clearMemoryDescriptor(true);
	putXfer(x);
	fActive--;
	IOStorage::complete(&completion, status, bytes);
}

void
StarbiteUFS::reap(bool fromIrq)
{
	(void)sbufs_hc_poll(&fHc, fromIrq, doneThunk, this);
}

void
StarbiteUFS::armPoll(void)
{
	if (fHc.inflight == 0 || fPollArmed || fPollTimer == NULL) {
		return;
	}
	fPollArmed = true;
	if (fIrq != NULL) {
		fPollTimer->setTimeoutMS(kPollMsIrq);
	} else {
		fPollTimer->setTimeoutUS(kPollUsPolled);
	}
}

void
StarbiteUFS::handleInterrupt(IOInterruptEventSource *src, int count)
{
	(void)src;
	(void)count;
	reap(true);
	pump();
}

void
StarbiteUFS::handlePollTimer(IOTimerEventSource *src)
{
	(void)src;
	fPollArmed = false;
	reap(false);
	pump();
}

struct SyncArgs {
	unsigned range;
	UInt64 block, nblks;
};

IOReturn
StarbiteUFS::gatedSync(OSObject *owner, void *a0, void *, void *, void *)
{
	StarbiteUFS *self = (StarbiteUFS *)owner;
	SyncArgs *a = (SyncArgs *)a0;
	return self->syncGated(a->range, a->block, a->nblks);
}

IOReturn
StarbiteUFS::syncGated(unsigned range, UInt64 block, UInt64 nblks)
{
	const struct sbufs_range *r = rangeInfo(range);
	uint32_t lun;
	uint64_t lba;

	if (r == NULL) {
		return kIOReturnBadArgument;
	}
	if (nblks == 0) {               /* "everything": the whole range, never the whole LU */
		block = 0;
		nblks = r->count;
	}
	if (nblks > UINT32_MAX) {
		return kIOReturnBadArgument;
	}
	int v = sbufs_fence_map(&fFence, range, block, nblks, true, &lun, &lba);
	if (v != SBUFS_FENCE_OK) {
		fenceRefusal(v, range, block, nblks, true);
		return kIOReturnNotPermitted;
	}
	StarbiteUFSXfer *x = getXfer();
	if (x == NULL) {
		return kIOReturnNoMemory;
	}
	x->sync = true;
	x->range = range;
	x->lun = lun;
	x->lba = lba;
	x->nblks = nblks;
	x->next = NULL;
	if (fReadyTail != NULL) {
		fReadyTail->next = x;
	} else {
		fReadyHead = x;
	}
	fReadyTail = x;
	pump();
	while (!x->finished) {
		fGate->commandSleep(x, THREAD_UNINT);
	}
	IOReturn ret = x->status;
	putXfer(x);
	return ret;
}

IOReturn
StarbiteUFS::synchronize(unsigned range, UInt64 block, UInt64 nblks)
{
	if (!rangeWritable(range)) {
		return kIOReturnSuccess;        /* nothing was ever written: nothing to flush */
	}
	if (fGate == NULL || fWorkLoop->onThread()) {
		return kIOReturnNotReady;       /* must not sleep on the work loop's own thread */
	}
	SyncArgs a = { range, block, nblks };
	return fGate->runAction(gatedSync, &a);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* the IOMedia tree, the root line, the fence log                                                                    */

bool
StarbiteUFS::underUs(IORegistryEntry *e) const
{
	for (unsigned depth = 0; e != NULL && depth < 32; depth++) {
		if (OSDynamicCast(StarbiteUFSRange, e) != NULL && e->getParentEntry(gIOServicePlane) == this) {
			return true;
		}
		e = e->getParentEntry(gIOServicePlane);
	}
	return false;
}

bool
StarbiteUFS::mediaPublished(void *ref, IOService *media, IONotifier *notifier)
{
	(void)ref;
	(void)notifier;
	if (underUs(media) && fTreeTimer != NULL) {
		fTreeTimer->setTimeoutMS(kTreeSettleMs);        /* re-armed by each new IOMedia: dump once they settle */
	}
	return true;
}

static const char *
str(IORegistryEntry *e, const char *key, char *buf, size_t len)
{
	OSObject *o = e->getProperty(key);
	OSString *s = OSDynamicCast(OSString, o);
	OSNumber *n = OSDynamicCast(OSNumber, o);
	OSBoolean *b = OSDynamicCast(OSBoolean, o);
	if (s != NULL) {
		return s->getCStringNoCopy();
	}
	if (n != NULL) {
		snprintf(buf, len, "%llu", n->unsigned64BitValue());
		return buf;
	}
	if (b != NULL) {
		return b->isTrue() ? "Yes" : "No";
	}
	return NULL;
}

void
StarbiteUFS::dumpEntry(IORegistryEntry *e, unsigned depth)
{
	char ind[2 * 12 + 1];
	char line[300];
	char b1[24], b2[24];
	unsigned d = depth < 12 ? depth : 12;
	memset(ind, ' ', 2 * d);
	ind[2 * d] = '\0';
	int o = snprintf(line, sizeof(line), "%s+-o %s  <class %s", ind, e->getName(), e->getMetaClass()->getClassName());
	if (OSDynamicCast(IOMedia, e) != NULL && o > 0 && (size_t)o < sizeof(line)) {
		const char *bsd = str(e, kIOBSDNameKey, b1, sizeof(b1));
		const char *uuid = str(e, kIOMediaUUIDKey, b2, sizeof(b2));
		IOMedia *m = (IOMedia *)e;
		snprintf(line + o, sizeof(line) - (size_t)o, ", Size %llu, Whole %s, Writable %s, Content \"%s\", UUID %s, BSD Name %s",
		    m->getSize(), m->isWhole() ? "Yes" : "No", m->isWritable() ? "Yes" : "No",
		    m->getContent() ? m->getContent() : "", uuid ? uuid : "-", bsd ? bsd : "-");
	} else if (OSDynamicCast(StarbiteUFSRange, e) != NULL && o > 0 && (size_t)o < sizeof(line)) {
		const struct sbufs_range *r = rangeInfo(((StarbiteUFSRange *)e)->index());
		snprintf(line + o, sizeof(line) - (size_t)o, ", range \"%s\" lun %u lba %llu +%llu", r ? r->name : "?",
		    r ? r->lun : 0, r ? (unsigned long long)r->first : 0ULL, r ? (unsigned long long)r->count : 0ULL);
	}
	IOLog("StarbiteUFS: tree| %s>\n", line);
	if (depth >= 12) {
		return;
	}
	OSIterator *it = e->getChildIterator(gIOServicePlane);
	if (it != NULL) {
		IORegistryEntry *c;
		while ((c = OSDynamicCast(IORegistryEntry, it->getNextObject())) != NULL) {
			dumpEntry(c, depth + 1);
		}
		it->release();
	}
}

void
StarbiteUFS::dumpTree(const char *why)
{
	LOG("IOMedia tree (%s):", why);
	dumpEntry(this, 0);
}

void
StarbiteUFS::fenceSummary(void)
{
	uint64_t foreign = fHc.foreign_touched;
	LOG("fence: %llu requests checked, %llu refused (%llu writes), %llu blocks allowed; doorbell audit refused %llu; "
	    "foreign LBA touched %llu", (unsigned long long)fFence.checked, (unsigned long long)fFence.refused,
	    (unsigned long long)fFence.refused_write, (unsigned long long)fFence.blocks_ok,
	    (unsigned long long)fHc.ngate, (unsigned long long)foreign);
	LOG("host: %llu commands, %llu errors, %llu timeouts, %llu fatal, %llu UIC errors, %llu UA retries, %llu bounced, "
	    "completions %llu irq / %llu poll, last IS 0x%x%s", (unsigned long long)fHc.ncmds, (unsigned long long)fHc.nerr,
	    (unsigned long long)fHc.ntimeout, (unsigned long long)fHc.nfatal, (unsigned long long)fHc.nue,
	    (unsigned long long)fUARetries, (unsigned long long)fBounced, (unsigned long long)fHc.ncompl_irq,
	    (unsigned long long)fHc.ncompl_poll, fHc.last_is, fHc.dead ? ", HOST GIVEN UP" : "");
}

bool
StarbiteUFS::reportRoot(void)
{
	char from[MAXPATHLEN];
	vnode_t vp = vfs_rootvnode();
	if (vp == NULLVP) {
		return false;
	}
	strlcpy(from, vfs_statfs(vnode_mount(vp))->f_mntfromname, sizeof(from));
	vnode_put(vp);

	const char *bsd = strncmp(from, "/dev/", 5) == 0 ? from + 5 : from;
	const OSSymbol *key = OSSymbol::withCString(kIOBSDNameKey);
	OSString *val = OSString::withCString(bsd);
	OSDictionary *match = IOService::serviceMatching("IOMedia");
	IOService *media = NULL;
	if (key != NULL && val != NULL && match != NULL) {
		match = IOService::propertyMatching(key, val, match);
		media = match ? IOService::copyMatchingService(match) : NULL;
	}
	OSSafeReleaseNULL(match);
	OSSafeReleaseNULL(val);
	OSSafeReleaseNULL(key);
	if (media == NULL || !underUs(media)) {
		LOG("root = %s, not on StarbiteUFS (no pass line)", from);
		OSSafeReleaseNULL(media);
		return true;
	}
	char ubuf[37];
	OSString *uuid = OSDynamicCast(OSString, media->getProperty(kIOMediaUUIDKey));
	const char *u = uuid ? uuid->getCStringNoCopy() : NULL;
	if (u == NULL) {
		/* a whole-range IOMedia has no GPT UUID: the range's own (from the allow-list) */
		for (IORegistryEntry *e = media; e != NULL; e = e->getParentEntry(gIOServicePlane)) {
			StarbiteUFSRange *r = OSDynamicCast(StarbiteUFSRange, e);
			if (r != NULL) {
				uuidString(rangeInfo(r->index())->uuid, ubuf);
				u = ubuf;
				break;
			}
		}
	}
	if (fHc.foreign_touched == 0) {
		LOG("root = IOMedia %s, fence log shows no foreign LBA touched", u ? u : "?");
	} else {
		LOG("root = IOMedia %s, fence log shows %llu FOREIGN commands: FAIL", u ? u : "?",
		    (unsigned long long)fHc.foreign_touched);
	}
	media->release();
	return true;
}

void
StarbiteUFS::handleTreeTimer(IOTimerEventSource *src)
{
	(void)src;
	bool first = !fTreeDumped;
	fTreeDumped = true;
	dumpTree("media settled");
	fenceSummary();
#if STARBITEUFS_SBSTOP
	if (first && !fStopped && starlte_sbstop_at("ufs")) {
		fStopped = true;
		starlte_sbstop_now("ufs");
	}
#else
	(void)first;
#endif
}

/* Every second: the root line as soon as the root is mounted (before a userland crash can bury it in the console),
 * and at sbufsreport= s the full report, then sbstop=ufsroot. Stops ticking after 600 s or when both are done. */
void
StarbiteUFS::handleReportTimer(IOTimerEventSource *src)
{
	(void)src;
	fReportTicks++;
	if (!fRootReported && reportRoot()) {
		fRootReported = true;
		fenceSummary();
	}
	if (fArgReport != 0 && !fReportDone && fReportTicks >= fArgReport) {
		fReportDone = true;
		LOG("report at sbufsreport=%u s", fArgReport);
		dumpTree("report");
		if (!reportRoot()) {
			LOG("root: not mounted by now");
		}
		fenceSummary();
#if STARBITEUFS_SBSTOP
		if (!fStopped && starlte_sbstop_at("ufsroot")) {
			fStopped = true;
			starlte_sbstop_now("ufsroot");
		}
#endif
	}
	if ((!fRootReported || (fArgReport != 0 && !fReportDone)) && fReportTicks < 600) {
		fReportTimer->setTimeoutMS(1000);
	}
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* StarbiteUFSRange                                                                                                  */

static void
setNum(IOService *s, const char *key, uint64_t v, unsigned bits)
{
	OSNumber *n = OSNumber::withNumber(v, bits);
	if (n != NULL) {
		s->setProperty(key, n);
		n->release();
	}
}

bool
StarbiteUFSRange::initRange(StarbiteUFS *controller, unsigned index)
{
	if (!IOBlockStorageDevice::init(NULL)) {
		return false;
	}
	fController = controller;
	fIndex = index;
	const struct sbufs_range *r = controller->rangeInfo(index);
	char u[37];
	uuidString(r->uuid, u);
	strlcpy(fVendor, controller->vendor(), sizeof(fVendor));
	strlcpy(fProduct, controller->product(), sizeof(fProduct));
	strlcpy(fRevision, controller->revision(), sizeof(fRevision));
	snprintf(fInfo, sizeof(fInfo), "Starbite range %s, LU %u", r->name, r->lun);

	setProperty(kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric);
	OSDictionary *dev = OSDictionary::withCapacity(3);
	OSDictionary *proto = OSDictionary::withCapacity(2);
	if (dev != NULL && proto != NULL) {
		OSString *v = OSString::withCString(fVendor), *p = OSString::withCString(fProduct),
		    *rv = OSString::withCString(fRevision);
		OSString *ic = OSString::withCString("UFS"), *loc = OSString::withCString(kIOPropertyInternalKey);
		if (v) { dev->setObject(kIOPropertyVendorNameKey, v); v->release(); }
		if (p) { dev->setObject(kIOPropertyProductNameKey, p); p->release(); }
		if (rv) { dev->setObject(kIOPropertyProductRevisionLevelKey, rv); rv->release(); }
		if (ic) { proto->setObject(kIOPropertyPhysicalInterconnectTypeKey, ic); ic->release(); }
		if (loc) { proto->setObject(kIOPropertyPhysicalInterconnectLocationKey, loc); loc->release(); }
		setProperty(kIOPropertyDeviceCharacteristicsKey, dev);
		setProperty(kIOPropertyProtocolCharacteristicsKey, proto);
	}
	OSSafeReleaseNULL(dev);
	OSSafeReleaseNULL(proto);

	/* what IOBlockStorageDriver splits and aligns requests by; anything else is bounced here */
	const uint64_t maxBlocks = 256;
	setNum(this, kIOMaximumBlockCountReadKey, maxBlocks, 64);
	setNum(this, kIOMaximumBlockCountWriteKey, maxBlocks, 64);
	setNum(this, kIOMaximumByteCountReadKey, maxBlocks * SBUFS_BLOCK, 64);
	setNum(this, kIOMaximumByteCountWriteKey, maxBlocks * SBUFS_BLOCK, 64);
	setNum(this, kIOMaximumSegmentCountReadKey, maxBlocks, 64);
	setNum(this, kIOMaximumSegmentCountWriteKey, maxBlocks, 64);
	setNum(this, kIOMaximumSegmentByteCountReadKey, SBUFS_PRD_MAX_BYTES, 64);
	setNum(this, kIOMaximumSegmentByteCountWriteKey, SBUFS_PRD_MAX_BYTES, 64);
	setNum(this, kIOMaximumSegmentAddressableBitCountKey, 32, 64);

	OSString *name = OSString::withCString(r->name), *uu = OSString::withCString(u);
	if (name) { setProperty("Starbite Range Name", name); name->release(); }
	if (uu) { setProperty("Starbite Range UUID", uu); uu->release(); }
	setNum(this, "Starbite Range LUN", r->lun, 32);
	setNum(this, "Starbite Range First LBA", r->first, 64);
	setNum(this, "Starbite Range Blocks", r->count, 64);
	setProperty("Starbite Range Root", (r->flags & SBUFS_RANGE_ROOT) != 0);
	setProperty("Starbite Range Writable", controller->rangeWritable(index));
	setLocation(r->name);
	return true;
}

IOReturn
StarbiteUFSRange::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
    IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
	return fController->submit(fIndex, buffer, block, nblks, attributes, completion);
}

IOReturn
StarbiteUFSRange::doSynchronize(UInt64 block, UInt64 nblks, IOStorageSynchronizeOptions options)
{
	(void)options;
	return fController->synchronize(fIndex, block, nblks);
}

IOReturn
StarbiteUFSRange::doUnmap(IOBlockStorageDeviceExtent *extents, UInt32 extentsCount, IOStorageUnmapOptions options)
{
	(void)extents;
	(void)extentsCount;
	(void)options;
	return kIOReturnUnsupported;    /* UNMAP is not on the whitelist */
}

IOReturn StarbiteUFSRange::doEjectMedia(void) { return kIOReturnUnsupported; }
IOReturn StarbiteUFSRange::doFormatMedia(UInt64 byteCapacity) { (void)byteCapacity; return kIOReturnUnsupported; }

UInt32
StarbiteUFSRange::doGetFormatCapacities(UInt64 *capacities, UInt32 capacitiesMaxCount) const
{
	if (capacities != NULL && capacitiesMaxCount > 0) {
		capacities[0] = fController->rangeInfo(fIndex)->count * SBUFS_BLOCK;
	}
	return 1;
}

char *StarbiteUFSRange::getVendorString(void) { return fVendor; }
char *StarbiteUFSRange::getProductString(void) { return fProduct; }
char *StarbiteUFSRange::getRevisionString(void) { return fRevision; }
char *StarbiteUFSRange::getAdditionalDeviceInfoString(void) { return fInfo; }

IOReturn StarbiteUFSRange::reportBlockSize(UInt64 *blockSize) { *blockSize = SBUFS_BLOCK; return kIOReturnSuccess; }
IOReturn StarbiteUFSRange::reportEjectability(bool *isEjectable) { *isEjectable = false; return kIOReturnSuccess; }

IOReturn
StarbiteUFSRange::reportMaxValidBlock(UInt64 *maxBlock)
{
	*maxBlock = fController->rangeInfo(fIndex)->count - 1;
	return kIOReturnSuccess;
}

IOReturn
StarbiteUFSRange::reportMediaState(bool *mediaPresent, bool *changedState)
{
	*mediaPresent = true;
	if (changedState != NULL) {
		*changedState = false;
	}
	return kIOReturnSuccess;
}

IOReturn StarbiteUFSRange::reportRemovability(bool *isRemovable) { *isRemovable = false; return kIOReturnSuccess; }

IOReturn
StarbiteUFSRange::reportWriteProtection(bool *isWriteProtected)
{
	*isWriteProtected = !fController->rangeWritable(fIndex);
	return kIOReturnSuccess;
}

#if TARGET_OS_OSX
IOReturn StarbiteUFSRange::doLockUnlockMedia(bool doLock) { (void)doLock; return kIOReturnUnsupported; }
IOReturn StarbiteUFSRange::doSynchronizeCache(void) { return doSynchronize(0, 0, 0); }
IOReturn StarbiteUFSRange::reportLockability(bool *isLockable) { *isLockable = false; return kIOReturnSuccess; }

IOReturn
StarbiteUFSRange::reportPollRequirements(bool *pollRequired, bool *pollIsExpensive)
{
	*pollRequired = false;
	*pollIsExpensive = false;
	return kIOReturnSuccess;
}
#endif
