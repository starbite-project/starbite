// SPDX-License-Identifier: GPL-2.0-only
// SbWifiCx: Wi-Fi for Windows 11 on the Galaxy S9, stage W2 of docs/WIFI-BCM4361.md section 7. A WiFiCx client
// driver (KMDF + NetAdapterCx 2.1 + WiFiCx 1.0) for the ACPI device \_SB.WLAN (STBW4361, starbite/wifi/win/wlan.asl):
// starbite/wifi's core brings the BCM4361 up (Windows cannot enumerate this PCIe controller), and this file maps the
// WDI commands Windows sends (scan, connect, keys, disconnect) onto the core and turns the firmware's events into WDI
// indications. Windows does the WPA2 handshake itself (the host supplicant); the EAPOL frames pass through the data
// path like any other frame.
//
// Threads: one worker thread owns the core (every call into bcmw happens there, at PASSIVE_LEVEL): it takes the WDI
// commands queued by EvtWifiDeviceSendCommand, the frames queued by the tx queue, polls the chip every 5 ms (INTx
// later), and queues received frames for the rx queue. The NetAdapterCx queue callbacks only copy frames in and out.
//
// Not yet: interrupts (polling), power save, WPA3, 6 GHz, roaming, Wi-Fi Direct, statistics. Everything Windows asks
// that is not handled is answered NOT_SUPPORTED and logged once to C:\starbite\sbwificx.txt (with every command id),
// so a phone run shows what is missing.
//
// The radio's firmware, NVRAM and CLM are read from \SystemRoot\System32\drivers\starbite\bcm4361\ (copied there
// from the phone's own VENDOR; never shipped). Network names and keys are never logged.
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <bcrypt.h>
#include <netadaptercx.h>
#include <net/virtualaddress.h>
#include <wificx.h>
#include <TlvGeneratorParser.hpp>

extern "C" {
#include "../bcmw.h"
#include "../wpa.h"
}

#ifndef SB_BUILD_ID
#define SB_BUILD_ID "unknown"
#endif

#define TAG 'xCbS'

// ---- C++ allocation for the WDI TLV library (operator new with an allocation context) ---------------------------

void *__cdecl operator new(size_t n, ULONG_PTR) throw() { return ExAllocatePool2(POOL_FLAG_NON_PAGED, n ? n : 1, TAG); }
void *__cdecl operator new[](size_t n, ULONG_PTR) throw() { return ExAllocatePool2(POOL_FLAG_NON_PAGED, n ? n : 1, TAG); }
void *__cdecl operator new(size_t n) { return ExAllocatePool2(POOL_FLAG_NON_PAGED, n ? n : 1, TAG); }
void *__cdecl operator new[](size_t n) { return ExAllocatePool2(POOL_FLAG_NON_PAGED, n ? n : 1, TAG); }
void __cdecl operator delete(void *p) { if (p) ExFreePoolWithTag(p, TAG); }
void __cdecl operator delete[](void *p) { if (p) ExFreePoolWithTag(p, TAG); }
void __cdecl operator delete(void *p, size_t) { if (p) ExFreePoolWithTag(p, TAG); }
void __cdecl operator delete[](void *p, size_t) { if (p) ExFreePoolWithTag(p, TAG); }

// ---- the log: C:\starbite\sbwificx.txt, written by the worker (PASSIVE) --------------------------------------------

static HANDLE g_log;

static void LogOpen()
{
    if (g_log)
        return;
    UNICODE_STRING dir = RTL_CONSTANT_STRING(L"\\??\\C:\\starbite");
    UNICODE_STRING file = RTL_CONSTANT_STRING(L"\\??\\C:\\starbite\\sbwificx.txt");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    InitializeObjectAttributes(&oa, &dir, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&h, FILE_LIST_DIRECTORY | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                                FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0)))
        ZwClose(h);
    InitializeObjectAttributes(&oa, &file, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&g_log, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ, FILE_OPEN_IF,
                                 FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0)))
        g_log = NULL;
}

static void Say(const char *line)
{
    char buf[300];
    size_t n = 0;
    IO_STATUS_BLOCK iosb;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "SbWifiCx: %s\n", line);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !g_log)
        return;
    RtlStringCbPrintfA(buf, sizeof(buf), "%10llu %s\r\n", (unsigned long long)(KeQueryInterruptTime() / 10000), line);
    if (NT_SUCCESS(RtlStringCbLengthA(buf, sizeof(buf), &n)))
        ZwWriteFile(g_log, NULL, NULL, NULL, &iosb, buf, (ULONG)n, NULL, NULL);
}

static void Sayf(const char *fmt, ...)
{
    char line[240];
    va_list a;
    va_start(a, fmt);
    RtlStringCbVPrintfA(line, sizeof(line), fmt, a);
    va_end(a);
    Say(line);
}

// ---- the device ---------------------------------------------------------------------------------------------------

#define MAX_BSS     48
#define BSS_IE_MAX  768
#define QLEN        64
#define FRAME_MAX   (14 + 2048)

struct BSS {
    UINT8 bssid[6], ssid[32], ssid_len;
    UINT16 capability, beacon_period;
    UINT32 channel, band5, ie_len;
    INT32 rssi;
    UINT64 seen;                    // KeQuerySystemTime when last heard (the BSS entry's age info)
    UINT8 ie[BSS_IE_MAX];
};

struct FRAME {
    UINT32 len;
    UINT8 data[FRAME_MAX];
};

struct DEVCTX {
    WDFDEVICE device;
    NETADAPTER adapter;
    NETPACKETQUEUE txq, rxq;
    UINT16 port;
    struct bcmw *w;
    int up;                         // the radio reached step 10
    // the worker
    HANDLE thread;
    KEVENT wake;
    volatile LONG stop;
    int stopping;                   // under cmd_lock: no more requests are queued (answered NOT_READY)
    int guard_cleared;              // the boot guard was cleared after the radio ran for 30 s
    volatile LONG rx_armed;         // NetAdapterCx asked to be told about new rx frames
    // WDI commands waiting for the worker
    KSPIN_LOCK cmd_lock;
    WIFIREQUEST cmd[16];
    ULONG cmd_head, cmd_count;
    // frames: tx (from Windows to the radio) and rx (from the radio to Windows)
    KSPIN_LOCK tx_lock, rx_lock;
    FRAME *tx, *rx;                 // rings of QLEN
    ULONG tx_head, tx_count, rx_head, rx_count;
    ULONG tx_dropped, rx_dropped;
    // scan
    BSS *bss;
    ULONG nbss;
    int scanning;
    UINT32 scan_txn;
    UINT16 scan_port;
    UINT64 scan_started;
    ULONG scans;                    // scan tasks so far (each is logged: W2 showed 0 networks in Windows)
    ULONG bss_frame;                // Parameters\BssFrame: 0 the frame body (from the timestamp), 1 with a MAC header
    // connection
    int connecting, connected;
    UINT32 connect_txn;
    UINT16 connect_port;
    UINT8 bssid[6], rsn[BCMW_RSN_OURS];
    UINT64 connect_started;
    int set_ssid_ok;
    // the log of commands seen (each id once)
    UINT8 seen[512];
    TLV_CONTEXT tlv;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVCTX, Ctx);

struct ADAPTERCTX {
    DEVCTX *dev;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ADAPTERCTX, AdapterCtx);


// ---- bcmw_os on Windows (as sbwifi.c, W1) -------------------------------------------------------------------------

// 1 MiB mappings, made on first use (W1 run 1: 64 KiB x 24 filled up during the firmware download and the end of
// the chip's RAM was silently never written). A failure is said once, loudly.
#define MAPS 48
#define MAP_SIZE 0x100000ull
static struct { ULONGLONG base; PUCHAR va; } maps[MAPS];
static ULONG nmaps;
static int map_failed;

