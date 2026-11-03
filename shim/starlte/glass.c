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
/* row buffers in the glass pool's table area (P_TAB), not .bss: the frost rows of a panel row,
 * the shader's accumulator (rgb, alpha) */
#define TAB_ROWS (P_TAB + 0x2C000UL)          /* after gw_wall's radial and column tables (.. 0x2B200) */
#define RA5 ((float *)TAB_ROWS)
#define RB (RA5 + 3 * GW)
#define ACC (RB + 3 * GW)
#define AL (ACC + 3 * GW)
_Static_assert(0x2C000UL + (10UL * GW) * 4UL <= 0x40000UL, "glass row buffers overflow");

static struct g7_stats ST;
static int inited, baked, first = 1, frozen, base_ok;
static struct g7_wall cur_wall;
static float cur_dim = -1.f, cur_back = -1.f, cur_lx, cur_ly;
static struct g7_word cur_word;
static struct gs_ctx CBASE, COVER;
static const struct g7_layer *LB, *LO;
static float back_now, glb_soft = -1.f;
static int bg8_ok;                  /* G7_BASE holds BG8 for the final wallpaper */
static void (*poll_fn)(void);

void g7_set_poll(void (*fn)(void)) { poll_fn = fn; }
void g7_poll(void)
{
    if (poll_fn)
        poll_fn();
}

static inline uint64_t us_between(uint64_t t0, uint64_t t1)
{
    uint64_t f = hw_freq();
    return f ? (t1 - t0) * 1000000ULL / f : 0;
}
static inline uint64_t us_since(uint64_t t0) { return us_between(t0, hw_ticks()); }

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

/* ---- GLB: the base layer's glass, composed (BG8 + every base shape before the lens, dithered) ----
 *
 * Kept in G7_SHARP while nothing is frozen. A frame whose glass did not change where it draws (the
 * lens moved, the countdown ring or a text changed) copies GLB there and shades only the lens (and
 * the intro's wordmark) on top, instead of every card under it. GV0 / GV1: per row, the x range of
 * GLB that holds this layout's glass. */
#define GLB ((uint32_t *)G7_SHARP)
static int16_t GV0[GH], GV1[GH];
static uint8_t GAPX[GH];            /* GLB row drawn at half resolution (motion): redraw it at rest */
static int napx;                    /* rows with GAPX set */
static void glb_inval_all(void)
{
    for (int y = 0; y < GH; y++) {
        GV0[y] = GV1[y] = 0;
        GAPX[y] = 0;
    }
    napx = 0;
}
static uint64_t glass_area;         /* this frame: the area of the glass's changes (GLB invalidated) */
static void glb_inval(const int16_t b[4])
{
    if (b[2] > b[0] && b[3] > b[1])
        glass_area += (uint64_t)(b[2] - b[0]) * (uint64_t)(b[3] - b[1]);
    for (int y = b[1] < 0 ? 0 : b[1]; y < b[3] && y < GH; y++) {
        if (b[2] <= GV0[y] || b[0] >= GV1[y])
            continue;
        /* keep the larger part of the row's valid range outside [b0, b2) */
        int l = b[0] - GV0[y], r = GV1[y] - b[2];
        if (l >= r && l > 0)
            GV1[y] = b[0];
        else if (r > 0)
            GV0[y] = b[2];
        else
            GV0[y] = GV1[y] = 0;
    }
}
static void glb_valid(int y, int x0, int x1)
{
    if (GV1[y] > GV0[y] && x0 <= GV1[y] && x1 >= GV0[y]) {
        if (x0 < GV0[y]) GV0[y] = (int16_t)x0;
        if (x1 > GV1[y]) GV1[y] = (int16_t)x1;
    } else {
        GV0[y] = (int16_t)x0;
        GV1[y] = (int16_t)x1;
    }
}


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
    uint8_t glass;                  /* 1: a base shape before the lens (its pixels live in GLB); 2: an over shape */
};
static uint64_t over_area;          /* this frame: the area of the over glass's changes */
static void dirty_e(const struct pent *e, const int16_t b[4])
{
    dirty_b(b);
    if (e->glass == 1)
        glb_inval(b);
    else if (e->glass == 2 && b[2] > b[0] && b[3] > b[1])
        over_area += (uint64_t)(b[2] - b[0]) * (uint64_t)(b[3] - b[1]);
}
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
            dirty_e(&cur[i], cur[i].b);
            continue;
        }
        SEEN[f] = 1;
        const int16_t *a = prev[f].b, *b = cur[i].b;
        if (prev[f].hash != cur[i].hash || prev[f].glass != cur[i].glass || a[0] != b[0] || a[1] != b[1] ||
            a[2] != b[2] || a[3] != b[3]) {
            dirty_e(&prev[f], a);
            dirty_e(&cur[i], b);
        }
    }
    for (int i = 0; i < np; i++)
        if (!SEEN[i])
            dirty_e(&prev[i], prev[i].b);
}

