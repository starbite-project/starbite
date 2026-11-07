/* SPDX-License-Identifier: GPL-2.0-only
 * Stress kernels for every core (Starbite Lab CPU stress, thermal and battery load).
 *
 * Built with -march=armv8-a (build.sh): they run on the Mongoose M3 cores too, which have no
 * ARMv8.1+ instructions; the build disassembles this object and refuses LSE atomics and other
 * v8.1+ instructions. Self-contained: no call outside this file except the worker slot helpers in
 * libsb/smp.c (also v8.0).
 *
 * Each kernel computes a fixed function of a fixed seed, so every batch must give the same result
 * on every core: the boot core computes the reference once (kern_gold) and every batch on every
 * core is compared with it. A mismatch is an error (a bit flip in a register, the ALU, the FPU,
 * the caches). The memory kernel writes and checks its own per-core buffer (a different pattern
 * every batch).
 *   INT   64-bit add / xor / rotate / multiply / multiply-high / divide chains
 *   FP    double precision: Horner polynomials, square roots, divisions
 *   NEON  128-bit integer multiply-accumulate and single-precision fused multiply-add lanes
 *   MEM   a per-core buffer (larger than L2) written with an address hash and read back
 *   HEAT  (2.2) eight independent single-precision FMA streams, no memory traffic: every FP pipe busy every
 *         cycle, the most power a core draws (the others are dependency chains or wait on DRAM)
 */
#include <arm_neon.h>
#include "eng.h"

#define SEED_INT 0x5bd1e9955bd1e995ULL
#define SEED_FP 0x2545f4914f6cdd1dULL
#define SEED_NEON 0x9e3779b97f4a7c15ULL

static uint64_t k_int(uint64_t s)
{
    uint64_t a = s ^ 0x9e3779b97f4a7c15ULL, b = s * 0xbf58476d1ce4e5b9ULL + 1U, c = 0x94d049bb133111ebULL, d = s;
    for (unsigned i = 0; i < 6144; ++i) {
        a += b;
        b ^= a >> 31;
        b *= 0xff51afd7ed558ccdULL;
        c = (c << 13 | c >> 51) ^ a;
        d += c * (uint64_t)(i | 1U);
        a ^= d >> 7;
        unsigned __int128 m = (unsigned __int128)a * c;
        d ^= (uint64_t)(m >> 64);
        if (!(i & 63U))
            a += d / (((b >> 40) | 1U));
    }
    return a ^ b ^ c ^ d;
}

static uint64_t k_fp(uint64_t s)
{
    double x[16], acc = 0;
    for (unsigned i = 0; i < 16; ++i)
        x[i] = 1.0 + (double)((s >> (i * 4U)) & 15U) / 16.0 + (double)i * 0.001;
    for (unsigned it = 0; it < 160; ++it)
        for (unsigned i = 0; i < 16; ++i) {
            double v = x[i];
            v = (((v * 0.5 + 0.25) * v - 0.125) * v + 0.0625) * v;
            v = __builtin_sqrt(v * v + 1.0);
            v = v / (1.0 + v * 0.001) - 0.5;
            if (v > 4.0)
                v = v * 0.25;
            x[i] = v + (double)it * 1e-6;
            acc += x[i];
        }
    union {
        double d;
        uint64_t u;
    } r = { acc };
    return r.u;
}

static uint64_t k_neon(uint64_t s)
{
    uint32x4_t a = vdupq_n_u32((uint32_t)s), b = vdupq_n_u32((uint32_t)(s >> 32) | 1U);
    uint32x4_t c = vaddq_u32(vdupq_n_u32(0x01000193U), a);
    float32x4_t f = vdupq_n_f32(1.0f), g = vdupq_n_f32(0.999f), h = vdupq_n_f32(0.001f), q = vdupq_n_f32(0.5f);
    for (unsigned i = 0; i < 3072; ++i) {
        a = vaddq_u32(a, b);
        b = veorq_u32(b, vshlq_n_u32(a, 3));
        c = vmlaq_u32(c, a, b);
        a = vorrq_u32(vshrq_n_u32(a, 7), vshlq_n_u32(c, 25));
        f = vfmaq_f32(h, f, g);
        q = vfmaq_f32(q, f, h);
        g = vmulq_f32(g, vdupq_n_f32(1.0000001f));
    }
    uint32x4_t fx = vreinterpretq_u32_f32(vaddq_f32(f, q));
    uint32x4_t m = veorq_u32(veorq_u32(a, b), veorq_u32(c, fx));
    return ((uint64_t)vgetq_lane_u32(m, 0) | ((uint64_t)vgetq_lane_u32(m, 1) << 32)) ^
           ((uint64_t)vgetq_lane_u32(m, 2) << 16) ^ ((uint64_t)vgetq_lane_u32(m, 3) << 48);
}

