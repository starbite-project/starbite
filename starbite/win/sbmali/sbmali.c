/* SPDX-License-Identifier: GPL-2.0-only
 * sbmali.sys: stage 3 of docs/WIN11-GPU.md, the Mali-G72 MP18 brought up under Windows 11 on the
 * Galaxy S9. A KMDF driver for the ACPI device \_SB.GPU0 (STRB0072), installed instead of sbdisplay for
 * the bring-up test T5. Port of StarbiteMali's bring-up (xnu/src/starlte_gpu.c) through sbm_core.c.
 *
 * When the device starts, a worker thread runs the self-test once and writes the results to
 *   HKLM\SYSTEM\CurrentControlSet\Services\sbmali\State
 *   1. the thermal gate: \Callback\StarbiteThermal (opened, never created) must answer, its seq must
 *      move, its reading must be fresh and its level Normal; else nothing is touched (waits up to 10 min
 *      for the watchdog, an auto-start service the SCM loads well after PnP starts this device)
 *   2. the read-only probe: G3D domain on (never switched), GPU_ID 0x6221
 *   3. vdd_g3d (BUCK6) to 950 mV through ACPM, read back; the clock stays at S-Boot's 260 MHz (no DVFS)
 *   4. Parameters\Runs (20) runs in each format of Parameters\Formats (bit 0 legacy Mali LPAE, bit 1
 *      AArch64 4K; default both): reset, cores on, AS0, WRITE_VALUE "STARBITE", a 64 x 64 fragment clear
 *      checked pixel by pixel, cores off. The watchdog is checked before every run; 50 ms between runs
 *   5. soft reset, BUCK6 back to the value found, State\Done = 1
 * Polled (no interrupt lines in the stage 2/3 SSDT). It writes only: the Mali's registers, BUCK6 through
 * ACPM, its own memory. Never the PMU, CMU, MIF/INT or any clock.
 *
 * Boot guard Services\sbmali\GuardBoots (cleared whenever the thread ends cleanly); kill file
 * \Windows\Starbite\sbmali.off; Start = 4 via tools/win/hive-service.py on a clean hive.
 */
#include <ntddk.h>
#include <wdf.h>
#include "sbm_core.h"
#include "../sbthermal/sbth_core.h"

#define SM_TAG 'mMbS'
#define SM_GUARD_LIMIT 2

#ifndef SB_BUILD_ID
#define SB_BUILD_ID "unknown"
#endif

typedef struct _SM_CTX {
    volatile UCHAR *Mmio[SBM_BLOCKS];
    SIZE_T MmioSize[SBM_BLOCKS];
    struct sbm_io Io;
    PUCHAR Mem;                           /* SBM_TEST_BYTES, contiguous, uncached */
    PHYSICAL_ADDRESS MemPa;
    BOOLEAN BuckRaised;
    UCHAR BuckOld;
    BOOLEAN GpuTouched;
    HANDLE Thread;
    PKTHREAD ThreadObj;
    KEVENT Started, Stop;
} SM_CTX, *PSM_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SM_CTX, SmGetCtx)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD SmDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE SmPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE SmReleaseHardware;
static KSTART_ROUTINE SmThread;

static UNICODE_STRING SmSvc;
static WCHAR SmSvcBuf[256];

/* ---- registry ------------------------------------------------------------------------------- */

static NTSTATUS OpenSvc(PCWSTR Sub, HANDLE *H)
{
    WCHAR buf[300];
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    ULONG disp;
    RtlInitEmptyUnicodeString(&path, buf, sizeof(buf));
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&path, &SmSvc)))
        return STATUS_NAME_TOO_LONG;
    if (Sub && (!NT_SUCCESS(RtlAppendUnicodeToString(&path, L"\\")) || !NT_SUCCESS(RtlAppendUnicodeToString(&path, Sub))))
        return STATUS_NAME_TOO_LONG;
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateKey(H, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp);
}

static ULONG GetDword(HANDLE H, PCWSTR Name, ULONG Dflt)
{
    UNICODE_STRING n;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len;
    RtlInitUnicodeString(&n, Name);
    if (NT_SUCCESS(ZwQueryValueKey(H, &n, KeyValuePartialInformation, info, sizeof(buf), &len)) &&
        info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
        return *(ULONG *)info->Data;
    return Dflt;
}

static void PutDword(HANDLE H, PCWSTR Name, ULONG V)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, Name);
    (void)ZwSetValueKey(H, &n, 0, REG_DWORD, &V, sizeof(V));
}

