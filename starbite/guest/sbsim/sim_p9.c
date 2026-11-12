/* sim_p9: sbp9's stand-in in sbsim (included by sim_v3.c): SIM_FS, a read-write in-memory tree for the guest's 9P
 * mount. Freestanding like the rest of sbsim (MMU off: byte / word accesses, no libc; every name is sp9_*).
 *
 * The tree at boot (v3test/p9-expect.py computes the same files' md5 list for the guest's checks):
 *   /hello.txt                         "Hello from Starbite OS (sbsim)\n"
 *   /big.bin                           3 MiB, generated (seed 1)
 *   /Documents/report.bin              100000 bytes (seed 2)
 *   /Documents/sub/deep.bin            4097 bytes (seed 3)
 *   /Pictures/photo-00.bin .. -23.bin  1000 * (i + 1) + i bytes (seed 100 + i)
 *   /Music/                            empty
 *   /Long/<251 x 'L'>.bin              777 bytes (seed 4): a 255-byte name
 * Generated content: 8-byte block b of a file with seed s is splitmix64((s << 40) ^ b), little-endian. A generated
 * file that is written, truncated or grown becomes a copy in the data arena (copy on write). New files and
 * directories live in SP9_NODES nodes and the SP9_ARENA-byte arena (a bump allocator: space comes back only when the
 * newest file shrinks or goes; ENOSPC after that).
 */
#include "../host/sbg_p9.h"

#define SP9_NODES 160U
#define SP9_ARENA (6U << 20)

struct sp9_node {
    uint8_t used, dir;
    uint16_t parent;                    /* node index (the root: 0, its own parent) */
    uint32_t seed;                      /* generated content (0: none) */
    const char *text;                   /* fixed text (NULL: none) */
    uint64_t size;
    uint32_t off, cap;                  /* the copy in the arena (cap 0: none) */
    int64_t mtime;
    char name[SBG_FS_NAME_MAX + 1];
};
static struct sp9_node sp9_n[SP9_NODES];
static uint8_t sp9_arena[SP9_ARENA];
static uint32_t sp9_top;                /* the arena's bump pointer */
static int sp9_ready;

static uint64_t sp9_mix(uint64_t z)
{
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static uint8_t sp9_gen(uint32_t seed, uint64_t off) { return (uint8_t)(sp9_mix(((uint64_t)seed << 40) ^ (off >> 3)) >> (8 * (off & 7))); }

static unsigned sp9_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        ++n;
    return n;
}
static int sp9_eq(const char *a, const char *b, unsigned bn)   /* a (NUL-terminated) == b[0..bn) */
{
    unsigned i = 0;
    for (; i < bn; ++i)
        if (a[i] != b[i])
            return 0;
    return !a[i];
}
static void sp9_setname(struct sp9_node *n, const char *s, unsigned len)
{
    unsigned i = 0;
    for (; i < len && i < SBG_FS_NAME_MAX; ++i)
        n->name[i] = s[i];
    n->name[i] = 0;
}
static int64_t sp9_now(void)
{
    int32_t tz;
    return (int64_t)sim_unix_s(&tz);
}

static int sp9_add(unsigned parent, const char *name, int dir, uint32_t seed, uint64_t size, const char *text)
{
    for (unsigned i = 1; i < SP9_NODES; ++i)
        if (!sp9_n[i].used) {
            struct sp9_node *n = &sp9_n[i];
            n->used = 1;
            n->dir = (uint8_t)(dir != 0);
            n->parent = (uint16_t)parent;
            n->seed = seed;
            n->text = text;
            n->size = text ? sp9_len(text) : size;
            n->off = n->cap = 0;
            n->mtime = 1794484800;      /* 2026-11-12 12:00 UTC */
            sp9_setname(n, name, sp9_len(name));
            return (int)i;
        }
    return -SBG_FS_ENOSPC;
}

