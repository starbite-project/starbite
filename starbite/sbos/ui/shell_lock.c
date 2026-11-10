/* SPDX-License-Identifier: MIT
 * The Starbite OS lock screen on sbtk (P1), after the approved concept (docs/concepts/starbite-os-shell/index.html;
 * its CSS gives every number here, in pt, and its script the behaviour):
 *   - the status bar, the "starbite" wordmark, the date, the centred clock (106 pt, tabular, its gradient);
 *   - the three 72 pt widgets (battery ring, alarm, temperature ring) in Liquid Glass over the wallpaper;
 *   - the Systems list: the boot menu's five vendor cards with their ghost stacks; a tap springs a card open into
 *     its member rows (rows spring 300 / 30, each row fading and sliding in), RUNNING on this system's row, LAST
 *     USED on the last booted one; the list scrolls (iOS physics, rubber-banding, our own: the concept leaves it
 *     to the browser) and an opening card scrolls itself into view as in the concept;
 *   - a member row opens the focus sheet (sheet spring 420 / 36) over a snapshot scrim; its slide to start (.86 of
 *     the track, 100 ms) starts the system (here: the concept's boot overlay);
 *   - torch hold bottom-left (420 ms) and Emergency hold bottom-right (700 ms) with the ring;
 *   - the swipe up (unlock spring 320 / 32, projection 200 ms, threshold .38 H, rubber .22 downwards) to the home,
 *     drawn here as the concept's first home page without its behaviour (a placeholder: home is P2).
 * Layers are drawn once (bake) and then only moved, faded and clipped by the compositor. */
#include "shell.h"

#define H_PT TK_HPT
static tk_comp *C;

/* ---- the catalog as the concept groups it (P2: from the store's catalog, as the boot menu) ----------------- */
typedef struct { const char *k, *name, *meta, *tag; int self; } sys_t;
typedef struct { const char *key, *name; int n; sys_t it[3]; } grp_t;
static const char *LAST = "win11";
static const grp_t G[5] = {
    { "apple", "Apple", 3, { { "ios16", "iOS 16", "Version 16.5", 0, 0 }, { "mac13", "macOS Ventura", "Version 13.5 \xc2\xb7 on the memory card", 0, 0 },
                             { "ios26", "iOS 26", "Version 26.6.2", "BETA", 0 } } },
    { "microsoft", "Microsoft", 1, { { "win11", "Windows 11", "Version 23H2", 0, 0 } } },
    { "google", "Google", 2, { { "android", "One UI 8", "Android 16", 0, 0 }, { "chromeos", "ChromiumOS", "Version 158", 0, 0 } } },
    { "linux", "Linux", 2, { { "linux-ubuntu", "Ubuntu", "26.04 LTS", 0, 0 }, { "linux-pmos", "postmarketOS", "Edge", 0, 0 } } },
    { "starbite", "Starbite", 3, { { "starbite-os", "Starbite OS", "This phone", 0, 1 }, { "engineer", "Engineer OS", "Version 2.1 \xc2\xb7 hardware tests", 0, 0 },
                                   { "utility", "Utility OS", "Version 2.0 \xc2\xb7 keys, gamepad, sensors", 0, 0 } } },
};
#define NG 5

static const tk_col INK = { 0xee, 0xf1, 0xf6, 1 };
static tk_col W(float a) { return TK_RGBA(255, 255, 255, a); }
static const tk_shadow SHTXT = { 0, 3.6923077f, 11.076923f, { 0, 0, 0, .28f } };   /* 0 1px 3px rgba(0,0,0,.28) */

/* ---- drawing in pt into a layer's bitmap -------------------------------------------------------------------- */
typedef struct { tk_bmp *b; tk_a8 *m; float ox, oy; } dc;   /* ox, oy: the bitmap's origin in screen px */

/* a layer whose bitmap covers the pt rect (plus a margin), placed at its rest position */
static dc mk(tk_layer *l, const char *name, float x, float y, float w, float h, float mg, int glass)
{
    tk_layer_init(l, name);
    float x0 = tk_maxf(PT(x - mg), -PT(40)), y0 = PT(y - mg);
    float x1 = tk_minf(PT(x + w + mg), PT(390 + 40)), y1 = PT(y + h + mg);
    int X = (int)tk_floorf(x0), Y = (int)tk_floorf(y0);
    tk_bmp_new(&l->bmp, (int)(x1 - X) + 2, (int)(y1 - Y) + 2);
    if (glass) tk_a8_new(&l->glass, l->bmp.w, l->bmp.h);
    l->x = l->bx = (float)X;
    l->y = l->by = (float)Y;
    dc d = { &l->bmp, glass ? &l->glass : 0, (float)X, (float)Y };
    return d;
}
static void d_text(dc *d, int f, float size, float x, float base, const char *s, float ls, int fl, tk_col c, int shadow)
{
    tk_paint p = tk_solid(c);
    tk_text(d->b, f, PT(size), PT(x) - d->ox, PT(base) - d->oy, s, PT(ls), fl, &p, shadow ? &SHTXT : 0);
}
static float tw(int f, float size, const char *s, float ls, int fl) { return tk_text_w(f, PT(size), s, PT(ls), fl) / TK_PT; }
/* a line box's baseline (Blink: ascent / descent rounded, half-leading floored) */
static float base_of(int f, float size, float top, float lh)
{
    float a = tk_ascent(f, PT(size)) / TK_PT, de = tk_descent(f, PT(size)) / TK_PT;
    if (lh <= 0) return top + a;
    return top + a + tk_floorf((lh - (a + de)) / 2);
}
static void d_icon(dc *d, const char *name, float x, float y, float alpha)
{
    const tk_bmp *ic = tk_icon(name);
    if (!ic) { tk_log(name); return; }
    float pad = tk_icon_pad(name);
    tk_blit(d->b, ic, (int)tk_floorf(PT(x) - pad - d->ox + .5f), (int)tk_floorf(PT(y) - pad - d->oy + .5f), alpha);
}
static void d_glass(dc *d, float x, float y, float w, float h, float r, int dark, int shadow)
{
    /* the bake works in the bitmap's px: shift the rect */
    tk_glass_bake(d->b, d->m, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r), dark, shadow);
}
static void d_rrect(dc *d, float x, float y, float w, float h, float r, tk_col c)
{
    tk_paint p = tk_solid(c);
    tk_fill_rrect(d->b, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r), &p, 1);
}
static void d_ring(dc *d, float cx, float cy, float r, float sw, float frac, tk_col c, int caps)
{
    /* only the ring's box */
    int bx = (int)tk_floorf(PT(cx - r - sw) - d->ox) - 2, by = (int)tk_floorf(PT(cy - r - sw) - d->oy) - 2;
    int n = (int)PT(2 * (r + sw)) + 5;
    tk_fm m;
    tk_fm_new(&m, n, n);
    float ox = d->ox + bx, oy = d->oy + by;
    if (frac >= 1) tk_fm_arc(&m, ox, oy, PT(cx), PT(cy), PT(r), PT(sw), 0, 6.2831853f, 0);
    else if (frac > 0) tk_fm_arc(&m, ox, oy, PT(cx), PT(cy), PT(r), PT(sw), 0, 6.2831853f * frac, caps);
    tk_paint p = tk_solid(c);
    tk_draw_fm(d->b, &m, bx, by, &p, 1);
    tk_fm_free(&m);
}

/* ---- state ------------------------------------------------------------------------------------------------- */
static tk_layer root, home, lockflat, lockg, lstatic, mark, glyph_c, glyph_o, list, etool, hint, homebar;
static tk_layer focus, scrim, fgrp, fcard, ffill, flabel, fthumb, boot, bootbar;
typedef struct {
    tk_layer face, facelit, icon, iconlit, ring, grp;
    tk_bmp ringbuf;
    float *ang, *cov;           /* the ring's per-pixel angle and radial coverage */
    float scale_from, scale_to, scale_t;
    int down, fired, lit;
    float t0, prog, ms;
} hold_t;
static hold_t Q[2];
typedef struct {
    tk_layer closed, open, face, head, chev, members, rows[3];
    tk_bmp chev0;               /* the chevron unrotated */
    float y;                    /* top in content pt (scroll 0) */
    float chx;                  /* the chevron's x */
    int isopen;
    float memh, full;           /* members' height now and open */
    tk_spring sp;
    float from, to;             /* the spring's ends (rows' p) */
    float chev_t, chev_from, chev_to, chev_now;   /* the chevron's .35 s transition */
} card_t;
static card_t K[NG];

