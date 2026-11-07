/* SPDX-License-Identifier: GPL-2.0-only
 * sbthermal.sys: the Starbite temperature watchdog for Windows 11 on the Galaxy S9 (Exynos 9810).
 * docs/WIN11-GPU.md section 8. Stage 2a of track D.
 *
 * A plain (non-PnP) WDM driver, started as a service at boot. Every 250 ms it reads the two TMU
 * blocks (TOP 0x10070000: big and little cores; SUB 0x10074000: GPU and ISP) and runs the level
 * machine of sbth_core.c. The TMU and CMU registers are only read, and the hardware over-temperature
 * trip S-Boot armed at 115 C is only reported, never changed. Its one write path is ACPM (below).
 *
 * CPU boost (docs/WIN11-GPU.md 8.4): the four A55s Windows runs on get Parameters\CpuKhz (default 1690000,
 * what XNU sets; 0 = never touch) at Normal, 1456000 at Warm, S-Boot's 1053000 from Hot up, through ACPM
 * DVFS domain 2 only (a level machine on the hottest probe). sbthermal owns ACPM in Windows: every ACPM
 * transaction, its own and other drivers' (SBTH_CB_ACPM_LOCK/UNLOCK), runs under one mutex.
 *
 * Others see it through
 *   \Callback\StarbiteThermal   ExNotifyCallback(obj, SBTH_CB_QUERY, &state) fills the state;
 *                               registered callbacks get (SBTH_CB_LEVEL, &state) on level changes.
 *                               A GPU driver must not power the GPU when this object is missing or
 *                               a query leaves state.version 0 (the watchdog is gone: the object
 *                               can outlive us while another driver holds a reference).
 *   \\.\StarbiteThermal        IOCTL SBTH_IOCTL_GET_STATE -> struct sbth_state
 *   ...\Services\sbthermal\State  registry values (every 60 s and on level changes, flushed)
 *
 * Boot guard: ...\Services\sbthermal\GuardBoots counts starts that did not reach 10 s of running (the
 * first TMU/CMU reads and the CPU boost request all happen within ~3 s; a crash later in boot is not ours);
 * at 2 the driver refuses to load. Kill switches, outside the registry first (an offline hive edit
 * can be undone by Windows replaying a dirty hive's logs): the file \Windows\Starbite\sbthermal.off
 * (created from TWRP), then Start = 4 (tools/win/hive-service.py disable, clean hives only).
 */
#include <ntddk.h>
#include "sbth_core.h"
#include "../common/sbw_acpm.h"

#define SBTH_TAG 'htBS'
#define SBTH_PERIOD_MS 250
#define SBTH_GUARD_CLEAR_MS 10000        /* was 120 s: bugchecks elsewhere early in boot (the combo2 firmware's PPTT)
                                         * counted against the watchdog, which then refused itself, leaving
                                         * the CPU at 1.05 GHz and every GPU test without its gate */
#define SBTH_STATE_EVERY_MS 60000
#define SBTH_GUARD_LIMIT 2

DRIVER_INITIALIZE DriverEntry;
static DRIVER_DISPATCH SbthCreateClose;
static DRIVER_DISPATCH SbthIoctl;
static KSTART_ROUTINE SbthThread;
static CALLBACK_FUNCTION SbthQueryCallback;

static struct {
    PDEVICE_OBJECT dev;
    BOOLEAN link;
    volatile UCHAR *blk[6];               /* TMU TOP, TMU SUB, CMU_TOP, CMU_CPUCL0 (read only), ACPM SRAM, mailbox */
    KMUTEX acpm_lock;                     /* every ACPM transaction in Windows runs under it */
    struct sbth_levels cpulv;             /* thread only: levels from the hottest probe */
    struct sbth_cpu cpu;                  /* under lock */
    sbth_u32 boost_khz;                   /* Parameters\CpuKhz (default 1690000; 0 = never touch the clock) */
    PCALLBACK_OBJECT cb;
    PVOID cb_reg;
    HANDLE thread;
    PKTHREAD thread_obj;
    KEVENT stop;
    KEVENT started;
    KSPIN_LOCK lock;
    struct sbth_state st;                 /* under lock */
    struct sbth_levels lv;                /* thread only */
    UNICODE_STRING svc;                   /* the service key path */
    WCHAR svc_buf[256];
} g;

