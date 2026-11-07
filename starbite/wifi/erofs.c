/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw: a read-only EROFS reader for the radio's files in VENDOR. S9 #1's One UI 8 VENDOR is EROFS,
 * 4 KiB blocks, lz4hc with compact indexes (UN1CA's build_fs_image.sh: mkfs.erofs -z lz4hc,9 -b 4096);
 * LineageOS's is ext4 (ext4.c). Supported: compact (32-byte) and extended (64-byte) inodes, inline
 * xattrs skipped, flat plain and flat inline data, compressed files with full or compact (4B, 4B+2B)
 * indexes, one-block physical clusters, LZ4 heads (with or without zero padding) and plain
 * ("shifted") clusters. Refused with a clear error: big pclusters, ztailpacking, fragments,
 * interlaced clusters, other algorithms, chunk-based files. Nothing is ever written.
 *
 * The on-disk format is Linux's fs/erofs/erofs_fs.h; the compact index decoding follows
 * z_erofs_load_compact_lcluster() in fs/erofs/zmap.c (GPL-2.0, Linux 7.1), the zero padding
 * z_erofs_fixup_insize() in decompressor.c; LZ4's block format is its own specification.
 */
#include "erofs.h"

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int rd(struct erofs *fs, uint64_t off, void *dst, uint32_t len)
{
    return fs->read(fs->ctx, off, dst, len) ? EROFS_E_IO : 0;
}

int erofs_open(struct erofs *fs)
{
    uint8_t sb[128];
    if (rd(fs, 1024, sb, sizeof(sb)))
        return EROFS_E_IO;
    if (g32(sb) != 0xE0F5E1E2)
        return EROFS_E_NOTEROFS;
    fs->blkbits = sb[12];
    if (fs->blkbits < 9 || (1u << fs->blkbits) > EROFS_MAX_BS)
        return EROFS_E_UNSUPPORTED;
    fs->bs = 1u << fs->blkbits;
    fs->root = g16(sb + 14);
    fs->meta = g32(sb + 40);
    fs->incompat = g32(sb + 80);
    /* erofs_fs.h: 0x1 LZ4 zero padding, 0x2 compression configs / big pclusters, 0x4 chunked files,
     * 0x8 device table / HEAD2, 0x10 ztailpacking, 0x20 fragments / dedupe, 0x40 xattr prefixes,
     * 0x80 48-bit, 0x100 metabox. Per-file features (big pclusters, chunks, ztailpacking, fragments)
     * are refused where a file uses them; 48-bit addresses and the metabox change where everything
     * is, so they refuse the whole filesystem. */
    if (fs->incompat & (0x80u | 0x100u))
        return EROFS_E_UNSUPPORTED;
    if (fs->incompat & ~0x7Fu)
        return EROFS_E_UNSUPPORTED;
    if (g16(sb + 86))                       /* extra devices: data elsewhere */
        return EROFS_E_UNSUPPORTED;
    return 0;
}

int erofs_inode(struct erofs *fs, uint64_t nid, struct erofs_inode *in)
{
    uint8_t b[64];
    uint16_t fmt, xc;

    in->nid = nid;
    in->iloc = ((uint64_t)fs->meta << fs->blkbits) + (nid << 5);
    if (rd(fs, in->iloc, b, 32))
        return EROFS_E_IO;
    fmt = g16(b);
    in->extended = fmt & 1;
    in->layout = (fmt >> 1) & 7;
    xc = g16(b + 2);
    in->mode = g16(b + 4);
    if (in->extended) {
        if (rd(fs, in->iloc, b, 64))
            return EROFS_E_IO;
        in->size = g32(b + 8) | (uint64_t)g32(b + 12) << 32;
        in->isize = 64;
    } else {
        in->size = g32(b + 8);
        in->isize = 32;
    }
    in->startblk = g32(b + 16);
    in->xsize = xc ? 12 + 4u * (xc - 1u) : 0;
    return 0;
}

/* ---- flat files ------------------------------------------------------------------------------ */

static int read_flat(struct erofs *fs, const struct erofs_inode *in, uint64_t off, uint8_t *dst, uint32_t len)
{
    uint64_t nblocks = (in->size + fs->bs - 1) >> fs->blkbits;
    while (len) {
        uint64_t lblk = off >> fs->blkbits;
        uint32_t o = (uint32_t)(off & (fs->bs - 1)), n = fs->bs - o < len ? fs->bs - o : len;
        uint64_t pos;
        if (in->layout == 2 && lblk == nblocks - 1 && (in->size & (fs->bs - 1)))
            pos = in->iloc + in->isize + in->xsize + o;            /* the tail, inline after the inode */
        else
            pos = ((uint64_t)(in->startblk + lblk) << fs->blkbits) + o;
        if (rd(fs, pos, dst, n))
            return EROFS_E_IO;
        dst += n, off += n, len -= n;
    }
    return 0;
}

/* ---- LZ4 (block format) ------------------------------------------------------------------------ */

/* decodes until `want` bytes are out (a partial decode may stop inside a sequence); the number out,
 * or -1 for a malformed stream */
int erofs_lz4(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t want)
{
    uint32_t ip = 0, op = 0;
    while (ip < inlen && op < want) {
        uint8_t token = in[ip++];
        uint32_t lit = token >> 4, mlen, off;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= inlen)
                    return -1;
                b = in[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > inlen - ip)
            return -1;
        for (uint32_t i = 0; i < lit && op < want; i++)
            out[op++] = in[ip + i];
        ip += lit;
        if (op >= want || ip >= inlen)
            break;
        if (inlen - ip < 2)
            return -1;
        off = g16(in + ip);
        ip += 2;
        if (off == 0 || off > op)
            return -1;
        mlen = (token & 15) + 4u;
        if ((token & 15) == 15) {
            uint8_t b;
            do {
                if (ip >= inlen)
                    return -1;
                b = in[ip++];
                mlen += b;
            } while (b == 255);
        }
        for (uint32_t i = 0; i < mlen && op < want; i++, op++)
            out[op] = out[op - off];
    }
    return (int)op;
}

