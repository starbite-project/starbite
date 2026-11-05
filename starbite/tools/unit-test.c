/* SPDX-License-Identifier: GPL-2.0-only
 * Host unit tests for libsb's and Engineer OS's pure logic (starbite/engineer/build.sh runs them in
 * the aarch64 VM before it links the image; a failure stops the build):
 *
 *   str.c        the string builder, number formatting, sb_crc32
 *   mmu.c        sb_map_kind: the identity map's attributes (RAM test window non-cacheable, the
 *                secure hole unmapped)
 *   policy.c     Register peek / poke policy: every CMU block, MIF/INT, PMU, ACPM, fuses and DRAM
 *                are never pokeable; only the five allowlisted blocks are tier 1
 *   fence.c      the write fence on an S9-like GPT: never-touch partitions, tiers, ranges, and
 *                that sb_write never reports a write (v1 is dry-run, whatever the tier)
 *   ufs.c        the SCSI opcode allowlist: only TEST UNIT READY, INQUIRY, START STOP UNIT and
 *                READ(10); a WRITE is refused before any register is touched
 *   sbui_input.c the key state machine: Power selects on release, Power held 0.6 s is back,
 *                Bixby short / long, Volume repeat, keys held across a reset are ignored
 *
 *   gcc -O1 -Ilibsb -Iengineer -o unit-test tools/unit-test.c libsb/str.c libsb/fence.c
 *       libsb/sbui_input.c libsb/mmu.c engineer/policy.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ufs.c's gpt_load reads the hardware: rename it and let the fence test bring its own GPT. */
#define gpt_load ufs_real_gpt_load
#include "../libsb/ufs.c"
#undef gpt_load
#include "eng.h"

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

/* ---- stubs for what the units under test call ------------------------------------------------- */

static uint64_t now_us = 1000000;
static unsigned keys;
uint64_t sb_ticks(void) { return now_us * 26U; }
uint64_t sb_freq(void) { return 26000000U; }
uint64_t sb_us(void) { return now_us; }
uint64_t sb_ms(void) { return now_us / 1000U; }
void sb_udelay(uint64_t us) { now_us += us; }
void sb_mdelay(unsigned ms) { now_us += (uint64_t)ms * 1000U; }
unsigned sb_keys(void) { return keys; }
int sb_touch_event(int *st, int *x, int *y) { (void)st; (void)x; (void)y; return 0; }
void sb_touch_clear(void) {}
void sb_dcache_all(unsigned op) { (void)op; }
static char last_log[256];
void sb_log(const char *s) { snprintf(last_log, sizeof(last_log), "%s", s); }

int gpt_load(void)
{
    static const struct { const char *n; uint64_t f, l; } g[] = {
        { "BOTA0", 6, 261 }, { "BOTA1", 262, 517 }, { "EFS", 518, 5637 }, { "PARAM", 5638, 7685 },
        { "BOOT", 7686, 17925 }, { "RECOVERY", 17926, 29189 }, { "SYSTEM", 29190, 1207813 },
        { "VENDOR", 1207814, 1310213 }, { "CACHE", 1310214, 1361413 }, { "USERDATA", 1361414, 15000000 },
    };
    gpt_n = (int)(sizeof(g) / sizeof(g[0]));
    for (int i = 0; i < gpt_n; ++i) {
        snprintf(gpt_name[i], sizeof(gpt_name[i]), "%s", g[i].n);
        gpt_first[i] = g[i].f;
        gpt_last[i] = g[i].l;
    }
    return gpt_n;
}

/* ---- str.c ------------------------------------------------------------------------------------ */

static const char *fmt_dec(uint64_t v) { static struct ub u; ub_init(&u); ub_dec(&u, v); return u.b; }

