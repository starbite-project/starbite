/* SPDX-License-Identifier: MIT
 * Survival kit (Track D, 2026-11-16; kern/survkit.h): the barometer's history, altitude and storm warning; the sun's
 * position (north without a compass); the water-purification and shadow-stick timers.
 *
 * Sources for the numbers:
 * - Altitude: the international standard atmosphere's barometric formula, h = 44330.77 (1 - (p / p0)^0.190263) m.
 * - Storm rules: the usual barograph rules of thumb (a fall of about 1.5 hPa in 3 h: worse weather; 3.5 hPa: worse
 *   weather likely within a day; 6 hPa or 2.5 hPa in one hour: a storm or gale likely). A change of height reads as a
 *   change of pressure (about 1.2 hPa per 10 m), so the trend is only good while the phone stays at one height.
 * - The sun: the Astronomical Almanac's low-precision solar coordinates (about 0.01 degrees, 1950-2050), the hour
 *   angle from Greenwich mean sidereal time; refraction left out (it matters only near the horizon).
 * - Water: boil 1 minute (3 minutes above 2000 m): WHO / CDC. SODIS: 6 h in sun, 2 days under clouds (EAWAG). Chlorine
 *   tablets 30 min (double when the water is cold or cloudy); chlorine dioxide 4 h for Cryptosporidium; iodine 30 min
 *   (CDC). The shadow stick: 15 minutes between the two marks. */
#include "survkit.h"

#ifndef SURVKIT_HOST
#include "sb.h"
#include "kern.h"
#include "survival.h"
#include "shub.h"
#include "shub_more.h"
#include "sbapp.h"
#include "hrm.h"
#include "snd.h"
#endif

/* ---- small maths (no libm in the kernel) ---- */
#define K_PI 3.14159265358979323846
#define K_RAD (K_PI / 180.0)
#define K_LN2 0.69314718055994530942

static double k_mod(double x, double m)
{
    double r = x - m * (double)(int64_t)(x / m);
    return r < 0 ? r + m : r;
}

static double k_sin(double x)
{
    x = k_mod(x + K_PI, 2 * K_PI) - K_PI;
    double x2 = x * x, t = x, s = x;
    for (int k = 1; k < 12; ++k) {
        t *= -x2 / (double)((2 * k) * (2 * k + 1));
        s += t;
    }
    return s;
}

static double k_cos(double x) { return k_sin(x + K_PI / 2); }

static double k_atan(double x)
{
    int neg = x < 0, inv = 0, shift = 0;
    if (neg)
        x = -x;
    if (x > 1) {
        x = 1 / x;
        inv = 1;
    }
    if (x > 0.41421356237) {                /* tan(pi/8): atan(x) = pi/4 + atan((x - 1) / (x + 1)) */
        x = (x - 1) / (x + 1);
        shift = 1;
    }
    double x2 = x * x, t = x, s = x;
    for (int k = 1; k < 30; ++k) {
        t *= -x2;
        s += t / (double)(2 * k + 1);
    }
    if (shift)
        s += K_PI / 4;
    if (inv)
        s = K_PI / 2 - s;
    return neg ? -s : s;
}

static double k_atan2(double y, double x)
{
    if (x > 0)
        return k_atan(y / x);
    if (x < 0)
        return y >= 0 ? k_atan(y / x) + K_PI : k_atan(y / x) - K_PI;
    return y > 0 ? K_PI / 2 : y < 0 ? -K_PI / 2 : 0;
}

static double k_sqrt(double x) { return x > 0 ? __builtin_sqrt(x) : 0; }

static double k_asin(double x)
{
    if (x >= 1)
        return K_PI / 2;
    if (x <= -1)
        return -K_PI / 2;
    return k_atan2(x, k_sqrt(1 - x * x));
}

static double k_ln(double x)
{
    if (x <= 0)
        return -1e300;
    int e = 0;
    while (x > 1.5) {
        x /= 2;
        ++e;
    }
    while (x < 0.75) {
        x *= 2;
        --e;
    }
    double z = (x - 1) / (x + 1), z2 = z * z, t = z, s = z;     /* 2 atanh(z) */
    for (int k = 1; k < 20; ++k) {
        t *= z2;
        s += t / (double)(2 * k + 1);
    }
    return 2 * s + e * K_LN2;
}

