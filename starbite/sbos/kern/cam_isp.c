/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the camera's image pipeline on the processor (no ISP firmware): a Bayer frame to pixels.
 *
 * In:  a RAW10 Bayer frame as CSIS1's WDMA writes it on this phone (each pixel in 16 bits, little-endian, the value in
 *      bits 9:0; seen so by an earlier port's driver on 3 November, its cam_s9_pack12 note) or MIPI-packed RAW10 (4 pixels in 5
 *      bytes, for other sources and the tests). The 3H1's order is GRBG (Samsung's fimc-is-cis-3h1.c, GR_BG).
 * Out: XRGB8888 (0xFFRRGGBB, the panel's and libsb's sbui's format) at any size up to the cell grid, rotated and
 *      mirrored as asked; a 24-bit BMP of a picture for Files / Photos.
 *
 * The steps, each the simplest that gives a recognisable picture:
 *   demosaic   one output pixel per 2 x 2 Bayer cell: R, the mean of the two greens, B ("superpixel"; no
 *              interpolation, no zipper artefacts; the picture is half the sensor's size: 816 x 612 at most)
 *   scale      the cell grid to the output's size by nearest cell (the preview), rotation / mirror on the way
 *   black      the black level off (64 of 1023 for the 3H1)
 *   white      per-channel gains (grey world from cam_isp_stats, within 0.5x..4x), 256 = 1x
 *   gamma      a square root (gamma 2.0) through a 1024-entry table, to 8 bits
 * Not done (CAMERA.md "left"): lens shading, a colour matrix, noise reduction, sharpening, JPEG.
 * Pure C with no library calls: the same file runs in the kernel and in the host test. */
#include "cam.h"

/* the colour of a cell's four places TL, TR, BL, BR: 0 R, 1 Gr (green on a red row), 2 Gb, 3 B */
static const uint8_t place[4][4] = {
    { 0, 1, 2, 3 },                         /* RGGB */
    { 1, 0, 3, 2 },                         /* GRBG */
    { 2, 3, 0, 1 },                         /* GBRG */
    { 3, 2, 1, 0 },                         /* BGGR */
};

static uint8_t lut[1024];
static int lut_ready;

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

static void lut_init(void)
{
    if (lut_ready)
        return;
    /* 255 x sqrt(i / 1023), rounded: sqrt(4 x 65025 x i / 1023), then halved with rounding */
    for (unsigned i = 0; i < 1024; ++i) {
        uint32_t v = (isqrt64((uint64_t)4U * 65025U * i / 1023U) + 1U) >> 1;
        lut[i] = (uint8_t)(v > 255U ? 255U : v);
    }
    lut_ready = 1;
}

uint32_t cam_isp_px(const struct cam_raw *r, unsigned x, unsigned y)
{
    const volatile uint8_t *line = r->p + (size_t)y * r->stride;
    if (r->fmt == CAM_RAW10P) {
        const volatile uint8_t *g = line + (size_t)(x >> 2) * 5U;
        unsigned k = x & 3U;
        return ((uint32_t)g[k] << 2) | ((g[4] >> (2U * k)) & 3U);
    }
    return (uint32_t)line[2U * x] | ((uint32_t)line[2U * x + 1U] << 8);
}

/* a cell's four samples TL, TR, BL, BR (16-bit format: two 32-bit reads, the slow memory read word-wise) */
static void cell(const struct cam_raw *r, unsigned cx, unsigned cy, uint32_t v[4])
{
    unsigned x = 2U * cx, y = 2U * cy;
    if (r->fmt == CAM_RAW16 && !(r->stride & 3U)) {
        const volatile uint32_t *t = (const volatile uint32_t *)(r->p + (size_t)y * r->stride) + cx;
        const volatile uint32_t *b = (const volatile uint32_t *)(r->p + (size_t)(y + 1U) * r->stride) + cx;
        uint32_t a = *t, c = *b;
        v[0] = a & 0xffffU, v[1] = a >> 16, v[2] = c & 0xffffU, v[3] = c >> 16;
        return;
    }
    v[0] = cam_isp_px(r, x, y), v[1] = cam_isp_px(r, x + 1U, y);
    v[2] = cam_isp_px(r, x, y + 1U), v[3] = cam_isp_px(r, x + 1U, y + 1U);
}

void cam_isp_stats(const struct cam_raw *r, unsigned step, struct cam_stats *s)
{
    uint64_t sum[4] = { 0, 0, 0, 0 }, gsq = 0;
    uint32_t n = 0, lo = 0xffffffffU, hi = 0, over = 0, clip = 0;
    const uint8_t *pl = place[r->bayer & 3U];
    unsigned cw = r->w / 2U, ch = r->h / 2U;
    if (!step)
        step = 1;
    for (unsigned cy = step / 2U; cy < ch; cy += step)
        for (unsigned cx = step / 2U; cx < cw; cx += step) {
            uint32_t v[4];
            cell(r, cx, cy, v);
            for (unsigned i = 0; i < 4; ++i) {
                uint32_t w = v[i];
                if (w > 1023U)
                    over++;
                lo = w < lo ? w : lo;
                hi = w > hi ? w : hi;
                w &= 0x3ffU;
                sum[pl[i]] += w;
                if (pl[i] == 1U || pl[i] == 2U) {
                    gsq += (uint64_t)w * w;
                    clip += w >= 1000U;
                }
            }
            n++;
        }
    for (unsigned i = 0; i < 4; ++i)
        s->mean[i] = n ? (uint32_t)(sum[i] / n) : 0;
    s->min = n ? lo : 0;
    s->max = hi;
    s->over = over;
    s->clip = clip;
    s->n = n;
    s->sd_g = 0;
    if (n) {
        uint64_t m = (sum[1] + sum[2]) / (2U * n), e2 = gsq / (2U * n);
        s->sd_g = e2 > m * m ? isqrt64(e2 - m * m) : 0;
    }
}

static uint32_t gain_for(uint32_t g, uint32_t c, unsigned black)
{
    if (g <= black + 2U || c <= black + 2U)
        return 256U;                        /* too dark to judge: no correction */
    uint32_t k = (g - black) * 256U / (c - black);
    return k < 128U ? 128U : k > 1024U ? 1024U : k;
}

void cam_isp_wb_grey(const struct cam_stats *s, unsigned black, struct cam_wb *wb)
{
    uint32_t g = (s->mean[1] + s->mean[2]) / 2U;
    wb->r = gain_for(g, s->mean[0], black);
    wb->g = 256U;
    wb->b = gain_for(g, s->mean[3], black);
}

static uint32_t tone(uint32_t v, uint32_t k)
{
    v &= 0x3ffU;
    uint64_t x = (uint64_t)v * k >> 16;     /* k: the gain and the black-to-white stretch, Q16 */
    return lut[x > 1023U ? 1023U : x];
}

void cam_isp_render(const struct cam_raw *r, const struct cam_wb *wb, const struct cam_out *o, unsigned y0, unsigned y1)
{
    lut_init();
    unsigned cw = r->w / 2U, ch = r->h / 2U, rot = o->xform & 3U, mir = (o->xform & CAM_MIRROR) != 0;
    unsigned rw = (rot & 1U) ? ch : cw, rh = (rot & 1U) ? cw : ch;
    unsigned black = r->black < 1000U ? r->black : 0;
    uint32_t span = 1023U - black, stretch = (1023U << 16) / span;
    /* per channel: (v - black) x gain / 256 x 1023 / span, in Q16 of v - black */
    uint32_t kr = (uint32_t)((uint64_t)stretch * wb->r >> 8), kg = (uint32_t)((uint64_t)stretch * wb->g >> 8),
             kb = (uint32_t)((uint64_t)stretch * wb->b >> 8);
    const uint8_t *pl = place[r->bayer & 3U];
    if (!o->w || !o->h || !cw || !ch)
        return;
    if (y1 > o->h)
        y1 = o->h;
    for (unsigned oy = y0; oy < y1; ++oy) {
        uint32_t *dst = o->px + (size_t)oy * o->stride;
        unsigned ry = (unsigned)(((2ULL * oy + 1U) * rh) / (2ULL * o->h));
        for (unsigned ox = 0; ox < o->w; ++ox) {
            unsigned rx = (unsigned)(((2ULL * ox + 1U) * rw) / (2ULL * o->w)), cx, cy;
            if (mir)
                rx = rw - 1U - rx;
            switch (rot) {
            case 1:  cx = ry, cy = ch - 1U - rx; break;             /* 90 clockwise */
            case 2:  cx = cw - 1U - rx, cy = ch - 1U - ry; break;
            case 3:  cx = cw - 1U - ry, cy = rx; break;             /* 270 clockwise */
            default: cx = rx, cy = ry; break;
            }
            uint32_t v[4], c[4] = { 0, 0, 0, 0 };
            cell(r, cx, cy, v);
            for (unsigned i = 0; i < 4; ++i) {
                uint32_t w = v[i] & 0x3ffU;
                c[pl[i]] = w > black ? w - black : 0;
            }
            uint32_t R = tone(c[0], kr), G = tone((c[1] + c[2]) / 2U, kg), B = tone(c[3], kb);
            dst[ox] = 0xff000000U | (R << 16) | (G << 8) | B;
        }
    }
}

uint32_t cam_isp_bmp_size(unsigned w, unsigned h)
{
    return 54U + ((w * 3U + 3U) & ~3U) * h;
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

uint32_t cam_isp_bmp(const uint32_t *px, unsigned w, unsigned h, unsigned stride, uint8_t *dst, uint32_t cap)
{
    uint32_t row = (w * 3U + 3U) & ~3U, len = cam_isp_bmp_size(w, h);
    if (!w || !h || w > 16384U || h > 16384U || len > cap)
        return 0;
    for (unsigned i = 0; i < 54U; ++i)
        dst[i] = 0;
    dst[0] = 'B', dst[1] = 'M';
    le32(dst + 2, len);
    le32(dst + 10, 54U);                    /* the pixels' offset */
    le32(dst + 14, 40U);                    /* BITMAPINFOHEADER */
    le32(dst + 18, w);
    le32(dst + 22, h);                      /* positive: bottom-up */
    dst[26] = 1;                            /* planes */
    dst[28] = 24;                           /* bits a pixel */
    le32(dst + 34, row * h);
    le32(dst + 38, 2835U);                  /* 72 dpi */
    le32(dst + 42, 2835U);
    for (unsigned y = 0; y < h; ++y) {
        const uint32_t *s = px + (size_t)(h - 1U - y) * stride;
        uint8_t *d = dst + 54U + (size_t)y * row;
        unsigned x = 0;
        for (; x < w; ++x) {
            d[3U * x] = (uint8_t)s[x];
            d[3U * x + 1U] = (uint8_t)(s[x] >> 8);
            d[3U * x + 2U] = (uint8_t)(s[x] >> 16);
        }
        for (unsigned k = 3U * w; k < row; ++k)
            d[k] = 0;
    }
    return len;
}

/* ==== the pipeline v2 (cam.h): area resample, lens shading, white balance, colour matrix, sRGB-like tone ========== */
#define V2_MAXW 1024U                       /* cells across (816) and output pixels across (<= 816) */
#define V2_TILE 8U
static uint16_t v2_la[2U * V2_MAXW + 8U], v2_lb[2U * V2_MAXW + 8U];   /* a raw row pair, cached */
static uint32_t v2_h[V2_MAXW][3];           /* a cell row resampled across: R, G, B (x2 scale), x the column weights */
static uint32_t v2_acc[V2_MAXW][3];         /* the U row being gathered */
static uint32_t v2_tile[V2_TILE][V2_MAXW];  /* finished U rows, written out rotated eight at a time */
static uint16_t v2_c0[V2_MAXW], v2_c1[V2_MAXW], v2_w0[V2_MAXW], v2_w1[V2_MAXW], v2_harea[V2_MAXW];
static uint32_t v2_colk[V2_MAXW], v2_dx2[V2_MAXW];
static unsigned v2_geo_cw, v2_geo_uw, v2_hrow = 0xFFFFFFFFU;   /* the column tables' size; the cell row in v2_h */
static const volatile uint8_t *v2_hp;       /* (whose frame v2_h holds) */
static uint8_t v2_lut[2][4096];
static int v2_lut_ready;

/* small maths for the tables (no libm): log2 / exp2 for x in (0, 1] */
static float v2_log2(float x)
{
    union { float f; uint32_t i; } u = { x };
    float e = (float)(int)((u.i >> 23) & 255U) - 127.0f;
    u.i = (u.i & 0x007FFFFFU) | 0x3F800000U;                /* m in [1, 2) */
    float m = u.f, t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    return e + 2.8853900818f * t * (1.0f + t2 * (0.3333333f + t2 * (0.2f + t2 * 0.1428571f)));
}
static float v2_exp2(float x)
{
    float n = (float)(int)x;
    if (n > x) n -= 1.0f;
    float f = (x - n) * 0.6931471806f;
    float p = 1 + f * (1 + f * (0.5f + f * (0.1666667f + f * (0.0416667f + f * 0.0083333f))));
    union { float f; uint32_t i; } u;
    u.i = (uint32_t)((int)n + 127) << 23;
    return p * u.f;
}
static void v2_tables(void)
{
    if (v2_lut_ready)
        return;
    lut_init();
    for (unsigned i = 0; i < 4096U; ++i) {
        v2_lut[0][i] = lut[i >> 2];                           /* v1's gamma 2.0 */
        float x = (float)i / 4095.0f, y;
        if (x <= 0.0031308f)
            y = 12.92f * x;
        else
            y = 1.055f * v2_exp2(v2_log2(x) / 2.4f) - 0.055f;
        y = y + 0.22f * (y * y * (3.0f - 2.0f * y) - y);     /* a mild S: deeper shadows, brighter highlights */
        int v = (int)(y * 255.0f + 0.5f);
        v2_lut[1][i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    v2_lut_ready = 1;
}
void cam_tune_default(struct cam_tune *t)
{
    /* a moderate saturation matrix, typical of small CMOS sensors behind a phone's lens (to be measured on the S9's
     * raw frames: the face test image saves them) */
    static const int16_t m[9] = { 1587, -410, -153, -256, 1485, -205, 0, -563, 1587 };
    for (unsigned i = 0; i < 9U; ++i)
        t->ccm[i] = m[i];
    t->lsc_corner = 358;                    /* 1.4x at the corners: a phone's front camera loses ~30 % there */
    t->tone = 1;
    t->rsv = 0;
}
unsigned cam_isp_rows(const struct cam_out *o) { return (o->xform & 1U) ? o->w : o->h; }

/* a raw row's samples into a cached line (the 16-bit format in 16-byte loads: the memory is uncached) */
static void v2_line(const struct cam_raw *r, unsigned y, uint16_t *dst)
{
    if (r->fmt == CAM_RAW16 && !(r->stride & 15U) && !((uintptr_t)r->p & 15U)) {
        typedef uint64_t v2u __attribute__((vector_size(16)));
        const volatile v2u *s = (const volatile v2u *)(r->p + (size_t)y * r->stride);
        v2u *d = (v2u *)(void *)dst;
        for (unsigned i = 0; i < (r->w * 2U + 15U) / 16U; ++i)
            d[i] = s[i];
        return;
    }
    for (unsigned x = 0; x < r->w; ++x)
        dst[x] = (uint16_t)cam_isp_px(r, x, y);
}
/* cell row cy across, resampled to uw columns (the column tables), into v2_h */
static void v2_hrow_make(const struct cam_raw *r, unsigned cy, unsigned uw, unsigned black)
{
    v2_line(r, 2U * cy, v2_la);
    v2_line(r, 2U * cy + 1U, v2_lb);
    const uint8_t *pl = place[r->bayer & 3U];
    for (unsigned ux = 0; ux < uw; ++ux) {
        uint32_t s0 = 0, s1 = 0, s2 = 0;
        for (unsigned cx = v2_c0[ux]; cx <= v2_c1[ux]; ++cx) {
            uint32_t w = cx == v2_c0[ux] ? v2_w0[ux] : cx == v2_c1[ux] ? v2_w1[ux] : 256U;
            uint32_t v[4] = { v2_la[2U * cx] & 0x3ffU, v2_la[2U * cx + 1U] & 0x3ffU, v2_lb[2U * cx] & 0x3ffU,
                              v2_lb[2U * cx + 1U] & 0x3ffU }, c[4] = { 0, 0, 0, 0 };
            for (unsigned i = 0; i < 4U; ++i)
                c[pl[i]] = v[i] > black ? v[i] - black : 0;
            s0 += w * 2U * c[0];
            s1 += w * (c[1] + c[2]);
            s2 += w * 2U * c[3];
        }
        v2_h[ux][0] = s0, v2_h[ux][1] = s1, v2_h[ux][2] = s2;
    }
    v2_hrow = cy;
    v2_hp = r->p;
}
/* the unrotated (ux, uy) -> the output's pixel */
static uint32_t *v2_dst(const struct cam_out *o, unsigned uw, unsigned uh, unsigned ux, unsigned uy)
{
    unsigned rx, ry;
    switch (o->xform & 3U) {
    case 1:  ry = ux, rx = uh - 1U - uy; break;
    case 2:  rx = uw - 1U - ux, ry = uh - 1U - uy; break;
    case 3:  ry = uw - 1U - ux, rx = uy; break;
    default: rx = ux, ry = uy; break;
    }
    if (o->xform & CAM_MIRROR)
        rx = o->w - 1U - rx;
    return o->px + (size_t)ry * o->stride + rx;
}
static void v2_flush(const struct cam_out *o, unsigned uw, unsigned uh, unsigned uy0, unsigned n)
{
    for (unsigned ux = 0; ux < uw; ++ux)
        for (unsigned t = 0; t < n; ++t)
            *v2_dst(o, uw, uh, ux, uy0 + t) = v2_tile[t][ux];
}
/* the ROI (output pixels) as an unrotated rectangle */
static void v2_roi_u(const struct cam_out *o, const struct cam_roi *roi, unsigned uw, unsigned uh, unsigned *x0,
                     unsigned *y0, unsigned *x1, unsigned *y1)
{
    *x0 = *y0 = 1, *x1 = *y1 = 0;
    if (!roi || !roi->valid || !roi->w || !roi->h)
        return;
    unsigned ax = roi->x, ay = roi->y, bx = roi->x + roi->w - 1U, by = roi->y + roi->h - 1U;
    if (bx >= o->w) bx = o->w - 1U;
    if (by >= o->h) by = o->h - 1U;
    if (ax > bx || ay > by)
        return;
    unsigned ux[2], uy[2];
    for (int k = 0; k < 2; ++k) {
        unsigned ox = k ? bx : ax, oy = k ? by : ay;
        unsigned rx = (o->xform & CAM_MIRROR) ? o->w - 1U - ox : ox, ry = oy;
        switch (o->xform & 3U) {
        case 1:  ux[k] = ry, uy[k] = uh - 1U - rx; break;
        case 2:  ux[k] = uw - 1U - rx, uy[k] = uh - 1U - ry; break;
        case 3:  ux[k] = uw - 1U - ry, uy[k] = rx; break;
        default: ux[k] = rx, uy[k] = ry; break;
        }
    }
    *x0 = ux[0] < ux[1] ? ux[0] : ux[1], *x1 = ux[0] < ux[1] ? ux[1] : ux[0];
    *y0 = uy[0] < uy[1] ? uy[0] : uy[1], *y1 = uy[0] < uy[1] ? uy[1] : uy[0];
}
void cam_isp_process(const struct cam_raw *r, const struct cam_wb *wb, const struct cam_tune *t, const struct cam_out *o,
                     const struct cam_roi *roi, unsigned y0, unsigned y1, struct cam_pstats *st)
{
    v2_tables();
    unsigned cw = r->w / 2U, ch = r->h / 2U, rot = o->xform & 3U;
    unsigned uw = (rot & 1U) ? o->h : o->w, uh = (rot & 1U) ? o->w : o->h;
    if (!cw || !ch || !uw || !uh || cw > V2_MAXW || uw > V2_MAXW || uw > cw || uh > ch)
        return;
    if (y1 > uh)
        y1 = uh;
    unsigned black = r->black < 1000U ? r->black : 0, span = 1023U - black;
    /* the column tables: U column ux covers cells [a, b) in 1/256 cell */
    if (v2_geo_cw != cw || v2_geo_uw != uw) {
        uint32_t hx = uw / 2U ? uw / 2U : 1U;
        for (unsigned ux = 0; ux < uw; ++ux) {
            uint32_t a = (uint32_t)((uint64_t)ux * cw * 256U / uw), b = (uint32_t)((uint64_t)(ux + 1U) * cw * 256U / uw);
            v2_c0[ux] = (uint16_t)(a >> 8), v2_c1[ux] = (uint16_t)((b - 1U) >> 8);
            v2_w0[ux] = (uint16_t)(v2_c0[ux] == v2_c1[ux] ? b - a : 256U - (a & 255U));
            v2_w1[ux] = (uint16_t)(v2_c0[ux] == v2_c1[ux] ? b - a : b - (v2_c1[ux] << 8));
            v2_harea[ux] = (uint16_t)(b - a);
            v2_colk[ux] = (1U << 24) / (b - a);
            int32_t d = (int32_t)(2U * ux + 1U) - (int32_t)uw;     /* x 2 from the middle */
            v2_dx2[ux] = (uint32_t)((int64_t)d * d * 16384 / (4 * (int64_t)hx * hx));   /* (dx / hx)^2, Q14 */
        }
        v2_geo_cw = cw, v2_geo_uw = uw, v2_hrow = 0xFFFFFFFFU;
    }
    const struct cam_tune *tu = t;
    struct cam_tune def;
    if (!tu) {
        cam_tune_default(&def);
        tu = &def;
    }
    const uint8_t *tone = v2_lut[tu->tone ? 1 : 0];
    unsigned rx0, ry0, rx1, ry1;
    v2_roi_u(o, roi, uw, uh, &rx0, &ry0, &rx1, &ry1);
    uint32_t hy = uh / 2U ? uh / 2U : 1U;
    unsigned tn = 0, tstart = y0;
    for (unsigned uy = y0; uy < y1; ++uy) {
        uint32_t a = (uint32_t)((uint64_t)uy * ch * 256U / uh), b = (uint32_t)((uint64_t)(uy + 1U) * ch * 256U / uh);
        unsigned c0 = a >> 8, c1 = (b - 1U) >> 8;
        for (unsigned ux = 0; ux < uw; ++ux)
            v2_acc[ux][0] = v2_acc[ux][1] = v2_acc[ux][2] = 0;
        for (unsigned cy = c0; cy <= c1; ++cy) {
            uint32_t wy = c0 == c1 ? b - a : cy == c0 ? 256U - (a & 255U) : cy == c1 ? b - (c1 << 8) : 256U;
            if (v2_hrow != cy || v2_hp != r->p)
                v2_hrow_make(r, cy, uw, black);
            for (unsigned ux = 0; ux < uw; ++ux) {
                v2_acc[ux][0] += (v2_h[ux][0] >> 4) * wy;   /* (>> 4: the sums stay in 32 bits) */
                v2_acc[ux][1] += (v2_h[ux][1] >> 4) * wy;
                v2_acc[ux][2] += (v2_h[ux][2] >> 4) * wy;
            }
        }
        /* to 12 bits: acc x 16 / (harea x varea) / (2 x span) x 4095 */
        uint64_t rowk = ((uint64_t)4095U << 28) / ((uint64_t)(b - a) * 2U * span);   /* Q24 after the >> 4 */
        int32_t d = (int32_t)(2U * uy + 1U) - (int32_t)uh;
        uint32_t dy2 = (uint32_t)((int64_t)d * d * 16384 / (4 * (int64_t)hy * hy));
        uint32_t lsc_k = tu->lsc_corner > 256U ? tu->lsc_corner - 256U : 0;
        int in_cy = uy * 3U >= uh && uy * 3U < 2U * uh;
        for (unsigned ux = 0; ux < uw; ++ux) {
            uint32_t L[3];
            /* lens shading: 1 + (corner - 1) x r^2, r^2 = 1 at the corners ((dx/hx)^2 + (dy/hy)^2 over 2) */
            uint32_t g = 256U + (uint32_t)(((uint64_t)lsc_k * ((v2_dx2[ux] + dy2) >> 1)) >> 14);
            for (unsigned c = 0; c < 3U; ++c) {
                uint64_t v = (((uint64_t)v2_acc[ux][c] * v2_colk[ux]) >> 24) * rowk >> 24;
                v = v * g >> 8;
                L[c] = (uint32_t)(v > 4095U ? 4095U : v);
            }
            if (st) {
                uint32_t Y = (L[0] + 2U * L[1] + L[2]) >> 2;
                int clipped = L[0] >= 3890U || L[1] >= 3890U || L[2] >= 3890U;
                st->n++;
                st->sum[0] += L[0], st->sum[1] += L[1], st->sum[2] += L[2];
                st->ysum += Y;
                st->hist[Y >> 8]++;
                st->clip += (uint32_t)clipped;
                if (in_cy && ux * 3U >= uw && ux * 3U < 2U * uw)
                    st->csum += Y, st->cn++;
                if (ux >= rx0 && ux <= rx1 && uy >= ry0 && uy <= ry1)
                    st->rsum += Y, st->rn++;
                uint32_t rr = L[0] * wb->r >> 8, bb = L[2] * wb->b >> 8, gg = L[1] * wb->g >> 8;
                if (!clipped && gg > 160U && (rr > gg ? rr - gg : gg - rr) * 4U < gg && (bb > gg ? bb - gg : gg - bb) * 4U < gg)
                    st->gsum[0] += L[0], st->gsum[1] += L[1], st->gsum[2] += L[2], st->gn++;
            }
            int32_t R = (int32_t)(L[0] * wb->r >> 8), G = (int32_t)(L[1] * wb->g >> 8), B = (int32_t)(L[2] * wb->b >> 8);
            R = R > 4095 ? 4095 : R, G = G > 4095 ? 4095 : G, B = B > 4095 ? 4095 : B;
            int32_t o3[3];
            for (unsigned k = 0; k < 3U; ++k) {
                int32_t v = (tu->ccm[3U * k] * R + tu->ccm[3U * k + 1U] * G + tu->ccm[3U * k + 2U] * B) >> 10;
                o3[k] = v < 0 ? 0 : v > 4095 ? 4095 : v;
            }
            v2_tile[tn][ux] = 0xFF000000U | ((uint32_t)tone[o3[0]] << 16) | ((uint32_t)tone[o3[1]] << 8) | tone[o3[2]];
        }
        if (++tn == V2_TILE) {
            v2_flush(o, uw, uh, tstart, tn);
            tn = 0, tstart = uy + 1U;
        }
    }
    if (tn)
        v2_flush(o, uw, uh, tstart, tn);
}
