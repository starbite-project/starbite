/* SPDX-License-Identifier: GPL-2.0-only
 * sbgpu's backend on the phone (Track E, 2026-11-14; ../sbshm-gpu.h, ../host/sbg_gpu.c): the Linux guest's GPU work
 * on the Mali-G72 that Starbite's compositor (gpucomp) already brought up and keeps (address space 0, job slot 1,
 * polled). The guest's Mesa (Panfrost) builds the job chains in its own pages; here:
 *   params  the Mali's own registers (GPU_ID, the presents, the features: Linux's panfrost_regs.h offsets), and
 *           address space 1 set up: Mali-LPAE tables (the format of mali.c's AS0) at GG_TBL, DRAM bank 2's free
 *           range (sb.h: "0x8_E800_0000..0x8_EFFF_FFFF is free"; Normal non-cacheable for the CPU like the whole
 *           bank, so no cache maintenance), MEMATTR as AS0, UPDATE
 *   map     the guest's pages (sbg_gpu.c checked each lies in the guest's RAM) into AS 1's level-3 tables (taken from
 *           GG_TBL as needed, 2 MiB each), read-write or read-only as AS0's attributes, then UPDATE (the AS's TLB)
 *   submit  the chain on slot 0 (a fragment chain) or 1 with AS 1 in CONFIG_NEXT (clean + invalidate the caches at
 *           both ends, priority 8: gpucomp's and Panfrost's), run to its end here (sb_gpu_wait, polled, 500 ms):
 *           Starbite's loop is single-threaded and gpucomp's jobs are synchronous too, so the two never overlap
 * Three failed jobs in one Linux run and the GPU is refused to the guest until the next start (gpucomp falls back
 * to the CPU itself if the GPU ends up wedged). The first three jobs' times and every failure go to the log. */
#if defined(SBOS_KERNEL) && !defined(GUEST_QEMU) && !defined(SB_HOST)
#include "../../guest/sbshm-gpu.h"
#define GG_TBL 0x8e8000000ULL               /* AS 1's tables: 4 MiB of DRAM bank 2's free range */
#define GG_TBL_PAGES 1024U                  /* L0, L1, 4 L2 (4 GiB of VA), then up to 1018 L3 (2 GiB mapped) */
#define GG_AS 1U
#define GG_ASR (0x2400U + GG_AS * 0x40U)
static unsigned gg_tbl_used, gg_fails, gg_jobs;
static int gg_dead;

static volatile uint64_t *gg_page(unsigned i) { return (volatile uint64_t *)(uintptr_t)(GG_TBL + (uint64_t)i * 0x1000U); }
static void gg_zero(unsigned i)
{
    volatile uint64_t *p = gg_page(i);
    for (unsigned k = 0; k < 512U; ++k)
        p[k] = 0;
}
static int gg_as_wait(void)
{
    for (unsigned i = 0; i < 10000U; ++i) {
        if (!(sb_gpu_rd(GG_ASR + 0x28) & 1U))      /* AS_STATUS: not active (busy) */
            return 1;
        sb_udelay(1);
    }
    return 0;
}
static int gg_as_update(void)
{
    uint64_t t = GG_TBL | 7U;
    __asm__ volatile("dsb sy" : : : "memory");
    if (!gg_as_wait())
        return 0;
    (void)sb_gpu_wr(GG_ASR + 0x00, (uint32_t)t);
    (void)sb_gpu_wr(GG_ASR + 0x04, (uint32_t)(t >> 32));
    (void)sb_gpu_wr(GG_ASR + 0x08, 0x888d88U);       /* MEMATTR as AS0 */
    (void)sb_gpu_wr(GG_ASR + 0x0c, 0);
    (void)sb_gpu_wr(GG_ASR + 0x30, 0);                /* TRANSCFG 0: legacy Mali-LPAE, as AS0 */
    (void)sb_gpu_wr(GG_ASR + 0x34, 0);
    (void)sb_gpu_wr(GG_ASR + 0x18, 1);                /* UPDATE */
    return gg_as_wait();
}
static volatile uint64_t *gg_l3(uint64_t va, int make)
{
    if (va >= (1ULL << 32))
        return 0;
    volatile uint64_t *l2 = gg_page(2U + (unsigned)(va >> 30));
    unsigned i2 = (unsigned)((va >> 21) & 0x1ffU);
    if (!(l2[i2] & 1U)) {
        if (!make || gg_tbl_used >= GG_TBL_PAGES)
            return 0;
        gg_zero(gg_tbl_used);
        l2[i2] = (GG_TBL + (uint64_t)gg_tbl_used++ * 0x1000U) | 3U;
    }
    return (volatile uint64_t *)(uintptr_t)(l2[i2] & 0xfffffff000ULL);
}
static uint64_t gg_rd64(unsigned lo) { return (uint64_t)sb_gpu_rd(lo) | (uint64_t)sb_gpu_rd(lo + 4U) << 32; }

