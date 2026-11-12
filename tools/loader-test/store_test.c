/* Host test of the loader's store.c with the home store on the card (docs/STORE-ON-CARD.md): the
 * real store.c and sbfmt.c over fake media, one forked child per case (store.c keeps its state in
 * statics, as on the phone, where every boot starts fresh).
 *
 *   store_test DIR      DIR from run.sh: ufs-off.img / ufs-on.img (UFS stores, card store flag off / on),
 *                       card-home.img (a compact --home store, data at block 2048), card-plain.img
 *
 * The fake UFS is USERDATA from block 1000 with the store at +16 GiB; every UFS call is counted, so
 * "the UFS store is never touched" is checked by count. The fake card's write fence is the one the
 * real sd.c applies (the slots store.c sets): writes outside it are refused (-9) and counted.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "store.h"
#include "sd.h"
#include "sbfmt.h"
#include "host_ufs.h"

uint8_t host_ufs_buf[1U << 20] __attribute__((aligned(4096)));
struct sd_report sd_rep;
static unsigned pmsg_lines;
static char pmsg_last[512];
void star_pmsg(const char *s)
{
    pmsg_lines++;
    snprintf(pmsg_last, sizeof(pmsg_last), "%s", s);
}

#define BLK 4096U
#define UD_FIRST 1000ULL
#define UFS_OFF 4194304ULL
#define UFS_CAP 721024ULL
#define P2_FIRST 22860800ULL     /* 182886400 / 8 */
#define P2_BLOCKS 4096ULL

static uint8_t *ufs, *card;      /* the UFS store area (UFS_CAP blocks), card p2 (P2_BLOCKS) */
static int ufs_ok = 1, card_present = 1;
static unsigned ufs_calls, ufs_writes, card_reads, card_writes, card_refused;
static uint64_t ufs_write_lba[64], card_write_lba[64];
static int allowed;
static uint64_t fence_lo[SD_FENCES], fence_n[SD_FENCES];

/* ---- fake UFS ---- */
int ufs_up(void) { ufs_calls++; return ufs_ok; }
int gpt_find(const char *name, uint64_t *first, uint64_t *last)
{
    ufs_calls++;
    if (strcmp(name, "USERDATA"))
        return 0;
    *first = UD_FIRST;
    *last = UD_FIRST + UFS_OFF + UFS_CAP - 1;
    return 1;
}
static uint8_t *ufs_at(uint64_t lba, uint64_t n)
{
    if (lba < UD_FIRST + UFS_OFF || lba + n > UD_FIRST + UFS_OFF + UFS_CAP) {
        fprintf(stderr, "fake UFS: access outside the store area at %llu\n", (unsigned long long)lba);
        exit(3);
    }
    return ufs + (lba - UD_FIRST - UFS_OFF) * BLK;
}
int ufs_read(uint64_t lba, uint32_t count) { ufs_calls++; memcpy(host_ufs_buf, ufs_at(lba, count), count * BLK); return 0; }
int ufs_read_to(uint64_t lba, uint64_t count, uintptr_t dst) { ufs_calls++; memcpy((void *)dst, ufs_at(lba, count), count * BLK); return 0; }
int ufs_write_block(uint64_t lba, uintptr_t src)
{
    ufs_calls++;
    if (ufs_writes < 64)
        ufs_write_lba[ufs_writes] = lba;
    ufs_writes++;
    memcpy(ufs_at(lba, 1), (void *)src, BLK);
    return 0;
}

