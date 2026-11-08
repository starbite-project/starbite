/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS heater service (docs/STARBITE-OS.md 8): reads the sensors at 4 Hz, runs the controller
 * (heatctl.c) and applies what it decides: the worker cores, the listed ACPM DVFS levels with a PLL readback,
 * the M3 cluster's PMU defaults (after the first-use confirmation), the heartbeat, and the checks after a stop
 * (every worker core off within 1.5 s, else the clocks are restored and the phone restarts).
 * Nothing here writes storage, the PMIC or the charger. The TMU only in Survival with the trip off (1.1): its
 * CONTROL registers' THERM_TRIP_EN (bit 12) cleared at the start, read back, and put back exactly at every end. */
#include "sb.h"
#include "kern.h"
#ifdef SB_HOST
#include <stdlib.h>
#endif

#define A55_RESTORE 1053000U
#define M3_RESTORE 1066000U

static struct heat_ctl ctl;
static struct heatk_cfg cfg __attribute__((aligned(64)));
/* Worker cores (1..7): launched with one PSCI CPU_ON and checked on later ticks, never waited for (0.2 on the
 * phone: a core that never checked in made sb_smp_start block the loop 1 s per tick, the other workers' dead man's
 * switch fired, the UI starved). A failure is retried after 2 s, then 4 s; the third disables the core until a
 * restart; the reason and the PSCI code are logged once per failure. */
enum { CS_OFF = 0, CS_LAUNCHED, CS_RUN, CS_FAILED, CS_DISABLED };
static struct {
    int st, fails, busy;                    /* busy: PSCI ALREADY_ON / ON_PENDING answers in a row */
    uint64_t t;                             /* launched at / next try at */
    int64_t code;
    int maybe_hung;                         /* CPU_ON said yes, the core never checked in */
    uint64_t mpidr;                         /* what the core wrote into its slot at check-in */
} cs[8];
static int hung_ever;                       /* a core never checked in (or was still starting at a stop) */
static int running[8];                      /* the heat kernel runs on core w (cs == CS_RUN) */
static int charger_now = -1, charger_seen = -2, charger_raw = -1;   /* unknown until two reads agree */
static int gpu_probe_ok;
static int trip_top_c;                      /* the hardware trip's decoded temperature (logged at a start) */
static void record(const struct heat_in *in, int running);
static uint64_t rec_saved_at;
static uint64_t rec_t, rec_uwh;             /* 1.1-beta4: the energy integral of this session */
static int launch_hold;
static int ctx_ok;                          /* 1 workers use their own start records, -1 the shared one, 0 unknown */                     /* a core never checked in: no more launches this session */
static int64_t last_code;                   /* the latest failure's code, for the screen */
static int cpu0_on;                         /* cpu0 heats between the kernel's events */
static uint64_t cpu0_it, cpu0_bad;
static int parked_logged[8];
static uint32_t a55_set, m3_set;            /* what we asked ACPM for (0: never) */
static int m3_prepped, confirmed;
static uint64_t bad0[8];
static int pending_level, pending_minutes;
static struct heat_in last_in;
static int stopping;                        /* stop checks in progress */
static uint64_t stop_t;
/* 1.1: Survival with the chip's hardware heat trip off */
static int trip_off;                        /* the trip is off now (we cleared it) */
static uint32_t trip_saved[2];              /* TMU_TOP / TMU_SUB CONTROL as S-Boot left them */
static uint64_t trip_restore_due;           /* after a stop: back on once cool, at the latest at this time */
static int surv_step;                       /* the confirmation being shown (0 .. 2 / 3) */
static int trip_tried;                      /* 1.1-beta3: this session's trip-off attempt was made */

static uint32_t mrd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static void mwr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void trip_log(const char *what)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, what);
    ub_s(&u, ": TMU_TOP CONTROL ");
    ub_hex(&u, mrd(SB_TMU_TOP + 0x20), 8);
    ub_s(&u, ", TMU_SUB CONTROL ");
    ub_hex(&u, mrd(SB_TMU_SUB + 0x20), 8);
    sb_log(u.b);
}

/* the trip off: only with the short watchdog in place, read back; 0 with why (nothing changed) */
static int trip_disable(const char **why)
{
    uint32_t t = mrd(SB_TMU_TOP + 0x20), s = mrd(SB_TMU_SUB + 0x20);
    trip_log("heater: Survival (trip off): before");
    if (!(t & 0x1000U)) {
        *why = "The chip's heat trip was not armed (S-Boot's)";
        return 0;
    }
    unsigned ms = hw_wdt_short(5);
    if (!ms) {
        *why = "No watchdog to back it (the loader's must count, its reset unmasked)";
        return 0;
    }
    sb_logx("heater: Survival (trip off): the watchdog at ms, fed only by the heat loop: ", ms);
    trip_saved[0] = t;
    trip_saved[1] = s;
    mwr(SB_TMU_TOP + 0x20, t & ~0x1000U);
    if (s & 0x1000U)
        mwr(SB_TMU_SUB + 0x20, s & ~0x1000U);
#ifdef SB_HOST
    if (getenv("SBOS_HOST_TMU_RO")) {           /* the host's model of a secure (not writable) TMU */
        mwr(SB_TMU_TOP + 0x20, t);
        mwr(SB_TMU_SUB + 0x20, s);
    }
#endif
    uint32_t t2 = mrd(SB_TMU_TOP + 0x20), s2 = mrd(SB_TMU_SUB + 0x20);
    trip_log("heater: Survival (trip off): after the write");
    if ((t2 & 0x1000U) || (s2 & 0x1000U)) {
        mwr(SB_TMU_TOP + 0x20, t);
        mwr(SB_TMU_SUB + 0x20, s);
        (void)hw_wdt_short(0);
        sb_log("heater: Survival (trip off): the TMU's CONTROL did not take the write: not writable from EL1 here "
               "(secure / TZPC?); the trip stays on");
        *why = "The chip's heat trip cannot be switched off from here";
        return 0;
    }
    trip_off = 1;
    ctl.trip_off = 1;
    trip_restore_due = 0;
    sb_log("heater: Survival (trip off): the chip's hardware heat trip is OFF; our stop at the user's SoC reading");
    return 1;
}

