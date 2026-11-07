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
    UINT8 l2;                       // tx: the packet's Layout.Layer2Type as Windows gave it
    UINT8 data[FRAME_MAX];
};

// ---- 802.11 <-> Ethernet (W2l): WiFiCx's data path carries 802.11 data frames with an LLC/SNAP header, as WDI's
// did (W2j-a: Windows' first frame was 60 bytes = an ARP in 802.11 form, read as Ethernet its "ethertype" was
// e1f5, two bytes of an address; Windows counted 0 bytes received from the Ethernet frames handed up). The radio
// (msgbuf) takes and gives Ethernet frames.

// the 802.11 data header's length, or 0 when this is not a data frame with an LLC/SNAP header
static ULONG Dot11Header(const UINT8 *f, ULONG len)
{
    if (len < 24 + 8 || (f[0] & 0x0c) != 0x08)
        return 0;                               // not a data frame
    ULONG h = 24;
    if ((f[1] & 3) == 3)
        h += 6;                                 // four addresses
    if (f[0] & 0x80) {
        h += 2;                                 // QoS control
        if (f[1] & 0x80)
            h += 4;                             // HT control (order bit on a QoS frame)
    }
    if (len < h + 8 || f[h] != 0xaa || f[h + 1] != 0xaa || f[h + 2] != 0x03)
        return 0;
    return h;
}

// an 802.11 data frame (to the AP) as the Ethernet frame the radio takes; 0 if it cannot be
static ULONG Dot11ToEth(const UINT8 *f, ULONG len, UINT8 *out)
{
    ULONG h = Dot11Header(f, len);
    if (!h || (f[1] & 0x40))
        return 0;                               // not convertible, or already protected
    const UINT8 *da, *sa;
    switch (f[1] & 3) {
    case 1: da = f + 16, sa = f + 10; break;    // to the DS: A1 BSSID, A2 SA, A3 DA
    case 2: da = f + 4, sa = f + 16; break;     // from the DS
    case 3: da = f + 16, sa = f + 24; break;
    default: da = f + 4, sa = f + 10; break;
    }
    RtlCopyMemory(out, da, 6);
    RtlCopyMemory(out + 6, sa, 6);
    out[12] = f[h + 6], out[13] = f[h + 7];     // the ethertype after the SNAP header
    RtlCopyMemory(out + 14, f + h + 8, len - h - 8);
    return 14 + len - h - 8;
}

// an Ethernet frame from the radio as an 802.11 data frame from the AP (FromDS: A1 DA, A2 BSSID, A3 SA)
static ULONG EthToDot11(const UINT8 *e, ULONG len, const UINT8 *bssid, UINT8 *out, ULONG cap)
{
    if (len < 14 || len - 14 + 32 > cap)
        return 0;
    UINT16 t = (UINT16)(e[12] << 8 | e[13]);
    RtlZeroMemory(out, 32);
    out[0] = 0x08;                              // data
    out[1] = 0x02;                              // from the DS
    RtlCopyMemory(out + 4, e, 6);
    RtlCopyMemory(out + 10, bssid, 6);
    RtlCopyMemory(out + 16, e + 6, 6);
    out[24] = 0xaa, out[25] = 0xaa, out[26] = 0x03;
    if (t == 0x80f3 || t == 0x8137)
        out[29] = 0xf8;                         // bridge tunnel encapsulation (802.1H)
    out[30] = e[12], out[31] = e[13];
    RtlCopyMemory(out + 32, e + 14, len - 14);
    return 32 + len - 14;
}

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
    UINT16 scan_sync;               // the running escan's sync id
    ULONG scans;                    // scan tasks so far (each is logged: W2 showed 0 networks in Windows)
    ULONG bss_frame;                // Parameters\BssFrame: 0 the frame body (from the timestamp), 1 with a MAC header
    // connection
    int connecting, connected;
    UINT32 connect_txn;
    // W2d: the join step by step (Windows' handshake failed after association in W2c)
    int ev_budget;                  // firmware events still logged in this connect
    UINT32 last_reason, set_ssid_status;
    volatile LONG eapol_in, eapol_up, eapol_out, eapol_out_fail;  // EAPOL frames: from the AP, to Windows, from Windows
    ULONG tx_dropped_seen, rx_dropped_seen;
    ULONG rx_at_connect;
    UINT64 rx_check_at;
    volatile LONG data_out;
    UINT64 assoc_at;                // when the association was reported (scans are refused for a while after)
    int keyed;                      // Windows' pairwise key reached the firmware
    int join_failed;                // the firmware's SET_SSID failed after the association (reported from the worker)
    UINT64 heartbeat_at;
    ULONG sta_budget;
    // W2i: the data path's callbacks, counted (they may run at DISPATCH, where nothing is logged)
    volatile LONG txq_made, rxq_made, tx_advances, tx_packets, tx_notifies, rx_advances, rx_up;
    UINT64 linkstate_at;
    BSS cur;                        // the associated AP's entry, kept apart from the scan list (W2k)
    int have_cur;
    int scan_cached;                // this scan was refused: the cached list is indicated, marked cached
    ULONG wdi_version;
    ULONG rx_80211;                 // Parameters\RxFormat: 1 (default) 802.11 frames up, 0 Ethernet (W2a..W2k)
    ULONG tx_logged;              // the WDI version declared to WiFiCx; the TLV parser uses the same (W2k)            // when to send WDI's link state again while associated               // sta_info lines still logged in this connect         // data frames from Windows given to the radio since the connect
    ULONG link_state;
    ULONG assoc_txn_mode;           // Parameters\AssocTxn: 0 (default) the association result unsolicited, 1 the task's id               // Parameters\LinkState: 1 (default) the adapter's link reported up/down             // when to log the rx state again (1.5 s after an association)            // the core's received-frame count when the connect began
    UINT16 connect_port;
    UINT8 bssid[6], rsn[BCMW_RSN_OURS];
    UINT64 connect_started;
    int set_ssid_ok;
    // the log of commands seen (each id once)
    UINT8 seen[512];
    int seen_stats;
    ULONG commands;
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

