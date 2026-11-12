/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite firmware layer (docs/STARBITE-FIRMWARE-LAYER.md), the loader side (sbfw.c): the per-OS page
 * (debug level, health checks, per-OS actions) and the debug console region.
 */
#pragma once
#include <stdint.h>

/* The per-OS page for catalog entry cat (Bixby / Back returns). Draws on the framebuffer itself:
 * the caller presents its own screen again afterwards. 1 if the debug level changed. */
int sbfw_page(int cat);

/* Debug level 0..3 (Off, Normal, High, Highest) = the settings record's log level. */
unsigned sbfw_level(void);
/* The first panel row the console holds at this level (High: bottom 20 %, Highest: 40 %), or
 * 2960 (none). */
int sbfw_console_y0(unsigned level);
/* Draw the console (the newest lines of the pmsg ring) if the ring changed or force; no-op below High. */
void sbfw_console_draw(int force);
/* A loader stage line for the console (written to pmsg at Highest only). */
void sbfw_console_note(const char *a, const char *b);
/* Test builds (SBFW_DEADMAN): feed the chain stub's watchdog from key loops; stop it before an OS
 * starts. No-ops otherwise. */
void sbfw_wdt_feed(void);
void sbfw_wdt_release(void);

/* ---- Starbite OS as the boot menu (docs/STARBITE-FIRMWARE-LAYER.md section 10) ---- */
struct cat_ent;
/* Once per boot, after POST (the store is up): ages the one-shot request (armed -> seen -> expired,
 * applied -> done) and saves the record if it changed. */
void sbfw_boot_start(void);
/* The catalog index of the shell to start instead of the menu, or -1 (shell mode off, skip asked,
 * a crash to show, a boot-next pending, no such ready entry). why: the reason when -1 (for pmsg). */
int sbfw_shell_pick(int skip, const char **why);
/* The one-shot request for this start of e (SBFW_REQ_*, 0 none): read only. */
unsigned sbfw_req_for(const struct cat_ent *e);
/* Apply request req to the copy e that will start (command line edits; *xfrag for XNU, cap bytes).
 * Returns req if applied, 0 if it could not (the request is then marked refused). */
unsigned sbfw_req_apply(unsigned req, struct cat_ent *e, char *xfrag, unsigned xcap);
/* Right before the jump: the request applied this start (if any) is recorded, and the SBFW table
 * is written for the OS. why: SBFW_WHY_*. */
void sbfw_handoff(const char *key, unsigned req, unsigned why, unsigned level);
/* The table as a hand-off now would write it (key "", why fastboot) into out: 4096 or 0. */
unsigned sbfw_table_preview(uint8_t *out, unsigned cap);
