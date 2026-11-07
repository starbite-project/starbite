// SPDX-License-Identifier: ISC
/*
 * bcmw: brcmfmac's msgbuf protocol over PCIe, host side: the shared area the firmware leaves in its
 * RAM, the five common rings, the control buffers, ioctls, events, received frames, the mailbox
 * and the firmware's console.
 *
 * Adapted from Linux's brcmfmac (pcie.c and msgbuf.c, Linux 7.1):
 *   Copyright (c) 2014 Broadcom Corporation
 *   (ISC licence: the permission notice in chip.c applies to this file too.)
 * The layouts below are brcmfmac's structures written out as byte offsets, so that no compiler's
 * packing matters; the host test checks them against a model of the firmware.
 */
#include "bcmw_priv.h"

/* the shared area (pciedev_shared_t), offsets from its start */
#define SH_FLAGS             0
#define SH_CONSOLE_ADDR      20
#define SH_MAX_RXBUFPOST     34
#define SH_RX_DATAOFFSET     36
#define SH_HTOD_MB_DATA_ADDR 40
#define SH_DTOH_MB_DATA_ADDR 44
#define SH_RING_INFO_ADDR    48
#define SH_DMA_SCRATCH_LEN   52
#define SH_DMA_SCRATCH_ADDR  56
#define SH_DMA_RINGUPD_LEN   64
#define SH_DMA_RINGUPD_ADDR  68
/* the ring info (brcmf_pcie_dhi_ringinfo) */
#define RI_RINGMEM           0
#define RI_H2D_W_IDX_PTR     4
#define RI_H2D_R_IDX_PTR     8
#define RI_D2H_W_IDX_PTR     12
#define RI_D2H_R_IDX_PTR     16
#define RI_H2D_W_IDX_HOST    20
#define RI_H2D_R_IDX_HOST    28
#define RI_D2H_W_IDX_HOST    36
#define RI_D2H_R_IDX_HOST    44
#define RI_MAX_FLOWRINGS     52
#define RI_MAX_SUBMISSION    54
#define RI_MAX_COMPLETION    56
#define RI_SIZE              60
/* a ring's descriptor in the chip's RAM (16 bytes each, from ringmem) */
#define RM_MAX_ITEM          4
#define RM_LEN_ITEMS         6
#define RM_BASE_ADDR         8
#define RM_SIZE              16
/* the console */
#define CON_BUFADDR          8
#define CON_BUFSIZE          12
#define CON_WRITEIDX         16

#define D2H_DEV_D3_ACK       0x00000001
#define D2H_DEV_DS_ENTER_REQ 0x00000002
#define D2H_DEV_FWHALT       0x10000000
#define H2D_HOST_DS_ACK      0x00000002
#define PCIE_REG_SBMBX       0x98       /* the endpoint's configuration register */

static const uint16_t ring_items[RING_COMMON] = { 64, 1024, 64, 1024, 1024 };
static const uint16_t ring_size_v7[RING_COMMON] = { 40, 32, 24, 24, 40 };
static const uint16_t ring_size_pre_v7[RING_COMMON] = { 40, 32, 24, 16, 32 };

/* ---- the mailbox registers (the PCIe2 core in BAR0's window) ----------------------------------- */

static uint32_t mbreg_rd(struct bcmw *w, uint32_t off)
{
    chip_select(w, CORE_PCIE2);
    return chip_reg_rd(w, off);
}

static void mbreg_wr(struct bcmw *w, uint32_t off, uint32_t v)
{
    chip_select(w, CORE_PCIE2);
    chip_reg_wr(w, off, v);
}

/* ---- where the indices live --------------------------------------------------------------------- */

/* every index write is preceded by a full barrier, every index read followed by a read barrier
 * (bcmw.h, struct bcmw_os mb/rmb): Linux's iowrite16/readw carry the same barriers for brcmfmac */
static uint16_t tcm_idx_rd(struct bcmw_ring *r, uint32_t where)
{
    uint16_t v = tcm_rd16((struct bcmw *)r->ctx, where);
    bcmw_rmb((struct bcmw *)r->ctx);
    return v;
}