/* ---- fake card (sd.h) ---- */
void sd_allow(void) { allowed = 1; }
int sd_allowed(void) { return allowed; }
static int card_idle;            /* power-cycled: the card answers no read until it is brought up again */
static unsigned card_inits;
int sd_up(void)
{
    if (sd_rep.state)
        return sd_rep.state > 0;
    if (!allowed)
        return 0;
    sd_rep.present = card_present;
    sd_rep.state = card_present ? 1 : -1;
    if (card_present) {
        card_idle = 0;
        card_inits++;
    }
    return card_present;
}
void sd_forget(void)
{
    if (sd_rep.state > 0)
        sd_rep.state = 0;
}
/* hw_sd_rails as before STC2 (no sd_forget) and as now */
static void power_cycle(int forget)
{
    card_idle = 1;
    if (forget)
        sd_forget();
}
int sd_read(uint64_t sector, uint32_t count, uintptr_t dst) { (void)sector; (void)count; (void)dst; return -1; }
int sd_store_area(uint64_t *first, uint64_t *blocks)
{
    *first = *blocks = 0;
    if (!allowed || !sd_up())
        return 0;
    *first = P2_FIRST;
    *blocks = P2_BLOCKS;
    return 1;
}
static uint8_t *card_at(uint64_t lba, uint64_t n)
{
    if (lba < P2_FIRST || lba + n > P2_FIRST + P2_BLOCKS) {
        fprintf(stderr, "fake card: access outside p2 at %llu\n", (unsigned long long)lba);
        exit(3);
    }
    return card + (lba - P2_FIRST) * BLK;
}
int sd_read4k(uint64_t lba, uint64_t count, uintptr_t dst)
{
    if (!allowed) {
        fprintf(stderr, "fake card: read before sd_allow\n");
        exit(3);
    }
    if (!sd_up())
        return -1;
    if (card_idle) {
        sd_rep.last_error = -7;
        return -7;                  /* the card is in idle: no data (read_chunk's timeout) */
    }
    card_reads++;
    memcpy((void *)dst, card_at(lba, count), count * BLK);
    return 0;
}
void sd_fence(unsigned slot, uint64_t lba, uint64_t count)
{
    if (slot < SD_FENCES) {
        fence_lo[slot] = lba;
        fence_n[slot] = count;
    }
}
int sd_write4k(uint64_t lba, uint64_t count, uintptr_t src)
{
    int ok = 0;
    for (unsigned i = 0; i < SD_FENCES; ++i)
        if (fence_n[i] && lba >= fence_lo[i] && count <= fence_n[i] && lba - fence_lo[i] <= fence_n[i] - count)
            ok = 1;
    if (!ok) {
        card_refused++;
        return -9;
    }
    if (card_writes < 64)
        card_write_lba[card_writes] = lba;
    card_writes++;
    memcpy(card_at(lba, count), (void *)src, count * BLK);
    return 0;
}

/* ---- media from the images ---- */
static uint8_t *slurp(const char *dir, const char *name, size_t *n)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) {
        perror(p);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (fread(b, 1, *n, f) != *n)
        exit(2);
    fclose(f);
    return b;
}

static void load_ufs(const char *dir, const char *name)
{
    size_t n;
    uint8_t *b = slurp(dir, name, &n);
    memcpy(ufs, b, n);
    free(b);
}

/* a compact image: file blocks 0..6 at p2 0..6, the rest at its SBSTHOME data block */
static void load_card(const char *dir, const char *name)
{
    size_t n;
    uint8_t *b = slurp(dir, name, &n);
    uint64_t data_at = 7;
    if (!memcmp(b + BLK + 64, "SBSTHOME", 8))
        memcpy(&data_at, b + BLK + 80, 8);
    memcpy(card, b, 7 * BLK);
    memcpy(card + data_at * BLK, b + 7 * BLK, n - 7 * BLK);
    free(b);
}

static int fails;
#define CHECK(c, ...) do { if (c) { printf("  ok   "); } else { printf("  FAIL "); ++fails; } printf(__VA_ARGS__); printf("\n"); } while (0)

static const char *D;

static void case_legacy_flag_off(void)
{
    load_ufs(D, "ufs-off.img");
    load_card(D, "card-home.img");
    struct star_settings s;
    CHECK(store_home() == (int)STORE_UFS, "flag off: the UFS store is home");
    CHECK(!allowed && card_reads == 0, "flag off: the card is not touched (allowed %d, reads %u)", allowed, card_reads);
    CHECK(settings_load(&s) == 1 && !strcmp(s.default_key, "ufs-def"), "flag off: settings from the UFS record (%s)", s.default_key);
    CHECK(settings_save(&s) == 0 && ufs_writes == 1 && ufs_write_lba[0] == UD_FIRST + UFS_OFF + 2,
          "flag off: settings_save writes UFS block +2 only");
    struct store_ent e;
    CHECK(store_crashlog(&e) && e.medium == STORE_UFS, "flag off: the crash log is the UFS one");
    CHECK(store_find("linux.image", &e) && e.medium == STORE_UFS, "flag off: entries from the UFS store");
}

