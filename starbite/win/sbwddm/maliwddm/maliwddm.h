/* SPDX-License-Identifier: GPL-2.0 */
/*
 * maliwddm: the WDDM 1.3 driver for the RK3576's Mali-G52 and its display
 * (milestone M5.2, docs/M5.2-PLAN.md). The display half is malidod's; the
 * GPU half is drivers/common/maligpu; this layer is what WDDM asks of a full
 * driver: allocations in one aperture segment mapped into the Mali MMU, one
 * engine node, submissions of job chains with fences.
 *
 * Shape and several WDDM details follow virtio-win's viogpu3d (BSD-3), the
 * one open WDDM driver with a Mesa d3d10umd user-mode side.
 */
#pragma once
#include <ntddk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>
#include "../common/maligpu.h"
#include "maliwddm_abi.h"
#include "../../sbdisplay/sbdc_core.h"   /* T24: DECON flips */
#include "../s9.h"
#include "../sbx_cmd.h"

#define MW_TAG                  'dwlM'
#define MW_SEGMENT_ID           1           /* the aperture segment */
/* S9 port (T15): 1.5 GiB (was 1 GiB). The user's session (2026-11-09) ran out from minute 4 with 128 MB tiler
 * heaps in every process (now 32/64 MB, sb-patches/0113); the margin is for many apps at once. Only an
 * address range for VidMm: memory is used only as allocations are made. */
#define MW_SEGMENT_SIZE         (1536ULL * 1024 * 1024)
#define MW_QUEUE_DEPTH          16          /* submissions the KMD holds */

/* 1: a render-only adapter (no VidPN sources or children; another device's
 * display-only driver keeps the screen). 0: a full adapter, display and
 * render on GPU0.
 *
 * Render-only is the WDDM pairing for a separate display device, but on
 * this ACPI GPU it never got past StartDevice: dxgkrnl reported the adapter
 * as a POST device (adapter type 0x9) and refused it with
 * STATUS_GRAPHICS_INVALID_DRIVER_MODEL whatever the DSDT order, resources,
 * INF class or caps (WinPE runs 5-11). A full adapter started (run 6 on the
 * installed Windows); it was black there only because DWM cannot render
 * through WARP, and WinPE has no DWM. */
#ifndef MW_RENDER_ONLY
#define MW_RENDER_ONLY          0
#endif
#define MW_W2(x)                L##x
#define MW_W(x)                 MW_W2(x)

/* ---- objects ---- */

typedef struct _MW_ALLOCATION {
    SIZE_T Size;
    ULONG Pages;
    ULONG Flags;                /* MW_ALLOC_* */
    ULONG64 GpuVa;              /* reserved at creation, fixed for life */
    BOOLEAN Mapped;             /* resident: pages mapped at GpuVa */
    BOOLEAN FbAliased;          /* T13z: a full-screen primary whose GPU pages are the framebuffer's */
    /* Standard allocations (primary, shadow, staging) the runtime creates. */
    UINT Width, Height, Pitch;
    D3DDDIFORMAT Format;
    /* While mapped: the backing pages VidMm handed MAP_APERTURE_SEGMENT,
     * which is how a present reaches the pixels on the CPU. */
    PMDL Mdl;
    ULONG MdlPage;
    /* S9 port (T13e): the CPU view the KMD's blts and flip copies read, mapped on first use with the
     * caching VidMm gave the pages (MAP_APERTURE_SEGMENT Flags.CacheCoherent; ours is a non-coherent
     * segment, so write-combined). The default MmGetSystemAddressForMdlSafe view is cached: mismatched
     * attributes on ARM64, stale lines instead of the GPU's pixels (T13d: 223 blts to the panel, black). */
    BOOLEAN Coherent;
    PVOID CpuMap;
    LONG ScanSlot;              /* T24: the scanout buffer this full-screen primary renders into (-1: none) */
    PVOID CpuMapCached;         /* T26: a cached view for blt-present reads, invalidated line by line before use */
} MW_ALLOCATION;

typedef struct _MW_ADAPTER MW_ADAPTER;