static volatile UCHAR *Va(ULONGLONG pa)
{
    ULONGLONG base = pa & ~(MAP_SIZE - 1);
    for (ULONG i = 0; i < nmaps; i++)
        if (maps[i].base == base)
            return maps[i].va + (pa - base);
    if (nmaps < MAPS) {
        PHYSICAL_ADDRESS p;
        p.QuadPart = (LONGLONG)base;
        maps[nmaps].va = (PUCHAR)MmMapIoSpaceEx(p, MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (maps[nmaps].va) {
            maps[nmaps].base = base;
            return maps[nmaps++].va + (pa - base);
        }
    }
    if (!map_failed) {
        map_failed = 1;
        Sayf("wificx: MAPPING FAILED at %llx (%lu of %u mappings in use): accesses there are dropped",
             (unsigned long long)pa, nmaps, MAPS);
    }
    return NULL;
}

static uint32_t Rd32(void *, uint64_t pa)
{
    volatile UCHAR *v = Va(pa);
    return v ? READ_REGISTER_ULONG((volatile ULONG *)v) : 0xffffffffu;
}
static void Wr32(void *, uint64_t pa, uint32_t val)
{
    volatile UCHAR *v;
    if (bcmw_write_allowed(pa) && (v = Va(pa)) != NULL)
        WRITE_REGISTER_ULONG((volatile ULONG *)v, val);
}
static uint16_t Rd16(void *, uint64_t pa)
{
    volatile UCHAR *v = Va(pa);
    return v ? READ_REGISTER_USHORT((volatile USHORT *)v) : 0xffff;
}
static void Wr16(void *, uint64_t pa, uint16_t val)
{
    volatile UCHAR *v;
    if (bcmw_write_allowed(pa & ~3ull) && (v = Va(pa)) != NULL)
        WRITE_REGISTER_USHORT((volatile USHORT *)v, val);
}
static void Barrier(void *) { KeMemoryBarrier(); }
static void Delay(void *, uint32_t us)
{
    if (us >= 1000) {
        LARGE_INTEGER t;
        t.QuadPart = -10ll * us;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    } else {
        KeStallExecutionProcessor(us);
    }
}
static uint64_t Now(void *)
{
    ULONGLONG qpc;
    return KeQueryInterruptTimePrecise(&qpc) / 10;     // the return value is the interrupt time; qpc is a QPC count
}
static void *DmaAlloc(void *, uint32_t size, uint32_t, uint64_t *bus)
{
    PHYSICAL_ADDRESS lo, hi, bound;
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFF;
    bound.QuadPart = 0;
    PVOID p = MmAllocateContiguousMemorySpecifyCache(size, lo, hi, bound, MmNonCached);
    if (!p)
        return NULL;
    RtlZeroMemory(p, size);
    *bus = (uint64_t)MmGetPhysicalAddress(p).QuadPart;
    return p;
}
static int Random(void *, uint8_t *out, uint32_t n)
{
    return NT_SUCCESS(BCryptGenRandom(NULL, out, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? 0 : -1;
}

static struct { char name[40]; UCHAR *data; ULONG len; } files[4];
static ULONG nfiles;

static int File(void *, const char *name, const uint8_t **data, uint32_t *len)
{
    for (ULONG i = 0; i < nfiles; i++)
        if (!strcmp(files[i].name, name)) {
            *data = files[i].data, *len = files[i].len;
            return 0;
        }
    WCHAR path[128];
    if (nfiles == RTL_NUMBER_OF(files) ||
        !NT_SUCCESS(RtlStringCbPrintfW(path, sizeof(path), L"\\SystemRoot\\System32\\drivers\\starbite\\bcm4361\\%S", name)))
        return -1;
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION fsi;
    HANDLE h;
    RtlInitUnicodeString(&u, path);
    InitializeObjectAttributes(&oa, &u, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                 FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0)))
        return -1;
    UCHAR *buf = NULL;
    if (NT_SUCCESS(ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation)) &&
        fsi.EndOfFile.QuadPart > 0 && fsi.EndOfFile.QuadPart <= (4 << 20) &&
        (buf = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)fsi.EndOfFile.QuadPart, TAG)) != NULL &&
        NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)fsi.EndOfFile.QuadPart, NULL, NULL)) &&
        iosb.Information == (ULONG_PTR)fsi.EndOfFile.QuadPart) {
        ZwClose(h);
        RtlStringCbCopyA(files[nfiles].name, sizeof(files[nfiles].name), name);
        files[nfiles].data = buf;
        files[nfiles].len = (ULONG)fsi.EndOfFile.QuadPart;
        *data = buf, *len = files[nfiles].len;
        nfiles++;
        return 0;
    }
    ZwClose(h);
    if (buf)
        ExFreePoolWithTag(buf, TAG);
    return -1;
}

static void OsLog(void *, const char *line) { Say(line); }

static void OsEvent(void *ctx, const struct bcmw_event *ev);
static void OsRx(void *ctx, unsigned ifidx, const uint8_t *frame, uint32_t len);

// ---- WDI messages ---------------------------------------------------------------------------------------------------

// an indication: header + the TLVs of a generated message (Generate*() leaves room for the header)
static void Indicate(DEVCTX *d, UINT16 id, UINT16 port, UINT32 txn, NDIS_STATUS status, UINT8 *gen, ULONG gen_len)
{
    UINT8 empty[sizeof(WDI_MESSAGE_HEADER)];
    UINT8 *buf = gen ? gen : empty;
    ULONG len = gen ? gen_len : sizeof(WDI_MESSAGE_HEADER);
    WDI_MESSAGE_HEADER *h = (WDI_MESSAGE_HEADER *)buf;
    RtlZeroMemory(h, sizeof(*h));
    h->PortId = port;
    h->Status = status;
    h->TransactionId = txn;
    WDFMEMORY mem;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = d->device;
    PVOID p;
    if (NT_SUCCESS(WdfMemoryCreate(&a, NonPagedPoolNx, TAG, len, &mem, &p))) {
        RtlCopyMemory(p, buf, len);
        WifiDeviceReceiveIndication(d->device, id, mem);
        WdfObjectDelete(mem);
    }
    if (gen)
        FreeGenerated(gen);
}

// a request answered with just its header (status), the usual answer to a task and to most sets
static void Answer(WIFIREQUEST r, NTSTATUS st)
{
    UINT in = 0, out = 0;
    WDI_MESSAGE_HEADER *h = (WDI_MESSAGE_HEADER *)WifiRequestGetInOutBuffer(r, &in, &out);
    ULONG n = 0;
    if (h && out >= sizeof(*h)) {
        h->Status = NT_SUCCESS(st) ? NDIS_STATUS_SUCCESS : NDIS_STATUS_NOT_SUPPORTED;
        n = sizeof(*h);
    }
    WifiRequestComplete(r, st, n);
}

// ---- scanning ------------------------------------------------------------------------------------------------------

static const UINT16 SCAN_SYNC = 0x5743;     // "WC"

static void ScanDone(DEVCTX *d, NDIS_STATUS status)
{
    WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS list;
    WDI_BSS_ENTRY_CONTAINER *e = NULL;
    UINT8 *gen = NULL;
    ULONG len = 0;
    ULONG n = d->nbss;

    if (n) {
        e = new (0) WDI_BSS_ENTRY_CONTAINER[n];
        if (e) {
            for (ULONG i = 0; i < n; i++) {
                BSS *b = &d->bss[i];
                RtlCopyMemory(e[i].BSSID.Address, b->bssid, 6);
                // the frame's body: timestamp (8), beacon interval (2), capability (2), then the elements;
                // BssFrame 1 puts a probe response's MAC header in front (WDI does not say which it wants)
                ULONG hdr = d->bss_frame == 1 ? 24 : 0;
                UINT8 *f = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, hdr + 12 + b->ie_len, TAG);
                if (f) {
                    UINT8 *body = f + hdr;
                    RtlZeroMemory(f, hdr + 12);
                    if (hdr) {
                        f[0] = 0x50;                            // management, probe response
                        RtlFillMemory(f + 4, 6, 0xff);          // to: broadcast
                        RtlCopyMemory(f + 10, b->bssid, 6);     // from and BSSID: the access point
                        RtlCopyMemory(f + 16, b->bssid, 6);
                    }
                    body[8] = (UINT8)b->beacon_period, body[9] = (UINT8)(b->beacon_period >> 8);
                    body[10] = (UINT8)b->capability, body[11] = (UINT8)(b->capability >> 8);
                    RtlCopyMemory(body + 12, b->ie, b->ie_len);
                    e[i].Optional.ProbeResponseFrame_IsPresent = 1;
                    e[i].ProbeResponseFrame.SimpleSet(f, hdr + 12 + b->ie_len, 0);
                    ExFreePoolWithTag(f, TAG);
                }
                e[i].Optional.EntryAgeInfo_IsPresent = 1;
                e[i].EntryAgeInfo.HostTimeStamp = b->seen;
                e[i].EntryAgeInfo.CachedInformation = FALSE;  // heard in this scan
                e[i].SignalInfo.RSSI = b->rssi;
                e[i].SignalInfo.LinkQuality = b->rssi >= -50 ? 100 : b->rssi <= -100 ? 0 : (UINT32)(2 * (b->rssi + 100));
                e[i].ChannelInfo.ChannelNumber = b->channel;
                e[i].ChannelInfo.BandId = b->band5 ? WDI_BAND_ID_5000 : WDI_BAND_ID_2400;
            }
            list.Optional.DeviceDescriptor_IsPresent = 1;
            list.DeviceDescriptor.SimpleAssign(e, n);
            // an unsolicited indication (transaction id 0), as WDI defines it; W2 sent the scan's id: 0 networks
            NDIS_STATUS gs = GenerateWdiIndicationBssEntryListFromIhv(&list, sizeof(WDI_MESSAGE_HEADER), &d->tlv, &len, &gen);
            if (gs == NDIS_STATUS_SUCCESS) {
                Sayf("wificx: BSS list: %lu entries, %lu bytes, unsolicited (frame %s); the first: channel %lu %s GHz, "
                     "%ld dBm, %lu bytes of elements", n, len, d->bss_frame == 1 ? "with MAC header" : "body",
                     d->bss[0].channel, d->bss[0].band5 ? "5" : "2.4", (long)d->bss[0].rssi, d->bss[0].ie_len);
                Indicate(d, WDI_INDICATION_BSS_ENTRY_LIST, d->scan_port, WDI_TRANSACTION_ID_UNSOLICIT,
                         NDIS_STATUS_SUCCESS, gen, len);
            } else {
                Sayf("wificx: the BSS list could not be generated (%08x)", gs);
            }
            list.DeviceDescriptor.SimpleAssign(NULL, 0);
            delete[] e;
        }
    }
    Sayf("wificx: scan %lu complete: %lu access points, status %08x", d->scans, n, status);
    Indicate(d, WDI_INDICATION_SCAN_COMPLETE, d->scan_port, d->scan_txn, status, NULL, 0);
    d->scanning = 0;
}

