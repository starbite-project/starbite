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
#include "abox.h"                           /* call audio: the ABOX DSP (Calliope + the CP's VSS), the amp, the codec */
#include "amp.h"
#include "codec.h"
#include "sb/bcmw_sb.h"                     /* the VENDOR file reader (Calliope's firmware) */
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
static void audio_prepare(void);
static void audio_after_online(void);
static void audio_stop(void);
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
    if (mo.attended == SBM_ATTENDED_DATA)
        audio_prepare();                    /* the call audio path before the CP starts (VSS waits for both) */
    uint64_t t0 = sb_ms();
    int r = sbm_modem_start(&mo, &plat, &im);
    if (r != 0) {
        sbm_cp_park(&plat);                 /* the core parked it already; safe again */
        armed = 0;
        audio_stop();                       /* ABOX off and media sound back (it stayed booted before) */
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
    if (mo.attended == SBM_ATTENDED_DATA) {
        audio_after_online();
        cell_begin();
    }
#endif
}

void kmodem_stop(const char *w)
{
    if (!kmodem_on())
        return;
#ifndef SB_HOST
    cell_stopped(w, 0);
    audio_stop();
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
        audio_stop();                       /* ABOX off and media sound back (it stayed booted before) */
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
       CS_RETRY, CS_DEACT, CS_DETACH, CS_VOICE };   /* 8z: CS_VOICE registered for calls and SMS, no data context */
#define CELL_APN_DEFAULT "internet"        /* Elisa, DNA, Telia (FI) */
static char cell_apn[64] = CELL_APN_DEFAULT;   /* Settings > Cellular > APN (kmodem_set_apn); the state slot keeps it */
#define CELL_APN cell_apn
static int cell_want, cst = KCELL_OFF, cstep = CS_IDLE, c_lf, c_esc, cnet_up, c_check, c_ntp, c_sms_ready;
static int cell_selftest;                  /* next-celltest: the unattended data self test (kmodem_selftest_phase) */
static int cell_calls;                     /* 8z: the user's "Calls and SMS": the radio registered without mobile data
                                            * (cell_want is mobile data; the radio runs while either is on) */
static int c_check_ok;                     /* the session's HTTPS check passed */
static uint32_t c_nitz_seen;               /* the NITZ time already looked at */
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
    if (cnet_up && !guestnet_rx(KNET_UPLINK_CELL, pkt, len))   /* the Linux guest's flows first (its NAT) */
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
    guestnet_uplink(KNET_UPLINK_CELL, 0);
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
    guestnet_uplink(KNET_UPLINK_CELL, &cnet);   /* the Linux guest may use mobile data (Wi-Fi first) */
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
    cell_calls = 0;
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
    c_nitz_seen = 0;
    comms_begin();
    cset(KCELL_STARTING, "Starting the modem");
    cgo(CS_SIM);
    kt_set(KT_MODEM, 5, sb_ms(), 100, 1);
}

static int c_registered(const struct sbm_modem_snap *sn)
{
    return sn->cs == SBM_REG_HOME || sn->cs == SBM_REG_ROAMING || sn->ps == SBM_REG_HOME || sn->ps == SBM_REG_ROAMING;
}

/* the network's time (NITZ, MISC 0x0A05): sets the clock only when Starbite OS has none (or only the last known
 * time); a clock from NTP or the PMIC stays (NTP over mobile data checks it anyway) */
static void c_nitz(void)
{
    const struct sbm_cell_state *cs = &mo.b.cell;
    if (cs->nitz_gen == c_nitz_seen)
        return;
    c_nitz_seen = cs->nitz_gen;
    if (!cs->have_nitz || !cs->nitz_utc || cs->nitz_utc < 1577836800LL)
        return;
    struct sbos_time t;
    if (clock_now(&t) && !t.approx)
        return;
    int was = 0;
    (void)clock_set_net(cs->nitz_utc, &was);
    if (was) {
        wifi_clock_checked_by(3);
        sb_log("cell: the clock was set from the network's time (NITZ)");
    }
}

