/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of libsb's self-relocation (starbite/libsb/reloc.c), run in the aarch64 Lima VM by
 * starbite/engineer/build.sh:
 *
 *   reloc-test engineer.img engineer.elf engineer-at40.bin engineer-at40.elf
 *
 * 1. Maps engineer.img (linked at 0) at 0x40000000 and runs the image's own sb_reloc_check on it,
 *    exactly as crt0.S does on the phone at 0x92080000.
 * 2. Compares the result, byte for byte, with the same objects linked at 0x40000000 (with that
 *    link's RELATIVE entries applied in place): only the header fields build.sh patches, the table
 *    itself (.rela.dyn) and the unused .dynamic / .dynsym may differ.
 * 3. Calls sb_selftest() inside the relocated mapping: it walks tables of function and string
 *    pointers that only work if every RELATIVE entry was applied (expected 141663).
 * 4. Relocates a second copy at a random 4 KiB-aligned address and checks every relocated word
 *    moved by exactly the load delta.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

struct sb_rela {
    uint64_t off, info;
    int64_t addend;
};
int64_t sb_reloc_check(uint64_t base, const struct sb_rela *r, const struct sb_rela *end, uint64_t limit);

static uint8_t *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) {
        perror(p);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) {
        perror(p);
        exit(2);
    }
    fclose(f);
    return b;
}

struct range {
    uint64_t a, n;
};

/* Section address and size by name, and a symbol's value. */
static struct range section(const uint8_t *elf, const char *name)
{
    const Elf64_Ehdr *eh = (const void *)elf;
    const Elf64_Shdr *sh = (const void *)(elf + eh->e_shoff);
    const char *str = (const char *)elf + sh[eh->e_shstrndx].sh_offset;
    for (int i = 0; i < eh->e_shnum; ++i)
        if (!strcmp(str + sh[i].sh_name, name))
            return (struct range){ sh[i].sh_addr, sh[i].sh_size };
    return (struct range){ 0, 0 };
}

static uint64_t symbol(const uint8_t *elf, const char *name)
{
    const Elf64_Ehdr *eh = (const void *)elf;
    const Elf64_Shdr *sh = (const void *)(elf + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != SHT_SYMTAB)
            continue;
        const Elf64_Sym *s = (const void *)(elf + sh[i].sh_offset);
        const char *str = (const char *)elf + sh[sh[i].sh_link].sh_offset;
        for (uint64_t k = 0; k < sh[i].sh_size / sizeof(*s); ++k)
            if (!strcmp(str + s[k].st_name, name))
                return s[k].st_value;
    }
    fprintf(stderr, "no symbol %s\n", name);
    exit(2);
}