static void CmdScan(DEVCTX *d, WIFIREQUEST r, WDI_MESSAGE_HEADER *h, UINT in)
{
    WDI_SCAN_PARAMETERS p;
    const UINT8 *ssid = NULL;
    UINT32 ssid_len = 0;
    if (ParseWdiTaskScanToIhv(in - sizeof(*h), (UINT8 *)(h + 1), &d->tlv, &p) == NDIS_STATUS_SUCCESS) {
        // one name only (directed); Windows usually asks for none, or for the networks it knows
        if (p.SSIDList.ElementCount == 1 && p.SSIDList.pElements[0].ElementCount <= 32) {
            ssid = p.SSIDList.pElements[0].pElements;
            ssid_len = p.SSIDList.pElements[0].ElementCount;
        }
    }
    d->scan_txn = h->TransactionId;
    d->scan_port = h->PortId;
    d->scans++;
    Sayf("wificx: scan %lu asked (transaction %lu, %s)", d->scans, h->TransactionId, ssid ? "one name" : "all networks");
    d->nbss = 0;
    d->scanning = 1;                // before the start: its ioctl wait already delivers results
    d->scan_started = Now(NULL);
    int e = d->up ? bcmw_scan_start(d->w, ssid, ssid_len, SCAN_SYNC) : BCMW_E_STATE;
    CleanupParsedWdiTaskScanToIhv(&p);
    Answer(r, STATUS_SUCCESS);
    if (e != BCMW_OK) {
        Sayf("wificx: the scan could not start (%d)", e);
        d->scanning = 1;
        ScanDone(d, NDIS_STATUS_FAILURE);
    }
}

// ---- connecting -----------------------------------------------------------------------------------------------------

static BSS *FindBss(DEVCTX *d, const UINT8 *bssid)
{
    for (ULONG i = 0; i < d->nbss; i++)
        if (RtlEqualMemory(d->bss[i].bssid, bssid, 6))
            return &d->bss[i];
    return NULL;
}

static void ConnectDone(DEVCTX *d, int ok, UINT32 reason)
{
    WDI_INDICATION_ASSOCIATION_RESULT_LIST list;
    WDI_ASSOCIATION_RESULT_CONTAINER res;
    UINT8 *gen = NULL;
    ULONG len = 0;
    UINT8 req[512], resp[512];
    UINT32 req_len = sizeof(req) - 4, resp_len = sizeof(resp) - 6;
    UINT8 reqf[520], respf[520];
    BSS *b = FindBss(d, d->bssid);
    // the IEs first: a deauthentication arriving during that query (set_ssid_ok < 0) turns this into a failure
    int have_ies = ok && bcmw_assoc_ies(d->w, req, &req_len, resp, &resp_len) == BCMW_OK;
    if (ok && d->set_ssid_ok < 0) {
        Say("wificx: deauthenticated right after associating");
        ok = 0, reason = 1;
    }

    RtlCopyMemory(res.BSSID.Address, d->bssid, 6);
    res.AssociationResultParameters.AssociationStatus = ok ? WDI_ASSOC_STATUS_SUCCESS : WDI_ASSOC_STATUS_FAILURE;
    res.AssociationResultParameters.StatusCode = ok ? 0 : reason;
    res.AssociationResultParameters.AuthAlgorithm = d->rsn[0] ? WDI_AUTH_ALGO_RSNA_PSK : WDI_AUTH_ALGO_80211_OPEN;
    res.AssociationResultParameters.UnicastCipherAlgorithm = d->rsn[0] ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
    res.AssociationResultParameters.MulticastDataCipherAlgorithm =
        !d->rsn[0] ? WDI_CIPHER_ALGO_NONE : d->rsn[7] == BCMW_CIPHER_TKIP ? WDI_CIPHER_ALGO_TKIP : WDI_CIPHER_ALGO_CCMP;
    res.AssociationResultParameters.MulticastMgmtCipherAlgorithm = WDI_CIPHER_ALGO_NONE;
    res.AssociationResultParameters.BandID = b && b->band5 ? WDI_BAND_ID_5000 : WDI_BAND_ID_2400;
    WDI_PHY_TYPE phy = b && b->band5 ? WDI_PHY_TYPE_VHT : WDI_PHY_TYPE_HT;    // required since WDI 1.1.4
    res.ActivePhyTypeList.SimpleAssign(&phy, 1);
    if (ok && have_ies) {
        // the frames' bodies: request = capability, listen interval, elements; response = capability, status, AID, elements
        RtlZeroMemory(reqf, 4);
        reqf[0] = 0x31, reqf[1] = 0x04;         // ESS, privacy, short slot time
        reqf[2] = 10;                           // listen interval
        RtlCopyMemory(reqf + 4, req, req_len);
        res.Optional.AssociationRequestFrame_IsPresent = 1;
        res.AssociationRequestFrame.SimpleSet(reqf, 4 + req_len, 0);
        RtlZeroMemory(respf, 6);
        if (b)
            respf[0] = (UINT8)b->capability, respf[1] = (UINT8)(b->capability >> 8);
        RtlCopyMemory(respf + 6, resp, resp_len);
        res.Optional.AssociationResponseFrame_IsPresent = 1;
        res.AssociationResponseFrame.SimpleSet(respf, 6 + resp_len, 0);
    }
    if (ok && b) {
        UINT8 *body = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 12 + b->ie_len, TAG);
        if (body) {
            RtlZeroMemory(body, 12);
            body[8] = (UINT8)b->beacon_period, body[9] = (UINT8)(b->beacon_period >> 8);
            body[10] = (UINT8)b->capability, body[11] = (UINT8)(b->capability >> 8);
            RtlCopyMemory(body + 12, b->ie, b->ie_len);
            res.Optional.BeaconProbeResponse_IsPresent = 1;
            res.BeaconProbeResponse.SimpleSet(body, 12 + b->ie_len, 0);
            ExFreePoolWithTag(body, TAG);
        }
    }
    list.AssociationResults.SimpleAssign(&res, 1);
    if (ok)
        d->connected = 1;           // before Windows hears it: its handshake message 2 may follow at once
    if (GenerateWdiIndicationAssociationResultFromIhv(&list, sizeof(WDI_MESSAGE_HEADER), &d->tlv, &len, &gen) ==
        NDIS_STATUS_SUCCESS)
        Indicate(d, WDI_INDICATION_ASSOCIATION_RESULT, d->connect_port, d->connect_txn, NDIS_STATUS_SUCCESS, gen, len);
    else
        Say("wificx: the association result could not be generated");
    list.AssociationResults.SimpleAssign(NULL, 0);
    res.ActivePhyTypeList.SimpleAssign(NULL, 0);
    Indicate(d, WDI_INDICATION_CONNECT_COMPLETE, d->connect_port, d->connect_txn,
             ok ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE, NULL, 0);
    Sayf("wificx: connect %s", ok ? "COMPLETE (associated; Windows does the handshake now)" : "FAILED");
    d->connecting = 0;
}

