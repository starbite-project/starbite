/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Link, Mac viewer: the link. USB (IOUSBHost) and TCP transports around one sbv session,
 * on one serial queue. See link.h and starbite/link/PROTOCOL.md (sections 1, 4, 5, 6).
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/IOMessage.h>
#import <IOUSBHost/IOUSBHost.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <os/lock.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "link.h"

#define TCP_RX_BUF     (256u * 1024u)
#define USB_RX_BUF     (64u * 1024u)     /* multiple of 512 (PROTOCOL.md 6.2) */
#define USB_RX_QUEUED  4
#define USB_TX_LIMIT   (8 * 1024 * 1024)
#define RETRY_S        1.0

enum { TR_NONE = 0, TR_TCP = 1, TR_USB = 2 };

static uint64_t mono_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u; }

/* one TCP connection; the reader thread owns the fd and closes it */
@interface SBLConn : NSObject {
@public
    int fd;
    int closed;
    os_unfair_lock lk;
    dispatch_semaphore_t done;   /* signalled when the reader has closed the fd */
}
@end
@implementation SBLConn
@end

@interface SBLLink : NSObject {
@public
    sbl_config cfg;
    sbl_callbacks cb;
    dispatch_queue_t q;
    sbv s;
    int running;
    int transport;
    char tdesc[96];

    /* TCP */
    int fd;
    SBLConn *conn;
    uint64_t tcp_gen;
    int tcp_connecting;

    /* USB */
    IONotificationPortRef nport;
    io_iterator_t it_if, it_dev, it_dev_gone;
    IOUSBHostInterface *intf;
    IOUSBHostDevice *dev;
    IOUSBHostPipe *pin, *pout;
    uint64_t usb_gen;
    int usb_present;
    uint64_t last_recover_us;
    _Atomic long usb_out_pending;
    int usb_hunt;                  /* after our HELLO: skip IN transfers until one starts with HELLO/BYE */
    uint64_t usb_skipped;

    dispatch_source_t timer;
    os_unfair_lock lock;
    sbl_status st;
    uint64_t win_t, win_frames, win_bytes;
}
@end

static void logf_(SBLLink *L, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(SBLLink *L, const char *fmt, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (L->cfg.verbose) fprintf(stderr, "starbite-link: %s\n", line);
    if (L->cb.log) L->cb.log(L->cb.ctx, line);
}

static void set_conn(SBLLink *L, int conn)
{
    os_unfair_lock_lock(&L->lock);
    L->st.conn = conn;
    snprintf(L->st.transport, sizeof L->st.transport, "%s", conn >= SBL_CONN_HANDSHAKE ? L->tdesc : "");
    os_unfair_lock_unlock(&L->lock);
}

static void set_error(SBLLink *L, const char *text)
{
    os_unfair_lock_lock(&L->lock);
    snprintf(L->st.last_error, sizeof L->st.last_error, "%s", text);
    os_unfair_lock_unlock(&L->lock);
}

/* ---- sbv callbacks (link queue) ------------------------------------------------------------------- */

static int tcp_send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static int usb_send(SBLLink *L, const uint8_t *p, size_t n);

static int cb_send(void *ctx, const uint8_t *p, size_t n)
{
    SBLLink *L = (__bridge SBLLink *)ctx;
    if (L->transport == TR_TCP && L->fd >= 0) return tcp_send_all(L->fd, p, n);
    if (L->transport == TR_USB) return usb_send(L, p, n);
    return -1;
}

static uint64_t cb_now(void *ctx) { (void)ctx; return mono_us(); }

static void cb_frame(void *ctx, const sbv_frame_info *fi)
{
    SBLLink *L = (__bridge SBLLink *)ctx;
    L->win_frames++;
    if (L->cb.frame) L->cb.frame(L->cb.ctx, fi);
}

static void cb_event(void *ctx, int ev, const char *text)
{
    SBLLink *L = (__bridge SBLLink *)ctx;
    if (ev == SBV_EV_PONG && !L->cfg.verbose) return;   /* once a second: too chatty */
    if (ev == SBV_EV_ERROR) set_error(L, text);
    if (ev == SBV_EV_HELLO) {
        set_conn(L, SBL_CONN_RUNNING);
        os_unfair_lock_lock(&L->lock);
        memcpy(L->st.peer, L->s.peer.name, 32);
        L->st.peer[32] = 0;
        L->st.peer_caps = L->s.peer.caps;
        L->st.refresh_mhz = L->s.peer.refresh_mhz;
        L->st.last_error[0] = 0;
        os_unfair_lock_unlock(&L->lock);
    }
    logf_(L, "%s", text);
}

/* ---- session end ------------------------------------------------------------------------------------ */

static void tcp_close(SBLLink *L, const char *why);
static void usb_recover(SBLLink *L, const char *why, double delay_s);
static void tcp_attempt(SBLLink *L);

static void session_ended(SBLLink *L, int err)
{
    logf_(L, "session ended: %s", sbv_strerror(err));
    if (L->transport == TR_TCP) tcp_close(L, sbv_strerror(err));
    else if (L->transport == TR_USB)
        usb_recover(L, sbv_strerror(err), (err == SBV_E_PEER_BYE || err == SBV_E_DEAD) ? RETRY_S : 0.2);
}

static void start_session(SBLLink *L)
{
    set_conn(L, SBL_CONN_HANDSHAKE);
    int e = sbv_start(&L->s);
    if (e < 0) session_ended(L, e);
}

/* ---- TCP ---------------------------------------------------------------------------------------------- */

static int want_tcp(SBLLink *L)
{
    return L->running && (L->cfg.mode == SBL_MODE_TCP || (L->cfg.mode == SBL_MODE_AUTO && !L->intf));
}

/* blocking connect with a timeout; returns fd or -1 */
static int tcp_connect(const char *host, uint16_t port, int timeout_ms, char *err, size_t errn)
{
    char ps[8];
    snprintf(ps, sizeof ps, "%u", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo(host, ps, &hints, &res);
    if (g) { snprintf(err, errn, "%s: %s", host, gai_strerror(g)); return -1; }
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            r = poll(&pf, 1, timeout_ms);
            if (r == 1) {
                int so = 0;
                socklen_t sl = sizeof so;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl);
                if (so) { errno = so; r = -1; } else r = 0;
            } else {
                if (r == 0) errno = ETIMEDOUT;
                r = -1;
            }
        }
        if (r == 0) {
            fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            break;
        }
        snprintf(err, errn, "%s:%u: %s", host, port, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        struct timeval tv = { 2, 0 };   /* a peer that stops reading must not hang the link queue */
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        int rb = 1 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    }
    return fd;
}

