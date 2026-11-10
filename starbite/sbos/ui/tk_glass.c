/* SPDX-License-Identifier: MIT
 * sbtk Liquid Glass: the concept's .lg recipe baked once per element. Nothing in it animates or reacts to touch, so
 * the tint gradient, the inset rims and glow, the two specular radials, the 1 pt gradient border and the drop
 * shadow all go into the element's cached layer; only the backdrop (the wallpaper blurred 14 pt, saturated 1.9,
 * brightened 1.06, made once per wallpaper) is sampled per frame, under the element where it is now. */
#include "sbtk.h"

static const tk_col WHITE0 = { 255, 255, 255, 0 };

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
    for (int j = 0; j < b->h; j++)
        for (int i = 0; i < b->w; i++) {
            float x = (i + .5f) / k, y = (j + .5f) / k;
            tk_col c = tk_paint_at(&lin, x, y);
            const tk_paint *L[3] = { &r3, &r2, &r1 };
            for (int k = 0; k < 3; k++) {
                tk_col s = tk_paint_at(L[k], x, y);
                c.r = s.r + c.r * (1 - s.a); c.g = s.g + c.g * (1 - s.a); c.b = s.b + c.b * (1 - s.a);
            }
            int R = (int)(c.r + .5f), G = (int)(c.g + .5f), B = (int)(c.b + .5f);
            b->px[j * b->stride + i] = 0xff000000u | (uint32_t)(R > 255 ? 255 : R) << 16 | (uint32_t)(G > 255 ? 255 : G) << 8 | (uint32_t)(B > 255 ? 255 : B);
        }
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

void tk_bmp_up(tk_bmp *dst, const tk_bmp *src, int x0, int y0, float f)
{
    /* dst px (i, j) = the bilinear sample of src at the point (x0 + i, y0 + j) of a space f times src's */
    for (int j = 0; j < dst->h; j++) {
        float fy = (y0 + j + .5f) / f - .5f;
        int ya = (int)tk_floorf(fy);
        unsigned ty = (unsigned)((fy - ya) * 256);
        int y_0 = tk_maxi(0, tk_mini(src->h - 1, ya)), y_1 = tk_maxi(0, tk_mini(src->h - 1, ya + 1));
        for (int i = 0; i < dst->w; i++) {
            float fx = (x0 + i + .5f) / f - .5f;
            int xa = (int)tk_floorf(fx);
            unsigned tx = (unsigned)((fx - xa) * 256);
            int x_0 = tk_maxi(0, tk_mini(src->w - 1, xa)), x_1 = tk_maxi(0, tk_mini(src->w - 1, xa + 1));
            uint32_t p00 = src->px[y_0 * src->stride + x_0], p01 = src->px[y_0 * src->stride + x_1];
            uint32_t p10 = src->px[y_1 * src->stride + x_0], p11 = src->px[y_1 * src->stride + x_1], o = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                unsigned t0 = (p00 >> sh & 255) * (256 - tx) + (p01 >> sh & 255) * tx, t1 = (p10 >> sh & 255) * (256 - tx) + (p11 >> sh & 255) * tx;
                o |= ((t0 * (256 - ty) + t1 * ty + 32768) >> 16) << sh;
            }
            dst->px[j * dst->stride + i] = o;
        }
    }
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

void tk_glass_bake(tk_bmp *b, tk_a8 *mask, float x, float y, float w, float h, float r, int dark, int shadow)
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

void tk_glass_local(tk_bmp *b, const tk_bmp *src, float x, float y, float w, float h, float r)
{
    /* Chrome's backdrop-filter: the backdrop under the element's box (edges mirrored), blurred, saturated,
     * brightened, drawn over b inside the shape */
    int X = (int)tk_floorf(x), Y = (int)tk_floorf(y), W = (int)(x + w + .999f) - X, H = (int)(y + h + .999f) - Y;
    tk_bmp t;
    if (!tk_bmp_new(&t, W, H)) return;
    tk_bmp_copy(&t, 0, 0, src, X, Y, W, H);
    tk_bmp_blur(&t, PT(14));
    tk_bmp_satbright(&t, 1.9f, 1.06f);
    tk_fm m;
    if (tk_fm_new(&m, W, H)) {
        tk_fm_rrect(&m, (float)X, (float)Y, x, y, w, h, r);
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++) {
                float c = m.v[j * W + i];
                int bx = X + i, by = Y + j;
                if (c <= 0 || bx < 0 || by < 0 || bx >= b->w || by >= b->h) continue;
                uint32_t s = t.px[j * W + i], *d = &b->px[by * b->stride + bx], o = *d, res = 0;
                float sa = (s >> 24) / 255.0f * c;
                for (int sh = 0; sh < 32; sh += 8) {
                    float v = (s >> sh & 255) * c + (o >> sh & 255) * (1 - sa);
                    int q = (int)(v + .5f);
                    res |= (uint32_t)(q > 255 ? 255 : q) << sh;
                }
                *d = res;
            }
        tk_fm_free(&m);
    }
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
