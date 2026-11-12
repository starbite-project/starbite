/* SPDX-License-Identifier: GPL-2.0-only
 * sbg_p9: the host side of sbp9 (../sbshm-p9.h): a 9P2000.L server over a struct sbg_fs (sbg_p9.h) and the channel
 * that carries it. Built inside sbghost.c (#include at its end) and by test-p9.c on its own; every name here is
 * p9_* or sbg_p9_* so it never meets sbghost.c's statics. No allocation, no libc.
 *
 * The server keeps one fid table (SBG_P9_FIDS entries, each a canonical path: the files are named by path, so a
 * rename moves the paths of every fid at or below the old name, as QEMU's local backend does). Directory offsets
 * are entry indices (0 "." / 1 ".." / 2.. the backend's list from 0), so nothing is kept per open directory.
 * Requests are parsed from a private copy of the message (the guest could change the slot under us); only Twrite's
 * payload is read in place, and Tread's data is written straight into the slot by the backend.
 */
#include "sbghost_int.h"
#include "sbg_p9.h"
#include "../sbshm-p9.h"

enum {
    P9_RLERROR = 7, P9_TSTATFS = 8, P9_TLOPEN = 12, P9_TLCREATE = 14, P9_TSYMLINK = 16, P9_TMKNOD = 18,
    P9_TRENAME = 20, P9_TREADLINK = 22, P9_TGETATTR = 24, P9_TSETATTR = 26, P9_TXATTRWALK = 30,
    P9_TXATTRCREATE = 32, P9_TREADDIR = 40, P9_TFSYNC = 50, P9_TLOCK = 52, P9_TGETLOCK = 54, P9_TLINK = 70,
    P9_TMKDIR = 72, P9_TRENAMEAT = 74, P9_TUNLINKAT = 76, P9_TVERSION = 100, P9_TAUTH = 102, P9_TATTACH = 104,
    P9_TFLUSH = 108, P9_TWALK = 110, P9_TREAD = 116, P9_TWRITE = 118, P9_TCLUNK = 120, P9_TREMOVE = 122,
};
#define P9_HDR      7U                  /* size[4] type[1] tag[2] */
#define P9_MAXWELEM 16U
#define P9_QTDIR    0x80U
#define P9_O_ACCMODE 3U
#define P9_O_EXCL   0200U
#define P9_O_TRUNC  01000U
#define P9_O_APPEND 02000U
#define P9_AT_REMOVEDIR 0x200U
#define P9_ATTR_SIZE 0x8U
#define P9_ATTR_MTIME 0x20U
#define P9_ATTR_MTIME_SET 0x100U
#define P9_GETATTR_BASIC 0x7ffULL
#define P9_IN_MAX   8192U               /* every T-message but Twrite fits (Twalk: 16 names of 255 bytes) */
#define P9_DENTS    16U                 /* directory entries asked of the backend at once */
#define P9_DIR_MAX  256U                /* entries in one Rreaddir at most (bounds a poll's work) */

struct p9_fid {
    uint32_t fid;
    uint8_t used, open;
    uint16_t pad;
    uint32_t oflags;                    /* Tlopen / Tlcreate flags (Linux O_*) */
    char path[SBG_P9_PATH];
};
static struct p9_fid p9_fids[SBG_P9_FIDS];
static unsigned p9_nfids;
static unsigned p9_msize = SBSHM_P9_SLOT;
static uint8_t p9_in[P9_IN_MAX];        /* the private copy of the T-message being served */
static char p9_names[P9_MAXWELEM][SBG_FS_NAME_MAX + 1];
static char p9_tmp[SBG_P9_PATH], p9_tmp2[SBG_P9_PATH];
static struct sbg_fs_dirent p9_dents[P9_DENTS];
static struct sbg_p9_stats p9_st;

/* ---- small helpers (no libc) ------------------------------------------------------------------------------- */
static unsigned p9_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        ++n;
    return n;
}
static void p9_strcpy(char *d, const char *s)          /* callers checked the length */
{
    while ((*d++ = *s++))
        ;
}
static int p9_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}
static uint64_t p9_hash_cont(uint64_t h, const char *s)   /* FNV-1a 64, streaming */
{
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}
#define P9_HASH0 0xcbf29ce484222325ULL
static uint64_t p9_ino(const struct sbg_fs_attr *a, uint64_t path_hash)
{
    return a->ino ? a->ino : path_hash ? path_hash : 1;
}