/* the trip back exactly as S-Boot left it, read back; then the loader's watchdog reload */
static void trip_restore(const char *when)
{
    if (!trip_off)
        return;
    mwr(SB_TMU_TOP + 0x20, trip_saved[0]);
    mwr(SB_TMU_SUB + 0x20, trip_saved[1]);
    uint32_t t = mrd(SB_TMU_TOP + 0x20), s = mrd(SB_TMU_SUB + 0x20);
    trip_off = 0;
    ctl.trip_off = 0;
    trip_restore_due = 0;
    sb_logf("heater: the chip's hardware heat trip back on: ", when);
    if (t != trip_saved[0] || s != trip_saved[1])
        trip_log("heater: THE TRIP DID NOT READ BACK AS SAVED");
    else
        trip_log("heater: restored");
    (void)hw_wdt_short(0);
    sb_log("heater: the watchdog back to the loader's");
}

static int soc_now_c10(void)
{
    struct sb_temps t;
    sb_temps_read(&t);
    return t.top == 1 && t.hottest > -30 && t.hottest < 150 ? t.hottest * 10 : 2000;
}

/* the loads are already off: wait (the heat loop's feed) until the reading is below Survival's 122 C, at most until
 * due, so the trip does not cut the power the moment it is back on */
static void trip_restore_when_cool(int force)
{
    if (!trip_off)
        return;
    hw_wdt_feed_heat();
    int c = soc_now_c10();
    if (force || c < heat_default_limits.surv_target_c10 || sb_ms() >= trip_restore_due)
        trip_restore(c < heat_default_limits.surv_target_c10 ? "the processor is below 122 C" :
                     force ? "a restart or a power-off" : "10 s after the stop (still hot: the trip may act now)");
}

void heater_trip_restore_now(void)
{
    if (!trip_off)
        return;
    uint64_t until = sb_ms() + 3000U;
    while (trip_off && sb_ms() < until && soc_now_c10() >= heat_default_limits.surv_target_c10) {
        hw_wdt_feed_heat();
        sb_mdelay(50);
    }
    trip_restore_when_cool(1);
}

int heater_busy(void)
{
    return heater_running_ui() || ctl.state == HEAT_ST_CONFIRM;
}

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
    gpu_probe_ok = t.sub == 1 && t.gpu > -30 && t.gpu < 150;
    if (gpuheat_on() && gpu_probe_ok && t.gpu * 10 > in->soc_c10)
        in->soc_c10 = t.gpu * 10;               /* the GPU's zone (TMU_SUB) counts while it heats */
    in->trip_armed = (t.trip_armed & 1) && (!gpuheat_on() || (t.trip_armed & 2));   /* the GPU's zone too while it runs */
    trip_top_c = t.trip_c;
    struct sb_bat b;
    in->gauge_ok = sb_bat_read(&b) && b.ok;
    in->pct = b.soc_pct;
    in->mv = b.mv;
    in->ma = b.ma;
    /* the charger (CHG_INT_OK 0xB2: bit 6 CHGIN_OK, bit 5 WCIN_OK): a change counts after two reads in a row
     * agree; a failed read counts as "unknown"; every change is logged with the raw byte */
    uint8_t c;
    int now_c = sb_chg_read(0xb2, &c) ? ((c & 0x60U) ? 1 : 0) : -1;
    if (now_c == charger_seen && now_c != charger_now) {
        charger_now = now_c;
        struct ub u;
        ub_init(&u);
        ub_s(&u, "heater: charger ");
        ub_s(&u, now_c > 0 ? "connected" : now_c < 0 ? "unreadable" : "not connected");
        ub_s(&u, ", CHG_INT_OK ");
        ub_hex(&u, now_c < 0 ? 0 : c, 2);
        ub_s(&u, ", current ");
        ub_sdec(&u, b.ma);
        ub_s(&u, " mA");
        sb_log(u.b);
    }
    charger_seen = now_c;
    charger_raw = now_c < 0 ? -1 : c;
    in->charger = charger_now;
    if (cpu0_bad)
        in->bad_results = 1;
    for (unsigned w = 1; w < 8; ++w) {
        const struct sb_wslot *s = sb_smp_slot(w);
        if (!s || cs[w].st != CS_RUN)
            continue;
        if (s->mpidr != cs[w].mpidr) {
            /* a second core wrote this slot: a late core took this core's start record (shared records) */
            sb_logx("heater: a second core took worker's start record, mpidr now ", s->mpidr);
            in->worker_fault = 1;
        }
        if (s->errors > bad0[w])
            in->bad_results = 1;
        if (s->state == SB_WK_FAULT)
            in->worker_fault = 1;
    }
}

/* 1.1-beta6: the M3 cores that run or may run now (launched, ours, or still in the heat kernel after a stop
 * request): what Samsung's core-count table is checked against */
static unsigned m3_busy(void)
{
    unsigned n = 0;
    for (unsigned v = 4; v < 8; ++v) {
        const struct sb_wslot *s = sb_smp_slot(v);
        n += running[v] || cs[v].st == CS_LAUNCHED || (s && s->state == SB_WK_RUNNING);
    }
    return n;
}

