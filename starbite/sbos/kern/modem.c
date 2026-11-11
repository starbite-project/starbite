/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 1.2-alpha21: the modem, receive only, through modem-core's session API (docs/MODEM.md 7.6,
 * starbite/modem/sbm_link.c sbm_modem_*; the platform glue starbite/modem/sb/sbm_sb.c, as the modemsess test image).
 * Session API v2 (docs/MODEM.md 7.7; phone-tested 2026-11-11 with modemprobe 4f93087: 3 starts in one boot, 25 min):
 *  - The image is streamed: BOOT/MAIN/VSS are read from RADIO at each start; only nv_data, its pristine copy and a
 *    256 KiB chunk stay in RAM (2.25 MiB, taken once) instead of about 42 MiB. The core restores NV from the pristine
 *    copy before every start.
 *  - Stop and Start again in the same boot use Android's reset path (no second cp_init).
 *  - A stall (an open query, 20 s of silence) or a CP crash parks it; it is restarted automatically after 5 s, at
 *    most 3 times in 30 min, then it stays off (Could not start). A guard (SIM, registration) never restarts.
 *  - Started only by the user: the Modem app asks (SYS_MODEM_POWER 1), the kernel shows its own confirmation, and
 *    only its hold starts it. Never at boot, never in a self-test image, never while Heat or Linux runs.
 *  - Status only: the core's allow-listed GETs every 10 s; no call, SMS or data frame exists in the core. A SIM that
 *    is not absent or a network registration parks the CP inside sbm_modem_poll (the guards).
 *  - RADIO and EFS are read once (read only); the CP's NV writes go to a RAM copy, put back from a pristine copy
 *    before every start, so each start serves the EFS data as read. Nothing is written to any partition, and libsb's
 *    storage hold is on while the CP runs (no block write at all, Starbite's own slot included).
 *  - Parked on Stop, on a guard, before Heat or Linux starts, on every restart / power-off path (services.c), and
 *    by sb_before_reset (libsb's reboot, power-off and crash-screen paths).
 *  - Polled every 2 ms from a kernel timer (the test image: every 1 ms); the CP's interrupt is not used.
 *  - Apps get the state and the masked line: never the PLMN (the operator), never an IMEI or IMSI. */
#include "sb.h"
#include "kern.h"
#include "sbapp.h"
#include "../../guest/sbos/guest.h"

#ifndef SB_HOST
#include "../../modem/sbm.h"
#include "../../modem/sb/sbm_sb.h"
#include "net.h"                            /* starbite/wifi: sbnet (raw IP for mobile data) and sbtcp */
#include "tcp.h"
#else
#include <stdlib.h>
#endif

#define POLL_MS 2U
#define QUERY_MS 10000U
#define RECOVER_MS 1000U
#define MIN_ARENA (4ULL << 20)             /* v2: nv_data + its pristine copy + the 256 KiB stream chunk (2.25 MiB) */

static int st = SBOS_MODEM_OFF, starts;
static uint64_t t_state, t_up, t_poll;
static char why[64], line[112];

#ifndef SB_HOST
static struct sbm_plat plat;
static struct sbm_image im;
static struct sbm_modem mo;
static int loaded, armed;
static uint64_t t_rec;
static uint32_t snap_gen;
static void cell_begin(void);
static void cell_net_down(const char *w);
static void cell_stopped(const char *w, int failed);
static void cset(int s, const char *w);
#else
static uint32_t h_msgs, h_queries, h_answers;
static int h_guard_s = -1;
#endif

static int guest_busy(void)
{
    int g = guest_state();
    return g == GUEST_STARTING || g == GUEST_RUNNING || g == GUEST_STOPPING;
}

int kmodem_on(void)
{
    return st == SBOS_MODEM_STARTING || st == SBOS_MODEM_ON;
}

static void notify(void)
{
    ui_mark_dirty();
    int a = app_by_id("modem");
    if (a >= 0)
        app_event_send(a, EV_APP_SERVICE, 4, st);
}

static void set_state(int s, const char *w)
{
    st = s;
    t_state = sb_ms();
    sb_strcpy(why, sizeof(why), w ? w : "");
    notify();
}

#ifndef SB_HOST
static void mlog(const char *s)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "modem: ");
    ub_s(&u, s);
    sb_log(u.b);
}

/* libsb's reboot / power-off / crash-screen paths: the CP parked first (Android's power-off) */
static void park_hook(void)
{
    if (!armed)
        return;
    armed = 0;
    sb_log("modem: reset hook: parking the CP before the reset");
    sbm_cp_park(&plat);
}

static int load(void)
{
    if (loaded)
        return 0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "modem: loading RADIO and EFS (read only); reserved at boot ");
    ub_size(&u, sbm_sb_reserved());
    ub_s(&u, ", free RAM ");
    ub_size(&u, sb_arena_free());
    sb_log(u.b);
    if (!sbm_sb_reserved() && sb_arena_free() < MIN_ARENA) {
        sb_log("modem: not enough free RAM for the modem's buffers (and none reserved at boot)");
        set_state(SBOS_MODEM_FAILED, "Not enough memory for the modem");
        return -1;
    }
    sbm_sb_plat(&plat, mlog);
    uint64_t used0 = sb_arena_used();
    int r = sbm_sb_load_image_stream(&im, mlog);    /* RADIO streamed at each start; NV + its pristine copy kept */
    if (r < 0) {
        struct ub e;
        ub_init(&e);
        ub_s(&e, "modem: loading RADIO / EFS failed (");
        ub_s(&e, sbm_strerror(r));
        ub_s(&e, ")");
        sb_log(e.b);
        set_state(SBOS_MODEM_FAILED, r == SBM_ENOSPC ? "Not enough memory for the modem" : "Reading RADIO / EFS failed");
        return -1;
    }
    ub_init(&u);
    ub_s(&u, "modem: image ready (streamed from RADIO at each start); RAM taken ");
    ub_size(&u, sb_arena_used() - used0);
    sb_log(u.b);
    sbm_sb_plat_stage4(&plat, im.mm.ipc_off);
    loaded = 1;
    return 0;
}
#endif

static void stopped(int s, const char *w)
{
    kt_cancel_owner(KT_MODEM);
    sb_storage_hold(0);
    set_state(s, w);
}

/* the start itself (timer id 1, after the Starting page was drawn): blocking, about 2 s plus the first load */
static void start_now(void)
{
#ifndef SB_HOST
    if (load() < 0) {
        sb_storage_hold(0);
        if (mo.attended == SBM_ATTENDED_DATA)
            cell_stopped(why, 1);           /* the switch shows why (P3m: it stayed on "Starting") */
        return;
    }
    armed = 1;                              /* the core puts NV back from its pristine copy before the start */
    sb_before_reset = park_hook;
    uint64_t t0 = sb_ms();
    int r = sbm_modem_start(&mo, &plat, &im);
    if (r != 0) {
        sbm_cp_park(&plat);                 /* the core parked it already; safe again */
        armed = 0;
        struct ub u;
        ub_init(&u);
        ub_s(&u, "modem: start failed (");
        ub_s(&u, sbm_strerror(r));
        ub_s(&u, "); parked");
        sb_log(u.b);
        if (mo.attended == SBM_ATTENDED_DATA)
            cell_stopped("The modem did not come online", 1);
        stopped(SBOS_MODEM_FAILED, "The modem did not come online (parked)");
        return;
    }
    snap_gen = 0;
    {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "modem: started in ");
        ub_dec(&u, (uint32_t)(sb_ms() - t0));
        ub_s(&u, mo.b.restart ? " ms (same-boot restart)" : " ms (first start)");
        sb_log(u.b);
    }