/* this frame's entries of a layer; bounds for drawing into BND[li] */
static void layer_entries(int li, const struct g7_layer *l, int *ns, int *ni)
{
    int n = 0, pre = li == 0;
    for (unsigned i = 0; l && i < l->nshape && i < G7_MAX_SHAPES; i++) {
        const struct g7_shape *s = &l->shape[i];
        if (s->flags & G7_S_LENS)
            pre = 0;
        CS[n].key = s->key ? s->key : 0xf0000000U | i;
        CS[n].glass = (uint8_t)(li == 0 ? pre : 2);
        /* the glass's paint order matters where shapes overlap: a GLB shape's place is in its hash */
        CS[n].hash = s->a > .002f ? shape_hash(s) ^ (pre ? i * 0x9e3779b9U : 0) : 0;
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
        CI[n].glass = 0;
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

static int shapes_touch_row(const struct g7_layer *l, float py)
{
    for (unsigned i = 0; l && i < l->nshape; i++) {
        const struct g7_shape *s = &l->shape[i];
        if (s->a > .002f && py >= s->y - 150.f && py <= s->y + s->h + 150.f)
            return 1;
    }
    return 0;
}

/* the layer's dithered pixels d[0 .. n) (panel x x0 ..) as ACC (0 .. 1), AL = 1 */
static void acc_from(const uint32_t *d, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t v = d[i];
        ACC[3 * i] = (float)CR(v) * (1.f / 255.f);
        ACC[3 * i + 1] = (float)CG(v) * (1.f / 255.f);
        ACC[3 * i + 2] = (float)CB(v) * (1.f / 255.f);
        AL[i] = 1.f;
    }
}

/* below the base glass: BG8 (the final wallpaper x .8, dithered once at the bake) or, in the
 * intro, the live wallpaper row x dim */
static void base_bg_row(uint32_t *d, int y, int x0, int x1)
{
    if (base_ok && CBASE.dim == .8f) {
        if (!bg8_ok) {
            gw_base();
            bg8_ok = 1;
        }
        g_copy32(d, (const uint32_t *)G7_BASE + (unsigned)y * GW + x0, x1 - x0);
    } else {
        gw_row_wall8(y, x0, x1, CBASE.dim, d);
    }
}

/* prepared base shapes s0 .. s1 - 1 (and the wordmark, if wd) over d[0 .. n) (panel x x0 ..): only
 * the span a row's glass touches goes through the float shader (and its dither again: a pixel the
 * glass leaves alone stores floor(v + h) = v) */
static void base_glass_row(uint32_t *d, int y, int x0, int x1, int s0, int s1, int wd, int wx0, int wx1)
{
    float py = y + .5f;
    int sa = 0, sb = 0, sh = s1 > s0 && gs_row_span(0, py, s0, s1, &sa, &sb);
    if (wd) {
        sa = sh ? (sa < wx0 ? sa : wx0) : wx0;
        sb = sh ? (sb > wx1 ? sb : wx1) : wx1;
    }
    if (!sh && !wd)
        return;
    if (sa < x0) sa = x0;
    if (sb > x1) sb = x1;
    if (sa >= sb)
        return;
    int m = sb - sa;
    acc_from(d + (sa - x0), m);
    if (sh) {
        gw_row_a5(y, sa, sb, RA5);
        gw_row_b(y, sa, sb, RB);
        gs_row(0, &CBASE, py, sa + .5f, m, ACC, AL, RA5, RB, s0, s1);
    }
    if (wd) {
        int a = wx0 > sa ? wx0 : sa, b = wx1 < sb ? wx1 : sb;
        if (a < b)
            gs_word_row(&CBASE, py, a, b - a, ACC + 3 * (a - sa));
    }
    gs_store_row(d + (sa - x0), y, sa, m, ACC);
}

/* ---- motion frames: the base glass at half resolution ----
 *
 * While the glass moves (the list scrolls, a section opens, the intro), a frame shades the base
 * glass on the half-resolution grid (panel x = 2i + 1, y = 2j + 1, the wallpaper texture's own
 * grid) as premultiplied colour C and transmission M over whatever is below, and every panel row
 * takes C + M x BG8 bilinear from the two half rows around it: a quarter of the shading. The rows
 * are marked (GAPX) and drawn again at full resolution once the glass rests (g7_idle). */
struct hrow { int j, ia, n; float C[3 * (GW / 2 + 4)], M[GW / 2 + 4]; };
#define TAB_HALF (P_TAB + 0xDD000UL)          /* after the specular tables (.. 0xDCA00) */
#define HR ((struct hrow *)TAB_HALF)          /* base half rows [2], over half rows HO [2] */
#define HO (HR + 2)
#define RA5H ((float *)(HR + 4))
#define RBH (RA5H + 3 * (GW / 2 + 4))
#define HV ((float (*)[GW / 2 + 8])(RBH + 3 * (GW / 2 + 4)))
_Static_assert(0xDD000UL + 4UL * sizeof(struct hrow) + 6UL * (GW / 2 + 4) * 4UL + 4UL * (GW / 2 + 8) * 4UL <= 0x100000UL,
               "glass half-row buffers overflow");
static void hr_reset(void) { HR[0].n = HR[1].n = -1; }
static const struct hrow *half_row(int j, int ia, int n, int s0, int s1)
{
    for (int k = 0; k < 2; k++)
        if (HR[k].j == j && HR[k].ia == ia && HR[k].n == n)
            return &HR[k];
    /* keep the neighbour row (j - 1 or j + 1) the caller needs with this one */
    int keep0 = HR[0].n == n && HR[0].ia == ia && (HR[0].j == j - 1 || HR[0].j == j + 1);
    struct hrow *h = &HR[keep0 ? 1 : 0];
    h->j = j;
    h->ia = ia;
    h->n = n;
    for (int i = 0; i < 3 * n; i++)
        h->C[i] = 0.f;
    for (int i = 0; i < n; i++)
        h->M[i] = 0.f;              /* AL, then 1 - AL */
    float py = 2.f * (float)j + 1.f, px0 = 2.f * (float)ia + 1.f;
    int a = 0, b = 0;
    if (s1 > s0 && gs_row_span(GS_HALF, py, s0, s1, &a, &b)) {
        gw_row_a5_at(py, n, px0, 2.f, RA5H);
        gw_row_b_at(py, n, px0, 2.f, RBH);
        gs_row(GS_HALF, &CBASE, py, px0, n, h->C, h->M, RA5H, RBH, s0, s1);
    }
    for (int i = 0; i < n; i++)
        h->M[i] = 1.f - h->M[i];
    return h;
}

/* d[0 .. x1 - x0) holds what is below (dithered): the half-resolution glass over it, dithered.
 * The two half rows blended (HV: r g b x 255, M), then panel x = 2m from half columns m - 1, m at
 * .25 / .75 and x = 2m + 1 from m, m + 1 at .75 / .25, eight pixels a NEON step. */
static inline uint32_t half_px(int x, int ia, float Y, uint32_t v)
{
    int k = ((x - 1) >> 1) - ia;
    float fx = (x & 1) ? .25f : .75f, h = g_dither(x, Y);
    float m = HV[3][k] + (HV[3][k + 1] - HV[3][k]) * fx;
    float r = HV[0][k] + (HV[0][k + 1] - HV[0][k]) * fx + m * (float)CR(v) + h;
    float g = HV[1][k] + (HV[1][k + 1] - HV[1][k]) * fx + m * (float)CG(v) + h;
    float b = HV[2][k] + (HV[2][k + 1] - HV[2][k]) * fx + m * (float)CB(v) + h;
    return 0xff000000U | g_q255(r) << 16 | g_q255(g) << 8 | g_q255(b);
}
static void base_glass_half(uint32_t *d, int y, int x0, int x1, int npre)
{
    float cy = (float)y * .5f - .25f;
    int j0 = g_ifloor(cy);
    float fy = cy - (float)j0;
    int ia = ((x0 - 1) >> 1), ib = ((x1 - 2) >> 1) + 1, n = ib - ia + 1;
    int ja = j0 < 0 ? 0 : j0, jb = j0 + 1 > WH - 1 ? WH - 1 : j0 + 1;
    const struct hrow *A = half_row(ja, ia, n, 0, npre), *B = half_row(jb, ia, n, 0, npre);
    float mn = 1.f;
    const float32x4_t vfy = vdupq_n_f32(fy), k255 = vdupq_n_f32(255.f);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t ma = vld1q_f32(A->M + i), mb = vld1q_f32(B->M + i);
        mn = g_min(mn, vminvq_f32(vminq_f32(ma, mb)));
        vst1q_f32(HV[3] + i, vfmaq_f32(ma, vsubq_f32(mb, ma), vfy));
        float32x4x3_t ca = vld3q_f32(A->C + 3 * i), cb = vld3q_f32(B->C + 3 * i);
        for (int c = 0; c < 3; c++)
            vst1q_f32(HV[c] + i, vmulq_f32(vfmaq_f32(ca.val[c], vsubq_f32(cb.val[c], ca.val[c]), vfy), k255));
    }
    for (; i < n; i++) {
        float ma = A->M[i], mb = B->M[i];
        mn = g_min(mn, g_min(ma, mb));
        HV[3][i] = ma + (mb - ma) * fy;
        for (int c = 0; c < 3; c++)
            HV[c][i] = (A->C[3 * i + c] + (B->C[3 * i + c] - A->C[3 * i + c]) * fy) * 255.f;
    }
    if (mn >= 1.f)
        return;                     /* no glass on this row: d stays */
    for (int c = 0; c < 4; c++)
        HV[c][n] = HV[c][n + 1] = HV[c][n - 1];
    float Y = gs_dither_y(y);
    int x = x0;
    while (x < x1 && (x & 1)) {
        d[x - x0] = half_px(x, ia, Y, d[x - x0]);
        x++;
    }
    const float32x4_t q1 = vdupq_n_f32(.25f), q3 = vdupq_n_f32(.75f);
    for (; x + 8 <= x1; x += 8) {
        int k = (x >> 1) - 1 - ia;
        float32x4_t v[4][2], bg[3][2];
        for (int c = 0; c < 4; c++) {
            float32x4_t a = vld1q_f32(HV[c] + k), b = vld1q_f32(HV[c] + k + 1), e = vld1q_f32(HV[c] + k + 2);
            float32x4x2_t z = vzipq_f32(vfmaq_f32(vmulq_f32(a, q1), b, q3), vfmaq_f32(vmulq_f32(b, q3), e, q1));
            v[c][0] = z.val[0];
            v[c][1] = z.val[1];
        }
        g_unpack8(d + (x - x0), bg);
        for (int hh = 0; hh < 2; hh++) {
            float32x4_t h = g_dither4(x + 4 * hh, Y), o[3];
            for (int c = 0; c < 3; c++)
                o[c] = vfmaq_f32(vaddq_f32(v[c][hh], h), v[3][hh], bg[c][hh]);
            vst1q_u32(d + (x - x0) + 4 * hh, g_pack4(o[0], o[1], o[2]));
        }
    }
    for (; x < x1; x++)
        d[x - x0] = half_px(x, ia, Y, d[x - x0]);
}