static void tcm_idx_wr(struct bcmw_ring *r, uint32_t where, uint16_t v)
{
    bcmw_mb((struct bcmw *)r->ctx);
    tcm_wr16((struct bcmw *)r->ctx, where, v);
}

static uint16_t host_idx_rd(struct bcmw_ring *r, uint32_t where)
{
    struct bcmw *w = r->ctx;
    uint16_t v;
    bcmw_sync(w, w->idxbuf + where, w->idx_size, 0);
    v = w->idx_size == 2 ? le16(w->idxbuf + where) : (uint16_t)le32(w->idxbuf + where);
    bcmw_rmb(w);
    return v;
}

static void host_idx_wr(struct bcmw_ring *r, uint32_t where, uint16_t v)
{
    struct bcmw *w = r->ctx;
    bcmw_mb(w);
    if (w->idx_size == 2)
        put16(w->idxbuf + where, v);
    else
        put32(w->idxbuf + where, v);
    bcmw_sync(w, w->idxbuf + where, w->idx_size, 1);
}

static void ring_bell(struct bcmw_ring *r)
{
    struct bcmw *w = r->ctx;
    bcmw_wmb(w);
    mbreg_wr(w, w->h2d_mb0, 1);
}

static void ring_flush(struct bcmw_ring *r)
{
    bcmw_sync((struct bcmw *)r->ctx, r->buf, (uint32_t)r->depth * r->item_len, 1);
}

static const struct bcmw_ring_ops tcm_ops = { tcm_idx_rd, tcm_idx_wr, ring_bell, ring_flush };
static const struct bcmw_ring_ops host_ops = { host_idx_rd, host_idx_wr, ring_bell, ring_flush };

/* ---- the shared area ---------------------------------------------------------------------------- */

int mb_shared(struct bcmw *w, uint32_t a)
{
    uint32_t v;
    struct bcmw_lb l;

    w->shared = a;
    w->flags = tcm_rd32(w, a + SH_FLAGS);
    v = w->flags & SHARED_VERSION_MASK;
    w->info.shared_addr = a;
    w->info.shared_flags = w->flags;
    w->info.shared_version = v;
    lb_init(&l);
    lb_s(&l, "msgbuf: 8: shared area at ");
    lb_hex(&l, a, 6);
    lb_s(&l, ", flags ");
    lb_hex(&l, w->flags, 8);
    lb_s(&l, ": protocol version ");
    lb_dec(&l, v);
    bcmw_say_lb(w, &l);
    if (v < 5 || v > 7) {
        bcmw_say(w, "msgbuf: 8: a protocol version outside 5..7");
        return BCMW_E_PROTO;
    }
    w->idx_size = 0;
    if (w->cfg.dma_indices && (w->flags & SHARED_DMA_INDEX))
        w->idx_size = (w->flags & SHARED_DMA_2B_IDX) ? 2 : 4;
    w->info.max_rxbufpost = tcm_rd16(w, a + SH_MAX_RXBUFPOST);
    if (w->info.max_rxbufpost == 0)
        w->info.max_rxbufpost = 255;
    w->info.rx_dataoffset = tcm_rd32(w, a + SH_RX_DATAOFFSET);
    w->htod_mb = tcm_rd32(w, a + SH_HTOD_MB_DATA_ADDR);
    w->dtoh_mb = tcm_rd32(w, a + SH_DTOH_MB_DATA_ADDR);
    w->ring_info = tcm_rd32(w, a + SH_RING_INFO_ADDR);
    w->console_base = tcm_rd32(w, a + SH_CONSOLE_ADDR);
    w->console_ridx = 0;
    w->console_n = 0;
    if (w->console_base) {
        w->console_buf = tcm_rd32(w, w->console_base + CON_BUFADDR);
        w->console_size = tcm_rd32(w, w->console_base + CON_BUFSIZE);
        if (w->console_size > 0x10000)
            w->console_size = 0;
    }
    lb_init(&l);
    lb_s(&l, "msgbuf: 8: max rx buffers ");
    lb_dec(&l, w->info.max_rxbufpost);
    lb_s(&l, ", rx data offset ");
    lb_dec(&l, w->info.rx_dataoffset);
    lb_s(&l, ", ring info at ");
    lb_hex(&l, w->ring_info, 6);
    lb_s(&l, ", console ");
    lb_dec(&l, w->console_size);
    lb_s(&l, " bytes; ");
    lb_s(&l, (w->flags & SHARED_DMA_INDEX) ? "DMA indices offered" : "no DMA indices");
    lb_s(&l, (w->flags & SHARED_D2H_SYNC_SEQNUM) ? ", D2H sequence numbers" : "");
    lb_s(&l, (w->flags & SHARED_HOSTRDY_DB1) ? ", host-ready doorbell" : "");
    bcmw_say_lb(w, &l);
    {
        /* every address the firmware gave must lie in its RAM (0 = none, for the optional ones) */
        uint32_t lo = w->info.rambase, hi = w->info.rambase + w->info.ramsize;
        const uint32_t must[] = { w->ring_info }, may[] = { w->htod_mb, w->dtoh_mb, w->console_base };
        for (unsigned i = 0; i < 1; i++)
            if (must[i] < lo || must[i] + RI_SIZE > hi)
                return BCMW_E_PROTO;
        for (unsigned i = 0; i < 3; i++)
            if (may[i] && (may[i] < lo || may[i] + 20 > hi)) {
                bcmw_sayx(w, "msgbuf: 8: a shared-area address outside the chip's RAM: ", may[i], NULL);
                return BCMW_E_PROTO;
            }
        if (w->console_base && (w->console_buf < lo || w->console_buf + w->console_size > hi))
            w->console_base = 0;            /* no console rather than a wild read */
    }
    mb_console(w);
    return BCMW_OK;
}

