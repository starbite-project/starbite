/* SPDX-License-Identifier: GPL-2.0-only
 * The boot intro (Starbite 2): from S-Boot's splash ("SAMSUNG Galaxy S9 ... Powered by starbite",
 * UP_PARAM logo.jpg) into the menu.
 *
 *   splash  Samsung's logo fades out; the blue star of "starbite" lifts off, grows and flies in an
 *           arc to the menu header's star, leaving a sparkle trail; it bursts into a ring of light
 *           and the header (menu sprite 0) fades in.               star_intro_splash()
 *   menu    the OS rows, service chips and hints slide up and fade in one after another.
 *                                                                   star_intro_rows()
 * A key pressed (after all keys were up once) skips to the end.
 *
 * Frames are composed in a write-back buffer (OFF, the whole panel) and only the rectangles that
 * changed are copied to the write-combined framebuffer (needs the loader's MMU: mmu.c; without it
 * the intro is skipped). Integer arithmetic only (-mgeneral-regs-only): 16.16 fixed point.
 * Star positions come from marker sprites 2 (header star) and 3 (splash star), see menu.mjs.
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
 * reserves nothing here. Both are in the loader's write-back window (mmu.c). */
#define OFF  ((uint32_t *)0x84000000UL)     /* W x H, 17 MiB */
#define LOGO ((uint32_t *)0x85200000UL)     /* S-Boot's splash rows LOGO_Y0..LOGO_Y1, W wide */
#define LOGO_X0 180U
#define LOGO_X1 1260U
#define LOGO_Y0 1144U
#define LOGO_Y1 2892U
#define BLACK 0xff000000U
#define ACCENT_R 0x6fU
#define ACCENT_G 0xb3U
#define ACCENT_B 0xffU
#define ONE 65536

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

static uint32_t sat(uint32_t v) { return v > 255 ? 255 : v; }

/* Lighten-blend a colour into OFF. */
static void put_max(int x, int y, uint32_t r, uint32_t g, uint32_t b)
{
    if ((unsigned)x >= W || (unsigned)y >= H)
        return;
    uint32_t *p = OFF + (uint64_t)y * W + (unsigned)x, d = *p;
    uint32_t dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    r = sat(r); g = sat(g); b = sat(b);
    *p = BLACK | ((r > dr ? r : dr) << 16) | ((g > dg ? g : dg) << 8) | (b > db ? b : db);
}

/*
 * The brand star (4 curved points: sqrt|u| + sqrt|v| <= 1, the curve of tools/brand/logo.mjs)
 * with a blue glow, radius r px, at (cx, cy), brightness i 0..256. Computed for one quadrant and
 * mirrored. Grows *dirty.
 */
static void draw_star(int cx, int cy, int r, int i, struct rect *dirty)
{
    if (r < 1 || i <= 0)
        return;
    int rb = r * 3 + 2;
    int64_t r2 = (int64_t)r * r;
    for (int dy = 0; dy <= rb; ++dy) {
        for (int dx = 0; dx <= rb; ++dx) {
            int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
            uint32_t glow = (uint32_t)((150 * r2) / (r2 + 3 * d2));       /* 150 at the centre */
            uint32_t core = 0;
            if (dx < r && dy < r) {
                uint64_t u = ((uint64_t)dx << 16) / (uint64_t)r, v = ((uint64_t)dy << 16) / (uint64_t)r;
                uint32_t f = isqrt64(u << 16) + isqrt64(v << 16);           /* 16.16 */
                if (f < ONE) {
                    int64_t a = ((int64_t)(ONE - f) * r) >> 8;              /* ~256 per pixel inside */
                    core = a > 256 ? 256 : (uint32_t)a;
                }
            }
            if (glow < 3 && !core)
                continue;
            uint32_t rr = (core * 255 + (256 - core) * (ACCENT_R * glow >> 7)) >> 8;
            uint32_t gg = (core * 255 + (256 - core) * (ACCENT_G * glow >> 7)) >> 8;
            uint32_t bb = (core * 255 + (256 - core) * (ACCENT_B * glow >> 7)) >> 8;
            rr = rr * (uint32_t)i >> 8; gg = gg * (uint32_t)i >> 8; bb = bb * (uint32_t)i >> 8;
            put_max(cx + dx, cy + dy, rr, gg, bb);
            if (dx) put_max(cx - dx, cy + dy, rr, gg, bb);
            if (dy) put_max(cx + dx, cy - dy, rr, gg, bb);
            if (dx && dy) put_max(cx - dx, cy - dy, rr, gg, bb);
        }
    }
    r_add(dirty, cx - rb, cy - rb, cx + rb + 1, cy + rb + 1);
}

