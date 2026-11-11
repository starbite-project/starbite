/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteDisplay: the Galaxy S9's display for Apple's OSes (docs/APPLE-S9-DRIVERS.md Part 3, row 18).
 *
 * Two services:
 *
 *   StarbiteDisplay   the hardware. Matches the ADT node "decon" (compatible starbite,decon). Takes every address
 *                     and interrupt from that nub and from the "dsim" nub: DECON_F, the DPU's DMA and DPP blocks,
 *                     DSIM0. Does address flips, counts frame interrupts, sends the panel's DCS commands.
 *
 *   AppleCLCD         what Apple's userland sees: an IOMobileFramebuffer on the ADT's "disp0" node (which carries
 *                     canvas-width / canvas-height), with IOMobileFramebufferUserClient and the selector ABI that
 *                     iOS 16, iPadOS 18, iOS 26, macOS 13 and macOS 26.5 accept (SweetFramebuffer.cpp, patches
 *                     0046..0398, ours). Swaps go to StarbiteDisplay's flips; the CPU copy into S-Boot's
 *                     framebuffer stays as the fallback (boot-arg sbdisp=copy, or whenever the hardware is not as
 *                     expected).
 *
 * Boot-args (all optional):
 *   sbdisp=flip|scale|copy   flip (default): flips between buffers of the panel's size on S-Boot's channel;
 *                            scale: the window on VGF0, canvas-sized buffers scaled by the DPP;
 *                            copy: no DECON writes at all, today's CPU copy.
 *   sbdisp_irq=0             no interrupts: refreshes are counted from FRAME_COUNT by the timer.
 *   sbdisp_panel=0|1|2|3     DSIM writes: 0 none; 1 brightness only (default); 2 also display off/on with the
 *                            power state; 3 also sleep in/out.
 *   sbdisp_bl=N              brightness 0-255 once at start.
 *   sbdisp_selftest=N        N flips between two test pictures at start, no userland needed (PHONE-TEST.md).
 *   sbdisp_limit=0x...       scan-out address limit (default 0xe0000000).
 *   sweetfb_hz, sweetfb_noconsole, sweetfb_native, sweetfb_lw, sweetfb_lh, sweetfbstats: as in SweetFramebuffer.
 */
#ifndef _STARBITE_DISPLAY_H
#define _STARBITE_DISPLAY_H

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>

#include "hw/sbd_decon.h"
#include "hw/sbd_dsim.h"
#include "hw/sbd_panel.h"
#include "hw/sbd_swap.h"

class SweetSurface;
class AppleCLCD;
class IOMobileFramebufferUserClient;

#define SBD_VSYNC_US            16667
#define SBD_NOTIFY_TYPES        8
#define SBD_NOTIFY_VSYNC        5
#define SBD_DOT_PITCH           458             /* ppi reported, as the iPhone X panel (kept from SweetFramebuffer) */
#define SBD_FORMAT_BGRA         0x42475241      /* 'BGRA' */

/* a refresh, delivered on StarbiteDisplay's work loop */
typedef void (*StarbiteDisplayVsync)(void *target, uint64_t count, uint64_t abstime, uint32_t events);

class StarbiteDisplay : public IOService {
	OSDeclareDefaultStructors(StarbiteDisplay);

	IOMemoryMap        *fMap[SBD_BLK_COUNT];
	uintptr_t           fBase[SBD_BLK_COUNT];
	uint32_t            fSize[SBD_BLK_COUNT];
	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fTimer;
	IOFilterInterruptEventSource *fIrq[3];  /* frame start, frame done, extra */
	IOSimpleLock       *fHwLock;            /* the DECON state: flips against the interrupt filter */
	IOLock             *fPanelLock;         /* DSIM packets, one sequence at a time */
	struct sbd_decon    fDecon;
	struct sbd_panel    fPanel;
	bool                fHaveDsim, fIrqOn;
	uint32_t            fPanelPolicy;       /* sbdisp_panel */
	volatile uint32_t   fIrqEvents;         /* SBD_EV_* gathered by the filter for the action */
	volatile uint32_t   fIrqVsyncs;         /* frame starts seen by the filter and not yet delivered */
	uint64_t            fIrqStamp;          /* mach_absolute_time() of the last frame start */
	uint64_t            fVsyncCount;        /* refreshes delivered to the client, whatever the source */
	uint64_t            fSyntheticVsyncs;   /* of them, made by the timer because the hardware had none */
	StarbiteDisplayVsync fVsyncFn;
	void               *fVsyncTarget;

	static uint32_t ioRead(void *ctx, unsigned block, uint32_t off);
	static void     ioWrite(void *ctx, unsigned block, uint32_t off, uint32_t val);
	static void     ioDelay(void *ctx, uint32_t us);
	static bool     irqFilter(OSObject *owner, IOFilterInterruptEventSource *src);
	static void     irqAction(OSObject *owner, IOInterruptEventSource *src, int count);
	static void     timerFired(OSObject *owner, IOTimerEventSource *sender);
	void            deliver(uint64_t stamp, uint32_t events, bool synthetic);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

	/* scan-out; every call may be made from a thread, none from an interrupt */
	int  probe(uint64_t fbPhys, uint32_t width, uint32_t height, uint32_t rowBytes);
	bool haveChannels() const { return fBase[SBD_BLK_IDMA] != 0 && fBase[SBD_BLK_DPP] != 0; }
	int  attachFlip();
	int  attachScaler(uint64_t phys, uint32_t w, uint32_t h, uint32_t rowBytes, const struct sbd_rect *dst);
	void detach();
	int  bufferOK(uint64_t phys, uint64_t bytes, uint32_t rowBytes);
	int  flip(uint64_t phys, uint32_t rowBytes);
	bool latched();
	void counters(struct sbd_decon *copy);
	uint64_t syntheticVsyncs() const { return fSyntheticVsyncs; }
	bool interruptsOn() const { return fIrqOn; }

