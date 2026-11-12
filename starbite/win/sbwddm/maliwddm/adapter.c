// SPDX-License-Identifier: GPL-2.0
/*
 * maliwddm: driver entry, PnP and power, adapter capabilities, interrupts,
 * escapes, and the registry trace that found every M5.1 bug.
 */
#include "maliwddm.h"
#include <ntstrsafe.h>

/* ---- trace ---- */

/* T17: the trace key to disk (after a batch of MwLog writes). */
void MwLogFlush(MW_ADAPTER *A)
{
    HANDLE key, sub;
    UNICODE_STRING subName = RTL_CONSTANT_STRING(L"sbwddm");
    OBJECT_ATTRIBUTES oa;

    if (A == NULL || A->Pdo == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key)))
        return;
    InitializeObjectAttributes(&oa, &subName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, key, NULL);
    if (NT_SUCCESS(ZwOpenKey(&sub, KEY_WRITE, &oa))) {
        (void)ZwFlushKey(sub);
        ZwClose(sub);
    }
    ZwClose(key);
}

void MwLog(MW_ADAPTER *A, PCWSTR Name, ULONG Value)
{
    HANDLE key, sub;
    UNICODE_STRING name, subName = RTL_CONSTANT_STRING(L"sbwddm");   /* S9 port: our trace key */
    OBJECT_ATTRIBUTES oa;

    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "sbwddm: %ws = 0x%08x\n", Name, Value);
    if (A == NULL || A->Pdo == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key)))
        return;
    /* A subkey of Device Parameters of its own, so a test run can delete
     * the previous run's trace (and malidod's) in one step. */
    InitializeObjectAttributes(&oa, &subName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, key, NULL);
    if (NT_SUCCESS(ZwCreateKey(&sub, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        RtlInitUnicodeString(&name, Name);
        (void)ZwSetValueKey(sub, &name, 0, REG_DWORD, &Value, sizeof(Value));
        /* To disk now: a bugcheck loses registry writes still in memory,
         * and the last value before one is the one that matters.
         * S9 port (T17): not inside the guard thread's batches. A flush writes the whole SYSTEM hive's log;
         * one per Live_ value (~26 a second) kept the UFS busy for the whole user session 2026-11-09, made
         * every registry user wait, and left the primary hive 10,000 log entries behind at its crash. */
        if (!A->LogBatch)
            (void)ZwFlushKey(sub);
        ZwClose(sub);
    }
    ZwClose(key);
}

/* T39: a text value in the same trace key (REG_SZ; the ASCII text widened). */
void MwLogSz(MW_ADAPTER *A, PCWSTR Name, const char *Text)
{
    HANDLE key, sub;
    UNICODE_STRING name, subName = RTL_CONSTANT_STRING(L"sbwddm");
    OBJECT_ATTRIBUTES oa;
    static WCHAR w[8192];                 /* the guard thread only (T44/T46: Live_HangLog, Live_FaultLog) */
    ULONG n = 0;

    if (A == NULL || A->Pdo == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    for (; Text[n] && n < RTL_NUMBER_OF(w) - 1; n++)
        w[n] = (WCHAR)(UCHAR)Text[n];
    w[n] = 0;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key)))
        return;
    InitializeObjectAttributes(&oa, &subName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, key, NULL);
    if (NT_SUCCESS(ZwCreateKey(&sub, KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
        RtlInitUnicodeString(&name, Name);
        (void)ZwSetValueKey(sub, &name, 0, REG_SZ, w, (n + 1) * sizeof(WCHAR));
        if (!A->LogBatch)
            (void)ZwFlushKey(sub);
        ZwClose(sub);
    }
    ZwClose(key);
}

static const char *JobTypeName(ULONG T);

/* T39: the newest 16 failed jobs as text, when there are new ones (guard thread, once a second).
 * T46: for an MMU fault, where its address falls among the live allocations (LIVE: that allocation, its paging state
 * and whether the running submission listed it; NONE: the nearest live allocations below and above), and the slot's
 * JS_HEAD with the job type the DPC read there. */
void MwFaultLog(MW_ADAPTER *A)
{
    static char t[8192];
    static const char *const why[] = { "unread", "", "no view", "head in no mapped allocation", "page changed",
                                       "head unaligned" };
    static const char *const pte[] = { "0", "1", "S" };   /* MW_PTE_*: S = the KMD-scratch page (evicted) */
    LONG n = A->FaultCount, first = n > 16 ? n - 16 : 0;
    size_t used = 0;
    MW_FAULT f;
    if (n == A->FaultLogged)
        return;
    A->FaultLogged = n;
    t[0] = 0;
    for (LONG k = first; k < n && used + 520 < sizeof(t); k++) {
        const MW_FAULT *r = &A->Faults[k & 15];
        LONG seq = r->Seq;
        KeMemoryBarrier();
        RtlCopyMemory(&f, (const void *)r, sizeof(f));
        KeMemoryBarrier();
        if (seq != k + 1 || r->Seq != seq) {
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used, "#%ld overwritten while read; ", k + 1);
            used = strlen(t);
            continue;
        }
        const char *cr = (const char *)f.Creator;
        char c[9] = { 0 };
        for (int j = 0; j < 8; j++)
            c[j] = cr[j] >= 32 && cr[j] < 127 ? cr[j] : (cr[j] ? '?' : 0);
        if (f.Kind == 2)
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used,
                "#%ld %lu.%03lus js 0x%02lx mmu 0x%08lx va 0x%llx IN A FREED RANGE 0x%llx+%lu pages of %s, freed %lu ms before",
                k + 1, f.Ms / 1000, f.Ms % 1000, f.Js, f.Status, f.Va, f.FreedVa, f.FreedPages, c, f.AgoMs);
        else
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used, "#%ld %lu.%03lus js 0x%02lx mmu 0x%08lx va 0x%llx %s",
                k + 1, f.Ms / 1000, f.Ms % 1000, f.Js, f.Status, f.Va,
                f.Kind ? "not in a recently freed range" : "no MMU fault");
        used = strlen(t);
        if (f.Where == MW_FAULT_IN_LIVE) {
            const char *lc = (const char *)f.LCreator;
            char l[9] = { 0 };
            char listed[48];
            for (int j = 0; j < 8; j++)
                l[j] = lc[j] >= 32 && lc[j] < 127 ? lc[j] : (lc[j] ? '?' : 0);
            if (f.LListed == f.RunSeq)
                (void)RtlStringCbCopyA(listed, sizeof(listed), "listed by the running submission");
            else if (f.LListed > f.RunSeq)
                (void)RtlStringCbPrintfA(listed, sizeof(listed), "listed %ld renders later", f.LListed - f.RunSeq);
            else if (f.LListed == 0)
                (void)RtlStringCbCopyA(listed, sizeof(listed), "NEVER listed");
            else
                (void)RtlStringCbPrintfA(listed, sizeof(listed), "NOT listed (last %ld renders before)",
                                         f.RunSeq - f.LListed);
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used,
                " | LIVE 0x%llx+%lup %s age %lu ms, mapped %u pte %s, maps %lu (last %ld ms ago) unmaps %lu (last %ld ms ago), %s",
                f.LVa, f.LPages, l[0] ? l : "?", f.Ms - f.LCreatedMs, f.LMapped,
                f.PteNow < RTL_NUMBER_OF(pte) ? pte[f.PteNow] : "?", f.LMaps,
                f.LMaps ? (LONG)(f.Ms - f.LMapMs) : -1L, f.LUnmaps, f.LUnmaps ? (LONG)(f.Ms - f.LUnmapMs) : -1L, listed);
        } else if (f.Where == MW_FAULT_IN_NONE) {
            ULONG64 bend = f.BelowVa + ((ULONG64)f.BelowPages << PAGE_SHIFT);
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used,
                " | NONE of %lu live, pte %s: below 0x%llx+%lup (ends 0x%llx, %lld bytes before va) above 0x%llx+%lup (%lld bytes after va)",
                f.LiveCount, f.PteNow < RTL_NUMBER_OF(pte) ? pte[f.PteNow] : "?", f.BelowVa, f.BelowPages, bend, f.BelowVa ? (LONG64)(f.Va - bend) : -1LL,
                f.AboveVa, f.AbovePages, f.AboveVa ? (LONG64)(f.AboveVa - f.Va) : -1LL);
        }
        used = strlen(t);
        if (f.DescState == MW_FDESC_OK)
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used, " | slot %lu head 0x%llx job %s idx %lu dep %lu/%lu exc 0x%lx; ",
                f.Slot, f.Head, JobTypeName((f.D4 >> 1) & 0x7F), f.D4 >> 16, f.D5 & 0xFFFF, f.D5 >> 16, f.D0);
        else
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used, " | slot %lu head 0x%llx job ? (%s); ", f.Slot, f.Head,
                f.DescState < RTL_NUMBER_OF(why) ? why[f.DescState] : "?");
        used = strlen(t);
    }
    MwLogSz(A, L"Live_FaultLog", t);
    MwLog(A, L"Live_FaultCount", (ULONG)n);
    MwLog(A, L"Live_FaultsInLive", A->FaultsInLive);
    MwLog(A, L"Live_FaultsInNone", A->FaultsInNone);
    MwLog(A, L"Live_FaultDescReads", A->FaultDescReads);
}