/* ---- the rings ---------------------------------------------------------------------------------- */

int mb_rings(struct bcmw *w)
{
    uint8_t ri[RI_SIZE];
    uint32_t ringmem, h2d_w, h2d_r, d2h_w, d2h_r, step;
    unsigned version = w->info.shared_version;
    const uint16_t *sizes = version >= 7 ? ring_size_v7 : ring_size_pre_v7;
    struct bcmw_lb l;

    tcm_read(w, w->ring_info, ri, RI_SIZE);
    if (version >= 6) {
        w->max_sub = le16(ri + RI_MAX_SUBMISSION);
        w->info.max_flowrings = le16(ri + RI_MAX_FLOWRINGS);
        w->max_cpl = le16(ri + RI_MAX_COMPLETION);
    } else {
        w->max_sub = le16(ri + RI_MAX_FLOWRINGS);
        w->info.max_flowrings = w->max_sub - 2u;
        w->max_cpl = 3;
    }
    if (w->info.max_flowrings > 512 || w->max_sub < 2 || w->max_cpl < 3 || w->max_cpl > 64) {
        bcmw_say(w, "msgbuf: 9: the ring counts are not believable");
        return BCMW_E_PROTO;
    }
    if (w->idx_size) {
        uint32_t sz = (uint32_t)(w->max_sub + w->max_cpl) * w->idx_size * 2;
        if (!w->idxbuf || w->idxbuf_size < sz) {
            w->idxbuf = bcmw_dma(w, sz, 64, &w->idxbuf_bus);
            w->idxbuf_size = w->idxbuf ? sz : 0;
        }
        if (!w->idxbuf)
            w->idx_size = 0;
        else
            bcmw_memset(w->idxbuf, 0, sz);
    }
    if (w->idx_size == 0) {
        h2d_w = le32(ri + RI_H2D_W_IDX_PTR);
        h2d_r = le32(ri + RI_H2D_R_IDX_PTR);
        d2h_w = le32(ri + RI_D2H_W_IDX_PTR);
        d2h_r = le32(ri + RI_D2H_R_IDX_PTR);
        step = 4;
    } else {
        uint64_t a = w->idxbuf_bus;
        step = w->idx_size;
        h2d_w = 0;
        put32(ri + RI_H2D_W_IDX_HOST, (uint32_t)a), put32(ri + RI_H2D_W_IDX_HOST + 4, (uint32_t)(a >> 32));
        h2d_r = h2d_w + w->max_sub * step, a += w->max_sub * step;
        put32(ri + RI_H2D_R_IDX_HOST, (uint32_t)a), put32(ri + RI_H2D_R_IDX_HOST + 4, (uint32_t)(a >> 32));
        d2h_w = h2d_r + w->max_sub * step, a += w->max_sub * step;
        put32(ri + RI_D2H_W_IDX_HOST, (uint32_t)a), put32(ri + RI_D2H_W_IDX_HOST + 4, (uint32_t)(a >> 32));
        d2h_r = d2h_w + w->max_cpl * step, a += w->max_cpl * step;
        put32(ri + RI_D2H_R_IDX_HOST, (uint32_t)a), put32(ri + RI_D2H_R_IDX_HOST + 4, (uint32_t)(a >> 32));
        bcmw_sync(w, w->idxbuf, (uint32_t)(w->max_sub + w->max_cpl) * step * 2, 1);
        tcm_write(w, w->ring_info, ri, RI_SIZE);
    }
    ringmem = le32(ri + RI_RINGMEM);
    {
        uint32_t lo = w->info.rambase, hi = w->info.rambase + w->info.ramsize;
        if (ringmem < lo || ringmem + RING_COMMON * RM_SIZE > hi ||
            (w->idx_size == 0 && (h2d_w < lo || h2d_w + 4u * w->max_sub > hi || h2d_r < lo || h2d_r + 4u * w->max_sub > hi ||
                                  d2h_w < lo || d2h_w + 4u * w->max_cpl > hi || d2h_r < lo || d2h_r + 4u * w->max_cpl > hi))) {
            bcmw_say(w, "msgbuf: 9: the ring info points outside the chip's RAM");
            return BCMW_E_PROTO;
        }
    }
    for (unsigned i = 0; i < RING_COMMON; i++) {
        struct bcmw_ring *r = &w->ring[i];
        uint32_t bytes = (uint32_t)ring_items[i] * sizes[i];
        int h2d = i < 2;
        uint8_t *buf;
        uint64_t bus;

        /* allocated for the largest item size (v7), once */
        if (!w->ring_mem[i])
            w->ring_mem[i] = bcmw_dma(w, (uint32_t)ring_items[i] * ring_size_v7[i], 4096, &w->ring_bus[i]);
        buf = w->ring_mem[i];
        bus = w->ring_bus[i];

        if (!buf)
            return BCMW_E_NOMEM;
        bcmw_memset(buf, 0, bytes);
        bcmw_sync(w, buf, bytes, 1);
        tcm_wr32(w, ringmem + RM_BASE_ADDR, (uint32_t)bus);
        tcm_wr32(w, ringmem + RM_BASE_ADDR + 4, (uint32_t)(bus >> 32));
        tcm_wr16(w, ringmem + RM_MAX_ITEM, ring_items[i]);
        tcm_wr16(w, ringmem + RM_LEN_ITEMS, sizes[i]);
        r->ops = w->idx_size ? &host_ops : &tcm_ops;
        r->ctx = w;
        r->id = (uint8_t)i;
        r->w_where = h2d ? h2d_w : d2h_w;
        r->r_where = h2d ? h2d_r : d2h_r;
        ring_config(r, ring_items[i], sizes[i], buf, bus, h2d);
        if (h2d)
            h2d_w += step, h2d_r += step;
        else
            d2h_w += step, d2h_r += step;
        ringmem += RM_SIZE;
    }
    /* the scratch and ring-update buffers the firmware may use */
    if (!w->scratch)
        w->scratch = bcmw_dma(w, 64, 64, &w->scratch_bus);
    if (!w->ringupd)
        w->ringupd = bcmw_dma(w, 1024, 64, &w->ringupd_bus);
    if (!w->scratch || !w->ringupd)
        return BCMW_E_NOMEM;
    tcm_wr32(w, w->shared + SH_DMA_SCRATCH_ADDR, (uint32_t)w->scratch_bus);
    tcm_wr32(w, w->shared + SH_DMA_SCRATCH_ADDR + 4, (uint32_t)(w->scratch_bus >> 32));
    tcm_wr32(w, w->shared + SH_DMA_SCRATCH_LEN, 8);
    tcm_wr32(w, w->shared + SH_DMA_RINGUPD_ADDR, (uint32_t)w->ringupd_bus);
    tcm_wr32(w, w->shared + SH_DMA_RINGUPD_ADDR + 4, (uint32_t)(w->ringupd_bus >> 32));
    tcm_wr32(w, w->shared + SH_DMA_RINGUPD_LEN, 1024);
    lb_init(&l);
    lb_s(&l, "msgbuf: 9: five rings in host memory (protocol ");
    lb_dec(&l, version);
    lb_s(&l, "), indices ");
    lb_s(&l, w->idx_size ? "in host memory by DMA" : "in the chip's RAM");
    lb_s(&l, "; ");
    lb_dec(&l, w->info.max_flowrings);
    lb_s(&l, " flow rings possible");
    bcmw_say_lb(w, &l);
    return BCMW_OK;
}