/* ask ACPM, then check the PLL within 5 ms: more than 2 % above the request is a stop */
static int clock_set(unsigned domain, uint32_t khz)
{
    sb_logx(domain == DVFS_A55 ? "heater: step: ACPM A55 kHz " : "heater: step: ACPM M3 kHz ", khz);
    if (!(domain == DVFS_M3 && khz > 1794000U ? hw_dvfs_m3_ocp(khz, m3_busy()) : hw_dvfs(domain, khz))) {
        sb_log("heater: step: ACPM did not answer");
        return 0;
    }
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

static int cores_running(void);

static void core_fail(unsigned w, const char *why, int64_t code, uint64_t now)
{
    struct ub u;
    cs[w].fails++;
    cs[w].code = code;
    last_code = code;
    cfg.run[w] = 0;
    sb_dcache_clean_inval(&cfg, sizeof(cfg));
    running[w] = 0;
    ub_init(&u);
    ub_s(&u, "heater: core ");
    ub_dec(&u, w);
    ub_s(&u, ": ");
    ub_s(&u, why);
    ub_s(&u, " (code ");
    ub_sdec(&u, code);
    ub_s(&u, ")");
    if (cs[w].fails >= 3) {
        cs[w].st = CS_DISABLED;
        ub_s(&u, ": not used until a restart");
    } else {
        cs[w].st = CS_FAILED;
        cs[w].t = now + (1000U << cs[w].fails);
        ub_s(&u, ": trying again later");
    }
    sb_log(u.b);
}

/* how many of the level's cores can still run (not disabled) */
static int cores_usable(int upto)
{
    int n = 0;
    for (int w = 1; w <= upto && w < 8; ++w)
        n += cs[w].st != CS_DISABLED;
    return n;
}

int heater_trip_off(void)
{
    return trip_off;
}

/* 1.1-beta2: M3 workers that run (or are starting) now, but w */
static int m3_others(unsigned w)
{
    int n = 0;
    for (unsigned v = 4; v < 8; ++v)
        n += v != w && (running[v] || cs[v].st == CS_LAUNCHED);
    return n;
}

static void workers_apply(void)
{
    uint64_t now = sb_ms();
    int want = ctl.cores;
    /* the M3 domain back to its restore level while an M3 still runs, before the last one leaves */
    int m3_after = 0;
    for (unsigned w = 4; w < 8; ++w)
        m3_after |= running[w] && (int)w <= want;
    if (!m3_after && m3_set) {
        (void)clock_set(DVFS_M3, M3_RESTORE);
        m3_set = 0;
    }
    int launched = 0;
    for (unsigned w = 1; w < 8; ++w)
        if (cs[w].st == CS_LAUNCHED)
            launched = 1;                           /* the start record is in use until it checks in */
    for (unsigned w = 1; w < 8; ++w) {
        const struct sb_wslot *s = sb_smp_slot(w);
        unsigned slot = s ? (unsigned)s->state : 0;
        int want_on = (int)w <= want;
        switch (cs[w].st) {
        case CS_RUN:
            if (!want_on) {
                if (w >= 4)
                    hw_m3_core_off(w, !m3_others(w));   /* before it sees run = 0 and calls CPU_OFF */
                cfg.run[w] = 0;
                sb_dcache_clean_inval(&cfg, sizeof(cfg));
                running[w] = 0;
                cs[w].st = CS_OFF;
            } else if (slot == SB_WK_DONE || slot == SB_WK_PARKED) {
                running[w] = 0;                     /* it left by itself (the dead man's switch) */
                core_fail(w, s && s->v[0] ? "left the load (no heartbeat)" : "left the load", 0, now);
            }
            break;
        case CS_LAUNCHED:
            if (slot == SB_WK_RUNNING) {
                /* the one start record is shared: a late core could have taken a newer core's record; two
                 * running cores reporting one MPIDR would share a stack */
                for (unsigned v = 1; v < 8; ++v) {
                    const struct sb_wslot *o = sb_smp_slot(v);
                    if (v != w && cs[v].st == CS_RUN && o && o->mpidr == s->mpidr) {
                        sb_logx("heater: two workers report one MPIDR: stopping, mpidr ", s->mpidr);
                        heater_stop_all("Two cores took one start record: stopped. A restart fixes it.");
                        return;
                    }
                }
                if (!ctx_ok) {
                    ctx_ok = sb_smp_own_record(w) ? 1 : -1;
                    sb_log(ctx_ok > 0 ? "heater: the firmware passes PSCI's context id: every core has its own start record"
                                      : "heater: the firmware drops PSCI's context id: the cores share one start record");
                }
                cs[w].st = CS_RUN;
                cs[w].busy = 0;
                cs[w].fails = 0;
                cs[w].mpidr = s->mpidr;
                bad0[w] = s->errors;
                running[w] = 1;
                {
                    struct ub u;
                    ub_init(&u);
                    ub_s(&u, "heater: core ");
                    ub_dec(&u, w);
                    ub_s(&u, w >= 4 ? " (M3) running, " : " (A55) running, ");
                    ub_dec(&u, (uint64_t)cores_running() + (cpu0_on ? 0U : 1U));
                    ub_s(&u, " cores with cpu0");
                    sb_log(u.b);
                }
                if (!want_on) {
                    if (w >= 4)
                        hw_m3_core_off(w, !m3_others(w));
                    cfg.run[w] = 0;
                    sb_dcache_clean_inval(&cfg, sizeof(cfg));
                    running[w] = 0;
                    cs[w].st = CS_OFF;
                }
            } else if (slot == SB_WK_FAULT) {
                core_fail(w, "faulted at start", -101, now);
            } else if (now - cs[w].t >= 1000U) {
                cs[w].maybe_hung = 1;
                hung_ever = 1;
                /* without its own start record it may still boot late and read whatever shared record is current
                 * then: no more launches in this session (the running cores go on) */
                if (ctx_ok < 0) {
                    launch_hold = 1;
                    core_fail(w, "no check-in within 1 s; no more cores start in this session", -100, now);
                } else {
                    core_fail(w, "no check-in within 1 s", -100, now);
                }
            }
            break;
        case CS_FAILED:
            if (now >= cs[w].t)
                cs[w].st = CS_OFF;
            break;
        default:
            break;
        }
        if (!want_on || cs[w].st != CS_OFF || launched || launch_hold || slot == SB_WK_RUNNING)
            continue;
        if (w >= 4 && !m3_prepped) {
            if (!hw_m3_prep()) {
                heater_stop_all("The big cores could not be prepared");
                return;
            }
            m3_prepped = 1;
        }
        if (w >= 4) {
            /* 1.1-beta6: never a core more than Samsung's table allows at the M3's present clock */
            uint32_t now_khz = hw_m3_khz();
            if (m3_set > now_khz)
                now_khz = m3_set;
            if (now_khz > sb_m3_ocp_max_khz(m3_busy() + 1U))
                continue;                   /* clocks_apply lowers the clock first (this tick), then the next tick */
            int first = !m3_others(w);
            if (first)
                hw_m3_fp_log("heater: flexpmu before the first M3 start");
            hw_m3_core_on(w, first);
        }
        cfg.run[w] = 1;
        sb_dcache_clean_inval(&cfg, sizeof(cfg));
        sb_logx("heater: step: PSCI CPU_ON core ", w);       /* 1.1-beta3: breadcrumbs for a hang */
        int64_t r = hw_heat_launch(w, &cfg);
        sb_logx("heater: step: PSCI CPU_ON returned, core ", w);
        if (r == 0) {
            launched = 1;                           /* one successful CPU_ON per tick */
            cs[w].st = CS_LAUNCHED;
            cs[w].t = now;
        } else if (r == -4 && slot == SB_WK_PARKED) {
            /* its last CPU_OFF returned (it waits in wfe) and PSCI calls it on: it cannot be started again
             * before a restart */
            cfg.run[w] = 0;
            cs[w].fails = 2;
            core_fail(w, "parked and PSCI says it is still on", r, now);
        } else if ((r == -4 || r == -5) && ++cs[w].busy < 8) {
            cfg.run[w] = 0;                         /* the last CPU_OFF is not done yet: the next tick */
        } else {
            cs[w].busy = 0;
            core_fail(w, r == -4 ? "PSCI says it is still on" : r == -5 ? "PSCI: on pending" :
                         r == -3 ? "PSCI denied the start" : "PSCI refused the start", r, now);
        }
    }
    /* nothing can run at this level: stop with the reason on the screen */
    if (cores_usable(ctl.max_cores) == 0) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "The processor cores did not start (code ");
        ub_sdec(&u, last_code);
        ub_s(&u, "). A restart fixes it.");
        heater_stop_all(u.b);
    }
}

