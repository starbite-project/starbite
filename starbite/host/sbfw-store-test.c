/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of libsb's half of the loader <-> OS interface (docs/STARBITE-FIRMWARE-LAYER.md section 10, 2026-11-12),
 * built and run by starbite/host/sbfw-store-test.sh:
 *   - libsb's REAL sbfw.c: the SBFW table (0x91FFE000) and the settings record's SBFW part (bytes 112..255), on
 *     the loader codec's own test vectors (fixtures/sbfw: sbfw-table.bin, sbfw-settings.bin, made by
 *     tools/loader-test/sbfw_test.c), and byte for byte the same encoding;
 *   - libsb's REAL store.c, fence.c, blkio.c and str.c on fake media: a UFS USERDATA with a store at +16 GiB and a
 *     microSD card whose partition-2 store keeps its head at the start and its data further in. With the table
 *     saying "card, card only" the card store gets the settings and "sbos-state" writes and the UFS store is
 *     never read or written; the card is also home without a table when its header says HOME; without a table
 *     (or with a damaged one) and no HOME card everything is as before (UFS).
 * Every scenario runs in its own process (fork), so libsb's statics start fresh each time.
 * Since the Startup pages (2026-11-12) also libsb's REAL sbfw_api.c on the same media: the settings struct round trip
 * (every other byte of the record kept), one-shot requests (written, refused, the mailbox key), the boot menu /
 * Recovery keys and the crash reports from fixtures/sbfw/sbfw-crashlog.bin (made by the loader's own encoder:
 * sbfw-crashlog-gen.c); the catalog and sb_boot_next are fakes here.
 *   sbfw-store-test FIXTURE_DIR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <setjmp.h>
#include "sb.h"
#include "store.h"
#include "catalog.h"
#include "sbfw_api.h"
#include "ufs.h"
#include "sd.h"

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

/* ---- the fixed pages libsb uses (SB_HOST_FAKE_MEM) -------------------------------------------------- */
uint8_t sb_host_dma[8u << 20] __attribute__((aligned(4096)));
uint8_t sb_host_fw_page[4096] __attribute__((aligned(4096)));

/* ---- the log ------------------------------------------------------------------------------------------- */
static int verbose;
uint64_t sb_ticks(void) { return 12345; }
void sb_log(const char *s) { if (verbose) printf("    log: %s\n", s); }
void sb_logf(const char *a, const char *b) { if (verbose) printf("    log: %s%s\n", a, b); }
void sb_logx(const char *a, uint64_t v) { if (verbose) printf("    log: %s0x%llx\n", a, (unsigned long long)v); }

/* ---- fake UFS: USERDATA from block 2048, the store at +16 GiB (only the store's blocks exist) ---------- */
#define U_FIRST 2048ULL
#define U_BASE (U_FIRST + 4194304ULL)
#define U_CAP 1024ULL
#define U_LAST (U_BASE + U_CAP + 4000ULL)
static uint8_t *ufs_store;
static int ufs_reads, ufs_writes, ufs_unfenced, ufs_syncs;
int ufs_fua;
int gpt_n;
char gpt_name[GPT_MAX][24];
uint64_t gpt_first[GPT_MAX], gpt_last[GPT_MAX];
int gpt_load(void) { return 0; }
int ufs_up(void) { return 1; }
int gpt_find(const char *name, uint64_t *first, uint64_t *last)
{
    if (strcmp(name, "USERDATA"))
        return 0;
    *first = U_FIRST;
    *last = U_LAST;
    return 1;
}
static int ufs_rd(uint64_t lba, uint64_t n, void *dst)
{
    ++ufs_reads;
    for (uint64_t i = 0; i < n; ++i) {
        uint8_t *d = (uint8_t *)dst + i * 4096;
        if (lba + i >= U_BASE && lba + i < U_BASE + U_CAP)
            memcpy(d, ufs_store + (lba + i - U_BASE) * 4096, 4096);
        else
            memset(d, 0, 4096);
    }
    return 0;
}
int ufs_read(uint64_t lba, uint32_t count) { return ufs_rd(lba, count, (void *)UFS_BUF); }
int ufs_read_to(uint64_t lba, uint64_t count, uintptr_t dst) { return ufs_rd(lba, count, (void *)dst); }
int ufs_read_lu_to(unsigned lu, uint64_t lba, uint64_t count, uintptr_t dst) { return lu ? -1 : ufs_rd(lba, count, (void *)dst); }
int ufs_write_blocks(unsigned lu, uint64_t lba, uint64_t count, uintptr_t src)
{
    if (!sb_fence_active()) {
        ++ufs_unfenced;
        return -6;
    }
    ++ufs_writes;
    if (lu || lba < U_BASE || lba + count > U_BASE + U_CAP)
        return -1;
    memcpy(ufs_store + (lba - U_BASE) * 4096, (const void *)src, count * 4096);
    return 0;
}
int ufs_sync_cache(unsigned lu) { (void)lu; ++ufs_syncs; return 0; }

