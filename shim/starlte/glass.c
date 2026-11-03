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
#ifdef M7_PREVIEW
#include <stdio.h>
#endif

#define G7_FB ((uint32_t *)(0xcc000000UL + G7_HOSTOFF))

/* preview builds: time per phase (h7 H7_PROF=1). 0 frame setup, 1 base glass, 2 base items,
 * 3 frozen crossfade, 4 over glass, 5 over items, 6 edge, 7 present, 8 freeze, 9 idle */
#ifdef M7_PREVIEW
uint64_t h7_ns(void);
uint64_t g7_prof[12];
#define PROF0 uint64_t prof_t = h7_ns()
#define PROF(i) do { uint64_t prof_n = h7_ns(); g7_prof[i] += prof_n - prof_t; prof_t = prof_n; } while (0)
#else
/* the device: the same phases in counter ticks (26 MHz), read by menu7's m7_report */
uint64_t g7_prof[12];
#define PROF0 uint64_t prof_t = hw_ticks()
#define PROF(i) do { uint64_t prof_n = hw_ticks(); g7_prof[i] += prof_n - prof_t; prof_t = prof_n; } while (0)
#endif

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
static struct g7_word frz_word;     /* the wordmark at the freeze (sharp_refine) */
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
/* Shifted rows (shift_apply): GOF, where the row's moving glass sits in GLB minus where it is (px,
 * |GOF| <= GOF_MAX: the shift takes the integer dy from where the glass sits, so rounding never adds
 * up; a horizontal edge row is shaded again at any offset), and GSN,
 * the shifts since the row was shaded (each adds the correction's quantisation and rim error; at
 * SHIFT_MAX the row is shaded again instead). GSN_MIXED: shaded in part over shifted content (two
 * offsets in one row), never a shift's source. */
#define SHIFT_MAX 4
#define GOF_MAX 1.001f
#define GSN_MIXED 255
static float GOF[GH];
static uint8_t GSN[GH];
static void glb_inval_all(void)
{
    for (int y = 0; y < GH; y++) {
        GV0[y] = GV1[y] = 0;
        GAPX[y] = 0;
        GOF[y] = 0.f;
        GSN[y] = 0;
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
    uint8_t mover;                  /* a GLB shape that only moved by this frame's shift (glb_shift) */
};
struct sext {                       /* a shape's entry, for the shift: all but y, and y */
    uint32_t nyh;
    float y, h;
    uint8_t clip;
};
static struct sext PSX[G7_MAX_SHAPES], CSX[G7_MAX_SHAPES];         /* base layer: previous, this frame */
struct oext {                       /* an over shape: all but its box, its box, and the box drawn */
    uint32_t ngh;
    float g[5], d[5];               /* x, y, w, h, r: this frame's, and as last drawn */
};
static struct oext POX[G7_MAX_SHAPES], COX[G7_MAX_SHAPES];
static uint64_t over_area;          /* this frame: the area of the over glass's changes */
static uint8_t AFF[GH];             /* rows a moving GLB shape touched (old or new place) */
static int sh_dy, sh_on;            /* this frame's shift (px; sh_on: there is one, maybe 0 px) */
static void dirty_e(const struct pent *e, const int16_t b[4])
{
    dirty_b(b);
    if (e->glass == 1 && e->mover == 1 && sh_on) {
        for (int y = b[1] < 0 ? 0 : b[1]; y < b[3] && y < GH; y++)
            AFF[y] = 1;
    } else if (e->glass == 1)
        glb_inval(b);
    else if (e->glass == 2 && b[2] > b[0] && b[3] > b[1])
        over_area += (uint64_t)(b[2] - b[0]) * (uint64_t)(b[3] - b[1]);
}
static struct pent PS[2][G7_MAX_SHAPES], PI[2][G7_MAX_ITEMS];       /* previous frame */
static int nps[2], npi[2];
static struct pent CS[G7_MAX_SHAPES], CI[G7_MAX_ITEMS];             /* this frame */
static int nshape_cur;
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
static uint32_t shape_hash_ny(const struct g7_shape *s)
{
    uint32_t h = 0x811c9dc5U ^ 0x5bd1e995U;
    h = hmix(h, s->flags);
    h = hf(h, s->x); h = hf(h, s->w); h = hf(h, s->h);
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
        if (cur[i].mover == 2)
            continue;               /* an over shape held where it was drawn (over_hold) */
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
        CS[n].mover = 0;
        if (li == 0) {
            CSX[n].nyh = shape_hash_ny(s) ^ (pre ? i * 0x9e3779b9U : 0);
            CSX[n].y = s->y;
            CSX[n].h = s->h;
            CSX[n].clip = (s->flags & G7_S_CLIP) != 0;
        } else {
            uint32_t h = 0x9747b28cU ^ i;
            h = hmix(h, s->flags);
            h = hf(h, s->a); h = hf(h, s->s);
            for (int k = 0; k < 4; k++)
                h = hf(h, s->tint[k]);
            COX[n].ngh = h;
            COX[n].g[0] = s->x; COX[n].g[1] = s->y; COX[n].g[2] = s->w; COX[n].g[3] = s->h; COX[n].g[4] = s->r;
            for (int k = 0; k < 5; k++)
                COX[n].d[k] = COX[n].g[k];
        }
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
        CI[n].mover = 0;
        CI[n].hash = b[2] > b[0] ? item_hash(l, it) : 0;
        for (int k = 0; k < 4; k++)
            CI[n].b[k] = b[k];
        n++;
    }
    *ni = n;
}

/* ---- moving frames: GLB rows shifted, with a frost correction ----
 *
 * When the base glass only moved (the list scrolls, the island makes room, the sections below an
 * opening one move down), every GLB row whose shapes all moved by the same dy is the GLB row dy
 * above it, shifted, instead of being shaded again. The glass refracts the wallpaper, which did not
 * move, so the row is corrected: where the glass lets the base through (T, the glass's
 * transmission, kept per pixel in TPL), by the change of BG8; where it covers it (1 - T), by the
 * change of the frost under it through shade()'s linear part (vibrancy, tint), on a 4 x 4 px grid
 * (the frost planes are 4 and 8 px per texel). Refraction and dispersion offsets at the rims are
 * not corrected: those pixels are off by up to a few 1/255 while the glass moves. Shifted rows are
 * marked like half-resolution ones (GAPX) and drawn exactly once the glass rests (g7_idle). */
#define TPL ((uint8_t *)P_SOFT)     /* the base glass's transmission x 255 per panel pixel (not frozen) */
_Static_assert((unsigned long)GW * GH <= PL_WALL_BYTES, "transmission plane overflows SOFT");
#define BG8P ((const uint32_t *)G7_BASE)
static float sh_dyf;
static int sh_clip;                 /* a moving shape is G7_S_CLIP: rows only where the viewport fade is 1 */
static int sh_allclip;              /* every moving shape is: rows where the fade is 0 do not change */
static uint8_t BLK[GH];             /* rows a shape that did not move touches (now, or dy below its old place) */
static uint8_t EDG[GH];             /* rows on a moving shape's straight top or bottom edge (where it is now) */