typedef struct _MW_DEVICE {
    MW_ADAPTER *Adapter;
} MW_DEVICE;

typedef struct _MW_CONTEXT {
    MW_DEVICE *Device;
} MW_CONTEXT;

/* What a DMA buffer carries, kept in its private data by Render and found
 * there again by SubmitCommand. */
/* A present the CPU carries out at submission (step d): a blt or a colour
 * fill into the primary (the POST framebuffer) or another allocation. */
#define MW_PRESENT_NONE         0
#define MW_PRESENT_BLT          1
#define MW_PRESENT_FILL         2
#define MW_PRESENT_FLIP         3       /* S9 port (T13h): DWM's flips, emulated (the panel shows Src) */
#define MW_MAX_RECTS            16
#define MW_COPY_HELPERS         3       /* T27: blt copies split over the submitting thread and these */
#define MW_COPY_CHUNK_ROWS      16
#define MW_COPY_IDLE            0x3FFFFFFF  /* MW_COPY_JOB.Next while no job is open: helpers take nothing */

typedef struct _MW_PRESENT_OP {
    ULONG Kind;
    ULONG Color;
    MW_ALLOCATION *Src, *Dst;
    RECT SrcRect, DstRect;
    ULONG NumRects;             /* 0: DstRect alone */
    RECT Rects[MW_MAX_RECTS];   /* destination sub-rectangles */
    /* T25: a flip's source as dxgkrnl addresses it (its segment address, from DxgkDdiPatch): what the vsync
     * interrupt must report once the flip is done, for dxgkrnl to count the flip as completed */
    PHYSICAL_ADDRESS SrcPhys;
    BOOLEAN SrcPhysValid;
} MW_PRESENT_OP;

/* T27: one blt copy, split into chunks of MW_COPY_CHUNK_ROWS rows over the rectangles taken together. The rectangles
 * are clipped to both surfaces; a source pixel is at (x + Dx, y + Dy). Next/Done are taken atomically. */
typedef struct _MW_COPY_JOB {
    PUCHAR Dst;
    const UCHAR *Src;
    LONG DstPitch, SrcPitch, Dx, Dy;
    ULONG NRects;
    RECT R[MW_MAX_RECTS];
    LONG RowStart[MW_MAX_RECTS + 1];
    LONG Chunks;
    volatile LONG Next, Done;
} MW_COPY_JOB;

typedef struct _MW_DMA_PRIVATE {
    ULONG Count;
    ULONG Reserved;
    MW_COMMAND Commands[MW_MAX_COMMANDS];
    MW_PRESENT_OP Present;
} MW_DMA_PRIVATE;

/* A submission the hardware works through, one job chain at a time. */
typedef struct _MW_SUBMISSION {
    ULONG FenceId;
    ULONG FlipSlot1;            /* T24: scanout buffer + 1 to show when this submission is reached (0: none) */
    PHYSICAL_ADDRESS FlipPhys;  /* T25: the flipped primary's address, reported by the vsyncs after it */
    BOOLEAN FlipPhysValid;
    ULONG Count, Next;          /* commands, and the next one to start */
    MW_COMMAND Commands[MW_MAX_COMMANDS];
} MW_SUBMISSION;

struct _MW_ADAPTER {
    PDEVICE_OBJECT Pdo;
    DXGKRNL_INTERFACE Dxgk;
    ULONG64 Seen;               /* DDIs already traced (see MwRet) */
    ULONG64 Entered;            /* DDIs whose first entry is traced */
    ULONG CallOrder;

    /* Display: the firmware's framebuffer, as malidod. */
    DXGK_DISPLAY_INFORMATION Fb;
    PUCHAR FbVa;
    SIZE_T FbSize;
    /* S9 port (T13f): DECON's registers, mapped for reading only, so the guard thread can log what the panel
     * really scans (T13e: 257 frames with pixels copied into the framebuffer, the panel black). */
    volatile UCHAR *DeconVa, *DeconDmaVa;
    BOOLEAN SourceVisible, PathActive;

    /* GPU. MmuLock serialises the address allocator and the page tables. */
    MK_GPU Gpu;
    FAST_MUTEX MmuLock;
    BOOLEAN GpuUp;

