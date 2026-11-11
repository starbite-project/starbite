/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteDisplay + AppleCLCD (see StarbiteDisplay.h and NOTES.md).
 *
 * The Apple-facing half (the IOMobileFramebuffer user client, its selectors, the swap record, the vsync
 * notification, the copy / scale / pointer-blend code) is SweetFramebuffer.cpp's, patches 0046..0398 of
 * xnu/series-12377: our own code, moved here and reshaped so that a frame can be drawn into any buffer and
 * then shown by an address flip instead of being copied into the buffer the DECON is reading.
 *
 * Built into the kernel (milestones M1-M8) or into a kext; INTEGRATION.md has the file list.
 */
#include "StarbiteDisplay.h"

#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/system.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/proc.h>

/* The IOSurface service (driver 19, StarbiteSurface; today SweetSurface.cpp in the kernel). */
#include "SweetSurface.h"

/*
 * STARBITE_DISPLAY_LEGACY (default on): link against, and stand in for, today's in-kernel pieces:
 *   uses     sweet_console_muted (serial_console.c), sweet_last_present_abs (SweetHID.cpp),
 *            starlte_gpu_present_* (starlte_gpu.c, sweetfb_gpu=1 in copy mode), starlte_sbstop_* (0423)
 *   defines  sweet_fb_first_present (0421's dead-man reads it), sweet_fb_cur_x/y/seq (SweetHID.cpp),
 *            sweet_fb_hold_until and sweet_fb_last_surface (herolte_gpu.c)
 * so that SweetFramebuffer.cpp can leave the build with nothing else changed. Set to 0 once those are gone.
 */
#ifndef STARBITE_DISPLAY_LEGACY
#define STARBITE_DISPLAY_LEGACY 1
#endif

#if STARBITE_DISPLAY_LEGACY
extern "C" {
extern int sweet_console_muted;
extern uint64_t sweet_last_present_abs;
int  starlte_gpu_present_ready(void);
int  starlte_gpu_present_slot(uint32_t id, uint64_t first_pa);
int  starlte_gpu_present_map(uint32_t id, const uint64_t *pages, unsigned n);
bool starlte_gpu_present_draw(int s, uint32_t off, uint32_t stride, uint32_t w, uint32_t h);
int  starlte_sbstop_at(const char *stage);
void starlte_sbstop_now(const char *stage);

uint64_t sweet_fb_first_present;
uint64_t sweet_fb_hold_until;
volatile int32_t sweet_fb_cur_x, sweet_fb_cur_y;
volatile uint32_t sweet_fb_cur_seq;
bool sweet_fb_last_surface(uint64_t *pages, unsigned max_pages, unsigned *npages, uint32_t *offset, uint32_t *stride,
    uint32_t *width, uint32_t *height, uintptr_t *kva);
}
static uint32_t sbd_last_surface_id;
#define SBD_GPU_MAX_PAGES 3072          /* one 12 MiB GPU slot (starlte_gpu.c) */
#endif

#define SBD_SU_MAX      320             /* pointer save-under: at most 320 x 320 target pixels */
#define SBD_COLUMNS     4096

enum {
	kSBDGetLayerDefaultSurface  = 3,
	kSBDSwapBegin               = 4,
	kSBDSwapEnd                 = 5,
	kSBDSwapWait                = 6,
	kSBDGetID                   = 7,
	kSBDGetDisplaySize          = 8,
	kSBDRequestPowerChange      = 12,
	kSBDIsMainDisplay           = 18,
	kSBDGetDotPitch             = 28,
	kSBDNotifications           = 72,
};

static uint64_t
sbd_uptime_ms(void)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000ULL;
}

static const char *
sbd_mode_name(uint32_t mode)
{
	return mode == kStarbitePresentScaler ? "scale" : (mode == kStarbitePresentFlip ? "flip" : "copy");
}

/* ------------------------------------------------------------------------------------------------------------
 * StarbiteDisplay: the hardware
 * ------------------------------------------------------------------------------------------------------------ */

OSDefineMetaClassAndStructors(StarbiteDisplay, IOService);

uint32_t
StarbiteDisplay::ioRead(void *ctx, unsigned block, uint32_t off)
{
	StarbiteDisplay *me = (StarbiteDisplay *)ctx;

	if (block >= SBD_BLK_COUNT || me->fBase[block] == 0 || off + 4 > me->fSize[block]) {
		return 0;
	}
	return *(volatile uint32_t *)(me->fBase[block] + off);
}

void
StarbiteDisplay::ioWrite(void *ctx, unsigned block, uint32_t off, uint32_t val)
{
	StarbiteDisplay *me = (StarbiteDisplay *)ctx;

	if (block >= SBD_BLK_COUNT || me->fBase[block] == 0 || off + 4 > me->fSize[block]) {
		return;
	}
	*(volatile uint32_t *)(me->fBase[block] + off) = val;
	OSSynchronizeIO();
}

void
StarbiteDisplay::ioDelay(void *ctx __unused, uint32_t us)
{
	if (us >= 1000) {
		IOSleep(us / 1000);
	} else {
		IODelay(us);
	}
}

bool
StarbiteDisplay::start(IOService *provider)
{
	static const uint32_t minSize[SBD_BLK_COUNT] = { 0x2000, 0x7000, 0x7000, 0x100 };
	struct sbd_io io = { &StarbiteDisplay::ioRead, &StarbiteDisplay::ioWrite, &StarbiteDisplay::ioDelay, this };
	uint32_t irq = 1;
	uint64_t limit = 0;

	if (!IOService::start(provider)) {
		return false;
	}
	/* the decon nub: reg 0 DECON_F, reg 1 DPU_DMA (IDMA), reg 2 DPP. Addresses come from nowhere else. */
	for (unsigned i = 0; i < 3 && i < provider->getDeviceMemoryCount(); i++) {
		fMap[i] = provider->mapDeviceMemoryWithIndex(i);
		if (fMap[i] != NULL && fMap[i]->getLength() >= minSize[i]) {
			fBase[i] = (uintptr_t)fMap[i]->getVirtualAddress();
			fSize[i] = (uint32_t)fMap[i]->getLength();
		}
	}
	if (fBase[SBD_BLK_DECON] == 0) {
		IOLog("StarbiteDisplay: the decon node has no usable reg: not started\n");
		return false;
	}
	/* the dsim nub: reg 0 DSIM0 (reg 1, the D-PHY, is never touched) */
	OSDictionary *match = IOService::nameMatching("dsim");
	IOService *dsim = match != NULL ? IOService::waitForMatchingService(match, 2ULL * NSEC_PER_SEC) : NULL;
	OSSafeReleaseNULL(match);
	if (dsim != NULL && dsim->getDeviceMemoryCount() >= 1) {
		fMap[SBD_BLK_DSIM] = dsim->mapDeviceMemoryWithIndex(0);
		if (fMap[SBD_BLK_DSIM] != NULL && fMap[SBD_BLK_DSIM]->getLength() >= minSize[SBD_BLK_DSIM]) {
			fBase[SBD_BLK_DSIM] = (uintptr_t)fMap[SBD_BLK_DSIM]->getVirtualAddress();
			fSize[SBD_BLK_DSIM] = (uint32_t)fMap[SBD_BLK_DSIM]->getLength();
		}
	}
	OSSafeReleaseNULL(dsim);

	fHwLock = IOSimpleLockAlloc();
	fPanelLock = IOLockAlloc();
	fWorkLoop = IOWorkLoop::workLoop();
	if (fHwLock == NULL || fPanelLock == NULL || fWorkLoop == NULL) {
		return false;
	}
	sbd_decon_init(&fDecon, &io);
	sbd_panel_init(&fPanel, &io);
	if (PE_parse_boot_argn("sbdisp_limit", &limit, sizeof(limit)) && limit != 0) {
		fDecon.scan_limit = limit;
	}
	fPanelPolicy = 1;
	PE_parse_boot_argn("sbdisp_panel", &fPanelPolicy, sizeof(fPanelPolicy));
	fHaveDsim = fBase[SBD_BLK_DSIM] != 0 && sbd_dsim_ready(&fPanel.io);

	fTimer = IOTimerEventSource::timerEventSource(this, &StarbiteDisplay::timerFired);
	if (fTimer == NULL || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
		IOLog("StarbiteDisplay: no timer\n");
		return false;
	}
	/* interrupts 1 (frame start), 2 (frame done), 3 (extra); 0 is the FIFO's, video mode only */
	PE_parse_boot_argn("sbdisp_irq", &irq, sizeof(irq));
	if (irq != 0) {
		unsigned made = 0;

		for (unsigned i = 0; i < 3; i++) {
			fIrq[i] = IOFilterInterruptEventSource::filterInterruptEventSource(this, &StarbiteDisplay::irqAction,
			    &StarbiteDisplay::irqFilter, provider, (int)i + 1);
			if (fIrq[i] != NULL && fWorkLoop->addEventSource(fIrq[i]) == kIOReturnSuccess) {
				made++;
			} else {
				OSSafeReleaseNULL(fIrq[i]);
			}
		}
		if (made == 0) {
			IOLog("StarbiteDisplay: the decon node gives no interrupts: refreshes are counted by polling\n");
		}
	}
	registerService();
	IOLog("StarbiteDisplay: decon %s, idma %s, dpp %s, dsim %s (%s), interrupts %s, panel policy %u\n",
	    fBase[SBD_BLK_DECON] ? "mapped" : "-", fBase[SBD_BLK_IDMA] ? "mapped" : "MISSING (reg 1)",
	    fBase[SBD_BLK_DPP] ? "mapped" : "MISSING (reg 2)", fBase[SBD_BLK_DSIM] ? "mapped" : "MISSING",
	    fHaveDsim ? "HS command mode" : "not usable", (fIrq[0] || fIrq[1] || fIrq[2]) ? "registered" : "none",
	    fPanelPolicy);
	return true;
}

