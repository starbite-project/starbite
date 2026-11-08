/* SPDX-License-Identifier: GPL-2.0-only
 * Copied from starbite/wifi/ext4.c (branch claude/wifi-core-c56c49, f52f7a9) for the modem glue (EFS nv_data.bin).
 * bcmw: a read-only ext4 reader, just enough to read the radio's files from the phone's VENDOR
 * partition at run time (/firmware/ on LineageOS, /etc/wifi/ on Samsung's Android): the
 * superblock, group descriptors (32 or 64 bytes), inodes, extent trees (any depth, uninitialised
 * extents read as zeros), the old block maps (direct, single and double indirect), directories
 * read linearly (hashed directories' leaves are ordinary entries). Nothing is ever written.
 * No OS in it: the caller gives a function that reads bytes of the partition. Layouts from the
 * ext4 disk format (Documentation/filesystems/ext4 in Linux).
 */
#include "ext4.h"

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int rd(struct ext4 *fs, uint64_t off, void *dst, uint32_t len)
{
    return fs->read(fs->ctx, off, dst, len);
}

int ext4_open(struct ext4 *fs)
{
    uint8_t sb[256];
    uint32_t incompat;

    if (rd(fs, 1024, sb, sizeof(sb)))
        return EXT4_E_IO;
    if (g16(sb + 0x38) != 0xEF53)
        return EXT4_E_NOTEXT4;
    fs->bs = 1024u << g32(sb + 0x18);
    if (fs->bs > EXT4_MAX_BS)
        return EXT4_E_UNSUPPORTED;
    fs->first_data = g32(sb + 0x14);
    fs->bpg = g32(sb + 0x20);
    fs->ipg = g32(sb + 0x28);
    fs->inode_size = g32(sb + 0x4C) >= 1 ? g16(sb + 0x58) : 128;
    incompat = g32(sb + 0x60);
    fs->desc_size = (incompat & 0x80) && g16(sb + 0xFE) >= 64 ? g16(sb + 0xFE) : 32;   /* INCOMPAT_64BIT */
    if (!fs->ipg || !fs->bpg || fs->inode_size < 128 || fs->inode_size > 1024)
        return EXT4_E_CORRUPT;
    /* INCOMPAT_ENCRYPT (0x10000) only says files *may* be encrypted (EFS on this S9 has it): encrypted inodes
     * (EXT4_ENCRYPT_FL) are refused one by one in ext4_read and the directory walks instead. */
    (void)incompat;
    return 0;
}

int ext4_inode(struct ext4 *fs, uint32_t ino, uint8_t inode[EXT4_INODE_MAX])
{
    uint8_t d[64];
    uint32_t group, index;
    uint64_t table;

    if (ino == 0)
        return EXT4_E_CORRUPT;
    group = (ino - 1) / fs->ipg;
    index = (ino - 1) % fs->ipg;
    if (rd(fs, (uint64_t)(fs->first_data + 1) * fs->bs + (uint64_t)group * fs->desc_size, d, fs->desc_size))
        return EXT4_E_IO;
    table = g32(d + 8);
    if (fs->desc_size >= 64)
        table |= (uint64_t)g32(d + 0x28) << 32;
    return rd(fs, table * fs->bs + (uint64_t)index * fs->inode_size, inode,
              fs->inode_size < EXT4_INODE_MAX ? fs->inode_size : EXT4_INODE_MAX) ? EXT4_E_IO : 0;
}

uint64_t ext4_size(const uint8_t *inode)
{
    return g32(inode + 4) | (uint64_t)g32(inode + 0x6C) << 32;
}

/* the physical block of logical block lblk; 0 for a hole (or an uninitialised extent) */
static int bmap(struct ext4 *fs, const uint8_t *inode, uint32_t lblk, uint64_t *pblk)
{
    uint32_t flags = g32(inode + 0x20);
    *pblk = 0;
    if (flags & 0x10000000)                 /* inline data: not expected here */
        return EXT4_E_UNSUPPORTED;
    if (flags & 0x80000) {                  /* extents */
        const uint8_t *node = inode + 0x28;
        uint8_t *blk = fs->buf;
        for (unsigned level = 0; level < 6; level++) {
            uint16_t entries = g16(node + 2), depth = g16(node + 6);
            const uint8_t *e = node + 12, *pick = 0;
            if (g16(node) != 0xF30A)
                return EXT4_E_CORRUPT;
            if (depth == 0) {
                for (unsigned i = 0; i < entries; i++, e += 12) {
                    uint32_t first = g32(e), len = g16(e + 4);
                    int uninit = len > 32768;
                    if (uninit)
                        len -= 32768;
                    if (lblk >= first && lblk < first + len) {
                        if (!uninit)
                            *pblk = ((uint64_t)g16(e + 6) << 32 | g32(e + 8)) + (lblk - first);
                        return 0;
                    }
                }
                return 0;
            }
            for (unsigned i = 0; i < entries; i++, e += 12)
                if (g32(e) <= lblk)
                    pick = e;
            if (!pick)
                return 0;
            if (rd(fs, ((uint64_t)g16(pick + 8) << 32 | g32(pick + 4)) * fs->bs, blk, fs->bs))
                return EXT4_E_IO;
            node = blk;
        }
        return EXT4_E_CORRUPT;
    }
    {
        /* the old block map */
        uint32_t per = fs->bs / 4, v;
        uint8_t w[4];
        if (lblk < 12) {
            *pblk = g32(inode + 0x28 + 4 * lblk);
            return 0;
        }
        lblk -= 12;
        if (lblk < per) {
            v = g32(inode + 0x28 + 48);
            if (v && rd(fs, (uint64_t)v * fs->bs + 4ull * lblk, w, 4))
                return EXT4_E_IO;
            *pblk = v ? g32(w) : 0;
            return 0;
        }
        lblk -= per;
        if (lblk < per * per) {
            v = g32(inode + 0x28 + 52);
            if (v && rd(fs, (uint64_t)v * fs->bs + 4ull * (lblk / per), w, 4))
                return EXT4_E_IO;
            v = v ? g32(w) : 0;
            if (v && rd(fs, (uint64_t)v * fs->bs + 4ull * (lblk % per), w, 4))
                return EXT4_E_IO;
            *pblk = v ? g32(w) : 0;
            return 0;
        }
        return EXT4_E_UNSUPPORTED;
    }
}

