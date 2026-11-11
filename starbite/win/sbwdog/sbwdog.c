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
 * The loader arms it, but on the phone something before Windows' boot-start drivers stops it again (T13i
 * --measure: WTCON 0, WTDAT the loader's 0xFE27 = 82 s, WTCNT held at WTDAT; most likely the woa UEFI). So when
 * sbwdog finds it stopped in an Unattended boot, it arms it itself (Parameters\ArmSec, default 82; 0 = never;
 * never in a boot without Unattended: the user's own boots stay unarmed, as the loader leaves them) with the loader's own
 * sequence: stop, WTDAT/WTCNT, WTCON 0xFF39, and bit 23 of the PMU's disable/mask words cleared only if set.
 * If anything stops it later, the feeder re-arms it (State\Rearms). Known gap: a hang before sbwdog starts
 * (UEFI, boot manager, winload, early kernel) is not covered on this firmware; only a UEFI-side feeder would.
 *
 * State (Services\sbwdog\State, flushed): ArmedAtStart, WtCon, WtDat, WtCntAtStart, PmuDisable, PmuMask,
 * GapMsAtStart, UptimeMsAtStart, CntFrq, ArmedBySelf, ArmedNow, Rearms,
 * Mode (0 display-only, 1 full, 2 unknown), Seconds, Feeds, RestartAt, RestartStatus, CapAt.
 * Dead-man test (gpu-cycle.sh --deadman-test): Parameters\DeadmanTest = 1 (one-shot: cleared, flushed, at start)
 * or the kill file C:\Windows\Starbite\sbwdog.off. The watchdog is armed and never fed, and nothing else runs
 * (no restart, no cap): the phone must reset ~82 s after sbwdog starts, which the loader then reports as
 * HANG (WATCHDOG). WtCon/WtCnt are written (flushed) right after arming and at 10 s and 20 s, to show the counter
 * runs. The kill file is looked for during the first 60 s: T13j checked it in DriverEntry, where a boot-start
 * driver cannot see files yet, and the test boot restarted cleanly instead.
 *
 * T35, the crash ledger (sbcl_core.h, docs/WIN11-GPU.md 6.45): every boot, user boots included. The page
 * 0xFED13C00 (ramoops zone 0's last KiB, T39; 0xFED1E000 in T35-T38 did not survive) survives warm resets; sbwdog marks the boot running, keeps its uptime, writes the stop code and its
 * parameters (each resolved to module+offset against a snapshot of the loaded modules taken at 20/60/180 s and
 * every 10 min) from a bugcheck callback, and marks a clean shutdown (IRP_MJ_SHUTDOWN). The next boot reports the
 * previous one in State: PrevBootText (REG_SZ), PrevBootEnd (0 none, 1 no shutdown/bugcheck, 2 clean, 3 bugcheck),
 * PrevBugCheck, PrevP1..PrevP4 (QWORD), BootSeq, and History0..History15 (REG_SZ ring, HistoryNext).
 * Ledger self-test: C:\starbite\auto\gpu\sbwdog.crashtest (a stage's gputest/ puts it there) makes the boot
 * bugcheck on purpose at Parameters\CrashTestSec (default 150 s): 0xE2 MANUALLY_INITIATED_CRASH ('SBCL', an
 * address in sbwdog.sys, one in ntoskrnl.exe, 0). Once per file: its last-write time goes to State\CrashTestStamp
 * (flushed) before the crash, and a file with that stamp is ignored.
 */
#include <ntddk.h>
/* T35: the stop code from the kernel's own export (aux_klib.lib's helpers need MSVC's ARM64 GS/intrinsic
 * runtime, which our clang link lacks), and the module list from SystemModuleInformation. */
__declspec(dllimport) extern ULONG_PTR KiBugCheckData[5];
NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG Class, PVOID Info, ULONG Length, PULONG ReturnLength);
#define SB_SYSTEM_MODULE_INFORMATION 11
typedef struct _SB_MODULE {
    HANDLE Section;
    PVOID MappedBase, ImageBase;
    ULONG ImageSize, Flags;
    USHORT LoadOrderIndex, InitOrderIndex, LoadCount, OffsetToFileName;
    UCHAR FullPathName[256];
} SB_MODULE;
typedef struct _SB_MODULES {
    ULONG NumberOfModules;
    SB_MODULE Modules[1];
} SB_MODULES;
#include "sbcl_core.h"

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
    /* T35: the crash ledger */
    volatile ULONG *Cl;                   /* the page, NULL if it could not be mapped */
    struct sbcl_page Cur, Prev;           /* this boot's record (written through to the page), the previous one */
    ULONG64 T0;
    KBUGCHECK_CALLBACK_RECORD BcRec;
    BOOLEAN BcRegistered;
    PDEVICE_OBJECT Dev;
    struct sbcl_mod Mods[2][256];
    ULONG ModN[2];
    volatile LONG ModCur;
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

NTSYSAPI NTSTATUS NTAPI ZwDeleteFile(POBJECT_ATTRIBUTES ObjectAttributes);   /* ntifs.h */

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

static void ParamPut(PCWSTR Name, ULONG Value)
{
    HANDLE h;
    UNICODE_STRING n;
    if (!NT_SUCCESS(OpenSub(L"Parameters", &h)))
        return;
    RtlInitUnicodeString(&n, Name);
    (void)ZwSetValueKey(h, &n, 0, REG_DWORD, &Value, sizeof(Value));
    (void)ZwFlushKey(h);
    ZwClose(h);
}

/* T37: a gputest belongs to this boot: still armed (it deletes the file when it starts at logon), or its log was
 * written after this boot began. The clean boot after a test sees neither. */
static BOOLEAN GputestThisBoot(void)
{
    UNICODE_STRING us = RTL_CONSTANT_STRING(L"\\??\\C:\\starbite\\auto\\gpu\\gputest.log");
    OBJECT_ATTRIBUTES oa;
    FILE_NETWORK_OPEN_INFORMATION fi;
    LARGE_INTEGER now;
    if (FileExists(L"\\??\\C:\\starbite\\auto\\gpu\\armed"))
        return TRUE;
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwQueryFullAttributesFile(&oa, &fi)))
        return FALSE;
    KeQuerySystemTimePrecise(&now);
    return fi.LastWriteTime.QuadPart > now.QuadPart - (LONGLONG)KeQueryInterruptTime();
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
    if (Rd(g.Pmu, PMU_WDT_DISABLE) & (1U << WDT1_PMU_BIT))
        Wr(g.Pmu, PMU_WDT_DISABLE, Rd(g.Pmu, PMU_WDT_DISABLE) & ~(1U << WDT1_PMU_BIT));
    if (Rd(g.Pmu, PMU_WDT_MASK) & (1U << WDT1_PMU_BIT))
        Wr(g.Pmu, PMU_WDT_MASK, Rd(g.Pmu, PMU_WDT_MASK) & ~(1U << WDT1_PMU_BIT));
    return Armed();
}

