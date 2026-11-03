/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 glass renderer: private declarations shared by glass.c and glass_*.c.
 * Not an interface: only the glass files include it (the interface is glass.h).
 *
 * Files:
 *   glass.c        the API: init, frame diff and dirty rects, compose driver, freeze, present
 *   glass_wall.c   WALL_FS (the static field, the wallpaper for any struct g7_wall), the four
 *                  BLUR_FS passes of renderWall into blur A and B, the frost plane A5, BASE
 *   glass_shade.c  GLASS_FS: shapes (sdSq, shadow, coverage, shade(), lens), the glass wordmark,
 *                  the dither, one row at a time
 *   glass_draw.c   the content items (text, images, tiles, rects, shadows, arcs, strokes), the
 *                  image and tile cache (G7_CACHE)
 *
 * Built with FP/SIMD (CFLAGS_V7). No libm: the transcendental functions below are polynomials
 * accurate to ~1e-7 relative (enough for 8-bit output); sqrt is fsqrt.
 */
#pragma once
#include <stdint.h>
#include "glass.h"
#include "sb7.h"

uint64_t hw_ticks(void);
uint64_t hw_freq(void);

/* ---- planes ---- */
#define GW G7_W
#define GH G7_H
#define WW 720                      /* WALL_FS's texture (renderWall W x H) */
#define WH 1480
#define AW 360                      /* blur A */
#define AH 740
#define BW 180                      /* blur B */
#define BH 370

/* G7_POOL layout (18 MiB). Sizes in bytes. */
#define PL_WALL_BYTES (4UL * WW * WH)                   /* 4 262 400 */
#define PL_A_BYTES (4UL * AW * AH)                      /* 1 065 600 */
#define PL_B_BYTES (4UL * BW * BH)                      /*   266 400 */
#define P_FIELD  (G7_POOL)                              /* the static field, packed 10/11/11 */
#define P_SOFT   (P_FIELD + PL_WALL_BYTES)              /* SOFT 720 x 1480 (bake: blur temps) */
#define P_TMP    (P_SOFT + PL_WALL_BYTES)               /* the wallpaper texture, freeze temp */
#define P_A      (P_TMP + PL_WALL_BYTES)                /* blur A, 8-bit */
#define P_A5     (P_A + PL_A_BYTES)                     /* frost taps on A, 10:10:10 */
#define P_B      (P_A5 + PL_A_BYTES)                    /* blur B, 8-bit */
#define P_TAB    (P_B + PL_B_BYTES)                     /* tables, 1 MiB */
#define P_TAB_BYTES 0x100000UL
#define P_SCR    (P_TAB + P_TAB_BYTES)                  /* scratch for decoding, 1 MiB */
#define P_SCR_BYTES 0x100000UL
#define P_END    (P_SCR + P_SCR_BYTES)
_Static_assert(P_END - G7_POOL <= G7_POOL_SIZE, "glass pool overflow");

/* ---- math ---- */
static inline float g_min(float a, float b) { return a < b ? a : b; }
static inline float g_max(float a, float b) { return a > b ? a : b; }
static inline float g_abs(float a) { return __builtin_fabsf(a); }
static inline float g_clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float g_sat(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }
static inline float g_mix(float a, float b, float t) { return a + (b - a) * t; }
static inline float g_floor(float x) { return __builtin_floorf(x); }
static inline float g_fract(float x) { return x - __builtin_floorf(x); }
static inline float g_sqrt(float x) { return __builtin_sqrtf(x); }
static inline int g_ifloor(float x) { return (int)__builtin_floorf(x); }
/* GLSL smoothstep, including e0 > e1 (the concept relies on it: core = smoothstep(1, .86, ast)) */
static inline float g_smooth(float e0, float e1, float x)
{
    float t = g_sat((x - e0) / (e1 - e0));
    return t * t * (3.f - 2.f * t);
}

