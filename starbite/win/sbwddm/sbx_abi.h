/* SPDX-License-Identifier: MIT
 * The private ABI between sbwddm.sys (stage 4 kernel driver) and its user-mode driver (Mesa 26.2.x
 * d3d10umd + a Panfrost winsys for WDDM), docs/WIN11-GPU.md section 6.5. Shared by both sides: fixed-size
 * little-endian structures only, no pointers. MIT so the Mesa side may include it.
 *
 * Allocations: DxgkDdiCreateAllocation's per-allocation private data is struct sbx_alloc (UMD fills
 * version/kind/flags/size; the KMD writes gpu_va: the address the buffer keeps for its whole life, as
 * Panfrost needs it at creation).
 * Submission: the UMD's command buffer (DxgkDdiRender copies it into the DMA buffer) is a struct sbx_cmd
 * header followed by `count` struct sbx_job. A job names its chain as (allocation-list index, offset), never
 * as a raw GPU address: the KMD resolves it against the DMA buffer's allocation list, so a chain can only
 * start inside an allocation this submission references (and VidMm made resident).
 */
#ifndef SBX_ABI_H
#define SBX_ABI_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef UINT32 sbx_u32;
typedef UINT64 sbx_u64;
#else
#include <stdint.h>
typedef uint32_t sbx_u32;
typedef uint64_t sbx_u64;
#endif

#define SBX_ABI_VERSION 1U

enum sbx_kind {
    SBX_KIND_BUFFER = 1,                  /* GPU data / descriptors / shaders */
    SBX_KIND_TEXTURE = 2,
    SBX_KIND_RENDER_TARGET = 3,
    SBX_KIND_HEAP = 4,                    /* tiler heap: mapped whole */
    SBX_KIND_PRIMARY = 5,                 /* scanout (shared primary, standard allocation) */
    SBX_KIND_SHADOW = 6                   /* shadow / staging surface for the primary */
};

#define SBX_ALLOC_NOEXEC 0x1U             /* never executable by the shader cores */
#define SBX_ALLOC_CPU 0x2U                /* the UMD maps it (Lock) */
#define SBX_ALLOC_SHARED 0x4U             /* opened by other processes (DWM redirection surfaces) */
#define SBX_ALLOC_KNOWN_FLAGS 0x7U

#define SBX_MAX_ALLOC_BYTES (1ULL << 31)  /* 2 GiB: more than the GPU window ever holds */

struct sbx_alloc {
    sbx_u32 version;                      /* SBX_ABI_VERSION */
    sbx_u32 kind;                         /* enum sbx_kind */
    sbx_u32 flags;                        /* SBX_ALLOC_* */
    sbx_u32 reserved;                     /* 0 */
    sbx_u64 size;                         /* bytes, > 0, multiple of 4096 */
    sbx_u64 gpu_va;                       /* out: written by the KMD */
};

#define SBX_CMD_MAGIC 0x31584253U         /* "SBX1" */
#define SBX_MAX_JOBS 16U

struct sbx_cmd {
    sbx_u32 magic;
    sbx_u32 count;                        /* jobs that follow, 1..SBX_MAX_JOBS */
};

struct sbx_job {
    sbx_u32 slot;                         /* 0 fragment, 1 vertex/tiler, 2 compute */
    sbx_u32 alloc_index;                  /* into the DMA buffer's allocation list */
    sbx_u64 offset;                       /* first job descriptor: allocation start + offset (64-byte aligned) */
};

#endif
