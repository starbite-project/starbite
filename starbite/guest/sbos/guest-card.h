/* SPDX-License-Identifier: GPL-2.0-only
 * The guest's block devices (sbblk, starbite/guest/blk/README.md) from FILES on the microSD card's exFAT
 * partition: nothing here reads or writes UFS, and nothing is in the OS store.
 *
 *   /starbite/linux/base.erofs    -> /dev/sbblk0, read-only
 *   /starbite/linux/overlay.img   -> /dev/sbblk1, read-write (ext4 with 4 KiB blocks)
 *   /starbite/linux/repo.erofs    -> /dev/sbblk2, read-only
 *   /starbite/linux/appstore.erofs -> /dev/sbblk3, read-only (Track F's offline App Store mirror, optional)
 *
 * At each guest start gcard_begin() + gcard_tick() mount the card's exFAT volume read-only (starbite/utility/exfat.c), walks
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
static struct { uint32_t reads, cached, walked; } gc;
static void gcard_begin(void) {}
static int gcard_tick(void) { return 1; }
static void gcard_abort(void) {}
static const char *gcard_missing(void) { return 0; }
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
#define GCARD_APPSTORE GCARD_DIR "appstore.erofs"

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

/* The proof that one file is ONE run of card sectors (see the head of this file), resumable: the FAT chain is
 * walked in batches of FAT sectors, so the phone's start can do one batch per timer tick (a 2 GiB overlay at
 * 32 KiB clusters is 512 FAT sectors: one read each made the start freeze the UI for seconds; cp7g 2026-11-13).
 *   gcard_proof_begin(p, fs, p_start, p_len, e)   from the file's directory entry (exfat_walk / exfat_list)
 *   gcard_proof_step(p, fs, buf, batch)           at most `batch` FAT sectors in one read into buf (batch * 512
 *                                                 bytes); 1 when the verdict is in
 *   gcard_proof_result(p, fs, &sector, &sectors)  NULL and the run, or the reason */
struct gcard_proof {
    struct exfat_ent e;
    uint64_t p_start, p_len;
    uint64_t n, i;                          /* the file's clusters; the chain links checked so far */
    const char *why;                        /* the verdict (NULL: proven) once done */
    int done;
    uint32_t reads;                         /* FAT reads made */
};

static void gcard_proof_begin(struct gcard_proof *p, const struct exfat *fs, uint64_t p_start, uint64_t p_len,
                              const struct exfat_ent *e)
{
    p->e = *e;
    p->p_start = p_start;
    p->p_len = p_len;
    p->n = p->i = 0;
    p->why = 0;
    p->done = 1;
    p->reads = 0;
    if (!fs->ok || fs->vol != p_start) {
        p->why = "the exFAT volume is not mounted";
        return;
    }
    if (e->dir) {
        p->why = "it is a folder";
        return;
    }
    if (e->size < 4096U) {
        p->why = "the file is smaller than 4 KiB";
        return;
    }
    uint64_t cb = fs->cluster_bytes, n = (e->size + cb - 1) / cb;
    if (e->first < 2 || n > fs->clusters || e->first - 2 > fs->clusters - n) {
        p->why = "its clusters are outside the volume";
        return;
    }
    p->n = n;
    p->done = e->nofat;                     /* NoFatChain: exFAT's own "one run" flag, no chain to walk */
}

static int gcard_proof_step(struct gcard_proof *p, struct exfat *fs, uint8_t *buf, uint32_t batch)
{
    if (p->done)
        return 1;
    /* every link must be "the next cluster", and the chain must end at the last one: the links of clusters
     * first + i .. first + n - 1 lie in consecutive FAT sectors, read `batch` at a time */
    uint32_t c0 = p->e.first + (uint32_t)p->i, last = p->e.first + (uint32_t)(p->n - 1);
    uint64_t s0 = c0 / 128U, s_end = last / 128U + 1;
    uint32_t k = batch ? batch : 1;
    if (s_end - s0 < k)
        k = (uint32_t)(s_end - s0);
    p->reads++;
    if (fs->rd(fs->vol + fs->fat_off + s0, k, buf)) {
        p->why = "the FAT cannot be read";
        p->done = 1;
        return 1;
    }
    uint64_t c_stop = (s0 + k) * 128U;     /* the first cluster whose link is not in this batch */
    for (uint64_t c = c0; c < c_stop && p->i < p->n; ++c, ++p->i) {
        uint32_t next = gcard_le32(buf + (c - s0 * 128U) * 4U);
        if (p->i + 1 < p->n ? next != c + 1 : next != 0xffffffffU) {
            p->why = p->i + 1 < p->n ? "the file is fragmented (its FAT chain is not one run)" :
                                       "its FAT chain does not end at the file's last cluster";
            p->done = 1;
            return 1;
        }
    }
    p->done = p->i >= p->n;
    return p->done;
}

