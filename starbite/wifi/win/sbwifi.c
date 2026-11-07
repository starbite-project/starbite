// SPDX-License-Identifier: GPL-2.0-only
// SbWifi: the BCM4361 under Windows 11 on the S9, stage W1 of docs/WIFI-BCM4361.md section 7: starbite/wifi's
// core in a KMDF driver that runs the bring-up to the step in its service key (Parameters\Step, DWORD, default
// 0 = nothing), writes every line to C:\starbite\sbwifi.txt, then unpowers the radio. No network yet (W3).
//
// Installed as a plain service (no INF, no device), like tools/win's hello-kmdf, because Windows has no device
// for this radio until our UEFI's DSDT gets one (\_SB.WLAN, "STBW4361"). The hardware is reached by physical
// address (MmMapIoSpaceEx); only the core's allow-list is ever written (bcmw_write_allowed, checked again here).
// The firmware, NVRAM and CLM blob are read from C:\Windows\System32\drivers\StarbiteWifi\, where they were
// copied from the phone's own VENDOR (they never leave the phone and are not part of this driver).
//
// Dead-man (as hello-kmdf): when Parameters\OneShot is 1 (the default), DriverEntry first sets the service's
// Start to 4 (disabled) and flushes the key, so a crash costs one boot, not a boot loop.
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <bcrypt.h>
#include "../bcmw.h"

#ifndef SB_BUILD_ID
#define SB_BUILD_ID "unknown"
#endif

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD SbWifiUnload;

#define POOL_TAG 'iWbS'
#define MAPS 24
static struct { ULONGLONG base; PUCHAR va; } maps[MAPS];
static ULONG nmaps;
static HANDLE logfile;
static struct { char name[40]; UCHAR *data; ULONG len; } files[4];
static ULONG nfiles;
static UNICODE_STRING regpath;

// ---- the log ----------------------------------------------------------------------------------------------

static void LogOpen(void)
{
    UNICODE_STRING dir = RTL_CONSTANT_STRING(L"\\??\\C:\\starbite");
    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\??\\C:\\starbite\\sbwifi.txt");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    InitializeObjectAttributes(&oa, &dir, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                                FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0)))
        ZwClose(h);
    InitializeObjectAttributes(&oa, &file, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&logfile, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ, FILE_OPEN_IF,
                                 FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0)))
        logfile = NULL;
}

static void Say(const char *line)
{
    char buf[256];
    size_t n = 0;
    IO_STATUS_BLOCK iosb;

    RtlStringCbPrintfA(buf, sizeof(buf), "%10llu %s\r\n", (unsigned long long)(KeQueryInterruptTime() / 10000), line);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "SbWifi: %s\n", line);
    if (logfile && NT_SUCCESS(RtlStringCbLengthA(buf, sizeof(buf), &n)))
        ZwWriteFile(logfile, NULL, NULL, NULL, &iosb, buf, (ULONG)n, NULL, NULL);
}

// ---- bcmw_os -------------------------------------------------------------------------------------------------

// The register pages, mapped 64 KiB at a time on first use (Device memory, uncached).
static volatile UCHAR *Va(ULONGLONG pa)
{
    ULONGLONG base = pa & ~0xFFFFull;
    for (ULONG i = 0; i < nmaps; i++)
        if (maps[i].base == base)
            return maps[i].va + (pa - base);
    if (nmaps == MAPS)
        return NULL;
    {
        PHYSICAL_ADDRESS p;
        p.QuadPart = (LONGLONG)base;
        maps[nmaps].va = MmMapIoSpaceEx(p, 0x10000, PAGE_READWRITE | PAGE_NOCACHE);
        if (!maps[nmaps].va)
            return NULL;
        maps[nmaps].base = base;
        return maps[nmaps++].va + (pa - base);
    }
}

static uint32_t Rd32(void *ctx, uint64_t pa)
{
    volatile UCHAR *v = Va(pa);
    UNREFERENCED_PARAMETER(ctx);
    return v ? READ_REGISTER_ULONG((volatile ULONG *)v) : 0xffffffffu;
}