#else
    const char *g = getenv("SBOS_HOST_MODEM_GUARD");   /* host: a SIM appears after this many seconds */
    h_guard_s = g ? atoi(g) : -1;
    h_msgs = h_queries = h_answers = 0;
#endif
    t_up = t_poll = sb_ms();
#ifndef SB_HOST
    if (mo.attended == SBM_ATTENDED_DATA)
        sb_log("modem: online for mobile data (the user's Cellular switch): SIM, radio, attach, data");
    else
#endif
        sb_log("modem: online, receive only (status GETs every 10 s; a SIM or a registration parks it)");
    set_state(SBOS_MODEM_ON, "");
    kt_set(KT_MODEM, 2, sb_ms(), POLL_MS, 1);
    kt_set(KT_MODEM, 3, sb_ms(), 3000, 0);
#ifndef SB_HOST
    if (mo.attended == SBM_ATTENDED_DATA)
        cell_begin();
#endif
}

void kmodem_stop(const char *w)
{
    if (!kmodem_on())
        return;
#ifndef SB_HOST
    cell_stopped(w, 0);
    sbm_modem_stop(&mo);                    /* the session's park (a stalled one: parked already; state OFF, so no
                                             * automatic restart follows a stop) */
    sbm_cp_park(&plat);                     /* safe again (and when it never started) */
    armed = 0;
#endif
    struct ub u;
    ub_init(&u);
    ub_s(&u, "modem: stopped (");
    ub_s(&u, w);
    ub_s(&u, "): parked");
    sb_log(u.b);
    stopped(SBOS_MODEM_OFF, w);
}

static void confirmed(int ok)
{
    if (!ok)
        return;
    if (sbos_selftest_enabled || heater_running_ui() || guest_busy()) {
        set_state(SBOS_MODEM_FAILED, "Not now: the self test, Heat or Linux is running");
        return;
    }
    ++starts;
    sb_storage_hold("the modem runs: no storage writes");
    set_state(SBOS_MODEM_STARTING, "");
    kt_set(KT_MODEM, 1, sb_ms(), 300, 0);   /* the Starting page drawn first */
}

/* SYS_MODEM_POWER: 1 the confirmation (the start only on its hold), 0 stop */
int kmodem_power(int on)
{
    if (!on) {
        kmodem_stop("Stopped");
        return 0;
    }
    if (kmodem_on())
        return 0;
#ifndef SB_HOST
    mo.attended = 0;                        /* the Modem app's start is receive only */
#endif
#ifdef SB_HOST
    if (getenv("SBOS_HOST_SELFTEST") || getenv("SBOS_HOST_SELFTEST_GUEST"))
        return SBOS_EAGAIN;
#endif
    if (sbos_selftest_enabled) {
        sb_log("modem: refused: a self-test image never starts the modem");
        return SBOS_EAGAIN;
    }
    if (heater_running_ui() || guest_busy()) {
        set_state(st == SBOS_MODEM_PARKED ? st : SBOS_MODEM_OFF, "Stop Heat and Linux first");
        return SBOS_EAGAIN;
    }
    kui_confirm("Start the modem?", "Receive only: its status, no calls, SMS or data.",
                "A SIM or a network registration parks it.", "The first start is a test: be there.",
                "Hold to start", confirmed);
    return 0;
}

static void query(void)
{
#ifndef SB_HOST
    sbm_modem_query(&mo);
    sbm_modem_status(&mo, line, sizeof(line));
#else
    ++h_queries;
    h_answers += 11;
    h_msgs += 12;
    sb_strcpy(line, sizeof(line), "SIM absent | CS emergency-only (GSM) | PS not registered (GSM) | PLMN set | bars 4 | rssi 0x3e");
#endif
    struct ub u;
    ub_init(&u);
    ub_s(&u, "modem: +");
    ub_dec(&u, (sb_ms() - t_up) / 1000);
    ub_s(&u, " s: ");
    ub_s(&u, line);
    sb_log(u.b);
    notify();
}

static void poll(void)
{
    uint64_t now = sb_ms();
    uint32_t el = (uint32_t)(now - t_poll);
    t_poll = now;
#ifndef SB_HOST
    int r = sbm_modem_poll(&mo, el ? el : 1);
    if (r == 1) {                           /* something the status bar shows changed */
        struct sbm_modem_snap sn;
        sbm_modem_snapshot(&mo, &sn);
        if (sn.gen != snap_gen) {
            snap_gen = sn.gen;
            notify();
        }
        return;
    }
    if (r == 0)
        return;
    if (mo.state == SBM_MS_STALLED || mo.state == SBM_MS_CRASHED) {
        /* parked by the core; Restarting... until sbm_modem_recover says yes (5 s) or no (3 in 30 min) */
        struct ub u;
        ub_init(&u);
        ub_s(&u, "modem: down (");
        ub_s(&u, sbm_strerror(r));
        ub_s(&u, mo.state == SBM_MS_STALLED ? ": the CP stopped answering" : ": the CP crashed");
        ub_s(&u, "); parked, restarting in 5 s");
        sb_log(u.b);
        kt_cancel_owner(KT_MODEM);
        t_rec = sb_ms();
        if (mo.attended == SBM_ATTENDED_DATA) {
            cell_net_down(mo.state == SBM_MS_STALLED ? "the modem stopped answering" : "the modem crashed");
            cset(KCELL_STARTING, "Restarting the modem");
        }
        set_state(SBOS_MODEM_STARTING, mo.state == SBM_MS_STALLED ? "Restarting: the modem stopped answering"
                                                                  : "Restarting: the modem crashed");
        kt_set(KT_MODEM, 4, sb_ms(), RECOVER_MS, 1);
        return;
    }
    armed = 0;
    sb_log(mo.state == SBM_MS_GUARD ? "modem: a guard parked the CP (a SIM or a registration)"
                                    : "modem: poll stopped; parked");
    stopped(SBOS_MODEM_PARKED, mo.state == SBM_MS_GUARD ? "Parked: a SIM or a network registration (receive only)"
                                                        : "Parked: the link reported an error");
    notif_post("modem", "Modem parked", why);
#else
    (void)el;
    if (h_guard_s >= 0 && now - t_up >= (uint64_t)h_guard_s * 1000U) {
        sb_log("modem: a guard parked the CP (a SIM or a registration)");
        stopped(SBOS_MODEM_PARKED, "Parked: a SIM or a network registration (receive only)");
        notif_post("modem", "Modem parked", why);
    }
#endif
}

