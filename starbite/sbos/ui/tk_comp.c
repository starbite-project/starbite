/* SPDX-License-Identifier: MIT
 * sbtk compositor. A frame composites the cached layers (tk_layer: a premultiplied bitmap, an optional glass mask,
 * translate / scale about an origin / opacity / a clip with mask-image fades / a 3-slice stretch) over the static
 * wallpaper; nothing is drawn, only copied and blended:
 *   - damage: each visible layer's screen rect and its transform / opacity / content generation are compared with
 *     the last frame; only the union of what changed is composed again (up to TK_MAXD rects, merged);
 *   - glass: under a layer with a glass mask, the target moves toward the wallpaper's glass backdrop (made once) by
 *     mask x opacity, then the layer is blended on top;
 *   - motion frames (the caller says so while a spring or a finger moves things) compose into the 720 x 1480 canvas
 *     from each layer's 2x2-downscaled copy and the present scales the damage 2x to the panel (NEON, 1/4 3/4
 *     weights); the first frame at rest composes at full resolution everything motion frames touched;
 *   - the inner loops are NEON (8 px per step: vld4 / vmull / vqadd), as uibench (P0) measures them on the phone.
 * The kernel's version splits the rects' rows over the four A55 (menu7's glass_smp pattern); the host runs one
 * thread, so its times are one core's. */
#include "sbtk.h"
#include <arm_neon.h>

#define TK_MAXD 16
#define TK_MAXITEM 256

void tk_layer_init(tk_layer *l, const char *name)
{
    tk_memset(l, 0, sizeof *l);
    l->name = name;
    l->sx = l->sy = 1;
    l->alpha = 1;
    l->gen = 1;
}
void tk_layer_add(tk_layer *p, tk_layer *c)
{
    c->next = 0;
    if (!p->child) { p->child = c; return; }
    tk_layer *t = p->child;
    while (t->next) t = t->next;
    t->next = c;
}
void tk_layer_dirty(tk_layer *l)
{
    l->gen++;
    tk_bmp_free(&l->half);
    tk_a8_free(&l->ghalf);
}

/* ---- the flattened draw list ------------------------------------------------------------------------------ */
typedef struct {
    tk_layer *l;
    float a, b, tx, ty;       /* screen = (a x + tx, b y + ty) for bitmap px (x, y) */
    float alpha;
    tk_irect clip;            /* screen, half-open */
    float ft0, ft1, fb0, fb1; /* fade: alpha 0 at ft0 .. 1 at ft1; 1 at fb0 .. 0 at fb1 (screen y) */
    int fade;
    tk_irect r;               /* screen rect drawn (clipped) */
    float key[8];
} item;

typedef struct { item it[TK_MAXITEM]; int n; } dlist;
static dlist cur, prev, flist;
static dlist *W = &cur;

