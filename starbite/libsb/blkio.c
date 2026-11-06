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

int sb_blk_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    if (medium == SB_MED_UFS)
        return ufs_write_blocks(lu, lba, n, (uintptr_t)src);
    if (medium == SB_MED_SD && !lu)
        return sd_write4k(lba, n, (uintptr_t)src);
    return -1;
}
