/* SPDX-License-Identifier: GPL-2.0-only
 * Engineer OS register policy (docs/ENGINEER-OS.md section 8): which addresses Register peek may
 * read and which tier a poke would need. Pure (no hardware access, no UI): starbite/tools/unit-test.c
 * checks it on the host, so the exclusions (CMU, MIF/INT, PMU, ACPM, fuses, DRAM) cannot regress.
 */
#include "eng.h"

/* ---- register policy ---------------------------------------------------------------------------
 * Reads: the blocks Engineer Lite's peek knows to answer (em.c peek_safe) and DRAM outside the
 * secure carve-outs, if libsb maps them. Pokes: never inside the compiled-in exclusions (every CMU
 * block, the ACPM/APM mailbox and SRAM, DREX/DMC and the MIF region, the whole PMU with its DVFS
 * registers, CHIPID / fuses, the TZPC and secure blocks, DRAM), tier 1 only in the allowlisted
 * peripheral blocks. */

struct range {
    uint64_t a, n;
    const char *why;
};

static const struct range never[] = {
    { 0x02000000ULL, 0x00100000ULL, "APM SRAM (ACPM)" },
    { 0x10000000ULL, 0x00010000ULL, "CHIPID / fuses" },
    { 0x10020000ULL, 0x00010000ULL, "CMU_PERIS" },
    { 0x10400000ULL, 0x00010000ULL, "CMU_PERIC0" },
    { 0x10800000ULL, 0x00010000ULL, "CMU_PERIC1" },
    { 0x11000000ULL, 0x00010000ULL, "CMU_FSYS0" },
    { 0x11400000ULL, 0x00010000ULL, "CMU_FSYS1" },
    { 0x14000000ULL, 0x00010000ULL, "CMU_APM" },
    { 0x14060000ULL, 0x00010000ULL, "PMU (DVFS, reset, power)" },
    { 0x14100000ULL, 0x00010000ULL, "ACPM/APM mailbox" },
    { 0x14200000ULL, 0x00010000ULL, "CMU_CMGP" },
    { 0x16000000ULL, 0x00010000ULL, "CMU_DPU" },
    { 0x17400000ULL, 0x00010000ULL, "CMU_G3D" },
    { 0x17c00000ULL, 0x00010000ULL, "CMU_AUD" },
    { 0x1a000000ULL, 0x00400000ULL, "CMU_TOP / CORE / BUSC (MIF and INT muxes)" },
    { 0x1b800000ULL, 0x00800000ULL, "MIF: CMU_MIF0-3, DREX / DMC, PHY" },
    { 0x1d000000ULL, 0x00010000ULL, "CMU_CPUCL0" },
    { 0x1d100000ULL, 0x00010000ULL, "CMU_CPUCL1" },
    { 0x80000000ULL, 0x900000000ULL - 0x80000000ULL, "DRAM (both banks)" },
};

static const struct range poke_ok[] = {
    { 0x10430000ULL, 0x1000ULL, "GPIO_PERIC0 (pins)" },
    { 0x104b0000ULL, 0x1000ULL, "HSI2C USI03 (touch bus)" },
    { 0x11430000ULL, 0x1000ULL, "GPIO_FSYS1 (card pins)" },
    { 0x14220000ULL, 0x1000ULL, "GPIO_CMGP (pins)" },
    { 0x14360000ULL, 0x1000ULL, "HSI2C USI_CMGP03 (MAX77705 bus)" },
};

int poke_tier(uint64_t a, const char **why)
{
    for (unsigned i = 0; i < sizeof(never) / sizeof(never[0]); ++i)
        if (a >= never[i].a && a - never[i].a < never[i].n) {
            *why = never[i].why;
            return TIER_NEVER;
        }
    for (unsigned i = 0; i < sizeof(poke_ok) / sizeof(poke_ok[0]); ++i)
        if (a >= poke_ok[i].a && a - poke_ok[i].a < poke_ok[i].n) {
            *why = poke_ok[i].why;
            return TIER1;
        }
    *why = "not on the poke allowlist";
    return TIER_NEVER;
}

int peek_allowed(uint64_t a)
{
    if (!sb_map_kind(a))
        return 0;
    if (a >= 0x80000000ULL && a < 0x100000000ULL)
        return !(a >= 0xaf800000ULL && a < 0xc0080000ULL) &&      /* RKP, TIMA, secure world, seclog */
               !(a >= 0xe1900000ULL && a < 0xe3700000ULL) &&      /* secure camera */
               !(a >= 0xf0000000ULL && a < 0xf9200000ULL);        /* modem */
    if (a >= 0x880000000ULL && a < 0x900000000ULL)
        return 1;
    switch (a & ~0xffffULL) {
    case 0x10000000: case 0x10050000: case 0x10070000:
    case 0x10400000: case 0x10410000: case 0x10430000: case 0x104b0000:
    case 0x11120000: case 0x11430000: case 0x11500000:
    case 0x14050000: case 0x14060000:
    case 0x14200000: case 0x14210000: case 0x14220000: case 0x14360000:
    case 0x16030000: case 0x16080000:
    case 0x1a240000: case 0x1b800000: case 0x1d000000:
        return 1;
    default:
        return 0;
    }
}