/* T44: Mesa's genxml v6.xml "Job Type" (Job Header word 4, bits [7:1]) */
static const char *JobTypeName(ULONG T)
{
    static const char *const n[] = { "not-started", "null", "write-value", "cache-flush", "compute", "vertex",
                                     "geometry", "tiler", "fused", "fragment", "indexed-vertex" };
    return T < RTL_NUMBER_OF(n) ? n[T] : "type?";
}

/* T44: the newest MW_HANGS job timeouts (MW_HANG, recorded by Reap before the hard stop), one line each, when there
 * are new ones (guard thread, PASSIVE_LEVEL, once a second). A record Reap rewrote while it was copied is skipped. */
void MwHangLog(MW_ADAPTER *A)
{
    static char t[6144];
    static const char *const why[] = { "", "no CPU view of JS_HEAD prepared", "JS_HEAD in no chain-start allocation",
                                       "view/page mismatch", "JS_HEAD unaligned" };
    LONG n = A->HangCount, first = n > MW_HANGS ? n - MW_HANGS : 0;
    size_t used = 0;
    MW_HANG h;

    if (n == A->HangLogged)
        return;
    A->HangLogged = n;
    t[0] = 0;
    for (LONG k = first; k < n && used + 640 < sizeof(t); k++) {
        const MW_HANG *r = &A->Hangs[k & (MW_HANGS - 1)];
        LONG seq = r->Seq;
        char c[9] = { 0 };
        KeMemoryBarrier();
        RtlCopyMemory(&h, (const void *)r, sizeof(h));
        KeMemoryBarrier();
        if (seq != k + 1 || r->Seq != seq) {
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used, "#%ld overwritten while read\n", k + 1);
            used = strlen(t);
            continue;
        }
        for (int j = 0; j < 8; j++) {
            char ch = ((const char *)h.Creator)[j];
            c[j] = ch >= 32 && ch < 127 ? ch : (ch ? '?' : 0);
        }
        (void)RtlStringCbPrintfA(t + used, sizeof(t) - used,
            "#%ld %lu.%03lus slot %lu fence %lu chain %lu/%lu ran %lu ms (sub %lu ms) jc 0x%llx head 0x%llx tail 0x%llx "
            "js 0x%02lx cfg 0x%08lx aff 0x%llx gpu 0x%08lx gfault 0x%08lx@0x%llx as0 0x%08lx@0x%llx mmuraw 0x%lx ",
            k + 1, h.Ms / 1000, h.Ms % 1000, h.Slot, h.Fence, h.Cmd, h.Cmds, h.ChainMs, h.SubMs, h.Jc, h.Head, h.Tail,
            h.JsStatus, h.JsConfig, h.Affinity, h.GpuStatus, h.GpuFaultStatus, h.GpuFaultAddress, h.AsFaultStatus,
            h.AsFaultAddress, h.MmuRawStat);
        used = strlen(t);
        if (h.AllocVa != 0)
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used, "in 0x%llx+%lu pages of %s ", h.AllocVa, h.AllocPages,
                                     c[0] ? c : "?");
        used = strlen(t);
        if (h.DescState == MW_HANG_DESC_OK) {
            const ULONG *d = h.Desc;
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used,
                "job %s idx %lu dep %lu/%lu next 0x%llx exc 0x%lx: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx "
                "%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
                JobTypeName((d[4] >> 1) & 0x7F), d[4] >> 16, d[5] & 0xFFFF, d[5] >> 16, d[6] | (ULONG64)d[7] << 32, d[0],
                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
        } else {
            (void)RtlStringCbPrintfA(t + used, sizeof(t) - used, "no descriptor: %s\n",
                                     h.DescState < RTL_NUMBER_OF(why) ? why[h.DescState] : "?");
        }
        used = strlen(t);
    }
    MwLogSz(A, L"Live_HangLog", t);
    MwLog(A, L"Live_HangCount", (ULONG)n);
    MwLog(A, L"Live_HangViews", A->HangViews);
    MwLog(A, L"Live_HangViewFails", A->HangViewFails);
}

