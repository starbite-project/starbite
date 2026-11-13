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
    ULONG Creator[2];           /* T31: the image name (8 chars) of the process that created it */
    /* T46 (fault forensics): on the adapter's LiveAllocs list from creation to destruction; the list is changed only
     * under the interrupt lock, so Reap (the ISR or SyncPoll) can walk it. The rest is written racily (diagnostics). */
    LIST_ENTRY Live;
    BOOLEAN InLive;
    volatile LONG LastListed;   /* the highest render sequence (MW_ADAPTER.RenderSeq) whose allocation list had it */
    ULONG CreatedMs, MapCount, UnmapCount, LastMapMs, LastUnmapMs;   /* BuildPagingBuffer's map/unmap of it */
    /* KMD-scratch: evicted with its GPU pages pointed at the read-only scratch page (Parameters\ScratchUnmap), not
     * PTE 0. Changed under MmuLock; destruction clears those PTEs to 0. */
    BOOLEAN Scratched;
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
/* T31: kinds of dxgkrnl's standard allocations (KMD-side flags, set in GetStandardAllocationDriverData) */
#define MW_ALLOC_STD_SHADOW     0x100   /* CDD's GDI shadow: written by the CPU only */
#define MW_ALLOC_STD_STAGING    0x200   /* CDD's staging surface: CPU only */
#define MW_ALLOC_STD_PRIMARY    0x400   /* the shared primary */
#define MW_MAX_RECTS            128     /* T29: 16 -> 128. GDI's (CDD's) blts carry up to 123 dirty rectangles; above
                                         * 16, T27/T28 copied their bounding box (up to 1440x1761 px a blt) */
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
    /* 6.57: the rotation the copy applies (SBDC_ROT_90..270; anything else = none). Blts: the path's rotation when
     * DXGKARG_PRESENT.Flags.Rotate was set (the source then holds the image in the desktop's orientation and the
     * rectangles are in its coordinates). Flips: the path's rotation only under Parameters\RotateFlips = 1. */
    ULONG Rot;
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
    BOOLEAN Neon;               /* T31: rows copied by CopyRowNeon */
    /* 6.57: a rotated blt: chunks are MW_COPY_CHUNK_ROWS presented lines of XR (sbdc_xf_blit_lines), not rows of R */
    BOOLEAN Rotated;
    struct sbdc_xf Xf;
    struct sbdc_rect XR[MW_MAX_RECTS];
    unsigned XStart[MW_MAX_RECTS + 1];
    volatile LONG Next, Done;
} MW_COPY_JOB;

typedef struct _MW_DMA_PRIVATE {
    ULONG Count;
    ULONG Reserved;
    MW_COMMAND Commands[MW_MAX_COMMANDS];
    MW_PRESENT_OP Present;
    /* T44: the allocation (from this render's list) each chain starts in. VidMm keeps every listed allocation
     * resident and alive until the submission's fence completes, so the hang forensics may look at them. */
    struct _MW_ALLOCATION *JcAlloc[MW_MAX_COMMANDS];
    LONG RenderSeq;             /* T46: this render's sequence number (stamped on every allocation it lists) */
} MW_DMA_PRIVATE;

/* A submission the hardware works through, one job chain at a time. */
typedef struct _MW_SUBMISSION {
    ULONG FenceId;
    ULONG FlipSlot1;            /* T24: scanout buffer + 1 to show when this submission is reached (0: none) */
    PHYSICAL_ADDRESS FlipPhys;  /* T25: the flipped primary's address, reported by the vsyncs after it */
    BOOLEAN FlipPhysValid;
    ULONG Count, Next;          /* commands, and the next one to start */
    MW_COMMAND Commands[MW_MAX_COMMANDS];
    struct _MW_ALLOCATION *JcAlloc[MW_MAX_COMMANDS];   /* T44: see MW_DMA_PRIVATE */
    LONG RenderSeq;             /* T46: MW_DMA_PRIVATE.RenderSeq */
} MW_SUBMISSION;

