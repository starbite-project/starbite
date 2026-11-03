/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7: the Liquid Glass renderer (glass.h), the driver. docs/MENU7-PORT.md 4.1.
 *
 * g7_frame: compare the frame's lists with the previous frame's (stable keys, a hash of each
 * shape's and item's content, their bounds), collect the dirty rects (a shape's box + its 110 x
 * 150 px shadow cull, an item's drawn bounds), merge them into at most 8 horizontal bands (or
 * the whole panel past 60 %), then compose each band in G7_CANVAS:
 *
 *   base   BASE x dim (or, frozen, mix(SHARP, SOFT, back)), the base shapes (GLASS_FS,
 *          uOpaque = 1), the glass wordmark, dither; then the base items
 *   over   backdrop (0, .004, .014) x back x .45, the over shapes (uOpaque = 0, soft 0), dither,
 *          premultiplied over the base; then the over items
 *   edge   x 0..35 and 1405..1440: black 60 % -> 0
 *
 * and copy the bands to the framebuffer. Behind an overlay (back > .002) the base layer is
 * frozen once (G7_SHARP) with its soft version (base glass at uSoft = 1 and its content at half
 * size, zoomed .965 about (720, 1332), blurred 26 px, x .75): deliberate difference D4.
 */
#include "glass_int.h"

#define G7_FB ((uint32_t *)(0xcc000000UL + G7_HOSTOFF))

float g_row_tmp[4 * GW];
static float RA5[3 * GW], RB[3 * GW], ACC[3 * GW], AL[GW];

static struct g7_stats ST;
static int inited, baked, first = 1, frozen, base_ok;
static struct g7_wall cur_wall;
static float cur_dim = -1.f, cur_back = -1.f, cur_lx, cur_ly;
static struct g7_word cur_word;
static struct gs_ctx CBASE, COVER;
static const struct g7_layer *LB, *LO;
static float back_now;

static inline uint64_t us_since(uint64_t t0)
{
    uint64_t f = hw_freq();
    return f ? (hw_ticks() - t0) * 1000000ULL / f : 0;
}

static const struct g7_wall WALL_FINAL = { G7_WALL_FINAL_SX, G7_WALL_FINAL_SY, G7_WALL_FINAL_SI, G7_WALL_FINAL_SR,
                                           G7_WALL_FINAL_SW, G7_WALL_FINAL_RR, G7_WALL_FINAL_RF, G7_WALL_FINAL_AMB };

/* ---- dirty rects ---- */

#define MAXR 160
#define MAXBAND 8
static int16_t RR[MAXR][4];
static int nrr, all_dirty;

static void merge_bands(void);
static void dirty(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GW) x1 = GW;
    if (y1 > GH) y1 = GH;
    if (x0 >= x1 || y0 >= y1 || all_dirty)
        return;
    if (nrr == MAXR)
        merge_bands();
    RR[nrr][0] = (int16_t)x0;
    RR[nrr][1] = (int16_t)y0;
    RR[nrr][2] = (int16_t)x1;
    RR[nrr][3] = (int16_t)y1;
    nrr++;
}
static void dirty_b(const int16_t b[4]) { dirty(b[0], b[1], b[2], b[3]); }

/* sort by y0, unite rects whose y ranges overlap (or nearly), then merge the closest bands */
static void merge_bands(void)
{
    for (int i = 1; i < nrr; i++) {
        int16_t t[4] = { RR[i][0], RR[i][1], RR[i][2], RR[i][3] };
        int j = i - 1;
        while (j >= 0 && RR[j][1] > t[1]) {
            for (int k = 0; k < 4; k++)
                RR[j + 1][k] = RR[j][k];
            j--;
        }
        for (int k = 0; k < 4; k++)
            RR[j + 1][k] = t[k];
    }
    int m = 0;
    for (int i = 0; i < nrr; i++) {
        if (m && RR[i][1] <= RR[m - 1][3] + 16) {
            if (RR[i][0] < RR[m - 1][0]) RR[m - 1][0] = RR[i][0];
            if (RR[i][2] > RR[m - 1][2]) RR[m - 1][2] = RR[i][2];
            if (RR[i][3] > RR[m - 1][3]) RR[m - 1][3] = RR[i][3];
        } else {
            for (int k = 0; k < 4; k++)
                RR[m][k] = RR[i][k];
            m++;
        }
    }
    while (m > MAXBAND) {
        int best = 0, gap = 1 << 30;
        for (int i = 0; i + 1 < m; i++)
            if (RR[i + 1][1] - RR[i][3] < gap) {
                gap = RR[i + 1][1] - RR[i][3];
                best = i;
            }
        if (RR[best + 1][0] < RR[best][0]) RR[best][0] = RR[best + 1][0];
        if (RR[best + 1][2] > RR[best][2]) RR[best][2] = RR[best + 1][2];
        if (RR[best + 1][3] > RR[best][3]) RR[best][3] = RR[best + 1][3];
        for (int i = best + 1; i + 1 < m; i++)
            for (int k = 0; k < 4; k++)
                RR[i][k] = RR[i + 1][k];
        m--;
    }
    nrr = m;
}