/* A ring of light: radius rad, half thickness th, brightness i 0..256. */
static void draw_ring(int cx, int cy, int rad, int th, int i, struct rect *dirty)
{
    if (i <= 0 || th < 1)
        return;
    int ro = rad + th, ri = rad - th;
    int64_t ri2 = ri > 0 ? (int64_t)ri * ri : 0, ro2 = (int64_t)ro * ro;
    for (int dy = 0; dy <= ro; ++dy) {
        for (int dx = 0; dx <= ro; ++dx) {
            int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
            if (d2 < ri2 || d2 > ro2)
                continue;
            int d = (int)isqrt64((uint64_t)d2), off = d > rad ? d - rad : rad - d;
            uint32_t a = (uint32_t)((th - off) * 256 / th) * (uint32_t)i >> 8;
            uint32_t w = a * a >> 8;                                   /* white hot at the crest */
            uint32_t rr = (ACCENT_R * a + (255 - ACCENT_R) * w) >> 8;
            uint32_t gg = (ACCENT_G * a + (255 - ACCENT_G) * w) >> 8;
            uint32_t bb = (ACCENT_B * a) >> 8;
            put_max(cx + dx, cy + dy, rr, gg, bb);
            if (dx) put_max(cx - dx, cy + dy, rr, gg, bb);
            if (dy) put_max(cx + dx, cy - dy, rr, gg, bb);
            if (dx && dy) put_max(cx - dx, cy - dy, rr, gg, bb);
        }
    }
    r_add(dirty, cx - ro, cy - ro, cx + ro + 1, cy + ro + 1);
}

static uint32_t rng_state = 0x5eedbeefU;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525U + 1013904223U;
    return rng_state >> 8;
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

#define PARTS 28
struct part { int x, y, r; int32_t born; int used; };

