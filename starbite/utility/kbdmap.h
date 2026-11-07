/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS keyboard layouts (kbdmap.c): which HID key presses type a character on a computer set to
 * the US, Finnish (Mac) or Finnish (Windows / Linux) layout. Pure C: the host test checks it. */
#pragma once
#include <stdint.h>

enum { KL_US, KL_FI_MAC, KL_FI_PC, KL_N };
extern const char *const kbd_layout_name[KL_N];     /* "US", "Finnish (Mac)", "Finnish (PC)" */
extern const char *const kbd_layout_key[KL_N];      /* "us", "fi-mac", "fi-pc" (the settings file) */

#define KM_SHIFT 0x02U
#define KM_ALTGR 0x40U                  /* right Alt: AltGr on a PC, Option on a Mac */

struct kstroke { uint8_t mod, key; };

/* The key presses for one Unicode code point on that layout: 1, or 2 for a dead key (the accent, then
 * Space), 0 when the layout cannot type it. */
int kbd_char(int layout, uint32_t cp, struct kstroke s[2]);
/* One UTF-8 character at s: its bytes (1..3; a broken sequence is one byte, U+FFFD). */
unsigned kbd_utf8(const char *s, uint32_t *cp);
/* The layout named k ("us", "fi-mac", "fi-pc", case-insensitive), -1 if none. */
int kbd_layout_find(const char *k, unsigned n);
