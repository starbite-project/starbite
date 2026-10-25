/* SPDX-License-Identifier: GPL-2.0-only
 * Based on ios-a5xelte shim/herolte/fbtext.h (wardevsa, herolte branch). */
#pragma once
#include <stdint.h>

/* S-Boot scanout buffer (uniLoader board-starlte.c / starlte_defconfig). */
#define STAR_FB_BASE 0xcc000000UL
#define STAR_FB_WIDTH 1440U
#define STAR_FB_HEIGHT 2960U

/* Let CPU writes reach the panel (DECON_F HW trigger), returns old value. */
uint32_t star_fb_enable(void);
void star_fb_fill(uint32_t pixel);
void star_fb_row_base(unsigned row);
void star_fb_text(unsigned row, const char *s);
void star_fb_hex(unsigned row, const char *label, uint64_t value);
