/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS heater service (docs/STARBITE-OS.md 8): reads the sensors at 4 Hz, runs the controller
 * (heatctl.c) and applies what it decides: the worker cores, the listed ACPM DVFS levels with a PLL readback,
 * the M3 cluster's PMU defaults (after the first-use confirmation), the heartbeat, and the checks after a stop
 * (every worker core off within 1.5 s, else the clocks are restored and the phone restarts).
 * Nothing here writes storage, the PMIC, the charger or the TMU. */
#include "sb.h"
#include "kern.h"

#define A55_RESTORE 1053000U
#define M3_RESTORE 1066000U

static struct heat_ctl ctl;
static struct heatk_cfg cfg __attribute__((aligned(64)));
static int running[8];                      /* workers started (1..7) */
static int start_fail[8];                   /* a start timed out: the core may be on and hung */
static int parked_logged[8];
static uint32_t a55_set, m3_set;            /* what we asked ACPM for (0: never) */
static int m3_prepped, confirmed;
static uint64_t bad0[8];
static int pending_level, pending_minutes;
static struct heat_in last_in;
static int stopping;                        /* stop checks in progress */
static uint64_t stop_t;

int heater_running_ui(void)
{
    return ctl.state == HEAT_ST_RUNNING || ctl.state == HEAT_ST_HOLDING || ctl.state == HEAT_ST_PAUSED;
}

static int adc_c10(unsigned ch, unsigned table, int *raw)
{
    const struct sb_adc_stat *st = sb_adc_stats(ch);
    uint32_t flag0 = st ? st->flag_only : 0;
    int r = sb_adc_filtered(ch, 0, 0, 0);
    if (r < 0)
        return -10000;
    if (st && st->flag_only != flag0)           /* a conversion without its completion: not trusted */
        return -10000;
    if (raw)
        *raw = r;
    return sb_therm_convert(table, r);
}

static void read_inputs(struct heat_in *in)
{
    memset(in, 0, sizeof(*in));
    in->now = sb_ms();
    int raw = 0, v;
    v = adc_c10(4, 1, &raw);
    in->bat_ok = v > -10000 && v > -300 && v < 1000;
    in->bat_c10 = v;
    in->bat_raw = raw;
    v = adc_c10(0, 0, 0);
    in->skin_ok = v > -300 && v < 1200;
    in->skin_c10 = v;
    v = adc_c10(1, 1, 0);
    in->usb_ok = v > -300 && v < 1000;
    in->usb_c10 = v;
    v = adc_c10(3, 1, 0);
    in->chg_ok = v > -300 && v < 1000;
    in->chg_c10 = v;
    struct sb_temps t;
    sb_temps_read(&t);
    in->tmu_on = t.top == 1;
    in->soc_ok = t.top == 1 && t.hottest > -30 && t.hottest < 150;
    in->soc_c10 = t.hottest * 10;
    in->trip_armed = (t.trip_armed & 1) != 0;
    struct sb_bat b;
    in->gauge_ok = sb_bat_read(&b) && b.ok;
    in->pct = b.soc_pct;
    in->mv = b.mv;
    in->ma = b.ma;
    uint8_t c;
    if (sb_chg_read(0xb2, &c))
        in->charger = (c & 0x60U) ? 1 : 0;
    else
        in->charger = -1;
    for (unsigned w = 1; w < 8; ++w) {
        const struct sb_wslot *s = sb_smp_slot(w);
        if (!s || !running[w])
            continue;
        if (s->state == SB_WK_DONE || s->state == SB_WK_PARKED) {
            running[w] = 0;                     /* it stopped by itself (the dead man's switch): restart it */
            continue;
        }
        if (s->errors > bad0[w])
            in->bad_results = 1;
        if (s->state == SB_WK_FAULT)
            in->worker_fault = 1;
    }
}

