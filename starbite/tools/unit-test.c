/* SPDX-License-Identifier: GPL-2.0-only
 * Host unit tests for libsb's and Engineer OS's pure logic (starbite/engineer/build.sh runs them in
 * the aarch64 VM before it links the image; a failure stops the build):
 *
 *   str.c        the string builder, number formatting, sb_crc32
 *   mmu.c        sb_map_kind: the identity map's attributes (RAM test window non-cacheable, the
 *                secure hole unmapped)
 *   policy.c     the tiers: the "most dangerous" list (PMU, CMU_TOP / MIF / CPU / GPU clocks and
 *                DVFS, ACPM, fuses, TMU, PMIC voltages, MAX77705 configuration) is tier 3, peripheral
 *                clock gates and unknown blocks tier 2, known peripherals and free DRAM tier 1; only
 *                the physical facts are impossible (S-Boot's ECT, the running image, the secure
 *                hole); an exhaustive sweep of the register space
 *   fence.c      the write fence on an S9-like GPT (tiers per partition, boot LUs, the live store,
 *                ranges) and the authorisation gate: without a complete confirmation nothing is
 *                written at any tier; with one, exactly the confirmed range is; tier 3 storage needs
 *                the backup step; a modified or revoked authorisation is refused; dry run writes
 *                nothing; an exhaustive sweep over partitions x tiers x every subset of steps
 *   ufs.c / sd.c the read-only opcode list; the write paths refuse unless the fence runs them,
 *                before any register is touched
 *   kern.c       the stress kernels: deterministic, the same on every core, a corrupted result is
 *                caught
 *   adc.c        the vendor thermistor tables (S-Boot's own reading: ADC 1950 = 26 C)
 *   sbui_input.c the key state machine: Power selects on release, Power held 0.6 s is back,
 *                Bixby short / long, Volume repeat, keys held across a reset are ignored
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
int sb_sd_rails(uint8_t *a, uint8_t *b) { *a = *b = 0; return 0; }
static char last_log[256];
void sb_log(const char *s) { snprintf(last_log, sizeof(last_log), "%s", s); }

/* the fence's block writer: records, never touches hardware */
static int writes;
static unsigned w_med, w_lu;
static uint64_t w_lba, w_n;
int sb_blk_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    (void)src;
    ++writes;
    w_med = medium;
    w_lu = lu;
    w_lba = lba;
    w_n = n;
    return sb_fence_active() ? 0 : -99;
}

/* kern.c's worker helpers */
static struct sb_wslot slots[SB_NCORES];
struct sb_wslot *sb_smp_own_slot(unsigned w) { return w < SB_NCORES ? &slots[w] : 0; }
static int stop_after;
int sb_worker_stop(void) { return --stop_after < 0; }
void sb_worker_publish(struct sb_wslot *s) { s->beats++; }

static const struct { const char *n; uint64_t f, l; } g[] = {
    { "BOTA0", 6, 261 }, { "BOTA1", 262, 517 }, { "EFS", 518, 5637 }, { "PARAM", 5638, 7685 },
    { "BOOT", 7686, 17925 }, { "RECOVERY", 17926, 29189 }, { "SYSTEM", 29190, 1207813 },
    { "VENDOR", 1207814, 1310213 }, { "CACHE", 1310214, 1361413 }, { "FOO", 1361414, 1361513 },
    { "USERDATA", 1361514, 15000000 },
};
#define NG (sizeof(g) / sizeof(g[0]))

int gpt_load(void)
{
    gpt_n = (int)NG;
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
    CHECK(sb_map_kind(SB_IO_BUF) == 3 && sb_map_kind(SB_STORE_IO + (1U << 20) - 1) == 3,
          "the lab I/O and store buffers are in the non-cacheable DMA window");
    for (uint64_t a = 0xaf800000ULL; a < 0xc0000000ULL; a += 0x200000ULL)
        CHECK(sb_map_kind(a) == 0, "secure hole %llx must be unmapped", (unsigned long long)a);
    CHECK(sb_map_kind(SB_FB) == 3, "framebuffer non-cacheable / write-combined");
    CHECK(sb_map_kind(0x900000000ULL) == 0, "beyond bank 2 unmapped");
}

