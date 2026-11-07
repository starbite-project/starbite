/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite thermal core (see sbth_core.h). Read-only towards the hardware: only rd() is used.
 */
#include "sbth_core.h"

int sbth_code_to_mc(int code, int te1, int te2, int two_point)
{
    if (te1 == 0)
        te1 = 55;                         /* the DT's efuse fallback (exynos_tmu.c), as libsb */
    if (two_point && te2 > te1)
        return (code - te1) * 60000 / (te2 - te1) + 25000;
    return (code - te1) * 1000 + 25000;
}

int sbth_read_code(const struct sbth_io *io, unsigned block, unsigned p)
{
    unsigned off = p < 2U ? 0x40U : 0x44U + 4U * ((p - 2U) / 3U);
    unsigned sh = p < 2U ? 9U * p : 9U * ((p - 2U) % 3U);
    return (int)((io->rd(io->ctx, block, off) >> sh) & 0x1ffU);
}

static int trim_mc(const struct sbth_io *io, unsigned block, unsigned p, int code)
{
    sbth_u32 tri = io->rd(io->ctx, block, SBTH_TRIMINFO(p));
    int two = (int)((io->rd(io->ctx, block, SBTH_TRIMINFO(0)) >> 23) & 1U);
    return sbth_code_to_mc(code, (int)(tri & 0x1ffU), (int)((tri >> 9) & 0x1ffU), two);
}

int sbth_probe_mc(const struct sbth_io *io, unsigned block, unsigned p, unsigned *rejected)
{
    int v[3];
    unsigned n = 0, rej = 0;
    for (unsigned tries = 0; tries < 6U && n < 3U; ++tries) {
        int code = sbth_read_code(io, block, p);
        int mc = trim_mc(io, block, p, code);
        if (code == 0 || code == 0x1ff || mc < -30000 || mc > 150000)
            ++rej;
        else
            v[n++] = mc;
        if (io->delay_us)
            io->delay_us(io->ctx, 10);
    }
    if (rejected)
        *rejected = rej;
    if (n < 3U)
        return SBTH_NO_TEMP;
    /* median of three */
    int a = v[0], b = v[1], c = v[2];
    if ((a <= b && b <= c) || (c <= b && b <= a))
        return b;
    if ((b <= a && a <= c) || (c <= a && a <= b))
        return a;
    return c;
}

int sbth_block_live(const struct sbth_io *io, unsigned block)
{
    return (io->rd(io->ctx, block, SBTH_CONTROL) & 1U) && !(io->rd(io->ctx, block, SBTH_EMUL_CON) & 1U);
}

int sbth_trip_armed(const struct sbth_io *io, unsigned block)
{
    return (io->rd(io->ctx, block, SBTH_CONTROL) & 0x1001U) == 0x1001U;
}

int sbth_trip_mc(const struct sbth_io *io, unsigned block)
{
    int code;
    if (!sbth_trip_armed(io, block))
        return SBTH_NO_TEMP;
    code = (int)((io->rd(io->ctx, block, SBTH_THD_TEMP0) >> 16) & 0x1ffU);
    if (!code)
        return SBTH_NO_TEMP;
    return trim_mc(io, block, 0, code);
}

void sbth_read_all(const struct sbth_io *io, struct sbth_reading *r)
{
    int live[2];
    live[0] = sbth_block_live(io, 0);
    live[1] = sbth_block_live(io, 1);
    r->rejected = 0;
    r->hottest_mc = SBTH_NO_TEMP;
    for (unsigned s = 0; s < SBTH_SLOTS; ++s) {
        unsigned block = s < 8U ? 0U : 1U, p = s < 8U ? s : s - 8U, rej = 0;
        r->mc[s] = live[block] ? sbth_probe_mc(io, block, p, &rej) : SBTH_NO_TEMP;
        r->rejected += rej;
        /* The probes with a known place: TOP 1..6 (big and little cores), SUB 1..2 (GPU, ISP). */
        int used = (block == 0U && p >= 1U && p <= 6U) || (block == 1U && p >= 1U);
        if (used && r->mc[s] != SBTH_NO_TEMP && r->mc[s] > r->hottest_mc)
            r->hottest_mc = r->mc[s];
    }
    r->gpu_mc = r->mc[SBTH_SLOT_GPU];
}

