/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS Wi-Fi (docs/STARBITE-OS.md 9.4), 0.4: the BCM4361 through the portable core starbite/wifi (bcmw, the
 * wifi-core session's) and its IPv4 layer (sbnet): the radio on and off at the user's word, a scan, joining an
 * OPEN network, DHCP, the gateway pinged. Secured networks are listed with a lock (their passphrase entry comes
 * later).
 *
 * The loop is not blocked for long except by the bring-up: it runs one step per tick, but step 8 reads the
 * firmware from VENDOR (the first time) and waits for it to start (up to 5 s), steps 9 and 10 send commands that
 * may each wait up to 2 s; the UI stalls meanwhile (the heater is never on then). Scans and joins are the
 * non-blocking calls (bcmw_scan_start, bcmw_connect) with events; the first transmitted frame waits for the tx
 * flow ring (up to 2 s, once). Heating and Wi-Fi do not run together in 0.4: the
 * heater's 250 ms safety ticks must never wait behind the radio (each refuses while the other is on).
 *
 * Writes (on the phone): only what bcmw_write_allowed() lists (PCIe channel 0, the chip's windows, single words of
 * the PMU, CMU_FSYS1, SYSREG_FSYS1, three GPIO banks); the radio's files are read from VENDOR, never written.
 * Logged: states, counts and times only: no network names, no addresses (the screen shows them, the pmsg ring
 * does not). Off: bcmw_quiesce (the chip in reset and unpowered).
 *
 * Next (wifi-sbos-next): TCP (sbtcp), HTTPS (sbhttp on BearSSL, Mozilla's CA roots: starbite/wifi/tls) for one fetch
 * at a time (wifi_fetch), its TLS randomness from an entropy pool (see ent_*), and a scan-only self test with the
 * offline TLS self-check. The fetch's URL and the body are never logged; the log has steps, sizes and times. */
#include "sb.h"
#include "kern.h"
#include "sbapp.h"
#include "bcmw.h"
#include "bcmw_priv.h"                      /* w->dead: the chip stopped answering */
#include "net.h"
#include "tcp.h"
#include "netio.h"
#include "http.h"
#include "tls/tls_check.h"
#include "store.h"
#ifdef SB_HOST
#include <stdlib.h>
#include "host/model.h"
#else
#include "sb/bcmw_sb.h"
#endif

#define TICK_ON_MS 20U                      /* the core's poll while the screen is on */
#define TICK_OFF_MS 200U                    /* ... and off */
#define SCAN_MS 8000U
#define JOIN_MS 15000U
#define DHCP_MS 20000U

static struct bcmw *w;
static struct bcmw_os os;
static struct bcmw_cfg cfg;
static struct sbnet net;
static int st = SBOS_WIFI_OFF, step;
static uint64_t t_state, t_on;
static char why[64];
static struct sbos_wifi_net nets[SBOS_WIFI_NETS];
static int nnets, scan_heard;
static uint16_t sync;
static int joined = -1;
static int pinged;
/* 0.5.8: the time over the network (NTP, pool.ntp.org) once joined: 0 idle, 1 DNS, 2 asked, 3 done (checked),
 * -1 no answer */
#define NTP_PORT 50123
static int ntp_st, clock_checked;
static int32_t clock_off;
static uint64_t ntp_t;
static uint8_t ntp_nonce[8], ntp_server[4];
/* the saved network (0.4.2): the store entry "wifinet", two lines (name, WPA2 passphrase or empty), made on the
 * Mac from the user's input for the Wi-Fi tests; read only, never logged, never shown (the passphrase) */
static uint8_t sv_ssid[33];
static unsigned sv_len;
static char sv_pass[65];
static int sv_ok, join_pending, join_active;   /* 0.6.1: join_active: the handshake runs, one round per tick */
static uint64_t join_t0;
/* 0.5: networks joined with a typed passphrase this session (RAM only: gone at a restart; never logged) */
#define REMEMBER 4
static struct { uint8_t ssid[33]; unsigned len; char pass[65]; } rem[REMEMBER];
static unsigned nrem;
static char jp_pass[65];                    /* the passphrase of the pending join */

static const char *known_pass(const uint8_t *ssid, unsigned len)
{
    for (unsigned i = 0; i < nrem; ++i)
        if (rem[i].len == len && !memcmp(rem[i].ssid, ssid, len))
            return rem[i].pass;
    if (sv_ok && sv_len == len && !memcmp(sv_ssid, ssid, len))
        return sv_pass;
    return 0;
}

/* 1.2-alpha21: join a known network the scan heard, once per power-on (Wi-Fi > Known networks; on by default) */
int wifi_autojoin = 1;
static int auto_pending, auto_done;
static int wifi_in_selftest(void)
{
#ifdef SB_HOST
    if (getenv("SBOS_HOST_SELFTEST"))
        return 1;
#endif
    return sbos_selftest_enabled;           /* the self test's Wi-Fi step joins by itself */
}

static void remember(const uint8_t *ssid, unsigned len, const char *pass)
{
    unsigned i = 0;
    while (i < nrem && !(rem[i].len == len && !memcmp(rem[i].ssid, ssid, len)))
        ++i;
    if (i == nrem) {
        if (nrem < REMEMBER)
            ++nrem;
        else
            i = REMEMBER - 1;               /* full: the last one goes */
    }
    memcpy(rem[i].ssid, ssid, len);
    rem[i].len = len;
    sb_strcpy(rem[i].pass, sizeof(rem[i].pass), pass);
}
static int w_ready;
/* 0.5: the chip's INTx (SPI 253 = INTID 285); polling stays as the fallback and relaxes once interrupts arrive */
#define WIFI_INTID 285U
static int irq_on, irq_kicked, irq_trusted;
static int scan_retry;                      /* a refused scan: tries left */
static uint64_t scan_retry_at;
static unsigned irq_n, irq_window_n, irq_storms, irq_missed;
static uint64_t irq_window_t;
static void irq_stop(void);                         /* bcmw_init succeeded on w (quiesce and leave only then) */
static int dhcp_go;                         /* the link came up inside a callback: DHCP starts from the tick */
static void net_tcp(void *ctx, const uint8_t from[4], const uint8_t *seg, uint32_t len);
static void net_down(void);                 /* Next: sbnet stopped and every TCP connection ended */
static void ent_tick(void);
static void ent_add(const void *p, uint32_t n);
static void fetch_poll(void);
static int fx_state;

/* ---- DMA memory reused across on / off (the review of 0.4: bcmw_init forgets the core's blocks, and the shim's
 * allocator never frees): the core asks for the same sizes in the same order every bring-up, so the n-th request
 * of a power-on gets the n-th block of the first one (zeroed); a request that does not match gets a new block. */
#define DMA_KEEP 48
static struct { uint32_t size, align; void *p; uint64_t bus; } dk[DMA_KEEP];
static unsigned ndk, dk_next, dk_new, dk_reused;
static void *(*dma_first)(void *ctx, uint32_t size, uint32_t align, uint64_t *bus);

static void *dma_reuse(void *ctx, uint32_t size, uint32_t align, uint64_t *bus)
{
    if (dk_next < ndk && dk[dk_next].size == size && dk[dk_next].align == align) {
        memset(dk[dk_next].p, 0, size);
        ++dk_reused;
        *bus = dk[dk_next].bus;
        return dk[dk_next++].p;
    }
    void *p = dma_first(ctx, size, align, bus);
    ++dk_new;
    if (p && dk_next < DMA_KEEP) {
        dk[dk_next].size = size;
        dk[dk_next].align = align;
        dk[dk_next].p = p;
        dk[dk_next].bus = *bus;
        ++dk_next;
        if (ndk < dk_next)
            ndk = dk_next;
    }
    return p;
}

int wifi_on(void)
{
    return st != SBOS_WIFI_OFF && st != SBOS_WIFI_FAILED;
}

static void notify(void)
{
    ui_mark_dirty();
    int a = app_by_id("wifi");
    if (a >= 0)
        app_event_send(a, EV_APP_SERVICE, 3, st);
}

static void set_state(int s, const char *w_)
{
    st = s;
    t_state = sb_ms();
    sb_strcpy(why, sizeof(why), w_ ? w_ : "");
    notify();
}

static void logn(const char *a, uint64_t n, const char *b)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, a);
    ub_dec(&u, n);
    ub_s(&u, b);
    sb_log(u.b);
}

