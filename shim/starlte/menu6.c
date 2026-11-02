/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v6 (Starbite 2, phase 2): the whole screen, drawn at run time from the catalog.
 *
 *   header      starbite wordmark, model                      (menu sprite 0, from the intro)
 *   tabs        Apple · Microsoft · Android · Linux · Starbite · Other · Lab (empty ones hidden,
 *               except the first four and Lab)
 *   list        one card per catalog entry of the category: icon, name, version line (or why it
 *               cannot start), tag chip, DEFAULT chip
 *   bar         Recovery · Download · Lab · Power off
 *   status      what happened (a failed start, a countdown)
 *   hints, footer
 *
 * Keys: Volume moves (down past the last card into the bar), Power starts / runs, Bixby switches
 * category, Bixby held opens Lab (EngineerMode), both volume keys held power off. Touch: tap a tab;
 * tap a card or button to select it, tap it again to start; swipe left / right for the next /
 * previous category; long-press a card for its options (default entry, auto-start).
 *
 * Frames are composed in CANVAS (write-back RAM, mmu.c) with ui.c drawing into it, and the rows
 * that changed are copied to the panel. Labels come from the catalog, icons are menu sprites
 * 96 + ICON_* (V6=1 tools/brand/menu.mjs), text is the Outfit atlas (tools/brand/em-font.py).
 * No pointer tables: everything is filled at run time.
 */
#include <stdint.h>
#include "menu.h"
#include "fbtext.h"
#include "hw.h"
#include "ui.h"
#include "catalog.h"
#include "store.h"

#define W UI_W
#define H UI_H
/* After anim.c's buffers (0x84000000..0x85c00000) and menu.c's MENU_SAVE (0x86000000, 16.3 MiB),
 * below the menu assets at 0x89000000. */
#define CANVAS ((uint32_t *)0x87100000UL)

#define X0 64
#define CW (W - 2 * X0)
#define TAB_Y 262
#define TAB_H 84
#define LIST_Y 400
#define CARD_H 216
#define CARD_STRIDE 240
#define LIST_ROWS 7
#define LIST_END (LIST_Y + LIST_ROWS * CARD_STRIDE)
#define BAR_Y 2120
#define BAR_H 176
#define BAR_GAP 32
#define BAR_W ((CW - 3 * BAR_GAP) / 4)
#define STATUS_Y 2400
#define HINT_Y 2700
#define FOOTER_Y 2790

#define BG 0xff000000U
#define INK 0xffeef1f6U
#define DIM 0xff8a93a3U
#define FAINT 0xff4a5160U
#define LINE 0xff2c313bU
#define CARD 0xff0e131cU
#define CARD_SEL 0xff111c2aU
#define ACCENT 0xff6fb3ffU
#define AMBER 0xffffb547U
#define GREEN 0xff3ddc84U
#define PURPLE 0xffb48cffU
#define RED 0xffff6b6bU

#define ICON_BASE 96U
#define ICON_RECOVERY 16U
#define ICON_DOWNLOAD 17U
#define ICON_LAB 18U
#define ICON_POWER 19U

#define NCAT 7U
#define EV_TAP 0x100U
#define EV_LONG 0x200U
#define EV_LEFT 0x400U
#define EV_RIGHT 0x800U
#define EV_BIXBY_HOLD 0x1000U
#define EV_VOL_HOLD 0x2000U
#define HOLD_MS 700U
#define LONG_MS 600U

int star_sprite_to(unsigned id, uint32_t *dst, int x, int y, int at, unsigned fade);
int em_main(const struct star_info *in, char *cmdline, unsigned len);

static struct {
    unsigned tab;                   /* index into tabs[] */
    int zone;                       /* 0 the list, 1 the bar */
    unsigned sel[NCAT];             /* selected card per tab */
    unsigned bar;
    unsigned top;                   /* first card shown */
    int inited;
} M;

static unsigned tabs[NCAT], ntabs;        /* categories shown, in order */
static int tab_x[NCAT], tab_w[NCAT];
static unsigned list[CAT_MAX], nlist;     /* catalog indices of the current tab */
static char status1[96], status2[96];
static uint32_t status_c;