/* ---- fake card: partition 2 from 4 KiB block C_FIRST; its store's head at 0..6, the data from C_DATA --- */
#define C_FIRST 1000ULL
#define C_BLOCKS 4096ULL
#define C_CAP 2048ULL
#define C_DATA 1024ULL
static uint8_t *card;                       /* blocks C_FIRST .. C_FIRST + C_BLOCKS */
static int sd_ok, sd_reads, sd_writes, sd_unfenced;
struct sd_report sd_rep;
void sd_allow(void) { sd_ok = 1; }
int sd_allowed(void) { return sd_ok; }
int sd_read(uint64_t sector, uint32_t count, uintptr_t dst) { (void)sector, (void)count, (void)dst; return -1; }
int sd_store_area(uint64_t *first, uint64_t *blocks)
{
    *first = *blocks = 0;
    if (!sd_ok)
        return 0;
    ++sd_reads;
    *first = C_FIRST;
    *blocks = C_BLOCKS;
    return 1;
}
int sd_read4k(uint64_t lba, uint64_t count, uintptr_t dst)
{
    if (!sd_ok)
        return -1;
    ++sd_reads;
    if (lba < C_FIRST || lba + count > C_FIRST + C_BLOCKS)
        return -1;
    memcpy((void *)dst, card + (lba - C_FIRST) * 4096, count * 4096);
    return 0;
}
int sd_write4k(uint64_t lba, uint64_t count, uintptr_t src)
{
    if (!sb_fence_active()) {
        ++sd_unfenced;
        return -9;
    }
    ++sd_writes;
    if (lba < C_FIRST || lba + count > C_FIRST + C_BLOCKS)
        return -1;
    memcpy(card + (lba - C_FIRST) * 4096, (const void *)src, count * 4096);
    return 0;
}

/* ---- building the stores ------------------------------------------------------------------------- */
static void p32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void p64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct ent { const char *name; uint64_t block, size; uint16_t flags; };

/* blocks 1 (header, with the home extension when home_ext), 3 (directory) and each entry's data */
static void build_store(uint8_t *s, uint64_t cap, uint64_t used, const struct ent *e, unsigned n, int home_ext,
                        int home_flag, uint64_t first_data)
{
    uint8_t *h = s + 4096, *d = s + 3 * 4096;
    for (unsigned i = 0; i < n; ++i) {
        uint8_t *x = d + 64 * i;
        strncpy((char *)x, e[i].name, 31);
        uint8_t *data = s + e[i].block * 4096;
        for (uint64_t k = 0; k < e[i].size; ++k)
            data[k] = (uint8_t)(k * 13 + i);
        p64(x + 32, e[i].block);
        p64(x + 40, e[i].size);
        p32(x + 48, sb_crc32(data, e[i].size));
        x[52] = 0;
        x[54] = (uint8_t)e[i].flags;
    }
    memcpy(h, "SBSTORE2", 8);
    p32(h + 8, 2);
    p32(h + 12, n);
    p64(h + 16, cap);
    p64(h + 24, used);
    p32(h + 32, 3);
    p32(h + 36, 1);
    p32(h + 40, 2);
    p32(h + 44, sb_crc32(d, n * 64));
    p32(h + 48, sb_crc32(h, 48));
    if (home_ext) {
        memcpy(h + 64, "SBSTHOME", 8);
        p32(h + 72, 1);
        p32(h + 76, home_flag ? 1 : 0);
        p64(h + 80, first_data);
        p32(h + 88, sb_crc32(h + 64, 24));
    }
}

static uint8_t fx_table[4096], fx_settings[4096];

static void ufs_settings(uint32_t flags)
{
    uint8_t *b = ufs_store + 2 * 4096;
    memset(b, 0, 4096);
    memcpy(b, "SBSETTNG", 8);
    p32(b + 8, 1);
    p32(b + 16, 7);                 /* seq */
    p32(b + 20, 5);
    p32(b + 24, flags);
    strcpy((char *)b + 32, "ufs-default");
    b[104] = 2;                     /* the loader's crash mode: kept by every save */
    b[110] = 90;
    p32(b + 12, sb_crc32(b + 16, 240));
}

/* home_flag: the card header's HOME bit; ufs_flags: the UFS record's flags (SET_SD_STORE) */
static void media(int home_flag, uint32_t ufs_flags)
{
    ufs_store = calloc(U_CAP, 4096);
    card = calloc(C_BLOCKS, 4096);
    static const struct ent ue[] = { { "catalog", 7, 300, 0 }, { "wifinet", 8, 20, 0 }, { "sbos-state", 300, 1048576, 1 } };
    build_store(ufs_store, U_CAP, 600, ue, 3, 0, 0, 7);
    ufs_settings(ufs_flags);
    static const struct ent ce[] = { { "catalog", C_DATA, 500, 0 }, { "crashlog", C_DATA + 1, 65536, 1 },
                                     { "sbos-state", C_DATA + 100, 1048576, 1 } };
    build_store(card, C_CAP, C_DATA + 400, ce, 3, 1, home_flag, C_DATA);
    memcpy(card + 2 * 4096, fx_settings, 4096);
}

static void table(uint32_t medium, uint64_t base, uint32_t flags, int damage)
{
    uint8_t *p = sb_host_fw_page;
    memcpy(p, fx_table, 4096);
    p32(p + 80, medium);
    p64(p + 88, base);
    p32(p + 148, flags);
    p32(p + 12, sb_crc32(p + 16, 4096 - 16));
    if (damage)
        p[300] ^= 0x40;
}

static void auth_store(unsigned medium)
{
    struct sb_auth a;
    sb_auth_init(&a, medium, 0, 0, 0, SB_TIER1, SB_KIND_STORE, "settings");
    sb_auth_step(&a, sb_steps_needed(SB_TIER1, SB_KIND_STORE));
    CHECK(sb_gate_open(&a), "a store authorisation opens");
}

