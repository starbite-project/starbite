/* SPDX-License-Identifier: GPL-2.0-only
 * KMD-side validation of everything sbx_abi.h lets user mode hand the kernel. No OS calls; host-fuzzed.
 */
#ifndef SBX_CMD_H
#define SBX_CMD_H

#include "sbx_abi.h"

/* What the KMD knows about an entry of a DMA buffer's allocation list. */
struct sbx_alloc_ref {
    sbx_u64 gpu_va, size;
    sbx_u32 kind, flags;
};

/* A validated job: start the chain at head on slot. */
struct sbx_job_out {
    sbx_u32 slot;
    sbx_u64 head;
};

enum {
    SBX_E_SHORT = -1, SBX_E_MAGIC = -2, SBX_E_COUNT = -3, SBX_E_SLOT = -4, SBX_E_INDEX = -5,
    SBX_E_ALIGN = -6, SBX_E_RANGE = -7, SBX_E_KIND = -8, SBX_E_VERSION = -9, SBX_E_FLAGS = -10, SBX_E_SIZE = -11
};

/* Check a create-allocation request (len = the private data size the runtime passed). 0 = acceptable. */
int sbx_alloc_check(const void *priv, sbx_u32 len);

/* Parse and check a command buffer of len bytes against n allocation-list entries. Returns the number of
 * jobs written to out (1..max_out) or an SBX_E_* code; nothing is trusted beyond len. Chains may start in
 * buffers only (never a primary, shadow or heap), 64-byte aligned, with a whole 64-byte descriptor inside. */
int sbx_cmd_parse(const void *buf, sbx_u32 len, const struct sbx_alloc_ref *allocs, sbx_u32 n,
                  struct sbx_job_out *out, sbx_u32 max_out);

#endif