#ifndef SB_HOST
/* timer id 4, every second while STALLED / CRASHED: the core restarts it (blocking, about 5 s) once allowed */
static void recover(void)
{
    uint64_t now = sb_ms();
    uint32_t el = (uint32_t)(now - t_rec);
    t_rec = now;
    int r = sbm_modem_recover(&mo, el ? el : 1);
    if (r == 1)
        return;                             /* not yet: the 5 s gap */
    kt_cancel_owner(KT_MODEM);
    if (r < 0) {
        armed = 0;
        sbm_cp_park(&plat);
        struct ub u;
        ub_init(&u);
        ub_s(&u, "modem: no restart (");
        ub_s(&u, sbm_strerror(r));
        ub_s(&u, "); parked, stays off");
        sb_log(u.b);
        cell_stopped("The modem kept failing: stopped", 1);
        stopped(SBOS_MODEM_FAILED, r == SBM_EPERM ? "Stopped: it went down 3 times in 30 min (parked)"
                                                  : "The modem did not come back (parked)");
        notif_post("modem", "Modem stopped", why);
        return;
    }
    sb_log("modem: restarted after a stall / crash (Android's reset path)");
    if (mo.attended == SBM_ATTENDED_DATA)
        cell_net_down("the modem restarted");
    snap_gen = 0;
    t_up = t_poll = sb_ms();
    set_state(SBOS_MODEM_ON, "");
    kt_set(KT_MODEM, 2, sb_ms(), POLL_MS, 1);
    kt_set(KT_MODEM, 3, sb_ms(), 3000, 0);
    if (mo.attended == SBM_ATTENDED_DATA)
        cell_begin();
}

/* ---- mobile data (Next): the user's Settings > Cellular switch ---------------------------------------------
 * The only path on which this kernel transmits. Off by default at every boot; never calls, SMS or SS (the core
 * cannot build them), never an emergency call. Steps (simtest-a2, 2026-11-11, the user's SIM): the SIM ready,
 * radio normal, the LTE initial-attach APN (without it the CP camps and never attaches), registration (with
 * network selection automatic, PS attach and a radio cycle if it takes long), the PDP context (cid 1, IPv4),
 * then a raw-IP sbnet + sbtcp (starbite/wifi) that wifi_fetch uses when Wi-Fi is not joined. Addresses, the
 * operator code and its name never reach the log; the name is for the screen. */
enum { CS_IDLE, CS_SIM, CS_RADIO, CS_ATTACH, CS_REG, CS_CYCLE_OFF, CS_CYCLE_ON, CS_DEFINE, CS_ACT, CS_IP, CS_UP,
       CS_RETRY, CS_DEACT, CS_DETACH };
#define CELL_APN "internet"                /* Elisa, DNA, Telia (FI); others: a setting later */
static int cell_want, cst = KCELL_OFF, cstep = CS_IDLE, c_lf, c_esc, cnet_up, c_check, c_ntp, c_sms_ready;
static uint64_t c_ntp_t;
static uint8_t c_ntp_nonce[8], c_ntp_server[4];
#define C_NTP_PORT 50124
static uint64_t c_t0, c_step_t;
static char c_why[64];
static struct sbnet cnet;
static struct sbtcp ctcp;
static uint32_t c_rng = 0x9E3779B9u, c_tx, c_rx;

static void cset(int s, const char *w)
{
    if (s != cst || (w && !sb_streq(w, c_why))) {
        cst = s;
        sb_strcpy(c_why, sizeof(c_why), w ? w : "");
        struct ub u;
        ub_init(&u);
        ub_s(&u, "cell: ");
        ub_s(&u, c_why[0] ? c_why : "state");
        sb_log(u.b);
        ui_mark_dirty();
    }
}
static void cgo(int step)
{
    cstep = step;
    c_step_t = sb_ms();
}
static uint32_t cin(void) { return (uint32_t)(sb_ms() - c_step_t); }

static int c_tx_ip(void *ctx, const uint8_t *pkt, uint32_t len)
{
    (void)ctx;
    int r = sbm_modem_ip_tx(&mo, pkt, len);
    if (r == 0)
        c_tx++;
    return r < 0 ? -1 : 0;
}
static void c_rx_ip(void *ctx, const uint8_t *pkt, uint32_t len)
{
    (void)ctx;
    c_rx++;
    if (cnet_up)
        sbnet_rx_ip(&cnet, pkt, len);
}
static uint64_t c_now(void *ctx) { (void)ctx; return sb_ms(); }
static uint32_t c_random(void *ctx)
{
    (void)ctx;
    c_rng ^= (uint32_t)sb_ms() * 2654435761u;
    c_rng ^= c_rng << 13, c_rng ^= c_rng >> 17, c_rng ^= c_rng << 5;
    return c_rng;
}
static void c_tcp_in(void *ctx, const uint8_t from[4], const uint8_t *seg, uint32_t len)
{
    (void)ctx;
    sbtcp_input(&ctcp, from, seg, len);
}
/* the NTP answer over mobile data (as wifi.c's: the server asked, our nonce, synchronised, after 2020) */
static uint32_t c_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void c_udp(void *ctx, uint16_t port, const uint8_t from[4], uint16_t from_port, const uint8_t *d, uint32_t len)
{
    (void)ctx;
    if (port != C_NTP_PORT || from_port != 123 || c_ntp != 2 || len < 48 || (d[0] & 7) != 4 ||
        memcmp(from, c_ntp_server, 4) || memcmp(d + 24, c_ntp_nonce, 8) || (d[0] >> 6) == 3 || d[1] == 0 || d[1] >= 16)
        return;
    uint32_t secs = c_be32(d + 40);
    if (secs < 2208988800U + 1577836800U)
        return;
    uint64_t rtt = sb_ms() - c_ntp_t;
    int64_t utc = (int64_t)(secs - 2208988800U) + (int64_t)((rtt / 2 + 500) / 1000U);
    int was_set = 0;
    int64_t d_s = clock_set_net(utc, &was_set);
    wifi_clock_checked_by(was_set ? 3 : d_s ? 2 : 1);
    c_ntp = 3;
    sb_log(was_set ? "cell: the clock was set over mobile data (NTP)" : d_s ? "cell: the clock was corrected over mobile data (NTP)"
                                                                           : "cell: the clock was checked over mobile data (NTP): right");
}

/* NTP over mobile data before the first HTTPS (certificates need a checked clock when Wi-Fi never ran):
 * 0 not yet, 1 DNS, 2 asked, 3 done, -1 no answer */
static void c_ntp_step(void)
{
    if (c_ntp == 0) {
        c_ntp = sbnet_dns_query(&cnet, "pool.ntp.org") == 0 ? 1 : -1;
        c_ntp_t = sb_ms();
    } else if (c_ntp == 1 && cnet.dns_state == SBNET_DNS_DONE) {
        uint8_t q[48];
        memset(q, 0, sizeof(q));
        q[0] = 0x23;                         /* NTP version 4, client */
        for (unsigned i = 0; i < 8; i += 4) {
            uint32_t r = c_random(0);
            memcpy(c_ntp_nonce + i, &r, 4);
        }
        memcpy(q + 40, c_ntp_nonce, 8);
        memcpy(c_ntp_server, cnet.dns_result, 4);
        c_ntp_t = sb_ms();
        c_ntp = sbnet_udp_send(&cnet, cnet.dns_result, 123, C_NTP_PORT, q, sizeof(q)) == 0 ? 2 : -1;
    } else if ((c_ntp == 1 && cnet.dns_state == SBNET_DNS_FAILED) || ((c_ntp == 1 || c_ntp == 2) && sb_ms() - c_ntp_t > 8000)) {
        c_ntp = -1;
        sb_log("cell: no NTP answer over mobile data (HTTPS needs a checked clock)");
    }
}

