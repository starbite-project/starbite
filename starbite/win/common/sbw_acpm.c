/* SPDX-License-Identifier: GPL-2.0-only
 * ACPM IPC, see sbw_acpm.h. Moved here from sbm_core.c unchanged in behaviour.
 */
#include "sbw_acpm.h"

#define ACPM_INITDATA 0x7f00U

static sbw_u32 ard(const struct sbw_acpm_io *io, sbw_u32 off)
{
    return off + 4U <= SBW_ACPM_SRAM_SIZE ? io->sram_rd(io->ctx, off) : 0;
}

static void awr(const struct sbw_acpm_io *io, sbw_u32 off, sbw_u32 v)
{
    if (off + 4U <= SBW_ACPM_SRAM_SIZE)
        io->sram_wr(io->ctx, off, v);
}

static void udelay(const struct sbw_acpm_io *io, unsigned us)
{
    if (io->delay_us)
        io->delay_us(io->ctx, us);
}

static void barrier(const struct sbw_acpm_io *io)
{
    if (io->barrier)
        io->barrier(io->ctx);
}

sbw_u32 sbw_acpm_chan(const struct sbw_acpm_io *io, sbw_u32 id)
{
    sbw_u32 chans = ard(io, ACPM_INITDATA + 0x08U), ap_max = ard(io, ACPM_INITDATA + 0x18U);
    if (!ap_max || ap_max > 32U || chans >= SBW_ACPM_SRAM_SIZE - ap_max * 0x48U)
        return 0;
    for (sbw_u32 i = 0; i < ap_max; ++i) {
        sbw_u32 c = chans + i * 0x48U;
        if (id == SBW_ACPM_DVFS_CH || ard(io, c) == id) {
            sbw_u32 q = ard(io, c + 0x34U), e = ard(io, c + 0x38U);
            return (q && q <= 64U && e >= 16U && e <= 64U && !(e & 3U) && ard(io, c) <= 31U) ? c : 0;
        }
    }
    return 0;
}

int sbw_acpm_send(const struct sbw_acpm_io *io, sbw_u32 c, sbw_u32 cmd[4])
{
    static sbw_u32 seq_counter;
    sbw_u32 id = ard(io, c), qlen = ard(io, c + 0x34U), esz = ard(io, c + 0x38U);
    sbw_u32 tx_rear = ard(io, c + 0x10U), tx_front = ard(io, c + 0x14U), tx_base = ard(io, c + 0x18U);
    sbw_u32 rx_rear = ard(io, c + 0x28U), rx_front = ard(io, c + 0x2cU), rx_base = ard(io, c + 0x30U);
    sbw_u32 front, next, seq, slot;
    if (!c || !qlen || id > 31U)
        return 0;
    front = ard(io, tx_front);
    next = (front + 1U) % qlen;
    for (unsigned us = 0; next == ard(io, tx_rear); ++us) {
        if (us > 100000U)
            return 0;
        udelay(io, 1);
    }
    if (++seq_counter >= 64U)
        seq_counter = 1;
    seq = seq_counter;
    cmd[0] = (cmd[0] & ~(0x3fU << 16)) | (seq << 16);
    slot = tx_base + esz * front;
    for (sbw_u32 i = 0; i < esz / 4U; ++i)
        awr(io, slot + 4U * i, i < 4U ? cmd[i] : 0);
    barrier(io);
    awr(io, tx_front, next);
    barrier(io);
    io->mbox_wr(io->ctx, 0x08U, (1U << id) << 16);           /* INTGR0 */
    for (unsigned us = 0; us < 100000U; ++us) {
        sbw_u32 rear = ard(io, rx_rear), rfront = ard(io, rx_front);
        for (sbw_u32 i = rear; i != rfront; i = (i + 1U) % qlen) {
            sbw_u32 e = rx_base + esz * i;
            if (((ard(io, e) >> 16) & 0x3fU) != seq)
                continue;
            for (sbw_u32 w = 0; w < 4U; ++w)
                cmd[w] = ard(io, e + 4U * w);
            if (i != rear)
                for (sbw_u32 w = 0; w < esz / 4U; ++w)
                    awr(io, e + 4U * w, ard(io, rx_base + esz * rear + 4U * w));
            rear = (rear + 1U) % qlen;
            awr(io, rx_rear, rear);
            barrier(io);
            if (rear == ard(io, rx_front))
                io->mbox_wr(io->ctx, 0x20U, 1U << id);        /* INTCR1 */
            return 1;
        }
        udelay(io, 1);
    }
    return 0;
}

int sbw_acpm_dvfs(const struct sbw_acpm_io *io, sbw_u32 domain, sbw_u32 khz)
{
    sbw_u32 c, cmd[4];
    /* Domain ids are the indices of enum acpm_dvfs_id (Samsung kernel drivers/soc/samsung/cal-if/exynos9810/
     * acpm_dvfs_exynos9810.h; cal-if.c cal_dfs_set_rate() sends GET_IDX(id)): 0 dvfs_mif, 1 dvfs_int,
     * 2 dvfs_cpucl0 (A55), 3 dvfs_cpucl1 (M3), 4 dvfs_g3d (GPU). MIF and INT, and everything else, are never
     * sent: the only domains this code will ever request are 2, 3 and 4. */
    if (domain != 2U && domain != 3U && domain != 4U)
        return 0;
    c = sbw_acpm_chan(io, SBW_ACPM_DVFS_CH);
    cmd[0] = domain;
    cmd[1] = khz;
    cmd[2] = cmd[3] = 0;
    return c && sbw_acpm_send(io, c, cmd);
}
