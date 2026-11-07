/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Mali core (see sbm_core.h). Ported from xnu/src/starlte_gpu.c, starbite/libsb/mali.c,
 * mali_desc.c and acpm.c; AArch64 table bits and MEMATTR as Linux panfrost_mmu.c / io-pgtable-arm.c.
 */
#include "sbm_core.h"

/* GPU registers (panfrost_regs.h) */
#define GPU_ID 0x000U
#define L2_FEATURES 0x004U
#define CORE_FEATURES 0x008U
#define TILER_FEATURES 0x00cU
#define MEM_FEATURES 0x010U
#define MMU_FEATURES 0x014U
#define AS_PRESENT 0x018U
#define JS_PRESENT 0x01cU
#define GPU_IRQ_RAWSTAT 0x020U
#define GPU_IRQ_CLEAR 0x024U
#define GPU_IRQ_MASK 0x028U
#define GPU_COMMAND 0x030U
#define GPU_FAULTSTATUS 0x03cU
#define GPU_FAULTADDR_LO 0x040U
#define SHADER_PRESENT_LO 0x100U
#define TILER_PRESENT_LO 0x110U
#define L2_PRESENT_LO 0x120U
#define SHADER_READY_LO 0x140U
#define TILER_READY_LO 0x150U
#define L2_READY_LO 0x160U
#define SHADER_PWRON_LO 0x180U
#define TILER_PWRON_LO 0x190U
#define L2_PWRON_LO 0x1a0U
#define SHADER_PWROFF_LO 0x1c0U
#define TILER_PWROFF_LO 0x1d0U
#define L2_PWROFF_LO 0x1e0U
#define COHERENCY_FEATURES 0x300U
#define JOB_IRQ_RAWSTAT 0x1000U
#define JOB_IRQ_CLEAR 0x1004U
#define JOB_IRQ_MASK 0x1008U
#define JS(n) (0x1800U + (n) * 0x80U)
#define MMU_IRQ_RAWSTAT 0x2000U
#define MMU_IRQ_CLEAR 0x2004U
#define MMU_IRQ_MASK 0x2008U
#define AS(n) (0x2400U + (n) * 0x40U)
#define AS_TRANSTAB 0x00U
#define AS_MEMATTR 0x08U
#define AS_LOCKADDR 0x10U
#define AS_COMMAND 0x18U
#define AS_FAULTSTATUS 0x1cU
#define AS_FAULTADDR 0x20U
#define AS_STATUS 0x28U
#define AS_TRANSCFG 0x30U
#define AS_CMD_UPDATE 1U
#define AS_CMD_LOCK 2U
#define AS_CMD_FLUSH_PT 4U
#define AS_CMD_FLUSH_MEM 5U

static sbm_u32 rd(const struct sbm_io *io, unsigned b, unsigned off)
{
    return io->rd(io->ctx, b, off);
}

static void wr(const struct sbm_io *io, unsigned b, unsigned off, sbm_u32 v)
{
    io->wr(io->ctx, b, off, v);
}

static void udelay(const struct sbm_io *io, unsigned us)
{
    if (io->delay_us)
        io->delay_us(io->ctx, us);
}

static void barrier(const struct sbm_io *io)
{
    if (io->barrier)
        io->barrier(io->ctx);
}

static sbm_u64 rd64(const struct sbm_io *io, unsigned off)
{
    return (sbm_u64)rd(io, SBM_GPU, off + 4U) << 32 | rd(io, SBM_GPU, off);
}

/* ---- probe ------------------------------------------------------------------------------------ */

int sbm_domain_on(const struct sbm_io *io)
{
    return (rd(io, SBM_PMU, 0x44U) & 0xfU) == 0xfU;
}

int sbm_probe(const struct sbm_io *io, struct sbm_info *inf)
{
    sbm_u32 con0, m, p, s;
    inf->pmu_status = rd(io, SBM_PMU, 0x44U);
    if ((inf->pmu_status & 0xfU) != 0xfU)
        return -1;                        /* a read into the powered-off block would hang the bus */
    inf->gpu_id = rd(io, SBM_GPU, GPU_ID);
    inf->l2_features = rd(io, SBM_GPU, L2_FEATURES);
    inf->core_features = rd(io, SBM_GPU, CORE_FEATURES);
    inf->tiler_features = rd(io, SBM_GPU, TILER_FEATURES);
    inf->mem_features = rd(io, SBM_GPU, MEM_FEATURES);
    inf->mmu_features = rd(io, SBM_GPU, MMU_FEATURES);
    inf->as_present = rd(io, SBM_GPU, AS_PRESENT);
    inf->js_present = rd(io, SBM_GPU, JS_PRESENT);
    inf->coherency = rd(io, SBM_GPU, COHERENCY_FEATURES);
    inf->shader_present = rd64(io, SHADER_PRESENT_LO);
    inf->tiler_present = rd64(io, TILER_PRESENT_LO);
    inf->l2_present = rd64(io, L2_PRESENT_LO);
    con0 = rd(io, SBM_CMU, 0x140U);
    inf->pll_con0 = con0;
    m = (con0 >> 16) & 0x3ffU;
    p = (con0 >> 8) & 0x3fU;
    s = con0 & 7U;
    inf->pll_khz = (con0 & 0x10U) && p ? (sbm_u32)(26000ULL * m / ((sbm_u64)p << s)) : 26000U;
    return (inf->gpu_id >> 16) == SBM_G72_PRODUCT ? 0 : -2;
}