static void tcp_close(SBLLink *L, const char *why)
{
    if (L->transport != TR_TCP) return;
    logf_(L, "TCP closed: %s", why);
    L->transport = TR_NONE;
    L->tcp_gen++;
    L->fd = -1;
    SBLConn *c = L->conn;
    L->conn = nil;
    if (!c) return;
    /* Graceful: FIN after what we sent (a BYE), keep reading (and discarding) until the peer closes, so the
     * peer gets our BYE instead of a reset; after 500 ms cut it. The reader thread closes the fd. */
    os_unfair_lock_lock(&c->lk);
    if (!c->closed) shutdown(c->fd, SHUT_WR);
    os_unfair_lock_unlock(&c->lk);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        os_unfair_lock_lock(&c->lk);
        if (!c->closed) shutdown(c->fd, SHUT_RDWR);
        os_unfair_lock_unlock(&c->lk);
    });
    set_conn(L, L->running ? SBL_CONN_SEARCHING : SBL_CONN_IDLE);
}

static void tcp_attempt(SBLLink *L)
{
    if (!want_tcp(L) || L->tcp_connecting || L->transport != TR_NONE) return;
    L->tcp_connecting = 1;
    uint64_t gen = ++L->tcp_gen;
    NSString *hostS = @(L->cfg.host[0] ? L->cfg.host : "127.0.0.1");
    uint16_t port = L->cfg.port ? L->cfg.port : SBLK_TCP_PORT;
    dispatch_queue_t q = L->q;
    set_conn(L, SBL_CONN_SEARCHING);

    [NSThread detachNewThreadWithBlock:^{
        const char *host = hostS.UTF8String;
        char errb[200] = "";
        int fd = tcp_connect(host, port, 2000, errb, sizeof errb);
        NSString *errS = @(errb);
        __block int adopted = 0;
        SBLConn *c = [SBLConn new];
        c->fd = fd;
        c->lk = OS_UNFAIR_LOCK_INIT;
        c->done = dispatch_semaphore_create(0);
        dispatch_sync(q, ^{
            if (gen != L->tcp_gen) return;
            L->tcp_connecting = 0;
            if (fd < 0) {
                set_error(L, errS.UTF8String);
                return;
            }
            if (!want_tcp(L) || L->transport != TR_NONE) return;
            L->fd = fd;
            L->conn = c;
            L->transport = TR_TCP;
            snprintf(L->tdesc, sizeof L->tdesc, "TCP %s:%u", hostS.UTF8String, port);
            logf_(L, "TCP connected to %s:%u", hostS.UTF8String, port);
            adopted = 1;
            start_session(L);
        });
        if (!adopted) {
            if (fd >= 0) close(fd);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(RETRY_S * NSEC_PER_SEC)), q, ^{ tcp_attempt(L); });
            return;
        }
        uint8_t *buf = malloc(TCP_RX_BUF);
        for (;;) {
            ssize_t n = recv(fd, buf, TCP_RX_BUF, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                int en = errno;
                dispatch_sync(q, ^{
                    if (L->tcp_gen == gen && L->transport == TR_TCP) tcp_close(L, n == 0 ? "closed by the peer" : strerror(en));
                });
                break;
            }
            dispatch_sync(q, ^{
                if (L->tcp_gen != gen || L->transport != TR_TCP) return;   /* closing: drain until EOF */
                L->win_bytes += (uint64_t)n;
                int e = sbv_feed(&L->s, buf, (size_t)n);
                if (e < 0) session_ended(L, e);
            });
        }
        free(buf);
        os_unfair_lock_lock(&c->lk);
        close(fd);
        c->closed = 1;
        os_unfair_lock_unlock(&c->lk);
        dispatch_semaphore_signal(c->done);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(RETRY_S * NSEC_PER_SEC)), q, ^{ tcp_attempt(L); });
    }];
}