static float lockY;             /* pt, 0 = locked in place */
static int locked = 1;
static tk_spring sp_lock;
static float scroll;            /* the list's scroll, pt */
static tk_spring sp_scroll;
static int scroll_momentum;
static float scroll_v;          /* pt per ms, momentum */
static int lock_ver = 1, flat_ver;
static float mark_a = 1, glyph_a, glyph_open;        /* their transitions' current values */
static float mark_t = 1, glyph_t = 1;                /* tween targets */
static float hint_a, hint_left;
static float focus_p;
static tk_spring sp_focus;
static int focus_on, focus_closing;
static const sys_t *focus_sys;
static float thumb_x;           /* pt */
static tk_spring sp_thumb;
static float boot_a, boot_t;
static int booting;
static float now_ms;

/* ---- the wallpaper and its glass backdrop ------------------------------------------------------------------ */
static void build_wall(void)
{
    tk_wall_draw(&C->base, 0);
    tk_bmp_down2(&C->base_half, &C->base);
    tk_glass_backdrop(&C->bw, &C->base);
    tk_bmp_down2(&C->bw_half, &C->bw);
}

/* ---- the status bar (lock and home) -------------------------------------------------------------------------- */
static void status_bar(dc *d, const char *left)
{
    d_text(d, TK_IT600, 15, 36, 29.5f, left, 0, 0, INK, 1);
    d_icon(d, "sb-wifi", 310, 18, 1);
    d_icon(d, "sb-batt", 333, 17.5f, 1);
}

/* ---- the lock's static layer: status bar, date, clock, widgets ------------------------------------------------ */
static void ring_widget(dc *d, float x, float p, tk_col col, const char *num, const char *lbl)
{
    d_glass(d, x, 220, 72, 72, 36, 1, 1);
    d_ring(d, x + 36, 256, 31, 4.5f, 1, W(.16f), 0);
    d_ring(d, x + 36, 256, 31, 4.5f, p, col, 1);
    float w = tw(TK_IT700, 17, num, 0, TK_TNUM);
    d_text(d, TK_IT700, 17, x + 36 - w / 2, 220 + 22 + 16, num, 0, TK_TNUM, INK, 0);
    w = tw(TK_IT700, 9.5f, lbl, .38f, 0);
    d_text(d, TK_IT700, 9.5f, x + 36 - w / 2, 220 + 50 + 9, lbl, .38f, 0, W(.72f), 0);
}
static void build_lstatic(void)
{
    dc d = mk(&lstatic, "lock-static", 0, 0, 390, 300, 0, 1);
    status_bar(&d, "No SIM");
    float w = tw(TK_IT600, 19, "Tuesday 10 November", 0, 0);
    d_text(&d, TK_IT600, 19, (390 - w) / 2, 78 + 18, "Tuesday 10 November", 0, 0, W(.9f), 1);
    /* the clock: 106 pt / 700 / -4 pt, tabular, gradient #fff -> #dfe9ff over its box (0, 96, 390 x 108.12) */
    {
        float cw = tw(TK_IT700, 106, "21:05", -4, TK_TNUM);
        tk_stop s[2] = { { 0, tk_hex(0xffffff, 1) }, { 1, tk_hex(0xdfe9ff, 1) } };
        tk_paint p = tk_linear(180, -d.ox, PT(96) - d.oy, PT(390), PT(108.12f), 2, s);
        tk_text(d.b, TK_IT700, PT(106), PT((390 - cw) / 2) - d.ox, PT(base_of(TK_IT700, 106, 96, 108.12f)) - d.oy, "21:05",
                PT(-4), TK_TNUM, &p, 0);
    }
    ring_widget(&d, 33, .87f, tk_hex(0x4cd964, 1), "87", "BATTERY");
    ring_widget(&d, 285, .41f, tk_hex(0x6fb3ff, 1), "41\xc2\xb0", "TEMP");
    d_glass(&d, 115, 220, 160, 72, 22, 1, 1);
    d_icon(&d, "alarm", 129, 239.5f, 1);
    d_text(&d, TK_IT700, 11, 145, 238 + 11, "ALARM", .33f, 0, W(.7f), 0);
    d_text(&d, TK_IT700, 17, 129, 254 + 16, "07:00 \xc2\xb7 Vibrate", 0, 0, INK, 0);
}

static void build_marks(void)
{
    dc d = mk(&mark, "mark", 150, 45, 90, 30, 4, 0);
    float w = tw(TK_OF300, 16, "starbite", -.15f, 0), x0 = (390 - (w + 1 + 9)) / 2;
    d_text(&d, TK_OF300, 16, x0, 66, "starbite", -.15f, 0, W(.82f), 1);
    d_icon(&d, "mark-star", x0 + w + 1, 51, 1);
    d = mk(&glyph_c, "glyph", 187.5f, 50, 15, 20, 2, 0);
    d_icon(&d, "lock-c", 187.5f, 50, 1);
    d = mk(&glyph_o, "glyph-open", 187.5f, 50, 15, 20, 2, 0);
    d_icon(&d, "lock-o", 187.5f, 50, 1);
    d = mk(&hint, "hint", 0, 756, 390, 16, 4, 0);
    w = tw(TK_IT600, 13, "Swipe up to open", 0, 0);
    d_text(&d, TK_IT600, 13, (390 - w) / 2, 756 + 13, "Swipe up to open", 0, 0, W(.75f), 1);
    d = mk(&homebar, "homebar", 127, 788, 136, 5, 1, 0);
    d_rrect(&d, 127, 788, 136, 5, 3, W(1));
}