static const char *gcard_proof_result(const struct gcard_proof *p, const struct exfat *fs, uint64_t *sector,
                                      uint64_t *sectors)
{
    *sector = *sectors = 0;
    if (!p->done)
        return "not proven yet";
    if (p->why)
        return p->why;
    uint64_t s0 = fs->vol + fs->heap_off + ((uint64_t)(p->e.first - 2) << fs->spc_shift);
    uint64_t ns = (p->e.size / 4096U) * 8U;
    if (fs->heap_off == 0 || s0 < p->p_start || s0 + ns < s0 || s0 + ns > p->p_start + p->p_len)
        return "the file's sectors are outside the exFAT partition";
    *sector = s0;
    *sectors = ns;
    return 0;
}

/* The file at path as one run of card sectors: *sector, *sectors (the size rounded down to 4 KiB). NULL when the
 * run is proven, else the reason. fs: mounted on the partition at p_start (p_len sectors); sec512: a sector
 * buffer (8-byte aligned; the chain is read one FAT sector at a time through it). */
static __attribute__((unused)) const char *gcard_extent(struct exfat *fs, uint64_t p_start, uint64_t p_len, const char *path,
                                uint8_t *sec512, uint64_t *sector, uint64_t *sectors)
{
    static struct exfat_ent hit[1];
    struct exfat_ent e;
    struct gcard_proof p;
    *sector = *sectors = 0;
    if (!fs->ok || fs->vol != p_start)
        return "the exFAT volume is not mounted";
    if (exfat_walk(fs, path, &e, hit, 1))
        return "the file is not on the card";
    gcard_proof_begin(&p, fs, p_start, p_len, &e);
    while (!gcard_proof_step(&p, fs, sec512, 1))
        ;
    return gcard_proof_result(&p, fs, sector, sectors);
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

/* Before each load (after cfg_init): which block devices this guest gets, in steps so the UI keeps drawing
 * (cp7i: the whole offer in one loop turn froze Starbite OS for 7.6 s on cp7g):
 *   gcard_begin()  the card, its partition, the exFAT mount (a few sector reads; the card's first start once)
 *   gcard_tick()   one step per call: the folder /starbite/linux listed once, then per file one batch of FAT
 *                  sectors (GC_FAT_BATCH in one read) or its cached proof; 1 when done (sbg_set_blk called)
 *   gcard_abort()  a stop during the steps: the grant is withdrawn, nothing offered
 * Always leaves sbghost with a consistent table (none when the card or the base image is missing).
 * The proof cache: a file proven by walking its FAT chain is remembered in the prefs (linux.cardp.<name>: the
 * card's CID, the first cluster, the size and the modified stamp); while all of those are the same the walk is
 * skipped. A file put back on the card (copied again) has a new stamp or place, so it is walked again; a file
 * with NoFatChain needs no walk. Self-test images keep nothing. */
#define GC_FAT_BATCH 32U                    /* FAT sectors per read (16 KiB, ~5.5 ms from the card: 4096 links) */
#define GC_LIST 24
enum { GC_IDLE, GC_DIR, GC_PROVE, GC_DONE };
static const char *const gc_path[SBSHM_BLK_DEVS] = {GCARD_BASE, GCARD_OVERLAY, GCARD_REPO, GCARD_APPSTORE};
static const char *const gc_label[SBSHM_BLK_DEVS] = {"base", "overlay", "repo", "appstore"};
static struct {
    int phase;
    unsigned f;                             /* the file being proven */
    int begun;                              /* gc_pr holds f's proof */
    uint64_t p_start, p_len;
    struct gcard_proof pr;
    struct exfat_ent ent[SBSHM_BLK_DEVS];
    int have[SBSHM_BLK_DEVS];
    uint32_t reads, cached, walked;         /* FAT reads; proofs from the cache; chains walked */
} gc;
static struct exfat_ent gc_lst[GC_LIST];

static void gc_cache_val(struct ub *u, const struct exfat_ent *e)
{
    ub_init(u);
    for (unsigned i = 0; i < 4; ++i)
        ub_hexn(u, gc_cid[i], 8);
    ub_s(u, ".");
    ub_hexn(u, e->first, 8);
    ub_s(u, ".");
    ub_hexn(u, e->size, 16);
    ub_s(u, ".");
    ub_hexn(u, e->mtime, 8);
}
static void gc_cache_key(struct ub *u, unsigned f)
{
    ub_init(u);
    ub_s(u, "linux.cardp.");
    ub_s(u, gc_label[f]);
}
static int gc_cache_hit(unsigned f, const struct exfat_ent *e)
{
    struct ub k, v;
    char have[PREFS_VAL_MAX + 1];
    gc_cache_key(&k, f);
    gc_cache_val(&v, e);
    return prefs_get(k.b, have, sizeof have) > 0 && sb_streq(have, v.b);
}
static void gc_cache_keep(unsigned f, const struct exfat_ent *e)
{
    struct ub k, v;
    if (sbos_selftest_enabled)
        return;                             /* a self-test image keeps nothing */
    gc_cache_key(&k, f);
    gc_cache_val(&v, e);
    if (prefs_set(k.b, v.b) > 0)
        settings_changed_soon(5000);
}

static void gc_reset(void)
{
    gc_writable = 0;
    gc_errs = 0;
    sb_card_grant_clear();                  /* a grant from an earlier start is asked for again */
    for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i) {
        gc_dev[i] = (struct sbshm_blkdev){0};
        gc_ext[i].sector = gc_ext[i].sectors = 0;
        gc.have[i] = 0;
    }
    gc.f = 0;
    gc.begun = 0;
    gc.reads = gc.cached = gc.walked = 0;
}

