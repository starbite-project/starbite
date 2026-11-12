/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/sbfw_health.c (Track J FW4: Settings > Startup's checks) with the loader's REAL
 * read-only checkers (shim/starlte/sbfwfs.c) and libsb's REAL sbfw_api.c / sbfw.c / str.c (the crash log's head, the
 * slot header, the record codec), around fakes: a microSD card (the home store at 4 KiB block C_BASE with its head
 * backup at p2 block 64, ext4 roots at an aligned and an unaligned 512-byte sector), UFS LU 0 with a GPT partition
 * LINUX (ext4, needs journal recovery), the fence's one volatile grant, the catalog.
 * Checks: every root kind (sd aligned / unaligned, ufs:NAME, a missing partition, RAM only), Windows without WINESP,
 * the result's line selection (problems kept), the store check (clean, a stale backup, an entry outside the store, a
 * damaged settings record, no crash log), the SBLOGS01 bundle (sections, read back by tools/store/sbstore2.py logs when
 * SBSTORE2=<claude/track-j's sbstore2.py> is given), "loader-logs" missing (-2), the slots.
 * Run: sh starbite/sbos/kern/host/sbfw-health-test.sh [-v]. Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "kern.h"
#include "store.h"
#include "catalog.h"
#include "sbfw_api.h"
#include "ufs.h"
#include "sd.h"
#include "../../ui/shell.h"

static int verbose, fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        } else if (verbose) {                                                                                          \
            printf("ok: ");                                                                                            \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- libsb's host bits ---------------------------------------------------------------------------------------- */
uint8_t sb_host_dma[1u << 16] __attribute__((aligned(4096)));
uint8_t sb_host_fw_page[4096] __attribute__((aligned(4096)));
const int sbos_selftest_enabled = 0;
uint64_t sb_ticks(void) { return 1; }
uint64_t sb_ms(void) { return 1000; }
static char logbuf[64][120];
static unsigned nlog;
void sb_log(const char *s)
{
    if (verbose) printf("    log: %s\n", s);
    snprintf(logbuf[nlog % 64], sizeof logbuf[0], "%s", s);
    nlog++;
}
void sb_logf(const char *a, const char *b) { char t[240]; snprintf(t, sizeof t, "%s%s", a, b); sb_log(t); }
void sb_logx(const char *a, uint64_t v) { char t[240]; snprintf(t, sizeof t, "%s0x%llx", a, (unsigned long long)v); sb_log(t); }
unsigned sb_log_count(void) { return nlog < 64 ? nlog : 64; }
const char *sb_log_line(unsigned i) { return logbuf[(nlog < 64 ? i : nlog + i) % 64]; }
void *sb_alloc(uint64_t n) { return calloc(1, n); }
void sb_boot_next(const char *key) { printf("FAIL: sb_boot_next(%s)\n", key); exit(2); }
void sb_gate_close(void) {}
static int gate_opens;
int sbfw_os_gate(const char *what) { (void)what; gate_opens++; return 0; }
static int lends;
void hw_slot_lend(void) { lends++; }

/* ---- the media -------------------------------------------------------------------------------------------------- */
#define CARD_BLOCKS 4096u                       /* 16 MiB */
#define C_BASE 100u                             /* the store (= p2) at 4 KiB block 100 */
#define C_CAP 1000u
#define EXT_AL 9600u                            /* an ext4 root at sector 9600 (4 KiB aligned) */
#define EXT_UN 11203u                           /* and one at sector 11203 (not aligned) */
#define UFS_BLOCKS 1024u
#define U_LINUX 10u                             /* GPT "LINUX": UFS blocks 10..521 */
static uint8_t *card, *ufs;
static int sd_unaligned_reads;
void sd_allow(void) {}
int sd_up(void) { return 1; }
int sd_read(uint64_t sector, uint32_t count, uintptr_t dst)
{
    if (sector + count > CARD_BLOCKS * 8ull) return -1;
    sd_unaligned_reads++;
    memcpy((void *)dst, card + sector * 512, count * 512u);
    return 0;
}
int sd_store_area(uint64_t *first, uint64_t *blocks) { *first = C_BASE; *blocks = C_CAP; return 1; }
int sb_blk_read(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, void *dst)
{
    if (lu) return -1;
    uint8_t *m = medium == SB_MED_SD ? card : ufs;
    uint64_t lim = medium == SB_MED_SD ? CARD_BLOCKS : UFS_BLOCKS;
    if (lba + n > lim) return -1;
    memcpy(dst, m + lba * 4096, n * 4096);
    return 0;
}
int gpt_load(void) { return 1; }
int gpt_find(const char *name, uint64_t *first, uint64_t *last)
{
    if (strcmp(name, "LINUX")) return 0;
    *first = U_LINUX;
    *last = U_LINUX + 511;
    return 1;
}

/* ---- the home store: entries at fixed blocks of the card's store ---------------------------------------------- */
static const struct { const char *name; uint32_t blk, blocks; uint16_t flags; } ENT[] = {
    { "catalog.bin", 7, 1, 0 }, { "crashlog", 16, 16, 1 }, { "loader-logs", 64, 64, 1 }, { "sbos-slot-a", 200, 8, 1 },
    { "sbos-slot-b", 300, 8, 1 },
};
#define NENT 5
static int no_logs, no_crashlog, settings_ok = 1;
int store_home(void) { return (int)STORE_SD; }
uint64_t store_base(unsigned m) { (void)m; return C_BASE; }
int store_find(const char *name, struct store_ent *e)
{
    if (!strncmp(name, "home:", 5)) name += 5;
    for (unsigned i = 0; i < NENT; i++)
        if (!strcmp(ENT[i].name, name)) {
            if ((no_logs && i == 2) || (no_crashlog && i == 1)) return 0;
            memset(e, 0, sizeof(*e));
            snprintf(e->name, sizeof e->name, "%s", name);
            e->lba = C_BASE + ENT[i].blk;
            e->size = ENT[i].blocks * 4096ull;
            e->flags = ENT[i].flags;
            e->medium = STORE_SD;
            return 1;
        }
    return 0;
}
int store_read_block(const struct store_ent *e, uint64_t k, void *dst)
{
    if (k * 4096 >= e->size) return -3;
    return sb_blk_read(SB_MED_SD, 0, e->lba + k, 1, dst);
}
static uint64_t g_first, g_n;
static int g_on;
int store_volatile_grant_part(const char *name, uint64_t min_bytes, uint64_t first, uint64_t count, struct store_ent *e)
{
    g_on = 0;
    if (!store_find(name, e) || !(e->flags & 1) || e->size < min_bytes) return 0;
    uint64_t n = e->size / 4096;
    if (first >= n || !count || count > 1024) return 0;
    if (count > n - first) count = n - first;
    g_first = e->lba + first;
    g_n = count;
    g_on = 1;
    return 1;
}
int sb_write_volatile(unsigned medium, uint64_t lba, uint64_t n, const void *src)
{
    if (medium != SB_MED_SD || !g_on || lba < g_first || lba + n > g_first + g_n) return SB_W_RANGE;
    memcpy(card + lba * 4096, src, n * 4096);
    return 0;
}
int settings_load(struct star_settings *s) { memset(s, 0, sizeof(*s)); return settings_ok; }
int settings_fw_load(struct sbfw_rec *r) { memset(r, 0, sizeof(*r)); r->slot_active = 1; return 1; }
int settings_fw_save(struct star_settings *s, const struct sbfw_rec *r) { (void)s; (void)r; return -1; }
int settings_all_save(struct star_settings *s, const struct sbfw_rec *r, const uint8_t l[8]) { (void)s; (void)r; (void)l; return -1; }
int settings_ldr_load(uint8_t l[8]) { memset(l, 0, 8); return 0; }

/* ---- the catalog --------------------------------------------------------------------------------------------- */
static struct cat_ent fcat[7];
static unsigned nfcat;
static void cat_add(const char *key, unsigned cat, unsigned m, const char *root)
{
    struct cat_ent *c = &fcat[nfcat++];
    memset(c, 0, sizeof(*c));
    snprintf(c->key, sizeof c->key, "%s", key);
    snprintf(c->name, sizeof c->name, "%s", key);
    snprintf(c->root, sizeof c->root, "%s", root);
    c->category = (uint8_t)cat;
    c->method = (uint8_t)m;
}
unsigned cat_count(void) { return nfcat; }
const struct cat_ent *cat_get(unsigned i) { return i < nfcat ? &fcat[i] : 0; }
int cat_find(const char *key)
{
    for (unsigned i = 0; i < nfcat; i++)
        if (!strcmp(fcat[i].key, key)) return (int)i;
    return -1;
}

/* ---- the images ------------------------------------------------------------------------------------------------ */
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }

