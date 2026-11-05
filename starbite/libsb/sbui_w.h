/* SPDX-License-Identifier: GPL-2.0-only
 * sbui retained widgets (sbui_page.c, sbui_w.c): the scrolling page of grouped cards (headers,
 * key/value rows, list rows, big values, monospace lines, notes, custom items), the status bar,
 * the tab bar, the dialog, the hold-to-confirm slider and the line chart.
 *
 * A page is rebuilt by its owner as often as it likes (page_begin, page_* adds, page_end); the
 * page compares the new items with what is on screen and redraws only the items whose content
 * changed (and everything below an item whose height changed). Scrolling moves the back buffer's
 * rows and draws only the rows that come into view.
 */
#pragma once
#include "sbui.h"

/* ---- page ------------------------------------------------------------------------------------ */

enum { PI_HDR, PI_KV, PI_MONO, PI_BIG, PI_ROW, PI_NOTE, PI_GAP, PI_CUSTOM, PI_BUTTON };

#define R_DIS 1U        /* greyed, not selectable */
#define R_CHECK 2U      /* switch at the right */
#define R_ON 4U         /* switch on */
#define R_WARN 8U       /* title in orange (writes or risk) */
#define R_RED 16U       /* title in red */
#define R_NOCHEV 32U    /* no chevron */
#define R_VALUE 64U     /* ROW: v is a right-aligned value, not a subtitle */

struct pitem;
typedef void (*pi_draw_fn)(const struct pitem *it, int x, int y, int w, int focused);

struct pitem {
    uint8_t kind, first, last, lines;
    uint16_t flags;
    int16_t row;                    /* index among PI_ROW / PI_BUTTON items, else -1 */
    uint32_t color;
    int y, h;                       /* layout, content coordinates */
    uint32_t hash;
    pi_draw_fn draw;
    uintptr_t arg;
    char k[72];
    char v[176];
};

#define PAGE_MAX 1400

struct page {
    struct pitem *build, *shown;    /* PAGE_MAX each, from a pool (nested screens: LIFO) */
    int n, nshown, nrows;
    int total, scroll, focus;       /* focus: row index or -1 */
    int vy0, vy1;                   /* viewport, panel rows */
    int drawn;                      /* the viewport shows this page */
    int pressed;                    /* row shown pressed, or -1 */
    int fling_v;                    /* px/s, 0 = still */
    uint64_t fling_t;
    int slot;
};

/* Pages come from a pool of 8 (one per nesting level): page_free in reverse order. */
void page_init(struct page *p, int vy0, int vy1);
void page_free(struct page *p);
void page_begin(struct page *p);
void page_hdr(struct page *p, const char *t);
void page_kv(struct page *p, const char *k, const char *v, uint32_t c);
void page_mono(struct page *p, const char *t, uint32_t c);
void page_big(struct page *p, const char *caption, const char *v, uint32_t c);
int page_row(struct page *p, const char *t, const char *s, uint32_t flags);
int page_button(struct page *p, const char *t, uint32_t c);
void page_note(struct page *p, const char *t, uint32_t c);
void page_gap(struct page *p, int h);
void page_custom(struct page *p, int h, pi_draw_fn draw, uintptr_t arg, uint32_t hash);
/* Lay out and draw: everything if the page was not on screen, else only what changed. */
void page_end(struct page *p);
void page_redraw(struct page *p);
void page_set_scroll(struct page *p, int s);
int page_max_scroll(const struct page *p);
void page_set_focus(struct page *p, int row);
/* The row (PI_ROW / PI_BUTTON index) at a panel point, or -1; *item = the item index. */
int page_hit(const struct page *p, int x, int y, int *item);
int page_next_row(const struct page *p, int from, int dir);   /* skips R_DIS */
void page_keep_visible(struct page *p, int row);
const struct pitem *page_row_item(const struct page *p, int row);
/* Touch press feedback on a row (0: none). */
void page_press(struct page *p, int row, int on);
/* Drag / fling / key scrolling for a page: returns 1 if the event was used. */
int page_scroll_event(struct page *p, const struct ui_ev *e);
/* Advance an active fling by one frame; 1 while it moves. */
int page_fling_step(struct page *p);

/* ---- chrome --------------------------------------------------------------------------------- */

#define UI_STATUS_H 110
#define UI_HDR_Y 110
#define UI_HDR_H 236
#define UI_VIEW_Y0 (UI_HDR_Y + UI_HDR_H)
#define UI_TAB_H 236
#define UI_TAB_Y (UI_H - UI_TAB_H)
#define UI_VIEW_Y1 UI_TAB_Y

struct ui_status {
    char left[24], mid[48], right[24];
    int bat;                        /* % or -1 */
    int charging;
    uint32_t mid_c;
};
/* Draws only what differs from the last call (force: everything). */
void ui_status_draw(const struct ui_status *s, int force);
/* Header: large title, optional back label ("" none), optional right note. */
void ui_header_draw(const char *title, const char *back, const char *note, uint32_t note_c);
int ui_header_back_hit(int x, int y);
void ui_tabbar_draw(const char *const *labels, int n, int active);
int ui_tabbar_hit(int n, int x, int y);

/* The armed-mode guard: while px > 0 nothing is drawn within px of the panel edge, where the
 * red border lives (ui_border_draw). */
void ui_guard(int px);
void ui_border_draw(uint32_t c, int px);

/* ---- modal widgets --------------------------------------------------------------------------- */

/* Two buttons; 1 for OK. Volume moves, Power picks, Bixby cancels; tap a button. */
int ui_dialog(const char *title, const char *l1, const char *l2, const char *ok, uint32_t ok_c);
/* A sheet with lines of key/value text (the change card) and a hold-to-confirm slider:
 * 1 confirmed, 0 cancelled. Touch: drag the knob to the end. Keys: hold Volume Up 1.5 s. */
int ui_confirm_slide(const char *title, const char *const *lines, int nlines, const char *slide_label,
                     uint32_t accent);
/* Hold Power for ms with a progress ring: 1 held to the end, 0 released early or cancelled. */
int ui_hold_power(const char *title, const char *hint, unsigned ms, uint32_t accent);
/* A one-line toast at the bottom for ms (blocking). */
void ui_toast(const char *t, uint32_t c, unsigned ms);
/* Save / restore the whole back buffer around a modal (one spare buffer). */
void ui_save_screen(void);
void ui_restore_screen(void);

/* ---- chart ----------------------------------------------------------------------------------- */

#define CHART_N 240
struct ui_chart {
    int32_t v[CHART_N];
    int n, head;
};
void chart_push(struct ui_chart *c, int32_t v);
/* Line chart of the samples in a rect: auto range with a zero line when it crosses 0. */
void chart_draw(const struct ui_chart *c, int x, int y, int w, int h, uint32_t line, uint32_t bg,
                const char *unit);