/* ---- parsing the private copy --------------------------------------------------------------------------------- */
struct p9_rd {
    const uint8_t *p;
    unsigned n, off;
    int bad;
};
static unsigned p9_need(struct p9_rd *r, unsigned k)
{
    if (r->bad || r->n - r->off < k || r->off > r->n) {
        r->bad = 1;
        return 0;
    }
    unsigned o = r->off;
    r->off += k;
    return o + 1;                       /* 1-based: 0 = not there */
}
static uint8_t p9_g1(struct p9_rd *r)
{
    unsigned o = p9_need(r, 1);
    return o ? r->p[o - 1] : 0;
}
static uint16_t p9_g2(struct p9_rd *r)
{
    unsigned o = p9_need(r, 2);
    return o ? (uint16_t)(r->p[o - 1] | r->p[o] << 8) : 0;
}
static uint32_t p9_g4(struct p9_rd *r)
{
    unsigned o = p9_need(r, 4);
    if (!o)
        return 0;
    const uint8_t *p = r->p + o - 1;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t p9_g8(struct p9_rd *r)
{
    uint64_t lo = p9_g4(r);
    return lo | (uint64_t)p9_g4(r) << 32;
}
/* a string into d (cap bytes with the NUL): 0, -ENAMETOOLONG (too long for d; parsing goes on), or the parse
 * failed (r->bad, return -EPROTO). NUL bytes inside are refused (-EINVAL). */
static int p9_gs(struct p9_rd *r, char *d, unsigned cap)
{
    unsigned len = p9_g2(r), o = p9_need(r, len);
    if (r->bad)
        return -SBG_FS_EPROTO;
    if (len >= cap) {
        d[0] = 0;
        return -SBG_FS_ENAMETOOLONG;
    }
    for (unsigned i = 0; i < len; ++i) {
        d[i] = (char)r->p[o - 1 + i];
        if (!d[i])
            return -SBG_FS_EINVAL;
    }
    d[len] = 0;
    return 0;
}

/* ---- writing the R-message into the slot (byte stores: unaligned fields, non-cacheable / Device memory) ------ */
struct p9_wr {
    volatile uint8_t *p;
    unsigned cap, off;
    int over;
};
static void p9_p1(struct p9_wr *w, uint32_t v)
{
    if (w->off + 1 > w->cap) {
        w->over = 1;
        return;
    }
    w->p[w->off++] = (uint8_t)v;
}
static void p9_p2(struct p9_wr *w, uint32_t v)
{
    p9_p1(w, v);
    p9_p1(w, v >> 8);
}
static void p9_p4(struct p9_wr *w, uint32_t v)
{
    p9_p2(w, v);
    p9_p2(w, v >> 16);
}
static void p9_p8(struct p9_wr *w, uint64_t v)
{
    p9_p4(w, (uint32_t)v);
    p9_p4(w, (uint32_t)(v >> 32));
}
static void p9_ps(struct p9_wr *w, const char *s)
{
    unsigned n = p9_strlen(s);
    p9_p2(w, n);
    for (unsigned i = 0; i < n; ++i)
        p9_p1(w, (uint8_t)s[i]);
}
static void p9_pqid(struct p9_wr *w, const struct sbg_fs_attr *a, uint64_t hash)
{
    p9_p1(w, (a->mode & 0170000U) == SBG_FS_S_IFDIR ? P9_QTDIR : 0);
    p9_p4(w, 0);                        /* version: no cache in the guest depends on it */
    p9_p8(w, p9_ino(a, hash));
}

/* ---- fids ----------------------------------------------------------------------------------------------------- */
static struct p9_fid *p9_fid_get(uint32_t fid)
{
    for (unsigned i = 0; i < SBG_P9_FIDS; ++i)
        if (p9_fids[i].used && p9_fids[i].fid == fid)
            return &p9_fids[i];
    return 0;
}
static struct p9_fid *p9_fid_new(uint32_t fid, int *err)
{
    if (p9_fid_get(fid)) {
        *err = -SBG_FS_EEXIST;
        return 0;
    }
    for (unsigned i = 0; i < SBG_P9_FIDS; ++i)
        if (!p9_fids[i].used) {
            struct p9_fid *f = &p9_fids[i];
            f->used = 1;
            f->open = 0;
            f->oflags = 0;
            f->fid = fid;
            f->path[0] = 0;
            if (++p9_nfids > p9_st.fids_max)
                p9_st.fids_max = p9_nfids;
            return f;
        }
    *err = -SBG_FS_ENFILE;
    return 0;
}
static void p9_fid_put(struct p9_fid *f)
{
    if (f && f->used) {
        f->used = 0;
        --p9_nfids;
    }
}
void sbg_p9_reset(void)
{
    for (unsigned i = 0; i < SBG_P9_FIDS; ++i)
        p9_fids[i].used = 0;
    p9_nfids = 0;
}

/* dir + "/" + name into d (d may be dir): 0 or -errno. The name is one component. */
static int p9_join(char *d, const char *dir, const char *name)
{
    unsigned nl = p9_strlen(name), dl = p9_strlen(dir);
    if (!nl || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
        return -SBG_FS_EINVAL;
    for (unsigned i = 0; i < nl; ++i)
        if (name[i] == '/')
            return -SBG_FS_EINVAL;
    if (nl > SBG_FS_NAME_MAX)
        return -SBG_FS_ENAMETOOLONG;
    int root = dl == 1 && dir[0] == '/';
    if (dl + 1 + nl >= SBG_P9_PATH)
        return -SBG_FS_ENAMETOOLONG;
    if (d != dir)
        p9_strcpy(d, dir);
    if (!root)
        d[dl++] = '/';
    p9_strcpy(d + dl, name);
    return 0;
}
static void p9_parent(char *p)                          /* "/a/b" -> "/a", "/a" -> "/", "/" stays */
{
    unsigned n = p9_strlen(p);
    while (n > 1 && p[n - 1] != '/')
        --n;
    p[n > 1 ? n - 1 : 1] = 0;
}

static int p9_stat(const struct sbg_fs *fs, const char *path, struct sbg_fs_attr *a)
{
    *a = (struct sbg_fs_attr){0};
    int e = fs->stat(fs->ctx, path, a);
    if (e < 0)
        return e;
    if (!(a->mode & 0170000U))
        a->mode |= SBG_FS_S_IFREG;
    return 0;
}
static int p9_isdir(const struct sbg_fs_attr *a) { return (a->mode & 0170000U) == SBG_FS_S_IFDIR; }

/* every fid at `from` or below it now names the same file under `to` (after a rename) */
static void p9_moved(const char *from, const char *to)
{
    unsigned fl = p9_strlen(from), tl = p9_strlen(to);
    for (unsigned i = 0; i < SBG_P9_FIDS; ++i) {
        struct p9_fid *f = &p9_fids[i];
        if (!f->used)
            continue;
        unsigned k = 0;
        while (k < fl && f->path[k] == from[k])
            ++k;
        if (k < fl || (f->path[k] && f->path[k] != '/'))
            continue;
        unsigned rest = p9_strlen(f->path + fl);
        if (tl + rest >= SBG_P9_PATH)
            continue;                   /* cannot be named any more: it stays at the old path (ENOENT later) */
        char *s = f->path + fl, *d = f->path + tl;
        if (tl > fl)                    /* move the tail (with its NUL) first, from the end */
            for (unsigned j = rest + 1; j-- > 0;)
                d[j] = s[j];
        else
            for (unsigned j = 0; j <= rest; ++j)
                d[j] = s[j];
        for (unsigned j = 0; j < tl; ++j)
            f->path[j] = to[j];
    }
}

/* ---- the messages ----------------------------------------------------------------------------------------- */
static uint32_t p9_mode(const struct sbg_fs *fs, const struct sbg_fs_attr *a)
{
    uint32_t m = a->mode;
    if (!(m & 07777U))
        m |= p9_isdir(a) ? 0755U : 0644U;
    if (!fs->writable)
        m &= ~0222U;
    return m;
}

/* Linux reads and writes in iounit pieces: a multiple of 4 KiB keeps them sector-aligned for the backend (the
 * card reader's fast path); 0 would mean msize - 24 */
static uint32_t p9_iounit(void)
{
    uint32_t u = (p9_msize - 24U) & ~4095U;
    return u >= 4096U ? u : 0;
}

static void p9_rgetattr(const struct sbg_fs *fs, struct p9_wr *w, const char *path, const struct sbg_fs_attr *a)
{
    uint64_t h = p9_hash_cont(P9_HASH0, path);
    p9_p8(w, P9_GETATTR_BASIC);
    p9_pqid(w, a, h);
    p9_p4(w, p9_mode(fs, a));
    p9_p4(w, fs->uid);
    p9_p4(w, fs->gid);
    p9_p8(w, a->nlink ? a->nlink : p9_isdir(a) ? 2 : 1);
    p9_p8(w, 0);                        /* rdev */
    p9_p8(w, a->size);
    p9_p8(w, 4096);                     /* blksize */
    p9_p8(w, (a->size + 511) / 512);
    for (int i = 0; i < 3; ++i) {       /* atime, mtime, ctime: the one time there is */
        p9_p8(w, (uint64_t)a->mtime);
        p9_p8(w, a->mtime_ns);
    }
    p9_p8(w, 0);                        /* btime */
    p9_p8(w, 0);
    p9_p8(w, 0);                        /* gen */
    p9_p8(w, 0);                        /* data_version */
}

static int p9_walk(const struct sbg_fs *fs, struct p9_rd *r, struct p9_wr *w)
{
    uint32_t fid = p9_g4(r), newfid = p9_g4(r);
    unsigned nw = p9_g2(r);
    if (r->bad)
        return -SBG_FS_EPROTO;
    if (nw > P9_MAXWELEM)
        return -SBG_FS_EINVAL;
    int err = 0;
    for (unsigned i = 0; i < nw; ++i) {
        int e = p9_gs(r, p9_names[i], sizeof(p9_names[i]));
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (e && !err)
            err = e;
    }
    struct p9_fid *f = p9_fid_get(fid);
    if (!f)
        return -SBG_FS_EBADF;
    if (newfid != fid && p9_fid_get(newfid))
        return -SBG_FS_EEXIST;
    p9_strcpy(p9_tmp, f->path);
    unsigned nq = 0, at = w->off;
    struct sbg_fs_attr a = {0};
    p9_p2(w, 0);                        /* nwqid, filled in below */
    for (unsigned i = 0; i < nw; ++i) {
        int e = err;
        if (!e) {
            if (p9_streq(p9_names[i], ".."))
                p9_parent(p9_tmp);
            else if (!p9_streq(p9_names[i], "."))
                e = p9_join(p9_tmp, p9_tmp, p9_names[i]);
        }
        if (!e && i && !p9_isdir(&a))
            e = -SBG_FS_ENOTDIR;        /* the previous element was a file (the first: the backend says so) */
        if (!e)
            e = p9_stat(fs, p9_tmp, &a);
        if (e) {
            if (!i)
                return e;               /* the first element: Rlerror */
            break;                      /* later: Rwalk with the qids so far, newfid untouched */
        }
        p9_pqid(w, &a, p9_hash_cont(P9_HASH0, p9_tmp));
        ++nq;
    }
    if (nq == nw) {
        struct p9_fid *nf = f;
        if (newfid != fid) {
            int e = 0;
            nf = p9_fid_new(newfid, &e);
            if (!nf)
                return e;
        }
        p9_strcpy(nf->path, p9_tmp);
        nf->open = 0;
        nf->oflags = 0;
    }
    w->p[at] = (uint8_t)nq;
    w->p[at + 1] = (uint8_t)(nq >> 8);
    return 0;
}

static int p9_open_check(const struct sbg_fs *fs, const struct sbg_fs_attr *a, uint32_t flags)
{
    uint32_t acc = flags & P9_O_ACCMODE;
    if (p9_isdir(a) && (acc || (flags & P9_O_TRUNC)))
        return -SBG_FS_EISDIR;
    if (!fs->writable && (acc || (flags & P9_O_TRUNC)))
        return -SBG_FS_EROFS;
    return 0;
}

static int p9_readdir(const struct sbg_fs *fs, struct p9_fid *f, uint64_t off, uint32_t count, struct p9_wr *w)
{
    struct sbg_fs_attr da;
    int e = p9_stat(fs, f->path, &da);
    if (e)
        return e;
    if (!p9_isdir(&da))
        return -SBG_FS_ENOTDIR;
    uint64_t hdir = p9_hash_cont(P9_HASH0, f->path);
    unsigned cnt_at = w->off;
    p9_p4(w, 0);
    unsigned start = w->off, limit = w->off + count, n = 0;
    if (limit > w->cap)
        limit = w->cap;
    uint64_t idx = off;
    for (; idx < 2 && n < P9_DIR_MAX; ++idx, ++n) {     /* "." and ".." */
        const char *nm = idx ? ".." : ".";
        struct sbg_fs_attr pa = da;
        uint64_t h = hdir;
        if (idx) {
            p9_strcpy(p9_tmp, f->path);
            p9_parent(p9_tmp);
            if (p9_stat(fs, p9_tmp, &pa))
                pa = da;
            h = p9_hash_cont(P9_HASH0, p9_tmp);
        }
        if (w->off + 24 + p9_strlen(nm) > limit)
            goto full;
        p9_pqid(w, &pa, h);
        p9_p8(w, idx + 1);
        p9_p1(w, 4);                    /* DT_DIR */
        p9_ps(w, nm);
    }
    while (n < P9_DIR_MAX) {
        if (idx - 2 > 0xffffffffULL)
            break;
        int k = fs->list(fs->ctx, f->path, (uint32_t)(idx - 2), p9_dents, P9_DENTS);
        if (k < 0) {
            if (w->off == start)
                return k;
            break;
        }
        if (k > (int)P9_DENTS)
            k = (int)P9_DENTS;
        for (int i = 0; i < k; ++i, ++idx) {
            struct sbg_fs_dirent *d = &p9_dents[i];
            d->name[SBG_FS_NAME_MAX] = 0;
            unsigned nl = p9_strlen(d->name), okname = nl > 0;
            for (unsigned j = 0; j < nl; ++j)
                okname &= d->name[j] != '/';
            if (!okname || p9_streq(d->name, ".") || p9_streq(d->name, ".."))
                continue;               /* never shown (its index still counts) */
            if (w->off + 24 + nl > limit || n >= P9_DIR_MAX)
                goto full;
            if (!(d->a.mode & 0170000U))
                d->a.mode |= SBG_FS_S_IFREG;
            uint64_t h = p9_hash_cont(hdir, (f->path[0] == '/' && !f->path[1]) ? "" : "/");
            p9_pqid(w, &d->a, p9_hash_cont(h, d->name));
            p9_p8(w, idx + 1);
            p9_p1(w, p9_isdir(&d->a) ? 4 : 8);   /* DT_DIR / DT_REG */
            p9_ps(w, d->name);
            ++n;
        }
        if (k < (int)P9_DENTS)
            break;                      /* the end */
    }
    goto done;
full:
    if (w->off == start)
        return -SBG_FS_EINVAL;          /* not even one entry fits the count (an empty answer would mean the end) */
done:;
    unsigned c = w->off - start;
    w->p[cnt_at] = (uint8_t)c;
    w->p[cnt_at + 1] = (uint8_t)(c >> 8);
    w->p[cnt_at + 2] = (uint8_t)(c >> 16);
    w->p[cnt_at + 3] = (uint8_t)(c >> 24);
    return 0;
}

/* one message: the R-message's type (its body is in w), or a negative errno for Rlerror */
static int p9_one(const struct sbg_fs *fs, uint8_t type, struct p9_rd *r, struct p9_wr *w, uint8_t *msg,
                  unsigned len, unsigned cap, unsigned msize_max)
{
    struct sbg_fs_attr a;
    struct p9_fid *f;
    int e;
    switch (type) {
    case P9_TVERSION: {
        uint32_t ms = p9_g4(r);
        char v[32];
        e = p9_gs(r, v, sizeof(v));
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (ms > msize_max)
            ms = msize_max;
        if (ms < 4096)
            return -SBG_FS_EINVAL;
        sbg_p9_reset();
        p9_msize = ms;
        p9_p4(w, ms);
        p9_ps(w, !e && v[0] == '9' && p9_streq(v, "9P2000.L") ? "9P2000.L" : "unknown");
        return type + 1;
    }
    case P9_TATTACH: {
        uint32_t fid = p9_g4(r);
        p9_g4(r);                       /* afid: no authentication */
        p9_gs(r, p9_tmp, sizeof(p9_tmp));       /* uname, aname: one share */
        p9_gs(r, p9_tmp, sizeof(p9_tmp));
        p9_g4(r);                       /* n_uname: the files have one owner (fs->uid) */
        if (r->bad)
            return -SBG_FS_EPROTO;
        if ((e = p9_stat(fs, "/", &a)))
            return e;
        if (!(f = p9_fid_new(fid, &e)))
            return e;
        p9_strcpy(f->path, "/");
        p9_pqid(w, &a, p9_hash_cont(P9_HASH0, "/"));
        return type + 1;
    }
    case P9_TWALK:
        e = p9_walk(fs, r, w);
        return e ? e : type + 1;
    case P9_TGETATTR:
        f = p9_fid_get(p9_g4(r));
        p9_g8(r);
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        if ((e = p9_stat(fs, f->path, &a)))
            return e;
        p9_rgetattr(fs, w, f->path, &a);
        return type + 1;
    case P9_TLOPEN: {
        f = p9_fid_get(p9_g4(r));
        uint32_t fl = p9_g4(r);
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        if ((e = p9_stat(fs, f->path, &a)) || (e = p9_open_check(fs, &a, fl)))
            return e;
        if ((fl & P9_O_TRUNC) && !p9_isdir(&a)) {
            if (!fs->truncate)
                return -SBG_FS_EOPNOTSUPP;
            if ((e = fs->truncate(fs->ctx, f->path, 0)) < 0)
                return e;
            a.size = 0;
        }
        f->open = 1;
        f->oflags = fl;
        p9_pqid(w, &a, p9_hash_cont(P9_HASH0, f->path));
        p9_p4(w, p9_iounit());
        return type + 1;
    }
    case P9_TLCREATE: {
        f = p9_fid_get(p9_g4(r));
        int ne = p9_gs(r, p9_names[0], sizeof(p9_names[0]));
        uint32_t fl = p9_g4(r), mode = p9_g4(r);
        p9_g4(r);                       /* gid */
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        if (!fs->writable)
            return -SBG_FS_EROFS;
        if (ne)
            return ne;
        if ((e = p9_join(p9_tmp, f->path, p9_names[0])))
            return e;
        if (!p9_stat(fs, p9_tmp, &a)) {
            if (fl & P9_O_EXCL)
                return -SBG_FS_EEXIST;
            if (p9_isdir(&a))
                return -SBG_FS_EISDIR;
            if (fl & P9_O_TRUNC) {
                if (!fs->truncate)
                    return -SBG_FS_EOPNOTSUPP;
                if ((e = fs->truncate(fs->ctx, p9_tmp, 0)) < 0)
                    return e;
            }
        } else {
            if (!fs->create)
                return -SBG_FS_EOPNOTSUPP;
            if ((e = fs->create(fs->ctx, p9_tmp, mode & 07777U)) < 0)
                return e;
        }
        if ((e = p9_stat(fs, p9_tmp, &a)))
            return e;
        p9_strcpy(f->path, p9_tmp);
        f->open = 1;
        f->oflags = fl;
        p9_pqid(w, &a, p9_hash_cont(P9_HASH0, f->path));
        p9_p4(w, p9_iounit());
        return type + 1;
    }
    case P9_TREAD: {
        f = p9_fid_get(p9_g4(r));
        uint64_t off = p9_g8(r);
        uint32_t count = p9_g4(r);
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f || !f->open)
            return -SBG_FS_EBADF;
        if ((f->oflags & P9_O_ACCMODE) == 1)
            return -SBG_FS_EBADF;       /* opened write-only */
        unsigned room = (p9_msize < cap ? p9_msize : cap) - 11;
        if (count > room)
            count = room;
        int n = fs->read(fs->ctx, f->path, off, msg + 11, count);
        if (n < 0)
            return n;
        if ((uint32_t)n > count)
            return -SBG_FS_EIO;
        p9_p4(w, (uint32_t)n);
        w->off += (unsigned)n;          /* the data is in place */
        p9_st.rd_bytes += (uint32_t)n;
        return type + 1;
    }
    case P9_TWRITE: {
        f = p9_fid_get(p9_g4(r));
        uint64_t off = p9_g8(r);
        uint32_t count = p9_g4(r);
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (count > len - 23)
            return -SBG_FS_EPROTO;
        if (!f || !f->open)
            return -SBG_FS_EBADF;
        if (!fs->writable)
            return -SBG_FS_EROFS;
        if (!(f->oflags & P9_O_ACCMODE))
            return -SBG_FS_EBADF;       /* opened read-only */
        if (!fs->write)
            return -SBG_FS_EOPNOTSUPP;
        if (f->oflags & P9_O_APPEND) {
            if ((e = p9_stat(fs, f->path, &a)))
                return e;
            off = a.size;
        }
        int n = fs->write(fs->ctx, f->path, off, msg + 23, count);
        if (n < 0)
            return n;
        if ((uint32_t)n > count)
            return -SBG_FS_EIO;
        p9_p4(w, (uint32_t)n);
        p9_st.wr_bytes += (uint32_t)n;
        return type + 1;
    }
    case P9_TREADDIR: {
        f = p9_fid_get(p9_g4(r));
        uint64_t off = p9_g8(r);
        uint32_t count = p9_g4(r);
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        unsigned room = (p9_msize < cap ? p9_msize : cap) - 11;
        e = p9_readdir(fs, f, off, count < room ? count : room, w);
        return e ? e : type + 1;
    }
    case P9_TCLUNK:
        f = p9_fid_get(p9_g4(r));
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        p9_fid_put(f);
        return type + 1;
    case P9_TREMOVE:
        f = p9_fid_get(p9_g4(r));
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        e = !fs->writable ? -SBG_FS_EROFS : !fs->remove ? -SBG_FS_EOPNOTSUPP : 0;
        if (!e && p9_streq(f->path, "/"))
            e = -SBG_FS_EBUSY;
        if (!e)
            e = p9_stat(fs, f->path, &a);
        if (!e)
            e = fs->remove(fs->ctx, f->path, p9_isdir(&a));
        p9_fid_put(f);                  /* clunked even when the remove failed */
        return e < 0 ? e : type + 1;
    case P9_TSTATFS: {
        f = p9_fid_get(p9_g4(r));
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        struct sbg_fs_statfs s = {0};
        if (fs->statfs && (e = fs->statfs(fs->ctx, &s)) < 0)
            return e;
        if (!fs->statfs) {
            s.blocks = 1U << 20;
            s.files = 1U << 20;
        }
        p9_p4(w, 0x01021997U);          /* V9FS_MAGIC */
        p9_p4(w, s.bsize ? s.bsize : 4096);
        p9_p8(w, s.blocks);
        p9_p8(w, fs->writable ? s.bfree : 0);
        p9_p8(w, fs->writable ? s.bfree : 0);
        p9_p8(w, s.files);
        p9_p8(w, fs->writable ? s.ffree : 0);
        p9_p8(w, 0x5342503900000000ULL);        /* fsid */
        p9_p4(w, SBG_FS_NAME_MAX);
        return type + 1;
    }
    case P9_TMKDIR: {
        f = p9_fid_get(p9_g4(r));
        int ne = p9_gs(r, p9_names[0], sizeof(p9_names[0]));
        uint32_t mode = p9_g4(r);
        p9_g4(r);
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        if (!fs->writable)
            return -SBG_FS_EROFS;
        if (ne)
            return ne;
        if ((e = p9_join(p9_tmp, f->path, p9_names[0])))
            return e;
        if (!fs->mkdir)
            return -SBG_FS_EOPNOTSUPP;
        if ((e = fs->mkdir(fs->ctx, p9_tmp, mode & 07777U)) < 0 || (e = p9_stat(fs, p9_tmp, &a)))
            return e;
        p9_pqid(w, &a, p9_hash_cont(P9_HASH0, p9_tmp));
        return type + 1;
    }
    case P9_TUNLINKAT: {
        f = p9_fid_get(p9_g4(r));
        int ne = p9_gs(r, p9_names[0], sizeof(p9_names[0]));
        uint32_t fl = p9_g4(r);
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        if (!fs->writable)
            return -SBG_FS_EROFS;
        if (ne)
            return ne;
        if ((e = p9_join(p9_tmp, f->path, p9_names[0])))
            return e;
        if (!fs->remove)
            return -SBG_FS_EOPNOTSUPP;
        if ((e = p9_stat(fs, p9_tmp, &a)))
            return e;
        if ((fl & P9_AT_REMOVEDIR) && !p9_isdir(&a))
            return -SBG_FS_ENOTDIR;
        if (!(fl & P9_AT_REMOVEDIR) && p9_isdir(&a))
            return -SBG_FS_EISDIR;
        if ((e = fs->remove(fs->ctx, p9_tmp, p9_isdir(&a))) < 0)
            return e;
        return type + 1;
    }
    case P9_TRENAMEAT:
    case P9_TRENAME: {
        struct p9_fid *of = p9_fid_get(p9_g4(r)), *nf;
        int ne = 0, oe = 0;
        if (type == P9_TRENAMEAT) {
            oe = p9_gs(r, p9_names[0], sizeof(p9_names[0]));
            nf = p9_fid_get(p9_g4(r));
        } else {
            nf = p9_fid_get(p9_g4(r));
        }
        ne = p9_gs(r, p9_names[1], sizeof(p9_names[1]));
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!of || !nf)
            return -SBG_FS_EBADF;
        if (!fs->writable)
            return -SBG_FS_EROFS;
        if (oe || ne)
            return oe ? oe : ne;
        if (type == P9_TRENAMEAT) {
            if ((e = p9_join(p9_tmp, of->path, p9_names[0])))
                return e;
        } else {
            if (p9_streq(of->path, "/"))
                return -SBG_FS_EBUSY;
            p9_strcpy(p9_tmp, of->path);
        }
        if ((e = p9_join(p9_tmp2, nf->path, p9_names[1])))
            return e;
        if (!fs->rename)
            return -SBG_FS_EOPNOTSUPP;
        if (p9_streq(p9_tmp, p9_tmp2))
            return type + 1;
        if ((e = fs->rename(fs->ctx, p9_tmp, p9_tmp2)) < 0)
            return e;
        p9_moved(p9_tmp, p9_tmp2);
        return type + 1;
    }
    case P9_TSETATTR: {
        f = p9_fid_get(p9_g4(r));
        uint32_t valid = p9_g4(r);
        p9_g4(r);                       /* mode, uid, gid: one owner, fixed modes (accepted, not kept) */
        p9_g4(r);
        p9_g4(r);
        uint64_t size = p9_g8(r);
        p9_g8(r);                       /* atime */
        p9_g8(r);
        int64_t ms = (int64_t)p9_g8(r);
        uint32_t mns = (uint32_t)p9_g8(r);
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        if (!fs->writable)
            return -SBG_FS_EROFS;
        if (valid & P9_ATTR_SIZE) {
            if ((e = p9_stat(fs, f->path, &a)))
                return e;
            if (p9_isdir(&a))
                return -SBG_FS_EISDIR;
            if (!fs->truncate)
                return -SBG_FS_EOPNOTSUPP;
            if ((e = fs->truncate(fs->ctx, f->path, size)) < 0)
                return e;
        }
        if ((valid & P9_ATTR_MTIME) && (valid & P9_ATTR_MTIME_SET) && fs->set_mtime &&
            (e = fs->set_mtime(fs->ctx, f->path, ms, mns)) < 0)
            return e;
        return type + 1;
    }
    case P9_TFSYNC:
        f = p9_fid_get(p9_g4(r));
        p9_g4(r);
        if (r->bad)
            return -SBG_FS_EPROTO;
        if (!f)
            return -SBG_FS_EBADF;
        if (fs->writable && fs->sync && (e = fs->sync(fs->ctx, f->path)) < 0)
            return e;
        return type + 1;
    case P9_TFLUSH:                     /* answered in order: the flushed request has its answer already */
        p9_g2(r);
        return r->bad ? -SBG_FS_EPROTO : type + 1;
    case P9_TLOCK:                      /* one client: every lock is granted */
        p9_g4(r);
        p9_g1(r);
        return r->bad ? -SBG_FS_EPROTO : (p9_p1(w, 0), type + 1);
    case P9_TGETLOCK: {
        p9_g4(r);
        p9_g1(r);
        uint64_t st = p9_g8(r), ln = p9_g8(r);
        uint32_t pid = p9_g4(r);
        e = p9_gs(r, p9_tmp, sizeof(p9_tmp));
        if (r->bad)
            return -SBG_FS_EPROTO;
        p9_p1(w, 2);                    /* P9_LOCK_TYPE_UNLCK: nobody holds it */
        p9_p8(w, st);
        p9_p8(w, ln);
        p9_p4(w, pid);
        p9_ps(w, e ? "" : p9_tmp);
        return type + 1;
    }
    case P9_TREADLINK:
        return -SBG_FS_EINVAL;          /* no symbolic links */
    case P9_TSYMLINK:
    case P9_TMKNOD:
    case P9_TLINK:
        return fs->writable ? -SBG_FS_EPERM : -SBG_FS_EROFS;
    case P9_TXATTRWALK:
    case P9_TXATTRCREATE:
    case P9_TAUTH:
    default:
        return -SBG_FS_EOPNOTSUPP;
    }
}

