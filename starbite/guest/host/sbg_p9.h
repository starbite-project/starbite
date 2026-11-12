/* SPDX-License-Identifier: GPL-2.0-only
 * sbg_p9: Starbite OS's 9P2000.L file server for the Linux guest (contract v3 sbp9, ../sbshm-p9.h). The host OS
 * hands sbghost a struct sbg_fs (sbg_os.fs): a few path-based calls over whatever holds the files (Starbite OS:
 * the microSD card's exFAT volume, ../sbos/guest-fs.h; sbsim: an in-memory tree; test-p9.c: a POSIX directory).
 * sbghost serves the guest's channel from sbg_poll (sbg_p9_poll: a bounded batch per poll).
 *
 * Paths: always absolute, canonical, "/" for the share's root, components joined by '/', no "." / "..", no empty
 * components, each at most SBG_FS_NAME_MAX bytes, the whole below SBG_P9_PATH bytes. Return values: >= 0 ok, else a
 * negative LINUX errno (SBG_FS_E*: the host's own <errno.h> values may differ). Buffers passed to read / write are
 * the slot in the shared region (non-cacheable on the phone, Device memory in sbsim): no unaligned word accesses.
 *
 * Writes: when `writable` is 0 every change answers EROFS and the write calls are never made (they may be NULL);
 * when it is 1, a NULL write call answers EOPNOTSUPP. Freestanding: no allocation, fixed tables (SBG_P9_FIDS fids).
 */
#ifndef SBG_P9_H
#define SBG_P9_H
#include <stdint.h>

#define SBG_FS_NAME_MAX 255U
#ifndef SBG_P9_PATH
#define SBG_P9_PATH 1024U               /* bytes of a path, with its NUL */
#endif
#ifndef SBG_P9_FIDS
#define SBG_P9_FIDS 256U                /* fids the guest may hold at once (an open file or a dentry in use: one each) */
#endif

/* Linux errno values (asm-generic/errno*.h) */
enum {
    SBG_FS_EPERM = 1, SBG_FS_ENOENT = 2, SBG_FS_EIO = 5, SBG_FS_EBADF = 9, SBG_FS_EAGAIN = 11, SBG_FS_ENOMEM = 12,
    SBG_FS_EACCES = 13, SBG_FS_EBUSY = 16, SBG_FS_EEXIST = 17, SBG_FS_EXDEV = 18, SBG_FS_ENOTDIR = 20, SBG_FS_EISDIR = 21,
    SBG_FS_EINVAL = 22, SBG_FS_ENFILE = 23, SBG_FS_EFBIG = 27, SBG_FS_ENOSPC = 28, SBG_FS_EROFS = 30,
    SBG_FS_ENAMETOOLONG = 36, SBG_FS_ENOTEMPTY = 39, SBG_FS_ENODATA = 61, SBG_FS_EPROTO = 71,
    SBG_FS_EOPNOTSUPP = 95,
};
#define SBG_FS_S_IFDIR 0040000U
#define SBG_FS_S_IFREG 0100000U

struct sbg_fs_attr {
    uint64_t size;                      /* bytes (a directory: anything, 0 is fine) */
    uint64_t ino;                       /* a stable number for the file, or 0: the server derives one from the path */
    uint32_t mode;                      /* SBG_FS_S_IFDIR or SBG_FS_S_IFREG | permission bits; bits 0 = 0755 / 0644 */
    uint32_t nlink;                     /* 0 = 1 (2 for a directory) */
    int64_t mtime;                      /* seconds since 1970 UTC (0: unknown) */
    uint32_t mtime_ns;
    uint32_t pad;
};

struct sbg_fs_dirent {
    char name[SBG_FS_NAME_MAX + 1];     /* NUL-terminated, no '/', not "." or ".." */
    struct sbg_fs_attr a;               /* at least mode's type bits and ino (or 0) */
};

struct sbg_fs_statfs {
    uint32_t bsize;                     /* 0 = 4096 */
    uint32_t pad;
    uint64_t blocks, bfree, files, ffree;
};

struct sbg_fs {
    void *ctx;                          /* passed to every call */
    uint32_t writable;                  /* 0: read-only (EROFS for every change) */
    uint32_t uid, gid;                  /* the owner the guest sees on every file (the desktop user: 1000) */
    const char *name;                   /* for logs and the channel's name word ("Starbite card"); may be NULL */
    /* ---- always ---- */
    int (*stat)(void *ctx, const char *path, struct sbg_fs_attr *a);        /* -ENOENT when it is not there */
    /* entries [first, first + max) of a directory, in a stable order without "." / "..": how many were stored;
     * fewer than max = the end. -ENOTDIR / -ENOENT. */
    int (*list)(void *ctx, const char *dir, uint32_t first, struct sbg_fs_dirent *out, unsigned max);
    /* up to len bytes from off: how many (0 at or past the end) */
    int (*read)(void *ctx, const char *path, uint64_t off, void *dst, uint32_t len);
    int (*statfs)(void *ctx, struct sbg_fs_statfs *s);                       /* NULL: made-up numbers */
    /* ---- writable only ---- */
    int (*write)(void *ctx, const char *path, uint64_t off, const void *src, uint32_t len);  /* how many */
    int (*create)(void *ctx, const char *path, uint32_t mode);   /* a new empty file; -EEXIST when the path exists */
    int (*mkdir)(void *ctx, const char *path, uint32_t mode);    /* -EEXIST */
    int (*remove)(void *ctx, const char *path, int dir);         /* dir 1: an empty directory (-ENOTEMPTY), 0: a file */
    int (*rename)(void *ctx, const char *from, const char *to);  /* like rename(2): replaces a file at `to` */
    int (*truncate)(void *ctx, const char *path, uint64_t size);
    int (*set_mtime)(void *ctx, const char *path, int64_t s, uint32_t ns);   /* NULL: times are not kept */
    int (*sync)(void *ctx, const char *path);                    /* NULL: nothing to do (fsync answers 0) */
};

struct sbg_p9_stats {
    uint32_t requests, errors, bad;     /* exchanges answered, Rlerror answers, malformed exchanges (EIO) */
    uint32_t doorbells, batch_max, capped, poll_ms_max;
    uint32_t fids_max;                  /* the most fids in use at once */
    uint64_t rd_bytes, wr_bytes;
};

/* The guest's channel (from sbghost: sbg_p9_load at sbg_load, sbg_p9_poll at every sbg_poll). */
const struct sbg_p9_stats *sbg_p9_stats(void);

/* The 9P server alone (the channel calls it per exchange; test-p9.c calls it directly): msg holds a T-message of
 * len bytes in a buffer of cap bytes; the R-message is written over it and its length returned (0: no answer, a
 * malformed message). msize_max: the largest msize Tversion may agree to (<= cap). */
unsigned sbg_p9_serve(const struct sbg_fs *fs, uint8_t *msg, unsigned len, unsigned cap, unsigned msize_max);
/* Forget every fid (a new guest boot); Tversion does the same. */
void sbg_p9_reset(void);
#endif