static void sp9_init(void)
{
    if (sp9_ready)
        return;
    sp9_ready = 1;
    sp9_n[0].used = sp9_n[0].dir = 1;
    sp9_n[0].mtime = 1794484800;
    sp9_add(0, "hello.txt", 0, 0, 0, "Hello from Starbite OS (sbsim)\n");
    sp9_add(0, "big.bin", 0, 1, 3U << 20, 0);
    int docs = sp9_add(0, "Documents", 1, 0, 0, 0);
    sp9_add((unsigned)docs, "report.bin", 0, 2, 100000, 0);
    int sub = sp9_add((unsigned)docs, "sub", 1, 0, 0, 0);
    sp9_add((unsigned)sub, "deep.bin", 0, 3, 4097, 0);
    int pics = sp9_add(0, "Pictures", 1, 0, 0, 0);
    for (unsigned i = 0; i < 24; ++i) {
        char nm[] = "photo-00.bin";
        nm[6] = (char)('0' + i / 10);
        nm[7] = (char)('0' + i % 10);
        sp9_add((unsigned)pics, nm, 0, 100 + i, 1000 * (i + 1) + i, 0);
    }
    sp9_add(0, "Music", 1, 0, 0, 0);
    int lng = sp9_add(0, "Long", 1, 0, 0, 0);
    static char ln[SBG_FS_NAME_MAX + 1];
    for (unsigned i = 0; i < 251; ++i)
        ln[i] = 'L';
    ln[251] = '.', ln[252] = 'b', ln[253] = 'i', ln[254] = 'n', ln[255] = 0;
    sp9_add((unsigned)lng, ln, 0, 4, 777, 0);
}

/* a canonical path -> node index, or -ENOENT / -ENOTDIR */
static int sp9_find(const char *path)
{
    sp9_init();
    unsigned cur = 0;
    const char *p = path;
    while (*p == '/')
        ++p;
    while (*p) {
        unsigned len = 0;
        while (p[len] && p[len] != '/')
            ++len;
        if (!sp9_n[cur].dir)
            return -SBG_FS_ENOTDIR;
        int hit = -1;
        for (unsigned i = 1; i < SP9_NODES && hit < 0; ++i)
            if (sp9_n[i].used && sp9_n[i].parent == cur && sp9_eq(sp9_n[i].name, p, len))
                hit = (int)i;
        if (hit < 0)
            return -SBG_FS_ENOENT;
        cur = (unsigned)hit;
        p += len;
        while (*p == '/')
            ++p;
    }
    return (int)cur;
}
/* the parent directory's node of a path and the last name in it */
static int sp9_split(const char *path, const char **name)
{
    unsigned n = sp9_len(path), slash = 0;
    for (unsigned i = 0; i < n; ++i)
        if (path[i] == '/')
            slash = i;
    *name = path + slash + 1;
    if (!**name)
        return -SBG_FS_EINVAL;
    static char par[SBG_P9_PATH];
    if (slash >= sizeof(par))
        return -SBG_FS_ENAMETOOLONG;
    for (unsigned i = 0; i < slash; ++i)
        par[i] = path[i];
    par[slash ? slash : 1] = 0;
    if (!slash)
        par[0] = '/';
    int d = sp9_find(par);
    if (d >= 0 && !sp9_n[d].dir)
        return -SBG_FS_ENOTDIR;
    return d;
}

static uint8_t sp9_byte(const struct sp9_node *n, uint64_t off)
{
    if (n->cap)
        return sp9_arena[n->off + off];
    if (n->text)
        return (uint8_t)n->text[off];
    return n->seed ? sp9_gen(n->seed, off) : 0;
}

/* room for `need` bytes in the arena for node n (its first `need` bytes kept): 0 or -ENOSPC */
static int sp9_room(struct sp9_node *n, uint64_t need)
{
    if (n->cap && need <= n->cap)
        return 0;
    if (need > SP9_ARENA)
        return -SBG_FS_ENOSPC;
    if (n->cap && n->off + n->cap == sp9_top && n->off + need <= SP9_ARENA) {     /* the newest: grow in place */
        sp9_top = n->off + (uint32_t)need;
        n->cap = (uint32_t)need;
        return 0;
    }
    uint64_t cap = need < 65536 ? 65536 : need + need / 2;
    if (sp9_top + cap > SP9_ARENA)
        cap = need;
    if (sp9_top + cap > SP9_ARENA)
        return -SBG_FS_ENOSPC;
    uint32_t off = sp9_top;
    for (uint64_t i = 0; i < n->size && i < need; ++i)  /* copy on write (generated / text / an older copy) */
        sp9_arena[off + i] = sp9_byte(n, i);
    n->off = off;
    n->cap = (uint32_t)cap;
    n->seed = 0;
    n->text = 0;
    sp9_top += (uint32_t)cap;
    return 0;
}
static void sp9_drop(struct sp9_node *n)
{
    if (n->cap && n->off + n->cap == sp9_top)
        sp9_top = n->off;               /* the newest copy: its space comes back */
    n->cap = 0;
}

static void sp9_attr(const struct sp9_node *n, unsigned idx, struct sbg_fs_attr *a)
{
    *a = (struct sbg_fs_attr){0};
    a->mode = n->dir ? SBG_FS_S_IFDIR | 0755U : SBG_FS_S_IFREG | 0644U;
    a->size = n->dir ? 0 : n->size;
    a->ino = 0x5000 + idx;              /* stable while the node lives (renames keep it) */
    a->mtime = n->mtime;
}

