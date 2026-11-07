/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw's read-only EROFS reader (erofs.c): the radio's files from a One UI VENDOR. */
#ifndef BCMW_EROFS_H
#define BCMW_EROFS_H
#include <stdint.h>

#define EROFS_MAX_BS 4096u

#define EROFS_E_IO          -1
#define EROFS_E_NOTEROFS    -2
#define EROFS_E_UNSUPPORTED -3
#define EROFS_E_CORRUPT     -4
#define EROFS_E_NOTFOUND    -5

struct erofs {
    int (*read)(void *ctx, uint64_t off, void *dst, uint32_t len);   /* bytes of the partition; 0 ok */
    void *ctx;
    uint32_t bs, blkbits, meta, incompat;
    uint64_t root;
    uint8_t buf[EROFS_MAX_BS], dir[EROFS_MAX_BS];
};

struct erofs_inode {
    uint64_t nid, iloc, size;
    uint32_t startblk, isize, xsize;
    uint16_t mode;
    uint8_t extended, layout;
};

int erofs_open(struct erofs *fs);
int erofs_lookup(struct erofs *fs, const char *path, uint64_t *nid);
int erofs_inode(struct erofs *fs, uint64_t nid, struct erofs_inode *in);
/* the whole file into dst (in->size bytes) */
int erofs_read_all(struct erofs *fs, const struct erofs_inode *in, uint8_t *dst);
/* LZ4 block decoding up to `want` bytes (exposed for the host test) */
int erofs_lz4(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t want);

#endif
