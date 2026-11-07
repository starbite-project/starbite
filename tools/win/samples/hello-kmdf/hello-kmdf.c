// hello-kmdf: the L0 test driver (docs/PLAN-2026-11-07.md). A non-PnP KMDF driver, installed as a plain
// service (no INF, no device): on every load it appends one line to C:\starbite\hello-kmdf.txt
//   hello-kmdf build <id> loaded <local time> (UTC <utc>) KMDF <major>.<minor> irql <n>
// and stores the same line as REG_SZ "LastLoad" under its service key (Parameters), so the result can be read
// back from TWRP/Linux either from the NTFS volume or from the SYSTEM hive. It touches nothing else.
// Unload (sc stop hello-kmdf) appends an "unloaded" line.
// Dead-man: when Parameters\OneShot is 1 (tools/win/hive-service.py sets it; also the default when missing),
// the very first thing DriverEntry does is set its own service Start to 4 (disabled) and flush the key, so a
// crash anywhere later costs one boot, not a boot loop. Re-arm offline with hive-service.py add.
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>

#ifndef SB_BUILD_ID
#define SB_BUILD_ID "unknown"
#endif

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD HelloUnload;

static void TimeLine(char *buf, size_t cb, const char *what)
{
    LARGE_INTEGER utc, local;
    TIME_FIELDS l, u;
    KeQuerySystemTimePrecise(&utc);
    ExSystemTimeToLocalTime(&utc, &local);
    RtlTimeToTimeFields(&local, &l);
    RtlTimeToTimeFields(&utc, &u);
    RtlStringCbPrintfA(buf, cb,
        "hello-kmdf build %s %s %04u-%02u-%02u %02u:%02u:%02u.%03u (UTC %02u:%02u:%02u) KMDF %u.%u irql %u\r\n",
        SB_BUILD_ID, what, l.Year, l.Month, l.Day, l.Hour, l.Minute, l.Second, l.Milliseconds,
        u.Hour, u.Minute, u.Second, (unsigned)KMDF_VERSION_MAJOR, (unsigned)KMDF_VERSION_MINOR,
        (unsigned)KeGetCurrentIrql());
}

// Append LINE to C:\starbite\hello-kmdf.txt (creates the folder and the file when missing).
static NTSTATUS AppendLine(const char *line)
{
    UNICODE_STRING dir = RTL_CONSTANT_STRING(L"\\??\\C:\\starbite");
    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\??\\C:\\starbite\\hello-kmdf.txt");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS st;
    size_t len = 0;

    InitializeObjectAttributes(&oa, &dir, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_DIRECTORY,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                      FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st))
        return st;
    ZwClose(h);

    InitializeObjectAttributes(&oa, &file, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&h, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ, FILE_OPEN_IF,
                      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0);
    if (!NT_SUCCESS(st))
        return st;
    RtlStringCbLengthA(line, 512, &len);
    st = ZwWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)line, (ULONG)len, NULL, NULL);   // write-through
    ZwClose(h);
    return st;
}

// <RegistryPath>\Parameters\LastLoad = LINE (REG_SZ), plus LastStatus = the file write's NTSTATUS.
static void RecordInRegistry(PCUNICODE_STRING regPath, const char *line, NTSTATUS fileStatus)
{
    WCHAR path[256], wline[256];
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    HANDLE key;
    ULONG disp;
    size_t i;

    if (!NT_SUCCESS(RtlStringCbPrintfW(path, sizeof path, L"%wZ\\Parameters", regPath)))
        return;
    RtlInitUnicodeString(&us, path);
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp)))
        return;
    for (i = 0; line[i] && line[i] != '\r' && i < RTL_NUMBER_OF(wline) - 1; i++)
        wline[i] = (WCHAR)(unsigned char)line[i];
    wline[i] = 0;
    RtlInitUnicodeString(&us, L"LastLoad");
    ZwSetValueKey(key, &us, 0, REG_SZ, wline, (ULONG)((i + 1) * sizeof(WCHAR)));
    RtlInitUnicodeString(&us, L"LastStatus");
    ZwSetValueKey(key, &us, 0, REG_DWORD, &fileStatus, sizeof fileStatus);
    ZwFlushKey(key);
    ZwClose(key);
}

// OneShot: Start = 4 in our own service key, flushed (the flush reaches the hive's log before we go on).
static NTSTATUS Disarm(PUNICODE_STRING regPath, ULONG *oneShot)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us, name;
    HANDLE key, par;
    NTSTATUS st;
    ULONG disabled = 4, len;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION kv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;

    *oneShot = 1;
    InitializeObjectAttributes(&oa, regPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwOpenKey(&key, KEY_READ | KEY_SET_VALUE, &oa);
    if (!NT_SUCCESS(st))
        return st;
    RtlInitUnicodeString(&us, L"Parameters");
    InitializeObjectAttributes(&oa, &us, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, key, NULL);
    if (NT_SUCCESS(ZwOpenKey(&par, KEY_QUERY_VALUE, &oa))) {
        RtlInitUnicodeString(&name, L"OneShot");
        if (NT_SUCCESS(ZwQueryValueKey(par, &name, KeyValuePartialInformation, kv, sizeof buf, &len)) &&
            kv->Type == REG_DWORD && kv->DataLength == sizeof(ULONG))
            *oneShot = *(ULONG *)kv->Data;
        ZwClose(par);
    }
    if (*oneShot) {
        RtlInitUnicodeString(&name, L"Start");
        st = ZwSetValueKey(key, &name, 0, REG_DWORD, &disabled, sizeof disabled);
        if (NT_SUCCESS(st))
            st = ZwFlushKey(key);
    }
    ZwClose(key);
    return st;
}

static VOID HelloUnload(_In_ WDFDRIVER driver)
{
    char line[256];
    UNREFERENCED_PARAMETER(driver);
    TimeLine(line, sizeof line, "unloaded");
    AppendLine(line);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER driver;
    NTSTATUS st, fileSt;
    char line[256];

    ULONG oneShot;
    NTSTATUS disarmSt = Disarm(registryPath, &oneShot);

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = HelloUnload;
    st = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(st))
        return st;

    TimeLine(line, sizeof line, oneShot ? (NT_SUCCESS(disarmSt) ? "loaded (one-shot, now Start=4)"
                                                                 : "loaded (one-shot, DISARM FAILED)")
                                        : "loaded");
    fileSt = AppendLine(line);
    RecordInRegistry(registryPath, line, fileSt);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "%s", line);
    return STATUS_SUCCESS;   // stay loaded even if C: was not reachable; LastStatus says why
}
