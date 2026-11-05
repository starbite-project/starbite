/* SPDX-License-Identifier: GPL-2.0-only
 * sbui widgets: status bar, header, tab bar, armed border, dialog, hold-to-confirm slider,
 * hold-Power ring, toast, chart (sbui_w.h).
 */
#include "sbui_w.h"
#include "sb7.h"

/* ---- status bar -------------------------------------------------------------------------------- */

static struct ui_status st_shown;
static int st_valid;

static int st_eq(const char *a, const char *b)
{
    return sb_streq(a, b);
}

static void battery(int x, int cy, int pct, int charging)
{
    uint32_t c = charging ? C_GREEN : pct >= 0 && pct < 20 ? C_RED : C_L1;
    ui_round(x, cy - 20, 76, 40, 11, 0x73ffffffU);
    ui_round(x + 4, cy - 16, 68, 32, 8, C_BG);
    if (pct > 0) {
        int w = 60 * (pct > 100 ? 100 : pct) / 100;
        ui_round(x + 8, cy - 12, w < 8 ? 8 : w, 24, 5, c);
    }
    ui_round(x + 80, cy - 8, 8, 16, 3, 0x73ffffffU);
}

void ui_status_draw(const struct ui_status *s, int force)
{
    if (!force && st_valid && st_eq(s->left, st_shown.left) && st_eq(s->mid, st_shown.mid) &&
        st_eq(s->right, st_shown.right) && s->bat == st_shown.bat && s->charging == st_shown.charging &&
        s->mid_c == st_shown.mid_c)
        return;
    struct ui_rect save = ui_get_clip();
    ui_noclip();
    const int base = 76;
    if (force || !st_valid || !st_eq(s->left, st_shown.left)) {
        ui_fill(0, 0, 440, UI_STATUS_H, C_BG);
        ui_text(F_STATUS, 72, base, s->left, C_L1);
    }
    if (force || !st_valid || !st_eq(s->mid, st_shown.mid) || s->mid_c != st_shown.mid_c) {
        ui_fill(440, 0, 560, UI_STATUS_H, C_BG);
        if (s->mid[0]) {
            int w = ui_text_w(F_HEAD, s->mid) + 48;
            uint32_t c = s->mid_c ? s->mid_c : C_L2;
            ui_round(720 - w / 2, 26, w, 60, 30, ui_mix(c, C_BG, 60));
            ui_text_c(F_HEAD, 720, 68, s->mid, c);
        }
    }
    if (force || !st_valid || !st_eq(s->right, st_shown.right) || s->bat != st_shown.bat ||
        s->charging != st_shown.charging) {
        ui_fill(1000, 0, UI_W - 1000, UI_STATUS_H, C_BG);
        int bx = UI_W - 72 - 88;
        ui_text_r(F_STATUS, bx - 18, base, s->right, C_L1);
        if (s->bat >= 0)
            battery(bx, base - 16, s->bat, s->charging);
    }
    st_shown = *s;
    st_valid = 1;
    ui_set_clip(save);
}

/* ---- header --------------------------------------------------------------------------------- */

void ui_header_draw(const char *title, const char *back, const char *note, uint32_t note_c)
{
    struct ui_rect save = ui_get_clip();
    ui_noclip();
    ui_fill(0, UI_HDR_Y, UI_W, UI_HDR_H, C_BG);
    if (back && back[0]) {
        int y = UI_HDR_Y + 48;
        ui_line(64, y, 46, y + 20, 6, C_STAR);
        ui_line(46, y + 20, 64, y + 40, 6, C_STAR);
        ui_text(F_ROW, 92, y + 38, back, C_STAR);
    }
    ui_text_cut(F_TITLE, 64, UI_HDR_Y + 198, title, note && note[0] ? 1000 : UI_W - 128, C_L1);
    if (note && note[0]) {
        uint32_t c = note_c ? note_c : C_L2;
        int w = ui_text_w(F_HEAD, note) + 44;
        ui_round(UI_W - 64 - w, UI_HDR_Y + 138, w, 64, 32, ui_mix(c, C_BG, 56));
        ui_text_c(F_HEAD, UI_W - 64 - w / 2, UI_HDR_Y + 182, note, c);
    }
    ui_set_clip(save);
}

int ui_header_back_hit(int x, int y)
{
    return y >= UI_HDR_Y && y < UI_HDR_Y + 120 && x < 560;
}