static void CmdConnect(DEVCTX *d, WIFIREQUEST r, WDI_MESSAGE_HEADER *h, UINT in)
{
    WDI_TASK_CONNECT_PARAMETERS p;
    int e = BCMW_E_REFUSED;
    d->connect_txn = h->TransactionId;
    d->connect_port = h->PortId;
    RtlZeroMemory(d->rsn, sizeof(d->rsn));
    if (ParseWdiTaskConnectToIhv(in - sizeof(*h), (UINT8 *)(h + 1), &d->tlv, &p) != NDIS_STATUS_SUCCESS) {
        Answer(r, STATUS_INVALID_PARAMETER);
        return;
    }
    WDI_CONNECT_PARAMETERS_CONTAINER *c = &p.ConnectParameters;
    int psk = 0, open = 0, tkip = 0;
    for (UINT32 i = 0; i < c->AuthenticationAlgorithms.ElementCount; i++) {
        psk |= c->AuthenticationAlgorithms.pElements[i] == WDI_AUTH_ALGO_RSNA_PSK;
        open |= c->AuthenticationAlgorithms.pElements[i] == WDI_AUTH_ALGO_80211_OPEN;
    }
    for (UINT32 i = 0; i < c->MulticastCipherAlgorithms.ElementCount; i++)
        tkip |= c->MulticastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_TKIP;
    if (c->SSIDList.ElementCount >= 1 && p.PreferredBSSEntryList.ElementCount >= 1 && (psk || open) && d->up) {
        const WDI_SSID *s = &c->SSIDList.pElements[0];
        RtlCopyMemory(d->bssid, p.PreferredBSSEntryList.pElements[0].BSSID.Address, 6);
        BSS *b = FindBss(d, d->bssid);
        if (psk) {
            unsigned group = BCMW_CIPHER_CCMP;
            if (b) {
                const uint8_t *rsn = bcmw_ie_find(b->ie, b->ie_len, 48);
                if (rsn)
                    bcmw_rsn_check(rsn, 2u + rsn[1], &group);
            }
            bcmw_rsn_ours(d->rsn, group);
            tkip = group == BCMW_CIPHER_TKIP;
        }
        Sayf("wificx: connect: %s, %u BSS candidates (names not logged)", psk ? "WPA2-PSK (Windows' supplicant)" : "open",
             p.PreferredBSSEntryList.ElementCount);
        d->connecting = 1;
        d->set_ssid_ok = 0;
        d->connect_started = Now(NULL);
        e = bcmw_connect(d->w, s->pElements, s->ElementCount, d->bssid, psk ? d->rsn : NULL, psk ? BCMW_RSN_OURS : 0, tkip);
    } else {
        Sayf("wificx: connect refused: %u names, %u BSS, auth %s", c->SSIDList.ElementCount,
             p.PreferredBSSEntryList.ElementCount, psk ? "PSK" : open ? "open" : "other (WPA3/802.1X: not yet)");
    }
    CleanupParsedWdiTaskConnectToIhv(&p);
    Answer(r, STATUS_SUCCESS);
    if (e != BCMW_OK) {
        d->connecting = 1;
        ConnectDone(d, 0, 1);
    }
}

static void Disconnected(DEVCTX *d, UINT32 reason, int by_host)
{
    WDI_INDICATION_DISASSOCIATION_PARAMETERS p;
    UINT8 *gen = NULL;
    ULONG len = 0;
    if (!d->connected)
        return;
    d->connected = 0;
    RtlCopyMemory(p.DisconnectIndicationParameters.MacAddress.Address, d->bssid, 6);
    p.DisconnectIndicationParameters.DisassociationWABIReason =
        by_host ? WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST : WDI_ASSOC_STATUS_PEER_DISASSOCIATED;
    if (GenerateWdiIndicationDisassociationFromIhv(&p, sizeof(WDI_MESSAGE_HEADER), &d->tlv, &len, &gen) == NDIS_STATUS_SUCCESS)
        Indicate(d, WDI_INDICATION_DISASSOCIATION, d->connect_port, 0, NDIS_STATUS_SUCCESS, gen, len);
    Sayf("wificx: disassociated (reason %u, %s)", reason, by_host ? "by the host" : "by the access point");
}

static void Forget(DEVCTX *d)
{
    // a reset or disconnect ends the scan and connect tasks: no late completions with their old transaction ids
    d->scanning = 0;
    d->connecting = 0;
    d->set_ssid_ok = 0;
}

static void CmdDisconnect(DEVCTX *d, WIFIREQUEST r, const WDI_MESSAGE_HEADER *h)
{
    Answer(r, STATUS_SUCCESS);
    Forget(d);
    if (d->up)
        bcmw_leave(d->w);
    Disconnected(d, 3, 1);
    Indicate(d, WDI_INDICATION_DISCONNECT_COMPLETE, h->PortId, h->TransactionId, NDIS_STATUS_SUCCESS, NULL, 0);
}

static void CmdAddKeys(DEVCTX *d, WIFIREQUEST r, WDI_MESSAGE_HEADER *h, UINT in)
{
    WDI_SET_ADD_CIPHER_KEYS_PARAMETERS p;
    NTSTATUS st = STATUS_SUCCESS;
    if (ParseWdiSetAddCipherKeysToIhv(in - sizeof(*h), (UINT8 *)(h + 1), &d->tlv, &p) != NDIS_STATUS_SUCCESS) {
        Answer(r, STATUS_INVALID_PARAMETER);
        return;
    }
    for (UINT32 i = 0; i < p.SetCipherKey.ElementCount; i++) {
        WDI_SET_ADD_CIPHER_KEYS_CONTAINER *k = &p.SetCipherKey.pElements[i];
        const UINT8 *ea = k->Optional.PeerMacAddress_IsPresent && k->CipherKeyTypeInfo.KeyType == WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY
                              ? k->PeerMacAddress.Address : NULL;
        UINT32 index = k->Optional.CipherKeyID_IsPresent ? k->CipherKeyID.CipherKeyID : 0;
        const UINT8 *rsc = k->Optional.ReceiveSequenceCount_IsPresent ? k->ReceiveSequenceCount.ReceiveSequenceCount : NULL;
        int e = BCMW_E_REFUSED;
        if (k->Optional.CCMPKey_IsPresent && k->CCMPKey.ElementCount == 16)
            e = bcmw_set_key(d->w, ea ? 0 : index, k->CCMPKey.pElements, 16, 4, ea, rsc);
        else if (k->Optional.TKIPInfo_IsPresent && k->TKIPInfo.TKIPKey.ElementCount == 16 &&
                 k->TKIPInfo.TKIPMIC.ElementCount == 16) {
            UINT8 t[32];
            RtlCopyMemory(t, k->TKIPInfo.TKIPKey.pElements, 16);
            RtlCopyMemory(t + 16, k->TKIPInfo.TKIPMIC.pElements, 16);
            e = bcmw_set_key(d->w, ea ? 0 : index, t, 32, 2, ea, rsc);
            RtlSecureZeroMemory(t, sizeof(t));
        }
        Sayf("wificx: a %s key %s", ea ? "pairwise" : "group", e == BCMW_OK ? "given to the firmware" : "REFUSED");
        if (e != BCMW_OK)
            st = STATUS_UNSUCCESSFUL;
    }
    CleanupParsedWdiSetAddCipherKeysToIhv(&p);
    Answer(r, st);
}

// ---- the commands ------------------------------------------------------------------------------------------------------

// WDI_ABORT_TASK: a signal, answered with success at once; the task it names is completed as soon as possible (W2
// answered NOT_SUPPORTED to the abort of the first scan, 30 ms after it began)
static void CmdAbort(DEVCTX *d, WIFIREQUEST r, WDI_MESSAGE_HEADER *h, UINT in)
{
    WDI_TASK_ABORT_PARAMETERS p;
    int parsed = ParseWdiAbortTaskToIhv(in - sizeof(*h), (UINT8 *)(h + 1), &d->tlv, &p) == NDIS_STATUS_SUCCESS;
    UINT32 txn = parsed ? p.CancelParameters.OriginalTransactionId : 0;
    ULONG oid = parsed ? (ULONG)p.CancelParameters.OriginalTaskOID : 0;
    if (parsed)
        CleanupParsedWdiAbortTaskToIhv(&p);
    Answer(r, STATUS_SUCCESS);
    Sayf("wificx: abort of task %08lx transaction %lu (%s)", oid, txn,
         !parsed ? "not parsed" : d->scanning && txn == d->scan_txn ? "the scan: completed now" :
         d->connecting && txn == d->connect_txn ? "the connect: failed now" : "nothing running with that id");
    if (!parsed)
        return;
    if (d->scanning && txn == d->scan_txn)
        ScanDone(d, NDIS_STATUS_REQUEST_ABORTED);
    else if (d->connecting && txn == d->connect_txn)
        ConnectDone(d, 0, 1);
}

