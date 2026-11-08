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
 *
 * Round 7 (2026-11-07, service mode): Windows ended the user session, killed the helper (exit 0x40010004)
 * and the service started a new one. The bus stayed attached and configured (closing the handle does not
 * drop the pull-up, and IOCTL_GENERICUSBFN_DEACTIVATE_USB_BUS is documented as not supported: no log ever
 * showed it make the Mac see an unplug), so the new helper's ACTIVATE_USB_BUS produced no Attach / Reset /
 * Configured event at all, it never posted an OUT read, and the Mac's HELLO timed out for 2 minutes.
 * Bus events are edges; UFX does not replay them to a new handle and has no "query state" IOCTL. Now:
 *   - adopting a configured bus: whoever sees Configured sets the named event
 *     Global\StarbiteLinkUsbConfigured (the service holds a handle, so it outlives every helper), and
 *     Reset / Detach / UnConfigured / BusTearDown clear it. A helper that activates the bus while it is
 *     set and gets no bus event within ADOPT_MS takes the pipe ids from GET_CLASS_INFO and posts its OUT
 *     reads ("adopted"). If an adopted read fails it does not re-activate (that cannot help): it goes back
 *     to waiting for events. The Mac's side (link.m) escalates to SET_INTERFACE and a USB device reset when
 *     HELLO gets no answer, and either one produces real bus events here;
 *   - stop wakes this thread at once (stop_hook), and serve() tears down (cancel, DEACTIVATE, close)
 *     before the process exits: main.c waits for it (sbl_sender_shutdown) on logoff, quit and service stop;
 *   - every bus event and every recovery step is logged, and the stats line / a 30 s "waiting" line carry
 *     the bus state (sbl_link.describe).
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
#define ADOPT_MS 1500              /* no bus event this long after ACTIVATE on a configured bus: adopt it */
#define HINT_NAME L"Global\\StarbiteLinkUsbConfigured"

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
    HANDLE stop_ev;                /* manual reset: the sender is stopping (sbl_sender_stop) */
    HANDLE hint_ev;                /* Global\StarbiteLinkUsbConfigured (see the top) */
    int hint;                      /* the same, in this process (when the named event cannot be had) */
    volatile LONG adopted;         /* configured by adoption, not by a Configured event */
    /* state for the logs (describe) */
    volatile LONG activated;
    uint64_t activated_us, last_ev_us, configured_us;
    const char *last_ev;
    volatile LONG64 out_xfers, out_bytes;   /* since configured */
} usb_ctx;

static void hint_set(usb_ctx *u, int on)
{
    u->hint = on;
    if (u->hint_ev) { if (on) SetEvent(u->hint_ev); else ResetEvent(u->hint_ev); }
}

static int hint_get(usb_ctx *u)
{
    if (u->hint_ev) return WaitForSingleObject(u->hint_ev, 0) == WAIT_OBJECT_0;
    return u->hint;
}