/* ---- the core's callbacks ------------------------------------------------------------------------------- */

static void os_log(void *ctx, const char *line)
{
    (void)ctx;
    /* the firmware's console ("fw: <text>") stays out of the pmsg ring; the core's own step errors ("fw: 8: ...")
     * go in */
    if (sb_prefix(line, "fw: ") && !(line[4] >= '0' && line[4] <= '9' && line[5] == ':'))
        return;
    sb_logf("wifi: ", line);
}

static void load_saved(void)
{
    static uint8_t buf[4096] __attribute__((aligned(4096)));
    unsigned i = 0, n = 0;
    sv_ok = 0;
#ifdef SB_HOST
    unsigned len = 0;
    const char *h = getenv("SBOS_HOST_WIFINET");    /* "name\npassphrase" */
    if (!h)
        return;
    while (h[len] && len + 1 < sizeof(buf))
        buf[len] = (uint8_t)h[len], ++len;
    buf[len] = 0;
#else
    struct store_ent e;
    if (!store_find("ufs:wifinet", &e) || !e.size || e.size >= sizeof(buf) || store_load(&e, (uintptr_t)buf, sizeof(buf)))
        return;
    buf[e.size] = 0;
#endif
    while (buf[i] && buf[i] != '\n' && buf[i] != '\r' && n < 32)
        sv_ssid[n++] = buf[i++];
    sv_len = n;
    while (buf[i] == '\r' || buf[i] == '\n')
        ++i;
    n = 0;
    while (buf[i] && buf[i] != '\n' && buf[i] != '\r' && n < 64)
        sv_pass[n++] = (char)buf[i++];
    sv_pass[n] = 0;
    memset(buf, 0, sizeof(buf));
    sv_ok = sv_len > 0 && (n == 0 || (n >= 8 && n <= 64));
    sb_log(sv_ok ? "wifi: a saved network is set (name and passphrase not logged)" :
                   "wifi: the saved network entry is not usable (name, or a passphrase of 8..64 characters)");
}

static void add_bss(const struct bcmw_bss *b)
{
    if (!b->ssid_len)
        return;                             /* hidden networks are not listed */
    int secure = (b->capability & 0x0010) != 0;
    for (int i = 0; i < nnets; ++i)
        if (nets[i].ssid_len == b->ssid_len && !memcmp(nets[i].ssid, b->ssid, b->ssid_len)) {
            if (b->rssi > nets[i].rssi) {   /* the strongest access point of a name */
                nets[i].rssi = b->rssi;
                memcpy(nets[i].bssid, b->bssid, 6);
                nets[i].channel = (int32_t)b->channel;
            }
            return;
        }
    int at = nnets;
    if (nnets == SBOS_WIFI_NETS) {          /* full: replace the weakest if this one is stronger */
        at = 0;
        for (int i = 1; i < nnets; ++i)
            if (nets[i].rssi < nets[at].rssi)
                at = i;
        if (nets[at].rssi >= b->rssi)
            return;
    } else {
        ++nnets;
    }
    struct sbos_wifi_net *n = &nets[at];
    memset(n, 0, sizeof(*n));
    memcpy(n->ssid, b->ssid, b->ssid_len);
    n->ssid_len = b->ssid_len;
    memcpy(n->bssid, b->bssid, 6);
    n->rssi = b->rssi;
    n->secure = secure;
    n->saved = secure && known_pass(b->ssid, b->ssid_len) != 0;
    n->channel = (int32_t)b->channel;
    n->band5 = (int32_t)b->band5;
}

static void sort_nets(void)
{
    for (int i = 1; i < nnets; ++i)         /* strongest first */
        for (int j = i; j > 0 && nets[j].rssi > nets[j - 1].rssi; --j) {
            struct sbos_wifi_net t = nets[j];
            nets[j] = nets[j - 1];
            nets[j - 1] = t;
        }
}

static void os_event(void *ctx, const struct bcmw_event *ev)
{
    (void)ctx;
    struct bcmw_bss b;
    uint16_t s = 0;
    ent_tick();
    ent_add(ev, sizeof(*ev));
    if (ev->type == BCMW_E_ESCAN_RESULT) {
        if (bcmw_bss_parse(ev, &b, &s)) {
            if (st != SBOS_WIFI_SCANNING || s != sync)
                return;                     /* a late result or another scan's: the list stays as shown */
            ++scan_heard;
            add_bss(&b);
        } else if (st == SBOS_WIFI_SCANNING && bcmw_scan_ended(ev, sync)) {
            sort_nets();
            logn("wifi: the scan heard ", (uint64_t)nnets, " networks (names not logged)");
            set_state(SBOS_WIFI_UP, "");
            auto_pending = wifi_autojoin && !auto_done && joined < 0 && !wifi_in_selftest();
        }
        return;
    }
    if (st != SBOS_WIFI_JOINING && st != SBOS_WIFI_DHCP && st != SBOS_WIFI_CONNECTED)
        return;
    if (ev->type == BCMW_E_SET_SSID && ev->status) {
        sb_logx("wifi: the join was refused, status ", ev->status);
        set_state(SBOS_WIFI_UP, "The network did not let the phone join");
        joined = -1;
    } else if (ev->type == BCMW_E_LINK) {
        if (ev->flags & 1) {
            if (st == SBOS_WIFI_JOINING && (join_pending || join_active)) {
                /* 0.6.1: a secured join: associated is not joined; the address only after the handshake (0.6 asked
                 * for it here, before the keys) */
                logn("wifi: associated after ", sb_ms() - t_state, " ms; the handshake next");
            } else if (st == SBOS_WIFI_JOINING) {
                logn("wifi: the link is up after ", sb_ms() - t_state, " ms; asking for an address (DHCP)");
                set_state(SBOS_WIFI_DHCP, "");
                dhcp_go = 1;                /* not from inside bcmw_poll: the first frame opens the tx flow ring */
            }
        } else {
            sb_log("wifi: the link went down");
            net_down();
            set_state(SBOS_WIFI_UP, "The network dropped the phone");
            joined = -1;
        }
    } else if (ev->type == BCMW_E_DEAUTH || ev->type == BCMW_E_DEAUTH_IND || ev->type == BCMW_E_DISASSOC_IND) {
        sb_log("wifi: left by the access point");
        net_down();
        set_state(SBOS_WIFI_UP, "The network dropped the phone");
        joined = -1;
    }
}