/* ---- list signatures ---- */

struct pent {
    uint32_t key, hash;
    int16_t b[4];
};
static struct pent PS[2][G7_MAX_SHAPES], PI[2][G7_MAX_ITEMS];       /* previous frame */
static int nps[2], npi[2];
static struct pent CS[G7_MAX_SHAPES], CI[G7_MAX_ITEMS];             /* this frame */
static int16_t BND[2][G7_MAX_ITEMS][4];                             /* item bounds, for drawing */

static inline uint32_t hmix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 16777619U;
    h ^= h >> 15;
    return h;
}
static inline uint32_t hf(uint32_t h, float f)
{
    union { float f; uint32_t u; } x;
    x.f = f;
    return hmix(h, x.u);
}

static uint32_t shape_hash(const struct g7_shape *s)
{
    uint32_t h = 2166136261U;
    h = hmix(h, s->flags);
    h = hf(h, s->x); h = hf(h, s->y); h = hf(h, s->w); h = hf(h, s->h);
    h = hf(h, s->r); h = hf(h, s->a); h = hf(h, s->s);
    for (int i = 0; i < 4; i++)
        h = hf(h, s->tint[i]);
    return h;
}
static uint32_t item_hash(const struct g7_layer *l, const struct g7_item *it)
{
    uint32_t h = 2166136261U;
    h = hmix(h, (uint32_t)it->kind | (uint32_t)it->flags << 8);
    h = hf(h, it->a);
    h = hmix(h, it->color);
    if (it->clip && it->clip < l->nclip) {
        const struct g7_clip *c = &l->clip[it->clip];
        h = hf(h, c->x0); h = hf(h, c->y0); h = hf(h, c->x1); h = hf(h, c->y1); h = hf(h, c->r);
    }
    switch (it->kind) {
    case G7_TEXT: {
        const struct g7_text *t = &it->u.text;
        h = hmix(h, (uint32_t)t->n | (uint32_t)t->font << 16);
        h = hf(h, t->x); h = hf(h, t->y); h = hf(h, t->track); h = hf(h, t->scale); h = hf(h, t->max_w);
        if (it->flags & G7_I_GRAD) {
            h = hmix(h, t->color2);
            h = hf(h, t->gy0); h = hf(h, t->gy1); h = hf(h, t->gstop);
        }
        for (unsigned i = 0; t->s && i < t->n; i++)
            h = hmix(h, (uint8_t)t->s[i]);
        break;
    }
    case G7_IMAGE: {
        const struct g7_image *m = &it->u.image;
        h = hmix(h, m->id);
        h = hf(h, m->x); h = hf(h, m->y); h = hf(h, m->w); h = hf(h, m->h); h = hf(h, m->sat);
        break;
    }
    case G7_TILE: {
        const struct g7_tile *t = &it->u.tile;
        h = hmix(h, (uint32_t)t->tile | (uint32_t)t->size << 16);
        h = hf(h, t->x); h = hf(h, t->y); h = hf(h, t->w); h = hf(h, t->sat);
        break;
    }
    case G7_RECT: {
        const struct g7_rect *r = &it->u.rect;
        h = hf(h, r->x); h = hf(h, r->y); h = hf(h, r->w); h = hf(h, r->h); h = hf(h, r->r); h = hf(h, r->ring);
        break;
    }
    case G7_SHADOW: {
        const struct g7_boxshadow *s = &it->u.shadow;
        h = hf(h, s->x); h = hf(h, s->y); h = hf(h, s->w); h = hf(h, s->h); h = hf(h, s->r);
        h = hf(h, s->dx); h = hf(h, s->dy); h = hf(h, s->blur); h = hf(h, s->spread);
        break;
    }
    case G7_ARC: {
        const struct g7_arc *a = &it->u.arc;
        h = hf(h, a->cx); h = hf(h, a->cy); h = hf(h, a->r); h = hf(h, a->width); h = hf(h, a->a0); h = hf(h, a->sweep);
        break;
    }
    case G7_STROKE: {
        const struct g7_stroke *s = &it->u.stroke;
        h = hmix(h, s->n);
        for (int i = 0; i < 4 && i < s->n; i++) {
            h = hf(h, s->x[i]);
            h = hf(h, s->y[i]);
        }
        h = hf(h, s->width);
        break;
    }
    }
    return h;
}

