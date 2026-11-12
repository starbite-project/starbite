/* SPDX-License-Identifier: GPL-2.0-only
 * test-p9: the host test of sbg_p9 (Starbite OS's 9P2000.L server for the guest, sbshm-p9.h). Plain cc on the Mac or
 * Linux, no sbghost: sbg_p9.c is built in with stand-ins for sbghost_int.h, a POSIX struct sbg_fs over a temporary
 * directory, a fake shared region, and a byte-level 9P client that goes through the channel exactly as the guest's
 * sbp9.c does (slot, request entry, req_head; the host's poll; done entry, done_tail). It checks every message the
 * server knows, long names, a 3 MiB read, a directory of 300 entries read in small pieces, the fid limit, errors,
 * the read-only mode, and the channel itself (batch bounds, ring order, a guest that does not reap, malformed
 * entries, a broken ring).
 *   cd starbite/guest/host && cc -O2 -Wall -Wextra -o /tmp/test-p9 test-p9.c && /tmp/test-p9
 * Last line: "test-p9: PASS (N checks)" or "test-p9: FAIL (k of N checks)"; exit status 0 / 1.
 */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "sbg_p9.c"

/* ---- stand-ins for sbghost (sbghost_int.h) ------------------------------------------------------------------- */
static uint8_t *shm;                    /* the channel's 2 MiB (SBSHM_P9_OFF maps to shm[0]) */
static uint32_t services = SBSHM_SVC_P9;
static unsigned doorbells;
static struct sbg_os the_os;
static int logs;
static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void host_log(const char *l)
{
    ++logs;
    printf("  host log: %s\n", l);
}
const struct sbg_os *sbg_os_(void) { return &the_os; }
uintptr_t sbg_shm(uint32_t off) { return (uintptr_t)(shm + (off - SBSHM_P9_OFF)); }
uint32_t sbg_services(void) { return services; }
void sbg_ring_guest(void) { ++doorbells; }

/* ---- a POSIX struct sbg_fs over a directory ---------------------------------------------------------------------- */
static char root[512];
static int lx(int e)                    /* the host's errno -> Linux's */
{
    switch (e) {
    case EPERM: return 1;
    case ENOENT: return 2;
    case EBADF: return 9;
    case EACCES: return 13;
    case EBUSY: return 16;
    case EEXIST: return 17;
    case EXDEV: return 18;
    case ENOTDIR: return 20;
    case EISDIR: return 21;
    case EINVAL: return 22;
    case ENFILE: return 23;
    case EFBIG: return 27;
    case ENOSPC: return 28;
    case EROFS: return 30;
    case ENAMETOOLONG: return 36;
    case ENOTEMPTY: return 39;
    default: return 5;
    }
}
#define NEG (-lx(errno))
static const char *hp(const char *path, char *buf)
{
    snprintf(buf, 4096, "%s%s", root, path);
    return buf;
}
static void fill_attr(const struct stat *s, struct sbg_fs_attr *a)
{
    a->size = (uint64_t)s->st_size;
    a->ino = (uint64_t)s->st_ino;
    a->mode = (S_ISDIR(s->st_mode) ? SBG_FS_S_IFDIR : SBG_FS_S_IFREG) | (s->st_mode & 07777);
    a->nlink = (uint32_t)s->st_nlink;
#ifdef __APPLE__
    a->mtime = s->st_mtimespec.tv_sec;
    a->mtime_ns = (uint32_t)s->st_mtimespec.tv_nsec;
#else
    a->mtime = s->st_mtim.tv_sec;
    a->mtime_ns = (uint32_t)s->st_mtim.tv_nsec;
#endif
}
static int px_stat(void *c, const char *path, struct sbg_fs_attr *a)
{
    (void)c;
    char b[4096];
    struct stat s;
    if (lstat(hp(path, b), &s))
        return NEG;
    fill_attr(&s, a);
    return 0;
}
static int px_list(void *c, const char *dir, uint32_t first, struct sbg_fs_dirent *out, unsigned max)
{
    (void)c;
    char b[4096], f[4096];
    DIR *d = opendir(hp(dir, b));
    if (!d)
        return NEG;
    struct dirent *e;
    uint32_t i = 0;
    unsigned n = 0;
    while (n < max && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (i++ < first)
            continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        struct stat s;
        snprintf(f, sizeof(f), "%s/%s", b, e->d_name);
        if (lstat(f, &s))
            continue;
        fill_attr(&s, &out[n].a);
        ++n;
    }
    closedir(d);
    return (int)n;
}
static int px_read(void *c, const char *path, uint64_t off, void *dst, uint32_t len)
{
    (void)c;
    char b[4096];
    int fd = open(hp(path, b), O_RDONLY);
    if (fd < 0)
        return NEG;
    ssize_t n = pread(fd, dst, len, (off_t)off);
    int e = errno;
    close(fd);
    return n < 0 ? -lx(e) : (int)n;
}
static int px_write(void *c, const char *path, uint64_t off, const void *src, uint32_t len)
{
    (void)c;
    char b[4096];
    int fd = open(hp(path, b), O_WRONLY);
    if (fd < 0)
        return NEG;
    ssize_t n = pwrite(fd, src, len, (off_t)off);
    int e = errno;
    close(fd);
    return n < 0 ? -lx(e) : (int)n;
}
static int px_create(void *c, const char *path, uint32_t mode)
{
    (void)c;
    char b[4096];
    int fd = open(hp(path, b), O_WRONLY | O_CREAT | O_EXCL, mode ? mode : 0644);
    if (fd < 0)
        return NEG;
    close(fd);
    return 0;
}
static int px_mkdir(void *c, const char *path, uint32_t mode)
{
    (void)c;
    char b[4096];
    return mkdir(hp(path, b), mode ? mode : 0755) ? NEG : 0;
}
static int px_remove(void *c, const char *path, int dir)
{
    (void)c;
    char b[4096];
    return (dir ? rmdir(hp(path, b)) : unlink(hp(path, b))) ? NEG : 0;
}
static int px_rename(void *c, const char *from, const char *to)
{
    (void)c;
    char b[4096], d[4096];
    return rename(hp(from, b), hp(to, d)) ? NEG : 0;
}
static int px_truncate(void *c, const char *path, uint64_t size)
{
    (void)c;
    char b[4096];
    return truncate(hp(path, b), (off_t)size) ? NEG : 0;
}
static int px_set_mtime(void *c, const char *path, int64_t s, uint32_t ns)
{
    (void)c;
    char b[4096];
    struct timeval tv[2] = {{(time_t)s, (int)(ns / 1000)}, {(time_t)s, (int)(ns / 1000)}};
    return utimes(hp(path, b), tv) ? NEG : 0;
}
static int px_sync(void *c, const char *path)
{
    (void)c;
    char b[4096];
    int fd = open(hp(path, b), O_RDONLY);
    if (fd < 0)
        return NEG;
    int r = fsync(fd);
    close(fd);
    return r ? -5 : 0;
}
static int px_statfs(void *c, struct sbg_fs_statfs *s)
{
    (void)c;
    struct statvfs v;
    if (statvfs(root, &v))
        return NEG;
    s->bsize = (uint32_t)v.f_frsize;
    s->blocks = v.f_blocks;
    s->bfree = v.f_bavail;
    s->files = v.f_files;
    s->ffree = v.f_ffree;
    return 0;
}
static struct sbg_fs pfs = {
    .ctx = 0, .writable = 1, .uid = 1000, .gid = 1000, .name = "test-p9 POSIX",
    .stat = px_stat, .list = px_list, .read = px_read, .statfs = px_statfs,
    .write = px_write, .create = px_create, .mkdir = px_mkdir, .remove = px_remove, .rename = px_rename,
    .truncate = px_truncate, .set_mtime = px_set_mtime, .sync = px_sync,
};

