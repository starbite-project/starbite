/* SPDX-License-Identifier: GPL-2.0-only
 * ext4.c against images made by mke2fs -d (host/run.sh): every file named on the command line is
 * read through the reader and compared with the original.
 * Usage: ext4-test image root path...   (path relative to root, also the path inside the image) */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include "../ext4.h"

static FILE *img;

static int rdf(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    (void)ctx;
    if (fseeko(img, (off_t)off, SEEK_SET) || fread(dst, 1, len, img) != len)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    static struct ext4 fs;
    int fails = 0;
    uint32_t ino;

    if (argc < 4 || !(img = fopen(argv[1], "rb")))
        return 2;
    fs.read = rdf;
    if (ext4_open(&fs)) {
        printf("FAIL ext4: %s does not open\n", argv[1]);
        return 1;
    }
    for (int i = 3; i < argc; i++) {
        char p[2048];
        FILE *f;
        uint8_t inode[EXT4_INODE_MAX], *a, *b;
        long n;
        snprintf(p, sizeof(p), "%s/%s", argv[2], argv[i]);
        f = fopen(p, "rb");
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        a = malloc((size_t)n + 1);
        b = malloc((size_t)n + 1);
        if (fread(a, 1, (size_t)n, f) != (size_t)n)
            n = -1;
        fclose(f);
        if (ext4_lookup(&fs, argv[i], &ino) || ext4_inode(&fs, ino, inode) || ext4_size(inode) != (uint64_t)n ||
            ext4_read(&fs, inode, 0, b, (uint32_t)n) || memcmp(a, b, (size_t)n)) {
            printf("FAIL ext4: %s in %s\n", argv[i], argv[1]);
            fails++;
        }
        free(a);
        free(b);
    }
    if (ext4_lookup(&fs, "firmware/no-such-file", &ino) != EXT4_E_NOTFOUND) {
        printf("FAIL ext4: a missing file found\n");
        fails++;
    }
    printf("ext4: %s (block %u, %u-byte descriptors): %d files, %d failed\n", argv[1], fs.bs, fs.desc_size, argc - 3, fails);
    return fails != 0;
}