unsigned sbg_p9_serve(const struct sbg_fs *fs, uint8_t *msg, unsigned len, unsigned cap, unsigned msize_max)
{
    const volatile uint8_t *m = msg;
    if (len < P9_HDR || len > cap || cap < 64)
        return 0;
    if (msize_max > cap)
        msize_max = cap;
    unsigned n = len < P9_IN_MAX ? len : P9_IN_MAX;
    for (unsigned i = 0; i < n; ++i)
        p9_in[i] = m[i];
    struct p9_rd r = {.p = p9_in, .n = n, .off = 0, .bad = 0};
    if (p9_g4(&r) != len)
        return 0;
    uint8_t type = p9_g1(&r);
    uint16_t tag = p9_g2(&r);
    struct p9_wr w = {.p = msg, .cap = cap, .off = P9_HDR, .over = 0};
    if (type != P9_TVERSION && cap > p9_msize)
        w.cap = p9_msize;               /* never more than the guest agreed to */
    int rt = p9_one(fs, type, &r, &w, msg, len, w.cap, msize_max);
    p9_st.requests++;
    if (rt >= 0 && w.over)
        rt = -SBG_FS_EIO;               /* cannot happen: every answer is bounded by the room */
    if (rt < 0) {
        p9_st.errors++;
        w.off = P9_HDR;
        w.over = 0;
        p9_p4(&w, (uint32_t)-rt);
        rt = P9_RLERROR;
    }
    volatile uint8_t *o = msg;
    o[0] = (uint8_t)w.off;
    o[1] = (uint8_t)(w.off >> 8);
    o[2] = (uint8_t)(w.off >> 16);
    o[3] = (uint8_t)(w.off >> 24);
    o[4] = (uint8_t)rt;
    o[5] = (uint8_t)tag;
    o[6] = (uint8_t)(tag >> 8);
    return w.off;
}

