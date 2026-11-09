/* SPDX-License-Identifier: GPL-2.0-only
 * Temperatures (TMU) and clocks, read only: port of shim/starlte/em.c fill_thermal / pll_hz /
 * top_switch (build/em-research-thermal-clocks.md). Clock registers are only read; the APM
 * firmware owns DVFS. CMU_CPUCL1 and CMU_G3D are read only while their power domain reports on.
 * Every probe value is filtered (sb_sample_filter, adc.c): code 0 / all ones and impossible
 * temperatures are rejected and counted, the value is the median of 3 samples.
 */
#include "sb.h"

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static const unsigned char big_probe[5] = { 1, 2, 3, 4, 6 };   /* DT mask 0x5E */

/* One reading of probe p's CURRENT_TEMP code (9 bits) and its conversion with the probe's trim. */
static int code_of(uintptr_t base, unsigned p)
{
    uintptr_t off = p < 2 ? 0x40U : 0x44U + 4U * ((p - 2U) / 3U);
    unsigned sh = p < 2 ? 9U * p : 9U * ((p - 2U) % 3U);
    return (int)((rd(base + off) >> sh) & 0x1ffU);
}

static int code_to_c(uintptr_t base, unsigned p, int code, unsigned *flags)
{
    uint32_t tri = rd(base + 4U * p);
    int te1 = (int)(tri & 0x1ffU), te2 = (int)((tri >> 9) & 0x1ffU);
    int two = (int)((rd(base) >> 23) & 1U);
    if (!te1) {
        te1 = 55;
        if (flags)
            *flags |= 1U;
    }
    return (two && te2 > te1) ? (code - te1) * 60 / (te2 - te1) + 25 : code - te1 + 25;
}

/* Session counters per probe (TOP 0..7, SUB 8..10): readings, rejected samples, last rejected
 * code. The TMU has no "valid" bit for CURRENT_TEMP (exynos_tmu.c reads it as plain status), so a
 * sample is rejected when its code is 0 or all ones (the glitch the user saw: every probe reading
 * code 0 now and then, shown as about -20 C) or its temperature is outside -30..150 C; the value
 * is the median of 3 accepted samples taken 10 us apart (6 tries at most). */
static uint32_t tmu_reads[11], tmu_rej[11];
static int tmu_bad[11] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

struct tmu_ctx {
    uintptr_t base;
    unsigned p, flags;
    int last_code, bad_code;
};

static int tmu_get(void *v)
{
    struct tmu_ctx *c = v;
    int code = code_of(c->base, c->p);
    sb_udelay(10);
    int t = code_to_c(c->base, c->p, code, &c->flags);
    if (code == 0 || code == 0x1ff || t < -30 || t >= 200) {      /* 1.2-alpha4: was > 150 (Survival "No limit") */
        c->bad_code = code;
        return SB_NO_SAMPLE;                    /* rejected (counted by the filter) */
    }
    c->last_code = code;
    return t;
}

static int tmu_filtered(uintptr_t base, unsigned p, unsigned *flags, int *code_out)
{
    unsigned slot = (base == SB_TMU_SUB ? 8U : 0U) + p, rej = 0;
    struct tmu_ctx c = { base, p, 0, 0, -1 };
    int bad = 0;
    int t = sb_sample_filter(tmu_get, &c, -30, 150, 3, 6, &rej, &bad);
    if (slot < 11) {
        ++tmu_reads[slot];
        tmu_rej[slot] += rej;
        if (rej)
            tmu_bad[slot] = c.bad_code;
    }
    if (flags)
        *flags |= c.flags;
    if (code_out)
        *code_out = c.last_code;
    return t == SB_NO_SAMPLE ? -100 : t;
}

int sb_tmu_temp(uintptr_t base, unsigned p, unsigned *flags)
{
    return tmu_filtered(base, p, flags, 0);
}

void sb_tmu_stats(unsigned slot, uint32_t *reads, uint32_t *rejected, int *last_bad)
{
    if (slot >= 11) {
        *reads = *rejected = 0;
        *last_bad = -1;
        return;
    }
    *reads = tmu_reads[slot];
    *rejected = tmu_rej[slot];
    *last_bad = tmu_bad[slot];
}

int sb_tmu_state(uintptr_t base)
{
    if (!(rd(base + 0x20) & 1U))
        return 0;
    return (rd(base + 0x160) & 1U) ? 2 : 1;
}

/* The hardware trip S-Boot armed (CONTROL bit 12, THERM_TRIP_EN): THD_TEMP0 0x50 [24:16] is the
 * rising code (te1 + 90 for 115 C in one-point mode: build/em-research-thermal-clocks.md 1.4),
 * converted with probe 0's trim. -100 when the trip is not armed. Decoded from the registers, not
 * seen firing: what the trip does (the TMU signals the PMU, which cuts the power) is the vendor
 * driver's description, and which probes it watches is not known. */
