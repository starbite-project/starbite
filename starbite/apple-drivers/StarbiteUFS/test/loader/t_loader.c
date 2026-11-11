/* SPDX-License-Identifier: APSL-2.0 */
/*
 * End to end, on the host: the loader's shim/starlte/ufs_fence.c (with ufs.c's real GPT reader) fills the ADT
 * placeholders from a GPT image, and StarbiteUFS's own parser (hw/sbufs_fence.c) reads what it wrote.
 *   t_loader FIXTURE_DIR          (fixtures: mkfixtures.py)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_loader.h"
#include "ufs_fence.h"
#include "sbufs_fence.h"

uint8_t host_ufs_dma[0x4000 + (1U << 20)] = { 0 };

static unsigned checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- fakes for ufs.c / store.c ---- */
static uint8_t *img;
static size_t imglen;
static int fail_reads, home = (int)STORE_SD, card_only;
static unsigned nreads, nups;

int ufs_up(void)
{
    nups++;
    return img != NULL;
}

int ufs_read(uint64_t lba, uint32_t count)
{
    nreads++;
    if (fail_reads || !img || count > 256 || (lba + count) * 4096U > imglen)
        return -1;
    memcpy((void *)UFS_BUF, img + lba * 4096U, count * 4096U);
    return 0;
}

int store_home(void) { return home; }
int store_card_only(void) { return card_only; }

static uint8_t *slurp(const char *dir, const char *name, size_t *len)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len ? *len : 1);
    if (fread(b, 1, *len, f) != *len) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    return b;
}

static const char *dir;
static uint64_t x_first, x_blocks, x_total;
static int x_parts;
static uint8_t x_uuid[16];

static void use_gpt(const char *name)
{
    free(img);
    img = slurp(dir, name, &imglen);
    gpt_n = -1;                               /* ufs.c caches the table per boot */
    nreads = nups = 0;
}

/* run the hook on a copy of ADT file adt: result, and the driver's parse of what it wrote */
static int hook(const char *adt, struct sbufs_fence *f, int *parsed, uint8_t **copy, size_t *len)
{
    *copy = slurp(dir, adt, len);
    int r = ufs_fence_xnu((uintptr_t)*copy, *len, 0x1000, 0x2000);
    uint32_t ao, al, dof, dl;
    *parsed = -99;
    if (r >= 0 && ufs_fence_adt_find(*copy, (uint32_t)*len, &ao, &al, &dof, &dl)) {
        char err[160];
        *parsed = sbufs_fence_parse(f, *copy + ao, al, *copy + dof, dl, err, sizeof(err));
    }
    return r;
}

static int deny_named(const struct sbufs_fence *f, const char *n)
{
    for (unsigned i = 0; i < f->ndeny; i++)
        if (strcmp(f->deny[i].name, n) == 0)
            return 1;
    return 0;
}

