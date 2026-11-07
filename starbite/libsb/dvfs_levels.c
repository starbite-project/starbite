/* SPDX-License-Identifier: GPL-2.0-only
 * The ACPM DVFS levels Starbite OS may ask for (sb.h sb_acpm_dvfs_listed; docs/STARBITE-OS.md 8.3). Pure:
 * the host test checks the list. */
#include "sb.h"

/* Starbite OS's listed levels (docs/STARBITE-OS.md 8.3): the vendor tables' levels (DT cpufreq domain@0
 * mif-perf table for the A55, the big cluster's table for the M3; 1898 / 1950 MHz A55 and every M3 level
 * above the all-core 1794 MHz left out on purpose). */
static const uint32_t lv_a55[] = { 455000, 598000, 715000, 832000, 949000, 1053000, 1248000, 1456000, 1690000, 1794000 };
static const uint32_t lv_m3[] = { 650000, 741000, 858000, 962000, 1066000, 1170000, 1261000, 1469000, 1586000,
                                  1690000, 1794000 };

int sb_dvfs_level_listed(uint32_t domain, uint32_t khz)
{
    const uint32_t *t = domain == 2 ? lv_a55 : domain == 3 ? lv_m3 : 0;
    unsigned n = domain == 2 ? sizeof(lv_a55) / sizeof(lv_a55[0]) : sizeof(lv_m3) / sizeof(lv_m3[0]);
    if (!t)
        return 0;
    for (unsigned i = 0; i < n; ++i)
        if (t[i] == khz)
            return 1;
    return 0;
}

