/* SPDX-License-Identifier: GPL-2.0-only
 * Host test for sbm_core.c (stage 3 of docs/WIN11-GPU.md) against a fake Mali-G72:
 *  - the fake GPU translates every job access through the page tables in the format AS0 was set up
 *    for (legacy: table 3 / leaf 1 with read+write; AArch64: table 3 / page 3 with AF, the exact
 *    TRANSCFG and MEMATTR), so wrong descriptor bits fault here as they would on the GPU;
 *  - it runs WRITE_VALUE and clear-only FRAGMENT jobs from the descriptors in memory;
 *  - the fake ACPM implements the IPC queues; the fake PMIC records every write and fails the test
 *    on any register but BUCK6, and on DVFS for any domain but the GPU's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sbmali/sbm_core.h"
#include "../common/sbw_acpm.h"
#include "../../../xnu/src/starlte_gpu_tri.h"

static int fails, checks;
#define CHECK(c, ...)                                                                    \
    do {                                                                                 \
        ++checks;                                                                        \
        if (!(c)) {                                                                      \
            ++fails;                                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

#define MEM_PA 0x8f0000000ULL

struct fake {
    uint32_t gpu[0x5000 / 4];
    uint32_t pmu44;
    uint8_t sram[0x30000];
    uint8_t *mem;                         /* host copy of [MEM_PA, MEM_PA + SBM_TEST_BYTES) */
    int domain_off_gpu_reads;
    uint8_t pmic[3][256];
    int pmic_bad_writes, buck6_writes;
    uint32_t dvfs_domain, dvfs_khz, dvfs_bad;
    int break_af;                         /* AArch64: treat leaves as missing AF */
    char lockaddr[64];
    struct { uint64_t pa, size; uint8_t *host; } region[2];   /* extra memory: render target, tiler heap */
    uint32_t tri_clear, tri_color;        /* the fake rasteriser paints x < y in tri_color on this clear */
    int tiler_jobs;
};

static struct fake F;

/* ---- fake ACPM layout ---- */
#define CHANS 0x8000U
#define Q 8U
#define ESZ 16U
static void s32(uint32_t off, uint32_t v) { memcpy(F.sram + off, &v, 4); }
static uint32_t g32(uint32_t off) { uint32_t v; memcpy(&v, F.sram + off, 4); return v; }

static void acpm_init(void)
{
    s32(0x7f00 + 0x08, CHANS);
    s32(0x7f00 + 0x18, 2);
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t c = CHANS + i * 0x48, area = 0x9000 + i * 0x400;
        s32(c + 0x00, i == 0 ? 0 : 2);    /* channel 0 = DVFS (id 0), channel 1 = PMIC (id 2) */
        s32(c + 0x10, area + 0x00);       /* tx_rear index */
        s32(c + 0x14, area + 0x04);       /* tx_front index */
        s32(c + 0x18, area + 0x40);       /* tx base */
        s32(c + 0x28, area + 0x08);       /* rx_rear */
        s32(c + 0x2c, area + 0x0c);       /* rx_front */
        s32(c + 0x30, area + 0x200);      /* rx base */
        s32(c + 0x34, Q);
        s32(c + 0x38, ESZ);
    }
}

static void acpm_ring(uint32_t id)
{
    uint32_t c = CHANS + (id == 0 ? 0 : 0x48);
    uint32_t txr = g32(c + 0x10), txf = g32(c + 0x14), txb = g32(c + 0x18);
    uint32_t rxf = g32(c + 0x2c), rxb = g32(c + 0x30);
    while (g32(txr) != g32(txf)) {
        uint32_t e = txb + ESZ * g32(txr), cmd[4];
        for (int w = 0; w < 4; ++w)
            cmd[w] = g32(e + 4 * w);
        if (id == 2) {
            uint32_t type = (cmd[0] >> 8) & 0xf, reg = cmd[0] & 0xff;
            if (cmd[1] & 1) {
                if (type != 1 || reg != SBM_BUCK6_REG)
                    F.pmic_bad_writes++;
                else
                    F.buck6_writes++;
                F.pmic[type][reg] = (uint8_t)(cmd[1] >> 8);
            }
            cmd[1] = (uint32_t)F.pmic[type][reg] << 8;
        } else {
            F.dvfs_domain = cmd[0] & 0xffff;
            F.dvfs_khz = cmd[1];
            if (F.dvfs_domain != SBM_DVFS_G3D)
                F.dvfs_bad += F.dvfs_domain != 2;
            F.pmic[1][SBM_BUCK6_REG] = 0x60;   /* ACPM sets its own table voltage */
        }
        uint32_t re = rxb + ESZ * g32(rxf);
        for (int w = 0; w < 4; ++w)
            s32(re + 4 * w, cmd[w]);
        s32(rxf, (g32(rxf) + 1) % Q);
        s32(txr, (g32(txr) + 1) % Q);
    }
}

