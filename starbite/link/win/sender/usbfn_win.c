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
 *
 * Recovery (the S9, 2026-11-07: after the Mac app was killed mid-session, an IN transfer timed out and was
 * cancelled, and from then on the phone never completed another OUT transfer: the new Mac app's HELLO
 * timed out, "link dead" forever, until Windows was restarted):
 *   - no wait on the driver is unbounded: a cancelled request gets CANCEL_MS to complete;
 *   - an IN transfer that times out (the Mac stopped reading) or fails, or an OUT transfer that fails,
 *     re-activates the whole USB function: everything cancelled, DEACTIVATE_USB_BUS (the Mac sees an
 *     unplug), the handle closed and opened again, ACTIVATE_USB_BUS. The Mac app re-opens the interface
 *     and starts over with a clean pipe state;
 *   - a request that does not even complete after a cancel leaves its buffer with the driver: it is
 *     leaked, never reused, and the process asks to be restarted (sbl_request_restart; main.c);
 *   - the receive loop stamps rx_beat_us at least every second for the watchdog.
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
#define CANCEL_MS 3000             /* a cancelled request must complete within this */

typedef struct usb_ctx {
    sbl_link link;                 /* first: sbl_link * == usb_ctx * */
    sbl_sender *s;
    HANDLE h;
    USBFNPIPEID pin, pout;
    volatile LONG configured;
    volatile LONG suspended;
    OVERLAPPED *txov;              /* per open handle; leaked if the driver never gives it back */
    int chunk;
    HANDLE recycle_ev;             /* manual reset: re-activate the USB function (serve() returns) */
    volatile LONG recycle;
    char recycle_why[96];
    volatile LONG wedged;          /* a request never completed: only a process restart helps */
} usb_ctx;

static void request_recycle(usb_ctx *u, const char *why)
{
    if (InterlockedExchange(&u->recycle, 1) == 0) {
        snprintf(u->recycle_why, sizeof u->recycle_why, "%s", why);
        SetEvent(u->recycle_ev);
    }
}

static void set_wedged(usb_ctx *u, const char *what)
{
    if (InterlockedExchange(&u->wedged, 1) == 0) {
        char why[96];
        snprintf(why, sizeof why, "usb: %s did not complete %u ms after its cancel", what, CANCEL_MS);
        sbl_log("%s: the USB driver is wedged, asking for a process restart", why);
        sbl_request_restart(u->s, why);
    }
}

/* waits up to CANCEL_MS for a cancelled (or finishing) request; 0 = completed (result in *n / return of
 * GetOverlappedResult ignored), -1 = still owned by the driver */
static int finish_cancelled(HANDLE h, OVERLAPPED *ov, DWORD *n)
{
    if (WaitForSingleObject(ov->hEvent, CANCEL_MS) != WAIT_OBJECT_0) return -1;
    GetOverlappedResult(h, ov, n, FALSE);
    return 0;
}

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

/* an IOCTL on the overlapped handle, waited for at most ms (+ CANCEL_MS for the cancel); returns the
 * Win32 error (0 = ok). out must stay valid until it completes: callers pass small buffers that are
 * copied, and a request that never completes leaks its OVERLAPPED and marks the device wedged. */
static DWORD ioctl_wait(usb_ctx *u, DWORD code, void *in, DWORD inl, void *out, DWORD outl, DWORD *ret, DWORD ms)
{
    OVERLAPPED *ov = calloc(1, sizeof *ov);
    if (!ov) return ERROR_OUTOFMEMORY;
    uint8_t *obuf = outl ? calloc(1, outl) : NULL;   /* the driver writes here, not into the caller's */
    ov->hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov->hEvent || (outl && !obuf)) { DWORD e = GetLastError(); if (ov->hEvent) CloseHandle(ov->hEvent); free(ov); free(obuf); return e ? e : ERROR_OUTOFMEMORY; }
    DWORD n = 0, err = 0;
    if (!DeviceIoControl(u->h, code, in, inl, obuf, outl, NULL, ov)) {
        err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            err = 0;
            if (WaitForSingleObject(ov->hEvent, ms) == WAIT_TIMEOUT) {
                CancelIoEx(u->h, ov);
                err = WAIT_TIMEOUT;
                if (finish_cancelled(u->h, ov, &n) < 0) {
                    set_wedged(u, "an IOCTL");
                    return WAIT_TIMEOUT;   /* ov, its event and obuf stay with the driver */
                }
            }
        }
    }
    if (!err && !GetOverlappedResult(u->h, ov, &n, TRUE)) err = GetLastError();   /* completed: no wait */
    if (!err && out && n <= outl) memcpy(out, obuf, n);
    CloseHandle(ov->hEvent);
    free(ov);
    free(obuf);
    if (ret) *ret = n;
    return err;
}