/* ---- T35: the crash ledger ------------------------------------------------------------------------------ */

static void ClWrite(void)
{
    const ULONG *w = (const ULONG *)&g.Cur;
    if (!g.Cl)
        return;
    sbcl_seal(&g.Cur);
    for (ULONG i = 0; i < sizeof(g.Cur) / 4; i++)
        WRITE_REGISTER_ULONG(&g.Cl[i], w[i]);
    __dsb(_ARM64_BARRIER_SY);
}

static ULONG Uptime(void)
{
    return (ULONG)((KeQueryInterruptTime() - g.T0) / 10000000ULL);
}

/* HIGH_LEVEL, other processors frozen: only our own nonpaged data and the page. */
static VOID BcCallback(PVOID Buffer, ULONG Length)
{
    LONG cur = g.ModCur;
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);
    g.Cur.state = SBCL_BUGCHECK;
    g.Cur.bc_uptime_s = Uptime();
    g.Cur.bc_code = (sbcl_u32)KiBugCheckData[0];
    g.Cur.p[0] = KiBugCheckData[1];
    g.Cur.p[1] = KiBugCheckData[2];
    g.Cur.p[2] = KiBugCheckData[3];
    g.Cur.p[3] = KiBugCheckData[4];
    g.Cur.mods = g.ModN[cur];
    for (unsigned i = 0; i < 4; i++)
        sbcl_resolve(g.Mods[cur], g.ModN[cur], g.Cur.p[i], g.Cur.where[i]);
    ClWrite();
}