static void c_log(void *ctx, const char *l)
{
    (void)ctx;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "cell: ");
    ub_s(&u, l);
    sb_log(u.b);
}

static void cell_net_down(const char *w)
{
    if (!cnet_up)
        return;
    cnet_up = 0;
    sbtcp_down(&ctcp);
    sbnet_stop(&cnet);
    sbm_modem_ip_rx_cb(0, 0);
    kt_cancel(KT_MODEM, 6);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "cell: mobile data down (");
    ub_s(&u, w);
    ub_s(&u, ")");
    sb_log(u.b);
}

static void cell_net_up(void)
{
    static const uint8_t host32[4] = { 255, 255, 255, 255 };
    struct sbnet_os os;
    memset(&os, 0, sizeof(os));
    os.tx = c_tx_ip, os.now_ms = c_now, os.random = c_random, os.tcp = c_tcp_in, os.log = c_log, os.udp = c_udp;
    sbnet_init_raw(&cnet, &os);
    sbnet_static(&cnet, mo.b.pdp.ip, host32, mo.b.pdp.gw, mo.b.pdp.dns1);
    /* the PDP's MTU when the CP gave one (IP_CONFIGURATION flag bit 13), else sbnet's raw default (1400) */
    if ((mo.b.pdp.ip_flags & (1u << 13)) && mo.b.pdp.mtu >= 576 && mo.b.pdp.mtu <= 1500)
        cnet.mtu = mo.b.pdp.mtu;
    sbtcp_init(&ctcp, &cnet);
    sbm_modem_ip_rx_cb(c_rx_ip, 0);
    cnet_up = 1;
    kt_set(KT_MODEM, 6, sb_ms(), 20, 1);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "cell: mobile data up (address and DNS not logged), MTU ");
    ub_dec(&u, mo.b.pdp.mtu);
    sb_log(u.b);
}

/* the modem stopped (the switch, Heat, Linux, a power path, a failure): mobile data ends with it */
static void cell_stopped(const char *w, int failed)
{
    cell_net_down(w);
    if (mo.attended != SBM_ATTENDED_DATA)
        return;
    cell_want = 0;
    cgo(CS_IDLE);
    if (failed)
        cset(KCELL_FAILED, w);
    else if (cst != KCELL_NO_SIM && cst != KCELL_SIM_LOCKED && cst != KCELL_FAILED)
        cset(KCELL_OFF, sb_streq(w, "Cellular off") ? "Off" : w);
}

static void comms_begin(void);
static void comms_tick(void);
static void cell_begin(void)
{
    c_lf = c_esc = 0;
    c_sms_ready = 0;
    comms_begin();
    cset(KCELL_STARTING, "Starting the modem");
    cgo(CS_SIM);
    kt_set(KT_MODEM, 5, sb_ms(), 100, 1);
}

static int c_registered(const struct sbm_modem_snap *sn)
{
    return sn->cs == SBM_REG_HOME || sn->cs == SBM_REG_ROAMING || sn->ps == SBM_REG_HOME || sn->ps == SBM_REG_ROAMING;
}

