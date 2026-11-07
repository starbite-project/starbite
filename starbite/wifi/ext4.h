/* SPDX-License-Identifier: GPL-2.0-only
 * bcmw's read-only ext4 reader (ext4.c): the radio's files from VENDOR. */
#ifndef BCMW_EXT4_H
#define BCMW_EXT4_H
#include <stdint.h>

#define EXT4_MAX_BS 4096u
#define EXT4_INODE_MAX 256u

#define EXT4_E_IO          -1
#define EXT4_E_NOTEXT4     -2   /* no ext4 magic (EROFS? f2fs?) */
#define EXT4_E_UNSUPPORTED -3
#define EXT4_E_CORRUPT     -4
#define EXT4_E_NOTFOUND    -5

struct ext4 {
    /* bytes [off, off + len) of the partition into dst; len <= the block size; 0 on success */
    int (*read)(void *ctx, uint64_t off, void *dst, uint32_t len);
    void *ctx;
    uint32_t bs, first_data, bpg, ipg, inode_size, desc_size;
    uint8_t buf[EXT4_MAX_BS], dir[EXT4_MAX_BS];
};

int ext4_open(struct ext4 *fs);
int ext4_lookup(struct ext4 *fs, const char *path, uint32_t *ino);
int ext4_inode(struct ext4 *fs, uint32_t ino, uint8_t inode[EXT4_INODE_MAX]);
uint64_t ext4_size(const uint8_t *inode);
int ext4_read(struct ext4 *fs, const uint8_t *inode, uint64_t off, uint8_t *dst, uint32_t len);

#endif