/* ---- sending (txlk held by session.c) ---------------------------------------------------------------- */

static int usb_send(sbl_link *l, const uint8_t *p, size_t n)
{
    usb_ctx *u = (usb_ctx *)l;
    if (!u->configured || u->recycle || u->wedged || !u->txov) return -1;
    OVERLAPPED *ov = u->txov;
    while (n) {
        DWORD len = (DWORD)(n > (size_t)u->chunk ? (size_t)u->chunk : n);
        DWORD code = (len % SBLK_USB_MPS) == 0 ? IOCTL_GENERICUSBFN_TRANSFER_IN_APPEND_ZERO_PKT
                                                : IOCTL_GENERICUSBFN_TRANSFER_IN;
        ResetEvent(ov->hEvent);
        DWORD done = 0, err = 0;
        if (!DeviceIoControl(u->h, code, &u->pin, sizeof u->pin, (void *)p, len, NULL, ov)) {
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                err = 0;
                HANDLE w[2] = { ov->hEvent, u->recycle_ev };
                DWORD r = WaitForMultipleObjects(2, w, FALSE, TX_TIMEOUT_MS);
                if (r != WAIT_OBJECT_0) {
                    /* the Mac is not reading (killed, asleep), or the function is being re-activated */
                    CancelIoEx(u->h, ov);
                    err = r == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_OPERATION_ABORTED;
                    if (finish_cancelled(u->h, ov, &done) < 0) {
                        u->txov = NULL;   /* the driver still has it (and p): never touch it again */
                        set_wedged(u, "an IN transfer");
                        request_recycle(u, "IN transfer never completed");
                        return -1;
                    }
                }
            }
        }
        if (!err && !GetOverlappedResult(u->h, ov, &done, TRUE)) err = GetLastError();   /* completed: no wait */
        if (err || done == 0 || done > len) {
            sbl_log("usb: IN transfer of %lu bytes: error %lu, %lu sent", len, err, done);
            /* a cancelled or failed IN can leave this controller's endpoints wedged (seen on the S9: no OUT
             * transfer ever completed afterwards): start the USB function over */
            request_recycle(u, err == WAIT_TIMEOUT ? "IN transfer timed out (the Mac stopped reading)"
                                                   : "IN transfer failed");
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
    DWORD n = 0, err = ioctl_wait(u, IOCTL_GENERICUSBFN_GET_CLASS_INFO, NULL, 0, &ci, sizeof ci, &n, 2000);
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

/* the two OUT reads: buffers and OVERLAPPEDs on the heap, so a read the driver never returns can be
 * left with it (leaked) instead of being reused */
typedef struct rx_slot { OVERLAPPED ov; uint8_t *buf; int posted; } rx_slot;

static rx_slot *rx_new(void)
{
    rx_slot *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->buf = malloc(RX_SIZE);
    r->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!r->buf || !r->ov.hEvent) { free(r->buf); if (r->ov.hEvent) CloseHandle(r->ov.hEvent); free(r); return NULL; }
    return r;
}

static void rx_free(rx_slot *r)
{
    if (!r) return;
    CloseHandle(r->ov.hEvent);
    free(r->buf);
    free(r);
}

/* cancels the posted reads; 0 = all back, -1 = the driver kept one (it is leaked: *slot = NULL) */
static int rx_cancel(usb_ctx *u, rx_slot **slot)
{
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        if (!slot[i] || !slot[i]->posted) continue;
        DWORD n = 0;
        CancelIoEx(u->h, &slot[i]->ov);
        if (finish_cancelled(u->h, &slot[i]->ov, &n) < 0) { slot[i] = NULL; ok = -1; set_wedged(u, "an OUT transfer"); }
        else slot[i]->posted = 0;
    }
    return ok;
}

