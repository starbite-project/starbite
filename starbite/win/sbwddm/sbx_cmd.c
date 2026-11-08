/* SPDX-License-Identifier: GPL-2.0-only
 * See sbx_cmd.h. Byte-wise reads: the buffer may be unaligned and is never trusted.
 */
#include "sbx_cmd.h"

static sbx_u32 rd32(const unsigned char *p)
{
    return (sbx_u32)p[0] | (sbx_u32)p[1] << 8 | (sbx_u32)p[2] << 16 | (sbx_u32)p[3] << 24;
}

static sbx_u64 rd64(const unsigned char *p)
{
    return (sbx_u64)rd32(p) | (sbx_u64)rd32(p + 4) << 32;
}

int sbx_alloc_check(const void *priv, sbx_u32 len)
{
    const unsigned char *p = (const unsigned char *)priv;
    sbx_u32 kind, flags;
    sbx_u64 size;
    if (!p || len < sizeof(struct sbx_alloc))
        return SBX_E_SHORT;
    if (rd32(p) != SBX_ABI_VERSION)
        return SBX_E_VERSION;
    kind = rd32(p + 4);
    flags = rd32(p + 8);
    size = rd64(p + 16);
    if (kind < SBX_KIND_BUFFER || kind > SBX_KIND_SHADOW)
        return SBX_E_KIND;
    if ((flags & ~SBX_ALLOC_KNOWN_FLAGS) || rd32(p + 12))
        return SBX_E_FLAGS;
    if (!size || (size & 0xfffU) || size > SBX_MAX_ALLOC_BYTES)
        return SBX_E_SIZE;
    return 0;
}

int sbx_head_in(sbx_u64 head, sbx_u64 va, sbx_u64 size)
{
    return !(head & 63U) && size >= 64U && size - 1U <= ~va /* [va, va + size) does not wrap */ &&
           head >= va && head - va <= size - 64U;
}

int sbx_cmd_parse(const void *buf, sbx_u32 len, const struct sbx_alloc_ref *allocs, sbx_u32 n,
                  struct sbx_job_out *out, sbx_u32 max_out)
{
    const unsigned char *p = (const unsigned char *)buf;
    sbx_u32 count;
    if (!p || len < sizeof(struct sbx_cmd))
        return SBX_E_SHORT;
    if (rd32(p) != SBX_CMD_MAGIC)
        return SBX_E_MAGIC;
    count = rd32(p + 4);
    if (!count || count > SBX_MAX_JOBS || count > max_out)
        return SBX_E_COUNT;
    if ((len - sizeof(struct sbx_cmd)) / sizeof(struct sbx_job) < count)
        return SBX_E_SHORT;
    for (sbx_u32 i = 0; i < count; ++i) {
        const unsigned char *j = p + sizeof(struct sbx_cmd) + (unsigned long)i * sizeof(struct sbx_job);
        sbx_u32 slot = rd32(j), idx = rd32(j + 4);
        sbx_u64 off = rd64(j + 8);
        const struct sbx_alloc_ref *a;
        if (slot > 2U)
            return SBX_E_SLOT;
        if (!allocs || idx >= n)
            return SBX_E_INDEX;
        a = &allocs[idx];
        if (a->kind != SBX_KIND_BUFFER)
            return SBX_E_KIND;
        if (off & 63U)
            return SBX_E_ALIGN;
        if (a->size < 64U || off > a->size - 64U || !sbx_head_in(a->gpu_va + off, a->gpu_va, a->size))
            return SBX_E_RANGE;
        out[i].slot = slot;
        out[i].head = a->gpu_va + off;
    }
    return (int)count;
}