/* the shift: the dy (float) by which the most glass area moved with nothing else changed */
static void shift_detect(const struct pent *prev, int np, const struct sext *px, const struct pent *cur,
                         const struct sext *cx, int nc)
{
    float cand[G7_MAX_SHAPES];
    uint64_t area[G7_MAX_SHAPES];
    int nca = 0;
    sh_dy = sh_on = 0;
    for (int i = 0; i < nc; i++) {
        if (cur[i].glass != 1 || cur[i].b[2] <= cur[i].b[0])
            continue;
        for (int k = 0; k < np; k++) {
            if (prev[k].key != cur[i].key || prev[k].glass != 1 || prev[k].b[2] <= prev[k].b[0] ||
                px[k].nyh != cx[i].nyh || px[k].y == cx[i].y)
                continue;
            float d = cx[i].y - px[k].y;
            uint64_t a = (uint64_t)(cur[i].b[2] - cur[i].b[0]) * (uint64_t)(cur[i].b[3] - cur[i].b[1]);
            int j = 0;
            while (j < nca && g_abs(cand[j] - d) > .01f)
                j++;
            if (j == nca) {
                cand[nca] = d;
                area[nca++] = 0;
            }
            area[j] += a;
            break;
        }
    }
    int best = -1;
    for (int j = 0; j < nca; j++)
        if (best < 0 || area[j] > area[best])
            best = j;
#ifdef M7_PREVIEW
    {
        extern int h7_dbg;
        if (h7_dbg) {
            fprintf(stderr, "shift: %d cands:", nca);
            for (int j = 0; j < nca; j++)
                fprintf(stderr, " %.3f(%llu)", cand[j], (unsigned long long)area[j]);
            fprintf(stderr, "\n");
        }
    }
#endif
    if (best < 0)
        return;
    float d = cand[best];
    /* the dy: from where the glass sits in GLB (GOF of the largest moving shape's rows), not from
     * where it was, so each frame's rounding is taken back by the next */
    float g = 0.f;
    {
        uint64_t ba = 0;
        int bk = -1;
        for (int i = 0; i < nc; i++) {
            if (cur[i].glass != 1 || cur[i].b[2] <= cur[i].b[0])
                continue;
            for (int k = 0; k < np; k++)
                if (prev[k].key == cur[i].key && prev[k].glass == 1 && prev[k].b[2] > prev[k].b[0] &&
                    px[k].nyh == cx[i].nyh && g_abs(cx[i].y - px[k].y - d) <= .01f) {
                    uint64_t a = (uint64_t)(cur[i].b[2] - cur[i].b[0]) * (uint64_t)(cur[i].b[3] - cur[i].b[1]);
                    if (bk < 0 || a > ba) {
                        ba = a;
                        bk = k;
                    }
                    break;
                }
        }
        if (bk >= 0) {
            int y0 = prev[bk].b[1] < 0 ? 0 : prev[bk].b[1], y1 = prev[bk].b[3] > GH ? GH : prev[bk].b[3];
            for (int y = (y0 + y1) / 2, n = 0; n < y1 - y0; n++, y = y + 1 >= y1 ? y0 : y + 1)
                if (GV0[y] == 0 && GV1[y] == GW && GSN[y] != GSN_MIXED) {
                    g = GOF[y];
                    break;
                }
        }
    }
    int di = g_ifloor(d - g + .5f);
    if (di >= GH / 2 || di <= -GH / 2)
        return;
    sh_dyf = d;
    sh_dy = di;
    sh_on = 1;                      /* di = 0: a spring's sub-pixel tail, the rows stay */
    sh_clip = 0;
    sh_allclip = 1;
    for (int i = 0; i < nc; i++) {
        if (cur[i].glass != 1 || cur[i].b[2] <= cur[i].b[0])
            continue;
        for (int k = 0; k < np; k++)
            if (prev[k].key == cur[i].key && prev[k].glass == 1 && prev[k].b[2] > prev[k].b[0] &&
                px[k].nyh == cx[i].nyh && g_abs(cx[i].y - px[k].y - d) <= .01f) {
                ((struct pent *)cur)[i].mover = 1;
                ((struct pent *)prev)[k].mover = 1;
                sh_clip |= cx[i].clip | px[k].clip;
                sh_allclip &= cx[i].clip & px[k].clip;
                break;
            }
    }
}

/* the frost change under a moved row, on the 4 px grid: block row j (panel rows 4j .. 4j + 3, sampled
 * at 4j + 2, where A5's texel row j is), against the same point dy above. DFA / DFB: A5 / B. */
/* the shift's scratch rows live in the pool's table area, after the half-resolution rows (see
 * TAB_HALF; not .bss): DFA, DFB, DT, DT2, DBA, DBB (3 x AW floats each), then JDX (3 x GW int16) */
#define TAB_SHIFT (P_TAB + 0xF0000UL)
#define DFA ((float (*)[AW])(TAB_SHIFT))
#define DFB (DFA + 3)
#define DT (DFA + 6)
#define DT2 (DFA + 9)
#define DBA (DFA + 12)
#define DBB (DFA + 15)
#define JDX ((int16_t (*)[GW])(TAB_SHIFT + 18UL * AW * 4UL))
_Static_assert(0xF0000UL + 18UL * AW * 4UL + 3UL * GW * 2UL <= 0x100000UL, "glass shift scratch overflow");
static int df_j = -1, df_dy, df_b;
static void a5_row_at(float py, float out[3][AW], int ia, int ib)
{
    float cy = py * .25f - .5f;
    int j0 = g_ifloor(cy);
    float f = cy - (float)j0;
    int ja = j0 < 0 ? 0 : j0 >= AH ? AH - 1 : j0, jb = j0 + 1 < 0 ? 0 : j0 + 1 >= AH ? AH - 1 : j0 + 1;
    const uint32_t *A = (const uint32_t *)P_A5 + (unsigned)ja * AW, *B = (const uint32_t *)P_A5 + (unsigned)jb * AW;
    const uint32x4_t m = vdupq_n_u32(1023U);
    const float32x4_t k = vdupq_n_f32(1.f / 1023.f), vf = vdupq_n_f32(f);
    for (int i = ia & ~3; i < ib; i += 4) {
        uint32x4_t a = vld1q_u32(A + i), b = vld1q_u32(B + i);
        for (int c = 0; c < 3; c++) {
            int sh = 20 - 10 * c;
            float32x4_t va = vcvtq_f32_u32(vandq_u32(vshlq_u32(a, vdupq_n_s32(-sh)), m));
            float32x4_t vb = vcvtq_f32_u32(vandq_u32(vshlq_u32(b, vdupq_n_s32(-sh)), m));
            vst1q_f32(out[c] + i, vmulq_f32(vfmaq_f32(va, vsubq_f32(vb, va), vf), k));
        }
    }
}
static void b_row_at(float py, float out[3][AW], int ia, int ib)
{
    /* B at panel (4i + 2, py): texel coordinates ((4i + 2) / 8 - .5, py / 8 - .5) */
    float cy = py * .125f - .5f;
    int j0 = g_ifloor(cy);
    float f = cy - (float)j0;
    int ja = j0 < 0 ? 0 : j0 >= BH ? BH - 1 : j0, jb = j0 + 1 < 0 ? 0 : j0 + 1 >= BH ? BH - 1 : j0 + 1;
    const uint32_t *A = (const uint32_t *)P_B + (unsigned)ja * BW, *B = (const uint32_t *)P_B + (unsigned)jb * BW;
    static float v[3][BW];
    for (int t = 0; t < BW; t++) {
        uint32_t a = A[t], b = B[t];
        v[0][t] = ((float)CR(a) + ((float)CR(b) - (float)CR(a)) * f) * (1.f / 255.f);
        v[1][t] = ((float)CG(a) + ((float)CG(b) - (float)CG(a)) * f) * (1.f / 255.f);
        v[2][t] = ((float)CB(a) + ((float)CB(b) - (float)CB(a)) * f) * (1.f / 255.f);
    }
    for (int i = ia; i < ib; i++) {
        /* even i = 2m: cx = m - .25; odd i = 2m + 1: cx = m + .25 */
        int m = i >> 1, t0 = (i & 1) ? m : m - 1;
        float fx = (i & 1) ? .25f : .75f;
        int ta = t0 < 0 ? 0 : t0, tb = t0 + 1 >= BW ? BW - 1 : t0 + 1;
        for (int c = 0; c < 3; c++)
            out[c][i] = v[c][ta] + (v[c][tb] - v[c][ta]) * fx;
    }
}
static int df_ia, df_ib;
static void df_rows(int j, int dy, int needb, int ia, int ib)
{
    if (df_j == j && df_dy == dy && (df_b || !needb) && ia >= df_ia && ib <= df_ib)
        return;
    float py = 4.f * (float)j + 2.f;
    if (df_j != j || df_dy != dy || ia < df_ia || ib > df_ib) {
        a5_row_at(py, DFA, ia, ib);
        a5_row_at(py - (float)dy, DT, ia, ib);
        for (int c = 0; c < 3; c++)
            for (int i = ia; i < ib; i++)
                DFA[c][i] -= DT[c][i];
        df_b = 0;
    }
    if (needb && !df_b) {
        b_row_at(py, DFB, ia, ib);
        b_row_at(py - (float)dy, DT, ia, ib);
        for (int c = 0; c < 3; c++)
            for (int i = ia; i < ib; i++)
                DFB[c][i] -= DT[c][i];
        df_b = 1;
    }
    df_j = j;
    df_dy = dy;
    df_ia = ia;
    df_ib = ib;
}

