/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Mali core: the Mali-G72 MP18 bring-up of xnu/src/starlte_gpu.c and starbite/libsb/mali.c,
 * without OS calls, for sbmali.sys (stage 3 of docs/WIN11-GPU.md) and its host test.
 *
 *   power    the G3D domain is only READ (PMU 0x14064044 [3:0] == 0xf; S-Boot leaves it on; switching
 *            it needs an EL3 SMC for the TZPC, so it is never switched); vdd_g3d = S2MPS18 BUCK6 (PMIC
 *            type 1 reg 0x2b) raised to 950 mV through ACPM, only from 0.5..1.0 V, read back, restored
 *            afterwards; clocks: only ACPM DVFS domain 4 (the GPU's), and only to a table frequency
 *   GPU      soft reset, L2 / shader cores / tiler on, address space 0 in one of two table formats:
 *              SBM_FMT_LEGACY  Mali LPAE (TRANSCFG 0, MEMATTR 0x888d88, TRANSTAB | 7, leaves | 0x2c1):
 *                              what XNU runs on this phone
 *              SBM_FMT_AARCH64 AArch64 4K (TRANSCFG ADRMODE 6 | INA 7 | PTW WB | PTW RA, MEMATTR from
 *                              Linux's mair_to_memattr, leaves nG | AP_UNPRIV | AttrIdx 1 | SH_IS | AF):
 *                              what the exynos-linux port reports as fixing faults on this G72 r0p1
 *   jobs     one chain on a job slot, polled with a bound, hard-stopped on failure
 * Register facts: Linux panfrost_regs.h / panfrost_mmu.c / io-pgtable-arm.c (GPL-2.0), our XNU code.
 * Job descriptors: starbite/libsb/mali_desc.c (checked word for word against Mesa).
 */
#ifndef SBM_CORE_H
#define SBM_CORE_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef UINT8 sbm_u8;
typedef UINT32 sbm_u32;
typedef UINT64 sbm_u64;
#else
#include <stdint.h>
typedef uint8_t sbm_u8;
typedef uint32_t sbm_u32;
typedef uint64_t sbm_u64;
#endif

/* Register blocks. */
enum { SBM_GPU = 0, SBM_PMU = 1, SBM_ACPM_SRAM = 2, SBM_ACPM_MBOX = 3, SBM_CMU = 4, SBM_BLOCKS = 5 };
#define SBM_GPU_PA 0x17500000ULL
#define SBM_GPU_SIZE 0x5000U
#define SBM_PMU_PA 0x14064000ULL          /* G3D CONFIGURATION +0x40 (never written), STATUS +0x44 */
#define SBM_PMU_SIZE 0x1000U
#define SBM_ACPM_SRAM_PA 0x02039000ULL
#define SBM_ACPM_SRAM_SIZE 0x30000U
#define SBM_ACPM_MBOX_PA 0x14100000ULL
#define SBM_ACPM_MBOX_SIZE 0x1000U
#define SBM_CMU_PA 0x17400000ULL          /* CMU_G3D: PLL_G3D CON0 +0x140, read only, domain on only */
#define SBM_CMU_SIZE 0x1000U

#define SBM_G72_PRODUCT 0x6221U
#define SBM_BUCK6_REG 0x2bU
#define SBM_BUCK6_950MV 0x68U             /* 300 mV + n * 6.25 mV */
#define SBM_BUCK6_LO 0x20U                /* 0.5 V: below this, not touched */
#define SBM_BUCK6_HI 0x70U                /* 1.0 V: above this, not touched */
#define SBM_DVFS_G3D 4U                   /* ACPM DVFS domain of the GPU (cal id 0xb040004) */

struct sbm_io {
    void *ctx;
    sbm_u32 (*rd)(void *ctx, unsigned block, unsigned off);
    void (*wr)(void *ctx, unsigned block, unsigned off, sbm_u32 v);   /* ordered (barriers inside) */
    void (*delay_us)(void *ctx, unsigned us);
    void (*barrier)(void *ctx);           /* DSB SY: CPU writes to GPU memory visible to the GPU */
};

/* What the GPU reports (read only). */
struct sbm_info {
    sbm_u32 pmu_status, gpu_id, l2_features, core_features, tiler_features, mem_features, mmu_features;
    sbm_u32 as_present, js_present, coherency;
    sbm_u64 shader_present, tiler_present, l2_present;
    sbm_u32 pll_con0;
    sbm_u32 pll_khz;
};

enum sbm_fmt { SBM_FMT_LEGACY = 0, SBM_FMT_AARCH64 = 1 };

/* Page tables in one physically contiguous, CPU-uncached pool: table i at cpu + i * 4 KiB, pa + i * 4 KiB.
 * Table 0 is the level-0 table; the others are handed out as needed. */
struct sbm_pt {
    enum sbm_fmt fmt;
    volatile sbm_u64 *cpu;
    sbm_u64 pa;
    unsigned tables, used;
};

struct sbm_job_result {
    int ok, timeout;
    unsigned js;
    sbm_u32 jirq, mirq, girq, js_status, as_status, as_fault, gpu_fault;
    sbm_u64 as_addr, gpu_addr;
    unsigned us;
};

/* ---- read-only probe ------------------------------------------------------------------------ */
int sbm_domain_on(const struct sbm_io *io);
/* 0 = a G72 with its domain on; < 0: -1 domain off (nothing else read), -2 not a G72. */
int sbm_probe(const struct sbm_io *io, struct sbm_info *inf);
const char *sbm_exc_name(sbm_u32 code);

/* ---- ACPM (APM firmware IPC; protocol of starbite/libsb/acpm.c) ------------------------------ */
int sbm_pmic_read(const struct sbm_io *io, sbm_u32 type, sbm_u32 reg, sbm_u8 *val);
/* Raises vdd_g3d to 950 mV: only from SBM_BUCK6_LO..SBM_BUCK6_HI, read back. *old = the value found.
 * 0 = done; -1 ACPM did not answer; -2 out of range (nothing written); -3 read back differs. */
int sbm_buck6_raise(const struct sbm_io *io, sbm_u8 *old);
/* Puts BUCK6 back to old (only a value in the same range). */
int sbm_buck6_restore(const struct sbm_io *io, sbm_u8 old);
/* DVFS request for the GPU's domain only, one of 572000 546000 455000 338000 299000 260000 kHz, then
 * BUCK6 back to 950 mV (ACPM sets its own table voltage on a DVFS request). 0 = done. */
int sbm_gpu_dvfs(const struct sbm_io *io, sbm_u32 khz);

/* ---- GPU bring-up --------------------------------------------------------------------------- */
int sbm_soft_reset(const struct sbm_io *io, unsigned *us);
int sbm_power_on(const struct sbm_io *io, sbm_u64 shaders, unsigned us[3]);
void sbm_power_off(const struct sbm_io *io);

void sbm_pt_init(struct sbm_pt *pt, enum sbm_fmt fmt, volatile sbm_u64 *cpu, sbm_u64 pa, unsigned tables);
/* Map n 4 KiB pages va -> pa (both 4 KiB aligned, va < 2^48). 0 = done, -1 out of tables / bad args. */
int sbm_pt_map(struct sbm_pt *pt, sbm_u64 va, sbm_u64 pa, unsigned n, int noexec);
void sbm_pt_unmap(struct sbm_pt *pt, sbm_u64 va, unsigned n);
/* The leaf the format writes for pa (exposed for the host test). */
sbm_u64 sbm_pt_leaf(enum sbm_fmt fmt, sbm_u64 pa, int noexec);
sbm_u64 sbm_aarch64_memattr(void);

/* Program address space `as` for pt (TRANSTAB, MEMATTR, TRANSCFG, UPDATE). 0 = done. */
int sbm_as_enable(const struct sbm_io *io, unsigned as, const struct sbm_pt *pt);
/* LOCK [va, va + size) and FLUSH_PT (after changing live mappings). 0 = done. */
int sbm_as_flush(const struct sbm_io *io, unsigned as, sbm_u64 va, sbm_u64 size);
/* Disable an address space (FLUSH_MEM, TRANSTAB/MEMATTR/TRANSCFG 0, UPDATE). */
void sbm_as_disable(const struct sbm_io *io, unsigned as);

/* Start the chain at job_va on slot js in address space as, wait at most timeout_us. 1 = done
 * without any fault. On failure the slot is hard-stopped; the caller resets before the next job. */
int sbm_run(const struct sbm_io *io, unsigned js, unsigned as, sbm_u64 job_va, sbm_u32 affinity,
            unsigned timeout_us, struct sbm_job_result *r);

/* ---- job descriptors (libsb mali_desc.c) ---------------------------------------------------- */
#define SBM_JOB_WRITE_VALUE 2U
#define SBM_JOB_FRAGMENT 9U
#define SBM_WV_IMM64 7U
void sbm_write_value(sbm_u32 w[16], unsigned index, sbm_u64 next, sbm_u64 addr, unsigned type, sbm_u64 imm);
void sbm_fragment(sbm_u32 w[16], unsigned index, sbm_u64 next, unsigned wpx, unsigned hpx, sbm_u64 fbd);
void sbm_fbd(sbm_u32 w[48], sbm_u64 samples, unsigned wpx, unsigned hpx, sbm_u64 rt_base, sbm_u32 stride, sbm_u32 clear);
void sbm_samples(sbm_u32 w[33]);

/* ---- the T5 self-test ----------------------------------------------------------------------- */
/* A work area the driver allocates: physically contiguous, CPU-uncached.
 *   +0x00000  page tables (32 tables)       +0x20000  data (64 KiB) at GPU VA SBM_TEST_VA */
#define SBM_TEST_BYTES 0x30000U
#define SBM_TEST_TABLES 32U
#define SBM_TEST_DATA_OFF 0x20000U
#define SBM_TEST_DATA_BYTES 0x10000U
#define SBM_TEST_VA 0x02000000ULL
#define SBM_TEST_W 64U
#define SBM_TEST_H 64U
#define SBM_TEST_CLEAR 0xff00ff00U        /* opaque green as packed RGBA8 (R in the low byte) */
#define SBM_TEST_MAGIC 0x4554494241425453ULL   /* "STARBITE" */

/* T8: a real draw, XNU's Mesa-built triangle (xnu/src/starlte_gpu_tri.h): tiler job on JS1, fragment job on
 * JS0. The blob is position dependent (its VAs are fixed), so the caller maps its tiler heap and a private
 * render-target window through map_extra (called after the data pages are mapped). */
struct sbm_pt;
struct sbm_tri {
    const sbm_u32 *blob;
    unsigned words;
    sbm_u64 blob_va, tiler_job, frag_job;         /* blob_va inside [SBM_TEST_VA, + SBM_TEST_DATA_BYTES) */
    sbm_u64 rt_va;                                /* where the blob's render target starts */
    unsigned rt_stride, w, h;
    volatile sbm_u32 *rt_cpu;                     /* CPU view of rt_va (uncached) */
    sbm_u32 clear, color;
    int (*map_extra)(void *ctx, struct sbm_pt *pt);
    void *ctx;
};

struct sbm_test_result {
    sbm_u32 runs, write_ok, clear_ok, faults, timeouts, reset_fail, power_fail, as_fail;
    sbm_u32 tri_ok, tri_px_first, tri_px_last, tri_mismatch, tri_bad_px;
    sbm_u32 last_js_status, last_as_fault, last_gpu_fault;
    sbm_u64 last_as_addr;
    sbm_u32 max_us;
};

/* One run: reset, cores on, AS0 in fmt, WRITE_VALUE "STARBITE" (JS1) checked, a 64 x 64 fragment clear
 * (JS0) checked pixel by pixel, cores off. Returns 1 when both jobs ran right. */
int sbm_selftest_run(const struct sbm_io *io, enum sbm_fmt fmt, sbm_u64 shaders, volatile sbm_u8 *mem_cpu,
                     sbm_u64 mem_pa, const struct sbm_tri *tri, struct sbm_test_result *res);

/* PLL_G3D as read from CMU_G3D (domain on only), kHz. */
sbm_u32 sbm_gpu_pll_khz(const struct sbm_io *io);

#endif
