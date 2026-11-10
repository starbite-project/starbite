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

static uint32_t rowbuf[TK_W + 16];
static uint8_t maskbuf[TK_W + 16];

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
            /* bilinear: target px centre -> source px */
            float sv = ((y + .5f) - TY) * ib - .5f;
            if (l->slice == 1) sv = (float)slice_row(l, (int)tk_floorf(sv + .5f), src->h, res);
            int v0 = (int)tk_floorf(sv);
            float fy = sv - v0;
            if (v0 < -1 || v0 >= src->h) continue;
            int va = tk_maxi(0, v0), vb = tk_mini(src->h - 1, v0 + 1);
            unsigned wy = (unsigned)(fy * 256 + .5f);
            if (v0 < 0) wy = 256;
            if (v0 + 1 >= src->h) wy = 0;
            const uint32_t *ra = src->px + (size_t)va * src->stride, *rb = src->px + (size_t)vb * src->stride;
            for (int i = 0; i < n; i++) {
                float su = ((d.x0 + i + .5f) - TX) * ia - .5f;
                if (l->slice == 2) su = (float)slice_row(l, (int)tk_floorf(su + .5f), src->w, res);
                int u0 = (int)tk_floorf(su);
                unsigned wx = (unsigned)((su - u0) * 256 + .5f);
                uint32_t p[4] = { 0, 0, 0, 0 };
                uint8_t mk[4] = { 0, 0, 0, 0 };
                for (int q = 0; q < 4; q++) {
                    int uu = u0 + (q & 1), vv = q < 2 ? va : vb;
                    if (uu < 0 || uu >= src->w) continue;
                    if (q < 2 && v0 < 0) continue;
                    if (q >= 2 && v0 + 1 >= src->h) continue;
                    p[q] = (q < 2 ? ra : rb)[uu];
                    if (gm) mk[q] = gm->v[(size_t)vv * gm->w + uu];
                }
                uint32_t o = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    unsigned t0 = (p[0] >> sh & 255) * (256 - wx) + (p[1] >> sh & 255) * wx;
                    unsigned t1 = (p[2] >> sh & 255) * (256 - wx) + (p[3] >> sh & 255) * wx;
                    o |= ((t0 * (256 - wy) + t1 * wy + 32768) >> 16) << sh;
                }
                rowbuf[i] = o;
                if (gm) {
                    unsigned t0 = mk[0] * (256 - wx) + mk[1] * wx, t1 = mk[2] * (256 - wx) + mk[3] * wx;
                    maskbuf[i] = (uint8_t)((t0 * (256 - wy) + t1 * wy + 32768) >> 16);
                }
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
static void up2x_rect(tk_bmp *fb, const tk_bmp *h, tk_irect r)
{
    for (int j = r.y0; j < r.y1; j++) {
        const uint32_t *c = h->px + (size_t)j * h->stride;
        const uint32_t *nu = h->px + (size_t)tk_maxi(0, j - 1) * h->stride;     /* row above */
        const uint32_t *nd = h->px + (size_t)tk_mini(h->h - 1, j + 1) * h->stride;
        uint32_t *o0 = fb->px + (size_t)(2 * j) * fb->stride, *o1 = o0 + fb->stride;
        int i = r.x0;
        for (; i + 4 <= r.x1 && i >= 1 && i + 5 <= h->w; i += 4) {
            uint8x16_t C = vld1q_u8((const uint8_t *)(c + i)), L = vld1q_u8((const uint8_t *)(c + i - 1)), Rr = vld1q_u8((const uint8_t *)(c + i + 1));
            uint8x16_t U = vld1q_u8((const uint8_t *)(nu + i)), D = vld1q_u8((const uint8_t *)(nd + i));
            uint8x16_t UL = vld1q_u8((const uint8_t *)(nu + i - 1)), UR = vld1q_u8((const uint8_t *)(nu + i + 1));
            uint8x16_t DL = vld1q_u8((const uint8_t *)(nd + i - 1)), DR = vld1q_u8((const uint8_t *)(nd + i + 1));
            /* vertical 3/4 1/4 first, then horizontal */
            uint8x16_t cu = vrhaddq_u8(C, vrhaddq_u8(C, U)), cd = vrhaddq_u8(C, vrhaddq_u8(C, D));
            uint8x16_t lu = vrhaddq_u8(L, vrhaddq_u8(L, UL)), ld = vrhaddq_u8(L, vrhaddq_u8(L, DL));
            uint8x16_t ru = vrhaddq_u8(Rr, vrhaddq_u8(Rr, UR)), rd = vrhaddq_u8(Rr, vrhaddq_u8(Rr, DR));
            uint8x16_t a0 = vrhaddq_u8(cu, vrhaddq_u8(cu, lu)), a1 = vrhaddq_u8(cu, vrhaddq_u8(cu, ru));
            uint8x16_t b0 = vrhaddq_u8(cd, vrhaddq_u8(cd, ld)), b1 = vrhaddq_u8(cd, vrhaddq_u8(cd, rd));
            uint32x4x2_t t = vzipq_u32(vreinterpretq_u32_u8(a0), vreinterpretq_u32_u8(a1));
            uint32x4x2_t b = vzipq_u32(vreinterpretq_u32_u8(b0), vreinterpretq_u32_u8(b1));
            vst1q_u32(o0 + 2 * i, t.val[0]); vst1q_u32(o0 + 2 * i + 4, t.val[1]);
            vst1q_u32(o1 + 2 * i, b.val[0]); vst1q_u32(o1 + 2 * i + 4, b.val[1]);
        }
        for (; i < r.x1; i++) {
            int il = tk_maxi(0, i - 1), ir = tk_mini(h->w - 1, i + 1);
            for (int q = 0; q < 4; q++) {
                const uint32_t *rv = q < 2 ? nu : nd;
                int ih = q & 1 ? ir : il;
                uint32_t o = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    unsigned v = 9 * (c[i] >> sh & 255) + 3 * (c[ih] >> sh & 255) + 3 * (rv[i] >> sh & 255) + (rv[ih] >> sh & 255);
                    o |= ((v + 8) >> 4) << sh;
                }
                (q < 2 ? o0 : o1)[2 * i + (q & 1)] = o;
            }
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
