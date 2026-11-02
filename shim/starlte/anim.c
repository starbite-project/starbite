/* SPDX-License-Identifier: GPL-2.0-only
 * The boot intro (Starbite 2): from S-Boot's splash ("SAMSUNG Galaxy S9 ... Powered by starbite",
 * UP_PARAM logo.jpg) into the menu. One thing moves at a time, on pure black:
 *
 *   part    the splash's lines leave through their own edges (a masked slide: Samsung's block
 *           up, "Powered by starbite" down); only the blue star of "starbite" stays
 *   hero    the star charges, rises to the middle of the screen and grows into a big glowing
 *           hero with a quarter turn; a white flash, one thin shock ring, a light sweep
 *   land    it shoots to the header's star, shrinking and turning, and settles; "starbite"
 *           slides out from under it, the model line and the footer come in
 *                                                                   star_intro_splash()
 *   menu    the rows of the menu rise in one after another          star_reveal() (v6),
 *                                                                   star_intro_rows() (v5)
 * A key pressed (after all keys were up once) skips to the end.
 *
 * Frames are composed in a write-back buffer (OFF, the whole panel) and only the rectangles that
 * changed are copied to the write-combined framebuffer (needs the loader's MMU: mmu.c; without it
 * the intro is skipped). Integer arithmetic only (-mgeneral-regs-only): 16.16 fixed point.
 * Star positions come from marker sprites 2 (header star) and 3 (splash star), see menu.mjs.
 * Host preview: tools/anim-preview (the real code, frame by frame, to PNG).
 */
#include <stdint.h>
#include "fbtext.h"
#include "hw.h"
#include "menu.h"
#include "mmu.h"

#define W STAR_FB_WIDTH
#define H STAR_FB_HEIGHT
#define FB ((volatile uint64_t *)STAR_FB_BASE)
/* Free RAM below the menu assets (0x89000000) and above the BOOT image (< 0x83800000); the DTB
 * reserves nothing here. All in the loader's write-back window (mmu.c), below 0x85c00000. */
#define OFF  ((uint32_t *)0x84000000UL)     /* W x H, 16.3 MiB */
#define LOGO ((uint32_t *)0x85100000UL)     /* S-Boot's splash, LW x LH (7.2 MiB) */
#define HDR  ((uint32_t *)0x85900000UL)     /* the header's rows, W wide, <= BAND_MAX */
#define FTR  ((uint32_t *)0x85a00000UL)     /* the footer's rows */
#define SQRT ((uint16_t *)0x85b00000UL)     /* [65536] sqrt(i / 65536) in 0.16 */
#define GLOW ((uint16_t *)0x85b20000UL)     /* [GLOW_N] the bloom's falloff over (d / r)^2 */
#define LOGO_X0 180
#define LOGO_X1 1260
#define LOGO_Y0 1144
#define LOGO_Y1 2892
#define LW (LOGO_X1 - LOGO_X0)
#define LH (LOGO_Y1 - LOGO_Y0)
#define BAND_MAX 160
#define BLACK 0xff000000U
#define ACCENT_R 0x6fU
#define ACCENT_G 0xb3U
#define ACCENT_B 0xffU
#define ONE 65536
#define GLOW_N 4956                         /* (d / r)^2 < 4.84: the glow ends at 2.2 r */

int star_sprite_to(unsigned id, uint32_t *dst, int x, int y, int at, unsigned fade);
int star_sprite_rect(unsigned id, unsigned *x, unsigned *y, unsigned *w, unsigned *h);

/* ---- small helpers ---------------------------------------------------------------------------- */

struct rect { int x0, y0, x1, y1; };        /* x1, y1 exclusive; empty when x1 <= x0 */

static void r_add(struct rect *r, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)W) x1 = (int)W;
    if (y1 > (int)H) y1 = (int)H;
    if (x1 <= x0 || y1 <= y0)
        return;
    if (r->x1 <= r->x0) {
        r->x0 = x0; r->y0 = y0; r->x1 = x1; r->y1 = y1;
        return;
    }
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}

static void off_fill(const struct rect *r, uint32_t px)
{
    for (int y = r->y0; y < r->y1; ++y) {
        uint32_t *row = OFF + (uint64_t)y * W;
        for (int x = r->x0; x < r->x1; ++x)
            row[x] = px;
    }
}

/* OFF -> panel, 8 bytes at a time (x rounded out to even pixels). */
static void present(const struct rect *r)
{
    if (r->x1 <= r->x0)
        return;
    int x0 = r->x0 & ~1, x1 = (r->x1 + 1) & ~1;
    if (x1 > (int)W)
        x1 = (int)W;
    for (int y = r->y0; y < r->y1; ++y) {
        const uint64_t *s = (const uint64_t *)(OFF + (uint64_t)y * W + x0);
        volatile uint64_t *d = FB + ((uint64_t)y * W + (uint64_t)x0) / 2;
        for (int x = x0; x < x1; x += 2)
            *d++ = *s++;
    }
    __asm__ volatile("dsb st" ::: "memory");
}

/* A frame's dirty rectangles. */
#define NRECT 16
struct rects { int n; struct rect r[NRECT]; };

static void rl_add(struct rects *l, int x0, int y0, int x1, int y1)
{
    struct rect t = { 0, 0, 0, 0 };
    r_add(&t, x0, y0, x1, y1);
    if (t.x1 <= t.x0)
        return;
    if (l->n < NRECT) {
        l->r[l->n].x0 = t.x0; l->r[l->n].y0 = t.y0; l->r[l->n].x1 = t.x1; l->r[l->n].y1 = t.y1;
        l->n++;
    } else {
        r_add(&l->r[NRECT - 1], t.x0, t.y0, t.x1, t.y1);
    }
}