/* ---- fake GPU ---- */
#define R(off) F.gpu[(off) / 4]
#define AS0 0x2400U

static uint8_t *phys(uint64_t pa, unsigned len)
{
    for (int i = 0; i < 2; ++i)
        if (F.region[i].host && pa >= F.region[i].pa && pa + len <= F.region[i].pa + F.region[i].size)
            return F.region[i].host + (pa - F.region[i].pa);
    if (pa < MEM_PA || pa + len > MEM_PA + SBM_TEST_BYTES)
        return NULL;
    return F.mem + (pa - MEM_PA);
}

/* Translate va in AS0; returns host pointer or NULL (sets the fault). */
static uint8_t *xlate(uint64_t va, unsigned len, int write)
{
    uint64_t transtab = (uint64_t)R(AS0 + 4) << 32 | R(AS0), transcfg = (uint64_t)R(AS0 + 0x34) << 32 | R(AS0 + 0x30);
    uint64_t memattr = (uint64_t)R(AS0 + 0xc) << 32 | R(AS0 + 8);
    int a64 = (transcfg & 0xf) == 6;
    uint64_t table;
    (void)write;
    if (a64) {
        if (transcfg != (6u | 7u << 6 | 2u << 24 | 1u << 30) || memattr != sbm_aarch64_memattr())
            goto fault;
        table = transtab & 0x000ffffffffff000ULL;
    } else {
        if (transcfg != 0 || (transtab & 7) != 7 || memattr != 0x888d88)
            goto fault;
        table = transtab & 0x000ffffffffff000ULL;
    }
    for (int lvl = 0; lvl < 4; ++lvl) {
        uint8_t *t = phys(table, 4096);
        uint64_t e;
        if (!t)
            goto fault;
        memcpy(&e, t + 8 * ((va >> (39 - 9 * lvl)) & 0x1ff), 8);
        if (lvl < 3) {
            if ((e & 3) != 3)
                goto fault;
        } else if (a64) {
            if ((e & 3) != 3 || !(e & (1ull << 10)) || F.break_af || ((e >> 6) & 3) != 1 || ((e >> 2) & 7) != 1)
                goto fault;
        } else {
            if ((e & 3) != 1 || (e & 0xc0) != 0xc0)
                goto fault;
        }
        table = e & 0x000ffffffffff000ULL;
    }
    if (((va & 0xfff) + len) > 4096)
        goto fault;
    return phys(table + (va & 0xfff), len);
fault:
    R(0x2000) |= 1;                       /* MMU_IRQ_RAWSTAT AS0 */
    R(AS0 + 0x1c) = 0xc3;                 /* TRANSLATION_FAULT level 3 */
    R(AS0 + 0x20) = (uint32_t)va;
    R(AS0 + 0x24) = (uint32_t)(va >> 32);
    return NULL;
}

