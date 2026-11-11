/* SPDX-License-Identifier: GPL-2.0-only
 * The Mali-G72 MP18 (Bifrost) for Starbite Lab's GPU tests, without any OS: the recipe Starbite's
 * iOS port ran on this phone (xnu/src/starlte_gpu.c, NOTES.md 2026-10-28 phases 1-5), polled.
 *
 *   power   G3D domain (PMU 0x14064044 [3:0] = 0xf: S-Boot leaves it on), PLL_G3D (S-Boot's
 *           260 MHz), vdd_g3d = S2MPS18 BUCK6 reg 0x2b raised to 950 mV (0x68) by the caller
 *           (jobs need it: lower gave DATA_INVALID_FAULTs on Linux); soft reset (GPU_COMMAND 1,
 *           RESET_COMPLETED); L2, then the shader cores, then the tiler (PWRON / READY)
 *   memory  32 MiB of DRAM bank 2 at 0x8F0000000 (nothing else uses it; non-cacheable on the CPU:
 *           the GPU is not IO-coherent): Mali-LPAE tables at +0 (L0, L1, L2, 4 L3 for the data,
 *           9 L3 for the panel), 8 MiB of data from +0x10000 at GPU VA 0x02000000; the panel's
 *           scanout 0xCC000000 at GPU VA 0x04000000. AS0: TRANSTAB = tables | 7 (table walk,
 *           inner read), MEMATTR 0x888D88, TRANSCFG 0 (legacy LPAE), UPDATE
 *   jobs    a chain on a job slot: HEAD_NEXT, AFFINITY_NEXT (every shader core), CONFIG_NEXT (AS0,
 *           clean + invalidate the GPU caches at the start and the end, priority 8), COMMAND_NEXT
 *           START; JOB_IRQ_RAWSTAT polled (interrupts are not wired to the CPU here: masked at
 *           the CPU, JOB_IRQ_MASK 0), with the MMU and GPU fault registers read on every failure
 *
 * Nothing here runs until the application opened a GPU session with a complete authorisation
 * (sb_gpu_open: the user confirmed "Power up the GPU", which names every step above); every
 * register write refuses otherwise. Registers: Linux panfrost_regs.h / kbase (GPL, reference only).
 */
#include "sb.h"

#define GPU_COMMAND 0x030
#define GPU_IRQ_RAWSTAT 0x020
#define GPU_IRQ_CLEAR 0x024
#define GPU_IRQ_MASK 0x028
#define GPU_STATUS 0x034
#define GPU_FAULTSTATUS 0x03c
#define GPU_FAULTADDR_LO 0x040
#define SHADER_PRESENT_LO 0x100
#define TILER_PRESENT_LO 0x110
#define L2_PRESENT_LO 0x120
#define SHADER_READY_LO 0x140
#define TILER_READY_LO 0x150
#define L2_READY_LO 0x160
#define SHADER_PWRON_LO 0x180
#define TILER_PWRON_LO 0x190
#define L2_PWRON_LO 0x1a0
#define SHADER_PWROFF_LO 0x1c0
#define TILER_PWROFF_LO 0x1d0
#define L2_PWROFF_LO 0x1e0
#define JOB_IRQ_RAWSTAT 0x1000
#define JOB_IRQ_CLEAR 0x1004
#define JOB_IRQ_MASK 0x1008
#define MMU_IRQ_RAWSTAT 0x2000
#define MMU_IRQ_CLEAR 0x2004
#define MMU_IRQ_MASK 0x2008
#define AS0 0x2400

static int gpu_open, gpu_up;

#ifdef SB_HOST
void host_gpu_write(unsigned off, uint32_t v);      /* the host preview's fake Mali (host.c) */
#endif

uint32_t sb_gpu_rd(unsigned off)
{
    uint32_t v = *(volatile uint32_t *)(SB_GPU + off);
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static int wr(unsigned off, uint32_t v)
{
    if (!gpu_open || off >= 0x5000U)
        return 0;
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)(SB_GPU + off) = v;
    __asm__ volatile("dsb osh" : : : "memory");
#ifdef SB_HOST
    host_gpu_write(off, v);
#endif
    return 1;
}

int sb_gpu_wr(unsigned off, uint32_t v)
{
    return wr(off, v);
}

int sb_gpu_domain(void)
{
    return (*(volatile uint32_t *)0x14064044UL & 0xfU) == 0xfU;
}

/* A GPU session: the user confirmed the power-up (an authorisation for the BUCK6 write, which
 * lists the reset, the cores, the page tables and the jobs as what happens). */
int sb_gpu_open(const struct sb_auth *a)
{
    gpu_open = a && sb_auth_complete(a) && a->space == SB_SP_PMIC && a->first == (1U << 8 | 0x2bU) &&
               sb_gpu_domain();
    if (gpu_open)
        sb_log("gpu: session opened (confirmed power-up)");
    return gpu_open;
}

