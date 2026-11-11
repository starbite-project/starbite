/* SPDX-License-Identifier: MIT
 * The Starbite OS shell on sbtk (P1: the lock screen, its Systems flows and the unlock to a placeholder home),
 * after the approved concept (docs/concepts/starbite-os-shell/index.html). Coordinates in concept pt. */
#ifndef SB_SHELL_H
#define SB_SHELL_H
#include "sbtk.h"

int sh_init(tk_comp *c);                  /* builds the wallpaper, the lock, the home; needs tk_pack_open first */
void sh_init_begin(tk_comp *c);           /* the same in steps: begin, then sh_init_step() until it returns 0 */
int sh_init_step(void);
/* deterministic states, as the concept's sbSet(): "unlock" v; "group" key ("" = none); "focus" key with p;
 * "scroll" v (the list's scroll in pt); "hold" which (0 torch, 1 Emergency) with p (0..1, the ring) */
void sh_set(const char *what, const char *key, float v);
/* touch: phase 0 down, 1 move, 2 up; t in ms */
void sh_touch(int phase, float x, float y, float t_ms);
/* advance time by dt s (the springs, holds, transitions); returns 1 while anything moves */
int sh_tick(float dt, float t_ms);
int sh_moving(void);
void sh_idle(void);
int sh_idle_pending(void);                /* idle work left (call sh_idle again soon) */
/* the live values: local time (wday 0 = Sunday, mon 1..12), battery %, temperature C; rebuilds what changed */
void sh_status(int h, int mi, int wday, int day, int mon, int pct, int temp);                       /* idle time: prepare the caches the next gesture needs */

/* provided by the platform: slide to start finished (the kernel: the loader's one-shot next boot) */
void sh_boot_os(const char *key);
/* provided by the platform: the torch hold toggled the flashlight */
void sh_torch(int lit);
/* the status bar's left text from the modem's status (default "No SIM") */
void sh_carrier(const char *text);
/* the modem's status for the UI: the fields of the modem session's snapshot that the shell shows (docs/MODEM.md
 * 7.7, struct sbm_modem_snap): state, SIM status (0x80 absent, -1 not known), emergency-only, RAT (1 GSM .. 7 LTE,
 * 0 unknown), bars 0..5 (-1 unknown). Never the PLMN or a cell id: nothing here is logged. Redraws only on a change. */
enum { SH_CELL_OFF, SH_CELL_ON, SH_CELL_RESTARTING, SH_CELL_GUARD, SH_CELL_FAILED, SH_CELL_STARTING };
typedef struct { int state, sim, emergency_only, rat, bars; } sh_cell_t;
void sh_cell(const sh_cell_t *c);
/* the platform's modem power (the kernel: kmodem_power: 1 asks for its hold confirmation, 0 stops); < 0 refused */
int sh_modem(int on);
/* a hold confirmation drawn by the shell (the kernel's kui_confirm): the answer goes to the platform's
 * sh_confirmed(1) on a full hold, sh_confirmed(0) on a tap outside the card */
void sh_confirm(const char *title, const char *l1, const char *l2, const char *l3, const char *hold);
void sh_confirmed(int ok);
/* P2r, sleep and wake: the screen off as a fade to black (now: black at once), on as the lock screen from black;
 * the shell calls sh_slept (the platform) when the fade has ended: the panel off after that frame */
void sh_sleep(int now);
void sh_wake(void);
int sh_sleeping(void);
void sh_slept(void);
/* P2r: the Control Centre's brightness slider (the panel's levels 1 dim .. 4 bright), and the level at start */
void sh_brightness(int level);
void sh_bright_now(int level);
/* P2y: the apps' live values besides sh_status: charging or not, the software's name and version */
void sh_sysinfo(int charging, const char *version);
void sh_alarm(int on, int h, int m);
/* P2z, the Clock app: when to come back (ms; < 0 nothing pending), the redraw then (1: something changed), and the
 * platform's vibration for a timer that ran out (no sound) */
int sh_next_update(float t_ms);
int sh_refresh(float t_ms);
void sh_vibrate(void);
void sh_year(int y);                      /* P3c: the year (the Calendar's month) */      /* the next alarm for the lock's widget (on 0: "No alarm") */

#endif
