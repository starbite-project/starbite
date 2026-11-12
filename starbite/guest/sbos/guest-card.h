/* SPDX-License-Identifier: GPL-2.0-only
 * The guest's block devices (sbblk, starbite/guest/blk/README.md) from FILES on the microSD card's exFAT
 * partition: nothing here reads or writes UFS, and nothing is in the OS store.
 *
 *   /starbite/linux/base.erofs    -> /dev/sbblk0, read-only
 *   /starbite/linux/overlay.img   -> /dev/sbblk1, read-write (ext4 with 4 KiB blocks)
 *   /starbite/linux/repo.erofs    -> /dev/sbblk2, read-only
 *
 * At each guest start gcard_offer() mounts the card's exFAT volume read-only (starbite/utility/exfat.c), walks
 * to each file and turns it into ONE run of card sectors. A file is offered only when that run is proven:
 *   - it is a file of at least 4 KiB whose clusters lie inside the volume's cluster heap,
 *   - it is contiguous: its directory entry says NoFatChain (exFAT's own "one run" flag), or its FAT chain is
 *     walked cluster by cluster and every link is "the next cluster", ending with the end mark at exactly the
 *     file's last cluster,
 *   - the run lies inside the exFAT partition of the card's MBR (type 0x07).
 * Anything else: the device is not offered and the log says why (no overlay -> the guest's init uses a tmpfs
 * upper layer and warns; no base -> no Linux desktop).
 *
 * Reads (all three) are plain card reads into the request's slot: no grant. Writes exist only for the overlay
 * and only when ALL of these hold: this build has GUEST_CARD_WRITE (on by default; off when the file is compiled
 * with SBOS_SELFTEST_NEXT_LINUX), the image is not a self-test image (sbos_selftest_enabled, which uitest.c
 * sets at compile time: starbite-next-linuxtest and every other self test write NOTHING, on any medium: there
 * the overlay is not offered at all, because mounting ext4 read-write already writes), the overlay's run starts
 * on a 4 KiB boundary of the card, and libsb's fence gave the card-file grant for exactly that run
 * (sb_card_grant: it reads the partition table and the exFAT boot sector itself). Every write then goes through
 * sb_write_card, which checks the range again. A flush is the card's own: sd_write returns when the card has
 * programmed the blocks (busy released, CMD13 in tran), so nothing is left to flush.
 *
 * The first part (gcard_extent and its helpers) is plain C over the exFAT reader, built by the host test
 * (starbite/guest/blk/card-test.c) against a real exFAT image; the second part is the phone's glue.
 */
#ifndef GUEST_CARD_H
#define GUEST_CARD_H
#include <stdint.h>
#if defined(GUEST_QEMU) && !defined(GCARD_HOST_TEST)
/* the QEMU run of guest.c has no card (sbsim serves files by semihosting) */
static void gcard_offer(void) {}
static int gcard_io(uint32_t dev, uint32_t op, uint64_t sector, uint32_t count, void *buf)
{
    (void)dev, (void)op, (void)sector, (void)count, (void)buf;
    return -SBG_ENODEV;
}
#else
#include "exfat.h"

#define GCARD_DIR "/starbite/linux/"
#define GCARD_BASE GCARD_DIR "base.erofs"
#define GCARD_OVERLAY GCARD_DIR "overlay.img"
#define GCARD_REPO GCARD_DIR "repo.erofs"

static uint32_t gcard_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* the card's exFAT partition from the MBR: its first sector and length. NULL, or why not. A card without a
 * partition table (a bare volume) is refused: "inside p1" could not be checked. */
static const char *gcard_partition(exfat_rd rd, uint8_t *sec512, uint64_t *start, uint64_t *len)
{
    if (rd(0, 1, sec512))
        return "the card's first sector cannot be read";
    if (sec512[510] != 0x55 || sec512[511] != 0xaa || (sec512[3] == 'E' && sec512[4] == 'X' && sec512[5] == 'F'))
        return "the card has no partition table";
    for (int i = 0; i < 4; ++i) {
        const uint8_t *e = sec512 + 446 + 16 * i;
        if (e[4] == 0x07 && gcard_le32(e + 8) && gcard_le32(e + 12)) {
            *start = gcard_le32(e + 8);
            *len = gcard_le32(e + 12);
            return 0;
        }
    }
    return "the card has no exFAT partition";
}

/* The file at path as one run of card sectors: *sector, *sectors (the size rounded down to 4 KiB). NULL when the
 * run is proven (see the head of this file), else the reason. fs: mounted on the partition at p_start (p_len
 * sectors); sec512: a sector buffer (8-byte aligned). */
