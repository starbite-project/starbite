/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS heater controller (docs/STARBITE-OS.md 8.2), pure C: given the sensor readings it decides
 * whether the heater may start, how many worker cores load the SoC and at what clocks, and when it must stop.
 * The host test drives it with scripted traces (every stop, the regulation, the cold start, the charger).
 * heater.c reads the sensors and applies the decisions. */
#include "kern.h"

const struct heat_limits heat_default_limits = {
    .start_bat_max_c10 = 380,
    .agree_c10 = 40,
    .hard_bat_c10 = 420,
    .hard_soc_c10 = 850,
    .hard_skin_c10 = 460,
    .resume_bat_c10 = 360,
    .band_bat_c10 = 400,
    .soft_skin_c10 = 430,
    .soft_soc_c10 = 750,
    .soft_ma = 3200,
    .hard_ma = 3500,
    .soft_sag_mv = 300,
    .cold_sag_mv = 150,
    .min_mv = 3450,
    .min_pct = { 0, 20, 30, 30, 0, 0, 0, 0, 0 },   /* 0.6.2: Extreme / Manual / Keep warm: only the reserve */
    .ext_hard_bat_c10 = 550,
    .ext_hard_soc_c10 = 1100,
    .ext_start_bat_max_c10 = 500,
    .stop_pct = 15,
    .blind_ms = 3000,
    .gauge_blind_ms = 10000,
    .stuck_ms = 60000,
    .window_ms = 300000,
    .step_ms = 1000,
    .ramp_ms = 60000,
    .max_minutes = 60,
    .def_minutes = 30,
    .surv_target_c10 = 1220,
    .surv_band_c10 = 20,
    .surv_floor_pct = 2,
    .surv_shed_mv = 3400,
    .surv_stop_ma = 4500,
};

#define A55_FULL 1794000U
#define A55_TOP 1794000U                    /* the A55's top: the ECT's MINMAX_dvfs_cpucl0 cap (0.6.1's 1950 was above it) */
#define A55_COLD 1248000U
#define M3_START 1469000U
#define M3_FULL 1794000U
/* 1.1 Survival: the A55's table top (1950 MHz, in the ECT with its own ASV voltage; Manual offers it with a warning).
 * The M3 stays at 1794 MHz: the ECT lists up to 2704 MHz, but for one or two cores only: Samsung's DT allows anything
 * above 1794 only with its over-current handler (exynos-ocp, max-freq-wo-ocp 1794), which throttles the moment the
 * cluster's buck reaches its current limit; four M3 cores at 2704 under a power-virus load would run the buck past
 * that rating (a brown-out reset, not heat) */
#define A55_SURV 1950000U