void sbth_levels_init(struct sbth_levels *s)
{
    s->level = SBTH_NORMAL;
    s->above = s->below = s->blind = 0;
    s->want_up = SBTH_CRITICAL;
    s->want_down = SBTH_NORMAL;
    s->max_gpu_mc = s->max_any_mc = SBTH_NO_TEMP;
}

enum sbth_level sbth_level_for(int gpu_mc, int hottest_mc)
{
    if (hottest_mc != SBTH_NO_TEMP && hottest_mc >= SBTH_CRIT_ANY_MC)
        return SBTH_CRITICAL;
    if (gpu_mc == SBTH_NO_TEMP)
        return SBTH_STOP;                 /* no GPU temperature: never run the GPU on faith */
    if (gpu_mc >= SBTH_CRIT_GPU_MC)
        return SBTH_CRITICAL;
    if (gpu_mc >= SBTH_STOP_MC)
        return SBTH_STOP;
    if (gpu_mc >= SBTH_HOT_MC)
        return SBTH_HOT;
    if (gpu_mc >= SBTH_WARM_MC)
        return SBTH_WARM;
    return SBTH_NORMAL;
}

enum sbth_level sbth_level_hold(int gpu_mc, int hottest_mc)
{
    if (hottest_mc != SBTH_NO_TEMP && hottest_mc >= SBTH_CRIT_ANY_MC - SBTH_HYST_MC)
        return SBTH_CRITICAL;
    if (gpu_mc == SBTH_NO_TEMP)
        return SBTH_STOP;
    if (gpu_mc >= SBTH_CRIT_GPU_MC - SBTH_HYST_MC)
        return SBTH_CRITICAL;
    if (gpu_mc >= SBTH_STOP_MC - SBTH_HYST_MC)
        return SBTH_STOP;
    if (gpu_mc >= SBTH_HOT_MC - SBTH_HYST_MC)
        return SBTH_HOT;
    if (gpu_mc >= SBTH_WARM_MC - SBTH_HYST_MC)
        return SBTH_WARM;
    return SBTH_NORMAL;
}

int sbth_levels_step(struct sbth_levels *s, int gpu_mc, int hottest_mc)
{
    enum sbth_level old = s->level, up, hold;

    if (gpu_mc != SBTH_NO_TEMP && (s->max_gpu_mc == SBTH_NO_TEMP || gpu_mc > s->max_gpu_mc))
        s->max_gpu_mc = gpu_mc;
    if (hottest_mc != SBTH_NO_TEMP && (s->max_any_mc == SBTH_NO_TEMP || hottest_mc > s->max_any_mc))
        s->max_any_mc = hottest_mc;

    /* A missing GPU temperature only counts once it repeats: one bad reading is noise. */
    if (gpu_mc == SBTH_NO_TEMP) {
        if (++s->blind < SBTH_BLIND_READINGS && s->level < SBTH_STOP) {
            s->above = s->below = 0;
            return 0;
        }
    } else {
        s->blind = 0;
    }

    up = sbth_level_for(gpu_mc, hottest_mc);
    hold = sbth_level_hold(gpu_mc, hottest_mc);
    if (gpu_mc == SBTH_NO_TEMP && s->blind >= SBTH_BLIND_READINGS) {
        /* Blind: straight to Stop (or stay higher), no rise delay. */
        s->above = s->below = 0;
        if (s->level < SBTH_STOP)
            s->level = SBTH_STOP;
        return s->level != old;
    }

    if (up > s->level) {
        s->below = 0;
        if (s->above == 0 || up < s->want_up)
            s->want_up = up;
        if (++s->above >= SBTH_RISE_READINGS) {
            s->level = s->want_up;        /* the lowest level all readings of the run agree on */
            s->above = 0;
            s->want_up = SBTH_CRITICAL;
        }
    } else if (hold < s->level) {
        s->above = 0;
        if (s->below == 0 || hold > s->want_down)
            s->want_down = hold;
        if (++s->below >= SBTH_FALL_READINGS) {
            s->level = s->want_down;      /* the highest level any reading of the run still held */
            s->below = 0;
            s->want_down = SBTH_NORMAL;
        }
    } else {
        s->above = s->below = 0;
    }
    return s->level != old;
}

