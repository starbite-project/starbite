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
#define SBW_HEALTHY_FLIPS 30U             /* frames with pixels that reached the panel (ContentFrames) */
#define SBW_HEALTHY_JOBS 30U
#define SBW_LIVE_MAX 64U

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
        { L"Live_ScanContent", a->ScanContent }, { L"Live_RefreshFromBltDst", a->RefreshFromBltDst },
        /* T13f: what DECON scans (read only; sbdisplay's registers): IDMA channel 0's base address, live and
         * shadow, its image size, the trigger control, the window enables and a pending shadow update */
        { L"Live_DeconBaseY", Rd(a->DeconDmaVa, SBDC_CH0 + SBDC_IDMA_BASE_Y) },
        { L"Live_DeconShadowBaseY", Rd(a->DeconDmaVa, SBDC_CH0 + SBDC_IDMA_SHADOW + SBDC_IDMA_BASE_Y) },
        { L"Live_DeconImgSize", Rd(a->DeconDmaVa, SBDC_CH0 + SBDC_IDMA_IMG_SIZE) },
        { L"Live_DeconTrig", Rd(a->DeconVa, SBDC_TRIG) }, { L"Live_DeconPath0", Rd(a->DeconVa, SBDC_PATH0) },
        { L"Live_DeconShadowReq", Rd(a->DeconVa, SBDC_SHADOW_REQ) },
        { L"Live_ThermalLevel", a->ThermalLevel },
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
    ULONG64 nextSec = KeQueryInterruptTime() + 10000000ULL, lastWork = (ULONG64)-1;
    MW_ALLOCATION *lastScan = NULL;
    if (refreshMs != 0 && refreshMs < 50)
        refreshMs = 50;

    for (;;) {
        ULONG level, gate;
        t.QuadPart = -10000LL * (refreshMs ? refreshMs : 1000);
        if (KeWaitForSingleObject(&a->GuardStop, Executive, KernelMode, FALSE, &t) != STATUS_TIMEOUT)
            break;
        if (refreshMs) {
            /* anything new on the GPU or presented, or another primary scanned: refresh the panel */
            ULONG64 work = (ULONG64)a->JobsDone << 32 | (a->Presents + a->Flips);
            if (work != lastWork || a->ScanAlloc != lastScan) {
                lastWork = work;
                lastScan = a->ScanAlloc;
                MwRefreshScanout(a);
            }
        }
        if (KeQueryInterruptTime() < nextSec)
            continue;
        nextSec += 10000000ULL;
        LiveCounters(a, prev);
        MwLog(a, L"Live_Seconds", ++secs);
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
        if (!a->Healthy && (KeQueryInterruptTime() - t0) / 10000 >= SBW_HEALTHY_MS &&
            a->ContentFrames >= SBW_HEALTHY_FLIPS && a->FbContentFrames >= SBW_HEALTHY_FLIPS &&
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
