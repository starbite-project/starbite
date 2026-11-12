/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/update.c (Track A: Software Update) with libsb's REAL sbfw_api.c / sbfw.c (the
 * update-slot calls and the settings record's codec, Track J's) and the real SHA-256 (kern/passcode.c), around fakes:
 * a home store with "sbos-slot-a" / "sbos-slot-b" (small here) and the settings record in RAM, the fence's one grant,
 * the card's exFAT folder /STARBITE/UPDATE as files in RAM, the gate, the restart, notifications and prefs.
 * Checks: the manifest's parsing (good, wrong key, a bad sha256 field, a missing image, a size mismatch, oversize, no
 * loader table), the slot choice (active 0 / 1 / 2), chunked staging inside the grant with the header written last,
 * a SHA-256 mismatch (nothing armed, no header), arming (bytes 116..121 and 232..234 of the record, the restart),
 * the commit on the trial start, the loader's rollback (REFUSED: one notice), the dry run.
 * Run: sh starbite/sbos/kern/host/update-test.sh [-v]. Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "kern.h"
#include "store.h"
#include "catalog.h"
#include "sbfw_api.h"
#include "passcode.h"

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
void sb_log(const char *s) { if (verbose) printf("    log: %s\n", s); }
void sb_logf(const char *a, const char *b) { if (verbose) printf("    log: %s%s\n", a, b); }
void sb_logx(const char *a, uint64_t v) { if (verbose) printf("    log: %s0x%llx\n", a, (unsigned long long)v); }

/* ---- the catalog (sbfw_api.c) ------------------------------------------------------------------------------------ */
static struct cat_ent fcat[2];
unsigned cat_count(void) { return 2; }
const struct cat_ent *cat_get(unsigned i) { return i < 2 ? &fcat[i] : 0; }
int cat_find(const char *key)
{
    for (unsigned i = 0; i < 2; i++)
        if (!strcmp(fcat[i].key, key)) return (int)i;
    return -1;
}
void sb_boot_next(const char *key)
{
    printf("FAIL: sb_boot_next(%s): update.c restarts with power_boot_os\n", key);
    exit(2);
}

/* ---- the home store: the settings record and the two slots -------------------------------------------------- */
#define SLOT_BYTES (1u << 20)                   /* 1 MiB entries here (64 MiB on the phone) */
#define LBA_A 1000u
#define LBA_B 5000u
static uint8_t rec[4096];                       /* the settings record (block 2) */
static int rec_ok, rec_writes;
static uint8_t *slotmem[3];                     /* [1] A, [2] B */
static int have_slots, gate_on, gate_opens;
static struct { uint64_t lba, n; } wlog[256];   /* the slot writes, in order */
static int nw;
static uint64_t g_first, g_n;                   /* the fence's grant */
static int g_on, lends, wr_refused;

int store_home(void) { return 1; }
static void ent_of(unsigned s, struct store_ent *e)
{
    memset(e, 0, sizeof(*e));
    strcpy(e->name, s == 2 ? "sbos-slot-b" : "sbos-slot-a");
    e->lba = s == 2 ? LBA_B : LBA_A;
    e->size = SLOT_BYTES;
    e->flags = 1;
    e->medium = STORE_SD;
}
int store_find(const char *name, struct store_ent *e)
{
    if (!have_slots) return 0;
    if (!strcmp(name, "home:sbos-slot-a")) { ent_of(1, e); return 1; }
    if (!strcmp(name, "home:sbos-slot-b")) { ent_of(2, e); return 1; }
    return 0;
}
static uint8_t *at(uint64_t lba)
{
    if (lba >= LBA_A && lba < LBA_A + SLOT_BYTES / 4096) return slotmem[1] + (lba - LBA_A) * 4096;
    if (lba >= LBA_B && lba < LBA_B + SLOT_BYTES / 4096) return slotmem[2] + (lba - LBA_B) * 4096;
    return 0;
}
int store_read_block(const struct store_ent *e, uint64_t k, void *dst)
{
    uint8_t *p = at(e->lba + k);
    if (!p) return -1;
    memcpy(dst, p, 4096);
    return 0;
}
int sb_blk_read(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, void *dst)
{
    (void)medium; (void)lu;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t *p = at(lba + i);
        if (!p) return -1;
        memcpy((uint8_t *)dst + i * 4096, p, 4096);
    }
    return 0;
}
int store_volatile_grant_part(const char *name, uint64_t min_bytes, uint64_t first, uint64_t count, struct store_ent *e)
{
    g_on = 0;
    if (!store_find(strcmp(name, "sbos-slot-b") ? "home:sbos-slot-a" : "home:sbos-slot-b", e) || e->size < min_bytes)
        return 0;
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
    (void)medium;
    if (!g_on || lba < g_first || lba + n > g_first + g_n) { wr_refused++; return SB_W_RANGE; }
    for (uint64_t i = 0; i < n; i++) memcpy(at(lba + i), (const uint8_t *)src + i * 4096, 4096);
    if (nw < 256) { wlog[nw].lba = lba; wlog[nw].n = n; nw++; }
    return 0;
}
void hw_slot_lend(void) { lends++; }

