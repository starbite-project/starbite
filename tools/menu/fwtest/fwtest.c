/* Host test of shim/starlte/sbfwfs.c (the firmware layer's read-only parsers) on real files:
 *   fwtest bcd FILE            the BCD check and the Advanced Startup plan
 *   fwtest esp IMAGE [PATH]    FAT32 health, then PATH (default /EFI/Microsoft/Boot/BCD) found, read, BCD-checked
 *   fwtest ntfs IMAGE          NTFS health
 *   fwtest fs IMAGE [MIB]      sniff + the matching health check (ext4, HFS+, APFS, FAT32, NTFS); reads
 *                              stay inside the first MIB MiB (default 8) of the image
 *   fwtest selftest            synthetic ext4 / HFS+ / APFS images in memory (clean, dirty, corrupt,
 *                              truncated, fuzzed): checks the findings and the problem counts
 * Build and run: sh tools/menu/fwtest/run.sh (cc -Wall -Wextra -Werror, ASan + UBSan).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sbfwfs.h"

static FILE *F;
static uint64_t rd_cap = ~0ULL;
static int rd(void *u, uint64_t off, void *buf, uint32_t len)
{
    (void)u;
    if (off > rd_cap || len > rd_cap - off)
        return -1;
    if (fseeko(F, (off_t)off, SEEK_SET))
        return -1;
    return fread(buf, 1, len, F) == len ? 0 : -1;
}
static void line(void *u, const char *s) { (void)u; printf("  %s\n", s); }

int selftest(void);

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "selftest"))
        return selftest();
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
    } else if (!strcmp(argv[1], "fs")) {
        rd_cap = (uint64_t)(argc > 3 ? atoi(argv[3]) : 8) << 20;
        fs_health(&io, &o);
    }
    printf("  problems: %d\n", o.bad);
    return 0;
}

/* ================================================================================================
 * selftest: synthetic images in memory
 * ============================================================================================== */

struct mem { uint8_t *p; uint64_t n; };
static int mrd(void *u, uint64_t off, void *buf, uint32_t len)
{
    struct mem *m = u;
    if (off > m->n || len > m->n - off)
        return -1;
    memcpy(buf, m->p + off, len);
    return 0;
}
static char outbuf[16384];
static int quiet;
static void mline(void *u, const char *s)
{
    (void)u;
    size_t k = strlen(outbuf);
    if (k + strlen(s) + 2 < sizeof(outbuf)) {
        strcat(outbuf, s);
        strcat(outbuf, "\n");
    }
}
static int fails, tests;

typedef void (*hfn)(const struct fw_io *, struct fw_out *);
static int run(const char *name, hfn f, uint8_t *img, uint64_t n, int want_bad, const char *want1, const char *want2)
{
    struct mem m = { img, n };
    struct fw_io io = { mrd, &m, n };
    struct fw_out o = { mline, 0, 0 };
    outbuf[0] = 0;
    f(&io, &o);
    int ok = o.bad == want_bad && (!want1 || strstr(outbuf, want1)) && (!want2 || strstr(outbuf, want2));
    tests++;
    if (!ok)
        fails++;
    if (!quiet || !ok) {
        printf("%s %s (problems %d, want %d)\n", ok ? "PASS" : "FAIL", name, o.bad, want_bad);
        if (!ok || getenv("FWTEST_VERBOSE")) {
            for (char *p = outbuf, *q; *p; p = q + 1) {
                q = strchr(p, '\n');
                printf("    %.*s\n", (int)(q - p), p);
            }
        }
    }
    return ok;
}
static void fsh(const struct fw_io *io, struct fw_out *o) { fs_health(io, o); }

static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static void b16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void b32(uint8_t *p, uint32_t v) { b16(p, v >> 16); b16(p + 2, v); }