static void Command(DEVCTX *d, WIFIREQUEST r)
{
    UINT in = 0, out = 0;
    UINT16 id = WifiRequestGetMessageId(r);
    WDI_MESSAGE_HEADER *h = (WDI_MESSAGE_HEADER *)WifiRequestGetInOutBuffer(r, &in, &out);
    if (id < sizeof(d->seen) && !d->seen[id]) {
        d->seen[id] = 1;
        Sayf("wificx: command %u (first time), %u bytes in, %u out", id, in, out);
    }
    if (!h || in < sizeof(*h)) {
        WifiRequestComplete(r, STATUS_INVALID_PARAMETER, 0);
        return;
    }
    const WDI_MESSAGE_HEADER hc = *h;   // the request's buffer is gone once it completes
    switch (id) {
    case WDI_TASK_OPEN:
        Answer(r, STATUS_SUCCESS);
        Indicate(d, WDI_INDICATION_OPEN_COMPLETE, hc.PortId, hc.TransactionId, NDIS_STATUS_SUCCESS, NULL, 0);
        break;
    case WDI_TASK_CLOSE:
        Answer(r, STATUS_SUCCESS);
        Indicate(d, WDI_INDICATION_CLOSE_COMPLETE, hc.PortId, hc.TransactionId, NDIS_STATUS_SUCCESS, NULL, 0);
        break;
    case WDI_TASK_DOT11_RESET:
        Answer(r, STATUS_SUCCESS);
        Forget(d);
        if (d->connected && d->up)
            bcmw_leave(d->w);
        Disconnected(d, 3, 1);
        Indicate(d, WDI_INDICATION_DOT11_RESET_COMPLETE, hc.PortId, hc.TransactionId, NDIS_STATUS_SUCCESS, NULL, 0);
        break;
    case WDI_TASK_SCAN:
        CmdScan(d, r, h, in);
        break;
    case WDI_TASK_CONNECT:
        CmdConnect(d, r, h, in);
        break;
    case WDI_TASK_DISCONNECT:
        CmdDisconnect(d, r, &hc);
        break;
    case WDI_SET_ADD_CIPHER_KEYS:
        CmdAddKeys(d, r, h, in);
        break;
    case WDI_ABORT_TASK:
        CmdAbort(d, r, h, in);
        break;
    case WDI_SET_DELETE_CIPHER_KEYS:
    case WDI_SET_ADAPTER_CONFIGURATION:
    case WDI_SET_RECEIVE_PACKET_FILTER:
    case WDI_SET_MULTICAST_LIST:
    case WDI_SET_PRIVACY_EXEMPTION_LIST:
    case WDI_SET_DEFAULT_KEY_ID:
    case WDI_SET_CONNECTION_QUALITY:
    case WDI_SET_POWER_STATE:
    case WDI_SET_FLUSH_BSS_ENTRY:
        Answer(r, STATUS_SUCCESS);  // accepted; nothing the firmware needs (yet)
        break;
    default:
        Answer(r, STATUS_NOT_SUPPORTED);
        break;
    }
}

// ---- the firmware's events and frames (inside bcmw_poll: no bcmw calls here) --------------------------------------------

static void OsEvent(void *ctx, const struct bcmw_event *ev)
{
    DEVCTX *d = (DEVCTX *)ctx;
    struct bcmw_bss b;
    UINT16 sync = 0;
    if (ev->type == BCMW_E_ESCAN_RESULT) {
        if (bcmw_bss_parse(ev, &b, &sync) && sync == SCAN_SYNC && d->scanning) {
            BSS *e = FindBss(d, b.bssid);
            if (!e && d->nbss < MAX_BSS)
                e = &d->bss[d->nbss++];
            if (e) {
                RtlCopyMemory(e->bssid, b.bssid, 6);
                RtlCopyMemory(e->ssid, b.ssid, b.ssid_len);
                e->ssid_len = b.ssid_len;
                e->capability = b.capability;
                e->beacon_period = b.beacon_period;
                e->channel = b.channel;
                e->band5 = b.band5;
                e->rssi = b.rssi;
                {
                    LARGE_INTEGER t;
                    KeQuerySystemTime(&t);
                    e->seen = (UINT64)t.QuadPart;
                }
                e->ie_len = b.ie_len < BSS_IE_MAX ? b.ie_len : BSS_IE_MAX;
                RtlCopyMemory(e->ie, b.ie, e->ie_len);
            }
        } else if (bcmw_scan_ended(ev, SCAN_SYNC) && d->scanning) {
            d->scanning = 2;            // reported from the worker loop
        }
        return;
    }
    switch (ev->type) {
    case BCMW_E_SET_SSID:
        if (d->connecting)
            d->set_ssid_ok = ev->status == 0 ? 1 : -1;
        break;
    case BCMW_E_LINK:
        if (d->connecting && (ev->flags & 1))
            d->set_ssid_ok = 2;         // associated: reported from the worker loop
        else if (!(ev->flags & 1) && d->connected)
            d->connected = 2;           // lost: reported from the worker loop
        break;
    case BCMW_E_DEAUTH:
    case BCMW_E_DEAUTH_IND:
    case BCMW_E_DISASSOC_IND:
        if (d->connected)
            d->connected = 2;
        else if (d->connecting)
            d->set_ssid_ok = -1;
        break;
    }
}

static void OsRx(void *ctx, unsigned, const uint8_t *frame, uint32_t len)
{
    DEVCTX *d = (DEVCTX *)ctx;
    KIRQL old;
    if (len > FRAME_MAX)
        return;
    KeAcquireSpinLock(&d->rx_lock, &old);
    if (d->rx_count < QLEN) {
        FRAME *f = &d->rx[(d->rx_head + d->rx_count) % QLEN];
        f->len = len;
        RtlCopyMemory(f->data, frame, len);
        d->rx_count++;
    } else {
        d->rx_dropped++;
    }
    if (d->rxq && InterlockedExchange(&d->rx_armed, 0))   // under the lock: the queue cannot go away meanwhile
        NetRxQueueNotifyMoreReceivedPacketsAvailable(d->rxq);
    KeReleaseSpinLock(&d->rx_lock, old);
}

// ---- the worker ------------------------------------------------------------------------------------------------------------