static void cell_step(void)
{
    struct sbm_modem_snap sn;
    if (!mo.up)
        return;                             /* down: poll()'s restart (or a stop) decides */
    comms_tick();
    sbm_modem_snapshot(&mo, &sn);
    switch (cstep) {
    case CS_SIM:
        if (sn.sim == SBM_SIM_ABSENT && cin() > 8000) {
            cset(KCELL_NO_SIM, "No SIM card");
            cell_want = 0;
            cgo(CS_IDLE);
            kmodem_stop("No SIM card");
        } else if (sn.sim == SBM_SIM_LOCK_SC) {
            cset(KCELL_SIM_LOCKED, mo.b.cell.sim_key == 2 ? "The SIM is PUK-locked" :
                 "The SIM's PIN is on (PIN entry is not built yet)");
            cell_want = 0;
            cgo(CS_IDLE);
            kmodem_stop("SIM locked");
        } else if (sn.sim == SBM_SIM_READY || sn.sim == SBM_SIM_INIT_COMPLETE || sn.sim == SBM_SIM_PB_INIT_COMPLETE) {
            cset(KCELL_SEARCHING, "Turning the radio on");
            sbm_modem_radio(&mo, 1);
            cgo(CS_RADIO);
        } else if (cin() > 60000) {
            cset(KCELL_FAILED, "The SIM did not answer");
            cell_want = 0;
            cgo(CS_IDLE);
            kmodem_stop("The SIM did not answer");
        }
        break;
    case CS_RADIO:
        if (mo.b.radio_res_seen || cin() > 5000) {
            sbm_modem_attach_apn(&mo, CELL_APN, 0x06, c_lf);
            cgo(CS_ATTACH);
        }
        break;
    case CS_ATTACH:
        if (mo.b.ctl_seen || cin() > 10000) {
            if ((!mo.b.ctl_seen || mo.b.ctl_res != 0x8000) && !c_lf) {
                c_lf = 1;                   /* the long form once */
                sbm_modem_attach_apn(&mo, CELL_APN, 0x06, 1);
                cgo(CS_ATTACH);
                break;
            }
            cset(KCELL_SEARCHING, "Searching for the network");
            c_t0 = sb_ms();
            c_esc = 0;
            cgo(CS_REG);
        }
        break;
    case CS_REG:
        if (c_registered(&sn)) {
            cset(KCELL_REGISTERED, sn.cs == SBM_REG_ROAMING || sn.ps == SBM_REG_ROAMING ? "Registered (roaming)"
                                                                                       : "Registered");
            if (!c_sms_ready) {
                c_sms_ready = 1;
                sbm_modem_sms_ready(&mo);   /* the AP takes SMS (and the SIM's SMSC) */
            }
            sbm_modem_pdp_define(&mo, CELL_APN);
            cgo(CS_DEFINE);
        } else if (c_esc == 0 && sb_ms() - c_t0 > 60000) {
            c_esc = 1;
            sbm_modem_net_auto(&mo);
        } else if (c_esc == 1 && sb_ms() - c_t0 > 100000) {
            c_esc = 2;
            sbm_modem_ps_attach(&mo);
        } else if (c_esc == 2 && sb_ms() - c_t0 > 140000) {
            c_esc = 3;
            sbm_modem_radio(&mo, 0);         /* as airplane mode on / off, then the attach APN again */
            cgo(CS_CYCLE_OFF);
        }
        break;
    case CS_CYCLE_OFF:
        if (cin() > 5000) {
            sbm_modem_radio(&mo, 1);
            cgo(CS_CYCLE_ON);
        }
        break;
    case CS_CYCLE_ON:
        if (cin() > 2000) {
            c_lf = 0;
            sbm_modem_attach_apn(&mo, CELL_APN, 0x06, 0);
            cgo(CS_ATTACH);
        }
        break;
    case CS_DEFINE:
        if (mo.b.pdp.define_seen || cin() > 10000) {
            if (!mo.b.pdp.define_seen || mo.b.pdp.define_res != 0x8000) {
                cset(KCELL_REGISTERED, "The network refused the data context: trying again in 30 s");
                cgo(CS_RETRY);
                break;
            }
            c_lf = 0;
            sbm_modem_pdp_activate(&mo, 1, 0);
            cgo(CS_ACT);
        }
        break;
    case CS_ACT:
        if (mo.b.pdp.act_seen || cin() > 10000) {
            if ((!mo.b.pdp.act_seen || mo.b.pdp.act_res != 0x8000) && !c_lf) {
                c_lf = 1;
                sbm_modem_pdp_activate(&mo, 1, 1);
                cgo(CS_ACT);
                break;
            }
            cgo(CS_IP);
        }
        break;
    case CS_IP:
        if (mo.b.pdp.have_ip && mo.b.pdp.have_status && mo.b.pdp.status == 1) {
            cell_net_up();
            cset(KCELL_DATA, "Mobile data on");
            c_check = 0;
            c_ntp = 0;
            cgo(CS_UP);
        } else if ((mo.b.pdp.have_status && mo.b.pdp.status != 1 && mo.b.pdp.status_notis) || cin() > 30000) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "No data connection (cause ");
            ub_dec(&u, mo.b.pdp.cause);
            ub_s(&u, "): trying again in 30 s");
            cset(KCELL_REGISTERED, u.b);
            cgo(CS_RETRY);
        }
        break;
    case CS_UP:
        /* once per session, as Android's connectivity check: one HTTPS GET over mobile data (the fetch path the
         * browser uses), its result in the log (steps, status, sizes and times; never the address) and a
         * notification */
        if (c_ntp >= 0 && c_ntp < 3)
            c_ntp_step();
        if (c_check == 0 && cin() > 1500 && (c_ntp == 3 || c_ntp < 0)) {
            c_check = wifi_fetch("https://example.com/") == 0 ? 1 : 3;
        } else if (c_check == 1) {
            struct sbos_wifi_fetch f;
            wifi_fetch_status(&f);
            if (f.state == 2) {
                struct ub u;
                ub_init(&u);
                ub_s(&u, "cell: HTTPS check over mobile data: ");
                ub_s(&u, f.result == 1 && f.status == 200 ? "PASS" : "FAIL");
                ub_s(&u, ", status ");
                ub_dec(&u, (uint32_t)(f.status < 0 ? 0 : f.status));
                ub_s(&u, ", TLS ");
                ub_dec(&u, f.tls_version);
                ub_s(&u, ", ");
                ub_dec(&u, f.bytes);
                ub_s(&u, " bytes; DNS ");
                ub_dec(&u, f.dns_ms);
                ub_s(&u, " ms, connect ");
                ub_dec(&u, f.connect_ms);
                ub_s(&u, " ms, TLS ");
                ub_dec(&u, f.tls_ms);
                ub_s(&u, " ms, total ");
                ub_dec(&u, f.total_ms);
                ub_s(&u, " ms (");
                ub_s(&u, f.why);
                ub_s(&u, ")");
                sb_log(u.b);
                notif_post("modem", "Mobile data", f.result == 1 && f.status == 200 ? "The Internet works over LTE (HTTPS)"
                                                                                  : "Connected, but the HTTPS check failed");
                c_check = 2;
            }
        }
        if (!c_registered(&sn) || (mo.b.pdp.have_status && mo.b.pdp.status != 1)) {
            cell_net_down(c_registered(&sn) ? "the data context ended" : "the registration was lost");
            mo.b.pdp.have_ip = 0;
            mo.b.pdp.have_status = 0;
            cset(KCELL_SEARCHING, "Reconnecting");
            c_t0 = sb_ms();
            c_esc = 3;                       /* no escalation on a reconnect: the CP tries by itself */
            cgo(CS_REG);
        }
        break;
    case CS_RETRY:
        if (cin() > 30000) {
            if (c_registered(&sn)) {
                sbm_modem_pdp_define(&mo, CELL_APN);
                cgo(CS_DEFINE);
            } else {
                c_t0 = sb_ms();
                cgo(CS_REG);
            }
        }
        break;
    case CS_DEACT:
        if (mo.b.pdp.act_seen || cin() > 5000) {
            sbm_modem_radio(&mo, 0);         /* low power: the CP detaches, then RF off */
            cgo(CS_DETACH);
        }
        break;
    case CS_DETACH:
        if ((mo.b.radio_res_seen && !c_registered(&sn)) || cin() > 5000) {
            cgo(CS_IDLE);
            kmodem_stop("Cellular off");
            cset(KCELL_OFF, "Off");
        }
        break;
    default:
        break;
    }
}

/* boot (main.c, before the UI's heap): the modem's 2.25 MiB taken from the arena now, so the Cellular switch still
 * works when the UI, Wi-Fi and the GPU have used the rest (P3m: 54 KiB left). No partition is read. */
void kmodem_reserve(void)
{
    uint64_t f0 = sb_arena_free();
    int r = sbm_sb_reserve(0x100000);       /* nv_data.bin is 1 MiB on this phone (RADIO's plan checks it) */
    struct ub u;
    ub_init(&u);
    ub_s(&u, r == 0 ? "modem: reserved at boot: " : "modem: could not reserve its memory at boot: ");
    ub_size(&u, sbm_sb_reserved());
    ub_s(&u, " (arena free before ");
    ub_size(&u, f0);
    ub_s(&u, ", after ");
    ub_size(&u, sb_arena_free());
    ub_s(&u, ")");
    sb_log(u.b);
}

int kmodem_cellular(int on)
{
    if (!on) {
        cell_want = 0;
        if (!kmodem_on() || mo.attended != SBM_ATTENDED_DATA) {
            cset(KCELL_OFF, "Off");
            return 0;
        }
        cell_net_down("Cellular off");
        if (mo.up && cstep != CS_IDLE && cstep != CS_SIM) {
            cset(KCELL_STOPPING, "Turning off: data off, detaching");
            sbm_modem_pdp_activate(&mo, 0, 0);
            cgo(CS_DEACT);
            kt_set(KT_MODEM, 5, sb_ms(), 100, 1);
        } else {
            cgo(CS_IDLE);
            kmodem_stop("Cellular off");
            cset(KCELL_OFF, "Off");
        }
        return 0;
    }
    if (cell_want && kmodem_on())
        return 0;
    if (sbos_selftest_enabled)
        return SBOS_EAGAIN;
    if (heater_running_ui() || guest_busy()) {
        cset(KCELL_FAILED, "Stop Heat and Linux first");
        return SBOS_EAGAIN;
    }
    if (kmodem_on())
        kmodem_stop("Cellular on");         /* a receive-only session: restarted for data */
    cell_want = 1;
    mo.attended = SBM_ATTENDED_DATA;        /* the user's switch: the only place this kernel sets it */
    cset(KCELL_STARTING, "Starting the modem");
    ++starts;
    sb_storage_hold("the modem runs: no storage writes");
    set_state(SBOS_MODEM_STARTING, "");
    kt_set(KT_MODEM, 1, sb_ms(), 50, 0);
    return 0;
}

