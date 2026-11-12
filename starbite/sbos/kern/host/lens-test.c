/* SPDX-License-Identifier: MIT
 * Host test of starbite/sbos/kern/lens.h (Liquid Glass V3: LIQUID-GLASS.md G1 G2 G4 G5 G14 G15): numeric checks of the
 * model, and stills of glass shapes over a test backdrop drawn on the CPU path exactly as gpucomp.c's reference
 * (cpu_px2) and tk_comp.c's draw_lens do it (lens_sd, lens_offset, the blurred backdrop bilinear at p + o,
 * lens_face, lens_contour, lens_blend over the sharp backdrop).
 *
 *   cc -std=gnu11 -O2 -Wall -Wextra -Werror -I starbite/sbos/kern starbite/sbos/kern/host/lens-test.c -lm \
 *      -o build/lens-test && build/lens-test [OUTDIR]
 *
 * With OUTDIR it writes OUTDIR/{pill,card,circle}.ppm: per row one variant (regular light, regular dark, clear,
 * tinted green, pressed) over a backdrop of stripes, blobs and text-like bars (bright under the light row). Another
 * lens model's stills: -DLENS_TEST_HEADER='"old/lens.h"' -DLENS_TEST_GLASS_PX='"old-px.inc"' (its glass_px; no checks).
 * Exit status 0 = all checks passed. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef LENS_TEST_HEADER
#include LENS_TEST_HEADER
#else
#include "lens.h"
#endif

static int fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

#define PT LENS_PT
typedef struct { float x, y, w, h, r; int var; uint32_t tint; float press; } el;

/* ---- the backdrop: sharp (what lies under the glass) and blurred (what the glass samples, sigma ~4 pt) ------------ */
static int W, H;
static float *S, *B;                            /* rgb, 0..1 */
static float hash(int x) { x = (x ^ 61) ^ (x >> 16); x *= 9; x ^= x >> 4; x *= 0x27d4eb2d; x ^= x >> 15; return (float)(x & 1023) / 1023; }
static void make_backdrop(const float *rowY, int rows, float rowh)
{
    S = calloc((size_t)W * H * 3, sizeof(float));
    B = calloc((size_t)W * H * 3, sizeof(float));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int r = (int)(y / rowh);
            float base = rowY[r < rows ? r : rows - 1], *p = S + ((size_t)y * W + x) * 3;
            float xp = x / PT, yp = y / PT;
            /* diagonal colour stripes 24 pt wide */
            int st = ((int)floorf((xp + yp) / 24)) & 3;
            static const float col[4][3] = { { .95f, .30f, .25f }, { .25f, .55f, .95f }, { .30f, .80f, .40f }, { .95f, .80f, .25f } };
            for (int c = 0; c < 3; c++) p[c] = base * .75f + col[st][c] * (base > .5f ? .25f : .35f);
            /* text-like bars: 3 pt tall, every 12 pt, broken into words */
            float ty = fmodf(yp, 12);
            int word = (int)(xp / 9);
            if (ty > 4 && ty < 7 && hash(word * 131 + (int)(yp / 12)) > .3f && fmodf(xp, 9) < 7.5f)
                for (int c = 0; c < 3; c++) p[c] = base > .5f ? .08f : .95f;
            /* a few soft blobs */
            for (int k = 0; k < 6; k++) {
                float bx = (40 + 70 * k) * PT, by = (30 + 47 * k % 300) * PT, rr = 22 * PT;
                float d2 = ((x - bx) * (x - bx) + (y - by) * (y - by)) / (rr * rr);
                if (d2 < 1) for (int c = 0; c < 3; c++) p[c] = p[c] * (1 - .6f * (1 - d2)) + col[k & 3][(c + 1) % 3] * .6f * (1 - d2);
            }
        }
    /* three box passes (radius 2.5 pt): sigma ~ 4 pt, like the live backdrop's dual filter */
    float *t = malloc((size_t)W * H * 3 * sizeof(float));
    memcpy(B, S, (size_t)W * H * 3 * sizeof(float));
    int R = (int)(2.5f * PT);
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < H; y++)
            for (int c = 0; c < 3; c++) {
                float s = 0;
                for (int x = -R; x <= R; x++) s += B[((size_t)y * W + (x < 0 ? 0 : x)) * 3 + c];
                for (int x = 0; x < W; x++) {
                    t[((size_t)y * W + x) * 3 + c] = s / (2 * R + 1);
                    int a = x - R < 0 ? 0 : x - R, b = x + R + 1 >= W ? W - 1 : x + R + 1;
                    s += B[((size_t)y * W + b) * 3 + c] - B[((size_t)y * W + a) * 3 + c];
                }
            }
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 3; c++) {
                float s = 0;
                for (int y = -R; y <= R; y++) s += t[((size_t)(y < 0 ? 0 : y) * W + x) * 3 + c];
                for (int y = 0; y < H; y++) {
                    B[((size_t)y * W + x) * 3 + c] = s / (2 * R + 1);
                    int a = y - R < 0 ? 0 : y - R, b = y + R + 1 >= H ? H - 1 : y + R + 1;
                    s += t[((size_t)b * W + x) * 3 + c] - t[((size_t)a * W + x) * 3 + c];
                }
            }
    }
    free(t);
}
static void bil(float u, float v, float o[3])       /* the blurred backdrop at the point (u, v) (px), clamped */
{
    u -= .5f; v -= .5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float tu = u - x0, tv = v - y0;
    for (int c = 0; c < 3; c++) {
        float s[4];
        for (int k = 0; k < 4; k++) {
            int x = x0 + (k & 1), y = y0 + (k >> 1);
            x = x < 0 ? 0 : x >= W ? W - 1 : x;
            y = y < 0 ? 0 : y >= H ? H - 1 : y;
            s[k] = B[((size_t)y * W + x) * 3 + c];
        }
        float top = s[0] + (s[1] - s[0]) * tu, bot = s[2] + (s[3] - s[2]) * tu;
        o[c] = top + (bot - top) * tv;
    }
}
static float yc_under(const el *e)                  /* section 2.5's Yc: the blurred backdrop's mean luma, 4 x 4 */
{
    float s = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            float o[3];
            bil(e->x + e->w * (i + .5f) / 4, e->y + e->h * (j + .5f) / 4, o);
            s += .2126f * o[0] + .7152f * o[1] + .0722f * o[2];
        }
    return s / 16;
}

