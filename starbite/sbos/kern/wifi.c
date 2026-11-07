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
 * does not). Off: bcmw_quiesce (the chip in reset and unpowered). */
#include "sb.h"
#include "kern.h"
#include "sbapp.h"
#include "bcmw.h"
#include "bcmw_priv.h"                      /* w->dead: the chip stopped answering */
#include "net.h"
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
/* the saved network (0.4.2): the store entry "wifinet", two lines (name, WPA2 passphrase or empty), made on the
 * Mac from the user's input for the Wi-Fi tests; read only, never logged, never shown (the passphrase) */
static uint8_t sv_ssid[33];
static unsigned sv_len;
static char sv_pass[65];
static int sv_ok, join_pending;
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
static unsigned irq_n, irq_window_n, irq_storms, irq_missed;
static uint64_t irq_window_t;
static void irq_stop(void);                         /* bcmw_init succeeded on w (quiesce and leave only then) */
static int dhcp_go;                         /* the link came up inside a callback: DHCP starts from the tick */

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
            if (st == SBOS_WIFI_JOINING) {
                logn("wifi: the link is up after ", sb_ms() - t_state, " ms; asking for an address (DHCP)");
                set_state(SBOS_WIFI_DHCP, "");
                dhcp_go = 1;                /* not from inside bcmw_poll: the first frame opens the tx flow ring */
            }
        } else {
            sb_log("wifi: the link went down");
            sbnet_stop(&net);
            set_state(SBOS_WIFI_UP, "The network dropped the phone");
            joined = -1;
        }
    } else if (ev->type == BCMW_E_DEAUTH || ev->type == BCMW_E_DEAUTH_IND || ev->type == BCMW_E_DISASSOC_IND) {
        sb_log("wifi: left by the access point");
        sbnet_stop(&net);
        set_state(SBOS_WIFI_UP, "The network dropped the phone");
        joined = -1;
    }
}

static void os_rx(void *ctx, unsigned ifidx, const uint8_t *frame, uint32_t len)
{
    (void)ctx;
    (void)ifidx;
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

static void net_log(void *ctx, const char *line)
{
    (void)ctx;
    sb_logf("wifi: ", line);
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
        irq_stop();
        logn("wifi: interrupts while on: ", irq_n, "");
        if (w_ready) {
            sbnet_stop(&net);
            if (joined >= 0 && !w->dead)
                (void)bcmw_leave(w);
            bcmw_quiesce(w);
        }
        kt_cancel(KT_WIFI, 0);
        dhcp_go = 0;
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
#ifdef SB_HOST
    cfg.any_files = 1;
    memcpy(cfg.mac, "\x02\x11\x22\x33\x44\x55", 6);   /* the station address the model's access point expects */
#endif
    if (bcmw_init(w, &os, &cfg) != BCMW_OK) {
        set_state(SBOS_WIFI_FAILED, "The Wi-Fi core did not start");
        return SBOS_EAGAIN;
    }
    w_ready = 1;
    dhcp_go = 0;
    join_pending = 0;
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
    if (bcmw_scan_start(w, 0, 0, ++sync) != BCMW_OK) {
        sb_log("wifi: the scan did not start");
        if (was == SBOS_WIFI_UP)
            set_state(SBOS_WIFI_UP, "The scan did not start");
        return SBOS_EAGAIN;
    }
    return 0;
}

int wifi_join(int i, const char *pass)
{
    if (st != SBOS_WIFI_UP || i < 0 || i >= nnets)
        return SBOS_EAGAIN;
    kt_set(KT_WIFI, 0, sb_ms(), 10, 0);    /* the tick soon (a relaxed poll may be pending) */
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
        /* bcmw_join does the WPA2 handshake and blocks (up to 15 s): the screen shows "Joining" first, the join
         * runs on the next tick (the heater is never on with Wi-Fi) */
        const struct bcmw_info *in = bcmw_info(w);
        struct sbnet_os no = { 0, net_tx, net_now, net_random, net_log, 0 };
        sbnet_init(&net, &no, in->mac);
        joined = i;
        join_pending = 1;
        sb_log("wifi: joining a secured network (name and passphrase not logged)");
        set_state(SBOS_WIFI_JOINING, "");
        return 0;
    }
    const struct bcmw_info *in = bcmw_info(w);
    struct sbnet_os no = { 0, net_tx, net_now, net_random, net_log, 0 };
    sbnet_init(&net, &no, in->mac);
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
    }
    s->secs = (int32_t)((sb_ms() - t_state) / 1000U);
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

