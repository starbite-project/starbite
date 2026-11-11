/* SPDX-License-Identifier: MIT
 * sbtk text: stb_truetype (public domain, third_party/) rasterising static instances of Inter Tight 400..800 and
 * Outfit 300 (OFL; tools/brand/fonts) into layer bitmaps when the text changes. The little layout Chrome does for
 * the concept is reproduced from the .lay tables (tools/sbtk-fonts.py): pair kerning, tabular figures, and the
 * raised colon between figures. Line boxes follow Blink: ascent and descent rounded to whole CSS px (pt here),
 * half-leading around them. */
#include "sbtk.h"

#define STBTT_ifloor(x) ((int)tk_floorf((float)(x)))
#define STBTT_iceil(x) (-(int)tk_floorf(-(float)(x)))
#define STBTT_sqrt(x) tk_sqrtf((float)(x))
#define STBTT_pow(x, y) tk_powf((float)(x), (float)(y))
#define STBTT_fmod(x, y) tk_fmodf((float)(x), (float)(y))
#define STBTT_cos(x) tk_cosf((float)(x))
#define STBTT_acos(x) tk_acosf((float)(x))
#define STBTT_fabs(x) tk_fabsf((float)(x))
#define STBTT_malloc(x, u) ((void)(u), tk_alloc(x))
#define STBTT_free(x, u) ((void)(u), tk_free(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) tk_strlen(x)
#define STBTT_memcpy tk_memcpy
#define STBTT_memset tk_memset
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "third_party/stb_truetype.h"
#pragma GCC diagnostic pop

static struct font {
    stbtt_fontinfo info;
    int ok, upm, asc, desc, gap;
    const uint8_t *pairs;          /* npairs x (u32 key, i32 adj) */
    uint32_t npairs, colon_case;
    uint32_t tn_cp[16], tn_g[16], ntn;
} F[TK_NFONT];

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int tk_font_load(int id, const uint8_t *ttf, const uint8_t *lay, uint32_t laylen)
{
    struct font *f = &F[id];
    if (!stbtt_InitFont(&f->info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) return 0;
    stbtt_GetFontVMetrics(&f->info, &f->asc, &f->desc, &f->gap);
    f->upm = ttUSHORT(f->info.data + f->info.head + 18);
    if (lay && laylen >= 20 && lay[0] == 'S') {
        f->npairs = rd32(lay + 8);
        uint32_t nt = rd32(lay + 12);
        f->colon_case = rd32(lay + 16);
        const uint8_t *p = lay + 20;
        for (uint32_t i = 0; i < nt && i < 16; i++) {
            f->tn_cp[i] = rd32(p + 8 * i);
            f->tn_g[i] = rd32(p + 8 * i + 4);
            f->ntn = i + 1;
        }
        f->pairs = p + 8 * nt;
        if (20 + 8 * nt + 8 * f->npairs > laylen) f->npairs = 0;
    }
    f->ok = 1;
    return 1;
}

static float pt_round(float v) { return tk_floorf(v + 0.5f); }
float tk_ascent(int font, float size)
{
    const struct font *f = &F[font];
    return pt_round((float)f->asc / f->upm * size / TK_PT) * TK_PT;
}
float tk_descent(int font, float size)
{
    const struct font *f = &F[font];
    return pt_round((float)-f->desc / f->upm * size / TK_PT) * TK_PT;
}
float tk_normal_lh(int font, float size)
{
    const struct font *f = &F[font];
    return tk_ascent(font, size) + tk_descent(font, size) + pt_round((float)f->gap / f->upm * size / TK_PT) * TK_PT;
}
float tk_baseline(int font, float size, float top, float lh)
{
    float a = tk_ascent(font, size), d = tk_descent(font, size);
    return top + (lh - (a + d)) / 2 + a;
}

static uint32_t utf8(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = *s++;
    if (c >= 0xf0) { c = (c & 7) << 18 | (s[0] & 63u) << 12 | (s[1] & 63u) << 6 | (s[2] & 63u); s += 3; }
    else if (c >= 0xe0) { c = (c & 15) << 12 | (s[0] & 63u) << 6 | (s[1] & 63u); s += 2; }
    else if (c >= 0xc0) { c = (c & 31) << 6 | (s[0] & 63u); s += 1; }
    *ps = (const char *)s;
    return c;
}

/* the glyph run: glyph ids of s with the concept's layout rules (as Blink: letter-spacing turns calt off) */
static int run(const struct font *f, const char *s, int flags, float ls, int *g, uint32_t *cp, int max)
{
    int n = 0;
    while (*s && n < max) {
        uint32_t c = utf8(&s);
        cp[n] = c;
        g[n] = stbtt_FindGlyphIndex(&f->info, (int)c);
        if (flags & TK_TNUM)
            for (uint32_t k = 0; k < f->ntn; k++)
                if (f->tn_cp[k] == c) g[n] = (int)f->tn_g[k];
        n++;
    }
    if (ls == 0)
        for (int i = 1; i + 1 < n; i++)   /* calt: a colon between two figures is raised */
        if (cp[i] == ':' && cp[i - 1] >= '0' && cp[i - 1] <= '9' && cp[i + 1] >= '0' && cp[i + 1] <= '9' && f->colon_case)
            g[i] = (int)f->colon_case;
    return n;
}
static int kern(const struct font *f, int a, int b)
{
    uint32_t key = (uint32_t)a << 16 | (uint32_t)b, lo = 0, hi = f->npairs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, k = rd32(f->pairs + 8 * mid);
        if (k == key) return (int32_t)rd32(f->pairs + 8 * mid + 4);
        if (k < key) lo = mid + 1; else hi = mid;
    }
    return 0;
}