static void SetParam(PCWSTR name, ULONG v);
static KSTART_ROUTINE Worker;
static void Worker(PVOID arg)
{
    DEVCTX *d = (DEVCTX *)arg;
    LARGE_INTEGER t;
    static UINT8 frame[FRAME_MAX];
    UINT64 started = Now(NULL);
    t.QuadPart = -50000;            // 5 ms
    while (!d->stop) {
        if (!d->guard_cleared && Now(NULL) - started > 30000000) {
            // 30 s up without a crash: this start counts as clean (a registry write at shutdown may never reach
            // the disk, so D0Exit's reset alone would let the count climb on ordinary restarts)
            d->guard_cleared = 1;
            SetParam(L"GuardBoots", 0);
            Say("wificx: 30 s up: the boot guard is cleared");
        }
        KIRQL old;
        KeWaitForSingleObject(&d->wake, Executive, KernelMode, FALSE, &t);
        // commands
        for (;;) {
            WIFIREQUEST r = NULL;
            KeAcquireSpinLock(&d->cmd_lock, &old);
            if (d->cmd_count) {
                r = d->cmd[d->cmd_head];
                d->cmd_head = (d->cmd_head + 1) % RTL_NUMBER_OF(d->cmd);
                d->cmd_count--;
            }
            KeReleaseSpinLock(&d->cmd_lock, old);
            if (!r)
                break;
            Command(d, r);
        }
        // frames out
        for (;;) {
            ULONG len = 0;
            KeAcquireSpinLock(&d->tx_lock, &old);
            if (d->tx_count) {
                FRAME *f = &d->tx[d->tx_head];
                len = f->len;
                RtlCopyMemory(frame, f->data, len);
                d->tx_head = (d->tx_head + 1) % QLEN;
                d->tx_count--;
            }
            KeReleaseSpinLock(&d->tx_lock, old);
            if (!len)
                break;
            if (d->up && bcmw_tx(d->w, frame, len) != BCMW_OK)
                d->tx_dropped++;
        }
        if (!d->up)
            continue;
        bcmw_poll(d->w);
        // what the events said
        if (d->scanning == 2 || (d->scanning == 1 && Now(NULL) - d->scan_started > 12000000))
            ScanDone(d, NDIS_STATUS_SUCCESS);
        if (d->connecting) {
            if (d->set_ssid_ok == 2)
                ConnectDone(d, 1, 0);
            else if (d->set_ssid_ok < 0 || Now(NULL) - d->connect_started > 10000000)
                ConnectDone(d, 0, 1);
        }
        if (d->connected == 2) {
            d->connected = 1;
            Disconnected(d, 4, 0);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ---- the data path -----------------------------------------------------------------------------------------------------------

struct QUEUECTX {
    DEVCTX *d;
    NET_RING_COLLECTION const *rings;
    NET_EXTENSION va;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUECTX, QueueCtx);

static EVT_PACKET_QUEUE_ADVANCE TxAdvance;
static void TxAdvance(NETPACKETQUEUE q)
{
    QUEUECTX *c = QueueCtx(q);
    DEVCTX *d = c->d;
    NET_RING *pr = NetRingCollectionGetPacketRing(c->rings), *fr = NetRingCollectionGetFragmentRing(c->rings);
    UINT32 i = pr->BeginIndex;
    while (i != pr->EndIndex) {
        NET_PACKET *p = NetRingGetPacketAtIndex(pr, i);
        if (!p->Ignore) {
            // copied straight into the next free slot (under the lock: at most one frame, ~1.5 KiB)
            KIRQL old;
            KeAcquireSpinLock(&d->tx_lock, &old);
            if (d->tx_count < QLEN && d->connected) {
                FRAME *out = &d->tx[(d->tx_head + d->tx_count) % QLEN];
                UINT32 len = 0, fi = p->FragmentIndex;
                for (UINT32 k = 0; k < p->FragmentCount; k++) {
                    NET_FRAGMENT *f = NetRingGetFragmentAtIndex(fr, fi);
                    NET_FRAGMENT_VIRTUAL_ADDRESS *va = NetExtensionGetFragmentVirtualAddress(&c->va, fi);
                    if (len + f->ValidLength <= FRAME_MAX)
                        RtlCopyMemory(out->data + len, (UINT8 *)va->VirtualAddress + f->Offset, (SIZE_T)f->ValidLength);
                    len += (UINT32)f->ValidLength;
                    fi = NetRingIncrementIndex(fr, fi);
                }
                if (len >= 14 && len <= FRAME_MAX) {
                    out->len = len;
                    d->tx_count++;
                }
            } else {
                d->tx_dropped++;            // full, or not associated: dropped (as a NIC without a link would)
            }
            KeReleaseSpinLock(&d->tx_lock, old);
        }
        fr->BeginIndex = NetRingAdvanceIndex(fr, fr->BeginIndex, p->FragmentCount);
        i = NetRingIncrementIndex(pr, i);
    }
    pr->BeginIndex = i;
    KeSetEvent(&d->wake, IO_NO_INCREMENT, FALSE);
}

static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED TxNotify;
static void TxNotify(NETPACKETQUEUE, BOOLEAN) {}

static EVT_PACKET_QUEUE_CANCEL TxCancel;
static void TxCancel(NETPACKETQUEUE q)
{
    QUEUECTX *c = QueueCtx(q);
    NET_RING *pr = NetRingCollectionGetPacketRing(c->rings), *fr = NetRingCollectionGetFragmentRing(c->rings);
    for (UINT32 i = pr->BeginIndex; i != pr->EndIndex; i = NetRingIncrementIndex(pr, i))
        NetRingGetPacketAtIndex(pr, i)->Ignore = 1;
    pr->BeginIndex = pr->EndIndex;
    fr->BeginIndex = fr->EndIndex;
}

static EVT_PACKET_QUEUE_ADVANCE RxAdvance;
static void RxAdvance(NETPACKETQUEUE q)
{
    QUEUECTX *c = QueueCtx(q);
    DEVCTX *d = c->d;
    NET_RING *pr = NetRingCollectionGetPacketRing(c->rings), *fr = NetRingCollectionGetFragmentRing(c->rings);
    UINT32 pi = pr->BeginIndex, fi = fr->BeginIndex;
    while (pi != pr->EndIndex && fi != fr->EndIndex) {
        KIRQL old;
        FRAME *f = NULL;
        KeAcquireSpinLock(&d->rx_lock, &old);
        if (d->rx_count)
            f = &d->rx[d->rx_head];
        KeReleaseSpinLock(&d->rx_lock, old);
        if (!f)
            break;
        NET_FRAGMENT *frag = NetRingGetFragmentAtIndex(fr, fi);
        NET_FRAGMENT_VIRTUAL_ADDRESS *va = NetExtensionGetFragmentVirtualAddress(&c->va, fi);
        UINT32 n = f->len <= frag->Capacity ? f->len : (UINT32)frag->Capacity;
        RtlCopyMemory(va->VirtualAddress, f->data, n);
        frag->ValidLength = n;
        frag->Offset = 0;
        NET_PACKET *p = NetRingGetPacketAtIndex(pr, pi);
        p->FragmentIndex = fi;
        p->FragmentCount = 1;
        p->Layout = {};
        p->Ignore = 0;
        KeAcquireSpinLock(&d->rx_lock, &old);
        d->rx_head = (d->rx_head + 1) % QLEN;
        d->rx_count--;
        KeReleaseSpinLock(&d->rx_lock, old);
        pi = NetRingIncrementIndex(pr, pi);
        fi = NetRingIncrementIndex(fr, fi);
    }
    pr->BeginIndex = pi;
    fr->BeginIndex = fi;
}

static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED RxNotify;
static void RxNotify(NETPACKETQUEUE q, BOOLEAN on)
{
    DEVCTX *d = QueueCtx(q)->d;
    InterlockedExchange(&d->rx_armed, on ? 1 : 0);
    if (on) {
        KIRQL old;
        ULONG n;
        KeAcquireSpinLock(&d->rx_lock, &old);
        n = d->rx_count;
        KeReleaseSpinLock(&d->rx_lock, old);
        if (n && InterlockedExchange(&d->rx_armed, 0))
            NetRxQueueNotifyMoreReceivedPacketsAvailable(q);   // frames came while it was not armed
    }
}

static EVT_PACKET_QUEUE_CANCEL RxCancel;
static void RxCancel(NETPACKETQUEUE q)
{
    QUEUECTX *c = QueueCtx(q);
    NET_RING *pr = NetRingCollectionGetPacketRing(c->rings), *fr = NetRingCollectionGetFragmentRing(c->rings);
    for (UINT32 i = pr->BeginIndex; i != pr->EndIndex; i = NetRingIncrementIndex(pr, i))
        NetRingGetPacketAtIndex(pr, i)->Ignore = 1;
    pr->BeginIndex = pr->EndIndex;
    fr->BeginIndex = fr->EndIndex;
}

static EVT_WDF_OBJECT_CONTEXT_DESTROY QueueDestroy;
static void QueueDestroy(WDFOBJECT o)
{
    QUEUECTX *c = QueueCtx(o);
    if (c->d && c->d->txq == (NETPACKETQUEUE)o)
        c->d->txq = NULL;
    if (c->d && c->d->rxq == (NETPACKETQUEUE)o) {
        KIRQL old;
        KeAcquireSpinLock(&c->d->rx_lock, &old);
        InterlockedExchange(&c->d->rx_armed, 0);
        c->d->rxq = NULL;
        KeReleaseSpinLock(&c->d->rx_lock, old);
    }
}

static EVT_NET_ADAPTER_CREATE_TXQUEUE CreateTxQueue;
static NTSTATUS CreateTxQueue(NETADAPTER adapter, NETTXQUEUE_INIT *init)
{
    NET_PACKET_QUEUE_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES a;
    NETPACKETQUEUE q;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, QUEUECTX);
    a.EvtDestroyCallback = QueueDestroy;
    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, TxAdvance, TxNotify, TxCancel);
    NTSTATUS st = NetTxQueueCreate(init, &a, &cfg, &q);
    if (!NT_SUCCESS(st))
        return st;
    QUEUECTX *c = QueueCtx(q);
    c->d = AdapterCtx(adapter)->dev;
    c->rings = NetTxQueueGetRingCollection(q);
    NET_EXTENSION_QUERY qy;
    NET_EXTENSION_QUERY_INIT(&qy, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
                             NetExtensionTypeFragment);
    NetTxQueueGetExtension(q, &qy, &c->va);
    c->d->txq = q;
    return STATUS_SUCCESS;
}

static EVT_NET_ADAPTER_CREATE_RXQUEUE CreateRxQueue;
static NTSTATUS CreateRxQueue(NETADAPTER adapter, NETRXQUEUE_INIT *init)
{
    NET_PACKET_QUEUE_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES a;
    NETPACKETQUEUE q;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, QUEUECTX);
    a.EvtDestroyCallback = QueueDestroy;
    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, RxAdvance, RxNotify, RxCancel);
    NTSTATUS st = NetRxQueueCreate(init, &a, &cfg, &q);
    if (!NT_SUCCESS(st))
        return st;
    QUEUECTX *c = QueueCtx(q);
    c->d = AdapterCtx(adapter)->dev;
    c->rings = NetRxQueueGetRingCollection(q);
    NET_EXTENSION_QUERY qy;
    NET_EXTENSION_QUERY_INIT(&qy, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
                             NetExtensionTypeFragment);
    NetRxQueueGetExtension(q, &qy, &c->va);
    c->d->rxq = q;
    return STATUS_SUCCESS;
}

// ---- WiFiCx: the adapter, the commands --------------------------------------------------------------------------------------