static void Log(PCWSTR Name, ULONG Value, BOOLEAN Flush)
{
    HANDLE h;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "sbmali: %ws = 0x%08x\n", Name, Value);
    if (!NT_SUCCESS(OpenSvc(L"State", &h)))
        return;
    PutDword(h, Name, Value);
    if (Flush)
        (void)ZwFlushKey(h);
    ZwClose(h);
}

static ULONG Param(PCWSTR Name, ULONG Dflt)
{
    HANDLE h;
    ULONG v = Dflt;
    if (NT_SUCCESS(OpenSvc(L"Parameters", &h))) {
        v = GetDword(h, Name, Dflt);
        ZwClose(h);
    }
    return v;
}

/* ---- register access ------------------------------------------------------------------------ */

#define SM_DSB() __dsb(_ARM64_BARRIER_SY)

static sbm_u32 IoRd(void *Ctx, unsigned B, unsigned Off)
{
    PSM_CTX c = (PSM_CTX)Ctx;
    if (B >= SBM_BLOCKS || !c->Mmio[B] || Off + 4 > c->MmioSize[B])
        return 0;
    return READ_REGISTER_ULONG((volatile ULONG *)(c->Mmio[B] + Off));
}

static void IoWr(void *Ctx, unsigned B, unsigned Off, sbm_u32 V)
{
    PSM_CTX c = (PSM_CTX)Ctx;
    /* The PMU and CMU_G3D are mapped for reading only. */
    if (B == SBM_PMU || B == SBM_CMU || B >= SBM_BLOCKS || !c->Mmio[B] || Off + 4 > c->MmioSize[B])
        return;
    SM_DSB();
    WRITE_REGISTER_ULONG((volatile ULONG *)(c->Mmio[B] + Off), V);
    SM_DSB();
}

static void IoDelay(void *Ctx, unsigned Us)
{
    UNREFERENCED_PARAMETER(Ctx);
    KeStallExecutionProcessor(Us);
}

static void IoBarrier(void *Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    SM_DSB();
}

/* ---- the thermal gate ----------------------------------------------------------------------- */

/* 0 = the watchdog answers, is alive and Normal; otherwise a reason code (State\ThermalGate):
 *   1 \Callback\StarbiteThermal does not exist (sbthermal not loaded yet, or not installed)
 *   2 the object exists but nothing answers the query (sbthermal gone)
 *   3 the last reading is older than 1 s       4 the reading counter did not move
 *   10 + level: alive but not Normal (11 Warm, 12 Hot, 13 Stop, 14 Critical) */
static ULONG ThermalGate(PCALLBACK_OBJECT *Cb, struct sbth_state *Last)
{
    struct sbth_state s;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    LARGE_INTEGER t;
    ULONG64 now;

    if (*Cb == NULL) {
        RtlInitUnicodeString(&name, SBTH_CALLBACK_NAME);
        InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (!NT_SUCCESS(ExCreateCallback(Cb, &oa, FALSE, TRUE)))   /* open only: never create it */
            return 1;
    }
    RtlZeroMemory(&s, sizeof(s));
    ExNotifyCallback(*Cb, SBTH_CB_QUERY, &s);
    if (s.version != SBTH_STATE_VERSION)
        return 2;                         /* object without the watchdog behind it */
    now = KeQueryInterruptTime();
    if (now - s.time_100ns > 10000000ULL)
        return 3;                         /* reading older than 1 s */
    if (Last->version && s.seq == Last->seq) {
        t.QuadPart = -10000LL * 600;      /* give it one more period, then it must have moved */
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        RtlZeroMemory(&s, sizeof(s));
        ExNotifyCallback(*Cb, SBTH_CB_QUERY, &s);
        if (s.seq == Last->seq)
            return 4;
    }
    *Last = s;
    return s.level == SBTH_NORMAL ? 0 : 10 + s.level;
}

/* ---- the test ------------------------------------------------------------------------------- */

#define SM_GATE_WAIT_MS (10U * 60U * 1000U)

/* Where the thread is, flushed (State\Step): 1 waiting for the watchdog, 2 probing (first GPU read),
 * 3 BUCK6, 4 legacy runs, 5 AArch64 runs, 6 putting everything back, 7 finished. */
static void Step(ULONG N)
{
    Log(L"Step", N, TRUE);
}