float tk_text_w(int font, float size, const char *s, float ls, int flags)
{
    const struct font *f = &F[font];
    int g[256];
    uint32_t cp[256];
    int n = run(f, s, flags, ls, g, cp, 256);
    float sc = size / f->upm, w = 0;
    for (int i = 0; i < n; i++) {
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f->info, g[i], &adv, &lsb);
        w += (adv + (i + 1 < n ? kern(f, g[i], g[i + 1]) : 0)) * sc + (i + 1 < n ? ls : 0);   /* P3i: no tracking after the last glyph (it shifted centred text by ls / 2) */
    }
    return w;
}

void tk_text_fm(tk_fm *m, int font, float size, float x, float baseline, const char *s, float ls, int flags)
{
    const struct font *f = &F[font];
    int g[256];
    uint32_t cp[256];
    int n = run(f, s, flags, ls, g, cp, 256);
    float sc = size / f->upm, pen = x;
    baseline = tk_floorf(baseline + .5f);
    int by = (int)tk_floorf(baseline);
    float fy = baseline - by;
    for (int i = 0; i < n; i++) {
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetGlyphHMetrics(&f->info, g[i], &adv, &lsb);
        int px = (int)tk_floorf(pen);
        float fx = pen - px;
        stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g[i], sc, sc, fx, fy, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            uint8_t *bm = tk_alloc((size_t)w * h);
            if (!bm) { pen += (adv + (i + 1 < n ? kern(f, g[i], g[i + 1]) : 0)) * sc + ls; continue; }
            stbtt_MakeGlyphBitmapSubpixel(&f->info, bm, w, h, w, sc, sc, fx, fy, g[i]);
            for (int j = 0; j < h; j++) {
                int yy = by + y0 + j;
                if (yy < 0 || yy >= m->h) continue;
                for (int k = 0; k < w; k++) {
                    int xx = px + x0 + k;
                    if (xx < 0 || xx >= m->w || !bm[j * w + k]) continue;
                    float c = bm[j * w + k] / 255.0f, *d = &m->v[yy * m->w + xx];
                    *d = *d + c - *d * c;
                }
            }
            tk_free(bm);
        }
        pen += (adv + (i + 1 < n ? kern(f, g[i], g[i + 1]) : 0)) * sc + ls;
    }
}

void tk_text(tk_bmp *b, int font, float size, float x, float baseline, const char *s, float ls, int flags,
             const tk_paint *p, const tk_shadow *sh)
{
    float w = tk_text_w(font, size, s, ls, flags) + size;
    float mg = sh && sh->c.a > 0 ? 3 * sh->blur / 2 + tk_fabsf(sh->dx) + tk_fabsf(sh->dy) + 4 : 2;
    int ox = (int)tk_floorf(x - size * 0.5f - mg), oy = (int)tk_floorf(baseline - size * 1.3f - mg);
    int W = (int)(w + size + 2 * mg), H = (int)(size * 1.8f + 2 * mg);
    tk_fm m;
    if (!tk_fm_new(&m, W, H)) return;
    tk_text_fm(&m, font, size, x - ox, baseline - oy, s, ls, flags);
    if (sh && sh->c.a > 0) {
        tk_fm t;
        if (tk_fm_new(&t, W, H)) {
            tk_fm_shift(&t, &m, sh->dx, sh->dy);
            tk_fm_blur(&t, sh->blur / 2);
            tk_paint sp = tk_solid(sh->c);
            tk_draw_fm(b, &t, ox, oy, &sp, 1);
            tk_fm_free(&t);
        }
    }
    tk_draw_fm(b, &m, ox, oy, p, 1);
    tk_fm_free(&m);
}
