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
//   Swap (SZ: what the folder step did), Result (SZ, one line summary), Svc (SZ: the service-list step, below).
// Optional per-boot service list (Parameters\DisableOnM3Services, REG_MULTI_SZ, default absent = off; README-services.md):
//   M3  boot: each listed service (at most 16 names; empty, over-long or backslashed names ignored) whose Start is not
//             4: Start saved to Services\sbxta\Saved\<name> (REG_DWORD; only if no saved value exists, so a later M3
//             boot never overwrites the original with 4), then Start = 4. A per-user template (Type has 0x40, e.g.
//             WpnUserService, OneSyncSvc) also gets its existing instance keys <name>_<suffix> done the same way,
//             each saved under its own name; the template matters most (instances are made from it at logon).
//   A55 boot: every value in Saved: that service's Start = the saved value if it is 4 now, then the value is deleted
//             (also when the service is gone or was re-enabled meanwhile). Needs no list, so removing the list and
//             booting the A55 row once undoes everything.
//   other CPUs: nothing. No service key is ever created or deleted; services neither listed nor in Saved are untouched.
// It never touches anything else: no Windows binary, no service outside XtaCache and the two lists above. DriverEntry returns success and the driver
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
#define SVC_MAX 16                      // names taken from DisableOnM3Services
#define SVC_NAME_MAX 80                 // characters, without the terminator (Services key names are short)
#define SVCLIST_CB 4096                 // DisableOnM3Services bytes read (16 * 80 chars fits)
#define SVCLOG_CB 4096                  // State\Svc bytes
#define SBXTA_TAG 'atxS'

static const UNICODE_STRING kXtaKey =
    RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\XtaCache");
static const UNICODE_STRING kSvcKey =
    RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services");

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
    RtlStringCbLengthW(text, SVCLOG_CB, &cb);
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

// ---- optional per-boot service list (Parameters\DisableOnM3Services) ---------------------------------------------

typedef struct {
    HANDLE services;                    // ...\Services, KEY_READ
    HANDLE saved;                       // ...\Services\sbxta\Saved (created on M3 boots, opened on A55 boots)
    PWSTR log;                          // State\Svc text
    size_t logCb;
    BOOLEAN dirty;                      // something written: flush at the end
} SVCCTX;

static void Log(SVCCTX *c, PCWSTR fmt, ...)
{
    size_t used = 0;
    va_list ap;
    RtlStringCbLengthW(c->log, c->logCb, &used);
    va_start(ap, fmt);
    RtlStringCbVPrintfW(c->log + used / sizeof(WCHAR), c->logCb - used, fmt, ap);   // truncation = shorter log
    va_end(ap);
}

static HANDLE OpenRel(HANDLE root, PCWSTR name, ACCESS_MASK access)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;

    RtlInitUnicodeString(&us, name);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, root, NULL);
    return NT_SUCCESS(ZwOpenKey(&key, access, &oa)) ? key : NULL;
}

// M3: one service key (a listed name or an instance of a listed template).
static void DisableOne(SVCCTX *c, PCWSTR name)
{
    UNICODE_STRING us;
    HANDLE svc = OpenRel(c->services, name, KEY_READ | KEY_SET_VALUE);
    ULONG before, four = 4, after;
    NTSTATUS st;

    if (!svc) {
        Log(c, L"%ws missing; ", name);
        return;
    }
    before = ReadDword(svc, L"Start", NOVALUE);
    if (before == NOVALUE) {
        Log(c, L"%ws no Start; ", name);
    } else if (before == 4) {
        Log(c, L"%ws 4 already; ", name);
    } else {
        // Keep the first saved value: a value already there is the original from an earlier M3 boot.
        if (ReadDword(c->saved, name, NOVALUE) == NOVALUE) {
            RtlInitUnicodeString(&us, name);
            st = ZwSetValueKey(c->saved, &us, 0, REG_DWORD, &before, sizeof before);
            c->dirty = TRUE;
        } else {
            st = STATUS_SUCCESS;
        }
        if (!NT_SUCCESS(st)) {                                  // no way back: leave the service as it is
            Log(c, L"%ws save failed 0x%08X; ", name, st);
        } else {
            RtlInitUnicodeString(&us, L"Start");
            st = ZwSetValueKey(svc, &us, 0, REG_DWORD, &four, sizeof four);
            c->dirty = TRUE;
            after = ReadDword(svc, L"Start", NOVALUE);
            Log(c, L"%ws %d->%d %ws; ", name, (int)before, (int)after, NT_SUCCESS(st) ? L"ok" : L"FAIL");
        }
    }
    ZwClose(svc);
}

