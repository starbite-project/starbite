/* SPDX-License-Identifier: GPL-2.0-only
 * sbwdog: the hardware dead-man for unattended Windows boots on the Galaxy S9 (docs/WIN11-GPU.md 6.17).
 *
 * The loader arms cluster 1's S3C2410-type watchdog (0x10060000, shim/starlte/wdt.c) right before UEFI starts,
 * for catalog rows with wdt = N and wdt_feed = "own". Nothing in Windows knows it, so without a feeder the
 * phone resets N seconds after the loader. This boot-start driver feeds it from a real-time system thread
 * (WTCNT = WTDAT every PeriodMs, default 5 s). If Windows hangs hard (all CPUs stuck at raised IRQL, an
 * interrupt storm, a bugcheck that never reboots), the feeding stops and the SoC resets back to the loader.
 *
 * Unattended mode (Parameters\Unattended = 1, set by tools/win/gpu-cycle.sh and cleared at its end) bounds a
 * boot that stays alive but useless (a black desktop):
 *   - at RestartSec a clean restart (NtShutdownSystem(ShutdownReboot) from a second thread: drivers get
 *     IRP_MJ_SHUTDOWN, the hives and NTFS are flushed and closed), while the feeder keeps feeding;
 *   - at CapSec the feeder stops: the watchdog resets the SoC within its timeout, whatever state Windows is in.
 * The times depend on sbwddm's mode this boot (Services\sbwddm\State\WddmMode, read at 60 s):
 *   full mode (a GPU test)  FullRestartSec 300, FullCapSec 360;
 *   display-only            DisplayRestartSec 120, DisplayCapSec 180 (the clean boot after a test).
 * Without Unattended it only feeds, for as long as Windows runs.
 *
 * It never arms the watchdog by itself unless Parameters\ArmSec (10..82) is set and the loader did not arm
 * it; then it writes the loader's own sequence (stop, WTDAT/WTCNT, WTCON 0xFF39, PMU unmask bit 23).
 *
 * State (Services\sbwdog\State, flushed): ArmedAtStart, WtCon, WtDat, PmuDisable, PmuMask, ArmedByUs,
 * Mode (0 display-only, 1 full, 2 unknown), Seconds, Feeds, RestartAt, RestartStatus, CapAt.
 * Kill file: C:\Windows\Starbite\sbwdog.off: no feeding at all (the loader's watchdog, if armed, then resets
 * the phone N seconds after the loader: only for testing the dead-man itself).
 */
#include <ntddk.h>

#define WDT1_PA 0x10060000ULL
#define PMU_PA 0x14060000ULL
#define WTCON 0x00U
#define WTDAT 0x04U
#define WTCNT 0x08U
#define WTCLRINT 0x0CU
#define PMU_WDT_DISABLE 0x408U
#define PMU_WDT_MASK 0x40CU
#define WDT1_PMU_BIT 23U
#define WDT_WTCON 0xFF39U                 /* prescaler 255, DIV128, ENABLE, RSTEN: 793.457 Hz, <= 82.6 s */
#define WDT_HZ_X1000 793457ULL            /* counts per 1000 s */
#define SBWD_TAG 'dwbS'

typedef enum _SHUTDOWN_ACTION { ShutdownNoReboot, ShutdownReboot, ShutdownPowerOff } SHUTDOWN_ACTION;
NTSYSAPI NTSTATUS NTAPI NtShutdownSystem(SHUTDOWN_ACTION Action);

DRIVER_INITIALIZE DriverEntry;
static KSTART_ROUTINE FeedThread, RestartThread;

static struct {
    volatile UCHAR *Wdt, *Pmu;
    BOOLEAN PmuWritable;
    WCHAR SvcBuf[256];
    UNICODE_STRING Svc;
} g;

/* ---- registry: this service's key, and one absolute read ---------------------------------------------- */