const char *sbm_exc_name(sbm_u32 code)
{
    switch (code & 0xffU) {
    case 0x00: return "not started";
    case 0x01: return "DONE";
    case 0x02: return "INTERRUPTED";
    case 0x03: return "STOPPED";
    case 0x04: return "TERMINATED";
    case 0x08: return "ACTIVE";
    case 0x40: return "JOB_CONFIG_FAULT";
    case 0x41: return "JOB_POWER_FAULT";
    case 0x42: return "JOB_READ_FAULT";
    case 0x43: return "JOB_WRITE_FAULT";
    case 0x44: return "JOB_AFFINITY_FAULT";
    case 0x48: return "JOB_BUS_FAULT";
    case 0x50: return "INSTR_INVALID_PC";
    case 0x51: return "INSTR_INVALID_ENC";
    case 0x58: return "DATA_INVALID_FAULT";
    case 0x59: return "TILE_RANGE_FAULT";
    case 0x5a: return "ADDR_RANGE_FAULT";
    case 0x60: return "OUT_OF_MEMORY";
    case 0x80: return "DELAYED_BUS_FAULT";
    case 0x88: return "SHAREABILITY_FAULT";
    case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: return "TRANSLATION_FAULT";
    case 0xc8: case 0xc9: case 0xca: case 0xcb: return "PERMISSION_FAULT";
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: return "TRANSTAB_BUS_FAULT";
    case 0xd8: case 0xd9: case 0xda: case 0xdb: return "ACCESS_FLAG_FAULT";
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: return "ADDRESS_SIZE_FAULT";
    case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xec: return "MEMORY_ATTRIBUTES_FAULT";
    default: return "unknown";
    }
}

/* ---- ACPM ------------------------------------------------------------------------------------- */

#define ACPM_INITDATA 0x7f00U
#define ACPM_PMIC_CH 2U

static sbm_u32 ard(const struct sbm_io *io, sbm_u32 off)
{
    return off + 4U <= SBM_ACPM_SRAM_SIZE ? rd(io, SBM_ACPM_SRAM, off) : 0;
}

static void awr(const struct sbm_io *io, sbm_u32 off, sbm_u32 v)
{
    if (off + 4U <= SBM_ACPM_SRAM_SIZE)
        wr(io, SBM_ACPM_SRAM, off, v);
}

static sbm_u32 acpm_seq;

/* The channel entry whose id is `id`; with id = ~0 the first one (the DVFS channel). 0 = none. */
static sbm_u32 acpm_chan(const struct sbm_io *io, sbm_u32 id)
{
    sbm_u32 chans = ard(io, ACPM_INITDATA + 0x08U), ap_max = ard(io, ACPM_INITDATA + 0x18U);
    if (!ap_max || ap_max > 32U || chans >= SBM_ACPM_SRAM_SIZE - ap_max * 0x48U)
        return 0;
    for (sbm_u32 i = 0; i < ap_max; ++i) {
        sbm_u32 c = chans + i * 0x48U;
        if (id == ~0U || ard(io, c) == id) {
            sbm_u32 q = ard(io, c + 0x34U), e = ard(io, c + 0x38U);
            return (q && q <= 64U && e >= 16U && e <= 64U && !(e & 3U) && ard(io, c) <= 31U) ? c : 0;
        }
    }
    return 0;
}