static tk_irect irect_of(float x0, float y0, float x1, float y1)
{
    tk_irect r = { (int)tk_floorf(x0), (int)tk_floorf(y0), -(int)tk_floorf(-x1), -(int)tk_floorf(-y1) };
    return r;
}
static tk_irect isect(tk_irect a, tk_irect b)
{
    tk_irect r = { tk_maxi(a.x0, b.x0), tk_maxi(a.y0, b.y0), tk_mini(a.x1, b.x1), tk_mini(a.y1, b.y1) };
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    return r;
}
static int iempty(tk_irect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
static tk_irect iunion(tk_irect a, tk_irect b)
{
    if (iempty(a)) return b;
    if (iempty(b)) return a;
    tk_irect r = { tk_mini(a.x0, b.x0), tk_mini(a.y0, b.y0), tk_maxi(a.x1, b.x1), tk_maxi(a.y1, b.y1) };
    return r;
}

static void walk(tk_layer *l, float a, float b, float tx, float ty, float alpha, tk_irect clip,
                 float ft0, float ft1, float fb0, float fb1, int fade)
{
    for (; l; l = l->next) {
        if (l->hidden || l->alpha <= 0) continue;
        /* the layer's own transform: p = s (q - o) + o + (x, y) in the parent's space */
        /* (x, y) is the offset of the bitmap / children; the scale applies about (ox, oy) of the parent's space */
        float la = l->sx, lb = l->sy, ltx = l->sx * l->x + (1 - l->sx) * l->ox, lty = l->sy * l->y + (1 - l->sy) * l->oy;
        float A = a * la, B = b * lb, TX = a * ltx + tx, TY = b * lty + ty, AL = alpha * l->alpha;
        tk_irect c = clip;
        float f0 = ft0, f1 = ft1, g0 = fb0, g1 = fb1;
        int fd = fade;
        if (l->clip) {
            tk_rect r = l->clip_r;     /* in the parent's space */
            tk_irect s = irect_of(a * r.x + tx, b * r.y + ty, a * (r.x + r.w) + tx, b * (r.y + r.h) + ty);
            c = isect(c, s);
            if (l->fade_top > 0 || l->fade_bot > 0) {
                f0 = b * r.y + ty; f1 = f0 + b * l->fade_top;
                g1 = b * (r.y + r.h) + ty; g0 = g1 - b * l->fade_bot;
                fd = 1;
            }
        }
        if (l->bmp.px && W->n < TK_MAXITEM) {
            item *it = &W->it[W->n];
            float h = l->slice == 1 ? l->slice_h : (float)l->bmp.h, w = l->slice == 2 ? l->slice_h : (float)l->bmp.w;
            it->l = l; it->a = A; it->b = B; it->tx = TX; it->ty = TY; it->alpha = AL;
            it->clip = c; it->fade = fd; it->ft0 = f0; it->ft1 = f1; it->fb0 = g0; it->fb1 = g1;
            it->r = isect(irect_of(TX, TY, TX + A * w, TY + B * h), c);
            if (!iempty(it->r)) {
                it->key[0] = A; it->key[1] = B; it->key[2] = TX; it->key[3] = TY; it->key[4] = AL;
                it->key[5] = (float)l->gen; it->key[6] = h; it->key[7] = (float)(c.x0 + 3 * c.y0 + 7 * c.x1 + 11 * c.y1);
                W->n++;
            }
        }
        if (l->child)
            walk(l->child, A, B, TX, TY, AL, c, f0, f1, g0, g1, fd);
    }
}

/* ---- the row kernels (NEON) ------------------------------------------------------------------------------- */
static inline uint8x8_t div255(uint16x8_t m) { return vrshrn_n_u16(vrsraq_n_u16(m, m, 8), 8); }

/* d = s x al / 255 over d; al 0..255 */
static void row_over(uint32_t *d, const uint32_t *s, int n, unsigned al)
{
    int i = 0;
    if (al >= 255) {
        for (; i + 8 <= n; i += 8) {
            uint8x8x4_t S = vld4_u8((const uint8_t *)(s + i)), D = vld4_u8((uint8_t *)(d + i));
            uint8x8_t ia = vmvn_u8(S.val[3]);
            for (int c = 0; c < 4; c++) D.val[c] = vqadd_u8(S.val[c], div255(vmull_u8(D.val[c], ia)));
            vst4_u8((uint8_t *)(d + i), D);
        }
    } else {
        uint8x8_t va = vdup_n_u8((uint8_t)al);
        for (; i + 8 <= n; i += 8) {
            uint8x8x4_t S = vld4_u8((const uint8_t *)(s + i)), D = vld4_u8((uint8_t *)(d + i));
            for (int c = 0; c < 4; c++) S.val[c] = div255(vmull_u8(S.val[c], va));
            uint8x8_t ia = vmvn_u8(S.val[3]);
            for (int c = 0; c < 4; c++) D.val[c] = vqadd_u8(S.val[c], div255(vmull_u8(D.val[c], ia)));
            vst4_u8((uint8_t *)(d + i), D);
        }
    }
    for (; i < n; i++) {
        uint32_t S = s[i], D = d[i], o = 0;
        if (al < 255) {
            uint32_t t = 0;
            for (int sh = 0; sh < 32; sh += 8) t |= (((S >> sh & 255) * al + 127) / 255) << sh;
            S = t;
        }
        uint32_t ia = 255 - (S >> 24);
        for (int sh = 0; sh < 32; sh += 8) {
            uint32_t v = (S >> sh & 255) + ((D >> sh & 255) * ia + 127) / 255;
            o |= (v > 255 ? 255 : v) << sh;
        }
        d[i] = o;
    }
}
/* d moves toward b by m x al / 255 (the glass backdrop under its mask) */
static void row_glass(uint32_t *d, const uint32_t *b, const uint8_t *m, int n, unsigned al)
{
    int i = 0;
    uint8x8_t va = vdup_n_u8((uint8_t)(al > 255 ? 255 : al));
    for (; i + 8 <= n; i += 8) {
        uint8x8_t w = vld1_u8(m + i);
        if (!vmaxv_u8(w)) continue;
        w = div255(vmull_u8(w, va));
        uint8x8_t iw = vmvn_u8(w);
        uint8x8x4_t D = vld4_u8((uint8_t *)(d + i)), Bk = vld4_u8((const uint8_t *)(b + i));
        for (int c = 0; c < 4; c++) D.val[c] = div255(vmlal_u8(vmull_u8(D.val[c], iw), Bk.val[c], w));
        vst4_u8((uint8_t *)(d + i), D);
    }
    for (; i < n; i++) {
        uint32_t w = (m[i] * al + 127) / 255, D = d[i], Bk = b[i], o = 0;
        if (!w) continue;
        for (int sh = 0; sh < 32; sh += 8) o |= (((D >> sh & 255) * (255 - w) + (Bk >> sh & 255) * w + 127) / 255) << sh;
        d[i] = o;
    }
}

/* ---- drawing one item into a target rect ------------------------------------------------------------------ */
typedef struct {
    tk_bmp *t;            /* target */
    tk_a8 *collect;       /* flatten: glass masks go here instead of the backdrop */
    const tk_bmp *bw;     /* glass backdrop at the target's resolution */
    float k;              /* target px per screen px (1 or .5) */
} tgt;

static const tk_bmp *half_of(tk_layer *l)
{
    if (!l->half.px) {
        tk_bmp_new(&l->half, (l->bmp.w + 1) / 2, (l->bmp.h + 1) / 2);
        tk_bmp_down2(&l->half, &l->bmp);
        if (l->glass.v) {
            tk_a8_new(&l->ghalf, l->half.w, l->half.h);
            for (int j = 0; j < l->ghalf.h; j++)
                for (int i = 0; i < l->ghalf.w; i++) {
                    int x0 = tk_mini(2 * i, l->glass.w - 1), x1 = tk_mini(2 * i + 1, l->glass.w - 1);
                    int y0 = tk_mini(2 * j, l->glass.h - 1), y1 = tk_mini(2 * j + 1, l->glass.h - 1);
                    const uint8_t *g = l->glass.v;
                    l->ghalf.v[j * l->ghalf.w + i] = (uint8_t)((g[y0 * l->glass.w + x0] + g[y0 * l->glass.w + x1] +
                                                                g[y1 * l->glass.w + x0] + g[y1 * l->glass.w + x1] + 2) >> 2);
                }
        }
    }
    return &l->half;
}

static float fade_at(const item *it, float y)
{
    if (!it->fade) return 1;
    float f = 1;
    if (y < it->ft1) f = it->ft1 > it->ft0 ? tk_clampf((y - it->ft0) / (it->ft1 - it->ft0), 0, 1) : (y < it->ft0 ? 0 : 1);
    if (y > it->fb0) f *= it->fb1 > it->fb0 ? tk_clampf((it->fb1 - y) / (it->fb1 - it->fb0), 0, 1) : (y > it->fb1 ? 0 : 1);
    return f;
}

/* source row for destination row v (in the item's own px at the source's resolution) under a 3-slice */
static int slice_row(const tk_layer *l, int v, int srch, float res)
{
    float sh = l->slice_h * res, s0 = l->slice_s0 * res, s1 = l->slice_s1 * res;
    if (v < s0) return v;
    if (v >= sh - s1) return srch - (int)(sh - v + .5f);
    float mid = sh - s0 - s1, smid = srch - s0 - s1;
    return (int)(s0 + (v - s0) * (mid > 0 ? smid / mid : 0));
}

static uint32_t rowbuf[TK_W + 16], vrow[TK_W + 16];
static uint8_t maskbuf[TK_W + 16], mrow[TK_W + 16];
static int colu[TK_W + 16];
static uint16_t colw[TK_W + 16];

static void draw_item(const tgt *T, const item *it, tk_irect R /* target px */)
{
    tk_layer *l = it->l;
    float k = T->k, A = it->a * k, B = it->b * k, TX = it->tx * k, TY = it->ty * k;
    /* the source: the half copy when it is the closer size */
    const tk_bmp *src = &l->bmp;
    const tk_a8 *gm = l->glass.v ? &l->glass : 0;
    float res = 1;
    if (A < .75f && B < .75f) {
        src = half_of(l);
        gm = l->ghalf.v ? &l->ghalf : 0;
        res = .5f;
        /* half px i covers full px 2i, 2i + 1: target px per source px doubles */
        A = it->a * k / res; B = it->b * k / res;
    }
    int glass = gm && (T->collect || T->bw) && !(l->glass_opaque_only && it->alpha < .999f);
    tk_irect d = isect(R, irect_of(it->r.x0 * k, it->r.y0 * k, it->r.x1 * k, it->r.y1 * k));
    if (iempty(d)) return;
    float ia = 1.0f / A, ib = 1.0f / B;
    int fast = tk_fabsf(A - 1) < 1e-4f && tk_fabsf(B - 1) < 1e-4f && l->slice != 2;
    int ox = (int)tk_floorf(TX + .5f), oy = (int)tk_floorf(TY + .5f);   /* snapped offset for the fast path */
    int colok = 0, umin = 0, umax = 0;
    for (int y = d.y0; y < d.y1; y++) {
        float f = fade_at(it, (y + .5f) / k);
        unsigned al = (unsigned)(it->alpha * f * 255 + .5f);
        if (!al) continue;
        uint32_t *dst = T->t->px + (size_t)y * T->t->stride + d.x0;
        int n = d.x1 - d.x0;
        if (fast) {
            int v = y - oy;
            if (l->slice) v = slice_row(l, v, src->h, res);
            if (v < 0 || v >= src->h) continue;
            int u0 = d.x0 - ox, i0 = 0, i1 = n;
            if (u0 < 0) i0 = -u0;
            if (u0 + n > src->w) i1 = src->w - u0;
            if (i1 <= i0) continue;
            if (glass && T->collect) {
                uint8_t *cm = T->collect->v + (size_t)y * T->collect->w + d.x0 + i0;
                const uint8_t *sm = gm->v + (size_t)v * gm->w + u0 + i0;
                for (int q = 0; q < i1 - i0; q++) { unsigned m = (sm[q] * al + 127) / 255; if (m > cm[q]) cm[q] = (uint8_t)m; }
            } else if (glass)
                row_glass(dst + i0, T->bw->px + (size_t)y * T->bw->stride + d.x0 + i0, gm->v + (size_t)v * gm->w + u0 + i0, i1 - i0, al);
            row_over(dst + i0, src->px + (size_t)v * src->stride + u0 + i0, i1 - i0, al);
        } else {
            /* bilinear, separable: the two source rows blended once over the span, then each target px from two
             * neighbours of that row (the columns' table is made once per item) */
            float sv = ((y + .5f) - TY) * ib - .5f;
            if (l->slice == 1) sv = (float)slice_row(l, (int)tk_floorf(sv + .5f), src->h, res);
            int v0 = (int)tk_floorf(sv);
            if (v0 < -1 || v0 >= src->h) continue;
            unsigned wy = (unsigned)((sv - v0) * 256 + .5f);
            const uint32_t *ra = v0 >= 0 ? src->px + (size_t)v0 * src->stride : 0;
            const uint32_t *rb = v0 + 1 < src->h ? src->px + (size_t)(v0 + 1) * src->stride : 0;
            const uint8_t *ma = gm && ra ? gm->v + (size_t)v0 * gm->w : 0, *mb = gm && rb ? gm->v + (size_t)(v0 + 1) * gm->w : 0;
            if (!colok) {
                for (int q = 0; q < n; q++) {
                    float su = ((d.x0 + q + .5f) - TX) * ia - .5f;
                    if (l->slice == 2) su = (float)slice_row(l, (int)tk_floorf(su + .5f), src->w, res);
                    int u0 = (int)tk_floorf(su);
                    colu[q] = u0;
                    colw[q] = (uint16_t)((su - u0) * 256 + .5f);
                }
                umin = colu[0]; umax = colu[n - 1] + 1;
                if (umin > umax) { int t = umin; umin = umax - 1; umax = t + 1; }
                umin = tk_maxi(umin, -1); umax = tk_mini(umax, src->w);
                colok = 1;
            }
            /* vblend[u + 1] for u in [umin, umax], zero outside the source */
            uint32_t *vb = vrow + 1;
            int ua = tk_maxi(umin, 0), ub = tk_mini(umax, src->w - 1);
            if (umin < 0) vb[-1] = 0;
            if (umax >= src->w) vb[src->w] = 0;
            if (ra && rb) {
                int u = ua;
                uint8x8_t w1 = vdup_n_u8((uint8_t)(wy > 255 ? 255 : wy)), w0 = vdup_n_u8((uint8_t)(255 - (wy > 255 ? 255 : wy)));
                for (; u + 2 <= ub + 1; u += 2) {
                    uint8x8_t A8 = vld1_u8((const uint8_t *)(ra + u)), B8 = vld1_u8((const uint8_t *)(rb + u));
                    vst1_u8((uint8_t *)(vb + u), div255(vmlal_u8(vmull_u8(A8, w0), B8, w1)));
                }
                for (; u <= ub; u++) {
                    uint32_t a = ra[u], b = rb[u], o = 0;
                    for (int sh = 0; sh < 32; sh += 8) o |= (((a >> sh & 255) * (256 - wy) + (b >> sh & 255) * wy + 128) >> 8) << sh;
                    vb[u] = o;
                }
                if (gm) for (int u2 = ua; u2 <= ub; u2++) mrow[u2 + 1] = (uint8_t)((ma[u2] * (256 - wy) + mb[u2] * wy + 128) >> 8);
            } else {
                const uint32_t *r1 = ra ? ra : rb;
                const uint8_t *m1 = ma ? ma : mb;
                unsigned w = ra ? 256 - wy : wy;
                for (int u = ua; u <= ub; u++) {
                    uint32_t a = r1[u], o = 0;
                    for (int sh = 0; sh < 32; sh += 8) o |= (((a >> sh & 255) * w + 128) >> 8) << sh;
                    vb[u] = o;
                    if (gm) mrow[u + 1] = (uint8_t)((m1[u] * w + 128) >> 8);
                }
            }
            if (gm) { if (umin < 0) mrow[0] = 0; if (umax >= src->w) mrow[src->w + 1] = 0; }
            int q = 0;
            if (!gm && n >= 4 && colu[0] >= -1 && colu[n - 1] + 1 <= src->w) {
                /* 4 px a step: the neighbours gathered into lanes, the lerp in NEON (16-bit weights) */
                for (; q + 4 <= n; q += 4) {
                    uint32x4_t P0 = vdupq_n_u32(0), P1 = vdupq_n_u32(0);
                    P0 = vsetq_lane_u32(vb[colu[q]], P0, 0); P1 = vsetq_lane_u32(vb[colu[q] + 1], P1, 0);
                    P0 = vsetq_lane_u32(vb[colu[q + 1]], P0, 1); P1 = vsetq_lane_u32(vb[colu[q + 1] + 1], P1, 1);
                    P0 = vsetq_lane_u32(vb[colu[q + 2]], P0, 2); P1 = vsetq_lane_u32(vb[colu[q + 2] + 1], P1, 2);
                    P0 = vsetq_lane_u32(vb[colu[q + 3]], P0, 3); P1 = vsetq_lane_u32(vb[colu[q + 3] + 1], P1, 3);
                    uint16x4_t w4 = vld1_u16(colw + q);
                    uint16x8_t W = vcombine_u16(vzip1_u16(w4, w4), vzip2_u16(w4, w4));          /* per px, x2 */
                    uint8x16_t a = vreinterpretq_u8_u32(P0), b = vreinterpretq_u8_u32(P1);
                    /* per byte: a + (b - a) x w / 256, done as a x (256 - w) + b x w in 16 bits */
                    uint16x8_t wl = vzip1q_u16(W, W), wh = vzip2q_u16(W, W);                     /* per byte, 8 bytes each */
                    uint16x8_t al = vmovl_u8(vget_low_u8(a)), ah = vmovl_u8(vget_high_u8(a));
                    uint16x8_t bl = vmovl_u8(vget_low_u8(b)), bh = vmovl_u8(vget_high_u8(b));
                    uint16x8_t rl = vmlaq_u16(vmulq_u16(al, vsubq_u16(vdupq_n_u16(256), wl)), bl, wl);
                    uint16x8_t rh = vmlaq_u16(vmulq_u16(ah, vsubq_u16(vdupq_n_u16(256), wh)), bh, wh);
                    vst1q_u8((uint8_t *)(rowbuf + q), vcombine_u8(vshrn_n_u16(rl, 8), vshrn_n_u16(rh, 8)));
                }
            }
            for (; q < n; q++) {
                int u0 = colu[q];
                uint32_t p0 = u0 >= -1 && u0 < src->w ? vb[u0] : 0, p1 = u0 + 1 >= 0 && u0 + 1 <= src->w ? vb[u0 + 1] : 0;
                if (u0 < -1 || u0 + 1 > src->w) { rowbuf[q] = 0; if (gm) maskbuf[q] = 0; continue; }
                uint32_t wx = colw[q], iw = 256 - wx;
                uint32_t rb2 = ((p0 & 0x00ff00ffu) * iw + (p1 & 0x00ff00ffu) * wx) >> 8 & 0x00ff00ffu;
                uint32_t ga = ((p0 >> 8 & 0x00ff00ffu) * iw + (p1 >> 8 & 0x00ff00ffu) * wx) & 0xff00ff00u;
                rowbuf[q] = rb2 | ga;
                if (gm) maskbuf[q] = (uint8_t)((mrow[u0 + 1] * iw + mrow[u0 + 2] * wx) >> 8);
            }
            if (glass && T->collect) {
                uint8_t *cm = T->collect->v + (size_t)y * T->collect->w + d.x0;
                for (int q = 0; q < n; q++) { unsigned m = (maskbuf[q] * al + 127) / 255; if (m > cm[q]) cm[q] = (uint8_t)m; }
            } else if (glass)
                row_glass(dst, T->bw->px + (size_t)y * T->bw->stride + d.x0, maskbuf, n, al);
            row_over(dst, rowbuf, n, al);
        }
    }
}

/* ---- the present: the half canvas's rect, 2x, into the panel (1/4 3/4 bilinear) -------------------------- */
static uint32_t prT[TK_W / 2 + 32], prB[TK_W / 2 + 32];
static void up2x_rect(tk_bmp *fb, const tk_bmp *h, tk_irect r)
{
    /* per half row: the two vertical blends (3/4 this row + 1/4 the row above / below) into padded temp rows,
     * then each horizontally into two panel px (3/4 + 1/4 the left / right neighbour): 3 loads per 4 px */
    int x0 = r.x0, x1 = r.x1, n = x1 - x0;
    uint32_t *T = prT + 8, *B = prB + 8;
    for (int j = r.y0; j < r.y1; j++) {
        const uint32_t *c = h->px + (size_t)j * h->stride;
        const uint32_t *u = h->px + (size_t)tk_maxi(0, j - 1) * h->stride;
        const uint32_t *d = h->px + (size_t)tk_mini(h->h - 1, j + 1) * h->stride;
        int a0 = tk_maxi(0, x0 - 1), a1 = tk_mini(h->w, x1 + 1), i = a0;
        for (; i + 4 <= a1; i += 4) {
            uint8x16_t C = vld1q_u8((const uint8_t *)(c + i)), U = vld1q_u8((const uint8_t *)(u + i)), D = vld1q_u8((const uint8_t *)(d + i));
            vst1q_u8((uint8_t *)(T + i - x0), vrhaddq_u8(C, vrhaddq_u8(C, U)));
            vst1q_u8((uint8_t *)(B + i - x0), vrhaddq_u8(C, vrhaddq_u8(C, D)));
        }
        for (; i < a1; i++) {
            uint32_t ot = 0, ob = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                ot |= ((3 * (c[i] >> sh & 255) + (u[i] >> sh & 255) + 2) >> 2) << sh;
                ob |= ((3 * (c[i] >> sh & 255) + (d[i] >> sh & 255) + 2) >> 2) << sh;
            }
            T[i - x0] = ot;
            B[i - x0] = ob;
        }
        if (x0 == 0) { T[-1] = T[0]; B[-1] = B[0]; }
        if (x1 == h->w) { T[n] = T[n - 1]; B[n] = B[n - 1]; }
        uint32_t *o0 = fb->px + (size_t)(2 * j) * fb->stride + 2 * x0, *o1 = o0 + fb->stride;
        int k = 0;
        for (; k + 4 <= n; k += 4) {
            uint8x16_t Tc = vld1q_u8((const uint8_t *)(T + k)), Tl = vld1q_u8((const uint8_t *)(T + k - 1)), Tr = vld1q_u8((const uint8_t *)(T + k + 1));
            uint8x16_t Bc = vld1q_u8((const uint8_t *)(B + k)), Bl = vld1q_u8((const uint8_t *)(B + k - 1)), Br = vld1q_u8((const uint8_t *)(B + k + 1));
            uint32x4x2_t t = vzipq_u32(vreinterpretq_u32_u8(vrhaddq_u8(Tc, vrhaddq_u8(Tc, Tl))), vreinterpretq_u32_u8(vrhaddq_u8(Tc, vrhaddq_u8(Tc, Tr))));
            uint32x4x2_t b = vzipq_u32(vreinterpretq_u32_u8(vrhaddq_u8(Bc, vrhaddq_u8(Bc, Bl))), vreinterpretq_u32_u8(vrhaddq_u8(Bc, vrhaddq_u8(Bc, Br))));
            vst1q_u32(o0 + 2 * k, t.val[0]); vst1q_u32(o0 + 2 * k + 4, t.val[1]);
            vst1q_u32(o1 + 2 * k, b.val[0]); vst1q_u32(o1 + 2 * k + 4, b.val[1]);
        }
        for (; k < n; k++)
            for (int q = 0; q < 4; q++) {
                const uint32_t *R = q < 2 ? T : B;
                uint32_t a = R[k], e = R[k + (q & 1 ? 1 : -1)], o = 0;
                for (int sh = 0; sh < 32; sh += 8) o |= ((3 * (a >> sh & 255) + (e >> sh & 255) + 2) >> 2) << sh;
                (q < 2 ? o0 : o1)[2 * k + (q & 1)] = o;
            }
    }
}