/* Put last frame's rectangles (cleared) and this frame's on the panel, each of the old ones
 * merged into a new one it mostly overlaps (the star's box moves a little each frame), so no
 * pixel is copied twice for nothing. */
static void present(const struct rect *r);
static void rl_present(const struct rects *prev, const struct rects *cur)
{
    struct rects out;
    out.n = cur->n;
    for (int j = 0; j < cur->n; ++j)
        out.r[j] = cur->r[j];
    for (int i = 0; i < prev->n; ++i) {
        const struct rect *p = &prev->r[i];
        int64_t pa = (int64_t)(p->x1 - p->x0) * (p->y1 - p->y0);
        int k = -1;
        for (int j = 0; j < out.n && k < 0; ++j) {
            const struct rect *c = &out.r[j];
            int ix0 = p->x0 > c->x0 ? p->x0 : c->x0, ix1 = p->x1 < c->x1 ? p->x1 : c->x1;
            int iy0 = p->y0 > c->y0 ? p->y0 : c->y0, iy1 = p->y1 < c->y1 ? p->y1 : c->y1;
            if (ix1 > ix0 && iy1 > iy0 && (int64_t)(ix1 - ix0) * (iy1 - iy0) * 4 >= pa * 3)
                k = j;
        }
        if (k >= 0)
            r_add(&out.r[k], p->x0, p->y0, p->x1, p->y1);
        else
            present(p);
    }
    for (int j = 0; j < out.n; ++j)
        present(&out.r[j]);
}

static int rl_hits(const struct rects *l, int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < l->n; ++i)
        if (l->r[i].x0 < x1 && x0 < l->r[i].x1 && l->r[i].y0 < y1 && y0 < l->r[i].y1)
            return 1;
    return 0;
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* 0..ONE progress of t in [a, b] (ms). */
static int32_t prog(int32_t t, int32_t a, int32_t b)
{
    if (t <= a) return 0;
    if (t >= b) return ONE;
    return (int32_t)(((int64_t)(t - a) << 16) / (b - a));
}

static int32_t fmul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static int32_t ease_out(int32_t s) { int32_t i = ONE - s; return ONE - fmul(fmul(i, i), i); }
static int32_t ease_in_out(int32_t s) { return fmul(fmul(s, s), 3 * ONE - 2 * s); }
static int32_t ease_in3(int32_t s) { return fmul(fmul(s, s), s); }
static int32_t ease_out5(int32_t s)
{
    int32_t i = ONE - s, i2 = fmul(i, i);
    return ONE - fmul(fmul(i2, i2), i);
}
static int32_t ease_io4(int32_t s)
{
    if (s < ONE / 2) {
        int32_t s2 = fmul(s, s);
        return 8 * fmul(s2, s2);
    }
    int32_t i = ONE - s, i2 = fmul(i, i);
    return ONE - 8 * fmul(i2, i2);
}
/* Overshoots by about k / 10 and settles (k 16.16). */
static int32_t ease_back(int32_t s, int32_t k)
{
    int32_t p = s - ONE, p2 = fmul(p, p);
    return ONE + fmul(k + ONE, fmul(p2, p)) + fmul(k, p2);
}
/* a + (b - a) * e, e 0..ONE (and a little past for ease_back) */
static int32_t lerp(int32_t a, int32_t b, int32_t e) { return a + (int32_t)(((int64_t)(b - a) * e) >> 16); }

/* sin / cos of a turn fraction (ONE = 360 degrees), 16.16; Taylor to x^7 on a quadrant. */
static int32_t fsin(int32_t a)
{
    a &= ONE - 1;
    int q = a >> 14;
    int32_t f = a & 16383;
    if (q & 1)
        f = 16384 - f;
    int32_t x = (int32_t)(((int64_t)f * 102944) >> 14);             /* radians, 16.16 */
    int32_t x2 = fmul(x, x), x3 = fmul(x2, x), x5 = fmul(x3, x2), x7 = fmul(x5, x2);
    int32_t s = x - x3 / 6 + x5 / 120 - x7 / 5040;
    if (s > ONE) s = ONE;
    return q & 2 ? -s : s;
}
static int32_t fcos(int32_t a) { return fsin(a + ONE / 4); }

static uint32_t sat(uint32_t v) { return v > 255 ? 255 : v; }

/* Lighten-blend a colour into OFF. */
static void put_max(uint32_t *p, uint32_t r, uint32_t g, uint32_t b)
{
    uint32_t d = *p;
    uint32_t dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    r = sat(r); g = sat(g); b = sat(b);
    *p = BLACK | ((r > dr ? r : dr) << 16) | ((g > dg ? g : dg) << 8) | (b > db ? b : db);
}

static uint32_t scale_px(uint32_t c, uint32_t a)
{
    if (a >= 256)
        return BLACK | c;
    return BLACK | ((((c >> 16) & 255U) * a >> 8) << 16) | ((((c >> 8) & 255U) * a >> 8) << 8) | ((c & 255U) * a >> 8);
}

/* The tables: SQRT for the star's curve, GLOW for its bloom (1 / (1 + k q) falloffs, a tight one
 * and a wide one, each brought down to 0 at the edge so the glow has no rim). */