/* T39/T46: one failed job (MMU fault, job status error or timeout), recorded by Reap at interrupt level. */
#define MW_FAULT_NONE           0   /* Where: not an MMU fault */
#define MW_FAULT_IN_LIVE        1   /* the fault address is inside a live allocation (L*) */
#define MW_FAULT_IN_NONE        2   /* inside no live allocation (Below*, Above*: the nearest ones) */
#define MW_FDESC_PENDING        0   /* DescState: the DPC reads the job header at JS_HEAD */
#define MW_FDESC_OK             1   /* D0/D4/D5 = Job Header words 0, 4, 5 at JS_HEAD */
#define MW_FDESC_NOVIEW         2   /* no CPU view (the page in use by the hang view, not RAM, or no reserved page) */
#define MW_FDESC_NOALLOC        3   /* JS_HEAD in no live, mapped, write-combined allocation, or not mapped by the GPU */
#define MW_FDESC_CHANGED        4   /* the GPU's page at JS_HEAD changed while it was read */
#define MW_FDESC_ALIGN          5   /* JS_HEAD not 64-byte aligned */
#define MW_PTE_NONE             0   /* PTE 0 (or no L3 table) */
#define MW_PTE_PAGE             1   /* a real page */
#define MW_PTE_SCRATCH          2   /* KMD-scratch: the read-only scratch page (evicted); logged as "pte S" */
typedef struct _MW_FAULT {
    ULONG64 Va, FreedVa;
    ULONG Status, Js, Ms, AgoMs, FreedPages, Creator[2], Kind, Submits;
    /* T46 */
    volatile LONG Seq;          /* fault number + 1, stored last by Reap */
    ULONG Slot, Where, DescState, D0, D4, D5;
    ULONG64 Head;               /* the faulting slot's JS_HEAD */
    LONG RunSeq;                /* the running submission's render sequence */
    /* Where == MW_FAULT_IN_LIVE: the allocation and what the KMD knows of its state at the fault */
    ULONG64 LVa;
    ULONG LPages, LCreator[2], LCreatedMs, LMaps, LUnmaps, LMapMs, LUnmapMs;
    LONG LListed;               /* its LastListed */
    BOOLEAN LMapped;            /* resident per BuildPagingBuffer */
    UCHAR PteNow;               /* the GPU page table at the fault page now: MW_PTE_* */
    /* Where == MW_FAULT_IN_NONE */
    ULONG64 BelowVa, AboveVa;
    ULONG BelowPages, AbovePages, LiveCount;
} MW_FAULT;

/* T44 (hang forensics): one GPU hang, i.e. a chain past MW_JOB_TIMEOUT_MS or its submission past
 * MW_SUB_TIMEOUT_MS, captured by Reap at the device's interrupt level just before the hard stop. Diagnostics only. */
#define MW_HANGS                8
#define MW_HANG_DESC_DWORDS     16
#define MW_HANG_DESC_OK         0   /* Desc[] = the 16 dwords at JS_HEAD */
#define MW_HANG_DESC_NOVIEW     1   /* no CPU view of JS_HEAD's page was prepared (see MW_HANG_VIEW) */
#define MW_HANG_DESC_UNKNOWN    2   /* JS_HEAD is in no allocation a chain of this submission starts in */
#define MW_HANG_DESC_MISMATCH   3   /* the view is of another page, or the GPU's page is not that allocation's */
#define MW_HANG_DESC_ALIGN      4   /* JS_HEAD not 64-byte aligned (a job header always is) */
typedef struct _MW_HANG {
    volatile LONG Seq;          /* record number + 1, stored last; 0 while it is being written */
    ULONG Ms, ChainMs, SubMs;   /* when (ms since start), and how long the chain and the submission had run */
    ULONG Slot, Fence, Cmd, Cmds, Submits;
    ULONG64 Jc, Head, Tail, Affinity;
    ULONG JsStatus, JsConfig, GpuStatus, GpuFaultStatus;
    ULONG64 GpuFaultAddress;
    ULONG AsFaultStatus, MmuRawStat;
    ULONG64 AsFaultAddress;
    ULONG DescState, AllocPages;
    ULONG64 AllocVa;            /* the known allocation JS_HEAD is in (0: none) */
    ULONG Creator[2];
    ULONG Desc[MW_HANG_DESC_DWORDS];
} MW_HANG;

/* T44: a CPU view of JS_HEAD's page, made by PollDpc at DISPATCH_LEVEL (mapping is not allowed at DIRQL) when the
 * running chain is about to time out, and handed to SyncPoll. Reap reads through it only after checking, under the
 * interrupt lock, that the page is the one the GPU maps there and belongs to an allocation of the hung submission. */