/* ---- buffers posted to the firmware --------------------------------------------------------------- */

static unsigned post_ctl(struct bcmw *w, int event, unsigned want)
{
    struct bcmw_ring *r = &w->ring[RING_H2D_CONTROL];
    unsigned done = 0;

    while (done < want) {
        unsigned slot = event ? 8 : 0, end = slot + 8;
        uint16_t got;
        uint8_t *it;
        uint64_t a;

        while (slot < end && w->ctl_posted[slot])
            slot++;
        if (slot == end)
            break;
        it = ring_reserve(r, 1, &got);
        if (!it)
            break;
        bcmw_memset(it, 0, r->item_len);
        it[0] = event ? MSGBUF_TYPE_EVENT_BUF_POST : MSGBUF_TYPE_IOCTLRESP_BUF_POST;
        put32(it + 4, PKTID_CTL + slot);
        put16(it + 8, CTL_BUF_SIZE);
        a = w->ctl_bus + (uint64_t)slot * CTL_BUF_SIZE;
        put32(it + 16, (uint32_t)a);
        put32(it + 20, (uint32_t)(a >> 32));
        bcmw_sync(w, w->ctl + slot * CTL_BUF_SIZE, CTL_BUF_SIZE, 1);
        w->ctl_posted[slot] = 1;
        done++;
    }
    if (done)
        ring_commit(r);
    return done;
}