static void test_str(void)
{
    struct ub u;
    CHECK(!strcmp(fmt_dec(0), "0"), "ub_dec 0");
    CHECK(!strcmp(fmt_dec(18446744073709551615ULL), "18446744073709551615"), "ub_dec max");
    ub_init(&u); ub_sdec(&u, -42); CHECK(!strcmp(u.b, "-42"), "ub_sdec -42: %s", u.b);
    ub_init(&u); ub_hex(&u, 0xa0100000ULL, 9); CHECK(!strcmp(u.b, "0x0A0100000"), "ub_hex: %s", u.b);
    ub_init(&u); ub_hexn(&u, 0xbeef, 4); CHECK(!strcmp(u.b, "BEEF"), "ub_hexn: %s", u.b);
    ub_init(&u); ub_dec2(&u, 7); CHECK(!strcmp(u.b, "07"), "ub_dec2: %s", u.b);
    ub_init(&u); ub_fix(&u, 3850, 3); CHECK(!strcmp(u.b, "3.850"), "ub_fix: %s", u.b);
    ub_init(&u); ub_size(&u, 126ULL << 20); CHECK(!strcmp(u.b, "126 MiB"), "ub_size: %s", u.b);
    ub_init(&u);
    for (int i = 0; i < 400; ++i)
        ub_c(&u, 'x');
    CHECK(u.n < sizeof(u.b) && u.b[u.n] == 0 && strlen(u.b) == u.n, "ub overflow kept terminated (n %u)", u.n);
    char small[6];
    sb_strcpy(small, sizeof(small), "Engineer");
    CHECK(!strcmp(small, "Engin"), "sb_strcpy truncates: %s", small);
    CHECK(sb_prefix("SBENG1 T PASS", "SBENG1 T ") && !sb_prefix("SBENG", "SBENG1"), "sb_prefix");
    CHECK(sb_streq("BOOT", "BOOT") && !sb_streq("BOOT", "BOOTX"), "sb_streq");
    CHECK(sb_crc32("123456789", 9) == 0xcbf43926U, "sb_crc32 check value %08x", sb_crc32("123456789", 9));
}

/* ---- mmu.c sb_map_kind ------------------------------------------------------------------------ */

static void test_map(void)
{
    CHECK(sb_map_kind(0x14060000ULL) == 1, "PMU is device memory");
    CHECK(sb_map_kind(0x92080000ULL) == 2, "the image is write-back");
    CHECK(sb_map_kind(SB_TEST_BASE) == 3 && sb_map_kind(SB_TEST_BASE + SB_TEST_SIZE - 1) == 3,
          "the RAM test window is non-cacheable end to end");
    CHECK(sb_map_kind(SB_DMA_BASE) == 3, "UFS DMA non-cacheable");
    for (uint64_t a = 0xaf800000ULL; a < 0xc0000000ULL; a += 0x200000ULL)
        CHECK(sb_map_kind(a) == 0, "secure hole %llx must be unmapped", (unsigned long long)a);
    CHECK(sb_map_kind(SB_FB) == 3, "framebuffer non-cacheable / write-combined");
    CHECK(sb_map_kind(0x900000000ULL) == 0, "beyond bank 2 unmapped");
}

/* ---- policy.c --------------------------------------------------------------------------------- */

static void test_policy(void)
{
    const char *why;
    static const uint64_t never_at[] = {
        0x02000000, 0x02080000,                 /* APM SRAM */
        0x10000000, 0x10000014,                 /* CHIPID / fuses */
        0x10020000, 0x10400000, 0x10800000, 0x11000000, 0x11400000, 0x14000000, 0x14200000,
        0x16000000, 0x17400000, 0x17c00000, 0x1d000000, 0x1d100000,   /* CMU blocks */
        0x14060000, 0x1406330c, 0x14064044,     /* PMU */
        0x14100000,                             /* ACPM mailbox */
        0x1a000000, 0x1a240000, 0x1a2410a8, 0x1a3ffffc,   /* CMU_TOP / CORE / BUSC */
        0x1b800000, 0x1b800120, 0x1bc00000, 0x1bfffffc,   /* MIF, DREX / DMC */
        0x80000000, SB_TEST_BASE, 0xa0000000, 0x8a000000, 0x880000000ULL, 0x8fffffffcULL,  /* DRAM */
        0x14050000,                             /* GPIO_ALIVE (keys): not on the allowlist */
        0x16080000,                             /* DSIM */
        0x12345678,
    };
    for (unsigned i = 0; i < sizeof(never_at) / sizeof(never_at[0]); ++i) {
        why = 0;
        CHECK(poke_tier(never_at[i], &why) == TIER_NEVER && why, "poke %llx must be never",
              (unsigned long long)never_at[i]);
    }
    static const uint64_t ok_at[] = { 0x10430000, 0x10430ffc, 0x104b0000, 0x11430000, 0x14220000, 0x14220180,
                                      0x14360000 };
    for (unsigned i = 0; i < sizeof(ok_at) / sizeof(ok_at[0]); ++i)
        CHECK(poke_tier(ok_at[i], &why) == TIER1, "poke %llx is tier 1", (unsigned long long)ok_at[i]);
    CHECK(poke_tier(0x10431000, &why) == TIER_NEVER, "just past GPIO_PERIC0");
    /* the exclusions are named (not just "not on the allowlist"): MIF/INT and CMU stay explicit */
    CHECK(poke_tier(0x1bfffffc, &why) == TIER_NEVER && strstr(why, "MIF"), "MIF end named: %s", why);
    CHECK(poke_tier(0x1a3ffffc, &why) == TIER_NEVER && strstr(why, "CMU_TOP"), "CMU_TOP end named: %s", why);
    CHECK(poke_tier(0x1406330c, &why) == TIER_NEVER && strstr(why, "PMU"), "PMU named: %s", why);
    CHECK(poke_tier(0xa0000000, &why) == TIER_NEVER && strstr(why, "DRAM"), "DRAM named: %s", why);
    /* Exhaustive over the register space in 4 KiB steps: tier 1 only inside the allowlist. */
    unsigned tier1 = 0;
    for (uint64_t a = 0; a < 0x20000000ULL; a += 0x1000) {
        int t = poke_tier(a, &why);
        CHECK(t == TIER_NEVER || t == TIER1, "poke tier %d at %llx", t, (unsigned long long)a);
        tier1 += t == TIER1;
    }
    CHECK(tier1 == 5, "tier-1 pages in the register space: %u (want 5)", tier1);
    /* reads */
    CHECK(peek_allowed(0x14060000) && peek_allowed(0x14050024) && peek_allowed(0x10000000), "peek PMU / keys / CHIPID");
    CHECK(!peek_allowed(0x14100000), "no peek of the ACPM mailbox");
    CHECK(!peek_allowed(0x02000000), "no peek of APM SRAM");
    CHECK(!peek_allowed(0xaf800000) && !peek_allowed(0xb8000000) && !peek_allowed(0xc0000000), "no peek of RKP / TIMA / seclog");
    CHECK(!peek_allowed(0xe1900000) && !peek_allowed(0xf0000000), "no peek of secure camera / modem");
    CHECK(peek_allowed(0xa0000000) && peek_allowed(0x92080000), "DRAM outside carve-outs is readable");
}