// M3: instances <tmpl>_<suffix> of a per-user service template.
static void DisableInstances(SVCCTX *c, PCWSTR tmpl)
{
    UCHAR buf[sizeof(KEY_BASIC_INFORMATION) + 256 * sizeof(WCHAR)];
    PKEY_BASIC_INFORMATION kb = (PKEY_BASIC_INFORMATION)buf;
    WCHAR name[SVC_NAME_MAX + 1];
    size_t tl = 0;
    ULONG i, len, nch;

    RtlStringCchLengthW(tmpl, SVC_NAME_MAX, &tl);
    for (i = 0; i < 100000; i++) {
        NTSTATUS st = ZwEnumerateKey(c->services, i, KeyBasicInformation, kb, sizeof buf, &len);
        if (st == STATUS_NO_MORE_ENTRIES)
            break;
        if (!NT_SUCCESS(st))
            continue;                                           // too long a name or a race: not one of ours
        nch = kb->NameLength / sizeof(WCHAR);
        if (nch <= tl + 1 || nch > SVC_NAME_MAX || kb->Name[tl] != L'_')
            continue;
        RtlCopyMemory(name, kb->Name, nch * sizeof(WCHAR));
        name[nch] = 0;
        if (_wcsnicmp(name, tmpl, tl) == 0)
            DisableOne(c, name);
    }
}

static BOOLEAN NameOk(PCWSTR n, size_t ch)
{
    size_t i;
    if (ch == 0 || ch > SVC_NAME_MAX)
        return FALSE;
    for (i = 0; i < ch; i++)
        if (n[i] == L'\\' || n[i] == L'/')
            return FALSE;
    return TRUE;
}

static void DisableListed(SVCCTX *c, PCWSTR list, ULONG cb)
{
    ULONG nch = cb / sizeof(WCHAR), pos = 0, taken = 0;

    while (pos < nch && list[pos] && taken < SVC_MAX) {
        PCWSTR n = list + pos;
        size_t ch = 0;
        while (pos + ch < nch && n[ch])
            ch++;
        pos += (ULONG)ch + 1;
        if (pos > nch)                                          // unterminated last string: ignore it
            break;
        taken++;
        if (!NameOk(n, ch)) {
            Log(c, L"bad name ignored; ");
            continue;
        }
        DisableOne(c, n);
        {
            HANDLE svc = OpenRel(c->services, n, KEY_READ);
            ULONG type = svc ? ReadDword(svc, L"Type", 0) : 0;
            if (svc)
                ZwClose(svc);
            if (type != NOVALUE && (type & 0x40))               // SERVICE_USER_SERVICE: a per-user template
                DisableInstances(c, n);
        }
    }
    if (pos < nch && list[pos] && taken >= SVC_MAX)
        Log(c, L"list cut at %d; ", SVC_MAX);
}

// A55: put back everything in Saved, list or no list.
static void RestoreSaved(SVCCTX *c)
{
    UCHAR buf[sizeof(KEY_VALUE_FULL_INFORMATION) + (SVC_NAME_MAX + 2) * sizeof(WCHAR) + sizeof(ULONG)];
    PKEY_VALUE_FULL_INFORMATION kv = (PKEY_VALUE_FULL_INFORMATION)buf;
    WCHAR name[SVC_NAME_MAX + 1];
    UNICODE_STRING us;
    ULONG i = 0, n, len, nch;

    for (n = 0; n < 1000; n++) {
        NTSTATUS st = ZwEnumerateValueKey(c->saved, i, KeyValueFullInformation, kv, sizeof buf, &len);
        BOOLEAN ok;
        if (st == STATUS_NO_MORE_ENTRIES)
            break;
        nch = NT_SUCCESS(st) ? kv->NameLength / sizeof(WCHAR) : 0;
        ok = NT_SUCCESS(st) && NameOk(kv->Name, nch);
        if (!ok) {                                              // not ours (too long, odd name): leave it, skip it
            Log(c, L"saved #%u skipped 0x%08X; ", i, st);
            i++;
            continue;
        }
        RtlCopyMemory(name, kv->Name, nch * sizeof(WCHAR));
        name[nch] = 0;
        if (kv->Type == REG_DWORD && kv->DataLength == sizeof(ULONG)) {
            ULONG want = *(ULONG *)((PUCHAR)kv + kv->DataOffset);
            HANDLE svc = OpenRel(c->services, name, KEY_READ | KEY_SET_VALUE);
            ULONG before = svc ? ReadDword(svc, L"Start", NOVALUE) : NOVALUE;
            if (!svc) {
                Log(c, L"%ws missing, dropped; ", name);
            } else if (before != 4) {
                Log(c, L"%ws is %d, left, dropped; ", name, (int)before);
            } else {
                RtlInitUnicodeString(&us, L"Start");
                st = ZwSetValueKey(svc, &us, 0, REG_DWORD, &want, sizeof want);
                Log(c, L"%ws 4->%d %ws; ", name, (int)ReadDword(svc, L"Start", NOVALUE),
                    NT_SUCCESS(st) ? L"ok" : L"FAIL");
            }
            if (svc)
                ZwClose(svc);
        } else {
            Log(c, L"%ws bad saved value, dropped; ", name);
        }
        RtlInitUnicodeString(&us, name);
        st = ZwDeleteValueKey(c->saved, &us);
        c->dirty = TRUE;
        if (!NT_SUCCESS(st))                                    // could not delete: step over it
            i++;
    }
}