static void clocks_apply(void)
{
    if (ctl.a55_khz && ctl.a55_khz != a55_set) {
        power_heater_took_clocks();         /* the screen-off 455 MHz is the heater's to change now */
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
        /* 1.1-beta6: up only when no more M3 cores run than Samsung's table allows at the new clock (they leave a
         * tick after their stop request); down always at once */
        if (ctl.m3_khz > 1794000U && m3_busy() > (ctl.m3_khz <= 2314000U ? 2U : 1U)) {
            static uint64_t said;
            if (sb_ms() - said > 2000U) {
                said = sb_ms();
                sb_logx("heater: M3 clock up waits for the extra cores to leave, busy ", m3_busy());
            }
            return;
        }
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
    int n = cpu0_on;
    for (unsigned w = 1; w < 8; ++w)
        n += running[w];
    return n;
}

int heater_cpu0_active(void)
{
    return ctl.state == HEAT_ST_RUNNING && ctl.cores > 0 && !stopping;
}

/* cpu0's share: streaming batches (about 0.5 ms each) until the next kernel deadline, at most 20 ms */
void heater_cpu0_run(uint64_t until_ms)
{
    uint64_t t0 = sb_ms();
    if (until_ms > t0 + 20U)
        until_ms = t0 + 20U;
    cpu0_on = 1;
    do
        cpu0_bad += heatk_mixed(&cfg, 0, cpu0_it++);
    while (sb_ms() < until_ms);
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
    ub_s(&u, " MHz");
    if (gpuheat_on()) {
        ub_s(&u, ", gpu ");
        ub_dec(&u, gpuheat_mhz());
    }
    ub_s(&u, ", ");
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
    ub_s(&u, " s, peak ");
    ub_dec(&u, (uint64_t)(heat_rec.peak_mw / 1000));
    ub_c(&u, '.');
    ub_dec(&u, (uint64_t)(heat_rec.peak_mw % 1000 / 100));
    ub_s(&u, " W, ");
    ub_dec(&u, (uint64_t)heat_rec.mwh);
    ub_s(&u, " mWh");
    sb_log(u.b);
}

/* Extreme's extra heat (0.6), all within ratings: the panel at its top brightness level and the LED on (its torch
 * current: the GPIO driver has one); both put back when Extreme stops or holds */
static int ext_on, ext_led, ext_led_used;
static void extreme_confirm_done(int ok);

/* on for the whole Extreme session (through HOLDING: no switching every few seconds at the band); the LED once per
 * session (its own 10-minute limit then holds) */
static void extreme_extras(int on)
{
    if (on == ext_on)
        return;
    ext_on = on;
    if (on) {
        panel_boost(1, 1);
        if (!ext_led_used) {
            ext_led_used = 1;
            ext_led = !torch_on() && torch_ready() && torch_set(1) == 0;   /* 240 mA with Settings' full LED (0.6.3) */
        }
        sb_log(ext_led ? "heater: Extreme: the panel at its top level, the LED on" : "heater: Extreme: the panel at its top level");
    } else {
        panel_boost(1, 0);
        if (ext_led)
            torch_set(0);
        ext_led = 0;
        sb_log("heater: Extreme's panel level and LED put back");
    }
}

static void restore_and_check(void)
{
    int was_recording = heat_rec.running != 0;
    extreme_extras(0);
    ext_led_used = 0;                       /* a new session may light it again */
    gpu_heat_session = 0;
    {
        /* 1.1-beta2: the M3s' FlexPMU off before they see the stop: C2 for each, CPD for the last */
        int left = 0;
        for (unsigned w = 4; w < 8; ++w)
            left += running[w] || cs[w].st == CS_LAUNCHED;
        for (unsigned w = 4; w < 8; ++w)
            if (running[w] || cs[w].st == CS_LAUNCHED)
                hw_m3_core_off(w, --left == 0);
    }
    for (unsigned w = 1; w < 8; ++w)
        cfg.run[w] = 0;
    sb_dcache_clean_inval(&cfg, sizeof(cfg));
    if (m3_set) {
        (void)clock_set(DVFS_M3, M3_RESTORE);
        m3_set = 0;
    }
    gpuheat_stop();
    hw_heat_stop_all();
    if (trip_off && !trip_restore_due) {
        trip_restore_due = sb_ms() + 10000U;    /* the loads are off: the trip back once below 122 C (<= 10 s) */
        sb_log("heater: Survival (trip off) stopped: the trip comes back on as the processor cools (<= 10 s)");
    }
    cpu0_on = 0;
    cpu0_bad = 0;
    for (unsigned w = 1; w < 8; ++w) {
        running[w] = 0;
        if (cs[w].st == CS_LAUNCHED) {          /* still starting at the stop: it may check in later */
            cs[w].maybe_hung = 1;
            hung_ever = 1;
        }
        if (cs[w].st != CS_DISABLED) {          /* a new session tries every usable core again */
            cs[w].st = CS_OFF;
            cs[w].busy = 0;
        }
    }
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
    if (was_recording)
        record(&last_in, 0);                /* 1.0-rc2 (review): the stop's record after the hardware is off */
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
        int st = heat_core_state(r, slot, cs[w].maybe_hung);
        if (st == 0) {
            all = 0;
            if (late)
                core_log("heater: worker still loading or unknown, core ", w, r, slot);
        } else if (st == 2 && !parked_logged[w]) {
            parked_logged[w] = 1;
            core_log(cs[w].maybe_hung ? "heater: a core never checked in (no load of ours), core " :
                                        "heater: worker left the load, PSCI still says on (parked), core ", w, r, slot);
        }
    }
    if (all) {
        stopping = 0;
        sb_log("heater: every worker core left the load");
        if (hw_m3_on())
            sb_log("heater: the M3 cluster still reads powered after the stop");
        hw_m3_fp_log("heater: flexpmu after the stop");
        return;
    }
    if (late) {
        sb_log("heater: a worker core did not turn off: restarting the phone");
        heater_trip_restore_now();
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
        ctl.a55_khz = 0;
        ctl.m3_khz = 0;
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
        trip_restore_when_cool(0);              /* 1.1: also feeds the short watchdog until the trip is back */
        if (!stopping && !trip_off)
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
    int cal_before = ctl.cal;
    heat_ctl_step(&ctl, &in);
    if (ctl.level >= HEAT_SURVIVAL && ctl.cal != cal_before) {
        /* 1.1-beta6: each measured M3 combination and the pick */
        if (cal_before >= 1 && cal_before <= 4) {
            const struct heat_m3_cand *k = &heat_m3_cands[cal_before - 1];
            struct ub u;
            ub_init(&u);
            ub_s(&u, "heater: survival M3 candidate ");
            ub_dec(&u, (uint64_t)k->n);
            ub_s(&u, " @ ");
            ub_dec(&u, k->khz / 1000U);
            ub_s(&u, " MHz (+ A55s, GPU, panel): ");
            ub_dec(&u, (uint64_t)(ctl.cal_mw[cal_before - 1] / 1000));
            ub_c(&u, '.');
            ub_dec(&u, (uint64_t)(ctl.cal_mw[cal_before - 1] % 1000 / 100));
            ub_s(&u, " W");
            sb_log(u.b);
        }
        if (ctl.cal == 5) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "heater: survival M3 pick: ");
            ub_dec(&u, (uint64_t)ctl.m3_n);
            ub_s(&u, " @ ");
            ub_dec(&u, ctl.surv_m3_khz / 1000U);
            ub_s(&u, cal_before >= 1 && cal_before <= 4 && ctl.cal_mw[cal_before - 1] ? " MHz (the most watts)" :
                                                                                     " MHz (measurement cut short)");
            sb_log(u.b);
        }
    }
    if (heater_running_ui())
        record(&in, 1);
    if (!heater_running_ui()) {
        restore_and_check();
        kt_cancel(KT_HEATER, 1);
        kt_set(KT_HEATER, 2, sb_ms(), 250, 1);
    } else {
        /* the heartbeat: only after a complete, valid evaluation */
        cfg.heartbeat = sb_ticks();
        if (trip_off)
            hw_wdt_feed_heat();                 /* 1.1: the only feed while the chip's trip is off */
        /* 1.1-beta3: the trip goes off only here, after a whole loop turn of the session (the start's PSCI and ACPM
         * calls ran with the trip on and the loader's 30 s watchdog), then the short watchdog is fed at once */
        if (ctl.level == HEAT_SURVIVAL_X && !trip_off && !trip_tried && ctl.state == HEAT_ST_RUNNING) {
            const char *no = 0;
            trip_tried = 1;
            if (!trip_disable(&no)) {
                sb_logf("heater: Survival (trip off) refused: ", no);
                heater_stop_all(no);
                ctl.state = HEAT_ST_REFUSED;
                return;
            }
            hw_wdt_feed_heat();
            sb_log("heater: step: trip off, the watchdog fed; on with the loop");
        }
        sb_dcache_clean_inval(&cfg, sizeof(cfg));
        workers_apply();
        if (heater_running_ui())
            clocks_apply();
        /* the GPU on High while the load runs (not while holding or paused) */
        /* the GPU on High: on once 3 workers run, off only when the load stops (holding, paused, a stop), so the
         * regulation's core-by-core steps do not switch vdd_g3d up and down */
        extreme_extras((ctl.level == HEAT_EXTREME || ctl.level >= HEAT_SURVIVAL) && heater_running_ui());
        if (((ctl.level == HEAT_HIGH && gpu_heat_allowed) || (ctl.level >= HEAT_EXTREME && gpu_heat_session)) &&
            ctl.gpu_want && ctl.state == HEAT_ST_RUNNING) {
            if (!gpuheat_on() && cores_running() - cpu0_on >= 3)
            {
                sb_log("heater: step: gpu start");
                (void)gpuheat_start();
                sb_log("heater: step: gpu start returned");
            }
            else if (gpuheat_on() && !gpu_probe_ok)
                gpuheat_fail_stop("its temperature probe (TMU_SUB) does not read: the GPU's share stops");
        } else if (gpuheat_on()) {
            gpuheat_stop();
        }
        gpuheat_tick();
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
    if (wifi_on()) {                        /* Wi-Fi came on while the confirmation was up */
        sb_strcpy(ctl.why, sizeof(ctl.why), "Turn Wi-Fi off first (0.4: not both at once)");
        ctl.state = HEAT_ST_REFUSED;
        sb_log("heater: refused at the start: Wi-Fi is on");
        return;
    }
    struct heat_in in;
    read_inputs(&in);
    sb_mdelay(5);
    read_inputs(&in);                           /* the charger's reading needs two reads that agree */
    last_in = in;
    if (stopping) {
        ctl.state = HEAT_ST_REFUSED;
        sb_strcpy(ctl.why, sizeof(ctl.why), "Still stopping the last session");
        return;
    }
    if (pending_level != HEAT_MANUAL && pending_level < HEAT_SURVIVAL)
        gpuheat_khz = 572000U;              /* 1.0-rc2 (review): Manual's GPU step never carries over */
    if (heat_ctl_request(&ctl, pending_level, pending_minutes, &in) == 0) {
        /* 1.1-beta3: before this session's first PSCI or ACPM call: the M3 FlexPMU words as the boot had them */
        hw_m3_fp_log("heater: flexpmu at the start");
        if (hw_m3_fp_reset())
            hw_m3_fp_log("heater: flexpmu put back to the boot state");
#ifndef SB_HOST
        __asm__ volatile("msr fpcr, xzr" ::: "memory");   /* the reference in the workers' FP mode */
#endif
        cfg.gold = heatk_batch();
        cfg.max_age = 2U * sb_freq();               /* 2 s: the loop never blocks that long now */
        cfg.heartbeat = sb_ticks();
        sb_dcache_clean_inval(&cfg, sizeof(cfg));
        if (kt_set(KT_HEATER, 1, sb_ms(), 250, 1)) {
            heater_stop_all("No timer for the heater");
            return;
        }
        sb_logx("heater: start, level ", (uint64_t)pending_level);
        trip_tried = 0;
        memset(&heat_rec, 0, sizeof(heat_rec));
        heat_rec.peak_bat_c10 = heat_rec.peak_soc_c10 = -1000;
        rec_t = 0;
        rec_uwh = 0;
        heat_rec.trip_c = trip_top_c;
        heat_last_unclean = 0;
        rec_saved_at = 0;                   /* the first record at once */
        sb_logx("heater: the SoC's own hardware heat trip is armed (S-Boot's), decoded C: ", (uint64_t)trip_top_c);
        gpuheat_new_session();
        /* a new session tries every usable core again, except with shared start records after a core went
         * missing: it could still boot late and take a newer core's record */
        launch_hold = ctx_ok < 0 && hung_ever;
        last_code = 0;
        for (unsigned w = 1; w < 8; ++w)
            if (cs[w].st != CS_DISABLED) {
                cs[w].fails = 0;
                cs[w].maybe_hung = 0;
            }
        workers_apply();
        if (heater_running_ui())
            clocks_apply();
    } else {
        sb_logf("heater: refused: ", ctl.why);
    }
    app_event_send(app_by_id("heat"), EV_APP_SERVICE, 1, ctl.state);
    ui_mark_dirty();
}

