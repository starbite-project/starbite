/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 1.2-alpha21: the modem, receive only, through modem-core's session API (docs/MODEM.md 7.6,
 * starbite/modem/sbm_link.c sbm_modem_*; the platform glue starbite/modem/sb/sbm_sb.c, as the modemsess test image).
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
#define MIN_ARENA (64ULL << 20)            /* RADIO's BOOT + MAIN + VSS + nv_data: about 42 MiB, plus room */

static int st = SBOS_MODEM_OFF, starts;
static uint64_t t_state, t_up, t_poll;
static char why[64], line[112];

#ifndef SB_HOST
static struct sbm_plat plat;
static struct sbm_image im;
static struct sbm_modem mo;
static int loaded, armed;
static uint8_t *nv_pristine;
static int nv_k = -1;
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
    int r = sbm_sb_load_image(&im, mlog);
    if (r < 0) {
        set_state(SBOS_MODEM_FAILED, "Reading RADIO / EFS failed");
        return -1;
    }
    for (unsigned k = 0; k < im.plan.n; ++k)
        if (im.plan.l[k].src == SBM_SRC_NV && im.data[k]) {
            nv_pristine = sb_alloc(im.plan.l[k].size);
            if (!nv_pristine) {
                set_state(SBOS_MODEM_FAILED, "Not enough free RAM for the NV copy");
                return -1;
            }
            memcpy(nv_pristine, im.data[k], im.plan.l[k].size);
            nv_k = (int)k;
        }
    if (nv_k < 0) {
        set_state(SBOS_MODEM_FAILED, "RADIO's plan has no NV data");
        return -1;
    }
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
    memcpy((uint8_t *)(uintptr_t)im.data[nv_k], nv_pristine, im.plan.l[nv_k].size);   /* EFS as read (sbm_link writes it so too) */
    armed = 1;
    sb_before_reset = park_hook;
    int r = sbm_modem_start(&mo, &plat, &im);
    if (r != 0) {
        sbm_cp_park(&plat);
        struct ub u;
        ub_init(&u);
        ub_s(&u, "modem: start failed (");
        ub_s(&u, sbm_strerror(r));
        ub_s(&u, "); parked");
        sb_log(u.b);
        stopped(SBOS_MODEM_FAILED, "The modem did not come online (parked)");
        return;
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
    if (st == SBOS_MODEM_ON)
        sbm_modem_stop(&mo);
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
    if (r != 0) {
        armed = 0;
        sb_log(r > 0 ? "modem: a guard parked the CP (a SIM or a registration)" : "modem: poll stopped; parked");
        stopped(SBOS_MODEM_PARKED, r > 0 ? "Parked: a SIM or a network registration (receive only)"
                                         : "Parked: the link reported an error");
        notif_post("modem", "Modem parked", why);
    }
#else
    (void)el;
    if (h_guard_s >= 0 && now - t_up >= (uint64_t)h_guard_s * 1000U) {
        sb_log("modem: a guard parked the CP (a SIM or a registration)");
        stopped(SBOS_MODEM_PARKED, "Parked: a SIM or a network registration (receive only)");
        notif_post("modem", "Modem parked", why);
    }
#endif
}

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