/* ---- tab bar ---------------------------------------------------------------------------------- */

static void tab_icon(int t, int cx, int cy, uint32_t c)
{
    int w, h;
    unsigned id = t == 1 ? SB7_I_CHECK_54 : t == 3 ? SB7_I_TIMER_54 : t == 5 ? SB7_I_STARF_54 :
                  t == 7 ? SB7_I_FLASK_54 : 0;
    if (id && ui_icon_size(id, &w, &h) && ui_icon(id, cx - w / 2, cy - h / 2, c))
        return;
    switch (t) {
    case 0:                                     /* dashboard: four tiles */
        for (int i = 0; i < 4; ++i)
            ui_round(cx - 26 + (i & 1) * 28, cy - 26 + (i >> 1) * 28, 24, 24, 7, c);
        break;
    case 2:                                     /* power: a battery */
        ui_round(cx - 30, cy - 16, 56, 32, 8, c);
        ui_round(cx - 26, cy - 12, 48, 24, 5, C_BAR | 0xff000000U);
        ui_round(cx - 22, cy - 8, 26, 16, 3, c);
        ui_round(cx + 28, cy - 7, 6, 14, 2, c);
        break;
    case 3:                                     /* clocks: a dial */
        ui_ring(cx, cy, 26, 5, c);
        ui_line(cx, cy, cx, cy - 15, 5, c);
        ui_line(cx, cy, cx + 12, cy + 6, 5, c);
        break;
    case 4:                                     /* storage: a stack of disks */
        for (int i = 0; i < 3; ++i)
            ui_round(cx - 28, cy - 26 + i * 19, 56, 14, 6, c);
        break;
    case 6:                                     /* logs: lines */
        for (int i = 0; i < 4; ++i)
            ui_round(cx - 26, cy - 22 + i * 14, i == 3 ? 34 : 52, 6, 3, c);
        break;
    default:
        ui_disc(cx, cy, 20, c);
        break;
    }
}

void ui_tabbar_draw(const char *const *labels, int n, int active)
{
    struct ui_rect save = ui_get_clip();
    ui_noclip();
    ui_fill(0, UI_TAB_Y, UI_W, UI_TAB_H, 0xff121214U);
    ui_fill(0, UI_TAB_Y, UI_W, 2, C_SEP);
    int cw = UI_W / (n > 0 ? n : 1);
    for (int t = 0; t < n; ++t) {
        int cx = t * cw + cw / 2;
        uint32_t c = t == active ? C_STAR : 0x8cebebf5U;
        tab_icon(t, cx, UI_TAB_Y + 72, c);
        ui_text_c(F_HEAD, cx, UI_TAB_Y + 150, labels[t], c);
    }
    ui_set_clip(save);
}

int ui_tabbar_hit(int n, int x, int y)
{
    if (y < UI_TAB_Y || n <= 0)
        return -1;
    int t = x / (UI_W / n);
    return t < n ? t : n - 1;
}

/* ---- armed border ------------------------------------------------------------------------------ */

void ui_border_draw(uint32_t c, int px)
{
    ui_guard(0);
    ui_fill(0, 0, UI_W, px, c);
    ui_fill(0, UI_H - px, UI_W, px, c);
    ui_fill(0, px, px, UI_H - 2 * px, c);
    ui_fill(UI_W - px, px, px, UI_H - 2 * px, c);
    ui_guard(px);
}

/* ---- save / restore, dim ---------------------------------------------------------------------- */

static uint32_t *spare;
static int saved;

void ui_save_screen(void)
{
    if (!spare)
        spare = sb_alloc((uint64_t)UI_W * UI_H * 4U);
    if (!spare)
        return;
    memcpy(spare, ui_back(), (uint64_t)UI_W * UI_H * 4U);
    saved = 1;
}

void ui_restore_screen(void)
{
    if (!saved || !spare)
        return;
    memcpy(ui_back(), spare, (uint64_t)UI_W * UI_H * 4U);
    saved = 0;
    ui_dirty_all();
}

