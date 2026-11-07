/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS gamepad: the phone held sideways (its top to the left) as a USB HID gamepad (hid.c report 4):
 * two sticks (X/Y left, Z/Rz right; each centres where the thumb lands), a D-pad (the hat switch), four
 * face buttons, L1/R1/L2/R2, Select, Start, Home; the volume keys are buttons 14 and 15. Every finger
 * counts (mt.c). Hold Bixby (or Power) to leave.
 * Drawing: the layout is in the player's coordinates (u to the right 0..2959, v down 0..1439) and turned
 * onto the panel (x = 1440 - v, y = u). Labels are drawn upright in a scratch corner the layout leaves
 * empty (u < 250, v > 760) and copied turned into place. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "hid.h"
#include "ut.h"
#include "usb_dwc3.h"
#include "mt.h"

#define GU 2960                         /* the player's width and height */
#define GV 1440
#define BG 0xff0b0b0dU

static int px_of(int u, int v) { (void)u; return UI_W - v; }

static void g_disc(int u, int v, int r, uint32_t c) { ui_disc(px_of(u, v), u, r, c); }
static void g_ring(int u, int v, int r, int w, uint32_t c) { ui_ring(px_of(u, v), u, r, w, c); }
/* a box in player coordinates: u..u+w, v..v+h */
static void g_round(int u, int v, int w, int h, int r, uint32_t c) { ui_round(UI_W - v - h, u, h, w, r, c); }

/* upright text for the player, its baseline-left at (u, v) */
static void g_text(int font, int u, int v, const char *s, uint32_t fg, uint32_t bg)
{
    int w = ui_text_w(font, s), asc = ui_ascent(font), h = ui_px(font) * 13 / 10;
    if (w > 1400 || h > 190)                          /* the scratch strip: u < 190, nothing else there */
        return;
    ui_fill(0, 0, w + 4, h, bg);                      /* the scratch: panel (0,0), the player's left edge */
    ui_text(font, 0, asc, s, fg);
    uint32_t *bb = ui_back();
    int top = v - asc;                                /* the text box's top in player coordinates */
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            uint32_t p = bb[(uint64_t)j * UI_W + (unsigned)i];
            if (p == bg)
                continue;
            int x = UI_W - (top + j), y = u + i;
            if (x >= 0 && x < UI_W && y >= 0 && y < UI_H)
                bb[(uint64_t)y * UI_W + (unsigned)x] = p;
        }
    ui_fill(0, 0, w + 4, h, BG);
    ui_dirty(0, 0, w + 4, h);
    ui_dirty(UI_W - top - h, u, h, w);
}

static void g_text_c(int font, int u, int v, const char *s, uint32_t fg, uint32_t bg)
{
    g_text(font, u - ui_text_w(font, s) / 2, v + ui_ascent(font) * 4 / 10, s, fg, bg);
}

/* ---- the layout --------------------------------------------------------------------------------------------- */
enum { B_A, B_B, B_X, B_Y, B_L1, B_R1, B_L2, B_R2, B_SEL, B_START, B_L3, B_R3, B_HOME, B_VUP, B_VDN, B_NB };
struct gbtn { int u, v, w, h, round; const char *lab; uint32_t c; };
static const struct gbtn btn[B_NB] = {
    [B_A] = { 2480, 880, 0, 0, 120, "A", 0xff2f9e5fU },   [B_B] = { 2720, 640, 0, 0, 120, "B", 0xffc8433aU },
    [B_X] = { 2240, 640, 0, 0, 120, "X", 0xff3a7bd5U },   [B_Y] = { 2480, 400, 0, 0, 120, "Y", 0xffd5a22aU },
    [B_L1] = { 260, 40, 620, 170, 0, "L1", C_CARD3 },     [B_R1] = { 2080, 40, 620, 170, 0, "R1", C_CARD3 },
    [B_L2] = { 920, 40, 440, 170, 0, "L2", C_CARD2 },     [B_R2] = { 1600, 40, 440, 170, 0, "R2", C_CARD2 },
    [B_SEL] = { 1240, 560, 200, 120, 0, "Select", C_CARD2 }, [B_START] = { 1520, 560, 200, 120, 0, "Start", C_CARD2 },
    [B_HOME] = { 1480, 860, 0, 0, 90, "Home", C_CARD3 },
};
#define LS_U 600                        /* left stick base */
#define LS_V 620
#define RS_U 1880
#define RS_V 1110
#define ST_R 250
#define DP_U 1100                       /* D-pad centre */
#define DP_V 1100
#define DP_A 130                        /* arm half-width */
#define DP_L 330                        /* arm reach from the centre */

static int in_btn(int i, int u, int v)
{
    const struct gbtn *b = &btn[i];
    if (b->round) {
        int du = u - b->u, dv = v - b->v, r = b->round + 40;
        return du * du + dv * dv <= r * r;
    }
    return u >= b->u - 30 && u < b->u + b->w + 30 && v >= b->v - 30 && v < b->v + b->h + 30;
}