/* ---- fakes for sbfw_api.c: a small catalog, the mailbox (sb_boot_next comes back here) ------------------------ */
static struct cat_ent fcat[5];
static unsigned fcat_n;
static void fcat_add(const char *key, const char *name, unsigned method)
{
    struct cat_ent *c = &fcat[fcat_n++];
    memset(c, 0, sizeof(*c));
    strcpy(c->key, key);
    strcpy(c->name, name);
    c->method = (uint8_t)method;
}
static void fcat_init(void)
{
    if (fcat_n)
        return;
    fcat_add("starbite-os", "Starbite OS", M_BARE);
    fcat_add("linux-mainline", "Linux", M_LINUX);
    fcat_add("ios26", "iOS 26", M_XNU);
    fcat_add("win11", "Windows 11", M_UEFI);
    fcat_add("android", "One UI 8", M_ANDROID);
}
unsigned cat_count(void) { fcat_init(); return fcat_n; }
const struct cat_ent *cat_get(unsigned i) { return i < cat_count() ? &fcat[i] : 0; }
int cat_find(const char *key)
{
    for (unsigned i = 0; i < cat_count(); ++i)
        if (!strcmp(fcat[i].key, key))
            return (int)i;
    return -1;
}
static jmp_buf boot_jb;
static char boot_key[24];
void sb_boot_next(const char *key)
{
    snprintf(boot_key, sizeof(boot_key), "%s", key);
    longjmp(boot_jb, 1);
}

static uint8_t fx_crashlog[3 * 4096];

/* ---- scenarios ------------------------------------------------------------------------------------------ */

static void t_fixtures(void)
{
    struct sb_fw f;
    CHECK(sbfw_table_check(fx_table), "the loader's table fixture is valid");
    CHECK(sbfw_table_decode(fx_table, &f) == 1 && f.valid, "decoded");
    CHECK(f.debug_level == 2 && f.req_action == SBFW_REQ_SAFE && !strcmp(f.key, "starbite-os"), "level 2, safe, starbite-os");
    CHECK(f.console_y0 == 2368 && f.console_y1 == 2960 && f.fb_base == 0xcc000000ULL && f.stride == 5760 && f.fb_fmt == 0,
          "console and framebuffer");
    CHECK(f.loader_version == 10 && f.boot_reason == SBFW_WHY_SHELL, "loader 10, shell start");
    CHECK(f.store_medium == STORE_SD && f.store_flags == 1 && f.store_base == 22860800ULL && f.store_capacity == 8389120ULL,
          "the card store, HOME, base and capacity");
    CHECK(f.crash_kind == 3 && f.crash_sev == 2 && !strcmp(f.crash_key, "linux-pmos"), "last crash");
    CHECK(f.post_fails == 1 && f.post_warns == 4 && f.settings_status == 1, "POST and settings status");
    CHECK(f.flags == (SBFW_TF_CARD_ONLY | SBFW_TF_SHELL), "card only, shell");
    CHECK(!strcmp(f.lines[0], "STARBITE: line 16. of the ring"), "oldest line: %s", f.lines[0]);
    CHECK(!strncmp(f.lines[23], "STARBITE: line 39. of the ring that is long", 40) && strlen(f.lines[23]) == 119,
          "newest line last, 119 characters");
    uint8_t bad[4096];
    memcpy(bad, fx_table, 4096);
    bad[2000] ^= 1;
    CHECK(!sbfw_table_check(bad) && sbfw_table_decode(bad, &f) == 0 && !f.valid && !f.store_medium && !f.flags,
          "a flipped byte: not valid, all zero");
    memcpy(bad, fx_table, 4096);
    bad[8] = 2;
    CHECK(!sbfw_table_check(bad), "version 2: not ours");

    struct sbfw_rec r;
    CHECK(g32(fx_settings + 12) == sb_crc32(fx_settings + 16, 240), "settings fixture CRC");
    sbfw_rec_decode(fx_settings, &r);
    CHECK(r.fw_flags == 1 && r.req_action == 3 && r.req_state == 1 && r.req_seq == 513, "fw_flags 1, req 3, state 1, seq 513");
    CHECK(!strcmp(r.req_key, "linux-pmos") && !strcmp(r.shell_key, "starbite-next"), "req_key, shell_key");
    int undo_ok = 1;
    for (int i = 0; i < 64; ++i)
        undo_ok &= r.undo[i] == (uint8_t)(i * 7);
    CHECK(undo_ok, "undo[i] = i * 7");
    CHECK(!strcmp((const char *)fx_settings + 32, "linux-pmos"), "default_key");
    uint8_t blk[4096];
    memcpy(blk, fx_settings, 4096);
    memset(blk + 112, 0x5a, 144);
    sbfw_rec_encode(blk, &r);
    sbset_crc_fix(blk);
    CHECK(!memcmp(blk, fx_settings, 4096), "decode + encode + CRC fix gives the loader's bytes back");
    memcpy(blk, fx_settings, 4096);
    blk[114] = 2;
    sbfw_rec_decode(blk, &r);
    CHECK(r.fw_flags == 0 && r.req_seq == 0 && !r.req_key[0], "an unknown SBFW version reads as all zero");
}