static double k_exp(double y)
{
    if (y > 700)
        y = 700;
    if (y < -700)
        return 0;
    int k = (int)(y / K_LN2 + (y < 0 ? -0.5 : 0.5));
    double r = y - k * K_LN2, t = 1, s = 1;
    for (int i = 1; i < 20; ++i) {
        t *= r / i;
        s += t;
    }
    for (; k > 0; --k)
        s *= 2;
    for (; k < 0; ++k)
        s /= 2;
    return s;
}

static double k_pow(double b, double e) { return b > 0 ? k_exp(e * k_ln(b)) : 0; }

/* ---- altitude ---- */
int kit_altitude_m(int32_t hpa_x100, int32_t p0_x100)
{
    if (hpa_x100 <= 0 || p0_x100 <= 0)
        return -100000;
    double h = 44330.77 * (1.0 - k_pow((double)hpa_x100 / (double)p0_x100, 0.190263));
    return (int)(h < 0 ? h - 0.5 : h + 0.5);
}

int32_t kit_p0_for(int32_t hpa_x100, int alt_m)
{
    if (hpa_x100 < 30000 || hpa_x100 > 110000 || alt_m < -500 || alt_m > 9000)
        return 0;
    double f = 1.0 - (double)alt_m / 44330.77;
    double p0 = (double)hpa_x100 / k_pow(f, 5.255876);
    if (p0 < 85000 || p0 > 110000)
        return 0;
    return (int32_t)(p0 + 0.5);
}

/* ---- the sun ---- */
int kit_sun_pos(int32_t lat_x100, int32_t lon_x100, int64_t utc, int *az_x10, int *el_x10)
{
    double n = (double)utc / 86400.0 - 10957.5;                 /* days since J2000.0 (2000-01-01 12:00 UTC) */
    double L = k_mod(280.460 + 0.9856474 * n, 360.0);
    double g = k_mod(357.528 + 0.9856003 * n, 360.0) * K_RAD;
    double lam = (L + 1.915 * k_sin(g) + 0.020 * k_sin(2 * g)) * K_RAD;
    double eps = (23.439 - 0.0000004 * n) * K_RAD;
    double ra = k_atan2(k_cos(eps) * k_sin(lam), k_cos(lam));
    double dec = k_asin(k_sin(eps) * k_sin(lam));
    double gmst_h = k_mod(18.697374558 + 24.06570982441908 * n, 24.0);
    double lat = lat_x100 / 100.0 * K_RAD, lon = lon_x100 / 100.0;
    double H = k_mod(gmst_h * 15.0 + lon, 360.0) * K_RAD - ra;
    double sel = k_sin(lat) * k_sin(dec) + k_cos(lat) * k_cos(dec) * k_cos(H);
    double el = k_asin(sel);
    double az = k_atan2(-k_sin(H), k_sin(dec) / k_cos(dec) * k_cos(lat) - k_sin(lat) * k_cos(H));
    double azd = k_mod(az / K_RAD, 360.0), eld = el / K_RAD;
    *az_x10 = (int)(azd * 10 + 0.5) % 3600;
    *el_x10 = (int)(eld < 0 ? eld * 10 - 0.5 : eld * 10 + 0.5);
    return 0;
}

/* ---- the pressure history ---- */
static struct {
    int64_t t[KIT_HIST];
    int32_t p[KIT_HIST];                    /* hPa x10 */
    unsigned head, n;
} H;

void kit_hist_clear(void)
{
    H.head = H.n = 0;
}

void kit_hist_add(int64_t t_s, int32_t hpa_x10)
{
    H.t[H.head] = t_s;
    H.p[H.head] = hpa_x10;
    H.head = (H.head + 1) % KIT_HIST;
    if (H.n < KIT_HIST)
        ++H.n;
}

static int hist_at(unsigned back, int64_t *t, int32_t *p)  /* back 0: the newest */
{
    if (back >= H.n)
        return 0;
    unsigned i = (H.head + KIT_HIST - 1 - back) % KIT_HIST;
    *t = H.t[i];
    *p = H.p[i];
    return 1;
}