static void os_rx(void *ctx, unsigned ifidx, const uint8_t *frame, uint32_t len)
{
    (void)ctx;
    (void)ifidx;
    ent_tick();
    ent_add(frame, len < 64 ? len : 64);
    sbnet_rx(&net, frame, len);
}

static int net_tx(void *ctx, const uint8_t *frame, uint32_t len)
{
    (void)ctx;
    return bcmw_tx(w, frame, len) == BCMW_OK ? 0 : -1;
}

static uint64_t net_now(void *ctx)
{
    (void)ctx;
    return sb_ms();
}

static uint32_t net_random(void *ctx)
{
    (void)ctx;
    static uint64_t x = 0x9e3779b97f4a7c15ULL;
    x ^= sb_ticks() + (x << 13);
    x ^= x >> 7;
    x ^= x << 17;
    return (uint32_t)x;
}

static uint32_t ntp_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* sbnet's UDP for our ports: the NTP answer */
static void net_udp(void *ctx, uint16_t port, const uint8_t from[4], uint16_t from_port, const uint8_t *d, uint32_t len)
{
    (void)ctx;
    (void)from;
    if (port != NTP_PORT || from_port != 123 || ntp_st != 2 || len < 48 || (d[0] & 7) != 4)
        return;
    if (memcmp(from, ntp_server, 4) || memcmp(d + 24, ntp_nonce, 8))
        return;                             /* not from the server asked, or not an answer to our request */
    if ((d[0] >> 6) == 3 || d[1] == 0 || d[1] >= 16)
        return;                             /* the server is not synchronised */
    uint32_t secs = ntp_be32(d + 40);           /* the transmit time, seconds since 1900 */
    if (secs < 2208988800U + 1577836800U)   /* before 2020: not believed */
        return;
    uint64_t rtt = sb_ms() - ntp_t;
    int64_t utc = (int64_t)(secs - 2208988800U) + (int64_t)((rtt / 2 + 500) / 1000U);
    int was_set = 0;
    int64_t d_s = clock_set_net(utc, &was_set);
    if (d_s > 2000000000LL)
        d_s = 2000000000LL;
    if (d_s < -2000000000LL)
        d_s = -2000000000LL;
    clock_off = (int32_t)d_s;
    clock_checked = was_set ? 3 : d_s ? 2 : 1;
    ntp_st = 3;
    if (was_set)
        sb_log("wifi: the clock was set from the network (there was none)");
    else if (d_s)
        logn("wifi: the clock was checked over the network and corrected by ", (uint64_t)(d_s < 0 ? -d_s : d_s),
             d_s < 0 ? " s (it was ahead)" : " s (it was behind)");
    else
        sb_log("wifi: the clock was checked over the network: right");
    notify();
}

static void net_log(void *ctx, const char *line)
{
    (void)ctx;
    sb_logf("wifi: ", line);
}

/* ---- TCP, the entropy pool, HTTPS (Next) ------------------------------------------------------------------ */

static struct sbtcp tcp;
static struct sbnetio nio;

static void net_tcp(void *ctx, const uint8_t from[4], const uint8_t *seg, uint32_t len)
{
    (void)ctx;
    sbtcp_input(&tcp, from, seg, len);
}

static void net_down(void)
{
    sbnet_stop(&net);
    sbtcp_down(&tcp);
}

/* The entropy pool for TLS's randomness (ECDHE keys, nonces). The phone gives this kernel no hardware random source
 * (the Exynos TRNG is the secure world's), so a SHA-256 pool takes everything hard to predict from outside: the
 * cycle counter at every radio interrupt, frame and event (their arrival times depend on the air and the chip),
 * the frames' and events' first bytes, timing jitter of memory walks when a seed is drawn, and the radio
 * firmware's own random source when it answers the "rand" iovar. Each seed is the pool's hash, then fed back in.
 * Not a certified generator: good enough that a passive listener cannot reproduce the TLS keys. */
static br_sha256_context ent;
static int ent_ready, fw_rand_logged;
static uint64_t ent_events;

static void ent_add(const void *p, uint32_t n)
{
    if (!ent_ready) {
        br_sha256_init(&ent);
        ent_ready = 1;
    }
    br_sha256_update(&ent, p, n);
    ent_events++;
}
static void ent_tick(void)
{
    uint64_t t = sb_ticks();
    ent_add(&t, sizeof(t));
}

static void ent_seed(uint8_t out[32])
{
    static volatile uint8_t walk[8192];
    uint8_t jit[256];
    memset(jit, 0, sizeof(jit));
    for (unsigned i = 0; i < 4096; i++) {
        uint64_t a = sb_ticks();
        for (unsigned k = 0; k < 48; k++)
            walk[(i * 67u + k * 1031u) & 8191u] += (uint8_t)(a >> (k & 7));
        jit[i & 255] ^= (uint8_t)(sb_ticks() - a) ^ (uint8_t)((sb_ticks() - a) >> 3);
        if ((i & 255) == 255)
            ent_add(jit, sizeof(jit));
    }
    if (w_ready && w && !w->dead) {
        uint8_t fr[32];
        int fe = 0;
        memset(fr, 0, sizeof(fr));
        int r = bcmw_iovar_get(w, "rand", fr, sizeof(fr), &fe);
        int any = 0;
        for (unsigned i = 0; i < sizeof(fr); i++)
            any |= fr[i];
        if (r == BCMW_OK && any)
            ent_add(fr, sizeof(fr));
        if (!fw_rand_logged) {
            fw_rand_logged = 1;
            sb_log(r == BCMW_OK && any ? "wifi: tls: the radio's random source answers: mixed into the pool"
                                       : "wifi: tls: the radio has no random source (\"rand\"): timing only");
        }
        memset(fr, 0, sizeof(fr));
    }
    ent_tick();
    {
        br_sha256_context c = ent;          /* the seed: the pool's hash so far, then fed back */
        br_sha256_out(&c, out);
    }
    ent_add(out, 32);
}

/* one fetch at a time: the request, its body (64 KiB kept), its result */
#define FETCH_BODY (64U * 1024U)
static struct sbhttp *fx;
static uint8_t *fx_body;
/* fx_state (declared above): 0 none, 1 running, 2 finished */
static int fx_result;
static char fx_host[64];