void
StarbiteDisplay::stop(IOService *provider)
{
	if (fTimer != NULL) {
		fTimer->cancelTimeout();
	}
	for (unsigned i = 0; i < 3; i++) {
		if (fIrq[i] != NULL) {
			fIrq[i]->disable();
		}
	}
	if (fIrqOn) {
		sbd_decon_irq_enable(&fDecon, false);
		fIrqOn = false;
	}
	IOService::stop(provider);
}

int
StarbiteDisplay::probe(uint64_t fbPhys, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	int err;

	if (!haveChannels()) {
		return SBD_ERR_NOT_PROBED;
	}
	err = sbd_decon_probe(&fDecon, fbPhys, width, height, rowBytes);
	if (err == SBD_OK) {
		IOLog("StarbiteDisplay: DECON as expected: window %u on channel block %u reads %#x, %ux%u, trigger %#x, "
		    "frame count %u\n", fDecon.win, fDecon.boot_type, fDecon.boot_base, width, height, fDecon.boot_trig,
		    ioRead(this, SBD_BLK_DECON, 0x2a0));
	} else {
		IOLog("StarbiteDisplay: DECON left alone: %s (control %#x, windows %#x, channels %#x)\n",
		    sbd_decon_strerror(err), ioRead(this, SBD_BLK_DECON, 0x0), ioRead(this, SBD_BLK_DECON, 0x214),
		    ioRead(this, SBD_BLK_DECON, 0x218));
	}
	return err;
}

/* attach and detach sleep, so they run only before the refresh client starts (no interrupt or timer yet) */
int
StarbiteDisplay::attachFlip()
{
	return fVsyncFn != NULL ? SBD_ERR_BUSY : sbd_decon_attach_flip(&fDecon);
}

int
StarbiteDisplay::attachScaler(uint64_t phys, uint32_t w, uint32_t h, uint32_t rowBytes, const struct sbd_rect *dst)
{
	return fVsyncFn != NULL ? SBD_ERR_BUSY : sbd_decon_attach_scaler(&fDecon, phys, w, h, rowBytes, dst);
}

void
StarbiteDisplay::detach()
{
	if (fVsyncFn == NULL) {
		sbd_decon_detach(&fDecon);
	}
}

int
StarbiteDisplay::bufferOK(uint64_t phys, uint64_t bytes, uint32_t rowBytes)
{
	return sbd_decon_buffer_ok(&fDecon, phys, bytes, rowBytes);
}

int
StarbiteDisplay::flip(uint64_t phys, uint32_t rowBytes)
{
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(fHwLock);
	int err = sbd_decon_flip(&fDecon, phys, rowBytes);

	IOSimpleLockUnlockEnableInterrupt(fHwLock, state);
	return err;
}

bool
StarbiteDisplay::latched()
{
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(fHwLock);
	bool taken = !fDecon.pending || sbd_decon_latched(&fDecon);

	IOSimpleLockUnlockEnableInterrupt(fHwLock, state);
	return taken;
}

void
StarbiteDisplay::counters(struct sbd_decon *copy)
{
	IOInterruptState state = IOSimpleLockLockDisableInterrupt(fHwLock);

	*copy = fDecon;
	IOSimpleLockUnlockEnableInterrupt(fHwLock, state);
}

void
StarbiteDisplay::setVsyncClient(StarbiteDisplayVsync fn, void *target)
{
	fVsyncTarget = target;
	fVsyncFn = fn;
	if (fn == NULL) {
		return;
	}
	if (fIrq[0] != NULL || fIrq[1] != NULL || fIrq[2] != NULL) {
		IOInterruptState state = IOSimpleLockLockDisableInterrupt(fHwLock);

		sbd_decon_irq_enable(&fDecon, true);
		IOSimpleLockUnlockEnableInterrupt(fHwLock, state);
		fIrqOn = true;
		for (unsigned i = 0; i < 3; i++) {
			if (fIrq[i] != NULL) {
				fIrq[i]->enable();
			}
		}
	}
	fTimer->setTimeoutUS(SBD_VSYNC_US);
}

/* Primary interrupt: read and clear the DECON's interrupt registers, count, stamp. Nothing else. */
bool
StarbiteDisplay::irqFilter(OSObject *owner, IOFilterInterruptEventSource *src __unused)
{
	StarbiteDisplay *me = (StarbiteDisplay *)owner;
	uint32_t ev;

	IOSimpleLockLock(me->fHwLock);
	ev = sbd_decon_irq(&me->fDecon);
	IOSimpleLockUnlock(me->fHwLock);
	if (ev == 0) {
		return false;           /* another of the three lines already handled it */
	}
	if (ev & SBD_EV_VSYNC) {
		me->fIrqStamp = mach_absolute_time();
		OSIncrementAtomic((volatile SInt32 *)&me->fIrqVsyncs);
	}
	OSBitOrAtomic(ev, (volatile UInt32 *)&me->fIrqEvents);
	return true;
}

/* On the work loop, after the filter: one refresh to the client for each frame start. */
void
StarbiteDisplay::irqAction(OSObject *owner, IOInterruptEventSource *src __unused, int count __unused)
{
	StarbiteDisplay *me = (StarbiteDisplay *)owner;
	uint32_t ev, n;

	do {
		ev = me->fIrqEvents;
	} while (!OSCompareAndSwap(ev, 0, (volatile UInt32 *)&me->fIrqEvents));
	do {
		n = me->fIrqVsyncs;
	} while (!OSCompareAndSwap(n, 0, (volatile UInt32 *)&me->fIrqVsyncs));
	if (ev & (SBD_EV_TIMEOUT | SBD_EV_CONFLICT)) {
		static uint32_t said;

		if (said < 8) {
			said++;
			IOLog("StarbiteDisplay: DECON extra interrupt:%s%s\n", (ev & SBD_EV_TIMEOUT) ? " timeout" : "",
			    (ev & SBD_EV_CONFLICT) ? " resource conflict" : "");
		}
	}
	if (n != 0) {
		me->deliver(me->fIrqStamp, ev, false);
	}
}

/*
 * The 60 Hz backstop. If the interrupt delivered a refresh since the last tick, nothing to do. Otherwise count
 * refreshes from FRAME_COUNT, and if the hardware had none at all (TE masked, panel asleep, no DECON), make one,
 * so that CoreAnimation's display link never stops.
 */
void
StarbiteDisplay::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteDisplay *me = (StarbiteDisplay *)owner;
	static uint64_t deliveredAtLastTick;

	if (me->fVsyncFn != NULL && me->fVsyncCount == deliveredAtLastTick) {
		uint32_t refreshes = 0, ev = 0;

		if (me->fDecon.probed > 0) {
			IOInterruptState state = IOSimpleLockLockDisableInterrupt(me->fHwLock);

			ev = sbd_decon_poll(&me->fDecon, &refreshes);
			IOSimpleLockUnlockEnableInterrupt(me->fHwLock, state);
		}
		me->deliver(mach_absolute_time(), ev, refreshes == 0);
	}
	deliveredAtLastTick = me->fVsyncCount;
	sender->setTimeoutUS(SBD_VSYNC_US);
}

void
StarbiteDisplay::deliver(uint64_t stamp, uint32_t events, bool synthetic)
{
	StarbiteDisplayVsync fn = fVsyncFn;

	fVsyncCount++;
	if (synthetic) {
		fSyntheticVsyncs++;
	}
	if (fn != NULL) {
		fn(fVsyncTarget, fVsyncCount, stamp, events);
	}
}

IOReturn
StarbiteDisplay::setBrightness(int level)
{
	int err;

	if (!panelUsable()) {
		return kIOReturnUnsupported;
	}
	IOLockLock(fPanelLock);
	err = sbd_panel_set_brightness(&fPanel, level);
	IOLockUnlock(fPanelLock);
	static uint32_t said;
	if (said < 16 || err != SBD_DSIM_OK) {
		said++;
		IOLog("StarbiteDisplay: brightness %d -> AOR %#06x: %s (packets %u, timeouts %u)\n", level,
		    sbd_panel_aor_for_level(level), err == SBD_DSIM_OK ? "sent" : "NOT sent", fPanel.stats.packets,
		    fPanel.stats.timeouts);
	}
	return err == SBD_DSIM_OK ? kIOReturnSuccess : kIOReturnTimeout;
}