static void shape_bounds(const struct g7_shape *s, int16_t b[4])
{
    float mx = (s->flags & G7_S_LENS) ? 4.f : 112.f, my = (s->flags & G7_S_LENS) ? 4.f : 152.f;
    float x0 = g_clamp(g_floor(s->x - mx), 0.f, GW), y0 = g_clamp(g_floor(s->y - my), 0.f, GH);
    float x1 = g_clamp(__builtin_ceilf(s->x + s->w + mx), 0.f, GW), y1 = g_clamp(__builtin_ceilf(s->y + s->h + my), 0.f, GH);
    b[0] = (int16_t)x0;
    b[1] = (int16_t)y0;
    b[2] = (int16_t)x1;
    b[3] = (int16_t)y1;
}

/* index of previous keys: open addressing */
#define HT 2048
static int16_t HTAB[HT];
static uint8_t SEEN[G7_MAX_ITEMS];

static void diff(const struct pent *prev, int np, const struct pent *cur, int nc)
{
    for (int i = 0; i < HT; i++)
        HTAB[i] = -1;
    for (int i = 0; i < np; i++) {
        unsigned h = (prev[i].key * 2654435761U) >> 21;
        while (HTAB[h] >= 0)
            h = (h + 1) & (HT - 1);
        HTAB[h] = (int16_t)i;
        SEEN[i] = 0;
    }
    for (int i = 0; i < nc; i++) {
        unsigned h = (cur[i].key * 2654435761U) >> 21;
        int f = -1;
        while (HTAB[h] >= 0) {
            if (prev[HTAB[h]].key == cur[i].key && !SEEN[HTAB[h]]) {
                f = HTAB[h];
                break;
            }
            h = (h + 1) & (HT - 1);
        }
        if (f < 0) {
            dirty_b(cur[i].b);
            continue;
        }
        SEEN[f] = 1;
        const int16_t *a = prev[f].b, *b = cur[i].b;
        if (prev[f].hash != cur[i].hash || a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3]) {
            dirty_b(a);
            dirty_b(b);
        }
    }
    for (int i = 0; i < np; i++)
        if (!SEEN[i])
            dirty_b(prev[i].b);
}

/* this frame's entries of a layer; bounds for drawing into BND[li] */
static void layer_entries(int li, const struct g7_layer *l, int *ns, int *ni)
{
    int n = 0;
    for (unsigned i = 0; l && i < l->nshape && i < G7_MAX_SHAPES; i++) {
        const struct g7_shape *s = &l->shape[i];
        CS[n].key = s->key ? s->key : 0xf0000000U | i;
        CS[n].hash = s->a > .002f ? shape_hash(s) : 0;
        if (s->a > .002f)
            shape_bounds(s, CS[n].b);
        else
            CS[n].b[0] = CS[n].b[1] = CS[n].b[2] = CS[n].b[3] = 0;
        n++;
    }
    *ns = n;
    n = 0;
    for (unsigned i = 0; l && i < l->nitem && i < G7_MAX_ITEMS; i++) {
        const struct g7_item *it = &l->item[i];
        int16_t *b = BND[li][i];
        if (!gd_bounds(l, it, b))
            b[0] = b[1] = b[2] = b[3] = 0;
        CI[n].key = it->key ? it->key : 0xf0000000U | i;
        CI[n].hash = b[2] > b[0] ? item_hash(l, it) : 0;
        for (int k = 0; k < 4; k++)
            CI[n].b[k] = b[k];
        n++;
    }
    *ni = n;
}

