// SPDX-License-Identifier: ISC
/*
 * bcmw: a msgbuf ring, host side. Adapted from Linux's brcmfmac commonring.c (Linux 7.1):
 *   Copyright (c) 2014 Broadcom Corporation
 *   (ISC licence: the permission notice in chip.c applies to this file too.)
 *
 * With Broadcom DHD's stamping of host-to-dongle items (bcmdhd dhd_msgbuf.c, read for the facts):
 * every H2D work item carries a per-ring sequence number in the common header's fourth byte
 * ("epoch" = seq % 253, seq from 254) and a phase bit (0x80) in its flags byte, the phase flipped
 * each time an item is allocated at the ring's base, so the first pass carries 0x80. The S9's
 * BCM4361 B2 firmware under the mainline kernel answered unstamped items with ring status
 * BADOPTION (docs/LINUX-REBUILD-PLAN.md 2.1); an earlier port's unpatched brcmfmac ran without it. DHD always
 * stamps, so this does.
 *
 * Reads and writes of the indices go through ring->ops (TCM or host memory); the ring does not
 * know which.
 */
#include "bcmw_priv.h"

void ring_config(struct bcmw_ring *r, uint16_t depth, uint16_t item_len, uint8_t *buf, uint64_t bus, int h2d)
{
    r->depth = depth;
    r->item_len = item_len;
    r->buf = buf;
    r->bus = bus;
    r->h2d = (uint8_t)h2d;
    r->r = 0;
    r->w = 0;
    r->stamp = 0;
    r->phase = 0;
    r->seq = BCMW_EPOCH_INIT;
    r->was_full = 0;
    if (r->ops) {
        r->ops->wr_idx(r, r->r_where, 0);
        r->ops->wr_idx(r, r->w_where, 0);
    }
}

static uint16_t avail(const struct bcmw_ring *r)
{
    return r->r <= r->w ? (uint16_t)(r->depth - r->w + r->r) : (uint16_t)(r->r - r->w);
}

uint16_t ring_space(struct bcmw_ring *r)
{
    uint16_t a = avail(r);
    if (a <= 1 && r->ops) {
        r->r = r->ops->rd_idx(r, r->r_where);
        a = avail(r);
    }
    return a > 1 ? (uint16_t)(a - 1) : 0;
}

uint8_t *ring_reserve(struct bcmw_ring *r, uint16_t n, uint16_t *got)
{
    uint16_t a;
    uint8_t *p;

    *got = 0;
    if (!n || !r->buf)
        return 0;
    a = avail(r);
    if (a <= n && r->ops) {         /* the chip may have read more since: look again */
        uint16_t nr = r->ops->rd_idx(r, r->r_where);
        if (nr < r->depth)
            r->r = nr;
        a = avail(r);
    }
    if (a <= 1) {
        r->was_full = 1;
        return 0;
    }
    p = r->buf + (uint32_t)r->w * r->item_len;
    *got = (uint16_t)(n < a - 1 ? n : a - 1);
    if (*got + r->w > r->depth)
        *got = (uint16_t)(r->depth - r->w);
    r->w = (uint16_t)(r->w + *got);
    if (r->w == r->depth)
        r->w = 0;
    return p;
}

void ring_cancel(struct bcmw_ring *r, uint16_t n)
{
    r->w = r->w >= n ? (uint16_t)(r->w - n) : (uint16_t)(r->depth - (n - r->w));
}

static void stamp(struct bcmw_ring *r)
{
    while (r->stamp != r->w) {
        uint8_t *item = r->buf + (uint32_t)r->stamp * r->item_len;
        if (r->stamp == 0)
            r->phase ^= BCMW_PHASE_BIT;
        item[2] = (uint8_t)((item[2] & ~BCMW_PHASE_BIT) | r->phase);
        item[3] = (uint8_t)(r->seq % BCMW_EPOCH_MODULO);
        r->seq++;
        if (++r->stamp == r->depth)
            r->stamp = 0;
    }
}

void ring_commit(struct bcmw_ring *r)
{
    if (r->h2d)
        stamp(r);
    if (r->ops && r->ops->flush)
        r->ops->flush(r);
    if (r->ops) {
        r->ops->wr_idx(r, r->w_where, r->w);
        r->ops->bell(r);
    }
}

uint8_t *ring_read(struct bcmw_ring *r, uint16_t *n)
{
    if (r->ops) {
        uint16_t nw = r->ops->rd_idx(r, r->w_where);
        if (nw < r->depth)          /* an index past the end is a bad read: keep ours */
            r->w = nw;
    }
    *n = r->w >= r->r ? (uint16_t)(r->w - r->r) : (uint16_t)(r->depth - r->r);
    return *n ? r->buf + (uint32_t)r->r * r->item_len : 0;
}

void ring_consumed(struct bcmw_ring *r, uint16_t n)
{
    r->r = (uint16_t)(r->r + n);
    if (r->r >= r->depth)
        r->r = 0;
    if (r->ops)
        r->ops->wr_idx(r, r->r_where, r->r);
}