static int acpm_send(const struct sbm_io *io, sbm_u32 c, sbm_u32 cmd[4])
{
    sbm_u32 id = ard(io, c), qlen = ard(io, c + 0x34U), esz = ard(io, c + 0x38U);
    sbm_u32 tx_rear = ard(io, c + 0x10U), tx_front = ard(io, c + 0x14U), tx_base = ard(io, c + 0x18U);
    sbm_u32 rx_rear = ard(io, c + 0x28U), rx_front = ard(io, c + 0x2cU), rx_base = ard(io, c + 0x30U);
    sbm_u32 front = ard(io, tx_front), next = (front + 1U) % qlen, seq, slot;
    for (unsigned us = 0; next == ard(io, tx_rear); ++us) {
        if (us > 100000U)
            return 0;
        udelay(io, 1);
    }
    if (++acpm_seq >= 64U)
        acpm_seq = 1;
    seq = acpm_seq;
    cmd[0] = (cmd[0] & ~(0x3fU << 16)) | (seq << 16);
    slot = tx_base + esz * front;
    for (sbm_u32 i = 0; i < esz / 4U; ++i)
        awr(io, slot + 4U * i, i < 4U ? cmd[i] : 0);
    barrier(io);
    awr(io, tx_front, next);
    barrier(io);
    wr(io, SBM_ACPM_MBOX, 0x08U, (1U << id) << 16);          /* INTGR0 */
    for (unsigned us = 0; us < 100000U; ++us) {
        sbm_u32 rear = ard(io, rx_rear), rfront = ard(io, rx_front);
        for (sbm_u32 i = rear; i != rfront; i = (i + 1U) % qlen) {
            sbm_u32 e = rx_base + esz * i;
            if (((ard(io, e) >> 16) & 0x3fU) != seq)
                continue;
            for (sbm_u32 w = 0; w < 4U; ++w)
                cmd[w] = ard(io, e + 4U * w);
            if (i != rear)
                for (sbm_u32 w = 0; w < esz / 4U; ++w)
                    awr(io, e + 4U * w, ard(io, rx_base + esz * rear + 4U * w));
            rear = (rear + 1U) % qlen;
            awr(io, rx_rear, rear);
            barrier(io);
            if (rear == ard(io, rx_front))
                wr(io, SBM_ACPM_MBOX, 0x20U, 1U << id);       /* INTCR1 */
            return 1;
        }
        udelay(io, 1);
    }
    return 0;
}

int sbm_pmic_read(const struct sbm_io *io, sbm_u32 type, sbm_u32 reg, sbm_u8 *val)
{
    sbm_u32 c = acpm_chan(io, ACPM_PMIC_CH), cmd[4];
    cmd[0] = (type & 0xfU) << 8 | (reg & 0xffU);
    cmd[1] = cmd[2] = cmd[3] = 0;
    if (!c || !acpm_send(io, c, cmd) || (cmd[1] >> 24))
        return 0;
    *val = (sbm_u8)(cmd[1] >> 8);
    return 1;
}

/* The only PMIC register this driver writes: BUCK6 (vdd_g3d). */
static int buck6_write(const struct sbm_io *io, sbm_u8 val)
{
    sbm_u32 c = acpm_chan(io, ACPM_PMIC_CH), cmd[4];
    if (val < SBM_BUCK6_LO || val > SBM_BUCK6_HI)
        return 0;
    cmd[0] = 1U << 8 | SBM_BUCK6_REG;
    cmd[1] = 1U | (sbm_u32)val << 8;
    cmd[2] = cmd[3] = 0;
    return c && acpm_send(io, c, cmd) && !(cmd[1] >> 24);
}

int sbm_buck6_raise(const struct sbm_io *io, sbm_u8 *old)
{
    sbm_u8 v = 0, back = 0;
    if (!sbm_pmic_read(io, 1, SBM_BUCK6_REG, &v))
        return -1;
    *old = v;
    if (v < SBM_BUCK6_LO || v > SBM_BUCK6_HI)
        return -2;
    if (v != SBM_BUCK6_950MV && !buck6_write(io, SBM_BUCK6_950MV))
        return -1;
    if (!sbm_pmic_read(io, 1, SBM_BUCK6_REG, &back))
        return -1;
    return back == SBM_BUCK6_950MV ? 0 : -3;
}

int sbm_buck6_restore(const struct sbm_io *io, sbm_u8 old)
{
    sbm_u8 back = 0;
    if (old < SBM_BUCK6_LO || old > SBM_BUCK6_HI)
        return -2;
    if (!buck6_write(io, old) || !sbm_pmic_read(io, 1, SBM_BUCK6_REG, &back))
        return -1;
    return back == old ? 0 : -3;
}

int sbm_gpu_dvfs(const struct sbm_io *io, sbm_u32 khz)
{
    static const sbm_u32 levels[] = { 572000, 546000, 455000, 338000, 299000, 260000 };
    sbm_u32 c = acpm_chan(io, ~0U), cmd[4];
    sbm_u8 old = 0;
    int ok = 0;
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i)
        ok |= khz == levels[i];
    if (!ok || !c)
        return -1;
    cmd[0] = SBM_DVFS_G3D;                /* domain: the GPU's, never another */
    cmd[1] = khz;
    cmd[2] = cmd[3] = 0;
    if (!acpm_send(io, c, cmd))
        return -1;
    return sbm_buck6_raise(io, &old);     /* ACPM set its own table voltage: back to 950 mV */
}

