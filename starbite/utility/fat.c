/* SPDX-License-Identifier: GPL-2.0-only
 * FAT32 formatter for the Utility OS's RAM disk (a "superfloppy": no partition table, the way macOS,
 * Windows and Linux mount a USB stick formatted without one). 512-byte sectors, 4 KiB clusters,
 * 32 reserved sectors, two FATs, the root directory in cluster 2 with the volume label. Only the
 * metadata is written; data clusters keep whatever the RAM held (they are free in the FAT).
 * Microsoft's FAT specification (fatgen103): BPB, FSInfo, the cluster-count rule (>= 65525). */
#include "fat.h"

static void w16(volatile uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(volatile uint8_t *p, uint32_t v) { w16(p, v & 0xffff); w16(p + 2, v >> 16); }

static void zero(volatile uint8_t *p, uint64_t n)
{
    volatile uint64_t *q = (volatile uint64_t *)p;
    for (uint64_t i = 0; i < n / 8; ++i)
        q[i] = 0;
}

static void text(volatile uint8_t *p, const char *s, unsigned n)
{
    unsigned i = 0;
    for (; s[i] && i < n; ++i)
        p[i] = (uint8_t)s[i];
    for (; i < n; ++i)
        p[i] = ' ';
}

int fat32_format(volatile uint8_t *disk, uint64_t sectors, const char *label, uint32_t serial, struct fat32_info *out)
{
    const unsigned spc = 8, rsvd = 32, nfat = 2;
    if (sectors > 0xffffffffULL || sectors < 65536ULL * spc)
        return -1;                                     /* FAT32 needs >= 65525 clusters: >= 256 MiB here */
    uint32_t fatsz = 1, clusters = 0;
    for (int it = 0; it < 8; ++it) {                   /* FAT size and cluster count settle in a few rounds */
        clusters = (uint32_t)((sectors - rsvd - (uint64_t)nfat * fatsz) / spc);
        uint32_t need = ((clusters + 2) * 4U + 511U) / 512U;
        if (need == fatsz)
            break;
        fatsz = need;
    }
    if (clusters < 65525U)
        return -1;
    uint64_t data0 = rsvd + (uint64_t)nfat * fatsz;    /* first data sector: cluster 2, the root */
    zero(disk, (data0 + spc) * 512ULL);                /* reserved area, both FATs, the root cluster */

    volatile uint8_t *b = disk;                        /* boot sector + BPB */
    b[0] = 0xeb; b[1] = 0x58; b[2] = 0x90;
    text(b + 3, "STARBITE", 8);
    w16(b + 11, 512); b[13] = (uint8_t)spc; w16(b + 14, rsvd); b[16] = (uint8_t)nfat;
    w16(b + 17, 0); w16(b + 19, 0); b[21] = 0xf8; w16(b + 22, 0);
    w16(b + 24, 63); w16(b + 26, 255); w32(b + 28, 0); w32(b + 32, (uint32_t)sectors);
    w32(b + 36, fatsz); w16(b + 40, 0); w16(b + 42, 0); w32(b + 44, 2);
    w16(b + 48, 1); w16(b + 50, 6);                    /* FSInfo at 1, backup boot sector at 6 */
    b[64] = 0x80; b[66] = 0x29; w32(b + 67, serial);
    text(b + 71, label, 11);
    text(b + 82, "FAT32", 8);
    b[510] = 0x55; b[511] = 0xaa;

    volatile uint8_t *fi = disk + 512;                 /* FSInfo */
    w32(fi + 0, 0x41615252U);
    w32(fi + 484, 0x61417272U);
    w32(fi + 488, clusters - 1);                       /* free: all but the root's cluster */
    w32(fi + 492, 3);                                  /* next free hint */
    w32(fi + 508, 0xaa550000U);

    for (unsigned i = 0; i < 1024; ++i)                /* backups at sectors 6 and 7 */
        disk[6 * 512 + i] = disk[i];

    for (unsigned f = 0; f < nfat; ++f) {
        volatile uint8_t *fat = disk + (rsvd + (uint64_t)f * fatsz) * 512ULL;
        w32(fat + 0, 0x0ffffff8U);                     /* media */
        w32(fat + 4, 0x0fffffffU);
        w32(fat + 8, 0x0fffffffU);                     /* cluster 2: the root directory, one cluster */
    }

    volatile uint8_t *root = disk + data0 * 512ULL;    /* the volume label entry */
    text(root, label, 11);
    root[11] = 0x08;

    if (out) {
        out->clusters = clusters;
        out->fat_sectors = fatsz;
        out->data_sector = (uint32_t)data0;
    }
    return 0;
}
