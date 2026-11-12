/* SPDX-License-Identifier: MIT
 * sbtk Liquid Glass on the CPU (P3p: audit 5, P3j-11, P3j-16; glass V3: LIQUID-GLASS.md G1 G2 G4 G5). What it shows
 * is its backdrop (blurred, saturated 1.2), bent at the rim where the backdrop is a bitmap the bake can sample, with
 * the iOS 26/27 face, glint and contour of kern/lens.h (the model the GPU's comp2 and the compositor's lens share).
 *
 * ---- The constants (kern/lens.h has the model; these are the bake's own) --------------------------------------
 * Units pt (1 pt = TK_PT px). m = min(w, h) of the element; s = inward distance to the edge; g = outward normal.
 *   size class   k = smoothstep(44, 300, m)
 *   frost        sigma = mix(3, 10, k) for a backdrop filtered per element (tk_glass_local, tk_glass_from_half);
 *                the wallpaper's shared backdrop (tk_glass_backdrop) keeps one 14 pt blur (it is smooth anyway)
 *   colour       saturate TK_GLASS_SAT = 1.2 (clear: TK_GLASS_SAT_CLEAR = 1.0), brightness TK_GLASS_BRIGHT = 1.0
 *   face         lens.h's preset (G2): Lb = luma (Rec. 709 weights on the sRGB values, 0..1) of the backdrop under
 *                the element's middle is its yc (small elements flip light / dark by it; .lg.dark, the shell's
 *                dark = 1, and large glass are dark); face = E + T lumY(Y) + K (c - Y) taken as an over-layer
 *                c (1 - a) + add, linearised at Lb (a = 1 - T lumY'(Lb), add = E + T lumY(Lb) - (1 - a) Lb)
 *   glint        lens.h's (G4): the top and bottom walls, 1.2 pt plus a .21 bleed to 4.8 pt, toward 1.6 white: a
 *                white over-layer matched at the element's face level vref
 *   contour      lens.h's (G5): .75 pt just outside, the side walls darkest
 *   shadow       outside the shape only: offset y mix(2, 6, k), sigma mix(6, 16, k), opacity mix(.10, .18, k)
 *   lens         lens.h's (G1): band h = min(20, m / 4), amount A = min(60, m / 2), d = A (1 - sqrt(1 - t^2)),
 *                t = 1 - s / h, a 257-entry table per element; the backdrop is sampled at q = p - g d (inward: the
 *                outer band shows a squeezed, mirrored strip of what lies further in).
 * The bake works over a backdrop it cannot see (the compositor moves the target toward the wallpaper's backdrop
 * under a glass mask, or there is none: the lock cards), so the face, glint and contour are over-layers; with a
 * glass mask the compositor's lens (comp2 on the GPU, draw_lens on the CPU) draws them per frame from the shape and
 * the bake keeps only the shadow. Where the bake can see the backdrop (already in the bitmap: the CC tiles, the app
 * bars) Lb is measured there; elsewhere it is the wallpaper backdrop's mean (tk_glass_backdrop sets it).
 * The lens needs the backdrop as a bitmap: tk_glass_local, tk_glass_local_put and tk_glass_from_half (static
 * backdrops: the CC tiles, the focus card and slider, the app bars) bend it; the compositor's glass (lock widgets,
 * dock, search) and the lock cards (no backdrop) do not. tk_glass_lens = 0 turns it off. */
#include "sbtk.h"
#include "../kern/lens.h"

int tk_glass_lens = 1;

typedef struct { tk_bmp *b; float k; const tk_paint *lin, *L[3]; } wall_ctx;
static void wall_rows(void *a, int y0, int y1)
{
    wall_ctx *W = a;
    tk_bmp *b = W->b;
    for (int j = y0; j < y1; j++)
        for (int i = 0; i < b->w; i++) {
            float x = (i + .5f) / W->k, y = (j + .5f) / W->k;
            tk_col c = tk_paint_at(W->lin, x, y);
            for (int q = 0; q < 3; q++) {
                tk_col s = tk_paint_at(W->L[q], x, y);
                c.r = s.r + c.r * (1 - s.a); c.g = s.g + c.g * (1 - s.a); c.b = s.b + c.b * (1 - s.a);
            }
            int R = (int)(c.r + .5f), G = (int)(c.g + .5f), B = (int)(c.b + .5f);
            b->px[j * b->stride + i] = 0xff000000u | (uint32_t)(R > 255 ? 255 : R) << 16 | (uint32_t)(G > 255 ? 255 : G) << 8 | (uint32_t)(B > 255 ? 255 : B);
        }
}