// an EAPOL-Key frame's key info and which of the four-way handshake's messages it is (0: not a key frame)
static int EapolKey(const UINT8 *f, ULONG len, UINT16 *info)
{
    if (len < 14 + 4 + 3 || f[12] != 0x88 || f[13] != 0x8e)
        return -1;                                  // not EAPOL
    if (f[15] != 3)
        return 0;                                   // EAPOL, not a key frame
    UINT16 ki = (UINT16)(f[19] << 8 | f[20]);
    *info = ki;
    if (!(ki & 0x0008))
        return 5;                                   // group key handshake
    if ((ki & 0x0080) && !(ki & 0x0100))
        return 1;
    if ((ki & 0x0080) && (ki & 0x0040))
        return 3;
    if (!(ki & 0x0080) && (ki & 0x0100))
        return (ki & 0x0200) ? 4 : 2;
    return 0;
}

static const char *EvName(UINT32 t)
{
    switch (t) {
    case 0: return "SET_SSID";
    case 1: return "JOIN";
    case 3: return "AUTH";
    case 4: return "AUTH_IND";
    case 5: return "DEAUTH";
    case 6: return "DEAUTH_IND";
    case 7: return "ASSOC";
    case 8: return "ASSOC_IND";
    case 9: return "REASSOC";
    case 11: return "DISASSOC";
    case 12: return "DISASSOC_IND";
    case 16: return "LINK";
    case 17: return "MIC_ERROR";
    case 46: return "PSK_SUP";
    case 52: return "PRUNE";
    default: return "other";
    }
}

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