static void tables(void)
{
    for (uint32_t i = 0; i < 65536; ++i) {
        uint32_t v = isqrt64((uint64_t)i << 16);
        SQRT[i] = (uint16_t)(v > 65535 ? 65535 : v);
    }
    const int64_t et = ((int64_t)1024 << 16) / (1024 + 9 * GLOW_N);
    const int64_t ew = ((int64_t)1024 << 16) / (1024 + (GLOW_N * 3) / 2);
    for (int64_t i = 0; i < GLOW_N; ++i) {
        int64_t t = ((int64_t)1024 << 16) / (1024 + 9 * i), w = ((int64_t)1024 << 16) / (1024 + (i * 3) / 2);
        int64_t nt = ((t - et) << 16) / (ONE - et), nw = ((w - ew) << 16) / (ONE - ew);
        int64_t g = (60 * nt + 40 * nw) / 100;                      /* 16.16 */
        GLOW[i] = (uint16_t)((g * 256) >> 16);
    }
}

struct star { int32_t x, y, r, turn; int heat, glow, bright; int32_t sweep; };

/* What every pixel of one star shares. */
struct shade { int64_t inv, c, sn, qlim; int32_t r, sweep; uint32_t heat, glow, bright, cr, cg; };

/* Core coverage *a and bloom *g (0..256+) at (tx, ty), the offset from the centre in r (16.16). */
static int sample(const struct shade *k, int64_t tx, int64_t ty, uint32_t *a, uint32_t *g)
{
    int64_t q = (tx * tx + ty * ty) >> 16;
    if (q >= k->qlim)
        return -1;
    *g = (uint32_t)GLOW[q >> 6] * k->glow >> 8;
    *a = 0;
    if (q < ONE) {
        int64_t u = (tx * k->c + ty * k->sn) >> 16, v = (ty * k->c - tx * k->sn) >> 16;
        if (u < 0) u = -u;
        if (v < 0) v = -v;
        if (u < ONE && v < ONE) {
            int32_t f = (int32_t)SQRT[u] + (int32_t)SQRT[v];
            if (f < ONE) {
                int64_t cov = ((int64_t)(ONE - f) * k->r * 5) >> 26;     /* 256 per 0.8 px */
                *a = cov > 256 ? 256 : (uint32_t)cov;
            }
        }
    }
    return *a || *g;
}

/* One pixel: the core (heated blue, the sweep's light band) over the bloom, lighten into OFF. */
static void shade_px(const struct shade *k, uint32_t *p, uint32_t a, uint32_t g, int64_t tx, int64_t ty)
{
    uint32_t rr = 0, gg = 0, bb = 0;
    if (g) {
        uint32_t wt = k->heat * (g > 256 ? 256 : g) >> 9;          /* whiter towards the middle */
        rr = (ACCENT_R + ((255 - ACCENT_R) * wt >> 8)) * g >> 8;
        gg = (ACCENT_G + ((255 - ACCENT_G) * wt >> 8)) * g >> 8;
        bb = 255 * g >> 8;
        if (rr > 255) rr = 255;
        if (gg > 255) gg = 255;
        if (bb > 255) bb = 255;
    }
    if (a) {
        uint32_t kr = k->cr, kg = k->cg;
        if (k->sweep > -2 * ONE && k->sweep < 2 * ONE) {
            int64_t d = ((tx + ty) * 46341 >> 16) - k->sweep;
            if (d < 0) d = -d;
            if (d < 22000) {
                uint32_t hl = (uint32_t)(((22000 - d) * 240) / 22000);
                kr += (255 - kr) * hl >> 8;
                kg += (255 - kg) * hl >> 8;
            }
        }
        rr = (kr * a + rr * (256 - a)) >> 8;
        gg = (kg * a + gg * (256 - a)) >> 8;
        bb = (255 * a + bb * (256 - a)) >> 8;
    }
    if (k->bright < 256) {
        rr = rr * k->bright >> 8;
        gg = gg * k->bright >> 8;
        bb = bb * k->bright >> 8;
    }
    put_max(p, rr, gg, bb);
}

/*
 * The brand star (4 curved points: sqrt|u| + sqrt|v| <= 1, the curve of the STAR path in
 * tools/brand/menu.mjs to within half a percent) and its bloom.
 *   x, y    centre, 16.16 px            r      tip radius, 16.16 px
 *   turn    rotation, ONE = a full turn  heat   0 the brand blue .. 256 white
 *   glow    bloom strength 0..384        bright the whole thing, 0..256
 *   sweep   centre of a light band across the star along the diagonal, in r (16.16);
 *           outside -2..2 = none
 * Small stars are drawn at sub-pixel positions; big ones (fast moving) sit on a pixel centre and
 * are computed for one quadrant, the star and its bloom being the same a quarter turn round.
 */
