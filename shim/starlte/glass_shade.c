/* SPDX-License-Identifier: GPL-2.0-only
 * Boot menu v7 glass: the concept's GLASS_FS (docs/concepts/ios27-boot.html), one row at a time.
 *
 * Per pixel and per shape, in layer order, exactly as the shader: the drop shadow (sdSq offset
 * 24, e^(-d/48), 32 %, smoothstep(-aa, 2 aa, d), culled at 110 / 150 px), coverage
 * clamp(.5 - d / aa), the bezel clamp(.8 r, 16, 64), the normal from central differences of the
 * SDF at 1.5 px where d > -bez - 4, shade() (frost from A5 / B, dispersion from A, vibrancy,
 * tint, milk, edge light, inner band, far side, specular), the lens rule, G7_S_CLIP's viewport
 * fade. Then the SAMSUNG glass wordmark (fld(), its gradient at 2.5 px) and the dither.
 *
 * sdSq's pow(): the continuous corner is homogeneous of degree 1, (mx^n + my^n)^(1/n) =
 * max * g(min / max) with g(t) = (1 + t^n)^(1/n), so a 1025-entry table per exponent (linear
 * interpolation: error < 1e-7 relative) replaces pow. Straight edges never reach the table.
 * The specular lobe exp(-2.2 |dq|^2) is separable: a column table per shape and a row factor.
 */
#include "glass_int.h"

#define GT_N 1024
#define NGT 15                     /* 15 x 1025 floats fit 0x40000 .. 0x50000 */
#define TAB_GT (P_TAB + 0x40000UL)                          /* 8 x 1025 floats */
#define TAB_EX (P_TAB + 0x50000UL)                          /* 3 sets x 20 shapes x 1440 floats */
#define EX_N 1440

struct gsh {
    float cx, cy, hbx, hby;         /* centre, half size */
    float r, eps2;                  /* sdSq's radius after the continuous-corner scale, 1e-3 * 2^(1/n) */
    const float *gt;                /* g(t) for n, 0 when n = 2 */
    float aa, bez, ibez;
    float alpha, s;
    float tr, tg, tb, tA;           /* tint rgb, clamp(T.a + SZ * .2) */
    float mixf, vib, milk;          /* frost toward B, saturation, .016 (1 - FR) */
    int lens, clip;
    float spx, spy, sdx, sdy;       /* specular centre and 1 / (hb * 1.05 + 50) */
    int ex0, exn;                   /* column table: grid index of entry 0, entries */
    float *ex;
};
static struct gsh SH[3][G7_MAX_SHAPES];
static int NSH[3], GRID[3];
static float LIT[3], RIMK[3];

/* g(t) tables by exponent: most shapes have n = 2.6 (r / min half size < .72) or 2 (capsules,
 * no table); the few in between (a lens changing shape) get a slot each, reused across frames */
static float gt_n[NGT];
static uint32_t gt_stamp[NGT], gt_frame = 1;
static int gt_used;

void gs_new_frame(void) { gt_frame++; }

static const float *gtable(float n)
{
    float *base = (float *)TAB_GT;
    int best = -1;
    for (int i = 0; i < gt_used; i++)
        if (gt_n[i] == n) {
            gt_stamp[i] = gt_frame;
            return base + i * (GT_N + 1);
        }
    if (gt_used < NGT)
        best = gt_used++;
    else {
        /* evict a table no shape of this frame uses; if all are in use, take the nearest n */
        for (int i = 0; i < NGT && best < 0; i++)
            if (gt_stamp[i] != gt_frame)
                best = i;
        if (best < 0) {
            best = 0;
            for (int i = 1; i < NGT; i++)
                if (g_abs(gt_n[i] - n) < g_abs(gt_n[best] - n))
                    best = i;
            return base + best * (GT_N + 1);
        }
    }
    float *t = base + best * (GT_N + 1);
    for (int k = 0; k <= GT_N; k++) {
        float x = (float)k / GT_N;
        t[k] = g_pow(1.f + g_pow(x, n), 1.f / n);
    }
    t[0] = 1.f;
    gt_n[best] = n;
    gt_stamp[best] = gt_frame;
    return t;
}

