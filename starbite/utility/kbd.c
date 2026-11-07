/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS keyboard: a full keyboard for the computer over USB HID (hid.c), with a trackpad above it.
 * Layers: letters (the layout's own: US or Finnish), symbols, Fn (Esc, F1-F12, navigation, arrows) and a
 * numpad. Shift, Ctrl, Alt/Option and Cmd/Win are sticky: tap once for the next key, twice to lock, again
 * to release. Every finger counts (mt.c): keys work while another finger holds one; held keys repeat.
 * The trackpad: drag moves the pointer, a tap clicks, two fingers scroll, a two-finger tap right-clicks.
 * Characters are typed as the key presses that give them on the computer's layout (kbdmap.c): set the
 * same layout here (the layout button) as on the computer. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "hid.h"
#include "ut.h"
#include "usb_dwc3.h"
#include "kbdmap.h"
#include "mt.h"

#define ECHO_Y 366
#define ECHO_H 150
#define PAD_Y0 540
#define PAD_Y1 1380
#define TAB_Y 1404
#define TAB_H 128
#define KB_Y0 1556
#define ROW_H 272
#define KEY_H 252
#define UNIT 58                                   /* a row is 24 units: a character key is 2 */
#define KB_X0 ((UI_W - 24 * UNIT) / 2)
#define GAP 10

enum { L_ABC, L_SYM, L_FN, L_NUM, L_N };
enum { K_CHAR, K_KEY, K_SHIFT, K_CTRL, K_ALT, K_GUI };

struct key {
    int16_t x, y, w, h;
    uint8_t kind, usage;
    uint32_t cp, scp;                             /* K_CHAR: the character, and with Shift */
    const char *lab;                              /* K_KEY and modifiers */
};

#define KMAX 72
static struct key kb[KMAX];
static int nk, layer;
static unsigned st_shift, st_ctrl, st_alt, st_gui;   /* 0 off, 1 next key, 2 locked */
static char echo[128];
static unsigned echo_n;

/* ---- building the layers -------------------------------------------------------------------------- */
static int row_start, row_units, row_y;

static void row_begin(int r)
{
    row_start = nk;
    row_units = 0;
    row_y = KB_Y0 + r * ROW_H;
}

static void row_end(void)
{
    int shift = (24 - row_units) * UNIT / 2;      /* centre a short row */
    for (int i = row_start; i < nk; ++i)
        kb[i].x = (int16_t)(kb[i].x + shift);
}

static struct key *add(int units, int kind)
{
    struct key *k = &kb[nk < KMAX - 1 ? nk++ : nk];
    k->x = (int16_t)(KB_X0 + row_units * UNIT + GAP / 2);
    k->y = (int16_t)row_y;
    k->w = (int16_t)(units * UNIT - GAP);
    k->h = KEY_H;
    k->kind = (uint8_t)kind;
    k->usage = 0;
    k->cp = k->scp = 0;
    k->lab = "";
    row_units += units;
    return k;
}

static void gap(int units) { row_units += units; }

static void add_key(int units, uint8_t usage, const char *lab)
{
    struct key *k = add(units, K_KEY);
    k->usage = usage;
    k->lab = lab;
}

/* one 2-unit key per character of s (UTF-8), shifted characters from sh */
static void add_chars(const char *s, const char *sh)
{
    while (*s) {
        struct key *k = add(2, K_CHAR);
        s += kbd_utf8(s, &k->cp);
        if (sh && *sh)
            sh += kbd_utf8(sh, &k->scp);
        else
            k->scp = k->cp;
    }
}

static void mods_row(int r)
{
    int pc = hid_layout == KL_FI_PC;
    row_begin(r);
    add_key(2, 0x29, "esc");
    add(3, K_CTRL)->lab = "ctrl";
    add(3, K_ALT)->lab = pc ? "alt" : "opt";
    add(3, K_GUI)->lab = pc ? "win" : "cmd";
    add_key(9, 0x2c, "space");
    add_key(2, 0x50, U_LEFT);
    add_key(2, 0x4f, U_RIGHT);
    row_end();
}

static void build(void)
{
    nk = 0;
    int fi = hid_layout != KL_US;
    if (layer == L_ABC) {
        row_begin(0);
        if (fi)
            add_chars("1234567890+", hid_layout == KL_FI_MAC ? "!\"#\xe2\x82\xac%&/()=?" : "!\"#\xc2\xa4%&/()=?");
        else
            add_chars("1234567890-", "!@#$%^&*()_");
        add_key(2, 0x2a, "del");
        row_end();
        row_begin(1);
        add_key(2, 0x2b, "tab");
        add_chars(fi ? "qwertyuiop\xc3\xa5" : "qwertyuiop[", fi ? "QWERTYUIOP\xc3\x85" : "QWERTYUIOP{");
        row_end();
        row_begin(2);
        add_chars(fi ? "asdfghjkl\xc3\xb6\xc3\xa4" : "asdfghjkl;'", fi ? "ASDFGHJKL\xc3\x96\xc3\x84" : "ASDFGHJKL:\"");
        add_key(2, 0x28, "ret");
        row_end();
        row_begin(3);
        add(2, K_SHIFT)->lab = U_UP;
        add_chars(fi ? "<zxcvbnm,.-" : "zxcvbnm,./\\", fi ? ">ZXCVBNM;:_" : "ZXCVBNM<>?|");
        row_end();
        mods_row(4);
    } else if (layer == L_SYM) {
        row_begin(0);
        add_chars("1234567890", 0);
        add_key(4, 0x2a, "del");
        row_end();
        row_begin(1);
        add_chars("!@#$%&*()=+", 0);
        row_end();
        row_begin(2);
        add_chars("-_/\\|[]{}<>", 0);
        row_end();
        row_begin(3);
        add_chars(";:'\",.?`^~\xe2\x82\xac", 0);
        row_end();
        mods_row(4);
    } else if (layer == L_FN) {
        static const char *const f[12] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
        row_begin(0);
        add_key(2, 0x29, "esc");
        for (int i = 0; i < 11; ++i)
            add_key(2, (uint8_t)(0x3a + i), f[i]);
        row_end();
        row_begin(1);
        add_key(2, 0x45, f[11]);
        add_key(2, 0x46, "prt");
        add_key(2, 0x47, "scr");
        add_key(2, 0x48, "pau");
        add_key(2, 0x49, "ins");
        add_key(2, 0x4a, "hom");
        add_key(2, 0x4b, "pg" U_UP);
        add_key(2, 0x39, "caps");
        add_key(2, 0x65, "menu");
        add_key(2, 0x2b, "tab");
        add_key(4, 0x2a, "del");
        row_end();
        row_begin(2);
        add_key(2, 0x4c, "fdel");
        add_key(2, 0x4d, "end");
        add_key(2, 0x4e, "pg" U_DOWN);
        gap(8);
        add_key(4, 0x28, "ret");
        gap(2);
        add_key(2, 0x52, U_UP);
        gap(2);
        row_end();
        row_begin(3);
        add(4, K_SHIFT)->lab = U_UP " shift";
        gap(14);
        add_key(2, 0x50, U_LEFT);
        add_key(2, 0x51, U_DOWN);
        add_key(2, 0x4f, U_RIGHT);
        row_end();
        mods_row(4);
    } else {
        static const uint8_t u[4][4] = { { 0x53, 0x54, 0x55, 0x56 }, { 0x5f, 0x60, 0x61, 0x57 },
                                         { 0x5c, 0x5d, 0x5e, 0x2a }, { 0x59, 0x5a, 0x5b, 0x58 } };
        static const char *const l[4][4] = { { "num", "/", "*", "-" }, { "7", "8", "9", "+" },
                                             { "4", "5", "6", "del" }, { "1", "2", "3", "enter" } };
        for (int r = 0; r < 4; ++r) {
            row_begin(r);
            for (int c = 0; c < 4; ++c)
                add_key(6, u[r][c], l[r][c]);
            row_end();
        }
        row_begin(4);
        add_key(12, 0x62, "0");
        add_key(6, 0x63, ".");
        add_key(6, 0x2b, "tab");
        row_end();
    }
}

/* ---- drawing ---------------------------------------------------------------------------------------- */
static unsigned *mod_state(int kind)
{
    return kind == K_SHIFT ? &st_shift : kind == K_CTRL ? &st_ctrl : kind == K_ALT ? &st_alt : &st_gui;
}

static int typable(uint32_t cp)
{
    struct kstroke s[2];
    return kbd_char(hid_layout, cp, s) > 0;
}

static void utf8_put(char *b, uint32_t cp)
{
    if (cp < 0x80) {
        b[0] = (char)cp;
        b[1] = 0;
    } else if (cp < 0x800) {
        b[0] = (char)(0xc0 | cp >> 6);
        b[1] = (char)(0x80 | (cp & 0x3f));
        b[2] = 0;
    } else {
        b[0] = (char)(0xe0 | cp >> 12);
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        b[2] = (char)(0x80 | (cp & 0x3f));
        b[3] = 0;
    }
}

static void draw_key(int i, int lit)
{
    const struct key *k = &kb[i];
    uint32_t bg = k->kind == K_CHAR ? C_CARD2 : C_CARD3, fg = C_L1;
    int font = F_SEG;
    char lab[8];
    const char *t = k->lab;
    if (k->kind == K_CHAR) {
        uint32_t cp = st_shift ? k->scp : k->cp;
        utf8_put(lab, cp);
        t = lab;
        font = F_BTN;
        if (!typable(cp))
            fg = C_L3;
    } else if (k->kind != K_KEY) {
        unsigned s = *mod_state(k->kind);
        if (s)
            bg = s == 2 ? C_BLUE : C_STAR;
        if (s)
            fg = C_BG;
    }
    if (lit) {
        bg = C_L1;
        fg = C_BG;
    }
    ui_fill(k->x - GAP / 2, k->y, k->w + GAP, k->h, C_BG);
    ui_round(k->x, k->y, k->w, k->h, 26, bg);
    if (k->kind != K_CHAR && k->kind != K_KEY && *mod_state(k->kind) == 2)
        ui_fill(k->x + k->w / 4, k->y + k->h - 34, k->w / 2, 8, fg);   /* locked: a bar under the label */
    if (ut_text_w(font, t) > k->w - 12)
        font = F_SMALL;
    ut_text_c(font, k->x + k->w / 2, k->y + k->h / 2 + ui_px(font) * 36 / 100, t, fg);
    ui_dirty(k->x - GAP / 2, k->y, k->w + GAP, k->h);
}

static const char *const tab_lab[L_N] = { "abc", "#+=", "Fn", "num" };

static void tab_rect(int i, int *x, int *w)
{
    *w = (UI_W - 2 * 40 - 4 * 16) / 5;
    *x = 40 + i * (*w + 16);
}

static void draw_tabs(void)
{
    ui_fill(0, TAB_Y, UI_W, TAB_H, C_BG);
    for (int i = 0; i < 5; ++i) {
        int x, w;
        tab_rect(i, &x, &w);
        uint32_t bg = i == layer ? C_STAR : C_CARD;
        ui_round(x, TAB_Y, w, TAB_H, 30, bg);
        const char *t = i < L_N ? tab_lab[i] : hid_layout == KL_US ? "US" : hid_layout == KL_FI_MAC ? "FI Mac" : "FI PC";
        ui_text_c(i < L_N ? F_SEG : F_SMALL, x + w / 2, TAB_Y + TAB_H / 2 + 16, t,
                  i == layer ? C_BG : i < L_N ? C_L1 : C_STAR);
    }
    ui_dirty(0, TAB_Y, UI_W, TAB_H);
}

static void draw_echo(void)
{
    ui_fill(0, ECHO_Y, UI_W, ECHO_H, C_BG);
    ui_round(40, ECHO_Y, UI_W - 80, ECHO_H, 36, C_CARD);
    int w = UI_W - 80 - 100;
    if (!echo_n) {
        ui_text(F_DETAIL, 90, ECHO_Y + ECHO_H / 2 + 14, "What you type goes to the computer", C_L3);
    } else {
        const char *s = echo;
        while (*s && ut_text_w(F_ROW_M, s) > w) {   /* the tail that fits */
            uint32_t cp;
            s += kbd_utf8(s, &cp);
        }
        int x = ut_text(F_ROW_M, 90, ECHO_Y + ECHO_H / 2 + 20, s, C_L1);
        ui_fill(x + 4, ECHO_Y + 40, 5, ECHO_H - 80, C_STAR);   /* the caret */
    }
    ui_dirty(0, ECHO_Y, UI_W, ECHO_H);
}

static void draw_pad(int lit)
{
    ui_round(40, PAD_Y0, UI_W - 80, PAD_Y1 - PAD_Y0, 48, lit ? C_CARD2 : C_CARD);
    ui_text(F_DETAIL, 100, PAD_Y0 + 90, "Trackpad", C_L2);
    ui_text(F_SMALL, 100, PAD_Y1 - 60, "drag: move   tap: click   2 fingers: scroll, tap: right click", C_L3);
    ui_dirty(40, PAD_Y0, UI_W - 80, PAD_Y1 - PAD_Y0);
}

static void draw_head(void)
{
    int c = usbd_configured();
    ui_header_draw("Keyboard", "Remote", c ? "CONNECTED" : "NO HOST", c ? C_GREEN : C_ORANGE);
    ut_status_bar();
}

static void draw_all(void)
{
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    draw_head();
    draw_echo();
    draw_pad(0);
    draw_tabs();
    for (int i = 0; i < nk; ++i)
        draw_key(i, 0);
    ui_dirty_all();
}

/* ---- typing ------------------------------------------------------------------------------------------- */
static void echo_add(uint32_t cp)
{
    char b[4];
    utf8_put(b, cp);
    unsigned n = 0;
    while (b[n])
        ++n;
    while (echo_n + n + 1 > sizeof(echo)) {      /* drop the oldest character */
        uint32_t c;
        unsigned k = kbd_utf8(echo, &c);
        for (unsigned i = k; i <= echo_n; ++i)
            echo[i - k] = echo[i];
        echo_n -= k;
    }
    for (unsigned i = 0; i < n; ++i)
        echo[echo_n++] = b[i];
    echo[echo_n] = 0;
}

static void echo_back(void)
{
    while (echo_n && (echo[echo_n - 1] & 0xc0) == 0x80)
        --echo_n;
    if (echo_n)
        --echo_n;
    echo[echo_n] = 0;
}

static uint8_t held_mods(int with_shift)
{
    return (uint8_t)((st_ctrl ? HID_CTRL : 0) | (st_alt ? HID_ALT : 0) | (st_gui ? HID_GUI : 0) |
                     (with_shift && st_shift ? HID_SHIFT : 0));
}

static void release_oneshots(void)
{
    int redraw = 0;
    unsigned *s[4] = { &st_shift, &st_ctrl, &st_alt, &st_gui };
    for (int i = 0; i < 4; ++i)
        if (*s[i] == 1) {
            *s[i] = 0;
            redraw = 1;
        }
    if (redraw)
        for (int i = 0; i < nk; ++i)
            if (kb[i].kind != K_KEY)
                draw_key(i, 0);
}

static void press_key(int i)
{
    const struct key *k = &kb[i];
    if (k->kind == K_CHAR) {
        uint32_t cp = st_shift ? k->scp : k->cp;
        struct kstroke s[2];
        int n = kbd_char(hid_layout, cp, s);
        if ((unsigned)(2 * n) > hid_room()) {          /* both strokes of a dead key, or neither */
            ++hid_stats.dropped;
            return;
        }
        uint8_t m = held_mods(0);
        for (int j = 0; j < n; ++j)
            hid_key((uint8_t)(s[j].mod | m), s[j].key);
        if (n && !m)
            echo_add(cp);
    } else if (k->kind == K_KEY) {
        uint8_t m = held_mods(1);
        hid_key(m, k->usage);
        if (!(m & (HID_CTRL | HID_ALT | HID_GUI))) {
            if (k->usage == 0x2a)
                echo_back();
            else if (k->usage == 0x28 || k->usage == 0x58)
                echo_n = 0, echo[0] = 0;
            else if (k->usage == 0x2c)
                echo_add(' ');
            else if (k->usage >= 0x54 && k->usage <= 0x63)
                echo_add((uint32_t)"/*-+\n1234567890."[k->usage - 0x54]);
        }
    } else {
        unsigned *s = mod_state(k->kind);
        *s = (*s + 1) % 3;
        draw_key(i, 0);
        if (k->kind == K_SHIFT)
            for (int j = 0; j < nk; ++j)
                if (kb[j].kind == K_CHAR)
                    draw_key(j, 0);
        return;
    }
    release_oneshots();
    draw_echo();
}

static int hit_key(int x, int y)
{
    for (int i = 0; i < nk; ++i)
        if (x >= kb[i].x - GAP / 2 && x < kb[i].x + kb[i].w + GAP / 2 && y >= kb[i].y && y < kb[i].y + ROW_H)
            return i;
    return -1;
}

static int hit_tab(int x, int y)
{
    if (y < TAB_Y || y >= TAB_Y + TAB_H)
        return -1;
    for (int i = 0; i < 5; ++i) {
        int tx, tw;
        tab_rect(i, &tx, &tw);
        if (x >= tx && x < tx + tw)
            return i;
    }
    return -1;
}

/* ---- the screen ----------------------------------------------------------------------------------------- */
enum { F_NONE, F_KEY, F_PAD };

/* fingers on the trackpad now (counted, so a lost lift cannot leave the count wrong), but finger skip */
static int pad_count(const int *what, const struct mt *m, int skip)
{
    int n = 0;
    for (int f = 0; f < MT_N; ++f)
        n += f != skip && what[f] == F_PAD && m->down[f];
    return n;
}

/* held keys that repeat: characters, delete, the arrows, space, return, tab, page up/down; not the toggles
 * (Caps Lock, Num Lock), F-keys, Esc and the like */
static int repeats(int k)
{
    if (kb[k].kind == K_CHAR)
        return 1;
    if (kb[k].kind != K_KEY)
        return 0;
    switch (kb[k].usage) {
    case 0x2a: case 0x4c: case 0x4f: case 0x50: case 0x51: case 0x52: case 0x2c: case 0x28: case 0x2b:
    case 0x4b: case 0x4e: case 0x58:
        return 1;
    default:
        return kb[k].usage >= 0x59 && kb[k].usage <= 0x63;   /* numpad digits and point */
    }
}

void ut_kbd(void)
{
    struct mt m;
    struct mt_ev ev[MT_N];
    int what[MT_N], kidx[MT_N], lx[MT_N], ly[MT_N];
    uint64_t rep_at[MT_N];
    int pad_max = 0, pad_moved = 0, scroll_acc = 0;
    uint64_t pad_t0 = 0;
    for (int i = 0; i < MT_N; ++i)
        what[i] = F_NONE;
    hid_reset();
    mt_reset(&m);
    build();
    draw_all();
    ui_present();
    int conf = usbd_configured();
    uint64_t next = sb_ms() + 1000;
    for (;;) {
        int n = mt_poll(&m, ev, MT_N);
        if ((m.pressed & SB_K_BIXBY) || mt_held_ms(&m, SB_K_POWER) >= UI_POWER_LONG_MS)
            break;
        if (m.released & SB_K_POWER)
            hid_click(HID_LEFT);
        if (m.pressed & SB_K_UP)
            hid_wheel(3);
        if (m.pressed & SB_K_DOWN)
            hid_wheel(-3);
        int leave = 0;
        for (int e = 0; e < n; ++e) {                 /* one pass, in order: a lift sees its finger's moves */
            int f = ev[e].id, x = ev[e].x, y = ev[e].y;
            if (ev[e].st == MT_DOWN) {
                int t = hit_tab(x, y), k;
                what[f] = F_NONE;
                if (ui_header_back_hit(x, y)) {
                    leave = 1;
                } else if (t >= 0) {
                    if (t < L_N) {
                        layer = t;
                    } else {
                        hid_layout = (hid_layout + 1) % KL_N;
                    }
                    build();
                    draw_tabs();
                    ui_fill(0, KB_Y0, UI_W, UI_H - KB_Y0, C_BG);
                    for (int i = 0; i < nk; ++i)
                        draw_key(i, 0);
                    ui_dirty(0, KB_Y0, UI_W, UI_H - KB_Y0);
                    for (int i = 0; i < MT_N; ++i)
                        if (what[i] == F_KEY)
                            what[i] = F_NONE;         /* the keys under other fingers are gone */
                } else if ((k = hit_key(x, y)) >= 0) {
                    what[f] = F_KEY;
                    kidx[f] = k;
                    press_key(k);
                    if (kb[k].kind == K_CHAR || kb[k].kind == K_KEY)
                        draw_key(k, 1);
                    rep_at[f] = repeats(k) ? sb_ms() + 450 : ~0ULL;
                } else if (y >= PAD_Y0 && y < PAD_Y1) {
                    what[f] = F_PAD;
                    lx[f] = x;
                    ly[f] = y;
                    if (!pad_count(what, &m, f)) {   /* the first finger on the pad */
                        pad_max = 0;
                        pad_moved = 0;
                        scroll_acc = 0;
                        pad_t0 = sb_ms();
                        draw_pad(1);
                    }
                    int c = pad_count(what, &m, -1);
                    if (c > pad_max)
                        pad_max = c;
                }
            } else if (ev[e].st == MT_MOVE && what[f] == F_PAD) {
                int dx = x - lx[f], dy = y - ly[f], c = pad_count(what, &m, -1);
                lx[f] = x;
                ly[f] = y;
                pad_moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                if (c >= 2) {
                    scroll_acc += dy;                 /* two fingers: each finger's share */
                    while (scroll_acc >= 40 * c) {
                        hid_wheel(1);
                        scroll_acc -= 40 * c;
                    }
                    while (scroll_acc <= -40 * c) {
                        hid_wheel(-1);
                        scroll_acc += 40 * c;
                    }
                } else {
                    hid_move(dx * 3 / 2, dy * 3 / 2);
                }
            } else if (ev[e].st == MT_UP) {
                if (what[f] == F_KEY) {
                    if (kb[kidx[f]].kind == K_CHAR || kb[kidx[f]].kind == K_KEY)
                        draw_key(kidx[f], 0);
                } else if (what[f] == F_PAD) {
                    what[f] = F_NONE;
                    if (!pad_count(what, &m, -1)) {  /* the last finger left the pad */
                        if (pad_moved < 40 && sb_ms() - pad_t0 < 350)
                            hid_click(pad_max >= 2 ? HID_RIGHT : HID_LEFT);
                        draw_pad(0);
                    }
                }
                what[f] = F_NONE;
            }
        }
        if (leave)
            break;
        uint64_t now = sb_ms();
        for (int f = 0; f < MT_N; ++f)
            if (what[f] == F_KEY && m.down[f] && now >= rep_at[f]) {
                press_key(kidx[f]);                   /* held: repeat */
                draw_key(kidx[f], 1);
                rep_at[f] = now + 70;
            }
        if (now >= next || usbd_configured() != conf) {
            conf = usbd_configured();
            draw_head();
            next = now + 1000;
        }
        ui_present();
    }
    hid_reset();
    ui_input_reset();
}
