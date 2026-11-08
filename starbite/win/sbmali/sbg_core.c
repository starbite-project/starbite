/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite GPU scheduler core: see sbg_core.h. Register use as sbm_core.c (Linux panfrost_regs.h names).
 */
#include "sbg_core.h"

#define GPU_IRQ_RAWSTAT 0x020U
#define GPU_IRQ_CLEAR 0x024U
#define GPU_IRQ_MASK 0x028U
#define GPU_FAULTSTATUS 0x03cU
#define JOB_IRQ_RAWSTAT 0x1000U
#define JOB_IRQ_CLEAR 0x1004U
#define JOB_IRQ_MASK 0x1008U
#define MMU_IRQ_RAWSTAT 0x2000U
#define MMU_IRQ_CLEAR 0x2004U
#define MMU_IRQ_MASK 0x2008U
#define JS(n) (0x1800U + (n) * 0x80U)
#define JS_COMMAND 0x20U
#define JS_STATUS 0x24U
#define JS_HEAD_NEXT 0x40U
#define JS_AFFINITY_NEXT 0x50U
#define JS_CONFIG_NEXT 0x58U
#define JS_COMMAND_NEXT 0x60U
#define AS0_FAULTSTATUS (0x2400U + 0x1cU)
#define AS0_FAULTADDR (0x2400U + 0x20U)

#define SLOTS_MASK 0x7U                   /* job slots 0..2 */
#define GPU_IRQ_FAULTS 0x81U              /* GPU_FAULT | MULTIPLE_GPU_FAULTS */

static sbm_u32 rd(struct sbg *g, unsigned off) { return g->io->rd(g->io->ctx, SBM_GPU, off); }
static void wr(struct sbg *g, unsigned off, sbm_u32 v) { g->io->wr(g->io->ctx, SBM_GPU, off, v); }
static void barrier(struct sbg *g)
{
    if (g->io->barrier)
        g->io->barrier(g->io->ctx);     /* the I/O layer's DSB, never this helper (T12 shipped a self-call) */
}

static void arm_irqs(struct sbg *g)
{
    wr(g, JOB_IRQ_CLEAR, 0xffffffffU);
    wr(g, MMU_IRQ_CLEAR, 0xffffffffU);
    wr(g, GPU_IRQ_CLEAR, GPU_IRQ_FAULTS);
    wr(g, JOB_IRQ_MASK, SLOTS_MASK | SLOTS_MASK << 16);   /* done and failed, slots 0..2 */
    wr(g, MMU_IRQ_MASK, 1U);                              /* AS0 page fault */
    wr(g, GPU_IRQ_MASK, GPU_IRQ_FAULTS);
}

/* Soft reset, cores, AS0 (its tables are kept: every mapping survives), interrupts. 0 = back. */
static int recover(struct sbg *g)
{
    unsigned us[3];
    g->resets++;
    if (!sbm_soft_reset(g->io, 0) || !sbm_power_on(g->io, g->shaders, us) || sbm_as_enable(g->io, 0, &g->pt)) {
        g->reset_fail++;
        return -1;
    }
    arm_irqs(g);
    return 0;
}

int sbg_init(struct sbg *g, const struct sbm_io *io, enum sbm_fmt fmt, sbm_u64 shaders, volatile sbm_u64 *pt_cpu,
             sbm_u64 pt_pa, unsigned pt_tables)
{
    unsigned char *p = (unsigned char *)g;
    for (unsigned i = 0; i < sizeof(*g); ++i)
        p[i] = 0;
    g->io = io;
    g->shaders = shaders;
    g->timeout_us = SBG_TIMEOUT_US;
    sbm_pt_init(&g->pt, fmt, pt_cpu, pt_pa, pt_tables);
    if (recover(g))
        return -1;
    g->resets = 0;
    return 0;
}

/* ---- buffer objects ------------------------------------------------------------------------- */

static int overlaps(const struct sbg *g, sbm_u64 va, sbm_u64 size)
{
    for (unsigned i = 0; i < SBG_MAX_BOS; ++i)
        if (g->bo[i].used && va < g->bo[i].va + g->bo[i].size && g->bo[i].va < va + size)
            return 1;
    return 0;
}

