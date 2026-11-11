/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of the card path of sbblk (starbite/guest/blk/card-test.sh builds and runs it):
 *   - libsb's REAL fence.c with guest-card.patch: the card-file grant and sb_write_card,
 *   - the REAL resolver of starbite/guest/sbos/guest-card.h over the REAL exFAT reader (starbite/utility/exfat.c),
 * on a fake microSD card (card-test-image.py: an MBR, p1 exFAT with files in every shape, p2 = the OS store's
 * partition) and a fake UFS that only counts calls. In the style of starbite/tools/unit-test.c.
 *   card-test CARD.img LAYOUT.h UNALIGNED.img UNALIGNED-LAYOUT.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "ufs.h"
#include "sd.h"
#define GCARD_HOST_TEST
#include "guest-card.h"

static int fails, checks;
#define CHECK(cond, ...)                                                                          \
    do {                                                                                          \
        ++checks;                                                                                 \
        if (!(cond)) {                                                                            \
            ++fails;                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                           \
            printf(__VA_ARGS__);                                                                  \
            printf("\n");                                                                         \
        }                                                                                         \
    } while (0)

/* ---- the fake media ------------------------------------------------------------------------------- */
static uint8_t *card, *card0;               /* the card and its pristine copy */
static uint64_t card_sectors;
static int ufs_calls, sd_writes, sd_unfenced, sd_reads;
struct sd_report sd_rep;
static char last_log[256];
static int logs;

uint64_t sb_ticks(void) { return 1; }
void sb_log(const char *s) { snprintf(last_log, sizeof(last_log), "%s", s); ++logs; }
int sd_read(uint64_t sector, uint32_t count, uintptr_t dst)
{
    ++sd_reads;
    if (sector + count > card_sectors)
        return -1;
    memcpy((void *)dst, card + sector * 512, (size_t)count * 512);
    return 0;
}
static int rd(uint64_t sector, uint32_t count, void *dst) { return sd_read(sector, count, (uintptr_t)dst); }
/* the fence's block writer (blkio.c on the phone): the card driver refuses outside the fence, UFS is counted */
int sb_blk_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    (void)lu;
    if (medium != SB_MED_SD) {
        ++ufs_calls;
        return 0;
    }
    if (!sb_fence_active()) {
        ++sd_unfenced;
        return -9;
    }
    if ((lba + n) * 8 > card_sectors)
        return -1;
    ++sd_writes;
    memcpy(card + lba * 4096, src, (size_t)n * 4096);
    return 0;
}
int sb_blk_write_sync(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    (void)medium, (void)lu, (void)lba, (void)n, (void)src;
    ++ufs_calls;
    return 0;
}
/* ufs.h's GPT (the fence's UFS classification; not used by the card path) */
int gpt_n;
char gpt_name[GPT_MAX][24];
uint64_t gpt_first[GPT_MAX], gpt_last[GPT_MAX];
int gpt_load(void) { ++ufs_calls; return 0; }

