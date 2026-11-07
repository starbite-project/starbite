/* SPDX-License-Identifier: GPL-2.0-only
 * Menu v7: the other three Cortex-A55 cores as row workers for the renderer (docs/BOOT-MENU-OVERNIGHT.md).
 *
 * The loader runs on A55 core 0 at S-Boot's clock (~1.05 GHz); every v7 frame is per-pixel work over
 * rows that do not depend on each other (the present's 2x, the glass rows, the wallpaper). g7_par
 * splits [y0, y1) into chunks that core 0 and up to three workers take in turn (an atomic counter),
 * and returns when every chunk is done.
 *
 * Workers (PSCI 0.1 CPU_ON 0xc4000003, targets 1..3 as em.c's core test measured 2026-10-25) start in
 * g7_worker_entry with the MMU off, read their start record (cleaned to DRAM by core 0), take core 0's
 * MAIR / TCR / TTBR0 / SCTLR (the loader's identity map, mmu.c) and their own stack and vectors, and
 * wait for jobs with WFE. The A55s are one coherent cluster, so after the MMU is on the job record
 * needs no cache maintenance. A worker that faults records ESR / ELR / FAR and powers itself off;
 * core 0 then stops using workers for good and runs the chunks still open itself.
 *
 * g7_smp_stop() asks every worker to leave and waits until each has called PSCI CPU_OFF (it says so in
 * its slot right before the SMC). menu7 calls it before menu7_main returns and before Lab: no OS and
 * no other code that starts cores ever finds one of ours on. A worker whose CPU_OFF returned is
 * reported (log slot 157 bit 31) and stays in WFE.
 *
 * Only core 0 polls the keys (g7_poll): poll_fn reads GPIOs and services the background USB link.
 *
 * Host preview (M7_PREVIEW): the same API on pthreads, H7_CORES=n (default 4), so h7 checks that a
 * frame on n cores is byte-identical to one core.
 */
#include <stdint.h>
#include "glass.h"
#include "glass_int.h"

#define NW G7_NCORES

int g7_smp_on;                       /* cores running jobs now (1 = core 0 only) */
static int smp_broken;               /* a worker faulted or timed out: core 0 alone from then on */
uint64_t g7_smp_stat[4];             /* jobs, chunks run by workers, worker faults, CPU_OFF returned */

struct job {
    g7_rows_fn fn;
    void *ctx;
    int y0, y1, grain, nchunk;
    uint32_t next;                   /* the next chunk to take (atomic) */
    uint32_t done;                   /* chunks finished (atomic) */
    uint64_t gen;                    /* bumped for every job */
    uint32_t stop;                   /* workers leave */
} __attribute__((aligned(64)));

static struct job J;

static void run_chunks(int core)
{
    for (;;) {
        uint32_t c = __atomic_fetch_add(&J.next, 1U, __ATOMIC_ACQ_REL);
        if ((int)c >= J.nchunk)
            return;
        int a = J.y0 + (int)c * J.grain, b = a + J.grain;
        if (b > J.y1)
            b = J.y1;
        J.fn(J.ctx, a, b, core);
        if (core)
            g7_smp_stat[1]++;        /* (a race between workers only loses counts) */
        __atomic_fetch_add(&J.done, 1U, __ATOMIC_ACQ_REL);
    }
}

#ifndef M7_PREVIEW
/* ================================================================================================
 * the phone
 * ============================================================================================== */

struct wslot {
    volatile uint64_t state, mpidr, esr, elr, far, offret, pad[2];
} __attribute__((aligned(64)));
enum { WS_OFF, WS_STARTING, WS_IDLE, WS_FAULT, WS_LEAVING, WS_PARKED };

#define WSTACK (48U << 10)
static uint8_t wstack[NW][WSTACK] __attribute__((aligned(64)));
static struct wslot WS[NW];
/* start record, read with the MMU off: core, MAIR, TCR, TTBR0, SCTLR, stack top, slot, magic */
uint64_t g7_wboot[8] __attribute__((aligned(64)));
static uint64_t boot_mpidr;
static int started[NW];