static void draw_star(const struct star *s, struct rects *dirty)
{
    if (s->r < ONE / 2)
        return;
    struct shade k;
    k.inv = (int64_t)(((uint64_t)1 << 48) / (uint64_t)s->r);         /* (d * inv) >> 32: d / r */
    k.c = fcos(s->turn);
    k.sn = fsin(s->turn);
    k.qlim = (int64_t)GLOW_N << 6;
    k.r = s->r;
    k.sweep = s->sweep;
    k.heat = (uint32_t)s->heat;
    k.glow = (uint32_t)(s->glow < 0 ? 0 : s->glow);
    k.bright = (uint32_t)(s->bright < 0 ? 0 : s->bright);
    k.cr = ACCENT_R + ((255 - ACCENT_R) * k.heat >> 8);
    k.cg = ACCENT_G + ((255 - ACCENT_G) * k.heat >> 8);
    int32_t ext = k.glow ? s->r / 5 * 11 : s->r + ONE;
    uint32_t a, g;
    if (s->r >= 24 * ONE) {
        int ix = s->x >> 16, iy = s->y >> 16, e = (ext >> 16) + 1;
        for (int dy = 0; dy <= e; ++dy) {
            int64_t ty = (((int64_t)dy << 16) * k.inv) >> 32;
            if ((ty * ty >> 16) >= k.qlim)
                break;
            for (int dx = 1; dx <= e; ++dx) {
                int64_t tx = (((int64_t)dx << 16) * k.inv) >> 32;
                int hit = sample(&k, tx, ty, &a, &g);
                if (hit < 0)
                    break;
                if (!hit)
                    continue;
                /* (dx, dy), dx >= 1, dy >= 0, and its quarter turns (-dy, dx), (-dx, -dy), (dy, -dx) */
                int x = ix + dx, y = iy + dy;
                if ((unsigned)x < W && (unsigned)y < H) shade_px(&k, OFF + (uint64_t)y * W + x, a, g, tx, ty);
                x = ix - dy; y = iy + dx;
                if ((unsigned)x < W && (unsigned)y < H) shade_px(&k, OFF + (uint64_t)y * W + x, a, g, -ty, tx);
                x = ix - dx; y = iy - dy;
                if ((unsigned)x < W && (unsigned)y < H) shade_px(&k, OFF + (uint64_t)y * W + x, a, g, -tx, -ty);
                x = ix + dy; y = iy - dx;
                if ((unsigned)x < W && (unsigned)y < H) shade_px(&k, OFF + (uint64_t)y * W + x, a, g, ty, -tx);
            }
        }
        if (sample(&k, 0, 0, &a, &g) > 0 && (unsigned)ix < W && (unsigned)iy < H)
            shade_px(&k, OFF + (uint64_t)iy * W + ix, a, g, 0, 0);
        rl_add(dirty, ix - e, iy - e, ix + e + 1, iy + e + 1);
        return;
    }
    int x0 = (s->x - ext) >> 16, x1 = ((s->x + ext) >> 16) + 2;
    int y0 = (s->y - ext) >> 16, y1 = ((s->y + ext) >> 16) + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)W) x1 = (int)W;
    if (y1 > (int)H) y1 = (int)H;
    for (int y = y0; y < y1; ++y) {
        int64_t ty = ((((int64_t)y << 16) + 0x8000 - s->y) * k.inv) >> 32;
        uint32_t *row = OFF + (uint64_t)y * W;
        for (int x = x0; x < x1; ++x) {
            int64_t tx = ((((int64_t)x << 16) + 0x8000 - s->x) * k.inv) >> 32;
            if (sample(&k, tx, ty, &a, &g) > 0)
                shade_px(&k, row + x, a, g, tx, ty);
        }
    }
    rl_add(dirty, x0, y0, x1, y1);
}

/* A thin ring of light: radius rad, half thickness th (px), brightness i 0..256. */
static void draw_ring(int cx, int cy, int rad, int th, int i, struct rects *dirty)
{
    if (i <= 0 || th < 1)
        return;
    int ro = rad + th + 1, ri = rad - th - 1;
    int64_t ri2 = ri > 0 ? (int64_t)ri * ri : 0, ro2 = (int64_t)ro * ro;
    for (int dy = 0; dy <= ro; ++dy) {
        for (int dx = 0; dx <= ro; ++dx) {
            int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
            if (d2 < ri2 || d2 > ro2)
                continue;
            int d = (int)(isqrt64((uint64_t)d2 << 8));                    /* 8.4 px */
            int off = d > rad * 16 ? d - rad * 16 : rad * 16 - d;
            if (off >= th * 16)
                continue;
            uint32_t a = (uint32_t)((th * 16 - off) * 256 / (th * 16)) * (uint32_t)i >> 8;
            uint32_t w = a * a >> 9;
            uint32_t rr = (ACCENT_R * a + (255 - ACCENT_R) * w) >> 8;
            uint32_t gg = (ACCENT_G * a + (255 - ACCENT_G) * w) >> 8;
            uint32_t bb = (ACCENT_B * a) >> 8;
            int px[4] = { cx + dx, cx - dx, cx + dx, cx - dx }, py[4] = { cy + dy, cy + dy, cy - dy, cy - dy };
            for (int k = 0; k < 4; ++k) {
                if ((k & 1) && !dx)
                    continue;
                if ((k & 2) && !dy)
                    continue;
                if ((unsigned)px[k] < W && (unsigned)py[k] < H)
                    put_max(OFF + (uint64_t)py[k] * W + (unsigned)px[k], rr, gg, bb);
            }
        }
    }
    rl_add(dirty, cx - ro, cy - ro, cx + ro + 1, cy + ro + 1);
}

/* Keys: a press counts only after every key was released once (a held Power from switch-on is not
 * a skip). */
static int keys_armed;
static int skip_pressed(void)
{
    unsigned k = hw_keys();
    if (!keys_armed) {
        keys_armed = !k;
        return 0;
    }
    return k != 0;
}

static int32_t now_ms(uint64_t t0)
{
    return (int32_t)((hw_ticks() - t0) * 1000U / hw_freq());
}

/* ---- splash -> header ------------------------------------------------------------------------- */