/* the base layer (glass and items) for the rect into dst (stride GW). glb: use and keep GLB;
 * half: rows that need their glass again get it at half resolution (a motion frame) */
static void compose_base(uint32_t *dst, int x0, int y0, int x1, int y1, int glb, int half)
{
    int n = x1 - x0, wy0 = 0, wy1 = 0, wx0 = 0, wx1 = 0;
    int word = gs_word_rows(&wy0, &wy1, &wx0, &wx1);
    int npre = LB ? gs_npre(0) : 0, nsh = LB ? gs_nshapes(0) : 0;
    hr_reset();
    for (int y = y0; y < y1; y++) {
        uint32_t *d = dst + (unsigned)y * GW + x0;
        if (!(y & 3))
            g7_poll();
        int wd = word && y >= wy0 && y < wy1;
        if (glb && GV0[y] <= x0 && x1 <= GV1[y]) {
            const uint32_t *g = GLB + (unsigned)y * GW + x0;
            if (g != d)
                g_copy32(d, g, n);
        } else {
            base_bg_row(d, y, x0, x1);
            if (half)
                base_glass_half(d, y, x0, x1, npre);
            else
                base_glass_row(d, y, x0, x1, 0, npre, 0, 0, 0);
            if (glb) {
                uint32_t *g = GLB + (unsigned)y * GW + x0;
                if (g != d)
                    g_copy32(g, d, n);
                glb_valid(y, x0, x1);
                if (half && !GAPX[y]) {
                    GAPX[y] = 1;
                    napx++;
                } else if (!half && GAPX[y] && x0 == 0 && x1 == GW) {
                    GAPX[y] = 0;
                    napx--;
                }
            }
        }
        base_glass_row(d, y, x0, x1, npre, nsh, wd, wx0, wx1);
    }
    if (LB) {
        struct gd_target t = { dst, GW, x0, y0, x1, y1, 1.f, 0.f, 0.f, 0 };
        gd_items(LB, (const int16_t (*)[4])BND[0], &t);
    }
}