void tk_wall_draw(tk_bmp *b, int emergency, float k) { tk_wall_draw_rows(b, emergency, k, 0, b->h); }
/* B6: rows [y0, y1) only (Emergency's switch draws the other wallpaper a band per slice of a frame) */
void tk_wall_draw_rows(tk_bmp *b, int emergency, float k, int y0, int y1)
{
    /* .wall: inset -30 pt, so the gradient box is 450 x 862 pt from (-30, -30) */
    float bx = PT(-30), by = PT(-30), bw = PT(450), bh = PT(862);
    tk_paint lin, r1, r2, r3;
    if (!emergency) {
        /* P3j-16: deep, low-chroma darks (the S9's AMOLED showed the old ones as electric royal blue) */
        tk_stop l[3] = { { 0, tk_hex(0x080e26, 1) }, { .45f, tk_hex(0x0c1636, 1) }, { 1, tk_hex(0x04060f, 1) } };
        tk_stop a[2] = { { 0, TK_RGBA(80, 110, 200, .34f) }, { .6f, TK_RGBA(0, 0, 0, 0) } };
        tk_stop c[2] = { { 0, TK_RGBA(50, 75, 160, .22f) }, { .7f, TK_RGBA(0, 0, 0, 0) } };
        tk_stop d[2] = { { 0, TK_RGBA(40, 60, 130, .14f) }, { .7f, TK_RGBA(0, 0, 0, 0) } };
        lin = tk_linear(180, bx, by, bw, bh, 3, l);
        r1 = tk_radial(1.2f, .7f, .92f, .04f, bx, by, bw, bh, 2, a);
        r2 = tk_radial(.9f, .6f, .08f, .96f, bx, by, bw, bh, 2, c);
        r3 = tk_radial(.6f, .4f, .5f, .55f, bx, by, bw, bh, 2, d);
    } else {
        tk_stop l[3] = { { 0, tk_hex(0x260807, 1) }, { .45f, tk_hex(0x2c0e08, 1) }, { 1, tk_hex(0x0d0403, 1) } };
        tk_stop a[2] = { { 0, TK_RGBA(255, 96, 60, .5f) }, { .6f, TK_RGBA(0, 0, 0, 0) } };
        tk_stop c[2] = { { 0, TK_RGBA(255, 150, 60, .3f) }, { .7f, TK_RGBA(0, 0, 0, 0) } };
        lin = tk_linear(180, bx, by, bw, bh, 3, l);
        r1 = tk_radial(1.2f, .7f, .92f, .04f, bx, by, bw, bh, 2, a);
        r2 = tk_radial(.9f, .6f, .08f, .96f, bx, by, bw, bh, 2, c);
        r3 = tk_solid(TK_RGBA(0, 0, 0, 0));
    }
    wall_ctx W = { b, k, &lin, { &r3, &r2, &r1 } };
    tk_par_rows(tk_maxi(0, y0), tk_mini(b->h, y1), wall_rows, &W);
}

/* the backdrop's quarter up to half resolution, rows [ja, jb) (B6: in bands, over the cores) */
typedef struct { tk_bmp *dst; const tk_bmp *src; } bil_ctx;
static void bilinear_rows(void *a, int ja, int jb)
{
    bil_ctx *B = a;
    tk_bmp *dst = B->dst;
    const tk_bmp *src = B->src;
    float sx = (float)src->w / dst->w, sy = (float)src->h / dst->h;
    for (int j = ja; j < jb; j++) {
        float fy = (j + .5f) * sy - .5f;
        int y0 = (int)tk_floorf(fy);
        float ty = fy - y0;
        int ya = tk_maxi(0, tk_mini(src->h - 1, y0)), yb = tk_maxi(0, tk_mini(src->h - 1, y0 + 1));
        for (int i = 0; i < dst->w; i++) {
            float fx = (i + .5f) * sx - .5f;
            int x0 = (int)tk_floorf(fx);
            float tx = fx - x0;
            int xa = tk_maxi(0, tk_mini(src->w - 1, x0)), xb = tk_maxi(0, tk_mini(src->w - 1, x0 + 1));
            uint32_t p00 = src->px[ya * src->stride + xa], p01 = src->px[ya * src->stride + xb];
            uint32_t p10 = src->px[yb * src->stride + xa], p11 = src->px[yb * src->stride + xb], o = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                float v = ((p00 >> sh & 255) * (1 - tx) + (p01 >> sh & 255) * tx) * (1 - ty) +
                          ((p10 >> sh & 255) * (1 - tx) + (p11 >> sh & 255) * tx) * ty;
                o |= (uint32_t)(int)(v + .5f) << sh;
            }
            dst->px[j * dst->stride + i] = o;
        }
    }
}

typedef struct { tk_bmp *dst; const tk_bmp *src; int y0; float f; const int32_t *cu; } up_ctx;
static void up_rows(void *a, int ja, int jb)
{
    up_ctx *U = a;
    tk_bmp *dst = U->dst;
    const tk_bmp *src = U->src;
    int y0 = U->y0;
    float f = U->f;
    const int32_t *cu = U->cu;
    for (int j = ja; j < jb; j++) {
        float fy = (y0 + j + .5f) / f - .5f;
        int ya = (int)tk_floorf(fy);
        unsigned ty = (unsigned)((fy - ya) * 256);
        int y_0 = tk_maxi(0, tk_mini(src->h - 1, ya)), y_1 = tk_maxi(0, tk_mini(src->h - 1, ya + 1));
        const uint32_t *r0 = src->px + (size_t)y_0 * src->stride, *r1 = src->px + (size_t)y_1 * src->stride;
        uint32_t *o = dst->px + (size_t)j * dst->stride;
        for (int i = 0; i < dst->w; i++) {
            int xa = cu[2 * i];
            unsigned tx = (unsigned)cu[2 * i + 1];
            int x_0 = tk_maxi(0, tk_mini(src->w - 1, xa)), x_1 = tk_maxi(0, tk_mini(src->w - 1, xa + 1));
            uint32_t p00 = r0[x_0], p01 = r0[x_1], p10 = r1[x_0], p11 = r1[x_1];
            /* two channels at once (the 0x00ff00ff trick), rows then the column */
            uint32_t a_rb = ((p00 & 0xff00ffu) * (256 - tx) + (p01 & 0xff00ffu) * tx) >> 8 & 0xff00ffu;
            uint32_t a_ag = ((p00 >> 8 & 0xff00ffu) * (256 - tx) + (p01 >> 8 & 0xff00ffu) * tx) >> 8 & 0xff00ffu;
            uint32_t b_rb = ((p10 & 0xff00ffu) * (256 - tx) + (p11 & 0xff00ffu) * tx) >> 8 & 0xff00ffu;
            uint32_t b_ag = ((p10 >> 8 & 0xff00ffu) * (256 - tx) + (p11 >> 8 & 0xff00ffu) * tx) >> 8 & 0xff00ffu;
            uint32_t rb = (a_rb * (256 - ty) + b_rb * ty) >> 8 & 0xff00ffu, ag = (a_ag * (256 - ty) + b_ag * ty) & 0xff00ff00u;
            o[i] = rb | ag;
        }
    }
}

