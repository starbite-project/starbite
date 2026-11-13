/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of the phone glue of guest-card.h (cp7i): the stepped offer gcard_begin / gcard_tick / gcard_abort
 * that guest.c's start runs one step per timer tick, with the REAL fence (fence.c) and exFAT reader (exfat.c) on
 * card-test-image.py's fake card (card-test.sh builds and runs it):
 *   - the offer's table equals the one-sector resolver's runs (gcard_extent), the overlay read-write + granted;
 *   - every tick reads at most one FAT batch (no tick walks a whole chain);
 *   - the proof cache: a second start walks no chain (the prefs remember the proof); another card (CID), or the
 *     file's modified stamp changed on the card, walks again; a self-test image keeps nothing;
 *   - a stop between the steps (gcard_abort) withdraws the grant.
 *   card-glue-test CARD.img
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "ufs.h"
#include "sd.h"
#include "../sbshm.h"
#include "../host/sbghost.h"

/* ---- what guest.c gives guest-card.h on the phone ---- */
#define PREFS_VAL_MAX 255U
static int sbos_selftest_enabled;
static void hw_wdt_feed(void) {}
void *sb_alloc(uint64_t n) { return calloc(1, (size_t)n); }
static struct { char k[256], v[256]; } prefs[16];
static int nprefs, pref_writes, settings_soon;
static int prefs_get(const char *k, char *v, int cap)
{
    for (int i = 0; i < nprefs; ++i)
        if (!strcmp(prefs[i].k, k)) {
            snprintf(v, (size_t)cap, "%s", prefs[i].v);
            return (int)strlen(prefs[i].v);
        }
    return -1;
}
static int prefs_set(const char *k, const char *v)
{
    ++pref_writes;
    for (int i = 0; i < nprefs; ++i)
        if (!strcmp(prefs[i].k, k)) {
            if (!strcmp(prefs[i].v, v))
                return 0;
            snprintf(prefs[i].v, sizeof prefs[i].v, "%s", v);
            return 1;
        }
    snprintf(prefs[nprefs].k, sizeof prefs[nprefs].k, "%s", k);
    snprintf(prefs[nprefs].v, sizeof prefs[nprefs].v, "%s", v);
    ++nprefs;
    return 1;
}
static void settings_changed_soon(uint32_t ms) { (void)ms; ++settings_soon; }
static struct sbshm_blkdev got[SBSHM_BLK_DEVS];
static int set_blk_calls;
void sbg_set_blk(const struct sbshm_blkdev *b)
{
    memcpy(got, b, sizeof got);
    ++set_blk_calls;
}

/* ---- the fake card (as card-test.c) ---- */
static uint8_t *card;
static uint64_t card_sectors;
static int sd_reads, ufs_calls, max_read;
struct sd_report sd_rep;
uint64_t sb_ticks(void) { return 1; }
void sb_log(const char *s) { printf("  log: %s\n", s); }
void sd_allow(void) {}
int sd_up(void) { return 1; }
int sd_read(uint64_t sector, uint32_t count, uintptr_t dst)
{
    ++sd_reads;
    if ((int)count > max_read)
        max_read = (int)count;
    if (sector + count > card_sectors)
        return -1;
    memcpy((void *)dst, card + sector * 512, (size_t)count * 512);
    return 0;
}
int sb_blk_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    (void)lu, (void)lba, (void)n, (void)src;
    if (medium != SB_MED_SD)
        ++ufs_calls;
    return -1;                              /* this test writes nothing */
}
int sb_blk_write_sync(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    (void)medium, (void)lu, (void)lba, (void)n, (void)src;
    ++ufs_calls;
    return 0;
}
int gpt_n;
char gpt_name[GPT_MAX][24];
uint64_t gpt_first[GPT_MAX], gpt_last[GPT_MAX];
int gpt_load(void) { ++ufs_calls; return 0; }

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

/* one whole stepped offer; the ticks it took and the most sectors one tick read in one go */
static int offer(int *ticks)
{
    memset(got, 0xee, sizeof got);
    set_blk_calls = 0;
    max_read = 0;
    gcard_begin();
    for (*ticks = 0; !gcard_tick(); ++*ticks)
        if (*ticks > 1000)
            return -1;
    return set_blk_calls == 1 ? 0 : -1;
}

/* the runs the one-sector resolver gives (the reference) */
static void reference(uint64_t s[SBSHM_BLK_DEVS], uint64_t n[SBSHM_BLK_DEVS])
{
    static struct exfat fs;
    static uint8_t cb[1 << 17];
    static uint64_t sec[64];
    uint64_t ps, pl;
    if (gcard_partition(gc_rd, (uint8_t *)sec, &ps, &pl) || exfat_mount(&fs, gc_rd, ps, cb, sizeof cb)) {
        printf("FAIL: the reference mount\n");
        exit(2);
    }
    for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i)
        if (gcard_extent(&fs, ps, pl, gc_path[i], (uint8_t *)sec, &s[i], &n[i]))
            s[i] = n[i] = 0;
}

