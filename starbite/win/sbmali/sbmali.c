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
 * Boot guard Services\sbmali\GuardBoots: armed (+1, flushed) only right before the first GPU access and
 * cleared after everything is put back, so waiting for the watchdog never counts; at 2 the driver does not
 * load. At power-down (D0Exit, also at shutdown) the thread is stopped and puts everything back. Kill file
 * \Windows\Starbite\sbmali.off; Start = 4 via tools/win/hive-service.py on a clean hive.
 */
#include <ntddk.h>
#include <wdf.h>
#include "sbm_core.h"
#include "../sbthermal/sbth_core.h"

/* T8's triangle: XNU's Mesa-built blob (proven on this phone under iOS). The generated header uses uint32_t. */
typedef unsigned int uint32_t;
#include "../../../xnu/src/starlte_gpu_tri.h"
#define SM_TRI_RT_STRIDE 5760U            /* the blob draws into the panel's layout: rows of 1440 pixels */
#define SM_TRI_RT_VA (0x4000000ULL + (ULONG64)SG_TRI_RT_Y * SM_TRI_RT_STRIDE + SG_TRI_RT_X * 4U)
#define SM_TRI_RT_LO (SM_TRI_RT_VA & ~0xfffULL)
#define SM_TRI_RT_HI ((SM_TRI_RT_VA + (SG_TRI_FB_H - 1ULL) * SM_TRI_RT_STRIDE + SG_TRI_FB_W * 4ULL + 0xfffULL) & ~0xfffULL)

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
    BOOLEAN GpuClockRaised;               /* a DVFS request above S-Boot's 260 MHz is in effect */
    ULONG GpuKhzNow;
    PMDL HeapMdl, RtMdl;                  /* T8: the blob's tiler heap (16 MiB) and its render-target window */
    PVOID RtVa;                           /* CPU view of the render-target window (uncached) */
    UCHAR BuckOld;
    BOOLEAN GpuTouched;
    PCALLBACK_OBJECT Cb;                  /* \Callback\StarbiteThermal, opened by the thread */
    HANDLE Thread;
    PKTHREAD ThreadObj;
    KEVENT Started, Stop;
} SM_CTX, *PSM_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SM_CTX, SmGetCtx)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD SmDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE SmPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE SmReleaseHardware;
static EVT_WDF_DEVICE_D0_EXIT SmD0Exit;
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

/* 0 = the watchdog answers and is alive (its last reading is fresh); otherwise a reason code:
 *   1 \Callback\StarbiteThermal does not exist (sbthermal not loaded yet, or not installed)
 *   2 the object exists but nothing answers the query (sbthermal gone)
 *   3 the last reading is older than 1 s (the watchdog thread is not running)
 * *Level = its level (enum sbth_level). State\ThermalGate = this code, or 10 + level when alive but not Normal. */
static ULONG ThermalGate(PCALLBACK_OBJECT *Cb, struct sbth_state *Last, ULONG *Level)
{
    struct sbth_state s;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;

    *Level = SBTH_CRITICAL;
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
    if (KeQueryInterruptTime() - s.time_100ns > 10000000ULL)
        return 3;                         /* the time is written with every reading: stale = not running */
    *Last = s;
    *Level = s.level;
    return 0;
}

static ULONG GateCode(ULONG Alive, ULONG Level)
{
    return Alive ? Alive : Level == SBTH_NORMAL ? 0 : 10 + Level;
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
                                    L"LastAsAddrLo", L"MaxJobUs", L"TriOk", L"TriPx", L"TriMismatch",
                                    L"TriBadPx" };
    ULONG v[17] = { r->runs, r->write_ok, r->clear_ok, r->faults, r->timeouts, r->reset_fail, r->power_fail,
                    r->as_fail, r->last_js_status, r->last_as_fault, r->last_gpu_fault, (ULONG)r->last_as_addr,
                    r->max_us, r->tri_ok, r->tri_px_first, r->tri_mismatch, r->tri_bad_px };
    for (unsigned i = 0; i < 17; i++) {
        WCHAR n[48];
        UNICODE_STRING us;
        RtlInitEmptyUnicodeString(&us, n, sizeof(n) - sizeof(WCHAR));
        if (NT_SUCCESS(RtlAppendUnicodeToString(&us, Prefix)) && NT_SUCCESS(RtlAppendUnicodeToString(&us, names[i]))) {
            n[us.Length / sizeof(WCHAR)] = 0;
            Log(n, v[i], FALSE);
        }
    }
}

/* sbthermal owns ACPM: take its lock around every ACPM sequence. A watchdog that predates the lock (it
 * answers no CPU query, and never touches ACPM itself) needs none. TRUE = go ahead. */