/* First fit in [SBG_VA_BASE, SBG_VA_END): try the window start and the end of every live buffer. */
static sbm_u64 find_va(const struct sbg *g, sbm_u64 size, sbm_u64 align)
{
    sbm_u64 best = 0;
    for (int i = -1; i < (int)SBG_MAX_BOS; ++i) {
        sbm_u64 c;
        if (i >= 0 && !g->bo[i].used)
            continue;
        c = i < 0 ? SBG_VA_BASE : g->bo[i].va + g->bo[i].size;
        c = (c + align - 1) & ~(align - 1);
        if (c + size > SBG_VA_END || c + size < c || overlaps(g, c, size))
            continue;
        if (!best || c < best)
            best = c;
    }
    return best;
}

int sbg_bo_create(struct sbg *g, const sbm_u64 *pa, unsigned npages, unsigned flags, sbm_u64 *va)
{
    sbm_u64 size = (sbm_u64)npages * 0x1000U, at;
    int h = -1;
    if (!npages || !va)
        return -1;
    for (unsigned i = 0; i < SBG_MAX_BOS && h < 0; ++i)
        if (!g->bo[i].used)
            h = (int)i;
    if (h < 0)
        return -1;
    if (flags & SBG_BO_FIXED) {
        at = *va;
        if ((at & 0xfffU) || at < SBG_VA_BASE || at + size > SBG_VA_END || overlaps(g, at, size))
            return -1;
    } else {
        at = find_va(g, size, size > 0x10000U ? 0x10000U : 0x1000U);
        if (!at)
            return -1;
    }
    for (unsigned i = 0; i < npages; ++i)
        if (sbm_pt_map(&g->pt, at + (sbm_u64)i * 0x1000U, pa[i], 1, (flags & SBG_BO_NOEXEC) != 0)) {
            sbm_pt_unmap(&g->pt, at, i);
            return -1;
        }
    barrier(g);
    (void)sbm_as_flush(g->io, 0, at, size);   /* as Panfrost after a map: no stale walk survives */
    g->bo[h].va = at;
    g->bo[h].size = size;
    g->bo[h].flags = flags;
    g->bo[h].release_pending = 0;
    g->bo[h].used = 1;
    *va = at;
    return h;
}

static void unmap_bo(struct sbg *g, int h)
{
    sbm_pt_unmap(&g->pt, g->bo[h].va, (unsigned)(g->bo[h].size / 0x1000U));
    barrier(g);
    (void)sbm_as_flush(g->io, 0, g->bo[h].va, g->bo[h].size);   /* before the caller reuses the pages */
    g->bo[h].used = 0;
    g->bo[h].release_pending = 0;
}

static void release_due(struct sbg *g)
{
    for (unsigned i = 0; i < SBG_MAX_BOS; ++i)
        if (g->bo[i].used && g->bo[i].release_pending && g->bo[i].release_after <= g->done_seq)
            unmap_bo(g, (int)i);
}

int sbg_bo_release(struct sbg *g, int handle, sbm_u64 after_seq)
{
    if (handle < 0 || handle >= (int)SBG_MAX_BOS || !g->bo[handle].used || g->bo[handle].release_pending)
        return -1;
    if (after_seq <= g->done_seq) {
        unmap_bo(g, handle);
        return 0;
    }
    g->bo[handle].release_after = after_seq;
    g->bo[handle].release_pending = 1;
    return 0;
}

int sbg_bo_find(const struct sbg *g, sbm_u64 va)
{
    for (unsigned i = 0; i < SBG_MAX_BOS; ++i)
        if (g->bo[i].used && va >= g->bo[i].va && va < g->bo[i].va + g->bo[i].size)
            return (int)i;
    return -1;
}

/* ---- submission ------------------------------------------------------------------------------ */

static void note_bad(struct sbg *g, const struct sbg_job *j, enum sbg_status st)
{
    g->bad.seq = j->seq;
    g->bad.ctx = j->ctx;
    g->bad.slot = j->slot;
    g->bad.status = st;
}

/* Completion is in order and one chain runs at a time, so finishing `seq` completes everything up to it. */
static void complete(struct sbg *g, sbm_u64 seq)
{
    if (seq > g->done_seq)
        g->done_seq = seq;
    release_due(g);
}