int kit_change(int minutes, int32_t *d_x10, int *covered_min)
{
    int64_t t0, t;
    int32_t p0, p;
    if (!hist_at(0, &t0, &p0))
        return 0;
    int64_t want = t0 - (int64_t)minutes * 60, tol = (int64_t)minutes * 60 / 4, best_e = -1;
    int32_t best_p = 0;
    int64_t best_t = 0;
    for (unsigned b = 1; hist_at(b, &t, &p); ++b) {
        int64_t e = t > want ? t - want : want - t;
        if (e <= tol && (best_e < 0 || e < best_e)) {
            best_e = e;
            best_p = p;
            best_t = t;
        }
    }
    if (best_e < 0)
        return 0;
    *d_x10 = p0 - best_p;
    *covered_min = (int)((t0 - best_t) / 60);
    return *covered_min > 0;
}

int kit_trend_level(int32_t d3, int32_t d1, int have3, int have1)
{
    if ((have3 && d3 <= -60) || (have1 && d1 <= -25))
        return KIT_STORM;
    if ((have3 && d3 <= -35) || (have1 && d1 <= -15))
        return KIT_FALLING_FAST;
    if (have3 && d3 >= 35)
        return KIT_RISING_FAST;
    if ((have3 && d3 <= -15) || (!have3 && have1 && d1 <= -8))
        return KIT_FALLING;
    if ((have3 && d3 >= 15) || (!have3 && have1 && d1 >= 8))
        return KIT_RISING;
    return KIT_STEADY;
}

/* the change scaled to the window asked for (a 2 h 40 min pair counts as 3 h at the same rate) */
__attribute__((unused)) static int scaled_change(int minutes, int32_t *d)
{
    int cov;
    if (!kit_change(minutes, d, &cov))
        return 0;
    *d = (int32_t)((int64_t)*d * minutes / cov);
    return 1;
}

/* ---- the timers ---- */
uint32_t kit_timer_seconds(int kind, int alt_m)
{
    switch (kind) {
    case KIT_T_BOIL: return alt_m > 2000 ? 180U : 60U;
    case KIT_T_SODIS: return 6U * 3600U;
    case KIT_T_SODIS_CLOUD: return 48U * 3600U;
    case KIT_T_CHLORINE: return 30U * 60U;
    case KIT_T_CHLORINE_COLD: return 60U * 60U;
    case KIT_T_CLO2: return 4U * 3600U;
    case KIT_T_IODINE: return 30U * 60U;
    case KIT_T_SHADOW: return 15U * 60U;
    default: return 0;
    }
}

#ifndef SURVKIT_HOST
/* ---- the kernel side ---- */
#define WATCH_MS (15U * 60U * 1000U)
#define READ_TRIES 30                       /* a reading: up to 30 s (the hub's start is about 4-7 s) */
#define PAGE_MS 3000U                       /* the page's keep-alive comes each second */
#define HIST_EVERY_S (15 * 60)

static int watch_on, watch_paused, reading, read_tries, hub_was_off, notified_level;
static uint64_t page_t, last_notify_ms;
static int32_t p0_x100 = KIT_P0_STD, last_hpa_x100;
static int p0_set;
static uint64_t last_hpa_ms;
static int timer_kind;
static uint64_t timer_end_ms;

static int64_t now_s(void) { return (int64_t)(sb_ms() / 1000U); }
static unsigned hist_pick;

static int page_open(void) { return page_t && sb_ms() - page_t < PAGE_MS; }

static int trend(int32_t *d3, int32_t *d1, int *h3, int *h1)
{
    *d3 = *d1 = 0;
    *h3 = scaled_change(180, d3);
    *h1 = scaled_change(60, d1);
    return kit_trend_level(*d3, *d1, *h3, *h1);
}

static void maybe_warn(void)
{
    int32_t d3, d1;
    int h3, h1, lv = trend(&d3, &d1, &h3, &h1);
    if (lv != KIT_FALLING_FAST && lv != KIT_STORM) {
        if (lv == KIT_STEADY || lv == KIT_RISING || lv == KIT_RISING_FAST)
            notified_level = 0;
        return;
    }
    uint64_t now = sb_ms();
    if (lv <= notified_level && last_notify_ms && now - last_notify_ms < 3U * 3600U * 1000U)
        return;
    notified_level = lv;
    last_notify_ms = now ? now : 1;
    if (lv == KIT_STORM)
        notif_post_app("survkit", "Storm likely",
                       "The air pressure is falling fast. Find or strengthen shelter, secure your gear, and stay off "
                       "ridges and open water.", 0);
    else
        notif_post_app("survkit", "Weather getting worse",
                       "The air pressure is falling: rain or wind likely within a day. Plan shelter and water.", 0);
    if (vib_ready())
        (void)vib_buzz(2, 400, 300);
    sb_log(lv == KIT_STORM ? "survkit: storm watch: storm likely (fast pressure fall)" :
                             "survkit: storm watch: worse weather (pressure falling)");
}