/* ---- policy.c --------------------------------------------------------------------------------- */

static void test_policy(void)
{
    const char *why;
    /* the user's "most dangerous" list: tier 3, named */
    static const struct { uint64_t a; const char *name; } t3[] = {
        { 0x02000000, "APM" }, { 0x02039000, "APM" }, { 0x10000000, "fuses" }, { 0x10000014, "fuses" },
        { 0x10070020, "TMU" }, { 0x10074020, "TMU" },
        { 0x14060000, "PMU" }, { 0x1406330c, "PMU" }, { 0x14061088, "PMU" }, { 0x14064044, "PMU" },
        { 0x14100000, "mailbox" }, { 0x17400140, "G3D" },
        { 0x1a000000, "CMU_TOP" }, { 0x1a240000, "CMU_TOP" }, { 0x1a2410a8, "CMU_TOP" }, { 0x1a242074, "CMU_TOP" },
        { 0x1a3ffffc, "CMU_TOP" }, { 0x1b800000, "MIF" }, { 0x1b800120, "MIF" }, { 0x1bc00000, "MIF" },
        { 0x1bfffffc, "MIF" }, { 0x1d000140, "CPU" }, { 0x1d100120, "CPU" }, { 0x1d1ffffc, "CPU" },
        { 0xe1900000, "secure camera" }, { 0xf0000000, "modem" },
    };
    for (unsigned i = 0; i < sizeof(t3) / sizeof(t3[0]); ++i) {
        why = 0;
        CHECK(poke_tier(t3[i].a, &why) == TIER3 && why && strstr(why, t3[i].name),
              "poke %llx must be tier 3 (%s): %s", (unsigned long long)t3[i].a, t3[i].name, why ? why : "-");
    }
    static const uint64_t t2[] = { 0x10020000, 0x10400000, 0x10800000, 0x11000000, 0x11400000, 0x14000000,
                                   0x14200000, 0x16000000, 0x17c00000,   /* peripheral CMU gates */
                                   0x10050000, 0x10100000, 0x11120000, 0x11500000, 0x14050024, 0x14210000,
                                   0x1a242070,                            /* the card gate alone */
                                   0x12345678, 0x15000000, 0x7ffffffc,    /* unknown devices */
                                   0xaf000000, 0xaf7ffffc, 0x8a000000 };
    for (unsigned i = 0; i < sizeof(t2) / sizeof(t2[0]); ++i)
        CHECK(poke_tier(t2[i], &why) == TIER2, "poke %llx must be tier 2: %s", (unsigned long long)t2[i], why);
    static const uint64_t t1[] = { 0x10430000, 0x10430ffc, 0x104b0000, 0x11430000, 0x14220180, 0x14230000,
                                   0x14360000, 0x16030070, 0x16080000, 0x80000000, 0x8c000000, 0x91fff000,
                                   SB_TEST_BASE, SB_TEST_BASE + SB_TEST_SIZE - 4, 0xa8000000, 0xcc000000,
                                   0xfed14000, 0xfed17fe0, 0x880000000ULL, 0x8fffffffcULL };
    for (unsigned i = 0; i < sizeof(t1) / sizeof(t1[0]); ++i)
        CHECK(poke_tier(t1[i], &why) == TIER1, "poke %llx must be tier 1: %s", (unsigned long long)t1[i], why);
    /* only the physical facts are impossible */
    static const uint64_t no[] = { 0x92000000, 0x92080000, 0x9ffffffc, 0xa0000000, 0xa0031ffc, 0xa00ffffc,
                                   0xaf800000, 0xb8000000, 0xbffffffc, 0x900000000ULL, 0x100000000ULL };
    for (unsigned i = 0; i < sizeof(no) / sizeof(no[0]); ++i)
        CHECK(poke_tier(no[i], &why) == TIER_NO, "poke %llx must be impossible: %s", (unsigned long long)no[i], why);
    CHECK(poke_tier(0xa0000000, &why) == TIER_NO && strstr(why, "ECT"), "ECT named: %s", why);
    CHECK(poke_tier(0x92080000, &why) == TIER_NO && strstr(why, "Engineer OS"), "the image named: %s", why);
    CHECK(SB_TEST_BASE >= 0xa0100000UL && poke_tier(SB_TEST_BASE - 4, &why) == TIER_NO, "the test window starts above the ECT");
    /* exhaustive over the register space in 4 KiB steps: always tier 1..3, never free, never
     * impossible; the tier 3 page count is exactly the sum of the tier 3 blocks */
    unsigned n3 = 0, n1 = 0, n2 = 0, bad = 0;
    for (uint64_t a = 0; a < 0x80000000ULL; a += 0x1000) {
        int t = poke_tier(a, &why);
        bad += t < TIER1 || t > TIER3;
        n1 += t == TIER1;
        n2 += t == TIER2;
        n3 += t == TIER3;
    }
    CHECK(!bad, "register space: %u pages outside tiers 1..3", bad);
    /* APM SRAM 1 MiB; CHIPID, TMU, PMU, ACPM mailbox, CMU_G3D 64 KiB each; CMU_TOP 4 MiB (the card
     * gate's 4 bytes do not change a page start); MIF 8 MiB; CPU clusters 2 MiB */
    unsigned want3 = (1U << 20) / 4096 + 5 * 16 + (4U << 20) / 4096 + (8U << 20) / 4096 + (2U << 20) / 4096;
    CHECK(n3 == want3, "tier 3 pages in the register space: %u (want %u)", n3, want3);
    CHECK(n1 == 6 + 2 * 16, "tier 1 pages in the register space: %u (want 38)", n1);
    (void)n2;
    /* memory fill over a range: the worst page decides */
    CHECK(range_tier(SB_TEST_BASE, 1U << 20, &why) == TIER1, "fill inside the test window: tier 1");
    CHECK(range_tier(0x9ff00000, 0x200000, &why) == TIER_NO, "fill from the arena into the ECT: impossible");
    CHECK(range_tier(0xa00ff000, 0x2000, &why) == TIER_NO, "fill touching the ECT window: impossible");
    CHECK(range_tier(0x1a23f000, 0x2000, &why) == TIER3, "fill into CMU_TOP: tier 3");
    CHECK(range_tier(0x80000000, 0, &why) == TIER_NO, "empty fill refused");
    /* reads */
    CHECK(read_tier(0x14060000, &why) == TIER0 && read_tier(0x14050024, &why) == TIER0 && peek_allowed(0x10000000),
          "free reads: PMU, keys, CHIPID");
    CHECK(read_tier(0x14100000, &why) == TIER1 && read_tier(0x02000000, &why) == TIER1, "ACPM reads ask first");
    CHECK(read_tier(0xaf800000, &why) == TIER_NO && read_tier(0xc0000000, &why) == TIER0, "secure hole never read");
    CHECK(read_tier(0xe1900000, &why) == TIER1 && read_tier(0xf0000000, &why) == TIER1, "carve-out reads ask first");
    /* PMIC: tier 1 only for the kernel's exact rail values and the RTC */
    CHECK(pmic_tier(1, 0x6c, 0xec, &why) == TIER1 && pmic_tier(1, 0x74, 0xf0, &why) == TIER1, "touch rails tier 1");
    CHECK(pmic_tier(1, 0x68, 0xe8, &why) == TIER1 && pmic_tier(1, 0x68, 0x28, &why) == TIER1 &&
              pmic_tier(1, 0x4a, 0xfc, &why) == TIER1 && pmic_tier(1, 0x4a, 0x3c, &why) == TIER1,
          "card rails tier 1");
    CHECK(pmic_tier(1, 0x6c, 0xff, &why) == TIER3 && pmic_tier(1, 0x74, 0xff, &why) == TIER3, "other rail values tier 3");
    unsigned pmic3 = 0;
    for (unsigned r = 0; r < 256; ++r)
        for (unsigned v = 0; v < 256; v += 17)
            pmic3 += pmic_tier(1, r, v, &why) == TIER3 && pmic_tier(0, r, v, &why) == TIER3;
    CHECK(pmic3 >= 256 * 16 - 8, "almost every PMIC / TOP register and value is tier 3 (%u)", pmic3);
    CHECK(pmic_tier(2, 0x02, 0x01, &why) == TIER1, "RTC tier 1");
    CHECK(pmic_tier(3, 0, 0, &why) == TIER_NO, "no such PMIC block");
    /* MAX77705: only the watchdog feed is tier 1 */
    CHECK(max_tier(0x69, 0xbd, 0x0d, &why) == TIER1 && max_tier(0x69, 0xbd, 0x01, &why) == TIER1, "watchdog feed tier 1");
    CHECK(max_tier(0x69, 0xbd, 0x02, &why) == TIER3 && max_tier(0x69, 0xbd, 0x31, &why) == TIER3, "other CNFG_06 tier 3");
    CHECK(max_tier(0x69, 0xb7, 0x05, &why) == TIER3 && max_tier(0x69, 0xc0, 0x7f, &why) == TIER3, "charger config tier 3");
    CHECK(max_tier(0x36, 0x18, 0, &why) == TIER3 && max_tier(0x25, 0x21, 0, &why) == TIER3, "FG / USB-C tier 3");
    CHECK(max_tier(0x69, 0xb0, 0, &why) == TIER_NO, "CHG_INT is not writable");
}