void tk_bmp_up(tk_bmp *dst, const tk_bmp *src, int x0, int y0, float f)
{
    /* dst px (i, j) = the bilinear sample of src at the point (x0 + i, y0 + j) of a space f times src's; fixed
     * point (16.16), the columns' table made once */
    if (!dst->px || !src->px) return;
    int32_t *cu = tk_alloc((size_t)dst->w * 8);
    if (!cu) return;
    for (int i = 0; i < dst->w; i++) {
        float fx = (x0 + i + .5f) / f - .5f;
        int xa = (int)tk_floorf(fx);
        cu[2 * i] = xa;
        cu[2 * i + 1] = (int32_t)((fx - xa) * 256);
    }
    up_ctx U = { dst, src, y0, f, cu };
    tk_par_rows(0, dst->h, up_rows, &U);
    tk_free(cu);
}

/* ---- the shared pieces: luma, the size class, the rounded box's distance and normal, the lens table ---------- */
static float glass_amb = .12f;              /* Lb where the bake cannot see its backdrop: the wallpaper backdrop's */

static inline float luma8(uint32_t p)       /* an opaque pixel's luma, 0..1 */
{
    return (.2126f * (p >> 16 & 255) + .7152f * (p >> 8 & 255) + .0722f * (p & 255)) / 255;
}

typedef struct { float k, frost, sg, sdy, sop; } gsize;   /* px, except k and sop (glass V3: the lens is lens.h's) */
static gsize gsize_of(float w, float h, float r)
{
    gsize z;
    (void)r;
    float m = tk_minf(w, h) / TK_PT, x = tk_clampf((m - 44) / 256, 0, 1), k = x * x * (3 - 2 * x);
    z.k = k;
    z.frost = PT(3 + 7 * k);
    z.sg = PT(6 + 10 * k);
    z.sdy = PT(2 + 4 * k);
    z.sop = .10f + .08f * k;
    return z;
}

/* the signed distance to the rounded box (as tk_sd_rrect4) and its outward unit normal there */
static inline float sd_g(float px, float py, float x, float y, float w, float h, const tk_r4 *k, float *gx, float *gy)
{
    float cx = 0, cy = 0, rr = -1;
    if (px < x + k->tl && py < y + k->tl) { cx = x + k->tl; cy = y + k->tl; rr = k->tl; }
    else if (px > x + w - k->tr && py < y + k->tr) { cx = x + w - k->tr; cy = y + k->tr; rr = k->tr; }
    else if (px > x + w - k->br && py > y + h - k->br) { cx = x + w - k->br; cy = y + h - k->br; rr = k->br; }
    else if (px < x + k->bl && py > y + h - k->bl) { cx = x + k->bl; cy = y + h - k->bl; rr = k->bl; }
    if (rr > 0) {
        float dx = px - cx, dy = py - cy, l = tk_sqrtf(dx * dx + dy * dy);
        if (l > 1e-4f) { *gx = dx / l; *gy = dy / l; }
        else { *gx = px < x + w / 2 ? -.7071f : .7071f; *gy = py < y + h / 2 ? -.7071f : .7071f; }
        return l - rr;
    }
    float a = x - px, b = px - (x + w), c = y - py, e = py - (y + h), m = a;
    *gx = -1; *gy = 0;
    if (b > m) { m = b; *gx = 1; *gy = 0; }
    if (c > m) { m = c; *gx = 0; *gy = -1; }
    if (e > m) { m = e; *gx = 0; *gy = 1; }
    return m;
}

/* glass V3: the lens's offset D(s) in px at 256 steps of s / h across its band h (lens.h lens_offset: the quarter
 * circle, A at the edge, 0 at the band's inner edge and beyond: the last entry) */
#define LENS_N 256
static void lens_table(float *D, const float *L)
{
    float hb = 1 / L[LQ_IH], oy;
    for (int i = 0; i <= LENS_N; i++) {
        float ox;
        lens_offset(L, hb * i / LENS_N, 0, -1, &ox, &oy);
        D[i] = oy;
    }
    D[LENS_N] = 0;
}

/* ---- the backdrop: the filtered bitmap drawn into b inside the shape, bent at the rim ------------------------ */
typedef struct {
    tk_bmp *b; const tk_bmp *s;
    float sx0, sy0, sk;                     /* s px = (b px + s0) * sk */
    float x, y, w, h, ib;                   /* the shape (b px); 1 / bezel */
    tk_r4 R;
    const float *D;                         /* NULL: no lens */
    int X0, X1;
    float Bz, rm;                           /* the lens's band (+ the edge's px) and the largest corner radius */
    const int32_t *cu;                      /* per column of [X0, X1): s's left tap and the 8-bit weight */
} lens_ctx;