/* Manual (0.6.1): the picked ACPM steps (kHz; 0 = the top normal step; GPU 0 = not used) */
uint32_t heat_man_a55, heat_man_m3, heat_man_gpu;
/* 0.6.2: the call / SOS reserve for Extreme / Manual / Keep warm (0 = overridden), Keep warm's battery target */
int heat_reserve_pct = 15, heat_target_c10 = 200, heat_no_current_stop;
int heat_reserve_override;                  /* 1.0-rc2 (review): the hold overrides the reserve for the next session only */
int heat_soc_stop_c10;                      /* 0.6.3: Manual's SoC stop (0 off: the user's default) */
int heat_surv_bat_c10 = 550;                /* 1.1-beta5: Survival's battery limit (55 C; 58 / 60 after a confirmation) */
int heat_surv_stop_c10 = 1350;              /* 1.1-beta4: Survival (trip off)'s SoC limit, held: 135 C (125..145) */
struct heat_record heat_rec, heat_last;
int heat_last_unclean;

/* 0.6.3: the session's record; saved every 30 s while heating and at the stop (where the slot exists) */
static void record(const struct heat_in *in, int running)
{
    heat_rec.magic = 0x31435248U;           /* "HRC1" */
    heat_rec.running = (uint32_t)running;
    heat_rec.level = ctl.level;
    heat_rec.secs = running || ctl.t_start ? (int32_t)((sb_ms() - ctl.t_start) / 1000U) : 0;
    heat_rec.cores = ctl.cores;
    if (in) {
        heat_rec.bat_c10 = in->bat_ok ? in->bat_c10 : -1000;
        heat_rec.skin_c10 = in->skin_ok ? in->skin_c10 : -1000;
        heat_rec.soc_c10 = in->soc_ok ? in->soc_c10 : -1000;
        heat_rec.ma = in->ma, heat_rec.mv = in->mv, heat_rec.pct = in->pct;
        if (in->bat_ok && in->bat_c10 > heat_rec.peak_bat_c10)
            heat_rec.peak_bat_c10 = in->bat_c10;
        if (in->soc_ok && in->soc_c10 > heat_rec.peak_soc_c10)
            heat_rec.peak_soc_c10 = in->soc_c10;
        if (in->gauge_ok && -in->ma > heat_rec.peak_ma)
            heat_rec.peak_ma = -in->ma;
        /* 1.1-beta4: the battery's power and the energy: V x I at each record (4 a second while heating) */
        uint64_t now = sb_ms();
        int mw = in->gauge_ok && in->ma < 0 && in->mv > 0 ? (int)((int64_t)-in->ma * in->mv / 1000) : 0;
        if (mw > heat_rec.peak_mw)
            heat_rec.peak_mw = mw;
        if (running && rec_t && now > rec_t && now - rec_t < 5000U)
            rec_uwh += (uint64_t)mw * (now - rec_t) / 3600U;  /* mW x ms / 3600 = uWh */
        rec_t = now;
        heat_rec.mwh = (int32_t)(rec_uwh / 1000U);
    }
    sb_strcpy(heat_rec.why, sizeof(heat_rec.why), ctl.why);
    if (!running || sb_ms() - rec_saved_at >= 30000U) {
        rec_saved_at = sb_ms();
        settings_changed();                 /* the slot's save, later (at most one write in 10 s; never in a stop's way) */
    }
}
#define FLAT_MA SBOS_HEAT_FULL_MA           /* 1.0-rc3: the measured full-heat draw (sbapp.h: 2.8 A, ~3 % a minute) */

