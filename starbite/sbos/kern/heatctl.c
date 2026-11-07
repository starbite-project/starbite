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
    .min_pct = { 0, 20, 30, 30 },
    .stop_pct = 15,
    .blind_ms = 3000,
    .stuck_ms = 60000,
    .window_ms = 300000,
    .step_ms = 1000,
    .ramp_ms = 60000,
    .max_minutes = 60,
    .def_minutes = 30,
};

#define A55_FULL 1794000U
#define A55_COLD 1248000U
#define M3_START 1469000U
#define M3_FULL 1794000U

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
    if (start_fail)
        return 0;                               /* never checked in: it may be on and hung */
    if (slot == 1U /* SB_WK_RUNNING */)
        return 0;                               /* still in the heat kernel */
    if (r == 1)
        return 1;                               /* PSCI: off */
    if (r == 0 || r == 2)
        return slot == 0U /* SB_WK_IDLE: never started */ ? 1 : 2;
    return 1;                                   /* AFFINITY_INFO not supported (PSCI 0.1): the slot says it left */
}

int heat_max_cores(int level)
{
    return level == HEAT_LOW ? 3 : level == HEAT_MEDIUM ? 5 : level == HEAT_HIGH ? 7 : 0;
}

void heat_ctl_init(struct heat_ctl *c, const struct heat_limits *lim)
{
    char *p = (char *)c;
    for (unsigned i = 0; i < sizeof(*c); ++i)
        p[i] = 0;
    c->lim = lim ? lim : &heat_default_limits;
    c->state = HEAT_ST_OFF;
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
    if (level > HEAT_HIGH)
        return SBOS_EINVAL;
    const char *no = 0;
    if (!in->bat_ok)
        no = "No battery temperature reading";
    else if (in->bat_c10 >= L->start_bat_max_c10)
        no = "The battery is already warm";
    else if (c->bat_latched && in->bat_c10 >= L->resume_bat_c10)
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
    c->a55_khz = c->cold ? A55_COLD : A55_FULL;
    c->m3_khz = 0;
    c->rest_mv = in->mv;
    c->t_start = c->t_step = c->last_bat_ok = c->last_soc_ok = in->now;
    c->t_end = in->now + (uint64_t)minutes * 60000U;
    c->t_ramp = 0;
    c->last_raw = in->bat_raw;
    c->raw_since = in->now;
    c->raw_soc_c10 = in->soc_c10;
    c->t_win = in->now;
    c->win_bat_c10 = in->bat_c10;
    c->win_soc_c10 = in->soc_c10;
    c->bat_latched = 0;
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
    if (in->bat_ok && in->bat_c10 >= L->hard_bat_c10) {
        c->bat_latched = 1;
        stop(c, HEAT_ST_STOPPED, "Battery reached 42 \xc2\xb0" "C");
        return;
    }
    if (in->soc_ok && in->soc_c10 >= L->hard_soc_c10) {
        stop(c, HEAT_ST_STOPPED, "Processor reached 85 \xc2\xb0" "C");
        return;
    }
    if (in->skin_ok && in->skin_c10 >= L->hard_skin_c10) {
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
    if (in->gauge_ok) {
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
    if (now - c->t_step >= L->step_ms) {
        int over = (in->bat_ok && in->bat_c10 >= L->band_bat_c10) ||
                   (in->skin_ok && in->skin_c10 >= L->soft_skin_c10) ||
                   (in->soc_ok && in->soc_c10 >= L->soft_soc_c10) ||
                   (in->gauge_ok && (iabs(in->ma) > L->soft_ma || sag > L->soft_sag_mv));
        int under = in->bat_ok && in->bat_c10 < L->band_bat_c10 - 10 &&
                    (!in->skin_ok || in->skin_c10 < L->soft_skin_c10 - 10) &&
                    in->soc_ok && in->soc_c10 < L->soft_soc_c10 - 10 &&
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