/* a row's middle [m0, m1): every pixel at least Bz inside the box (no rim effect, full coverage) */
static inline void mid_span(float x, float y, float w, float h, float Bz, float rm, int X0, int X1, float py, int *m0, int *m1)
{
    float dv = tk_minf(py - y, y + h - py);
    *m0 = *m1 = X1;
    if (dv < Bz) return;
    float bx = dv >= rm ? Bz : tk_maxf(Bz, rm);
    *m0 = tk_maxi(X0, (int)(x + bx) + 1);
    *m1 = tk_maxi(*m0, tk_mini(X1, (int)(x + w - bx) - 1));
}

static inline uint32_t bil(uint32_t p00, uint32_t p01, uint32_t p10, uint32_t p11, unsigned tx, unsigned ty)
{
    /* two channels at once (the 0x00ff00ff trick), rows then the column */
    uint32_t a_rb = ((p00 & 0xff00ffu) * (256 - tx) + (p01 & 0xff00ffu) * tx) >> 8 & 0xff00ffu;
    uint32_t a_ag = ((p00 >> 8 & 0xff00ffu) * (256 - tx) + (p01 >> 8 & 0xff00ffu) * tx) >> 8 & 0xff00ffu;
    uint32_t b_rb = ((p10 & 0xff00ffu) * (256 - tx) + (p11 & 0xff00ffu) * tx) >> 8 & 0xff00ffu;
    uint32_t b_ag = ((p10 >> 8 & 0xff00ffu) * (256 - tx) + (p11 >> 8 & 0xff00ffu) * tx) >> 8 & 0xff00ffu;
    return ((a_rb * (256 - ty) + b_rb * ty) >> 8 & 0xff00ffu) | ((a_ag * (256 - ty) + b_ag * ty) & 0xff00ff00u);
}

static inline uint32_t samp(const tk_bmp *s, float u, float v)
{
    /* bilinear at s's continuous px (u, v) (centres at .5), clamped to the edge */
    u -= .5f; v -= .5f;
    int xa = (int)tk_floorf(u), ya = (int)tk_floorf(v);
    unsigned tx = (unsigned)((u - xa) * 256), ty = (unsigned)((v - ya) * 256);
    int x0 = tk_maxi(0, tk_mini(s->w - 1, xa)), x1 = tk_maxi(0, tk_mini(s->w - 1, xa + 1));
    int y0 = tk_maxi(0, tk_mini(s->h - 1, ya)), y1 = tk_maxi(0, tk_mini(s->h - 1, ya + 1));
    const uint32_t *r0 = s->px + (size_t)y0 * s->stride, *r1 = s->px + (size_t)y1 * s->stride;
    return bil(r0[x0], r0[x1], r1[x0], r1[x1], tx, ty);
}

static inline void put_px(uint32_t *d, uint32_t sp, unsigned c)
{
    /* sp (premultiplied) at coverage c / 255 over *d */
    if (c == 255 && (sp >> 24) == 255) { *d = sp; return; }
    uint32_t o = *d, res = 0;
    unsigned sa = ((sp >> 24) * c + 127) / 255;
    for (int sh = 0; sh < 32; sh += 8) {
        unsigned v = ((sp >> sh & 255) * c + (o >> sh & 255) * (255 - sa) + 127) / 255;
        res |= (v > 255 ? 255 : v) << sh;
    }
    *d = res;
}

static inline float lens_d(const float *D, float t)
{
    float f = tk_clampf(t, 0, 1) * LENS_N;
    int n = (int)f;
    if (n >= LENS_N) return D[LENS_N];
    return D[n] + (D[n + 1] - D[n]) * (f - n);
}

static void lens_rows(void *a, int ja, int jb)
{
    lens_ctx *L = a;
    tk_bmp *b = L->b;
    const tk_bmp *s = L->s;
    for (int j = ja; j < jb; j++) {
        float py = j + .5f;
        uint32_t *row = b->px + (size_t)j * b->stride;
        /* the row's fast span: the middle (no lens), or in the top / bottom band the straight edge, where the lens
         * moves every pixel by the same vertical D; the rest (the sides, the corners) per pixel */
        float dv = tk_minf(py - L->y, L->y + L->h - py), shift = 0;
        int m0 = L->X1, m1 = L->X1;
        if (L->cu) {
            if (dv >= L->Bz) mid_span(L->x, L->y, L->w, L->h, L->Bz, L->rm, L->X0, L->X1, py, &m0, &m1);
            else if (dv >= 1 && L->D) {
                float bx = tk_maxf(L->Bz, L->rm);
                m0 = tk_maxi(L->X0, (int)(L->x + bx) + 1);
                m1 = tk_maxi(m0, tk_mini(L->X1, (int)(L->x + L->w - bx) - 1));
                shift = lens_d(L->D, dv * L->ib);
                if (py - L->y > L->y + L->h - py) shift = -shift;     /* the bottom band samples upward */
            }
        }
        for (int i = L->X0; i < L->X1; i++) {
            if (i == m0) {
                float v = (py + shift + L->sy0) * L->sk - .5f;
                int ya = (int)tk_floorf(v);
                unsigned ty = (unsigned)((v - ya) * 256);
                const uint32_t *r0 = s->px + (size_t)tk_maxi(0, tk_mini(s->h - 1, ya)) * s->stride;
                const uint32_t *r1 = s->px + (size_t)tk_maxi(0, tk_mini(s->h - 1, ya + 1)) * s->stride;
                const int32_t *c = L->cu + 2 * (i - L->X0);
                if (ty == 0)
                    for (; i < m1; i++, c += 2) {
                        unsigned tx = (unsigned)c[1];
                        uint32_t sp = tx ? bil(r0[c[0]], r0[c[0] + 1], 0, 0, tx, 0) : r0[c[0]];
                        put_px(row + i, sp, 255);
                    }
                else
                    for (; i < m1; i++, c += 2) {
                        int x0 = c[0];
                        put_px(row + i, bil(r0[x0], r0[x0 + 1], r1[x0], r1[x0 + 1], (unsigned)c[1], ty), 255);
                    }
                if (i >= L->X1) break;
            }
            float px = i + .5f, gx, gy;
            float sd = sd_g(px, py, L->x, L->y, L->w, L->h, &L->R, &gx, &gy);
            if (sd >= .5f) continue;
            unsigned c = (unsigned)(tk_clampf(.5f - sd, 0, 1) * 255 + .5f);
            float qx = px, qy = py;
            if (L->D && -sd < L->Bz) {
                float D = lens_d(L->D, -sd * L->ib);
                qx -= gx * D; qy -= gy * D;
            }
            put_px(row + i, samp(s, (qx + L->sx0) * L->sk, (qy + L->sy0) * L->sk), c);
        }
    }
}