int sb_gpu_is_open(void)
{
    return gpu_open;
}

int sb_gpu_is_up(void)
{
    return gpu_open && gpu_up;
}

void sb_gpu_close(void)
{
    gpu_open = gpu_up = 0;
}

static uint64_t ticks_us(uint64_t t)
{
    return t * 1000000U / sb_freq();
}

int sb_gpu_reset(unsigned *us)
{
    uint64_t t0 = sb_ticks();
    uint32_t raw = 0;
    if (!wr(GPU_IRQ_CLEAR, 0xffffffffU) || !wr(GPU_COMMAND, 1))
        return 0;
    while (ticks_us(sb_ticks() - t0) < 10000U) {
        raw = sb_gpu_rd(GPU_IRQ_RAWSTAT);
        if (raw & 0x100U)
            break;
        sb_udelay(5);
    }
    (void)wr(GPU_IRQ_CLEAR, 0xffffffffU);
    (void)wr(GPU_IRQ_MASK, 0);
    (void)wr(JOB_IRQ_MASK, 0);
    (void)wr(MMU_IRQ_MASK, 0);
    if (us)
        *us = (unsigned)ticks_us(sb_ticks() - t0);
    gpu_up = 0;
    return (raw & 0x100U) != 0;
}

static int power(unsigned on, unsigned ready, uint32_t mask, unsigned *us)
{
    uint64_t t0 = sb_ticks();
    uint32_t r = 0;
    if (!wr(on, mask))
        return 0;
    while (ticks_us(sb_ticks() - t0) < 20000U) {
        r = sb_gpu_rd(ready);
        if ((r & mask) == mask)
            break;
        sb_udelay(5);
    }
    if (us)
        *us = (unsigned)ticks_us(sb_ticks() - t0);
    return (r & mask) == mask;
}

int sb_gpu_power_on(uint32_t shaders, unsigned us[3])
{
    unsigned d[3] = { 0, 0, 0 };
    int ok = power(L2_PWRON_LO, L2_READY_LO, sb_gpu_rd(L2_PRESENT_LO), &d[0]) &&
             power(SHADER_PWRON_LO, SHADER_READY_LO, shaders, &d[1]) &&
             power(TILER_PWRON_LO, TILER_READY_LO, sb_gpu_rd(TILER_PRESENT_LO), &d[2]);
    if (us)
        for (unsigned i = 0; i < 3; ++i)
            us[i] = d[i];
    return ok;
}

void sb_gpu_power_off(void)
{
    (void)wr(TILER_PWROFF_LO, sb_gpu_rd(TILER_READY_LO));
    (void)wr(SHADER_PWROFF_LO, sb_gpu_rd(SHADER_READY_LO));
    sb_udelay(100);
    (void)wr(L2_PWROFF_LO, sb_gpu_rd(L2_READY_LO));
    sb_udelay(100);
    gpu_up = 0;
}

static unsigned map_tables_used;           /* sb_gpu_map's tables handed out since sb_gpu_tables */

/* Page tables (Mali-LPAE: table entries | 3, level-3 leaves | 0x2c1 = valid, read, write, outer
 * shareable, MEMATTR 0) and zeroed data. */
void sb_gpu_tables(void)
{
    volatile uint64_t *q = (volatile uint64_t *)SB_GPU_MEM;
    if (!gpu_open)
        return;
    map_tables_used = 0;                    /* sb_gpu_map's tables start again */
    for (uint64_t i = 0; i < (0x10000U + SB_GPU_DATA_SIZE) / 8U; ++i)
        q[i] = 0;
    volatile uint64_t *l0 = q, *l1 = q + 512, *l2 = q + 1024;
    l0[0] = (SB_GPU_MEM + 0x1000U) | 3U;
    l1[0] = (SB_GPU_MEM + 0x2000U) | 3U;
    unsigned dpages = (unsigned)(SB_GPU_DATA_SIZE / 0x1000U);
    for (unsigned t = 0; t * 512U < dpages; ++t) {
        volatile uint64_t *l3 = q + (0x3000U + t * 0x1000U) / 8U;
        l2[((SB_GPU_VA >> 21) & 0x1ffU) + t] = (SB_GPU_MEM + 0x3000U + t * 0x1000U) | 3U;
        for (unsigned i = 0; i < 512U && t * 512U + i < dpages; ++i)
            l3[i] = (SB_GPU_DATA + (uint64_t)(t * 512U + i) * 0x1000U) | 0x2c1U;
    }
    unsigned ppages = (unsigned)(((uint64_t)SB_FB_W * 4U * SB_FB_H + 0xfffU) / 0x1000U);
    for (unsigned t = 0; t * 512U < ppages; ++t) {
        volatile uint64_t *l3 = q + (0x7000U + t * 0x1000U) / 8U;
        l2[((SB_GPU_PANEL_VA >> 21) & 0x1ffU) + t] = (SB_GPU_MEM + 0x7000U + t * 0x1000U) | 3U;
        for (unsigned i = 0; i < 512U && t * 512U + i < ppages; ++i)
            l3[i] = ((uint64_t)SB_FB + (uint64_t)(t * 512U + i) * 0x1000U) | 0x2c1U;
    }
    __asm__ volatile("dsb sy" : : : "memory");
}