static int sp9_stat(void *ctx, const char *path, struct sbg_fs_attr *a)
{
    (void)ctx;
    int i = sp9_find(path);
    if (i < 0)
        return i;
    sp9_attr(&sp9_n[i], (unsigned)i, a);
    return 0;
}

static int sp9_list(void *ctx, const char *dir, uint32_t first, struct sbg_fs_dirent *out, unsigned max)
{
    (void)ctx;
    int d = sp9_find(dir);
    if (d < 0)
        return d;
    if (!sp9_n[d].dir)
        return -SBG_FS_ENOTDIR;
    uint32_t seen = 0;
    unsigned k = 0;
    for (unsigned i = 1; i < SP9_NODES && k < max; ++i) {
        if (!sp9_n[i].used || sp9_n[i].parent != (unsigned)d)
            continue;
        if (seen++ < first)
            continue;
        unsigned j = 0;
        for (; sp9_n[i].name[j]; ++j)
            out[k].name[j] = sp9_n[i].name[j];
        out[k].name[j] = 0;
        sp9_attr(&sp9_n[i], i, &out[k].a);
        ++k;
    }
    return (int)k;
}

static int sp9_read(void *ctx, const char *path, uint64_t off, void *dst, uint32_t len)
{
    (void)ctx;
    int i = sp9_find(path);
    if (i < 0)
        return i;
    struct sp9_node *n = &sp9_n[i];
    if (n->dir)
        return -SBG_FS_EISDIR;
    if (off >= n->size)
        return 0;
    uint64_t k = n->size - off < len ? n->size - off : len;
    volatile uint8_t *d = (volatile uint8_t *)dst;
    if (n->cap) {
        for (uint64_t j = 0; j < k; ++j)
            d[j] = sp9_arena[n->off + off + j];
    } else if (n->seed && !(off & 7)) {             /* generated, whole blocks at a time */
        for (uint64_t j = 0; j < k; j += 8) {
            uint64_t v = sp9_mix(((uint64_t)n->seed << 40) ^ ((off + j) >> 3));
            for (unsigned b = 0; b < 8 && j + b < k; ++b)
                d[j + b] = (uint8_t)(v >> (8 * b));
        }
    } else {
        for (uint64_t j = 0; j < k; ++j)
            d[j] = sp9_byte(n, off + j);
    }
    return (int)k;
}

static int sp9_write(void *ctx, const char *path, uint64_t off, const void *src, uint32_t len)
{
    (void)ctx;
    int i = sp9_find(path);
    if (i < 0)
        return i;
    struct sp9_node *n = &sp9_n[i];
    if (n->dir)
        return -SBG_FS_EISDIR;
    uint64_t end = off + len;
    if (end > SP9_ARENA)
        return -SBG_FS_EFBIG;
    int e = sp9_room(n, end > n->size ? end : n->size);
    if (e)
        return e;
    for (uint64_t j = n->size; j < off; ++j)        /* a hole reads as zeros */
        sp9_arena[n->off + j] = 0;
    const volatile uint8_t *s = (const volatile uint8_t *)src;
    for (uint32_t j = 0; j < len; ++j)
        sp9_arena[n->off + off + j] = s[j];
    if (end > n->size)
        n->size = end;
    n->mtime = sp9_now();
    return (int)len;
}