static void lens_put(tk_bmp *b, const tk_bmp *s, float sx0, float sy0, float sk, float x, float y, float w, float h, float r)
{
    if (!b->px || !s->px) return;
    float D[LENS_N + 1], Lq[LQ_N];
    lens_make(Lq, w, h, TK_PT, 0, 0, 0, -1);    /* glass V3: the geometry only (band, amount) */
    float hb = 1 / Lq[LQ_IH];
    if (tk_glass_lens) lens_table(D, Lq);
    tk_r4 R = tk_radii(w, h, r);
    float rm = tk_maxf(tk_maxf(R.tl, R.tr), tk_maxf(R.br, R.bl));
    /* the lens's band: as far in as it moves the (blurred) backdrop by an 8th of a px or more (nearly h) */
    float band = 0;
    if (tk_glass_lens)
        for (int i = LENS_N - 1; i >= 0; i--)
            if (D[i] >= 1.0f / 8) { band = (i + 1) * hb / LENS_N; break; }
    lens_ctx L = { b, s, sx0, sy0, sk, x, y, w, h, 1 / hb, R, tk_glass_lens ? D : 0,
                   tk_maxi(0, (int)tk_floorf(x) - 1), tk_mini(b->w, (int)(x + w) + 2), band + 1.5f, rm, 0 };
    /* the fast spans' columns, clamped into s (s covers the box, so only the outermost ever clamp) */
    int32_t *cu = s->w >= 2 && L.X1 > L.X0 ? tk_alloc((size_t)(L.X1 - L.X0) * 8) : 0;
    if (cu) {
        for (int i = L.X0; i < L.X1; i++) {
            float u = (i + .5f + sx0) * sk - .5f;
            int xa = (int)tk_floorf(u);
            unsigned tx = (unsigned)((u - xa) * 256);
            if (xa < 0) { xa = 0; tx = 0; }
            if (xa > s->w - 2) { tx = xa >= s->w - 1 ? 256 : tx; xa = s->w - 2; }
            cu[2 * (i - L.X0)] = xa; cu[2 * (i - L.X0) + 1] = (int32_t)tx;
        }
        L.cu = cu;
    }
    tk_par_rows(tk_maxi(0, (int)tk_floorf(y) - 1), tk_mini(b->h, (int)(y + h) + 2), lens_rows, &L);
    if (cu) tk_free(cu);
}

void tk_glass_backdrop(tk_bmp *dst, const tk_bmp *src)
{
    /* src and dst at half resolution; the blur at a quarter (sigma 14 pt = 51.7 px is far wider than what that
     * loses), then up. Its mean luma is the tone's Lb for glass that cannot see its backdrop. */
    tk_bmp q;
    tk_bmp_new(&q, src->w / 2, src->h / 2);
    tk_bmp_down2(&q, src);
    float amb = tk_glass_backdrop_q(&q, -1);
    if (amb >= 0) glass_amb = amb;
    tk_glass_backdrop_up(dst, &q, 0, dst->h);
    tk_bmp_free(&q);
}
/* B6: tk_glass_backdrop in steps (Emergency's switch, a slice of a frame at a time): q (src down 2x) blurred (part 0)
 * and saturated in place, its mean luma returned (part 1; -1: both; -1 returned: none); then dst's rows [y0, y1) up
 * from it */
float tk_glass_backdrop_q(tk_bmp *q, int part)
{
    if (part <= 0) tk_bmp_blur(q, PT(14) / 4);
    if (!part) return 0;
    tk_bmp_satbright(q, TK_GLASS_SAT, TK_GLASS_BRIGHT);
    float sum = 0;
    int n = 0;
    for (int j = 0; j < q->h; j += 8)
        for (int i = 0; i < q->w; i += 8) { sum += luma8(q->px[j * q->stride + i]); n++; }
    return n ? sum / n : -1;
}
void tk_glass_backdrop_up(tk_bmp *dst, const tk_bmp *q, int y0, int y1)
{
    if (!q->px || !dst->px) return;
    bil_ctx B = { dst, q };
    tk_par_rows(tk_maxi(0, y0), tk_mini(dst->h, y1), bilinear_rows, &B);
}
/* the Lb of glass that cannot see its backdrop, set (Emergency's cards made for the other ground before it shows);
 * returns the one before */
float tk_glass_amb_set(float v)
{
    float o = glass_amb;
    glass_amb = v;
    return o;
}

void tk_glass_local_put(tk_bmp *b, const tk_bmp *t, int X, int Y, float x, float y, float w, float h, float r)
{
    /* t (the filtered backdrop, its px (0, 0) at b's (X, Y)) over b inside the rounded box, bent at the rim */
    lens_put(b, t, (float)-X, (float)-Y, 1, x, y, w, h, r);
}