/* Starbite OS's GPU compositor (gpucomp.c): more of the GPU's address space, below 1 GiB. The level-3 tables come
 * from the GPU memory block after the data window (SB_GPU_TBL_POOL), handed out as needed (zeroed); call after
 * sb_gpu_tables and before sb_gpu_up (or sb_gpu_as0 again after). 1 mapped, 0 out of tables or out of range. */
int sb_gpu_map(uint64_t va, uint64_t pa, uint64_t size)
{
    volatile uint64_t *q = (volatile uint64_t *)SB_GPU_MEM;
    volatile uint64_t *l2 = q + 1024;
    if (!gpu_open || (va & 0xfffU) || (pa & 0xfffU) || va + size > (1ULL << 30))
        return 0;
    for (uint64_t o = 0; o < size; o += 0x1000U) {
        uint64_t v = va + o;
        unsigned i2 = (unsigned)((v >> 21) & 0x1ffU);
        if (!(l2[i2] & 1U)) {
            if (map_tables_used >= SB_GPU_TBL_N)
                return 0;
            uint64_t t = SB_GPU_MEM + SB_GPU_TBL_POOL + (uint64_t)map_tables_used++ * 0x1000U;
            volatile uint64_t *z = (volatile uint64_t *)t;
            for (unsigned k = 0; k < 512U; ++k)
                z[k] = 0;
            l2[i2] = t | 3U;
        }
        volatile uint64_t *l3 = (volatile uint64_t *)(uintptr_t)(l2[i2] & ~0xfffULL);
        l3[(v >> 12) & 0x1ffU] = (pa + o) | 0x2c1U;
    }
    __asm__ volatile("dsb sy" : : : "memory");
    return 1;
}

static int as_wait(void)
{
    for (unsigned i = 0; i < 10000U; ++i) {
        if (!(sb_gpu_rd(AS0 + 0x28) & 1U))
            return 1;
        sb_udelay(1);
    }
    return 0;
}

int sb_gpu_as0(void)
{
    uint64_t t = SB_GPU_MEM | 7U;
    if (!gpu_open || !as_wait())
        return 0;
    (void)wr(AS0 + 0x00, (uint32_t)t);
    (void)wr(AS0 + 0x04, (uint32_t)(t >> 32));
    (void)wr(AS0 + 0x08, 0x888d88U);
    (void)wr(AS0 + 0x0c, 0);
    (void)wr(AS0 + 0x30, 0);
    (void)wr(AS0 + 0x34, 0);
    (void)wr(AS0 + 0x18, 1);                 /* UPDATE */
    return as_wait();
}

/* Reset, cores on, AS0 (the tables stay as they are). */
int sb_gpu_up(uint32_t shaders, unsigned us[4])
{
    unsigned r = 0, p[3] = { 0, 0, 0 };
    int ok = sb_gpu_reset(&r) && sb_gpu_power_on(shaders, p) && sb_gpu_as0();
    if (us) {
        us[0] = r;
        us[1] = p[0];
        us[2] = p[1];
        us[3] = p[2];
    }
    gpu_up = ok;
    return ok;
}

/* ---- jobs ------------------------------------------------------------------------------------ */

int sb_gpu_submit(unsigned js, uint64_t job_va, uint32_t affinity)
{
    unsigned s = 0x1800U + js * 0x80U;
    if (!gpu_up || js > 15)
        return 0;
    __asm__ volatile("dsb sy" : : : "memory");
    (void)wr(s + 0x40, (uint32_t)job_va);                   /* HEAD_NEXT */
    (void)wr(s + 0x44, (uint32_t)(job_va >> 32));
    (void)wr(s + 0x50, affinity);                           /* AFFINITY_NEXT */
    (void)wr(s + 0x54, 0);
    (void)wr(s + 0x58, 8U << 16 | 3U << 8 | 3U << 12);      /* CONFIG_NEXT: AS0, flushes, priority 8 */
    return wr(s + 0x60, 1);                                 /* COMMAND_NEXT: START */
}

/* Wait until every slot in js_mask finished (or one failed, or an MMU / GPU fault, or the time is
 * up); fill r. 1 when every slot finished without a fault. On failure the slots are hard-stopped
 * (the caller resets the GPU before the next job). */