static void case_legacy_card_home(void)
{
    load_ufs(D, "ufs-on.img");
    load_card(D, "card-home.img");
    struct star_settings s;
    struct store_ent e;
    CHECK(store_home() == (int)STORE_SD, "flag on + home card store: the card is home");
    unsigned u0 = ufs_calls;
    CHECK(store_open(STORE_UFS) == 0 && store_version(STORE_UFS) == 0, "the UFS store is closed for this boot");
    store_reset(STORE_UFS);
    CHECK(store_open(STORE_UFS) == 0 && ufs_calls == u0, "and stays closed after store_reset (no UFS call)");
    CHECK(settings_load(&s) == 1 && !strcmp(s.default_key, "card-def"), "settings from the card's record (%s)", s.default_key);
    s.timeout_s = 9;
    CHECK(settings_save(&s) == 0 && card_writes == 1 && card_write_lba[0] == P2_FIRST + 2 && ufs_writes == 0,
          "settings_save writes card block +2, nothing on UFS");
    struct star_settings s2;
    CHECK(settings_load(&s2) == 1 && s2.timeout_s == 9, "and reads back");
    CHECK(store_find("linux.image", &e) && e.medium == STORE_SD && e.lba >= P2_FIRST + 2048, "entries from the card (data at +2048)");
    CHECK(!store_find("ufs:linux.image", &e), "\"ufs:\" finds nothing");
    CHECK(store_crashlog(&e) && e.medium == STORE_SD, "the crash log is the card's");
    static uint8_t blk[BLK] __attribute__((aligned(16)));
    memset(blk, 0x5a, sizeof(blk));
    CHECK(store_crashlog_write(&e, 15, (uintptr_t)blk) == 0 && card_write_lba[card_writes - 1] == e.lba + 15,
          "crash log block 15 written on the card");
    CHECK(store_crashlog_write(&e, 16, (uintptr_t)blk) == -1, "crash log block 16 refused");
    CHECK(fence_lo[0] == P2_FIRST + 2 && fence_n[0] == 1 && fence_lo[1] == e.lba && fence_n[1] == 16,
          "the fence: settings block and the 16 crash log blocks, nothing else");
    CHECK(sd_write4k(P2_FIRST + 3, 1, (uintptr_t)blk) == -9 && sd_write4k(e.lba + 15, 2, (uintptr_t)blk) == -9,
          "writes outside the fence refused (directory block, past the crash log)");
    CHECK(store_read(&e, 15, 1, (uintptr_t)blk) == 0 && store_read(&e, 15, 2, (uintptr_t)blk) == -1,
          "store_read stays inside the entry");
    CHECK(ufs_writes == 0, "no UFS write at all");
    CHECK(store_home_why() == 0 && store_marked_home(STORE_SD), "home: no reason text, marked home");
}

static void case_legacy_card_plain(void)
{
    load_ufs(D, "ufs-on.img");
    load_card(D, "card-plain.img");
    struct store_ent e;
    CHECK(store_home() == (int)STORE_UFS, "flag on + a plain card store: UFS stays home");
    CHECK(store_open(STORE_SD) > 0 && store_open(STORE_UFS) > 0, "both stores listed");
    CHECK(store_find("catalog", &e) && e.medium == STORE_UFS, "UFS first, as before");
    CHECK(store_find("sd:catalog", &e) && e.medium == STORE_SD, "\"sd:\" the card's");
    CHECK(fence_n[0] == 0 && fence_n[1] == 0, "no card write fence");
}

