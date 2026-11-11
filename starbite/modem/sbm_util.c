/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sbm: freestanding helpers (no libc): memory, strings, a tiny formatter, logging. */
#include <stdarg.h>
#include "sbm.h"

void sbm_memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--)
        *dp++ = *sp++;
}

void sbm_memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = (uint8_t)c;
}

int sbm_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; --n, ++x, ++y)
        if (*x != *y)
            return *x < *y ? -1 : 1;
    return 0;
}

size_t sbm_strlcpy(char *d, const char *s, size_t cap)
{
    size_t i = 0;
    if (!cap)
        return 0;
    for (; s[i] && i + 1 < cap; ++i)
        d[i] = s[i];
    d[i] = 0;
    return i;
}

static void put(char *out, size_t cap, size_t *n, char c)
{
    if (*n + 1 < cap)
        out[*n] = c;
    ++*n;
}

static void put_num(char *out, size_t cap, size_t *n, uint64_t v, unsigned base, int width, char padc)
{
    char tmp[24];
    int k = 0;
    do {
        tmp[k++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v && k < (int)sizeof(tmp));
    while (k < width--)
        put(out, cap, n, padc);
    while (k)
        put(out, cap, n, tmp[--k]);
}

static int vfmt(char *out, size_t cap, const char *f, va_list ap)
{
    size_t n = 0;
    for (; *f; ++f) {
        if (*f != '%') {
            put(out, cap, &n, *f);
            continue;
        }
        ++f;
        char padc = ' ';
        int width = 0, lng = 0;
        if (*f == '0') {
            padc = '0';
            ++f;
        }
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        while (*f == 'l') {
            lng = 1;
            ++f;
        }
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = 0;
            while (s[len])
                ++len;
            while (len < width--)
                put(out, cap, &n, ' ');
            for (; *s; ++s)
                put(out, cap, &n, *s);
            break;
        }
        case 'u':
            put_num(out, cap, &n, lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned), 10, width, padc);
            break;
        case 'd': {
            long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
            uint64_t u = (uint64_t)v;
            if (v < 0) {
                put(out, cap, &n, '-');
                u = 0 - u;
            }
            put_num(out, cap, &n, u, 10, width, padc);
            break;
        }
        case 'x':
            put_num(out, cap, &n, lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned), 16, width, padc);
            break;
        case 'c':
            put(out, cap, &n, (char)va_arg(ap, int));
            break;
        case '%':
            put(out, cap, &n, '%');
            break;
        default:
            put(out, cap, &n, '?');
            break;
        }
    }
    if (cap)
        out[n < cap ? n : cap - 1] = 0;
    return (int)n;
}

int sbm_fmt(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfmt(out, cap, fmt, ap);
    va_end(ap);
    return r;
}

void sbm_logf(const struct sbm_plat *p, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    if (!p || !p->log)
        return;
    va_start(ap, fmt);
    vfmt(line, sizeof(line), fmt, ap);
    va_end(ap);
    /* belt and braces: nothing that looks like an IMEI/IMSI/ICCID leaves through the log */
    sbm_redact_text(line, sizeof(line));
    p->log(p->ctx, line);
}

const char *sbm_strerror(int e)
{
    switch (e) {
    case 0: return "ok";
    case SBM_EINVAL: return "invalid";
    case SBM_ERANGE: return "out of range";
    case SBM_ENOSPC: return "no space";
    case SBM_ENOENT: return "not found";
    case SBM_EPERM: return "not permitted";
    case SBM_ETIME: return "timeout";
    case SBM_EIO: return "i/o";
    case SBM_ESHORT: return "short";
    case SBM_EBADF: return "malformed";
    default: return "?";
    }
}

uint32_t sbm_strlen(const char *s)
{
    uint32_t n = 0;
    while (s && s[n])
        ++n;
    return n;
}

void *sbm_memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    if (a < b)
        for (size_t i = 0; i < n; ++i)
            a[i] = b[i];
    else
        for (size_t i = n; i-- > 0;)
            a[i] = b[i];
    return d;
}

int sbm_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}