/* behind an overlay: mix(SHARP, SOFT bilinear x 2, back). SOFT's weights are sixteenths
 * (rows at .25 / .75, columns at .25 / .75), so the upsample is integer: 9, 3, 3, 1. */
static void compose_frozen(uint32_t *dst, int x0, int y0, int x1, int y1)
{
    const uint32_t *sharp = (const uint32_t *)G7_SHARP, *soft = (const uint32_t *)P_SOFT;
    float k = g_sat(back_now);
    uint32_t K = (uint32_t)(k * 256.f + .5f), IK = 256U - K;
    uint16x8_t vK = vdupq_n_u16((uint16_t)K), vIK = vdupq_n_u16((uint16_t)IK), three = vdupq_n_u16(3);
    for (int y = y0; y < y1; y++) {
        if (!(y & 7))
            g7_poll();
        int r0 = (y - 1) >> 1;      /* floor(y / 2 - .25) */
        int ra = r0 < 0 ? 0 : r0, rb = r0 + 1 >= WH ? WH - 1 : r0 + 1;
        int up = (y & 1) ? 3 : 1;   /* weight of row ra: y odd -> .75 */
        const uint32_t *A = soft + (unsigned)ra * WW, *B = soft + (unsigned)rb * WW, *S = sharp + (unsigned)y * GW;
        uint32_t *d = dst + (unsigned)y * GW;
        int x = x0;
        /* scalar edges: x not in [2, GW - 16] alignment */
        while (x < x1 && (x < 2 || (x & 1))) {
            int c0 = (x - 1) >> 1, ca = c0 < 0 ? 0 : c0, cb = c0 + 1 >= WW ? WW - 1 : c0 + 1;
            int wl = (x & 1) ? 3 : 1;
            uint32_t p00 = A[ca], p01 = A[cb], p10 = B[ca], p11 = B[cb], sv = S[x], o = 0xff000000U;
            for (int sh = 0; sh < 24; sh += 8) {
                uint32_t t = wl * up * ((p00 >> sh) & 255U) + (4U - wl) * up * ((p01 >> sh) & 255U) +
                             wl * (4U - up) * ((p10 >> sh) & 255U) + (4U - wl) * (4U - up) * ((p11 >> sh) & 255U);
                uint32_t sv8 = (t + 8U) >> 4;
                o |= ((sv8 * K + ((sv >> sh) & 255U) * IK + 128U) >> 8) << sh;
            }
            d[x] = o;
            x++;
        }
        for (; x + 16 <= x1 && (x >> 1) + 8 < WW; x += 16) {
            int t = (x >> 1) - 1;   /* x = 2m: texels m - 1 (1/4), m (3/4) */
            uint8x8x4_t a0 = vld4_u8((const uint8_t *)(A + t)), a1 = vld4_u8((const uint8_t *)(A + t + 1)),
                        a2 = vld4_u8((const uint8_t *)(A + t + 2));
            uint8x8x4_t b0 = vld4_u8((const uint8_t *)(B + t)), b1 = vld4_u8((const uint8_t *)(B + t + 1)),
                        b2 = vld4_u8((const uint8_t *)(B + t + 2));
            uint8x16x4_t sv = vld4q_u8((const uint8_t *)(S + x)), o;
            for (int c = 0; c < 3; c++) {
                /* vertical: up * A + (4 - up) * B */
                uint16x8_t v0, v1, v2;
                if (up == 3) {
                    v0 = vmlaq_u16(vmovl_u8(b0.val[c]), vmovl_u8(a0.val[c]), three);
                    v1 = vmlaq_u16(vmovl_u8(b1.val[c]), vmovl_u8(a1.val[c]), three);
                    v2 = vmlaq_u16(vmovl_u8(b2.val[c]), vmovl_u8(a2.val[c]), three);
                } else {
                    v0 = vmlaq_u16(vmovl_u8(a0.val[c]), vmovl_u8(b0.val[c]), three);
                    v1 = vmlaq_u16(vmovl_u8(a1.val[c]), vmovl_u8(b1.val[c]), three);
                    v2 = vmlaq_u16(vmovl_u8(a2.val[c]), vmovl_u8(b2.val[c]), three);
                }
                uint16x8_t ev = vrshrq_n_u16(vmlaq_u16(v0, v1, three), 4);    /* x = 2m */
                uint16x8_t od = vrshrq_n_u16(vmlaq_u16(v2, v1, three), 4);    /* x = 2m + 1 */
                uint16x8x2_t z = vzipq_u16(ev, od);
                uint16x8_t sl = vmovl_u8(vget_low_u8(sv.val[c])), sh = vmovl_u8(vget_high_u8(sv.val[c]));
                /* (soft K + sharp (256 - K) + 128) >> 8, in 16 bits: soft K <= 255 x 256 */
                uint16x8_t lo = vaddq_u16(vmulq_u16(z.val[0], vK), vmulq_u16(sl, vIK));
                uint16x8_t hi = vaddq_u16(vmulq_u16(z.val[1], vK), vmulq_u16(sh, vIK));
                o.val[c] = vcombine_u8(vrshrn_n_u16(lo, 8), vrshrn_n_u16(hi, 8));
            }
            o.val[3] = vdupq_n_u8(255);
            vst4q_u8((uint8_t *)(d + x), o);
        }
        for (; x < x1; x++) {
            int c0 = (x - 1) >> 1, ca = c0 < 0 ? 0 : c0, cb = c0 + 1 >= WW ? WW - 1 : c0 + 1;
            int wl = (x & 1) ? 3 : 1;
            uint32_t p00 = A[ca], p01 = A[cb], p10 = B[ca], p11 = B[cb], sv = S[x], o = 0xff000000U;
            for (int sh = 0; sh < 24; sh += 8) {
                uint32_t tt = wl * up * ((p00 >> sh) & 255U) + (4U - wl) * up * ((p01 >> sh) & 255U) +
                              wl * (4U - up) * ((p10 >> sh) & 255U) + (4U - wl) * (4U - up) * ((p11 >> sh) & 255U);
                uint32_t sv8 = (tt + 8U) >> 4;
                o |= ((sv8 * K + ((sv >> sh) & 255U) * IK + 128U) >> 8) << sh;
            }
            d[x] = o;
        }
    }
}