/* ---- fence.c ---------------------------------------------------------------------------------- */

static void complete(struct sb_auth *a)
{
    unsigned need = sb_steps_needed(a->tier, a->kind);
    for (unsigned s = 1; s <= SB_STEP_ARMED; s <<= 1)
        if (need & s)
            sb_auth_step(a, s);
}

static void test_fence(void)
{
    struct sb_wdecide d;
    struct sb_auth a;
    static uint8_t blk[8192];
    /* the tiers */
    CHECK(sb_write_decide(SB_MED_UFS, 0, 600, 1, &d) == 0 && d.tier == 3 && !strcmp(d.part, "EFS"), "EFS tier 3");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 5700, 1, &d) == 0 && d.tier == 3, "PARAM tier 3");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 10, 1, &d) == 0 && d.tier == 3, "BOTA0 tier 3");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 0, 1, &d) == 0 && d.tier == 3, "GPT head tier 3");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 15000001, 1, &d) == 0 && d.tier == 3, "GPT tail tier 3");
    CHECK(sb_write_decide(SB_MED_UFS, 1, 0, 1, &d) == 0 && d.tier == 3, "boot LU 1 tier 3");
    CHECK(sb_write_decide(SB_MED_UFS, 4, 123, 8, &d) == 0 && d.tier == 3, "boot LU 4 tier 3");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 7686, 1, &d) == 0 && d.tier == 2, "BOOT tier 2");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 20000, 1, &d) == 0 && d.tier == 2, "RECOVERY tier 2");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 1361420, 1, &d) == 0 && d.tier == 2, "an unknown partition tier 2");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 30000, 8, &d) == 0 && d.tier == 1, "SYSTEM tier 1");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 1310300, 1, &d) == 0 && d.tier == 1, "CACHE tier 1");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 1361514 + 4194306, 1, &d) == 0 && d.tier == 1, "USERDATA tier 1");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 17920, 10, &d) == SB_W_RANGE, "BOOT into RECOVERY: crosses");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 4, 4, &d) == SB_W_RANGE, "GPT head into BOTA0: crosses");
    CHECK(sb_write_decide(SB_MED_UFS, 0, 30000, 0, &d) == SB_W_RANGE, "empty write");
    CHECK(sb_write_decide(SB_MED_SD, 0, 123, 1, &d) == 0 && d.tier == 1, "card tier 1");
    CHECK(sb_write_decide(7, 0, 0, 1, &d) == SB_W_RANGE, "other media refused");
    /* the live store inside USERDATA: raw writes there are tier 2; past its used end tier 1 */
    uint64_t base = 1361514 + 4194304;
    sb_fence_store(SB_MED_UFS, base, 1000, 721024);
    CHECK(sb_write_decide(SB_MED_UFS, 0, base + 1, 1, &d) == 0 && d.tier == 2, "store header tier 2");
    CHECK(sb_write_decide(SB_MED_UFS, 0, base + 999, 1, &d) == 0 && d.tier == 2, "store entries tier 2");
    CHECK(sb_write_decide(SB_MED_UFS, 0, base + 1000, 1, &d) == 0 && d.tier == 1, "past the store's used end tier 1");
    /* the steps each tier needs */
    CHECK(sb_steps_needed(TIER1, 0) == (SB_STEP_WARN | SB_STEP_HELD), "tier 1: warning + held");
    CHECK(sb_steps_needed(TIER2, 0) == (SB_STEP_WARN | SB_STEP_HELD | SB_STEP_TYPED), "tier 2: + typed");
    CHECK(sb_steps_needed(TIER3, 0) == (SB_STEP_WARN | SB_STEP_HELD | SB_STEP_TYPED | SB_STEP_SECOND), "tier 3: + second");
    CHECK(sb_steps_needed(TIER3, SB_KIND_STORAGE) & SB_STEP_BACKUP, "tier 3 storage: + backup");
    CHECK(!(sb_steps_needed(TIER2, SB_KIND_STORAGE) & SB_STEP_BACKUP), "tier 2 storage: no backup needed");
    CHECK(sb_steps_needed(TIER1, SB_KIND_FREEFORM) & SB_STEP_ARMED, "free-form tools need armed mode");

    /* nothing open: refused at every tier and target, the writer never called */
    static const uint64_t lbas[] = { 0, 6, 600, 5700, 7686, 20000, 30000, 1207814, 1310300, 5555555, 15000000,
                                     15000001 };
    sb_gate_close();
    writes = 0;
    for (unsigned m = 0; m < 2; ++m)
        for (unsigned lu = 0; lu < 2; ++lu)
            for (unsigned i = 0; i < sizeof(lbas) / sizeof(lbas[0]); ++i) {
                int r = sb_write(m, lu, lbas[i], 1, blk);
                CHECK(r < 0, "unconfirmed sb_write(m %u, lu %u, lba %llu) = %d", m, lu, (unsigned long long)lbas[i], r);
                CHECK(strstr(last_log, "fence: refused") != 0, "the refusal is logged: %s", last_log);
            }
    CHECK(writes == 0, "no write reached the driver without a confirmation (%d)", writes);
    CHECK(sb_write_store(SB_MED_UFS, base + 2000, 1, blk) == SB_W_AUTH && writes == 0, "store writer refused unconfirmed");
    /* the drivers refuse when the fence is not running them, before any register */
    CHECK(ufs_write_blocks(0, 30000, 1, (uintptr_t)blk) == -6, "ufs_write_blocks outside the fence: -6");
    CHECK(sd_write(0, 1, (uintptr_t)blk) == -9 && sd_write4k(0, 1, (uintptr_t)blk) == -9, "sd_write outside the fence: -9");

    /* an incomplete authorisation does not open the gate */
    sb_auth_init(&a, SB_MED_UFS, 0, 30000, 16, TIER1, SB_KIND_STORAGE, "test");
    CHECK(!sb_gate_open(&a), "no steps: the gate stays shut");
    sb_auth_step(&a, SB_STEP_WARN);
    CHECK(!sb_gate_open(&a), "warning only: shut");
    sb_auth_step(&a, SB_STEP_HELD);
    CHECK(sb_gate_open(&a), "tier 1 with warning + held: open");
    writes = 0;
    CHECK(sb_write(SB_MED_UFS, 0, 30000, 16, blk) == 0 && writes == 1 && w_lba == 30000 && w_n == 16, "the confirmed range is written");
    CHECK(sb_write(SB_MED_UFS, 0, 30010, 7, blk) == SB_W_AUTH, "past the confirmed range: refused");
    CHECK(sb_write(SB_MED_UFS, 0, 29999, 1, blk) == SB_W_AUTH, "before the confirmed range: refused");
    CHECK(sb_write(SB_MED_UFS, 1, 30000, 1, blk) == SB_W_AUTH, "another LU: refused");
    CHECK(sb_write(SB_MED_SD, 0, 30000, 1, blk) == SB_W_AUTH, "another medium: refused");
    CHECK(writes == 1, "only the confirmed write reached the driver (%d)", writes);
    sb_gate_close();
    CHECK(sb_write(SB_MED_UFS, 0, 30000, 1, blk) == SB_W_AUTH, "closed again: refused");
    /* tier: a tier 1 confirmation does not cover BOOT */
    sb_auth_init(&a, SB_MED_UFS, 0, 7686, 1, TIER1, SB_KIND_STORAGE, "test");
    complete(&a);
    CHECK(sb_gate_open(&a) && sb_write(SB_MED_UFS, 0, 7686, 1, blk) == SB_W_TIER, "tier 1 confirmation, BOOT: refused");
    sb_auth_init(&a, SB_MED_UFS, 0, 7686, 1, TIER2, SB_KIND_STORAGE, "test");
    complete(&a);
    CHECK(sb_gate_open(&a) && sb_write(SB_MED_UFS, 0, 7686, 1, blk) == 0, "tier 2 with the typed name, BOOT: written");
    /* tier 3: the backup step is required */
    sb_auth_init(&a, SB_MED_UFS, 0, 518, 5120, TIER3, SB_KIND_STORAGE, "test");
    sb_auth_step(&a, SB_STEP_WARN);
    sb_auth_step(&a, SB_STEP_HELD);
    sb_auth_step(&a, SB_STEP_TYPED);
    sb_auth_step(&a, SB_STEP_SECOND);
    CHECK(!sb_gate_open(&a), "tier 3 storage without the backup step: shut");
    CHECK(sb_write(SB_MED_UFS, 0, 600, 1, blk) == SB_W_AUTH, "EFS without a backup: refused");
    sb_auth_step(&a, SB_STEP_BACKUP);
    CHECK(sb_gate_open(&a) && sb_write(SB_MED_UFS, 0, 600, 1, blk) == 0, "EFS with every step and the backup: written");
    /* a modified authorisation is refused */
    struct sb_auth b = a;
    b.count = 100000;
    CHECK(!sb_auth_valid(&b) && !sb_gate_open(&b), "a widened range breaks the seal");
    b = a;
    b.tier = TIER3;
    b.steps |= SB_STEP_BACKUP;
    b.first = 0;
    CHECK(!sb_gate_open(&b), "a moved range breaks the seal");
    /* revoking (disarm) voids every authorisation */
    CHECK(sb_gate_open(&a), "still valid before the revoke");
    sb_auth_revoke_all();
    CHECK(!sb_auth_valid(&a) && sb_write(SB_MED_UFS, 0, 600, 1, blk) == SB_W_AUTH, "revoked: refused");
    /* dry run: logged, the driver not called */
    sb_auth_init(&a, SB_MED_UFS, 0, 30000, 1, TIER1, SB_KIND_STORAGE, "test");
    complete(&a);
    sb_fence_dry_run(1);
    writes = 0;
    CHECK(sb_gate_open(&a) && sb_write(SB_MED_UFS, 0, 30000, 1, blk) == SB_W_DRYRUN && writes == 0, "dry run writes nothing");
    sb_fence_dry_run(0);
    /* the store writer needs an authorisation that names the store, and stays inside it */
    sb_auth_init(&a, SB_MED_UFS, 0, 0, 0, TIER1, SB_KIND_STORE, "store");
    complete(&a);
    CHECK(sb_gate_open(&a), "store authorisation open");
    writes = 0;
    CHECK(sb_write_store(SB_MED_UFS, base + 2000, 1, blk) == 0 && writes == 1, "store writer inside the store");
    CHECK(sb_write_store(SB_MED_UFS, base - 1, 1, blk) == SB_W_RANGE, "store writer below the store refused");
    CHECK(sb_write_store(SB_MED_UFS, base + 721024, 1, blk) == SB_W_RANGE, "store writer past the store refused");
    CHECK(sb_write(SB_MED_UFS, 0, 30000, 1, blk) == SB_W_AUTH, "a store authorisation does not cover SYSTEM");
    sb_gate_close();
    sb_auth_init(&a, SB_MED_UFS, 0, 30000, 1, TIER1, SB_KIND_STORAGE, "not the store");
    complete(&a);
    CHECK(sb_gate_open(&a) && sb_write_store(SB_MED_UFS, base + 2000, 1, blk) == SB_W_AUTH, "SYSTEM authorisation does not cover the store");
    sb_gate_close();

    /* exhaustive: every partition x confirmed tier x every subset of steps */
    unsigned wrong = 0, ok_writes = 0;
    for (unsigned p = 0; p < NG; ++p) {
        int need_t = 0;
        uint64_t lba = g[p].f;
        if (sb_write_decide(SB_MED_UFS, 0, lba, 1, &d))
            continue;
        need_t = d.tier;
        for (int t = TIER1; t <= TIER3; ++t)
            for (unsigned steps = 0; steps < 32; ++steps) {
                sb_auth_init(&a, SB_MED_UFS, 0, lba, 1, t, SB_KIND_STORAGE, "sweep");
                for (unsigned s = 1; s < 32; s <<= 1)
                    if (steps & s)
                        sb_auth_step(&a, s);
                (void)sb_gate_open(&a);
                writes = 0;
                int r = sb_write(SB_MED_UFS, 0, lba, 1, blk);
                unsigned need = sb_steps_needed(need_t, SB_KIND_STORAGE), have_need = sb_steps_needed(t, SB_KIND_STORAGE);
                int should = t >= need_t && (steps & have_need) == have_need && (steps & need) == need;
                if ((r == 0) != should || (writes == 1) != should)
                    ++wrong;
                ok_writes += r == 0;
                sb_gate_close();
            }
    }
    CHECK(!wrong, "sweep: %u decisions disagree with the tier rule", wrong);
    CHECK(ok_writes > 0, "sweep: some writes were allowed (%u)", ok_writes);
    sb_fence_store(SB_MED_UFS, 0, 0, 0);
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
    CHECK(allowed == 4, "allowed SCSI opcodes on the general path: %u (want 4)", allowed);
    if (allowed != 4)
        return;                                 /* a write opcode would reach the (unmapped) host */
    static const uint8_t wr_ops[] = { 0x0a, 0x2a, 0x2e, 0x35, 0x3b, 0x41, 0x42, 0x55, 0x8a, 0x8e, 0x91, 0x93, 0xaa, 0xae };
    for (unsigned i = 0; i < sizeof(wr_ops); ++i) {
        uint8_t cdb[16] = { wr_ops[i] };
        /* refused before u_zero or any register write: no mapping exists here, so a touch would fault */
        CHECK(ufs_scsi(cdb, 10, 4096) == -6, "write opcode %02x must be refused", wr_ops[i]);
    }
}