/* ---- the frame -------------------------------------------------------------------------------------------- */
int tk_comp_init(tk_comp *c)
{
    if (!tk_bmp_new(&c->fb, TK_W, TK_H) || !tk_bmp_new(&c->base, TK_W, TK_H) || !tk_bmp_new(&c->bw, TK_W, TK_H) ||
        !tk_bmp_new(&c->base_half, TK_W / 2, TK_H / 2) || !tk_bmp_new(&c->bw_half, TK_W / 2, TK_H / 2) ||
        !tk_bmp_new(&c->half, TK_W / 2, TK_H / 2))
        return 0;
    return 1;
}

static int was_motion = -1;

int tk_comp_frame(tk_comp *c, int motion, int full)
{
    uint64_t t0 = tk_us();
    cur.n = 0;
    W = &cur;
    tk_irect screen = { 0, 0, TK_W, TK_H };
    walk(c->root, 1, 1, 0, 0, 1, screen, 0, 0, 0, 0, 0);

    /* damage */
    tk_irect D[TK_MAXD];
    int nd = 0;
    tk_irect bbox = { 0, 0, 0, 0 };
    if (full || was_motion < 0 || (motion && !was_motion)) {
        D[nd++] = screen;
    } else {
        tk_irect acc[TK_MAXITEM * 2];
        int na = 0;
        for (int i = 0; i < cur.n; i++) {
            item *it = &cur.it[i];
            int found = 0;
            for (int j = 0; j < prev.n; j++)
                if (prev.it[j].l == it->l) {
                    found = 1;
                    int same = 1;
                    for (int q = 0; q < 8; q++) if (prev.it[j].key[q] != it->key[q]) same = 0;
                    if (!same || prev.it[j].fade != it->fade || prev.it[j].ft0 != it->ft0) { acc[na++] = prev.it[j].r; acc[na++] = it->r; }
                    break;
                }
            if (!found) acc[na++] = it->r;
        }
        for (int j = 0; j < prev.n; j++) {
            int found = 0;
            for (int i = 0; i < cur.n; i++) if (cur.it[i].l == prev.it[j].l) { found = 1; break; }
            if (!found) acc[na++] = prev.it[j].r;
        }
        if (!motion && was_motion && c->have_moved) acc[na++] = c->moved;
        for (int i = 0; i < na; i++) {
            tk_irect r = isect(acc[i], screen);
            if (iempty(r)) continue;
            /* merge into an overlapping rect, else add, else everything into the last */
            int m = -1;
            for (int j = 0; j < nd; j++)
                if (!(r.x1 <= D[j].x0 || r.x0 >= D[j].x1 || r.y1 <= D[j].y0 || r.y0 >= D[j].y1)) { m = j; break; }
            if (m >= 0) D[m] = iunion(D[m], r);
            else if (nd < TK_MAXD) D[nd++] = r;
            else D[nd - 1] = iunion(D[nd - 1], r);
        }
        /* merging can make new overlaps */
        for (int again = 1; again;) {
            again = 0;
            for (int i = 0; i < nd && !again; i++)
                for (int j = i + 1; j < nd; j++)
                    if (!(D[i].x1 <= D[j].x0 || D[i].x0 >= D[j].x1 || D[i].y1 <= D[j].y0 || D[i].y0 >= D[j].y1)) {
                        D[i] = iunion(D[i], D[j]);
                        D[j] = D[--nd];
                        again = 1;
                        break;
                    }
        }
    }
    if (!motion) { c->have_moved = 0; c->moved = (tk_irect){ 0, 0, 0, 0 }; }

    tgt T = { motion ? &c->half : &c->fb, 0, motion ? &c->bw_half : &c->bw, motion ? .5f : 1 };
    const tk_bmp *base = motion ? &c->base_half : &c->base;
    int px = 0;
    for (int q = 0; q < nd; q++) {
        tk_irect r = D[q];
        if (motion) {
            r = (tk_irect){ r.x0 / 2, r.y0 / 2, (r.x1 + 1) / 2, (r.y1 + 1) / 2 };
            D[q] = r;
            c->moved = iunion(c->moved, (tk_irect){ 2 * r.x0, 2 * r.y0, 2 * r.x1, 2 * r.y1 });
            c->have_moved = 1;
        }
        bbox = iunion(bbox, r);
        px += (r.x1 - r.x0) * (r.y1 - r.y0);
        for (int y = r.y0; y < r.y1; y++)
            tk_memcpy(T.t->px + (size_t)y * T.t->stride + r.x0, base->px + (size_t)y * base->stride + r.x0, (size_t)(r.x1 - r.x0) * 4);
        for (int i = 0; i < cur.n; i++) draw_item(&T, &cur.it[i], r);
    }
    uint64_t t1 = tk_us();
    if (motion)
        for (int q = 0; q < nd; q++) {
            /* one half px of margin for the bilinear neighbours */
            tk_irect r = isect((tk_irect){ D[q].x0 - 1, D[q].y0 - 1, D[q].x1 + 1, D[q].y1 + 1 }, (tk_irect){ 0, 0, TK_W / 2, TK_H / 2 });
            up2x_rect(&c->fb, &c->half, r);
        }
    uint64_t t2 = tk_us();
    prev = cur;
    was_motion = motion;
    c->last_half = motion;
    c->last_px = motion ? 4 * px : px;
    c->us_comp = t1 - t0;
    c->us_present = t2 - t1;
    c->last_us = t2 - t0;
    (void)bbox;
    return c->last_px;
}

void tk_flatten(tk_layer *g, tk_bmp *out, tk_a8 *mask)
{
    flist.n = 0;
    W = &flist;
    tk_irect screen = { 0, 0, out->w, out->h };
    walk(g->child, 1, 1, 0, 0, 1, screen, 0, 0, 0, 0, 0);
    W = &cur;
    tk_memset(out->px, 0, (size_t)out->stride * out->h * 4);
    if (mask && mask->v) tk_memset(mask->v, 0, (size_t)mask->w * mask->h);
    tgt T = { out, mask && mask->v ? mask : 0, 0, 1 };
    for (int i = 0; i < flist.n; i++) draw_item(&T, &flist.it[i], screen);
}

void tk_layer_prepare(tk_layer *l) { if (l->bmp.px) half_of(l); }

static void prep(tk_layer *l)
{
    for (; l; l = l->next) {
        if (l->hidden) continue;
        if (l->bmp.px && !l->half.px) half_of(l);
        prep(l->child);
    }
}
/* idle time: the half copies the next motion frame will need (else its first frame makes them) */
void tk_comp_prepare(tk_comp *c) { prep(c->root); }