/* the over layer's half row j: backdrop and every over shape on the half grid, (A, al) */
static const struct hrow *half_over_row(int j, int ia, int n, float al0)
{
    for (int k = 0; k < 2; k++)
        if (HO[k].j == j && HO[k].ia == ia && HO[k].n == n)
            return &HO[k];
    int keep0 = HO[0].n == n && HO[0].ia == ia && (HO[0].j == j - 1 || HO[0].j == j + 1);
    struct hrow *h = &HO[keep0 ? 1 : 0];
    h->j = j;
    h->ia = ia;
    h->n = n;
    for (int i = 0; i < n; i++) {
        h->C[3 * i] = 0.f;
        h->C[3 * i + 1] = .004f;
        h->C[3 * i + 2] = .014f;
        h->M[i] = al0;
    }
    float py = 2.f * (float)j + 1.f, px0 = 2.f * (float)ia + 1.f;
    int a = 0, b = 0;
    if (gs_row_span(GS_HALFO, py, 0, G7_MAX_SHAPES, &a, &b)) {
        gw_row_a5_at(py, n, px0, 2.f, RA5H);
        gw_row_b_at(py, n, px0, 2.f, RBH);
        gs_row(GS_HALFO, &COVER, py, px0, n, h->C, h->M, RA5H, RBH, 0, G7_MAX_SHAPES);
    }
    return h;
}