int kmodem_netif(struct sbnet **net, struct sbtcp **tcp)
{
    if (!cnet_up || cst != KCELL_DATA)
        return 0;
    if (net)
        *net = &cnet;
    if (tcp)
        *tcp = &ctcp;
    return 1;
}

void kmodem_cell(struct kcell *c)
{
    struct sbm_modem_snap sn;
    memset(c, 0, sizeof(*c));
    c->state = cst;
    c->want = cell_want;
    c->bars = -1;
    sb_strcpy(c->why, sizeof(c->why), c_why);
    c->tx_packets = c_tx;
    c->rx_packets = c_rx;
    if (!kmodem_on() || mo.attended != SBM_ATTENDED_DATA || !mo.up)
        return;
    sbm_modem_snapshot(&mo, &sn);
    c->bars = sn.bars == 0xFF ? -1 : sn.bars;
    c->rssi_dbm = sn.rssi_dbm;
    c->roaming = sn.cs == SBM_REG_ROAMING || sn.ps == SBM_REG_ROAMING;
    if (sn.rat)
        sb_strcpy(c->rat, sizeof(c->rat), sbm_act_name(sn.rat));
    if (sn.have_plmn && c_registered(&sn)) {
        const char *n = sbm_operator_name(sn.plmn);
        if (n)
            sb_strcpy(c->oper, sizeof(c->oper), n);
        else {
            char code[8];
            unsigned k = 0;
            for (unsigned i = 0; sn.plmn[i] && k < 7; ++i)
                if (sn.plmn[i] >= '0' && sn.plmn[i] <= '9')
                    code[k++] = sn.plmn[i];
            code[k] = 0;
            sb_strcpy(c->oper, sizeof(c->oper), code);   /* the screen only */
        }
    }
}

/* ---- calls + SMS (Next's Phone / Messages; docs/MODEM.md 7.11) ---------------------------------------------------
 * Only while Cellular is on and registered (or data up). The user typed the number and held to confirm: the policy's
 * user_confirmed; the two locks (emergency numbers: the world list + the CP's own; short codes) are checked in the
 * core twice and here first, for the words. Self-test images refuse all of it (a dry run). Numbers and text stay
 * in these RAM structures and the screen; the log has steps and counts only. The in-call audio path (ABOX / codec /
 * amp) is not built yet: audio_ok stays 0 and the UI says so. */
static struct sbm_dial_policy c_pol = { { 0 }, 0, 0, 0, SBM_DIAL_USER_CONFIRMED };
static struct {
    int pending, audio, muted, ended, dtmf_on;
    uint64_t t_dial, t_active, t_ended, t_buzz;
    uint32_t gen_seen;
    int ring_seen;
    char number[24], why[48];
} kc;
static struct ksms kmsg[KSMS_MAX];
static int nkmsg, ksend = -1;
static uint32_t kgen, kmsg_id, kinbox_seen;
static uint8_t ksms_ref;
static struct sbm_inbox kinbox;

static int comms_ready(void)
{
    return kmodem_on() && mo.attended == SBM_ATTENDED_DATA && mo.up &&
           (cst == KCELL_REGISTERED || cst == KCELL_DATA);
}

static const char *end_words(uint8_t cls, uint8_t cause)
{
    if (cls == 0 || cls == 1)
        switch (cause) {
        case 1: return "Number not in use";
        case 5: return "Call ended";
        case 6: return "Busy";
        case 7:
        case 8: return "No answer";
        case 9: return "Call declined";
        case 17: return "Network busy";
        default: break;
        }
    return "Call ended";
}