/* ---- text helpers -------------------------------------------------------------------------- */

static void scopy(char *d, unsigned cap, const char *s)
{
    unsigned k = 0;
    for (; s && s[k] && k + 1 < cap; ++k)
        d[k] = s[k];
    d[k] = 0;
}

static void scat(char *d, unsigned cap, const char *s)
{
    unsigned n = 0;
    while (n + 1 < cap && d[n])
        ++n;
    scopy(d + n, cap - n, s);
}

/* "NO KERNEL" -> "No kernel" (the loader's reasons are upper case for fbtext). */
static void sentence(char *d, unsigned cap, const char *s)
{
    scopy(d, cap, s);
    for (unsigned k = 1; d[k]; ++k)
        if (d[k] >= 'A' && d[k] <= 'Z')
            d[k] = (char)(d[k] + 32);
}

static int key_eq(const char *a, const char *b)
{
    unsigned k = 0;
    while (k < 23 && a[k] && a[k] == b[k])
        ++k;
    return k == 23 || a[k] == b[k];
}

static const char *cat_name(unsigned c)
{
    switch (c) {
    case CAT_APPLE: return "Apple";
    case CAT_MICROSOFT: return "Microsoft";
    case CAT_ANDROID: return "Android";
    case CAT_LINUX: return "Linux";
    case CAT_STARBITE: return "Starbite";
    case CAT_OTHER: return "Other";
    default: return "Lab";
    }
}

static const char *tag_name(unsigned t, uint32_t *c)
{
    switch (t) {
    case TAG_DAILY: *c = GREEN; return "DAILY";
    case TAG_BETA: *c = ACCENT; return "BETA";
    case TAG_EXPERIMENTAL: *c = AMBER; return "EXPERIMENTAL";
    case TAG_LAB: *c = PURPLE; return "LAB";
    case TAG_STABLE: *c = GREEN; return "STABLE";
    default: *c = DIM; return 0;
    }
}

static unsigned icon_of(const struct cat_ent *e)
{
    if (e->icon)
        return e->icon;
    switch (e->category) {
    case CAT_APPLE: return 1;
    case CAT_MICROSOFT: return 2;
    case CAT_ANDROID: return 3;
    case CAT_LINUX: return 4;
    case CAT_STARBITE: return 5;
    case CAT_LAB: return 7;
    default: return 6;
    }
}

/* ---- the catalog by tab -------------------------------------------------------------------- */

static unsigned count_in(unsigned c)
{
    unsigned n = 0;
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *e = cat_get(i);
        n += e->category == c && !(e->flags & CF_HIDDEN);
    }
    return n;
}

static void make_tabs(void)
{
    ntabs = 0;
    for (unsigned c = 0; c < NCAT; ++c)
        if (c <= CAT_LINUX || c == CAT_LAB || count_in(c))
            tabs[ntabs++] = c;
}

/* nlist / list[] for tab t, by the entries' order field. */
static void make_list(unsigned t)
{
    nlist = 0;
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *e = cat_get(i);
        if (e->category != tabs[t] || (e->flags & CF_HIDDEN))
            continue;
        unsigned k = nlist++;
        while (k && cat_get(list[k - 1])->order > e->order) {
            list[k] = list[k - 1];
            --k;
        }
        list[k] = i;
    }
    if (M.sel[t] >= nlist)
        M.sel[t] = 0;
}

static unsigned tab_of(unsigned category)
{
    for (unsigned t = 0; t < ntabs; ++t)
        if (tabs[t] == category)
            return t;
    return 0;
}

/* Focus catalog entry idx (its tab, its card). */
static void focus_entry(int idx)
{
    const struct cat_ent *e = idx >= 0 ? cat_get((unsigned)idx) : 0;
    if (!e)
        return;
    M.tab = tab_of(e->category);
    make_list(M.tab);
    for (unsigned k = 0; k < nlist; ++k)
        if (list[k] == (unsigned)idx)
            M.sel[M.tab] = k;
    M.zone = 0;
}

/* ---- drawing (into CANVAS) ------------------------------------------------------------------ */

