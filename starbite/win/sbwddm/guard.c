/* SPDX-License-Identifier: GPL-2.0-only
 * sbwddm's guards (docs/WIN11-GPU.md 6.8). One binary, two modes:
 *   full          the WDDM 1.3 render + display driver (maliwddm, S9 port): DWM on the Mali through Mesa;
 *   display-only  sbdisplay, unchanged (its code is built into this image): today's desktop.
 * DriverEntry picks full mode only when Parameters\FullMode = 1, no kill file
 * (\SystemRoot\Starbite\sbwddm.full.off) and State\FullGuardBoots < 1. A full-mode boot is counted at once
 * and cleared only when it is healthy: 60 s after start, with DWM's flips and GPU jobs completing. So any
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

#define SBW_FULL_GUARD_LIMIT 1U
#define SBW_HEALTHY_MS 60000U
#define SBW_HEALTHY_FLIPS 30U             /* frames that reached the panel: flip copies + blts into the primary */
#define SBW_HEALTHY_JOBS 30U

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
        { L"Live_PrimaryBlts", a->PrimaryBlts },
        { L"Live_ThermalLevel", a->ThermalLevel },
    };
    for (ULONG i = 0; i < sizeof(c) / sizeof(c[0]) && i < 40; i++)
        if (c[i].v != Prev[i] || Prev[40] == 0) {
            Prev[i] = c[i].v;
            MwLog(a, c[i].n, c[i].v);
        }
    Prev[40] = 1;
}

static VOID GuardThread(PVOID Ctx)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Ctx;
    ULONG64 t0 = KeQueryInterruptTime();
    LARGE_INTEGER t;
    BOOLEAN hold = FALSE;
    ULONG prev[41] = { 0 }, secs = 0;

    for (;;) {
        ULONG level, gate;
        t.QuadPart = -10000LL * 1000;
        if (KeWaitForSingleObject(&a->GuardStop, Executive, KernelMode, FALSE, &t) != STATUS_TIMEOUT)
            break;
        LiveCounters(a, prev);
        MwLog(a, L"Live_Seconds", ++secs);
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
            a->FlipCopies + a->PrimaryBlts >= SBW_HEALTHY_FLIPS && a->JobsDone >= SBW_HEALTHY_JOBS) {
            /* T13c: flips + any blts counted 212 presents that never reached the panel as healthy */
            a->Healthy = TRUE;
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
