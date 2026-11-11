/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite firmware layer (docs/STARBITE-FIRMWARE-LAYER.md): read-only file-system and BCD parsers,
 * no hardware (sbfwfs.c), so the host tests them on the phone's own files (tools/menu/fwtest.c).
 * Every read goes through fw_io (byte offset inside one partition); every finding is a text line
 * through fw_out. No libc, no pointer tables (the loader Image has no absolute relocations).
 */
#pragma once
#include <stdint.h>

struct fw_io {
    int (*rd)(void *u, uint64_t off, void *buf, uint32_t len);   /* 0 on success */
    void *u;
    uint64_t size;                                                /* partition bytes */
};
struct fw_out {
    void (*line)(void *u, const char *s);
    void *u;
    int bad;                                                      /* problems found (counted by the checks) */
};

/* ---- FAT32 (WINESP) ---- */
struct fat32 {
    struct fw_io io;
    uint32_t bps, spc, rsvd, nfats, fatsz, root, total_clus, fsinfo;
    uint64_t data_off, clus_bytes;
};
int fat32_mount(struct fat32 *f, const struct fw_io *io);                   /* 1 a FAT32 volume */
void fat32_health(const struct fat32 *f, struct fw_out *o);
/* a file by path ("/EFI/Microsoft/Boot/BCD", any case, long names too): 1 found */
int fat32_find(const struct fat32 *f, const char *path, uint32_t *clus, uint32_t *size);
/* the file's bytes [0, size) into buf (cap bytes): bytes read, or -1 */
int64_t fat32_read(const struct fat32 *f, uint32_t clus, uint32_t size, uint8_t *buf, uint32_t cap);
/* the partition byte offset of the file's byte off: 1 found */
int fat32_map(const struct fat32 *f, uint32_t clus, uint64_t off, uint64_t *poff);

/* ---- NTFS (WINDOWS) ---- */
void ntfs_health(const struct fw_io *io, struct fw_out *o);

/* ---- the BCD hive (a registry hive file in memory) ---- */
struct bcd_plan {
    int ok;                      /* 1: the plan below is complete and safe to apply */
    uint32_t off;                /* hive file offset of the default entry's GUID string (UTF-16) */
    uint32_t len;                /* bytes to change (76: 38 UTF-16 characters, the NUL stays) */
    char cur[40], next[40];      /* the GUIDs (ASCII): today's default and WinRE */
    char cur_name[40], next_name[40];
};
/* Read-only: the header (checksum, clean = equal sequence numbers), the default boot entry, its
 * recovery sequence (WinRE) and whether that entry exists. Fills plan for "Advanced Startup next boot":
 * the default entry's GUID -> the WinRE entry's GUID, in place. */
void bcd_check(const uint8_t *h, uint32_t len, struct bcd_plan *plan, struct fw_out *o);

/* small helpers shared with sbfw.c */
struct fwb { char b[160]; unsigned n; };
void fwb_init(struct fwb *s);
void fwb_s(struct fwb *s, const char *t);
void fwb_u(struct fwb *s, uint64_t v);
void fwb_x(struct fwb *s, uint64_t v, unsigned digits);
void fwb_mb(struct fwb *s, uint64_t bytes);
