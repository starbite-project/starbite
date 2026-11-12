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
