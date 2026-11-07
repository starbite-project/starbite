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
