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
/* the live values: local time (wday 0 = Sunday, mon 1..12), battery %, temperature C; rebuilds what changed */
void sh_status(int h, int mi, int wday, int day, int mon, int pct, int temp);                       /* idle time: prepare the caches the next gesture needs */

/* provided by the platform: slide to start finished (the kernel: the loader's one-shot next boot) */
void sh_boot_os(const char *key);
/* provided by the platform: the torch hold toggled the flashlight */
void sh_torch(int lit);
/* the status bar's left text from the modem's status (default "No SIM") */
void sh_carrier(const char *text);

#endif
