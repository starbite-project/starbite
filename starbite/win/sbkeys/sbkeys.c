/* SPDX-License-Identifier: GPL-2.0-only
 * sbkeys: the Galaxy S9's hardware keys under Windows (docs/WIN11-GPU.md 6.40).
 *
 * The release's ACPI has no button device, so the keys do nothing in Windows, and a Start menu that will
 * not open leaves no clean way to restart. This service (Type 1, Start 2: loaded by the SCM once C: is up)
 * polls the always-on GPIO DAT registers, read only, every PollMs (default 30) from a system thread:
 *   - Bixby alone held HoldMs (default 3000): a clean restart, NtShutdownSystem(ShutdownReboot) from a
 *     second thread, as sbwdog does it: drivers get IRP_MJ_SHUTDOWN, the hives and NTFS are flushed and
 *     closed. Apps are not asked to save (no WM_QUERYENDSESSION).
 *   - Volume Up + Power, a brief press (all keys up within SasMaxMs, default 1500), only with Parameters\Sas=1
 *     (default 0): signals the named event \BaseNamedObjects\StarbiteSasRequest (Global\StarbiteSasRequest in
 *     user mode); a SYSTEM service (sblinkd) waiting on it calls SendSAS (Ctrl+Alt+Del; needs the
 *     SoftwareSASGeneration policy). sbkeys only signals.
 *   - Volume Down + Power is never used: held ~7 s it is the hardware force restart.
 * Patterns and their guards: sbk_core.c (host test starbite/win/tests/keys_test.c).
 *
 * No PnP device and no SSDT: the page (0x14050000) is already a memory resource of the release's touch
 * device TSP0, so a second ACPI device claiming it would get a resource conflict. sbkeys maps it itself with
 * MmMapIoSpaceEx(PAGE_READONLY | PAGE_NOCACHE) (the same device memory type as the touch driver's mapping) and
 * only reads CON and DAT; it never writes the page.
 *
 * Off switches: the kill file C:\Windows\Starbite\sbkeys.off (DriverEntry returns before mapping anything);
 * Parameters\Enable = 0; Services\sbkeys\Start = 4 (offline, clean hive only).
 * Unattended proof: Parameters\SelfTestSec = N (one-shot: cleared and flushed at start) feeds the state
 * machine "Bixby pressed" from N s after start, so the restart path runs without a finger (the real pins
 * are still read and logged).
 *
 * State (Services\sbkeys\State, flushed on every event): Started, KillFile, MapFailed, Gpa0Con, Gpa2Con,
 * Gpa0Dat, Gpa2Dat (at start), PinsOk, ArmedAt (s), KeysSeen (every key seen pressed, SBK_* bits), Presses,
 * LastKeys, RestartAt (s), RestartStatus, SasRequests, SelfTest.
 */
#include <ntddk.h>
#include "sbk_core.h"

#define SBK_TAG 'ykbS'

typedef enum _SHUTDOWN_ACTION { ShutdownNoReboot, ShutdownReboot, ShutdownPowerOff } SHUTDOWN_ACTION;
NTSYSAPI NTSTATUS NTAPI NtShutdownSystem(SHUTDOWN_ACTION Action);

DRIVER_INITIALIZE DriverEntry;
static KSTART_ROUTINE PollThread, RestartThread;

static struct {
    volatile UCHAR *Gpio;
    struct sbk_cfg Cfg;
    ULONG PollMs, SelfTestSec;
    PKEVENT SasEvent;
    HANDLE SasHandle;
    WCHAR SvcBuf[256];
    UNICODE_STRING Svc;
} g;

/* ---- registry: this service's key ---------------------------------------------------------------------- */

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