/* ---- the press-and-hold buttons ---------------------------------------------------------------------------- */
static void build_hold(int i)
{
    hold_t *q = &Q[i];
    float x = i ? 292 : 46, y = 694;
    tk_layer_init(&q->grp, i ? "q-em" : "q-light");
    q->grp.ox = PT(x + 26);
    q->grp.oy = PT(y + 26);
    dc d = mk(&q->face, "q-face", x, y, 52, 52, 56, 1);
    d_glass(&d, x, y, 52, 52, 26, 1, 1);
    /* lit: the background is white (torch) or the Emergency red instead of the tint, the rest of the glass stays */
    d = mk(&q->facelit, "q-lit", x, y, 52, 52, 56, 1);
    d_glass(&d, x, y, 52, 52, 26, 1, 1);
    d_rrect(&d, x, y, 52, 52, 26, i ? tk_hex(0xff5a4e, 1) : W(1));
    tk_box_shadow(d.b, PT(x) - d.ox, PT(y) - d.oy, PT(52), PT(52), PT(26), 0, PT(1.5f), 0, 0, W(.42f), 1);
    tk_box_shadow(d.b, PT(x) - d.ox, PT(y) - d.oy, PT(52), PT(52), PT(26), 0, PT(-1), 0, 0, W(.12f), 1);
    d = mk(&q->icon, "q-icon", x + 16, y + 14, 20, 24, 2, 0);
    d_icon(&d, i ? "flame" : "torch", x + 16, y + 14, 1);
    d = mk(&q->iconlit, "q-iconlit", x + 16, y + 14, 20, 24, 2, 0);
    d_icon(&d, i ? "flame" : "torch-lit", x + 16, y + 14, 1);
    d = mk(&q->ring, "q-ring", x - 4, y - 4, 60, 60, 1, 0);
    /* the ring's tables: circle r 28 in a 60 pt box, stroke 2 (#fff / #ff8a7a), from 12 o'clock clockwise */
    int n = q->ring.bmp.w * q->ring.bmp.h;
    q->ang = tk_alloc((size_t)n * 4);
    q->cov = tk_alloc((size_t)n * 4);
    for (int j = 0; j < q->ring.bmp.h; j++)
        for (int k = 0; k < q->ring.bmp.w; k++) {
            float px = k + .5f + d.ox - PT(x + 26), py = j + .5f + d.oy - PT(y + 26);
            float a = tk_atan2f(px, -py);
            if (a < 0) a += 6.2831853f;
            q->ang[j * q->ring.bmp.w + k] = a;
            q->cov[j * q->ring.bmp.w + k] = tk_clampf(.5f - (tk_fabsf(tk_sqrtf(px * px + py * py) - PT(28)) - PT(1)), 0, 1);
        }
    q->ms = i ? 700 : 420;
    q->scale_t = q->scale_from = q->scale_to = 1;
    q->facelit.hidden = q->iconlit.hidden = 1;
    q->ring.hidden = 1;
    tk_layer_add(&q->grp, &q->face);
    tk_layer_add(&q->grp, &q->facelit);
    tk_layer_add(&q->grp, &q->icon);
    tk_layer_add(&q->grp, &q->iconlit);
    tk_layer_add(&q->grp, &q->ring);
}
static void hold_ring(hold_t *q, int i, float p)
{
    tk_bmp *b = &q->ring.bmp;
    uint32_t col = i ? tk_premul(tk_hex(0xff8a7a, 1)) : 0xffffffffu;
    float sweep = 6.2831853f * p, aa = 1.0f / PT(28);       /* a px of arc in radians */
    for (int k = 0; k < b->w * b->h; k++) {
        float c = q->cov[k];
        if (c <= 0) { b->px[k] = 0; continue; }
        float e = tk_clampf((sweep - q->ang[k]) / aa + .5f, 0, 1) * c;
        unsigned a = (unsigned)(e * 255 + .5f);
        b->px[k] = a == 255 ? col : (((col >> 24) * a / 255) << 24 | ((col >> 16 & 255) * a / 255) << 16 | ((col >> 8 & 255) * a / 255) << 8 | ((col & 255) * a / 255));
    }
    q->ring.hidden = p <= 0;
    tk_layer_dirty(&q->ring);
}

/* ---- the Systems list ------------------------------------------------------------------------------------------ */
#define LIST_TOP 310.0f
#define LIST_H 374.0f           /* .nwrap: top 310, bottom 118 */
static int stack(int g) { return G[g].n > 1; }
static void layout_list(void)
{
    float y = 10;
    for (int g = 0; g < NG; g++) {
        K[g].y = y;
        y += 68 + K[g].memh + (stack(g) ? 22 : 10);
    }
}
static float content_h(void)
{
    float y = 10;
    for (int g = 0; g < NG; g++) y += 68 + K[g].memh + (stack(g) ? 22 : 10);
    /* the last margin (22) collapses with the tools' 12; then their 32, margin 4, padding 14 */
    return y + 32 + 4 + 14;
}
static float max_scroll(void) { return tk_maxf(0, content_h() - LIST_H); }

static void head_content(dc *d, int g, float top)
{
    const grp_t *gr = &G[g];
    char tilename[40] = "t46-";
    tk_memcpy(tilename + 4, gr->key, tk_strlen(gr->key) + 1);
    d_icon(d, tilename, 25, top + 11, 1);
    float b = base_of(TK_IT800, 21, top + 13.45f, 23.1f);
    d_text(d, TK_IT800, 21, 83, b, gr->name, -.5f, 0, INK, 0);
    char names[160];
    int n = 0;
    for (int i = 0; i < gr->n; i++) {
        const char *s = gr->it[i].name;
        if (i) { names[n++] = ','; names[n++] = ' '; }
        while (*s) names[n++] = *s++;
    }
    names[n++] = ' ';
    names[n] = 0;
    char em[40] = "\xc2\xb7 0 System";
    em[3] = (char)('0' + gr->n);
    if (gr->n > 1) { em[11] = 's'; em[12] = 0; }
    float sb = base_of(TK_IT400, 13.5f, top + 38.55f, 0);
    d_rrect(d, 83, sb - 7, 6, 6, 3, tk_hex(0x4cd964, 1));
    d_text(d, TK_IT400, 13.5f, 95, sb, names, 0, 0, W(.82f), 0);
    d_text(d, TK_IT400, 13.5f, 95 + tw(TK_IT400, 13.5f, names, 0, 0), sb, em, 0, 0, W(.48f), 0);
    /* the grid's 1fr column grows to the nowrap line's width (min-content), pushing the chevron */
    float col = tk_maxf(249, tk_maxf(tw(TK_IT800, 21, gr->name, -.5f, 0),
                                     12 + tw(TK_IT400, 13.5f, names, 0, 0) + tw(TK_IT400, 13.5f, em, 0, 0)));
    K[g].chx = 83 + col + 12;
}
static void ghost(tk_bmp *G0, float ox, float oy, float x, float y, float w, float op)
{
    /* an .lg ghost: radius 0 0 22 22, its own backdrop the list content under it, then faded as one (worked in
     * a box around it with room for its shadow) */
    int X = tk_maxi(0, (int)tk_floorf(PT(x - 56) - ox)), Y = tk_maxi(0, (int)tk_floorf(PT(y - 56) - oy));
    int W = tk_mini(G0->w - X, (int)PT(w + 112)), H = tk_mini(G0->h - Y, (int)PT(26 + 112));
    tk_bmp t, g;
    tk_bmp_new(&t, W, H);
    tk_bmp_new(&g, W, H);
    tk_bmp_copy(&g, 0, 0, G0, X, Y, W, H);
    tk_corner = (tk_r4){ 0, 0, PT(22), PT(22) };
    tk_use_r4 = 1;
    tk_glass_local(&t, &g, PT(x) - ox - X, PT(y) - oy - Y, PT(w), PT(26), PT(22));
    tk_glass_bake(&t, 0, PT(x) - ox - X, PT(y) - oy - Y, PT(w), PT(26), PT(22), 0, 1);
    tk_use_r4 = 0;
    tk_blit(G0, &t, X, Y, op);
    tk_bmp_free(&t);
    tk_bmp_free(&g);
}
static void build_card(int g)
{
    card_t *k = &K[g];
    const grp_t *gr = &G[g];
    float top = LIST_TOP + k->y;            /* rest screen pt at scroll 0 */
    k->full = 60.0f * gr->n;
    /* closed: the ghosts (painted before every card), the card's own glass over them, its head */
    dc d = mk(&k->closed, "card", 14, top, 362, 76, 56, 0);
    if (stack(g)) {
        ghost(d.b, d.ox, d.oy, 33, top + 56, 324, .4f);
        ghost(d.b, d.ox, d.oy, 23, top + 50, 344, .7f);
    }
    tk_glass_local(d.b, d.b, PT(14) - d.ox, PT(top) - d.oy, PT(362), PT(68), PT(26));
    d_glass(&d, 14, top, 362, 68, 26, 0, 1);
    head_content(&d, g, top);
    /* open: the face at the open height (3-slice while it grows), the head, the rows */
    tk_layer_init(&k->open, "card-open");
    d = mk(&k->face, "face", 14, top, 362, 68 + k->full, 56, 0);
    d_glass(&d, 14, top, 362, 68 + k->full, 26, 0, 1);
    k->face.slice = 1;
    k->face.slice_s0 = PT(40 + 56);
    k->face.slice_s1 = PT(30 + 56);
    d = mk(&k->head, "head", 14, top, 362, 68, 2, 0);
    head_content(&d, g, top);
    d = mk(&k->chev, "chev", k->chx, top + 27, 14, 14, 0, 0);
    d_icon(&d, "chev", k->chx, top + 27, 1);
    k->chev_t = 1;
    tk_bmp_new(&k->chev0, k->chev.bmp.w, k->chev.bmp.h);
    tk_bmp_copy(&k->chev0, 0, 0, &k->chev.bmp, 0, 0, k->chev.bmp.w, k->chev.bmp.h);
    tk_layer_init(&k->members, "members");
    k->members.clip = 1;
    for (int i = 0; i < gr->n; i++) {
        const sys_t *s = &gr->it[i];
        float ry = top + 68 + 60 * i;
        d = mk(&k->rows[i], "row", 20, ry - 2, 350, 58, 1, 0);
        if (i) d_rrect(&d, 82, ry - 2, 278, .5f, 0, W(.14f));
        char tn[40] = "t38-";
        tk_memcpy(tn + 4, s->k, tk_strlen(s->k) + 1);
        d_icon(&d, tn, 35, ry + 9, 1);
        d_text(&d, TK_IT700, 16.5f, 85, ry + 9.5f + 16, s->name, -.2f, 0, INK, 0);
        d_text(&d, TK_IT400, 13, 85, ry + 30.5f + 13, s->meta, 0, 0, W(.72f), 0);
        const char *tag = s->self ? "RUNNING" : !tk_strcmp(s->k, LAST) ? "LAST USED" : s->tag;
        if (tag) {
            float w = tw(TK_IT700, 10.5f, tag, .42f, 0) + 14;
            float tx = 356 - w, ty = ry + 18.5f;
            d_rrect(&d, tx, ty, w, 19, 9.5f, s->self ? TK_RGBA(76, 217, 100, .18f) : TK_RGBA(111, 179, 255, .18f));
            d_text(&d, TK_IT700, 10.5f, tx + 7, ty + 3 + 10, tag, .42f, 0, s->self ? tk_hex(0xc9f7d3, 1) : tk_hex(0xcfe4ff, 1), 0);
        }
        tk_layer_add(&k->members, &k->rows[i]);
    }
    tk_layer_add(&k->open, &k->face);
    tk_layer_add(&k->open, &k->head);
    tk_layer_add(&k->open, &k->members);
    k->open.hidden = 1;
    k->chev.clip = 1;
    tk_layer_add(&list, &k->closed);
    tk_layer_add(&list, &k->open);
    tk_layer_add(&list, &k->chev);
}
static void build_etool(void)
{
    float top = LIST_TOP + 448;
    dc d = mk(&etool, "etool", 90, top, 210, 32, 56, 0);
    float w1 = 14 + 15 + 6 + tw(TK_IT600, 13, "Recovery", 0, 0) + 14, w2 = 14 + 15 + 6 + tw(TK_IT600, 13, "Lab", 0, 0) + 14;
    float x = (390 - (w1 + 10 + w2)) / 2;
    d_glass(&d, x, top, w1, 32, 16, 1, 1);
    d_icon(&d, "recovery", x + 14, top + 8.5f, 1);
    d_text(&d, TK_IT600, 13, x + 35, top + 8 + 13, "Recovery", 0, 0, INK, 0);
    x += w1 + 10;
    d_glass(&d, x, top, w2, 32, 16, 1, 1);
    d_icon(&d, "lab", x + 14, top + 8.5f, 1);
    d_text(&d, TK_IT600, 13, x + 35, top + 8 + 13, "Lab", 0, 0, INK, 0);
}