/* ---- USB ---------------------------------------------------------------------------------------------- */

static void usb_post_read(SBLLink *L, NSMutableData *d, uint64_t gen);

/* a read buffer: the interface's DMA-able memory when it gives exactly the size asked for */
static NSMutableData *usb_buf(SBLLink *L)
{
    NSError *err = nil;
    NSMutableData *d = [L->intf ioDataWithCapacity:USB_RX_BUF error:&err];
    if (!d || d.length != USB_RX_BUF) d = [NSMutableData dataWithLength:USB_RX_BUF];
    return d;
}

static int usb_send(SBLLink *L, const uint8_t *p, size_t n)
{
    IOUSBHostPipe *pout = L->pout;
    if (!pout) return -1;
    if (atomic_load(&L->usb_out_pending) > USB_TX_LIMIT) return -1;   /* the phone stopped reading */
    NSMutableData *d = [NSMutableData dataWithBytes:p length:n];
    uint64_t gen = L->usb_gen;
    NSError *err = nil;
    atomic_fetch_add(&L->usb_out_pending, (long)n);
    BOOL ok = [pout enqueueIORequestWithData:d completionTimeout:5.0 error:&err
                           completionHandler:^(IOReturn status, NSUInteger done) {
        (void)done;
        atomic_fetch_sub(&L->usb_out_pending, (long)n);
        if (status != kIOReturnSuccess && status != kIOReturnAborted && gen == L->usb_gen) {
            char why[64];
            snprintf(why, sizeof why, "OUT transfer failed (0x%08x)", status);
            usb_recover(L, why, RETRY_S);
        }
    }];
    if (!ok) {
        atomic_fetch_sub(&L->usb_out_pending, (long)n);
        logf_(L, "USB OUT enqueue: %s", err.localizedDescription.UTF8String);
        return -1;
    }
    /* a transfer of an exact multiple of 512 ends with a zero-length packet (PROTOCOL.md 6.2) */
    if (n % SBLK_USB_MPS == 0) {
        ok = [pout enqueueIORequestWithData:nil completionTimeout:5.0 error:&err
                          completionHandler:^(IOReturn status, NSUInteger done) { (void)status; (void)done; }];
        if (!ok) return -1;
    }
    return 0;
}