static inline float sdq(const struct gsh *s, float qx, float qy)
{
    float dx = g_abs(qx) - s->hbx + s->r, dy = g_abs(qy) - s->hby + s->r;
    if (dx <= 0.f) {
        if (dy <= 0.f)
            return g_max(dx, dy) - s->r + s->eps2;
        return dy + 1e-3f - s->r;
    }
    if (dy <= 0.f)
        return dx + 1e-3f - s->r;
    float mx = dx + 1e-3f, my = dy + 1e-3f;
    if (!s->gt)
        return g_sqrt(mx * mx + my * my) - s->r;
    float hi = g_max(mx, my), lo = g_min(mx, my);
    float t = lo / hi * (float)GT_N;
    int i = (int)t;
    if (i >= GT_N)
        i = GT_N - 1;
    float f = t - (float)i;
    return hi * (s->gt[i] + (s->gt[i + 1] - s->gt[i]) * f) - s->r;
}

/* sdSq's parameters for (b, r) */
static void sq_params(struct gsh *s, float r)
{
    float mb = g_min(s->hbx, s->hby);
    float k = g_smooth(.72f, .98f, mb > 0.f ? r / mb : 1.f);
    float n = g_mix(2.6f, 2.f, k);
    s->r = g_min(r * g_mix(1.18f, 1.f, k), mb);
    s->gt = n == 2.f ? 0 : gtable(n);
    s->eps2 = 1e-3f * g_exp2(1.f / n);
}

void gs_prepare(int layer, const struct g7_shape *in, unsigned n, const struct gs_ctx *c, float zoom)
{
    int G = layer == 2 ? 2 : 1;
    GRID[layer] = G;
    LIT[layer] = 1.f - c->soft * .85f;
    RIMK[layer] = g_max(2.f, c->K * 1.1f);
    float aa = c->K * 1.25f + c->soft * 30.f;
    int m = 0;
    for (unsigned i = 0; i < n && i < G7_MAX_SHAPES; i++) {
        const struct g7_shape *p = &in[i];
        float x = p->x, y = p->y, w = p->w, h = p->h, r = p->r;
        if (zoom != 1.f) {          /* #menu's scale about (720, 1332) */
            x = 720.f + (x - 720.f) * zoom;
            y = 1332.f + (y - 1332.f) * zoom;
            w *= zoom;
            h *= zoom;
            r *= zoom;
        }
        if (p->a <= .002f || w < 1.f || h < 1.f)
            continue;
        struct gsh *s = &SH[layer][m];
        s->hbx = w * .5f;
        s->hby = h * .5f;
        s->cx = x + s->hbx;
        s->cy = y + s->hby;
        sq_params(s, r);
        s->aa = aa;
        s->bez = g_clamp(r * .8f, 16.f, 64.f);
        s->ibez = 1.f / s->bez;
        s->alpha = p->a;
        s->lens = (p->flags & G7_S_LENS) != 0;
        s->clip = (p->flags & G7_S_CLIP) != 0;
        s->s = p->s;
        float FR = (p->flags & G7_S_FROST) ? 1.f : 0.f;
        float SZ = s->lens ? 0.f : g_smooth(300.f, 900.f, g_min(w, h));
        s->tr = p->tint[0];
        s->tg = p->tint[1];
        s->tb = p->tint[2];
        s->tA = g_sat(p->tint[3] + SZ * .2f);
        s->mixf = g_max(g_max(c->soft * .8f, SZ * .75f), FR * .92f);
        s->vib = g_mix(1.45f, 1.2f, FR);
        s->milk = .016f * (1.f - FR);
        s->spx = s->cx + c->Ldx * s->hbx * .6f;
        s->spy = s->cy + c->Ldy * s->hby * .6f;
        s->sdx = 1.f / (s->hbx * 1.05f + 50.f);
        s->sdy = 1.f / (s->hby * 1.05f + 50.f);
        /* specular column table over the grid columns of the shape box (+ aa) */
        float xa = s->cx - s->hbx - aa - 2.f, xb = s->cx + s->hbx + aa + 2.f;
        int gw = GW / G;
        int i0 = g_ifloor(xa / (float)G - .5f), i1 = g_ifloor(xb / (float)G - .5f) + 1;
        if (i0 < 0)
            i0 = 0;
        if (i1 > gw)
            i1 = gw;
        if (i1 - i0 > EX_N)
            i1 = i0 + EX_N;
        s->ex0 = i0;
        s->exn = i1 > i0 ? i1 - i0 : 0;
        s->ex = (float *)TAB_EX + ((unsigned)layer * G7_MAX_SHAPES + (unsigned)m) * EX_N;
        for (int k = 0; k < s->exn; k++) {
            float px = ((float)(i0 + k) + .5f) * (float)G, dq = (px - s->spx) * s->sdx;
            s->ex[k] = g_exp(-2.2f * dq * dq);
        }
        m++;
    }
    NSH[layer] = m;
}

