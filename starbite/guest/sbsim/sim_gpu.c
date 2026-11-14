/* SPDX-License-Identifier: GPL-2.0-only */
/* sbsim's stand-in for sbgpu's backend (../sbshm-gpu.h, ../host/sbg_gpu.c; included by sim_v3.c): a Mali-G72
 * r0p1's parameters as the phone's GPU reports them (GPU_ID 0x62210010, shader_present 0x5771777: NOTES.md; the rest
 * Bifrost v7 values), the guest's MAPs and UNMAPs counted (pages in the guest's RAM: sbg_gpu.c checks), and each
 * SUBMIT "run" at once: no GPU here, so the guest's Mesa gets its fences and frames of nothing. What it shows: the
 * guest's Panfrost UAPI (sbguest.ko's sbgpu) and Mesa's use of it, end to end over the region. */
#include "../sbshm-gpu.h"
static uint64_t sg_pages_mapped, sg_maps, sg_unmaps, sg_jobs, sg_jobs_fs;
static int sim_gpu_params(uint64_t *p, unsigned n)
{
    static const uint64_t v[] = {
        0x6221, 0x0010, 0x5771777, 0x1, 0x1, 0x0, 0xff, 0x7,     /* PROD_ID REVISION SHADER TILER L2 STACK AS JS */
        0x07120206, 0x0, 0x809, 0x1, 0x2830, 0x0a040400,           /* L2_F CORE_F TILER_F MEM_F MMU_F THREAD_F */
        0x180, 0x180, 0x180, 0x0,                                  /* MAX_THREADS WG BARRIER COHERENCY (none) */
        0x00fe001e, 0x0000ffff, 0x9f81ffff, 0x0,                   /* TEXTURE_FEATURES0..3 */
        0x20e, 0x1fe, 0x7e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* JS_FEATURES0..15 */
        0x1, 0x0, 0x0,                                             /* NR_CORE_GROUPS THREAD_TLS_ALLOC AFBC_F */
    };
    for (unsigned i = 0; i < n && i < sizeof(v) / sizeof(v[0]); ++i)
        p[i] = v[i];
    return 1;
}
static int sim_gpu_map(uint64_t va, const volatile uint64_t *pfn, uint32_t n, uint32_t flags)
{
    (void)va, (void)pfn, (void)flags;
    sg_maps++;
    sg_pages_mapped += n;
    return 0;
}
static int sim_gpu_unmap(uint64_t va, uint32_t n)
{
    (void)va;
    sg_unmaps++;
    sg_pages_mapped -= n <= sg_pages_mapped ? n : sg_pages_mapped;
    return 0;
}
static int sim_gpu_submit(uint64_t jc, uint32_t flags, uint64_t seq)
{
    (void)jc;
    sg_jobs++;
    sg_jobs_fs += (flags & SBGPU_SUBMIT_FS) != 0;
    sbg_gpu_done(seq, 0);
    return 0;
}
static const struct sbg_gpu sim_gpu = {sim_gpu_params, sim_gpu_map, sim_gpu_unmap, sim_gpu_submit};
#define SIM_GPU (&sim_gpu)
static void sim_gpu_tick(uint64_t t)
{
    static uint64_t t_last, jobs_last = ~0ULL;
    if (t - t_last < 5000)
        return;
    t_last = t;
    if (sg_jobs == jobs_last && !sg_maps)
        return;
    jobs_last = sg_jobs;
    puts_("sbsim: sbgpu: maps ");
    putu(sg_maps);
    puts_(", unmaps ");
    putu(sg_unmaps);
    puts_(", pages mapped now ");
    putu(sg_pages_mapped);
    puts_(", jobs ");
    putu(sg_jobs);
    puts_(" (fragment ");
    putu(sg_jobs_fs);
    puts_(")\n");
}