	/* refreshes: one client */
	void setVsyncClient(StarbiteDisplayVsync fn, void *target);

	/* panel */
	bool     panelUsable() const { return fHaveDsim && fPanelPolicy != 0; }
	uint32_t panelPolicy() const { return fPanelPolicy; }
	IOReturn setBrightness(int level);      /* 0-255 */
	int      brightness() const { return fPanel.level; }
	IOReturn panelPower(bool on);           /* by policy: nothing, 28/29, or sleep in/out */
};

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

/* one buffer this driver draws into and the DECON can read */
struct StarbiteTarget {
	uintptr_t           va;
	uint64_t            phys;
	uint32_t            width, height, rowBytes;
	IOMemoryDescriptor *memory;         /* for the cache store before a flip */
	IOMemoryMap        *map;            /* the kernel mapping of a carve-out buffer (NULL for an allocated one) */
};

enum {
	kStarbitePresentCopy   = 0,         /* CPU copy into S-Boot's framebuffer (SweetFramebuffer's path) */
	kStarbitePresentFlip   = 1,         /* own panel-sized buffers, flipped by address */
	kStarbitePresentScaler = 2          /* canvas-sized buffers on VGF0, flipped by address */
};

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fTimer;         /* only without StarbiteDisplay */
	StarbiteDisplay    *fHw;

	static void timerFired(OSObject *owner, IOTimerEventSource *sender);
	static void hwVsync(void *target, uint64_t count, uint64_t abstime, uint32_t events);
	void tick(uint64_t count, uint64_t abstime);

	bool setupFlip();
	bool setupScaler();
	bool allocTarget(StarbiteTarget *t, uint32_t width, uint32_t height, uint32_t rowBytes);
	bool allocCarveout(StarbiteTarget *t, uint32_t width, uint32_t height, uint32_t rowBytes, vm_size_t bytes);
	void freeTarget(StarbiteTarget *t);
	void readCarveout();
	void publishProperties();
	void fillTestPattern(const StarbiteTarget *t, uint32_t seed);
	static void selfTestThread(void *arg, void *unused);
	void selfTest(uint32_t flips);
	void reportAndStop(const char *why);

	/* drawing (called only from present(), which the tick serialises) */
	bool surfaceFlippable(SweetSurface *s, const struct sbd_swap_layer *layer, uint64_t *phys);
	uint64_t drawLayers(const struct sbd_swap *swap, const StarbiteTarget *t);
	uint64_t drawLayer(const StarbiteTarget *t, SweetSurface *s, const struct sbd_swap_layer *layer, uint32_t index);
	void mapRect(const StarbiteTarget *t, int32_t dx, int32_t dy, int32_t w, int32_t h,
	    int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1, int32_t *barX, int32_t *barY, int32_t *outW, int32_t *outH);
	void saveUnder(const StarbiteTarget *t, bool save);
	void surfaceStats(SweetSurface *s, uint32_t swapID);
	bool gpuPresent(SweetSurface *s, int32_t sx, int32_t sy, int32_t w, int32_t h);

public:
	uint64_t      fPhys;                        /* S-Boot's framebuffer */
	uint32_t      fWidth, fHeight, fRowBytes;   /* the panel */
	uint32_t      fLogicalW, fLogicalH;         /* the canvas userland draws */
	bool          fScaled;                      /* canvas != panel */
	bool          fGpu;                         /* sweetfb_gpu: the Mali scales (copy mode only) */
	SweetSurface *fSurface;                     /* the default framebuffer surface */
	SweetSurface *fPanelSurface;                /* S-Boot's framebuffer as a surface (its mapping) */
	IOLock       *fLock;                        /* swaps and the client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	uint8_t       fPending[SBD_SWAP_REC_MAX];   /* the latest swap, not yet shown */
	uint8_t       fLastFull[SBD_SWAP_REC_MAX];  /* the latest swap with a full layer 0 */
	bool          fHaveLastFull;
	bool          fDirty;
	bool          fConsolePaused;
	uint32_t      fFlushTicks;
	bool          fPowerOff;

	uint32_t      fMode;                        /* kStarbitePresent* */
	StarbiteTarget fTargets[2];
	uint64_t      fCarveBase, fCarveSize, fCarveNext;   /* the board's scan-out carve-out (M8c), bump-allocated */
	uint32_t      fFront;                       /* which of fTargets the DECON reads, when fFrontOwn */
	bool          fFrontOwn;                    /* false: the DECON reads a client's surface (zero copy) */
	SweetSurface *fShown, *fShownBefore;        /* kept alive while the DECON may read them */
	uint64_t      fZeroCopyFlips, fCopyFlips, fInPlace, fCopies, fFlipRefused;
	uint32_t      fSelfTestFlips;
	uint64_t      fVsyncs;

	/* pointer save-under (0396), on the buffer it was drawn on */
	uint32_t     *fSuBuf;
	int32_t       fSuX, fSuY, fSuW, fSuH;
	bool          fSuHave;
	uintptr_t     fSuTarget;

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec, uint32_t size);
	uint64_t present(const uint8_t *rec);
	void setPower(bool off);
};

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[SBD_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SBD_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

#endif /* _STARBITE_DISPLAY_H */
