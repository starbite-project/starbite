/* SPDX-License-Identifier: MIT
 * Host test of the Survival kit's sums (kern/survkit.c built with -DSURVKIT_HOST): altitude and its reference, the
 * pressure trend and its levels, the sun's position against NOAA's solar calculator (an independent algorithm, the
 * expected values from tools in Python), the timers' lengths.
 * The pulse detector (kern/hrm.c, -DHRM_HOST) on synthetic fingertip signals: 48, 72, 150 a minute with baseline drift,
 * a dicrotic notch and noise; a flat signal gives nothing.
 *   cc -O1 -Wall -Wextra -Werror -DSURVKIT_HOST -DHRM_HOST -I kern kern/host/survkit-test.c kern/survkit.c kern/hrm.c
 *      -lm -o /tmp/t && /tmp/t */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "survkit.h"
#include "hrm.h"

static int fails, checks;

static void check(const char *what, int ok)
{
    ++checks;
    if (!ok) {
        ++fails;
        printf("FAIL: %s\n", what);
    }
}

static int near(int a, int b, int tol) { return abs(a - b) <= tol; }

static const struct { int lat, lon; long long utc; int az, el; } sun[] = {
    { 6017, 2494, 1794823200LL, 1788, 110 },    /* 2026-11-16 10:00 UTC, Helsinki */
    { 6017, 2494, 1782034200LL, 1603, 522 },    /* 2026-06-21 09:30 UTC */
    { -3387, 15121, 1768010400LL, 28, 781 },    /* 2026-01-10 02:00 UTC, Sydney */
    { 4071, -7401, 1774026000LL, 1787, 493 },   /* 2026-03-20 17:00 UTC, New York */
    { 0, 0, 1790164800LL, 2642, 881 },          /* 2026-09-23 12:00 UTC: the sun near the zenith */
    { 6900, 2500, 1797849000LL, 1827, -25 },    /* 2026-12-21 10:30 UTC, Lapland: below the horizon */
    { -5480, -6830, 1782928800LL, 3407, 101 },  /* 2026-07-01 18:00 UTC, Ushuaia */
    { 3500, 13900, 1776295800LL, 1074, 394 },   /* 2026-04-15 23:30 UTC, Tokyo */
};


/* a fingertip's IR signal: light back falls with each pulse (a fast rise of blood, a slower fall, a notch) */
static double pulse_shape(double ph)
{
    double p = ph < 0.12 ? ph / 0.12 : exp(-(ph - 0.12) * 5.0);
    p += 0.25 * exp(-((ph - 0.42) / 0.05) * ((ph - 0.42) / 0.05));
    return p;
}

static unsigned seed = 12345;
static int noise(int amp)
{
    seed = seed * 1103515245U + 12345U;
    return (int)((seed >> 16) % (unsigned)(2 * amp + 1)) - amp;
}

static int run_pulse(double bpm, int secs, int amp, int noise_amp, int *quality)
{
    struct hrm_algo a;
    hrm_algo_reset(&a, 100);
    for (int i = 0; i < secs * 100; ++i) {
        double t = i / 100.0, ph = fmod(t * bpm / 60.0, 1.0);
        int32_t ir = 250000 + (int32_t)(8000 * sin(2 * M_PI * 0.15 * t)) - (int32_t)(amp * pulse_shape(ph)) +
                     noise(noise_amp);
        hrm_algo_push(&a, ir);
    }
    *quality = a.quality;
    return a.bpm;
}