static void LogFormat(PCWSTR Prefix, const struct sbm_test_result *r)
{
    static const PCWSTR names[] = { L"Runs", L"WriteOk", L"ClearOk", L"Faults", L"Timeouts", L"ResetFail",
                                    L"PowerFail", L"AsFail", L"LastJsStatus", L"LastAsFault", L"LastGpuFault",
                                    L"LastAsAddrLo", L"MaxJobUs" };
    ULONG v[13] = { r->runs, r->write_ok, r->clear_ok, r->faults, r->timeouts, r->reset_fail, r->power_fail,
                    r->as_fail, r->last_js_status, r->last_as_fault, r->last_gpu_fault, (ULONG)r->last_as_addr,
                    r->max_us };
    for (unsigned i = 0; i < 13; i++) {
        WCHAR n[48];
        UNICODE_STRING us;
        RtlInitEmptyUnicodeString(&us, n, sizeof(n) - sizeof(WCHAR));
        if (NT_SUCCESS(RtlAppendUnicodeToString(&us, Prefix)) && NT_SUCCESS(RtlAppendUnicodeToString(&us, names[i]))) {
            n[us.Length / sizeof(WCHAR)] = 0;
            Log(n, v[i], FALSE);
        }
    }
}

static void Finish(PSM_CTX c)
{
    if (c->GpuTouched) {
        (void)sbm_soft_reset(&c->Io, NULL);   /* address spaces and job slots back to idle, cores off */
        sbm_power_off(&c->Io);
        c->GpuTouched = FALSE;
    }
    if (c->BuckRaised) {
        Log(L"BuckRestore", (ULONG)sbm_buck6_restore(&c->Io, c->BuckOld), TRUE);
        c->BuckRaised = FALSE;
    }
}