/* frost(): A5 toward B by mixf, at a panel point */
static inline void frost_at(float px, float py, float mixf, float col[3])
{
    float a[3], b[3];
    gw_sample_a5(px, py, a);
    gw_sample_b(px, py, b);
    col[0] = a[0] + (b[0] - a[0]) * mixf;
    col[1] = a[1] + (b[1] - a[1]) * mixf;
    col[2] = a[2] + (b[2] - a[2]) * mixf;
}

/* shade(): col from the frost sample (already in col), then everything after it */
static inline void shade_tail(float col[3], float d, float nx, float ny, float bend, float tr, float tg, float tb,
                              float tA, float vib, float milk, float lit, float rimk, float Ldx, float Ldy, float spec)
{
    float L = .2126f * col[0] + .7152f * col[1] + .0722f * col[2];
    float r = (L + (col[0] - L) * vib) * 1.1f + .014f;
    float g = (L + (col[1] - L) * vib) * 1.1f + .014f;
    float b = (L + (col[2] - L) * vib) * 1.1f + .014f;
    float k = .42f + L * 1.3f;
    r += (tr * k - r) * tA;
    g += (tg * k - g) * tA;
    b += (tb * k - b) * tA;
    float mk = milk * (1.f - bend);
    r += mk;
    g += mk;
    b += mk;
    float f = nx * Ldx + ny * Ldy, fp = g_max(f, 0.f), fn = g_max(-f, 0.f);
    if (d > -30.f) {
        float rim = g_exp(g_min(d, 0.f) / rimk) * (.07f + .6f * fp * fp + .32f * fn * fn) * lit;
        r += rim;
        g += rim;
        b += rim;
    }
    if (bend > 0.f) {
        float band = bend * (.03f + .08f * fp) * lit, far = 1.f - bend * .14f * fn;
        r = (r + band) * far;
        g = (g + band) * far;
        b = (b + band) * far;
    }
    float sp = spec * .045f * lit;
    col[0] = r + sp;
    col[1] = g + sp;
    col[2] = b + sp;
}

