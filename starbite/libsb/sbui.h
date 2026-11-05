/* SPDX-License-Identifier: GPL-2.0-only
 * sbui, libsb's UI toolkit (docs/ENGINEER-OS.md section 5).
 *
 * Drawing goes into a write-back back buffer (1440 x 2960 XRGB8888, in the arena); every draw
 * call marks its rectangle dirty, and ui_present() copies only the dirty rectangles to the
 * write-combined framebuffer at 0xcc000000. Nothing is ever read back from the panel. Fonts and
 * icons are menu v7's (the SB7ASSET container: fonts rasterised by FreeType at exact sizes, A8
 * coverage, kerning), embedded in the image so they do not depend on what the loader left in RAM.
 * Colours are 0xAARRGGBB straight alpha, blended over what the back buffer holds. Text is UTF-8;
 * the loader font's one-byte codes 0x80..0x8d (catalog strings, ui_glyphs.h) are understood too.
 * Visual language: menu v7's palette and type, with flat cards instead of glass.
 */
#pragma once
#include <stdint.h>
#include "sb.h"

#define UI_W 1440
#define UI_H 2960

/* ---- palette (menu v7: shim/starlte/menu7.h M7_*) ------------------------------------------ */
#define C_BG      0xff000000U       /* page background */
#define C_CARD    0xff1c1c1eU       /* grouped card */
#define C_CARD2   0xff2c2c2eU       /* pressed / focused row, keys, buttons */
#define C_CARD3   0xff3a3a3cU
#define C_BAR     0xf2121214U       /* tab bar */
#define C_L1      0xffffffffU       /* label */
#define C_L2      0x9eebebf5U       /* secondary */
#define C_L3      0x57ebebf5U       /* tertiary */
#define C_SEP     0x1cffffffU       /* separator */
#define C_STAR    0xff6fb3ffU       /* the starbite star: accent, focus */
#define C_BLUE    0xff0a84ffU
#define C_GREEN   0xff30d158U
#define C_ORANGE  0xffff9f0aU
#define C_RED     0xffff453aU
#define C_YELLOW  0xffffd60aU
#define C_TAG_EXP_FG 0xffffb340U
#define C_TAG_OK_FG  0xff5ee07fU
#define C_TAG_DEF_FG 0xffa9d2ffU

/* ---- fonts (roles -> SB7 font ids, sb7.h) ---------------------------------------------------- */
#define F_TITLE   4                 /* Inter Tight 700 92: page title */
#define F_TITLE_S 5                 /* Inter Tight 700 70: dialog title */
#define F_BIG     3                 /* Inter Tight 700 100: big values */
#define F_TILE    8                 /* Inter Tight 600 54: dashboard tile values */
#define F_STATUS  9                 /* Inter Tight 600 46: status bar */
#define F_ROW     18                /* Inter 400 50: rows, key/value */
#define F_ROW_M   13                /* Inter 500 58: emphasised rows */
#define F_DETAIL  22                /* Inter 400 42: row detail */
#define F_SMALL   24                /* Inter 400 38: hints */
#define F_HEAD    17                /* Inter 600 33: section headers, tab labels, tags */
#define F_BTN     15                /* Inter 600 56: buttons */
#define F_SEG     16                /* Inter 600 46: segmented, keypad */
#define F_MONO    29                /* JetBrains Mono 500 34: logs, hex */
#define F_MONO_L  27                /* JetBrains Mono 500 40: values */

/* UTF-8 symbols the fonts carry */
#define U_DEG "\xc2\xb0"
#define U_DOT "\xc2\xb7"
#define U_ELL "\xe2\x80\xa6"
#define U_UP "\xe2\x86\x91"
#define U_DOWN "\xe2\x86\x93"
#define U_LEFT "\xe2\x86\x90"
#define U_RIGHT "\xe2\x86\x92"
#define U_MICRO "\xc2\xb5"
#define U_PM "\xc2\xb1"
#define U_TIMES "\xc3\x97"
#define U_CHECK "\xe2\x9c\x93"
#define U_SQUARE "\xe2\x96\xa0"
#define U_TRI_UP "\xe2\x96\xb2"
#define U_TRI_DOWN "\xe2\x96\xbc"
#define U_DASH "\xe2\x80\x93"

