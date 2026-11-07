/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd USB transport on Windows: the phone as a USB device through the inbox GenericUsbFn driver
 * (user mode: GenericUsbFnIoctl.h, structures from usbfnbase.h), PROTOCOL.md section 6.
 *
 *   find the device interface {d274dae6-37b0-4c76-bf10-3a9681346dd1} (CM_Get_Device_Interface_List),
 *   open it overlapped, IOCTL_GENERICUSBFN_ACTIVATE_USB_BUS, keep one BUS_EVENT_NOTIFICATION pending;
 *   on Configured: GET_CLASS_INFO -> pipe ids of bulk IN 0x81 / OUT 0x01, two TRANSFER_OUT reads queued;
 *   frames go out as TRANSFER_IN, ending with a zero-length packet (TRANSFER_IN_APPEND_ZERO_PKT) when a
 *   transfer is a multiple of 512 bytes; Reset/Detach/Unconfigured end the session; Suspend/Resume logged.
 * The Mac is the USB host and starts every session with HELLO in a transfer of its own; after a protocol
 * error OUT data is ignored until a transfer starts with HELLO (session.c, link->hunting).
 * Not run on the phone yet: the USB device mode itself is still being brought up.
 */
#include "sender.h"

#include <winioctl.h>
#include <cfgmgr32.h>
#include <usbspec.h>
#include <usbfnbase.h>
#include <GenericUsbFnIoctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID SBL_USBFN_IF = { 0xd274dae6, 0x37b0, 0x4c76, { 0xbf, 0x10, 0x3a, 0x96, 0x81, 0x34, 0x6d, 0xd1 } };

#define RX_SIZE  (64u * 1024u)     /* multiple of 512 */
#define TX_TIMEOUT_MS 5000

typedef struct usb_ctx {
    sbl_link link;                 /* first: sbl_link * == usb_ctx * */
    sbl_sender *s;
    HANDLE h;
    USBFNPIPEID pin, pout;
    volatile LONG configured;
    volatile LONG suspended;
    OVERLAPPED txov;
    int chunk;
} usb_ctx;

static const char *ev_name(USBFN_EVENT e)
{
    switch (e) {
    case UsbfnEventAttach: return "Attach";
    case UsbfnEventReset: return "Reset";
    case UsbfnEventDetach: return "Detach";
    case UsbfnEventSuspend: return "Suspend";
    case UsbfnEventResume: return "Resume";
    case UsbfnEventSetupPacket: return "SetupPacket";
    case UsbfnEventConfigured: return "Configured";
    case UsbfnEventUnConfigured: return "UnConfigured";
    case UsbfnEventPortType: return "PortType";
    case UsbfnEventBusTearDown: return "BusTearDown";
    case UsbfnEventSetInterface: return "SetInterface";
    default: return "?";
    }
}

/* an IOCTL on the overlapped handle, waited for; returns the Win32 error (0 = ok) */
static DWORD ioctl_wait(HANDLE h, DWORD code, void *in, DWORD inl, void *out, DWORD outl, DWORD *ret, DWORD ms)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return GetLastError();
    DWORD n = 0, err = 0;
    if (!DeviceIoControl(h, code, in, inl, out, outl, NULL, &ov)) {
        err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            err = 0;
            if (WaitForSingleObject(ov.hEvent, ms) == WAIT_TIMEOUT) {
                CancelIoEx(h, &ov);
                err = WAIT_TIMEOUT;
            }
        }
    }
    if (!GetOverlappedResult(h, &ov, &n, TRUE) && !err) err = GetLastError();
    CloseHandle(ov.hEvent);
    if (ret) *ret = n;
    return err;
}

/* ---- sending (txlk held by session.c) ---------------------------------------------------------------- */

