/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite display core (see sbdc_core.h).
 */
#include "sbdc_core.h"

static sbdc_u32 rd(const struct sbdc *d, unsigned b, unsigned off)
{
    return d->io->rd(d->io->ctx, b, off);
}

static void wr(const struct sbdc *d, unsigned b, unsigned off, sbdc_u32 v)
{
    d->io->wr(d->io->ctx, b, off, v);
}

static void delay(const struct sbdc *d, unsigned us)
{
    if (d->io->delay_us)
        d->io->delay_us(d->io->ctx, us);
}

int sbdc_identify(struct sbdc *d, const struct sbdc_io *io, sbdc_u32 fb_pa, unsigned w, unsigned h)
{
    sbdc_u32 path0, end;
    unsigned win;

    d->io = io;
    d->win = SBDC_MAX_WIN;
    d->w = w;
    d->h = h;
    d->fb_pa = fb_pa;
    d->flips = d->latch_timeouts = 0;
    if (!w || !h || w > 4096U || h > 4096U || (fb_pa & 0xfffU))
        return -1;

    /* The live window by geometry, as s9p-decon: enabled and covering the whole panel. */
    path0 = rd(d, SBDC_DECON, SBDC_PATH0);
    end = (sbdc_u32)(h - 1U) << 16 | (sbdc_u32)(w - 1U);
    for (win = 0; win < SBDC_MAX_WIN; ++win) {
        if (!(path0 & (1U << (4U * win))))
            continue;
        if (rd(d, SBDC_DECON, SBDC_WIN_START(win)) == 0 && rd(d, SBDC_DECON, SBDC_WIN_END(win)) == end)
            d->win = win;
    }
    if (d->win == SBDC_MAX_WIN)
        return -2;

    /* The channel: only channel 0 (S-Boot wires the window to it; stock DT default_idma 0). Its
     * active (shadow) bank must scan the firmware framebuffer, its image must be the panel's. */
    if (rd(d, SBDC_DMA, SBDC_CH0 + SBDC_IDMA_SHADOW + SBDC_IDMA_BASE_Y) != fb_pa)
        return -3;
    if (rd(d, SBDC_DMA, SBDC_CH0 + SBDC_IDMA_IMG_SIZE) != ((sbdc_u32)h << 16 | (sbdc_u32)w))
        return -4;
    d->cur_pa = rd(d, SBDC_DMA, SBDC_CH0 + SBDC_IDMA_BASE_Y);
    if (d->cur_pa != fb_pa)
        return -5;
    d->trig_inherited = rd(d, SBDC_DECON, SBDC_TRIG);
    return 0;
}

int sbdc_flip_pending(const struct sbdc *d)
{
    return (rd(d, SBDC_DECON, SBDC_SHADOW_REQ) & (1U << d->win)) != 0;
}

int sbdc_wait_latched(struct sbdc *d, unsigned timeout_us, unsigned step_us)
{
    unsigned t = 0;
    if (!step_us)
        step_us = 50;
    while (sbdc_flip_pending(d)) {
        if (t >= timeout_us) {
            ++d->latch_timeouts;
            return -1;
        }
        delay(d, step_us);
        t += step_us;
    }
    return 0;
}

int sbdc_flip(struct sbdc *d, sbdc_u32 pa)
{
    sbdc_u32 v;
    if (d->win >= SBDC_MAX_WIN || !pa || (pa & 0xfffU) || sbdc_flip_pending(d))
        return -1;
    /* Packed XRGB8888: the vendor and s9p-decon write the Y address and the word after it. */
    wr(d, SBDC_DMA, SBDC_CH0 + SBDC_IDMA_BASE_Y, pa);
    wr(d, SBDC_DMA, SBDC_CH0 + SBDC_IDMA_BASE_Y + 4U, pa);
    wr(d, SBDC_DECON, SBDC_SHADOW_REQ, 1U << d->win);
    /* Command mode, hardware (TE) trigger: make sure it is enabled and unmasked; every other bit of
     * the inherited value (uniLoader's 0x1281 pattern) survives. */
    v = rd(d, SBDC_DECON, SBDC_TRIG);
    if ((v & (SBDC_TRIG_HW_EN | SBDC_TRIG_HW_MASK)) != SBDC_TRIG_HW_EN)
        wr(d, SBDC_DECON, SBDC_TRIG, (v | SBDC_TRIG_HW_EN) & ~SBDC_TRIG_HW_MASK);
    d->cur_pa = pa;
    ++d->flips;
    return 0;
}

