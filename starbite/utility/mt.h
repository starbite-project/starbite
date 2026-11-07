/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS multi-touch and raw keys for the keyboard, gamepad and remote screens (mt.c).
 * ui_poll reads finger 0 only (libsb sb_touch_event drops the others), so these screens read every finger
 * with sb_touch_events and the keys with sb_keys instead of ui_poll; ut_service still runs every turn and
 * any touch or key restarts the idle timer (the screen saver and the idle return count from it). */
#pragma once
#include <stdint.h>

#define MT_N 10
enum { MT_DOWN = 1, MT_MOVE, MT_UP };

struct mt_ev { int id, st, x, y; };
struct mt {
    int down[MT_N], x[MT_N], y[MT_N], x0[MT_N], y0[MT_N];
    uint64_t t0[MT_N];                  /* ms the finger went down */
    unsigned keys;                      /* SB_K_* held (debounced) */
    unsigned pressed, released;         /* since the last mt_poll */
    uint64_t kt0[4];                    /* ms each key (bit index) went down */
    unsigned wakes;                     /* ut_saver_wakes when last polled */
    unsigned ignore;                    /* fingers down before the screen or a saver wake: until they lift */
};

void mt_reset(struct mt *m);
/* Every finger event since the last call (at most max) and the keys; also runs ut_service. A screen saver
 * wake in between lifts every finger first (MT_UP events for them). */
int mt_poll(struct mt *m, struct mt_ev *ev, int max);
/* ms a key (SB_K_*) has been held, 0 when up. */
uint64_t mt_held_ms(const struct mt *m, unsigned key);
/* Fingers down now. */
int mt_count(const struct mt *m);