    /* Submissions, shared with the ISR (DxgkCbSynchronizeExecution). */
    MW_SUBMISSION Queue[MW_QUEUE_DEPTH];
    ULONG QHead;                /* ring; QHead is the one running */
    volatile ULONG QCount;
    BOOLEAN Running;            /* a job chain of Queue[QHead] is on the GPU */
    BOOLEAN NeedDpc;            /* a fence completed: queue the DPC */
    /* T17: MwLog writes without ZwFlushKey while set (the guard thread's batches, flushed once per LiveFlushSec) */
    volatile BOOLEAN LogBatch;
    ULONG RunningSlot;
    ULONG64 JobStart;           /* interrupt time the running chain started */
    ULONG LastSubmittedFence, LastCompletedFence;
    ULONG JobsDone, JobsFailed, JobsTimedOut, ChainFlag;
    ULONG LastJsStatus, LastFaultStatus;
    ULONG64 LastFaultAddress;

    /* Polls the job status while work is in flight (command.c). */
    KTIMER PollTimer;
    KDPC PollDpc;
    volatile LONG PollArmed;
    volatile BOOLEAN Stopping;

    /* VSync, emulated (command.c): the scanout is the firmware framebuffer,
     * which raises no interrupt, but DWM paces every frame on CRTC_VSYNC
     * and its flips complete only on one. ScanAlloc is the primary
     * SetVidPnSourceAddress last made current; ShownAlloc the one whose
     * pixels were last copied to the framebuffer. */
    KTIMER VsyncTimer;
    KDPC VsyncDpc;
    volatile LONG VsyncOn;
    PHYSICAL_ADDRESS ScanAddr;
    struct _MW_ALLOCATION *volatile ScanAlloc;
    struct _MW_ALLOCATION *ShownAlloc;
    ULONG Vsyncs, Flips, FlipCopies, FlipCopySkips;

