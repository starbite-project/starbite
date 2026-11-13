/* SPDX-License-Identifier: GPL-2.0-only
 * Read-only exFAT for the Utility OS (Files on the microSD card): the MBR's exFAT partition, the boot
 * sector, directory listing (file / stream extension / file name entry sets), and reads at any offset that
 * follow the FAT chain or, for NoFatChain data, one contiguous run; consecutive clusters are read together.
 * Never writes. Microsoft's exFAT specification (2019): sections 3.1 (boot sector), 4 (FAT), 6/7 (entries).
 * Also built by the host test against a real mkfs.exfat image (starbite/tools/ut-host-test.c). */
#include "exfat.h"

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

int exfat_find_volume(exfat_rd rd, uint8_t *sec512, uint64_t *start)
{
    if (rd(0, 1, sec512))
        return -1;
    if (sec512[3] == 'E' && sec512[4] == 'X' && sec512[5] == 'F' && sec512[6] == 'A' && sec512[7] == 'T') {
        *start = 0;                                    /* a volume without a partition table */
        return 0;
    }
    if (sec512[510] != 0x55 || sec512[511] != 0xaa)
        return -2;
    for (int i = 0; i < 4; ++i) {
        const uint8_t *e = sec512 + 446 + 16 * i;
        if (e[4] == 0x07 && le32(e + 8)) {             /* exFAT / NTFS type: the boot sector decides */
            *start = le32(e + 8);
            return 0;
        }
    }
    return -3;
}

int exfat_mount(struct exfat *fs, exfat_rd rd, uint64_t start, uint8_t *buf, uint32_t buf_bytes)
{
    fs->ok = 0;
    fs->rd = rd;
    fs->vol = start;
    fs->buf = buf;
    fs->buf_bytes = buf_bytes;
    if (rd(start, 1, buf))
        return -1;
    if (buf[3] != 'E' || buf[4] != 'X' || buf[5] != 'F' || buf[6] != 'A' || buf[7] != 'T' ||
        buf[510] != 0x55 || buf[511] != 0xaa)
        return -2;
    fs->bps_shift = buf[108];
    fs->spc_shift = buf[109];
    if (fs->bps_shift != 9 || fs->spc_shift > 16)     /* this reader: 512-byte sectors (every SD card) */
        return -3;
    fs->fat_off = le32(buf + 80);
    fs->heap_off = le32(buf + 88);
    fs->clusters = le32(buf + 92);
    fs->root = le32(buf + 96);
    fs->cluster_bytes = 512U << fs->spc_shift;
    if (fs->cluster_bytes > buf_bytes || fs->root < 2)
        return -4;
    fs->fat_sec = 0xffffffffffffffffULL;
    fs->ok = 1;
    return 0;
}

static uint64_t cl_sector(const struct exfat *fs, uint32_t c)
{
    return fs->vol + fs->heap_off + ((uint64_t)(c - 2) << fs->spc_shift);
}

/* the next cluster in the FAT (one sector cached) */
static uint32_t fat_next(struct exfat *fs, uint32_t c)
{
    uint64_t sec = fs->vol + fs->fat_off + c / 128U;
    if (sec != fs->fat_sec) {
        if (fs->rd(sec, 1, fs->fat_cache))
            return 0xffffffffU;
        fs->fat_sec = sec;
    }
    return le32(fs->fat_cache + (c % 128U) * 4U);
}

static int valid(const struct exfat *fs, uint32_t c) { return c >= 2 && c < fs->clusters + 2; }

/* read len bytes at off of the chain starting at first (nofat: contiguous) into dst */
int exfat_read_chain(struct exfat *fs, uint32_t first, int nofat, uint64_t off, uint8_t *dst, uint64_t len)
{
    uint64_t cb = fs->cluster_bytes;
    uint32_t c = first;
    uint64_t skip = off / cb;
    if (nofat) {
        c += (uint32_t)skip;
    } else {
        for (uint64_t i = 0; i < skip; ++i) {
            if (!valid(fs, c))
                return -1;
            c = fat_next(fs, c);
        }
    }
    uint64_t in = off % cb;
    while (len) {
        if (!valid(fs, c))
            return -1;
        /* a run of consecutive clusters, as long as the request needs */
        uint32_t run = 1;
        uint64_t need = (in + len + cb - 1) / cb;
        uint32_t last = c;
        while (run < need && run < 64) {
            uint32_t n = nofat ? last + 1 : fat_next(fs, last);
            if (n != last + 1 || !valid(fs, n))
                break;
            last = n;
            ++run;
        }
        uint64_t bytes = (uint64_t)run * cb - in;
        if (bytes > len)
            bytes = len;
        /* whole sectors from the run, through the buffer when not sector aligned */
        uint64_t s0 = cl_sector(fs, c) + in / 512U;
        uint32_t lead = (uint32_t)(in % 512U);
        if (!lead && !(bytes % 512U)) {
            if (fs->rd(s0, (uint32_t)(bytes / 512U), dst))
                return -1;
        } else {
            uint64_t done = 0;
            while (done < bytes) {
                uint64_t sec = s0 + (lead + done) / 512U;
                uint32_t o = (uint32_t)((lead + done) % 512U);
                if (fs->rd(sec, 1, fs->buf))
                    return -1;
                uint64_t k = 512U - o < bytes - done ? 512U - o : bytes - done;
                for (uint64_t i = 0; i < k; ++i)
                    dst[done + i] = fs->buf[o + i];
                done += k;
            }
        }
        dst += bytes;
        len -= bytes;
        in = 0;
        c = nofat ? last + 1 : fat_next(fs, last);
    }
    return 0;
}