static NTSTATUS OpenSub(PCWSTR Sub, HANDLE *H)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING sub;
    HANDLE root;
    NTSTATUS st;
    InitializeObjectAttributes(&oa, &g.Svc, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwOpenKey(&root, KEY_READ | KEY_WRITE, &oa);
    if (!NT_SUCCESS(st))
        return st;
    RtlInitUnicodeString(&sub, Sub);
    InitializeObjectAttributes(&oa, &sub, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, root, NULL);
    st = ZwCreateKey(H, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    ZwClose(root);
    return st;
}

static ULONG QueryDword(HANDLE H, PCWSTR Name, ULONG Dflt)
{
    UNICODE_STRING n;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len;
    RtlInitUnicodeString(&n, Name);
    if (NT_SUCCESS(ZwQueryValueKey(H, &n, KeyValuePartialInformation, v, sizeof(buf), &len)) &&
        v->Type == REG_DWORD && v->DataLength == sizeof(ULONG))
        return *(ULONG *)v->Data;
    return Dflt;
}

static ULONG Param(PCWSTR Name, ULONG Dflt)
{
    HANDLE h;
    ULONG r;
    if (!NT_SUCCESS(OpenSub(L"Parameters", &h)))
        return Dflt;
    r = QueryDword(h, Name, Dflt);
    ZwClose(h);
    return r;
}

static void State(PCWSTR Name, ULONG Value, BOOLEAN Flush)
{
    HANDLE h;
    UNICODE_STRING n;
    if (!NT_SUCCESS(OpenSub(L"State", &h)))
        return;
    RtlInitUnicodeString(&n, Name);
    (void)ZwSetValueKey(h, &n, 0, REG_DWORD, &Value, sizeof(Value));
    if (Flush)
        (void)ZwFlushKey(h);
    ZwClose(h);
}

/* sbwddm's mode this boot: 1 full, 0 display-only, 2 unknown (no sbwddm). */
static ULONG WddmMode(void)
{
    UNICODE_STRING k = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\sbwddm\\State");
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    ULONG m;
    InitializeObjectAttributes(&oa, &k, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa)))
        return 2;
    m = QueryDword(h, L"WddmMode", 2);
    ZwClose(h);
    return m > 1 ? 2 : m;
}

static BOOLEAN FileExists(PCWSTR Path)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    FILE_NETWORK_OPEN_INFORMATION fi;
    RtlInitUnicodeString(&us, Path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NT_SUCCESS(ZwQueryFullAttributesFile(&oa, &fi));
}

/* ---- the watchdog ------------------------------------------------------------------------------------- */

static ULONG Rd(volatile UCHAR *B, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(B + Off));
}

static void Wr(volatile UCHAR *B, ULONG Off, ULONG V)
{
    __dsb(_ARM64_BARRIER_SY);
    WRITE_REGISTER_ULONG((volatile ULONG *)(B + Off), V);
    __dsb(_ARM64_BARRIER_SY);
}

static BOOLEAN Armed(void)
{
    ULONG con = Rd(g.Wdt, WTCON);
    ULONG bit = 1U << WDT1_PMU_BIT;
    return (con & 0x21U) == 0x21U && g.Pmu != NULL && !(Rd(g.Pmu, PMU_WDT_DISABLE) & bit) &&
           !(Rd(g.Pmu, PMU_WDT_MASK) & bit);
}

static void Feed(void)
{
    Wr(g.Wdt, WTCNT, Rd(g.Wdt, WTDAT));
}

/* The loader's sequence (shim/starlte/wdt.c wdt_arm), for ArmSec when nothing armed it. */
static BOOLEAN Arm(ULONG Secs)
{
    ULONG64 c;
    if (Secs < 10 || Secs > 82 || !g.PmuWritable)
        return FALSE;
    c = (ULONG64)Secs * WDT_HZ_X1000 / 1000ULL;
    if (c > 0xFFFF)
        c = 0xFFFF;
    Wr(g.Wdt, WTCON, 0);
    Wr(g.Wdt, WTDAT, (ULONG)c);
    Wr(g.Wdt, WTCNT, (ULONG)c);
    Wr(g.Wdt, WTCLRINT, 0);
    Wr(g.Wdt, WTCON, WDT_WTCON);
    Wr(g.Pmu, PMU_WDT_DISABLE, Rd(g.Pmu, PMU_WDT_DISABLE) & ~(1U << WDT1_PMU_BIT));
    Wr(g.Pmu, PMU_WDT_MASK, Rd(g.Pmu, PMU_WDT_MASK) & ~(1U << WDT1_PMU_BIT));
    return Armed();
}

/* ---- threads ------------------------------------------------------------------------------------------ */

static VOID RestartThread(PVOID Ctx)
{
    NTSTATUS st;
    UNREFERENCED_PARAMETER(Ctx);
    /* Does not return when it works. The feeder keeps feeding meanwhile: a shutdown that hangs is ended by
     * the cap. */
    st = NtShutdownSystem(ShutdownReboot);
    State(L"RestartStatus", (ULONG)st, TRUE);
    PsTerminateSystemThread(st);
}