/* ---- the channel (sbshm-p9.h) ---------------------------------------------------------------------------------- */
#define SBG_P9_PER_POLL 8U              /* exchanges per sbg_poll (8 x 128 KiB = 1 MiB) */
#define SBG_P9_POLL_MS  4U              /* no new exchange once this long was spent in one poll */
static uint32_t p9_req_tail, p9_done_head;
static int p9_ring_bad;

static volatile struct sbshm_p9 *p9_ch(void) { return (volatile struct sbshm_p9 *)sbg_shm(SBSHM_P9_OFF); }
const struct sbg_p9_stats *sbg_p9_stats(void) { return &p9_st; }

void sbg_p9_load(void)
{
    p9_req_tail = p9_done_head = 0;
    p9_ring_bad = 0;
    p9_msize = SBSHM_P9_SLOT;
    sbg_p9_reset();
    const struct sbg_fs *fs = sbg_os_()->fs;
    if (!(sbg_services() & SBSHM_SVC_P9) || !fs)
        return;
    volatile struct sbshm_p9 *c = p9_ch();     /* sbg_load zeroed it: indices 0 */
    c->msize = SBSHM_P9_SLOT;
    c->slots = SBSHM_P9_SLOTS;
    c->flags = fs->writable ? 0 : SBSHM_P9_RO;
    const char *nm = fs->name ? fs->name : "";
    unsigned i = 0;
    for (; i < sizeof(c->name) - 1 && nm[i]; ++i)
        c->name[i] = nm[i];
    for (; i < sizeof(c->name); ++i)
        c->name[i] = 0;
    sbshm_dmb();
    c->magic = SBSHM_P9_MAGIC;
}

