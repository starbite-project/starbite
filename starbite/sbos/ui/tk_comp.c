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
    /* a layer made again keeps counting its generation: the compositor must see its new content as a change */
    uint32_t g = l->gen;
    tk_memset(l, 0, sizeof *l);
    l->gen = g + 1;
    l->name = name;
    l->sx = l->sy = 1;
    l->alpha = 1;
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
    tk_free(l->hspan);
    l->hspan = 0;
    tk_a8_free(&l->ghalf);
}

/* ---- the flattened draw list ------------------------------------------------------------------------------ */
typedef struct {
    tk_layer *l;
    float a, b, tx, ty;       /* screen = (a x + tx, b y + ty) for bitmap px (x, y) */
    float alpha;
    tk_irect clip;            /* screen, half-open */
    float crad;               /* the clip's corner radius (screen px; 0 square) */
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
                 float ft0, float ft1, float fb0, float fb1, int fade, float crad)
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
        float cr = crad;
        if (l->clip) {
            tk_rect r = l->clip_r;     /* in the parent's space */
            tk_irect s = irect_of(a * r.x + tx, b * r.y + ty, a * (r.x + r.w) + tx, b * (r.y + r.h) + ty);
            c = isect(c, s);
            if (l->clip_radius > 0) cr = a * l->clip_radius;
            if (l->fade_top > 0 || l->fade_bot > 0) {
                f0 = b * r.y + ty; f1 = f0 + b * l->fade_top;
                g1 = b * (r.y + r.h) + ty; g0 = g1 - b * l->fade_bot;
                fd = 1;
            }
        }
        if ((l->bmp.px || l->solid) && W->n < TK_MAXITEM) {
            item *it = &W->it[W->n];
            float h = l->solid ? l->solid_h : l->slice == 1 ? l->slice_h : (float)l->bmp.h;
            float w = l->solid ? l->solid_w : l->slice == 2 ? l->slice_h : (float)l->bmp.w;
            it->l = l; it->a = A; it->b = B; it->tx = TX; it->ty = TY; it->alpha = AL;
            it->clip = c; it->crad = cr; it->fade = fd; it->ft0 = f0; it->ft1 = f1; it->fb0 = g0; it->fb1 = g1;
            it->r = isect(irect_of(TX, TY, TX + A * w, TY + B * h), c);
            if (!iempty(it->r)) {
                it->key[0] = A; it->key[1] = B; it->key[2] = TX; it->key[3] = TY; it->key[4] = AL;
                it->key[5] = (float)l->gen; it->key[6] = h; it->key[7] = (float)(c.x0 + 3 * c.y0 + 7 * c.x1 + 11 * c.y1) + cr * 13;
                W->n++;
            }
        }
        if (l->child)
            walk(l->child, A, B, TX, TY, AL, c, f0, f1, g0, g1, fd, cr);
    }
}

/* ---- the row kernels (NEON) ------------------------------------------------------------------------------- */
static inline uint8x8_t div255(uint16x8_t m) { return vrshrn_n_u16(vrsraq_n_u16(m, m, 8), 8); }

