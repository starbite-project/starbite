/* SPDX-License-Identifier: GPL-2.0-only
 * sbwddm's guards (docs/WIN11-GPU.md 6.8). One binary, two modes:
 *   full          the WDDM 1.3 render + display driver (maliwddm, S9 port): DWM on the Mali through Mesa;
 *   display-only  sbdisplay, unchanged (its code is built into this image): today's desktop.
 * DriverEntry picks full mode only when Parameters\FullMode = 1, no kill file
 * (\SystemRoot\Starbite\sbwddm.full.off) and State\FullGuardBoots < 1. A full-mode boot is counted at once
 * and cleared only when it is healthy: 60 s after start, with DWM's flips and GPU jobs completing (and since
 * T13f only with Parameters\GuardAutoClear = 1: during bring-up every full-mode boot is followed by a
 * display-only one until FullGuardBoots is reset by hand). So any
 * full-mode boot that does not get there (a black desktop and a forced or dead-man restart, a bugcheck, a
 * failed start) is followed by a display-only boot. There is no WARP fallback for a failing UMD (6.3), so
 * this guard is what brings the desktop back.
 *
 * Crash bounds without touching the phone: a full-mode boot that dies -> the next boot is display-only; a
 * display-only boot that dies twice (sbdisplay's own GuardBoots, limit 2) -> the third boot refuses -> Basic
 * Display. Kill files (C:\Windows\Starbite\, from TWRP): sbwddm.off = the whole driver off (checked first);
 * sbwddm.full.off = full mode off; sbdisplay.off = display-only mode off (sbdisplay's own, as before).
 */
#include "maliwddm/maliwddm.h"
#include "../sbdisplay/sbdc_core.h"     /* DECON register offsets (read only) */

#define SBW_FULL_GUARD_LIMIT 1U
#define SBW_HEALTHY_MS 60000U
#define SBW_HEALTHY_FLIPS 30U             /* DWM frames to the panel (flips + primary blts); with FbLiveSecs >= 30 */
#define SBW_HEALTHY_JOBS 30U
#define SBW_LIVE_MAX 192U   /* T27: 128 -> 192 (the guard thread keeps one stack array of these) */

NTSTATUS SdDriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);   /* sbdisplay (sd_embed.c) */

static void State(PCWSTR Name, ULONG Value)
{
    S9PutDword(L"State", Name, Value);
}

