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
