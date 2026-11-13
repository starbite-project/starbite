/* SPDX-License-Identifier: MIT
 * sbtk bake-time drawing: CSS paints (solid, linear, radial; premultiplied interpolation as CSS Images 4), coverage
 * planes (rounded rects and circles by their distance field, stroked arcs with round caps), gaussian blur as three
 * box passes, CSS box-shadow (outer clipped to outside the box, inset), images, and the backdrop filters. All of it
 * runs when a layer's content changes, never per frame, so it is float and plain C. */
#include "sbtk.h"
#include <arm_neon.h>

tk_col tk_hex(uint32_t rgb, float a) { return TK_RGBA((float)(rgb >> 16 & 255), (float)(rgb >> 8 & 255), (float)(rgb & 255), a); }

static inline uint32_t pack(float r, float g, float b, float a)      /* premultiplied floats (rgb 0..255, a 0..1) */
{
    int A = (int)(a * 255.0f + 0.5f), R = (int)(r + 0.5f), G = (int)(g + 0.5f), B = (int)(b + 0.5f);
    A = A < 0 ? 0 : A > 255 ? 255 : A;
    R = R < 0 ? 0 : R > A ? A : R;
    G = G < 0 ? 0 : G > A ? A : G;
    B = B < 0 ? 0 : B > A ? A : B;
    return (uint32_t)A << 24 | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
}
uint32_t tk_premul(tk_col c) { return pack(c.r * c.a, c.g * c.a, c.b * c.a, c.a); }

int tk_bmp_new(tk_bmp *b, int w, int h)
{
    /* on no memory: an empty bitmap (w = h = 0), which every drawing call skips */
    b->px = w > 0 && h > 0 ? tk_alloc((size_t)w * h * 4) : 0;
    b->w = b->px ? w : 0; b->h = b->px ? h : 0; b->stride = b->w;
    return b->px != 0;
}
void tk_bmp_free(tk_bmp *b) { if (b->px) tk_free(b->px); b->px = 0; b->w = b->h = 0; }
/* cp7: src turned exactly (no resampling) a quarter clockwise (q 1), a half (2) or a quarter counter-clockwise (3)
 * into a new dst (q 0: a copy). 0: no memory (dst empty) */
int tk_bmp_turned(tk_bmp *dst, const tk_bmp *src, int q)
{
    q &= 3;
    int w = src->w, h = src->h;
    if (!src->px || !tk_bmp_new(dst, q & 1 ? h : w, q & 1 ? w : h)) return 0;
    for (int y = 0; y < h; y++) {
        const uint32_t *s = src->px + (size_t)y * src->stride;
        if (!q) tk_memcpy(dst->px + (size_t)y * dst->stride, s, (size_t)w * 4);
        else if (q == 2) {
            uint32_t *d = dst->px + (size_t)(h - 1 - y) * dst->stride + (w - 1);
            for (int x = 0; x < w; x++) *d-- = s[x];
        } else if (q == 1) {
            uint32_t *d = dst->px + (h - 1 - y);                    /* column h-1-y, from the top */
            for (int x = 0; x < w; x++, d += dst->stride) *d = s[x];
        } else {
            uint32_t *d = dst->px + (size_t)(w - 1) * dst->stride + y; /* column y, from the bottom */
            for (int x = 0; x < w; x++, d -= dst->stride) *d = s[x];
        }
    }
    return 1;
}
/* b turned in place (a new bitmap takes its place: both exist for a moment). 0: no memory (b as it was) */
int tk_bmp_turn(tk_bmp *b, int q)
{
    if (!(q & 3) || !b->px) return 1;
    tk_bmp o;
    if (!tk_bmp_turned(&o, b, q)) return 0;
    tk_bmp_free(b);
    *b = o;
    return 1;
}
int tk_fm_new(tk_fm *m, int w, int h)
{
    m->v = w > 0 && h > 0 ? tk_alloc((size_t)w * h * 4) : 0;
    m->w = m->v ? w : 0; m->h = m->v ? h : 0;
    return m->v != 0;
}
void tk_fm_free(tk_fm *m) { if (m->v) tk_free(m->v); m->v = 0; m->w = m->h = 0; }
int tk_a8_new(tk_a8 *m, int w, int h)
{
    m->v = w > 0 && h > 0 ? tk_alloc((size_t)w * h) : 0;
    m->w = m->v ? w : 0; m->h = m->v ? h : 0;
    return m->v != 0;
}
void tk_a8_free(tk_a8 *m) { if (m->v) tk_free(m->v); m->v = 0; m->w = m->h = 0; }

