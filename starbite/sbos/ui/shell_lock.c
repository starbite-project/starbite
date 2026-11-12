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
#include "sbtk_colors.h"                            /* GAP-LIST #1: the system palette (iOS 26 dark) */

/* helper B2: Settings > Sounds & Haptics (shell_settings.inc): System Haptics gates every light tap, Keyboard Feedback
 * the keys' (kb_hit_r marks a key's tap in kb_key_tap; the sh_haptic() after it reads and clears it) */
static int kb_key_tap;
static int se_haptic_ok(int key);
#define sh_haptic() do { int kk_ = kb_key_tap; kb_key_tap = 0; if (se_haptic_ok(kk_)) sh_haptic(); } while (0)

#define H_PT TK_HPT
static tk_comp *C;
/* the frame on the panel now, for screen snapshots (scrims, blurs): the GPU row draws into the scanout buffer, so its
 * RAM frame can be stale; its sbtk.h defines TK_COMP_SHOWN with tk_comp_shown(). On the CPU path it is the RAM frame */
#ifdef TK_COMP_SHOWN
static const tk_bmp *shown_frame(void) { return tk_comp_shown(C); }
#else
static const tk_bmp *shown_frame(void) { return &C->fb; }
#endif

/* P3p (audit 15): the safe margins. The S9's long edges curve away from x < 72 px and x > 1368 px (rims and
 * highlights bend and dim there): every card, group, dock and sheet keeps MARGIN pt from the sides (iOS: 16 pt on a
 * flat panel, 20 on the large phones), and its corners stay concentric with the display's (S9_CORNER_PT ~ 118 px) */
#define MARGIN 20.0f
#define CONTENT_W (390 - 2 * MARGIN)
#define CONTENT_R (390 - MARGIN)                       /* the content's right edge */
#define S9_CORNER_PT 32.0f
#define CONCENTRIC(inset) tk_maxf(S9_CORNER_PT - (inset), 24)
#define INTER_CAP .727f                               /* Inter's cap (and figure) height per em */
/* the lock's cards: the card, the lower cards of a stack 10 / 20 pt in from it, the head's icon / title / names */
#define CARD_X MARGIN
#define CARD_W CONTENT_W
#define CARD_ICON (CARD_X + 11)
#define CARD_TX (CARD_X + 69)
#define CARD_SX (CARD_X + 81)
#define CARD_CHEV (CARD_X + CARD_W - 32)
/* the inset-grouped lists (Settings, Notes, the Phone's tab bar): the group and its rows' text */
#define GROUP_X MARGIN
#define GROUP_W CONTENT_W
#define ROW_TX (MARGIN + 16)
#define TAB_W (GROUP_W / 4)
/* P3p (P3j-5): iOS's header bands under the status bar: the nav row (44 pt: its items centred on NAV_MID, 17 pt
 * text on NAV_BASE), then the large title row (52 pt; 34 pt Bold on LARGE_BASE); a page's content from
 * CONTENT_TOP, a pushed page's (inline title, no large one) from PUSHED_TOP */
#define SAFE_TOP 44.0f
#define NAVBAR_H 44.0f
#define LARGE_H 52.0f
#define NAV_MID (SAFE_TOP + NAVBAR_H / 2)
#define NAV_BASE (SAFE_TOP + 28.2f)
#define LARGE_BASE (SAFE_TOP + NAVBAR_H + 36)
#define CONTENT_TOP (SAFE_TOP + NAVBAR_H + LARGE_H + 10)
#define PUSHED_TOP (SAFE_TOP + NAVBAR_H + 16)
#define IN_NAV(y) ((y) >= SAFE_TOP - 4 && (y) < SAFE_TOP + NAVBAR_H)
/* B9: the lock screen as iOS 26 lays it out (IOS-REFERENCE 9.1 / 5 / 9.3, measured on the WWDC25 hero): the widgets
 * row at y 214 (72 tall); the notifications bottom-anchored on y 677 (20.5 above the buttons); the torch / Emergency
 * buttons 50 pt glass circles centred at x 71 / 319, y 727. B10 (the user, 2026-11-12: "it used to be notification
 * style"): the vendor cards are back under the widgets (the list from LIST_TOP), as notification platters */
#define WROW_Y 214.0f
#define PL_X 14.5f
#define PL_W 361.0f
#define PL_R 23.0f
#define LKN_TOP 310.0f                               /* the notifications' area fanned out (the list's top: LIST_TOP) */
#define LKN_BOT 677.0f
/* B10: a vendor card is an iOS platter (PL_X / PL_W / PL_R, the notifications' glass): its icon, name, systems and
 * chevron where B4's cards had them (11 / 69 / 81 in, the chevron 32 from the right) */
#define VC_X PL_X
#define VC_W PL_W
#define VC_R PL_R
#define VC_ICON (VC_X + 11)
#define VC_TX (VC_X + 69)
#define VC_SX (VC_X + 81)
#define VC_CHEV (VC_X + VC_W - 32)

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
    float x0 = tk_maxf(PT(x - mg), 0), y0 = PT(y - mg);       /* nothing beyond the panel's sides is ever shown */
    float x1 = tk_minf(PT(x + w + mg), (float)TK_W), y1 = PT(y + h + mg);
    int X = (int)tk_floorf(x0), Y = (int)tk_floorf(y0);
    tk_bmp_new(&l->bmp, (int)(x1 - X) + 2, (int)(y1 - Y) + 2);    if (glass) tk_a8_new(&l->glass, l->bmp.w, l->bmp.h);
    l->x = l->bx = (float)X;
    l->y = l->by = (float)Y;
    dc d = { &l->bmp, glass ? &l->glass : 0, (float)X, (float)Y };
    return d;
}
static void d_text(dc *d, int f, float size, float x, float base, const char *s, float ls, int fl, tk_col c, int shadow)
{
    float by = PT(base) - d->oy;                /* nothing of it in this bitmap (a strip of a page): nothing to do */
    if (by + PT(size * .6f) < 0 || by - PT(size * 1.3f) > d->b->h) return;
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
    /* P4: the compositor's lens: the shape on the layer (a mask is always a layer's glass, mk) */
    if (d->m) tk_layer_glass_shape((tk_layer *)(void *)((char *)d->m - __builtin_offsetof(tk_layer, glass)),
                                   PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r));
}
static void d_rrect(dc *d, float x, float y, float w, float h, float r, tk_col c)
{
    tk_paint p = tk_solid(c);
    tk_fill_rrect(d->b, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r), &p, 1);
}
__attribute__((unused)) static void d_ring(dc *d, float cx, float cy, float r, float sw, float frac, tk_col c, int caps)
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

static void relayer(tk_layer *l) { tk_layer_dirty(l); tk_bmp_free(&l->bmp); tk_a8_free(&l->glass); }
/* P3s (the 160 MiB heap): a drawn layer's bitmap and glass mask cut to what they hold, a few px of nothing kept round
 * it (the bilinear neighbours of a scaled draw stay inside), every cut a whole even number of px (the half copy's
 * 2 x 2 cells and the 3-slice rows stay where they were); the layer moves by the cut, so it shows exactly what it did.
 * The bitmap keeps its block, made smaller (tk_shrink: no copy, no hole). Returns the px cut off the top and the
 * bottom (a 3-slice's lengths shrink by them), or 0 if it was not cut. */
static int trim_layer(tk_layer *l, int *cut_top, int *cut_bot)
{
    tk_bmp *b = &l->bmp;
    if (cut_top) *cut_top = 0;
    if (cut_bot) *cut_bot = 0;
    if (!b->px || b->stride != b->w || (l->glass.v && (l->glass.w != b->w || l->glass.h != b->h))) return 0;
    int w = b->w, h = b->h, x0 = w, x1 = 0, y0 = h, y1 = 0;
    for (int j = 0; j < h; j++) {
        const uint32_t *r = b->px + (size_t)j * w;
        const uint8_t *g = l->glass.v ? l->glass.v + (size_t)j * w : 0;
        int a = 0, e = w;
        while (a < e && !r[a] && !(g && g[a])) a++;
        if (a == e) continue;
        while (e > a && !r[e - 1] && !(g && g[e - 1])) e--;
        if (a < x0) x0 = a;
        if (e > x1) x1 = e;
        if (j < y0) y0 = j;
        y1 = j + 1;
    }
    /* P4 (the GPU row): its glass shapes stay inside (a shape's middle may hold nothing once its mask is freed) */
    for (int q = 0; q < l->ngsh; q++) {
        const float *g = l->gsh[q];
        x0 = tk_mini(x0, tk_maxi(0, (int)tk_floorf(g[0]) - 1)); x1 = tk_maxi(x1, tk_mini(w, (int)(g[0] + g[2]) + 2));
        y0 = tk_mini(y0, tk_maxi(0, (int)tk_floorf(g[1]) - 1)); y1 = tk_maxi(y1, tk_mini(h, (int)(g[1] + g[3]) + 2));
    }
    if (x1 <= x0) return 0;                       /* nothing in it: left as it is */
    /* sideways by whole 16 px (8 half px): the compositor's 8 px blocks fall on the same px, so the same blends
     * round the same way; 16 px of nothing kept on the right (the row's last few px blend one at a time) */
    x0 = tk_maxi(0, x0 - 16) & ~15;
    y0 = tk_maxi(0, y0 - 4) & ~1;
    x1 = tk_mini(w, x1 + 16);
    y1 = tk_mini(h, y1 + 4);
    if ((w - x1) & 1) x1++;                       /* what is cut on the right and at the bottom even too */
    if ((h - y1) & 1) y1++;
    int nw = x1 - x0, nh = y1 - y0;
    if ((size_t)nw * nh * 4 + (64u << 10) > (size_t)w * h * 4) return 0;   /* not worth it */
    for (int j = 0; j < nh; j++) tk_memcpy(b->px + (size_t)j * nw, b->px + (size_t)(y0 + j) * w + x0, (size_t)nw * 4);
    b->px = tk_shrink(b->px, (size_t)nw * nh * 4);
    b->w = b->stride = nw;
    b->h = nh;
    if (l->glass.v) {
        for (int j = 0; j < nh; j++) tk_memcpy(l->glass.v + (size_t)j * nw, l->glass.v + (size_t)(y0 + j) * w + x0, (size_t)nw);
        l->glass.v = tk_shrink(l->glass.v, (size_t)nw * nh);
        l->glass.w = nw;
        l->glass.h = nh;
    }
    l->x += (float)x0; l->bx += (float)x0;
    l->y += (float)y0; l->by += (float)y0;
    tk_layer_dirty(l);
    for (int q = 0; q < l->ngsh; q++) { l->gsh[q][0] -= (float)x0; l->gsh[q][1] -= (float)y0; }   /* P4: with the cut */
    if (cut_top) *cut_top = y0;
    if (cut_bot) *cut_bot = h - y1;
    return 1;
}
static void trim(tk_layer *l) { trim_layer(l, 0, 0); }

/* ---- state ------------------------------------------------------------------------------------------------- */
static tk_layer root, home, lockflat, lockg, mark, glyph_c, glyph_o, list, etool, hint, homebar;
static tk_layer boot, bootc, bootbar;
typedef struct {
    tk_layer face, facelit, icon, iconlit, ring, grp;
    tk_bmp ringbuf;
    float *ang, *cov;           /* the ring's per-pixel angle and radial coverage */
    float scale_from, scale_to, scale_t;
    tk_spring ss;               /* B4: the scale on the press spring (response .30, damping .6), velocity kept on a retarget */
    int down, fired, lit;
    float t0, prog, ms;
} hold_t;
static hold_t Q[2];
/* B9: the lock's one platter glass (iOS 26's notification platter: x 14.5, w 361, r 23) and the stack's lower cards
 * under it, baked once into K[0] (K[1..] stay empty): the vendor cards (B10), their members' platters, the lock's
 * notifications and the Notification Centre's platters (views, shell_nc.inc) all draw it as a 3-slice at any height */
typedef struct {
    tk_layer gh, face;          /* gh: the stack's lower cards (P3i) */
    float fcut;                 /* P3s: px cut off the face's top and bottom (its 3-slice is that much shorter) */
    float y;                    /* where it was baked: LIST_TOP + y (pt) */
} card_t;
static card_t K[NG];
/* B10: the lock's vendor cards (B4 / P3i's, back over B9's tiles). The glass is K[0]'s (face and gh are views of it:
 * nothing of a card's glass is baked per card); a card's own pixels are its head (icon, name, systems), its chevron
 * and, while it is open, its member rows and "Show less" */
typedef struct {
    tk_layer gh, face, head, chev, members, rows[3];   /* gh: the stack's lower cards (a view of K[0].gh) */
    tk_layer mpl[3], sl;        /* B4 (P3j-8): each member's own platter (a view of the face), the "Show less" pill */
    tk_bmp chev0;               /* the chevron unrotated */
    float y;                    /* top in content pt (scroll 0, every card closed): where its head was drawn */
    float chx;                  /* the chevron's x */
    int isopen;
    float memh, full;           /* members' height now and open */
    tk_spring sp;
    float from, to;             /* the spring's ends (rows' p) */
    float chev_t, chev_from, chev_to, chev_now;   /* the chevron's .35 s transition */
} vcard_t;
static vcard_t V[NG];

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
static int EMG;                 /* Emergency mode (the concept's em-mode) */
static int cell_running(void);  /* the modem runs (sh_cell) */
static void cell_toggle(void);  /* the Cellular switch (sh_cellular) */
static const char *md_text(void);

/* ---- the wallpaper and its glass backdrop ------------------------------------------------------------------ */
static tk_layer star, star_e;
static void build_wall(void)
{
    /* the wallpaper at half resolution (smooth gradients; the rest frames scale it 2x), the star as a layer over
     * it; the glass backdrop is the wallpaper with the star, blurred */
    tk_wall_draw(&C->base_half, 0, .5f);
    const tk_bmp *st = tk_icon("star");
    float pad = tk_icon_pad("star");
    tk_layer_init(&star, "star");
    star.bmp = *st;                       /* a view into the pack */
    star.x = (float)(int)tk_floorf(PT(390 - 34 - 36) - pad + .5f);
    star.y = (float)(int)tk_floorf(PT(56) - pad + .5f);
    /* Emergency's star: the same with a warm glow (.7 s, with the wallpaper) */
    tk_layer_init(&star_e, "star-e");
    const tk_bmp *se = tk_icon("star-e");
    if (se) star_e.bmp = *se;
    star_e.x = (float)(int)tk_floorf(PT(390 - 34 - 36) - tk_icon_pad("star-e") + .5f);
    star_e.y = star.y;
    star_e.hidden = 1;
    tk_bmp src, sh;
    tk_bmp_new(&src, C->base_half.w, C->base_half.h);
    tk_bmp_copy(&src, 0, 0, &C->base_half, 0, 0, src.w, src.h);
    tk_bmp_new(&sh, (st->w + 1) / 2, (st->h + 1) / 2);
    tk_bmp_down2(&sh, st);
    tk_blit(&src, &sh, (int)star.x / 2, (int)star.y / 2, 1);
    tk_glass_backdrop(&C->bw_half, &src);
    tk_bmp_free(&sh);
    tk_bmp_free(&src);
}

/* ---- the status bar (lock and home) -------------------------------------------------------------------------- */
static void d_rrect(dc *d, float x, float y, float w, float h, float r, tk_col c);
static struct { int state, bars, rat; } SB_CELL;      /* the status bar's copy of the modem's (sh_cell) */
static int SB_WIFI;                                  /* wifi-sbos-next: joined with an address (sh_wifi) */
static struct { int h, mi, wday, day, mon, pct, temp; } ST = { 21, 5, 2, 10, 11, 87, 41 };
static struct { int on, h, m; } ALM = { 1, 7, 0 };   /* the host's is the concept's 07:00 */
static void prep_tree(tk_layer *l);   /* P3g: home's half copies again right after a live rebuild */
static void build_apsb(void);
static int sys_charging;           /* P2y: sh_sysinfo's (the status bar's battery is green while charging) */
#define SYS_CHARGING() sys_charging
static void d_text_box(dc *d, int f, float size, float x, float base, float maxw, int align, const char *s, float ls, int fl, tk_col c, int shadow);
/* P3p (P3j-5): iOS's status bar metrics: the left text 17 pt / 600 at MARGIN + 16, cap-centred on y 30 (baseline
 * 36.2); the battery centred on y 30 with its right edge at 390 - MARGIN - 16; the Wi-Fi glyph and the bars (with
 * the RAT, 15 pt / 600) to its left, each 6 pt apart */
#define SB_MID 30.0f
#define SB_BASE 36.2f
static int sb_cap;                   /* B7 (shell_island.inc): the status capsule shows: the left text stays out */
static void status_bar(dc *d, const char *left)
{
    if (!sb_cap) d_text_box(d, TK_IT600, 17, MARGIN + 16, SB_BASE, 150, 0, left, 0, 0, INK, 1);   /* the carrier: at most 150 pt */
    float bx = CONTENT_R - 16 - 27.6f, by = SB_MID - 6, x = bx - 6;
    /* wifi-sbos-next: the Wi-Fi glyph while joined with an address */
    if (SB_WIFI) { x -= 17; d_icon(d, "sb-wifi", x, SB_MID - 6, 1); x -= 6; }
    if (SB_CELL.state == 1 && SB_CELL.bars >= 0) {
        /* four bars (0..5 from the CP: 5 is four), the RAT before them: only while the modem is on */
        int n = SB_CELL.bars >= 4 ? 4 : SB_CELL.bars;
        x -= 16.5f;
        for (int i = 0; i < 4; i++) d_rrect(d, x + 4.5f * i, by + 12 - (4.5f + 2.5f * i), 3, 4.5f + 2.5f * i, 1, W(i < n ? 1 : .3f));
        const char *r = SB_CELL.rat == 7 ? "LTE" : SB_CELL.rat == 4 ? "3G" : SB_CELL.rat >= 1 && SB_CELL.rat <= 3 ? "2G" : "";
        if (r[0]) d_text(d, TK_IT600, 15, x - 4 - tk_text_w(TK_IT600, PT(15), r, 0, 0) / TK_PT, SB_MID + 5.5f, r, 0, 0, INK, 1);
    }
    /* the battery drawn from the live charge (iOS's glyph: a translucent body, the fill by percent, red at 20 % or
     * less, green while charging, the nub) */
    float fw = 21.0f * tk_clampf(ST.pct / 100.0f, .04f, 1);
    d_rrect(d, bx, by, 25, 12, 3.6f, W(.35f));
    d_rrect(d, bx + 2, by + 2, fw, 8, 2, ST.pct <= 20 && !SYS_CHARGING() ? C_RED : SYS_CHARGING() ? C_GREEN : W(1));
    d_rrect(d, bx + 26, by + 4, 1.6f, 4, .8f, W(.4f));
}

/* ---- the lock's static layer: status bar, date, clock, widgets ------------------------------------------------ */
/* P3j (audit 2): iOS's circular gauge: an open 270 degree arc (7:30 round to 4:30), the value centred on its
 * digits, the label small in the gap at the bottom (a caption inside the ring overprinted it) */
static void gauge_arc(dc *d, float cx, float cy, float r, float sw, float sweep, tk_col c)
{
    if (sweep <= 0) return;
    int bx = (int)tk_floorf(PT(cx - r - sw) - d->ox) - 2, by = (int)tk_floorf(PT(cy - r - sw) - d->oy) - 2;
    int n = (int)PT(2 * (r + sw)) + 5;
    tk_fm m;
    if (!tk_fm_new(&m, n, n)) return;
    tk_fm_arc(&m, d->ox + bx, d->oy + by, PT(cx), PT(cy), PT(r), PT(sw), 3.92699082f, sweep, 1);
    tk_paint pp = tk_solid(c);
    tk_draw_fm(d->b, &m, bx, by, &pp, 1);
    tk_fm_free(&m);
}
static void ring_widget(dc *d, float x, float p, tk_col col, const char *num, const char *lbl)
{
    d_glass(d, x, WROW_Y, 72, 72, 36, 1, 1);
    float cx = x + 36, cy = WROW_Y + 36;
    gauge_arc(d, cx, cy, 29, 4.5f, 4.71238898f, W(.16f));
    gauge_arc(d, cx, cy, 29, 4.5f, 4.71238898f * tk_clampf(p, 0, 1), col);
    /* the value: 18 pt / 600 on the ring's centre; a trailing degree sign hangs right of the centred digits */
    char dg[12];
    int k = 0;
    while (num[k] && (unsigned char)num[k] < 0x80 && k < 11) { dg[k] = num[k]; k++; }
    dg[k] = 0;
    float w = tw(TK_IT600, 18, dg, 0, TK_TNUM);
    d_text(d, TK_IT600, 18, cx - w / 2, cy + 6.5f, num, 0, TK_TNUM, INK, 0);
    /* the label, mixed case, 10 pt / 600 in the gap */
    char lb[16];
    int n = 0;
    for (; lbl[n] && n < 15; n++) lb[n] = n && lbl[n] >= 'A' && lbl[n] <= 'Z' ? (char)(lbl[n] + 32) : lbl[n];
    lb[n] = 0;
    w = tw(TK_IT600, 10, lb, 0, 0);
    d_text(d, TK_IT600, 10, cx - w / 2, cy + 29 + 3.5f, lb, 0, 0, W(.75f), 0);
}
/* the live values (sh_status): the host's are the concept's */
static const char *const WD[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const MO[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December" };
static int put_dec(char *b, int v) { int n = 0; char t[12]; int k = 0; if (v < 0) { b[n++] = '-'; v = -v; }
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) b[n++] = t[--k]; b[n] = 0; return n; }
/* Track A: 12 / 24-hour (sh_clock24). fmt_hm: the status bar's and the lock's "9:41" (no AM / PM, as iOS); fmt_time
 * with ampm: lists' "9:41 AM" (b: 9 bytes) */
#include "sh_timefmt.h"
static int fmt_time(char *b, int h, int m, int ampm) { return sh_fmt_time(b, h, m, sh_clock24(), ampm); }
static void fmt_hm(char *b) { fmt_time(b, ST.h, ST.mi, 0); }

static tk_layer lstatus, lclock, lwidgets;
static void relink(void);
/* helper D (shell_nc.inc, shell_switcher.inc): the hooks this file calls before their definitions */
static void nc_sb_stale(void);
static void nc_clock_stale(void);
static void nc_cards_gone(void);
static float nc_next_update(float t);
static float pc_next_update(float t);                /* Track A: shell_passcode.inc */
/* Track A update (shell_update.inc): Settings > Software Update, opened from About's Software row for now */
static void su_page(dc *d);
static int su_app_tap(float x, float y);
static void su_open(void);
static int su_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c);
static int su_set(const char *what, const char *key, float v);
static int su_refresh(float t);
static float su_next_update(float t);
#define SU_SUB 33                                   /* (B2's strip pages: 30 startup, 31 passcode, 32 zones) */
static int pc_refresh(float t);
static int nc_refresh(float t);
static int sw_app_alpha(void);
static void sw_opened(int a);
static void sw_app_gone(void);
static int sw_dirty;                          /* (shell_switcher.inc's: the open app's snapshot is old) */
static void nc_blit_scaled(tk_bmp *dst, const tk_bmp *src, float X, float Y, float f);
static void link(tk_layer *parent, tk_layer **kids, int n);
static void lap_us(const char *what, uint64_t us);
static char carrier[32] = "";            /* P3z: nothing while Cellular is off (the SIM is unknown then; "No SIM" only when the modem says so) */
static void build_lstatus(void)
{
    relayer(&lstatus);
    dc d = mk(&lstatus, "lock-status", 0, 0, 390, 48, 2, 0);
    status_bar(&d, carrier);
    trim(&lstatus);
    nc_sb_stale();                               /* helper D: the Notification Centre's status bar too */
}
/* the modem's status (its session's status API): the carrier or "No SIM" / "Emergency calls only" */
static void cc_status_stale(void);
void sh_carrier(const char *text)
{
    if (!tk_strcmp(text, carrier)) return;
    int n = 0;
    while (text[n] && n < 31) { carrier[n] = text[n]; n++; }
    carrier[n] = 0;
    build_lstatus();
    cc_status_stale();                           /* P3l: the CC's status bar shows the carrier too */
    relink();
    lock_ver++;
}
static void build_lclock(void)
{
    /* B9 (IOS-REFERENCE 9.1, measured): the date 22 / 600 centred on y 82; the clock 116 pt / 600 (Inter for SF's
     * Display), its digits' top on y 108; gradient #fff -> #dfe9ff over its box */
    relayer(&lclock);
    dc d = mk(&lclock, "lock-clock", 0, 66, 390, 134, 2, 0);
    char date[48];
    int n = 0;
    for (const char *p = WD[ST.wday % 7]; *p;) date[n++] = *p++;
    date[n++] = ' ';
    n += put_dec(date + n, ST.day);
    date[n++] = ' ';
    for (const char *p = MO[(ST.mon + 11) % 12]; *p;) date[n++] = *p++;
    date[n] = 0;
    float w = tw(TK_IT600, 22, date, 0, 0);
    d_text(&d, TK_IT600, 22, (390 - w) / 2, 82 + 22 * INTER_CAP / 2, date, 0, 0, W(.9f), 1);
    char hm[8];
    fmt_hm(hm);
    /* P3j (audit 3): hours, colon and minutes as three runs: proportional figures at -1 pt (the -4 pt tabular
     * run pushed the colon onto the next digit and set the clock 2.4 pt right), the colon raised to the digits'
     * middle (Inter's sits at x-height) with even room each side, the whole centred */
    int hn = hm[1] == ':' ? 1 : 2;               /* Track A: "9:41" in 12-hour time */
    char hh[3] = { hm[0], hn == 2 ? hm[1] : 0, 0 }, mm[3] = { hm[hn + 1], hm[hn + 2], 0 };
    const float cs = 116, ctop = 108;
    float wh = tw(TK_IT600, cs, hh, -1, 0), wc = tw(TK_IT600, cs, ":", 0, 0), wm = tw(TK_IT600, cs, mm, -1, 0), gap = 2;
    float x0 = (390 - (wh + gap + wc + gap + wm)) / 2, base = ctop + cs * INTER_CAP;
    tk_stop s[2] = { { 0, tk_hex(0xffffff, 1) }, { 1, tk_hex(0xdfe9ff, 1) } };
    tk_paint p = tk_linear(180, -d.ox, PT(ctop) - d.oy, PT(390), PT(cs * INTER_CAP), 2, s);
    tk_text(d.b, TK_IT600, PT(cs), PT(x0) - d.ox, PT(base) - d.oy, hh, PT(-1), 0, &p, 0);
    tk_text(d.b, TK_IT600, PT(cs), PT(x0 + wh + gap) - d.ox, PT(base - cs * .0906f) - d.oy, ":", 0, 0, &p, 0);
    tk_text(d.b, TK_IT600, PT(cs), PT(x0 + wh + gap + wc + gap) - d.ox, PT(base) - d.oy, mm, PT(-1), 0, &p, 0);
    trim(&lclock);
    nc_clock_stale();                            /* helper D: the Notification Centre shows it (a view: again now) */
}
/* P3p (P3j-17): iOS's accessoryRectangular between the two gauges: no platter or border, left-aligned vibrant
 * text: a glyph and the label (15 / 600, .7), the value 30 pt / 600 tabular, a 13 pt second line */
static int alm_when = -1;          /* the alarm's "Today" / "Tomorrow" as drawn (sh_status redraws it on a change) */
static void rect_widget(dc *d, const char *glyph, const char *label, const char *value, const char *sub)
{
    float x = 123;
    const float o = WROW_Y - 220;
    if (glyph) { d_icon(d, glyph, x, 225 + o, 1); x += 15; }
    d_text_box(d, TK_IT600, 15, x, 235 + o, 275 - x, 0, label, 0, 0, W(.7f), 0);
    d_text_box(d, TK_IT600, 30, 122, 266 + o, 150, 0, value, 0, TK_TNUM, INK, 0);
    d_text_box(d, TK_IT400, 13, 123, 285 + o, 150, 0, sub, 0, 0, C_LABEL2, 0);
}
/* the lock's widgets of a mode (emg) into d: part 0 the left ring, 1 the right one, 2 the middle one, -1 all (B6:
 * Emergency's switch draws them aside a piece at a time) */
static void lw_draw(dc *d, int emg, int part)
{
    char num[12];
    put_dec(num, ST.pct);
    if (emg) {
        /* Emergency: the battery ring amber, Heat (ready, the phone's temperature), Saving (off) */
        if (part <= 0) ring_widget(d, 33, tk_clampf(ST.pct / 100.0f, 0, 1), tk_hex(0xffb547, 1), num, "BATTERY");
        if (part < 0 || part == 1)
            ring_widget(d, 285, sh_last2() ? 1 : 0, tk_hex(0xff5a4e, 1), sh_last2() ? "On" : "Off", "SAVING");   /* survival-next: Last 2 % */
        if (part < 0 || part == 2) {
            char v[40];
            int k = put_dec(v, ST.temp);
            tk_memcpy(v + k, " \xc2\xb0" "C battery", 13);
            rect_widget(d, 0, "Heat", "Ready", v);
        }
        return;
    }
    if (part <= 0) ring_widget(d, 33, tk_clampf(ST.pct / 100.0f, 0, 1), C_GREEN, num, "BATTERY");
    if (part < 0 || part == 1) {
        int n = put_dec(num, ST.temp);
        num[n] = (char)0xc2; num[n + 1] = (char)0xb0; num[n + 2] = 0;
        ring_widget(d, 285, tk_clampf(ST.temp / 100.0f, 0, 1), C_ACCENT, num, "TEMP");
    }
    if (part < 0 || part == 2) {
        char at[16] = "Off";                       /* P2y: the kernel's next alarm (sh_alarm), not the concept's */
        if (ALM.on) fmt_time(at, ALM.h, ALM.m, 1);    /* Track A: "7:00 AM" in 12-hour time */
        alm_when = !ALM.on ? 0 : ALM.h * 60 + ALM.m > ST.h * 60 + ST.mi ? 1 : 2;
        rect_widget(d, "alarm", "Alarm", at, alm_when == 1 ? "Today" : alm_when == 2 ? "Tomorrow" : "No alarm set");
    }
}
static dc lw_mk(tk_layer *l) { return mk(l, "lock-widgets", 0, WROW_Y - 10, 390, 92, 0, 1); }   /* B9: the widgets row */
static void build_lwidgets(void)
{
    relayer(&lwidgets);
    dc d = lw_mk(&lwidgets);
    lw_draw(&d, EMG, -1);
    trim(&lwidgets);
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
    trim(&mark);
    trim(&hint);
    d = mk(&homebar, "homebar", 128, 789, 134, 5, 1, 0);    /* P3i: iOS's 134 x 5, radius 2.5 */
    d_rrect(&d, 128, 789, 134, 5, 2.5f, W(1));
}

/* ---- the press-and-hold buttons ---------------------------------------------------------------------------- */
#define HOLD_X(i) ((i) ? 294.0f : 46.0f)          /* B9: 50 pt, centred at 71 / 319, y 727 (IOS-REFERENCE 9.1) */
#define HOLD_Y 702.0f
static void build_hold(int i)
{
    hold_t *q = &Q[i];
    /* P3p (P3j-17): iOS's lock-screen buttons: 50 pt (no blue tint, no ring: the hold fires at its threshold with a
     * haptic tap, the button still growing to 1.22); lit: the torch a white disc with a dark glyph, Emergency red.
     * B9: both are iOS 26's glass circles, the widgets' dark glass (rim, light, the lens's shape on the GPU) */
    float x = HOLD_X(i), y = HOLD_Y;
    q->grp.ox = PT(x + 25);
    q->grp.oy = PT(y + 25);
    dc d = mk(&q->face, "q-face", x, y, 50, 50, 8, 1);
    d_glass(&d, x, y, 50, 50, 25, 1, 1);
    d_rrect(&d, x, y, 50, 50, 25, W(.04f));
    d = mk(&q->facelit, "q-lit", x, y, 50, 50, 2, 0);
    d_rrect(&d, x, y, 50, 50, 25, i ? tk_hex(0xff5a4e, 1) : W(1));
    d = mk(&q->icon, "q-icon", x + 15, y + 13, 20, 24, 2, 0);
    d_icon(&d, i ? "flame" : "torch", x + 15, y + 13, 1);
    d = mk(&q->iconlit, "q-iconlit", x + 15, y + 13, 20, 24, 2, 0);
    d_icon(&d, i ? "flame" : "torch-lit", x + 15, y + 13, 1);
    tk_layer_init(&q->ring, "q-ring");               /* no ring (no bitmap, no tables): hold_ring does nothing */
    q->ang = q->cov = 0;
    q->ms = i ? 700 : 420;
    q->scale_t = q->scale_from = q->scale_to = 1;
    q->facelit.hidden = q->iconlit.hidden = 1;
    q->ring.hidden = 1;
}
static void hold_ring(hold_t *q, int i, float p)
{
    tk_bmp *b = &q->ring.bmp;
    if (!q->ang || !q->cov || !b->px) return;
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

/* survival-next: Emergency mode's tools (shell_survival.inc) */
static float sv_ec_max_scroll(void);
static int sv_tile(dc *d, const char *k, float x, float y, float s);
static const char *sv_ec_sub(int i, char *b);
static int sv_ec_hit(float x, float y);
static void sv_ec_tap(int i);
static int sv_idle_pending(void);
static void sv_idle(void);
static int sv_own(const char *k);
static int sv_page(const char *k, dc *d);
static void sv_page_card_edit(dc *d);
static void sv_extras(const char *k);
static int sv_tap(float x, float y);
static void sv_opened(void);
static void sv_closed(void);
static uint32_t sv_ground(int a);
static float sv_next_update(float t);
static int sv_refresh(float t);

/* ---- the Systems list ------------------------------------------------------------------------------------------
 * B10 (the user, 2026-11-12, over B9's Systems tiles): the vendors as notification cards under the widgets, B4's
 * stacks: a card's members peek under it; a tap fans them out as their own platters ("Show less" above), a member's
 * tap opens the focus sheet and its slide-to-start. The list scrolls; it ends above the lock's notifications (their
 * stack, shell_lknote.inc), and runs down to the buttons while a card is open (the notifications fade out of its
 * way) or when there are none. Emergency keeps its own list of eight cards (the same list layer). */
#define FACE_MG 44.0f            /* the margin around a card's face (its shadow) */
#define LIST_TOP 310.0f
#define LIST_H 374.0f           /* .nwrap: top 310, bottom 118 */
static int stack(int g) { return G[g].n > 1; }
/* P3i: a stack's lower cards peek 8 and 15 pt below the top one (iOS's collapsed notification stack), then the
 * list's 10 pt gap */
#define PEEK1 8.0f
#define PEEK2 15.0f
/* the gap under a card: room for its stack's peek, which closes as the card opens (the lower cards are gone) */
static float stack_gap(int g);
#define STACK_GAP(g) stack_gap(g)
static void ellipsize(char *out, int cap, int f, float size, const char *s, float maxw);
static float stack_gap(int g)
{
    if (!stack(g)) return 10;
    float q = V[g].full > 0 ? tk_clampf(V[g].memh / V[g].full, 0, 1) : 0;
    return 10 + PEEK2 * (1 - q);
}
static void layout_list(void)
{
    float y = 10;
    for (int g = 0; g < NG; g++) {
        V[g].y = y;
        y += 68 + V[g].memh + STACK_GAP(g);
    }
}
/* B4 (P3j-8): an open stack's members are their own 68 pt platters 8 apart (76 a member), under a 36 pt band with
 * "Show less" above the head card; q the stack's share of its fan (0 closed .. 1 open) */
#define MEM_P 76.0f
#define SL_BAND 36.0f
static float card_top(int g);
static float bl(int f, float size, float top);
static float stack_q(int g) { return V[g].full > 0 ? tk_clampf(V[g].memh / V[g].full, 0, 1) : 0; }
static float card_h(int g) { return SL_BAND * stack_q(g) + 68 + V[g].memh + STACK_GAP(g); }
static float content_h(void)
{
    float y = 10;
    for (int g = 0; g < NG; g++) y += card_h(g);
    /* the last margin (22) collapses with the tools' 12; then their 32, margin 4, padding 14 */
    return y + 32 + 4 + 14;
}
/* B10: the most any card is open (0 all closed .. 1): the lock's notifications fade out of an opening card's way */
static float vc_open_q(void) { float q = 0; for (int g = 0; g < NG; g++) q = tk_maxf(q, stack_q(g)); return q; }
static int vc_any_open(void) { for (int g = 0; g < NG; g++) if (V[g].isopen) return 1; return 0; }
/* the list's bottom (screen pt): above the notifications' stack while it shows, else (or while a card is open) above
 * the buttons, as B4's list */
static float lkn_stack_top(void);       /* shell_lknote.inc: the stack's top (0: no notifications) */
static float list_bot(void)
{
    float lb = LIST_TOP + LIST_H, st = EMG ? 0 : lkn_stack_top();
    if (st <= 0) return lb;
    float a = tk_clampf(1 - vc_open_q() * 3, 0, 1);       /* (the notifications' fade: lkn_place) */
    return lb + (tk_minf(lb, st - 10) - lb) * a;
}
static float list_h(void) { return list_bot() - LIST_TOP; }
static float max_scroll(void) { return EMG ? sv_ec_max_scroll() : tk_maxf(0, content_h() - list_h()); }

/* P3i: a lower card of the stack, iOS-style: the whole card's shape (narrower, its radius scaled with it) baked in
 * its own bitmap and faded as one; what the cards above it cover is taken out afterwards (erase_rrect) */
static void ghost(tk_bmp *G0, float ox, float oy, float x, float y, float w, float h, float r, float op)
{
    tk_bmp t;
    if (!tk_bmp_new(&t, G0->w, G0->h)) return;
    tk_glass_bake(&t, 0, PT(x) - ox, PT(y) - oy, PT(w), PT(h), PT(r), 0, 0);   /* the card above casts the shadow */
    tk_blit(G0, &t, 0, 0, op);
    tk_bmp_free(&t);
}
/* nothing of b inside the rounded box (a card above covers it: none of it may show through that card's glass) */
static void erase_rrect(tk_bmp *b, float ox, float oy, float x, float y, float w, float h, float r)
{
    tk_fm m;
    if (!tk_fm_new(&m, b->w, b->h)) return;
    tk_fm_rrect(&m, 0, 0, PT(x) - ox, PT(y) - oy, PT(w), PT(h), PT(r));
    for (int j = 0; j < b->h; j++)
        for (int i = 0; i < b->w; i++) {
            float c = m.v[j * b->w + i];
            if (c <= 0) continue;
            uint32_t *p = &b->px[j * b->stride + i], o = *p, res = 0;
            unsigned k = (unsigned)((1 - c) * 256 + .5f);
            for (int sh = 0; sh < 32; sh += 8) res |= ((o >> sh & 255) * k >> 8) << sh;
            *p = res;
        }
    tk_fm_free(&m);
}
/* the platter glass (K[0]): the face, a 3-slice drawn at any height from 48 pt, and the stack's lower cards under a
 * 68 pt one (shell_nc.inc's pl_peek places them so). Baked once at boot (and again after Emergency); never in a tap */
static void build_platter(void)
{
    card_t *k = &K[0];
    k->y = 10;
    float top = LIST_TOP + k->y;
    /* the second card 9 pt in at each side (.95 of the width), the third 18 (.90), peeking PEEK1 / PEEK2 below
     * (IOS-REFERENCE 5: opacity .7 / .5); each covers the one under it and the top card covers both */
    dc e = mk(&k->gh, "stack", PL_X + 9, top + 68 - 26, PL_W - 18, 26 + PEEK2, 2, 0);
    float b1 = top + 68 + PEEK1, b2 = top + 68 + PEEK2;
    ghost(e.b, e.ox, e.oy, PL_X + 18, b2 - 68 * .94f, PL_W - 36, 68 * .94f, PL_R * .9f, .5f);
    erase_rrect(e.b, e.ox, e.oy, PL_X + 9, b1 - 68 * .97f, PL_W - 18, 68 * .97f, PL_R * .95f);
    ghost(e.b, e.ox, e.oy, PL_X + 9, b1 - 68 * .97f, PL_W - 18, 68 * .97f, PL_R * .95f, .72f);
    erase_rrect(e.b, e.ox, e.oy, PL_X, top, PL_W, 68, PL_R);
    trim(&k->gh);
    k->gh.hidden = 1;
    /* P3q (P3j-11): made with a glass mask so d_glass records the box's shape (the GPU row's live, refracted glass),
     * the mask freed at once (the CPU path keeps the baked look; no heap kept) */
    dc d = mk(&k->face, "face", PL_X, top, PL_W, 68 + 12, FACE_MG, 1);
    d_glass(&d, PL_X, top, PL_W, 68 + 12, PL_R, 0, 1);
    tk_a8_free(&k->face.glass);
    k->face.slice = 1;
    k->face.slice_s0 = PT(24 + FACE_MG);
    k->face.slice_s1 = PT(24 + FACE_MG);
    k->face.slice_h = PT(68 + 2 * FACE_MG);
    int ct, cb;
    k->fcut = 0;
    if (trim_layer(&k->face, &ct, &cb)) {
        k->face.slice_s0 -= (float)ct;
        k->face.slice_s1 -= (float)cb;
        k->fcut = (float)(ct + cb);
        k->face.slice_h -= k->fcut;
    }
    k->face.hidden = 1;
}
/* the face's 3-slice for a platter h pt tall whose top is y (screen pt): on the face itself (the open vendor) or a view of it */
static void platter_at(tk_layer *f, float y, float h)
{
    card_t *k = &K[0];
    f->x = k->face.bx;                            /* (a view's own bx / by are not the face's) */
    f->y = tk_floorf(PT(y - (LIST_TOP + k->y)) + k->face.by + .5f);
    f->slice_h = PT(tk_maxf(48, h) + 2 * FACE_MG) - k->fcut;
}

/* ---- the vendor cards (B10: B4 / P3i's, on B9's platter glass) ------------------------------------------------- */
static void nc_view(tk_layer *v, tk_layer *m);             /* shell_nc.inc: a view shares the glass's bitmaps */
static void nc_unview(tk_layer *v);
static void nc_view_shapes(tk_layer *v, const tk_layer *m);
static void lkn_place(void);              /* the lock's notifications (shell_lknote.inc) */
static void lkn_collapse(int instant);
static float lkn_p(void);                 /* how far they are fanned out (0..1): the list gives way */

static void head_content(dc *d, int g, float top)
{
    const grp_t *gr = &G[g];
    char tilename[40] = "t46-";
    tk_memcpy(tilename + 4, gr->key, tk_strlen(gr->key) + 1);
    d_icon(d, tilename, VC_ICON, top + 11, 1);
    float b = base_of(TK_IT700, 20, top + 13.45f, 23.1f);
    d_text(d, TK_IT700, 20, VC_TX, b, gr->name, 0, 0, INK, 0);         /* P3j: Bold, untracked (ExtraBold -0.5 was heavy) */
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
    d_rrect(d, VC_TX, sb - 7, 6, 6, 3, C_GREEN);
    /* P3i: the chevron at one place on every card (iOS: a row's chevron never moves); the names give way with an
     * ellipsis so the count stays whole */
    V[g].chx = VC_CHEV;
    char fit[160];
    float mw = V[g].chx - 12 - VC_SX - tw(TK_IT400, 13.5f, em, 0, 0);
    ellipsize(fit, sizeof fit, TK_IT400, 13.5f, names, mw);
    int fl = tk_strlen(fit);
    if (tk_strcmp(fit, names)) {
        /* whole names and the rest as "+N" (audit 4: "iOS 16, macOS Ventura +1"), never cut mid-word */
        int kept = gr->n;
        for (int c = n - 1; c > 0; c--) {
            if (names[c] != ',') continue;
            kept--;
            tk_memcpy(fit, names, (size_t)c);
            fit[c] = ' '; fit[c + 1] = '+'; fit[c + 2] = (char)('0' + (gr->n - kept)); fit[c + 3] = 0;
            if (tw(TK_IT400, 13.5f, fit, 0, 0) + tw(TK_IT400, 13.5f, " ", 0, 0) <= mw) break;
        }
        fl = tk_strlen(fit);
    }
    if (fl && fit[fl - 1] != ' ' && fl < 158) { fit[fl] = ' '; fit[fl + 1] = 0; }
    d_text(d, TK_IT400, 13.5f, VC_SX, sb, fit, 0, 0, W(.82f), 0);
    d_text(d, TK_IT400, 13.5f, VC_SX + tw(TK_IT400, 13.5f, fit, 0, 0), sb, em, 0, 0, W(.48f), 0);
}
/* a card's own pixels, made at boot (and again after Emergency): its head (icon and names) and its chevron. Its glass
 * is K[0]'s (views placed by place_list): nothing is baked here, nor at the tap that opens it (P3i) */
static void build_vcard(int g)
{
    vcard_t *k = &V[g];
    float top = LIST_TOP + k->y;            /* rest screen pt at scroll 0 */
    k->full = MEM_P * G[g].n;
    dc d = mk(&k->head, "head", VC_ICON, top + 9, VC_CHEV - 4 - VC_ICON, 52, 1, 0);
    head_content(&d, g, top);
    trim(&k->head);
    d = mk(&k->chev, "chev", k->chx, top + 27, 14, 14, 0, 0);
    d_icon(&d, "chev", k->chx, top + 27, 1);
    k->chev_t = 1;
    tk_bmp_free(&k->chev0);
    tk_bmp_new(&k->chev0, k->chev.bmp.w, k->chev.bmp.h);
    tk_bmp_copy(&k->chev0, 0, 0, &k->chev.bmp, 0, 0, k->chev.bmp.w, k->chev.bmp.h);
    k->members.clip = 0;
    k->chev.clip = 1;
}
/* a view of the platter glass (K[0].face) as a platter h tall at y (content pt at scroll 0; 0 if there is no glass) */
static int vc_glass(tk_layer *v, float y, float h)
{
    tk_layer *f = &K[0].face;
    if (!f->bmp.px) { if (v->bmp.px) nc_unview(v); v->hidden = 1; return 0; }
    if (v->bmp.px != f->bmp.px) { nc_view(v, f); nc_view_shapes(v, f); }
    platter_at(v, y, h);
    v->hidden = 0;
    return 1;
}
/* an opening card's member rows and its "Show less" (words and icons only: the members' platters are views of the
 * glass), at the tap; freed when it has closed */
static void build_open(int g)
{
    vcard_t *k = &V[g];
    const grp_t *gr = &G[g];
    if (k->rows[0].bmp.px) return;
    float top = LIST_TOP + k->y;
    dc d;
    k->members.child = 0;
    for (int i = 0; i < gr->n; i++) {
        const sys_t *s = &gr->it[i];
        float ry = top + 68 + 8 + MEM_P * i + 7;   /* B4: in its own platter (centred in its 68 pt) */
        d = mk(&k->rows[i], "row", VC_X + 6, ry - 2, VC_W - 12, 58, 1, 0);
        char tn[40] = "t38-";
        tk_memcpy(tn + 4, s->k, tk_strlen(s->k) + 1);
        d_icon(&d, tn, VC_X + 21, ry + 9, 1);
        d_text(&d, TK_IT700, 16.5f, VC_X + 71, ry + 9.5f + 16, s->name, -.2f, 0, INK, 0);
        const char *tag = s->self ? "RUNNING" : !tk_strcmp(s->k, LAST) ? "LAST USED" : s->tag;
        float tgw = tag ? tw(TK_IT700, 10.5f, tag, .42f, 0) + 14 : 0;
        d_text_box(&d, TK_IT400, 13, VC_X + 71, ry + 30.5f + 13, VC_W - 71 - 20 - (tag ? tgw + 8 : 0), 0, s->meta, 0, 0, W(.72f), 0);
        if (tag) {
            float tx = VC_X + VC_W - 20 - tgw, ty = ry + 18.5f;
            d_rrect(&d, tx, ty, tgw, 19, 9.5f, s->self ? sbca(SBC_GREEN, .18f) : sbca(SBC_ACCENT, .18f));
            d_text(&d, TK_IT700, 10.5f, tx + 7, ty + 3 + 10, tag, .42f, 0, s->self ? tk_hex(0xc9f7d3, 1) : tk_hex(0xcfe4ff, 1), 0);
        }
        trim(&k->rows[i]);
        tk_layer_add(&k->members, &k->rows[i]);
    }
    /* their half copies now (at the tap), not in the first frame of the card's spring */
    for (int i = 0; i < gr->n; i++) tk_layer_prepare(&k->rows[i]);
    /* "Show less" (13 / 600 in a 28 pt capsule) right-aligned above the card */
    {
        float w = tw(TK_IT600, 13, "Show less", 0, 0) + 24, x = VC_X + VC_W - w, y = top - SL_BAND + 4;
        d = mk(&k->sl, "show-less", x, y, w, 28, 1, 0);
        d_rrect(&d, x, y, w, 28, 14, W(.18f));
        d_text(&d, TK_IT600, 13, x + 12, bl(TK_IT600, 13, y + 6), "Show less", 0, 0, INK, 0);
        trim(&k->sl);
        tk_layer_prepare(&k->sl);
        k->sl.hidden = 1;
    }
    relink();                                 /* (the new layers' places in the list) */
}
/* the rows and the pill only (the head and the glass's views stay: they are the card) */
static void free_open(int g)
{
    vcard_t *k = &V[g];
    if (!k->rows[0].bmp.px) return;
    for (int i = 0; i < G[g].n; i++) relayer(&k->rows[i]);
    k->members.child = 0;
    for (int i = 0; i < 3; i++) { if (k->mpl[i].bmp.px) nc_unview(&k->mpl[i]); k->mpl[i].hidden = 1; }
    relayer(&k->sl); k->sl.hidden = 1;
}
/* a card's glass views (Emergency frees the glass: none may point at it) */
static void vc_unview(int g)
{
    vcard_t *k = &V[g];
    tk_layer *v[5] = { &k->face, &k->gh, &k->mpl[0], &k->mpl[1], &k->mpl[2] };
    for (int i = 0; i < 5; i++) { if (v[i]->bmp.px) nc_unview(v[i]); v[i]->hidden = 1; }
}

static void build_etool(void)
{
    float top = LIST_TOP + 448;
    dc d = mk(&etool, "etool", 90, top, 210, 32, 44, 0);
    float w1 = 14 + 15 + 6 + tw(TK_IT600, 13, "Recovery", 0, 0) + 14, w2 = 14 + 15 + 6 + tw(TK_IT600, 13, "Lab", 0, 0) + 14;
    float x = (390 - (w1 + 10 + w2)) / 2;
    d_glass(&d, x, top, w1, 32, 16, 1, 1);
    d_icon(&d, "recovery", x + 14, top + 8.5f, 1);
    d_text(&d, TK_IT600, 13, x + 35, top + 8 + 13, "Recovery", 0, 0, INK, 0);
    x += w1 + 10;
    d_glass(&d, x, top, w2, 32, 16, 1, 1);
    d_icon(&d, "lab", x + 14, top + 8.5f, 1);
    d_text(&d, TK_IT600, 13, x + 35, top + 8 + 13, "Lab", 0, 0, INK, 0);
    trim(&etool);
}

/* Emergency's list (the concept's renderEmergency): eight cards and Exit Emergency; made on entering, freed on
 * leaving. survival-next: the four tools after the concept's four (USB Power, Sun, Guide, Compass / Level); the
 * list scrolls; ecard[5] and [6] hold cards 4..7, drawn one per idle turn (shell_survival.inc: sv_idle). survival-next (the merge): two
 * cards a layer below the fold (5 MB each, not one 8 MB layer: the 160 MiB heap's holes refused it in Emergency) */
#define NEC 7                           /* 0 cards 0..3, 4 Exit, 5 cards 4..5, 6 cards 6..7 */
#define EC_LAYER(i) (&ecard[5 + ((i) - 4) / 2])   /* the layer of card i (4..7) */
static tk_layer ecard[NEC], empill;
static int ec_next = 8;                 /* survival-next: cards 4..7 drawn so far */
static int ec_wait;                     /* B6: Emergency's first cards or Exit not drawn yet (hidden till then) */
typedef struct { const char *icon, *t, *s, *act; } ecard_t;   /* act: the app it opens (none: a toast) */
static const ecard_t EC[8] = {
    { "e46-heat", "Survival Heat", "Warms your hands \xc2\xb7 stops at a 65 \xc2\xb0" "C battery", "Survival Heat" },
    { "e46-power", "Last 2 %", "Everything off but the clock, the torch and SOS", "power" },
    { "e46-sos", "Emergency SOS", "Morse SOS on the torch, the motor and the screen", "sos" },
    { "e46-medical", "Medical Card", "Shown here without unlocking", "medical" },
    { "usb", "USB Power", "5 V out of the USB-C port: a fire starter", "usb" },
    { "sun", "Sun", 0, "sun" },
    { "guide", "Survival Guide", "First aid, water, shelter, signals: offline", "guide" },
    { "compass", "Compass / Level", "Heading, level and steps: live from the sensors", "compass" } };
static void ec_card(dc *d, int i, float top)
{
    /* P3i: no blur of the layer under each card (only the card above's shadow: ~200 ms for nothing seen) */
    d_glass(d, CARD_X, top, CARD_W, 68, 26, 0, 1);
    if (!sv_tile(d, EC[i].icon, CARD_ICON, top + 11, 46)) d_icon(d, EC[i].icon, CARD_ICON, top + 11, 1);
    /* b: 19 / 800 / -.5, line-height 1.1 (half-leading floored); .sub 13.5, .82 */
    d_text(d, TK_IT700, 19, CARD_TX, top + 12.55f + tk_floorf(tk_ascent(TK_IT700, PT(19)) / TK_PT + .5f), EC[i].t, 0, 0, INK, 0);
    /* P3i: the chevron where the systems' is; a long line gives way with an ellipsis (it ran past the card) */
    char sub[96], sb[64];
    ellipsize(sub, sizeof sub, TK_IT400, 13.5f, EC[i].s ? EC[i].s : sv_ec_sub(i, sb), CARD_CHEV - 12 - CARD_TX);
    d_text(d, TK_IT400, 13.5f, CARD_TX, top + 37.44f + tk_floorf(tk_ascent(TK_IT400, PT(13.5f)) / TK_PT + .5f), sub, 0, 0, W(.82f), 0);
    d_icon(d, "chev", CARD_CHEV, top + 27, 1);
}
/* (the first four cards in one layer, each card's backdrop what is drawn before it, as in the list; then Exit, then
 * the layers of cards 4..7: B6's ep_card, a piece at a time, below set_emergency) */
static void free_ecards(void) { for (int i = 0; i < NEC; i++) { relayer(&ecard[i]); ecard[i].hidden = 1; } ec_next = 8; }
static int cards_pending;       /* the platter glass, the vendor cards and the tools still to make again after Emergency (one per idle turn) */
static void build_lockpart(int i) { if (i == 0) build_platter(); else if (i <= NG) build_vcard(i - 1); else build_etool(); }
#define LOCK_PARTS (NG + 2)
static void lkn_free(void);
static void toggle_group(int g, int instant);
/* every open card closed (instant: at once, its rows freed) */
static void vc_close_all(int instant)
{
    for (int g = 0; g < NG; g++) {
        if (V[g].isopen) toggle_group(g, instant);
        if (instant && !V[g].isopen) { V[g].sp.live = 0; V[g].memh = 0; free_open(g); }
    }
}
static void free_vendor_cards(void)
{
    vc_close_all(1);
    lkn_free();                                  /* B9: the lock's notifications (views of the platter glass) */
    for (int g = 0; g < NG; g++) {
        free_open(g);
        vc_unview(g);
        relayer(&V[g].head); V[g].head.hidden = 1;
        relayer(&V[g].chev); V[g].chev.hidden = 1;
        tk_bmp_free(&V[g].chev0);
    }
    relayer(&etool); etool.hidden = 1;
    relayer(&K[0].face); K[0].face.hidden = 1;
    relayer(&K[0].gh); K[0].gh.hidden = 1;
    cards_pending = 0;
    nc_cards_gone();                             /* helper D: no platter may show the freed glass */
}
static void build_empill(void)
{
    /* .empill: 11 / 700 / .08 em, #ffd7d2, padding 4 11, gap 6, rgba(255, 90, 78, .22), a 1 pt inset ring */
    float tw_ = tw(TK_IT700, 11, "EMERGENCY", .88f, 0), w = 11 + 10 + 6 + tw_ + 11, x = 195 - w / 2;
    dc d = mk(&empill, "empill", x, 50, w, 22, 2, 0);
    d_rrect(&d, x, 50, w, 22, 11, TK_RGBA(255, 90, 78, .22f));
    tk_box_shadow(d.b, PT(x) - d.ox, PT(50) - d.oy, PT(w), PT(22), PT(11), 0, 0, 0, PT(1), TK_RGBA(255, 140, 120, .5f), 1);
    d_icon(&d, "em-flame", x + 11, 56, 1);
    d_text(&d, TK_IT700, 11, x + 27, 54 + tk_floorf(tk_ascent(TK_IT700, PT(11)) / TK_PT + .5f), "EMERGENCY", .88f, 0, tk_hex(0xffd7d2, 1), 0);
    empill.hidden = 1;
}

/* place every list layer for the current scroll, heights and row animations (and the notifications, which make
 * room for an open card) */
static void place_list(void)
{
    float y = 10;
    int on = !EMG && K[0].face.bmp.px;
    for (int g = 0; g < NG; g++) {
        vcard_t *k = &V[g];
        float q = stack_q(g), band = SL_BAND * q;
        float dy = PT(y + band - k->y);    /* how far the card moved from where its head was drawn */
        float top = LIST_TOP + y + band;
        int has = on && k->head.bmp.px;
        if (has && vc_glass(&k->face, top, 68)) k->face.alpha = 1;
        else k->face.hidden = 1;
        /* the stack's lower cards (K[0].gh, baked under a 68 pt card at LIST_TOP + K[0].y): B4: the baked peeks hand
         * over to the members' own platters as they come out */
        tk_layer *gh = &k->gh;
        float ga = tk_clampf(1 - q * 5, 0, 1);
        if (has && stack(g) && K[0].gh.bmp.px && ga > 0) {
            if (gh->bmp.px != K[0].gh.bmp.px) nc_view(gh, &K[0].gh);
            gh->x = K[0].gh.bx;
            gh->y = tk_floorf(PT(top - (LIST_TOP + K[0].y)) + K[0].gh.by + .5f);
            gh->alpha = ga;
            gh->hidden = 0;
        } else
            gh->hidden = 1;
        /* B4 (P3j-8): each member springs from its peek (scale .97 / .94, 8 / 15 pt below) to its own slot, staggered
         * (a member's share starts .12 of the fan after the one above), its words in over the last 65 % */
        for (int i = 0; i < 3; i++) {
            tk_layer *v = &k->mpl[i], *r = &k->rows[i];
            if (i >= G[g].n || !r->bmp.px || !has) { v->hidden = 1; continue; }
            float qi = tk_clampf((q - .12f * i) / (1 - .12f * i), 0, 1);
            float sc0 = i ? .94f : .97f, pk = i ? PEEK2 : PEEK1;
            float top0 = top + 68 + pk - 68 * sc0, top1 = top + 68 + 8 + MEM_P * i;
            float sc = sc0 + (1 - sc0) * qi, pt = top0 + (top1 - top0) * qi;
            if (k->sp.live && k->memh > k->full) pt = top1 + (k->memh - k->full) * (i + 1) / G[g].n;   /* (the spring's overshoot) */
            float va = tk_clampf((qi - .06f) * 3, 0, 1);
            if (va > .004f && vc_glass(v, pt, 68)) {
                v->sx = v->sy = sc;
                v->ox = PT(195); v->oy = PT(pt);
                v->alpha = va;
            } else
                v->hidden = 1;
            float rb = LIST_TOP + k->y + 68 + 8 + MEM_P * i;   /* where its words were drawn */
            r->y = r->by + PT(pt - rb);
            r->sx = r->sy = sc;
            r->ox = PT(195); r->oy = PT(pt);
            r->alpha = tk_clampf((qi - .35f) / .65f, 0, 1);
            r->hidden = 0;
        }
        if (k->sl.bmp.px) {
            k->sl.y = k->sl.by + dy;
            k->sl.alpha = tk_clampf((q - .4f) / .6f, 0, 1);
            k->sl.hidden = !has || k->sl.alpha <= .004f;
        }
        k->head.y = k->head.by + dy;
        k->head.hidden = !has;
        k->chev.y = k->chev.by + dy;
        k->chev.clip_r = (tk_rect){ PT(VC_X), PT(top), PT(VC_W), PT(68) };
        k->chev.hidden = !has;
        k->members.hidden = !has;
        y += card_h(g);
    }
    float edy = PT(y - 448);
    etool.y = etool.by + edy;
    etool.hidden = EMG || !etool.bmp.px;
    ecard[0].hidden = ecard[4].hidden = !EMG || ec_wait;
    ecard[5].hidden = !EMG || ec_next <= 4;          /* survival-next: shown from its first card */
    ecard[6].hidden = !EMG || ec_next <= 6;
    list.y = PT(-scroll);
    float lh = EMG ? LIST_H : list_h();
    list.clip_r = (tk_rect){ 0, PT(LIST_TOP), PT(390), PT(lh) };
    /* B10: the notifications fanned out over the list: it gives way (and takes no touches, sh_touch) */
    list.alpha = EMG ? 1 : 1 - tk_clampf(lkn_p() * 1.6f, 0, 1);
    list.hidden = list.alpha <= .004f;
    lkn_place();
    lock_ver++;
}

/* the chevron's .35 s cubic-bezier(.3, 1.4, .4, 1) turn */
static void chev_draw(vcard_t *k, float deg)
{
    if (!k->chev.bmp.px || !k->chev0.px) return;
    tk_bmp_rotate(&k->chev.bmp, &k->chev0, deg);
    tk_layer_dirty(&k->chev);
}

static void toggle_group(int g, int instant)
{
    vcard_t *k = &V[g];
    k->isopen = !k->isopen;
    if (k->isopen) { build_open(g); lkn_collapse(instant); }   /* B10: the notifications fold back out of its way */
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
        tk_spring_start(&k->sp, from, to, 0, TK_SPR_FAN);   /* B4: IOS-REFERENCE 5's fan (.40 / .85) */
    place_list();
}
/* the content top of card g (list pt at scroll 0) */
static float card_top(int g)
{
    float yy = 10;
    for (int h = 0; h < g; h++) yy += card_h(h);
    return yy + SL_BAND * stack_q(g);
}
/* B10: a touch on the list (not past its bottom, nor while the notifications are fanned out over it) */
static int in_list(float y) { return !EMG && lkn_p() <= 0 && y >= LIST_TOP && y <= list_bot(); }
static int hit_head(float x, float y)
{
    if (!in_list(y) || !V[0].head.bmp.px) return -1;
    float cy = y - LIST_TOP + scroll;
    for (int g = 0; g < NG; g++) {
        float top = card_top(g);
        if (cy >= top && cy < top + 68 && x >= VC_X && x < VC_X + VC_W) return g;
        /* B4: an open stack's "Show less" folds it too */
        if (V[g].sl.bmp.px && stack_q(g) > .5f && cy >= top - SL_BAND && cy < top && x >= VC_X + VC_W - 110 && x < VC_X + VC_W) return g;
    }
    return -1;
}
static int hit_row(float x, float y, int *gi)
{
    if (!in_list(y) || x < VC_X + 6 || x > VC_X + VC_W - 6) return -1;
    float cy = y - LIST_TOP + scroll;
    for (int g = 0; g < NG; g++) {
        float yy = card_top(g);
        if (V[g].memh > 1)
            for (int i = 0; i < G[g].n; i++) {
                float rt = yy + 68 + 8 + MEM_P * i;          /* B4: its own platter */
                if (cy >= rt && cy < rt + 68 && cy < yy + 68 + V[g].memh) { *gi = g; return i; }
            }
    }
    return -1;
}

static tk_layer hparts[8], hsearch[3], pages;
/* L3, shell_linux.inc (Linux apps, the App Store): the hooks this file calls */
static int lx_is(const char *k);
static int ap_slim(const char *k);                 /* helper B5: shell_apps.inc */
static int lx_icon(const char *k, float *pad, const tk_bmp **ic);
static void lx_home(void);
/* L3: the Search pill's page dots, the lit one page pg's (redrawn only when it changes: a 40 x 8 pt layer) */
static void home_dots(int pg)
{
    static int lit = -1;
    if (pg == lit && hsearch[2].bmp.px) return;
    lit = pg;
    int hid = hsearch[2].hidden;
    relayer(&hsearch[2]);
    dc d = mk(&hsearch[2], "search-dots", 189.5f, 644, 30, 6, 1, 0);   /* B8: centred in the 24 pt pill at y 647 */
    for (int j = 0; j < 3; j++) d_rrect(&d, 189.5f + 12 * j, 644, 6, 6, 3, pg == j ? W(1) : W(.4f));
    hsearch[2].hidden = hid;
    relink();
}
static int lx_hit(float x0, float y);
static int lx_strips(const char *k);
static void lx_page(dc *d);
static void lx_build(void);
static void lx_open(int a);
static void lx_close(void);
static int lx_tap(float x, float y);
static int lx_touch(int ph, float x, float y);
static int lx_moving(void);
static int lx_tick(float dt);                   /* Track F: the App Store's scroll and page builds over frames */
static int lx_layers(tk_layer **o);
static int lx_set(const char *what, const char *key, float v);
/* P3p: an app's 62 pt icon. Calendar shows today, as on iOS: the blank a62-calendar-bg with the weekday (red,
 * semibold, .14 of the tile) over the date (regular, .45), composed once a day (one 62 pt bitmap, ~210 KB) and
 * used wherever the icon is drawn: home, Search and the opening / closing icon (cal_newday() redraws home's) */
static tk_bmp calic;
static int calic_key = -1, calic_x = -1, calic_y;   /* calic_x, _y: where home's first page drew it (its px) */
static int cal_key(void) { return ST.wday % 7 * 32 + ST.day; }
static const tk_bmp *app_icon(const char *k, float *pad)
{
    const tk_bmp *li;
    if (lx_icon(k, pad, &li)) return li;          /* L3: the App Store's, a Linux app's */
    char nm[24] = "a62-";
    int n = tk_mini(tk_strlen(k), 19);
    tk_memcpy(nm + 4, k, n);
    nm[4 + n] = 0;
    const tk_bmp *bg = tk_strcmp(k, "calendar") ? 0 : tk_icon("a62-calendar-bg");
    if (!bg) { *pad = tk_icon_pad(nm); return tk_icon(nm); }
    *pad = tk_icon_pad("a62-calendar-bg");
    if (calic.px && calic_key == cal_key()) return &calic;
    if (!calic.px && !tk_bmp_new(&calic, bg->w, bg->h)) return bg;
    tk_bmp_copy(&calic, 0, 0, bg, 0, 0, bg->w, bg->h);
    dc c = { &calic, 0, -*pad, -*pad };
    char wd[4], day[4];
    for (int i = 0; i < 3; i++) { char ch = WD[ST.wday % 7][i]; wd[i] = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch; }
    wd[3] = 0;
    day[put_dec(day, ST.day)] = 0;
    d_text(&c, TK_IT600, 8.68f, 31 - tw(TK_IT600, 8.68f, wd, 0, 0) / 2, 18.6f, wd, 0, 0, tk_hex(0xff3b30, 1), 0);
    d_text(&c, TK_IT400, 27.9f, 31 - tw(TK_IT400, 27.9f, day, 0, 0) / 2, 48.36f, day, 0, 0, tk_hex(0x1c1c1e, 1), 0);
    calic_key = cal_key();
    return &calic;
}
static void d_appicon(dc *d, const char *k, float x, float y)
{
    float pad;
    const tk_bmp *ic = app_icon(k, &pad);
    if (!ic) { tk_log(k); return; }
    int X = (int)tk_floorf(PT(x) - pad - d->ox + .5f), Y = (int)tk_floorf(PT(y) - pad - d->oy + .5f);
    tk_blit(d->b, ic, X, Y, 1);
    if (ic == &calic && d->b == &hparts[2].bmp) { calic_x = X; calic_y = Y; }
}
static tk_layer icobg, icofg, lockhl, apphl;   /* P3p: press feedback (below) */
static void ico_free(void);
static void hl_off(void);
static tk_bmp dockbg[4];             /* P3p: the dock's glass under each icon (its press) */
static void build_home_sb(void)
{
    relayer(&hparts[0]);
    dc d = mk(&hparts[0], "home-sb", 0, 0, 390, 48, 0, 0);
    char hm[8];
    fmt_hm(hm);
    status_bar(&d, hm);
    trim(&hparts[0]);
}

/* ---- the home (P1: the concept's first page, drawn once; P2 builds the real one) --------------------------- */
/* P3p (P3j-6): the iOS home lattice: four columns centred at 60 / 150 / 240 / 330 (pitch 90: outer margin = gap =
 * 30 for 60 pt icons; ours are 62), rows 98 apart; the small widgets 158 pt square over the column pairs (26 / 206,
 * content 16 in), the dock's icons on the same columns */
#define COL_CX(c) (60.0f + 90.0f * (c))
#define ICON_PT 62.0f
#define ROW_PITCH 98.0f
#define HOME_TOP1 256.0f            /* page 1's grid under the widgets */
#define HOME_TOP2 60.0f             /* page 2's grid */
#define WID_X(i) ((i) ? 206.0f : 26.0f)
#define WID_W 158.0f
/* B8 (IOS-REFERENCE §9.2, measured): the dock's glass x 11..379, top 702, 88 tall (radius concentric, clamped to
 * 24); its icons 12 below its top. The Search pill 78 x 24 centred on y 647 (the page dots in the same pill) */
#define DOCK_X 11.0f
#define DOCK_TOP 702.0f
#define DOCK_H 88.0f
#define DOCK_Y (DOCK_TOP + 12)
#define SRCH_W 78.0f
#define SRCH_H 24.0f
#define SRCH_X (195 - SRCH_W / 2)
#define SRCH_Y (647 - SRCH_H / 2)
#define SRCH_HIT(x, y) ((y) >= SRCH_Y - 6 && (y) <= SRCH_Y + SRCH_H + 6 && (x) >= SRCH_X - 10 && (x) <= SRCH_X + SRCH_W + 10)
static const char *const P2K[9] = { "contacts", "files", "light", "battery", "terminal", "browser", "medical", "sos", "maps" };   /* helper D: Maps in the free third row */
static const char *const P2E[12] = { "heat", "power", "sos", "medical", "usb", "sun", "guide", "compass", "light", "battery", "contacts", "files" };
#define NP2 (EMG ? 12 : 9)                  /* survival-next: Emergency's page 2 has a third row (the tools); helper D: Maps */
/* helper D: a system's 52 pt tile (the pack has four; the others are its 66 pt tile scaled) */
static void d_systile(dc *d, const char *k, float x, float y)
{
    char nm[40] = "t52-";
    int n = tk_mini(tk_strlen(k), 30);
    tk_memcpy(nm + 4, k, (size_t)n);
    nm[4 + n] = 0;
    if (tk_icon(nm)) { d_icon(d, nm, x, y, 1); return; }
    nm[1] = '6'; nm[2] = '6';
    const tk_bmp *ic = tk_icon(nm);
    if (!ic) return;
    float f = 52.0f / 66, pad = tk_icon_pad(nm) * f;
    nc_blit_scaled(d->b, ic, PT(x) - pad - d->ox, PT(y) - pad - d->oy, f);
}
static void build_home_w(void)
{
    relayer(&hparts[1]);
    dc d = mk(&hparts[1], "home-w", 0, 50, 390, 200, 44, 1);
    d_glass(&d, WID_X(0), 60, WID_W, WID_W, 24, 0, 1);
    d_glass(&d, WID_X(1), 60, WID_W, WID_W, 24, 0, 1);
    /* Battery: .lab, .big, three rows */
    if (EMG) {
        /* Emergency: the battery glyph amber (#ffb547) */
        const tk_bmp *ic = tk_icon("home-batt");
        if (ic) tk_blit_tinted(d.b, ic, (int)tk_floorf(PT(WID_X(0) + 16) - tk_icon_pad("home-batt") - d.ox + .5f),
                               (int)tk_floorf(PT(74 + 1.5f) - tk_icon_pad("home-batt") - d.oy + .5f), tk_hex(0xffb547, 1));
    } else
        d_icon(&d, "home-batt", WID_X(0) + 16, 74 + 1.5f, 1);
    d_text(&d, TK_IT700, 11.5f, WID_X(0) + 34, base_of(TK_IT700, 11.5f, 74, 0), "Galaxy S9", .345f, 0, W(.7f), 0);
    char big[8];
    int bn0 = put_dec(big, ST.pct);
    big[bn0] = '%'; big[bn0 + 1] = 0;
    d_text(&d, TK_IT700, 38, WID_X(0) + 16, base_of(TK_IT700, 38, 74 + 14 + 4, 0), big, -.6f, TK_TNUM, INK, 0);
    {
        /* P2y: the live charge, temperature and charging (the concept's "Phone" and "Lasts" have no source) */
        static const char *L[2] = { "Temperature", "Charging" };
        char tb[12];
        int tn = put_dec(tb, ST.temp);
        tk_memcpy(tb + tn, " \xc2\xb0" "C", 5);
        const char *R[2] = { tb, sys_charging ? "Yes" : "No" };
        float y = 74 + 14 + 4 + 46;
        for (int i = 0; i < 2; i++) {
            y += 3;
            float b = base_of(TK_IT400, 12.5f, y, 0);
            d_text(&d, TK_IT400, 12.5f, WID_X(0) + 16, b, L[i], 0, TK_TNUM, W(.82f), 0);
            d_text(&d, TK_IT400, 12.5f, WID_X(0) + WID_W - 16 - tw(TK_IT400, 12.5f, R[i], 0, TK_TNUM), b, R[i], 0, TK_TNUM, W(.82f), 0);
            y += 15;
        }
    }
    d_icon(&d, "home-star", WID_X(1) + 16, 74 + 1.5f, 1);
    d_text(&d, TK_IT700, 11.5f, WID_X(1) + 34, base_of(TK_IT700, 11.5f, 74, 0), "Recent", .345f, 0, W(.7f), 0);
    {
        /* helper D: the boot menu's last choice first (sh_lastos), then the concept's */
        static const char *T4[4] = { "win11", "ios26", "engineer", "ios16" };
        const char *rk[4];
        int n = 0;
        if (LAST) rk[n++] = LAST;
        for (int i = 0; i < 4 && n < 4; i++) if (!LAST || tk_strcmp(T4[i], LAST)) rk[n++] = T4[i];
        for (int i = 0; i < 4; i++) d_systile(&d, rk[i], WID_X(1) + 16 + (i & 1) * (WID_W - 32 - 52), 74 + 14 + 9 + (i >> 1) * 60);
    }
    float cw = tw(TK_IT400, 12, "Battery", 0, 0);
    d_text(&d, TK_IT400, 12, WID_X(0) + WID_W / 2 - cw / 2, 224 + 12, "Battery", 0, 0, INK, 1);
    cw = tw(TK_IT400, 12, "Systems", 0, 0);
    d_text(&d, TK_IT400, 12, WID_X(1) + WID_W / 2 - cw / 2, 224 + 12, "Systems", 0, 0, INK, 1);
    trim(&hparts[1]);
}
static void build_home_apps2(void)
{
    /* page 2: the second grid (top 62) */
    {
        static const char *P2N_[9] = { "Contacts", "Files", "Flashlight", "Battery", "Terminal", "Browser", "Medical Card", "SOS", "Maps" };
        static const char *P2EN[12] = { "Survival Heat", "Last 2 %", "SOS", "Medical Card", "USB Power", "Sun", "Guide", "Compass",
                                        "Flashlight", "Battery", "Contacts", "Files" };
        const char *const *P2 = EMG ? P2E : P2K, *const *P2N = EMG ? P2EN : P2N_;
        relayer(&hparts[4]);
        relayer(&hparts[6]);
        /* drawn as if on screen, then moved one page to the right (layers never extend past the panel's sides).
         * survival-next (the merge): Emergency's third row (the tools) in a layer of its own (one 7 MB layer was refused in the 160 MiB
         * heap's holes; 5 + 1.5 MB fit) */
        dc d = mk(&hparts[4], "home-apps2", 0, 55, 390, 220, 4, 0), d3 = { 0 };
        if (NP2 > 8) d3 = mk(&hparts[6], "home-apps2e", 0, HOME_TOP2 + 2 * ROW_PITCH - 5, 390, 96, 4, 0);
        hparts[6].hidden = NP2 <= 8;
        for (int i = 0; i < NP2; i++) {
            float cx = COL_CX(i % 4), y = HOME_TOP2 + ROW_PITCH * (i / 4);
            dc *dd = i < 8 ? &d : &d3;
            if (!dd->b || !dd->b->px) continue;
            char n[40] = "a62-";
            tk_memcpy(n + 4, P2[i], tk_strlen(P2[i]) + 1);
            if (!sv_tile(dd, P2[i], cx - 31, y, 62)) d_icon(dd, n, cx - 31, y, 1);
            d_text_box(dd, TK_IT400, 12, cx, y + 68 + 12, 84, 1, P2N[i], 0, 0, INK, 1);   /* P3p: one line, at most 84 pt */
        }
        for (int k = 4; k <= 6; k += 2) { hparts[k].x = hparts[k].bx = hparts[k].x + PT(390); trim(&hparts[k]); }
    }
}
/* B8 (GAP-LIST #21): the dock's badges, iOS's: a red (#FF4245) 18 pt circle (wider for more digits) with the count
 * 12 / 600, its top-right corner 5 pt out from the icon's. Only where a count exists: shell_comms.inc's cm_badge()
 * (Phone: missed calls not yet seen in Recents; Messages: unread). Their own small layer over the dock, redrawn only
 * when a count changes (from home's build and sh_idle: text and two discs, no glass), freed when there are none */
static tk_layer hbadge;
static int cm_badge(const char *k);
static void home_badges(int force)
{
    static const char *const BK[2] = { "phone", "messages" };          /* the dock's columns 0 and 1 */
    static int shown[2] = { -1, -1 };
    int n[2] = { cm_badge(BK[0]), cm_badge(BK[1]) };
    if (!force && n[0] == shown[0] && n[1] == shown[1]) return;
    shown[0] = n[0]; shown[1] = n[1];
    relayer(&hbadge);
    if (!n[0] && !n[1]) { tk_layer_init(&hbadge, "home-badges"); hbadge.hidden = 1; relink(); return; }
    dc d = mk(&hbadge, "home-badges", COL_CX(0) - ICON_PT / 2, DOCK_Y - 5, COL_CX(1) - COL_CX(0) + ICON_PT + 40, 18, 1, 0);
    for (int i = 0; i < 2; i++) {
        if (!n[i]) continue;
        char b[8];
        b[put_dec(b, tk_mini(n[i], 999))] = 0;
        float w = tw(TK_IT600, 12, b, 0, TK_TNUM), bw = tk_maxf(18, w + 11), r = COL_CX(i) + ICON_PT / 2 + 5;
        d_rrect(&d, r - bw, DOCK_Y - 5, bw, 18, 9, C_RED);
        d_text(&d, TK_IT600, 12, r - bw / 2 - w / 2, base_of(TK_IT600, 12, DOCK_Y - 5, 18), b, 0, TK_TNUM, W(1), 0);
    }
    trim(&hbadge);
    relink();
}
static void build_wclock(void);
static void build_home(void)
{
    static const char *P1[8] = { "calendar", "weather", "clock", "camera", "photos", "music", "notes", "calc" };
    static const char *P1N[8] = { "Calendar", "Weather", "Clock", "Camera", "Photos", "Music", "Notes", "Calculator" };
    static const char *DK[4] = { "phone", "messages", "browser", "settings" };
    build_home_sb();
    dc d;
    build_home_w();
    d = mk(&hparts[2], "home-apps", 0, 250, 390, 220, 4, 0);
    for (int i = 0; i < 8; i++) {
        float cx = COL_CX(i % 4), y = HOME_TOP1 + ROW_PITCH * (i / 4);
        d_appicon(&d, P1[i], cx - 31, y);
        d_text_box(&d, TK_IT400, 12, cx, y + 68 + 12, 84, 1, P1N[i], 0, 0, INK, 1);   /* P3p: one line, at most 84 pt */
    }
    build_home_apps2();
    build_wclock();                             /* helper D: page 1's Clock widget */
    lx_home();                                  /* L3: page 3, the Linux apps */
    /* the Search pill, four ways: the word (at rest on a page), the page dots (moving, one of three pages lit) */
    {
        /* B8 (§9.2): one 78 x 24 capsule, the glyph and the word centred in it */
        float cw = 13 + 5 + tw(TK_IT600, 13, "Search", 0, 0), cx0 = 195 - cw / 2;
        d = mk(&hsearch[0], "search", SRCH_X, SRCH_Y, SRCH_W, SRCH_H, 44, 1);
        d_glass(&d, SRCH_X, SRCH_Y, SRCH_W, SRCH_H, SRCH_H / 2, 1, 1);
        d_icon(&d, "search", cx0, 647 - 6.5f, 1);
        d_text(&d, TK_IT600, 13, cx0 + 18, base_of(TK_IT600, 13, SRCH_Y, SRCH_H), "Search", 0, 0, INK, 1);
        /* L3: one pill for the dots and a small layer of the three dots over it (home_dots: the lit one follows the
         * page), not a pill per page */
        d = mk(&hsearch[1], "search-pill", SRCH_X, SRCH_Y, SRCH_W, SRCH_H, 44, 1);
        d_glass(&d, SRCH_X, SRCH_Y, SRCH_W, SRCH_H, SRCH_H / 2, 1, 1);
        d_icon(&d, "search", SRCH_X + 14.5f, 647 - 6.5f, 1);
        for (int k = 0; k < 2; k++) tk_a8_free(&hsearch[k].glass);
        home_dots(0);
        hsearch[1].hidden = hsearch[2].hidden = 1;
    }
    d = mk(&hparts[3], "home-dock", 0, 650, 390, 152, 0, 1);
    d_glass(&d, DOCK_X, DOCK_TOP, 390 - 2 * DOCK_X, DOCK_H, CONCENTRIC(DOCK_X), 0, 1);   /* B8 (§9.2): iOS's inset 11, concentric */
    for (int i = 0; i < 4; i++) {
        float x = COL_CX(i) - ICON_PT / 2;
        char n[40] = "a62-";
        tk_memcpy(n + 4, DK[i], tk_strlen(DK[i]) + 1);
        {
            /* P3p (P3j-7): the dock's glass under this icon, kept (210 KB each) for its press (the icon shrinks) */
            const tk_bmp *ic = tk_icon(n);
            int X = (int)tk_floorf(PT(x) - tk_icon_pad(n) - d.ox + .5f), Y = (int)tk_floorf(PT(DOCK_Y) - tk_icon_pad(n) - d.oy + .5f);
            tk_bmp_free(&dockbg[i]);
            if (ic && tk_bmp_new(&dockbg[i], ic->w, ic->h)) tk_bmp_copy(&dockbg[i], 0, 0, d.b, X, Y, ic->w, ic->h);
        }
        d_icon(&d, n, x, DOCK_Y, 1);
    }
    home_badges(1);                              /* B8 (GAP #21): Phone's missed calls, Messages' unread */
    /* the parts stay separate layers under one group (they do not overlap, so the group's opacity per part is
     * exact); #home has will-change: opacity, a backdrop root in Chrome: its glass shows no wallpaper blur. v6:
     * Liquid Glass refracts the wallpaper under the widgets, the dock and the Search pill (from their shapes:
     * the masks go, as before) */
    for (int i = 0; i < 5; i++) tk_a8_free(&hparts[i].glass);
    /* P3s: cut to what they hold (Calendar's tile is redrawn in place later: its spot moves with the cut) */
    {
        float ox = hparts[2].x, oy = hparts[2].y;
        trim(&hparts[2]);
        if (calic_x >= 0) { calic_x -= (int)(hparts[2].x - ox); calic_y -= (int)(hparts[2].y - oy); }
    }
    trim(&hparts[3]);
    trim(&hsearch[0]);
    trim(&hsearch[1]);
}

static float focus_sy;

/* ---- the focus sheet --------------------------------------------------------------------------------------- */
/* P3p (audit 15): MARGIN from the sides; the slide to start 20 pt inside it */
#define FOC_X MARGIN
#define FOC_W CONTENT_W
/* P3p (P3j-19): an iOS sheet: from the bottom (8 pt above it), its corners concentric with the display's */
#define FOC_H 267.0f
#define FOC_Y (H_PT - 8 - FOC_H)
#define FOC_R CONCENTRIC(8)
#define SL_X (FOC_X + 20)
#define SL_W (FOC_W - 40)
static tk_layer focus, funder, scrim, fgrp, fcard, ffill, flabel, fthumb;
/* the sheet's glass (geometry only: the same for every system) and its slider's (two places: 2 or 3 facts): made in
 * idle time while a card is open on the lock (a row's tap opens the sheet), freed when none is (P3s: 10 MB were
 * kept from the first sheet on) */
static tk_layer ffacel, fslide[2];
static float slider_y(int nf) { return FOC_Y + 102 + 17 * nf + 7 * (nf - 1) + 18; }
static int focus_nf(const sys_t *s) { return !tk_strcmp(s->k, LAST) ? 3 : 2; }
static void bake_focus_face(void)
{
    if (ffacel.bmp.px) return;
    dc fd = mk(&ffacel, "fcard-face", FOC_X, FOC_Y, FOC_W, FOC_H, 44, 0);
    d_glass(&fd, FOC_X, FOC_Y, FOC_W, FOC_H, FOC_R, 0, 1);
}
static void bake_slider_face(int nf)
{
    tk_layer *sf = &fslide[nf == 3];
    if (sf->bmp.px) return;
    float sy = slider_y(nf);
    dc fd = mk(sf, "slider-face", SL_X, sy, SL_W, 62, 44, 0);
    d_glass(&fd, SL_X, sy, SL_W, 62, 31, 1, 1);
}
static int card_open(void) { return vc_any_open(); }
static int focus_faces_keep(void) { return locked && !EMG && (card_open() || focus_on); }
static int focus_faces_missing(void)
{
    if (!focus_faces_keep()) return 0;
    if (!ffacel.bmp.px) return 1;
    for (int g = 0; g < NG; g++)
        if (V[g].isopen)
            for (int i = 0; i < G[g].n; i++) if (!fslide[focus_nf(&G[g].it[i]) == 3].bmp.px) return 1;
    return 0;
}
/* idle time: what the next sheet needs (one glass a turn: 1 if one was made), or nothing kept if no card is open */
static int focus_faces_idle(int keep)
{
    if (!keep) { relayer(&ffacel); relayer(&fslide[0]); relayer(&fslide[1]); return 0; }
    if (!ffacel.bmp.px) { bake_focus_face(); return 1; }
    for (int g = 0; g < NG; g++) {
        if (!V[g].isopen) continue;
        for (int i = 0; i < G[g].n; i++)
            if (!fslide[focus_nf(&G[g].it[i]) == 3].bmp.px) { bake_slider_face(focus_nf(&G[g].it[i])); return 1; }
    }
    return 0;
}
/* a closed sheet keeps nothing of its own (about 19 MB) */
static void free_focus(void)
{
    relayer(&scrim); relayer(&funder); relayer(&fcard); relayer(&ffill); relayer(&flabel); relayer(&fthumb);
    focus.child = 0;
}
static void free_cc_all(void);
static void build_focus(const sys_t *s, const grp_t *gr)
{
    free_cc_all();                           /* B4: the lock's Control Centre makes room for the sheet */
    /* the scrim: what is on the panel now, blurred 22 pt, saturated 1.3, under rgba(4, 8, 26, .28) */
    tk_comp_frame(C, 0, 0);                  /* the frame in RAM is current (only what changed is drawn) */
    relayer(&scrim);
    tk_layer_init(&scrim, "scrim");
    /* half resolution, drawn 2x (it is a blur): 4 MB instead of 17 */
    tk_bmp_new(&scrim.bmp, TK_W / 2, TK_H / 2);
    scrim.sx = scrim.sy = 2;
    {
        tk_bmp q;
        tk_bmp_new(&q, TK_W / 4, TK_H / 4);
        tk_bmp_down2(&scrim.bmp, shown_frame());
        tk_bmp_down2(&q, &scrim.bmp);
        tk_bmp_blur(&q, PT(22) / 4);
        tk_bmp_satbright(&q, 1.3f, 1);
        /* the tint over it at this size (a constant over a smooth image: the same after the bilinear up) */
        tk_bmp_tint(&q, TK_RGBA(0, 0, 0, .2f));       /* P3p (P3j-19): a neutral dim */
        tk_bmp_up(&scrim.bmp, &q, 0, 0, 2);
        tk_bmp_free(&q);
    }
    scrim.opaque = 1;                        /* nothing under it needs drawing once it is fully in */
    /* what is under the sheet does not change while it is open: the screen as it is now (the half canvas follows
     * every rest frame), one opaque layer, so a frame draws it and the scrim instead of every lock layer */
    relayer(&funder);
    tk_layer_init(&funder, "focus-under");
    if (tk_bmp_new(&funder.bmp, TK_W / 2, TK_H / 2)) {
        tk_bmp_copy(&funder.bmp, 0, 0, &C->half, 0, 0, TK_W / 2, TK_H / 2);
        funder.sx = funder.sy = 2;
        funder.opaque = 1;
    }
    /* the card: its backdrop is the scrim filtered again (blur 14, saturate 1.9, brightness 1.06), taken once at
     * rest and moved with the card while it springs in (Chrome filters the live scene under it each frame) */
    float sy = 0;
    uint64_t tb0 = tk_us();
    for (int pass = 0; pass < 1; pass++) {
        tk_layer *L = &fcard;
        relayer(L);
        dc d = mk(L, "fcard", FOC_X, FOC_Y, FOC_W, FOC_H, 44, 0);
        if (!pass) {
            /* straight from the half-resolution scrim (it is already a blur) */
            tk_glass_from_half(d.b, (int)d.ox, (int)d.oy, &scrim.bmp, PT(FOC_X) - d.ox, PT(FOC_Y) - d.oy, PT(FOC_W), PT(FOC_H), PT(FOC_R));
        }
        /* the card's glass (geometry only: the same for every system), made in idle time while the card was open */
        {
            tk_layer *face = &ffacel;
            bake_focus_face();
            tk_blit(d.b, &face->bmp, (int)(face->x - d.ox), (int)(face->y - d.oy), 1);
        }
        lap_us("focus: the card's backdrop and glass", tk_us() - tb0);
        char tn[40] = "t66-";
        tk_memcpy(tn + 4, s->k, tk_strlen(s->k) + 1);
        d_rrect(&d, 195 - 18, FOC_Y + 5, 36, 5, 2.5f, W(.3f));     /* P3p: the sheet's grabber */
        d_icon(&d, tn, SL_X, FOC_Y + 20, 1);
        d_text(&d, TK_IT600, 13, FOC_X + 100, FOC_Y + 29.16f + 13, gr->name, 0, 0, W(.62f), 0);
        d_text(&d, TK_IT700, 27, FOC_X + 100, base_of(TK_IT700, 27, FOC_Y + 47.16f, 29.7f), s->name, 0, 0, INK, 0);
        int last = !tk_strcmp(s->k, LAST);
        const char *dt[3] = { "Version", last ? "Last used" : "When it shuts down", "When it shuts down" };
        const char *dd[3] = { s->meta, last ? "Today" : "Back to Starbite", "Back to Starbite" };
        int nf = focus_nf(s);
        float dtw = 0;
        for (int i = 0; i < nf; i++) dtw = tk_maxf(dtw, tw(TK_IT400, 14.5f, dt[i], 0, 0));
        for (int i = 0; i < nf; i++) {
            float b = FOC_Y + 102 + 24 * i + 14;
            d_text(&d, TK_IT400, 14.5f, SL_X, b, dt[i], 0, 0, C_LABEL2, 0);
            d_text_box(&d, TK_IT400, 14.5f, SL_X + dtw + 14, b, SL_W - dtw - 14, 0, dd[i], 0, TK_TNUM, INK, 0);
        }
        sy = slider_y(nf);
        /* the slider: dark glass whose backdrop is the card under it; its glass kept per position (2 or 3 facts) */
        tk_glass_local(d.b, d.b, PT(SL_X) - d.ox, PT(sy) - d.oy, PT(SL_W), PT(62), PT(31));
        {
            tk_layer *sf = &fslide[nf == 3];
            bake_slider_face(nf);
            tk_blit(d.b, &sf->bmp, (int)(sf->x - d.ox), (int)(sf->y - d.oy), 1);
        }
    }
    relayer(&ffill);
    focus_sy = sy;
    dc f = mk(&ffill, "fill", SL_X, sy, 62, 62, 1, 0);
    d_rrect(&f, SL_X, sy, 62, 62, 31, sbca(SBC_ACCENT, .28f));
    ffill.slice = 2;
    ffill.slice_s0 = PT(32);
    ffill.slice_s1 = PT(32);
    relayer(&flabel);
    dc l = mk(&flabel, "label", SL_X, sy, SL_W, 62, 1, 0);
    {
        char lbl[96] = "slide to start ";
        tk_memcpy(lbl + 15, s->name, tk_strlen(s->name) + 1);
        float w = tw(TK_IT600, 16.5f, lbl, 0, 0);
        /* background: 90 deg .4 -> 1 @45 % -> .4 @60 % over 200 % of the box, position 0 (the shine at rest) */
        tk_stop st[3] = { { 0, W(.4f) }, { .45f, W(1) }, { .6f, W(.4f) } };
        tk_paint p = tk_linear(90, PT(SL_X) - l.ox, PT(sy) - l.oy, PT(2 * SL_W), PT(62), 3, st);
        tk_text(l.b, TK_IT600, PT(16.5f), PT(SL_X + 44 + (SL_W - 44 - w) / 2) - l.ox, PT(base_of(TK_IT600, 16.5f, sy + 21, 0)) - l.oy, lbl, 0, 0, &p, 0);
    }
    relayer(&fthumb);
    dc t = mk(&fthumb, "thumb", SL_X + 5, sy + 5, 52, 52, 20, 0);
    tk_box_shadow(t.b, PT(SL_X + 5) - t.ox, PT(sy + 5) - t.oy, PT(52), PT(52), PT(26), 0, PT(2), PT(12), 0, TK_RGBA(0, 0, 0, .35f), 0);
    d_rrect(&t, SL_X + 5, sy + 5, 52, 52, 26, W(1));
    d_icon(&t, "arrow", SL_X + 20, sy + 20, 1);
    tk_layer_init(&fgrp, "fgrp");
    fgrp.ox = PT(195);
    fgrp.oy = PT(FOC_Y + .4f * FOC_H);
    tk_layer_add(&fgrp, &fcard);
    tk_layer_add(&fgrp, &ffill);
    tk_layer_add(&fgrp, &flabel);
    tk_layer_add(&fgrp, &fthumb);
    focus.child = 0;
    if (funder.bmp.px) tk_layer_add(&focus, &funder);
    tk_layer_add(&focus, &scrim);
    tk_layer_add(&focus, &fgrp);
    focus_sys = s;
    thumb_x = 0;
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
    /* P3p (P3j-19): the sheet rises from below the screen (no scale, no fade but its first frames) */
    fgrp.alpha = tk_clampf(p * 4, 0, 1);
    fgrp.sx = fgrp.sy = 1;
    fgrp.y = PT((1 - p) * (FOC_H + 8 + 24));
    focus.hidden = !focus_on;
}

/* the lock as one cached layer for the unlock (its opacity applies to it as a whole, as CSS group opacity), made
 * while nothing moves so the swipe's first frame does not pay for it */
/* P3p (P3j-9): iOS's cover sheet is opaque and slides off whole: the flattened lock gets its own ground under its
 * content (the wallpaper, or the glass backdrop where its glass is, as the compositor would show them; the star),
 * so it needs no glass of its own and covers what is under it (home settling in) */
static tk_a8 flatmask;
static uint32_t flat_ground = ~0u;
typedef struct { tk_bmp *o; const tk_a8 *m; } under_ctx;
static void under_rows(void *v, int y0, int y1)
{
    under_ctx *U = v;
    for (int j = y0; j < y1 && j < C->base_half.h; j++) {
        uint32_t *o = U->o->px + (size_t)j * U->o->stride;
        const uint32_t *bs = C->base_half.px + (size_t)j * C->base_half.stride, *bw = C->bw_half.px + (size_t)j * C->bw_half.stride;
        const uint8_t *m = U->m && U->m->v ? U->m->v + (size_t)j * U->m->w : 0;
        for (int i = 0; i < U->o->w; i++) {
            uint32_t s = o[i], a = s >> 24;
            if (a == 255) continue;
            uint32_t b = bs[i], w = m ? m[i] : 0, r = 0;
            if (w) { uint32_t q = tk_flat_lens(C, i, j, bw[i]), t = 0;   /* P4: the lens's glass (else the backdrop) */ for (int sh = 0; sh < 32; sh += 8) t |= (((b >> sh & 255) * (255 - w) + (q >> sh & 255) * w + 127) / 255) << sh; b = t; }
            for (int sh = 0; sh < 32; sh += 8) r |= ((s >> sh & 255) + ((b >> sh & 255) * (255 - a) + 127) / 255) << sh;
            o[i] = r | 0xff000000u;
        }
    }
}
static void flatten_lock(int ground)          /* ground: also when the wallpaper changed (idle; not mid-swipe) */
{
    if (flat_ver == lock_ver && (!ground || flat_ground == C->ground_gen)) return;
    tk_a8_new(&flatmask, TK_W / 2, TK_H / 2);    /* P3p: the glass goes into its own ground (P3s: made for this only) */
    tk_flatten(&lockg, &lockflat.bmp, &flatmask, .5f);
    under_ctx U = { &lockflat.bmp, &flatmask };
    tk_par_rows(0, lockflat.bmp.h, under_rows, &U);
    tk_a8_free(&flatmask);
    /* the star (nothing of the lock's is drawn where it is) */
    tk_layer *s[2] = { &star, &star_e };
    for (int i = 0; i < 2; i++) {
        if (s[i]->hidden || s[i]->alpha <= 0 || !s[i]->bmp.px) continue;
        tk_layer_prepare(s[i]);
        if (s[i]->half.px) tk_blit(&lockflat.bmp, &s[i]->half, (int)s[i]->x / 2, (int)s[i]->y / 2, s[i]->alpha);
    }
    flat_ver = lock_ver;
    flat_ground = C->ground_gen;
    tk_layer_dirty(&lockflat);
}

/* ---- home's pages: the grid slides, the Search pill shows the page dots while it moves ---------------------- */
static float page_x;                    /* pt: 0 page 1, -390 page 2 */
static tk_spring sp_page;
static int page_dir, page_dragging;
static int cc_stale;
static void set_page(float x)
{
    cc_stale = 1;
    page_x = x;
    pages.x = tk_floorf(PT(x) + .5f);          /* P3i (audit 9): whole px, no resampling of the icons */
    float p = -x / 390;
    float fr = p - tk_floorf(p + .5f);
    int moving = tk_fabsf(fr) > .02f || page_dragging;
    hsearch[0].hidden = moving;
    hsearch[1].hidden = hsearch[2].hidden = !moving;
    if (moving) home_dots(tk_mini(2, tk_maxi(0, (int)tk_floorf(p + .5f))));   /* L3: three pages */
}


/* ---- the Control Centre (the concept's #cc): pulled down from home's top-right corner -------------------------
 * A scrim (home blurred, a neutral dim) and the grid. B4: the grid on docs/design/IOS-REFERENCE.md 1's lattice (unit 68,
 * gap 14.5, pitch 82.5, x0 37.25, first row 119): the connectivity 2x2 (Airplane, Cellular, Wi-Fi, a cluster of four mini
 * circles), the Brightness and Volume sliders, eight round modules. The grid's glass and contents are still one bitmap
 * baked while home is at rest (and again when a tile changes), but each module is its own layer: a view of that bitmap
 * clipped to the module's cell (the cells tile the bitmap, so at rest the picture is the same and nothing is copied).
 * The modules fly in by row (top first), each from scale .9 and 30 pt higher, on the morph spring (.40 / .80) driven
 * by the pull; the close is the fast spring (.32 / 1.0). A long press (450 ms) on the connectivity module or a slider
 * morphs it into its expanded panel (solid layers and one small drawing at the threshold: no glass bake). */
static tk_layer ccl, ccscrim, ccsb, ccg, ccgrid, cclvl[2], ccic[2], cclit;   /* cclit: the torch's lit tile (P3i) */
static tk_layer ccem;                   /* B6: Emergency's red tile (Emergency mode switches it: no grid rebuild) */
static tk_layer ccblur1;            /* P3p (P3j-20): a lighter blur (8 pt, quarter resolution) for the first part of the pull */
static void cc_status_stale(void) { relayer(&ccsb); cc_stale = 1; }
static struct { int on, built, closing, wifi, cell, bt, air, rot, save; float p, lvl[2]; } CCS = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, { .62f, .4f } };   /* P2y: Wi-Fi and rotation lock off (neither is real) */
static void cc_levels(void);
static int cc_bright;           /* the panel's brightness level the slider last sent (1..4; 0: none yet) */
/* the platform's brightness now (1..4; 0: the boot's own): the slider starts there */
void sh_bright_now(int level)
{
    if (level < 1 || level > 4) return;
    cc_bright = level;
    CCS.lvl[0] = (level - .5f) / 4;
    if (CCS.built) cc_levels();
}
static tk_spring sp_cc;
#define CC_U 68.0f
#define CC_GAP 14.5f
#define CC_P (CC_U + CC_GAP)
#define CC_W2 (2 * CC_U + CC_GAP)                 /* a 2x2 / a tall slider's length: 150.5 */
#define CC_COL(c) (37.25f + CC_P * (c))
#define CC_ROW(r) (119.0f + CC_P * (r))
#define CC_R3 CC_ROW(2)                            /* the round modules' rows (under the 2x2 and the sliders) */
#define CC_R4 CC_ROW(3)
static const float CCX[4] = { CC_COL(0), CC_COL(1), CC_COL(2), CC_COL(3) };
#define CC_NV 11                                   /* the views: 0 connectivity, 1 Brightness, 2 Volume, 3..6 row 3, 7..10 row 4 */
#define CC_NM 12                                   /* + 11: the chrome (the + and Power circles, the page icons) */
static tk_layer ccmod[CC_NM], ccview[CC_NV], cctop, ccpg;
static void *cc_vhalf[CC_NV];                      /* the half copy each view was given (another one is the view's own) */
/* the connectivity module's circles: Airplane, Cellular, Wi-Fi (55.6 pt), and the mini cluster (24.7 pt) */
#define CN_D 55.6f
#define CN_PAD ((CC_W2 - 2 * CN_D - 14) / 2)
#define CN_MD 24.7f
static void cc_mod_rect(int i, float *r)
{
    if (i == 11) { r[0] = 37.25f; r[1] = 16; r[2] = 315.5f; r[3] = 28; r[4] = 14; return; }
    if (i == 0) { r[0] = CC_COL(0); r[1] = CC_ROW(0); r[2] = r[3] = CC_W2; r[4] = 30; return; }
    if (i < 3) { r[0] = CC_COL(i + 1); r[1] = CC_ROW(0); r[2] = CC_U; r[3] = CC_W2; r[4] = 34; return; }
    r[0] = CCX[(i - 3) % 4]; r[1] = i < 7 ? CC_R3 : CC_R4; r[2] = r[3] = CC_U; r[4] = 34;
}
/* the cell a module's view shows (they tile the bitmap: the gaps split down the middle) */
static void cc_mod_cell(int i, float *c)
{
    static const float XB[5] = { -400, CC_COL(1) - CC_GAP / 2, CC_COL(2) - CC_GAP / 2, CC_COL(3) - CC_GAP / 2, 800 };
    int c0 = i == 0 ? 0 : i < 3 ? i + 1 : (i - 3) % 4, c1 = i == 0 ? 2 : c0 + 1;
    c[0] = XB[c0]; c[2] = XB[c1];
    c[1] = i < 3 ? -400 : i < 7 ? CC_R3 - CC_GAP / 2 : CC_R4 - CC_GAP / 2;
    c[3] = i < 3 ? CC_R3 - CC_GAP / 2 : i < 7 ? CC_R4 - CC_GAP / 2 : 1200;
}
static void nc_blit_scaled(tk_bmp *dst, const tk_bmp *src, float X, float Y, float f);
/* a glyph at scale s, its box's top-left at (x, y) pt */
static void d_icon_s(dc *d, const char *name, float x, float y, float s)
{
    const tk_bmp *ic = tk_icon(name);
    if (!ic) return;
    float pad = tk_icon_pad(name);
    nc_blit_scaled(d->b, ic, PT(x) - pad * s - d->ox, PT(y) - pad * s - d->oy, s);
}
static void gauge_arc(dc *d, float cx, float cy, float r, float sw, float sweep, tk_col c);
static void d_seg(dc *d, float x0, float y0, float x1, float y1, float sw, tk_col c);
/* an arc from start (radians clockwise from 12 o'clock) through sweep, sw wide, round caps */
static float bl(int f, float size, float top);
static void cc_arc(dc *d, float cx, float cy, float r, float sw, float start, float sweep, tk_col c)
{
    int bx = (int)tk_floorf(PT(cx - r - sw) - d->ox) - 2, by = (int)tk_floorf(PT(cy - r - sw) - d->oy) - 2;
    int n = (int)PT(2 * (r + sw)) + 5;
    tk_fm m;
    if (!tk_fm_new(&m, n, n)) return;
    tk_fm_arc(&m, d->ox + bx, d->oy + by, PT(cx), PT(cy), PT(r), PT(sw), start, sweep, 1);
    tk_paint pp = tk_solid(c);
    tk_draw_fm(d->b, &m, bx, by, &pp, 1);
    tk_fm_free(&m);
}
/* the links that have no icon in the pack, drawn: Nearby sharing (AirDrop's rings), Personal Hotspot (a chain), VPN */
static void cc_draw_link(dc *d, int k, float cx, float cy, float s, tk_col c)
{
    if (k == 0) {
        for (int i = 1; i <= 3; i++) gauge_arc(d, cx, cy + 1.5f * s, 3.4f * i * s, 1.5f * s, 4.712389f, c);
        d_rrect(d, cx - 1.4f * s, cy + .1f * s, 2.8f * s, 2.8f * s, 1.4f * s, c);
    } else if (k == 1) {
        d_ring(d, cx - 3.2f * s, cy, 4.2f * s, 1.6f * s, 1, c, 0);
        d_ring(d, cx + 3.2f * s, cy, 4.2f * s, 1.6f * s, 1, c, 0);
    } else {
        float w = tw(TK_IT700, 7 * s, "VPN", 0, 0);
        d_text(d, TK_IT700, 7 * s, cx - w / 2, cy + 2.5f * s, "VPN", 0, 0, c, 0);
    }
}
static int data_on(void);         /* shell_cell.inc */
static void cc_tile(dc *d, float x, float y, float w, float h, float r, int on)
{
    /* Track A: with the GPU compositor the tile's lens is live (its backdrop made each frame from what is under it):
     * an opaque baked backdrop in the tile layer would hide it. The CPU path keeps the baked one. */
    if (!C || !C->gpu) tk_glass_from_half(d->b, (int)d->ox, (int)d->oy, &ccscrim.bmp, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r));
    d_glass(d, x, y, w, h, r, 0, 1);
    if (on) d_rrect(d, x, y, w, h, r, W(1));
}
static void cc_glyph(dc *d, const char *name, float tx, float ty, float w, float h)
{
    d_icon(d, name, tx + (CC_U - w) / 2, ty + (CC_U - h) / 2, 1);
}
__attribute__((unused)) static void cc_label(dc *d, const char *t, float tx, float ty)
{
    float w = tw(TK_IT600, 10.5f, t, 0, 0);
    d_text(d, TK_IT600, 10.5f, tx + 35 - w / 2, ty + 74 + 10, t, 0, 0, W(.8f), 0);
}
/* the expanded module (B4): -1 none, 0 connectivity, 1 Brightness, 2 Volume; e its morph (0 the module, 1 the panel) */
static tk_layer ccx, ccxdim, ccxbg, ccxlvl, ccxc;
static struct { int m, closing, sig, todo; float e; tk_spring sp; } CX = { -1, 0, 0, 6, 0, { 0, 0, 0, 0, 0, 0, 0 } };   /* todo: links still to draw */
static const float CXR[3][5] = { { 36, 200, 318, 262, 38 }, { 120, 160, 150, 400, 44 }, { 120, 160, 150, 400, 44 } };
/* the press (the glass spec's gel, IOS-REFERENCE 1): a module under the finger grows by min(.12, 10 / its short side),
 * a slider being dragged to 1.05, on the press spring; back to 1 when it is let go */
static tk_spring cc_ps[CC_NM];
static float cc_pscale[CC_NM] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static int cc_pressed = -1;
static int cc_edge[2], cc_step[2];
static void cc_link(void)
{
    if (!ccx.name) { tk_layer_init(&ccx, "cc-expanded"); ccx.hidden = 1; }
    tk_layer *g[CC_NM];
    for (int i = 0; i < CC_NM; i++) {
        tk_layer *m = &ccmod[i];
        if (!m->name) tk_layer_init(m, "cc-mod");
        g[i] = m;
    }
    for (int i = 0; i < CC_NV; i++) {
        tk_layer *v = &ccview[i];
        if (v->half.px && v->half.px != cc_vhalf[i]) tk_layer_dirty(v);   /* a half copy the view made itself */
        tk_layer_init(v, "cc-module");
        v->bmp = ccgrid.bmp;
        v->half = ccgrid.half;
        v->hspan = ccgrid.hspan;
        cc_vhalf[i] = ccgrid.half.px;
        v->x = ccgrid.x; v->y = ccgrid.y;
        float c[4];
        cc_mod_cell(i, c);
        v->clip = 1;
        v->clip_r = (tk_rect){ PT(c[0]), PT(c[1]), PT(c[2] - c[0]), PT(c[3] - c[1]) };
    }
    link(&ccg, g, CC_NM);
    tk_layer *ch[] = { &cctop, &ccpg };
    link(&ccmod[11], ch, 2);
    for (int i = 0; i < CC_NV; i++) {
        tk_layer *k[3] = { &ccview[i], 0, 0 };
        int n = 1;
        if (i == 1 || i == 2) { k[1] = &cclvl[i - 1]; k[2] = &ccic[i - 1]; n = 3; }
        if (i == 3) { k[1] = &cclit; n = 2; }
        if (i == 5) { k[1] = &ccem; n = 2; }       /* B6: Emergency's red tile, its own small layer */
        link(&ccmod[i], k, n);
    }
    ccem.hidden = !EMG || !ccem.bmp.px;
    cclit.hidden = !Q[0].lit || !cclit.bmp.px;
    tk_layer *xb[] = { &ccxlvl };
    link(&ccxbg, xb, 1);
    tk_layer *x[] = { &ccxdim, &ccxbg, &ccxc };
    link(&ccx, x, 3);
    tk_layer *k[] = { &ccblur1, &ccscrim, &ccsb, &ccg, &ccx };
    link(&ccl, k, 5);
}
/* the views go with the grid's bitmap (never drawn after it is freed) */
static void cc_unview(void)
{
    for (int i = 0; i < CC_NV; i++) {
        tk_layer *v = &ccview[i];
        if (v->half.px && v->half.px != cc_vhalf[i]) tk_layer_dirty(v);
        v->bmp = (tk_bmp){ 0, 0, 0, 0 };
        v->half = (tk_bmp){ 0, 0, 0, 0 };
        v->hspan = 0;
        cc_vhalf[i] = 0;
        v->gen++;
    }
}
static void build_cc_grid(void)
{
    cc_unview();
    relayer(&ccgrid);
    dc d = mk(&ccgrid, "cc-grid", CC_COL(0), CC_ROW(0), 4 * CC_U + 3 * CC_GAP, 4 * CC_U + 3 * CC_GAP, 44, 1);   /* P3q (P3j-11): the tiles' shapes recorded, mask freed below */
    /* the connectivity 2x2: Airplane, Cellular, Wi-Fi and the mini cluster (Bluetooth, Nearby, Hotspot, VPN) */
    float X = CC_COL(0), Y = CC_ROW(0);
    cc_tile(&d, X, Y, CC_W2, CC_W2, 30, 0);
    static const char *CI[3] = { "cc-air", "cc-cell", "cc-wifi" };
    static const float CW[3][2] = { { 20, 20 }, { 20, 18 }, { 22, 16 } };
    int ons[3] = { CCS.air, cell_running(), CCS.wifi };
    static const uint32_t ONC[3] = { 0xff9f0a, 0x30d158, 0x0091ff };   /* IOS-REFERENCE 1: orange, green, blue */
    for (int i = 0; i < 3; i++) {
        float cx = X + CN_PAD + (i & 1 ? CN_D + 14 : 0), cy = Y + CN_PAD + (i >> 1 ? CN_D + 14 : 0);
        d_rrect(&d, cx, cy, CN_D, CN_D, CN_D / 2, ons[i] ? tk_hex(ONC[i], 1) : W(.2f));
        d_icon(&d, CI[i], cx + (CN_D - CW[i][0]) / 2, cy + (CN_D - CW[i][1]) / 2, 1);
    }
    {
        float bx = X + CN_PAD + CN_D + 14, by = Y + CN_PAD + CN_D + 14;
        for (int k = 0; k < 4; k++) {
            float mx = bx + .35f + (k & 1) * (CN_MD + 5.5f), my = by + (k >> 1) * (CN_MD + 6.2f);
            d_rrect(&d, mx, my, CN_MD, CN_MD, CN_MD / 2, k == 0 && CCS.bt ? C_ACCENT : W(.2f));
            if (k == 0) d_icon_s(&d, "cc-bt", mx + (CN_MD - 14 * .6f) / 2, my + (CN_MD - 20 * .6f) / 2, .6f);
            else cc_draw_link(&d, k - 1, mx + CN_MD / 2, my + CN_MD / 2, .85f, W(1));
        }
    }
    /* the sliders' glass (their level and glyph are layers above) */
    cc_tile(&d, CC_COL(2), CC_ROW(0), CC_U, CC_W2, 34, 0);
    cc_tile(&d, CC_COL(3), CC_ROW(0), CC_U, CC_W2, 34, 0);
    /* row 3: Flashlight, Rotation, Emergency, Saving; row 4: Timer, Calculator, Mirror, Camera */
    /* P3i: the torch's tile baked off; its lit look is a small layer over it, so the tap shows it at once (the
     * whole grid's rebuild took ~300 ms at the tap) */
    cc_tile(&d, CCX[0], CC_R3, CC_U, CC_U, 34, 0); cc_glyph(&d, "cc-light", CCX[0], CC_R3, 16, 24);
    {
        relayer(&cclit);
        dc e = mk(&cclit, "cc-lit", CCX[0], CC_R3, CC_U, CC_U, 1, 0);
        d_rrect(&e, CCX[0], CC_R3, CC_U, CC_U, 34, W(1));
        cc_glyph(&e, "cc-light-on", CCX[0], CC_R3, 16, 24);
        tk_layer_prepare(&cclit);
        cclit.hidden = !Q[0].lit;
    }
    cc_tile(&d, CCX[1], CC_R3, CC_U, CC_U, 34, CCS.rot); cc_glyph(&d, CCS.rot ? "cc-rot-on" : "cc-rot", CCX[1], CC_R3, 22, 22);
    cc_tile(&d, CCX[2], CC_R3, CC_U, CC_U, 34, 0);
    cc_glyph(&d, "cc-flame", CCX[2], CC_R3, 20, 24);
    {
        /* B6: Emergency's red a small layer over its tile, as the torch's: the switch shows it (the grid's rebuild
         * was most of a frame); B4's module 5 holds it */
        relayer(&ccem);
        dc e = mk(&ccem, "cc-em", CCX[2], CC_R3, CC_U, CC_U, 1, 0);
        d_rrect(&e, CCX[2], CC_R3, CC_U, CC_U, 34, tk_hex(0xff5a4e, 1));
        cc_glyph(&e, "cc-flame", CCX[2], CC_R3, 20, 24);
        tk_layer_prepare(&ccem);
        ccem.hidden = !EMG;
    }
    cc_tile(&d, CCX[3], CC_R3, CC_U, CC_U, 34, CCS.save); cc_glyph(&d, CCS.save ? "cc-save-on" : "cc-save", CCX[3], CC_R3, 24, 14);
    cc_tile(&d, CCX[0], CC_R4, CC_U, CC_U, 34, 0); cc_glyph(&d, "cc-timer", CCX[0], CC_R4, 22, 22);
    /* P3i: white glyphs like the other buttons (the app icons in colour looked out of place); P3p: baked ones */
    cc_tile(&d, CCX[1], CC_R4, CC_U, CC_U, 34, 0); cc_glyph(&d, "cc-calc", CCX[1], CC_R4, 24, 24);
    cc_tile(&d, CCX[2], CC_R4, CC_U, CC_U, 34, 0); cc_glyph(&d, "cc-wifi2", CCX[2], CC_R4, 22, 22);
    cc_tile(&d, CCX[3], CC_R4, CC_U, CC_U, 34, 0); cc_glyph(&d, "cc-camera", CCX[3], CC_R4, 24, 24);
    tk_a8_free(&ccgrid.glass);
    trim(&ccgrid);
    tk_layer_prepare(&ccgrid);
}
static void cc_levels(void)
{
    for (int i = 0; i < 2; i++) {
        float x = CC_COL(2 + i), v = CCS.lvl[i];
        cclvl[i].clip_r = (tk_rect){ PT(x), PT(CC_ROW(0) + CC_W2 * (1 - v)), PT(CC_U), PT(CC_W2 * v) };
    }
}
static void build_cc(void)
{
    /* the scrim from home as it is now (half resolution, drawn 2x), then the grid over it */
    tk_comp_frame(C, 0, 0);
    relayer(&ccscrim);
    tk_layer_init(&ccscrim, "cc-scrim");
    if (tk_bmp_new(&ccscrim.bmp, TK_W / 2, TK_H / 2)) {
        tk_bmp q;
        tk_bmp_new(&q, TK_W / 4, TK_H / 4);
        tk_bmp_down2(&ccscrim.bmp, shown_frame());
        tk_bmp_down2(&q, &ccscrim.bmp);
        /* P3p (P3j-20): the blur and the dim grow with the pull: an 8 pt level (dim .12, quarter resolution, 1 MB)
         * comes in first, the 26 pt one (neutral dim .25, not a blue tint) over it */
        relayer(&ccblur1);
        tk_layer_init(&ccblur1, "cc-blur1");
        if (tk_bmp_new(&ccblur1.bmp, q.w, q.h)) {
            tk_bmp_copy(&ccblur1.bmp, 0, 0, &q, 0, 0, q.w, q.h);
            tk_bmp_blur(&ccblur1.bmp, PT(8) / 4);
            tk_bmp_satbright(&ccblur1.bmp, 1.15f, 1);
            tk_bmp_tint(&ccblur1.bmp, TK_RGBA(0, 0, 0, .12f));
            ccblur1.sx = ccblur1.sy = 4;
            ccblur1.opaque = 1;
        }
        tk_bmp_blur(&q, PT(26) / 4);
        tk_bmp_satbright(&q, 1.3f, 1);
        tk_bmp_tint(&q, TK_RGBA(0, 0, 0, .25f));
        if (C && C->gpu) {
            /* P3z: the GPU row keeps the 26 pt blur at quarter resolution, drawn 4x (its filtering is smooth on a blur
             * this wide): 1 MB to flush to the GPU at the first open instead of the 4.2 MB half copy (cp3: compose
             * 112 ms against the GPU's 9 on the CC's first open). The half copy is the CPU row's glass source
             * (tk_glass_from_half in cc_tile), which the GPU row doesn't use (its tiles are the lens) */
            tk_bmp_free(&ccscrim.bmp);
            ccscrim.bmp = q;
            ccscrim.sx = ccscrim.sy = 4;
        } else {
            tk_bmp_up(&ccscrim.bmp, &q, 0, 0, 2);
            tk_bmp_free(&q);
            ccscrim.sx = ccscrim.sy = 2;
        }
        ccscrim.opaque = 1;
    }
    if (!ccsb.bmp.px) {
        dc d = mk(&ccsb, "cc-status", 0, 0, 390, 48, 2, 0);
        status_bar(&d, carrier);
        trim(&ccsb);
    }
    {
        /* IOS-REFERENCE 1: the + and Power circles (28 pt, centre y 30, on the grid's margins; their glass from this
         * scrim) and the page icons */
        relayer(&cctop);
        dc d = mk(&cctop, "cc-chrome", 37.25f, 16, 315.5f, 28, 14, 1);
        cc_tile(&d, 37.25f, 16, 28, 28, 14, 0);
        d_seg(&d, 51.25f, 24.5f, 51.25f, 35.5f, 1.8f, W(1));
        d_seg(&d, 45.75f, 30, 56.75f, 30, 1.8f, W(1));
        cc_tile(&d, 324.75f, 16, 28, 28, 14, 0);
        cc_arc(&d, 338.75f, 30.5f, 5.6f, 1.7f, .55f, 5.18f, W(1));
        d_seg(&d, 338.75f, 23.5f, 338.75f, 29.5f, 1.7f, W(1));
        tk_a8_free(&cctop.glass);
        trim(&cctop);
    }
    if (!ccpg.bmp.px) {
        dc e = mk(&ccpg, "cc-pages", 369, 341, 14, 132, 2, 0);
        d_rrect(&e, 369.6f, 342.8f, 6.8f, 6.8f, 3.4f, W(1));          /* a heart: this page */
        d_rrect(&e, 375.6f, 342.8f, 6.8f, 6.8f, 3.4f, W(1));
        for (int k = 0; k <= 6; k++) d_seg(&e, 370.4f + 1.87f * k, 347.4f, 376, 353.6f, 1.6f, W(1));
        d_rrect(&e, 371.5f, 404.5f, 5, 5, 2.5f, W(.45f));          /* a note */
        d_seg(&e, 376, 395, 376, 406.5f, 1.4f, W(.45f));
        d_seg(&e, 376, 395, 380.5f, 397, 1.4f, W(.45f));
        cc_draw_link(&e, 0, 376, 461, .9f, W(.45f));              /* the links */
        trim(&ccpg);
    }
    if (!cclvl[0].bmp.px) {
        for (int i = 0; i < 2; i++) {
            float x = CC_COL(2 + i), y = CC_ROW(0);
            dc l = mk(&cclvl[i], "cc-level", x, y, CC_U, CC_W2, 0, 0);
            d_rrect(&l, x, y, CC_U, CC_W2, 34, W(1));
            cclvl[i].clip = 1;
            /* IOS-REFERENCE 1: the glyph's centre 22 pt above the slider's bottom */
            dc g = mk(&ccic[i], "cc-glyph", x + 23, y + CC_W2 - 33, 22, 22, 1, 0);
            d_icon(&g, i ? "cc-vol" : "cc-sun", x + 23, y + CC_W2 - 33, 1);
        }
    }
    build_cc_grid();
    tk_layer_init(&ccg, "cc-grid-group");
    ccg.ox = PT(CC_COL(0) + .85f * (4 * CC_U + 3 * CC_GAP));
    ccg.oy = PT(CC_ROW(0));
    cc_link();
    cc_levels();
    CCS.built = 1;
}
static void cc_x_reset(void);
/* an open app needs the room (its page is a whole screen): the Control Centre's snapshot and grid go, home's idle
 * time builds them again */
static void free_cc(void)
{
    if (!CCS.built || CCS.on) return;
    cc_x_reset();
    cc_unview();
    relayer(&ccscrim);
    relayer(&ccblur1);
    relayer(&ccgrid);
    relayer(&ccem);
    CCS.built = 0;
}
/* P3s: on the lock (it opens only from home) nothing of it is kept; home's idle time builds it again */
static void free_cc_all(void)
{
    if (CCS.on) return;
    free_cc();
    if (!ccsb.bmp.px && !cclvl[0].bmp.px && !cclit.bmp.px) return;
    relayer(&ccsb); relayer(&cclit); relayer(&cctop); relayer(&ccpg);
    for (int i = 0; i < 2; i++) { relayer(&cclvl[i]); relayer(&ccic[i]); }
}
/* B4: each module's place: its row's share of the pull (top row first), scale .9 -> 1, 30 pt higher -> 0, its fade;
 * the expanded one hands over to its panel, the others fade back under it; a dragged slider grows to 1.05 */
static void cc_mods(float p)
{
    float e = CX.m >= 0 ? tk_clampf(CX.e, 0, 1) : 0;
    for (int i = 0; i < CC_NM; i++) {
        tk_layer *m = &ccmod[i];
        float r[5];
        cc_mod_rect(i, r);
        int row = i < 3 || i == 11 ? 0 : i < 7 ? 1 : 2;
        float dl = .07f * row, q = tk_clampf((p - dl) / (1 - dl), 0, 1);
        float s = .9f + .1f * q, a = tk_clampf(q * 1.6f, 0, 1);
        s *= cc_pscale[i];
        if (e > 0) {
            if (i == CX.m) a *= 1 - tk_minf(1, e * 3);
            else { a *= 1 - .97f * e; s *= 1 - .04f * e; }
        }
        m->ox = PT(r[0] + r[2] / 2);
        m->oy = PT(r[1] + r[3] / 2);
        m->sx = m->sy = s;
        m->y = PT((1 - q) * -30) / s;
        m->alpha = a;
        m->hidden = a <= .004f;
    }
}
/* the expanded panel's place: a rect between the module's and the panel's, its fill, its words */
static void cc_x_place(void)
{
    if (CX.m < 0) { ccx.hidden = 1; return; }
    float r0[5];
    cc_mod_rect(CX.m, r0);
    const float *B = CXR[CX.m];
    float e = tk_clampf(CX.e, 0, 1.08f), u = tk_minf(e, 1);
    float x = r0[0] + (B[0] - r0[0]) * e, y = r0[1] + (B[1] - r0[1]) * e;
    float w = r0[2] + (B[2] - r0[2]) * e, h = r0[3] + (B[3] - r0[3]) * e, rad = r0[4] + (B[4] - r0[4]) * u;
    ccxdim.solid = tk_premul(TK_RGBA(0, 0, 0, .3f));
    ccxdim.solid_w = TK_W; ccxdim.solid_h = TK_H;
    ccxdim.alpha = u;
    ccxbg.solid = tk_premul(W(CX.m ? .2f : .14f));
    ccxbg.x = PT(x); ccxbg.y = PT(y);
    ccxbg.solid_w = PT(w); ccxbg.solid_h = PT(h);
    ccxbg.clip = 1;
    ccxbg.clip_r = (tk_rect){ PT(x), PT(y), PT(w), PT(h) };
    ccxbg.clip_radius = PT(rad);
    ccxbg.alpha = tk_minf(1, u * 4);
    ccxbg.hidden = 0;
    if (CX.m) {
        float v = CCS.lvl[CX.m - 1];
        ccxlvl.solid = tk_premul(W(1));
        ccxlvl.x = 0; ccxlvl.y = PT(h * (1 - v));           /* (in the panel's space: its x, y move it) */
        ccxlvl.solid_w = PT(w); ccxlvl.solid_h = PT(h * v) + 1;
        ccxlvl.hidden = v <= 0;
        if (ccxc.bmp.px) {         /* the glyph rides the panel's bottom centre */
            ccxc.x = ccxc.bx + PT(x + w / 2 - (B[0] + B[2] / 2));
            ccxc.y = ccxc.by + PT(y + h - (B[1] + B[3]));
            ccxc.alpha = u;
            ccxc.hidden = 0;
        }
    } else {
        ccxlvl.hidden = 1;
        if (ccxc.bmp.px && CX.todo < 6) ccxc.hidden = 1;
        else if (ccxc.bmp.px) {    /* the links scale with the panel; they come in over its last 60 % */
            float s = w / B[2];
            ccxc.sx = ccxc.sy = s;
            ccxc.ox = PT(B[0]); ccxc.oy = PT(B[1]);
            ccxc.x = ccxc.bx + PT(x - B[0]) / s;
            ccxc.y = ccxc.by + PT(y - B[1]) / s;
            ccxc.alpha = tk_clampf((u - .4f) / .6f, 0, 1);
            ccxc.hidden = ccxc.alpha <= .004f;
        }
    }
    ccx.hidden = 0;
}
static void set_cc(float p)
{
    CCS.p = p;
    /* P3p (P3j-20): the light blur first, the full one from .35; only the status bar's words cross over (.3 .. .6),
     * so two bars never sit on each other; the grid grows to 1.04 at most on an over-pull */
    ccblur1.alpha = tk_clampf(p * 2.2f, 0, 1);
    ccscrim.alpha = tk_clampf((p - .35f) / .65f, 0, 1);
    ccg.alpha = 1;
    ccg.sx = ccg.sy = tk_minf(1.04f, 1 + tk_maxf(0, p - 1) * .14f);
    ccg.y = 0;
    float k = tk_clampf((p - .3f) / .3f, 0, 1);
    ccsb.alpha = k;
    ccsb.y = ccsb.by + PT(62 * tk_clampf(p, 0, 1));    /* IOS-REFERENCE 1: the panel's status row at y 92 */
    hparts[0].alpha = 1 - k;
    cc_mods(p);
    cc_x_place();
    ccl.hidden = !CCS.on;
}
static int open_app_key(const char *k);   /* Track A, 1.0 (after the app table) */
static void cc_close(void)
{
    CCS.closing = 1;
    tk_spring_start(&sp_cc, CCS.p, 0, 0, TK_SPR_FAST);
}
/* ---- B4: the expanded module ---- */
static int cc_x_sig(void) { return CCS.air | cell_running() << 1 | data_on() << 2 | CCS.wifi << 3 | SB_WIFI << 4 | CCS.bt << 5; }
static const char *const CXN[6] = { "Airplane Mode", "Nearby Sharing", "Mobile Data", "Wi-Fi", "Bluetooth", "Hotspot" };
static void cc_x_centre(int k, float *cx, float *cy)
{
    const float *B = CXR[0];
    *cx = B[0] + B[2] * (1 + 2 * (k % 3)) / 6;
    *cy = B[1] + 22 + 32 + 120 * (k / 3);
}
/* the connectivity panel's six links (circle, name, state): one a frame from the threshold (they show from 40 % of
 * the morph on, ~6 frames in), and again on a change; ~2 ms each on the Mac */
static void cc_x_links(void)
{
    const float *B = CXR[0];
    relayer(&ccxc);
    mk(&ccxc, "cc-links", B[0], B[1], B[2], B[3], 1, 0);
    ccxc.hidden = 1;
    CX.todo = 0;
    CX.sig = cc_x_sig();
}
static void cc_x_link_step(void)
{
    const float *B = CXR[0];
    if (CX.todo >= 6 || !ccxc.bmp.px) return;
    dc d = { &ccxc.bmp, 0, ccxc.bx, ccxc.by };
    int ons[6] = { CCS.air, 0, cell_running(), CCS.wifi, CCS.bt, 0 };
    static const uint32_t ONC[6] = { 0xff9f0a, 0x0091ff, 0x30d158, 0x0091ff, 0x0091ff, 0x30d158 };
    {
        int k = CX.todo++;
        float cx, cy;
        cc_x_centre(k, &cx, &cy);
        d_rrect(&d, cx - 32, cy - 32, 64, 64, 32, ons[k] ? tk_hex(ONC[k], 1) : W(.2f));
        if (k == 0) d_icon(&d, "cc-air", cx - 10, cy - 10, 1);
        else if (k == 2) d_icon(&d, "cc-cell", cx - 10, cy - 9, 1);
        else if (k == 3) d_icon(&d, "cc-wifi", cx - 11, cy - 8, 1);
        else if (k == 4) d_icon(&d, "cc-bt", cx - 7, cy - 10, 1);
        else cc_draw_link(&d, k == 1 ? 0 : 1, cx, cy, 1.9f, W(1));
        const char *st = k == 2 ? (data_on() ? "On" : cell_running() ? "Data off" : "Off") :
                         k == 3 ? (!CCS.wifi ? "Off" : SB_WIFI ? "Connected" : "Not connected") :
                         k == 0 ? "Off" : "Not available";
        d_text_box(&d, TK_IT600, 13, cx, bl(TK_IT600, 13, cy + 40), B[2] / 3 - 6, 1, CXN[k], 0, 0, INK, 0);
        d_text_box(&d, TK_IT400, 12, cx, bl(TK_IT400, 12, cy + 57), B[2] / 3 - 6, 1, st, 0, 0, C_LABEL2, 0);
    }
    if (CX.todo == 6) { trim(&ccxc); tk_layer_prepare(&ccxc); }
}
static void cc_x_open(int m)
{
    if (CX.m >= 0 && !CX.closing) return;
    CX.m = m;
    CX.closing = 0;
    tk_layer_init(&ccxdim, "cc-x-dim");
    tk_layer_init(&ccxbg, "cc-x-panel");
    tk_layer_init(&ccxlvl, "cc-x-level");
    uint64_t t0 = tk_us();
    if (m == 0) { cc_x_links(); cc_x_link_step(); }
    else {
        /* the big slider's glyph (a 30 pt drawing) */
        const float *B = CXR[m];
        relayer(&ccxc);
        float gx = B[0] + B[2] / 2 - 15, gy = B[1] + B[3] - 30 - 30;
        dc d = mk(&ccxc, "cc-x-glyph", gx, gy, 30, 30, 1, 0);
        d_icon_s(&d, m == 1 ? "cc-sun" : "cc-vol", gx, gy, 30.0f / 22);
        tk_layer_prepare(&ccxc);
        CX.todo = 6;
    }
    lap_us("cc: expanded drawn", tk_us() - t0);
    cc_link();
    tk_spring_start(&CX.sp, CX.e, 1, CX.sp.live ? CX.sp.v : 0, TK_SPR_MORPH);
    sh_haptic_k(SH_HAP_MEDIUM);                 /* IOS-REFERENCE 1 / 17: the long press commits */
    set_cc(CCS.p);
}
static void cc_x_close(void)
{
    if (CX.m < 0 || CX.closing) return;
    CX.closing = 1;
    tk_spring_start(&CX.sp, CX.e, 0, CX.sp.v, TK_SPR_MORPH);
}
static void cc_x_reset(void)
{
    if (CX.m < 0 && !ccxc.bmp.px) return;
    CX.m = -1; CX.closing = 0; CX.e = 0; CX.sp.live = 0; CX.todo = 6;
    relayer(&ccxc);
    ccxc.hidden = 1;
    ccx.hidden = 1;
    if (CCS.built) set_cc(CCS.p);
}
static int cc_lk = -1;             /* the lock state and the lock's version the Control Centre's snapshot was taken in */
static int cc_lv;
static int cc_moving(void)
{
    if (CX.sp.live || (CX.m == 0 && CX.todo < 6)) return 1;
    for (int i = 0; i < CC_NM; i++) if (cc_ps[i].live) return 1;
    return 0;
}
static int cc_x_tick(float dt)
{
    int mv = 0;
    for (int i = 0; i < CC_NM; i++)
        if (cc_ps[i].live) { tk_spring_step(&cc_ps[i], dt); cc_pscale[i] = cc_ps[i].x; mv = 1; }
    if (CX.sp.live) {
        int live = tk_spring_step(&CX.sp, dt);
        CX.e = CX.sp.x;
        if (!live && CX.closing) cc_x_reset();
        mv = 1;
    }
    if (CX.m == 0 && CX.todo < 6) { cc_x_link_step(); mv = 1; }
    if (CX.m == 0 && !CX.closing && CX.todo >= 6 && cc_x_sig() != CX.sig) { cc_x_links(); cc_link(); mv = 1; }
    if (mv && CCS.built) set_cc(CCS.p);
    return mv;
}
/* which of the panel's links is under (x, y), -1 none */
static int cc_x_hit(float x, float y)
{
    for (int k = 0; k < 6; k++) {
        float cx, cy;
        cc_x_centre(k, &cx, &cy);
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < 40 * 40) return k;
    }
    return -1;
}
static int cc_x_inside(float x, float y)
{
    const float *B = CXR[CX.m < 0 ? 0 : CX.m];
    return x >= B[0] && x < B[0] + B[2] && y >= B[1] && y < B[1] + B[3];
}
/* a slider's level (the panel's brightness follows); a selection tick each 1/16, a rigid one at either end */
static void cc_set_level(int i, float v)
{
    v = tk_clampf(v, 0, 1);
    int edge = v <= 0 ? 1 : v >= 1 ? 2 : 0, st = (int)(v * 16);
    if (edge && edge != cc_edge[i]) sh_haptic_k(SH_HAP_RIGID);
    else if (!edge && st != cc_step[i]) sh_haptic_k(SH_HAP_SELECT);
    cc_edge[i] = edge;
    cc_step[i] = st;
    CCS.lvl[i] = v;
    cc_levels();
    if (i == 0) {                        /* P2r: the brightness slider sets the panel's level (1..4) */
        int lv = 1 + (int)(v * 3.999f);
        if (lv != cc_bright) { cc_bright = lv; sh_brightness(lv); }
    }
    if (CX.m == i + 1) cc_x_place();
}
/* the module under (x, y) (the grid's, not the chrome), -1 none */
static int cc_mod_at(float x, float y)
{
    for (int i = 0; i < CC_NV; i++) {
        float r[5];
        cc_mod_rect(i, r);
        if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) return i;
    }
    return -1;
}
static void cc_press(int i, int down)
{
    if (i < 0 || i >= CC_NM) return;
    float r[5];
    cc_mod_rect(i, r);
    float to = !down ? 1 : (i == 1 || i == 2) ? 1.05f : 1 + tk_minf(.12f, 10 / tk_minf(r[2], r[3]));
    if (down && (i == 1 || i == 2)) { float v = CCS.lvl[i - 1]; cc_edge[i - 1] = v <= 0 ? 1 : v >= 1 ? 2 : 0; cc_step[i - 1] = (int)(v * 16); }
    tk_spring_start(&cc_ps[i], cc_pscale[i], to, cc_ps[i].live ? cc_ps[i].v : 0, TK_SPR_PRESS);
    cc_pressed = down ? i : -1;
}
static void cc_release(void) { if (cc_pressed >= 0) cc_press(cc_pressed, 0); }
/* a tap on the open Control Centre: a tile, a circle, or outside (close); 1 if it was on the grid */
static void toast(const char *msg);
static void cc_wifi_tap(void)
{
    int on = CCS.wifi;                   /* wifi-sbos-next: Wi-Fi is real */
    if (sh_wifi_power(!on) < 0) toast(on ? "Wi-Fi can't stop now" : "Wi-Fi can't start now");
    else {
        sh_haptic_k(SH_HAP_LIGHT);
        toast(on ? "Wi-Fi off" : "Wi-Fi on: Settings > Wi-Fi to join");
        sh_pref_set_int("cc.wifi", !on);     /* Track A: the user's switch, kept (sh_restore) */
    }
}
static const char *const CC_NOT[6] = { "Airplane mode isn't in this version yet", "Nearby sharing isn't available yet", 0, 0,
                                       "Bluetooth isn't available yet", "Personal Hotspot isn't available yet" };
static int cc_tap(float x, float y)
{
    float X = CC_COL(0), Y = CC_ROW(0);
    if (x >= 316 && x < 362 && y >= 8 && y < 52) { sh_power_held(); return 1; }   /* P3x: the Power circle: the power sheet */
    if (x >= X && x < X + CC_W2 && y >= Y && y < Y + CC_W2) {
        int i = (x >= X + CC_W2 / 2) + 2 * (y >= Y + CC_W2 / 2);
        if (i == 1) return 1;                /* the modem: a hold (G_CCCELL) */
        if (i == 2) { cc_wifi_tap(); return 1; }
        if (i == 0) { toast(CC_NOT[0]); return 1; }
        /* the mini cluster: Bluetooth, Nearby, Hotspot, VPN (P2y: no switch pretends) */
        int k = (x >= X + CN_PAD + CN_D + 14 + CN_MD + 2.75f) + 2 * (y >= Y + CN_PAD + CN_D + 14 + CN_MD + 3.1f);
        toast(k == 0 ? CC_NOT[4] : k == 1 ? CC_NOT[1] : k == 2 ? CC_NOT[5] : "VPN isn't available yet");
        return 1;
    } else if (y >= CC_R3 && y < CC_R3 + CC_U) {
        int c = x < CCX[1] - CC_GAP / 2 ? 0 : x < CCX[2] - CC_GAP / 2 ? 1 : x < CCX[3] - CC_GAP / 2 ? 2 : 3;
        if (c == 0) {
            Q[0].lit = !Q[0].lit; Q[0].facelit.hidden = Q[0].iconlit.hidden = !Q[0].lit; Q[0].face.hidden = Q[0].icon.hidden = Q[0].lit; sh_torch(Q[0].lit); lock_ver++;
            cclit.hidden = !Q[0].lit || !cclit.bmp.px;
            sh_haptic_k(SH_HAP_LIGHT);       /* B4: a toggle, felt */
            return 1;
        }
        else if (c == 1) { toast("The screen doesn't rotate yet"); return 1; }
        else if (c == 3) {                   /* Track A, 1.0: Low Power Mode (kernel: the conservative clocks, kept) */
            int on = !sh_low_power_on();
            sh_low_power(on);
            sh_haptic_k(SH_HAP_LIGHT);
            toast(on ? "Low Power Mode On" : "Low Power Mode Off");
            return 1;
        }
        else return 1;                       /* Emergency: a hold (G_CCEM) */
    } else if (y >= CC_R4 && y < CC_R4 + CC_U && x >= CCX[0] && x < CCX[3] + CC_U) {
        /* Track A, 1.0: Timer, Calculator, Mirror, Camera open their apps (Timer: Clock; Mirror: the front camera);
         * on the lock screen they ask for the unlock first (no app over the lock without it) */
        static const char *const K4[4] = { "clock", "calc", "camera", "camera" };
        int c = x < CCX[1] - CC_GAP / 2 ? 0 : x < CCX[2] - CC_GAP / 2 ? 1 : x < CCX[3] - CC_GAP / 2 ? 2 : 3;
        if (lockY > -H_PT * .5f) { toast("Unlock to open it"); return 1; }
        sh_haptic_k(SH_HAP_LIGHT);
        cc_close();
        if (!open_app_key(K4[c])) toast("Not in this version yet");
        return 1;
    } else
        return 0;
}
/* B4: a tap on the expanded panel's links (the Cellular one is a hold: G_CCCELL) */
static void cc_x_tap(int k)
{
    if (k == 3) cc_wifi_tap();
    else if (k >= 0 && CC_NOT[k]) toast(CC_NOT[k]);
}


/* ---- an app opened from home: the window grows from its icon (the concept's openApp / setAppRect) -----------
 * R goes from the icon's rect (radius 15) to the panel (radius 52) on the open spring (380 / 34); the window is the
 * panel's page scaled uniformly by R.w / 390 and clipped to R; its background mixes from the app's tint to #0a0f1f
 * (min(1, 2 p)), its content fades in (p x 1.6 - .2), home behind scales to 1.05 and fades to .6. A swipe up from
 * the bottom bar follows the finger (scale >= .35, radius 52 -> 30) and closes it into the icon (360 / 30) past
 * 60 pt or faster than .35 pt/ms, else springs back. P2: every app shows the concept's generic screen. */
typedef struct { float x, y, w, h, r; } arect;
typedef struct { const char *l, *r; } arow;
typedef struct { const char *cap; int n; arow rows[8]; } asec;
typedef struct { const char *k, *name; uint32_t tint; int ns; asec sec[4]; } appdef;
static const appdef APPS[] = {
    /* P2y: what each app can honestly show today; "@..." rows take a live value (live_text) */
    { "calendar", "Calendar", 0x1d2440, 1, { { "Today", 2, { { "@date", "" }, { "No events", "" } } } } },
    { "weather", "Weather", 0x1d2440, 1, { { "", 2, { { "Weather needs the internet", "" }, { "Not available yet", "" } } } } },
    { "clock", "Clock", 0x1d2440, 1, { { "Now", 2, { { "@time", "" }, { "@date", "" } } } } },
    { "camera", "Camera", 0x1d2440, 1, { { "", 1, { { "", "" } } } } },
    { "photos", "Photos", 0x1d2440, 1, { { "", 1, { { "", "" } } } } },       /* photos, music, files: shell_media.inc */
    { "music", "Music", 0x1d2440, 1, { { "", 1, { { "", "" } } } } },
    { "notes", "Notes", 0x1d2440, 1, { { "", 1, { { "No notes yet", "" } } } } },
    { "calc", "Calculator", 0x1d2440, 1, { { "", 1, { { "Not in this version yet", "" } } } } },
    { "contacts", "Contacts", 0x1d2440, 1, { { "", 1, { { "", "" } } } } },
    { "files", "Files", 0x1d2440, 1, { { "", 1, { { "", "" } } } } },
    { "light", "Flashlight", 0x1d2440, 1, { { "", 2, { { "Flashlight", "@torch" }, { "Hold the torch button on the lock screen", "" } } } } },
    { "battery", "Battery", 0x1d2440, 1, { { "Now", 3, { { "Charge", "@pct" }, { "Charging", "@chg" }, { "Battery temperature", "@temp" } } } } },
    { "terminal", "Terminal", 0x1d2440, 1, { { "", 1, { { "Linux apps run in Starbite OS", "" } } } } },
    { "browser", "Browser", 0x2f7bf6, 1, { { "", 1, { { "The browser needs the internet", "" } } } } },
    { "medical", "Medical Card", 0xd62f3f, 1, { { "", 1, { { "", "" } } } } },     /* survival-next */
    { "sos", "SOS", 0xb3150c, 1, { { "", 1, { { "", "" } } } } },
    { "heat", "Survival Heat", 0x1d2440, 1, { { "", 1, { { "Survival Heat runs in Starbite OS", "" } } } } },
    { "power", "Last 2 %", 0xe09f1f, 1, { { "", 1, { { "", "" } } } } },      /* survival-next: shell_survival.inc */
    { "usb", "USB Power", 0xe0461f, 1, { { "", 1, { { "", "" } } } } },
    { "sun", "Sun", 0xff8a00, 1, { { "", 1, { { "", "" } } } } },
    { "guide", "Survival Guide", 0x1f8a4c, 1, { { "", 1, { { "", "" } } } } },
    { "compass", "Compass / Level", 0x55555c, 1, { { "", 1, { { "", "" } } } } },
    { "phone", "Phone", 0x16b34a, 1, { { "", 1, { { "", "" } } } } },
    { "messages", "Messages", 0x16b34a, 1, { { "", 1, { { "", "" } } } } },
    { "search", "Search", 0x1d2440, 1, { { "", 1, { { "", "" } } } } },
    { "linux", "Linux", 0x1d2440, 1, { { "", 1, { { "", "" } } } } },      /* L3: the open Linux app (shell_linux.inc) */
    { "store", "App Store", 0x0a6fe8, 1, { { "", 1, { { "", "" } } } } },
    { "maps", "Maps", 0xe3decd, 1, { { "", 1, { { "", "" } } } } },           /* helper D: shell_nc.inc's page_maps */
    { "settings", "Settings", 0x55555a, 1, { { "", 1, { { "", "" } } } } },     /* helper B2: shell_settings.inc */
};
#define NAPPS ((int)(sizeof APPS / sizeof APPS[0]))
/* Track A lifecycle (sbos-core-api.md 6): the apps' optional callbacks, beside APPS by key (an app without a row has
 * none): on_show the page shown (cold: a first open, or after the switcher threw it away), on_hide it left the screen
 * (closed, another app, the lock), on_save before a restart or a switch-off (sh_save_all: keep data with sh_blob_put) */
typedef struct { const char *k; void (*on_show)(int cold); void (*on_hide)(void); void (*on_save)(void); } apphook;
static void cam_show(int cold);                   /* shell_camera.inc: the camera on with the page, off without it */
static void cam_hide(void);
static const apphook APPHOOK[] = {
    { "camera", cam_show, cam_hide, 0 },
    { 0, 0, 0, 0 },                                   /* (the pages add theirs here) */
};
static const apphook *ap_hook(int a)
{
    for (int i = 0; APPHOOK[i].k; i++) if (!tk_strcmp(APPHOOK[i].k, APPS[a].k)) return &APPHOOK[i];
    return 0;
}
static tk_layer appg, appbg, appc, appst[4];      /* the page: a group of four strips (no 17 MB block) */
#define NST 4
/* strip i of the page: exact panel rows, so the strips meet without overlap */
static dc strip_dc(int i, const char *name)
{
    tk_layer *l = &appst[i];
    relayer(l);
    tk_layer_init(l, name);
    int y0 = TK_H * i / NST, y1 = TK_H * (i + 1) / NST;
    tk_bmp_new(&l->bmp, TK_W, y1 - y0);
    l->x = l->bx = 0;
    l->y = l->by = (float)y0;
    dc d = { &l->bmp, 0, 0, (float)y0 };
    return d;
}
static struct { int on, closing, following, a; arect icon, R, from, to; float travel; } AP;
static tk_spring sp_app;
static const arect FULLR = { 0, 0, 390, H_PT, 52 };
static arect lerpr(arect a, arect b, float t)
{
    return (arect){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t, a.r + (b.r - a.r) * t };
}
/* ---- the apps' own screens (P2l), after the concept's SCREENS: Phone (keypad, Recents, Contacts, Favourites),
 * Messages (the list, a thread), Contacts, Photos, Camera. Places from Chrome's own layout of them
 * (tools/concept-boxes.mjs): a text's baseline is its line's top plus its rounded ascent. The concept's sample
 * people and messages are its placeholders until the modem carries SMS. -------------------------------------- */
static float bl(int f, float size, float top) { return top + tk_floorf(tk_ascent(f, PT(size)) / TK_PT + .5f); }
static float lh(int f, float size)
{
    return tk_floorf(tk_ascent(f, PT(size)) / TK_PT + .5f) + tk_floorf(tk_descent(f, PT(size)) / TK_PT + .5f);
}
typedef struct { const char *n; uint32_t c; } person_t;
static const person_t PEOPLE[6] = { { "Aino Virtanen", 0x5e5ce6 }, { "Mikko Laine", 0xff9f0a }, { "Sara Niemi", 0x30d158 },
                                    { "Juho K.", 0x64d2ff }, { "Mum", 0xff375f }, { "Ville Ojala", 0xbf5af2 } };
static uint32_t pcol(const char *n)
{
    for (int i = 0; i < 6; i++) if (!tk_strcmp(PEOPLE[i].n, n)) return PEOPLE[i].c;
    return 0x8e8e93;
}
/* the avatar: a circle, the initials (each word's first letter, at most two) centred as a grid centres them */
static void d_av(dc *d, float x, float y, float s, const char *n, float fs)
{
    d_rrect(d, x, y, s, s, s / 2, tk_hex(pcol(n), 1));
    char in[3];
    int k = 0;
    in[k++] = n[0];
    for (const char *c = n; *c && k < 2; c++) if (*c == ' ' && c[1] && c[1] != ' ') in[k++] = c[1];
    in[k] = 0;
    float w = tw(TK_IT700, fs, in, 0, 0);
    d_text(d, TK_IT700, fs, x + (s - w) / 2, bl(TK_IT700, fs, y + (s - lh(TK_IT700, fs)) / 2), in, 0, 0, W(1), 0);
}
/* text-overflow: ellipsis: the longest start that fits with the ellipsis */
static void ellipsize(char *out, int cap, int f, float size, const char *s, float maxw)
{
    int n = 0;
    while (s[n] && n < cap - 1) { out[n] = s[n]; n++; }
    out[n] = 0;
    if (tw(f, size, out, 0, 0) <= maxw) return;
    while (n > 0) {
        n--;
        while (n > 0 && (out[n] & 0xc0) == 0x80) n--;          /* a whole UTF-8 character */
        out[n] = 0;
        char t[128];
        int k = 0;
        while (out[k] && k < 120) { t[k] = out[k]; k++; }
        t[k++] = (char)0xe2; t[k++] = (char)0x80; t[k++] = (char)0xa6; t[k] = 0;
        if (tw(f, size, t, 0, 0) <= maxw) { tk_memcpy(out, t, (size_t)k + 1); return; }
    }
}
/* P3p (P3j-14): a one-line label in maxw pt, as UILabel's default (one line, tail truncation): measured with its own
 * tracking and flags, cut after a whole UTF-8 character (trailing spaces dropped) with U+2026 when it is wider;
 * align 0: starts at x, 1: centred on x, 2: ends at x */
static void d_text_box(dc *d, int f, float size, float x, float base, float maxw, int align, const char *s, float ls, int fl, tk_col c, int shadow)
{
    char out[164];
    int n = 0;
    while (s[n] && n < 156) { out[n] = s[n]; n++; }
    while (n > 0 && s[n] && ((unsigned char)s[n] & 0xc0) == 0x80) n--;      /* never half a character */
    out[n] = 0;
    float w = tw(f, size, out, ls, fl);
    if (w > maxw || s[n]) {
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)out[n] & 0xc0) == 0x80) n--;
            int k = n;
            while (k > 0 && out[k - 1] == ' ') k--;
            out[k] = (char)0xe2; out[k + 1] = (char)0x80; out[k + 2] = (char)0xa6; out[k + 3] = 0;
            w = tw(f, size, out, ls, fl);
            if (w <= maxw) break;
        }
    }
    float x0 = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
    d_text(d, f, size, x0, base, out, ls, fl, c, shadow);
}
/* greedy word wrap into at most maxl (<= 4) lines of maxw; returns the line count, the widest line in *wmax. P3p
 * (P3j-14): a word wider than the line breaks by character, and text past the last line ends it with an ellipsis */
static int wrapn(char lines[4][96], int maxl, int f, float size, const char *s, float maxw, float *wmax)
{
    int nl = 0, i = 0;
    *wmax = 0;
    while (s[i] && nl < maxl) {
        int st = i, last = -1, k = i;
        char t[96];
        for (;;) {
            int e = k;
            while (s[e] && s[e] != ' ') e++;
            int len = e - st;
            if (len > 95) len = 95;
            tk_memcpy(t, s + st, (size_t)len);
            t[len] = 0;
            if (last >= 0 && tw(f, size, t, 0, 0) > maxw) break;
            if (last < 0 && tw(f, size, t, 0, 0) > maxw) {
                /* one word wider than the line: as much of it as fits (whole UTF-8 characters) */
                int m = len;
                while (m > 1) {
                    m--;
                    while (m > 1 && ((unsigned char)t[m] & 0xc0) == 0x80) m--;
                    t[m] = 0;
                    if (tw(f, size, t, 0, 0) <= maxw) break;
                }
                last = st + m;
                break;
            }
            last = e;
            if (!s[e]) break;
            k = e + 1;
        }
        if (nl == maxl - 1 && s[last]) {
            /* the last line: the rest of the text, cut at its tail */
            char e[96];
            ellipsize(e, sizeof e, f, size, s + st, maxw);
            tk_memcpy(lines[nl], e, tk_strlen(e) + 1);
            float w = tw(f, size, lines[nl], 0, 0);
            if (w > *wmax) *wmax = w;
            return nl + 1;
        }
        int len = last - st;
        if (len > 95) len = 95;
        tk_memcpy(lines[nl], s + st, (size_t)len);
        lines[nl][len] = 0;
        float w = tw(f, size, lines[nl], 0, 0);
        if (w > *wmax) *wmax = w;
        nl++;
        i = last;
        while (s[i] == ' ') i++;
    }
    return nl;
}
static int wrap(char lines[4][96], int f, float size, const char *s, float maxw, float *wmax) { return wrapn(lines, 4, f, size, s, maxw, wmax); }
/* Liquid Glass over the app's own ground (#0a0f1f) and what is drawn so far under it (Chrome's backdrop) */
__attribute__((unused)) static void d_lg_app(dc *d, float x, float y, float w, float h, float r, int dark)
{
    float bx = PT(x) - d->ox, by = PT(y) - d->oy;
    if (by + PT(h + 48) < 0 || by - PT(8) > d->b->h) return;          /* its shadow included */
    int X = (int)tk_floorf(bx), Y = (int)tk_floorf(by), Wd = (int)(bx + PT(w) + .999f) - X, Hd = (int)(by + PT(h) + .999f) - Y;
    tk_bmp t;
    if (!tk_bmp_new(&t, Wd, Hd)) return;
    tk_fill_rect_raw(&t, 0, 0, Wd, Hd, sbc_argb(SBC_GROUND));
    tk_blit(&t, d->b, -X, -Y, 1);
    tk_bmp_blur_sat(&t, PT(14), TK_GLASS_SAT, TK_GLASS_BRIGHT);
    tk_glass_local_put(d->b, &t, X, Y, bx, by, PT(w), PT(h), PT(r));
    tk_bmp_free(&t);
    tk_glass_bake(d->b, 0, bx, by, PT(w), PT(h), PT(r), dark, 1);
}
/* the same over the bare ground (nothing drawn under it): the blur of a flat colour is that colour, so no backdrop
 * buffer is needed (a card over the dimmed screen: its 4.6 MB backdrop refused on a full heap) */
static void d_lg_flat(dc *d, float x, float y, float w, float h, float r, int dark)
{
    tk_bmp g;
    uint32_t c = sbc_argb(SBC_GROUND);
    if (tk_bmp_new(&g, 1, 1)) {
        g.px[0] = c;
        tk_bmp_satbright(&g, TK_GLASS_SAT, TK_GLASS_BRIGHT);
        c = g.px[0];
        tk_bmp_free(&g);
    }
    tk_paint p = tk_solid(TK_RGBA((float)(c >> 16 & 255), (float)(c >> 8 & 255), (float)(c & 255), 1));
    tk_fill_rrect(d->b, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r), &p, 1);
    tk_glass_bake(d->b, 0, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r), dark, 1);
}
/* P3p (P3j-13): a pushed page's nav row, as iOS 26: a 44 pt glass circle at the leading edge with the back chevron
 * (no text), the title inline (17 pt / 600) centred. The circle's glass is baked once (init; 300 KB) and copied. */
#define BACK_X MARGIN
#define BACK_D 44.0f
#define BACK_HIT(x, y) (IN_NAV(y) && (x) < BACK_X + BACK_D + 12)
#define DONE_HIT(x, y) (IN_NAV(y) && (x) >= CONTENT_R - BACK_D - 12)
#define COMPOSE_D 48.0f
#define COMPOSE_Y 718.0f
#define COMPOSE_HIT(x, y) ((x) >= CONTENT_R - COMPOSE_D - 8 && (y) >= COMPOSE_Y - 6 && (y) < COMPOSE_Y + COMPOSE_D + 4)
/* iOS 26's bar buttons: glass circles (the back, Notes' Done and compose), each baked once at init (~400 KB each,
 * a 20 pt margin for the shadow) and copied into a page */
enum { GC_BACK, GC_DONE, GC_COMPOSE, GC_N };
static const float GCR[GC_N][3] = { { BACK_X, SAFE_TOP, BACK_D }, { CONTENT_R - BACK_D, SAFE_TOP, BACK_D },
                                     { CONTENT_R - COMPOSE_D, COMPOSE_Y, COMPOSE_D } };
static struct { tk_bmp b; float ox, oy; } GCB[GC_N];
static void gc_bake(int i)
{
    if (GCB[i].b.px) return;
    float mg = 20, x = GCR[i][0], y = GCR[i][1], s = GCR[i][2];
    GCB[i].ox = tk_floorf(PT(x - mg));
    GCB[i].oy = tk_floorf(PT(y - mg));
    if (!tk_bmp_new(&GCB[i].b, (int)PT(s + 2 * mg) + 2, (int)PT(s + 2 * mg) + 2)) return;
    dc d = { &GCB[i].b, 0, GCB[i].ox, GCB[i].oy };
    d_lg_flat(&d, x, y, s, s, s / 2, 1);
}
static void back_bake(void) { for (int i = 0; i < GC_N; i++) gc_bake(i); }
static void d_gcircle(dc *d, int i)
{
    gc_bake(i);
    if (GCB[i].b.px) tk_blit(d->b, &GCB[i].b, (int)(GCB[i].ox - d->ox), (int)(GCB[i].oy - d->oy), 1);
}
/* a stroked segment with round caps (a glyph's line: the check mark, the compose pencil), sw wide, in pt */
static void d_seg(dc *d, float x0, float y0, float x1, float y1, float sw, tk_col c)
{
    float hw = PT(sw) / 2, ax = PT(x0) - d->ox, ay = PT(y0) - d->oy, bx = PT(x1) - d->ox, by = PT(y1) - d->oy;
    int X0 = (int)tk_floorf(tk_minf(ax, bx) - hw) - 1, Y0 = (int)tk_floorf(tk_minf(ay, by) - hw) - 1;
    int X1 = (int)(tk_maxf(ax, bx) + hw) + 2, Y1 = (int)(tk_maxf(ay, by) + hw) + 2;
    tk_fm m;
    if (X1 <= X0 || Y1 <= Y0 || !tk_fm_new(&m, X1 - X0, Y1 - Y0)) return;
    float vx = bx - ax, vy = by - ay, vv = tk_maxf(1e-6f, vx * vx + vy * vy);
    for (int j = 0; j < m.h; j++)
        for (int i = 0; i < m.w; i++) {
            float px = X0 + i + .5f - ax, py = Y0 + j + .5f - ay, t = tk_clampf((px * vx + py * vy) / vv, 0, 1);
            float dx = px - t * vx, dy = py - t * vy;
            m.v[j * m.w + i] = tk_clampf(hw - tk_sqrtf(dx * dx + dy * dy) + .5f, 0, 1);
        }
    tk_paint p = tk_solid(c);
    tk_draw_fm(d->b, &m, X0, Y0, &p, 1);
    tk_fm_free(&m);
}
static void d_back(dc *d, tk_col chev)
{
    d_gcircle(d, GC_BACK);
    const tk_bmp *ic = tk_icon("back");
    if (ic) tk_blit_tinted(d->b, ic, (int)tk_floorf(PT(BACK_X + BACK_D / 2 - 7) - tk_icon_pad("back") - d->ox + .5f),
                           (int)tk_floorf(PT(NAV_MID - 10) - tk_icon_pad("back") - d->oy + .5f), chev);
}
static void d_navtitle(dc *d, const char *t)
{
    d_text_box(d, TK_IT600, 17, 195, NAV_BASE, 390 - 2 * (BACK_X + BACK_D + 10), 1, t, 0, 0, INK, 0);
}
static void d_h1(dc *d, const char *t) { d_text_box(d, TK_IT700, 34, MARGIN, LARGE_BASE, CONTENT_W, 0, t, 0, 0, INK, 0); }   /* P3j: iOS's Large Title, Bold; P3p: in its row */
static tk_col ACCENT(void) { return C_ACCENT; }
/* ---- helper B2: the component kit (shell_kit.inc) and Settings (shell_settings.inc), declared for the parts before
 * them ---- */
enum { KIT_DEFAULT, KIT_CANCEL, KIT_DESTRUCTIVE, KIT_PREFERRED };
typedef struct { const char *label; int style; } kit_btn;
typedef void (*kit_cb)(int i);
static int kit_alert(const char *title, const char *msg, const kit_btn *b, int n, kit_cb cb);
static int kit_actions(const char *title, const kit_btn *b, int n, kit_cb cb);
static int kit_sheet(const char *title, float content_h, void (*draw)(dc *d, float x, float y, float w), void (*tap)(float x, float y),
                     void (*done)(void));
static int kit_on(void);
static int se_owns(void);
static void se_build(void);
static void se_close(void);
static int se_refresh(float t);                     /* Track B: Settings' driver polls (Bluetooth, the lux reading) */
static float se_next_update(float t);
static int se_layers(tk_layer **o);
static int se_autocap(void);
static int se_timer_vib(void);
static int se_back_from_card(void);
static void press_cancel(void);
static void hl_set(tk_layer *l, tk_rect r, tk_rect clip, float rad, tk_col c);
static float rubber(float over, float dim);
void settings_back(void);                        /* a pushed Settings page's back: the Settings root */
/* Settings > Startup (Track J's shell_startup.inc: SHELL_STARTUP, page_startup / startup_tap, startup_tick with
 * SHELL_STARTUP_TICK) and Settings > Passcode (Track A's shell_passcode.inc: SHELL_PASSCODE, page_passcode /
 * passcode_tap, passcode_tick with SHELL_PASSCODE_TICK): their pages are drawn in the page strips (AS.sub
 * SE_STARTUP_PAGE / SE_PASSCODE_PAGE); without the file the row says "Coming soon" */
enum { SE_STARTUP_PAGE = 30, SE_PASSCODE_PAGE = 31, SE_TZ_PAGE = 32 };   /* (P3x: Track A's zone list, from Date & Time) */
#define PC_SUB SE_PASSCODE_PAGE                    /* shell_passcode.inc's sub-page is B2's Passcode row's */
#if __has_include("shell_startup.inc")
#define SH_HAS_STARTUP 1
static void page_startup(dc *d);
static int startup_tap(float x, float y);
#endif
#if __has_include("shell_passcode.inc")
#define SH_HAS_PASSCODE 1
static void page_passcode(dc *d);
static int passcode_tap(float x, float y);
#endif
/* a .row with an avatar: the name, an optional second line, the right-hand text, an optional unread dot */
__attribute__((unused)) static float d_prow(dc *d, float y, const char *name, int nfw, tk_col nc, const char *sub, const char *right, int dot, int last)
{
    float who = sub ? 36 : 34, rh = who + 26;
    d_av(d, 32, y + 13 + (who - 34) / 2, 34, name, 13);
    float rw = 0, sw = tw(TK_IT400, 16, " ", 0, TK_TNUM);
    if (right && right[0]) rw = tw(TK_IT400, 16, right, 0, TK_TNUM) + (dot ? sw + 4 + 9 : 0);
    float top = sub ? y + 13 : y + 20;
    d_text(d, nfw, 16, 78, bl(nfw, 16, top), name, 0, 0, nc, 0);
    if (sub) {
        char e[96];
        float avail = 358 - 78 - (rw > 0 ? rw + 10 : 0);
        ellipsize(e, sizeof e, TK_IT400, 13.5f, sub, tk_minf(230, avail));
        d_text(d, TK_IT400, 13.5f, 78, bl(TK_IT400, 13.5f, y + 33), e, 0, 0, C_LABEL2, 0);
    }
    if (rw > 0) {
        float rx = 358 - rw, rb = bl(TK_IT400, 16, y + 13 + (who - 20) / 2);
        d_text(d, TK_IT400, 16, rx, rb, right, 0, TK_TNUM, C_LABEL2, 0);
        if (dot) d_rrect(d, 358 - 9, rb - 9, 9, 9, 4.5f, C_ACCENT);
    }
    if (!last) d_rrect(d, GROUP_X, y + rh, GROUP_W, 1, 0, C_SEP);
    return rh + 1;
}

static const char *const KD[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };
static const char *const KL[12] = { "", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ", "", "+", "" };
static struct { int tab; char num[24]; int thread, sub; } AS;   /* the phone's tab (0 fav .. 3 pad), the dialled number, the open
                                                                 * thread; sub: Settings' Display & Brightness page (P3d) */
static char cell_hint[48] = "Cellular off";

/* P2y: an honest empty state (iOS's ContentUnavailableView): a title and up to three lines, centred, grey. P3p
 * (P3j-18): centred in its area (between the header and the bottom or a bar), not at a fixed top */
#define EMPTY_BOT 780.0f              /* the area's bottom without a bar */
#define EMPTY_TAB 718.0f              /* above the Phone's tab bar */
static void d_empty(dc *d, float area_top, float area_bot, const char *title, const char *line)
{
    char ln[4][96];
    float wm;
    int n = line ? wrapn(ln, 3, TK_IT400, 15, line, 300, &wm) : 0;
    float h = 36 + 20.0f * n - (n ? 2 : 8), top = tk_floorf((area_top + area_bot - h) / 2);
    d_text_box(d, TK_IT700, 22, 195, bl(TK_IT700, 22, top), CONTENT_W, 1, title, 0, 0, W(.9f), 0);
    for (int k = 0; k < n && k < 3; k++) {
        float lw = tw(TK_IT400, 15, ln[k], 0, 0);
        d_text(d, TK_IT400, 15, (390 - lw) / 2, bl(TK_IT400, 15, top + 36 + 20 * k), ln[k], 0, 0, C_LABEL2, 0);
    }
}
static void page_contacts(dc *d)
{
    d_h1(d, "Contacts");
    d_empty(d, CONTENT_TOP, AP.on && !tk_strcmp(APPS[AP.a].k, "phone") ? EMPTY_TAB : EMPTY_BOT, "No Contacts", "Contacts can't be added on this phone yet.");
}
/* helper I: Phone and Messages (shell_comms.inc, included after the other parts) */
static void page_phone(dc *d);
static void page_messages(dc *d);
static int cm_app_tap(float x, float y);
static void cm_extras(const char *k);
static void cm_opened(int a);
static void cm_closed(void);
static float cm_next_update(float t);
static int cm_refresh(float t);
static void cm_nc_launch(const char *who, const char *title, int a);
__attribute__((unused)) static void hsl(float h, float s, float l, float *r, float *g, float *b)
{
    float c = (1 - tk_fabsf(2 * l - 1)) * s, hp = h / 60, x = c * (1 - tk_fabsf(tk_fmodf(hp, 2) - 1)), m = l - c / 2;
    float R = 0, G2 = 0, B = 0;
    if (hp < 1) { R = c; G2 = x; } else if (hp < 2) { R = x; G2 = c; } else if (hp < 3) { G2 = c; B = x; }
    else if (hp < 4) { G2 = x; B = c; } else if (hp < 5) { R = x; B = c; } else { R = c; B = x; }
    *r = (R + m) * 255; *g = (G2 + m) * 255; *b = (B + m) * 255;
}
static void page_photos(dc *d)
{
    d_h1(d, "Photos");
    d_empty(d, CONTENT_TOP, EMPTY_BOT, "No Photos", "The camera doesn't work on this phone yet, so there is nothing here.");
}
static void page_camera(dc *d);                  /* shell_camera.inc: the Camera on Track A's front-camera driver */
static void camera_tap(float x, float y);
/* ---- P2z: the Clock app: the live time, a stopwatch, a timer that vibrates (no sound). The page's static part is
 * drawn in the strips; the stopwatch's and the timer's digits and the buttons are three small layers, redrawn
 * only when their text changes (the platform asks sh_next_update when to come back) ---- */
static int is_app(const char *k);
/* shell_media.inc: Photos, Files and Music (the microSD card, read only) */
static int med_is(const char *k);
static void med_build(void);
static void med_close(void);
static int med_layers(tk_layer **o);
static int med_tick(float dt);
static int med_moving(void);
static int med_pending(void);
static int med_work(uint32_t us, int slow);
static void med_touch(int ph, float x, float y, float t);
static int med_set(const char *what, const char *key, float v);
static float toast_left;
static tk_layer apsw, aptm, apctl;
static struct { int sw_run, laps, tm_run, sw_ds, tm_s, ctl_ver, ctl_drawn; float sw_t0, sw_acc, lap_at, tm_end; } CLK = { 0, 0, 0, -1, -1, 1, 0, 0, 0, 0, 0 };
#define CLK_DY 27.0f              /* P3p: the page under the iOS header band */
/* Track A time (shell_time.inc): Settings > Date & Time's zone list (Date & Time itself is shell_settings.inc's, the
 * Clock's Alarms tab shell_apps.inc's) */
static int ta_page(dc *d);
static int ta_tap(float x, float y);
static int ta_touch(int ph, float x, float y, float t);
static int ta_set(const char *what, const char *key, float v);
static void ta_restyle(void);
static void ta_tz_open(void);
#define TA_SUB_DT SE_DATETIME     /* AS.sub: B2's Settings > Date & Time; TA_SUB_TZ Track A's zone list (a strip page) */
#define TA_SUB_TZ SE_TZ_PAGE
static float clk_sw_ms(float t) { return CLK.sw_acc + (CLK.sw_run ? t - CLK.sw_t0 : 0); }
static int fmt_ms(char *b, float ms, int tenths)
{
    int ds = (int)(ms / 100), s = ds / 10, m = s / 60;
    int n = 0;
    if (m >= 60) { n += put_dec(b, m / 60); b[n++] = ':'; m %= 60; }
    b[n++] = (char)('0' + m / 10 % 10); b[n++] = (char)('0' + m % 10); b[n++] = ':';
    b[n++] = (char)('0' + s % 60 / 10); b[n++] = (char)('0' + s % 10);
    if (tenths) { b[n++] = '.'; b[n++] = (char)('0' + ds % 10); }
    b[n] = 0;
    return n;
}
/* helper B5: the Clock's tabs (World Clock, Alarms, Stopwatch, Timers) are in shell_apps.inc */
static void page_clock(dc *d);
static int clk_update(float t);
static void clk_tap(float x, float y);
static void ck_extras_of(const char *k);
static float isl_next_update(float t);              /* B7: shell_island.inc */
static int isl_refresh(float t);
static int isl_sync(void);
/* when the platform should come back for the clock (ms; < 0: nothing pending) */
int sh_next_update(float t)
{
    int open = AP.on && is_app("clock");
    float best = -1;
    if (CLK.sw_run && open) best = 100 - (float)((int)clk_sw_ms(t) % 100);
    if (CLK.tm_run) {
        float tl = CLK.tm_end - t, nx = open ? (float)((int)tl % 1000) + 1 : tl;
        if (nx < 1) nx = 1;
        if (best < 0 || nx < best) best = nx;
    }
    if (med_pending() && (best < 0 || best > 4)) best = 4;      /* the card's work, a turn at a time */
    float sv = sv_next_update(t);                       /* survival-next: SOS's beacon, USB power's state */
    if (sv >= 0 && (best < 0 || sv < best)) best = sv;
    float nb = nc_next_update(t);                       /* helper D: the banner's 4 s */
    if (nb >= 0 && (best < 0 || nb < best)) best = nb;
    float cb = cm_next_update(t);                       /* helper I: an ended call's screen */
    if (cb >= 0 && (best < 0 || cb < best)) best = cb;
    float ib = isl_next_update(t);                      /* B7: the pill's hold, USB power's capsule */
    if (ib >= 0 && (best < 0 || ib < best)) best = ib;
    float pb = pc_next_update(t);                       /* Track A: the back-off's minutes */
    if (pb >= 0 && (best < 0 || pb < best)) best = pb;
    float ub = su_next_update(t);                       /* Track A update: the staging's chunks */
    if (ub >= 0 && (best < 0 || ub < best)) best = ub;
    float sb = se_next_update(t);                       /* Track B: Settings' Bluetooth / Display polls */
    if (sb >= 0 && (best < 0 || sb < best)) best = sb;
    return (int)best;
}
int sh_refresh(float t)
{
    now_ms = t;
    int ch = clk_update(t);
    if (med_pending() && !sh_moving()) ch |= med_work(10000, 1);
    ch |= sv_refresh(t) | nc_refresh(t) | cm_refresh(t) | pc_refresh(t) | su_refresh(t);   /* survival-next: SOS's beacon, USB power's state; helper D: the banner; Track A: the back-off */
    ch |= isl_refresh(t);                              /* B7: the capsule (SOS, USB power), the pill's hold */
    ch |= se_refresh(t);                               /* Track B: Settings' Bluetooth list, the lux reading */
    if (ch && AP.on) sw_dirty = 1;                     /* helper D: the switcher's snapshot is old now */
    return ch;
}

/* ---- P2z: the Calculator (iOS's layout: the display right-aligned, the keys in five rows: grey functions, dark
 * digits, orange operators). The keys are the page; the display a small layer redrawn per key ---- */
static tk_layer apcalc;
static struct { double acc, cur; int op, entering, err, has_dot, ndig, frac; } CAL;
/* helper B5: the iOS 26 layout, the keys and the display are in shell_apps.inc */
static void page_calc(dc *d);
static void build_calc(void);
static void calc_tap(float x, float y);

/* P3b: the Flashlight app: one big switch (iOS's torch control), the same LED as the lock's button and the CC */
static void page_light(dc *d)
{
    d_h1(d, "Flashlight");
    int on = Q[0].lit;
    d_rrect(d, 135, 250, 120, 240, 34, on ? tk_hex(0xffffff, 1) : W(.16f));
    d_rrect(d, 135, on ? 250 : 370, 120, 120, 34, on ? C_YELLOW : W(.32f));
    const char *t = on ? "On" : "Off";
    float w = tw(TK_IT600, 20, t, 0, 0);
    d_text(d, TK_IT600, 20, (390 - w) / 2, bl(TK_IT600, 20, 520), t, 0, 0, INK, 0);
    const char *h = "Tap to turn the LED on or off";
    w = tw(TK_IT400, 15, h, 0, 0);
    d_text(d, TK_IT400, 15, (390 - w) / 2, bl(TK_IT400, 15, 556), h, 0, 0, C_LABEL2, 0);
}

/* P3c: the Calendar: this month's grid (Monday first, as iOS in Finland), today in a red circle, from the phone's
 * clock (sh_year: the year; the host's is 2026) */
static int cal_year = 2026;
#define CAL_DY 27.0f              /* P3p: the grid under the iOS header band */
void sh_year(int y) { if (y > 2000 && y < 2200) cal_year = y; }
static void page_calendar(dc *d)
{
    const char *mo = MO[(ST.mon + 11) % 12];
    d_text(d, TK_IT700, 34, MARGIN, LARGE_BASE, mo, 0, 0, INK, 0);
    char yb[8];
    put_dec(yb, cal_year);
    d_text(d, TK_IT600, 17, MARGIN + tw(TK_IT700, 34, mo, 0, 0) + 10, LARGE_BASE, yb, 0, 0, C_RED, 0);
    static const char *const H[7] = { "M", "T", "W", "T", "F", "S", "S" };
    for (int c = 0; c < 7; c++) {
        float w = tw(TK_IT600, 13, H[c], 0, 0);
        d_text(d, TK_IT600, 13, 20 + 50 * c + (50 - w) / 2, bl(TK_IT600, 13, 122 + CAL_DY), H[c], 0, 0, W(c >= 5 ? .4f : .6f), 0);
    }
    d_rrect(d, MARGIN, 146 + CAL_DY, CONTENT_W, .5f, 0, C_SEP);
    static const int DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int m = (ST.mon + 11) % 12, nd = DIM[m];
    if (m == 1 && ((cal_year % 4 == 0 && cal_year % 100 != 0) || cal_year % 400 == 0)) nd = 29;
    /* the 1st's weekday from today's (Sunday 0), as a Monday-first column */
    int wd1 = ((ST.wday - (ST.day - 1)) % 7 + 7) % 7, col0 = (wd1 + 6) % 7;
    for (int day = 1; day <= nd; day++) {
        int cell = col0 + day - 1, r = cell / 7, c = cell % 7;
        float cx = 20 + 50 * c + 25, cy = 160 + CAL_DY + 56 * r + 22;
        char b[4];
        put_dec(b, day);
        int today = day == ST.day;
        if (today) d_rrect(d, cx - 19, cy - 19, 38, 38, 19, C_RED);
        float w = tw(today ? TK_IT700 : TK_IT400, 20, b, 0, TK_TNUM);
        d_text(d, today ? TK_IT700 : TK_IT400, 20, cx - w / 2, bl(TK_IT400, 20, cy - 13), b, 0, TK_TNUM, today || c < 5 ? INK : C_LABEL2, 0);
    }
    float y = 160 + CAL_DY + 56 * 6 + 10;
    d_rrect(d, MARGIN, y, CONTENT_W, .5f, 0, C_SEP);
    char dt[48];
    const char *wd = WD[ST.wday % 7];
    int n = (int)tk_strlen(wd);
    tk_memcpy(dt, wd, (size_t)n); dt[n++] = ' ';
    n += put_dec(dt + n, ST.day); dt[n++] = ' ';
    tk_memcpy(dt + n, mo, tk_strlen(mo) + 1);
    d_text(d, TK_IT600, 15, 20, bl(TK_IT600, 15, y + 18), dt, 0, 0, W(.85f), 0);
    d_text(d, TK_IT400, 15, 20, bl(TK_IT400, 15, y + 46), "No events", 0, 0, C_LABEL2, 0);
}

/* the strip-drawn Settings pages' first group (Wi-Fi's, Cellular's, the Medical Card's; Display & Brightness moved to
 * shell_settings.inc) */
#define DSP_Y 150.0f

/* ---- P3e: Notes with an iOS keyboard. The list (each note's first line), New, and the editor: the text with a
 * caret over a dark keyboard (letters, shift, 123, space, return, delete). Track A: kept across restarts (the blob
 * "notes" in the state slot, where the phone has one). ---- */
#define NNOTES 8
#define NOTELEN 600
static char NOTES[NNOTES][NOTELEN];
static int NOTE_HM[NNOTES];                   /* B5: when each was last written (hh * 100 + mm), its row's second line */
#define NOTE_RH 64.0f                         /* B5 (P3j-18): a row: the title, then the time and the text after it */
static struct { int n, view, cur, shift, num; } NS;
/* Track A: the notes kept in the blob "notes" (the store saves 3 s after the last change): version 1, the count, then
 * each note as a 2-byte length and its bytes */
static void notes_save(void)
{
    static uint8_t b[2 + NNOTES * (2 + NOTELEN)];
    unsigned n = 2;
    b[0] = 1;
    b[1] = (uint8_t)NS.n;
    for (int i = 0; i < NS.n; i++) {
        unsigned l = (unsigned)tk_strlen(NOTES[i]);
        b[n] = (uint8_t)l; b[n + 1] = (uint8_t)(l >> 8);
        tk_memcpy(b + n + 2, NOTES[i], l);
        n += 2 + l;
    }
    (void)sh_blob_put("notes", b, n);
}
static void notes_load(void)
{
    static uint8_t b[2 + NNOTES * (2 + NOTELEN)];
    int n = sh_blob_get("notes", b, sizeof b);
    if (n < 2 || b[0] != 1 || b[1] > NNOTES) return;
    int at = 2, k = 0;
    for (; k < b[1] && at + 2 <= n; k++) {
        int l = b[at] | b[at + 1] << 8;
        if (l >= NOTELEN || at + 2 + l > n) break;
        tk_memcpy(NOTES[k], b + at + 2, (size_t)l);
        NOTES[k][l] = 0;
        at += 2 + l;
    }
    NS.n = k;
}
static tk_layer apnote;
static void build_app_page(int a);
static void note_title(const char *t, char *out)
{
    int k = 0;
    while (t[k] && t[k] != '\n' && k < 128) { out[k] = t[k]; k++; }
    while (k > 0 && t[k] && ((unsigned char)t[k] & 0xc0) == 0x80) k--;     /* P3p: whole characters (d_text_box cuts it) */
    out[k] = 0;
    if (!k) tk_memcpy(out, "New Note", 9);
}
#include "shell_kb.inc"                      /* B3: the keyboard (Notes, Search, Messages, the editors, Linux) */
static void page_notes(dc *d)
{
    if (!NS.view) {
        d_h1(d, "Notes");
        /* P3p (audit 13): iOS 26's compose button in the bottom bar (a glass circle, the pencil in Notes' yellow),
         * the count centred beside it */
        d_gcircle(d, GC_COMPOSE);
        {
            float cx = CONTENT_R - COMPOSE_D / 2, cy = COMPOSE_Y + COMPOSE_D / 2, x0 = cx - 10, y0 = cy - 8, x1 = cx + 7, y1 = cy + 9;
            tk_col yl = C_YELLOW;
            d_seg(d, x0 + 2, y0, x0 + 8, y0, 2, yl);
            d_seg(d, x0, y0 + 2, x0, y1 - 2, 2, yl);
            d_seg(d, x0, y0 + 2, x0 + 2, y0, 2, yl);
            d_seg(d, x0, y1 - 2, x0 + 2, y1, 2, yl);
            d_seg(d, x0 + 2, y1, x1 - 2, y1, 2, yl);
            d_seg(d, x1 - 2, y1, x1, y1 - 2, 2, yl);
            d_seg(d, x1, y1 - 2, x1, y0 + 9, 2, yl);
            d_seg(d, x0 + 7, y0 + 10, x1 + 3, y0 - 6, 2.6f, yl);     /* the pencil */
        }
        {
            char cnt[24];
            int k = put_dec(cnt, NS.n);
            tk_memcpy(cnt + k, NS.n == 1 ? " Note" : " Notes", NS.n == 1 ? 6 : 7);
            if (NS.n) d_text_box(d, TK_IT400, 13, 195, COMPOSE_Y + COMPOSE_D / 2 + 4.7f, 200, 1, cnt, 0, 0, C_LABEL2, 0);
        }
        if (!NS.n) { d_empty(d, CONTENT_TOP, EMPTY_BOT, "No Notes", "Tap the pencil to write one."); return; }   /* trunk: notes persist now */
        d_rrect(d, GROUP_X, CONTENT_TOP, GROUP_W, NOTE_RH * NS.n, 16, C_CARD);
        for (int i = 0; i < NS.n; i++) {
            char t[136], sub[160];
            int k = NS.n - 1 - i, hm = NOTE_HM[k];
            const char *s2 = NOTES[k];
            note_title(s2, t);
            while (*s2 && *s2 != '\n') s2++;              /* the second line: the time, then the text after the title */
            while (*s2 == '\n' || *s2 == ' ') s2++;
            int q = 0;
            sub[q++] = (char)('0' + hm / 1000 % 10); sub[q++] = (char)('0' + hm / 100 % 10); sub[q++] = ':';
            sub[q++] = (char)('0' + hm / 10 % 10); sub[q++] = (char)('0' + hm % 10); sub[q++] = ' '; sub[q++] = ' ';
            if (!*s2) for (const char *c = "No additional text"; *c; c++) sub[q++] = *c;
            for (; *s2 && q < 150; s2++) sub[q++] = *s2 == '\n' ? ' ' : *s2;
            sub[q] = 0;
            float top = CONTENT_TOP + NOTE_RH * i;
            d_text_box(d, TK_IT600, 17, ROW_TX, bl(TK_IT600, 17, top + 11), CONTENT_W - 32, 0, t, 0, 0, INK, 0);
            d_text_box(d, TK_IT400, 15, ROW_TX, bl(TK_IT400, 15, top + 35), CONTENT_W - 32, 0, sub, 0, 0, C_LABEL2, 0);
            if (i + 1 < NS.n) d_rrect(d, ROW_TX, top + NOTE_RH - .5f, CONTENT_R - ROW_TX, .5f, 0, C_SEP);
        }
        return;
    }
    d_back(d, C_YELLOW);                /* P3p (audit 13): Notes' yellow, on the glass circle */
    d_gcircle(d, GC_DONE);                         /* and Done: a yellow check mark */
    {
        float cx = CONTENT_R - BACK_D / 2, cy = NAV_MID;
        d_seg(d, cx - 8, cy + .5f, cx - 2.5f, cy + 6, 2.6f, C_YELLOW);
        d_seg(d, cx - 2.5f, cy + 6, cx + 8.5f, cy - 6.5f, 2.6f, C_YELLOW);
    }
    kb_draw(d, NS.shift, NS.num);
}
static float note_w, note_bot = PUSHED_TOP;   /* B3: the text's box (the edit menu's selection) */
static void build_note(void)
{
    relayer(&apnote);
    if (!NS.view || !is_app("notes")) { apnote.hidden = 1; return; }
    const char *t = NOTES[NS.cur];
    /* B3: down to the keyboard's strip (KB_TOP - KB_STRIP = 500) */
    dc d = mk(&apnote, "note-text", MARGIN - 4, PUSHED_TOP - 12, CONTENT_W + 8, KB_TOP - KB_STRIP - 8 - PUSHED_TOP, 0, 0);
    /* the lines: the text's own line breaks, then words wrapped at 350 pt (17 pt, 24 pt lines), the caret after */
    char line[NOTELEN + 2];
    float y = PUSHED_TOP;
    int i = 0, n = (int)tk_strlen(t);
    float cx = 20, cy = PUSHED_TOP;
    note_w = 0;
    int first = 1, cfirst = 1;
    while (i <= n && y < KB_TOP - KB_STRIP - 20) {
        int k = 0, lastsp = -1, start = i, f = first ? TK_IT700 : TK_IT400;
        float fs = first ? 28 : 17;
        while (i < n && t[i] != '\n') {
            line[k] = t[i]; line[k + 1] = 0;
            if (tw(f, fs, line, 0, 0) > 350) {
                if (lastsp > 0) { i = start + lastsp + 1; k = lastsp; }
                break;
            }
            if (t[i] == ' ') lastsp = k;
            k++; i++;
        }
        line[k] = 0;
        d_text(&d, f, fs, 20, bl(f, fs, y), line, 0, 0, INK, 0);
        cx = 20 + tw(f, fs, line, 0, 0);
        if (cx - 20 > note_w) note_w = cx - 20;
        cy = y;
        cfirst = first;
        y += first ? 38 : 24;
        if (i < n && t[i] == '\n') first = 0;
        if (i < n && t[i] == '\n') { i++; if (i == n) { cx = 20; cy = y; cfirst = 0; } continue; }
        if (i >= n) break;
    }
    d_rrect(&d, cx + 1, cy + 1, 2, cfirst ? 33 : 21, 1, C_YELLOW);   /* the caret (the title's taller) */
    note_bot = cy + (cfirst ? 38 : 24);
    trim(&apnote);                                 /* B3: a 9 MB box for a few lines: cut to them (the keyboard's heap) */
    apnote.hidden = 0;
    relink();
}
static void notes_tap(float x, float y)
{
    if (!NS.view) {
        if (COMPOSE_HIT(x, y)) {                      /* P3p: the compose button (was "New" in the nav row) */
            if (NS.n == NNOTES) { toast("Up to eight notes in this version"); return; }
            NOTES[NS.n][0] = 0;
            NOTE_HM[NS.n] = ST.h * 100 + ST.mi;
            NS.cur = NS.n++;
            NS.view = 1; NS.shift = se_autocap(); NS.num = 0;   /* helper B2: Keyboard > Auto-Capitalization */
        } else if (y >= CONTENT_TOP && y < CONTENT_TOP + NOTE_RH * NS.n && x >= GROUP_X && x < GROUP_X + GROUP_W) {
            NS.cur = NS.n - 1 - (int)((y - CONTENT_TOP) / NOTE_RH);
            NS.view = 1; NS.shift = 0; NS.num = 0;
        } else return;
        kb_focus(KB_AUTOCAP | KB_NL, 0, NOTES[NS.cur]);   /* B3: return is a new line; sentences start with a capital */
        build_app_page(AP.a);
        return;
    }
    if (BACK_HIT(x, y) || DONE_HIT(x, y)) {           /* back or Done: an empty note is dropped */
        if (!NOTES[NS.cur][0]) {
            for (int i = NS.cur; i + 1 < NS.n; i++) { tk_memcpy(NOTES[i], NOTES[i + 1], NOTELEN); NOTE_HM[i] = NOTE_HM[i + 1]; }
            NS.n--;
        }
        NS.view = 0;
        notes_save();                                 /* Track A: kept (an empty note dropped) */
        build_app_page(AP.a);
        return;
    }
    if (y < KB_TOP) return;
    /* B3: the keyboard's key (shift, 123 and the capitals are the keyboard's own): only the text redraws */
    if (kb_edit(NOTES[NS.cur], NOTELEN - 1)) {   /* Track A: a typed or deleted character is kept; B5: the note's time */
        NOTE_HM[NS.cur] = ST.h * 100 + ST.mi;
        notes_save();
        build_note();
    }
}

/* ---- P3g: Search (the home's Search pill, as iOS's): a field, the keyboard, the apps whose names contain what is
 * typed (all of them while it is empty) as icons; a tap opens one ---- */
static struct { char q[24]; int shift, num, hits[8], nh; } SR;
static tk_layer apsrch;
static int ci_has(const char *s, const char *q)
{
    if (!q[0]) return 1;
    for (; *s; s++) {
        int k = 0;
        while (q[k] && s[k] && (s[k] | 32) == (q[k] | 32)) k++;
        if (!q[k]) return 1;
    }
    return 0;
}
static void page_search(dc *d)
{
    d_text(d, TK_IT400, 17, 316, bl(TK_IT400, 17, 70), "Cancel", 0, 0, ACCENT(), 0);
    kb_draw(d, SR.shift, SR.num);
}
static tk_bmp sfield;              /* P3i: the search field's glass, while Search is open */
static void build_search(void)
{
    relayer(&apsrch);
    if (!is_app("search")) { apsrch.hidden = 1; return; }
    dc d = mk(&apsrch, "search-results", 12, 52, 366, 480, 0, 0);
    /* P3i: the field's glass (over the flat ground: no blur) baked once per opening and copied at each key */
    if (!sfield.px || sfield.w != d.b->w) {
        tk_bmp_free(&sfield);
        if (tk_bmp_new(&sfield, d.b->w, (int)PT(56 + 40 + 44 - 52))) {
            dc f = { &sfield, 0, d.ox, d.oy };
            d_lg_flat(&f, 16, 56, 288, 40, 20, 1);
        }
    }
    if (sfield.px) tk_bmp_copy(d.b, 0, 0, &sfield, 0, 0, sfield.w, sfield.h);
    else d_lg_flat(&d, 16, 56, 288, 40, 20, 1);
    d_icon(&d, "search15", 30, 68.5f, 1);
    if (SR.q[0]) d_text(&d, TK_IT400, 17, 53, bl(TK_IT400, 17, 65), SR.q, 0, 0, INK, 0);
    else d_text(&d, TK_IT400, 17, 53, bl(TK_IT400, 17, 65), "Search", 0, 0, C_LABEL2, 0);
    if (SR.q[0]) d_rrect(&d, 53 + tw(TK_IT400, 17, SR.q, 0, 0) + 1, 66, 2, 21, 1, ACCENT());
    SR.nh = 0;
    for (int a = 0; a < NAPPS && SR.nh < 8; a++) {
        if (!tk_strcmp(APPS[a].k, "search") || !ci_has(APPS[a].name, SR.q)) continue;
        float pad;
        if (!app_icon(APPS[a].k, &pad)) continue;
        SR.hits[SR.nh] = a;
        float x = COL_CX(SR.nh % 4) - ICON_PT / 2, y = 148 + ROW_PITCH * (SR.nh / 4);
        d_appicon(&d, APPS[a].k, x, y);
        d_text_box(&d, TK_IT500, 12, x + 31, bl(TK_IT500, 12, y + 67), 84, 1, APPS[a].name, 0, 0, INK, 0);
        SR.nh++;
    }
    d_text(&d, TK_IT600, 13, 24, bl(TK_IT600, 13, 116), SR.nh ? (SR.q[0] ? "APPLICATIONS" : "SUGGESTIONS") : "NO RESULTS", .4f, 0, C_LABEL2, 0);
    trim(&apsrch);                                 /* B3: cut to what it holds (the keyboard's heap) */
    apsrch.hidden = 0;
    relink();
}
static void app_finish_close(void);
static void app_open(int a);
static void app_close(float v);
static void search_tap(float x, float y)
{
    if (y >= 52 && y < 92 && x >= 306) { app_close(0); return; }     /* Cancel: back into the pill */
    if (y >= 148 && y < 148 + 2 * ROW_PITCH && y < 540 && x >= 15) {
        int col = (int)((x - 15) / 90), row = (int)((y - 148) / ROW_PITCH), i = row * 4 + col;
        if (col >= 0 && col < 4 && i < SR.nh) { int a = SR.hits[i]; app_finish_close(); app_open(a); return; }
    }
    if (y < KB_TOP) return;
    /* B3: the keyboard's key; return opens the first app found; a space doesn't start the query */
    const char *ks;
    int ev = kb_take(&ks);
    if (ev == KBE_RET) { if (SR.nh) { int a = SR.hits[0]; app_finish_close(); app_open(a); } return; }
    if (ev == KBE_TEXT && ks[0] == ' ' && !SR.q[0]) return;
    if (kb_edit(SR.q, (int)sizeof SR.q - 2)) build_search();
}

static tk_layer apkey, apnum, apdel;   /* apdel: the keypad's delete (P3j) */
static void build_apnum(void)
{
    relayer(&apnum);
    relayer(&apdel);
    apnum.hidden = apdel.hidden = !AS.num[0];
    if (!AS.num[0]) return;
    {
        /* P3j (audit 12): iOS's delete beside the call button once a number is typed (a chevron and a cross) */
        dc e = mk(&apdel, "dial-del", 280, 594, 40, 22, 1, 0);
        const tk_bmp *ic = tk_icon("back");
        if (ic) tk_blit_tinted(e.b, ic, (int)tk_floorf(PT(281) - tk_icon_pad("back") - e.ox + .5f),
                               (int)tk_floorf(PT(595) - tk_icon_pad("back") - e.oy + .5f), W(.85f));
        d_rrect(&e, 291, 595, 27, 20, 4, W(.85f));
        float xw = tw(TK_IT600, 16, "\xc3\x97", 0, 0);
        d_text(&e, TK_IT600, 16, 291 + (27 - xw) / 2, bl(TK_IT600, 16, 596), "\xc3\x97", 0, 0, C_GROUND, 0);
    }
    float w = tw(TK_IT500, 36, AS.num, 1, TK_TNUM);
    dc d = mk(&apnum, "dial", (390 - w) / 2, 86, w, 44, 2, 0);
    d_text(&d, TK_IT500, 36, (390 - w) / 2, bl(TK_IT500, 36, 86), AS.num, 1, TK_TNUM, INK, 0);
    relink();
}
/* P2y: the live values the app rows show ("@..."; anything else as it is) */
static struct { int charging; char ver[40]; } SYS = { 0, "Starbite OS Next" };
void sh_alarm(int on, int h, int m)
{
    if (ALM.on == !!on && (!on || (ALM.h == h && ALM.m == m))) return;
    ALM.on = !!on; ALM.h = h; ALM.m = m;
    build_lwidgets();
    relink();
    lock_ver++;
}
void sh_sysinfo(int charging, const char *version)
{
    if (!!charging != sys_charging) { sys_charging = !!charging; build_lstatus(); build_home_sb(); build_home_w(); if (AP.on) build_apsb(); relink(); lock_ver++; prep_tree(home.child); }
    SYS.charging = charging;
    int n = 0;
    while (version && version[n] && n < 39) { SYS.ver[n] = version[n]; n++; }
    if (version) SYS.ver[n] = 0;
}
static const char *wf_row_text(void);               /* shell_wifi.inc */
static void page_wifi(dc *d);
static void page_cell(dc *d);         /* shell_cell.inc: Settings > Cellular */
static void cell_tap(float x, float y);
static void wifi_tap(float x, float y);
static const char *pc_live(const char *s);            /* Track A: shell_passcode.inc */
static void pc_page(dc *d);
static int pc_app_tap(float x, float y);
static const char *live_text(const char *s, char *buf)
{
    { const char *pl = pc_live(s); if (pl) return pl; }   /* Track A: @pass */
    if (!tk_strcmp(s, "@wifi")) return wf_row_text();
    if (s[0] != '@') return s;
    int n = 0;
    if (!tk_strcmp(s, "@pct")) { n = put_dec(buf, ST.pct); buf[n++] = ' '; buf[n++] = '%'; buf[n] = 0; }
    else if (!tk_strcmp(s, "@chg")) return SYS.charging ? "Yes" : "No";
    else if (!tk_strcmp(s, "@temp")) { n = put_dec(buf, ST.temp); tk_memcpy(buf + n, " \xc2\xb0" "C", 5); }
    else if (!tk_strcmp(s, "@torch")) return Q[0].lit ? "On" : "Off";
    else if (!tk_strcmp(s, "@bright")) {
        if (!cc_bright) return "Default";
        tk_memcpy(buf, "Level ", 6); n = 6 + put_dec(buf + 6, cc_bright); tk_memcpy(buf + n, " of 4", 6);
    } else if (!tk_strcmp(s, "@time")) fmt_hm(buf);
    else if (!tk_strcmp(s, "@tz")) { const char *c = ""; sh_tz_name(sh_tz(), &c); return c; }   /* Track A */
    else if (!tk_strcmp(s, "@date")) {
        const char *w = WD[ST.wday % 7], *m = MO[(ST.mon + 11) % 12];
        n = (int)tk_strlen(w); tk_memcpy(buf, w, (size_t)n); buf[n++] = ' ';
        n += put_dec(buf + n, ST.day); buf[n++] = ' ';
        tk_memcpy(buf + n, m, tk_strlen(m) + 1);
    } else if (!tk_strcmp(s, "@ver")) return SYS.ver;
    else return "";
    return buf;
}
/* P3i: Settings' row icons (the Control Centre's glyphs, tinted white) */
typedef struct { const char *row, *glyph; uint32_t col; float w, h; } setico;
static const setico SETICO[] = {
    { "Wi-Fi", "cc-wifi", 0x0a84ff, 19, 14 }, { "Cellular", "cc-cell", 0x30d158, 18, 16 }, { "Battery", "cc-save", 0x30d158, 20, 12 },
    { "Display & Brightness", "cc-sun", 0x0a84ff, 20, 20 }, { "Sounds & Haptics", "cc-vol", 0xff375f, 20, 20 },
    { "Bluetooth", "cc-bt", 0x0a84ff, 12, 18 }, { "Airplane Mode", "cc-air", 0xff9f0a, 18, 18 }, { "Date & Time", "cc-timer", 0x8e8e93, 19, 19 },
    { "Medical Card", "heart", 0xff3b30, 18, 18 },      /* survival-next */
    { "Face ID & Passcode", "lock-c", 0x30d158, 15, 20 },   /* Track A */
};
static const setico *set_icon(const char *app, const char *row)
{
    if (tk_strcmp(app, "settings")) return 0;
    for (unsigned i = 0; i < sizeof SETICO / sizeof SETICO[0]; i++) if (!tk_strcmp(SETICO[i].row, row)) return &SETICO[i];
    return 0;
}
/* P3i: the apps with nothing to show yet get iOS's centred empty state, not a lone grey row */
typedef struct { const char *k, *title, *line; } infopg;
static const infopg INFO[] = {
    { "weather", "No Weather", "Weather needs the internet. This phone isn't online yet." },
    { "music", "No Music", "There is no music on this phone." },
    { "files", "No Files", "Files isn't in this version yet." },
    { "terminal", "Not Here Yet", "The terminal and Linux apps run in Starbite OS." },
    { "browser", "No Internet", "The browser needs the internet. This phone isn't online yet." },
    { "heat", "Not Here Yet", "Survival Heat runs in Starbite OS." },
};
static const infopg *info_of(const char *k)
{
    for (unsigned i = 0; i < sizeof INFO / sizeof INFO[0]; i++) if (!tk_strcmp(INFO[i].k, k)) return &INFO[i];
    return 0;
}
/* P3i: the status bar over every app (iOS never hides it); rebuilt with the minute and the charger */
static tk_layer apsb;
static void build_apsb(void)
{
    relayer(&apsb);
    if (!AP.on) { apsb.hidden = 1; return; }
    dc d = mk(&apsb, "app-sb", 0, 0, 390, 44, 0, 0);
    char hm[8];
    fmt_hm(hm);
    status_bar(&d, hm);
    apsb.hidden = 0;
    apsb.alpha = appc.alpha;
    tk_layer_prepare(&apsb);
}
static void page_maps(dc *d);                  /* helper D: shell_nc.inc */
static int app_own(int a)
{
    const char *k = APPS[a].k;
    return !tk_strcmp(k, "phone") || !tk_strcmp(k, "messages") || !tk_strcmp(k, "contacts") || !tk_strcmp(k, "photos") ||
           !tk_strcmp(k, "camera") || !tk_strcmp(k, "clock") || !tk_strcmp(k, "calc") || !tk_strcmp(k, "light") ||
           !tk_strcmp(k, "calendar") || !tk_strcmp(k, "settings") || !tk_strcmp(k, "notes") ||
           !tk_strcmp(k, "search") || info_of(k) || med_is(k) || lx_is(k) || sv_own(k) || !tk_strcmp(k, "maps");
}
static void own_strip(int a, int i)
{
    const char *k = APPS[a].k;
    const infopg *inf = info_of(k);
    if (i == 0) KB.want = 0;                            /* B3: the page says again whether the keyboard is up */
    if (med_is(k)) { relayer(&appst[i]); return; }      /* shell_media.inc: its own layers (med_build), no strips */
    if (lx_is(k) && !lx_strips(k)) { relayer(&appst[i]); appst[i].hidden = 1; return; }   /* shell_linux.inc: the same */
    if (!tk_strcmp(k, "settings") && se_owns()) { relayer(&appst[i]); appst[i].hidden = 1; return; }   /* helper B2: its own layers */
    if (!tk_strcmp(k, "settings") && i == 0) se_build();   /* helper B2: a strip page next: Settings' own page goes first (the heap) */
    dc d = strip_dc(i, "app-page");
    if (!d.b->px) return;
    if (!tk_strcmp(k, "phone")) page_phone(&d);
    else if (!tk_strcmp(k, "messages")) page_messages(&d);
    else if (!tk_strcmp(k, "contacts")) page_contacts(&d);
    else if (!tk_strcmp(k, "photos")) page_photos(&d);
    else if (!tk_strcmp(k, "clock")) page_clock(&d);
    else if (!tk_strcmp(k, "calc")) page_calc(&d);
    else if (!tk_strcmp(k, "light")) page_light(&d);
    else if (!tk_strcmp(k, "calendar")) page_calendar(&d);
    else if (!tk_strcmp(k, "settings")) {
        if (AS.sub == 2) page_wifi(&d);
        else if (AS.sub == 3) page_cell(&d);
        else if (AS.sub == 4) sv_page_card_edit(&d);
#ifdef SH_HAS_STARTUP
        else if (AS.sub == SE_STARTUP_PAGE) page_startup(&d);    /* Track J's */
#endif
#ifdef SH_HAS_PASSCODE
        else if (AS.sub == SE_PASSCODE_PAGE) page_passcode(&d);  /* Track A's */
#endif
        else if (AS.sub == SU_SUB) su_page(&d);                  /* Track A update: Software Update */
        else if (AS.sub == SE_TZ_PAGE) (void)ta_page(&d);        /* Track A's zone list (Date & Time is shell_settings.inc's) */
    }
    else if (!tk_strcmp(k, "notes")) page_notes(&d);
    else if (!tk_strcmp(k, "search")) page_search(&d);
    else if (lx_is(k)) lx_page(&d);
    else if (sv_page(k, &d)) ;                       /* survival-next */
    else if (!tk_strcmp(k, "maps")) page_maps(&d);   /* helper D */
    else if (inf) { d_h1(&d, APPS[a].name); d_empty(&d, CONTENT_TOP, EMPTY_BOT, inf->title, inf->line); }
    else page_camera(&d);
    if (KB.want || ap_slim(k)) kb_slim(&appst[i]);   /* B3 / B5: one trim: a strip cut to what it holds (gone if empty) */
    tk_layer_prepare(&appst[i]);
}
static void own_extras(int a)
{
    const char *k = APPS[a].k;
    build_apnum();
    if (med_is(k)) med_build();
    if (!tk_strcmp(k, "settings")) se_build();       /* helper B2: Settings' own pages (none for the strips' pages) */
    if (!tk_strcmp(k, "notes")) build_note();
    if (!tk_strcmp(k, "search")) build_search();
    if (lx_is(k)) lx_build();
    ck_extras_of(k);                                 /* helper B5: the Clock's and the Calculator's layers */
    sv_extras(k);                                    /* survival-next: the tools' live layers */
    cm_extras(k);                                    /* helper I */
    kb_commit();                                     /* B3: the keyboard up or down, as the page drew */
    relink();
}
/* P3i: an opening app's own page drawn over its first frames (two strips a frame, then the extras) instead of all
 * at the tap: the tap's 30-110 ms (Mac) block was a pause before the window moved. Its content is hidden until
 * the page is whole (the icon covers the window then). */
static int ap_pending;
static void app_build_step(void)
{
    if (!ap_pending || !AP.on) { ap_pending = 0; return; }
    int st = ap_pending - 1;
    if (st < NST / 2) { own_strip(AP.a, 2 * st); own_strip(AP.a, 2 * st + 1); relink(); ap_pending++; return; }
    own_extras(AP.a);
    ap_pending = 0;
}
/* P3p: the generic page's layout: the top of row r of section s (its drawing and the taps share it) */
static float gen_row_y(const appdef *A, int s, int r)
{
    float y = CONTENT_TOP - 18;
    for (int i = 0; i < A->ns; i++) {
        const asec *S = &A->sec[i];
        y += S->cap[0] ? (i ? 18 : 16) + 16 + 6 : 18;
        if (i == s) return y + 47.0f * r;
        y += 47.0f * S->n - 1;
    }
    return y;
}

static int ap_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c);
static void build_app_page(int a)
{
    const infopg *inf = info_of(APPS[a].k);
    int own = app_own(a);
    ap_pending = 0;
    build_apsb();
    if (own) {
        for (int i = 0; i < NST; i++) own_strip(a, i);
        own_extras(a);
        return;
    }
    (void)inf;
    KB.want = 0;                                        /* B3: no keyboard on a generic page */
    kb_commit();
    /* the concept's genericScreen: the large title, captions (or an 18 pt gap), grouped rows */
    const appdef *A = &APPS[a];
    float y = CONTENT_TOP - 18;                         /* P3p: under the large title's row */
    float h = y;
    for (int i = 0; i < A->ns; i++) h += (A->sec[i].cap[0] ? 34 : 18) + 47.0f * A->sec[i].n;
    for (int i = 1; i < NST; i++) relayer(&appst[i]);
    relayer(&appst[0]);
    apnum.hidden = 1;
    dc d = mk(&appst[0], "app-page", 0, 0, 390, h + 4, 0, 0);
    d_h1(&d, A->name);
    for (int i = 0; i < A->ns; i++) {
        const asec *S = &A->sec[i];
        if (S->cap[0]) {
            /* .sec: 13 px, upper case, .04 em, .55; margin 18 36 6 (the 18 collapses with the title's 12) */
            char up[64];
            int k = 0;
            for (const char *c = S->cap; *c && k < 63; c++) up[k++] = *c >= 'a' && *c <= 'z' ? (char)(*c - 32) : *c;
            up[k] = 0;
            d_text(&d, TK_IT400, 13, ROW_TX, y + (i ? 18 : 16) + 13, up, .52f, 0, C_LABEL2, 0);
            y += (i ? 18 : 16) + 16 + 6;
        } else
            y += 18;
        float gh = 47.0f * S->n - 1;
        d_rrect(&d, GROUP_X, y, GROUP_W, gh, 16, C_CARD);
        for (int r = 0; r < S->n; r++) {
            float ry = y + 47 * r;
            char lb[48], rb[48];
            float lx = ROW_TX;
            const setico *si = set_icon(A->k, S->rows[r].l);
            if (si) {
                /* P3i: iOS Settings' 29 pt colour square with its white glyph; the label and the rule after it */
                d_rrect(&d, ROW_TX, ry + 9, 29, 29, 7, tk_hex(si->col, 1));
                const tk_bmp *ic = tk_icon(si->glyph);
                if (ic) {
                    float pad = tk_icon_pad(si->glyph);
                    tk_blit_tinted(d.b, ic, (int)tk_floorf(PT(ROW_TX + (29 - si->w) / 2) - pad - d.ox + .5f),
                                   (int)tk_floorf(PT(ry + 9 + (29 - si->h) / 2) - pad - d.oy + .5f), W(1));
                }
                lx = ROW_TX + 29 + 13;
            }
            int nav = !tk_strcmp(A->k, "settings") && (!tk_strcmp(S->rows[r].l, "Display & Brightness") || !tk_strcmp(S->rows[r].l, "Wi-Fi") ||
                                                       !tk_strcmp(S->rows[r].l, "Cellular") || !tk_strcmp(S->rows[r].l, "Medical Card") ||
                                                       !tk_strcmp(S->rows[r].l, "Face ID & Passcode") ||
                                                       !tk_strcmp(S->rows[r].l, "Date & Time") ||   /* Track A */
                                                       !tk_strcmp(S->rows[r].l, "Software"));      /* Track A update */
            if (nav) d_icon(&d, "chev", CONTENT_R - 30, ry + 16.5f, .45f);
            const char *rt = live_text(S->rows[r].r, rb);
            if (!tk_strcmp(A->k, "settings") && !tk_strcmp(S->rows[r].l, "Cellular")) rt = md_text();
            /* P3p (P3j-14): one line each; the value gives way first (to half the row), then the label */
            const char *lt = live_text(S->rows[r].l, lb);
            float right = nav ? CONTENT_R - 36 : CONTENT_R - 16, room = right - lx;
            float lw = tw(TK_IT400, 16, lt, 0, 0), vw = rt[0] ? tw(TK_IT400, 16, rt, 0, TK_TNUM) : 0;
            float lmax = rt[0] ? tk_maxf(room - vw - 16, room / 2) : room;
            d_text_box(&d, TK_IT400, 16, lx, ry + 13 + 16, lmax, 0, lt, 0, 0, INK, 0);
            if (rt[0]) d_text_box(&d, TK_IT400, 16, right, ry + 13 + 16, room - tk_minf(lw, lmax) - 16, 2, rt, 0, TK_TNUM, C_LABEL2, 0);
            if (r + 1 < S->n) d_rrect(&d, lx, ry + 46, CONTENT_R - lx, .5f, 0, C_SEP);
        }
        y += gh;
    }
    tk_layer_prepare(&appst[0]);
    relink();
}
/* P2x: the app's icon over the window while it is small (iOS: the icon grows into the app and the app shrinks back
 * into its icon, so a closing window never ends as an empty tile); a view into the pack, never freed */
static tk_layer apicon, apdim;               /* apdim: B4, the dim under an opening / closing window */
static void place_apicon(arect R, float p)
{
    float pad;
    const tk_bmp *ic = app_icon(APPS[AP.a].k, &pad);    /* P3p: Calendar's is today's */
    float a = 1 - tk_clampf(p * 3, 0, 1);
    if (!ic || !AP.on || a <= 0) { apicon.hidden = 1; return; }
    if (apicon.bmp.px != ic->px) { tk_layer_dirty(&apicon); apicon.bmp = *ic; }
    float sx = R.w / 62;
    apicon.sx = apicon.sy = sx;
    apicon.ox = apicon.oy = 0;
    apicon.x = (PT(R.x) - pad * sx) / sx;
    apicon.y = (PT(R.y + (R.h - R.w) / 2) - pad * sx) / sx;    /* centred in a window still taller than wide */
    apicon.alpha = a;
    apicon.hidden = 0;
}
static void set_app_rect(arect R)
{
    AP.R = R;
    float s = R.w / 390, ch = R.h / s, inset = (H_PT - ch) / 2;
    appg.sx = appg.sy = s;
    appg.ox = appg.oy = 0;
    appg.x = PT(R.x) / s;
    appg.y = PT(R.y - inset * s) / s;
    appg.clip = 1;
    appg.clip_r = (tk_rect){ PT(R.x), PT(R.y), PT(R.w), PT(R.h) };
    appg.clip_radius = PT(R.r);
    float p = (R.w - 62) / (390 - 62);
    appc.alpha = ap_pending ? 0 : tk_clampf(p * 1.6f - .2f, 0, 1);
    if (sw_app_alpha()) appc.alpha = ap_pending ? 0 : 1;      /* helper D: a card in the switcher shows its page whole */
    apsb.alpha = appc.alpha;
    kbg.alpha = appc.alpha;                                  /* B3: the keyboard fades in and out with the page */
    uint32_t t = APPS[AP.a].tint, b = sv_ground(AP.a);
    float m = tk_clampf(p * 2, 0, 1);
    uint32_t cr = (uint32_t)((t >> 16 & 255) * (1 - m) + (b >> 16 & 255) * m + .5f), cg = (uint32_t)((t >> 8 & 255) * (1 - m) + (b >> 8 & 255) * m + .5f),
             cb = (uint32_t)((t & 255) * (1 - m) + (b & 255) * m + .5f);
    appbg.solid = 0xff000000u | cr << 16 | cg << 8 | cb;
    appbg.gen++;
    if (!locked) {                       /* survival-next: an app over the lock (Emergency's cards): home stays away */
        home.sx = home.sy = 1 + .08f * p;   /* B4: iOS's zoom behind an opening app (and back on the close) */
        home.alpha = 1 - .4f * tk_clampf(p, 0, 1);
    }
    /* B4: the wallpaper (and home) dim under the window as it grows; nothing to draw once it covers the screen */
    apdim.solid = tk_premul(TK_RGBA(0, 0, 0, 1));
    apdim.solid_w = TK_W; apdim.solid_h = TK_H;
    apdim.alpha = .3f * tk_clampf(p, 0, 1);
    apdim.hidden = !AP.on || sw_app_alpha() || apdim.alpha <= .004f || (R.w >= 389.5f && R.y <= .5f && R.x > -.5f && R.x < .5f);
    appg.hidden = !AP.on;
    place_apicon(R, p);
}
static int lx_icon_rect(const char *k, arect *r);
static int sv_icon_rect(int a, arect *r);
static arect icon_rect(int a)
{
    arect lr;
    if (sv_icon_rect(a, &lr)) return lr;                 /* survival-next: opened from the lock's Emergency card */
    static const char *P1[8] = { "calendar", "weather", "clock", "camera", "photos", "music", "notes", "calc" };
    const char *const *P2 = EMG ? P2E : P2K;
    static const char *DK[4] = { "phone", "messages", "browser", "settings" };
    const char *k = APPS[a].k;
    for (int i = 0; i < 4; i++) if (!tk_strcmp(DK[i], k)) return (arect){ COL_CX(i) - ICON_PT / 2, DOCK_Y, ICON_PT, ICON_PT, 15 };
    if (!tk_strcmp(k, "search")) return (arect){ SRCH_X, SRCH_Y, SRCH_W, SRCH_H, SRCH_H / 2 };   /* P3g: the Search pill (B8: §9.2) */
    { arect r; if (lx_icon_rect(k, &r)) return r; }                       /* L3: page 3 */
    for (int i = 0; i < 8; i++) if (!tk_strcmp(P1[i], k)) return (arect){ COL_CX(i % 4) - ICON_PT / 2 + page_x, HOME_TOP1 + ROW_PITCH * (i / 4), ICON_PT, ICON_PT, 15 };
    for (int i = 0; i < NP2; i++) if (!tk_strcmp(P2[i], k)) return (arect){ 390 + COL_CX(i % 4) - ICON_PT / 2 + page_x, HOME_TOP2 + ROW_PITCH * (i / 4), ICON_PT, ICON_PT, 15 };
    return (arect){ 164, 370, 62, 62, 15 };
}
/* the app under a tap on home (the page's grid or the dock), or -1 */
static int app_hit(float x, float y)
{
    static const char *P1[8] = { "calendar", "weather", "clock", "camera", "photos", "music", "notes", "calc" };
    const char *const *P2 = EMG ? P2E : P2K;
    static const char *DK[4] = { "phone", "messages", "browser", "settings" };
    const char *k = 0;
    if (y >= DOCK_TOP && y <= DOCK_TOP + DOCK_H && x >= 15) { int i = (int)((x - 15) / 90); if (i < 4) k = DK[i]; }
    else if (SRCH_HIT(x, y)) k = "search";
    else {
        int pg = tk_mini(2, tk_maxi(0, (int)tk_floorf(-page_x / 390 + .5f)));
        float x0 = x - (390 * pg + page_x), top = pg ? HOME_TOP2 : HOME_TOP1;
        int col = (int)((x0 - 15) / 90), row = (int)((y - top) / ROW_PITCH);
        if (pg == 2) return lx_hit(x0, y);           /* L3: the Linux page */
        if (x0 >= 15 && y >= top && col < 4 && row < (pg ? 3 : 2) && (!pg || row * 4 + col < NP2) && y - top - row * ROW_PITCH < 83)
            k = (pg ? P2 : P1)[row * 4 + col];
    }
    if (!k) return -1;
    for (int a = 0; a < NAPPS; a++) if (!tk_strcmp(APPS[a].k, k)) return a;
    return -1;
}
static void app_open(int a);
/* Track A, 1.0: open an app by its key (the Control Centre's app tiles); 0 when there is none */
static int open_app_key(const char *k)
{
    for (int a = 0; a < NAPPS; a++)
        if (!tk_strcmp(APPS[a].k, k)) { app_open(a); return 1; }
    return 0;
}
static int ap_from_on;                     /* helper D: the next open grows from this rect (a switcher card) */
static arect ap_from;
static unsigned char ap_warm[NAPPS];       /* Track A lifecycle: opened before and not thrown away: its state kept */
static int sw_keeps(int a);
static int sw_find(int a);
static void app_open(int a)
{
    int cold = !ap_warm[a] || (sw_keeps(a) && sw_find(a) < 0);   /* Track A lifecycle: first, or out of the recents */
    ap_warm[a] = 1;
    sw_opened(a);                              /* helper D: first in the recents */
    AP.a = a;
    AP.on = 1;
    AP.closing = AP.following = 0;
    free_cc();
    if (cold) {                                /* Track A lifecycle: a warm open keeps the tab, number, thread, sub-page */
        if (!tk_strcmp(APPS[a].k, "phone")) { AS.tab = 3; AS.num[0] = 0; }
        if (!tk_strcmp(APPS[a].k, "messages")) AS.thread = -1;
        if (!tk_strcmp(APPS[a].k, "settings")) AS.sub = 0;
        if (!tk_strcmp(APPS[a].k, "notes")) NS.view = 0;
    }
    cm_opened(a);                              /* helper I: Settings > Cellular, a notification's thread */
    sv_opened();
    if (!tk_strcmp(APPS[a].k, "search")) kb_focus(0, "search", "");   /* B3: Search's field has the focus */
    apkey.hidden = 1;
    if (app_own(a)) {
        /* the page over the first frames (app_build_step); the previous app's strips gone now */
        for (int i = 0; i < NST; i++) { relayer(&appst[i]); appst[i].hidden = 1; }
        apnum.hidden = 1;
        build_apsb();
        relink();
        ap_pending = 1;
    } else
        build_app_page(a);
    if (lx_is(APPS[a].k)) lx_open(a);            /* L3: the Linux app (started), or the App Store */
    AP.icon = ap_from_on ? ap_from : icon_rect(a);
    ap_from_on = 0;
    AP.from = AP.icon;
    AP.to = FULLR;
    tk_spring_start(&sp_app, 0, 1, 0, TK_SPR_OPEN);
    set_app_rect(AP.icon);
    { const apphook *h = ap_hook(a); if (h && h->on_show) h->on_show(cold); }   /* Track A lifecycle */
}
static void sw_commit(void);
static void app_close(float v)
{
    sw_commit();                                 /* B4: a quick switch's app goes first in the recents now */
    AP.closing = 1;
    AP.from = AP.R;
    AP.to = icon_rect(AP.a);
    tk_spring_start(&sp_app, 0, 1, tk_minf(6, tk_fabsf(v) * 2), TK_SPR_CLOSE);
}
static void app_finish_close(void)
{
    if (AP.on) { const apphook *h = ap_hook(AP.a); if (h && h->on_hide) h->on_hide(); }   /* Track A lifecycle */
    sw_app_gone();                               /* helper D: no window in the switcher */
    lx_close();                                  /* L3: the window's strips, the store's layers */
    ap_pending = 0;
    AP.on = AP.closing = 0;
    appg.hidden = 1;
    apicon.hidden = 1;
    apdim.hidden = 1;
    /* the page (a whole screen) is not kept: the next open draws it again */
    for (int i = 0; i < NST; i++) relayer(&appst[i]);
    relayer(&apnum);
    relayer(&apdel); apdel.hidden = 1;
    relayer(&apsw); relayer(&aptm); relayer(&apctl);           /* P2z: the clock's (the timer keeps running) */
    apsw.hidden = aptm.hidden = apctl.hidden = 1;
    relayer(&apcalc); apcalc.hidden = 1;
    relayer(&apnote); apnote.hidden = 1;
    relayer(&apsrch); apsrch.hidden = 1;
    cm_closed();                                 /* helper I */
    kb_close();                                  /* B3 */
    em_close(0);
    se_close();                                  /* helper B2: Settings' page layers */
    med_close();
    tk_bmp_free(&sfield);
    relayer(&apsb); apsb.hidden = 1;
    SR.shift = 0; SR.num = 0;                                   /* Track A lifecycle: Search keeps its words */
    /* Track A lifecycle: Notes reopens where it was (the list or the note) */                        /* the Calculator keeps its value (CAL) */
    apnum.hidden = apkey.hidden = 1;
    home.sx = home.sy = 1;
    home.alpha = 1;
    sv_closed();                                                /* survival-next (over the lock: home hidden again) */
}

/* ---- the toast: B7 made it the live pill's expanded state (shell_island.inc: toast(), the island's group toastl) */
static tk_layer toastl;

/* ---- sleep and wake (P2r): the screen off as a fade to black, the screen on as the lock screen growing in from
 * black (iOS's wake: a little larger to its place); while black the shell goes back to the lock screen. The kernel
 * turns the panel off once the fade has ended (sh_slept) ---- */
static tk_layer slp;
static struct { int dir, black, back; float t; } SL;   /* dir -1 going dark, 1 waking, 0 still; black: dark at rest;
                                                       * back: a fade out undone (no lock, no growing) */
#define SL_OUT .26f
#define SL_IN .42f
static void sl_place(void)
{
    float a = SL.black ? 1 : 0, s = 1;
    if (SL.dir < 0) a = tk_bezier(.4f, 0, .7f, 1, SL.t);
    else if (SL.dir > 0) {
        float e = tk_bezier(.2f, .7f, .2f, 1, SL.t);
        a = 1 - tk_minf(1, SL.t * 1.6f) * (2 - tk_minf(1, SL.t * 1.6f));     /* the black goes in 60 % of the time */
        (void)e;
        s = 1;      /* P2v: no growing (the phone: a scaled lock is 15 ms a frame; at 1:1 its half copies draw fast) */
    }
    slp.alpha = a;
    slp.hidden = a <= 0;
    lockg.sx = lockg.sy = s;
}

/* ---- the kernel's hold confirmation (kui_confirm: the modem's Start). Helper B2: on the kit's alert (IOS-REFERENCE
 * 3.1): the screen dimmed (.5), a 300 pt glass card (r 36, the lens: no bake) with the kernel's words left-aligned,
 * its hold as the primary capsule filling blue (1.5 s, as the kernel's own), Cancel under it; a tap on Cancel or
 * outside the card cancels ---- */
#define CF_X ((390 - 300) / 2.0f)
#define CF_BX (CF_X + 14)
#define CF_BW 272.0f
static tk_layer cfl, cfdim, cfcard, cffill, cflabel;
static struct { int on, down; float t0, p, a, at, hy, cy, top, bot; } CF;
static void kit_shape(tk_layer *l, float x, float y, float w, float h, float r);
static void cf_place(void)
{
    cffill.slice_h = PT(48 + (CF_BW - 48) * CF.p);
    cfl.alpha = tk_bezier(.25f, .1f, .25f, 1, CF.a);     /* B4: eased, not linear */
    cfl.hidden = !CF.on && CF.a <= 0;
}
void sh_confirm(const char *title, const char *l1, const char *l2, const char *l3, const char *hold)
{
    const char *L[3] = { l1, l2, l3 };
    char tl[4][96], ln[3][4][96];
    int nl[3];
    float wm;
    int nt = title && title[0] ? wrapn(tl, 2, TK_IT600, 17, title, 252, &wm) : 0;
    float yb = 37 + 22.0f * (nt ? nt - 1 : 0);
    int first = 1;        /* the last baseline so far, from the card's top */
    float mb[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        nl[i] = L[i] && L[i][0] ? wrap(ln[i], TK_IT400, 15, L[i], 252, &wm) : 0;
        if (!nl[i]) continue;
        mb[i] = yb + (first ? (nt ? 32 : 0) : 26);                 /* a paragraph: 6 more than a line */
        yb = mb[i] + 20.0f * (nl[i] - 1);
        first = 0;
    }
    float h = yb + 28 + 48 + 8 + 48 + 14, y = tk_floorf((H_PT - h) / 2);
    tk_layer_init(&cfl, "confirm");
    tk_layer_init(&cfdim, "confirm-dim");
    cfdim.solid = 0x80000000u;                /* black at .5 (premultiplied) */
    cfdim.solid_w = TK_W;
    cfdim.solid_h = TK_H;
    relayer(&cfcard);
    dc d = mk(&cfcard, "confirm-card", CF_X, y, 300, h, 2, 0);
    if (tk_glass_lens) kit_shape(&cfcard, PT(CF_X) - d.ox, PT(y) - d.oy, PT(300), PT(h), PT(36));
    else d_lg_flat(&d, CF_X, y, 300, h, 36, 1);
    d_rrect(&d, CF_X, y, 300, h, 36, tk_hex(0x1e1e20, .78f));
    for (int k = 0; k < nt; k++) d_text(&d, TK_IT600, 17, CF_X + 24, y + 37 + 22.0f * k, tl[k], 0, 0, INK, 0);
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < nl[i]; k++) d_text(&d, TK_IT400, 15, CF_X + 24, y + mb[i] + 20.0f * k, ln[i][k], 0, 0, TK_RGBA(235, 235, 245, .6f), 0);
    CF.top = y;
    CF.bot = y + h;
    CF.hy = y + yb + 28;
    CF.cy = CF.hy + 48 + 8;
    d_rrect(&d, CF_BX, CF.hy, CF_BW, 48, 24, sbca(SBC_ACCENT, .3f));          /* the hold: the primary capsule's track */
    d_rrect(&d, CF_BX, CF.cy, CF_BW, 48, 24, tk_hex(0x363639, 1));            /* Cancel */
    d_text_box(&d, TK_IT600, 17, 195, bl(TK_IT600, 17, CF.cy + (48 - lh(TK_IT600, 17)) / 2), CF_BW - 20, 1, "Cancel", 0, 0, INK, 0);
    relayer(&cffill);
    dc f = mk(&cffill, "confirm-fill", CF_BX, CF.hy, 48, 48, 1, 0);
    d_rrect(&f, CF_BX, CF.hy, 48, 48, 24, C_ACCENT);
    cffill.slice = 2;
    cffill.slice_s0 = cffill.slice_s1 = PT(24);
    relayer(&cflabel);
    dc l = mk(&cflabel, "confirm-label", CF_BX, CF.hy, CF_BW, 48, 1, 0);
    d_text_box(&l, TK_IT600, 17, 195, bl(TK_IT600, 17, CF.hy + (48 - lh(TK_IT600, 17)) / 2), CF_BW - 20, 1, hold, 0, 0, INK, 0);
    tk_layer *k[] = { &cfdim, &cfcard, &cffill, &cflabel };
    link(&cfl, k, 4);
    relink();
    CF.on = 1;
    CF.down = 0;
    CF.p = 0;
    CF.a = 0;
    CF.at = 1;
    cf_place();
}
static void cf_close(int ok)
{
    CF.on = 0;
    CF.down = 0;
    CF.at = 0;
    sh_confirmed(ok);
}
static void cf_free(void)
{
    relayer(&cfcard); relayer(&cffill); relayer(&cflabel);
    cfl.child = 0;
    cfl.hidden = 1;
}

/* ---- Emergency mode (the concept's setEmergency) ---------------------------------------------------------------
 * The wallpaper and its glass backdrop cross-fade (.7 s ease) to the other ground: both move in place toward a
 * target made ahead (two half-resolution buffers, freed at the end), so every glass element follows the fade. The
 * lock's widgets, its list (four cards and Exit Emergency), the pill, the flame, home's battery widget and its second
 * page, the Control Centre's tile change with it. Visual: no Emergency function runs. */
static tk_bmp em_wall, em_bw;
static float em_t = 1, em_e = 1;
typedef struct { tk_bmp *a; const tk_bmp *t; int f; } lerp_ctx;
static void lerp_rows(void *v, int y0, int y1)
{
    lerp_ctx *L = v;
    for (int j = y0; j < y1; j++) {
        uint32_t *a = L->a->px + (size_t)j * L->a->stride;
        const uint32_t *t = L->t->px + (size_t)j * L->t->stride;
        /* B6: two channels at once, the same rounding as x + ((y - x) f + 128) >> 8 per channel (x (256 - f) + y f + 128
         * is at most 65408: no carry into the next channel) */
        uint32_t f = (uint32_t)L->f, g = 256 - f;
        for (int i = 0; i < L->a->w; i++) {
            uint32_t p = a[i], q = t[i];
            if (p == q) continue;
            uint32_t rb = ((p & 0xff00ffu) * g + (q & 0xff00ffu) * f + 0x800080u) >> 8 & 0xff00ffu;
            uint32_t ag = ((p >> 8 & 0xff00ffu) * g + (q >> 8 & 0xff00ffu) * f + 0x800080u) & 0xff00ff00u;
            a[i] = rb | ag;
        }
    }
}
static void em_step(float e)
{
    if (em_wall.px && em_bw.px) {
        float f = e >= 1 ? 1 : (e - em_e) / (1 - em_e);
        lerp_ctx L = { &C->base_half, &em_wall, (int)(f * 256 + .5f) };
        if (L.f > 0) {
            tk_par_rows(0, C->base_half.h, lerp_rows, &L);
            L.a = &C->bw_half;
            L.t = &em_bw;
            tk_par_rows(0, C->bw_half.h, lerp_rows, &L);
            C->ground_gen++;
        }
    }
    em_e = e;
    star.alpha = EMG ? 1 - e : e;
    star_e.alpha = EMG ? e : 1 - e;
    star.hidden = star.alpha <= 0;
    star_e.hidden = star_e.alpha <= 0;
    if (e >= 1) { tk_bmp_free(&em_wall); tk_bmp_free(&em_bw); }
}
static void build_home_w(void);
static void build_home_apps2(void);
static void build_cc_grid(void);
static void cc_link(void);
/* ---- B6: the switch prepared ahead -------------------------------------------------------------------------------
 * set_emergency made all of it at once: ~600 ms in one frame on the phone (387 on the Mac). Now the Emergency hold's
 * 700 ms (and the Control Centre tile's) prepare the switch in a slice of each of its frames (EP_SLICE_US): the other
 * ground (the wallpaper a band at a time, the star in and the quarter blurred, the backdrop's rows up), then,
 * entering, Emergency's four cards and Exit (hidden), then the lock's widgets for that mode (aside, in ep_lw); a big
 * piece only as a frame's first. A hold let go frees all of it. The switch itself is cheap: the widgets swapped in,
 * the prepared ground handed to the cross-fade. What is not ready when it fires (all of it after Exit Emergency's
 * tap) is made over the next frames, the lock's first (its cards and widgets, then the ground: the fade waits for
 * it); home's widgets and its second page, never seen from the lock, at the idle turns after the fade (at once if home
 * is about to show). Never two grounds at a time: nothing is prepared while a fade runs. The Control Centre's
 * Emergency tile is a layer of its own (ccem): the switch shows or hides it. */
#define EP_SLICE_US 7000           /* a frame's share of the work, us (no fade runs while there is any of it) */
#define EP_ROWS 64                 /* a band of the wallpaper or the backdrop, half-res rows (64: over the cores) */
enum { EP_CARDS = 1, EP_LW = 2, EP_HOMEW = 4, EP_APPS2 = 8 };
static tk_layer ep_lw;             /* the lock's widgets for the mode prepared, until the switch */
static struct {
    int goal;                      /* the mode being prepared (-1 none); goal == EMG: switched, the rest being made */
    int wy, qd, uy, fail;          /* wallpaper rows; the quarter made 1, blurred 2, toned 3; backdrop rows; no room */
    tk_bmp wall, bw, q;
    float amb;                     /* the new ground's Lb for glass that cannot see it (-1: not known) */
    int cards;                     /* entering: 0..3 cards drawn into ecard[0], 4 Exit, 5 the layers below the fold */
    int lw;                        /* ep_lw's pieces made (3: all), for the values in lwkey */
    uint32_t lwkey;
    int ground;                    /* the ground is the fade's (em_wall / em_bw): 1 handed over this frame, 2 fading */
    int post;                      /* after the switch: the EP_* still to make */
    int fresh;                     /* the switch was in this frame (a tap's): no more work in it */
} EP = { -1, 0, 0, 0, 0, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, -1, 0, 0, 0, 0, 0, 0 };
static void ep_reset(void)
{
    tk_bmp_free(&EP.wall);
    tk_bmp_free(&EP.bw);
    tk_bmp_free(&EP.q);
    relayer(&ep_lw);
    EP.goal = -1;
    EP.wy = EP.qd = EP.uy = EP.fail = EP.cards = EP.lw = EP.ground = EP.post = EP.fresh = 0;
    EP.lwkey = 0;
    EP.amb = -1;
}
static void ep_begin(int on)
{
    ep_reset();
    EP.goal = on;
}
/* a hold let go (or the mode set some other way): what was made for it goes */
static void ep_cancel(void)
{
    if (EP.goal == 1 && !EMG && EP.cards) free_ecards();
    ep_reset();
}
static void ep_noroom(void)
{
    tk_bmp_free(&EP.wall);
    tk_bmp_free(&EP.bw);
    tk_bmp_free(&EP.q);
    EP.fail = 1;
    EP.amb = -1;
}
static int ep_ground_done(void) { return EP.fail || (EP.qd == 3 && EP.uy >= C->bw_half.h); }
/* the next piece of the target ground (as tk_wall_draw + tk_glass_backdrop of the wallpaper with the star of that
 * mode made it); 2: a big one (the blur, the saturation), 1: a band, 0: it is whole, -1: a big one is next */
static int ep_ground_step(int big_ok)
{
    if (ep_ground_done()) return 0;
    int on = EP.goal;
    if (EP.wy < C->base_half.h) {
        if (!EP.wall.px && !tk_bmp_new(&EP.wall, C->base_half.w, C->base_half.h)) { ep_noroom(); return 1; }
        tk_wall_draw_rows(&EP.wall, on, .5f, EP.wy, EP.wy + EP_ROWS);
        EP.wy += EP_ROWS;
        return 1;
    }
    if (!EP.qd) {
        /* the star into the wallpaper (what is under it kept), the quarter from both, the wallpaper as it was */
        if (!tk_bmp_new(&EP.q, EP.wall.w / 2, EP.wall.h / 2)) { ep_noroom(); return 1; }
        const tk_bmp *st = tk_icon(on ? "star-e" : "star");
        tk_bmp sh = { 0, 0, 0, 0 }, keep = { 0, 0, 0, 0 };
        int sx = (int)(on ? star_e.x : star.x) / 2, sy = (int)star.y / 2;
        if (st && tk_bmp_new(&sh, (st->w + 1) / 2, (st->h + 1) / 2) && tk_bmp_new(&keep, sh.w, sh.h)) {
            tk_bmp_down2(&sh, st);
            tk_bmp_copy(&keep, 0, 0, &EP.wall, sx, sy, sh.w, sh.h);
            tk_blit(&EP.wall, &sh, sx, sy, 1);
            tk_bmp_down2(&EP.q, &EP.wall);
            tk_bmp_copy(&EP.wall, sx, sy, &keep, 0, 0, sh.w, sh.h);
        } else
            tk_bmp_down2(&EP.q, &EP.wall);
        tk_bmp_free(&sh);
        tk_bmp_free(&keep);
        EP.qd = 1;
        return 1;
    }
    if (EP.qd < 3) {                       /* the blur, then the saturation and the mean: a frame each */
        if (!big_ok) return -1;
        float a = tk_glass_backdrop_q(&EP.q, EP.qd - 1);
        if (EP.qd++ == 2) EP.amb = a;
        return 2;
    }
    if (!EP.bw.px && !tk_bmp_new(&EP.bw, C->bw_half.w, C->bw_half.h)) { ep_noroom(); return 1; }
    tk_glass_backdrop_up(&EP.bw, &EP.q, EP.uy, EP.uy + EP_ROWS);
    EP.uy += EP_ROWS;
    if (EP.uy >= C->bw_half.h) tk_bmp_free(&EP.q);
    return 1;
}
/* the ground to the fade: from this frame on the wallpaper and backdrop move toward it */
static void ep_handover(void)
{
    tk_bmp_free(&em_wall);
    tk_bmp_free(&em_bw);
    em_wall = EP.wall;
    em_bw = EP.bw;
    EP.wall = EP.bw = (tk_bmp){ 0, 0, 0, 0 };
    tk_bmp_free(&EP.q);
    if (EP.fail) tk_log("sbtk: Emergency: no room for the other wallpaper");
    else if (EP.amb >= 0) tk_glass_amb_set(EP.amb);
    em_e = 0;
    EP.ground = 1;
}
/* Emergency's cards a piece at a time (as they were made at once), their glass toned for the new ground */
static void ep_card(void)
{
    int i = EP.cards;
    float amb0 = EP.amb >= 0 ? tk_glass_amb_set(EP.amb) : -1;
    if (i < 4) {
        if (!i) {
            mk(&ecard[0], "ecards", CARD_X, LIST_TOP + 10, CARD_W, 78 * 3 + 68, FACE_MG, 1);   /* P3q: shapes recorded */
            ecard[0].hidden = 1;
            relink();
        }
        dc d = { &ecard[0].bmp, &ecard[0].glass, ecard[0].x, ecard[0].y };
        ec_card(&d, i, LIST_TOP + 10 + 78 * i);
    } else if (i == 4) {
        float top = LIST_TOP + 10 + 78 * 8 - 10 + 12, w = 14 + tw(TK_IT600, 13, "Exit Emergency", 0, 0) + 14, x = 195 - w / 2;
        dc d = mk(&ecard[4], "eexit", x, top, w, 32, 44, 1);
        ecard[4].hidden = 1;
        d_glass(&d, x, top, w, 32, 16, 1, 1);
        d_text(&d, TK_IT600, 13, x + 14, top + 8 + 13, "Exit Emergency", 0, 0, INK, 0);
        tk_a8_free(&ecard[0].glass);
        trim(&ecard[0]);
        trim(&ecard[4]);
        tk_layer_prepare(&ecard[0]);
        tk_layer_prepare(&ecard[4]);
        relink();
    } else {
        /* survival-next: cards 4..7 below the fold: their layers now, the cards at the next idle turns (sv_idle) */
        for (int k = 0; k < 2; k++) {
            mk(&ecard[5 + k], k ? "ecards3" : "ecards2", CARD_X, LIST_TOP + 10 + 78 * (4 + 2 * k), CARD_W, 78 + 68, FACE_MG, 0);
            ecard[5 + k].hidden = 1;
        }
        ec_next = 4;
        relink();
    }
    if (amb0 >= 0) tk_glass_amb_set(amb0);
    EP.cards++;
    if (EP.cards >= 5 && ec_wait) { ec_wait = 0; place_list(); }
}
/* what the lock's widgets show (prepared ones are swapped in only if it is still the same) */
static uint32_t ep_lwkey(void)
{
    int am = ALM.h * 60 + ALM.m, when = !ALM.on ? 0 : am > ST.h * 60 + ST.mi ? 1 : 2;
    return (uint32_t)(ST.pct & 255) | (uint32_t)(ST.temp & 255) << 8 | (uint32_t)(sh_last2() & 1) << 16 |
           (uint32_t)when << 17 | (uint32_t)(am & 2047) << 19;
}
/* the lock's widgets for the mode, aside (the shown ones stay as they are): a ring, the other, the middle one
 * (EP.lw 3: made) */
static void ep_lw_step(void)
{
    dc d = { &ep_lw.bmp, &ep_lw.glass, ep_lw.x, ep_lw.y };
    if (!EP.lw) {
        relayer(&ep_lw);
        d = lw_mk(&ep_lw);
        EP.lwkey = ep_lwkey();
    }
    lw_draw(&d, EP.goal, EP.lw);
    if (++EP.lw == 3) {
        trim(&ep_lw);
        if (EP.lwkey != ep_lwkey()) EP.lw = 0;     /* the values changed meanwhile: again */
    }
}
/* the made widgets in place of the shown ones (relink after: the tree's links are the shown layer's) */
static void ep_lw_swap(void)
{
    uint32_t g = lwidgets.gen > ep_lw.gen ? lwidgets.gen : ep_lw.gen;
    relayer(&lwidgets);
    lwidgets = ep_lw;
    lwidgets.gen = g + 1;
    tk_layer_init(&ep_lw, "ep-lw");
    EP.lw = 0;
}
/* before the switch: the ground, then (entering) the cards and Exit, then the widgets; -1: a big piece is next (one a
 * frame), 0: all done */
static int ep_pre_step(int big_ok)
{
    int r = ep_ground_step(big_ok);
    if (r) return r;
    if (EP.goal == 1 && EP.cards < 5) {
        if (!big_ok) return -1;
        ep_card();
        return 2;
    }
    if (EP.lw < 3) {
        if (!big_ok) return -1;
        ep_lw_step();
        return 2;
    }
    return 0;
}
/* after it: the lock's pieces first (one big piece a frame), the ground's rest and its hand-over, home's at idle
 * (idle) or when it shows; 0 when all done */
static int ep_post_step(int big_ok, int idle)
{
    if (EP.post & (EP_CARDS | EP_LW) && big_ok) {
        if (EP.post & EP_CARDS) {
            ep_card();
            if (EP.cards > 5) EP.post &= ~EP_CARDS;
        } else {
            ep_lw_step();
            if (EP.lw == 3) {
                EP.post &= ~EP_LW;
                ep_lw_swap();
                relink();
            }
        }
        lock_ver++;
        return 2;
    }
    if (!EP.ground) {
        int r = ep_ground_step(big_ok);
        if (r) return r;
        ep_handover();                       /* (the fade from the next frame: this one did the bands) */
        return 2;
    }
    if (EP.post & (EP_CARDS | EP_LW)) return -1;
    if (!EP.post) { ep_reset(); return 0; }
    if (!idle || !big_ok) return -1;
    if (EP.post & EP_HOMEW) { EP.post &= ~EP_HOMEW; build_home_w(); }
    else { EP.post &= ~EP_APPS2; build_home_apps2(); }
    relink();
    return 2;
}
/* a slice of the work: small pieces until EP_SLICE_US, a big one only as the slice's first */
static void ep_work(int idle)
{
    uint64_t t0 = tk_us();
    for (int first = 1; EP.goal >= 0; first = 0) {
        if (!first && tk_us() - t0 >= EP_SLICE_US) break;
        int r = EP.goal == EMG ? ep_post_step(first, idle) : ep_pre_step(first);
        if (r <= 0 || r == 2) break;
    }
}
/* home is about to show (the lock lifting, the Control Centre closed): its pieces now */
static void ep_home_now(void)
{
    if (EP.post & EP_HOMEW) build_home_w();
    if (EP.post & EP_APPS2) build_home_apps2();
    EP.post &= ~(EP_HOMEW | EP_APPS2);
    relink();
}
/* all of it now (a set state, the reset): a switch's rest made, a prepared one dropped */
static void ep_finish(void)
{
    if (EP.goal >= 0 && EP.goal != EMG) ep_cancel();
    while (EP.goal >= 0 && ep_post_step(1, 1) > 0) ;
}
static void set_emergency(int on, int instant)
{
    if (on == EMG) return;
    if (EP.goal >= 0 && EP.goal != on) ep_cancel();   /* back before the last switch was whole: made again */
    if (EP.goal < 0) ep_begin(on);                   /* nothing prepared (Exit Emergency's tap): over the next frames */
    EMG = on;
    if (!on) { free_ecards(); cards_pending = LOCK_PARTS; }
    /* a fade still running stops where it is: the next one starts from there */
    tk_bmp_free(&em_wall);
    tk_bmp_free(&em_bw);
    em_e = 0;
    em_t = 0;
    EP.ground = 0;
    EP.post = (on ? EP_CARDS : 0) | EP_HOMEW | EP_APPS2;
    ec_wait = on && EP.cards < 5;
    /* the lock */
    hold_t *q = &Q[1];
    q->lit = on;
    q->facelit.hidden = q->iconlit.hidden = !on;
    q->face.hidden = q->icon.hidden = on;
    vc_close_all(1);
    scroll = 0;
    sp_scroll.live = 0;
    scroll_momentum = 0;
    if (on) free_vendor_cards();
    empill.hidden = !on;
    if (EP.lw == 3 && EP.lwkey == ep_lwkey()) ep_lw_swap();
    else { EP.lw = 0; EP.post |= EP_LW; }
    EP.fresh = !instant;                             /* this frame did the switch: no more work in it */
    mark_t = on || lockY < 0 ? 0 : 1;
    /* the Control Centre: its tile now; its scrim (home as it was) made again at home's next idle turn */
    ccem.hidden = !on || !ccem.bmp.px;
    cc_stale = 1;
    if (instant) {
        ep_finish();
        em_t = 1;
        em_step(1);
        mark_a = mark_t;
    } else if (ep_ground_done()) {
        ep_handover();                               /* prepared: the fade starts with the next frame */
        EP.ground = 2;
    }
    relink();
    place_list();
    lock_ver++;
    toast(on ? "Emergency mode on" : "Emergency mode off");
}

/* ---- taps inside the apps -------------------------------------------------------------------------------- */
static int key_hit(float x, float y)
{
    for (int i = 0; i < 12; i++) {
        float cx = 52 + 104.0f * (i % 3) + 39, cy = 182 + 96.0f * (i / 3) + 39, dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy <= 39 * 39) return i;
    }
    return -1;
}
static void key_down(int i)
{
    /* :active: rgba(255, 255, 255, .34) instead of .14: a white circle at (.34 - .14) / (1 - .14) over it */
    if (!apkey.bmp.px) {
        dc d = mk(&apkey, "key-down", 52, 182, 78, 78, 1, 0);
        d_rrect(&d, 52, 182, 78, 78, 39, W(.2f / .86f));
        relink();
    }
    apkey.hidden = i < 0;
    if (i >= 0) {
        apkey.x = apkey.bx + PT(104.0f * (i % 3));
        apkey.y = apkey.by + PT(96.0f * (i / 3));
    }
}
static int is_app(const char *k) { return AP.on && !tk_strcmp(APPS[AP.a].k, k); }
static void app_tap(float x, float y)
{
    if (lx_tap(x, y)) return;                    /* L3: a Linux app's keyboard, the App Store */
    if (sv_tap(x, y)) return;                                   /* survival-next: the tools, Settings > Medical Card */
    if (cm_app_tap(x, y)) return;                             /* helper I: Phone and Messages (shell_comms.inc) */
    if (is_app("clock")) {
        clk_tap(x, y);
    } else if (is_app("calc")) {
        calc_tap(x, y);
    } else if (is_app("camera")) {
        camera_tap(x, y);
    } else if (is_app("notes")) {
        notes_tap(x, y);
    } else if (is_app("search")) {
        search_tap(x, y);
    } else if (is_app("light")) {
        if (x >= 135 && x < 255 && y >= 250 && y < 490) {
            Q[0].lit = !Q[0].lit;
            Q[0].facelit.hidden = Q[0].iconlit.hidden = !Q[0].lit;
            Q[0].face.hidden = Q[0].icon.hidden = Q[0].lit;
            sh_torch(Q[0].lit);
            lock_ver++;
            build_app_page(AP.a);
        }
    } else if (is_app("settings") && AS.sub == 2) {
        wifi_tap(x, y);
    } else if (is_app("settings") && AS.sub == 3) {
        cell_tap(x, y);
    } else if (is_app("settings") && AS.sub == SE_TZ_PAGE) {
        (void)ta_tap(x, y);                       /* Track A: the zone list (its back: shell_settings.inc's Date & Time) */
    }
#ifdef SH_HAS_STARTUP
    else if (is_app("settings") && AS.sub == SE_STARTUP_PAGE) (void)startup_tap(x, y);      /* Track J's page */
#endif
#ifdef SH_HAS_PASSCODE
    else if (is_app("settings") && AS.sub == SE_PASSCODE_PAGE) (void)passcode_tap(x, y);    /* Track A's page */
#endif
    else if (is_app("settings") && AS.sub == SU_SUB) (void)su_app_tap(x, y);                 /* Track A update's page */
    /* helper B2: the other Settings pages are shell_settings.inc's (their own gesture, G_SET) */
}

/* ---- the modem's status (docs/MODEM.md 7.7's snapshot): the status bar's bars and RAT, the keypad's line ----- */
static void build_cc_grid(void);
static void cc_link(void);
static sh_cell_t CELL;
static int cell_running(void) { return CELL.state == SH_CELL_ON || CELL.state == SH_CELL_STARTING || CELL.state == SH_CELL_RESTARTING; }
static int data_on(void);         /* shell_cell.inc */
static char md_buf[48];
static const char *md_join(const char *a, const char *b)
{
    int n = 0;
    for (const char *p = a; *p && n < 47; p++) md_buf[n++] = *p;
    for (const char *p = b; *p && n < 47; p++) md_buf[n++] = *p;
    md_buf[n] = 0;
    return md_buf;
}
/* Settings > Cellular: the modem's state in words (the contract's wording) */
static const char *md_text(void)
{
    switch (CELL.data) {
    case SH_DATA_STARTING: return "Starting\xe2\x80\xa6";
    case SH_DATA_SEARCHING: return "Searching\xe2\x80\xa6";
    case SH_DATA_REGISTERED: return CELL.oper[0] ? md_join(CELL.oper, " \xc2\xb7 connecting") : "Connecting\xe2\x80\xa6";
    case SH_DATA_UP: return md_join(CELL.oper[0] ? CELL.oper : "On", CELL.rat == 7 ? " \xc2\xb7 LTE" : CELL.rat == 4 ? " \xc2\xb7 3G" :
                                    CELL.rat ? " \xc2\xb7 2G" : "");
    case SH_DATA_STOPPING: return "Turning off\xe2\x80\xa6";
    case SH_DATA_NO_SIM: return "No SIM";
    case SH_DATA_LOCKED: return "SIM PIN on";
    case SH_DATA_FAILED: return CELL.why[0] ? CELL.why : "Could not connect";
    default: break;
    }
    switch (CELL.state) {
    case SH_CELL_STARTING: return "Starting\xe2\x80\xa6";
    case SH_CELL_RESTARTING: return "Restarting\xe2\x80\xa6";
    case SH_CELL_GUARD: return "Stopped: a SIM or a registration";
    case SH_CELL_FAILED: return "Could not start";
    case SH_CELL_ON: return CELL.emergency_only ? "Emergency calls only" : "No Service";
    default: return "Off";
    }
}
static void build_home_sb(void);
void sh_cell(const sh_cell_t *c)
{
    if (c->state == CELL.state && c->sim == CELL.sim && c->emergency_only == CELL.emergency_only && c->rat == CELL.rat &&
        c->bars == CELL.bars && c->data == CELL.data && !tk_strcmp(c->oper, CELL.oper) && !tk_strcmp(c->why, CELL.why))
        return;
    CELL = *c;
    /* the lock's status bar: the network's honest line (the operator's name once registered) */
    const char *cr = (CELL.data == SH_DATA_REGISTERED || CELL.data == SH_DATA_UP) && CELL.oper[0] ? CELL.oper :
                     CELL.data == SH_DATA_SEARCHING ? "Searching\xe2\x80\xa6" : CELL.data == SH_DATA_LOCKED ? "SIM PIN on" :
                     CELL.state == SH_CELL_STARTING ? "Starting\xe2\x80\xa6" : CELL.state == SH_CELL_RESTARTING ? "Restarting\xe2\x80\xa6" :
                     CELL.data == SH_DATA_NO_SIM ? "No SIM" :
                     CELL.state == SH_CELL_ON && CELL.emergency_only ? "Emergency calls only" :
                     CELL.state == SH_CELL_ON ? "No Service" : "";   /* P3z: Cellular off: nothing (the user saw "No SIM" with a SIM in) */
    sh_carrier(cr);                     /* the lock screen's left (and the Control Centre's): the operator */
    SB_CELL.state = CELL.state == SH_CELL_ON;
    SB_CELL.bars = CELL.bars;
    SB_CELL.rat = CELL.rat;
    const char *h = CELL.data == SH_DATA_UP ? md_join(CELL.oper[0] ? CELL.oper : "Mobile", " \xc2\xb7 cellular data on") :
                    data_on() ? "Cellular data \xc2\xb7 connecting" :
                    CELL.data == SH_DATA_NO_SIM ? "No SIM \xc2\xb7 emergency calls only" :
                    CELL.state == SH_CELL_ON && !CELL.emergency_only && CELL.bars <= 0 ? "Searching\xe2\x80\xa6" :
                    CELL.state == SH_CELL_ON ? "Emergency calls only" : "Cellular off";
    int n = 0;
    while (h[n] && n < 47) { cell_hint[n] = h[n]; n++; }
    cell_hint[n] = 0;
    build_lstatus();
    build_home_sb();
    if (is_app("phone") || is_app("messages") || is_app("settings") || is_app("maps")) build_app_page(AP.a);     /* (it rebuilds the app's status bar too) */
    else if (AP.on && !AP.closing) build_apsb();     /* P3l: every app's status bar shows the bars */
    relayer(&ccsb);                                  /* P3l: the CC's status bar again at its next build */
    cc_stale = 1;
    if (CCS.built) { build_cc_grid(); cc_link(); }
    relink();
    lock_ver++;
    isl_sync();                                      /* B7: mobile data starting: the blue capsule (drawn in this push's frame) */
}

#include "shell_kit.inc"                             /* helper B2: alerts, action sheets, sheets, switches, sliders, lists */
static int pc_gate_launch(unsigned id);             /* Track A: shell_passcode.inc (shell_nc.inc asks it) */
#include "shell_wifi.inc"
#include "shell_cell.inc"
#include "shell_media_img.inc"
#include "shell_media.inc"
/* Track E's v3 hooks (starbite/guest/next/INTEGRATION-NEXT.md "v3 hooks"): the switcher's external cards
 * (shell_switcher.inc's sw_ext_add / sw_ext_remove), B3's key bar (shell_kb.inc's kb_accessory / kb_accessory_tap in
 * its 40 pt strip, 500..540) and the Centre's "linux:<id>" items (shell_nc.inc's nc_withdraw, lx_nc_*) */
#define SH_EXT_API 1
#define KB_ACC_H KB_STRIP
#include "shell_linux.inc"
#include "shell_survival.inc"
#include "shell_nc.inc"                             /* helper D: notifications, the Notification Centre, the banner */
#include "shell_lknote.inc"                         /* B9: the lock's notifications (the stack above the buttons) */
#include "shell_switcher.inc"                       /* helper D: the app switcher */
#include "shell_camera.inc"                         /* the Camera on Track A's front-camera driver */
#include "shell_apps.inc"                           /* helper B5: the lists (SCV), the avatars, the dock's badges */
#include "shell_comms.inc"                          /* helper I: Phone and Messages, the call screen */
#include "shell_power.inc"                          /* P3x: Power held: slide to power off, Restart, Cancel */
#include "shell_settings.inc"                       /* helper B2: Settings */
#ifdef SH_HAS_STARTUP
#include "shell_startup.inc"                        /* Track J: Settings > Startup */
#endif
#include "shell_passcode.inc"                       /* Track A: the passcode screen, Settings > Passcode, the lock's lifecycle */
#include "shell_time.inc"                           /* Track A: the alarms API's Clock hooks, Settings > Date & Time's zone list */
#include "shell_update.inc"                         /* Track A update: Settings > General > Software Update */
/* B2's Settings hooks onto Track A's passcode page (AS.sub SE_PASSCODE_PAGE = shell_passcode.inc's PC_SUB) */
static void page_passcode(dc *d) { pc_page(d); }
static int passcode_tap(float x, float y) { return pc_app_tap(x, y); }

/* ---- Track A: what the shell keeps across restarts (the platform's prefs and blobs), taken back once the store is
 * ready. Cellular data is never kept (off at every boot by design); Wi-Fi comes back on when the user left it on. -- */
void sh_restore(void)
{
    static int done;
    if (done) return;
    done = 1;
    notes_load();
    nc_restore();
    cm_rec_restore();
    sw_restore();
    if (sh_pref_int("cc.wifi", 0) == 1 && !sh_safe_mode()) (void)sh_wifi_power(1);   /* a refusal leaves it off; the choice stays; not in a safe start (Track A) */
}
#include "shell_island.inc"                         /* B7: the status capsule, the live pill (toasts) */

/* ---- the unlock -------------------------------------------------------------------------------------------- */
static void set_lock(float y)
{
    lockY = y;
    float p = tk_clampf(-y / H_PT, 0, 1);
    /* P3p (P3j-9): the home settles from a little larger (1.06) and comes up from .35 under the sliding cover */
    home.alpha = y < 0 ? tk_minf(1, .35f + p * 1.3f) : 0;
    home.sx = home.sy = 1.06f - .06f * tk_minf(1, p * 1.3f);
    mark_t = p > .02f || EMG ? 0 : 1;
    glyph_t = p > .02f && !EMG ? 1 : 0;
    glyph_open = p > .06f;
    if (y == 0) {
        lockg.hidden = 0;
        lockflat.hidden = 1;
    } else {
        flatten_lock(0);
        lockg.hidden = 1;
        lockflat.hidden = 0;
        lockflat.y = PT(y) / 2;           /* the layer's offset is scaled with it */
        lockflat.alpha = 1;               /* P3p: the cover slides off whole (no fade) */
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

/* the tree, rebuilt from what exists (a layer re-made by mk() loses its links): called after every bake step */
static void link(tk_layer *parent, tk_layer **kids, int n)
{
    parent->child = 0;
    tk_layer *last = 0;
    for (int i = 0; i < n; i++) {
        tk_layer *k = kids[i];
        k->next = 0;
        if (last) last->next = k; else parent->child = k;
        last = k;
    }
}
static void relink(void)
{
    if (!apdim.name) { tk_layer_init(&apdim, "app-dim"); apdim.hidden = 1; }
    tk_layer *r[] = { &star, &star_e, &home, &swg, &lockg, &lockflat, &apdim, &appg, &apicon, &swcover, &ncl, &focus, &ccl, &bnl, &cfl, &kitl, &cvl,
                      &boot, &homebar, &pwl, &toastl, &slp };   /* P3x: pwl the power sheet */
    nc_link();                                       /* helper D: the Notification Centre (ncl), the banner (bnl) */
    sw_link();                                       /* helper D: the switcher's cards (swg), a card's snapshot over its opening window */
    cm_link();                                       /* helper I: the call screen (cvl) */
    kit_link();                                      /* helper B2: an alert, an action sheet, a sheet (kitl) */
    pw_link();                                       /* P3x: the power sheet (pwl) */
    link(&root, r, 22);
    pc_link();                                       /* Track A: the passcode screen (under the call screen) */
    isl_link();                                      /* B7: the capsule, the pill and its words */
    tk_layer *ag[] = { &appbg, &appc, &apkey, &apnum, &apdel, &apsw, &aptm, &apctl, &apcalc, &apnote, &apsrch, &emsel, &kbg, &emp, &apsb, &apphl };
    link(&appg, ag, 16);                             /* B3: emsel the edit menu's selection, emp its pill */
    kb_link();                                       /* B3: the keyboard's panel, keys, lit key and balloon */
    tk_layer *st[NST + 64] = { &appst[0], &appst[1], &appst[2], &appst[3] };
    int na = NST + med_layers(st + NST);
    na += lx_layers(st + na);                        /* L3: shell_linux.inc's */
    na += se_layers(st + na);                        /* helper B2: Settings' page (scrolled, clipped) and its nav row */
    na += cam_layers(st + na);                       /* the Camera's preview, shutter, thumbnail, flash (shell_camera.inc) */
    link(&appc, st, na + cm_layers(st + na));    /* helper I: the keypad's line and ring, Messages' fields */
    /* B9: the notifications' stack is the lock's (lkng, over the list: fanned out it takes the list's room); the
     * Centre's "N Notifications" pill only in Emergency */
    tk_layer *lk[] = { &lstatus, &lclock, &lwidgets, &mark, &glyph_c, &glyph_o, &empill, &list, &lkng, &Q[0].grp, &Q[1].grp, &hint, &ncpill };
    link(&lockg, lk, EMG ? 13 : 12);
    lkn_link();
    /* B10: the vendor cards in the list as B4 had them: the lower cards, the members' platters and words under the
     * head card, "Show less" above it, the chevron on top */
    tk_layer *ls[10 * NG + 2 + NEC];
    int n = 0;
    for (int g = 0; g < NG; g++) {
        vcard_t *k = &V[g];
        ls[n++] = &k->gh;
        for (int i = 2; i >= 0; i--) ls[n++] = &k->mpl[i];
        k->members.child = 0;
        for (int i = 0; i < G[g].n; i++) if (k->rows[i].bmp.px) tk_layer_add(&k->members, &k->rows[i]);
        ls[n++] = &k->members; ls[n++] = &k->sl;
        ls[n++] = &k->face; ls[n++] = &k->head; ls[n++] = &k->chev;
    }
    ls[n++] = &etool;
    for (int i = 0; i < NEC; i++) ls[n++] = &ecard[i];
    ls[n++] = &lockhl;
    link(&list, ls, n);
    for (int i = 0; i < 2; i++) {
        tk_layer *q[] = { &Q[i].face, &Q[i].facelit, &Q[i].icon, &Q[i].iconlit, &Q[i].ring };
        link(&Q[i].grp, q, 5);
    }
    tk_layer *pg[] = { &hparts[1], &hparts[2], &hparts[4], &hparts[5], &hparts[6], &hparts[7] };   /* [5] the Linux page (3); [6] page 2's third row;
                                                                                                * [7] page 1's Clock widget (helper D) */
    link(&pages, pg, 6);
    tk_layer *h[] = { &hparts[0], &pages, &hsearch[0], &hsearch[1], &hsearch[2], &hparts[3], &icobg, &icofg, &hbadge };
    link(&home, h, 9);
    sv_relink();                                     /* survival-next: the tools' live layers, SOS's beacon */
}

/* the bake in steps (the kernel runs one per loop turn: the watchdog is fed and the screen fills in as it goes);
 * returns 1 while steps remain */
static int init_step;
void sh_init_begin(tk_comp *c)
{
    C = c;
    tb = tk_us();
    init_step = 0;
    tk_layer_init(&root, "root");
    tk_layer_init(&home, "home");
    home.ox = PT(195);
    home.oy = PT(401);
    tk_layer_init(&pages, "pages");
    tk_layer_init(&ccl, "cc");
    ccl.hidden = 1;
    tk_layer_init(&appg, "app");
    appg.hidden = 1;
    tk_layer_init(&appbg, "app-ground");
    appbg.solid = sbc_argb(SBC_GROUND);
    appbg.solid_w = TK_W;
    appbg.solid_h = TK_H;
    appbg.opaque = 1;
    tk_layer_init(&appc, "app-page");
    for (int i = 0; i < NST; i++) tk_layer_init(&appst[i], "app-strip");
    tk_layer_init(&apkey, "key-down");
    apkey.hidden = 1;
    tk_layer_init(&apnum, "dial");
    apnum.hidden = 1;
    tk_layer_init(&apicon, "app-icon");
    apicon.hidden = 1;
    isl_init();                                    /* B7: the island's group (toastl) */
    tk_layer_init(&cfl, "confirm");
    cfl.hidden = 1;
    tk_layer_init(&slp, "sleep");                  /* black over everything, its opacity the fade */
    slp.solid = 0xff000000u;
    slp.solid_w = TK_W;
    slp.solid_h = TK_H;
    slp.opaque = 1;
    slp.hidden = 1;
    tk_layer_init(&lockg, "lock");
    lockg.ox = PT(195);                            /* the wake grows it about the middle */
    lockg.oy = PT(401);
    tk_layer_init(&list, "list");
    list.clip = 1;
    list.fade_top = PT(18);
    list.fade_bot = PT(44);                    /* P3i (audit 16): no sliced lip above the hold buttons */
    tk_layer_init(&lockflat, "lock-flat");
    tk_layer_init(&focus, "focus");
    focus.hidden = 1;
    tk_layer_init(&boot, "boot");
    boot.hidden = 1;
    for (int i = 0; i < 2; i++) tk_layer_init(&Q[i].grp, i ? "q-em" : "q-light");
    for (int i = 0; i < NEC; i++) { tk_layer_init(&ecard[i], "ecard"); ecard[i].hidden = 1; }
    tk_layer_init(&hparts[6], "home-apps2e");
    hparts[6].hidden = 1;
    tk_layer_init(&hbadge, "home-badges"); hbadge.hidden = 1;     /* B8: the dock's badges (home_badges) */
    tk_layer_init(&empill, "empill");
    empill.hidden = 1;
    tk_layer_init(&icobg, "icon-ground"); icobg.hidden = 1;     /* P3p: press feedback */
    tk_layer_init(&icofg, "icon-pressed"); icofg.hidden = 1;
    tk_layer_init(&lockhl, "press"); lockhl.hidden = 1;
    tk_layer_init(&apphl, "press"); apphl.hidden = 1;
    tk_layer_init(&kbg, "keyboard"); kbg.hidden = 1;        /* B3 */
    tk_layer_init(&emp, "edit-menu"); emp.hidden = 1;
    tk_layer_init(&emsel, "edit-selection"); emsel.hidden = 1;
    for (int g = 0; g < NG; g++) { tk_layer_init(&K[g].face, "face"); K[g].face.hidden = 1; tk_layer_init(&K[g].gh, "stack"); K[g].gh.hidden = 1; }
    /* B10: the vendor cards (their glass: views of K[0], placed by place_list) */
    for (int g = 0; g < NG; g++) {
        vcard_t *k = &V[g];
        tk_layer *l[] = { &k->gh, &k->face, &k->head, &k->chev, &k->members, &k->rows[0], &k->rows[1], &k->rows[2],
                          &k->mpl[0], &k->mpl[1], &k->mpl[2], &k->sl };
        static const char *const nm[] = { "stack", "card", "head", "chev", "members", "row", "row", "row", "member", "member", "member", "show-less" };
        for (int i = 0; i < 12; i++) { tk_layer_init(l[i], nm[i]); l[i]->hidden = i != 4; }
        k->chev_t = 1;
    }
    tk_layer_init(&etool, "etool"); etool.hidden = 1;
    layout_list();
    lkn_init();
    relink();
    c->root = &root;
}

int sh_init_step(void)
{
    switch (init_step++) {
    case 0: build_wall(); lap("bake: wallpaper + glass backdrop"); break;
    case 1:
        build_lstatus();
        build_lclock();
        build_lwidgets();
        build_marks();
        glyph_c.alpha = glyph_o.alpha = 0;
        hint.alpha = 0;
        lap("bake: lock status, clock, widgets");
        break;
    case 2: build_platter(); lap("bake: the platter glass"); break;
    case 3: case 4: case 5: case 6: build_vcard(init_step - 4); lap("bake: a card's words"); break;   /* B10 */
    case 7: build_vcard(4); build_etool(); build_hold(0); build_hold(1); build_empill(); lap("bake: tools and holds"); break;
    case 8: build_home(); back_bake(); lap("bake: home"); break;
    case 9: {
        /* the flattened lock is only ever seen moving (half-resolution frames): kept at half resolution, drawn 2x */
        tk_bmp_new(&lockflat.bmp, TK_W / 2, TK_H / 2);
        lockflat.sx = lockflat.sy = 2;
        lockflat.opaque = 1;
        lockflat.hidden = 1;
        break;
    }
    default:
        relink();
        place_list();
        set_lock(0);
        return 0;
    }
    relink();
    place_list();
    return 1;
}

int sh_init(tk_comp *c)
{
    sh_init_begin(c);
    while (sh_init_step())
        ;
    return 1;
}

static void apply_tweens(void)
{
    /* B4 (audit "Easing on fades"): the linear tweens through iOS's ease */
    mark.alpha = tk_bezier(.25f, .1f, .25f, 1, mark_a);
    float g = tk_bezier(.25f, .1f, .25f, 1, glyph_a);
    glyph_c.alpha = glyph_open ? 0 : g;
    glyph_o.alpha = glyph_open ? g : 0;
    hint.alpha = tk_bezier(.25f, .1f, .25f, 1, hint_a);
}

/* back to the plain lock screen: the sheet and the boot overlay gone, every card closed, the top (exit_em: and out
 * of Emergency mode; the Power key's sleep keeps it) */
static void back_to_lock(int exit_em)
{
    focus_on = 0; focus_closing = 0; focus.hidden = 1; sp_focus.live = sp_thumb.live = 0;
    free_focus();
    booting = 0; boot.hidden = 1;
    if (boot.solid) { relayer(&bootc); relayer(&bootbar); boot.solid = 0; boot.child = 0; }   /* P3s: made again for the next */
    ico_free(); hl_off();                          /* P3p */
    vc_close_all(1);
    lkn_collapse(1);
    scroll = 0; sp_scroll.live = 0; scroll_momentum = 0;
    sp_lock.live = 0;
    sp_page.live = 0; page_dragging = 0;
    set_page(0);
    CCS.on = 0; sp_cc.live = 0; cc_x_reset(); set_cc(0);
    pc_lock_note(exit_em);                         /* Track A lifecycle: the open app comes back after the unlock */
    if (AP.on) { sp_app.live = 0; app_finish_close(); }
    if (exit_em && EMG) set_emergency(0, 1);
    if (exit_em) ep_finish();                      /* B6: a switch's rest at once (a prepared one goes) */
    if (!EMG) while (cards_pending > 0) { build_lockpart(LOCK_PARTS - cards_pending); cards_pending--; }
    if (CF.on) cf_close(0);
    pw_reset();                                    /* P3x: the power sheet goes */
    kit_reset();                                   /* helper B2: an alert or a sheet goes, unanswered */
    nc_reset();                                    /* helper D: the Notification Centre and the banner gone */
    sw_reset();                                    /* helper D: the switcher (the recents and their snapshots stay) */
    free_cc_all();                                 /* P3s: the lock keeps none of it */
    relink();
    isl_toast_hide();                              /* B7: the pill gone */
    locked = 1;
    set_lock(0);
    mark_a = mark_t; glyph_a = glyph_t; hint_a = 0; hint_left = 0;
    apply_tweens();
    place_list();
}
void sh_sleep(int now);
void sh_wake(void);

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
            if (V[g].isopen != want) toggle_group(g, 1);
        }
    } else if (!tk_strcmp(what, "scroll")) {
        scroll = v;
        place_list();
    } else if (!tk_strcmp(what, "focus")) {
        for (int g = 0; g < NG; g++)
            for (int i = 0; i < G[g].n; i++)
                if (!tk_strcmp(G[g].it[i].k, key)) {
                    focus_on = 1;
                    uint64_t t0 = tk_us();
                    build_focus(&G[g].it[i], &G[g]);
                    lap_us("focus: snapshot and bake", tk_us() - t0);
                    set_focus(v);
                }
    } else if (!tk_strcmp(what, "reset")) {
        cm_reset();                                /* helper I: a film's start: no call screen */
        isl_reset();                               /* B7 */
        back_to_lock(1);
    } else if (!tk_strcmp(what, "sleep")) {        /* the films: the Power key's fade (v 1) and the wake (v 0) */
        if (v > 0) sh_sleep(0); else sh_wake();
    } else if (pw_set(what, key, v)) {            /* P3x: power=:V (the power sheet, its thumb at V; -1 closed) */
    } else if (lx_set(what, key, v)) {            /* L3: lapp=; Track F: store=, storecat=, storeq=, storeapp=, ... */
    } else if (lkn_set(what, key, v)) {           /* B9: lkn= (the lock's notifications fanned out) */
    } else if (nc_set(what, key, v)) {            /* helper D: notify=, banner=, nc=, ncgroup=, ncswipe= (shell_nc.inc) */
        if (LK.sig != lkn_sig()) lkn_sync(0);     /* B9: the lock's stack at once (a still has no idle turn) */
    } else if (sw_set(what, key, v)) {            /* helper D: switcher=, swsnap= (shell_switcher.inc) */
    } else if (cm_set(what, key, v)) {            /* helper I: thread=, compose=, to=, dial=, recent=, callpad=, ... */
    } else if (pc_set(what, key, v)) {            /* Track A: passcode=unlock:N, wrong:P, set, off */
    } else if (ta_set(what, key, v)) {            /* Track A: clock24=, tz=, sub=datetime / tz, clocktab=, alarmedit= */
    } else if (su_set(what, key, v)) {            /* Track A update: update=open, stage:N, ready, install */
    } else if (cam_set(what, key, v)) {           /* the Camera: cam=:0 (a frame in), shot, review, flash:V */
    } else if (!tk_strcmp(what, "app")) {
        if (!key[0] && AP.on) app_finish_close();       /* L2: "app=" closes the open app at once */
        for (int a = 0; a < NAPPS; a++)
            if (!tk_strcmp(APPS[a].k, key)) {
                app_open(a);
                while (ap_pending) app_build_step();      /* a set state is whole at once */
                sp_app.live = 0;
                set_app_rect(lerpr(AP.icon, FULLR, v));
                if (v >= 1) appg.clip = 0;
            }
    } else if (!tk_strcmp(what, "emergency")) {
        set_emergency(v > 0, 1);
        while (sv_idle_pending()) sv_idle();         /* survival-next: a set state is whole at once */
        place_list();
        isl_settle();                                /* B7: its toast whole */
    } else if (!tk_strcmp(what, "cell")) {
        /* the modem's state for the host's stills: "on" (emergency-only, GSM, v bars), "starting", "off" */
        sh_cell_t c = { SH_CELL_OFF, -1, 0, 0, -1, SH_DATA_OFF, "", "" };
        if (!tk_strcmp(key, "on")) { c.state = SH_CELL_ON; c.sim = 0x80; c.emergency_only = 1; c.rat = 1; c.bars = (int)v; }
        else if (!tk_strcmp(key, "starting")) c.state = SH_CELL_STARTING;
        sh_cell(&c);
    } else if (!tk_strcmp(what, "tab")) {
        static const char *const TK[4] = { "fav", "rec", "con", "pad" };
        for (int t = 0; t < 4; t++) if (!tk_strcmp(TK[t], key)) { AS.tab = t; build_app_page(AP.a); }
    } else if (!tk_strcmp(what, "sub")) {
        /* the stills: a Settings page (display, wifi, cell, general, about, ...: se_sub_key; nothing is switched on) */
        AS.sub = se_sub_key(key);
        if (AP.on) build_app_page(AP.a);
    } else if (!tk_strcmp(what, "kit")) {
        kit_set(key, v);                             /* helper B2: kit=alert / alert3 / actions / sheet / sheetl */
    } else if (se_set(what, key, v)) {                /* helper B2: sescroll=, sehist= */
    } else if (!tk_strcmp(what, "note")) {
        /* the stills: a note with this text open in the editor (v 0), or back to the list (v 1) */
        if (key[0] && NS.n < NNOTES) {
            int n = 0;
            while (key[n] && n < NOTELEN - 1) { NOTES[NS.n][n] = key[n]; n++; }
            NOTES[NS.n][n] = 0;
            NS.cur = NS.n++;
            notes_save();                              /* Track A: kept (the host's "after a restart" stills) */
        }
        NS.view = v < 1;
        if (AP.on) build_app_page(AP.a);
    } else if (!tk_strcmp(what, "toast")) {
        toast(key);
        isl_settle();                                /* B7: the pill open with it */
    } else if (!tk_strcmp(what, "island")) {
        isl_set(key, v);                             /* B7: shell_island.inc */
    } else if (!tk_strcmp(what, "cc")) {
        if (!CCS.built || cc_stale) { build_cc(); cc_stale = 0; }
        sp_cc.live = 0;
        CCS.on = v > 0;
        CCS.closing = 0;
        set_cc(v);
    } else if (!tk_strcmp(what, "ccx")) {
        /* B4, the stills: the open Control Centre's module v expanded (0 connectivity, 1 Brightness, 2 Volume), at rest */
        if (CCS.built && CCS.on) { cc_x_open((int)v); while (CX.todo < 6) cc_x_link_step(); CX.sp.live = 0; CX.e = 1; set_cc(CCS.p); }
    } else if (!tk_strcmp(what, "page")) {
        sp_page.live = 0;
        set_page(-390 * v);
    } else if (!tk_strcmp(what, "hint")) {
        hint_a = v;
        apply_tweens();
    } else if (what[0] == 'm' && med_set(what, key, v)) {
    } else if (!tk_strcmp(what, "sv")) {
        sv_set(key, v);                              /* survival-next: the tools' states (shell_survival.inc) */
    } else if (!tk_strcmp(what, "hold")) {
        int i = key && key[0] == 'e';
        Q[i].grp.sx = Q[i].grp.sy = v > 0 ? 1.22f : 1;
        hold_ring(&Q[i], i, v);
        lock_ver++;
    }
}

/* B3: the text field under a long press (Notes' text, Search's field, Messages' message): 1 and *o */
static int cm_em_target(float x, float y, emt *o);
static int em_target(float x, float y, emt *o)
{
    if (is_app("notes") && NS.view && y >= PUSHED_TOP - 12 && y < KB_TOP - KB_STRIP) {
        *o = (emt){ NOTES[NS.cur], NOTELEN - 1, { 18, PUSHED_TOP + 1, note_bot - PUSHED_TOP > 24 ? 354 : note_w + 4, note_bot - PUSHED_TOP - 2 }, build_note };
        return 1;
    }
    if (is_app("search") && y >= 52 && y < 100 && x < 306) {
        *o = (emt){ SR.q, (int)sizeof SR.q - 2, { 51, 63, tw(TK_IT400, 17, SR.q, 0, 0) + 4, 24 }, build_search };
        return 1;
    }
    return cm_em_target(x, y, o);
}

/* ---- gestures ---------------------------------------------------------------------------------------------- */
enum { G_NONE, G_LOCK, G_LIST, G_TAP_HEAD, G_TAP_ROW, G_HOLD, G_THUMB, G_SCRIM, G_RELOCK, G_PAGE, G_CC, G_CCTAP, G_CCSL, G_APP, G_APPTAP, G_DOCK, G_ETAP, G_CCEM, G_CF, G_CCCELL,
       G_CCDRAG, G_FOCUSDRAG, G_MEDIA, G_LINUX, G_NC, G_SW, G_KB, G_CCX, G_SET, G_LKN };   /* B3: G_KB the keyboard's (shell_kb.inc); B4: G_CCX the Control Centre's expanded module; helper B2: G_SET Settings' own pages; B9: G_LKN the lock's notifications (shell_lknote.inc) */   /* helper D: G_NC shell_nc.inc's (the Centre, the banner, the
                                                     * lock's pill), G_SW shell_switcher.inc's (the cards) */
static struct {
    int kind, arg, arg2, moved;
    float x0, y0, base, t0;
    tk_tracker tr;
} T;

static float rubber(float over, float dim) { return (1 - 1 / (over * .55f / dim + 1)) * dim; }
static int list_over;                       /* B4: the lock list's drag is past an end (its haptic once) */

/* ---- P3p (P3j-7): press feedback. Nothing is baked in a tap: an icon's press is two small layers (the ground
 * under it: the wallpaper or the dock's kept glass, and the icon dimmed (.2 black) and shrinking to .96 in .1 s and back in .2 s; B8, GAP #21: iOS dims slightly, hardly shrinks);
 * a row, card or key is a solid rounded rect with no bitmap (W .10 / lighter on keys), clipped to its group. Both
 * go on a move and at lift-off. --------------------------------------------------------------------------------- */
/* icobg, icofg, lockhl, apphl: declared with the home's layers */
static struct { int on; float s, from, to, t, dur; } IP = { 0, 1, 1, 1, 1, .1f };
static void ico_free(void)
{
    relayer(&icobg); relayer(&icofg);
    icobg.hidden = icofg.hidden = 1;
    IP.on = 0; IP.t = 1; IP.s = 1;
}
static void icon_press(int a)
{
    ico_free();
    if (a < 0 || !tk_strcmp(APPS[a].k, "search")) return;
    float pad;
    const tk_bmp *ic = app_icon(APPS[a].k, &pad);      /* (Calendar: today's composed tile) */
    if (!ic) return;
    arect R = icon_rect(a);
    int X = (int)tk_floorf(PT(R.x) - pad + .5f), Y = (int)tk_floorf(PT(R.y) - pad + .5f);
    tk_layer_init(&icobg, "icon-ground");
    tk_layer_init(&icofg, "icon-pressed");
    if (!tk_bmp_new(&icobg.bmp, ic->w, ic->h) || !tk_bmp_new(&icofg.bmp, ic->w, ic->h)) { ico_free(); return; }
    int dk = -1;
    for (int i = 0; i < 4 && R.y >= DOCK_Y - 1; i++) if (tk_fabsf(R.x - (COL_CX(i) - ICON_PT / 2)) < 1) dk = i;
    if (dk >= 0 && dockbg[dk].px) tk_bmp_copy(&icobg.bmp, 0, 0, &dockbg[dk], 0, 0, ic->w, ic->h);
    else tk_bmp_up(&icobg.bmp, &C->base_half, X, Y, 2);            /* the page's icons sit on the wallpaper */
    tk_blit(&icofg.bmp, ic, 0, 0, 1);
    tk_blit_tinted(&icofg.bmp, ic, 0, 0, TK_RGBA(0, 0, 0, .2f));   /* iOS: the icon darkens under the finger */
    icobg.x = icofg.x = (float)X;
    icobg.y = icofg.y = (float)Y;
    icofg.ox = PT(R.x + R.w / 2);
    icofg.oy = PT(R.y + R.h / 2);
    IP.on = 1; IP.from = 1; IP.to = .96f; IP.t = 0; IP.dur = .1f; IP.s = 1;
    icofg.sx = icofg.sy = 1;
    relink();
}
static void icon_release(void)
{
    if (!IP.on) return;
    IP.from = IP.s; IP.to = 1; IP.t = 0; IP.dur = .2f;
}
static int ico_step(float dt)
{
    if (!IP.on || IP.t >= 1) return 0;
    IP.t = tk_minf(1, IP.t + dt / IP.dur);
    IP.s = IP.from + (IP.to - IP.from) * tk_bezier(.25f, .1f, .25f, 1, IP.t);
    icofg.sx = icofg.sy = IP.s;
    if (IP.t >= 1 && IP.to >= 1) ico_free();
    return 1;
}
/* a solid highlight: r the lit rect, clip the group's (rounded by rad) in pt; l's parent space is the screen's
 * (lockhl's is the list's content: its y + scroll) */
static void hl_set(tk_layer *l, tk_rect r, tk_rect clip, float rad, tk_col c)
{
    l->solid = tk_premul(c);
    l->x = PT(r.x); l->y = PT(r.y);
    l->solid_w = PT(r.w); l->solid_h = PT(r.h);
    l->clip = 1;
    l->clip_r = (tk_rect){ PT(clip.x), PT(clip.y), PT(clip.w), PT(clip.h) };
    l->clip_radius = PT(rad);
    l->alpha = 1;
    l->hidden = 0;
    l->gen++;
}
static void hl_off(void)
{
    if (!lockhl.hidden || !apphl.hidden) { lockhl.hidden = apphl.hidden = 1; lockhl.gen++; apphl.gen++; lock_ver++; }
}
static void press_cancel(void) { hl_off(); if (IP.on) icon_release(); }

/* what lights under a finger on an open app's page */
static void app_press(float x, float y)
{
    tk_rect r = { 0, 0, 0, 0 }, g = r;
    float rad = 0;
    tk_col c = W(.1f);
    int ok = 0;
    int pushed = (is_app("settings") && AS.sub && !(AS.sub == 2 && WV.view == 1) && !(AS.sub == 4 && SV.cf >= 0)) ||
                 (is_app("messages") && AS.thread >= 0) || (is_app("notes") && NS.view);
    if (sv_press(x, y, &r, &g, &rad, &c)) ok = 1;                 /* survival-next (E's bands): Emergency's tools, Settings > Medical Card */
    else if (cm_press(x, y, &r, &g, &rad, &c)) ok = 1;             /* helper I: Recents, Messages */
    else if (ap_press(x, y, &r, &g, &rad, &c)) ok = 1;             /* helper B5: the Calculator's keys, the Clock's buttons */
    else if (pc_press(x, y, &r, &g, &rad, &c)) ok = 1;             /* Track A: Settings > Passcode */
    else if (su_press(x, y, &r, &g, &rad, &c)) ok = 1;             /* Track A update */
    else if (pushed && BACK_HIT(x, y)) {
        r = g = (tk_rect){ BACK_X, SAFE_TOP, BACK_D, BACK_D };   /* the glass back lights */
        rad = BACK_D / 2; c = W(.16f); ok = 1;
    } else if (is_app("notes") && NS.view && DONE_HIT(x, y)) {
        r = g = (tk_rect){ CONTENT_R - BACK_D, SAFE_TOP, BACK_D, BACK_D };
        rad = BACK_D / 2; c = W(.16f); ok = 1;
    } else if (is_app("notes") && !NS.view && COMPOSE_HIT(x, y)) {
        r = g = (tk_rect){ CONTENT_R - COMPOSE_D, COMPOSE_Y, COMPOSE_D, COMPOSE_D };
        rad = COMPOSE_D / 2; c = W(.16f); ok = 1;
    } else if (is_app("notes") && !NS.view && NS.n) {
        if (x >= GROUP_X && x < CONTENT_R && y >= CONTENT_TOP && y < CONTENT_TOP + NOTE_RH * NS.n) {
            int i = (int)((y - CONTENT_TOP) / NOTE_RH);
            r = (tk_rect){ GROUP_X, CONTENT_TOP + NOTE_RH * i, GROUP_W, NOTE_RH };
            g = (tk_rect){ GROUP_X, CONTENT_TOP, GROUP_W, NOTE_RH * NS.n };
            rad = 16; ok = 1;
        }
    } else if ((is_app("notes") && NS.view) || is_app("search")) {
        int num = is_app("notes") ? NS.num : SR.num;
        if (y >= 548 && kb_rect(x, y, KR[num], !num, &r)) { g = r; rad = 6; c = W(.35f); ok = 1; }
    } else if (is_app("settings") && AS.sub == 2 && WV.view == 1) {
        if (y >= 548 && kb_rect(x, y, 0, 0, &r)) { g = r; rad = 6; c = W(.35f); ok = 1; }
    }
    if (ok) hl_set(&apphl, r, g, rad, c);
}

/* helper B2: a note held for half a second asks iOS's alert (Delete Note? Cancel / Delete) */
static int note_del = -1;
static void notes_delete(int i)
{
    int k = note_del;
    note_del = -1;
    if (i != 1 || k < 0 || k >= NS.n) return;
    for (; k + 1 < NS.n; k++) { tk_memcpy(NOTES[k], NOTES[k + 1], NOTELEN); NOTE_HM[k] = NOTE_HM[k + 1]; }
    NS.n--;
    notes_save();                                  /* Track A: the deletion is kept */
    if (is_app("notes")) build_app_page(AP.a);
}
static int notes_hold(float t)
{
    if (T.kind != G_APPTAP || T.moved || !is_app("notes") || NS.view || kit_on() || AP.closing) return 0;
    float x = T.x0, y = T.y0;
    if (y < CONTENT_TOP || y >= CONTENT_TOP + NOTE_RH * NS.n || x < GROUP_X || x >= GROUP_X + GROUP_W) return 0;
    if (t - T.t0 < 500) return 1;                  /* frames on while it is held: the tick fires at 0.5 s */
    note_del = NS.n - 1 - (int)((y - CONTENT_TOP) / NOTE_RH);
    T.kind = G_NONE;
    hl_off();
    sh_haptic();                                   /* iOS: a medium tap at a long press */
    static char msg[200];
    char nt[136];
    note_title(NOTES[note_del], nt);
    int n = 0;
    const char *a = "\xe2\x80\x9c", *b = "\xe2\x80\x9d will be deleted.";
    for (const char *p = a; *p && n < 190; p++) msg[n++] = *p;
    for (int k = 0; nt[k] && k < 40 && n < 190; k++) msg[n++] = nt[k];
    for (const char *p = b; *p && n < 199; p++) msg[n++] = *p;
    msg[n] = 0;
    static const kit_btn B[2] = { { "Cancel", KIT_CANCEL }, { "Delete", KIT_DESTRUCTIVE } };
    kit_alert("Delete Note?", msg, B, 2, notes_delete);
    return 1;
}

void sh_touch(int phase, float x, float y, float t)
{
    now_ms = t;
    if (pw_touch(phase, x, y, t)) { T.kind = G_NONE; return; }   /* P3x: the power sheet, over everything (modal) */
    { int r = isl_touch(phase, x, y, t); if (r) { if (r == 1) T.kind = G_NONE; return; } }   /* B7: the capsule, the call's minimise */
    if (cm_touch(phase, x, y, t)) { T.kind = G_NONE; return; }   /* helper I: the call screen, the call button's hold */
    if (sv_touch(phase, x, y, t)) { T.kind = G_NONE; return; }   /* survival-next: SOS's beacon, a tool's hold */
    if (pc_touch(phase, x, y, t)) { T.kind = G_NONE; return; }   /* Track A: the passcode screen (over the kit's modals) */
    if (kit_touch(phase, x, y, t)) { if (T.kind == G_SET) se_touch(2, -1, -1, t); T.kind = G_NONE; hl_off(); return; }   /* helper B2: modal */
    if (ta_touch(phase, x, y, t)) { T.kind = G_NONE; return; }   /* Track A: the zone list's scroll */
    if (phase == 0) {
        tk_tracker_reset(&T.tr);
        T.moved = 0;
        T.x0 = x;
        T.y0 = y;
        T.t0 = t;
        T.kind = G_NONE;
        if (CF.on) {
            /* the confirmation: its hold bar, or outside the card (cancel) */
            T.kind = G_CF;
            if (x >= CF_BX && x < CF_BX + CF_BW && y >= CF.hy && y < CF.hy + 48) { CF.down = 1; CF.t0 = t; }
            return;
        }
        if (focus_on) {
            float sy = focus_sy;
            if (x >= SL_X + 5 + thumb_x && x <= SL_X + 57 + thumb_x && y >= sy + 5 && y <= sy + 57) { T.kind = G_THUMB; T.base = thumb_x; T.arg2 = 0; sp_thumb.live = 0; }
            else if (y < FOC_Y || y > FOC_Y + FOC_H) T.kind = G_SCRIM;
            else { T.kind = G_FOCUSDRAG; sp_focus.live = 0; focus_closing = 0; tk_tracker_add(&T.tr, y, t); }   /* P3p: drag it down */
            return;
        }
        if (AP.on && em_down(x, y)) { T.kind = G_NONE; return; }   /* B3: the edit menu's item (or it closes) */
        /* B9: the lock shows its notifications' stack (below), not the Centre's "N Notifications" pill (Emergency does) */
        int pill = ncpill.hidden;
        if (locked && !EMG) ncpill.hidden = 1;
        int ncd = nc_down(x, y, t);
        ncpill.hidden = pill;
        if (ncd) { T.kind = G_NC; return; }       /* helper D: the banner, the open Notification Centre, the pill */
        if (sw_down(x, y, t)) { T.kind = G_SW; return; }    /* helper D: the app switcher */
        if (AP.on) {
            /* an open app (survival-next: also over the lock, from Emergency's cards): the bottom bar's swipe up closes it (the rest of its page does nothing yet) */
            if (y < 36 && x < 195 && !AP.closing && !sp_app.live) { nc_pull_begin(x, y, t); T.kind = G_NC; return; }   /* helper D */
            if (y < H_PT - 30) sw_commit();          /* B4: the app is used: a quick switch's order is kept now */
            if (!AP.closing && !sp_app.live && kb_owns(x, y)) { T.kind = G_KB; kb_touch(0, x, y, t); return; }   /* B3 */
            if (y >= H_PT - 30 && !AP.closing) { T.kind = G_APP; T.arg2 = 0; sp_app.live = 0; AP.following = 1; tk_tracker_add(&T.tr, y, t); }
            else if (!sp_app.live && med_is(APPS[AP.a].k)) { T.kind = G_MEDIA; med_touch(0, x, y, t); }
            else if (!sp_app.live && !AP.closing && se_owns() && SE.built) { T.kind = G_SET; se_touch(0, x, y, t); }   /* helper B2 */
            else if (!sp_app.live && !AP.closing && lx_touch(0, x, y)) T.kind = G_LINUX;   /* L3 */
            else if (!sp_app.live) {
                /* a tap on the app's page (a key lights while the finger is on it) */
                T.kind = G_APPTAP;
                if (is_app("phone") && AS.tab == 3) key_down(key_hit(x, y));
                else app_press(x, y);                  /* P3p: rows, keys and buttons light */
            }
            return;
        }
        if (CCS.on && !CCS.closing) {
            /* the open Control Centre (B4: from the lock too): a slider, a tile, or outside it (closes it) */
            if (CX.m >= 0) {
                /* B4: the expanded module: its links (Mobile Data is a hold), the big slider (dragged from anywhere),
                 * or outside it (it folds back) */
                T.kind = G_CCX; T.arg = -1; T.arg2 = 0;
                if (CX.closing) { T.arg = -2; return; }
                if (CX.m == 0) { T.arg = cc_x_hit(x, y); if (T.arg == 2) { T.kind = G_CCCELL; T.arg = 0; return; } }
                else { T.arg2 = 1; T.base = CCS.lvl[CX.m - 1]; }
                tk_tracker_add(&T.tr, y, t);
                return;
            }
            int m = cc_mod_at(x, y);
            cc_press(m, 1);                      /* B4: the module grows under the finger (the glass spec's press) */
            if (m == 5) { T.kind = G_CCEM; T.arg = 0; return; }
            /* the cellular circle: a hold starts the modem (then the kernel's confirmation) or stops it */
            float cxl = CC_COL(0) + CN_PAD + CN_D + 14, cyl = CC_ROW(0) + CN_PAD;
            if (x >= cxl - 6 && x < cxl + CN_D + 6 && y >= cyl - 6 && y < cyl + CN_D + 6) { T.kind = G_CCCELL; T.arg = 0; return; }
            if (m == 1 || m == 2) {              /* B4: a slider moves with the finger from where it is (no jump) */
                T.kind = G_CCSL; T.arg = m - 1; T.base = CCS.lvl[m - 1];
                tk_tracker_add(&T.tr, y, t);
                return;
            }
            T.kind = G_CCTAP;
            tk_tracker_add(&T.tr, y, t);
            return;
        }
        if (!locked) {
            if (y < 56 && x >= 195) {           /* the top-right corner: the Control Centre */
                T.kind = G_CC;
                sp_cc.live = 0;
                if (!CCS.built) build_cc();
                tk_tracker_add(&T.tr, y, t);
                return;
            }
            /* helper D: a pull down from the top-left half: the Notification Centre (iOS; the concept's re-lock was here:
             * the Power key brings the lock back) */
            if (y < 56 && x < 195) { nc_pull_begin(x, y, t); T.kind = G_NC; return; }
            /* elsewhere but the dock, Search and the widgets: the pages (direction decided after 8 pt) */
            int on_w = page_x > -195 && y >= 60 && y <= 60 + WID_W && ((x >= WID_X(0) && x <= WID_X(0) + WID_W) || (x >= WID_X(1) && x <= WID_X(1) + WID_W));
            if (y >= DOCK_TOP && y < H_PT - 30) { T.kind = G_DOCK; icon_press(app_hit(x, y)); return; }   /* the dock: a tap opens its app */
            if (SRCH_HIT(x, y)) { T.kind = G_DOCK; return; }   /* P3g: Search */
            if (y >= 56 && y < DOCK_TOP && !SRCH_HIT(x, y) && !on_w) {
                T.kind = G_PAGE;
                T.base = page_x;
                page_dir = 0;
                sp_page.live = 0;
                tk_tracker_add(&T.tr, x, t);
                if (!sp_page.live && tk_fabsf(page_x - tk_floorf(page_x / 390 + .5f) * 390) < 1) icon_press(app_hit(x, y));   /* P3p */
            }
            return;
        }
        if (y < 56 && x >= 195 && lockY == 0 && !focus_on) {
            /* B4 (IOS-REFERENCE 1): the Control Centre opens over the lock too (made in the lock's idle time) */
            T.kind = G_CC;
            sp_cc.live = 0;
            if (!CCS.built || cc_stale) { build_cc(); cc_stale = 0; }
            tk_tracker_add(&T.tr, y, t);
            return;
        }
        for (int i = 0; i < 2; i++) {
            float bx = HOLD_X(i);
            if (x >= bx - 4 && x <= bx + 54 && y >= HOLD_Y - 4 && y <= HOLD_Y + 54) {
                T.kind = G_HOLD;
                T.arg = i;
                Q[i].down = 1; Q[i].fired = 0; Q[i].t0 = t;
                Q[i].scale_from = Q[i].grp.sx; Q[i].scale_to = 1.22f; Q[i].scale_t = 0;
                return;
            }
        }
        int gi = -1, r;
        if (EMG && y >= LIST_TOP && y < LIST_TOP + LIST_H && y < H_PT - 30) {
            /* Emergency's list: a card (survival-next: its tool over the lock), Exit Emergency, or a scroll */
            T.kind = G_ETAP;
            T.arg = sv_ec_hit(x, y);                    /* survival-next: 0..7 a card, 8 Exit (the list scrolls) */
            if (T.arg >= 0 && T.arg < 8) {
                tk_rect c = { CARD_X, LIST_TOP + 10 + 78.0f * T.arg, CARD_W, 68 };
                hl_set(&lockhl, c, c, 26, W(.1f));
            }
            tk_tracker_add(&T.tr, y, t);
            return;
        }
        if (y >= H_PT - 30) { T.kind = G_LOCK; T.base = lockY; sp_lock.live = 0; }
        /* B10: the notifications fanned out are over the list: theirs first (a tap beside them folds them back) */
        else if (lkn_p() > 0 && lkn_down(x, y, t)) { T.kind = G_LKN; return; }
        else if ((r = hit_row(x, y, &gi)) >= 0) {
            T.kind = G_TAP_ROW; T.arg = gi; T.arg2 = r;
            /* P3p: the row lights across its card (clipped to the card's rounded face) */
            float ct = LIST_TOP + card_top(gi);
            tk_rect pr = { VC_X, ct + 68 + 8 + MEM_P * r, VC_W, 68 };   /* B4: the member's own platter */
            hl_set(&lockhl, pr, pr, VC_R, W(.08f));
            lock_ver++;
        } else if ((r = hit_head(x, y)) >= 0) {
            T.kind = G_TAP_HEAD; T.arg = r;
            tk_rect c = { VC_X, LIST_TOP + card_top(r), VC_W, 68 };
            hl_set(&lockhl, c, c, VC_R, W(.1f));
            lock_ver++;
        }
        else if (in_list(y)) { T.kind = G_LIST; T.base = scroll; sp_scroll.live = 0; scroll_momentum = 0; list_over = scroll < 0 || scroll > max_scroll(); }
        else if (lkn_down(x, y, t)) { T.kind = G_LKN; return; }   /* B9: the notifications (a tap expands / opens) */
        else { T.kind = G_LOCK; T.base = lockY; sp_lock.live = 0; }
        tk_tracker_add(&T.tr, y, t);
        return;
    }
    if (T.kind == G_SW) {                    /* helper D, shell_switcher.inc */
        sw_touch(phase, x, y, t);
        if (phase == 2) T.kind = G_NONE;
        return;
    }
    if (T.kind == G_LKN) {                   /* B9, shell_lknote.inc: 2 hands the touch to the lock's swipe */
        if (lkn_touch(phase, x, y, t) == 2) {
            T.kind = G_LOCK; T.base = lockY; sp_lock.live = 0;
            tk_tracker_add(&T.tr, y, t);
        } else if (phase == 2) T.kind = G_NONE;
        return;
    }
    if (T.kind == G_NC) {                    /* helper D, shell_nc.inc */
        if (nc_touch(phase, x, y, t) == 2) {     /* the lock's pill moved: the lock's swipe after all */
            T.kind = G_LOCK; T.base = lockY; sp_lock.live = 0;
            tk_tracker_add(&T.tr, y, t);
        } else if (phase == 2) T.kind = G_NONE;
        return;
    }
    if (T.kind == G_KB) {                    /* B3, shell_kb.inc: the keyboard */
        kb_touch(phase, x, y, t);
        if (phase == 2) T.kind = G_NONE;
        return;
    }
    if (T.kind == G_LINUX) {                 /* L3, shell_linux.inc: a Linux window, the App Store's list */
        lx_touch(phase, x, y);
        if (phase == 2) T.kind = G_NONE;
        return;
    }
    if (T.kind == G_MEDIA) {                 /* shell_media.inc: its lists, the viewer */
        med_touch(phase, x, y, t);
        if (phase == 2) T.kind = G_NONE;
        return;
    }
    if (T.kind == G_SET) {                   /* helper B2, shell_settings.inc: a row, a switch, the slider, the scroll */
        if (phase == 2) T.kind = G_NONE;     /* (first: a tap may open another app, whose touches start anew) */
        se_touch(phase, x, y, t);
        return;
    }
    if (phase == 1) {
        float dy = y - T.y0;
        if (tk_fabsf(dy) > 5 || tk_fabsf(x - T.x0) > 5) T.moved = 1;
        switch (T.kind) {
        case G_THUMB:
            tk_tracker_add(&T.tr, x, t);
            set_thumb(tk_clampf(T.base + x - T.x0, 0, SL_W - 62));
            {   /* B4: the slide's start threshold, felt once each way it is crossed */
                int past = thumb_x > (SL_W - 62) * .86f;
                if (past != T.arg2) { T.arg2 = past; if (past) sh_haptic_k(SH_HAP_LIGHT); }
            }
            break;
        case G_FOCUSDRAG:
            /* P3p (P3j-19): the sheet follows the finger down, rubber-bands up */
            tk_tracker_add(&T.tr, y, t);
            set_focus(dy >= 0 ? tk_maxf(0, 1 - dy / 300) : 1 + rubber(-dy, H_PT) * .3f / 300);
            break;
        case G_APPTAP:
            if (T.moved && !apkey.hidden) key_down(-1);
            if (T.moved) hl_off();
            break;
        case G_DOCK:
            if (T.moved) press_cancel();
            break;
        case G_TAP_HEAD: case G_TAP_ROW: case G_ETAP:
            if (T.moved) hl_off();
            /* a drag: the list scrolls (Emergency's, or the vendor cards': B10, as B4's) */
            if (T.moved) { T.kind = G_LIST; T.base = scroll; T.y0 = y; sp_scroll.live = 0; }
            tk_tracker_add(&T.tr, y, t);
            break;
        case G_LIST: {
            tk_tracker_add(&T.tr, y, t);
            float s = T.base - dy, m = max_scroll();
            if (s < 0) s = -rubber(-s, LIST_H);
            else if (s > m) s = m + rubber(s - m, LIST_H);
            scroll = s;
            {   /* B4: a soft tick where the list's end is reached (its rubber band begins) */
                int over = s < 0 || s > m;
                if (over && !list_over) sh_haptic_k(SH_HAP_SOFT);
                list_over = over;
            }
            place_list();
            break;
        }
        case G_RELOCK:
            tk_tracker_add(&T.tr, y, t);
            set_lock(-H_PT + tk_maxf(0, dy));
            break;
        case G_APP: {
            if (!T.arg2) {
                /* B4: the first 8 pt decide: sideways along the bottom edge is the quick switch (shell_switcher.inc),
                 * else the window follows (from here: no jump) */
                float dx = x - T.x0;
                if (dx * dx + dy * dy < 64) break;
                if (tk_fabsf(dx) > tk_fabsf(dy) && sw_edge_ok()) { AP.following = 0; T.kind = G_SW; sw_edge_begin(x, t); break; }
                T.arg2 = 1; T.x0 = x; T.y0 = y; dy = 0;
            }
            tk_tracker_add(&T.tr, y, t);
            float d2 = tk_minf(0, dy), sc = tk_maxf(.35f, 1 + d2 / (H_PT * .9f));
            arect R = { 0, 0, 390 * sc, H_PT * sc, 52 + (30 - 52) * (1 - sc) };
            /* P3i (audit 8): the point grabbed stays under the finger (no 64 pt jump at the first move) */
            R.x = x - T.x0 / 390 * R.w;
            R.y = y - T.y0 / H_PT * R.h;
            AP.travel = d2;
            set_app_rect(R);
            sw_drag(d2, tk_tracker_v(&T.tr), t);      /* helper D: a pause shows the switcher */
            break;
        }
        case G_CC:
            tk_tracker_add(&T.tr, y, t);
            CCS.on = 1;
            cclit.hidden = !Q[0].lit || !cclit.bmp.px;
            CCS.closing = 0;
            /* P3p (P3j-10): past full the pull rubber-bands */
            set_cc(dy <= 260 ? tk_maxf(0, dy) / 260 : 1 + rubber(dy - 260, H_PT) / 260);
            break;
        case G_CCTAP:
            /* P3p (P3j-10): a vertical drag on the open Control Centre moves it (up closes it) */
            tk_tracker_add(&T.tr, y, t);
            if (T.moved) cc_release();
            if (T.moved && tk_fabsf(dy) > tk_fabsf(x - T.x0)) { T.kind = G_CCDRAG; sp_cc.live = 0; CCS.closing = 0; }
            if (T.kind == G_CCDRAG) set_cc(tk_clampf(dy < 0 ? 1 + dy / 260 : 1 + rubber(dy, H_PT) / 260, 0, 1.3f));
            break;
        case G_CCDRAG:
            tk_tracker_add(&T.tr, y, t);
            set_cc(tk_clampf(dy < 0 ? 1 + dy / 260 : 1 + rubber(dy, H_PT) / 260, 0, 1.3f));
            break;
        case G_CCSL:
            cc_set_level(T.arg, T.base - dy / CC_W2);
            break;
        case G_CCX:
            if (T.arg2 == 1 && CX.m > 0) cc_set_level(CX.m - 1, T.base - dy / CXR[CX.m][3]);
            break;
        case G_PAGE: {
            float dx = x - T.x0;
            tk_tracker_add(&T.tr, x, t);
            if (!page_dir && dx * dx + dy * dy > 64) { page_dir = tk_fabsf(dx) > tk_fabsf(dy) ? 1 : 2; press_cancel(); }
            if (page_dir == 1) {
                page_dragging = 1;
                float px = T.base + dx;
                if (px > 0) px = rubber(px, 390);                 /* B8 (GAP #21): iOS's rubber band, not a flat .3 */
                if (px < -780) px = -780 - rubber(-780 - px, 390);   /* L3: three pages */
                set_page(px);
            }
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
    hl_off();
    icon_release();                            /* P3p: the pressed icon springs back (under the opening app) */
    cc_release();                              /* B4: a pressed Control Centre module */
    switch (T.kind) {
    case G_THUMB:
        if (thumb_x > (SL_W - 62) * .86f) { set_thumb(SL_W - 62); booting = 1; boot_t = -100; }
        else tk_spring_start(&sp_thumb, thumb_x, 0, 0, TK_SPR_SHEET);
        break;
    case G_FOCUSDRAG: {
        /* P3p (P3j-19): dismissed where a flick would carry it below .6; the finger's speed goes to the spring */
        float vp = -tk_tracker_v(&T.tr) * 1000 / 300;
        if (focus_p + vp * .2f < .6f) { focus_closing = 1; tk_spring_start(&sp_focus, focus_p, 0, vp, TK_SPR_SHEET); }
        else tk_spring_start(&sp_focus, focus_p, 1, vp, TK_SPR_SHEET);
        break;
    }
    case G_SCRIM:
        focus_closing = 1;
        tk_spring_start(&sp_focus, focus_p, 0, 0, TK_SPR_SHEET);
        break;
    case G_HOLD: {
        hold_t *q = &Q[T.arg];
        q->down = 0;
        q->scale_from = q->grp.sx; q->scale_to = 1; q->scale_t = 0;
        if (!q->fired) toast("Press and hold");
        hold_ring(q, T.arg, 0);
        break;
    }
    case G_ETAP:
        if (!T.moved && T.arg >= 0) sv_ec_tap(T.arg);   /* survival-next */
        break;
    case G_CCEM:
        if (!T.arg) toast("Press and hold");
        break;
    case G_CCCELL:
        if (!T.arg) toast(data_on() ? "Hold to turn mobile data off" : "Hold for mobile data");
        break;
    case G_CF:
        if (CF.down) {                         /* let go before the bar was full */
            CF.down = 0;
            CF.p = 0;
            cf_place();
            toast("Press and hold");
        } else if (CF.on) {
            /* outside the card: cancel */
            float h = 0;
            (void)h;
            if (y < CF.top || y > CF.bot || x < CF_X || x > CF_X + 300 || (y >= CF.cy && y < CF.cy + 48)) cf_close(0);   /* outside, or Cancel */
        }
        break;
    case G_DOCK:
        if (!T.moved) { int a = app_hit(x, y); if (a >= 0) app_open(a); }
        break;
    case G_APPTAP:
        if (!apkey.hidden) key_down(-1);
        if (!T.moved) app_tap(x, y);
        break;
    case G_TAP_HEAD:
        toggle_group(T.arg, 0);
        break;
    case G_TAP_ROW: {
        const sys_t *s = &G[T.arg].it[T.arg2];
        if (s->self) { toast("You are in Starbite OS"); break; }
        tk_comp_frame(C, 0, 0);
        tk_comp_idle(C);                       /* P3p: the half canvas without the row's press light (the sheet's under-layer) */
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
    case G_CC: {
        float v = tk_tracker_v(&T.tr);
        if (CCS.p > .35f || v > .4f) { tk_spring_start(&sp_cc, CCS.p, 1, v * 1000 / 260, TK_SPR_CC); sh_haptic_k(SH_HAP_LIGHT); }
        else cc_close();
        break;
    }
    case G_CCX:
        /* B4: a link of the expanded panel, or a tap outside it (it folds back); nothing after the long press itself */
        if (T.arg == -2 || T.moved) break;
        if (CX.m == 0 && T.arg >= 0) cc_x_tap(T.arg);
        else if (!cc_x_inside(x, y)) cc_x_close();
        break;
    case G_CCTAP:
        if (!T.moved && !cc_tap(x, y)) cc_close();
        break;
    case G_CCDRAG: {
        /* P3p (P3j-10): where the flick would carry it (200 ms) decides; the finger's speed goes to the spring */
        float vp = tk_tracker_v(&T.tr) * 1000 / 260;
        if (CCS.p + vp * .2f < .65f) { CCS.closing = 1; tk_spring_start(&sp_cc, CCS.p, 0, vp, TK_SPR_FAST); }
        else tk_spring_start(&sp_cc, CCS.p, 1, vp, TK_SPR_CC);
        break;
    }
    case G_APP: {
        float v = tk_tracker_v(&T.tr);
        AP.following = 0;
        if (sw_release(v)) break;              /* helper D: paused (or slow past 30 %): the switcher */
        if (AP.travel < -60 || v < -.35f) app_close(v);
        else { AP.from = AP.R; AP.to = FULLR; tk_spring_start(&sp_app, 0, 1, 0, TK_SPR_OPEN); }
        break;
    }
    case G_PAGE:
        if (!page_dir) {                      /* a tap: an app icon opens its app */
            int a = app_hit(x, y);
            if (a >= 0) app_open(a);
            else wclock_tap(x, y);             /* helper D: the Clock widget opens Clock from itself */
            break;
        }
        if (page_dir == 1) {
            /* L3: three pages, at most one from where the drag began */
            float v = tk_tracker_v(&T.tr);
            int bp = (int)tk_floorf(-T.base / 390 + .5f), tp = (int)tk_floorf(-(page_x + v * 220) / 390 + .5f);
            float target = -390.0f * tk_mini(tk_mini(bp + 1, 2), tk_maxi(tk_maxi(bp - 1, 0), tp));
            page_dragging = 0;
            tk_spring_start(&sp_page, page_x, target, v * 1000, TK_SPR_ROWS);
        }
        break;
    case G_RELOCK: {
        /* P3p (P3j-10): decided by where the pull would carry it (200 ms), the finger's speed handed to the spring */
        float v = tk_tracker_v(&T.tr);
        if (lockY + v * 200 > -H_PT * .5f) { locked = 1; tk_spring_start(&sp_lock, lockY, 0, v * 1000, TK_SPR_UNLOCK); }
        else tk_spring_start(&sp_lock, lockY, -H_PT, v * 1000, TK_SPR_UNLOCK);
        break;
    }
    case G_LOCK:
        if (!T.moved) {
            hint_left = 2200;
            tk_spring_start(&sp_lock, 0, 0, -700, TK_SPR_UNLOCK);
        } else {
            float v = tk_tracker_v(&T.tr), proj = lockY + v * 200;
            /* P3p (P3j-9): unlocked as soon as it is decided: taps in the settle reach home */
            if (proj < -H_PT * .38f && locked) {
                tk_spring_start(&sp_lock, lockY, -H_PT, v * 1000, TK_SPR_UNLOCK);
                if (!pc_unlock_begin()) locked = 0;   /* Track A: the passcode first; else the kept app comes back */
            }
            else tk_spring_start(&sp_lock, lockY, 0, v * 1000, TK_SPR_UNLOCK);
        }
        break;
    }
    T.kind = G_NONE;
}

/* ---- time ------------------------------------------------------------------------------------------------- */
/* the Power key (or the screen-off timer): the fade to black; again during it: back (now: black at once, the
 * screen already off) */
void sh_sleep(int now)
{
    T.kind = G_NONE;
    pw_reset();                                    /* P3x: the Power key (or the screen's timer) cancels the power sheet */
    if (now) {
        if (SL.black && !SL.dir) return;   /* dark already (the fade ended) */
        SL.dir = 0;
        SL.black = 1;
        back_to_lock(0);
        sl_place();
        return;
    }
    if (SL.dir < 0) return;
    float a = slp.hidden ? 0 : slp.alpha;
    SL.dir = -1;
    SL.t = 0;
    for (int i = 0; i < 20 && tk_bezier(.4f, 0, .7f, 1, SL.t) < a; i++) SL.t += .05f;   /* from a wake's opacity */
    sl_place();
}
int sh_sleeping(void) { return SL.dir < 0; }
/* the screen on: the lock screen from black (a fade back from a sleep that had not ended starts where it is) */
void sh_wake(void)
{
    T.kind = G_NONE;
    float a = slp.hidden ? 0 : slp.alpha;
    SL.back = SL.dir < 0;                  /* Power again during the fade: back from where it is, as it was */
    SL.dir = 1;
    SL.black = 0;
    SL.t = SL.back ? (1 - tk_sqrtf(tk_clampf(a, 0, 1))) / 1.6f : 0;    /* the wake's opacity curve inverted */
    sl_place();
}

static int tween(float *v, float target, float dur_s, float dt)
{
    if (*v == target) return 0;
    float step = dt / dur_s;
    if (*v < target) *v = tk_minf(target, *v + step); else *v = tk_maxf(target, *v - step);
    return 1;
}

/* B6: the Emergency hold (or the Control Centre's tile) is down and not fired yet */
static int ep_holding(void) { return (Q[1].down && !Q[1].fired) || (T.kind == G_CCEM && !T.arg); }
/* a frame's slice of the switch's work: prepared while the hold is down (never during a fade: one ground at a time),
 * freed when it is let go; after the switch the rest (home's at once if it is about to show) */
static void ep_tick(void)
{
    if (EP.goal >= 0 && EP.goal != EMG && !ep_holding()) ep_cancel();
    if (EP.goal < 0 && ep_holding() && em_t >= 1) ep_begin(!EMG);
    if (EP.goal < 0) return;
    if (EP.goal == EMG && EP.post & (EP_HOMEW | EP_APPS2) && (locked ? lockY != 0 : !CCS.on)) ep_home_now();
    if (!EP.fresh) ep_work(0);                   /* the frame of a tap's switch does the switch only */
    EP.fresh = 0;
}
int sh_tick(float dt, float t)
{
    int mv = 0;
    now_ms = t;
    clk_update(t);
    if (ap_pending) { app_build_step(); mv = 1; if (!ap_pending && AP.on) set_app_rect(AP.R); }   /* P3i */
    if (SL.dir) {
        SL.t = tk_minf(1, SL.t + dt / (SL.dir < 0 ? SL_OUT : SL_IN));
        if (SL.t >= 1) {
            if (SL.dir < 0) {
                SL.black = 1;
                SL.dir = 0;
                back_to_lock(0);                 /* in the dark: the lock screen for the wake */
                sl_place();
                sh_slept();                      /* the kernel: the panel off after this black frame */
                return 1;
            }
            SL.dir = 0;
        }
        sl_place();
        mv = 1;
    }
    if (sp_lock.live) {
        int live = tk_spring_step(&sp_lock, dt);
        set_lock(sp_lock.x);
        if (!live && sp_lock.to <= -H_PT + .01f && !pc_holds()) { locked = 0; lockflat.hidden = 1; home.sx = home.sy = 1; }   /* Track A */
        mv = 1;
    }
    if (T.kind == G_LOCK || T.kind == G_LIST || T.kind == G_THUMB || T.kind == G_RELOCK || T.kind == G_PAGE) mv = 1;
    if (sp_page.live) {
        tk_spring_step(&sp_page, dt);
        set_page(sp_page.x);
        mv = 1;
    }
    if (T.kind == G_CC || T.kind == G_CCSL || T.kind == G_APP || T.kind == G_CCDRAG || T.kind == G_FOCUSDRAG) mv = 1;
    if (sp_app.live) {
        int live = tk_spring_step(&sp_app, dt);
        float u = AP.closing ? tk_minf(1.04f, sp_app.x) : sp_app.x;
        set_app_rect(lerpr(AP.from, AP.to, u));
        if (!live) {
            if (AP.closing) app_finish_close();
            else if (!SW.on) appg.clip = 0;  /* at rest: the whole panel, no clip (as the concept drops it); a card keeps it */
        }
        mv = 1;
    }
    mv |= ico_step(dt);                          /* P3p: the pressed icon */
    mv |= med_tick(dt);
    mv |= lx_tick(dt);                           /* L3, Track F: the App Store's page (scroll, momentum, builds) */
    ep_tick();                                   /* B6: Emergency's switch prepared during the hold, finished after */
    if (em_t < 1) {
        /* B6: the fade waits for its ground, and starts the frame after a hand-over made over frames */
        if (EP.goal >= 0 && EP.goal == EMG && EP.ground == 1) EP.ground = 2;
        else if (!(EP.goal >= 0 && EP.goal == EMG && !EP.ground)) {
            em_t = tk_minf(1, em_t + dt / .7f);
            em_step(tk_bezier(.25f, .1f, .25f, 1, em_t));
        }
        mv = 1;
    }
    if (T.kind == G_CCCELL && !T.arg) {
        if (t - T.t0 >= 700) {
            T.arg = 1;
            cell_toggle();
            sh_haptic_k(SH_HAP_MEDIUM);              /* B4: the hold's threshold, felt */
        }
        mv = 1;
    }
    if (CF.down) {
        CF.p = tk_minf(1, (t - CF.t0) / 1500);
        cf_place();
        if (CF.p >= 1) { sh_haptic_k(SH_HAP_MEDIUM); cf_close(1); }   /* B4: the hold bar's end, felt */
        mv = 1;
    }
    if (CF.a != CF.at) {
        CF.a = CF.at > CF.a ? tk_minf(CF.at, CF.a + dt / .2f) : tk_maxf(CF.at, CF.a - dt / .2f);
        cf_place();
        if (!CF.on && CF.a <= 0) cf_free();
        mv = 1;
    }
    if (T.kind == G_CCEM && !T.arg) {
        if (t - T.t0 >= 700) { T.arg = 1; sh_haptic_k(SH_HAP_MEDIUM); set_emergency(!EMG, 0); }
        mv = 1;
    }
    mv |= isl_tick(dt, t);                           /* B7: the live pill (toasts), the status capsule */
    if (sp_cc.live) {
        int live = tk_spring_step(&sp_cc, dt);
        set_cc(tk_maxf(0, sp_cc.x));
        if (!live && CCS.closing) { CCS.on = 0; CCS.closing = 0; cc_x_reset(); set_cc(0); }
        mv = 1;
    }
    if (CCS.built) mv |= cc_x_tick(dt);          /* B4: the expanded module's morph, the modules' press */
    /* B4: a long press (450 ms) on a slider or the connectivity module expands it; the finger goes on with it */
    if (CCS.on && CX.m < 0 && !T.moved && t - T.t0 >= 450) {
        float X = CC_COL(0), Y = CC_ROW(0);
        if (T.kind == G_CCSL) {
            cc_release();
            int i = T.arg;
            cc_x_open(i + 1);
            T.kind = G_CCX; T.arg = -2; T.arg2 = 1; T.base = CCS.lvl[i];
        } else if (T.kind == G_CCTAP && T.x0 >= X && T.x0 < X + CC_W2 && T.y0 >= Y && T.y0 < Y + CC_W2) {
            cc_release();
            cc_x_open(0);
            T.kind = G_CCX; T.arg = -2; T.arg2 = 0;
        }
    }
    if (T.kind == G_CCTAP || T.kind == G_CCX) mv = 1;
    /* the mark (.3 s), the glyph (.2 s) and the hint (.3 s): linear stand-ins for their CSS ease */
    mv |= tween(&mark_a, mark_t, .3f, dt);
    mv |= tween(&glyph_a, glyph_t, .2f, dt);
    if (hint_left > 0) { hint_left -= dt * 1000; mv |= tween(&hint_a, 1, .3f, dt); }
    else mv |= tween(&hint_a, 0, .3f, dt);
    apply_tweens();
    for (int g = 0; g < NG; g++) {               /* B10: B4's vendor cards (their fan and chevron) */
        vcard_t *k = &V[g];
        if (k->sp.live) {
            int live = tk_spring_step(&k->sp, dt);
            k->memh = tk_maxf(0, k->sp.x);
            if (!live && k->isopen) {
                /* the concept: when open, scroll the card into view (rows spring) */
                float ct = card_top(g), top = ct - SL_BAND - 8, m = max_scroll();
                if (ct + 68 + k->full > scroll + list_h()) tk_spring_start(&sp_scroll, scroll, tk_minf(top, m), 0, TK_SPR_ROWS);
            }
            mv = 1;
            place_list();
        }
        if (!k->isopen && !k->sp.live && k->chev_t >= 1 && k->memh <= 0 && k->rows[0].bmp.px) { free_open(g); relink(); place_list(); }
        if (k->chev_t < 1) {
            k->chev_t = tk_minf(1, k->chev_t + dt / .3f);      /* B4 (P3j-8): iOS's ease, no overshoot */
            k->chev_now = k->chev_from + (k->chev_to - k->chev_from) * tk_bezier(.25f, .1f, .25f, 1, k->chev_t);
            chev_draw(k, k->chev_now);
            mv = 1;
            if (k->chev_t >= 1) place_list();
        }
    }
    mv |= lkn_tick(dt, t);                       /* B9: the lock's notifications */
    /* B10: the list's room shrank (a notification came, a card closed): back inside its end */
    if (!EMG && locked && !sp_scroll.live && !scroll_momentum && T.kind != G_LIST && scroll > max_scroll() + .5f)
        tk_spring_start(&sp_scroll, scroll, max_scroll(), 0, TK_SPR_ROWS);
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
            if (q->scale_t <= 0) tk_spring_start(&q->ss, q->grp.sx, q->scale_to, q->ss.live ? q->ss.v : 0, TK_SPR_PRESS);
            q->scale_t = .5f;
            if (!tk_spring_step(&q->ss, dt)) q->scale_t = 1;
            q->grp.sx = q->grp.sy = q->ss.x;
            mv = 1;
        }
        if (q->down && !q->fired) {
            float p = tk_minf(1, (t - q->t0) / q->ms);
            hold_ring(q, i, p);
            if (p >= 1) {
                q->fired = 1;
                sh_haptic_k(SH_HAP_MEDIUM);      /* P3p (P3j-17): the threshold, felt (no ring); B4: a long press's commit */
                if (i == 0) {
                    q->lit = !q->lit;
                    sh_torch(q->lit);
                    toast(q->lit ? "Flashlight on" : "Flashlight off");
                    q->facelit.hidden = q->iconlit.hidden = !q->lit;
                    q->face.hidden = q->icon.hidden = q->lit;
                } else
                    set_emergency(!EMG, 0);
                hold_ring(q, i, 0);
            }
            mv = 1;
        }
    }
    if (focus_on) {
        if (sp_focus.live) {
            int live = tk_spring_step(&sp_focus, dt);
            set_focus(tk_maxf(0, sp_focus.x));
            if (!live && focus_closing) { focus_on = 0; focus.hidden = 1; free_focus(); }
            mv = 1;
        }
        if (sp_thumb.live) { tk_spring_step(&sp_thumb, dt); set_thumb(sp_thumb.x); mv = 1; }
    }
    if (booting) {
        boot_t += dt * 1000;
        if (boot_t >= 0) {
            if (!boot.solid) {
                /* the black ground is a solid layer (no 17 MB bitmap); the tile, the words and the bar's track
                 * one small layer over it */
                boot.solid = 0xff000000u;
                boot.opaque = 1;
                boot.solid_w = TK_W;
                boot.solid_h = TK_H;
                dc d = mk(&bootc, "boot-content", 0, 300, 390, 175, 0, 0);
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
                boot.child = &bootc;
                bootc.next = &bootbar;
            }
            /* the bar: width 0 -> 100 % over 2.1 s, CSS ease, starting with the overlay */
            bootbar.slice_h = PT(4 + 146 * tk_bezier(.25f, .1f, .25f, 1, tk_minf(1, boot_t / 2100)));
            boot.hidden = 0;
            boot_a = tk_minf(1, boot_t / 350);
            boot.alpha = boot_a;
            mv = 1;
            if (boot_t > 2400) {
                booting = 0;
                sh_boot_os(focus_sys->k);       /* the loader's one-shot next boot (the kernel's driver) */
            }
        }
    }
    mv |= sv_tick(dt, t);                       /* survival-next: a tool's hold, its polls */
    mv |= nc_tick(dt, t);                       /* helper D: the Notification Centre, the banner */
    mv |= sw_tick(dt, t);                       /* helper D: the app switcher */
    mv |= cm_tick(dt, t);                       /* helper I: the call button's hold, the call screen */
    mv |= pw_tick(dt, t);                       /* P3x: the power sheet */
    mv |= cam_tick(dt, t);                      /* the Camera: its frames, a shot (shell_camera.inc) */
    mv |= kb_tick(dt, t);                       /* B3: delete held */
    if (T.kind == G_APPTAP && !T.moved && t - T.t0 >= 500) {    /* B3: a long press on a text field: the edit menu */
        emt e;
        if (em_target(T.x0, T.y0, &e)) { hl_off(); T.kind = G_NONE; em_open(&e, T.x0, T.y0); mv = 1; }
    }
    mv |= kit_tick(dt);                         /* helper B2: an alert, a sheet, a switch's thumb */
    mv |= se_tick(dt);                          /* helper B2: Settings' scroll */
    mv |= notes_hold(t);                        /* helper B2: a held note: Delete? */
#ifdef SHELL_STARTUP_TICK
    if (is_app("settings") && AS.sub == SE_STARTUP_PAGE) mv |= startup_tick(dt);
#endif
    mv |= pc_tick(dt, t);                       /* Track A: the passcode screen */
    if (mv && AP.on) sw_dirty = 1;             /* helper D: the page moved or changed: a new snapshot at the next rest */
    return mv;
}

/* P3p: a new day: today's Calendar icon (app_icon() composes it again) over yesterday's on home's first page, in
 * place (no page rebuild); Search and the opening icon take it when they next draw */
static void cal_newday(void)
{
    float pad;
    const tk_bmp *ic = app_icon("calendar", &pad);
    if (ic != &calic) return;
    if (apicon.bmp.px == calic.px) tk_layer_dirty(&apicon);
    if (calic_x < 0 || !hparts[2].bmp.px) return;
    tk_fill_rect_raw(&hparts[2].bmp, calic_x, calic_y, ic->w, ic->h, 0);
    tk_blit(&hparts[2].bmp, ic, calic_x, calic_y, 1);
    tk_layer_dirty(&hparts[2]);
}
void sh_status(int h, int mi, int wday, int day, int mon, int pct, int temp)
{
    int tchange = h != ST.h || mi != ST.mi || wday != ST.wday || day != ST.day || mon != ST.mon;
    int wchange = pct != ST.pct || temp != ST.temp;
    ST.h = h; ST.mi = mi; ST.wday = wday; ST.day = day; ST.mon = mon; ST.pct = pct; ST.temp = temp;
    se_batt_sample(h, mi, pct);                   /* helper B2: Settings > Battery's history since the start (RAM) */
    if (tchange && AP.on && (is_app("clock") || is_app("calendar")) && !AP.closing) build_app_page(AP.a);   /* the time, today */
    if (tchange && ALM.on && alm_when != (ALM.h * 60 + ALM.m > ST.h * 60 + ST.mi ? 1 : 2)) wchange = 1;   /* P3p: Today / Tomorrow */
    if (tchange) {
        /* the layers keep their place in the tree: rebuilt in place */
        if (calic_key != cal_key()) cal_newday();
        build_lclock();
        build_home_sb();
        if (AP.on && !AP.closing) build_apsb();
        nc_minute();                             /* helper D: the platters' "5m ago" */
        lkn_minute();                            /* B9: the lock's too */
        build_wclock();                          /* helper D: the Clock widget's hands */
        cm_minute();                             /* helper I: the call screen's status bar */
        relink();
    }
    if (wchange) {
        build_lwidgets();
        build_home_w();                         /* P2y: home's battery widget shows the same live values */
        relink();
    }
    if (tchange || wchange) prep_tree(home.child);   /* P3g: not in the next frame that shows home (22 ms) */
    if (tchange || wchange) lock_ver++;
    if (tchange) isl_sync();                     /* B7: the capsule's time */
}

/* the half copies of a subtree (hidden ones too: home is drawn the moment the unlock starts) */
static void prep_tree(tk_layer *l)
{
    for (; l; l = l->next) {
        tk_layer_prepare(l);
        prep_tree(l->child);
    }
}
void sh_idle(void)
{
    /* P3j: an app page still being drawn (an open without frames, e.g. the host's stills): finish it now */
    if (ap_pending) { while (ap_pending) app_build_step(); if (AP.on) set_app_rect(AP.R); }
    if (sh_moving()) return;
    /* B6: a prepared switch nobody holds any more goes; a switch's rest, a slice a turn (sh_idle_pending) */
    if (EP.goal >= 0 && EP.goal != EMG && !ep_holding()) ep_cancel();
    if (EP.goal >= 0 && EP.goal == EMG && !(!EMG && cards_pending > 0)) { ep_work(1); return; }   /* lock's cards first */
    tk_comp_idle(C);
    sw_idle();                                   /* helper D: the open app's snapshot for the switcher (when it changed) */
    mp_idle();                                   /* helper D: Maps' last GPS fix (it may read the card: idle only) */
    cm_idle();                                   /* helper I: the call screen's static parts while Cellular is on */
    home_badges(0);                              /* B8: the dock's badges, when a count changed */
    kb_idle();                                   /* B3: the keyboard's accessory row (a Linux window's keys) */
    se_idle();                                   /* helper B2: Settings' page's half copy */
    if (!EMG && cards_pending > 0) {
        /* after Emergency: the vendor cards again, one per idle turn (the platform calls again while
         * sh_idle_pending); the half copies below are made in the same turn (P2w: an unlock right after
         * Emergency made them in its first frame, 31 ms) */
        build_lockpart(LOCK_PARTS - cards_pending);
        cards_pending--;
        relink();
        place_list();
    }
    sv_idle();                                   /* survival-next: Emergency's next card, the lock's widgets */
    /* P3s: what is not shown here and the next gesture does not need goes; what the next one needs is made */
    /* B4: the lock keeps it too (it opens there), unless a sheet needs the room */
    int cc_lock_ok = locked && lockY == 0 && !focus_on;
    if (locked && !cc_lock_ok) free_cc_all();
    if (focus_faces_idle(focus_faces_keep())) return;    /* a sheet's glass: one bake a turn (sh_idle_pending) */
    lkn_idle();                                  /* B9: the lock's notifications as the store has them (before the cache below) */
    if (locked && lockY == 0 && !focus_on) flatten_lock(1);
    /* home at rest: the Control Centre's snapshot and grid ready before the pull */
    if ((!locked || cc_lock_ok) && !CCS.on && !AP.on && !nc_on() &&
        (!CCS.built || cc_stale || cc_lk != locked || (locked && cc_lv != lock_ver))) {
        build_cc();
        cc_stale = 0; cc_lk = locked; cc_lv = lock_ver;
    }
    /* the half copies of what moves (the rest are never drawn at half resolution: the half canvas follows the
     * rest frames) */
    prep_tree(home.child);                       /* home's pages too (their widgets and app grids, P2s) */
    tk_layer_prepare(&star);
    tk_layer_prepare(&homebar);
    if (K[0].gh.bmp.px) tk_layer_prepare(&K[0].gh);
    if (K[0].face.bmp.px) tk_layer_prepare(&K[0].face);
    for (int g = 0; g < NG; g++) {               /* B10: the vendor cards' own pixels (their glass: the views share K[0]'s) */
        if (V[g].head.bmp.px) tk_layer_prepare(&V[g].head);
        if (V[g].chev.bmp.px) tk_layer_prepare(&V[g].chev);
        for (int i = 0; i < G[g].n; i++) if (V[g].rows[i].bmp.px) tk_layer_prepare(&V[g].rows[i]);
    }
    if (etool.bmp.px) tk_layer_prepare(&etool);
    for (int i = 0; i < 2; i++) {
        tk_layer_prepare(&Q[i].face); tk_layer_prepare(&Q[i].facelit);
        tk_layer_prepare(&Q[i].icon); tk_layer_prepare(&Q[i].iconlit);
    }
    tk_layer_prepare(&mark); tk_layer_prepare(&glyph_c); tk_layer_prepare(&glyph_o); tk_layer_prepare(&hint);
}

int sh_idle_pending(void) { return (EP.goal >= 0 && EP.goal == EMG) || (!EMG && cards_pending > 0) || sv_idle_pending() || focus_faces_missing() || tk_comp_idle_pending(C); }

int sh_moving(void)
{
    int m = sp_lock.live || sp_scroll.live || scroll_momentum || T.kind == G_LOCK || T.kind == G_LIST || T.kind == G_THUMB ||
            T.kind == G_RELOCK || T.kind == G_PAGE || sp_page.live || T.kind == G_CC || T.kind == G_CCSL || sp_cc.live ||
            T.kind == G_APP || sp_app.live || booting || isl_moving() || em_t < 1 || T.kind == G_CCEM ||
            T.kind == G_CCCELL || CF.down || CF.a != CF.at || pw_moving() || SL.dir || T.kind == G_CCDRAG || T.kind == G_FOCUSDRAG || sv_moving() ||
            T.kind == G_CCX || (T.kind == G_CCTAP && CCS.on) || cc_moving();
    for (int g = 0; g < NG; g++) m |= V[g].sp.live || V[g].chev_t < 1;   /* B10 */
    m |= lkn_moving();                           /* B9 */
    m |= sp_focus.live || sp_thumb.live || med_moving() || lx_moving();
    m |= IP.on && IP.t < 1;
    m |= nc_moving() || sw_moving();             /* helper D */
    m |= cm_moving();                            /* helper I */
    m |= kb_moving();                            /* B3: delete held (its repeats need the frames) */
    if (T.kind == G_APPTAP && !T.moved) { emt e; m |= em_target(T.x0, T.y0, &e); }   /* B3: a press on a text field: the long press's frames */
    m |= kit_moving() || se_moving();            /* helper B2 */
    m |= pc_moving();                            /* Track A */
    return m;
}