extern char g7_worker_entry[];
void g7_worker_main(uint64_t w);

__asm__(
    ".text\n"
    ".balign 64\n"
    ".global g7_worker_entry\n"
    "g7_worker_entry:\n"
    "    msr daifset, #15\n"
    "    adrp x9, g7_wboot\n"
    "    add x9, x9, :lo12:g7_wboot\n"
    "    ldr x19, [x9]\n"                    /* core index */
    "    ldr x20, [x9, #48]\n"               /* its slot */
    "    adr x10, g7_worker_vectors\n"
    "    msr vbar_el1, x10\n"
    "    mrs x10, cpacr_el1\n"
    "    orr x10, x10, #(3 << 20)\n"
    "    msr cpacr_el1, x10\n"
    "    ldr x10, [x9, #40]\n"
    "    mov sp, x10\n"
    "    ldr x10, [x9, #8]\n"
    "    msr mair_el1, x10\n"
    "    ldr x10, [x9, #16]\n"
    "    msr tcr_el1, x10\n"
    "    ldr x10, [x9, #24]\n"
    "    msr ttbr0_el1, x10\n"
    "    isb\n"
    "    tlbi vmalle1\n"
    "    ic iallu\n"
    "    dsb sy\n"
    "    isb\n"
    "    ldr x10, [x9, #32]\n"
    "    msr sctlr_el1, x10\n"
    "    isb\n"
    "    msr tpidr_el1, x20\n"
    "    mov x0, x19\n"
    "    bl g7_worker_main\n"
    /* leaving: slot state WS_LEAVING was written by g7_worker_main; this core's own cache levels
     * written back, then CPU_OFF (does not return) */
    "    bl g7_dcache_private\n"
    "    mrs x20, tpidr_el1\n"
    "    movz w0, #0x8400, lsl #16\n"
    "    movk w0, #0x0002\n"
    "    smc #0\n"
    "    str x0, [x20, #40]\n"
    "    mov x1, #5\n"                       /* WS_PARKED: CPU_OFF returned */
    "    str x1, [x20]\n"
    "    dsb sy\n"
    "1:  wfe\n"
    "    b 1b\n"
    ".balign 2048\n"
    "g7_worker_vectors:\n"
    "    .rept 16\n"
    "    .balign 0x80\n"
    "    b 2f\n"
    "    .endr\n"
    "2:  mrs x0, tpidr_el1\n"
    "    cbz x0, 3f\n"
    "    mrs x1, esr_el1\n"
    "    str x1, [x0, #16]\n"
    "    mrs x1, elr_el1\n"
    "    str x1, [x0, #24]\n"
    "    mrs x1, far_el1\n"
    "    str x1, [x0, #32]\n"
    "    mov x1, #3\n"                       /* WS_FAULT */
    "    str x1, [x0]\n"
    "    dsb sy\n"
    "    sev\n"
    "    bl g7_dcache_private\n"
    "3:  movz w0, #0x8400, lsl #16\n"
    "    movk w0, #0x0002\n"
    "    smc #0\n"
    "4:  wfe\n"
    "    b 4b\n");

/* g7_dcache_private: clean + invalidate by set/way every data cache level of this core below the
 * last one before the point of coherency (on the A55: its private L1 and L2; the cluster's shared
 * last level stays, core 0 keeps using it), so nothing this worker wrote is only in caches that
 * power off with it. Registers only (x0-x11), no stack. */