void sbdc_frame_irq(struct sbdc *d, int on)
{
    sbdc_u32 v = rd(d, SBDC_DECON, SBDC_INT_EN);
    if (on)
        v |= SBDC_INT_EN_ON | SBDC_INT_EN_FRAME_START;
    else
        v &= ~(SBDC_INT_EN_ON | SBDC_INT_EN_FRAME_START);
    wr(d, SBDC_DECON, SBDC_INT_EN, v);
}

sbdc_u32 sbdc_irq_ack(struct sbdc *d)
{
    sbdc_u32 p = rd(d, SBDC_DECON, SBDC_INT_PEND) &
                 (SBDC_INT_PEND_FRAME_START | SBDC_INT_PEND_FRAME_DONE | SBDC_INT_PEND_EXTRA);
    if (p)
        wr(d, SBDC_DECON, SBDC_INT_PEND, p);
    return p;
}

/* ---- dirty rectangles ---------------------------------------------------------------------- */

void sbdc_rects_clear(struct sbdc_rects *s)
{
    s->n = 0;
}

void sbdc_rects_full(struct sbdc_rects *s, unsigned w, unsigned h)
{
    s->n = 1;
    s->r[0].l = 0;
    s->r[0].t = 0;
    s->r[0].r = (int)w;
    s->r[0].b = (int)h;
}

static int contains(const struct sbdc_rect *a, const struct sbdc_rect *b)
{
    return a->l <= b->l && a->t <= b->t && a->r >= b->r && a->b >= b->b;
}

void sbdc_rects_add(struct sbdc_rects *s, struct sbdc_rect r, unsigned w, unsigned h)
{
    unsigned i;
    if (r.l < 0)
        r.l = 0;
    if (r.t < 0)
        r.t = 0;
    if (r.r > (int)w)
        r.r = (int)w;
    if (r.b > (int)h)
        r.b = (int)h;
    if (r.r <= r.l || r.b <= r.t)
        return;
    for (i = 0; i < s->n; ++i)
        if (contains(&s->r[i], &r))
            return;
    if (s->n < SBDC_MAX_RECTS) {
        s->r[s->n++] = r;
        return;
    }
    /* Too many: one bounding box. */
    for (i = 0; i < s->n; ++i) {
        if (s->r[i].l < r.l)
            r.l = s->r[i].l;
        if (s->r[i].t < r.t)
            r.t = s->r[i].t;
        if (s->r[i].r > r.r)
            r.r = s->r[i].r;
        if (s->r[i].b > r.b)
            r.b = s->r[i].b;
    }
    s->n = 1;
    s->r[0] = r;
}

unsigned long long sbdc_rects_area(const struct sbdc_rects *s)
{
    unsigned long long a = 0;
    for (unsigned i = 0; i < s->n; ++i)
        a += (unsigned long long)(s->r[i].r - s->r[i].l) * (unsigned long long)(s->r[i].b - s->r[i].t);
    return a;
}

void sbdc_swap_init(struct sbdc_swap *s, sbdc_u32 fb_pa, sbdc_u32 second_pa, unsigned w, unsigned h)
{
    s->pa[0] = fb_pa;
    s->pa[1] = second_pa;
    s->front = 0;
    sbdc_rects_clear(&s->stale[0]);
    sbdc_rects_full(&s->stale[1], w, h);  /* ours starts with nothing valid in it */
}

void sbdc_swap_damage(struct sbdc_swap *s, const struct sbdc_rect *r, unsigned n, unsigned w, unsigned h)
{
    for (unsigned i = 0; i < n; ++i) {
        sbdc_rects_add(&s->stale[0], r[i], w, h);
        sbdc_rects_add(&s->stale[1], r[i], w, h);
    }
}

int sbdc_swap_back(const struct sbdc_swap *s)
{
    return 1 - s->front;
}