static ULONG Param(PCWSTR Name, ULONG Dflt)
{
    HANDLE h;
    UNICODE_STRING n;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len, r = Dflt;
    if (!NT_SUCCESS(OpenSub(L"Parameters", &h)))
        return Dflt;
    RtlInitUnicodeString(&n, Name);
    if (NT_SUCCESS(ZwQueryValueKey(h, &n, KeyValuePartialInformation, v, sizeof(buf), &len)) &&
        v->Type == REG_DWORD && v->DataLength == sizeof(ULONG))
        r = *(ULONG *)v->Data;
    ZwClose(h);
    return r;
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

static BOOLEAN FileExists(PCWSTR Path)
{
    UNICODE_STRING p;
    OBJECT_ATTRIBUTES oa;
    FILE_NETWORK_OPEN_INFORMATION fi;
    RtlInitUnicodeString(&p, Path);
    InitializeObjectAttributes(&oa, &p, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NT_SUCCESS(ZwQueryFullAttributesFile(&oa, &fi));
}

static ULONG Rd(ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(g.Gpio + Off));
}

/* ---- threads ------------------------------------------------------------------------------------------ */

static VOID RestartThread(PVOID Ctx)
{
    NTSTATUS st;
    UNREFERENCED_PARAMETER(Ctx);
    st = NtShutdownSystem(ShutdownReboot);   /* does not return when it works */
    State(L"RestartStatus", (ULONG)st, TRUE);
    PsTerminateSystemThread(st);
}

static VOID PollThread(PVOID Ctx)
{
    struct sbk_state s;
    ULONG64 t0 = KeQueryInterruptTime(), last = t0;
    ULONG seen = 0, presses = 0, sas = 0, prevKeys = 0;
    BOOLEAN armedLogged = FALSE, restarted = FALSE;
    LARGE_INTEGER wait;
    UNREFERENCED_PARAMETER(Ctx);

    /* above normal work so a busy or stuck desktop still gets its keys read; it sleeps 30 ms at a time */
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    sbk_init(&s);
    wait.QuadPart = -(LONGLONG)g.PollMs * 10000;
    for (;;) {
        ULONG64 now;
        ULONG keys, dt, secs;
        enum sbk_action a;
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
        now = KeQueryInterruptTime();
        dt = (ULONG)((now - last) / 10000);
        last = now;
        secs = (ULONG)((now - t0) / 10000000);
        keys = sbk_keys(Rd(SBK_GPA0_DAT), Rd(SBK_GPA2_DAT));
        if (g.SelfTestSec && secs >= g.SelfTestSec)
            keys = SBK_BIXBY;                /* the self test: Bixby "held" from SelfTestSec on */
        if (keys != prevKeys) {
            if (keys & ~seen) {
                seen |= keys;
                State(L"KeysSeen", seen, TRUE);
            }
            if (keys && !prevKeys)
                State(L"Presses", ++presses, FALSE);
            State(L"LastKeys", keys, FALSE);
            prevKeys = keys;
        }
        a = sbk_step(&g.Cfg, &s, keys, dt);
        if (s.armed && !armedLogged) {
            armedLogged = TRUE;
            State(L"ArmedAt", secs, TRUE);
        }
        if (a == SBK_RESTART && !restarted) {
            OBJECT_ATTRIBUTES oa;
            HANDLE th;
            restarted = TRUE;                /* once per boot */
            State(L"RestartAt", secs, TRUE);
            InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
            if (NT_SUCCESS(PsCreateSystemThread(&th, THREAD_ALL_ACCESS, &oa, NULL, NULL, RestartThread, NULL)))
                ZwClose(th);
        } else if (a == SBK_SAS && g.SasEvent) {
            KeSetEvent(g.SasEvent, IO_NO_INCREMENT, FALSE);
            State(L"SasRequests", ++sas, TRUE);
        }
    }
}

/* ---- entry -------------------------------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING Reg)
{
    PHYSICAL_ADDRESS pa;
    OBJECT_ATTRIBUTES oa;
    HANDLE th;
    ULONG c0, c2;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(Drv);

    RtlInitEmptyUnicodeString(&g.Svc, g.SvcBuf, sizeof(g.SvcBuf));
    (void)RtlAppendUnicodeStringToString(&g.Svc, Reg);
    if (FileExists(L"\\SystemRoot\\Starbite\\sbkeys.off")) {
        State(L"KillFile", 1, TRUE);
        return STATUS_UNSUCCESSFUL;
    }
    State(L"KillFile", 0, FALSE);
    if (!Param(L"Enable", 1)) {
        State(L"Started", 0, TRUE);
        return STATUS_UNSUCCESSFUL;
    }
    g.Cfg.hold_ms = Param(L"HoldMs", 3000);
    g.Cfg.arm_ms = Param(L"ArmMs", 2000);
    g.Cfg.sas = Param(L"Sas", 0);
    g.Cfg.sas_max_ms = Param(L"SasMaxMs", 1500);
    sbk_cfg_fix(&g.Cfg);
    g.PollMs = Param(L"PollMs", 30);
    if (g.PollMs < 10 || g.PollMs > 200)
        g.PollMs = 30;
    g.SelfTestSec = Param(L"SelfTestSec", 0);
    if (g.SelfTestSec)
        ParamPut(L"SelfTestSec", 0);         /* one-shot: the boot after the test is a normal one */
    State(L"SelfTest", g.SelfTestSec, FALSE);

    pa.QuadPart = (LONGLONG)SBK_GPIO_PA;
    g.Gpio = (volatile UCHAR *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (g.Gpio == NULL) {
        State(L"MapFailed", 1, TRUE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    State(L"MapFailed", 0, FALSE);
    c0 = Rd(SBK_GPA0_CON);
    c2 = Rd(SBK_GPA2_CON);
    State(L"Gpa0Con", c0, FALSE);
    State(L"Gpa2Con", c2, FALSE);
    State(L"Gpa0Dat", Rd(SBK_GPA0_DAT), FALSE);
    State(L"Gpa2Dat", Rd(SBK_GPA2_DAT), FALSE);
    State(L"ArmedAt", 0, FALSE);
    State(L"KeysSeen", 0, FALSE);
    State(L"Presses", 0, FALSE);
    State(L"RestartAt", 0, FALSE);
    State(L"SasRequests", 0, FALSE);
    /* a key pin that is not an input (or EINT) does not read a key: run nothing */
    State(L"PinsOk", sbk_pins_ok(c0, c2), FALSE);
    if (!sbk_pins_ok(c0, c2)) {
        State(L"Started", 0, TRUE);
        MmUnmapIoSpace((PVOID)g.Gpio, PAGE_SIZE);
        return STATUS_UNSUCCESSFUL;
    }
    if (g.Cfg.sas) {
        UNICODE_STRING n;
        RtlInitUnicodeString(&n, L"\\BaseNamedObjects\\StarbiteSasRequest");
        g.SasEvent = IoCreateSynchronizationEvent(&n, &g.SasHandle);
        if (g.SasEvent)
            KeClearEvent(g.SasEvent);
    }
    State(L"SasEvent", g.SasEvent != NULL, FALSE);
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, &oa, NULL, NULL, PollThread, NULL);
    if (!NT_SUCCESS(st)) {
        State(L"Started", 0, TRUE);
        return st;
    }
    ZwClose(th);
    State(L"Started", 1, TRUE);
    /* No unload: the poller runs as long as Windows does. */
    return STATUS_SUCCESS;
}
