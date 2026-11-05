/* SPDX-License-Identifier: GPL-2.0-only
 * sbui canvas: back buffer, dirty rectangles, presentation, fonts, shapes, icons (sbui.h).
 *
 * Presentation: the back buffer is write-back RAM, the panel write-combined (mmu.c), so drawing
 * (which reads what it blends over) stays in the caches and the panel only ever sees streaming
 * 64-bit stores of finished rows. Dirty rectangles are kept as a short list, merged when they
 * overlap or touch; past UI_MAXDIRTY the list collapses to its bounding box.
 */
#include "sbui.h"
#include "sb7.h"

#define UI_MAXDIRTY 24

extern char sb_assets_start[], sb_assets_end[];   /* assets.S: the embedded SB7 container */

static uint32_t *bb;
static struct ui_rect clip = { 0, 0, UI_W, UI_H };
static struct ui_rect dirty[UI_MAXDIRTY];
static int ndirty;
static const char *init_why = "not initialised";
struct ui_stats ui_stats;

struct font {
    const struct sb7_font *f;
    const struct sb7_glyph *g;
    const struct sb7_kern *k;
    const uint8_t *bits;
};
static struct font FT[SB7_F_COUNT];

/* ---- init ------------------------------------------------------------------------------------ */

static int font_load(unsigned id)
{
    uint32_t sz;
    const struct sb7_font *f = (const struct sb7_font *)sb7_get(SB7_T_FONT, id, &sz);
    if (!f || sz < sizeof(*f))
        return 0;
    if ((uint64_t)f->glyph_off + (uint64_t)f->nglyph * sizeof(struct sb7_glyph) > sz ||
        (uint64_t)f->kern_off + (uint64_t)f->nkern * sizeof(struct sb7_kern) > sz ||
        (uint64_t)f->bits_off + f->bits_size > sz)
        return 0;
    FT[id].f = f;
    FT[id].g = (const struct sb7_glyph *)((const uint8_t *)f + f->glyph_off);
    FT[id].k = (const struct sb7_kern *)((const uint8_t *)f + f->kern_off);
    FT[id].bits = (const uint8_t *)f + f->bits_off;
    return 1;
}

int ui_init(void)
{
    if (bb)
        return 1;
    if (!sb7_open((uintptr_t)sb_assets_start, (uintptr_t)sb_assets_end)) {
        init_why = "embedded SB7 container missing or corrupt";
        return 0;
    }
    static const unsigned need[] = { F_TITLE, F_TITLE_S, F_BIG, F_TILE, F_STATUS, F_ROW, F_ROW_M, F_DETAIL,
                                     F_SMALL, F_HEAD, F_BTN, F_SEG, F_MONO, F_MONO_L };
    for (unsigned i = 0; i < sizeof(need) / sizeof(need[0]); ++i)
        if (!font_load(need[i])) {
            init_why = "a font is missing from the container";
            return 0;
        }
    for (unsigned id = 1; id < SB7_F_COUNT; ++id)
        if (!FT[id].f)
            (void)font_load(id);
    bb = sb_alloc((uint64_t)UI_W * UI_H * 4U);
    if (!bb) {
        init_why = "no RAM for the back buffer";
        return 0;
    }
    for (uint64_t i = 0; i < (uint64_t)UI_W * UI_H; ++i)
        bb[i] = C_BG;
    init_why = 0;
    ui_dirty_all();
    return 1;
}

const char *ui_init_why(void)
{
    return init_why;
}

uint32_t *ui_back(void)
{
    return bb;
}

int ui_has_font(int font)
{
    return font > 0 && font < (int)SB7_F_COUNT && FT[font].f;
}

/* ---- clip and dirty rectangles ----------------------------------------------------------------- */

static struct ui_rect isect(struct ui_rect a, struct ui_rect b)
{
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    struct ui_rect r = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
    return r;
}

static int guard;

void ui_clip(int x, int y, int w, int h)
{
    struct ui_rect p = { guard, guard, UI_W - 2 * guard, UI_H - 2 * guard }, r = { x, y, w, h };
    clip = isect(p, r);
}

/* sbui_w.h: keep drawing out of the armed-mode border */
void ui_guard(int px)
{
    guard = px > 0 ? px : 0;
    ui_noclip();
}

void ui_noclip(void)
{
    ui_clip(0, 0, UI_W, UI_H);
}