/* GLB row y = GLB row y - dy moved down by dy (TPL with it), corrected (above). y - dy is read before
 * anything writes it (the caller's order). */
/* JDX: this row's frost correction per pixel, in 1/8 of a unit (x 255) */
static int jd_ia, jd_ib;            /* the blocks JDX holds non-zero values for */
static struct gs_cls CLS[2 * G7_MAX_SHAPES];
static uint32_t jd_mask;
static int jd_j = -1;
static void glb_shift_row(int y, int dy)
{
    int s = y - dy;
    const uint32_t *src = GLB + (unsigned)s * GW, *b1 = BG8P + (unsigned)y * GW, *b0 = BG8P + (unsigned)s * GW;
    uint32_t *dst = GLB + (unsigned)y * GW;
    const uint8_t *ts = TPL + (unsigned)s * GW;
    uint8_t *td = TPL + (unsigned)y * GW;
    float py = (float)y + .5f;
    int npre = gs_npre(0);
    uint32_t mask = gs_class_mask(0, py, npre);
    if (mask != jd_mask || (y >> 2) != jd_j) {
        /* the correction changes only with the 4-row block and the shapes the row crosses */
        jd_mask = mask;
        jd_j = y >> 2;
        int nc = mask ? gs_classes(0, py, npre, CLS, 2 * G7_MAX_SHAPES) : 0;
        int needb = 0, ia = AW, ib = 0;
        for (int k = 0; k < nc; k++) {
            needb |= CLS[k].mixf > 0.f || (CLS[k].has_u && CLS[k].umixf > 0.f);
            int i0 = (CLS[k].x0 + 1) >> 2, i1 = (CLS[k].x1 + 1) >> 2;
            if (i0 < ia) ia = i0;
            if (i1 > ib) ib = i1;
        }
        if (ib > AW)
            ib = AW;
        /* clear what the last rows set outside this row's span */
        for (int c = 0; c < 3; c++) {
            for (int x = 4 * jd_ia; x < 4 * ia && x < 4 * jd_ib; x++)
                JDX[c][x] = 0;
            for (int x = 4 * (ib > jd_ia ? ib : jd_ia); x < 4 * jd_ib; x++)
                JDX[c][x] = 0;
        }
        jd_ia = ia;
        jd_ib = ib;
        if (nc)
            df_rows(y >> 2, dy, needb, ia, ib);
        int at = ia, band = 0;
        for (int k = 0; k < nc; k++) {
            const struct gs_cls *C = &CLS[k];
            int i0 = (C->x0 + 1) >> 2, i1 = (C->x1 + 1) >> 2;    /* blocks whose centre 4i + 2 is in [x0, x1) */
            if (i1 > AW)
                i1 = AW;
            for (; at < i0; at++)                               /* gaps between spans */
                for (int c = 0; c < 3; c++)
                    JDX[c][4 * at] = JDX[c][4 * at + 1] = JDX[c][4 * at + 2] = JDX[c][4 * at + 3] = 0;
            float m = C->mixf, om = 1.f - m;
            int ba = i1, bb = i1;
            if (C->oy != 0.f) {
                /* the straight band refracts the row oy away: its frost change there */
                ba = (C->bx0 + 1) >> 2;
                bb = (C->bx1 + 1) >> 2;
                if (ba < i0) ba = i0;
                if (bb > i1) bb = i1;
                if (ba < bb) {
                    a5_row_at(py + C->oy, DBA, ba, bb);
                    a5_row_at(py + C->oy - (float)dy, DT, ba, bb);
                    if (m > 0.f) {
                        b_row_at(py + C->oy, DBB, ba, bb);
                        b_row_at(py + C->oy - (float)dy, DT2, ba, bb);
                    }
                }
                band = 1;
            }
            /* a shape on another (a tile on its card): per pixel cov x its J + (1 - cov) x the
             * one under it, so the box's corners outside the shape (and an alpha < 1) take the card's */
            static float COVB[GW + 4];
            int pu = C->has_u && i1 > i0;
            if (pu && gs_cover_row(0, C->si, py, 4 * i0, 4 * i1, COVB))
                band = 1;           /* (a corner row: the coverage changes from row to row) */
            for (int i = i0; i < i1; i++) {
                float f[3], fu[3];
                for (int c = 0; c < 3; c++)
                    fu[c] = (DFA[c][i] * om + (m > 0.f ? DFB[c][i] * m : 0.f)) * (255.f * 8.f);
                if (i >= ba && i < bb)
                    for (int c = 0; c < 3; c++)
                        f[c] = ((DBA[c][i] - DT[c][i]) * om + (m > 0.f ? (DBB[c][i] - DT2[c][i]) * m : 0.f)) * (255.f * 8.f);
                else
                    for (int c = 0; c < 3; c++)
                        f[c] = fu[c];
                if (pu) {
                    float um = C->umixf, uom = 1.f - um;
                    for (int c = 0; c < 3; c++)
                        fu[c] = (DFA[c][i] * uom + (um > 0.f ? DFB[c][i] * um : 0.f)) * (255.f * 8.f);
                }
                for (int c = 0; c < 3; c++) {
                    float v = g_clamp(C->J[3 * c] * f[0] + C->J[3 * c + 1] * f[1] + C->J[3 * c + 2] * f[2], -32000.f, 32000.f);
                    if (!pu) {
                        int16_t q = (int16_t)(v < 0.f ? v - .5f : v + .5f);
                        JDX[c][4 * i] = JDX[c][4 * i + 1] = JDX[c][4 * i + 2] = JDX[c][4 * i + 3] = q;
                        continue;
                    }
                    float u = g_clamp(C->uJ[3 * c] * fu[0] + C->uJ[3 * c + 1] * fu[1] + C->uJ[3 * c + 2] * fu[2], -32000.f, 32000.f);
                    for (int k = 0; k < 4; k++) {
                        float w = u + (v - u) * COVB[4 * (i - i0) + k];
                        JDX[c][4 * i + k] = (int16_t)(w < 0.f ? w - .5f : w + .5f);
                    }
                }
            }
            if (i1 > at)
                at = i1;
        }
        if (band)
            jd_j = -1;              /* the band's offset changes from row to row */
    }
#ifdef M7_PREVIEW
    {
        extern int h7_dbg;
        if (h7_dbg == 5 || h7_dbg == 6) {      /* the frost correction's test: 5 none, 6 BG8 only */
            for (int x = 0; x < GW; x++) {
                if (h7_dbg == 5) {
                    dst[x] = src[x];
                } else {
                    float t = (float)ts[x] / 255.f;
                    uint32_t o = 0xff000000U;
                    for (int sh = 0; sh < 24; sh += 8) {
                        float v = (float)((src[x] >> sh) & 255U) + t * ((float)((b1[x] >> sh) & 255U) - (float)((b0[x] >> sh) & 255U)) + .5f;
                        o |= g_q255(v) << sh;
                    }
                    dst[x] = o;
                }
                td[x] = ts[x];
            }
            return;
        }
    }
#endif
    /* 16 pixels a step, in 16-bit lanes: out = g + d T / 256 + JD (255 - T) / 256 (JD in 1/8) */
    for (int x = 0; x < GW; x += 16) {
        uint8x16x4_t g = vld4q_u8((const uint8_t *)(src + x)), n = vld4q_u8((const uint8_t *)(b1 + x)),
                     o = vld4q_u8((const uint8_t *)(b0 + x)), r;
        uint8x16_t t = vld1q_u8(ts + x);
        vst1q_u8(td + x, t);
        int16x8_t tb[2], wb[2];
        tb[0] = vreinterpretq_s16_u16(vshll_n_u8(vget_low_u8(t), 7));
        tb[1] = vreinterpretq_s16_u16(vshll_n_u8(vget_high_u8(t), 7));
        uint8x16_t it = vmvnq_u8(t);
        wb[0] = vreinterpretq_s16_u16(vshll_n_u8(vget_low_u8(it), 7));
        wb[1] = vreinterpretq_s16_u16(vshll_n_u8(vget_high_u8(it), 7));
        for (int c = 0; c < 3; c++) {          /* byte plane c: B, G, R = JDX[2 - c] */
            const int16_t *j = JDX[2 - c] + x;
            int16x8_t v[2];
            for (int h = 0; h < 2; h++) {
                uint8x8_t gn = h ? vget_high_u8(n.val[c]) : vget_low_u8(n.val[c]);
                uint8x8_t go = h ? vget_high_u8(o.val[c]) : vget_low_u8(o.val[c]);
                uint8x8_t gg = h ? vget_high_u8(g.val[c]) : vget_low_u8(g.val[c]);
                int16x8_t d = vreinterpretq_s16_u16(vsubl_u8(gn, go));
                int16x8_t c1 = vqrdmulhq_s16(d, tb[h]);
                int16x8_t c2 = vrshrq_n_s16(vqrdmulhq_s16(vld1q_s16(j + 8 * h), wb[h]), 3);
                v[h] = vaddq_s16(vaddq_s16(vreinterpretq_s16_u16(vmovl_u8(gg)), c1), c2);
            }
            r.val[c] = vcombine_u8(vqmovun_s16(v[0]), vqmovun_s16(v[1]));
        }
        r.val[3] = vdupq_n_u8(255);
        vst4q_u8((uint8_t *)(dst + x), r);
    }
}