static int gg_params(uint64_t *p, unsigned n)
{
    if (!sb_gpu_is_up() || n < 41)
        return 0;                           /* no compositor GPU in this boot: the guest has none either */
    uint32_t id = sb_gpu_rd(0x000);
    uint64_t l2 = gg_rd64(0x120);
    p[0] = id >> 16;                        /* DRM_PANFROST_PARAM_GPU_PROD_ID */
    p[1] = id & 0xffffU;                    /* GPU_REVISION */
    p[2] = gg_rd64(0x100);                  /* SHADER_PRESENT */
    p[3] = gg_rd64(0x110);                  /* TILER_PRESENT */
    p[4] = l2;                              /* L2_PRESENT */
    p[5] = gg_rd64(0xe00);                  /* STACK_PRESENT */
    p[6] = sb_gpu_rd(0x018);                /* AS_PRESENT */
    p[7] = sb_gpu_rd(0x01c);                /* JS_PRESENT */
    p[8] = sb_gpu_rd(0x004);                /* L2_FEATURES */
    p[9] = sb_gpu_rd(0x008);                /* CORE_FEATURES */
    p[10] = sb_gpu_rd(0x00c);               /* TILER_FEATURES */
    p[11] = sb_gpu_rd(0x010);               /* MEM_FEATURES */
    p[12] = sb_gpu_rd(0x014);               /* MMU_FEATURES */
    p[13] = sb_gpu_rd(0x0ac);               /* THREAD_FEATURES */
    p[14] = sb_gpu_rd(0x0a0);               /* MAX_THREADS */
    p[15] = sb_gpu_rd(0x0a4);               /* THREAD_MAX_WORKGROUP_SZ */
    p[16] = sb_gpu_rd(0x0a8);               /* THREAD_MAX_BARRIER_SZ */
    p[17] = sb_gpu_rd(0x300);               /* COHERENCY_FEATURES */
    for (unsigned k = 0; k < 4; ++k)
        p[18 + k] = sb_gpu_rd(0x0b0 + 4U * k);   /* TEXTURE_FEATURES0..3 */
    for (unsigned k = 0; k < 16; ++k)
        p[22 + k] = sb_gpu_rd(0x0c0 + 4U * k);   /* JS_FEATURES0..15 */
    unsigned groups = 0;
    for (uint64_t m = l2; m; m &= m - 1)
        ++groups;
    p[38] = groups;                         /* NR_CORE_GROUPS */
    p[39] = sb_gpu_rd(0x310);               /* THREAD_TLS_ALLOC */
    p[40] = sb_gpu_rd(0x04c);               /* AFBC_FEATURES */
    for (unsigned i = 0; i < 6; ++i)        /* AS 1: L0 -> L1 -> 4 L2 (VA 0..4 GiB) */
        gg_zero(i);
    gg_page(0)[0] = (GG_TBL + 0x1000U) | 3U;
    for (unsigned j = 0; j < 4; ++j)
        gg_page(1)[j] = (GG_TBL + (2U + j) * 0x1000ULL) | 3U;
    gg_tbl_used = 6;
    gg_dead = 0;
    gg_fails = gg_jobs = 0;
    if (!gg_as_update()) {
        sb_log("guest: sbgpu: the GPU's address space 1 did not take its tables: no GPU for Linux");
        return 0;
    }
    return 1;
}