void tk_glass_local(tk_bmp *b, const tk_bmp *src, float x, float y, float w, float h, float r)
{
    /* Chrome's backdrop-filter: the backdrop under the element's box (edges mirrored), blurred by the size class,
     * saturated, drawn over b inside the shape */
    int X = (int)tk_floorf(x), Y = (int)tk_floorf(y), W = (int)(x + w + .999f) - X, H = (int)(y + h + .999f) - Y;
    tk_bmp t;
    if (!tk_bmp_new(&t, W, H)) return;
    tk_bmp_copy(&t, 0, 0, src, X, Y, W, H);
    tk_bmp_blur_sat(&t, gsize_of(w, h, r).frost, TK_GLASS_SAT, TK_GLASS_BRIGHT);
    tk_glass_local_put(b, &t, X, Y, x, y, w, h, r);
    tk_bmp_free(&t);
}

void tk_glass_from_half(tk_bmp *b, int box, int boy, const tk_bmp *half, float x, float y, float w, float h, float r)
{
    /* tk_glass_local for a backdrop that exists only at half resolution (the focus scrim, the CC scrim): the
     * element's region of it, down to a quarter, blurred by the size class, saturated, then sampled (bent at the
     * rim) into b inside the shape. b's origin is the panel point (box, boy); (x, y, w, h) are in b's px */
    int X0 = (int)tk_floorf(x), Y0 = (int)tk_floorf(y), X1 = (int)(x + w + .999f), Y1 = (int)(y + h + .999f);
    int hx0 = (box + X0) >> 1, hy0 = (boy + Y0) >> 1, hx1 = (box + X1 + 1) >> 1, hy1 = (boy + Y1 + 1) >> 1;
    hx0 = tk_maxi(0, hx0); hy0 = tk_maxi(0, hy0); hx1 = tk_mini(half->w, hx1); hy1 = tk_mini(half->h, hy1);
    tk_bmp t, q;
    if (!tk_bmp_new(&t, hx1 - hx0, hy1 - hy0)) return;
    tk_bmp_copy(&t, 0, 0, half, hx0, hy0, t.w, t.h);
    if (!tk_bmp_new(&q, (t.w + 1) / 2, (t.h + 1) / 2)) { tk_bmp_free(&t); return; }
    tk_bmp_down2(&q, &t);
    tk_bmp_free(&t);
    tk_bmp_blur_sat(&q, gsize_of(w, h, r).frost / 4, TK_GLASS_SAT, TK_GLASS_BRIGHT);
    /* q px (i, j) covers panel px [4 i, 4 i + 4) from (2 hx0, 2 hy0) */
    lens_put(b, &q, (float)(box - 2 * hx0), (float)(boy - 2 * hy0), .25f, x, y, w, h, r);
    tk_bmp_free(&q);
}

void tk_bmp_rotate(tk_bmp *dst, const tk_bmp *src, float deg)
{
    float a = deg * 0.0174532925f, c = tk_cosf(a), s = tk_sinf(a), cx = src->w / 2.0f, cy = src->h / 2.0f;
    for (int j = 0; j < dst->h; j++)
        for (int i = 0; i < dst->w; i++) {
            float dx = i + .5f - cx, dy = j + .5f - cy;
            float u = c * dx + s * dy + cx - .5f, v = -s * dx + c * dy + cy - .5f;
            int u0 = (int)tk_floorf(u), v0 = (int)tk_floorf(v);
            float fx = u - u0, fy = v - v0, acc[4] = { 0, 0, 0, 0 };
            for (int q = 0; q < 4; q++) {
                int uu = u0 + (q & 1), vv = v0 + (q >> 1);
                if (uu < 0 || vv < 0 || uu >= src->w || vv >= src->h) continue;
                float wgt = (q & 1 ? fx : 1 - fx) * (q >> 1 ? fy : 1 - fy);
                uint32_t p = src->px[vv * src->stride + uu];
                for (int k = 0; k < 4; k++) acc[k] += (float)(p >> (8 * k) & 255) * wgt;
            }
            uint32_t o = 0;
            for (int k = 0; k < 4; k++) o |= (uint32_t)(int)(acc[k] + .5f) << (8 * k);
            dst->px[j * dst->stride + i] = o;
        }
}

float tk_bezier(float x1, float y1, float x2, float y2, float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    float u = t;
    for (int i = 0; i < 12; i++) {          /* solve x(u) = t by Newton, then bisection-safe clamp */
        float iu = 1 - u, x = 3 * iu * iu * u * x1 + 3 * iu * u * u * x2 + u * u * u - t;
        float dx = 3 * iu * iu * x1 + 6 * iu * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (tk_fabsf(dx) < 1e-6f) break;
        u = tk_clampf(u - x / dx, 0, 1);
    }
    float iu = 1 - u;
    return 3 * iu * iu * u * y1 + 3 * iu * u * u * y2 + u * u * u;
}

/* ---- the glass's own paint in one pass per pixel ----------------------------------------------------------------
 * Per pixel: the drop shadow outside (the box moved down, a gaussian of its distance: exact along straight edges,
 * close at the corners), and inside the tone, the depth and the rim specular, all functions of the distance s and
 * the normal g. tk_glass_bake runs it over the four cores with tables (the shadow's CDF, the specular's
 * linear-to-over conversion); tk_glass_bake_ref is the same pixel evaluated directly (the erf, the sRGB
 * conversions) on one core: the reference the tables are checked against (they agree within a level). */