static unsigned post_rx(struct bcmw *w)
{
    struct bcmw_ring *r = &w->ring[RING_H2D_RXPOST];
    unsigned done = 0;

    for (unsigned i = 0; i < w->nrx; i++) {
        uint16_t got;
        uint8_t *it;
        uint64_t a;

        if (w->rx_posted[i])
            continue;
        it = ring_reserve(r, 1, &got);
        if (!it)
            break;
        bcmw_memset(it, 0, r->item_len);
        it[0] = MSGBUF_TYPE_RXBUF_POST;
        put32(it + 4, i);
        put16(it + 10, RX_BUF_SIZE);
        a = w->rxb_bus + (uint64_t)i * RX_BUF_SIZE;
        put32(it + 24, (uint32_t)a);
        put32(it + 28, (uint32_t)(a >> 32));
        bcmw_sync(w, w->rxb + i * RX_BUF_SIZE, RX_BUF_SIZE, 1);
        w->rx_posted[i] = 1;
        done++;
    }
    if (done)
        ring_commit(r);
    return done;
}

int mb_post_all(struct bcmw *w)
{
    unsigned a = post_ctl(w, 0, 8), b = post_ctl(w, 1, 8), c = post_rx(w);
    struct bcmw_lb l;
    lb_init(&l);
    lb_s(&l, "msgbuf: 9: posted ");
    lb_dec(&l, a);
    lb_s(&l, " ioctl response, ");
    lb_dec(&l, b);
    lb_s(&l, " event and ");
    lb_dec(&l, c);
    lb_s(&l, " rx buffers");
    bcmw_say_lb(w, &l);
    return a == 8 && b == 8 && c == w->nrx ? BCMW_OK : BCMW_E_NOMEM;
}