static const char *gcard_extent(struct exfat *fs, uint64_t p_start, uint64_t p_len, const char *path,
                                uint8_t *sec512, uint64_t *sector, uint64_t *sectors)
{
    static struct exfat_ent hit[1];
    struct exfat_ent e;
    *sector = *sectors = 0;
    if (!fs->ok || fs->vol != p_start)
        return "the exFAT volume is not mounted";
    if (exfat_walk(fs, path, &e, hit, 1))
        return "the file is not on the card";
    if (e.dir)
        return "it is a folder";
    if (e.size < 4096U)
        return "the file is smaller than 4 KiB";
    uint64_t cb = fs->cluster_bytes, n = (e.size + cb - 1) / cb;
    if (e.first < 2 || n > fs->clusters || e.first - 2 > fs->clusters - n)
        return "its clusters are outside the volume";
    if (!e.nofat) {
        /* a FAT chain: every link must be "the next cluster", and the chain must end at the last one */
        uint64_t have = ~0ULL;
        for (uint64_t i = 0; i < n; ++i) {
            uint32_t c = e.first + (uint32_t)i;
            uint64_t s = fs->vol + fs->fat_off + c / 128U;
            if (s != have) {
                if (fs->rd(s, 1, sec512))
                    return "the FAT cannot be read";
                have = s;
            }
            uint32_t next = gcard_le32(sec512 + (c % 128U) * 4U);
            if (i + 1 < n ? next != c + 1 : next != 0xffffffffU)
                return i + 1 < n ? "the file is fragmented (its FAT chain is not one run)" :
                                   "its FAT chain does not end at the file's last cluster";
        }
    }
    uint64_t s0 = fs->vol + fs->heap_off + ((uint64_t)(e.first - 2) << fs->spc_shift);
    uint64_t ns = (e.size / 4096U) * 8U;
    if (fs->heap_off == 0 || s0 < p_start || s0 + ns < s0 || s0 + ns > p_start + p_len)
        return "the file's sectors are outside the exFAT partition";
    *sector = s0;
    *sectors = ns;
    return 0;
}

/* 1 when two runs share a sector (two directory entries on the same clusters: a damaged volume) */
static int gcard_overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return an && bn && a < b + bn && b < a + an;
}

/* Why the overlay's run may not be written, or NULL: the fence's grant and its writes are in 4 KiB blocks of the
 * card, so the run must start on one; a self-test image writes nothing at all. */
static __attribute__((unused)) const char *gcard_overlay_why(uint64_t sector, int selftest_image)
{
    if (selftest_image)
        return "a self-test image writes nothing (tmpfs upper in the guest)";
    if (sector & 7U)
        return "it does not start on a 4 KiB boundary of the card";
    return 0;
}

#ifndef GCARD_HOST_TEST
/* ============================== the phone's glue (inside guest.c) ============================== */
#include "sd.h"

#ifndef GUEST_CARD_WRITE
#define GUEST_CARD_WRITE 1                  /* the overlay may be written, inside the fence's card-file grant */
#endif
#ifdef SBOS_SELFTEST_NEXT_LINUX             /* the Linux self test writes nothing, the fenced file included */
#undef GUEST_CARD_WRITE
#define GUEST_CARD_WRITE 0
#endif

static struct exfat gc_fs;
static uint8_t *gc_buf;                     /* one cluster (128 KiB), as kern/files.c */
static uint64_t gc_sec[64];                 /* one sector */
static struct { uint64_t sector, sectors; } gc_ext[SBSHM_BLK_DEVS];
static struct sbshm_blkdev gc_dev[SBSHM_BLK_DEVS];
static uint32_t gc_cid[4];                  /* the card the runs were resolved on */
static int gc_writable;
static uint32_t gc_errs;

static int gc_rd(uint64_t sector, uint32_t count, void *dst)
{
    return sd_read(sector, count, (uintptr_t)dst);
}

static void gc_say(const char *what, const char *name, const char *why)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "guest: card: ");
    ub_s(&u, name);
    ub_s(&u, what);
    ub_s(&u, why);
    sb_log(u.b);
}

/* before each load (after cfg_init): which block devices this guest gets. Always leaves sbghost with a
 * consistent table (none when the card or the base image is missing). */
static void gcard_offer(void)
{
    static const char *const path[3] = {GCARD_BASE, GCARD_OVERLAY, GCARD_REPO};
    static const char *const label[3] = {"base", "overlay", "repo"};
    uint64_t p_start = 0, p_len = 0;
    const char *no = 0;
    gc_writable = 0;
    gc_errs = 0;
    sb_card_grant_clear();                  /* a grant from an earlier start is asked for again */
    for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i) {
        gc_dev[i] = (struct sbshm_blkdev){0};
        gc_ext[i].sector = gc_ext[i].sectors = 0;
    }
#ifdef GUEST_WL_IMAGE                       /* with the Linux apps' guest (next-linux.patch): only that one has a root there */
    if (mode != GUEST_WL) {
        sbg_set_blk(gc_dev);
        return;
    }