/* a fresh reading: into the history when the last is 15 min old */
static void got(int32_t hpa_x100)
{
    last_hpa_x100 = hpa_x100;
    last_hpa_ms = sb_ms();
    int64_t t;
    int32_t p;
    if (!hist_at(0, &t, &p) || now_s() - t >= HIST_EVERY_S) {
        kit_hist_add(now_s(), (hpa_x100 + 5) / 10);
        maybe_warn();
    }
}

static void reading_end(const char *why)
{
    reading = 0;
    kt_cancel(KT_SURV, KT_SURV_KIT_POLL);
    if (page_open())
        return;                             /* the page keeps the sensor */
    sens_release(SENS_PRESSURE);
    if (hub_was_off && surv_sensors_state() != SHUB_OFF)
        surv_sensors_on(0);                 /* ours alone: off now, not after the hub's 60 s idle */
    if (why)
        sb_logf("survkit: storm watch: ", why);
}

static void watch_tick(void)
{
    if (!watch_on || reading)
        return;
    if (surv_saver()) {
        watch_paused = 1;
        return;
    }
    watch_paused = 0;
    if (page_open()) {                      /* the page reads anyway */
        int32_t p, tc;
        if (sens_pressure_hpa(&p, &tc))
            got(p);
        return;
    }
    hub_was_off = surv_sensors_state() == SHUB_OFF;
    reading = 1;
    read_tries = 0;
    (void)sens_want(SENS_PRESSURE);
    kt_set(KT_SURV, KT_SURV_KIT_POLL, sb_ms(), 1000, 1);
}

static void poll_tick(void)
{
    int32_t p, tc;
    if (!reading) {
        kt_cancel(KT_SURV, KT_SURV_KIT_POLL);
        return;
    }
    if (sens_pressure_hpa(&p, &tc)) {
        got(p);
        reading_end(0);
        return;
    }
    if (surv_sensors_state() == SHUB_FAILED)
        reading_end("the sensor hub did not start");
    else if (++read_tries >= READ_TRIES)
        reading_end("no pressure reading in 30 s");
}

static void timer_end(void)
{
    static const char *const title[KIT_T_N] = { "", "Boiling done", "Sun disinfection done", "Sun disinfection done",
                                                "Chlorine done", "Chlorine done", "Chlorine dioxide done",
                                                "Iodine done", "Shadow stick" };
    static const char *const text[KIT_T_N] = {
        "",
        "Let the water cool, covered. It is safe to drink.",
        "6 hours of sun: the water in the clear bottles is safe to drink.",
        "2 days under clouds: the water in the clear bottles is safe to drink.",
        "30 minutes: the water is ready (a faint chlorine smell is right; none means add more).",
        "60 minutes (cold or cloudy water): the water is ready.",
        "4 hours: the water is ready, Cryptosporidium included.",
        "30 minutes: the water is ready (double it for cold water). Not for long use or in pregnancy.",
        "Mark the shadow's tip again now. The line from the first mark to the second points east; north is 90 "
        "degrees to its left." };
    int k = timer_kind;
    timer_kind = 0;
    timer_end_ms = 0;
    if (k <= 0 || k >= KIT_T_N)
        return;
    notif_post_app("survkit", title[k], text[k], 0);
    if (vib_ready())
        (void)vib_buzz(3, 500, 400);
    sb_logf("survkit: timer done: ", title[k]);
}


/* ---- the whistle and the siren on the loudspeaker (Track D 11-16) ----
 * A 3.15 kHz whistle tone (survival.c's surv_whistle) or a 650-1650 Hz wail (the siren), synthesized here at the
 * speaker's 48 kHz, at full volume on the loudspeaker; the route and volume put back at the end. Under Starbite OS
 * Next the shell's own sound glue is used (sh_snd_*, linked only there: weak here); the classic OS starts the sound
 * driver itself on the first use and polls it every 5 ms while a tone plays. Never in a test image (no sounds). */