/* ---- canvas ---------------------------------------------------------------------------------- */

struct ui_rect {
    int x, y, w, h;
};

/* Fonts from the embedded container, the back buffer from the arena: 1 ready. */
int ui_init(void);
const char *ui_init_why(void);
uint32_t *ui_back(void);
/* Clip every draw call to this rectangle (default: the panel). */
void ui_clip(int x, int y, int w, int h);
void ui_noclip(void);
struct ui_rect ui_get_clip(void);
void ui_set_clip(struct ui_rect r);

void ui_dirty(int x, int y, int w, int h);
void ui_dirty_all(void);
/* Copy the dirty rectangles to the panel; returns the pixels copied. */
uint64_t ui_present(void);
/* Move rows [y0, y1) of the back buffer by dy (scrolling): the rows that come into view are left
 * as they were and must be drawn by the caller. */
void ui_scroll_rows(int y0, int y1, int dy);

/* Frame statistics for the About and Dashboard pages. */
struct ui_stats {
    uint64_t frames, px_total, px_last, us_last, us_worst, us_total;
};
extern struct ui_stats ui_stats;

/* ---- drawing --------------------------------------------------------------------------------- */

void ui_fill(int x, int y, int w, int h, uint32_t c);
void ui_round(int x, int y, int w, int h, int r, uint32_t c);
/* Corners: bit 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right rounded. */
void ui_round_c(int x, int y, int w, int h, int r, unsigned corners, uint32_t c);
void ui_ring(int cx, int cy, int r, int width, uint32_t c);
void ui_disc(int cx, int cy, int r, uint32_t c);
void ui_line(int x0, int y0, int x1, int y1, int width, uint32_t c);
void ui_frame(int x, int y, int w, int h, int t, uint32_t c);

/* Text with its baseline at y; returns the x after the last glyph. */
int ui_text(int font, int x, int y, const char *s, uint32_t c);
int ui_text_r(int font, int right, int y, const char *s, uint32_t c);
int ui_text_c(int font, int cx, int y, const char *s, uint32_t c);
int ui_text_w(int font, const char *s);
/* The longest prefix (bytes) of s that fits in w px. */
unsigned ui_text_fit(int font, const char *s, int w);
/* Draw at most w px of s, ending with an ellipsis when cut. */
int ui_text_cut(int font, int x, int y, const char *s, int w, uint32_t c);
int ui_ascent(int font);
int ui_px(int font);
int ui_has_font(int font);

/* An SB7 image (sb7.h SB7_I_*), its nominal box at (x, y) w x h (scaled only when the size
 * differs), painted in c for A8 images; ARGB images keep their colours. 0 if missing. */
int ui_icon(unsigned id, int x, int y, uint32_t c);
int ui_icon_size(unsigned id, int *w, int *h);     /* the nominal box */

uint32_t ui_mix(uint32_t fg, uint32_t bg, unsigned a);

/* ---- input ----------------------------------------------------------------------------------
 * Keys: Volume moves (auto-repeat while held), Power selects (on release), Power held
 * UI_POWER_LONG_MS goes back, short Bixby goes back, long Bixby goes home. Touch: tap, drag
 * (scroll), fling, sideways swipe. */
#define UI_POWER_LONG_MS 600U
enum {
    EV_NONE, EV_UP, EV_DOWN, EV_SELECT, EV_BACK, EV_HOME,
    EV_TAP, EV_PRESS, EV_DRAG, EV_RELEASE, EV_SWIPE_L, EV_SWIPE_R, EV_TOUCH_UP
};
struct ui_ev {
    int type, x, y, d;              /* EV_DRAG: d = dy since the last event; EV_RELEASE: d = px/s */
    int x0, y0;                     /* where the touch went down */
};
/* Next event; EV_NONE after timeout_ms (0: poll once). Keys and touch are polled every 4 ms. */
void ui_poll(struct ui_ev *e, unsigned timeout_ms);
/* Drop every held key and touch: the next press is a new one. */
void ui_input_reset(void);
/* Raw keys (sb_keys) for hold gestures. */
unsigned ui_keys(void);
/* ms since the last key or touch event (auto-disarm, idle). */
uint64_t ui_idle_ms(void);
/* Called once per ui_poll loop turn (the armed-mode timer, live status bar). */
extern void (*ui_tick)(void);