/* S9 port (T13c): delete the previous boot's trace subkey, so nothing stale is read as this boot's (T13b's
 * pull still showed T13a's Fail_StartGpu and S9NoInterrupt). */
void MwTraceReset(MW_ADAPTER *A)
{
    HANDLE key, sub;
    UNICODE_STRING subName = RTL_CONSTANT_STRING(L"sbwddm");
    OBJECT_ATTRIBUTES oa;
    if (A == NULL || A->Pdo == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(A->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key)))
        return;
    InitializeObjectAttributes(&oa, &subName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, key, NULL);
    if (NT_SUCCESS(ZwOpenKey(&sub, DELETE, &oa))) {
        (void)ZwDeleteKey(sub);
        ZwClose(sub);
    }
    ZwClose(key);
}

/* The CI run that built this image (0 for a local build): WDK builds are
 * deterministic, so __DATE__ and __TIME__ do not exist. */
#ifndef MW_BUILD
#define MW_BUILD 0
#endif

void MwLogHook(void *Owner, PCWSTR Name, ULONG Value)
{
    MwLog((MW_ADAPTER *)Owner, Name, Value);
}

/* Seen_<ddi> on a DDI's first call, Fail_<ddi> on any failure; Name is
 * "Fail_<ddi>". Only at PASSIVE_LEVEL, where the registry can be written. */
/* Enter_<ddi> = call order, written BEFORE a DDI's first call runs, so a
 * DDI that never returns (a bugcheck inside it) is still on record. Name is
 * "Enter_<ddi>". */
/* S9 port (T13c): exact first-call tracking. Upstream kept one bit per hash(name) % 64, and in T13b Render,
 * Escape and SubmitCommand shared bits with earlier DDIs, so their Seen/Order entries never appeared. A
 * name table instead: a racing duplicate only logs twice. */
#define MW_TRACE_NAMES 128
static PCWSTR TraceNames[MW_TRACE_NAMES];
static volatile LONG TraceCount;

static BOOLEAN FirstTime(PCWSTR Name)
{
    LONG n = TraceCount, i;
    for (i = 0; i < n && i < MW_TRACE_NAMES; i++)
        if (TraceNames[i] && wcscmp(TraceNames[i], Name) == 0)
            return FALSE;
    i = InterlockedIncrement(&TraceCount) - 1;
    if (i < MW_TRACE_NAMES)
        TraceNames[i] = Name;
    return TRUE;
}

void MwEnter(MW_ADAPTER *A, PCWSTR Name)
{
    if (A == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    if (!FirstTime(Name))
        return;
    MwLog(A, Name, (ULONG)InterlockedIncrement((volatile LONG *)&A->CallOrder));
}

NTSTATUS MwRet(MW_ADAPTER *A, PCWSTR Name, NTSTATUS St)
{
    if (A == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return St;
    if (FirstTime(Name)) {
        WCHAR seen[64];
        UNICODE_STRING us;
        RtlInitEmptyUnicodeString(&us, seen, sizeof(seen));
        if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"Seen_%ws", Name + 5)))
            MwLog(A, seen, (ULONG)St);
        /* Which DDI dxgkrnl called first, second, ... */
        RtlInitEmptyUnicodeString(&us, seen, sizeof(seen));
        if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"Order_%ws", Name + 5)))
            MwLog(A, seen, (ULONG)InterlockedIncrement((volatile LONG *)&A->CallOrder));
    }
    if (!NT_SUCCESS(St)) {
        MwLog(A, Name, (ULONG)St);
        MwLog(A, L"LastFailedDdi", (ULONG)St);
    }
    return St;
}

/* ---- PnP ---- */

static void StopGpu(MW_ADAPTER *A);

static NTSTATUS APIENTRY MwAddDevice(IN_CONST_PDEVICE_OBJECT Pdo, OUT_PPVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(MW_ADAPTER), MW_TAG);
    if (a == NULL)
        return STATUS_NO_MEMORY;
    a->Pdo = (PDEVICE_OBJECT)Pdo;
    MwTraceReset(a);                      /* S9 port: only this boot's values in the trace key */
    ExInitializeFastMutex(&a->MmuLock);
    a->Gpu.Log = MwLogHook;
    a->Gpu.LogOwner = a;
    MwCommandInit(a);
    *Context = a;
    return STATUS_SUCCESS;
}

/* S9 port: the Mali's register block among the device's resources (STRB0072 also lists DECON, DPU_DMA and
 * DSIM; malidod's first-resource rule would map DECON). */