/* after the diff: the rows the moving shapes touched, shifted where nothing else is, the rest
 * invalidated (shaded again by compose_base) */
static void shift_apply(void)
{
    int dy = sh_dy;
    for (int y = 0; y < GH; y++)
        BLK[y] = EDG[y] = 0;
    /* a horizontal edge row is shaded again: with the content off by GOF the whole row is off by up
     * to half the edge's contrast, and its rim highlight depends on where the shape is */
    for (int i = 0; i < nshape_cur; i++) {
        if (CS[i].glass != 1 || CS[i].mover != 1)
            continue;
        for (int e = 0; e < 2; e++) {
            int ye = g_ifloor(CSX[i].y + (e ? CSX[i].h : 0.f));
            for (int y = ye - 1; y <= ye + 2; y++)
                if (y >= 0 && y < GH)
                    EDG[y] = 1;
        }
    }
    /* dy = 0 (a sub-pixel tail): a shape that did not move is where it was; one that changed
     * invalidated its rows in the diff (GV), which the source check below sees */
    for (int pass = 0; pass < 2 && dy; pass++) {
        const struct pent *e = pass ? PS[0] : CS;
        int n = pass ? nps[0] : nshape_cur;
        int off = pass ? dy : 0;
        for (int i = 0; i < n; i++) {
            if (e[i].glass != 1 || e[i].mover || e[i].b[2] <= e[i].b[0])
                continue;
            int a = e[i].b[1] + off, b = e[i].b[3] + off;
            for (int y = a < 0 ? 0 : a; y < b && y < GH; y++)
                BLK[y] = 1;
        }
    }
    int y0 = dy > 0 ? GH - 1 : 0, y1 = dy > 0 ? -1 : GH, st = dy > 0 ? -1 : 1;
    for (int y = y0; y != y1; y += st) {
        if (!AFF[y])
            continue;
        int s = y - dy;
        if (sh_allclip && (y < 170 || y >= 2620) && GV0[y] == 0 && GV1[y] == GW) {
            AFF[y] = 3;             /* outside the viewport's fade no moving glass shows: the row is as
                                     * it was (a shape that changed there invalidated it in the diff) */
            continue;
        }
        int ok = !BLK[y] && s >= 0 && s < GH && GV0[s] == 0 && GV1[s] == GW;
        if (ok && sh_clip && dy)    /* (a sub-pixel tail: the fade changes by nothing either) */
            ok = y >= 246 && y < 2480 && s >= 246 && s < 2480;
        /* the content's offset stays within GOF_MAX (the dy keeps the largest shape's rows within
         * .5 px; rows shaded since may be up to 1 px off it), and a row is shifted at most SHIFT_MAX
         * times (+ 0..3 by its 128-row band, so the rows due for shading come in turns) */
        float of = ok ? GOF[s] + (float)dy - sh_dyf : 0.f;
        if (ok && (EDG[y] || g_abs(of) > GOF_MAX || GSN[s] >= SHIFT_MAX + ((y >> 7) & 3)))
            ok = 0;
        if (!ok)
            continue;
        if (!(y & 7))
            g7_poll();
        GSN[y] = (uint8_t)(GSN[s] + (dy != 0));
        GOF[y] = of;
        if (dy)
            glb_shift_row(y, dy);
        GV0[y] = 0;
        GV1[y] = GW;
        if (!GAPX[y]) {
            GAPX[y] = 1;
            napx++;
        }
        AFF[y] = 2;                 /* shifted */
#ifdef M7_PREVIEW
        g7_prof[10]++;
#endif
    }
#ifdef M7_PREVIEW
    {
        extern int h7_dbg;
        if (h7_dbg) {
            int nb = 0, nv = 0, nz = 0, no = 0, r0 = -1, r1 = -1;
            for (int y = 0; y < GH; y++) {
                if (AFF[y] != 1)
                    continue;
                int sr = y - dy;
                if (r0 < 0) r0 = y;
                r1 = y;
                if (BLK[y]) nb++;
                else if (sr < 0 || sr >= GH) no++;
                else if (!(GV0[sr] == 0 && GV1[sr] == GW)) nv++;
                else nz++;
            }
            fprintf(stderr, "shift dy %d: blk %d oor %d gv %d zone %d (rows %d..%d)\n", dy, nb, no, nv, nz, r0, r1);
        }
    }
#endif
    /* rows the moving glass touched that could not be shifted: shaded again */
    for (int y = 0; y < GH; y++) {
        if (AFF[y] != 1)
            continue;
        int e = y;
        while (e < GH && AFF[e] == 1)
            e++;
        int16_t b[4] = { 0, (int16_t)y, GW, (int16_t)e };
        glb_inval(b);
#ifdef M7_PREVIEW
        g7_prof[11] += (uint64_t)(e - y);
#endif
        y = e - 1;
    }
    df_j = -1;
    jd_j = -1;
    jd_mask = 0xffffffffU;
}

static int shift_on;                /* this frame may shift GLB rows (set by g7_frame) */
static int forced_now;              /* this frame redraws everything (first, G7_F_FULL) */