/* Track A 9c (main 11-15: S9 #2's card had no Linux files and the Linux apps said "Starting Linux" for ever): why this
 * start has no root, in the user's words (GCARD_NO_*), or NULL; guest.c fails the start with it instead of booting a
 * guest that waits for a disk that is not there */
#define GCARD_NO_LINUX "Linux isn't installed on this card."
#define GCARD_NO_CARD "There's no microSD card, or it didn't start."
#define GCARD_NO_FS "The microSD card isn't exFAT."
#define GCARD_NO_CHECK "Linux's files on this card didn't check out."
static const char *gc_none;
static const char *gcard_missing(void)
{
#ifdef GUEST_WL_IMAGE                       /* (only the Linux apps' guest has its root on the card) */
    return mode == GUEST_WL && gc.phase == GC_DONE ? gc_none : 0;
#else
    return 0;
#endif
}

static void gc_finish(void)
{
    if (!(gc_dev[SBSHM_BLK_BASE].flags & SBSHM_BLKDEV_PRESENT)) {    /* no root: offer nothing at all */
        gc_none = gc.have[SBSHM_BLK_BASE] ? GCARD_NO_CHECK : GCARD_NO_LINUX;
        sb_card_grant_clear();
        gc_writable = 0;
        for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i)
            gc_dev[i] = (struct sbshm_blkdev){0};
    }
    sbg_set_blk(gc_dev);
    gc.phase = GC_DONE;
}

static void gcard_begin(void)
{
    const char *no = 0;
    int card = 0;
    gc_reset();
    gc.phase = GC_DONE;
    gc_none = 0;
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
    gc.p_start = gc.p_len = 0;
    if (!gc_buf)
        no = "no memory for the cluster buffer";
    else if (!(card = sd_up()))
        no = "no card, or it did not start";
    else if (!(no = gcard_partition(gc_rd, (uint8_t *)gc_sec, &gc.p_start, &gc.p_len)) &&
             exfat_mount(&gc_fs, gc_rd, gc.p_start, gc_buf, 1U << 17))
        no = "the partition does not mount as exFAT (512-byte sectors, clusters up to 128 KiB)";
    if (no) {
        gc_say(": ", "no Linux block devices", no);
        gc_none = !gc_buf ? GCARD_NO_CHECK : !card ? GCARD_NO_CARD : GCARD_NO_FS;
        sbg_set_blk(gc_dev);
        return;
    }
    for (unsigned i = 0; i < 4; ++i)
        gc_cid[i] = sd_rep.cid[i];
    gc.phase = GC_DIR;
}