/* ---- checks ----------------------------------------------------------------------------------------------- */
static unsigned checks, failed;
static void ck(int ok, const char *fmt, ...)
{
    ++checks;
    if (ok)
        return;
    ++failed;
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ---- the guest's side of the channel: T-message builder, the ring, R-message reader --------------------------- */
static uint8_t tb[SBSHM_P9_SLOT], rb[SBSHM_P9_SLOT];
static unsigned tn, rn, ro;
static uint16_t tag_next = 1;
static uint32_t g_req_head, g_done_tail, g_gen;
static unsigned g_slot;

static volatile struct sbshm_p9 *ch(void) { return (volatile struct sbshm_p9 *)shm; }
static uint8_t *slot_buf(unsigned s) { return shm + SBSHM_P9_SLOTS_OFF + s * SBSHM_P9_SLOT; }

static void t_begin(uint8_t type)
{
    tn = 7;
    tb[4] = type;
    uint16_t tag = type == P9_TVERSION ? 0xffff : tag_next++;
    tb[5] = (uint8_t)tag;
    tb[6] = (uint8_t)(tag >> 8);
}
static void t1(uint32_t v) { tb[tn++] = (uint8_t)v; }
static void t2(uint32_t v) { t1(v); t1(v >> 8); }
static void t4(uint32_t v) { t2(v); t2(v >> 16); }
static void t8(uint64_t v) { t4((uint32_t)v); t4((uint32_t)(v >> 32)); }
static void tsn(const char *s, unsigned n)
{
    t2(n);
    memcpy(tb + tn, s, n);
    tn += n;
}
static void ts(const char *s) { tsn(s, (unsigned)strlen(s)); }
static unsigned t_end(void)
{
    tb[0] = (uint8_t)tn;
    tb[1] = (uint8_t)(tn >> 8);
    tb[2] = (uint8_t)(tn >> 16);
    tb[3] = (uint8_t)(tn >> 24);
    return tn;
}

/* put the T-message into slot s and queue it (no poll) */
static uint32_t submit(unsigned s, unsigned len)
{
    memcpy(slot_buf(s), tb, len < SBSHM_P9_SLOT ? len : SBSHM_P9_SLOT);    /* a bad length: the entry lies, not the copy */
    uint32_t id = s | (++g_gen << 8);
    volatile struct sbshm_p9_ent *e = &ch()->req[g_req_head & (SBSHM_P9_RING - 1)];
    e->id = id;
    e->slot = s;
    e->len = len;
    e->status = 0;
    ch()->req_head = ++g_req_head;
    return id;
}
/* reap one completion: 1 and its entry, or 0 when there is none */
static int reap(struct sbshm_p9_ent *out)
{
    if (ch()->done_head == g_done_tail)
        return 0;
    volatile struct sbshm_p9_ent *e = &ch()->done[g_done_tail & (SBSHM_P9_RING - 1)];
    out->id = e->id;
    out->slot = e->slot;
    out->len = e->len;
    out->status = e->status;
    ch()->done_tail = ++g_done_tail;
    return 1;
}
static uint8_t r1(void) { return rb[ro++]; }
static uint16_t r2(void) { uint16_t v = (uint16_t)(rb[ro] | rb[ro + 1] << 8); ro += 2; return v; }
static uint32_t r4(void) { uint32_t v = (uint32_t)r2(); return v | (uint32_t)r2() << 16; }
static uint64_t r8(void) { uint64_t v = r4(); return v | (uint64_t)r4() << 32; }
static unsigned rs(char *d, unsigned cap)
{
    unsigned n = r2();
    unsigned k = n < cap - 1 ? n : cap - 1;
    memcpy(d, rb + ro, k);
    d[k] = 0;
    ro += n;
    return n;
}

/* one exchange through the channel: the R-message's type (rb / rn), ro at its body; 0 = the channel failed */
static uint8_t rpc(void)
{
    unsigned len = t_end(), s = g_slot++ % SBSHM_P9_SLOTS;
    uint32_t id = submit(s, len);
    sbg_p9_poll();
    struct sbshm_p9_ent e;
    if (!reap(&e)) {
        ck(0, "no completion for request type %u", tb[4]);
        return 0;
    }
    ck(e.id == id && e.slot == s, "completion id %#x slot %u, want %#x %u", e.id, e.slot, id, s);
    if (e.status || !e.len) {
        ck(0, "the channel failed type %u: status %d len %u", tb[4], e.status, e.len);
        return 0;
    }
    memcpy(rb, slot_buf(s), e.len);
    rn = e.len;
    ro = 7;
    uint32_t sz = (uint32_t)rb[0] | (uint32_t)rb[1] << 8 | (uint32_t)rb[2] << 16 | (uint32_t)rb[3] << 24;
    ck(sz == rn, "R size word %u, entry len %u", sz, rn);
    ck(rb[5] == tb[5] && rb[6] == tb[6], "R tag differs");
    ck(rb[4] == P9_RLERROR || rb[4] == tb[4] + 1, "R type %u for T %u", rb[4], tb[4]);
    return rb[4];
}
static uint32_t rerr(void) { return rb[4] == P9_RLERROR ? (ro = 7, r4()) : 0; }
static int expect_err(uint32_t e, const char *what)
{
    uint8_t t = rpc();
    uint32_t got = t == P9_RLERROR ? (ro = 7, r4()) : 0;
    ck(t == P9_RLERROR && got == e, "%s: want Rlerror %u, got type %u ecode %u", what, e, t, got);
    return t == P9_RLERROR && got == e;
}
static int expect_ok(const char *what)
{
    uint8_t t = rpc();
    ck(t == tb[4] + 1, "%s: want R%u, got type %u (ecode %u)", what, tb[4] + 1, t, t == P9_RLERROR ? rerr() : 0);
    ro = 7;
    return t == tb[4] + 1;
}

/* ---- 9P calls ------------------------------------------------------------------------------------------------ */
struct qid {
    uint8_t type;
    uint32_t ver;
    uint64_t path;
};
static struct qid rqid(void)
{
    struct qid q;
    q.type = r1();
    q.ver = r4();
    q.path = r8();
    return q;
}
static uint32_t version(uint32_t msize, const char *v, char *got)
{
    t_begin(P9_TVERSION);
    t4(msize);
    ts(v);
    if (rpc() != P9_TVERSION + 1)
        return 0;
    uint32_t m = r4();
    rs(got, 32);
    return m;
}
static int attach(uint32_t fid)
{
    t_begin(P9_TATTACH);
    t4(fid);
    t4(0xffffffffU);
    ts("user");
    ts("");
    t4(1000);
    return expect_ok("attach");
}
/* walk fid -> newfid through a '/'-separated relative path ("" = clone): the number of qids, -ecode on Rlerror */
static int walk(uint32_t fid, uint32_t newfid, const char *rel, struct qid *last)
{
    t_begin(P9_TWALK);
    t4(fid);
    t4(newfid);
    unsigned at = tn, n = 0;
    t2(0);
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", rel);
    for (char *p = buf, *q; *p; p = q) {
        q = strchr(p, '/');
        if (q)
            *q++ = 0;
        else
            q = p + strlen(p);
        ts(p);
        ++n;
    }
    tb[at] = (uint8_t)n;
    tb[at + 1] = (uint8_t)(n >> 8);
    uint8_t t = rpc();
    if (t == P9_RLERROR)
        return -(int)rerr();
    unsigned nq = r2();
    for (unsigned i = 0; i < nq; ++i) {
        struct qid q = rqid();
        if (last)
            *last = q;
    }
    return (int)nq;
}
static int clunk(uint32_t fid)
{
    t_begin(P9_TCLUNK);
    t4(fid);
    uint8_t t = rpc();
    return t == P9_TCLUNK + 1 ? 0 : -(int)rerr();
}
static int lopen(uint32_t fid, uint32_t flags)
{
    t_begin(P9_TLOPEN);
    t4(fid);
    t4(flags);
    uint8_t t = rpc();
    return t == P9_TLOPEN + 1 ? 0 : -(int)rerr();
}
struct attr {
    uint64_t valid;
    struct qid q;
    uint32_t mode, uid, gid;
    uint64_t nlink, size, mtime;
};
static int getattr(uint32_t fid, struct attr *a)
{
    t_begin(P9_TGETATTR);
    t4(fid);
    t8(0x3fff);
    if (rpc() != P9_TGETATTR + 1)
        return -(int)rerr();
    a->valid = r8();
    a->q = rqid();
    a->mode = r4();
    a->uid = r4();
    a->gid = r4();
    a->nlink = r8();
    r8();
    a->size = r8();
    r8();
    r8();
    r8();
    r8();
    a->mtime = r8();
    return 0;
}
/* Tread: bytes read into dst (or -ecode) */
static int readat(uint32_t fid, uint64_t off, uint32_t count, uint8_t *dst)
{
    t_begin(P9_TREAD);
    t4(fid);
    t8(off);
    t4(count);
    if (rpc() != P9_TREAD + 1)
        return -(int)rerr();
    uint32_t n = r4();
    ck(n <= count && 11 + n == rn, "Rread count %u for %u, message %u bytes", n, count, rn);
    if (dst)
        memcpy(dst, rb + 11, n);
    return (int)n;
}
static int writeat(uint32_t fid, uint64_t off, const void *src, uint32_t count)
{
    t_begin(P9_TWRITE);
    t4(fid);
    t8(off);
    t4(count);
    memcpy(tb + tn, src, count);
    tn += count;
    if (rpc() != P9_TWRITE + 1)
        return -(int)rerr();
    return (int)r4();
}
static int lcreate(uint32_t dfid, const char *name, uint32_t flags, uint32_t mode)
{
    t_begin(P9_TLCREATE);
    t4(dfid);
    ts(name);
    t4(flags);
    t4(mode);
    t4(1000);
    uint8_t t = rpc();
    return t == P9_TLCREATE + 1 ? 0 : -(int)rerr();
}
static int mkdir9(uint32_t dfid, const char *name)
{
    t_begin(P9_TMKDIR);
    t4(dfid);
    ts(name);
    t4(0755);
    t4(1000);
    uint8_t t = rpc();
    return t == P9_TMKDIR + 1 ? 0 : -(int)rerr();
}
static int unlinkat9(uint32_t dfid, const char *name, uint32_t flags)
{
    t_begin(P9_TUNLINKAT);
    t4(dfid);
    ts(name);
    t4(flags);
    uint8_t t = rpc();
    return t == P9_TUNLINKAT + 1 ? 0 : -(int)rerr();
}
static int renameat9(uint32_t ofid, const char *on, uint32_t nfid, const char *nn)
{
    t_begin(P9_TRENAMEAT);
    t4(ofid);
    ts(on);
    t4(nfid);
    ts(nn);
    uint8_t t = rpc();
    return t == P9_TRENAMEAT + 1 ? 0 : -(int)rerr();
}
static int setattr9(uint32_t fid, uint32_t valid, uint64_t size, uint64_t mtime)
{
    t_begin(P9_TSETATTR);
    t4(fid);
    t4(valid);
    t4(0644);
    t4(0);
    t4(0);
    t8(size);
    t8(0);
    t8(0);
    t8(mtime);
    t8(0);
    uint8_t t = rpc();
    return t == P9_TSETATTR + 1 ? 0 : -(int)rerr();
}
/* the whole directory through Treaddir with `count` bytes per call: names into a list, the number of entries */
static int readdir_all(uint32_t fid, uint32_t count, char names[][SBG_FS_NAME_MAX + 1], unsigned max, unsigned *calls)
{
    uint64_t off = 0;
    unsigned n = 0;
    *calls = 0;
    for (;;) {
        t_begin(P9_TREADDIR);
        t4(fid);
        t8(off);
        t4(count);
        ++*calls;
        if (rpc() != P9_TREADDIR + 1)
            return -(int)rerr();
        uint32_t c = r4();
        ck(c <= count && 11 + c == rn, "Rreaddir %u bytes for count %u", c, count);
        if (!c)
            return (int)n;
        unsigned end = ro + c;
        while (ro < end) {
            rqid();
            uint64_t o = r8();
            r1();
            char nm[SBG_FS_NAME_MAX + 2];
            rs(nm, sizeof(nm));
            ck(o > off, "readdir offset %llu not after %llu", (unsigned long long)o, (unsigned long long)off);
            off = o;
            if (n < max)
                snprintf(names[n], SBG_FS_NAME_MAX + 1, "%s", nm);
            ++n;
        }
        if (*calls > 100000)
            return -1;
    }
}

/* ---- files in the temp dir ------------------------------------------------------------------------------------- */
static void put_file(const char *rel, const void *data, size_t n)
{
    char b[4096];
    snprintf(b, sizeof(b), "%s/%s", root, rel);
    FILE *f = fopen(b, "wb");
    if (!f || fwrite(data, 1, n, f) != n) {
        printf("test-p9: cannot write %s\n", b);
        exit(2);
    }
    fclose(f);
}
static void mkd(const char *rel)
{
    char b[4096];
    snprintf(b, sizeof(b), "%s/%s", root, rel);
    mkdir(b, 0755);
}
static int host_has(const char *rel)
{
    char b[4096];
    struct stat s;
    snprintf(b, sizeof(b), "%s/%s", root, rel);
    return !lstat(b, &s);
}
static void rm_rf(const char *p)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", p);
    if (system(cmd)) {
    }
}
static uint32_t rnd = 20261112;
static uint32_t xr(void)
{
    rnd ^= rnd << 13;
    rnd ^= rnd >> 17;
    rnd ^= rnd << 5;
    return rnd;
}