/* An over shape whose box moved less than half a pixel from where it was last drawn (a spring's
 * tail: the launch card creeping to the panel's edges for half a second) is not drawn again: its
 * rows are marked like half-resolution ones (OAPX) and drawn exactly once the glass rests. */
static uint8_t OAPX[GH];            /* a panel row whose over glass was drawn at half resolution */
static int noapx;
static void over_hold(int ns)
{
    for (int i = 0; i < ns; i++) {
        if (CS[i].b[2] <= CS[i].b[0])
            continue;
        for (int k = 0; k < nps[1]; k++) {
            if (PS[1][k].key != CS[i].key)
                continue;
            if (PS[1][k].hash == CS[i].hash) {
                for (int j = 0; j < 5; j++)
                    COX[i].d[j] = POX[k].d[j];          /* unchanged since: drawn as it was */
                break;
            }
            if (POX[k].ngh != COX[i].ngh || PS[1][k].b[2] <= PS[1][k].b[0])
                break;
            int near = 1;
            for (int j = 0; j < 5; j++)
                near &= g_abs(COX[i].g[j] - POX[k].d[j]) < .5f;
            if (!near)
                break;
            CS[i].mover = 2;
            for (int j = 0; j < 5; j++)
                COX[i].d[j] = POX[k].d[j];
            int y0 = CS[i].b[1] < PS[1][k].b[1] ? CS[i].b[1] : PS[1][k].b[1];
            int y1 = CS[i].b[3] > PS[1][k].b[3] ? CS[i].b[3] : PS[1][k].b[3];
            for (int y = y0 < 0 ? 0 : y0; y < y1 && y < GH; y++)
                if (!OAPX[y]) {
                    OAPX[y] = 1;
                    noapx++;
                }
            break;
        }
    }
}
static void layer_diff_save(int li, const struct g7_layer *l, int check)
{
    int ns, ni;
    layer_entries(li, l, &ns, &ni);
    if (check) {
        sh_dy = sh_on = 0;
        if (li == 0 && shift_on)
            shift_detect(PS[0], nps[0], PSX, CS, CSX, ns);
        if (li == 0 && sh_on)
            for (int y = 0; y < GH; y++)
                AFF[y] = 0;
        if (li == 1 && !forced_now)
            over_hold(ns);
        nshape_cur = ns;
        diff(PS[li], nps[li], CS, ns);
        if (li == 0 && sh_on)
            shift_apply();
        diff(PI[li], npi[li], CI, ni);
    }
    if (li == 0)
        for (int i = 0; i < ns; i++)
            PSX[i] = CSX[i];
    else
        for (int i = 0; i < ns; i++)
            POX[i] = COX[i];
    /* mover is this frame's (shift_detect sets it again on both sides before diff reads it): a
     * stale flag would make the next frame's diff skip glb_inval for a shape that moved here */
    for (int i = 0; i < ns; i++) {
        PS[li][i] = CS[i];
        PS[li][i].mover = 0;
    }
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

/* the layer's dithered pixels d[0 .. n) (panel x x0 ..) as ACC (0 .. 1), AL = 0: the base layer is
 * opaque, so AL only counts the glass's cover, 1 - AL being its transmission (TPL) */
static void acc_from(const uint32_t *d, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t v = d[i];
        ACC[3 * i] = (float)CR(v) * (1.f / 255.f);
        ACC[3 * i + 1] = (float)CG(v) * (1.f / 255.f);
        ACC[3 * i + 2] = (float)CB(v) * (1.f / 255.f);
        AL[i] = 0.f;
    }
}
static void t_fill(uint8_t *t, int n)
{
    for (int i = 0; i < n; i++)
        t[i] = 255;
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
static void base_glass_row(uint32_t *d, int y, int x0, int x1, int s0, int s1, int wd, int wx0, int wx1, uint8_t *tp)
{
    float py = y + .5f;
    if (tp)
        t_fill(tp, x1 - x0);
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
        if (tp)
            for (int i = 0; i < m; i++)
                tp[sa - x0 + i] = (uint8_t)g_q255((1.f - AL[i]) * 255.f + .5f);
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
_Static_assert(0xDD000UL + 4UL * sizeof(struct hrow) + 6UL * (GW / 2 + 4) * 4UL + 4UL * (GW / 2 + 8) * 4UL <= 0xF0000UL,
               "glass half-row buffers overflow (TAB_SHIFT follows)");
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
static inline uint32_t half_px(int x, int ia, float Y, uint32_t v, uint8_t *tp)
{
    int k = ((x - 1) >> 1) - ia;
    float fx = (x & 1) ? .25f : .75f, h = g_dither(x, Y);
    float m = HV[3][k] + (HV[3][k + 1] - HV[3][k]) * fx;
    if (tp)
        *tp = (uint8_t)g_q255(m * 255.f + .5f);
    float r = HV[0][k] + (HV[0][k + 1] - HV[0][k]) * fx + m * (float)CR(v) + h;
    float g = HV[1][k] + (HV[1][k + 1] - HV[1][k]) * fx + m * (float)CG(v) + h;
    float b = HV[2][k] + (HV[2][k + 1] - HV[2][k]) * fx + m * (float)CB(v) + h;
    return 0xff000000U | g_q255(r) << 16 | g_q255(g) << 8 | g_q255(b);
}
static void base_glass_half(uint32_t *d, int y, int x0, int x1, int npre, uint8_t *tp)
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
    if (mn >= 1.f) {
        if (tp)
            t_fill(tp, x1 - x0);
        return;                     /* no glass on this row: d stays */
    }
    for (int c = 0; c < 4; c++)
        HV[c][n] = HV[c][n + 1] = HV[c][n - 1];
    float Y = gs_dither_y(y);
    int x = x0;
    while (x < x1 && (x & 1)) {
        d[x - x0] = half_px(x, ia, Y, d[x - x0], tp ? tp + (x - x0) : 0);
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
        if (tp) {
            uint32x4_t t0 = vcvtq_u32_f32(vfmaq_f32(vdupq_n_f32(.5f), v[3][0], vdupq_n_f32(255.f)));
            uint32x4_t t1 = vcvtq_u32_f32(vfmaq_f32(vdupq_n_f32(.5f), v[3][1], vdupq_n_f32(255.f)));
            uint16x8_t t16 = vcombine_u16(vqmovn_u32(t0), vqmovn_u32(t1));
            vst1_u8(tp + (x - x0), vqmovn_u16(t16));
        }
    }
    for (; x < x1; x++)
        d[x - x0] = half_px(x, ia, Y, d[x - x0], tp ? tp + (x - x0) : 0);
}

/* the base layer (glass and items) for the rect into dst (stride GW). glb: use and keep GLB;
 * half: rows that need their glass again get it at half resolution (a motion frame) */
static void compose_base(uint32_t *dst, int x0, int y0, int x1, int y1, int glb, int half)
{
    int n = x1 - x0, wy0 = 0, wy1 = 0, wx0 = 0, wx1 = 0;
    int word = gs_word_rows(&wy0, &wy1, &wx0, &wx1);
    int npre = LB ? gs_npre(0) : 0, nsh = LB ? gs_nshapes(0) : 0;
    PROF0;
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
            uint8_t *tp = glb && !frozen ? TPL + (unsigned)y * GW + x0 : 0;    /* (frozen: TPL is SOFT) */
            if (half)
                base_glass_half(d, y, x0, x1, npre, tp);
            else
                base_glass_row(d, y, x0, x1, 0, npre, 0, 0, 0, tp);
            if (glb) {
                uint32_t *g = GLB + (unsigned)y * GW + x0;
                if (g != d)
                    g_copy32(g, d, n);
                if ((x0 == 0 && x1 == GW) || GV1[y] <= GV0[y]) {
                    GOF[y] = 0.f;   /* the row's glass is all shaded where it is now */
                    GSN[y] = 0;
                } else if (GSN[y] || GOF[y] != 0.f) {
                    GSN[y] = GSN_MIXED;
                }
                glb_valid(y, x0, x1);
            }
            if (glb || dst == (uint32_t *)G7_CANVAS) {
                if (half && !GAPX[y]) {
                    GAPX[y] = 1;
                    napx++;
                } else if (!half && GAPX[y] && x0 == 0 && x1 == GW) {
                    GAPX[y] = 0;
                    napx--;
                }
            }
        }
        base_glass_row(d, y, x0, x1, npre, nsh, wd, wx0, wx1, 0);
    }
    PROF(1);
    if (LB) {
        struct gd_target t = { dst, GW, x0, y0, x1, y1, 1.f, 0.f, 0.f, 0 };
        gd_items(LB, (const int16_t (*)[4])BND[0], &t);
    }
    PROF(2);
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

