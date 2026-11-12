/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: haptic patterns on the vibration motor (haptic.c; docs/drivers/HAPTICS.md).
 * vib.c drives the LRA (PWM timer 1 + the MAX77705's haptic bits); this plays patterns on it: on / off / strength
 * segments, looped for a ringtone until stopped, the standard set below, and a global intensity 0..3.
 *
 * Strength = the PWM duty between 50 % (no drive) and the DT's rated 70 % (haptic,normal_ratio = 70, duty 34179 of
 * 48828 ns); never above it (the DT's overdrive_ratio 95 is never used). Safety: an "on" segment is at most 2 s, the
 * motor is never driven for more than 5 s without a rest of at least 250 ms (a forced 1 s rest is inserted), and a
 * looped pattern stops by itself after 60 s. */
#ifndef SBOS_HAPTIC_H
#define SBOS_HAPTIC_H
#include <stdint.h>

enum {
    HAPTIC_RING = 0,                /* looped until haptic_stop (or 60 s): an incoming call */
    HAPTIC_NOTIFY,                  /* a message / notification: two short taps */
    HAPTIC_ALARM,                   /* looped: three pulses, a pause */
    HAPTIC_TICK,                    /* a key / keyboard tick */
    HAPTIC_SUCCESS,                 /* like iOS's notification feedbacks */
    HAPTIC_WARNING,
    HAPTIC_ERROR,
    HAPTIC_N
};

struct haptic_seg {
    uint16_t ms;                    /* 1..2000 for an "on" segment (longer is cut), any for a gap */
    uint8_t strength;               /* 0 a gap (motor off), 1..100 % of the rated drive */
};

/* 0 started (or the intensity is 0: nothing to feel, nothing driven); <0: the motor is refused (haptic_why), or a
 * looped pattern (ring / alarm) is playing and this one-shot is dropped (SBOS_EBUSY-like -16). A looped pattern
 * replaces anything; a one-shot replaces a one-shot. */
int haptic_play(int which);
int haptic_play_segs(const struct haptic_seg *s, unsigned n, int loop);    /* s must stay valid while it plays */
void haptic_stop(void);
int haptic_playing(void);           /* 0 none, 1 one-shot, 2 looped */
void haptic_set_intensity(int level);   /* 0 off, 1 light, 2 medium, 3 strong (the default: today's vib.c drive) */
int haptic_intensity(void);
const char *haptic_why(void);       /* plain words when the motor is refused */
const char *haptic_name(int which);
/* the duty (per mille) a strength gets at the current intensity: 0 for none, else 540..700 */
unsigned haptic_duty_for(unsigned strength);

/* the on-phone probe (drvprobe.h) */
int drv_haptic_probe(int level, unsigned *wait_ms);
void drv_haptic_abort(void);
#define DRV_HAPTIC_BUDGET_L0 1000U
#define DRV_HAPTIC_BUDGET_L1 100U
#define DRV_HAPTIC_BUDGET_L2 12000U
#endif
