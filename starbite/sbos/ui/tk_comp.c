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
#include "../kern/lens.h"
#include <arm_neon.h>

#define TK_MAXD 16
int getenv_dbg;
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
tk_gstyle_t tk_gstyle;
void tk_layer_glass_shape_v2(tk_layer *l, float x, float y, float w, float h, float r, int variant, uint32_t tint, float press)
{
    int i = 0;
    for (; i < l->ngsh; i++)                /* the same box baked again (a redraw without tk_layer_init): its style anew */
        if (l->gsh[i][0] == x && l->gsh[i][1] == y && l->gsh[i][2] == w && l->gsh[i][3] == h && l->gsh[i][4] == r) break;
    if (i == l->ngsh) {
        if (l->ngsh >= 24) return;
        float *g = l->gsh[l->ngsh++];
        g[0] = x; g[1] = y; g[2] = w; g[3] = h; g[4] = r;
    }
    l->gvar[i] = (uint8_t)(variant ? 1 : 0);
    l->gtint[i] = tint;
    l->gpress[i] = (uint8_t)(tk_clampf(press, 0, 1) * 255 + .5f);
}
void tk_layer_glass_shape(tk_layer *l, float x, float y, float w, float h, float r)
{
    tk_layer_glass_shape_v2(l, x, y, w, h, r, tk_gstyle.variant, tk_gstyle.tint, tk_gstyle.press);
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

/* a glass shape (its layer's bitmap px) where the item draws it (screen px, or the flatten's frame): through a
 * 3-slice's stretch (a lock card's face drawn taller than its bitmap), then the item's transform */
static float unslice(const tk_layer *l, float v, float srch)
{
    float sh = l->slice_h, s0 = l->slice_s0, s1 = l->slice_s1;
    if (v <= s0) return v;
    if (v >= srch - s1) return sh - (srch - v);
    float mid = sh - s0 - s1, smid = srch - s0 - s1;
    return s0 + (v - s0) * (smid > 0 ? mid / smid : 0);
}
static void shape_at(const item *it, const float *g, float *X, float *Y, float *W, float *H, float *r)
{
    const tk_layer *l = it->l;
    float x0 = g[0], y0 = g[1], x1 = g[0] + g[2], y1 = g[1] + g[3];
    if (l->slice == 1) { y0 = unslice(l, y0, (float)l->bmp.h); y1 = unslice(l, y1, (float)l->bmp.h); }
    else if (l->slice == 2) { x0 = unslice(l, x0, (float)l->bmp.w); x1 = unslice(l, x1, (float)l->bmp.w); }
    *X = x0 * it->a + it->tx; *Y = y0 * it->b + it->ty;
    *W = (x1 - x0) * it->a; *H = (y1 - y0) * it->b;
    *r = tk_maxf(tk_minf(g[4] * it->a, .5f * tk_minf(*W, *H)), .01f);
}

/* v9, Liquid Glass on the CPU (the fallback; comp2's lens path and gpucomp's reference do the same): each glass
 * shape of the item, its lens evaluated per target px from the shape (lens.h), over the backdrop (half resolution),
 * in R; drawn before the layer's own pixels */
static void draw_lens(const tgt *T, const item *it, tk_irect R)
{
    const tk_layer *l = it->l;
    const tk_bmp *bw = T->bw;
    if (!bw || !bw->px) return;
    float k = T->k;
    tk_irect ic = isect(R, irect_of(it->r.x0 * k, it->r.y0 * k, it->r.x1 * k, it->r.y1 * k));
    if (iempty(ic)) return;
    for (int q = 0; q < l->ngsh; q++) {
        float X, Y, W, H, r;
        shape_at(it, l->gsh[q], &X, &Y, &W, &H, &r);
        X *= k; Y *= k; W *= k; H *= k; r *= k;
        lens_par P = lens_params(W, H, r, TK_PT * k);
        lens_tone TN = lens_tone_of(l->gvar[q], l->gpress[q] * (1.0f / 255));   /* 1.0, glass V2 */
        uint32_t tint = l->gtint[q];
        float cx = X + W * .5f, cy = Y + H * .5f, ex = W * .5f - r, ey = H * .5f - r;
        tk_irect d = isect(ic, irect_of(X - 1, Y - 1, X + W + 1, Y + H + 1));
        for (int y = d.y0; y < d.y1; y++) {
            float al = it->alpha * fade_at(it, (y + .5f) / k);
            if (al <= 0) continue;
            uint32_t *row = T->t->px + (size_t)y * T->t->stride;
            for (int x = d.x0; x < d.x1; x++) {
                float gx, gy, ox = 0, oy = 0, e = .02f, rgb[3];
                float sd = lens_sd(x + .5f, y + .5f, cx, cy, ex, ey, r, &gx, &gy), cov = tk_clampf(.5f - sd, 0, 1);
                if (cov <= 0) continue;
                float sv = sd < 0 ? -sd : 0;
                if (sv < P.lb) lens_eval(&P, sv, gx, gy, &ox, &oy, &e);   /* inside the bezel (else flat: o 0, e .02) */
                /* the backdrop (screen / 2) bilinear at p + o */
                float bu = (x + .5f + ox) / k * .5f - .5f, bv = (y + .5f + oy) / k * .5f - .5f;
                float uf = tk_floorf(bu), vf = tk_floorf(bv), tu = bu - uf, tv = bv - vf;
                uint32_t p[4];
                for (int c = 0; c < 4; c++) {
                    int Xb = tk_mini(tk_maxi((int)uf + (c & 1), 0), bw->w - 1), Yb = tk_mini(tk_maxi((int)vf + (c >> 1), 0), bw->h - 1);
                    p[c] = bw->px[(size_t)Yb * bw->stride + Xb];
                }
                for (int c = 0; c < 3; c++) {
                    int sh = 16 - 8 * c;
                    float a0 = (float)(p[0] >> sh & 255), a1 = (float)(p[1] >> sh & 255), b0 = (float)(p[2] >> sh & 255), b1 = (float)(p[3] >> sh & 255);
                    float top = a0 + (a1 - a0) * tu, bot = b0 + (b1 - b0) * tu;
                    rgb[c] = (top + (bot - top) * tv) * (1.0f / 255);
                }
                lens_shade_t(rgb, e, TN);
                if (tint) lens_tint(rgb, tint);
                float m = cov * al;
                uint32_t D = row[x], o = 0;
                for (int c = 0; c < 3; c++) {
                    int sh = 16 - 8 * c;
                    int vv = (int)((float)(D >> sh & 255) + (rgb[c] * 255 - (float)(D >> sh & 255)) * m + .5f);
                    o |= (uint32_t)(vv < 0 ? 0 : vv > 255 ? 255 : vv) << sh;
                }
                int av = (int)((float)(D >> 24) + (255 - (float)(D >> 24)) * m + .5f);
                row[x] = o | (uint32_t)(av > 255 ? 255 : av) << 24;
            }
        }
    }
}
static int lens_on(const tk_layer *l, const item *it)
{
    return tk_glass_lens && l->ngsh && !(l->glass_opaque_only && it->alpha < .999f);
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
    int lens = lens_on(l, it) && !T->collect && T->bw;
    if (lens) draw_lens(T, it, R);           /* v9: the lens under the layer's pixels */
    int glass = gm && (T->collect || (T->bw && !lens)) && !(l->glass_opaque_only && it->alpha < .999f);
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
/* v10: idle time on the GPU path: every layer in the tree (hidden ones too: the Control Centre, an app's window
 * before they show) cleaned out to DRAM and its block map made, at most 5 ms a turn, so the first frame that shows
 * or moves it does not (ff0d466: first frames of a gesture 31-41 ms of CPU for 5-8 ms of GPU) */
static void gpu_flush_layer(tk_comp *c, tk_layer *l);
static int gidle_more;
static void gidle_tree(tk_comp *c, tk_layer *l, uint64_t t0)
{
    for (; l && !gidle_more; l = l->next) {
        if (tk_us() - t0 > 5000) { gidle_more = 1; return; }
        if ((l->bmp.px || l->glass.v) && l->gpu_gen != l->gen + 1u) {
            gpu_flush_layer(c, l);
            c->gpu_idle_flushed++;
        }
        gidle_tree(c, l->child, t0);
    }
}
int tk_comp_idle_pending(tk_comp *c) { return c->gpu && gidle_more; }
void tk_comp_idle(tk_comp *c)
{
    if (c->gpu) {                               /* P4: the GPU draws whole frames: no half canvas to keep */
        gidle_more = 0;
        if (c->gpu_clean) gidle_tree(c, c->root, tk_us());
        return;
    }
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
        } else if (J->phase == 2) {
            /* two buffers: what the last presented frame changed (outside this frame's damage), as it showed then:
             * from the half canvas if it was a motion frame, else from the RAM frame */
            if (J->motion) up2x_fast(&J->c->panel, &J->c->half, b);
            else
                for (int y = b.y0; y < b.y1; y++)
                    tk_memcpy(J->c->panel.px + (size_t)y * J->c->panel.stride + b.x0, J->c->fb.px + (size_t)y * J->c->fb.stride + b.x0, (size_t)(b.x1 - b.x0) * 4);
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

/* P4: the frame on the GPU (c->gpu): the wallpaper and every item as quads, the whole screen into the free scanout
 * buffer at full resolution (no half canvas, no catch-up: each GPU frame is whole). The px drawn, or -1 (the CPU draws
 * this frame: too many items, or the GPU refused) */
static tk_gpu_item gq[TK_GPU_MAX];
/* v4: the layer's block map (32 px blocks: empty, partial, opaque), as the CPU's row_over skips and copies 8 px
 * blocks; not for small bitmaps, nor for content that changes frame after frame. v4.1: block rows in parallel over
 * the four cores, 2 px per 64-bit read, and the cache clean of those rows in the same pass (the phone's first frame
 * with a new full-screen layer spent ~30 ms scanning and cleaning on one core) */
typedef struct { tk_comp *c; tk_layer *l; int map; uint32_t ow; } occ_job;
typedef uint64_t __attribute__((aligned(4), may_alias)) u64a;
static void occ_rows(void *a, int r0, int r1)
{
    occ_job *J = a;
    tk_layer *l = J->l;
    const uint64_t AM = 0xff000000ff000000ull;
    for (int by = r0; by < r1; by++) {
        int y0 = by * 32, y1 = y0 + 32 < l->bmp.h ? y0 + 32 : l->bmp.h;
        if (J->map) {
            for (uint32_t bx = 0; bx < J->ow; bx++) {
                int x0 = (int)bx * 32, x1 = x0 + 32 < l->bmp.w ? x0 + 32 : l->bmp.w;
                uint64_t any = 0, all = AM;
                for (int y = y0; y < y1; y++) {
                    const uint32_t *r = l->bmp.px + (size_t)y * l->bmp.stride;
                    int x = x0;
                    if (x & 1) { any |= (uint64_t)r[x] << 32; all &= (uint64_t)r[x] << 32 | 0xff000000u; x++; }
                    for (; x + 2 <= x1; x += 2) {
                        uint64_t v = *(const u64a *)(r + x);        /* one load (tk_memcpy was a call per 2 px) */
                        any |= v;
                        all &= v;
                    }
                    if (x < x1) { any |= (uint64_t)r[x]; all &= (uint64_t)r[x] | 0xff00000000000000ull; }
                }
                any &= AM;
                all &= AM;
                J->l->gpu_occ[(uint32_t)by * J->ow + bx] = (uint8_t)(!any ? 0 : all == AM ? 2 : 1);
            }
        }
        if (J->c->gpu_clean && y1 > y0)
            J->c->gpu_clean(J->c->gpu_ctx, l->bmp.px + (size_t)y0 * l->bmp.stride, (uint64_t)(y1 - y0) * l->bmp.stride * 4u);
    }
}
/* a bitmap's cache clean over the cores, by rows */
typedef struct { tk_comp *c; const tk_bmp *b; } clean_job;
static void clean_rows(void *a, int y0, int y1)
{
    clean_job *J = a;
    if (y1 > y0)
        J->c->gpu_clean(J->c->gpu_ctx, J->b->px + (size_t)y0 * J->b->stride, (uint64_t)(y1 - y0) * J->b->stride * 4u);
}
static void gpu_flush_layer(tk_comp *c, tk_layer *l)
{
    if (l->gpu_gen == l->gen + 1u || !c->gpu_clean) return;
    uint32_t fr = c->gpu_frames;
    int busy = l->gpu_occ_seen && fr - l->gpu_occ_frame <= 2u;
    l->gpu_occ_frame = fr;
    l->gpu_occ_seen = 1;
    l->gpu_occ_ok = 0;
    occ_job J = { c, l, 0, 0 };
    if (l->bmp.px) {
        uint32_t ow = (uint32_t)(l->bmp.w + 31) / 32u, oh = (uint32_t)(l->bmp.h + 31) / 32u;
        if (!busy && l->bmp.w >= 64 && l->bmp.h >= 64) {
            if (ow * oh > l->gpu_occ_cap) {
                tk_free(l->gpu_occ);
                l->gpu_occ = tk_alloc(ow * oh);
                l->gpu_occ_cap = l->gpu_occ ? ow * oh : 0;
            }
            J.map = l->gpu_occ != 0;
        }
        J.ow = ow;
        if (oh >= 8) tk_par_rows(0, (int)oh, occ_rows, &J);
        else occ_rows(&J, 0, (int)oh);
        if (J.map) {
            l->gpu_occ_w = ow;
            l->gpu_occ_h = oh;
            l->gpu_occ_ok = 1;
        }
    }
    if (l->glass.v) c->gpu_clean(c->gpu_ctx, l->glass.v, (uint64_t)l->glass.w * (uint64_t)l->glass.h);
    l->gpu_gen = l->gen + 1u;
}
static int gpu_frame(tk_comp *c, const tk_irect *D, int nd)
{
    int n = 0;
    if (c->ground_gen + 1u != c->gpu_ground && c->gpu_clean) {
        clean_job J1 = { c, &c->base_half }, J2 = { c, &c->bw_half };
        tk_par_rows(0, c->base_half.h, clean_rows, &J1);
        tk_par_rows(0, c->bw_half.h, clean_rows, &J2);
        c->gpu_ground = c->ground_gen + 1u;
    }
    /* the ground: the wallpaper from its half-resolution copy, smooth (bilinear 2x), as the CPU's rest frames */
    tk_memset(&gq[n], 0, sizeof gq[n]);
    gq[n].src = c->base_half.px;
    gq[n].stride = (uint32_t)c->base_half.stride;
    gq[n].w = (uint32_t)c->base_half.w;
    gq[n].h = (uint32_t)c->base_half.h;
    gq[n].flags = TK_GPU_BILINEAR;
    gq[n].alpha = 1;
    gq[n].isx = gq[n].isy = .5f;
    gq[n].cx1 = TK_W;
    gq[n].cy1 = TK_H;
    gq[n].gen = c->ground_gen;
    n++;
    for (int i = 0; i < cur.n; i++) {
        const item *it = &cur.it[i];
        tk_layer *l = it->l;
        int lens = !l->solid && c->bw_half.px && lens_on(l, it), below = n;
        if (lens)                               /* v9: Liquid Glass: a quad per glass shape, under the layer's */
            for (int k = 0; k < l->ngsh; k++) {
                if (n >= TK_GPU_MAX) return -1;
                float X, Y, W, H, r;
                shape_at(it, l->gsh[k], &X, &Y, &W, &H, &r);
                tk_gpu_item *e = &gq[n++];
                tk_memset(e, 0, sizeof *e);
                lens_par P = lens_params(W, H, r, TK_PT);
                e->flags = TK_GPU_SOLID | TK_GPU_LENS;
                e->alpha = it->alpha;
                e->dx0 = tk_floorf(X) - 1; e->dy0 = tk_floorf(Y) - 1;
                e->isx = e->isy = 1;
                e->w = (uint32_t)(W + 3.999f); e->h = (uint32_t)(H + 3.999f);
                e->cx0 = tk_maxf((float)it->clip.x0, X - 1); e->cy0 = tk_maxf((float)it->clip.y0, Y - 1);
                e->cx1 = tk_minf((float)it->clip.x1, X + W + 1); e->cy1 = tk_minf((float)it->clip.y1, Y + H + 1);
                if (e->cx1 <= e->cx0 || e->cy1 <= e->cy0) { n--; continue; }
                e->radius = r;
                if (it->fade) {
                    e->flags |= TK_GPU_FADE;
                    e->ft0 = it->ft0; e->ft1 = it->ft1; e->fb0 = it->fb0; e->fb1 = it->fb1;
                }
                e->bd = c->bw_half.px;
                e->bstride = (uint32_t)c->bw_half.stride;
                e->bk = .5f;
                e->bw = (uint32_t)c->bw_half.w;
                e->bh = (uint32_t)c->bw_half.h;
                e->lx = X; e->ly = Y; e->lw = W; e->lh = H;
                e->lb = P.lb; e->lt = P.lt; e->lc = P.lc; e->lr = P.lr;
                e->lsat = 1;                    /* the wallpaper's blur is saturated already */
                e->live = below > 1 ? (uint32_t)below : 0;   /* v10: what lies under it (more than the ground) */
                e->gen = c->ground_gen;
                {                               /* 1.0, glass V2: the shape's tone (variant, press) and tint */
                    lens_tone T = lens_tone_of(l->gvar[k], l->gpress[k] * (1.0f / 255));
                    e->lk = T.k; e->la0 = T.a0; e->la1 = T.a1;
                    e->solid = lens_tint_pm(l->gtint[k]);
                }
            }
        if (n >= TK_GPU_MAX) return -1;
        tk_gpu_item *q = &gq[n++];
        tk_memset(q, 0, sizeof *q);
        q->alpha = it->alpha;
        /* at 1:1, whole pixels (one load a pixel, as sharp as at rest; a sub-pixel step of a move is not visible) */
        int one = tk_fabsf(it->a - 1) <= 1e-4f && tk_fabsf(it->b - 1) <= 1e-4f;
        q->dx0 = one ? tk_floorf(it->tx + .5f) : it->tx;
        q->dy0 = one ? tk_floorf(it->ty + .5f) : it->ty;
        q->isx = 1.0f / it->a;
        q->isy = 1.0f / it->b;
        q->cx0 = (float)it->clip.x0; q->cy0 = (float)it->clip.y0;
        q->cx1 = (float)it->clip.x1; q->cy1 = (float)it->clip.y1;
        q->radius = it->crad > 0 ? it->crad : 0;
        if (it->fade) {
            q->flags |= TK_GPU_FADE;
            q->ft0 = it->ft0; q->ft1 = it->ft1; q->fb0 = it->fb0; q->fb1 = it->fb1;
        }
        if (l->solid) {
            q->flags |= TK_GPU_SOLID;
            q->solid = l->solid;
            q->w = (uint32_t)(l->solid_w + .5f);
            q->h = (uint32_t)(l->solid_h + .5f);
            continue;
        }
        gpu_flush_layer(c, l);
        q->gen = l->gen;
        if (l->gpu_occ_ok) {
            q->occ = l->gpu_occ;
            q->occ_w = l->gpu_occ_w;
            q->occ_h = l->gpu_occ_h;
        }
        if (l->opaque) q->flags |= TK_GPU_OPAQUE;
        q->src = l->bmp.px;
        q->stride = (uint32_t)l->bmp.stride;
        q->w = (uint32_t)l->bmp.w;
        q->h = (uint32_t)l->bmp.h;
        if (!one) q->flags |= TK_GPU_BILINEAR;
        if (l->slice) {
            q->flags |= l->slice == 1 ? TK_GPU_SLICE_V : TK_GPU_SLICE_H;
            q->len = l->slice_h; q->s0 = l->slice_s0; q->s1 = l->slice_s1;
        }
        if (!lens && l->glass.v && c->bw_half.px && !(l->glass_opaque_only && it->alpha < .999f)) {
            q->flags |= TK_GPU_GLASS;
            q->gen += c->ground_gen << 20;      /* the backdrop is the ground's */
            q->mask = l->glass.v;
            q->mstride = (uint32_t)l->glass.w;
            q->bd = c->bw_half.px;
            q->bstride = (uint32_t)c->bw_half.stride;
            q->bk = .5f;
            q->bw = (uint32_t)c->bw_half.w;
            q->bh = (uint32_t)c->bw_half.h;
        }
    }
    uint64_t tw = tk_us();
    int age = 0;
    uint32_t *dst = c->acquire(c->acq_ctx, &age);
    c->us_wait = tk_us() - tw;
    /* v3, damage: the buffer is age frames behind; this frame's rects plus what those frames changed (P2t's
     * history), else (unknown age, too many rects) everything */
    static tk_irect gr[64];
    int ng = 0, px = 0;
    if (age >= 1 && age <= TK_HIST && (uint32_t)age <= c->fno) {
        for (int q = 0; q < nd && ng >= 0; q++) gr[ng++] = D[q];
        for (int k = age; k >= 1 && ng >= 0; k--) {
            int s2 = (int)((c->fno - (uint32_t)k) % TK_HIST);
            for (int i = 0; i < c->hn[s2]; i++) {
                if (ng >= 64) { ng = -1; break; }
                gr[ng++] = c->hd[s2][i];
            }
        }
    } else
        ng = -1;
    if (ng < 0) ng = 0;
    for (int i = 0; i < ng; i++) px += (gr[i].x1 - gr[i].x0) * (gr[i].y1 - gr[i].y0);
    if (!ng || px > TK_W * TK_H) px = TK_W * TK_H;
    uint64_t us = 0;
    if (!c->gpu(c->gpu_ctx, dst, gq, n, gr, ng, &us)) return -1;
    c->panel.px = dst;
    c->us_gpu = us;
    c->gpu_items = (uint32_t)n;
    c->gpu_frames++;
    /* P2t's history: what this frame changed (a later frame into an older buffer redraws it too) */
    int sl = (int)(c->fno % TK_HIST);
    for (int q = 0; q < nd; q++) c->hd[sl][q] = D[q];
    c->hn[sl] = nd;
    c->hh[sl] = 0;
    c->fno++;
    return px;
}

/* the frame on the panel now, in RAM: on the GPU path the RAM frame is stale (the GPU draws into the scanout
 * buffers, which the CPU reads uncached: ec2936a read 17 MB of one for the CC's scrim, ~600 ms, and a self-test
 * film's Power press was skipped in that stall), so the CPU draws the last walk's items into it once, whole, at rest
 * resolution, without presenting (about a CPU frame: 5-6 ms on four cores) */
static uint32_t ram_fresh;                  /* gpu_frames when the RAM frame was last drawn for tk_comp_shown */
const tk_bmp *tk_comp_shown(tk_comp *c)
{
    if (!c->gpu_stale || ram_fresh == c->gpu_frames + 1u) return &c->fb;
    tgt T = { &SCR[0], &c->fb, 0, &c->bw_half, 1, 0 };
    T.bw_double = 1;
    tk_irect D[1] = { { 0, 0, TK_W, TK_H } };
    job J = { c, &T, D, 1, 0, 0 };
    run_bands(c, &J);
    ram_fresh = c->gpu_frames + 1u;
    return &c->fb;
}

int tk_comp_frame(tk_comp *c, int motion, int full)
{
    uint64_t ti = tk_us();
    if (motion) tk_comp_idle(c);             /* a finger came before the idle time did */
    c->us_idle = tk_us() - ti;
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
    /* P4: after GPU frames the CPU's own canvases are stale: its next frame is whole */
    if (full || was_motion < 0 || (c->gpu_stale && !(c->gpu && c->acquire))) {
        D[nd++] = screen;
        c->gpu_stale = 0;
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
    /* P4: the GPU draws the whole frame (sharp in motion too); if it cannot, the CPU draws this one, everything */
    /* v7: a big motion (more than a sixth of the screen changing) is drawn at half resolution by the GPU to its end
     * (as the CPU's half canvas); a half frame covers everything, and the first frame at rest after one redraws
     * everything sharp */
    static int gpu_big, gpu_last_half;
    if (!motion) gpu_big = 0;
    if (c->gpu && c->acquire && (nd || (!motion && gpu_last_half))) {
        uint64_t tg = tk_us();
        int half = 0;
        if (motion) {
            long a = 0;
            for (int q = 0; q < nd; q++) a += (long)(D[q].x1 - D[q].x0) * (D[q].y1 - D[q].y0);
            if (gpu_big || a > (long)TK_W * TK_H / 6) gpu_big = half = 1;
        }
        if (half || gpu_last_half) { D[0] = screen; nd = 1; }
        c->gpu_half = half;
        int gpx = gpu_frame(c, D, nd);
        c->gpu_half = 0;
        if (gpx > 0) {
            gpu_last_half = half;
            if (half) c->gpu_halves++;
            c->gpu_stale = 1;           /* the RAM frame and the half canvas did not see this frame */
            c->nd = nd;
            for (int q = 0; q < nd; q++) c->dmg[q] = D[q];
            c->have_moved = 0;
            prev = cur;
            was_motion = 0;
            c->last_half = 0;
            c->last_px = gpx;
            c->us_catch = 0;
            c->us_comp = tk_us() - tg - c->us_wait;
            c->us_present = 0;
            c->last_us = tk_us() - t0 - c->us_wait;
            return gpx;
        }
        c->gpu_fallbacks++;
        D[0] = screen;                  /* the CPU takes over: everything (its canvases are stale, every buffer) */
        nd = 1;
        c->gpu_stale = 0;
    }
    /* P2s: a small motion (a toast, a hold ring, a chevron, a key, a toggle) at full resolution, sharp as at rest,
     * while the motion so far has been small (a large one stays at half resolution to its end) */
    if (motion && was_motion != 1) {
        long a = 0;
        for (int q = 0; q < nd; q++) a += (long)(D[q].x1 - D[q].x0) * (D[q].y1 - D[q].y0);
        if (a <= (long)TK_W * TK_H / 6) { motion = 0; c->sharp_motion++; }
    }
    if (!motion) { c->have_moved = 0; c->moved = (tk_irect){ 0, 0, 0, 0 }; }

    /* motion: the half canvas, its backdrop 1:1; at rest: the RAM frame, the wallpaper and the backdrop from their
     * half-resolution copies (smooth: the wallpaper's gradients, the 52 px blur) */
    tgt T = { &SCR[0], motion ? &c->half : &c->fb, 0, &c->bw_half, motion ? .5f : 1, 0 };
    T.bw_double = !motion;
    /* what is drawn from a half copy gets it made now, on this core (the bands run on several) */
    uint64_t th = tk_us();
    for (int i = 0; i < cur.n; i++) {
        item *it = &cur.it[i];
        if (it->l->bmp.px && it->a * T.k < .75f && it->b * T.k < .75f) {
            if (!it->l->half.px && getenv_dbg) tk_log(it->l->name);
            half_of(it->l);
        }
    }
    c->us_halfs = tk_us() - th;
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
    c->us_wait = c->us_catch = 0;
    if (c->acquire && nd) {
        /* the buffer the display neither shows nor has queued (the platform's; it may wait) */
        int age = 1;
        c->panel.px = c->acquire(c->acq_ctx, &age);
        uint64_t tw = tk_us();
        c->us_wait = tw - t1;
        t1 = tw;
        /* it is age frames behind: their rects come over first, oldest first, each from where that frame drew
         * (the half canvas for a motion frame, else the RAM frame), minus what this frame presents anyway (rect
         * differences; past 64 pieces a piece stays whole: drawn twice, still right) */
        int known = age >= 1 && age <= TK_HIST && (uint32_t)age <= c->fno;
        for (int k = known ? age : 1; k >= 1; k--) {
            tk_irect E[64];
            int ne = 0, src_half;
            if (known) {
                int sl = (int)((c->fno - (uint32_t)k) % TK_HIST);
                for (int i = 0; i < c->hn[sl]; i++) E[ne++] = c->hd[sl][i];
                src_half = c->hh[sl];
            } else {
                E[ne++] = screen;                       /* unknown: everything, from what is current */
                src_half = was_motion == 1;
            }
            for (int q = 0; q < nd && ne; q++) {
                tk_irect d = motion ? (tk_irect){ 2 * D[q].x0, 2 * D[q].y0, 2 * D[q].x1, 2 * D[q].y1 } : D[q];
                tk_irect N[64];
                int nn = 0;
                for (int i = 0; i < ne; i++) {
                    tk_irect e = E[i], in = isect(e, d);
                    if (iempty(in)) { if (nn < 64) N[nn++] = e; continue; }
                    tk_irect pc[4];
                    int np = 0;
                    if (e.y0 < in.y0) pc[np++] = (tk_irect){ e.x0, e.y0, e.x1, in.y0 };
                    if (in.y1 < e.y1) pc[np++] = (tk_irect){ e.x0, in.y1, e.x1, e.y1 };
                    if (e.x0 < in.x0) pc[np++] = (tk_irect){ e.x0, in.y0, in.x0, in.y1 };
                    if (in.x1 < e.x1) pc[np++] = (tk_irect){ in.x1, in.y0, e.x1, in.y1 };
                    if (nn + np > 64) { if (nn < 64) N[nn++] = e; continue; }
                    for (int k2 = 0; k2 < np; k2++) N[nn++] = pc[k2];
                }
                tk_memcpy(E, N, sizeof(tk_irect) * (size_t)nn);
                ne = nn;
            }
            if (ne) {
                job J2 = { c, &T, E, ne, src_half, 2 };
                run_bands(c, &J2);
            }
        }
        uint64_t tc = tk_us();
        c->us_catch = tc - t1;
        t1 = tc;
    }
    J.phase = 1;                                /* the present (and, at rest, the half canvas following) */
    run_bands(c, &J);
    uint64_t t2 = tk_us();
    if (c->acquire && nd) {
        /* what this frame wrote into its buffer: the other buffers lack exactly that (motion: the 2x margin) */
        int sl = (int)(c->fno % TK_HIST);
        for (int q = 0; q < nd; q++)
            c->hd[sl][q] = motion ? isect((tk_irect){ 2 * D[q].x0 - 2, 2 * D[q].y0 - 2, 2 * D[q].x1 + 2, 2 * D[q].y1 + 2 }, screen) : D[q];
        c->hn[sl] = nd;
        c->hh[sl] = motion;
        c->fno++;
    }
    if (!motion)
        for (int q = 0; q < nd; q++) { pend = have_pend ? iunion(pend, D[q]) : D[q]; have_pend = 1; }
    c->nd = nd;
    for (int q = 0; q < nd; q++) c->dmg[q] = motion ? (tk_irect){ 2 * D[q].x0, 2 * D[q].y0, 2 * D[q].x1, 2 * D[q].y1 } : D[q];
    prev = cur;
    was_motion = motion;
    c->last_half = motion;
    c->last_px = motion ? 4 * px : px;
    c->us_comp = t1 - t0 - c->us_wait - c->us_catch;
    c->us_present = t2 - t1 + c->us_catch;
    c->last_us = t2 - t0 - c->us_wait;         /* the work; the wait for the display is counted apart */
    return c->last_px;
}

static float fsh[24][5], fk = 1;
static uint32_t ftint[24];
static uint8_t fvar[24], fpress[24];   /* V2: the flattened shapes' style */
static lens_par fpar[24];
static int nfsh, fit[24];                   /* fit: the shape's item in flist (its clip, fade and opacity) */
void tk_flatten_shapes(tk_layer *dst)
{
    dst->ngsh = 0;
    for (int i = 0; i < nfsh; i++)
        tk_layer_glass_shape_v2(dst, fsh[i][0], fsh[i][1], fsh[i][2], fsh[i][3], fsh[i][4], fvar[i], ftint[i], fpress[i] / 255.0f);
}
uint32_t tk_flat_lens(const tk_comp *c, int x, int y, uint32_t fb)
{
    const tk_bmp *bw = &c->bw_half;
    if (!tk_glass_lens || !bw->px) return fb;
    float sx = (x + .5f) / fk, sy = (y + .5f) / fk;             /* screen px */
    for (int q = 0; q < nfsh; q++) {
        float X = fsh[q][0] / fk, Y = fsh[q][1] / fk, W = fsh[q][2] / fk, H = fsh[q][3] / fk, r = fsh[q][4] / fk;
        if (sx < X - 1 || sy < Y - 1 || sx > X + W + 1 || sy > Y + H + 1) continue;
        float gx, gy, ox = 0, oy = 0, e = .02f, rgb[3];
        float sd = lens_sd(sx, sy, X + W * .5f, Y + H * .5f, W * .5f - r, H * .5f - r, r, &gx, &gy);
        if (sd >= .5f) continue;
        float sv = sd < 0 ? -sd : 0;
        if (sv < fpar[q].lb) lens_eval(&fpar[q], sv, gx, gy, &ox, &oy, &e);
        float bu = (sx + ox) * .5f - .5f, bv = (sy + oy) * .5f - .5f, uf = tk_floorf(bu), vf = tk_floorf(bv);
        float tu = bu - uf, tv = bv - vf;
        uint32_t p[4];
        for (int k2 = 0; k2 < 4; k2++) {
            int Xb = tk_mini(tk_maxi((int)uf + (k2 & 1), 0), bw->w - 1), Yb = tk_mini(tk_maxi((int)vf + (k2 >> 1), 0), bw->h - 1);
            p[k2] = bw->px[(size_t)Yb * bw->stride + Xb];
        }
        for (int ch = 0; ch < 3; ch++) {
            int sh = 16 - 8 * ch;
            float a0 = (float)(p[0] >> sh & 255), a1 = (float)(p[1] >> sh & 255), b0 = (float)(p[2] >> sh & 255), b1 = (float)(p[3] >> sh & 255);
            float top = a0 + (a1 - a0) * tu, bot = b0 + (b1 - b0) * tu;
            rgb[ch] = (top + (bot - top) * tv) * (1.0f / 255);
        }
        lens_shade_t(rgb, e, lens_tone_of(fvar[q], fpress[q] * (1.0f / 255)));   /* 1.0, glass V2 */
        if (ftint[q]) lens_tint(rgb, ftint[q]);
        uint32_t o = 0xff000000u;
        for (int ch = 0; ch < 3; ch++) {
            int v = (int)(rgb[ch] * 255 + .5f);
            o |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << (16 - 8 * ch);
        }
        return o;
    }
    return fb;
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
    nfsh = 0;
    fk = k;
    for (int i = 0; i < flist.n; i++) {
        const item *it = &flist.it[i];
        draw_item(&T, it, screen);
        /* its glass shapes into the flat frame (the item's transform; a layer whose mask was freed after its bake
         * keeps its shapes) */
        if (mask && it->l->ngsh && !(it->l->glass_opaque_only && it->alpha < .999f))
            for (int q = 0; q < it->l->ngsh && nfsh < 24; q++, nfsh++)
            {
                fit[nfsh] = i;
                shape_at(it, it->l->gsh[q], &fsh[nfsh][0], &fsh[nfsh][1], &fsh[nfsh][2], &fsh[nfsh][3], &fsh[nfsh][4]);
                fvar[nfsh] = it->l->gvar[q]; ftint[nfsh] = it->l->gtint[q]; fpress[nfsh] = it->l->gpress[q];
                fpar[nfsh] = lens_params(fsh[nfsh][2] / k, fsh[nfsh][3] / k, fsh[nfsh][4] / k, TK_PT);   /* screen px */
            }
    }
    /* the shapes' coverage into the mask too (a layer that freed its mask after the bake drew none) */
    if (mask && mask->v)
        for (int q = 0; q < nfsh; q++) {
            float x = fsh[q][0], y = fsh[q][1], w = fsh[q][2], h = fsh[q][3];
            tk_r4 R = tk_radii(w, h, fsh[q][4]);
            int X0 = tk_maxi(0, (int)tk_floorf(x) - 1), X1 = tk_mini(mask->w, (int)(x + w) + 2);
            int Y0 = tk_maxi(0, (int)tk_floorf(y) - 1), Y1 = tk_mini(mask->h, (int)(y + h) + 2);
            const item *it = &flist.it[fit[q]];
            X0 = tk_maxi(X0, it->clip.x0); X1 = tk_mini(X1, it->clip.x1);
            Y0 = tk_maxi(Y0, it->clip.y0); Y1 = tk_mini(Y1, it->clip.y1);
            for (int j = Y0; j < Y1; j++) {
                float al = it->alpha * fade_at(it, j + .5f);
                if (al <= 0) continue;
                for (int i = X0; i < X1; i++) {
                    float cov = al * tk_clampf(.5f - tk_sd_rrect4(i + .5f, j + .5f, x, y, w, h, &R), 0, 1);
                    uint8_t v = (uint8_t)(cov * 255 + .5f), *m = &mask->v[(size_t)j * mask->w + i];
                    if (v > *m) *m = v;
                }
            }
        }
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