static int motion_over;             /* the over glass moves: shade it at half resolution */

/* ---- the end screens' curtain ----
 *
 * The end screens fade a full-panel opaque rect (the curtain) in over everything else, which no
 * longer changes. Behind the launch card the base is frozen and back is 1, so the crossfade shows
 * SOFT only and G7_SHARP is free: the first frame of the curtain keeps the panel as drawn below it
 * there (CURT), and the next frames only blend the curtain over that copy and draw what follows it.
 * SHARP is then lost: back leaving 1 takes the base layer again (freeze). */
#define CURT ((uint32_t *)G7_SHARP)
static int curt_k = -1;             /* this frame: the curtain's item index, or -1 */
static int curt_cap, curt_use;      /* this frame keeps what is below it / draws from CURT */
static int curt_ok;                 /* CURT holds the panel below the curtain for curt_sig */
static uint32_t curt_sig;
static int sharp_lost;              /* SHARP holds CURT, not the frozen base */
static int curtain_index(const struct g7_layer *l)
{
    for (unsigned i = 0; l && i < l->nitem; i++) {
        const struct g7_item *it = &l->item[i];
        /* (a = 1 too: the fade's last frame stays a copy and a blend) */
        if (it->kind != G7_RECT || it->clip || it->flags || CA(it->color) != 255U || it->a <= 0.f || it->a > 1.f)
            continue;
        const struct g7_rect *q = &it->u.rect;
        if (q->r == 0.f && q->ring == 0.f && q->x <= 0.f && q->y <= 0.f && q->x + q->w >= (float)GW &&
            q->y + q->h >= (float)GH)
            return (int)i;
    }
    return -1;
}

/* a motion row of the over layer: (A, al) bilinear from the two half rows (as half_fill), composited
 * as gs_over_row does, in one pass, 8 pixels a NEON step */
static inline void over_px(uint32_t *d, int x, int ia, float Y)
{
    int k = ((x - 1) >> 1) - ia;
    float fx = (x & 1) ? .25f : .75f;
    float a = HV[3][k] + (HV[3][k + 1] - HV[3][k]) * fx;
    if (a <= 0.f)
        return;
    float h = (g_dither(x, Y) - .5f) * a, kk = 1.f - a;
    uint32_t v = *d;
    float c[3];
    for (int ch = 0; ch < 3; ch++)
        c[ch] = g_sat(HV[ch][k] + (HV[ch][k + 1] - HV[ch][k]) * fx);
    float r = c[0] * a * 255.f + h + (float)CR(v) * kk + .5f;
    float g = c[1] * a * 255.f + h + (float)CG(v) * kk + .5f;
    float b = c[2] * a * 255.f + h + (float)CB(v) * kk + .5f;
    *d = 0xff000000U | g_q255(r) << 16 | g_q255(g) << 8 | g_q255(b);
}
static void over_half_store(uint32_t *d, int y, int x0, int x1, const struct hrow *A, const struct hrow *B, float fy,
                            int ia, int m)
{
    const float32x4_t vfy = vdupq_n_f32(fy);
    int i = 0;
    for (; i + 4 <= m; i += 4) {
        float32x4_t ma = vld1q_f32(A->M + i), mb = vld1q_f32(B->M + i);
        vst1q_f32(HV[3] + i, vfmaq_f32(ma, vsubq_f32(mb, ma), vfy));
        float32x4x3_t ca = vld3q_f32(A->C + 3 * i), cb = vld3q_f32(B->C + 3 * i);
        for (int c = 0; c < 3; c++)
            vst1q_f32(HV[c] + i, vfmaq_f32(ca.val[c], vsubq_f32(cb.val[c], ca.val[c]), vfy));
    }
    for (; i < m; i++) {
        HV[3][i] = A->M[i] + (B->M[i] - A->M[i]) * fy;
        for (int c = 0; c < 3; c++)
            HV[c][i] = A->C[3 * i + c] + (B->C[3 * i + c] - A->C[3 * i + c]) * fy;
    }
    for (int c = 0; c < 4; c++)
        HV[c][m] = HV[c][m + 1] = HV[c][m - 1];
    float Y = gs_dither_y(y);
    int x = x0;
    for (; x < x1 && (x & 1); x++)
        over_px(d + (x - x0), x, ia, Y);
    const float32x4_t q1 = vdupq_n_f32(.25f), q3 = vdupq_n_f32(.75f), k255 = vdupq_n_f32(255.f),
                      zero = vdupq_n_f32(0.f), one = vdupq_n_f32(1.f), half = vdupq_n_f32(.5f);
    for (; x + 8 <= x1; x += 8) {
        int k = (x >> 1) - 1 - ia;
        float32x4_t v[4][2];
        for (int c = 0; c < 4; c++) {
            float32x4_t a = vld1q_f32(HV[c] + k), b = vld1q_f32(HV[c] + k + 1), e = vld1q_f32(HV[c] + k + 2);
            float32x4x2_t z = vzipq_f32(vfmaq_f32(vmulq_f32(a, q1), b, q3), vfmaq_f32(vmulq_f32(b, q3), e, q1));
            v[c][0] = z.val[0];
            v[c][1] = z.val[1];
        }
        for (int hh = 0; hh < 2; hh++) {
            uint32_t *p = d + (x - x0) + 4 * hh;
            float32x4_t a = v[3][hh];
            if (vmaxvq_f32(a) <= 0.f)
                continue;
            float32x4_t h = vfmaq_f32(half, vsubq_f32(g_dither4(x + 4 * hh, Y), half), a), kk = vsubq_f32(one, a);
            float32x4_t ka = vmulq_f32(a, k255), bg[3], o[3];
            g_unpack4(p, bg);
            for (int ch = 0; ch < 3; ch++) {
                float32x4_t cs = vminq_f32(vmaxq_f32(v[ch][hh], zero), one);
                o[ch] = vfmaq_f32(vfmaq_f32(h, cs, ka), bg[ch], kk);
            }
            uint32x4_t px = g_pack4(o[0], o[1], o[2]), keep = vcleq_f32(a, zero);
            vst1q_u32(p, vbslq_u32(keep, vld1q_u32(p), px));
        }
    }
    for (; x < x1; x++)
        over_px(d + (x - x0), x, ia, Y);
}

static void compose_over(uint32_t *dst, int x0, int y0, int x1, int y1)
{
    int n = x1 - x0;
    float al0 = COVER.back * .45f;
    PROF0;
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
                over_half_store(dst + (unsigned)y * GW + x0, y, x0, x1, A, B, fy, ia, m);
                continue;
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
    PROF(4);
    if (LO && LO->nitem) {
        struct gd_target t = { dst, GW, x0, y0, x1, y1, 1.f, 0.f, 0.f, 0 };
        if (curt_cap) {
            /* the first frame of a curtain: what is below it, kept (curtain_rows) */
            gd_items_range(LO, (const int16_t (*)[4])BND[1], &t, 0, (unsigned)curt_k);
            for (int y = y0; y < y1; y++)
                g_copy32(CURT + (unsigned)y * GW + x0, dst + (unsigned)y * GW + x0, n);
            gd_items_range(LO, (const int16_t (*)[4])BND[1], &t, (unsigned)curt_k, LO->nitem);
        } else {
            gd_items(LO, (const int16_t (*)[4])BND[1], &t);
        }
    }
    PROF(5);
}