void sbdc_swap_flipped(struct sbdc_swap *s)
{
    int b = 1 - s->front;
    sbdc_rects_clear(&s->stale[b]);
    s->front = b;
}

/* ---- scaled copies ---------------------------------------------------------------------------- */

void sbdc_blit_c(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                 unsigned scale)
{
    if (r.r <= r.l || r.b <= r.t)
        return;
    for (int y = r.t; y < r.b; ++y) {
        const sbdc_u32 *s = (const sbdc_u32 *)(src + (unsigned)y * src_pitch) + r.l;
        if (scale == 2) {
            /* one 64-bit store per source pixel per row: the pixel twice */
            unsigned long long *d0 = (unsigned long long *)(dst + (unsigned)(2 * y) * dst_pitch) + r.l;
            unsigned long long *d1 = (unsigned long long *)(dst + (unsigned)(2 * y + 1) * dst_pitch) + r.l;
            for (int x = 0; x < r.r - r.l; ++x) {
                unsigned long long v = (unsigned long long)s[x] | (unsigned long long)s[x] << 32;
                d0[x] = v;
                d1[x] = v;
            }
        } else {
            sbdc_u32 *d = (sbdc_u32 *)(dst + (unsigned)y * dst_pitch) + r.l;
            for (int x = 0; x < r.r - r.l; ++x)
                d[x] = s[x];
        }
    }
}

/* ---- brightness ---------------------------------------------------------------------------- */

sbdc_u32 sbdc_level_to_aor(unsigned level)
{
    /* (level, AOR), level rising: S-Boot's darkest, its level 10-19, a mid point, its normal, the
     * vendor's brightest. Linear between points. */
    static const struct { unsigned lv; sbdc_u32 aor; } pt[] = {
        { 0, 0x0ba3 }, { 4, 0x0b7c }, { 31, 0x0800 }, { 64, 0x0440 }, { 100, 0x000c },
    };
    unsigned i;
    if (level >= 100U)
        return SBDC_AOR_MIN;
    for (i = 1; i < sizeof(pt) / sizeof(pt[0]); ++i)
        if (level <= pt[i].lv) {
            unsigned span = pt[i].lv - pt[i - 1].lv, into = level - pt[i - 1].lv;
            sbdc_u32 hi = pt[i - 1].aor, lo = pt[i].aor;
            return hi - (sbdc_u32)(((hi - lo) * into + span / 2U) / span);
        }
    return SBDC_AOR_MIN;
}

int sbdc_dsim_ready(const struct sbdc *d)
{
    return (rd(d, SBDC_DSIM, SBDC_DSIM_CLK_CTRL) & (1U << 20)) && !(rd(d, SBDC_DSIM, SBDC_DSIM_CONFIG) & (1U << 18));
}

static int dsim_wait(struct sbdc *d)
{
    for (unsigned t = 0; t < 50000U; t += 10U) {
        sbdc_u32 s = rd(d, SBDC_DSIM, SBDC_DSIM_INTSRC), f = rd(d, SBDC_DSIM, SBDC_DSIM_FIFOCTRL);
        if ((s & SBDC_DSIM_PH_EMPTY) || ((f & (1U << 10)) && !((f >> 16) & 0x3fU))) {
            wr(d, SBDC_DSIM, SBDC_DSIM_INTSRC, SBDC_DSIM_PH_EMPTY);
            return 0;
        }
        delay(d, 10);
    }
    return -1;
}

int sbdc_dcs_long(struct sbdc *d, const sbdc_u8 *b, unsigned n)
{
    sbdc_u32 hdr;
    if (!n || n > 64U)
        return -1;
    if (((rd(d, SBDC_DSIM, SBDC_DSIM_FIFOCTRL) >> 16) & 0x3fU) && dsim_wait(d))
        return -1;
    wr(d, SBDC_DSIM, SBDC_DSIM_INTSRC, SBDC_DSIM_PH_EMPTY);
    for (unsigned i = 0; i < n; i += 4U) {
        sbdc_u32 w = 0;
        for (unsigned k = 0; k < 4U && i + k < n; ++k)
            w |= (sbdc_u32)b[i + k] << (8U * k);
        wr(d, SBDC_DSIM, SBDC_DSIM_PAYLOAD, w);
    }
    hdr = rd(d, SBDC_DSIM, SBDC_DSIM_PKTHDR) & 0xfe000000U;
    wr(d, SBDC_DSIM, SBDC_DSIM_PKTHDR, hdr | ((n >> 8) & 0xffU) << 16 | (n & 0xffU) << 8 | 0x39U);
    return dsim_wait(d);
}