static void extreme_confirm_done(int ok)
{
    if (!ok) {
        ctl.state = HEAT_ST_OFF;
        sb_strcpy(ctl.why, sizeof(ctl.why), "Not started");
        app_event_send(app_by_id("heat"), EV_APP_SERVICE, 1, ctl.state);
        return;
    }
    confirmed = 1;
    K.heat_confirmed = 1;
    ctl.reserve_pct = heat_reserve_override ? 0 : heat_reserve_pct;
    heat_reserve_override = 0;              /* used up by this start (a refusal asks again) */
    ctl.target_c10 = heat_target_c10;
    ctl.no_current_stop = pending_level == HEAT_MANUAL && heat_no_current_stop;
    ctl.soc_stop_c10 = pending_level == HEAT_MANUAL ? heat_soc_stop_c10 : 0;
    if (pending_level == HEAT_KEEPWARM) {
        ctl.man_a55 = ctl.man_m3 = 0;
        gpu_heat_session = 0;               /* the cores alone keep a battery warm */
        sb_log("heater: Keep warm confirmed (the battery's target)");
    } else if (pending_level >= HEAT_SURVIVAL) {
        ctl.man_a55 = ctl.man_m3 = 0;
        ctl.reserve_pct = 0;                /* Survival runs to 2 %: no reserve (its confirmations say so) */
        ctl.surv_stop_c10 = heat_surv_stop_c10;
        ctl.surv_bat_c10 = heat_surv_bat_c10;
        gpuheat_khz = 598000U;              /* the GPU's table top (ECT dvfs_g3d) */
        gpu_heat_session = 1;
        sb_log(pending_level == HEAT_SURVIVAL_X ?
               "heater: Survival heat with the chip's trip OFF confirmed four times (the tables' tops, to the user's SoC stop)" :
               "heater: Survival heat confirmed three times (the tables' tops, just below the chip's power cut, to 2 %)");
    } else if (pending_level == HEAT_MANUAL) {
        ctl.man_a55 = heat_man_a55;
        ctl.man_m3 = heat_man_m3;
        gpuheat_khz = heat_man_gpu ? heat_man_gpu : 572000U;
        gpu_heat_session = heat_man_gpu != 0;
        sb_log("heater: Manual confirmed (the picked table steps, flat out)");
    } else {
        ctl.man_a55 = ctl.man_m3 = 0;
        gpuheat_khz = 572000U;
        gpu_heat_session = 1;               /* this confirmation named the GPU */
        sb_log("heater: Extreme confirmed (every core, the GPU, the panel and the LED at their top table steps)");
    }
    start_now();
    if (!heater_running_ui())
        gpu_heat_session = 0;               /* refused: no session to cover */
}