static uint64_t nio_unix(void *ctx)
{
    (void)ctx;
    struct sbos_time t;
    if (!clock_now(&t) || t.utc <= 0)
        return 0;
    if (t.approx && clock_checked <= 0)
        return 0;                           /* the last known time, not checked yet: not good enough for certificates */
    return (uint64_t)t.utc;
}

static uint64_t ms_now(void *ctx)
{
    (void)ctx;
    return sb_ms();
}

/* the offline TLS self-check's log: to pmsg */
static void tls_log(void *ctx, const char *line)
{
    (void)ctx;
    sb_logf("wifi: ", line);
}

int wifi_tls_selfcheck(void)
{
    return sbtls_selfcheck(tls_log, 0, ms_now);
}

int wifi_fetch(const char *url)
{
    struct sbhttp_net hn;
    uint8_t seed[32];
    if (st != SBOS_WIFI_CONNECTED)
        return SBOS_EAGAIN;
    if (fx_state == 1)
        return SBOS_EBUSY;
    if (!fx) {
        fx = sb_alloc(sizeof(*fx));
        fx_body = sb_alloc(FETCH_BODY);
        if (!fx || !fx_body)
            return SBOS_EAGAIN;
    }
    nio.net = &net, nio.tcp = &tcp, nio.unix_time = nio_unix, nio.log = net_log, nio.ctx = 0;
    sbnetio_init(&nio, &hn);
    hn.log_host = 0;                        /* where the user goes is theirs: not logged */
    {
        /* the host, for the screen (never logged) */
        const char *p = url;
        unsigned n = 0;
        if (sb_prefix(p, "https://"))
            p += 8;
        else if (sb_prefix(p, "http://"))
            p += 7;
        while (*p && *p != '/' && *p != ':' && n + 1 < sizeof(fx_host))
            fx_host[n++] = *p++;
        fx_host[n] = 0;
    }
    hw_wdt_feed();
    ent_seed(seed);
    hw_wdt_feed();
    fx_state = 1;
    fx_result = sbhttp_start(fx, &hn, url, fx_body, FETCH_BODY, seed, 30000);
    memset(seed, 0, sizeof(seed));
    if (fx_result != SBHTTP_BUSY)
        fx_state = 2;
    kt_set(KT_WIFI, 0, sb_ms(), 10, 0);
    notify();
    return 0;
}

static void fetch_poll(void)
{
    if (fx_state != 1)
        return;
    if (st != SBOS_WIFI_CONNECTED) {
        sbhttp_abort(fx);
        fx_result = SBHTTP_E_NETWORK;
        fx_state = 2;
        notify();
        return;
    }
    hw_wdt_feed();                          /* a TLS handshake's RSA/ECDSA checks take tens of ms */
    fx_result = sbhttp_poll(fx);
    if (fx_result != SBHTTP_BUSY) {
        fx_state = 2;
        notify();
    }
}

void wifi_fetch_cancel(void)
{
    if (fx_state == 1) {
        sbhttp_abort(fx);
        fx_result = SBHTTP_E_ABORTED;
        fx_state = 2;
        notify();
    }
}

void wifi_fetch_status(struct sbos_wifi_fetch *f)
{
    memset(f, 0, sizeof(*f));
    f->state = fx_state;
    f->result = fx_result;
    sb_strcpy(f->host, sizeof(f->host), fx_host);
    if (!fx)
        return;
    sb_strcpy(f->why, sizeof(f->why), sbhttp_why(fx_result));
    f->status = fx->status;
    f->tls_err = fx->tls_err;
    f->tls = fx->tls;
    f->tls_version = fx->tls_version;
    f->tls_suite = fx->tls_suite;
    f->bytes = (uint32_t)(fx->body_total > 0xffffffffull ? 0xffffffffu : fx->body_total);
    f->kept = fx->body_len;
    f->dns_ms = fx->dns_ms, f->connect_ms = fx->connect_ms, f->tls_ms = fx->tls_ms, f->total_ms = fx->total_ms;
    f->first_byte_ms = fx->first_byte_ms;
    f->secs = (int32_t)((sb_ms() - fx->t_start) / 1000U);
}

const uint8_t *wifi_fetch_body(uint32_t *len)
{
    *len = fx && fx_state == 2 ? fx->body_len : 0;
    return fx_body;
}

/* ---- on and off ------------------------------------------------------------------------------------------ */

#ifdef SB_HOST
static int host_file(void *ctx, const char *name, const uint8_t **data, uint32_t *len)
{
    /* the model's synthetic firmware (starbite/wifi/host/test.c's synth_files) */
    static uint8_t fw[64 * 1024];
    static const char banner[] = "4361b2-roml/config_pcie_release_ipa Version: 13.50.99 (model) FWID: 01-0";
    static const char nv[] = "boardrev=0x1101\nmacaddr=00:90:4c:12:d0:01\nccode=0\n";
    static uint8_t clm[3000];
    (void)ctx;
    if (!fw[1]) {
        for (unsigned i = 0; i < sizeof(fw); i++)
            fw[i] = (uint8_t)(i * 7);
        fw[0x6c] = 0;
        memcpy(fw + sizeof(fw) - 200, banner, sizeof(banner));
        memcpy(clm, "BLOB", 4);
        M.fw_rstvec = (uint32_t)fw[0] | (uint32_t)fw[1] << 8 | (uint32_t)fw[2] << 16 | (uint32_t)fw[3] << 24;
        M.fw_image = fw;
        M.fw_image_len = sizeof(fw);
    }
    if (sb_prefix(name, "bcmdhd_sta.bin"))
        return *data = fw, *len = sizeof(fw), 0;
    if (sb_prefix(name, "nvram.txt"))
        return *data = (const uint8_t *)nv, *len = sizeof(nv) - 1, 0;
    if (sb_prefix(name, "bcmdhd_clm"))
        return *data = clm, *len = sizeof(clm), 0;
    return -1;
}
#endif

#ifndef SB_HOST
static void say(const char *s)
{
    sb_log(s);
}
#endif