/* the record, as store.c keeps it: the SBFW part through the real codec (sbfw.c), the CRC over 16..255 */
int settings_load(struct star_settings *s) { memset(s, 0, sizeof(*s)); return rec_ok; }
int settings_fw_load(struct sbfw_rec *r)
{
    if (!rec_ok) { memset(r, 0, sizeof(*r)); return 0; }
    sbfw_rec_decode(rec, r);
    return 1;
}
int settings_fw_save(struct star_settings *s, const struct sbfw_rec *r)
{
    (void)s;
    if (!gate_on) return SB_W_AUTH;             /* the gate is the authorisation */
    memcpy(rec, "SBSETTNG", 8);
    rec[8] = 1;
    sbfw_rec_encode(rec, r);
    sbset_crc_fix(rec);
    rec_ok = 1;
    rec_writes++;
    return 0;
}
int settings_ldr_load(uint8_t ldr[8]) { memset(ldr, 0, 8); return rec_ok; }
int settings_all_save(struct star_settings *s, const struct sbfw_rec *r, const uint8_t ldr[8]) { (void)ldr; return settings_fw_save(s, r); }

/* the gate */
void sb_auth_init(struct sb_auth *a, unsigned space, unsigned lu, uint64_t first, uint64_t count, int tier, unsigned kind, const char *what)
{
    (void)space; (void)lu; (void)first; (void)count; (void)tier; (void)kind; (void)what;
    memset(a, 0, sizeof(*a));
}
void sb_auth_step(struct sb_auth *a, unsigned step) { a->steps |= step; }
unsigned sb_steps_needed(int tier, unsigned kind) { (void)tier; (void)kind; return 1; }
int sb_gate_open(const struct sb_auth *a) { gate_on = a->steps != 0; gate_opens++; return gate_on; }
void sb_gate_close(void) { gate_on = 0; }

/* ---- the card: /STARBITE/UPDATE ------------------------------------------------------------------------------- */
static char man[2048];
static uint8_t *img;
static uint32_t img_n, list_n;                  /* the file's bytes; its size in the listing */
static char img_name[64];
static int card_in = 1, card_reads;
int kfile_mount(void) { return card_in ? 1 : -1; }
int kfile_state(const char **why) { if (why) *why = card_in ? "" : "no card"; return card_in ? 1 : -1; }
int kfile_list_at(const char *path, struct kfile_ent *out, int max, int first)
{
    if (strcmp(path, "/STARBITE/UPDATE") || first > 0 || max < 2) return first > 0 ? 0 : -1;
    memset(out, 0, 2 * sizeof(*out));
    strcpy(out[0].name, "MANIFEST.TXT");
    out[0].size = strlen(man);
    if (!img_name[0]) return 1;
    strcpy(out[1].name, img_name);
    out[1].size = list_n;
    return 2;
}
int kfile_read_at(const char *path, uint64_t off, void *dst, uint32_t len)
{
    card_reads++;
    if (len > (64u << 10)) { printf("FAIL: a card read of %u bytes (64 KiB at most)\n", len); fails++; }
    const uint8_t *src = 0;
    uint64_t n = 0;
    if (!strcmp(path, "/STARBITE/UPDATE/MANIFEST.TXT") && man[0]) { src = (const uint8_t *)man; n = strlen(man); }
    else if (img_name[0] && !strncmp(path, "/STARBITE/UPDATE/", 17) && !strcasecmp(path + 17, img_name)) { src = img; n = img_n; }
    if (!src) return SBOS_EINVAL;
    if (off >= n) return 0;
    uint64_t k = n - off < len ? n - off : len;
    memcpy(dst, src + off, k);
    return (int)k;
}

