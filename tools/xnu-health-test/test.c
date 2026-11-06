/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of XNU's side of the boot health (xnu/series-12377/0399, bsd/dev/starlte_health.h,
 * docs/STARBITE-HEALTH.md 5.4): the header comes out of the patch (run.sh), and is checked
 * against the loader's own code: sbfmt.c's watchdog counts, ring reader, text helpers and the
 * decision table; crash.c's needles, hw.h's INFORM values and recovery.c's mailbox (passed in by
 * run.sh as -D values).
 *   sh tools/xnu-health-test/run.sh
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sbfmt.h"
#include "starlte_health.h"

#define M_XNU_ 1U                       /* catalog.h M_XNU */
#define CAT_LOG_MAX_ 128U               /* catalog.h CAT_LOG_MAX: xnu_boot.c launch.xfrag is this + 32 */
#define MID_PANIC "\n--- console before the panic:\n"   /* herolte_ufs.c starlte_pmsg_save */
#define MID_RESET "\n--- console before the reset:\n"

static int fails, checks;

#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++fails;                                                                               \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);                                    \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)len + 1);
    *n = fread(b, 1, (size_t)len, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

/* The string literal after `prefix` in crash.c ("int xp = sb_find(t1, n1, \"") */
static int literal_after(const char *src, const char *prefix, char *out, size_t cap)
{
    const char *p = strstr(src, prefix);
    if (!p)
        return 0;
    p += strlen(prefix);
    size_t k = 0;
    while (*p && *p != '"' && k + 1 < cap)
        out[k++] = *p++;
    out[k] = 0;
    return *p == '"';
}

static void test_constants(const char *crash_c)
{
    CHECK(SBH_INFORM3_NORMAL == LDR_INFORM_NORMAL, "0x%x vs hw.h 0x%x", SBH_INFORM3_NORMAL, LDR_INFORM_NORMAL);
    CHECK(SBH_INFORM3_RECOVERY == LDR_INFORM_RECOVERY, "0x%x vs hw.h 0x%x", SBH_INFORM3_RECOVERY, LDR_INFORM_RECOVERY);
    CHECK(SBH_PMU_PA == LDR_PMU_BASE, "PMU");
    CHECK(SBH_PMU_INFORM2 == 0x808u && SBH_PMU_INFORM3 == 0x80cu && SBH_PMU_SWRESET == 0x400u, "PMU offsets");
    CHECK(SBH_INFORM2_REBOOT == 0x12345678u, "INFORM2 magic as menu.c writes it");
    CHECK(SBH_WDT1_PA == LDR_WDT1_BASE, "WDT1 0x%x vs wdt.h 0x%x", SBH_WDT1_PA, (unsigned)LDR_WDT1_BASE);
    CHECK(SBH_PMU_PA + SBH_PMU_WDT_DISABLE == LDR_PMU_WDT_DISABLE, "PMU WDT disable");
    CHECK(SBH_PMU_PA + SBH_PMU_WDT_MASK == LDR_PMU_WDT_MASK, "PMU WDT mask");
    CHECK(SBH_WDT_CLK_HZ == WDT_CLK_HZ, "clock");
    CHECK(SBH_RING_SIG == RING_SIG && SBH_PMSG_DATA == RING_PMSG, "ring format");
    CHECK(SBH_PMSG_PA + SBH_MAILBOX_OFF == LDR_NEXT_MAILBOX, "mailbox 0x%x vs recovery.c 0x%x",
          SBH_PMSG_PA + SBH_MAILBOX_OFF, (unsigned)LDR_NEXT_MAILBOX);
    CHECK(SBH_PMSG_HDR + SBH_PMSG_DATA <= SBH_MAILBOX_OFF, "data reaches the mailbox");
    CHECK(SBH_PMSG_MAP == 0x4000u, "one ramoops zone mapped");
    /* the loader's needles must open XNU's headers byte for byte */
    char xp[96], xh[96];
    CHECK(literal_after(crash_c, "int xp = sb_find(t1, n1, \"", xp, sizeof(xp)), "crash.c panic needle not found");
    CHECK(literal_after(crash_c, "int xh = sb_find(t1, n1, \"", xh, sizeof(xh)), "crash.c timer needle not found");
    CHECK(strncmp(SBH_HDR_PANIC, xp, strlen(xp)) == 0, "panic header \"%s\" vs needle \"%s\"", SBH_HDR_PANIC, xp);
    CHECK(strncmp(SBH_HDR_TIMER, xh, strlen(xh)) == 0, "timer header \"%s\" vs needle \"%s\"", SBH_HDR_TIMER, xh);
    CHECK(strstr(SBH_HDR_PANIC, "panic(") == NULL && strstr(SBH_HDR_TIMER, "panic(") == NULL,
          "a header must not look like the reason line");
}

static void test_crash_mode(void)
{
    static const struct { const char *v; int m; } t[] = {
        { "loader", SBH_CRASH_LOADER }, { "recovery", SBH_CRASH_RECOVERY }, { "", SBH_CRASH_UNSET },
        { "load", SBH_CRASH_UNSET }, { "loaderx", SBH_CRASH_UNSET }, { "LOADER", SBH_CRASH_UNSET },
        { "recover", SBH_CRASH_UNSET }, { "recoveryy", SBH_CRASH_UNSET }, { "1", SBH_CRASH_UNSET },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); ++i)
        CHECK(sbh_crash_mode(t[i].v) == t[i].m, "sbcrash=%s -> %d", t[i].v, sbh_crash_mode(t[i].v));
    /* the reset a panic asks for */
    CHECK(sbh_panic_inform3(SBH_CRASH_LOADER, 0) == SBH_INFORM3_NORMAL, "loader");
    CHECK(sbh_panic_inform3(SBH_CRASH_LOADER, 86400) == SBH_INFORM3_NORMAL, "loader wins over sweetreboot");
    CHECK(sbh_panic_inform3(SBH_CRASH_RECOVERY, 0) == SBH_INFORM3_RECOVERY, "recovery");
    CHECK(sbh_panic_inform3(SBH_CRASH_UNSET, 300) == SBH_INFORM3_RECOVERY, "sweetreboot as before 0399");
    CHECK(sbh_panic_inform3(SBH_CRASH_UNSET, 0) == 0, "nothing: spin");
}