int sh_snd_open(void) __attribute__((weak));
unsigned sh_snd_write(int id, const int16_t *frames, unsigned n) __attribute__((weak));
unsigned sh_snd_space(int id) __attribute__((weak));
void sh_snd_close(int id) __attribute__((weak));

#define KT_SURV_KIT_TONE 12U
#define TONE_RATE 48000U
static int tone_id = -1, tone_hz, own_snd_up, tone_vol_was = -1;
static uint32_t tone_ph, tone_n;
static int16_t tone_tab[256];

static unsigned t_space(void) { return sh_snd_space ? sh_snd_space(tone_id) : snd_space(tone_id); }
static unsigned t_write(const int16_t *f, unsigned n) { return sh_snd_write ? sh_snd_write(tone_id, f, n) : snd_write(tone_id, f, n); }

static void tone_fill(void)
{
    int16_t b[2 * 256];
    unsigned space = t_space();
    while (space) {
        unsigned k = space > 256U ? 256U : space;
        for (unsigned i = 0; i < k; ++i) {
            uint32_t hz = (uint32_t)tone_hz;
            if (!hz) {                      /* the siren: up for 1.5 s, down for 1.5 s */
                uint32_t c = tone_n % (3U * TONE_RATE), u = c < 3U * TONE_RATE / 2U ? c : 3U * TONE_RATE - c;
                hz = 650U + (uint32_t)((uint64_t)u * 1000U / (3U * TONE_RATE / 2U));
            }
            tone_ph += (uint32_t)(((uint64_t)hz << 32) / TONE_RATE);
            int32_t v = tone_tab[tone_ph >> 24] * 3 / 2;     /* a little clipped: louder for the same peak */
            v = v > 30000 ? 30000 : v < -30000 ? -30000 : v;
            b[2 * i] = b[2 * i + 1] = (int16_t)v;
            ++tone_n;
        }
        if (t_write(b, k) < k)
            break;
        space -= k;
    }
}

int survkit_tone(int hz, int on)
{
    if (!on) {
        if (tone_id < 0)
            return 0;
        kt_cancel(KT_SURV, KT_SURV_KIT_TONE);
        if (sh_snd_close)
            sh_snd_close(tone_id);
        else
            snd_close(tone_id);
        tone_id = -1;
        snd_route(SND_OUT_AUTO);
        if (tone_vol_was >= 0)
            snd_volume(SND_OUT_SPEAKER, (unsigned)tone_vol_was);
        tone_vol_was = -1;
        sb_log("survkit: the tone off (the speaker's route and volume back)");
        return 0;
    }
    if (sbos_selftest_enabled && !sbos_selftest_sound)
        return -1;                          /* no sounds from a test image */
    if (tone_id >= 0) {
        tone_hz = hz;
        return 0;
    }
    if (!tone_tab[64])
        for (int i = 0; i < 256; ++i)
            tone_tab[i] = (int16_t)(k_sin(2 * K_PI * i / 256.0) * 26000.0);
    if (sh_snd_open) {
        tone_id = sh_snd_open();
    } else {
        if (!own_snd_up) {
            snd_init_libsb();
            own_snd_up = 1;
            sb_log("survkit: sound started for the whistle / siren");
        }
        tone_id = snd_open();
    }
    if (tone_id < 0) {
        tone_id = -1;
        sb_log("survkit: the tone refused: no free sound stream (a call has the speaker?)");
        return -1;
    }
    tone_vol_was = (int)snd_get_volume(SND_OUT_SPEAKER);
    snd_route(SND_OUT_SPEAKER);
    snd_volume(SND_OUT_SPEAKER, SND_VOL_MAX);
    tone_hz = hz;
    tone_ph = tone_n = 0;
    tone_fill();
    kt_set(KT_SURV, KT_SURV_KIT_TONE, sb_ms(), sh_snd_open ? 20U : 5U, 1);
    sb_log(hz ? "survkit: the whistle on (3.15 kHz, full volume)" : "survkit: the siren on (650-1650 Hz, full volume)");
    return 0;
}

static void tone_tick(void)
{
    if (tone_id < 0) {
        kt_cancel(KT_SURV, KT_SURV_KIT_TONE);
        return;
    }
    if (!sh_snd_open)
        snd_poll(sb_us());
    tone_fill();
}