/* ---- registry ---------------------------------------------------------------------------- */

static NTSTATUS open_key(PCWSTR sub, HANDLE *h)
{
    WCHAR buf[300];
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    ULONG disp;
    RtlInitEmptyUnicodeString(&path, buf, sizeof(buf));
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&path, &g.svc)))
        return STATUS_NAME_TOO_LONG;
    if (sub && (!NT_SUCCESS(RtlAppendUnicodeToString(&path, L"\\")) ||
                !NT_SUCCESS(RtlAppendUnicodeToString(&path, sub))))
        return STATUS_NAME_TOO_LONG;
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateKey(h, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp);
}

static void put_dword(HANDLE h, PCWSTR name, ULONG v)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    (void)ZwSetValueKey(h, &n, 0, REG_DWORD, &v, sizeof(v));
}

static ULONG get_dword(HANDLE h, PCWSTR name, ULONG dflt)
{
    UNICODE_STRING n;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len;
    RtlInitUnicodeString(&n, name);
    if (NT_SUCCESS(ZwQueryValueKey(h, &n, KeyValuePartialInformation, info, sizeof(buf), &len)) &&
        info->Type == REG_DWORD && info->DataLength == sizeof(ULONG))
        return *(ULONG *)info->Data;
    return dflt;
}

/* Temperatures go to the registry as signed m C in a DWORD (two's complement). */
static void write_state(const struct sbth_state *s, BOOLEAN flush)
{
    HANDLE h;
    WCHAR name[16];
    UNICODE_STRING us;
    if (!NT_SUCCESS(open_key(L"State", &h)))
        return;
    put_dword(h, L"Level", s->level);
    put_dword(h, L"GpuMilliC", (ULONG)s->gpu_mc);
    put_dword(h, L"HottestMilliC", (ULONG)s->hottest_mc);
    put_dword(h, L"MaxGpuMilliC", (ULONG)s->max_gpu_mc);
    put_dword(h, L"MaxAnyMilliC", (ULONG)s->max_any_mc);
    put_dword(h, L"Readings", s->readings);
    put_dword(h, L"Rejected", s->rejected);
    put_dword(h, L"BlindEvents", s->blind_events);
    put_dword(h, L"TripArmed", s->trip_armed);
    put_dword(h, L"TripTopMilliC", (ULONG)s->trip_top_mc);
    put_dword(h, L"TripSubMilliC", (ULONG)s->trip_sub_mc);
    put_dword(h, L"BlocksLive", s->blocks_live);
    put_dword(h, L"LevelChanges", (ULONG)s->changes);
    for (unsigned i = 0; i < SBTH_SLOTS; ++i) {
        RtlInitEmptyUnicodeString(&us, name, sizeof(name));
        if (NT_SUCCESS(RtlIntegerToUnicodeString(i, 10, &us)) &&
            NT_SUCCESS(RtlAppendUnicodeToString(&us, L"_mC"))) {
            name[us.Length / sizeof(WCHAR)] = 0;
            put_dword(h, name, (ULONG)s->mc[i]);
        }
    }
    if (flush)
        (void)ZwFlushKey(h);
    ZwClose(h);
}

/* ---- TMU access ------------------------------------------------------------------------- */

static const ULONG64 blk_pa[6] = { SBTH_TMU_TOP_PA, SBTH_TMU_SUB_PA, SBTH_CMU_TOP_PA, SBTH_CMU_CPUCL0_PA,
                                    SBW_ACPM_SRAM_PA, SBW_ACPM_MBOX_PA };
static const ULONG blk_size[6] = { SBTH_TMU_SIZE, SBTH_TMU_SIZE, SBTH_CMU_SIZE, SBTH_CMU_SIZE, SBW_ACPM_SRAM_SIZE,
                                   SBW_ACPM_MBOX_SIZE };

/* Blocks 0-3 (TMUs, CMUs): read only. */
static sbth_u32 tmu_rd(void *ctx, unsigned block, unsigned off)
{
    UNREFERENCED_PARAMETER(ctx);
    if (block > 3 || off + 4 > blk_size[block] || !g.blk[block])
        return 0;
    return READ_REGISTER_ULONG((volatile ULONG *)(g.blk[block] + off));
}