/* ---- reset and power ------------------------------------------------------------------------- */

int sbm_soft_reset(const struct sbm_io *io, unsigned *us)
{
    unsigned t = 0;
    sbm_u32 raw = 0;
    wr(io, SBM_GPU, GPU_IRQ_CLEAR, 0xffffffffU);
    wr(io, SBM_GPU, GPU_COMMAND, 1U);   /* SOFT_RESET */
    for (; t < 10000U; t += 5U) {
        raw = rd(io, SBM_GPU, GPU_IRQ_RAWSTAT);
        if (raw & 0x100U)                 /* RESET_COMPLETED */
            break;
        udelay(io, 5);
    }
    wr(io, SBM_GPU, GPU_IRQ_CLEAR, 0xffffffffU);
    wr(io, SBM_GPU, GPU_IRQ_MASK, 0);   /* polled: no interrupt line in this stage */
    wr(io, SBM_GPU, JOB_IRQ_MASK, 0);
    wr(io, SBM_GPU, MMU_IRQ_MASK, 0);
    if (us)
        *us = t;
    return (raw & 0x100U) != 0;
}

static int power(const struct sbm_io *io, unsigned on, unsigned ready, sbm_u32 mask, unsigned *us)
{
    unsigned t = 0;
    sbm_u32 r = 0;
    wr(io, SBM_GPU, on, mask);
    for (; t < 20000U; t += 5U) {
        r = rd(io, SBM_GPU, ready);
        if ((r & mask) == mask)
            break;
        udelay(io, 5);
    }
    if (us)
        *us = t;
    return (r & mask) == mask;
}

int sbm_power_on(const struct sbm_io *io, sbm_u64 shaders, unsigned us[3])
{
    unsigned d[3] = { 0, 0, 0 };
    int ok = power(io, L2_PWRON_LO, L2_READY_LO, rd(io, SBM_GPU, L2_PRESENT_LO), &d[0]) &&
             power(io, SHADER_PWRON_LO, SHADER_READY_LO, (sbm_u32)shaders, &d[1]) &&
             power(io, TILER_PWRON_LO, TILER_READY_LO, rd(io, SBM_GPU, TILER_PRESENT_LO), &d[2]);
    if (us)
        for (unsigned i = 0; i < 3; ++i)
            us[i] = d[i];
    return ok;
}

void sbm_power_off(const struct sbm_io *io)
{
    wr(io, SBM_GPU, TILER_PWROFF_LO, rd(io, SBM_GPU, TILER_READY_LO));
    wr(io, SBM_GPU, SHADER_PWROFF_LO, rd(io, SBM_GPU, SHADER_READY_LO));
    udelay(io, 100);
    wr(io, SBM_GPU, L2_PWROFF_LO, rd(io, SBM_GPU, L2_READY_LO));
    udelay(io, 100);
}

/* ---- page tables ----------------------------------------------------------------------------- */

#define PTE_TABLE 3ULL
/* Mali LPAE leaf: ATE (1), read + write (bits 6-7), outer shareable (bits 8-9 = 2), MEMATTR index 0.
 * Exactly XNU's and libsb's 0x2c1. */
#define LEGACY_LEAF 0x2c1ULL
/* AArch64 stage-1 leaf as io-pgtable-arm builds it for Panfrost (IOMMU_READ | WRITE | CACHE):
 * page (3), AttrIndx 1 = MAIR write-back (bits 2-4), AP_UNPRIV (bit 6), SH_IS (bits 8-9 = 3),
 * AF (bit 10), nG (bit 11); XN (bits 53-54) for IOMMU_NOEXEC. */
#define A64_LEAF (3ULL | 1ULL << 2 | 1ULL << 6 | 3ULL << 8 | 1ULL << 10 | 1ULL << 11)
#define A64_XN (3ULL << 53)
#define PA_MASK 0x000ffffffffff000ULL

sbm_u64 sbm_pt_leaf(enum sbm_fmt fmt, sbm_u64 pa, int noexec)
{
    pa &= PA_MASK;
    if (fmt == SBM_FMT_LEGACY)
        return pa | LEGACY_LEAF;
    return pa | A64_LEAF | (noexec ? A64_XN : 0);
}

