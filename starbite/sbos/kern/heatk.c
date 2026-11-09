/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS heat kernel (docs/STARBITE-OS.md 8.1): what the heater's worker cores run. Eight independent
 * NEON FMA streams (no dependency between them, so the FP pipes stay busy every cycle), no memory traffic,
 * a few tens of microseconds per batch; every batch's result must equal the reference the boot core computed,
 * else the core is unstable and the heater stops.
 *
 * Built with -march=armv8-a (the Mongoose M3 cores are ARMv8.0; build.sh checks the object). The worker stops
 * when the controller's heartbeat is older than max_age ticks or its run flag is cleared. Self-contained:
 * only libsb's worker helpers (smp.c, v8.0) are called. */
#include <arm_neon.h>
#include "sb.h"
#include "kern.h"

#define ITER 24000U

uint64_t heatk_batch(void)
{
    float32x4_t k0 = vdupq_n_f32(0.99991f), k1 = vdupq_n_f32(1.00007f), d = vdupq_n_f32(0.00013f);
    float32x4_t a0 = vdupq_n_f32(1.0f), a1 = vdupq_n_f32(1.5f), a2 = vdupq_n_f32(2.0f), a3 = vdupq_n_f32(2.5f);
    float32x4_t a4 = vdupq_n_f32(3.0f), a5 = vdupq_n_f32(3.5f), a6 = vdupq_n_f32(4.0f), a7 = vdupq_n_f32(4.5f);
    for (unsigned i = 0; i < ITER; ++i) {
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
    float32x4_t s = vaddq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)), vaddq_f32(vaddq_f32(a4, a5), vaddq_f32(a6, a7)));
    uint32x4_t u = vreinterpretq_u32_f32(s);
    return ((uint64_t)vgetq_lane_u32(u, 0) << 32 | vgetq_lane_u32(u, 1)) ^
           ((uint64_t)vgetq_lane_u32(u, 2) << 16) ^ ((uint64_t)vgetq_lane_u32(u, 3) << 40);
}

/* The heat batch (0.2.1, after the review: a DRAM-streaming kernel keeps the cores waiting on memory, the very
 * thing that kept Engineer OS's stress cool). Most batches work inside a 256 KiB window of the core's buffer (the
 * caches hold it): every 64 bytes are converted and scaled into a bounded range (no NaN, no denormals), feed
 * eight FMA streams for 16 rounds (192 FMAs per 64 bytes in 12 streams: the FP pipes stay full) and are written back changed
 * (an xorshift step), so no batch repeats the last. Every 16th batch instead streams 512 KiB from the far part of
 * the 4 MiB buffer (DRAM, the memory system's share of the heat) and runs the fixed reference block, whose result
 * must equal the boot core's. */
#define WIN_WORDS (256U * 1024U / 8U)       /* the cache-resident window */
#define CHUNK_WORDS (32U * 1024U / 8U)      /* 32 KiB per normal batch */
#define FAR_WORDS (512U * 1024U / 8U)       /* 512 KiB per far batch */
_Static_assert(WIN_WORDS + ((HEATK_WORDS - WIN_WORDS) / FAR_WORDS) * FAR_WORDS <= HEATK_WORDS, "far batches stay in the buffer");
_Static_assert(WIN_WORDS % CHUNK_WORDS == 0, "chunks tile the window");

static float32x4_t acc[8][12] __attribute__((aligned(64)));  /* per core: no shared lines */