static void layer_diff_save(int li, const struct g7_layer *l, int check)
{
    int ns, ni;
    layer_entries(li, l, &ns, &ni);
    if (check) {
        diff(PS[li], nps[li], CS, ns);
        diff(PI[li], npi[li], CI, ni);
    }
    for (int i = 0; i < ns; i++)
        PS[li][i] = CS[i];
    for (int i = 0; i < ni; i++)
        PI[li][i] = CI[i];
    nps[li] = ns;
    npi[li] = ni;
}

/* ---- composition ---- */

static void base_acc_row(int y, int x0, int n)
{
    float dim = CBASE.dim;
    if (base_ok) {
        const uint32_t *b = (const uint32_t *)G7_BASE + (unsigned)y * GW + x0;
        for (int i = 0; i < n; i++) {
            uint32_t v = b[i];
            ACC[3 * i] = UNP10_R(v) * dim;
            ACC[3 * i + 1] = UNP10_G(v) * dim;
            ACC[3 * i + 2] = UNP10_B(v) * dim;
        }
    } else {
        gw_row_wall(y, x0, x0 + n, ACC);
        for (int i = 0; i < 3 * n; i++)
            ACC[i] *= dim;
    }
    for (int i = 0; i < n; i++)
        AL[i] = 1.f;
}

static int shapes_touch_row(const struct g7_layer *l, float py)
{
    for (unsigned i = 0; l && i < l->nshape; i++) {
        const struct g7_shape *s = &l->shape[i];
        if (s->a > .002f && py >= s->y - 150.f && py <= s->y + s->h + 150.f)
            return 1;
    }
    return 0;
}

/* the base layer (glass and items) for the rect into dst (stride GW) */
static void compose_base(uint32_t *dst, int x0, int y0, int x1, int y1)
{
    int n = x1 - x0, wy0 = 0, wy1 = 0, wx0 = 0, wx1 = 0;
    int word = gs_word_rows(&wy0, &wy1, &wx0, &wx1);
    for (int y = y0; y < y1; y++) {
        float py = y + .5f;
        base_acc_row(y, x0, n);
        if (shapes_touch_row(LB, py)) {
            gw_row_a5(y, x0, x1, RA5);
            gw_row_b(y, x0, x1, RB);
            gs_row(0, &CBASE, py, x0 + .5f, n, ACC, AL, RA5, RB);
        }
        if (word && y >= wy0 && y < wy1) {
            int a = wx0 > x0 ? wx0 : x0, b = wx1 < x1 ? wx1 : x1;
            if (a < b)
                gs_word_row(&CBASE, py, a, b - a, ACC + 3 * (a - x0));
        }
        gs_store_row(dst + (unsigned)y * GW + x0, y, x0, n, ACC);
    }
    if (LB) {
        struct gd_target t = { dst, GW, x0, y0, x1, y1, 1.f, 0.f, 0.f, 0 };
        gd_items(LB, (const int16_t (*)[4])BND[0], &t);
    }
}