/* ask ACPM, then check the PLL within 5 ms: more than 2 % above the request is a stop */
static int clock_set(unsigned domain, uint32_t khz)
{
    if (!hw_dvfs(domain, khz))
        return 0;
    for (unsigned i = 0; i < 5; ++i) {
        sb_mdelay(1);
        uint32_t got = domain == DVFS_A55 ? hw_a55_khz() : hw_m3_khz();
        if (got && got <= khz + khz / 50U && got + khz / 50U >= khz)
            return 1;
    }
    uint32_t got = domain == DVFS_A55 ? hw_a55_khz() : hw_m3_khz();
    sb_logx("heater: the PLL does not show the level, kHz ", got);
    return got && got <= khz + khz / 50U;      /* lower than asked is safe; higher is not */
}

static void workers_apply(void)
{
    int want = ctl.cores;
    int m3_after = 0;
    for (unsigned w = 4; w < 8; ++w)
        m3_after |= running[w] && (int)w <= want;
    if (!m3_after && m3_set) {                  /* domain 3 back to its restore level while an M3 still runs */
        (void)clock_set(DVFS_M3, M3_RESTORE);
        m3_set = 0;
    }
    for (unsigned w = 1; w < 8; ++w) {
        if ((int)w > want && running[w]) {
            cfg.run[w] = 0;
            sb_dcache_clean_inval(&cfg, sizeof(cfg));
            running[w] = 0;
        }
    }
    for (unsigned w = 1; w < 8; ++w) {
        if ((int)w > want || running[w])
            continue;
        const struct sb_wslot *s = sb_smp_slot(w);
        if (s && s->state == SB_WK_RUNNING)
            continue;                               /* still leaving: next step */
        if (w >= 4 && !m3_prepped) {
            if (!hw_m3_prep()) {
                heater_stop_all("The big cores could not be prepared");
                return;
            }
            m3_prepped = 1;
        }
        cfg.run[w] = 1;
        sb_dcache_clean_inval(&cfg, sizeof(cfg));
        int r = hw_heat_start(w, &cfg);
        if (r) {
            sb_logx("heater: core did not start, worker ", w);
            if (r == -100)
                start_fail[w] = 1;
            cfg.run[w] = 0;
            if (w >= 4) {                           /* no M3 cores: the level stays at what runs */
                ctl.max_cores = (int)w - 1;
                ctl.cores = (int)w - 1;
            }
            break;
        }
        const struct sb_wslot *s2 = sb_smp_slot(w);
        bad0[w] = s2 ? s2->errors : 0;
        running[w] = 1;
    }
}

static void clocks_apply(void)
{
    if (ctl.a55_khz && ctl.a55_khz != a55_set) {
        if (!clock_set(DVFS_A55, ctl.a55_khz)) {
            heater_stop_all("The processor clock did not change as asked");
            return;
        }
        a55_set = ctl.a55_khz;
    }
    int m3_running = 0;
    for (unsigned w = 4; w < 8; ++w)
        m3_running |= running[w];
    if (ctl.m3_khz && m3_running && hw_m3_on() && ctl.m3_khz != m3_set) {
        if (!clock_set(DVFS_M3, ctl.m3_khz)) {
            heater_stop_all("The big cores' clock did not change as asked");
            return;
        }
        m3_set = ctl.m3_khz;
    }
}

/* ---- the run's log: every 10 s while heating, and the peaks at the stop ---- */
static struct {
    int bat, skin, soc, ma, mv;             /* peaks (mv: the lowest) */
    uint64_t t0, next;
    int on;
} pk;

static void ub_c10(struct ub *u, int ok, int c10)
{
    if (!ok) {
        ub_s(u, "--");
        return;
    }
    if (c10 < 0) {
        ub_c(u, '-');
        c10 = -c10;
    }
    ub_dec(u, (uint64_t)(c10 / 10));
    ub_c(u, '.');
    ub_c(u, (char)('0' + c10 % 10));
}

