// sbxta.sys: per-boot XtaCache policy for the two Windows rows on the Galaxy S9 (docs/WIN11-M3-CLEAN.md section 34).
//
// The A55 row and the big-core (Exynos M3) row boot the same Windows install. xtajit's persistent translation cache
// (C:\Windows\XtaCache, written through the XtaCache service) holds code made on the A55, which uses LSE and LDAPR
// (LRCPC); the M3 is ARMv8.0 and faults on those. T2 (2026-11-11): with the XtaCache service disabled and the folder
// out of the way, x64 and x86 programs run on the M3.
//
// System-start driver (Start = 1): loaded by the kernel before the Service Control Manager reads service configs.
// On every load it reads MIDR_EL1 and:
//   M3  (implementer 0x53, part 0x002): Services\XtaCache\Start = 4 (disabled)            [Parameters\DisableOnM3, default 1]
//   A55 (implementer 0x41, part 0xD05): Services\XtaCache\Start = 2 if it is 4 (restore)  [same switch]
//   anything else: changes nothing.
// Optional (Parameters\SwapCache = 1, default 0; for the case where the folder itself must go on the M3 row):
//   M3  boot (no XtaCache.a55 yet): \SystemRoot\XtaCache -> XtaCache.a55, then XtaCache.m3 -> XtaCache (if present)
//   A55 boot (XtaCache.a55 present): XtaCache -> XtaCache.m3, then XtaCache.a55 -> XtaCache
//   so each CPU class keeps its own cache; a name clash = no rename, logged. Use with DisableOnM3 = 0 if the M3 row
//   should build its own cache (xtajit on the M3 translates without LSE/LRCPC).
// Everything it did goes to Services\sbxta\State (non-volatile, flushed; readable from the offline SYSTEM hive):
//   Build (SZ), Boots (DWORD, +1 per load), Midr (QWORD), Cpu (SZ "M3"/"A55"/"other"),
//   XtaStartBefore / XtaStartAfter (DWORD, 0xFFFFFFFF = not readable), XtaStatus (NTSTATUS of the write),
//   Swap (SZ: what the folder step did), Result (SZ, one line summary).
// It never touches anything else: no Windows binary, no other service. DriverEntry returns success and the driver
// stays loaded (nothing to do after that; an unload routine is set so sc stop works).
#include <ntifs.h>
#include <ntstrsafe.h>

#ifndef SB_BUILD_ID
#define SB_BUILD_ID "unknown"
#endif

DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD SbxtaUnload;

#define MIDR_EL1 ARM64_SYSREG(3, 0, 0, 0, 0)
#define NOVALUE 0xFFFFFFFFu

static const UNICODE_STRING kXtaKey =
    RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\XtaCache");

static ULONG ReadDword(HANDLE key, PCWSTR name, ULONG dflt)
{
    UNICODE_STRING us;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION kv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len;

    RtlInitUnicodeString(&us, name);
    if (NT_SUCCESS(ZwQueryValueKey(key, &us, KeyValuePartialInformation, kv, sizeof buf, &len)) &&
        kv->Type == REG_DWORD && kv->DataLength == sizeof(ULONG))
        return *(ULONG *)kv->Data;
    return dflt;
}

static void SetValue(HANDLE key, PCWSTR name, ULONG type, const void *data, ULONG cb)
{
    UNICODE_STRING us;
    RtlInitUnicodeString(&us, name);
    ZwSetValueKey(key, &us, 0, type, (PVOID)data, cb);
}

static void SetSz(HANDLE key, PCWSTR name, PCWSTR text)
{
    size_t cb = 0;
    RtlStringCbLengthW(text, 1024, &cb);
    SetValue(key, name, REG_SZ, text, (ULONG)cb + sizeof(WCHAR));
}

static HANDLE OpenSub(PCUNICODE_STRING base, PCWSTR sub, BOOLEAN create)
{
    WCHAR path[300];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    ULONG disp;

    if (!NT_SUCCESS(RtlStringCbPrintfW(path, sizeof path, L"%wZ\\%ws", base, sub)))
        return NULL;
    RtlInitUnicodeString(&us, path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (create) {
        if (!NT_SUCCESS(ZwCreateKey(&key, KEY_READ | KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp)))
            return NULL;
    } else if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
        return NULL;
    }
    return key;
}

// ---- optional cache-folder swap (Parameters\SwapCache = 1) -------------------------------------------------------

static BOOLEAN Exists(HANDLE root, PCWSTR name)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    FILE_NETWORK_OPEN_INFORMATION fi;

    RtlInitUnicodeString(&us, name);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, root, NULL);
    return NT_SUCCESS(ZwQueryFullAttributesFile(&oa, &fi));
}

// Rename root\from -> root\to (a directory; same parent). Fails rather than replaces.
static NTSTATUS RenameDir(HANDLE root, PCWSTR from, PCWSTR to)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS st;
    UCHAR buf[sizeof(FILE_RENAME_INFORMATION) + 64 * sizeof(WCHAR)];
    PFILE_RENAME_INFORMATION ri = (PFILE_RENAME_INFORMATION)buf;
    size_t cb = 0;

    RtlInitUnicodeString(&us, from);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, root, NULL);
    st = ZwOpenFile(&h, DELETE | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(st))
        return st;
    RtlZeroMemory(buf, sizeof buf);
    RtlStringCbLengthW(to, 60 * sizeof(WCHAR), &cb);
    ri->ReplaceIfExists = FALSE;
    ri->RootDirectory = root;
    ri->FileNameLength = (ULONG)cb;
    RtlCopyMemory(ri->FileName, to, cb);
    st = ZwSetInformationFile(h, &iosb, ri, sizeof buf, FileRenameInformation);
    ZwClose(h);
    return st;
}