static void why(struct heat_ctl *c, const char *s)
{
    unsigned i = 0;
    for (; s[i] && i + 1 < sizeof(c->why); ++i)
        c->why[i] = s[i];
    c->why[i] = 0;
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

int heat_core_state(int64_t psci, unsigned slot, int start_fail)
{
    int32_t r = (int32_t)psci;                  /* w0 */
    if (slot == 1U /* SB_WK_RUNNING */)
        return 0;                               /* still in the heat kernel */
    if (start_fail)
        return 2;                               /* never checked in: no load of ours there; logged, no restart */
    if (r == 1)
        return 1;                               /* PSCI: off */
    if (r == 0 || r == 2)
        return slot == 0U /* SB_WK_IDLE: never started */ ? 1 : 2;
    return 1;                                   /* AFFINITY_INFO not supported (PSCI 0.1): the slot says it left */
}

int heat_max_cores(int level)
{
    return level == HEAT_LOW ? 3 : level == HEAT_MEDIUM ? 5 : level >= HEAT_HIGH && level <= HEAT_SURVIVAL_X ? 7 : 0;
}

int heat_surv_target(const struct heat_ctl *c)
{
    return c->level == HEAT_SURVIVAL_X ? c->surv_stop_c10 - 30 : c->lim->surv_target_c10;
}

void heat_ctl_init(struct heat_ctl *c, const struct heat_limits *lim)
{
    char *p = (char *)c;
    for (unsigned i = 0; i < sizeof(*c); ++i)
        p[i] = 0;
    c->lim = lim ? lim : &heat_default_limits;
    c->state = HEAT_ST_OFF;
    c->reserve_pct = c->lim->stop_pct;      /* 15 % for a call / SOS until the user overrides it */
    c->target_c10 = 200;                    /* Keep warm: 20 C */
    c->surv_stop_c10 = 1310;                /* Survival (trip off): 131 C */
    c->gpu_want = 1;
}

static uint32_t flat_a55(const struct heat_ctl *c)
{
    return c->level == HEAT_MANUAL && c->man_a55 ? c->man_a55 : c->level >= HEAT_SURVIVAL ? A55_SURV :
           c->level >= HEAT_EXTREME ? A55_TOP : A55_FULL;
}

static uint32_t flat_m3(const struct heat_ctl *c)
{
    return c->level == HEAT_MANUAL && c->man_m3 ? c->man_m3 : M3_FULL;
}

static void stop(struct heat_ctl *c, int st, const char *w)
{
    c->state = st;
    c->cores = 0;
    c->a55_khz = 0;
    c->m3_khz = 0;
    why(c, w);
}

int heat_ctl_request(struct heat_ctl *c, int level, int minutes, const struct heat_in *in)
{
    const struct heat_limits *L = c->lim;
    if (level <= HEAT_OFF) {
        if (c->state == HEAT_ST_RUNNING || c->state == HEAT_ST_HOLDING || c->state == HEAT_ST_PAUSED)
            stop(c, HEAT_ST_OFF, "Stopped");
        return 0;
    }
    if (level > HEAT_SURVIVAL_X)
        return SBOS_EINVAL;
    int flat = level >= HEAT_EXTREME;
    int surv = level >= HEAT_SURVIVAL;
    const char *no = 0;
    if (!in->bat_ok)
        no = "No battery temperature reading";
    else if (in->bat_c10 >= (flat ? L->ext_start_bat_max_c10 : L->start_bat_max_c10))
        no = flat ? "The battery is at 50 \xc2\xb0" "C already" : "The battery is already warm";
    else if (c->bat_latched && !flat && in->bat_c10 >= L->resume_bat_c10)
        no = "The battery is still warm from the last session";
    else if (!(in->usb_ok && iabs(in->usb_c10 - in->bat_c10) <= L->agree_c10) &&
             !(in->chg_ok && iabs(in->chg_c10 - in->bat_c10) <= L->agree_c10))
        no = "The battery sensor disagrees with the others";
    else if (!in->soc_ok || !in->tmu_on)
        no = "No processor temperature reading";
    else if (!in->trip_armed)
        no = "The processor's hardware heat limit is not armed";
    else if (!in->gauge_ok)
        no = "No battery reading";
    else if (in->pct < L->min_pct[level])
        no = level == HEAT_LOW ? "Battery below 20 %" : "Battery below 30 % (try Low)";
    else if (surv && in->pct <= L->surv_floor_pct)
        no = "The battery is at 2 %: nothing left to turn into heat";
    else if (level == HEAT_SURVIVAL_X && (c->surv_stop_c10 < 1250 || c->surv_stop_c10 > 1310))
        no = "Survival's SoC stop is not set (125 to 131 \xc2\xb0" "C)";
    else if (flat && !surv && c->reserve_pct > 0 && in->pct <= c->reserve_pct)
        no = "At the call / SOS reserve (hold Continue below the reserve to go on)";
    else if (in->charger > 0)
        no = "Unplug the charger first";
    else if (in->charger < 0)
        no = "Cannot tell whether a charger is connected";
    if (no) {
        c->state = HEAT_ST_REFUSED;
        why(c, no);
        return SBOS_EAGAIN;
    }
    if (minutes <= 0)
        minutes = (int)L->def_minutes;
    if (minutes > (int)L->max_minutes)
        minutes = (int)L->max_minutes;
    c->level = level;
    c->minutes = minutes;
    c->max_cores = heat_max_cores(level);
    c->cores = 1;                               /* one core more per step, while everything stays in range */
    c->cold = in->soc_c10 < 200 || in->bat_c10 < 50;
    c->a55_khz = c->cold ? A55_COLD : flat_a55(c);
    c->m3_khz = 0;
    c->rest_mv = in->mv;
    c->t_start = c->t_step = c->last_bat_ok = c->last_soc_ok = c->last_gauge_ok = in->now;
    c->t_end = flat ? ~(uint64_t)0 : in->now + (uint64_t)minutes * 60000U;    /* flat out: until the user stops */
    c->t_ramp = 0;
    c->last_raw = in->bat_raw;
    c->raw_since = in->now;
    c->raw_soc_c10 = in->soc_c10;
    c->t_win = in->now;
    c->win_bat_c10 = in->bat_c10;
    c->win_soc_c10 = in->soc_c10;
    c->bat_latched = 0;
    c->gpu_want = 1;
    c->t_shed = in->now;
    c->state = HEAT_ST_RUNNING;
    why(c, c->cold ? "Starting gently (cold)" : "Heating");
    return 0;
}

void heat_ctl_step(struct heat_ctl *c, const struct heat_in *in)
{
    const struct heat_limits *L = c->lim;
    if (c->state != HEAT_ST_RUNNING && c->state != HEAT_ST_HOLDING && c->state != HEAT_ST_PAUSED)
        return;
    uint64_t now = in->now;
    /* ---- hard stops ---- */
    if (in->bat_ok)
        c->last_bat_ok = now;
    if (in->soc_ok)
        c->last_soc_ok = now;
    if (now - c->last_bat_ok >= L->blind_ms) {
        stop(c, HEAT_ST_STOPPED, "Lost the battery temperature");
        return;
    }
    if (now - c->last_soc_ok >= L->blind_ms) {
        stop(c, HEAT_ST_STOPPED, "Lost the processor temperature");
        return;
    }
    /* 1.0-rc2 (review): the charge, voltage and current stops need the fuel gauge: without it for 10 s, stop */
    if (in->gauge_ok)
        c->last_gauge_ok = now;
    if (now - c->last_gauge_ok >= L->gauge_blind_ms) {
        stop(c, HEAT_ST_STOPPED, "Lost the battery's charge and current reading");
        return;
    }
    /* Extreme / Manual / Keep warm (0.6.1, 0.6.2): no software limits but the battery's 55 C (its fire safety), the
     * readings that stop needs, a core fault, the call / SOS reserve (overridable) and the 3.5 A battery current (a
     * short or a failing cell; the user can turn it off in Manual only); the SoC's own 115 C hardware trip stays
     * armed; the voltage is the PMIC's and the fuel gauge's own business */
    int flat = c->level >= HEAT_EXTREME;
    int surv = c->level >= HEAT_SURVIVAL;
    int hard_bat = flat ? L->ext_hard_bat_c10 : L->hard_bat_c10;
    int band_bat = L->band_bat_c10;
    int soft_soc = L->soft_soc_c10;
    if (in->bat_ok && in->bat_c10 >= hard_bat) {
        c->bat_latched = 1;
        stop(c, HEAT_ST_STOPPED, flat ? "Battery reached 55 \xc2\xb0" "C (its safety limit)" : "Battery reached 42 \xc2\xb0" "C");
        return;
    }
    if (flat && c->level == HEAT_MANUAL && c->soc_stop_c10 > 0 && in->soc_ok && in->soc_c10 >= c->soc_stop_c10) {
        stop(c, HEAT_ST_STOPPED, "Processor at your SoC stop");
        return;
    }
    /* Survival with the trip off (1.1): the user's SoC stop is the only temperature stop for the chip */
    if (c->level == HEAT_SURVIVAL_X && c->trip_off && in->soc_ok && in->soc_c10 >= c->surv_stop_c10) {
        stop(c, HEAT_ST_STOPPED, "Processor at your Survival stop");
        return;
    }
    if (flat && !in->trip_armed && !c->trip_off) {
        /* no stop of ours for the SoC's temperature in these levels: the hardware trip is the protection, so it
         * must stay armed the whole time (it was checked at the start) */
        stop(c, HEAT_ST_STOPPED, "The processor's hardware heat trip is not armed");
        return;
    }
    if (!flat && in->soc_ok && in->soc_c10 >= L->hard_soc_c10) {
        stop(c, HEAT_ST_STOPPED, "Processor reached 85 \xc2\xb0" "C");
        return;
    }
    if (!flat && in->skin_ok && in->skin_c10 >= L->hard_skin_c10) {
        stop(c, HEAT_ST_STOPPED, "The phone's back reached 46 \xc2\xb0" "C");
        return;
    }
    if (in->bad_results) {
        stop(c, HEAT_ST_STOPPED, "A core computed a wrong result");
        return;
    }
    if (in->worker_fault) {
        stop(c, HEAT_ST_STOPPED, "A core stopped with a fault");
        return;
    }
    if (surv && in->gauge_ok && in->pct < L->surv_floor_pct) {
        stop(c, HEAT_ST_STOPPED, "Battery at 2 %: all of it went to heat");
        return;
    }
    if (surv && in->gauge_ok && iabs(in->ma) > L->surv_stop_ma) {
        stop(c, HEAT_ST_STOPPED, "Battery current above 4.5 A (a short or a failing cell?)");
        return;
    }
    if (flat && !surv && in->gauge_ok && c->reserve_pct > 0 && in->pct < c->reserve_pct) {
        stop(c, HEAT_ST_STOPPED, "Reserve reached: kept for a call or SOS");
        return;
    }
    if (flat && !surv && in->gauge_ok && !(c->level == HEAT_MANUAL && c->no_current_stop) && iabs(in->ma) > L->hard_ma) {
        stop(c, HEAT_ST_STOPPED, "Battery current above 3.5 A (a short or a failing cell?)");
        return;
    }
    if (!flat && in->gauge_ok) {
        if (in->pct < L->stop_pct) {
            stop(c, HEAT_ST_STOPPED, "Battery below 15 %: kept for Light and SOS");
            return;
        }
        if (in->mv > 0 && in->mv < L->min_mv) {
            stop(c, HEAT_ST_STOPPED, "Battery voltage too low");
            return;
        }
        if (iabs(in->ma) > L->hard_ma) {
            stop(c, HEAT_ST_STOPPED, "Battery current too high");
            return;
        }
    }
    if (now >= c->t_end) {
        stop(c, HEAT_ST_OFF, "Finished");
        return;
    }
    /* stuck battery sensor: the same raw value for a minute while the SoC moved 5 C */
    if (in->bat_ok) {
        if (in->bat_raw != c->last_raw) {
            c->last_raw = in->bat_raw;
            c->raw_since = now;
            c->raw_soc_c10 = in->soc_c10;
        } else if (now - c->raw_since >= L->stuck_ms && in->soc_ok && iabs(in->soc_c10 - c->raw_soc_c10) >= 50) {
            stop(c, HEAT_ST_STOPPED, "The battery sensor stopped changing");
            return;
        }
    }
    /* or the SoC 20 C up while the battery moved less than 0.5 C in 5 minutes */
    if (now - c->t_win >= L->window_ms) {
        if (in->soc_ok && in->bat_ok && in->soc_c10 - c->win_soc_c10 >= 200 &&
            iabs(in->bat_c10 - c->win_bat_c10) < 5) {
            stop(c, HEAT_ST_STOPPED, "The battery sensor does not follow the heat");
            return;
        }
        c->t_win = now;
        c->win_bat_c10 = in->bat_c10;
        c->win_soc_c10 = in->soc_c10;
    }
    /* ---- the charger pauses ---- */
    if (in->charger != 0) {
        c->state = HEAT_ST_PAUSED;
        c->cores = 0;
        c->m3_khz = 0;
        c->t_ramp = 0;
        why(c, in->charger > 0 ? "Paused: a charger is connected" : "Paused: cannot read the charger");
        return;
    }
    if (c->state == HEAT_ST_PAUSED) {
        c->state = HEAT_ST_RUNNING;
        c->cores = 1;
        c->t_step = now;
        c->rest_mv = in->mv;
    }
    /* ---- regulation: one core off when anything is over its soft limit, one on when all are below ---- */
    int sag = in->gauge_ok && c->rest_mv > 0 ? c->rest_mv - in->mv : 0;
    if (surv) {
        /* Survival (1.1): as much load as the chip takes just below its own power cut, the battery's current and
         * its voltage. Shed at once (one load per 250 ms tick, two when 2 C over): the M3 cores first, then the
         * A55s, the GPU last; add again (one a second) only 2 C below the target, the GPU after every core */
        if (c->level == HEAT_SURVIVAL_X) {
            /* 1.1-beta2 (the user's spec): the hardware trip off = no shedding at all: every core and the GPU,
             * one core more a second, flat out until the user's SoC stop (above) or a stop that stays */
            if (now - c->t_step >= L->step_ms && c->cores < c->max_cores && !(c->cold && c->cores >= 3)) {
                c->cores++;
                c->t_step = now;
            }
            if (c->cold && now - c->t_start >= 30000U) {
                c->cold = 0;
                c->a55_khz = flat_a55(c);
            }
            c->gpu_want = 1;
            c->m3_khz = c->cores > 3 ? flat_m3(c) : 0;
            c->state = HEAT_ST_RUNNING;
            why(c, "Survival, trip OFF: flat out to your stop");
            return;
        }
        int target = heat_surv_target(c);
        int hot = in->soc_ok && in->soc_c10 >= target;
        int very = in->soc_ok && in->soc_c10 >= target + 20;
        int amps = in->gauge_ok && iabs(in->ma) > L->hard_ma;
        int low = in->gauge_ok && in->mv > 0 && in->mv < L->surv_shed_mv;
        int cool = in->soc_ok && in->soc_c10 < target - L->surv_band_c10 && in->gauge_ok &&
                   iabs(in->ma) < L->hard_ma - 300 && in->mv >= L->surv_shed_mv + 50;
        if ((hot || amps || low) && now - c->t_shed >= 250U) {
            for (int n = very ? 2 : 1; n > 0; --n) {
                if (c->cores > 0)
                    c->cores--;
                else
                    c->gpu_want = 0;
            }
            c->t_shed = now;
            c->t_step = now;
        } else if (cool && now - c->t_step >= L->step_ms && !(c->cold && c->cores >= 3)) {
            if (c->cores < c->max_cores)
                c->cores++;
            else
                c->gpu_want = 1;
            c->t_step = now;
        }
        if (c->cold && now - c->t_start >= 30000U) {
            c->cold = 0;
            c->a55_khz = flat_a55(c);
        }
        c->m3_khz = c->cores > 3 ? flat_m3(c) : 0;
        c->state = c->cores || c->gpu_want ? HEAT_ST_RUNNING : HEAT_ST_HOLDING;
        why(c, hot ? (c->level == HEAT_SURVIVAL_X ? "Survival (trip off): holding below your stop" :
                                                    "Survival: holding just below the chip's power cut") :
               amps ? "Survival: at the battery's current limit" : low ? "Survival: the battery's voltage is low" :
               c->cores == c->max_cores && c->gpu_want ? "Survival: everything flat out" : "Survival: adding load");
        return;
    }
    if (flat) {
        /* no regulation: one core more per step until all run, then they stay; the clocks at once. Keep warm: one
         * core more below the battery's target, one less above it (the battery's warmth is the goal) */
        int want_more = c->level != HEAT_KEEPWARM || (in->bat_ok && in->bat_c10 < c->target_c10 - 5);
        int want_less = c->level == HEAT_KEEPWARM && in->bat_ok && in->bat_c10 > c->target_c10 + 5;
        if (now - c->t_step >= L->step_ms && want_more && c->cores < c->max_cores && !(c->cold && c->cores >= 3)) {
            c->cores++;
            c->t_step = now;
        } else if (now - c->t_step >= L->step_ms && want_less && c->cores > 0) {
            c->cores--;
            c->t_step = now;
        }
        if (c->cold && now - c->t_start >= 30000U) {
            c->cold = 0;                    /* below 5 C: 30 s gently first, then the top regardless of sag */
            c->a55_khz = flat_a55(c);
        }
        c->m3_khz = c->cores > 3 ? flat_m3(c) : 0;
        c->state = c->cores ? HEAT_ST_RUNNING : HEAT_ST_HOLDING;
        why(c, c->cold ? "Starting gently (cold)" : c->level == HEAT_MANUAL ? "Manual: flat out" :
               c->level == HEAT_KEEPWARM ? (c->cores ? "Warming the battery" : "The battery is at its target") :
               "Extreme: flat out");
        return;
    }
    if (now - c->t_step >= L->step_ms) {
        int over = (in->bat_ok && in->bat_c10 >= band_bat) ||
                   (in->skin_ok && in->skin_c10 >= L->soft_skin_c10) ||
                   (in->soc_ok && in->soc_c10 >= soft_soc) ||
                   (in->gauge_ok && (iabs(in->ma) > L->soft_ma || sag > L->soft_sag_mv));
        int under = in->bat_ok && in->bat_c10 < band_bat - 10 &&
                    (!in->skin_ok || in->skin_c10 < L->soft_skin_c10 - 10) &&
                    in->soc_ok && in->soc_c10 < soft_soc - 10 &&
                    in->gauge_ok && iabs(in->ma) < L->soft_ma - 200 && sag < L->soft_sag_mv - 50;
        if (over && c->cores > 0) {
            c->cores--;
            c->t_step = now;
        } else if (under && c->cores < c->max_cores) {
            /* cold: no M3 core until the A55s run at full clock */
            if (!(c->cold && c->cores >= 3)) {
                c->cores++;
                c->t_step = now;
            }
        }
    }
    /* ---- clocks ---- */
    if (c->cold && now - c->t_start >= 30000U && in->gauge_ok && sag < L->cold_sag_mv) {
        c->cold = 0;
        c->a55_khz = A55_FULL;
    }
    if (c->cores > 3) {
        if (!c->m3_khz) {
            c->m3_khz = M3_START;
            c->t_ramp = now + L->ramp_ms;
        } else if (c->m3_khz == M3_START && now >= c->t_ramp && in->gauge_ok && sag < L->cold_sag_mv) {
            c->m3_khz = M3_FULL;
        }
    } else {
        c->m3_khz = 0;
        c->t_ramp = 0;
    }
    c->state = c->cores ? HEAT_ST_RUNNING : HEAT_ST_HOLDING;
    if (c->state == HEAT_ST_HOLDING)
        why(c, "Warm: holding the temperature");
    else
        why(c, c->cold ? "Starting gently (cold)" : "Heating");
}
