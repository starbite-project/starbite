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
    float x0 = tk_maxf(PT(x - mg), 0), y0 = PT(y - mg);       /* nothing beyond the panel's sides is ever shown */
    float x1 = tk_minf(PT(x + w + mg), (float)TK_W), y1 = PT(y + h + mg);
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

static void relayer(tk_layer *l) { tk_layer_dirty(l); tk_bmp_free(&l->bmp); tk_a8_free(&l->glass); }

/* ---- state ------------------------------------------------------------------------------------------------- */
static tk_layer root, home, lockflat, lockg, mark, glyph_c, glyph_o, list, etool, hint, homebar;
static tk_layer boot, bootc, bootbar;
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
static int EMG;                 /* Emergency mode (the concept's em-mode) */
static int cell_running(void);  /* the modem runs (sh_cell) */
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
static struct { int h, mi, wday, day, mon, pct, temp; } ST = { 21, 5, 2, 10, 11, 87, 41 };
static struct { int on, h, m; } ALM = { 1, 7, 0 };   /* the host's is the concept's 07:00 */
static int sys_charging;           /* P2y: sh_sysinfo's (the status bar's battery is green while charging) */
#define SYS_CHARGING() sys_charging
static void status_bar(dc *d, const char *left)
{
    d_text(d, TK_IT600, 15, 36, 29.5f, left, 0, 0, INK, 1);
    if (SB_CELL.state == 1 && SB_CELL.bars >= 0) {
        /* four bars (0..5 from the CP: 5 is four), the RAT before them: only while the modem is on */
        int n = SB_CELL.bars >= 4 ? 4 : SB_CELL.bars;
        for (int i = 0; i < 4; i++) d_rrect(d, 289 + 4.5f * i, 30 - (4.5f + 2.5f * i), 3, 4.5f + 2.5f * i, 1, W(i < n ? 1 : .3f));
        const char *r = SB_CELL.rat == 7 ? "4G" : SB_CELL.rat == 4 ? "3G" : SB_CELL.rat >= 1 && SB_CELL.rat <= 3 ? "2G" : "";
        if (r[0]) d_text(d, TK_IT600, 12, 286 - tk_text_w(TK_IT600, PT(12), r, 0, 0) / TK_PT, 29.5f, r, 0, 0, INK, 1);
    }
    /* P2y: no Wi-Fi glyph (Next has no Wi-Fi connection); the battery drawn from the live charge (iOS's glyph:
     * a translucent body, the fill by percent, red at 20 % or less, green while charging, the nub) */
    float bx = 334, by = 18.5f, fw = 21.0f * tk_clampf(ST.pct / 100.0f, .04f, 1);
    d_rrect(d, bx, by, 25, 12, 3.6f, W(.35f));
    d_rrect(d, bx + 2, by + 2, fw, 8, 2, ST.pct <= 20 && !SYS_CHARGING() ? tk_hex(0xff453a, 1) : SYS_CHARGING() ? tk_hex(0x34c759, 1) : W(1));
    d_rrect(d, bx + 26, by + 4, 1.6f, 4, .8f, W(.4f));
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
/* the live values (sh_status): the host's are the concept's */
static const char *const WD[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const MO[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December" };
static int put_dec(char *b, int v) { int n = 0; char t[12]; int k = 0; if (v < 0) { b[n++] = '-'; v = -v; }
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) b[n++] = t[--k]; b[n] = 0; return n; }
static void fmt_hm(char *b) { b[0] = (char)('0' + ST.h / 10); b[1] = (char)('0' + ST.h % 10); b[2] = ':';
    b[3] = (char)('0' + ST.mi / 10); b[4] = (char)('0' + ST.mi % 10); b[5] = 0; }

static tk_layer lstatus, lclock, lwidgets;
static void relink(void);
static void link(tk_layer *parent, tk_layer **kids, int n);
static void lap_us(const char *what, uint64_t us);
static char carrier[32] = "No SIM";
static void build_lstatus(void)
{
    relayer(&lstatus);
    dc d = mk(&lstatus, "lock-status", 0, 0, 390, 48, 2, 0);
    status_bar(&d, carrier);
}
/* the modem's status (its session's status API): the carrier or "No SIM" / "Emergency calls only" */
void sh_carrier(const char *text)
{
    if (!tk_strcmp(text, carrier)) return;
    int n = 0;
    while (text[n] && n < 31) { carrier[n] = text[n]; n++; }
    carrier[n] = 0;
    build_lstatus();
    relink();
    lock_ver++;
}
static void build_lclock(void)
{
    /* the date (19 pt / 600) and the clock: 106 pt / 700 / -4 pt, tabular, gradient #fff -> #dfe9ff over its box */
    relayer(&lclock);
    dc d = mk(&lclock, "lock-clock", 0, 72, 390, 140, 2, 0);
    char date[48];
    int n = 0;
    for (const char *p = WD[ST.wday % 7]; *p;) date[n++] = *p++;
    date[n++] = ' ';
    n += put_dec(date + n, ST.day);
    date[n++] = ' ';
    for (const char *p = MO[(ST.mon + 11) % 12]; *p;) date[n++] = *p++;
    date[n] = 0;
    float w = tw(TK_IT600, 19, date, 0, 0);
    d_text(&d, TK_IT600, 19, (390 - w) / 2, 78 + 18, date, 0, 0, W(.9f), 1);
    char hm[8];
    fmt_hm(hm);
    float cw = tw(TK_IT700, 106, hm, -4, TK_TNUM);
    tk_stop s[2] = { { 0, tk_hex(0xffffff, 1) }, { 1, tk_hex(0xdfe9ff, 1) } };
    tk_paint p = tk_linear(180, -d.ox, PT(96) - d.oy, PT(390), PT(108.12f), 2, s);
    tk_text(d.b, TK_IT700, PT(106), PT((390 - cw) / 2) - d.ox, PT(base_of(TK_IT700, 106, 96, 108.12f)) - d.oy, hm,
            PT(-4), TK_TNUM, &p, 0);
}
static void build_lwidgets(void)
{
    relayer(&lwidgets);
    dc d = mk(&lwidgets, "lock-widgets", 0, 210, 390, 92, 0, 1);
    char num[12];
    put_dec(num, ST.pct);
    if (EMG) {
        /* Emergency: the battery ring amber, Heat (ready, the phone's temperature), Saving (off) */
        ring_widget(&d, 33, tk_clampf(ST.pct / 100.0f, 0, 1), tk_hex(0xffb547, 1), num, "BATTERY");
        ring_widget(&d, 285, 0, tk_hex(0xff5a4e, 1), "Off", "SAVING");
        d_glass(&d, 115, 220, 160, 72, 22, 1, 1);
        d_text(&d, TK_IT700, 11, 129, 238 + 11, "HEAT", .33f, 0, W(.7f), 0);
        char v[40] = "Ready \xc2\xb7 ";
        int k = (int)tk_strlen(v);
        k += put_dec(v + k, ST.temp);
        v[k++] = ' '; v[k++] = (char)0xc2; v[k++] = (char)0xb0; v[k++] = 'C'; v[k] = 0;
        d_text(&d, TK_IT700, 17, 129, 254 + 16, v, 0, 0, INK, 0);
        return;
    }
    ring_widget(&d, 33, tk_clampf(ST.pct / 100.0f, 0, 1), tk_hex(0x4cd964, 1), num, "BATTERY");
    int n = put_dec(num, ST.temp);
    num[n] = (char)0xc2; num[n + 1] = (char)0xb0; num[n + 2] = 0;
    ring_widget(&d, 285, tk_clampf(ST.temp / 100.0f, 0, 1), tk_hex(0x6fb3ff, 1), num, "TEMP");
    d_glass(&d, 115, 220, 160, 72, 22, 1, 1);
    d_icon(&d, "alarm", 129, 239.5f, 1);
    d_text(&d, TK_IT700, 11, 145, 238 + 11, "ALARM", .33f, 0, W(.7f), 0);
    char at[16] = "No alarm";                  /* P2y: the kernel's next alarm (sh_alarm), not the concept's */
    if (ALM.on) { at[0] = (char)('0' + ALM.h / 10); at[1] = (char)('0' + ALM.h % 10); at[2] = ':';
                  at[3] = (char)('0' + ALM.m / 10); at[4] = (char)('0' + ALM.m % 10); at[5] = 0; }
    d_text(&d, TK_IT700, 17, 129, 254 + 16, at, 0, 0, INK, 0);
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
    q->grp.ox = PT(x + 26);
    q->grp.oy = PT(y + 26);
    dc d = mk(&q->face, "q-face", x, y, 52, 52, 44, 1);
    d_glass(&d, x, y, 52, 52, 26, 1, 1);
    /* lit: the background is white (torch) or the Emergency red instead of the tint, the rest of the glass stays */
    d = mk(&q->facelit, "q-lit", x, y, 52, 52, 44, 1);
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
    if (!q->ang || !q->cov) n = 0;
    for (int j = 0; n && j < q->ring.bmp.h; j++)
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

/* ---- the Systems list ------------------------------------------------------------------------------------------ */
#define FACE_MG 44.0f            /* the margin around a card's face (its shadow) */
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
static float max_scroll(void) { return EMG ? 0 : tk_maxf(0, content_h() - LIST_H); }

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
    dc d = mk(&k->closed, "card", 14, top, 362, 76, 44, 0);
    if (stack(g)) {
        ghost(d.b, d.ox, d.oy, 33, top + 56, 324, .4f);
        ghost(d.b, d.ox, d.oy, 23, top + 50, 344, .7f);
    }
    tk_glass_local(d.b, d.b, PT(14) - d.ox, PT(top) - d.oy, PT(362), PT(68), PT(26));
    d_glass(&d, 14, top, 362, 68, 26, 0, 1);
    head_content(&d, g, top);
    /* open: made when the card opens, freed when it has closed (build_open / free_open) */
    tk_layer_init(&k->open, "card-open");
    d = mk(&k->chev, "chev", k->chx, top + 27, 14, 14, 0, 0);
    d_icon(&d, "chev", k->chx, top + 27, 1);
    k->chev_t = 1;
    tk_bmp_new(&k->chev0, k->chev.bmp.w, k->chev.bmp.h);
    tk_bmp_copy(&k->chev0, 0, 0, &k->chev.bmp, 0, 0, k->chev.bmp.w, k->chev.bmp.h);
    tk_layer_init(&k->members, "members");
    k->members.clip = 1;
    k->open.hidden = 1;
    k->chev.clip = 1;
}
/* an opening card's face (at the open height: a 3-slice while it grows), its head and its member rows */
static void build_open(int g)
{
    card_t *k = &K[g];
    const grp_t *gr = &G[g];
    if (k->face.bmp.px) return;
    float top = LIST_TOP + k->y;
    dc d = mk(&k->face, "face", 14, top, 362, 68 + k->full, FACE_MG, 0);
    d_glass(&d, 14, top, 362, 68 + k->full, 26, 0, 1);
    k->face.slice = 1;
    k->face.slice_s0 = PT(40 + FACE_MG);
    k->face.slice_s1 = PT(30 + FACE_MG);
    d = mk(&k->head, "head", 14, top, 362, 68, 2, 0);
    head_content(&d, g, top);
    k->members.child = 0;
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
    /* their half copies now (at the tap), not in the first frame of the card's spring */
    tk_layer_prepare(&k->face);
    tk_layer_prepare(&k->head);
    for (int i = 0; i < gr->n; i++) tk_layer_prepare(&k->rows[i]);
    k->open.child = 0;
    tk_layer_add(&k->open, &k->face);
    tk_layer_add(&k->open, &k->head);
    tk_layer_add(&k->open, &k->members);
}
static void free_open(int g)
{
    card_t *k = &K[g];
    if (!k->face.bmp.px) return;
    relayer(&k->face);
    relayer(&k->head);
    for (int i = 0; i < G[g].n; i++) relayer(&k->rows[i]);
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
}

/* Emergency's list (the concept's renderEmergency): four cards and Exit Emergency; made on entering, freed on
 * leaving (they are about 15 MB) */
static tk_layer ecard[5], empill;
typedef struct { const char *icon, *t, *s, *act; } ecard_t;
static const ecard_t EC[4] = {
    { "e46-heat", "Survival Heat", "Warms your hands \xc2\xb7 stops at a 65 \xc2\xb0" "C battery", "Survival Heat" },
    { "e46-power", "Power Saving", "Dims the screen and turns off radios", "Power Saving" },
    { "e46-sos", "Emergency SOS", "Siren, flashing light and your medical card", "SOS" },
    { "e46-medical", "Medical Card", "Shown here without unlocking", "Medical Card" } };
static void build_ecards(void)
{
    /* the four cards in one layer (each card's backdrop is what is drawn before it, as in the list) */
    dc d = mk(&ecard[0], "ecards", 14, LIST_TOP + 10, 362, 78 * 3 + 68, FACE_MG, 0);
    for (int i = 0; i < 4; i++) {
        float top = LIST_TOP + 10 + 78 * i;
        tk_glass_local(d.b, d.b, PT(14) - d.ox, PT(top) - d.oy, PT(362), PT(68), PT(26));
        d_glass(&d, 14, top, 362, 68, 26, 0, 1);
        d_icon(&d, EC[i].icon, 25, top + 11, 1);
        /* b: 19 / 800 / -.5, line-height 1.1 (half-leading floored); .sub 13.5, .82 */
        d_text(&d, TK_IT800, 19, 83, top + 12.55f + tk_floorf(tk_ascent(TK_IT800, PT(19)) / TK_PT + .5f), EC[i].t, -.5f, 0, INK, 0);
        d_text(&d, TK_IT400, 13.5f, 83, top + 37.44f + tk_floorf(tk_ascent(TK_IT400, PT(13.5f)) / TK_PT + .5f), EC[i].s, 0, 0, W(.82f), 0);
        float col = tk_maxf(249, tk_maxf(tw(TK_IT800, 19, EC[i].t, -.5f, 0), tw(TK_IT400, 13.5f, EC[i].s, 0, 0)));
        d_icon(&d, "chev", 83 + col + 12, top + 27, 1);
    }
    float top = LIST_TOP + 10 + 78 * 4 - 10 + 12, w = 14 + tw(TK_IT600, 13, "Exit Emergency", 0, 0) + 14, x = 195 - w / 2;
    d = mk(&ecard[4], "eexit", x, top, w, 32, 44, 1);
    d_glass(&d, x, top, w, 32, 16, 1, 1);
    d_text(&d, TK_IT600, 13, x + 14, top + 8 + 13, "Exit Emergency", 0, 0, INK, 0);
    tk_layer_prepare(&ecard[0]);
    tk_layer_prepare(&ecard[4]);
}
static void free_ecards(void) { for (int i = 0; i < 5; i++) { relayer(&ecard[i]); ecard[i].hidden = 1; } }
static int cards_pending;       /* vendor cards still to bake again after Emergency (one per idle turn) */
static void build_card(int g);
static void free_open(int g);
static void free_vendor_cards(void)
{
    for (int g = 0; g < NG; g++) {
        free_open(g);
        relayer(&K[g].closed);
        relayer(&K[g].chev);
        tk_bmp_free(&K[g].chev0);
    }
    cards_pending = 0;
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
        k->face.slice_h = PT(h + 2 * FACE_MG);
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
    for (int g = 0; g < NG; g++) {
        if (EMG) K[g].closed.hidden = K[g].open.hidden = 1;
        K[g].chev.hidden = EMG;
    }
    etool.hidden = EMG;
    ecard[0].hidden = ecard[4].hidden = !EMG;
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
    if (k->isopen) build_open(g);
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

static tk_layer hparts[8], hsearch[3], pages;
static void build_home_sb(void)
{
    relayer(&hparts[0]);
    dc d = mk(&hparts[0], "home-sb", 0, 0, 390, 48, 0, 0);
    char hm[8];
    fmt_hm(hm);
    status_bar(&d, hm);
}

/* ---- the home (P1: the concept's first page, drawn once; P2 builds the real one) --------------------------- */
static const char *const P2K[8] = { "contacts", "files", "light", "battery", "terminal", "browser", "medical", "sos" };
static const char *const P2E[8] = { "heat", "power", "sos", "medical", "light", "battery", "contacts", "files" };
static void build_home_w(void)
{
    relayer(&hparts[1]);
    dc d = mk(&hparts[1], "home-w", 0, 50, 390, 200, 44, 1);
    d_glass(&d, 24, 60, 158, 158, 24, 0, 1);
    d_glass(&d, 208, 60, 158, 158, 24, 0, 1);
    /* Battery: .lab, .big, three rows */
    if (EMG) {
        /* Emergency: the battery glyph amber (#ffb547) */
        const tk_bmp *ic = tk_icon("home-batt");
        if (ic) tk_blit_tinted(d.b, ic, (int)tk_floorf(PT(38) - tk_icon_pad("home-batt") - d.ox + .5f),
                               (int)tk_floorf(PT(74 + 1.5f) - tk_icon_pad("home-batt") - d.oy + .5f), tk_hex(0xffb547, 1));
    } else
        d_icon(&d, "home-batt", 38, 74 + 1.5f, 1);
    d_text(&d, TK_IT700, 11.5f, 56, base_of(TK_IT700, 11.5f, 74, 0), "Galaxy S9", .345f, 0, W(.7f), 0);
    char big[8];
    int bn0 = put_dec(big, ST.pct);
    big[bn0] = '%'; big[bn0 + 1] = 0;
    d_text(&d, TK_IT800, 38, 38, base_of(TK_IT800, 38, 74 + 14 + 4, 0), big, -1.2f, TK_TNUM, INK, 0);
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
}
static void build_home_apps2(void)
{
    /* page 2: the second grid (top 62) */
    {
        static const char *P2N_[8] = { "Contacts", "Files", "Flashlight", "Battery", "Terminal", "Browser", "Medical Card", "SOS" };
        static const char *P2EN[8] = { "Survival Heat", "Power Saving", "SOS", "Medical Card", "Flashlight", "Battery", "Contacts", "Files" };
        const char *const *P2 = EMG ? P2E : P2K, *const *P2N = EMG ? P2EN : P2N_;
        relayer(&hparts[4]);
        /* drawn as if on screen, then moved one page to the right (layers never extend past the panel's sides) */
        dc d = mk(&hparts[4], "home-apps2", 0, 55, 390, 220, 4, 0);
        for (int i = 0; i < 8; i++) {
            float cx = 22 + 86.5f * (i % 4) + 43.25f, y = 62 + 103 * (i / 4);
            char n[40] = "a62-";
            tk_memcpy(n + 4, P2[i], tk_strlen(P2[i]) + 1);
            d_icon(&d, n, cx - 31, y, 1);
            float w = tw(TK_IT500, 12, P2N[i], 0, 0);
            d_text(&d, TK_IT500, 12, cx - w / 2, y + 68 + 12, P2N[i], 0, 0, INK, 1);
        }
        hparts[4].x = hparts[4].bx = hparts[4].x + PT(390);
    }
}
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
        float cx = 22 + 86.5f * (i % 4) + 43.25f, y = 258 + 103 * (i / 4);
        char n[40] = "a62-";
        tk_memcpy(n + 4, P1[i], tk_strlen(P1[i]) + 1);
        d_icon(&d, n, cx - 31, y, 1);
        float w = tw(TK_IT500, 12, P1N[i], 0, 0);
        d_text(&d, TK_IT500, 12, cx - w / 2, y + 68 + 12, P1N[i], 0, 0, INK, 1);
    }
    build_home_apps2();
    /* the Search pill, three ways: the word (at rest on a page), the page dots (moving, either page lit) */
    {
        float sw = 15 + 13 + 6 + tw(TK_IT600, 13, "Search", 0, 0) + 15, sx = 195 - sw / 2;
        d = mk(&hsearch[0], "search", sx, 640, sw, 30, 44, 1);
        d_glass(&d, sx, 640, sw, 30, 15, 1, 1);
        d_icon(&d, "search", sx + 15, 640 + 8.5f, 1);
        d_text(&d, TK_IT600, 13, sx + 34, 640 + 7 + 13, "Search", 0, 0, INK, 1);
        for (int k = 0; k < 2; k++) {
            d = mk(&hsearch[1 + k], "search-dots", 161.5f, 640, 67, 27, 44, 1);
            d_glass(&d, 161.5f, 640, 67, 27, 13.5f, 1, 1);
            d_icon(&d, "search", 161.5f + 15, 640 + 7, 1);
            d_rrect(&d, 195.5f, 650.5f, 6, 6, 3, k == 0 ? W(1) : W(.4f));
            d_rrect(&d, 207.5f, 650.5f, 6, 6, 3, k == 1 ? W(1) : W(.4f));
        }
        for (int k = 0; k < 3; k++) tk_a8_free(&hsearch[k].glass);
        hsearch[1].hidden = hsearch[2].hidden = 1;
    }
    d = mk(&hparts[3], "home-dock", 0, 650, 390, 152, 0, 1);
    d_glass(&d, 12, 688, 366, 96, 36, 0, 1);
    for (int i = 0; i < 4; i++) {
        float x = 22 + 86.5f * i + 12.25f;
        char n[40] = "a62-";
        tk_memcpy(n + 4, DK[i], tk_strlen(DK[i]) + 1);
        d_icon(&d, n, x, 705, 1);
        if (0) {                                   /* P2y: no badges (no calls or messages to count) */
            const char *bn = i ? "2" : "1";
            float bw = tk_maxf(20, tw(TK_IT700, 12, bn, 0, 0) + 10);
            d_rrect(&d, x + 60 - bw, 699, bw, 20, 10, tk_hex(0xff3b30, 1));
            d_text(&d, TK_IT700, 12, x + 60 - bw / 2 - tw(TK_IT700, 12, bn, 0, 0) / 2, base_of(TK_IT700, 12, 699, 20), bn, 0, 0, INK, 0);
        }
    }
    /* the parts stay separate layers under one group (they do not overlap, so the group's opacity per part is
     * exact); #home has will-change: opacity, a backdrop root in Chrome: its glass shows no wallpaper blur */
    for (int i = 0; i < 5; i++) tk_a8_free(&hparts[i].glass);
}

static float focus_sy;

/* ---- the focus sheet --------------------------------------------------------------------------------------- */
static tk_layer focus, funder, scrim, fgrp, fcard, ffill, flabel, fthumb;
/* a closed sheet keeps nothing of its own (about 19 MB); the glass faces made once stay */
static void free_focus(void)
{
    relayer(&scrim); relayer(&funder); relayer(&fcard); relayer(&ffill); relayer(&flabel); relayer(&fthumb);
    focus.child = 0;
}
static void build_focus(const sys_t *s, const grp_t *gr)
{
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
        tk_bmp_down2(&scrim.bmp, &C->fb);
        tk_bmp_down2(&q, &scrim.bmp);
        tk_bmp_blur(&q, PT(22) / 4);
        tk_bmp_satbright(&q, 1.3f, 1);
        /* the tint over it at this size (a constant over a smooth image: the same after the bilinear up) */
        tk_bmp_tint(&q, TK_RGBA(4, 8, 26, .28f));
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
        dc d = mk(L, "fcard", 14, 196, 362, 267, 44, 0);
        if (!pass) {
            /* straight from the half-resolution scrim (it is already a blur) */
            tk_glass_from_half(d.b, (int)d.ox, (int)d.oy, &scrim.bmp, PT(14) - d.ox, PT(196) - d.oy, PT(362), PT(267), PT(32));
        }
        /* the card's glass (geometry only: the same for every system) is baked once and kept */
        {
            static tk_layer face;
            if (!face.bmp.px) {
                dc fd = mk(&face, "fcard-face", 14, 196, 362, 267, 44, 0);
                d_glass(&fd, 14, 196, 362, 267, 32, 0, 1);
            }
            tk_blit(d.b, &face.bmp, (int)(face.x - d.ox), (int)(face.y - d.oy), 1);
        }
        lap_us("focus: the card's backdrop and glass", tk_us() - tb0);
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
        /* the slider: dark glass whose backdrop is the card under it; its glass kept per position (2 or 3 facts) */
        tk_glass_local(d.b, d.b, PT(34) - d.ox, PT(sy) - d.oy, PT(322), PT(62), PT(31));
        {
            static tk_layer sface[2];
            tk_layer *sf = &sface[nf == 3];
            if (!sf->bmp.px) {
                dc fd = mk(sf, "slider-face", 34, sy, 322, 62, 44, 0);
                d_glass(&fd, 34, sy, 322, 62, 31, 1, 1);
            }
            tk_blit(d.b, &sf->bmp, (int)(sf->x - d.ox), (int)(sf->y - d.oy), 1);
        }
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
    tk_flatten(&lockg, &lockflat.bmp, &lockflat.glass, .5f);
    flat_ver = lock_ver;
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
    pages.x = PT(x);
    float p = -x / 390;
    float fr = p - tk_floorf(p + .5f);
    int moving = tk_fabsf(fr) > .02f || page_dragging;
    hsearch[0].hidden = moving;
    hsearch[1].hidden = !moving || p >= .5f;
    hsearch[2].hidden = !moving || p < .5f;
}


/* ---- the Control Centre (the concept's #cc): pulled down from home's top-right corner -------------------------
 * A scrim (home blurred 26 pt, saturated 1.3, under rgba(4, 8, 24, .3)) and the grid: the connectivity tile (air,
 * cellular, Wi-Fi, Bluetooth), brightness and volume sliders, eight tiles. The grid's glass, backdrops and contents
 * are one bitmap baked while home is at rest (and again when a tile changes); the slider levels and their glyphs
 * are layers over it. setCC: scrim min(1, p), grid opacity min(1, 1.3 p), translateY((1 - p) x -30) scale(.86 +
 * .14 p) about 85 % 0 of the grid; spring cc 400 / 36; open past .35 or faster than .4 pt/ms. */
static tk_layer ccl, ccscrim, ccsb, ccg, ccgrid, cclvl[2], ccic[2];
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
static const float CCX[4] = { 24, 114.66f, 205.31f, 295.97f };
static void cc_tile(dc *d, float x, float y, float w, float h, float r, int on)
{
    tk_glass_from_half(d->b, (int)d->ox, (int)d->oy, &ccscrim.bmp, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r));
    d_glass(d, x, y, w, h, r, 0, 1);
    if (on) d_rrect(d, x, y, w, h, r, W(1));
}
static void cc_glyph(dc *d, const char *name, float tx, float ty, float w, float h)
{
    d_icon(d, name, tx + (70 - w) / 2, ty + (70 - h) / 2, 1);
}
static void cc_label(dc *d, const char *t, float tx, float ty)
{
    float w = tw(TK_IT600, 10.5f, t, 0, 0);
    d_text(d, TK_IT600, 10.5f, tx + 35 - w / 2, ty + 74 + 10, t, 0, 0, W(.8f), 0);
}
static void cc_link(void)
{
    tk_layer *g[] = { &ccgrid, &cclvl[0], &ccic[0], &cclvl[1], &ccic[1] };
    link(&ccg, g, 5);
    tk_layer *k[] = { &ccscrim, &ccsb, &ccg };
    link(&ccl, k, 3);
}
static void build_cc_grid(void)
{
    relayer(&ccgrid);
    dc d = mk(&ccgrid, "cc-grid", 24, 92, 342, 346, 44, 0);
    /* the connectivity tile and its four circles (air, cell / Wi-Fi, Bluetooth) */
    cc_tile(&d, 24, 92, 160.66f, 156, 28, 0);
    static const char *CI[4] = { "cc-air", "cc-cell", "cc-wifi", "cc-bt" };
    static const float CW[4][2] = { { 20, 20 }, { 20, 18 }, { 22, 16 }, { 14, 20 } };
    int ons[4] = { CCS.air, cell_running(), CCS.wifi, CCS.bt };
    for (int i = 0; i < 4; i++) {
        float cx = i & 1 ? 112.48f : 44.16f, cy = i >> 1 ? 177 : 111;
        d_rrect(&d, cx, cy, 52, 52, 26, ons[i] ? tk_hex(0x6fb3ff, 1) : W(.18f));
        d_icon(&d, CI[i], cx + (52 - CW[i][0]) / 2, cy + (52 - CW[i][1]) / 2, 1);
    }
    /* the sliders' glass (their level and glyph are layers above) */
    cc_tile(&d, 205.31f, 92, 70, 156, 24, 0);
    cc_tile(&d, 295.97f, 92, 70, 156, 24, 0);
    /* row 3: Flashlight, Rotation, Emergency, Saving; row 4: Timer, Calculator, Mirror, Camera */
    int lit = Q[0].lit;
    cc_tile(&d, CCX[0], 264, 70, 70, 22, lit); cc_glyph(&d, lit ? "cc-light-on" : "cc-light", CCX[0], 264, 16, 24); cc_label(&d, "Flashlight", CCX[0], 264);
    cc_tile(&d, CCX[1], 264, 70, 70, 22, CCS.rot); cc_glyph(&d, CCS.rot ? "cc-rot-on" : "cc-rot", CCX[1], 264, 22, 22); cc_label(&d, "Rotation", CCX[1], 264);
    cc_tile(&d, CCX[2], 264, 70, 70, 22, 0);
    if (EMG) d_rrect(&d, CCX[2], 264, 70, 70, 22, tk_hex(0xff5a4e, 1));
    cc_glyph(&d, "cc-flame", CCX[2], 264, 20, 24); cc_label(&d, "Emergency", CCX[2], 264);
    cc_tile(&d, CCX[3], 264, 70, 70, 22, CCS.save); cc_glyph(&d, CCS.save ? "cc-save-on" : "cc-save", CCX[3], 264, 24, 14); cc_label(&d, "Saving", CCX[3], 264);
    cc_tile(&d, CCX[0], 350, 70, 70, 22, 0); cc_glyph(&d, "cc-timer", CCX[0], 350, 22, 22); cc_label(&d, "Timer", CCX[0], 350);
    cc_tile(&d, CCX[1], 350, 70, 70, 22, 0); cc_glyph(&d, "a30-calc", CCX[1], 350, 30, 30);
    cc_tile(&d, CCX[2], 350, 70, 70, 22, 0); cc_glyph(&d, "cc-wifi2", CCX[2], 350, 22, 22); cc_label(&d, "Mirror", CCX[2], 350);
    cc_tile(&d, CCX[3], 350, 70, 70, 22, 0); cc_glyph(&d, "a30-camera", CCX[3], 350, 30, 30);
    tk_layer_prepare(&ccgrid);
}
static void cc_levels(void)
{
    for (int i = 0; i < 2; i++) {
        float x = i ? 295.97f : 205.31f, v = CCS.lvl[i];
        cclvl[i].clip_r = (tk_rect){ PT(x), PT(92 + 156 * (1 - v)), PT(70), PT(156 * v) };
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
        tk_bmp_down2(&ccscrim.bmp, &C->fb);
        tk_bmp_down2(&q, &ccscrim.bmp);
        tk_bmp_blur(&q, PT(26) / 4);
        tk_bmp_satbright(&q, 1.3f, 1);
        tk_bmp_tint(&q, TK_RGBA(4, 8, 24, .3f));
        tk_bmp_up(&ccscrim.bmp, &q, 0, 0, 2);
        tk_bmp_free(&q);
        ccscrim.sx = ccscrim.sy = 2;
        ccscrim.opaque = 1;
    }
    if (!ccsb.bmp.px) {
        dc d = mk(&ccsb, "cc-status", 0, 0, 390, 48, 2, 0);
        status_bar(&d, carrier);
    }
    if (!cclvl[0].bmp.px) {
        for (int i = 0; i < 2; i++) {
            float x = i ? 295.97f : 205.31f;
            dc l = mk(&cclvl[i], "cc-level", x, 92, 70, 156, 0, 0);
            d_rrect(&l, x, 92, 70, 156, 24, W(1));
            cclvl[i].clip = 1;
            dc g = mk(&ccic[i], "cc-glyph", x + 24, 212, 22, 22, 1, 0);
            d_icon(&g, i ? "cc-vol" : "cc-sun", x + 24, 212, 1);
        }
    }
    build_cc_grid();
    tk_layer_init(&ccg, "cc-grid-group");
    ccg.ox = PT(24 + .85f * 342);
    ccg.oy = PT(92);
    cc_link();
    cc_levels();
    CCS.built = 1;
}
/* an open app needs the room (its page is a whole screen): the Control Centre's snapshot and grid go, home's idle
 * time builds them again */