/* ---- compressed files ------------------------------------------------------------------------ */

struct lcl {
    unsigned type;          /* 0 plain head, 1 head1, 2 nonhead, 3 head2 */
    uint32_t clusterofs;
    uint32_t pblk;
};

static int load_full(struct erofs *fs, const struct erofs_inode *in, uint64_t ebase, uint32_t lcn, struct lcl *l)
{
    uint8_t d[8];
    if (rd(fs, ebase + 8 + 8ull * lcn, d, 8))
        return EROFS_E_IO;
    l->type = g16(d) & 3;
    if (l->type == 2) {
        if (g16(d + 4) & (1u << 11))
            return EROFS_E_UNSUPPORTED;                  /* a big pcluster's block count */
        return 0;
    }
    l->clusterofs = g16(d + 2);
    l->pblk = g32(d + 4);
    (void)in;
    return 0;
}

static unsigned bits(const uint8_t *in, unsigned lobits, unsigned pos, unsigned *type)
{
    uint32_t v = g32(in + pos / 8) >> (pos & 7);
    *type = (v >> lobits) & 3;
    return v & ((1u << lobits) - 1);
}

static int load_compact(struct erofs *fs, const struct erofs_inode *in, uint64_t ebase, uint16_t advise, uint32_t lcn,
                        struct lcl *l)
{
    const uint32_t lclusterbits = fs->blkbits;
    uint32_t total = (uint32_t)((in->size + fs->bs - 1) >> fs->blkbits);
    uint32_t c4 = (uint32_t)(((32 - ebase % 32) / 4) & 7), c2 = 0, shift = 2, vcnt, lobits, enc, bytes, packsz;
    uint64_t pos = ebase;
    uint8_t pack[40];
    unsigned type, lo;
    int i;

    if (lcn >= total)
        return EROFS_E_CORRUPT;
    if ((advise & 1) && c4 < total)
        c2 = (total - c4) / 16 * 16;
    if (lcn >= c4) {
        pos += 4ull * c4;
        lcn -= c4;
        if (lcn < c2) {
            shift = 1;
        } else {
            pos += 2ull * c2;
            lcn -= c2;
        }
    }
    pos += (uint64_t)lcn << shift;
    vcnt = shift == 2 ? 2 : 16;
    if (shift == 1 && lclusterbits > 12)
        return EROFS_E_UNSUPPORTED;
    packsz = vcnt << shift;
    lobits = lclusterbits > 12 ? lclusterbits : 12;
    enc = (packsz - 4) * 8 / vcnt;
    bytes = (uint32_t)(pos & (packsz - 1));
    if (rd(fs, pos - bytes, pack, packsz))
        return EROFS_E_IO;
    i = (int)(bytes >> shift);
    lo = bits(pack, lobits, enc * (unsigned)i, &type);
    l->type = type;
    if (type == 2) {
        if (lo & (1u << 11))
            return EROFS_E_UNSUPPORTED;
        return 0;
    }
    l->clusterofs = lo;
    {
        uint32_t nblk = 1;
        while (i > 0) {
            --i;
            lo = bits(pack, lobits, enc * (unsigned)i, &type);
            if (type == 2)
                i -= (int)lo;
            if (i >= 0)
                ++nblk;
        }
        l->pblk = g32(pack + packsz - 4) + nblk;
    }
    return 0;
}