static void run_job(unsigned js)
{
    uint64_t va = (uint64_t)R(0x1800 + js * 0x80 + 0x44) << 32 | R(0x1800 + js * 0x80 + 0x40);
    uint32_t *h = (uint32_t *)xlate(va, 64, 0);
    if (!h)
        return;
    unsigned type = (h[4] >> 1) & 0x7f;
    if (type == SBM_JOB_WRITE_VALUE && h[10] == SBM_WV_IMM64) {
        uint64_t addr = (uint64_t)h[9] << 32 | h[8], imm = (uint64_t)h[13] << 32 | h[12];
        uint8_t *p = xlate(addr, 8, 1);
        if (!p)
            return;
        memcpy(p, &imm, 8);
    } else if (type == 7) {                       /* TILER: binning is not simulated */
        F.tiler_jobs++;
    } else if (type == SBM_JOB_FRAGMENT) {
        uint64_t fbd = ((uint64_t)h[11] << 32 | h[10]) & ~0x3fULL;
        uint32_t *f = (uint32_t *)xlate(fbd, 192, 0);
        if (!f || (h[10] & 1) != 1)
            return;
        unsigned w = (f[8] & 0xffff) + 1, hh = (f[8] >> 16) + 1;
        uint64_t base = (uint64_t)f[41] << 32 | f[40];
        for (unsigned y = 0; y < hh; ++y)
            for (unsigned x = 0; x < w; ++x) {
                uint8_t *p = xlate(base + (uint64_t)y * f[42] + x * 4, 4, 1);
                uint32_t v = (F.tri_clear && f[44] == F.tri_clear && x < y) ? F.tri_color : f[44];
                if (!p)
                    return;
                memcpy(p, &v, 4);
            }
    } else {
        R(0x1800 + js * 0x80 + 0x24) = 0x40;  /* JOB_CONFIG_FAULT */
        R(0x1000) |= 1u << (js + 16);
        return;
    }
    R(0x1800 + js * 0x80 + 0x24) = 1;         /* DONE */
    R(0x1000) |= 1u << js;
}

static uint32_t f_rd(void *ctx, unsigned b, unsigned off)
{
    (void)ctx;
    switch (b) {
    case SBM_GPU:
        if ((F.pmu44 & 0xf) != 0xf)
            F.domain_off_gpu_reads++;
        return off < sizeof(F.gpu) ? F.gpu[off / 4] : 0;
    case SBM_PMU:
        return off == 0x44 ? F.pmu44 : 0;
    case SBM_ACPM_SRAM:
        return g32(off);
    case SBM_CMU:
        return off == 0x140 ? 0xa0a00412u : 0;
    default:
        return 0;
    }
}

static void f_wr(void *ctx, unsigned b, unsigned off, uint32_t v)
{
    (void)ctx;
    if (b == SBM_PMU || b == SBM_CMU) {
        printf("FAIL: write to %s\n", b == SBM_PMU ? "the PMU" : "CMU_G3D");
        ++fails;
        return;
    }
    if (b == SBM_ACPM_SRAM) {
        s32(off, v);
        return;
    }
    if (b == SBM_ACPM_MBOX) {
        if (off == 0x08)
            for (uint32_t id = 0; id < 16; ++id)
                if (v & (1u << (id + 16)))
                    acpm_ring(id);
        return;
    }
    switch (off) {
    case 0x024: R(0x020) &= ~v; return;                       /* GPU_IRQ_CLEAR */
    case 0x030: if (v == 1) R(0x020) |= 0x100; return;        /* SOFT_RESET -> RESET_COMPLETED */
    case 0x180: R(0x140) |= v; return;
    case 0x190: R(0x150) |= v; return;
    case 0x1a0: R(0x160) |= v; return;
    case 0x1c0: R(0x140) &= ~v; return;
    case 0x1d0: R(0x150) &= ~v; return;
    case 0x1e0: R(0x160) &= ~v; return;
    case 0x1004: R(0x1000) &= ~v; return;
    case 0x2004: R(0x2000) &= ~v; return;
    }
    if (off >= 0x1800 && off < 0x1980 && (off & 0x7f) == 0x60 && v == 1) {
        run_job((off - 0x1800) / 0x80);
        return;
    }
    if (off == AS0 + 0x18 && v == 2)
        snprintf(F.lockaddr, sizeof(F.lockaddr), "%08x%08x", R(AS0 + 0x14), R(AS0 + 0x10));
    if (off < sizeof(F.gpu))
        F.gpu[off / 4] = v;
}

static void fake_init(uint8_t *mem)
{
    memset(&F, 0, sizeof(F));
    F.mem = mem;
    F.pmu44 = 0x10000f;
    R(0x000) = 0x62210010;
    R(0x014) = 0x2830;
    R(0x018) = 0xff;
    R(0x01c) = 7;
    R(0x100) = 0x5771777;
    R(0x110) = 1;
    R(0x120) = 1;
    F.pmic[1][SBM_BUCK6_REG] = 0x33;
    acpm_init();
}