/* The timeline (ms). */
#define T_PART0   30        /* the splash's lines start leaving, ST apart, PART_D each */
#define T_PART_ST 50
#define T_PART_D  260
#define T_CHARGE0 60        /* the star brightens where it is */
#define T_CHARGE1 300
#define T_RISE0   260       /* ... rises to the middle, growing and turning a quarter */
#define T_GROW0   320
#define T_RISE1   740
#define T_FLASH   740       /* white flash, the ring, the light sweep */
#define T_COOL    980
#define T_RING1   1160
#define T_SWEEP0  790
#define T_SWEEP1  1010
#define T_FLY0    980       /* to the header star */
#define T_FLY1    1280
#define T_LAND1   1400      /* it lands on the header's own star, a quick glint, gone */
#define T_WORD0   1210      /* "starbite" slides out from under the star */
#define T_WORD1   1560
#define T_MODEL0  1320      /* the model line, the footer */
#define T_MODEL1  1600
#define T_FOOT0   1360
#define T_FOOT1   1640
#define T_DONE    1650

#define HERO_R 260          /* the hero: tip radius, centre y */
#define HERO_Y ((int)H / 2 - 90)

#define NBAND 8
static int band_y0[NBAND], band_y1[NBAND], band_dir[NBAND], band_t0[NBAND], band_done[NBAND];
static int nband;

/* Rows of LOGO with something on them, in bands (gaps of 10 rows or more split them). */
static void find_bands(void)
{
    int in = 0, last = -100;
    nband = 0;
    for (int y = 0; y < LH; ++y) {
        const uint32_t *s = LOGO + (uint64_t)y * LW;
        int any = 0;
        for (int x = 0; x < LW && !any; ++x)
            any = (s[x] & 0xffffffU) != 0;
        if (!any)
            continue;
        if (in && y - last < 10) {
            band_y1[nband - 1] = y + 1;
        } else if (nband < NBAND) {
            band_y0[nband] = y;
            band_y1[nband] = y + 1;
            nband++;
            in = 1;
        } else {
            band_y1[nband - 1] = y + 1;
        }
        last = y;
    }
    /* Samsung's block leaves upwards, top line first; "Powered by starbite" downwards */
    int up = 0, down = 0;
    for (int i = 0; i < nband; ++i) {
        band_dir[i] = band_y0[i] + band_y1[i] < LH ? -1 : 1;
        band_t0[i] = T_PART0 + T_PART_ST * (band_dir[i] < 0 ? up++ : down++);
        band_done[i] = 0;
    }
}

/* Band i at time t: it slides through its own top (or bottom) edge and fades. */
static void draw_band(int i, int32_t t)
{
    int32_t s = prog(t, band_t0[i], band_t0[i] + T_PART_D);
    int h = band_y1[i] - band_y0[i];
    int o = (int)(((int64_t)fmul(s, s) * (h + 8)) >> 16);
    uint32_t a = 256 - (uint32_t)(ease_in3(s) >> 8);              /* bright until it is nearly out */
    for (int y = band_y0[i]; y < band_y1[i]; ++y) {
        int ys = y - band_dir[i] * o;
        uint32_t *d = OFF + (uint64_t)(LOGO_Y0 + y) * W + LOGO_X0;
        if (ys < band_y0[i] || ys >= band_y1[i] || s >= ONE) {
            for (int x = 0; x < LW; ++x)
                d[x] = BLACK;
            continue;
        }
        const uint32_t *p = LOGO + (uint64_t)ys * LW;
        for (int x = 0; x < LW; ++x)
            d[x] = scale_px(p[x], a);
    }
}

static int hdr_y0, hdr_h, ftr_y0, ftr_h, split, clip;

/* The header at t: the wordmark slides out from under the star (clipped at clip), the star's own
 * pixels once the moving one is on them, the model line from the right. */
static void draw_header(int32_t t)
{
    int32_t sw = ease_out5(prog(t, T_WORD0, T_WORD1));
    int dw = (int)(((int64_t)(ONE - sw) * 260) >> 16);
    uint32_t aw = (uint32_t)(ease_out(prog(t, T_WORD0, T_WORD0 + (T_WORD1 - T_WORD0) * 3 / 5)) >> 8);
    int32_t sm = ease_out5(prog(t, T_MODEL0, T_MODEL1));
    int dm = (int)(((int64_t)(ONE - sm) * 70) >> 16);
    uint32_t am = (uint32_t)(ease_out(sm) >> 8);
    int star_on = t >= T_FLY1;
    for (int r = 0; r < hdr_h; ++r) {
        const uint32_t *s = HDR + (uint64_t)r * W;
        uint32_t *d = OFF + (uint64_t)(hdr_y0 + r) * W;
        for (int x = 0; x < split; ++x) {
            uint32_t c = BLACK;
            if (x < clip) {
                if (x - dw >= 0 && aw)
                    c = scale_px(s[x - dw], aw);
            } else if (star_on) {
                c = s[x];
            }
            d[x] = c;
        }
        for (int x = split; x < (int)W; ++x)
            d[x] = x + dm < (int)W && am ? scale_px(s[x + dm], am) : BLACK;
    }
}

/* The footer rises into place through its bottom edge. */
static void draw_footer(int32_t t)
{
    int32_t s = ease_out5(prog(t, T_FOOT0, T_FOOT1));
    int o = (int)(((int64_t)(ONE - s) * 56) >> 16);
    uint32_t a = (uint32_t)(ease_out(s) >> 8);
    for (int r = 0; r < ftr_h; ++r) {
        uint32_t *d = OFF + (uint64_t)(ftr_y0 + r) * W;
        int rs = r - o;
        if (rs < 0 || !a) {
            for (int x = 0; x < (int)W; ++x)
                d[x] = BLACK;
            continue;
        }
        const uint32_t *s2 = FTR + (uint64_t)rs * W;
        for (int x = 0; x < (int)W; ++x)
            d[x] = scale_px(s2[x], a);
    }
}