/* ---- the rest of the kernel ------------------------------------------------------------------------------------ */
static char booted[24];
static int boots, notifs;
static char last_notif[96];
void power_boot_os(const char *key) { snprintf(booted, sizeof(booted), "%s", key); boots++; }
void notif_post(const char *who, const char *title, const char *text)
{
    (void)who; (void)title;
    notifs++;
    snprintf(last_notif, sizeof(last_notif), "%s", text);
}
static char pref_v[32];
int prefs_get(const char *key, char *val, int cap)
{
    if (strcmp(key, "sbos.update.seen") || !pref_v[0]) return -1;
    snprintf(val, (size_t)cap, "%s", pref_v);
    return (int)strlen(val);
}
int prefs_set(const char *key, const char *val)
{
    if (strcmp(key, "sbos.update.seen")) return -1;
    int ch = strcmp(pref_v, val) != 0;
    snprintf(pref_v, sizeof(pref_v), "%s", val);
    return ch;
}
static int changed;
void settings_changed(void) { changed++; }

/* ---- helpers ------------------------------------------------------------------------------------------------------ */
static void hexs(char *o, const uint8_t h[32]) { for (int i = 0; i < 32; i++) snprintf(o + 2 * i, 3, "%02x", h[i]); }
static void make_image(uint32_t n, uint32_t seed)
{
    free(img);
    img = malloc(n);
    for (uint32_t i = 0; i < n; i++) img[i] = (uint8_t)((i * 2654435761u + seed) >> 11);
    img_n = list_n = n;
    strcpy(img_name, "next-1.0.1.img");
}
static void write_manifest(const char *key, const char *ver, int bad_sha)
{
    uint8_t h[32];
    char hx[65];
    pass_sha256(img, img_n, h);
    if (bad_sha) h[5] ^= 1;
    hexs(hx, h);
    snprintf(man, sizeof(man), "# Starbite OS update\r\nkey=%s\r\nversion = %s\nimage=%s\nsize=%u\nsha256=%s\nnotes=Faster Wi-Fi; fixes.\n",
             key, ver, img_name, img_n, hx);
}
/* a fresh phone: a loader table for starbite-next, slots, the record with slot_active = act */
static void reset(unsigned act)
{
    memset(&sb_fw, 0, sizeof(sb_fw));
    sb_fw.valid = 1;
    strcpy(sb_fw.key, "starbite-next");
    memset(rec, 0, sizeof(rec));
    rec_ok = 0;
    have_slots = 1;
    for (int s = 1; s <= 2; s++) { free(slotmem[s]); slotmem[s] = calloc(1, SLOT_BYTES); }
    nw = lends = wr_refused = boots = notifs = changed = gate_opens = rec_writes = 0;
    g_on = gate_on = 0;
    booted[0] = last_notif[0] = pref_v[0] = 0;
    card_in = 1;
    kupdate_dry(0);
    kupdate_reset();
    if (act) {
        gate_on = 1;
        sbfw_slot_set(act, 0, 0);
        gate_on = 0;
        rec_writes = 0;
    }
    make_image(300000, act);
    write_manifest("starbite-next", "1.0.1", 0);
}
/* stage to the end (cap bytes a call): the final state */
static int stage_all(struct kupdate *u, uint32_t cap, int *calls)
{
    static uint8_t buf[4u << 20];
    int st = u->state, n = 0;
    while (n < 1000) {
        st = kupdate_stage(u, buf, cap);
        n++;
        if (st != KUP_STAGING) break;
    }
    if (calls) *calls = n;
    return st;
}
static void rec_fields(unsigned *act, unsigned *try_, unsigned *tries, unsigned *action, unsigned *state, char key[24])
{
    struct sbfw_rec r;
    sbfw_rec_decode(rec, &r);
    *act = r.slot_active; *try_ = r.slot_try; *tries = r.slot_tries;
    *action = r.req_action; *state = r.req_state;
    memcpy(key, r.req_key, 24);
}