void star_intro_splash(void)
{
    unsigned sx, sy, sw, sh, hx, hy, hw, hh;
    if (!star_mmu_enabled() || !star_sprite_rect(3, &sx, &sy, &sw, &sh) || !star_sprite_rect(2, &hx, &hy, &hw, &hh)) {
        (void)star_sprite_to(0, (uint32_t *)0, 0, 0, 0, 256);           /* plain base, as before */
        return;
    }
    /* What S-Boot drew: the splash rows into LOGO (one pass over the uncached panel). */
    const volatile uint64_t *fb = FB;
    for (unsigned y = LOGO_Y0; y < LOGO_Y1; ++y) {
        uint64_t *d = (uint64_t *)(LOGO + (uint64_t)(y - LOGO_Y0) * W);
        for (unsigned x = 0; x < W; x += 2)
            d[x / 2] = fb[((uint64_t)y * W + x) / 2];
    }
    /* The panel as it is now, in OFF. */
    struct rect all = { 0, 0, (int)W, (int)H };
    off_fill(&all, BLACK);
    for (unsigned y = LOGO_Y0; y < LOGO_Y1; ++y)
        for (unsigned x = LOGO_X0; x < LOGO_X1; ++x)
            OFF[(uint64_t)y * W + x] = LOGO[(uint64_t)(y - LOGO_Y0) * W + x];
    /* The star leaves the logo: our own one replaces it. */
    for (int dy = -(int)sw; dy <= (int)sw; ++dy)
        for (int dx = -(int)sw; dx <= (int)sw; ++dx) {
            int x = (int)sx + dx, y = (int)sy + dy;
            if (y >= (int)LOGO_Y0 && y < (int)LOGO_Y1 && x >= 0 && x < (int)W)
                LOGO[(uint64_t)(y - (int)LOGO_Y0) * W + (unsigned)x] = BLACK;
        }

    const int32_t T_END = 1750;
    const int r0 = (int)sw / 2, r1 = (int)hw / 2;
    /* flight: quadratic Bezier P0 (splash star) -> P1 (out to the right, half way up) -> P2 (header) */
    const int p0x = (int)sx, p0y = (int)sy, p2x = (int)hx, p2y = (int)hy;
    const int p1x = 1300, p1y = (p0y + p2y) / 2 + 200;
    struct part parts[PARTS];
    for (unsigned k = 0; k < PARTS; ++k)
        parts[k].used = 0;
    unsigned next_part = 0;
    int32_t last_spawn = 0;
    struct rect prev = { 0, 0, 0, 0 };
    int logo_done = 0;
    uint64_t t0 = hw_ticks();
    for (;;) {
        int32_t t = now_ms(t0);
        int skip = skip_pressed();
        if (t > T_END || skip)
            break;
        struct rect cur = { 0, 0, 0, 0 };
        off_fill(&prev, BLACK);

        /* Samsung's logo and "Powered by starbite" fade (100..600 ms). */
        int32_t lf = ONE - ease_in_out(prog(t, 100, 600));
        if (lf > 0) {
            uint32_t a = (uint32_t)(lf >> 8);
            for (unsigned y = LOGO_Y0; y < LOGO_Y1; ++y) {
                const uint32_t *s = LOGO + (uint64_t)(y - LOGO_Y0) * W;
                uint32_t *d = OFF + (uint64_t)y * W;
                for (unsigned x = LOGO_X0; x < LOGO_X1; ++x) {
                    uint32_t c = s[x];
                    if ((c & 0xffffffU) == 0) {
                        d[x] = BLACK;
                        continue;
                    }
                    d[x] = BLACK | ((((c >> 16) & 255) * a >> 8) << 16) | ((((c >> 8) & 255) * a >> 8) << 8) |
                           ((c & 255) * a >> 8);
                }
            }
            r_add(&cur, (int)LOGO_X0, (int)LOGO_Y0, (int)LOGO_X1, (int)LOGO_Y1);
        } else if (!logo_done) {
            struct rect lr = { (int)LOGO_X0, (int)LOGO_Y0, (int)LOGO_X1, (int)LOGO_Y1 };
            off_fill(&lr, BLACK);
            r_add(&cur, lr.x0, lr.y0, lr.x1, lr.y1);
            logo_done = 1;
        }

        /* The star: a glowing lift-off (0..300), the flight (300..1150), into the header (..1300). */
        int cx, cy, r, bright = 256;
        if (t < 300) {
            int32_t s = ease_out(prog(t, 0, 300));
            cx = p0x; cy = p0y - (int)(fmul(s, 40 << 16) >> 16);
            r = r0 + (int)(fmul(s, (r0 / 2) << 16) >> 16);
        } else if (t < 1150) {
            int32_t s = ease_in_out(prog(t, 300, 1150)), u = ONE - s;
            int64_t bx = ((int64_t)fmul(fmul(u, u), p0x << 16) + 2 * (int64_t)fmul(fmul(u, s), p1x << 16) +
                          (int64_t)fmul(fmul(s, s), p2x << 16)) >> 16;
            int64_t by = ((int64_t)fmul(fmul(u, u), (p0y - 40) << 16) + 2 * (int64_t)fmul(fmul(u, s), p1y << 16) +
                          (int64_t)fmul(fmul(s, s), p2y << 16)) >> 16;
            cx = (int)bx; cy = (int)by;
            /* swells to ~3x mid-flight, lands at the header star's size */
            int32_t bump = fmul(4 * s, ONE - s);                       /* 0 -> 1 -> 0 */
            r = (int)(((int64_t)(r0 + r0 / 2) * (ONE - s) + (int64_t)r1 * s + (int64_t)(r0 * 2) * bump) >> 16);
            if (t - last_spawn >= 18) {                                /* sparkle trail */
                struct part *p = &parts[next_part++ % PARTS];
                p->x = cx + (int)(rnd() % 61) - 30;
                p->y = cy + (int)(rnd() % 61) - 30;
                p->r = 3 + (int)(rnd() % 7);
                p->born = t;
                p->used = 1;
                last_spawn = t;
            }
        } else {
            cx = p2x; cy = p2y; r = r1;
            bright = 256 - (int)(prog(t, 1150, 1450) >> 8);              /* hands over to the header */
        }
        draw_star(cx, cy, r, bright, &cur);
        for (unsigned k = 0; k < PARTS; ++k) {
            struct part *p = &parts[k];
            if (!p->used)
                continue;
            int32_t age = t - p->born;
            if (age > 520) {
                p->used = 0;
                continue;
            }
            int i = 256 - age * 256 / 520;
            draw_star(p->x, p->y + age / 12, p->r, i, &cur);
        }

        /* The burst (1150..1700) and the header fading in (1200..1600). */
        if (t >= 1150) {
            int32_t s = prog(t, 1150, 1700);
            int rad = 12 + (int)(fmul(ease_out(s), 380 << 16) >> 16);
            draw_ring(p2x, p2y, rad, 10 + rad / 12, 256 - (int)(s >> 8), &cur);
            draw_ring(p2x, p2y, rad * 2 / 3, 6 + rad / 20, (256 - (int)(s >> 8)) / 2, &cur);
        }
        if (t >= 1200) {
            unsigned f = (unsigned)(ease_in_out(prog(t, 1200, 1600)) >> 8);
            if (star_sprite_to(0, OFF, 0, 0, 0, f) > 0)
                r_add(&cur, 0, 0, (int)W, (int)H > 260 ? 260 : (int)H);   /* header + label */
            /* the base's other parts (service label, footer) */
            r_add(&cur, 0, 1600, (int)W, 1680);
            r_add(&cur, 0, 2500, (int)W, (int)H);
        }

        struct rect out = prev;
        r_add(&out, cur.x0, cur.y0, cur.x1, cur.y1);
        present(&out);
        prev = cur;
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