static int usb_send(sbl_link *l, const uint8_t *p, size_t n)
{
    usb_ctx *u = (usb_ctx *)l;
    if (!u->configured) return -1;
    while (n) {
        DWORD len = (DWORD)(n > (size_t)u->chunk ? (size_t)u->chunk : n);
        DWORD code = (len % SBLK_USB_MPS) == 0 ? IOCTL_GENERICUSBFN_TRANSFER_IN_APPEND_ZERO_PKT
                                                : IOCTL_GENERICUSBFN_TRANSFER_IN;
        ResetEvent(u->txov.hEvent);
        DWORD done = 0, err = 0;
        if (!DeviceIoControl(u->h, code, &u->pin, sizeof u->pin, (void *)p, len, NULL, &u->txov)) {
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                err = 0;
                if (WaitForSingleObject(u->txov.hEvent, TX_TIMEOUT_MS) == WAIT_TIMEOUT) {
                    CancelIoEx(u->h, &u->txov);
                    err = WAIT_TIMEOUT;   /* the Mac is not reading */
                }
            }
        }
        if (!GetOverlappedResult(u->h, &u->txov, &done, TRUE) && !err) err = GetLastError();
        if (err || done == 0 || done > len) {
            sbl_log("usb: IN transfer of %lu bytes: error %lu, %lu sent", len, err, done);
            return -1;
        }
        if (done != len) {
            /* GenericUsbFn/UFX may complete a large IN transfer early (seen on the S9: 258048 of 262144, no
             * error). The SBLINK stream has no transfer boundaries, so just send the rest. */
            static unsigned partial_logs;
            if (partial_logs < 8) { partial_logs++; sbl_log("usb: IN transfer of %lu bytes completed %lu: sending the rest", len, done); }
        }
        p += done;
        n -= done;
    }
    return 0;
}

static void usb_close(sbl_link *l) { (void)l; }

/* ---- device ---------------------------------------------------------------------------------------- */

static int find_interface(WCHAR *path, size_t cap)
{
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID)&SBL_USBFN_IF, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len <= 1)
        return -1;
    WCHAR *list = calloc(len, sizeof(WCHAR));
    if (!list) return -1;
    int r = -1;
    if (CM_Get_Device_Interface_ListW((LPGUID)&SBL_USBFN_IF, NULL, list, len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS &&
        list[0] && wcslen(list) < cap) {
        wcscpy(path, list);
        r = 0;
    }
    free(list);
    return r;
}

static int get_pipes(usb_ctx *u)
{
    USBFN_CLASS_INFORMATION_PACKET ci;
    memset(&ci, 0, sizeof ci);
    DWORD n = 0, err = ioctl_wait(u->h, IOCTL_GENERICUSBFN_GET_CLASS_INFO, NULL, 0, &ci, sizeof ci, &n, 2000);
    if (err) { sbl_log("usb: GET_CLASS_INFO error %lu", err); return -1; }
    const USBFN_CLASS_INTERFACE *ifs[3] = { &ci.HighSpeedClassInterface, &ci.FullSpeedClassInterface,
                                            &ci.SuperSpeedClassInterface };
    int have_in = 0, have_out = 0;
    for (int k = 0; k < 3 && !(have_in && have_out); k++) {
        const USBFN_CLASS_INTERFACE *ci1 = ifs[k];
        for (int i = 0; i < ci1->PipeCount && i < MAX_NUM_USBFN_PIPES; i++) {
            const USB_ENDPOINT_DESCRIPTOR *ep = &ci1->PipeArr[i].EpDesc;
            sbl_log("usb: interface %u pipe %lu: endpoint 0x%02x attr %u max packet %u", ci1->InterfaceNumber,
                    (unsigned long)ci1->PipeArr[i].PipeId, ep->bEndpointAddress, ep->bmAttributes, ep->wMaxPacketSize);
            if ((ep->bmAttributes & 3) != USB_ENDPOINT_TYPE_BULK) continue;
            if ((ep->bEndpointAddress & 0x80) && !have_in) { u->pin = ci1->PipeArr[i].PipeId; have_in = 1; }
            if (!(ep->bEndpointAddress & 0x80) && !have_out) { u->pout = ci1->PipeArr[i].PipeId; have_out = 1; }
        }
    }
    if (!have_in || !have_out) { sbl_log("usb: no bulk IN + OUT pipe pair"); return -1; }
    return 0;
}