NTSTATUS S9Start(MW_ADAPTER *A)
{
    ULONG level = SBTH_CRITICAL, gate = 1, waited = 0, maxwait;
    int r;
    LARGE_INTEGER t;

    /* sbthermal must be running before the GPU is touched. As a system-start service it is up early; give
     * it a little time, then refuse (Windows falls back to Basic Display, the next boot to sbdisplay). */
    maxwait = S9GetDword(L"Parameters", L"ThermalWaitMs", 20000);
    for (;;) {
        gate = S9Gate(&A->S9, &level);
        if (!gate || waited >= maxwait)
            break;
        t.QuadPart = -10000LL * 250;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        waited += 250;
    }
    MwLog(A, L"S9ThermalGate", gate);
    MwLog(A, L"S9ThermalWaitMs", waited);
    MwLog(A, L"S9ThermalLevel", level);
    if (gate || level >= SBTH_STOP)
        return STATUS_DEVICE_NOT_READY;
    if (!S9DomainOn(&A->S9)) {
        MwLog(A, L"S9DomainOff", 1);
        return STATUS_DEVICE_NOT_READY;
    }
    r = S9RailUp(&A->S9);
    MwLog(A, L"S9BuckOld", A->S9.BuckOld);
    MwLog(A, L"S9BuckRaise", (ULONG)r);
    return r == 0 ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

static KSTART_ROUTINE GuardThread;

static ULONG Rd(volatile UCHAR *Base, ULONG Off)
{
    return Base != NULL ? READ_REGISTER_ULONG((volatile ULONG *)(Base + Off)) : 0xFFFFFFFFu;
}

/* T13c: the driver's live counters, logged (Live_<name>) whenever one changed, once a second. T13b's trace
 * ended at DWM's first Render; what came after (submits, fences, GPU jobs and faults, flips, vsyncs) never
 * reached the registry. */
static void LiveCounters(MW_ADAPTER *a, ULONG *Prev)
{
    const struct { PCWSTR n; ULONG v; } c[] = {
        { L"Live_Renders", a->Renders }, { L"Live_Presents", a->Presents }, { L"Live_Submits", a->Submits },
        { L"Live_FenceSubmitted", a->LastSubmittedFence }, { L"Live_FenceCompleted", a->LastCompletedFence },
        { L"Live_Queued", a->QCount }, { L"Live_JobsDone", a->JobsDone }, { L"Live_JobsFailed", a->JobsFailed },
        { L"Live_JobsTimedOut", a->JobsTimedOut }, { L"Live_LastJsStatus", a->LastJsStatus },
        { L"Live_LastFaultStatus", a->LastFaultStatus }, { L"Live_LastFaultAddrLo", (ULONG)a->LastFaultAddress },
        { L"Live_LastFaultAddrHi", (ULONG)(a->LastFaultAddress >> 32) }, { L"Live_LastMmuFault", a->Gpu.LastMmuFault },
        { L"Live_Irqs", a->Gpu.Irqs }, { L"Live_GpuResets", a->Gpu.Resets }, { L"Live_GpuUp", a->GpuUp },
        { L"Live_Flips", a->Flips }, { L"Live_FlipCopies", a->FlipCopies }, { L"Live_FlipCopySkips", a->FlipCopySkips },
        { L"Live_Vsyncs", a->Vsyncs }, { L"Live_PresentBlts", a->PresentBlts }, { L"Live_PresentFills", a->PresentFills },
        { L"Live_PresentSkips", a->PresentSkips }, { L"Live_Maps", a->Maps }, { L"Live_MapFails", a->MapFails },
        { L"Live_Unmaps", a->Unmaps }, { L"Live_Allocations", a->Allocations }, { L"Live_Patches", a->Patches },
        { L"Live_JcRejected", a->JcRejected }, { L"Live_ThermalHolds", a->ThermalHolds },
        { L"Live_PrimaryBlts", a->PrimaryBlts }, { L"Live_PresentWaitTimeouts", a->PresentWaitTimeouts },
        { L"Live_ContentFrames", a->ContentFrames }, { L"Live_CoherentMaps", a->CoherentMaps },
        { L"Live_CpuMapFails", a->CpuMapFails },
        { L"Live_FbContentFrames", a->FbContentFrames }, { L"Live_FbKPixels", (ULONG)(a->FbPixels >> 10) },
        { L"Live_LastFbRectLT", a->LastFbRectLT }, { L"Live_LastFbRectRB", a->LastFbRectRB },
        { L"Live_PrimaryOwnCopies", a->PrimaryOwnCopies },
        { L"Live_PresentFlipFlag", a->PresentFlipFlag }, { L"Live_PresentBltFlag", a->PresentBltFlag },
        { L"Live_PresentFillFlag", a->PresentFillFlag }, { L"Live_PresentOtherFlag", a->PresentOtherFlag },
        { L"Live_PrimaryBltsToScan", a->PrimaryBltsToScan }, { L"Live_PrimaryBltsOther", a->PrimaryBltsOther },
        { L"Live_Refreshes", a->Refreshes }, { L"Live_RefreshSkipsBlack", a->RefreshSkipsBlack },
        { L"Live_RefreshSkipsNoScan", a->RefreshSkipsNoScan }, { L"Live_RefreshUsMax", a->RefreshUsMax },
        { L"Live_ScanContent", a->ScanContent }, { L"Live_EmuFlips", a->EmuFlips },
        { L"Live_EmuFlipsNotPrimary", a->EmuFlipsNotPrimary }, { L"Live_RefreshUsLast", a->RefreshUsLast },
        { L"Live_RefreshesPeriodic", a->RefreshesPeriodic }, { L"Live_RefSrcTag", a->RefSrcTag },
        { L"Live_RefPx0", a->RefPx[0] }, { L"Live_RefPx1", a->RefPx[1] }, { L"Live_RefPx2", a->RefPx[2] },
        { L"Live_RefPx3", a->RefPx[3] }, { L"Live_RefPx4", a->RefPx[4] }, { L"Live_RefPx5", a->RefPx[5] },
        { L"Live_RefPx6", a->RefPx[6] }, { L"Live_RefPx7", a->RefPx[7] },
        { L"Live_FlipContent", a->FlipContent }, { L"Live_FlipBlack", a->FlipBlack },
        { L"Live_FlipPxCentre", a->FlipPxCentre }, { L"Live_FlipSrcVa", a->FlipSrcVa },
        { L"Live_FlipSrcPages", a->FlipSrcPages }, { L"Live_ShowFromBltSrc", a->ShowFromBltSrc },
        { L"Live_FlipsIgnoredBlack", a->FlipsIgnoredBlack }, { L"Live_FbAliasMaps", a->FbAliasMaps },
        { L"Live_RefreshSkipsAliased", a->RefreshSkipsAliased },
        { L"Live_Fail1Slot", a->Fail1Slot }, { L"Live_Fail1JcLo", a->Fail1JcLo }, { L"Live_Fail1HeadLo", a->Fail1HeadLo },
        { L"Live_Fail1TailLo", a->Fail1TailLo }, { L"Live_Fail1Status", a->Fail1Status },
        { L"Live_Fail1JobsDone", a->Fail1JobsDone }, { L"Live_Fail1Submits", a->Fail1Submits },
        { L"Live_Fail1Ms", a->Fail1Ms },
        { L"Live_FbLiveSecs", a->FbLiveSecs }, { L"Live_FbLiveLast", a->FbLiveLast },
        { L"Live_NpBlts", a->NpBlts }, { L"Live_NpSrcContent", a->NpSrcContent }, { L"Live_NpDstContent", a->NpDstContent },
        { L"Live_NpSrcTag", a->NpSrcTag }, { L"Live_NpDstTag", a->NpDstTag }, { L"Live_NpRectLT", a->NpRectLT },
        { L"Live_NpRectRB", a->NpRectRB }, { L"Live_NpRects", a->NpRects }, { L"Live_NpSrcPx", a->NpSrcPx },
        { L"Live_NpDstPx", a->NpDstPx }, { L"Live_PbSrcFullContent", a->PbSrcFullContent },
        { L"Live_PbSrcFullBlack", a->PbSrcFullBlack }, { L"Live_PbSrcPx", a->PbSrcPx },
        /* T13f: what DECON scans (read only; sbdisplay's registers): IDMA channel 0's base address, live and
         * shadow, its image size, the trigger control, the window enables and a pending shadow update */
        { L"Live_DeconBaseY", Rd(a->DeconDmaVa, SBDC_CH0 + SBDC_IDMA_BASE_Y) },
        { L"Live_DeconShadowBaseY", Rd(a->DeconDmaVa, SBDC_CH0 + SBDC_IDMA_SHADOW + SBDC_IDMA_BASE_Y) },
        { L"Live_DeconImgSize", Rd(a->DeconDmaVa, SBDC_CH0 + SBDC_IDMA_IMG_SIZE) },
        { L"Live_DeconTrig", Rd(a->DeconVa, SBDC_TRIG) }, { L"Live_DeconPath0", Rd(a->DeconVa, SBDC_PATH0) },
        { L"Live_DeconShadowReq", Rd(a->DeconVa, SBDC_SHADOW_REQ) },
        { L"Live_ThermalLevel", a->ThermalLevel },
        /* T15: GPU memory (MB alive now, the most so far) and allocations that failed */
        { L"Live_AllocMB", (ULONG)(a->AllocBytes >> 20) }, { L"Live_AllocMBPeak", (ULONG)(a->AllocBytesPeak >> 20) },
        { L"Live_AllocVaFails", (ULONG)a->AllocVaFails }, { L"Live_AllocFails", (ULONG)a->AllocFails },
        /* T23: rates over the last logging interval: GPU busy (per mille), the present path's queue wait and CPU copy
         * (ms per second; the longest single wait), presents and GPU jobs per 10 s */
        { L"Live_RateGpuPermille", a->RateGpuPermille }, { L"Live_RatePresentWaitMs", a->RatePresentWaitMs },
        { L"Live_RatePresentCpuMs", a->RatePresentCpuMs }, { L"Live_RatePresentWaitMaxMs", a->RatePresentWaitMaxMs },
        { L"Live_RatePresents10", a->RatePresents10 }, { L"Live_RateJobs10", a->RateJobs10 },
        /* T24: real flips: done, done over one not latched yet, primaries sharing the framebuffer, the fallback */
        { L"Live_ScanFlips", a->ScanFlips }, { L"Live_ScanFlipsForced", a->ScanFlipsForced },
        { L"Live_ScanSlotShared", a->ScanSlotShared }, { L"Live_ScanFallback", a->ScanFallback },
        { L"Live_ScanCur", (ULONG)a->ScanCur },
        /* T25: flips whose source address Patch saw, and that the vsync then reported */
        { L"Live_FlipAddrPatched", a->FlipAddrPatched }, { L"Live_FlipAddrShown", a->FlipAddrShown },
        { L"Live_ScanAddrLo", a->ScanAddr.LowPart },
        /* T26: flip addresses taken at Present; blt presents read through the cached view; mismatches (then off) */
        { L"Live_FlipAddrPresent", a->FlipAddrPresent }, { L"Live_CachedBlts", a->CachedBlts },
        { L"Live_CachedBltMismatch", a->CachedBltMismatch },
        /* T27: blt copies: time and pixels (all; big ones on one CPU [0] and split [1]), sizes, the A/B verdict,
         * the view read test, new CPU views, the presenter of the last big blt, submit IRQLs */
        { L"Live_BltCopyMs", (ULONG)(a->BltCopy100ns / 10000) }, { L"Live_BltMPx", (ULONG)(a->BltPx >> 20) },
        { L"Live_BltArea64k", a->BltArea64k }, { L"Live_BltArea1M", a->BltArea1M }, { L"Live_BltAreaBig", a->BltAreaBig },
        { L"Live_El0N", a->ElN[0] }, { L"Live_El0Ms", (ULONG)(a->ElCopy100ns[0] / 10000) },
        { L"Live_El0MPx", (ULONG)(a->ElPx[0] >> 20) }, { L"Live_El1N", a->ElN[1] },
        { L"Live_El1Ms", (ULONG)(a->ElCopy100ns[1] / 10000) }, { L"Live_El1MPx", (ULONG)(a->ElPx[1] >> 20) },
        { L"Live_ParDecided", a->ParDecided }, { L"Live_CopyHelperChunks", a->CopyHelperChunks },
        { L"Live_CopyBusySerial", a->CopyBusySerial }, { L"Live_CopyWaitLong", a->CopyWaitLong },
        { L"Live_ViewTestDone", a->ViewTestDone }, { L"Live_ViewWcMBps", a->ViewWcMBps },
        { L"Live_ViewCachedColdMBps", a->ViewCachedColdMBps }, { L"Live_ViewCachedHotMBps", a->ViewCachedHotMBps },
        { L"Live_CpuViewNew", a->CpuViewNew }, { L"Live_CpuViewNewMs", (ULONG)(a->CpuViewNew100ns / 10000) },
        { L"Live_BltBoundsFallback", a->BltBoundsFallback }, { L"Live_BltMaxSubRects", a->BltMaxSubRects },
        { L"Live_BltBigProcs", a->BltBigProcs }, { L"Live_BltProcPx", a->BltProcPx },
        { L"Live_BltProc0", a->BltProc[0] }, { L"Live_BltProc1", a->BltProc[1] }, { L"Live_BltProc2", a->BltProc[2] },
        { L"Live_SubmitIrqlSeen", a->SubmitIrqlSeen }, { L"Live_PresentMaxMs", a->PresentMaxMs },
        /* T31 */
        { L"Live_BltSrcShadow", a->BltSrcKind[0] }, { L"Live_BltSrcStaging", a->BltSrcKind[1] },
        { L"Live_BltSrcPrimary", a->BltSrcKind[2] }, { L"Live_BltSrcOther", a->BltSrcKind[3] },
        { L"Live_BltDstShadow", a->BltDstKind[0] }, { L"Live_BltDstStaging", a->BltDstKind[1] },
        { L"Live_BltDstPrimary", a->BltDstKind[2] }, { L"Live_BltDstOther", a->BltDstKind[3] },
        { L"Live_BltSrcCreator0", a->BltSrcCreator[0] }, { L"Live_BltSrcCreator1", a->BltSrcCreator[1] },
        { L"Live_BltDstCreator0", a->BltDstCreator[0] }, { L"Live_BltDstCreator1", a->BltDstCreator[1] },
        { L"Live_BltNoDrainCount", a->BltNoDrainCount }, { L"Live_NeonDecided", a->NeonDecided },
        { L"Live_Ne0N", a->NeN[0] }, { L"Live_Ne0Ms", (ULONG)(a->NeCopy100ns[0] / 10000) },
        { L"Live_Ne0MPx", (ULONG)(a->NePx[0] >> 20) }, { L"Live_Ne1N", a->NeN[1] },
        { L"Live_Ne1Ms", (ULONG)(a->NeCopy100ns[1] / 10000) }, { L"Live_Ne1MPx", (ULONG)(a->NePx[1] >> 20) },
        /* T41: Windows' TDR calls (per engine, whole adapter) and status queries that found a hang */
        { L"Live_TdrEngineResets", a->TdrEngineResets }, { L"Live_TdrAdapterResets", a->TdrAdapterResets },
        { L"Live_TdrQueriedHung", a->TdrQueriedHung },
    };
    static_assert(sizeof(c) / sizeof(c[0]) < SBW_LIVE_MAX, "Live_ table too long");
    for (ULONG i = 0; i < sizeof(c) / sizeof(c[0]); i++)
        if (c[i].v != Prev[i] || Prev[SBW_LIVE_MAX] == 0) {
            Prev[i] = c[i].v;
            MwLog(a, c[i].n, c[i].v);
        }
    Prev[SBW_LIVE_MAX] = 1;
}

static VOID GuardThread(PVOID Ctx)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Ctx;
    ULONG64 t0 = KeQueryInterruptTime();
    LARGE_INTEGER t;
    BOOLEAN hold = FALSE;
    ULONG prev[SBW_LIVE_MAX + 1] = { 0 }, secs = 0;
    /* T13f: during bring-up a full-mode boot never clears its own guard (Parameters\GuardAutoClear = 0, the
     * default): T13d and T13e both passed the health test with a black panel. The person resets
     * FullGuardBoots from TWRP for each full-mode test; every other boot after one is display-only. */
    ULONG autoClear = S9GetDword(L"Parameters", L"GuardAutoClear", 0);
    ULONG testBand = S9GetDword(L"Parameters", L"TestBand", 0);     /* T13f: seen by the user; off now */
    /* T13g: the emulated scanout's period (MwRefreshScanout), 0 = off */
    ULONG refreshMs = S9GetDword(L"Parameters", L"RefreshMs", 200);
    /* T17: Live_ logging every LiveSec seconds, written to disk every LiveFlushSec (one hive flush per batch;
     * T13-T16 flushed after every single value). Test cycles: 1/1; the user's install: 10/60. */
    ULONG liveSec = S9GetDword(L"Parameters", L"LiveSec", 1), liveFlushSec = S9GetDword(L"Parameters", L"LiveFlushSec", 1);
    ULONG lastFlush = 0;
    ULONG scanFlips0 = 0, scanForced0 = 0;   /* T24: the latch check */
    LONG64 rateBusy = 0, rateWait = 0, rateCpu = 0;      /* T23 */
    ULONG ratePres = 0, rateJobs = 0, rateSec = 0;
    ULONG64 nextSec = KeQueryInterruptTime() + 10000000ULL, lastWork = (ULONG64)-1, lastPeriodic = 0;
    LONG lastGen = -1;
    if (refreshMs != 0 && refreshMs < 50)
        refreshMs = 50;

    for (;;) {
        ULONG level, gate;
        t.QuadPart = -10000LL * (refreshMs ? refreshMs : 1000);
        if (KeWaitForSingleObject(&a->GuardStop, Executive, KernelMode, FALSE, &t) != STATUS_TIMEOUT)
            break;
        if (a->ScanFlipOn) {
            /* T24: is DECON taking flips? A flip normally latches at the next TE (16.7 ms), so a sample may well
             * catch one pending. Stuck means: with flips coming, 90 % of them found the previous one still pending
             * (MwScanFlipHw counts those as forced), two ticks running; with no new flips, one pending for 3 ticks.
             * Then back to the aliased firmware framebuffer for the rest of the boot. */
            ULONG df = a->ScanFlips - scanFlips0, dforced = a->ScanFlipsForced - scanForced0;
            BOOLEAN pend = sbdc_flip_pending(&a->Dc) != 0;
            BOOLEAN bad = df >= 5 ? dforced * 10 >= df * 9 : (df == 0 && pend);
            scanFlips0 = a->ScanFlips;
            scanForced0 = a->ScanFlipsForced;
            a->ScanPendingTicks = bad ? a->ScanPendingTicks + 1 : 0;
            if (a->ScanPendingTicks >= (df >= 5 ? 2u : 3u)) {
                MW_ALLOCATION *own[3];
                ULONG n = 0;
                a->ScanFlipOn = FALSE;
                for (ULONG i = 0; i < a->ScanSlots && i < 3; i++)
                    if (a->Slot[i].Owner != NULL)
                        own[n++] = a->Slot[i].Owner;
                MwScanFallback(a, own, n);
                MwScanFlipHw(a, 0);
                a->ScanFallback++;
                MwLog(a, L"ScanFallbackAtFlip", a->ScanFlips);
            }
        }
        if (refreshMs) {
            /* T13h: a flip or another allocation shown: copy it now. Otherwise, when GPU work completed (it may
             * draw straight into the shown primary), at most every 2 s: one whole copy costs up to 0.7 s. */
            ULONG64 now = KeQueryInterruptTime(), work = (ULONG64)a->JobsDone << 32 | a->Presents;
            LONG gen = a->ShowGen;
            if (gen != lastGen) {
                lastGen = gen;
                lastWork = work;
                lastPeriodic = now;
                MwRefreshScanout(a);
            } else if (work != lastWork && now - lastPeriodic >= 100000000ULL) {
                /* T13y: the slow safety refresh, every 10 s at most (was 2 s): the dirty rectangles keep the
                 * panel current in between */
                lastWork = work;
                lastPeriodic = now;
                a->RefreshesPeriodic++;
                MwRefreshScanout(a);
            }
        }
        if (KeQueryInterruptTime() < nextSec)
            continue;
        nextSec += 10000000ULL;
        ++secs;
        BOOLEAN logNow = liveSec <= 1 || secs % liveSec == 0;
        a->LogBatch = TRUE;
        if (logNow) {
            /* T23: rates since the last logging tick */
            ULONG span = secs - rateSec ? secs - rateSec : 1;
            LONG64 busy = a->GpuBusy100ns, wait = a->PresentWait100ns, cpu = a->PresentCpu100ns;
            a->RateGpuPermille = (ULONG)((busy - rateBusy) / 10000 / span);           /* 100 ns -> 1/1000 of a second */
            a->RatePresentWaitMs = (ULONG)((wait - rateWait) / 10000 / span);
            a->RatePresentCpuMs = (ULONG)((cpu - rateCpu) / 10000 / span);
            a->RatePresentWaitMaxMs = (ULONG)(InterlockedExchange64(&a->PresentWaitMax100ns, 0) / 10000);
            a->RatePresents10 = (a->Presents - ratePres) * 10 / span;
            a->RateJobs10 = (a->JobsDone - rateJobs) * 10 / span;
            rateBusy = busy; rateWait = wait; rateCpu = cpu; ratePres = a->Presents; rateJobs = a->JobsDone; rateSec = secs;
            LiveCounters(a, prev);
            MwFaultLog(a);                    /* T39 */
            MwHangLog(a);                     /* T44 */
            MwLog(a, L"Live_Seconds", secs);
        }
        /* T13f: a visible probe of the framebuffer path, independent of DWM and the GPU: a magenta band over
         * the top 96 rows at 20 s and 40 s (Parameters\TestBand, default 1). Seen on the panel = the
         * framebuffer the driver writes is the one DECON scans. */
        if ((secs == 20 || secs == 40) && testBand && a->FbVa != NULL) {
            ULONG rows = min(96u, a->Fb.Height), ok = 0;
            for (ULONG y = 0; y < rows; y++) {
                ULONG *row = (ULONG *)(a->FbVa + (SIZE_T)y * a->Fb.Pitch);
                for (ULONG x = 0; x < a->Fb.Width; x++)
                    row[x] = 0xFFFF00FFu;
            }
            KeMemoryBarrier();
            ok = ((volatile ULONG *)a->FbVa)[a->Fb.Width / 2] == 0xFFFF00FFu;
            MwLog(a, secs == 20 ? L"TestBand20" : L"TestBand40", ok ? rows : 0xBAD);
        }
        gate = S9Gate(&a->S9, &level);
        a->ThermalLevel = gate ? 10 + gate : level;
        if ((gate != 0 || level >= SBTH_STOP) != hold) {
            hold = !hold;
            a->ThermalHold = hold;
            if (hold)
                a->ThermalHolds++;
            MwLog(a, L"S9ThermalHold", hold ? a->ThermalLevel : 0);
        }
        a->FbLiveLast = MwFbSamples(a);
        if (logNow && secs % 10 == 0 && a->FbVa != NULL && secs <= 600) {
            /* T14d: what the panel shows over the boot (lock screen, sign-in, desktop, a test app): the centre
             * pixel every 10 s as FbCtr<seconds> */
            WCHAR nm[16];
            UNICODE_STRING us;
            RtlInitEmptyUnicodeString(&us, nm, sizeof(nm));
            if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"FbCtr%03u", secs)))
                MwLog(a, nm, ((const ULONG *)(a->FbVa + (SIZE_T)(a->Fb.Height / 2) * a->Fb.Pitch))[a->Fb.Width / 2]);
        }
        a->LogBatch = FALSE;
        if (logNow && secs - lastFlush >= (liveFlushSec ? liveFlushSec : 1)) {
            lastFlush = secs;
            MwLogFlush(a);
        }
        if (a->FbLiveLast >= 8)
            a->FbLiveSecs++;
        /* T14a: healthy = the panel itself showed content for 30 s (at least 8 of 16 points across the whole
         * framebuffer not black: a black screen with a spinner does not pass, T13d..T13s did), DWM sent 30
         * frames (flips or blts into the primary), and 30 GPU jobs completed, a minute after start */
        if (!a->Healthy && (KeQueryInterruptTime() - t0) / 10000 >= SBW_HEALTHY_MS &&
            a->FbLiveSecs >= 30 && a->EmuFlips + a->PrimaryBlts >= SBW_HEALTHY_FLIPS &&
            a->JobsDone >= SBW_HEALTHY_JOBS) {
            /* T13d: 223 copies to the panel, all black, counted as healthy: only frames with pixels count */
            /* T13c: flips + any blts counted 212 presents that never reached the panel as healthy */
            a->Healthy = TRUE;
            if (autoClear)
                S9PutDword(NULL, L"FullGuardBoots", 0);
            MwLog(a, L"S9Healthy", a->JobsDone);
            State(L"FullHealthy", 1);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

void S9GuardStart(MW_ADAPTER *A)
{
    OBJECT_ATTRIBUTES oa;
    KeInitializeEvent(&A->GuardStop, NotificationEvent, FALSE);
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(PsCreateSystemThread(&A->GuardThread, THREAD_ALL_ACCESS, &oa, NULL, NULL, GuardThread, A)))
        (void)ObReferenceObjectByHandle(A->GuardThread, SYNCHRONIZE, *PsThreadType, KernelMode,
                                        (PVOID *)&A->GuardThreadObj, NULL);
}