struct ui_rect ui_get_clip(void)
{
    return clip;
}

void ui_set_clip(struct ui_rect r)
{
    clip = r;
}

static int touches(struct ui_rect a, struct ui_rect b)
{
    return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
}

static struct ui_rect uni(struct ui_rect a, struct ui_rect b)
{
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    struct ui_rect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

void ui_dirty(int x, int y, int w, int h)
{
    struct ui_rect p = { 0, 0, UI_W, UI_H }, r = { x, y, w, h };
    r = isect(p, r);
    if (!r.w || !r.h)
        return;
    for (int merged = 1; merged;) {
        merged = 0;
        for (int i = 0; i < ndirty; ++i)
            if (touches(dirty[i], r)) {
                r = uni(dirty[i], r);
                dirty[i] = dirty[--ndirty];
                merged = 1;
                break;
            }
    }
    if (ndirty == UI_MAXDIRTY) {
        for (int i = 1; i < ndirty; ++i)
            dirty[0] = uni(dirty[0], dirty[i]);
        ndirty = 1;
        dirty[0] = uni(dirty[0], r);
        return;
    }
    dirty[ndirty++] = r;
}

void ui_dirty_all(void)
{
    ndirty = 0;
    ui_dirty(0, 0, UI_W, UI_H);
}

#ifdef SB_HOST
void host_present_rect(const uint32_t *bb, int x, int y, int w, int h);
#endif

uint64_t ui_present(void)
{
    uint64_t t0 = sb_us(), px = 0;
    for (int i = 0; i < ndirty; ++i) {
        struct ui_rect r = dirty[i];
#ifdef SB_HOST
        host_present_rect(bb, r.x, r.y, r.w, r.h);
#else
        volatile uint32_t *fb = (volatile uint32_t *)SB_FB;
        for (int y = r.y; y < r.y + r.h; ++y) {
            const uint32_t *s = bb + (uint64_t)y * UI_W + r.x;
            volatile uint32_t *d = fb + (uint64_t)y * UI_W + r.x;
            int n = r.w, k = 0;
            if (((uintptr_t)s & 7) && n) {
                d[0] = s[0];
                k = 1;
            }
            for (; k + 4 <= n; k += 4) {
                uint64_t a = *(const uint64_t *)(s + k), b = *(const uint64_t *)(s + k + 2);
                *(volatile uint64_t *)(d + k) = a;
                *(volatile uint64_t *)(d + k + 2) = b;
            }
            for (; k < n; ++k)
                d[k] = s[k];
        }
#endif
        px += (uint64_t)r.w * r.h;
    }
    ndirty = 0;
#ifndef SB_HOST
    __asm__ volatile("dsb sy" : : : "memory");
#endif
    if (px) {
        uint64_t dt = sb_us() - t0;
        ui_stats.frames++;
        ui_stats.px_total += px;
        ui_stats.px_last = px;
        ui_stats.us_last = dt;
        ui_stats.us_total += dt;
        if (dt > ui_stats.us_worst)
            ui_stats.us_worst = dt;
    }
    return px;
}

void ui_scroll_rows(int y0, int y1, int dy)
{
    if (y0 < 0)
        y0 = 0;
    if (y1 > UI_H)
        y1 = UI_H;
    if (!dy || y1 <= y0 || (dy > 0 ? dy : -dy) >= y1 - y0)
        return;
    uint64_t row = UI_W;
    if (dy > 0)
        memmove(bb + (uint64_t)(y0 + dy) * row, bb + (uint64_t)y0 * row, (uint64_t)(y1 - y0 - dy) * row * 4U);
    else
        memmove(bb + (uint64_t)y0 * row, bb + (uint64_t)(y0 - dy) * row, (uint64_t)(y1 - y0 + dy) * row * 4U);
    ui_dirty(0, y0, UI_W, y1 - y0);
}

/* ---- pixels ------------------------------------------------------------------------------------ */

uint32_t ui_mix(uint32_t fg, uint32_t bg, unsigned a)
{
    if (a >= 255)
        return fg | 0xff000000U;
    if (!a)
        return bg;
    uint32_t na = 255U - a;
    uint32_t rb = ((fg & 0xff00ffU) * a + (bg & 0xff00ffU) * na + 0x800080U);
    uint32_t g = ((fg & 0x00ff00U) * a + (bg & 0x00ff00U) * na + 0x008000U);
    rb = ((rb + ((rb >> 8) & 0xff00ffU)) >> 8) & 0xff00ffU;
    g = ((g + ((g >> 8) & 0x00ff00U)) >> 8) & 0x00ff00U;
    return 0xff000000U | rb | g;
}

static inline void px_blend(uint32_t *p, uint32_t c, unsigned a)
{
    *p = ui_mix(c, *p, a);
}

void ui_fill(int x, int y, int w, int h, uint32_t c)
{
    struct ui_rect r = { x, y, w, h };
    r = isect(r, clip);
    if (!r.w || !r.h)
        return;
    unsigned a = c >> 24;
    for (int yy = r.y; yy < r.y + r.h; ++yy) {
        uint32_t *row = bb + (uint64_t)yy * UI_W + r.x;
        if (a == 255) {
            int n = r.w, k = 0;
            if (((uintptr_t)row & 7) && n) {
                row[0] = c;
                k = 1;
            }
            uint64_t cc = (uint64_t)c << 32 | c;
            for (; k + 2 <= n; k += 2)
                *(uint64_t *)(row + k) = cc;
            for (; k < n; ++k)
                row[k] = c;
        } else {
            for (int k = 0; k < r.w; ++k)
                px_blend(row + k, c, a);
        }
    }
    ui_dirty(r.x, r.y, r.w, r.h);
}

void ui_frame(int x, int y, int w, int h, int t, uint32_t c)
{
    ui_fill(x, y, w, t, c);
    ui_fill(x, y + h - t, w, t, c);
    ui_fill(x, y + t, t, h - 2 * t, c);
    ui_fill(x + w - t, y + t, t, h - 2 * t, c);
}

static unsigned isqrt(unsigned v)
{
    unsigned r = 0, b = 1U << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* Coverage (0..255) of a pixel at distance d16/16 px from a circle's centre, radius r. */
static unsigned cover(unsigned d16, int r)
{
    int e = r * 16 - (int)d16 + 8;          /* 16ths of a pixel inside the edge */
    return e <= 0 ? 0 : e >= 16 ? 255 : (unsigned)e * 255U / 16U;
}

void ui_round_c(int x, int y, int w, int h, int r, unsigned corners, uint32_t c)
{
    if (w <= 0 || h <= 0)
        return;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r <= 0 || !corners) {
        ui_fill(x, y, w, h, c);
        return;
    }
    unsigned ca = c >> 24;
    struct ui_rect all = { x, y, w, h }, cr = isect(all, clip);
    if (!cr.w || !cr.h)
        return;
    /* middle band and the side bands between the corners */
    ui_fill(x, y + r, w, h - 2 * r, c);
    ui_fill(x + r, y, w - 2 * r, r, c);
    ui_fill(x + r, y + h - r, w - 2 * r, r, c);
    for (int q = 0; q < 4; ++q) {
        int cx = (q & 1) ? x + w - r : x, cy = (q & 2) ? y + h - r : y;
        int ox = (q & 1) ? x + w - r : x + r, oy = (q & 2) ? y + h - r : y + r;   /* circle centre */
        for (int yy = cy; yy < cy + r; ++yy) {
            if (yy < clip.y || yy >= clip.y + clip.h)
                continue;
            uint32_t *row = bb + (uint64_t)yy * UI_W;
            for (int xx = cx; xx < cx + r; ++xx) {
                if (xx < clip.x || xx >= clip.x + clip.w)
                    continue;
                unsigned a = 255;
                if (corners & (1U << q)) {
                    int dx = 2 * (xx - ox) + 1, dy = 2 * (yy - oy) + 1;
                    unsigned d16 = isqrt((unsigned)(dx * dx + dy * dy) * 64U);   /* 16ths of px */
                    a = cover(d16, r);
                }
                if (a)
                    px_blend(row + xx, c, a * ca / 255U);
            }
        }
    }
    ui_dirty(cr.x, cr.y, cr.w, cr.h);
}

void ui_round(int x, int y, int w, int h, int r, uint32_t c)
{
    ui_round_c(x, y, w, h, r, 15, c);
}

void ui_disc(int cx, int cy, int r, uint32_t c)
{
    unsigned ca = c >> 24;
    for (int yy = cy - r - 1; yy <= cy + r; ++yy) {
        if (yy < clip.y || yy >= clip.y + clip.h)
            continue;
        uint32_t *row = bb + (uint64_t)yy * UI_W;
        for (int xx = cx - r - 1; xx <= cx + r; ++xx) {
            if (xx < clip.x || xx >= clip.x + clip.w)
                continue;
            int dx = 2 * (xx - cx) + 1, dy = 2 * (yy - cy) + 1;
            unsigned a = cover(isqrt((unsigned)(dx * dx + dy * dy) * 64U), r);
            if (a)
                px_blend(row + xx, c, a * ca / 255U);
        }
    }
    ui_dirty(cx - r - 1, cy - r - 1, 2 * r + 2, 2 * r + 2);
}

void ui_ring(int cx, int cy, int r, int width, uint32_t c)
{
    unsigned ca = c >> 24;
    for (int yy = cy - r - 1; yy <= cy + r; ++yy) {
        if (yy < clip.y || yy >= clip.y + clip.h)
            continue;
        uint32_t *row = bb + (uint64_t)yy * UI_W;
        for (int xx = cx - r - 1; xx <= cx + r; ++xx) {
            if (xx < clip.x || xx >= clip.x + clip.w)
                continue;
            int dx = 2 * (xx - cx) + 1, dy = 2 * (yy - cy) + 1;
            unsigned d16 = isqrt((unsigned)(dx * dx + dy * dy) * 64U);
            unsigned a = cover(d16, r), b = cover(d16, r - width);
            a = a > b ? a - b : 0;
            if (a)
                px_blend(row + xx, c, a * ca / 255U);
        }
    }
    ui_dirty(cx - r - 1, cy - r - 1, 2 * r + 2, 2 * r + 2);
}

/* A thick line as overlapping discs of width/2 every pixel (chart strokes). */
void ui_line(int x0, int y0, int x1, int y1, int width, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    int r = width / 2 > 0 ? width / 2 : 1;
    if (!n) {
        ui_fill(x0 - r, y0 - r, 2 * r, 2 * r, c);
        return;
    }
    for (int i = 0; i <= n; ++i)
        ui_fill(x0 + dx * i / n - r, y0 + dy * i / n - r, 2 * r, 2 * r, c);
}

/* ---- text ---------------------------------------------------------------------------------------- */

static int glyph_find(const struct font *F, uint32_t cp)
{
    int lo = 0, hi = (int)F->f->nglyph - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t c = F->g[m].cp;
        if (c == cp)
            return m;
        if (c < cp)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return -1;
}

static int32_t kern_find(const struct font *F, int l, int r)
{
    int lo = 0, hi = (int)F->f->nkern - 1;
    uint32_t key = (uint32_t)l << 16 | (uint32_t)r;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        uint32_t k = (uint32_t)F->k[m].left << 16 | F->k[m].right;
        if (k == key)
            return F->k[m].adj64;
        if (k < key)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}

/* The loader font's one-byte codes (shim/starlte/ui_glyphs.h, the catalog's strings). */
static uint32_t legacy_cp(unsigned b)
{
    switch (b) {
    case 0x80: return 0x00b0;   /* ° */
    case 0x81: return 0x00b7;   /* · */
    case 0x82: return 0x2026;   /* … */
    case 0x83: return 0x2191;
    case 0x84: return 0x2193;
    case 0x85: return 0x2190;
    case 0x86: return 0x2192;
    case 0x87: return 0x00b5;
    case 0x88: return 0x00b1;
    case 0x89: return 0x00d7;
    case 0x8a: return 0x25a0;
    case 0x8b: return 0x25b2;
    case 0x8c: return 0x25bc;
    case 0x8d: return 0x2713;
    default: return '?';
    }
}

static uint32_t utf8_next(const char *s, unsigned *i)
{
    const uint8_t *u = (const uint8_t *)s;
    uint32_t c = u[(*i)++];
    if (c < 0x80)
        return c;
    if (c < 0xc0)
        return legacy_cp(c);
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
    c &= 0x3fU >> more;
    while (more--) {
        if ((u[*i] & 0xc0U) != 0x80U)
            return '?';
        c = c << 6 | (u[(*i)++] & 0x3fU);
    }
    return c;
}

struct pen {
    const struct font *F;
    int32_t x64;
    int prev;
};

/* Advance over code point cp; returns the glyph index or -1, *at = its pen x in 1/64 px. */
static int pen_step(struct pen *p, uint32_t cp, int32_t *at)
{
    int g = glyph_find(p->F, cp);
    if (g < 0)
        g = glyph_find(p->F, '?');
    if (g < 0)
        return -1;
    if (p->prev >= 0)
        p->x64 += kern_find(p->F, p->prev, g);
    *at = p->x64;
    p->x64 += p->F->g[g].adv64;
    p->prev = g;
    return g;
}

static const struct font *font_of(int font)
{
    if (font <= 0 || font >= (int)SB7_F_COUNT || !FT[font].f)
        font = F_ROW;
    return FT[font].f ? &FT[font] : 0;
}

int ui_text_w(int font, const char *s)
{
    const struct font *F = font_of(font);
    if (!F || !s)
        return 0;
    struct pen p = { F, 0, -1 };
    int32_t at;
    for (unsigned i = 0; s[i];)
        (void)pen_step(&p, utf8_next(s, &i), &at);
    return (p.x64 + 32) >> 6;
}

unsigned ui_text_fit(int font, const char *s, int w)
{
    const struct font *F = font_of(font);
    if (!F || !s)
        return 0;
    struct pen p = { F, 0, -1 };
    int32_t at;
    unsigned i = 0, ok = 0;
    while (s[i]) {
        (void)pen_step(&p, utf8_next(s, &i), &at);
        if ((p.x64 >> 6) > w)
            break;
        ok = i;
    }
    return ok;
}

int ui_ascent(int font)
{
    const struct font *F = font_of(font);
    return F ? F->f->asc64 >> 6 : 0;
}

int ui_px(int font)
{
    const struct font *F = font_of(font);
    return F ? F->f->px : 0;
}

static int draw_run(const struct font *F, int x, int y, const char *s, unsigned n, uint32_t c)
{
    struct pen p = { F, 0, -1 };
    unsigned ca = c >> 24;
    int minx = UI_W, maxx = 0, miny = UI_H, maxy = 0;
    for (unsigned i = 0; i < n && s[i];) {
        int32_t at;
        int g = pen_step(&p, utf8_next(s, &i), &at);
        if (g < 0)
            continue;
        const struct sb7_glyph *G = &F->g[g];
        if (!G->w || !G->h)
            continue;
        int gx = x + ((at + 32) >> 6) + G->bx, gy = y + G->by;
        const uint8_t *bits = F->bits + G->off;
        int x0 = gx < clip.x ? clip.x : gx, x1 = gx + G->w > clip.x + clip.w ? clip.x + clip.w : gx + G->w;
        int y0 = gy < clip.y ? clip.y : gy, y1 = gy + G->h > clip.y + clip.h ? clip.y + clip.h : gy + G->h;
        for (int yy = y0; yy < y1; ++yy) {
            uint32_t *row = bb + (uint64_t)yy * UI_W;
            const uint8_t *br = bits + (uint64_t)(yy - gy) * G->w - gx;
            for (int xx = x0; xx < x1; ++xx) {
                unsigned a = br[xx];
                if (a)
                    px_blend(row + xx, c, ca == 255 ? a : a * ca / 255U);
            }
        }
        if (x0 < x1 && y0 < y1) {
            minx = x0 < minx ? x0 : minx;
            maxx = x1 > maxx ? x1 : maxx;
            miny = y0 < miny ? y0 : miny;
            maxy = y1 > maxy ? y1 : maxy;
        }
    }
    if (maxx > minx)
        ui_dirty(minx, miny, maxx - minx, maxy - miny);
    return x + ((p.x64 + 32) >> 6);
}

int ui_text(int font, int x, int y, const char *s, uint32_t c)
{
    const struct font *F = font_of(font);
    if (!F || !s)
        return x;
    return draw_run(F, x, y, s, 0xffffffffU, c);
}

int ui_text_r(int font, int right, int y, const char *s, uint32_t c)
{
    return ui_text(font, right - ui_text_w(font, s), y, s, c);
}

int ui_text_c(int font, int cx, int y, const char *s, uint32_t c)
{
    return ui_text(font, cx - ui_text_w(font, s) / 2, y, s, c);
}

int ui_text_cut(int font, int x, int y, const char *s, int w, uint32_t c)
{
    const struct font *F = font_of(font);
    if (!F || !s)
        return x;
    if (ui_text_w(font, s) <= w)
        return ui_text(font, x, y, s, c);
    int ew = ui_text_w(font, U_ELL);
    unsigned n = ui_text_fit(font, s, w - ew);
    x = draw_run(F, x, y, s, n, c);
    return ui_text(font, x, y, U_ELL, c);
}

/* ---- icons --------------------------------------------------------------------------------------- */

struct icon {
    uint16_t w, h, nx, ny;
    uint8_t fmt;
    const uint8_t *px;              /* decoded: w * h bytes (A8) or w * h * 4 (ARGB premultiplied) */
};
#define MAX_ICON 128U
static struct icon icons[MAX_ICON];

static const struct icon *icon_get(unsigned id)
{
    if (id >= MAX_ICON)
        return 0;
    struct icon *I = &icons[id];
    if (I->px)
        return I;
    uint32_t sz;
    const struct sb7_image *im = (const struct sb7_image *)sb7_get(SB7_T_IMAGE, id, &sz);
    if (!im || sz < sizeof(*im) || im->data_size > sz - sizeof(*im))
        return 0;
    unsigned bpp = im->format == SB7_ARGB ? 4U : 1U;
    uint64_t need = (uint64_t)im->w * im->h * bpp;
    const uint8_t *src = (const uint8_t *)(im + 1);
    if (im->enc == SB7_RAW) {
        if (need > im->data_size)
            return 0;
        I->px = src;
    } else {
        uint8_t *out = sb_alloc(need);
        if (!out)
            return 0;
        uint64_t o = 0, i = 0, end = im->data_size;
        while (o < need && i + 2 <= end) {
            unsigned c = src[i] | (unsigned)src[i + 1] << 8;
            i += 2;
            if (c < 0x8000U) {
                uint64_t n = (uint64_t)c * bpp;
                if (i + n > end || o + n > need)
                    break;
                memcpy(out + o, src + i, n);
                o += n;
                i += n;
            } else {
                uint64_t n = c - 0x8000U;
                if (i + bpp > end)
                    break;
                for (uint64_t k = 0; k < n && o + bpp <= need; ++k, o += bpp)
                    memcpy(out + o, src + i, bpp);
                i += bpp;
            }
            i = (i + 1) & ~1ULL;
        }
        if (o < need)
            memset(out + o, 0, need - o);
        I->px = out;
    }
    I->w = im->w;
    I->h = im->h;
    I->nx = im->nx;
    I->ny = im->ny;
    I->fmt = im->format;
    return I;
}

int ui_icon_size(unsigned id, int *w, int *h)
{
    uint32_t sz;
    const struct sb7_image *im = (const struct sb7_image *)sb7_get(SB7_T_IMAGE, id, &sz);
    if (!im || sz < sizeof(*im))
        return 0;
    *w = im->nw;
    *h = im->nh;
    return 1;
}

int ui_icon(unsigned id, int x, int y, uint32_t c)
{
    const struct icon *I = icon_get(id);
    if (!I)
        return 0;
    int ox = x - I->nx, oy = y - I->ny;
    unsigned ca = c >> 24;
    for (int r = 0; r < I->h; ++r) {
        int yy = oy + r;
        if (yy < clip.y || yy >= clip.y + clip.h)
            continue;
        uint32_t *row = bb + (uint64_t)yy * UI_W;
        for (int k = 0; k < I->w; ++k) {
            int xx = ox + k;
            if (xx < clip.x || xx >= clip.x + clip.w)
                continue;
            if (I->fmt == SB7_ARGB) {
                uint32_t p = ((const uint32_t *)I->px)[(uint64_t)r * I->w + k];
                unsigned a = p >> 24;
                if (!a)
                    continue;
                uint32_t d = row[xx], o = 0xff000000U;
                for (unsigned sh = 0; sh < 24; sh += 8) {
                    unsigned v = ((p >> sh) & 255U) + ((d >> sh) & 255U) * (255U - a) / 255U;
                    o |= (v > 255 ? 255U : v) << sh;
                }
                row[xx] = o;
            } else {
                unsigned a = I->px[(uint64_t)r * I->w + k];
                if (a)
                    px_blend(row + xx, c, a * ca / 255U);
            }
        }
    }
    ui_dirty(ox, oy, I->w, I->h);
    return 1;
}