static NTSTATUS MapGpu(MW_ADAPTER *A)
{
    DXGK_DEVICE_INFO info;
    NTSTATUS st = A->Dxgk.DxgkCbGetDeviceInformation(A->Dxgk.DeviceHandle, &info);
    if (!NT_SUCCESS(st))
        return st;
    PCM_RESOURCE_LIST list = info.TranslatedResourceList;
    for (ULONG l = 0; list && l < list->Count; l++) {
        PCM_PARTIAL_RESOURCE_LIST p = &list->List[l].PartialResourceList;
        for (ULONG i = 0; i < p->Count; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &p->PartialDescriptors[i];
            if (d->Type == CmResourceTypeInterrupt)
                A->Interrupts++;
            if (d->Type == CmResourceTypeMemory && A->Gpu.Regs == NULL &&
                (ULONG64)d->u.Memory.Start.QuadPart == SBM_GPU_PA && d->u.Memory.Length >= SBM_GPU_SIZE) {
                A->Gpu.RegsLength = d->u.Memory.Length;
                A->Gpu.Regs = (volatile ULONG *)MmMapIoSpaceEx(d->u.Memory.Start, d->u.Memory.Length,
                                                               PAGE_READWRITE | PAGE_NOCACHE);
            }
        }
    }
    return A->Gpu.Regs ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS APIENTRY MwStartDevice(IN_CONST_PVOID Context, IN_PDXGK_START_INFO StartInfo,
                                       IN_PDXGKRNL_INTERFACE Dxgk, OUT_PULONG Sources, OUT_PULONG Children)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(StartInfo);

    a->Dxgk = *Dxgk;
    MwLog(a, L"Build", MW_BUILD);
    MwLog(a, L"RenderOnly", MW_RENDER_ONLY);

    /* S9 port: the GPU first, the display after it. The GPU is NOT optional: without it DWM's surfaces would
     * never be drawn (a black desktop with no fallback). A refusal here happens before the firmware
     * framebuffer is taken over, so Windows' Basic Display takes the desktop as it found it, and the guard
     * makes the next boot sbdisplay. */
    st = MapGpu(a);
    if (NT_SUCCESS(st))
        st = S9Init(&a->S9, a->Gpu.Regs, a->Gpu.RegsLength);
    if (NT_SUCCESS(st))
        st = S9Start(a);                  /* watchdog alive, G3D domain on, vdd_g3d 950 mV */
    if (NT_SUCCESS(st))
        st = MkMmuInit(&a->Gpu);
    if (NT_SUCCESS(st))
        st = MkGpuInit(&a->Gpu);
    a->GpuUp = NT_SUCCESS(st);
    MwLog(a, L"GpuUp", NT_SUCCESS(st) ? 1 : (ULONG)st);
    /* KMD-scratch: evictions point PTEs at a read-only zero page (default on; 0 = PTE 0 as before) */
    a->ScratchUnmap = S9GetDword(L"Parameters", L"ScratchUnmap", 1) != 0 && a->Gpu.ScratchPa != 0;
    MwLog(a, L"ScratchUnmap", a->ScratchUnmap);
    /* Fences complete from the GPU interrupt (and the poll goes through DxgkCbSynchronizeExecution, which
     * needs one). Without the Mali's interrupt lines (the stage-2 SSDT declares none) work would never
     * complete: repeated TDRs, then bugcheck 0x116. So after the whole bring-up (logged), refuse. */
    MwLog(a, L"S9Interrupts", a->Interrupts);
    if (a->GpuUp && a->Interrupts == 0 && !S9GetDword(L"Parameters", L"AllowNoInterrupt", 0)) {
        MwLog(a, L"S9NoInterrupt", 1);
        a->GpuUp = FALSE;
    }
    if (!a->GpuUp) {
        StopGpu(a);
        return MwRet(a, L"Fail_StartGpu", STATUS_DEVICE_NOT_READY);
    }
#if !MW_RENDER_ONLY
    st = MwDisplayStart(a);
    if (!NT_SUCCESS(st)) {
        StopGpu(a);
        return MwRet(a, L"Fail_StartDisplay", st);
    }
#endif
    a->StartTime = KeQueryInterruptTime();
    S9GuardStart(a);

    /* Render-only: no sources and no children, and the firmware framebuffer
     * stays with Basic Display, which keeps the desktop up. */
    *Sources = MW_RENDER_ONLY ? 0 : 1;
    *Children = MW_RENDER_ONLY ? 0 : 1;
    MwLog(a, L"Started", 1);
    return STATUS_SUCCESS;
}

static void StopGpu(MW_ADAPTER *A)
{
    S9GuardStop(A);
    MkGpuStop(&A->Gpu);
    if (A->Gpu.Regs != NULL && S9DomainOn(&A->S9)) {
        /* cores off before the rail goes back (sbm_core's order) */
        sbm_power_off(&A->S9.Io);
    }
    S9RailDown(&A->S9);
    S9Fini(&A->S9);
    MkMmuFree(&A->Gpu);
    if (A->Gpu.Regs != NULL) {
        MmUnmapIoSpace((PVOID)A->Gpu.Regs, A->Gpu.RegsLength);
        A->Gpu.Regs = NULL;
    }
    A->GpuUp = FALSE;
}

static NTSTATUS APIENTRY MwStopDevice(IN_CONST_PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    MwCommandStop(a);
    MwLog(a, L"Renders", a->Renders);
    MwLog(a, L"Submits", a->Submits);
    MwLog(a, L"JobsDone", a->JobsDone);
    MwLog(a, L"JobsFailed", a->JobsFailed);
    MwLog(a, L"Vsyncs", a->Vsyncs);
    MwLog(a, L"Flips", a->Flips);
    MwLog(a, L"FlipCopies", a->FlipCopies);
    MwLog(a, L"FlipCopySkips", a->FlipCopySkips);
    StopGpu(a);
    MwDisplayStop(a);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY MwRemoveDevice(IN_CONST_PVOID Context)
{
    MwCommandFini((MW_ADAPTER *)Context);  /* T44 */
    ExFreePoolWithTag(Context, MW_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY MwDispatchIoRequest(IN_CONST_PVOID Context, IN_ULONG Source,
                                             IN_PVIDEO_REQUEST_PACKET Vrp)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Source);
    UNREFERENCED_PARAMETER(Vrp);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY MwSetPowerState(IN_CONST_PVOID Context, IN_ULONG Uid,
                                         IN_DEVICE_POWER_STATE State, IN_POWER_ACTION Action)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Uid);
    UNREFERENCED_PARAMETER(State);
    UNREFERENCED_PARAMETER(Action);
    return STATUS_SUCCESS;
}

static VOID APIENTRY MwResetDevice(IN_CONST_PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static VOID APIENTRY MwUnload(VOID)
{
}

static NTSTATUS APIENTRY MwQueryInterface(IN_CONST_PVOID Context, IN_PQUERY_INTERFACE Qi)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Qi);
    return STATUS_NOT_SUPPORTED;
}

/* ---- interrupts ---- */

static BOOLEAN APIENTRY MwInterruptRoutine(IN_CONST_PVOID Context, IN_ULONG MessageNumber)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    UNREFERENCED_PARAMETER(MessageNumber);
    if (!a->GpuUp || !MkIsr(&a->Gpu))
        return FALSE;
    (void)MwJobInterrupt(a);
    return TRUE;
}

static VOID APIENTRY MwDpcRoutine(IN_CONST_PVOID Context)
{
    MW_ADAPTER *a = (MW_ADAPTER *)Context;
    MwFaultDescDpc(a);                    /* T46: before dxgkrnl processes the dropped fence */
    a->Dxgk.DxgkCbNotifyDpc(a->Dxgk.DeviceHandle);
}