#ifdef LENS_TEST_GLASS_PX
#include LENS_TEST_GLASS_PX
#else
/* one pixel of a glass element over acc (rgba, premultiplied; opaque here): the steps of cpu_px2's lens path */
static void glass_px(const el *e, const float *Lq, float px, float py, float acc[4])
{
    float r = e->r, gx, gy, ox, oy, rgb[3];
    float sd = lens_sd(px, py, e->x + e->w * .5f, e->y + e->h * .5f, e->w * .5f - r, e->h * .5f - r, r, &gx, &gy);
    if (sd > LENS_APRON) return;
    float s = sd < 0 ? -sd : 0;
    lens_offset(Lq, s, gx, gy, &ox, &oy);
    bil(px + ox, py + oy, rgb);
    lens_face(Lq, rgb, s, gx);
    lens_blend(acc, rgb, sd, 1, lens_contour(Lq, sd, gx));
}
static void glass_make(const el *e, float *Lq)
{
    lens_make(Lq, e->w, e->h, PT, e->var, e->tint, e->press, yc_under(e));
}
#endif

static void render(const char *path, const el *els, int n)
{
    float Lq[32][LQ_N];
    for (int i = 0; i < n; i++) glass_make(&els[i], Lq[i]);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    unsigned char *row = malloc((size_t)W * 3);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float acc[4];
            for (int c = 0; c < 3; c++) acc[c] = S[((size_t)y * W + x) * 3 + c];
            acc[3] = 1;
            for (int i = 0; i < n; i++) {
                const el *e = &els[i];
                if (x + .5f < e->x - LENS_APRON || x + .5f > e->x + e->w + LENS_APRON || y + .5f < e->y - LENS_APRON ||
                    y + .5f > e->y + e->h + LENS_APRON)
                    continue;
                glass_px(e, Lq[i], x + .5f, y + .5f, acc);
            }
            for (int c = 0; c < 3; c++) {
                int v = (int)(acc[c] * 255 + .5f);
                row[x * 3 + c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
        fwrite(row, 1, (size_t)W * 3, f);
    }
    free(row);
    fclose(f);
    printf("lens-test: wrote %s\n", path);
}

#ifndef LENS_TEST_GLASS_PX
static void checks_run(void)
{
    float L[LQ_N], ox, oy;
    /* G1: a 390 x 80 pt pill: band 20 pt, amount 40 pt; d = A at the silhouette, 0 at the band's inner edge */
    lens_make(L, 390 * PT, 80 * PT, PT, 0, 0, 0, .1f);
    CHECK(fabsf(1 / L[LQ_IH] - 20 * PT) < .01f, "band %f px", 1 / L[LQ_IH]);
    CHECK(fabsf(L[LQ_A] - 40 * PT * LENS_GAIN) < .01f, "amount %f px", L[LQ_A]);
    lens_offset(L, 0, 0, -1, &ox, &oy);
    CHECK(fabsf(oy - L[LQ_A]) < .01f && ox == 0, "edge offset %f %f (top wall: sampled downward by A)", ox, oy);
    lens_offset(L, 20 * PT, 0, -1, &ox, &oy);
    CHECK(fabsf(oy) < 1e-4f, "band's inner edge offset %f", oy);
    lens_offset(L, 10 * PT, 0, -1, &ox, &oy);
    CHECK(fabsf(oy - L[LQ_A] * (1 - sqrtf(1 - .25f))) < .01f, "mid-band offset %f", oy);
    /* the rim mirrors: the sample point q = s + d(s) falls as s grows near the edge (A / h = 2 > 1) */
    {
        float q0 = 0 + L[LQ_A], s1 = 2 * PT, q1;
        lens_offset(L, s1, 0, -1, &ox, &oy);
        q1 = s1 + oy;
        CHECK(q1 < q0, "no mirrored rim: q(0) %f q(2 pt) %f", q0, q1);
    }
    /* small elements: band .25 short, amount .5 short (a 44 pt button: 11 and 22 pt) */
    lens_make(L, 44 * PT, 44 * PT, PT, 0, 0, 0, .1f);
    CHECK(fabsf(1 / L[LQ_IH] - 11 * PT) < .01f && fabsf(L[LQ_A] - 22 * PT * LENS_GAIN) < .01f, "44 pt: %f %f",
          1 / L[LQ_IH], L[LQ_A]);
    /* G2: the presets (s .2): dark over black = E 32/255; light over white ~ 1; clear over grey keeps it ~ */
    float c[3] = { 0, 0, 0 };
    lens_make(L, 44 * PT, 44 * PT, PT, 0, 0, 0, .1f);
    lens_face(L, c, 100, 1);
    CHECK(fabsf(c[0] - 32.0f / 255) < .002f, "dark face over black %f", c[0]);
    lens_make(L, 44 * PT, 44 * PT, PT, 0, 0, 0, .8f);
    c[0] = c[1] = c[2] = 1;
    lens_face(L, c, 100, 1);
    CHECK(c[0] > .99f, "light face over white %f", c[0]);
    c[0] = c[1] = c[2] = 0;
    lens_face(L, c, 100, 1);
    CHECK(fabsf(c[0] - .9908f * (.403f + .2f * (.726f - .403f))) < .003f, "light face over black %f", c[0]);
    /* the flip only for small elements: a 200 pt card over white stays dark */
    lens_make(L, 200 * PT, 120 * PT, PT, 0, 0, 0, .9f);
    CHECK(fabsf(L[LQ_ER] - 32.0f / 255) < 1e-4f, "large glass flipped: E %f", L[LQ_ER]);
    /* clear: E .126, no flip; dimmed .35 over bright content */
    lens_make(L, 44 * PT, 44 * PT, PT, 1, 0, 0, .1f);
    CHECK(fabsf(L[LQ_ER] - .126f) < 1e-4f && fabsf(L[LQ_TR] - .954f) < 1e-4f, "clear %f %f", L[LQ_ER], L[LQ_TR]);
    lens_make(L, 44 * PT, 44 * PT, PT, 1, 0, 0, .9f);
    CHECK(fabsf(L[LQ_ER] - .126f * .65f) < 1e-4f && fabsf(L[LQ_K] - 1.057f * .65f) < 1e-4f, "clear dim %f %f", L[LQ_ER],
          L[LQ_K]);
    /* G15: the tint folded into E and T equals the spec's mix(face, tone, .8 a) for any backdrop */
    {
        uint32_t tint = 0xcc34c759u;
        float Lt[LQ_N], L0[LQ_N];
        lens_make(Lt, 120 * PT, 50 * PT, PT, 0, tint, 0, .2f);
        lens_make(L0, 120 * PT, 50 * PT, PT, 0, 0, 0, .2f);
        float worst = 0;
        for (int k = 0; k < 200; k++) {
            float bg[3] = { hash(k * 3) * .9f, hash(k * 3 + 1) * .9f, hash(k * 3 + 2) * .9f }, a[3], b[3];
            memcpy(a, bg, sizeof a);
            memcpy(b, bg, sizeof b);
            lens_face(Lt, a, 100, 1);           /* deep inside (no glint) */
            lens_face(L0, b, 100, 1);
            float Y = lens_luma(bg), lum = Y + L0[LQ_LIFT] * Y * (1 - Y), w = .8f * (float)(tint >> 24) / 255;
            for (int ch = 0; ch < 3; ch++) {
                float tc = (float)(tint >> (16 - 8 * ch) & 255) / 255;
                float tone = .55f * tc + (tc + (1 - tc) * .35f - .55f * tc) * lum;
                /* the face before its clamp: b clamps, so compare only where it did not */
                float want = b[ch] + (tone - b[ch]) * w;
                if (b[ch] > .001f && b[ch] < .999f) worst = fmaxf(worst, fabsf(a[ch] - want));
            }
        }
        CHECK(worst < 2e-3f, "tint fold off by %f", worst);
    }
    /* G4: the glint: top and bottom walls only (|n.x| = 1: none), within ~1.2 pt (+ the faint bleed to 4.8 pt) */
    {
        lens_make(L, 200 * PT, 80 * PT, PT, 0, 0, 0, .1f);
        float a[3] = { .2f, .2f, .2f }, b[3] = { .2f, .2f, .2f }, d[3] = { .2f, .2f, .2f }, e[3] = { .2f, .2f, .2f };
        lens_face(L, a, 0, 0);                  /* the top wall's outer px */
        lens_face(L, b, 0, 1);                  /* a side wall's */
        lens_face(L, d, 10 * PT, 0);            /* well inside */
        lens_face(L, e, 3 * PT, 0);             /* in the bleed */
        CHECK(a[0] > d[0] + .1f, "no glint on the top wall: %f vs %f", a[0], d[0]);
        CHECK(fabsf(b[0] - d[0]) < 1e-5f, "glint on a side wall: %f vs %f", b[0], d[0]);
        CHECK(e[0] > d[0] && e[0] < d[0] + .05f, "the bleed: %f vs %f", e[0], d[0]);
        /* pressed: x 1.5 */
        float Lp[LQ_N];
        lens_make(Lp, 200 * PT, 80 * PT, PT, 0, 0, 1, .1f);
        CHECK(fabsf(Lp[LQ_GL] - 1.5f * L[LQ_GL]) < 1e-6f, "press glint %f", Lp[LQ_GL]);
    }
    /* G5: the contour: outside only, .75 pt; dark: the side walls at .88, none on the top wall */
    {
        lens_make(L, 200 * PT, 80 * PT, PT, 0, 0, 0, .1f);
        float acc[4] = { .5f, .5f, .5f, 1 }, rgb[3] = { 1, 1, 1 };
        lens_blend(acc, rgb, 1.0f, 1, lens_contour(L, 1.0f, 1));
        CHECK(fabsf(acc[0] - .5f * (1 - .88f)) < 1e-4f, "side contour %f", acc[0]);
        float acc2[4] = { .5f, .5f, .5f, 1 };
        lens_blend(acc2, rgb, 1.0f, 1, lens_contour(L, 1.0f, 0));
        CHECK(fabsf(acc2[0] - .5f) < 1e-4f, "top contour %f", acc2[0]);
        float acc3[4] = { .5f, .5f, .5f, 1 };
        lens_blend(acc3, rgb, 1.0f * PT, 1, lens_contour(L, 1.0f * PT, 1));
        CHECK(fabsf(acc3[0] - .5f) < 1e-4f, "contour beyond .75 pt %f", acc3[0]);
        float acc4[4] = { .5f, .5f, .5f, 1 };
        lens_blend(acc4, rgb, -5, .6f, lens_contour(L, -5, 1));
        CHECK(fabsf(acc4[0] - .8f) < 1e-4f, "inside at alpha .6: %f", acc4[0]);
    }
    /* a half frame's record: the same shape at half the px */
    {
        float Lf[LQ_N], Lh[LQ_N];
        lens_make(Lf, 300 * PT, 80 * PT, PT, 0, 0, 0, .1f);
        lens_make(Lh, 150 * PT, 40 * PT, PT * .5f, 0, 0, 0, .1f);
        lens_scale(Lf, .5f);
        float worst = 0;
        for (int k = 0; k < LQ_N; k++) worst = fmaxf(worst, fabsf(Lf[k] - Lh[k]) / fmaxf(1e-3f, fabsf(Lh[k])));
        CHECK(worst < 1e-4f, "lens_scale off by %f", worst);
    }
}
#endif

int main(int argc, char **argv)
{
#ifndef LENS_TEST_GLASS_PX
    checks_run();
    printf("lens-test: %d checks, %d failed\n", checks, fails);
#endif
    if (argc < 2) return fails ? 1 : 0;
    char p[512];
    /* five rows, one per variant: regular over a bright backdrop (light), regular over a dark one (dark), clear,
     * tinted green, pressed */
    static const float rowY[5] = { .82f, .10f, .55f, .30f, .30f };
    const uint32_t green = 0xcc34c759u;
    /* the pill: 360 x 70 pt (a tab bar's) */
    W = (int)(390 * PT);
    float rh = 100 * PT;
    H = (int)(5 * rh);
    make_backdrop(rowY, 5, rh);
    el pills[5];
    for (int i = 0; i < 5; i++)
        pills[i] = (el){ 15 * PT, i * rh + 15 * PT, 360 * PT, 70 * PT, 35 * PT, i == 2, i == 3 ? green : 0, i == 4 ? 1.0f : 0 };
    snprintf(p, sizeof p, "%s/pill.ppm", argv[1]);
    render(p, pills, 5);
    /* the card: 300 x 140 pt, radius 32 (no flip: it is large) */
    free(S); free(B);
    rh = 170 * PT;
    H = (int)(5 * rh);
    make_backdrop(rowY, 5, rh);
    el cards[5];
    for (int i = 0; i < 5; i++)
        cards[i] = (el){ 45 * PT, i * rh + 15 * PT, 300 * PT, 140 * PT, 32 * PT, i == 2, i == 3 ? green : 0, i == 4 ? 1.0f : 0 };
    snprintf(p, sizeof p, "%s/card.ppm", argv[1]);
    render(p, cards, 5);
    /* the circular button: 60 pt, and a 44 pt one beside it */
    free(S); free(B);
    rh = 90 * PT;
    H = (int)(5 * rh);
    make_backdrop(rowY, 5, rh);
    el circ[10];
    for (int i = 0; i < 5; i++) {
        circ[2 * i] = (el){ 60 * PT, i * rh + 15 * PT, 60 * PT, 60 * PT, 30 * PT, i == 2, i == 3 ? green : 0, i == 4 ? 1.0f : 0 };
        circ[2 * i + 1] = (el){ 220 * PT, i * rh + 23 * PT, 44 * PT, 44 * PT, 22 * PT, i == 2, i == 3 ? green : 0, i == 4 ? 1.0f : 0 };
    }
    snprintf(p, sizeof p, "%s/circle.ppm", argv[1]);
    render(p, circ, 10);
    return fails ? 1 : 0;
}