/* The moving star at t (0 when it is not drawn). */
static int star_at(int32_t t, struct star *s, int32_t sx, int32_t sy, int32_t r0, int32_t hx, int32_t hy,
                   int32_t r1)
{
    const int32_t cx = ((int32_t)(W / 2) << 16) + ONE / 2, cy = (HERO_Y << 16) + ONE / 2;
    const int32_t R = HERO_R * ONE;
    s->sweep = 4 * ONE;
    s->bright = 256;
    if (t < T_RISE0) {
        int32_t e = ease_out(prog(t, T_CHARGE0, T_CHARGE1));
        s->x = sx; s->y = sy;
        s->r = r0 + fmul(r0 / 4, e);
        s->turn = 0;
        s->heat = (int)(fmul(e, 70 * ONE) >> 16);
        s->glow = (int)(fmul(e, 120 * ONE) >> 16);
        return 1;
    }
    const int32_t rc = r0 + r0 / 4;
    if (t < T_FLY0) {
        int32_t p = prog(t, T_RISE0, T_RISE1), e = ease_io4(p);
        s->x = lerp(sx, cx, ease_in_out(p));
        s->y = lerp(sy, cy, e);
        s->r = lerp(rc, R, ease_back(prog(t, T_GROW0, T_RISE1), ONE));
        s->turn = ease_in_out(p) / 4;
        s->glow = 120 + (int)(fmul(ease_out(p), 136 * ONE) >> 16);
        if (t < T_FLASH) {
            s->heat = 70 + (int)(fmul(e, 110 * ONE) >> 16);
        } else {
            int32_t f = ease_out(prog(t, T_FLASH, T_COOL));
            s->heat = 256 - (int)(fmul(f, 136 * ONE) >> 16);
            /* the flash swells the bloom a moment */
            s->glow = 384 - (int)(fmul(f, 128 * ONE) >> 16);
        }
        if (t >= T_SWEEP0 && t < T_SWEEP1)
            s->sweep = -3 * ONE / 2 + 3 * ease_in_out(prog(t, T_SWEEP0, T_SWEEP1));
        return 1;
    }
    if (t < T_LAND1) {
        int32_t p = prog(t, T_FLY0, T_FLY1), e = ease_io4(p), u = ONE - e;
        s->x = lerp(cx, hx, e);
        s->y = lerp(cy, hy, e);
        s->r = r1 + fmul(R - r1, fmul(u, u));
        s->turn = ONE / 4 + ease_in_out(p) / 4;
        s->heat = (int)(fmul(ONE - e, 120 * ONE) >> 16);
        s->glow = 256 - (int)(fmul(e, 176 * ONE) >> 16);
        if (t >= T_FLY1) {      /* the glint over the header's star: up in a fifth, then out */
            int32_t g = prog(t, T_FLY1, T_LAND1);
            if (g < ONE / 5) {
                int32_t u2 = ease_out(g * 5);
                s->glow = 80 + (int)(fmul(u2, 150 * ONE) >> 16);
                s->heat = (int)(fmul(u2, 170 * ONE) >> 16);
            } else {
                int32_t d = ONE - ease_in_out((g - ONE / 5) * 5 / 4);
                s->glow = (int)(fmul(d, 230 * ONE) >> 16);
                s->heat = (int)(fmul(d, 170 * ONE) >> 16);
                s->bright = (int)(d >> 8);                  /* down to the header's own star */
            }
        }
        return 1;
    }
    return 0;
}