static int in(struct range r, uint64_t a, uint64_t base)
{
    return r.n && a >= r.a - base && a < r.a - base + r.n;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: reloc-test engineer.img engineer.elf engineer-at40.bin engineer-at40.elf\n");
        return 2;
    }
    size_t na, nea, nb, neb;
    uint8_t *img = slurp(argv[1], &na), *ea = slurp(argv[2], &nea);
    uint8_t *bin40 = slurp(argv[3], &nb), *eb = slurp(argv[4], &neb);
    const uint64_t B = 0x40000000ULL;
    uint64_t rs = symbol(ea, "__rela_start"), re = symbol(ea, "__rela_end");
    uint64_t rs40 = symbol(eb, "__rela_start") - B, re40 = symbol(eb, "__rela_end") - B;
    uint64_t st = symbol(ea, "sb_selftest");
    int fails = 0;

    /* 1: relocate at 0x40000000 */
    uint8_t *m = mmap((void *)B, na, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)B) {
        perror("mmap 0x40000000");
        return 2;
    }
    memcpy(m, img, na);
    int64_t n = sb_reloc_check(B, (const void *)(B + rs), (const void *)(B + re), na);
    printf("relocated at %#llx: %lld R_AARCH64_RELATIVE entries\n", (unsigned long long)B, (long long)n);
    if (n <= 0) {
        printf("FAIL: sb_reloc_check returned %lld\n", (long long)n);
        return 1;
    }

    /* 2: compare with the link at 0x40000000, its own entries applied */
    uint8_t *ref = calloc(1, na);
    memcpy(ref, bin40, nb < na ? nb : na);
    if (re40 - rs40 != re - rs) {
        printf("FAIL: the two links have different .rela.dyn sizes\n");
        return 1;
    }
    for (uint64_t r = rs40; r < re40; r += 24) {
        const struct sb_rela *e = (const void *)(ref + r);
        *(uint64_t *)(ref + (e->off - B)) = (uint64_t)e->addend;
    }
    struct range dyn = section(eb, ".dynamic"), dsym = section(eb, ".dynsym"), rela = section(eb, ".rela.dyn");
    /* GOT[0] holds the link-time address of _DYNAMIC (for a dynamic loader; nothing reads it) */
    struct range got0 = { symbol(eb, "_GLOBAL_OFFSET_TABLE_"), 8 };
    uint64_t diff = 0, odd = 0;
    for (uint64_t i = 0; i < na; ++i) {
        if (m[i] == ref[i])
            continue;
        ++diff;
        int hdr = (i >= 16 && i < 24) || (i >= 88 && i < 96);      /* image_size, crc (build.sh) */
        if (!hdr && !in(dyn, i, B) && !in(dsym, i, B) && !in(rela, i, B) && !in(got0, i, B)) {
            if (odd < 8)
                printf("  differs at +%#llx: %02x vs %02x\n", (unsigned long long)i, m[i], ref[i]);
            ++odd;
        }
    }
    printf("compare with the 0x40000000 link: %llu bytes differ, %llu outside header/.dynamic/.dynsym/.rela.dyn\n",
           (unsigned long long)diff, (unsigned long long)odd);
    if (odd)
        ++fails;

    /* 3: call through the relocated function-pointer tables */
    __builtin___clear_cache((char *)m, (char *)m + na);
    int64_t (*fn)(void) = (int64_t (*)(void))(B + st);
    int64_t v = fn();
    printf("sb_selftest() in the relocated image: %lld (want 141663)\n", (long long)v);
    if (v != 141663)
        ++fails;

    /* 4: a random base: every relocated word moves by exactly the delta */
    srand((unsigned)time(0));
    uint64_t R = 0x100000000ULL + ((uint64_t)(rand() & 0xffff) << 16);
    uint8_t *m2 = mmap((void *)R, na, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m2 != (void *)R) {
        perror("mmap random");
        return 2;
    }
    memcpy(m2, img, na);
    int64_t n2 = sb_reloc_check(R, (const void *)(R + rs), (const void *)(R + re), na);
    uint64_t bad = 0;
    for (uint64_t r = rs; r < re; r += 24) {
        const struct sb_rela *e = (const void *)(img + r);
        uint64_t got = *(uint64_t *)(m2 + e->off), want = R + (uint64_t)e->addend;
        uint64_t at_b = *(uint64_t *)(m + e->off);
        if (got != want || got - at_b != R - B)
            ++bad;
    }
    printf("relocated at %#llx: %lld entries, %llu wrong\n", (unsigned long long)R, (long long)n2,
           (unsigned long long)bad);
    if (bad || n2 != n)
        ++fails;

    /* a corrupted entry must be refused, not applied */
    memcpy(m2, img, na);
    struct sb_rela *e0 = (struct sb_rela *)(m2 + rs);
    e0[1].info = 257;                                   /* R_AARCH64_ABS64 */
    int64_t n3 = sb_reloc_check(R, (const void *)(R + rs), (const void *)(R + re), na);
    printf("non-RELATIVE entry 1: sb_reloc_check -> %lld (want -2)\n", (long long)n3);
    if (n3 != -2)
        ++fails;

    printf(fails ? "RELOC TEST FAILED\n" : "RELOC TEST PASSED\n");
    return fails ? 1 : 0;
}
