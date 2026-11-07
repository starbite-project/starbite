/* SPDX-License-Identifier: GPL-2.0-only
 * FAT32 formatter for the Utility OS RAM disk: fat.c. */
#pragma once
#include <stdint.h>

struct fat32_info { uint32_t clusters, fat_sectors, data_sector; };

/* Format sectors x 512 bytes at disk as FAT32 (no partition table). label: 11 characters or fewer
 * (padded). 0 on success, -1 if the size cannot hold FAT32 (< 256 MiB or >= 2 TiB). */
int fat32_format(volatile uint8_t *disk, uint64_t sectors, const char *label, uint32_t serial, struct fat32_info *out);