static void case_card_only_home(void)
{
    load_ufs(D, "ufs-off.img");
    load_card(D, "card-home.img");
    store_cmdline("console=x starmenu7=2 sbstore=card");
    struct star_settings s;
    struct store_ent e;
    CHECK(store_card_only() && store_home() == (int)STORE_SD, "sbstore=card: the card is home (no settings flag needed)");
    CHECK(settings_load(&s) == 1 && settings_save(&s) == 0, "settings load and save on the card");
    CHECK(store_find("linux.image", &e) && e.medium == STORE_SD, "entries from the card");
    CHECK(store_open(STORE_UFS) == 0, "the UFS store is not opened");
    CHECK(ufs_calls == 0 && ufs_writes == 0, "no UFS call at all (%u)", ufs_calls);
}

static void case_card_only_no_card(void)
{
    load_ufs(D, "ufs-on.img");
    card_present = 0;
    store_cmdline("sbstore=card");
    struct star_settings s;
    struct store_ent e;
    CHECK(store_home() == -1, "sbstore=card, no card: no home store");
    const char *w = store_home_why();
    CHECK(w && !strcmp(w, "NO OS STORE: NO MICROSD CARD"), "reason: %s", w ? w : "(none)");
    CHECK(settings_load(&s) == 0 && settings_status() == -2 && s.timeout_s == 0, "settings: defaults, status no store");
    CHECK(settings_save(&s) == -1, "settings_save refused");
    CHECK(!store_crashlog(&e) && !store_find("catalog", &e), "no crash log, no entries");
    CHECK(ufs_calls == 0, "the UFS store is not used as a fallback (%u UFS calls)", ufs_calls);
}

static void case_card_only_blank(void)
{
    load_ufs(D, "ufs-on.img");
    store_cmdline("sbstore=card");
    CHECK(store_home() == -1, "sbstore=card, blank p2: no home store");
    const char *w = store_home_why();
    CHECK(w && !strcmp(w, "NO OS STORE ON THE CARD"), "reason: %s", w ? w : "(none)");
    CHECK(ufs_calls == 0, "no UFS call");
}

/* STC1 on the phone (2026-11-11): Linux's launch power-cycled the card, then every store read failed */
static void case_power_cycle(void)
{
    load_ufs(D, "ufs-off.img");
    load_card(D, "card-home.img");
    store_cmdline("sbstore=card");
    struct store_ent e;
    static uint8_t buf[1U << 20] __attribute__((aligned(16)));
    CHECK(store_home() == (int)STORE_SD && store_find("linux.image", &e), "card home, linux.image found");
    CHECK(store_load(&e, (uintptr_t)buf, sizeof(buf)) == 0, "loads before the power cycle");
    power_cycle(0);
    unsigned l0 = pmsg_lines;
    CHECK(store_load(&e, (uintptr_t)buf, sizeof(buf)) == -1, "STC1 reproduced: the read after a power cycle the driver missed fails");
    CHECK(pmsg_lines == l0 + 1 && strstr(pmsg_last, "read failed sd:linux.image r=-7 lba ") &&
          strstr(pmsg_last, "(card sector ") && strstr(pmsg_last, " first "), "and pmsg says so: %s", pmsg_last);
    sd_rep.state = 1;
    power_cycle(1);
    unsigned i0 = card_inits;
    CHECK(store_load(&e, (uintptr_t)buf, sizeof(buf)) == 0 && card_inits == i0 + 1,
          "with sd_forget (hw_sd_rails since STC2): the card comes up again, CRC OK");
    CHECK(store_head(&e, (uintptr_t)buf) == 0 && *(uint32_t *)(buf) != 0xffffffffU, "and the head reads");
}

