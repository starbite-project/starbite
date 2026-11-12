/* SPDX-License-Identifier: GPL-2.0-only
 * Block I/O in 4 KiB blocks for the UFS LUs and the microSD card: the one place that picks the
 * driver. Writes are only called by fence.c (sb_write, sb_write_store); the drivers refuse a write
 * unless the fence is active.
 */
#include "sb.h"
#include "ufs.h"
#include "sd.h"

int sb_blk_read(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, void *dst)
{
    if (medium == SB_MED_UFS)
        return ufs_read_lu_to(lu, lba, n, (uintptr_t)dst);
    if (medium == SB_MED_SD && !lu)
        return sd_read4k(lba, n, (uintptr_t)dst);
    return -1;
}

/* The same with forced unit access, then the device's cache flushed (UFS); the card has no FUA: sd.c's write
 * returns once the card has programmed the blocks (busy released, CMD13 in tran), then each block is read back
 * and compared (the fence's volatile writer). */
static uint64_t vrb[512];                   /* one 4 KiB block read back (the card is read by the CPU) */

int sb_blk_write_sync(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    if (medium == SB_MED_SD && !lu) {
        if (sd_write4k(lba, n, (uintptr_t)src))
            return -1;
        for (uint64_t i = 0; i < n; ++i)
            if (sd_read4k(lba + i, 1, (uintptr_t)vrb) || memcmp(vrb, (const uint8_t *)src + i * 4096U, 4096))
                return -1;
        return 0;
    }
    if (medium != SB_MED_UFS)
        return -1;
    ufs_fua = 1;
    int r = ufs_write_blocks(lu, lba, n, (uintptr_t)src);
    ufs_fua = 0;
    if (!r)
        r = ufs_sync_cache(lu);
    return r;
}

int sb_blk_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    if (medium == SB_MED_UFS)
        return ufs_write_blocks(lu, lba, n, (uintptr_t)src);
    if (medium == SB_MED_SD && !lu)
        return sd_write4k(lba, n, (uintptr_t)src);
    return -1;
}