static void usb_post_read(SBLLink *L, NSMutableData *d, uint64_t gen)
{
    if (gen != L->usb_gen || !L->pin) return;
    NSError *err = nil;
    BOOL ok = [L->pin enqueueIORequestWithData:d completionTimeout:0 error:&err
                             completionHandler:^(IOReturn status, NSUInteger n) {
        if (gen != L->usb_gen) return;                      /* an older session's read */
        if (status == kIOReturnAborted) return;
        if (status != kIOReturnSuccess) {
            char why[64];
            snprintf(why, sizeof why, "IN transfer failed (0x%08x)", status);
            usb_recover(L, why, RETRY_S);
            return;
        }
        if (n && L->usb_hunt) {
            /* the phone may still finish a frame of the session we just left (it was sending when this
             * app died or restarted); its HELLO answer always starts an IN transfer of its own, so skip
             * whole transfers until one starts with HELLO (or BYE) instead of failing on "message before
             * HELLO" and restarting again (PROTOCOL.md 6.3) */
            const uint8_t *b = d.bytes;
            int start = n >= SBLK_HDR_SIZE && b[0] == 'S' && b[1] == 'B' && b[2] == 'L' && b[3] == 'K' &&
                        b[4] == SBLK_VERSION && (b[5] == SBLK_T_HELLO || b[5] == SBLK_T_BYE);
            if (!start) {
                L->usb_skipped += n;
                usb_post_read(L, d, gen);
                return;
            }
            if (L->usb_skipped)
                logf_(L, "USB: skipped %llu bytes of the previous session before the phone's HELLO",
                      (unsigned long long)L->usb_skipped);
            L->usb_hunt = 0;
        }
        if (n) {
            L->win_bytes += n;
            int e = sbv_feed(&L->s, d.bytes, n);
            if (e < 0) { session_ended(L, e); return; }
        }
        usb_post_read(L, d, gen);
    }];
    if (!ok) {
        logf_(L, "USB IN enqueue: %s", err.localizedDescription.UTF8String);
        usb_recover(L, "IN enqueue failed", RETRY_S);
    }
}

/* PROTOCOL.md 6.3: drain IN (short reads until nothing comes), then HELLO as its own transfer, then keep
 * USB_RX_QUEUED reads posted. Asynchronous only: the interface's I/O queue is the link queue. */
static void usb_drain_then_start(SBLLink *L, NSMutableData *d, uint64_t gen, int left)
{
    if (gen != L->usb_gen || !L->pin) return;
    NSError *err = nil;
    BOOL ok = [L->pin enqueueIORequestWithData:d completionTimeout:0.05 error:&err
                             completionHandler:^(IOReturn status, NSUInteger n) {
        if (gen != L->usb_gen) return;
        if (status == kIOReturnSuccess && n > 0 && left > 0) {
            usb_drain_then_start(L, d, gen, left - 1);
            return;
        }
        start_session(L);
        if (gen != L->usb_gen || L->transport != TR_USB) return;
        usb_post_read(L, d, gen);
        for (int i = 1; i < USB_RX_QUEUED; i++) {
            usb_post_read(L, usb_buf(L), gen);
        }
    }];
    if (!ok) {
        logf_(L, "USB drain: %s", err.localizedDescription.UTF8String);
        usb_recover(L, "drain failed", RETRY_S);
    }
}

static void usb_session_start(SBLLink *L)
{
    uint64_t gen = L->usb_gen;
    NSMutableData *d = usb_buf(L);
    set_conn(L, SBL_CONN_HANDSHAKE);
    L->usb_hunt = 1;
    L->usb_skipped = 0;
    usb_drain_then_start(L, d, gen, 256);
}

/* PROTOCOL.md 6.4: abort, clear halt on both pipes (resets the toggles), drain, HELLO again */
static void usb_recover(SBLLink *L, const char *why, double delay_s)
{
    if (L->transport != TR_USB) return;
    uint64_t t = mono_us();
    if (t - L->last_recover_us < 3000000u && delay_s < RETRY_S) delay_s = RETRY_S;   /* no tight loops */
    L->last_recover_us = t;
    logf_(L, "USB session restart: %s", why);
    set_error(L, why);
    if (L->s.state == SBV_RUNNING || L->s.state == SBV_HELLO_SENT) sbv_bye(&L->s, SBLK_BYE_PROTOCOL, why);
    uint64_t gen = ++L->usb_gen;
    [L->pin abortWithOption:IOUSBHostAbortOptionAsynchronous error:nil];
    [L->pout abortWithOption:IOUSBHostAbortOptionAsynchronous error:nil];
    set_conn(L, SBL_CONN_SEARCHING);
    IOUSBHostPipe *pin = L->pin, *pout = L->pout;
    dispatch_queue_t q = L->q;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay_s * NSEC_PER_SEC)),
                   dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        /* synchronous control requests: off the link queue */
        [pin clearStallWithError:nil];
        [pout clearStallWithError:nil];
        dispatch_async(q, ^{
            if (gen != L->usb_gen || L->transport != TR_USB || !L->running) return;
            usb_session_start(L);
        });
    });
}

