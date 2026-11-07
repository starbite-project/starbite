/* SPDX-License-Identifier: GPL-2.0-only
 * The other seven cores as workers (sb.h "secondary cores as workers"; crt0.S sb_worker_entry).
 *
 * Built with -march=armv8-a (build.sh): sb_worker_main runs on the Mongoose M3 cores, which have
 * no ARMv8.1+ instructions. The boot core starts one core at a time: it writes the start record
 * sb_worker_boot (index, its own MAIR / TCR / TTBR0 / SCTLR, the core's stack), cleans it to DRAM
 * (the new core reads it with its MMU off), calls PSCI CPU_ON and waits for the core's check-in in
 * its slot before it starts the next one. Slots, the per-core work record and the stop flag are in
 * cacheable RAM; both sides also clean / invalidate them by line, so the protocol does not depend
 * on the clusters being coherent. A worker that faults records ESR / ELR / FAR and turns itself
 * off (sb_worker_vectors); the boot core's own state is never touched by a worker.
 * The M3 cluster needs the vendor PMU defaults before CPU_ON (xnu patch 0304, NOTES.md
 * 2026-10-28): that is a PMU write, so the application does it (sb_smp_m3_prep, after the user
 * confirmed it), not this file.
 */
#include "sb.h"

static struct sb_wslot slots[SB_NCORES] __attribute__((aligned(128)));
static struct {
    sb_work_fn fn;
    void *arg;
    uint64_t pad[6];
} work[SB_NCORES] __attribute__((aligned(64)));
static volatile uint64_t stop_line[8] __attribute__((aligned(64)));
static uintptr_t stacks[SB_NCORES];
sb_m3_prep_fn sb_smp_m3_prep;

#define STACK_SIZE (64U << 10)

extern char _image_end[];

