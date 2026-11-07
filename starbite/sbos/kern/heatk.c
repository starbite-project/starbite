/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS heat kernel (docs/STARBITE-OS.md 8.1): what the heater's worker cores run. Eight independent
 * NEON FMA streams (no dependency between them, so the FP pipes stay busy every cycle), no memory traffic,
 * about 0.5 ms per batch at 1.8 GHz; every batch's result must equal the reference the boot core computed,
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
    for (;;) {
        line_civac(c);
        if (!c->run[w] || sb_worker_stop())
            break;
        uint64_t hb = c->heartbeat, now = counter();
        if (now > hb && now - hb > c->max_age) {
            s->v[0] = 1;                        /* stopped by the dead man's switch */
            break;
        }
        uint64_t r = heatk_batch();
        if (r != c->gold)
            s->errors++;
        s->iters = ++iters;
        sb_worker_publish(s);
        if (s->errors)
            break;
    }
}
