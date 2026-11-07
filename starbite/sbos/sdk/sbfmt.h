/* SPDX-License-Identifier: MIT
 * Small text building for apps (no libc): fmt_s, fmt_d, fmt_2 (two digits), fmt_c10 ("38.5"), fmt_dur. */
#pragma once
#include <stdint.h>

struct fmt {
    char b[96];
    unsigned n;
};

static inline void fmt_init(struct fmt *f) { f->n = 0; f->b[0] = 0; }
static inline void fmt_c(struct fmt *f, char c)
{
    if (f->n + 1 < sizeof(f->b)) {
        f->b[f->n++] = c;
        f->b[f->n] = 0;
    }
}
static inline void fmt_s(struct fmt *f, const char *s) { while (s && *s) fmt_c(f, *s++); }
static inline void fmt_u(struct fmt *f, uint64_t v)
{
    char t[24];
    int i = 0;
    do {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i)
        fmt_c(f, t[--i]);
}
static inline void fmt_d(struct fmt *f, int64_t v)
{
    if (v < 0) {
        fmt_c(f, '-');
        v = -v;
    }
    fmt_u(f, (uint64_t)v);
}
static inline void fmt_2(struct fmt *f, unsigned v) { fmt_c(f, (char)('0' + v / 10 % 10)); fmt_c(f, (char)('0' + v % 10)); }
/* tenths: 385 -> "38.5" */
static inline void fmt_c10(struct fmt *f, int v)
{
    if (v < 0) {
        fmt_c(f, '-');
        v = -v;
    }
    fmt_u(f, (uint64_t)(v / 10));
    fmt_c(f, '.');
    fmt_c(f, (char)('0' + v % 10));
}
/* minutes -> "about 9 h" / "about 3 days" / "45 min" */
static inline void fmt_dur(struct fmt *f, int32_t min)
{
    if (min < 90) {
        fmt_d(f, min);
        fmt_s(f, " min");
    } else if (min < 48 * 60) {
        fmt_s(f, "about ");
        fmt_d(f, (min + 30) / 60);
        fmt_s(f, " h");
    } else {
        fmt_s(f, "about ");
        fmt_d(f, (min + 720) / (24 * 60));
        fmt_s(f, " days");
    }
}
