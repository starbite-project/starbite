/* SPDX-License-Identifier: GPL-2.0-only
 * Read-only exFAT: exfat.c (the Utility OS's card files). */
#pragma once
#include <stdint.h>

typedef int (*exfat_rd)(uint64_t sector, uint32_t count, void *dst);   /* 512-byte sectors; 0 ok */

struct exfat {
    exfat_rd rd;
    uint64_t vol;                       /* first sector of the volume */
    uint32_t bps_shift, spc_shift, fat_off, heap_off, clusters, root, cluster_bytes;
    uint8_t *buf;                       /* one cluster */
    uint32_t buf_bytes;
    uint8_t fat_cache[512];
    uint64_t fat_sec;
    int ok;
};

struct exfat_ent {
    char name[128];                     /* UTF-8 (2-byte sequences; others '?') */
    uint64_t size;
    uint32_t first;
    int dir, nofat;
};

int exfat_find_volume(exfat_rd rd, uint8_t *sec512, uint64_t *start);       /* MBR type 0x07 or a bare volume */
int exfat_mount(struct exfat *fs, exfat_rd rd, uint64_t start, uint8_t *buf, uint32_t buf_bytes);
/* entries of dir (0: the root); returns how many were stored (<= max), *total how many there are */
int exfat_list(struct exfat *fs, const struct exfat_ent *dir, struct exfat_ent *out, int max, int *total);
int exfat_read_chain(struct exfat *fs, uint32_t first, int nofat, uint64_t off, uint8_t *dst, uint64_t len);
/* a path like "/distros/pmos.img" (case-insensitive ASCII) */
int exfat_walk(struct exfat *fs, const char *path, struct exfat_ent *out, struct exfat_ent *scratch, int scratch_n);