/* Linux mair_to_memattr() for io-pgtable's MAIR (NC, WBRWA, DEVICE, INC_OWBRWA), non-coherent GPU. */
sbm_u64 sbm_aarch64_memattr(void)
{
    static const sbm_u8 mair[8] = { 0x44, 0xff, 0x04, 0xf4, 0, 0, 0, 0 };
    sbm_u64 memattr = 0;
    for (unsigned i = 0; i < 8; ++i) {
        sbm_u8 outer = mair[i] >> 4, inner = mair[i] & 0xfU, a;
        if (!(outer & 3U) || !(outer & 4U) || !(inner & 4U))
            a = (sbm_u8)(1U << 6 | 0U << 4 | 3U << 2);                  /* INNER_OUTER_NC, SH_MIDGARD_INNER, ALLOC_EXPL(0,0) */
        else
            a = (sbm_u8)(2U << 6 | 3U << 2 | (inner & 1U) | (inner & 2U)); /* INNER_OUTER_WB, ALLOC_EXPL(w,r), SH_MIDGARD_INNER */
        memattr |= (sbm_u64)a << (8U * i);
    }
    return memattr;
}

void sbm_pt_init(struct sbm_pt *pt, enum sbm_fmt fmt, volatile sbm_u64 *cpu, sbm_u64 pa, unsigned tables)
{
    pt->fmt = fmt;
    pt->cpu = cpu;
    pt->pa = pa;
    pt->tables = tables;
    pt->used = tables ? 1U : 0U;
    for (unsigned i = 0; i < tables * 512U; ++i)
        cpu[i] = 0;
}

/* The table that entry `e` of table `t` points to, created if missing; -1 when out of tables. */
static int next_table(struct sbm_pt *pt, unsigned t, unsigned e)
{
    volatile sbm_u64 *ent = pt->cpu + t * 512U + e;
    sbm_u64 v = *ent;
    if ((v & 3ULL) == PTE_TABLE) {
        sbm_u64 a = (v & PA_MASK) - pt->pa;
        return a < (sbm_u64)pt->tables * 0x1000U ? (int)(a / 0x1000U) : -1;
    }
    if (v || pt->used >= pt->tables)
        return -1;
    *ent = (pt->pa + (sbm_u64)pt->used * 0x1000U) | PTE_TABLE;
    return (int)pt->used++;
}

int sbm_pt_map(struct sbm_pt *pt, sbm_u64 va, sbm_u64 pa, unsigned n, int noexec)
{
    if ((va & 0xfffU) || (pa & 0xfffU) || va + (sbm_u64)n * 0x1000U > (1ULL << 48))
        return -1;
    for (unsigned i = 0; i < n; ++i, va += 0x1000U, pa += 0x1000U) {
        int t = 0;
        for (unsigned lvl = 0; lvl < 3U && t >= 0; ++lvl)
            t = next_table(pt, (unsigned)t, (unsigned)(va >> (39U - 9U * lvl)) & 0x1ffU);
        if (t < 0)
            return -1;
        pt->cpu[(unsigned)t * 512U + ((va >> 12) & 0x1ffU)] = sbm_pt_leaf(pt->fmt, pa, noexec);
    }
    return 0;
}

void sbm_pt_unmap(struct sbm_pt *pt, sbm_u64 va, unsigned n)
{
    for (unsigned i = 0; i < n; ++i, va += 0x1000U) {
        unsigned t = 0;
        int found = 1;
        for (unsigned lvl = 0; lvl < 3U && found; ++lvl) {
            sbm_u64 v = pt->cpu[t * 512U + ((va >> (39U - 9U * lvl)) & 0x1ffU)];
            found = (v & 3ULL) == PTE_TABLE && (v & PA_MASK) - pt->pa < (sbm_u64)pt->tables * 0x1000U;
            if (found)
                t = (unsigned)(((v & PA_MASK) - pt->pa) / 0x1000U);
        }
        if (found)
            pt->cpu[t * 512U + ((va >> 12) & 0x1ffU)] = 0;
    }
}

/* ---- address spaces ------------------------------------------------------------------------- */

static int as_wait(const struct sbm_io *io, unsigned as)
{
    for (unsigned i = 0; i < 10000U; ++i) {
        if (!(rd(io, SBM_GPU, AS(as) + AS_STATUS) & 1U))
            return 1;
        udelay(io, 1);
    }
    return 0;
}

static int as_cmd(const struct sbm_io *io, unsigned as, sbm_u32 cmd)
{
    if (!as_wait(io, as))
        return 0;
    wr(io, SBM_GPU, AS(as) + AS_COMMAND, cmd);
    return as_wait(io, as);
}