static int gg_map(uint64_t va, const volatile uint64_t *pfn, uint32_t n, uint32_t flags)
{
    if (gg_dead)
        return -SBG_EIO;
    uint64_t attr = (flags & SBGPU_MAP_RO) ? 0x241U : 0x2c1U;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t v = va + (uint64_t)i * 0x1000U;
        volatile uint64_t *l3 = gg_l3(v, 1);
        if (!l3)
            return -SBG_ENOSPC;
        l3[(v >> 12) & 0x1ffU] = (pfn[i] << 12) | attr;
    }
    return gg_as_update() ? 0 : -SBG_EIO;
}

static int gg_unmap(uint64_t va, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t v = va + (uint64_t)i * 0x1000U;
        volatile uint64_t *l3 = gg_l3(v, 0);
        if (l3)
            l3[(v >> 12) & 0x1ffU] = 0;
    }
    return gg_as_update() ? 0 : -SBG_EIO;
}

static int gg_submit(uint64_t jc, uint32_t flags, uint64_t seq)
{
    if (gg_dead || !sb_gpu_is_up())
        return -SBG_EIO;
    unsigned js = (flags & SBGPU_SUBMIT_FS) ? 0U : 1U, s = 0x1800U + js * 0x80U;
    (void)sb_gpu_wr(0x2004, 0xffffffffU);             /* MMU_IRQ_CLEAR */
    (void)sb_gpu_wr(0x1004, 0xffffffffU);             /* JOB_IRQ_CLEAR */
    __asm__ volatile("dsb sy" : : : "memory");
    (void)sb_gpu_wr(s + 0x40, (uint32_t)jc);          /* HEAD_NEXT */
    (void)sb_gpu_wr(s + 0x44, (uint32_t)(jc >> 32));
    (void)sb_gpu_wr(s + 0x50, sb_gpu_rd(0x100));      /* AFFINITY_NEXT: every shader core */
    (void)sb_gpu_wr(s + 0x54, sb_gpu_rd(0x104));
    (void)sb_gpu_wr(s + 0x58, GG_AS | 3U << 8 | 3U << 12 | 8U << 16);   /* CONFIG_NEXT */
    (void)sb_gpu_wr(s + 0x60, 1);                     /* COMMAND_NEXT: START */
    struct sb_gpu_result r;
    int ok = sb_gpu_wait(1U << js, 500000, &r);
    ++gg_jobs;
    if (!ok || gg_jobs <= 3) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, ok ? "guest: sbgpu: job " : "guest: sbgpu: job FAILED ");
        ub_dec(&u, gg_jobs);
        ub_s(&u, " on slot ");
        ub_dec(&u, js);
        ub_s(&u, " in us ");
        ub_dec(&u, r.us);
        if (!ok) {
            ub_s(&u, ": js status ");
            ub_hex(&u, r.js_status, 2);
            ub_s(&u, r.timeout ? " (timeout)" : "");
            ub_s(&u, ", AS1 fault ");
            ub_hex(&u, sb_gpu_rd(GG_ASR + 0x1c), 8);
            ub_s(&u, ", mmu irq ");
            ub_hex(&u, r.mirq, 8);
        }
        sb_log(u.b);
    }
    if (!ok && ++gg_fails >= 3) {
        gg_dead = 1;
        sb_log("guest: sbgpu: three jobs failed: no GPU for Linux until its next start");
    }
    sbg_gpu_done(seq, ok ? 0 : (r.js_status ? r.js_status : 0x40U));
    return 0;
}

static const struct sbg_gpu guest_gpu = {gg_params, gg_map, gg_unmap, gg_submit};
#define GUEST_GPU_OPS (&guest_gpu)
#else
#define GUEST_GPU_OPS 0
#endif