void gs_row(int layer, const struct gs_ctx *c, float py, float px0, int n, float *acc, float *al,
            const float *rowa5, const float *rowb)
{
    int G = GRID[layer];
    float lit = LIT[layer], rimk = RIMK[layer];
    float Ldx = c->Ldx, Ldy = c->Ldy;
    int gx0 = g_ifloor(px0 / (float)G - .5f + .25f);     /* grid index of pixel 0 */
    float fade = g_smooth(170.f, 246.f, py) * (1.f - g_smooth(2480.f, 2620.f, py));
    for (int si = 0; si < NSH[layer]; si++) {
        const struct gsh *s = &SH[layer][si];
        float qy = py - s->cy;
        if (g_abs(qy) > s->hby + 150.f)
            continue;
        float alpha = s->alpha;
        if (s->clip)
            alpha *= fade;
        if (alpha < .002f)
            continue;
        /* pixels with |qx| <= hbx + 110 */
        float xa = s->cx - s->hbx - 110.f, xb = s->cx + s->hbx + 110.f;
        int i0 = (int)__builtin_ceilf((xa - px0) / (float)G - 1e-4f), i1 = g_ifloor((xb - px0) / (float)G + 1e-4f);
        if (i0 < 0)
            i0 = 0;
        if (i1 > n - 1)
            i1 = n - 1;
        if (i0 > i1)
            continue;
        float aa = s->aa, iaa = 1.f / aa, bez = s->bez, ibez = s->ibez;
        float dyq = (py - s->spy) * s->sdy, ey = g_exp(-2.2f * dyq * dyq);
        float deep = -bez - 4.f;
        float k13 = bez * 1.3f * s->s, k8 = 8.f * s->s;
        float omx = 1.f - s->mixf;
        for (int i = i0; i <= i1; i++) {
            float px = px0 + (float)(i * G);
            float qx = px - s->cx;
            float d = sdq(s, qx, qy);
            float *A = acc + 3 * i;
            if (!s->lens && d > -aa) {
                float sd2 = sdq(s, qx, qy - 24.f);
                float sh = .32f * alpha * g_exp(-g_max(sd2, 0.f) * (1.f / 48.f)) * g_smooth(-aa, aa * 2.f, d);
                float k = 1.f - sh;
                A[0] *= k;
                A[1] *= k;
                A[2] *= k;
                al[i] += sh * (1.f - al[i]);
            }
            if (d > aa)
                continue;
            float cov = g_sat(.5f - d * iaa) * alpha;
            int ei = gx0 + i - s->ex0;
            float spec = (ei >= 0 && ei < s->exn ? s->ex[ei] : 0.f) * ey;
            float col[3], e, bend, nx = 0.f, ny = 0.f;
            if (d <= deep) {        /* interior: n = 0, bend = 0, no dispersion */
                const float *fa = rowa5 + 3 * i, *fb = rowb + 3 * i;
                col[0] = fa[0] * omx + fb[0] * s->mixf;
                col[1] = fa[1] * omx + fb[1] * s->mixf;
                col[2] = fa[2] * omx + fb[2] * s->mixf;
                e = 0.f;
                bend = 0.f;
            } else {
                const float h = 1.5f;
                /* straight edges: all four samples on one side's straight part -> the axis */
                float ddx = g_abs(qx) - s->hbx + s->r, ddy = g_abs(qy) - s->hby + s->r;
                if (ddy + h <= 0.f && ddx - h > 0.f) {
                    nx = qx < 0.f ? -1.f : 1.f;
                } else if (ddx + h <= 0.f && ddy - h > 0.f) {
                    ny = qy < 0.f ? -1.f : 1.f;
                } else if (ddx + h <= 0.f && ddy + h <= 0.f && g_abs(ddx - ddy) > 2.f * h) {
                    if (ddx > ddy)
                        nx = qx < 0.f ? -1.f : 1.f;
                    else
                        ny = qy < 0.f ? -1.f : 1.f;
                } else {
                    nx = sdq(s, qx + h, qy) - sdq(s, qx - h, qy);
                    ny = sdq(s, qx, qy + h) - sdq(s, qx, qy - h);
                    float ln = g_sqrt(nx * nx + ny * ny);
                    ln = 1.f / g_max(ln, 1e-5f);
                    nx *= ln;
                    ny *= ln;
                }
                e = g_sat(1.f + d * ibez);
                bend = 1.f - g_sqrt(g_max(1.f - e * e, 0.f));
                float ox = nx * bend * k13, oy = ny * bend * k13;
                if (bend > 0.f) {
                    frost_at(px + ox, py + oy, s->mixf, col);
                    float dx = nx * bend * k8, dy = ny * bend * k8, t[3];
                    gw_sample_a(px + ox + dx, py + oy + dy, t);
                    col[0] += (t[0] - col[0]) * bend;
                    gw_sample_a(px + ox - dx, py + oy - dy, t);
                    col[2] += (t[2] - col[2]) * bend;
                } else {
                    const float *fa = rowa5 + 3 * i, *fb = rowb + 3 * i;
                    col[0] = fa[0] * omx + fb[0] * s->mixf;
                    col[1] = fa[1] * omx + fb[1] * s->mixf;
                    col[2] = fa[2] * omx + fb[2] * s->mixf;
                }
            }
            shade_tail(col, d, nx, ny, bend, s->tr, s->tg, s->tb, s->tA, s->vib, s->milk, lit, rimk, Ldx, Ldy, spec);
            if (s->lens) {
                float k = (1.f - e) * .82f;
                col[0] += (A[0] * 1.07f + .018f - col[0]) * k;
                col[1] += (A[1] * 1.07f + .018f - col[1]) * k;
                col[2] += (A[2] * 1.07f + .018f - col[2]) * k;
            }
            A[0] += (col[0] - A[0]) * cov;
            A[1] += (col[1] - A[1]) * cov;
            A[2] += (col[2] - A[2]) * cov;
            al[i] += cov * (1.f - al[i]);
        }
    }
}

