/* SPDX-License-Identifier: MIT
 * Starbite Liquid Glass, the lens (glass V3: iOS 26/27, docs/design/LIQUID-GLASS.md section 2 and the rows G1, G2, G4,
 * G5, G14, G15 of its section 6.2). One model for every glass element, in four places that must agree: the GPU's comp2
 * shader (tools/mesa-compjob.c lens_px builds these steps in NIR), gpucomp.c's CPU reference (cpu_px2, the phone's
 * self test: GPU within 3 levels of it), the toolkit's CPU compositor (tk_comp.c draw_lens, tk_flat_lens) and the
 * glass baked into a layer (tk_glass.c: the lens over bitmap backdrops, the face as an over-layer).
 *
 * Per element, on the CPU once (lens_make; units pt, 1 pt = LENS_PT px on the S9; short = min(w, h)):
 *   G1  band h = min(20, .25 short), amount A = min(60, .5 short) x LENS_GAIN. Inside the band the backdrop is
 *       sampled inward at q = p - d n (n the outward normal), d = A (1 - sqrt(1 - t^2)), t = 1 - s / h (s the
 *       distance in from the edge): a quarter circle, C1 with the flat face, d = A at the silhouette. Where A > h
 *       the outer part of the band shows a squeezed, mirrored strip of what lies further in: the "liquid" rim.
 *   G2  the face: Y = luma(bg), lumY = Y + lift Y (1 - Y), face = E + T lumY + K (bg - Y) (clamped), K plus
 *       .4 sat((chroma - .15) / .65) for already-colourful pixels. Presets at the slider s (LENS_S, default .2):
 *       light E (253, 252, 253) x mix(.403, .726, s), T mix(.599, .274, s), lift mix(.216, .270, s),
 *       K mix(1.169, .755, s); dark E 32 / 255, T and lift by size (<= 63 pt: mix(.600, .298, s) and 0; >= 150 pt:
 *       mix(.4525, .2086, s) and 1.2; linear between), K mix(1.005, .786, s); clear E white x .126, T .954, lift 0,
 *       K 1.057. Which preset (section 2.5): regular elements with short <= 70 pt flip by the backdrop's mean
 *       luminance under them (yc: dark below .35, light above .55, blended between: no state, so no hysteresis
 *       yet: G6); larger ones and s >= .75 use the system appearance (dark). The caller measures yc on the CPU per
 *       element (the toolkit: the wallpaper's blurred backdrop under the shape; < 0 unknown: dark).
 *   G14 clear (variant 1): the clear preset, never flips; over a bright backdrop (yc .45 .. .55 and up) a black
 *       layer of .35 over the element (folded into E, T, K).
 *   G15 a tint (ARGB, straight alpha a): tone = mix(.55 tint, mix(tint, white, .35), lumY), face = mix(face, tone,
 *       .8 a). The tone is linear in lumY, so the tint folds into E and T per channel (the shader has no tint step).
 *   press (section 5.1, the part a shader can do): a flat white glow of .08 x press over the face, the glint x 1.5.
 *   G4  the glint (iOS 27): a fixed vertical light, the top and bottom walls alike: lobe = 1 - |n.x|, a 1.2 pt
 *       inward ramp plus a .21 bleed to 4.8 pt; the face is recoloured toward 1.6 white + 2.85 (face - luma(face))
 *       by .252 x .6 x lobe x ramp (glass over colour glints in that colour). No Fresnel term all round (it read as
 *       an outline), no dispersion (G18).
 *   G5  the contour (iOS 27's defined border): 0.75 pt just outside the silhouette, the backdrop darkened by
 *       peak (1 - dir lobe): light .43 / .77, dark .88 / 1, clear .36 / .77: the side walls, not where the glint
 *       is. The lens quad reaches 4 px past the shape for it.
 * Everything per element lands in a float[LQ_N] (gc_quad.lq; gpucomp.c packs it into the 160-byte lens quad: the
 * geometry as f32, the rest as fp16 pairs in words a lens quad does not use; mesa-compjob.c's comment has the
 * layout); a half frame scales the geometry's px (lens_scale). Per pixel (GPU and CPU alike): lens_sd (the rounded
 * box's distance and normal), lens_offset, the backdrop bilinear at p + o, lens_face, lens_contour, lens_blend over
 * what is under it. comp2 works the face, glint and contour in fp16 (within a level of these f32 steps). */
#ifndef SB_LENS_H
#define SB_LENS_H
#include <stdint.h>