IOReturn
StarbiteDisplay::panelPower(bool on)
{
	int err = SBD_DSIM_OK;

	if (!fHaveDsim || fPanelPolicy < 2) {
		return kIOReturnSuccess;        /* dark by a black frame alone (AMOLED: black pixels are off) */
	}
	IOLockLock(fPanelLock);
	if (fPanelPolicy >= 3) {
		err = sbd_panel_sleep(&fPanel, !on);
	} else {
		err = sbd_panel_display(&fPanel, on);
	}
	IOLockUnlock(fPanelLock);
	IOLog("StarbiteDisplay: panel %s (%s): %s\n", on ? "on" : "off", fPanelPolicy >= 3 ? "sleep" : "display",
	    err == SBD_DSIM_OK ? "sent" : "NOT sent");
	return err == SBD_DSIM_OK ? kIOReturnSuccess : kIOReturnTimeout;
}

/* ------------------------------------------------------------------------------------------------------------
 * AppleCLCD: the IOMobileFramebuffer service
 * ------------------------------------------------------------------------------------------------------------ */

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);
OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);
OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

static uint32_t
sbd_nub_u32(IOService *nub, const char *key)
{
	OSData *data = nub != NULL ? OSDynamicCast(OSData, nub->getProperty(key)) : NULL;
	uint32_t v = 0;

	if (data != NULL && data->getLength() >= sizeof(v)) {
		memcpy(&v, data->getBytesNoCopy(), sizeof(v));
	}
	return v;
}

bool
AppleCLCD::allocTarget(StarbiteTarget *t, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	vm_size_t bytes = round_page((vm_size_t)rowBytes * height);
	/* physically contiguous, below 4 GiB (the IDMA takes 32-bit addresses), write-combined like S-Boot's buffer:
	 * what is drawn is in RAM without a cache flush */
	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache, bytes, 0x00000000fffff000ULL);
	IOByteCount len = 0;

	if (md == NULL) {
		IOLog("StarbiteDisplay: no contiguous %lu KiB below 4 GiB for a scan-out buffer\n", (unsigned long)(bytes >> 10));
		return false;
	}
	addr64_t pa = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
	if (pa == 0 || len < bytes || fHw->bufferOK(pa, (uint64_t)rowBytes * height, rowBytes) != SBD_OK) {
		IOLog("StarbiteDisplay: scan-out buffer at %#llx (%llu of %lu bytes in one piece) not usable by the IDMA\n",
		    (unsigned long long)pa, (unsigned long long)len, (unsigned long)bytes);
		md->release();
		return false;
	}
	t->va = (uintptr_t)md->getBytesNoCopy();
	t->phys = pa;
	t->width = width;
	t->height = height;
	t->rowBytes = rowBytes;
	t->memory = md;
	bzero((void *)t->va, bytes);
	return true;
}

bool
AppleCLCD::setupFlip()
{
	if (!allocTarget(&fTargets[1], fWidth, fHeight, fRowBytes)) {
		return false;
	}
	int err = fHw->attachFlip();
	if (err != SBD_OK) {
		IOLog("StarbiteDisplay: flips not attached: %s\n", sbd_decon_strerror(err));
		OSSafeReleaseNULL(fTargets[1].memory);
		return false;
	}
	fMode = kStarbitePresentFlip;
	fFront = 0;                     /* S-Boot's framebuffer is on screen */
	fFrontOwn = true;
	return true;
}

bool
AppleCLCD::setupScaler()
{
	struct sbd_rect dst;
	uint32_t rowBytes = (fLogicalW * 4 + 63) & ~63u;
	StarbiteTarget a = {}, b = {};

	if (!allocTarget(&a, fLogicalW, fLogicalH, rowBytes) || !allocTarget(&b, fLogicalW, fLogicalH, rowBytes)) {
		OSSafeReleaseNULL(a.memory);
		OSSafeReleaseNULL(b.memory);
		return false;
	}
	sbd_decon_fit(fLogicalW, fLogicalH, fWidth, fHeight, &dst);
	int err = fHw->attachScaler(a.phys, fLogicalW, fLogicalH, rowBytes, &dst);
	if (err != SBD_OK) {
		IOLog("StarbiteDisplay: VGF0's scaler not attached: %s; S-Boot's window stays\n", sbd_decon_strerror(err));
		OSSafeReleaseNULL(a.memory);
		OSSafeReleaseNULL(b.memory);
		return false;
	}
	IOLog("StarbiteDisplay: VGF0 scales %ux%u to %ux%u at %u,%u\n", fLogicalW, fLogicalH, dst.w, dst.h, dst.x, dst.y);
	fTargets[0] = a;
	fTargets[1] = b;
	fMode = kStarbitePresentScaler;
	fFront = 0;
	fFrontOwn = true;
	return true;
}

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;
	char want[16] = "flip";
	uint32_t hz = 10, lw = 0, lh = 0, selftest = 0;
	int native = 0, gpu = 0, bl = -1;

	if (!IOService::start(provider)) {
		return false;
	}
	/* S-Boot's framebuffer, as the loader's boot_args describe it */
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("StarbiteDisplay: AppleCLCD: no boot framebuffer\n");
		return false;
	}
	/* The canvas userland draws: the ADT's (IOMobileFramebuffer.framework reads the same two properties from
	 * this service's provider), else sweetfb_lw / sweetfb_lh, else the panel. sweetfb_native=1: the panel. */
	fLogicalW = fWidth;
	fLogicalH = fHeight;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	if (!native) {
		lw = sbd_nub_u32(provider, "canvas-width");
		lh = sbd_nub_u32(provider, "canvas-height");
		if (lw == 0 || lh == 0) {
			PE_parse_boot_argn("sweetfb_lw", &lw, sizeof(lw));
			PE_parse_boot_argn("sweetfb_lh", &lh, sizeof(lh));
		}
		if (lw >= 320 && lw <= fWidth && lh >= 480 && lh <= fHeight) {
			fLogicalW = lw;
			fLogicalH = lh;
		}
	}
	fScaled = fLogicalW != fWidth || fLogicalH != fHeight;

	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fWorkLoop = IOWorkLoop::workLoop();
	fSuBuf = (uint32_t *)IOMallocData(SBD_SU_MAX * SBD_SU_MAX * sizeof(uint32_t));
	fPanelSurface = sweet_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, SBD_FORMAT_BGRA);
	if (fLock == NULL || fClients == NULL || fWorkLoop == NULL || fSuBuf == NULL || fPanelSurface == NULL ||
	    fPanelSurface->kernelAddress() == 0) {
		IOLog("StarbiteDisplay: AppleCLCD: setup failed\n");
		return false;
	}
	fTargets[0].va = fPanelSurface->kernelAddress();
	fTargets[0].phys = fPhys;
	fTargets[0].width = fWidth;
	fTargets[0].height = fHeight;
	fTargets[0].rowBytes = fRowBytes;
	fTargets[0].memory = NULL;
	fMode = kStarbitePresentCopy;
	fFront = 0;
	fFrontOwn = true;

	/* the hardware half, if the ADT has a decon node and the DECON is in the state we know */
	PE_parse_boot_argn("sbdisp", want, sizeof(want));
	OSDictionary *match = IOService::serviceMatching("StarbiteDisplay");
	IOService *hw = match != NULL ? IOService::waitForMatchingService(match, 3ULL * NSEC_PER_SEC) : NULL;
	OSSafeReleaseNULL(match);
	fHw = OSDynamicCast(StarbiteDisplay, hw);
	if (fHw == NULL) {
		OSSafeReleaseNULL(hw);
		IOLog("StarbiteDisplay: AppleCLCD: no StarbiteDisplay service (no decon node?): CPU copy, timer vsync\n");
	} else if (strcmp(want, "copy") != 0) {
		if (fHw->probe(fPhys, fWidth, fHeight, fRowBytes) == SBD_OK) {
			if (strcmp(want, "scale") == 0 && fScaled && !setupScaler()) {
				IOLog("StarbiteDisplay: sbdisp=scale not possible: trying flips\n");
			}
			if (fMode == kStarbitePresentCopy && !setupFlip()) {
				IOLog("StarbiteDisplay: flips not possible: CPU copy\n");
			}
		}
	}
	PE_parse_boot_argn("sweetfb_gpu", &gpu, sizeof(gpu));
	fGpu = gpu != 0 && fMode == kStarbitePresentCopy && fScaled;

	/* The default framebuffer surface has the canvas's size. While the DECON reads S-Boot's buffer directly
	 * (copy mode, canvas = panel) it is that buffer; otherwise a buffer of its own, presented like any other. */
	if (fMode == kStarbitePresentCopy && !fScaled) {
		fSurface = fPanelSurface;
		fSurface->retain();
	} else {
		fSurface = sweet_surface_create_buffer(fLogicalW, fLogicalH, SBD_FORMAT_BGRA);
	}
	if (fSurface == NULL) {
		IOLog("StarbiteDisplay: AppleCLCD: no default surface\n");
		return false;
	}

	PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
	fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;

	publishProperties();
	if (fHw != NULL) {
		if (PE_parse_boot_argn("sbdisp_bl", &bl, sizeof(bl)) && bl >= 0) {
			(void)fHw->setBrightness(bl);
		}
		fHw->setVsyncClient(&AppleCLCD::hwVsync, this);
	} else {
		fTimer = IOTimerEventSource::timerEventSource(this, &AppleCLCD::timerFired);
		if (fTimer == NULL || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
			IOLog("StarbiteDisplay: AppleCLCD: no vsync timer\n");
			return false;
		}
		fTimer->setTimeoutUS(SBD_VSYNC_US);
	}
	registerService();
	IOLog("StarbiteDisplay: AppleCLCD %ux%u stride %u at %#llx, canvas %ux%u, default surface %u, mode %s%s, "
	    "vsync by %s\n", fWidth, fHeight, fRowBytes, fPhys, fLogicalW, fLogicalH, fSurface->id, sbd_mode_name(fMode),
	    fGpu ? " (Mali scales)" : "", fHw == NULL ? "timer" : (fHw->interruptsOn() ? "DECON interrupt" : "FRAME_COUNT poll"));

	if (PE_parse_boot_argn("sbdisp_selftest", &selftest, sizeof(selftest)) && selftest != 0 &&
	    fMode != kStarbitePresentCopy) {
		fSelfTestFlips = selftest > 600 ? 600 : selftest;
		thread_call_t call = thread_call_allocate(&AppleCLCD::selfTestThread, this);
		if (call != NULL) {
			thread_call_enter(call);
		}
	}
	return true;
}