/* a frame below an unchanged curtain: what is below it from CURT, then the curtain and what follows */
static void compose_curtain(uint32_t *dst, int x0, int y0, int x1, int y1)
{
    PROF0;
    for (int y = y0; y < y1; y++) {
        if (!(y & 63))
            g7_poll();
        g_copy32(dst + (unsigned)y * GW + x0, CURT + (unsigned)y * GW + x0, x1 - x0);
    }
    struct gd_target t = { dst, GW, x0, y0, x1, y1, 1.f, 0.f, 0.f, 0 };
    gd_items_range(LO, (const int16_t (*)[4])BND[1], &t, (unsigned)curt_k, LO->nitem);
    PROF(5);
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
    if (curt_use) {
        compose_curtain(canvas, x0, y0, x1, y1);
        PROF0;
        compose_edge(canvas, x0, y0, x1, y1);
        PROF(6);
        return;
    }
    if (frozen) {
        PROF0;
        compose_frozen(canvas, x0, y0, x1, y1);
        PROF(3);
    } else {
        /* GLB only for the final wallpaper at dim .8: in the intro every frame changes it all */
        compose_base(canvas, x0, y0, x1, y1, base_ok && CBASE.dim == .8f, motion_half);
    }
    compose_over(canvas, x0, y0, x1, y1);
    PROF0;
    compose_edge(canvas, x0, y0, x1, y1);
    PROF(6);
}

static void present(int x0, int y0, int x1, int y1)
{
    PROF0;
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
    PROF(7);
}

/* ---- the freeze (D4) ---- */

static uint8_t SAPX[GH];            /* a SHARP row taken from an approximate GLB row: shade it again */
static int nsapx;

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
    sharp_lost = 0;
    curt_ok = 0;
    if (base_ok && !bg8_ok) {       /* from the wallpaper texture, before the freeze reuses it */
        gw_base();
        bg8_ok = 1;
    }
    /* SHARP: the base layer as it is. GLB rows a motion frame left approximate (half resolution,
     * shifted) are taken as they are and shaded exactly afterwards, a band per g7_idle (SAPX) */
    nsapx = 0;
    for (int y = 0; y < GH; y++) {
        SAPX[y] = (uint8_t)(napx && GAPX[y] && GV1[y] > GV0[y]);
        nsapx += SAPX[y];
        GAPX[y] = 0;
    }
    napx = 0;
#ifdef M7_PREVIEW
    uint64_t fz0 = h7_ns();
    {
        extern int h7_dbg;
        int inv = 0;
        for (int y = 0; y < GH; y++)
            inv += !(GV0[y] == 0 && GV1[y] == GW);
        if (h7_dbg)
            fprintf(stderr, "freeze: %d rows not valid, %d approximate\n", inv, nsapx);
    }
#endif
    compose_base((uint32_t *)G7_SHARP, 0, 0, GW, GH, 1, 0);
#ifdef M7_PREVIEW
    uint64_t fz1 = h7_ns();
#endif
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
#ifdef M7_PREVIEW
    uint64_t fz2 = h7_ns();
#endif
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
#ifdef M7_PREVIEW
    {
        extern int h7_dbg;
        uint64_t fz3 = h7_ns();
        if (h7_dbg)
            fprintf(stderr, "freeze: sharp %.2f soft glass %.2f content %.2f ms\n", (fz1 - fz0) / 1e6, (fz2 - fz1) / 1e6,
                    (fz3 - fz2) / 1e6);
    }
#endif
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

static int16_t XR[2][4];             /* this frame's extra rects (back_dirty's side strips) */
static int nxr;
static void set4(int16_t *r, int x0, int y0, int x1, int y1)
{
    r[0] = (int16_t)x0;
    r[1] = (int16_t)y0;
    r[2] = (int16_t)x1;
    r[3] = (int16_t)y1;
}
/* back changed while frozen: it shows only where the over glass does not cover everything (al < 1).
 * Inside an over shape at full opacity, a pixel half the AA width inside its edge has cov = 1, so
 * al = 1 and the colour no longer depends on what is below: the largest such shape's box, inset by
 * its corner (the circle's 45 degree point covers the continuous corner too), is left alone. */