static uint64_t k_heat(void)
{
    float32x4_t k0 = vdupq_n_f32(0.99991f), k1 = vdupq_n_f32(1.00007f), d = vdupq_n_f32(0.00013f);
    float32x4_t a0 = vdupq_n_f32(1.0f), a1 = vdupq_n_f32(1.5f), a2 = vdupq_n_f32(2.0f), a3 = vdupq_n_f32(2.5f);
    float32x4_t a4 = vdupq_n_f32(3.0f), a5 = vdupq_n_f32(3.5f), a6 = vdupq_n_f32(4.0f), a7 = vdupq_n_f32(4.5f);
    for (unsigned i = 0; i < 24000U; ++i) {
        float32x4_t k = (i & 1U) ? k1 : k0;
        a0 = vfmaq_f32(d, a0, k);
        a1 = vfmaq_f32(d, a1, k);
        a2 = vfmaq_f32(d, a2, k);
        a3 = vfmaq_f32(d, a3, k);
        a4 = vfmaq_f32(d, a4, k);
        a5 = vfmaq_f32(d, a5, k);
        a6 = vfmaq_f32(d, a6, k);
        a7 = vfmaq_f32(d, a7, k);
    }
    float32x4_t t = vaddq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)), vaddq_f32(vaddq_f32(a4, a5), vaddq_f32(a6, a7)));
    uint32x4_t u = vreinterpretq_u32_f32(t);
    return ((uint64_t)vgetq_lane_u32(u, 0) << 32 | vgetq_lane_u32(u, 1)) ^
           ((uint64_t)vgetq_lane_u32(u, 2) << 16) ^ ((uint64_t)vgetq_lane_u32(u, 3) << 40);
}

static uint64_t hash64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}

static unsigned k_mem(uint64_t *buf, uint64_t words, uint64_t seed)
{
    volatile uint64_t *m = buf;
    unsigned bad = 0;
    for (uint64_t i = 0; i < words; ++i)
        m[i] = hash64(i ^ seed);
    for (uint64_t i = 0; i < words; ++i)
        bad += m[i] != hash64(i ^ seed);
    return bad;
}

void kern_gold(struct kern_cfg *c)
{
    c->gold[0] = k_int(SEED_INT);
    c->gold[1] = k_fp(SEED_FP);
    c->gold[2] = k_neon(SEED_NEON);
    c->gold[3] = k_heat();
}

unsigned kern_batch(struct kern_cfg *c, unsigned core, uint64_t *iters)
{
    unsigned bad = 0;
    uint64_t inj = c->inject == core + 1U ? 1U : 0;
    if (inj)
        c->inject = 0;
    if (c->mask & KERN_INT)
        bad += (k_int(SEED_INT) ^ inj) != c->gold[0];
    if (c->mask & KERN_FP)
        bad += k_fp(SEED_FP) != c->gold[1];
    if (c->mask & KERN_NEON)
        bad += k_neon(SEED_NEON) != c->gold[2];
    if (c->mask & KERN_HEAT)
        bad += k_heat() != c->gold[3];
    if ((c->mask & KERN_MEM) && core < SB_NCORES && c->buf[core] && c->words)
        bad += k_mem(c->buf[core], c->words, *iters * 0x9e3779b97f4a7c15ULL + core);
    ++*iters;
    return bad;
}

void kern_worker(unsigned w, void *arg)
{
    struct kern_cfg *c = arg;
    struct sb_wslot *s = sb_smp_own_slot(w);
    uint64_t iters = 0;
    if (!s)
        return;
    while (!sb_worker_stop()) {
        unsigned e = kern_batch(c, w, &iters);
        s->errors += e;
        s->iters = iters;
        sb_worker_publish(s);
    }
}