/*
 * What CoreAnimation reads from this service (QuartzCore's CA::WindowServer::AppleDisplay::update_display_limits,
 * macOS 27.2, and the shapes a real IOMobileFramebufferShim publishes: NOTES.md "Colour and capability
 * properties"). A key CoreAnimation does not find is taken as "supported" for several of them (multi-plane RGB,
 * compressed framebuffers), so what the DECON cannot scan out is said here.
 */
static void
sbd_set_num(OSDictionary *d, const char *key, uint64_t value)
{
	OSNumber *n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		d->setObject(key, n);
		n->release();
	}
}

void
AppleCLCD::publishProperties()
{
	uint32_t maxW = fWidth > 4096 ? fWidth : 4096, maxH = fHeight > 4096 ? fHeight : 4096;
	OSDictionary *d;

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayWidth", (unsigned long long)fLogicalW, 32);
	setProperty("DisplayHeight", (unsigned long long)fLogicalH, 32);

	/* buffer layout the pipe takes: any 4-byte row multiple, 64-byte base alignment (flips need 256) */
	if ((d = OSDictionary::withCapacity(4)) != NULL) {
		sbd_set_num(d, "PlaneBaseAlignmentLinear", 256);
		sbd_set_num(d, "LinearX_Alignment", 64);
		sbd_set_num(d, "LinearY_Alignment", 1);
		sbd_set_num(d, "DefaultStride", (fLogicalW * 4 + 63) & ~63u);
		setProperty("DisplayPipePlaneBaseAlignment", d);
		d->release();
	}
	if ((d = OSDictionary::withCapacity(2)) != NULL) {
		sbd_set_num(d, "StrideLinearHorizontal", 64);
		sbd_set_num(d, "StrideLinearVertical", 1);
		setProperty("DisplayPipeStrideRequirements", d);
		d->release();
	}
	/* source limits; the compression answers live in this dictionary */
	if ((d = OSDictionary::withCapacity(12)) != NULL) {
		sbd_set_num(d, "MaxSrcBufferWidth", maxW);
		sbd_set_num(d, "MaxSrcBufferHeight", maxH);
		sbd_set_num(d, "MaxSrcRectWidth", maxW);
		sbd_set_num(d, "MaxSrcRectTotal", (uint64_t)maxW * maxH);
		sbd_set_num(d, "MaxVideoSrcDownscalingWidth", maxW);
		sbd_set_num(d, "IOMFBMaxCompressedSizeInBytes", 0);
		sbd_set_num(d, "IOMFBCompressionSupport", 0);           /* no AGX-compressed framebuffers */
		sbd_set_num(d, "IOMFBAsyncRGBCompressionSupport", 0);
		setProperty("IOMFBMaxSrcPixels", d);
		d->release();
	}
	/* one window is driven today: every frame is one full layer (the pointer is blended by this driver) */
	if ((d = OSDictionary::withCapacity(2)) != NULL) {
		sbd_set_num(d, "MaxNumLayers", SBD_SWAP_LAYERS);
		sbd_set_num(d, "FullFrameRequired", 0);
		setProperty("IOMFBNumLayers", d);
		d->release();
	}
	if ((d = OSDictionary::withCapacity(4)) != NULL) {
		sbd_set_num(d, "RGBLayer_MaxScale", 1);
		sbd_set_num(d, "RGBLayer_MinScaleFraction", 1);
		sbd_set_num(d, "YUVLayer_MaxScale", 1);
		sbd_set_num(d, "YUVLayer_MinScaleFraction", 1);
		setProperty("IOMFBScalingLimits", d);
		d->release();
	}
	/* colour formats: 8-bit BGRA only. No multi-plane RGB ('b3a8'), no YUV layers, no ICC, no rotation. */
	setProperty("IOMFBRGBMultiPlaneSupportKey", false);
	setProperty("IOMFBYUVSupportKey", false);
	setProperty("IOMFBYUVPackedUnSupported", true);
	setProperty("IOMFBYUV420UnSupported", true);
	setProperty("IOMFBSupportsICC", false);
	setProperty("IOMFBSupportsRotation", false);
	setProperty("IOMFBSwapAppleLookSupported", false);
	setProperty("IOMFBRGBSupportsCursorOnSCLregionKey", false);
	setProperty("W40a_Blending_OK", 0ULL, 32);
	setProperty("IOMFBAsyncSwapDisabledKey", true);                 /* no vsync-off swaps */
	/* 60 Hz, fixed: intervals in 2^-32 s and in timebase ticks */
	if ((d = OSDictionary::withCapacity(6)) != NULL) {
		uint64_t ticks = 0;

		nanoseconds_to_absolutetime(1000000000ULL / 60, &ticks);
		sbd_set_num(d, "displayMinRefreshInterval", 0x100000000ULL / 60);
		sbd_set_num(d, "displayMaxRefreshInterval", 0x100000000ULL / 60);
		sbd_set_num(d, "displayMinRefreshIntervalMachTime", ticks);
		sbd_set_num(d, "displayMaxRefreshIntervalMachTime", ticks);
		sbd_set_num(d, "displayRefreshStep", 0);
		sbd_set_num(d, "displayRefreshStepMachTime", 0);
		setProperty("IOMFBDisplayRefresh", d);
		d->release();
	}
	/* DisplayAttributes: SkyLight's SLCADisplay::copy_ioav_display_attributes (optional; the real panel's facts) */
	if ((d = OSDictionary::withCapacity(2)) != NULL) {
		OSDictionary *product = OSDictionary::withCapacity(4), *tiled = OSDictionary::withCapacity(1);

		if (product != NULL) {
			OSString *name = OSString::withCString("S6E3HA8");
			OSString *maker = OSString::withCString("SDC");                 /* Samsung Display's PNP ID */

			sbd_set_num(product, "LegacyManufacturerID", 0x4c83);           /* 'SDC' packed as EDID does */
			sbd_set_num(product, "ProductID", 0x910444);                    /* the panel's DCS ID bytes 91 04 44 */
			if (maker != NULL) {
				product->setObject("ManufacturerID", maker);
				maker->release();
			}
			if (name != NULL) {
				product->setObject("ProductName", name);
				name->release();
			}
			d->setObject("ProductAttributes", product);
			product->release();
		}
		if (tiled != NULL) {
			d->setObject("TiledDisplayInfo", tiled);
			tiled->release();
		}
		setProperty("DisplayAttributes", d);
		d->release();
	}
	/* brightness, as IOGraphics names a display's parameters: 0..65536, set through setProperties() */
	if (fHw != NULL && fHw->panelUsable() && (d = OSDictionary::withCapacity(2)) != NULL) {
		static const char *const names[2] = { "brightness", "linear-brightness" };

		for (unsigned i = 0; i < 2; i++) {
			OSDictionary *range = OSDictionary::withCapacity(3);

			if (range != NULL) {
				sbd_set_num(range, "min", 0);
				sbd_set_num(range, "max", 65536);
				sbd_set_num(range, "value", 65536);
				d->setObject(names[i], range);
				range->release();
			}
		}
		setProperty("IODisplayParameters", d);
		d->release();
	}
}