#define ENCRYPTED(inode) ((inode)[0x21] & 0x08)    /* i_flags (le32 at 0x20) & EXT4_ENCRYPT_FL (0x800) */
int ext4_read(struct ext4 *fs, const uint8_t *inode, uint64_t off, uint8_t *dst, uint32_t len)
{
    if (ENCRYPTED(inode))                   /* ciphertext: never handed out as if it were the file */
        return EXT4_E_UNSUPPORTED;
    uint64_t size = ext4_size(inode);
    if (off > size || len > size - off)
        return EXT4_E_CORRUPT;
    while (len) {
        uint32_t lblk = (uint32_t)(off / fs->bs), in = (uint32_t)(off % fs->bs);
        uint32_t n = fs->bs - in < len ? fs->bs - in : len;
        uint64_t p;
        int e = bmap(fs, inode, lblk, &p);
        if (e)
            return e;
        if (p) {
            if (rd(fs, p * fs->bs + in, dst, n))
                return EXT4_E_IO;
        } else {
            for (uint32_t i = 0; i < n; i++)
                dst[i] = 0;
        }
        dst += n, off += n, len -= n;
    }
    return 0;
}

static int find(struct ext4 *fs, uint32_t dir, const char *name, uint32_t nl, uint32_t *ino)
{
    uint8_t inode[EXT4_INODE_MAX];
    uint64_t size;
    int e = ext4_inode(fs, dir, inode);

    if (e)
        return e;
    if ((g16(inode) & 0xF000) != 0x4000)
        return EXT4_E_NOTFOUND;
    size = ext4_size(inode);
    for (uint64_t off = 0; off < size; off += fs->bs) {
        uint8_t *b = fs->dir;
        uint32_t at = 0;
        if ((e = ext4_read(fs, inode, off, b, fs->bs)))
            return e;
        while (at + 8 <= fs->bs) {
            uint32_t i = g32(b + at), rl = g16(b + at + 4), l = b[at + 6];
            if (rl < 8 || at + rl > fs->bs)
                break;
            if (i && l == nl && at + 8 + l <= fs->bs) {
                uint32_t k = 0;
                while (k < nl && b[at + 8 + k] == (uint8_t)name[k])
                    k++;
                if (k == nl) {
                    *ino = i;
                    return 0;
                }
            }
            at += rl;
        }
    }
    return EXT4_E_NOTFOUND;
}

int ext4_lookup(struct ext4 *fs, const char *path, uint32_t *ino)
{
    uint32_t cur = 2;
    while (*path) {
        uint32_t n = 0;
        int e;
        while (*path == '/')
            path++;
        while (path[n] && path[n] != '/')
            n++;
        if (!n)
            break;
        if ((e = find(fs, cur, path, n, &cur)))
            return e;
        path += n;
    }
    *ino = cur;
    return 0;
}

/* Added for the modem glue: the physical block of an inode's logical block (0 = a hole), and a directory
 * listing (names and inode numbers only). */
int ext4_bmap(struct ext4 *fs, const uint8_t *inode, uint32_t lblk, uint64_t *pblk)
{
    return bmap(fs, inode, lblk, pblk);
}

int ext4_list(struct ext4 *fs, uint32_t dir, void (*cb)(void *ctx, const char *name, unsigned len, uint32_t ino),
              void *ctx)
{
    uint8_t inode[EXT4_INODE_MAX];
    int e = ext4_inode(fs, dir, inode);
    if (e)
        return e;
    if ((g16(inode) & 0xF000) != 0x4000)
        return EXT4_E_NOTFOUND;
    uint64_t size = ext4_size(inode);
    for (uint64_t off = 0; off < size; off += fs->bs) {
        uint8_t *b = fs->dir;
        uint32_t at = 0;
        if ((e = ext4_read(fs, inode, off, b, fs->bs)))
            return e;
        while (at + 8 <= fs->bs) {
            uint32_t i = g32(b + at), rl = g16(b + at + 4), l = b[at + 6];
            if (rl < 8 || at + rl > fs->bs)
                break;
            if (i && l && at + 8 + l <= fs->bs)
                cb(ctx, (const char *)b + at + 8, l, i);
            at += rl;
        }
    }
    return 0;
}

