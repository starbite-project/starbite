/* SPDX-License-Identifier: MIT
 * sbtk bake-time drawing: CSS paints (solid, linear, radial; premultiplied interpolation as CSS Images 4), coverage
 * planes (rounded rects and circles by their distance field, stroked arcs with round caps), gaussian blur as three
 * box passes, CSS box-shadow (outer clipped to outside the box, inset), images, and the backdrop filters. All of it
 * runs when a layer's content changes, never per frame, so it is float and plain C. */
#include "sbtk.h"

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

/* the signed distance to a rounded box with radii per corner (CSS border-radius after its scaling rule) */
float tk_sd_rrect(float px, float py, float x, float y, float w, float h, float r)
{
    float tl = r, tr = r, br = r, bl = r;
    if (tk_use_r4) { tl = tk_corner.tl; tr = tk_corner.tr; br = tk_corner.br; bl = tk_corner.bl; }
    else { float m = tk_minf(w, h) / 2; if (r > m) tl = tr = br = bl = m; }
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
    for (int j = 0; j < m->h; j++) {
        int y = oy + j;
        if (y < 0 || y >= b->h) continue;
        for (int i = 0; i < m->w; i++) {
            int x = ox + i;
            float c = m->v[j * m->w + i] * alpha;
            if (x < 0 || x >= b->w || c < 1.0f / 1024) continue;
            tk_col s = solid ? sc : tk_paint_at(p, x + 0.5f, y + 0.5f);
            over_px(&b->px[y * b->stride + x], s.r * c, s.g * c, s.b * c, s.a * c);
        }
    }
}
void tk_fill_rrect(tk_bmp *b, float x, float y, float w, float h, float r, const tk_paint *p, float alpha)
{
    int ox = (int)tk_floorf(x) - 1, oy = (int)tk_floorf(y) - 1;
    tk_fm m;
    if (!tk_fm_new(&m, (int)w + 4, (int)h + 4)) return;
    tk_fm_rrect(&m, (float)ox, (float)oy, x, y, w, h, r);
    tk_draw_fm(b, &m, ox, oy, p, alpha);
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

void tk_blit(tk_bmp *dst, const tk_bmp *src, int x, int y, float alpha)
{
    int a = (int)(alpha * 256 + 0.5f);
    for (int j = tk_maxi(0, -y); j < src->h && y + j < dst->h; j++)
        for (int i = tk_maxi(0, -x); i < src->w && x + i < dst->w; i++) {
            uint32_t s = src->px[j * src->stride + i];
            if (!s) continue;
            if (a < 256) {
                s = ((s >> 24 & 255) * a >> 8) << 24 | ((s >> 16 & 255) * a >> 8) << 16 | ((s >> 8 & 255) * a >> 8) << 8 | ((s & 255) * a >> 8);
            }
            uint32_t *d = &dst->px[(y + j) * dst->stride + x + i], o = *d, ia = 255 - (s >> 24);
            uint32_t r = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                uint32_t v = (s >> sh & 255) + ((o >> sh & 255) * ia + 127) / 255;
                r |= (v > 255 ? 255 : v) << sh;
            }
            *d = r;
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
    for (int j = 0; j < b->h; j++) {
        float fy = (j + .5f) / 4 - .5f;
        int ya = (int)tk_floorf(fy);
        unsigned ty = (unsigned)((fy - ya) * 256);
        int y0 = tk_maxi(0, ya), y1 = tk_mini(q.h - 1, ya + 1);
        for (int i = 0; i < b->w; i++) {
            float fx = (i + .5f) / 4 - .5f;
            int xa = (int)tk_floorf(fx);
            unsigned tx = (unsigned)((fx - xa) * 256);
            int x0 = tk_maxi(0, xa), x1 = tk_mini(q.w - 1, xa + 1);
            uint32_t p00 = q.px[y0 * q.w + x0], p01 = q.px[y0 * q.w + x1], p10 = q.px[y1 * q.w + x0], p11 = q.px[y1 * q.w + x1], o = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                unsigned t0 = (p00 >> sh & 255) * (256 - tx) + (p01 >> sh & 255) * tx, t1 = (p10 >> sh & 255) * (256 - tx) + (p11 >> sh & 255) * tx;
                o |= ((t0 * (256 - ty) + t1 * ty + 32768) >> 16) << sh;
            }
            b->px[j * b->stride + i] = o;
        }
    }
    tk_bmp_free(&a);
    tk_bmp_free(&q);
}
static void bmp_blur_full(tk_bmp *b, float sigma)
{
    int r[3];
    boxes(sigma, r);
    int n = tk_maxi(b->w, b->h);
    for (int k = 0; k < 3; k++) r[k] = tk_mini(r[k], n - 1);
    float *ch = tk_alloc((size_t)n * 4 * 4), *t = tk_alloc((size_t)n * 4);
    if (!ch || !t) { tk_free(ch); tk_free(t); return; }
    for (int j = 0; j < b->h; j++) {
        uint32_t *row = b->px + j * b->stride;
        for (int c = 0; c < 4; c++) {
            float *v = ch + c * n;
            for (int i = 0; i < b->w; i++) v[i] = (float)(row[i] >> (8 * c) & 255);
            for (int p = 0; p < 3; p++) box_mirror(v, t, b->w, tk_mini(r[p], b->w - 1));
        }
        for (int i = 0; i < b->w; i++) {
            uint32_t o = 0;
            for (int c = 0; c < 4; c++) { int q = (int)(ch[c * n + i] + 0.5f); o |= (uint32_t)(q > 255 ? 255 : q) << (8 * c); }
            row[i] = o;
        }
    }
    for (int i = 0; i < b->w; i++) {
        for (int c = 0; c < 4; c++) {
            float *v = ch + c * n;
            for (int j = 0; j < b->h; j++) v[j] = (float)(b->px[j * b->stride + i] >> (8 * c) & 255);
            for (int p = 0; p < 3; p++) box_mirror(v, t, b->h, tk_mini(r[p], b->h - 1));
        }
        for (int j = 0; j < b->h; j++) {
            uint32_t o = 0;
            for (int c = 0; c < 4; c++) { int q = (int)(ch[c * n + j] + 0.5f); o |= (uint32_t)(q > 255 ? 255 : q) << (8 * c); }
            b->px[j * b->stride + i] = o;
        }
    }
    tk_free(ch);
    tk_free(t);
}
void tk_bmp_satbright(tk_bmp *b, float s, float br)
{
    /* CSS saturate() and brightness() on the unpremultiplied colour, clamped after each */
    for (int j = 0; j < b->h; j++)
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
    for (int j = 0; j < dst->h; j++)
        for (int i = 0; i < dst->w; i++) {
            int x0 = tk_mini(2 * i, src->w - 1), x1 = tk_mini(2 * i + 1, src->w - 1);
            int y0 = tk_mini(2 * j, src->h - 1), y1 = tk_mini(2 * j + 1, src->h - 1);
            uint32_t a = src->px[y0 * src->stride + x0], b = src->px[y0 * src->stride + x1];
            uint32_t c = src->px[y1 * src->stride + x0], d = src->px[y1 * src->stride + x1], o = 0;
            for (int sh = 0; sh < 32; sh += 8)
                o |= (((a >> sh & 255) + (b >> sh & 255) + (c >> sh & 255) + (d >> sh & 255) + 2) >> 2) << sh;
            dst->px[j * dst->stride + i] = o;
        }
}