/* ---- paints ----------------------------------------------------------------------------------------------- */
tk_paint tk_solid(tk_col c) { tk_paint p; tk_memset(&p, 0, sizeof p); p.kind = TK_P_SOLID; p.solid = c; return p; }
static void stops(tk_paint *p, int n, const tk_stop *s)
{
    p->n = n > TK_MAXSTOP ? TK_MAXSTOP : n;
    for (int i = 0; i < p->n; i++) p->s[i] = s[i];
}
tk_paint tk_linear(float angle, float bx, float by, float bw, float bh, int n, const tk_stop *s)
{
    tk_paint p = tk_solid(TK_RGBA(0, 0, 0, 0));
    p.kind = TK_P_LINEAR; p.angle = angle; p.bx = bx; p.by = by; p.bw = bw; p.bh = bh;
    float a = angle * 0.0174532925f;
    p.rx = tk_sinf(a); p.ry = -tk_cosf(a);
    p.cx = tk_fabsf(bw * p.rx) + tk_fabsf(bh * p.ry);
    stops(&p, n, s);
    return p;
}
tk_paint tk_radial(float rx, float ry, float cx, float cy, float bx, float by, float bw, float bh, int n, const tk_stop *s)
{
    tk_paint p = tk_solid(TK_RGBA(0, 0, 0, 0));
    p.kind = TK_P_RADIAL; p.rx = rx; p.ry = ry; p.cx = cx; p.cy = cy; p.bx = bx; p.by = by; p.bw = bw; p.bh = bh;
    stops(&p, n, s);
    return p;
}
static tk_col stop_at(const tk_paint *p, float t)          /* premultiplied */
{
    const tk_stop *s = p->s;
    int i = 0;
    if (t <= s[0].pos) { tk_col c = s[0].c; return TK_RGBA(c.r * c.a, c.g * c.a, c.b * c.a, c.a); }
    while (i + 1 < p->n && t > s[i + 1].pos) i++;
    if (i + 1 >= p->n) { tk_col c = s[p->n - 1].c; return TK_RGBA(c.r * c.a, c.g * c.a, c.b * c.a, c.a); }
    float f = (t - s[i].pos) / tk_maxf(1e-6f, s[i + 1].pos - s[i].pos);
    tk_col a = s[i].c, b = s[i + 1].c;
    return TK_RGBA(a.r * a.a + (b.r * b.a - a.r * a.a) * f, a.g * a.a + (b.g * b.a - a.g * a.a) * f,
                   a.b * a.a + (b.b * b.a - a.b * a.a) * f, a.a + (b.a - a.a) * f);
}
tk_col tk_paint_at(const tk_paint *p, float x, float y)
{
    if (p->kind == TK_P_SOLID) { tk_col c = p->solid; return TK_RGBA(c.r * c.a, c.g * c.a, c.b * c.a, c.a); }
    if (p->kind == TK_P_LINEAR) {
        float sx = p->rx, sy = p->ry, L = p->cx;      /* direction and length, made by tk_linear */
        float t = ((x - p->bx - p->bw / 2) * sx + (y - p->by - p->bh / 2) * sy) / L + 0.5f;
        return stop_at(p, t);
    }
    float dx = (x - p->bx - p->cx * p->bw) / (p->rx * p->bw), dy = (y - p->by - p->cy * p->bh) / (p->ry * p->bh);
    return stop_at(p, tk_sqrtf(dx * dx + dy * dy));
}

/* ---- coverage planes ------------------------------------------------------------------------------------- */
void tk_fm_clear(tk_fm *m, float v) { for (int i = 0; i < m->w * m->h; i++) m->v[i] = v; }

tk_r4 tk_corner = { 0, 0, 0, 0 };
int tk_use_r4;

/* the signed distance to a rounded box with explicit radii per corner */
float tk_sd_rrect4(float px, float py, float x, float y, float w, float h, const tk_r4 *k)
{
    float tl = k->tl, tr = k->tr, br = k->br, bl = k->bl;
    float cx, cy, rr = -1;
    if (px < x + tl && py < y + tl) { cx = x + tl; cy = y + tl; rr = tl; }
    else if (px > x + w - tr && py < y + tr) { cx = x + w - tr; cy = y + tr; rr = tr; }
    else if (px > x + w - br && py > y + h - br) { cx = x + w - br; cy = y + h - br; rr = br; }
    else if (px < x + bl && py > y + h - bl) { cx = x + bl; cy = y + h - bl; rr = bl; }
    if (rr > 0) {
        float dx = px - cx, dy = py - cy;
        return tk_sqrtf(dx * dx + dy * dy) - rr;
    }
    return tk_maxf(tk_maxf(x - px, px - (x + w)), tk_maxf(y - py, py - (y + h)));
}
/* the radii a box draws with: per corner while tk_use_r4, else r (CSS scaling: at most half the shorter side) */
tk_r4 tk_radii(float w, float h, float r)
{
    if (tk_use_r4) return tk_corner;
    float m = tk_minf(w, h) / 2;
    if (r > m) r = m;
    return (tk_r4){ r, r, r, r };
}
float tk_sd_rrect(float px, float py, float x, float y, float w, float h, float r)
{
    tk_r4 k = tk_radii(w, h, r);
    return tk_sd_rrect4(px, py, x, y, w, h, &k);
}