void wifi_timer(unsigned id)
{
    (void)id;
    if (st == SBOS_WIFI_OFF)
        return;
    uint64_t now = sb_ms();
    if (st == SBOS_WIFI_STARTING) {
        uint64_t t0 = sb_ms();
        int r = bcmw_bringup(w, (unsigned)step + 1U);
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
        if (join_pending && st == SBOS_WIFI_JOINING && now - t_state >= 100) {
            join_pending = 0;
            uint64_t t0 = sb_ms();
            int r = joined >= 0 ? bcmw_join(w, nets[joined].ssid, nets[joined].ssid_len, jp_pass, 15000) : BCMW_E_STATE;
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
            now = sb_ms();                  /* the join blocked: states were set after the tick's start */
        }
        if (dhcp_go && st == SBOS_WIFI_DHCP) {
            dhcp_go = 0;
            sbnet_start(&net);
        }
        if (st == SBOS_WIFI_SCANNING && now - t_state > SCAN_MS) {
            sort_nets();
            logn("wifi: the scan timed out with ", (uint64_t)nnets, " networks");
            set_state(SBOS_WIFI_UP, "");
        }
        if (st == SBOS_WIFI_JOINING && !join_pending && now - t_state > JOIN_MS) {
            sb_log("wifi: the join timed out");
            (void)bcmw_leave(w);
            joined = -1;
            set_state(SBOS_WIFI_UP, "The network did not answer");
        }
        if (st == SBOS_WIFI_DHCP || st == SBOS_WIFI_CONNECTED) {
            sbnet_tick(&net);
            if (st == SBOS_WIFI_DHCP && net.state == SBNET_BOUND) {
                logn("wifi: an address from DHCP (not logged), lease ", net.lease_s, " s");
                set_state(SBOS_WIFI_CONNECTED, "");
                pinged = 0;
            } else if (st == SBOS_WIFI_DHCP && now - t_state > DHCP_MS) {
                sb_log("wifi: no DHCP answer");
                (void)bcmw_leave(w);
                sbnet_stop(&net);
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
        }
    }
    int busy = st == SBOS_WIFI_STARTING || st == SBOS_WIFI_SCANNING || st == SBOS_WIFI_JOINING || st == SBOS_WIFI_DHCP ||
               join_pending || dhcp_go;
    uint32_t every = st == SBOS_WIFI_STARTING ? 10U : K.screen_on ? TICK_ON_MS : TICK_OFF_MS;
    if (irq_on && irq_n && irq_trusted && !busy)
        every = st == SBOS_WIFI_CONNECTED ? 250U : 1000U;   /* interrupts wake it; sbnet's timers need 250 ms */
    if (st != SBOS_WIFI_OFF && st != SBOS_WIFI_FAILED && !kt_pending(KT_WIFI, 0))
        kt_set(KT_WIFI, 0, sb_ms(), every, 0);
}

/* the self test: on, the bring-up, one scan, off (never a join): 1 passed, 0 failed */
int wifi_selftest_phase(int *done)
{
    static int started;
    *done = 0;
    if (!started) {
        started = 1;
        wifi_power(0);                      /* from off: a radio already on would make t_on stale */
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
    if (st == SBOS_WIFI_UP) {
        logn("selftest: wifi: up and scanned in ", sb_ms() - t_on, " ms");
        logn("selftest: wifi: networks heard: ", (uint64_t)nnets, " (names not logged)");
        logn("selftest: wifi: interrupts during the bring-up and scan: ", irq_n,
             irq_storms ? " (a STORM: off, polling)" : irq_on ? " (on)" : " (polling)");
        if (irq_missed)
            logn("selftest: wifi: work found without an interrupt, times: ", irq_missed, "");
        wifi_power(0);
        *done = 1;
        return 1;
    }
    if (sb_ms() - t_on > 30000U) {
        sb_log("selftest: wifi: not up within 30 s");
        wifi_power(0);
        *done = 1;
        return 0;
    }
    return 1;
}