static void usb_close(SBLLink *L, const char *why)
{
    if (!L->intf) return;
    logf_(L, "USB closed: %s", why);
    L->usb_gen++;
    if (L->transport == TR_USB) {
        L->transport = TR_NONE;
        L->s.state = SBV_CLOSED;
    }
    [L->intf destroy];
    L->intf = nil;
    L->pin = L->pout = nil;
    set_conn(L, L->running ? SBL_CONN_SEARCHING : SBL_CONN_IDLE);
}

static void usb_interface_appeared(SBLLink *L, io_service_t svc)
{
    if (!L->running || L->cfg.mode == SBL_MODE_TCP || L->intf) { IOObjectRelease(svc); return; }
    NSError *err = nil;
    dispatch_queue_t q = L->q;
    __weak SBLLink *weak = L;
    IOUSBHostInterface *intf = [[IOUSBHostInterface alloc] initWithIOService:svc
        options:IOUSBHostObjectInitOptionsNone queue:q error:&err
        interestHandler:^(IOUSBHostObject *obj, uint32_t type, void *arg) {
            (void)arg;
            if (type != kIOMessageServiceIsTerminated) return;
            dispatch_async(q, ^{
                SBLLink *S = weak;
                if (S && S->intf == obj) {
                    usb_close(S, "device unplugged");
                    tcp_attempt(S);
                }
            });
        }];
    if (!intf) {
        logf_(L, "USB: cannot open the Starbite Link interface: %s", err.localizedDescription.UTF8String);
        set_error(L, "USB interface busy or not allowed");
        IOObjectRelease(svc);
        return;
    }
    NSError *e1 = nil, *e2 = nil;
    IOUSBHostPipe *pin = [intf copyPipeWithAddress:SBLK_USB_EP_IN error:&e1];
    IOUSBHostPipe *pout = [intf copyPipeWithAddress:SBLK_USB_EP_OUT error:&e2];
    if (!pin || !pout) {
        logf_(L, "USB: endpoints 0x81/0x01 missing (%s)", (e1 ?: e2).localizedDescription.UTF8String);
        [intf destroy];
        return;
    }
    [intf setIdleTimeout:0 error:nil];
    if (L->transport == TR_TCP) {
        sbv_bye(&L->s, SBLK_BYE_NORMAL, "switching to USB");
        tcp_close(L, "switching to USB");
    }
    L->intf = intf;
    L->pin = pin;
    L->pout = pout;
    L->transport = TR_USB;
    L->usb_gen++;
    snprintf(L->tdesc, sizeof L->tdesc, "USB %04x:%04x", SBLK_USB_VID, SBLK_USB_PID);
    logf_(L, "USB: Starbite Link interface opened");
    usb_session_start(L);
}

static void if_matched(void *refcon, io_iterator_t it)
{
    SBLLink *L = (__bridge SBLLink *)refcon;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) usb_interface_appeared(L, svc);
}

static void dev_matched(void *refcon, io_iterator_t it)
{
    SBLLink *L = (__bridge SBLLink *)refcon;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        L->usb_present++;
        logf_(L, "USB: Starbite Link device attached");
        /* A device whose configuration nobody set has no interfaces yet: set configuration 1 ourselves if
         * no interface showed up after 2 s. */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), L->q, ^{
            if (L->intf || L->dev || !L->running || L->cfg.mode == SBL_MODE_TCP) { IOObjectRelease(svc); return; }
            NSError *err = nil;
            IOUSBHostDevice *d = [[IOUSBHostDevice alloc] initWithIOService:svc
                options:IOUSBHostObjectInitOptionsNone queue:L->q error:&err interestHandler:nil];
            if (!d) { IOObjectRelease(svc); return; }   /* someone (the composite driver) owns it: fine */
            if (![d configureWithValue:1 matchInterfaces:YES error:&err]) {
                logf_(L, "USB: set configuration 1: %s", err.localizedDescription.UTF8String);
                [d destroy];
                return;
            }
            logf_(L, "USB: configuration 1 set");
            L->dev = d;
        });
    }
}