void S9GuardStop(MW_ADAPTER *A)
{
    if (A->GuardThreadObj) {
        KeSetEvent(&A->GuardStop, IO_NO_INCREMENT, FALSE);
        (void)KeWaitForSingleObject(A->GuardThreadObj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(A->GuardThreadObj);
        A->GuardThreadObj = NULL;
    }
    if (A->GuardThread) {
        ZwClose(A->GuardThread);
        A->GuardThread = NULL;
    }
    A->ThermalHold = FALSE;
}

NTSTATUS S9DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath, PDRIVER_INITIALIZATION_DATA Full)
{
    ULONG want, guard;
    BOOLEAN kill;

    /* The whole driver off, before anything else runs (no registry access yet): a file dropped from TWRP at
     * C:\Windows\Starbite\sbwddm.off. Windows then uses Basic Display. Covers a fault in this function too,
     * which neither boot guard would count. */
    if (S9FileExists(L"\\SystemRoot\\Starbite\\sbwddm.off"))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    S9SetServiceKey(RegistryPath);
    {
        static const char id[] = SB_BUILD_ID;
        ULONG h = 0;
        for (const char *c = id; *c; c++)
            h = h * 31 + (UCHAR)*c;
        State(L"WddmBuildHash", h);
    }
    want = S9GetDword(L"Parameters", L"FullMode", 0);
    guard = S9GetDword(NULL, L"FullGuardBoots", 0);
    kill = S9FileExists(L"\\SystemRoot\\Starbite\\sbwddm.full.off");
    State(L"FullModeWanted", want);
    State(L"FullGuardBootsAtStart", guard);
    State(L"FullKillFile", kill);
    if (!want || kill || guard >= SBW_FULL_GUARD_LIMIT) {
        State(L"WddmMode", 0);            /* 0 = display-only (sbdisplay) */
        if (want && guard >= SBW_FULL_GUARD_LIMIT)
            State(L"FullRefused", guard);
        return SdDriverEntry(DriverObject, RegistryPath);
    }
    S9PutDword(NULL, L"FullGuardBoots", guard + 1);   /* counted now, cleared when healthy */
    State(L"WddmMode", 1);
    State(L"FullRefused", 0);             /* no stale refusal from an earlier boot */
    State(L"FullHealthy", 0);
    return DxgkInitialize(DriverObject, RegistryPath, Full);
}