/* place every list layer for the current scroll, heights and row animations */
static void place_list(void)
{
    float y = 10;
    for (int g = 0; g < NG; g++) {
        card_t *k = &K[g];
        float dy = PT(y - k->y);           /* how far the card moved from where it was baked */
        float top = LIST_TOP + y;
        int anim = k->sp.live || k->isopen || k->memh > 0.01f || k->chev_now != 0;
        k->closed.hidden = anim;
        k->open.hidden = !anim;
        k->closed.y = k->closed.by + dy;
        k->face.y = k->face.by + dy;
        k->head.y = k->head.by + dy;
        k->chev.y = k->chev.by + dy;
        k->chev.clip_r = (tk_rect){ PT(14), PT(top), PT(362), PT(68) };
        float h = 68 + k->memh;
        k->face.slice_h = PT(h + 112);
        k->members.clip_r = (tk_rect){ PT(14), PT(top + 68), PT(362), PT(k->memh) };
        float p = k->full > 0 ? k->memh / (k->to > k->from ? k->full : tk_maxf(1, k->from)) : 0;
        if (!k->sp.live) p = k->isopen ? 1 : 0;
        for (int i = 0; i < G[g].n; i++) {
            float a = tk_clampf(p * 1.4f - i * .15f, 0, 1), ty = (1 - tk_minf(1, p)) * -14 * (i + 1);
            tk_layer *r = &K[g].rows[i];
            r->alpha = a;
            r->y = r->by + dy + PT(ty);
        }
        y += h + (stack(g) ? 22 : 10);
    }
    float edy = PT(y - 448);
    etool.y = etool.by + edy;
    list.y = PT(-scroll);
    list.clip_r = (tk_rect){ 0, PT(LIST_TOP), PT(390), PT(LIST_H) };
    lock_ver++;
}

/* the chevron's .35 s cubic-bezier(.3, 1.4, .4, 1) turn */
static void chev_draw(card_t *k, float deg)
{
    tk_bmp_rotate(&k->chev.bmp, &k->chev0, deg);
    tk_layer_dirty(&k->chev);
}

static void toggle_group(int g, int instant)
{
    card_t *k = &K[g];
    k->isopen = !k->isopen;
    float from = k->memh, to = k->isopen ? k->full : 0;
    k->from = from;
    k->to = to;
    k->chev_from = k->chev_now;
    k->chev_to = k->isopen ? 90 : 0;
    k->chev_t = 0;
    if (instant) {
        k->memh = to;
        k->sp.live = 0;
        k->chev_now = k->chev_to;
        k->chev_t = 1;
        chev_draw(k, k->chev_now);
    } else
        tk_spring_start(&k->sp, from, to, 0, TK_SPR_ROWS);
    place_list();
}