static void free_cc(void)
{
    if (!CCS.built || CCS.on) return;
    relayer(&ccscrim);
    relayer(&ccgrid);
    CCS.built = 0;
}
static void set_cc(float p)
{
    CCS.p = p;
    ccscrim.alpha = tk_clampf(p, 0, 1);
    ccg.alpha = tk_clampf(p * 1.3f, 0, 1);
    ccg.sx = ccg.sy = .86f + .14f * p;
    ccg.y = PT((1 - p) * -30) / ccg.sy;
    ccl.hidden = !CCS.on;
}
static void cc_close(void)
{
    CCS.closing = 1;
    tk_spring_start(&sp_cc, CCS.p, 0, 0, TK_SPR_CC);
}
/* a tap on the open Control Centre: a tile, a circle, or outside (close); 1 if it was on the grid */
static void toast(const char *msg);
static int cc_tap(float x, float y)
{
    float ex = 0, ey = 0;
    (void)ex; (void)ey;
    if (x >= 24 && x < 184.66f && y >= 92 && y < 248) {
        int i = (x >= 104) + 2 * (y >= 170);
        if (i == 1) return 1;                /* the modem: a hold (G_CCCELL) */
        /* P2y: no switch pretends: Airplane, Wi-Fi and Bluetooth have nothing to switch in Next yet */
        toast(i == 0 ? "Airplane mode isn't in this version yet" : i == 2 ? "Wi-Fi isn't in this version yet" : "Bluetooth isn't available yet");
        return 1;
    } else if (y >= 264 && y < 334) {
        int c = x < 94 ? 0 : x < 184.66f ? 1 : x < 275.31f ? 2 : 3;
        if (c == 0) { Q[0].lit = !Q[0].lit; Q[0].facelit.hidden = Q[0].iconlit.hidden = !Q[0].lit; Q[0].face.hidden = Q[0].icon.hidden = Q[0].lit; sh_torch(Q[0].lit); lock_ver++; }
        else if (c == 1) { toast("The screen doesn't rotate yet"); return 1; }
        else if (c == 3) { toast("Power Saving isn't in this version yet"); return 1; }
        else return 1;                       /* Emergency: a hold (G_CCEM) */
    } else if (y >= 350 && y < 420 && x >= 24 && x < 366) {
        toast("Not in this version yet");    /* Timer, Calculator, Mirror, Camera: the apps come later */
        return 1;
    } else
        return 0;
    build_cc_grid();
    cc_link();
    return 1;
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
    { "photos", "Photos", 0x1d2440, 1, { { "", 1, { { "", "" } } } } },
    { "music", "Music", 0x1d2440, 1, { { "", 1, { { "No music on this phone", "" } } } } },
    { "notes", "Notes", 0x1d2440, 1, { { "", 1, { { "No notes yet", "" } } } } },
    { "calc", "Calculator", 0x1d2440, 1, { { "", 1, { { "Not in this version yet", "" } } } } },
    { "contacts", "Contacts", 0x1d2440, 1, { { "", 1, { { "", "" } } } } },
    { "files", "Files", 0x1d2440, 1, { { "", 1, { { "Not in this version yet", "" } } } } },
    { "light", "Flashlight", 0x1d2440, 1, { { "", 2, { { "Flashlight", "@torch" }, { "Hold the torch button on the lock screen", "" } } } } },
    { "battery", "Battery", 0x1d2440, 1, { { "Now", 3, { { "Charge", "@pct" }, { "Charging", "@chg" }, { "Battery temperature", "@temp" } } } } },
    { "terminal", "Terminal", 0x1d2440, 1, { { "", 1, { { "Linux apps run in Starbite OS", "" } } } } },
    { "browser", "Browser", 0x2f7bf6, 1, { { "", 1, { { "The browser needs the internet", "" } } } } },
    { "medical", "Medical Card", 0x1d2440, 1, { { "", 1, { { "Nothing is stored on this phone yet", "" } } } } },
    { "sos", "SOS", 0x1d2440, 1, { { "", 2, { { "Call 112", "Needs a SIM" }, { "Siren and light", "In Starbite OS" } } } } },
    { "heat", "Survival Heat", 0x1d2440, 1, { { "", 1, { { "Survival Heat runs in Starbite OS", "" } } } } },
    { "power", "Power Saving", 0x1d2440, 1, { { "", 1, { { "Not in this version yet", "" } } } } },
    { "phone", "Phone", 0x16b34a, 1, { { "", 1, { { "", "" } } } } },
    { "messages", "Messages", 0x16b34a, 1, { { "", 1, { { "", "" } } } } },
    { "settings", "Settings", 0x55555a, 3, { { "", 3, { { "Wi-Fi", "Not in this version" }, { "Mobile Data", "No SIM" }, { "Battery", "@pct" } } },
                                           { "", 4, { { "Display & Brightness", "@bright" }, { "Bluetooth", "Not available" }, { "Airplane Mode", "Off" }, { "Date & Time", "Phone clock" } } },
                                           { "About", 3, { { "Model", "Galaxy S9 (SM-G960F)" }, { "Software", "@ver" }, { "Memory", "4 GB" } } } } },
};
#define NAPPS ((int)(sizeof APPS / sizeof APPS[0]))
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
/* greedy word wrap into at most 4 lines of maxw; returns the line count, the widest line in *wmax */
static int wrap(char lines[4][96], int f, float size, const char *s, float maxw, float *wmax)
{
    int nl = 0, i = 0;
    *wmax = 0;
    while (s[i] && nl < 4) {
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
            last = e;
            if (!s[e]) break;
            k = e + 1;
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
/* Liquid Glass over the app's own ground (#0a0f1f) and what is drawn so far under it (Chrome's backdrop) */
static void d_lg_app(dc *d, float x, float y, float w, float h, float r, int dark)
{
    float bx = PT(x) - d->ox, by = PT(y) - d->oy;
    if (by + PT(h + 48) < 0 || by - PT(8) > d->b->h) return;          /* its shadow included */
    int X = (int)tk_floorf(bx), Y = (int)tk_floorf(by), Wd = (int)(bx + PT(w) + .999f) - X, Hd = (int)(by + PT(h) + .999f) - Y;
    tk_bmp t;
    if (!tk_bmp_new(&t, Wd, Hd)) return;
    tk_fill_rect_raw(&t, 0, 0, Wd, Hd, 0xff0a0f1fu);
    tk_blit(&t, d->b, -X, -Y, 1);
    tk_bmp_blur_sat(&t, PT(14), 1.9f, 1.06f);
    tk_glass_local_put(d->b, &t, X, Y, bx, by, PT(w), PT(h), PT(r));
    tk_bmp_free(&t);
    tk_glass_bake(d->b, 0, bx, by, PT(w), PT(h), PT(r), dark, 1);
}
/* the same over the bare ground (nothing drawn under it): the blur of a flat colour is that colour, so no backdrop
 * buffer is needed (a card over the dimmed screen: its 4.6 MB backdrop refused on a full heap) */
static void d_lg_flat(dc *d, float x, float y, float w, float h, float r, int dark)
{
    tk_bmp g;
    uint32_t c = 0xff0a0f1fu;
    if (tk_bmp_new(&g, 1, 1)) {
        g.px[0] = c;
        tk_bmp_satbright(&g, 1.9f, 1.06f);
        c = g.px[0];
        tk_bmp_free(&g);
    }
    tk_paint p = tk_solid(TK_RGBA((float)(c >> 16 & 255), (float)(c >> 8 & 255), (float)(c & 255), 1));
    tk_fill_rrect(d->b, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r), &p, 1);
    tk_glass_bake(d->b, 0, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r), dark, 1);
}
static void d_h1(dc *d, const char *t) { d_text(d, TK_IT800, 34, 20, bl(TK_IT800, 34, 64), t, -.8f, 0, INK, 0); }
static tk_col ACCENT(void) { return tk_hex(0x6fb3ff, 1); }
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
        d_text(d, TK_IT400, 13.5f, 78, bl(TK_IT400, 13.5f, y + 33), e, 0, 0, W(.55f), 0);
    }
    if (rw > 0) {
        float rx = 358 - rw, rb = bl(TK_IT400, 16, y + 13 + (who - 20) / 2);
        d_text(d, TK_IT400, 16, rx, rb, right, 0, TK_TNUM, W(.55f), 0);
        if (dot) d_rrect(d, 358 - 9, rb - 9, 9, 9, 4.5f, tk_hex(0x2f7bf6, 1));
    }
    if (!last) d_rrect(d, 16, y + rh, 358, 1, 0, W(.08f));
    return rh + 1;
}
__attribute__((unused)) static void d_group(dc *d, float y, int n, int sub) { d_rrect(d, 16, y, 358, (sub ? 63.0f : 61.0f) * n - 1, 16, tk_hex(0x151c33, 1)); }