/* the platform's parallel run (the kernel: the four A55); NULL runs the bands here */
void (*tk_par)(void (*fn)(void *, int), void *arg, int n);
typedef struct { void (*fn)(void *, int, int); void *ctx; int y0, y1; } rows_job;
static void rows_band(void *a, int i)
{
    rows_job *J = a;
    int h = J->y1 - J->y0, y0 = J->y0 + h * i / 8, y1 = J->y0 + h * (i + 1) / 8;
    if (y1 > y0) J->fn(J->ctx, y0, y1);
}
void tk_par_rows(int y0, int y1, void (*fn)(void *, int, int), void *ctx)
{
    rows_job J = { fn, ctx, y0, y1 };
    if (tk_par && y1 - y0 >= 64) tk_par(rows_band, &J, 8);
    else fn(ctx, y0, y1);
}
void tk_fm_rrect(tk_fm *m, float ox, float oy, float x, float y, float w, float h, float r)
{
    if (w <= 0 || h <= 0) return;
    int y0 = tk_maxi(0, (int)tk_floorf(y - oy - 1)), y1 = tk_mini(m->h, (int)(y - oy + h) + 2);
    int x0 = tk_maxi(0, (int)tk_floorf(x - ox - 1)), x1 = tk_mini(m->w, (int)(x - ox + w) + 2);
    float rm = tk_use_r4 ? tk_maxf(tk_maxf(tk_corner.tl, tk_corner.tr), tk_maxf(tk_corner.br, tk_corner.bl)) : tk_minf(r, tk_minf(w, h) / 2);
    for (int j = y0; j < y1; j++) {
        float py = j + 0.5f + oy;
        /* where the boundary can be on this row: within rm of a side (corners) or the row is near the top / bottom */
        float in = 0;
        if (py < y + rm) { float d = y + rm - py; in = rm - tk_sqrtf(tk_maxf(0, rm * rm - d * d)); }
        else if (py > y + h - rm) { float d = py - (y + h - rm); in = rm - tk_sqrtf(tk_maxf(0, rm * rm - d * d)); }
        int edge = py < y + 1.5f || py > y + h - 1.5f;
        int a = edge ? x1 : tk_maxi(x0, (int)tk_floorf(x - ox + in + 1.5f)), b = edge ? x1 : tk_mini(x1, (int)tk_floorf(x - ox + w - in - 1.5f));
        if (a > b) a = b = x1;
        float *row = m->v + j * m->w;
        for (int i = a; i < b; i++) row[i] = 1;
        for (int i = x0; i < x1; i++) {
            if (i == a) { i = b - 1; continue; }
            float c = tk_clampf(0.5f - tk_sd_rrect(i + 0.5f + ox, py, x, y, w, h, r), 0, 1);
            if (c > row[i]) row[i] = c;
        }
    }
}
void tk_fm_circle(tk_fm *m, float ox, float oy, float cx, float cy, float r)
{
    tk_fm_rrect(m, ox, oy, cx - r, cy - r, 2 * r, 2 * r, r);
}
void tk_fm_arc(tk_fm *m, float ox, float oy, float cx, float cy, float r, float sw, float a0, float sweep, int caps)
{
    const float tau = 6.28318531f;
    float e0x = cx + r * tk_sinf(a0), e0y = cy - r * tk_cosf(a0);
    float e1x = cx + r * tk_sinf(a0 + sweep), e1y = cy - r * tk_cosf(a0 + sweep);
    int y0 = tk_maxi(0, (int)(cy - oy - r - sw - 2)), y1 = tk_mini(m->h, (int)(cy - oy + r + sw + 3));
    int x0 = tk_maxi(0, (int)(cx - ox - r - sw - 2)), x1 = tk_mini(m->w, (int)(cx - ox + r + sw + 3));
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            float px = i + 0.5f + ox, py = j + 0.5f + oy, dx = px - cx, dy = py - cy;
            float phi = tk_atan2f(dx, -dy);            /* 0 at 12 o'clock, clockwise */
            if (phi < 0) phi += tau;
            float rel = phi - a0;
            while (rel < 0) rel += tau;
            while (rel >= tau) rel -= tau;
            float sd;
            if (sweep >= tau || rel <= sweep)
                sd = tk_fabsf(tk_sqrtf(dx * dx + dy * dy) - r) - sw / 2;
            else if (caps) {
                float d0 = tk_sqrtf((px - e0x) * (px - e0x) + (py - e0y) * (py - e0y));
                float d1 = tk_sqrtf((px - e1x) * (px - e1x) + (py - e1y) * (py - e1y));
                sd = tk_minf(d0, d1) - sw / 2;
            } else
                continue;
            float c = tk_clampf(0.5f - sd, 0, 1);
            float *d = &m->v[j * m->w + i];
            if (c > *d) *d = c;
        }
}