/* ---- what the firmware sends ---------------------------------------------------------------------- */

static uint8_t *ctl_take(struct bcmw *w, uint32_t pktid)
{
    uint32_t slot = pktid - PKTID_CTL;
    if (pktid < PKTID_CTL || slot >= CTL_BUFS || !w->ctl_posted[slot]) {
        bcmw_sayx(w, "msgbuf: a completion names a control buffer that is not ours: ", pktid, NULL);
        return 0;
    }
    w->ctl_posted[slot] = 0;
    bcmw_sync(w, w->ctl + slot * CTL_BUF_SIZE, CTL_BUF_SIZE, 0);
    return w->ctl + slot * CTL_BUF_SIZE;
}

static void got_event(struct bcmw *w, const uint8_t *frame, uint32_t len)
{
    struct bcmw_event ev;
    if (fw_event_parse(frame, len, &ev)) {
        bcmw_sayx(w, "msgbuf: an event frame that does not parse, length ", len, NULL);
        return;
    }
    w->info.events++;
    if (w->os.event)
        w->os.event(w->os.ctx, &ev);
}

static void process_item(struct bcmw *w, const uint8_t *it)
{
    uint8_t type = it[0];
    uint32_t id = le32(it + 4);
    uint16_t status = le16(it + 8);

    switch (type) {
    case MSGBUF_TYPE_GEN_STATUS:
        if (status)
            bcmw_sayx(w, "msgbuf: the firmware reports a general error ", status, NULL);
        break;
    case MSGBUF_TYPE_RING_STATUS:
        if (status) {
            struct bcmw_lb l;
            lb_init(&l);
            lb_s(&l, "msgbuf: the firmware reports ring ");
            lb_dec(&l, le16(it + 10));
            lb_s(&l, " status ");
            lb_dec(&l, status);
            lb_s(&l, status == 3 ? " (BADOPTION: unstamped H2D items?)" : status == 7 ? " (no event buffer)" :
                       status == 8 ? " (no rx buffer)" : "");
            bcmw_say_lb(w, &l);
        }
        break;
    case MSGBUF_TYPE_IOCTLPTR_REQ_ACK:
        if (status)
            bcmw_sayx(w, "msgbuf: the ioctl request was refused at the ring, status ", status, NULL);
        break;
    case MSGBUF_TYPE_IOCTL_CMPLT: {
        uint16_t trans = le16(it + 14);
        if (trans != w->trans_id || w->ioctl_done) {
            /* a late answer to a request that timed out: its buffer back, and go on waiting */
            bcmw_sayx(w, "msgbuf: a late ioctl answer (transaction ", trans, ") dropped");
            ctl_take(w, id);
            post_ctl(w, 0, 1);
            break;
        }
        w->ioctl_status = (int16_t)status;
        w->ioctl_len = le16(it + 12);
        w->ioctl_pktid = id;
        w->ioctl_done = 1;
        break;
    }
    case MSGBUF_TYPE_WL_EVENT: {
        uint16_t len = le16(it + 12);
        uint8_t *b = ctl_take(w, id);
        if (b) {
            uint32_t off = w->info.rx_dataoffset;
            if (off + len <= CTL_BUF_SIZE)
                got_event(w, b + off, len);
        }
        post_ctl(w, 1, 1);
        break;
    }
    case MSGBUF_TYPE_RX_CMPLT: {
        uint16_t len = le16(it + 14), off = le16(it + 16);
        if (id < w->nrx && w->rx_posted[id]) {
            uint8_t *b = w->rxb + id * RX_BUF_SIZE;
            w->rx_posted[id] = 0;
            bcmw_sync(w, b, RX_BUF_SIZE, 0);
            if (!off)
                off = (uint16_t)w->info.rx_dataoffset;
            if (off + (uint32_t)len <= RX_BUF_SIZE) {
                w->info.rx_frames++;
                if (len >= 14 && be16(b + off + 12) == 0x886c)
                    got_event(w, b + off, len);
                else if (w->os.rx)
                    w->os.rx(w->os.ctx, it[1], b + off, len);
            }
        } else {
            bcmw_sayx(w, "msgbuf: rx completion for a buffer that is not ours: ", id, NULL);
        }
        post_rx(w);
        break;
    }
    case MSGBUF_TYPE_TX_STATUS:
    case MSGBUF_TYPE_FLOW_RING_CREATE_CMPLT:
    case MSGBUF_TYPE_FLOW_RING_DELETE_CMPLT:
        break;
    default:
        bcmw_sayx(w, "msgbuf: an item of unknown type ", type, NULL);
        break;
    }
}