int wifi_power(int on)
{
    if (!on) {
        if (st == SBOS_WIFI_OFF)
            return 0;
        wifi_fetch_cancel();
        irq_stop();
        logn("wifi: interrupts while on: ", irq_n, "");
        if (w_ready) {
            net_down();
            if (joined >= 0 && !w->dead)
                (void)bcmw_leave(w);
            bcmw_quiesce(w);
        }
        kt_cancel(KT_WIFI, 0);
        dhcp_go = 0;
        scan_retry = 0;
        join_pending = join_active = 0;
        auto_pending = 0;
        memset(jp_pass, 0, sizeof(jp_pass));
        joined = -1;
        nnets = 0;
        logn("wifi: off (the radio held in reset and unpowered) after ", (sb_ms() - t_on) / 1000U, " s");
        set_state(SBOS_WIFI_OFF, "");
        return 0;
    }
    if (st != SBOS_WIFI_OFF && st != SBOS_WIFI_FAILED)
        return 0;
    if (heater_busy()) {
        set_state(SBOS_WIFI_OFF, "Not while heating (0.4)");
        return SBOS_EAGAIN;
    }
    if (!w) {
        w = sb_alloc(bcmw_size());
        if (!w)
            return SBOS_EAGAIN;
        memset(w, 0, bcmw_size());
    }
    w_ready = 0;
    scan_retry = 0;
    auto_pending = auto_done = 0;           /* 1.2-alpha21: one automatic join per power-on */
    irq_n = irq_missed = 0;
    irq_trusted = irq_kicked = 0;
    dk_next = dk_new = dk_reused = 0;      /* the DMA blocks of the first power-on, again in order */
#ifdef SB_HOST
    model_reset();
    M.open_ap = !getenv("SBOS_HOST_WIFINET") && !getenv("SBOS_HOST_WPA2");   /* else the model's WPA2 "test-ap" */
    M.echo_sync = 1;
    memset(&os, 0, sizeof(os));
    os.rd32 = model_rd32, os.wr32 = model_wr32, os.rd16 = model_rd16, os.wr16 = model_wr16;
    os.udelay = model_udelay, os.now_us = model_now, os.dma_alloc = model_dma_alloc;
    os.file = host_file, os.log = os_log, os.event = os_event;
#else
    if (!bcmw_sb_vendor(say)) {
        set_state(SBOS_WIFI_FAILED, "The radio's files could not be read");
        return SBOS_EAGAIN;
    }
    bcmw_sb_os(&os, os_log, os_event);
#endif
    os.rx = os_rx;
    dma_first = os.dma_alloc;
    os.dma_alloc = dma_reuse;
    memset(&cfg, 0, sizeof(cfg));
    cfg.stop_after = BCMW_STEP_UP;
    cfg.rx_buffers = 256;                   /* 1.2-alpha7: as the Windows driver that joins (wifi-core 24a0adf) */
#ifdef SB_HOST
    cfg.any_files = 1;
    memcpy(cfg.mac, "\x02\x11\x22\x33\x44\x55", 6);   /* the station address the model's access point expects */
#endif
    hw_wdt_feed();
    if (bcmw_init(w, &os, &cfg) != BCMW_OK) {
        set_state(SBOS_WIFI_FAILED, "The Wi-Fi core did not start");
        return SBOS_EAGAIN;
    }
    w_ready = 1;
    dhcp_go = 0;
    join_pending = join_active = 0;
    load_saved();
    step = 0;
    nnets = 0;
    joined = -1;
    pinged = 0;
    t_on = sb_ms();
    sb_log("wifi: on (asked): the radio's bring-up, one step per tick");
    set_state(SBOS_WIFI_STARTING, "");
    kt_set(KT_WIFI, 0, sb_ms(), 10, 0);
    return 0;
}

int wifi_scan(void)
{
    if (st != SBOS_WIFI_UP && st != SBOS_WIFI_CONNECTED)
        return SBOS_EAGAIN;
    int was = st;
    nnets = st == SBOS_WIFI_CONNECTED ? nnets : 0;
    scan_heard = 0;
    if (was == SBOS_WIFI_UP)
        set_state(SBOS_WIFI_SCANNING, "");  /* first: the scan's events can arrive while the command waits */
    kt_set(KT_WIFI, 0, sb_ms(), 10, 0);    /* the tick soon: the scan's events */
    hw_wdt_feed();
    if (bcmw_scan_start(w, 0, 0, ++sync) != BCMW_OK) {
        if (was == SBOS_WIFI_UP)
            set_state(SBOS_WIFI_UP, "");
        if (!scan_retry)
            scan_retry = 3;
        if (--scan_retry > 0) {             /* busy (the bring-up's own scan): again in 1.5 s */
            scan_retry_at = sb_ms() + 1500;
            sb_log("wifi: the scan was refused (busy?): trying again");
            return 0;
        }
        sb_log("wifi: the scan did not start");
        if (was == SBOS_WIFI_UP)
            set_state(SBOS_WIFI_UP, "The scan did not start");
        return SBOS_EAGAIN;
    }
    scan_retry = 0;
    return 0;
}

int wifi_join(int i, const char *pass)
{
    if (st != SBOS_WIFI_UP || i < 0 || i >= nnets)
        return SBOS_EAGAIN;
    kt_set(KT_WIFI, 0, sb_ms(), 10, 0);    /* the tick soon (a relaxed poll may be pending) */
    scan_retry = 0;
    if (nets[i].secure) {
        const char *k = pass && pass[0] ? pass : known_pass(nets[i].ssid, nets[i].ssid_len);
        unsigned n = k ? sb_strlen(k) : 0;
        if (!k)
            return SBOS_EPERM;              /* secured, no passphrase known: the app asks for one */
        if (n < 8 || n > 64)
            return SBOS_EINVAL;
        sb_strcpy(jp_pass, sizeof(jp_pass), k);
    }
    if (nets[i].secure) {
        /* 0.6.1: bcmw_join_start (the directed scan, the PMK, SET_SSID: a few seconds at most) on the next tick, then
         * one handshake round per tick (bcmw_join_poll): the screen keeps working (0.6 blocked up to 15 s) */
        const struct bcmw_info *in = bcmw_info(w);
        struct sbnet_os no = { 0, net_tx, net_now, net_random, net_log, net_udp, net_tcp };
        sbnet_init(&net, &no, in->mac);
        sbtcp_init(&tcp, &net);
        joined = i;
        join_pending = 1;
        sb_log("wifi: joining a secured network (name and passphrase not logged)");
        set_state(SBOS_WIFI_JOINING, "");
        return 0;
    }
    const struct bcmw_info *in = bcmw_info(w);
    struct sbnet_os no = { 0, net_tx, net_now, net_random, net_log, net_udp, net_tcp };
    sbnet_init(&net, &no, in->mac);
    sbtcp_init(&tcp, &net);
    joined = i;
    sb_log("wifi: joining an open network (name not logged)");
    set_state(SBOS_WIFI_JOINING, "");       /* first: the association's events can arrive inside bcmw_connect */
    if (bcmw_connect(w, nets[i].ssid, nets[i].ssid_len, nets[i].bssid, 0, 0, 0) != BCMW_OK) {
        joined = -1;
        set_state(SBOS_WIFI_UP, "The join did not start");
        return SBOS_EAGAIN;
    }
    return 0;
}