static void compose_frozen(uint32_t *dst, int x0, int y0, int x1, int y1)
{
    const uint32_t *sharp = (const uint32_t *)G7_SHARP, *soft = (const uint32_t *)P_SOFT;
    float k = g_sat(back_now), ik = 1.f - k;
    for (int y = y0; y < y1; y++) {
        float cy = (y + .5f) * .5f - .5f;
        int r0 = g_ifloor(cy);
        float fy = cy - (float)r0;
        int ra = r0 < 0 ? 0 : r0, rb = r0 + 1 >= WH ? WH - 1 : r0 + 1;
        const uint32_t *A = soft + (unsigned)ra * WW, *B = soft + (unsigned)rb * WW, *S = sharp + (unsigned)y * GW;
        uint32_t *d = dst + (unsigned)y * GW;
        for (int x = x0; x < x1; x++) {
            float cx = (x + .5f) * .5f - .5f;
            int c0 = g_ifloor(cx);
            float fx = cx - (float)c0;
            int ca = c0 < 0 ? 0 : c0, cb = c0 + 1 >= WW ? WW - 1 : c0 + 1;
            uint32_t p00 = A[ca], p01 = A[cb], p10 = B[ca], p11 = B[cb], s = S[x];
            float w00 = (1.f - fx) * (1.f - fy), w01 = fx * (1.f - fy), w10 = (1.f - fx) * fy, w11 = fx * fy;
            float r = (w00 * CR(p00) + w01 * CR(p01) + w10 * CR(p10) + w11 * CR(p11)) * k + CR(s) * ik;
            float g = (w00 * CG(p00) + w01 * CG(p01) + w10 * CG(p10) + w11 * CG(p11)) * k + CG(s) * ik;
            float b = (w00 * CB(p00) + w01 * CB(p01) + w10 * CB(p10) + w11 * CB(p11)) * k + CB(s) * ik;
            d[x] = 0xff000000U | (uint32_t)(r + .5f) << 16 | (uint32_t)(g + .5f) << 8 | (uint32_t)(b + .5f);
        }
    }
}

static void compose_over(uint32_t *dst, int x0, int y0, int x1, int y1)
{
    int n = x1 - x0;
    float al0 = COVER.back * .45f;
    if (LO && (LO->nshape || al0 > 0.f))
        for (int y = y0; y < y1; y++) {
            float py = y + .5f;
            int touch = shapes_touch_row(LO, py);
            if (!touch && al0 <= 0.f)
                continue;
            for (int i = 0; i < n; i++) {
                ACC[3 * i] = 0.f;
                ACC[3 * i + 1] = .004f;
                ACC[3 * i + 2] = .014f;
                AL[i] = al0;
            }
            if (touch) {
                gw_row_a5(y, x0, x1, RA5);
                gw_row_b(y, x0, x1, RB);
                gs_row(1, &COVER, py, x0 + .5f, n, ACC, AL, RA5, RB);
            }
            gs_over_row(dst + (unsigned)y * GW + x0, y, x0, n, ACC, AL);
        }
    if (LO && LO->nitem) {
        struct gd_target t = { dst, GW, x0, y0, x1, y1, 1.f, 0.f, 0.f, 0 };
        gd_items(LO, (const int16_t (*)[4])BND[1], &t);
    }
}

static void compose_edge(uint32_t *dst, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++) {
        uint32_t *d = dst + (unsigned)y * GW;
        for (int x = x0; x < x1; x++) {
            float t;
            if (x < 35)
                t = (x + .5f) / 34.56f;
            else if (x >= 1405)
                t = ((float)GW - x - .5f) / 34.56f;
            else {
                x = 1404;
                continue;
            }
            if (t >= 1.f)
                continue;
            uint32_t k = (uint32_t)((1.f - .6f * (1.f - t)) * 256.f + .5f), v = d[x];
            d[x] = 0xff000000U | ((CR(v) * k) >> 8) << 16 | ((CG(v) * k) >> 8) << 8 | ((CB(v) * k) >> 8);
        }
    }
}

static void compose(int x0, int y0, int x1, int y1)
{
    uint32_t *canvas = (uint32_t *)G7_CANVAS;
    if (frozen)
        compose_frozen(canvas, x0, y0, x1, y1);
    else
        compose_base(canvas, x0, y0, x1, y1);
    compose_over(canvas, x0, y0, x1, y1);
    compose_edge(canvas, x0, y0, x1, y1);
}

static void present(int x0, int y0, int x1, int y1)
{
    const uint32_t *s = (const uint32_t *)G7_CANVAS;
    volatile uint32_t *fb = G7_FB;
    if (x0 == 0 && x1 == GW) {
        const uint64_t *a = (const uint64_t *)(s + (unsigned)y0 * GW);
        volatile uint64_t *b = (volatile uint64_t *)(fb + (unsigned)y0 * GW);
        for (unsigned i = 0; i < (unsigned)(y1 - y0) * GW / 2; i++)
            b[i] = a[i];
    } else {
        x0 &= ~1;
        x1 = (x1 + 1) & ~1;
        for (int y = y0; y < y1; y++) {
            const uint64_t *a = (const uint64_t *)(s + (unsigned)y * GW + x0);
            volatile uint64_t *b = (volatile uint64_t *)(fb + (unsigned)y * GW + x0);
            for (int i = 0; i < (x1 - x0) / 2; i++)
                b[i] = a[i];
        }
    }
#ifndef M7_PREVIEW
    __asm__ volatile("dsb st" : : : "memory");
#endif
}