/* d = s x al / 255 over d; al 0..255 */
static void row_over(uint32_t *d, const uint32_t *s, int n, unsigned al)
{
    int i = 0;
    if (al >= 255) {
        /* cached layers are mostly empty or opaque: an 8 px block with no coverage is skipped, an opaque one copied
         * (both exactly what the blend gives) */
        for (; i + 8 <= n; i += 8) {
            uint8x8x4_t S = vld4_u8((const uint8_t *)(s + i));
            if (!vmaxv_u8(S.val[3])) continue;
            if (vminv_u8(S.val[3]) == 255) {
                vst1q_u32(d + i, vld1q_u32(s + i));
                vst1q_u32(d + i + 4, vld1q_u32(s + i + 4));
                continue;
            }
            uint8x8x4_t D = vld4_u8((uint8_t *)(d + i));
            uint8x8_t ia = vmvn_u8(S.val[3]);
            for (int c = 0; c < 4; c++) D.val[c] = vqadd_u8(S.val[c], div255(vmull_u8(D.val[c], ia)));
            vst4_u8((uint8_t *)(d + i), D);
        }
    } else {
        uint8x8_t va = vdup_n_u8((uint8_t)al);
        for (; i + 8 <= n; i += 8) {
            uint8x8x4_t S = vld4_u8((const uint8_t *)(s + i));
            if (!vmaxv_u8(S.val[3])) continue;
            uint8x8x4_t D = vld4_u8((uint8_t *)(d + i));
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
/* per band scratch: a frame's rows are split into bands that run on several cores at once */
#define TK_NBAND 8
typedef struct {
    uint32_t rowbuf[TK_W + 16], vrow[TK_W + 16], bwrow[TK_W + 16], upV[TK_W / 2 + 32], upB[TK_W / 2 + 32];
    uint8_t maskbuf[TK_W + 16], mrow[TK_W + 16];
    int colu[TK_W + 16];
    uint16_t colw[TK_W + 16];
} scr;
static scr SCR[TK_NBAND];

typedef struct {
    scr *S;               /* this band's scratch */
    tk_bmp *t;            /* target */
    tk_a8 *collect;       /* flatten: glass masks go here instead of the backdrop */
    const tk_bmp *bw;     /* the glass backdrop (half resolution) */
    float k;              /* target px per screen px (1 or .5) */
    int bw_double;        /* the target is full resolution: each backdrop px covers 2 x 2 */
} tgt;
static const uint32_t *bw_row(const tgt *T, int y, int x0, int n)
{
    uint32_t *bwrow = T->S->bwrow;
    if (!T->bw_double) return T->bw->px + (size_t)y * T->bw->stride + x0;
    const uint32_t *r = T->bw->px + (size_t)(y >> 1) * T->bw->stride;
    for (int i = 0; i < n; i++) bwrow[i] = r[(x0 + i) >> 1];
    return bwrow;
}

static const tk_bmp *half_of(tk_layer *l)
{
    if (!l->half.px) {
        tk_bmp_new(&l->half, (l->bmp.w + 1) / 2, (l->bmp.h + 1) / 2);
        tk_bmp_down2(&l->half, &l->bmp);
        /* where each row has anything: a scaled draw (the app's zoom) then skips the empty rows and margins */
        if (l->half.px && (l->hspan = tk_alloc((size_t)l->half.h * 4)) != 0)
            for (int j = 0; j < l->half.h; j++) {
                const uint32_t *r = l->half.px + (size_t)j * l->half.stride;
                int a0 = 0, a1 = l->half.w;
                while (a0 < a1 && !r[a0]) a0++;
                while (a1 > a0 && !r[a1 - 1]) a1--;
                l->hspan[2 * j] = (uint16_t)a0;
                l->hspan[2 * j + 1] = (uint16_t)a1;
            }
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


/* a rounded clip (an app window growing from its icon): the row's span inside it, hard-edged (motion frames) */
static inline void round_span(const item *it, float k, int y, tk_irect *d)
{
    if (it->crad <= 0) return;
    float r = it->crad * k, y0 = it->clip.y0 * k, y1 = it->clip.y1 * k, yc = y + .5f, in = 0;
    if (yc < y0 + r) { float dy = y0 + r - yc; in = r - tk_sqrtf(tk_maxf(0, r * r - dy * dy)); }
    else if (yc > y1 - r) { float dy = yc - (y1 - r); in = r - tk_sqrtf(tk_maxf(0, r * r - dy * dy)); }
    if (in <= 0) return;
    int xa = (int)(it->clip.x0 * k + in + .5f), xb = (int)(it->clip.x1 * k - in + .5f);
    if (d->x0 < xa) d->x0 = xa;
    if (d->x1 > xb) d->x1 = xb;
}
static inline void up2x_hrow(uint32_t *o, const uint32_t *V, int X0, int X1);
static void draw_solid(const tgt *T, const item *it, tk_irect R)
{
    /* a solid-colour layer (the boot overlay's black): its rect filled, or blended at its opacity */
    float k = T->k;
    tk_irect d = isect(R, irect_of(it->r.x0 * k, it->r.y0 * k, it->r.x1 * k, it->r.y1 * k));
    if (iempty(d)) return;
    uint32_t c = it->l->solid, *rowbuf = T->S->rowbuf;
    tk_irect D0 = d;
    for (int y = D0.y0; y < D0.y1; y++) {
        tk_irect d = D0;
        round_span(it, k, y, &d);
        if (d.x1 <= d.x0) continue;
        float f = fade_at(it, (y + .5f) / k);
        unsigned al = (unsigned)(it->alpha * f * 255 + .5f);
        uint32_t *dst = T->t->px + (size_t)y * T->t->stride + d.x0;
        int n = d.x1 - d.x0;
        if (al >= 255 && (c >> 24) == 255) { for (int i = 0; i < n; i++) dst[i] = c; continue; }
        for (int i = 0; i < n; i += 64) {
            int m = n - i < 64 ? n - i : 64;
            for (int q = 0; q < m; q++) rowbuf[q] = c;
            row_over(dst + i, rowbuf, m, al);
        }
    }
}

static void draw_item(const tgt *T, const item *it, tk_irect R /* target px */)
{
    tk_layer *l = it->l;
    if (l->solid) { draw_solid(T, it, R); return; }
    uint32_t *rowbuf = T->S->rowbuf, *vrow = T->S->vrow;
    uint8_t *maskbuf = T->S->maskbuf, *mrow = T->S->mrow;
    int *colu = T->S->colu;
    uint16_t *colw = T->S->colw;
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
    /* exactly 2x on whole px (the half-resolution layers at rest: the scrim, the screen under the sheet): the
     * present's 1/4 3/4 kernel, which is what the bilinear gives at that scale */
    int two = tk_fabsf(A - 2) < 1e-4f && tk_fabsf(B - 2) < 1e-4f && !l->slice && !(glass && gm) &&
              tk_fabsf(TX - ox) < 1e-3f && tk_fabsf(TY - oy) < 1e-3f;
    int colok = 0, umin = 0, umax = 0, lastx0 = d.x0;
    tk_irect D0 = d;
    for (int y = D0.y0; y < D0.y1; y++) {
        tk_irect d = D0;
        round_span(it, k, y, &d);
        if (d.x1 <= d.x0) continue;
        if (d.x0 != lastx0 || d.x1 != D0.x1) { colok = 0; lastx0 = d.x0; }
        float f = fade_at(it, (y + .5f) / k);
        unsigned al = (unsigned)(it->alpha * f * 255 + .5f);
        if (!al) continue;
        if (two) {
            int yl = y - oy, j = yl >> 1;
            if (yl < 0 || j >= src->h) continue;
            int jn = yl & 1 ? tk_mini(src->h - 1, j + 1) : tk_maxi(0, j - 1);
            const uint32_t *c = src->px + (size_t)j * src->stride, *nr = src->px + (size_t)jn * src->stride;
            int X0 = tk_maxi(0, d.x0 - ox), X1 = tk_mini(2 * src->w, d.x1 - ox);
            if (X1 <= X0) continue;
            int i0 = tk_maxi(0, (X0 >> 1) - 1), i1 = tk_mini(src->w, ((X1 - 1) >> 1) + 2);
            uint32_t *V = T->S->upV + 8 - i0;
            int i = i0;
            for (; i + 4 <= i1; i += 4) {
                uint8x16_t C = vld1q_u8((const uint8_t *)(c + i)), N = vld1q_u8((const uint8_t *)(nr + i));
                vst1q_u8((uint8_t *)(V + i), vrhaddq_u8(C, vrhaddq_u8(C, N)));
            }
            for (; i < i1; i++) {
                uint32_t o = 0;
                for (int sh = 0; sh < 32; sh += 8) o |= ((3 * (c[i] >> sh & 255) + (nr[i] >> sh & 255) + 2) >> 2) << sh;
                V[i] = o;
            }
            V[i0 - 1] = V[i0];
            V[i1] = V[i1 - 1];
            up2x_hrow(rowbuf - X0, V, X0, X1);
            row_over(T->t->px + (size_t)y * T->t->stride + ox + X0, rowbuf, X1 - X0, al);
            continue;
        }
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
                row_glass(dst + i0, bw_row(T, y, d.x0 + i0, i1 - i0), gm->v + (size_t)v * gm->w + u0 + i0, i1 - i0, al);
            row_over(dst + i0, src->px + (size_t)v * src->stride + u0 + i0, i1 - i0, al);
        } else {
            if (T->k < 1 && !l->slice && !(glass && gm)) {
                /* a motion frame: the nearest source px (a scaled layer moves; the frame at rest is bilinear) */
                float sv0 = ((y + .5f) - TY) * ib;
                int v = (int)tk_floorf(sv0);
                if (v < 0 || v >= src->h) continue;
                if (!colok) {
                    for (int q = 0; q < n; q++) colu[q] = (int)tk_floorf(((d.x0 + q + .5f) - TX) * ia);
                    colok = 1;
                }
                const uint32_t *sr = src->px + (size_t)v * src->stride;
                int qa = 0, qb = n;
                if (src == &l->half && l->hspan) {
                    /* only the target px whose source column can hold anything */
                    int u0 = l->hspan[2 * v], u1 = l->hspan[2 * v + 1];
                    if (u1 <= u0) continue;
                    qa = tk_maxi(0, (int)tk_floorf(u0 * A + TX) - d.x0 - 1);
                    qb = tk_mini(n, (int)tk_floorf(u1 * A + TX) - d.x0 + 2);
                    if (qb <= qa) continue;
                }
                for (int q = qa; q < qb; q++) {
                    int u = colu[q];
                    rowbuf[q] = u >= 0 && u < src->w ? sr[u] : 0;
                }
                row_over(dst + qa, rowbuf + qa, qb - qa, al);
                continue;
            }
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
                row_glass(dst, bw_row(T, y, d.x0, n), maskbuf, n, al);
            row_over(dst, rowbuf, n, al);
        }
    }
}

/* ---- 2x up: a half-resolution image into an output rect (any parity) of a full one, 1/4 3/4 bilinear -------- */
/* one output row from its vertical blend V (padded: V[i0 - 1] .. V[i1]): 3/4 V[k] + 1/4 the left / right neighbour */
static inline void up2x_hrow(uint32_t *o, const uint32_t *V, int X0, int X1)
{
    int X = X0;
    if (X & 1) {                                     /* an odd first px: the right half of its pair */
        int k = X >> 1;
        uint32_t a = V[k], e = V[k + 1], r = 0;
        for (int sh = 0; sh < 32; sh += 8) r |= ((3 * (a >> sh & 255) + (e >> sh & 255) + 2) >> 2) << sh;
        o[X++] = r;
    }
    for (; X + 16 <= X1; X += 16) {                  /* 16 px (64 bytes) a step: whole write-combining bursts */
        int k = X >> 1;
        uint8x16_t c0 = vld1q_u8((const uint8_t *)(V + k)), l0 = vld1q_u8((const uint8_t *)(V + k - 1)), r0 = vld1q_u8((const uint8_t *)(V + k + 1));
        uint8x16_t c1 = vld1q_u8((const uint8_t *)(V + k + 4)), l1 = vld1q_u8((const uint8_t *)(V + k + 3)), r1 = vld1q_u8((const uint8_t *)(V + k + 5));
        uint32x4x2_t a = vzipq_u32(vreinterpretq_u32_u8(vrhaddq_u8(c0, vrhaddq_u8(c0, l0))), vreinterpretq_u32_u8(vrhaddq_u8(c0, vrhaddq_u8(c0, r0))));
        uint32x4x2_t b = vzipq_u32(vreinterpretq_u32_u8(vrhaddq_u8(c1, vrhaddq_u8(c1, l1))), vreinterpretq_u32_u8(vrhaddq_u8(c1, vrhaddq_u8(c1, r1))));
        uint32x4x4_t w = { { a.val[0], a.val[1], b.val[0], b.val[1] } };
        vst1q_u32_x4(o + X, w);
    }
    for (; X + 8 <= X1; X += 8) {
        int k = X >> 1;
        uint8x16_t Vc = vld1q_u8((const uint8_t *)(V + k)), Vl = vld1q_u8((const uint8_t *)(V + k - 1)), Vr = vld1q_u8((const uint8_t *)(V + k + 1));
        uint32x4x2_t t = vzipq_u32(vreinterpretq_u32_u8(vrhaddq_u8(Vc, vrhaddq_u8(Vc, Vl))), vreinterpretq_u32_u8(vrhaddq_u8(Vc, vrhaddq_u8(Vc, Vr))));
        vst1q_u32(o + X, t.val[0]);
        vst1q_u32(o + X + 4, t.val[1]);
    }
    for (; X < X1; X++) {
        int k = X >> 1;
        uint32_t a = V[k], e = V[X & 1 ? k + 1 : k - 1], r = 0;
        for (int sh = 0; sh < 32; sh += 8) r |= ((3 * (a >> sh & 255) + (e >> sh & 255) + 2) >> 2) << sh;
        o[X] = r;
    }
}

static void up2x_out(tk_bmp *dst, const tk_bmp *h, tk_irect R, scr *S)
{
    /* per half row j: the two vertical blends (3/4 row j + 1/4 the row above, for output row 2j; + 1/4 the row
     * below, for 2j + 1) from one pass over its three rows, then each output row horizontally */
    R = isect(R, (tk_irect){ 0, 0, dst->w, dst->h });
    if (iempty(R)) return;
    int i0 = tk_maxi(0, (R.x0 >> 1) - 1), i1 = tk_mini(h->w, ((R.x1 - 1) >> 1) + 2);
    uint32_t *T = S->upV + 8 - i0, *B = S->upB + 8 - i0;   /* [i0 - 1, i1] */
    for (int j = R.y0 >> 1; 2 * j < R.y1; j++) {
        int jj = tk_mini(j, h->h - 1), ju = tk_maxi(0, jj - 1), jd = tk_mini(h->h - 1, jj + 1);
        int top = 2 * j >= R.y0, bot = 2 * j + 1 < R.y1;
        const uint32_t *c = h->px + (size_t)jj * h->stride, *u = h->px + (size_t)ju * h->stride, *d = h->px + (size_t)jd * h->stride;
        int i = i0;
        for (; i + 4 <= i1; i += 4) {
            uint8x16_t C = vld1q_u8((const uint8_t *)(c + i));
            if (top) vst1q_u8((uint8_t *)(T + i), vrhaddq_u8(C, vrhaddq_u8(C, vld1q_u8((const uint8_t *)(u + i)))));
            if (bot) vst1q_u8((uint8_t *)(B + i), vrhaddq_u8(C, vrhaddq_u8(C, vld1q_u8((const uint8_t *)(d + i)))));
        }
        for (; i < i1; i++) {
            uint32_t ot = 0, ob = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                ot |= ((3 * (c[i] >> sh & 255) + (u[i] >> sh & 255) + 2) >> 2) << sh;
                ob |= ((3 * (c[i] >> sh & 255) + (d[i] >> sh & 255) + 2) >> 2) << sh;
            }
            T[i] = ot;
            B[i] = ob;
        }
        T[i0 - 1] = T[i0]; T[i1] = T[i1 - 1];
        B[i0 - 1] = B[i0]; B[i1] = B[i1 - 1];
        if (top) up2x_hrow(dst->px + (size_t)(2 * j) * dst->stride, T, R.x0, R.x1);
        if (bot) up2x_hrow(dst->px + (size_t)(2 * j + 1) * dst->stride, B, R.x0, R.x1);
    }
}

/* motion frames' present: uibench's 2x (half px i at panel px 2i; the odd px and rows averages of their
 * neighbours): half the work of the centred kernel; the half-px offset exists only while things move */
/* one panel row of the fast 2x (the edges of a band: an odd first row, a lone last row) */
static void up2x_row(uint32_t *o, const uint32_t *a, const uint32_t *b, int odd, int w, int x0, int x1)
{
    int i0 = x0 >> 1, i1 = tk_mini(w, (x1 + 1) >> 1), i = i0;
    for (; i + 5 <= i1 && i + 5 <= w; i += 4) {
        uint8x16_t A = vld1q_u8((const uint8_t *)(a + i)), An = vld1q_u8((const uint8_t *)(a + i + 1));
        if (odd) {
            An = vrhaddq_u8(An, vld1q_u8((const uint8_t *)(b + i + 1)));
            A = vrhaddq_u8(A, vld1q_u8((const uint8_t *)(b + i)));
        }
        uint32x4x2_t t = vzipq_u32(vreinterpretq_u32_u8(A), vreinterpretq_u32_u8(vrhaddq_u8(A, An)));
        vst1q_u32(o + 2 * i, t.val[0]);
        vst1q_u32(o + 2 * i + 4, t.val[1]);
    }
    for (; i < i1; i++) {
        int in = tk_mini(w - 1, i + 1);
        uint32_t p = a[i], q = a[in];
        if (odd) {
            uint32_t p2 = 0, q2 = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                p2 |= (((p >> sh & 255) + (b[i] >> sh & 255) + 1) >> 1) << sh;
                q2 |= (((q >> sh & 255) + (b[in] >> sh & 255) + 1) >> 1) << sh;
            }
            p = p2; q = q2;
        }
        uint32_t m = 0;
        for (int sh = 0; sh < 32; sh += 8) m |= (((p >> sh & 255) + (q >> sh & 255) + 1) >> 1) << sh;
        if (2 * i >= x0 && 2 * i < x1) o[2 * i] = p;
        if (2 * i + 1 >= x0 && 2 * i + 1 < x1) o[2 * i + 1] = m;
    }
}

/* motion frames' present: uibench's 2x (half px i at panel px 2i; the odd px and rows averages of their
 * neighbours). Rows go in pairs from one set of loads (4 loads per 8 px pair, as uibench's loop): the in-order
 * A55 paid ~1.7x for one row at a time (6 loads per pair and a branch in the loop) */
static void up2x_fast(tk_bmp *dst, const tk_bmp *h, tk_irect R)
{
    R = isect(R, (tk_irect){ 0, 0, dst->w, dst->h });
    if (iempty(R)) return;
    int w = h->w, Y = R.y0;
#define HROW(j) (h->px + (size_t)tk_mini(h->h - 1, (j)) * h->stride)
    if (Y & 1) { up2x_row(dst->px + (size_t)Y * dst->stride, HROW(Y >> 1), HROW((Y >> 1) + 1), 1, w, R.x0, R.x1); Y++; }
    int i0 = R.x0 >> 1, i1 = tk_mini(w, (R.x1 + 1) >> 1), iv = i0;
    while (iv + 5 <= i1 && iv + 5 <= w) iv += 4;          /* [i0, iv) in NEON, [iv, i1) by up2x_row */
    for (; Y + 1 < R.y1; Y += 2) {
        const uint8_t *r0 = (const uint8_t *)HROW(Y >> 1), *r1 = (const uint8_t *)HROW((Y >> 1) + 1);
        uint32_t *o0 = dst->px + (size_t)Y * dst->stride, *o1 = o0 + dst->stride;
        for (int i = i0; i < iv; i += 4) {
            uint8x16_t A = vld1q_u8(r0 + 4 * i), An = vld1q_u8(r0 + 4 * i + 4);
            uint8x16_t B = vld1q_u8(r1 + 4 * i), Bn = vld1q_u8(r1 + 4 * i + 4);
            uint8x16_t Ah = vrhaddq_u8(A, An), V = vrhaddq_u8(A, B), Vn = vrhaddq_u8(An, Bn);
            uint32x4x2_t t = vzipq_u32(vreinterpretq_u32_u8(A), vreinterpretq_u32_u8(Ah));
            uint32x4x2_t m = vzipq_u32(vreinterpretq_u32_u8(V), vreinterpretq_u32_u8(vrhaddq_u8(V, Vn)));
            vst1q_u32(o0 + 2 * i, t.val[0]); vst1q_u32(o0 + 2 * i + 4, t.val[1]);
            vst1q_u32(o1 + 2 * i, m.val[0]); vst1q_u32(o1 + 2 * i + 4, m.val[1]);
        }
        if (iv < i1) {
            int x0 = tk_maxi(R.x0, 2 * iv);
            up2x_row(o0, (const uint32_t *)r0, (const uint32_t *)r1, 0, w, x0, R.x1);
            up2x_row(o1, (const uint32_t *)r0, (const uint32_t *)r1, 1, w, x0, R.x1);
        }
    }
    if (Y < R.y1) up2x_row(dst->px + (size_t)Y * dst->stride, HROW(Y >> 1), HROW((Y >> 1) + 1), 0, w, R.x0, R.x1);
#undef HROW
}

static void down2_rect(tk_bmp *h, const tk_bmp *f, tk_irect r)
{
    int x0 = r.x0 >> 1, x1 = tk_mini(h->w, (r.x1 + 1) >> 1), y0 = r.y0 >> 1, y1 = tk_mini(h->h, (r.y1 + 1) >> 1);
    for (int j = y0; j < y1; j++) {
        const uint32_t *a = f->px + (size_t)(2 * j) * f->stride, *b = a + f->stride;
        uint32_t *o = h->px + (size_t)j * h->stride;
        int i = x0;
        for (; i + 4 <= x1; i += 4) {
            uint32x4x2_t A = vld2q_u32(a + 2 * i), B = vld2q_u32(b + 2 * i);   /* even and odd px */
            uint8x16_t t = vrhaddq_u8(vreinterpretq_u8_u32(A.val[0]), vreinterpretq_u8_u32(A.val[1]));
            uint8x16_t u = vrhaddq_u8(vreinterpretq_u8_u32(B.val[0]), vreinterpretq_u8_u32(B.val[1]));
            vst1q_u32(o + i, vreinterpretq_u32_u8(vrhaddq_u8(t, u)));
        }
        for (; i < x1; i++) {
            uint32_t p0 = a[2 * i], p1 = a[2 * i + 1], p2 = b[2 * i], p3 = b[2 * i + 1], v = 0;
            for (int sh = 0; sh < 32; sh += 8)
                v |= (((p0 >> sh & 255) + (p1 >> sh & 255) + (p2 >> sh & 255) + (p3 >> sh & 255) + 2) >> 2) << sh;
            o[i] = v;
        }
    }
}

/* ---- the frame -------------------------------------------------------------------------------------------- */
int tk_comp_init(tk_comp *c, uint32_t *fb, uint32_t *panel)
{
    /* fb: the full-resolution compose target in RAM (the kernel lends the old UI's back buffer); panel: where
     * frames are presented (the framebuffer; write only: motion frames are scaled straight into it) */
    if (fb) { c->fb.w = TK_W; c->fb.h = TK_H; c->fb.stride = TK_W; c->fb.px = fb; }
    else if (!tk_bmp_new(&c->fb, TK_W, TK_H)) return 0;
    if (panel) { c->panel.w = TK_W; c->panel.h = TK_H; c->panel.stride = TK_W; c->panel.px = panel; }
    else if (!tk_bmp_new(&c->panel, TK_W, TK_H)) return 0;
    if (!tk_bmp_new(&c->base_half, TK_W / 2, TK_H / 2) || !tk_bmp_new(&c->bw_half, TK_W / 2, TK_H / 2) ||
        !tk_bmp_new(&c->half, TK_W / 2, TK_H / 2))
        return 0;
    return 1;
}

static int was_motion = -1;
static tk_irect pend;                       /* what rest frames changed since the half canvas last followed */
static int have_pend;
typedef struct { tk_comp *c; tk_irect r; } djob;
static void dband(void *a, int i)
{
    djob *J = a;
    int h = J->r.y1 - J->r.y0, y0 = J->r.y0 + h * i / TK_NBAND, y1 = J->r.y0 + h * (i + 1) / TK_NBAND;
    if (y1 <= y0) return;
    down2_rect(&J->c->half, &J->c->fb, (tk_irect){ J->r.x0, y0 & ~1, J->r.x1, y1 + (y1 & 1) });
}
/* the half canvas follows the rest frames (what changed, 2x2 averaged), so a motion frame only composes what moves
 * and layers that never move never need half copies: in idle time, or before the next motion frame */
void tk_comp_idle(tk_comp *c)
{
    if (!have_pend) return;
    have_pend = 0;
    djob J = { c, (tk_irect){ pend.x0 & ~1, pend.y0, pend.x1, pend.y1 } };
    if (c->par) c->par(dband, &J, TK_NBAND);
    else for (int i = 0; i < TK_NBAND; i++) dband(&J, i);
}

/* a frame's work over bands of rows: phase 0 composes every damage rect, phase 1 presents it */
typedef struct { tk_comp *c; const tgt *T; tk_irect *D; int nd, motion, phase; } job;
static void band(void *arg, int i)
{
    job *J = arg;
    tgt T = *J->T;
    T.S = &SCR[i];
    for (int q = 0; q < J->nd; q++) {
        tk_irect r = J->D[q];
        int h = r.y1 - r.y0, y0 = r.y0 + h * i / TK_NBAND, y1 = r.y0 + h * (i + 1) / TK_NBAND;
        if (y1 <= y0) continue;
        tk_irect b = { r.x0, y0, r.x1, y1 };
        if (J->phase == 0) {
            /* the topmost opaque layer at full opacity that covers most of the band: under it nothing is drawn;
             * the rest of the band (its edges, a rounded window's corner rows) gets the ground and every layer */
            int first = 0;
            float k = T.k;
            tk_irect cov = { 0, 0, 0, 0 };
            long ab = (long)(b.x1 - b.x0) * (b.y1 - b.y0);
            for (int q2 = cur.n - 1; q2 > 0; q2--) {
                const item *it = &cur.it[q2];
                if (!it->l->opaque || it->alpha < .999f || it->fade) continue;
                tk_irect cv = { (int)(it->r.x0 * k + .999f), (int)(it->r.y0 * k + .999f), (int)(it->r.x1 * k), (int)(it->r.y1 * k) };
                if (it->crad > 0) {
                    int cr = (int)(it->crad * k + .999f) + 1;
                    cv.y0 += cr;
                    cv.y1 -= cr;
                }
                tk_irect in = isect(cv, b);
                if (iempty(in) || (long)(in.x1 - in.x0) * (in.y1 - in.y0) * 2 < ab) continue;
                first = q2;
                cov = in;
                break;
            }
            tk_irect part[5];
            int np = 0;
            if (first) {
                if (cov.y0 > b.y0) part[np++] = (tk_irect){ b.x0, b.y0, b.x1, cov.y0 };
                if (cov.y1 < b.y1) part[np++] = (tk_irect){ b.x0, cov.y1, b.x1, b.y1 };
                if (cov.x0 > b.x0) part[np++] = (tk_irect){ b.x0, cov.y0, cov.x0, cov.y1 };
                if (cov.x1 < b.x1) part[np++] = (tk_irect){ cov.x1, cov.y0, b.x1, cov.y1 };
                for (int k2 = first; k2 < cur.n; k2++) draw_item(&T, &cur.it[k2], cov);
            } else
                part[np++] = b;
            for (int pi = 0; pi < np; pi++) {
                tk_irect pr = part[pi];
                if (J->motion)
                    for (int y = pr.y0; y < pr.y1; y++)
                        tk_memcpy(T.t->px + (size_t)y * T.t->stride + pr.x0, J->c->base_half.px + (size_t)y * J->c->base_half.stride + pr.x0, (size_t)(pr.x1 - pr.x0) * 4);
                else
                    up2x_out(&J->c->fb, &J->c->base_half, pr, T.S);
                for (int k2 = 0; k2 < cur.n; k2++) draw_item(&T, &cur.it[k2], pr);
            }
        } else if (J->motion) {
            /* the damage 2x into the panel, a half px of margin for the bilinear neighbours (the margin rows only
             * by the first and last band) */
            tk_irect o = { 2 * b.x0 - 2, 2 * b.y0 - (i == 0 ? 2 : 0), 2 * b.x1 + 2, 2 * b.y1 + (i == TK_NBAND - 1 ? 2 : 0) };
            up2x_fast(&J->c->panel, &J->c->half, isect(o, (tk_irect){ 0, 0, TK_W, TK_H }));
        } else {
            if (J->c->panel.px != J->c->fb.px)
                for (int y = b.y0; y < b.y1; y++)
                    tk_memcpy(J->c->panel.px + (size_t)y * J->c->panel.stride + b.x0, J->c->fb.px + (size_t)y * J->c->fb.stride + b.x0, (size_t)(b.x1 - b.x0) * 4);
            /* the half canvas follows the rest frame later (tk_comp_idle), not in this frame */
        }
    }
}
static void run_bands(tk_comp *c, job *J)
{
    if (c->par) c->par(band, J, TK_NBAND);
    else for (int i = 0; i < TK_NBAND; i++) band(J, i);
}

int tk_comp_frame(tk_comp *c, int motion, int full)
{
    if (motion) tk_comp_idle(c);             /* a finger came before the idle time did */
    static uint32_t ground_seen;
    if (c->ground_gen != ground_seen) { ground_seen = c->ground_gen; full = 1; }
    uint64_t t0 = tk_us();
    cur.n = 0;
    W = &cur;
    tk_irect screen = { 0, 0, TK_W, TK_H };
    walk(c->root, 1, 1, 0, 0, 1, screen, 0, 0, 0, 0, 0, 0);

    /* damage */
    tk_irect D[TK_MAXD];
    int nd = 0;
    if (full || was_motion < 0) {
        D[nd++] = screen;
    } else {
        static tk_irect acc[TK_MAXITEM * 2 + 2];
        int na = 0;
        for (int i = 0; i < cur.n; i++) {
            item *it = &cur.it[i];
            int found = 0;
            for (int j = 0; j < prev.n; j++)
                if (prev.it[j].l == it->l) {
                    found = 1;
                    int same = 1;
                    for (int q = 0; q < 8; q++) if (prev.it[j].key[q] != it->key[q]) same = 0;
                    tk_irect pr = prev.it[j].r, cr2 = it->r;      /* a 3-slice that only grows sideways */
                    if (pr.x0 != cr2.x0 || pr.y0 != cr2.y0 || pr.x1 != cr2.x1 || pr.y1 != cr2.y1) same = 0;
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
        for (int again = 1; again;) {           /* merging can make new overlaps */
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

    /* motion: the half canvas, its backdrop 1:1; at rest: the RAM frame, the wallpaper and the backdrop from their
     * half-resolution copies (smooth: the wallpaper's gradients, the 52 px blur) */
    tgt T = { &SCR[0], motion ? &c->half : &c->fb, 0, &c->bw_half, motion ? .5f : 1, 0 };
    T.bw_double = !motion;
    /* what is drawn from a half copy gets it made now, on this core (the bands run on several) */
    for (int i = 0; i < cur.n; i++) {
        item *it = &cur.it[i];
        if (it->l->bmp.px && it->a * T.k < .75f && it->b * T.k < .75f) half_of(it->l);
    }
    int px = 0;
    for (int q = 0; q < nd; q++) {
        tk_irect r = D[q];
        if (motion) {
            r = (tk_irect){ r.x0 / 2, r.y0 / 2, (r.x1 + 1) / 2, (r.y1 + 1) / 2 };
            D[q] = r;
            c->moved = iunion(c->moved, (tk_irect){ 2 * r.x0, 2 * r.y0, 2 * r.x1, 2 * r.y1 });
            c->have_moved = 1;
        }
        px += (r.x1 - r.x0) * (r.y1 - r.y0);
    }
    job J = { c, &T, D, nd, motion, 0 };
    J.phase = 0;
    run_bands(c, &J);
    uint64_t t1 = tk_us();
    J.phase = 1;                                /* the present (and, at rest, the half canvas following) */
    run_bands(c, &J);
    uint64_t t2 = tk_us();
    if (!motion)
        for (int q = 0; q < nd; q++) { pend = have_pend ? iunion(pend, D[q]) : D[q]; have_pend = 1; }
    c->nd = nd;
    for (int q = 0; q < nd; q++) c->dmg[q] = motion ? (tk_irect){ 2 * D[q].x0, 2 * D[q].y0, 2 * D[q].x1, 2 * D[q].y1 } : D[q];
    prev = cur;
    was_motion = motion;
    c->last_half = motion;
    c->last_px = motion ? 4 * px : px;
    c->us_comp = t1 - t0;
    c->us_present = t2 - t1;
    c->last_us = t2 - t0;
    return c->last_px;
}

void tk_flatten(tk_layer *g, tk_bmp *out, tk_a8 *mask, float k)
{
    flist.n = 0;
    W = &flist;
    tk_irect screen = { 0, 0, out->w, out->h };
    walk(g->child, k, k, 0, 0, 1, screen, 0, 0, 0, 0, 0, 0);
    W = &cur;
    tk_memset(out->px, 0, (size_t)out->stride * out->h * 4);
    if (mask && mask->v) tk_memset(mask->v, 0, (size_t)mask->w * mask->h);
    tgt T = { &SCR[0], out, mask && mask->v ? mask : 0, 0, 1, 0 };
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

/* ---- a benchmark of the present's kernels against uibench's simplest loops, on the same buffers and cores ---- */
typedef struct { tk_comp *c; int kind; } bjob;
static void bband(void *a, int i)
{
    bjob *B = a;
    tk_comp *c = B->c;
    int y0 = TK_H * i / TK_NBAND, y1 = TK_H * (i + 1) / TK_NBAND;
    switch (B->kind) {
    case 0:                                         /* the motion present: the half canvas 2x into the panel */
        up2x_fast(&c->panel, &c->half, (tk_irect){ 0, y0, TK_W, y1 });
        break;
    case 1:                                         /* the rest present: RAM to the panel, tk_memcpy per row */
        for (int y = y0; y < y1; y++) tk_memcpy(c->panel.px + (size_t)y * TK_W, c->fb.px + (size_t)y * TK_W, TK_W * 4);
        break;
    case 2: {                                       /* uibench's copy: one stream of 64-byte loads and stores */
        const uint8_t *s = (const uint8_t *)(c->fb.px + (size_t)y0 * TK_W);
        uint8_t *d = (uint8_t *)(c->panel.px + (size_t)y0 * TK_W);
        for (size_t x = 0; x < (size_t)(y1 - y0) * TK_W * 4; x += 64) vst1q_u8_x4(d + x, vld1q_u8_x4(s + x));
        break;
    }
    case 3: {                                       /* uibench's 2x: no centring, 2 loads per 4 half px */
        for (int y = y0 & ~1; y < y1; y += 2) {
            const uint8_t *r0 = (const uint8_t *)(c->half.px + (size_t)(y / 2) * (TK_W / 2));
            const uint8_t *r1 = (const uint8_t *)(c->half.px + (size_t)tk_mini(TK_H / 2 - 1, y / 2 + 1) * (TK_W / 2));
            uint32_t *o0 = c->panel.px + (size_t)y * TK_W, *o1 = o0 + TK_W;
            for (int x = 0; x + 4 < TK_W / 2; x += 4) {
                uint8x16_t a = vld1q_u8(r0 + x * 4), an = vld1q_u8(r0 + x * 4 + 4), b = vld1q_u8(r1 + x * 4), bn = vld1q_u8(r1 + x * 4 + 4);
                uint8x16_t ah = vrhaddq_u8(a, an), bh = vrhaddq_u8(b, bn);
                uint32x4x2_t t = vzipq_u32(vreinterpretq_u32_u8(a), vreinterpretq_u32_u8(ah));
                uint32x4x2_t m = vzipq_u32(vreinterpretq_u32_u8(vrhaddq_u8(a, b)), vreinterpretq_u32_u8(vrhaddq_u8(ah, bh)));
                vst1q_u32(o0 + 2 * x, t.val[0]); vst1q_u32(o0 + 2 * x + 4, t.val[1]);
                vst1q_u32(o1 + 2 * x, m.val[0]); vst1q_u32(o1 + 2 * x + 4, m.val[1]);
            }
        }
        break;
    }
    default:                                        /* the half canvas following a rest frame */
        down2_rect(&c->half, &c->fb, (tk_irect){ 0, y0, TK_W, y1 });
        break;
    }
}
/* best of 3 runs in us: 0 the motion present, 1 the rest copy, 2 uibench's copy, 3 uibench's 2x, 4 the 2x2 down */
uint64_t tk_comp_bench(tk_comp *c, int kind)
{
    uint64_t best = ~0ULL;
    for (int r = 0; r < 3; r++) {
        bjob B = { c, kind };
        uint64_t t0 = tk_us();
        if (c->par) c->par(bband, &B, TK_NBAND);
        else for (int i = 0; i < TK_NBAND; i++) bband(&B, i);
        uint64_t t = tk_us() - t0;
        if (t < best) best = t;
    }
    return best;
}