static int read_compressed(struct erofs *fs, const struct erofs_inode *in, uint8_t *dst)
{
    uint64_t hdr = ((in->iloc + in->isize + in->xsize + 7) & ~7ull), ebase = hdr + 8;
    uint8_t h[8];
    uint16_t advise;
    uint32_t total = (uint32_t)((in->size + fs->bs - 1) >> fs->blkbits);
    struct lcl cur = { 0 }, l;
    uint64_t cur_la = 0;
    int have = 0, e;

    if (rd(fs, hdr, h, 8))
        return EROFS_E_IO;
    advise = g16(h + 4);
    if ((h[7] & 0x0f) != 0 || (h[7] & 0x80))
        return EROFS_E_UNSUPPORTED;                     /* lclusters bigger than a block; a packed file */
    if (advise & (0x2 | 0x4 | 0x8 | 0x10 | 0x20))
        return EROFS_E_UNSUPPORTED;                     /* big pclusters, ztailpacking, interlaced, fragments */
    if (in->layout == 1 && (advise & 1))
        return EROFS_E_UNSUPPORTED;                     /* extent records */
    for (uint32_t lcn = 0; lcn <= total; lcn++) {
        uint64_t la, end;
        if (lcn < total) {
            e = in->layout == 3 ? load_compact(fs, in, ebase, advise, lcn, &l) : load_full(fs, in, ebase, lcn, &l);
            if (e)
                return e;
            if (l.type == 2)
                continue;
            la = ((uint64_t)lcn << fs->blkbits) + l.clusterofs;
            if (la >= in->size)
                la = in->size;
            if (!have && la != 0)
                return EROFS_E_CORRUPT;         /* the first extent must start at 0: no gap left unread */
        } else {
            la = in->size;
        }
        if (have && la < cur_la)
            return EROFS_E_CORRUPT;             /* extents out of order */
        if (have && la > cur_la) {
            /* the extent [cur_la, la) of the head before */
            uint32_t llen = (uint32_t)(la - cur_la);
            uint8_t *blk = fs->buf;
            if (rd(fs, (uint64_t)cur.pblk << fs->blkbits, blk, fs->bs))
                return EROFS_E_IO;
            if (cur.type == 0) {                        /* plain, shifted: from the block's start */
                if (llen > fs->bs)
                    return EROFS_E_CORRUPT;
                for (uint32_t k = 0; k < llen; k++)
                    dst[cur_la + k] = blk[k];
            } else {
                unsigned alg = cur.type == 1 ? (h[6] & 15) : (h[6] >> 4);
                uint32_t skip = 0;
                if (alg != 0)
                    return EROFS_E_UNSUPPORTED;         /* LZMA, DEFLATE, ZSTD */
                if (fs->incompat & 1)
                    while (skip < fs->bs && blk[skip] == 0)
                        skip++;
                if (erofs_lz4(blk + skip, fs->bs - skip, dst + cur_la, llen) != (int)llen)
                    return EROFS_E_CORRUPT;
            }
        }
        if (lcn < total) {
            cur = l;
            cur_la = la;
            have = 1;
        }
        end = la;
        (void)end;
    }
    return have || in->size == 0 ? 0 : EROFS_E_CORRUPT;
}

int erofs_read_all(struct erofs *fs, const struct erofs_inode *in, uint8_t *dst)
{
    if (in->layout == 0 || in->layout == 2)
        return read_flat(fs, in, 0, dst, (uint32_t)in->size);
    if (in->layout == 1 || in->layout == 3) {
        for (uint64_t i = 0; i < in->size; i++)     /* nothing of the caller's memory can show through */
            dst[i] = 0;
        return read_compressed(fs, in, dst);
    }
    return EROFS_E_UNSUPPORTED;
}

/* ---- directories -------------------------------------------------------------------------------- */

static int find(struct erofs *fs, uint64_t dir, const char *name, uint32_t nl, uint64_t *nid)
{
    struct erofs_inode in;
    int e = erofs_inode(fs, dir, &in);
    if (e)
        return e;
    if ((in.mode & 0xF000) != 0x4000)
        return EROFS_E_NOTFOUND;
    if (in.layout != 0 && in.layout != 2)
        return EROFS_E_UNSUPPORTED;
    for (uint64_t off = 0; off < in.size; off += fs->bs) {
        uint32_t blen = in.size - off < fs->bs ? (uint32_t)(in.size - off) : fs->bs, count;
        uint8_t *b = fs->dir;
        if ((e = read_flat(fs, &in, off, b, blen)))
            return e;
        if (blen < 12)
            return EROFS_E_CORRUPT;
        count = g16(b + 8) / 12;
        if (!count || count * 12 > blen)
            return EROFS_E_CORRUPT;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t s = g16(b + 12 * i + 8), t = i + 1 < count ? g16(b + 12 * (i + 1) + 8) : blen;
            if (s > blen || t > blen || t < s)
                return EROFS_E_CORRUPT;
            if (i + 1 == count)
                while (t > s && b[t - 1] == 0)
                    t--;
            if (t - s == nl) {
                uint32_t k = 0;
                while (k < nl && b[s + k] == (uint8_t)name[k])
                    k++;
                if (k == nl) {
                    *nid = g32(b + 12 * i) | (uint64_t)g32(b + 12 * i + 4) << 32;
                    return 0;
                }
            }
        }
    }
    return EROFS_E_NOTFOUND;
}

int erofs_lookup(struct erofs *fs, const char *path, uint64_t *nid)
{
    uint64_t cur = fs->root;
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
    *nid = cur;
    return 0;
}