    /* S9 port (docs/WIN11-GPU.md 6.8): the platform around the Mali, and the guard thread that watches the
     * temperature (ThermalHold: fences complete without running while it is set) and marks a boot
     * healthy once DWM has flipped through us for a while (FullGuardBoots back to 0). */
    S9_PLAT S9;
    volatile BOOLEAN ThermalHold;
    ULONG ThermalLevel, ThermalHolds;
    HANDLE GuardThread;
    PKTHREAD GuardThreadObj;
    KEVENT GuardStop;
    BOOLEAN Healthy;
    ULONG Interrupts;
    ULONG JcRejected;
    ULONG PrimaryBlts;
    ULONG PresentWaitTimeouts;
    /* T23: where frame time goes (100 ns units, accumulated): GPU job run time, the present path's wait for the queue
     * to drain (SubmitCommand), the CPU copies of presents (DoPresent); the guard turns them into per-second rates */
    volatile LONG64 GpuBusy100ns, PresentWait100ns, PresentCpu100ns, PresentWaitMax100ns;
    ULONG RateGpuPermille, RatePresentWaitMs, RatePresentCpuMs, RatePresentWaitMaxMs, RatePresents10, RateJobs10;
    ULONG ContentFrames;                  /* frames copied to the panel whose sampled pixels were not all black */
    ULONG FbContentFrames;                /* T13f: the same test read back from the framebuffer after the copy */
    ULONG64 FbPixels;                     /* pixels written into the framebuffer */
    ULONG LastFbRectLT, LastFbRectRB;     /* the last blt's rectangle into it, x << 16 | y */
    ULONG PrimaryOwnCopies;               /* blts into a primary also written to its own pages */
    /* T13g: T13f wrote only ~714K pixels into the framebuffer in 202 copies (DWM's dirty rectangles) and never
     * a whole frame, so the panel stayed black around them. A real scanout shows the scanned primary's pages:
     * the refresh copies ScanAlloc to the framebuffer whenever work completed since the last refresh. */
    ULONG PresentFlipFlag, PresentBltFlag, PresentFillFlag, PresentOtherFlag;
    ULONG PrimaryBltsToScan, PrimaryBltsOther;
    ULONG Refreshes, RefreshSkipsBlack, RefreshSkipsNoScan, RefreshUsMax, ScanContent;
    ULONG PresentLogged;
    /* T13h: what the panel should show: the source of DWM's latest flip, the primary the latest blt to the
     * panel went into, or the one SetVidPnSourceAddress made current, whichever came last. ShowGen counts
     * changes of content (every flip, any change of allocation); the guard thread copies it whole to the
     * framebuffer at PASSIVE_LEVEL (T13g: one whole copy took up to 0.7 s, far too long for the vsync DPC). */
    struct _MW_ALLOCATION *volatile ShowAlloc;
    volatile LONG ShowGen;
    ULONG EmuFlips, EmuFlipsNotPrimary, RefreshUsLast, RefreshesPeriodic;
    ULONG RefPx[8], RefSrcTag;
    ULONG FlipContent, FlipBlack, FlipPxCentre, FlipSrcVa, FlipSrcPages;  /* T13q: sampled at the flip itself */
    ULONG ShowFromBltSrc, FlipsIgnoredBlack;                             /* T13s */
    /* T13z: full-screen primaries rendered straight into the framebuffer (Parameters\PrimaryInFb, default 1):
     * the framebuffer's page numbers, mapped at such a primary's GPU address instead of its own pages */
    PFN_NUMBER *FbPfn;
    /* T24 (docs/WIN11-GPU.md 6.31): real flips. Parameters\ScanFlip = 1: each full-screen primary renders into a
     * scanout buffer of its own (slot 0 = the firmware framebuffer, 1-2 contiguous below 4 GiB), and a flip points
     * DECON's IDMA channel 0 at it when the in-order queue reaches it (sbdisplay's sequence, proven on Linux). FbVa
     * always maps the buffer DECON scans, so every CPU path follows. A flip that never latches (500 ms) falls back
     * to T13z's single aliased framebuffer. */
    BOOLEAN ScanFlipOn;
    struct sbdc Dc;
    struct sbdc_io DcIo;
    volatile UCHAR *DcMmio[3];
    PUCHAR FbVaOrig;
    struct { ULONG64 Pa; PUCHAR Va; PFN_NUMBER *Pfn; MW_ALLOCATION *Owner; } Slot[3];
    ULONG ScanSlots;
    volatile LONG ScanCur;
    ULONG ScanFlips, ScanFlipsForced, ScanSlotShared, ScanFallback, ScanPendingTicks;
    ULONG FlipAddrPatched, FlipAddrShown;   /* T25 */
    ULONG FlipAddrPresent;                  /* T26: flip addresses taken at Present (VidMm patches only on moves) */
    /* T27: blt copies. [0] on one CPU, [1] split over MW_COPY_HELPERS + 1; El* only copies of >= 64k pixels */
    ULONG ParallelBlt;                      /* Parameters\ParallelBlt: 0 one CPU, 1 A/B then the faster, 2 always */
    ULONG ParDecided;                       /* 0 measuring, 1 one CPU won, 2 split won */
    HANDLE CopyThread[MW_COPY_HELPERS];
    PKTHREAD CopyThreadObj[MW_COPY_HELPERS];
    KEVENT CopyGo[MW_COPY_HELPERS];
    struct { struct _MW_ADAPTER *A; ULONG I; } CopyCtx[MW_COPY_HELPERS];
    volatile LONG CopyStop, CopyBusy, CopyHelpersUp;
    MW_COPY_JOB CopyJob;
    ULONG64 ElCopy100ns[2], ElPx[2];
    ULONG ElN[2], CopyHelperChunks, CopyBusySerial, CopyWaitLong;
    ULONG64 BltCopy100ns, BltPx, CpuViewNew100ns;
    ULONG BltMaxMs, BltArea64k, BltArea1M, BltAreaBig, BltBoundsFallback, BltMaxSubRects, CpuViewNew;
    ULONG BltProc[4], BltProcPx, BltBigProcs;
    ULONG ViewTestDone, ViewWcMBps, ViewCachedColdMBps, ViewCachedHotMBps;
    ULONG SubmitIrqlSeen, PresentMaxMs;
    BOOLEAN CachedBltReads;                 /* T26: Parameters\CachedBltReads (default 1); off after a mismatch */
    ULONG CachedBlts, CachedBltMismatch;
    /* T14a: the first failed job of the boot (JS 0x51 INSTR_INVALID_ENC, once per boot since T13b), for
     * matching against the umdlogs' "bo N va A-B" lines */
    ULONG64 RunningJc;
    ULONG Fail1Slot, Fail1JcLo, Fail1HeadLo, Fail1TailLo, Fail1Status, Fail1JobsDone, Fail1Ms, Fail1Submits;
    ULONG64 StartTime;
    ULONG FbLiveSecs, FbLiveLast;         /* T14a: seconds the panel showed content (>= 8 of 16 samples) */
    ULONG FbPages, PrimaryInFb, FbAliasMaps, RefreshSkipsAliased;
    /* T13t: the present blts into non-primary surfaces (apps' frames into the surfaces DWM samples) and the
     * full frame of the blt sources into primaries (DWM's back buffer) */
    ULONG NpBlts, NpSrcContent, NpDstContent, NpSrcTag, NpDstTag, NpRectLT, NpRectRB, NpRects, NpSrcPx, NpDstPx;
    ULONG PbSrcFullContent, PbSrcFullBlack, PbSrcPx;
    ULONG CoherentMaps, CpuMapFails;            /* presents copied before the queue drained (200 ms bound) */                    /* presents whose destination is the primary: pixels on the panel */                     /* renders refused: a chain outside the render's allocations */                     /* interrupt resources the device has (the stage-4 SSDT: the Mali's 3) */