// Layout is keyed on XtaCache.a55: present = M3 layout (the A55's cache parked), absent = A55 layout (the starting
// state: the existing XtaCache belongs to the A55; the M3's cache, once there is one, is parked as XtaCache.m3).
static void SwapCache(BOOLEAN m3, PWSTR log, size_t cb)
{
    UNICODE_STRING us = RTL_CONSTANT_STRING(L"\\SystemRoot");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE root;
    NTSTATUS a = STATUS_SUCCESS, b = STATUS_SUCCESS;
    BOOLEAN parkedA55, cur;

    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenFile(&root, FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE, &oa, &iosb,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT))) {
        RtlStringCbCopyW(log, cb, L"SystemRoot not openable; nothing renamed");
        return;
    }
    parkedA55 = Exists(root, L"XtaCache.a55");
    cur = Exists(root, L"XtaCache");
    if (m3 == parkedA55) {
        RtlStringCbCopyW(log, cb, m3 ? L"M3 layout already" : L"A55 layout already");
    } else if (m3) {                                            // A55 layout -> M3 layout
        if (cur)
            a = RenameDir(root, L"XtaCache", L"XtaCache.a55");
        if (NT_SUCCESS(a) && Exists(root, L"XtaCache.m3"))
            b = RenameDir(root, L"XtaCache.m3", L"XtaCache");
        RtlStringCbPrintfW(log, cb, L"to M3: XtaCache->.a55 0x%08X, .m3->XtaCache 0x%08X", a, b);
    } else if (cur && Exists(root, L"XtaCache.m3")) {           // M3 layout but both names taken: leave it
        RtlStringCbCopyW(log, cb, L"to A55: XtaCache and XtaCache.m3 both exist; nothing renamed");
    } else {                                                    // M3 layout -> A55 layout
        if (cur)
            a = RenameDir(root, L"XtaCache", L"XtaCache.m3");
        if (NT_SUCCESS(a))
            b = RenameDir(root, L"XtaCache.a55", L"XtaCache");
        RtlStringCbPrintfW(log, cb, L"to A55: XtaCache->.m3 0x%08X, .a55->XtaCache 0x%08X", a, b);
    }
    ZwClose(root);
}

// ------------------------------------------------------------------------------------------------------------------

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regPath)
{
    OBJECT_ATTRIBUTES oa;
    HANDLE state, par, xta = NULL;
    ULONG64 midr = (ULONG64)_ReadStatusReg(MIDR_EL1);
    ULONG impl = (ULONG)(midr >> 24) & 0xFF, part = (ULONG)(midr >> 4) & 0xFFF;
    BOOLEAN m3 = impl == 0x53 && part == 0x002, a55 = impl == 0x41 && part == 0xD05;
    ULONG disableOnM3 = 1, swap = 0, before = NOVALUE, after = NOVALUE, boots;
    NTSTATUS xst = STATUS_NOT_FOUND;
    WCHAR swaplog[160] = L"off", result[256];

    drv->DriverUnload = SbxtaUnload;
    par = OpenSub(regPath, L"Parameters", FALSE);
    if (par) {
        disableOnM3 = ReadDword(par, L"DisableOnM3", 1);
        swap = ReadDword(par, L"SwapCache", 0);
        ZwClose(par);
    }

    InitializeObjectAttributes(&oa, (PUNICODE_STRING)&kXtaKey, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    xst = ZwOpenKey(&xta, KEY_READ | KEY_SET_VALUE, &oa);
    if (NT_SUCCESS(xst)) {
        ULONG want;
        before = ReadDword(xta, L"Start", NOVALUE);
        want = before;
        if (disableOnM3 && m3 && before != 4)
            want = 4;
        else if (disableOnM3 && a55 && before == 4)
            want = 2;
        if (want != before) {
            UNICODE_STRING us = RTL_CONSTANT_STRING(L"Start");
            xst = ZwSetValueKey(xta, &us, 0, REG_DWORD, &want, sizeof want);
            ZwFlushKey(xta);
        }
        after = ReadDword(xta, L"Start", NOVALUE);
        ZwClose(xta);
    }
    if (swap && (m3 || a55))
        SwapCache(m3, swaplog, sizeof swaplog);

    RtlStringCbPrintfW(result, sizeof result, L"sbxta %S: %ws MIDR %08I64X, XtaCache Start %d -> %d (0x%08X), swap: %ws",
                       SB_BUILD_ID, m3 ? L"M3" : a55 ? L"A55" : L"other", midr, (int)before, (int)after, xst, swaplog);

    state = OpenSub(regPath, L"State", TRUE);
    if (state) {
        WCHAR build[64];
        RtlStringCbPrintfW(build, sizeof build, L"%S", SB_BUILD_ID);
        boots = ReadDword(state, L"Boots", 0) + 1;
        SetSz(state, L"Build", build);
        SetValue(state, L"Boots", REG_DWORD, &boots, sizeof boots);
        SetValue(state, L"Midr", REG_QWORD, &midr, sizeof midr);
        SetSz(state, L"Cpu", m3 ? L"M3" : a55 ? L"A55" : L"other");
        SetValue(state, L"XtaStartBefore", REG_DWORD, &before, sizeof before);
        SetValue(state, L"XtaStartAfter", REG_DWORD, &after, sizeof after);
        SetValue(state, L"XtaStatus", REG_DWORD, &xst, sizeof xst);
        SetSz(state, L"Swap", swaplog);
        SetSz(state, L"Result", result);
        ZwFlushKey(state);
        ZwClose(state);
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "%ws\n", result);
    return STATUS_SUCCESS;
}

static void SbxtaUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
}