static void Wr32(void *ctx, uint64_t pa, uint32_t val)
{
    volatile UCHAR *v;
    UNREFERENCED_PARAMETER(ctx);
    if (!bcmw_write_allowed(pa) || !(v = Va(pa)))
        return;
    WRITE_REGISTER_ULONG((volatile ULONG *)v, val);
}

static uint16_t Rd16(void *ctx, uint64_t pa)
{
    volatile UCHAR *v = Va(pa);
    UNREFERENCED_PARAMETER(ctx);
    return v ? READ_REGISTER_USHORT((volatile USHORT *)v) : 0xffff;
}

static void Wr16(void *ctx, uint64_t pa, uint16_t val)
{
    volatile UCHAR *v;
    UNREFERENCED_PARAMETER(ctx);
    if (!bcmw_write_allowed(pa & ~3ull) || !(v = Va(pa)))
        return;
    WRITE_REGISTER_USHORT((volatile USHORT *)v, val);
}

static void Wmb(void *ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    KeMemoryBarrier();
}

static void Delay(void *ctx, uint32_t us)
{
    UNREFERENCED_PARAMETER(ctx);
    if (us >= 1000) {
        LARGE_INTEGER t;
        t.QuadPart = -10ll * us;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    } else {
        KeStallExecutionProcessor(us);
    }
}

static uint64_t Now(void *ctx)
{
    ULONGLONG qit;
    UNREFERENCED_PARAMETER(ctx);
    KeQueryInterruptTimePrecise(&qit);
    return qit / 10;
}

static void *DmaAlloc(void *ctx, uint32_t size, uint32_t align, uint64_t *bus)
{
    PHYSICAL_ADDRESS lo, hi, bound;
    PVOID p;
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(align);       // contiguous allocations are page aligned
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFF;            // below 4 GiB: the chip is given 32-bit addresses in practice
    bound.QuadPart = 0;
    p = MmAllocateContiguousMemorySpecifyCache(size, lo, hi, bound, MmNonCached);
    if (!p)
        return NULL;
    RtlZeroMemory(p, size);
    *bus = (uint64_t)MmGetPhysicalAddress(p).QuadPart;
    return p;
}

static int File(void *ctx, const char *name, const uint8_t **data, uint32_t *len)
{
    WCHAR path[128];
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION fsi;
    HANDLE h;
    UCHAR *buf;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(ctx);

    for (ULONG i = 0; i < nfiles; i++)
        if (!strcmp(files[i].name, name)) {
            *data = files[i].data, *len = files[i].len;
            return 0;
        }
    if (nfiles == RTL_NUMBER_OF(files) ||
        !NT_SUCCESS(RtlStringCbPrintfW(path, sizeof(path), L"\\SystemRoot\\System32\\drivers\\StarbiteWifi\\%S", name)))
        return -1;
    RtlInitUnicodeString(&u, path);
    InitializeObjectAttributes(&oa, &u, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                      FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st))
        return -1;
    st = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (!NT_SUCCESS(st) || fsi.EndOfFile.QuadPart <= 0 || fsi.EndOfFile.QuadPart > (4 << 20) ||
        !(buf = ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)fsi.EndOfFile.QuadPart, POOL_TAG))) {
        ZwClose(h);
        return -1;
    }
    st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)fsi.EndOfFile.QuadPart, NULL, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(st) || iosb.Information != (ULONG_PTR)fsi.EndOfFile.QuadPart) {
        ExFreePoolWithTag(buf, POOL_TAG);
        return -1;
    }
    RtlStringCbCopyA(files[nfiles].name, sizeof(files[nfiles].name), name);
    files[nfiles].data = buf;
    files[nfiles].len = (ULONG)fsi.EndOfFile.QuadPart;
    *data = buf, *len = files[nfiles].len;
    nfiles++;
    return 0;
}

