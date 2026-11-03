/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 glass: the wallpaper (the concept's WALL_FS at 720 x 1480), the four BLUR_FS
 * passes of renderWall into blur A (360 x 740) and blur B (180 x 370), the frost plane A5 and
 * BASE (the wallpaper texture bilinear at panel resolution). docs/MENU7-PORT.md 4.1.
 *
 * WALL_FS is split into what does not depend on the uniforms (the night field, washes, silk,
 * vignette: P_FIELD, computed once) and the star / reveal terms, evaluated per frame from row and
 * column tables (the core's pow(a + 1e-4, .62) and the spikes are separable) and a radial table
 * (lit, halo, reveal mask) interpolated at 1/4 px. The tiny stars are a list (WALL_FS adds one
 * only where its cell's hash > .984). Output: an 8-bit RGBA texture, as the concept's FBO.
 *
 * BLUR_FS: every tap is bilinear with clamp to edge; along the pass axis the tap positions have
 * the same fraction for every pixel, so each pass is a discrete kernel of <= 14 integer taps
 * (exactly the bilinear weights), across it the half-texel of a downsampling pass is a 50/50 row
 * blend. Each pass is stored at 8 bits, as the concept's RGBA8 FBOs.
 */
#include "glass_int.h"

#define TEX(a) ((uint32_t *)(a))
static int field_ok;

/* ---- the static field ---- */

/* P_FIELD packing: R 10 bits over [0, .125], G 11 bits over [0, .25], B 11 bits over [0, .5]
 * (the field's maxima are .065, .158, .450) */
static inline uint32_t fpack(float r, float g, float b)
{
    uint32_t R = (uint32_t)(g_sat(r * 8.f) * 1023.f + .5f), G = (uint32_t)(g_sat(g * 4.f) * 2047.f + .5f),
             B = (uint32_t)(g_sat(b * 2.f) * 2047.f + .5f);
    return R << 22 | G << 11 | B;
}
#define FUNP_R(v) ((float)((v) >> 22) * (.125f / 1023.f))
#define FUNP_G(v) ((float)(((v) >> 11) & 2047U) * (.25f / 2047.f))
#define FUNP_B(v) ((float)((v) & 2047U) * (.5f / 2047.f))

/* tiny stars (WALL_FS: cell = floor(p / 46), h = hash(cell) > .984) */
#define NSTAR 96
static struct { float x, y, k; int cx, cy; } star[NSTAR];
static int nstar;

static float whash(float x, float y)   /* WALL_FS hash */
{
    x = g_fract(x * 123.34f);
    y = g_fract(y * 456.21f);
    float d = x * (x + 45.32f) + y * (y + 45.32f);
    x += d;
    y += d;
    return g_fract(x * y);
}

void gw_field(void)
{
    uint32_t *F = TEX(P_FIELD);
    float *cx1 = g_row_tmp, *cx2 = g_row_tmp + WW, *gx1 = g_row_tmp + 2 * WW, *gx2 = g_row_tmp + 3 * WW;
    const float ar = 2960.f / 1440.f, s0x = 1290.f / 1440.f, s0y = 232.f / 1440.f;
    for (int i = 0; i < WW; i++) {
        float ux = (i + .5f) * 2.f / 1440.f;
        cx1[i] = .06f + 1.05f * (1.f - ux) + .05f * g_sin(ux * 3.1f + .6f);
        cx2[i] = .72f + .85f * (1.f - ux) + .08f * g_sin(ux * 2.4f + 2.f);
        gx1[i] = (ux - .08f) * (ux - .08f);
        gx2[i] = (ux - .9f) * (ux - .9f);
    }
    for (int j = 0; j < WH; j++) {
        float uy = (j + .5f) * 2.f / 1440.f;
        g7_poll();
        float t = g_smooth(0.f, ar * .95f, uy);
        float br = g_mix(.011f, .002f, t), bg = g_mix(.024f, .005f, t), bb = g_mix(.066f, .017f, t);
        float gy1 = (uy - 1.55f) * (uy - 1.55f), gy2 = (uy - 1.95f) * (uy - 1.95f);
        float dsy = (uy - s0y) * .82f, vy = (uy - ar * .5f) * .5f;
        uint32_t *row = F + (unsigned)j * WW;
        for (int i = 0; i < WW; i++) {
            float ux = (i + .5f) * 2.f / 1440.f;
            float dsx = ux - s0x, dd2 = dsx * dsx + dsy * dsy, dd = g_sqrt(dd2);
            float r = br, g = bg, b = bb, k;
            k = g_exp(-dd2 / .13f) * .85f;
            r += .03f * k; g += .075f * k; b += .22f * k;
            float edd = g_exp(-dd / .9f);
            k = edd * .5f;
            r += .016f * k; g += .04f * k; b += .12f * k;
            k = g_exp(-(gx1[i] + gy1) / .22f);
            r += .016f * k; g += .04f * k; b += .13f * k;
            k = g_exp(-(gx2[i] + gy2) / .12f);
            r += .012f * k; g += .026f * k; b += .085f * k;
            float s1 = uy - cx1[i];
            float f1 = s1 < 0.f ? g_exp(-s1 * s1 / (.30f * .30f)) : g_exp(-s1 * s1 / (.075f * .075f));
            k = f1 * (.55f + .45f * g_exp(-dd * .9f));
            r += .022f * k; g += .055f * k; b += .15f * k;
            float s2 = uy - cx2[i];
            float f2 = s2 > 0.f ? g_exp(-s2 * s2 / (.34f * .34f)) : g_exp(-s2 * s2 / (.09f * .09f));
            k = f2 * .6f;
            r += .03f * k; g += .035f * k; b += .13f * k;
            float vx = (ux - .5f) * 1.25f;
            float v = 1.f - .32f * g_smooth(.55f, 1.15f, g_sqrt(vx * vx + vy * vy));
            row[i] = fpack(r * v, g * v, b * v);
        }
    }
    /* the tiny stars */
    nstar = 0;
    for (int cy = 0; cy * 46 < 2960; cy++)
        for (int cx = 0; cx * 46 < 1440; cx++) {
            float h = whash((float)cx, (float)cy);
            if (h <= .984f || nstar >= NSTAR)
                continue;
            star[nstar].x = ((float)cx + .5f + (whash(cx + 3.1f, cy + 3.1f) - .5f) * .7f) * 46.f;
            star[nstar].y = ((float)cy + .5f + (whash(cx + 7.7f, cy + 7.7f) - .5f) * .7f) * 46.f;
            star[nstar].k = .42f * (h - .984f) / .016f;
            star[nstar].cx = cx;
            star[nstar].cy = cy;
            nstar++;
        }
    field_ok = 1;
}
int gw_field_valid(void) { return field_ok; }
void gw_field_lost(void) { field_ok = 0; }

int gw_wall_same(const struct g7_wall *a, const struct g7_wall *b)
{
    /* the reveal mask is 1 everywhere once rr - rf is past the farthest pixel (3300 px) */
    int ra = a->rr - a->rf > 3400.f, rb = b->rr - b->rf > 3400.f;
    if (ra != rb)
        return 0;
    if (!ra && (g_abs(a->rr - b->rr) > 1e-4f || g_abs(a->rf - b->rf) > 1e-4f))
        return 0;
    return g_abs(a->sx - b->sx) <= 1e-4f && g_abs(a->sy - b->sy) <= 1e-4f && g_abs(a->si - b->si) <= 1e-4f &&
           g_abs(a->sr - b->sr) <= 1e-4f && g_abs(a->sw - b->sw) <= 1e-4f && g_abs(a->amb - b->amb) <= 1e-4f;
}

/* ---- the wallpaper for a struct g7_wall ---- */

#define RT_STEP 4.f                     /* radial table: 1/4 px */
#define RT_N 14000                      /* 0 .. 3500 px */

void gw_wall(const struct g7_wall *w)
{
    float *rt = (float *)P_TAB;                         /* RT_N x {lit, halo, m} */
    float *cx1 = rt + 3 * RT_N, *cx2 = cx1 + WW, *cp = cx2 + WW;    /* columns */
    float R = g_max(w->sr, 1.f), si = w->si, amb = w->amb;
    for (int k = 0; k < RT_N; k++) {
        float r = (float)k / RT_STEP;
        rt[3 * k] = 1.f + g_exp(-r / 700.f) * si * 1.2f;
        rt[3 * k + 1] = g_exp(-r / (R * 1.5f)) * .85f + g_exp(-r / (R * 6.f)) * .36f + g_exp(-r / (R * 22.f)) * .2f;
        rt[3 * k + 2] = (1.f - g_smooth(w->rr - w->rf, w->rr, r)) * amb;
    }
    for (int i = 0; i < WW; i++) {
        float dx = g_abs((i + .5f) * 2.f - w->sx);
        cx1[i] = g_exp(-dx / (R * 1.25f));
        cx2[i] = g_exp(-dx / (R * .035f));
        cp[i] = dx / R < 1.2f ? g_pow(dx / R + 1e-4f, .62f) : 9.f;
    }
    const float scr = .40f, scg = .64f, scb = 1.f;
    float ssw = g_smooth(0.f, 1.f, w->sw * 1.6f + .15f);
    float cor = g_mix(scr, 1.f, w->sw) * 1.05f, cog = g_mix(scg, 1.f, w->sw) * 1.05f, cob = g_mix(scb, 1.f, w->sw) * 1.05f;
    float *row = g_row_tmp;                             /* 3 x WW */
    uint32_t *T = TEX(P_TMP);
    const uint32_t *F = TEX(P_FIELD);
    for (int j = 0; j < WH; j++) {
        float py = (j + .5f) * 2.f, dy = py - w->sy, ady = g_abs(dy);
        g7_poll();
        float ry1 = g_exp(-ady / (R * 1.25f)), ry2 = g_exp(-ady / (R * .035f));
        float rp = ady / R < 1.2f ? g_pow(ady / R + 1e-4f, .62f) : 9.f;
        const uint32_t *fr = F + (unsigned)j * WW;
        float uy = py / 1440.f, tiny = 1.f - .6f * g_smooth(.4f, 1.8f, uy);
        for (int i = 0; i < WW; i++) {
            float dx = (i + .5f) * 2.f - w->sx;
            float r = g_sqrt(dx * dx + dy * dy);
            float q = r * RT_STEP;
            int k = (int)q;
            float f = q - (float)k;
            if (k >= RT_N - 1) {
                k = RT_N - 2;
                f = 1.f;
            }
            const float *e = rt + 3 * k;
            float lit = e[0] + (e[3] - e[0]) * f, halo = e[1] + (e[4] - e[1]) * f, m = e[2] + (e[5] - e[2]) * f;
            uint32_t v = fr[i];
            float cr = FUNP_R(v) * lit * m, cg = FUNP_G(v) * lit * m, cb = FUNP_B(v) * lit * m;
            float spike = ry2 * cx1[i] + cx2[i] * ry1;
            float hs = (halo + spike * .4f) * ssw;
            float ast = cp[i] + rp;
            float core = ast < 1.f ? g_smooth(1.f, .86f, ast) : 0.f;
            row[3 * i] = cr + (cor * core + scr * hs) * si;
            row[3 * i + 1] = cg + (cog * core + scg * hs) * si;
            row[3 * i + 2] = cb + (cob * core + scb * hs) * si;
        }
        /* tiny stars of this row's cells, before the reveal mask (col *= m * amb) */
        int cy = g_ifloor(py / 46.f);
        for (int s = 0; s < nstar; s++) {
            if (star[s].cy != cy || g_abs(star[s].y - py) > 7.f)
                continue;
            int i0 = g_ifloor((star[s].x - 7.f) * .5f), i1 = g_ifloor((star[s].x + 7.f) * .5f);
            for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < WW; i++) {
                float px = (i + .5f) * 2.f;
                if (g_ifloor(px / 46.f) != star[s].cx)
                    continue;
                float ddx = px - star[s].x, ddy = py - star[s].y, sd2 = ddx * ddx + ddy * ddy;
                float dx = px - w->sx, r = g_sqrt(dx * dx + dy * dy);
                float m = (1.f - g_smooth(w->rr - w->rf, w->rr, r)) * amb;
                float k = g_exp(-sd2 / 3.f) * star[s].k * tiny * m;
                row[3 * i] += .7f * k;
                row[3 * i + 1] += .82f * k;
                row[3 * i + 2] += 1.f * k;
            }
        }
        uint32_t *t = T + (unsigned)j * WW;
        for (int i = 0; i < WW; i++) {
            uint32_t R8 = (uint32_t)(g_sat(row[3 * i]) * 255.f + .5f), G8 = (uint32_t)(g_sat(row[3 * i + 1]) * 255.f + .5f),
                     B8 = (uint32_t)(g_sat(row[3 * i + 2]) * 255.f + .5f);
            t[i] = 0xff000000U | R8 << 16 | G8 << 8 | B8;
        }
    }
}

/* ---- BLUR_FS ---- */

static const float bo[4] = { 0.f, 1.411764705882353f, 3.2941176470588234f, 5.176470588235294f };
static const float bw[4] = { .1964825501511404f, .2969069646728344f, .09447039785044732f, .010381362401148057f };

struct kern { int n; int off[16]; float w[16]; };
/* the pass's kernel along its axis: tap centre c0 = ratio * i + base, taps at c0 + d * bo[k] */
static void mk_kern(struct kern *k, float base, float d)
{
    k->n = 0;
    for (int t = -3; t <= 3; t++) {
        float c = base + d * (t < 0 ? -bo[-t] : bo[t]), wt = bw[t < 0 ? -t : t];
        int x0 = g_ifloor(c);
        float f = c - (float)x0;
        for (int e = 0; e < 2; e++) {
            int o = x0 + e;
            float ww = wt * (e ? f : 1.f - f);
            int j;
            for (j = 0; j < k->n && k->off[j] != o; j++)
                ;
            if (j == k->n) {
                k->off[j] = o;
                k->w[j] = 0.f;
                k->n++;
            }
            k->w[j] += ww;
        }
    }
}
static inline uint32_t q8(float r, float g, float b)
{
    return 0xff000000U | (uint32_t)(g_sat(r) * 255.f + .5f) << 16 | (uint32_t)(g_sat(g) * 255.f + .5f) << 8 |
           (uint32_t)(g_sat(b) * 255.f + .5f);
}
/* one renderWall blur(src, dst, dx, dy) pass; ratio = src / dst size (1 or 2) */
static void blur_pass(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh, int horiz, float d)
{
    int ratio = sw / dw;
    struct kern k;
    /* centre along the axis for dst index 0: (0 + .5) * ratio - .5 */
    mk_kern(&k, .5f * (float)ratio - .5f, d);
    for (int j = 0; j < dh; j++) {
        if (!(j & 7))
            g7_poll();
        for (int i = 0; i < dw; i++) {
            float r = 0.f, g = 0.f, b = 0.f;
            for (int t = 0; t < k.n; t++) {
                uint32_t v;
                if (horiz) {
                    int x = ratio * i + k.off[t];
                    x = x < 0 ? 0 : x >= sw ? sw - 1 : x;
                    if (ratio == 2) {   /* rows 2j, 2j + 1 at .5 each */
                        uint32_t a = src[(unsigned)(2 * j) * sw + x], c = src[(unsigned)(2 * j + 1) * sw + x];
                        r += k.w[t] * .5f * (float)(CR(a) + CR(c));
                        g += k.w[t] * .5f * (float)(CG(a) + CG(c));
                        b += k.w[t] * .5f * (float)(CB(a) + CB(c));
                        continue;
                    }
                    v = src[(unsigned)j * sw + x];
                } else {
                    int y = ratio * j + k.off[t];
                    y = y < 0 ? 0 : y >= sh ? sh - 1 : y;
                    if (ratio == 2) {
                        uint32_t a = src[(unsigned)y * sw + 2 * i], c = src[(unsigned)y * sw + 2 * i + 1];
                        r += k.w[t] * .5f * (float)(CR(a) + CR(c));
                        g += k.w[t] * .5f * (float)(CG(a) + CG(c));
                        b += k.w[t] * .5f * (float)(CB(a) + CB(c));
                        continue;
                    }
                    v = src[(unsigned)y * sw + i];
                }
                r += k.w[t] * (float)CR(v);
                g += k.w[t] * (float)CG(v);
                b += k.w[t] * (float)CB(v);
            }
            dst[(unsigned)j * dw + i] = q8(r * (1.f / 255.f), g * (1.f / 255.f), b * (1.f / 255.f));
        }
    }
}

void gw_blur(void)
{
    uint32_t *wall = TEX(P_TMP), *a1 = TEX(P_SOFT), *a2 = TEX(P_A), *b1 = TEX(P_SOFT + PL_A_BYTES), *b2 = TEX(P_B);
    /* renderWall: blur(wall,a1,2,0); blur(a1,a2,0,1.6); blur(a2,a1,1.6,0); blur(a1,a2,0,1.6);
     *             blur(a2,b1,2,0); blur(b1,b2,0,1.6); blur(b2,b1,1.6,0); blur(b1,b2,0,1.6) */
    blur_pass(wall, WW, WH, a1, AW, AH, 1, 2.f);
    blur_pass(a1, AW, AH, a2, AW, AH, 0, 1.6f);
    blur_pass(a2, AW, AH, a1, AW, AH, 1, 1.6f);
    blur_pass(a1, AW, AH, a2, AW, AH, 0, 1.6f);
    blur_pass(a2, AW, AH, b1, BW, BH, 1, 2.f);
    blur_pass(b1, BW, BH, b2, BW, BH, 0, 1.6f);
    blur_pass(b2, BW, BH, b1, BW, BH, 1, 1.6f);
    blur_pass(b1, BW, BH, b2, BW, BH, 0, 1.6f);
    /* frost(): A * .36 + the four diagonal taps at +-12 panel px = +-3 texels of A, * .16 */
    uint32_t *a5 = TEX(P_A5);
    for (int j = 0; j < AH; j++) {
        int jm = j - 3 < 0 ? 0 : j - 3, jp = j + 3 >= AH ? AH - 1 : j + 3;
        for (int i = 0; i < AW; i++) {
            int im = i - 3 < 0 ? 0 : i - 3, ip = i + 3 >= AW ? AW - 1 : i + 3;
            uint32_t c = a2[j * AW + i], t0 = a2[jm * AW + im], t1 = a2[jm * AW + ip], t2 = a2[jp * AW + im],
                     t3 = a2[jp * AW + ip];
            float r = .36f * CR(c) + .16f * (float)(CR(t0) + CR(t1) + CR(t2) + CR(t3));
            float g = .36f * CG(c) + .16f * (float)(CG(t0) + CG(t1) + CG(t2) + CG(t3));
            float b = .36f * CB(c) + .16f * (float)(CB(t0) + CB(t1) + CB(t2) + CB(t3));
            a5[j * AW + i] = g_pack10(r * (1.f / 255.f), g * (1.f / 255.f), b * (1.f / 255.f));
        }
    }
}

/* ---- BASE: the wallpaper texture bilinear at panel pixel centres (texel x / 2 - .25) ---- */

static void wall_row_panel(const uint32_t *T, int y, int x0, int x1, float *rgb)
{
    float cy = (y + .5f) * .5f - .5f;
    int r0 = g_ifloor(cy);
    float fy = cy - (float)r0;
    int ra = r0 < 0 ? 0 : r0 >= WH ? WH - 1 : r0, rb = r0 + 1 >= WH ? WH - 1 : r0 + 1 < 0 ? 0 : r0 + 1;
    const uint32_t *A = T + (unsigned)ra * WW, *B = T + (unsigned)rb * WW;
    for (int x = x0; x < x1; x++) {
        float cx = (x + .5f) * .5f - .5f;
        int c0 = g_ifloor(cx);
        float fx = cx - (float)c0;
        int ca = c0 < 0 ? 0 : c0, cb = c0 + 1 >= WW ? WW - 1 : c0 + 1;
        uint32_t p00 = A[ca], p01 = A[cb], p10 = B[ca], p11 = B[cb];
        float w00 = (1.f - fx) * (1.f - fy), w01 = fx * (1.f - fy), w10 = (1.f - fx) * fy, w11 = fx * fy;
        float *o = rgb + 3 * (x - x0);
        o[0] = (w00 * CR(p00) + w01 * CR(p01) + w10 * CR(p10) + w11 * CR(p11)) * (1.f / 255.f);
        o[1] = (w00 * CG(p00) + w01 * CG(p01) + w10 * CG(p10) + w11 * CG(p11)) * (1.f / 255.f);
        o[2] = (w00 * CB(p00) + w01 * CB(p01) + w10 * CB(p10) + w11 * CB(p11)) * (1.f / 255.f);
    }
}

void gw_base(void)
{
    uint32_t *base = (uint32_t *)G7_BASE;
    const uint32_t *T = TEX(P_TMP);
    float *row = g_row_tmp;
    for (int y = 0; y < GH; y++) {
        if (!(y & 3))
            g7_poll();
        wall_row_panel(T, y, 0, GW, row);
        uint32_t *o = base + (unsigned)y * GW;
        for (int x = 0; x < GW; x++)
            o[x] = g_pack10(row[3 * x], row[3 * x + 1], row[3 * x + 2]);
    }
}

/* the wallpaper at panel resolution straight from the texture (intro frames, BASE not valid) */
void gw_row_wall(int y, int x0, int x1, float *rgb) { wall_row_panel(TEX(P_TMP), y, x0, x1, rgb); }

/* ---- bilinear samplers (WebGL: texel = uv * size - .5, clamp to edge) ---- */

static inline void bil8(const uint32_t *T, int w, int h, float cx, float cy, float out[3])
{
    int x0 = g_ifloor(cx), y0 = g_ifloor(cy);
    float fx = cx - (float)x0, fy = cy - (float)y0;
    int xa = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= w ? w - 1 : x0 + 1;
    int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;
    uint32_t p00 = T[ya * w + xa], p01 = T[ya * w + xb], p10 = T[yb * w + xa], p11 = T[yb * w + xb];
    float w00 = (1.f - fx) * (1.f - fy), w01 = fx * (1.f - fy), w10 = (1.f - fx) * fy, w11 = fx * fy;
    out[0] = (w00 * CR(p00) + w01 * CR(p01) + w10 * CR(p10) + w11 * CR(p11)) * (1.f / 255.f);
    out[1] = (w00 * CG(p00) + w01 * CG(p01) + w10 * CG(p10) + w11 * CG(p11)) * (1.f / 255.f);
    out[2] = (w00 * CB(p00) + w01 * CB(p01) + w10 * CB(p10) + w11 * CB(p11)) * (1.f / 255.f);
}
static inline void bil10(const uint32_t *T, int w, int h, float cx, float cy, float out[3])
{
    int x0 = g_ifloor(cx), y0 = g_ifloor(cy);
    float fx = cx - (float)x0, fy = cy - (float)y0;
    int xa = x0 < 0 ? 0 : x0 >= w ? w - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= w ? w - 1 : x0 + 1;
    int ya = y0 < 0 ? 0 : y0 >= h ? h - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= h ? h - 1 : y0 + 1;
    uint32_t p00 = T[ya * w + xa], p01 = T[ya * w + xb], p10 = T[yb * w + xa], p11 = T[yb * w + xb];
    float w00 = (1.f - fx) * (1.f - fy), w01 = fx * (1.f - fy), w10 = (1.f - fx) * fy, w11 = fx * fy;
    out[0] = w00 * UNP10_R(p00) + w01 * UNP10_R(p01) + w10 * UNP10_R(p10) + w11 * UNP10_R(p11);
    out[1] = w00 * UNP10_G(p00) + w01 * UNP10_G(p01) + w10 * UNP10_G(p10) + w11 * UNP10_G(p11);
    out[2] = w00 * UNP10_B(p00) + w01 * UNP10_B(p01) + w10 * UNP10_B(p10) + w11 * UNP10_B(p11);
}
void gw_sample_a(float px, float py, float out[3]) { bil8(TEX(P_A), AW, AH, px * .25f - .5f, py * .25f - .5f, out); }
void gw_sample_a5(float px, float py, float out[3]) { bil10(TEX(P_A5), AW, AH, px * .25f - .5f, py * .25f - .5f, out); }
void gw_sample_b(float px, float py, float out[3]) { bil8(TEX(P_B), BW, BH, px * .125f - .5f, py * .125f - .5f, out); }

void gw_row_a5_at(float py, int n, float px0, float dpx, float *rgb)
{
    const uint32_t *T = TEX(P_A5);
    float cy = py * .25f - .5f;
    int y0 = g_ifloor(cy);
    float fy = cy - (float)y0;
    int ya = y0 < 0 ? 0 : y0 >= AH ? AH - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= AH ? AH - 1 : y0 + 1;
    const uint32_t *A = T + ya * AW, *B = T + yb * AW;
    for (int i = 0; i < n; i++) {
        float cx = (px0 + dpx * (float)i) * .25f - .5f;
        int x0 = g_ifloor(cx);
        float fx = cx - (float)x0;
        int xa = x0 < 0 ? 0 : x0 >= AW ? AW - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= AW ? AW - 1 : x0 + 1;
        uint32_t p00 = A[xa], p01 = A[xb], p10 = B[xa], p11 = B[xb];
        float w00 = (1.f - fx) * (1.f - fy), w01 = fx * (1.f - fy), w10 = (1.f - fx) * fy, w11 = fx * fy;
        float *o = rgb + 3 * i;
        o[0] = w00 * UNP10_R(p00) + w01 * UNP10_R(p01) + w10 * UNP10_R(p10) + w11 * UNP10_R(p11);
        o[1] = w00 * UNP10_G(p00) + w01 * UNP10_G(p01) + w10 * UNP10_G(p10) + w11 * UNP10_G(p11);
        o[2] = w00 * UNP10_B(p00) + w01 * UNP10_B(p01) + w10 * UNP10_B(p10) + w11 * UNP10_B(p11);
    }
}
void gw_row_b_at(float py, int n, float px0, float dpx, float *rgb)
{
    const uint32_t *T = TEX(P_B);
    float cy = py * .125f - .5f;
    int y0 = g_ifloor(cy);
    float fy = cy - (float)y0;
    int ya = y0 < 0 ? 0 : y0 >= BH ? BH - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= BH ? BH - 1 : y0 + 1;
    const uint32_t *A = T + ya * BW, *B = T + yb * BW;
    for (int i = 0; i < n; i++) {
        float cx = (px0 + dpx * (float)i) * .125f - .5f;
        int x0 = g_ifloor(cx);
        float fx = cx - (float)x0;
        int xa = x0 < 0 ? 0 : x0 >= BW ? BW - 1 : x0, xb = x0 + 1 < 0 ? 0 : x0 + 1 >= BW ? BW - 1 : x0 + 1;
        uint32_t p00 = A[xa], p01 = A[xb], p10 = B[xa], p11 = B[xb];
        float w00 = (1.f - fx) * (1.f - fy), w01 = fx * (1.f - fy), w10 = (1.f - fx) * fy, w11 = fx * fy;
        float *o = rgb + 3 * i;
        o[0] = (w00 * CR(p00) + w01 * CR(p01) + w10 * CR(p10) + w11 * CR(p11)) * (1.f / 255.f);
        o[1] = (w00 * CG(p00) + w01 * CG(p01) + w10 * CG(p10) + w11 * CG(p11)) * (1.f / 255.f);
        o[2] = (w00 * CB(p00) + w01 * CB(p01) + w10 * CB(p10) + w11 * CB(p11)) * (1.f / 255.f);
    }
}
/* full-resolution rows: blend the two texel rows once, then the fixed horizontal pattern
 * (A: 4 panel px per texel, B: 8) */
static float vrow[3 * (AW + 2)];
static void row_fast(const uint32_t *T, int tw, int th, int pp, int ten, int y, int x0, int x1, float *rgb)
{
    float cy = (y + .5f) / (float)pp - .5f;
    int y0 = g_ifloor(cy);
    float fy = cy - (float)y0;
    int ya = y0 < 0 ? 0 : y0 >= th ? th - 1 : y0, yb = y0 + 1 < 0 ? 0 : y0 + 1 >= th ? th - 1 : y0 + 1;
    const uint32_t *A = T + ya * tw, *B = T + yb * tw;
    int t0 = g_ifloor((x0 + .5f) / (float)pp - .5f), t1 = g_ifloor((x1 - .5f) / (float)pp - .5f) + 1;
    for (int t = t0; t <= t1; t++) {
        int tc = t < 0 ? 0 : t >= tw ? tw - 1 : t;
        uint32_t a = A[tc], b = B[tc];
        float *v = vrow + 3 * (t - t0);
        if (ten) {
            v[0] = UNP10_R(a) + (UNP10_R(b) - UNP10_R(a)) * fy;
            v[1] = UNP10_G(a) + (UNP10_G(b) - UNP10_G(a)) * fy;
            v[2] = UNP10_B(a) + (UNP10_B(b) - UNP10_B(a)) * fy;
        } else {
            v[0] = ((float)CR(a) + ((float)CR(b) - (float)CR(a)) * fy) * (1.f / 255.f);
            v[1] = ((float)CG(a) + ((float)CG(b) - (float)CG(a)) * fy) * (1.f / 255.f);
            v[2] = ((float)CB(a) + ((float)CB(b) - (float)CB(a)) * fy) * (1.f / 255.f);
        }
    }
    for (int x = x0; x < x1; x++) {
        float cx = (x + .5f) / (float)pp - .5f;
        int c0 = g_ifloor(cx);
        float fx = cx - (float)c0;
        const float *v = vrow + 3 * (c0 - t0);
        float *o = rgb + 3 * (x - x0);
        o[0] = v[0] + (v[3] - v[0]) * fx;
        o[1] = v[1] + (v[4] - v[1]) * fx;
        o[2] = v[2] + (v[5] - v[2]) * fx;
    }
}
void gw_row_a5(int y, int x0, int x1, float *rgb) { row_fast(TEX(P_A5), AW, AH, 4, 1, y, x0, x1, rgb); }
void gw_row_b(int y, int x0, int x1, float *rgb) { row_fast(TEX(P_B), BW, BH, 8, 0, y, x0, x1, rgb); }
