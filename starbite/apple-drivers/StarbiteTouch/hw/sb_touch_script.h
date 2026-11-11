/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Scripted input and the packet self-test, for unattended test boots (PHONE-TEST.md). Plain C, host-tested.
 *
 * Script (boot-arg sbtouchscript=, the syntax of today's sweetscript= 0420 minus Home, which is a button):
 *   <s>t<x>:<y>                 tap at panel pixels x,y, <s> seconds after the script's start
 *   <s>s<x0>:<y0>:<x1>:<y1>     swipe: 51 samples 6 ms apart, eased in and out (smoothstep), then up
 * steps separated by commas, at most 24, e.g. 20t556:1406,30s720:2300:720:900. Scripted contacts use finger 9
 * and enter the driver exactly where the chip's contacts do (after decoding), so they test the HID path.
 *
 * Self-test: a fixed list of real S6SY761 event packets (tap, hold-and-drag, two-finger scroll, two-finger tap,
 * an edge swipe, a status event, an empty read) decoded and run through a private gesture engine or digitizer
 * with simulated time; nothing reaches the HID system. The driver logs the counts; the host test checks them.
 */
#ifndef SB_TOUCH_SCRIPT_H
#define SB_TOUCH_SCRIPT_H

#include <stdint.h>
#include <stdbool.h>
#include "sb_touch_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SB_SCRIPT_MAX           24
#define SB_SCRIPT_FINGER        9
#define SB_SCRIPT_SWIPE_SAMPLES 50
#define SB_SCRIPT_SAMPLE_MS     6
#define SB_SCRIPT_TAP_MS        80

struct sb_script_step {
	uint64_t at_ms;
	char kind;                      /* 't' or 's' */
	int x0, y0, x1, y1;
};

struct sb_script {
	struct sb_script_step steps[SB_SCRIPT_MAX];
	unsigned n;
	int bad;                        /* index of the first bad step, -1 none */
	unsigned step, sample;          /* the player */
};

/* Parses; returns the number of steps (a bad step stops the parse, as sweetscript did). */
unsigned sb_script_parse(struct sb_script *s, const char *text);
/* If a sample is due at now_ms, writes it and returns true. *next_ms = when the next sample is due
 * (UINT64_MAX when the script is over). */
bool sb_script_next(struct sb_script *s, uint64_t now_ms, int *finger, bool *down, int *x, int *y,
    uint64_t *next_ms);

struct sb_selftest_result {
	unsigned packets, contacts, ignored;
	/* pointer */
	unsigned rel_moves, presses1, presses2, releases, scrolls, abs_events;
	unsigned taps, secondary, drags;
	/* digitizer */
	unsigned frames, edge_frames, touch_changes;
	bool all_up;                    /* the last frame / state has no finger down */
	bool pass;
};

/* pointer: run the macOS engine (cfg->mode decides direct / absolute); else the iOS digitizer. */
void sb_touch_selftest(bool pointer, const struct sb_tg_config *tg, const struct sb_dig_config *dig,
    unsigned panel_w, unsigned panel_h, struct sb_selftest_result *r);

#ifdef __cplusplus
}
#endif
#endif /* SB_TOUCH_SCRIPT_H */
