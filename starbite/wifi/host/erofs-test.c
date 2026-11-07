/* SPDX-License-Identifier: GPL-2.0-only
 * erofs.c against images made by mkfs.erofs (host/run.sh) and, with BCMW_VENDOR_IMG, against a
 * copy of the phone's own VENDOR image: every file named is read through the reader and compared
 * with the original (for the phone's image: with what fsck.erofs --extract made of it).
 * Usage: erofs-test image root path...   (path relative to root, also the path inside the image) */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "../erofs.h"

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
    static struct erofs fs;
    int fails = 0, e;
    uint64_t nid;
    uint8_t lz[64], out[64];

    /* LZ4 by hand: literals "abcd", then a match of 8 at distance 4 -> "abcdabcdabcd" */
    lz[0] = 0x44;
    memcpy(lz + 1, "abcd", 4);
    lz[5] = 4, lz[6] = 0;
    if (erofs_lz4(lz, 7, out, 12) != 12 || memcmp(out, "abcdabcdabcd", 12)) {
        printf("FAIL erofs: the hand-made LZ4 sequence\n");
        fails++;
    }
    if (erofs_lz4(lz, 7, out, 6) != 6 || memcmp(out, "abcdab", 6)) {
        printf("FAIL erofs: a partial LZ4 decode\n");
        fails++;
    }
    lz[5] = 9;          /* an offset before the start */
    if (erofs_lz4(lz, 7, out, 12) != -1) {
        printf("FAIL erofs: a bad LZ4 offset accepted\n");
        fails++;
    }
    if (argc < 4 || !(img = fopen(argv[1], "rb")))
        return 2;
    fs.read = rdf;
    if ((e = erofs_open(&fs))) {
        printf("FAIL erofs: %s does not open (%d)\n", argv[1], e);
        return 1;
    }
    for (int i = 3; i < argc; i++) {
        char p[2048];
        FILE *f;
        struct erofs_inode in;
        uint8_t *a, *b;
        long n;
        snprintf(p, sizeof(p), "%s/%s", argv[2], argv[i]);
        f = fopen(p, "rb");
        if (!f) {
            printf("FAIL erofs: no reference %s\n", p);
            fails++;
            continue;
        }
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        a = malloc((size_t)n + 1);
        b = calloc(1, (size_t)n + 1);
        if (fread(a, 1, (size_t)n, f) != (size_t)n)
            n = -1;
        fclose(f);
        if ((e = erofs_lookup(&fs, argv[i], &nid)) || (e = erofs_inode(&fs, nid, &in)) || in.size != (uint64_t)n ||
            (e = erofs_read_all(&fs, &in, b)) || memcmp(a, b, (size_t)n)) {
            printf("FAIL erofs: %s in %s (error %d, layout %u)\n", argv[i], argv[1], e, in.layout);
            fails++;
        }
        free(a);
        free(b);
    }
    if (erofs_lookup(&fs, "firmware/no-such-file", &nid) != EROFS_E_NOTFOUND) {
        printf("FAIL erofs: a missing file found\n");
        fails++;
    }
    printf("erofs: %s: %d files, %d failed\n", argv[1], argc - 3, fails);
    return fails != 0;
}
