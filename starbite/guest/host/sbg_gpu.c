/* SPDX-License-Identifier: GPL-2.0-only */
/* sbg_gpu: the host side of sbgpu (contract v3, ../sbshm-gpu.h): the guest's GPU requests to the backend
 * (sbg_os.gpu: Starbite OS's Mali in a second address space, or sbsim's stand-in). Built inside sbghost.c.
 *   load: the region zeroed, the backend's parameters copied in, magic + version last (the guest waits for them);
 *         no parameters (no GPU): the service stays unoffered (the guest sees no magic)
 *   poll: the requests in order, a few per poll: MAP (every PFN checked against the guest's RAM first), UNMAP,
 *         SUBMIT (accepted: the backend calls sbg_gpu_done when the chain has run); each answered (done_seq,
 *         status), then the guest's doorbell
 * Counters only in the log, never an address. */
#include "sbghost_int.h"
#include "../sbshm-gpu.h"

#define GPU_PER_POLL 8U
#define GPUS ((volatile struct sbshm_gpu *)sbg_shm(SBSHM_GPU_OFF))

static uint32_t gpu_on, gpu_tail, gpu_bad;
static uint32_t gpu_maps, gpu_unmaps, gpu_submits, gpu_refused;

static int gpu_live(void) { return gpu_on && (sbg_services() & SBSHM_SVC_GPU) && sbg_os_()->gpu; }

static void gpu_say(const char *what, uint32_t a, uint32_t b)
{
    struct tb t = {.n = 0};
    t_s(&t, "guest: sbgpu: ");
    t_s(&t, what);
    t_u(&t, a);
    t_s(&t, " / ");
    t_u(&t, b);
    say(&t);
}

void sbg_gpu_load(void)
{
    gpu_on = gpu_tail = gpu_bad = 0;
    if (!(sbg_services() & SBSHM_SVC_GPU) || !sbg_os_()->gpu)
        return;
    volatile struct sbshm_gpu *g = GPUS;
    zero_words(sbg_shm(SBSHM_GPU_OFF), SBSHM_GPU_SIZE);
    uint64_t p[SBGPU_PARAMS];
    for (unsigned i = 0; i < SBGPU_PARAMS; ++i)
        p[i] = 0;
    if (!sbg_os_()->gpu->params || sbg_os_()->gpu->params(p, SBGPU_PARAMS) <= 0) {
        gpu_say("no GPU from the backend: not offered (params ", 0, 0);
        return;
    }
    for (unsigned i = 0; i < SBGPU_PARAMS; ++i)
        g->params[i] = p[i];
    g->as = 1;
    sbshm_dmb();
    g->version = SBGPU_VERSION;
    g->magic = SBGPU_MAGIC;
    gpu_on = 1;
    gpu_say("offered: GPU id ", (uint32_t)p[0], (uint32_t)p[1]);
}

/* every PFN of a MAP inside the guest's RAM (the carve-out): the GPU must never be pointed at the host's memory */
static int gpu_pfns_ok(const volatile uint64_t *pfn, uint32_t n)
{
    const struct sbg_cfg *c = sbg_cfg_();
    uint64_t lo = c->linux_base >> 12, hi = (c->linux_base + c->linux_size) >> 12;
    for (uint32_t i = 0; i < n; ++i)
        if (pfn[i] < lo || pfn[i] >= hi)
            return 0;
    return 1;
}

/* the backend: job seq finished (in submission order) */
void sbg_gpu_done(uint64_t seq, uint32_t status)
{
    if (!gpu_on)
        return;
    volatile struct sbshm_gpu *g = GPUS;
    if (status) {
        g->job_status = status;
        g->job_failed = g->job_failed + 1;
    }
    sbshm_dmb();
    if ((int64_t)(seq - g->job_done) > 0)
        g->job_done = seq;
    sbg_ring_guest();
}

void sbg_gpu_poll(void)
{
    if (!gpu_live() || gpu_bad)
        return;
    volatile struct sbshm_gpu *g = GPUS;
    const struct sbg_gpu *b = sbg_os_()->gpu;
    int any = 0;
    for (unsigned k = 0; k < GPU_PER_POLL; ++k) {
        uint32_t head = g->req_head;
        if (head == gpu_tail)
            break;
        if (head - gpu_tail > SBGPU_RING) {             /* not a ring any more: stop serving, say so once */
            gpu_bad = 1;
            gpu_say("the request ring is broken (head - tail ", head - gpu_tail, SBGPU_RING);
            return;
        }
        sbshm_dmb();
        volatile struct sbgpu_req *r = &g->req[gpu_tail & (SBGPU_RING - 1)];
        uint32_t op = r->op, flags = r->flags;
        uint64_t seq = r->seq, a = r->a, n = r->b;
        int st = -SBG_EINVAL;
        if (op == SBGPU_OP_MAP) {
            const volatile uint64_t *pfn = (const volatile uint64_t *)sbg_shm(SBSHM_GPU_OFF + SBGPU_PFN_OFF);
            if (n && n <= SBGPU_PFN_MAX && !(a & 0xfffU) && gpu_pfns_ok(pfn, (uint32_t)n))
                st = b->map(a, pfn, (uint32_t)n, flags);
            gpu_maps++;
        } else if (op == SBGPU_OP_UNMAP) {
            if (n && !(a & 0xfffU))
                st = b->unmap(a, (uint32_t)n);
            gpu_unmaps++;
        } else if (op == SBGPU_OP_SUBMIT) {
            st = a ? b->submit(a, flags, n) : -SBG_EINVAL;
            gpu_submits++;
            if (st < 0)
                sbg_gpu_done(n, 0x40U);                /* refused: done at once, failed (DONE + a fault code) */
        }
        if (st < 0 && gpu_refused++ < 4)
            gpu_say("a request refused (op / count so far) ", op, gpu_refused);
        g->status = st;
        sbshm_dmb();
        g->done_seq = seq;
        g->req_tail = ++gpu_tail;
        any = 1;
    }
    if (any)
        sbg_ring_guest();
}