/* "brightness" / "linear-brightness": a number, or a dictionary with "value"; 0..65536 */
IOReturn
AppleCLCD::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	static const char *const names[2] = { "brightness", "linear-brightness" };

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	for (unsigned i = 0; i < 2; i++) {
		OSObject *o = dict->getObject(names[i]);
		OSDictionary *inner = OSDynamicCast(OSDictionary, o);
		OSNumber *n = OSDynamicCast(OSNumber, inner != NULL ? inner->getObject("value") : o);

		if (n != NULL) {
			uint64_t v = n->unsigned64BitValue();

			if (fHw == NULL) {
				return kIOReturnUnsupported;
			}
			return fHw->setBrightness((int)((v > 65536 ? 65536 : v) * 255 / 65536));
		}
	}
	return kIOReturnUnsupported;
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

/*
 * A swap only records the frame. The refresh tick shows the latest one: at every refresh when it can go out by
 * address alone, every fFlushTicks ticks when it has to be drawn first (sweetfb_hz, default 10 Hz).
 */
void
AppleCLCD::swap(const uint8_t *rec, uint32_t size)
{
	static uint64_t count;
	static uint32_t dumped;
	struct sbd_swap s;

	IOLockLock(fLock);
	memcpy(fPending, rec, SBD_SWAP_REC_MAX);
	fDirty = true;
	IOLockUnlock(fLock);
	/* the first records, every non-zero word (a new OS release may move the fields again) */
	if (dumped < 4 && sbd_swap_parse(rec, size, &s) && s.enabled != 0) {
		char line[160];
		int n = 0;

		dumped++;
		IOLog("StarbiteDisplay: swap record %llu size %#x, fields at +%#x\n", count, size, s.shift);
		for (uint32_t off = 0; off + 4 <= size && off + 4 <= SBD_SWAP_REC_MAX; off += 4) {
			uint32_t v;

			memcpy(&v, rec + off, 4);
			if (v == 0) {
				continue;
			}
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %x=%x", off, v);
			if (n > (int)sizeof(line) - 24) {
				IOLog("StarbiteDisplay: rec%s\n", line);
				n = 0;
			}
		}
		if (n != 0) {
			IOLog("StarbiteDisplay: rec%s\n", line);
		}
	}
	count++;
}

void
AppleCLCD::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;

	fb->tick(++fb->fVsyncs, mach_absolute_time());
	sender->setTimeoutUS(SBD_VSYNC_US);
}

void
AppleCLCD::hwVsync(void *target, uint64_t count, uint64_t abstime, uint32_t events __unused)
{
	AppleCLCD *fb = (AppleCLCD *)target;

	fb->fVsyncs = count;
	fb->tick(count, abstime);
}

void
AppleCLCD::tick(uint64_t count, uint64_t abstime)
{
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)abstime, (uint32_t)(abstime >> 32),
		0, 0,
	};
	static bool blanked;
	uint8_t *rec = NULL;
	bool wake = false;

	IOLockLock(fLock);
	for (unsigned int i = 0; i < fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client = (IOMobileFramebufferUserClient *)fClients->getObject(i);

		client->notify(SBD_NOTIFY_VSYNC, values, 6);
	}
	bool off = fPowerOff;
	bool flush = !off && fDirty && fSelfTestFlips == 0;
	if (flush && (count % fFlushTicks) != 0) {
		/* off the copy schedule: only a frame that needs no drawing goes out now. The last full frame having
		 * gone out by address (the DECON reads a client's surface) is the sign that this one can too. */
		struct sbd_swap s;

		flush = fMode != kStarbitePresentCopy && !fFrontOwn && sbd_swap_parse(fPending, SBD_SWAP_REC_MAX, &s) &&
		    s.enabled == 1 && s.layer[0].enabled;
	}
#if STARBITE_DISPLAY_LEGACY
	if (flush && abstime < sweet_fb_hold_until) {
		flush = false;          /* herolte_gpu.c holds the panel for its test frames */
	}
#endif
	if (flush) {
		rec = (uint8_t *)IOMallocData(SBD_SWAP_REC_MAX);
		if (rec != NULL) {
			memcpy(rec, fPending, SBD_SWAP_REC_MAX);
			fDirty = false;
		}
	}
	IOLockUnlock(fLock);

	if (off && !blanked) {
		/* display off: a black frame (AMOLED black is off), then the panel by policy */
		const StarbiteTarget *t = &fTargets[fMode == kStarbitePresentCopy ? 0 : (fFrontOwn ? 1 - fFront : 0)];

		blanked = true;
		bzero((void *)t->va, (size_t)t->rowBytes * t->height);
		if (fMode != kStarbitePresentCopy && fHw->flip(t->phys, t->rowBytes) == SBD_OK) {
			fFront = (uint32_t)(t - fTargets);
			fFrontOwn = true;
		}
		fSuHave = false;
		if (fHw != NULL) {
			(void)fHw->panelPower(false);
		}
	} else if (!off && blanked) {
		blanked = false;
		wake = true;
		if (fHw != NULL) {
			(void)fHw->panelPower(true);
		}
	}
	if (wake && fHaveLastFull && rec == NULL) {
		(void)present(fLastFull);       /* the frame that was on screen before */
	}
	if (rec != NULL) {
		(void)present(rec);
#if STARBITE_DISPLAY_LEGACY
		sweet_last_present_abs = mach_absolute_time();  /* 0413: the stall probe */
#endif
		IOFreeData(rec, SBD_SWAP_REC_MAX);
	}

	/* every 30 s, and for a milestone test boot (sbstop=display): the counters */
	if ((count % 1800) == 0) {
		reportAndStop(NULL);
	}
#if STARBITE_DISPLAY_LEGACY
	if ((count % 30) == 0 && starlte_sbstop_at("display")) {
		struct sbd_decon c = {};
		uint32_t limit = 45, want = 60;

		PE_parse_boot_argn("sbdisp_stop_s", &limit, sizeof(limit));
		PE_parse_boot_argn("sbdisp_stop_flips", &want, sizeof(want));
		if (fHw != NULL) {
			fHw->counters(&c);
		}
		if (fSelfTestFlips == 0 && c.flips_latched >= want && c.vsync_irqs >= 120) {
			reportAndStop("PASS");
			starlte_sbstop_now("display");
		} else if (sbd_uptime_ms() >= (uint64_t)limit * 1000) {
			reportAndStop("FAIL");
			starlte_sbstop_now("display");
		}
	}
#endif
}

void
AppleCLCD::reportAndStop(const char *verdict)
{
	struct sbd_decon c = {};

	if (fHw != NULL) {
		fHw->counters(&c);
	}
	if (verdict != NULL && strcmp(verdict, "PASS") == 0) {
		IOLog("StarbiteDisplay: M8 PASS: frame flips by address %llu, vsync IRQ counted %llu\n", c.flips_latched,
		    c.vsync_irqs);
	} else if (verdict != NULL) {
		IOLog("StarbiteDisplay: M8 FAIL: frame flips by address %llu, vsync IRQ counted %llu at %llu ms\n",
		    c.flips_latched, c.vsync_irqs, sbd_uptime_ms());
	}
	IOLog("StarbiteDisplay: mode %s: flips requested %llu latched %llu replaced %llu (zero-copy %llu, drawn %llu, "
	    "refused %llu), in place %llu, copies %llu; vsync irq %llu, frame done %llu, polled %llu, synthetic %llu, "
	    "delivered %llu; DECON timeouts %llu conflicts %llu; shown %#x\n", sbd_mode_name(fMode), c.flips_requested,
	    c.flips_latched, c.flips_replaced, fZeroCopyFlips, fCopyFlips, fFlipRefused, fInPlace, fCopies, c.vsync_irqs,
	    c.frame_done_irqs, c.refreshes_polled, fHw != NULL ? fHw->syntheticVsyncs() : 0, fVsyncs, c.timeout_irqs,
	    c.conflict_irqs, c.shown_base);
}

/* ---- self test: flips with no userland ---- */

void
AppleCLCD::fillTestPattern(const StarbiteTarget *t, uint32_t seed)
{
	/* eight colour bars, rotated by the seed, with a white line that moves down: tearing would show */
	static const uint32_t bars[8] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00, 0xffff00ff, 0xffff0000, 0xff0000ff, 0xff202020,
	};
	uint32_t line = (seed * 97) % t->height;

	for (uint32_t y = 0; y < t->height; y++) {
		uint32_t *row = (uint32_t *)(t->va + (uintptr_t)y * t->rowBytes);
		uint32_t colour = bars[((y * 8 / t->height) + seed) & 7];

		if (y >= line && y < line + 8) {
			colour = 0xffffffff;
		}
		for (uint32_t x = 0; x < t->width; x++) {
			row[x] = colour;
		}
	}
}

void
AppleCLCD::selfTestThread(void *arg, void *unused __unused)
{
	AppleCLCD *fb = (AppleCLCD *)arg;

	fb->selfTest(fb->fSelfTestFlips);
}

