/* SPDX-License-Identifier: GPL-2.0-only
 * gwin-host: the kernel calls gwin.c makes, for the host harness */
#pragma once
#include <stdint.h>
int hw_m3_on(void);
/* v3: the wall clock for a Linux notification's time (kern.h / sdk/sbapp.h's, the fields gwin.c reads) */
struct sbos_time {
    int64_t utc;
    int32_t y, mo, d, h, mi, s, wday, dst;
    int32_t off_min;
    char abbr[8];
    char zone[24];
    int32_t approx;
};
int clock_now(struct sbos_time *t);
/* 8l / 8m (Track A): the linuxtest's network check: mobile data, else the saved Wi-Fi (kern.h's); the harness has
 * neither: data never comes up (no SIM), Wi-Fi refuses, so the check says "skipped" */
enum { KCELL_OFF = 0, KCELL_STARTING, KCELL_NO_SIM, KCELL_SIM_LOCKED, KCELL_SEARCHING, KCELL_REGISTERED,
       KCELL_DATA, KCELL_STOPPING, KCELL_FAILED };
struct kcell { int32_t state, want, bars; char why[64]; };
static inline void kmodem_cell(struct kcell *c) { c->state = KCELL_NO_SIM; c->want = 0; c->bars = -1; c->why[0] = 0; }
/* 8z: the linuxtest's battery line */
struct sbos_battery { int32_t ok, pct, pct_x10, mv, ma, avg_ma, temp_c10, charger, cap_mah, full_mah; };
static inline void battery_get(struct sbos_battery *b) { b->ok = 1; b->pct = 80; b->pct_x10 = 800; b->mv = 3900; b->ma = -450; b->avg_ma = -420;
                                                         b->temp_c10 = 312; b->charger = 0; b->cap_mah = 2400; b->full_mah = 3000; }
static inline int kmodem_selftest_data(int on) { (void)on; return 0; }
static inline int wifi_power(int on) { (void)on; return 0; }
static inline int wifi_selftest_uplink(void) { return -1; }
static inline void guestnet_log(void) { }