typedef struct _MW_HANG_VIEW {
    const volatile UCHAR *Va;   /* the page's first byte; NULL: no view */
    ULONG64 Page;               /* its GPU address */
    PFN_NUMBER Pfn;
    ULONG64 JobStart;           /* the chain it was made for (MW_ADAPTER.JobStart) */
} MW_HANG_VIEW;

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
    /* T57: cached CPU-read allocations and MW_ESCAPE_SB_CACHE */
    LONG CachedAllocs, CacheOps, CacheOpsNotCached, CacheOpFails;
    LONG64 CacheOpBytes, CacheOp100ns;
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
    /* T31: blt sources/destinations by kind [0] shadow [1] staging [2] shared primary [3] other (UMD); the creators
     * of the last big blt's source and destination; blts copied without draining the queue; the NEON copy A/B */
    ULONG BltSrcKind[4], BltDstKind[4], BltSrcCreator[2], BltDstCreator[2];
    ULONG BltNoDrain, BltNoDrainCount, NeonCopy, NeonDecided;
    ULONG64 NeCopy100ns[2], NePx[2];
    ULONG NeN[2];
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
    /* T39: GPU fault diagnosis. Every freed GPU range (newest 256) and every failed job (newest 16) with its fault
     * address checked against them: an address in a range freed moments before = a buffer freed while the GPU
     * still used it. Read racily from the ISR (diagnostics only). Live_FaultLog (REG_SZ) has the text. */
    struct { ULONG64 Va; ULONG Pages; ULONG Creator[2]; ULONG Ms; } Freed[256];
    volatile LONG FreedNext;
    MW_FAULT Faults[16];
    volatile LONG FaultCount;
    LONG FaultLogged;
    /* T41: GPU hang recovery */
    ULONG64 SubStart;                       /* when the head submission's first chain started */
    ULONG TdrEngineResets, TdrAdapterResets, TdrQueriedHung;
    /* T44: hang forensics. Hangs is written by Reap (interrupt level) and read racily by the guard thread, which
     * keeps a record only if its Seq did not change while it copied it. Live_HangLog (REG_SZ), Live_HangCount. */
    MW_HANG Hangs[MW_HANGS];
    volatile LONG HangCount;
    LONG HangLogged;
    PVOID HangMapVa;                        /* one page of system address space reserved at AddDevice; NULL: no dumps */
    struct { MDL Mdl; PFN_NUMBER Pfn[1]; } HangMdl;   /* the one-page MDL mapped there (PollDpc only) */
    volatile LONG HangViewBusy;
    ULONG HangViews, HangViewFails;
    ULONG RamRuns;                          /* the PFN ranges that are RAM (MmGetPhysicalMemoryRanges at AddDevice) */
    struct { ULONG64 First, End; } RamRun[32];
    /* T46: fault forensics. Every live allocation (changed under the interrupt lock only), the render sequence, the
     * fault counts by where the address fell, and the job header read the DPC owes the newest fault record. */
    LIST_ENTRY LiveAllocs;
    ULONG LiveCount;
    volatile LONG RenderSeq;
    ULONG FaultsInLive, FaultsInNone, FaultDescReads;
    /* KMD-scratch: Parameters\ScratchUnmap (default 1) and a scratch page exist: an eviction points the allocation's
     * PTEs at the scratch page read-only instead of 0. ScratchMaps = evictions done so, ScratchPtes = PTEs written. */
    BOOLEAN ScratchUnmap;
    ULONG ScratchMaps, ScratchPtes;
    struct { volatile LONG Seq; ULONG64 Head; PFN_NUMBER Pfn; } FaultPend;
    LONG FaultPendDone;
    /* 6.57: display rotation (Settings > Display orientation). Parameters\Rotation (default 1; 0 = identity only, as
     * before) offers 90/180/270 on the path; source modes stay in the panel's orientation. Rot = the path's rotation
     * (SBDC_ROT_*, from CommitVidPn / UpdateActiveVidPnPresentPath). Blt presents with Flags.Rotate are rotated in the
     * CPU copy; flips are shown as they are (DWM renders them pre-rotated), unless Parameters\RotateFlips = 1:
     * then flips while rotated go through the emulated scanout, which copies them rotated where they fit (ShowRot). */
    ULONG RotAllowed, RotateFlips;
    volatile LONG Rot, ShowRot;
    ULONG RotatedBlts, RotatedFlips, RotBltWide, FlipRotateFlag, RotPathAsked, RotFlipCpu, RotFlipNoFit, RotFlipAliased;
    ULONG64 RotBlt100ns;
};
#define MW_ROTATED(r) ((ULONG)(r) == SBDC_ROT_90 || (ULONG)(r) == SBDC_ROT_180 || (ULONG)(r) == SBDC_ROT_270)

/* ---- trace: Seen_<ddi>, Fail_<ddi> in the device's registry key ---- */
void MwLog(MW_ADAPTER *A, PCWSTR Name, ULONG Value);
void MwLogSz(MW_ADAPTER *A, PCWSTR Name, const char *Text);   /* T39 */
void MwFaultLog(MW_ADAPTER *A);                                /* T39: Live_FaultLog when new faults came */
void MwHangLog(MW_ADAPTER *A);                                 /* T44: Live_HangLog when new hangs came */
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
NTSTATUS MwEscapeCache(MW_ADAPTER *A, const DXGKARG_ESCAPE *Esc, MW_ESCAPE_SB_CACHE_DATA *D);   /* T57 */

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
void MwCommandFini(MW_ADAPTER *A);        /* T44: RemoveDevice (PASSIVE_LEVEL) */
void MwLiveLink(MW_ADAPTER *A, struct _MW_ALLOCATION *Alloc, BOOLEAN Add);   /* T46: PASSIVE_LEVEL */
void MwFaultDescDpc(MW_ADAPTER *A);       /* T46: from the DPC routine, DISPATCH_LEVEL */
void MwRefreshScanout(MW_ADAPTER *A);
void MwShow(MW_ADAPTER *A, struct _MW_ALLOCATION *P, BOOLEAN NewContent);
ULONG MwFbSamples(MW_ADAPTER *A);
void MwArmPoll(MW_ADAPTER *A);

/* S9 port (guard.c) */
NTSTATUS S9Start(MW_ADAPTER *A);          /* watchdog alive (waits up to Parameters\ThermalWaitMs), domain on, rail up */
void S9GuardStart(MW_ADAPTER *A);         /* the guard thread: thermal hold, healthy mark */
void S9GuardStop(MW_ADAPTER *A);
NTSTATUS S9DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PDRIVER_INITIALIZATION_DATA Full);