static void tmu_delay(void *ctx, unsigned us)
{
    UNREFERENCED_PARAMETER(ctx);
    KeStallExecutionProcessor(us);
}

static const struct sbth_io tmu_io = { NULL, tmu_rd, tmu_delay };

/* ---- ACPM: the A55 clock (DVFS domain 2 only) ---------------------------------------------- */

static sbw_u32 acpm_srd(void *ctx, sbw_u32 off)
{
    UNREFERENCED_PARAMETER(ctx);
    return off + 4 <= SBW_ACPM_SRAM_SIZE && g.blk[4] ? READ_REGISTER_ULONG((volatile ULONG *)(g.blk[4] + off)) : 0;
}

static void acpm_barrier(void *ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    __dsb(_ARM64_BARRIER_SY);
}

static void acpm_swr(void *ctx, sbw_u32 off, sbw_u32 v)
{
    if (off + 4 <= SBW_ACPM_SRAM_SIZE && g.blk[4]) {
        acpm_barrier(ctx);
        WRITE_REGISTER_ULONG((volatile ULONG *)(g.blk[4] + off), v);
    }
}

static void acpm_mwr(void *ctx, sbw_u32 off, sbw_u32 v)
{
    /* the mailbox: only INTGR0 (0x08, ring) and INTCR1 (0x20, clear) */
    if ((off == 0x08 || off == 0x20) && g.blk[5]) {
        acpm_barrier(ctx);
        WRITE_REGISTER_ULONG((volatile ULONG *)(g.blk[5] + off), v);
        acpm_barrier(ctx);
    }
}

static const struct sbw_acpm_io acpm_io = { NULL, acpm_srd, acpm_swr, acpm_mwr, tmu_delay, acpm_barrier };

static BOOLEAN acpm_lock(void)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000000LL;             /* 1 s */
    return KeWaitForSingleObject(&g.acpm_lock, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS;
}

static void acpm_unlock(void)
{
    KeReleaseMutex(&g.acpm_lock, FALSE);
}

/* Ask ACPM for the A55 clock; returns TRUE when answered. Domain 2 and the allowed levels only. */
static BOOLEAN set_a55(sbth_u32 khz)
{
    BOOLEAN ok = FALSE;
    if (!sbth_a55_allowed(khz) || !acpm_lock())
        return FALSE;
    ok = sbw_acpm_dvfs(&acpm_io, SBTH_DVFS_A55, khz) != 0;
    acpm_unlock();
    return ok;
}

static void write_cpu(const struct sbth_cpu *c, BOOLEAN flush)
{
    HANDLE h;
    if (!NT_SUCCESS(open_key(L"State", &h)))
        return;
    put_dword(h, L"A55KhzBoot", c->a55_khz_boot);
    put_dword(h, L"A55Khz", c->a55_khz);
    put_dword(h, L"CpuTargetKhz", c->target_khz);
    put_dword(h, L"CpuLevel", c->level);
    put_dword(h, L"CpuRequests", c->requests);
    put_dword(h, L"CpuFailures", c->failures);
    if (flush)
        (void)ZwFlushKey(h);
    ZwClose(h);
}

/* The CPU policy, once per reading: a level machine on the hottest probe; on a new target one DVFS
 * request (after 3 unanswered requests the boost gives up for this boot). */
static void cpu_step(const struct sbth_reading *r)
{
    struct sbth_cpu c;
    KIRQL irql;
    sbth_u32 target;
    BOOLEAN changed = FALSE;

    (void)sbth_levels_step(&g.cpulv, r->hottest_mc, r->hottest_mc);
    target = sbth_cpu_target(g.cpulv.level, g.boost_khz);
    KeAcquireSpinLock(&g.lock, &irql);
    g.cpu.level = g.cpulv.level;
    c = g.cpu;
    KeReleaseSpinLock(&g.lock, irql);
    if (target && target != c.target_khz && c.failures < 3) {
        BOOLEAN ok = set_a55(target);
        sbth_u32 now;
        KeStallExecutionProcessor(1000);  /* let the PLL settle before reading it back */
        now = sbth_a55_khz(&tmu_io);
        KeAcquireSpinLock(&g.lock, &irql);
        g.cpu.requests++;
        if (ok)
            g.cpu.target_khz = target;
        else
            g.cpu.failures++;
        g.cpu.a55_khz = now;
        c = g.cpu;
        KeReleaseSpinLock(&g.lock, irql);
        changed = TRUE;
    }
    if (changed)
        write_cpu(&c, TRUE);
}