static void dev_terminated(void *refcon, io_iterator_t it)
{
    SBLLink *L = (__bridge SBLLink *)refcon;
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        if (L->usb_present > 0) L->usb_present--;
        if (L->dev && L->dev.ioService == svc) {
            [L->dev destroy];
            L->dev = nil;
        }
        IOObjectRelease(svc);
    }
}

static void usb_setup(SBLLink *L)
{
    if (L->nport) return;
    L->nport = IONotificationPortCreate(kIOMainPortDefault);
    IONotificationPortSetDispatchQueue(L->nport, L->q);
    NSNumber *vid = @(SBLK_USB_VID), *pid = @(SBLK_USB_PID);
    CFMutableDictionaryRef d = [IOUSBHostInterface createMatchingDictionaryWithVendorID:vid productID:pid
        bcdDevice:nil interfaceNumber:@0 configurationValue:@1 interfaceClass:nil interfaceSubclass:nil
        interfaceProtocol:nil speed:nil productIDArray:nil];
    kern_return_t kr = IOServiceAddMatchingNotification(L->nport, kIOFirstMatchNotification, d, if_matched,
                                                        (__bridge void *)L, &L->it_if);
    if (kr == KERN_SUCCESS) if_matched((__bridge void *)L, L->it_if);
    else logf_(L, "USB: interface matching failed (0x%08x)", kr);

    d = [IOUSBHostDevice createMatchingDictionaryWithVendorID:vid productID:pid bcdDevice:nil deviceClass:nil
        deviceSubclass:nil deviceProtocol:nil speed:nil productIDArray:nil];
    kr = IOServiceAddMatchingNotification(L->nport, kIOFirstMatchNotification, d, dev_matched,
                                          (__bridge void *)L, &L->it_dev);
    if (kr == KERN_SUCCESS) dev_matched((__bridge void *)L, L->it_dev);
    d = [IOUSBHostDevice createMatchingDictionaryWithVendorID:vid productID:pid bcdDevice:nil deviceClass:nil
        deviceSubclass:nil deviceProtocol:nil speed:nil productIDArray:nil];
    kr = IOServiceAddMatchingNotification(L->nport, kIOTerminatedNotification, d, dev_terminated,
                                          (__bridge void *)L, &L->it_dev_gone);
    if (kr == KERN_SUCCESS) dev_terminated((__bridge void *)L, L->it_dev_gone);
}

/* ---- timer, status ------------------------------------------------------------------------------------ */

static void tick(SBLLink *L)
{
    if (L->transport != TR_NONE) {
        int e = sbv_tick(&L->s);
        if (e < 0) session_ended(L, e);
    }
    uint64_t t = mono_us();
    if (!L->win_t) { L->win_t = t; L->win_frames = L->win_bytes = 0; }   /* first window starts now */
    double dt = (double)(t - L->win_t) / 1e6;
    os_unfair_lock_lock(&L->lock);
    if (dt >= 1.0) {
        L->st.fps = (double)L->win_frames / dt;
        L->st.mbps = (double)L->win_bytes * 8.0 / dt / 1e6;
        L->win_frames = L->win_bytes = 0;
        L->win_t = t;
    }
    L->st.width = L->s.fb_w;
    L->st.height = L->s.fb_h;
    L->st.rtt_ms = (double)L->s.rtt_us / 1000.0;
    L->st.lag_ms = (double)L->s.lag_us / 1000.0;
    L->st.frames = L->s.frames;
    L->st.sessions = L->s.sessions;
    L->st.pongs = L->s.pongs;
    L->st.rx_bytes = L->s.rx_bytes;
    L->st.usb_present = L->usb_present;
    if (L->transport == TR_NONE) { L->st.fps = 0; L->st.mbps = 0; }
    os_unfair_lock_unlock(&L->lock);
}

@implementation SBLLink
@end

/* ---- C API -------------------------------------------------------------------------------------------- */