/* ---- adapter information ---- */

/* A field of a structure dxgkrnl sized for our interface version: write it
 * only if it lies inside the buffer (M5.1: the WDK's sizeof is larger). */
#define FITS(Size, Type, Field) (FIELD_OFFSET(Type, Field) + RTL_FIELD_SIZE(Type, Field) <= (Size))

static NTSTATUS DriverCaps(const DXGKARG_QUERYADAPTERINFO *Info)
{
    DXGK_DRIVERCAPS *caps = (DXGK_DRIVERCAPS *)Info->pOutputData;
    UINT size = Info->OutputDataSize;

    if (!FITS(size, DXGK_DRIVERCAPS, WDDMVersion))
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(caps, size);
    caps->WDDMVersion = DXGKDDI_WDDMv1_3;
    caps->HighestAcceptableAddress.QuadPart = (1LL << 40) - 1;   /* the Mali sees 40 bits */
    caps->MaxAllocationListSlotId = 256;   /* Render patches every allocation, SlotId = index */
    caps->SchedulingCaps.MultiEngineAware = 1;
    /* Preemption is off system-wide (the INF sets EnablePreemption 0), as
     * viogpu3d does; DMA-buffer-boundary preemption comes later. */
    caps->SchedulingCaps.PreemptionAware = 1;
    caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
    /* The four caps WDDM 1.2's table marks mandatory for a render-only
     * driver. Without them, every render-only start failed with
     * STATUS_GRAPHICS_INVALID_DRIVER_MODEL right after dxgkrnl read these
     * caps (WinPE runs 5-9, ETW events 24/110/250 then the failure); a full
     * driver without them was accepted. */
#if MW_RENDER_ONLY
#if MW_RENDER_ONLY
    caps->PresentationCaps.SupportKernelModeCommandBuffer = 1; /* see MwRenderKm */
#endif
    /* The shared primary lives in our only segment, an aperture, and the
     * UMD does not set UseAlternateVA; dxgkrnl then insists that "all
     * segments in the supported write segment set for a shared primary be
     * CPU visible" (an aperture segment never is) and the primary is never
     * created: installed Windows, 2026-10-17, black screen, no
     * SetVidPnSourceAddress. DXGK_SEGMENTFLAGS' remarks give the way out:
     * a section-backed primary. */
    caps->MemoryManagementCaps.SectionBackedPrimary = 1;
    caps->FlipCaps.FlipOnVSyncMmIo = 1;                         /* no display: unused */
    if (FITS(size, DXGK_DRIVERCAPS, PreemptionCaps)) {
        /* Honest: a DMA buffer always runs to its end. */
        caps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
    }
    if (FITS(size, DXGK_DRIVERCAPS, SupportPerEngineTDR))
        caps->SupportPerEngineTDR = TRUE;   /* ResetEngine, QueryEngineStatus, QueryDependentEngineGroup */
#endif
    if (FITS(size, DXGK_DRIVERCAPS, SupportNonVGA))
        caps->SupportNonVGA = TRUE;
    if (FITS(size, DXGK_DRIVERCAPS, SupportSmoothRotation))
        caps->SupportSmoothRotation = TRUE;
    return STATUS_SUCCESS;
}

static void GpuInfo(MW_ADAPTER *A, MW_ESCAPE_GPU_INFO_DATA *D)
{
    const GPU_FEATURES *f = &A->Gpu.F;
    mw_u64 v[MW_GPU_PARAMS] = {0};

    /* Indices are DRM_PANFROST_PARAM_*, as malikm's GET_PARAM. */
    v[0] = f->Id; v[1] = f->Revision; v[2] = f->ShaderPresent; v[3] = f->TilerPresent;
    v[4] = f->L2Present; v[5] = f->StackPresent; v[6] = f->AsPresent; v[7] = f->JsPresent;
    v[8] = f->L2Features; v[9] = f->CoreFeatures; v[10] = f->TilerFeatures; v[11] = f->MemFeatures;
    v[12] = f->MmuFeatures; v[13] = f->ThreadFeatures; v[14] = f->MaxThreads;
    v[15] = f->MaxWorkgroupSize; v[16] = f->MaxBarrierSize; v[17] = f->CoherencyFeatures;
    for (ULONG i = 0; i < 4; i++)
        v[18 + i] = f->TextureFeatures[i];
    for (ULONG i = 0; i < 16; i++)
        v[22 + i] = f->JsFeatures[i];
    v[38] = f->NrCoreGroups; v[39] = f->ThreadTlsAlloc; v[40] = f->AfbcFeatures;
    v[44] = 31;                                  /* SELECTED_COHERENCY: none */
    RtlCopyMemory(D->Value, v, sizeof(v));
    D->Count = MW_GPU_PARAMS;
    D->Valid = (1ULL << 41) - 1;                 /* 0..40 */
    D->Valid |= 1ULL << 44;
    if (!A->GpuUp)
        D->Valid = 0;
}

static NTSTATUS APIENTRY MwQueryAdapterInfo(IN_CONST_HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *Info)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    NTSTATUS st;

    switch (Info->Type) {
    case DXGKQAITYPE_DRIVERCAPS:
        st = DriverCaps(Info);
        break;
    case DXGKQAITYPE_QUERYSEGMENT3:
        st = MwQuerySegment(a, Info);
        break;
    case DXGKQAITYPE_HISTORYBUFFERPRECISION:
        /* WDDM 1.3 (Windows 8.1) asks this of every 1.3 driver. Answering
         * STATUS_NOT_SUPPORTED was logged by dxgkrnl as "Driver returned an
         * invalid NTSTATUS code" right before every render-only start
         * failed with STATUS_GRAPHICS_INVALID_DRIVER_MODEL (WinPE runs 5-8).
         * No history buffers are written; 64 bits is the honest width of
         * anything that would be. */
        if (Info->OutputDataSize < sizeof(DXGKARG_HISTORYBUFFERPRECISION)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            ((DXGKARG_HISTORYBUFFERPRECISION *)Info->pOutputData)->PrecisionBits = 64;
            st = STATUS_SUCCESS;
        }
        break;
    case 47: /* DXGKQAITYPE_64BITONLYCAPS: zero = not restricted to 64-bit */
        RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
        st = STATUS_SUCCESS;
        break;
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        RtlZeroMemory(Info->pOutputData, Info->OutputDataSize);
        st = STATUS_SUCCESS;
        break;
    default:
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    WCHAR name[32];
    UNICODE_STRING us;
    RtlInitEmptyUnicodeString(&us, name, sizeof(name));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"QAI_%u", (ULONG)Info->Type)))
        MwLog(a, name, (ULONG)st);
    RtlInitEmptyUnicodeString(&us, name, sizeof(name));
    if (NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"QAI_%u_Size", (ULONG)Info->Type)))
        MwLog(a, name, Info->OutputDataSize);
    return st;
}