static float PHI[1025];                     /* the standard normal CDF on [-4, 4] */
static int phi_ok;
static float erf_cdf(float v)
{
    /* Abramowitz-Stegun 7.1.26 for erf */
    float z = tk_fabsf(v) / 1.41421356f, t = 1 / (1 + .3275911f * z);
    float e = 1 - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t - .284496736f) * t + .254829592f) * t * tk_expf(-z * z);
    return .5f * (1 + (v < 0 ? -e : e));
}
static float phi(float x)
{
    if (!phi_ok) {
        for (int i = 0; i <= 1024; i++) PHI[i] = erf_cdf(-4 + 8.0f * i / 1024);
        phi_ok = 1;
    }
    if (x <= -4) return 0;
    if (x >= 4) return 1;
    float f = (x + 4) * 128;
    int i = (int)f;
    return PHI[i] + (PHI[i + 1] - PHI[i]) * (f - i);
}
static inline void over4(float *d, float r, float g, float b, float a)      /* premultiplied: d = s over d */
{
    d[0] = r + d[0] * (1 - a); d[1] = g + d[1] * (1 - a); d[2] = b + d[2] * (1 - a); d[3] = a + d[3] * (1 - a);
}
static inline void over_col(float *d, float cr, float cg, float cb, float a)  /* straight colour at alpha a */
{
    over4(d, cr * a, cg * a, cb * a, a);
}

typedef struct {
    tk_bmp *b; tk_a8 *mask;
    float x, y, w, h, sg, sdy, sop, srho;
    int X0, X1, shadow, exact;
    tk_r4 R;
    float Bz, rm;                           /* the middle (mid_span): the tone alone (no glint, no contour) */
    float tone_c, tone_a;                   /* the face as an over-layer: premultiplied grey level (0..255), alpha */
    float vref;                             /* the face's level at the element's backdrop (sRGB 0..1) */
    float L[LQ_N];                          /* glass V3: the lens's record (the glint, the contour) */
    int lensed;                             /* P4: the compositor lenses it: only the shadow and the mask */
} gbake;

static void gbake_rows(void *a, int Y0, int Y1)
{
    gbake *G = a;
    tk_bmp *b = G->b;
    tk_a8 *mask = G->mask;
    float x = G->x, y = G->y, w = G->w, h = G->h, sg = G->sg, sdy = G->sdy;
    /* the middle's pixel: the tone over what is there */
    unsigned tc = (unsigned)(G->tone_c + .5f), tia = (unsigned)((1 - G->tone_a) * 256 + .5f), tA = (unsigned)(G->tone_a * 255 + .5f);
    for (int j = Y0; j < Y1; j++) {
        float py = j + .5f;
        int m0 = G->X1, m1 = G->X1;
        if (!G->exact) mid_span(x, y, w, h, G->Bz, G->rm, G->X0, G->X1, py, &m0, &m1);
        for (int i = G->X0; i < G->X1; i++) {
            if (i == m0) {
                uint32_t *d = b->px + (size_t)j * b->stride;
                for (; i < m1 && !G->lensed; i++) {
                    uint32_t o = d[i];
                    unsigned A = tA + ((o >> 24) * tia >> 8), R = tc + ((o >> 16 & 255) * tia >> 8);
                    unsigned Gc = tc + ((o >> 8 & 255) * tia >> 8), Bc = tc + ((o & 255) * tia >> 8);
                    A = A > 255 ? 255 : A; R = R > A ? A : R; Gc = Gc > A ? A : Gc; Bc = Bc > A ? A : Bc;
                    d[i] = A << 24 | R << 16 | Gc << 8 | Bc;
                }
                if (mask && mask->v && j < mask->h)
                    for (int q = m0; q < m1 && q < mask->w; q++) mask->v[j * mask->w + q] = 255;
                i = m1;
                if (i >= G->X1) break;
            }
            float px = i + .5f, gx, gy;
            float sd = sd_g(px, py, x, y, w, h, &G->R, &gx, &gy);
            float cov = tk_clampf(.5f - sd, 0, 1);
            float out[4] = { 0, 0, 0, 0 };
            if (G->shadow && cov < 1) {
                /* the drop shadow: the box moved down, blurred; only outside the box */
                float ss = tk_sd_rrect4(px, py - sdy, x, y, w, h, &G->R);
                float al = G->sop * (G->exact ? erf_cdf(-ss / sg) : phi(-ss / sg)) * (1 - cov);
                if (al > 1.0f / 1024) over_col(out, 0, 0, 0, al);
            }
            if (!G->lensed && cov < 1) {
                /* glass V3 (G5): the contour just outside, darkest on the side walls (lens_contour) */
                float ct = lens_contour(G->L, sd, gx) * (1 - cov);
                if (ct > 1.0f / 1024) over_col(out, 0, 0, 0, ct);
            }
            if (cov > 0 && !G->lensed) {
                float f[4] = { G->tone_c, G->tone_c, G->tone_c, G->tone_a };
                float s = tk_maxf(-sd, 0);
                /* glass V3 (G4): the glint, the top and bottom walls alike (lens_face's), as a white over-layer that
                 * matches it at the element's face level vref */
                if (s < G->srho) {
                    float sp = s * G->L[LQ_IPT], r1 = tk_clampf(1 - sp / 1.2f, 0, 1), r2 = .21f * tk_clampf(1 - sp / 4.8f, 0, 1);
                    float gw = G->L[LQ_GL] * (1 - tk_fabsf(gx)) * tk_maxf(r1, r2);
                    if (gw > 1.0f / 1024) over_col(f, 255, 255, 255, tk_clampf(gw * (1.6f - G->vref) / (1 - G->vref), 0, 1));
                }
                over4(out, f[0] * cov, f[1] * cov, f[2] * cov, f[3] * cov);
            }
            if (out[3] <= 0) {
                if (G->lensed && mask && mask->v && i < mask->w && j < mask->h && cov > 0) {   /* the mask still */
                    int v = (int)(cov * 255 + .5f);
                    if (v > mask->v[j * mask->w + i]) mask->v[j * mask->w + i] = (uint8_t)v;
                }
                continue;
            }
            uint32_t *d = &b->px[j * b->stride + i], o = *d;
            float ia = 1 - out[3], A = out[3] + (o >> 24) / 255.0f * ia;
            int R = (int)(out[0] + (o >> 16 & 255) * ia + .5f), Gc = (int)(out[1] + (o >> 8 & 255) * ia + .5f), B = (int)(out[2] + (o & 255) * ia + .5f);
            int Aq = (int)(A * 255 + .5f);
            Aq = Aq > 255 ? 255 : Aq;
            R = R > Aq ? Aq : R; Gc = Gc > Aq ? Aq : Gc; B = B > Aq ? Aq : B;
            *d = (uint32_t)Aq << 24 | (uint32_t)R << 16 | (uint32_t)Gc << 8 | (uint32_t)B;
            if (mask && mask->v && i < mask->w && j < mask->h && cov > 0) {
                int v = (int)(cov * 255 + .5f);
                if (v > mask->v[j * mask->w + i]) mask->v[j * mask->w + i] = (uint8_t)v;
            }
        }
    }
}