static void test_watchdog(void)
{
    /* the hardware timeout XNU computes from WTCON/WTDAT = the loader's own ms, for every value
     * the loader can arm; the period leaves two thirds of the timeout as margin */
    for (unsigned s = 0; s <= 100; ++s) {
        unsigned ms = 0;
        uint32_t c = sb_wdt_count(s, &ms);
        if (!s) {
            CHECK(c == 0 && sbh_wdt_period_ms(0, 0) == 0, "0 = off");
            continue;
        }
        uint32_t hw = sbh_wdt_hw_ms(WDT_WTCON, c);
        CHECK(hw == ms, "%u s: %u counts, XNU %u ms, loader %u ms", s, c, hw, ms);
        unsigned arg = (ms + 500U) / 1000U;     /* xnu_boot.c launch_prepare: sbwdt= */
        CHECK(arg >= WDT_MIN_S && arg <= WDT_MAX_S + 1, "%u s: sbwdt=%u", s, arg);
        uint32_t p = sbh_wdt_period_ms(arg, hw);
        CHECK(p >= SBH_PERIOD_MIN_MS && p <= SBH_PERIOD_MAX_MS, "%u s: period %u", s, p);
        CHECK(3ULL * p <= hw, "%u s: period %u ms > a third of %u ms", s, p, hw);
        CHECK(3ULL * p + 3 >= (hw < arg * 1000U ? hw : arg * 1000U), "%u s: period %u ms too short", s, p);
        /* a payload's own sbwdt= that disagrees: the hardware's timeout still rules */
        CHECK(3ULL * sbh_wdt_period_ms(82, hw) <= hw, "%u s: sbwdt=82 on a shorter watchdog", s);
        CHECK(sbh_wdt_period_ms(10, hw) == 3333, "%u s: sbwdt=10: %u", s, sbh_wdt_period_ms(10, hw));
    }
    /* the documented values (docs/STARBITE-HEALTH.md 5.4) */
    unsigned ms;
    CHECK(sb_wdt_count(60, &ms) == 47607 && sbh_wdt_hw_ms(0xFF39, 47607) == 59999, "60 s");
    CHECK(sbh_wdt_period_ms(60, 59999) == 19999, "60 s: %u", sbh_wdt_period_ms(60, 59999));
    CHECK(sbh_wdt_period_ms(30, sbh_wdt_hw_ms(0xFF39, 23803)) == 9999, "30 s");
    CHECK(sbh_wdt_period_ms(82, sbh_wdt_hw_ms(0xFF39, 65063)) == 27333, "82 s: %u",
          sbh_wdt_period_ms(82, sbh_wdt_hw_ms(0xFF39, 65063)));
    CHECK(sbh_wdt_hw_ms(0xFF39, 0xffff) == 82594, "the longest (82.6 s): %u", sbh_wdt_hw_ms(0xFF39, 0xffff));
    CHECK(sbh_wdt_period_ms(82, 82594) == 27333, "82 s on the longest");
    CHECK(sbh_wdt_period_ms(100, 82594) == SBH_PERIOD_MAX_MS, "capped: %u", sbh_wdt_period_ms(100, 82594));
    CHECK(sbh_wdt_period_ms(2, 1500) == SBH_PERIOD_MIN_MS, "floor: %u", sbh_wdt_period_ms(2, 1500));
    /* not armed: never fed (XNU must not start a watchdog the loader did not arm) */
    CHECK(sbh_wdt_hw_ms(0xFF39 & ~SBH_WTCON_ENABLE, 47607) == 0, "disabled");
    CHECK(sbh_wdt_hw_ms(0xFF39 & ~SBH_WTCON_RSTEN, 47607) == 0, "no reset");
    CHECK(sbh_wdt_hw_ms(0xFF39, 0) == 0 && sbh_wdt_hw_ms(0xFF39, 0x10000) == 0, "zero count");
    CHECK(sbh_wdt_period_ms(60, 0) == 0, "unknown hardware: not fed");
    /* whether to feed: sbwdt=N; sbwdt=0 never; no sbwdt= (the loader's token did not fit) only
     * for a watchdog armed with its reset unmasked in both PMU words, as wdt_arm leaves it */
    const uint32_t b23 = 1u << SBH_WDT1_PMU_BIT;
    CHECK(SBH_WDT1_PMU_BIT == LDR_WDT1_PMU_BIT, "PMU bit vs wdt.h");
    CHECK(sbh_wdt_feed_s(1, 60, 0xFF39, 47607, 0, 0) == 60, "sbwdt=60");
    CHECK(sbh_wdt_feed_s(1, 60, 0xFF39, 47607, b23, b23) == 60, "sbwdt=60, masked: still fed (harmless)");
    CHECK(sbh_wdt_feed_s(1, 0, 0xFF39, 47607, 0, 0) == 0, "sbwdt=0: never");
    CHECK(sbh_wdt_feed_s(1, 60, 0xFF39 & ~SBH_WTCON_ENABLE, 47607, 0, 0) == 0, "sbwdt=60, not armed");
    CHECK(sbh_wdt_feed_s(0, 0, 0xFF39, 47607, 0, 0) == 60, "no sbwdt=, armed by the loader: its 60 s");
    CHECK(sbh_wdt_feed_s(0, 0, 0xFF39, 23803, 0, 0) == 30, "no sbwdt=, 30 s");
    CHECK(sbh_wdt_feed_s(0, 0, 0xFF39, 47607, b23, b23) == 0, "S-Boot's state: masked, not ours");
    CHECK(sbh_wdt_feed_s(0, 0, 0xFF39, 47607, b23, 0) == 0, "reset disabled: not ours");
    CHECK(sbh_wdt_feed_s(0, 0, 0xFF39, 47607, 0, b23) == 0, "reset masked: not ours");
    CHECK(sbh_wdt_feed_s(0, 0, 0xFF39, 47607, 1u << 24, 1u << 24) == 60, "cluster 0's bits are not ours");
    CHECK(sbh_wdt_feed_s(0, 0, 0, 0, 0, 0) == 0, "stopped");
    for (unsigned s = WDT_MIN_S; s <= WDT_MAX_S; ++s) {
        unsigned ms2;
        uint32_t c = sb_wdt_count(s, &ms2);
        uint32_t f = sbh_wdt_feed_s(0, 0, WDT_WTCON, c, 0, 0);
        uint32_t p = sbh_wdt_period_ms(f, sbh_wdt_hw_ms(WDT_WTCON, c));
        CHECK(f >= s - 1 && f <= s + 1 && p != 0 && 3ULL * p <= ms2, "%u s armed, no sbwdt=: feed %u s every %u ms", s, f, p);
    }
    /* other dividers / prescalers: (pre + 1) * 16 << div */
    CHECK(sbh_wdt_hw_ms(0x0021, 65000) == 40, "pre 1, /16: %u", sbh_wdt_hw_ms(0x0021, 65000));          /* 65000*16/26e6 s */
    CHECK(sbh_wdt_hw_ms(0x7f31, 63477) == 20000, "pre 128, /64: %u", sbh_wdt_hw_ms(0x7f31, 63477)); /* *8192/26e6 */
}