/* 2^x: round to nearest integer, degree-7 polynomial on [-.5, .5] (rel. error ~5e-9). */
static inline float g_exp2(float x)
{
    if (x < -125.f)
        return 0.f;
    if (x > 127.f)
        x = 127.f;
    float fi = __builtin_floorf(x + .5f);
    float f = x - fi;
    float p = 1.52527338e-5f;
    p = p * f + 1.54035304e-4f;
    p = p * f + 1.33335581e-3f;
    p = p * f + 9.61812911e-3f;
    p = p * f + 5.55041087e-2f;
    p = p * f + 2.40226507e-1f;
    p = p * f + 6.93147181e-1f;
    p = p * f + 1.f;
    union { float f; int32_t i; } u;
    u.f = p;
    u.i += (int32_t)fi << 23;
    return u.f;
}
static inline float g_exp(float x) { return g_exp2(x * 1.44269504f); }
/* log2 for x > 0: mantissa in [sqrt(.5), sqrt(2)), atanh series (error ~1e-9). */
static inline float g_log2(float x)
{
    union { float f; int32_t i; } u;
    u.f = x;
    int32_t e = ((u.i >> 23) & 255) - 127;
    u.i = (u.i & 0x007fffff) | 0x3f800000;  /* m in [1, 2) */
    float m = u.f;
    if (m > 1.41421356f) {
        m *= .5f;
        e++;
    }
    float s = (m - 1.f) / (m + 1.f), s2 = s * s;
    float p = s2 * (1.f / 9.f) + (1.f / 7.f);
    p = p * s2 + (1.f / 5.f);
    p = p * s2 + (1.f / 3.f);
    p = p * s2 + 1.f;
    return (float)e + 2.88539008f * s * p;       /* 2 / ln 2 */
}
static inline float g_pow(float x, float y) { return x <= 0.f ? 0.f : g_exp2(y * g_log2(x)); }
/* sin(x), any x: reduce to [-pi, pi], fold to [-pi/2, pi/2], odd series to x^13. */
static inline float g_sin(float x)
{
    const float pi = 3.14159265f, tpi = 6.28318531f;
    x -= tpi * __builtin_floorf(x / tpi + .5f);
    if (x > pi * .5f)
        x = pi - x;
    else if (x < -pi * .5f)
        x = -pi - x;
    float x2 = x * x;
    float p = -1.60590438e-10f;
    p = p * x2 + 2.50521084e-8f;
    p = p * x2 - 2.75573192e-6f;
    p = p * x2 + 1.98412698e-4f;
    p = p * x2 - 8.33333333e-3f;
    p = p * x2 + 1.66666667e-1f;
    return x - x * x2 * p;
}
static inline float g_cos(float x) { return g_sin(x + 1.57079633f); }
/* atan2 in radians, |error| < 1e-5 */
static inline float g_atan2(float y, float x)
{
    float ax = g_abs(x), ay = g_abs(y);
    float mx = g_max(ax, ay), mn = g_min(ax, ay);
    if (mx == 0.f)
        return 0.f;
    float a = mn / mx, s = a * a;
    float r = ((((-0.0117212f * s + 0.05265332f) * s - 0.11643287f) * s + 0.19354346f) * s - 0.33262347f) * s + 0.99997726f;
    r *= a;
    if (ay > ax)
        r = 1.57079633f - r;
    if (x < 0.f)
        r = 3.14159265f - r;
    return y < 0.f ? -r : r;
}
/* erf, the Abramowitz-Stegun style rational used by box-shadow renderers (|error| < 5e-4) */
static inline float g_erf(float x)
{
    float s = x < 0.f ? -1.f : 1.f, a = g_abs(x);
    float t = 1.f + (0.278393f + (0.230389f + 0.000972f * a + 0.078108f * a * a) * a) * a;
    t *= t;
    return s - s / (t * t);
}

/* 0xAARRGGBB helpers */
#define CA(c) (((c) >> 24) & 255U)
#define CR(c) (((c) >> 16) & 255U)
#define CG(c) (((c) >> 8) & 255U)
#define CB(c) ((c) & 255U)
/* x / 255 rounded, for x in 0 .. 255 * 255 */
static inline uint32_t g_div255(uint32_t x) { x += 128U; return (x + (x >> 8)) >> 8; }

/* packed 10:10:10 planes (BASE, A5): value in 0..1 per channel */
static inline uint32_t g_pack10(float r, float g, float b)
{
    uint32_t R = (uint32_t)(g_sat(r) * 1023.f + .5f), G = (uint32_t)(g_sat(g) * 1023.f + .5f),
             B = (uint32_t)(g_sat(b) * 1023.f + .5f);
    return R << 20 | G << 10 | B;
}
#define UNP10_R(v) ((float)(((v) >> 20) & 1023U) * (1.f / 1023.f))
#define UNP10_G(v) ((float)(((v) >> 10) & 1023U) * (1.f / 1023.f))
#define UNP10_B(v) ((float)((v) & 1023U) * (1.f / 1023.f))

