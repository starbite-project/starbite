/* SPDX-License-Identifier: GPL-2.0-only
 * The microSD card from the loader (sd.c): read only, for the card's OS store (store.c). */
#pragma once
#include <stdint.h>

/* The card's store partition (MBR entry of type 0xDA) in 4 KiB blocks: 1 found. */
int sd_store_area(uint64_t *first, uint64_t *blocks);
/* count 4 KiB blocks from 4 KiB block lba of the card to dst: 0 on success. */
int sd_read4k(uint64_t lba, uint64_t count, uintptr_t dst);