#define LENS_PT 3.6923077f        /* px per pt */
#ifndef LENS_GAIN
#define LENS_GAIN 1.0f            /* G1: the refraction amount's scale (ramp it .33 -> 1 while checking on the phone) */
#endif
#ifndef LENS_S
#define LENS_S .2f                /* section 2.2: the Liquid Glass slider s, clear 0 .. tinted 1 (G17 makes it a setting) */
#endif

/* the per-element record: band 1 / h (px), amount A (px), 1 / (px per pt), glint strength, chroma gain K, lift,
 * the contour's outer edge (px, + .5 for its antialiasing), its peak and peak x dir, K's chromatic boost, E (rgb),
 * T (rgb) */
enum { LQ_IH, LQ_A, LQ_IPT, LQ_GL, LQ_K, LQ_LIFT, LQ_WC, LQ_CA, LQ_CB, LQ_KB, LQ_ER, LQ_EG, LQ_EB, LQ_TR, LQ_TG, LQ_TB,
       LQ_N };
#define LENS_APRON 4.0f           /* px the lens quad reaches past the shape (the contour: .75 pt + .5 = 3.3 px) */

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
static inline float lens_abs(float v) { return v < 0 ? -v : v; }
static inline float lens_luma(const float c[3]) { return .2126f * c[0] + .7152f * c[1] + .0722f * c[2]; }

/* a face preset (section 2.4): E (grey) x E.a, T, lift, K, the contour's peak and dir */
typedef struct { float E, T, lift, K, cp, cd; } lens_preset;
static inline lens_preset lens_preset_of(int which /* 0 light, 1 dark, 2 clear */, float short_pt, float s)
{
    lens_preset p;
    if (which == 2) {
        p = (lens_preset){ .126f, .954f, 0, 1.057f, .36f, .77f };
    } else if (which == 0) {
        p = (lens_preset){ (252.667f / 255) * lens_mix(.403f, .726f, s), lens_mix(.599f, .274f, s),
                           lens_mix(.216f, .270f, s), lens_mix(1.169f, .755f, s), .43f, .77f };
    } else {
        float z = lens_sat((short_pt - 63) * (1.0f / (150 - 63)));      /* dark: larger glass is more opaque */
        p = (lens_preset){ 32.0f / 255, lens_mix(lens_mix(.600f, .298f, s), lens_mix(.4525f, .2086f, s), z),
                           lens_mix(0, 1.2f, z), lens_mix(1.005f, .786f, s), .88f, 1.0f };
    }
    return p;
}

/* an element's record: its shape (px), px per pt (a half frame: half of LENS_PT), its variant (0 regular, 1 clear),
 * tint (ARGB, straight alpha; 0 none), press (0..1) and yc (the backdrop's mean luminance under it, < 0 unknown) */