static void back_dirty(void)
{
    float best = 0.f, bx0 = 0.f, by0 = 0.f, bx1 = 0.f, by1 = 0.f;
    for (unsigned i = 0; LO && i < LO->nshape; i++) {
        const struct g7_shape *q = &LO->shape[i];
        if (q->a < 1.f || (q->flags & G7_S_LENS))
            continue;
        float in = q->r * 1.18f * .2929f + 3.f;
        float x0 = q->x + in, y0 = q->y + in, x1 = q->x + q->w - in, y1 = q->y + q->h - in;
        if (x1 - x0 < 64.f || y1 - y0 < 64.f || (x1 - x0) * (y1 - y0) <= best)
            continue;
        best = (x1 - x0) * (y1 - y0);
        bx0 = x0;
        by0 = y0;
        bx1 = x1;
        by1 = y1;
    }
    if (best <= 0.f) {
        all_dirty = 1;
        return;
    }
    int a = (int)__builtin_ceilf(bx0), b = g_ifloor(bx1), c = (int)__builtin_ceilf(by0), d = g_ifloor(by1);
    dirty(0, 0, GW, c);
    dirty(0, d, GW, GH);
    /* the side strips beside the core are drawn on their own: as dirty rects they would merge with
     * anything between them into the whole width */
    nxr = 0;
    if (a > 0 && c < d)
        set4(XR[nxr++], 0, c, a < GW ? a : GW, d);
    if (b < GW && c < d)
        set4(XR[nxr++], b > 0 ? b : 0, c, GW, d);
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
    uint32_t froze_us = 0;
    nxr = 0;
    forced_now = first || (f->flags & G7_F_FULL);
    PROF0;
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
    /* back while frozen: the crossfade's weight is in 256ths and the backdrop is .45 back, so a
     * change under 1/256 moves no pixel by more than one unit; such a step (a spring's tail) keeps
     * the drawn value, and back reaching 1 is drawn exactly */
    float bk = f->back;
    if (frozen && !forced && !(f->flags & G7_F_REFREEZE) && bk > .002f && bk != 1.f && cur_back > .002f &&
        g_abs(bk - cur_back) < 1.f / 256.f)
        bk = cur_back;
    CBASE.K = 1.f;
    CBASE.soft = 0.f;
    CBASE.dim = f->dim;
    CBASE.back = bk;
    CBASE.opaque = 1;
    CBASE.lx = f->lx;
    CBASE.ly = f->ly;
    CBASE.Ldx = Ldx / ln;
    CBASE.Ldy = Ldy / ln;
    COVER = CBASE;
    COVER.opaque = 0;
    COVER.back = bk;
    back_now = bk;
    int want = bk > .002f, was_frozen = frozen;
    if (want && (!frozen || (f->flags & G7_F_REFREEZE) || (sharp_lost && bk < 1.f))) {
        frozen = 0;
        if (glb_soft != 0.f) {      /* SHARP is the base at soft 0 */
            glb_inval_all();
            glb_soft = 0.f;
        }
        gs_prepare(0, LB ? LB->shape : 0, LB ? LB->nshape : 0, &CBASE, 1.f);
        gs_word_prepare(&f->word, &CBASE);
        /* GLB: only where this base's glass differs from the last (moved rows shifted, SAPX) */
        shift_on = !forced && !all_dirty && base_ok && bg8_ok && CBASE.dim == .8f && f->word.ta <= .001f;
        layer_diff_save(0, LB, 1);
        shift_on = 0;
        PROF0;
        frz_word = f->word;
        freeze();                   /* SHARP = GLB + the lens and items (GLB is in SHARP) */
        PROF(8);                    /* (its compose_base counts in 1 and 2 as well) */
        froze_us = ST.freeze_us;
        glb_inval_all();
        frozen = 1;
        all_dirty = 1;
    } else if (!want && frozen) {
        frozen = 0;
        all_dirty = 1;
        sharp_lost = 0;             /* (GLB is invalid since the freeze) */
        curt_ok = 0;
    }
    if (bk != cur_back) {
        if (was_frozen && frozen && !all_dirty)
            back_dirty();           /* only where the over layer lets the backdrop through */
        else
            all_dirty = 1;
    }
    /* soft = back for the base glass below the freeze threshold: back <= .002 there, which softens
     * nothing by a unit (aa + .06 px, light - .2 %), so it stays 0 and an overlay opening keeps GLB */
    if (!frozen) {
        CBASE.soft = 0.f;
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
        /* moving frames shift GLB rows (glb_shift_row) where the glass only moved */
        shift_on = !forced && !all_dirty && base_ok && bg8_ok && CBASE.dim == .8f && glb_soft == 0.f &&
                   f->word.ta <= .001f && !(f->flags & G7_F_REFREEZE);
        layer_diff_save(0, LB, 1);  /* always: GLB follows every change of the glass */
        shift_on = 0;
        /* a motion frame: the glass changed over more than an eighth of the panel (or all of it,
         * and this is not a first or forced frame) */
        /* a shifting frame too: the rows it shades again (edges, rows due after SHIFT_MAX, rows a
         * still shape blocks) are as approximate as the shifted ones and drawn exactly at rest */
        motion_half = !forced && (wide || glass_area * 8U > (uint64_t)GW * GH || (sh_on && sh_dy != 0));
        if (motion_half)
            gs_prepare(GS_HALF, LB ? LB->shape : 0, LB ? LB->nshape : 0, &CBASE, 1.f);
    } else {
        gs_word_prepare(&f->word, &CBASE);
        motion_half = 0;            /* frozen: the base does not move (the rest check below) */
    }
    gs_prepare(1, LO ? LO->shape : 0, LO ? LO->nshape : 0, &COVER, 1.f);
    layer_diff_save(1, LO, 1);
    /* the over glass moves over more than an eighth of the panel, or the backdrop fades */
    motion_over = !forced && LO && LO->nshape &&
                  (over_area * 8U > (uint64_t)GW * GH || bk != cur_back);
    if (motion_over)
        gs_prepare(GS_HALFO, LO->shape, LO->nshape, &COVER, 1.f);
    /* an end screen's curtain over an unchanged panel (frozen, back 1) */
    curt_k = frozen && bk == 1.f ? curtain_index(LO) : -1;
    curt_cap = curt_use = 0;
    if (curt_k >= 0) {
        uint32_t sig = hmix(2166136261U, (uint32_t)curt_k);
        for (int i = 0; i < nps[1]; i++)
            sig = hmix(hmix(sig, PS[1][i].key), PS[1][i].hash);
        for (int i = 0; i < curt_k && i < npi[1]; i++)
            sig = hmix(hmix(sig, PI[1][i].key), PI[1][i].hash);
        if (curt_ok && sig == curt_sig && !forced)
            curt_use = 1;
        else if (!motion_over) {
            curt_cap = 1;
            curt_sig = sig;
            all_dirty = 1;          /* CURT is the whole panel */
        }
    } else {
        curt_ok = 0;
    }
    cur_word = f->word;
    cur_dim = f->dim;
    cur_back = bk;
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
    PROF(0);
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
    if (!all_dirty)
        for (int i = 0; i < nxr; i++) {
            compose(XR[i][0], XR[i][1], XR[i][2], XR[i][3]);
            present(XR[i][0], XR[i][1], XR[i][2], XR[i][3]);
            px += (uint32_t)(XR[i][2] - XR[i][0]) * (uint32_t)(XR[i][3] - XR[i][1]);
        }
    nxr = 0;
    if (curt_cap) {
        curt_ok = 1;
        sharp_lost = 1;
        curt_cap = 0;
    }
    if (px) {
        ST.frames++;
        ST.last_us = (uint32_t)us_since(t0);
        if (ST.last_us > ST.worst_us)
            ST.worst_us = ST.last_us;
        /* a full frame's cost without this frame's freeze (once per overlay: not a frame rate) */
        if (px >= (uint32_t)GW * GH / 8U)
            ST.full_us = (uint32_t)((uint64_t)(ST.last_us > froze_us ? ST.last_us - froze_us : 0) * GW * GH / px);
    }
    ST.last_px = px;
    if (!motion_half && !motion_over && (napx || noapx || (frozen && nsapx)))
        (void)g7_idle();            /* the glass rests: one band of the half-resolution rows, exact */
    return px;
}

/* Rest: the rows a motion frame drew at half resolution, again at full resolution, a band of
 * IDLE_ROWS at a time (a few ms of the A55), from the last frame's lists. 1: more to do. */
#define IDLE_ROWS 128
/* frozen: the next band of SHARP rows taken approximate, shaded exactly, if the base layer is still
 * the one frozen (its entries and the wordmark equal the freeze's). Else a band of the new base would
 * tear the backdrop: the approximate rows stay until the overlay closes (a freeze again would be
 * exact but costs a full shade, ~95 ms VM, in the middle of an overlay's motion: menu7's list spring
 * keeps moving under the options menu; the freeze-time items are gone, their strings live only
 * during g7_frame). Since the shift keeps its rows within .5 px and SHIFT_MAX shifts, they are as
 * close as the half-resolution rows. */
static int sharp_refine(void)
{
    int ns, ni;
    layer_entries(0, LB, &ns, &ni);
    int same = ns == nps[0] && ni == npi[0] && word_same(&frz_word, &cur_word);
    for (int i = 0; same && i < ns; i++)
        same = CS[i].key == PS[0][i].key && CS[i].hash == PS[0][i].hash;
    for (int i = 0; same && i < ni; i++)
        same = CI[i].key == PI[0][i].key && CI[i].hash == PI[0][i].hash;
    if (!same) {
        for (int y = 0; y < GH; y++)
            SAPX[y] = 0;
        nsapx = 0;
        return 0;
    }
    int y0 = 0;
    while (y0 < GH && !SAPX[y0])
        y0++;
    if (y0 >= GH) {
        nsapx = 0;
        return 0;
    }
    int y1 = y0 + IDLE_ROWS > GH ? GH : y0 + IDLE_ROWS;
    for (int y = y0; y < y1; y++) {
        GV0[y] = GV1[y] = 0;
        if (SAPX[y]) {
            SAPX[y] = 0;
            nsapx--;
        }
    }
    hr_reset();
    compose_base((uint32_t *)G7_SHARP, 0, y0, GW, y1, 1, 0);
    for (int y = y0; y < y1; y++)
        GV0[y] = GV1[y] = 0;        /* SHARP holds the lens and the items too: never GLB */
    int mh = motion_half, mo = motion_over;
    motion_half = motion_over = 0;
    compose(0, y0, GW, y1);
    present(0, y0, GW, y1);
    motion_half = mh;
    motion_over = mo;
    return nsapx > 0;
}

int g7_idle(void)
{
    if (!inited || !baked)
        return 0;
    if (frozen && nsapx > 0 && LB && !sharp_lost && !curt_use)
        return sharp_refine() || noapx;
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