static void full_check(const char *adt, int whole)
{
    struct sbufs_fence f;
    int parsed;
    uint8_t *a;
    size_t len;
    use_gpt("gpt-oneui.img");
    int r = hook(adt, &f, &parsed, &a, &len);
    CHECK(r == 1);
    CHECK(parsed == SBUFS_OK);
    CHECK(f.nallow == 1);
    uint64_t want = whole ? x_blocks : UFS_FENCE_STORE_OFFSET;
    CHECK(f.allow[0].lun == 0 && f.allow[0].first == x_first && f.allow[0].count == want);
    CHECK(f.allow[0].flags == SBUFS_RANGE_ROOT);          /* never WRITABLE */
    CHECK(strcmp(f.allow[0].name, "USERDATA") == 0);
    CHECK(memcmp(f.allow[0].uuid, x_uuid, 16) == 0);
    CHECK((int)f.ndeny == x_parts - 1 + 2 + (whole ? 0 : 1));
    static const char *never[] = { "EFS", "PARAM", "UP_PARAM", "KEYSTORAGE", "PERSISTENT", "STEADY", "BOTA0",
                                   "BOTA1", "BOTA2", "DTB", "DTBO", "BOOT", "RECOVERY", "RADIO", "SYSTEM", "WINESP",
                                   "WINDOWS", "A16DATA", "VENDOR", "ODM", "GPT-HEAD", "GPT-TAIL" };
    for (unsigned i = 0; i < sizeof(never) / sizeof(never[0]); i++)
        CHECK(deny_named(&f, never[i]));
    CHECK(whole || deny_named(&f, "SBSTORE-UFS"));
    sbufs_fence_set_capacity(&f, 0, x_total);
    CHECK(sbufs_fence_seal(&f) == 1);
    for (unsigned i = 0; i < f.ndeny; i++) {
        uint64_t lo = f.deny[i].first, hi = f.deny[i].first + f.deny[i].count - 1;
        CHECK(sbufs_fence_check(&f, 0, lo, 1, false) != SBUFS_FENCE_OK);
        CHECK(sbufs_fence_check(&f, 0, hi, 1, false) != SBUFS_FENCE_OK);
        CHECK(sbufs_fence_check(&f, 0, lo + (hi - lo) / 2, 1, false) != SBUFS_FENCE_OK);
    }
    for (uint64_t b = 0; b < 6; b++)
        CHECK(sbufs_fence_check(&f, 0, b, 1, false) != SBUFS_FENCE_OK);
    CHECK(sbufs_fence_check(&f, 0, x_first, 1, false) == SBUFS_FENCE_OK);
    CHECK(sbufs_fence_check(&f, 0, x_first + want - 1, 1, false) == SBUFS_FENCE_OK);
    CHECK(sbufs_fence_check(&f, 0, x_first, want, false) == SBUFS_FENCE_OK);
    CHECK(sbufs_fence_check(&f, 0, x_first - 1, 1, false) != SBUFS_FENCE_OK);
    CHECK(sbufs_fence_check(&f, 0, x_first + want, 1, false) != SBUFS_FENCE_OK);
    f.writes_enabled = true;                                 /* even with the switch on: the range is not WRITABLE */
    CHECK(sbufs_fence_check(&f, 0, x_first, 1, true) == SBUFS_FENCE_READONLY);
    printf("  %s, %s: USERDATA %llu +%llu, %u denied extents: driver parse ok, 1 usable range\n", adt,
           whole ? "card store home" : "UFS store home", (unsigned long long)f.allow[0].first,
           (unsigned long long)f.allow[0].count, f.ndeny);
    free(a);
}