/* ---- the freeze (D4) ---- */

/* box blur of a premultiplied 720 x 1480 buffer, radius r, zero outside; src -> dst */
static void box_h(const uint32_t *src, uint32_t *dst, int r)
{
    int w = 2 * r + 1;
    for (int y = 0; y < WH; y++) {
        const uint32_t *s = src + (unsigned)y * WW;
        uint32_t *d = dst + (unsigned)y * WW;
        uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
        for (int x = 0; x < r && x < WW; x++) {
            sa += CA(s[x]); sr += CR(s[x]); sg += CG(s[x]); sb += CB(s[x]);
        }
        for (int x = 0; x < WW; x++) {
            if (x + r < WW) {
                uint32_t v = s[x + r];
                sa += CA(v); sr += CR(v); sg += CG(v); sb += CB(v);
            }
            d[x] = (sa + w / 2) / w << 24 | (sr + w / 2) / w << 16 | (sg + w / 2) / w << 8 | (sb + w / 2) / w;
            if (x - r >= 0) {
                uint32_t v = s[x - r];
                sa -= CA(v); sr -= CR(v); sg -= CG(v); sb -= CB(v);
            }
        }
    }
}
static void box_v(const uint32_t *src, uint32_t *dst, int r)
{
    static uint32_t sum[4 * WW];
    int w = 2 * r + 1;
    for (int i = 0; i < 4 * WW; i++)
        sum[i] = 0;
    for (int y = 0; y < r && y < WH; y++)
        for (int x = 0; x < WW; x++) {
            uint32_t v = src[(unsigned)y * WW + x];
            sum[4 * x] += CA(v); sum[4 * x + 1] += CR(v); sum[4 * x + 2] += CG(v); sum[4 * x + 3] += CB(v);
        }
    for (int y = 0; y < WH; y++) {
        if (y + r < WH)
            for (int x = 0; x < WW; x++) {
                uint32_t v = src[(unsigned)(y + r) * WW + x];
                sum[4 * x] += CA(v); sum[4 * x + 1] += CR(v); sum[4 * x + 2] += CG(v); sum[4 * x + 3] += CB(v);
            }
        uint32_t *d = dst + (unsigned)y * WW;
        for (int x = 0; x < WW; x++)
            d[x] = (sum[4 * x] + w / 2) / w << 24 | (sum[4 * x + 1] + w / 2) / w << 16 | (sum[4 * x + 2] + w / 2) / w << 8 |
                   (sum[4 * x + 3] + w / 2) / w;
        if (y - r >= 0)
            for (int x = 0; x < WW; x++) {
                uint32_t v = src[(unsigned)(y - r) * WW + x];
                sum[4 * x] -= CA(v); sum[4 * x + 1] -= CR(v); sum[4 * x + 2] -= CG(v); sum[4 * x + 3] -= CB(v);
            }
    }
}