int main(void)
{
    /* altitude: the standard atmosphere (1000 m = 898.76 hPa; 2000 m = 794.95; 5000 m = 540.20) */
    check("altitude at the reference is 0", kit_altitude_m(101325, KIT_P0_STD) == 0);
    check("1000 m", near(kit_altitude_m(89876, KIT_P0_STD), 1000, 2));
    check("2000 m", near(kit_altitude_m(79495, KIT_P0_STD), 2000, 2));
    check("5000 m", near(kit_altitude_m(54020, KIT_P0_STD), 5000, 3));
    check("below sea level", near(kit_altitude_m(102531, KIT_P0_STD), -100, 2));
    check("no pressure", kit_altitude_m(0, KIT_P0_STD) == -100000);
    {
        int32_t p0 = kit_p0_for(95000, 450);
        check("the reference from a known 450 m", p0 > 100000 && p0 < 101000);
        check("reads back 450 m", near(kit_altitude_m(95000, p0), 450, 1));
        check("a silly altitude is refused", kit_p0_for(95000, 20000) == 0);
        check("a silly pressure is refused", kit_p0_for(5000, 100) == 0);
    }

    /* the trend: readings every 15 min */
    kit_hist_clear();
    int32_t d;
    int cov;
    check("no history: no change", !kit_change(180, &d, &cov));
    for (int i = 0; i <= 12; ++i)                       /* 3 h steady at 1013.0 */
        kit_hist_add(1000 + i * 900, 10130);
    check("steady: a 3 h change", kit_change(180, &d, &cov) && d == 0 && cov == 180);
    check("steady level", kit_trend_level(0, 0, 1, 1) == KIT_STEADY);
    kit_hist_clear();
    for (int i = 0; i <= 12; ++i)                       /* -4 hPa in 3 h */
        kit_hist_add(1000 + i * 900, 10130 - i * 40 / 12);
    check("falling 4 hPa in 3 h", kit_change(180, &d, &cov) && near(d, -40, 1));
    check("falling fast", kit_trend_level(d, -13, 1, 1) == KIT_FALLING_FAST);
    check("storm: 6 hPa in 3 h", kit_trend_level(-60, -20, 1, 1) == KIT_STORM);
    check("storm: 2.5 hPa in 1 h", kit_trend_level(0, -25, 0, 1) == KIT_STORM);
    check("falling 2 hPa", kit_trend_level(-20, -7, 1, 1) == KIT_FALLING);
    check("rising 2 hPa", kit_trend_level(20, 7, 1, 1) == KIT_RISING);
    check("rising fast", kit_trend_level(40, 12, 1, 1) == KIT_RISING_FAST);
    check("only 1 h known: falling", kit_trend_level(0, -9, 0, 1) == KIT_FALLING);
    kit_hist_clear();
    kit_hist_add(0, 10130);
    kit_hist_add(3600, 10120);                          /* a 1 h gap, nothing near 3 h back */
    check("1 h pair", kit_change(60, &d, &cov) && d == -10 && cov == 60);
    check("no 3 h pair from 1 h of readings", !kit_change(180, &d, &cov));
    kit_hist_clear();
    for (int i = 0; i < KIT_HIST + 20; ++i)             /* the ring wraps: the newest 96 are kept */
        kit_hist_add(i * 900, 10000 + i);
    check("after the wrap: 3 h back", kit_change(180, &d, &cov) && d == 12 && cov == 180);

    /* the sun against NOAA (0.3 degrees; the azimuth near the zenith is loose by nature) */
    for (unsigned i = 0; i < sizeof(sun) / sizeof(sun[0]); ++i) {
        int az, el;
        char b[96];
        kit_sun_pos(sun[i].lat, sun[i].lon, sun[i].utc, &az, &el);
        int da = az - sun[i].az;
        if (da > 1800) da -= 3600;
        if (da < -1800) da += 3600;
        snprintf(b, sizeof(b), "sun %u: az %d (NOAA %d), el %d (NOAA %d)", i, az, sun[i].az, el, sun[i].el);
        check(b, near(el, sun[i].el, 3) && abs(da) <= (sun[i].el > 850 ? 30 : 3));
    }

    /* the timers */
    check("boil 1 min", kit_timer_seconds(KIT_T_BOIL, 300) == 60);
    check("boil 3 min above 2000 m", kit_timer_seconds(KIT_T_BOIL, 2500) == 180);
    check("boil, altitude unknown: 1 min", kit_timer_seconds(KIT_T_BOIL, -100000) == 60);
    check("SODIS 6 h", kit_timer_seconds(KIT_T_SODIS, 0) == 21600);
    check("SODIS cloudy 2 days", kit_timer_seconds(KIT_T_SODIS_CLOUD, 0) == 172800);
    check("chlorine dioxide 4 h", kit_timer_seconds(KIT_T_CLO2, 0) == 14400);
    check("shadow stick 15 min", kit_timer_seconds(KIT_T_SHADOW, 0) == 900);
    check("no such timer", kit_timer_seconds(KIT_T_N, 0) == 0);


    /* the pulse */
    {
        static const double rates[] = { 48, 72, 95, 150 };
        for (unsigned i = 0; i < 4; ++i) {
            int q = 0, b = run_pulse(rates[i], 20, 3000, 150, &q);
            char m[80];
            snprintf(m, sizeof(m), "pulse %.0f: got %d, quality %d", rates[i], b, q);
            check(m, near(b, (int)rates[i], 3) && q == 2);
        }
        {
            int q2 = 0, b2 = run_pulse(66, 25, 2000, 400, &q2);
            char m2[80];
            snprintf(m2, sizeof(m2), "a noisy pulse 66: got %d, quality %d", b2, q2);
            check(m2, near(b2, 66, 3) && q2 >= 1);
        }
        int q = 0, b = run_pulse(72, 20, 600, 300, &q);
        char m[80];
        snprintf(m, sizeof(m), "a weak, noisy pulse 72: got %d, quality %d", b, q);
        check(m, b == 0 || near(b, 72, 5));
        struct hrm_algo a;
        hrm_algo_reset(&a, 100);
        for (int i = 0; i < 2000; ++i)
            hrm_algo_push(&a, 250000 + noise(40));
        check("a flat signal: no rate", a.bpm == 0 && a.quality == 0);
    }

    printf("survkit host test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