static void Stats(MW_ADAPTER *A, MW_ESCAPE_STATS_DATA *D, UINT Size)
{
    D->GpuUp = A->GpuUp;
    D->Renders = A->Renders;
    D->Presents = A->Presents;
    D->Submits = A->Submits;
    D->LastSubmittedFence = A->LastSubmittedFence;
    D->LastCompletedFence = A->LastCompletedFence;
    D->Queued = A->QCount;
    D->Running = A->Running;
    D->JobsDone = A->JobsDone;
    D->JobsFailed = A->JobsFailed;
    D->JobsTimedOut = A->JobsTimedOut;
    D->LastJsStatus = A->LastJsStatus;
    D->LastFaultStatus = A->LastFaultStatus;
    D->Irqs = A->Gpu.Irqs;
    D->Resets = A->Gpu.Resets;
    D->LastFaultAddress = A->LastFaultAddress;
    for (ULONG i = 0; i < 16; i++)
        D->PagingOps[i] = A->PagingOps[i];
    D->Maps = A->Maps;
    D->MapFails = A->MapFails;
    D->Unmaps = A->Unmaps;
    D->LastMapPages = A->LastMapPages;
    D->LastMapStatus = (mw_u32)A->LastMapStatus;
    D->Allocations = A->Allocations;
    D->LastMapVa = A->LastMapVa;
    D->LastAllocVa = A->LastAllocVa;
    D->Patches = A->Patches;
    D->PatchAllocations = A->PatchAllocations;
    D->PatchPatchLocations = A->PatchPatchLocations;
    D->PatchSegment0 = A->PatchSegment0;
    D->PatchAddress0 = A->PatchAddress0;
    D->PresentBlts = A->PresentBlts;
    D->PresentFills = A->PresentFills;
    D->PresentSkips = A->PresentSkips;
    /* Callers built before these existed pass the shorter structure. */
    if (Size >= RTL_SIZEOF_THROUGH_FIELD(MW_ESCAPE_STATS_DATA, FlipCopySkips)) {
        D->Vsyncs = A->Vsyncs;
        D->Flips = A->Flips;
        D->FlipCopies = A->FlipCopies;
        D->FlipCopySkips = A->FlipCopySkips;
    }
}

static NTSTATUS APIENTRY MwEscape(IN_CONST_HANDLE hAdapter, const DXGKARG_ESCAPE *Esc)
{
    MW_ADAPTER *a = (MW_ADAPTER *)hAdapter;
    mw_u32 code;

    if (Esc->PrivateDriverDataSize < sizeof(code)) {
        /* Run 6: an escape we do not know (DWM's, by the timing) failed
         * here and its device was destroyed next. Record what it was. */
        MwLog(a, L"EscapeSmallSize", Esc->PrivateDriverDataSize);
        MwLog(a, L"EscapeSmallFlags", Esc->Flags.Value);
        return STATUS_INVALID_PARAMETER;
    }
    code = *(const mw_u32 *)Esc->pPrivateDriverData;
    switch (code) {
    case MW_ESCAPE_ALLOC_INFO:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_ALLOC_INFO_DATA))
            return STATUS_INVALID_PARAMETER;
        return MwEscapeAllocInfo(a, Esc, (MW_ESCAPE_ALLOC_INFO_DATA *)Esc->pPrivateDriverData);
    case MW_ESCAPE_GPU_INFO:
        if (Esc->PrivateDriverDataSize < sizeof(MW_ESCAPE_GPU_INFO_DATA))
            return STATUS_INVALID_PARAMETER;
        GpuInfo(a, (MW_ESCAPE_GPU_INFO_DATA *)Esc->pPrivateDriverData);
        return STATUS_SUCCESS;
    case MW_ESCAPE_SB_NOTE:
        if (Esc->PrivateDriverDataSize >= sizeof(MW_ESCAPE_SB_NOTE_DATA)) {
            const MW_ESCAPE_SB_NOTE_DATA *n = (const MW_ESCAPE_SB_NOTE_DATA *)Esc->pPrivateDriverData;
            WCHAR nm[24];
            UNICODE_STRING us;
            RtlInitEmptyUnicodeString(&us, nm, sizeof(nm));
            if (n->Id < 64 && NT_SUCCESS(RtlUnicodeStringPrintf(&us, L"UmdNote_%u", n->Id)))
                MwLog(a, nm, n->Value);
        }
        return STATUS_SUCCESS;
    case MW_ESCAPE_SB_RATES:
        if (Esc->PrivateDriverDataSize >= sizeof(MW_ESCAPE_SB_RATES_DATA)) {
            MW_ESCAPE_SB_RATES_DATA *r = (MW_ESCAPE_SB_RATES_DATA *)Esc->pPrivateDriverData;
            r->Presents = a->Presents;
            r->JobsDone = a->JobsDone;
            r->Renders = a->Renders;
            r->GpuBusy100ns = (mw_u64)a->GpuBusy100ns;
            r->PresentWait100ns = (mw_u64)a->PresentWait100ns;
            r->PresentCpu100ns = (mw_u64)a->PresentCpu100ns;
            r->Now100ns = KeQueryInterruptTime();
            r->ThermalHolds = a->ThermalHolds;
            return STATUS_SUCCESS;
        }
        return STATUS_INVALID_PARAMETER;
    case MW_ESCAPE_STATS:
        if (Esc->PrivateDriverDataSize < (UINT)FIELD_OFFSET(MW_ESCAPE_STATS_DATA, Vsyncs))
            return STATUS_INVALID_PARAMETER;
        Stats(a, (MW_ESCAPE_STATS_DATA *)Esc->pPrivateDriverData, Esc->PrivateDriverDataSize);
        return STATUS_SUCCESS;
    default:
        MwLog(a, L"EscapeUnknownCode", code);
        MwLog(a, L"EscapeUnknownSize", Esc->PrivateDriverDataSize);
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS APIENTRY MwSetPointerPosition(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY MwSetPointerShape(IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *Arg)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Arg);
    return STATUS_NOT_SUPPORTED;
}