static void freeze(void)
{
    uint64_t t0 = hw_ticks();
    /* SHARP: the base layer as it is */
    compose_base((uint32_t *)G7_SHARP, 0, 0, GW, GH);
    /* SOFT: base glass at uSoft = 1, half resolution (K = 2), shapes zoomed .965 */
    struct gs_ctx cs = CBASE;
    cs.K = 2.f;
    cs.soft = 1.f;
    const float zoom = .965f;
    gs_prepare(2, LB ? LB->shape : 0, LB ? LB->nshape : 0, &cs, zoom);
    uint32_t *soft = (uint32_t *)P_SOFT;
    for (int j = 0; j < WH; j++) {
        float py = 2.f * j + 1.f;
        gw_row_b_at(py, WW, 1.f, 2.f, RB);
        for (int i = 0; i < WW; i++) {
            ACC[3 * i] = RB[3 * i] * cs.dim;
            ACC[3 * i + 1] = RB[3 * i + 1] * cs.dim;
            ACC[3 * i + 2] = RB[3 * i + 2] * cs.dim;
            AL[i] = 1.f;
        }
        gw_row_a5_at(py, WW, 1.f, 2.f, RA5);
        gs_row(2, &cs, py, 1.f, WW, ACC, AL, RA5, RB);
        uint32_t *d = soft + (unsigned)j * WW;
        for (int i = 0; i < WW; i++)
            d[i] = 0xff000000U | (uint32_t)(g_sat(ACC[3 * i]) * 255.f + .5f) << 16 |
                   (uint32_t)(g_sat(ACC[3 * i + 1]) * 255.f + .5f) << 8 | (uint32_t)(g_sat(ACC[3 * i + 2]) * 255.f + .5f);
    }
    /* the content at half size, zoomed about (720, 1332): target = (720 + (p - 720) .965) / 2 */
    uint32_t *ct = (uint32_t *)P_FIELD, *tmp = (uint32_t *)P_TMP;
    gw_field_lost();
    for (unsigned i = 0; i < WW * WH; i++)
        ct[i] = 0;
    if (LB && LB->nitem) {
        float s = zoom * .5f;
        struct gd_target t = { ct, WW, 0, 0, WW, WH, s, (720.f - 720.f * zoom) * .5f, (1332.f - 1332.f * zoom) * .5f, 1 };
        gd_items(LB, (const int16_t (*)[4])BND[0], &t);
        /* blur(26px) = sigma 13 half-res px: three box passes (radii 12, 12, 13) each way */
        box_h(ct, tmp, 12);
        box_h(tmp, ct, 12);
        box_h(ct, tmp, 13);
        box_v(tmp, ct, 12);
        box_v(ct, tmp, 12);
        box_v(tmp, ct, 13);
        /* brightness(.75), over the soft glass */
        for (unsigned i = 0; i < WW * WH; i++) {
            uint32_t c = ct[i];
            if (!CA(c))
                continue;
            uint32_t v = soft[i], ia = 255U - CA(c);
            uint32_t r = (CR(c) * 3U + 2U) / 4U + g_div255(CR(v) * ia), g = (CG(c) * 3U + 2U) / 4U + g_div255(CG(v) * ia),
                     b = (CB(c) * 3U + 2U) / 4U + g_div255(CB(v) * ia);
            soft[i] = 0xff000000U | (r > 255U ? 255U : r) << 16 | (g > 255U ? 255U : g) << 8 | (b > 255U ? 255U : b);
        }
    }
    ST.freeze_us = (uint32_t)us_since(t0);
}

/* ---- API ---- */

int g7_init(void)
{
    if (!sb7_base(0))
        return 0;
    if (!gd_init())
        return 0;
    gs_load_height();
    inited = 1;
    first = 1;
    frozen = 0;
    nps[0] = nps[1] = npi[0] = npi[1] = 0;
    return 1;
}

void g7_bake(void)
{
    uint64_t t0 = hw_ticks();
    gw_field();
    gw_wall(&WALL_FINAL);
    gw_blur();
    gw_base();
    cur_wall = WALL_FINAL;
    base_ok = 1;
    baked = 1;
    first = 1;
    ST.bake_us = (uint32_t)us_since(t0);
}

static int word_same(const struct g7_word *a, const struct g7_word *b)
{
    if (a->ta <= .001f && b->ta <= .001f)
        return 1;
    return a->tm == b->tm && a->tf == b->tf && a->ta == b->ta && a->cap[0] == b->cap[0] && a->cap[1] == b->cap[1] &&
           a->cap[2] == b->cap[2] && a->cap[3] == b->cap[3];
}