/* the loader's next start (its half, Track J's): applies the armed request (the trial), or rolls back */
static void loader_start(int trial_ok)
{
    struct sbfw_rec r;
    sbfw_rec_decode(rec, &r);
    gate_on = 1;
    if (trial_ok) {
        r.req_state = SBFW_ST_APPLIED;
        sb_fw.req_action = SBFW_REQ_UPDATE;
    } else {
        r.slot_try = 0;
        r.req_state = SBFW_ST_REFUSED;
        sb_fw.req_action = 0;
    }
    struct star_settings st;
    settings_fw_save(&st, &r);
    gate_on = 0;
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    strcpy(fcat[0].key, "starbite-next");
    fcat[0].method = M_BARE;
    strcpy(fcat[1].key, "linux-mainline");
    fcat[1].method = M_LINUX;
    struct kupdate u;

    /* ---- the SHA-256 in pieces (passcode.c's pass_sha_*) against the one-shot ---- */
    reset(0);
    {
        uint8_t a[32], b[32];
        struct pass_sha c;
        pass_sha256(img, img_n, a);
        pass_sha_init(&c);
        uint32_t off = 0, step = 1;
        while (off < img_n) {
            uint32_t k = img_n - off < step ? img_n - off : step;
            pass_sha_update(&c, img + off, k);
            off += k;
            step = step * 3 + 7;
        }
        pass_sha_final(&c, b);
        CHECK(!memcmp(a, b, 32), "SHA-256 in odd pieces = in one");
        pass_sha256("abc", 3, a);
        static const uint8_t abc[4] = { 0xba, 0x78, 0x16, 0xbf };
        CHECK(!memcmp(a, abc, 4), "SHA-256(\"abc\") = ba7816bf...");
        CHECK(sb_crc32_more(sb_crc32("1234", 4), "56789", 5) == 0xcbf43926U, "CRC32 in parts (zlib's check value)");
    }

    /* ---- the manifest ---- */
    reset(0);
    CHECK(kupdate_parse(man, (int)strlen(man), &u) == 0 && !strcmp(u.key, "starbite-next") && !strcmp(u.version, "1.0.1") &&
          !strcmp(u.image, "next-1.0.1.img") && u.size == 300000 && !strcmp(u.notes, "Faster Wi-Fi; fixes."),
          "a good manifest parses (CRLF, spaces around '=', a comment)");
    {
        const char *bad[] = { "key=a\nversion=1\nimage=x.img\nsize=10\n",                                         /* no sha256 */
                              "key=a\nversion=1\nimage=x.img\nsize=1x\nsha256=" "00000000000000000000000000000000"
                              "00000000000000000000000000000000\n",
                              "key=a\nversion=1\nimage=../x.img\nsize=10\nsha256=" "00000000000000000000000000000000"
                              "00000000000000000000000000000000\n",
                              "key=a\nversion=1\nimage=x.img\nsize=10\nsha256=abc\n",
                              "key=a\nversion=1\nimage=x.img\nsize=10\nsha256=" "0000000000000000000000000000000g"
                              "00000000000000000000000000000000\n" };
        for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
            CHECK(kupdate_parse(bad[i], (int)strlen(bad[i]), &u) < 0, "a broken manifest (%u) is refused", i);
    }
    CHECK(kupdate_check(&u) == 1 && u.state == KUP_FOUND && !strcmp(u.version, "1.0.1"), "check: found");
    reset(0);
    write_manifest("linux-mainline", "1.0.1", 0);
    CHECK(kupdate_check(&u) == 0 && u.state == KUP_NONE, "an update for another row is not offered");
    reset(0);
    strcpy(man, "key=starbite-next\nversion=1\nimage=x.img\nsize=10\nsha256=zz\n");
    CHECK(kupdate_check(&u) == 0 && u.state == KUP_FAILED, "a bad sha256 field: failed (%s)", u.why);
    reset(0);
    img_name[0] = 0;
    CHECK(kupdate_check(&u) == 0 && u.state == KUP_FAILED && strstr(u.why, "missing"), "the image is missing: %s", u.why);
    reset(0);
    list_n = img_n + 1;
    CHECK(kupdate_check(&u) == 0 && u.state == KUP_FAILED && strstr(u.why, "size"), "the size differs: %s", u.why);
    reset(0);
    make_image(SLOT_BYTES - 4095, 7);
    write_manifest("starbite-next", "1.0.1", 0);
    CHECK(kupdate_check(&u) == 0 && u.state == KUP_FAILED && strstr(u.why, "larger"), "oversize: %s", u.why);
    reset(0);
    make_image(SLOT_BYTES - 4096, 7);
    write_manifest("starbite-next", "1.0.1", 0);
    CHECK(kupdate_check(&u) == 1, "exactly the slot's room: found");
    reset(0);
    sb_fw.valid = 0;
    CHECK(kupdate_check(&u) == 0 && u.state == KUP_FAILED && strstr(u.why, "new boot loader"), "an older loader: %s", u.why);
    reset(0);
    card_in = 0;
    CHECK(kupdate_check(&u) == 0 && u.state == KUP_NONE, "no card: none found");
    reset(0);
    have_slots = 0;
    CHECK(kupdate_check(&u) == 0 && u.state == KUP_FAILED, "no slot entries: %s", u.why);

    /* ---- the slot choice ---- */
    CHECK(kupdate_target(0) == 1 && kupdate_target(1) == 2 && kupdate_target(2) == 1, "inactive slot: 0 -> A, 1 -> B, 2 -> A");

    /* ---- staging: chunks inside the grant, the header last, read back ---- */
    for (unsigned act = 0; act <= 2; act++) {
        reset(act);
        CHECK(kupdate_check(&u) == 1, "active %u: found", act);
        int calls = 0;
        int st = stage_all(&u, 128u << 10, &calls);
        unsigned want = kupdate_target(act);
        uint64_t lba0 = want == 2 ? LBA_B : LBA_A;
        CHECK(st == KUP_READY && u.pct == 100, "active %u: staged (%d calls)", act, calls);
        CHECK(!wr_refused && nw == 4, "active %u: 3 image chunks + the header, all inside their grant (%d writes)", act, nw);
        CHECK(nw >= 2 && wlog[nw - 1].lba == lba0 && wlog[nw - 1].n == 1, "active %u: the header (block 0 of slot %u) written last", act, want);
        int img_first = 1;
        for (int i = 0; i + 1 < nw; i++) img_first &= wlog[i].lba > lba0;
        CHECK(img_first && wlog[0].lba == lba0 + 1, "active %u: the image from +4096", act);
        CHECK(!memcmp(slotmem[want] + 4096, img, img_n), "active %u: the image's bytes in slot %u", act, want);
        struct sbslot_info si;
        CHECK(sbslot_check(slotmem[want], SLOT_BYTES, "starbite-next", &si) && si.size == img_n &&
              si.crc == sb_crc32(img, img_n) && !strcmp(si.vstr, "1.0.1"), "active %u: the header checks (CRC32 of the image)", act);
        CHECK(lends == nw, "active %u: every window lent the state slot's grant", act);
        uint8_t *other = slotmem[3 - want];
        int clean = 1;
        for (uint32_t i = 0; i < SLOT_BYTES; i++) clean &= !other[i];
        CHECK(clean, "active %u: the active slot untouched", act);
        CHECK(rec_writes == 0, "active %u: staging writes no settings", act);
    }
    /* the header written last: a stage stopped half way leaves no header */
    reset(0);
    kupdate_check(&u);
    {
        static uint8_t buf[128u << 10];
        kupdate_stage(&u, buf, sizeof(buf));
        CHECK(u.state == KUP_STAGING && u.pct > 0 && u.pct < 90, "one chunk: still staging (%d %%)", u.pct);
        struct sbslot_info si;
        CHECK(!sbslot_check(slotmem[1], SLOT_BYTES, "starbite-next", &si), "no header yet");
        char why[80];
        CHECK(kupdate_install(why, sizeof(why)) < 0 && !boots, "install refused while staging");
    }

    /* ---- a SHA-256 mismatch: nothing armed, no header ---- */
    reset(0);
    write_manifest("starbite-next", "1.0.1", 1);
    kupdate_check(&u);
    CHECK(stage_all(&u, 128u << 10, 0) == KUP_FAILED && strstr(u.why, "SHA-256"), "sha mismatch: failed (%s)", u.why);
    {
        struct sbslot_info si;
        int hdr = 0;
        for (int i = 0; i < nw; i++) hdr |= wlog[i].lba == LBA_A;
        CHECK(!hdr && !sbslot_check(slotmem[1], SLOT_BYTES, "starbite-next", &si), "sha mismatch: the header never written");
        char why[80];
        CHECK(kupdate_install(why, sizeof(why)) < 0 && !boots && !rec_writes && !rec_ok, "sha mismatch: nothing armed, no restart");
    }

    /* ---- arming, the trial start's commit ---- */
    reset(2);
    kupdate_check(&u);
    stage_all(&u, 1u << 20, 0);
    {
        char why[80], key[24];
        CHECK(kupdate_install(why, sizeof(why)) < 0 && boots == 1 && !strcmp(booted, "starbite-next"),
              "install: armed, then restarted into this row (the fake comes back)");
        unsigned act, tr, tries, action, state;
        rec_fields(&act, &tr, &tries, &action, &state, key);
        CHECK(act == 2 && tr == 1 && tries == 1, "the record: slot_active 2 kept, slot_try 1 (A), tries 1");
        CHECK(rec[232] == 2 && rec[233] == 1 && rec[234] == 1 && rec[235] == 0, "bytes 232..235: 2 1 1 0");
        CHECK(action == SBFW_REQ_UPDATE && state == SBFW_ST_ARMED && !strcmp(key, "starbite-next") && rec[116] == 9 && rec[117] == 1,
              "the one-shot: req_action 9 armed for starbite-next");
        CHECK(!gate_on && gate_opens == 1, "the gate was opened for the one write and closed");
        CHECK(kupdate_after_start() == 0 && !notifs, "the same start: nothing to commit");
        loader_start(1);
        /* the new start: the shell's 10 s timer */
        CHECK(kupdate_after_start() == 1 && notifs == 1 && !strcmp(last_notif, "Updated to 1.0.1."), "committed: \"%s\"", last_notif);
        rec_fields(&act, &tr, &tries, &action, &state, key);
        CHECK(act == 1 && tr == 0 && state == SBFW_ST_DONE && rec[232] == 1 && rec[233] == 0, "slot_active 1, slot_try 0, the request done");
        CHECK(kupdate_committed() && kupdate_check(&u) == 0 && u.state == KUP_INSTALLED && !strcmp(u.version, "1.0.1"),
              "check after the commit: installed 1.0.1");
        CHECK(kupdate_after_start() == 0 && notifs == 1, "the commit only once");
    }
    /* the same manifest once more, on a later start: up to date */
    {
        unsigned keep_act = rec[232];
        uint8_t *a = slotmem[1];
        slotmem[1] = 0;
        reset(keep_act);
        free(slotmem[1]);
        slotmem[1] = a;                          /* slot A still holds 1.0.1 */
        CHECK(kupdate_check(&u) == 0 && u.state == KUP_NONE && strstr(u.why, "installed"), "the installed version is not offered again");
    }

    /* ---- the loader's rollback: REFUSED, one notice ---- */
    reset(0);
    kupdate_check(&u);
    stage_all(&u, 1u << 20, 0);
    {
        char why[80], key[24];
        kupdate_install(why, sizeof(why));
        loader_start(0);
        unsigned act, tr, tries, action, state;
        rec_fields(&act, &tr, &tries, &action, &state, key);
        CHECK(act == 0 && tr == 0 && state == SBFW_ST_REFUSED, "rolled back: slot_active 0, slot_try 0, REFUSED");
        CHECK(kupdate_after_start() == 2 && notifs == 1 && strstr(last_notif, "previous version was restored") && changed == 1,
              "the notice: \"%s\"", last_notif);
        CHECK(kupdate_after_start() == 0 && notifs == 1, "said once (the seen request kept in prefs)");
        CHECK(rec[232] == 0, "nothing committed");
    }

    /* ---- the dry run: no writes, no arming, no restart ---- */
    reset(0);
    kupdate_check(&u);
    kupdate_dry(1);
    int reads0 = card_reads;
    CHECK(stage_all(&u, 1u << 20, 0) == KUP_READY && !nw && !lends && card_reads == reads0, "dry: staged without a read or a write");
    {
        char why[80];
        CHECK(kupdate_install(why, sizeof(why)) == 0 && !boots && !rec_writes, "dry: install only logs");
    }

    printf("update-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