int sbdc_set_aor(struct sbdc *d, sbdc_u32 aor)
{
    static const sbdc_u8 key_on[3] = { 0xf0, 0x5a, 0x5a };
    static const sbdc_u8 commit[2] = { 0xf7, 0x03 };
    static const sbdc_u8 key_off[3] = { 0xf0, 0xa5, 0xa5 };
    sbdc_u8 b1[3];
    int r = -1;
    if (!sbdc_dsim_ready(d))
        return -2;
    if (aor < SBDC_AOR_MIN)
        aor = SBDC_AOR_MIN;
    if (aor > SBDC_AOR_MAX)
        aor = SBDC_AOR_MAX;
    b1[0] = 0xb1;
    b1[1] = (sbdc_u8)(aor >> 8);
    b1[2] = (sbdc_u8)aor;
    for (int attempt = 0; attempt < 2; ++attempt) {
        r = sbdc_dcs_long(d, key_on, 3);
        if (!r)
            r = sbdc_dcs_long(d, b1, 3);
        if (!r)
            r = sbdc_dcs_long(d, commit, 2);
        (void)sbdc_dcs_long(d, key_off, 3);   /* always lock again */
        if (!r)
            break;
        delay(d, 20000);
    }
    return r;
}

/* ---- modes, rotation and non-integer scales ------------------------------------------------- */

void sbdc_xf_src_size(const struct sbdc_xf *x, unsigned *w, unsigned *h)
{
    int swap = x->rot == SBDC_ROT_90 || x->rot == SBDC_ROT_270;
    *w = swap ? x->lh : x->lw;
    *h = swap ? x->lw : x->lh;
}

/* First panel coordinate whose mode coordinate ((X * den) >> shift) is >= l. */
static unsigned up(unsigned l, const struct sbdc_xf *x)
{
    return ((l << x->shift) + x->den - 1) / x->den;
}

void sbdc_blit_xf_c(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                    const struct sbdc_xf *x)
{
    unsigned sw, sh, W = x->lw, H = x->lh, lx0, lx1, ly0, ly1, X0, X1, Y0, Y1;
    long long pitch = (long long)src_pitch;
    sbdc_xf_src_size(x, &sw, &sh);
    if (r.l < 0)
        r.l = 0;
    if (r.t < 0)
        r.t = 0;
    if (r.r > (int)sw)
        r.r = (int)sw;
    if (r.b > (int)sh)
        r.b = (int)sh;
    if (r.r <= r.l || r.b <= r.t || !x->den)
        return;
    /* the rectangle in mode coordinates (inverse of the source -> mode maps below) */
    switch (x->rot) {
    case SBDC_ROT_90:  lx0 = (unsigned)r.t; lx1 = (unsigned)r.b; ly0 = H - (unsigned)r.r; ly1 = H - (unsigned)r.l; break;
    case SBDC_ROT_180: lx0 = W - (unsigned)r.r; lx1 = W - (unsigned)r.l; ly0 = H - (unsigned)r.b; ly1 = H - (unsigned)r.t; break;
    case SBDC_ROT_270: lx0 = W - (unsigned)r.b; lx1 = W - (unsigned)r.t; ly0 = (unsigned)r.l; ly1 = (unsigned)r.r; break;
    default:           lx0 = (unsigned)r.l; lx1 = (unsigned)r.r; ly0 = (unsigned)r.t; ly1 = (unsigned)r.b; break;
    }
    X0 = up(lx0, x);
    X1 = up(lx1, x);
    Y0 = up(ly0, x);
    Y1 = up(ly1, x);
    if (X1 > x->pw)
        X1 = x->pw;
    if (Y1 > x->ph)
        Y1 = x->ph;
    for (unsigned Y = Y0; Y < Y1; ++Y) {
        unsigned ly = (Y * x->den) >> x->shift;
        const sbdc_u8 *base;
        long long step;
        sbdc_u32 *d = (sbdc_u32 *)(dst + (unsigned long long)Y * dst_pitch);
        /* mode pixel (lx, ly) = presented pixel base + lx * step */
        switch (x->rot) {
        case SBDC_ROT_90:  base = src + (unsigned long long)(H - 1 - ly) * 4; step = pitch; break;         /* sx = H-1-ly, sy = lx */
        case SBDC_ROT_180: base = src + (unsigned long long)(H - 1 - ly) * src_pitch + (unsigned long long)(W - 1) * 4; step = -4; break;
        case SBDC_ROT_270: base = src + (unsigned long long)(W - 1) * src_pitch + (unsigned long long)ly * 4; step = -pitch; break; /* sx = ly, sy = W-1-lx */
        default:           base = src + (unsigned long long)ly * src_pitch; step = 4; break;
        }
        for (unsigned X = X0; X < X1; ++X) {
            unsigned lx = (X * x->den) >> x->shift;
            d[X] = *(const sbdc_u32 *)(base + (long long)lx * step);
        }
    }
}