static void mix(unsigned w, uint64_t *b, uint64_t words, unsigned rounds, uint64_t it)
{
    float32x4_t k0 = vdupq_n_f32(0.99991f), sc = vdupq_n_f32(1.0e-9f);
    /* the xorshift steps alone are linear over GF(2) and cycle (x ^= x << 13 four times is the identity on 32-bit
     * lanes): a seed that differs per batch and per core is added, so the data never comes back */
    uint32x4_t seed = vdupq_n_u32((uint32_t)(it * 0x9e3779b97f4a7c15ULL >> 32) ^ (w * 0x85ebca6bU));
    /* 12 independent streams: an M3 has three FMA pipes of about 4 cycles' latency, so 12 keep it full */
    float32x4_t *A = acc[w & 7U];
    float32x4_t a0 = A[0], a1 = A[1], a2 = A[2], a3 = A[3], a4 = A[4], a5 = A[5], a6 = A[6], a7 = A[7];
    float32x4_t a8 = A[8], a9 = A[9], a10 = A[10], a11 = A[11];
    for (uint64_t i = 0; i < words; i += 8) {
        uint64_t *p = b + i;
        uint32x4_t v0 = vld1q_u32((const uint32_t *)p), v1 = vld1q_u32((const uint32_t *)(p + 2));
        uint32x4_t v2 = vld1q_u32((const uint32_t *)(p + 4)), v3 = vld1q_u32((const uint32_t *)(p + 6));
        float32x4_t d0 = vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(v0, 8)), sc);
        float32x4_t d1 = vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(v1, 8)), sc);
        float32x4_t d2 = vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(v2, 8)), sc);
        float32x4_t d3 = vmulq_f32(vcvtq_f32_u32(vshrq_n_u32(v3, 8)), sc);
        for (unsigned r = 0; r < rounds; ++r) {
            a0 = vfmaq_f32(d0, a0, k0);
            a1 = vfmaq_f32(d1, a1, k0);
            a2 = vfmaq_f32(d2, a2, k0);
            a3 = vfmaq_f32(d3, a3, k0);
            a4 = vfmaq_f32(d0, a4, k0);
            a5 = vfmaq_f32(d1, a5, k0);
            a6 = vfmaq_f32(d2, a6, k0);
            a7 = vfmaq_f32(d3, a7, k0);
            a8 = vfmaq_f32(d0, a8, k0);
            a9 = vfmaq_f32(d1, a9, k0);
            a10 = vfmaq_f32(d2, a10, k0);
            a11 = vfmaq_f32(d3, a11, k0);
        }
        vst1q_u32((uint32_t *)p, vaddq_u32(veorq_u32(v0, vshlq_n_u32(v0, 13)), seed));
        vst1q_u32((uint32_t *)(p + 2), vaddq_u32(veorq_u32(v1, vshrq_n_u32(v1, 7)), seed));
        vst1q_u32((uint32_t *)(p + 4), vaddq_u32(veorq_u32(v2, vshlq_n_u32(v2, 5)), seed));
        vst1q_u32((uint32_t *)(p + 6), vaddq_u32(v3, veorq_u32(seed, vdupq_n_u32(0x9e3779b9U))));
    }
    /* the accumulators stay bounded (|a| < 1 / (1 - 0.99991) x 0.0168): carried to the next batch */
    A[0] = a0, A[1] = a1, A[2] = a2, A[3] = a3, A[4] = a4, A[5] = a5, A[6] = a6, A[7] = a7;
    A[8] = a8, A[9] = a9, A[10] = a10, A[11] = a11;
}

unsigned heatk_mixed(struct heatk_cfg *c, unsigned w, uint64_t it)
{
    uint64_t *b = c->buf[w];
    if ((it & 15U) == 0) {
        unsigned bad = heatk_batch() != c->gold;
        if (b)
            mix(w, b + WIN_WORDS + ((it >> 4) % ((HEATK_WORDS - WIN_WORDS) / FAR_WORDS)) * FAR_WORDS, FAR_WORDS, 1, it);
        return bad;
    }
    if (!b)
        return heatk_batch() != c->gold;
    mix(w, b + (it * CHUNK_WORDS) % WIN_WORDS, CHUNK_WORDS, 16, it);
    return 0;
}

static void line_civac(const volatile void *p)
{
#ifndef SB_HOST
    __asm__ volatile("dsb sy\n\tdc civac, %0\n\tdsb sy" : : "r"(p) : "memory");
#else
    (void)p;
#endif
}

static uint64_t counter(void)
{
#ifndef SB_HOST
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
#else
    return sb_ticks();
#endif
}

void heatk_worker(unsigned w, void *arg)
{
    struct heatk_cfg *c = arg;
    struct sb_wslot *s = sb_smp_own_slot(w);
    if (!s || w >= 8)
        return;
    uint64_t iters = 0;
#ifndef SB_HOST
    __asm__ volatile("msr fpcr, xzr" ::: "memory");   /* the same FP mode as the boot core's reference */
#endif
    for (;;) {
        line_civac(c);
        if (!c->run[w] || sb_worker_stop())
            break;
        uint64_t hb = c->heartbeat, now = counter();
        if (now > hb && now - hb > c->max_age) {
            s->v[0] = 1;                        /* stopped by the dead man's switch */
            break;
        }
        s->errors += heatk_mixed(c, w, iters);
#ifndef SB_HOST
        /* 1.2-alpha6: the M3 GCU's throttle counter and control (libsb ocp.c's registers), only after the read-only
         * OCP probe found them readable this boot (heater.c sets ocp_read): slot v[2] OCPTHROTTCNTA, v[3] CTL */
        if (w == 4 && c->ocp_read && !(iters & 255U)) {
            uint64_t r;
            __asm__ volatile("mrs %0, S3_0_C15_C14_4" : "=r"(r));
            s->v[2] = r;
            __asm__ volatile("mrs %0, S3_0_C15_C12_7" : "=r"(r));
            s->v[3] = r;
        }
#endif
        s->iters = ++iters;
        sb_worker_publish(s);
        if (s->errors)
            break;
    }
}