/* box sizes for a gaussian of sigma as three box passes (W3C filter effects) */
static void boxes(float sigma, int r[3])
{
    float wi = tk_sqrtf(12 * sigma * sigma / 3 + 1);
    int wl = (int)tk_floorf(wi);
    if (!(wl & 1)) wl--;
    int wu = wl + 2;
    float mi = (12 * sigma * sigma - 3 * wl * wl - 4 * 3 * wl - 3 * 3) / (-4 * wl - 4);
    int mm = (int)(mi + 0.5f);
    for (int i = 0; i < 3; i++) r[i] = ((i < mm ? wl : wu) - 1) / 2;
}
static void box1(const float *in, float *out, int n, int stride, int r)   /* zero outside */
{
    float acc = 0, inv = 1.0f / (2 * r + 1);
    for (int i = 0; i < r && i < n; i++) acc += in[i * stride];
    for (int i = 0; i < n; i++) {
        if (i + r < n) acc += in[(i + r) * stride];
        if (i - r - 1 >= 0) acc -= in[(i - r - 1) * stride];
        out[i * stride] = acc * inv;
    }
}
static void fm_blur_full(tk_fm *m, float sigma);
void tk_fm_blur(tk_fm *m, float sigma)
{
    if (sigma < 0.3f) return;
    if (sigma < 8 || m->w < 16 || m->h < 16) { fm_blur_full(m, sigma); return; }
    /* a wide gaussian loses nothing at a quarter of the resolution: box down 4x, blur sigma / 4, bilinear up */
    tk_fm q;
    int W = (m->w + 3) / 4, H = (m->h + 3) / 4;
    if (!tk_fm_new(&q, W, H)) { fm_blur_full(m, sigma); return; }
    for (int j = 0; j < m->h; j++)
        for (int i = 0; i < m->w; i++) q.v[(j >> 2) * W + (i >> 2)] += m->v[j * m->w + i] * (1.0f / 16);
    fm_blur_full(&q, sigma / 4);
    for (int j = 0; j < m->h; j++) {
        float fy = (j + .5f) / 4 - .5f;
        int ya = (int)tk_floorf(fy);
        float ty = fy - ya;
        int y0 = tk_maxi(0, ya), y1 = tk_mini(H - 1, ya + 1);
        for (int i = 0; i < m->w; i++) {
            float fx = (i + .5f) / 4 - .5f;
            int xa = (int)tk_floorf(fx);
            float tx = fx - xa;
            int x0 = tk_maxi(0, xa), x1 = tk_mini(W - 1, xa + 1);
            m->v[j * m->w + i] = (q.v[y0 * W + x0] * (1 - tx) + q.v[y0 * W + x1] * tx) * (1 - ty) + (q.v[y1 * W + x0] * (1 - tx) + q.v[y1 * W + x1] * tx) * ty;
        }
    }
    tk_fm_free(&q);
}
static void fm_blur_full(tk_fm *m, float sigma)
{
    int r[3];
    boxes(sigma, r);
    int n = tk_maxi(m->w, m->h);
    float *t = tk_alloc((size_t)n * 4), *u = tk_alloc((size_t)n * 4);
    if (!t || !u) { tk_free(t); tk_free(u); return; }
    for (int j = 0; j < m->h; j++) {
        float *row = m->v + j * m->w;
        for (int p = 0; p < 3; p++) {
            box1(row, t, m->w, 1, r[p]);
            tk_memcpy(row, t, (size_t)m->w * 4);
        }
    }
    for (int i = 0; i < m->w; i++) {
        for (int j = 0; j < m->h; j++) u[j] = m->v[j * m->w + i];
        for (int p = 0; p < 3; p++) {
            box1(u, t, m->h, 1, r[p]);
            tk_memcpy(u, t, (size_t)m->h * 4);
        }
        for (int j = 0; j < m->h; j++) m->v[j * m->w + i] = u[j];
    }
    tk_free(t);
    tk_free(u);
}
void tk_fm_mul(tk_fm *m, const tk_fm *k, int invert)
{
    for (int i = 0; i < m->w * m->h; i++) m->v[i] *= invert ? 1 - k->v[i] : k->v[i];
}
void tk_fm_shift(tk_fm *dst, const tk_fm *src, float dx, float dy)
{
    for (int j = 0; j < dst->h; j++)
        for (int i = 0; i < dst->w; i++) {
            float sx = i - dx, sy = j - dy;
            int x0 = (int)tk_floorf(sx), y0 = (int)tk_floorf(sy);
            float fx = sx - x0, fy = sy - y0, v = 0;
            for (int b = 0; b < 2; b++)
                for (int a = 0; a < 2; a++) {
                    int xx = x0 + a, yy = y0 + b;
                    if (xx >= 0 && yy >= 0 && xx < src->w && yy < src->h)
                        v += src->v[yy * src->w + xx] * (a ? fx : 1 - fx) * (b ? fy : 1 - fy);
                }
            dst->v[j * dst->w + i] = v;
        }
}