/* Lb: what b already holds under the element's middle if that is opaque (a backdrop put there first), else the
 * wallpaper backdrop's */
static float backdrop_lb(const tk_bmp *b, float x, float y, float w, float h)
{
    float sum = 0;
    int n = 0;
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) {
            int X = (int)(x + w * (.2f + .6f * u / 7)), Y = (int)(y + h * (.2f + .6f * v / 7));
            if (X < 0 || Y < 0 || X >= b->w || Y >= b->h) continue;
            uint32_t p = b->px[Y * b->stride + X];
            if ((p >> 24) < 245) continue;
            sum += luma8(p);
            n++;
        }
    return n >= 32 ? sum / n : glass_amb;
}

static void bake(tk_bmp *b, tk_a8 *mask, float x, float y, float w, float h, float r, int dark, int shadow, int exact)
{
    if (!b->px) return;
    gbake G;
    gsize z = gsize_of(w, h, r);
    G.b = b; G.mask = mask; G.x = x; G.y = y; G.w = w; G.h = h; G.shadow = shadow; G.exact = exact;
    /* P4 (the GPU row): glass with a mask is drawn by the compositor's lens (comp2 on the GPU, draw_lens on the CPU:
     * the backdrop bent, toned and lit per frame), from the shape d_glass records: the bake keeps only the shadow */
    G.lensed = tk_glass_lens && mask && mask->v;
    G.sg = z.sg; G.sdy = z.sdy; G.sop = z.sop;
    G.srho = PT(4.8f);                                          /* the glint's reach (its bleed) */
    G.R = tk_radii(w, h, r);
    G.rm = tk_maxf(tk_maxf(G.R.tl, G.R.tr), tk_maxf(G.R.br, G.R.bl));
    G.Bz = tk_maxf(G.srho, PT(.75f)) + 1;
    /* glass V3 (G2): the face, lens.h's preset (a small element flips by Lb; .lg.dark and large glass are dark),
     * linearised at Lb as an over-layer c (1 - a) + add: the slope T lumY'(Lb), the level E + T lumY(Lb) */
    float lb = backdrop_lb(b, x, y, w, h);
    lens_make(G.L, w, h, TK_PT, 0, 0, 0, dark ? -1 : lb);
    float T = G.L[LQ_TR], lift = G.L[LQ_LIFT], lum = lb + lift * lb * (1 - lb);
    float slope = tk_clampf(T * (1 + lift * (1 - 2 * lb)), 0, 1), lev = tk_clampf(G.L[LQ_ER] + T * lum, 0, 1);
    float ta = 1 - slope, add = tk_clampf(lev - slope * lb, 0, ta);
    G.tone_a = ta;
    G.tone_c = add * 255;
    G.vref = tk_clampf(lev, .06f, .94f);
    float mg = shadow ? 3 * G.sg + G.sdy + 2 : LENS_APRON;
    G.X0 = tk_maxi(0, (int)tk_floorf(x - mg)); G.X1 = tk_mini(b->w, (int)(x + w + mg) + 1);
    int Y0 = tk_maxi(0, (int)tk_floorf(y - (shadow ? 3 * G.sg - G.sdy : 0) - LENS_APRON)), Y1 = tk_mini(b->h, (int)(y + h + mg) + 1);
    if (exact) gbake_rows(&G, Y0, Y1);
    else {
        (void)phi(0);                       /* the table, before any band uses it */
        tk_par_rows(Y0, Y1, gbake_rows, &G);
    }
}

void tk_glass_bake(tk_bmp *b, tk_a8 *mask, float x, float y, float w, float h, float r, int dark, int shadow)
{
    bake(b, mask, x, y, w, h, r, dark, shadow, 0);
}

void tk_glass_bake_ref(tk_bmp *b, tk_a8 *mask, float x, float y, float w, float h, float r, int dark, int shadow)
{
    bake(b, mask, x, y, w, h, r, dark, shadow, 1);
}
