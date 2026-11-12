/* SPDX-License-Identifier: MIT
 * Starbite Liquid Glass, the lens (P4 v8: the iOS audit's optical spec, starbite-ios-audit.md "For the GPU session").
 * One model for every glass element, in three places that must agree: the GPU's comp2 shader (tools/mesa-compjob.c
 * builds lens_tone + the light in NIR), gpucomp.c's CPU reference, and the toolkit (the CPU fallback's compositor
 * and the glass baked into a layer, tk_glass.c).
 *
 * Geometry, per element (a rounded box; units pt, 1 pt = 3.692 px on the S9), done on the CPU once per element:
 *   - size class k = smoothstep(44, 300, m), m = min(w, h): thickness T = mix(8, 20, k), bezel
 *     b = min(r, m / 2, mix(12, 22, k));
 *   - profile (convex squircle) h(t) = (1 - (1 - t)^4)^(1/4), t = s / b (s: the distance in from the edge), its
 *     slope clamped to 8; the normal n = normalize(g T / b h'(t), 1) (g: the outward normal in the plane);
 *   - refraction (Snell, n = 1.5): the ray straight down bends inward; the backdrop shows at q = p - g D,
 *     D = |R.xy| / -R.z T h(t), normalised so its peak is 0.2 b (the spec's calibration target): the rim magnifies;
 *   - light (from the top-left, Lt = normalize(-1, -1.4)), f = dot(g, Lt): a thin specular
 *     exp(-s / 1.2) (.55 max(f, 0)^3 + .25 max(-f, 0)^3), Fresnel .5 (.04 + .96 (1 - n.z)^5) inside the bezel,
 *     less a depth shade .06 (1 - t)^2 max(-f, 0) on the side away from the light. No stroke, gloss or tint.
 * The lens map (one 32-bit word per px of the layer's glass mask): [0..7] coverage, [8..15] o.x and [16..23] o.y
 * (the sample offset q - p in panel px, signed, 1/4 px), [24..31] the light e (signed, / 127: > 0 white added,
 * < 0 darkened).
 * Per pixel (GPU, CPU fallback): the backdrop at p + o (bilinear), the tone by its luminance Y (dark below .40,
 * light above .50, blended between: c = .82 c + mix(.04, .18, light)), then the light e. */
#ifndef SB_LENS_H
#define SB_LENS_H
#include <stdint.h>

#define LENS_PT 3.6923077f        /* px per pt */
#define LENS_LX -.58123819f       /* toward the light: normalize(-1, -1.4) */
#define LENS_LY -.81373347f

static inline float lens_sqrt(float x)
{
#if defined(__aarch64__)
    float r;
    __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    return r;
#else
    return __builtin_sqrtf(x);
#endif
}
static inline float lens_sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static inline float lens_mix(float a, float b, float t) { return a + (b - a) * t; }
static inline float lens_exp_neg(float x)    /* e^-x for x >= 0 (within 1 % where it matters) */
{
    float y = 1 + x * (1.0f / 32);
    y *= y; y *= y; y *= y; y *= y; y *= y;
    return 1 / y;
}

/* an element's lens class (from its size and corner radius, pt) */
typedef struct { float T, b, dk; } lens_cls;
/* the profile at t: the offset (pt, before the class's normalisation) and n.z */
static inline float lens_d_raw(float t, float T, float b, float *nz)
{
    float u = 1 - t, u4 = u * u * u * u, q = 1 - u4;
    float h = q > 0 ? lens_sqrt(lens_sqrt(q)) : 0;
    float hp = q > 0 ? u * u * u / (h * h * h) : 8;
    hp = hp > 8 ? 8 : hp;
    float sl = T / b * hp, inn = 1 / lens_sqrt(1 + sl * sl), nx = sl * inn, z = inn;
    const float eta = 1 / 1.5f;
    float k = 1 - eta * eta * (1 - z * z), a = eta * z - lens_sqrt(k > 0 ? k : 0);
    float rx = a * nx, rz = -eta + a * z;
    if (nz) *nz = z;
    return (rx < 0 ? -rx : rx) / (rz < -1e-4f ? -rz : 1e-4f) * T * h;
}
static inline lens_cls lens_class(float w_pt, float h_pt, float r_pt)
{
    float m = w_pt < h_pt ? w_pt : h_pt, x = lens_sat((m - 44) / (300 - 44)), k = x * x * (3 - 2 * x);
    lens_cls c;
    c.T = lens_mix(8, 20, k);
    c.b = lens_mix(12, 22, k);
    if (r_pt < c.b) c.b = r_pt;
    if (m * .5f < c.b) c.b = m * .5f;
    if (c.b < 1) c.b = 1;
    float mx = 0;
    for (int i = 1; i <= 64; ++i) {
        float d = lens_d_raw((float)i / 64, c.T, c.b, 0);
        if (d > mx) mx = d;
    }
    c.dk = mx > 1e-4f ? .2f * c.b / mx : 0;
    return c;
}
/* v9: a glass shape's lens in frame px (no per-pixel map: the GPU and the CPU evaluate it per pixel from the shape's
 * rounded-box distance): lb the bezel (px), lt = T / b, lc the offset scale (px), lr the specular's falloff
 * (log2(e) / 1.2 pt, per px). A half frame halves lb and lc and doubles lr. */