/* ---- the glass wordmark (uText / uCap / uTxP) ---- */

static struct {
    const uint8_t *hm;
    int hw, hh;
    float fx, fy, fw, fh;
    float tm, tf, ta;
    struct gsh cap;
    int on;
} WD;

int gs_load_height(void)
{
    uint32_t sz;
    const struct sb7_height *h = (const struct sb7_height *)sb7_get(SB7_T_HEIGHT, 0, &sz);
    if (!h || sz < sizeof(*h) + (uint32_t)h->w * h->h)
        return 0;
    WD.hm = (const uint8_t *)(h + 1);
    WD.hw = h->w;
    WD.hh = h->h;
    WD.fx = h->fx;
    WD.fy = h->fy;
    WD.fw = h->fw;
    WD.fh = h->fh;
    return 1;
}

void gs_word_prepare(const struct g7_word *w, const struct gs_ctx *c)
{
    (void)c;
    WD.on = WD.hm && w->ta > .001f;
    WD.tm = w->tm;
    WD.tf = w->tf;
    WD.ta = w->ta;
    struct gsh *s = &WD.cap;
    s->hbx = w->cap[2] * .5f;
    s->hby = w->cap[3] * .5f;
    s->cx = w->cap[0];
    s->cy = w->cap[1];
    sq_params(s, s->hby);
    /* shade(p, d, n, uCap.xy, uCap.zw * .5, 18, ...): the specular lobe of the capsule */
    s->spx = s->cx + c->Ldx * s->hbx * .6f;
    s->spy = s->cy + c->Ldy * s->hby * .6f;
    s->sdx = 1.f / (s->hbx * 1.05f + 50.f);
    s->sdy = 1.f / (s->hby * 1.05f + 50.f);
}

int gs_word_rows(int *y0, int *y1, int *x0, int *x1)
{
    if (!WD.on)
        return 0;
    float ax = g_min(WD.fx, WD.cap.cx - WD.cap.hbx - 8.f), bx = g_max(WD.fx + WD.fw, WD.cap.cx + WD.cap.hbx + 8.f);
    float ay = g_min(WD.fy, WD.cap.cy - WD.cap.hby - 8.f), by = g_max(WD.fy + WD.fh, WD.cap.cy + WD.cap.hby + 8.f);
    *x0 = g_ifloor(ax) - 3;
    *x1 = g_ifloor(bx) + 4;
    *y0 = g_ifloor(ay) - 3;
    *y1 = g_ifloor(by) + 4;
    return 1;
}

static inline float fld(float px, float py)
{
    float qx = (px - WD.fx) / WD.fw, qy = (py - WD.fy) / WD.fh, ft = 0.f;
    if (qx > 0.f && qy > 0.f && qx < 1.f && qy < 1.f) {
        float cx = qx * (float)WD.hw - .5f, cy = qy * (float)WD.hh - .5f;
        int x0 = g_ifloor(cx), y0 = g_ifloor(cy);
        float fx = cx - (float)x0, fy = cy - (float)y0;
        int xa = x0 < 0 ? 0 : x0, xb = x0 + 1 >= WD.hw ? WD.hw - 1 : x0 + 1;
        int ya = y0 < 0 ? 0 : y0, yb = y0 + 1 >= WD.hh ? WD.hh - 1 : y0 + 1;
        const uint8_t *m = WD.hm;
        float a = m[ya * WD.hw + xa] + (m[ya * WD.hw + xb] - m[ya * WD.hw + xa]) * fx;
        float b = m[yb * WD.hw + xa] + (m[yb * WD.hw + xb] - m[yb * WD.hw + xa]) * fx;
        ft = (a + (b - a) * fy) * (1.f / 255.f);
    }
    float fc = 0.f;
    if (WD.tm > 0.f)
        fc = g_sat(.5f - sdq(&WD.cap, px - WD.cap.cx, py - WD.cap.cy) / 14.f);
    return ft + (fc - ft) * WD.tm;
}