/* ---- the watchdog thread ----------------------------------------------------------------- */

static void snapshot(struct sbth_state *out)
{
    KIRQL irql;
    KeAcquireSpinLock(&g.lock, &irql);
    *out = g.st;
    KeReleaseSpinLock(&g.lock, irql);
}

static VOID SbthThread(PVOID ctx)
{
    LARGE_INTEGER period;
    ULONG elapsed = 0, since_state = 0;
    BOOLEAN guard_cleared = FALSE;
    struct sbth_reading r;
    struct sbth_state s;
    UNREFERENCED_PARAMETER(ctx);

    /* The thread object, referenced by the thread itself so DriverEntry can always wait on it. */
    ObReferenceObject(KeGetCurrentThread());
    g.thread_obj = KeGetCurrentThread();
    KeSetEvent(&g.started, IO_NO_INCREMENT, FALSE);

    period.QuadPart = -10000LL * SBTH_PERIOD_MS;
    for (;;) {
        BOOLEAN changed, was_blind = g.lv.blind >= SBTH_BLIND_READINGS;
        KIRQL irql;

        sbth_read_all(&tmu_io, &r);
        changed = sbth_levels_step(&g.lv, r.gpu_mc, r.hottest_mc) != 0;
        cpu_step(&r);

        KeAcquireSpinLock(&g.lock, &irql);
        g.st.level = g.lv.level;
        g.st.gpu_mc = r.gpu_mc;
        g.st.hottest_mc = r.hottest_mc;
        g.st.max_gpu_mc = g.lv.max_gpu_mc;
        g.st.max_any_mc = g.lv.max_any_mc;
        for (unsigned i = 0; i < SBTH_SLOTS; ++i)
            g.st.mc[i] = r.mc[i];
        g.st.readings++;
        g.st.seq++;
        g.st.time_100ns = KeQueryInterruptTime();
        g.st.rejected += r.rejected;
        if (!was_blind && g.lv.blind >= SBTH_BLIND_READINGS)
            g.st.blind_events++;
        if (changed)
            g.st.changes++;
        s = g.st;
        KeReleaseSpinLock(&g.lock, irql);

        if (changed) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "sbthermal: level %u, GPU %d mC, hottest %d mC\n",
                       s.level, s.gpu_mc, s.hottest_mc);
            ExNotifyCallback(g.cb, SBTH_CB_LEVEL, &s);
            write_state(&s, TRUE);
            since_state = 0;
        } else if (since_state >= SBTH_STATE_EVERY_MS) {
            struct sbth_cpu c;
            write_state(&s, FALSE);
            KeAcquireSpinLock(&g.lock, &irql);
            c = g.cpu;
            KeReleaseSpinLock(&g.lock, irql);
            write_cpu(&c, FALSE);
            since_state = 0;
        }

        if (!guard_cleared && elapsed >= SBTH_GUARD_CLEAR_MS) {
            HANDLE h;
            if (NT_SUCCESS(open_key(NULL, &h))) {
                put_dword(h, L"GuardBoots", 0);
                (void)ZwFlushKey(h);
                ZwClose(h);
            }
            guard_cleared = TRUE;
        }

        if (KeWaitForSingleObject(&g.stop, Executive, KernelMode, FALSE, &period) == STATUS_SUCCESS)
            break;
        elapsed += SBTH_PERIOD_MS;
        since_state += SBTH_PERIOD_MS;
    }
    snapshot(&s);
    write_state(&s, TRUE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Registered on our own callback object: answers SBTH_CB_QUERY for any caller. */
static VOID SbthQueryCallback(PVOID ctx, PVOID arg1, PVOID arg2)
{
    KIRQL irql;
    UNREFERENCED_PARAMETER(ctx);
    if (arg1 == SBTH_CB_QUERY && arg2 != NULL) {
        snapshot((struct sbth_state *)arg2);
    } else if (arg1 == SBTH_CB_QUERY_CPU && arg2 != NULL) {
        KeAcquireSpinLock(&g.lock, &irql);
        *(struct sbth_cpu *)arg2 = g.cpu;
        KeReleaseSpinLock(&g.lock, irql);
    } else if (arg1 == SBTH_CB_ACPM_LOCK && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        /* runs in the caller's thread, so the caller owns the mutex until its UNLOCK */
        BOOLEAN ok = acpm_lock();
        if (arg2)
            *(LONG *)arg2 = ok ? 1 : 0;
    } else if (arg1 == SBTH_CB_ACPM_UNLOCK) {
        acpm_unlock();
    }
}

/* ---- device ------------------------------------------------------------------------------ */

static NTSTATUS SbthCreateClose(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS SbthIoctl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    UNREFERENCED_PARAMETER(dev);
    if (sp->Parameters.DeviceIoControl.IoControlCode == SBTH_IOCTL_GET_STATE) {
        if (sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(struct sbth_state)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            snapshot((struct sbth_state *)irp->AssociatedIrp.SystemBuffer);
            info = sizeof(struct sbth_state);
            st = STATUS_SUCCESS;
        }
    }
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* ---- load / unload ----------------------------------------------------------------------- */

static void cleanup(void)
{
    UNICODE_STRING link;
    if (g.thread_obj) {
        KeSetEvent(&g.stop, IO_NO_INCREMENT, FALSE);
        (void)KeWaitForSingleObject(g.thread_obj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g.thread_obj);
        g.thread_obj = NULL;
    }
    if (g.thread) {
        ZwClose(g.thread);
        g.thread = NULL;
    }
    if (g.cb_reg) {
        ExUnregisterCallback(g.cb_reg);
        g.cb_reg = NULL;
    }
    if (g.cb) {
        ObDereferenceObject(g.cb);
        g.cb = NULL;
    }
    if (g.link) {
        RtlInitUnicodeString(&link, L"\\DosDevices\\StarbiteThermal");
        (void)IoDeleteSymbolicLink(&link);
        g.link = FALSE;
    }
    if (g.dev) {
        IoDeleteDevice(g.dev);
        g.dev = NULL;
    }
    for (unsigned b = 0; b < 6; ++b)
        if (g.blk[b]) {
            MmUnmapIoSpace((PVOID)g.blk[b], blk_size[b]);
            g.blk[b] = NULL;
        }
}

/* A kill switch that needs no registry: \SystemRoot\Starbite\<name>.off exists -> do not load. */
static BOOLEAN kill_file(PCWSTR path)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    FILE_NETWORK_OPEN_INFORMATION fi;
    RtlInitUnicodeString(&us, path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NT_SUCCESS(ZwQueryFullAttributesFile(&oa, &fi));
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNICODE_STRING name, link, cbname;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    NTSTATUS st;
    ULONG guard;

    RtlZeroMemory(&g, sizeof(g));
    KeInitializeSpinLock(&g.lock);
    KeInitializeEvent(&g.stop, NotificationEvent, FALSE);
    KeInitializeEvent(&g.started, NotificationEvent, FALSE);
    KeInitializeMutex(&g.acpm_lock, 0);
    RtlInitEmptyUnicodeString(&g.svc, g.svc_buf, sizeof(g.svc_buf));
    st = RtlAppendUnicodeStringToString(&g.svc, reg);
    if (!NT_SUCCESS(st))
        return st;

    if (kill_file(L"\\SystemRoot\\Starbite\\sbthermal.off"))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    /* Boot guard: two starts in a row that did not run 10 s -> stay off. */
    st = open_key(NULL, &h);
    if (!NT_SUCCESS(st))
        return st;
    guard = get_dword(h, L"GuardBoots", 0);
    if (guard >= SBTH_GUARD_LIMIT) {
        put_dword(h, L"GuardRefused", guard);
        (void)ZwFlushKey(h);
        ZwClose(h);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    put_dword(h, L"GuardBoots", guard + 1);
    (void)ZwFlushKey(h);
    ZwClose(h);

    for (unsigned b = 0; b < 6; ++b) {
        PHYSICAL_ADDRESS p;
        p.QuadPart = (LONGLONG)blk_pa[b];
        g.blk[b] = (volatile UCHAR *)MmMapIoSpaceEx(p, blk_size[b], PAGE_READWRITE | PAGE_NOCACHE);
        if (!g.blk[b]) {
            cleanup();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /* The A55 clock as found, and the boost setting (Parameters\CpuKhz). */
    {
        HANDLE ph;
        g.boost_khz = SBTH_A55_MAX_KHZ;
        if (NT_SUCCESS(open_key(L"Parameters", &ph))) {
            g.boost_khz = get_dword(ph, L"CpuKhz", SBTH_A55_MAX_KHZ);
            ZwClose(ph);
        }
        if (g.boost_khz && !sbth_a55_allowed(g.boost_khz))
            g.boost_khz = 0;
        g.cpu.version = 1;
        g.cpu.a55_khz_boot = g.cpu.a55_khz = sbth_a55_khz(&tmu_io);
        sbth_levels_init(&g.cpulv);
        g.cpulv.level = SBTH_STOP;        /* like the GPU: 2 s of cool readings before the boost */
        write_cpu(&g.cpu, TRUE);
    }

    /* What S-Boot left: blocks on, the hardware trip and its temperature. */
    sbth_levels_init(&g.lv);
    g.st.version = SBTH_STATE_VERSION;
    g.st.level = SBTH_STOP;               /* until the first readings say otherwise */
    g.lv.level = SBTH_STOP;
    g.st.blocks_live = (sbth_block_live(&tmu_io, 0) ? 1U : 0U) | (sbth_block_live(&tmu_io, 1) ? 2U : 0U);
    g.st.trip_armed = (sbth_trip_armed(&tmu_io, 0) ? 1U : 0U) | (sbth_trip_armed(&tmu_io, 1) ? 2U : 0U);
    g.st.trip_top_mc = sbth_trip_mc(&tmu_io, 0);
    g.st.trip_sub_mc = sbth_trip_mc(&tmu_io, 1);
    g.st.gpu_mc = g.st.hottest_mc = g.st.max_gpu_mc = g.st.max_any_mc = SBTH_NO_TEMP;
    for (unsigned i = 0; i < SBTH_SLOTS; ++i)
        g.st.mc[i] = SBTH_NO_TEMP;

    RtlInitUnicodeString(&name, L"\\Device\\StarbiteThermal");
    st = IoCreateDevice(drv, 0, &name, SBTH_DEVICE_TYPE, FILE_DEVICE_SECURE_OPEN, FALSE, &g.dev);
    if (!NT_SUCCESS(st)) {
        cleanup();
        return st;
    }
    RtlInitUnicodeString(&link, L"\\DosDevices\\StarbiteThermal");
    g.link = NT_SUCCESS(IoCreateSymbolicLink(&link, &name));

    RtlInitUnicodeString(&cbname, SBTH_CALLBACK_NAME);
    /* OBJ_PERMANENT is required: a named object that is not permanent leaves the namespace when its last
     * handle closes, and ExCreateCallback closes its own handle at once, so nobody else could open it
     * (T5 runs 1-2: sbmali waited 6 min for an object that never appeared while sbthermal ran). Stale
     * answers are caught by the consumers' seq/time checks; sbthermal has no unload routine anyway. */
    InitializeObjectAttributes(&oa, &cbname, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE | OBJ_PERMANENT, NULL, NULL);
    st = ExCreateCallback(&g.cb, &oa, TRUE, TRUE);
    if (!NT_SUCCESS(st)) {
        cleanup();
        return st;
    }
    g.cb_reg = ExRegisterCallback(g.cb, SbthQueryCallback, NULL);
    if (!g.cb_reg) {
        cleanup();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    drv->MajorFunction[IRP_MJ_CREATE] = SbthCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE] = SbthCreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SbthIoctl;
    /* No DriverUnload: the watchdog cannot be stopped while Windows runs (a GPU driver relies on it). */

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&g.thread, THREAD_ALL_ACCESS, &oa, NULL, NULL, SbthThread, NULL);
    if (!NT_SUCCESS(st)) {
        cleanup();
        return st;
    }
    (void)KeWaitForSingleObject(&g.started, Executive, KernelMode, FALSE, NULL);
    write_state(&g.st, TRUE);
    return STATUS_SUCCESS;
}