static inline void over_px(uint32_t *d, float r, float g, float b, float a)     /* premultiplied float over */
{
    uint32_t o = *d;
    float da = (o >> 24) / 255.0f, ia = 1 - a;
    *d = pack(r + (float)(o >> 16 & 255) * ia, g + (float)(o >> 8 & 255) * ia, b + (float)(o & 255) * ia, a + da * ia);
}
void tk_draw_fm(tk_bmp *b, const tk_fm *m, int ox, int oy, const tk_paint *p, float alpha)
{
    tk_col sc = tk_paint_at(p, 0, 0);
    int solid = p->kind == TK_P_SOLID;
    /* cp7: a solid paint's fully covered px is one packed value, over anything when it is opaque (over_px's ia is 0)
     * and over an empty px (nothing to add); any px over an empty one is its own colour: bit for bit what over_px
     * gives, without its float blend (a grouped list's card: most of a Settings page's build on the A55) */
    uint32_t full = solid ? pack(sc.r * alpha, sc.g * alpha, sc.b * alpha, sc.a * alpha) : 0;
    int full_opaque = solid && sc.a * alpha == 1.0f;
    for (int j = 0; j < m->h; j++) {
        int y = oy + j;
        if (y < 0 || y >= b->h) continue;
        uint32_t *row = b->px + (size_t)y * b->stride;
        const float *mv = m->v + (size_t)j * m->w;
        for (int i = 0; i < m->w; i++) {
            int x = ox + i;
            float v = mv[i], c = v * alpha;
            if (x < 0 || x >= b->w || c < 1.0f / 1024) continue;
            uint32_t *d = &row[x];
            if (solid && v == 1.0f && (full_opaque || !*d)) { *d = full; continue; }
            tk_col s = solid ? sc : tk_paint_at(p, x + 0.5f, y + 0.5f);
            if (!*d) { *d = pack(s.r * c, s.g * c, s.b * c, s.a * c); continue; }
            over_px(d, s.r * c, s.g * c, s.b * c, s.a * c);
        }
    }
}
void tk_fill_rrect(tk_bmp *b, float x, float y, float w, float h, float r, const tk_paint *p, float alpha)
{
    /* in bands of 64 rows (the coverage is per px, so the result is the same): a small buffer for a big card,
     * and nothing done for the rows outside the bitmap */
    int ox = (int)tk_floorf(x) - 1, oy = (int)tk_floorf(y) - 1, H = (int)h + 4, BH = H < 64 ? H : 64;
    tk_fm m;
    if (!tk_fm_new(&m, (int)w + 4, BH)) return;
    for (int y0 = 0; y0 < H; y0 += BH) {
        int by = oy + y0;
        if (by + BH <= 0 || by >= b->h) continue;
        if (y0) tk_fm_clear(&m, 0);
        tk_fm_rrect(&m, (float)ox, (float)by, x, y, w, h, r);
        tk_draw_fm(b, &m, ox, by, p, alpha);
    }
    tk_fm_free(&m);
}
void tk_fill_rect_raw(tk_bmp *b, int x, int y, int w, int h, uint32_t pm)
{
    for (int j = tk_maxi(0, y); j < tk_mini(b->h, y + h); j++)
        for (int i = tk_maxi(0, x); i < tk_mini(b->w, x + w); i++) b->px[j * b->stride + i] = pm;
}

void tk_box_shadow(tk_bmp *b, float x, float y, float w, float h, float r, float dx, float dy, float blur,
                   float spread, tk_col c, int inset)
{
    float sigma = blur / 2, mg = 3 * sigma + tk_fabsf(spread) + 4;
    int ox, oy, W, H;
    if (inset) { ox = (int)tk_floorf(x) - 1; oy = (int)tk_floorf(y) - 1; W = (int)w + 4; H = (int)h + 4; }
    else {
        ox = (int)tk_floorf(x + tk_minf(dx, 0) - mg); oy = (int)tk_floorf(y + tk_minf(dy, 0) - mg);
        W = (int)(w + tk_fabsf(dx) + 2 * mg) + 3; H = (int)(h + tk_fabsf(dy) + 2 * mg) + 3;
    }
    tk_fm s, box;
    if (!tk_fm_new(&s, W, H) || !tk_fm_new(&box, W, H)) { tk_fm_free(&s); return; }
    tk_fm_rrect(&box, (float)ox, (float)oy, x, y, w, h, r);
    if (inset) {
        /* the hole: the box shrunk by spread and moved by the offset; the shadow is the box minus the blurred hole.
         * Outside the box the hole counts as 0 so the blur darkens the edges */
        float rr = tk_maxf(0, r - spread);
        tk_r4 keep = tk_corner;
        if (tk_use_r4) { tk_corner.tl = tk_maxf(0, keep.tl - spread); tk_corner.tr = tk_maxf(0, keep.tr - spread); tk_corner.br = tk_maxf(0, keep.br - spread); tk_corner.bl = tk_maxf(0, keep.bl - spread); }
        tk_fm_rrect(&s, (float)ox, (float)oy, x + dx + spread, y + dy + spread, w - 2 * spread, h - 2 * spread, rr);
        tk_corner = keep;
        tk_fm_blur(&s, sigma);
        for (int i = 0; i < W * H; i++) s.v[i] = (1 - s.v[i]) * box.v[i];
    } else {
        float rr = spread >= 0 ? r + spread : tk_maxf(0, r + spread);
        tk_fm_rrect(&s, (float)ox, (float)oy, x + dx - spread, y + dy - spread, w + 2 * spread, h + 2 * spread, rr);
        tk_fm_blur(&s, sigma);
        tk_fm_mul(&s, &box, 1);
    }
    tk_paint p = tk_solid(c);
    tk_draw_fm(b, &s, ox, oy, &p, 1);
    tk_fm_free(&s);
    tk_fm_free(&box);
}