static EVT_WIFI_DEVICE_CREATE_ADAPTER CreateAdapter;
static NTSTATUS CreateAdapter(WDFDEVICE device, NETADAPTER_INIT *init)
{
    DEVCTX *d = Ctx(device);
    NET_ADAPTER_DATAPATH_CALLBACKS dp;
    if (!d->up) {
        Say("wificx: an adapter was asked for, but the radio is not up");
        return STATUS_DEVICE_NOT_READY;
    }
    WDF_OBJECT_ATTRIBUTES a;
    NETADAPTER adapter;
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&dp, CreateTxQueue, CreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(init, &dp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, ADAPTERCTX);
    NTSTATUS st = NetAdapterCreate(init, &a, &adapter);
    if (!NT_SUCCESS(st)) {
        Sayf("wificx: NetAdapterCreate failed %08x", st);
        return st;
    }
    AdapterCtx(adapter)->dev = d;
    if (!NT_SUCCESS(st = WifiAdapterInitialize(adapter))) {
        Sayf("wificx: WifiAdapterInitialize failed %08x", st);
        return st;
    }
    NET_ADAPTER_LINK_LAYER_CAPABILITIES ll;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&ll, 433000000ull, 433000000ull);
    NetAdapterSetLinkLayerCapabilities(adapter, &ll);
    NetAdapterSetLinkLayerMtuSize(adapter, 1500);
    NET_ADAPTER_LINK_LAYER_ADDRESS mac;
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&mac, 6, bcmw_info(d->w)->mac);
    NetAdapterSetPermanentLinkLayerAddress(adapter, &mac);
    NetAdapterSetCurrentLinkLayerAddress(adapter, &mac);
    NET_ADAPTER_TX_CAPABILITIES tx;
    NET_ADAPTER_TX_CAPABILITIES_INIT(&tx, 1);
    NET_ADAPTER_RX_CAPABILITIES rx;
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&rx, FRAME_MAX, 1);
    NetAdapterSetDataPathCapabilities(adapter, &tx, &rx);
    d->adapter = adapter;
    d->port = WifiAdapterGetPortId(adapter);
    st = NetAdapterStart(adapter);
    Sayf("wificx: the station adapter: port %u, started %08x", d->port, st);
    return st;
}

static EVT_WIFI_DEVICE_SEND_COMMAND SendCommand;
static void SendCommand(WDFDEVICE device, WIFIREQUEST r)
{
    DEVCTX *d = Ctx(device);
    KIRQL old;
    int queued = 0;
    KeAcquireSpinLock(&d->cmd_lock, &old);
    if (!d->stopping && d->thread && d->cmd_count < RTL_NUMBER_OF(d->cmd)) {
        d->cmd[(d->cmd_head + d->cmd_count) % RTL_NUMBER_OF(d->cmd)] = r;
        d->cmd_count++;
        queued = 1;
    }
    KeReleaseSpinLock(&d->cmd_lock, old);
    if (queued)
        KeSetEvent(&d->wake, IO_NO_INCREMENT, FALSE);
    else
        WifiRequestComplete(r, STATUS_DEVICE_BUSY, 0);
}

// ---- capabilities -----------------------------------------------------------------------------------------------------------------

static NTSTATUS SetCapabilities(DEVCTX *d)
{
    WIFI_DEVICE_CAPABILITIES dc;
    WIFI_DEVICE_CAPABILITIES_INIT(&dc);
    dc.HardwareRadioState = TRUE;
    dc.SoftwareRadioState = TRUE;
    RtlStringCbCopyA((char *)dc.FirmwareVersion, sizeof(dc.FirmwareVersion), bcmw_info(d->w)->fw_version);
    dc.NumRxStreams = 2;
    dc.NumTxStreams = 2;
    NTSTATUS st = WifiDeviceSetDeviceCapabilities(d->device, &dc);
    if (!NT_SUCCESS(st))
        return Sayf("wificx: device capabilities refused %08x", st), st;

    static DOT11_AUTH_CIPHER_PAIR uni[] = { { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
                                            { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP } };
    static DOT11_AUTH_CIPHER_PAIR multi[] = { { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
                                              { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP },
                                              { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_TKIP } };
    WIFI_STATION_CAPABILITIES sc;
    WIFI_STATION_CAPABILITIES_INIT(&sc);
    sc.ScanSSIDListSize = 4;
    sc.DesiredSSIDListSize = 1;
    sc.PrivacyExemptionListSize = 32;
    sc.KeyMappingTableSize = 32;
    sc.DefaultKeyTableSize = 4;
    sc.WEPKeyValueMaxLength = 0;
    sc.MaxNumPerSTA = 4;
    sc.NumSupportedUnicastAlgorithms = RTL_NUMBER_OF(uni);
    sc.UnicastAlgorithmsList = uni;
    sc.NumSupportedMulticastDataAlgorithms = RTL_NUMBER_OF(multi);
    sc.MulticastDataAlgorithmsList = multi;
    if (!NT_SUCCESS(st = WifiDeviceSetStationCapabilities(d->device, &sc)))
        return Sayf("wificx: station capabilities refused %08x", st), st;

    static WDI_PHY_TYPE phy24[] = { WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT }, phy5[] = { WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT, WDI_PHY_TYPE_VHT };
    static WDI_CHANNEL_MAPPING_ENTRY ch24[13], ch5[25];
    static const UINT8 c5[25] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165 };
    static UINT32 widths[] = { 20 };
    for (UINT32 i = 0; i < 13; i++)
        ch24[i].ChannelNumber = i + 1, ch24[i].ChannelCenterFrequency = 2412 + 5 * i;
    for (UINT32 i = 0; i < 25; i++)
        ch5[i].ChannelNumber = c5[i], ch5[i].ChannelCenterFrequency = 5000 + 5 * c5[i];
    static WIFI_BAND_INFO bands[2];
    bands[0].BandID = WDI_BAND_ID_2400, bands[0].BandState = TRUE, bands[0].NumValidPhyTypes = 2, bands[0].ValidPhyTypeList = phy24;
    bands[0].NumValidChannelTypes = 13, bands[0].ValidChannelTypes = ch24, bands[0].NumChannelWidths = 1, bands[0].ChannelWidthList = widths;
    bands[1].BandID = WDI_BAND_ID_5000, bands[1].BandState = TRUE, bands[1].NumValidPhyTypes = 3, bands[1].ValidPhyTypeList = phy5;
    bands[1].NumValidChannelTypes = 25, bands[1].ValidChannelTypes = ch5, bands[1].NumChannelWidths = 1, bands[1].ChannelWidthList = widths;
    WIFI_BAND_CAPABILITIES bc;
    RtlZeroMemory(&bc, sizeof(bc));
    bc.Size = sizeof(bc);
    bc.NumBands = 2;
    bc.BandInfoList = bands;
    if (!NT_SUCCESS(st = WifiDeviceSetBandCapabilities(d->device, &bc)))
        return Sayf("wificx: band capabilities refused %08x", st), st;

    static WIFI_PHY_INFO phys[4];
    static const WDI_PHY_TYPE types[4] = { WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT, WDI_PHY_TYPE_VHT };
    static const UINT16 rates[8] = { 12, 18, 24, 36, 48, 72, 96, 108 };      // 500 kb/s units: 6..54 Mb/s
    for (UINT32 p = 0; p < 4; p++) {
        phys[p].PhyType = types[p];
        phys[p].NumberDataRateEntries = 8;
        for (UINT32 i = 0; i < 8; i++)
            phys[p].DataRateList[i].DataRateFlag = 0, phys[p].DataRateList[i].DataRateValue = rates[i];
    }
    WIFI_PHY_CAPABILITIES pc;
    RtlZeroMemory(&pc, sizeof(pc));
    pc.Size = sizeof(pc);
    pc.NumPhyTypes = 4;
    pc.PhyInfoList = phys;
    if (!NT_SUCCESS(st = WifiDeviceSetPhyCapabilities(d->device, &pc)))
        return Sayf("wificx: PHY capabilities refused %08x", st), st;
    Say("wificx: capabilities given to WiFiCx");
    return STATUS_SUCCESS;
}

// ---- the boot guard: Services\sbwificx\Parameters ----------------------------------------------------------------
// GuardBoots counts starts that did not end in a clean power-down (D0Exit). PnP starts this driver at every boot,
// so a crash in the bring-up would repeat at every boot: after 2 unclean starts the driver leaves the hardware
// alone (Windows boots without Wi-Fi) until GuardBoots is set back to 0. Off = 1 also leaves it alone.

static ULONG Param(PCWSTR name, ULONG def)
{
    WDFKEY key;
    ULONG v = def;
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        if (!NT_SUCCESS(WdfRegistryQueryULong(key, &n, &v)))
            v = def;
        WdfRegistryClose(key);
    }
    return v;
}