static void present_rows(int y0, int y1)
{
    if (y0 < 0)
        y0 = 0;
    if (y1 > H)
        y1 = H;
    volatile uint64_t *fb = (volatile uint64_t *)STAR_FB_BASE;
    const uint64_t *s = (const uint64_t *)CANVAS;
    for (uint64_t i = (uint64_t)y0 * W / 2; i < (uint64_t)y1 * W / 2; ++i)
        fb[i] = s[i];
    __asm__ volatile("dsb st" : : : "memory");
}

static void chip(int right, int y, const char *s, uint32_t c)
{
    int w = ui_text_w(F_CHIP, s) + 44;
    ui_round(right - w, y, w, 52, 26, (c & 0x00ffffffU) | 0x2e000000U);
    ui_text(F_CHIP, right - w + 22, y + 36, s, c, UI_BEHIND);
}

static int chip_w(const char *s)
{
    return ui_text_w(F_CHIP, s) + 44;
}

static void draw_tabs(void)
{
    ui_fill(0, TAB_Y - 8, W, TAB_H + 16, BG);
    int x = X0;
    for (unsigned t = 0; t < ntabs; ++t) {
        const char *s = cat_name(tabs[t]);
        int w = ui_text_w(F_META, s) + 68;
        tab_x[t] = x;
        tab_w[t] = w;
        int on = t == M.tab;
        if (on)
            ui_round(x, TAB_Y, w, TAB_H, TAB_H / 2, (ACCENT & 0x00ffffffU) | 0x2a000000U);
        ui_text(F_META, x + 34, TAB_Y + 56, s, on ? ACCENT : DIM, UI_BEHIND);
        x += w + 10;
    }
}

/* st: 0 normal, 1 selected, 2 starting, 3 selected while the bar has focus */
static void draw_card(unsigned slot, unsigned idx, int st)
{
    const struct cat_ent *e = cat_get(idx);
    const char *why;
    int ready = star_entry_ready(idx, &why);
    int y = LIST_Y + (int)slot * CARD_STRIDE;
    char b[96];
    ui_fill(X0 - 4, y - 4, CW + 8, CARD_H + 8, BG);
    int sel = st == 1 || st == 2;
    ui_round(X0, y, CW, CARD_H, 40, sel ? ACCENT : st == 3 ? 0xff3a4c66U : LINE);
    ui_round(X0 + 3, y + 3, CW - 6, CARD_H - 6, 37, sel || st == 3 ? CARD_SEL : CARD);
    (void)star_sprite_to(ICON_BASE + icon_of(e), CANVAS, X0 + 48, y + (CARD_H - 112) / 2, 1, ready ? 256U : 80U);
    ui_text(F_NAME, X0 + 200, y + 100, e->name, ready ? INK : FAINT, UI_BEHIND);
    if (st == 2)
        ui_text(F_META, X0 + 200, y + 166, "starting" UI_ELLIPSIS, ACCENT, UI_BEHIND);
    else if (!ready) {
        sentence(b, sizeof(b), why);
        scat(b, sizeof(b), " " UI_DOT " not installed");
        ui_text(F_META, X0 + 200, y + 166, b, FAINT, UI_BEHIND);
    } else
        ui_text(F_META, X0 + 200, y + 166, e->version, DIM, UI_BEHIND);
    uint32_t tc;
    const char *tag = tag_name(e->tag, &tc);
    int right = X0 + CW - 36;
    if (tag) {
        chip(right, y + 36, tag, ready ? tc : FAINT);
        right -= chip_w(tag) + 12;
    }
    int valid;
    struct star_settings *s = star_menu_settings(&valid);
    if (s->default_key[0] && key_eq(s->default_key, e->key))
        chip(right, y + 36, "DEFAULT", INK);
}