/* ---- fast paths (T10): NEON on ARM64, bit-identical to the _c versions above (host-tested) ------------ */

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define SBDC_NEON 1
#endif

/* Stores to the panel are 64-bit: on the phone's A55s, 128-bit stores to write-combined memory measured
 * slower than 64-bit ones (T10: 720 upright 3.7 ms with 64-bit stores, 6.0 ms with 128-bit). */
static void row_copy(sbdc_u32 *d, const sbdc_u32 *s, unsigned n)
{
    unsigned i = 0;
    if (n && ((unsigned long long)(d) & 4U)) {   /* to 8-byte alignment */
        d[0] = s[0];
        i = 1;
    }
    for (; i + 2 <= n; i += 2)
        *(unsigned long long *)(d + i) = (unsigned long long)s[i] | (unsigned long long)s[i + 1] << 32;
    for (; i < n; ++i)
        d[i] = s[i];
}

/* d[2i] = d[2i+1] = s[i]: one 64-bit store per source pixel (T7's copy, the fastest measured) */
static void row_double(sbdc_u32 *d, const sbdc_u32 *s, unsigned n)
{
    unsigned i = 0;
    if (!((unsigned long long)(d) & 4U)) {
        for (; i < n; ++i)
            *(unsigned long long *)(d + 2 * i) = (unsigned long long)s[i] | (unsigned long long)s[i] << 32;
    }
    for (; i < n; ++i)
        d[2 * i] = d[2 * i + 1] = s[i];
}