static inline void lens_make(float L[LQ_N], float w_px, float h_px, float px_per_pt, int variant, uint32_t tint,
                             float press, float yc)
{
    float sh = (w_px < h_px ? w_px : h_px) / px_per_pt, s = LENS_S;
    float hb = (sh * .25f < 20 ? sh * .25f : 20) * px_per_pt, A = (sh * .5f < 60 ? sh * .5f : 60) * px_per_pt;
    L[LQ_IH] = 1 / (hb > 1 ? hb : 1);
    L[LQ_A] = A * LENS_GAIN;
    L[LQ_IPT] = 1 / px_per_pt;
    L[LQ_WC] = .75f * px_per_pt + .5f;
    /* the preset: clear; or regular, small ones flipping light / dark by yc (section 2.5), the rest dark */
    lens_preset P;
    float dim = 0;
    if (variant == 1) {
        P = lens_preset_of(2, sh, s);
        dim = .35f * lens_sat((yc - .45f) * 10);                    /* G14: the black .35 over bright content */
    } else {
        float wl = sh <= 70 && s < .75f && yc >= 0 ? lens_sat((yc - .35f) * 5) : 0;
        lens_preset D = lens_preset_of(1, sh, s), Lt = lens_preset_of(0, sh, s);
        P = (lens_preset){ lens_mix(D.E, Lt.E, wl), lens_mix(D.T, Lt.T, wl), lens_mix(D.lift, Lt.lift, wl),
                           lens_mix(D.K, Lt.K, wl), lens_mix(D.cp, Lt.cp, wl), lens_mix(D.cd, Lt.cd, wl) };
    }
    float E[3] = { P.E, P.E, P.E }, T[3] = { P.T, P.T, P.T }, K = P.K, KB = .4f;
    /* G15: the tint folded into E and T: (1 - w) face + w (.55 c + (.1 c + .35) lumY), w = .8 a */
    float w = .8f * (float)(tint >> 24) * (1.0f / 255);
    for (int c = 0; c < 3; ++c) {
        float tc = (float)(tint >> (16 - 8 * c) & 255) * (1.0f / 255);
        E[c] = (1 - w) * E[c] + w * .55f * tc;
        T[c] = (1 - w) * T[c] + w * (.1f * tc + .35f);
    }
    /* the press's flat glow (white .08 x press), then the clear variant's dim (black), as layers over the face */
    float g = .08f * lens_sat(press), keep = (1 - w) * (1 - g) * (1 - dim);
    for (int c = 0; c < 3; ++c) {
        E[c] = (E[c] * (1 - g) + g) * (1 - dim);
        T[c] *= (1 - g) * (1 - dim);
        L[LQ_ER + c] = E[c];
        L[LQ_TR + c] = T[c];
    }
    L[LQ_K] = K * keep;
    L[LQ_KB] = KB * keep;
    L[LQ_LIFT] = P.lift;
    L[LQ_GL] = .252f * .6f * (1 + .5f * lens_sat(press));
    L[LQ_CA] = P.cp;
    L[LQ_CB] = P.cp * P.cd;
}
/* the record for a frame k times the size (a big move's half frame: k = .5) */
static inline void lens_scale(float L[LQ_N], float k)
{
    L[LQ_IH] /= k;
    L[LQ_A] *= k;
    L[LQ_IPT] /= k;
    L[LQ_WC] = (L[LQ_WC] - .5f) * k + .5f;
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

/* G1: the sample offset (px) at s px in from the edge (0 on and outside it), outward normal (gx, gy) */
static inline void lens_offset(const float *L, float s, float gx, float gy, float *ox, float *oy)
{
    float t = lens_sat(1 - s * L[LQ_IH]);
    float d = L[LQ_A] * (1 - lens_sqrt(1 - t * t));
    *ox = -gx * d;
    *oy = -gy * d;
}

/* G2 + G4: rgb, the backdrop at p + o (0..1), becomes the face with its glint; s px in from the edge, gx the outward
 * normal's x */
static inline void lens_face(const float *L, float rgb[3], float s, float gx)
{
    float Y = lens_luma(rgb), lum = Y + L[LQ_LIFT] * Y * (1 - Y);
    float mx = rgb[0] > rgb[1] ? rgb[0] : rgb[1], mn = rgb[0] < rgb[1] ? rgb[0] : rgb[1];
    mx = mx > rgb[2] ? mx : rgb[2];
    mn = mn < rgb[2] ? mn : rgb[2];
    float K = L[LQ_K] + L[LQ_KB] * lens_sat((mx - mn - .15f) * (1 / .65f)), f[3];
    for (int c = 0; c < 3; ++c)
        f[c] = lens_sat(L[LQ_ER + c] + L[LQ_TR + c] * lum + K * (rgb[c] - Y));
    float Lf = lens_luma(f), sp = s * L[LQ_IPT];
    float r1 = lens_sat(1 - sp * (1 / 1.2f)), r2 = .21f * lens_sat(1 - sp * (1 / 4.8f));
    float gw = L[LQ_GL] * (1 - lens_abs(gx)) * (r1 > r2 ? r1 : r2);
    for (int c = 0; c < 3; ++c)
        rgb[c] = lens_sat(f[c] + gw * (1.6f + 2.85f * (f[c] - Lf) - f[c]));
}

/* G5: the contour's darkening at the signed distance sd (px; it only shows outside: lens_blend) */
static inline float lens_contour(const float *L, float sd, float gx)
{
    return (L[LQ_CA] - L[LQ_CB] * (1 - lens_abs(gx))) * lens_sat(L[LQ_WC] - sd);
}

/* the glass over what is under it (acc, premultiplied rgba): the contour (ct) as a black layer outside the shape,
 * then the face (rgb) by its coverage; al the quad's alpha (and fade) */
static inline void lens_blend(float acc[4], const float rgb[3], float sd, float al, float ct)
{
    float cov = lens_sat(.5f - sd), x = al * ct * (1 - cov), m = al * cov;
    for (int c = 0; c < 3; ++c) {
        float d = acc[c] * (1 - x);
        acc[c] = d + (rgb[c] - d) * m;
    }
    float a = acc[3] * (1 - x) + x;
    acc[3] = a + (1 - a) * m;
}
#endif