/* the card store, as the table says (card only): settings and sbos-state on the card, UFS never touched */
static void t_card_only(void)
{
    media(1, SET_SD_STORE);
    table(STORE_SD, C_FIRST, SBFW_TF_CARD_ONLY | SBFW_TF_SHELL, 0);
    CHECK(sb_fw_read() == 1 && sb_fw.valid && sb_fw.store_medium == STORE_SD, "table read");
    CHECK(store_card_only(), "card only");
    CHECK(store_home() == (int)STORE_SD, "home: the card");
    struct star_settings st;
    CHECK(settings_load(&st) == 1 && !strcmp(st.default_key, "linux-pmos"), "the card's record read");
    struct sbfw_rec r;
    CHECK(settings_fw_load(&r) == 1 && r.req_seq == 513 && !strcmp(r.shell_key, "starbite-next"), "its SBFW part");
    struct store_ent e;
    CHECK(store_find("catalog", &e) && e.medium == STORE_SD && e.lba == C_FIRST + C_DATA, "catalog from the card");
    CHECK(!store_find("ufs:catalog", &e) && !store_find("ufs:wifinet", &e), "no UFS entries");
    CHECK(store_find("home:crashlog", &e) && e.medium == STORE_SD, "home: prefix");
    CHECK(store_open(STORE_UFS) == 0 && store_version(STORE_UFS) == 0, "the UFS store stays closed");

    /* settings_save: the gate first, then block 2 of the card, the tail kept */
    CHECK(settings_save(&st) == SB_W_AUTH && sd_writes == 0, "no authorisation: nothing written");
    auth_store(SB_MED_SD);
    sb_strcpy(st.default_key, sizeof(st.default_key), "macos-sequoia");
    CHECK(settings_save(&st) == 0 && sd_writes == 1, "saved on the card");
    const uint8_t *b2 = card + 2 * 4096;
    CHECK(!strcmp((const char *)b2 + 32, "macos-sequoia") && g32(b2 + 16) == st.seq, "the new key and seq on the card");
    CHECK(!memcmp(b2 + 104, fx_settings + 104, 152) && g32(b2 + 12) == sb_crc32(b2 + 16, 240), "bytes 104..255 kept, CRC");
    r.req_action = SBFW_REQ_VERBOSE;
    r.req_state = SBFW_ST_ARMED;
    r.req_seq++;
    sb_strcpy(r.req_key, sizeof(r.req_key), "linux-mainline");
    CHECK(settings_fw_save(&st, &r) == 0 && sd_writes == 2, "SBFW part saved");
    struct sbfw_rec r2;
    CHECK(settings_fw_load(&r2) == 1 && r2.req_action == 1 && r2.req_state == 1 && r2.req_seq == 514 &&
          !strcmp(r2.req_key, "linux-mainline") && r2.undo[9] == 63 && !strcmp(r2.shell_key, "starbite-next"),
          "read back: the request, the rest kept");
    uint8_t want[4096];
    memcpy(want, b2, 4096);
    sbfw_rec_encode(want, &r);
    sbset_crc_fix(want);
    CHECK(!memcmp(want, b2, 256), "the loader's codec writes the same bytes");
    sb_gate_close();

    /* the state slot (kern/slot.c): granted on the card, written through the fence, read back */
    CHECK(store_volatile_grant("sbos-state", 1048576, &e) && e.medium == STORE_SD && e.lba == C_FIRST + C_DATA + 100,
          "sbos-state granted on the card");
    uint8_t blk[8192];
    memset(blk, 0xa5, sizeof(blk));
    CHECK(sb_write_volatile(SB_MED_SD, e.lba + 128, 2, blk) == 0 && sd_writes == 3, "a state write on the card");
    CHECK(!memcmp(card + (C_DATA + 100 + 128) * 4096, blk, 8192), "on the card");
    CHECK(sb_write_volatile(SB_MED_UFS, U_BASE + 300, 1, blk) == SB_W_RANGE, "not on UFS");
    CHECK(sb_write_volatile(SB_MED_SD, C_FIRST + 2, 1, blk) == SB_W_RANGE && sd_writes == 3, "not the settings block");
    CHECK(sb_write_volatile(SB_MED_SD, e.lba + 256, 1, blk) == SB_W_RANGE, "not past the entry");
    sb_fence_dry_run(1);
    CHECK(sb_write_volatile(SB_MED_SD, e.lba, 1, blk) == SB_W_DRYRUN && sd_writes == 3, "dry run");
    sb_fence_dry_run(0);
    CHECK(store_volatile_grant("crashlog", 4096, &e) && e.medium == STORE_SD, "another volatile entry");
    CHECK(ufs_reads == 0 && ufs_writes == 0 && ufs_unfenced == 0 && ufs_syncs == 0, "UFS: %d reads, %d writes",
          ufs_reads, ufs_writes);
    CHECK(sd_unfenced == 0, "no unfenced card write");
}

/* the table says card (not card only): the UFS store is closed for this boot, as the loader does */
static void t_card_home(void)
{
    media(1, 0);
    table(STORE_SD, C_FIRST, 0, 0);
    sb_fw_read();
    CHECK(!store_card_only() && store_home() == (int)STORE_SD, "home: the card");
    struct store_ent e;
    CHECK(!store_find("ufs:catalog", &e) && store_find("catalog", &e) && e.medium == STORE_SD, "UFS closed, card first");
    auth_store(SB_MED_SD);
    struct star_settings st;
    settings_load(&st);
    CHECK(settings_save(&st) == 0 && sd_writes == 1 && ufs_writes == 0, "settings on the card");
    sb_gate_close();
    CHECK(store_volatile_grant("sbos-state", 1048576, &e) && e.medium == STORE_SD, "slot on the card");
    uint8_t blk[4096] = { 1 };
    CHECK(sb_write_volatile(SB_MED_SD, e.lba, 1, blk) == 0 && ufs_writes == 0 && ufs_syncs == 0, "slot write on the card");
}