/* SCTS (BCD, nibbles swapped: yy mm dd hh mi ss tz) -> Unix time */
static int64_t scts_utc(const uint8_t t[7])
{
    int v[7];
    for (int i = 0; i < 7; ++i)
        v[i] = (t[i] & 0x0F) * 10 + ((t[i] >> 4) & (i == 6 ? 0x07 : 0x0F));
    int y = 2000 + v[0], m = v[1], d = v[2];
    if (m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    y -= m <= 2;
    int era = y / 400, yoe = y - era * 400, mp = (m + 9) % 12;
    int doy = (153 * mp + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    int tz = v[6] * 15 * 60 * ((t[6] & 0x08) ? -1 : 1);
    return days * 86400 + v[3] * 3600 + v[4] * 60 + v[5] - tz;
}

static struct ksms *kmsg_new(void)
{
    if (nkmsg == KSMS_MAX) {
        memmove(&kmsg[0], &kmsg[1], (KSMS_MAX - 1) * sizeof(kmsg[0]));
        nkmsg--;
        if (ksend >= 0)
            ksend--;
    }
    struct ksms *k = &kmsg[nkmsg++];
    memset(k, 0, sizeof(*k));
    k->id = (int32_t)++kmsg_id;
    kgen++;
    return k;
}

static void kinbox_take(void)
{
    for (unsigned i = 0; i < kinbox.n; ++i) {
        const struct sbm_inbox_msg *m = &kinbox.m[i];
        if (m->id <= kinbox_seen)
            continue;
        kinbox_seen = m->id;
        struct ksms *k = kmsg_new();
        k->incoming = 1;
        k->state = 0;
        k->utc = scts_utc(m->scts);
        sb_strcpy(k->number, sizeof(k->number), m->from);
        sb_strcpy(k->text, sizeof(k->text), m->text);
        sb_log("comms: an SMS received (sender and text not logged)");
        if (vib_ready())
            vib_pulse(300);                          /* a short buzz for a new message */
        ui_mark_dirty();
    }
}

static void c_sms_rx(void *ctx, const struct sbm_sms *s)
{
    (void)ctx;
    sbm_inbox_add(&kinbox, s, (uint32_t)sb_ms());
    kinbox_take();
}

static void comms_begin(void)
{
    memset(&kc, 0, sizeof(kc));
    kc.audio = KAUDIO_EARPIECE;
    sbm_modem_sms_rx_cb(c_sms_rx, 0);
}

static void comms_tick(void)
{
    struct sbm_calls *c = &mo.b.calls;
    uint64_t now = sb_ms();
    sbm_inbox_add(&kinbox, 0, (uint32_t)now);        /* parts given up after 10 min */
    kinbox_take();
    if (kc.dtmf_on && ++kc.dtmf_on > 2) {
        sbm_modem_call_dtmf(&mo, 0, 0);              /* stop the tone after about 200 ms (two ticks) */
        kc.dtmf_on = 0;
    }
    if (c->gen != kc.gen_seen) {
        kc.gen_seen = c->gen;
        if (c->n && !c->c[0].outgoing && (c->c[0].state == SBM_CALL_INCOMING_ST || c->c[0].state == SBM_CALL_WAITING_ST)) {
            if (!kc.ring_seen) {
                kc.ring_seen = 1;
                kc.ended = 0;
                sb_log("comms: an incoming call (number not logged)");
                power_screen_set(1);                 /* the incoming-call screen, over the lock too */
            }
        } else {
            if (kc.ring_seen)
                vib_off();                           /* answered, declined or gone: stop the ringing buzz */
            kc.ring_seen = 0;
        }
        if (c->n && c->c[0].state == SBM_CALL_ACTIVE && !kc.t_active)
            kc.t_active = now;
        if (c->n)
            kc.pending = 0;
        if (c->end_seen && !c->n) {
            c->end_seen = 0;
            kc.ended = 1;
            kc.t_ended = now;
            kc.t_active = 0;
            kc.pending = 0;
            sb_strcpy(kc.why, sizeof(kc.why), end_words(c->end_class, c->end_cause));
            sb_log("comms: the call ended");
        }
        if (c->dial_seen && c->dial_res != 0x8000 && c->dial_res != 0x8001 && kc.pending) {
            kc.pending = 0;
            kc.ended = 1;
            kc.t_ended = now;
            sb_strcpy(kc.why, sizeof(kc.why), "The call could not be placed");
        }
        ui_mark_dirty();
    }
    /* an incoming call rings as a buzz (no ringtone yet: no audio path): 1 s on, 2 s off while it rings */
    if (kc.ring_seen && now - kc.t_buzz >= 3000) {
        kc.t_buzz = now;
        if (vib_ready())
            vib_pulse(1000);
    }
    if (kc.pending && now - kc.t_dial > 30000) {
        kc.pending = 0;                              /* no answer from the CP at all */
        kc.ended = 1;
        kc.t_ended = now;
        sb_strcpy(kc.why, sizeof(kc.why), "The call could not be placed");
        ui_mark_dirty();
    }
    if (kc.ended && now - kc.t_ended > 4000) {
        kc.ended = 0;
        ui_mark_dirty();
    }
    if (ksend >= 0 && ksend < nkmsg) {               /* the send in flight: all parts answered? */
        struct sbm_smsq *q = &mo.b.sms;
        int done = 1, ok = 1;
        for (unsigned i = 0; i < q->parts; ++i) {
            done &= q->done[i];
            ok &= q->done[i] && q->res[i] == 0;
        }
        if (done) {
            kmsg[ksend].state = ok ? 2 : 3;
            if (!ok)
                sb_strcpy(kmsg[ksend].why, sizeof(kmsg[ksend].why), "Not sent: the network refused it");
            sb_log(ok ? "comms: the SMS was sent" : "comms: the SMS was not sent");
            ksend = -1;
            kgen++;
            ui_mark_dirty();
        }
    }
}

static int refuse_words(const char *number, const char **why)
{
    if (sbm_is_emergency(number, mo.b.calls.ecc_p, mo.b.calls.n_ecc)) {
        *why = "Emergency calls are not supported on Starbite OS: use another phone";
        return SBOS_EPERM;
    }
    unsigned digits = 0;
    for (const char *p = number; *p; ++p)
        digits += *p >= '0' && *p <= '9';
    if (digits < SBM_DIAL_MIN_DIGITS) {
        *why = "Short numbers are not supported";
        return SBOS_EPERM;
    }
    return 0;
}

int kmodem_call_dial(const char *number)
{
    const char *why = 0;
    if (sbos_selftest_enabled || !number)
        return SBOS_EAGAIN;                          /* the self test: a dry run */
    int r = refuse_words(number, &why);
    if (r < 0) {
        kc.ended = 1;
        kc.t_ended = sb_ms();
        sb_strcpy(kc.why, sizeof(kc.why), why);
        ui_mark_dirty();
        return r;
    }
    if (!comms_ready())
        return SBOS_EAGAIN;
    if (mo.b.calls.n || kc.pending)
        return SBOS_EBUSY;
    r = sbm_modem_call_dial(&mo, number, &c_pol);
    if (r < 0)
        return r == SBM_EPERM ? SBOS_EPERM : SBOS_EAGAIN;
    memset(kc.why, 0, sizeof(kc.why));
    sb_strcpy(kc.number, sizeof(kc.number), number);
    kc.pending = 1;
    kc.ended = 0;
    kc.t_dial = sb_ms();
    kc.t_active = 0;
    kc.muted = 0;
    kc.audio = KAUDIO_EARPIECE;
    sbm_modem_snd(&mo, 0, 4, 0);                     /* the CP's side of the earpiece path (no AP audio yet) */
    ui_mark_dirty();
    return 0;
}

int kmodem_call_answer(void)
{
    if (sbos_selftest_enabled)
        return SBOS_EAGAIN;
    if (!comms_ready() || !mo.b.calls.n || mo.b.calls.c[0].outgoing)
        return SBOS_EAGAIN;
    kc.audio = KAUDIO_EARPIECE;
    kc.muted = 0;
    sbm_modem_snd(&mo, 0, 4, 0);
    return sbm_modem_call_answer(&mo) < 0 ? SBOS_EAGAIN : 0;
}

int kmodem_call_hangup(void)
{
    if (sbos_selftest_enabled)
        return SBOS_EAGAIN;
    if (!comms_ready() || (!mo.b.calls.n && !kc.pending))
        return SBOS_EAGAIN;
    return sbm_modem_call_hangup(&mo, 0) < 0 ? SBOS_EAGAIN : 0;
}

int kmodem_call_route(int audio)
{
    if (sbos_selftest_enabled || !comms_ready() || !mo.b.calls.n)
        return SBOS_EAGAIN;
    kc.audio = audio == KAUDIO_SPEAKER ? KAUDIO_SPEAKER : KAUDIO_EARPIECE;
    ui_mark_dirty();
    return sbm_modem_snd(&mo, kc.audio == KAUDIO_SPEAKER, 4, kc.muted) < 0 ? SBOS_EAGAIN : 0;
}

int kmodem_call_mute(int on)
{
    if (sbos_selftest_enabled || !comms_ready() || !mo.b.calls.n)
        return SBOS_EAGAIN;
    kc.muted = !!on;
    ui_mark_dirty();
    return sbm_modem_snd(&mo, kc.audio == KAUDIO_SPEAKER, 4, kc.muted) < 0 ? SBOS_EAGAIN : 0;
}

int kmodem_call_dtmf(char digit)
{
    if (sbos_selftest_enabled || !comms_ready() || !mo.b.calls.n || mo.b.calls.c[0].state != SBM_CALL_ACTIVE)
        return SBOS_EAGAIN;
    if (!((digit >= '0' && digit <= '9') || digit == '*' || digit == '#'))
        return SBOS_EINVAL;
    if (sbm_modem_call_dtmf(&mo, digit, 1) < 0)
        return SBOS_EAGAIN;
    kc.dtmf_on = 1;
    return 0;
}

void kmodem_call(struct kcall *k)
{
    memset(k, 0, sizeof(*k));
    k->audio = kc.audio;
    k->muted = kc.muted;
    k->audio_ok = 0;
    const struct sbm_calls *c = &mo.b.calls;
    if (comms_ready() && c->n) {
        const struct sbm_call *x = &c->c[0];
        k->outgoing = x->outgoing;
        sb_strcpy(k->number, sizeof(k->number), x->number[0] ? x->number : kc.number);
        k->state = x->state == SBM_CALL_ACTIVE ? KCALL_ACTIVE : x->state == SBM_CALL_HELD ? KCALL_HELD :
                   x->state == SBM_CALL_DIALING ? KCALL_DIALING : x->state == SBM_CALL_ALERTING ? KCALL_ALERTING :
                   KCALL_INCOMING;
        if (k->state == KCALL_ACTIVE && kc.t_active)
            k->secs = (int32_t)((sb_ms() - kc.t_active) / 1000);
        return;
    }
    if (kc.pending) {
        k->state = KCALL_DIALING;
        k->outgoing = 1;
        sb_strcpy(k->number, sizeof(k->number), kc.number);
        return;
    }
    if (kc.ended) {
        k->state = KCALL_ENDED;
        sb_strcpy(k->number, sizeof(k->number), kc.number);
        sb_strcpy(k->why, sizeof(k->why), kc.why);
    }
}

int kmodem_sms_send(const char *number, const char *utf8)
{
    const char *why = 0;
    if (sbos_selftest_enabled || !number || !utf8 || !*utf8)
        return SBOS_EAGAIN;
    int r = refuse_words(number, &why);
    struct ksms *k;
    if (r < 0) {
        k = kmsg_new();
        k->state = 3;
        sb_strcpy(k->number, sizeof(k->number), number);
        sb_strcpy(k->text, sizeof(k->text), utf8);
        sb_strcpy(k->why, sizeof(k->why), why);
        ui_mark_dirty();
        return r;
    }
    if (!comms_ready())
        return SBOS_EAGAIN;
    if (ksend >= 0)
        return SBOS_EBUSY;
    r = sbm_modem_sms_send(&mo, number, utf8, &c_pol, ++ksms_ref);
    if (r < 0)
        return r == SBM_EPERM ? SBOS_EPERM : r == SBM_ERANGE ? SBOS_EINVAL : SBOS_EAGAIN;
    k = kmsg_new();
    k->state = 1;
    struct sbos_time t;
    k->utc = clock_now(&t) ? t.utc : 0;
    sb_strcpy(k->number, sizeof(k->number), number);
    sb_strcpy(k->text, sizeof(k->text), utf8);
    ksend = nkmsg - 1;
    ui_mark_dirty();
    return 0;
}

int kmodem_sms_count(void) { return nkmsg; }
int kmodem_sms_get(int i, struct ksms *m)
{
    if (i < 0 || i >= nkmsg)
        return SBOS_EINVAL;
    *m = kmsg[i];
    return 0;
}
uint32_t kmodem_sms_gen(void) { return kgen; }
#else
void kmodem_reserve(void) { }
int kmodem_call_dial(const char *number) { (void)number; return SBOS_EAGAIN; }
int kmodem_call_answer(void) { return SBOS_EAGAIN; }
int kmodem_call_hangup(void) { return SBOS_EAGAIN; }
int kmodem_call_route(int audio) { (void)audio; return SBOS_EAGAIN; }
int kmodem_call_mute(int on) { (void)on; return SBOS_EAGAIN; }
int kmodem_call_dtmf(char digit) { (void)digit; return SBOS_EAGAIN; }
void kmodem_call(struct kcall *c) { memset(c, 0, sizeof(*c)); }
int kmodem_sms_send(const char *number, const char *utf8) { (void)number; (void)utf8; return SBOS_EAGAIN; }
int kmodem_sms_count(void) { return 0; }
int kmodem_sms_get(int i, struct ksms *m) { (void)i; (void)m; return SBOS_EINVAL; }
uint32_t kmodem_sms_gen(void) { return 0; }

int kmodem_cellular(int on) { (void)on; return SBOS_EAGAIN; }
int kmodem_netif(struct sbnet **net, struct sbtcp **tcp) { (void)net; (void)tcp; return 0; }
void kmodem_cell(struct kcell *c) { memset(c, 0, sizeof(*c)); c->bars = -1; }
#endif

void kmodem_timer(unsigned id)
{
    switch (id) {
    case 1:
        if (st == SBOS_MODEM_STARTING)
            start_now();
        break;
    case 2:
        if (st == SBOS_MODEM_ON)
            poll();
        break;
    case 3:
        if (st == SBOS_MODEM_ON) {
            query();
            kt_set(KT_MODEM, 3, sb_ms(), QUERY_MS, 0);
        }
        break;
#ifndef SB_HOST
    case 4:
        if (st == SBOS_MODEM_STARTING && !mo.up)
            recover();
        break;
    case 5:
        cell_step();
        break;
    case 6:
        if (cnet_up) {
            sbnet_tick(&cnet);
            sbtcp_tick(&ctcp);
            wifi_fetch_poll_cell();
        }
        break;
#endif
    default:
        break;
    }
}

void kmodem_status(struct sbos_phone *s)
{
    memset(s, 0, sizeof(*s));
    s->state = st;
    s->secs = (int32_t)((sb_ms() - t_state) / 1000);
    s->starts = starts;
    s->sim = s->bars = s->rssi = -1;
    sb_strcpy(s->why, sizeof(s->why), why);
    sb_strcpy(s->line, sizeof(s->line), st == SBOS_MODEM_ON ? line : "");
    sb_strcpy(s->sim_name, sizeof(s->sim_name), "?");
    sb_strcpy(s->cs, sizeof(s->cs), "?");
    sb_strcpy(s->ps, sizeof(s->ps), "?");
    if (st != SBOS_MODEM_ON)
        return;
#ifndef SB_HOST
    const struct sbm_cell_state *c = &mo.b.cell;
    if (c->have_sim) {
        s->sim = c->sim_status;
        sb_strcpy(s->sim_name, sizeof(s->sim_name), sbm_sim_name(c->sim_status));
    }
    if (c->have_bars)
        s->bars = c->bars;
    if (c->have_rssi)
        s->rssi = c->rssi_raw[0];
    if (c->have_cs) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, sbm_reg_name(c->cs.state));
        ub_s(&u, " (");
        ub_s(&u, sbm_act_name(c->cs.act));
        ub_s(&u, ")");
        sb_strcpy(s->cs, sizeof(s->cs), u.b);
    }
    if (c->have_ps) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, sbm_reg_name(c->ps.state));
        ub_s(&u, " (");
        ub_s(&u, sbm_act_name(c->ps.act));
        ub_s(&u, ")");
        sb_strcpy(s->ps, sizeof(s->ps), u.b);
    }
    s->msgs = mo.b.fmt_rx;
    s->rfs = mo.b.rfs_rx;
    s->queries = mo.b.fmt_tx;
    s->answers = mo.b.fmt_resp + mo.b.fmt_gen_res;
#else
    if (h_queries) {
        s->sim = 0;
        sb_strcpy(s->sim_name, sizeof(s->sim_name), "absent");
        sb_strcpy(s->cs, sizeof(s->cs), "emergency-only (GSM)");
        sb_strcpy(s->ps, sizeof(s->ps), "not registered (GSM)");
        s->bars = 4;
        s->rssi = 0x3e;
    }
    s->msgs = h_msgs;
    s->queries = h_queries;
    s->answers = h_answers;
#endif
}