static void line_civac(const volatile void *p, uint64_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63, end = (uintptr_t)p + n;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; a < end; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- the worker side --------------------------------------------------------------------- */

int sb_worker_stop(void)
{
    line_civac(stop_line, 64);
    return stop_line[0] != 0;
}

void sb_worker_publish(struct sb_wslot *s)
{
    s->beats++;
    line_civac(s, sizeof(*s));
}

void sb_worker_main(uint64_t w);
void sb_worker_main(uint64_t w)
{
    if (w == 0 || w >= SB_NCORES)
        return;
    struct sb_wslot *s = &slots[w];
    uint64_t mpidr;
    __asm__ volatile("msr tpidr_el1, %0" : : "r"(s) : "memory");
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    s->mpidr = mpidr;
    s->esr = s->elr = s->far = 0;
    s->state = SB_WK_RUNNING;
    line_civac(s, sizeof(*s));
    line_civac(&work[w], sizeof(work[w]));
    sb_work_fn fn = work[w].fn;
    if (fn)
        fn((unsigned)w, work[w].arg);
    s->state = SB_WK_DONE;
    line_civac(s, sizeof(*s));
}

/* ---- the boot core's side --------------------------------------------------------------------- */

uint64_t sb_smp_target(unsigned w)
{
    return w < 4 ? w : 0x100U + (w - 4U);
}

const struct sb_wslot *sb_smp_slot(unsigned w)
{
    if (w >= SB_NCORES)
        return 0;
    if (w)
        line_civac(&slots[w], sizeof(slots[w]));
    return &slots[w];
}

struct sb_wslot *sb_smp_own_slot(unsigned w)
{
    return w < SB_NCORES ? &slots[w] : 0;
}

int64_t sb_smp_start(unsigned w, sb_work_fn fn, void *arg)
{
    if (w == 0 || w >= SB_NCORES || !fn)
        return -2;
    line_civac(&slots[w], sizeof(slots[w]));
    if (slots[w].state == SB_WK_RUNNING)
        return -4;
    if (!stacks[w]) {
        void *p = sb_alloc(STACK_SIZE);
        if (!p)
            return -102;
        stacks[w] = (uintptr_t)p + STACK_SIZE;
    }
    work[w].fn = fn;
    work[w].arg = arg;
    line_civac(&work[w], sizeof(work[w]));
    /* The whole image (code, relocated GOT, .data, .bss with the work function's configuration)
     * to DRAM: a core outside the boot cluster then reads the same values even if the clusters
     * are not coherent yet. */
    line_civac((const void *)sb_boot.base, (uint64_t)((uintptr_t)_image_end - sb_boot.base));
    stop_line[0] = 0;
    line_civac(stop_line, 64);
    volatile uint64_t *z = (volatile uint64_t *)&slots[w];
    for (unsigned i = 0; i < sizeof(slots[w]) / 8; ++i)
        z[i] = 0;
    line_civac(&slots[w], sizeof(slots[w]));
    uint64_t mair, tcr, ttbr, sctlr;
    __asm__ volatile("mrs %0, mair_el1\n\tmrs %1, tcr_el1\n\tmrs %2, ttbr0_el1\n\tmrs %3, sctlr_el1"
                     : "=r"(mair), "=r"(tcr), "=r"(ttbr), "=r"(sctlr));
    sb_worker_boot[0] = w;
    sb_worker_boot[1] = mair;
    sb_worker_boot[2] = tcr;
    sb_worker_boot[3] = ttbr;
    sb_worker_boot[4] = sctlr;
    sb_worker_boot[5] = stacks[w];
    sb_worker_boot[6] = 0x52454b524f574253ULL;     /* "SBWORKER" */
    line_civac(sb_worker_boot, 64);
    if (w >= 4 && sb_smp_m3_prep)
        sb_smp_m3_prep();
    int64_t r = -5;
    for (unsigned a = 0; a < 20; ++a) {             /* ALREADY_ON / ON_PENDING: the last CPU_OFF is not done */
        r = sb_psci(SB_PSCI_CPU_ON, sb_smp_target(w), (uint64_t)(uintptr_t)sb_worker_entry, 0);
        if (r != -4 && r != -5)
            break;
        sb_mdelay(10);
    }
    if (r)
        return r;
    for (unsigned ms = 0; ms < 1000; ++ms) {
        line_civac(&slots[w], sizeof(slots[w]));
        if (slots[w].state != SB_WK_IDLE)
            return slots[w].state == SB_WK_FAULT ? -101 : 0;
        sb_mdelay(1);
    }
    return -100;
}

/* The vendor PMU defaults for the M3 cluster (sb.h; the values of engineer/lab.c m3_pmu). Plain MMIO to
 * those six PMU registers' fields only. */
static const struct {
    uint32_t off, mask, val;
} m3_pmu[] = { { 0x1088, 0xff000000U, 0x8d000000U }, { 0x2114, 3, 2 }, { 0x2180, 3, 2 }, { 0x21ec, 3, 2 },
               { 0x2258, 3, 2 }, { 0x2384, 3, 2 } };

int sb_m3_pmu_defaults(void)
{
    int ok = 1;
    for (unsigned i = 0; i < sizeof(m3_pmu) / sizeof(m3_pmu[0]); ++i) {
        uint64_t a = SB_PMU + m3_pmu[i].off;
        uint32_t v = 0;
        if (!sb_probe32(a, &v)) {
            ok = 0;
            continue;
        }
        if ((v & m3_pmu[i].mask) == m3_pmu[i].val)
            continue;
        *(volatile uint32_t *)a = (v & ~m3_pmu[i].mask) | m3_pmu[i].val;
        __asm__ volatile("dsb sy" ::: "memory");
        if (!sb_probe32(a, &v) || (v & m3_pmu[i].mask) != m3_pmu[i].val)
            ok = 0;
    }
    sb_log(ok ? "m3 pmu defaults: set" : "m3 pmu defaults: a register did not read back");
    return ok;
}

int64_t sb_smp_affinity(unsigned w)
{
    if (w >= SB_NCORES)
        return -2;
    /* PSCI answers in w0: on this phone an unknown function (PSCI 0.1 has no AFFINITY_INFO) comes back as
     * 0xffffffff with the upper half zero, which a 64-bit read takes for a large positive number */
    return (int32_t)sb_psci(0xc4000004U, sb_smp_target(w), 0, 0);
}

unsigned sb_smp_running(void)
{
    unsigned n = 0;
    for (unsigned w = 1; w < SB_NCORES; ++w) {
        line_civac(&slots[w], sizeof(slots[w]));
        n += slots[w].state == SB_WK_RUNNING;
    }
    return n;
}

unsigned sb_smp_stop(unsigned ms)
{
    stop_line[0] = 1;
    line_civac(stop_line, 64);
    uint64_t t0 = sb_ms();
    unsigned left;
    while ((left = sb_smp_running()) && sb_ms() - t0 < ms)
        sb_mdelay(2);
    return left;
}