static void draw_list(void)
{
    ui_fill(0, LIST_Y - 8, W, LIST_END - LIST_Y + 8, BG);
    make_list(M.tab);
    unsigned sel = M.sel[M.tab];
    if (sel < M.top)
        M.top = sel;
    if (sel >= M.top + LIST_ROWS)
        M.top = sel - LIST_ROWS + 1;
    if (M.top + LIST_ROWS > nlist)
        M.top = nlist > LIST_ROWS ? nlist - LIST_ROWS : 0;
    if (!nlist) {
        ui_text_c(F_NAME, W / 2, LIST_Y + 300, "Nothing here yet", DIM, UI_BEHIND);
        ui_text_c(F_META, W / 2, LIST_Y + 380, "Add an entry to tools/store/catalog.toml", FAINT, UI_BEHIND);
        return;
    }
    for (unsigned k = M.top; k < nlist && k < M.top + LIST_ROWS; ++k)
        draw_card(k - M.top, list[k], k != sel ? 0 : M.zone ? 3 : 1);
    if (M.top + LIST_ROWS < nlist)
        ui_text_c(F_HINT, W / 2, LIST_END - 6, UI_DOWN " more", DIM, UI_BEHIND);
}

static void draw_bar(void)
{
    ui_fill(0, BAR_Y - 8, W, BAR_H + 16, BG);
    for (unsigned i = 0; i < 4; ++i) {
        int x = X0 + (int)i * (BAR_W + BAR_GAP);
        int on = M.zone == 1 && M.bar == i;
        ui_round(x, BAR_Y, BAR_W, BAR_H, 36, on ? ACCENT : LINE);
        ui_round(x + 3, BAR_Y + 3, BAR_W - 6, BAR_H - 6, 33, on ? CARD_SEL : CARD);
        (void)star_sprite_to(ICON_BASE + ICON_RECOVERY + i, CANVAS, x + (BAR_W - 64) / 2, BAR_Y + 30, 1, 256);
        const char *s = i == 0 ? "Recovery" : i == 1 ? "Download" : i == 2 ? "Lab" : "Power off";
        ui_text_c(F_META, x + BAR_W / 2, BAR_Y + 148, s, on ? INK : DIM, UI_BEHIND);
    }
}

static void draw_status(void)
{
    ui_fill(0, STATUS_Y - 60, W, 180, BG);
    if (status1[0])
        ui_text_c(F_META, W / 2, STATUS_Y, status1, status_c, UI_BEHIND);
    if (status2[0])
        ui_text_c(F_HINT, W / 2, STATUS_Y + 60, status2, DIM, UI_BEHIND);
}

static void draw_hints(void)
{
    ui_fill(0, HINT_Y - 50, W, 80, BG);
    ui_text_c(F_HINT, W / 2, HINT_Y,
              "Volume move  " UI_DOT "  Power start  " UI_DOT "  Bixby category  " UI_DOT "  hold Bixby Lab",
              FAINT, UI_BEHIND);
}

static void set_status(const char *a, const char *b, uint32_t c)
{
    scopy(status1, sizeof(status1), a);
    scopy(status2, sizeof(status2), b);
    status_c = c;
}

static void compose(void)
{
    ui_target(CANVAS);
    ui_clip(0, H);
    ui_fill(0, 0, W, H, BG);
    (void)star_sprite_to(0, CANVAS, 0, 0, 0, 256);     /* header + footer */
    draw_tabs();
    draw_list();
    draw_bar();
    draw_status();
    draw_hints();
    ui_target(0);
}

/* Redraw what a focus change touches and put it on the panel. */
static void refresh(int tabs_too)
{
    ui_target(CANVAS);
    if (tabs_too)
        draw_tabs();
    draw_list();
    draw_bar();
    draw_status();
    ui_target(0);
    present_rows(tabs_too ? TAB_Y - 8 : LIST_Y - 8, STATUS_Y + 130);
}

/* ---- input ----------------------------------------------------------------------------------- */

static int tp_is_down, tp_x0, tp_y0, tp_lx, tp_ly, tp_long_done;
static uint64_t tp_t0;