/* the handshake's nonce: the system's RNG (kernel-mode CNG, cng.lib) */
static int Random(void *ctx, uint8_t *out, uint32_t n)
{
    UNREFERENCED_PARAMETER(ctx);
    return NT_SUCCESS(BCryptGenRandom(NULL, out, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? 0 : -1;
}

static void Log(void *ctx, const char *line)
{
    UNREFERENCED_PARAMETER(ctx);
    Say(line);
}

// ---- the run ---------------------------------------------------------------------------------------------------

static ULONG RegDword(const WCHAR *name, ULONG dflt)
{
    ULONG v = dflt;
    RTL_QUERY_REGISTRY_TABLE t[2];
    ULONG d = dflt;
    WCHAR path[256];

    if (!NT_SUCCESS(RtlStringCbPrintfW(path, sizeof(path), L"%wZ\\Parameters", &regpath)))
        return dflt;
    RtlZeroMemory(t, sizeof(t));
    t[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    t[0].Name = (PWSTR)name;
    t[0].EntryContext = &v;
    t[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    t[0].DefaultData = &d;
    t[0].DefaultLength = sizeof(d);
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, path, t, NULL, NULL)))
        return dflt;
    return v;
}

static void OneShot(void)
{
    ULONG disabled = 4;
    if (RegDword(L"OneShot", 1))
        RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, regpath.Buffer, L"Start", REG_DWORD, &disabled, sizeof(disabled));
}

static KSTART_ROUTINE Run;
static void Run(PVOID arg)
{
    static UCHAR mem[1 << 17];          // struct bcmw (bcmw_size() is checked)
    struct bcmw *w = (struct bcmw *)mem;
    struct bcmw_os os;
    struct bcmw_cfg cfg;
    ULONG step = RegDword(L"Step", 0);
    char line[160];
    int r;
    UNREFERENCED_PARAMETER(arg);

    RtlStringCbPrintfA(line, sizeof(line), "SbWifi build %s: Step %lu (0: nothing is touched)", SB_BUILD_ID, step);
    Say(line);
    if (step == 0 || step > BCMW_STEP_ALL || bcmw_size() > sizeof(mem)) {
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }
    RtlZeroMemory(&os, sizeof(os));
    os.rd32 = Rd32, os.wr32 = Wr32, os.rd16 = Rd16, os.wr16 = Wr16, os.wmb = Wmb, os.mb = Wmb, os.rmb = Wmb;
    os.udelay = Delay, os.now_us = Now, os.dma_alloc = DmaAlloc, os.file = File, os.log = Log, os.random = Random;
    RtlZeroMemory(&cfg, sizeof(cfg));
    cfg.stop_after = step;
    cfg.link_speed = RegDword(L"Gen", 2);
    cfg.link_tries = 3;
    if (bcmw_init(w, &os, &cfg) == BCMW_OK) {
        r = bcmw_bringup(w, 0);
        if (r >= BCMW_STEP_RINGS) {
            for (int i = 0; i < 2000; i++) {   // 10 s of polling: events, the console
                bcmw_poll(w);
                Delay(NULL, 5000);
            }
        }
        RtlStringCbPrintfA(line, sizeof(line), "SbWifi: END: reached step %u (%d)", bcmw_step(w), r);
        bcmw_quiesce(w);
        Say(line);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void SbWifiUnload(WDFDRIVER driver)
{
    UNREFERENCED_PARAMETER(driver);
    Say("SbWifi: unloaded");
    if (logfile)
        ZwClose(logfile);
    for (ULONG i = 0; i < nmaps; i++)
        MmUnmapIoSpace(maps[i].va, 0x10000);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    WDF_DRIVER_CONFIG cfg;
    WDFDRIVER driver;
    HANDLE th;
    NTSTATUS st;

    regpath.Buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, reg->Length + sizeof(WCHAR), POOL_TAG);
    if (!regpath.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    regpath.Length = 0;
    regpath.MaximumLength = reg->Length + sizeof(WCHAR);
    RtlCopyUnicodeString(&regpath, reg);
    regpath.Buffer[regpath.Length / sizeof(WCHAR)] = 0;
    OneShot();
    WDF_DRIVER_CONFIG_INIT(&cfg, WDF_NO_EVENT_CALLBACK);
    cfg.DriverInitFlags |= WdfDriverInitNonPnpDriver;
    cfg.EvtDriverUnload = SbWifiUnload;
    st = WdfDriverCreate(drv, reg, WDF_NO_OBJECT_ATTRIBUTES, &cfg, &driver);
    if (!NT_SUCCESS(st))
        return st;
    LogOpen();
    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, Run, NULL);
    if (NT_SUCCESS(st))
        ZwClose(th);
    return STATUS_SUCCESS;
}
