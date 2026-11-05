/* SPDX-License-Identifier: GPL-2.0-only
 * libsb strings and memory: the C library functions GCC may call, the string builder of
 * shim/starlte/ui.c, CRC32 (ARMv8 CRC instructions, as shim/starlte/store.c star_crc32).
 */
#include "sb.h"

#ifndef SB_HOST
#define NOLIB __attribute__((optimize("no-tree-loop-distribute-patterns"), used))

NOLIB void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (!(((uintptr_t)dp | (uintptr_t)sp) & 7)) {
        for (; n >= 32; n -= 32, dp += 32, sp += 32) {
            uint64_t a = ((const uint64_t *)sp)[0], b = ((const uint64_t *)sp)[1];
            uint64_t c = ((const uint64_t *)sp)[2], e = ((const uint64_t *)sp)[3];
            ((uint64_t *)dp)[0] = a;
            ((uint64_t *)dp)[1] = b;
            ((uint64_t *)dp)[2] = c;
            ((uint64_t *)dp)[3] = e;
        }
        for (; n >= 8; n -= 8, dp += 8, sp += 8)
            *(uint64_t *)dp = *(const uint64_t *)sp;
    }
    while (n--)
        *dp++ = *sp++;
    return d;
}

NOLIB void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp == sp || !n)
        return d;
    if (dp < sp || dp >= sp + n)
        return memcpy(d, s, n);
    dp += n;
    sp += n;
    if (!(((uintptr_t)dp | (uintptr_t)sp | n) & 7)) {
        for (; n >= 8; n -= 8) {
            dp -= 8;
            sp -= 8;
            *(uint64_t *)dp = *(const uint64_t *)sp;
        }
    }
    while (n--)
        *--dp = *--sp;
    return d;
}

NOLIB void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    if (!((uintptr_t)dp & 7)) {
        uint64_t v = (uint8_t)c;
        v |= v << 8;
        v |= v << 16;
        v |= v << 32;
        for (; n >= 8; n -= 8, dp += 8)
            *(uint64_t *)dp = v;
    }
    while (n--)
        *dp++ = (uint8_t)c;
    return d;
}

NOLIB int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; --n, ++x, ++y)
        if (*x != *y)
            return *x < *y ? -1 : 1;
    return 0;
}

NOLIB size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}
#endif

unsigned sb_strlen(const char *s)
{
    unsigned n = 0;
    while (s && s[n])
        ++n;
    return n;
}

int sb_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

int sb_prefix(const char *s, const char *pre)
{
    while (*pre)
        if (*s++ != *pre++)
            return 0;
    return 1;
}

void sb_strcpy(char *d, unsigned cap, const char *s)
{
    unsigned i = 0;
    if (s)
        for (; s[i] && i + 1 < cap; ++i)
            d[i] = s[i];
    if (cap)
        d[i] = 0;
}

/* ---- string builder --------------------------------------------------------------------------- */

void ub_init(struct ub *u)
{
    u->n = 0;
    u->b[0] = 0;
}

void ub_c(struct ub *u, char c)
{
    if (u->n + 1 < sizeof(u->b)) {
        u->b[u->n++] = c;
        u->b[u->n] = 0;
    }
}

void ub_s(struct ub *u, const char *s)
{
    while (s && *s)
        ub_c(u, *s++);
}

void ub_dec(struct ub *u, uint64_t v)
{
    char t[24];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v);
    while (n)
        ub_c(u, t[--n]);
}

void ub_sdec(struct ub *u, int64_t v)
{
    if (v < 0) {
        ub_c(u, '-');
        ub_dec(u, (uint64_t)(-v));
    } else {
        ub_dec(u, (uint64_t)v);
    }
}

void ub_hexn(struct ub *u, uint64_t v, unsigned digits)
{
    for (unsigned i = digits; i; --i)
        ub_c(u, "0123456789ABCDEF"[(v >> (4 * (i - 1))) & 15U]);
}

void ub_hex(struct ub *u, uint64_t v, unsigned digits)
{
    ub_s(u, "0x");
    ub_hexn(u, v, digits);
}

void ub_dec2(struct ub *u, unsigned v)
{
    ub_c(u, (char)('0' + (v / 10U) % 10U));
    ub_c(u, (char)('0' + v % 10U));
}

void ub_fix(struct ub *u, uint64_t milli, unsigned decimals)
{
    ub_dec(u, milli / 1000U);
    if (decimals) {
        ub_c(u, '.');
        uint64_t f = milli % 1000U;
        ub_c(u, (char)('0' + f / 100U));
        if (decimals > 1)
            ub_c(u, (char)('0' + (f / 10U) % 10U));
        if (decimals > 2)
            ub_c(u, (char)('0' + f % 10U));
    }
}

void ub_size(struct ub *u, uint64_t bytes)
{
    if (bytes >= (1ULL << 30) && !(bytes & ((1ULL << 30) - 1))) {
        ub_dec(u, bytes >> 30);
        ub_s(u, " GiB");
    } else if (bytes >= (1ULL << 30)) {
        ub_dec(u, bytes >> 30);
        ub_c(u, '.');
        ub_dec(u, ((bytes & ((1ULL << 30) - 1)) * 10U) >> 30);
        ub_s(u, " GiB");
    } else if (bytes >= (1ULL << 20)) {
        ub_dec(u, bytes >> 20);
        if (bytes & ((1ULL << 20) - 1)) {
            ub_c(u, '.');
            ub_dec(u, ((bytes & ((1ULL << 20) - 1)) * 10U) >> 20);
        }
        ub_s(u, " MiB");
    } else if (bytes >= 1024U) {
        ub_dec(u, bytes >> 10);
        ub_s(u, " KiB");
    } else {
        ub_dec(u, bytes);
        ub_s(u, " B");
    }
}

/* ---- CRC32 (IEEE, reflected, zlib.crc32) -------------------------------------------------------- */

#if defined(__ARM_FEATURE_CRC32) || !defined(SB_HOST)
__attribute__((target("+crc"))) uint32_t sb_crc32(const void *p, uint64_t n)
{
    const uint8_t *b = p;
    uint32_t c = 0xffffffffU;
    uint64_t i = 0;
    for (; i < n && ((uintptr_t)(b + i) & 7); ++i)
        __asm__("crc32b %w0, %w0, %w1" : "+r"(c) : "r"((uint32_t)b[i]));
    for (; i + 8 <= n; i += 8)
        __asm__("crc32x %w0, %w0, %x1" : "+r"(c) : "r"(*(const uint64_t *)(b + i)));
    for (; i < n; ++i)
        __asm__("crc32b %w0, %w0, %w1" : "+r"(c) : "r"((uint32_t)b[i]));
    return ~c;
}
#else
uint32_t sb_crc32(const void *p, uint64_t n)
{
    const uint8_t *b = p;
    uint32_t c = 0xffffffffU;
    while (n--) {
        c ^= *b++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}
#endif