static int ms_since(uint64_t t0)
{
    return (int)((hw_ticks() - t0) * 1000U / hw_freq());
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/* A key (K_* bits, debounced), EV_BIXBY_HOLD / EV_VOL_HOLD, or a touch event with *x, *y. With
 * timeout_ms, 0 when nothing happened in that time. */
static unsigned input(int *x, int *y, unsigned timeout_ms)
{
    uint64_t t_in = hw_ticks();
    while (hw_keys()) {
        hw_wait_ms(20);
        if (timeout_ms && ms_since(t_in) > (int)timeout_ms)
            return 0;
    }
    for (;;) {
        unsigned k = hw_keys();
        if (k) {
            hw_wait_ms(40);
            if (hw_keys() != k)
                continue;
            if (k == K_BIXBY) {
                for (unsigned t = 40; t < HOLD_MS; t += 10) {
                    if (!(hw_keys() & K_BIXBY))
                        return K_BIXBY;
                    hw_wait_ms(10);
                }
                return EV_BIXBY_HOLD;
            }
            unsigned both = 0, now;
            while ((now = hw_keys())) {
                both = (now & (K_UP | K_DOWN)) == (K_UP | K_DOWN) ? both + 20U : 0;
                if (both >= HOLD_MS)
                    return EV_VOL_HOLD;
                hw_wait_ms(20);
            }
            return k;
        }
        int st, ex, ey;
        while (hw_tp_event(&st, &ex, &ey)) {
            if (st == 1) {
                tp_is_down = 1;
                tp_x0 = tp_lx = ex;
                tp_y0 = tp_ly = ey;
                tp_t0 = hw_ticks();
                tp_long_done = 0;
            } else if (st == 2 && tp_is_down) {
                tp_lx = ex;
                tp_ly = ey;
            } else if (st == 3 && tp_is_down) {
                tp_is_down = 0;
                if (tp_long_done)
                    continue;
                int dx = ex - tp_x0, dy = ey - tp_y0;
                if (iabs(dx) < 80 && iabs(dy) < 80) {
                    *x = ex;
                    *y = ey;
                    return EV_TAP;
                }
                if (iabs(dx) > 220 && iabs(dx) > 2 * iabs(dy))
                    return dx < 0 ? EV_LEFT : EV_RIGHT;
            }
        }
        if (tp_is_down && !tp_long_done && ms_since(tp_t0) > (int)LONG_MS &&
            iabs(tp_lx - tp_x0) < 60 && iabs(tp_ly - tp_y0) < 60) {
            tp_long_done = 1;
            *x = tp_x0;
            *y = tp_y0;
            return EV_LONG;
        }
        if (timeout_ms && ms_since(t_in) > (int)timeout_ms)
            return 0;
        hw_wait_ms(10);
    }
}

/* What is under (x, y): 1 tab (*i), 2 card (*i = list position), 3 bar button, 0 nothing. */
static int hit(int x, int y, unsigned *i)
{
    if (y >= TAB_Y && y < TAB_Y + TAB_H)
        for (unsigned t = 0; t < ntabs; ++t)
            if (x >= tab_x[t] && x < tab_x[t] + tab_w[t]) {
                *i = t;
                return 1;
            }
    if (x >= X0 && x < X0 + CW && y >= LIST_Y && y < LIST_END) {
        unsigned slot = (unsigned)(y - LIST_Y) / CARD_STRIDE;
        if ((y - LIST_Y) % CARD_STRIDE < CARD_H && M.top + slot < nlist) {
            *i = M.top + slot;
            return 2;
        }
    }
    if (y >= BAR_Y && y < BAR_Y + BAR_H && x >= X0)
        for (unsigned b = 0; b < 4; ++b) {
            int bx = X0 + (int)b * (BAR_W + BAR_GAP);
            if (x >= bx && x < bx + BAR_W) {
                *i = b;
                return 3;
            }
        }
    return 0;
}

/* ---- the options sheet (long-press a card) ---------------------------------------------------- */

#define SHEET_Y 1460
#define SHEET_ROW 132
#define SHEET_H (220 + 4 * SHEET_ROW + 24)

static void sheet_draw(unsigned idx, unsigned sel)
{
    const struct cat_ent *e = cat_get(idx);
    struct star_settings *s = star_menu_settings(0);
    int is_def = s->default_key[0] && key_eq(s->default_key, e->key);
    char b[96];
    ui_target(CANVAS);
    ui_round(X0, SHEET_Y, CW, SHEET_H, 48, 0xff161b25U);
    ui_text(F_NAME, X0 + 56, SHEET_Y + 110, e->name, INK, UI_BEHIND);
    scopy(b, sizeof(b), e->root[0] ? e->root : e->version);
    ui_text(F_META, X0 + 56, SHEET_Y + 172, b, DIM, UI_BEHIND);
    for (unsigned r = 0; r < 4; ++r) {
        int y = SHEET_Y + 220 + (int)r * SHEET_ROW;
        ui_round(X0 + 32, y, CW - 64, SHEET_ROW - 16, 30, r == sel ? CARD_SEL : 0xff161b25U);
        if (r == sel)
            ui_round(X0 + 32, y, 8, SHEET_ROW - 16, 4, ACCENT);
        const char *t = r == 0 ? "Start now" : r == 1 ? (is_def ? "Default entry " UI_CHECK : "Make default") :
                        r == 2 ? "Start the default automatically" : "Close";
        ui_text(F_META, X0 + 80, y + 72, t, r == sel ? INK : DIM, UI_BEHIND);
        if (r == 2) {
            b[0] = 0;
            if (!s->timeout_s)
                scopy(b, sizeof(b), "off");
            else {
                char n[4] = { (char)('0' + s->timeout_s / 10), (char)('0' + s->timeout_s % 10), 0, 0 };
                scopy(b, sizeof(b), n[0] == '0' ? n + 1 : n);
                scat(b, sizeof(b), " s");
            }
            ui_text_r(F_META, X0 + CW - 80, y + 72, b, ACCENT, UI_BEHIND);
        }
    }
    ui_target(0);
    present_rows(SHEET_Y, SHEET_Y + SHEET_H);
}

/* Everything under the header at 40 %, behind the sheet. */
static void dim_canvas(void)
{
    for (uint64_t i = (uint64_t)(TAB_Y - 8) * W; i < (uint64_t)H * W; ++i) {
        uint32_t p = CANVAS[i];
        CANVAS[i] = 0xff000000U | ((((p >> 16) & 255U) * 2 / 5) << 16) | ((((p >> 8) & 255U) * 2 / 5) << 8) |
                    ((p & 255U) * 2 / 5);
    }
    present_rows(TAB_Y - 8, H);
}

/* 1: start the entry. */
static int sheet(unsigned idx)
{
    unsigned sel = 0;
    const struct cat_ent *e = cat_get(idx);
    dim_canvas();
    for (;;) {
        sheet_draw(idx, sel);
        int x = 0, y = 0;
        unsigned ev = input(&x, &y, 0);
        if (ev == EV_TAP) {
            if (y < SHEET_Y || y >= SHEET_Y + SHEET_H)
                ev = K_BIXBY;                               /* outside: close */
            else if (y >= SHEET_Y + 220) {
                unsigned r = (unsigned)(y - SHEET_Y - 220) / SHEET_ROW;
                if (r != sel) {
                    sel = r;
                    continue;
                }
                ev = K_POWER;
            } else
                continue;
        }
        if (ev & (K_UP | K_DOWN)) {
            sel = (sel + ((ev & K_UP) ? 3U : 1U)) % 4U;
            continue;
        }
        if (ev == K_BIXBY || ev == EV_BIXBY_HOLD)
            return 0;
        if (!(ev & K_POWER))
            continue;
        struct star_settings *s = star_menu_settings(0);
        if (sel == 0)
            return star_entry_ready(idx, 0);
        if (sel == 1) {
            if (s->default_key[0] && key_eq(s->default_key, e->key))
                s->default_key[0] = 0;
            else
                scopy(s->default_key, sizeof(s->default_key), e->key);
            if (star_menu_settings_save())
                set_status("Could not save the settings", "", RED);
        } else if (sel == 2) {
            s->timeout_s = s->timeout_s == 0 ? 3 : s->timeout_s == 3 ? 5 : s->timeout_s == 5 ? 10 : 0;
            if (star_menu_settings_save())
                set_status("Could not save the settings", "", RED);
        } else {
            return 0;
        }
    }
}

/* ---- starting ---------------------------------------------------------------------------------- */

/* The card says "starting"; the bottom of the screen is cleared for the loader's status lines. */
static int go(int idx)
{
    focus_entry(idx);
    set_status("", "", DIM);
    compose();
    ui_target(CANVAS);
    make_list(M.tab);
    unsigned k = M.sel[M.tab];
    if (k >= M.top && k < M.top + LIST_ROWS)
        draw_card(k - M.top, (unsigned)idx, 2);
    ui_fill(0, BAR_Y - 8, W, FOOTER_Y - BAR_Y, BG);
    ui_target(0);
    present_rows(0, H);
    star_menu_remember(idx);
    star_fb_row_base((BAR_Y - 200U) / 60U);                 /* fbtext: TOP 200, 60 px rows */
    hw_tp_clear();
    return idx + 1;
}

static void lab(const struct star_info *in, char *cmdline, unsigned len, int *boot)
{
    ui_target(0);
    *boot = em_main(in, cmdline, len);
    compose();
    present_rows(0, H);
}

int menu_v6(const struct star_info *in, char *cmdline, unsigned len)
{
    const char *fname, *fwhy;
    ui_init();
    if (!M.inited) {
        star_menu_begin();
        make_tabs();
        M.inited = 1;
        M.tab = 0;
        M.zone = 0;
        /* focus: the last choice, else the default, else the first entry that can start */
        struct star_settings *s = star_menu_settings(0);
        int f = star_menu_ready_key(s->last_key);
        if (f < 0)
            f = star_menu_ready_key(s->default_key);
        for (unsigned i = 0; f < 0 && i < cat_count(); ++i)
            if (!(cat_get(i)->flags & CF_HIDDEN) && star_entry_ready(i, 0))
                f = (int)i;
        focus_entry(f);
        hw_tp_start();
        int next = star_menu_next();
        if (next >= 0)
            return go(next);
        set_status("", "", DIM);
        compose();
        /* the intro: tabs, cards, bar and hints rise in over the header the splash left */
        int rc[4 * 16], n = 0;
        rc[n++] = 0; rc[n++] = TAB_Y - 8; rc[n++] = W; rc[n++] = TAB_H + 16;
        make_list(M.tab);
        for (unsigned k = 0; k < nlist && k < LIST_ROWS; ++k) {
            rc[n++] = X0 - 4; rc[n++] = LIST_Y + (int)k * CARD_STRIDE - 4; rc[n++] = CW + 8; rc[n++] = CARD_H + 8;
        }
        if (!nlist) {
            rc[n++] = 0; rc[n++] = LIST_Y; rc[n++] = W; rc[n++] = 420;
        }
        rc[n++] = 0; rc[n++] = BAR_Y - 8; rc[n++] = W; rc[n++] = BAR_H + 16;
        rc[n++] = 0; rc[n++] = HINT_Y - 50; rc[n++] = W; rc[n++] = 80;
        star_reveal(CANVAS, rc, (unsigned)(n / 4));
        /* a default entry with a timeout starts unless something is pressed or touched */
        int dflt = s->timeout_s ? star_menu_ready_key(s->default_key) : -1;
        if (dflt >= 0) {
            char b[96];
            focus_entry(dflt);
            for (unsigned left = s->timeout_s; left; --left) {
                char n2[3] = { (char)('0' + left / 10), (char)('0' + left % 10), 0 };
                scopy(b, sizeof(b), cat_get((unsigned)dflt)->name);
                scat(b, sizeof(b), " starts in ");
                scat(b, sizeof(b), n2[0] == '0' ? n2 + 1 : n2);
                scat(b, sizeof(b), " s");
                set_status(b, "Press any key or touch the screen to stay here", INK);
                refresh(1);
                int x, y;
                if (input(&x, &y, 1000)) {
                    dflt = -1;
                    break;
                }
            }
            set_status("", "", DIM);
            if (dflt >= 0)
                return go(dflt);
            refresh(1);
        }
    } else if (star_menu_failure(&fname, &fwhy)) {
        /* back from a start that failed: the menu as it was, and why */
        char b[96], w2[96];
        scopy(b, sizeof(b), "Could not start ");
        scat(b, sizeof(b), fname);
        sentence(w2, sizeof(w2), fwhy ? fwhy : "UNKNOWN ERROR");
        set_status(b, w2, RED);
        compose();
        present_rows(0, H);
    }

    for (;;) {
        int x = 0, y = 0;
        unsigned ev = input(&x, &y, 0);
        unsigned i = 0;
        int boot = 0;
        if (ev == EV_TAP || ev == EV_LONG) {
            int what = hit(x, y, &i);
            if (what == 1) {
                if (i != M.tab) {
                    M.tab = i;
                    M.zone = 0;
                    M.top = 0;
                    refresh(1);
                }
                continue;
            }
            if (what == 2) {
                if (ev == EV_LONG) {
                    M.zone = 0;
                    M.sel[M.tab] = i;
                    refresh(0);
                    int start = sheet(list[i]);
                    compose();
                    present_rows(0, H);
                    if (start)
                        return go((int)list[i]);
                    continue;
                }
                if (M.zone == 0 && M.sel[M.tab] == i) {
                    ev = K_POWER;                           /* second tap starts it */
                } else {
                    M.zone = 0;
                    M.sel[M.tab] = i;
                    refresh(0);
                    continue;
                }
            } else if (what == 3) {
                if (M.zone == 1 && M.bar == i) {
                    ev = K_POWER;
                } else {
                    M.zone = 1;
                    M.bar = i;
                    refresh(0);
                    continue;
                }
            } else {
                continue;
            }
        }
        if (ev == EV_LEFT || ev == EV_RIGHT || ev == K_BIXBY) {
            M.tab = (M.tab + (ev == EV_RIGHT ? ntabs - 1U : 1U)) % ntabs;
            M.zone = 0;
            M.top = 0;
            refresh(1);
            continue;
        }
        if (ev == EV_BIXBY_HOLD) {
            lab(in, cmdline, len, &boot);
            if (boot > 0)
                return go(boot - 1);
            continue;
        }
        if (ev == EV_VOL_HOLD) {
            M.zone = 1;
            M.bar = 3;
            ev = K_POWER;
        }
        if (ev & K_POWER) {
            if (M.zone == 0) {
                make_list(M.tab);
                if (nlist && star_entry_ready(list[M.sel[M.tab]], 0))
                    return go((int)list[M.sel[M.tab]]);
                continue;
            }
            if (M.bar == 2) {
                lab(in, cmdline, len, &boot);
                if (boot > 0)
                    return go(boot - 1);
                continue;
            }
            star_fb_fill(BG);
            star_fb_row_base(22);
            if (M.bar == 3) {
                star_fb_text_c(0, "POWERING OFF", 0xffffffffU, BG);
                hw_power_off();
                star_fb_text_c(1, "STILL ON: CHARGER CONNECTED?", DIM, BG);
                star_fb_text_c(2, "REBOOTING", DIM, BG);
                hw_wait_ms(1500);
                hw_reboot(INFORM_NORMAL);
            }
            star_fb_text_c(0, M.bar == 0 ? "REBOOTING TO RECOVERY" : "REBOOTING TO DOWNLOAD MODE", 0xffffffffU, BG);
            hw_reboot(M.bar == 0 ? INFORM_RECOVERY : INFORM_DOWNLOAD);
        }
        if (ev & (K_UP | K_DOWN)) {
            make_list(M.tab);
            int down = (ev & K_DOWN) != 0;
            if (M.zone == 0) {
                if (down) {
                    if (M.sel[M.tab] + 1 < nlist)
                        ++M.sel[M.tab];
                    else {
                        M.zone = 1;
                        M.bar = 0;
                    }
                } else if (M.sel[M.tab]) {
                    --M.sel[M.tab];
                }
            } else if (down) {
                M.bar = (M.bar + 1U) % 4U;
            } else if (M.bar) {
                --M.bar;
            } else if (nlist) {
                M.zone = 0;
                M.sel[M.tab] = nlist - 1;
            }
            refresh(0);
        }
    }
}