static const char *const KD[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };
static const char *const KL[12] = { "", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ", "", "+", "" };
static struct { int tab; char num[16]; int thread; } AS;        /* the phone's tab (0 fav .. 3 pad), the dialled number, the open thread */
static char cell_hint[48] = "No SIM \xc2\xb7 emergency calls only";

static void phone_tabs(dc *d)
{
    static const char *const TK[4] = { "fav", "rec", "con", "pad" };
    static const char *const TL[4] = { "Favourites", "Recents", "Contacts", "Keypad" };
    d_lg_app(d, 16, 718, 358, 62, 31, 1);
    for (int i = 0; i < 4; i++) {
        float bx = 16 + 89.5f * i;
        char n[16] = "tab-";
        int k = 4;
        for (const char *c = TK[i]; *c; c++) n[k++] = *c;
        if (i == AS.tab) { n[k++] = '-'; n[k++] = 'o'; n[k++] = 'n'; }
        n[k] = 0;
        d_icon(d, n, bx + 33.75f, 730.5f, 1);
        float w = tw(TK_IT600, 10.5f, TL[i], 0, 0);
        d_text(d, TK_IT600, 10.5f, bx + (89.5f - w) / 2, bl(TK_IT600, 10.5f, 754.5f), TL[i], 0, 0, i == AS.tab ? ACCENT() : W(.6f), 0);
    }
}
/* P2y: an honest empty state (iOS's ContentUnavailableView): a title and up to three lines, centred, grey */
static void d_empty(dc *d, float top, const char *title, const char *line)
{
    float w = tw(TK_IT700, 22, title, 0, 0);
    d_text(d, TK_IT700, 22, (390 - w) / 2, bl(TK_IT700, 22, top), title, 0, 0, W(.9f), 0);
    char ln[4][96];
    float wm;
    int n = line ? wrap(ln, TK_IT400, 15, line, 300, &wm) : 0;
    for (int k = 0; k < n && k < 3; k++) {
        float lw = tw(TK_IT400, 15, ln[k], 0, 0);
        d_text(d, TK_IT400, 15, (390 - lw) / 2, bl(TK_IT400, 15, top + 36 + 20 * k), ln[k], 0, 0, W(.5f), 0);
    }
}
static void page_contacts(dc *d)
{
    d_h1(d, "Contacts");
    d_empty(d, 330, "No Contacts", "Contacts can't be added on this phone yet.");
}
static void page_phone(dc *d)
{
    if (AS.tab == 3) {
        float w = tw(TK_IT400, 13, cell_hint, 0, 0);
        d_text(d, TK_IT400, 13, (390 - w) / 2, bl(TK_IT400, 13, 150), cell_hint, 0, 0, W(.5f), 0);
        for (int i = 0; i < 12; i++) {
            float x = 52 + 104.0f * (i % 3), y = 182 + 96.0f * (i / 3);
            d_rrect(d, x, y, 78, 78, 39, W(.14f));
            float dw = tw(TK_IT500, 32, KD[i], 0, 0);
            d_text(d, TK_IT500, 32, x + (78 - dw) / 2, y + 12 + tk_floorf(tk_ascent(TK_IT500, PT(32)) / TK_PT + .5f), KD[i], 0, 0, W(1), 0);
            if (KL[i][0]) {
                float lw = tw(TK_IT700, 10, KL[i], 1.4f, 0);
                d_text(d, TK_IT700, 10, x + (78 - lw) / 2, bl(TK_IT700, 10, y + 50), KL[i], 1.4f, 0, W(.75f), 0);
            }
        }
        d_rrect(d, 156, 566, 78, 78, 39, tk_hex(0x34c759, 1));
        d_icon(d, "call", 180, 590, 1);
    } else if (AS.tab == 1) {
        d_h1(d, "Recents");
        d_empty(d, 330, "No Recent Calls", cell_hint);
    } else if (AS.tab == 2) {
        page_contacts(d);
    } else {
        d_h1(d, "Favourites");
        d_empty(d, 330, "No Favourites", cell_hint);
    }
    phone_tabs(d);
}
typedef struct { const char *n, *m, *t; int unread; } thread_t;
static const thread_t THREADS[5] = {
    { "Aino Virtanen", "Are you bringing the phone tomorrow?", "21:02", 1 }, { "Mum", "Good luck with the exam!", "18:40", 1 },
    { "Mikko Laine", "That boot menu looks insane", "Yesterday", 0 }, { "Sara Niemi", "Ok see you at 9", "Monday", 0 },
    { "112 Info", "Emergency number test message", "Sunday", 0 } };
static void page_messages(dc *d)
{
    if (AS.thread < 0) {
        d_h1(d, "Messages");
        d_lg_app(d, 16, 117, 358, 44, 22, 1);
        d_icon(d, "search15", 30, 131.5f, 1);
        d_text(d, TK_IT400, 17, 53, 128 + 1 + tk_floorf(tk_ascent(TK_IT400, PT(17)) / TK_PT + .5f), "Search", 0, 0, tk_hex(0xa9a9a9, 1), 0);
        d_empty(d, 360, "No Messages", "Text messages need a SIM card. This phone doesn't send or receive any yet.");
        return;
    }
    const thread_t *th = &THREADS[AS.thread];
    d_icon(d, "back", 18, 53, 1);
    d_text(d, TK_IT400, 17, 32, bl(TK_IT400, 17, 53), "Messages", 0, 0, ACCENT(), 0);
    d_av(d, 169, 60, 52, th->n, 18);
    float w = tw(TK_IT600, 13, th->n, 0, 0);
    d_text(d, TK_IT600, 13, (390 - w) / 2, bl(TK_IT600, 13, 116), th->n, 0, 0, INK, 0);
    w = tw(TK_IT400, 12, "Today 20:55", 0, 0);
    d_text(d, TK_IT400, 12, (390 - w) / 2, bl(TK_IT400, 12, 142), "Today 20:55", 0, 0, W(.45f), 0);
    static const char *const AINO[3] = { "Hey! Did the big-core thing work?", "Getting closer, three tests passed tonight",
                                         "Are you bringing the phone tomorrow?" };
    static const char *const MUM[2] = { "Good luck with the exam!", "Thanks" };
    const char *const *msg = AS.thread == 0 ? AINO : AS.thread == 1 ? MUM : 0;
    int nm = AS.thread == 0 ? 3 : AS.thread == 1 ? 2 : 1;
    float y = 167;
    for (int i = 0; i < nm; i++) {
        const char *s = msg ? msg[i] : "Hi!";
        int out = i % 2 == 1;
        char ln[4][96];
        float wmax;
        int n = wrap(ln, TK_IT400, 16, s, 267.88f, &wmax);
        int heart = AS.thread == 1 && i == 1;
        if (heart) wmax += tw(TK_IT400, 16, " ", 0, 0) + 20;
        float bw = (n > 1 ? 267.88f : wmax) + 26, bh = 18 + 20.8f * n, bx = out ? 376 - bw : 14;
        tk_corner = (tk_r4){ PT(20), PT(20), PT(out ? 6 : 20), PT(out ? 20 : 6) };
        tk_use_r4 = 1;
        d_rrect(d, bx, y, bw, bh, 20, tk_hex(out ? 0x2f7bf6 : 0x262c3e, 1));
        tk_use_r4 = 0;
        for (int k = 0; k < n; k++) d_text(d, TK_IT400, 16, bx + 13, bl(TK_IT400, 16, y + 9 + 20.8f * k), ln[k], 0, 0, INK, 0);
        if (heart) d_icon(d, "heart", bx + 13 + tw(TK_IT400, 16, "Thanks ", 0, 0), y + 9, 1);
        y += bh + 5;
    }
    d_lg_app(d, 12, 734, 322, 40, 20, 1);
    d_text(d, TK_IT400, 16, 28, 734 + 10 + tk_floorf(tk_ascent(TK_IT400, PT(16)) / TK_PT + .5f), "Text message", 0, 0, tk_hex(0xa9a9a9, 1), 0);
    d_rrect(d, 342, 736, 36, 36, 18, tk_hex(0x2f7bf6, 1));
    d_icon(d, "send", 352, 746, 1);
}
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
    d_empty(d, 330, "No Photos", "The camera doesn't work on this phone yet, so there is nothing here.");
}
static void page_camera(dc *d)
{
    d_h1(d, "Camera");
    d_empty(d, 330, "Camera Not Available", "The Galaxy S9's camera needs its driver first.");
}
/* ---- P2z: the Clock app: the live time, a stopwatch, a timer that vibrates (no sound). The page's static part is
 * drawn in the strips; the stopwatch's and the timer's digits and the buttons are three small layers, redrawn
 * only when their text changes (the platform asks sh_next_update when to come back) ---- */
