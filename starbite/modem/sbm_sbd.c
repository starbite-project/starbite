/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: SBD (shared buffer descriptor) rings in the CP's IPC region (docs/MODEM.md section 3.2). Layout, as the
 * AP builds it on CMD_INIT_START and as this phone's kernel logged it (dmesg 2026-11-08 091233):
 *
 *   0x0000 control (magic, access)  0x0400 command area  0x0800 global descriptor:
 *       u32 version (1), u32 num_channels, u32 rbps_offset,
 *       32 x { u32 ul_rbd_offset, ul_sbdv_offset, dl_rbd_offset, dl_sbdv_offset }         at 0x080c (2060)
 *       32 x 2 x { u16 ch, reserved, direction, signaling; u32 sig_mask;
 *                  u16 length, id, buff_size, payload_offset }                            at 0x0a0c (2572)
 *   then the RBP set u16 rp[UL][n], wp[UL][n], rp[DL][n], wp[DL][n]                       at 0x0f0c (3852)
 *   then per channel: UL addr_v[len], UL size_v[len], DL addr_v[len], DL size_v[len]      (u32, from 3924)
 *   0x10000 data buffers: UL ch0, DL ch0, UL ch1, ... (len x buff_size each)
 *
 * Indices are 16-bit buffer indices; one slot stays empty (full = wp + 1 == rp). The producer owns wp, the
 * consumer rp. Every index, size and address the other side can write is checked before it is used.
 */
#include "sbm.h"

/* The nine link channels of this phone, in DT order (the order fixes the layout). */
const struct sbm_chan sbm_starlte_chans[] = {
    { "umts_cass",   SBM_CH_CASS,   1, 0, { 16, 16 },   { 2048, 2048 } },
    { "umts_ipc0",   SBM_CH_FMT0,   1, 0, { 16, 32 },   { 4096, 4096 } },
    { "umts_ipc1",   SBM_CH_FMT1,   1, 0, { 16, 32 },   { 4096, 4096 } },
    { "umts_rfs0",   SBM_CH_RFS0,   1, 0, { 16, 512 },  { 2048, 2048 } },
    { "umts_csd",    SBM_CH_CSD,    1, 0, { 32, 64 },   { 2048, 2048 } },
    { "umts_router", SBM_CH_ROUTER, 1, 0, { 16, 16 },   { 2048, 2048 } },
    { "umts_dm0",    SBM_CH_DM,     1, 0, { 16, 128 },  { 2048, 2048 } },
    { "rmnet0",      SBM_CH_PDP0,   1, 0, { 256, 256 }, { 2048, 2048 } },
    { "multipdp_hi", SBM_CH_PDP_HI, 1, 1, { 512, 2048 }, { 2048, 8 } },
};
const unsigned sbm_starlte_nchans = sizeof(sbm_starlte_chans) / sizeof(sbm_starlte_chans[0]);

static inline volatile uint16_t *p16(const struct sbm_sbd *s, uint32_t off)
{
    return (volatile uint16_t *)(void *)(s->ipc + off);
}
static inline volatile uint32_t *p32(const struct sbm_sbd *s, uint32_t off)
{
    return (volatile uint32_t *)(void *)(s->ipc + off);
}
static inline void bar(const struct sbm_plat *p)
{
    if (p && p->barrier)
        p->barrier(p->ctx);
}

static uint32_t rbp_off(const struct sbm_sbd *s, unsigned id, unsigned dir, int wp)
{
    return s->rbps_off + 2U * (s->n * (2U * dir + (unsigned)wp) + id);
}

uint16_t sbm_sbd_rp(const struct sbm_sbd *s, unsigned id, unsigned dir) { return *p16(s, rbp_off(s, id, dir, 0)); }
uint16_t sbm_sbd_wp(const struct sbm_sbd *s, unsigned id, unsigned dir) { return *p16(s, rbp_off(s, id, dir, 1)); }

int sbm_sbd_id(const struct sbm_sbd *s, uint16_t ch)
{
    return ch < 256 && s->ch2id[ch] != 0xFF ? s->ch2id[ch] : -1;
}

static void reset(struct sbm_sbd *s, uint8_t *ipc, uint32_t ipc_size)
{
    sbm_memset(s, 0, sizeof(*s));
    sbm_memset(s->ch2id, 0xFF, sizeof(s->ch2id));
    s->ipc = ipc;
    s->ipc_size = ipc_size;
}