/* ---- fence.c ---------------------------------------------------------------------------------- */

static void test_fence(void)
{
    struct sb_wdecide d;
    CHECK(sb_write_decide(SB_MED_UFS, 600, 1, &d) == SB_W_NEVER && !strcmp(d.part, "EFS"), "EFS never");
    CHECK(sb_write_decide(SB_MED_UFS, 5700, 1, &d) == SB_W_NEVER, "PARAM never");
    CHECK(sb_write_decide(SB_MED_UFS, 10, 1, &d) == SB_W_NEVER, "BOTA0 never");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 1, &d) == SB_W_NEVER, "GPT head never");
    CHECK(sb_write_decide(SB_MED_UFS, 15000001, 1, &d) == SB_W_NEVER, "GPT tail never");
    CHECK(sb_write_decide(SB_MED_UFS, 7686, 1, &d) == 0 && d.tier == 2, "BOOT tier 2");
    CHECK(sb_write_decide(SB_MED_UFS, 20000, 1, &d) == 0 && d.tier == 2, "RECOVERY tier 2");
    CHECK(sb_write_decide(SB_MED_UFS, 30000, 8, &d) == 0 && d.tier == 1, "SYSTEM tier 1");
    CHECK(sb_write_decide(SB_MED_UFS, 1361414 + 4194306, 1, &d) == 0 && d.tier == 1, "USERDATA (store) tier 1");
    CHECK(sb_write_decide(SB_MED_UFS, 1310300, 1, &d) == SB_W_NEVER, "CACHE (not listed) never");
    CHECK(sb_write_decide(SB_MED_UFS, 17920, 10, &d) == SB_W_RANGE, "BOOT into RECOVERY: crosses");
    CHECK(sb_write_decide(SB_MED_UFS, 30000, 0, &d) == SB_W_RANGE, "empty write");
    CHECK(sb_write_decide(SB_MED_SD, 123, 1, &d) == 0 && d.tier == 1, "card tier 1");
    CHECK(sb_write_decide(7, 0, 1, &d) == SB_W_NEVER, "other media never");
    /* v1 is dry-run: sb_write never says it wrote, whatever the tier and target */
    static const uint64_t lbas[] = { 0, 6, 600, 5700, 7686, 20000, 30000, 1207814, 1310300, 5555555, 15000000,
                                     15000001, ~0ULL - 4 };
    for (unsigned m = 0; m < 3; ++m)
        for (int tier = 0; tier <= 3; ++tier)
            for (unsigned i = 0; i < sizeof(lbas) / sizeof(lbas[0]); ++i) {
                int r = sb_write(m, lbas[i], 1, 0, tier);
                CHECK(r < 0, "sb_write(m %u, lba %llu, tier %d) = %d: must refuse", m, (unsigned long long)lbas[i], tier, r);
                CHECK(strstr(last_log, "fence: would write") != 0, "sb_write logs the decision");
            }
    CHECK(sb_write(SB_MED_UFS, 30000, 1, 0, 1) == SB_W_DRYRUN, "armed tier 1 SYSTEM: dry run");
    CHECK(sb_write(SB_MED_UFS, 30000, 1, 0, 0) == SB_W_TIER, "unarmed SYSTEM: needs tier");
    CHECK(sb_write(SB_MED_UFS, 7686, 1, 0, 1) == SB_W_TIER, "tier 1 BOOT: needs tier 2");
    CHECK(sb_write(SB_MED_UFS, 600, 1, 0, 3) == SB_W_NEVER, "EFS at any tier: never");
}