__asm__(
    ".text\n"
    ".balign 16\n"
    ".global g7_dcache_private\n"
    "g7_dcache_private:\n"
    "    dsb sy\n"
    "    mrs x0, clidr_el1\n"
    "    ubfx x3, x0, #24, #3\n"             /* LoC */
    "    sub x3, x3, #1\n"                   /* levels 1 .. LoC - 1 */
    "    lsl x3, x3, #1\n"                   /* as level x 2 (CSSELR) */
    "    cmp x3, #0\n"
    "    b.le 5f\n"
    "    mov x10, #0\n"
    "1:  add x2, x10, x10, lsr #1\n"         /* level x 3: its Ctype field */
    "    lsr x1, x0, x2\n"
    "    and x1, x1, #7\n"
    "    cmp x1, #2\n"
    "    b.lt 4f\n"                          /* no data cache at this level */
    "    msr csselr_el1, x10\n"
    "    isb\n"
    "    mrs x1, ccsidr_el1\n"
    "    and x2, x1, #7\n"
    "    add x2, x2, #4\n"                   /* log2 line bytes */
    "    ubfx x4, x1, #3, #10\n"             /* ways - 1 */
    "    clz w5, w4\n"                       /* way shift */
    "    ubfx x7, x1, #13, #15\n"            /* sets - 1 */
    "2:  mov x9, x4\n"
    "3:  lsl x6, x9, x5\n"
    "    orr x11, x10, x6\n"
    "    lsl x6, x7, x2\n"
    "    orr x11, x11, x6\n"
    "    dc cisw, x11\n"
    "    subs x9, x9, #1\n"
    "    b.ge 3b\n"
    "    subs x7, x7, #1\n"
    "    b.ge 2b\n"
    "4:  add x10, x10, #2\n"
    "    cmp x3, x10\n"
    "    b.gt 1b\n"
    "5:  dsb sy\n"
    "    isb\n"
    "    ret\n");

static int64_t psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
                       "x17", "memory");
    return (int64_t)(int32_t)x0;     /* PSCI 0.1 answers in w0 */
}

static void clean_range(const volatile void *p, uint64_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63, end = (uintptr_t)p + n;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; a < end; a += 64)
        __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t ms_now(void)
{
    uint64_t f = hw_freq() ? hw_freq() : 26000000U;
    return hw_ticks() * 1000U / f;
}

void g7_worker_main(uint64_t w)
{
    struct wslot *s = &WS[w];
    uint64_t mpidr, seen;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    s->mpidr = mpidr;
    seen = __atomic_load_n(&J.gen, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s->state, (uint64_t)WS_IDLE, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish\n sev" ::: "memory");
    for (;;) {
        while (__atomic_load_n(&J.gen, __ATOMIC_ACQUIRE) == seen && !__atomic_load_n(&J.stop, __ATOMIC_ACQUIRE))
            __asm__ volatile("wfe" ::: "memory");
        if (__atomic_load_n(&J.stop, __ATOMIC_ACQUIRE))
            break;
        seen = __atomic_load_n(&J.gen, __ATOMIC_ACQUIRE);
        run_chunks((int)w);
        __asm__ volatile("dsb ish\n sev" ::: "memory");
    }
    __atomic_store_n(&s->state, (uint64_t)WS_LEAVING, __ATOMIC_RELEASE);
    __asm__ volatile("dsb sy\n sev" ::: "memory");
}

int g7_is_core0(void)
{
    uint64_t m;
    if (g7_smp_on <= 1)
        return 1;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m));
    return (m & 0xffffffU) == (boot_mpidr & 0xffffffU);
}