static void usb_describe(sbl_link *l, char *b, size_t n)
{
    usb_ctx *u = (usb_ctx *)l;
    uint64_t now = plat_now_us();
    char ev[64] = "no bus event yet";
    if (u->last_ev) snprintf(ev, sizeof ev, "last bus event %s %llu s ago", u->last_ev,
                             (unsigned long long)((now - u->last_ev_us) / 1000000u));
    snprintf(b, n, "usb: %s%s%s, %s, OUT %lld transfers %lld KiB since configured, bus %s",
             !u->activated ? "not activated" : u->configured ? (u->adopted ? "configured (adopted)" : "configured")
                                                             : "activated, not configured",
             u->suspended ? ", suspended" : "", u->wedged ? ", WEDGED" : "", ev, (long long)u->out_xfers,
             (long long)(u->out_bytes >> 10), hint_get(u) ? "marked configured" : "not marked configured");
}

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
    /* the whole buffer, not n bytes: GenericUsbFn completes GET_CLASS_INFO with 0 bytes reported (seen on
     * the S9 with d175117: every PipeCount read as 0, "no bulk IN + OUT pipe pair", no session) */
    if (!err && out) memcpy(out, obuf, outl);
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
                HANDLE w[3] = { ov->hEvent, u->recycle_ev, u->stop_ev };
                DWORD r = WaitForMultipleObjects(3, w, FALSE, TX_TIMEOUT_MS);
                if (r != WAIT_OBJECT_0) {
                    /* the Mac is not reading (killed, asleep), the function is being re-activated, or the
                     * sender stops */
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

static int get_pipes_once(usb_ctx *u, int attempt)
{
    USBFN_CLASS_INFORMATION_PACKET ci;
    memset(&ci, 0, sizeof ci);
    DWORD n = 0, err = ioctl_wait(u, IOCTL_GENERICUSBFN_GET_CLASS_INFO, NULL, 0, &ci, sizeof ci, &n, 2000);
    if (err) { sbl_log("usb: GET_CLASS_INFO error %lu (attempt %d)", err, attempt); return -1; }
    if (attempt > 1 || !ci.HighSpeedClassInterface.PipeCount)
        sbl_log("usb: GET_CLASS_INFO attempt %d: %lu of %u bytes, pipes HS %u FS %u SS %u, interface %u", attempt,
                n, (unsigned)sizeof ci, ci.HighSpeedClassInterface.PipeCount, ci.FullSpeedClassInterface.PipeCount,
                ci.SuperSpeedClassInterface.PipeCount, ci.HighSpeedClassInterface.InterfaceNumber);
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
    if (!have_in || !have_out) { sbl_log("usb: no bulk IN + OUT pipe pair (attempt %d)", attempt); return -1; }
    return 0;
}

/* on Configured: the class information may lag the event; retry with a short backoff (50 .. 800 ms) */
static int get_pipes(usb_ctx *u)
{
    for (int a = 1; a <= 5; a++) {
        if (get_pipes_once(u, a) == 0) return 0;
        if (u->s->stop || u->wedged) break;
        plat_sleep_ms(50 << (a - 1));
        u->link.rx_beat_us = plat_now_us();
    }
    return -1;
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

/* adopted pipes that do not work from this handle: back to waiting for a bus event (re-activating the
 * function cannot help: it does not detach; the Mac's SET_INTERFACE / device reset makes real events) */
static int unadopt(usb_ctx *u, rx_slot **rx, const char *what, DWORD e)
{
    sbl_log("usb: %s on the adopted pipes (error %lu): not usable from this handle; waiting for a bus event "
            "(the Mac app resets the device when HELLO gets no answer)", what, e);
    InterlockedExchange(&u->configured, 0);
    InterlockedExchange(&u->adopted, 0);
    int r = rx_cancel(u, rx);
    sbl_link_end(u->s, &u->link, "adopted pipes failed");
    return r;
}

/* an OUT read completed cancelled (error ERROR_OPERATION_ABORTED, not by us) or with 0 bytes. A few in a row
 * are normal (a cancel before a bus event, a zero-length packet); many in a row mean the driver completes
 * every read at once and reposting would spin this thread. Adopted pipes: give them up after RX_EMPTY_MAX
 * (back to waiting for a bus event). Pipes from a real Configured event: from then on wait 50 ms before
 * reposting (stop and bus events still wake it). Returns -1 when a cancel failed (the caller bails out). */
#define RX_EMPTY_MAX 8
static int rx_empty_spin(usb_ctx *u, rx_slot **rx, int *run, int *logged, DWORD e)
{
    if (++*run < RX_EMPTY_MAX) return 0;
    if (u->adopted) {
        *run = 0;
        *logged = 0;
        return unadopt(u, rx, e ? "OUT reads keep completing cancelled" : "OUT reads keep completing empty",
                       e ? e : ERROR_NO_DATA);
    }
    if (!*logged) {
        *logged = 1;
        sbl_log("usb: %d OUT reads in a row completed %s (last error %lu): reposting every 50 ms until data or a "
                "bus event comes", *run, e ? "cancelled" : "empty", e);
    }
    WaitForSingleObject(u->stop_ev, 50);
    return 0;
}

/* is a session up on this link? (for the "waiting" log) */
static int has_session(usb_ctx *u)
{
    plat_mutex_lock(&u->s->lk);
    int on = u->s->active && u->s->owner == &u->link;
    plat_mutex_unlock(&u->s->lk);
    return on;
}

/* runs one opened device until it goes away or must be re-activated; returns to re-discover */
static void serve(usb_ctx *u)
{
    sbl_sender *s = u->s;
    u->link.rx_beat_us = plat_now_us();
    DWORD err = ioctl_wait(u, IOCTL_GENERICUSBFN_ACTIVATE_USB_BUS, NULL, 0, NULL, 0, NULL, 5000);
    if (err) { sbl_log("usb: ACTIVATE_USB_BUS error %lu", err); return; }
    uint64_t t_act = plat_now_us();
    u->activated_us = t_act;
    u->last_ev = NULL;
    InterlockedExchange(&u->adopted, 0);
    InterlockedExchange(&u->activated, 1);
    int was_configured = hint_get(u);
    uint64_t adopt_at = was_configured ? t_act + ADOPT_MS * 1000ull : 0;
    sbl_log("usb: bus activated, waiting for the Mac%s", was_configured
            ? " (the bus was configured before this handle: its pipes are adopted if no bus event comes within 1.5 s)" : "");

    USBFN_NOTIFICATION *note = calloc(1, sizeof *note);
    OVERLAPPED *evov = calloc(1, sizeof *evov);
    rx_slot *rx[2] = { rx_new(), rx_new() };
    u->txov = calloc(1, sizeof *u->txov);
    int rx_next = 0, prev_full = 0, ev_posted = 0, events_seen = 0;
    int adopted_rx_logged = 0, adopted_quiet_logged = 0;
    int rx_empty_run = 0, rx_spin_logged = 0;   /* consecutive OUT reads that completed aborted or empty */
    uint64_t next_state_log = t_act + 30000000u, adopted_at = 0;
    DWORD n = 0;
    if (evov) evov->hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (u->txov) u->txov->hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!note || !evov || !evov->hEvent || !rx[0] || !rx[1] || !u->txov || !u->txov->hEvent) {
        sbl_log("usb: out of memory");
        goto out;
    }

    while (!s->stop && !u->recycle) {
        uint64_t now = plat_now_us();
        u->link.rx_beat_us = now;
        if (now >= next_state_log) {   /* a helper stuck without a session is obvious in the log */
            next_state_log = now + 30000000u;
            if (!has_session(u)) {
                char d[200];
                usb_describe(&u->link, d, sizeof d);
                sbl_log("usb: waiting, no session: %s", d);
            }
        }
        if (u->adopted && !adopted_rx_logged && !adopted_quiet_logged && now - adopted_at > 30000000u) {
            adopted_quiet_logged = 1;
            sbl_log("usb: adopted pipes, but no OUT data from the Mac for 30 s (still waiting for HELLO or a bus event)");
        }
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
        if (adopt_at && now >= adopt_at) {
            adopt_at = 0;
            if (!events_seen && !u->configured) {
                sbl_log("usb: no bus event %llu ms after ACTIVATE, and the bus was configured before (the previous "
                        "helper ended without a detach): adopting its pipes", (unsigned long long)((now - t_act) / 1000u));
                if (get_pipes_once(u, 1) == 0) {
                    sbl_link_end(s, &u->link, "USB pipes adopted");
                    u->out_xfers = u->out_bytes = 0;
                    u->configured_us = now;
                    adopted_at = now;
                    adopted_rx_logged = adopted_quiet_logged = 0;
                    InterlockedExchange(&u->adopted, 1);
                    InterlockedExchange(&u->configured, 1);
                    InterlockedExchange(&u->suspended, 0);
                    sbl_log("usb: adopted pipes IN %lu OUT %lu: OUT reads posted, waiting for HELLO (the Mac sends it "
                            "again every few seconds)", (unsigned long)u->pin, (unsigned long)u->pout);
                } else {
                    sbl_log("usb: no pipes to adopt: waiting for a bus event (the Mac app resets the device when HELLO "
                            "gets no answer)");
                }
            }
        }
        if (u->configured) {
            int failed = 0;
            for (int i = 0; i < 2; i++) {
                if (rx[i]->posted) continue;
                ResetEvent(rx[i]->ov.hEvent);
                if (!DeviceIoControl(u->h, IOCTL_GENERICUSBFN_TRANSFER_OUT, &u->pout, sizeof u->pout, rx[i]->buf, RX_SIZE, NULL, &rx[i]->ov) &&
                    GetLastError() != ERROR_IO_PENDING) {
                    DWORD e = GetLastError();
                    if (u->adopted) {
                        if (unadopt(u, rx, "an OUT read could not be queued", e) < 0) goto out;
                        rx_next = 0;
                        prev_full = 0;
                        failed = 1;
                        break;
                    }
                    char why[64];
                    snprintf(why, sizeof why, "OUT transfer could not be queued (%lu)", e);
                    request_recycle(u, why);
                    break;
                }
                rx[i]->posted = 1;
            }
            if (u->recycle) break;
            if (failed) continue;
        }
        DWORD ms = 1000;
        if (adopt_at) {
            uint64_t t = plat_now_us();
            uint64_t left = adopt_at > t ? (adopt_at - t) / 1000u + 1 : 0;
            if (left < ms) ms = (DWORD)left;
        }
        HANDLE w[4] = { evov->hEvent, u->recycle_ev, u->stop_ev, rx[rx_next]->ov.hEvent };
        DWORD r = WaitForMultipleObjects(rx[rx_next]->posted ? 4 : 3, w, FALSE, ms);
        u->link.rx_beat_us = plat_now_us();
        if (s->stop || u->recycle || r == WAIT_OBJECT_0 + 2) break;
        if (r == WAIT_TIMEOUT) continue;

        if (r == WAIT_OBJECT_0 + 3) {   /* OUT data, completed in the order posted */
            n = 0;
            BOOL ok = GetOverlappedResult(u->h, &rx[rx_next]->ov, &n, FALSE);
            rx[rx_next]->posted = 0;
            int i = rx_next;
            rx_next ^= 1;
            if (!ok) {
                DWORD e = GetLastError();
                prev_full = 0;
                if (e == ERROR_OPERATION_ABORTED) {
                    /* the driver cancelled a read we did not cancel: a bus event usually follows. Reposting
                     * at once would spin a core if it keeps cancelling (adopted pipes the driver purged, a
                     * stale configured marker): give adopted pipes up, otherwise throttle (review, round 8) */
                    if (rx_empty_spin(u, rx, &rx_empty_run, &rx_spin_logged, e) < 0) goto out;
                    if (!u->configured) rx_next = 0;
                    continue;
                }
                if (u->adopted) {
                    if (unadopt(u, rx, "an OUT read failed", e) < 0) goto out;
                    rx_next = 0;
                    continue;
                }
                char why[64];
                snprintf(why, sizeof why, "OUT transfer failed (%lu)", e);
                request_recycle(u, why);
                break;
            }
            if (!n) {
                /* a zero-length packet ends a transfer that filled whole reads; many in a row are not data */
                if (rx_empty_spin(u, rx, &rx_empty_run, &rx_spin_logged, 0) < 0) goto out;
                if (!u->configured) { rx_next = 0; prev_full = 0; continue; }
            } else {
                rx_empty_run = 0;
                rx_spin_logged = 0;
            }
            if (n) {
                InterlockedIncrement64(&u->out_xfers);
                InterlockedAdd64(&u->out_bytes, (LONG64)n);
                if (u->adopted && !adopted_rx_logged) {
                    adopted_rx_logged = 1;
                    sbl_log("usb: the adopted pipes work: first OUT transfer %lu bytes%s, %llu ms after adopting", n,
                            n >= 6 && rx[i]->buf[0] == 'S' && rx[i]->buf[5] == SBLK_T_HELLO ? " (HELLO)" : "",
                            (unsigned long long)((plat_now_us() - adopted_at) / 1000u));
                }
                sbl_rx(s, &u->link, rx[i]->buf, n, !prev_full);
            }
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
        uint64_t te = plat_now_us();
        events_seen = 1;
        u->last_ev = ev_name(note->Event);
        u->last_ev_us = te;
        sbl_log("usb: event %s (%llu ms after ACTIVATE%s)", ev_name(note->Event), (unsigned long long)((te - t_act) / 1000u),
                u->adopted ? ", pipes were adopted" : "");
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
            InterlockedExchange(&u->adopted, 0);
            sbl_link_end(s, &u->link, ev_name(note->Event));
            static int pipe_fails;   /* consecutive: recycle at most 3 times in a row, then just wait */
            if (get_pipes(u) == 0) {
                pipe_fails = 0;
                u->out_xfers = u->out_bytes = 0;
                u->configured_us = te;
                InterlockedExchange(&u->configured, 1);
                InterlockedExchange(&u->suspended, 0);
                hint_set(u, 1);
                sbl_log("usb: configured (value %u), pipes IN %lu OUT %lu, waiting for HELLO",
                        note->Event == UsbfnEventConfigured ? note->u.ConfigurationValue : 0, (unsigned long)u->pin,
                        (unsigned long)u->pout);
            } else {
                /* configured without pipes = no session ever: start the function over rather than sit here */
                if (++pipe_fails <= 3) request_recycle(u, "no usable pipes after Configured");
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
            InterlockedExchange(&u->adopted, 0);
            hint_set(u, 0);
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
    if (u->recycle) sbl_log("usb: %s: re-activating the USB function", u->recycle_why);
    else if (s->stop) sbl_log("usb: the sender is stopping: cancelling USB requests and deactivating the bus");
    InterlockedExchange(&u->configured, 0);
    InterlockedExchange(&u->adopted, 0);
    /* the session first: a send in progress sees recycle_ev and returns (txlk is then free) */
    SetEvent(u->recycle_ev);
    sbl_link_end(s, &u->link, u->recycle ? "USB function re-activated" : s->stop ? "sender stopping" : "device closed");
    /* every request back from the driver before its memory is freed; what it keeps is leaked */
    if (rx[0] && rx[1]) rx_cancel(u, rx);
    if (ev_posted && evov) {
        CancelIoEx(u->h, evov);
        if (finish_cancelled(u->h, evov, &n) < 0) { evov = NULL; note = NULL; set_wedged(u, "the bus event request"); }
    }
    /* documented as unsupported by GenericUsbFn; kept, and its result logged, to see what this build does */
    err = ioctl_wait(u, IOCTL_GENERICUSBFN_DEACTIVATE_USB_BUS, NULL, 0, NULL, 0, NULL, 2000);
    sbl_log("usb: DEACTIVATE_USB_BUS: %s (%lu)", err ? "error" : "ok", err);
    InterlockedExchange(&u->activated, 0);
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
            WaitForSingleObject(u->stop_ev, 1000);
            continue;
        }
        WCHAR path[512];
        if (find_interface(path, 512) < 0) {
            if (!logged) sbl_log("usb: no GenericUsbFn interface {d274dae6-...} yet (USB device mode off?); checking every 2 s");
            logged = 1;
            WaitForSingleObject(u->stop_ev, 2000);
            continue;
        }
        logged = 0;
        u->h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
        if (u->h == INVALID_HANDLE_VALUE) {
            sbl_log("usb: cannot open the interface (%lu)", GetLastError());
            WaitForSingleObject(u->stop_ev, 2000);
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
        if (u->s->stop) { sbl_log("usb: interface closed (sender stopping)"); break; }
        /* after a re-activation stay off the bus a while (in case DEACTIVATE did detach) */
        WaitForSingleObject(u->stop_ev, recycled ? 2000 : 1000);
    }
    InterlockedDecrement(&u->s->alive);
}

static void usb_stop_hook(void *ctx)
{
    usb_ctx *u = ctx;
    SetEvent(u->stop_ev);
}

int sbl_usb_start(sbl_sender *s)
{
    usb_ctx *u = calloc(1, sizeof *u);
    if (!u) return -1;
    sbl_link_init(&u->link, "usb", 1);
    u->link.send = usb_send;
    u->link.close = usb_close;
    u->link.describe = usb_describe;
    u->link.ctx = u;
    u->s = s;
    u->h = INVALID_HANDLE_VALUE;
    u->chunk = s->cfg.usb_chunk;
    u->recycle_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    u->stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    /* the service created it (and keeps it); a Startup-folder instance gets a private one */
    u->hint_ev = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, HINT_NAME);
    DWORD he = u->hint_ev ? 0 : GetLastError();
    if (!u->hint_ev) u->hint_ev = CreateEventW(NULL, TRUE, FALSE, HINT_NAME);
    if (!u->recycle_ev || !u->stop_ev) { free(u); return -1; }
    s->stop_hook = usb_stop_hook;
    s->stop_hook_ctx = u;
    InterlockedIncrement(&s->alive);
    if (plat_thread(usb_thread, u) < 0) { InterlockedDecrement(&s->alive); s->stop_hook = NULL; free(u); return -1; }
    sbl_log("usb: GenericUsbFn transport started (VID %04x PID %04x, IN 0x%02x OUT 0x%02x); configured-bus marker %s%s",
            SBLK_USB_VID, SBLK_USB_PID, SBLK_USB_EP_IN, SBLK_USB_EP_OUT,
            !u->hint_ev ? "unavailable" : he ? "created here" : "from the service",
            u->hint_ev && hint_get(u) ? " (set: the bus was configured by an earlier helper)" : "");
    return 0;
}