/* no table, the UFS record allows the card and the card's header says HOME: the card is home */
static void t_notable_card_home(void)
{
    media(1, SET_SD_STORE);
    sb_fw_read();
    CHECK(!sb_fw.valid, "no table");
    CHECK(store_home() == (int)STORE_SD, "home: the HOME card");
    struct store_ent e;
    CHECK(store_volatile_grant("sbos-state", 1048576, &e) && e.medium == STORE_SD, "slot on the card");
    auth_store(SB_MED_SD);
    struct star_settings st;
    CHECK(settings_load(&st) && !strcmp(st.default_key, "linux-pmos"), "the card's record");
    CHECK(settings_save(&st) == 0 && sd_writes == 1 && ufs_writes == 0, "saved on the card, nothing on UFS");
}

/* no table (or a damaged one), no HOME card: exactly as before (UFS) */
static void t_ufs(int damaged, uint32_t ufs_flags, int card_home)
{
    media(card_home, ufs_flags);
    if (damaged)
        table(STORE_SD, C_FIRST, SBFW_TF_CARD_ONLY, 1);
    CHECK(sb_fw_read() == 0 && !sb_fw.valid, "no valid table");
    CHECK(store_home() == (int)STORE_UFS, "home: UFS");
    CHECK(sd_ok == !!(ufs_flags & SET_SD_STORE), "the card touched only with SET_SD_STORE");
    if (!(ufs_flags & SET_SD_STORE))
        CHECK(sd_reads == 0, "the card not read");
    struct star_settings st;
    CHECK(settings_load(&st) && !strcmp(st.default_key, "ufs-default") && st.seq == 7, "the UFS record");
    struct store_ent e;
    CHECK(store_find("catalog", &e) && e.medium == STORE_UFS, "catalog from UFS first");
    CHECK(store_find("home:wifinet", &e) && e.medium == STORE_UFS && store_find("ufs:wifinet", &e), "wifinet");
    if (ufs_flags & SET_SD_STORE)
        CHECK(store_find("sd:crashlog", &e) && e.medium == STORE_SD, "the card's store still readable");
    auth_store(SB_MED_UFS);
    sb_strcpy(st.last_key, sizeof(st.last_key), "android-oneui");
    CHECK(settings_save(&st) == 0 && ufs_writes == 1 && sd_writes == 0, "saved on UFS");
    const uint8_t *b2 = ufs_store + 2 * 4096;
    CHECK(!strcmp((const char *)b2 + 56, "android-oneui") && b2[104] == 2 && b2[110] == 90 &&
          g32(b2 + 12) == sb_crc32(b2 + 16, 240), "UFS record: key, the loader's bytes kept, CRC");
    struct sbfw_rec r;
    CHECK(settings_fw_load(&r) == 1 && r.fw_flags == 0 && r.req_seq == 0, "no SBFW part: zeros");
    sb_gate_close();
    CHECK(store_volatile_grant("sbos-state", 1048576, &e) && e.medium == STORE_UFS && e.lba == U_BASE + 300, "slot on UFS");
    uint8_t blk[4096] = { 7 };
    CHECK(sb_write_volatile(SB_MED_UFS, e.lba + 5, 1, blk) == 0 && ufs_writes == 2 && ufs_syncs == 1 && sd_writes == 0,
          "slot write on UFS: FUA + flush");
    CHECK(sb_write_volatile(SB_MED_SD, C_FIRST + C_DATA + 100, 1, blk) == SB_W_RANGE && sd_writes == 0, "not on the card");
}

/* the table names a card store at another base: no home, nothing written anywhere */
static void t_mismatch(void)
{
    media(1, SET_SD_STORE);
    table(STORE_SD, C_FIRST + 8, SBFW_TF_CARD_ONLY, 0);
    sb_fw_read();
    CHECK(store_home() == -1, "no home");
    struct star_settings st;
    CHECK(!settings_load(&st), "no record");
    auth_store(SB_MED_SD);
    CHECK(settings_save(&st) == -1, "no save");
    struct store_ent e;
    CHECK(!store_volatile_grant("sbos-state", 1048576, &e), "no slot");
    CHECK(!store_find("ufs:catalog", &e) && !store_find("home:catalog", &e), "card only: no UFS entries, no home");
    CHECK(store_find("catalog", &e) && e.medium == STORE_SD, "the card's entries still readable");
    CHECK(sd_writes == 0 && ufs_writes == 0 && ufs_reads == 0, "nothing written, UFS untouched");
}

/* the table says UFS (not card only): UFS home even though the card says HOME (the loader decided) */
static void t_table_ufs(void)
{
    media(1, 0);
    table(STORE_UFS, U_BASE, 0, 0);
    sb_fw_read();
    CHECK(store_home() == (int)STORE_UFS && !sd_ok, "home: UFS, the card untouched");
    struct store_ent e;
    CHECK(store_volatile_grant("sbos-state", 1048576, &e) && e.medium == STORE_UFS, "slot on UFS");
}