static char dnames[400][SBG_FS_NAME_MAX + 1];
static uint8_t big[3 << 20], got[3 << 20];

int main(void)
{
    snprintf(root, sizeof(root), "%s/test-p9.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    chmod(root, 0755);                  /* mkdtemp makes it 0700 */
    shm = calloc(1, SBSHM_P9_SIZE);
    the_os.ms = now_ms;
    the_os.log = host_log;
    the_os.fs = &pfs;
    sbg_p9_load();
    ck(ch()->magic == SBSHM_P9_MAGIC && ch()->msize == SBSHM_P9_SLOT && ch()->slots == SBSHM_P9_SLOTS &&
           ch()->flags == 0 && !strcmp((const char *)ch()->name, "test-p9 POSIX"),
       "the channel's config words");
    ck(sizeof(struct sbshm_p9) <= SBSHM_P9_SLOTS_OFF &&
           SBSHM_P9_SLOTS_OFF + SBSHM_P9_SLOTS * SBSHM_P9_SLOT <= SBSHM_P9_SIZE && SBSHM_P9_RING >= SBSHM_P9_SLOTS,
       "the layout fits 2 MiB");

    /* the tree */
    mkd("docs");
    mkd("docs/sub");
    mkd("many");
    put_file("hello.txt", "hello, starbite\n", 16);
    put_file("docs/sub/deep.txt", "deep", 4);
    put_file("empty", "", 0);
    for (size_t i = 0; i < sizeof(big); ++i)
        big[i] = (uint8_t)xr();
    put_file("big.bin", big, sizeof(big));
    char longname[SBG_FS_NAME_MAX + 2];
    memset(longname, 'L', SBG_FS_NAME_MAX);
    longname[SBG_FS_NAME_MAX] = 0;
    for (int i = 0; i < 300; ++i) {
        char nm[300];
        if (i % 50 == 7)
            snprintf(nm, sizeof(nm), "many/%.*s%03d", (int)SBG_FS_NAME_MAX - 3, longname, i);
        else
            snprintf(nm, sizeof(nm), "many/file-%03d", i);
        put_file(nm, nm, strlen(nm));
    }
    /* three nested 255-byte names: a 770-byte path (below SBG_P9_PATH) */
    char deep[1024];
    snprintf(deep, sizeof(deep), "%s", longname);
    mkd(deep);
    snprintf(deep, sizeof(deep), "%s/%s", longname, longname);
    mkd(deep);
    snprintf(deep, sizeof(deep), "%s/%s/%s", longname, longname, longname);
    put_file(deep, "bottom", 6);

    /* ---- version, attach ---- */
    char v[40];
    uint32_t ms = version(1 << 20, "9P2000.L", v);
    ck(ms == SBSHM_P9_SLOT && !strcmp(v, "9P2000.L"), "Tversion 1 MiB: msize %u %s", ms, v);
    ms = version(8192, "9P2000.u", v);
    ck(ms == 8192 && !strcmp(v, "unknown"), "Tversion 9P2000.u: %u %s", ms, v);
    t_begin(P9_TVERSION);
    t4(1024);
    ts("9P2000.L");
    expect_err(SBG_FS_EINVAL, "Tversion msize 1024");
    ms = version(SBSHM_P9_SLOT, "9P2000.L", v);
    ck(ms == SBSHM_P9_SLOT, "Tversion 128 KiB");
    ck(attach(0), "Tattach");
    t_begin(P9_TATTACH);
    t4(0);
    t4(0xffffffffU);
    ts("user");
    ts("");
    t4(1000);
    expect_err(SBG_FS_EEXIST, "Tattach on a fid in use");
    struct attr a;
    ck(!getattr(0, &a) && a.q.type == P9_QTDIR && (a.mode & 0170000) == SBG_FS_S_IFDIR && a.uid == 1000 &&
           a.gid == 1000 && a.valid == P9_GETATTR_BASIC,
       "Tgetattr of the root: type %u mode %o uid %u", a.q.type, a.mode, a.uid);

    /* ---- walk ---- */
    struct qid q;
    ck(walk(0, 1, "hello.txt", &q) == 1 && q.type == 0, "walk to hello.txt");
    ck(!getattr(1, &a) && a.size == 16 && (a.mode & 0777) == 0644 && a.nlink == 1, "getattr hello.txt: size %llu mode %o",
       (unsigned long long)a.size, a.mode);
    ck(walk(0, 2, "docs/sub/deep.txt", &q) == 3 && q.type == 0, "walk three names");
    ck(walk(0, 3, "docs/nothere/deep.txt", 0) == 1, "walk with a missing middle: one qid");
    ck(getattr(3, &a) == -SBG_FS_EBADF, "the newfid of a partial walk is not created");
    ck(walk(0, 3, "nothere", 0) == -SBG_FS_ENOENT, "walk to a missing name: ENOENT");
    ck(walk(0, 3, "hello.txt/x", 0) == 1, "walk through a file stops at it");
    ck(walk(0, 3, "docs/sub/../sub/./deep.txt", &q) == 6, "walk with .. and .");
    ck(!getattr(3, &a) && a.size == 4, "it is deep.txt");
    clunk(3);
    ck(walk(0, 3, "..", &q) == 1 && q.type == P9_QTDIR, "walk .. at the root stays at the root");
    clunk(3);
    ck(walk(1, 3, "", 0) == 0 && !getattr(3, &a) && a.size == 16, "clone walk (no names)");
    ck(walk(0, 3, "hello.txt", 0) == -SBG_FS_EEXIST, "walk to a newfid in use: EEXIST");
    ck(walk(9, 4, "hello.txt", 0) == -SBG_FS_EBADF, "walk from an unknown fid: EBADF");
    ck(walk(0, 4, "a/b", 0) == -SBG_FS_ENOENT, "walk ENOENT");
    char toolong[SBG_FS_NAME_MAX + 2];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = 0;
    ck(walk(0, 4, toolong, 0) == -SBG_FS_ENAMETOOLONG, "walk to a 256-byte name: ENAMETOOLONG");
    snprintf(deep, sizeof(deep), "%s/%s/%s", longname, longname, longname);
    ck(walk(0, 4, deep, &q) == 3, "walk three 255-byte names");
    uint8_t small[64];
    ck(!lopen(4, 0) && readat(4, 0, 64, small) == 6 && !memcmp(small, "bottom", 6), "read at the bottom of a 770-byte path");
    clunk(4);
    clunk(3);
    {   /* 17 names: more than 9P allows */
        t_begin(P9_TWALK);
        t4(0);
        t4(5);
        t2(17);
        for (int i = 0; i < 17; ++i)
            ts("docs");
        expect_err(SBG_FS_EINVAL, "walk of 17 names");
    }

    /* ---- open, read ---- */
    ck(readat(1, 0, 10, small) == -SBG_FS_EBADF, "read on a fid that is not open: EBADF");
    ck(!lopen(1, 0), "lopen hello.txt read-only");
    ro = 7;
    rqid();
    ck(r4() == ((ms - 24) & ~4095U), "Rlopen's iounit is msize - 24 rounded down to 4 KiB");
    ck(readat(1, 0, 64, small) == 16 && !memcmp(small, "hello, starbite\n", 16), "read hello.txt");
    ck(readat(1, 7, 4, small) == 4 && !memcmp(small, "star", 4), "read at an offset");
    ck(readat(1, 16, 10, small) == 0 && readat(1, 1000, 10, small) == 0, "read at and past the end: 0");
    ck(writeat(1, 0, "x", 1) == -SBG_FS_EBADF, "write on a read-only open: EBADF");
    ck(walk(0, 5, "docs", 0) == 1 && lopen(5, 2) == -SBG_FS_EISDIR, "open a directory for writing: EISDIR");
    ck(!lopen(5, 0) && readat(5, 0, 10, small) < 0, "read of a directory fails");
    clunk(5);
    ck(walk(0, 5, "empty", 0) == 1 && !lopen(5, 0) && readat(5, 0, 100, small) == 0, "read an empty file");
    clunk(5);
    {   /* the 3 MiB file in msize pieces */
        ck(walk(0, 6, "big.bin", 0) == 1 && !lopen(6, 0), "open big.bin");
        uint64_t t0 = now_ms(), off = 0;
        unsigned reqs = 0;
        for (;;) {
            int n = readat(6, off, ms - 24, got + off);
            ++reqs;
            if (n <= 0)
                break;
            off += (unsigned)n;
            if (off > sizeof(got))
                break;
        }
        uint64_t t1 = now_ms();
        ck(off == sizeof(big) && !memcmp(big, got, sizeof(big)), "3 MiB read matches (%llu bytes)", (unsigned long long)off);
        printf("test-p9: 3 MiB read in %u requests of %u bytes, %llu ms through the channel\n", reqs, ms - 24,
               (unsigned long long)(t1 - t0));
        ck(readat(6, 0, 1 << 20, 0) == (int)(ms - 11), "a read bigger than msize is cut to msize - 11");
        clunk(6);
    }

    /* ---- readdir ---- */
    ck(walk(0, 7, "many", 0) == 1 && !lopen(7, 0), "open many/");
    unsigned calls;
    int n = readdir_all(7, 512, dnames, 400, &calls);
    int dot = 0, dotdot = 0, files = 0, longs = 0, dup = 0;
    for (int i = 0; i < n && i < 400; ++i) {
        dot += !strcmp(dnames[i], ".");
        dotdot += !strcmp(dnames[i], "..");
        files += !strncmp(dnames[i], "file-", 5);
        longs += strlen(dnames[i]) == SBG_FS_NAME_MAX;
        for (int j = 0; j < i; ++j)
            dup += !strcmp(dnames[i], dnames[j]);
    }
    ck(n == 302 && dot == 1 && dotdot == 1 && files == 294 && longs == 6 && !dup,
       "readdir of 300 entries in 512-byte pieces: %d entries (%d long, %d dup) in %u calls", n, longs, dup, calls);
    printf("test-p9: readdir of 300 entries (6 with 255-byte names) took %u Treaddir calls of 512 bytes\n", calls);
    n = readdir_all(7, 128 * 1024, dnames, 400, &calls);
    ck(n == 302 && calls == 3, "readdir with a big count (at most 256 entries a reply): %d entries in %u calls", n, calls);
    {   /* a count too small for the next entry: EINVAL, not an empty (end) answer */
        t_begin(P9_TREADDIR);
        t4(7);
        t8(2);
        t4(20);
        expect_err(SBG_FS_EINVAL, "readdir with a count no entry fits");
    }
    clunk(7);
    ck(walk(0, 7, "docs", 0) == 1, "walk docs");
    n = readdir_all(7, 4096, dnames, 400, &calls);
    ck(n == 3 && !strcmp(dnames[2], "sub"), "readdir docs/ (not opened): %d", n);
    clunk(7);
    ck(walk(0, 7, "hello.txt", 0) == 1, "walk hello.txt");
    t_begin(P9_TREADDIR);
    t4(7);
    t8(0);
    t4(4096);
    expect_err(SBG_FS_ENOTDIR, "readdir of a file");
    clunk(7);

    /* ---- create, write, read back, setattr, fsync ---- */
    ck(walk(0, 10, "docs", 0) == 1, "walk docs for create");
    ck(!lcreate(10, "new.txt", 0102 /* O_RDWR | O_CREAT */, 0640), "lcreate docs/new.txt");
    ck(host_has("docs/new.txt"), "the file is there on the host");
    ck(writeat(10, 0, "0123456789", 10) == 10 && writeat(10, 5, "abc", 3) == 3, "write");
    ck(readat(10, 0, 64, small) == 10 && !memcmp(small, "01234abc89", 10), "read back what was written");
    ck(!getattr(10, &a) && a.size == 10 && (a.mode & 0777) == 0640, "getattr after write: size %llu mode %o",
       (unsigned long long)a.size, a.mode);
    t_begin(P9_TFSYNC);
    t4(10);
    t4(0);
    expect_ok("fsync");
    ck(!setattr9(10, P9_ATTR_SIZE, 4, 0) && !getattr(10, &a) && a.size == 4, "setattr size 4 (truncate)");
    ck(!setattr9(10, P9_ATTR_MTIME | P9_ATTR_MTIME_SET, 0, 1700000000) && !getattr(10, &a) && a.mtime == 1700000000,
       "setattr mtime");
    ck(!setattr9(10, 1 | 2 | 4, 0, 0), "setattr mode / uid / gid: accepted");
    {   /* a 128 KiB - 24 write in one message, then read in pieces */
        static uint8_t wbuf[SBSHM_P9_SLOT];
        for (unsigned i = 0; i < sizeof(wbuf); ++i)
            wbuf[i] = (uint8_t)(i * 7 + 3);
        ck(writeat(10, 0, wbuf, ms - 24) == (int)(ms - 24), "a full-msize write");
        ck(readat(10, 1000, 4096, got) == 4096 && !memcmp(got, wbuf + 1000, 4096), "read back from inside it");
    }
    clunk(10);
    ck(walk(0, 10, "docs", 0) == 1, "walk docs");
    ck(lcreate(10, "new.txt", 0302 /* O_EXCL */, 0644) == -SBG_FS_EEXIST, "lcreate O_EXCL of an existing file: EEXIST");
    ck(lcreate(10, "sub", 0102, 0644) == -SBG_FS_EISDIR, "lcreate over a directory: EISDIR");
    ck(lcreate(10, "a/b", 0102, 0644) == -SBG_FS_EINVAL, "lcreate a name with '/': EINVAL");
    ck(lcreate(10, "..", 0102, 0644) == -SBG_FS_EINVAL, "lcreate '..': EINVAL");
    ck(lcreate(10, toolong, 0102, 0644) == -SBG_FS_ENAMETOOLONG, "lcreate a 256-byte name: ENAMETOOLONG");
    ck(!lcreate(10, "new.txt", 01102 /* O_TRUNC */, 0644) && !getattr(10, &a) && a.size == 0,
       "lcreate O_TRUNC of an existing file empties it");
    clunk(10);
    ck(walk(0, 11, "docs/new.txt", 0) == 2 && !lopen(11, 02001 /* O_WRONLY | O_APPEND */), "open O_APPEND");
    ck(writeat(11, 0, "ab", 2) == 2 && writeat(11, 0, "cd", 2) == 2, "two appends");
    clunk(11);
    ck(walk(0, 11, "docs/new.txt", 0) == 2 && !lopen(11, 0) && readat(11, 0, 10, small) == 4 &&
           !memcmp(small, "abcd", 4), "appends went to the end");
    clunk(11);
    ck(walk(0, 11, "docs/new.txt", 0) == 2 && !lopen(11, 01001 /* O_WRONLY | O_TRUNC */) && !getattr(11, &a) &&
           a.size == 0, "lopen O_TRUNC empties it");
    clunk(11);

    /* ---- mkdir, rename (fids follow), unlinkat, remove ---- */
    ck(!mkdir9(0, "made"), "mkdir made");
    ck(mkdir9(0, "made") == -SBG_FS_EEXIST, "mkdir again: EEXIST");
    ck(walk(0, 12, "made", 0) == 1 && !lcreate(12, "inner.txt", 0102, 0644) && writeat(12, 0, "inner", 5) == 5,
       "a file inside made/");
    clunk(12);
    ck(walk(0, 13, "made/inner.txt", 0) == 2 && !lopen(13, 0), "an open fid on made/inner.txt");
    ck(walk(0, 14, "made", 0) == 1, "a fid on made/");
    ck(!renameat9(0, "made", 0, "moved"), "renameat made -> moved");
    ck(host_has("moved/inner.txt") && !host_has("made"), "renamed on the host");
    ck(readat(13, 0, 10, small) == 5 && !memcmp(small, "inner", 5), "the open fid still reads after the rename");
    ck(!getattr(14, &a) && (a.mode & 0170000) == SBG_FS_S_IFDIR, "the directory fid follows the rename");
    ck(!renameat9(14, "inner.txt", 0, "top.txt") && host_has("top.txt"), "renameat moved/inner.txt -> top.txt");
    ck(readat(13, 0, 10, small) == 5, "the open fid followed again");
    {   /* Trename (the old style) */
        t_begin(P9_TRENAME);
        t4(13);
        t4(14);
        ts("back.txt");
        expect_ok("Trename top.txt -> moved/back.txt");
        ck(host_has("moved/back.txt") && readat(13, 0, 10, small) == 5, "Trename moved the file and the fid");
    }
    ck(renameat9(0, "nothere", 0, "x") == -SBG_FS_ENOENT, "rename of a missing file: ENOENT");
    ck(unlinkat9(0, "moved", 0x200) == -SBG_FS_ENOTEMPTY, "rmdir of a full directory: ENOTEMPTY");
    ck(unlinkat9(0, "moved", 0) == -SBG_FS_EISDIR, "unlink of a directory: EISDIR");
    ck(unlinkat9(14, "back.txt", 0x200) == -SBG_FS_ENOTDIR, "rmdir of a file: ENOTDIR");
    ck(!unlinkat9(14, "back.txt", 0) && !host_has("moved/back.txt"), "unlinkat a file");
    clunk(13);
    ck(!unlinkat9(0, "moved", 0x200) && !host_has("moved"), "unlinkat an empty directory");
    clunk(14);
    ck(walk(0, 15, "docs/new.txt", 0) == 2, "walk docs/new.txt");
    t_begin(P9_TREMOVE);
    t4(15);
    expect_ok("Tremove");
    ck(!host_has("docs/new.txt") && getattr(15, &a) == -SBG_FS_EBADF, "Tremove removed it and clunked the fid");
    ck(walk(0, 15, "", 0) == 0, "clone the root");
    t_begin(P9_TREMOVE);
    t4(15);
    expect_err(SBG_FS_EBUSY, "Tremove of the root: EBUSY");
    ck(getattr(15, &a) == -SBG_FS_EBADF, "the fid is clunked anyway");

    /* ---- statfs, lock, flush, the rest ---- */
    t_begin(P9_TSTATFS);
    t4(0);
    if (expect_ok("Tstatfs")) {
        uint32_t ty = r4(), bs = r4();
        uint64_t blocks = r8(), bfree = r8();
        ck(ty == 0x01021997U && bs > 0 && blocks > 0 && bfree <= blocks, "statfs fields");
    }
    t_begin(P9_TLOCK);
    t4(1);
    t1(1);
    t4(0);
    t8(0);
    t8(0);
    t4(42);
    ts("client");
    ck(expect_ok("Tlock") && r1() == 0, "Tlock granted");
    t_begin(P9_TGETLOCK);
    t4(1);
    t1(1);
    t8(5);
    t8(10);
    t4(42);
    ts("client");
    if (expect_ok("Tgetlock")) {
        uint8_t ty = r1();
        uint64_t st = r8(), ln = r8();
        uint32_t pid = r4();
        char cl[16];
        rs(cl, sizeof(cl));
        ck(ty == 2 && st == 5 && ln == 10 && pid == 42 && !strcmp(cl, "client"), "Tgetlock: unlocked, echoed");
    }
    t_begin(P9_TFLUSH);
    t2(3);
    expect_ok("Tflush");
    t_begin(P9_TXATTRWALK);
    t4(1);
    t4(20);
    ts("user.x");
    expect_err(SBG_FS_EOPNOTSUPP, "Txattrwalk");
    t_begin(P9_TREADLINK);
    t4(1);
    expect_err(SBG_FS_EINVAL, "Treadlink");
    t_begin(P9_TSYMLINK);
    t4(0);
    ts("l");
    ts("hello.txt");
    t4(0);
    expect_err(SBG_FS_EPERM, "Tsymlink");
    t_begin(124);                       /* Tstat (9P2000) */
    t4(0);
    expect_err(SBG_FS_EOPNOTSUPP, "a 9P2000 Tstat");
    t_begin(P9_TGETATTR);               /* cut short */
    t2(0);
    expect_err(SBG_FS_EPROTO, "a truncated Tgetattr");
    t_begin(P9_TWRITE);                 /* the count says more than the message carries */
    t4(1);
    t8(0);
    t4(100);
    t4(0);
    expect_err(SBG_FS_EPROTO, "a Twrite whose count runs past the message");
    ck(clunk(77) == -SBG_FS_EBADF, "clunk an unknown fid: EBADF");

    /* ---- many fids ---- */
    unsigned inuse = p9_nfids, made = 0;
    int last = 0;
    for (unsigned i = 0; i < SBG_P9_FIDS + 4; ++i) {
        last = walk(0, 1000 + i, "docs", 0);
        if (last != 1)
            break;
        ++made;
    }
    ck(made == SBG_P9_FIDS - inuse && last == -SBG_FS_ENFILE, "fids: %u more after %u in use, then %d (ENFILE)", made,
       inuse, last);
    printf("test-p9: %u fids at once (the table's size), then ENFILE\n", made + inuse);
    for (unsigned i = 0; i < made; ++i)
        clunk(1000 + i);
    ck(p9_nfids == inuse && walk(0, 1000, "docs", 0) == 1 && !clunk(1000), "every fid back after the clunks");

    /* ---- the channel: batch bound, order, a guest that does not reap, malformed entries ---- */
    {
        uint32_t ids[SBSHM_P9_SLOTS];
        for (unsigned s = 0; s < SBSHM_P9_SLOTS; ++s) {
            t_begin(P9_TGETATTR);
            t4(1);
            t8(0x7ff);
            ids[s] = submit(s, t_end());
        }
        unsigned db0 = doorbells;
        sbg_p9_poll();
        ck(ch()->done_head - g_done_tail == SBG_P9_PER_POLL && doorbells == db0 + 1, "one poll serves %u, one doorbell",
           SBG_P9_PER_POLL);
        sbg_p9_poll();
        ck(ch()->done_head - g_done_tail == SBSHM_P9_SLOTS, "the next poll serves the rest");
        int order = 1;
        for (unsigned s = 0; s < SBSHM_P9_SLOTS; ++s) {
            struct sbshm_p9_ent e;
            order &= reap(&e) && e.id == ids[s] && e.slot == s && e.len == 7 + 153 && !e.status &&
                     slot_buf(s)[4] == P9_TGETATTR + 1;
        }
        ck(order, "completions in ring order, each a full Rgetattr in its own slot");
        /* a guest that does not reap: the host stops at a full completion ring */
        for (unsigned k = 0; k < SBSHM_P9_RING + 1; ++k) {
            t_begin(P9_TCLUNK);
            t4(4242);
            submit(k % SBSHM_P9_SLOTS, t_end());
            if (k % SBSHM_P9_SLOTS == SBSHM_P9_SLOTS - 1 || k == SBSHM_P9_RING)
                for (int p = 0; p < 4; ++p)
                    sbg_p9_poll();
        }
        ck(ch()->done_head - g_done_tail == SBSHM_P9_RING && ch()->req_tail == g_req_head - 1,
           "a full completion ring holds the host back");
        struct sbshm_p9_ent e;
        while (reap(&e))
            ;
        sbg_p9_poll();
        ck(reap(&e) && ch()->req_tail == g_req_head, "after the reap the last one is served");
        /* malformed entries: EIO, no answer in the slot */
        t_begin(P9_TCLUNK);
        t4(4242);
        unsigned len = t_end();
        submit(0, len);
        volatile struct sbshm_p9_ent *q = &ch()->req[(g_req_head - 1) & (SBSHM_P9_RING - 1)];
        q->slot = 99;
        sbg_p9_poll();
        ck(reap(&e) && e.status == -SBG_FS_EIO && !e.len, "a slot out of range: EIO");
        submit(0, 3);
        sbg_p9_poll();
        ck(reap(&e) && e.status == -SBG_FS_EIO && !e.len, "a 3-byte message: EIO");
        submit(0, len + 4);
        sbg_p9_poll();
        ck(reap(&e) && e.status == -SBG_FS_EIO && !e.len, "a size word that disagrees with the entry: EIO");
        submit(0, SBSHM_P9_SLOT + 1);
        sbg_p9_poll();
        ck(reap(&e) && e.status == -SBG_FS_EIO, "a length past the slot: EIO");
        ck(attach(50) && !clunk(50), "the channel still serves after malformed entries");
    }

    /* ---- read-only ---- */
    pfs.writable = 0;
    memset(shm, 0, SBSHM_P9_SIZE);      /* sbg_load zeroes the v3 regions before the services' loads */
    sbg_p9_load();                      /* a new boot: the channel says read-only, the fids are gone */
    g_req_head = g_done_tail = 0;
    ck(ch()->flags == SBSHM_P9_RO, "the channel says read-only");
    ck(version(SBSHM_P9_SLOT, "9P2000.L", v) == SBSHM_P9_SLOT && attach(0), "version + attach (read-only)");
    ck(walk(0, 1, "hello.txt", 0) == 1 && !getattr(1, &a) && (a.mode & 0777) == 0444, "files show 0444: %o", a.mode);
    ck(!getattr(0, &a) && (a.mode & 0777) == 0555, "directories show 0555");
    ck(lopen(1, 2) == -SBG_FS_EROFS && lopen(1, 01000) == -SBG_FS_EROFS, "lopen for writing / O_TRUNC: EROFS");
    ck(!lopen(1, 0) && readat(1, 0, 64, small) == 16, "reads work");
    ck(writeat(1, 0, "x", 1) == -SBG_FS_EROFS, "write: EROFS");
    ck(lcreate(0, "x", 0102, 0644) == -SBG_FS_EROFS && mkdir9(0, "x") == -SBG_FS_EROFS &&
           unlinkat9(0, "hello.txt", 0) == -SBG_FS_EROFS && renameat9(0, "hello.txt", 0, "y") == -SBG_FS_EROFS &&
           setattr9(1, P9_ATTR_SIZE, 0, 0) == -SBG_FS_EROFS, "create, mkdir, unlink, rename, setattr: EROFS");
    ck(host_has("hello.txt") && !host_has("x") && !host_has("y"), "nothing changed on the host");
    ck(walk(0, 2, "", 0) == 0, "clone");
    t_begin(P9_TREMOVE);
    t4(2);
    expect_err(SBG_FS_EROFS, "Tremove: EROFS");
    t_begin(P9_TSTATFS);
    t4(0);
    if (expect_ok("Tstatfs read-only")) {
        r4();
        r4();
        r8();
        ck(r8() == 0, "read-only statfs: no free blocks");
    }
    {   /* the 3 MiB read again, read-only */
        ck(walk(0, 6, "big.bin", 0) == 1 && !lopen(6, 0), "open big.bin read-only");
        uint64_t off = 0;
        for (int k; (k = readat(6, off, ms - 24, got + off)) > 0 && off <= sizeof(got);)
            off += (unsigned)k;
        ck(off == sizeof(big) && !memcmp(big, got, sizeof(big)), "3 MiB read matches (read-only)");
    }

    /* ---- a broken ring stops the service until the next load ---- */
    ch()->req_head = g_req_head + 100;
    int logs0 = logs;
    sbg_p9_poll();
    ck(p9_ring_bad && logs == logs0 + 1 && ch()->req_tail == g_req_head, "a req_head far ahead: service stopped, logged");
    services = 0;
    sbg_p9_load();
    ck(ch()->magic == SBSHM_P9_MAGIC, "the load without the service bit leaves the region alone");
    memset(shm, 0, SBSHM_P9_SIZE);
    services = SBSHM_SVC_P9;
    sbg_p9_load();
    g_req_head = g_done_tail = 0;
    ck(!p9_ring_bad && version(SBSHM_P9_SLOT, "9P2000.L", v) == SBSHM_P9_SLOT && attach(0), "served again after a load");

    const struct sbg_p9_stats *st = sbg_p9_stats();
    printf("test-p9: server stats: %u requests, %u Rlerror, %u malformed, %u doorbells, batch max %u, capped %u, "
           "fids max %u, read %llu bytes, wrote %llu bytes\n",
           st->requests, st->errors, st->bad, st->doorbells, st->batch_max, st->capped, st->fids_max,
           (unsigned long long)st->rd_bytes, (unsigned long long)st->wr_bytes);
    rm_rf(root);
    if (failed) {
        printf("test-p9: FAIL (%u of %u checks)\n", failed, checks);
        return 1;
    }
    printf("test-p9: PASS (%u checks)\n", checks);
    return 0;
}