uint32_t g7_frame(const struct g7_frame *f)
{
    if (!inited || !baked)
        return 0;
    uint64_t t0 = hw_ticks();
    gs_new_frame();
    nrr = 0;
    all_dirty = first || (f->flags & G7_F_FULL);
    LB = f->base;
    LO = f->over;
    /* the wallpaper (intro) */
    if (!gw_wall_same(&f->wall, &cur_wall)) {
        if (!gw_field_valid())
            gw_field();
        gw_wall(&f->wall);
        cur_wall = f->wall;
        base_ok = 0;
        if (gw_wall_same(&f->wall, &WALL_FINAL)) {
            gw_base();
            base_ok = 1;
        }
        all_dirty = 1;
    }
    if (f->dim != cur_dim || f->lx != cur_lx || f->ly != cur_ly)
        all_dirty = 1;
    float Ldx = -.45f + f->lx * .9f, Ldy = -.9f + f->ly * .6f, ln = g_sqrt(Ldx * Ldx + Ldy * Ldy);
    CBASE.K = 1.f;
    CBASE.soft = 0.f;
    CBASE.dim = f->dim;
    CBASE.back = f->back;
    CBASE.opaque = 1;
    CBASE.lx = f->lx;
    CBASE.ly = f->ly;
    CBASE.Ldx = Ldx / ln;
    CBASE.Ldy = Ldy / ln;
    COVER = CBASE;
    COVER.opaque = 0;
    COVER.back = f->back;
    back_now = f->back;
    int want = f->back > .002f;
    if (want && (!frozen || (f->flags & G7_F_REFREEZE))) {
        frozen = 0;
        gs_prepare(0, LB ? LB->shape : 0, LB ? LB->nshape : 0, &CBASE, 1.f);
        gs_word_prepare(&f->word, &CBASE);
        layer_diff_save(0, LB, 0);
        freeze();
        frozen = 1;
        all_dirty = 1;
    } else if (!want && frozen) {
        frozen = 0;
        all_dirty = 1;
    }
    if (f->back != cur_back)
        all_dirty = 1;
    /* soft = back for the base glass (only while it is below the freeze threshold) */
    if (!frozen)
        CBASE.soft = f->back;
    /* lists */
    if (!frozen) {
        gs_prepare(0, LB ? LB->shape : 0, LB ? LB->nshape : 0, &CBASE, 1.f);
        gs_word_prepare(&f->word, &CBASE);
        if (!word_same(&f->word, &cur_word)) {
            int a, b, c, d;
            if (gs_word_rows(&a, &b, &c, &d))
                dirty(c, a, d, b);
            gs_word_prepare(&cur_word, &CBASE);
            if (gs_word_rows(&a, &b, &c, &d))
                dirty(c, a, d, b);
            gs_word_prepare(&f->word, &CBASE);
        }
        layer_diff_save(0, LB, !all_dirty);
    } else {
        gs_word_prepare(&f->word, &CBASE);
    }
    gs_prepare(1, LO ? LO->shape : 0, LO ? LO->nshape : 0, &COVER, 1.f);
    layer_diff_save(1, LO, !all_dirty);
    cur_word = f->word;
    cur_dim = f->dim;
    cur_back = f->back;
    cur_lx = f->lx;
    cur_ly = f->ly;
    first = 0;
    /* compose and present */
    uint32_t px = 0;
    if (!all_dirty && nrr) {
        merge_bands();
        uint64_t area = 0;
        for (int i = 0; i < nrr; i++)
            area += (uint64_t)(RR[i][2] - RR[i][0]) * (uint64_t)(RR[i][3] - RR[i][1]);
        if (area * 10U > (uint64_t)GW * GH * 6U)
            all_dirty = 1;
    }
    if (all_dirty) {
        nrr = 1;
        RR[0][0] = 0;
        RR[0][1] = 0;
        RR[0][2] = GW;
        RR[0][3] = GH;
    }
    for (int i = 0; i < nrr; i++) {
        compose(RR[i][0], RR[i][1], RR[i][2], RR[i][3]);
        present(RR[i][0], RR[i][1], RR[i][2], RR[i][3]);
        px += (uint32_t)(RR[i][2] - RR[i][0]) * (uint32_t)(RR[i][3] - RR[i][1]);
    }
    if (px) {
        ST.frames++;
        ST.last_us = (uint32_t)us_since(t0);
        if (ST.last_us > ST.worst_us)
            ST.worst_us = ST.last_us;
    }
    ST.last_px = px;
    return px;
}

void g7_present_all(void)
{
    if (inited && baked)
        present(0, 0, GW, GH);
}

void g7_get_stats(struct g7_stats *s)
{
    *s = ST;
}

#ifdef M7_PREVIEW
/* preview harness: the dirty rects of the last frame */
int g7_preview_rects(int16_t (*out)[4], int max)
{
    int n = nrr < max ? nrr : max;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 4; k++)
            out[i][k] = RR[i][k];
    return n;
}
#endif