typedef struct { float lb, lt, lc, lr; } lens_par;
static inline lens_par lens_params(float w_px, float h_px, float r_px, float px_per_pt)
{
    lens_cls c = lens_class(w_px / px_per_pt, h_px / px_per_pt, r_px / px_per_pt);
    lens_par p;
    p.lb = c.b * px_per_pt;
    p.lt = c.T / c.b;
    p.lc = c.T * c.dk * px_per_pt;
    p.lr = 1.44269504f / (1.2f * px_per_pt);
    return p;
}
static inline float lens_exp2_neg(float x)   /* 2^-x, x >= 0 */
{
    return lens_exp_neg(x * .69314718f);
}
/* s px in from the edge, outward normal (gx, gy) (unit): the offset (frame px) and the light e; the steps comp2's
 * lens path does */
static inline void lens_eval(const lens_par *P, float s, float gx, float gy, float *ox, float *oy, float *e)
{
    float t = lens_sat(s / P->lb), u = 1 - t, u2 = u * u, q = 1 - u2 * u2;
    float h = lens_sqrt(lens_sqrt(q)), h3 = h * h * h;
    float hp = u2 * u / (h3 > 1e-6f ? h3 : 1e-6f);
    hp = hp < 8 ? hp : 8;
    float sl = P->lt * hp, inn = 1 / lens_sqrt(1 + sl * sl), nx = sl * inn, nz = inn;
    const float eta = 1 / 1.5f;
    float k = 1 - eta * eta * (1 - nz * nz), a = eta * nz - lens_sqrt(k > 0 ? k : 0);
    float rx = a * nx, rz = -eta + a * nz;
    float D = (rx < 0 ? -rx : rx) / (-rz > 1e-4f ? -rz : 1e-4f) * h * P->lc;
    *ox = -gx * D;
    *oy = -gy * D;
    float f = gx * LENS_LX + gy * LENS_LY, fp = f > 0 ? f : 0, fn = f < 0 ? -f : 0;
    float rho = lens_exp2_neg(s * P->lr), w = 1 - nz, w2 = w * w;
    *e = rho * (.55f * fp * fp * fp + .25f * fn * fn * fn) + .5f * (.04f + .96f * w2 * w2 * w) - .06f * u2 * fn;
}
/* a rounded box (centre cx, cy; half size less the radius ex, ey; radius r) at (px, py): the signed distance
 * (negative inside) and the outward normal, as comp2 computes them */
static inline float lens_sd(float px, float py, float cx, float cy, float ex, float ey, float r, float *gx, float *gy)
{
    float dx = px - cx, dy = py - cy, sx = dx < 0 ? -1.0f : 1.0f, sy = dy < 0 ? -1.0f : 1.0f;
    float qx = dx * sx - ex, qy = dy * sy - ey, mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0, mm = qx > qy ? qx : qy;
    float l = lens_sqrt(mx * mx + my * my);
    float nx = l > 0 ? mx : qx >= qy ? 1.0f : 0, ny = l > 0 ? my : qx >= qy ? 0 : 1.0f, il = 1 / lens_sqrt(nx * nx + ny * ny);
    *gx = sx * nx * il;
    *gy = sy * ny * il;
    return l + (mm < 0 ? mm : 0) - r;
}

