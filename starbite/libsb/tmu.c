/* SPDX-License-Identifier: GPL-2.0-only
 * Temperatures (TMU) and clocks, read only: port of shim/starlte/em.c fill_thermal / pll_hz /
 * top_switch (build/em-research-thermal-clocks.md). Clock registers are only read; the APM
 * firmware owns DVFS. CMU_CPUCL1 and CMU_G3D are read only while their power domain reports on.
 */
#include "sb.h"

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static const unsigned char big_probe[5] = { 1, 2, 3, 4, 6 };   /* DT mask 0x5E */

int sb_tmu_temp(uintptr_t base, unsigned p, unsigned *flags)
{
    uint32_t tri = rd(base + 4U * p);
    int te1 = (int)(tri & 0x1ffU), te2 = (int)((tri >> 9) & 0x1ffU);
    int two = (int)((rd(base) >> 23) & 1U);
    if (!te1) {
        te1 = 55;
        *flags |= 1U;
    }
    uintptr_t off = p < 2 ? 0x40U : 0x44U + 4U * ((p - 2U) / 3U);
    unsigned sh = p < 2 ? 9U * p : 9U * ((p - 2U) % 3U);
    int code = (int)((rd(base + off) >> sh) & 0x1ffU);
    return (two && te2 > te1) ? (code - te1) * 60 / (te2 - te1) + 25 : code - te1 + 25;
}

int sb_tmu_state(uintptr_t base)
{
    if (!(rd(base + 0x20) & 1U))
        return 0;
    return (rd(base + 0x160) & 1U) ? 2 : 1;
}

void sb_temps_read(struct sb_temps *t)
{
    t->top = sb_tmu_state(SB_TMU_TOP);
    t->sub = sb_tmu_state(SB_TMU_SUB);
    t->hottest = -100;
    t->flags = 0;
    t->big_max = t->little = t->gpu = t->isp = -100;
    if (t->top) {
        for (unsigned i = 0; i < 5; ++i) {
            t->big[i] = sb_tmu_temp(SB_TMU_TOP, big_probe[i], &t->flags);
            if (t->big[i] > t->big_max)
                t->big_max = t->big[i];
        }
        t->little = sb_tmu_temp(SB_TMU_TOP, 5, &t->flags);
        t->hottest = t->big_max > t->little ? t->big_max : t->little;
    }
    if (t->sub) {
        t->gpu = sb_tmu_temp(SB_TMU_SUB, 1, &t->flags);
        t->isp = sb_tmu_temp(SB_TMU_SUB, 2, &t->flags);
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