void wifi_status(struct sbos_wifi *s)
{
    memset(s, 0, sizeof(*s));
    s->state = st;
    s->nets = nnets;
    sb_strcpy(s->why, sizeof(s->why), why);
    if (joined >= 0 && joined < nnets) {
        memcpy(s->ssid, nets[joined].ssid, nets[joined].ssid_len);
        s->ssid_len = nets[joined].ssid_len;
        s->rssi = nets[joined].rssi;
    }
    if (st == SBOS_WIFI_CONNECTED) {
        memcpy(s->ip, net.ip, 4);
        memcpy(s->gateway, net.gateway, 4);
        s->ping_ms = net.ping_replies ? (int32_t)net.ping_rtt_ms : -1;
        s->clock_checked = clock_checked;
        s->clock_off_s = clock_off;
    }
    s->secs = (int32_t)((sb_ms() - t_state) / 1000U);
}

/* 1.2-alpha21: the known networks: the saved one (the store's "wifinet", set from the computer) first, then the ones
 * joined with a typed passphrase since the start (RAM only). Names only; passphrases never leave the kernel. */
int wifi_known(struct sbos_wifi_known *out, int max)
{
    int n = 0;
    if (sv_ok && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        memcpy(out[n].ssid, sv_ssid, sv_len);
        out[n].ssid_len = (uint8_t)sv_len;
        out[n].saved = 1;
        ++n;
    }
    for (unsigned i = 0; i < nrem && n < max; ++i) {
        if (sv_ok && rem[i].len == sv_len && !memcmp(rem[i].ssid, sv_ssid, sv_len))
            continue;                       /* the saved one, typed again: listed once */
        memset(&out[n], 0, sizeof(out[n]));
        memcpy(out[n].ssid, rem[i].ssid, rem[i].len);
        out[n].ssid_len = (uint8_t)rem[i].len;
        ++n;
    }
    for (int k = 0; k < n; ++k)
        for (int i = 0; i < nnets; ++i)
            if (nets[i].ssid_len == out[k].ssid_len && !memcmp(nets[i].ssid, out[k].ssid, out[k].ssid_len))
                out[k].heard = 1;
    return n;
}

/* forget a network joined since the start (k: its place in wifi_known's list); the saved one: SBOS_EPERM */
int wifi_forget(int k)
{
    struct sbos_wifi_known kn[1 + REMEMBER];
    int n = wifi_known(kn, 1 + REMEMBER);
    if (k < 0 || k >= n)
        return SBOS_EINVAL;
    if (kn[k].saved)
        return SBOS_EPERM;
    for (unsigned i = 0; i < nrem; ++i)
        if (rem[i].len == kn[k].ssid_len && !memcmp(rem[i].ssid, kn[k].ssid, rem[i].len)) {
            memset(rem[i].pass, 0, sizeof(rem[i].pass));
            for (unsigned j = i; j + 1 < nrem; ++j)
                rem[j] = rem[j + 1];
            --nrem;
            memset(&rem[nrem], 0, sizeof(rem[nrem]));
            sb_log("wifi: a network joined since the start was forgotten (name not logged)");
            return 0;
        }
    return SBOS_EINVAL;
}

int wifi_list(struct sbos_wifi_net *out, int max)
{
    int n = nnets < max ? nnets : max;
    for (int i = 0; i < n; ++i)
        out[i] = nets[i];
    return n;
}

/* the status bar: 0 off, 1 on (not joined), 2 joined with an address */
int wifi_bar(void)
{
    return st == SBOS_WIFI_CONNECTED ? 2 : st == SBOS_WIFI_OFF || st == SBOS_WIFI_FAILED ? 0 : 1;
}

void wifi_irq(void)
{
    uint64_t now = sb_ms();
    ent_tick();
    ++irq_n;
    if (now - irq_window_t >= 1000) {
        irq_window_t = now;
        irq_window_n = 0;
    }
    if (++irq_window_n > 2000) {            /* a storm: the line stays asserted; back to polling */
        hw_spi(WIFI_INTID, 0);
        irq_on = 0;
        ++irq_storms;
        sb_log("wifi: an interrupt storm (over 2000 in a second): interrupts off, polling");
        return;
    }
    if (w && w_ready)
        (void)bcmw_isr(w);                  /* the chip's mailbox masked until the next poll; the latch cleared */
    irq_kicked = 1;
    if (st != SBOS_WIFI_OFF)
        kt_set(KT_WIFI, 0, now, 0, 0);      /* the tick (bcmw_poll) at once */
}

static void irq_start(void)
{
    if (irq_on || irq_storms)
        return;
    ex_intx(w, 1);                          /* the root complex's INTx enable (ELBI), the core's own sequence */
    if (hw_spi(WIFI_INTID, 1)) {
        irq_on = 1;
        sb_log("wifi: interrupts on (SPI 253); polling stays as the fallback");
    }
}

static void irq_stop(void)
{
    if (irq_on)
        hw_spi(WIFI_INTID, 0);
    irq_on = 0;
}

/* 1.2-alpha26: a boot without a clock: Wi-Fi on once for the time (the saved network joins by itself), then off.
 * Not in a self test, not without a saved network or with automatic joins off; the user's own Wi-Fi switch, or Heat,
 * ends it */
static int time_only;
static uint64_t time_only_t;

int wifi_time_only(void)
{
    return time_only && st != SBOS_WIFI_OFF;
}

void wifi_time_only_end(const char *why)
{
    if (!time_only)
        return;
    time_only = 0;
    sb_logf("wifi: the boot's time check ends: ", why);
}

void wifi_boot_time_check(void)
{
    struct sbos_time t;
    if (sbos_selftest_enabled || wifi_in_selftest() || !wifi_autojoin || (clock_now(&t) && !t.approx))
        return;
    load_saved();
    if (!sv_ok)
        return;
    time_only = 1;
    time_only_t = sb_ms();
    sb_log("wifi: no clock at boot: on once for the time (the saved network), then off");
    if (wifi_power(1))
        wifi_time_only_end("Wi-Fi did not come on");
}