static void ServiceStep(PCUNICODE_STRING regPath, BOOLEAN m3, BOOLEAN a55, PWSTR log, size_t logCb)
{
    SVCCTX c = { NULL, NULL, log, logCb, FALSE };
    OBJECT_ATTRIBUTES oa;
    PKEY_VALUE_PARTIAL_INFORMATION list = NULL;
    HANDLE par;

    RtlStringCbCopyW(log, logCb, m3 ? L"M3: " : a55 ? L"A55: " : L"other: nothing");
    if (!m3 && !a55)
        return;
    if (m3) {                                                   // the list is only read on M3 boots
        ULONG len;
        UNICODE_STRING us = RTL_CONSTANT_STRING(L"DisableOnM3Services");
        par = OpenSub(regPath, L"Parameters", FALSE);
        if (par) {
            list = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED, SVCLIST_CB, SBXTA_TAG);
            // Absent, not REG_MULTI_SZ, empty or longer than SVCLIST_CB: off.
            if (list && (!NT_SUCCESS(ZwQueryValueKey(par, &us, KeyValuePartialInformation, list, SVCLIST_CB, &len)) ||
                         list->Type != REG_MULTI_SZ || list->DataLength < sizeof(WCHAR) ||
                         ((PWCHAR)list->Data)[0] == 0)) {
                ExFreePoolWithTag(list, SBXTA_TAG);
                list = NULL;
            }
            ZwClose(par);
        }
        if (!list) {
            Log(&c, L"no DisableOnM3Services, nothing");
            return;
        }
    }
    InitializeObjectAttributes(&oa, (PUNICODE_STRING)&kSvcKey, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&c.services, KEY_READ, &oa))) {
        Log(&c, L"Services not openable");
    } else if (m3) {
        c.saved = OpenSub(regPath, L"Saved", TRUE);
        if (c.saved)
            DisableListed(&c, (PCWSTR)list->Data, list->DataLength);
        else
            Log(&c, L"Saved not creatable, nothing");
    } else {
        c.saved = OpenSub(regPath, L"Saved", FALSE);
        if (c.saved)
            RestoreSaved(&c);
        else
            Log(&c, L"nothing saved");
    }
    if (c.dirty) {                                              // one flush per hive is enough; both are SYSTEM
        ZwFlushKey(c.saved);
        ZwFlushKey(c.services);
    }
    if (c.saved)
        ZwClose(c.saved);
    if (c.services)
        ZwClose(c.services);
    if (list)
        ExFreePoolWithTag(list, SBXTA_TAG);
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
    PWSTR svclog;

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
    svclog = (PWSTR)ExAllocatePool2(POOL_FLAG_PAGED, SVCLOG_CB, SBXTA_TAG);
    if (svclog)
        ServiceStep(regPath, m3, a55, svclog, SVCLOG_CB);

    RtlStringCbPrintfW(result, sizeof result, L"sbxta %S: %ws MIDR %08I64X, XtaCache Start %d -> %d (0x%08X), swap: %ws",
                       SB_BUILD_ID, m3 ? L"M3" : a55 ? L"A55" : L"other", midr, (int)before, (int)after, xst, swaplog);

    state = OpenSub(regPath, L"State", TRUE);
    if (state) {
        WCHAR build[64];
        RtlStringCbPrintfW(build, sizeof build, L"%S v2", SB_BUILD_ID);   // v2: service list step
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
        SetSz(state, L"Svc", svclog ? svclog : L"no memory; service step skipped");
        ZwFlushKey(state);
        ZwClose(state);
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "%ws\n", result);
    if (svclog)
        ExFreePoolWithTag(svclog, SBXTA_TAG);
    return STATUS_SUCCESS;
}

static void SbxtaUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
}