/* ACC / AL for panel x0 .. x1 - 1 bilinear from half rows A, B (fy): HV blend, then the even / odd
 * pattern (x = 2m: columns m - 1, m at .25 / .75; x = 2m + 1: m, m + 1 at .75 / .25) */
static void half_fill(const struct hrow *A, const struct hrow *B, float fy, int ia, int m, int x0, int x1)
{
    for (int i = 0; i < m; i++) {
        HV[3][i] = A->M[i] + (B->M[i] - A->M[i]) * fy;
        for (int c = 0; c < 3; c++)
            HV[c][i] = A->C[3 * i + c] + (B->C[3 * i + c] - A->C[3 * i + c]) * fy;
    }
    for (int c = 0; c < 4; c++)
        HV[c][m] = HV[c][m + 1] = HV[c][m - 1];
    int x = x0;
    for (; x < x1 && (x & 1); x++) {
        int k = ((x - 1) >> 1) - ia;
        for (int c = 0; c < 3; c++)
            ACC[3 * (x - x0) + c] = HV[c][k] + (HV[c][k + 1] - HV[c][k]) * .25f;
        AL[x - x0] = HV[3][k] + (HV[3][k + 1] - HV[3][k]) * .25f;
    }
    const float32x4_t q1 = vdupq_n_f32(.25f), q3 = vdupq_n_f32(.75f);
    for (; x + 8 <= x1; x += 8) {
        int k = (x >> 1) - 1 - ia;
        float32x4x3_t o0, o1;
        for (int c = 0; c < 4; c++) {
            float32x4_t a = vld1q_f32(HV[c] + k), b = vld1q_f32(HV[c] + k + 1), e = vld1q_f32(HV[c] + k + 2);
            float32x4x2_t z = vzipq_f32(vfmaq_f32(vmulq_f32(a, q1), b, q3), vfmaq_f32(vmulq_f32(b, q3), e, q1));
            if (c < 3) {
                o0.val[c] = z.val[0];
                o1.val[c] = z.val[1];
            } else {
                vst1q_f32(AL + (x - x0), z.val[0]);
                vst1q_f32(AL + (x - x0) + 4, z.val[1]);
            }
        }
        vst3q_f32(ACC + 3 * (x - x0), o0);
        vst3q_f32(ACC + 3 * (x - x0) + 12, o1);
    }
    for (; x < x1; x++) {
        int k = ((x - 1) >> 1) - ia;
        float fx = (x & 1) ? .25f : .75f;
        for (int c = 0; c < 3; c++)
            ACC[3 * (x - x0) + c] = HV[c][k] + (HV[c][k + 1] - HV[c][k]) * fx;
        AL[x - x0] = HV[3][k] + (HV[3][k + 1] - HV[3][k]) * fx;
    }
}