static void test_marker(void)
{
    char b[160];
    size_t n = sbh_marker(b, sizeof(b), SBH_INFORM3_NORMAL, 1234, 60, 41);
    CHECK(strcmp(b, "STARBITE: xnu crash v1: return loader, uptime 1234 s, watchdog 60 s fed 41 times\n") == 0, "%s", b);
    CHECK(n == strlen(b), "length");
    sbh_marker(b, sizeof(b), SBH_INFORM3_RECOVERY, 0, 0, 0);
    CHECK(strcmp(b, "STARBITE: xnu crash v1: return recovery, uptime 0 s, watchdog off\n") == 0, "%s", b);
    sbh_marker(b, sizeof(b), 0, UINT64_MAX, 82, UINT64_MAX);
    CHECK(strstr(b, "return none (spin)") && strstr(b, "18446744073709551615 times\n"), "%s", b);
    CHECK(strlen(b) < sizeof(b) - 1, "the longest marker fits: %zu", strlen(b));
    CHECK(strstr(b, "panic(") == NULL, "never a reason line");
    /* small buffers: cut, terminated, nothing written past cap (ASan) */
    for (size_t cap = 1; cap < 40; ++cap) {
        char *s = malloc(cap);
        size_t k = sbh_marker(s, cap, SBH_INFORM3_NORMAL, 99999, 60, 7);
        CHECK(k == cap - 1 && strlen(s) == cap - 1, "cap %zu: %zu", cap, k);
        free(s);
    }
}

