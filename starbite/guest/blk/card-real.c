/* SPDX-License-Identifier: GPL-2.0-only
 * card-test.sh's second part: the resolver of guest-card.h on an exFAT volume made by mkfs.exfat and filled by
 * the Linux kernel's exFAT driver: the run it finds for a file must hold exactly the file's bytes, and a file
 * the kernel wrote in pieces must be seen as it is (one run or fragmented).
 *   card-real CARD.img BASE.bin OVERLAY.bin FRAG.bin   (the files' contents as they were copied in) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GCARD_HOST_TEST
#include "guest-card.h"

static uint8_t *card;
static uint64_t card_sectors;
static int rd(uint64_t sector, uint32_t count, void *dst)
{
    if (sector + count > card_sectors)
        return -1;
    memcpy(dst, card + sector * 512, (size_t)count * 512);
    return 0;
}
static uint8_t *slurp(const char *p, uint64_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = (uint64_t)ftell(f);
    rewind(f);
    uint8_t *b = malloc(*n);
    if (fread(b, 1, *n, f) != *n) exit(2);
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    static struct exfat fs;
    static uint8_t cbuf[1 << 17];
    static uint64_t sec[64];
    uint64_t n, ps, pl, s, ns;
    int fails = 0;
    if (argc < 5)
        return 2;
    card = slurp(argv[1], &n);
    card_sectors = n / 512;
    const char *no = gcard_partition(rd, (uint8_t *)sec, &ps, &pl);
    if (no || exfat_mount(&fs, rd, ps, cbuf, sizeof(cbuf))) {
        printf("FAIL card-real: the volume does not mount: %s\n", no ? no : "exfat_mount");
        return 1;
    }
    static const char *const path[2] = {GCARD_BASE, GCARD_OVERLAY};
    for (int i = 0; i < 2; ++i) {
        uint64_t fn;
        uint8_t *want = slurp(argv[2 + i], &fn);
        no = gcard_extent(&fs, ps, pl, path[i], (uint8_t *)sec, &s, &ns);
        int ok = !no && ns == fn / 4096 * 8 && !memcmp(card + s * 512, want, ns * 512);
        printf("%s card-real: %s: %s (sectors %llu + %llu, %s)\n", ok ? "PASS" : "FAIL", path[i],
               no ? no : ok ? "one run, its bytes are the file's" : "the run's bytes differ from the file",
               (unsigned long long)s, (unsigned long long)ns, no ? "-" : (s & 7) ? "off a 4 KiB boundary" : "4 KiB aligned");
        fails += !ok;
        free(want);
    }
    /* the file written in turns with another: whatever the kernel did, the verdict must match the bytes */
    uint64_t fn;
    uint8_t *want = slurp(argv[4], &fn);
    no = gcard_extent(&fs, ps, pl, GCARD_DIR "frag.img", (uint8_t *)sec, &s, &ns);
    int same = !no && !memcmp(card + s * 512, want, ns * 512);
    int ok = no ? strstr(no, "fragmented") != 0 : same;
    printf("%s card-real: frag.img (grown in turns with another file): %s\n", ok ? "PASS" : "FAIL",
           no ? no : same ? "the kernel kept it in one run, and the run's bytes are the file's" :
                            "OFFERED, but the run's bytes are not the file's");
    fails += !ok;
    return fails ? 1 : 0;
}
