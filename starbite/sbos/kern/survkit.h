/* SPDX-License-Identifier: MIT
 * Survival kit (Track D, 2026-11-16): the barometer's history, altitude and storm warning; the sun's position for
 * north without a compass; the water-purification and shadow-stick timers. The Survival kit app (apps/survkit.c)
 * reads it through the SET_KIT_* settings (sdk/sbapp.h).
 *
 * The pressure itself comes from the drivers track's shub_more.c (sens_pressure_hpa: the ST LPS22H behind the
 * BCM4775 sensor hub, the hub started through survival.c from the card's firmware).
 *
 * Power: nothing runs unless asked. The barometer page keeps the hub on while it is open; Storm watch (off by
 * default, remembered) wakes the hub for one reading every 15 minutes and lets it stop again. Timers are one
 * kernel timer each, ending in a notification and three buzzes (no sound). */
#ifndef SBOS_SURVKIT_H
#define SBOS_SURVKIT_H
#include <stdint.h>

/* KT_SURV ids from 8 up are this file's (survival.c's surv_timer passes them on) */
#define KT_SURV_KIT0 8U
#define KT_SURV_KIT_WATCH 8U                /* Storm watch: one reading every 15 min */
#define KT_SURV_KIT_POLL 9U                 /* a reading being taken: a look every second, up to 30 s */
#define KT_SURV_KIT_TIMER 10U               /* the purification / shadow-stick timer's end */
#define KT_SURV_KIT_HRM 11U                 /* the heart-rate sensor's FIFO, every 100 ms (hrm.c) */
void survkit_timer(unsigned id);
/* the loudspeaker's whistle (hz) or siren (0): survival.c's surv_whistle uses it; -1 refused (a test image, busy) */
int survkit_tone(int hz, int on);

/* the app's settings (sdk/sbapp.h SET_KIT_FIRST..SET_KIT_LAST) */
int64_t survkit_get(unsigned key);
int64_t survkit_set(unsigned key, int64_t v);
int survkit_saved_key(unsigned key);        /* 1: a change to save (Storm watch, the altitude reference) */

/* survival.c's "sbos.surv" record: o[2] bit 2 Storm watch, o[14..15] the sea-level reference (hPa x10 - 9000, 0 =
 * the standard 1013.25) */
void survkit_pack(uint8_t o[16]);
void survkit_unpack(const uint8_t in[16]);

/* ---- the sums (also host-tested: kern/host/survkit-test.c) ---- */
#define KIT_P0_STD 101325                   /* hPa x100 */
/* altitude in metres for a pressure and a sea-level reference (both hPa x100), the standard atmosphere */
int kit_altitude_m(int32_t hpa_x100, int32_t p0_x100);
/* the sea-level reference that makes the pressure read as alt_m (hPa x100); 0 for nonsense */
int32_t kit_p0_for(int32_t hpa_x100, int alt_m);
/* the sun's azimuth (0.1 degrees from true north, clockwise) and elevation (0.1 degrees, refraction ignored) at a
 * UTC time and a place (0.01 degrees, N / E positive); 0 */
int kit_sun_pos(int32_t lat_x100, int32_t lon_x100, int64_t utc, int *az_x10, int *el_x10);

/* the pressure history and its trend */
#define KIT_HIST 96                         /* 24 h at 15 min */
#define KIT_STEADY 0
#define KIT_RISING 1
#define KIT_FALLING 2
#define KIT_FALLING_FAST 3                  /* worse weather likely within a day */
#define KIT_STORM 4                         /* a storm likely */
#define KIT_RISING_FAST 5
void kit_hist_add(int64_t t_s, int32_t hpa_x10);
void kit_hist_clear(void);
/* the change over about `minutes` (hPa x10) and the minutes it really covers; 0 no pair of readings */
int kit_change(int minutes, int32_t *d_x10, int *covered_min);
int kit_trend_level(int32_t d3h_x10, int32_t d1h_x10, int have3, int have1);

/* ---- named calls for Starbite OS Next's pages (Track A); the same as the SET_KIT_* keys ---- */
int surv_baro(int32_t *hpa_x10);            /* 1 a fresh reading; also the page's keep-alive (call ~1/s on screen);
                                             * 0 none yet, -2 refused (Extreme saving / Last 2 %), -3 the hub failed */
void surv_baro_close(void);
int surv_baro_alt(int *alt_m, int *from_known);    /* 1 known */
int surv_baro_alt_set(int known_alt_m);     /* 0 set; -100000 back to 1013.25 hPa; <0 no reading / nonsense */
int surv_baro_trend(int *level, int32_t *d3_x10, int32_t *d1_x10, int *have3, int *have1);   /* returns level */
int surv_baro_hist(int i, int32_t *hpa_x10, int *age_min);     /* i 0 the newest; 1 there is one */
int surv_baro_hist_n(void);
int surv_storm_watch(int on);               /* -1 reads; 1 / 0 sets; returns on | paused << 1 */
int surv_sun_pos_now(int *az_x10, int *el_x10);    /* 0, -2 no location, -3 no clock */
int surv_water_timer(int kind);             /* KIT_T_* starts (one at a time), 0 stops; 0 or <0 */
int surv_water_left(int *kind);             /* seconds left (0 none) */

/* the timers */
enum { KIT_T_NONE, KIT_T_BOIL, KIT_T_SODIS, KIT_T_SODIS_CLOUD, KIT_T_CHLORINE, KIT_T_CHLORINE_COLD, KIT_T_CLO2,
       KIT_T_IODINE, KIT_T_SHADOW, KIT_T_N };
uint32_t kit_timer_seconds(int kind, int alt_m);    /* alt_m: -100000 unknown */
#endif