static inline uint8x8_t d255(uint16x8_t m) { return vrshrn_n_u16(vrsraq_n_u16(m, m, 8), 8); }
void tk_blit(tk_bmp *dst, const tk_bmp *src, int x, int y, float alpha)
{
    /* premultiplied src over dst at (x, y) times alpha: 8 px a NEON step (empty blocks skipped) */
    if (!dst->px || !src->px) return;
    unsigned a = (unsigned)(alpha * 255 + 0.5f);
    if (a > 255) a = 255;
    int i0 = tk_maxi(0, -x), i1 = tk_mini(src->w, dst->w - x);
    if (i1 <= i0) return;
    uint8x8_t va = vdup_n_u8((uint8_t)a);
    for (int j = tk_maxi(0, -y); j < src->h && y + j < dst->h; j++) {
        const uint32_t *sp = src->px + (size_t)j * src->stride;
        uint32_t *dp = dst->px + (size_t)(y + j) * dst->stride + x;
        int i = i0;
        for (; i + 8 <= i1; i += 8) {
            uint8x8x4_t S = vld4_u8((const uint8_t *)(sp + i));
            if (!vmaxv_u8(S.val[3])) continue;
            if (a < 255)
                for (int c = 0; c < 4; c++) S.val[c] = d255(vmull_u8(S.val[c], va));
            uint8x8x4_t D = vld4_u8((uint8_t *)(dp + i));
            uint8x8_t ia = vmvn_u8(S.val[3]);
            for (int c = 0; c < 4; c++) D.val[c] = vqadd_u8(S.val[c], d255(vmull_u8(D.val[c], ia)));
            vst4_u8((uint8_t *)(dp + i), D);
        }
        for (; i < i1; i++) {
            uint32_t s0 = sp[i];
            if (!s0) continue;
            uint32_t sc = 0;
            for (int sh = 0; sh < 32; sh += 8) sc |= (((s0 >> sh & 255) * a + 127) / 255) << sh;
            uint32_t o = dp[i], ia = 255 - (sc >> 24), r = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t v = (sc >> sh & 255) + ((o >> sh & 255) * ia + 127) / 255;
                r |= (v > 255 ? 255 : v) << sh;
            }
            dp[i] = r;
        }
    }
}
void tk_blit_tinted(tk_bmp *dst, const tk_bmp *src, int x, int y, tk_col tint)
{
    for (int j = tk_maxi(0, -y); j < src->h && y + j < dst->h; j++)
        for (int i = tk_maxi(0, -x); i < src->w && x + i < dst->w; i++) {
            float a = (src->px[j * src->stride + i] >> 24) / 255.0f * tint.a;
            if (a > 0) over_px(&dst->px[(y + j) * dst->stride + x + i], tint.r * a, tint.g * a, tint.b * a, a);
        }
}