static BOOLEAN AcpmLock(PSM_CTX c, BOOLEAN *Locked)
{
    struct sbth_cpu cpu;
    LONG ok = 0;
    *Locked = FALSE;
    if (!c->Cb)
        return FALSE;
    RtlZeroMemory(&cpu, sizeof(cpu));
    ExNotifyCallback(c->Cb, SBTH_CB_QUERY_CPU, &cpu);
    if (cpu.version == 0)
        return TRUE;                      /* old sbthermal: no other ACPM user in Windows */
    ExNotifyCallback(c->Cb, SBTH_CB_ACPM_LOCK, &ok);
    *Locked = ok != 0;
    return *Locked;
}

static void AcpmUnlock(PSM_CTX c, BOOLEAN Locked)
{
    if (Locked)
        ExNotifyCallback(c->Cb, SBTH_CB_ACPM_UNLOCK, NULL);
}

/* The GPU clock through ACPM DVFS domain 4 (the GPU; sbw_acpm refuses anything but 2, 3 and 4), BUCK6 back
 * to 950 mV after it, then PLL_G3D read back: it must land on the request (within 1 %). Returns 0 when it
 * did; logs GpuKhzReq / GpuPllKhz / GpuDvfs. */
static int SetGpuClock(PSM_CTX c, ULONG Khz)
{
    BOOLEAN locked;
    int rc;
    ULONG pll;
    if (!AcpmLock(c, &locked))
        return -10;
    rc = sbm_gpu_dvfs(&c->Io, Khz);
    AcpmUnlock(c, locked);
    KeStallExecutionProcessor(1000);
    pll = sbm_gpu_pll_khz(&c->Io);
    Log(L"GpuKhzReq", Khz, FALSE);
    Log(L"GpuPllKhz", pll, FALSE);
    Log(L"GpuDvfs", (ULONG)rc, TRUE);
    if (rc)
        return rc;
    c->GpuKhzNow = Khz;
    c->GpuClockRaised = Khz != 260000;
    return (pll * 100 >= Khz * 99 && pll * 100 <= Khz * 101) ? 0 : -20;
}

static void Finish(PSM_CTX c)
{
    if (c->GpuTouched) {
        (void)sbm_soft_reset(&c->Io, NULL);   /* address spaces and job slots back to idle, cores off */
        sbm_power_off(&c->Io);
        c->GpuTouched = FALSE;
    }
    if (c->GpuClockRaised)
        Log(L"GpuClockRestore", (ULONG)SetGpuClock(c, 260000), TRUE);   /* S-Boot's clock, before BUCK6 */
    if (c->BuckRaised) {
        BOOLEAN locked;
        if (AcpmLock(c, &locked)) {
            Log(L"BuckRestore", (ULONG)sbm_buck6_restore(&c->Io, c->BuckOld), TRUE);
            AcpmUnlock(c, locked);
        } else {
            Log(L"BuckRestore", 0xfffffffeU, TRUE);   /* lock not taken: S-Boot resets BUCK6 at reboot */
        }
        c->BuckRaised = FALSE;
    }
}

/* T8: the blob's render-target window and tiler heap, page by page from their MDLs (no-execute). */
static int MapMdl(struct sbm_pt *Pt, ULONG64 Va, PMDL Mdl)
{
    PPFN_NUMBER pfn = MmGetMdlPfnArray(Mdl);
    ULONG n = MmGetMdlByteCount(Mdl) / PAGE_SIZE;
    for (ULONG i = 0; i < n; i++)
        if (sbm_pt_map(Pt, Va + (ULONG64)i * PAGE_SIZE, (ULONG64)pfn[i] << PAGE_SHIFT, 1, 1))
            return -1;
    return 0;
}

static int MapExtra(void *Ctx, struct sbm_pt *Pt)
{
    PSM_CTX c = (PSM_CTX)Ctx;
    return MapMdl(Pt, SM_TRI_RT_LO, c->RtMdl) || MapMdl(Pt, SG_TRI_HEAP_VA, c->HeapMdl) ? -1 : 0;
}