void star_intro_splash(void)
{
    unsigned sx, sy, sw, sh, hx, hy, hw, hh, bx, by, bw, bh;
    if (!star_mmu_enabled() || !star_sprite_rect(3, &sx, &sy, &sw, &sh) || !star_sprite_rect(2, &hx, &hy, &hw, &hh) ||
        !star_sprite_rect(0, &bx, &by, &bw, &bh) || bh > BAND_MAX) {
        (void)star_sprite_to(0, (uint32_t *)0, 0, 0, 0, 256);           /* plain base, as before */
        return;
    }
    /* What S-Boot drew: the splash into LOGO (one pass over the uncached panel); the faint JPEG
     * noise around the letters goes to black. */
    const volatile uint64_t *fb = FB;
    for (int y = 0; y < LH; ++y) {
        uint32_t *d = LOGO + (uint64_t)y * LW;
        for (int x = 0; x < LW; x += 2) {
            uint64_t v = fb[((uint64_t)(LOGO_Y0 + y) * W + LOGO_X0 + x) / 2];
            uint32_t p[2] = { (uint32_t)v, (uint32_t)(v >> 32) };
            for (int k = 0; k < 2; ++k) {
                uint32_t c = p[k], m = (c >> 16) & 255;
                if (((c >> 8) & 255) > m) m = (c >> 8) & 255;
                if ((c & 255) > m) m = c & 255;
                d[x + k] = m < 10 ? 0 : (c & 0xffffffU);
            }
        }
    }
    /* The star leaves the logo: ours replaces it. Its blue pixels go, and everything close. */
    {
        int r0 = (int)sw * 46 / 100, cx = (int)sx - LOGO_X0, cy = (int)sy - LOGO_Y0;
        for (int dy = -2 * r0; dy <= 2 * r0; ++dy)
            for (int dx = -2 * r0; dx <= 2 * r0; ++dx) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= LW || y >= LH)
                    continue;
                uint32_t *p = LOGO + (uint64_t)y * LW + x, c = *p;
                int blue = (int)(c & 255) > (int)((c >> 16) & 255) + 12;
                if (blue || dx * dx + dy * dy <= r0 * r0 * 3 / 2)
                    *p = 0;
            }
    }
    tables();
    find_bands();
    /* The menu's base (header, footer) at full strength, kept aside. */
    struct rect all = { 0, 0, (int)W, (int)H };
    off_fill(&all, BLACK);
    (void)star_sprite_to(0, OFF, 0, 0, 0, 256);
    hdr_y0 = (int)by;
    hdr_h = (int)bh;
    for (int r = 0; r < hdr_h; ++r)
        for (int x = 0; x < (int)W; ++x)
            HDR[(uint64_t)r * W + x] = OFF[(uint64_t)(hdr_y0 + r) * W + x];
    ftr_y0 = 0;
    ftr_h = 0;
    for (int y = hdr_y0 + hdr_h; y < (int)H; ++y) {
        const uint32_t *s = OFF + (uint64_t)y * W;
        int any = 0;
        for (int x = 0; x < (int)W && !any; ++x)
            any = (s[x] & 0xffffffU) != 0;
        if (!any)
            continue;
        if (!ftr_h)
            ftr_y0 = y;
        ftr_h = y + 1 - ftr_y0;
    }
    if (ftr_h > BAND_MAX)
        ftr_h = BAND_MAX;
    for (int r = 0; r < ftr_h; ++r)
        for (int x = 0; x < (int)W; ++x)
            FTR[(uint64_t)r * W + x] = OFF[(uint64_t)(ftr_y0 + r) * W + x];
    /* The wordmark ends left of the star; the model line starts well right of it. */
    const int rs = (int)hw * 46 / 100;
    clip = (int)hx - rs - 1;
    split = (int)hx + rs + 24;

    /* The panel as it is now, without the star, in OFF. */
    off_fill(&all, BLACK);
    for (int y = 0; y < LH; ++y)
        for (int x = 0; x < LW; ++x)
            OFF[(uint64_t)(LOGO_Y0 + y) * W + LOGO_X0 + x] = BLACK | LOGO[(uint64_t)y * LW + x];

    const int32_t r0 = (int32_t)sw * (ONE / 100) * 46, r1 = (int32_t)hw * (ONE / 100) * 46;
    const int32_t px0 = ((int32_t)sx << 16) + ONE / 2, py0 = ((int32_t)sy << 16) + ONE / 2;
    const int32_t px1 = ((int32_t)hx << 16) + ONE / 2, py1 = ((int32_t)hy << 16) + ONE / 2;
    struct rects prev, cur;
    prev.n = 0;
    int logo_clean = 0, first = 1;
    uint64_t t0 = hw_ticks();
    for (;;) {
        int32_t t = now_ms(t0);
        if (t > T_DONE || skip_pressed())
            break;
        cur.n = 0;
        for (int i = 0; i < prev.n; ++i)
            off_fill(&prev.r[i], BLACK);
        if (first) {                                /* where S-Boot's star was */
            int e = (int)sw + 2;
            rl_add(&cur, (int)sx - e, (int)sy - e, (int)sx + e + 1, (int)sy + e + 1);
            first = 0;
        }

        /* the splash's lines */
        int left = 0;
        for (int i = 0; i < nband; ++i) {
            if (band_done[i])
                continue;
            int moving = t > band_t0[i];
            int bx0 = LOGO_X0, by0 = LOGO_Y0 + band_y0[i], bx1 = LOGO_X1, by1 = LOGO_Y0 + band_y1[i];
            if (moving || rl_hits(&prev, bx0, by0, bx1, by1))
                draw_band(i, t);
            if (moving)
                rl_add(&cur, bx0, by0, bx1, by1);
            if (t >= band_t0[i] + T_PART_D)
                band_done[i] = 1;
            else
                left = 1;
        }
        if (!left && !logo_clean) {                 /* once, the whole splash area to black */
            struct rect lr = { LOGO_X0, LOGO_Y0, LOGO_X1, LOGO_Y1 };
            off_fill(&lr, BLACK);
            rl_add(&cur, lr.x0, lr.y0, lr.x1, lr.y1);
            logo_clean = 1;
        }

        /* the menu's base */
        if (t >= T_WORD0 || rl_hits(&prev, 0, hdr_y0, (int)W, hdr_y0 + hdr_h)) {
            draw_header(t);
            if (t >= T_WORD0)
                rl_add(&cur, 0, hdr_y0, (int)W, hdr_y0 + hdr_h);
        }
        if (ftr_h && (t >= T_FOOT0 || rl_hits(&prev, 0, ftr_y0, (int)W, ftr_y0 + ftr_h))) {
            draw_footer(t);
            if (t >= T_FOOT0)
                rl_add(&cur, 0, ftr_y0, (int)W, ftr_y0 + ftr_h);
        }

        /* the shock ring and the star */
        if (t >= T_FLASH && t < T_RING1) {
            int32_t s = prog(t, T_FLASH, T_RING1), e = ease_out(s);
            int rad = HERO_R + (int)(fmul(e, 360 * ONE) >> 16);
            draw_ring((int)W / 2, HERO_Y, rad, 3, 230 - (int)(fmul(ease_in_out(s), 230 * ONE) >> 16), &cur);
        }
        struct star st;
        if (star_at(t, &st, px0, py0, r0, px1, py1, r1))
            draw_star(&st, &cur);

        rl_present(&prev, &cur);
        prev.n = cur.n;
        for (int i = 0; i < cur.n; ++i)
            prev.r[i] = cur.r[i];
    }
    /* The end state: black, the base at full strength. */
    off_fill(&all, BLACK);
    (void)star_sprite_to(0, OFF, 0, 0, 0, 256);
    present(&all);
}