/* ---- filters on premultiplied bitmaps -------------------------------------------------------------------- */
static void box_mirror(float *v, float *t, int n, int r)
{
    /* running sum with mirrored edges (Skia's backdrop edge mode) */
    float acc = 0, inv = 1.0f / (2 * r + 1);
#define MIR(k) ((k) < 0 ? -(k) - 1 : (k) >= n ? 2 * n - (k) - 1 : (k))
    for (int k = -r; k <= r; k++) acc += v[MIR(k)];
    for (int i = 0; i < n; i++) {
        t[i] = acc * inv;
        acc += v[MIR(i + r + 1)] - v[MIR(i - r)];
    }
#undef MIR
    tk_memcpy(v, t, (size_t)n * 4);
}
static void bmp_blur_full(tk_bmp *b, float sigma);
void tk_bmp_blur(tk_bmp *b, float sigma) { tk_bmp_blur_sat(b, sigma, 1, 1); }
/* rows y0..y1 of b: the quarter q bilinear up 4x (tk_bmp_blur_sat's wide blur) */
typedef struct { tk_bmp *b; const tk_bmp *q; } up4_ctx;
static void up4_rows(void *v, int y0r, int y1r)
{
    up4_ctx *U = v;
    tk_bmp *b = U->b;
    const tk_bmp *q = U->q;
    for (int j = y0r; j < y1r; j++) {
        float fy = (j + .5f) / 4 - .5f;
        int ya = (int)tk_floorf(fy);
        unsigned ty = (unsigned)((fy - ya) * 256);
        int y0 = tk_maxi(0, ya), y1 = tk_mini(q->h - 1, ya + 1);
        for (int i = 0; i < b->w; i++) {
            float fx = (i + .5f) / 4 - .5f;
            int xa = (int)tk_floorf(fx);
            unsigned tx = (unsigned)((fx - xa) * 256);
            int x0 = tk_maxi(0, xa), x1 = tk_mini(q->w - 1, xa + 1);
            uint32_t p00 = q->px[y0 * q->w + x0], p01 = q->px[y0 * q->w + x1], p10 = q->px[y1 * q->w + x0], p11 = q->px[y1 * q->w + x1], o = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                unsigned t0 = (p00 >> sh & 255) * (256 - tx) + (p01 >> sh & 255) * tx, t1 = (p10 >> sh & 255) * (256 - tx) + (p11 >> sh & 255) * tx;
                o |= ((t0 * (256 - ty) + t1 * ty + 32768) >> 16) << sh;
            }
            b->px[j * b->stride + i] = o;
        }
    }
}
void tk_bmp_blur_sat(tk_bmp *b, float sigma, float sat, float bright)
{
    /* the blur, then CSS saturate / brightness; for a wide blur both at a quarter of the resolution (the result
     * is smooth: the filter commutes with the bilinear up to within a level) */
    int filt = sat != 1 || bright != 1;
    if (sigma < 0.3f) { if (filt) tk_bmp_satbright(b, sat, bright); return; }
    if (sigma < 8 || b->w < 16 || b->h < 16) { bmp_blur_full(b, sigma); if (filt) tk_bmp_satbright(b, sat, bright); return; }
    /* as tk_fm_blur: a wide gaussian at a quarter of the resolution, then bilinear up */
    tk_bmp a, q;
    if (!tk_bmp_new(&a, (b->w + 1) / 2, (b->h + 1) / 2)) { bmp_blur_full(b, sigma); return; }
    tk_bmp_new(&q, (a.w + 1) / 2, (a.h + 1) / 2);
    tk_bmp_down2(&a, b);
    tk_bmp_down2(&q, &a);
    bmp_blur_full(&q, sigma / 4);
    if (filt) tk_bmp_satbright(&q, sat, bright);
    up4_ctx U = { b, &q };
    tk_par_rows(0, b->h, up4_rows, &U);         /* cp7c: the bilinear up over the cores (rows are independent) */
    tk_bmp_free(&a);
    tk_bmp_free(&q);
}
/* cp7c: the rows, then the columns, in 8 bands over the cores (tk_par; each band its own scratch, made here: the
 * workers allocate nothing); every line is filtered as before, so the result is the same bit for bit */