static void draw_btn(int i, int lit)
{
    const struct gbtn *b = &btn[i];
    if (!b->lab)
        return;
    uint32_t c = lit ? C_L1 : b->c, fg = lit ? C_BG : C_L1;
    if (b->round) {
        g_disc(b->u, b->v, b->round + 6, BG);
        g_disc(b->u, b->v, b->round, c);
        g_text_c(b->round > 100 ? F_TITLE_S : F_SEG, b->u, b->v, b->lab, fg, c);
        ui_dirty(px_of(b->u, b->v) - b->round - 8, b->u - b->round - 8, 2 * b->round + 16, 2 * b->round + 16);
    } else {
        g_round(b->u, b->v, b->w, b->h, 50, c);
        g_text_c(F_SEG, b->u + b->w / 2, b->v + b->h / 2, b->lab, fg, c);
        ui_dirty(UI_W - b->v - b->h, b->u, b->h, b->w);
    }
}

/* Each control clears only its own disc or cross: the layout keeps them apart (a stick's disc of
 * ST_R + 115 from the D-pad, the face buttons, Home, the shoulders). */
static void draw_stick(int bu, int bv, int du, int dv, int active)
{
    int r = ST_R + 115;
    g_disc(bu, bv, r, BG);
    g_disc(bu, bv, ST_R, C_CARD);
    g_ring(bu, bv, ST_R, 6, active ? C_STAR : C_CARD3);
    g_disc(bu + du, bv + dv, 110, active ? C_STAR : C_CARD3);
    ui_dirty(UI_W - bv - r, bu - r, 2 * r, 2 * r);
}