static void start_next(struct sbg *g, sbm_u64 now_us)
{
    while (!g->busy && g->qhead != g->qtail) {
        struct sbg_job j = g->q[g->qhead & (SBG_QUEUE - 1U)];
        g->qhead++;
        if (g->guilty & (1U << j.ctx)) {  /* device lost: never run */
            g->skipped++;
            complete(g, j.seq);
            continue;
        }
        g->cur = j;
        g->busy = 1;
        g->cur_start_us = now_us;
        g->starts++;
        barrier(g);                       /* the chain's descriptors visible to the GPU */
        wr(g, JS(j.slot) + JS_HEAD_NEXT, (sbm_u32)j.head);
        wr(g, JS(j.slot) + JS_HEAD_NEXT + 4U, (sbm_u32)(j.head >> 32));
        wr(g, JS(j.slot) + JS_AFFINITY_NEXT, (sbm_u32)g->shaders);
        wr(g, JS(j.slot) + JS_AFFINITY_NEXT + 4U, (sbm_u32)(g->shaders >> 32));
        wr(g, JS(j.slot) + JS_CONFIG_NEXT, 8U << 16 | 3U << 12 | 3U << 8 | 0U);   /* flush both ends, AS0 */
        wr(g, JS(j.slot) + JS_COMMAND_NEXT, 1U);                                 /* START */
    }
}

int sbg_submit(struct sbg *g, unsigned slot, sbm_u64 head, unsigned ctx, sbm_u64 now_us, sbm_u64 *seq)
{
    struct sbg_job *j;
    if (slot > 2U || ctx >= SBG_MAX_CTX || !head || !seq || g->qtail - g->qhead >= SBG_QUEUE)
        return -1;
    j = &g->q[g->qtail & (SBG_QUEUE - 1U)];
    j->head = head;
    j->slot = slot;
    j->ctx = ctx;
    j->seq = ++g->next_seq;
    g->qtail++;
    *seq = j->seq;
    start_next(g, now_us);
    return 0;
}

/* The running chain failed or hung: stop it, reset, complete it with st, its context guilty. */
static void fail_cur(struct sbg *g, enum sbg_status st, sbm_u32 jirq, sbm_u32 mirq, sbm_u32 girq)
{
    note_bad(g, &g->cur, st);
    g->bad.js_status = rd(g, JS(g->cur.slot) + JS_STATUS);
    g->bad.as_fault = rd(g, AS0_FAULTSTATUS);
    g->bad.fault_addr = (sbm_u64)rd(g, AS0_FAULTADDR + 4U) << 32 | rd(g, AS0_FAULTADDR);
    g->bad.gpu_fault = rd(g, GPU_FAULTSTATUS);
    g->bad.mmu_irq = mirq;
    (void)jirq;
    (void)girq;
    wr(g, JS(g->cur.slot) + JS_COMMAND, 3U);      /* HARD_STOP */
    g->guilty |= 1U << g->cur.ctx;
    if (st == SBG_TIMEOUT)
        g->timeouts++;
    else
        g->faults++;
    g->busy = 0;
    (void)recover(g);
    complete(g, g->cur.seq);
}

int sbg_irq(struct sbg *g, sbm_u64 now_us)
{
    sbm_u32 j, m, gi, done, fail;
    if (!g->busy) {
        start_next(g, now_us);
        return 0;
    }
    j = rd(g, JOB_IRQ_RAWSTAT);
    m = rd(g, MMU_IRQ_RAWSTAT);
    gi = rd(g, GPU_IRQ_RAWSTAT);
    done = 1U << g->cur.slot;
    fail = 1U << (g->cur.slot + 16U);
    if (m || (gi & GPU_IRQ_FAULTS) || (j & fail)) {
        fail_cur(g, SBG_FAULT, j, m, gi);
    } else if (j & done) {
        wr(g, JOB_IRQ_CLEAR, done);
        g->busy = 0;
        complete(g, g->cur.seq);
    } else {
        return 0;                         /* not ours / not yet */
    }
    start_next(g, now_us);
    return 1;
}

int sbg_tick(struct sbg *g, sbm_u64 now_us)
{
    if (!g->busy || now_us - g->cur_start_us < g->timeout_us)
        return 0;
    if (sbg_irq(g, now_us))               /* it ended just now */
        return 0;
    fail_cur(g, SBG_TIMEOUT, rd(g, JOB_IRQ_RAWSTAT), 0, 0);
    start_next(g, now_us);
    return 1;
}

void sbg_ctx_reset(struct sbg *g, unsigned ctx)
{
    if (ctx < SBG_MAX_CTX)
        g->guilty &= ~(1U << ctx);
}

void sbg_fini(struct sbg *g)
{
    if (g->busy)
        wr(g, JS(g->cur.slot) + JS_COMMAND, 3U);
    g->busy = 0;
    while (g->qhead != g->qtail) {
        g->skipped++;
        g->qhead++;
    }
    complete(g, g->next_seq);
    sbm_as_disable(g->io, 0);
    (void)sbm_soft_reset(g->io, 0);
    sbm_power_off(g->io);
}