/* at s pt in from the edge, outward normal (gx, gy) (unit): the sample offset (panel px) and the light e */
static inline void lens_at(const lens_cls *c, float s, float gx, float gy, float *ox, float *oy, float *e)
{
    float t = lens_sat(s / c->b), nz;
    float D = lens_d_raw(t, c->T, c->b, &nz) * c->dk * LENS_PT;
    *ox = -gx * D;
    *oy = -gy * D;
    float f = gx * LENS_LX + gy * LENS_LY, fp = f > 0 ? f : 0, fn = f < 0 ? -f : 0;
    float rho = lens_exp_neg(s * (1.0f / 1.2f)), u = 1 - nz, u5 = u * u * u * u * u;
    float spec = rho * (.55f * fp * fp * fp + .25f * fn * fn * fn) + .5f * (.04f + .96f * u5);
    float depth = .06f * (1 - t) * (1 - t) * fn;
    *e = spec - depth;
}

static inline uint32_t lens_word(float cov, float ox, float oy, float e)
{
    int c = (int)(lens_sat(cov) * 255 + .5f);
    int x = (int)(ox * 4 + (ox < 0 ? -.5f : .5f)), y = (int)(oy * 4 + (oy < 0 ? -.5f : .5f));
    int q = (int)(e * 127 + (e < 0 ? -.5f : .5f));
    x = x < -127 ? -127 : x > 127 ? 127 : x;
    y = y < -127 ? -127 : y > 127 ? 127 : y;
    q = q < -127 ? -127 : q > 127 ? 127 : q;
    return (uint32_t)c | (uint32_t)(uint8_t)(int8_t)x << 8 | (uint32_t)(uint8_t)(int8_t)y << 16 |
           (uint32_t)(uint8_t)(int8_t)q << 24;
}
static inline void lens_unword(uint32_t m, float *cov, float *ox, float *oy, float *e)
{
    *cov = (float)(m & 255u) * (1.0f / 255);
    *ox = (float)(int8_t)(uint8_t)(m >> 8) * .25f;
    *oy = (float)(int8_t)(uint8_t)(m >> 16) * .25f;
    *e = (float)(int8_t)(uint8_t)(m >> 24) * (1.0f / 127);
}

/* v10: the backdrop's saturation (about its luminance, clamped), before lens_shade */
static inline void lens_saturate(float rgb[3], float k)
{
    float Y = .2126f * rgb[0] + .7152f * rgb[1] + .0722f * rgb[2];
    for (int c = 0; c < 3; ++c) rgb[c] = lens_sat(Y + (rgb[c] - Y) * k);
}
/* rgb: the refracted backdrop, 0..1; shaded in place (the shader does exactly these steps: comp2, flag 256) */
static inline void lens_shade(float rgb[3], float e)
{
    float Y = .2126f * rgb[0] + .7152f * rgb[1] + .0722f * rgb[2];
    float add = lens_mix(.04f, .18f, lens_sat((Y - .40f) * 10));
    for (int c = 0; c < 3; ++c) {
        float v = rgb[c] * .82f + add;
        rgb[c] = e > 0 ? v + (1 - v) * e : v * (1 + e);
    }
}
/* the same over a premultiplied backdrop with alpha (glass baked into a layer over its own content, which may be
 * transparent): the tone as a black layer of .18 and a white one of add, the light as a white or black layer; for
 * c[3] = 1 it is lens_shade */
static inline void lens_shade_pm(float c[4], float e)
{
    float A = c[3] > 1e-4f ? c[3] : 1e-4f;
    float Y = (.2126f * c[0] + .7152f * c[1] + .0722f * c[2]) / A * c[3];
    float add = lens_mix(.04f, .18f, lens_sat((Y - .40f) * 10));
    for (int q = 0; q < 3; ++q) c[q] = c[q] * .82f + add;
    c[3] = c[3] * .82f + .18f;                 /* black .18 then white add: alpha 1 - .82 (1 - a) */
    if (e > 0) {
        for (int q = 0; q < 3; ++q) c[q] = c[q] * (1 - e) + e;
        c[3] = c[3] * (1 - e) + e;
    } else {
        for (int q = 0; q < 3; ++q) c[q] *= 1 + e;
        c[3] = c[3] * (1 + e) - e;
    }
}
#endif