static void dim(void)
{
    struct ui_rect c = ui_get_clip();
    ui_noclip();
    struct ui_rect r = ui_get_clip();
    uint32_t *b = ui_back();
    for (int y = r.y; y < r.y + r.h; ++y) {
        uint32_t *row = b + (uint64_t)y * UI_W;
        for (int x = r.x; x < r.x + r.w; ++x) {
            uint32_t p = row[x];                /* x 0.375 per channel */
            row[x] = 0xff000000U | (((p & 0xfcfcfcU) >> 2) + ((p & 0xf8f8f8U) >> 3));
        }
    }
    ui_dirty(r.x, r.y, r.w, r.h);
    ui_set_clip(c);
}

/* ---- dialog ------------------------------------------------------------------------------------- */

static int wrap_lines(const char *s, char out[][120], int max, int w)
{
    int n = 0;
    while (s && *s && n < max) {
        unsigned fit = ui_text_fit(F_ROW, s, w);
        if (s[fit]) {
            unsigned br = fit;
            while (br > 0 && s[br] != ' ')
                --br;
            if (br > fit / 3)
                fit = br + 1;
        }
        if (!fit)
            fit = 1;
        unsigned k = 0;
        for (; k < fit && k < 119; ++k)
            out[n][k] = s[k];
        out[n][k] = 0;
        s += fit;
        ++n;
    }
    return n;
}

int ui_dialog(const char *title, const char *l1, const char *l2, const char *ok, uint32_t ok_c)
{
    char lines[8][120];
    int n = wrap_lines(l1, lines, 4, 1000);
    n += wrap_lines(l2, lines + n, 8 - n, 1000);
    const int x = 140, w = UI_W - 280, lh = 64;
    int h = 150 + 70 + n * lh + 60 + 170;
    int y = (UI_H - h) / 2, by = y + h - 170;
    int sel = 1;
    ui_save_screen();
    dim();
    ui_input_reset();
    for (int redraw = 1;;) {
        if (redraw) {
            ui_round(x, y, w, h, 56, 0xff232326U);
            ui_text_c(F_TITLE_S, x + w / 2, y + 140, title, C_L1);
            for (int i = 0; i < n; ++i)
                ui_text_c(F_ROW, x + w / 2, y + 230 + i * lh, lines[i], i < (l1 ? 4 : 0) ? C_L1 : C_L2);
            ui_fill(x, by, w, 2, C_SEP);
            ui_fill(x + w / 2 - 1, by, 2, 170, C_SEP);
            ui_round_c(x, by + 2, w / 2 - 1, 168, 56, 4, sel == 0 ? C_CARD3 : 0xff232326U);
            ui_round_c(x + w / 2 + 1, by + 2, w / 2 - 1, 168, 56, 8, sel == 1 ? C_CARD3 : 0xff232326U);
            ui_text_c(F_BTN, x + w / 4, by + 106, "Cancel", C_STAR);
            ui_text_c(F_BTN, x + 3 * w / 4, by + 106, ok, ok_c ? ok_c : C_STAR);
            ui_present();
            redraw = 0;
        }
        struct ui_ev e;
        ui_poll(&e, 50);
        int r = -1;
        if (e.type == EV_UP || e.type == EV_DOWN) {
            sel ^= 1;
            redraw = 1;
        } else if (e.type == EV_SELECT) {
            r = sel;
        } else if (e.type == EV_BACK || e.type == EV_HOME) {
            r = 0;
        } else if (e.type == EV_TAP && e.y >= by && e.y < by + 170 && e.x >= x && e.x < x + w) {
            r = e.x >= x + w / 2;
        } else if (e.type == EV_TAP && (e.y < y || e.y >= y + h)) {
            r = 0;
        }
        if (r >= 0) {
            ui_restore_screen();
            ui_present();
            ui_input_reset();
            return r;
        }
    }
}

/* ---- hold-to-confirm slider ----------------------------------------------------------------------- */

