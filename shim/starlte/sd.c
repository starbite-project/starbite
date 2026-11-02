/* SPDX-License-Identifier: GPL-2.0-only
 * The microSD card from the loader: see sd.h. No card driver yet: the loader sees no card store.
 */
#include <stdint.h>
#include "sd.h"

int sd_store_area(uint64_t *first, uint64_t *blocks)
{
    *first = *blocks = 0;
    return 0;
}

int sd_read4k(uint64_t lba, uint64_t count, uintptr_t dst)
{
    (void)lba;
    (void)count;
    (void)dst;
    return -1;
}