static int cores_running(void)
{
    int n = 0;
    for (unsigned w = 1; w < 8; ++w)
        n += running[w];
    return n;
}

static void run_track(const struct heat_in *in)
{
    if (!pk.on) {
        pk.on = 1;
        pk.t0 = in->now;
        pk.next = in->now;
        pk.bat = pk.skin = pk.soc = -10000;
        pk.ma = 0;
        pk.mv = 100000;
    }
    if (in->bat_ok && in->bat_c10 > pk.bat)
        pk.bat = in->bat_c10;
    if (in->skin_ok && in->skin_c10 > pk.skin)
        pk.skin = in->skin_c10;
    if (in->soc_ok && in->soc_c10 > pk.soc)
        pk.soc = in->soc_c10;
    if (in->gauge_ok) {
        int a = in->ma < 0 ? -in->ma : in->ma;
        if (a > pk.ma)
            pk.ma = a;
        if (in->mv > 0 && in->mv < pk.mv)
            pk.mv = in->mv;
    }
    if (in->now < pk.next)
        return;
    pk.next = in->now + 10000;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "heater: ");
    ub_dec(&u, (in->now - pk.t0) / 1000U);
    ub_s(&u, " s bat ");
    ub_c10(&u, in->bat_ok, in->bat_c10);
    ub_s(&u, " skin ");
    ub_c10(&u, in->skin_ok, in->skin_c10);
    ub_s(&u, " soc ");
    ub_c10(&u, in->soc_ok, in->soc_c10);
    ub_s(&u, " C, cores ");
    ub_dec(&u, (uint64_t)cores_running());
    ub_s(&u, ", ");
    ub_dec(&u, hw_a55_khz() / 1000U);
    ub_s(&u, "/");
    ub_dec(&u, hw_m3_khz() / 1000U);
    ub_s(&u, " MHz, ");
    ub_dec(&u, (uint64_t)(in->mv > 0 ? in->mv : 0));
    ub_s(&u, " mV ");
    ub_sdec(&u, in->ma);
    ub_s(&u, " mA ");
    ub_dec(&u, (uint64_t)(in->pct > 0 ? in->pct : 0));
    ub_s(&u, " %");
    sb_log(u.b);
}

static void run_summary(void)
{
    if (!pk.on)
        return;
    pk.on = 0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "heater: peak bat ");
    ub_c10(&u, pk.bat > -10000, pk.bat);
    ub_s(&u, " skin ");
    ub_c10(&u, pk.skin > -10000, pk.skin);
    ub_s(&u, " soc ");
    ub_c10(&u, pk.soc > -10000, pk.soc);
    ub_s(&u, " C, max ");
    ub_dec(&u, (uint64_t)pk.ma);
    ub_s(&u, " mA, min ");
    ub_dec(&u, (uint64_t)(pk.mv < 100000 ? pk.mv : 0));
    ub_s(&u, " mV, ran ");
    ub_dec(&u, (sb_ms() - pk.t0) / 1000U);
    ub_s(&u, " s");
    sb_log(u.b);
}

static void restore_and_check(void)
{
    for (unsigned w = 1; w < 8; ++w)
        cfg.run[w] = 0;
    sb_dcache_clean_inval(&cfg, sizeof(cfg));
    if (m3_set) {
        (void)clock_set(DVFS_M3, M3_RESTORE);
        m3_set = 0;
    }
    hw_heat_stop_all();
    for (unsigned w = 1; w < 8; ++w)
        running[w] = 0;
    if (a55_set) {
        (void)clock_set(DVFS_A55, A55_RESTORE);
        a55_set = 0;
    }
    stopping = 1;
    stop_t = sb_ms();
    for (unsigned w = 0; w < 8; ++w)
        parked_logged[w] = 0;
    sb_logf("heater: stopped: ", ctl.why[0] ? ctl.why : "?");
    run_summary();
}