/* ---- the home (P1: the concept's first page, drawn once; P2 builds the real one) --------------------------- */
static tk_layer hparts[8];
static void build_home(void)
{
    static const char *P1[8] = { "calendar", "weather", "clock", "camera", "photos", "music", "notes", "calc" };
    static const char *P1N[8] = { "Calendar", "Weather", "Clock", "Camera", "Photos", "Music", "Notes", "Calculator" };
    static const char *DK[4] = { "phone", "messages", "browser", "settings" };
    tk_layer grp;
    tk_layer_init(&grp, "home-build");
    dc d = mk(&hparts[0], "home-sb", 0, 0, 390, 48, 0, 0);
    status_bar(&d, "21:05");
    d = mk(&hparts[1], "home-w", 0, 50, 390, 200, 56, 1);
    d_glass(&d, 24, 60, 158, 158, 24, 0, 1);
    d_glass(&d, 208, 60, 158, 158, 24, 0, 1);
    /* Battery: .lab, .big, three rows */
    d_icon(&d, "home-batt", 38, 74 + 1.5f, 1);
    d_text(&d, TK_IT700, 11.5f, 56, base_of(TK_IT700, 11.5f, 74, 0), "Galaxy S9", .345f, 0, W(.7f), 0);
    d_text(&d, TK_IT800, 38, 38, base_of(TK_IT800, 38, 74 + 14 + 4, 0), "87%", -1.2f, TK_TNUM, INK, 0);
    {
        static const char *L[3] = { "Battery", "Phone", "Lasts" }, *R[3] = { "31 \xc2\xb0" "C", "41 \xc2\xb0" "C", "19 h" };
        float y = 74 + 14 + 4 + 46;
        for (int i = 0; i < 3; i++) {
            y += 3;
            float b = base_of(TK_IT400, 12.5f, y, 0);
            d_text(&d, TK_IT400, 12.5f, 38, b, L[i], 0, TK_TNUM, W(.82f), 0);
            d_text(&d, TK_IT400, 12.5f, 352 - 184 - tw(TK_IT400, 12.5f, R[i], 0, TK_TNUM), b, R[i], 0, TK_TNUM, W(.82f), 0);
            y += 15;
        }
    }
    d_icon(&d, "home-star", 222, 74 + 1.5f, 1);
    d_text(&d, TK_IT700, 11.5f, 240, base_of(TK_IT700, 11.5f, 74, 0), "Recent", .345f, 0, W(.7f), 0);
    {
        static const char *T4[4] = { "t52-win11", "t52-ios26", "t52-engineer", "t52-ios16" };
        for (int i = 0; i < 4; i++) d_icon(&d, T4[i], 222 + (i & 1) * 78, 74 + 14 + 9 + (i >> 1) * 60, 1);
    }
    float cw = tw(TK_IT500, 12, "Battery", 0, 0);
    d_text(&d, TK_IT500, 12, 24 + 79 - cw / 2, 224 + 12, "Battery", 0, 0, INK, 1);
    cw = tw(TK_IT500, 12, "Systems", 0, 0);
    d_text(&d, TK_IT500, 12, 208 + 79 - cw / 2, 224 + 12, "Systems", 0, 0, INK, 1);
    d = mk(&hparts[2], "home-apps", 0, 250, 390, 220, 4, 0);
    for (int i = 0; i < 8; i++) {
        float cx = 22 + 86.5f * (i % 4) + 43.25f, y = 258 + 103 * (i / 4);
        char n[40] = "a62-";
        tk_memcpy(n + 4, P1[i], tk_strlen(P1[i]) + 1);
        d_icon(&d, n, cx - 31, y, 1);
        float w = tw(TK_IT500, 12, P1N[i], 0, 0);
        d_text(&d, TK_IT500, 12, cx - w / 2, y + 68 + 12, P1N[i], 0, 0, INK, 1);
    }
    d = mk(&hparts[3], "home-dock", 0, 620, 390, 182, 56, 1);
    {
        float sw = 15 + 13 + 6 + tw(TK_IT600, 13, "Search", 0, 0) + 15, sx = 195 - sw / 2;
        d_glass(&d, sx, 640, sw, 30, 15, 1, 1);
        d_icon(&d, "search", sx + 15, 640 + 8.5f, 1);
        d_text(&d, TK_IT600, 13, sx + 34, 640 + 7 + 13, "Search", 0, 0, INK, 1);
    }
    d_glass(&d, 12, 688, 366, 96, 36, 0, 1);
    for (int i = 0; i < 4; i++) {
        float x = 22 + 86.5f * i + 12.25f;
        char n[40] = "a62-";
        tk_memcpy(n + 4, DK[i], tk_strlen(DK[i]) + 1);
        d_icon(&d, n, x, 705, 1);
        if (i < 2) {
            const char *bn = i ? "2" : "1";
            float bw = tk_maxf(20, tw(TK_IT700, 12, bn, 0, 0) + 10);
            d_rrect(&d, x + 60 - bw, 699, bw, 20, 10, tk_hex(0xff3b30, 1));
            d_text(&d, TK_IT700, 12, x + 60 - bw / 2 - tw(TK_IT700, 12, bn, 0, 0) / 2, base_of(TK_IT700, 12, 699, 20), bn, 0, 0, INK, 0);
        }
    }
    for (int i = 0; i < 4; i++) tk_layer_add(&grp, &hparts[i]);
    tk_layer_init(&home, "home");
    tk_bmp_new(&home.bmp, TK_W, TK_H);
    tk_a8_new(&home.glass, TK_W, TK_H);
    tk_flatten(&grp, &home.bmp, &home.glass);
    for (int i = 0; i < 4; i++) { tk_bmp_free(&hparts[i].bmp); tk_a8_free(&hparts[i].glass); }
    /* #home has will-change: opacity, a backdrop root in Chrome: its glass shows no wallpaper blur, only its tint */
    tk_a8_free(&home.glass);
    home.ox = PT(195);
    home.oy = PT(401);
}

static void relayer(tk_layer *l) { tk_layer_dirty(l); tk_bmp_free(&l->bmp); tk_a8_free(&l->glass); }
static float focus_sy;