/* reference CRC-32C (table-driven, the standard final inversion): "123456789" -> 0xE3069283 */
static uint32_t crc32c_ref(const uint8_t *p, size_t n)
{
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? (c >> 1) ^ 0x82F63B78U : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFU;
    while (n--)
        c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
/* reference Fletcher-64 as in the APFS reference: over the object minus its first 8 bytes */
static uint64_t fletcher_ref(const uint8_t *b, size_t n)
{
    uint64_t lo = 0, hi = 0, mod = 0xFFFFFFFFULL;
    for (size_t i = 8; i < n; i += 4) {
        lo += (uint32_t)(b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24);
        hi += lo;
        lo %= mod;
        hi %= mod;
    }
    uint64_t c1 = mod - ((lo + hi) % mod), c2 = mod - ((lo + c1) % mod);
    return c2 << 32 | c1;
}

#define MB (1u << 20)
static uint8_t img[MB];

static void ext4_make(int csum, int dirty)
{
    memset(img, 0, MB);
    uint8_t *s = img + 1024;
    w32(s + 0x00, 256);           /* inodes */
    w32(s + 0x04, 1024);          /* blocks (1 KiB) */
    w32(s + 0x0C, 900);
    w32(s + 0x10, 200);
    w32(s + 0x18, 0);             /* 1 KiB blocks */
    w32(s + 0x2C, 1762905600);    /* 2025-11-12 00:00 UTC */
    w32(s + 0x30, 1762909200);
    w16(s + 0x34, 3);
    w16(s + 0x36, 0xFFFF);
    w16(s + 0x38, 0xEF53);
    w16(s + 0x3A, 1);
    w16(s + 0x3C, 2);
    w32(s + 0x40, 1700000000);
    w32(s + 0x4C, 1);
    w32(s + 0x5C, 4);             /* has_journal */
    w32(s + 0x60, 0x40 | 0x80);   /* extents, 64bit */
    for (int i = 0; i < 16; i++)
        s[0x68 + i] = (uint8_t)(0x10 + i);
    memcpy(s + 0x78, "testroot", 8);
    if (dirty) {
        w16(s + 0x3A, 2);
        w32(s + 0x60, 0x40 | 0x80 | 4);
        w32(s + 0x194, 5);
        w32(s + 0x198, 1762900000);
        w32(s + 0x19C, 12);
        memcpy(s + 0x1A8, "ext4_lookup", 11);
        w32(s + 0x1C8, 1789);
        w32(s + 0x1CC, 1762901000);
        memcpy(s + 0x1E0, "ext4_find_entry", 15);
        w32(s + 0x1D4, 1600);
    }
    if (csum) {
        w32(s + 0x64, 0x400);
        s[0x175] = 1;
        w32(s + 0x3FC, ~crc32c_ref(s, 1020));     /* ext4 stores the CRC without the final inversion */
    }
}

static void hfs_make(int sig, int attr)
{
    memset(img, 0, MB);
    for (int k = 0; k < 2; k++) {
        uint8_t *h = img + (k ? MB - 1024 : 1024);
        b16(h, sig);
        b16(h + 2, sig == 0x4858 ? 5 : 4);
        b32(h + 4, (uint32_t)attr);
        b32(h + 8, 0x4846534A);           /* HFSJ */
        b32(h + 12, 3);
        b32(h + 16, 3845000000U);
        b32(h + 20, 3845700000U);
        b32(h + 28, 3845600000U);
        b32(h + 32, 1234);
        b32(h + 36, 56);
        b32(h + 40, 4096);
        b32(h + 44, 256);
        b32(h + 48, 100);
    }
}

#define APFS_N 16
static void apfs_seal(uint8_t *b) { w64(b, fletcher_ref(b, 4096)); }
static void apfs_make(void)
{
    memset(img, 0, MB);
    uint8_t *b = img;
    w64(b + 8, 1);
    w64(b + 16, 10);
    w32(b + 24, 0x80000001U);
    memcpy(b + 32, "NXSB", 4);
    w32(b + 36, 4096);
    w64(b + 40, APFS_N);
    for (int i = 0; i < 16; i++)
        b[72 + i] = (uint8_t)(0xA0 + i);
    w64(b + 88, 0x500);
    w64(b + 96, 11);
    w32(b + 104, 4);
    w32(b + 108, 4);
    w64(b + 112, 1);
    w64(b + 120, 5);
    w32(b + 128, 1);
    w32(b + 132, 1);
    w32(b + 136, 0);
    w32(b + 140, 1);
    w32(b + 144, 0);
    w32(b + 148, 1);
    w64(b + 152, 0x400);
    w64(b + 160, 0x401);
    w64(b + 168, 0x402);
    w32(b + 180, 1);
    w64(b + 184, 0x403);
    apfs_seal(b);
    memcpy(img + 4096, b, 4096);       /* descriptor block 0 = the same superblock */
}

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

int selftest(void)
{
    quiet = 0;
    /* CRC-32C reference vector */
    tests++;
    if (crc32c_ref((const uint8_t *)"123456789", 9) != 0xE3069283U) {
        printf("FAIL crc32c reference vector\n");
        fails++;
    }

    ext4_make(0, 0);
    run("ext4 clean, no metadata_csum", ext4_health, img, MB, 0, "ext4: state clean", "label 'testroot'");
    run("ext4 clean, label/UUID/time", ext4_health, img, MB, 0, "UUID 10111213-1415-1617-1819-1a1b1c1d1e1f",
        "last mounted 2025-11-12 00:00 UTC");
    ext4_make(1, 0);
    run("ext4 clean, metadata_csum OK", ext4_health, img, MB, 0, "superblock checksum OK", "metadata_csum");
    img[1024 + 0x78] ^= 1;
    run("ext4 metadata_csum, corrupted byte", ext4_health, img, MB, 1, "checksum WRONG", 0);
    ext4_make(1, 1);
    run("ext4 errors + needs_recovery (csum OK)", ext4_health, img, MB, 3, "first error 2025-11-11",
        "in ext4_lookup:1789, inode 12");
    run("ext4 errors: last error, journal", ext4_health, img, MB, 3, "last error", "needs_recovery: the journal");
    ext4_make(0, 1);
    run("ext4 errors, no csum", ext4_health, img, MB, 3, "5 error(s) recorded", "state ERRORS");
    ext4_make(0, 0);
    img[1024 + 0x38] = 0;
    run("ext4 bad magic", ext4_health, img, MB, 1, "no superblock magic", 0);
    ext4_make(0, 0);
    run("ext4 truncated (1500 bytes)", ext4_health, img, 1500, 1, "could not be read", 0);
    ext4_make(0, 0);
    w32(img + 1024 + 0x18, 40);
    run("ext4 absurd block size", ext4_health, img, MB, 1, "block size out of range", 0);
    ext4_make(0, 0);
    w32(img + 1024 + 0x04, 0xFFFFFFF0U);
    run("ext4 volume larger than partition", ext4_health, img, MB, 1, "larger than its partition", 0);
    ext4_make(0, 0);
    w16(img + 1024 + 0x36, 3);
    run("ext4 fsck due by mount count", ext4_health, img, MB, 0, "(fsck due)", 0);

    hfs_make(0x482B, 1 << 8 | 1 << 13);
    run("HFS+ clean, journaled", hfs_health, img, MB, 0, "HFS+: unmounted cleanly", "alternate volume header present");
    run("HFS+ counts and dates", hfs_health, img, MB, 0, "1234 files, 56 folders", "'HFSJ' (macOS, journaled)");
    hfs_make(0x4858, 1 << 8);
    run("HFSX clean, not journaled", hfs_health, img, MB, 0, "HFSX (case-sensitive HFS+): version 5", "not journaled");
    hfs_make(0x482B, 1 << 13 | 1 << 11);
    run("HFS+ dirty + inconsistent", hfs_health, img, MB, 2, "NOT unmounted cleanly", "inconsistent bit");
    hfs_make(0x482B, 1 << 8);
    b32(img + 1024 + 40, 3000);
    run("HFS+ bad block size", hfs_health, img, MB, 1, "block size out of range", 0);
    hfs_make(0x482B, 1 << 8);
    b32(img + 1024 + 48, 999);
    run("HFS+ free > total", hfs_health, img, MB, 1, "more free blocks", 0);
    hfs_make(0x482B, 1 << 8);
    run("HFS+ truncated (1200 bytes)", hfs_health, img, 1200, 1, "could not be read", 0);
    memset(img, 0, MB);
    run("HFS+ no signature", hfs_health, img, MB, 1, "no volume header signature", 0);

    apfs_make();
    run("APFS block 0 checksum OK", apfs_health, img, MB, 0, "block 0 checksum OK", "1 volume(s), at most 1");
    run("APFS checkpoint", apfs_health, img, MB, 0, "checkpoint layout consistent", "xid 10 at descriptor block 0, checksum OK (= block 0)");
    apfs_make();
    w64(img + 4096 + 16, 12);
    apfs_seal(img + 4096);
    run("APFS newer checkpoint in the descriptor area", apfs_health, img, MB, 0, "xid 12", "(newer than block 0)");
    apfs_make();
    img[1500] ^= 0x40;            /* past the header fields: only the checksum notices */
    run("APFS broken checksum", apfs_health, img, MB, 1, "block 0 checksum WRONG", 0);
    apfs_make();
    w64(img + 112, 100);
    apfs_seal(img);
    run("APFS descriptor area outside the container", apfs_health, img, MB, 1, "outside the container", "INCONSISTENT");
    apfs_make();
    img[33] = 'Y';
    run("APFS bad magic", apfs_health, img, MB, 1, "no container superblock magic", 0);
    apfs_make();
    run("APFS truncated (3000 bytes)", apfs_health, img, 3000, 2, "could not be read whole", "larger than its partition");
    apfs_make();
    w32(img + 36, 5000);
    run("APFS bad block size", apfs_health, img, MB, 1, "block size out of range", 0);

    /* the dispatcher */
    ext4_make(1, 0);
    run("fs_health: ext4", fsh, img, MB, 0, "file system: ext2/3/4", "checksum OK");
    hfs_make(0x4858, 1 << 8 | 1 << 13);
    run("fs_health: HFSX", fsh, img, MB, 0, "file system: HFS+ / HFSX", 0);
    apfs_make();
    run("fs_health: APFS", fsh, img, MB, 0, "file system: APFS", "checksum OK");
    memset(img, 0, MB);
    run("fs_health: zeros", fsh, img, MB, 0, "file system: unknown", 0);
    run("fs_health: tiny (100 bytes)", fsh, img, 100, 0, "file system: unknown", 0);
    memcpy(img + 3, "NTFS    ", 8);
    run("fs_health: NTFS signature only", fsh, img, MB, 1, "file system: NTFS", "NTFS:");
    memset(img, 0, MB);
    memcpy(img + 82, "FAT32   ", 8);
    img[510] = 0x55;
    img[511] = 0xAA;
    run("fs_health: FAT32 signature, bad BPB", fsh, img, MB, 1, "file system: FAT32", "out of range");

    /* fuzz: random headers with the magic kept, random sizes; must end and stay in bounds (ASan) */
    quiet = 1;
    int before = fails;
    for (int it = 0; it < 3000; it++) {
        int kind = it % 3;
        if (kind == 0)
            ext4_make(it & 1, it & 2);
        else if (kind == 1)
            hfs_make(it & 4 ? 0x4858 : 0x482B, 1 << 8);
        else
            apfs_make();
        int flips = 1 + (int)(rnd() % 64);
        for (int k = 0; k < flips; k++) {
            uint32_t at = kind == 2 ? rnd() % 1408 : 1024 + rnd() % 1024;
            if ((kind == 2 && at >= 32 && at < 36) || (kind == 0 && (at == 1080 || at == 1081)) ||
                (kind == 1 && at < 1026 && at >= 1024))
                continue;
            img[at] = (uint8_t)rnd();
        }
        uint64_t n = it % 7 == 0 ? rnd() % MB : MB;
        struct mem m = { img, n };
        struct fw_io io = { mrd, &m, n };
        struct fw_out o = { mline, 0, 0 };
        outbuf[0] = 0;
        fs_health(&io, &o);
    }
    printf("%s fuzz: 3000 corrupted / truncated images, all checks ended\n", fails == before ? "PASS" : "FAIL");
    printf("selftest: %d checks, %d failed\n", tests, fails);
    return fails ? 1 : 0;
}