/* LOCK a naturally aligned power-of-two region covering [va, va + size) (Linux lock_region). */
static int as_lock(const struct sbm_io *io, unsigned as, sbm_u64 va, sbm_u64 size)
{
    sbm_u64 x = va ^ (va + size - 1U), region;
    unsigned width = 0;
    while (x) {
        ++width;
        x >>= 1;
    }
    if (width < 15U)
        width = 15U;                      /* AS_LOCK_REGION_MIN_SIZE 32 KiB */
    --width;
    region = (va & ~((1ULL << width) - 1U)) | width;
    if (!as_wait(io, as))
        return 0;
    wr(io, SBM_GPU, AS(as) + AS_LOCKADDR, (sbm_u32)region);
    wr(io, SBM_GPU, AS(as) + AS_LOCKADDR + 4U, (sbm_u32)(region >> 32));
    return as_cmd(io, as, AS_CMD_LOCK);
}

int sbm_as_enable(const struct sbm_io *io, unsigned as, const struct sbm_pt *pt)
{
    sbm_u64 transtab, memattr, transcfg;
    if (pt->fmt == SBM_FMT_LEGACY) {
        transtab = (pt->pa & PA_MASK) | 7U;       /* ADRMODE_TABLE | READ_INNER */
        memattr = 0x888d88U;
        transcfg = 0;                             /* ADRMODE_LEGACY */
    } else {
        transtab = pt->pa & 0xfffffffffffffff0ULL;
        memattr = sbm_aarch64_memattr();
        transcfg = 6U | (55U - 48U) << 6 | 2U << 24 | 1U << 30;  /* AARCH64_4K, INA 48 bits, PTW WB, PTW RA */
    }
    barrier(io);
    if (!as_wait(io, as))
        return -1;
    wr(io, SBM_GPU, AS(as) + AS_TRANSTAB, (sbm_u32)transtab);
    wr(io, SBM_GPU, AS(as) + AS_TRANSTAB + 4U, (sbm_u32)(transtab >> 32));
    wr(io, SBM_GPU, AS(as) + AS_MEMATTR, (sbm_u32)memattr);
    wr(io, SBM_GPU, AS(as) + AS_MEMATTR + 4U, (sbm_u32)(memattr >> 32));
    wr(io, SBM_GPU, AS(as) + AS_TRANSCFG, (sbm_u32)transcfg);
    wr(io, SBM_GPU, AS(as) + AS_TRANSCFG + 4U, (sbm_u32)(transcfg >> 32));
    return as_cmd(io, as, AS_CMD_UPDATE) ? 0 : -1;
}

int sbm_as_flush(const struct sbm_io *io, unsigned as, sbm_u64 va, sbm_u64 size)
{
    barrier(io);
    if (!size || !as_lock(io, as, va, size))
        return -1;
    return as_cmd(io, as, AS_CMD_FLUSH_PT) ? 0 : -1;
}

void sbm_as_disable(const struct sbm_io *io, unsigned as)
{
    if (as_lock(io, as, 0, ~0ULL))
        (void)as_cmd(io, as, AS_CMD_FLUSH_MEM);
    for (unsigned off = AS_TRANSTAB; off <= AS_MEMATTR + 4U; off += 4U)
        wr(io, SBM_GPU, AS(as) + off, 0);
    wr(io, SBM_GPU, AS(as) + AS_TRANSCFG, 0);
    wr(io, SBM_GPU, AS(as) + AS_TRANSCFG + 4U, 0);
    (void)as_cmd(io, as, AS_CMD_UPDATE);
}

/* ---- jobs ------------------------------------------------------------------------------------ */