/* The ledger self-test (above): TRUE = crash this boot. PASSIVE, once the file system answers. */
static BOOLEAN CrashTestArmed(void)
{
    UNICODE_STRING us = RTL_CONSTANT_STRING(L"\\??\\C:\\starbite\\auto\\gpu\\sbwdog.crashtest");
    OBJECT_ATTRIBUTES oa;
    FILE_NETWORK_OPEN_INFORMATION fi;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG64)];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    UNICODE_STRING n = RTL_CONSTANT_STRING(L"CrashTestStamp");
    ULONG len;
    HANDLE h;
    ULONG64 stamp;
    BOOLEAN go = FALSE;
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwQueryFullAttributesFile(&oa, &fi)))
        return FALSE;
    stamp = (ULONG64)fi.LastWriteTime.QuadPart;
    if (!NT_SUCCESS(OpenSub(L"State", &h)))
        return FALSE;
    if (!NT_SUCCESS(ZwQueryValueKey(h, &n, KeyValuePartialInformation, v, sizeof(buf), &len)) ||
        v->Type != REG_QWORD || *(ULONG64 *)v->Data != stamp) {
        /* durable before the crash: a file the bugcheck could not delete never fires twice */
        go = NT_SUCCESS(ZwSetValueKey(h, &n, 0, REG_QWORD, &stamp, sizeof(stamp))) && NT_SUCCESS(ZwFlushKey(h));
    }
    ZwClose(h);
    return go;
}

static NTSTATUS ShutdownDispatch(PDEVICE_OBJECT Dev, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Dev);
    if (g.Cur.state == SBCL_RUNNING) {
        g.Cur.state = SBCL_SHUTDOWN;
        g.Cur.shutdown_uptime_s = Uptime();
        ClWrite();
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS PassDispatch(PDEVICE_OBJECT Dev, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Dev);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* PASSIVE: the loaded modules into the spare table, then switch to it. */
static void ClSnapshotModules(void)
{
    ULONG size = 0, n, k;
    SB_MODULES *mi;
    LONG next = g.ModCur ^ 1;
    if (!g.Cl)
        return;
    (void)ZwQuerySystemInformation(SB_SYSTEM_MODULE_INFORMATION, NULL, 0, &size);
    if (!size || size > 1024 * 1024)
        return;
    size += 4096;                         /* modules may load in between */
    mi = (SB_MODULES *)ExAllocatePool2(POOL_FLAG_PAGED, size, SBWD_TAG);
    if (!mi)
        return;
    if (NT_SUCCESS(ZwQuerySystemInformation(SB_SYSTEM_MODULE_INFORMATION, mi, size, &size))) {
        n = mi->NumberOfModules;
        if (n > RTL_NUMBER_OF(g.Mods[0]))
            n = RTL_NUMBER_OF(g.Mods[0]);
        for (ULONG i = 0; i < n; i++) {
            struct sbcl_mod *m = &g.Mods[next][i];
            USHORT off = mi->Modules[i].OffsetToFileName < 256 ? mi->Modules[i].OffsetToFileName : 0;
            const UCHAR *name = mi->Modules[i].FullPathName + off;
            m->base = (sbcl_u64)(ULONG_PTR)mi->Modules[i].ImageBase;
            m->size = mi->Modules[i].ImageSize;
            for (k = 0; k < sizeof(m->name) && off + k < 256 && name[k]; k++)
                m->name[k] = (char)name[k];
            for (; k < sizeof(m->name); k++)
                m->name[k] = 0;
        }
        g.ModN[next] = n;
        __dmb(_ARM64_BARRIER_ISH);       /* the table before the switch (one writer: the feeder) */
        g.ModCur = next;
    }
    ExFreePoolWithTag(mi, SBWD_TAG);
}

static void StateSz(HANDLE H, PCWSTR Name, const char *A)
{
    WCHAR w[512];
    UNICODE_STRING n;
    ULONG i;
    for (i = 0; i < RTL_NUMBER_OF(w) - 1 && A[i]; i++)
        w[i] = (WCHAR)(UCHAR)A[i];
    w[i] = 0;
    RtlInitUnicodeString(&n, Name);
    (void)ZwSetValueKey(H, &n, 0, REG_SZ, w, (i + 1) * sizeof(WCHAR));
}

static void StateQword(HANDLE H, PCWSTR Name, ULONG64 V)
{
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, Name);
    (void)ZwSetValueKey(H, &n, 0, REG_QWORD, &V, sizeof(V));
}