int sbm_sbd_init(struct sbm_sbd *s, uint8_t *ipc, uint32_t ipc_size, const struct sbm_chan *chans, unsigned n)
{
    reset(s, ipc, ipc_size);
    if (!ipc || !chans || n == 0 || n > SBM_SBD_MAX_CH || ipc_size < SBM_SBD_BUFF_OFF || ((uintptr_t)ipc & 3))
        return SBM_EINVAL;
    s->n = n;
    s->rbps_off = SBM_SBD_GDESC_END;
    uint32_t desc = s->rbps_off + 8U * n;      /* 4 arrays of n u16 */
    uint32_t buff = SBM_SBD_BUFF_OFF;
    for (unsigned i = 0; i < n; ++i) {
        const struct sbm_chan *c = &chans[i];
        if (c->ch == 0 || c->ch > 255 || s->ch2id[c->ch] != 0xFF)
            return SBM_EINVAL;
        for (unsigned d = 0; d < 2; ++d) {
            if (c->len[d] < 2 || c->size[d] == 0)
                return SBM_EINVAL;
            struct sbm_ring *r = &s->r[i][d];
            r->id = (uint16_t)i;
            r->ch = c->ch;
            r->dir = (uint16_t)d;
            r->len = c->len[d];
            r->size = c->size[d];
            r->rbd_off = SBM_SBD_RBDESC_OFF + (2U * i + d) * SBM_SBD_RBDESC_SIZE;
            uint64_t bytes = (uint64_t)r->len * r->size;
            if (buff + bytes > ipc_size)
                return SBM_ENOSPC;
            r->buff_off = buff;
            buff += (uint32_t)bytes;
            r->addr_v_off = desc;
            r->size_v_off = desc + 4U * r->len;
            desc += 8U * r->len;
            if (desc > SBM_SBD_DESC_END)
                return SBM_ENOSPC;
        }
        s->ch2id[c->ch] = (uint8_t)i;
    }
    s->desc_end = desc;
    s->buff_end = buff;

    /* write it out */
    sbm_memset(ipc + SBM_SBD_DESC_OFF, 0, SBM_SBD_DESC_END - SBM_SBD_DESC_OFF);
    uint8_t *g = ipc + SBM_SBD_DESC_OFF;
    sbm_put32(g + 0, 1);
    sbm_put32(g + 4, n);
    sbm_put32(g + 8, s->rbps_off);
    for (unsigned i = 0; i < n; ++i) {
        uint8_t *ch = ipc + SBM_SBD_RBCH_OFF + i * SBM_SBD_RBCH_SIZE;
        sbm_put32(ch + 0, s->r[i][SBM_UL].rbd_off);
        sbm_put32(ch + 4, s->r[i][SBM_UL].addr_v_off);
        sbm_put32(ch + 8, s->r[i][SBM_DL].rbd_off);
        sbm_put32(ch + 12, s->r[i][SBM_DL].addr_v_off);
        for (unsigned d = 0; d < 2; ++d) {
            const struct sbm_ring *r = &s->r[i][d];
            uint8_t *rd = ipc + r->rbd_off;
            sbm_put16(rd + 0, r->ch);
            sbm_put16(rd + 2, 0);
            sbm_put16(rd + 4, r->dir);
            sbm_put16(rd + 6, 1);                           /* signaling: interrupt */
            sbm_put32(rd + 8, SBM_INT_VALID | 0x0001);      /* sig_mask: INT_VALID | SEND_DATA */
            sbm_put16(rd + 12, r->len);
            sbm_put16(rd + 14, r->id);
            sbm_put16(rd + 16, r->size);
            sbm_put16(rd + 18, 0);                          /* payload_offset */
            for (unsigned k = 0; k < r->len; ++k) {
                sbm_put32(ipc + r->addr_v_off + 4 * k, r->buff_off + k * r->size);
                sbm_put32(ipc + r->size_v_off + 4 * k, 0);
            }
        }
    }
    return 0;
}

int sbm_sbd_attach(struct sbm_sbd *s, uint8_t *ipc, uint32_t ipc_size)
{
    reset(s, ipc, ipc_size);
    if (!ipc || ipc_size < SBM_SBD_BUFF_OFF || ((uintptr_t)ipc & 3))
        return SBM_EINVAL;
    const uint8_t *g = ipc + SBM_SBD_DESC_OFF;
    uint32_t ver = sbm_le32(g), n = sbm_le32(g + 4), rbps = sbm_le32(g + 8);
    if (ver != 1 || n == 0 || n > SBM_SBD_MAX_CH)
        return SBM_EBADF;
    if (rbps < SBM_SBD_GDESC_END || (rbps & 1) || rbps + 8U * n > SBM_SBD_DESC_END)
        return SBM_EBADF;
    s->n = n;
    s->rbps_off = rbps;
    uint32_t desc_end = rbps + 8U * n, buff_end = SBM_SBD_BUFF_OFF;
    for (unsigned i = 0; i < n; ++i) {
        const uint8_t *ch = ipc + SBM_SBD_RBCH_OFF + i * SBM_SBD_RBCH_SIZE;
        for (unsigned d = 0; d < 2; ++d) {
            struct sbm_ring *r = &s->r[i][d];
            uint32_t rbd = sbm_le32(ch + 8 * d), sbdv = sbm_le32(ch + 8 * d + 4);
            if (rbd != SBM_SBD_RBDESC_OFF + (2U * i + d) * SBM_SBD_RBDESC_SIZE)
                return SBM_EBADF;
            const uint8_t *rd = ipc + rbd;
            r->ch = sbm_le16(rd);
            r->dir = sbm_le16(rd + 4);
            r->len = sbm_le16(rd + 12);
            r->id = sbm_le16(rd + 14);
            r->size = sbm_le16(rd + 16);
            if (r->dir != d || r->id != i || r->len < 2 || r->size == 0 || r->ch == 0 || r->ch > 255 ||
                sbm_le16(rd + 18) != 0)
                return SBM_EBADF;
            if (d == 1 && r->ch != s->r[i][0].ch)
                return SBM_EBADF;
            if ((sbdv & 3) || sbdv < rbps + 8U * n || (uint64_t)sbdv + 8ULL * r->len > SBM_SBD_DESC_END)
                return SBM_EBADF;
            r->rbd_off = rbd;
            r->addr_v_off = sbdv;
            r->size_v_off = sbdv + 4U * r->len;
            if (sbdv + 8U * r->len > desc_end)
                desc_end = sbdv + 8U * r->len;
            /* the buffers must be one contiguous array inside the region, after the descriptors */
            uint32_t b0 = sbm_le32(ipc + sbdv);
            uint64_t bytes = (uint64_t)r->len * r->size;
            if (b0 < SBM_SBD_BUFF_OFF || b0 + bytes > ipc_size)
                return SBM_EBADF;
            for (unsigned k = 1; k < r->len; ++k)
                if (sbm_le32(ipc + sbdv + 4 * k) != b0 + k * r->size)
                    return SBM_EBADF;
            r->buff_off = b0;
            if (b0 + bytes > buff_end)
                buff_end = (uint32_t)(b0 + bytes);
        }
        if (s->ch2id[s->r[i][0].ch] != 0xFF)
            return SBM_EBADF;
        s->ch2id[s->r[i][0].ch] = (uint8_t)i;
    }
    s->desc_end = desc_end;
    s->buff_end = buff_end;
    return 0;
}