static VOID SmThread(PVOID Ctx)
{
    PSM_CTX c = (PSM_CTX)Ctx;
    struct sbth_state last;
    struct sbm_info inf;
    ULONG gate = 1, runs, formats, level = SBTH_CRITICAL, gpukhz, throttles = 0;
    struct sbm_tri tri, *ptri = NULL;
    LARGE_INTEGER t;
    HANDLE h;
    int pr, br;

    ObReferenceObject(KeGetCurrentThread());
    c->ThreadObj = KeGetCurrentThread();
    KeSetEvent(&c->Started, IO_NO_INCREMENT, FALSE);
    RtlZeroMemory(&last, sizeof(last));

    runs = Param(L"Runs", 200);
    formats = Param(L"Formats", 3);
    gpukhz = Param(L"GpuKhz", 572000);   /* 0: stay at S-Boot's 260 MHz */
    if (runs > 1000)
        runs = 1000;
    if (c->HeapMdl && c->RtMdl && c->RtVa && Param(L"Triangle", 1)) {
        tri.blob = sg_tri_blob;
        tri.words = SG_TRI_BLOB_WORDS;
        tri.blob_va = SG_TRI_BLOB_VA;
        tri.tiler_job = SG_TRI_TILER_JOB;
        tri.frag_job = SG_TRI_FRAG_JOB;
        tri.rt_va = SM_TRI_RT_VA;
        tri.rt_stride = SM_TRI_RT_STRIDE;
        tri.w = SG_TRI_FB_W;
        tri.h = SG_TRI_FB_H;
        tri.rt_cpu = (volatile sbm_u32 *)((PUCHAR)c->RtVa + (SM_TRI_RT_VA - SM_TRI_RT_LO));
        tri.clear = SG_TRI_CLEAR;
        tri.color = SG_TRI_COLOR;
        tri.map_extra = MapExtra;
        tri.ctx = c;
        ptri = &tri;
    }
    Log(L"Triangle", ptri != NULL, FALSE);

    /* 1. The watchdog. PnP starts this device early in boot; sbthermal (an auto-start service) is loaded by
     * the Service Control Manager much later (T5 run 1: about 75 s after boot began, and this driver gave
     * up after 30 s). Wait up to SM_GATE_WAIT_MS, reporting progress every 10 s. Nothing is touched
     * before the gate opens. */
    Step(1);
    {
        ULONG waited = 0, tries = 0;
        for (;;) {
            gate = GateCode(ThermalGate(&c->Cb, &last, &level), level);
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
    /* Arm the boot guard: from here on a crash counts (T5 rounds 1-3: guard counted waits that a shutdown
     * interrupted, and the driver refused itself). */
    if (NT_SUCCESS(OpenSvc(NULL, &h))) {
        PutDword(h, L"GuardBoots", GetDword(h, L"GuardBoots", 0) + 1);
        (void)ZwFlushKey(h);
        ZwClose(h);
    }
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
    {
        BOOLEAN locked;
        if (!AcpmLock(c, &locked)) {
            Log(L"AcpmLock", 0, TRUE);
            goto out;
        }
        br = sbm_buck6_raise(&c->Io, &c->BuckOld);
        AcpmUnlock(c, locked);
    }
    c->BuckRaised = br == 0 || br == -3;  /* -3: written but not read back as asked: restore anyway */
    Log(L"BuckOld", c->BuckOld, FALSE);
    Log(L"BuckRaise", (ULONG)br, TRUE);
    if (br)
        goto out;

    /* 3b. The GPU clock (T8): domain 4, cross-checked on PLL_G3D; a miss puts 260 MHz back and stops. */
    if (gpukhz) {
        int g = SetGpuClock(c, gpukhz);
        if (g) {
            Log(L"GpuClockMismatch", (ULONG)g, TRUE);
            goto out;
        }
    }

    /* 4. The runs. */
    for (int f = 0; f < 2; f++) {
        struct sbm_test_result r;
        if (!(formats & (1U << f)))
            continue;
        Step(4 + f);
        RtlZeroMemory(&r, sizeof(r));
        for (ULONG i = 0; i < runs; i++) {
            ULONG want;
            gate = ThermalGate(&c->Cb, &last, &level);
            if (gate || level >= SBTH_STOP) {
                gate = GateCode(gate, level);
                Log(L"ThermalStop", gate, TRUE);
                break;
            }
            /* Throttle: Warm -> 338 MHz, Hot -> 260 MHz (never above the request). */
            want = level == SBTH_HOT ? 260000 : level == SBTH_WARM ? 338000 : gpukhz;
            if (gpukhz && want > gpukhz)
                want = gpukhz;
            if (gpukhz && want != c->GpuKhzNow) {
                throttles++;
                if (SetGpuClock(c, want)) {
                    gate = 30;
                    Log(L"ThermalStop", gate, TRUE);
                    break;
                }
            }
            c->GpuTouched = TRUE;
            (void)sbm_selftest_run(&c->Io, (enum sbm_fmt)f, inf.shader_present, c->Mem, (sbm_u64)c->MemPa.QuadPart, ptri, &r);
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
    Log(L"GpuMaxMilliC", (ULONG)last.max_gpu_mc, FALSE);
    Log(L"Throttles", throttles, FALSE);
    Log(L"Done", 1, TRUE);
out:
    Step(6);
    Finish(c);
    /* Reaching this point means no crash: clear the boot guard (also when it was never armed). */
    if (NT_SUCCESS(OpenSvc(NULL, &h))) {
        PutDword(h, L"GuardBoots", 0);
        (void)ZwFlushKey(h);
        ZwClose(h);
    }
    Step(7);
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

/* Map the fixed SoC blocks (the Mali too when Standalone: no ACPI resources then), allocate the work memory,
 * start the test thread. Shared by the PnP path (PrepareHardware) and the standalone service. */
static NTSTATUS StartHw(PSM_CTX c, BOOLEAN Standalone)
{
    PHYSICAL_ADDRESS lo, hi, skip;
    OBJECT_ATTRIBUTES oa;

    if (Standalone) {
        c->Mmio[SBM_GPU] = (volatile UCHAR *)MapPa(SBM_GPU_PA, SBM_GPU_SIZE);
        c->MmioSize[SBM_GPU] = SBM_GPU_SIZE;
    }
    Log(L"Standalone", Standalone, FALSE);
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

    /* T8: tiler heap and render-target window, any pages below 2^40, uncached. Optional: without them the
     * test runs without the triangle. */
    {
        PHYSICAL_ADDRESS l = { 0 }, hgh, sk = { 0 };
        hgh.QuadPart = 0xffffffffffLL;
        c->HeapMdl = MmAllocatePagesForMdlEx(l, hgh, sk, SG_TRI_HEAP_BYTES, MmNonCached, MM_ALLOCATE_FULLY_REQUIRED);
        c->RtMdl = MmAllocatePagesForMdlEx(l, hgh, sk, (SIZE_T)(SM_TRI_RT_HI - SM_TRI_RT_LO), MmNonCached,
                                           MM_ALLOCATE_FULLY_REQUIRED);
        if (c->RtMdl)
            c->RtVa = MmMapLockedPagesSpecifyCache(c->RtMdl, KernelMode, MmNonCached, NULL, FALSE,
                                                   NormalPagePriority | MdlMappingNoExecute);
        Log(L"TriAlloc", (c->HeapMdl ? 1u : 0u) | (c->RtMdl ? 2u : 0u) | (c->RtVa ? 4u : 0u), FALSE);
    }
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

static NTSTATUS SmPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSM_CTX c = SmGetCtx(Device);
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
    return StartHw(c, FALSE);
}

/* Stop the test thread; it puts the GPU and BUCK6 back and clears the guard before it ends. */
static void StopThread(PSM_CTX c)
{
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
}

/* Power-down, including system shutdown (KMDF does not release hardware then). */
static NTSTATUS SmD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    StopThread(SmGetCtx(Device));
    return STATUS_SUCCESS;
}

static void StopHw(PSM_CTX c)
{
    StopThread(c);
    Finish(c);                            /* no-op when the thread already put everything back */
    if (c->Cb) {
        ObDereferenceObject(c->Cb);
        c->Cb = NULL;
    }
    /* The GPU is reset and its cores are off: nothing reads the work area any more. */
    if (c->Mem) {
        MmFreeContiguousMemory(c->Mem);
        c->Mem = NULL;
    }
    if (c->RtVa) {
        MmUnmapLockedPages(c->RtVa, c->RtMdl);
        c->RtVa = NULL;
    }
    if (c->RtMdl) {
        MmFreePagesFromMdl(c->RtMdl);
        ExFreePool(c->RtMdl);
        c->RtMdl = NULL;
    }
    if (c->HeapMdl) {
        MmFreePagesFromMdl(c->HeapMdl);
        ExFreePool(c->HeapMdl);
        c->HeapMdl = NULL;
    }
    Unmap(c);
}

static NTSTATUS SmReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    StopHw(SmGetCtx(Device));
    return STATUS_SUCCESS;
}

/* Standalone service (Services\sbmali\Start 1 or 2, e.g. tools/win/hive-service.py add): no device, so it
 * coexists with sbdisplay bound to ACPI\STRB0072. The test runs once per load; unload puts everything back.
 * (No shutdown notification: a shutdown during the ~40 s of runs leaves the guard armed once.) */
static SM_CTX SmStandalone;

static VOID SmUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    StopHw(&SmStandalone);
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
    pnp.EvtDeviceD0Exit = SmD0Exit;
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
    ULONG guard, start = 3;

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
        ZwClose(h);                       /* armed later, right before the first GPU access */
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
    if (NT_SUCCESS(OpenSvc(NULL, &h))) {
        start = GetDword(h, L"Start", 3);
        ZwClose(h);
    }
    if (start <= 2) {                     /* boot/system/auto: the standalone service */
        NTSTATUS st;
        WDF_DRIVER_CONFIG_INIT(&cfg, WDF_NO_EVENT_CALLBACK);
        cfg.DriverInitFlags |= WdfDriverInitNonPnpDriver;
        cfg.EvtDriverUnload = SmUnload;
        st = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
        if (NT_SUCCESS(st))
            st = StartHw(&SmStandalone, TRUE);
        return st;
    }
    WDF_DRIVER_CONFIG_INIT(&cfg, SmDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