/* every worker core must be off within 1.5 s of a stop */
static void core_log(const char *what, unsigned w, int64_t psci, unsigned slot)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, what);
    ub_dec(&u, w);
    ub_s(&u, ": psci ");
    ub_hex(&u, (uint64_t)psci, 8);
    ub_s(&u, ", slot ");
    ub_dec(&u, slot);
    sb_log(u.b);
}

static void stop_check(void)
{
    int all = 1, late = sb_ms() - stop_t >= 1500;
    for (unsigned w = 1; w < 8; ++w) {
        const struct sb_wslot *s = sb_smp_slot(w);
        unsigned slot = s ? (unsigned)s->state : 0;
        int64_t r = hw_core_psci(w);
        int st = heat_core_state(r, slot, start_fail[w]);
        if (st == 0) {
            all = 0;
            if (late)
                core_log("heater: worker still loading or unknown, core ", w, r, slot);
        } else if (st == 2 && !parked_logged[w]) {
            parked_logged[w] = 1;
            core_log("heater: worker left the load, PSCI still says on (parked), core ", w, r, slot);
        }
    }
    if (all) {
        stopping = 0;
        sb_log("heater: every worker core left the load");
        if (hw_m3_on())
            sb_log("heater: the M3 cluster still reads powered after the stop");
        return;
    }
    if (late) {
        sb_log("heater: a worker core did not turn off: restarting the phone");
        (void)hw_dvfs(DVFS_A55, A55_RESTORE);
        sb_reboot(SB_INFORM_NORMAL);
    }
}

void heater_stop_all(const char *why)
{
    int was = heater_running_ui();
    int any = a55_set || m3_set;
    for (unsigned w = 1; w < 8; ++w)
        any |= running[w];
    if (!was && !any)
        return;                                 /* nothing to stop (a restart, a power-off) */
    if (was) {
        ctl.state = HEAT_ST_STOPPED;
        ctl.cores = 0;
        unsigned i = 0;
        for (; why[i] && i + 1 < sizeof(ctl.why); ++i)
            ctl.why[i] = why[i];
        ctl.why[i] = 0;
    }
    restore_and_check();
    kt_cancel(KT_HEATER, 1);
    kt_set(KT_HEATER, 2, sb_ms(), 250, 1);          /* the stop checks */
    app_event_send(app_by_id("heat"), EV_APP_SERVICE, 1, ctl.state);
    ui_mark_dirty();
}

void heater_timer(unsigned id)
{
    if (id == 2) {
        if (stopping)
            stop_check();
        if (!stopping)
            kt_cancel(KT_HEATER, 2);
        return;
    }
    if (!heater_running_ui()) {
        kt_cancel(KT_HEATER, 1);
        return;
    }
    struct heat_in in;
    read_inputs(&in);
    last_in = in;
    int before = ctl.state;
    run_track(&in);
    heat_ctl_step(&ctl, &in);
    if (!heater_running_ui()) {
        restore_and_check();
        kt_cancel(KT_HEATER, 1);
        kt_set(KT_HEATER, 2, sb_ms(), 250, 1);
    } else {
        /* the heartbeat: only after a complete, valid evaluation */
        cfg.heartbeat = sb_ticks();
        sb_dcache_clean_inval(&cfg, sizeof(cfg));
        workers_apply();
        clocks_apply();
    }
    if (before != ctl.state) {
        if (heater_running_ui())
            sb_logf("heater: ", ctl.why);       /* holding / paused / heating again (a stop logs itself) */
        app_event_send(app_by_id("heat"), EV_APP_SERVICE, 1, ctl.state);
    }
    ui_mark_dirty();
}