int sbm_sbd_put(struct sbm_sbd *s, uint16_t ch, unsigned dir, const uint8_t *frame, uint32_t len,
                const struct sbm_plat *p)
{
    int id = sbm_sbd_id(s, ch);
    if (id < 0 || dir > 1 || !frame || len == 0)
        return SBM_EINVAL;
    struct sbm_ring *r = &s->r[id][dir];
    if (len > r->size)
        return SBM_ERANGE;
    unsigned in = sbm_sbd_wp(s, (unsigned)id, dir), out = sbm_sbd_rp(s, (unsigned)id, dir);
    if (in >= r->len || out >= r->len) {
        s->errors++;
        return SBM_EIO;
    }
    unsigned space = in < out ? out - in - 1 : r->len + out - in - 1;
    if (space == 0)
        return SBM_ENOSPC;
    sbm_memcpy(s->ipc + r->buff_off + in * r->size, frame, len);
    uint32_t sv = len;
    /* the AP (UL) packs the channel only for PS channels (sbd_pio_tx); the CP (DL, the fake CP here) packs it
     * on every channel, as seen on the phone (T9b) */
    if (sbm_ch_is_ps(ch) || dir == SBM_DL)
        sv = (len & 0xFFFF) | ((uint32_t)ch << 16);
    *p32(s, r->size_v_off + 4 * in) = sv;
    bar(p);                          /* the data and its size before the index */
    *p16(s, rbp_off(s, (unsigned)id, dir, 1)) = (uint16_t)((in + 1) % r->len);
    bar(p);
    return 0;
}

int sbm_sbd_get(struct sbm_sbd *s, unsigned id, unsigned dir, const uint8_t **f, const struct sbm_plat *p)
{
    *f = 0;
    if (id >= s->n || dir > 1)
        return SBM_EINVAL;
    struct sbm_ring *r = &s->r[id][dir];
    unsigned in = sbm_sbd_wp(s, id, dir), out = sbm_sbd_rp(s, id, dir);
    if (in >= r->len || out >= r->len) {
        s->errors++;
        return SBM_EIO;
    }
    if (in == out)
        return 0;
    bar(p);                          /* the index before the size and the data it covers */
    uint32_t sv = *p32(s, r->size_v_off + 4 * out);
    /* Android's recv_data: the length is the low 16 bits on every channel (the CP puts the channel in the high
     * half of FMT and RFS entries too: T9b, 2026-11-09, 78 frames refused before this) */
    uint32_t len = sv & 0xFFFF;
    s->last_size_v = sv;
    if (len == 0 || len > r->size) {
        s->errors++;
        *p16(s, rbp_off(s, id, dir, 0)) = (uint16_t)((out + 1) % r->len);   /* skip it */
        return SBM_EBADF;
    }
    *f = s->ipc + r->buff_off + out * r->size;
    return (int)len;
}

void sbm_sbd_get_done(struct sbm_sbd *s, unsigned id, unsigned dir, const struct sbm_plat *p)
{
    if (id >= s->n || dir > 1)
        return;
    struct sbm_ring *r = &s->r[id][dir];
    unsigned out = sbm_sbd_rp(s, id, dir);
    if (out >= r->len || out == sbm_sbd_wp(s, id, dir))
        return;
    bar(p);                          /* finished reading the buffer before handing it back */
    *p16(s, rbp_off(s, id, dir, 0)) = (uint16_t)((out + 1) % r->len);
    bar(p);
}