/* ---- the traced DDI table ---- */

/* The wrappers find the adapter from the DDI's first parameter, whose type
 * differs: an adapter, a device (OpenAllocation, CreateContext, ...), a
 * context (Render, Present, ...) or the miniport context. Build 97 traced
 * OpenAllocation as if its hDevice were the adapter: an out-of-bounds pool
 * write and a device pointer passed to IoOpenDeviceRegistryKey as a PDO,
 * from a system call -- SYSTEM_SERVICE_EXCEPTION. */
#define TRACE_WITH(Name, Proto, Args, AdapterExpr)                             \
    static NTSTATUS APIENTRY T_##Name Proto                                    \
    {                                                                          \
        MW_ADAPTER *a_ = (AdapterExpr);                                        \
        MwEnter(a_, L"Enter_" MW_W(#Name));                                    \
        return MwRet(a_, L"Fail_" MW_W(#Name), Mw##Name Args);                 \
    }
#define TRACE_ADAPTER(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, (MW_ADAPTER *)hAdapter)
#define TRACE_DEVICE(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, ((MW_DEVICE *)hDevice)->Adapter)
#define TRACE_CTX(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, ((MW_CONTEXT *)hContext)->Device->Adapter)

TRACE_ADAPTER(QueryAdapterInfo, (IN_CONST_HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *a), (hAdapter, a))
TRACE_ADAPTER(Escape, (IN_CONST_HANDLE hAdapter, const DXGKARG_ESCAPE *a), (hAdapter, a))
TRACE_ADAPTER(CreateAllocation, (IN_CONST_HANDLE hAdapter, DXGKARG_CREATEALLOCATION *a), (hAdapter, a))
TRACE_ADAPTER(DestroyAllocation, (IN_CONST_HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *a), (hAdapter, a))
TRACE_ADAPTER(DescribeAllocation, (IN_CONST_HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *a), (hAdapter, a))
TRACE_ADAPTER(GetStandardAllocationDriverData, (IN_CONST_HANDLE hAdapter, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *a), (hAdapter, a))
TRACE_ADAPTER(BuildPagingBuffer, (IN_CONST_HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER *a), (hAdapter, a))
TRACE_ADAPTER(CreateDevice, (IN_CONST_HANDLE hAdapter, DXGKARG_CREATEDEVICE *a), (hAdapter, a))
TRACE_ADAPTER(IsSupportedVidPn, (IN_CONST_HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *a), (hAdapter, a))
TRACE_ADAPTER(RecommendFunctionalVidPn, (IN_CONST_HANDLE hAdapter, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *a), (hAdapter, a))
TRACE_ADAPTER(EnumVidPnCofuncModality, (IN_CONST_HANDLE hAdapter, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *a), (hAdapter, a))
TRACE_ADAPTER(SetVidPnSourceVisibility, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *a), (hAdapter, a))
TRACE_ADAPTER(CommitVidPn, (IN_CONST_HANDLE hAdapter, const DXGKARG_COMMITVIDPN *a), (hAdapter, a))
TRACE_ADAPTER(UpdateActiveVidPnPresentPath, (IN_CONST_HANDLE hAdapter, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *a), (hAdapter, a))
TRACE_ADAPTER(RecommendMonitorModes, (IN_CONST_HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *a), (hAdapter, a))
TRACE_ADAPTER(QueryVidPnHWCapability, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYVIDPNHWCAPABILITY *a), (hAdapter, a))
TRACE_ADAPTER(SetVidPnSourceAddress, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *a), (hAdapter, a))
TRACE_ADAPTER(ResetFromTimeout, (IN_CONST_HANDLE hAdapter), (hAdapter))
TRACE_ADAPTER(RestartFromTimeout, (IN_CONST_HANDLE hAdapter), (hAdapter))
TRACE_ADAPTER(ResetEngine, (IN_CONST_HANDLE hAdapter, DXGKARG_RESETENGINE *a), (hAdapter, a))
TRACE_ADAPTER(QueryEngineStatus, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *a), (hAdapter, a))
TRACE_ADAPTER(ControlInterrupt, (IN_CONST_HANDLE hAdapter, IN_CONST_DXGK_INTERRUPT_TYPE t, IN_BOOLEAN e), (hAdapter, t, e))
TRACE_ADAPTER(GetScanLine, (IN_CONST_HANDLE hAdapter, DXGKARG_GETSCANLINE *a), (hAdapter, a))

#define TRACE_CONTEXT(Name, Proto, Args) TRACE_WITH(Name, Proto, Args, (MW_ADAPTER *)Context)