/* an ext4 superblock (fwtest.c's ext4_make, no checksum): clean, or needing journal recovery */
static void ext4_at(uint8_t *img, int dirty)
{
    uint8_t *s = img + 1024;
    memset(img, 0, 4096);
    w32(s + 0x00, 256);
    w32(s + 0x04, 1024);
    w32(s + 0x0C, 900);
    w32(s + 0x10, 200);
    w32(s + 0x2C, 1762905600);
    w32(s + 0x30, 1762909200);
    w16(s + 0x34, 3);
    w16(s + 0x36, 0xFFFF);
    w16(s + 0x38, 0xEF53);
    w16(s + 0x3A, 1);
    w16(s + 0x3C, 2);
    w32(s + 0x40, 1700000000);
    w32(s + 0x4C, 1);
    w32(s + 0x5C, 4);
    w32(s + 0x60, 0x40 | 0x80 | (dirty ? 4 : 0));
    memcpy(s + 0x78, "testroot", 8);
}

static void store_make(void)
{
    uint8_t *h = card + C_BASE * 4096ull;
    memset(h, 0, 7 * 4096);
    uint8_t *d = h + 3 * 4096;
    for (unsigned i = 0; i < NENT; i++) {
        uint8_t *e = d + 64 * i;
        snprintf((char *)e, 32, "%s", ENT[i].name);
        w64(e + 32, ENT[i].blk);
        w64(e + 40, ENT[i].blocks * 4096ull);
        w16(e + 54, ENT[i].flags);
    }
    uint8_t *b = h + 4096;
    memcpy(b, "SBSTORE2", 8);
    w32(b + 8, 2);
    w32(b + 12, NENT);
    w64(b + 16, C_CAP);
    w64(b + 24, 400);
    w32(b + 32, 3);
    w32(b + 36, 4);
    w32(b + 40, 2);
    w32(b + 44, sb_crc32(d, NENT * 64));
    w32(b + 48, sb_crc32(b, 48));
    memcpy(h + 2 * 4096, "SBSETTNG", 8);
    memcpy(card + (C_BASE + 64) * 4096ull, h, 7 * 4096);      /* the head backup, current */
    /* the crash log's head: "SBCRASH1", 15 slots of 4096, next 2, count 2 */
    uint8_t *c = card + (C_BASE + 16) * 4096ull;
    memset(c, 0, 16 * 4096);
    memcpy(c, "SBCRASH1", 8);
    w32(c + 8, 1);
    w32(c + 16, 15);
    w32(c + 20, 4096);
    w32(c + 24, 2);
    w32(c + 28, 2);
    w32(c + 12, sb_crc32(c + 16, 1024 - 16));
    /* slot A: a valid header for starbite-os, "1.0 cp6"; slot B: empty */
    struct sbslot_info si;
    memset(&si, 0, sizeof si);
    si.size = 4096;
    si.crc = 1;
    snprintf(si.vstr, sizeof si.vstr, "1.0 cp6");
    snprintf(si.key, sizeof si.key, "starbite-os");
    sbslot_build(card + (C_BASE + 200) * 4096ull, &si);
    memset(card + (C_BASE + 300) * 4096ull, 0, 4096);
}