int sbm_run(const struct sbm_io *io, unsigned js, unsigned as, sbm_u64 job_va, sbm_u32 affinity,
            unsigned timeout_us, struct sbm_job_result *r)
{
    unsigned t = 0;
    sbm_u32 j = 0, m = 0, g = 0, done = 1U << js, fail = 1U << (js + 16U);
    int ok;
    *r = (struct sbm_job_result){ 0 };
    r->js = js;
    if (js > 2U || as > 7U)
        return 0;
    barrier(io);
    wr(io, SBM_GPU, MMU_IRQ_CLEAR, 0xffffffffU);
    wr(io, SBM_GPU, JOB_IRQ_CLEAR, 0xffffffffU);
    wr(io, SBM_GPU, JS(js) + 0x40U, (sbm_u32)job_va);            /* HEAD_NEXT */
    wr(io, SBM_GPU, JS(js) + 0x44U, (sbm_u32)(job_va >> 32));
    wr(io, SBM_GPU, JS(js) + 0x50U, affinity);                   /* AFFINITY_NEXT */
    wr(io, SBM_GPU, JS(js) + 0x54U, 0);
    wr(io, SBM_GPU, JS(js) + 0x58U, 8U << 16 | 3U << 12 | 3U << 8 | as);   /* CONFIG_NEXT: flush both ends */
    wr(io, SBM_GPU, JS(js) + 0x60U, 1U);                         /* COMMAND_NEXT: START */
    for (;;) {
        j = rd(io, SBM_GPU, JOB_IRQ_RAWSTAT);
        m = rd(io, SBM_GPU, MMU_IRQ_RAWSTAT);
        g = rd(io, SBM_GPU, GPU_IRQ_RAWSTAT);
        if ((j & (done | fail)) || m || (g & 0x81U) || t >= timeout_us)
            break;
        udelay(io, 10);
        t += 10U;
    }
    ok = (j & done) && !(j & fail) && !m && !(g & 0x81U);
    r->ok = ok;
    r->timeout = !(j & (done | fail)) && !m && !(g & 0x81U);
    r->us = t;
    r->jirq = j;
    r->mirq = m;
    r->girq = g;
    r->js_status = rd(io, SBM_GPU, JS(js) + 0x24U);
    r->as_status = rd(io, SBM_GPU, AS(as) + AS_STATUS);
    r->as_fault = rd(io, SBM_GPU, AS(as) + AS_FAULTSTATUS);
    r->as_addr = rd64(io, AS(as) + AS_FAULTADDR);
    r->gpu_fault = rd(io, SBM_GPU, GPU_FAULTSTATUS);
    r->gpu_addr = rd64(io, GPU_FAULTADDR_LO);
    if (!ok)
        wr(io, SBM_GPU, JS(js) + 0x20U, 3U);                     /* JS_COMMAND: HARD_STOP */
    wr(io, SBM_GPU, JOB_IRQ_CLEAR, 0xffffffffU);
    wr(io, SBM_GPU, MMU_IRQ_CLEAR, 0xffffffffU);
    wr(io, SBM_GPU, GPU_IRQ_CLEAR, 0x81U);
    return ok;
}

/* ---- job descriptors (starbite/libsb/mali_desc.c) -------------------------------------------- */

static void job_header(sbm_u32 *w, unsigned type, unsigned index, sbm_u64 next)
{
    for (unsigned i = 0; i < 8; ++i)
        w[i] = 0;
    w[4] = 1U | (type & 0x7fU) << 1 | (index & 0xffffU) << 16;
    w[6] = (sbm_u32)next;
    w[7] = (sbm_u32)(next >> 32);
}

void sbm_write_value(sbm_u32 w[16], unsigned index, sbm_u64 next, sbm_u64 addr, unsigned type, sbm_u64 imm)
{
    job_header(w, SBM_JOB_WRITE_VALUE, index, next);
    w[8] = (sbm_u32)addr;
    w[9] = (sbm_u32)(addr >> 32);
    w[10] = type;
    w[11] = 0;
    w[12] = (sbm_u32)imm;
    w[13] = (sbm_u32)(imm >> 32);
    w[14] = w[15] = 0;
}

void sbm_fragment(sbm_u32 w[16], unsigned index, sbm_u64 next, unsigned wpx, unsigned hpx, sbm_u64 fbd)
{
    job_header(w, SBM_JOB_FRAGMENT, index, next);
    w[8] = 0;
    w[9] = ((wpx - 1U) >> 4) | ((hpx - 1U) >> 4) << 16;
    w[10] = (sbm_u32)(fbd | 1U);          /* MFBD, one render target */
    w[11] = (sbm_u32)(fbd >> 32);
    for (unsigned i = 12; i < 16; ++i)
        w[i] = 0;
}

void sbm_fbd(sbm_u32 w[48], sbm_u64 samples, unsigned wpx, unsigned hpx, sbm_u64 rt_base, sbm_u32 stride, sbm_u32 clear)
{
    for (unsigned i = 0; i < 48; ++i)
        w[i] = 0;
    w[4] = (sbm_u32)samples;
    w[5] = (sbm_u32)(samples >> 32);
    w[8] = (wpx - 1U) | (hpx - 1U) << 16;
    w[9] = 0;
    w[10] = (wpx - 1U) | (hpx - 1U) << 16;
    w[11] = 2U << 6 | 8U << 9 | 1U << 24;
    w[12] = 1U << 16;
    w[32] = 1U << 26;
    w[33] = 1U | 19U << 3 | 2U << 10 | 1U << 15 | 0x688U << 16 | 1U << 31;
    w[40] = (sbm_u32)rt_base;
    w[41] = (sbm_u32)(rt_base >> 32);
    w[42] = stride;
    w[44] = clear;
}

void sbm_samples(sbm_u32 w[33])
{
    for (unsigned i = 0; i < 33; ++i)
        w[i] = 0;
    w[0] = 128U | 128U << 16;
    w[32] = 128U | 128U << 16;
}

/* ---- the T5 self-test ------------------------------------------------------------------------ */