void wifi_timer(unsigned id)
{
    (void)id;
    if (st == SBOS_WIFI_OFF)
        return;
    uint64_t now = sb_ms();
    if (time_only && (ntp_st == 3 || ntp_st == -1 || now - time_only_t > 90000U)) {
        wifi_time_only_end(ntp_st == 3 ? "the clock is set; Wi-Fi off again" : "no time came; Wi-Fi off again");
        wifi_power(0);
        return;
    }
#ifdef SB_HOST
    if (M.now < now * 1000U)
        M.now = now * 1000U;                /* the model's clock follows the host's (a polled join's timeout) */
#endif
    if (auto_pending && st == SBOS_WIFI_UP) {   /* 1.2-alpha21: from the tick, not inside the chip's event */
        auto_pending = 0;
        auto_done = 1;
        for (int i = 0; i < nnets; ++i)     /* strongest first (sort_nets) */
            if (known_pass(nets[i].ssid, nets[i].ssid_len)) {
                sb_log("wifi: a known network was heard: joining it automatically (name not logged)");
                (void)wifi_join(i, 0);
                break;
            }
    }
    if (st == SBOS_WIFI_STARTING) {
        uint64_t t0 = sb_ms();
        hw_wdt_feed();                      /* the longest steps take seconds (0.5.12) */
        int r = bcmw_bringup(w, (unsigned)step + 1U);
        hw_wdt_feed();
        uint64_t took = sb_ms() - t0;
        if (r < 0) {
            logn("wifi: the bring-up failed at step ", (uint64_t)step + 1U, "");
            irq_stop();
            bcmw_quiesce(w);
            set_state(SBOS_WIFI_FAILED, "The radio did not start (logged)");
            return;
        }
        step = r;
        if (took >= 150)
            logn("wifi: a bring-up step took ", took, " ms");
        if (step >= BCMW_STEP_UP) {
            irq_start();
            logn("wifi: up in ", now - t_on, " ms; scanning");
            struct ub u;
            ub_init(&u);
            ub_s(&u, "wifi: DMA blocks: ");
            ub_dec(&u, dk_new);
            ub_s(&u, " new, ");
            ub_dec(&u, dk_reused);
            ub_s(&u, " reused");
            sb_log(u.b);
            set_state(SBOS_WIFI_UP, "");
            (void)wifi_scan();
        }
    } else {
        int kicked = irq_kicked;
        irq_kicked = 0;
        int got = bcmw_poll(w);
        if (got > 0 && !kicked && irq_on && irq_n && irq_trusted) {
            /* work no interrupt announced: the line is not reliable; poll fast again (logged once) */
            irq_trusted = 0;
            if (!irq_missed++)
                sb_log("wifi: work arrived without an interrupt: polling at the normal rate again");
        } else if (kicked && got > 0 && !irq_missed) {
            irq_trusted = 1;
        }
        if (w->dead) {
            irq_stop();
            sb_log("wifi: the chip stopped answering: off");
            bcmw_quiesce(w);
            joined = -1;
            kt_cancel(KT_WIFI, 0);
            set_state(SBOS_WIFI_FAILED, "The radio stopped answering (logged)");
            return;
        }
        if (scan_retry && st == SBOS_WIFI_UP && now >= scan_retry_at)
            (void)wifi_scan();
        int r = BCMW_E_PENDING;
        if (join_pending && st == SBOS_WIFI_JOINING && now - t_state >= 100) {
            join_pending = 0;
            join_active = 1;                /* first: the association's events arrive inside bcmw_join_start */
            join_t0 = sb_ms();
            hw_wdt_feed();
            r = joined >= 0 ? bcmw_join_start(w, nets[joined].ssid, nets[joined].ssid_len, jp_pass, 15000) : BCMW_E_STATE;
            hw_wdt_feed();
            uint64_t took = sb_ms() - join_t0;
            if (took >= 150)
                logn("wifi: the join's start (the directed scan, the PMK) took ", took, " ms");
            if (r == BCMW_OK)
                r = BCMW_E_PENDING;
            now = sb_ms();
        } else if (join_active) {
            r = bcmw_join_poll(w);
        }
        if (r != BCMW_E_PENDING) {
            uint64_t t0 = join_t0;
            join_active = 0;
            if (r == BCMW_OK && joined >= 0)
                remember(nets[joined].ssid, nets[joined].ssid_len, jp_pass);
            memset(jp_pass, 0, sizeof(jp_pass));
            if (r != BCMW_OK) {
                logn("wifi: the network did not take the phone after ", sb_ms() - t0, " ms");
                joined = -1;
                dhcp_go = 0;                /* a LINK event may have come before the handshake failed */
                set_state(SBOS_WIFI_UP, "Could not join: the passphrase, or too far away");
            } else if (st == SBOS_WIFI_JOINING) {
                logn("wifi: joined a secured network in ", sb_ms() - t0, " ms; asking for an address (DHCP)");
                set_state(SBOS_WIFI_DHCP, "");
                dhcp_go = 1;
            }
            now = sb_ms();
        }
        if (dhcp_go && st == SBOS_WIFI_DHCP) {
            dhcp_go = 0;
            sbnet_start(&net);
        }
        if (st == SBOS_WIFI_SCANNING && now - t_state > SCAN_MS) {
            sort_nets();
            logn("wifi: the scan timed out with ", (uint64_t)nnets, " networks");
            set_state(SBOS_WIFI_UP, "");
            auto_pending = wifi_autojoin && !auto_done && joined < 0 && !wifi_in_selftest();
        }
        if (st == SBOS_WIFI_JOINING && !join_pending && !join_active && now - t_state > JOIN_MS) {
            sb_log("wifi: the join timed out");
            (void)bcmw_leave(w);
            joined = -1;
            set_state(SBOS_WIFI_UP, "The network did not answer");
        }
        if (st == SBOS_WIFI_DHCP || st == SBOS_WIFI_CONNECTED) {
            sbnet_tick(&net);
            sbtcp_tick(&tcp);
            fetch_poll();
            if (st == SBOS_WIFI_DHCP && net.state == SBNET_BOUND) {
                logn("wifi: an address from DHCP (not logged), lease ", net.lease_s, " s");
                set_state(SBOS_WIFI_CONNECTED, "");
                pinged = 0;
                ntp_st = 0;
                clock_checked = 0;
            } else if (st == SBOS_WIFI_DHCP && now - t_state > DHCP_MS) {
                sb_log("wifi: no DHCP answer");
                (void)bcmw_leave(w);
                net_down();
                joined = -1;
                set_state(SBOS_WIFI_UP, "Joined, but no address came (DHCP)");
            }
            if (st == SBOS_WIFI_CONNECTED && net.state != SBNET_BOUND && net.state != SBNET_RENEWING &&
                net.state != SBNET_REBINDING) {
                sb_log("wifi: the address was lost (DHCP); asking again");
                set_state(SBOS_WIFI_DHCP, "");
                sbnet_start(&net);
            }
            if (st == SBOS_WIFI_CONNECTED && !pinged && now - t_state > 500) {
                pinged = 1;
                (void)sbnet_ping(&net, net.gateway);
            }
            if (st == SBOS_WIFI_CONNECTED && pinged == 1 && net.ping_replies) {
                pinged = 2;
                logn("wifi: the gateway answers a ping in ", net.ping_rtt_ms, " ms");
                notify();
            }
            if (st == SBOS_WIFI_CONNECTED && ntp_st == 0 && (pinged == 2 || (pinged == 1 && now - t_state > 3500))) {
                ntp_st = sbnet_dns_query(&net, "pool.ntp.org") == 0 ? 1 : -1;
                ntp_t = now;
            }
            if (ntp_st == 1 && net.dns_state == SBNET_DNS_DONE) {
                uint8_t q[48];
                memset(q, 0, sizeof(q));
                q[0] = 0x23;                /* NTP version 4, client */
                for (int i = 0; i < 8; i += 4) {    /* a nonce in the transmit time: the answer must echo it */
                    uint32_t r = net_random(0);
                    memcpy(ntp_nonce + i, &r, 4);
                }
                memcpy(q + 40, ntp_nonce, 8);
                memcpy(ntp_server, net.dns_result, 4);
                ntp_t = sb_ms();
                ntp_st = sbnet_udp_send(&net, net.dns_result, 123, NTP_PORT, q, sizeof(q)) == 0 ? 2 : -1;
            } else if ((ntp_st == 1 && net.dns_state == SBNET_DNS_FAILED) ||
                       ((ntp_st == 1 || ntp_st == 2) && now - ntp_t > 8000)) {
                ntp_st = -1;
                clock_checked = -1;
                sb_log("wifi: no answer from the time server (the clock stays as it is)");
                notify();
            }
        }
    }
    if (fx_state == 1 && st != SBOS_WIFI_CONNECTED)
        fetch_poll();                       /* the network went: the fetch ends */
    int busy = st == SBOS_WIFI_STARTING || st == SBOS_WIFI_SCANNING || st == SBOS_WIFI_JOINING || st == SBOS_WIFI_DHCP ||
               join_pending || join_active || dhcp_go || fx_state == 1;
    uint32_t every = st == SBOS_WIFI_STARTING ? 10U : K.screen_on ? TICK_ON_MS : TICK_OFF_MS;
    if (irq_on && irq_n && irq_trusted && !busy)
        every = st == SBOS_WIFI_CONNECTED ? 250U : 1000U;   /* interrupts wake it; sbnet's timers need 250 ms */
    if (fx_state == 1)
        every = 5U;                         /* a fetch: TCP's segments and TLS's records without waiting */
    if (st != SBOS_WIFI_OFF && st != SBOS_WIFI_FAILED && !kt_pending(KT_WIFI, 0))
        kt_set(KT_WIFI, 0, sb_ms(), every, 0);
}