/* DriverEntry: read what the previous Windows boot left, start this boot's record, report the previous one. */
static void ClInit(PDRIVER_OBJECT Drv)
{
    PHYSICAL_ADDRESS pa;
    ULONG *w = (ULONG *)&g.Prev;
    HANDLE h;
    char text[400];
    BOOLEAN valid;

    g.T0 = KeQueryInterruptTime();
    pa.QuadPart = (LONGLONG)SBCL_PA;
    g.Cl = (volatile ULONG *)MmMapIoSpaceEx(pa, SBCL_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    State(L"CrashLedger", g.Cl != NULL ? 2 : 0, FALSE);   /* T39: 2 = the zone-0 location */
    if (!g.Cl)
        return;
    g.Cl = (volatile ULONG *)((volatile UCHAR *)g.Cl + SBCL_OFF);
    for (ULONG i = 0; i < sizeof(g.Prev) / 4; i++)
        w[i] = READ_REGISTER_ULONG(&g.Cl[i]);
    valid = sbcl_valid(&g.Prev) != 0;
    RtlZeroMemory(&g.Cur, sizeof(g.Cur));
    g.Cur.magic = SBCL_MAGIC;
    g.Cur.version = SBCL_VERSION;
    g.Cur.boot_seq = (g.Prev.magic == SBCL_MAGIC ? g.Prev.boot_seq : 0) + 1;
    g.Cur.state = SBCL_RUNNING;
    ClWrite();

    sbcl_describe(&g.Prev, text, sizeof(text));
    if (NT_SUCCESS(OpenSub(L"State", &h))) {
        ULONG next;
        UNICODE_STRING n;
        WCHAR hn[16] = L"History";
        {
            ULONG v;
            v = g.Cur.boot_seq;
            RtlInitUnicodeString(&n, L"BootSeq");
            (void)ZwSetValueKey(h, &n, 0, REG_DWORD, &v, sizeof(v));
            v = valid ? g.Prev.state : 0;
            RtlInitUnicodeString(&n, L"PrevBootEnd");
            (void)ZwSetValueKey(h, &n, 0, REG_DWORD, &v, sizeof(v));
            v = valid && g.Prev.state == SBCL_BUGCHECK ? g.Prev.bc_code : 0;
            RtlInitUnicodeString(&n, L"PrevBugCheck");
            (void)ZwSetValueKey(h, &n, 0, REG_DWORD, &v, sizeof(v));
        }
        StateSz(h, L"PrevBootText", text);
        StateQword(h, L"PrevP1", valid ? g.Prev.p[0] : 0);
        StateQword(h, L"PrevP2", valid ? g.Prev.p[1] : 0);
        StateQword(h, L"PrevP3", valid ? g.Prev.p[2] : 0);
        StateQword(h, L"PrevP4", valid ? g.Prev.p[3] : 0);
        if (valid) {
            /* the ring: History<HistoryNext> = this text, HistoryNext + 1 (mod 16) */
            next = QueryDword(h, L"HistoryNext", 0) % 16;
            hn[7] = (WCHAR)(next >= 10 ? L'1' : L'0' + next);
            hn[8] = next >= 10 ? (WCHAR)(L'0' + next - 10) : 0;
            hn[9] = 0;
            StateSz(h, hn, text);
            next = (next + 1) % 16;
            RtlInitUnicodeString(&n, L"HistoryNext");
            (void)ZwSetValueKey(h, &n, 0, REG_DWORD, &next, sizeof(next));
        }
        (void)ZwFlushKey(h);
        ZwClose(h);
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "sbwdog: previous boot: %s\n", text);

    KeInitializeCallbackRecord(&g.BcRec);
    g.BcRegistered = KeRegisterBugCheckCallback(&g.BcRec, BcCallback, NULL, 0, (PUCHAR)"sbwdog");
    State(L"BugCheckCallback", g.BcRegistered, FALSE);
    if (NT_SUCCESS(IoCreateDevice(Drv, 0, NULL, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &g.Dev))) {
        Drv->MajorFunction[IRP_MJ_CREATE] = PassDispatch;
        Drv->MajorFunction[IRP_MJ_CLOSE] = PassDispatch;
        Drv->MajorFunction[IRP_MJ_SHUTDOWN] = ShutdownDispatch;
        State(L"ShutdownNotify", NT_SUCCESS(IoRegisterShutdownNotification(g.Dev)), FALSE);
        g.Dev->Flags &= ~DO_DEVICE_INITIALIZING;
    }
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
    BOOLEAN dead = Ctx != NULL;           /* DeadmanTest (registry) */
    ULONG period = Param(L"PeriodMs", 5000);
    ULONG unattended = Param(L"Unattended", 0);
    ULONG restartSec = 0, capSec = 0, mode = 2, secs = 0, feeds = 0, rearms = 0, crashAt = 0;
    BOOLEAN crashChecked = FALSE, longer = FALSE;
    ULONG armSec = (unattended || dead) ? Param(L"ArmSec", 82) : 0;
    BOOLEAN feeding = !dead, restarted = FALSE, decided = FALSE, armed, sampled10 = FALSE, sampled20 = FALSE;
    ULONG64 t0 = KeQueryInterruptTime();
    LARGE_INTEGER t;

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY + 14);
    if (period < 500 || period > 10000)
        period = 5000;
    armed = Armed();
    for (;;) {
        ULONG elapsed;
        if (dead && armSec && !Armed()) {
            armed = Arm(armSec);          /* the test needs it running: armed, never fed */
            State(L"Rearms", ++rearms, TRUE);
        }
        if (feeding && armSec && !Armed()) {
            /* stopped by someone after us (or never armed): arm again; the dead-man comes first */
            armed = Arm(armSec);
            State(L"Rearms", ++rearms, TRUE);
        }
        if (feeding && armed)
            Feed(), feeds++;
        t.QuadPart = -10000LL * period;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
        elapsed = (ULONG)((KeQueryInterruptTime() - t0) / 10000000ULL);
        /* T35: the ledger's uptime, and the module snapshot the bugcheck callback resolves against */
        if (g.Cl && g.Cur.state == SBCL_RUNNING) {
            g.Cur.uptime_s = Uptime();
            ClWrite();
        }
        if ((elapsed >= 20 && secs < 20) || (elapsed >= 60 && secs < 60) || (elapsed >= 180 && secs < 180) ||
            (elapsed >= 600 && elapsed / 600 != secs / 600))
            ClSnapshotModules();
        if (!crashChecked && elapsed >= 15) {
            crashChecked = elapsed >= 90;     /* C: may not exist yet at 15 s: look every feed until 90 s */
            if (g.Cl && CrashTestArmed()) {
                crashChecked = TRUE;
                crashAt = Param(L"CrashTestSec", 150);
                if (crashAt < 30 || crashAt > 3000)
                    crashAt = 150;
                State(L"CrashTestAt", crashAt, TRUE);
            }
        }
        if (crashAt && elapsed >= crashAt) {
            ClSnapshotModules();
            KeBugCheckEx(0xE2, SBCL_MAGIC, (ULONG_PTR)BcCallback, (ULONG_PTR)&KeBugCheckEx, 0);
        }
        /* the kill file, once the file system answers (not in DriverEntry: boot-start, too early) */
        if (!dead && elapsed < 60 && FileExists(L"\\SystemRoot\\Starbite\\sbwdog.off")) {
            dead = TRUE;
            feeding = FALSE;
            State(L"KillFile", 1, TRUE);
            State(L"KillFileSeenAt", elapsed, TRUE);
            if (!Armed() && Param(L"ArmSec", 82)) {
                armSec = Param(L"ArmSec", 82);
                armed = Arm(armSec);
                State(L"ArmedNow", armed, TRUE);
            }
        }
        /* T17: \Windows\Starbite\sbwdog.unattended (made from TWRP, one-shot, deleted here) makes this one boot
         * an unattended one without a registry write: a dirty hive (the user's T15 session ended in a 0x7E at
         * restart) must not be edited offline, and a boot that repairs it must still come back by itself. The
         * watchdog is armed as soon as the file is seen (seconds after boot-start); restart and cap as for
         * Unattended=1. */
        if (!dead && !unattended && elapsed < 60 && FileExists(L"\\SystemRoot\\Starbite\\sbwdog.unattended")) {
            UNICODE_STRING fn = RTL_CONSTANT_STRING(L"\\SystemRoot\\Starbite\\sbwdog.unattended");
            OBJECT_ATTRIBUTES foa;
            unattended = 1;
            g.PmuWritable = TRUE;
            armSec = Param(L"ArmSec", 82);
            armed = Armed() || Arm(armSec);
            State(L"UnattendedByFile", elapsed, TRUE);
            State(L"ArmedNow", armed, TRUE);
            InitializeObjectAttributes(&foa, &fn, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
            State(L"UnattendedFileDeleted", NT_SUCCESS(ZwDeleteFile(&foa)), TRUE);
        }
        if (dead) {
            /* a pure dead-man: does the counter run? (82 s = 65063 counts; 10 s = 7935) */
            if (!sampled10 && elapsed >= 10) {
                sampled10 = TRUE;
                State(L"Dm10WtCon", Rd(g.Wdt, WTCON), FALSE);
                State(L"Dm10WtCnt", Rd(g.Wdt, WTCNT), TRUE);
            }
            if (!sampled20 && elapsed >= 20) {
                sampled20 = TRUE;
                State(L"Dm20WtCon", Rd(g.Wdt, WTCON), FALSE);
                State(L"Dm20WtCnt", Rd(g.Wdt, WTCNT), TRUE);
            }
            State(L"Seconds", elapsed, TRUE);   /* the last one written is how long Windows lived */
            secs = elapsed;
            continue;                     /* no restart, no cap, no feeding */
        }
        if (!decided && elapsed >= 60) {
            /* sbwddm has started by now in every boot that has it */
            decided = TRUE;
            mode = WddmMode();
            /* T37: a display-only boot that runs a gputest (gpu-cycle --reference: the sweep drawn by WARP) gets the
             * full-mode times; the clean boot after it keeps the short ones */
            longer = mode != 1 && GputestThisBoot();
            State(L"GputestDisplayOnly", longer, FALSE);
            restartSec = Param(mode == 1 || longer ? L"FullRestartSec" : L"DisplayRestartSec", mode == 1 || longer ? 300 : 120);
            capSec = Param(mode == 1 || longer ? L"FullCapSec" : L"DisplayCapSec", mode == 1 || longer ? 360 : 180);
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
    BOOLEAN dead;
    NTSTATUS st;

    RtlInitEmptyUnicodeString(&g.Svc, g.SvcBuf, sizeof(g.SvcBuf));
    (void)RtlAppendUnicodeStringToString(&g.Svc, Reg);
    ClInit(Drv);                          /* T35: before anything that could fail */
    pa.QuadPart = (LONGLONG)WDT1_PA;
    g.Wdt = (volatile UCHAR *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    dead = Param(L"DeadmanTest", 0) != 0;
    if (dead)
        ParamPut(L"DeadmanTest", 0);      /* one-shot: the boot after the test is a normal one */
    /* only automation boots (and the dead-man test) arm it */
    armSec = (Param(L"Unattended", 0) || dead) ? Param(L"ArmSec", 82) : 0;
    pa.QuadPart = (LONGLONG)PMU_PA;
    g.PmuWritable = armSec != 0;          /* the PMU is written only to arm (ArmSec), and only bit 23 */
    /* T17: mapped writable always (the file switch may arm later); written only while PmuWritable */
    g.Pmu = (volatile UCHAR *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_NOCACHE | PAGE_READWRITE);
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
        if (frq == 0)
            frq = 26000000;               /* S-Boot leaves CNTFRQ_EL0 = 0; the counter runs at 26 MHz (NOTES.md) */
        State(L"UptimeMsAtStart", (ULONG)(cnt * 1000ULL / frq), FALSE);
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
    /* ArmedBySelf: 1 = it was stopped and we armed it; 0 = the loader's arming survived (or ArmSec 0) */
    if (!Armed() && armSec)
        State(L"ArmedBySelf", Arm(armSec), FALSE);
    else
        State(L"ArmedBySelf", 0, FALSE);
    State(L"ArmedNow", Armed(), FALSE);
    State(L"ArmWtCon", Rd(g.Wdt, WTCON), FALSE);
    State(L"ArmWtCnt", Rd(g.Wdt, WTCNT), FALSE);
    State(L"Rearms", 0, FALSE);
    State(L"KillFile", 0, FALSE);
    State(L"KillFileSeenAt", 0, FALSE);
    State(L"UnattendedByFile", 0, FALSE);
    State(L"Dm10WtCnt", 0, FALSE);
    State(L"Dm20WtCnt", 0, FALSE);
    State(L"DeadmanTest", dead, TRUE);
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, &oa, NULL, NULL, FeedThread, dead ? (PVOID)1 : NULL);
    if (!NT_SUCCESS(st))
        return st;
    ZwClose(th);
    /* No unload: the feeder runs as long as Windows does. */
    return STATUS_SUCCESS;
}