static int map_extra(void *ctx, struct sbm_pt *pt)
{
    struct { uint64_t rt_lo, rt_hi; } *c = ctx;
    if (sbm_pt_map(pt, c->rt_lo, 0x900000000ULL, (unsigned)((c->rt_hi - c->rt_lo) / 4096), 1))
        return -1;
    return sbm_pt_map(pt, SG_TRI_HEAP_VA, 0x910000000ULL, SG_TRI_HEAP_BYTES / 4096, 1);
}

/* sbw_acpm accessors straight onto the fake (for the domain refusal test) */
static uint32_t a_srd(void *c, uint32_t off) { (void)c; return g32(off); }
static void a_swr(void *c, uint32_t off, uint32_t v) { (void)c; s32(off, v); }
static void a_mwr(void *c, uint32_t off, uint32_t v) { (void)c; if (off == 0x08) for (uint32_t id = 0; id < 16; ++id) if (v & (1u << (id + 16))) acpm_ring(id); }

int main(void)
{
    static uint8_t mem[SBM_TEST_BYTES] __attribute__((aligned(4096)));
    struct sbm_io io = { NULL, f_rd, f_wr, NULL, NULL };
    struct sbm_info inf;
    struct sbm_test_result res;
    uint8_t old = 0;

    /* probe */
    fake_init(mem);
    CHECK(sbm_probe(&io, &inf) == 0, "probe a G72");
    CHECK(inf.shader_present == 0x5771777 && inf.pll_khz == 260000, "cores %#llx, PLL %u kHz",
          (unsigned long long)inf.shader_present, inf.pll_khz);
    fake_init(mem);
    F.pmu44 = 0x100000;
    CHECK(sbm_probe(&io, &inf) == -1 && F.domain_off_gpu_reads == 0, "domain off: no GPU read");
    fake_init(mem);
    R(0) = 0x70020000;
    CHECK(sbm_probe(&io, &inf) == -2, "not a G72");

    /* BUCK6 through ACPM */
    fake_init(mem);
    CHECK(sbm_buck6_raise(&io, &old) == 0 && old == 0x33 && F.pmic[1][0x2b] == 0x68, "BUCK6 0x33 -> 0x68");
    CHECK(sbm_buck6_restore(&io, old) == 0 && F.pmic[1][0x2b] == 0x33, "BUCK6 restored");
    fake_init(mem);
    F.pmic[1][0x2b] = 0x10;
    CHECK(sbm_buck6_raise(&io, &old) == -2 && F.buck6_writes == 0, "out of range: nothing written");
    CHECK(sbm_buck6_restore(&io, 0x80) == -2 && F.buck6_writes == 0, "restore refuses out of range");
    fake_init(mem);
    CHECK(sbm_gpu_dvfs(&io, 572000) == 0 && F.dvfs_domain == 4 && F.dvfs_khz == 572000, "DVFS domain 4");
    CHECK(F.pmic[1][0x2b] == 0x68, "BUCK6 rewritten after DVFS (ACPM left %#x)", F.pmic[1][0x2b]);
    CHECK(sbm_gpu_dvfs(&io, 600000) == -1 && F.dvfs_khz == 572000, "not a table frequency: refused");
    CHECK(F.pmic_bad_writes == 0 && F.dvfs_bad == 0, "only BUCK6 and domain 4 ever written");

    /* descriptors */
    CHECK(sbm_pt_leaf(SBM_FMT_LEGACY, 0x8f0010000ULL, 0) == 0x8f00102c1ULL, "legacy leaf");
    CHECK(sbm_pt_leaf(SBM_FMT_AARCH64, 0x8f0010000ULL, 0) == 0x8f0010f47ULL, "aarch64 leaf %#llx",
          (unsigned long long)sbm_pt_leaf(SBM_FMT_AARCH64, 0x8f0010000ULL, 0));
    CHECK(sbm_pt_leaf(SBM_FMT_AARCH64, 0x8f0010000ULL, 1) == (0x8f0010f47ULL | 3ULL << 53), "aarch64 XN");
    CHECK(sbm_aarch64_memattr() == 0x4c4c4c4c8c4c8f4cULL, "MEMATTR %#llx", (unsigned long long)sbm_aarch64_memattr());

    /* the self-test in both formats, against the table-walking fake */
    for (int fmt = 0; fmt < 2; ++fmt) {
        fake_init(mem);
        memset(&res, 0, sizeof(res));
        int ok = 1;
        for (int i = 0; i < 20; ++i)
            ok &= sbm_selftest_run(&io, (enum sbm_fmt)fmt, 0x5771777, mem, MEM_PA, NULL, &res);
        CHECK(ok && res.write_ok == 20 && res.clear_ok == 20 && res.faults == 0, "fmt %d: 20 runs (%u/%u, %u faults)",
              fmt, res.write_ok, res.clear_ok, res.faults);
        CHECK(R(0x140) == 0 && R(0x150) == 0 && R(0x160) == 0, "fmt %d: cores off after the test", fmt);
    }

    /* a broken AArch64 leaf (no access flag) faults and is reported, the slot is hard-stopped */
    fake_init(mem);
    memset(&res, 0, sizeof(res));
    F.break_af = 1;
    CHECK(sbm_selftest_run(&io, SBM_FMT_AARCH64, 0x5771777, mem, MEM_PA, NULL, &res) == 0 && res.faults == 1 &&
          res.last_as_fault == 0xc3 && res.last_as_addr == SBM_TEST_VA, "fault reported (%u, %#x @ %#llx)",
          res.faults, res.last_as_fault, (unsigned long long)res.last_as_addr);
    CHECK(R(0x1880 + 0x20) == 3, "JS1 hard-stopped");

    /* a fragment that does not clear everything is caught */
    {
        struct sbm_pt pt;
        fake_init(mem);
        sbm_pt_init(&pt, SBM_FMT_LEGACY, (volatile uint64_t *)mem, MEM_PA, SBM_TEST_TABLES);
        CHECK(sbm_pt_map(&pt, 0x02000000, MEM_PA + 0x10000, 16, 0) == 0, "map");
        CHECK(sbm_pt_map(&pt, 0x02001000, MEM_PA + 0x10000, 1, 0) == 0, "remap one page");
        sbm_pt_unmap(&pt, 0x02001000, 1);
        uint64_t e;
        memcpy(&e, mem + 0x3000 + 8 * 1, 8);
        CHECK(e == 0, "unmapped leaf cleared");
        CHECK(sbm_pt_map(&pt, 0x02000001, MEM_PA, 1, 0) == -1, "unaligned refused");
        CHECK(sbm_pt_map(&pt, 1ULL << 48, MEM_PA, 1, 0) == -1, "beyond 48 bits refused");
        /* running out of tables: 16 tables, each new 1 GiB needs 2, each new 2 MiB 1 */
        int r = 0;
        for (int i = 0; i < 20 && !r; ++i)
            r = sbm_pt_map(&pt, (uint64_t)(i + 1) << 30, MEM_PA, 1, 0);
        CHECK(r == -1, "out of tables refused");
    }

    /* MMU lock region (Linux lock_region): 64 KiB at 0x02000000 -> width 15, i.e. 32 KiB? no: 64 KiB = 2^16 -> 15 */
    fake_init(mem);
    {
        struct sbm_pt pt;
        sbm_pt_init(&pt, SBM_FMT_LEGACY, (volatile uint64_t *)mem, MEM_PA, SBM_TEST_TABLES);
        sbm_as_enable(&io, 0, &pt);
        CHECK(sbm_as_flush(&io, 0, 0x02000000, 0x10000) == 0, "flush");
        CHECK(strcmp(F.lockaddr, "000000000200000f") == 0, "LOCKADDR %s", F.lockaddr);
        CHECK(sbm_as_flush(&io, 0, 0x02000000, 0x1000) == 0 && strcmp(F.lockaddr, "000000000200000e") == 0,
              "minimum 32 KiB lock: %s", F.lockaddr);
    }

    /* T8: XNU's Mesa-built triangle through both formats, heap and render target mapped by map_extra */
    {
        static uint8_t heap[SG_TRI_HEAP_BYTES];
        uint64_t rt_va = 0x4000000ULL + (uint64_t)SG_TRI_RT_Y * 5760 + SG_TRI_RT_X * 4;
        uint64_t rt_lo = rt_va & ~0xfffULL, rt_hi = (rt_va + (SG_TRI_FB_H - 1) * 5760ULL + SG_TRI_FB_W * 4 + 0xfff) & ~0xfffULL;
        uint8_t *rt = aligned_alloc(4096, rt_hi - rt_lo);
        struct tri_ctx { uint64_t rt_lo, rt_hi; } tc = { rt_lo, rt_hi };
        struct sbm_tri t = { sg_tri_blob, SG_TRI_BLOB_WORDS, SG_TRI_BLOB_VA, SG_TRI_TILER_JOB, SG_TRI_FRAG_JOB, rt_va, 5760,
                             SG_TRI_FB_W, SG_TRI_FB_H, (volatile uint32_t *)(rt + (rt_va - rt_lo)), SG_TRI_CLEAR,
                             SG_TRI_COLOR, map_extra, &tc };
        for (int fmt = 0; fmt < 2; ++fmt) {
            fake_init(mem);
            F.region[0].pa = 0x900000000ULL; F.region[0].size = rt_hi - rt_lo; F.region[0].host = rt;
            F.region[1].pa = 0x910000000ULL; F.region[1].size = sizeof(heap); F.region[1].host = heap;
            F.tri_clear = SG_TRI_CLEAR;
            F.tri_color = SG_TRI_COLOR;
            memset(&res, 0, sizeof(res));
            int ok = 1;
            for (int i = 0; i < 5; ++i)
                ok &= sbm_selftest_run(&io, (enum sbm_fmt)fmt, 0x5771777, mem, MEM_PA, &t, &res);
            CHECK(ok && res.tri_ok == 5 && res.tri_mismatch == 0 && res.tri_bad_px == 0 && F.tiler_jobs == 5,
                  "fmt %d triangle: %u ok, %u px, %u bad, %d tiler jobs, %u faults", fmt, res.tri_ok, res.tri_px_first,
                  res.tri_bad_px, F.tiler_jobs, res.faults);
            CHECK(res.tri_px_first == SG_TRI_FB_W * (SG_TRI_FB_W - 1) / 2, "fake triangle pixel count %u", res.tri_px_first);
        }
        /* a fragment job that leaves foreign pixels is caught */
        fake_init(mem);
        F.region[0].pa = 0x900000000ULL; F.region[0].size = rt_hi - rt_lo; F.region[0].host = rt;
        F.region[1].pa = 0x910000000ULL; F.region[1].size = sizeof(heap); F.region[1].host = heap;
        F.tri_clear = SG_TRI_CLEAR;
        F.tri_color = 0x12345678;
        memset(&res, 0, sizeof(res));
        CHECK(!sbm_selftest_run(&io, SBM_FMT_LEGACY, 0x5771777, mem, MEM_PA, &t, &res) && res.tri_bad_px > 0,
              "foreign pixels caught (%u)", res.tri_bad_px);
        free(rt);
    }

    /* DVFS: only domains 2, 3 and 4 ever reach ACPM (0 = MIF, 1 = INT never) */
    fake_init(mem);
    CHECK(sbm_gpu_dvfs(&io, 260000) == 0 && F.dvfs_domain == 4, "GPU DVFS uses domain 4");
    CHECK(sbm_gpu_pll_khz(&io) == 260000, "PLL_G3D readback %u", sbm_gpu_pll_khz(&io));

    {
        struct sbw_acpm_io a = { NULL, a_srd, a_swr, a_mwr, NULL, NULL };
        fake_init(mem);
        CHECK(!sbw_acpm_dvfs(&a, 0, 1794000) && !sbw_acpm_dvfs(&a, 1, 534000) && !sbw_acpm_dvfs(&a, 5, 100000) &&
              F.dvfs_khz == 0, "MIF (0), INT (1), others: refused before ACPM");
        CHECK(sbw_acpm_dvfs(&a, 2, 1690000) && F.dvfs_domain == 2, "A55 (2) passes");
    }
    CHECK(!strcmp(sbm_exc_name(0xc3), "TRANSLATION_FAULT") && !strcmp(sbm_exc_name(0x58), "DATA_INVALID_FAULT"), "names");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