#endif
    if (!gc_buf)
        gc_buf = sb_alloc(1U << 17);
    sd_allow();
    hw_wdt_feed();
    if (!gc_buf)
        no = "no memory for the cluster buffer";
    else if (!sd_up())
        no = "no card, or it did not start";
    else if (!(no = gcard_partition(gc_rd, (uint8_t *)gc_sec, &p_start, &p_len)) &&
             exfat_mount(&gc_fs, gc_rd, p_start, gc_buf, 1U << 17))
        no = "the partition does not mount as exFAT (512-byte sectors, clusters up to 128 KiB)";
    if (no) {
        gc_say(": ", "no Linux block devices", no);
        sbg_set_blk(gc_dev);
        return;
    }
    for (unsigned i = 0; i < 4; ++i)
        gc_cid[i] = sd_rep.cid[i];
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t s = 0, n = 0;
        hw_wdt_feed();
        no = gcard_extent(&gc_fs, p_start, p_len, path[i], (uint8_t *)gc_sec, &s, &n);
        for (unsigned k = 0; !no && k < i; ++k)
            if (gcard_overlap(s, n, gc_ext[k].sector, gc_ext[k].sectors))
                no = "it shares sectors with another of the three files";
        if (!no && i == SBSHM_BLK_OVERLAY) {
#if GUEST_CARD_WRITE
            no = gcard_overlay_why(s, sbos_selftest_enabled);
            if (!no && !sb_card_grant(s / 8U, n / 8U, "overlay.img"))
                no = "the write fence refused the grant (see the fence's line)";
            gc_writable = !no;
#else
            no = "this build cannot write the card (tmpfs upper in the guest)";
#endif
        }
        if (no) {
            gc_say(" not offered: ", path[i], no);
            continue;
        }
        gc_ext[i].sector = s;
        gc_ext[i].sectors = n;
        gc_dev[i].sectors = n;
        gc_dev[i].flags = SBSHM_BLKDEV_PRESENT | (i == SBSHM_BLK_OVERLAY ? 0U : SBSHM_BLKDEV_RO);
        sb_strcpy(gc_dev[i].name, sizeof(gc_dev[i].name), label[i]);
        {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "guest: card: ");
            ub_s(&u, path[i]);
            ub_s(&u, i == SBSHM_BLK_OVERLAY ? " read-write, sectors " : " read-only, sectors ");
            ub_dec(&u, s);
            ub_s(&u, " + ");
            ub_dec(&u, n);
            sb_log(u.b);
        }
    }
    if (!(gc_dev[SBSHM_BLK_BASE].flags & SBSHM_BLKDEV_PRESENT)) {    /* no root: offer nothing at all */
        sb_card_grant_clear();
        gc_writable = 0;
        for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i)
            gc_dev[i] = (struct sbshm_blkdev){0};
    }
    sbg_set_blk(gc_dev);
}

/* sbghost's blk_io (from sbg_poll, in the guest timer): one request of at most 64 KiB, straight between the
 * card and the request's slot (the card driver moves the data itself: no DMA, no bounce). sbghost has checked
 * the device, the range and "no write to a read-only device" against the table gcard_offer gave it; the range is
 * checked again against the file's run here, and a write once more by the fence. */
static int gcard_io(uint32_t dev, uint32_t op, uint64_t sector, uint32_t count, void *buf)
{
    if (dev >= SBSHM_BLK_DEVS || !(gc_dev[dev].flags & SBSHM_BLKDEV_PRESENT))
        return -SBG_ENODEV;
    if (op == SBG_BLK_FLUSH)
        return 0;                           /* every write returned with the card back in tran: nothing pending */
    if (!count || count > SBSHM_BLK_SLOT / 512U || sector >= gc_ext[dev].sectors ||
        count > gc_ext[dev].sectors - sector)
        return -SBG_EINVAL;
    int r = -SBG_EROFS;
    if (sd_rep.cid[0] != gc_cid[0] || sd_rep.cid[1] != gc_cid[1] || sd_rep.cid[2] != gc_cid[2] ||
        sd_rep.cid[3] != gc_cid[3]) {
        r = -SBG_EIO;                       /* another card: these sectors are not the files any more */
    } else if (op == SBG_BLK_READ) {
        r = sd_read(gc_ext[dev].sector + sector, count, (uintptr_t)buf) ? -SBG_EIO : 0;
#if GUEST_CARD_WRITE
    } else if (op == SBG_BLK_WRITE && dev == SBSHM_BLK_OVERLAY && gc_writable) {
        if ((sector & 7U) || (count & 7U))
            r = -SBG_EINVAL;                /* whole 4 KiB blocks only: never a read-modify-write */
        else
            r = sb_write_card((gc_ext[dev].sector + sector) / 8U, count / 8U, buf) ? -SBG_EIO : 0;
#endif
    }
    if (r && gc_errs++ < 4)
        gc_say(op == SBG_BLK_WRITE ? ": a write failed or was refused" : ": a read failed", gc_dev[dev].name, "");
    return r;
}
#endif /* !GCARD_HOST_TEST */
#endif /* a card */
#endif