/* 1.1: Survival's confirmations: three, four with the trip off; each says what it is; any No ends it */
static void survival_done(int ok);
static void survival_show(void)
{
    static char est[160];
    struct heat_in in;
    read_inputs(&in);
    struct ub u;
    ub_init(&u);
    if (in.gauge_ok && in.pct > 2) {
        ub_s(&u, "It turns the WHOLE battery into heat: about ");
        ub_dec(&u, (uint64_t)((in.pct - 2) * SBOS_HEAT_MAH_PER_PCT * 60 / FLAT_MA));
        ub_s(&u, " minutes from ");
        ub_dec(&u, (uint64_t)in.pct);
        ub_s(&u, " % to 2 %. Nothing is kept for a call, the light or SOS.");
    } else {
        ub_s(&u, "It turns the WHOLE battery into heat. Nothing is kept for a call, the light or SOS.");
    }
    sb_strcpy(est, sizeof(est), u.b);
    int x = pending_level == HEAT_SURVIVAL_X;
    static char stopl[160], batl[128];
    ub_init(&u);
    ub_s(&u, "Holds at your limit (");
    ub_dec(&u, (uint64_t)(heat_surv_stop_c10 / 10));
    ub_s(&u, " \xc2\xb0" "C) to 2 %; stops 5 \xc2\xb0" "C past it or at ");
    ub_dec(&u, (uint64_t)(heat_surv_bat_c10 / 10));
    ub_s(&u, " \xc2\xb0" "C battery. A watchdog restarts a stalled phone.");
    sb_strcpy(stopl, sizeof(stopl), u.b);
    ub_init(&u);
    ub_s(&u, "It holds the battery below ");
    ub_dec(&u, (uint64_t)(heat_surv_bat_c10 / 10));
    ub_s(&u, " \xc2\xb0" "C, stops at it or at 2 %. Cancel stops it at any time.");
    sb_strcpy(batl, sizeof(batl), u.b);
    switch (surv_step) {
    case 0:
        kui_confirm(x ? "Survival heat, trip OFF (1 of 4)?" : "Survival heat (1 of 3)?",
                    "A LAST RESORT for a survival situation: the most heat this phone can make.",
                    "Every core and the GPU at the chip's top table steps, the screen at its brightest and the LED.",
                    x ? "The chip's own heat power cut will be switched OFF: flat out up to your limit, held there to 2 %." :
                        "It holds the processor just below the chip's own power cut and runs to 2 % battery.",
                    x ? "Hold: 1 of 4" : "Hold: 1 of 3", survival_done);
        break;
    case 1:
        kui_confirm(x ? "Survival heat, trip OFF (2 of 4)?" : "Survival heat (2 of 3)?", est,
                    "When it ends the phone is empty and too hot to hold bare.",
                    "Only for a survival situation.", x ? "Hold: 2 of 4" : "Hold: 2 of 3", survival_done);
        break;
    case 2:
        if (x)
            kui_confirm("The chip's power cut OFF (3 of 4)?",
                        "WARNING: the processor's hardware heat trip is the one stop that works without software.",
                        "Off, the processor runs hotter than its rating and may be damaged for good.",
                        stopl, "Hold: 3 of 4", survival_done);
        else
            kui_confirm("Survival heat: last check (3 of 3)",
                        "The back gets very hot: hold it through a pocket or a glove, never against bare skin.",
                        batl,
                        "Survival heat: a last resort for a survival situation.", "Hold: 3 of 3, start", survival_done);
        break;
    default:
        kui_confirm("Survival heat, trip OFF: last check (4 of 4)",
                    "WARNING: a battery sits next to the processor. Keep the phone where you can drop it.",
                    "Cancel or Power stops it at any time; the trip comes back on as soon as it stops.",
                    "You take responsibility for any damage.", "Hold: 4 of 4, start", survival_done);
        break;
    }
}