    /* Counters for the trace. */
    ULONG Renders, Presents, Submits;
    ULONG PagingOps[16];
    ULONG Maps, MapFails, Unmaps, LastMapPages, Allocations;
    /* T15: allocation bytes alive now and at most, and failures (no GPU address; refused) */
    volatile LONG64 AllocBytes, AllocBytesPeak;
    volatile LONG AllocVaFails, AllocFails;
    NTSTATUS LastMapStatus;
    ULONG64 LastMapVa, LastAllocVa;
    ULONG Patches, PatchAllocations, PatchPatchLocations, PatchSegment0;
    ULONG PresentBlts, PresentFills, PresentSkips;
    ULONG64 PatchAddress0;
};

/* ---- trace: Seen_<ddi>, Fail_<ddi> in the device's registry key ---- */
void MwLog(MW_ADAPTER *A, PCWSTR Name, ULONG Value);
void MwLogFlush(MW_ADAPTER *A);
void MwCopyStart(MW_ADAPTER *A);  /* T27 */
void MwScanFallback(MW_ADAPTER *A, MW_ALLOCATION **Owners, ULONG N);
void MwScanFlipHw(MW_ADAPTER *A, ULONG Slot);
void MwTraceReset(MW_ADAPTER *A);
void MwLogHook(void *Owner, PCWSTR Name, ULONG Value);
NTSTATUS MwRet(MW_ADAPTER *A, PCWSTR Name, NTSTATUS St);
void MwEnter(MW_ADAPTER *A, PCWSTR Name);

/* display.c: VidPN and the POST framebuffer */
NTSTATUS MwDisplayStart(MW_ADAPTER *A);
void MwDisplayStop(MW_ADAPTER *A);
DXGKDDI_QUERY_CHILD_RELATIONS MwQueryChildRelations;
DXGKDDI_QUERY_CHILD_STATUS MwQueryChildStatus;
DXGKDDI_QUERY_DEVICE_DESCRIPTOR MwQueryDeviceDescriptor;
DXGKDDI_ISSUPPORTEDVIDPN MwIsSupportedVidPn;
DXGKDDI_RECOMMENDFUNCTIONALVIDPN MwRecommendFunctionalVidPn;
DXGKDDI_ENUMVIDPNCOFUNCMODALITY MwEnumVidPnCofuncModality;
DXGKDDI_SETVIDPNSOURCEVISIBILITY MwSetVidPnSourceVisibility;
DXGKDDI_COMMITVIDPN MwCommitVidPn;
DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH MwUpdateActiveVidPnPresentPath;
DXGKDDI_RECOMMENDMONITORMODES MwRecommendMonitorModes;
DXGKDDI_QUERYVIDPNHWCAPABILITY MwQueryVidPnHWCapability;
DXGKDDI_SETVIDPNSOURCEADDRESS MwSetVidPnSourceAddress;
DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP MwStopDeviceAndReleasePostDisplayOwnership;
DXGKDDI_SYSTEM_DISPLAY_ENABLE MwSystemDisplayEnable;
DXGKDDI_SYSTEM_DISPLAY_WRITE MwSystemDisplayWrite;