/* the folder once: its entries matched by name (a folder of more than GC_LIST entries: the rest by walking) */
static void gc_dir(void)
{
    static struct exfat_ent hit[1];
    struct exfat_ent d;
    int total = 0, n = -1;
    if (!exfat_walk(&gc_fs, "/starbite/linux", &d, hit, 1) && d.dir)
        n = exfat_list(&gc_fs, &d, gc_lst, GC_LIST, &total);
    for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i) {
        const char *name = gc_path[i] + sizeof(GCARD_DIR) - 1;
        for (int k = 0; k < n && !gc.have[i]; ++k) {
            const char *a = gc_lst[k].name, *b = name;
            while (*a && *b && (*a | 0x20) == (*b | 0x20))
                ++a, ++b;
            if (!*a && !*b) {
                gc.ent[i] = gc_lst[k];
                gc.have[i] = 1;
            }
        }
        if (!gc.have[i] && total > n && !exfat_walk(&gc_fs, gc_path[i], &gc.ent[i], hit, 1))
            gc.have[i] = 1;
    }
    gc.phase = GC_PROVE;
}

/* file f's verdict: offered (the overlap check, the overlay's grant) or said why not */
static void gc_take(unsigned i, const char *no, uint64_t s, uint64_t n)
{
    for (unsigned k = 0; !no && k < i; ++k)
        if (gcard_overlap(s, n, gc_ext[k].sector, gc_ext[k].sectors))
            no = "it shares sectors with another of the card files";
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
        gc_say(" not offered: ", gc_path[i], no);
        return;
    }
    gc_ext[i].sector = s;
    gc_ext[i].sectors = n;
    gc_dev[i].sectors = n;
    gc_dev[i].flags = SBSHM_BLKDEV_PRESENT | (i == SBSHM_BLK_OVERLAY ? 0U : SBSHM_BLKDEV_RO);
    sb_strcpy(gc_dev[i].name, sizeof(gc_dev[i].name), gc_label[i]);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "guest: card: ");
    ub_s(&u, gc_path[i]);
    ub_s(&u, i == SBSHM_BLK_OVERLAY ? " read-write, sectors " : " read-only, sectors ");
    ub_dec(&u, s);
    ub_s(&u, " + ");
    ub_dec(&u, n);
    sb_log(u.b);
}

static int gcard_tick(void)
{
    if (gc.phase == GC_DONE || gc.phase == GC_IDLE)
        return 1;
    hw_wdt_feed();
    if (gc.phase == GC_DIR) {
        gc_dir();
        return 0;
    }
    /* GC_PROVE: one file's step */
    unsigned i = gc.f;
    if (i >= SBSHM_BLK_DEVS) {
        gc_finish();
        return 1;
    }
    uint64_t s = 0, n = 0;
    if (!gc.have[i]) {
        gc_take(i, "the file is not on the card", 0, 0);
        gc.f++;
        return 0;
    }
    if (!gc.begun) {
        gcard_proof_begin(&gc.pr, &gc_fs, gc.p_start, gc.p_len, &gc.ent[i]);
        gc.begun = 1;
        if (!gc.pr.done && gc_cache_hit(i, &gc.ent[i])) {
            gc.pr.done = 1;                 /* proven on an earlier start: same card, place, size and stamp */
            gc.cached++;
        }
    }
    if (!gc.pr.done) {
        gcard_proof_step(&gc.pr, &gc_fs, gc_buf, GC_FAT_BATCH);
        gc.reads++;
        if (!gc.pr.done)
            return 0;                       /* the next batch at the next tick */
        gc.walked++;
        if (!gc.pr.why)
            gc_cache_keep(i, &gc.ent[i]);
    }
    const char *no = gcard_proof_result(&gc.pr, &gc_fs, &s, &n);
    gc_take(i, no, s, n);
    gc.f++;
    gc.begun = 0;
    return 0;
}

static void gcard_abort(void)
{
    if (gc.phase != GC_IDLE && gc.phase != GC_DONE)
        sb_card_grant_clear();
    gc.phase = GC_IDLE;
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