/* ---- the A55 clock ------------------------------------------------------------------------------ */

static sbth_u32 crd(const struct sbth_io *io, sbth_u64 pa)
{
    if (pa >= SBTH_CMU_TOP_PA && pa < SBTH_CMU_TOP_PA + SBTH_CMU_SIZE)
        return io->rd(io->ctx, 2, (unsigned)(pa - SBTH_CMU_TOP_PA));
    if (pa >= SBTH_CMU_CPUCL0_PA && pa < SBTH_CMU_CPUCL0_PA + SBTH_CMU_SIZE)
        return io->rd(io->ctx, 3, (unsigned)(pa - SBTH_CMU_CPUCL0_PA));
    return 0;
}

/* PLL_CON0: [31] enable, [29] locked, [25:16] M, [13:8] P, [4] mux (0 = 26 MHz), [2:0] S. In kHz. */
static sbth_u64 pll_khz(const struct sbth_io *io, sbth_u64 con0)
{
    sbth_u32 v = crd(io, con0);
    sbth_u64 m = (v >> 16) & 0x3ffU, p = (v >> 8) & 0x3fU, s = v & 7U;
    if (!(v & 0x10U))
        return 26000U;
    if (!(v & 0x80000000U) || !p)
        return 0;
    return 26000ULL * m / (p << s);
}

static sbth_u64 top_switch(const struct sbth_io *io, sbth_u64 user, sbth_u64 mux, sbth_u64 div)
{
    sbth_u64 src;
    if (!((crd(io, user) >> 4) & 1U))
        return 26000U;
    switch (crd(io, mux) & 3U) {
    case 0: src = pll_khz(io, 0x1a240120ULL) / ((crd(io, 0x1a2418d4ULL) & 1U) + 1U); break;
    case 1: src = pll_khz(io, 0x1a2401a0ULL) / ((crd(io, 0x1a2418e0ULL) & 1U) + 1U); break;
    case 2: src = pll_khz(io, 0x1a240220ULL); break;
    default: src = pll_khz(io, 0x1a240320ULL); break;
    }
    return src / ((crd(io, div) & 7U) + 1U);
}

sbth_u32 sbth_a55_khz(const struct sbth_io *io)
{
    sbth_u64 src = (crd(io, 0x1d00100cULL) & 1U) ? top_switch(io, 0x1d000120ULL, 0x1a241034ULL, 0x1a241830ULL)
                                                  : pll_khz(io, 0x1d000140ULL);
    return (sbth_u32)(src / ((crd(io, 0x1d001818ULL) & 0xfffU) + 1U));
}

int sbth_a55_allowed(sbth_u32 khz)
{
    return khz == 1690000U || khz == 1456000U || khz == 1248000U || khz == 1053000U;
}

sbth_u32 sbth_cpu_target(enum sbth_level cpu_level, sbth_u32 boost_khz)
{
    if (!boost_khz || !sbth_a55_allowed(boost_khz))
        return 0;
    if (cpu_level == SBTH_NORMAL)
        return boost_khz;
    if (cpu_level == SBTH_WARM)
        return boost_khz < SBTH_A55_WARM_KHZ ? boost_khz : SBTH_A55_WARM_KHZ;
    return SBTH_A55_SBOOT_KHZ;
}