static int motion_over;             /* the over glass moves: shade it at half resolution */
static uint8_t OAPX[GH];            /* a panel row whose over glass was drawn at half resolution */
static int noapx;
static void compose_over(uint32_t *dst, int x0, int y0, int x1, int y1)
{
    int n = x1 - x0;
    float al0 = COVER.back * .45f;
    HO[0].n = HO[1].n = -1;
    if (noapx && !(LO && (LO->nshape || al0 > 0.f)) && x0 == 0 && x1 == GW)
        for (int y = y0; y < y1; y++)
            if (OAPX[y]) {
                OAPX[y] = 0;
                noapx--;
            }
    if (LO && (LO->nshape || al0 > 0.f))
        for (int y = y0; y < y1; y++) {
            float py = y + .5f;
            if (!(y & 3))
                g7_poll();
            int touch = shapes_touch_row(LO, py);
            if (OAPX[y] && !(touch && motion_over) && x0 == 0 && x1 == GW) {
                OAPX[y] = 0;        /* drawn exactly below */
                noapx--;
            }
            if (!touch && al0 <= 0.f)
                continue;
            if (touch && motion_over) {
                /* (A, al) bilinear from the two half rows around y, then the premultiplied store */
                float cy = (float)y * .5f - .25f;
                int j0 = g_ifloor(cy);
                float fy = cy - (float)j0;
                int ia = ((x0 - 1) >> 1), ib = ((x1 - 2) >> 1) + 1, m = ib - ia + 1;
                if (!OAPX[y]) {
                    OAPX[y] = 1;
                    noapx++;
                }
                const struct hrow *A = half_over_row(j0 < 0 ? 0 : j0, ia, m, al0),
                                  *B = half_over_row(j0 + 1 > WH - 1 ? WH - 1 : j0 + 1, ia, m, al0);
                half_fill(A, B, fy, ia, m, x0, x1);
            } else {
                for (int i = 0; i < n; i++) {
                    ACC[3 * i] = 0.f;
                    ACC[3 * i + 1] = .004f;
                    ACC[3 * i + 2] = .014f;
                    AL[i] = al0;
                }
                if (touch) {
                    gw_row_a5(y, x0, x1, RA5);
                    gw_row_b(y, x0, x1, RB);
                    gs_row(1, &COVER, py, x0 + .5f, n, ACC, AL, RA5, RB, 0, G7_MAX_SHAPES);
                }

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

static int motion_half;             /* this frame's glass moves: shade it at half resolution */
int g7_idle(void);
static void compose(int x0, int y0, int x1, int y1)
{
    uint32_t *canvas = (uint32_t *)G7_CANVAS;
    if (frozen)
        compose_frozen(canvas, x0, y0, x1, y1);
    else
        compose_base(canvas, x0, y0, x1, y1, 1, motion_half);
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

/* box blur of a premultiplied 720 x 1480 buffer, radius r, zero outside; src -> dst. All four
 * channels at once: a pixel's bytes widened to 16-bit lanes (sums <= 27 x 255), the mean rounded
 * through a 16.16 reciprocal. */
static inline uint8x8_t box_div(uint16x8_t sum, uint32_t w)
{
    uint32x4_t m = vdupq_n_u32((65536U + w - 1U) / w), hw = vdupq_n_u32(w / 2U);
    uint32x4_t lo = vmulq_u32(vaddw_u16(hw, vget_low_u16(sum)), m), hi = vmulq_u32(vaddw_u16(hw, vget_high_u16(sum)), m);
    return vmovn_u16(vcombine_u16(vshrn_n_u32(lo, 16), vshrn_n_u32(hi, 16)));
}
static void box_h(const uint32_t *src, uint32_t *dst, int r)
{
    uint32_t w = 2U * (uint32_t)r + 1U;
    for (int y = 0; y < WH; y++) {
        const uint32_t *s = src + (unsigned)y * WW;
        if (!(y & 15))
            g7_poll();
        uint32_t *d = dst + (unsigned)y * WW;
        uint16x4_t sum = vdup_n_u16(0);
        for (int x = 0; x < r && x < WW; x++)
            sum = vadd_u16(sum, vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(s[x])))));
        for (int x = 0; x < WW; x++) {
            if (x + r < WW)
                sum = vadd_u16(sum, vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(s[x + r])))));
            uint8x8_t o = box_div(vcombine_u16(sum, sum), w);
            d[x] = vget_lane_u32(vreinterpret_u32_u8(o), 0);
            if (x - r >= 0)
                sum = vsub_u16(sum, vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(s[x - r])))));
        }
    }
}
static void box_v(const uint32_t *src, uint32_t *dst, int r)
{
    static uint16_t sum[4 * WW];
    uint32_t w = 2U * (uint32_t)r + 1U;
    for (int i = 0; i < 4 * WW; i++)
        sum[i] = 0;
    for (int y = 0; y < r && y < WH; y++)
        for (int x = 0; x < WW; x += 2)
            vst1q_u16(sum + 4 * x, vaddw_u8(vld1q_u16(sum + 4 * x), vld1_u8((const uint8_t *)(src + (unsigned)y * WW + x))));
    for (int y = 0; y < WH; y++) {
        if (y + r < WH) {
            const uint8_t *p = (const uint8_t *)(src + (unsigned)(y + r) * WW);
            for (int x = 0; x < WW; x += 2)
                vst1q_u16(sum + 4 * x, vaddw_u8(vld1q_u16(sum + 4 * x), vld1_u8(p + 4 * x)));
        }
        uint8_t *d = (uint8_t *)(dst + (unsigned)y * WW);
        if (!(y & 15))
            g7_poll();
        for (int x = 0; x < WW; x += 2)
            vst1_u8(d + 4 * x, box_div(vld1q_u16(sum + 4 * x), w));
        if (y - r >= 0) {
            const uint8_t *p = (const uint8_t *)(src + (unsigned)(y - r) * WW);
            for (int x = 0; x < WW; x += 2)
                vst1q_u16(sum + 4 * x, vsubw_u8(vld1q_u16(sum + 4 * x), vld1_u8(p + 4 * x)));
        }
    }
}

