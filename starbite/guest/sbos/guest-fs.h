/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS's files for the Linux guest (sbp9, ../sbshm-p9.h): a READ-ONLY struct sbg_fs (../host/sbg_p9.h) over
 * the microSD card's exFAT volume through kern/files.c (kfile_state, kfile_mount, kfile_list_at, kfile_read_at), the
 * same reader the Files / Photos / Music apps use. The guest sees it at /home/user/Starbite (rootfs/sbp9-mount).
 *
 * Wiring (guest.c already has the hook): included after kern.h and sbghost.h, it defines GUEST_FS_OPS, so the shim's
 * `.fs = GUEST_FS_OPS` offers SBSHM_SVC_P9. Header-only; everything is static.
 *
 * Semantics:
 *   - paths come from the server canonical ("/", "/DCIM/x.jpg"): files.c's own format (exfat_walk is ASCII
 *     case-insensitive; names are UTF-8 with '?' for what exfat.c cannot spell: such a name lists but never walks);
 *   - kfile_list_at / kfile_read_at answer SBOS_EAGAIN until the card is mounted. The first stat of the root (the
 *     guest's Tattach, at its mount) calls kfile_mount() when kfile_state() says "not tried": that brings the card
 *     up and blocks (normally short: gcard_offer() brought the card up for the guest's block devices already); a
 *     card that failed is retried by kfile_state's own 30 s rule. No card: every call answers EIO;
 *   - stat: a missing name costs one walk (kfile_read_at / kfile_list_at with length 0 say file / folder / none);
 *     a present one is found in its parent's listing (sizes and dates live there), which also fills a small cache
 *     (GFS_CACHE entries, by a 64-bit hash of the path) so `ls -l` after a listing does not list again. The card is
 *     never written by Starbite OS, so the cache stays true; it is dropped whenever the card is not mounted;
 *   - dates: exFAT's DOS stamp (local time) is shown as if UTC;
 *   - reads at an offset that is not a multiple of 512 read the head through a small buffer, so the bulk always goes
 *     sector-aligned straight into the request's slot (files.c's fast path); the server's iounit keeps Linux's
 *     reads 4 KiB aligned anyway.
 * Writes: none (writable = 0: the server answers EROFS, the guest mounts read-only). When files.c learns to write,
 * fill in write / create / mkdir / remove / rename / truncate and set writable.
 */
#ifndef GUEST_FS_H
#define GUEST_FS_H
#include <stdint.h>
#include "../host/sbg_p9.h"

#define GFS_PAGE  16U                   /* entries per kfile_list_at call */
#define GFS_CACHE 512U                  /* attribute cache entries (direct-mapped by path hash) */

static struct kfile_ent gfs_page[GFS_PAGE];
static struct {
    uint64_t h;                         /* FNV-1a 64 of the path; 0 = empty */
    struct sbg_fs_attr a;
} gfs_cache[GFS_CACHE];
static int gfs_cache_live;
static uint8_t gfs_head[512];           /* the unaligned head of a read */

static uint64_t gfs_hash(uint64_t h, const char *s)
{
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h ? h : 1;
}
#define GFS_H0 0xcbf29ce484222325ULL

static int gfs_err(int r)               /* files.c's answers -> Linux errno */
{
    return r == SBOS_EINVAL ? -SBG_FS_ENOENT : -SBG_FS_EIO;     /* EINVAL: the walk failed; EAGAIN: no card */
}

/* the card mounted (1), or why not (-EIO); drops the cache when it is not */
static int gfs_ready(int may_mount)
{
    int s = kfile_state(0);
    if (s == 0 && may_mount)
        s = kfile_mount();
    if (s != 1) {
        if (gfs_cache_live)
            for (unsigned i = 0; i < GFS_CACHE; ++i)
                gfs_cache[i].h = 0;
        gfs_cache_live = 0;
        return -SBG_FS_EIO;
    }
    return 1;
}

/* days since 1970-01-01 of a civil date (H. Hinnant's days_from_civil) */
static int64_t gfs_days(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static int64_t gfs_time(uint32_t t)     /* exFAT's DOS stamp -> seconds (local time taken as UTC); 0 none */
{
    unsigned mon = (t >> 21) & 15U, day = (t >> 16) & 31U;
    if (!t || !mon || mon > 12 || !day)
        return 0;
    return gfs_days((int)(t >> 25) + 1980, mon, day) * 86400 + ((t >> 11) & 31U) * 3600 + ((t >> 5) & 63U) * 60 +
           (t & 31U) * 2;
}

static void gfs_attr(const struct kfile_ent *e, struct sbg_fs_attr *a)
{
    *a = (struct sbg_fs_attr){0};
    a->mode = e->dir ? SBG_FS_S_IFDIR | 0755U : SBG_FS_S_IFREG | 0644U;
    a->size = e->dir ? 0 : e->size;
    a->mtime = gfs_time(e->mtime);
}
static void gfs_remember(uint64_t h, const struct sbg_fs_attr *a)
{
    gfs_cache[h % GFS_CACHE].h = h;
    gfs_cache[h % GFS_CACHE].a = *a;
    gfs_cache_live = 1;
}
static int gfs_same(const char *a, const char *b)       /* ASCII case-insensitive, as exfat_walk */
{
    for (;; ++a, ++b) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

static int gfs_stat(void *ctx, const char *path, struct sbg_fs_attr *a)
{
    (void)ctx;
    int r = gfs_ready(path[0] == '/' && !path[1]);
    if (r < 0)
        return r;
    if (path[0] == '/' && !path[1]) {
        *a = (struct sbg_fs_attr){0};
        a->mode = SBG_FS_S_IFDIR | 0755U;
        return 0;
    }
    uint64_t h = gfs_hash(GFS_H0, path);
    if (gfs_cache[h % GFS_CACHE].h == h) {
        *a = gfs_cache[h % GFS_CACHE].a;
        return 0;
    }
    /* there at all? one walk each way (a missing name never lists its parent) */
    static uint8_t none;
    int is_file = kfile_read_at(path, 0, &none, 0) >= 0;
    if (!is_file && kfile_list_at(path, gfs_page, 0, 0) < 0)
        return -SBG_FS_ENOENT;
    /* its parent's listing has the size and the date */
    static char parent[SBG_P9_PATH];
    unsigned n = 0, slash = 0;
    for (; path[n] && n < sizeof(parent) - 1; ++n)
        if ((parent[n] = path[n]) == '/')
            slash = n;
    if (path[n])
        return -SBG_FS_ENAMETOOLONG;
    parent[slash ? slash : 1] = 0;
    const char *name = path + slash + 1;
    uint64_t hp = gfs_hash(GFS_H0, parent);
    for (int first = 0;; first += (int)GFS_PAGE) {
        int k = kfile_list_at(parent, gfs_page, (int)GFS_PAGE, first);
        if (k < 0)
            return gfs_err(k);
        for (int i = 0; i < k; ++i) {
            struct sbg_fs_attr ea;
            gfs_attr(&gfs_page[i], &ea);
            gfs_remember(gfs_hash(slash ? gfs_hash(hp, "/") : hp, gfs_page[i].name), &ea);
            if (gfs_same(gfs_page[i].name, name) && (gfs_page[i].dir == 0) == is_file) {
                *a = ea;
                return 0;
            }
        }
        if (k < (int)GFS_PAGE)
            break;
    }
    /* walked but not listed (a name exfat.c spells with '?'): say what the walk knows */
    *a = (struct sbg_fs_attr){0};
    a->mode = is_file ? SBG_FS_S_IFREG | 0644U : SBG_FS_S_IFDIR | 0755U;
    return 0;
}

static int gfs_list(void *ctx, const char *dir, uint32_t first, struct sbg_fs_dirent *out, unsigned max)
{
    (void)ctx;
    int r = gfs_ready(0);
    if (r < 0)
        return r;
    uint64_t hd = gfs_hash(GFS_H0, dir);
    int root = dir[0] == '/' && !dir[1];
    unsigned k = 0;
    while (k < max) {
        unsigned want = max - k < GFS_PAGE ? max - k : GFS_PAGE;
        int n = kfile_list_at(dir, gfs_page, (int)want, (int)(first + k));
        if (n < 0)
            return k ? (int)k : gfs_err(n);
        for (int i = 0; i < n; ++i, ++k) {
            unsigned j = 0;
            for (; j < SBG_FS_NAME_MAX && gfs_page[i].name[j]; ++j)
                out[k].name[j] = gfs_page[i].name[j];
            out[k].name[j] = 0;
            gfs_attr(&gfs_page[i], &out[k].a);
            gfs_remember(gfs_hash(root ? hd : gfs_hash(hd, "/"), out[k].name), &out[k].a);
        }
        if ((unsigned)n < want)
            break;                      /* the folder's end */
    }
    return (int)k;
}

static int gfs_read(void *ctx, const char *path, uint64_t off, void *dst, uint32_t len)
{
    (void)ctx;
    int r = gfs_ready(0);
    if (r < 0)
        return r;
    uint32_t head = (uint32_t)((512U - off % 512U) % 512U), done = 0;
    if (head > len)
        head = len;
    if (head) {                         /* the bytes up to the next sector boundary, through gfs_head */
        int n = kfile_read_at(path, off, gfs_head, head);
        if (n < 0)
            return gfs_err(n);
        volatile uint8_t *d = (volatile uint8_t *)dst;
        for (int i = 0; i < n; ++i)
            d[i] = gfs_head[i];
        if ((uint32_t)n < head)
            return n;                   /* the file's end */
        done = head;
    }
    if (done < len) {                   /* sector-aligned from here: straight into the slot */
        int n = kfile_read_at(path, off + done, (uint8_t *)dst + done, len - done);
        if (n < 0)
            return done ? (int)done : gfs_err(n);
        done += (uint32_t)n;
    }
    return (int)done;
}

static const struct sbg_fs gfs_card = {
    .ctx = 0,
    .writable = 0,
    .uid = 1000, .gid = 1000,           /* the desktop user (rootfs/base-customize.sh) */
    .name = "Starbite card (read-only)",
    .stat = gfs_stat, .list = gfs_list, .read = gfs_read,
};
#define GUEST_FS_OPS (&gfs_card)
#endif