void
AppleCLCD::selfTest(uint32_t flips)
{
	struct sbd_decon before = {}, after = {};
	uint32_t latched = 0, late = 0;
	/* S-Boot's framebuffer keeps its picture (flip mode: target 0): only the second buffer is drawn into,
	 * and the test flips between the two */
	uint32_t draw = 1;

	IOSleep(500);
	fHw->counters(&before);
	IOLog("StarbiteDisplay: self test: %u flips between %#llx and %#llx (%s mode)\n", flips, fTargets[0].phys,
	    fTargets[1].phys, sbd_mode_name(fMode));
	for (uint32_t i = 0; i < flips; i++) {
		uint32_t to = (i & 1) ? 0 : 1;
		unsigned waited = 0;

		if (to == draw || fMode == kStarbitePresentScaler) {
			fillTestPattern(&fTargets[to], i / 2);
		}
		if (fHw->flip(fTargets[to].phys, fTargets[to].rowBytes) != SBD_OK) {
			IOLog("StarbiteDisplay: self test: flip %u refused\n", i);
			break;
		}
		while (!fHw->latched() && waited < 100) {
			IOSleep(2);
			waited += 2;
		}
		if (fHw->latched()) {
			latched++;
			late += waited > 34;
		}
		fFront = to;
		fFrontOwn = true;
		IOSleep(34);            /* two refreshes on screen */
	}
	IOSleep(50);
	fHw->counters(&after);
	IOLog("StarbiteDisplay: self test: %u of %u flips taken by the DECON (%u later than two refreshes); "
	    "flips latched +%llu, vsync IRQ +%llu, FRAME_COUNT polled +%llu, synthetic %llu: %s\n", latched, flips, late,
	    after.flips_latched - before.flips_latched, after.vsync_irqs - before.vsync_irqs,
	    after.refreshes_polled - before.refreshes_polled, fHw->syntheticVsyncs(),
	    latched == flips && after.vsync_irqs > before.vsync_irqs ? "ok" : "NOT ok");
	fSelfTestFlips = 0;             /* presents may run again */
}

/* ---- drawing ---- */

static uint16_t sbd_column[SBD_COLUMNS];

/*
 * Where the canvas rectangle (dx, dy, w, h) lands in target t: one to one when the target has the canvas's
 * size, otherwise scaled to the largest rectangle of the target with the canvas's aspect, centred.
 */
void
AppleCLCD::mapRect(const StarbiteTarget *t, int32_t dx, int32_t dy, int32_t w, int32_t h, int32_t *x0, int32_t *y0,
    int32_t *x1, int32_t *y1, int32_t *barX, int32_t *barY, int32_t *outW, int32_t *outH)
{
	int32_t lw = (int32_t)fLogicalW, lh = (int32_t)fLogicalH, tw = (int32_t)t->width, th = (int32_t)t->height;

	if (tw == lw && th == lh) {
		*barX = *barY = 0;
		*outW = tw;
		*outH = th;
		*x0 = dx;
		*y0 = dy;
		*x1 = dx + w;
		*y1 = dy + h;
	} else {
		*outW = (int32_t)((int64_t)lw * th / lh);
		*outH = th;
		if (*outW > tw) {
			*outW = tw;
			*outH = (int32_t)((int64_t)lh * tw / lw);
		}
		*barX = (tw - *outW) / 2;
		*barY = (th - *outH) / 2;
		*x0 = *barX + (int32_t)((int64_t)dx * *outW / lw);
		*x1 = *barX + (int32_t)(((int64_t)(dx + w) * *outW + lw - 1) / lw);
		*y0 = *barY + (int32_t)((int64_t)dy * *outH / lh);
		*y1 = *barY + (int32_t)(((int64_t)(dy + h) * *outH + lh - 1) / lh);
	}
	if (*x0 < *barX) {
		*x0 = *barX;
	}
	if (*y0 < *barY) {
		*y0 = *barY;
	}
	if (*x1 > *barX + *outW) {
		*x1 = *barX + *outW;
	}
	if (*y1 > *barY + *outH) {
		*y1 = *barY + *outH;
	}
}

/* save (true) the target's pixels of the current save-under rectangle, or put the saved ones back (false) */
void
AppleCLCD::saveUnder(const StarbiteTarget *t, bool save)
{
	if (fSuW <= 0 || fSuH <= 0 || (!save && fSuTarget != t->va)) {
		fSuHave = false;
		return;
	}
	for (int32_t y = 0; y < fSuH; y++) {
		uint32_t *pixels = (uint32_t *)(t->va + (uintptr_t)(fSuY + y) * t->rowBytes + (uintptr_t)fSuX * 4);
		uint32_t *buf = &fSuBuf[y * SBD_SU_MAX];

		if (save) {
			memcpy(buf, pixels, (size_t)fSuW * 4);
		} else {
			memcpy(pixels, buf, (size_t)fSuW * 4);
		}
	}
	fSuHave = save;
	fSuTarget = t->va;
}

/* sweetfbstats=1: what is in a full-size surface, at most once a second, without eyes on the panel */
void
AppleCLCD::surfaceStats(SweetSurface *s, uint32_t swapID)
{
	static int on = -1;
	static uint64_t last;
	uintptr_t src = s->kernelAddress();

	if (on < 0) {
		on = 0;
		PE_parse_boot_argn("sweetfbstats", &on, sizeof(on));
	}
	if (!on || src == 0 || s->bytesPerElement != 4 || s->width < 256 || s->height < 256 || sbd_uptime_ms() - last < 1000) {
		return;
	}
	last = sbd_uptime_ms();
	uint32_t lit = 0;
	uint64_t r = 0, g = 0, b = 0;
	char rows[16][17];
	for (uint32_t gy = 0; gy < 16; gy++) {
		for (uint32_t gx = 0; gx < 16; gx++) {
			uint32_t px = (gx * 2 + 1) * s->width / 32, py = (gy * 2 + 1) * s->height / 32;
			uint32_t v = *(const volatile uint32_t *)(src + s->offset + (size_t)py * s->bytesPerRow + px * 4);
			uint32_t B = v & 0xff, G = (v >> 8) & 0xff, R = (v >> 16) & 0xff;
			uint32_t y = (R * 3 + G * 6 + B) / 10;

			lit += y > 16;
			r += R;
			g += G;
			b += B;
			rows[gy][gx] = " .:-=+*#%@"[y * 10 / 256];
		}
		rows[gy][16] = 0;
	}
	IOLog("SWEET FBSTAT: swap %u surface %u %ux%u: %u/256 lit, avg rgb %llu,%llu,%llu\n", swapID, s->id, s->width,
	    s->height, lit, r / 256, g / 256, b / 256);
	for (uint32_t gy = 0; gy < 16; gy++) {
		IOLog("SWEET FBSTAT: |%s|\n", rows[gy]);
	}
}

/*
 * One layer into a target. Layer 0 is copied (scaled, nearest neighbour, when the target is not canvas-sized);
 * the layers above it (the pointer, overlays) are blended, premultiplied BGRA over what is there.
 */