static VOID SmThread(PVOID Ctx)
{
    PSM_CTX c = (PSM_CTX)Ctx;
    PCALLBACK_OBJECT cb = NULL;
    struct sbth_state last;
    struct sbm_info inf;
    ULONG gate = 1, runs, formats;
    LARGE_INTEGER t;
    HANDLE h;
    int pr, br;

    ObReferenceObject(KeGetCurrentThread());
    c->ThreadObj = KeGetCurrentThread();
    KeSetEvent(&c->Started, IO_NO_INCREMENT, FALSE);
    RtlZeroMemory(&last, sizeof(last));

    runs = Param(L"Runs", 20);
    formats = Param(L"Formats", 3);
    if (runs > 200)
        runs = 200;

    /* 1. The watchdog. PnP starts this device early in boot; sbthermal (an auto-start service) is loaded by
     * the Service Control Manager much later (T5 run 1: about 75 s after boot began, and this driver gave
     * up after 30 s). Wait up to SM_GATE_WAIT_MS, reporting progress every 10 s. Nothing is touched
     * before the gate opens. */
    Step(1);
    {
        ULONG waited = 0, tries = 0;
        for (;;) {
            gate = ThermalGate(&cb, &last);
            tries++;
            if (!gate || waited >= SM_GATE_WAIT_MS)
                break;
            if (tries % 20 == 1) {
                Log(L"ThermalGateLast", gate, FALSE);
                Log(L"ThermalGateWaitMs", waited, TRUE);
            }
            t.QuadPart = -10000LL * 500;
            if (KeWaitForSingleObject(&c->Stop, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS)
                break;
            waited += 500;
        }
        Log(L"ThermalGateTries", tries, FALSE);
        Log(L"ThermalGateWaitMs", waited, FALSE);
    }
    Log(L"ThermalGate", gate, TRUE);
    if (gate)
        goto out;
    Log(L"GpuMilliCStart", (ULONG)last.gpu_mc, FALSE);

    /* 2. Read only: the domain, the GPU. The step is flushed first: if the first GPU read faults (the
     * TZPC case), Step 2 is the last thing the hive shows. */
    Step(2);
    pr = sbm_probe(&c->Io, &inf);
    Log(L"Probe", (ULONG)pr, FALSE);
    Log(L"PmuStatus", inf.pmu_status, FALSE);
    if (pr == -1)
        goto out;
    Log(L"GpuId", inf.gpu_id, FALSE);
    Log(L"ShaderPresent", (ULONG)inf.shader_present, FALSE);
    Log(L"MmuFeatures", inf.mmu_features, FALSE);
    Log(L"AsPresent", inf.as_present, FALSE);
    Log(L"JsPresent", inf.js_present, FALSE);
    Log(L"Coherency", inf.coherency, FALSE);
    Log(L"PllKhz", inf.pll_khz, TRUE);
    if (pr)
        goto out;

    /* 3. vdd_g3d to 950 mV. */
    Step(3);
    br = sbm_buck6_raise(&c->Io, &c->BuckOld);
    c->BuckRaised = br == 0 || br == -3;  /* -3: written but not read back as asked: restore anyway */
    Log(L"BuckOld", c->BuckOld, FALSE);
    Log(L"BuckRaise", (ULONG)br, TRUE);
    if (br)
        goto out;

    /* 4. The runs. */
    for (int f = 0; f < 2; f++) {
        struct sbm_test_result r;
        if (!(formats & (1U << f)))
            continue;
        Step(4 + f);
        RtlZeroMemory(&r, sizeof(r));
        for (ULONG i = 0; i < runs; i++) {
            gate = ThermalGate(&cb, &last);
            if (gate) {
                Log(L"ThermalStop", gate, TRUE);
                break;
            }
            c->GpuTouched = TRUE;
            (void)sbm_selftest_run(&c->Io, (enum sbm_fmt)f, inf.shader_present, c->Mem, (sbm_u64)c->MemPa.QuadPart, &r);
            Log(f ? L"A64_RunsDone" : L"Legacy_RunsDone", r.runs, TRUE);   /* progress, flushed per run */
            t.QuadPart = -10000LL * 50;
            if (KeWaitForSingleObject(&c->Stop, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS)
                break;
        }
        LogFormat(f ? L"A64_" : L"Legacy_", &r);
        if (gate)
            break;
    }
    Log(L"GpuMilliCEnd", (ULONG)last.gpu_mc, FALSE);
    Log(L"Done", 1, TRUE);
out:
    Step(6);
    Finish(c);
    /* Reaching this point means no crash: clear the boot guard whether the test ran or the gate refused
     * (a refusal is not a reason to stop loading next time). */
    if (NT_SUCCESS(OpenSvc(NULL, &h))) {
        PutDword(h, L"GuardBoots", 0);
        (void)ZwFlushKey(h);
        ZwClose(h);
    }
    Step(7);
    if (cb)
        ObDereferenceObject(cb);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- PnP ------------------------------------------------------------------------------------ */

static PVOID MapPa(ULONG64 Pa, SIZE_T Size)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = (LONGLONG)Pa;
    return MmMapIoSpaceEx(p, Size, PAGE_READWRITE | PAGE_NOCACHE);
}

static void Unmap(PSM_CTX c)
{
    for (unsigned b = 0; b < SBM_BLOCKS; b++)
        if (c->Mmio[b]) {
            MmUnmapIoSpace((PVOID)c->Mmio[b], c->MmioSize[b]);
            c->Mmio[b] = NULL;
        }
}

static NTSTATUS SmPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSM_CTX c = SmGetCtx(Device);
    PHYSICAL_ADDRESS lo, hi, skip;
    OBJECT_ATTRIBUTES oa;
    ULONG n = WdfCmResourceListGetCount(Translated);
    UNREFERENCED_PARAMETER(Raw);

    /* The Mali from the ACPI resources; the rest are fixed SoC blocks the SSDT does not list. */
    for (ULONG i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);
        if (d && d->Type == CmResourceTypeMemory && (ULONG64)d->u.Memory.Start.QuadPart == SBM_GPU_PA &&
            d->u.Memory.Length >= SBM_GPU_SIZE && !c->Mmio[SBM_GPU]) {
            c->Mmio[SBM_GPU] = (volatile UCHAR *)MmMapIoSpaceEx(d->u.Memory.Start, SBM_GPU_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
            c->MmioSize[SBM_GPU] = SBM_GPU_SIZE;
        }
    }
    c->Mmio[SBM_PMU] = (volatile UCHAR *)MapPa(SBM_PMU_PA, SBM_PMU_SIZE);
    c->MmioSize[SBM_PMU] = SBM_PMU_SIZE;
    c->Mmio[SBM_CMU] = (volatile UCHAR *)MapPa(SBM_CMU_PA, SBM_CMU_SIZE);
    c->MmioSize[SBM_CMU] = SBM_CMU_SIZE;
    c->Mmio[SBM_ACPM_SRAM] = (volatile UCHAR *)MapPa(SBM_ACPM_SRAM_PA, SBM_ACPM_SRAM_SIZE);
    c->MmioSize[SBM_ACPM_SRAM] = SBM_ACPM_SRAM_SIZE;
    c->Mmio[SBM_ACPM_MBOX] = (volatile UCHAR *)MapPa(SBM_ACPM_MBOX_PA, SBM_ACPM_MBOX_SIZE);
    c->MmioSize[SBM_ACPM_MBOX] = SBM_ACPM_MBOX_SIZE;
    for (unsigned b = 0; b < SBM_BLOCKS; b++)
        if (!c->Mmio[b]) {
            Log(L"MapFailedBlock", b, TRUE);
            Unmap(c);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    /* The CMU is read only while the domain is on (sbm_probe checks the PMU first). */

    lo.QuadPart = 0;
    hi.QuadPart = 0xffffffffffLL;         /* 40-bit output addresses */
    skip.QuadPart = 0;
    c->Mem = (PUCHAR)MmAllocateContiguousNodeMemory(SBM_TEST_BYTES, lo, hi, skip, PAGE_READWRITE | PAGE_NOCACHE,
                                                    MM_ANY_NODE_OK);
    if (!c->Mem) {
        Unmap(c);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    c->MemPa = MmGetPhysicalAddress(c->Mem);
    Log(L"MemPaLo", (ULONG)c->MemPa.QuadPart, FALSE);
    Log(L"MemPaHi", (ULONG)(c->MemPa.QuadPart >> 32), TRUE);

    c->Io.ctx = c;
    c->Io.rd = IoRd;
    c->Io.wr = IoWr;
    c->Io.delay_us = IoDelay;
    c->Io.barrier = IoBarrier;
    KeInitializeEvent(&c->Started, NotificationEvent, FALSE);
    KeInitializeEvent(&c->Stop, NotificationEvent, FALSE);
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(PsCreateSystemThread(&c->Thread, THREAD_ALL_ACCESS, &oa, NULL, NULL, SmThread, c)))
        (void)KeWaitForSingleObject(&c->Started, Executive, KernelMode, FALSE, NULL);
    Log(L"Started", 1, TRUE);
    return STATUS_SUCCESS;
}

static NTSTATUS SmReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSM_CTX c = SmGetCtx(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (c->ThreadObj) {
        KeSetEvent(&c->Stop, IO_NO_INCREMENT, FALSE);
        (void)KeWaitForSingleObject(c->ThreadObj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(c->ThreadObj);
        c->ThreadObj = NULL;
    }
    if (c->Thread) {
        ZwClose(c->Thread);
        c->Thread = NULL;
    }
    Finish(c);                            /* no-op when the thread already put everything back */
    /* The GPU is reset and its cores are off: nothing reads the work area any more. */
    if (c->Mem) {
        MmFreeContiguousMemory(c->Mem);
        c->Mem = NULL;
    }
    Unmap(c);
    return STATUS_SUCCESS;
}

static NTSTATUS SmDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE dev;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = SmPrepareHardware;
    pnp.EvtDeviceReleaseHardware = SmReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, SM_CTX);
    st = WdfDeviceCreate(&Init, &attr, &dev);
    if (NT_SUCCESS(st))
        RtlZeroMemory(SmGetCtx(dev), sizeof(SM_CTX));
    return st;
}

static BOOLEAN KillFile(PCWSTR Path)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    FILE_NETWORK_OPEN_INFORMATION fi;
    RtlInitUnicodeString(&us, Path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NT_SUCCESS(ZwQueryFullAttributesFile(&oa, &fi));
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    HANDLE h;
    ULONG guard;

    if (KillFile(L"\\SystemRoot\\Starbite\\sbmali.off"))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlInitEmptyUnicodeString(&SmSvc, SmSvcBuf, sizeof(SmSvcBuf));
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&SmSvc, RegistryPath)))
        return STATUS_NAME_TOO_LONG;
    if (NT_SUCCESS(OpenSvc(NULL, &h))) {
        guard = GetDword(h, L"GuardBoots", 0);
        if (guard >= SM_GUARD_LIMIT) {
            PutDword(h, L"GuardRefused", guard);
            (void)ZwFlushKey(h);
            ZwClose(h);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        PutDword(h, L"GuardBoots", guard + 1);
        (void)ZwFlushKey(h);
        ZwClose(h);
    }
    {
        UNICODE_STRING n;
        HANDLE s;
        static const char id[] = SB_BUILD_ID;
        if (NT_SUCCESS(OpenSvc(L"State", &s))) {
            RtlInitUnicodeString(&n, L"BuildId");
            (void)ZwSetValueKey(s, &n, 0, REG_BINARY, (PVOID)id, sizeof(id));
            ZwClose(s);
        }
    }
    WDF_DRIVER_CONFIG_INIT(&cfg, SmDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