static int is_app(const char *k);
static tk_layer apsw, aptm, apctl;
static struct { int sw_run, laps, tm_run, sw_ds, tm_s, ctl_ver, ctl_drawn; float sw_t0, sw_acc, lap_at, tm_end; } CLK = { 0, 0, 0, -1, -1, 1, 0, 0, 0, 0, 0 };
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
static void page_clock(dc *d)
{
    d_h1(d, "Clock");
    char hm[8], dt[48];
    fmt_hm(hm);
    float w = tw(TK_IT600, 64, hm, 0, TK_TNUM);
    d_text(d, TK_IT600, 64, (390 - w) / 2, bl(TK_IT600, 64, 112), hm, 0, TK_TNUM, INK, 0);
    const char *wd = WD[ST.wday % 7], *mo = MO[(ST.mon + 11) % 12];
    int n = (int)tk_strlen(wd);
    tk_memcpy(dt, wd, (size_t)n); dt[n++] = ' ';
    n += put_dec(dt + n, ST.day); dt[n++] = ' ';
    tk_memcpy(dt + n, mo, tk_strlen(mo) + 1);
    w = tw(TK_IT400, 15, dt, 0, 0);
    d_text(d, TK_IT400, 15, (390 - w) / 2, bl(TK_IT400, 15, 192), dt, 0, 0, W(.55f), 0);
    d_text(d, TK_IT400, 13, 36, bl(TK_IT400, 13, 236), "STOPWATCH", .52f, 0, W(.55f), 0);
    d_text(d, TK_IT400, 13, 36, bl(TK_IT400, 13, 462), "TIMER", .52f, 0, W(.55f), 0);
}
static void clk_digits(tk_layer *l, float y, const char *txt, const char *sub)
{
    relayer(l);
    dc d = mk(l, "clock-digits", 20, y, 350, 84, 0, 0);
    float w = tw(TK_IT400, 54, txt, 0, TK_TNUM);
    d_text(&d, TK_IT400, 54, (390 - w) / 2, bl(TK_IT400, 54, y), txt, 0, TK_TNUM, INK, 0);
    if (sub && sub[0]) {
        w = tw(TK_IT400, 14, sub, 0, TK_TNUM);
        d_text(&d, TK_IT400, 14, (390 - w) / 2, bl(TK_IT400, 14, y + 66), sub, 0, TK_TNUM, W(.55f), 0);
    }
    l->hidden = 0;
}
static void clk_button(dc *d, float x, float y, float wd, float h, const char *t, tk_col bg, tk_col fg)
{
    d_rrect(d, x, y, wd, h, h / 2, bg);
    float w = tw(TK_IT600, 16, t, 0, 0);
    d_text(d, TK_IT600, 16, x + (wd - w) / 2, bl(TK_IT600, 16, y + h / 2 - 10), t, 0, 0, fg, 0);
}
static void clk_controls(void)
{
    relayer(&apctl);
    dc d = mk(&apctl, "clock-buttons", 20, 344, 350, 270, 0, 0);
    float sw = clk_sw_ms(now_ms);
    clk_button(&d, 36, 350, 78, 78, CLK.sw_run ? "Lap" : "Reset", W(CLK.sw_run || sw > 0 ? .16f : .07f), W(CLK.sw_run || sw > 0 ? 1 : .4f));
    clk_button(&d, 276, 350, 78, 78, CLK.sw_run ? "Stop" : "Start", CLK.sw_run ? TK_RGBA(255, 69, 58, .28f) : TK_RGBA(48, 209, 88, .28f),
               CLK.sw_run ? tk_hex(0xff6961, 1) : tk_hex(0x4cd964, 1));
    if (CLK.tm_run) clk_button(&d, 125, 570, 140, 44, "Cancel", TK_RGBA(255, 159, 10, .25f), tk_hex(0xffb340, 1));
    else {
        static const char *const P[3] = { "1 min", "5 min", "10 min" };
        for (int i = 0; i < 3; i++) clk_button(&d, 24 + 125 * i, 570, 92, 44, P[i], W(.14f), INK);
    }
    apctl.hidden = 0;
    CLK.ctl_drawn = CLK.ctl_ver;
}
/* the digits and the buttons as of t (ms); 1 if anything was redrawn. A timer that ran out vibrates (sh_vibrate). */
static int clk_update(float t)
{
    int any = 0;
    if (CLK.tm_run && t >= CLK.tm_end) {
        CLK.tm_run = 0;
        CLK.ctl_ver++;
        sh_vibrate();
        toast("Timer done");
        any = 1;
    }
    if (!is_app("clock") || !AP.on) return any;
    float sw = clk_sw_ms(t);
    int ds = (int)(sw / 100);
    if (ds != CLK.sw_ds || !apsw.bmp.px) {
        CLK.sw_ds = ds;
        char b[16], l[32] = "";
        fmt_ms(b, sw, 1);
        if (CLK.laps) { tk_memcpy(l, "Lap ", 4); int n = 4 + put_dec(l + 4, CLK.laps); l[n++] = ' '; l[n++] = ' '; fmt_ms(l + n, CLK.lap_at, 1); }
        clk_digits(&apsw, 254, b, l);
        any = 1;
    }
    int ts = CLK.tm_run ? (int)((CLK.tm_end - t + 999) / 1000) : 0;
    if (ts != CLK.tm_s || !aptm.bmp.px) {
        CLK.tm_s = ts;
        char b[16];
        fmt_ms(b, ts * 1000.0f, 0);
        clk_digits(&aptm, 478, CLK.tm_run ? b : "00:00", CLK.tm_run ? "" : "Choose a time");
        any = 1;
    }
    if (CLK.ctl_drawn != CLK.ctl_ver || !apctl.bmp.px) { clk_controls(); any = 1; }
    if (any) relink();
    return any;
}
static void clk_tap(float x, float y)
{
    float t = now_ms;
    if (y >= 350 && y < 428) {
        if (x >= 276 && x < 354) {                       /* Start / Stop */
            if (CLK.sw_run) { CLK.sw_acc += t - CLK.sw_t0; CLK.sw_run = 0; }
            else { CLK.sw_t0 = t; CLK.sw_run = 1; }
            CLK.ctl_ver++;
        } else if (x >= 36 && x < 114) {                 /* Lap / Reset */
            if (CLK.sw_run) { CLK.laps++; CLK.lap_at = clk_sw_ms(t); CLK.sw_ds = -1; }
            else { CLK.sw_acc = 0; CLK.laps = 0; CLK.sw_ds = -1; CLK.ctl_ver++; }
        }
    } else if (y >= 570 && y < 614) {
        if (CLK.tm_run) { if (x >= 125 && x < 265) { CLK.tm_run = 0; CLK.ctl_ver++; } }
        else for (int i = 0; i < 3; i++)
            if (x >= 24 + 125 * i && x < 116 + 125 * i) {
                static const int M[3] = { 1, 5, 10 };
                CLK.tm_end = t + M[i] * 60000.0f;
                CLK.tm_run = 1;
                CLK.ctl_ver++;
            }
    }
    clk_update(t);
}
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
    return (int)best;
}
int sh_refresh(float t) { now_ms = t; return clk_update(t); }

