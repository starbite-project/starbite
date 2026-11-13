/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the audio probes' loopback check (snd.c, abox_media.c): is a 1 kHz tone in the main mic's samples?
 *
 * A three-bin Goertzel filter (1000 Hz and its neighbours 700 Hz / 1300 Hz) over blocks of 960 samples at 48 kHz
 * (20 ms, 50 Hz bins): the three are exact bins (20, 14, 26), so an exact 1 kHz tone (ABOX clocks the speaker side and
 * the mic side from the same AUDIF) leaves nothing in the neighbours with a plain rectangular window. Integers only
 * (the kernel's probes use no FPU): coefficients 2 cos(2 pi f / 48000) in Q30, int64 state; one block's power
 * |X|^2 = s1^2 + s2^2 - c s1 s2 (a full-scale on-bin sine: (32767 x 480)^2 = 2.5e14, far inside int64), summed >> 8.
 *
 * Use: gz_reset(&g); gz_put(&g, sample) for every sample; gz_mean(&g, GZ_1K) the mean block power of a bin;
 * gz_judge(&tone, &baseline, &verdict): heard when the 1 kHz bin is >= 10 dB over the baseline's 1 kHz bin AND
 * >= 6 dB over the mean of the tone's own neighbour bins (a louder room raises all three, a tone only the middle one).
 * gz_db10() is 10 log10(a / b) in tenths of a dB (0 counts as 1). */
#ifndef SBOS_GOERTZEL_H
#define SBOS_GOERTZEL_H
#include <stdint.h>

#define GZ_N 960U                       /* samples a block (20 ms at 48 kHz) */
#define GZ_1K 0
#define GZ_700 1
#define GZ_1300 2
#define GZ_HEARD_DB10 100               /* 10 dB over the baseline */
#define GZ_NB_DB10 60                   /* 6 dB over the neighbours */

struct gz {
    int64_t s1[3], s2[3];
    uint32_t n, blocks;                 /* samples in the open block, full blocks */
    uint64_t pw[3];                     /* the blocks' powers >> 8, summed */
    uint64_t sq, cnt;                   /* every sample fed: sum of squares, count (RMS) */
    uint32_t peak;
};

struct gz_verdict {
    int heard;
    int32_t over_base, over_nb;         /* tenths of a dB */
};

static const int64_t gz_coef[3] = { 2129111628LL, 2138474799LL, 2116465521LL };   /* 1000, 700, 1300 Hz (Q30) */

static inline void gz_reset(struct gz *g)
{
    for (int k = 0; k < 3; ++k)
        g->s1[k] = g->s2[k] = 0, g->pw[k] = 0;
    g->n = g->blocks = 0;
    g->sq = g->cnt = 0;
    g->peak = 0;
}

static inline void gz_put(struct gz *g, int32_t x)
{
    uint32_t a = (uint32_t)(x < 0 ? -x : x);
    g->peak = a > g->peak ? a : g->peak;
    g->sq += (uint64_t)((int64_t)x * x);
    g->cnt++;
    for (int k = 0; k < 3; ++k) {
        int64_t s0 = x + ((gz_coef[k] * g->s1[k]) >> 30) - g->s2[k];
        g->s2[k] = g->s1[k];
        g->s1[k] = s0;
    }
    if (++g->n < GZ_N)
        return;
    for (int k = 0; k < 3; ++k) {
        int64_t p1 = g->s1[k], p2 = g->s2[k];
        int64_t p = p1 * p1 + p2 * p2 - ((gz_coef[k] * p1) >> 30) * p2;
        g->pw[k] += (uint64_t)(p > 0 ? p : 0) >> 8;
        g->s1[k] = g->s2[k] = 0;
    }
    g->n = 0;
    g->blocks++;
}

static inline uint64_t gz_mean(const struct gz *g, int bin) { return g->blocks ? g->pw[bin] / g->blocks : 0; }

static inline uint32_t gz_rms(const struct gz *g)
{
    uint64_t m = g->cnt ? g->sq / g->cnt : 0, r = 0, b = 1ULL << 62;
    while (b > m)
        b >>= 2;
    while (b) {
        if (m >= r + b) {
            m -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return (uint32_t)r;
}

/* log2(x) in Q16, x > 0: the integer part, then 16 fraction bits by squaring a Q31 mantissa in [1, 2) */
static inline int32_t gz_log2_q16(uint64_t x)
{
    int32_t n = 63;
    while (n > 0 && !(x >> n))
        n--;
    uint64_t y = n >= 31 ? x >> (n - 31) : x << (31 - n);
    int32_t r = n * 65536;
    for (int b = 15; b >= 0; --b) {
        y = (y * y) >> 31;                      /* y < 2^32: the square fits */
        if (y >= (1ULL << 32)) {
            y >>= 1;
            r += 1 << b;
        }
    }
    return r;
}

static inline int32_t gz_db10(uint64_t a, uint64_t b)
{
    int64_t d = (int64_t)gz_log2_q16(a ? a : 1) - gz_log2_q16(b ? b : 1);
    d *= 30103;                                 /* 10 log10(2) = 3.0103 dB an octave of power */
    return (int32_t)((d + (d < 0 ? -32768000 : 32768000)) / 65536000);     /* rounded */
}

static inline void gz_judge(const struct gz *tone, const struct gz *base, struct gz_verdict *v)
{
    uint64_t t = gz_mean(tone, GZ_1K), nb = (gz_mean(tone, GZ_700) + gz_mean(tone, GZ_1300)) / 2;
    v->over_base = gz_db10(t, gz_mean(base, GZ_1K));
    v->over_nb = gz_db10(t, nb);
    v->heard = tone->blocks && base->blocks && v->over_base >= GZ_HEARD_DB10 && v->over_nb >= GZ_NB_DB10;
}

#endif