/* runs one opened device until it goes away or must be re-activated; returns to re-discover */
static void serve(usb_ctx *u)
{
    sbl_sender *s = u->s;
    u->link.rx_beat_us = plat_now_us();
    DWORD err = ioctl_wait(u, IOCTL_GENERICUSBFN_ACTIVATE_USB_BUS, NULL, 0, NULL, 0, NULL, 5000);
    if (err) { sbl_log("usb: ACTIVATE_USB_BUS error %lu", err); return; }
    sbl_log("usb: bus activated, waiting for the Mac");

    USBFN_NOTIFICATION *note = calloc(1, sizeof *note);
    OVERLAPPED *evov = calloc(1, sizeof *evov);
    rx_slot *rx[2] = { rx_new(), rx_new() };
    u->txov = calloc(1, sizeof *u->txov);
    int rx_next = 0, prev_full = 0, ev_posted = 0;
    DWORD n = 0;
    if (evov) evov->hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (u->txov) u->txov->hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!note || !evov || !evov->hEvent || !rx[0] || !rx[1] || !u->txov || !u->txov->hEvent) {
        sbl_log("usb: out of memory");
        goto out;
    }

    while (!s->stop && !u->recycle) {
        u->link.rx_beat_us = plat_now_us();
        if (!ev_posted) {
            ResetEvent(evov->hEvent);
            memset(note, 0, sizeof *note);
            if (!DeviceIoControl(u->h, IOCTL_GENERICUSBFN_BUS_EVENT_NOTIFICATION, NULL, 0, note, sizeof *note, NULL, evov) &&
                GetLastError() != ERROR_IO_PENDING) {
                sbl_log("usb: BUS_EVENT_NOTIFICATION error %lu", GetLastError());
                break;
            }
            ev_posted = 1;
        }
        if (u->configured) {
            for (int i = 0; i < 2; i++) {
                if (rx[i]->posted) continue;
                ResetEvent(rx[i]->ov.hEvent);
                if (!DeviceIoControl(u->h, IOCTL_GENERICUSBFN_TRANSFER_OUT, &u->pout, sizeof u->pout, rx[i]->buf, RX_SIZE, NULL, &rx[i]->ov) &&
                    GetLastError() != ERROR_IO_PENDING) {
                    char why[64];
                    snprintf(why, sizeof why, "OUT transfer could not be queued (%lu)", GetLastError());
                    request_recycle(u, why);
                    break;
                }
                rx[i]->posted = 1;
            }
            if (u->recycle) break;
        }
        HANDLE w[3] = { evov->hEvent, u->recycle_ev, rx[rx_next]->ov.hEvent };
        DWORD r = WaitForMultipleObjects(rx[rx_next]->posted ? 3 : 2, w, FALSE, 1000);
        u->link.rx_beat_us = plat_now_us();
        if (s->stop || u->recycle) break;
        if (r == WAIT_TIMEOUT) continue;

        if (r == WAIT_OBJECT_0 + 2) {   /* OUT data, completed in the order posted */
            n = 0;
            BOOL ok = GetOverlappedResult(u->h, &rx[rx_next]->ov, &n, FALSE);
            rx[rx_next]->posted = 0;
            int i = rx_next;
            rx_next ^= 1;
            if (!ok) {
                DWORD e = GetLastError();
                prev_full = 0;
                if (e == ERROR_OPERATION_ABORTED) continue;
                char why[64];
                snprintf(why, sizeof why, "OUT transfer failed (%lu)", e);
                request_recycle(u, why);
                break;
            }
            if (n) sbl_rx(s, &u->link, rx[i]->buf, n, !prev_full);
            prev_full = n == RX_SIZE;   /* a full read: the transfer may continue in the next one */
            continue;
        }

        /* bus event */
        n = 0;
        ev_posted = 0;
        if (!GetOverlappedResult(u->h, evov, &n, FALSE)) {
            sbl_log("usb: bus event wait failed %lu: device gone", GetLastError());
            break;
        }
        sbl_log("usb: event %s", ev_name(note->Event));
        switch (note->Event) {
        case UsbfnEventConfigured:
        case UsbfnEventSetInterface:
            /* a new SET_CONFIGURATION / SET_INTERFACE ends the session too (PROTOCOL.md 6.4): drop the
             * reads posted on the old pipes and the session state before taking the new pipe ids */
            if (u->configured) {
                InterlockedExchange(&u->configured, 0);
                if (rx_cancel(u, rx) < 0) goto out;
                rx_next = 0;
                prev_full = 0;
            }
            sbl_link_end(s, &u->link, ev_name(note->Event));
            if (get_pipes(u) == 0) {
                InterlockedExchange(&u->configured, 1);
                InterlockedExchange(&u->suspended, 0);
                sbl_log("usb: configured (value %u), waiting for HELLO", note->u.ConfigurationValue);
            }
            break;
        case UsbfnEventReset:
        case UsbfnEventDetach:
        case UsbfnEventUnConfigured:
        case UsbfnEventBusTearDown:
            if (u->configured) {
                InterlockedExchange(&u->configured, 0);
                if (rx_cancel(u, rx) < 0) goto out;
                rx_next = 0;
                prev_full = 0;
            }
            sbl_link_end(s, &u->link, ev_name(note->Event));
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
            sbl_log("usb: setup bmRequestType 0x%02x bRequest 0x%02x wLength %u", note->u.SetupPacket.bmRequestType.B,
                    note->u.SetupPacket.bRequest, note->u.SetupPacket.wLength);
            if (note->u.SetupPacket.wLength == 0)
                ioctl_wait(u, IOCTL_GENERICUSBFN_CONTROL_STATUS_HANDSHAKE_IN, NULL, 0, NULL, 0, NULL, 1000);
            break;
        default:
            break;
        }
    }