static int has_line(const sh_health_t *h, const char *s)
{
    for (int i = 0; i < SH_HEALTH_LINES; i++)
        if (strstr(h->line[i], s)) return 1;
    return 0;
}
static void dump(const char *t, const sh_health_t *h)
{
    if (!verbose) return;
    printf("  %s: bad %d checked %d fs %s where %s\n", t, h->bad, h->checked, h->fs, h->where);
    for (int i = 0; i < SH_HEALTH_LINES && h->line[i][0]; i++) printf("    | %s\n", h->line[i]);
}

int main(int argc, char **argv)
{
    const char *bundle_out = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else bundle_out = argv[i];
    }
    card = calloc(CARD_BLOCKS, 4096);
    ufs = calloc(UFS_BLOCKS, 4096);
    ext4_at(card + EXT_AL * 512ull, 0);
    ext4_at(card + EXT_UN * 512ull, 0);
    ext4_at(ufs + U_LINUX * 4096ull, 1);
    store_make();
    cat_add("linux-al", CAT_LINUX, M_LINUX, "sd:lba 9600+2048");
    cat_add("linux-un", CAT_LINUX, M_LINUX, "sd:lba 11203+2048");
    cat_add("linux-ufs", CAT_LINUX, M_LINUX, "ufs:LINUX");
    cat_add("linux-gone", CAT_LINUX, M_LINUX, "ufs:NOPE");
    cat_add("starbite-os", CAT_STARBITE, M_BARE, "none (RAM only)");
    cat_add("win11", CAT_MICROSOFT, M_UEFI, "");
    sh_health_t h;

    /* ---- the roots ---- */
    CHECK(sh_health_os("nope", &h) < 0 && h.line[0][0], "an unknown key refused with a reason");
    CHECK(sh_health_os("linux-al", &h) == 0 && h.checked && !h.bad && !strcmp(h.fs, "ext4") &&
          !strcmp(h.where, "card sectors 9600+2048") && has_line(&h, "No problems found"), "card root, aligned: ext4 clean");
    dump("linux-al", &h);
    CHECK(sd_unaligned_reads == 0, "an aligned card range reads through the block layer");
    CHECK(sh_health_os("linux-un", &h) == 0 && h.checked && !h.bad && !strcmp(h.fs, "ext4"), "card root, not aligned: ext4");
    CHECK(sd_unaligned_reads > 0, "a range that is not 4 KiB aligned reads sectors from the card driver");
    CHECK(sh_health_os("linux-ufs", &h) == 0 && h.checked && h.bad == 1 && !strcmp(h.fs, "ext4") &&
          !strcmp(h.where, "UFS LINUX") && has_line(&h, "PROBLEM: ext4: needs_recovery"), "UFS root: ext4 needs journal recovery, kept");
    dump("linux-ufs", &h);
    CHECK(sh_health_os("linux-gone", &h) == 0 && h.checked && h.bad == 1 && has_line(&h, "not in the UFS partition table"),
          "a partition the GPT lacks: one problem");
    CHECK(sh_health_os("starbite-os", &h) == 0 && !h.checked && !h.bad && !strcmp(h.fs, "-") &&
          !strcmp(h.where, "none (RAM only)"), "RAM only: nothing checked");
    CHECK(sh_health_os("win11", &h) == 0 && h.checked && h.bad == 2 && has_line(&h, "WINESP") && has_line(&h, "no WINDOWS"),
          "Windows without WINESP / WINDOWS: two problems");
    dump("win11", &h);
    int last = 0;
    for (int i = 0; i < SH_HEALTH_LINES; i++) last = h.line[i][0] ? i : last;
    CHECK(last < SH_HEALTH_LINES, "lines inside the struct");

    /* ---- the store ---- */
    CHECK(sh_store_check(&h) == 0 && !h.bad && h.checked && has_line(&h, "Header: OK, 5 entries") &&
          has_line(&h, "Settings record: OK") && has_line(&h, "Crash log: OK, 2 reports") &&
          has_line(&h, "Head backup: OK, the same") && has_line(&h, "Entries: all inside"), "a clean card store");
    dump("store", &h);
    /* an older backup: valid, not the same */
    uint8_t *bk = card + (C_BASE + 64) * 4096ull;
    uint8_t keep[7 * 4096];
    memcpy(keep, bk, sizeof keep);
    w32(bk + 4096 + 12, 4);
    w32(bk + 4096 + 44, sb_crc32(bk + 3 * 4096, 4 * 64));
    w32(bk + 4096 + 48, sb_crc32(bk + 4096, 48));
    CHECK(sh_store_check(&h) == 0 && !h.bad && has_line(&h, "valid but older (4 entries; the store has 5)"), "an older backup: noted");
    bk[4096] = 'X';
    CHECK(sh_store_check(&h) == 0 && h.bad == 1 && has_line(&h, "no valid head backup"), "a damaged backup: a problem");
    memcpy(bk, keep, sizeof keep);
    settings_ok = 0;
    no_crashlog = 1;
    CHECK(sh_store_check(&h) == 0 && h.bad == 2 && has_line(&h, "settings record is damaged") && has_line(&h, "no crash log"),
          "a damaged record and no crash log: two problems");
    settings_ok = 1;
    no_crashlog = 0;
    /* an entry past the capacity (the directory and header CRCs made right) */
    uint8_t *hd = card + C_BASE * 4096ull;
    w64(hd + 3 * 4096 + 64 * 4 + 32, C_CAP - 2);
    w32(hd + 4096 + 44, sb_crc32(hd + 3 * 4096, NENT * 64));
    w32(hd + 4096 + 48, sb_crc32(hd + 4096, 48));
    CHECK(sh_store_check(&h) == 0 && h.bad >= 1 && has_line(&h, "entry outside the store: sbos-slot-b"), "an entry past the end");
    store_make();
    hd[4096 + 20] ^= 1;
    CHECK(sh_store_check(&h) == 0 && h.bad == 1 && has_line(&h, "SBSTORE2 header is damaged"), "a damaged header");
    store_make();

    /* ---- the logs ---- */
    no_logs = 1;
    CHECK(sh_logs_save() == -2, "no \"loader-logs\": -2");
    no_logs = 0;
    sb_fw.valid = 1;
    snprintf(sb_fw.lines[22], sizeof sb_fw.lines[0], "STARBITE: the loader's line");
    snprintf(sb_fw.lines[23], sizeof sb_fw.lines[0], "STARBITE: starting starbite-os");
    sb_log("a line of Starbite OS's own log");
    CHECK(sh_logs_save() == 0 && gate_opens == 1 && lends == 1, "saved: the gate opened once, the grant lent");
    const uint8_t *lg = card + (C_BASE + 64) * 4096ull;
    uint32_t cnt = (uint32_t)(lg[12] | lg[13] << 8);
    CHECK(!memcmp(lg, "SBLOGS01", 8) && lg[8] == 1 && cnt == 3, "SBLOGS01, version 1, 3 sections (sbos-log, loader-lines, crashlog)");
    const char *want[3] = { "sbos-log", "loader-lines", "crashlog" };
    for (unsigned i = 0; i < 3 && i < cnt; i++) {
        const uint8_t *d = lg + 16 + 24 * i;
        uint32_t off = (uint32_t)(d[16] | d[17] << 8 | d[18] << 16 | (uint32_t)d[19] << 24);
        uint32_t sz = (uint32_t)(d[20] | d[21] << 8 | d[22] << 16 | (uint32_t)d[23] << 24);
        CHECK(!strcmp((const char *)d, want[i]) && off % 4096 == 0 && off + sz <= 64 * 4096, "section %u: %s", i, want[i]);
        if (i == 0) CHECK(memmem(lg + off, sz, "a line of Starbite OS's own log", 31) != 0, "the OS's log line is in it");
        if (i == 1) CHECK(memmem(lg + off, sz, "STARBITE: the loader's line\nSTARBITE: starting starbite-os\n", 58) != 0,
                          "the table's lines, oldest first");
        if (i == 2) CHECK(sz == 16 * 4096 && !memcmp(lg + off, card + (C_BASE + 16) * 4096ull, sz), "the crash log, raw");
    }
    if (bundle_out) {
        FILE *f = fopen(bundle_out, "wb");
        if (f) { fwrite(lg, 1, 64 * 4096, f); fclose(f); }
    }

    /* ---- the slots ---- */
    sh_slots_t s;
    CHECK(sh_slots_get(&s) == 0 && s.active == 1 && s.trying == 0 && s.state[0] == 1 && s.state[1] == 1 &&
          !strcmp(s.version[1], "1.0 cp6") && s.state[2] == 0, "slots: A active and valid (1.0 cp6), B empty");

    printf("sbfw-health-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