uint64_t
AppleCLCD::drawLayer(const StarbiteTarget *t, SweetSurface *s, const struct sbd_swap_layer *layer, uint32_t index)
{
	int32_t sx = layer->sx, sy = layer->sy, dx = layer->dx, dy = layer->dy, w = layer->dw, h = layer->dh;
	int32_t x0, y0, x1, y1, barX, barY, outW, outH;
	uintptr_t base = s->kernelAddress();
	bool blend = index > 0;
	bool oneToOne = t->width == fLogicalW && t->height == fLogicalH;

	if (base == 0 || s->bytesPerElement != 4 || s->elementWidth != 1 || s->elementHeight != 1 || s->memory == NULL) {
		return 0;
	}
	if (w <= 0 || h <= 0) {
		w = (int32_t)s->width;
		h = (int32_t)s->height;
	}
	if (sx < 0 || sy < 0) {
		sx = sy = 0;
	}
	if (dx < 0 || dy < 0) {
		dx = dy = 0;
	}
	if (w > (int32_t)s->width - sx) {
		w = (int32_t)s->width - sx;
	}
	if (h > (int32_t)s->height - sy) {
		h = (int32_t)s->height - sy;
	}
	if (w > (int32_t)fLogicalW - dx) {
		w = (int32_t)fLogicalW - dx;
	}
	if (h > (int32_t)fLogicalH - dy) {
		h = (int32_t)fLogicalH - dy;
	}
	if (w <= 0 || h <= 0) {
		return 0;
	}
	uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow + (uint64_t)(sx + w) * 4;
	if (last > s->memory->getLength()) {
		return 0;
	}
	uintptr_t src = base + s->offset + (uintptr_t)sy * s->bytesPerRow + (uintptr_t)sx * 4;

	if (blend && w <= 256 && h <= 256) {
#if STARBITE_DISPLAY_LEGACY
		/* 0397: where the pointer layer is drawn (canvas coordinates), for the touch driver's direct touch */
		sweet_fb_cur_x = dx - sx;
		sweet_fb_cur_y = dy - sy;
		__atomic_fetch_add(&sweet_fb_cur_seq, 1, __ATOMIC_RELEASE);
#endif
	}
	mapRect(t, dx, dy, w, h, &x0, &y0, &x1, &y1, &barX, &barY, &outW, &outH);
	int32_t pw = x1 - x0;
	if (pw <= 0 || y1 <= y0 || pw > SBD_COLUMNS) {
		return 0;
	}
	if (blend) {
		/* 0396: the target's pixels under the layer are saved before it is blended in */
		fSuX = x0;
		fSuY = y0;
		fSuW = pw > SBD_SU_MAX ? SBD_SU_MAX : pw;
		fSuH = y1 - y0 > SBD_SU_MAX ? SBD_SU_MAX : y1 - y0;
		saveUnder(t, true);
	}
	if (oneToOne && !blend) {
		for (int32_t y = 0; y < h; y++) {
			memcpy((void *)(t->va + (uintptr_t)(y0 + y) * t->rowBytes + (uintptr_t)x0 * 4),
			    (const void *)(src + (uintptr_t)y * s->bytesPerRow), (size_t)pw * 4);
		}
		return (uint64_t)pw * (uint64_t)h * 4;
	}
	for (int32_t x = 0; x < pw; x++) {
		int32_t c = oneToOne ? x : (int32_t)((int64_t)(x0 + x - barX) * (int32_t)fLogicalW / outW) - dx;

		sbd_column[x] = (uint16_t)(c < 0 ? 0 : (c >= w ? w - 1 : c));
	}
	int32_t prev = -1;
	uint32_t *prevRow = NULL;
	for (int32_t py = y0; py < y1; py++) {
		int32_t r = oneToOne ? py - y0 : (int32_t)((int64_t)(py - barY) * (int32_t)fLogicalH / outH) - dy;
		uint32_t *out = (uint32_t *)(t->va + (uintptr_t)py * t->rowBytes + (uintptr_t)x0 * 4);

		r = r < 0 ? 0 : (r >= h ? h - 1 : r);
		const uint32_t *in = (const uint32_t *)(src + (uintptr_t)r * s->bytesPerRow);
		if (blend) {
			for (int32_t x = 0; x < pw; x++) {
				uint32_t v = in[sbd_column[x]], a = v >> 24;

				if (a == 0) {
					continue;
				}
				if (a == 255) {
					out[x] = v;
					continue;
				}
				uint32_t o = out[x], k = 255 - a, res = 0xff000000u;
				for (uint32_t sh = 0; sh < 24; sh += 8) {
					uint32_t c = ((v >> sh) & 0xff) + (((o >> sh) & 0xff) * k + 127) / 255;

					res |= (c > 255 ? 255 : c) << sh;
				}
				out[x] = res;
			}
			continue;
		}
		if (r == prev && prevRow != NULL) {
			memcpy(out, prevRow, (size_t)pw * 4);   /* a repeated source row: copy the row just made */
		} else {
			for (int32_t x = 0; x < pw; x++) {
				out[x] = in[sbd_column[x]];
			}
			prev = r;
		}
		prevRow = out;
	}
	return (uint64_t)pw * (uint64_t)(y1 - y0) * 4;
}

uint64_t
AppleCLCD::drawLayers(const struct sbd_swap *swap, const StarbiteTarget *t)
{
	bool l0 = swap->layer[0].enabled, upper = false;
	uint64_t drawn = 0;
	static uint32_t logged;
	static uintptr_t cleared[2];

	/* bars beside a scaled picture are black: clear a target once before its first scaled frame */
	if (l0 && (t->width != fLogicalW || t->height != fLogicalH) && cleared[0] != t->va && cleared[1] != t->va) {
		cleared[cleared[0] == 0 ? 0 : 1] = t->va;
		bzero((void *)t->va, (size_t)t->rowBytes * t->height);
	}
	for (uint32_t i = 0; i < SBD_SWAP_LAYERS; i++) {
		const struct sbd_swap_layer *layer = &swap->layer[i];

		if (!layer->enabled) {
			continue;
		}
		SweetSurface *s = sweet_surface_copy(layer->surface);
		if (logged < 16) {
			logged++;
			IOLog("StarbiteDisplay: swap %u layer %u surface %u %ux%u fmt %#x bpr %u a %d,%d %dx%d b %d,%d %dx%d\n",
			    swap->swap_id, i, layer->surface, s ? s->width : 0, s ? s->height : 0, s ? s->pixelFormat : 0,
			    s ? s->bytesPerRow : 0, layer->sx, layer->sy, layer->sw, layer->sh, layer->dx, layer->dy, layer->dw,
			    layer->dh);
		}
		if (s == NULL) {
			continue;
		}
		if (s == fPanelSurface && t->va == fTargets[0].va && fMode != kStarbitePresentScaler) {
			s->release();           /* drawn straight into S-Boot's framebuffer: nothing to copy */
			continue;
		}
		if (i == 0) {
			surfaceStats(s, swap->swap_id);
#if STARBITE_DISPLAY_LEGACY
			if (s->width == fLogicalW && s->height == fLogicalH) {
				__atomic_store_n(&sbd_last_surface_id, s->id, __ATOMIC_RELEASE);
			}
#endif
		} else {
			/* a pointer-only swap first puts back what was under the pointer */
			if (!upper && fSuHave && !l0) {
				saveUnder(t, false);
			}
			upper = true;
		}
		drawn += drawLayer(t, s, layer, i);
		s->release();
	}
	if (l0 && !upper) {
		fSuHave = false;                /* the new frame has no pointer on it */
	} else if (!l0 && !upper && fSuHave) {
		saveUnder(t, false);            /* pointer hidden: put the frame back */
		fSuHave = false;
	}
	return drawn;
}

/*
 * Can the DECON read this surface where it is? One full-canvas layer whose pixels are one physically contiguous
 * piece below the scan-out limit, 32-bit BGRA, of the size the current channel is set up for. Its pixels are
 * pushed out of the processor's cache first (the DMA reads RAM).
 */
bool
AppleCLCD::surfaceFlippable(SweetSurface *s, const struct sbd_swap_layer *layer, uint64_t *phys)
{
	uint32_t wantW = fMode == kStarbitePresentScaler ? fLogicalW : fWidth;
	uint32_t wantH = fMode == kStarbitePresentScaler ? fLogicalH : fHeight;
	IOByteCount len = 0;

	if (s->memory == NULL || s->bytesPerElement != 4 || s->elementWidth != 1 || s->elementHeight != 1 ||
	    s->pixelFormat != SBD_FORMAT_BGRA || s->width != wantW || s->height != wantH ||
	    layer->sx > 0 || layer->sy > 0 || layer->dx > 0 || layer->dy > 0) {
		return false;
	}
	uint64_t bytes = (uint64_t)s->bytesPerRow * s->height;
	if ((uint64_t)s->offset + bytes > s->memory->getLength()) {
		return false;
	}
	addr64_t pa = s->memory->getPhysicalSegment(s->offset, &len, kIOMemoryMapperNone);
	if (pa == 0 || len < bytes || fHw->bufferOK(pa, bytes, s->bytesPerRow) != SBD_OK) {
		return false;
	}
	if (s->memory->performOperation(kIOMemoryIncoherentIOStore, s->offset, (IOByteCount)bytes) != kIOReturnSuccess) {
		return false;           /* cannot promise the pixels are in RAM: draw a copy instead */
	}
	*phys = pa;
	return true;
}