static void closed(const char *gpt, const char *what)
{
    struct sbufs_fence f;
    int parsed;
    uint8_t *a;
    size_t len;
    use_gpt(gpt);
    int r = hook("adt-ph.bin", &f, &parsed, &a, &len);
    CHECK(r == 0);
    CHECK(parsed == SBUFS_OK && f.nallow == 0 && f.ndeny == 0);
    printf("  %-24s -> empty allow-list (%s): driver exposes nothing\n", what, ufs_fence_why ? ufs_fence_why : "?");
    free(a);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    dir = argv[1];
    char hex[33];
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/expect.txt", dir);
        FILE *x = fopen(p, "r");
        if (!x || fscanf(x, "%llu %llu %32s %llu %d", (unsigned long long *)&x_first, (unsigned long long *)&x_blocks,
                         hex, (unsigned long long *)&x_total, &x_parts) != 5)
            return 2;
        fclose(x);
        for (int i = 0; i < 16; i++) {
            unsigned v;
            sscanf(hex + 2 * i, "%2x", &v);
            x_uuid[i] = (uint8_t)v;
        }
    }

    /* the S9 #1 table, card store home (all of USERDATA is iOS's), and with the UFS store home */
    home = (int)STORE_SD;
    full_check("adt-ph.bin", 1);
    home = (int)STORE_UFS;
    full_check("adt-ph.bin", 0);
    card_only = 1;
    full_check("adt-ph.bin", 1);
    card_only = 0;
    home = (int)STORE_SD;
    size_t blen;
    uint8_t *board = slurp(dir, "adt-board.bin", &blen);
    if (board) {
        full_check("adt-board.bin", 1);                      /* the real mkadt-board.py output */
        free(board);
    } else {
        printf("  (no out/starlte-iphone11-1792.adt here: the real board ADT case skipped)\n");
    }

    /* a payload from before the fence: byte for byte untouched, UFS never touched */
    {
        size_t len, len0;
        use_gpt("gpt-oneui.img");
        uint8_t *a = slurp(dir, "adt-none.bin", &len), *a0 = slurp(dir, "adt-none.bin", &len0);
        CHECK(ufs_fence_xnu((uintptr_t)a, len, 0x1000, 0x2000) == -1);
        CHECK(len == len0 && memcmp(a, a0, len) == 0);
        CHECK(nreads == 0 && nups == 0);
        printf("  adt-none.bin: untouched, %u UFS reads\n", nreads);
        free(a);
        free(a0);
    }

    /* fail closed */
    use_gpt("gpt-oneui.img");
    fail_reads = 1;
    closed("gpt-oneui.img", "UFS reads fail");
    fail_reads = 0;
    closed("gpt-badsig.img", "bad GPT signature");
    closed("gpt-nouserdata.img", "no USERDATA");
    closed("gpt-twouserdata.img", "two USERDATA");
    closed("gpt-overlap.img", "overlapping partitions");
    closed("gpt-pastusable.img", "past the usable range");
    closed("gpt-many.img", "50 partitions");
    free(img);
    img = NULL;
    closed("gpt-none", "no UFS host");
    {
        struct sbufs_fence f;
        size_t len;
        char err[160];
        uint32_t ao, al, dof, dl;
        use_gpt("gpt-oneui.img");
        uint8_t *a = slurp(dir, "adt-ph.bin", &len);
        /* everything the loader placed for XNU overlapping UFS's DMA buffers: no UFS access, empty */
        int r = ufs_fence_xnu((uintptr_t)a, len, UFS_DMA - 0x1000, UFS_DMA + 0x10);
        CHECK(r == 0 && nreads == 0 && nups == 0);
        CHECK(ufs_fence_adt_find(a, (uint32_t)len, &ao, &al, &dof, &dl));
        CHECK(sbufs_fence_parse(&f, a + ao, al, a + dof, dl, err, sizeof(err)) == SBUFS_OK && f.nallow == 0);
        printf("  DMA overlap              -> empty allow-list (%s), no UFS access\n", ufs_fence_why);
        free(a);
    }

    /* malformed ADTs: every truncation, then random corruption: never a crash; a fill is always parseable */
    {
        size_t len;
        uint8_t *a = slurp(dir, "adt-ph.bin", &len);
        uint8_t *c = malloc(len);
        use_gpt("gpt-oneui.img");
        unsigned truncated_found = 0, filled = 0, untouched = 0;
        for (size_t n = 0; n < len; n++) {
            uint32_t ao, al, dof, dl;
            memcpy(c, a, n);
            truncated_found += ufs_fence_adt_find(c, (uint32_t)n, &ao, &al, &dof, &dl);
        }
        CHECK(truncated_found == 0);
        uint64_t rng = 99;
        for (int it = 0; it < 30000; it++) {
            memcpy(c, a, len);
            for (int k = 0; k < 1 + it % 4; k++) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                c[rng % len] ^= (uint8_t)(1u << (rng >> 40) % 8);
            }
            int r = ufs_fence_xnu((uintptr_t)c, len, 0x1000, 0x2000);
            uint32_t ao, al, dof, dl;
            if (r < 0) {
                untouched++;
                continue;
            }
            filled++;
            struct sbufs_fence f;
            char err[160];
            CHECK(ufs_fence_adt_find(c, (uint32_t)len, &ao, &al, &dof, &dl));
            CHECK(sbufs_fence_parse(&f, c + ao, al, c + dof, dl, err, sizeof(err)) == SBUFS_OK);
        }
        /* a placeholder flagged the iBoot way (bit 31: XNU would not mask it) is not ours */
        memcpy(c, a, len);
        for (size_t i = 0; i + 32 < len; i++) {
            if (memcmp(c + i, "starbite,denied-ranges", 23) == 0) {
                c[i + 35] |= 0x80;
                break;
            }
        }
        CHECK(ufs_fence_xnu((uintptr_t)c, len, 0x1000, 0x2000) == -1);
        printf("  malformed ADTs: %zu truncations found nothing; 30000 corrupted: %u filled (all parse), %u untouched\n",
               len, filled, untouched);
        free(a);
        free(c);
    }
    printf("StarbiteUFS loader test: %u checks, %u failed: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails != 0;
}