int sb_gpu_wait(unsigned js_mask, uint64_t timeout_us, struct sb_gpu_result *r)
{
    uint64_t t0 = sb_ticks();
    uint32_t done = js_mask, fail = js_mask << 16, j = 0, m = 0, g = 0;
    int to = 0;
    for (;;) {
        j = sb_gpu_rd(JOB_IRQ_RAWSTAT);
        m = sb_gpu_rd(MMU_IRQ_RAWSTAT);
        g = sb_gpu_rd(GPU_IRQ_RAWSTAT);
        if ((j & done) == done || (j & fail) || m || (g & 0x81U))
            break;
        if (ticks_us(sb_ticks() - t0) >= timeout_us) {
            to = 1;
            break;
        }
#ifdef SB_HOST
        sb_udelay(1);
#endif
    }
    uint64_t t1 = sb_ticks();
    int ok = !to && (j & done) == done && !(j & fail) && !m && !(g & 0x81U);
    if (r) {
        r->ok = ok;
        r->timeout = to;
        r->us = ticks_us(t1 - t0);
        r->ticks = t1 - t0;
        r->jirq = j;
        r->mirq = m;
        r->girq = g;
        unsigned js = 0;
        while (js < 15 && !(js_mask & (1U << js)))
            ++js;
        if (!ok)                                /* the first slot that failed, else the first one */
            for (unsigned k = 0; k < 16; ++k)
                if ((js_mask & (1U << k)) && ((j & (1U << (k + 16))) || !(j & (1U << k)))) {
                    js = k;
                    break;
                }
        unsigned s = 0x1800U + js * 0x80U;
        r->js = js;
        r->js_status = sb_gpu_rd(s + 0x24);
        r->js_head = (uint64_t)sb_gpu_rd(s + 0x04) << 32 | sb_gpu_rd(s + 0x00);
        r->as_status = sb_gpu_rd(AS0 + 0x28);
        r->as_fault = sb_gpu_rd(AS0 + 0x1c);
        r->as_addr = (uint64_t)sb_gpu_rd(AS0 + 0x24) << 32 | sb_gpu_rd(AS0 + 0x20);
        r->gpu_fault = sb_gpu_rd(GPU_FAULTSTATUS);
        r->gpu_addr = (uint64_t)sb_gpu_rd(GPU_FAULTADDR_LO + 4) << 32 | sb_gpu_rd(GPU_FAULTADDR_LO);
    }
    if (!ok)
        for (unsigned k = 0; k < 16; ++k)
            if (js_mask & (1U << k))
                (void)wr(0x1800U + k * 0x80U + 0x20, 3);    /* JS_COMMAND: HARD_STOP */
    (void)wr(JOB_IRQ_CLEAR, 0xffffffffU);
    (void)wr(MMU_IRQ_CLEAR, 0xffffffffU);
    (void)wr(GPU_IRQ_CLEAR, 0x81U);
    return ok;
}

int sb_gpu_run(unsigned js, uint64_t job_va, uint32_t affinity, uint64_t timeout_us, struct sb_gpu_result *r)
{
    (void)wr(MMU_IRQ_CLEAR, 0xffffffffU);
    (void)wr(JOB_IRQ_CLEAR, 0xffffffffU);
    if (!sb_gpu_submit(js, job_va, affinity)) {
        if (r) {
            r->ok = 0;
            r->timeout = 0;
            r->us = 0;
            r->jirq = r->mirq = r->girq = r->js_status = r->as_status = r->as_fault = r->gpu_fault = 0;
            r->js = js;
            r->js_head = r->as_addr = r->gpu_addr = r->ticks = 0;
        }
        return 0;
    }
    return sb_gpu_wait(1U << js, timeout_us, r);
}

/* What a job's exception / fault status code means (Mali JM exception codes; the MMU fault
 * status's low byte is the same table). */
const char *sb_gpu_exc_name(uint32_t code)
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

/* Cycle counter on / off (GPU_COMMAND CYCLE_COUNT_START 5 / STOP 6). */
void sb_gpu_cycles(int on)
{
    (void)wr(GPU_COMMAND, on ? 5U : 6U);
}

uint64_t sb_gpu_cycle_count(void)
{
    uint32_t hi, lo, hi2;
    do {
        hi = sb_gpu_rd(0x94);
        lo = sb_gpu_rd(0x90);
        hi2 = sb_gpu_rd(0x94);
    } while (hi != hi2);
    return (uint64_t)hi << 32 | lo;
}

uint64_t sb_gpu_timestamp(void)
{
    uint32_t hi, lo, hi2;
    do {
        hi = sb_gpu_rd(0x9c);
        lo = sb_gpu_rd(0x98);
        hi2 = sb_gpu_rd(0x9c);
    } while (hi != hi2);
    return (uint64_t)hi << 32 | lo;
}