uint64_t
AppleCLCD::present(const uint8_t *rec)
{
	struct sbd_swap swap;
	uint64_t drawn = 0;

	if (!sbd_swap_parse(rec, SBD_SWAP_REC_MAX, &swap)) {
		return 0;
	}
	if (!fConsolePaused) {
		int quiet = 0;

		fConsolePaused = true;
#if STARBITE_DISPLAY_LEGACY
		if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
			sweet_console_muted = 1;
		}
#else
		(void)quiet;
#endif
	}
	bool l0 = swap.layer[0].enabled;

	if (fMode == kStarbitePresentCopy) {
#if STARBITE_DISPLAY_LEGACY
		/* sweetfb_gpu: the Mali scales a lone full-canvas layer into S-Boot's framebuffer */
		if (fGpu && l0 && swap.enabled == 1) {
			SweetSurface *s = sweet_surface_copy(swap.layer[0].surface);
			bool done = s != NULL && s->width == fLogicalW && s->height == fLogicalH && s->bytesPerElement == 4 &&
			    gpuPresent(s, 0, 0, (int32_t)fLogicalW, (int32_t)fLogicalH);

			OSSafeReleaseNULL(s);
			if (done) {
				fCopies++;
				drawn = (uint64_t)fWidth * fHeight * 4;
				goto shown;
			}
		}
#endif
		drawn = drawLayers(&swap, &fTargets[0]);
		fCopies += drawn != 0;
		goto shown;
	}

	if (l0) {
		memcpy(fLastFull, rec, SBD_SWAP_REC_MAX);
		fHaveLastFull = true;
		/* by address alone? */
		if (swap.enabled == 1) {
			SweetSurface *s = sweet_surface_copy(swap.layer[0].surface);
			uint64_t phys = 0;

			if (s != NULL && surfaceFlippable(s, &swap.layer[0], &phys) && fHw->flip(phys, s->bytesPerRow) == SBD_OK) {
				surfaceStats(s, swap.swap_id);
				OSSafeReleaseNULL(fShownBefore);        /* two flips ago: the DECON has let go of it */
				fShownBefore = fShown;
				fShown = s;
				fFrontOwn = false;
				fSuHave = false;
				fZeroCopyFlips++;
				drawn = (uint64_t)s->bytesPerRow * s->height;
				goto shown;
			}
			if (s != NULL) {
				fFlipRefused++;
				s->release();
			}
		}
		/* draw it into the buffer the DECON is not reading, then flip to that */
		uint32_t back = fFrontOwn ? 1 - fFront : 0;
		const StarbiteTarget *t = &fTargets[back];

		drawn = drawLayers(&swap, t);
		if (drawn != 0 && fHw->flip(t->phys, t->rowBytes) == SBD_OK) {
			fFront = back;
			fFrontOwn = true;
			fCopyFlips++;
		}
		goto shown;
	}

	/* no layer 0: a small update (the pointer) on top of the frame that is showing */
	if (fFrontOwn) {
		drawn = drawLayers(&swap, &fTargets[fFront]);
		fInPlace += drawn != 0;
	} else if (fHaveLastFull) {
		/* the DECON reads a client's surface: it is not ours to draw on. Rebuild the frame in a buffer of ours. */
		struct sbd_swap full;
		const StarbiteTarget *t = &fTargets[0];

		if (sbd_swap_parse(fLastFull, SBD_SWAP_REC_MAX, &full)) {
			fSuHave = false;
			drawn = drawLayers(&full, t) + drawLayers(&swap, t);
			if (drawn != 0 && fHw->flip(t->phys, t->rowBytes) == SBD_OK) {
				fFront = 0;
				fFrontOwn = true;
				fCopyFlips++;
			}
		}
	}

shown:
#if STARBITE_DISPLAY_LEGACY
	if (drawn != 0 && sweet_fb_first_present == 0) {
		sweet_fb_first_present = mach_absolute_time();  /* 0421's dead-man: a first frame was shown */
	}
#endif
	return drawn;
}

void
AppleCLCD::setPower(bool off)
{
	IOLockLock(fLock);
	if (off != fPowerOff) {
		fPowerOff = off;
		fDirty = true;          /* on: show the latest frame again */
	}
	IOLockUnlock(fLock);
	IOLog("StarbiteDisplay: display %s\n", off ? "off" : "on");
}

#if STARBITE_DISPLAY_LEGACY
/*
 * sweetfb_gpu (copy mode): the Mali draws the whole canvas scaled into S-Boot's framebuffer (starlte_gpu.c, 0332).
 * The surface's pages are mapped into a GPU slot the first time; its pixels are cleaned out of the cache first.
 * Called only from present(). False: use the CPU.
 */
bool
AppleCLCD::gpuPresent(SweetSurface *s, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
	static uint64_t pages[SBD_GPU_MAX_PAGES];
	static uint32_t failures;
	IOByteCount len = 0;

	if (!starlte_gpu_present_ready() || failures >= 8 || s->memory == NULL) {
		return false;
	}
	addr64_t pa0 = s->memory->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
	int slot = pa0 ? starlte_gpu_present_slot(s->id, pa0 & ~0xfffULL) : -1;
	if (slot < 0 && pa0 != 0) {
		unsigned n = (unsigned)((s->memory->getLength() + 0xfff) / 0x1000);
		bool ok = n <= SBD_GPU_MAX_PAGES;

		for (unsigned i = 0; ok && i < n; i++) {
			addr64_t pa = s->memory->getPhysicalSegment((IOByteCount)i * 0x1000, &len, kIOMemoryMapperNone);

			ok = pa != 0;
			pages[i] = pa & ~0xfffULL;
		}
		slot = ok ? starlte_gpu_present_map(s->id, pages, n) : -1;
	}
	if (slot < 0) {
		IOLog("StarbiteDisplay: GPU present: surface %u not mapped (%u of 8)\n", s->id, ++failures);
		return false;
	}
	uint64_t off = (uint64_t)s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4;
	uint64_t bytes = (uint64_t)(h - 1) * s->bytesPerRow + (uint64_t)w * 4;
	if (s->memory->performOperation(kIOMemoryIncoherentIOStore, (IOByteCount)off, (IOByteCount)bytes) != kIOReturnSuccess) {
		failures++;
		return false;
	}
	if (!starlte_gpu_present_draw(slot, (uint32_t)off, s->bytesPerRow, (uint32_t)w, (uint32_t)h)) {
		IOLog("StarbiteDisplay: GPU present: %dx%d stride %u not drawn (%u of 8)\n", w, h, s->bytesPerRow, ++failures);
		return false;
	}
	return true;
}

/* For the GPU copy test (herolte_gpu.c): the pages of the last full-canvas surface shown, its layout and address. */
bool
sweet_fb_last_surface(uint64_t *pages, unsigned max_pages, unsigned *npages, uint32_t *offset, uint32_t *stride,
    uint32_t *width, uint32_t *height, uintptr_t *kva)
{
	uint32_t id = __atomic_load_n(&sbd_last_surface_id, __ATOMIC_ACQUIRE);
	SweetSurface *s = id ? sweet_surface_copy(id) : NULL;

	if (s == NULL) {
		return false;
	}
	bool ok = s->memory != NULL && s->bytesPerElement == 4 && s->elementWidth == 1 && s->elementHeight == 1;
	unsigned n = ok ? (unsigned)((s->memory->getLength() + 0xfff) / 0x1000) : 0;
	if (ok && n <= max_pages) {
		for (unsigned i = 0; i < n; i++) {
			IOByteCount len = 0;
			addr64_t pa = s->memory->getPhysicalSegment((IOByteCount)i * 0x1000, &len, kIOMemoryMapperNone);

			if (pa == 0) {
				ok = false;
				break;
			}
			pages[i] = pa & ~0xfffULL;
		}
	} else {
		ok = false;
	}
	if (ok) {
		*npages = n;
		*offset = s->offset;
		*stride = s->bytesPerRow;
		*width = s->width;
		*height = s->height;
		*kva = s->kernelAddress();
	}
	s->release();
	return ok && *kva != 0;
}
#else
bool
AppleCLCD::gpuPresent(SweetSurface *s __unused, int32_t sx __unused, int32_t sy __unused, int32_t w __unused,
    int32_t h __unused)
{
	return false;
}
#endif /* STARBITE_DISPLAY_LEGACY */

/* ------------------------------------------------------------------------------------------------------------
 * IOMobileFramebufferUserClient: the selector ABI, unchanged from SweetFramebuffer.cpp
 *   3  GetLayerDefaultSurface -> [IOSurface port name]     4  SwapBegin -> [swap id]
 *   5  SwapEnd (struct 0x400..0x600)                       6  SwapWait
 *   7  GetID                                               8  GetDisplaySize -> [w, h]
 *   12 RequestPowerChange [state]                          18 IsMainDisplay
 *   28 GetDotPitch                                         72 Enable/DisableNotifications [func, refcon, type]
 * Everything else is accepted, answered with zeros and logged once.
 * ------------------------------------------------------------------------------------------------------------ */

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;

	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SBD_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count, kIOUserNotifyOptionCanDrop);
}

static uint64_t sbd_selectors_logged;   /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kSBDGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSBDSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSBDSwapEnd: {
		if (args->structureInputDescriptor != NULL || args->structureInputSize < SBD_SWAP_REC_MIN) {
			ret = kIOReturnBadArgument;
			break;
		}
		uint8_t *rec = (uint8_t *)IOMallocData(SBD_SWAP_REC_MAX);
		if (rec == NULL) {
			ret = kIOReturnNoMemory;
			break;
		}
		uint32_t size = (uint32_t)args->structureInputSize;
		if (size > SBD_SWAP_REC_MAX) {
			size = SBD_SWAP_REC_MAX;
		}
		bzero(rec, SBD_SWAP_REC_MAX);
		memcpy(rec, args->structureInput, size);
		fFB->swap(rec, size);
		IOFreeData(rec, SBD_SWAP_REC_MAX);
		break;
	}
	case kSBDGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSBDGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kSBDIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSBDGetDotPitch:
		out[0] = SBD_DOT_PITCH;
		outCount = 1;
		break;
	case kSBDNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SBD_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			char name[32];

			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			proc_selfname(name, sizeof(name));
			IOLog("StarbiteDisplay: %s notifications type %u %s\n", name, type, args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSBDRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			fFB->setPower(args->scalarInput[0] == 0);
		}
		break;
	case kSBDSwapWait:
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(sbd_selectors_logged & (1ULL << selector))) {
		char name[32];

		if (selector < 64) {
			sbd_selectors_logged |= 1ULL << selector;
		}
		proc_selfname(name, sizeof(name));
		IOLog("StarbiteDisplay: %s sel %u in %u/%u out %u/%u -> %#x\n", name, selector, args->scalarInputCount,
		    args->structureInputSize, args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