/* runs one opened device until it goes away; returns to re-discover */
static void serve(usb_ctx *u)
{
    sbl_sender *s = u->s;
    DWORD err = ioctl_wait(u->h, IOCTL_GENERICUSBFN_ACTIVATE_USB_BUS, NULL, 0, NULL, 0, NULL, 5000);
    if (err) { sbl_log("usb: ACTIVATE_USB_BUS error %lu", err); return; }
    sbl_log("usb: bus activated, waiting for the Mac");

    USBFN_NOTIFICATION note;
    OVERLAPPED evov, rxov[2];
    uint8_t *rxbuf[2] = { malloc(RX_SIZE), malloc(RX_SIZE) };
    int rx_posted[2] = { 0, 0 }, rx_next = 0, prev_full = 0, ev_posted = 0;
    DWORD n = 0;
    memset(&evov, 0, sizeof evov);
    memset(rxov, 0, sizeof rxov);
    evov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    rxov[0].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    rxov[1].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!rxbuf[0] || !rxbuf[1] || !evov.hEvent || !rxov[0].hEvent || !rxov[1].hEvent) goto out;

    for (;;) {
        if (!ev_posted) {
            ResetEvent(evov.hEvent);
            memset(&note, 0, sizeof note);
            if (!DeviceIoControl(u->h, IOCTL_GENERICUSBFN_BUS_EVENT_NOTIFICATION, NULL, 0, &note, sizeof note, NULL, &evov) &&
                GetLastError() != ERROR_IO_PENDING) {
                sbl_log("usb: BUS_EVENT_NOTIFICATION error %lu", GetLastError());
                break;
            }
            ev_posted = 1;
        }
        if (u->configured) {
            for (int i = 0; i < 2; i++) {
                if (rx_posted[i]) continue;
                ResetEvent(rxov[i].hEvent);
                if (!DeviceIoControl(u->h, IOCTL_GENERICUSBFN_TRANSFER_OUT, &u->pout, sizeof u->pout, rxbuf[i], RX_SIZE, NULL, &rxov[i]) &&
                    GetLastError() != ERROR_IO_PENDING) {
                    sbl_log("usb: OUT transfer error %lu", GetLastError());
                    break;
                }
                rx_posted[i] = 1;
            }
        }
        HANDLE w[2] = { evov.hEvent, rxov[rx_next].hEvent };
        DWORD r = WaitForMultipleObjects(rx_posted[rx_next] ? 2 : 1, w, FALSE, 1000);
        if (s->stop) break;
        if (r == WAIT_TIMEOUT) continue;

        if (r == WAIT_OBJECT_0 + 1) {   /* OUT data, completed in the order posted */
            n = 0;
            BOOL ok = GetOverlappedResult(u->h, &rxov[rx_next], &n, FALSE);
            rx_posted[rx_next] = 0;
            int i = rx_next;
            rx_next ^= 1;
            if (!ok) {
                DWORD e = GetLastError();
                if (e != ERROR_OPERATION_ABORTED) sbl_log("usb: OUT transfer completed with error %lu", e);
                prev_full = 0;
                continue;
            }
            if (n) sbl_rx(s, &u->link, rxbuf[i], n, !prev_full);
            prev_full = n == RX_SIZE;   /* a full read: the transfer may continue in the next one */
            continue;
        }

        /* bus event */
        n = 0;
        ev_posted = 0;
        if (!GetOverlappedResult(u->h, &evov, &n, FALSE)) {
            sbl_log("usb: bus event wait failed %lu: device gone", GetLastError());
            break;
        }
        sbl_log("usb: event %s", ev_name(note.Event));
        switch (note.Event) {
        case UsbfnEventConfigured:
        case UsbfnEventSetInterface:
            /* a new SET_CONFIGURATION / SET_INTERFACE ends the session too (PROTOCOL.md 6.4): drop the
             * reads posted on the old pipes and the session state before taking the new pipe ids */
            if (u->configured) {
                InterlockedExchange(&u->configured, 0);
                for (int i = 0; i < 2; i++)
                    if (rx_posted[i]) { CancelIoEx(u->h, &rxov[i]); GetOverlappedResult(u->h, &rxov[i], &n, TRUE); rx_posted[i] = 0; }
                rx_next = 0;
                prev_full = 0;
            }
            sbl_link_end(s, &u->link, ev_name(note.Event));
            if (get_pipes(u) == 0) {
                InterlockedExchange(&u->configured, 1);
                InterlockedExchange(&u->suspended, 0);
                sbl_log("usb: configured (value %u), waiting for HELLO", note.u.ConfigurationValue);
            }
            break;
        case UsbfnEventReset:
        case UsbfnEventDetach:
        case UsbfnEventUnConfigured:
        case UsbfnEventBusTearDown:
            if (u->configured) {
                InterlockedExchange(&u->configured, 0);
                for (int i = 0; i < 2; i++)
                    if (rx_posted[i]) { CancelIoEx(u->h, &rxov[i]); GetOverlappedResult(u->h, &rxov[i], &n, TRUE); rx_posted[i] = 0; }
                rx_next = 0;
                prev_full = 0;
            }
            sbl_link_end(s, &u->link, ev_name(note.Event));
            break;
        case UsbfnEventSuspend:
            InterlockedExchange(&u->suspended, 1);   /* the Mac sleeps; the dead timer ends the session */
            break;
        case UsbfnEventResume:
            InterlockedExchange(&u->suspended, 0);
            break;
        case UsbfnEventSetupPacket:
            /* no class/vendor requests are defined for this interface: complete a no-data request,
             * leave others to time out on the host. A request without a data stage has its status
             * stage IN (device -> host zero-length packet), USB 2.0 section 8.5.3: HANDSHAKE_IN. */
            sbl_log("usb: setup bmRequestType 0x%02x bRequest 0x%02x wLength %u", note.u.SetupPacket.bmRequestType.B,
                    note.u.SetupPacket.bRequest, note.u.SetupPacket.wLength);
            if (note.u.SetupPacket.wLength == 0)
                ioctl_wait(u->h, IOCTL_GENERICUSBFN_CONTROL_STATUS_HANDSHAKE_IN, NULL, 0, NULL, 0, NULL, 1000);
            break;
        default:
            break;
        }
    }