/* names compare case-insensitively (ASCII), as exFAT's up-case table does for these */
static int name_eq(const char *a, const char *b)
{
    while (*a && *b && (*a | 0x20) == (*b | 0x20)) {
        ++a;
        ++b;
    }
    return !*a && !*b;
}

/* the directory's entries into out (at most max); with match, only the entries of that name */
static int list_impl(struct exfat *fs, const struct exfat_ent *dir, struct exfat_ent *out, int max, int *total,
                     const char *match, int skip)
{
    uint32_t first = dir ? dir->first : fs->root;
    int nofat = dir ? dir->nofat : 0;
    uint64_t size = dir ? dir->size : 0;               /* the root: follow the chain to its end */
    int n = 0, count = 0, need = 0, got_name = 0;
    struct exfat_ent cur;
    unsigned namelen = 0, nl = 0;
    uint32_t c = first;
    uint64_t pos = 0;
    for (uint32_t guard = 0;; ++guard) {
        if (size && pos >= size)
            break;
        if (guard > fs->clusters)
            break;                                     /* a cycle in the FAT (a corrupt card) */
        if (!valid(fs, c))
            break;
        /* the cluster in pieces of at most 16 KiB, so a small folder (its end mark in the first piece) costs one
         * short read: on the phone's microSD (~2.9 MB/s) a whole 128 KiB cluster was ~45 ms (cp7q) */
        uint32_t spc = 1U << fs->spc_shift, piece = spc < 32U ? spc : 32U, pb = piece * 512U;
        for (uint32_t o = 0; o < fs->cluster_bytes; o += 32) {
            if (o % pb == 0 && fs->rd(cl_sector(fs, c) + o / 512U, piece, fs->buf))
                return -1;
            const uint8_t *e = fs->buf + o % pb;
            uint8_t t = e[0];
            if (t == 0x00)
                goto end;                              /* end of directory */
            if (t == 0x85) {                           /* file */
                for (unsigned k = 0; k < sizeof(cur); ++k)
                    ((uint8_t *)&cur)[k] = 0;
                need = e[1];
                cur.dir = (e[4] & 0x10) != 0;
                cur.mtime = le32(e + 12);              /* LastModifiedTimestamp */
                got_name = 0;
                nl = 0;
            } else if (t == 0xc0 && need) {            /* stream extension */
                cur.nofat = (e[1] & 2) != 0;
                namelen = e[3];
                cur.first = le32(e + 20);
                cur.size = le64(e + 24);
                --need;
            } else if (t == 0xc1 && need) {            /* file name: 15 UTF-16 code units */
                for (int k = 0; k < 15 && nl < namelen; ++k, ++nl) {
                    uint16_t ch = (uint16_t)(e[2 + 2 * k] | e[3 + 2 * k] << 8);
                    unsigned len = 0;
                    while (cur.name[len]) ++len;
                    if (len + 3 >= sizeof(cur.name))
                        continue;
                    if (ch < 0x80) {
                        cur.name[len] = (char)ch;
                    } else if (ch < 0x800) {
                        cur.name[len] = (char)(0xc0 | ch >> 6);
                        cur.name[len + 1] = (char)(0x80 | (ch & 0x3f));
                    } else {
                        cur.name[len] = '?';           /* outside the font's range */
                    }
                }
                if (--need == 0)
                    got_name = 1;
            } else if (need) {
                --need;                                /* other secondary entries */
                if (!need)
                    got_name = 1;
            }
            if (got_name) {
                got_name = 0;
                if (!match || name_eq(cur.name, match)) {
                    if (n < max && count >= skip)
                        out[n++] = cur;
                    if (match && n >= max)
                        goto end;                      /* found: the rest of the folder is not needed */
                }
                ++count;
                if (!match && !total && n >= max)
                    goto end;                          /* a page without the count: no need to read on */
            }
        }
        pos += fs->cluster_bytes;
        c = nofat ? c + 1 : fat_next(fs, c);
        if (!nofat && c >= 0xfffffff7U)
            break;
    }
end:
    if (total)
        *total = count;
    return n;
}

int exfat_list(struct exfat *fs, const struct exfat_ent *dir, struct exfat_ent *out, int max, int *total)
{
    return list_impl(fs, dir, out, max, total, 0, 0);
}

int exfat_list_from(struct exfat *fs, const struct exfat_ent *dir, struct exfat_ent *out, int max, int skip, int *total)
{
    return list_impl(fs, dir, out, max, total, 0, skip);
}

int exfat_walk(struct exfat *fs, const char *path, struct exfat_ent *out, struct exfat_ent *scratch, int scratch_n)
{
    struct exfat_ent dir, *dp = 0;
    const char *p = path;
    while (*p == '/')
        ++p;
    if (!*p)
        return -1;
    for (;;) {
        char part[128];
        unsigned k = 0;
        while (*p && *p != '/' && k + 1 < sizeof(part))
            part[k++] = *p++;
        part[k] = 0;
        /* matched while listing: a folder of any size, scratch holds just the hit */
        (void)scratch_n;
        if (list_impl(fs, dp, scratch, 1, 0, part, 0) < 1)
            return -2;
        int hit = 0;
        while (*p == '/')
            ++p;
        if (!*p) {
            *out = scratch[hit];
            return 0;
        }
        dir = scratch[hit];
        dp = &dir;
    }
}
