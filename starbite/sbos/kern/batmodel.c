/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS battery model (docs/STARBITE-OS.md 4.5): the mean discharge current the phone really draws in
 * each power state, learned from the fuel gauge's average current, kept across power-offs in the state slot.
 * Pure C. Until a state has 5 readings the typical value below is used and the estimate says so. */
#include "kern.h"

/* Typical currents before anything is learned (mA). Active: panel on with a mostly black screen and an
 * idle A55 (the Utility OS's status bar showed about 300 mA on the phone); screen off without the SoC's own
 * sleep (v0.1/v0.2): the SoC awake in wfi; heat: 8 cores at full clock. Replaced by measurements. */
static const int32_t typical[PS_N] = { 300, 120, 2500 };

void bat_model_init(struct bat_model *m)
{
    for (unsigned i = 0; i < PS_N; ++i) {
        m->ma[i] = typical[i];
        m->samples[i] = 0;
    }
    m->version = 1;
}

void bat_model_feed(struct bat_model *m, int st, int avg_ma, uint64_t in_state_ms)
{
    if (st < 0 || st >= PS_N || avg_ma >= 0 || in_state_ms < 30000U)
        return;                                 /* charging, idle current or a fresh transition */
    int32_t d = -avg_ma;
    if (d > 6000)
        return;                                 /* not a plausible reading */
    if (m->samples[st] < 5) {                   /* the first readings: a plain mean, starting over */
        m->ma[st] = m->samples[st] ? (m->ma[st] * (int32_t)m->samples[st] + d) / ((int32_t)m->samples[st] + 1) : d;
    } else {                                    /* then a moving average, about the last 32 readings */
        int32_t diff = d - m->ma[st];
        m->ma[st] += (diff + (diff > 0 ? 31 : diff < 0 ? -31 : 0)) / 32;   /* rounded away from zero: reaches d */
    }
    if (m->ma[st] < 1)
        m->ma[st] = 1;
    m->samples[st]++;
}

int32_t bat_model_ma(const struct bat_model *m, int st, int *learned)
{
    if (st < 0 || st >= PS_N)
        return 1;
    if (learned)
        *learned = m->samples[st] >= 5;
    return m->ma[st] > 0 ? m->ma[st] : 1;
}

int32_t bat_minutes(int cap_mah, int32_t ma)
{
    if (cap_mah <= 0)
        return 0;
    if (ma < 1)
        ma = 1;
    int64_t m = (int64_t)cap_mah * 60 / ma;
    return m > 60 * 24 * 60 ? 60 * 24 * 60 : (int32_t)m;
}
