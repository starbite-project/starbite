/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: the ECT clock table the AP writes for the CP right before INIT_END (stage 5; Samsung's
 * write_clk_table_to_shmem, in link_device_shmem.c, runs when RIL opens the FMT / RFS channel). Layout (struct
 * clock_table, link_device_memory.h), at the legacy map's reserved area (IPC region + 40):
 *   char parser_version[4] "CT0", u32 total_table_count 4, { char name[4]; u32 count; } x 4 (of 8 slots),
 *   then the frequencies (kHz, u32) from &table_info[4] on: CL0, CL1, MIF, INT.
 * The values are this phone's, as its kernel logged them (dmesg 2026-11-08 091233, 20.6310): Android reads them
 * from the ECT S-Boot leaves in RAM.
 */
#include "sbm.h"

static const uint32_t cl0[] = { 208000, 299000, 455000, 598000, 715000, 832000, 949000, 1053000, 1248000, 1456000,
                                1690000, 1794000, 1950000, 2002000 };
static const uint32_t cl1[] = { 598000, 650000, 741000, 858000, 962000, 1066000, 1170000, 1261000, 1469000,
                                1586000, 1690000, 1794000, 1924000, 2002000, 2106000, 2314000, 2496000, 2652000,
                                2704000, 2860000, 2886000 };
static const uint32_t mif[] = { 421000, 546000, 676000, 845000, 1014000, 1352000, 1539000, 1794000, 2002000,
                                2093000, 2094000 };
static const uint32_t intf[] = { 107000, 178000, 267000, 336000, 400000, 534000 };

static const struct { const char name[4]; const uint32_t *f; uint32_t n; } tabs[4] = {
    { "CL0", cl0, sizeof(cl0) / 4 }, { "CL1", cl1, sizeof(cl1) / 4 },
    { "MIF", mif, sizeof(mif) / 4 }, { "INT", intf, sizeof(intf) / 4 },
};

uint32_t sbm_clk_table_size(void)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i)
        n += tabs[i].n;
    return 8 + 4 * 8 + 4 * n;
}

int sbm_clk_table_write(uint8_t *ipc, uint32_t ipc_size)
{
    if (!ipc || ipc_size < SBM_CLK_TABLE_OFF + sbm_clk_table_size() ||
        SBM_CLK_TABLE_OFF + sbm_clk_table_size() > 4096)       /* inside the reserved area */
        return SBM_EINVAL;
    uint8_t *t = ipc + SBM_CLK_TABLE_OFF;
    sbm_memset(t, 0, 8 + 8 * 8);
    t[0] = 'C'; t[1] = 'T'; t[2] = '0'; t[3] = 0;
    sbm_put32(t + 4, 4);
    uint8_t *d = t + 8 + 4 * 8;                                /* &table_info[total_table_count] */
    for (unsigned i = 0; i < 4; ++i) {
        sbm_memcpy(t + 8 + 8 * i, tabs[i].name, 4);
        sbm_put32(t + 8 + 8 * i + 4, tabs[i].n);
        for (uint32_t k = 0; k < tabs[i].n; ++k, d += 4)
            sbm_put32(d, tabs[i].f[k]);
    }
    return (int)sbm_clk_table_size();
}
