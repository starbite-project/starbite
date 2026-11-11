/* SPDX-License-Identifier: MIT
 * Starbite Liquid Glass, the lens (P4 v6). One model for every glass element, in three places that must agree: the
 * GPU's comp2 shader (tools/mesa-compjob.c builds the same maths in NIR), gpucomp.c's CPU reference, and the
 * toolkit (the CPU fallback's compositor and the glass baked into a layer, tk_glass.c).
 *
 * A glass element is a slab whose top surface is flat in the middle and curves down to its edge over the bezel
 * (LENS_B). Looking straight down through it, a ray that meets the curved part bends toward the middle, so the
 * pixel at distance d from the edge shows the backdrop LENS_A x (1 - d / B)^2 px further in: the backdrop is
 * pulled out toward the rim (a magnifier's edge) and the middle shows it as it is. Per pixel:
 *   - refraction: the backdrop at p + o, with o the inward offset above (the lens map holds it per pixel);
 *   - dispersion: red's sample at o x (1 + D), blue's at o x (1 - D): colour fringes where the backdrop changes;
 *   - adaptive tint: a white veil over a dark backdrop, a dark one over a bright backdrop (its luminance);
 *   - an inner shadow: the rim's inside darkened, more on the side away from the light;
 *   - a specular rim: a thin bright edge, strongest where the edge faces the light (up and a little left), a
 *     weaker one on the opposite edge (the light inside the slab), and a soft glow on the lit side.
 * The lens map (one 32-bit word per px of the layer's glass mask): [0..7] coverage, [8..15] o.x and [16..23] o.y
 * (signed, 1/4 px), [24..31] d (1/4 px, 255 = 63.75 or more). */
#ifndef SB_LENS_H
#define SB_LENS_H
#include <stdint.h>

#define LENS_B 44.0f          /* the bezel: 12 pt (px) */
#define LENS_A 16.0f          /* the offset at the edge (px): 4.3 pt */
#define LENS_D .22f           /* dispersion */
#define LENS_RIM 5.0f         /* the specular rim's width (px) */
#define LENS_GLOW 24.0f       /* the lit side's glow */
#define LENS_SHW 36.0f        /* the inner shadow's width */
#define LENS_LX -.5f          /* toward the light: up and a little left */
#define LENS_LY -.8660254f

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

/* the inward offset's length at distance d from the edge of a shape whose bezel is b (b = LENS_B, or less for a
 * thin shape: half its short side) */
static inline float lens_disp(float d, float b)
{
    float t = 1 - lens_sat(d / b);
    return LENS_A * (b / LENS_B) * t * t;
}

static inline uint32_t lens_word(float cov, float ox, float oy, float d)
{
    int c = (int)(lens_sat(cov) * 255 + .5f);
    int x = (int)(ox * 4 + (ox < 0 ? -.5f : .5f)), y = (int)(oy * 4 + (oy < 0 ? -.5f : .5f));
    int q = (int)(d * 4 + .5f);
    x = x < -127 ? -127 : x > 127 ? 127 : x;
    y = y < -127 ? -127 : y > 127 ? 127 : y;
    q = q < 0 ? 0 : q > 255 ? 255 : q;
    return (uint32_t)c | (uint32_t)(uint8_t)(int8_t)x << 8 | (uint32_t)(uint8_t)(int8_t)y << 16 | (uint32_t)q << 24;
}
static inline void lens_unword(uint32_t m, float *cov, float *ox, float *oy, float *d)
{
    *cov = (float)(m & 255u) * (1.0f / 255);
    *ox = (float)(int8_t)(uint8_t)(m >> 8) * .25f;
    *oy = (float)(int8_t)(uint8_t)(m >> 16) * .25f;
    *d = (float)(m >> 24) * .25f;
}

/* rgb: the refracted backdrop, 0..1 (red from red's sample, green from o's, blue from blue's); shaded in place.
 * The shader does exactly these steps (comp2, flag 256). */
static inline void lens_shade(float rgb[3], float ox, float oy, float d)
{
    float Y = .2126f * rgb[0] + .7152f * rgb[1] + .0722f * rgb[2];
    float wv = .10f * lens_sat((.62f - Y) * 2.0f), kv = .12f * lens_sat((Y - .62f) * 2.5f);
    float keep = 1 - wv - kv;
    for (int c = 0; c < 3; ++c) rgb[c] = rgb[c] * keep + wv;
    if (d < LENS_SHW) {
        float m2 = ox * ox + oy * oy, f = 0;
        if (m2 > 1e-6f) {
            float im = 1 / lens_sqrt(m2);
            f = -(ox * LENS_LX + oy * LENS_LY) * im;      /* the outward normal (-o) against the light */
        }
        float e = 1 - d * (1.0f / LENS_SHW), sh = .20f * e * e * (.55f - .45f * f);
        float fp = f > 0 ? f : 0, fn = f < 0 ? -f : 0;
        float e1 = lens_sat(1 - d * (1.0f / LENS_RIM)), g = lens_sat(1 - d * (1.0f / LENS_GLOW));
        float sp = e1 * (.16f + .62f * fp * fp + .30f * fn * fn) + .14f * fp * g * g;
        for (int c = 0; c < 3; ++c) {
            float v = rgb[c] * (1 - sh);
            rgb[c] = v + (1 - v) * sp;
        }
    }
}
/* the same shading for a premultiplied backdrop with alpha (c[3]; glass baked into a layer over its own content,
 * which may be transparent): each step is a white or a black layer over it (for c[3] = 1, lens_shade exactly), so
 * glass over nothing is its veil, its inner shadow and its rim over whatever lies under the layer */
static inline void lens_shade_pm(float c[4], float ox, float oy, float d)
{
    float Y = .2126f * c[0] + .7152f * c[1] + .0722f * c[2];
    float wv = .10f * lens_sat((.62f - Y) * 2.0f), kv = .12f * lens_sat((Y - .62f) * 2.5f);
    float sh = 0, sp = 0;
    if (d < LENS_SHW) {
        float m2 = ox * ox + oy * oy, f = 0;
        if (m2 > 1e-6f) f = -(ox * LENS_LX + oy * LENS_LY) / lens_sqrt(m2);
        float e = 1 - d * (1.0f / LENS_SHW), fp = f > 0 ? f : 0, fn = f < 0 ? -f : 0;
        float e1 = lens_sat(1 - d * (1.0f / LENS_RIM)), g = lens_sat(1 - d * (1.0f / LENS_GLOW));
        sh = .20f * e * e * (.55f - .45f * f);
        sp = e1 * (.16f + .62f * fp * fp + .30f * fn * fn) + .14f * fp * g * g;
    }
    float w[4] = { wv, kv, sh, sp }, white[4] = { 1, 0, 0, 1 };
    for (int k = 0; k < 4; ++k) {
        float a = w[k];
        for (int q = 0; q < 3; ++q) c[q] = c[q] * (1 - a) + white[k] * a;
        c[3] = c[3] * (1 - a) + a;
    }
}
#endif