static void survival_done(int ok)
{
    if (!ok) {
        surv_step = 0;
        sb_log("heater: Survival heat not confirmed");
        extreme_confirm_done(0);
        return;
    }
    int n = pending_level == HEAT_SURVIVAL_X ? 4 : 3;
    if (++surv_step < n) {
        survival_show();
        return;
    }
    surv_step = 0;
    extreme_confirm_done(1);
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
    if (level > HEAT_SURVIVAL_X)
        return SBOS_EINVAL;
    if (wifi_on() && !heater_running_ui()) {
        sb_strcpy(ctl.why, sizeof(ctl.why), "Turn Wi-Fi off first (0.4: not both at once)");
        ctl.state = HEAT_ST_REFUSED;
        sb_log("heater: refused: Wi-Fi is on");
        return SBOS_EAGAIN;
    }
    if (heater_running_ui() && (level >= HEAT_EXTREME || ctl.level >= HEAT_EXTREME) && level != ctl.level) {
        heater_request(0, 0);               /* to or from Extreme / Manual: stop, then start anew (its own rules) */
        if (level < HEAT_EXTREME)
            return SBOS_EAGAIN;             /* the old session is stopping: ask again in a moment */
    }
    if (level >= HEAT_EXTREME && !heater_running_ui()) {
        pending_level = level;
        pending_minutes = minutes;
        ctl.state = HEAT_ST_CONFIRM;
        /* the estimate first (0.6.2): the battery's charge above the reserve at the measured full-heat rate */
        static char est[128], stops[128];
        int res = heat_reserve_override ? 0 : heat_reserve_pct;     /* 1.0-rc2: this session's reserve */
        sb_strcpy(stops, sizeof(stops), res > 0 ? "Stops only at 55 \xc2\xb0" "C battery or the reserve. The back gets hot: hold it through a pocket or a glove." :
                                                  "The reserve is OFF for this session: it stops only at 55 \xc2\xb0" "C battery. The back gets hot: use a pocket or a glove.");
        {
            struct heat_in in;
            read_inputs(&in);
            int usable = in.gauge_ok ? in.pct - (res > 0 ? res : 0) : -1;
            struct ub u;
            ub_init(&u);
            if (usable > 0) {
                ub_s(&u, "About ");
                ub_dec(&u, (uint64_t)(usable * SBOS_HEAT_MAH_PER_PCT * 60 / FLAT_MA));
                ub_s(&u, res > 0 ? " minutes of full heat before the call / SOS reserve (" :
                                                " minutes of full heat until the battery is empty (");
                ub_dec(&u, (uint64_t)in.pct);
                ub_s(&u, " % now, about 2.8 A: about 3 % a minute).");
            } else {
                ub_s(&u, in.gauge_ok ? "The battery is at the call / SOS reserve." : "The battery's charge is unknown.");
            }
            sb_strcpy(est, sizeof(est), u.b);
        }
        if (level == HEAT_KEEPWARM) {
            static char tl[128];
            struct ub u;
            ub_init(&u);
            ub_s(&u, "Keeps the battery near ");
            ub_dec(&u, (uint64_t)(heat_target_c10 / 10));
            ub_s(&u, " \xc2\xb0" "C: a cold battery loses its charge; all cores while it is colder, none while warmer.");
            sb_strcpy(tl, sizeof(tl), u.b);
            kui_confirm("Keep the battery warm?", tl, est,
                        res > 0 ? "No other limits: it stops only at 55 \xc2\xb0" "C battery or the reserve." :
                                  "The reserve is OFF for this session: it stops only at 55 \xc2\xb0" "C battery.",
                        "Hold to start", extreme_confirm_done);
            return 0;
        }
        if (level >= HEAT_SURVIVAL) {
            surv_step = 0;
            survival_show();
            return 0;
        }
        if (level == HEAT_EXTREME)
            kui_confirm("Extreme heat?",
                        "Every core at its top table step, the GPU, the screen at its top and the LED: flat out until you stop it.",
                        est,
                        stops, "Hold for Extreme", extreme_confirm_done);
        else
            kui_confirm("Manual heat?",
                        "The clocks you picked for the small cores, the big cores and the GPU, flat out until you stop it.",
                        est,
                        stops, "Hold for Manual", extreme_confirm_done);
        return 0;
    }
    if (heater_running_ui()) {
        /* a level change: the controller keeps its state, the new ceiling applies */
        if (ctl.level == HEAT_EXTREME && level != HEAT_EXTREME) {
            extreme_extras(0);
            gpu_heat_session = 0;
        }
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

/* SYS_SENSOR_READ (0.3): a temperature in 0.1 C, or -1000 unknown. While the heater runs, its own last
 * samples (no extra ADC reads beside the controller's); otherwise a fresh read at most once a second
 * (no charger reads: those belong to the controller's debounce). id 1 battery, 2 skin (AP), 3 SoC, 4 GPU. */
int heater_sensor(int id)
{
    static uint64_t t_read;
    static int c[5] = { -1000, -1000, -1000, -1000, -1000 };
    uint64_t now = sb_ms();
    if (id < 1 || id > 4)
        return -1000;
    if (heater_running_ui() || stopping) {
        if (id == 1)
            return last_in.bat_ok ? last_in.bat_c10 : -1000;
        if (id == 2)
            return last_in.skin_ok ? last_in.skin_c10 : -1000;
        if (id == 3)
            return last_in.soc_ok ? last_in.soc_c10 : -1000;
        return c[4];                            /* the GPU: the last value from before the start */
    }
    if (!t_read || now - t_read >= 1000) {
        t_read = now;
        int v = adc_c10(4, 1, 0);
        c[1] = v > -300 && v < 1000 ? v : -1000;
        v = adc_c10(0, 0, 0);
        c[2] = v > -300 && v < 1200 ? v : -1000;
        struct sb_temps t;
        sb_temps_read(&t);
        c[3] = t.top == 1 && t.hottest > -30 && t.hottest < 150 ? t.hottest * 10 : -1000;
        c[4] = t.sub == 1 && t.gpu > -30 && t.gpu < 150 ? t.gpu * 10 : -1000;
    }
    return c[id];
}

void heater_status(struct sbos_heat *h)
{
    memset(h, 0, sizeof(*h));
    h->state = ctl.state;
    h->level = ctl.level;
    h->cores = cores_running();             /* the workers and cpu0, as the log counts them */
    h->bat_c10 = last_in.bat_ok ? last_in.bat_c10 : -1000;
    h->soc_c10 = last_in.soc_ok ? last_in.soc_c10 : -1000;
    if (!heater_running_ui() && !stopping) {    /* off: now, not the last session's values */
        h->bat_c10 = heater_sensor(1);
        h->soc_c10 = heater_sensor(3);
    }
    uint64_t now = sb_ms();
    h->secs_left = heater_running_ui() && ctl.level < HEAT_EXTREME && ctl.t_end > now ? (int32_t)((ctl.t_end - now) / 1000U) : 0;
    h->secs_run = heater_running_ui() ? (int32_t)((now - ctl.t_start) / 1000U) : 0;
    h->a55_mhz = (int32_t)(hw_a55_khz() / 1000U);
    h->m3_mhz = (int32_t)(hw_m3_khz() / 1000U);
    sb_strcpy(h->why, sizeof(h->why), ctl.why);
    h->skin_c10 = last_in.skin_ok && heater_running_ui() ? last_in.skin_c10 : -1000;
    h->gpu_mhz = (int32_t)gpuheat_mhz();
    h->ma = last_in.gauge_ok && heater_running_ui() ? last_in.ma : 0;
    h->mv = last_in.gauge_ok && heater_running_ui() ? last_in.mv : 0;
    h->pct = last_in.gauge_ok ? last_in.pct : -1;
    h->reserve_pct = heater_running_ui() ? ctl.reserve_pct : heat_reserve_pct;   /* this session's (1.0-rc2) */
    h->target_c10 = heat_target_c10;
    h->no_current_stop = heat_no_current_stop;
    h->trip_c = trip_top_c;
    h->soc_stop_c10 = heat_soc_stop_c10;
    h->last_unclean = heat_last_unclean;
    h->last_peak_soc_c10 = heat_last.peak_soc_c10;
    h->last_secs = heat_last.secs;
    h->trip_off = trip_off;
    h->surv_target_c10 = heater_running_ui() && ctl.level >= HEAT_SURVIVAL ? heat_surv_target(&ctl) :
                         heat_default_limits.surv_target_c10;
    h->surv_stop_c10 = heat_surv_stop_c10;
    h->gpu_on = gpuheat_on();
    h->surv_bat_c10 = heater_running_ui() && ctl.level >= HEAT_SURVIVAL ? ctl.surv_bat_c10 : heat_surv_bat_c10;
}

void heater_init(void)
{
    heat_ctl_init(&ctl, &heat_default_limits);
    memset(&cfg, 0, sizeof(cfg));
    for (unsigned w = 0; w < 8; ++w)                /* 32 MiB of the arena, once */
        cfg.buf[w] = sb_alloc((uint64_t)HEATK_WORDS * 8U);
    struct heat_in in;
    read_inputs(&in);
    last_in = in;
}