int ui_confirm_slide(const char *title, const char *const *lines, int nlines, const char *slide_label,
                     uint32_t accent)
{
    const int x = 60, w = UI_W - 120, lh = 70;
    int h = 160 + nlines * lh + 80 + 200 + 170;
    int y = UI_H - h - 60;
    int ty = y + h - 200 - 150, tx = x + 60, tw = w - 120, th = 170, kr = 70;
    int knob = 0, dragging = 0, done = -1;
    uint64_t hold_t0 = 0;
    ui_save_screen();
    dim();
    ui_input_reset();
    ui_round(x, y, w, h, 56, 0xff232326U);
    ui_text(F_TITLE_S, x + 60, y + 120, title, C_L1);
    for (int i = 0; i < nlines; ++i) {
        const char *s = lines[i];
        const char *tab = s;
        while (*tab && *tab != '\t')
            ++tab;
        if (*tab) {
            char k[64];
            unsigned n = (unsigned)(tab - s);
            sb_strcpy(k, n + 1 < sizeof(k) ? n + 1 : sizeof(k), s);
            ui_text(F_ROW, x + 60, y + 210 + i * lh, k, C_L2);
            ui_text_cut(F_MONO_L, x + 420, y + 210 + i * lh, tab + 1, w - 480, C_L1);
        } else {
            ui_text_cut(F_ROW, x + 60, y + 210 + i * lh, s, w - 120, C_L2);
        }
    }
    int cy = y + h - 120;
    ui_text_c(F_BTN, x + w / 2, cy + 20, "Cancel", C_STAR);
    for (;;) {
        ui_round(tx, ty, tw, th, th / 2, ui_mix(accent, 0xff232326U, 60));
        int kx = tx + th / 2 + knob;
        ui_text_c(F_SEG, tx + tw / 2 + th / 4, ty + th / 2 + 16, slide_label, ui_mix(C_L1, 0xff232326U, 200 - knob * 160 / (tw - th)));
        ui_disc(kx, ty + th / 2, kr, accent);
        ui_line(kx - 14, ty + th / 2 - 22, kx + 8, ty + th / 2, 7, C_BG);
        ui_line(kx + 8, ty + th / 2, kx - 14, ty + th / 2 + 22, 7, C_BG);
        ui_present();
        if (done >= 0)
            break;
        struct ui_ev e;
        ui_poll(&e, 16);
        int range = tw - th;
        if (e.type == EV_PRESS && e.y >= ty && e.y < ty + th && e.x < kx + kr + 40 && e.x > kx - kr - 40)
            dragging = 1;
        else if (e.type == EV_DRAG && dragging)
            knob = e.x - (tx + th / 2);
        else if (e.type == EV_TOUCH_UP) {
            if (dragging && knob >= range - 8)
                done = 1;
            dragging = 0;
            if (done < 0)
                knob = 0;
        } else if (e.type == EV_TAP && e.y > cy - 80 && e.y < cy + 80)
            done = 0;
        else if (e.type == EV_BACK || e.type == EV_HOME)
            done = 0;
        if (ui_keys() & SB_K_UP) {
            if (!hold_t0)
                hold_t0 = sb_ms();
            knob = (int)((sb_ms() - hold_t0) * (uint64_t)range / 1500U);
            if (knob >= range)
                done = 1;
        } else if (hold_t0) {
            hold_t0 = 0;
            knob = 0;
        }
        if (knob < 0)
            knob = 0;
        if (knob > range)
            knob = range;
    }
    sb_mdelay(done ? 250 : 0);
    ui_restore_screen();
    ui_present();
    ui_input_reset();
    return done;
}

/* ---- hold Power ------------------------------------------------------------------------------- */

int ui_hold_power(const char *title, const char *hint, unsigned ms, uint32_t accent)
{
    const int cx = UI_W / 2, cy = 1400, r = 220;
    ui_save_screen();
    dim();
    ui_input_reset();
    sb_keys_release_wait();
    ui_round(120, 900, UI_W - 240, 1100, 64, 0xff232326U);
    ui_text_c(F_TITLE_S, cx, 1040, title, C_L1);
    ui_text_c(F_ROW, cx, 1860, hint, C_L2);
    uint64_t t0 = 0, start = sb_ms();
    int res = -1, last = -1;
    while (res < 0) {
        unsigned k = ui_keys();
        uint64_t now = sb_ms();
        int prog = 0;
        if (k & SB_K_POWER) {
            if (!t0)
                t0 = now;
            prog = (int)((now - t0) * 1000U / ms);
            if (prog >= 1000) {
                prog = 1000;
                res = 1;
            }
        } else if (t0) {
            res = 0;                            /* released early */
        } else if (now - start > 10000 || (k & SB_K_BIXBY)) {
            res = 0;
        }
        if (prog / 10 != last) {
            last = prog / 10;
            ui_disc(cx, cy, r, 0xff232326U);
            ui_ring(cx, cy, r, 22, C_CARD3);
            /* the arc as dots along the ring, clockwise from the top */
            int n = prog * 120 / 1000;
            static const int16_t sn[30] = { 0, 52, 104, 155, 207, 258, 309, 358, 406, 453, 500, 545, 588, 629,
                                            669, 707, 743, 777, 809, 839, 866, 891, 914, 934, 951, 966, 978,
                                            988, 995, 999 };
            for (int i = 0; i <= n && i < 120; ++i) {
                int q = i / 30, j = i % 30;
                int s = sn[j], c = sn[29 - j];
                int dx = q == 0 ? s : q == 1 ? c : q == 2 ? -s : -c;
                int dy = q == 0 ? -c : q == 1 ? s : q == 2 ? c : -s;
                ui_disc(cx + dx * (r - 11) / 1000, cy + dy * (r - 11) / 1000, 12, accent);
            }
            struct ub u;
            ub_init(&u);
            ub_fix(&u, (uint64_t)prog * ms / 1000U, 1);
            ub_s(&u, " s");
            ui_text_c(F_BIG, cx, cy + 36, t0 ? u.b : "Power", t0 ? C_L1 : C_L2);
            ui_present();
        }
        if (res < 0)
            sb_mdelay(16);
    }
    sb_keys_release_wait();
    ui_restore_screen();
    ui_present();
    ui_input_reset();
    return res;
}

