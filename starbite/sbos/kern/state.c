/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS state slot format (docs/STARBITE-OS.md 6.2). Two halves written alternately; each:
 *   "SBOSSTA1", u64 seq, u32 length (header included), u32 CRC32 of the bytes after the header up to length,
 *   then records: char id[24], u32 n, data (n bytes, padded to 8).
 * Pure C. A half is trusted only when every byte checks; anything else is a fresh start. */
#include "kern.h"

#define HDR 24U

static uint32_t crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffU;
    for (uint32_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (unsigned k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}

static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

void state_begin(struct state_buf *s, uint8_t *mem, uint64_t seq)
{
    s->b = mem;
    s->len = HDR;
    s->seq = seq;
}

int state_add(struct state_buf *s, const char *id, const void *data, uint32_t n)
{
    uint32_t need = 28U + ((n + 7U) & ~7U);
    if (n > STATE_REC_MAX || s->len + need > STATE_HALF)
        return -1;
    uint8_t *p = s->b + s->len;
    unsigned i = 0;
    for (; i < 23 && id[i]; ++i)
        p[i] = (uint8_t)id[i];
    for (; i < 24; ++i)
        p[i] = 0;
    put32(p + 24, n);
    const uint8_t *d = data;
    for (uint32_t k = 0; k < n; ++k)
        p[28 + k] = d[k];
    for (uint32_t k = n; k < ((n + 7U) & ~7U); ++k)
        p[28 + k] = 0;
    s->len += need;
    return 0;
}

uint32_t state_finish(struct state_buf *s)
{
    static const char magic[8] = { 'S', 'B', 'O', 'S', 'S', 'T', 'A', '1' };
    for (unsigned i = 0; i < 8; ++i)
        s->b[i] = (uint8_t)magic[i];
    put64(s->b + 8, s->seq);
    put32(s->b + 16, s->len);
    put32(s->b + 20, crc(s->b + HDR, s->len - HDR));
    return s->len;
}

int state_valid(const uint8_t *h, uint64_t *seq)
{
    static const char magic[8] = { 'S', 'B', 'O', 'S', 'S', 'T', 'A', '1' };
    for (unsigned i = 0; i < 8; ++i)
        if (h[i] != (uint8_t)magic[i])
            return 0;
    uint32_t len = get32(h + 16);
    if (len < HDR || len > STATE_HALF || crc(h + HDR, len - HDR) != get32(h + 20))
        return 0;
    /* every record must lie inside the length */
    for (uint32_t at = HDR; at < len;) {
        if (at + 28U > len)
            return 0;
        uint32_t n = get32(h + at + 24);
        if (n > STATE_REC_MAX || at + 28U + ((n + 7U) & ~7U) > len)
            return 0;
        at += 28U + ((n + 7U) & ~7U);
    }
    if (seq)
        *seq = get64(h + 8);
    return 1;
}

int state_find(const uint8_t *h, const char *id, const uint8_t **data)
{
    uint32_t len = get32(h + 16);
    for (uint32_t at = HDR; at + 28U <= len;) {
        uint32_t n = get32(h + at + 24);
        int eq = 1;
        for (unsigned i = 0; i < 24; ++i) {
            char c = i < 23 ? id[i] : 0;
            if ((uint8_t)c != h[at + i]) {
                eq = 0;
                break;
            }
            if (!c)
                break;
        }
        if (eq) {
            *data = h + at + 28;
            return (int)n;
        }
        at += 28U + ((n + 7U) & ~7U);
    }
    return -1;
}

int state_pick(const uint8_t *a, const uint8_t *b)
{
    uint64_t sa = 0, sb = 0;
    int va = state_valid(a, &sa), vb = state_valid(b, &sb);
    if (va && vb)
        return sb > sa ? 1 : 0;
    return va ? 0 : vb ? 1 : -1;
}