/* memory.c: allocations and the aperture segment */
DXGKDDI_CREATEALLOCATION MwCreateAllocation;
DXGKDDI_DESTROYALLOCATION MwDestroyAllocation;
DXGKDDI_DESCRIBEALLOCATION MwDescribeAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA MwGetStandardAllocationDriverData;
DXGKDDI_OPENALLOCATIONINFO MwOpenAllocation;
DXGKDDI_CLOSEALLOCATION MwCloseAllocation;
DXGKDDI_BUILDPAGINGBUFFER MwBuildPagingBuffer;
NTSTATUS MwQuerySegment(MW_ADAPTER *A, const DXGKARG_QUERYADAPTERINFO *Info);
NTSTATUS MwEscapeAllocInfo(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc, MW_ESCAPE_ALLOC_INFO_DATA *D);

/* command.c: devices, contexts, render, submission, fences */
DXGKDDI_CREATEDEVICE MwCreateDevice;
DXGKDDI_DESTROYDEVICE MwDestroyDevice;
DXGKDDI_CREATECONTEXT MwCreateContext;
DXGKDDI_DESTROYCONTEXT MwDestroyContext;
DXGKDDI_RENDER MwRender;
DXGKDDI_RENDERKM MwRenderKm;
DXGKDDI_PRESENT MwPresent;
DXGKDDI_PATCH MwPatch;
DXGKDDI_SUBMITCOMMAND MwSubmitCommand;
DXGKDDI_PREEMPTCOMMAND MwPreemptCommand;
DXGKDDI_QUERYCURRENTFENCE MwQueryCurrentFence;
DXGKDDI_RESETFROMTIMEOUT MwResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT MwRestartFromTimeout;
DXGKDDI_COLLECTDBGINFO MwCollectDbgInfo;
DXGKDDI_GETNODEMETADATA MwGetNodeMetadata;
DXGKDDI_RESETENGINE MwResetEngine;
DXGKDDI_QUERYENGINESTATUS MwQueryEngineStatus;
DXGKDDI_QUERYDEPENDENTENGINEGROUP MwQueryDependentEngineGroup;
DXGKDDI_CANCELCOMMAND MwCancelCommand;
DXGKDDI_CONTROLINTERRUPT MwControlInterrupt;
DXGKDDI_GETSCANLINE MwGetScanLine;
BOOLEAN MwJobInterrupt(MW_ADAPTER *A);    /* from the ISR, at DIRQL */
void MwCommandInit(MW_ADAPTER *A);
void MwCommandStop(MW_ADAPTER *A);
void MwRefreshScanout(MW_ADAPTER *A);
void MwShow(MW_ADAPTER *A, struct _MW_ALLOCATION *P, BOOLEAN NewContent);
ULONG MwFbSamples(MW_ADAPTER *A);
void MwArmPoll(MW_ADAPTER *A);

/* S9 port (guard.c) */
NTSTATUS S9Start(MW_ADAPTER *A);          /* watchdog alive (waits up to Parameters\ThermalWaitMs), domain on, rail up */
void S9GuardStart(MW_ADAPTER *A);         /* the guard thread: thermal hold, healthy mark */
void S9GuardStop(MW_ADAPTER *A);
NTSTATUS S9DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PDRIVER_INITIALIZATION_DATA Full);