/* ---- glass_wall.c ---- */
void gw_field(void);                            /* the static field into P_FIELD (bake) */
int gw_field_valid(void);
void gw_field_lost(void);                       /* P_FIELD was reused (freeze temp) */
void gw_wall(const struct g7_wall *w);          /* WALL_FS for w into P_TMP (8-bit texture) */
void gw_blur(void);                             /* renderWall's blur passes: P_TMP -> A, B; A5 */
void gw_base(void);                             /* BASE = the texture at panel res (10:10:10) */
int gw_wall_same(const struct g7_wall *a, const struct g7_wall *b);
/* bilinear samples at a panel point (clamp to edge, WebGL texel centres) */
void gw_sample_a(float px, float py, float out[3]);
void gw_sample_a5(float px, float py, float out[3]);
void gw_sample_b(float px, float py, float out[3]);
/* one panel row of A5 and B at pixel centres x0 .. x1 - 1 into rgb triplets */
void gw_row_a5(int y, int x0, int x1, float *rgb);
void gw_row_b(int y, int x0, int x1, float *rgb);
void gw_row_wall(int y, int x0, int x1, float *rgb);  /* the texture at panel res (BASE not valid) */
/* rows at an arbitrary panel y (for the half resolution render) */
void gw_row_b_at(float py, int n, float px0, float dpx, float *rgb);
void gw_row_a5_at(float py, int n, float px0, float dpx, float *rgb);

/* ---- glass_shade.c ---- */
struct gs_ctx {                                 /* the uniforms of one GLASS_FS draw */
    float K, soft, dim, back;
    int opaque;
    float lx, ly, Ldx, Ldy;
};
/* prepare a layer's shapes (sdSq parameters, cull boxes, specular tables). layer 0 base, 1 over,
 * 2 the soft base at half resolution; zoom: #menu's scale about (720, 1332) (1 = none) */
void gs_prepare(int layer, const struct g7_shape *sh, unsigned n, const struct gs_ctx *c, float zoom);
/* shade one row: pixel centres px = px0 + i * grid (grid 1, or 2 for layer 2), py; acc rgb and al
 * in/out; rowa5 / rowb: A5 and B at those pixel centres */
void gs_row(int layer, const struct gs_ctx *c, float py, float px0, int n, float *acc, float *al,
            const float *rowa5, const float *rowb);
void gs_new_frame(void);
/* the glass wordmark over a base row */
void gs_word_prepare(const struct g7_word *w, const struct gs_ctx *c);
int gs_word_rows(int *y0, int *y1, int *x0, int *x1);
void gs_word_row(const struct gs_ctx *c, float py, int x0, int n, float *acc);
/* dither and store: out = floor(255 acc + hash) for gl_FragCoord (x + .5, 2960 - y - .5) */
void gs_store_row(uint32_t *dst, int y, int x0, int n, const float *acc);
/* over layer composite: dst = acc al + dst (1 - al), dithered */
void gs_over_row(uint32_t *dst, int y, int x0, int n, const float *acc, const float *al);
int gs_load_height(void);

/* ---- glass_draw.c ---- */
struct gd_target {
    uint32_t *px;                               /* pixel (0, 0) of the target */
    int stride;                                 /* in pixels */
    int x0, y0, x1, y1;                         /* the region drawn now (target px, x1 / y1 exclusive) */
    float s, ox, oy;                            /* target = panel * s + o */
    int premul;                                 /* 1: a transparent premultiplied buffer */
};
void gd_items(const struct g7_layer *l, const int16_t (*bounds)[4], const struct gd_target *t);
/* an item's drawn bounds in panel px (x0, y0, x1, y1), 0 if it draws nothing */
int gd_bounds(const struct g7_layer *l, const struct g7_item *it, int16_t b[4]);
int gd_init(void);
const struct sb7_font *gd_font(unsigned id);

/* ---- glass.c ---- */
void g7_poll(void);                             /* the g7_set_poll hook, between rows (glass.c) */
extern float g_row_tmp[];                       /* shared row scratch (see glass.c) */