static int drain(struct bcmw *w, struct bcmw_ring *r)
{
    int total = 0;
    for (unsigned round = 0; round < 4; round++) {
        uint16_t n;
        uint8_t *p = ring_read(r, &n);
        if (!p)
            break;
        bcmw_sync(w, p, (uint32_t)n * r->item_len, 0);
        for (uint16_t i = 0; i < n; i++)
            /* brcmfmac reads each item at + rx_dataoffset (0 for this firmware, as far as known) */
            process_item(w, p + (uint32_t)i * r->item_len + (w->info.rx_dataoffset < r->item_len ? w->info.rx_dataoffset : 0));
        ring_consumed(r, n);
        total += n;
        if (r->r != 0)      /* not wrapped: nothing more is contiguous */
            break;
    }
    return total;
}

static void mb_data(struct bcmw *w)
{
    uint32_t d;
    if (!w->dtoh_mb)
        return;
    d = tcm_rd32(w, w->dtoh_mb);
    if (!d)
        return;
    tcm_wr32(w, w->dtoh_mb, 0);
    if (d & D2H_DEV_DS_ENTER_REQ) {
        /* deep sleep asked: acknowledged, as brcmfmac does */
        for (unsigned i = 0; i < 100 && tcm_rd32(w, w->htod_mb); i++)
            bcmw_mdelay(w, 10);
        tcm_wr32(w, w->htod_mb, H2D_HOST_DS_ACK);
        ex_cfg_wr(w, PCIE_REG_SBMBX, 1);
        if (chip_core(w, CORE_PCIE2)->rev <= 13)
            ex_cfg_wr(w, PCIE_REG_SBMBX, 1);
    }
    if (d & D2H_DEV_FWHALT) {
        bcmw_say(w, "msgbuf: THE FIRMWARE HALTED (its console follows)");
        w->console_quota += 64;
        mb_console(w);
        w->dead = 1;
    }
}

int mb_process(struct bcmw *w)
{
    uint32_t st;
    int n = 0;

    if (w->dead)
        return 0;
    w->info.polls++;
    st = mbreg_rd(w, w->mb_int);
    if (st == 0xffffffffu) {
        bcmw_say(w, "msgbuf: the mailbox reads all ones: the link is gone");
        w->dead = 1;
        return 0;
    }
    if (st)
        mbreg_wr(w, w->mb_int, st);
    if (st & w->int_fn0)
        mb_data(w);
    /* the rings are looked at whether or not a doorbell was seen: polling must not depend on it */
    n += drain(w, &w->ring[RING_D2H_RXCPL]);
    n += drain(w, &w->ring[RING_D2H_TXCPL]);
    n += drain(w, &w->ring[RING_D2H_CONTROL]);
    mb_console(w);
    return n;
}

/* ---- ioctls --------------------------------------------------------------------------------------- */

