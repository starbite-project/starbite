/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track (docs/STARBITE-OS-DRIVERS.md): the shared contract of the on-phone driver probes.
 *
 * One combined probe image (starbite-next-drvprobe.img, uitest.c -DSBOS_SELFTEST_NEXT_DRV) runs every driver's probe
 * in turn under its own 60 s watchdog, then restarts into recovery so the log can be read from pmsg. Each driver
 * gives one step function:
 *
 *   int drv_<name>_probe(int level, unsigned *wait_ms);
 *
 *   level 0  read-only: only register / bus reads of state the phone is already in (IDs, pin and supply state). No
 *            write that changes what any part does. Always run first.
 *   level 1  active but silent: a part may be powered, reset, clocked and talked to (an ID read that needs the part
 *            powered, a firmware download into RAM, an HCI reset), and must be put back as found (powered off, pins
 *            and clocks restored) before the step returns 1. Nothing a person would see, hear or feel.
 *   level 2  attended: a person notices it (a tone, a vibration pattern, a camera frame on screen). Only in the
 *            attended image (-DSBOS_SELFTEST_NEXT_DRV_ATTENDED).
 *
 *   Each call does at most ~100 ms of work. Return 0: call again after *wait_ms ms (0 = at once); 1: finished.
 *   Log lines start "drvprobe: <name>: "; the last line is "drvprobe: <name>: PASS <what was seen>" or
 *   "drvprobe: <name>: FAIL <plain-words why>". A step must also end (FAIL, everything put back) when it has not
 *   finished within its own time budget, so one stuck part never stalls the image.
 *
 *   void drv_<name>_abort(void);   everything put back as found; safe at any point (the watchdog path calls it).
 *
 * Never: EFS / CPEFS / PARAM, the battery's PCM, disabling a protection, a write outside the part's own block. */
#ifndef SBOS_DRVPROBE_H
#define SBOS_DRVPROBE_H

#define DRV_LVL_READ 0
#define DRV_LVL_ACTIVE 1
#define DRV_LVL_ATTENDED 2

struct drv_probe {
    const char *name;
    int (*probe)(int level, unsigned *wait_ms);
    void (*abort)(void);
    unsigned budget_ms;                     /* the step's own limit across all its calls at one level */
};

/* The list (drvprobe.c): every driver of the track, in the order the image runs them. */
extern const struct drv_probe drv_probes[];
extern const unsigned drv_nprobes;

/* The runner (drvprobe.c): level 0 for every probe, then level 1, then (attended) level 2. */
void drv_run_begin(int attended);
int drv_run_step(unsigned *wait_ms);        /* 0: again after *wait_ms; 1: all done */
void drv_run_abort(void);

#endif