/* SBFW (section 10): the firmware part survives the loader's ordinary settings saves */
static void case_sbfw_part(void)
{
    load_ufs(D, "ufs-off.img");
    load_card(D, "card-home.img");
    store_cmdline("sbstore=card");
    struct star_settings s;
    struct sbfw_rec r, r2;
    CHECK(settings_fw(&r) == 1 && r.fw_flags == 0, "card record without an SBFW part: zeros");
    settings_load(&s);
    r.fw_flags = SBFW_FLAG_SHELL;
    r.req_action = SBFW_REQ_SAFE;
    r.req_state = SBFW_ST_ARMED;
    memcpy(r.req_key, "starbite-os", 12);
    CHECK(settings_fw_save(&s, &r) == 0, "settings_fw_save writes");
    memset(&r2, 0, sizeof(r2));
    CHECK(settings_fw(&r2) == 1 && r2.fw_flags == SBFW_FLAG_SHELL && r2.req_state == SBFW_ST_ARMED &&
          !strcmp(r2.req_key, "starbite-os"), "reads back");
    settings_load(&s);
    memcpy(s.last_key, "mac13", 6);
    CHECK(settings_save(&s) == 0, "the menu's own save (last choice)");
    memset(&r2, 0, sizeof(r2));
    CHECK(settings_fw(&r2) == 1 && r2.fw_flags == SBFW_FLAG_SHELL && r2.req_action == SBFW_REQ_SAFE,
          "the SBFW part survives the menu's save");
    CHECK(settings_load(&s) == 1 && !strcmp(s.last_key, "mac13") && !strcmp(s.default_key, "card-def"),
          "and the menu's fields are there");
}

static void case_sbfw_no_record(void)
{
    load_ufs(D, "ufs-off.img");
    load_card(D, "card-home.img");
    store_cmdline("sbstore=card");
    memset(card + 2 * BLK, 0, BLK);          /* no settings record on the card yet */
    struct star_settings s;
    struct sbfw_rec r, r2;
    CHECK(settings_fw(&r) == 0 && r.fw_flags == 0, "no record: zeros");
    sbset_defaults(&s);
    memcpy(s.default_key, "ios27", 6);
    r.fw_flags = SBFW_FLAG_SHELL;
    CHECK(settings_fw_save(&s, &r) == 0, "first record with an SBFW part");
    CHECK(settings_fw(&r2) == 1 && r2.fw_flags == SBFW_FLAG_SHELL && settings_load(&s) == 1 &&
          !strcmp(s.default_key, "ios27"), "both parts there");
}

static void case_cmdline(void)
{
    store_cmdline("xsbstore=card sbstore=cardx sbstore=ufs");
    CHECK(!store_card_only(), "sbstore=cardx / xsbstore=card / sbstore=ufs: not card only");
    store_cmdline("sbstore=card");
    CHECK(store_card_only(), "sbstore=card alone: card only");
}

static void case_legacy_no_card(void)
{
    load_ufs(D, "ufs-on.img");
    card_present = 0;
    CHECK(store_home() == (int)STORE_UFS, "flag on, no card, not card only: UFS home (before the release)");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: store_test DIR\n");
        return 2;
    }
    D = argv[1];
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "legacy, card store flag off", case_legacy_flag_off },
        { "legacy, flag on, home card store", case_legacy_card_home },
        { "legacy, flag on, plain card store", case_legacy_card_plain },
        { "legacy, flag on, no card", case_legacy_no_card },
        { "card only, home card store", case_card_only_home },
        { "card only, no card", case_card_only_no_card },
        { "card only, blank card", case_card_only_blank },
        { "power cycle between boot and launch (STC1)", case_power_cycle },
        { "SBFW part kept by the menu's saves", case_sbfw_part },
        { "SBFW part on a card without a record", case_sbfw_no_record },
        { "command line", case_cmdline },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        printf("%s\n", cases[i].name);
        fflush(stdout);
        pid_t p = fork();
        if (!p) {
            ufs = calloc(UFS_CAP, BLK);
            card = calloc(P2_BLOCKS, BLK);
            cases[i].fn();
            fflush(stdout);
            _exit(fails ? 1 : 0);
        }
        int st = 0;
        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) {
            printf("  case failed (status %d)\n", st);
            ++bad;
        }
    }
    printf(bad ? "store_test: %d case(s) FAILED\n" : "store_test: all passed\n", bad);
    return bad != 0;
}