void survkit_timer(unsigned id)
{
    if (id == KT_SURV_KIT_WATCH)
        watch_tick();
    else if (id == KT_SURV_KIT_POLL)
        poll_tick();
    else if (id == KT_SURV_KIT_TIMER)
        timer_end();
    else if (id == KT_SURV_KIT_HRM)
        hrm_tick();
    else if (id == KT_SURV_KIT_TONE)
        tone_tick();
}

static void watch_set(int on)
{
    watch_on = on != 0;
    if (watch_on) {
        kt_set(KT_SURV, KT_SURV_KIT_WATCH, sb_ms(), WATCH_MS, 1);
        watch_tick();                       /* one reading now */
        sb_log("survkit: storm watch on (a reading every 15 min)");
    } else {
        kt_cancel(KT_SURV, KT_SURV_KIT_WATCH);
        if (reading)
            reading_end("off");
        watch_paused = 0;
    }
}

static int alt_now(void)
{
    if (!last_hpa_ms || sb_ms() - last_hpa_ms > 120000U)
        return -100000;
    return kit_altitude_m(last_hpa_x100, p0_x100);
}

int64_t survkit_get(unsigned key)
{
    switch (key) {
    case SET_KIT_BARO: {
        int32_t p, tc;
        if (surv_saver())
            return -2;
        if (sens_pressure_hpa(&p, &tc)) {
            got(p);
            return (p + 5) / 10;
        }
        return surv_sensors_state() == SHUB_FAILED ? -3 : -1;
    }
    case SET_KIT_ALT: return alt_now();
    case SET_KIT_REF: return (p0_x100 + 5) / 10 | (int64_t)(p0_set != 0) << 16;
    case SET_KIT_TREND: {
        int32_t d3, d1;
        int h3, h1, lv = trend(&d3, &d1, &h3, &h1);
        if (d3 < -2048) d3 = -2048;
        if (d3 > 2047) d3 = 2047;
        if (d1 < -2048) d1 = -2048;
        if (d1 > 2047) d1 = 2047;
        return lv | h3 << 3 | h1 << 4 | (int64_t)((d3 + 2048) & 0xfff) << 8 | (int64_t)((d1 + 2048) & 0xfff) << 20;
    }
    case SET_KIT_WATCH: return watch_on | (watch_on && surv_saver()) << 1;
    case SET_KIT_HIST: {
        int64_t t0, t1;
        int32_t p;
        int span = 0;
        if (hist_at(0, &t0, &p) && hist_at(H.n - 1, &t1, &p))
            span = (int)((t0 - t1) / 60);
        return (int64_t)H.n | (int64_t)(span > 0xffffff ? 0xffffff : span) << 8;
    }
    case SET_KIT_SUN: {
        struct sbos_time t;
        int az, el;
        if (surv_lat_x100 == SURV_NOLOC || surv_lon_x100 == SURV_NOLOC)
            return -2;
        if (!clock_now(&t) || !t.utc)
            return -3;
        kit_sun_pos(surv_lat_x100, surv_lon_x100, t.utc, &az, &el);
        return az | (int64_t)(el + 900) << 12;
    }
    case SET_KIT_TIMER: {
        if (!timer_kind)
            return 0;
        uint64_t now = sb_ms();
        uint32_t left = timer_end_ms > now ? (uint32_t)((timer_end_ms - now + 999U) / 1000U) : 0;
        return timer_kind | (int64_t)left << 8;
    }
    case SET_KIT_BOIL_S: return kit_timer_seconds(KIT_T_BOIL, alt_now());
    case SET_KIT_HR: {
        struct hrm_status s;
        hrm_status(&s);
        int sig = s.dc <= 0 ? 0 : s.dc >= 524287 ? 100 : (int)((int64_t)s.dc * 100 / 524287);
        return s.state | s.finger << 2 | s.quality << 3 | (int64_t)(s.bpm & 0x1ff) << 5 |
               (int64_t)(s.seconds > 255 ? 255 : s.seconds) << 14 | (int64_t)sig << 22;
    }
    case SET_KIT_SIREN: return sbos_selftest_enabled && !sbos_selftest_sound ? -1 : (tone_id >= 0 && !tone_hz);
    case SET_KIT_HR_INFO: {
        struct hrm_status s;
        hrm_status(&s);
        return (s.led_ma10 & 0xfff) | (int64_t)(s.rate_hz & 0x3ff) << 12;
    }
    case SET_KIT_HIST_AT: {
        int64_t t;
        int32_t p;
        if (!hist_at(hist_pick, &t, &p))
            return -1;
        int64_t age = (now_s() - t) / 60;
        return (int64_t)(p & 0xffff) | (age > 0x7fff ? 0x7fff : age) << 16;
    }
    default: return SBOS_EINVAL;
    }
}