/* ---- kern.c --------------------------------------------------------------------------------------- */

static void test_kern(void)
{
    static struct kern_cfg c, c2;
    static uint64_t buf[2][4096];
    c.mask = KERN_INT | KERN_FP | KERN_NEON | KERN_MEM;
    c.buf[0] = buf[0];
    c.buf[5] = buf[1];
    c.words = 4096;
    kern_gold(&c);
    c2 = c;
    kern_gold(&c2);
    CHECK(!memcmp(c.gold, c2.gold, sizeof(c.gold)), "the reference results are deterministic");
    CHECK(c.gold[0] && c.gold[1] && c.gold[2], "references are not zero");
    uint64_t it = 0;
    unsigned e = 0;
    for (unsigned core = 0; core < SB_NCORES; ++core)
        e += kern_batch(&c, core, &it);
    CHECK(e == 0 && it == SB_NCORES, "every core's batch matches the reference (%u errors)", e);
    c.inject = 3;                                 /* core 2's next result is corrupted */
    CHECK(kern_batch(&c, 1, &it) == 0, "core 1: no error");
    CHECK(kern_batch(&c, 2, &it) == 1, "core 2: the corrupted result is caught");
    CHECK(kern_batch(&c, 2, &it) == 0 && c.inject == 0, "the injection is one-shot");
    c.gold[1] ^= 1;                               /* a wrong reference is a mismatch on every core */
    CHECK(kern_batch(&c, 0, &it) == 1, "an FP mismatch is counted");
    c.gold[1] ^= 1;
    /* the worker loop: runs until told to stop, publishes its counters */
    memset(slots, 0, sizeof(slots));
    stop_after = 3;
    kern_worker(5, &c);
    CHECK(slots[5].iters == 3 && slots[5].beats == 3 && slots[5].errors == 0, "worker: 3 batches, published (%llu)",
          (unsigned long long)slots[5].iters);
}