/* the file entry (0x85) of name in the card's /starbite/linux folder: its modified stamp */
static uint8_t *stamp_of(const char *name)
{
    size_t len = strlen(name);
    for (uint64_t o = 0; o + 64 < card_sectors * 512; o += 32) {
        uint8_t *e = card + o;
        if (e[0] != 0xc1)
            continue;
        size_t k = 0;
        while (k < len && k < 15 && e[2 + 2 * k] == (uint8_t)name[k] && !e[3 + 2 * k])
            ++k;
        if (k == len || k == 15)
            return e[-64] == 0x85 ? e - 64 + 12 : 0;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: card-glue-test CARD.img\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    card = malloc((size_t)sz);
    if (fread(card, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);
    card_sectors = (uint64_t)sz / 512;
    for (unsigned i = 0; i < 4; ++i)
        sd_rep.cid[i] = 0x5bd00000U + i;

    uint64_t rs[SBSHM_BLK_DEVS], rn[SBSHM_BLK_DEVS];
    reference(rs, rn);
    int ticks;

    /* 1. the first start: the same runs, the repo's chain walked (and remembered), one batch per tick at most */
    CHECK(!offer(&ticks), "the first offer ends, sbg_set_blk once");
    for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i) {
        int present = (got[i].flags & SBSHM_BLKDEV_PRESENT) != 0;
        CHECK(present == (rn[i] != 0) && (!present || (gc_ext[i].sector == rs[i] && got[i].sectors == rn[i])),
              "%s: offered %d (want %d) at %llu + %llu", gc_path[i], present, rn[i] != 0,
              (unsigned long long)gc_ext[i].sector, (unsigned long long)got[i].sectors);
    }
    CHECK(!(got[SBSHM_BLK_OVERLAY].flags & SBSHM_BLKDEV_RO) && gc_writable, "the overlay read-write (granted)");
    CHECK(gc.walked >= 1 && gc.cached == 0, "first start: chains walked %u, cached %u", gc.walked, gc.cached);
    CHECK(max_read <= 128, "no read of more than one FAT batch or a cluster (%d sectors)",
          max_read);
    CHECK(pref_writes >= 1 && settings_soon >= 1, "the proof kept in the prefs (%d writes)", pref_writes);
    int t1 = ticks;
    printf("  first start: %d ticks, FAT reads %u, walked %u\n", t1, gc.reads, gc.walked);

    /* 2. the second start: nothing walked */
    int w = pref_writes;
    CHECK(!offer(&ticks), "the second offer ends");
    CHECK(gc.walked == 0 && gc.cached >= 1 && gc.reads == 0, "second start: walked %u, cached %u, FAT reads %u",
          gc.walked, gc.cached, gc.reads);
    CHECK(pref_writes == w, "second start: no prefs written");
    CHECK(gc_ext[SBSHM_BLK_REPO].sector == rs[SBSHM_BLK_REPO] && got[SBSHM_BLK_REPO].sectors == rn[SBSHM_BLK_REPO],
          "second start: repo's run from the cache is the same");

    /* 3. another card: walked again */
    sd_rep.cid[3] ^= 1;
    CHECK(!offer(&ticks) && gc.walked >= 1 && gc.cached == 0, "another card's CID: walked %u, cached %u", gc.walked,
          gc.cached);
    /* 4. the repo copied again (its modified stamp changed on the card): walked again */
    uint8_t *st = stamp_of("repo.erofs");
    CHECK(st != 0, "found repo.erofs's file entry");
    if (st) {
        st[0] ^= 0x22;
        CHECK(!offer(&ticks) && gc.walked >= 1 && gc.cached == 0, "repo.erofs's stamp changed: walked %u, cached %u",
              gc.walked, gc.cached);
        CHECK(!offer(&ticks) && gc.walked == 0, "then cached again: walked %u", gc.walked);
    }
    /* 5. a stop between the steps: nothing offered, the grant withdrawn */
    gcard_begin();
    (void)gcard_tick();
    gcard_abort();
    CHECK(gcard_tick() == 1, "after the abort: no more steps");
    /* 6. a self-test image: keeps nothing, the overlay not writable */
    sbos_selftest_enabled = 1;
    nprefs = 0;
    w = pref_writes;
    CHECK(!offer(&ticks) && pref_writes == w && nprefs == 0, "a self-test image keeps no proof (%d prefs)", nprefs);
    CHECK(!gc_writable && !(got[SBSHM_BLK_OVERLAY].flags & SBSHM_BLKDEV_PRESENT), "a self-test image: no writable overlay");
    CHECK(ufs_calls == 0, "UFS never touched (%d calls)", ufs_calls);
    printf("%s card-glue-test: the stepped offer and its proof cache: %d checks, %d failed\n", fails ? "FAIL" : "PASS",
           checks, fails);
    return fails ? 1 : 0;
}
