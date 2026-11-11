/* Host test of shim/starlte/sbfwfs.c (the firmware layer's read-only parsers) on real files:
 *   fwtest bcd FILE            the BCD check and the Advanced Startup plan
 *   fwtest esp IMAGE [PATH]    FAT32 health, then PATH (default /EFI/Microsoft/Boot/BCD) found, read, BCD-checked
 *   fwtest ntfs IMAGE          NTFS health
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sbfwfs.h"

static FILE *F;
static int rd(void *u, uint64_t off, void *buf, uint32_t len)
{
    (void)u;
    if (fseeko(F, (off_t)off, SEEK_SET))
        return -1;
    return fread(buf, 1, len, F) == len ? 0 : -1;
}
static void line(void *u, const char *s) { (void)u; printf("  %s\n", s); }

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    F = fopen(argv[2], "rb");
    if (!F)
        return 2;
    fseeko(F, 0, SEEK_END);
    struct fw_io io = { rd, 0, (uint64_t)ftello(F) };
    struct fw_out o = { line, 0, 0 };
    static uint8_t buf[1 << 20];
    struct bcd_plan p;
    if (!strcmp(argv[1], "bcd")) {
        size_t n = (size_t)io.size;
        fseeko(F, 0, SEEK_SET);
        if (n > sizeof(buf) || fread(buf, 1, n, F) != n)
            return 2;
        bcd_check(buf, (uint32_t)n, &p, &o);
    } else if (!strcmp(argv[1], "esp")) {
        struct fat32 f;
        if (!fat32_mount(&f, &io)) {
            printf("  not FAT32\n");
            return 1;
        }
        fat32_health(&f, &o);
        const char *path = argc > 3 ? argv[3] : "/EFI/Microsoft/Boot/BCD";
        uint32_t c, sz;
        if (!fat32_find(&f, path, &c, &sz)) {
            printf("  %s: not found\n", path);
            return 1;
        }
        int64_t n = fat32_read(&f, c, sz, buf, sizeof(buf));
        printf("  %s: cluster %u, %u bytes, read %lld\n", path, c, sz, (long long)n);
        if (n > 0 && !memcmp(buf, "regf", 4)) {
            bcd_check(buf, (uint32_t)n, &p, &o);
            uint64_t po;
            if (p.ok && fat32_map(&f, c, p.off, &po))
                printf("  plan: partition byte %llu = 4 KiB block %llu + %llu\n", (unsigned long long)po,
                       (unsigned long long)(po / 4096), (unsigned long long)(po % 4096));
        }
    } else if (!strcmp(argv[1], "ntfs")) {
        ntfs_health(&io, &o);
    }
    printf("  problems: %d\n", o.bad);
    return 0;
}