/* ---- P2z: the Calculator (iOS's layout: the display right-aligned, the keys in five rows: grey functions, dark
 * digits, orange operators). The keys are the page; the display a small layer redrawn per key ---- */
static tk_layer apcalc;
static struct { double acc, cur; int op, entering, err, has_dot, ndig, frac; } CAL;
static const char *const CK[19] = { "AC", "\xc2\xb1", "%", "\xc3\xb7", "7", "8", "9", "\xc3\x97", "4", "5", "6", "\xe2\x88\x92",
                                    "1", "2", "3", "+", "0", ".", "=" };
static void calc_key_rect(int i, float *x, float *y, float *w)
{
    int r = i < 16 ? i / 4 : 4, c = i < 16 ? i % 4 : i == 16 ? 0 : i == 17 ? 2 : 3;
    *x = 16 + 92 * c;
    *y = 318 + 90 * r;
    *w = i == 16 ? 170 : 78;
}
static void page_calc(dc *d)
{
    for (int i = 0; i < 19; i++) {
        float x, y, w;
        calc_key_rect(i, &x, &y, &w);
        int fn = i < 3, op = (i % 4 == 3 && i < 16) || i == 18;
        d_rrect(d, x, y, w, 78, 39, op ? tk_hex(0xff9f0a, 1) : fn ? tk_hex(0xa5a5a5, 1) : tk_hex(0x333333, 1));
        float tw0 = tw(TK_IT500, 32, CK[i], 0, 0);
        float tx = i == 16 ? x + 31 : x + (78 - tw0) / 2;
        d_text(d, TK_IT500, 32, tx, bl(TK_IT500, 32, y + 19), CK[i], 0, 0, fn ? tk_hex(0x000000, 1) : INK, 0);
    }
}
static int calc_fmt(char *b, double v)
{
    if (v != v || v > 1e15 || v < -1e15) { tk_memcpy(b, "Error", 6); return 5; }
    int n = 0;
    if (v < 0) { b[n++] = '-'; v = -v; }
    double ip = (double)(long long)v, fp = v - ip;
    long long I = (long long)ip;
    char t[24];
    int k = 0;
    do { t[k++] = (char)('0' + I % 10); I /= 10; } while (I);
    int intd = k;
    while (k) b[n++] = t[--k];
    int room = 9 - intd;                    /* nine digits in all, as iOS's portrait display */
    if (room > 0) {
        long long f = 1;
        for (int i = 0; i < room; i++) f *= 10;
        long long F = (long long)(fp * (double)f + .5);
        if (F >= f) F = f - 1;
        int m = room;
        while (m > 0 && F % 10 == 0) { F /= 10; m--; }
        if (m > 0) {
            b[n++] = '.';
            char u[16];
            for (int i = m - 1; i >= 0; i--) { u[i] = (char)('0' + F % 10); F /= 10; }
            for (int i = 0; i < m; i++) b[n++] = u[i];
        }
    }
    b[n] = 0;
    return n;
}
static void build_calc(void)
{
    relayer(&apcalc);
    char b[32];
    if (CAL.err) tk_memcpy(b, "Error", 6);
    else {
        int n = calc_fmt(b, CAL.cur);
        if (CAL.entering && CAL.has_dot && CAL.frac == 0) { b[n++] = '.'; b[n] = 0; }
        else if (CAL.entering && CAL.has_dot && CAL.frac > 0) {   /* keep the typed trailing zeros */
            int have = 0;
            for (int i = 0; b[i]; i++) if (b[i] == '.') { have = (int)tk_strlen(b + i + 1); break; }
            if (!have) { b[n++] = '.'; }
            for (; have < CAL.frac && n < 30; have++) b[n++] = '0';
            b[n] = 0;
        }
    }
    float size = tk_strlen(b) > 7 ? 64 : 84;
    float w = tw(TK_IT400, size, b, 0, TK_TNUM);
    dc d = mk(&apcalc, "calc-display", 20, 186, 350, 118, 0, 0);
    d_text(&d, TK_IT400, size, 362 - w, bl(TK_IT400, size, 300 - size * 1.21f), b, 0, TK_TNUM, INK, 0);
    apcalc.hidden = 0;
    relink();
}
static double calc_apply(double a, double b, int op, int *err)
{
    switch (op) {
    case 1: if (b == 0) { *err = 1; return 0; } return a / b;
    case 2: return a * b;
    case 3: return a - b;
    case 4: return a + b;
    default: return b;
    }
}
static void calc_tap(float x, float y)
{
    int k = -1;
    for (int i = 0; i < 19; i++) {
        float kx, ky, kw;
        calc_key_rect(i, &kx, &ky, &kw);
        if (x >= kx && x < kx + kw && y >= ky && y < ky + 78) { k = i; break; }
    }
    if (k < 0) return;
    const char *c = CK[k];
    if (k == 0) { CAL.acc = CAL.cur = 0; CAL.op = 0; CAL.entering = CAL.err = CAL.has_dot = CAL.ndig = CAL.frac = 0; }
    else if (CAL.err) return;
    else if (k == 1) { CAL.cur = -CAL.cur; }
    else if (k == 2) { CAL.cur /= 100; CAL.entering = 0; }
    else if (c[0] >= '0' && c[0] <= '9' && !c[1]) {
        if (!CAL.entering) { CAL.cur = 0; CAL.entering = 1; CAL.has_dot = CAL.ndig = CAL.frac = 0; }
        if (CAL.ndig >= 9) return;
        int dg = c[0] - '0';
        if (CAL.has_dot) {
            double f = 1;
            for (int i = 0; i <= CAL.frac; i++) f /= 10;
            CAL.cur += (CAL.cur < 0 ? -dg : dg) * f;
            CAL.frac++;
        } else CAL.cur = CAL.cur * 10 + (CAL.cur < 0 ? -dg : dg);
        if (CAL.cur != 0 || CAL.has_dot) CAL.ndig++;
    } else if (k == 17) {
        if (!CAL.entering) { CAL.cur = 0; CAL.entering = 1; CAL.ndig = CAL.frac = 0; }
        CAL.has_dot = 1;
    } else {
        /* an operator or =: the pending operation first (left to right, as iOS's basic calculator chains) */
        if (CAL.op && (CAL.entering || k == 18)) CAL.cur = calc_apply(CAL.acc, CAL.cur, CAL.op, &CAL.err);
        CAL.acc = CAL.cur;
        CAL.op = k == 18 ? 0 : k == 3 ? 1 : k == 7 ? 2 : k == 11 ? 3 : 4;
        CAL.entering = 0;
    }
    build_calc();
}

/* P3b: the Flashlight app: one big switch (iOS's torch control), the same LED as the lock's button and the CC */
static void page_light(dc *d)
{
    d_h1(d, "Flashlight");
    int on = Q[0].lit;
    d_rrect(d, 135, 250, 120, 240, 34, on ? tk_hex(0xffffff, 1) : W(.16f));
    d_rrect(d, 135, on ? 250 : 370, 120, 120, 34, on ? tk_hex(0xffd60a, 1) : W(.32f));
    const char *t = on ? "On" : "Off";
    float w = tw(TK_IT600, 20, t, 0, 0);
    d_text(d, TK_IT600, 20, (390 - w) / 2, bl(TK_IT600, 20, 520), t, 0, 0, INK, 0);
    const char *h = "Tap to turn the LED on or off";
    w = tw(TK_IT400, 15, h, 0, 0);
    d_text(d, TK_IT400, 15, (390 - w) / 2, bl(TK_IT400, 15, 556), h, 0, 0, W(.5f), 0);
}

/* P3c: the Calendar: this month's grid (Monday first, as iOS in Finland), today in a red circle, from the phone's
 * clock (sh_year: the year; the host's is 2026) */
static int cal_year = 2026;
void sh_year(int y) { if (y > 2000 && y < 2200) cal_year = y; }
static void page_calendar(dc *d)
{
    const char *mo = MO[(ST.mon + 11) % 12];
    d_text(d, TK_IT800, 34, 20, bl(TK_IT800, 34, 64), mo, -.8f, 0, INK, 0);
    char yb[8];
    put_dec(yb, cal_year);
    d_text(d, TK_IT600, 17, 20 + tw(TK_IT800, 34, mo, -.8f, 0) + 10, bl(TK_IT800, 34, 64) - 0, yb, 0, 0, tk_hex(0xff453a, 1), 0);
    static const char *const H[7] = { "M", "T", "W", "T", "F", "S", "S" };
    for (int c = 0; c < 7; c++) {
        float w = tw(TK_IT600, 13, H[c], 0, 0);
        d_text(d, TK_IT600, 13, 20 + 50 * c + (50 - w) / 2, bl(TK_IT600, 13, 122), H[c], 0, 0, W(c >= 5 ? .4f : .6f), 0);
    }
    d_rrect(d, 16, 146, 358, .5f, 0, W(.15f));
    static const int DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int m = (ST.mon + 11) % 12, nd = DIM[m];
    if (m == 1 && ((cal_year % 4 == 0 && cal_year % 100 != 0) || cal_year % 400 == 0)) nd = 29;
    /* the 1st's weekday from today's (Sunday 0), as a Monday-first column */
    int wd1 = ((ST.wday - (ST.day - 1)) % 7 + 7) % 7, col0 = (wd1 + 6) % 7;
    for (int day = 1; day <= nd; day++) {
        int cell = col0 + day - 1, r = cell / 7, c = cell % 7;
        float cx = 20 + 50 * c + 25, cy = 160 + 56 * r + 22;
        char b[4];
        put_dec(b, day);
        int today = day == ST.day;
        if (today) d_rrect(d, cx - 19, cy - 19, 38, 38, 19, tk_hex(0xff453a, 1));
        float w = tw(today ? TK_IT700 : TK_IT400, 20, b, 0, TK_TNUM);
        d_text(d, today ? TK_IT700 : TK_IT400, 20, cx - w / 2, bl(TK_IT400, 20, cy - 13), b, 0, TK_TNUM, today || c < 5 ? INK : W(.5f), 0);
    }
    float y = 160 + 56 * 6 + 10;
    d_rrect(d, 16, y, 358, .5f, 0, W(.15f));
    char dt[48];
    const char *wd = WD[ST.wday % 7];
    int n = (int)tk_strlen(wd);
    tk_memcpy(dt, wd, (size_t)n); dt[n++] = ' ';
    n += put_dec(dt + n, ST.day); dt[n++] = ' ';
    tk_memcpy(dt + n, mo, tk_strlen(mo) + 1);
    d_text(d, TK_IT600, 15, 20, bl(TK_IT600, 15, y + 18), dt, 0, 0, W(.85f), 0);
    d_text(d, TK_IT400, 15, 20, bl(TK_IT400, 15, y + 46), "No events", 0, 0, W(.45f), 0);
}