/* ---- the focus sheet --------------------------------------------------------------------------------------- */
static void build_focus(const sys_t *s, const grp_t *gr)
{
    /* the scrim: what is on the panel now, blurred 22 pt, saturated 1.3, under rgba(4, 8, 26, .28) */
    tk_comp_frame(C, 0, 1);
    relayer(&scrim);
    tk_layer_init(&scrim, "scrim");
    tk_bmp_new(&scrim.bmp, TK_W, TK_H);
    {
        tk_bmp a, q;
        tk_bmp_new(&a, TK_W / 2, TK_H / 2);
        tk_bmp_new(&q, TK_W / 4, TK_H / 4);
        tk_bmp_down2(&a, &C->fb);
        tk_bmp_down2(&q, &a);
        tk_bmp_blur(&q, PT(22) / 4);
        tk_bmp_satbright(&q, 1.3f, 1);
        /* up to full (bilinear, via the glass module's helper: blur 0 is a copy) */
        for (int j = 0; j < TK_H; j++)
            for (int i = 0; i < TK_W; i++) {
                float fx = (i + .5f) / 4 - .5f, fy = (j + .5f) / 4 - .5f;
                int x0 = (int)tk_floorf(fx), y0 = (int)tk_floorf(fy);
                float tx = fx - x0, ty = fy - y0;
                int xa = tk_maxi(0, x0), xb = tk_mini(q.w - 1, x0 + 1), ya = tk_maxi(0, y0), yb = tk_mini(q.h - 1, y0 + 1);
                uint32_t p00 = q.px[ya * q.w + xa], p01 = q.px[ya * q.w + xb], p10 = q.px[yb * q.w + xa], p11 = q.px[yb * q.w + xb], o = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    float v = ((p00 >> sh & 255) * (1 - tx) + (p01 >> sh & 255) * tx) * (1 - ty) + ((p10 >> sh & 255) * (1 - tx) + (p11 >> sh & 255) * tx) * ty;
                    o |= (uint32_t)(int)(v + .5f) << sh;
                }
                scrim.bmp.px[j * TK_W + i] = o;
            }
        tk_bmp_free(&a);
        tk_bmp_free(&q);
        tk_paint t = tk_solid(TK_RGBA(4, 8, 26, .28f));
        tk_fill_rrect(&scrim.bmp, -2, -2, TK_W + 4, TK_H + 4, 0, &t, 1);
    }
    /* the card: its backdrop is the scrim filtered again (blur 14, saturate 1.9, brightness 1.06), taken once at
     * rest and moved with the card while it springs in (Chrome filters the live scene under it each frame) */
    float sy = 0;
    for (int pass = 0; pass < 1; pass++) {
        tk_layer *L = &fcard;
        relayer(L);
        dc d = mk(L, pass ? "fcard-moving" : "fcard", 14, 196, 362, 267, 56, 0);
        if (!pass) {
            tk_bmp crop;
            tk_bmp_new(&crop, d.b->w, d.b->h);
            tk_bmp_copy(&crop, 0, 0, &scrim.bmp, (int)d.ox, (int)d.oy, crop.w, crop.h);
            tk_glass_local(d.b, &crop, PT(14) - d.ox, PT(196) - d.oy, PT(362), PT(267), PT(32));
            tk_bmp_free(&crop);
        }
        d_glass(&d, 14, 196, 362, 267, 32, 0, 1);
        char tn[40] = "t66-";
        tk_memcpy(tn + 4, s->k, tk_strlen(s->k) + 1);
        d_icon(&d, tn, 34, 216, 1);
        d_text(&d, TK_IT600, 13, 114, 225.16f + 13, gr->name, 0, 0, W(.62f), 0);
        d_text(&d, TK_IT800, 27, 114, base_of(TK_IT800, 27, 243.16f, 29.7f), s->name, -.6f, 0, INK, 0);
        int last = !tk_strcmp(s->k, LAST);
        const char *dt[3] = { "Version", last ? "Last used" : "When it shuts down", "When it shuts down" };
        const char *dd[3] = { s->meta, last ? "Today" : "Back to Starbite", "Back to Starbite" };
        int nf = last ? 3 : 2;
        float dtw = 0;
        for (int i = 0; i < nf; i++) dtw = tk_maxf(dtw, tw(TK_IT400, 14.5f, dt[i], 0, 0));
        for (int i = 0; i < nf; i++) {
            float b = 298 + 24 * i + 14;
            d_text(&d, TK_IT400, 14.5f, 34, b, dt[i], 0, 0, W(.55f), 0);
            d_text(&d, TK_IT400, 14.5f, 34 + dtw + 14, b, dd[i], 0, TK_TNUM, INK, 0);
        }
        sy = 298 + 17 * nf + 7 * (nf - 1) + 18;
        /* the slider: dark glass whose backdrop is the card under it */
        tk_glass_local(d.b, d.b, PT(34) - d.ox, PT(sy) - d.oy, PT(322), PT(62), PT(31));
        d_glass(&d, 34, sy, 322, 62, 31, 1, 1);
    }
    relayer(&ffill);
    focus_sy = sy;
    dc f = mk(&ffill, "fill", 34, sy, 62, 62, 1, 0);
    d_rrect(&f, 34, sy, 62, 62, 31, TK_RGBA(111, 179, 255, .28f));
    ffill.slice = 2;
    ffill.slice_s0 = PT(32);
    ffill.slice_s1 = PT(32);
    relayer(&flabel);
    dc l = mk(&flabel, "label", 34, sy, 322, 62, 1, 0);
    {
        char lbl[96] = "slide to start ";
        tk_memcpy(lbl + 15, s->name, tk_strlen(s->name) + 1);
        float w = tw(TK_IT600, 16.5f, lbl, 0, 0);
        /* background: 90 deg .4 -> 1 @45 % -> .4 @60 % over 200 % of the box, position 0 (the shine at rest) */
        tk_stop st[3] = { { 0, W(.4f) }, { .45f, W(1) }, { .6f, W(.4f) } };
        tk_paint p = tk_linear(90, PT(34) - l.ox, PT(sy) - l.oy, PT(644), PT(62), 3, st);
        tk_text(l.b, TK_IT600, PT(16.5f), PT(34 + 44 + (322 - 44 - w) / 2) - l.ox, PT(base_of(TK_IT600, 16.5f, sy + 21, 0)) - l.oy, lbl, 0, 0, &p, 0);
    }
    relayer(&fthumb);
    dc t = mk(&fthumb, "thumb", 39, sy + 5, 52, 52, 20, 0);
    tk_box_shadow(t.b, PT(39) - t.ox, PT(sy + 5) - t.oy, PT(52), PT(52), PT(26), 0, PT(2), PT(12), 0, TK_RGBA(0, 0, 0, .35f), 0);
    d_rrect(&t, 39, sy + 5, 52, 52, 26, W(1));
    d_icon(&t, "arrow", 54, sy + 20, 1);
    tk_layer_init(&fgrp, "fgrp");
    fgrp.ox = PT(195);
    fgrp.oy = PT(196 + .4f * 267);
    tk_layer_add(&fgrp, &fcard);
    tk_layer_add(&fgrp, &ffill);
    tk_layer_add(&fgrp, &flabel);
    tk_layer_add(&fgrp, &fthumb);
    focus.child = 0;
    tk_layer_add(&focus, &scrim);
    tk_layer_add(&focus, &fgrp);
    focus_sys = s;
    thumb_x = 0;
    tk_layer_prepare(&scrim);
    tk_layer_prepare(&fcard);
    tk_layer_prepare(&ffill);
    tk_layer_prepare(&flabel);
    tk_layer_prepare(&fthumb);
}
static void set_thumb(float x)
{
    thumb_x = x;
    fthumb.x = fthumb.bx + PT(x);
    ffill.slice_h = PT(64 + x);
}
static void set_focus(float p)
{
    focus_p = p;
    scrim.alpha = tk_minf(1, tk_maxf(0, p));
    fgrp.alpha = tk_minf(1, tk_maxf(0, p * 1.5f));
    fgrp.sx = fgrp.sy = .9f + .1f * p;
    fgrp.y = PT((1 - p) * 40) / fgrp.sy;
    focus.hidden = !focus_on;
}

/* the lock as one cached layer for the unlock (its opacity applies to it as a whole, as CSS group opacity), made
 * while nothing moves so the swipe's first frame does not pay for it */
static void flatten_lock(void)
{
    if (flat_ver == lock_ver) return;
    tk_flatten(&lockg, &lockflat.bmp, &lockflat.glass);
    flat_ver = lock_ver;
    tk_layer_dirty(&lockflat);
    tk_layer_prepare(&lockflat);
    tk_layer_prepare(&home);
}

/* ---- the unlock -------------------------------------------------------------------------------------------- */
static void set_lock(float y)
{
    lockY = y;
    float p = tk_clampf(-y / H_PT, 0, 1);
    float la = 1 - tk_maxf(0, p - .55f) * 2.2f;
    home.alpha = tk_minf(1, p * 1.6f);
    home.sx = home.sy = .92f + .08f * tk_minf(1, p * 1.3f);
    mark_t = p > .02f ? 0 : 1;
    glyph_t = p > .02f ? 1 : 0;
    glyph_open = p > .06f;
    if (y == 0 && la >= 1) {
        lockg.hidden = 0;
        lockflat.hidden = 1;
    } else {
        flatten_lock();
        lockg.hidden = 1;
        lockflat.hidden = la <= 0;
        lockflat.y = PT(y);
        lockflat.alpha = tk_maxf(0, la);
    }
}

/* ---- init and states --------------------------------------------------------------------------------------- */
static void lap_us(const char *what, uint64_t us);
static uint64_t tb;
static void lap(const char *what)
{
    uint64_t t = tk_us();
    char b[64];
    int n = 0;
    while (*what) b[n++] = *what++;
    b[n++] = ' ';
    uint64_t ms = (t - tb) / 1000;
    char d[20];
    int k = 0;
    do { d[k++] = (char)('0' + ms % 10); ms /= 10; } while (ms);
    while (k) b[n++] = d[--k];
    b[n++] = 'm'; b[n++] = 's'; b[n] = 0;
    tk_log(b);
    tb = t;
}

static void lap_us(const char *what, uint64_t us)
{
    char b[80];
    int n = 0;
    while (*what) b[n++] = *what++;
    b[n++] = ' ';
    uint64_t ms = us / 1000;
    char d[20];
    int k = 0;
    do { d[k++] = (char)('0' + ms % 10); ms /= 10; } while (ms);
    while (k) b[n++] = d[--k];
    b[n++] = 'm'; b[n++] = 's'; b[n] = 0;
    tk_log(b);
}

int sh_init(tk_comp *c)
{
    C = c;
    tb = tk_us();
    build_wall();
    lap("bake: wallpaper + glass backdrop");
    tk_layer_init(&root, "root");
    build_home();
    lap("bake: home");
    build_lstatic();
    build_marks();
    lap("bake: lock static");
    tk_layer_init(&list, "list");
    list.clip = 1;
    list.fade_top = PT(18);
    list.fade_bot = PT(14);
    layout_list();
    for (int g = 0; g < NG; g++) build_card(g);
    build_etool();
    lap("bake: cards");
    tk_layer_add(&list, &etool);
    build_hold(0);
    build_hold(1);
    lap("bake: holds");
    tk_layer_init(&lockg, "lock");
    tk_layer_add(&lockg, &lstatic);
    tk_layer_add(&lockg, &mark);
    tk_layer_add(&lockg, &glyph_c);
    tk_layer_add(&lockg, &glyph_o);
    tk_layer_add(&lockg, &list);
    tk_layer_add(&lockg, &Q[0].grp);
    tk_layer_add(&lockg, &Q[1].grp);
    tk_layer_add(&lockg, &hint);
    tk_layer_init(&lockflat, "lock-flat");
    tk_bmp_new(&lockflat.bmp, TK_W, TK_H);
    tk_a8_new(&lockflat.glass, TK_W, TK_H);
    lockflat.glass_opaque_only = 1;
    lockflat.hidden = 1;
    tk_layer_init(&focus, "focus");
    focus.hidden = 1;
    tk_layer_init(&boot, "boot");
    boot.hidden = 1;
    tk_layer_add(&root, &home);
    tk_layer_add(&root, &lockg);
    tk_layer_add(&root, &lockflat);
    tk_layer_add(&root, &focus);
    tk_layer_add(&root, &boot);
    tk_layer_add(&root, &homebar);
    c->root = &root;
    glyph_c.alpha = glyph_o.alpha = 0;
    hint.alpha = 0;
    place_list();
    set_lock(0);
    return 1;
}