static void draw_dpad(unsigned hat)
{
    static const int du[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, dv[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
    int r = DP_L + 20, off = DP_L - DP_A, doff = off * 6 / 10;
    for (int i = 1; i < 8; i += 2)                    /* the diagonal marks sit between the arms: erase them */
        g_disc(DP_U + du[i] * doff, DP_V + dv[i] * doff, DP_A - 16, BG);
    g_round(DP_U - DP_L, DP_V - DP_A, 2 * DP_L, 2 * DP_A, 30, C_CARD2);
    g_round(DP_U - DP_A, DP_V - DP_L, 2 * DP_A, 2 * DP_L, 30, C_CARD2);
    if (hat < 8) {
        int o = hat & 1 ? doff : off;                 /* diagonals nearer the centre: clear of Home */
        g_disc(DP_U + du[hat] * o, DP_V + dv[hat] * o, DP_A - 20, C_STAR);
    }
    ui_dirty(UI_W - DP_V - r, DP_U - r, 2 * r, 2 * r);
}

static int clamp127(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

/* hat from an offset: 8 ways, a diagonal when the smaller axis is over 40 % of the larger */
static unsigned hat_of(int du, int dv)
{
    int au = du < 0 ? -du : du, av = dv < 0 ? -dv : dv;
    if (au < 40 && av < 40)
        return 8;
    int h = au * 10 >= av * 4 ? (du > 0 ? 1 : -1) : 0, v = av * 10 >= au * 4 ? (dv > 0 ? 1 : -1) : 0;
    static const unsigned tab[3][3] = { { 7, 0, 1 }, { 6, 8, 2 }, { 5, 4, 3 } };   /* [v+1][h+1] */
    return tab[v + 1][h + 1];
}

enum { G_NONE, G_LS, G_RS, G_DPAD, G_BTN };

void ut_gamepad(void)
{
    struct mt m;
    struct mt_ev ev[MT_N];
    int what[MT_N], arg[MT_N], cu[MT_N], cv[MT_N], ou[MT_N], ov[MT_N];
    int ls_du = 0, ls_dv = 0, rs_du = 0, rs_dv = 0;
    unsigned hat = 8;
    for (int i = 0; i < MT_N; ++i)
        what[i] = G_NONE;
    hid_reset();
    mt_reset(&m);

    ui_fill(0, 0, UI_W, UI_H, BG);
    for (int i = 0; i < B_NB; ++i)
        draw_btn(i, 0);
    draw_stick(LS_U, LS_V, 0, 0, 0);
    draw_stick(RS_U, RS_V, 0, 0, 0);
    draw_dpad(8);
    int conf = -1;
    ui_dirty_all();
    uint16_t last_btn = 0xffff;
    for (;;) {
        int n = mt_poll(&m, ev, MT_N);
        ut_awake = ui_idle_ms() < UT_WATCH_MS;        /* a game: no saver while it is played */
        if (mt_held_ms(&m, SB_K_BIXBY) >= 500 || mt_held_ms(&m, SB_K_POWER) >= 1000)
            break;
        if (usbd_configured() != conf) {
            conf = usbd_configured();
            g_round(GU / 2 - 520, 250, 1040, 110, 0, BG);
            ui_dirty(UI_W - 250 - 110, GU / 2 - 520, 110, 1040);
            g_text_c(F_SMALL, GU / 2, 300, conf ? "USB: connected    hold Bixby to leave" :
                     "USB: no computer    hold Bixby to leave", conf ? C_GREEN : C_ORANGE, BG);
        }
        for (int e = 0; e < n; ++e) {
            int f = ev[e].id, u = ev[e].y, v = UI_W - ev[e].x;
            if (ev[e].st == MT_DOWN) {
                what[f] = G_NONE;
                for (int i = 0; i < B_NB; ++i)
                    if (btn[i].lab && in_btn(i, u, v)) {
                        what[f] = G_BTN;
                        arg[f] = i;
                    }
                if (what[f] == G_NONE) {
                    int du = u - DP_U, dv = v - DP_V;
                    if (du > -DP_L - 60 && du < DP_L + 60 && dv > -DP_L - 60 && dv < DP_L + 60)
                        what[f] = G_DPAD;
                    else if (u < GU / 2)
                        what[f] = G_LS;
                    else
                        what[f] = G_RS;
                    cu[f] = u;                    /* a stick centres where the thumb lands */
                    cv[f] = v;
                }
            }
            if (ev[e].st == MT_UP) {
                what[f] = G_NONE;
                continue;
            }
            if (what[f] == G_BTN) {               /* slide between buttons */
                for (int i = 0; i < B_NB; ++i)
                    if (btn[i].lab && in_btn(i, u, v))
                        arg[f] = i;
            } else {                              /* the offset from the stick's or the D-pad's centre */
                ou[f] = u - (what[f] == G_DPAD ? DP_U : cu[f]);
                ov[f] = v - (what[f] == G_DPAD ? DP_V : cv[f]);
            }
        }
        /* the state from every finger */
        struct hid_pad p = { 0, 8, 0, 0, 0, 0 };
        int nls = 0, nrs = 0, ndp = 0;
        for (int f = 0; f < MT_N; ++f) {
            if (!m.down[f] || what[f] == G_NONE)
                continue;
            if (what[f] == G_BTN) {
                p.buttons |= (uint16_t)(1U << arg[f]);
            } else if (what[f] == G_DPAD) {
                p.hat = (uint8_t)hat_of(ou[f], ov[f]);
                ndp = 1;
            } else {
                int du = ou[f], dv = ov[f], d2 = du * du + dv * dv;
                if (d2 > ST_R * ST_R) {           /* keep the knob on the ring */
                    int d = 1;
                    while (d * d < d2)
                        ++d;
                    du = du * ST_R / d;
                    dv = dv * ST_R / d;
                }
                if (what[f] == G_LS) {
                    p.x = (int8_t)clamp127(du * 127 / ST_R);
                    p.y = (int8_t)clamp127(dv * 127 / ST_R);
                    if (ls_du != du || ls_dv != dv)
                        draw_stick(LS_U, LS_V, du, dv, 1);
                    ls_du = du;
                    ls_dv = dv;
                    nls = 1;
                } else {
                    p.z = (int8_t)clamp127(du * 127 / ST_R);
                    p.rz = (int8_t)clamp127(dv * 127 / ST_R);
                    if (rs_du != du || rs_dv != dv)
                        draw_stick(RS_U, RS_V, du, dv, 1);
                    rs_du = du;
                    rs_dv = dv;
                    nrs = 1;
                }
            }
        }
        if (!nls && (ls_du || ls_dv)) {
            draw_stick(LS_U, LS_V, 0, 0, 0);
            ls_du = ls_dv = 0;
        }
        if (!nrs && (rs_du || rs_dv)) {
            draw_stick(RS_U, RS_V, 0, 0, 0);
            rs_du = rs_dv = 0;
        }
        if (!ndp)
            p.hat = 8;
        if (p.hat != hat) {
            hat = p.hat;
            draw_dpad(hat);
        }
        if (m.keys & SB_K_UP)
            p.buttons |= 1U << B_VUP;
        if (m.keys & SB_K_DOWN)
            p.buttons |= 1U << B_VDN;
        if (p.buttons != last_btn) {
            for (int i = 0; i < B_NB; ++i)
                if (((p.buttons ^ last_btn) >> i) & 1U)
                    draw_btn(i, (p.buttons >> i) & 1U);
            last_btn = p.buttons;
        }
        hid_gamepad(&p);
        ui_present();
    }
    struct hid_pad z = { 0, 8, 0, 0, 0, 0 };
    hid_gamepad(&z);                                  /* everything released before leaving */
    for (int i = 0; i < 50 && usbd_configured(); ++i) {
        ut_service();
        sb_mdelay(2);
    }
    ut_awake = 0;
    hid_reset();
    ui_input_reset();
}