static int sp9_mk(const char *path, int dir)
{
    const char *name;
    int d = sp9_split(path, &name);
    if (d < 0)
        return d;
    if (sp9_find(path) >= 0)
        return -SBG_FS_EEXIST;
    int i = sp9_add((unsigned)d, name, dir, 0, 0, 0);
    if (i < 0)
        return i;
    sp9_n[i].mtime = sp9_now();
    return 0;
}
static int sp9_create(void *ctx, const char *path, uint32_t mode)
{
    (void)ctx, (void)mode;
    return sp9_mk(path, 0);
}
static int sp9_mkdir(void *ctx, const char *path, uint32_t mode)
{
    (void)ctx, (void)mode;
    return sp9_mk(path, 1);
}
static int sp9_empty(unsigned d)
{
    for (unsigned i = 1; i < SP9_NODES; ++i)
        if (sp9_n[i].used && sp9_n[i].parent == d)
            return 0;
    return 1;
}
static int sp9_remove(void *ctx, const char *path, int dir)
{
    (void)ctx;
    int i = sp9_find(path);
    if (i < 0)
        return i;
    if (!i)
        return -SBG_FS_EBUSY;
    struct sp9_node *n = &sp9_n[i];
    if (dir && !n->dir)
        return -SBG_FS_ENOTDIR;
    if (!dir && n->dir)
        return -SBG_FS_EISDIR;
    if (n->dir && !sp9_empty((unsigned)i))
        return -SBG_FS_ENOTEMPTY;
    sp9_drop(n);
    n->used = 0;
    return 0;
}
static int sp9_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    int s = sp9_find(from);
    if (s < 0)
        return s;
    if (!s)
        return -SBG_FS_EBUSY;
    const char *name;
    int d = sp9_split(to, &name);
    if (d < 0)
        return d;
    for (unsigned p = (unsigned)d;; p = sp9_n[p].parent) {      /* not into itself */
        if (p == (unsigned)s)
            return -SBG_FS_EINVAL;
        if (!p)
            break;
    }
    int t = sp9_find(to);
    if (t == s)
        return 0;
    if (t >= 0) {                       /* replace the target, as rename(2) */
        if (sp9_n[t].dir && !sp9_n[s].dir)
            return -SBG_FS_EISDIR;
        if (!sp9_n[t].dir && sp9_n[s].dir)
            return -SBG_FS_ENOTDIR;
        if (sp9_n[t].dir && !sp9_empty((unsigned)t))
            return -SBG_FS_ENOTEMPTY;
        sp9_drop(&sp9_n[t]);
        sp9_n[t].used = 0;
    }
    sp9_n[s].parent = (uint16_t)d;
    sp9_setname(&sp9_n[s], name, sp9_len(name));
    sp9_n[s].mtime = sp9_now();
    return 0;
}
static int sp9_truncate(void *ctx, const char *path, uint64_t size)
{
    (void)ctx;
    int i = sp9_find(path);
    if (i < 0)
        return i;
    struct sp9_node *n = &sp9_n[i];
    if (n->dir)
        return -SBG_FS_EISDIR;
    if (size == n->size)
        return 0;
    if (!size) {                        /* emptied: nothing to keep */
        sp9_drop(n);
        n->seed = 0;
        n->text = 0;
    } else {
        int e = sp9_room(n, size);      /* (shrinking a generated file copies only what stays) */
        if (e)
            return e;
        for (uint64_t j = n->size; j < size; ++j)
            sp9_arena[n->off + j] = 0;
    }
    n->size = size;
    n->mtime = sp9_now();
    return 0;
}
static int sp9_set_mtime(void *ctx, const char *path, int64_t s, uint32_t ns)
{
    (void)ctx, (void)ns;
    int i = sp9_find(path);
    if (i < 0)
        return i;
    sp9_n[i].mtime = s;
    return 0;
}
static int sp9_statfs(void *ctx, struct sbg_fs_statfs *s)
{
    (void)ctx;
    unsigned used = 0;
    for (unsigned i = 0; i < SP9_NODES; ++i)
        used += sp9_n[i].used;
    s->bsize = 4096;
    s->blocks = SP9_ARENA / 4096;
    s->bfree = (SP9_ARENA - sp9_top) / 4096;
    s->files = SP9_NODES;
    s->ffree = SP9_NODES - used;
    return 0;
}

static const struct sbg_fs sp9_fs = {
    .ctx = 0, .writable = 1, .uid = 1000, .gid = 1000, .name = "sbsim tree",
    .stat = sp9_stat, .list = sp9_list, .read = sp9_read, .statfs = sp9_statfs,
    .write = sp9_write, .create = sp9_create, .mkdir = sp9_mkdir, .remove = sp9_remove, .rename = sp9_rename,
    .truncate = sp9_truncate, .set_mtime = sp9_set_mtime, .sync = 0,
};
#define SIM_FS (&sp9_fs)

/* once in a while (10 s), when something changed: what the server did, on the console */
static void sim_p9_tick(uint64_t t)
{
    static uint64_t last;
    static uint32_t last_req;
    const struct sbg_p9_stats *s = sbg_p9_stats();
    if (t - last < 10000 || s->requests == last_req)
        return;
    last = t;
    last_req = s->requests;
    puts_("sbsim: sbp9: ");
    putu(s->requests);
    puts_(" requests (");
    putu(s->errors);
    puts_(" Rlerror, ");
    putu(s->bad);
    puts_(" malformed), read ");
    putu(s->rd_bytes >> 10);
    puts_(" KiB, wrote ");
    putu(s->wr_bytes >> 10);
    puts_(" KiB, most in one poll ");
    putu(s->batch_max);
    puts_(", fids max ");
    putu(s->fids_max);
    puts_(", arena ");
    putu(sp9_top >> 10);
    puts_(" KiB used\n");
}
