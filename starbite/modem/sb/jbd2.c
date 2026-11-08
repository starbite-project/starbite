/* SPDX-License-Identifier: GPL-2.0-only OR MIT
 * A read-only, in-memory replay of an ext4 journal (jbd2), for reading EFS's nv_data.bin when the file system
 * was last left with transactions in its journal (Android writes EFS and is often reset without an unmount;
 * TWRP's log shows "EXT4-fs (sda3): recovery complete" when it mounts EFS). Nothing is ever written: the result
 * is a map "file system block -> journal block" that the caller's read function consults, newest committed
 * transaction first, revoked blocks dropped, escaped blocks restored. Layouts from the jbd2 on-disk format
 * (Documentation/filesystems/ext4 and include/linux/jbd2.h in Linux): all journal fields are big-endian.
 */
#include "jbd2.h"

#define JBD2_MAGIC 0xC03B3998U
#define T_DESC 1
#define T_COMMIT 2
#define T_SB1 3
#define T_SB2 4
#define T_REVOKE 5
#define F_REVOKE 0x1
#define F_64BIT 0x2
#define F_CSUM2 0x8
#define F_CSUM3 0x10
#define F_FAST 0x20
#define TAG_ESCAPE 0x1
#define TAG_SAME_UUID 0x2
#define TAG_LAST 0x8

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint8_t jb_buf[EXT4_MAX_BS];
static struct { uint64_t blk; uint32_t seq; } revs[JBD2_REVOKE_MAX];
static unsigned nrev;
static struct { uint64_t fsblk; uint32_t jb; uint8_t esc; } pend[JBD2_PENDING_MAX];
static unsigned npend;
static struct { uint64_t blk; } pend_rev[JBD2_REVOKE_MAX];
static unsigned npend_rev;

static int jread(struct ext4 *fs, const uint8_t *jin, uint32_t jb, uint64_t *pb)
{
    uint64_t p;
    if (ext4_bmap(fs, jin, jb, &p) || !p)
        return -1;
    *pb = p;
    return fs->read(fs->ctx, p * fs->bs, jb_buf, fs->bs);
}

const struct jbd2_map *jbd2_find(const struct jbd2 *j, uint64_t fsblk)
{
    for (unsigned i = 0; j && i < j->n; ++i)
        if (j->m[i].fsblk == fsblk)
            return &j->m[i];
    return 0;
}

static int revoked(uint64_t blk, uint32_t seq)
{
    for (unsigned i = 0; i < nrev; ++i)
        if (revs[i].blk == blk && (int32_t)(seq - revs[i].seq) <= 0)
            return 1;
    return 0;
}