typedef struct { tk_bmp *b; int r[3], n, cols; float *scr; } bbf_ctx;
static void bbf_band(void *v, int k)
{
    bbf_ctx *B = v;
    tk_bmp *b = B->b;
    int n = B->n, len = B->cols ? b->w : b->h, a = len * k / 8, e = len * (k + 1) / 8;
    float *ch = B->scr + (size_t)k * (size_t)n * 5, *t = ch + 4 * (size_t)n;
    for (int L = a; L < e; L++) {
        if (!B->cols) {
            uint32_t *row = b->px + L * b->stride;
            for (int c = 0; c < 4; c++) {
                float *v2 = ch + c * n;
                for (int i = 0; i < b->w; i++) v2[i] = (float)(row[i] >> (8 * c) & 255);
                for (int p = 0; p < 3; p++) box_mirror(v2, t, b->w, tk_mini(B->r[p], b->w - 1));
            }
            for (int i = 0; i < b->w; i++) {
                uint32_t o = 0;
                for (int c = 0; c < 4; c++) { int q = (int)(ch[c * n + i] + 0.5f); o |= (uint32_t)(q > 255 ? 255 : q) << (8 * c); }
                row[i] = o;
            }
        } else {
            int i = L;
            for (int c = 0; c < 4; c++) {
                float *v2 = ch + c * n;
                for (int j = 0; j < b->h; j++) v2[j] = (float)(b->px[j * b->stride + i] >> (8 * c) & 255);
                for (int p = 0; p < 3; p++) box_mirror(v2, t, b->h, tk_mini(B->r[p], b->h - 1));
            }
            for (int j = 0; j < b->h; j++) {
                uint32_t o = 0;
                for (int c = 0; c < 4; c++) { int q = (int)(ch[c * n + j] + 0.5f); o |= (uint32_t)(q > 255 ? 255 : q) << (8 * c); }
                b->px[j * b->stride + i] = o;
            }
        }
    }
}
static void bmp_blur_full(tk_bmp *b, float sigma)
{
    bbf_ctx B;
    boxes(sigma, B.r);
    B.b = b;
    B.n = tk_maxi(b->w, b->h);
    for (int k = 0; k < 3; k++) B.r[k] = tk_mini(B.r[k], B.n - 1);
    B.scr = tk_alloc((size_t)B.n * 5 * 4 * 8);
    if (!B.scr) return;
    for (B.cols = 0; B.cols < 2; B.cols++) {
        if (tk_par && (B.cols ? b->w : b->h) >= 64) tk_par(bbf_band, &B, 8);
        else for (int k = 0; k < 8; k++) bbf_band(&B, k);
    }
    tk_free(B.scr);
}
typedef struct { tk_bmp *b; float s, br; } sb_ctx;
static void satbright_rows(void *v, int y0, int y1)
{
    sb_ctx *S = v;
    tk_bmp *b = S->b;
    float s = S->s, br = S->br;
    for (int j = y0; j < y1; j++)
        for (int i = 0; i < b->w; i++) {
            uint32_t *p = &b->px[j * b->stride + i], v = *p;
            float a = (v >> 24) / 255.0f;
            if (a <= 0) continue;
            float R = (v >> 16 & 255) / a, G = (v >> 8 & 255) / a, B = (v & 255) / a;
            float r2 = (0.213f + 0.787f * s) * R + (0.715f - 0.715f * s) * G + (0.072f - 0.072f * s) * B;
            float g2 = (0.213f - 0.213f * s) * R + (0.715f + 0.285f * s) * G + (0.072f - 0.072f * s) * B;
            float b2 = (0.213f - 0.213f * s) * R + (0.715f - 0.715f * s) * G + (0.072f + 0.928f * s) * B;
            r2 = tk_clampf(r2, 0, 255) * br; g2 = tk_clampf(g2, 0, 255) * br; b2 = tk_clampf(b2, 0, 255) * br;
            *p = pack(tk_minf(r2, 255) * a, tk_minf(g2, 255) * a, tk_minf(b2, 255) * a, a);
        }
}
void tk_bmp_satbright(tk_bmp *b, float s, float br)
{
    /* CSS saturate() and brightness() on the unpremultiplied colour, clamped after each (cp7c: over the cores) */
    sb_ctx S = { b, s, br };
    tk_par_rows(0, b->h, satbright_rows, &S);
}
void tk_bmp_copy(tk_bmp *dst, int dx, int dy, const tk_bmp *src, int sx, int sy, int w, int h)
{
    for (int j = 0; j < h; j++) {
        int yd = dy + j, ys = sy + j;
        if (yd < 0 || yd >= dst->h || ys < 0 || ys >= src->h) continue;
        for (int i = 0; i < w; i++) {
            int xd = dx + i, xs = sx + i;
            if (xd < 0 || xd >= dst->w || xs < 0 || xs >= src->w) continue;
            dst->px[yd * dst->stride + xd] = src->px[ys * src->stride + xs];
        }
    }
}
void tk_bmp_down2(tk_bmp *dst, const tk_bmp *src)
{
    /* the 2x2 box ((a + b + c + d + 2) >> 2), 4 px a NEON step where both source rows and columns exist */
    if (!dst->px || !src->px) return;
    for (int j = 0; j < dst->h; j++) {
        int y0 = tk_mini(2 * j, src->h - 1), y1 = tk_mini(2 * j + 1, src->h - 1);
        const uint32_t *ra = src->px + (size_t)y0 * src->stride, *rb = src->px + (size_t)y1 * src->stride;
        uint32_t *o = dst->px + (size_t)j * dst->stride;
        int i = 0, full = src->w / 2;
        for (; i + 4 <= full && i + 4 <= dst->w; i += 4) {
            uint32x4x2_t A = vld2q_u32(ra + 2 * i), B = vld2q_u32(rb + 2 * i);
            uint8x16_t ae = vreinterpretq_u8_u32(A.val[0]), ao = vreinterpretq_u8_u32(A.val[1]);
            uint8x16_t be = vreinterpretq_u8_u32(B.val[0]), bo = vreinterpretq_u8_u32(B.val[1]);
            uint16x8_t lo = vaddq_u16(vaddl_u8(vget_low_u8(ae), vget_low_u8(ao)), vaddl_u8(vget_low_u8(be), vget_low_u8(bo)));
            uint16x8_t hi = vaddq_u16(vaddl_u8(vget_high_u8(ae), vget_high_u8(ao)), vaddl_u8(vget_high_u8(be), vget_high_u8(bo)));
            vst1q_u8((uint8_t *)(o + i), vcombine_u8(vrshrn_n_u16(lo, 2), vrshrn_n_u16(hi, 2)));
        }
        for (; i < dst->w; i++) {
            int x0 = tk_mini(2 * i, src->w - 1), x1 = tk_mini(2 * i + 1, src->w - 1);
            uint32_t a = ra[x0], b = ra[x1], c = rb[x0], d = rb[x1], v = 0;
            for (int sh = 0; sh < 32; sh += 8)
                v |= (((a >> sh & 255) + (b >> sh & 255) + (c >> sh & 255) + (d >> sh & 255) + 2) >> 2) << sh;
            o[i] = v;
        }
    }
}

void tk_bmp_tint(tk_bmp *b, tk_col c)
{
    /* a constant colour over the whole bitmap (premultiplied) */
    uint32_t sp = tk_premul(c), ia = 255 - (sp >> 24);
    for (int j = 0; j < b->h; j++)
        for (int i = 0; i < b->w; i++) {
            uint32_t *d = &b->px[j * b->stride + i], o = *d, r = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t v = (sp >> sh & 255) + ((o >> sh & 255) * ia + 127) / 255;
                r |= (v > 255 ? 255 : v) << sh;
            }
            *d = r;
        }
}