/* sbfw_api.c: the settings struct on the card (card only); every byte it does not own stays */
static void t_api_settings(void)
{
    media(1, SET_SD_STORE);
    table(STORE_SD, C_FIRST, SBFW_TF_CARD_ONLY | SBFW_TF_SHELL, 0);
    sb_fw_read();
    uint8_t *b2 = card + 2 * 4096;
    b2[104] = 1; b2[105] = 2; b2[106] = 2; b2[107] = 6; b2[108] = 2; b2[109] = 0; b2[110] = 60; b2[111] = 0;
    p32(b2 + 24, 1U << SET_LOG_SHIFT | 1U << SET_POST_SHIFT | SET_SD_STORE | 0x1000U);   /* + a bit nobody knows */
    p32(b2 + 20, 7);
    p32(b2 + 12, sb_crc32(b2 + 16, 240));
    struct sbfw_settings s;
    CHECK(sbfw_settings_read(&s) == 1, "read");
    CHECK(!strcmp(s.default_key, "linux-pmos") && s.timeout_s == 7 && s.debug_level == 1 && s.post == 1, "key, timeout, levels");
    CHECK(s.crash_mode == 1 && s.crash_detail == 2 && s.crash_action == 2 && s.wdt_mode == 2 && s.wdt_s == 60,
          "the loader's crash / watchdog bytes");
    CHECK(s.shell == 1 && !strcmp(s.shell_key, "starbite-next"), "shell mode, shell key");
    uint8_t before[4096];
    memcpy(before, b2, 4096);

    CHECK(sbfw_settings_write(&s) == SB_W_AUTH && sd_writes == 0, "no authorisation: nothing written");
    auth_store(SB_MED_SD);
    struct sbfw_settings w = s;
    w.debug_level = 4;
    CHECK(sbfw_settings_write(&w) == SBFW_E_INVAL, "debug level 4 refused");
    w = s; w.post = 3;
    CHECK(sbfw_settings_write(&w) == SBFW_E_INVAL, "POST 3 refused");
    w = s; w.wdt_mode = SBFW_WDT_FIXED; w.wdt_s = 9;
    CHECK(sbfw_settings_write(&w) == SBFW_E_INVAL, "watchdog 9 s refused");
    w = s; w.crash_action = 3;
    CHECK(sbfw_settings_write(&w) == SBFW_E_INVAL && sd_writes == 0, "crash action 3 refused, nothing written");

    w = s;
    strcpy(w.default_key, "macos-sequoia");
    w.timeout_s = 12;
    w.debug_level = SBFW_DEBUG_HIGHEST;
    w.post = SBFW_POST_OFF;
    w.wdt_mode = SBFW_WDT_FIXED;
    w.wdt_s = 30;
    w.crash_mode = SBFW_CRASH_ON;
    w.crash_action = SBFW_AFTER_RECOVERY;
    w.crash_detail = 3;
    w.shell = 0;
    strcpy(w.shell_key, "starbite-os");
    CHECK(sbfw_settings_write(&w) == 0 && sd_writes == 1, "written on the card");
    struct sbfw_settings r;
    CHECK(sbfw_settings_read(&r) == 1 && !strcmp(r.default_key, w.default_key) && r.timeout_s == w.timeout_s &&
          r.debug_level == w.debug_level && r.post == w.post && r.wdt_mode == w.wdt_mode && r.wdt_s == w.wdt_s &&
          r.crash_mode == w.crash_mode && r.crash_action == w.crash_action && r.crash_detail == w.crash_detail &&
          r.shell == w.shell && !strcmp(r.shell_key, w.shell_key), "read back as written");
    CHECK(g32(b2 + 16) == g32(before + 16) + 1 && g32(b2 + 12) == sb_crc32(b2 + 16, 240), "seq + 1, CRC");
    CHECK(g32(b2 + 24) == (3U << SET_LOG_SHIFT | 2U << SET_POST_SHIFT | SET_SD_STORE | 0x1000U), "flags: levels, the rest kept");
    CHECK(b2[104] == 0 && b2[105] == 3 && b2[106] == 1 && b2[107] == 6 && b2[108] == 2 && b2[110] == 30 && b2[111] == 0,
          "bytes 104..111 (crash_timeout kept)");
    CHECK(b2[115] == 0 && !strcmp((const char *)b2 + 144, "starbite-os"), "fw_flags, shell_key");
    CHECK(!memcmp(b2 + 56, before + 56, 48), "last_key, next_key kept");
    CHECK(!memcmp(b2 + 112, before + 112, 3) && !memcmp(b2 + 116, before + 116, 28) && !memcmp(b2 + 168, before + 168, 88),
          "\"FW\" v1, the request fields, the undo record and the reserved bytes kept");
    CHECK(!memcmp(b2 + 256, before + 256, 4096 - 256), "past the record: kept");
    w.wdt_mode = SBFW_WDT_ENTRY;
    CHECK(sbfw_settings_write(&w) == 0 && b2[108] == 0 && b2[110] == 0, "per entry: wdt_s cleared");
    sb_gate_close();
}