/* the self test: on, the bring-up, one scan; 0.6.1: then, if the saved network ("wifinet") was heard, a join with
 * its passphrase (WPA2 or open), an address, the router's ping and the clock check; then off. 1 passed, 0 failed */
int wifi_selftest_scan_only;                /* Next's Wi-Fi self test: the scan only, never a join */

int wifi_selftest_phase(int *done)
{
    static int started, jstate;            /* jstate: 0 not yet, 1 joining, 2 done */
    static int seen_dhcp;                   /* 1.2-alpha25: the join and its handshake done, the address asked for */
    static uint64_t jt0;
    *done = 0;
    if (jstate == 1) {
        if (st == SBOS_WIFI_DHCP)
            seen_dhcp = 1;
        int failed = st == SBOS_WIFI_UP || st == SBOS_WIFI_FAILED || st == SBOS_WIFI_OFF;
        int finished = st == SBOS_WIFI_CONNECTED && (ntp_st == 3 || ntp_st == -1);   /* the clock checked, or no answer */
        if (failed || finished || sb_ms() - jt0 > 45000U) {
            jstate = 2;
            if (st == SBOS_WIFI_CONNECTED) {
                struct ub u;
                ub_init(&u);
                ub_s(&u, "selftest: wifi: JOINED the saved network, an address; the router answered: ");
                ub_s(&u, pinged == 2 ? "yes" : "no");
                ub_s(&u, "; the clock checked: ");
                ub_s(&u, ntp_st == 3 ? "yes" : "no");
                ub_s(&u, "; ");
                ub_dec(&u, sb_ms() - jt0);
                ub_s(&u, " ms");
                sb_log(u.b);
            } else if (seen_dhcp) {
                /* 1.2-alpha25 (the main session): the join and the 4-way handshake are ours and passed; the address
                 * is the router's: a warning, not a failure (alpha24 on the phone: three DISCOVERs unanswered) */
                sb_log("selftest: WARNING: wifi: joined (the handshake done) but no address (DHCP; the router)");
                ++selftest_warnings;
            } else {
                sb_logf("selftest: wifi: the saved network did NOT join: ", why[0] ? why : "no address in 45 s");
            }
            int ok = st == SBOS_WIFI_CONNECTED || seen_dhcp;
            wifi_power(0);
            *done = 1;
            return ok;
        }
        return 1;
    }
    if (!started) {
        started = 1;
        wifi_power(0);                      /* from off: a radio already on would make t_on stale */
        if (wifi_selftest_scan_only) {
            /* Next: TLS's own check first (no network): known answers and two real certificate chains */
            uint64_t t0 = sb_ms();
            hw_wdt_feed();
            int tr = wifi_tls_selfcheck();
            hw_wdt_feed();
            logn(tr ? "selftest: wifi: the TLS self-check FAILED at check " : "selftest: wifi: the TLS self-check passed in ",
                 tr ? (uint64_t)tr : sb_ms() - t0, tr ? "" : " ms");
            if (tr) {
                *done = 1;
                return 0;
            }
        }
    }
    if (st == SBOS_WIFI_OFF) {
        if (wifi_power(1)) {
            *done = 1;
            return 0;
        }
        return 1;
    }
    if (st == SBOS_WIFI_FAILED) {
        *done = 1;
        return 0;
    }
    if (st == SBOS_WIFI_UP && !scan_retry) {
        logn("selftest: wifi: up and scanned in ", sb_ms() - t_on, " ms");
        logn("selftest: wifi: networks heard: ", (uint64_t)nnets, " (names not logged)");
        logn("selftest: wifi: interrupts during the bring-up and scan: ", irq_n,
             irq_storms ? " (a STORM: off, polling)" : irq_on ? " (on)" : " (polling)");
        if (irq_missed)
            logn("selftest: wifi: work found without an interrupt, times: ", irq_missed, "");
        int heard = -1;
        if (wifi_selftest_scan_only) {
            int secure = 0;
            for (int i = 0; i < nnets; ++i)
                secure += nets[i].secure != 0;
            logn("selftest: wifi: secured networks among them: ", (uint64_t)secure, "");
            sb_log(nnets ? "selftest: wifi: passed: scan only (no join in this test)" :
                           "selftest: wifi: FAILED: the scan heard nothing");
            wifi_power(0);
            *done = 1;
            return nnets > 0;
        }
        for (int i = 0; sv_ok && jstate == 0 && i < nnets; ++i)
            if (nets[i].ssid_len == sv_len && !memcmp(nets[i].ssid, sv_ssid, sv_len))
                heard = i;
        if (heard >= 0) {
            sb_log(nets[heard].secure ? "selftest: wifi: the saved network was heard: joining it (WPA2; name not logged)"
                                      : "selftest: wifi: the saved network was heard: joining it (open; name not logged)");
            jt0 = sb_ms();
            if (wifi_join(heard, 0) == 0) {
                jstate = 1;
                return 1;
            }
            sb_log("selftest: wifi: the join was refused");
            wifi_power(0);
            *done = 1;
            return 0;
        }
        sb_log(sv_ok ? "selftest: wifi: the saved network was not heard: no join" : "selftest: wifi: no saved network: no join");
        wifi_power(0);
        *done = 1;
        return 1;
    }
    if (jstate == 0 && sb_ms() - t_on > 30000U) {
        sb_log("selftest: wifi: not up within 30 s");
        wifi_power(0);
        *done = 1;
        return 0;
    }
    return 1;
}