static tk_layer apkey, apnum;
static void build_apnum(void)
{
    relayer(&apnum);
    apnum.hidden = !AS.num[0];
    if (!AS.num[0]) return;
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
    if (!!charging != sys_charging) { sys_charging = !!charging; build_lstatus(); build_home_sb(); build_home_w(); relink(); lock_ver++; }
    SYS.charging = charging;
    int n = 0;
    while (version && version[n] && n < 39) { SYS.ver[n] = version[n]; n++; }
    if (version) SYS.ver[n] = 0;
}
static const char *live_text(const char *s, char *buf)
{
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
    else if (!tk_strcmp(s, "@date")) {
        const char *w = WD[ST.wday % 7], *m = MO[(ST.mon + 11) % 12];
        n = (int)tk_strlen(w); tk_memcpy(buf, w, (size_t)n); buf[n++] = ' ';
        n += put_dec(buf + n, ST.day); buf[n++] = ' ';
        tk_memcpy(buf + n, m, tk_strlen(m) + 1);
    } else if (!tk_strcmp(s, "@ver")) return SYS.ver;
    else return "";
    return buf;
}
static void build_app_page(int a)
{
    const char *k = APPS[a].k;
    int own = !tk_strcmp(k, "phone") || !tk_strcmp(k, "messages") || !tk_strcmp(k, "contacts") || !tk_strcmp(k, "photos") ||
              !tk_strcmp(k, "camera") || !tk_strcmp(k, "clock") || !tk_strcmp(k, "calc") || !tk_strcmp(k, "light") ||
              !tk_strcmp(k, "calendar");
    if (own) {
        for (int i = 0; i < NST; i++) {
            dc d = strip_dc(i, "app-page");
            if (!d.b->px) continue;
            if (!tk_strcmp(k, "phone")) page_phone(&d);
            else if (!tk_strcmp(k, "messages")) page_messages(&d);
            else if (!tk_strcmp(k, "contacts")) page_contacts(&d);
            else if (!tk_strcmp(k, "photos")) page_photos(&d);
            else if (!tk_strcmp(k, "clock")) page_clock(&d);
            else if (!tk_strcmp(k, "calc")) page_calc(&d);
            else if (!tk_strcmp(k, "light")) page_light(&d);
            else if (!tk_strcmp(k, "calendar")) page_calendar(&d);
            else page_camera(&d);
            tk_layer_prepare(&appst[i]);
        }
        build_apnum();
        if (!tk_strcmp(k, "calc")) build_calc();
        if (!tk_strcmp(k, "clock")) { relayer(&apsw); relayer(&aptm); relayer(&apctl); CLK.sw_ds = CLK.tm_s = -1; clk_update(now_ms); }
        relink();
        return;
    }
    /* the concept's genericScreen: the large title, captions (or an 18 pt gap), grouped rows */
    const appdef *A = &APPS[a];
    float y = 56 + 8 + 41 + 12;                         /* the title's box and margins */
    float h = y;
    for (int i = 0; i < A->ns; i++) h += (A->sec[i].cap[0] ? 34 : 18) + 47.0f * A->sec[i].n;
    for (int i = 1; i < NST; i++) relayer(&appst[i]);
    relayer(&appst[0]);
    apnum.hidden = 1;
    dc d = mk(&appst[0], "app-page", 0, 0, 390, h + 4, 0, 0);
    d_text(&d, TK_IT800, 34, 20, 64 + 33, A->name, -.8f, 0, INK, 0);
    for (int i = 0; i < A->ns; i++) {
        const asec *S = &A->sec[i];
        if (S->cap[0]) {
            /* .sec: 13 px, upper case, .04 em, .55; margin 18 36 6 (the 18 collapses with the title's 12) */
            char up[64];
            int k = 0;
            for (const char *c = S->cap; *c && k < 63; c++) up[k++] = *c >= 'a' && *c <= 'z' ? (char)(*c - 32) : *c;
            up[k] = 0;
            d_text(&d, TK_IT400, 13, 36, y + (i ? 18 : 6) + 13, up, .52f, 0, W(.55f), 0);
            y += (i ? 18 : 6) + 16 + 6;
        } else
            y += 18;
        float gh = 47.0f * S->n - 1;
        d_rrect(&d, 16, y, 358, gh, 16, tk_hex(0x151c33, 1));
        for (int r = 0; r < S->n; r++) {
            float ry = y + 47 * r;
            char lb[48], rb[48];
            d_text(&d, TK_IT400, 16, 32, ry + 13 + 16, live_text(S->rows[r].l, lb), 0, 0, INK, 0);
            const char *rt = live_text(S->rows[r].r, rb);
            if (!tk_strcmp(A->k, "settings") && !tk_strcmp(S->rows[r].l, "Mobile Data")) rt = md_text();
            if (rt[0]) {
                float w = tw(TK_IT400, 16, rt, 0, TK_TNUM);
                d_text(&d, TK_IT400, 16, 358 - w, ry + 13 + 16, rt, 0, TK_TNUM, W(.55f), 0);
            }
            if (r + 1 < S->n) d_rrect(&d, 16, ry + 46, 358, .5f, 0, W(.08f));
        }
        y += gh;
    }
    tk_layer_prepare(&appst[0]);
    relink();
}
/* P2x: the app's icon over the window while it is small (iOS: the icon grows into the app and the app shrinks back
 * into its icon, so a closing window never ends as an empty tile); a view into the pack, never freed */
static tk_layer apicon;
static void place_apicon(arect R, float p)
{
    char nm[24] = "a62-";
    const char *k = APPS[AP.a].k;
    tk_memcpy(nm + 4, k, tk_strlen(k) + 1);
    const tk_bmp *ic = tk_icon(nm);
    float a = 1 - tk_clampf(p * 3, 0, 1);
    if (!ic || !AP.on || a <= 0) { apicon.hidden = 1; return; }
    if (apicon.bmp.px != ic->px) { tk_layer_dirty(&apicon); apicon.bmp = *ic; }
    float sx = R.w / 62, pad = tk_icon_pad(nm);
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
    appc.alpha = tk_clampf(p * 1.6f - .2f, 0, 1);
    uint32_t t = APPS[AP.a].tint, b = 0x0a0f1f;
    float m = tk_clampf(p * 2, 0, 1);
    uint32_t cr = (uint32_t)((t >> 16 & 255) * (1 - m) + (b >> 16 & 255) * m + .5f), cg = (uint32_t)((t >> 8 & 255) * (1 - m) + (b >> 8 & 255) * m + .5f),
             cb = (uint32_t)((t & 255) * (1 - m) + (b & 255) * m + .5f);
    appbg.solid = 0xff000000u | cr << 16 | cg << 8 | cb;
    appbg.gen++;
    home.sx = home.sy = 1 + .05f * p;
    home.alpha = 1 - .4f * tk_clampf(p, 0, 1);
    appg.hidden = !AP.on;
    place_apicon(R, p);
}
static arect icon_rect(int a)
{
    static const char *P1[8] = { "calendar", "weather", "clock", "camera", "photos", "music", "notes", "calc" };
    const char *const *P2 = EMG ? P2E : P2K;
    static const char *DK[4] = { "phone", "messages", "browser", "settings" };
    const char *k = APPS[a].k;
    for (int i = 0; i < 4; i++) if (!tk_strcmp(DK[i], k)) return (arect){ 22 + 86.5f * i + 12.25f, 705, 62, 62, 15 };
    for (int i = 0; i < 8; i++) if (!tk_strcmp(P1[i], k)) return (arect){ 22 + 86.5f * (i % 4) + 12.25f + page_x, 258 + 103 * (i / 4), 62, 62, 15 };
    for (int i = 0; i < 8; i++) if (!tk_strcmp(P2[i], k)) return (arect){ 390 + 22 + 86.5f * (i % 4) + 12.25f + page_x, 62 + 103 * (i / 4), 62, 62, 15 };
    return (arect){ 164, 370, 62, 62, 15 };
}
/* the app under a tap on home (the page's grid or the dock), or -1 */
static int app_hit(float x, float y)
{
    static const char *P1[8] = { "calendar", "weather", "clock", "camera", "photos", "music", "notes", "calc" };
    const char *const *P2 = EMG ? P2E : P2K;
    static const char *DK[4] = { "phone", "messages", "browser", "settings" };
    const char *k = 0;
    if (y >= 700 && y <= 772) { int i = (int)((x - 22) / 86.5f); if (i >= 0 && i < 4) k = DK[i]; }
    else {
        int pg = page_x < -195;
        float x0 = x - (pg ? 390 + page_x : page_x), top = pg ? 62 : 258;
        int col = (int)((x0 - 22) / 86.5f), row = (int)((y - top) / 103);
        if (x0 >= 22 && y >= top && col < 4 && row < 2 && y - top - row * 103 < 83) k = (pg ? P2 : P1)[row * 4 + col];
    }
    if (!k) return -1;
    for (int a = 0; a < NAPPS; a++) if (!tk_strcmp(APPS[a].k, k)) return a;
    return -1;
}
static void app_open(int a)
{
    AP.a = a;
    AP.on = 1;
    AP.closing = AP.following = 0;
    free_cc();
    AS.tab = 3;
    AS.num[0] = 0;
    AS.thread = -1;
    apkey.hidden = 1;
    build_app_page(a);
    AP.icon = icon_rect(a);
    AP.from = AP.icon;
    AP.to = FULLR;
    tk_spring_start(&sp_app, 0, 1, 0, TK_SPR_OPEN);
    set_app_rect(AP.icon);
}
static void app_close(float v)
{
    AP.closing = 1;
    AP.from = AP.R;
    AP.to = icon_rect(AP.a);
    tk_spring_start(&sp_app, 0, 1, tk_minf(6, tk_fabsf(v) * 2), TK_SPR_CLOSE);
}
static void app_finish_close(void)
{
    AP.on = AP.closing = 0;
    appg.hidden = 1;
    apicon.hidden = 1;
    /* the page (a whole screen) is not kept: the next open draws it again */
    for (int i = 0; i < NST; i++) relayer(&appst[i]);
    relayer(&apnum);
    relayer(&apsw); relayer(&aptm); relayer(&apctl);           /* P2z: the clock's (the timer keeps running) */
    apsw.hidden = aptm.hidden = apctl.hidden = 1;
    relayer(&apcalc); apcalc.hidden = 1;                        /* the Calculator keeps its value (CAL) */
    apnum.hidden = apkey.hidden = 1;
    home.sx = home.sy = 1;
    home.alpha = 1;
}

/* ---- the toast (the concept's #toast: dark glass, 13 pt / 600, from 90 pt above with an overshoot, 2 s) ------ */
static tk_layer toastl;
static float toast_o = -90, toast_from = -90, toast_to = -90, toast_t = 1, toast_left;
static void toast_place(void)
{
    toast_o = toast_from + (toast_to - toast_from) * tk_bezier(.2f, 1.3f, .3f, 1, toast_t);
    toastl.y = toastl.by + PT(toast_o);
    toastl.hidden = toast_t >= 1 && toast_to <= -90;
}
static void toast(const char *msg)
{
    float w = tw(TK_IT600, 13, msg, 0, 0) + 30, x = 195 - w / 2;
    relayer(&toastl);
    /* in an app its backdrop is the app's ground; elsewhere the wallpaper's glass (the shell's backdrop) */
    dc d = mk(&toastl, "toast", x, 52, w, 34, 32, !AP.on);
    if (AP.on) d_lg_app(&d, x, 52, w, 34, 17, 1);
    else d_glass(&d, x, 52, w, 34, 17, 1, 1);
    d_text(&d, TK_IT600, 13, x + 15, bl(TK_IT600, 13, 52 + 9), msg, 0, 0, INK, 0);
    relink();
    toast_from = toast_o;
    toast_to = 0;
    toast_t = 0;
    toast_left = 2000;
    toast_place();
}

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

/* ---- the kernel's hold confirmation (kui_confirm: the modem's Start), in this UI's look: the screen dimmed, a
 * dark glass card with the kernel's words and its hold bar (1.5 s, as the kernel's own); a tap outside cancels ---- */