/* ---- toast --------------------------------------------------------------------------------------- */

void ui_toast(const char *t, uint32_t c, unsigned ms)
{
    int w = ui_text_w(F_ROW, t) + 120, h = 140;
    if (w > UI_W - 120)
        w = UI_W - 120;
    int x = (UI_W - w) / 2, y = UI_TAB_Y - h - 60;
    ui_save_screen();
    ui_round(x, y, w, h, h / 2, 0xf0303034U);
    ui_text_cut(F_ROW, x + 60, y + 88, t, w - 120, c ? c : C_L1);
    ui_present();
    sb_mdelay(ms);
    ui_restore_screen();
    ui_present();
}

/* ---- chart ------------------------------------------------------------------------------------------ */

void chart_push(struct ui_chart *c, int32_t v)
{
    c->v[c->head] = v;
    c->head = (c->head + 1) % CHART_N;
    if (c->n < CHART_N)
        c->n++;
}

void chart_draw(const struct ui_chart *c, int x, int y, int w, int h, uint32_t line, uint32_t bg,
                const char *unit)
{
    ui_fill(x, y, w, h, bg);
    if (!c->n) {
        ui_text_c(F_DETAIL, x + w / 2, y + h / 2 + 14, "no samples yet", C_L3);
        return;
    }
    int32_t lo = c->v[(c->head + CHART_N - c->n) % CHART_N], hi = lo;
    for (int i = 0; i < c->n; ++i) {
        int32_t v = c->v[(c->head + CHART_N - c->n + i) % CHART_N];
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    if (lo > 0 && hi > 0 && lo < hi / 4)
        lo = 0;
    if (hi - lo < 20) {
        hi += 10;
        lo -= 10;
    }
    int32_t pad = (hi - lo) / 8;
    hi += pad;
    lo -= pad;
    int gx = x + 150, gw = w - 170, gy = y + 20, gh = h - 40;
    for (int i = 0; i <= 2; ++i)
        ui_fill(gx, gy + gh * i / 2, gw, 2, C_SEP);
    if (lo < 0 && hi > 0) {
        int zy = gy + (int)((int64_t)hi * gh / (hi - lo));
        ui_fill(gx, zy, gw, 3, 0x40ffffffU);
    }
    struct ub u;
    ub_init(&u);
    ub_sdec(&u, hi);
    ub_s(&u, unit);
    ui_text_r(F_SMALL, gx - 16, gy + 30, u.b, C_L3);
    ub_init(&u);
    ub_sdec(&u, lo);
    ub_s(&u, unit);
    ui_text_r(F_SMALL, gx - 16, gy + gh, u.b, C_L3);
    int px = -1, py = 0;
    for (int i = 0; i < c->n; ++i) {
        int32_t v = c->v[(c->head + CHART_N - c->n + i) % CHART_N];
        int sx = gx + gw - (c->n - 1 - i) * gw / (CHART_N - 1);
        int sy = gy + (int)((int64_t)(hi - v) * gh / (hi - lo));
        if (px >= 0)
            ui_line(px, py, sx, sy, 6, line);
        px = sx;
        py = sy;
    }
    ui_disc(px, py, 10, line);
}