/* ---- the layout the image generator printed --------------------------------------------------------- */
static struct { char k[32]; uint64_t v; } lay[64];
static int nlay;
static uint64_t L(const char *k)
{
    for (int i = 0; i < nlay; ++i)
        if (!strcmp(lay[i].k, k))
            return lay[i].v;
    printf("FAIL: no %s in the layout\n", k);
    exit(2);
}
static void load(const char *img, const char *layout)
{
    FILE *f = fopen(img, "rb");
    if (!f) { perror(img); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    free(card);
    free(card0);
    card = malloc((size_t)n);
    card0 = malloc((size_t)n);
    if (fread(card, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    memcpy(card0, card, (size_t)n);
    card_sectors = (uint64_t)n / 512;
    nlay = 0;
    f = fopen(layout, "r");
    if (!f) { perror(layout); exit(2); }
    unsigned long long v;
    while (nlay < 64 && fscanf(f, " #define %31s %lluULL", lay[nlay].k, &v) >= 1) {
        lay[nlay].v = v;
        if (!strncmp(lay[nlay].k, "T_", 2))
            ++nlay;
    }
    fclose(f);
}
static uint8_t pat(unsigned id, uint64_t i) { return (uint8_t)(id * 131 + i * 7 + (i >> 9)); }
/* bytes that differ from the pristine card outside [s0, s1) sectors */
static uint64_t changed_outside(uint64_t s0, uint64_t s1)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < card_sectors * 512; ++i)
        if (card[i] != card0[i] && (i < s0 * 512 || i >= s1 * 512))
            ++n;
    return n;
}

static struct exfat fs;
static uint8_t cbuf[1 << 17];
static uint64_t sec[64];
static uint64_t p_start, p_len;

static void mount_card(void)
{
    const char *no = gcard_partition(rd, (uint8_t *)sec, &p_start, &p_len);
    CHECK(!no && p_start == L("T_P1") && p_len == L("T_P1_LEN"), "the exFAT partition: %s", no ? no : "wrong range");
    CHECK(!exfat_mount(&fs, rd, p_start, cbuf, sizeof(cbuf)), "exfat_mount");
}

/* ---- the resolver: which files become devices ---------------------------------------------------------- */
static void test_resolve(void)
{
    uint64_t s, n;
    const char *no;
    mount_card();
    no = gcard_extent(&fs, p_start, p_len, GCARD_BASE, (uint8_t *)sec, &s, &n);
    CHECK(!no && s == L("T_BASE_SECTOR") && n == L("T_BASE_SIZE") / 4096 * 8, "base.erofs (NoFatChain): %s",
          no ? no : "wrong run");
    int bad = 0;
    for (uint64_t i = 0; i < n * 512; ++i)
        bad += card[s * 512 + i] != pat((unsigned)L("T_BASE_ID"), i);
    CHECK(!bad, "base.erofs: the run's bytes are the file's (%d differ)", bad);
    int r0 = sd_reads;
    no = gcard_extent(&fs, p_start, p_len, GCARD_REPO, (uint8_t *)sec, &s, &n);
    CHECK(!no && s == L("T_REPO_SECTOR") && n == L("T_REPO_SIZE") / 512, "repo.erofs (a FAT chain in one run): %s",
          no ? no : "wrong run");
    CHECK(sd_reads > r0, "repo.erofs: the FAT was read to verify the chain");
    bad = 0;
    for (uint64_t i = 0; i < n * 512; ++i)
        bad += card[s * 512 + i] != pat((unsigned)L("T_REPO_ID"), i);
    CHECK(!bad, "repo.erofs: the run's bytes are the file's (%d differ)", bad);
    no = gcard_extent(&fs, p_start, p_len, GCARD_OVERLAY, (uint8_t *)sec, &s, &n);
    CHECK(!no && s == L("T_OVERLAY_SECTOR") && n == L("T_OVERLAY_SIZE") / 512, "overlay.img: %s", no ? no : "wrong run");
    no = gcard_extent(&fs, p_start, p_len, GCARD_DIR "frag.img", (uint8_t *)sec, &s, &n);
    CHECK(no && strstr(no, "fragmented") && !s && !n, "frag.img must be refused as fragmented: %s", no ? no : "offered");
    no = gcard_extent(&fs, p_start, p_len, GCARD_DIR "long.img", (uint8_t *)sec, &s, &n);
    CHECK(no && strstr(no, "does not end"), "long.img (chain longer than the size) must be refused: %s", no ? no : "offered");
    no = gcard_extent(&fs, p_start, p_len, GCARD_DIR "tiny.img", (uint8_t *)sec, &s, &n);
    CHECK(no && strstr(no, "smaller"), "tiny.img must be refused: %s", no ? no : "offered");
    no = gcard_extent(&fs, p_start, p_len, GCARD_DIR "sub", (uint8_t *)sec, &s, &n);
    CHECK(no && strstr(no, "folder"), "a folder must be refused: %s", no ? no : "offered");
    no = gcard_extent(&fs, p_start, p_len, GCARD_DIR "missing.img", (uint8_t *)sec, &s, &n);
    CHECK(no && strstr(no, "not on the card"), "a missing file must be refused: %s", no ? no : "offered");
    /* a repo chain broken on the card (one link changed): refused; a partition shorter than the file: refused */
    uint64_t c = (L("T_REPO_SECTOR") - L("T_HEAP")) / 64 + 2;
    uint8_t *link = card + L("T_FAT") * 512 + c * 4;
    uint8_t keep = link[0];
    link[0] = (uint8_t)(keep + 5);
    no = gcard_extent(&fs, p_start, p_len, GCARD_REPO, (uint8_t *)sec, &s, &n);
    CHECK(no && strstr(no, "fragmented"), "repo.erofs with a changed FAT link must be refused: %s", no ? no : "offered");
    link[0] = keep;
    no = gcard_extent(&fs, p_start, L("T_OVERLAY_SECTOR") - p_start + 8, GCARD_OVERLAY, (uint8_t *)sec, &s, &n);
    CHECK(no && strstr(no, "outside the exFAT partition"), "a run past the partition's end must be refused: %s",
          no ? no : "offered");
    CHECK(!gcard_overlay_why(L("T_OVERLAY_SECTOR"), 0), "the overlay's run is writable in the normal image");
    CHECK(gcard_overlay_why(L("T_OVERLAY_SECTOR"), 1) != 0, "a self-test image never gets a writable overlay");
    CHECK(gcard_overlap(100, 10, 105, 10) && !gcard_overlap(100, 10, 110, 10), "overlap helper");
    CHECK(!changed_outside(0, 0), "resolving wrote nothing");
}

/* ---- the fence: the card-file grant and its writes ------------------------------------------------------ */
static void test_fence(void)
{
    static uint8_t blk[8 * 4096];
    uint64_t s = L("T_OVERLAY_SECTOR"), n = L("T_OVERLAY_SIZE") / 512, f = s / 8, c = n / 8;
    memset(blk, 0xa5, sizeof(blk));
    for (unsigned i = 0; i < 4; ++i)
        sd_rep.cid[i] = 0x1000 + i;
    ufs_calls = sd_writes = sd_unfenced = 0;

    CHECK(sb_write_card(f, 1, blk) == SB_W_AUTH && !sd_writes, "no grant: a write is refused");
    /* grants the fence must refuse, whatever the caller says */
    CHECK(!sb_card_grant(L("T_P2") / 8, 16, "x") && strstr(last_log, "not the exFAT"), "a grant in p2 refused: %s", last_log);
    CHECK(!sb_card_grant((L("T_P2") - 64) / 8, 16, "x"), "a grant from p1's tail into p2 refused: %s", last_log);
    CHECK(!sb_card_grant(f, (L("T_P1") + L("T_P1_LEN") - s) / 8 + 1, "x"), "a grant past p1's end refused: %s", last_log);
    CHECK(!sb_card_grant(0, 1, "x"), "a grant on the partition table refused: %s", last_log);
    CHECK(!sb_card_grant(L("T_P1") / 8, 1, "x") && strstr(last_log, "cluster heap"), "a grant on the exFAT boot region refused: %s", last_log);
    CHECK(!sb_card_grant(L("T_FAT") / 8, 1, "x") && strstr(last_log, "cluster heap"), "a grant on the FAT refused: %s", last_log);
    CHECK(!sb_card_grant((L("T_P2") + L("T_P2_LEN")) / 8 + 4, 4, "x"), "a grant outside every partition refused: %s", last_log);
    CHECK(!sb_card_grant(~0ULL - 2, 8, "x"), "a wrapping grant refused");
    CHECK(sb_write_card(f, 1, blk) == SB_W_AUTH && !sd_writes, "refused grants leave none");
    sb_fence_store(SB_MED_SD, f + 2, 4, 64);            /* pretend the card's live OS store were there */
    CHECK(!sb_card_grant(f, c, "x") && strstr(last_log, "OS store"), "a grant over the card's live OS store refused: %s", last_log);
    sb_fence_store(SB_MED_SD, L("T_P2") / 8, 64, 1024); /* where it really is: p2 */

    CHECK(sb_card_grant(f, c, "overlay.img") == 1, "the overlay's extent is granted: %s", last_log);
    CHECK(sb_write_card(f, 1, blk) == 0 && sd_writes == 1 && !memcmp(card + s * 512, blk, 4096), "the extent's first block is written");
    CHECK(sb_write_card(f + c - 1, 1, blk) == 0 && sd_writes == 2 && !memcmp(card + (s + n - 8) * 512, blk, 4096),
          "the extent's last block is written");
    CHECK(sb_write_card(f + 3, 8, blk) == 0 && sd_writes == 3, "eight blocks inside are written");
    CHECK(sb_write_card(f - 1, 1, blk) == SB_W_RANGE && sd_writes == 3, "one block before the extent refused");
    CHECK(sb_write_card(f + c, 1, blk) == SB_W_RANGE && sd_writes == 3, "one block past the extent refused");
    CHECK(sb_write_card(f + c - 1, 2, blk) == SB_W_RANGE && sd_writes == 3, "a write that ends one block past it refused");
    CHECK(sb_write_card(L("T_P2") / 8, 1, blk) == SB_W_RANGE && sd_writes == 3, "a write to p2 refused");
    CHECK(sb_write_card(0, 1, blk) == SB_W_RANGE && sb_write_card(f, 0, blk) == SB_W_RANGE &&
          sb_write_card(f, 1, 0) == SB_W_RANGE && sb_write_card(~0ULL, 2, blk) == SB_W_RANGE && sd_writes == 3,
          "sector 0, an empty, a null and a wrapping write refused");
    sb_fence_dry_run(1);
    CHECK(sb_write_card(f + 20, 1, blk) == SB_W_DRYRUN && sd_writes == 3, "dry run: nothing written, not reported as written");
    sb_fence_dry_run(0);
    sb_storage_hold("test hold");
    CHECK(sb_write_card(f + 20, 1, blk) == SB_W_HOLD && sd_writes == 3, "storage hold: nothing written");
    sb_storage_hold(0);
    CHECK(sb_write_card(f + 20, 1, blk) == 0 && sd_writes == 4, "after the hold: written");

    /* the two standing grants are separate: the volatile one (UFS, kern/slot.c) neither takes nor loses anything */
    int u0 = ufs_calls;
    sb_fence_store(SB_MED_UFS, 1000, 500, 2000);
    CHECK(sb_volatile_grant(SB_MED_UFS, 1100, 64, "sbos-state") == 1, "the volatile grant beside the card grant");
    CHECK(sb_write_card(f + 21, 1, blk) == 0 && sd_writes == 5, "the card grant survives a volatile grant");
    CHECK(sb_write_volatile(SB_MED_UFS, 1100, 1, blk) == 0 && ufs_calls == u0 + 1, "the volatile grant still writes its entry (the one UFS call of this test, not the card path's)");
    CHECK(sb_write_volatile(SB_MED_SD, f, 1, blk) != 0 && sb_write_card(f + c + 1100, 1, blk) == SB_W_RANGE && sd_writes == 5,
          "neither grant opens the other's range");
    sb_volatile_grant(SB_MED_UFS, 0, 0, 0);
    ufs_calls = u0;

    /* another card in the slot: the grant is void */
    sd_rep.cid[2] ^= 1;
    CHECK(sb_write_card(f, 1, blk) == SB_W_AUTH && sd_writes == 5, "another card (CID changed): refused");
    sd_rep.cid[2] ^= 1;
    CHECK(sb_write_card(f, 1, blk) == SB_W_AUTH && sd_writes == 5, "... and the grant stays void when the old card is back");
    CHECK(sb_card_grant(f, c, "overlay.img") == 1 && sb_write_card(f, 1, blk) == 0 && sd_writes == 6, "granted again: written");
    sb_card_grant_clear();
    CHECK(sb_write_card(f, 1, blk) == SB_W_AUTH && sd_writes == 6, "cleared: refused");
    /* the driver's own rule: a card write outside the fence never happens */
    CHECK(sb_blk_write(SB_MED_SD, 0, f, 1, blk) == -9 && sd_unfenced == 1 && sd_writes == 6, "a card write outside the fence: -9");

    CHECK(changed_outside(s, s + n) == 0, "every byte of the card outside overlay.img's extent is unchanged (%llu differ)",
          (unsigned long long)changed_outside(s, s + n));
    CHECK(!memcmp(card + L("T_P2") * 512, card0 + L("T_P2") * 512, L("T_P2_LEN") * 512), "p2 (the OS store's partition) untouched");
    CHECK(!memcmp(card, card0, 512) && !memcmp(card + L("T_P1") * 512, card0 + L("T_P1") * 512, (L("T_HEAP") - L("T_P1")) * 512),
          "the partition table, the exFAT boot region and the FAT untouched");
    CHECK(ufs_calls == 0, "UFS calls from the card path: %d", ufs_calls);
}

/* ---- a card whose exFAT partition starts off a 4 KiB boundary ------------------------------------------- */
static void test_unaligned(void)
{
    uint64_t s, n;
    mount_card();
    const char *no = gcard_extent(&fs, p_start, p_len, GCARD_BASE, (uint8_t *)sec, &s, &n);
    CHECK(!no && (s & 7), "base.erofs off a 4 KiB boundary is still readable: %s", no ? no : "aligned?");
    no = gcard_extent(&fs, p_start, p_len, GCARD_OVERLAY, (uint8_t *)sec, &s, &n);
    CHECK(!no && (s & 7), "overlay.img resolves: %s", no ? no : "aligned?");
    no = gcard_overlay_why(s, 0);
    CHECK(no && strstr(no, "4 KiB boundary"), "... but is not writable (not offered): %s", no ? no : "writable");
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        printf("usage: card-test CARD.img LAYOUT.h UNALIGNED.img UNALIGNED-LAYOUT.h\n");
        return 2;
    }
    load(argv[1], argv[2]);
    int c0 = checks, f0 = fails;
    test_resolve();
    printf("%s card-test: resolver (exFAT files to one run each): %d checks\n", fails == f0 ? "PASS" : "FAIL", checks - c0);
    c0 = checks, f0 = fails;
    test_fence();
    printf("%s card-test: fence (card-file grant, writes inside only, p2 untouched, UFS calls %d): %d checks\n",
           fails == f0 ? "PASS" : "FAIL", ufs_calls, checks - c0);
    load(argv[3], argv[4]);
    c0 = checks, f0 = fails;
    test_unaligned();
    printf("%s card-test: unaligned card (reads offered, overlay not): %d checks\n", fails == f0 ? "PASS" : "FAIL", checks - c0);
    printf("%s card-test: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