// a GET answered with a generated message (header space reserved, as for indications)
static void AnswerData(WIFIREQUEST r, UINT8 *gen, ULONG len)
{
    UINT in = 0, out = 0;
    WDI_MESSAGE_HEADER *h = (WDI_MESSAGE_HEADER *)WifiRequestGetInOutBuffer(r, &in, &out);
    if (!h || !gen || len > out || len < sizeof(*h)) {
        if (gen)
            FreeGenerated(gen);
        Answer(r, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    WDI_MESSAGE_HEADER hc = *h;
    RtlCopyMemory(h, gen, len);
    *h = hc;
    h->Status = NDIS_STATUS_SUCCESS;
    WifiRequestComplete(r, STATUS_SUCCESS, len);
    FreeGenerated(gen);
}

// ---- scanning ------------------------------------------------------------------------------------------------------

static const UINT16 SCAN_SYNC = 0x5743;     // "WC": plus the scan's number, so a cancelled scan's late end event
                                            // cannot end the next one (W2b: scans 2 and 3 ended at once, empty)

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
                e[i].EntryAgeInfo.CachedInformation = d->scan_cached ? TRUE : FALSE;
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
    {
        // what Windows can list: one line per network NAME (hidden ones: no name). Counts only, no names.
        ULONG names = 0, hidden = 0, b5 = 0;
        for (ULONG i = 0; i < n; i++) {
            BSS *b = &d->bss[i];
            int blank = 1;
            for (ULONG k = 0; k < b->ssid_len; k++)
                if (b->ssid[k])
                    blank = 0;
            if (b->band5)
                b5++;
            if (blank) {
                hidden++;
                continue;
            }
            ULONG j;
            for (j = 0; j < i; j++)
                if (d->bss[j].ssid_len == b->ssid_len && RtlEqualMemory(d->bss[j].ssid, b->ssid, b->ssid_len))
                    break;
            if (j == i)
                names++;
        }
        Sayf("wificx: scan %lu complete: %lu access points (%lu on 5 GHz) = %lu network names + %lu hidden, status %08x",
             d->scans, n, b5, names, hidden, status);
    }
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
    // not during a join or the 15 s after the association while no key is in (bcmdhd and brcmfmac refuse scans
    // while connecting): the firmware is not asked, the list kept (W2h..W2j wiped it here), and the cached list is
    // indicated as cached with a successful completion
    int busy = d->connecting || (d->connected && !d->keyed && Now(NULL) - d->assoc_at < 15000000);
    d->scanning = 1;                // before the start: its ioctl wait already delivers results
    d->scan_started = Now(NULL);
    d->scan_sync = (UINT16)(SCAN_SYNC + d->scans);
    d->scan_cached = busy;
    if (!busy)
        d->nbss = 0;
    int e = busy ? BCMW_OK : d->up ? bcmw_scan_start(d->w, ssid, ssid_len, d->scan_sync) : BCMW_E_STATE;
    CleanupParsedWdiTaskScanToIhv(&p);
    Answer(r, STATUS_SUCCESS);
    if (busy) {
        Sayf("wificx: scan %lu during a join: the cached list (%lu) indicated instead", d->scans, d->nbss);
        ScanDone(d, NDIS_STATUS_SUCCESS);
        return;
    }
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

// The firmware's own view of the AP (sta_info, brcmf_sta_info_le, natural alignment): whether unicast frames from
// it arrive at all (rx_ucast) or are dropped later; flags: 0x8 authenticated, 0x10 associated, 0x20 authorized,
// 0x100 power save (as the AP sees us). No address is logged.
static void StaInfo(DEVCTX *d, const char *when)
{
    UINT8 b[256];
    int fwerr = 0;
    if (!d->up || !d->sta_budget)
        return;
    d->sta_budget--;
    RtlZeroMemory(b, sizeof(b));
    int e = bcmw_iovar_getbuf(d->w, "sta_info", d->bssid, 6, b, sizeof(b), &fwerr);
    if (e != BCMW_OK) {
        Sayf("wificx: sta_info %s: refused (%d, firmware error %d)", when, e, fwerr);
        return;
    }
#define U32(o) ((ULONG)b[o] | (ULONG)b[(o) + 1] << 8 | (ULONG)b[(o) + 2] << 16 | (ULONG)b[(o) + 3] << 24)
    Sayf("wificx: sta_info %s: version %u, flags %08lx, idle %lu, associated %lu s; tx %lu (failed %lu); rx unicast %lu, "
         "multicast %lu; decrypted %lu, decrypt failures %lu", when, b[0] | b[1] << 8, U32(8), U32(12), U32(44), U32(52),
         U32(56), U32(60), U32(64), U32(76), U32(80));
#undef U32
}

// WDI_GET_STATISTICS (W2h: asked 3 times right after the association, answered NOT_SUPPORTED, then Windows reset
// the port 28 s later without sending a frame): the AP's and the broadcast counts, one PHY
static void CmdStatistics(DEVCTX *d, WIFIREQUEST r)
{
    const struct bcmw_info *in = bcmw_info(d->w);
    WDI_GET_STATISTICS_PARAMETERS p;
    WDI_MAC_STATISTICS_CONTAINER m[2];
    WDI_PHY_STATISTICS_CONTAINER phy;
    UINT8 *gen = NULL;
    ULONG len = 0;
    ULONG n = 0;
    RtlZeroMemory(m, sizeof(m));
    RtlZeroMemory(&phy, sizeof(phy));
    if (d->connected) {
        RtlCopyMemory(m[n].MACAddress.Address, d->bssid, 6);
        m[n].TransmittedFrameCount = in->tx_done;
        m[n].ReceivedFrameCount = in->rx_frames;
        n++;
    }
    RtlFillMemory(m[n].MACAddress.Address, 6, 0xff);
    n++;
    BSS *b = d->connected && d->have_cur ? &d->cur : NULL;
    phy.PhyType = b && b->band5 ? WDI_PHY_TYPE_VHT : WDI_PHY_TYPE_HT;
    phy.TransmittedFrameCount = in->tx_done;
    phy.FailedCount = in->tx_errors;
    phy.ReceivedFrameCount = in->rx_frames;
    p.PeerMACStatistics.SimpleAssign(m, n);
    p.PhyStatistics.SimpleAssign(&phy, 1);
    NDIS_STATUS gs = GenerateWdiGetStatisticsFromIhv(&p, sizeof(WDI_MESSAGE_HEADER), &d->tlv, &len, &gen);
    p.PeerMACStatistics.SimpleAssign(NULL, 0);
    p.PhyStatistics.SimpleAssign(NULL, 0);
    if (gs != NDIS_STATUS_SUCCESS) {
        Sayf("wificx: statistics could not be generated (%08x)", gs);
        Answer(r, STATUS_NOT_SUPPORTED);
        return;
    }
    if (!d->seen_stats) {
        d->seen_stats = 1;
        Sayf("wificx: statistics answered (%lu bytes)", len);
    }
    AnswerData(r, gen, len);
}

// WDI_INDICATION_LINK_STATE_CHANGE: the link's speed and quality, after the association and then every 30 s
// (nothing sent it before W2i)
static void LinkStateWdi(DEVCTX *d)
{
    WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS p;
    WDI_LINK_INFO_CONTAINER li;
    UINT8 *gen = NULL;
    ULONG len = 0;
    BSS *b = d->have_cur ? &d->cur : FindBss(d, d->bssid);
    INT32 rssi = b ? b->rssi : -60;
    UINT8 q = rssi >= -50 ? 100 : rssi <= -100 ? 0 : (UINT8)(2 * (rssi + 100));
    UINT32 kbps = b && b->band5 ? 433300 : 144400;
    RtlCopyMemory(p.LinkStateChangeParameters.PeerMACAddress.Address, d->bssid, 6);
    p.LinkStateChangeParameters.TxLinkSpeed = kbps;
    p.LinkStateChangeParameters.RxLinkSpeed = kbps;
    p.LinkStateChangeParameters.LinkQuality = q;
    RtlZeroMemory(&li, sizeof(li));
    RtlCopyMemory(li.LocalLinkMACAddress.Address, bcmw_info(d->w)->mac, 6);
    RtlCopyMemory(li.PeerLinkMACAddress.Address, d->bssid, 6);
    li.ChannelNumber = b ? b->channel : 0;
    li.BandId = b && b->band5 ? WDI_BAND_ID_5000 : WDI_BAND_ID_2400;
    li.RSSI = rssi;
    li.Bandwidth = b && b->band5 ? 80 : 20;
    p.LinkInfo.SimpleAssign(&li, 1);
    NDIS_STATUS gs = GenerateWdiIndicationLinkStateChangeFromIhv(&p, sizeof(WDI_MESSAGE_HEADER), &d->tlv, &len, &gen);
    p.LinkInfo.SimpleAssign(NULL, 0);
    if (gs == NDIS_STATUS_SUCCESS)
        Indicate(d, WDI_INDICATION_LINK_STATE_CHANGE, d->connect_port, WDI_TRANSACTION_ID_UNSOLICIT, NDIS_STATUS_SUCCESS,
                 gen, len);
    Sayf("wificx: WDI link state: %lu kbps, quality %u, %s", kbps, q, gs == NDIS_STATUS_SUCCESS ? "indicated" : "NOT generated");
    d->linkstate_at = Now(NULL) + 30000000;
}

// The adapter's media state for NDIS (W2f: on an open network Windows sent no frame at all in 22 s, no DHCP;
// nothing ever reported the link up). Parameters\LinkState 0 leaves it to WiFiCx.
static void LinkState(DEVCTX *d, int up)
{
    NET_ADAPTER_LINK_STATE ls;
    if (!d->link_state || !d->adapter)
        return;
    if (up)
        NET_ADAPTER_LINK_STATE_INIT(&ls, 150000000ull, MediaConnectStateConnected, MediaDuplexStateHalf,
                                    NetAdapterPauseFunctionTypeUnsupported, NetAdapterAutoNegotiationFlagNone);
    else
        NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&ls);
    NetAdapterSetLinkState(d->adapter, &ls);
    Sayf("wificx: the adapter's link reported %s", up ? "UP" : "down");
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
    if (b) {
        d->cur = *b;
        d->have_cur = 1;
    } else if (d->have_cur && RtlEqualMemory(d->cur.bssid, d->bssid, 6)) {
        b = &d->cur;                // a repeated connect after the list changed
    } else {
        d->have_cur = 0;
    }
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
    if (b) {
        // WMM negotiated when the AP advertises it (vendor element 00:50:F2 type 2): the firmware uses it then
        for (UINT32 o = 0; o + 2 <= b->ie_len && o + 2u + b->ie[o + 1] <= b->ie_len; o += 2u + b->ie[o + 1])
            if (b->ie[o] == 221 && b->ie[o + 1] >= 4 && b->ie[o + 2] == 0x00 && b->ie[o + 3] == 0x50 &&
                b->ie[o + 4] == 0xf2 && b->ie[o + 5] == 2)
                res.AssociationResultParameters.WMMQoSEnabled = TRUE;
    }
    WDI_PHY_TYPE phy = b && b->band5 ? WDI_PHY_TYPE_VHT : WDI_PHY_TYPE_HT;    // required since WDI 1.1.4
    res.ActivePhyTypeList.SimpleAssign(&phy, 1);
    if (ok && have_ies) {
        // the frames' bodies: request = capability, listen interval, elements; response = capability, status, AID, elements
        RtlZeroMemory(reqf, 4);
        reqf[0] = d->rsn[0] ? 0x31 : 0x21, reqf[1] = 0x04;     // ESS, (privacy,) short slot time
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
        // WDI lists the association result among the connect task's UNSOLICITED indications (transaction id 0),
        // like the BSS list. W2a..W2i sent the task's id: AutoConfig logged every attempt as 8002 "the specific
        // network is not available" (0x2800B, RSSI 255) the moment it came, never an association.
        // Parameters\AssocTxn 1: the task's id again.
        Indicate(d, WDI_INDICATION_ASSOCIATION_RESULT, d->connect_port,
                 d->assoc_txn_mode ? d->connect_txn : WDI_TRANSACTION_ID_UNSOLICIT, NDIS_STATUS_SUCCESS, gen, len);
    else
        Say("wificx: the association result could not be generated");
    {
        const UINT8 *our = d->rsn[0] ? d->rsn : NULL;
        const UINT8 *sent = have_ies ? bcmw_ie_find(req, req_len, 48) : NULL;
        Sayf("wificx: association result: %s, status code %lu; request elements %lu bytes (RSN %s), response %lu, "
             "AP elements %lu (%s); set_ssid status %lu, last deauth reason %lu", ok ? "SUCCESS" : "FAILURE",
             ok ? 0 : reason, have_ies ? req_len : 0, !our ? "none asked" : !sent ? "MISSING" :
             (sent[1] == our[1] && RtlEqualMemory(sent, our, 2u + our[1])) ? "as set" : "DIFFERENT from what was set",
             have_ies ? resp_len : 0, b ? b->ie_len : 0, b ? "from the scan" : "AP NOT IN THE SCAN LIST",
             d->set_ssid_status, d->last_reason);
    }
    list.AssociationResults.SimpleAssign(NULL, 0);
    res.ActivePhyTypeList.SimpleAssign(NULL, 0);
    if (ok) {
        LinkState(d, 1);
        LinkStateWdi(d);            // before the task's completion, as Microsoft's sample does
        d->assoc_at = Now(NULL);
        d->keyed = 0;
        d->join_failed = 0;
        d->sta_budget = 6;
    }
    Indicate(d, WDI_INDICATION_CONNECT_COMPLETE, d->connect_port, d->connect_txn,
             ok ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE, NULL, 0);
    Sayf("wificx: connect %s (association result %s, then the task's completion)", ok ?
         "COMPLETE (associated; Windows does the handshake now)" : "FAILED", d->assoc_txn_mode ? "with the task's id" :
         "unsolicited");
    if (ok)
        d->heartbeat_at = 0;        // the 10 s heartbeat starts now
    bcmw_rx_state(d->w, "at the association result");
    d->rx_check_at = ok ? Now(NULL) + 1500000 : 0;
    d->connecting = 0;
    {   // the frames held during the report (the AP's first handshake message) go up now
        KIRQL old;
        KeAcquireSpinLock(&d->rx_lock, &old);
        if (d->rx_count && d->rxq && InterlockedExchange(&d->rx_armed, 0))
            NetRxQueueNotifyMoreReceivedPacketsAvailable(d->rxq);
        KeReleaseSpinLock(&d->rx_lock, old);
    }
}

static void CmdConnect(DEVCTX *d, WIFIREQUEST r, WDI_MESSAGE_HEADER *h, UINT in)
{
    WDI_TASK_CONNECT_PARAMETERS p;
    int e = BCMW_E_REFUSED;
    d->connect_txn = h->TransactionId;
    d->connect_port = h->PortId;
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
    {
        // what Windows asked, as numbers (WDI_AUTH_ALGORITHM / WDI_CIPHER_ALGORITHM; W2d: joins logged as "open")
        char t[160];
        size_t o = 0;
        t[0] = 0;
        RtlStringCbPrintfA(t + o, sizeof(t) - o, "auth");
        o = strlen(t);
        for (UINT32 i = 0; i < c->AuthenticationAlgorithms.ElementCount && i < 6 && o < sizeof(t) - 12; i++, o = strlen(t))
            RtlStringCbPrintfA(t + o, sizeof(t) - o, " %lu", (ULONG)c->AuthenticationAlgorithms.pElements[i]);
        RtlStringCbPrintfA(t + o, sizeof(t) - o, "; unicast");
        o = strlen(t);
        for (UINT32 i = 0; i < c->UnicastCipherAlgorithms.ElementCount && i < 6 && o < sizeof(t) - 12; i++, o = strlen(t))
            RtlStringCbPrintfA(t + o, sizeof(t) - o, " %lu", (ULONG)c->UnicastCipherAlgorithms.pElements[i]);
        RtlStringCbPrintfA(t + o, sizeof(t) - o, "; multicast");
        o = strlen(t);
        for (UINT32 i = 0; i < c->MulticastCipherAlgorithms.ElementCount && i < 6 && o < sizeof(t) - 12; i++, o = strlen(t))
            RtlStringCbPrintfA(t + o, sizeof(t) - o, " %lu", (ULONG)c->MulticastCipherAlgorithms.pElements[i]);
        Sayf("wificx: connect asked: %s (open %u, RSNA-PSK %u)", t, WDI_AUTH_ALGO_80211_OPEN, WDI_AUTH_ALGO_RSNA_PSK);
    }
    if (d->connected == 1 && p.PreferredBSSEntryList.ElementCount >= 1 &&
        RtlEqualMemory(p.PreferredBSSEntryList.pElements[0].BSSID.Address, d->bssid, 6)) {
        // W2f: Windows asked again right after the association; a new SET_SSID aborted the one in progress
        // (status 4) and lost the AP's first handshake message. The association stands: reported again.
        Say("wificx: connect asked while associated with that access point: the association is reported again");
        CleanupParsedWdiTaskConnectToIhv(&p);
        Answer(r, STATUS_SUCCESS);
        d->connecting = 1;
        d->set_ssid_ok = 2;
        ConnectDone(d, 1, 0);
        return;
    }
    RtlZeroMemory(d->rsn, sizeof(d->rsn));     // after the repeat check above: a repeat keeps the link's RSN
    if (c->SSIDList.ElementCount >= 1 && p.PreferredBSSEntryList.ElementCount >= 1 && (psk || open) && d->up) {
        const WDI_SSID *s = &c->SSIDList.pElements[0];
        RtlCopyMemory(d->bssid, p.PreferredBSSEntryList.pElements[0].BSSID.Address, 6);
        BSS *b = FindBss(d, d->bssid);
        if (b && !psk && bcmw_ie_find(b->ie, b->ie_len, 48))
            Say("wificx: connect: Windows asks an OPEN join, but this access point advertises RSN (WPA2/3)");
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
        Sayf("wificx: connect: %s, %u BSS candidates (names not logged); the first: %s, channel %lu %s GHz, %ld dBm, "
             "group cipher %s, RSN element %u bytes", psk ? "WPA2-PSK (Windows' supplicant)" : "open",
             p.PreferredBSSEntryList.ElementCount, b ? "in the scan list" : "NOT in the scan list", b ? b->channel : 0,
             b && b->band5 ? "5" : "2.4", b ? (long)b->rssi : 0L, tkip ? "TKIP" : "CCMP", psk ? 2u + d->rsn[1] : 0u);
        d->ev_budget = 40;
        d->rx_at_connect = bcmw_info(d->w)->rx_frames;
        d->data_out = 0;
        {
            UINT8 fwmac[6] = { 0 };
            int me = bcmw_iovar_get(d->w, "cur_etheraddr", fwmac, 6, NULL);
            Sayf("wificx: the radio's address (cur_etheraddr) %s the adapter's", me != BCMW_OK ? "UNREADABLE, compared with" :
                 RtlEqualMemory(fwmac, bcmw_info(d->w)->mac, 6) ? "is" : "DIFFERS FROM");
        }
        d->last_reason = 0;
        d->set_ssid_status = 0;
        d->eapol_in = d->eapol_up = d->eapol_out = d->eapol_out_fail = 0;
        bcmw_console_more(d->w, 40);    // the firmware's own account of the join
        bcmw_rx_log(d->w, 24);          // W2f: what (if anything) arrives after the association
        bcmw_rx_state(d->w, "at the connect");
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
    LinkState(d, 0);
    StaInfo(d, "at the disassociation");
    bcmw_rx_state(d->w, "at the disassociation");
    d->rx_check_at = 0;
    Sayf("wificx: disassociated (%s, 802.11 reason %lu); EAPOL so far: %ld in from the AP, %ld handed to Windows, "
         "%ld out from Windows (%ld not sent); frames in from the radio since the connect: %lu, data frames out "
         "from Windows: %ld", by_host ? "by the host" : "by the access point", d->last_reason, d->eapol_in, d->eapol_up,
         d->eapol_out, d->eapol_out_fail, (ULONG)(bcmw_info(d->w)->rx_frames - d->rx_at_connect), d->data_out);
    UNREFERENCED_PARAMETER(reason);
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

static void SendQueued(DEVCTX *d);

static void CmdAddKeys(DEVCTX *d, WIFIREQUEST r, WDI_MESSAGE_HEADER *h, UINT in)
{
    {
        // Windows sends message 4, then the keys; commands run before queued frames, so message 4 is sent now and
        // the radio given up to 500 ms to put it on the air before the key goes in (brcmfmac waits on pend_8021x),
        // or it would leave encrypted and the AP would drop it
        SendQueued(d);
        const struct bcmw_info *bi = bcmw_info(d->w);
        UINT64 t0 = Now(NULL);
        while (d->up && bi->tx_done < bi->tx_frames && Now(NULL) - t0 < 500000) {
            bcmw_poll(d->w);
            Delay(NULL, 2000);
        }
        if (bi->tx_done < bi->tx_frames)
            Sayf("wificx: keys: %lu frames still in the radio after 500 ms", bi->tx_frames - bi->tx_done);
    }
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
        if (ea && e == BCMW_OK)
            d->keyed = 1;
        if (e != BCMW_OK)
            st = STATUS_UNSUCCESSFUL;
    }
    CleanupParsedWdiSetAddCipherKeysToIhv(&p);
    Answer(r, st);
}

// ---- the commands ------------------------------------------------------------------------------------------------------

// A MAC address Windows configures (SET_ADAPTER_CONFIGURATION, DOT11_RESET): the radio takes it when it differs, so
// the handshake's keys (computed by Windows from its address) match what the radio sends. No address is logged.
static void ConfiguredMac(DEVCTX *d, const char *from, const UINT8 *mac)
{
    const UINT8 *own = bcmw_info(d->w)->mac;
    if (RtlEqualMemory(mac, own, 6)) {
        Sayf("wificx: %s: Windows' MAC address is the radio's", from);
        return;
    }
    int e = d->up && !d->connected ? bcmw_set_mac(d->w, mac) : BCMW_E_STATE;
    Sayf("wificx: %s: Windows configures a DIFFERENT MAC address (%s); given to the radio: %d", from,
         (mac[0] & 2) ? "locally administered: randomized" : "a global one", e);
    if (e == BCMW_OK) {
        NET_ADAPTER_LINK_LAYER_ADDRESS a;
        NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&a, 6, mac);
        NetAdapterSetCurrentLinkLayerAddress(d->adapter, &a);
    }
}

static void CmdAdapterConfig(DEVCTX *d, WIFIREQUEST r, WDI_MESSAGE_HEADER *h, UINT in)
{
    WDI_SET_FIRMWARE_CONFIGURATION_PARAMETERS p;
    if (ParseWdiSetAdapterConfigurationToIhv(in - sizeof(*h), (UINT8 *)(h + 1), &d->tlv, &p) == NDIS_STATUS_SUCCESS) {
        if (p.Optional.MacAddress_IsPresent)
            ConfiguredMac(d, "adapter configuration", p.MacAddress.Address);
        CleanupParsedWdiSetAdapterConfigurationToIhv(&p);
    }
    Answer(r, STATUS_SUCCESS);
}

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
    d->commands++;
    if (id < sizeof(d->seen) && !d->seen[id]) {
        d->seen[id] = 1;
        Sayf("wificx: command %u (first time), %u bytes in, %u out", id, in, out);
    } else if ((d->connecting || d->connected) && id != WDI_TASK_SCAN && id != WDI_ABORT_TASK) {
        Sayf("wificx: command %u, %u bytes in", id, in);     // during a join, every command (W2g: what Windows did)
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
    case WDI_TASK_DOT11_RESET: {
        WDI_TASK_DOT11_RESET_PARAMETERS rp;
        if (ParseWdiTaskDot11ResetToIhv(in - sizeof(*h), (UINT8 *)(h + 1), &d->tlv, &rp) == NDIS_STATUS_SUCCESS) {
            UINT8 mac[6];
            int has = rp.Optional.ResetMACAddress_IsPresent;
            if (has)
                RtlCopyMemory(mac, rp.ResetMACAddress.Address, 6);
            CleanupParsedWdiTaskDot11ResetToIhv(&rp);
            if (has && !d->connected)
                ConfiguredMac(d, "reset", mac);
        }
        Answer(r, STATUS_SUCCESS);
        Forget(d);
        if (d->connected && d->up)
            bcmw_leave(d->w);
        Disconnected(d, 3, 1);
        Indicate(d, WDI_INDICATION_DOT11_RESET_COMPLETE, hc.PortId, hc.TransactionId, NDIS_STATUS_SUCCESS, NULL, 0);
        break;
    }
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
    case WDI_GET_STATISTICS:
        CmdStatistics(d, r);
        break;
    case WDI_SET_ADAPTER_CONFIGURATION:
        CmdAdapterConfig(d, r, h, in);
        break;
    case WDI_SET_DELETE_CIPHER_KEYS:
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
        if (bcmw_bss_parse(ev, &b, &sync) && sync == d->scan_sync && d->scanning) {
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
        } else if (bcmw_scan_ended(ev, d->scan_sync) && d->scanning) {
            d->scanning = 2;            // reported from the worker loop
        }
        return;
    }
    if ((d->connecting || d->connected) && d->ev_budget > 0) {
        d->ev_budget--;
        Sayf("wificx: fw event %lu %s: status %lu, reason %lu, flags %04x, auth %lu", ev->type, EvName(ev->type),
             ev->status, ev->reason, ev->flags, ev->auth_type);
    }
    switch (ev->type) {
    case BCMW_E_SET_SSID:
        d->set_ssid_status = ev->status;
        if (d->connecting && d->set_ssid_ok != 2)   // the LINK event may come first: keep "associated"
            d->set_ssid_ok = ev->status == 0 ? 1 : -1;
        else if (d->connected == 1 && ev->status != 0)
            d->join_failed = 1;                     // reported from the worker loop
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
        d->last_reason = ev->reason;
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
    {
        UINT16 ki = 0;
        int m = EapolKey(frame, len, &ki);
        if (m >= 0) {
            InterlockedIncrement(&d->eapol_in);
            Sayf("wificx: EAPOL in from the AP: %s%u, key info %04x, %lu bytes; %s", m == 5 ? "group " : "message ",
                 m == 5 ? 1 : m, ki, len, !d->rxq ? "NO RX QUEUE (dropped later)" :
                 d->connecting ? "held until Windows has the association" : "to Windows");
        }
    }
    KeAcquireSpinLock(&d->rx_lock, &old);
    if (d->rx_count < QLEN) {
        FRAME *f = &d->rx[(d->rx_head + d->rx_count) % QLEN];
        f->len = len;
        RtlCopyMemory(f->data, frame, len);
        d->rx_count++;
    } else {
        d->rx_dropped++;
    }
    // under the lock: the queue cannot go away meanwhile. Not while a connect is being reported: the AP's first
    // handshake message comes within milliseconds of the association, before Windows has the association result
    if (d->rxq && !d->connecting && InterlockedExchange(&d->rx_armed, 0))
        NetRxQueueNotifyMoreReceivedPacketsAvailable(d->rxq);
    KeReleaseSpinLock(&d->rx_lock, old);
}

// ---- the worker ------------------------------------------------------------------------------------------------------------

// the frames Windows queued, to the radio (from the worker; also before a key goes in, so the handshake's last
// message leaves unencrypted)
static void SendQueued(DEVCTX *d)
{
    static UINT8 frame[FRAME_MAX], raw[FRAME_MAX];
    KIRQL old;
    for (;;) {
        ULONG len = 0;
        UINT8 l2 = 0;
        KeAcquireSpinLock(&d->tx_lock, &old);
        if (d->tx_count) {
            FRAME *f = &d->tx[d->tx_head];
            len = f->len;
            l2 = f->l2;
            RtlCopyMemory(raw, f->data, len);
            d->tx_head = (d->tx_head + 1) % QLEN;
            d->tx_count--;
        }
        KeReleaseSpinLock(&d->tx_lock, old);
        if (!len)
            break;
        {
            ULONG h = Dot11Header(raw, len);
            int dot11 = l2 == NetPacketLayer2TypeIeee80211 || h;
            ULONG elen = dot11 ? Dot11ToEth(raw, len, frame) : 0;
            if (d->tx_logged < 5) {
                // no addresses: the frame control, the header length, the LLC/SNAP and the ethertype
                d->tx_logged++;
                Sayf("wificx: tx frame %lu: layout type %u, %lu bytes, frame control %02x%02x, %s, header %lu, "
                     "LLC %02x %02x %02x %02x %02x %02x, ethertype %02x%02x -> %s %lu bytes", d->tx_logged, l2, len,
                     raw[0], raw[1], dot11 ? "802.11" : "Ethernet", h, h ? raw[h] : 0, h ? raw[h + 1] : 0,
                     h ? raw[h + 2] : 0, h ? raw[h + 3] : 0, h ? raw[h + 4] : 0, h ? raw[h + 5] : 0,
                     h ? raw[h + 6] : raw[12], h ? raw[h + 7] : raw[13], dot11 ? (elen ? "Ethernet" : "NOT CONVERTED")
                     : "as is", dot11 ? elen : len);
            }
            if (dot11) {
                if (!elen) {
                    d->tx_dropped++;
                    continue;
                }
                len = elen;
            } else {
                RtlCopyMemory(frame, raw, len);
            }
        }
        UINT16 ki = 0;
        int m = EapolKey(frame, len, &ki);
        int te = d->up ? bcmw_tx(d->w, frame, len) : BCMW_E_STATE;
        if (InterlockedIncrement(&d->data_out) == 1)
            Sayf("wificx: the first data frame from Windows: ethertype %02x%02x, %lu bytes, sent: %d", frame[12],
                 frame[13], len, te);
        if (te != BCMW_OK)
            d->tx_dropped++;
        if (m >= 0) {
            InterlockedIncrement(&d->eapol_out);
            if (te != BCMW_OK)
                InterlockedIncrement(&d->eapol_out_fail);
            Sayf("wificx: EAPOL out from Windows: %s%u, key info %04x, %lu bytes, source %s; sent: %d", m == 5 ? "group " : "message ",
                 m == 5 ? 2 : m, ki, len, RtlEqualMemory(frame + 6, bcmw_info(d->w)->mac, 6) ? "the radio's address" :
                 "NOT the radio's address", te);
        }
    }
}

static void SetParam(PCWSTR name, ULONG v);
static KSTART_ROUTINE Worker;
static void Worker(PVOID arg)
{
    DEVCTX *d = (DEVCTX *)arg;
    LARGE_INTEGER t;
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
        SendQueued(d);
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
        if ((d->connected || d->connecting) && (d->tx_dropped != d->tx_dropped_seen || d->rx_dropped != d->rx_dropped_seen)) {
            Sayf("wificx: frames dropped so far: %lu out (no link or queue full), %lu in (queue full)", d->tx_dropped,
                 d->rx_dropped);
            d->tx_dropped_seen = d->tx_dropped, d->rx_dropped_seen = d->rx_dropped;
        }
        if (d->rx_check_at && Now(NULL) > d->rx_check_at) {
            d->rx_check_at = 0;
            bcmw_rx_state(d->w, "1.5 s after the association");
            StaInfo(d, "1.5 s after the association");
        }
        if (d->join_failed && d->connected == 1) {
            // the firmware gave up its join after the association (W2g: SET_SSID status 2 two seconds later,
            // then nothing): Windows hears of a lost link instead of waiting on a dead one
            d->join_failed = 0;
            StaInfo(d, "when the firmware's join failed");
            Sayf("wificx: the firmware's join FAILED after the association (SET_SSID status %lu): reported as a lost link",
                 d->set_ssid_status);
            if (d->up)
                bcmw_leave(d->w);
            Disconnected(d, d->set_ssid_status, 0);
        }
        if (d->connected == 1 && d->linkstate_at && Now(NULL) > d->linkstate_at)
            LinkStateWdi(d);
        if (Now(NULL) > d->heartbeat_at) {
            const struct bcmw_info *in = bcmw_info(d->w);
            d->heartbeat_at = Now(NULL) + (d->connected || d->connecting ? 10000000 : 60000000);
            Sayf("wificx: alive: %s; polls %lu, events %lu, frames in %lu, out %lu (done %lu); commands %lu",
                 d->connected ? (d->keyed ? "associated, keyed" : "associated, no key yet") :
                 d->connecting ? "connecting" : "idle", in->polls, in->events, in->rx_frames, in->tx_frames, in->tx_done,
                 d->commands);
            Sayf("wificx: data path: tx queue made %ld, rx queue made %ld (now %s/%s); tx advances %ld, packets %ld, "
                 "notifications %ld; rx advances %ld, frames handed to Windows %ld", d->txq_made, d->rxq_made,
                 d->txq ? "tx" : "NO TX", d->rxq ? "rx" : "NO RX", d->tx_advances, d->tx_packets, d->tx_notifies,
                 d->rx_advances, d->rx_up);
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
    InterlockedIncrement(&d->tx_advances);
    while (i != pr->EndIndex) {
        NET_PACKET *p = NetRingGetPacketAtIndex(pr, i);
        if (!p->Ignore) {
            InterlockedIncrement(&d->tx_packets);
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
                    out->l2 = (UINT8)p->Layout.Layer2Type;
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
static void TxNotify(NETPACKETQUEUE q, BOOLEAN)
{
    InterlockedIncrement(&QueueCtx(q)->d->tx_notifies);
}

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
    InterlockedIncrement(&d->rx_advances);
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
        UINT32 n;
        if (d->rx_80211) {
            n = EthToDot11(f->data, f->len, d->bssid, (UINT8 *)va->VirtualAddress, (ULONG)frag->Capacity);
            if (!n) {                               // does not fit: dropped
                KeAcquireSpinLock(&d->rx_lock, &old);
                d->rx_head = (d->rx_head + 1) % QLEN;
                d->rx_count--;
                d->rx_dropped++;
                KeReleaseSpinLock(&d->rx_lock, old);
                continue;
            }
        } else {
            n = f->len <= frag->Capacity ? f->len : (UINT32)frag->Capacity;
            RtlCopyMemory(va->VirtualAddress, f->data, n);
        }
        InterlockedIncrement(&d->rx_up);
        if (n >= 14 && f->data[12] == 0x88 && f->data[13] == 0x8e)
            InterlockedIncrement(&d->eapol_up);
        frag->ValidLength = n;
        frag->Offset = 0;
        NET_PACKET *p = NetRingGetPacketAtIndex(pr, pi);
        p->FragmentIndex = fi;
        p->FragmentCount = 1;
        p->Layout = {};
        p->Layout.Layer2Type = d->rx_80211 ? NetPacketLayer2TypeIeee80211 : NetPacketLayer2TypeEthernet;
        p->Layout.Layer2HeaderLength = d->rx_80211 ? 32 : 14;     // to the network header (802.11 + LLC/SNAP)
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
        if (n && !d->connecting && InterlockedExchange(&d->rx_armed, 0))
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
    InterlockedIncrement(&c->d->txq_made);
    Say("wificx: data path: the tx queue was created");
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
    InterlockedIncrement(&c->d->rxq_made);
    Say("wificx: data path: the rx queue was created");
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
    d->link_state = Param(L"LinkState", 0);    // W2k: left to WiFiCx (Microsoft's sample does not set it)
    d->assoc_txn_mode = Param(L"AssocTxn", 0);
    d->rx_80211 = Param(L"RxFormat", 1);
    Sayf("wificx: settings: association result %s, link state %s, BSS frames %s, received frames up as %s",
         d->assoc_txn_mode ? "with the task's id" : "unsolicited", d->link_state ? "reported" : "left to WiFiCx",
         d->bss_frame == 1 ? "with MAC header" : "body", d->rx_80211 ? "802.11" : "Ethernet");
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
    {
        // W2k: the TLV parser/generator speaks the version declared to WiFiCx (it converts to and from the OS's);
        // W2a..W2j used the OS's (2.0.3) against a declared 2.0.0
        ULONG os = WifiDeviceGetOsWdiVersion(device);
#ifndef SB_TLV_OS
#define SB_TLV_OS 1     // the OS's version (W2j-a associated with it); 0 = the declared one (W2k)
#endif
        // Parameters\WdiTlv 1: the OS's version, as W2a..W2j (W2j-a associated with that)
        d->tlv.PeerVersion = !Param(L"WdiTlv", SB_TLV_OS) && d->wdi_version && d->wdi_version < os ? d->wdi_version : os;
        Sayf("wificx: WDI: declared %lx, the OS %lx, the TLVs speak %lx", d->wdi_version, os, d->tlv.PeerVersion);
    }
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
#ifndef SB_WDI_DEFAULT
#define SB_WDI_DEFAULT WDI_VERSION_2_0_0
#endif
    ULONG wdi = Param(L"WdiVersion", SB_WDI_DEFAULT);
    Ctx(device)->wdi_version = wdi;
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