int jbd2_scan(struct ext4 *fs, uint32_t jinum, struct jbd2 *j, struct jbd2_map *store, unsigned cap)
{
    uint8_t jin[EXT4_INODE_MAX];
    uint64_t pb;
    for (unsigned i = 0; i < sizeof(*j); ++i)
        ((uint8_t *)j)[i] = 0;
    j->m = store;
    j->cap = cap;
    nrev = npend = npend_rev = 0;
    if (!jinum || ext4_inode(fs, jinum, jin)) {
        j->why = "no journal inode";
        return JBD2_E_NOJOURNAL;
    }
    if (jread(fs, jin, 0, &pb) || be32(jb_buf) != JBD2_MAGIC) {
        j->why = "no journal superblock";
        return JBD2_E_CORRUPT;
    }
    uint32_t type = be32(jb_buf + 4);
    if (type != T_SB1 && type != T_SB2) {
        j->why = "journal superblock type";
        return JBD2_E_CORRUPT;
    }
    j->bs = be32(jb_buf + 12);
    j->maxlen = be32(jb_buf + 16);
    j->first = be32(jb_buf + 20);
    j->seq0 = be32(jb_buf + 24);
    j->start = be32(jb_buf + 28);
    j->features = type == T_SB2 ? be32(jb_buf + 40) : 0;
    if (j->bs != fs->bs || j->first == 0 || j->first >= j->maxlen) {
        j->why = "journal geometry";
        return JBD2_E_CORRUPT;
    }
    if (j->features & ~(uint32_t)(F_REVOKE | F_64BIT | F_CSUM2 | F_CSUM3 | 0x4)) {
        j->why = "journal feature not supported (fast commit?)";
        return JBD2_E_UNSUPPORTED;
    }
    if (j->start == 0)
        return 0;                      /* clean: nothing to replay */
    if (j->start < j->first || j->start >= j->maxlen) {
        j->why = "journal start";
        return JBD2_E_CORRUPT;
    }
    unsigned tagsz = j->features & F_CSUM3 ? 16 : 8 + (j->features & F_64BIT ? 4 : 0) + (j->features & F_CSUM2 ? 2 : 0);
    unsigned tail = j->features & (F_CSUM2 | F_CSUM3) ? 4 : 0;
    uint32_t jb = j->start, seq = j->seq0;
#define NEXT(b) ((b) + 1 >= j->maxlen ? j->first : (b) + 1)
    for (uint32_t steps = 0; steps < j->maxlen; ++steps) {
        if (jread(fs, jin, jb, &pb) || be32(jb_buf) != JBD2_MAGIC || be32(jb_buf + 8) != seq)
            break;
        type = be32(jb_buf + 4);
        if (type == T_DESC) {
            uint32_t at = 12, data = jb;
            for (;;) {
                if (at + tagsz > fs->bs - tail)
                    break;
                const uint8_t *t = jb_buf + at;
                uint64_t blk = be32(t);
                uint32_t flags = j->features & F_CSUM3 ? be32(t + 4) : be16(t + 6);
                if (j->features & F_64BIT)
                    blk |= (uint64_t)be32(t + 8) << 32;
                data = NEXT(data);
                if (npend >= JBD2_PENDING_MAX) {
                    j->why = "journal transaction too large";
                    return JBD2_E_UNSUPPORTED;
                }
                pend[npend].fsblk = blk;
                pend[npend].jb = data;
                pend[npend].esc = (flags & TAG_ESCAPE) != 0;
                npend++;
                at += tagsz + (flags & TAG_SAME_UUID ? 0 : 16);
                if (flags & TAG_LAST)
                    break;
            }
            jb = data;                 /* the data blocks follow the descriptor */
        } else if (type == T_REVOKE) {
            uint32_t count = be32(jb_buf + 12), w = j->features & F_64BIT ? 8 : 4;
            if (count > fs->bs)
                count = fs->bs;
            for (uint32_t at = 16; at + w <= count; at += w) {
                if (npend_rev >= JBD2_REVOKE_MAX) {
                    j->why = "too many revokes";
                    return JBD2_E_UNSUPPORTED;
                }
                pend_rev[npend_rev++].blk = w == 8 ? (uint64_t)be32(jb_buf + at) << 32 | be32(jb_buf + at + 4)
                                                   : be32(jb_buf + at);
            }
        } else if (type == T_COMMIT) {
            for (unsigned i = 0; i < npend_rev; ++i) {
                if (nrev >= JBD2_REVOKE_MAX)
                    return JBD2_E_UNSUPPORTED;
                revs[nrev].blk = pend_rev[i].blk;
                revs[nrev].seq = seq;
                nrev++;
            }
            for (unsigned i = 0; i < npend; ++i) {
                uint64_t p;
                if (ext4_bmap(fs, jin, pend[i].jb, &p) || !p)
                    return JBD2_E_CORRUPT;
                struct jbd2_map *m = (struct jbd2_map *)jbd2_find(j, pend[i].fsblk);
                if (!m) {
                    if (j->n >= j->cap) {
                        j->why = "journal map full";
                        return JBD2_E_UNSUPPORTED;
                    }
                    m = &j->m[j->n++];
                    m->fsblk = pend[i].fsblk;
                }
                m->pblk = p;
                m->seq = seq;
                m->escaped = pend[i].esc;
                j->blocks++;
            }
            j->transactions++;
            npend = npend_rev = 0;
            seq++;
        } else {
            break;
        }
        jb = NEXT(jb);
    }
#undef NEXT
    if (npend || npend_rev)
        j->dropped_incomplete = 1;     /* a transaction without its commit: ignored, as jbd2 does */
    /* revoked: a block revoked in transaction T is not replayed from transactions up to T */
    unsigned w = 0;
    for (unsigned i = 0; i < j->n; ++i) {
        if (revoked(j->m[i].fsblk, j->m[i].seq)) {
            j->skipped_revoked++;
            continue;
        }
        j->m[w++] = j->m[i];
    }
    j->n = w;
    j->replayed = 1;
    return 0;
}

void jbd2_fix_escape(uint8_t *block)
{
    block[0] = 0xC0;
    block[1] = 0x3B;
    block[2] = 0x39;
    block[3] = 0x98;
}

int jbd2_read(const struct jbd2 *j, uint32_t bs, int (*raw)(void *, uint64_t, void *, uint32_t), void *ctx,
              uint64_t off, void *dst, uint32_t len, uint8_t *scratch, uint32_t scratch_len)
{
    uint8_t *d = dst;
    if (!bs)
        return -1;
    while (len) {
        uint64_t blk = off / bs;
        uint32_t o = (uint32_t)(off % bs), k = bs - o < len ? bs - o : len;
        const struct jbd2_map *m = j ? jbd2_find(j, blk) : 0;
        if (m) {
            if (bs > scratch_len || raw(ctx, m->pblk * bs, scratch, bs) != 0)
                return -1;
            if (m->escaped)
                jbd2_fix_escape(scratch);
            for (uint32_t i = 0; i < k; ++i)
                d[i] = scratch[o + i];
        } else if (raw(ctx, off, d, k) != 0) {
            return -1;
        }
        d += k;
        off += k;
        len -= k;
    }
    return 0;
}