/* Data layout (offsets from the data page, GPU VA SBM_TEST_VA + offset). */
#define D_WV 0x000U
#define D_OUT 0x040U
#define D_FRAG 0x100U
#define D_FBD 0x200U
#define D_SAMPLES 0x400U
#define D_PIX 0x1000U

static void put(volatile sbm_u8 *d, unsigned off, const sbm_u32 *w, unsigned n)
{
    volatile sbm_u32 *p = (volatile sbm_u32 *)(d + off);
    for (unsigned i = 0; i < n; ++i)
        p[i] = w[i];
}

static void note_fail(struct sbm_test_result *res, const struct sbm_job_result *r)
{
    if (r->timeout)
        res->timeouts++;
    else
        res->faults++;
    res->last_js_status = r->js_status;
    res->last_as_fault = r->as_fault;
    res->last_as_addr = r->as_addr;
    res->last_gpu_fault = r->gpu_fault;
}

int sbm_selftest_run(const struct sbm_io *io, enum sbm_fmt fmt, sbm_u64 shaders, volatile sbm_u8 *mem_cpu,
                     sbm_u64 mem_pa, struct sbm_test_result *res)
{
    struct sbm_pt pt;
    struct sbm_job_result r;
    volatile sbm_u8 *d = mem_cpu + SBM_TEST_DATA_OFF;
    volatile sbm_u64 *out = (volatile sbm_u64 *)(d + D_OUT);
    volatile sbm_u32 *pix = (volatile sbm_u32 *)(d + D_PIX);
    sbm_u32 w[48];
    unsigned us3[3];
    int wv, clear = 1;

    res->runs++;
    if (!sbm_soft_reset(io, 0)) {
        res->reset_fail++;
        return 0;
    }
    if (!sbm_power_on(io, shaders, us3)) {
        res->power_fail++;
        sbm_power_off(io);
        return 0;
    }
    sbm_pt_init(&pt, fmt, (volatile sbm_u64 *)mem_cpu, mem_pa, SBM_TEST_TABLES);
    if (sbm_pt_map(&pt, SBM_TEST_VA, mem_pa + SBM_TEST_DATA_OFF, SBM_TEST_DATA_BYTES / 0x1000U, 0) ||
        sbm_as_enable(io, 0, &pt)) {
        res->as_fail++;
        sbm_power_off(io);
        return 0;
    }

    for (unsigned i = 0; i < SBM_TEST_DATA_BYTES / 4U; ++i)
        ((volatile sbm_u32 *)d)[i] = 0;

    /* 1. WRITE_VALUE "STARBITE" on JS1. */
    sbm_write_value(w, 1, 0, SBM_TEST_VA + D_OUT, SBM_WV_IMM64, SBM_TEST_MAGIC);
    put(d, D_WV, w, 16);
    *out = 0xa5a5a5a55a5a5a5aULL;
    barrier(io);
    wv = sbm_run(io, 1, 0, SBM_TEST_VA + D_WV, (sbm_u32)shaders, 200000U, &r);
    if (r.us > res->max_us)
        res->max_us = r.us;
    if (wv && *out == SBM_TEST_MAGIC)
        res->write_ok++;
    else {
        if (!wv)
            note_fail(res, &r);
        wv = 0;
    }

    /* 2. A 64 x 64 fragment clear (no draws, no tiler) on JS0, checked pixel by pixel. */
    if (wv) {
        for (unsigned i = 0; i < SBM_TEST_W * SBM_TEST_H; ++i)
            pix[i] = 0xa5a5a5a5U;
        sbm_samples(w);
        put(d, D_SAMPLES, w, 33);
        sbm_fbd(w, SBM_TEST_VA + D_SAMPLES, SBM_TEST_W, SBM_TEST_H, SBM_TEST_VA + D_PIX, SBM_TEST_W * 4U, SBM_TEST_CLEAR);
        put(d, D_FBD, w, 48);
        sbm_fragment(w, 1, 0, SBM_TEST_W, SBM_TEST_H, SBM_TEST_VA + D_FBD);
        put(d, D_FRAG, w, 16);
        barrier(io);
        clear = sbm_run(io, 0, 0, SBM_TEST_VA + D_FRAG, (sbm_u32)shaders, 200000U, &r);
        if (r.us > res->max_us)
            res->max_us = r.us;
        if (!clear)
            note_fail(res, &r);
        for (unsigned i = 0; clear && i < SBM_TEST_W * SBM_TEST_H; ++i)
            if (pix[i] != SBM_TEST_CLEAR)
                clear = 0;
        if (clear)
            res->clear_ok++;
    }

    sbm_power_off(io);
    return wv && clear;
}