int g7_smp_start(int want)
{
    if (smp_broken || want <= 1)
        return g7_smp_on = 1;
    if (g7_smp_on > 1)
        return g7_smp_on;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(boot_mpidr));
    if (boot_mpidr & 0xffff00U)      /* the loader is not on A55 core 0: leave the cores alone */
        return g7_smp_on = 1;
    uint64_t mair, tcr, ttbr, sctlr;
    __asm__ volatile("mrs %0, mair_el1\n mrs %1, tcr_el1\n mrs %2, ttbr0_el1\n mrs %3, sctlr_el1"
                     : "=r"(mair), "=r"(tcr), "=r"(ttbr), "=r"(sctlr));
    if (!(sctlr & 1))                /* no MMU, no caches: workers would not be coherent */
        return g7_smp_on = 1;
    __atomic_store_n(&J.stop, 0U, __ATOMIC_RELEASE);
    int n = 1;
    if (want > NW)
        want = NW;
    for (int w = 1; w < want; ++w) {
        struct wslot *s = &WS[w];
        s->state = WS_STARTING;
        s->esr = s->elr = s->far = s->offret = 0;
        g7_wboot[0] = (uint64_t)w;
        g7_wboot[1] = mair;
        g7_wboot[2] = tcr;
        g7_wboot[3] = ttbr;
        g7_wboot[4] = sctlr;
        g7_wboot[5] = (uint64_t)(uintptr_t)(wstack[w] + WSTACK);
        g7_wboot[6] = (uint64_t)(uintptr_t)s;
        g7_wboot[7] = 0x37574758454e494dULL;
        clean_range(g7_wboot, sizeof(g7_wboot));
        clean_range(s, sizeof(*s));
        clean_range(&J, sizeof(J));
        int64_t r = -5;
        for (int a = 0; a < 10; ++a) {   /* ALREADY_ON / ON_PENDING: a CPU_OFF still finishing */
            r = psci(0xc4000003U, (uint64_t)w, (uint64_t)(uintptr_t)g7_worker_entry, 0);
            if (r != -4 && r != -5)
                break;
            hw_wait_us(2000);
        }
        if (r) {
            s->state = WS_OFF;
            break;
        }
        uint64_t t0 = ms_now();
        while (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == WS_STARTING && ms_now() - t0 < 100)
            hw_wait_us(50);
        if (s->state != WS_IDLE) {
            /* it never took its start record (or faulted): it is somewhere we cannot reach */
            smp_broken = 1;
            g7_smp_stat[2]++;
            break;
        }
        started[w] = 1;
        n = w + 1;
    }
    g7_smp_on = n;
    return n;
}