/* XNU's dump (starlte_pmsg_save's order), then the loader's reading of it */
static unsigned dump(uint8_t *zone, const char *hdr, const char *marker, const char *plog, const char *ring,
                     uint32_t rsize, uint64_t total)
{
    volatile uint8_t *d = zone + SBH_PMSG_HDR;
    uint32_t o = 0;
    sbh_pmsg_put(d, &o, hdr, (uint32_t)strlen(hdr));
    if (marker)
        sbh_pmsg_put(d, &o, marker, (uint32_t)strlen(marker));
    if (plog)
        sbh_pmsg_put(d, &o, plog, (uint32_t)strlen(plog));
    const char *mid = plog ? MID_PANIC : MID_RESET;
    sbh_pmsg_put(d, &o, mid, (uint32_t)strlen(mid));
    sbh_pmsg_tail(d, &o, ring, rsize, total);
    sbh_pmsg_close((volatile uint32_t *)(void *)zone, o);
    return o;
}

static void judge(uint8_t *zone, uint32_t rst, uint32_t inform3, unsigned want_kind, const char *want_reason,
                  const char *what)
{
    static char t1[0x4000];
    unsigned n1 = sb_ring_text(zone, 0x4000, t1, sizeof(t1));
    /* crash.c evaluate(), the XNU part */
    int xp = sb_find(t1, n1, "STARBITE: XNU panic (sweetreboot)");
    int xh = sb_find(t1, n1, "STARBITE: sweetreboot timer (no panic)");
    int xat = xp >= 0 ? xp : xh;
    struct sb_evidence e = { 0 };
    e.rst_stat = rst;
    e.inform3 = inform3;
    e.sboot_rr = e.sboot_pwron = -1;
    e.ev = xat >= 0 ? (xp >= 0 ? EV_XNU_PANIC : EV_XNU_HANG) : 0;
    e.launched = 1;
    e.method = (uint8_t)M_XNU_;
    unsigned sev, kind = sb_classify(&e, &sev);
    CHECK(kind == want_kind, "%s: kind %s", what, sb_kind_name(kind));
    CHECK(sev == SEV_CRASH, "%s: severity %u", what, sev);
    if (want_reason) {
        char reason[96] = "";
        int at = sb_find(t1 + xat, n1 - (unsigned)xat, "panic(");
        CHECK(at >= 0, "%s: no panic( line", what);
        if (at >= 0)
            (void)sb_line_at(t1 + xat, n1 - (unsigned)xat, (unsigned)at, reason, sizeof(reason));
        CHECK(strncmp(reason, want_reason, strlen(want_reason)) == 0, "%s: reason \"%s\"", what, reason);
    }
}

