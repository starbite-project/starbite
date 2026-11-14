/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sbgpu (contract v3 service SBSHM_SVC_GPU, Track E 2026-11-14: "Linux apps have no GPU, they're super slow"):
 * the Linux guest's GPU work forwarded to Starbite OS, which owns the Mali-G72 (gpucomp, address space 0).
 *
 * The guest's sbguest.ko offers a DRM render node named "panfrost" with Panfrost's UAPI, so Mesa's Panfrost (GL)
 * and PanVK run unchanged in the guest. Mesa builds the job chains itself in buffers it allocated; the guest kernel
 * gives every buffer a GPU virtual address in the guest's own address space (the GPU's AS 1 on the phone) and has
 * the host map its pages there; SUBMIT hands the host a job chain's address. The host runs it on the Mali in that
 * address space when its own compositing leaves the GPU free, and reports completion. No copy of any pixel or
 * command: the GPU reads and writes the guest's pages (physical = guest physical: there is no stage 2).
 *
 * Layout at SBSHM_GPU_OFF (1 MiB): struct sbshm_gpu (the parameters, the request ring, the completion words), then
 * SBGPU_PFN_OFF: the staging area for a MAP's page frame numbers (the guest writes it through its non-cacheable
 * mapping of the region, so the host never reads a stale guest cache line; one MAP at a time uses it).
 *
 * Requests (guest -> host), ring of SBGPU_RING, producer req_head (guest), consumer req_tail (host):
 *   MAP    a = GPU VA (4 KiB aligned), b = pages, flags SBGPU_MAP_*; the PFNs are at SBGPU_PFN_OFF (uint64 each)
 *   UNMAP  a = GPU VA, b = pages
 *   SUBMIT a = the job chain's GPU VA, b = the job's seq (1, 2, ... in order), flags SBGPU_SUBMIT_FS (fragment)
 * The host answers every request in order: done_seq = the request's seq, status = 0 or -errno (a MAP's pages are
 * checked against the guest's RAM; one outside it fails with -EINVAL and maps nothing). A SUBMIT is answered when
 * accepted; its completion is job_done (the seq of the last finished job, in submission order) with job_status
 * (0 or the Mali's exception code) and job_failed (a count). Then the guest's doorbell.
 */
#ifndef SBSHM_GPU_H
#define SBSHM_GPU_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define SBSHM_GPU_OFF       0x3F00000U          /* the last MiB of the 64 MiB region (surfaces end below it) */
#define SBSHM_GPU_SIZE      0x100000U
#define SBGPU_MAGIC         0x55504753U         /* "SGPU" */
#define SBGPU_VERSION       1U
#define SBGPU_RING          64U                 /* requests, a power of two */
#define SBGPU_PARAMS        64U                 /* DRM_PANFROST_PARAM_* values, by index */
#define SBGPU_PFN_OFF       0x10000U            /* from SBSHM_GPU_OFF: the MAP staging area */
#define SBGPU_PFN_MAX       ((SBSHM_GPU_SIZE - SBGPU_PFN_OFF) / 8U)   /* 122880 pages (480 MiB) per MAP */

enum { SBGPU_OP_MAP = 1, SBGPU_OP_UNMAP = 2, SBGPU_OP_SUBMIT = 3 };
#define SBGPU_MAP_NOEXEC    1U                  /* not executable (Panfrost's NOEXEC) */
#define SBGPU_MAP_RO        2U                  /* the GPU only reads it */
#define SBGPU_SUBMIT_FS     1U                  /* a fragment job chain (Panfrost's PANFROST_JD_REQ_FS: job slot 0) */

struct sbgpu_req {
    uint32_t op, flags;
    uint64_t seq;                               /* the request's own (1, 2, ...), answered in done_seq */
    uint64_t a, b;
};

struct sbshm_gpu {
    uint32_t magic, version;                    /* host: SBGPU_MAGIC, SBGPU_VERSION once ready (params valid) */
    uint32_t as;                                /* host: the GPU address space the guest's jobs run in (diagnostic) */
    uint32_t pad0;
    uint64_t params[SBGPU_PARAMS];              /* host: DRM_PANFROST_PARAM_* (GPU_PROD_ID, SHADER_PRESENT, ...) */
    volatile uint32_t req_head;                 /* guest */
    uint32_t pad1[15];
    volatile uint32_t req_tail;                 /* host */
    uint32_t pad2[15];
    volatile uint64_t done_seq;                 /* host: the last request answered */
    volatile int32_t status;                    /* host: its answer (0, -errno) */
    uint32_t pad3;
    volatile uint64_t job_done;                 /* host: the last finished job's seq */
    volatile uint32_t job_status;               /* host: the last failed job's exception code (0 none yet) */
    volatile uint32_t job_failed;               /* host: failed jobs so far */
    uint64_t pad4[4];
    struct sbgpu_req req[SBGPU_RING];
};

#endif