int mb_ioctl(struct bcmw *w, unsigned ifidx, uint32_t cmd, void *buf, uint32_t len, int *fwerr)
{
    struct bcmw_ring *r = &w->ring[RING_H2D_CONTROL];
    uint32_t in = len < IOCTL_REQ_SIZE ? len : IOCTL_REQ_SIZE;
    uint16_t got;
    uint8_t *it, *resp;
    uint64_t t0;

    if (fwerr)
        *fwerr = 0;
    if (w->dead)
        return BCMW_E_STATE;
    it = ring_reserve(r, 1, &got);
    if (!it)
        return BCMW_E_NOMEM;
    w->trans_id++;
    bcmw_memset(it, 0, r->item_len);
    it[0] = MSGBUF_TYPE_IOCTLPTR_REQ;
    it[1] = (uint8_t)ifidx;
    put32(it + 4, 0xFFFE);                      /* BRCMF_IOCTL_REQ_PKTID */
    put32(it + 8, cmd);
    put16(it + 12, w->trans_id);
    put16(it + 14, (uint16_t)in);
    put16(it + 16, (uint16_t)(len > 0xffff ? 0xffff : len));
    put32(it + 24, (uint32_t)w->ioctl_req_bus);
    put32(it + 28, (uint32_t)(w->ioctl_req_bus >> 32));
    if (buf)
        bcmw_memcpy(w->ioctl_req, buf, in);
    else
        bcmw_memset(w->ioctl_req, 0, in);
    bcmw_sync(w, w->ioctl_req, IOCTL_REQ_SIZE, 1);
    w->ioctl_done = 0;
    ring_commit(r);
    w->info.ioctls++;
    t0 = bcmw_now(w);
    while (!w->ioctl_done && !w->dead) {
        mb_process(w);
        if (w->ioctl_done)
            break;
        if (bcmw_now(w) - t0 > 2000000) {
            bcmw_sayx(w, "msgbuf: TIMEOUT: no answer to ioctl ", cmd, " in 2 s");
            return BCMW_E_TIMEOUT;
        }
        bcmw_udelay(w, 50);
    }
    if (w->dead)
        return BCMW_E_STATE;
    resp = ctl_take(w, w->ioctl_pktid);
    if (w->ioctl_len && resp && buf)
        bcmw_memcpy(buf, resp, len < w->ioctl_len ? len : w->ioctl_len);
    post_ctl(w, 0, 1);
    if (fwerr)
        *fwerr = w->ioctl_status;
    if (w->ioctl_len && !resp)
        return BCMW_E_PROTO;
    return w->ioctl_status ? BCMW_E_FW : BCMW_OK;
}

/* ---- the firmware's console ----------------------------------------------------------------------- */

void mb_console(struct bcmw *w)
{
    uint32_t widx;
    unsigned guard = 0;

    if (!w->console_base || !w->console_size)
        return;
    widx = tcm_rd32(w, w->console_base + CON_WRITEIDX);
    if (widx >= w->console_size)
        return;
    while (w->console_ridx != widx && guard++ < 4096) {
        uint8_t c = (uint8_t)(tcm_rd32(w, w->console_buf + (w->console_ridx & ~3u)) >> (8 * (w->console_ridx & 3)));
        if (++w->console_ridx == w->console_size)
            w->console_ridx = 0;
        if (c == '\r')
            continue;
        if (c != '\n' && w->console_n < sizeof(w->console_line) - 1) {
            w->console_line[w->console_n++] = (char)(c >= 0x20 && c < 0x7f ? c : '.');
            continue;
        }
        w->console_line[w->console_n] = 0;
        if (w->console_n && w->console_quota == 0) {
            w->console_n = 0;
            continue;
        }
        if (w->console_n && --w->console_quota == 0) {
            bcmw_say(w, "fw: (the firmware's console goes on; not logged further, to keep the log short)");
            w->console_n = 0;
            continue;
        }
        if (w->console_n) {
            struct bcmw_lb l;
            lb_init(&l);
            lb_s(&l, "fw: ");
            lb_s(&l, w->console_line);
            bcmw_say_lb(w, &l);
            w->info.console_lines++;
        }
        w->console_n = 0;
    }
}
