/* SPDX-License-Identifier: GPL-2.0-only
 * StarbiteUFS's fence, filled by the loader (ufs_fence.c): the XNU payload's ADT carries two zero-filled
 * placeholders on /arm-io/ufs (tools/board/mkadt-board.py, board key "fence"), "starbite,allowed-ranges" and
 * "starbite,denied-ranges"; at every XNU start the loader fills them in place from LU 0's live GPT, the way
 * iBoot fills ADT properties. Format: starbite/apple-drivers/StarbiteUFS/NOTES.md "Fence property format".
 *
 * The allow-list rule (evidence in ufs_fence.c's head): exactly one range, LU 0's GPT partition "USERDATA" (the
 * Apple slot): all of it when USERDATA belongs to iOS (the card store is home, or card only), else only its
 * first 16 GiB (the UFS OS store lives behind it). Flags ROOT, never WRITABLE. The deny-list: every other GPT
 * partition by name, the GPT head and tail, and the UFS store area when it is excluded. Anything the loader
 * cannot read or check gives an EMPTY allow-list: the driver then exposes nothing.
 */
#pragma once
#include <stdint.h>

#define UFS_FENCE_ALLOW_MAGIC 0x52464253U    /* "SBFR" */
#define UFS_FENCE_DENY_MAGIC  0x44464253U    /* "SBFD" */
#define UFS_FENCE_ALLOW_REC   64U
#define UFS_FENCE_DENY_REC    40U
#define UFS_FENCE_ALLOW_MAX   8U             /* the driver's SBUFS_MAX_RANGES */
#define UFS_FENCE_DENY_MAX    48U            /* the driver's SBUFS_MAX_DENY */
#define UFS_FENCE_STORE_OFFSET 4194304ULL    /* store.c UFS_STORE_OFFSET: the UFS store at USERDATA + 16 GiB */

/* What the builder needs from the GPT (ufs.c's gpt_load arrays plus the header and USERDATA's GUID). */
struct ufs_fence_gpt {
    int n;                                   /* partitions; < 0: the GPT could not be read */
    const char (*name)[24];
    const uint64_t *first, *last;
    uint64_t first_usable, last_usable, alt_lba;
    uint8_t ios_uuid[16];                    /* USERDATA's unique GUID, RFC 4122 byte order */
    int ios_whole;                           /* 1: all of USERDATA is iOS's (no UFS store behind it) */
};

/* Fill the two placeholder values (alen / dlen bytes each: the header plus whole records). Returns the
 * number of allowed ranges written (0: an empty allow-list, *why says why), or -1 if a buffer cannot hold
 * the header (nothing written). */
int ufs_fence_build(const struct ufs_fence_gpt *g, uint8_t *allow, uint32_t alen, uint8_t *deny, uint32_t dlen,
                    const char **why);

/* Find /arm-io/ufs's two placeholders in an ADT (size bytes): 1 with their value offsets and lengths, 0 if the
 * ADT does not carry both (or is malformed: nothing is touched then). */
int ufs_fence_adt_find(const uint8_t *adt, uint32_t size, uint32_t *aoff, uint32_t *alen, uint32_t *doff,
                       uint32_t *dlen);

/* Both steps on an ADT in memory: -1 no placeholders (untouched), else what ufs_fence_build returned. */
int ufs_fence_adt_fill(uint8_t *adt, uint32_t size, const struct ufs_fence_gpt *g, const char **why);

/* The hook in xnu_enter: the ADT copy at adt (adt_size bytes); [lo, hi) is everything the loader placed for XNU
 * (kernel, ADT, boot_args), which UFS's DMA buffers (ufs.h UFS_DMA ..) must not overlap. Reads the GPT only
 * when the ADT has the placeholders. Returns as ufs_fence_adt_fill; ufs_fence_why says why when 0. */
int ufs_fence_xnu(uintptr_t adt, uint64_t adt_size, uintptr_t lo, uintptr_t hi);
extern const char *ufs_fence_why;
