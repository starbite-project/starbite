/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/nled.c (the MAX77705 RGB LED) against a fake MAX77705 bus and a fake libsb gate.
 * Build and run: starbite/sbos/kern/host/nled-test.sh [-v]
 *
 * The fake chip answers at 0x66 (all 256 registers), 0x69 / 0x36 / 0x25 (never to be touched). Checked independently of
 * nled.c's own guards: every write is one of the seven LED registers at 0x66, under an open tier-1 authorisation
 * covering exactly that register with both steps; no BRT write is above the DT cap unless it puts back the value the
 * block was found with; the probe's level 0 / 1 write nothing; level 2's sequence and timing; the block back as found
 * after level 2, after an abort at every step, after a budget overrun and after every injected fault (a NACK, a
 * register that does not read back, a gate refusal, a bus that never answers). Each scenario runs in its own process
 * (the driver keeps static state). Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "sb.h"
#include "nled.h"

static int fails, checks, verbose;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(c)) {                                                                                                    \
            ++fails;                                                                                                   \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- the fake libsb ------------------------------------------------------------------------------------------- */
static uint64_t now_ms = 100000;
uint64_t sb_ms(void) { return now_ms; }
uint64_t sb_us(void) { return now_ms * 1000; }

static char lastlog[8][300];
static unsigned nlog;
static char logall[1 << 16];
void sb_log(const char *s)
{
    snprintf(lastlog[nlog++ % 8], sizeof lastlog[0], "%s", s);
    if (strlen(logall) + strlen(s) + 2 < sizeof logall) {
        strcat(logall, s);
        strcat(logall, "\n");
    }
    if (verbose)
        printf("    | %s\n", s);
}
void sb_logf(const char *a, const char *b)
{
    char t[600];
    snprintf(t, sizeof t, "%s%s", a, b);
    sb_log(t);
}
void sb_logx(const char *a, uint64_t v)
{
    char t[300];
    snprintf(t, sizeof t, "%s0x%llx", a, (unsigned long long)v);
    sb_log(t);
}
static const char *last(void) { return lastlog[(nlog + 7) % 8]; }

static struct {
    uint8_t r66[256], other[256];
    uint8_t asfound[256];
    int bus_ok;                 /* sb_max_init answers */
    int nack_write;             /* the n-th write (1-based) is not acknowledged; 0 none */
    int stuck_reg;              /* writes to this register are ignored by the chip; -1 none */
    int gate_refuses;
    unsigned writes, reads, violations, cap_violations, gate_violations, other_reads;
    int open;                   /* an authorisation is open */
    struct sb_auth au;
    uint8_t wlog[4096][2];
} F;

struct sb_i2c sb_max_bus;
const char *sb_max_why;

int sb_max_init(void)
{
    if (!F.bus_ok)
        sb_max_why = "no answer on HSI2C 0x14360000";
    return F.bus_ok;
}

int sb_i2c_read(struct sb_i2c *b, unsigned addr, uint8_t reg, uint8_t *data, unsigned len)
{
    (void)b;
    ++F.reads;
    if (!F.bus_ok)
        return 0;
    if (addr != NLED_ADDR) {
        ++F.other_reads;
        return 0;
    }
    for (unsigned i = 0; i < len; ++i)
        data[i] = F.r66[(uint8_t)(reg + i)];
    return 1;
}

void sb_auth_init(struct sb_auth *a, unsigned space, unsigned lu, uint64_t first, uint64_t count, int tier,
                  unsigned kind, const char *what)
{
    memset(a, 0, sizeof *a);
    a->magic = 0x5eed;
    a->space = space, a->lu = lu, a->first = first, a->count = count, a->tier = tier, a->kind = kind;
    snprintf(a->what, sizeof a->what, "%s", what);
}
void sb_auth_step(struct sb_auth *a, unsigned step) { a->steps |= step; }
int sb_gate_open(const struct sb_auth *a)
{
    if (F.gate_refuses)
        return 0;
    if (a->magic != 0x5eed || a->tier < SB_TIER1 || (a->steps & (SB_STEP_WARN | SB_STEP_HELD)) !=
                                                        (SB_STEP_WARN | SB_STEP_HELD))
        return 0;
    F.au = *a;
    F.open = 1;
    return 1;
}
void sb_gate_close(void) { F.open = 0; }

static int led_reg(unsigned r)
{
    return r == 0x30 || (r >= 0x31 && r <= 0x34) || r == 0x36 || r == 0x38;
}

int sb_max_write(unsigned addr7, uint8_t reg, uint8_t val, int tier)
{
    ++F.writes;
    if (F.writes <= 4096)
        F.wlog[F.writes - 1][0] = reg, F.wlog[F.writes - 1][1] = val;
    /* the independent allow-list and gate check */
    if (addr7 != NLED_ADDR || !led_reg(reg)) {
        ++F.violations;
        return 0;
    }
    if (!F.open || F.au.space != SB_SP_MAX || F.au.first != ((uint64_t)addr7 << 8 | reg) || F.au.count != 1 ||
        tier < SB_TIER1 || F.au.tier < tier) {
        ++F.gate_violations;
        return 0;
    }
    static const uint8_t cap[4] = { 10, 70, 6, 12 };       /* the DT's: 10 x {100, 700, 60, 120} / 100 */
    if (reg >= 0x31 && reg <= 0x34 && val > cap[reg - 0x31] && val != F.asfound[reg])
        ++F.cap_violations;
    if (F.nack_write && (int)F.writes == F.nack_write)
        return 0;
    if ((int)reg != F.stuck_reg)
        F.r66[reg] = val;
    return 1;
}

/* ---- scenario helpers ------------------------------------------------------------------------------------------ */
static void chip(uint8_t leden, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3, uint8_t rmp, uint8_t blnk)
{
    memset(&F, 0, sizeof F);
    for (unsigned i = 0; i < 256; ++i)
        F.r66[i] = (uint8_t)(0xA5 ^ i);                    /* everything else at 0x66: a pattern that must not change */
    F.r66[0x30] = leden, F.r66[0x31] = b0, F.r66[0x32] = b1, F.r66[0x33] = b2, F.r66[0x34] = b3;
    F.r66[0x36] = rmp, F.r66[0x38] = blnk;
    memcpy(F.asfound, F.r66, 256);
    F.bus_ok = 1;
    F.stuck_reg = -1;
}

static int as_found(void)
{
    return memcmp(F.r66, F.asfound, 256) == 0;
}

static int no_bad_writes(void)
{
    return !F.violations && !F.cap_violations && !F.gate_violations && !F.other_reads;
}

/* run one level of the probe to the end, advancing time by what it asks; returns the total wait; *calls the calls */
static unsigned run_probe(int level, unsigned *calls, unsigned extra_ms_per_call)
{
    unsigned total = 0, n = 0, w = 0;
    while (n < 100) {
        ++n;
        int done = drv_nled_probe(level, &w);
        if (done)
            break;
        total += w;
        now_ms += w + extra_ms_per_call;
    }
    *calls = n;
    return total;
}

/* ---- the scenarios --------------------------------------------------------------------------------------------- */
static void t_allowlist(void)
{
    unsigned n = 0;
    const unsigned addrs[] = { 0x25, 0x36, 0x4A, 0x62, 0x66, 0x69 };
    for (unsigned a = 0; a < sizeof addrs / sizeof addrs[0]; ++a)
        for (unsigned r = 0; r < 256; ++r)
            if (nled_allowed(addrs[a], r)) {
                ++n;
                CHECK(addrs[a] == 0x66 && led_reg(r), "allowed %02x:%02x", addrs[a], r);
            }
    CHECK(n == 7, "exactly the seven LED registers are allowed (got %u)", n);
    CHECK(nled_cap[0] == 10 && nled_cap[1] == 70 && nled_cap[2] == 6 && nled_cap[3] == 12, "caps from the DT");
    /* Samsung's LEDBLNK_ON / OFF */
    CHECK(nled_blnk_code(500, 500) == 0x41, "blink 500/500 = 0x41 (got %02x)", nled_blnk_code(500, 500));
    CHECK(nled_blnk_code(500, 5000) == 0x4A, "blink 500/5000 = 0x4A (got %02x)", nled_blnk_code(500, 5000));
    CHECK(nled_blnk_code(200, 200) == 0x11, "blink 200/200 = 0x11");
    CHECK(nled_blnk_code(3250, 12000) == 0xFF, "the longest = 0xFF");
    CHECK(nled_blnk_code(1000, 10000) == 0x6E, "blink 1000/10000 = 0x6E (got %02x)", nled_blnk_code(1000, 10000));
}

static void t_level0(void)
{
    chip(0, 0, 0, 0, 0, 0, 0);
    unsigned calls, w = run_probe(0, &calls, 0);
    CHECK(calls == 1 && w == 0, "level 0 is one call");
    CHECK(F.writes == 0, "level 0 writes nothing (%u)", F.writes);
    CHECK(strstr(last(), "drvprobe: nled: PASS the LED block answers at 0x66, LEDEN 0x00") != 0, "level 0 PASS: %s",
          last());
    CHECK(as_found() && no_bad_writes(), "level 0: the chip untouched");
    run_probe(1, &calls, 0);
    CHECK(F.writes == 0 && strstr(last(), "drvprobe: nled: PASS skipped (attended)"), "level 1: %s", last());
}

static void t_level0_nobus(void)
{
    chip(0, 0, 0, 0, 0, 0, 0);
    F.bus_ok = 0;
    unsigned calls;
    run_probe(0, &calls, 0);
    CHECK(F.writes == 0 && strstr(last(), "drvprobe: nled: FAIL the LED block (MAX77705 0x66"), "no bus: %s", last());
    CHECK(!nled_color(255, 0, 0, 100) && F.writes == 0, "no bus: no colour, nothing written");
    run_probe(2, &calls, 0);
    CHECK(F.writes == 0 && strstr(last(), "FAIL"), "no bus, level 2: FAIL, nothing written: %s", last());
}

static void t_level2(void)
{
    chip(0, 0, 0, 0, 0, 0x22, 0x13);
    unsigned calls, wait = 0, n = 0, w = 0;
    /* step by step: what is lit after each call */
    struct { uint8_t leden, reg, val; unsigned wait; } want[] = {
        { 0x04, 0x32, 17, 1000 },   /* red, a quarter of 70 */
        { 0x10, 0x33, 1, 1000 },    /* green, a quarter of 6 = 1 */
        { 0x40, 0x34, 3, 1000 },    /* blue */
        { 0x54, 0x32, 17, 1000 },   /* white = r + g + b */
        { 0x01, 0x31, 2, 1000 },    /* LED0 */
        { 0x80, 0x34, 3, 3000 },    /* blue blinking */
    };
    for (n = 0; n < 6; ++n) {
        int done = drv_nled_probe(2, &w);
        CHECK(!done, "step %u not the end", n);
        CHECK(F.r66[0x30] == want[n].leden, "step %u LEDEN %02x (want %02x)", n, F.r66[0x30], want[n].leden);
        CHECK(F.r66[want[n].reg] == want[n].val, "step %u reg %02x = %u (want %u)", n, want[n].reg,
              F.r66[want[n].reg], want[n].val);
        CHECK(w == want[n].wait, "step %u waits %u", n, w);
        wait += w;
        now_ms += w + 5;
    }
    CHECK(F.r66[0x38] == 0x41 && F.r66[0x36] == 0, "blink 0.5 / 0.5 s, no ramp (BLNK %02x RMP %02x)", F.r66[0x38],
          F.r66[0x36]);
    int done = drv_nled_probe(2, &w);
    CHECK(done && strstr(last(), "drvprobe: nled: PASS red, green, blue, white, LED0 and a blink"), "end: %s", last());
    CHECK(as_found(), "level 2: the block as found after");
    CHECK(no_bad_writes(), "level 2: every write allowed, gated, capped");
    CHECK(wait == 8000 && wait < NLED_PROBE_BUDGET_MS, "level 2 waits 8 s in all (%u)", wait);
    (void)calls;
}

/* abort at every step of level 2, and the watchdog path: always back as found */
static void t_abort_each(int at)
{
    chip(0x04, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00);       /* red on from before (e.g. charging) */
    unsigned w;
    for (int n = 0; n < at; ++n) {
        drv_nled_probe(2, &w);
        now_ms += w;
    }
    drv_nled_abort();
    CHECK(as_found(), "abort after %d steps: as found", at);
    CHECK(no_bad_writes(), "abort after %d steps: writes allowed", at);
    drv_nled_abort();                                    /* twice is harmless */
    CHECK(as_found(), "a second abort changes nothing");
}

static void t_budget(void)
{
    chip(0, 0, 0, 0, 0, 0, 0);
    unsigned calls;
    run_probe(2, &calls, 2000);                           /* each step 2 s late: over the 12 s */
    CHECK(strstr(last(), "drvprobe: nled: FAIL did not finish within its 12 s"), "budget: %s", last());
    CHECK(as_found() && no_bad_writes(), "budget: as found");
}

static void t_fault_level2(int nack, int stuck, int gate)
{
    chip(0, 0, 0, 0, 0, 0x22, 0);                         /* a ramp from before: RMP must be written too */
    F.nack_write = nack, F.stuck_reg = stuck, F.gate_refuses = gate;
    unsigned calls;
    run_probe(2, &calls, 0);
    /* a fault while showing: FAIL; a fault in the final restore: FAIL unless the restore recovered (it retries LEDEN) */
    CHECK(strstr(last(), "drvprobe: nled: FAIL") || (as_found() && strstr(last(), "drvprobe: nled: PASS")),
          "fault nack %d stuck %d gate %d: FAIL or recovered (%s)", nack, stuck, gate, last());
    /* the NACKed / stuck register may hold a lit value only when nothing could be written afterwards; LEDEN must be
     * off or as found */
    CHECK(F.r66[0x30] == 0, "fault nack %d stuck %d gate %d: the LED off (LEDEN %02x)", nack, stuck, gate, F.r66[0x30]);
    for (unsigned r = 0; r < 256; ++r)
        if (!led_reg(r))
            CHECK(F.r66[r] == F.asfound[r], "fault: non-LED register %02x untouched", r);
    CHECK(no_bad_writes(), "fault: writes allowed");
}

static void t_api(void)
{
    chip(0, 0, 0, 0, 0, 0, 0);
    CHECK(nled_color(255, 255, 255, 100), "full white");
    CHECK(F.r66[0x32] == 70 && F.r66[0x33] == 6 && F.r66[0x34] == 12 && F.r66[0x30] == 0x54,
          "full = the caps (%u %u %u, LEDEN %02x)", F.r66[0x32], F.r66[0x33], F.r66[0x34], F.r66[0x30]);
    CHECK(nled_color(255, 0, 0, 50) && F.r66[0x32] == 35 && F.r66[0x30] == 0x04, "red 50 %% = 35, only red lit");
    CHECK(nled_color(1, 0, 0, 1) && F.r66[0x32] == 1, "the dimmest lit is 1");
    CHECK(nled_color(255, 0, 0, 1000) && F.r66[0x32] == 70, "pct above 100 is 100");
    CHECK(nled_pattern(NLED_MISSED_CALL) && F.r66[0x30] == 0x80 && F.r66[0x34] == 12 && F.r66[0x38] == 0x4A,
          "missed call: blue blink 0.5 / 5 s (LEDEN %02x BLNK %02x)", F.r66[0x30], F.r66[0x38]);
    CHECK(nled_pattern(NLED_MESSAGE) && F.r66[0x30] == 0x80, "message: blue blink");
    CHECK(nled_pattern(NLED_CHARGING) && F.r66[0x30] == 0x04 && F.r66[0x32] == 70, "charging: red steady");
    CHECK(nled_pattern(NLED_CHARGED) && F.r66[0x30] == 0x10 && F.r66[0x33] == 6, "charged: green steady");
    CHECK(nled_pattern(NLED_LOW_BATTERY) && F.r66[0x30] == 0x08 && F.r66[0x38] == 0x4A, "low battery: red blink");
    CHECK(nled_pattern(NLED_CHARGE_ERROR) && F.r66[0x30] == 0x08 && F.r66[0x38] == 0x41, "charge error: fast blink");
    CHECK(nled_blink(0, 255, 0, 100, 10, 99999) && F.r66[0x38] == 0x0F, "blink times clamped (%02x)", F.r66[0x38]);
    CHECK(nled_pattern(NLED_OFF) && F.r66[0x30] == 0, "off");
    CHECK(nled_color(0, 0, 0, 100) && F.r66[0x30] == 0, "black = off");
    CHECK(!nled_pattern((enum nled_pattern)99), "no such pattern refused");
    CHECK(!strcmp(nled_pattern_name(NLED_MISSED_CALL), "Missed call") && !strcmp(nled_pattern_name(99), "?"),
          "pattern names");
    unsigned before = F.writes;
    CHECK(nled_pattern(NLED_OFF) && F.writes == before, "an unchanged register is not written again");
    nled_restore();
    CHECK(as_found() && no_bad_writes(), "restore: as found");
}

/* the block found lit above our caps (left by another OS): never driven above the caps, but put back exactly */
static void t_restore_high(void)
{
    chip(0x04, 0x00, 0x90, 0x00, 0x00, 0x11, 0x22);
    CHECK(nled_pattern(NLED_CHARGED), "charged");
    CHECK(F.r66[0x32] == 0x90 && F.r66[0x30] == 0x10, "red's BRT left alone while green is lit");
    CHECK(nled_color(255, 0, 0, 100) && F.r66[0x32] == 70, "red from us: at the cap (%u), not the found 0x90",
          F.r66[0x32]);
    nled_restore();
    CHECK(as_found() && no_bad_writes(), "restore puts back the found 0x90 exactly");
}

/* each in a fresh process: the driver's "as found" is per boot */
static void t_fault_stuck(void)
{
    chip(0, 0, 0, 0, 0, 0, 0);
    F.stuck_reg = 0x33;
    CHECK(!nled_color(0, 255, 0, 100), "green stuck: refused");
    CHECK(F.r66[0x30] == 0 && strstr(nled_why(), "read back"), "stuck: LED off, why '%s'", nled_why());
    CHECK(no_bad_writes(), "writes allowed");
}
static void t_fault_nack(void)
{
    chip(0, 0, 0, 0, 0, 0, 0);
    F.nack_write = 2;                                     /* BLNK ok, the BRT NACKed */
    CHECK(!nled_pattern(NLED_LOW_BATTERY) && F.r66[0x30] == 0, "NACK: refused, the LED off");
    CHECK(strstr(nled_why(), "not answered") != 0, "why: %s", nled_why());
    CHECK(no_bad_writes(), "writes allowed");
}
static void t_fault_gate(void)
{
    chip(0, 0, 0, 0, 0, 0, 0);
    F.gate_refuses = 1;
    CHECK(!nled_pattern(NLED_CHARGING) && F.writes == 0 && strstr(nled_why(), "gate"), "gate refused: nothing written");
}
static void t_fault_lit(void)
{
    chip(0x04, 0, 0x20, 0, 0, 0, 0);
    F.nack_write = 1;                                     /* red lit from before, our BRT NACKed: the LED off */
    CHECK(!nled_color(255, 0, 0, 100) && F.r66[0x30] == 0, "BRT NACK refused, the LED off (LEDEN %02x)", F.r66[0x30]);
    CHECK(no_bad_writes(), "writes allowed");
    F.nack_write = 0;
    nled_restore();
    CHECK(as_found(), "restore after the fault: red lit again as found");
}
static void t_retry(void)
{
    chip(0, 0, 0, 0, 0, 0, 0);
    F.bus_ok = 0;
    CHECK(!nled_init(), "no bus");
    F.bus_ok = 1;
    CHECK(nled_init() && nled_pattern(NLED_CHARGED), "the bus back: a later call works");
    nled_restore();
    CHECK(as_found() && no_bad_writes(), "as found");
}

typedef void (*scen)(void);
static int run(const char *name, scen f)
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        f();
        printf("  %s: %d checks, %d failed\n", name, checks, fails);
        fflush(stdout);
        _exit(fails ? 1 : 0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

static int abort_at;
static void t_abort(void) { t_abort_each(abort_at); }
static int fn, fs, fg;
static void t_fault(void) { t_fault_level2(fn, fs, fg); }

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    int bad = 0;
    bad += run("allow-list and blink codes", t_allowlist);
    bad += run("level 0 / 1", t_level0);
    bad += run("no bus", t_level0_nobus);
    bad += run("level 2", t_level2);
    for (abort_at = 0; abort_at <= 7; ++abort_at)
        bad += run("abort", t_abort);
    bad += run("budget", t_budget);
    for (fn = 1; fn <= 19; ++fn) {
        fs = -1, fg = 0;
        bad += run("NACK in level 2", t_fault);
    }
    fn = 0, fg = 0;
    const int stuck[] = { 0x31, 0x32, 0x33, 0x34, 0x36, 0x38 };
    for (unsigned i = 0; i < 6; ++i) {
        fs = stuck[i];
        bad += run("stuck register in level 2", t_fault);
    }
    fs = -1, fg = 1;
    bad += run("gate refuses in level 2", t_fault);
    bad += run("the API and patterns", t_api);
    bad += run("found above the caps", t_restore_high);
    bad += run("API: stuck register", t_fault_stuck);
    bad += run("API: NACK", t_fault_nack);
    bad += run("API: gate refuses", t_fault_gate);
    bad += run("API: NACK with the LED lit from before", t_fault_lit);
    bad += run("API: the bus answers later", t_retry);
    printf(bad ? "nled-test: %d scenario(s) FAILED\n" : "nled-test: all scenarios passed\n", bad);
    return bad ? 1 : 0;
}