/* Panel pixels X0..X1 of one row at 4/3: d[X] = s[(3X) >> 2]; s holds sw pixels (never read beyond). */
static void row_43(sbdc_u32 *d, const sbdc_u32 *s, unsigned X0, unsigned X1, unsigned sw)
{
    unsigned X = X0;
    for (; X < X1 && (X & 3U); ++X)
        d[X] = s[(X * 3U) >> 2];
#ifdef SBDC_NEON
    {
        /* 8 panel pixels from 6 source pixels: 0,0,1,2 | 3,3,4,5 (byte indices into 32 loaded bytes) */
        static const sbdc_u8 ia[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        static const sbdc_u8 ib[16] = { 12, 13, 14, 15, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 };
        uint8x16_t ta = vld1q_u8(ia), tb = vld1q_u8(ib);
        for (; X + 8 <= X1 && ((X * 3U) >> 2) + 8 <= sw; X += 8) {
            const sbdc_u8 *p = (const sbdc_u8 *)(s + ((X * 3U) >> 2));
            uint8x16x2_t t;
            t.val[0] = vld1q_u8(p);
            t.val[1] = vld1q_u8(p + 16);
            uint32x4_t a = vreinterpretq_u32_u8(vqtbl2q_u8(t, ta)), b = vreinterpretq_u32_u8(vqtbl2q_u8(t, tb));
            vst1_u32(d + X, vget_low_u32(a));      /* 64-bit stores (see row_copy) */
            vst1_u32(d + X + 2, vget_high_u32(a));
            vst1_u32(d + X + 4, vget_low_u32(b));
            vst1_u32(d + X + 6, vget_high_u32(b));
        }
    }
#endif
    for (; X < X1; ++X)
        d[X] = s[(X * 3U) >> 2];
}

void sbdc_blit(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
               unsigned scale)
{
    unsigned n;
    if (r.r <= r.l || r.b <= r.t)
        return;
    n = (unsigned)(r.r - r.l);
    for (int y = r.t; y < r.b; ++y) {
        const sbdc_u32 *s = (const sbdc_u32 *)(src + (unsigned)y * src_pitch) + r.l;
        if (scale == 2) {
            row_double((sbdc_u32 *)(dst + (unsigned)(2 * y) * dst_pitch) + 2 * r.l, s, n);
            row_double((sbdc_u32 *)(dst + (unsigned)(2 * y + 1) * dst_pitch) + 2 * r.l, s, n);
        } else {
            row_copy((sbdc_u32 *)(dst + (unsigned)y * dst_pitch) + r.l, s, n);
        }
    }
}

#define SBDC_TILE_Y 16U                   /* rotated: 16 panel rows (16 source columns) x 64 pixels per tile, */
#define SBDC_TILE_X 64U                   /* so each source cache line is used 16 times while it is in L1 */

void sbdc_blit_xf(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                  const struct sbdc_xf *x)
{
    unsigned sw, sh, W = x->lw, H = x->lh, lx0, lx1, ly0, ly1, X0, X1, Y0, Y1;
    long long pitch = (long long)src_pitch;
    sbdc_xf_src_size(x, &sw, &sh);
    if (r.l < 0)
        r.l = 0;
    if (r.t < 0)
        r.t = 0;
    if (r.r > (int)sw)
        r.r = (int)sw;
    if (r.b > (int)sh)
        r.b = (int)sh;
    if (r.r <= r.l || r.b <= r.t || !x->den)
        return;
    if (x->rot == SBDC_ROT_180) {         /* rare: the scalar path */
        sbdc_blit_xf_c(dst, dst_pitch, src, src_pitch, r, x);
        return;
    }
    switch (x->rot) {
    case SBDC_ROT_90:  lx0 = (unsigned)r.t; lx1 = (unsigned)r.b; ly0 = H - (unsigned)r.r; ly1 = H - (unsigned)r.l; break;
    case SBDC_ROT_270: lx0 = W - (unsigned)r.b; lx1 = W - (unsigned)r.t; ly0 = (unsigned)r.l; ly1 = (unsigned)r.r; break;
    default:           lx0 = (unsigned)r.l; lx1 = (unsigned)r.r; ly0 = (unsigned)r.t; ly1 = (unsigned)r.b; break;
    }
    X0 = up(lx0, x);
    X1 = up(lx1, x);
    Y0 = up(ly0, x);
    Y1 = up(ly1, x);
    if (X1 > x->pw)
        X1 = x->pw;
    if (Y1 > x->ph)
        Y1 = x->ph;
    if (X1 <= X0 || Y1 <= Y0)
        return;

    if (x->rot != SBDC_ROT_90 && x->rot != SBDC_ROT_270) {
        for (unsigned Y = Y0; Y < Y1; ++Y) {
            unsigned ly = (Y * x->den) >> x->shift;
            const sbdc_u32 *s = (const sbdc_u32 *)(src + (unsigned long long)ly * src_pitch);
            sbdc_u32 *d = (sbdc_u32 *)(dst + (unsigned long long)Y * dst_pitch);
            if (x->den == 1 && x->shift == 0)
                row_copy(d + X0, s + X0, X1 - X0);
            else if (x->den == 1 && x->shift == 1 && !(X0 & 1U) && !(X1 & 1U))
                row_double(d + X0, s + X0 / 2, (X1 - X0) / 2);
            else if (x->den == 3 && x->shift == 2)
                row_43(d, s, X0, X1, sw);
            else
                for (unsigned X = X0; X < X1; ++X)
                    d[X] = s[(X * x->den) >> x->shift];
        }
        return;
    }
    /* 90 / 270 with a scratch buffer: SBDC_SCRATCH_ROWS panel rows at a time. Per panel column X the rows'
     * pixels come from one source row (sy = lx) and adjacent columns: gathered into the cached scratch rows,
     * then each row goes to the panel in one sequential pass of 64-bit stores. */
    if (x->scratch) {
        const sbdc_u8 *rowbase = x->rot == SBDC_ROT_90 ? src : src + (unsigned long long)(W - 1) * src_pitch;
        long long step = x->rot == SBDC_ROT_90 ? pitch : -pitch;
        for (unsigned Yb = Y0; Yb < Y1; Yb += SBDC_SCRATCH_ROWS) {
            unsigned n = Y1 - Yb < SBDC_SCRATCH_ROWS ? Y1 - Yb : SBDC_SCRATCH_ROWS;
            unsigned col[SBDC_SCRATCH_ROWS];
            for (unsigned j = 0; j < n; ++j) {
                unsigned ly = ((Yb + j) * x->den) >> x->shift;
                col[j] = (x->rot == SBDC_ROT_90 ? H - 1 - ly : ly) * 4U;   /* byte offset of sx in a row */
            }
            for (unsigned X = X0; X < X1; ++X) {
                const sbdc_u8 *row = rowbase + (long long)((X * x->den) >> x->shift) * step;
                for (unsigned j = 0; j < n; ++j)
                    x->scratch[j * x->pw + X] = *(const sbdc_u32 *)(row + col[j]);
            }
            for (unsigned j = 0; j < n; ++j)
                row_copy((sbdc_u32 *)(dst + (unsigned long long)(Yb + j) * dst_pitch) + X0, x->scratch + j * x->pw + X0,
                         X1 - X0);
        }
        return;
    }
    /* 90 / 270 without one: tiles */
    for (unsigned Yb = Y0; Yb < Y1; Yb += SBDC_TILE_Y) {
        unsigned Ye = Yb + SBDC_TILE_Y < Y1 ? Yb + SBDC_TILE_Y : Y1;
        for (unsigned Xb = X0; Xb < X1; Xb += SBDC_TILE_X) {
            unsigned Xe = Xb + SBDC_TILE_X < X1 ? Xb + SBDC_TILE_X : X1;
            for (unsigned Y = Yb; Y < Ye; ++Y) {
                unsigned ly = (Y * x->den) >> x->shift;
                const sbdc_u8 *base;
                long long step;
                sbdc_u32 *d = (sbdc_u32 *)(dst + (unsigned long long)Y * dst_pitch);
                if (x->rot == SBDC_ROT_90) {
                    base = src + (unsigned long long)(H - 1 - ly) * 4;
                    step = pitch;
                } else {
                    base = src + (unsigned long long)(W - 1) * src_pitch + (unsigned long long)ly * 4;
                    step = -pitch;
                }
                for (unsigned X = Xb; X < Xe; ++X)
                    d[X] = *(const sbdc_u32 *)(base + (long long)((X * x->den) >> x->shift) * step);
            }
        }
    }
}

static int present_simple(const struct sbdc_xf *x, struct sbdc_rect *r)
{
    unsigned rows;
    if (x->rot != SBDC_ROT_0 || x->den != 1 || x->shift > 1)
        return 0;
    rows = x->ph >> x->shift;             /* source rows that fit the panel */
    if (r->b > (int)rows)
        r->b = (int)rows;
    if (r->r > (int)x->lw)
        r->r = (int)x->lw;
    if (r->l < 0)
        r->l = 0;
    if (r->t < 0)
        r->t = 0;
    return 1;
}

void sbdc_present_blit(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                       const struct sbdc_xf *x)
{
    if (present_simple(x, &r))
        sbdc_blit(dst, dst_pitch, src, src_pitch, r, 1U << x->shift);
    else
        sbdc_blit_xf(dst, dst_pitch, src, src_pitch, r, x);
}

void sbdc_present_blit_c(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                         const struct sbdc_xf *x)
{
    if (present_simple(x, &r))
        sbdc_blit_c(dst, dst_pitch, src, src_pitch, r, 1U << x->shift);
    else
        sbdc_blit_xf_c(dst, dst_pitch, src, src_pitch, r, x);
}