static void test_dump(void)
{
    static uint8_t zone[0x4000];
    static char ring[64 * 1024];
    char marker[160];
    const char *plog = "panic(cpu 2 caller 0xfffffe0012345678): STARBITE: sbtestpanic=90: test panic "
                       "(docs/STARBITE-HEALTH.md 5.4)\nDebugger message: panic\nMemory ID: 0x6\n";
    for (unsigned i = 0; i < sizeof(ring); ++i)
        ring[i] = (char)(i % 64 == 63 ? '\n' : 'a' + i % 26);
    /* the boot-next mailbox above the data stays as it was */
    memset(zone, 0xA5, sizeof(zone));
    memcpy(zone + SBH_MAILBOX_OFF, "SBNEXT2!mac13", 14);

    /* a panic, sbcrash=loader, a short console */
    sbh_marker(marker, sizeof(marker), SBH_INFORM3_NORMAL, 321, 60, 16);
    unsigned o = dump(zone, SBH_HDR_PANIC, marker, plog, ring, 4096, 1000);
    CHECK(o == strlen(SBH_HDR_PANIC) + strlen(marker) + strlen(plog) + strlen(MID_PANIC) + 1000, "short dump %u", o);
    judge(zone, RST_SWRESET, SBH_INFORM3_NORMAL, CK_PANIC, "panic(cpu 2 caller 0xfffffe0012345678): STARBITE: sbtestpanic=90",
          "panic, sbcrash=loader");
    /* the same panic with the watchdog's reset (sbwdt alone): still a panic */
    judge(zone, RST_WDT_CL1, 0, CK_PANIC, "panic(cpu 2", "panic then watchdog");

    /* a full ring: the console fills it to exactly SBH_PMSG_DATA */
    o = dump(zone, SBH_HDR_PANIC, marker, plog, ring, sizeof(ring), 5u << 20);
    CHECK(o == SBH_PMSG_DATA, "full dump %u", o);
    uint32_t h[3];
    memcpy(h, zone, sizeof(h));
    CHECK(h[0] == RING_SIG && h[1] == 0 && h[2] == SBH_PMSG_DATA, "header %x %u %u", h[0], h[1], h[2]);
    judge(zone, RST_SWRESET, SBH_INFORM3_NORMAL, CK_PANIC, "panic(cpu 2", "full dump");

    /* the early-panic fallback: the format string only */
    o = dump(zone, SBH_HDR_PANIC, marker, "panic(unformatted): Kernel trap at 0x%016llx, type %d=%s", ring, 4096, 10);
    judge(zone, RST_SWRESET, SBH_INFORM3_NORMAL, CK_PANIC, "panic(unformatted): Kernel trap", "fallback");

    /* the sweetreboot timer: no marker, no panic log: HANG (OS TIMER) */
    o = dump(zone, SBH_HDR_TIMER, NULL, NULL, ring, 4096, 3000);
    judge(zone, RST_SWRESET, SBH_INFORM3_RECOVERY, CK_SWHANG, NULL, "sweetreboot timer");

    /* sbtesthang: nothing new in the ring (an old loader line), cluster 1's watchdog reset */
    memset(zone, 0, 16);
    judge(zone, RST_WDT_CL1, SBH_INFORM3_NORMAL, CK_WATCHDOG, NULL, "watchdog");

    CHECK(memcmp(zone + SBH_MAILBOX_OFF, "SBNEXT2!mac13", 14) == 0, "the mailbox was written");
    for (unsigned i = SBH_PMSG_HDR + SBH_PMSG_DATA; i < SBH_MAILBOX_OFF; ++i)
        if (zone[i] != 0xA5) {
            CHECK(0, "byte 0x%x past the data written", i);
            break;
        }
    /* a console ring that is empty or absent */
    o = dump(zone, SBH_HDR_PANIC, marker, plog, NULL, 0, 0);
    CHECK(o == strlen(SBH_HDR_PANIC) + strlen(marker) + strlen(plog) + strlen(MID_PANIC), "no console %u", o);
}