/* sbfw_api.c: no home store: nothing to read or write */
static void t_api_nohome(void)
{
    media(1, SET_SD_STORE);
    table(STORE_SD, C_FIRST + 8, SBFW_TF_CARD_ONLY, 0);
    sb_fw_read();
    struct sbfw_settings s;
    CHECK(sbfw_settings_read(&s) == 0 && !s.default_key[0] && !s.shell, "defaults");
    auth_store(SB_MED_SD);
    CHECK(sbfw_settings_write(&s) == SBFW_E_NOHOME, "no home: refused");
    CHECK(sbfw_request_arm("linux-mainline", SBFW_REQ_SINGLE) == SBFW_E_NOHOME, "no request");
    CHECK(sbfw_crash_count() == SBFW_E_NOLOG, "no crash log");
    CHECK(sd_writes == 0 && ufs_writes == 0, "nothing written");
}

/* sbfw_api.c: one-shot requests and the mailbox keys */
static void t_api_request(void)
{
    media(1, SET_SD_STORE);
    table(STORE_SD, C_FIRST, SBFW_TF_CARD_ONLY, 0);
    sb_fw_read();
    uint8_t *b2 = card + 2 * 4096;
    unsigned a = 9, st = 9;
    char k[24];
    CHECK(sbfw_request_status(&a, &st, k) == 513 && a == 3 && st == SBFW_ST_ARMED && !strcmp(k, "linux-pmos"), "the fixture's request");
    /* the loader's table of what fits (sbfmt.c sbfw_req_fits) */
    static const char want[9][6] = { { 0, 0, 0, 0, 0, 0 }, { 0, 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 0, 1 }, { 0, 0, 1, 0, 0, 0 },
                                     { 0, 0, 1, 0, 0, 0 }, { 0, 1, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 1, 0 },
                                     { 0, 0, 0, 0, 0, 0 } };
    int fits_ok = 1;
    for (unsigned q = 0; q < 9; ++q)
        for (unsigned m = 0; m < 6; ++m)
            fits_ok &= sbfw_req_fits(q, m) == want[q][m];
    CHECK(fits_ok && !sbfw_req_fits(9, 2), "sbfw_req_fits: the loader's table");
    CHECK(!strcmp(sbfw_req_name(SBFW_REQ_XNU_SINGLE), "xnu-single") && !sbfw_req_name(0) && !sbfw_req_name(9), "names");

    auth_store(SB_MED_SD);
    CHECK(sbfw_request_arm("win11", SBFW_REQ_WINRE) == SBFW_E_INVAL, "WinRE: plan only");
    CHECK(sbfw_request_arm("starbite-os", SBFW_REQ_NO_SHELL) == SBFW_E_INVAL, "no-shell: not a start's request");
    CHECK(sbfw_request_arm("ios26", 0) == SBFW_E_INVAL && sbfw_request_arm("ios26", 9) == SBFW_E_INVAL, "unknown actions");
    CHECK(sbfw_request_arm("linux-mainline", SBFW_REQ_XNU_SINGLE) == SBFW_E_NOFIT, "XNU single user on Linux: no");
    CHECK(sbfw_request_arm("android", SBFW_REQ_SAFE) == SBFW_E_NOFIT, "safe mode on Android: no");
    CHECK(sbfw_request_arm("nothing", SBFW_REQ_VERBOSE) == SBFW_E_NOFIT, "an unknown key");
    CHECK(sd_writes == 0, "refusals write nothing");

    uint8_t before[4096];
    memcpy(before, b2, 4096);
    CHECK(sbfw_request_arm("linux-mainline", SBFW_REQ_PREV) == 0 && sd_writes == 1, "armed: previous kernel");
    CHECK(b2[116] == SBFW_REQ_PREV && b2[117] == SBFW_ST_ARMED && (b2[118] | b2[119] << 8) == 514 &&
          !strcmp((const char *)b2 + 120, "linux-mainline"), "req_action, req_state, req_seq + 1, req_key");
    CHECK(b2[115] == before[115] && !memcmp(b2 + 144, before + 144, 112) && !memcmp(b2 + 20, before + 20, 92),
          "the rest of the record kept");
    CHECK(sbfw_request_status(&a, &st, k) == 514 && a == SBFW_REQ_PREV && st == SBFW_ST_ARMED, "status");

    boot_key[0] = 0;
    if (!setjmp(boot_jb))
        sbfw_request("ios26", SBFW_REQ_XNU_SINGLE);
    CHECK(!strcmp(boot_key, "ios26") && b2[116] == SBFW_REQ_XNU_SINGLE && (b2[118] | b2[119] << 8) == 515 && sd_writes == 2,
          "sbfw_request: written, then the mailbox key");
    boot_key[0] = 0;
    int r = 0;
    if (!setjmp(boot_jb))
        r = sbfw_request("ios26", SBFW_REQ_SINGLE);
    CHECK(r == SBFW_E_NOFIT && !boot_key[0] && sd_writes == 2, "refused: no restart");

    /* an applied write action with its undo still pending: refused */
    struct star_settings ss;
    struct sbfw_rec fr;
    settings_load(&ss);
    settings_fw_load(&fr);
    fr.req_state = SBFW_ST_APPLIED;
    fr.undo[0] = 3;
    settings_fw_save(&ss, &fr);
    CHECK(sbfw_request_arm("linux-mainline", SBFW_REQ_SINGLE) == SBFW_E_BUSY, "an undo pending: busy");
    fr.undo[0] = 0;
    settings_fw_save(&ss, &fr);
    CHECK(sbfw_request_arm("linux-mainline", SBFW_REQ_SINGLE) == 0, "a command-line request applied: a new one is fine");
    sb_gate_close();

    if (!setjmp(boot_jb))
        sb_boot_menu_next();
    CHECK(!strcmp(boot_key, "menu"), "the boot menu once: \"menu\"");
    if (!setjmp(boot_jb))
        sb_boot_recovery_next();
    CHECK(!strcmp(boot_key, "recovery"), "Starbite Recovery: \"recovery\"");
}