static void cell_step(void)
{
    struct sbm_modem_snap sn;
    if (!mo.up)
        return;                             /* down: poll()'s restart (or a stop) decides */
    comms_tick();
    c_nitz();
    sbm_modem_snapshot(&mo, &sn);
    switch (cstep) {
    case CS_SIM:
        if (sn.sim == SBM_SIM_ABSENT && cin() > 8000) {
            cset(KCELL_NO_SIM, "No SIM card");
            cell_want = cell_calls = 0;
            cgo(CS_IDLE);
            kmodem_stop("No SIM card");
        } else if (sn.sim == SBM_SIM_LOCK_SC) {
            cset(KCELL_SIM_LOCKED, mo.b.cell.sim_key == 2 ? "The SIM is PUK-locked" :
                 "The SIM's PIN is on (PIN entry is not built yet)");
            cell_want = cell_calls = 0;
            cgo(CS_IDLE);
            kmodem_stop("SIM locked");
        } else if (sn.sim == SBM_SIM_READY || sn.sim == SBM_SIM_INIT_COMPLETE || sn.sim == SBM_SIM_PB_INIT_COMPLETE) {
            cset(KCELL_SEARCHING, "Turning the radio on");
            sbm_modem_radio(&mo, 1);
            cgo(CS_RADIO);
        } else if (cin() > 60000) {
            cset(KCELL_FAILED, "The SIM did not answer");
            cell_want = cell_calls = 0;
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
            if (!c_sms_ready && !cell_selftest) {   /* unattended: no SMS reaches the AP (nothing to acknowledge) */
                c_sms_ready = 1;
                sbm_modem_sms_ready(&mo);   /* the AP takes SMS (and the SIM's SMSC) */
            }
            if (!cell_want) {               /* 8z: calls and SMS only: no data context */
                cset(KCELL_REGISTERED, "Calls and SMS (mobile data off)");
                cgo(CS_VOICE);
                break;
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
            c_check_ok = 0;
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
                c_check_ok = f.result == 1 && f.status == 200;
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
            if (cell_want) {                 /* 8z: mobile data on again meanwhile */
                c_t0 = sb_ms();
                c_esc = 3;
                cgo(CS_REG);
            } else if (cell_calls) {         /* 8z: mobile data off, the radio stays for calls and SMS */
                cset(KCELL_REGISTERED, "Calls and SMS (mobile data off)");
                cgo(CS_VOICE);
            } else {
                sbm_modem_radio(&mo, 0);     /* low power: the CP detaches, then RF off */
                cgo(CS_DETACH);
            }
        }
        break;
    case CS_VOICE:                           /* 8z: registered for calls and SMS; data waits for its switch */
        if (!c_registered(&sn)) {
            cset(KCELL_SEARCHING, "Reconnecting");
            c_t0 = sb_ms();
            c_esc = 3;                       /* no escalation on a reconnect: the CP tries by itself */
            cgo(CS_REG);
        } else if (cell_want) {
            cset(KCELL_REGISTERED, "Starting mobile data");
            sbm_modem_pdp_define(&mo, CELL_APN);
            cgo(CS_DEFINE);
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

static int cell_start(const char *who);
int kmodem_cellular(int on)
{
    if (!on) {
        cell_want = 0;
        if (!kmodem_on() || mo.attended != SBM_ATTENDED_DATA) {
            if (!cell_calls)
                cset(KCELL_OFF, "Off");
            return 0;
        }
        if (cell_calls) {                   /* 8z: mobile data off; the radio stays registered for calls and SMS */
            cell_net_down("Mobile data off");
            if (cstep == CS_DEFINE || cstep == CS_ACT || cstep == CS_IP || cstep == CS_UP || cstep == CS_RETRY) {
                cset(KCELL_REGISTERED, "Mobile data off");
                sbm_modem_pdp_activate(&mo, 0, 0);
                cgo(CS_DEACT);
                kt_set(KT_MODEM, 5, sb_ms(), 100, 1);
            }
            return 0;                       /* (earlier steps: CS_REG goes to CS_VOICE without data) */
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
    if (sbos_selftest_enabled && !cell_selftest)
        return SBOS_EAGAIN;
    if (kmodem_on() && mo.attended == SBM_ATTENDED_DATA && cell_calls) {
        cell_want = 1;                      /* 8z: the radio is up for calls: data from where it is (CS_VOICE / CS_REG) */
        return 0;
    }
    int e = cell_start("Cellular on");
    if (e)
        return e;
    cell_want = 1;
    return 0;
}

/* 8z (the user's calls / SMS sitting, main 11-15): the radio for calls and SMS without mobile data ("Calls and SMS",
 * at the start when kept on). Off: the radio stops unless mobile data keeps it. */
int kmodem_calls(int on)
{
    if (!on) {
        cell_calls = 0;
        if (cell_want)
            return 0;                       /* mobile data keeps the radio (and calls) */
        return kmodem_cellular(0);
    }
    if (cell_calls && kmodem_on())
        return 0;
    if (sbos_selftest_enabled && !cell_selftest)
        return SBOS_EAGAIN;
    if (kmodem_on() && mo.attended == SBM_ATTENDED_DATA) {
        cell_calls = 1;                     /* mobile data runs the radio already */
        return 0;
    }
    int e = cell_start("Calls on");
    if (e)
        return e;
    cell_calls = 1;
    return 0;
}

/* the start for mobile data or calls: Heat refuses (8z: Linux no longer does in Next: the guest's RAM, its shared
 * region and sbgpu's tables are DRAM bank 2's top, 0x8_8000_0000..0x8_E83F_FFFF; the modem's modem_if, VSS and
 * cp_ram_logging are 0xF000_0000..0xFB1F_FFFF; its mailbox is polled on Starbite's cores, no interrupt; the classic
 * Modem app's receive-only start keeps its own Linux guard) */
static int cell_start(const char *who)
{
    if (heater_running_ui()) {
        cset(KCELL_FAILED, "Stop Heat first");
        return SBOS_EAGAIN;
    }
    if (kmodem_on())
        kmodem_stop(who);                   /* a receive-only session: restarted for calls and data */
    mo.attended = SBM_ATTENDED_DATA;        /* the user's switches (Cellular Data, Calls and SMS): the only places */
    if (guest_busy())
        sb_log("cell: starting beside a running Linux (8z)");
    cset(KCELL_STARTING, "Starting the modem");
    ++starts;
    /* Track G (2026-11-12): no storage hold for the user's Cellular: Starbite OS saves settings and messages while
     * the modem runs (its own state slot, the card). The modem code still has no block-write path (build.sh checks
     * the objects), RADIO / EFS stay read once, the CP's NV writes stay in RAM, and the fence still refuses EFS,
     * CPEFS, PARAM and the rest without the user's typed confirmation. */
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
    c->calls = cell_calls;
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
    if (c_registered(&sn) && mo.b.cell.have_names) {  /* the network's own name (NITZ): the short one fits */
        const char *nn = mo.b.cell.net_short[0] ? mo.b.cell.net_short : mo.b.cell.net_full;
        sb_strcpy(c->oper, sizeof(c->oper), nn);
    } else if (sn.have_plmn && c_registered(&sn)) {
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

/* ---- call audio (docs/MODEM.md 7.12): ABOX (Calliope + the CP's VSS) <-> UAIF1 -> the amp's earpiece / speaker,
 * the codec's main mic -> UAIF0 -> ABOX. Brought up when Cellular starts (before the CP: VSS waits for both), routed
 * while a call is active, taken down with the modem. A failure never stops calls or data: audio_ok just stays 0
 * and the log says which step. The speaker is capped in amp.c (no speaker protection here). */
static struct abox_hal au_hal;
static int au_hal_set, au_amp, au_codec, au_abox, au_vss, au_on, au_speaker;
/* the drivers' sound service (snd.c): media sound pauses while Cellular holds ABOX (Calliope + the CP's VSS), so
 * its idle-off can never stop ABOX under the modem, and a staged media boot of Calliope is finished before ours.
 * Weak: a kernel without snd.c links, and these are then skipped. */
void snd_suspend_for_call(void) __attribute__((weak));
void snd_resume_after_call(void) __attribute__((weak));
void snd_calliope_shared(int on) __attribute__((weak));   /* 9a */
/* 9g (9d's cellsnd: periods 0 with Cellular on, the bypass engine after it): snd up BEFORE our abox_boot, so its media
 * ring windows (cal_map, refused once Calliope runs) are in the SysMMU tables the boot writes; kern_next.c's */
int snd_media_early(void) __attribute__((weak));
static int au_snd_held, au_snd_call;          /* 9a: Calliope shared with media; media paused for a call's routes */

static long au_file(void *ctx, const char *path, void *dst, uint32_t cap)
{
    (void)ctx;
    return bcmw_sb_vendor_read(path, dst, cap);
}

static void au_log(const char *what, int r, const char *why)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "audio: ");
    ub_s(&u, what);
    ub_s(&u, r == 0 ? ": ok" : ": FAILED (");
    if (r != 0) {
        ub_s(&u, why ? why : "?");
        ub_s(&u, ")");
    }
    sb_log(u.b);
}

static void audio_prepare(void)
{
    if (!au_snd_held && snd_media_early && abox_state() == ABOX_ST_OFF && !snd_media_early())
        sb_log("audio: media sound is not up (a self test without sound): Calliope boots without its ring");
    if (!au_snd_held) {
        au_snd_held = 1;
        /* before our HAL is set: a pending media boot finishes on snd's. 9a (main 11-15): media is not paused for
         * all of Cellular any more (no ringtone, no alarm sound): it plays through our Calliope while no call routes */
        if (snd_calliope_shared) {
            snd_calliope_shared(1);
            sb_log("audio: media sound shares Calliope with Cellular (paused only while a call's audio is routed)");
        } else if (snd_suspend_for_call) {
            snd_suspend_for_call();
            sb_log("audio: media sound paused while Cellular is on (ABOX is the modem's)");
        }
    }
    if (!au_hal_set) {
        abox_hal_libsb(&au_hal, au_file, 0);
        abox_hal_set(&au_hal);
        amp_hal_set(audio_hal_libsb());
        codec_hal_set(audio_hal_libsb());
        au_hal_set = 1;
    }
    int r;
    if (!au_amp) {
        r = amp_probe();
        au_amp = r == 0 ? 1 : -1;
        au_log("the amp (MAX98512 x 2)", r, amp_why());
    }
    if (!au_codec) {
        r = codec_probe();
        au_codec = r == 0 ? 1 : -1;
        au_log("the codec (CS47L93)", r, codec_why());
    }
    if (au_abox <= 0) {
        uint64_t t = sb_ms();
        r = abox_boot();
        au_abox = r == 0 ? 1 : -1;
        au_log("ABOX + Calliope", r, abox_why());
        if (r == 0) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "audio: ABOX up in ");
            ub_dec(&u, (uint32_t)(sb_ms() - t));
            ub_s(&u, " ms");
            sb_log(u.b);
            abox_vss_prepare();             /* the VSS magic word (the core clears it too) */
            kt_set(KT_MODEM, 7, sb_ms(), 10, 1);   /* abox_poll every 10 ms */
        }
    }
    au_vss = 0;
}

static void audio_after_online(void)
{
    if (au_abox != 1)
        return;
    int r = abox_vss_start();
    au_vss = r == 0;
    au_log("VSS started (the CP's voice codec on ABOX)", r, abox_why());
    kt_set(KT_MODEM, 7, sb_ms(), 10, 1);    /* the start cancelled the timers: the poll again */
}

static void audio_route(int on)
{
    if (on == au_on && (!on || au_speaker == (kc.audio == KAUDIO_SPEAKER)))
        return;
    if (on) {
        int sp = kc.audio == KAUDIO_SPEAKER;
        if (!au_on && snd_calliope_shared && snd_suspend_for_call && !au_snd_call) {
            snd_suspend_for_call();          /* 9a: the ringtone / media off before the call takes the routes */
            au_snd_call = 1;
            sb_log("audio: media sound paused for the call");
        }
        if (!au_on && au_abox == 1)
            au_log("ABOX call routes", abox_call(1), abox_why());
        if (au_amp == 1) {
            if (sp) {
                amp_receiver(0, 0);
                au_log("the speaker (capped)", amp_speaker(1, 3), amp_why());
            } else {
                amp_speaker(0, 0);
                au_log("the earpiece", amp_receiver(1, 3), amp_why());
            }
        }
        if (!au_on && au_codec == 1)
            au_log("the main mic", codec_mic(1), codec_why());
        au_speaker = sp;
        au_on = 1;
    } else {
        if (au_amp == 1)
            amp_off();
        if (au_codec == 1)
            codec_mic(0);
        if (au_abox == 1)
            abox_call(0);
        au_on = 0;
        sb_log("audio: the call's routes off");
        if (au_snd_call) {                   /* 9a: media through Calliope again while Cellular stays on */
            au_snd_call = 0;
            if (snd_resume_after_call)
                snd_resume_after_call();
        }
    }
}

static void audio_stop(void)
{
    if (au_on)
        audio_route(0);
    kt_cancel(KT_MODEM, 7);
    if (au_snd_held && snd_calliope_shared)
        snd_calliope_shared(0);             /* 9a: media stopped cleanly before Calliope goes */
    if (au_abox == 1) {
        abox_off();
        sb_log("audio: ABOX off (with the modem)");
    }
    if (au_codec == 1)
        codec_off();
    au_abox = au_vss = 0;
    au_codec = 0;
    if (au_snd_held) {                      /* after abox_off(): snd boots Calliope afresh when it next plays */
        au_snd_held = 0;
        if (snd_resume_after_call) {
            snd_resume_after_call();
            sb_log("audio: media sound may play again (Cellular off)");
        }
    }
}

static int audio_ok(void)
{
    return au_abox == 1 && au_vss && au_amp == 1;
}

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
        k->unread = 1;
        settings_changed();                          /* the store keeps it (the state slot, within 10 s) */
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

/* delivery reports (sbm_link.c's ring): each to the newest sent message with that reference; all its parts
 * delivered: Delivered; a permanent failure (TP-ST 0x40..): Not delivered. 0x20..0x3F: the network still tries. */
static uint32_t rep_seen;
static void sms_reports(void)
{
    const struct sbm_smsq *q = &mo.b.sms;
    if (q->rep_gen - rep_seen > SBM_SMS_REP_MAX)
        rep_seen = q->rep_gen - SBM_SMS_REP_MAX;
    for (; rep_seen != q->rep_gen; ++rep_seen) {
        uint8_t mr = q->rep_mr[rep_seen % SBM_SMS_REP_MAX], stv = q->rep_st[rep_seen % SBM_SMS_REP_MAX];
        if (stv >= 0x20 && stv < 0x40)
            continue;
        for (int i = nkmsg - 1; i >= 0; --i) {
            struct ksms *k = &kmsg[i];
            if (k->incoming || k->state != KSMS_SENT || !k->parts)
                continue;
            int part = -1;
            for (unsigned j = 0; j < k->parts && j < 4; ++j)
                if (k->mr[j] == mr && !(k->rep & (1u << j)))
                    part = (int)j;
            if (part < 0)
                continue;
            if (stv < 0x20) {
                k->rep |= (uint8_t)(1u << part);
                if (k->rep == (uint8_t)((1u << k->parts) - 1u))
                    k->state = KSMS_DELIVERED;
            } else {
                k->state = KSMS_NOT_DELIVERED;
                sb_strcpy(k->why, sizeof(k->why), "Not delivered: the network gave up");
            }
            sb_log(k->state == KSMS_DELIVERED ? "comms: the SMS was delivered" :
                   k->state == KSMS_NOT_DELIVERED ? "comms: the SMS was not delivered" :
                   "comms: a part of the SMS was delivered");
            kgen++;
            settings_changed();
            ui_mark_dirty();
            break;
        }
    }
}

static void comms_begin(void)
{
    memset(&kc, 0, sizeof(kc));
    kc.audio = KAUDIO_EARPIECE;
    rep_seen = 0;
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
    /* the call audio follows the call: on while one is active (the route: earpiece or speaker), off otherwise */
    audio_route(c->n && c->c[0].state == SBM_CALL_ACTIVE);
    sms_reports();
    if (ksend >= 0 && ksend < nkmsg) {               /* the send in flight: all parts answered? */
        struct sbm_smsq *q = &mo.b.sms;
        int done = 1, ok = 1;
        for (unsigned i = 0; i < q->parts; ++i) {
            done &= q->done[i];
            ok &= q->done[i] && q->res[i] == 0;
        }
        if (done) {
            struct ksms *k = &kmsg[ksend];
            k->parts = q->parts;
            for (unsigned i = 0; i < q->parts && i < 4; ++i)
                k->mr[i] = q->mr[i];
            k->rep = 0;
            settings_changed();
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

/* ---- next-celltest: the unattended mobile-data self test (uitest.c SBOS_SELFTEST_NEXT_CELL, every 500 ms) --------
 * The user's consent (2026-11-11): unattended mobile data is OK; calls and SMS stay attended. So this test turns
 * Cellular on itself (the same path as the switch), never sends SMS_READY (no SMS reaches the AP, nothing is
 * acknowledged), and dial / answer / hang-up / send stay refused (the self-test image). An incoming call only shows
 * the screen and buzzes; nobody answers it.
 * Steps: data up (and the audio stages 1-2: amp, codec, ABOX + Calliope, VSS: no call, no sound); the session's
 * HTTPS check; SOAK_N more HTTPS GETs SOAK_MS apart; Cellular off (deactivate, detach, park); Cellular on again in
 * the same boot; its HTTPS check; off. At most 10 min. Addresses, the operator and identities are never logged. */
#define SOAK_N 5
#define SOAK_MS 30000U
static int st_ph, st_round, st_fails, st_pass, st_restarted;
static uint64_t st_t0, st_tp, st_all;
static uint32_t st_min = 0xffffffffu, st_max, st_sum, st_n, st_up_ms[2], st_off_ms;
static char st_audio[64];                   /* the audio stages, for the summary (a short pmsg tail keeps it) */
/* no SIM (2026-11-13: the SIM is out for the day): the modem and the call-audio path start and stop twice */
static int st_nosim, st_hs[2], st_abox_ok[2];
static char st_aud[2][64];

static void st_audio_snap(int k)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "amp ");
    ub_s(&u, au_amp == 1 ? "ok" : au_amp == -1 ? "FAILED" : "not run");
    ub_s(&u, ", codec ");
    ub_s(&u, au_codec == 1 ? "ok" : au_codec == -1 ? "FAILED" : "not run");
    ub_s(&u, ", ABOX ");
    ub_s(&u, au_abox == 1 ? "ok" : au_abox == -1 ? "FAILED" : "not run");
    ub_s(&u, ", VSS ");
    ub_s(&u, au_vss ? "started" : "not started");
    sb_strcpy(st_aud[k], sizeof(st_aud[k]), u.b);
    if (au_abox == 1)
        st_abox_ok[k] = 1;
    int v = abox_vss_ready();
    if (v && !st_hs[k])
        st_hs[k] = v;
}

static void st_line(const char *a, uint64_t ms)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "selftest: cell: ");
    ub_s(&u, a);
    ub_dec(&u, (uint32_t)ms);
    ub_s(&u, " ms");
    sb_log(u.b);
}

static const char *au_words(int v) { return v == 1 ? "ok" : v == -1 ? "FAILED" : "not run"; }

/* 8l (Track A): the linuxtest's Linux network check: mobile data on (unattended, as next-celltest: the user allows
 * data with the SIM in; no SMS, no calls) before Linux starts, off at the end. Its state through kmodem_cell */
int kmodem_selftest_data(int on)
{
    if (on) {
        cell_selftest = 1;
        sb_log("selftest: cell: Cellular on for Linux's network check (unattended data; no SMS, no calls)");
        return kmodem_cellular(1);
    }
    int r = kmodem_cellular(0);
    cell_selftest = 0;
    return r;
}
void kmodem_selftest_abort(void)
{
    if (!cell_selftest)
        return;
    sb_log("selftest: cell: stopped by a touch or key: Cellular off (data off, detach, park)");
    kmodem_cellular(0);
    cell_selftest = 0;
}

int kmodem_selftest_phase(int *done)
{
    uint64_t now = sb_ms();
    *done = 0;
    if (!st_all)
        st_all = now;
    if (st_ph < 9 && now - st_all > 600000U) {
        sb_log("selftest: cell: FAILED: not done in 10 min; Cellular off");
        ++st_fails;
        kmodem_cellular(0);
        st_ph = 8;
        st_tp = now;
    }
    switch (st_ph) {
    case 0:
        cell_selftest = 1;
        sb_log("selftest: cell: Cellular on (unattended data; no SMS, no calls)");
        st_t0 = now;
        if (kmodem_cellular(1) != 0) {
            sb_logf("selftest: cell: FAILED: Cellular did not start: ", c_why[0] ? c_why : "?");
            ++st_fails;
            st_ph = 9;
            break;
        }
        st_ph = 1;
        break;
    case 1:                                 /* data up */
        if (au_abox)
            st_audio_snap(st_restarted);    /* before a stop resets it (no SIM: the stop comes after 8 s) */
        if (cst == KCELL_NO_SIM) {
            if (!st_nosim)
                sb_log("selftest: cell: no SIM: the radio test only (the modem and the call-audio path start and "
                       "stop, twice; no registration, nothing transmitted)");
            st_nosim = 1;
            st_tp = now;
            st_ph = st_restarted ? 9 : 6;
            st_restarted = 1;
            break;
        }
        if (cst == KCELL_DATA) {
            st_line(st_restarted ? "data up again (the restart in one boot) in " : "data up in ", now - st_t0);
            st_up_ms[st_restarted ? 1 : 0] = (uint32_t)(now - st_t0);
            st_tp = now;
            st_ph = 2;
        } else if (cst == KCELL_FAILED || cst == KCELL_NO_SIM || cst == KCELL_SIM_LOCKED || now - st_t0 > 240000U) {
            sb_logf("selftest: cell: FAILED: no data: ", c_why[0] ? c_why : "not in 240 s");
            ++st_fails;
            kmodem_cellular(0);
            st_ph = 8;
            st_tp = now;
        }
        break;
    case 2:                                 /* the session's own HTTPS check (cell_step logs its line) */
        if (c_check == 2 || c_check == 3 || now - st_tp > 90000U) {
            if (c_check_ok)
                ++st_pass;
            else
                ++st_fails;
            sb_log(c_check_ok ? "selftest: cell: the session's HTTPS check: PASS"
                              : "selftest: cell: the session's HTTPS check: FAILED");
            if (!st_restarted) {
                struct ub u;
                ub_init(&u);
                ub_s(&u, "selftest: cell: audio stages (no call, no sound): amp ");
                ub_s(&u, au_words(au_amp));
                ub_s(&u, ", codec ");
                ub_s(&u, au_words(au_codec));
                ub_s(&u, ", ABOX + Calliope ");
                ub_s(&u, au_words(au_abox));
                ub_s(&u, ", VSS ");
                ub_s(&u, au_vss ? "started" : "not started");
                sb_log(u.b);
                sb_strcpy(st_audio, sizeof(st_audio), u.b + sizeof("selftest: cell: audio stages (no call, no sound): ") - 1);
            }
            st_round = 0;
            st_tp = now;
            st_ph = st_restarted ? 7 : 3;
        }
        break;
    case 3:                                 /* the soak: one GET every SOAK_MS */
        if (cst != KCELL_DATA) {
            sb_logf("selftest: cell: data dropped during the soak: ", c_why[0] ? c_why : "?");
            ++st_fails;
            st_ph = 7;
            break;
        }
        if (now - st_tp >= SOAK_MS) {
            st_tp = now;
            if (wifi_fetch("https://example.com/") == 0)
                st_ph = 4;
            else {
                sb_log("selftest: cell: soak GET refused (the fetch path is busy)");
                ++st_fails;
                if (++st_round >= SOAK_N)
                    st_ph = 7;
            }
        }
        break;
    case 4: {
        struct sbos_wifi_fetch f;
        wifi_fetch_status(&f);
        if (f.state != 2 && now - st_tp < 60000U)
            break;
        int ok = f.state == 2 && f.result == 1 && f.status == 200;
        struct ub u;
        ub_init(&u);
        ub_s(&u, "selftest: cell: soak ");
        ub_dec(&u, (uint32_t)st_round + 1U);
        ub_s(&u, "/");
        ub_dec(&u, (uint32_t)SOAK_N);
        ub_s(&u, ok ? ": PASS, total " : ": FAILED, total ");
        ub_dec(&u, f.total_ms);
        ub_s(&u, " ms (DNS ");
        ub_dec(&u, f.dns_ms);
        ub_s(&u, ", connect ");
        ub_dec(&u, f.connect_ms);
        ub_s(&u, ", TLS ");
        ub_dec(&u, f.tls_ms);
        ub_s(&u, "); ");
        ub_s(&u, f.why);
        sb_log(u.b);
        if (ok) {
            ++st_pass;
            st_n++;
            st_sum += f.total_ms;
            if (f.total_ms < st_min)
                st_min = f.total_ms;
            if (f.total_ms > st_max)
                st_max = f.total_ms;
        } else
            ++st_fails;
        st_ph = ++st_round >= SOAK_N ? 7 : 3;
        break;
    }
    case 6:                                 /* no SIM: parked; start again after 3 s (the AUD domain's power-on path) */
        if (!kmodem_on() && now - st_tp > 3000U)
            st_ph = 0;
        break;
    case 7:                                 /* Cellular off: deactivate, detach, park */
        sb_log("selftest: cell: Cellular off");
        kmodem_cellular(0);
        st_tp = now;
        st_ph = 8;
        break;
    case 8:
        if (cst == KCELL_OFF || cst == KCELL_FAILED || now - st_tp > 30000U) {
            if (cst == KCELL_OFF && !kmodem_on()) {
                st_line("off and parked in ", now - st_tp);
                st_off_ms = (uint32_t)(now - st_tp);
            }
            else {
                sb_log("selftest: cell: FAILED: not off and parked in 30 s; parking");
                kmodem_stop("self test");
                ++st_fails;
            }
            if (!st_restarted && !st_fails && now - st_all < 480000U) {
                st_restarted = 1;
                st_ph = 0;                  /* on again in the same boot */
            } else
                st_ph = 9;
        }
        break;
    default: {
        struct ub u;
        ub_init(&u);
        if (st_nosim) {
            int ok = st_abox_ok[0] && st_abox_ok[1] && !kmodem_on();
            ub_s(&u, ok ? "selftest: cell: PASS (no SIM): " : "selftest: cell: FAILED (no SIM): ");
            for (int k = 0; k < 2; ++k) {
                ub_s(&u, k ? "; start 2: " : "start 1: ");
                ub_s(&u, st_aud[k][0] ? st_aud[k] : "audio not reached");
                ub_s(&u, ", VSS handshake ");
                ub_s(&u, st_hs[k] == 1 ? "done" : st_hs[k] == -1 ? "TIMED OUT" : "not seen");
            }
            ub_s(&u, kmodem_on() ? "; the modem is STILL ON" : "; parked");
            sb_log(u.b);
            cell_selftest = 0;
            *done = 1;
            return ok;
        }
        ub_s(&u, st_fails ? "selftest: cell: FAILED: " : "selftest: cell: PASS: ");
        ub_dec(&u, (uint32_t)st_pass);
        ub_s(&u, " HTTPS GETs passed, ");
        ub_dec(&u, (uint32_t)st_fails);
        ub_s(&u, " failures; soak total ms min ");
        ub_dec(&u, st_n ? st_min : 0);
        ub_s(&u, " avg ");
        ub_dec(&u, st_n ? st_sum / st_n : 0);
        ub_s(&u, " max ");
        ub_dec(&u, st_max);
        ub_s(&u, st_restarted ? "; restarted once in this boot" : "; no restart");
        ub_s(&u, "; data up in ");
        ub_dec(&u, st_up_ms[0]);
        ub_s(&u, " / ");
        ub_dec(&u, st_up_ms[1]);
        ub_s(&u, " ms; parked in ");
        ub_dec(&u, st_off_ms);
        ub_s(&u, " ms; audio: ");
        ub_s(&u, st_audio[0] ? st_audio : "not reached");
        sb_log(u.b);
        cell_selftest = 0;
        *done = 1;
        return st_fails == 0;
    }
    }
    return 1;
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
    k->audio_ok = audio_ok();
    if (!k->audio_ok)                        /* the first part missing, in the order the audio path needs them */
        sb_strcpy(k->audio_why, sizeof(k->audio_why),
                  au_abox == -1 ? "No audio: the audio processor did not start" :
                  au_abox == 0 ? "No audio: the audio path is not ready" :
                  !au_vss ? "No audio: the modem's voice link did not start" :
                  au_amp == -1 ? "No audio: the earpiece amplifier did not answer" :
                  "No audio yet");
    else if (au_codec == -1)
        sb_strcpy(k->audio_why, sizeof(k->audio_why), "Microphone not working: they can't hear you");
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
    mo.b.sms.want_report = 1;                        /* "Delivered" (TP-SRR): the network's report, 0x0402 type 2 */
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

int kmodem_sms_read(const char *number)
{
    int n = 0;
    for (int i = 0; number && i < nkmsg; ++i)
        if (kmsg[i].incoming && kmsg[i].unread && sb_streq(kmsg[i].number, number)) {
            kmsg[i].unread = 0;
            ++n;
        }
    if (n) {
        kgen++;
        settings_changed();
    }
    return n;
}

static void sms_remove(int i)
{
    memmove(&kmsg[i], &kmsg[i + 1], (size_t)(nkmsg - i - 1) * sizeof(kmsg[0]));
    nkmsg--;
    if (ksend > i)
        ksend--;
}

int kmodem_sms_delete(int32_t id)
{
    for (int i = 0; i < nkmsg; ++i)
        if (kmsg[i].id == id) {
            if (i == ksend)
                return SBOS_EBUSY;
            sms_remove(i);
            kgen++;
            settings_changed();
            ui_mark_dirty();
            return 0;
        }
    return SBOS_EINVAL;
}

int kmodem_sms_delete_thread(const char *number)
{
    int n = 0;
    for (int i = nkmsg - 1; number && i >= 0; --i)
        if (i != ksend && sb_streq(kmsg[i].number, number)) {
            sms_remove(i);
            ++n;
        }
    if (n) {
        kgen++;
        settings_changed();
        ui_mark_dirty();
    }
    return n;
}

/* The store (the state slot, written by settings.c with the rest of it): "sms.00" .. "sms.31", each at most
 * STATE_REC_MAX bytes in sbm_store.c's format, oldest first. When they do not all fit, the oldest are left out.
 * Only counts are logged, never a number or a text. */
#define SMS_RECS 32
static uint8_t sms_rec[STATE_REC_MAX];

static void sms_to_store(const struct ksms *k, struct sbm_store_msg *m)
{
    memset(m, 0, sizeof(*m));
    m->id = (uint32_t)k->id;
    m->utc = k->utc;
    m->incoming = (uint8_t)(k->incoming != 0);
    m->state = (uint8_t)k->state;
    m->unread = (uint8_t)(k->unread != 0);
    sb_strcpy(m->number, sizeof(m->number), k->number);
    sb_strcpy(m->text, sizeof(m->text), k->text);
}

void kmodem_sms_save(struct state_buf *b)
{
    static struct sbm_store_msg m;
    if (!sb_streq(cell_apn, CELL_APN_DEFAULT) && state_add(b, "cell.apn", cell_apn, sb_strlen(cell_apn) + 1) < 0)
        sb_log("cell: the APN setting did not fit in the slot");
    uint32_t room = SMS_RECS * (STATE_REC_MAX - 8U) * 7U / 8U, need = 0;   /* 1/8 slack for record ends */
    int first = nkmsg;
    while (first > 0) {
        sms_to_store(&kmsg[first - 1], &m);
        uint32_t z = sbm_store_msg_size(&m);
        if (need + z > room)
            break;
        need += z;
        --first;
    }
    char id[8] = "sms.00";
    struct sbm_store_rec r;
    unsigned k = 0;
    int saved = 0;
    sbm_store_begin(&r, sms_rec, sizeof(sms_rec));
    for (int i = first; i <= nkmsg; ++i) {
        if (i < nkmsg) {
            sms_to_store(&kmsg[i], &m);
            if (sbm_store_add(&r, &m)) {
                ++saved;
                continue;
            }
        }
        if (!r.count)
            break;
        id[4] = (char)('0' + k / 10), id[5] = (char)('0' + k % 10);
        if (state_add(b, id, sms_rec, r.len) < 0) {
            saved -= (int)r.count;
            break;
        }
        if (++k >= SMS_RECS || i == nkmsg)
            break;
        sbm_store_begin(&r, sms_rec, sizeof(sms_rec));
        --i;                                         /* this message again, into the new record */
    }
    if (saved < nkmsg || nkmsg) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "sms: ");
        ub_dec(&u, (uint32_t)(saved < 0 ? 0 : saved));
        ub_s(&u, " of ");
        ub_dec(&u, (uint32_t)nkmsg);
        ub_s(&u, " messages saved in ");
        ub_dec(&u, k);
        ub_s(&u, " records");
        sb_log(u.b);
    }
}

static int apn_valid(const char *a)
{
    unsigned n = 0;
    for (; a[n]; ++n) {
        char c = a[n];
        if (n >= sizeof(cell_apn) - 1 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                           c == '.' || c == '-'))
            return 0;
    }
    return n > 0;
}

/* the APN for mobile data and the LTE attach: [A-Za-z0-9.-], 1..63 characters (the builder allows the same set);
 * "" or 0 = the default "internet". Used from the next Cellular on; saved in the state slot. Never logged. */
int kmodem_set_apn(const char *apn)
{
    if (!apn || !*apn)
        apn = CELL_APN_DEFAULT;
    if (!apn_valid(apn))
        return SBOS_EINVAL;
    if (!sb_streq(apn, cell_apn)) {
        sb_strcpy(cell_apn, sizeof(cell_apn), apn);
        settings_changed();
        sb_log("cell: the APN was changed (used from the next Cellular on)");
    }
    return 0;
}

void kmodem_get_apn(char *out, unsigned cap)
{
    sb_strcpy(out, cap, cell_apn);
}

void kmodem_sms_restore(const uint8_t *half)
{
    static struct sbm_store_msg m;
    {
        const uint8_t *a = 0;
        int an = half ? state_find(half, "cell.apn", &a) : 0;
        if (an > 1 && an <= (int)sizeof(cell_apn) && a && !a[an - 1] && apn_valid((const char *)a)) {
            sb_strcpy(cell_apn, sizeof(cell_apn), (const char *)a);
            sb_log("cell: the APN setting restored");
        }
    }
    char id[8] = "sms.00";
    int got = 0;
    for (unsigned k = 0; half && k < SMS_RECS; ++k) {
        id[4] = (char)('0' + k / 10), id[5] = (char)('0' + k % 10);
        const uint8_t *d = 0;
        int n = state_find(half, id, &d);
        if (n <= 0 || !d)
            break;
        uint32_t off = 0;
        unsigned idx = 0;
        while (sbm_store_next(d, (uint32_t)n, &off, &idx, &m)) {
            struct ksms *x = kmsg_new();
            x->id = (int32_t)m.id;
            if (m.id > kmsg_id)
                kmsg_id = m.id;
            x->utc = m.utc;
            x->incoming = m.incoming;
            x->state = m.state <= KSMS_NOT_DELIVERED ? m.state : KSMS_SENT;
            x->unread = m.unread;
            sb_strcpy(x->number, sizeof(x->number), m.number);
            sb_strcpy(x->text, sizeof(x->text), m.text);
            if (!x->incoming && x->state == KSMS_SENDING) {
                x->state = KSMS_FAILED;              /* the restart came before the network's answer */
                sb_strcpy(x->why, sizeof(x->why), "Not sent: Starbite OS restarted");
            }
            ++got;
        }
    }
    if (got)
        sb_logx("sms: messages restored from the state slot: ", (uint64_t)got);
}
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
int kmodem_sms_read(const char *number) { (void)number; return 0; }
int kmodem_sms_delete(int32_t id) { (void)id; return SBOS_EINVAL; }
int kmodem_sms_delete_thread(const char *number) { (void)number; return 0; }
void kmodem_sms_save(struct state_buf *b) { (void)b; }
void kmodem_sms_restore(const uint8_t *half) { (void)half; }
int kmodem_set_apn(const char *apn) { (void)apn; return 0; }
void kmodem_get_apn(char *out, unsigned cap) { sb_strcpy(out, cap, "internet"); }

int kmodem_cellular(int on) { (void)on; return SBOS_EAGAIN; }
int kmodem_calls(int on) { (void)on; return SBOS_EAGAIN; }
int kmodem_netif(struct sbnet **net, struct sbtcp **tcp) { (void)net; (void)tcp; return 0; }
void kmodem_cell(struct kcell *c) { memset(c, 0, sizeof(*c)); c->bars = -1; }
int kmodem_selftest_phase(int *done) { *done = 1; return 1; }
int kmodem_selftest_data(int on) { (void)on; return SBOS_EAGAIN; }
void kmodem_selftest_abort(void) { }
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
    case 7:
        if (au_abox == 1 && abox_poll() < 0) {
            au_log("ABOX fell over", -1, abox_why());
            audio_route(0);
            abox_off();
            au_abox = -1;
            au_vss = 0;
            kt_cancel(KT_MODEM, 7);
        }
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