static void apply_tweens(void)
{
    mark.alpha = mark_a;
    glyph_c.alpha = glyph_open ? 0 : glyph_a;
    glyph_o.alpha = glyph_open ? glyph_a : 0;
    hint.alpha = hint_a;
}

void sh_set(const char *what, const char *key, float v)
{
    if (!tk_strcmp(what, "unlock")) {
        sp_lock.live = 0;
        set_lock(-H_PT * v);
        locked = v < 1;
        mark_a = mark_t;
        glyph_a = glyph_t;
        apply_tweens();
    } else if (!tk_strcmp(what, "group")) {
        for (int g = 0; g < NG; g++) {
            int want = key && !tk_strcmp(G[g].key, key);
            if (K[g].isopen != want) toggle_group(g, 1);
        }
    } else if (!tk_strcmp(what, "scroll")) {
        scroll = v;
        place_list();
    } else if (!tk_strcmp(what, "focus")) {
        for (int g = 0; g < NG; g++)
            for (int i = 0; i < G[g].n; i++)
                if (!tk_strcmp(G[g].it[i].k, key)) {
                    focus_on = 1;
                    build_focus(&G[g].it[i], &G[g]);
                    set_focus(v);
                }
    } else if (!tk_strcmp(what, "hint")) {
        hint_a = v;
        apply_tweens();
    } else if (!tk_strcmp(what, "hold")) {
        int i = key && key[0] == 'e';
        Q[i].grp.sx = Q[i].grp.sy = v > 0 ? 1.22f : 1;
        hold_ring(&Q[i], i, v);
        lock_ver++;
    }
}

/* ---- gestures ---------------------------------------------------------------------------------------------- */
enum { G_NONE, G_LOCK, G_LIST, G_TAP_HEAD, G_TAP_ROW, G_HOLD, G_THUMB, G_SCRIM };
static struct {
    int kind, arg, arg2, moved;
    float x0, y0, base, t0;
    tk_tracker tr;
} T;

static int hit_head(float x, float y)
{
    if (y < LIST_TOP || y > LIST_TOP + LIST_H) return -1;
    float cy = y - LIST_TOP + scroll;
    for (int g = 0; g < NG; g++) {
        float top = 10, yy = 10;
        for (int h = 0; h < g; h++) yy += 68 + K[h].memh + (stack(h) ? 22 : 10);
        top = yy;
        if (cy >= top && cy < top + 68 && x >= 14 && x < 376) return g;
    }
    return -1;
}
static int hit_row(float x, float y, int *gi)
{
    if (y < LIST_TOP || y > LIST_TOP + LIST_H || x < 20 || x > 370) return -1;
    float cy = y - LIST_TOP + scroll, yy = 10;
    for (int g = 0; g < NG; g++) {
        if (K[g].memh > 1)
            for (int i = 0; i < G[g].n; i++) {
                float rt = yy + 68 + 60 * i;
                if (cy >= rt && cy < rt + 56 && cy < yy + 68 + K[g].memh) { *gi = g; return i; }
            }
        yy += 68 + K[g].memh + (stack(g) ? 22 : 10);
    }
    return -1;
}
static float rubber(float over, float dim) { return (1 - 1 / (over * .55f / dim + 1)) * dim; }

void sh_touch(int phase, float x, float y, float t)
{
    now_ms = t;
    if (phase == 0) {
        tk_tracker_reset(&T.tr);
        T.moved = 0;
        T.x0 = x;
        T.y0 = y;
        T.t0 = t;
        T.kind = G_NONE;
        if (focus_on) {
            float sy = focus_sy;
            if (x >= 39 + thumb_x && x <= 91 + thumb_x && y >= sy + 5 && y <= sy + 57) { T.kind = G_THUMB; T.base = thumb_x; sp_thumb.live = 0; }
            else if (y < 196 || y > 463) T.kind = G_SCRIM;
            return;
        }
        if (!locked) return;
        for (int i = 0; i < 2; i++) {
            float bx = i ? 292 : 46;
            if (x >= bx && x <= bx + 52 && y >= 694 && y <= 746) {
                T.kind = G_HOLD;
                T.arg = i;
                Q[i].down = 1; Q[i].fired = 0; Q[i].t0 = t;
                Q[i].scale_from = Q[i].grp.sx; Q[i].scale_to = 1.22f; Q[i].scale_t = 0;
                return;
            }
        }
        int gi = -1, r;
        if (y >= H_PT - 30) { T.kind = G_LOCK; T.base = lockY; sp_lock.live = 0; }
        else if ((r = hit_row(x, y, &gi)) >= 0) { T.kind = G_TAP_ROW; T.arg = gi; T.arg2 = r; }
        else if ((r = hit_head(x, y)) >= 0) { T.kind = G_TAP_HEAD; T.arg = r; }
        else if (y >= LIST_TOP && y <= LIST_TOP + LIST_H) { T.kind = G_LIST; T.base = scroll; sp_scroll.live = 0; scroll_momentum = 0; }
        else { T.kind = G_LOCK; T.base = lockY; sp_lock.live = 0; }
        tk_tracker_add(&T.tr, y, t);
        return;
    }
    if (phase == 1) {
        float dy = y - T.y0;
        if (tk_fabsf(dy) > 5 || tk_fabsf(x - T.x0) > 5) T.moved = 1;
        switch (T.kind) {
        case G_THUMB:
            tk_tracker_add(&T.tr, x, t);
            set_thumb(tk_clampf(T.base + x - T.x0, 0, 322 - 62));
            break;
        case G_TAP_HEAD: case G_TAP_ROW:
            if (T.moved) { T.kind = G_LIST; T.base = scroll; T.y0 = y; sp_scroll.live = 0; }
            tk_tracker_add(&T.tr, y, t);
            break;
        case G_LIST: {
            tk_tracker_add(&T.tr, y, t);
            float s = T.base - dy, m = max_scroll();
            if (s < 0) s = -rubber(-s, LIST_H);
            else if (s > m) s = m + rubber(s - m, LIST_H);
            scroll = s;
            place_list();
            break;
        }
        case G_LOCK: {
            tk_tracker_add(&T.tr, y, t);
            float d = dy + T.base;
            if (d > 0) d *= .22f;
            set_lock(d);
            break;
        }
        }
        return;
    }
    /* up */
    switch (T.kind) {
    case G_THUMB:
        if (thumb_x > (322 - 62) * .86f) { set_thumb(322 - 62); booting = 1; boot_t = -100; }
        else tk_spring_start(&sp_thumb, thumb_x, 0, 0, TK_SPR_SHEET);
        break;
    case G_SCRIM:
        focus_closing = 1;
        tk_spring_start(&sp_focus, focus_p, 0, 0, TK_SPR_SHEET);
        break;
    case G_HOLD: {
        hold_t *q = &Q[T.arg];
        q->down = 0;
        q->scale_from = q->grp.sx; q->scale_to = 1; q->scale_t = 0;
        hold_ring(q, T.arg, 0);
        break;
    }
    case G_TAP_HEAD:
        toggle_group(T.arg, 0);
        break;
    case G_TAP_ROW: {
        const sys_t *s = &G[T.arg].it[T.arg2];
        if (s->self) break;           /* the concept's toast: "You are in Starbite OS" */
        tk_comp_frame(C, 0, 0);
        uint64_t t0 = tk_us();
        build_focus(s, &G[T.arg]);
        lap_us("focus: snapshot and bake", tk_us() - t0);
        focus_on = 1;
        focus_closing = 0;
        set_focus(0);
        tk_spring_start(&sp_focus, 0, 1, 0, TK_SPR_SHEET);
        break;
    }
    case G_LIST: {
        float m = max_scroll();
        if (scroll < 0 || scroll > m) tk_spring_start(&sp_scroll, scroll, scroll < 0 ? 0 : m, 0, TK_SPR_ROWS);
        else { scroll_v = -tk_tracker_v(&T.tr); scroll_momentum = tk_fabsf(scroll_v) > .02f; }
        break;
    }
    case G_LOCK:
        if (!T.moved) {
            hint_left = 2200;
            tk_spring_start(&sp_lock, 0, 0, -700, TK_SPR_UNLOCK);
        } else {
            float v = tk_tracker_v(&T.tr), proj = lockY + v * 200;
            if (proj < -H_PT * .38f && locked) tk_spring_start(&sp_lock, lockY, -H_PT, v * 1000, TK_SPR_UNLOCK);
            else tk_spring_start(&sp_lock, lockY, 0, v * 1000, TK_SPR_UNLOCK);
        }
        break;
    }
    T.kind = G_NONE;
}

