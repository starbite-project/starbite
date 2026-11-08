/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite GPU scheduler core (stage 4, docs/WIN11-GPU.md section 6.4): the kernel half that both a WDDM
 * render driver (sbwddm) and the stage-4 test IOCTL device sit on. No OS calls; host-tested against the
 * fake Mali-G72 in tests/mali_test.c.
 *
 * What it owns, in the shape of Linux Panfrost (drivers/gpu/drm/panfrost) so Mesa's Panfrost needs no new
 * model of the hardware:
 *   - buffer objects: caller-supplied 4 KiB pages mapped at a GPU address the core chooses in
 *     [32 MiB, 4 GiB) (Panfrost's window), or at a fixed one; one address space (AS0) for now;
 *   - submissions: one job chain per submit on job slot 0 (fragment), 1 (vertex/tiler) or 2 (compute),
 *     completed strictly in submission order with a 64-bit fence sequence number (WDDM 1.x: one node,
 *     in-order fences). One chain on the GPU at a time (no slot overlap yet);
 *   - completion from the job/MMU/GPU interrupt (sbg_irq) or by polling, timeouts (sbg_tick: hard stop,
 *     soft reset, AS0 and the cores back, the queue continues), MMU and job faults (the submitting context
 *     is marked guilty; its queued and later submits complete without running: "device lost");
 *   - deferred release: a buffer is unmapped only when the fence it was last used under has completed.
 *
 * Locking is the caller's: every call (including sbg_irq) under one lock (a spinlock at the ISR's level
 * on Windows). Nothing here sleeps; sbm_soft_reset/sbm_power_on poll with delay_us.
 */
#ifndef SBG_CORE_H
#define SBG_CORE_H

#include "sbm_core.h"

#define SBG_VA_BASE 0x02000000ULL         /* 32 MiB .. 4 GiB, as Panfrost's drm_mm range */
#define SBG_VA_END 0x100000000ULL
#define SBG_MAX_BOS 1024U
#define SBG_QUEUE 64U                     /* submits waiting (power of two) */
#define SBG_MAX_CTX 32U
#define SBG_TIMEOUT_US 2000000ULL         /* WDDM's default TDR delay */

#define SBG_BO_NOEXEC 1U                  /* data: never executable by the GPU */
#define SBG_BO_FIXED 2U                   /* at the address given (position-dependent blobs) */

enum sbg_status { SBG_PENDING = 0, SBG_DONE = 1, SBG_FAULT = 2, SBG_TIMEOUT = 3, SBG_SKIPPED = 4, SBG_REJECTED = 5 };

struct sbg_bo {
    sbm_u64 va, size;
    sbm_u64 release_after;                /* unmap once done_seq reaches this (when release_pending) */
    unsigned flags;
    unsigned char used, release_pending;
};

struct sbg_job {
    sbm_u64 head, seq;
    unsigned slot, ctx;
};

/* The last submit that did not end SBG_DONE. */
struct sbg_bad {
    sbm_u64 seq;
    unsigned ctx, slot;
    enum sbg_status status;
    sbm_u32 js_status, as_fault, gpu_fault, mmu_irq;
    sbm_u64 fault_addr;
};

struct sbg {
    const struct sbm_io *io;
    struct sbm_pt pt;
    sbm_u64 shaders;
    sbm_u64 timeout_us;
    struct sbg_bo bo[SBG_MAX_BOS];
    struct sbg_job q[SBG_QUEUE];
    unsigned qhead, qtail;                /* q[qhead & (SBG_QUEUE-1)] is the next to start */
    int busy;
    struct sbg_job cur;
    sbm_u64 cur_start_us;
    sbm_u64 next_seq;                     /* the last sequence number handed out */
    sbm_u64 done_seq;                     /* every submit up to this one has completed (any status) */
    sbm_u32 guilty;                       /* bit per context */
    unsigned starts, faults, timeouts, resets, reset_fail, skipped;
    struct sbg_bad bad;
};

/* Power the cores, soft reset, AS0 on the page-table pool (cpu/pa: physically contiguous, uncached,
 * `tables` 4 KiB tables), interrupts unmasked for the three slots and AS0. 0 = ready. */
int sbg_init(struct sbg *g, const struct sbm_io *io, enum sbm_fmt fmt, sbm_u64 shaders, volatile sbm_u64 *pt_cpu,
             sbm_u64 pt_pa, unsigned pt_tables);
/* Everything idle: the GPU reset, cores off, AS0 disabled. Pending submits complete as SBG_SKIPPED. */
void sbg_fini(struct sbg *g);

/* Map `npages` caller pages (pa[i] = page i) as a buffer of npages * 4 KiB. va: in = the address for
 * SBG_BO_FIXED, out = where it lives. Buffers larger than 64 KiB are 64 KiB aligned. Returns the handle
 * (>= 0) or -1 (no address space left, a fixed address taken or outside the window, out of page tables). */
int sbg_bo_create(struct sbg *g, const sbm_u64 *pa, unsigned npages, unsigned flags, sbm_u64 *va);
/* Unmap once every submit up to after_seq has completed (0 / already completed: now). 0 = accepted. */
int sbg_bo_release(struct sbg *g, int handle, sbm_u64 after_seq);
/* The buffer that contains va, or -1. */
int sbg_bo_find(const struct sbg *g, sbm_u64 va);

/* Queue the chain at head on slot (0..2) for context ctx (< SBG_MAX_CTX). *seq = its fence. Starts it
 * when the GPU is idle. A guilty context's submit completes at once as SBG_SKIPPED. -1 = queue full or bad
 * arguments (nothing queued). */
int sbg_submit(struct sbg *g, unsigned slot, sbm_u64 head, unsigned ctx, sbm_u64 now_us, sbm_u64 *seq);

/* The interrupt (or a poll): completes the running chain if it ended, starts the next. Returns 1 when
 * something completed. now_us: a monotonic clock, for timeouts. */
int sbg_irq(struct sbg *g, sbm_u64 now_us);
/* Timeout check (a periodic timer): a chain running longer than timeout_us is hard-stopped, the GPU
 * reset, the submit completed as SBG_TIMEOUT and its context marked guilty. 1 = it fired. */
int sbg_tick(struct sbg *g, sbm_u64 now_us);

static inline int sbg_seq_done(const struct sbg *g, sbm_u64 seq) { return seq <= g->done_seq; }
/* A context starts over (a new D3D device on the same id): not guilty any more. */
void sbg_ctx_reset(struct sbg *g, unsigned ctx);

#endif