/* ---- ufs.c opcode allowlist ---------------------------------------------------------------- */

static void test_ufs(void)
{
    unsigned allowed = 0;
    for (unsigned op = 0; op < 256; ++op)
        if (cdb_allowed((uint8_t)op)) {
            ++allowed;
            CHECK(op == 0x00 || op == 0x12 || op == 0x1b || op == 0x28, "opcode %02x allowed", op);
        }
    CHECK(allowed == 4, "allowed SCSI opcodes: %u (want 4)", allowed);
    if (allowed != 4)
        return;                                 /* a write opcode would reach the (unmapped) host */
    static const uint8_t writes[] = { 0x0a, 0x2a, 0x2e, 0x35, 0x3b, 0x41, 0x42, 0x55, 0x8a, 0x8e, 0x91, 0x93, 0xaa, 0xae };
    for (unsigned i = 0; i < sizeof(writes); ++i) {
        uint8_t cdb[16] = { writes[i] };
        /* refused before u_zero or any register write: no mapping exists here, so a touch would fault */
        CHECK(ufs_scsi(cdb, 10, 4096) == -6, "write opcode %02x must be refused", writes[i]);
    }
}

/* ---- sbui_input.c key state machine -------------------------------------------------------------- */

static int next_event(unsigned ms)
{
    struct ui_ev e;
    ui_poll(&e, ms);
    return e.type;
}

static int held_events[16], nheld;

static void hold(unsigned k, unsigned ms)
{
    keys |= k;
    uint64_t end = now_us + (uint64_t)ms * 1000U;
    while (now_us < end) {
        struct ui_ev e;
        ui_poll(&e, 0);
        if (e.type != EV_NONE && nheld < 16)
            held_events[nheld++] = e.type;      /* what came while the key was down */
        now_us += 2000;
    }
    keys &= ~k;
}

static void test_input(void)
{
    ui_input_reset();
    /* short Power: nothing while down, SELECT on release */
    nheld = 0;
    hold(SB_K_POWER, 200);
    CHECK(nheld == 0, "short Power: no event while held (%d)", nheld);
    CHECK(next_event(50) == EV_SELECT, "short Power selects on release");
    CHECK(next_event(50) == EV_NONE, "short Power: one event");
    /* long Power: BACK once while held, nothing on release */
    nheld = 0;
    hold(SB_K_POWER, 1700);
    CHECK(nheld == 1 && held_events[0] == EV_BACK, "long Power: one BACK while held (%d, %d)", nheld, held_events[0]);
    CHECK(next_event(50) == EV_NONE, "long Power: nothing on release");
    /* just under / over the threshold */
    nheld = 0;
    hold(SB_K_POWER, UI_POWER_LONG_MS - 60);
    CHECK(nheld == 0 && next_event(50) == EV_SELECT, "Power held %u ms still selects", UI_POWER_LONG_MS - 60);
    /* Bixby: short back, long home */
    nheld = 0;
    hold(SB_K_BIXBY, 150);
    CHECK(nheld == 0 && next_event(50) == EV_BACK, "short Bixby: back");
    nheld = 0;
    hold(SB_K_BIXBY, 1000);
    CHECK(nheld == 1 && held_events[0] == EV_HOME && next_event(50) == EV_NONE, "long Bixby: home only");
    /* Volume: one step, then repeat after 420 ms */
    nheld = 0;
    hold(SB_K_DOWN, 100);
    CHECK(nheld == 1 && held_events[0] == EV_DOWN, "Volume Down: one step");
    nheld = 0;
    hold(SB_K_UP, 1000);
    CHECK(nheld >= 6 && nheld <= 9 && held_events[0] == EV_UP, "Volume Up held 1 s: %d steps (repeat)", nheld);
    /* a key held across ui_input_reset is ignored until released */
    keys = SB_K_POWER;
    ui_input_reset();
    nheld = 0;
    hold(SB_K_POWER, 900);
    CHECK(nheld == 0 && next_event(50) == EV_NONE, "Power held across a reset: no select, no back");
    CHECK(next_event(50) == EV_NONE, "queue empty");
}

int main(void)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    test_str();
    test_map();
    test_policy();
    test_fence();
    test_ufs();
    test_input();
    printf("unit tests: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
