/* SPDX-License-Identifier: GPL-2.0-only
 * The SMS store's record format (Starbite OS: the state slot's "sms.NN" records, at most SBM_STORE_REC bytes each).
 * Pure C, no I/O: the kernel packs its messages into records and its settings code writes them with the rest of the
 * slot; at start it unpacks them again. The user's messages live on the user's own phone only: nothing here logs a
 * number or a text.
 *
 * A record: "SMS1", u16 count, u16 reserved, then count messages, each:
 *   u32 id, i64 utc, u8 incoming, u8 state, u8 unread, u8 number length (<= 23), u16 text length (<= 511),
 *   the number's bytes, the text's bytes (UTF-8, no terminator).
 * A message never spans two records. Anything that does not check (magic, lengths past the end) ends the record:
 * what was read before it is kept, the rest is dropped (never a crash on a damaged slot). */
#include "sbm.h"

static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, v); p16(p + 2, v >> 16); }
static uint32_t g16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t g32(const uint8_t *p) { return g16(p) | g16(p + 2) << 16; }

/* tl, or less so the text ends on a whole UTF-8 character (a lead byte without all its continuation bytes is cut) */
static uint32_t utf8_whole(const char *t, uint32_t tl)
{
    uint32_t i = tl;
    unsigned cont = 0;
    while (i && ((uint8_t)t[i - 1] & 0xC0) == 0x80 && cont < 3)
        --i, ++cont;
    if (!i)
        return tl;
    uint8_t c = (uint8_t)t[i - 1];
    unsigned need = c >= 0xF0 ? 3U : c >= 0xE0 ? 2U : c >= 0xC0 ? 1U : 0U;
    return c >= 0xC0 && cont < need ? i - 1 : tl;
}

uint32_t sbm_store_msg_size(const struct sbm_store_msg *m)
{
    uint32_t nl = sbm_strlen(m->number), tl = sbm_strlen(m->text);
    if (nl > SBM_STORE_NUM - 1)
        nl = SBM_STORE_NUM - 1;
    if (tl > SBM_STORE_TEXT - 1)
        tl = SBM_STORE_TEXT - 1;
    return SBM_STORE_MSG_HDR + nl + tl;
}

void sbm_store_begin(struct sbm_store_rec *r, uint8_t *buf, uint32_t cap)
{
    r->b = buf;
    r->cap = cap;
    r->len = 0;
    r->count = 0;
    if (cap >= 8) {
        buf[0] = 'S', buf[1] = 'M', buf[2] = 'S', buf[3] = '1';
        p16(buf + 4, 0);
        p16(buf + 6, 0);
        r->len = 8;
    }
}

int sbm_store_add(struct sbm_store_rec *r, const struct sbm_store_msg *m)
{
    uint32_t need = sbm_store_msg_size(m);
    if (r->len < 8 || r->len + need > r->cap || r->count >= 0xffff)
        return 0;
    uint32_t nl = sbm_strlen(m->number), tl = sbm_strlen(m->text);
    if (nl > SBM_STORE_NUM - 1)
        nl = SBM_STORE_NUM - 1;
    if (tl > SBM_STORE_TEXT - 1)
        tl = SBM_STORE_TEXT - 1;
    tl = utf8_whole(m->text, tl);           /* never store half a character */
    uint8_t *p = r->b + r->len;
    p32(p, m->id);
    p32(p + 4, (uint32_t)(uint64_t)m->utc);
    p32(p + 8, (uint32_t)((uint64_t)m->utc >> 32));
    p[12] = m->incoming;
    p[13] = m->state;
    p[14] = m->unread;
    p[15] = (uint8_t)nl;
    p16(p + 16, tl);
    sbm_memcpy(p + SBM_STORE_MSG_HDR, m->number, nl);
    sbm_memcpy(p + SBM_STORE_MSG_HDR + nl, m->text, tl);
    r->len += SBM_STORE_MSG_HDR + nl + tl;
    r->count++;
    p16(r->b + 4, r->count);
    return 1;
}

int sbm_store_next(const uint8_t *rec, uint32_t n, uint32_t *off, unsigned *idx, struct sbm_store_msg *m)
{
    if (!rec || n < 8 || rec[0] != 'S' || rec[1] != 'M' || rec[2] != 'S' || rec[3] != '1')
        return 0;
    unsigned count = g16(rec + 4);
    if (*off < 8)
        *off = 8, *idx = 0;
    if (*idx >= count || *off + SBM_STORE_MSG_HDR > n)
        return 0;
    const uint8_t *p = rec + *off;
    uint32_t nl = p[15], tl = g16(p + 16);
    if (nl > SBM_STORE_NUM - 1 || tl > SBM_STORE_TEXT - 1 || *off + SBM_STORE_MSG_HDR + nl + tl > n)
        return 0;
    m->id = g32(p);
    m->utc = (int64_t)((uint64_t)g32(p + 4) | (uint64_t)g32(p + 8) << 32);
    m->incoming = p[12];
    m->state = p[13];
    m->unread = p[14];
    sbm_memcpy(m->number, p + SBM_STORE_MSG_HDR, nl);
    m->number[nl] = 0;
    sbm_memcpy(m->text, p + SBM_STORE_MSG_HDR + nl, tl);
    m->text[tl] = 0;
    for (uint32_t i = 0; i < nl + tl; ++i)
        if (!(i < nl ? m->number[i] : m->text[i - nl]))
            return 0;                       /* a NUL inside: damaged */
    *off += SBM_STORE_MSG_HDR + nl + tl;
    ++*idx;
    return 1;
}
