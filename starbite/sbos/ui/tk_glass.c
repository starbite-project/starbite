/* SPDX-License-Identifier: MIT
 * sbtk Liquid Glass: the concept's .lg recipe baked once per element. Nothing in it animates or reacts to touch, so
 * the tint gradient, the inset rims and glow, the two specular radials, the 1 pt gradient border and the drop
 * shadow all go into the element's cached layer; only the backdrop (the wallpaper blurred 14 pt, saturated 1.9,
 * brightened 1.06, made once per wallpaper) is sampled per frame, under the element where it is now. */
#include "sbtk.h"

static const tk_col WHITE0 = { 255, 255, 255, 0 };

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

void tk_wall_draw(tk_bmp *b, int emergency, float k)
{
    /* .wall: inset -30 pt, so the gradient box is 450 x 862 pt from (-30, -30) */
    float bx = PT(-30), by = PT(-30), bw = PT(450), bh = PT(862);
    tk_paint lin, r1, r2, r3;
    if (!emergency) {
        tk_stop l[3] = { { 0, tk_hex(0x071131, 1) }, { .45f, tk_hex(0x0b1a4c, 1) }, { 1, tk_hex(0x050a1e, 1) } };
        tk_stop a[2] = { { 0, TK_RGBA(70, 120, 255, .6f) }, { .6f, TK_RGBA(0, 0, 0, 0) } };
        tk_stop c[2] = { { 0, TK_RGBA(40, 80, 210, .42f) }, { .7f, TK_RGBA(0, 0, 0, 0) } };
        tk_stop d[2] = { { 0, TK_RGBA(30, 60, 160, .25f) }, { .7f, TK_RGBA(0, 0, 0, 0) } };
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
    tk_par_rows(0, b->h, wall_rows, &W);
}

static void bilinear_up(tk_bmp *dst, const tk_bmp *src)
{
    float sx = (float)src->w / dst->w, sy = (float)src->h / dst->h;
    for (int j = 0; j < dst->h; j++) {
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

void tk_glass_backdrop(tk_bmp *dst, const tk_bmp *src)
{
    /* src and dst at half resolution; the blur at a quarter (sigma 14 pt = 51.7 px is far wider than what that
     * loses), then up */
    tk_bmp q;
    tk_bmp_new(&q, src->w / 2, src->h / 2);
    tk_bmp_down2(&q, src);
    tk_bmp_blur(&q, PT(14) / 4);
    tk_bmp_satbright(&q, 1.9f, 1.06f);
    bilinear_up(dst, &q);
    tk_bmp_free(&q);
}

void tk_glass_bake_ref(tk_bmp *b, tk_a8 *mask, float x, float y, float w, float h, float r, int dark, int shadow)
{
    if (shadow)
        tk_box_shadow(b, x, y, w, h, r, 0, PT(10), PT(28), 0, TK_RGBA(0, 0, 0, .26f), 0);
    /* background: the white vertical gradient over the tint */
    tk_col tint = dark ? TK_RGBA(14, 22, 48, .42f) : TK_RGBA(36, 56, 120, .20f);
    tk_paint pt = tk_solid(tint);
    tk_fill_rrect(b, x, y, w, h, r, &pt, 1);
    tk_stop g[3] = { { 0, TK_RGBA(255, 255, 255, dark ? .08f : .12f) }, { .46f, TK_RGBA(255, 255, 255, dark ? .02f : .035f) },
                     { 1, TK_RGBA(255, 255, 255, dark ? .04f : .06f) } };
    tk_paint pg = tk_linear(180, x, y, w, h, 3, g);
    tk_fill_rrect(b, x, y, w, h, r, &pg, 1);
    /* inset shadows, the last listed lowest: the glow, the bottom rim, the top rim */
    tk_box_shadow(b, x, y, w, h, r, 0, 0, PT(22), 0, TK_RGBA(255, 255, 255, .05f), 1);
    tk_box_shadow(b, x, y, w, h, r, 0, PT(-1), 0, 0, TK_RGBA(255, 255, 255, .12f), 1);
    tk_box_shadow(b, x, y, w, h, r, 0, PT(1.5f), 0, 0, TK_RGBA(255, 255, 255, .42f), 1);
    /* ::before: the specular radials */
    tk_stop s1[2] = { { 0, TK_RGBA(255, 255, 255, .24f) }, { .55f, WHITE0 } };
    tk_stop s2[2] = { { 0, TK_RGBA(160, 200, 255, .12f) }, { .6f, TK_RGBA(160, 200, 255, 0) } };
    s2[1].c = TK_RGBA(0, 0, 0, 0);
    s1[1].c = TK_RGBA(0, 0, 0, 0);
    tk_paint q2 = tk_radial(.8f, .5f, 1.0f, 1.2f, x, y, w, h, 2, s2);
    tk_paint q1 = tk_radial(1.1f, .7f, .18f, -.1f, x, y, w, h, 2, s1);
    tk_fill_rrect(b, x, y, w, h, r, &q2, 1);
    tk_fill_rrect(b, x, y, w, h, r, &q1, 1);
    /* ::after: the 1 pt ring (border box minus content box) with its 140 deg gradient */
    {
        int ox = (int)tk_floorf(x) - 1, oy = (int)tk_floorf(y) - 1, W = (int)w + 4, H = (int)h + 4;
        tk_fm o, in;
        if (tk_fm_new(&o, W, H) && tk_fm_new(&in, W, H)) {
            float p = PT(1);
            tk_fm_rrect(&o, (float)ox, (float)oy, x, y, w, h, r);
            tk_r4 keep = tk_corner;
            if (tk_use_r4) { tk_corner.tl = tk_maxf(0, keep.tl - p); tk_corner.tr = tk_maxf(0, keep.tr - p); tk_corner.br = tk_maxf(0, keep.br - p); tk_corner.bl = tk_maxf(0, keep.bl - p); }
            tk_fm_rrect(&in, (float)ox, (float)oy, x + p, y + p, w - 2 * p, h - 2 * p, tk_maxf(0, r - p));
            tk_corner = keep;
            tk_fm_mul(&o, &in, 1);
            tk_stop e[5] = { { 0, TK_RGBA(255, 255, 255, .65f) }, { .28f, TK_RGBA(255, 255, 255, .08f) },
                             { .5f, TK_RGBA(255, 255, 255, 0) }, { .72f, TK_RGBA(255, 255, 255, .08f) },
                             { 1, TK_RGBA(255, 255, 255, .45f) } };
            tk_paint pe = tk_linear(140, x, y, w, h, 5, e);
            tk_draw_fm(b, &o, ox, oy, &pe, 1);
        }
        tk_fm_free(&o);
        tk_fm_free(&in);
    }
    if (mask && mask->v) {
        int ox = (int)tk_floorf(x) - 1, oy = (int)tk_floorf(y) - 1, W = (int)w + 4, H = (int)h + 4;
        tk_fm m;
        if (tk_fm_new(&m, W, H)) {
            tk_fm_rrect(&m, (float)ox, (float)oy, x, y, w, h, r);
            for (int j = 0; j < H; j++)
                for (int i = 0; i < W; i++) {
                    int X = ox + i, Y = oy + j;
                    if (X < 0 || Y < 0 || X >= mask->w || Y >= mask->h) continue;
                    int v = (int)(m.v[j * W + i] * 255 + .5f);
                    if (v > mask->v[Y * mask->w + X]) mask->v[Y * mask->w + X] = (uint8_t)v;
                }
            tk_fm_free(&m);
        }
    }
}

typedef struct { tk_bmp *b; const tk_bmp *t; const tk_fm *m; int X, Y; } put_ctx;
static void put_rows(void *a, int ja, int jb)
{
    put_ctx *P = a;
    tk_bmp *b = P->b;
    const tk_bmp *t = P->t;
    const tk_fm *m = P->m;
    int X = P->X, Y = P->Y, W = t->w;
    for (int j = ja; j < jb; j++) {
        int by = Y + j;
        if (by < 0 || by >= b->h) continue;
        for (int i = 0; i < W; i++) {
            unsigned c = (unsigned)(m->v[j * W + i] * 255 + .5f);
            int bx = X + i;
            if (!c || bx < 0 || bx >= b->w) continue;
            uint32_t sp = t->px[j * t->stride + i], *d = &b->px[by * b->stride + bx], o = *d, res = 0;
            unsigned sa = ((sp >> 24) * c + 127) / 255;
            for (int sh = 0; sh < 32; sh += 8) {
                unsigned v = ((sp >> sh & 255) * c + (o >> sh & 255) * (255 - sa) + 127) / 255;
                res |= (v > 255 ? 255 : v) << sh;
            }
            *d = res;
        }
    }
}

void tk_glass_local_put(tk_bmp *b, const tk_bmp *t, int X, int Y, float x, float y, float w, float h, float r)
{
    /* t (the filtered backdrop, its px (0, 0) at b's (X, Y)) over b inside the rounded box */
    int W = t->w, H = t->h;
    tk_fm m;
    if (!tk_fm_new(&m, W, H)) return;
    tk_fm_rrect(&m, (float)X, (float)Y, x, y, w, h, r);
    put_ctx P = { b, t, &m, X, Y };
    tk_par_rows(0, t->h, put_rows, &P);
    tk_fm_free(&m);
}

void tk_glass_local(tk_bmp *b, const tk_bmp *src, float x, float y, float w, float h, float r)
{
    /* Chrome's backdrop-filter: the backdrop under the element's box (edges mirrored), blurred, saturated,
     * brightened, drawn over b inside the shape */
    int X = (int)tk_floorf(x), Y = (int)tk_floorf(y), W = (int)(x + w + .999f) - X, H = (int)(y + h + .999f) - Y;
    tk_bmp t;
    if (!tk_bmp_new(&t, W, H)) return;
    tk_bmp_copy(&t, 0, 0, src, X, Y, W, H);
    tk_bmp_blur_sat(&t, PT(14), 1.9f, 1.06f);
    tk_glass_local_put(b, &t, X, Y, x, y, w, h, r);
    tk_bmp_free(&t);
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

/* ---- the same recipe in one pass per pixel (the bake the phone uses) -------------------------------------------
 * Everything .lg paints is a function of the pixel's signed distance to the rounded box (and of the box shifted by
 * the rims' offsets), its position (the gradients) and, for the blurred parts, a gaussian of that distance: the
 * drop shadow outside (0 10 28: sigma 14 pt, offset 10 pt) and the glow inside (inset 0 0 22: sigma 11 pt). The
 * gaussian of a distance is exact along straight edges and close at the corners, which is where tk_glass_bake_ref
 * (fill, blur and draw per part: about fifteen passes over the element) and this one differ, by a level or two. */
static float PHI[1025];                     /* the standard normal CDF on [-4, 4] */
static int phi_ok;
static float phi(float x)
{
    if (!phi_ok) {
        /* Abramowitz-Stegun 7.1.26 for erf */
        for (int i = 0; i <= 1024; i++) {
            float v = -4 + 8.0f * i / 1024, z = tk_fabsf(v) / 1.41421356f, t = 1 / (1 + .3275911f * z);
            float e = 1 - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t - .284496736f) * t + .254829592f) * t * tk_expf(-z * z);
            PHI[i] = .5f * (1 + (v < 0 ? -e : e));
        }
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
    float x, y, w, h, r, sg, sdy, gg, p1, r15;
    int X0, X1, shadow;
    tk_r4 R, Ri;                            /* the box's radii and the 1 pt ring's inner ones */
    tk_col tint;
    tk_paint pg, q1, q2, pe;
} gbake;

static void gbake_rows(void *a, int Y0, int Y1)
{
    gbake *G = a;
    tk_bmp *b = G->b;
    tk_a8 *mask = G->mask;
    float x = G->x, y = G->y, w = G->w, h = G->h, sg = G->sg, sdy = G->sdy, gg = G->gg, p1 = G->p1, r15 = G->r15;
    for (int j = Y0; j < Y1; j++) {
        float py = j + .5f;
        tk_col grow = tk_paint_at(&G->pg, x + w / 2, py);      /* the vertical gradient: one colour per row */
        for (int i = G->X0; i < G->X1; i++) {
            float px = i + .5f;
            float sd = tk_sd_rrect4(px, py, x, y, w, h, &G->R);
            float cov = tk_clampf(.5f - sd, 0, 1);
            float out[4] = { 0, 0, 0, 0 };
            if (G->shadow && cov < 1) {
                /* the drop shadow: the box moved down 10 pt, blurred; only outside the box (as Chrome clips it) */
                float ss = tk_sd_rrect4(px, py - sdy, x, y, w, h, &G->R);
                float al = .26f * phi(-ss / sg) * (1 - cov);
                if (al > 1.0f / 1024) over_col(out, 0, 0, 0, al);
            }
            if (cov > 0) {
                float f[4] = { 0, 0, 0, 0 };
                over_col(f, G->tint.r, G->tint.g, G->tint.b, G->tint.a);
                over4(f, grow.r, grow.g, grow.b, grow.a);
                if (-sd < 3 * gg) over_col(f, 255, 255, 255, .05f * (1 - phi(-sd / gg)));   /* the inset glow */
                /* the rims: the box minus the box moved by the offset (the bottom rim: up 1 pt; the top: down 1.5 pt) */
                if (py > y + h - 4 * p1) {
                    float hb = tk_clampf(.5f - tk_sd_rrect4(px, py + p1, x, y, w, h, &G->R), 0, 1);
                    if (hb < 1) over_col(f, 255, 255, 255, .12f * (1 - hb));
                }
                if (py < y + 4 * r15 || sd > -3 * r15) {
                    float ht = tk_clampf(.5f - tk_sd_rrect4(px, py - r15, x, y, w, h, &G->R), 0, 1);
                    if (ht < 1) over_col(f, 255, 255, 255, .42f * (1 - ht));
                }
                tk_col c2 = tk_paint_at(&G->q2, px, py), c1 = tk_paint_at(&G->q1, px, py);
                over4(f, c2.r, c2.g, c2.b, c2.a);
                over4(f, c1.r, c1.g, c1.b, c1.a);
                if (sd > -p1 - 1) {                 /* the 1 pt gradient ring */
                    float ci = tk_clampf(.5f - tk_sd_rrect4(px, py, x + p1, y + p1, w - 2 * p1, h - 2 * p1, &G->Ri), 0, 1);
                    float ring = 1 - ci;
                    if (ring > 0) {
                        tk_col ce = tk_paint_at(&G->pe, px, py);
                        over4(f, ce.r * ring, ce.g * ring, ce.b * ring, ce.a * ring);
                    }
                }
                over4(out, f[0] * cov, f[1] * cov, f[2] * cov, f[3] * cov);
            }
            if (out[3] <= 0) continue;
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

void tk_glass_bake(tk_bmp *b, tk_a8 *mask, float x, float y, float w, float h, float r, int dark, int shadow)
{
    if (!b->px) return;
    (void)phi(0);                           /* the table, before any band uses it */
    gbake G;
    G.b = b; G.mask = mask; G.x = x; G.y = y; G.w = w; G.h = h; G.r = r; G.shadow = shadow;
    G.sg = PT(14); G.sdy = PT(10); G.gg = PT(11); G.p1 = PT(1); G.r15 = PT(1.5f);
    float mg = shadow ? 3 * G.sg + G.sdy + 2 : 2;
    G.X0 = tk_maxi(0, (int)tk_floorf(x - mg)); G.X1 = tk_mini(b->w, (int)(x + w + mg) + 1);
    int Y0 = tk_maxi(0, (int)tk_floorf(y - (shadow ? 3 * G.sg - G.sdy : 0) - 2)), Y1 = tk_mini(b->h, (int)(y + h + mg) + 1);
    G.R = tk_radii(w, h, r);
    G.Ri = (tk_r4){ tk_maxf(0, G.R.tl - G.p1), tk_maxf(0, G.R.tr - G.p1), tk_maxf(0, G.R.br - G.p1), tk_maxf(0, G.R.bl - G.p1) };
    if (!tk_use_r4) {                       /* the reference's inner box: r - 1 pt, then CSS-scaled to its own size */
        float ri = tk_maxf(0, r - G.p1), m = tk_minf(w - 2 * G.p1, h - 2 * G.p1) / 2;
        if (ri > m) ri = m;
        G.Ri = (tk_r4){ ri, ri, ri, ri };
    }
    G.tint = dark ? TK_RGBA(14, 22, 48, .42f) : TK_RGBA(36, 56, 120, .20f);
    tk_stop g[3] = { { 0, TK_RGBA(255, 255, 255, dark ? .08f : .12f) }, { .46f, TK_RGBA(255, 255, 255, dark ? .02f : .035f) },
                     { 1, TK_RGBA(255, 255, 255, dark ? .04f : .06f) } };
    G.pg = tk_linear(180, x, y, w, h, 3, g);
    tk_stop s1[2] = { { 0, TK_RGBA(255, 255, 255, .24f) }, { .55f, TK_RGBA(0, 0, 0, 0) } };
    tk_stop s2[2] = { { 0, TK_RGBA(160, 200, 255, .12f) }, { .6f, TK_RGBA(0, 0, 0, 0) } };
    G.q1 = tk_radial(1.1f, .7f, .18f, -.1f, x, y, w, h, 2, s1);
    G.q2 = tk_radial(.8f, .5f, 1.0f, 1.2f, x, y, w, h, 2, s2);
    tk_stop e[5] = { { 0, TK_RGBA(255, 255, 255, .65f) }, { .28f, TK_RGBA(255, 255, 255, .08f) },
                     { .5f, TK_RGBA(255, 255, 255, 0) }, { .72f, TK_RGBA(255, 255, 255, .08f) }, { 1, TK_RGBA(255, 255, 255, .45f) } };
    G.pe = tk_linear(140, x, y, w, h, 5, e);
    tk_par_rows(Y0, Y1, gbake_rows, &G);
}

void tk_glass_from_half(tk_bmp *b, int box, int boy, const tk_bmp *half, float x, float y, float w, float h, float r)
{
    /* tk_glass_local for a backdrop that exists only at half resolution (the focus scrim): the element's region of
     * it, down to a quarter, blurred 14 pt, saturated 1.9, brightened 1.06, then sampled up into b inside the shape.
     * b's origin is the panel point (box, boy); (x, y, w, h) are in b's px */
    int X0 = (int)tk_floorf(x), Y0 = (int)tk_floorf(y), X1 = (int)(x + w + .999f), Y1 = (int)(y + h + .999f);
    int hx0 = (box + X0) >> 1, hy0 = (boy + Y0) >> 1, hx1 = (box + X1 + 1) >> 1, hy1 = (boy + Y1 + 1) >> 1;
    hx0 = tk_maxi(0, hx0); hy0 = tk_maxi(0, hy0); hx1 = tk_mini(half->w, hx1); hy1 = tk_mini(half->h, hy1);
    tk_bmp t, q;
    if (!tk_bmp_new(&t, hx1 - hx0, hy1 - hy0)) return;
    tk_bmp_copy(&t, 0, 0, half, hx0, hy0, t.w, t.h);
    if (!tk_bmp_new(&q, (t.w + 1) / 2, (t.h + 1) / 2)) { tk_bmp_free(&t); return; }
    tk_bmp_down2(&q, &t);
    tk_bmp_free(&t);
    tk_bmp_blur_sat(&q, PT(14) / 4, 1.9f, 1.06f);
    /* q px (i, j) covers panel px [4 i, 4 i + 4) from (2 hx0, 2 hy0) */
    tk_bmp up;
    if (tk_bmp_new(&up, X1 - X0, Y1 - Y0)) {
        tk_bmp_up(&up, &q, box + X0 - 2 * hx0, boy + Y0 - 2 * hy0, 4);
        tk_glass_local_put(b, &up, X0, Y0, x, y, w, h, r);
        tk_bmp_free(&up);
    }
    tk_bmp_free(&q);
}