int g7_smp_stop(void)
{
    if (g7_smp_on <= 1 && !started[1] && !started[2] && !started[3])
        return 0;
    __atomic_store_n(&J.stop, 1U, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish\n sev" ::: "memory");
    int bad = 0;
    for (int w = 1; w < NW; ++w) {
        if (!started[w])
            continue;
        uint64_t t0 = ms_now();
        while (__atomic_load_n(&WS[w].state, __ATOMIC_ACQUIRE) != WS_LEAVING &&
               __atomic_load_n(&WS[w].state, __ATOMIC_ACQUIRE) != WS_FAULT && ms_now() - t0 < 50)
            hw_wait_us(50);
        /* CPU_OFF itself: give it a moment, then see whether it came back */
        hw_wait_us(2000);
        if (WS[w].state == WS_PARKED || (WS[w].state != WS_LEAVING && WS[w].state != WS_FAULT)) {
            bad |= 1 << w;
            g7_smp_stat[3]++;
        }
        started[w] = 0;
    }
    g7_smp_on = 1;
    if (bad)
        smp_broken = 1;              /* a core we could not turn off: never start more */
    return bad;
}

uint64_t g7_smp_report(void)
{
    uint64_t v = (uint64_t)g7_smp_on | (uint64_t)smp_broken << 4;
    for (int w = 1; w < NW; ++w)
        v |= (uint64_t)(WS[w].state & 15U) << (8 + 4 * w);
    v |= (g7_smp_stat[0] & 0xffffffU) << 24;
    v |= (g7_smp_stat[3] ? 1ULL : 0ULL) << 63;
    return v;
}

const uint64_t *g7_smp_fault(int w) { return (const uint64_t *)&WS[w & 3]; }

void g7_par(g7_rows_fn fn, void *ctx, int y0, int y1, int grain)
{
    if (y1 <= y0)
        return;
    if (grain < 1)
        grain = 1;
    int nchunk = (y1 - y0 + grain - 1) / grain;
    if (g7_smp_on <= 1 || nchunk < 2) {
        fn(ctx, y0, y1, 0);
        return;
    }
    J.fn = fn;
    J.ctx = ctx;
    J.y0 = y0;
    J.y1 = y1;
    J.grain = grain;
    J.nchunk = nchunk;
    __atomic_store_n(&J.done, 0U, __ATOMIC_RELAXED);
    /* release: a worker still leaving the last job that takes chunk 0 here sees the new fields */
    __atomic_store_n(&J.next, 0U, __ATOMIC_RELEASE);
    __atomic_fetch_add(&J.gen, 1U, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish\n sev" ::: "memory");
    g7_smp_stat[0]++;
    run_chunks(0);
    uint64_t t0 = 0;
    unsigned spins = 0;
    while (__atomic_load_n(&J.done, __ATOMIC_ACQUIRE) < (uint32_t)nchunk) {
        int faulted = 0;
        for (int w = 1; w < g7_smp_on; ++w)
            faulted |= WS[w].state == WS_FAULT;
        if (++spins == 64)
            t0 = ms_now();
        if (faulted || (spins > 64 && ms_now() - t0 > 400)) {
            /* a worker died with a chunk: core 0 alone from now on. The lost chunks cannot be
             * told apart from finished ones, so the whole job runs again on core 0 (each fn writes
             * only its own rows: running a chunk twice gives the same pixels). */
            smp_broken = 1;
            g7_smp_stat[2]++;
            g7_smp_on = 1;
            fn(ctx, y0, y1, 0);
            return;
        }
        __asm__ volatile("wfe" ::: "memory");
    }
}

#else
/* ================================================================================================
 * the host preview: pthreads
 * ============================================================================================== */
#include <pthread.h>
#include <stdlib.h>

static pthread_t TH[NW];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER, cv_done = PTHREAD_COND_INITIALIZER;
__thread int g7_cur_core;
#define my_core g7_cur_core
static int nthreads;

int g7_is_core0(void) { return my_core == 0; }

static void *worker(void *arg)
{
    int w = (int)(intptr_t)arg;
    my_core = w;
    uint64_t seen = 0;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (J.gen == seen && !J.stop)
            pthread_cond_wait(&cv, &mu);
        if (J.stop)
            break;
        seen = J.gen;
        pthread_mutex_unlock(&mu);
        run_chunks(w);
        pthread_mutex_lock(&mu);
        pthread_cond_broadcast(&cv_done);
    }
    pthread_mutex_unlock(&mu);
    return 0;
}

int g7_smp_start(int want)
{
    const char *e = getenv("H7_CORES");
    if (e)
        want = atoi(e);
    if (want > NW)
        want = NW;
    if (want <= 1 || smp_broken)
        return g7_smp_on = 1;
    if (g7_smp_on > 1)
        return g7_smp_on;
    J.stop = 0;
    for (int w = 1; w < want; ++w)
        pthread_create(&TH[w], 0, worker, (void *)(intptr_t)w);
    nthreads = want;
    return g7_smp_on = want;
}

int g7_smp_stop(void)
{
    if (g7_smp_on <= 1)
        return 0;
    pthread_mutex_lock(&mu);
    J.stop = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    for (int w = 1; w < nthreads; ++w)
        pthread_join(TH[w], 0);
    g7_smp_on = 1;
    return 0;
}

uint64_t g7_smp_report(void) { return (uint64_t)g7_smp_on; }
const uint64_t *g7_smp_fault(int w) { (void)w; return g7_smp_stat; }

void g7_par(g7_rows_fn fn, void *ctx, int y0, int y1, int grain)
{
    if (y1 <= y0)
        return;
    if (grain < 1)
        grain = 1;
    int nchunk = (y1 - y0 + grain - 1) / grain;
    if (g7_smp_on <= 1 || nchunk < 2) {
        fn(ctx, y0, y1, 0);
        return;
    }
    pthread_mutex_lock(&mu);
    J.fn = fn;
    J.ctx = ctx;
    J.y0 = y0;
    J.y1 = y1;
    J.grain = grain;
    J.nchunk = nchunk;
    J.done = 0;
    __atomic_store_n(&J.next, 0U, __ATOMIC_RELEASE);
    J.gen++;
    g7_smp_stat[0]++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    run_chunks(0);
    pthread_mutex_lock(&mu);
    while (__atomic_load_n(&J.done, __ATOMIC_ACQUIRE) < (uint32_t)nchunk)
        pthread_cond_wait(&cv_done, &mu);
    pthread_mutex_unlock(&mu);
}
#endif