sbl_link *sbl_link_create(const sbl_config *cfg, const sbl_callbacks *cbs)
{
    SBLLink *L = [SBLLink new];
    L->cfg = *cfg;
    if (cbs) L->cb = *cbs;
    L->fd = -1;
    L->lock = OS_UNFAIR_LOCK_INIT;
    L->q = dispatch_queue_create("starbite.link", dispatch_queue_attr_make_with_qos_class(
                                     DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    sbv_events ev = { (__bridge void *)L, cb_send, cb_now, cb_frame, cb_event };
    if (sbv_init(&L->s, &ev, cfg->name, cfg->pref_w, cfg->pref_h, cfg->refresh_mhz) < 0) return NULL;
    return (sbl_link *)CFBridgingRetain(L);
}

static SBLLink *LL(sbl_link *l) { return (__bridge SBLLink *)(void *)l; }

void sbl_link_start(sbl_link *l)
{
    SBLLink *L = LL(l);
    dispatch_async(L->q, ^{
        if (L->running) return;
        L->running = 1;
        set_conn(L, SBL_CONN_SEARCHING);
        if (L->cfg.mode != SBL_MODE_TCP) usb_setup(L);
        if (!L->timer) {
            L->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, L->q);
            dispatch_source_set_timer(L->timer, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC),
                                      250 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(L->timer, ^{ tick(L); });
            dispatch_resume(L->timer);
        }
        tcp_attempt(L);
    });
}

void sbl_link_stop(sbl_link *l, uint32_t reason)
{
    SBLLink *L = LL(l);
    __block int usb = 0;
    __block SBLConn *c = nil;
    dispatch_sync(L->q, ^{
        L->running = 0;
        sbv_bye(&L->s, reason, reason == SBLK_BYE_SHUTDOWN ? "viewer quit" : NULL);
        usb = L->transport == TR_USB;
        c = L->conn;
        tcp_close(L, "stopped");
    });
    if (c) dispatch_semaphore_wait(c->done, dispatch_time(DISPATCH_TIME_NOW, 700 * NSEC_PER_MSEC));
    for (int i = 0; usb && i < 30 && atomic_load(&L->usb_out_pending) > 0; i++) usleep(10000);
    dispatch_sync(L->q, ^{
        usb_close(L, "stopped");
        set_conn(L, SBL_CONN_IDLE);
    });
}

void sbl_link_destroy(sbl_link *l)
{
    if (!l) return;
    sbl_link_stop(l, SBLK_BYE_SHUTDOWN);
    SBLLink *L = (SBLLink *)CFBridgingRelease(l);
    dispatch_sync(L->q, ^{
        if (L->timer) dispatch_source_cancel(L->timer);
        if (L->dev) { [L->dev destroy]; L->dev = nil; }
        io_iterator_t its[3] = { L->it_if, L->it_dev, L->it_dev_gone };
        for (int i = 0; i < 3; i++) if (its[i]) IOObjectRelease(its[i]);
        L->it_if = L->it_dev = L->it_dev_gone = 0;
        if (L->nport) IONotificationPortDestroy(L->nport);
        L->nport = NULL;
        sbv_free(&L->s);
    });
}

void sbl_link_status(sbl_link *l, sbl_status *out)
{
    SBLLink *L = LL(l);
    os_unfair_lock_lock(&L->lock);
    *out = L->st;
    os_unfair_lock_unlock(&L->lock);
}

#define ON_LINK(body) do { SBLLink *L = LL(l); dispatch_async(L->q, ^{ int e_ = (body); \
    if (e_ < 0 && L->transport != TR_NONE) session_ended(L, e_); }); } while (0)

void sbl_link_key(sbl_link *l, uint16_t page, uint16_t usage, int down, uint16_t locks)
{
    ON_LINK(sbv_key(&L->s, page, usage, down, locks));
}

void sbl_link_pointer(sbl_link *l, int32_t x, int32_t y, uint32_t buttons, int16_t wdx, int16_t wdy)
{
    ON_LINK(sbv_pointer(&L->s, x, y, buttons, wdx, wdy));
}

void sbl_link_text(sbl_link *l, const char *utf8)
{
    NSData *copy = [NSData dataWithBytes:utf8 length:strlen(utf8)];
    ON_LINK(sbv_text(&L->s, copy.bytes, copy.length));
}

void sbl_link_release_all(sbl_link *l, uint16_t locks) { ON_LINK(sbv_release_all(&L->s, locks)); }
void sbl_link_request_mode(sbl_link *l, uint16_t w, uint16_t h) { ON_LINK(sbv_request_mode(&L->s, w, h, 0)); }
void sbl_link_ping(sbl_link *l) { ON_LINK(sbv_ping(&L->s)); }
void sbl_link_request_keyframe(sbl_link *l) { ON_LINK(sbv_ack_keyframe(&L->s)); }

void sbl_link_flush(sbl_link *l)
{
    SBLLink *L = LL(l);
    dispatch_sync(L->q, ^{});
}