out:
    if (u->recycle) sbl_log("usb: %s: re-activating the USB function (the Mac sees an unplug and a replug)", u->recycle_why);
    InterlockedExchange(&u->configured, 0);
    /* the session first: a send in progress sees recycle_ev and returns (txlk is then free) */
    SetEvent(u->recycle_ev);
    sbl_link_end(s, &u->link, u->recycle ? "USB function re-activated" : "device closed");
    /* every request back from the driver before its memory is freed; what it keeps is leaked */
    if (rx[0] && rx[1]) rx_cancel(u, rx);
    if (ev_posted && evov) {
        CancelIoEx(u->h, evov);
        if (finish_cancelled(u->h, evov, &n) < 0) { evov = NULL; note = NULL; set_wedged(u, "the bus event request"); }
    }
    ioctl_wait(u, IOCTL_GENERICUSBFN_DEACTIVATE_USB_BUS, NULL, 0, NULL, 0, NULL, 2000);
    if (evov) { if (evov->hEvent) CloseHandle(evov->hEvent); free(evov); }
    free(note);
    rx_free(rx[0]);
    rx_free(rx[1]);
    /* the IN OVERLAPPED: usb_send runs under txlk, which sbl_link_end above waited for, and a send that
     * starts now fails at once (configured = 0); a wedged one already dropped it (txov = NULL) */
    plat_mutex_lock(&s->txlk);
    if (u->txov) { if (u->txov->hEvent) CloseHandle(u->txov->hEvent); free(u->txov); u->txov = NULL; }
    plat_mutex_unlock(&s->txlk);
}

static void usb_thread(void *arg)
{
    usb_ctx *u = arg;
    int logged = 0;
    sbl_watch_link(u->s, &u->link);
    while (!u->s->stop) {
        u->link.rx_beat_us = plat_now_us();
        if (u->wedged) {   /* only a new process helps; main.c's watchdog restarts it */
            plat_sleep_ms(1000);
            continue;
        }
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
        InterlockedExchange(&u->recycle, 0);
        ResetEvent(u->recycle_ev);
        serve(u);
        int recycled = u->recycle != 0;
        CloseHandle(u->h);
        u->h = INVALID_HANDLE_VALUE;
        /* after a re-activation stay off the bus long enough for the Mac to see the unplug */
        plat_sleep_ms(recycled ? 2000 : 1000);
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
    u->recycle_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!u->recycle_ev || plat_thread(usb_thread, u) < 0) { free(u); return -1; }
    sbl_log("usb: GenericUsbFn transport started (VID %04x PID %04x, IN 0x%02x OUT 0x%02x)", SBLK_USB_VID,
            SBLK_USB_PID, SBLK_USB_EP_IN, SBLK_USB_EP_OUT);
    return 0;
}
