/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: SIPC5 link frames. Header: u8 cfg (top 5 bits 0xF8, then PADDING 0x04, EXT 0x02, CTL 0x01), u8 channel,
 * u16 length (whole frame, header included, padding excluded); EXT|CTL adds a control byte { id:7, more:1 }
 * (multi-frame message), EXT alone makes the length field 32 bits wide (6-byte header). With PADDING set,
 * frames in a stream are padded to 8 bytes. Every frame logged on this phone is cfg 0xF8 (one SBD buffer per
 * frame, no padding), e.g. "f8 eb 0c 00 | 08 00 01 00 01 01 03 53".
 */
#include "sbm.h"

int sbm_sipc5_parse(const uint8_t *b, uint32_t len, struct sbm_sipc5 *f)
{
    sbm_memset(f, 0, sizeof(*f));
    if (!b || len < 4)
        return SBM_ESHORT;
    uint8_t cfg = b[0];
    if ((cfg & SBM_SIPC5_START) != SBM_SIPC5_START)
        return SBM_EBADF;
    f->cfg = cfg;
    f->ch = b[1];
    uint32_t flen;
    if (cfg & SBM_SIPC5_EXT) {
        if ((cfg & SBM_SIPC5_MULTI) == SBM_SIPC5_MULTI) {
            if (len < 5)
                return SBM_ESHORT;
            f->hdr_len = 5;
            f->multi = 1;
            f->id = b[4] & 0x7F;
            f->more = b[4] >> 7;
            flen = sbm_le16(b + 2);
        } else {
            if (len < 6)
                return SBM_ESHORT;
            f->hdr_len = 6;
            flen = sbm_le32(b + 2);
        }
    } else {
        f->hdr_len = 4;
        flen = sbm_le16(b + 2);
    }
    if (flen < f->hdr_len)
        return SBM_EBADF;
    if (flen > len)
        return SBM_ESHORT;
    f->frame_len = flen;
    f->payload = b + f->hdr_len;
    f->payload_len = flen - f->hdr_len;
    uint32_t total = flen;
    if (cfg & SBM_SIPC5_PADDING) {
        uint32_t r = flen & (SBM_SIPC5_ALIGN - 1);
        if (r)
            total += SBM_SIPC5_ALIGN - r;
        if (total > len)            /* the last frame of a buffer may omit its padding */
            total = len;
    }
    f->total = total;
    return 0;
}

int sbm_sipc5_build(uint8_t *out, uint32_t cap, uint8_t ch, const uint8_t *payload, uint32_t plen)
{
    uint32_t hl = plen + 4 <= 0xFFFF ? 4 : 6;
    if (plen > 0xFFFFFFFFU - hl)
        return SBM_ERANGE;
    uint32_t total = plen + hl;
    if (!out || total > cap || total > 0x7FFFFFFF)
        return SBM_ENOSPC;
    out[0] = hl == 4 ? SBM_SIPC5_START : (SBM_SIPC5_START | SBM_SIPC5_EXTLEN);
    out[1] = ch;
    if (hl == 4)
        sbm_put16(out + 2, (uint16_t)total);
    else
        sbm_put32(out + 2, total);
    if (plen)
        sbm_memcpy(out + hl, payload, plen);
    return (int)total;
}

int sbm_sipc5_build_multi(uint8_t *out, uint32_t cap, uint8_t ch, uint8_t id, int more,
                          const uint8_t *payload, uint32_t plen)
{
    if (plen > 0xFFFF - 5)
        return SBM_ERANGE;
    uint32_t total = plen + 5;
    if (!out || total > cap)
        return SBM_ENOSPC;
    out[0] = SBM_SIPC5_START | SBM_SIPC5_MULTI;
    out[1] = ch;
    sbm_put16(out + 2, (uint16_t)total);
    out[4] = (uint8_t)((id & 0x7F) | (more ? 0x80 : 0));
    if (plen)
        sbm_memcpy(out + 5, payload, plen);
    return (int)total;
}

int sbm_reasm_feed(struct sbm_reasm *r, const struct sbm_sipc5 *f, const uint8_t **msg, uint32_t *mlen)
{
    *msg = 0;
    *mlen = 0;
    if (!f->multi) {
        if (r->active) {            /* a single frame in the middle of a multi-frame message */
            r->active = 0;
            r->len = 0;
            r->dropped++;
        }
        *msg = f->payload;
        *mlen = f->payload_len;
        return 1;
    }
    if (!r->active) {
        r->active = 1;
        r->id = f->id;
        r->len = 0;
    } else if (r->id != f->id) {
        r->dropped++;               /* a new message started before the old one ended */
        r->id = f->id;
        r->len = 0;
    }
    if (!r->buf || f->payload_len > r->cap - r->len) {
        r->active = 0;
        r->len = 0;
        r->dropped++;
        return SBM_ENOSPC;
    }
    sbm_memcpy(r->buf + r->len, f->payload, f->payload_len);
    r->len += f->payload_len;
    if (f->more)
        return 0;
    r->active = 0;
    *msg = r->buf;
    *mlen = r->len;
    r->len = 0;
    return 1;
}