/* ---- adc.c thermistor tables ------------------------------------------------------------------------ */

static void test_therm(void)
{
    int t = sb_therm_convert(1, 1950);
    CHECK(t >= 255 && t <= 270, "battery: ADC 1950 = %d (S-Boot: 26 C)", t);
    CHECK(sb_therm_convert(1, 0x113) == 900 && sb_therm_convert(1, 0) == 900, "battery: clamps at 90 C");
    CHECK(sb_therm_convert(1, 0xe5c) == -200 && sb_therm_convert(1, 4095) == -200, "battery: clamps at -20 C");
    CHECK(sb_therm_convert(1, 0xc2c) == 0, "battery: 0 C at 0xC2C");
    CHECK(sb_therm_convert(0, 0x3eb) == 500, "AP: 50 C at 0x3EB");
    CHECK(sb_therm_convert(2, 0x147) == -200 && sb_therm_convert(2, 0xee4) == 900 && sb_therm_convert(2, 0x800) == 250,
          "PA: -20 / 90 / 25 C");
    int prev = 1000, mono = 1;
    for (int a = 0; a < 4096; a += 8) {
        int v = sb_therm_convert(1, a);
        mono &= v <= prev;
        prev = v;
    }
    CHECK(mono, "battery table: temperature falls as the ADC value rises");
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
    nheld = 0;
    hold(SB_K_POWER, 200);
    CHECK(nheld == 0, "short Power: no event while held (%d)", nheld);
    CHECK(next_event(50) == EV_SELECT, "short Power selects on release");
    CHECK(next_event(50) == EV_NONE, "short Power: one event");
    nheld = 0;
    hold(SB_K_POWER, 1700);
    CHECK(nheld == 1 && held_events[0] == EV_BACK, "long Power: one BACK while held (%d, %d)", nheld, held_events[0]);
    CHECK(next_event(50) == EV_NONE, "long Power: nothing on release");
    nheld = 0;
    hold(SB_K_POWER, UI_POWER_LONG_MS - 60);
    CHECK(nheld == 0 && next_event(50) == EV_SELECT, "Power held %u ms still selects", UI_POWER_LONG_MS - 60);
    nheld = 0;
    hold(SB_K_BIXBY, 150);
    CHECK(nheld == 0 && next_event(50) == EV_BACK, "short Bixby: back");
    nheld = 0;
    hold(SB_K_BIXBY, 1000);
    CHECK(nheld == 1 && held_events[0] == EV_HOME && next_event(50) == EV_NONE, "long Bixby: home only");
    nheld = 0;
    hold(SB_K_DOWN, 100);
    CHECK(nheld == 1 && held_events[0] == EV_DOWN, "Volume Down: one step");
    nheld = 0;
    hold(SB_K_UP, 1000);
    CHECK(nheld >= 6 && nheld <= 9 && held_events[0] == EV_UP, "Volume Up held 1 s: %d steps (repeat)", nheld);
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
    test_kern();
    test_therm();
    test_input();
    printf("unit tests: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