/* ---- time ------------------------------------------------------------------------------------------------- */
static int tween(float *v, float target, float dur_s, float dt)
{
    if (*v == target) return 0;
    float step = dt / dur_s;
    if (*v < target) *v = tk_minf(target, *v + step); else *v = tk_maxf(target, *v - step);
    return 1;
}

int sh_tick(float dt, float t)
{
    int mv = 0;
    now_ms = t;
    if (sp_lock.live) {
        int live = tk_spring_step(&sp_lock, dt);
        set_lock(sp_lock.x);
        if (!live && sp_lock.to <= -H_PT + .01f) { locked = 0; lockflat.hidden = 1; home.sx = home.sy = 1; }
        mv = 1;
    }
    if (T.kind == G_LOCK || T.kind == G_LIST || T.kind == G_THUMB) mv = 1;
    /* the mark (.3 s), the glyph (.2 s) and the hint (.3 s): linear stand-ins for their CSS ease */
    mv |= tween(&mark_a, mark_t, .3f, dt);
    mv |= tween(&glyph_a, glyph_t, .2f, dt);
    if (hint_left > 0) { hint_left -= dt * 1000; mv |= tween(&hint_a, 1, .3f, dt); }
    else mv |= tween(&hint_a, 0, .3f, dt);
    apply_tweens();
    for (int g = 0; g < NG; g++) {
        card_t *k = &K[g];
        if (k->sp.live) {
            int live = tk_spring_step(&k->sp, dt);
            k->memh = tk_maxf(0, k->sp.x);
            if (!live && k->isopen) {
                /* the concept: when open, scroll the card into view (rows spring) */
                float top = k->y - 8, m = max_scroll();
                if (k->y + 68 + k->full > scroll + LIST_H) tk_spring_start(&sp_scroll, scroll, tk_minf(top, m), 0, TK_SPR_ROWS);
            }
            mv = 1;
            place_list();
        }
        if (k->chev_t < 1) {
            k->chev_t = tk_minf(1, k->chev_t + dt / .35f);
            k->chev_now = k->chev_from + (k->chev_to - k->chev_from) * tk_bezier(.3f, 1.4f, .4f, 1, k->chev_t);
            chev_draw(k, k->chev_now);
            mv = 1;
            if (k->chev_t >= 1) place_list();
        }
    }
    if (sp_scroll.live) {
        tk_spring_step(&sp_scroll, dt);
        scroll = sp_scroll.x;
        place_list();
        mv = 1;
    } else if (scroll_momentum) {
        /* iOS deceleration: v x 0.998 per ms; past an end, a spring back */
        float ms = dt * 1000, m = max_scroll();
        float decay = tk_powf(.998f, ms);
        scroll += scroll_v * (1 - decay) / (1 - .998f);
        scroll_v *= decay;
        if (scroll < 0 || scroll > m) {
            scroll_momentum = 0;
            tk_spring_start(&sp_scroll, scroll, scroll < 0 ? 0 : m, scroll_v * 1000, TK_SPR_ROWS);
        } else if (tk_fabsf(scroll_v) < .01f) scroll_momentum = 0;
        place_list();
        mv = 1;
    }
    for (int i = 0; i < 2; i++) {
        hold_t *q = &Q[i];
        if (q->scale_t < 1) {
            q->scale_t = tk_minf(1, q->scale_t + dt / .25f);
            q->grp.sx = q->grp.sy = q->scale_from + (q->scale_to - q->scale_from) * tk_bezier(.3f, 1.5f, .4f, 1, q->scale_t);
            mv = 1;
        }
        if (q->down && !q->fired) {
            float p = tk_minf(1, (t - q->t0) / q->ms);
            hold_ring(q, i, p);
            if (p >= 1) {
                q->fired = 1;
                q->lit = !q->lit;
                q->facelit.hidden = q->iconlit.hidden = !q->lit;
                q->face.hidden = q->icon.hidden = q->lit;
                hold_ring(q, i, 0);
            }
            mv = 1;
        }
    }
    if (focus_on) {
        if (sp_focus.live) {
            int live = tk_spring_step(&sp_focus, dt);
            set_focus(tk_maxf(0, sp_focus.x));
            if (!live && focus_closing) { focus_on = 0; focus.hidden = 1; }
            mv = 1;
        }
        if (sp_thumb.live) { tk_spring_step(&sp_thumb, dt); set_thumb(sp_thumb.x); mv = 1; }
    }
    if (booting) {
        boot_t += dt * 1000;
        if (boot_t >= 0) {
            if (!boot.bmp.px) {
                dc d = mk(&boot, "boot", 0, 0, 390, H_PT, 0, 0);
                tk_fill_rect_raw(d.b, 0, 0, d.b->w, d.b->h, 0xff000000u);
                char tn[40] = "t66-";
                tk_memcpy(tn + 4, focus_sys->k, tk_strlen(focus_sys->k) + 1);
                d_icon(&d, tn, 162, 315, 1);
                char s[96] = "Starting ";
                tk_memcpy(s + 9, focus_sys->name, tk_strlen(focus_sys->name) + 1);
                float w = tw(TK_IT700, 22, s, 0, 0);
                d_text(&d, TK_IT700, 22, (390 - w) / 2, base_of(TK_IT700, 22, 416, 0), s, 0, 0, INK, 0);
                d_rrect(&d, 120, 465, 150, 4, 2, tk_hex(0x222222, 1));
                dc e = mk(&bootbar, "bootbar", 120, 465, 4, 4, 0, 0);
                d_rrect(&e, 120, 465, 4, 4, 2, W(1));
                bootbar.slice = 2;
                bootbar.slice_s0 = bootbar.slice_s1 = PT(2);
                boot.child = &bootbar;
            }
            /* the bar: width 0 -> 100 % over 2.1 s, CSS ease, starting with the overlay */
            bootbar.slice_h = PT(4 + 146 * tk_bezier(.25f, .1f, .25f, 1, tk_minf(1, boot_t / 2100)));
            boot.hidden = 0;
            boot_a = tk_minf(1, boot_t / 350);
            boot.alpha = boot_a;
            mv = 1;
            if (boot_t > 2400) booting = 0;
        }
    }
    return mv;
}

void sh_idle(void)
{
    if (sh_moving()) return;
    if (locked && lockY == 0 && !focus_on) flatten_lock();
    tk_comp_prepare(C);
}

int sh_moving(void)
{
    int m = sp_lock.live || sp_scroll.live || scroll_momentum || T.kind == G_LOCK || T.kind == G_LIST || T.kind == G_THUMB || booting;
    for (int g = 0; g < NG; g++) m |= K[g].sp.live || K[g].chev_t < 1;
    m |= sp_focus.live || sp_thumb.live;
    return m;
}