static int trip_c(uintptr_t base)
{
    if (!(rd(base + 0x20) & 0x1000U))
        return -100;
    uint32_t tri = rd(base);
    int te1 = (int)(tri & 0x1ffU), te2 = (int)((tri >> 9) & 0x1ffU), two = (int)((tri >> 23) & 1U);
    int code = (int)((rd(base + 0x50) >> 16) & 0x1ffU);
    if (!te1)
        te1 = 55;
    if (!code)
        return -100;
    return (two && te2 > te1) ? (code - te1) * 60 / (te2 - te1) + 25 : code - te1 + 25;
}

void sb_temps_read(struct sb_temps *t)
{
    t->top = sb_tmu_state(SB_TMU_TOP);
    t->sub = sb_tmu_state(SB_TMU_SUB);
    t->hottest = -100;
    t->flags = 0;
    t->big_max = t->little = t->gpu = t->isp = -100;
    for (unsigned i = 0; i < 8; ++i) {
        t->code_top[i] = 0;
        t->top_all[i] = t->top ? tmu_filtered(SB_TMU_TOP, i, &t->flags, &t->code_top[i]) : -100;
    }
    for (unsigned i = 0; i < 3; ++i) {
        t->code_sub[i] = 0;
        t->sub_all[i] = t->sub ? tmu_filtered(SB_TMU_SUB, i, &t->flags, &t->code_sub[i]) : -100;
    }
    t->trip_c = t->top ? trip_c(SB_TMU_TOP) : -100;
    t->trip_sub_c = t->sub ? trip_c(SB_TMU_SUB) : -100;
    t->trip_armed = (t->top && (rd(SB_TMU_TOP + 0x20) & 0x1000U) ? 1 : 0) |
                    (t->sub && (rd(SB_TMU_SUB + 0x20) & 0x1000U) ? 2 : 0);
    if (t->top) {
        for (unsigned i = 0; i < 5; ++i) {
            t->big[i] = t->top_all[big_probe[i]];
            if (t->big[i] > t->big_max)
                t->big_max = t->big[i];
        }
        t->little = t->top_all[5];
        t->hottest = t->big_max > t->little ? t->big_max : t->little;
    }
    if (t->sub) {
        t->gpu = t->sub_all[1];
        t->isp = t->sub_all[2];
        if (t->gpu > t->hottest)
            t->hottest = t->gpu;
        if (t->isp > t->hottest)
            t->hottest = t->isp;
    }
}

/* PLL_CON0: [31] enable, [29] locked, [25:16] M, [13:8] P, [4] mux (0 = 26 MHz), [2:0] S */
uint64_t sb_pll_hz(uintptr_t con0, int *unlocked)
{
    uint32_t v = rd(con0);
    if (!(v & 0x10U))
        return 26000000U;
    if (!(v & 0x80000000U))
        return 0;
    if (unlocked && !(v & 0x20000000U))
        *unlocked = 1;
    uint64_t m = (v >> 16) & 0x3ffU, p = (v >> 8) & 0x3fU, s = v & 7U;
    return p ? 26000000ULL * m / (p << s) : 0;
}

static uint64_t top_switch(uintptr_t user, uintptr_t mux, uintptr_t div)
{
    uint64_t src;
    if (!((rd(user) >> 4) & 1U))
        return 26000000U;
    switch (rd(mux) & 3U) {
    case 0: src = sb_pll_hz(0x1a240120UL, 0) / ((rd(0x1a2418d4UL) & 1U) + 1U); break;
    case 1: src = sb_pll_hz(0x1a2401a0UL, 0) / ((rd(0x1a2418e0UL) & 1U) + 1U); break;
    case 2: src = sb_pll_hz(0x1a240220UL, 0); break;
    default: src = sb_pll_hz(0x1a240320UL, 0); break;
    }
    return src / ((rd(div) & 7U) + 1U);
}

int sb_m3_on(void)
{
    return (rd(0x14062044UL) & 0xfU) == 0xfU;
}

int sb_g3d_on(void)
{
    return (rd(0x14064044UL) & 0xfU) == 0xfU;
}

uint64_t sb_a55_hz(void)
{
    uint64_t src = (rd(0x1d00100cUL) & 1U) ? top_switch(0x1d000120UL, 0x1a241034UL, 0x1a241830UL)
                                           : sb_pll_hz(0x1d000140UL, 0);
    return src / ((rd(0x1d001818UL) & 0xfffU) + 1U);
}

uint64_t sb_m3_hz(int *off)
{
    *off = !sb_m3_on();
    if (*off)
        return 0;
    uint64_t src = (rd(0x1d101000UL) & 1U) ? top_switch(0x1d100100UL, 0x1a241038UL, 0x1a241834UL)
                                           : sb_pll_hz(0x1d100120UL, 0);
    return src / ((rd(0x1d10180cUL) & 0xfffU) + 1U);
}

uint64_t sb_gpu_hz(int *off)
{
    *off = !sb_g3d_on();
    return *off ? 0 : sb_pll_hz(0x17400140UL, 0);
}