void sbg_p9_poll(void)
{
    const struct sbg_os *os = sbg_os_();
    const struct sbg_fs *fs = os->fs;
    if (!(sbg_services() & SBSHM_SVC_P9) || !fs || p9_ring_bad)
        return;
    volatile struct sbshm_p9 *c = p9_ch();
    unsigned n = 0;
    uint64_t t0 = os->ms(), t1 = t0;
    for (;;) {
        uint32_t head = c->req_head;
        if (head == p9_req_tail)
            break;
        if (head - p9_req_tail > SBSHM_P9_RING) {      /* not a ring any more: stop serving, say so once */
            p9_ring_bad = 1;
            p9_st.bad++;
            if (os->log)
                os->log("guest: sbp9: the request ring is broken (req_head runs ahead): file service stopped");
            break;
        }
        if (p9_done_head - c->done_tail >= SBSHM_P9_RING)  /* the guest has not reaped: wait for it */
            break;
        if (n >= SBG_P9_PER_POLL || (n && t1 - t0 >= SBG_P9_POLL_MS)) {
            p9_st.capped++;
            break;
        }
        sbshm_dmb();
        const volatile struct sbshm_p9_ent *q = &c->req[p9_req_tail & (SBSHM_P9_RING - 1)];
        struct sbshm_p9_ent e = {q->id, q->slot, q->len, 0};
        unsigned rlen = 0;
        if (e.slot < SBSHM_P9_SLOTS && e.len >= P9_HDR && e.len <= SBSHM_P9_SLOT)
            rlen = sbg_p9_serve(fs, (uint8_t *)sbg_shm(SBSHM_P9_OFF + SBSHM_P9_SLOTS_OFF + e.slot * SBSHM_P9_SLOT),
                                e.len, SBSHM_P9_SLOT, SBSHM_P9_SLOT);
        if (!rlen) {
            p9_st.bad++;
            e.status = -SBG_FS_EIO;
            if (p9_st.bad <= 4 && os->log)
                os->log("guest: sbp9: a malformed request (slot, length or size word): answered EIO");
        }
        volatile struct sbshm_p9_ent *d = &c->done[p9_done_head & (SBSHM_P9_RING - 1)];
        d->id = e.id;
        d->slot = e.slot;
        d->len = rlen;
        d->status = e.status;
        sbshm_dmb();                    /* the R-message and the entry, then the indices */
        c->req_tail = ++p9_req_tail;
        c->done_head = ++p9_done_head;
        ++n;
        t1 = os->ms();
    }
    if (!n)
        return;
    sbg_ring_guest();
    p9_st.doorbells++;
    if (n > p9_st.batch_max)
        p9_st.batch_max = n;
    if (t1 - t0 > p9_st.poll_ms_max)
        p9_st.poll_ms_max = (uint32_t)(t1 - t0);
}