int64_t survkit_set(unsigned key, int64_t v)
{
    switch (key) {
    case SET_KIT_BARO:
        if (v) {
            if (surv_saver())
                return SBOS_EPERM;
            page_t = sb_ms() ? sb_ms() : 1;
            int32_t p, tc;
            if (sens_pressure_hpa(&p, &tc))
                got(p);
        } else {
            page_t = 0;
            if (!reading)
                sens_release(SENS_PRESSURE);
        }
        return 0;
    case SET_KIT_ALT:
        if (v == -100000) {
            p0_x100 = KIT_P0_STD;
            p0_set = 0;
            sb_log("survkit: the altitude reference back to 1013.25 hPa");
            return 0;
        } else {
            int32_t p0;
            if (!last_hpa_ms || sb_ms() - last_hpa_ms > 120000U)
                return SBOS_EINVAL;         /* no reading to set it from */
            p0 = kit_p0_for(last_hpa_x100, (int)v);
            if (!p0)
                return SBOS_EINVAL;
            p0_x100 = p0;
            p0_set = 1;
            sb_logx("survkit: the altitude reference set from a known altitude, hPa x100 ", (uint32_t)p0);
            return 0;
        }
    case SET_KIT_WATCH:
        watch_set((int)v);
        return 0;
    case SET_KIT_HIST_AT:
        hist_pick = v < 0 ? 0 : (unsigned)v;
        return 0;
    case SET_KIT_SIREN:
        if (!v)
            return tone_hz ? 0 : survkit_tone(0, 0);
        return survkit_tone(0, 1) == 0 ? 0 : SBOS_EAGAIN;
    case SET_KIT_HR: {
        static int told;
        if (!v) {
            hrm_stop("its page closed");
            told = 0;
            return 0;
        }
        if (hrm_want() == 0)
            return 0;
        if (!told++)
            notif_post_app("survkit", "Heart rate: the sensor did not start", hrm_why(), 0);
        return SBOS_EINVAL;
    }
    case SET_KIT_TIMER: {
        int k = (int)v;
        if (k == 0) {
            if (timer_kind)
                sb_log("survkit: timer stopped");
            timer_kind = 0;
            timer_end_ms = 0;
            kt_cancel(KT_SURV, KT_SURV_KIT_TIMER);
            return 0;
        }
        uint32_t s = kit_timer_seconds(k, alt_now());
        if (!s)
            return SBOS_EINVAL;
        timer_kind = k;
        timer_end_ms = sb_ms() + (uint64_t)s * 1000U;
        kt_set(KT_SURV, KT_SURV_KIT_TIMER, sb_ms(), s * 1000U, 0);
        sb_logx("survkit: timer started, seconds ", s);
        return 0;
    }
    default:
        return SBOS_EINVAL;
    }
}

int survkit_saved_key(unsigned key)
{
    return key == SET_KIT_WATCH || key == SET_KIT_ALT;
}

void survkit_pack(uint8_t o[16])
{
    o[2] = (uint8_t)((o[2] & ~4U) | (watch_on ? 4U : 0U));
    uint32_t r = p0_set ? (uint32_t)((p0_x100 + 5) / 10 - 9000) : 0U;
    o[14] = (uint8_t)r;
    o[15] = (uint8_t)(r >> 8);
}

void survkit_unpack(const uint8_t in[16])
{
    uint32_t r = (uint32_t)in[14] | (uint32_t)in[15] << 8;
    if (r && r <= 2000U) {                  /* 900.0 .. 1100.0 hPa */
        p0_x100 = (int32_t)(r + 9000U) * 10;
        p0_set = 1;
    }
    if ((in[2] & 4U) && !watch_on) {        /* on again after a restart: the first reading in 15 min */
        watch_on = 1;
        kt_set(KT_SURV, KT_SURV_KIT_WATCH, sb_ms(), WATCH_MS, 1);
    }
}
#endif
