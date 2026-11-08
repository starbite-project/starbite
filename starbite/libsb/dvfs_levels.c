/* SPDX-License-Identifier: GPL-2.0-only
 * The ACPM DVFS levels Starbite OS may ask for (sb.h sb_acpm_dvfs_listed; docs/STARBITE-OS.md 8.3). Pure:
 * the host test checks the list. */
#include "sb.h"

/* Starbite OS's listed levels (docs/STARBITE-OS.md 8.3): the vendor tables' levels (DT cpufreq domain@0
 * mif-perf table for the A55, the big cluster's table for the M3). The chip's own ECT (build/em-research-g3d-ect.md)
 * caps them: GEN MINMAX_dvfs_cpucl0 max 1794 MHz for every ASV version. 1898 / 1950 MHz (in the DT's and the ECT's
 * tables, with the ECT's own ASV voltages) are listed for Manual only, with a warning (the main session's rule,
 * 2026-11-08); Extreme stays at 1794 (heatctl.c A55_TOP). 2002 MHz never. Every M3 level above 1794 MHz stays out: the
 * DT's exynos-ocp node allows them only with Samsung's over-current handler (max-freq-wo-ocp 1794 MHz). */
static const uint32_t lv_a55[] = { 455000, 598000, 715000, 832000, 949000, 1053000, 1248000, 1456000, 1690000, 1794000,
                                   1898000, 1950000 };
static const uint32_t lv_m3[] = { 650000, 741000, 858000, 962000, 1066000, 1170000, 1261000, 1469000, 1586000,
                                  1690000, 1794000 };

/* the GPU (domain 4): the vendor DT's gpu_dvfs_table; 572 MHz ran for days under Starbite's iOS at 950 mV. 0.6.2:
 * 598 MHz, the ECT's dvfs_g3d top level with its own ASV voltages, inside the ECT's MINMAX_dvfs_g3d (max 598 for
 * every ASV version); the DT caps Android at 572. Manual only. */
static const uint32_t lv_g3d[] = { 260000, 299000, 338000, 455000, 546000, 572000, 598000 };

/* 1.1-beta6: Samsung's exynos-ocp core-count table for the M3 cluster (exynos9810.dtsi; the vendor driver): busy M3
 * cores -> the highest level without its over-current handler's throttle: 1 core 2704, 2 cores 2314, 3-4 cores
 * 1794 MHz (max-freq-wo-ocp). Four cores above 1794 trip the cluster buck's hard OCP: an instant power-off (Engineer
 * OS oc8, 4 x 2886). The cluster has one clock, so every M3 core runs at the same level. */
uint32_t sb_m3_ocp_max_khz(unsigned busy)
{
    return busy <= 1 ? 2704000U : busy == 2 ? 2314000U : 1794000U;
}

/* the M3 levels above the list (ECT dvfs_cpucl1 steps, their own ASV voltages), each only with few enough busy cores */
int sb_m3_ocp_level_ok(uint32_t khz, unsigned busy)
{
    if (sb_dvfs_level_listed(3, khz))
        return 1;
    return (khz == 2314000U || khz == 2704000U) && khz <= sb_m3_ocp_max_khz(busy);
}

int sb_dvfs_level_listed(uint32_t domain, uint32_t khz)
{
    const uint32_t *t = domain == 2 ? lv_a55 : domain == 3 ? lv_m3 : domain == 4 ? lv_g3d : 0;
    unsigned n = domain == 2 ? sizeof(lv_a55) / sizeof(lv_a55[0]) :
                 domain == 3 ? sizeof(lv_m3) / sizeof(lv_m3[0]) : sizeof(lv_g3d) / sizeof(lv_g3d[0]);
    if (!t)
        return 0;
    for (unsigned i = 0; i < n; ++i)
        if (t[i] == khz)
            return 1;
    return 0;
}