out:
    InterlockedExchange(&u->configured, 0);
    CancelIoEx(u->h, NULL);
    /* the buffers belong to the driver until every cancelled request has completed */
    for (int i = 0; i < 2; i++) if (rx_posted[i]) GetOverlappedResult(u->h, &rxov[i], &n, TRUE);
    if (ev_posted) GetOverlappedResult(u->h, &evov, &n, TRUE);
    sbl_link_end(s, &u->link, "device closed");
    ioctl_wait(u->h, IOCTL_GENERICUSBFN_DEACTIVATE_USB_BUS, NULL, 0, NULL, 0, NULL, 2000);
    if (evov.hEvent) CloseHandle(evov.hEvent);
    if (rxov[0].hEvent) CloseHandle(rxov[0].hEvent);
    if (rxov[1].hEvent) CloseHandle(rxov[1].hEvent);
    free(rxbuf[0]);
    free(rxbuf[1]);
}

static void usb_thread(void *arg)
{
    usb_ctx *u = arg;
    int logged = 0;
    while (!u->s->stop) {
        WCHAR path[512];
        if (find_interface(path, 512) < 0) {
            if (!logged) sbl_log("usb: no GenericUsbFn interface {d274dae6-...} yet (USB device mode off?); checking every 2 s");
            logged = 1;
            plat_sleep_ms(2000);
            continue;
        }
        logged = 0;
        u->h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
        if (u->h == INVALID_HANDLE_VALUE) {
            sbl_log("usb: cannot open the interface (%lu)", GetLastError());
            plat_sleep_ms(2000);
            continue;
        }
        char p8[512];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof p8, NULL, NULL);
        sbl_log("usb: opened %s", p8);
        serve(u);
        CloseHandle(u->h);
        u->h = INVALID_HANDLE_VALUE;
        plat_sleep_ms(1000);
    }
}

int sbl_usb_start(sbl_sender *s)
{
    usb_ctx *u = calloc(1, sizeof *u);
    if (!u) return -1;
    sbl_link_init(&u->link, "usb", 1);
    u->link.send = usb_send;
    u->link.close = usb_close;
    u->link.ctx = u;
    u->s = s;
    u->h = INVALID_HANDLE_VALUE;
    u->chunk = s->cfg.usb_chunk;
    u->txov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!u->txov.hEvent || plat_thread(usb_thread, u) < 0) { free(u); return -1; }
    sbl_log("usb: GenericUsbFn transport started (VID %04x PID %04x, IN 0x%02x OUT 0x%02x)", SBLK_USB_VID,
            SBLK_USB_PID, SBLK_USB_EP_IN, SBLK_USB_EP_OUT);
    return 0;
}