/* sbfw_api.c: the crash reports, from the loader's own encoding */
static void t_api_crash(void)
{
    media(1, SET_SD_STORE);
    table(STORE_SD, C_FIRST, SBFW_TF_CARD_ONLY, 0);
    sb_fw_read();
    uint8_t *cl = card + (C_DATA + 1) * 4096;
    memset(cl, 0, 16 * 4096);
    CHECK(sbfw_crash_count() == SBFW_E_NOLOG, "an empty crash log: none");
    memcpy(cl, fx_crashlog, sizeof(fx_crashlog));
    CHECK(sbfw_crash_count() == 2, "two reports");
    struct sbfw_crash c;
    CHECK(sbfw_crash_get(0, &c) == 0, "the newest");
    CHECK(!strcmp(c.key, "linux-pmos") && !strcmp(c.os, "postmarketOS") && !strcmp(c.kind, "KERNEL PANIC") &&
          c.kind_id == 3 && c.severity == 2 && c.method == 2 && c.seq == 2, "a Linux panic");
    CHECK(c.epoch == 1794470123ULL && !strcmp(c.line, "panic: Attempted to kill init! exitcode=0x0000000b"), "event time, reason");
    CHECK(sbfw_crash_get(1, &c) == 0 && !strcmp(c.key, "ios26") && !strcmp(c.kind, "HANG (OS TIMER)") && c.seq == 1, "the older");
    CHECK(c.epoch == 1794400100ULL && !strcmp(c.line, "STARBITE: sweetreboot timer (no panic)"),
          "detected time, the first text line after \"[...]\": %s", c.line);
    CHECK(sbfw_crash_get(2, &c) == SBFW_E_INVAL && !c.key[0], "no third");
    /* the ring wrapped: 17 reports, next slot 1 -> 15 there, the newest in slot 0 */
    p32(cl + 24, 1);
    p32(cl + 28, 17);
    p32(cl + 12, sb_crc32(cl + 16, 1024 - 16));
    CHECK(sbfw_crash_count() == 15, "15 at most");
    CHECK(sbfw_crash_get(0, &c) == 0 && !strcmp(c.key, "ios26"), "slot 0 is the newest now");
    CHECK(sbfw_crash_get(1, &c) == -6, "slot 14: no report there");
    cl[4096 + 300] ^= 1;
    CHECK(sbfw_crash_get(0, &c) == -6, "a damaged report");
    cl[500] ^= 1;
    CHECK(sbfw_crash_count() == SBFW_E_NOLOG, "a damaged head: no crash log");
    CHECK(sd_writes == 0 && ufs_reads == 0, "read only, UFS untouched");
}

static int run(const char *name, void (*f)(void))
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        f();
        printf("  %-28s %d checks, %d failed\n", name, checks, fails);
        fflush(stdout);
        _exit(fails ? 1 : 0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("FAIL: %s\n", name);
        return 1;
    }
    return 0;
}

static void t_ufs_plain(void) { t_ufs(0, 0, 0); }
static void t_ufs_damaged(void) { t_ufs(1, 0, 1); }
static void t_ufs_sd_allowed(void) { t_ufs(0, SET_SD_STORE, 0); }

static void load(const char *dir, const char *name, uint8_t *dst)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f || fread(dst, 1, 4096, f) != 4096) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: sbfw-store-test FIXTURE_DIR [-v]\n");
        return 2;
    }
    verbose = argc > 2;
    load(argv[1], "sbfw-table.bin", fx_table);
    load(argv[1], "sbfw-settings.bin", fx_settings);
    for (int i = 0; i < 3; ++i) {
        char nm[64];
        static uint8_t blk[4096];
        snprintf(nm, sizeof(nm), "sbfw-crashlog.bin");
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", argv[1], nm);
        FILE *f = fopen(path, "rb");
        if (!f || fseek(f, 4096L * i, SEEK_SET) || fread(blk, 1, 4096, f) != 4096) {
            printf("cannot read %s\n", path);
            return 2;
        }
        fclose(f);
        memcpy(fx_crashlog + 4096 * i, blk, 4096);
    }
    int bad = 0;
    bad += run("fixtures", t_fixtures);
    bad += run("table: card only", t_card_only);
    bad += run("table: card home", t_card_home);
    bad += run("no table: HOME card", t_notable_card_home);
    bad += run("no table: UFS", t_ufs_plain);
    bad += run("damaged table: UFS", t_ufs_damaged);
    bad += run("no table: UFS + SET_SD_STORE", t_ufs_sd_allowed);
    bad += run("table: base mismatch", t_mismatch);
    bad += run("table: UFS", t_table_ufs);
    bad += run("api: settings", t_api_settings);
    bad += run("api: no home", t_api_nohome);
    bad += run("api: requests", t_api_request);
    bad += run("api: crash reports", t_api_crash);
    printf("sbfw-store-test: %d scenario(s) failed\n", bad);
    return bad ? 1 : 0;
}