static void SetParam(PCWSTR name, ULONG v)
{
    WDFKEY key;
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        WdfRegistryAssignULong(key, &n, v);
        WdfRegistryClose(key);
    }
}

// ---- PnP --------------------------------------------------------------------------------------------------------------------------

static EVT_WDF_DEVICE_PREPARE_HARDWARE PrepareHardware;
static NTSTATUS PrepareHardware(WDFDEVICE device, WDFCMRESLIST, WDFCMRESLIST)
{
    DEVCTX *d = Ctx(device);
    struct bcmw_os os;
    struct bcmw_cfg cfg;
    LogOpen();
    Sayf("SbWifiCx build %s: the radio up (the PCIe controller is ours: Windows does not enumerate it)", SB_BUILD_ID);
    ULONG guard = Param(L"GuardBoots", 0);
    if (Param(L"Off", 0) || guard >= 2) {
        Sayf("wificx: NOT STARTED: %s (the hardware is left alone)",
             guard >= 2 ? "2 starts without a clean power-down (GuardBoots; set it to 0 to retry)" : "Parameters\\Off = 1");
        return STATUS_DEVICE_NOT_READY;
    }
    SetParam(L"GuardBoots", guard + 1);
    // once per device (freed with it); a restart reuses them. The core's DMA buffers are not given back on a
    // restart yet (bcmw has no free call): a few hundred KiB per restart, fine for the tests.
    if (!d->w)
        d->w = (struct bcmw *)ExAllocatePool2(POOL_FLAG_NON_PAGED, bcmw_size(), TAG);
    if (!d->bss)
        d->bss = (BSS *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(BSS) * MAX_BSS, TAG);
    if (!d->tx)
        d->tx = (FRAME *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(FRAME) * QLEN, TAG);
    if (!d->rx)
        d->rx = (FRAME *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(FRAME) * QLEN, TAG);
    d->tx_head = d->tx_count = d->rx_head = d->rx_count = 0;
    d->guard_cleared = 0;
    if (!d->w || !d->bss || !d->tx || !d->rx)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(&os, sizeof(os));
    os.ctx = d;
    os.rd32 = Rd32, os.wr32 = Wr32, os.rd16 = Rd16, os.wr16 = Wr16, os.wmb = Barrier, os.mb = Barrier, os.rmb = Barrier;
    os.udelay = Delay, os.now_us = Now, os.dma_alloc = DmaAlloc, os.file = File, os.log = OsLog, os.random = Random;
    os.event = OsEvent, os.rx = OsRx;
    RtlZeroMemory(&cfg, sizeof(cfg));
    cfg.stop_after = BCMW_STEP_UP;
    cfg.link_tries = 3;
    cfg.rx_buffers = 128;
    cfg.diag = 1;                   // step 8 checked word by word until W2 has passed once
    d->bss_frame = Param(L"BssFrame", 0);
    if (bcmw_init(d->w, &os, &cfg) != BCMW_OK) {
        SetParam(L"GuardBoots", guard);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    int r = bcmw_bringup(d->w, 0);
    d->up = r == BCMW_STEP_UP;
    Sayf("wificx: the radio %s (step %d)", d->up ? "is up" : "DID NOT COME UP", r);
    if (!d->up) {
        bcmw_quiesce(d->w);
        SetParam(L"GuardBoots", guard);     // a clean failure (no crash): not counted
        return STATUS_DEVICE_NOT_READY;
    }
    d->tlv.AllocationContext = 0;
    d->tlv.PeerVersion = WifiDeviceGetOsWdiVersion(device);
    NTSTATUS st = SetCapabilities(d);
    if (!NT_SUCCESS(st)) {
        bcmw_quiesce(d->w);
        d->up = 0;
        SetParam(L"GuardBoots", guard);
        return st;
    }
    // the worker
    d->stop = 0;
    d->stopping = 0;
    st = PsCreateSystemThread(&d->thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, Worker, d);
    return st;
}

static void Stop(DEVCTX *d)
{
    KIRQL old;
    KeAcquireSpinLock(&d->cmd_lock, &old);
    d->stopping = 1;
    KeReleaseSpinLock(&d->cmd_lock, old);
    if (d->thread) {
        PVOID obj;
        d->stop = 1;
        KeSetEvent(&d->wake, IO_NO_INCREMENT, FALSE);
        if (NT_SUCCESS(ObReferenceObjectByHandle(d->thread, SYNCHRONIZE, NULL, KernelMode, &obj, NULL))) {
            KeWaitForSingleObject(obj, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(obj);
        }
        ZwClose(d->thread);
        d->thread = NULL;
    }
    // requests the worker did not take: answered, so WiFiCx never waits on them
    for (;;) {
        WIFIREQUEST r = NULL;
        KeAcquireSpinLock(&d->cmd_lock, &old);
        if (d->cmd_count) {
            r = d->cmd[d->cmd_head];
            d->cmd_head = (d->cmd_head + 1) % RTL_NUMBER_OF(d->cmd);
            d->cmd_count--;
        }
        KeReleaseSpinLock(&d->cmd_lock, old);
        if (!r)
            break;
        WifiRequestComplete(r, STATUS_DEVICE_NOT_READY, 0);
    }
    if (d->w && d->up) {
        bcmw_quiesce(d->w);         // the radio held in reset and unpowered
        d->up = 0;
        Say("wificx: the radio is off");
    }
}

static EVT_WDF_DEVICE_D0_EXIT D0Exit;
static NTSTATUS D0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    DEVCTX *d = Ctx(device);
    if (target == WdfPowerDeviceD3Final)
        Stop(d);                    // removal or shutdown: nothing comes after this
    SetParam(L"GuardBoots", 0);
    Sayf("wificx: D0 exit to %d (clean: the boot guard is reset)", (int)target);
    return STATUS_SUCCESS;
}

static EVT_WDF_DEVICE_RELEASE_HARDWARE ReleaseHardware;
static NTSTATUS ReleaseHardware(WDFDEVICE device, WDFCMRESLIST)
{
    Stop(Ctx(device));
    Say("wificx: released");
    return STATUS_SUCCESS;
}

static EVT_WDF_OBJECT_CONTEXT_CLEANUP DeviceCleanup;
static void DeviceCleanup(WDFOBJECT o)
{
    DEVCTX *d = Ctx((WDFDEVICE)o);
    void *p[4] = { d->w, d->bss, d->tx, d->rx };
    for (int i = 0; i < 4; i++)
        if (p[i])
            ExFreePoolWithTag(p[i], TAG);
    d->w = NULL, d->bss = NULL, d->tx = NULL, d->rx = NULL;
}

static EVT_WDF_DRIVER_DEVICE_ADD DeviceAdd;
static NTSTATUS DeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init)
{
    LogOpen();
    NTSTATUS st = NetDeviceInitConfig(init);
    if (!NT_SUCCESS(st))
        return st;
    if (!NT_SUCCESS(st = WifiDeviceInitConfig(init)))
        return st;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = PrepareHardware;
    pnp.EvtDeviceReleaseHardware = ReleaseHardware;
    pnp.EvtDeviceD0Exit = D0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, DEVCTX);
    a.EvtCleanupCallback = DeviceCleanup;
    WDFDEVICE device;
    if (!NT_SUCCESS(st = WdfDeviceCreate(&init, &a, &device)))
        return st;
    DEVCTX *d = Ctx(device);
    d->device = device;
    KeInitializeEvent(&d->wake, SynchronizationEvent, FALSE);
    KeInitializeSpinLock(&d->cmd_lock);
    KeInitializeSpinLock(&d->tx_lock);
    KeInitializeSpinLock(&d->rx_lock);
    WIFI_DEVICE_CONFIG wc;
    // WDI 2.0.0: the WiFiCx baseline (the SDK's LATEST, 2.0.13, may be newer than 23H2's WiFiCx accepts);
    // Parameters\WdiVersion overrides it for a retry without a rebuild
    ULONG wdi = Param(L"WdiVersion", WDI_VERSION_2_0_0);
    WIFI_DEVICE_CONFIG_INIT(&wc, wdi, SendCommand, CreateAdapter, NULL);
    st = WifiDeviceInitialize(device, &wc);
    Sayf("SbWifiCx build %s: device added; WDI %lu.%lu.%lu asked, WifiDeviceInitialize %08x", SB_BUILD_ID, wdi >> 16,
         (wdi >> 8) & 0xff, wdi & 0xff, st);
    if (NT_SUCCESS(st)) {
        ULONG os = WifiDeviceGetOsWdiVersion(device);
        Sayf("wificx: the OS speaks WDI %lu.%lu.%lu", os >> 16, (os >> 8) & 0xff, os & 0xff);
    }
    return st;
}

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, DeviceAdd);
    return WdfDriverCreate(drv, reg, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