/* What XNU receives: the payload's boot-args, then (xnu_boot.c boot_xnu) " " + the entry's cmdline,
 * then sb_line_edit with the log fragment + " sbwdt=N" (launch_prepare), in 608 bytes. */
static void boot_args(char *cmd, const char *payload, const char *entry_cmdline, const char *frag, unsigned wdt)
{
    snprintf(cmd, 608, "%s", payload);
    if (entry_cmdline[0])
        snprintf(cmd + strlen(cmd), 608 - strlen(cmd), " %s", entry_cmdline);
    char x[CAT_LOG_MAX_ + 32];
    snprintf(x, sizeof(x), "%s sbwdt=%u", frag, wdt);      /* arg_num appends it after the fragment */
    (void)sb_line_edit(cmd, 608, x, 1);
}

static int has_tok(const char *cmd, const char *tok)
{
    size_t n = strlen(tok);
    for (const char *p = cmd; (p = strstr(p, tok)) != NULL; p += n)
        if ((p == cmd || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ' || (tok[n - 1] != '=' && p[n] == '=')))
            return 1;
    return 0;
}

/* The proposal's fragments (tools/store/catalog-xnu-health.toml) on the real payloads' boot-args. */
static void test_payload(const char *path, int expect_fits)
{
    size_t n;
    char *b = slurp(path, &n);
    if (!b || n < 0x48 + 608 || memcmp(b, "SWEETXNU", 8)) {
        printf("(skipped: %s)\n", path);
        free(b);
        return;
    }
    char line[608], cmd[608];
    memcpy(line, b + 0x48, 608);
    line[607] = 0;
    free(b);
    /* the daily entry: part 1's level-0 fragment, part 2's wdt */
    boot_args(cmd, line, "", "sbcrash=loader", 60);
    int fits = has_tok(cmd, "sbcrash=loader") && has_tok(cmd, "sbwdt=60");
    CHECK(fits == expect_fits, "%s: %zu bytes of boot-args: sbcrash/sbwdt %s", path, strlen(line),
          fits ? "appended" : "dropped (no room)");
    if (!fits)
        return;
    /* the test entries: no root, no card writes, the console on, the test switch kept */
    boot_args(cmd, line, "sbtestpanic=60", "!rd !sdrw !sdlog !sweetreboot !a5fb_noconsole !sweetquiet sbcrash=loader", 30);
    CHECK(!has_tok(cmd, "rd") && !has_tok(cmd, "sdrw") && !has_tok(cmd, "sdlog") && !has_tok(cmd, "sweetreboot") &&
          !has_tok(cmd, "a5fb_noconsole") && !has_tok(cmd, "sweetquiet"), "%s: removals: %s", path, cmd);
    CHECK(has_tok(cmd, "sbtestpanic=60") && has_tok(cmd, "sbcrash=loader") && has_tok(cmd, "sbwdt=30") &&
          has_tok(cmd, "debug=0xc") && has_tok(cmd, "wdt=-1"), "%s: kept/added: %s", path, cmd);
    CHECK(strlen(cmd) < 607, "%s: %zu bytes", path, strlen(cmd));
}

int main(int argc, char **argv)
{
    size_t n;
    char *crash_c = argc > 1 ? slurp(argv[1], &n) : NULL;
    if (!crash_c) {
        printf("usage: test shim/starlte/crash.c [MAC13.xnu IOS26.xnu IOS26SDT.xnu]\n");
        return 2;
    }
    test_constants(crash_c);
    test_crash_mode();
    test_watchdog();
    test_marker();
    test_dump();
    if (argc > 4) {
        test_payload(argv[2], 1);
        test_payload(argv[3], 1);
        test_payload(argv[4], 0);       /* 593 bytes: hazard 2 of docs/STARBITE-HEALTH.md 5.4 */
    }
    free(crash_c);
    printf("xnu-health-test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