static void freeze(void)
{
    uint64_t t0 = hw_ticks();
    if (base_ok && !bg8_ok) {       /* from the wallpaper texture, before the freeze reuses it */
        gw_base();
        bg8_ok = 1;
    }
    /* SHARP: the base layer as it is */
    compose_base((uint32_t *)G7_SHARP, 0, 0, GW, GH, 1, 0);
    /* SOFT: base glass at uSoft = 1, half resolution (K = 2), shapes zoomed .965 */
    struct gs_ctx cs = CBASE;
    cs.K = 2.f;
    cs.soft = 1.f;
    const float zoom = .965f;
    gs_prepare(2, LB ? LB->shape : 0, LB ? LB->nshape : 0, &cs, zoom);
    uint32_t *soft = (uint32_t *)P_SOFT;
    for (int j = 0; j < WH; j++) {
        float py = 2.f * j + 1.f;
        g7_poll();
        gw_row_b_at(py, WW, 1.f, 2.f, RB);
        for (int i = 0; i < WW; i++) {
            ACC[3 * i] = RB[3 * i] * cs.dim;
            ACC[3 * i + 1] = RB[3 * i + 1] * cs.dim;
            ACC[3 * i + 2] = RB[3 * i + 2] * cs.dim;
            AL[i] = 1.f;
        }
        gw_row_a5_at(py, WW, 1.f, 2.f, RA5);
        gs_row(2, &cs, py, 1.f, WW, ACC, AL, RA5, RB, 0, G7_MAX_SHAPES);
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
    gs_dither_init();
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
    uint64_t t1 = hw_ticks();
    gw_wall(&WALL_FINAL);
    uint64_t t2 = hw_ticks();
    gw_blur();
    uint64_t t3 = hw_ticks();
    /* BG8 waits for its first use: the intro rebuilds the wallpaper (and BG8) before it shows it
     * at dim .8, so only a boot straight to the launch card pays for it here (base_bg_row) */
    bg8_ok = 0;
    ST.bake_part[0] = (uint32_t)us_between(t0, t1);
    ST.bake_part[1] = (uint32_t)us_between(t1, t2);
    ST.bake_part[2] = (uint32_t)us_between(t2, t3);
    ST.bake_part[3] = (uint32_t)us_since(t3);
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
    glass_area = over_area = 0;
    int forced = first || (f->flags & G7_F_FULL), wide = 0;
    all_dirty = forced;
    LB = f->base;
    LO = f->over;
    /* the wallpaper (intro) */
    if (!gw_wall_same(&f->wall, &cur_wall)) {
        if (!gw_field_valid())
            gw_field();
        gw_wall(&f->wall);
        cur_wall = f->wall;
        base_ok = 0;
        wide = 1;
        bg8_ok = 0;
        if (gw_wall_same(&f->wall, &WALL_FINAL))
            base_ok = 1;            /* BG8 again at its first use */
        all_dirty = 1;
    }
    if (f->dim != cur_dim || f->lx != cur_lx || f->ly != cur_ly)
        all_dirty = wide = 1;
    if (all_dirty)                  /* first frame, G7_F_FULL, wallpaper, dim or light: GLB is stale */
        glb_inval_all();
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
        if (glb_soft != 0.f) {      /* SHARP is the base at soft 0 */
            glb_inval_all();
            glb_soft = 0.f;
        }
        gs_prepare(0, LB ? LB->shape : 0, LB ? LB->nshape : 0, &CBASE, 1.f);
        gs_word_prepare(&f->word, &CBASE);
        layer_diff_save(0, LB, 1);  /* GLB: only where this base's glass differs from the last */
        freeze();                   /* SHARP = GLB + the lens and items (GLB is in SHARP) */
        glb_inval_all();
        frozen = 1;
        all_dirty = 1;
    } else if (!want && frozen) {
        frozen = 0;
        all_dirty = 1;
    }
    if (f->back != cur_back)
        all_dirty = 1;
    /* soft = back for the base glass (only while it is below the freeze threshold) */
    if (!frozen) {
        CBASE.soft = f->back;
        if (glb_soft != CBASE.soft) {
            glb_inval_all();
            glb_soft = CBASE.soft;
            wide = 1;
        }
    }
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
        layer_diff_save(0, LB, 1);  /* always: GLB follows every change of the glass */
        /* a motion frame: the glass changed over more than an eighth of the panel (or all of it,
         * and this is not a first or forced frame) */
        motion_half = !forced && (wide || glass_area * 8U > (uint64_t)GW * GH);
        if (motion_half)
            gs_prepare(GS_HALF, LB ? LB->shape : 0, LB ? LB->nshape : 0, &CBASE, 1.f);
    } else {
        gs_word_prepare(&f->word, &CBASE);
    }
    gs_prepare(1, LO ? LO->shape : 0, LO ? LO->nshape : 0, &COVER, 1.f);
    layer_diff_save(1, LO, 1);
    /* the over glass moves over more than an eighth of the panel, or the backdrop fades */
    motion_over = !forced && LO && LO->nshape &&
                  (over_area * 8U > (uint64_t)GW * GH || (f->back != cur_back && over_area));
    if (motion_over)
        gs_prepare(GS_HALFO, LO->shape, LO->nshape, &COVER, 1.f);
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
        if (px >= (uint32_t)GW * GH / 8U)
            ST.full_us = (uint32_t)((uint64_t)ST.last_us * GW * GH / px);
    }
    ST.last_px = px;
    if (!motion_half && !motion_over && (napx || noapx))
        (void)g7_idle();            /* the glass rests: one band of the half-resolution rows, exact */
    return px;
}

/* Rest: the rows a motion frame drew at half resolution, again at full resolution, a band of
 * IDLE_ROWS at a time (a few ms of the A55), from the last frame's lists. 1: more to do. */
#define IDLE_ROWS 128
int g7_idle(void)
{
    if (!inited || !baked)
        return 0;
    const uint8_t *flag = 0;
    if (napx && !frozen && LB)
        flag = GAPX;
    else if (noapx)
        flag = OAPX;
    if (!flag)
        return 0;
    int y0 = 0;
    while (y0 < GH && !flag[y0])
        y0++;
    if (y0 >= GH) {
        if (flag == GAPX)
            napx = 0;
        else
            noapx = 0;
        return napx || noapx;
    }
    int y1 = y0 + IDLE_ROWS > GH ? GH : y0 + IDLE_ROWS;
    if (flag == GAPX)
        for (int y = y0; y < y1; y++)
            GV0[y] = GV1[y] = 0;    /* redraw the glass of these rows */
    int mh = motion_half, mo = motion_over;
    motion_half = motion_over = 0;
    compose(0, y0, GW, y1);
    present(0, y0, GW, y1);
    motion_half = mh;
    motion_over = mo;
    return napx || noapx;
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