static tk_layer cfl, cfdim, cfcard, cffill, cflabel;
static struct { int on, down; float t0, p, a, at, hy; } CF;
static void cf_place(void)
{
    cffill.slice_h = PT(56 + (302 - 56) * CF.p);
    cfl.alpha = CF.a;
    cfl.hidden = !CF.on && CF.a <= 0;
}
void sh_confirm(const char *title, const char *l1, const char *l2, const char *l3, const char *hold)
{
    const char *L[3] = { l1, l2, l3 };
    char ln[3][4][96];
    int nl[3];
    float wm, h = 28 + 30 + 12;
    for (int i = 0; i < 3; i++) {
        nl[i] = L[i] && L[i][0] ? wrap(ln[i], TK_IT400, 15, L[i], 302, &wm) : 0;
        h += 21.0f * nl[i] + (nl[i] ? 6 : 0);
    }
    h += 18 + 56 + 24;
    float y = (H_PT - h) / 2;
    tk_layer_init(&cfl, "confirm");
    tk_layer_init(&cfdim, "confirm-dim");
    cfdim.solid = 0x8c000000u;                /* black at .55 (premultiplied) */
    cfdim.solid_w = TK_W;
    cfdim.solid_h = TK_H;
    relayer(&cfcard);
    dc d = mk(&cfcard, "confirm-card", 24, y, 342, h, 44, 0);
    d_lg_flat(&d, 24, y, 342, h, 32, 1);
    d_text(&d, TK_IT800, 22, 44, bl(TK_IT800, 22, y + 28), title, -.4f, 0, INK, 0);
    float ty = y + 28 + 30 + 12;
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < nl[i]; k++) d_text(&d, TK_IT400, 15, 44, bl(TK_IT400, 15, ty + 21.0f * k), ln[i][k], 0, 0, W(.72f), 0);
        ty += 21.0f * nl[i] + (nl[i] ? 6 : 0);
    }
    CF.hy = ty + 12;
    d_rrect(&d, 44, CF.hy, 302, 56, 28, W(.12f));
    relayer(&cffill);
    dc f = mk(&cffill, "confirm-fill", 44, CF.hy, 56, 56, 1, 0);
    d_rrect(&f, 44, CF.hy, 56, 56, 28, TK_RGBA(111, 179, 255, .55f));
    cffill.slice = 2;
    cffill.slice_s0 = cffill.slice_s1 = PT(28);
    relayer(&cflabel);
    float w = tw(TK_IT600, 16, hold, 0, 0);
    dc l = mk(&cflabel, "confirm-label", 44, CF.hy, 302, 56, 1, 0);
    d_text(&l, TK_IT600, 16, 44 + (302 - w) / 2, bl(TK_IT600, 16, CF.hy + 18), hold, 0, 0, INK, 0);
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
 * target made once (two half-resolution buffers, freed at the end), so every glass element follows the fade. The
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
        for (int i = 0; i < L->a->w; i++) {
            uint32_t p = a[i], q = t[i], o = 0;
            if (p == q) continue;
            for (int sh = 0; sh < 32; sh += 8) {
                int x = (int)(p >> sh & 255), y = (int)(q >> sh & 255);
                o |= (uint32_t)(x + (((y - x) * L->f + 128) >> 8)) << sh;
            }
            a[i] = o;
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
static void set_emergency(int on, int instant)
{
    if (on == EMG) return;
    EMG = on;
    if (!on) { free_ecards(); cards_pending = NG; }
    /* the target ground: the other wallpaper, its backdrop with the star of that mode */
    tk_bmp_free(&em_wall);
    tk_bmp_free(&em_bw);
    if (tk_bmp_new(&em_wall, C->base_half.w, C->base_half.h) && tk_bmp_new(&em_bw, C->bw_half.w, C->bw_half.h)) {
        tk_wall_draw(&em_wall, on, .5f);
        tk_bmp src, sh;
        if (tk_bmp_new(&src, em_wall.w, em_wall.h)) {
            tk_bmp_copy(&src, 0, 0, &em_wall, 0, 0, src.w, src.h);
            const tk_bmp *st = tk_icon(on ? "star-e" : "star");
            if (st && tk_bmp_new(&sh, (st->w + 1) / 2, (st->h + 1) / 2)) {
                tk_bmp_down2(&sh, st);
                tk_blit(&src, &sh, (int)(on ? star_e.x : star.x) / 2, (int)star.y / 2, 1);
                tk_bmp_free(&sh);
            }
            tk_glass_backdrop(&em_bw, &src);
            tk_bmp_free(&src);
        }
    } else {
        tk_bmp_free(&em_wall);
        tk_bmp_free(&em_bw);
        tk_log("sbtk: Emergency: no room for the other wallpaper");
    }
    em_e = 0;
    em_t = 0;
    if (instant) { em_t = 1; em_step(1); }
    /* the lock */
    hold_t *q = &Q[1];
    q->lit = on;
    q->facelit.hidden = q->iconlit.hidden = !on;
    q->face.hidden = q->icon.hidden = on;
    for (int g = 0; g < NG; g++) if (K[g].isopen) toggle_group(g, 1);
    scroll = 0;
    sp_scroll.live = 0;
    scroll_momentum = 0;
    if (on) { free_vendor_cards(); build_ecards(); }
    empill.hidden = !on;
    build_lwidgets();
    mark_t = on || lockY < 0 ? 0 : 1;
    if (instant) mark_a = mark_t;
    /* home and the Control Centre */
    build_home_w();
    build_home_apps2();
    cc_stale = 1;
    if (CCS.built) { build_cc_grid(); cc_link(); }
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
    if (is_app("phone")) {
        if (y >= 718 && y <= 780 && x >= 16 && x < 374) {
            int t = (int)((x - 16) / 89.5f);
            if (t != AS.tab) { AS.tab = t; if (t != 3) AS.num[0] = 0; build_app_page(AP.a); }
            return;
        }
        if (AS.tab != 3) return;
        int i = key_hit(x, y);
        if (i >= 0) {
            int n = (int)tk_strlen(AS.num);
            if (n < 15) { AS.num[n] = KD[i][0]; AS.num[n + 1] = 0; build_apnum(); }
            return;
        }
        float dx = x - 195, dy = y - 605;
        if (dx * dx + dy * dy <= 39 * 39)
            toast(!tk_strcmp(AS.num, "112") ? "Calling 112 needs the modem (receive-only today)" :
                  AS.num[0] ? "No SIM: only emergency calls" : "Enter a number");
    } else if (is_app("clock")) {
        clk_tap(x, y);
    } else if (is_app("calc")) {
        calc_tap(x, y);
    } else if (is_app("light")) {
        if (x >= 135 && x < 255 && y >= 250 && y < 490) {
            Q[0].lit = !Q[0].lit;
            Q[0].facelit.hidden = Q[0].iconlit.hidden = !Q[0].lit;
            Q[0].face.hidden = Q[0].icon.hidden = Q[0].lit;
            sh_torch(Q[0].lit);
            lock_ver++;
            build_app_page(AP.a);
        }
    } else if (is_app("settings")) {
        if (x >= 16 && x < 374 && y >= 182 && y < 229) {
            if (cell_running()) { sh_modem(0); toast("Modem stopped"); }
            else if (sh_modem(1) < 0) toast("The modem can't start now");
        }
    } else if (is_app("messages")) {
        if (AS.thread < 0) {
            (void)x; (void)y;                         /* P2y: no conversations (no SIM): nothing to open */
        } else if (x < 120 && y >= 40 && y < 86) {
            AS.thread = -1;
            build_app_page(AP.a);
        }
    }
}

/* ---- the modem's status (docs/MODEM.md 7.7's snapshot): the status bar's bars and RAT, the keypad's line ----- */
static void build_cc_grid(void);
static void cc_link(void);
static sh_cell_t CELL;
static int cell_running(void) { return CELL.state == SH_CELL_ON || CELL.state == SH_CELL_STARTING || CELL.state == SH_CELL_RESTARTING; }
/* Settings > Mobile Data: the modem's state in words (the contract's wording) */
static const char *md_text(void)
{
    switch (CELL.state) {
    case SH_CELL_STARTING: return "Starting\xe2\x80\xa6";
    case SH_CELL_RESTARTING: return "Restarting\xe2\x80\xa6";
    case SH_CELL_GUARD: return "Stopped: a SIM or a registration";
    case SH_CELL_FAILED: return "Could not start";
    case SH_CELL_ON: return CELL.emergency_only ? "Emergency calls only" : "No SIM";
    default: return "Off";
    }
}
static void build_home_sb(void);
void sh_cell(const sh_cell_t *c)
{
    if (c->state == CELL.state && c->sim == CELL.sim && c->emergency_only == CELL.emergency_only && c->rat == CELL.rat &&
        c->bars == CELL.bars)
        return;
    CELL = *c;
    /* the lock's status bar: the network's honest line */
    const char *cr = CELL.state == SH_CELL_STARTING ? "Starting\xe2\x80\xa6" : CELL.state == SH_CELL_RESTARTING ? "Restarting\xe2\x80\xa6" :
                     CELL.state == SH_CELL_ON && CELL.emergency_only ? "Emergency calls only" : "No SIM";
    int n2 = 0;
    while (cr[n2] && n2 < 31) { carrier[n2] = cr[n2]; n2++; }
    carrier[n2] = 0;
    SB_CELL.state = CELL.state == SH_CELL_ON;
    SB_CELL.bars = CELL.bars;
    SB_CELL.rat = CELL.rat;
    const char *h = CELL.state == SH_CELL_ON && !CELL.emergency_only && CELL.bars <= 0 ? "No SIM \xc2\xb7 searching" :
                    "No SIM \xc2\xb7 emergency calls only";
    int n = 0;
    while (h[n] && n < 47) { cell_hint[n] = h[n]; n++; }
    cell_hint[n] = 0;
    build_lstatus();
    build_home_sb();
    if (is_app("phone") || is_app("settings")) build_app_page(AP.a);
    if (CCS.built) { relayer(&ccsb); build_cc_grid(); cc_link(); }
    cc_stale = 1;
    relink();
    lock_ver++;
}

/* ---- the unlock -------------------------------------------------------------------------------------------- */
static void set_lock(float y)
{
    lockY = y;
    float p = tk_clampf(-y / H_PT, 0, 1);
    float la = 1 - tk_maxf(0, p - .55f) * 2.2f;
    home.alpha = tk_minf(1, p * 1.6f);
    home.sx = home.sy = .92f + .08f * tk_minf(1, p * 1.3f);
    mark_t = p > .02f || EMG ? 0 : 1;
    glyph_t = p > .02f && !EMG ? 1 : 0;
    glyph_open = p > .06f;
    if (y == 0 && la >= 1) {
        lockg.hidden = 0;
        lockflat.hidden = 1;
    } else {
        flatten_lock();
        lockg.hidden = 1;
        lockflat.hidden = la <= 0;
        lockflat.y = PT(y) / 2;           /* the layer's offset is scaled with it */
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
    tk_layer *r[] = { &star, &star_e, &home, &lockg, &lockflat, &appg, &apicon, &focus, &ccl, &cfl, &boot, &homebar, &toastl, &slp };
    link(&root, r, 14);
    tk_layer *ag[] = { &appbg, &appc, &apkey, &apnum, &apsw, &aptm, &apctl, &apcalc };
    link(&appg, ag, 8);
    tk_layer *st[] = { &appst[0], &appst[1], &appst[2], &appst[3] };
    link(&appc, st, NST);
    tk_layer *lk[] = { &lstatus, &lclock, &lwidgets, &mark, &glyph_c, &glyph_o, &empill, &list, &Q[0].grp, &Q[1].grp, &hint };
    link(&lockg, lk, 11);
    tk_layer *ls[3 * NG + 6];
    int n = 0;
    for (int g = 0; g < NG; g++) { ls[n++] = &K[g].closed; ls[n++] = &K[g].open; ls[n++] = &K[g].chev; }
    ls[n++] = &etool;
    for (int i = 0; i < 5; i++) ls[n++] = &ecard[i];
    link(&list, ls, n);
    for (int i = 0; i < 2; i++) {
        tk_layer *q[] = { &Q[i].face, &Q[i].facelit, &Q[i].icon, &Q[i].iconlit, &Q[i].ring };
        link(&Q[i].grp, q, 5);
    }
    tk_layer *pg[] = { &hparts[1], &hparts[2], &hparts[4] };
    link(&pages, pg, 3);
    tk_layer *h[] = { &hparts[0], &pages, &hsearch[0], &hsearch[1], &hsearch[2], &hparts[3] };
    link(&home, h, 6);
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
    appbg.solid = 0xff0a0f1fu;
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
    tk_layer_init(&toastl, "toast");
    toastl.hidden = 1;
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
    list.fade_bot = PT(14);
    tk_layer_init(&lockflat, "lock-flat");
    tk_layer_init(&focus, "focus");
    focus.hidden = 1;
    tk_layer_init(&boot, "boot");
    boot.hidden = 1;
    for (int i = 0; i < 2; i++) tk_layer_init(&Q[i].grp, i ? "q-em" : "q-light");
    for (int i = 0; i < 5; i++) { tk_layer_init(&ecard[i], "ecard"); ecard[i].hidden = 1; }
    tk_layer_init(&empill, "empill");
    empill.hidden = 1;
    layout_list();
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
    case 2: case 3: case 4: case 5: case 6: build_card(init_step - 3); lap("bake: a card"); break;
    case 7: build_etool(); build_hold(0); build_hold(1); build_empill(); lap("bake: tools and holds"); break;
    case 8: build_home(); lap("bake: home"); break;
    case 9: {
        /* the flattened lock is only ever seen moving (half-resolution frames): kept at half resolution, drawn 2x */
        tk_bmp_new(&lockflat.bmp, TK_W / 2, TK_H / 2);
        tk_a8_new(&lockflat.glass, TK_W / 2, TK_H / 2);
        lockflat.sx = lockflat.sy = 2;
        lockflat.glass_opaque_only = 1;
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
    mark.alpha = mark_a;
    glyph_c.alpha = glyph_open ? 0 : glyph_a;
    glyph_o.alpha = glyph_open ? glyph_a : 0;
    hint.alpha = hint_a;
}

/* back to the plain lock screen: the sheet and the boot overlay gone, every card closed, the top (exit_em: and out
 * of Emergency mode; the Power key's sleep keeps it) */
static void back_to_lock(int exit_em)
{
    focus_on = 0; focus_closing = 0; focus.hidden = 1; sp_focus.live = sp_thumb.live = 0;
    free_focus();
    booting = 0; boot.hidden = 1;
    for (int g = 0; g < NG; g++) if (K[g].isopen) toggle_group(g, 1);
    scroll = 0; sp_scroll.live = 0; scroll_momentum = 0;
    sp_lock.live = 0;
    sp_page.live = 0; page_dragging = 0;
    set_page(0);
    CCS.on = 0; sp_cc.live = 0; set_cc(0);
    if (AP.on) { sp_app.live = 0; app_finish_close(); }
    if (exit_em && EMG) set_emergency(0, 1);
    if (!EMG) while (cards_pending > 0) { build_card(NG - cards_pending); cards_pending--; }
    if (CF.on) cf_close(0);
    relink();
    toast_t = 1; toast_to = toast_from = -90; toast_left = 0; toast_place();
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
                    uint64_t t0 = tk_us();
                    build_focus(&G[g].it[i], &G[g]);
                    lap_us("focus: snapshot and bake", tk_us() - t0);
                    set_focus(v);
                }
    } else if (!tk_strcmp(what, "reset")) {
        back_to_lock(1);
    } else if (!tk_strcmp(what, "sleep")) {        /* the films: the Power key's fade (v 1) and the wake (v 0) */
        if (v > 0) sh_sleep(0); else sh_wake();
    } else if (!tk_strcmp(what, "app")) {
        for (int a = 0; a < NAPPS; a++)
            if (!tk_strcmp(APPS[a].k, key)) {
                app_open(a);
                sp_app.live = 0;
                set_app_rect(lerpr(AP.icon, FULLR, v));
                if (v >= 1) appg.clip = 0;
            }
    } else if (!tk_strcmp(what, "emergency")) {
        set_emergency(v > 0, 1);
        toast_t = 1;
        toast_to = toast_from = 0;
        toast_place();
    } else if (!tk_strcmp(what, "cell")) {
        /* the modem's state for the host's stills: "on" (emergency-only, GSM, v bars), "starting", "off" */
        sh_cell_t c = { SH_CELL_OFF, -1, 0, 0, -1 };
        if (!tk_strcmp(key, "on")) { c.state = SH_CELL_ON; c.sim = 0x80; c.emergency_only = 1; c.rat = 1; c.bars = (int)v; }
        else if (!tk_strcmp(key, "starting")) c.state = SH_CELL_STARTING;
        sh_cell(&c);
    } else if (!tk_strcmp(what, "tab")) {
        static const char *const TK[4] = { "fav", "rec", "con", "pad" };
        for (int t = 0; t < 4; t++) if (!tk_strcmp(TK[t], key)) { AS.tab = t; build_app_page(AP.a); }
    } else if (!tk_strcmp(what, "dial")) {
        int n = 0;
        while (key[n] && n < 15) { AS.num[n] = key[n]; n++; }
        AS.num[n] = 0;
        build_apnum();
    } else if (!tk_strcmp(what, "thread")) {
        AS.thread = key[0] - '0';
        build_app_page(AP.a);
    } else if (!tk_strcmp(what, "toast")) {
        toast(key);
        toast_t = 1;
        toast_to = toast_from = 0;
        toast_place();
    } else if (!tk_strcmp(what, "cc")) {
        if (!CCS.built || cc_stale) { build_cc(); cc_stale = 0; }
        sp_cc.live = 0;
        CCS.on = v > 0;
        CCS.closing = 0;
        set_cc(v);
    } else if (!tk_strcmp(what, "page")) {
        sp_page.live = 0;
        set_page(-390 * v);
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
enum { G_NONE, G_LOCK, G_LIST, G_TAP_HEAD, G_TAP_ROW, G_HOLD, G_THUMB, G_SCRIM, G_RELOCK, G_PAGE, G_CC, G_CCTAP, G_CCSL, G_APP, G_APPTAP, G_DOCK, G_ETAP, G_CCEM, G_CF, G_CCCELL };
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
        if (CF.on) {
            /* the confirmation: its hold bar, or outside the card (cancel) */
            T.kind = G_CF;
            if (x >= 44 && x < 346 && y >= CF.hy && y < CF.hy + 56) { CF.down = 1; CF.t0 = t; }
            return;
        }
        if (focus_on) {
            float sy = focus_sy;
            if (x >= 39 + thumb_x && x <= 91 + thumb_x && y >= sy + 5 && y <= sy + 57) { T.kind = G_THUMB; T.base = thumb_x; sp_thumb.live = 0; }
            else if (y < 196 || y > 463) T.kind = G_SCRIM;
            return;
        }
        if (!locked && AP.on) {
            /* an open app: the bottom bar's swipe up closes it (the rest of its page does nothing yet) */
            if (y >= H_PT - 30 && !AP.closing) { T.kind = G_APP; sp_app.live = 0; AP.following = 1; tk_tracker_add(&T.tr, y, t); }
            else if (!sp_app.live) {
                /* a tap on the app's page (a key lights while the finger is on it) */
                T.kind = G_APPTAP;
                if (is_app("phone") && AS.tab == 3) key_down(key_hit(x, y));
            }
            return;
        }
        if (!locked && CCS.on && !CCS.closing) {
            /* the open Control Centre: a slider, a tile, or outside it (closes it) */
            if (x >= CCX[2] && x < CCX[2] + 70 && y >= 264 && y < 334) { T.kind = G_CCEM; T.arg = 0; return; }
            /* the cellular circle: a hold starts the modem (then the kernel's confirmation) or stops it */
            if (x >= 104 && x < 184.66f && y >= 92 && y < 170) { T.kind = G_CCCELL; T.arg = 0; return; }
            for (int i = 0; i < 2; i++) {
                float sx = i ? 295.97f : 205.31f;
                if (x >= sx && x < sx + 70 && y >= 92 && y < 248) { T.kind = G_CCSL; T.arg = i; sh_touch(1, x, y, t); return; }
            }
            T.kind = G_CCTAP;
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
            /* home: a pull down from the top-left corner brings the lock back (the concept's re-lock) */
            if (y < 56 && x < 195) { T.kind = G_RELOCK; sp_lock.live = 0; tk_tracker_add(&T.tr, y, t); return; }
            /* elsewhere but the dock, Search and the widgets: the pages (direction decided after 8 pt) */
            int on_w = page_x > -195 && y >= 60 && y <= 218 && ((x >= 24 && x <= 182) || (x >= 208 && x <= 366));
            if (y >= 688 && y < H_PT - 30) { T.kind = G_DOCK; return; }   /* the dock: a tap opens its app */
            if (y >= 56 && y < 688 && !(y >= 640 && y <= 670 && x >= 150 && x <= 240) && !on_w) {
                T.kind = G_PAGE;
                T.base = page_x;
                page_dir = 0;
                sp_page.live = 0;
                tk_tracker_add(&T.tr, x, t);
            }
            return;
        }
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
        if (EMG && y >= LIST_TOP && y < LIST_TOP + LIST_H && y < H_PT - 30) {
            /* Emergency's list: a card (its app, by name: locked, a toast) or Exit Emergency */
            T.kind = G_ETAP;
            T.arg = -1;
            for (int i = 0; i < 4; i++) {
                float top = LIST_TOP + 10 + 78 * i;
                if (y >= top && y < top + 68 && x >= 14 && x < 376) T.arg = i;
            }
            if (y >= 630 && y < 670 && x >= 120 && x < 270) T.arg = 4;
            tk_tracker_add(&T.tr, y, t);
            return;
        }
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
        case G_APPTAP:
            if (T.moved && !apkey.hidden) key_down(-1);
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
        case G_RELOCK:
            tk_tracker_add(&T.tr, y, t);
            set_lock(-H_PT + tk_maxf(0, dy));
            break;
        case G_APP: {
            tk_tracker_add(&T.tr, y, t);
            float d2 = tk_minf(0, dy), sc = tk_maxf(.35f, 1 + d2 / (H_PT * .9f));
            arect R = { 0, 0, 390 * sc, H_PT * sc, 52 + (30 - 52) * (1 - sc) };
            R.x = x - R.w / 2;
            R.y = (H_PT + d2) - R.h * .92f;
            AP.travel = d2;
            set_app_rect(R);
            break;
        }
        case G_CC:
            tk_tracker_add(&T.tr, y, t);
            CCS.on = 1;
            CCS.closing = 0;
            set_cc(tk_minf(1, tk_maxf(0, dy) / 260));
            break;
        case G_CCSL:
            CCS.lvl[T.arg] = tk_clampf((248 - y) / 156, 0, 1);
            cc_levels();
            if (T.arg == 0) {                    /* P2r: the brightness slider sets the panel's level (1..4) */
                int lv = 1 + (int)(CCS.lvl[0] * 3.999f);
                if (lv != cc_bright) { cc_bright = lv; sh_brightness(lv); }
            }
            break;
        case G_PAGE: {
            float dx = x - T.x0;
            tk_tracker_add(&T.tr, x, t);
            if (!page_dir && dx * dx + dy * dy > 64) page_dir = tk_fabsf(dx) > tk_fabsf(dy) ? 1 : 2;
            if (page_dir == 1) {
                page_dragging = 1;
                float px = T.base + dx;
                if (px > 0) px *= .3f;
                if (px < -390) px = -390 + (px + 390) * .3f;
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
        if (!q->fired) toast("Press and hold");
        hold_ring(q, T.arg, 0);
        break;
    }
    case G_ETAP:
        if (!T.moved && T.arg >= 0) {
            if (T.arg == 4) set_emergency(0, 0);
            else {
                char m[48] = "Opens ";
                tk_memcpy(m + 6, EC[T.arg].act, tk_strlen(EC[T.arg].act) + 1);
                toast(m);
            }
        }
        break;
    case G_CCEM:
        if (!T.arg) toast("Press and hold");
        break;
    case G_CCCELL:
        if (!T.arg) toast(cell_running() ? "Hold to stop the modem" : "Hold to start the modem");
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
            if (y < cfcard.by / TK_PT + 44 || y > CF.hy + 56 + 24 + 4) cf_close(0);
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
        if (CCS.p > .35f || v > .4f) tk_spring_start(&sp_cc, CCS.p, 1, v * 1000 / 260, TK_SPR_CC);
        else cc_close();
        break;
    }
    case G_CCTAP:
        if (!T.moved && !cc_tap(x, y)) cc_close();
        break;
    case G_APP: {
        float v = tk_tracker_v(&T.tr);
        AP.following = 0;
        if (AP.travel < -60 || v < -.35f) app_close(v);
        else { AP.from = AP.R; AP.to = FULLR; tk_spring_start(&sp_app, 0, 1, 0, TK_SPR_OPEN); }
        break;
    }
    case G_PAGE:
        if (!page_dir) {                      /* a tap: an app icon opens its app */
            int a = app_hit(x, y);
            if (a >= 0) app_open(a);
            break;
        }
        if (page_dir == 1) {
            float v = tk_tracker_v(&T.tr), target = page_x + v * 220 < -195 ? -390 : 0;
            page_dragging = 0;
            tk_spring_start(&sp_page, page_x, target, v * 1000, TK_SPR_ROWS);
        }
        break;
    case G_RELOCK: {
        float v = tk_tracker_v(&T.tr);
        if (-lockY < H_PT * .7f || v > .4f) { locked = 1; tk_spring_start(&sp_lock, lockY, 0, 0, TK_SPR_UNLOCK); }
        else tk_spring_start(&sp_lock, lockY, -H_PT, 0, TK_SPR_UNLOCK);
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
/* the Power key (or the screen-off timer): the fade to black; again during it: back (now: black at once, the
 * screen already off) */
void sh_sleep(int now)
{
    T.kind = G_NONE;
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

int sh_tick(float dt, float t)
{
    int mv = 0;
    now_ms = t;
    clk_update(t);
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
        if (!live && sp_lock.to <= -H_PT + .01f) { locked = 0; lockflat.hidden = 1; home.sx = home.sy = 1; }
        mv = 1;
    }
    if (T.kind == G_LOCK || T.kind == G_LIST || T.kind == G_THUMB || T.kind == G_RELOCK || T.kind == G_PAGE) mv = 1;
    if (sp_page.live) {
        tk_spring_step(&sp_page, dt);
        set_page(sp_page.x);
        mv = 1;
    }
    if (T.kind == G_CC || T.kind == G_CCSL || T.kind == G_APP) mv = 1;
    if (sp_app.live) {
        int live = tk_spring_step(&sp_app, dt);
        float u = AP.closing ? tk_minf(1.04f, sp_app.x) : sp_app.x;
        set_app_rect(lerpr(AP.from, AP.to, u));
        if (!live) {
            if (AP.closing) app_finish_close();
            else appg.clip = 0;              /* at rest: the whole panel, no clip (as the concept drops it) */
        }
        mv = 1;
    }
    if (em_t < 1) {
        em_t = tk_minf(1, em_t + dt / .7f);
        em_step(tk_bezier(.25f, .1f, .25f, 1, em_t));
        mv = 1;
    }
    if (T.kind == G_CCCELL && !T.arg) {
        if (t - T.t0 >= 700) {
            T.arg = 1;
            if (cell_running()) { sh_modem(0); toast("Modem stopped"); }
            else if (sh_modem(1) < 0) toast("The modem can't start now");
        }
        mv = 1;
    }
    if (CF.down) {
        CF.p = tk_minf(1, (t - CF.t0) / 1500);
        cf_place();
        if (CF.p >= 1) cf_close(1);
        mv = 1;
    }
    if (CF.a != CF.at) {
        CF.a = CF.at > CF.a ? tk_minf(CF.at, CF.a + dt / .2f) : tk_maxf(CF.at, CF.a - dt / .2f);
        cf_place();
        if (!CF.on && CF.a <= 0) cf_free();
        mv = 1;
    }
    if (T.kind == G_CCEM && !T.arg) {
        if (t - T.t0 >= 700) { T.arg = 1; set_emergency(!EMG, 0); }
        mv = 1;
    }
    if (toast_t < 1) {
        toast_t = tk_minf(1, toast_t + dt / .45f);
        toast_place();
        mv = 1;
    }
    if (toast_left > 0) {
        toast_left -= dt * 1000;
        if (toast_left <= 0) { toast_from = toast_o; toast_to = -90; toast_t = 0; }
    }
    if (sp_cc.live) {
        int live = tk_spring_step(&sp_cc, dt);
        set_cc(tk_maxf(0, sp_cc.x));
        if (!live && CCS.closing) { CCS.on = 0; CCS.closing = 0; set_cc(0); }
        mv = 1;
    }
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
        if (!k->isopen && !k->sp.live && k->chev_t >= 1 && k->memh <= 0 && k->face.bmp.px) free_open(g);
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
    return mv;
}

void sh_status(int h, int mi, int wday, int day, int mon, int pct, int temp)
{
    int tchange = h != ST.h || mi != ST.mi || wday != ST.wday || day != ST.day || mon != ST.mon;
    int wchange = pct != ST.pct || temp != ST.temp;
    ST.h = h; ST.mi = mi; ST.wday = wday; ST.day = day; ST.mon = mon; ST.pct = pct; ST.temp = temp;
    if (tchange && AP.on && (is_app("clock") || is_app("calendar")) && !AP.closing) build_app_page(AP.a);   /* the time, today */
    if (tchange) {
        /* the layers keep their place in the tree: rebuilt in place */
        build_lclock();
        build_home_sb();
        relink();
    }
    if (wchange) {
        build_lwidgets();
        build_home_w();                         /* P2y: home's battery widget shows the same live values */
        relink();
    }
    if (tchange || wchange) lock_ver++;
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
    if (sh_moving()) return;
    tk_comp_idle(C);
    if (!EMG && cards_pending > 0) {
        /* after Emergency: the vendor cards again, one per idle turn (the platform calls again while
         * sh_idle_pending); the half copies below are made in the same turn (P2w: an unlock right after
         * Emergency made them in its first frame, 31 ms) */
        build_card(NG - cards_pending);
        cards_pending--;
        relink();
        place_list();
    }
    if (locked && lockY == 0 && !focus_on) flatten_lock();
    /* home at rest: the Control Centre's snapshot and grid ready before the pull */
    if (!locked && !CCS.on && !AP.on && (!CCS.built || cc_stale)) { build_cc(); cc_stale = 0; }
    /* the half copies of what moves (the rest are never drawn at half resolution: the half canvas follows the
     * rest frames) */
    prep_tree(home.child);                       /* home's pages too (their widgets and app grids, P2s) */
    tk_layer_prepare(&star);
    tk_layer_prepare(&homebar);
    for (int g = 0; g < NG; g++) {
        tk_layer_prepare(&K[g].closed);
        tk_layer_prepare(&K[g].chev);
        if (K[g].face.bmp.px) {
            tk_layer_prepare(&K[g].face);
            tk_layer_prepare(&K[g].head);
            for (int i = 0; i < G[g].n; i++) tk_layer_prepare(&K[g].rows[i]);
        }
    }
    tk_layer_prepare(&etool);
    for (int i = 0; i < 2; i++) {
        tk_layer_prepare(&Q[i].face); tk_layer_prepare(&Q[i].facelit);
        tk_layer_prepare(&Q[i].icon); tk_layer_prepare(&Q[i].iconlit);
    }
    tk_layer_prepare(&mark); tk_layer_prepare(&glyph_c); tk_layer_prepare(&glyph_o); tk_layer_prepare(&hint);
}

int sh_idle_pending(void) { return !EMG && cards_pending > 0; }

int sh_moving(void)
{
    int m = sp_lock.live || sp_scroll.live || scroll_momentum || T.kind == G_LOCK || T.kind == G_LIST || T.kind == G_THUMB ||
            T.kind == G_RELOCK || T.kind == G_PAGE || sp_page.live || T.kind == G_CC || T.kind == G_CCSL || sp_cc.live ||
            T.kind == G_APP || sp_app.live || booting || toast_t < 1 || em_t < 1 || T.kind == G_CCEM ||
            T.kind == G_CCCELL || CF.down || CF.a != CF.at || SL.dir;
    for (int g = 0; g < NG; g++) m |= K[g].sp.live || K[g].chev_t < 1;
    m |= sp_focus.live || sp_thumb.live;
    return m;
}
