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
    ub_s(&u, "modem: loading RADIO and EFS (read only); free RAM ");
    ub_size(&u, sb_arena_free());
    sb_log(u.b);
    if (sb_arena_free() < MIN_ARENA) {
        set_state(SBOS_MODEM_FAILED, "Not enough free RAM for the modem's image");
        return -1;
    }
    sbm_sb_plat(&plat, mlog);
    uint64_t used0 = sb_arena_used();
    int r = sbm_sb_load_image_stream(&im, mlog);    /* RADIO streamed at each start; NV + its pristine copy kept */
    if (r < 0) {
        set_state(SBOS_MODEM_FAILED, "Reading RADIO / EFS failed");
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
    sb_log("modem: online, receive only (status GETs every 10 s; a SIM or a registration parks it)");
    set_state(SBOS_MODEM_ON, "");
    kt_set(KT_MODEM, 2, sb_ms(), POLL_MS, 1);
    kt_set(KT_MODEM, 3, sb_ms(), 3000, 0);
}

void kmodem_stop(const char *w)
{
    if (!kmodem_on())
        return;
#ifndef SB_HOST
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
        stopped(SBOS_MODEM_FAILED, r == SBM_EPERM ? "Stopped: it went down 3 times in 30 min (parked)"
                                                  : "The modem did not come back (parked)");
        notif_post("modem", "Modem stopped", why);
        return;
    }
    sb_log("modem: restarted after a stall / crash (Android's reset path)");
    snap_gen = 0;
    t_up = t_poll = sb_ms();
    set_state(SBOS_MODEM_ON, "");
    kt_set(KT_MODEM, 2, sb_ms(), POLL_MS, 1);
    kt_set(KT_MODEM, 3, sb_ms(), 3000, 0);
}
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
