/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS files (docs/STARBITE-OS.md 5: the files group), v0.2: read-only files on the microSD card's exFAT
 * partition, through libsb's card driver and the Utility OS's exFAT reader (starbite/utility/exfat.c). The card
 * is brought up on the first read, not at boot (it costs time and power). Nothing is ever written. */
#include "sb.h"
#include "sd.h"
#include "kern.h"
#include "exfat.h"

static struct exfat fs;
static uint8_t *cbuf;
static int st;                              /* 0 not tried, 1 mounted, -1 failed (not retried for 30 s) */
static uint64_t st_t;
static struct exfat_ent scratch[64];

static int card_rd(uint64_t sector, uint32_t count, void *dst)
{
    return sd_read(sector, count, (uintptr_t)dst);
}

static int mount(void)
{
    if (st > 0)
        return 1;
    if (st < 0 && sb_ms() - st_t < 30000U)
        return 0;
    st_t = sb_ms();
    if (!cbuf)
        cbuf = sb_alloc(1U << 17);
    uint64_t start;
    sd_allow();
    if (!cbuf || !sd_up()) {
        st = -1;
        sb_log(cbuf ? "files: no card" : "files: no memory for the cluster buffer (128 KiB)");
        return 0;
    }
    int r = exfat_find_volume(card_rd, cbuf, &start);
    if (!r)
        r = exfat_mount(&fs, card_rd, start, cbuf, 1U << 17);
    st = r ? -1 : 1;
    sb_log(st > 0 ? "files: card mounted (exFAT, read only)" : "files: the card has no exFAT volume");
    return st > 0;
}

/* 1.2-alpha21: a folder's entries (names, sizes; read only) for the Files app */
static struct exfat_ent lst[128];

int kfile_list(const char *path, struct sbos_dirent *out, int max, int first, int *total)
{
    struct exfat_ent d;
    const struct exfat_ent *dir = 0;
    *total = 0;
    if (!mount())
        return SBOS_EAGAIN;
    if (path[0] && !(path[0] == '/' && !path[1])) {
        if (exfat_walk(&fs, path, &d, scratch, (int)(sizeof(scratch) / sizeof(scratch[0]))) || !d.dir)
            return SBOS_EINVAL;
        dir = &d;
    }
    int all = 0;
    int n = exfat_list(&fs, dir, lst, (int)(sizeof(lst) / sizeof(lst[0])), &all);
    if (n < 0)
        return SBOS_EAGAIN;
    *total = n;                             /* the first 128 (a folder with more says so in the app) */
    int k = 0;
    for (int i = first; i < n && k < max; ++i, ++k) {
        sb_strcpy(out[k].name, sizeof(out[k].name), lst[i].name);
        out[k].size = lst[i].size;
        out[k].dir = lst[i].dir;
    }
    return k;
}

int kfile_read(const char *path, void *dst, uint32_t cap)
{
    struct exfat_ent e;
    if (!mount())
        return SBOS_EAGAIN;
    if (exfat_walk(&fs, path, &e, scratch, (int)(sizeof(scratch) / sizeof(scratch[0]))) || e.dir)
        return SBOS_EINVAL;
    uint64_t n = e.size < cap ? e.size : cap;
    if (n && exfat_read_chain(&fs, e.first, e.nofat, 0, dst, n))
        return SBOS_EAGAIN;
    return (int)n;
}

/* ---- Next (the Photos, Files and Music apps, ui/shell_media.inc through kern_next.c): the same read-only reader
 * with a status that never blocks, folders of any size a page at a time with their dates, and reads at an offset.
 * The last folder and the last file walked are kept, so a file read in pieces walks its path once. Only the card's
 * exFAT partition (MBR type 0x07) is ever read; the OS store's partition (0xDA) is never looked at. ---- */
int kfile_state(const char **why)
{
    if (why)
        *why = st > 0 ? "" : !st ? "not read yet" : !cbuf ? "The phone is out of memory for the card." :
               sd_rep.state >= 0 ? "The card has no exFAT partition." : !sd_rep.present ? "no card" :
               sd_rep.why ? sd_rep.why : "The card did not answer.";
    if (st > 0)
        return 1;
    if (!st || sb_ms() - st_t >= 30000U)
        return 0;                           /* not tried, or long enough ago to try again (a card put in since) */
    return cbuf && sd_rep.state < 0 && !sd_rep.present ? -1 : -2;      /* -1 no card, -2 a card that cannot be read */
}

int kfile_mount(void)
{
    mount();
    return kfile_state(0);
}

static struct { char path[256]; struct exfat_ent e; int ok; } last_dir, last_file;

static int path_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return !*a && !*b;
}

static int walk_cached(const char *path, int want_dir, struct exfat_ent *out)
{
    __typeof__(last_dir) *c = want_dir ? &last_dir : &last_file;
    if (c->ok && path_eq(c->path, path)) {
        *out = c->e;
        return 0;
    }
    if (exfat_walk(&fs, path, out, scratch, (int)(sizeof(scratch) / sizeof(scratch[0]))) || !out->dir != !want_dir)
        return -1;
    sb_strcpy(c->path, sizeof(c->path), path);
    c->e = *out;
    c->ok = 1;
    return 0;
}

int kfile_list_at(const char *path, struct kfile_ent *out, int max, int first)
{
    struct exfat_ent d;
    const struct exfat_ent *dir = 0;
    if (st <= 0)
        return SBOS_EAGAIN;                 /* the caller mounts first (kfile_mount, from an idle turn) */
    if (path[0] && !(path[0] == '/' && !path[1])) {
        if (walk_cached(path, 1, &d))
            return SBOS_EINVAL;
        dir = &d;
    }
    int k = 0;
    while (k < max) {
        int want = max - k < 16 ? max - k : 16;
        int n = exfat_list_from(&fs, dir, scratch, want, first + k, 0);    /* no count: it stops at the page's end */
        if (n < 0)
            return SBOS_EAGAIN;
        for (int i = 0; i < n; ++i, ++k) {
            sb_strcpy(out[k].name, sizeof(out[k].name), scratch[i].name);
            out[k].size = scratch[i].size;
            out[k].dir = scratch[i].dir;
            out[k].mtime = scratch[i].mtime;
        }
        if (n < want)
            break;
    }
    return k;                               /* fewer than max: the folder's end */
}

int kfile_read_at(const char *path, uint64_t off, void *dst, uint32_t len)
{
    struct exfat_ent e;
    if (st <= 0)
        return SBOS_EAGAIN;
    if (walk_cached(path, 0, &e))
        return SBOS_EINVAL;
    if (off >= e.size)
        return 0;
    uint64_t n = e.size - off < len ? e.size - off : len;
    /* from a sector's start: the whole sectors straight into dst (one command a run of clusters), then the file's
     * last bytes through the cluster buffer; elsewhere exfat.c reads a sector at a time (the shell never asks so) */
    uint64_t whole = off % 512U ? 0 : n & ~511ULL;
    if (whole && exfat_read_chain(&fs, e.first, e.nofat, off, dst, whole))
        return SBOS_EAGAIN;
    if (n > whole && exfat_read_chain(&fs, e.first, e.nofat, off + whole, (uint8_t *)dst + whole, n - whole))
        return SBOS_EAGAIN;
    return (int)n;
}