TRACE_CONTEXT(QueryChildRelations, (IN_CONST_PVOID Context, PDXGK_CHILD_DESCRIPTOR d, IN_ULONG n), (Context, d, n))
TRACE_CONTEXT(QueryChildStatus, (IN_CONST_PVOID Context, INOUT_PDXGK_CHILD_STATUS c, IN_BOOLEAN b), (Context, c, b))
TRACE_CONTEXT(QueryDeviceDescriptor, (IN_CONST_PVOID Context, IN_ULONG u, INOUT_PDXGK_DEVICE_DESCRIPTOR d), (Context, u, d))
TRACE_CONTEXT(SetPowerState, (IN_CONST_PVOID Context, IN_ULONG u, IN_DEVICE_POWER_STATE p, IN_POWER_ACTION a), (Context, u, p, a))
TRACE_CONTEXT(QueryInterface, (IN_CONST_PVOID Context, IN_PQUERY_INTERFACE q), (Context, q))
TRACE_CONTEXT(DispatchIoRequest, (IN_CONST_PVOID Context, IN_ULONG s, IN_PVIDEO_REQUEST_PACKET v), (Context, s, v))
TRACE_CONTEXT(StopDevice, (IN_CONST_PVOID Context), (Context))
TRACE_DEVICE(OpenAllocation, (IN_CONST_HANDLE hDevice, const DXGKARG_OPENALLOCATION *a), (hDevice, a))
TRACE_DEVICE(CloseAllocation, (IN_CONST_HANDLE hDevice, const DXGKARG_CLOSEALLOCATION *a), (hDevice, a))
TRACE_DEVICE(CreateContext, (IN_CONST_HANDLE hDevice, DXGKARG_CREATECONTEXT *a), (hDevice, a))
TRACE_DEVICE(DestroyDevice, (IN_CONST_HANDLE hDevice), (hDevice))
TRACE_CTX(DestroyContext, (IN_CONST_HANDLE hContext), (hContext))
TRACE_CTX(Render, (IN_CONST_HANDLE hContext, DXGKARG_RENDER *a), (hContext, a))
TRACE_CTX(Present, (IN_CONST_HANDLE hContext, DXGKARG_PRESENT *a), (hContext, a))
TRACE_ADAPTER(SetPointerPosition, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERPOSITION *a), (hAdapter, a))
TRACE_ADAPTER(SetPointerShape, (IN_CONST_HANDLE hAdapter, const DXGKARG_SETPOINTERSHAPE *a), (hAdapter, a))
TRACE_ADAPTER(Patch, (IN_CONST_HANDLE hAdapter, const DXGKARG_PATCH *a), (hAdapter, a))
TRACE_ADAPTER(SubmitCommand, (IN_CONST_HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *a), (hAdapter, a))
TRACE_ADAPTER(QueryCurrentFence, (IN_CONST_HANDLE hAdapter, DXGKARG_QUERYCURRENTFENCE *a), (hAdapter, a))
TRACE_ADAPTER(CollectDbgInfo, (IN_CONST_HANDLE hAdapter, const DXGKARG_COLLECTDBGINFO *a), (hAdapter, a))
TRACE_ADAPTER(CancelCommand, (IN_CONST_HANDLE hAdapter, const DXGKARG_CANCELCOMMAND *a), (hAdapter, a))
TRACE_ADAPTER(PreemptCommand, (IN_CONST_HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *a), (hAdapter, a))

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA init = {0};

    init.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;
    init.DxgkDdiAddDevice = MwAddDevice;
    init.DxgkDdiStartDevice = MwStartDevice;
    init.DxgkDdiStopDevice = T_StopDevice;
    init.DxgkDdiRemoveDevice = MwRemoveDevice;
    init.DxgkDdiDispatchIoRequest = T_DispatchIoRequest;
    init.DxgkDdiInterruptRoutine = MwInterruptRoutine;
    init.DxgkDdiDpcRoutine = MwDpcRoutine;
    init.DxgkDdiQueryChildRelations = T_QueryChildRelations;
    init.DxgkDdiQueryChildStatus = T_QueryChildStatus;
    init.DxgkDdiQueryDeviceDescriptor = T_QueryDeviceDescriptor;
    init.DxgkDdiSetPowerState = T_SetPowerState;
    init.DxgkDdiResetDevice = MwResetDevice;
    init.DxgkDdiUnload = MwUnload;
    init.DxgkDdiQueryInterface = T_QueryInterface;

    init.DxgkDdiQueryAdapterInfo = T_QueryAdapterInfo;
    init.DxgkDdiEscape = T_Escape;
    init.DxgkDdiCreateAllocation = T_CreateAllocation;
    init.DxgkDdiDestroyAllocation = T_DestroyAllocation;
    init.DxgkDdiDescribeAllocation = T_DescribeAllocation;
    init.DxgkDdiGetStandardAllocationDriverData = T_GetStandardAllocationDriverData;
    init.DxgkDdiOpenAllocation = T_OpenAllocation;
    init.DxgkDdiCloseAllocation = T_CloseAllocation;
    init.DxgkDdiBuildPagingBuffer = T_BuildPagingBuffer;

    init.DxgkDdiCreateDevice = T_CreateDevice;
    init.DxgkDdiDestroyDevice = T_DestroyDevice;
    init.DxgkDdiCreateContext = T_CreateContext;
    init.DxgkDdiDestroyContext = T_DestroyContext;
    init.DxgkDdiRender = T_Render;
    init.DxgkDdiRenderKm = MwRenderKm;
    init.DxgkDdiPresent = T_Present;
    init.DxgkDdiPatch = T_Patch;
    init.DxgkDdiSubmitCommand = T_SubmitCommand;
    init.DxgkDdiPreemptCommand = T_PreemptCommand;
    init.DxgkDdiQueryCurrentFence = T_QueryCurrentFence;
    init.DxgkDdiResetFromTimeout = T_ResetFromTimeout;
    init.DxgkDdiRestartFromTimeout = T_RestartFromTimeout;
    init.DxgkDdiCollectDbgInfo = T_CollectDbgInfo;
    init.DxgkDdiGetNodeMetadata = MwGetNodeMetadata;
    init.DxgkDdiResetEngine = T_ResetEngine;
    init.DxgkDdiQueryEngineStatus = T_QueryEngineStatus;
    init.DxgkDdiQueryDependentEngineGroup = MwQueryDependentEngineGroup;
    init.DxgkDdiCancelCommand = T_CancelCommand;
    init.DxgkDdiControlInterrupt = T_ControlInterrupt;
    init.DxgkDdiGetScanLine = T_GetScanLine;

    init.DxgkDdiSetPointerPosition = T_SetPointerPosition;
    init.DxgkDdiSetPointerShape = T_SetPointerShape;
    init.DxgkDdiIsSupportedVidPn = T_IsSupportedVidPn;
    init.DxgkDdiRecommendFunctionalVidPn = T_RecommendFunctionalVidPn;
    init.DxgkDdiEnumVidPnCofuncModality = T_EnumVidPnCofuncModality;
    init.DxgkDdiSetVidPnSourceVisibility = T_SetVidPnSourceVisibility;
    init.DxgkDdiCommitVidPn = T_CommitVidPn;
    init.DxgkDdiUpdateActiveVidPnPresentPath = T_UpdateActiveVidPnPresentPath;
    init.DxgkDdiSetVidPnSourceAddress = T_SetVidPnSourceAddress;
    init.DxgkDdiRecommendMonitorModes = T_RecommendMonitorModes;
    init.DxgkDdiQueryVidPnHWCapability = T_QueryVidPnHWCapability;
    init.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = MwStopDeviceAndReleasePostDisplayOwnership;
    init.DxgkDdiSystemDisplayEnable = MwSystemDisplayEnable;
    init.DxgkDdiSystemDisplayWrite = MwSystemDisplayWrite;

    /* S9 port: full mode or sbdisplay (display-only), chosen by guard.c */
    return S9DriverEntry(DriverObject, RegistryPath, &init);
}