static VOID FeedThread(PVOID Ctx)
{
    ULONG period = Param(L"PeriodMs", 5000);
    ULONG unattended = Param(L"Unattended", 0);
    ULONG restartSec = 0, capSec = 0, mode = 2, secs = 0, feeds = 0;
    BOOLEAN feeding = TRUE, restarted = FALSE, decided = FALSE, armed;
    ULONG64 t0 = KeQueryInterruptTime();
    LARGE_INTEGER t;
    UNREFERENCED_PARAMETER(Ctx);

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY + 14);
    if (period < 500 || period > 10000)
        period = 5000;
    armed = Armed();
    for (;;) {
        ULONG elapsed;
        if (feeding && armed)
            Feed(), feeds++;
        t.QuadPart = -10000LL * period;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        elapsed = (ULONG)((KeQueryInterruptTime() - t0) / 10000000ULL);
        if (!decided && elapsed >= 60) {
            /* sbwddm has started by now in every boot that has it */
            decided = TRUE;
            mode = WddmMode();
            restartSec = Param(mode == 1 ? L"FullRestartSec" : L"DisplayRestartSec", mode == 1 ? 300 : 120);
            capSec = Param(mode == 1 ? L"FullCapSec" : L"DisplayCapSec", mode == 1 ? 360 : 180);
            State(L"Mode", mode, TRUE);
            State(L"Unattended", unattended, TRUE);
        }
        if (elapsed / 10 != secs / 10) {  /* every 10 s, to disk every 30 s */
            State(L"Feeds", feeds, FALSE);
            State(L"Seconds", elapsed, elapsed / 30 != secs / 30);
        }
        secs = elapsed;
        if (!unattended || !decided)
            continue;
        if (restartSec && !restarted && elapsed >= restartSec) {
            OBJECT_ATTRIBUTES oa;
            HANDLE th;
            restarted = TRUE;
            State(L"RestartAt", elapsed, TRUE);
            InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
            if (NT_SUCCESS(PsCreateSystemThread(&th, THREAD_ALL_ACCESS, &oa, NULL, NULL, RestartThread, NULL)))
                ZwClose(th);
        }
        if (capSec && feeding && elapsed >= capSec) {
            feeding = FALSE;              /* the watchdog resets the SoC within its timeout */
            State(L"CapAt", elapsed, TRUE);
        }
    }
}

/* ---- entry -------------------------------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING Reg)
{
    PHYSICAL_ADDRESS pa;
    OBJECT_ATTRIBUTES oa;
    HANDLE th;
    ULONG armSec;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(Drv);

    RtlInitEmptyUnicodeString(&g.Svc, g.SvcBuf, sizeof(g.SvcBuf));
    (void)RtlAppendUnicodeStringToString(&g.Svc, Reg);
    pa.QuadPart = (LONGLONG)WDT1_PA;
    g.Wdt = (volatile UCHAR *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    armSec = Param(L"ArmSec", 0);
    pa.QuadPart = (LONGLONG)PMU_PA;
    g.PmuWritable = armSec != 0;          /* the PMU is written only to arm (ArmSec) */
    g.Pmu = (volatile UCHAR *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_NOCACHE | (g.PmuWritable ? PAGE_READWRITE : PAGE_READONLY));
    if (g.Wdt == NULL || g.Pmu == NULL) {
        State(L"MapFailed", (g.Wdt == NULL) | (g.Pmu == NULL) << 1, TRUE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    {
        /* T13i: how long Windows took to get here, the margin every armed boot depends on.
         * GapMsAtStart: since the loader armed the watchdog (its 16-bit count, valid only while it has not run
         * out, i.e. < WTDAT / 793.457 Hz = 82 s); UptimeMsAtStart: since the SoC's reset by the generic timer
         * (S-Boot and the loader included, so an upper bound of the gap even in a boot nothing armed). */
        ULONG64 cnt = _ReadStatusReg(ARM64_CNTVCT), frq = _ReadStatusReg(ARM64_CNTFRQ_EL0);
        ULONG dat = Rd(g.Wdt, WTDAT), now = Rd(g.Wdt, WTCNT);
        State(L"CntFrq", (ULONG)frq, FALSE);
        State(L"UptimeMsAtStart", frq ? (ULONG)(cnt * 1000ULL / frq) : 0, FALSE);
        State(L"GapMsAtStart", Armed() && dat >= now ? (ULONG)((ULONG64)(dat - now) * 1000000ULL / WDT_HZ_X1000) : 0,
              FALSE);
        State(L"WtCntAtStart", now, FALSE);
    }
    State(L"ArmedAtStart", Armed(), FALSE);
    State(L"WtCon", Rd(g.Wdt, WTCON), FALSE);
    State(L"WtDat", Rd(g.Wdt, WTDAT), FALSE);
    State(L"PmuDisable", Rd(g.Pmu, PMU_WDT_DISABLE), FALSE);
    State(L"PmuMask", Rd(g.Pmu, PMU_WDT_MASK), FALSE);
    State(L"RestartAt", 0, FALSE);
    State(L"CapAt", 0, FALSE);
    State(L"Mode", 2, FALSE);
    if (!Armed() && armSec)
        State(L"ArmedByUs", Arm(armSec), FALSE);
    if (FileExists(L"\\SystemRoot\\Starbite\\sbwdog.off")) {
        State(L"KillFile", 1, TRUE);
        return STATUS_SUCCESS;            /* loaded, feeds nothing */
    }
    State(L"KillFile", 0, TRUE);
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, &oa, NULL, NULL, FeedThread, NULL);
    if (!NT_SUCCESS(st))
        return st;
    ZwClose(th);
    /* No unload: the feeder runs as long as Windows does. */
    return STATUS_SUCCESS;
}