void gs_word_row(const struct gs_ctx *c, float py, int x0, int n, float *acc)
{
    if (!WD.on)
        return;
    float lit = 1.f - c->soft * .85f, rimk = g_max(2.f, c->K * 1.1f);
    const float bez = 18.f;
    float dyq = (py - WD.cap.spy) * WD.cap.sdy, ey = g_exp(-2.2f * dyq * dyq);
    for (int i = 0; i < n; i++) {
        float px = (float)(x0 + i) + .5f;
        float f0 = fld(px, py);
        if (f0 <= .02f)
            continue;
        const float e = 2.5f;
        float gx = fld(px + e, py) - fld(px - e, py), gy = fld(px, py + e) - fld(px, py - e);
        float ln = 1.f / g_max(g_sqrt(gx * gx + gy * gy), 1e-5f);
        float nx = -gx * ln, ny = -gy * ln;
        float d = (.5f - f0) * 30.f;
        float cov = g_smooth(.42f, .58f, f0) * WD.ta;
        /* shade(p, d, n, uCap.xy, uCap.zw * .5, 18., vec4(.02, .035, .08, .12), 1.) with SZ = FR = 0 */
        float ee = g_sat(1.f + d / bez), bend = 1.f - g_sqrt(g_max(1.f - ee * ee, 0.f));
        float ox = nx * bend * bez * 1.3f, oy = ny * bend * bez * 1.3f, col[3], t[3];
        frost_at(px + ox, py + oy, g_max(c->soft * .8f, 0.f), col);
        float dx = nx * bend * 8.f, dy = ny * bend * 8.f;
        gw_sample_a(px + ox + dx, py + oy + dy, t);
        col[0] += (t[0] - col[0]) * bend;
        gw_sample_a(px + ox - dx, py + oy - dy, t);
        col[2] += (t[2] - col[2]) * bend;
        float dxq = (px - WD.cap.spx) * WD.cap.sdx, spec = g_exp(-2.2f * dxq * dxq) * ey;
        shade_tail(col, d, nx, ny, bend, .02f, .035f, .08f, .12f, 1.45f, .016f, lit, rimk, c->Ldx, c->Ldy, spec);
        float *A = acc + 3 * i;
        for (int k = 0; k < 3; k++) {
            float g3 = col[k] + (1.f - col[k]) * WD.tf;
            A[k] += (g3 - A[k]) * cov;
        }
    }
}

/* ---- dither and store ---- */

static inline float dhash(float x, float y)    /* GLASS_FS hash(gl_FragCoord.xy) */
{
    x = g_fract(x * 443.897f);
    y = g_fract(y * 441.423f);
    float d = x * (y + 19.19f) + y * (x + 19.19f);
    x += d;
    y += d;
    return g_fract((x + y) * x);
}
static inline uint32_t q255(float v)
{
    return v <= 0.f ? 0U : v >= 255.f ? 255U : (uint32_t)v;
}

void gs_store_row(uint32_t *dst, int y, int x0, int n, const float *acc)
{
    float fy = (float)GH - (float)y - .5f;
    for (int i = 0; i < n; i++) {
        float h = dhash((float)(x0 + i) + .5f, fy);
        const float *a = acc + 3 * i;
        dst[i] = 0xff000000U | q255(a[0] * 255.f + h) << 16 | q255(a[1] * 255.f + h) << 8 | q255(a[2] * 255.f + h);
    }
}

void gs_over_row(uint32_t *dst, int y, int x0, int n, const float *acc, const float *al)
{
    float fy = (float)GH - (float)y - .5f;
    for (int i = 0; i < n; i++) {
        float a = al[i];
        if (a <= 0.f)
            continue;
        float h = (dhash((float)(x0 + i) + .5f, fy) - .5f) * a, k = 1.f - a;
        const float *c = acc + 3 * i;
        uint32_t v = dst[i];
        float r = g_sat(c[0]) * a * 255.f + h + (float)CR(v) * k + .5f;
        float g = g_sat(c[1]) * a * 255.f + h + (float)CG(v) * k + .5f;
        float b = g_sat(c[2]) * a * 255.f + h + (float)CB(v) * k + .5f;
        dst[i] = 0xff000000U | q255(r) << 16 | q255(g) << 8 | q255(b);
    }
}