/* ---- menu rows -------------------------------------------------------------------------------- */

/* ids[i]: menu sprites in the order they come in (rows top to bottom, then chips, hints). */
void star_intro_rows(const unsigned *ids, unsigned n)
{
    if (!star_mmu_enabled() || n == 0 || n > 16)
        return;
    struct { int x, y, w, h; } r[16];
    for (unsigned i = 0; i < n; ++i) {
        unsigned x, y, w, h;
        if (!star_sprite_rect(ids[i], &x, &y, &w, &h))
            return;
        r[i].x = (int)x; r[i].y = (int)y; r[i].w = (int)w; r[i].h = (int)h;
    }
    const int32_t STAGGER = 70, DUR = 340, RISE = 60;
    const int32_t T_END = STAGGER * (int32_t)(n - 1) + DUR;
    uint64_t t0 = hw_ticks();
    for (;;) {
        int32_t t = now_ms(t0);
        if (t > T_END + 20 || skip_pressed())
            break;
        /* clear every moving element first, then draw them all: sliding rows overlap a little */
        for (unsigned i = 0; i < n; ++i) {
            if (prog(t, STAGGER * (int32_t)i, STAGGER * (int32_t)i + DUR) == 0)
                continue;
            struct rect a = { r[i].x, r[i].y, r[i].x + r[i].w, r[i].y + r[i].h + RISE };
            off_fill(&a, BLACK);
        }
        for (unsigned i = 0; i < n; ++i) {
            int32_t s = prog(t, STAGGER * (int32_t)i, STAGGER * (int32_t)i + DUR);
            if (s == 0)
                continue;
            int32_t e = ease_out(s);
            int dy = RISE - (int)(fmul(e, RISE << 16) >> 16);
            (void)star_sprite_to(ids[i], OFF, r[i].x, r[i].y + dy, 1, (unsigned)(e >> 8));
        }
        for (unsigned i = 0; i < n; ++i) {
            if (prog(t, STAGGER * (int32_t)i, STAGGER * (int32_t)i + DUR) == 0)
                continue;
            struct rect a = { r[i].x, r[i].y, r[i].x + r[i].w, r[i].y + r[i].h + RISE };
            present(&a);
        }
    }
    /* menu.c draws the exact states next */
}

/* Copy src's rectangle (x, y, w, h) into OFF at y + dy, every channel scaled by fade (0..256). */
static void blit_fade(const uint32_t *src, int x, int y, int w, int h, int dy, uint32_t fade)
{
    for (int r = 0; r < h; ++r) {
        int ty = y + r + dy;
        if (ty < 0 || ty >= (int)H)
            continue;
        const uint32_t *s = src + (uint64_t)(y + r) * W + x;
        uint32_t *d = OFF + (uint64_t)ty * W + x;
        for (int c = 0; c < w; ++c)
            d[c] = scale_px(s[c], fade);
    }
}

/* The menu assembles: each rectangle rises RISE px into place (a quick, soft landing: quintic
 * ease-out) and is opaque well before it stops; STAGGER ms apart, top to bottom. */
void star_reveal(const uint32_t *src, const int *rc, unsigned n)
{
    const int32_t STAGGER = 50, DUR = 380, RISE = 72, FADE = 200;
    if (star_mmu_enabled() && n && n <= 32) {
        const int32_t T_LAST = STAGGER * (int32_t)(n - 1) + DUR;
        uint64_t t0 = hw_ticks();
        for (;;) {
            int32_t t = now_ms(t0);
            if (t > T_LAST + 20 || skip_pressed())
                break;
            for (unsigned i = 0; i < n; ++i) {
                const int *q = rc + 4 * i;
                int32_t s = prog(t, STAGGER * (int32_t)i, STAGGER * (int32_t)i + DUR);
                if (s == 0)
                    continue;
                struct rect a = { q[0], q[1], q[0] + q[2], q[1] + q[3] + RISE };
                off_fill(&a, BLACK);
            }
            for (unsigned i = 0; i < n; ++i) {
                const int *q = rc + 4 * i;
                int32_t a0 = STAGGER * (int32_t)i, s = prog(t, a0, a0 + DUR);
                if (s == 0)
                    continue;
                int32_t e = ease_out5(s), f = ease_out(prog(t, a0, a0 + FADE));
                blit_fade(src, q[0], q[1], q[2], q[3], RISE - (int)(fmul(e, RISE << 16) >> 16), (uint32_t)(f >> 8));
            }
            for (unsigned i = 0; i < n; ++i) {
                const int *q = rc + 4 * i;
                if (prog(t, STAGGER * (int32_t)i, STAGGER * (int32_t)i + DUR) == 0)
                    continue;
                struct rect a = { q[0], q[1], q[0] + q[2], q[1] + q[3] + RISE };
                present(&a);
            }
        }
    }
    /* the end state: src on the panel */
    volatile uint64_t *fb = FB;
    const uint64_t *s = (const uint64_t *)src;
    for (uint64_t i = 0; i < (uint64_t)W * H / 2; ++i)
        fb[i] = s[i];
    __asm__ volatile("dsb st" : : : "memory");
}