static void start_now(void)
{
    struct heat_in in;
    read_inputs(&in);
    last_in = in;
    if (stopping) {
        ctl.state = HEAT_ST_REFUSED;
        sb_strcpy(ctl.why, sizeof(ctl.why), "Still stopping the last session");
        return;
    }
    if (heat_ctl_request(&ctl, pending_level, pending_minutes, &in) == 0) {
#ifndef SB_HOST
        __asm__ volatile("msr fpcr, xzr" ::: "memory");   /* the reference in the workers' FP mode */
#endif
        cfg.gold = heatk_batch();
        cfg.max_age = sb_freq();                    /* 1 s */
        cfg.heartbeat = sb_ticks();
        sb_dcache_clean_inval(&cfg, sizeof(cfg));
        if (kt_set(KT_HEATER, 1, sb_ms(), 250, 1)) {
            heater_stop_all("No timer for the heater");
            return;
        }
        sb_logx("heater: start, level ", (uint64_t)pending_level);
        workers_apply();
        clocks_apply();
    }
    app_event_send(app_by_id("heat"), EV_APP_SERVICE, 1, ctl.state);
    ui_mark_dirty();
}

static void confirm_done(int ok)
{
    if (!ok) {
        ctl.state = HEAT_ST_OFF;
        sb_strcpy(ctl.why, sizeof(ctl.why), "Not started");
        app_event_send(app_by_id("heat"), EV_APP_SERVICE, 1, ctl.state);
        return;
    }
    confirmed = 1;
    K.heat_confirmed = 1;
    start_now();
}

int heater_request(int level, int minutes)
{
    if (level <= 0) {
        if (heater_running_ui()) {
            heat_ctl_request(&ctl, 0, 0, &last_in);
            sb_log("heater: Off (asked)");
            restore_and_check();
            kt_cancel(KT_HEATER, 1);
            kt_set(KT_HEATER, 2, sb_ms(), 250, 1);
            app_event_send(app_by_id("heat"), EV_APP_SERVICE, 1, ctl.state);
        }
        return 0;
    }
    if (level > HEAT_HIGH)
        return SBOS_EINVAL;
    if (heater_running_ui()) {
        /* a level change: the controller keeps its state, the new ceiling applies */
        ctl.level = level;
        ctl.max_cores = heat_max_cores(level);
        if (ctl.cores > ctl.max_cores)
            ctl.cores = ctl.max_cores;
        workers_apply();
        return 0;
    }
    pending_level = level;
    pending_minutes = minutes;
    if (!confirmed) {
        ctl.state = HEAT_ST_CONFIRM;
        kui_confirm("Use the heater?",
                    "The phone makes heat by running its processors at full speed.",
                    "It stops by itself at 42 \xc2\xb0" "C battery, 46 \xc2\xb0" "C on the back or 15 % battery.",
                    "It uses a lot of battery: about 1 % a minute on High.",
                    "Hold to allow", confirm_done);
        return 0;
    }
    start_now();
    return ctl.state == HEAT_ST_RUNNING ? 0 : SBOS_EAGAIN;
}

void heater_status(struct sbos_heat *h)
{
    memset(h, 0, sizeof(*h));
    h->state = ctl.state;
    h->level = ctl.level;
    int n = 0;
    for (unsigned w = 1; w < 8; ++w)
        n += running[w];
    h->cores = n;
    h->bat_c10 = last_in.bat_ok ? last_in.bat_c10 : -1000;
    h->soc_c10 = last_in.soc_ok ? last_in.soc_c10 : -1000;
    uint64_t now = sb_ms();
    h->secs_left = heater_running_ui() && ctl.t_end > now ? (int32_t)((ctl.t_end - now) / 1000U) : 0;
    h->secs_run = heater_running_ui() ? (int32_t)((now - ctl.t_start) / 1000U) : 0;
    h->a55_mhz = (int32_t)(hw_a55_khz() / 1000U);
    h->m3_mhz = (int32_t)(hw_m3_khz() / 1000U);
    sb_strcpy(h->why, sizeof(h->why), ctl.why);
}

void heater_init(void)
{
    heat_ctl_init(&ctl, &heat_default_limits);
    memset(&cfg, 0, sizeof(cfg));
    struct heat_in in;
    read_inputs(&in);
    last_in = in;
}
