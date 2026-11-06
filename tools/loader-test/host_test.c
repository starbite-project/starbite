/* Host test of the loader's hardware-free health code (tools/loader-test/run.sh):
 * shim/starlte/sbfmt.c (settings record, crash log, reset-reason decision, watchdog math, ramoops
 * rings, command line edits, dates), shim/starlte/dtmerge.c (DTBH pick, S-Boot merge, DTB patch)
 * and shim/starlte/catalog.c's extension parsing, with the real files: S-Boot's FDT (identity
 * blanked), UN1CA's DS-ACK G960F DTBH, LineageOS's dt.img and the Linux DTB when present.
 *
 *   host_test OUTDIR SBOOT_FDT UN1CA_DTBH [LINEAGE_DTIMG|-] [LINUX_DTB|-]
 * OUTDIR holds the catalog sbstore2.py packed (cat.sbc) and the settings block it wrote
 * (set-py.bin); the test writes crashlog.bin, set-c-old.bin, set-c-new.bin, names-c.txt,
 * cat-c.txt and merged-*.dtb there for test_sbstore2.py and check_dtmerge.py.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sbfmt.h"
#include "dtmerge.h"
#include "catalog.h"
#include "store.h"

static unsigned nchk, nfail;
#define CHECK(c, ...) do { ++nchk; if (!(c)) { ++nfail; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *outdir;

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        return 0;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void spit(const char *name, const void *b, size_t n)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", outdir, name);
    FILE *f = fopen(p, "wb");
    if (!f || fwrite(b, 1, n, f) != n) {
        printf("cannot write %s\n", p);
        exit(2);
    }
    fclose(f);
}

static FILE *open_out(const char *name)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", outdir, name);
    FILE *f = fopen(p, "w");
    if (!f) {
        printf("cannot write %s\n", p);
        exit(2);
    }
    return f;
}

/* ---- the loader's store functions catalog.c calls ------------------------------------------- */

unsigned char host_cat_buf[64 << 10];
static uint8_t *cat_file;
static size_t cat_file_n;

int store_find(const char *name, struct store_ent *e)
{
    if (strcmp(name, "catalog") || !cat_file)
        return 0;
    memset(e, 0, sizeof(*e));
    strcpy(e->name, "catalog");
    e->size = cat_file_n;
    return 1;
}

int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap)
{
    if (e->size > cap)
        return -3;
    memcpy((void *)dst, cat_file, e->size);
    return 0;
}

/* ---- tests ------------------------------------------------------------------------------------- */

static void test_crc(void)
{
    CHECK(sb_crc32("123456789", 9) == 0xcbf43926U, "crc32 check value");
    CHECK(sb_crc32_more(sb_crc32("1234", 4), "56789", 5) == 0xcbf43926U, "crc32 in parts");
}

/* What the loader before 2026-11-06 wrote (store.c settings_save of then): zeros after next_key. */
static void old_record(uint8_t *b)
{
    memset(b, 0, 4096);
    memcpy(b, "SBSETTNG", 8);
    b[8] = 1;
    b[16] = 7;                         /* seq */
    b[20] = 5;                         /* timeout */
    b[24] = 1;                         /* SET_SD_STORE */
    strcpy((char *)b + 32, "linux-pmos");
    strcpy((char *)b + 56, "mac13");
    uint32_t c = sb_crc32(b + 16, 240);
    memcpy(b + 12, &c, 4);
}

static void test_settings(void)
{
    static uint8_t b[4096], b2[4096];
    struct star_settings s, t;
    old_record(b);
    CHECK(sbset_check(b) == 1, "old record valid");
    sbset_decode(b, &s);
    CHECK(s.seq == 7 && s.timeout_s == 5 && s.flags == 1, "old record numbers");
    CHECK(!strcmp(s.default_key, "linux-pmos") && !strcmp(s.last_key, "mac13") && !s.next_key[0], "old record keys");
    CHECK(sbset_post(&s) == SET_POST_FULL && sbset_log(&s) == 0 && s.crash_mode == SET_CRASH_ON &&
          s.crash_action == SET_AFTER_MENU && s.wdt_mode == SET_WDT_CATALOG && sbset_crash_timeout(&s) == 60,
          "old record = every new default (full POST, quiet, crash on, menu, 60 s, watchdog per entry)");
    spit("set-c-old.bin", b, 4096);

    /* every field, round trip */
    sbset_defaults(&t);
    t.seq = 99;
    t.timeout_s = 3;
    t.flags = 0;
    strcpy(t.default_key, "android");
    strcpy(t.last_key, "linux-ubuntu");
    strcpy(t.next_key, "rescue");
    sbset_set_post(&t, SET_POST_QUICK);
    sbset_set_log(&t, SET_LOG_VERBOSE);
    CHECK(t.flags == (1U << 4 | 2U << 8), "flags bits 4-5 POST, 8-9 log (0x%x)", t.flags);
    t.crash_mode = SET_CRASH_LOG;
    t.crash_detail = 3;
    t.crash_action = SET_AFTER_RETRY;
    t.crash_timeout = 12;
    t.wdt_mode = SET_WDT_FIXED;
    t.wdt_s = 45;
    sbset_encode(b2, &t);
    CHECK(sbset_check(b2) == 1, "new record valid");
    sbset_decode(b2, &s);
    CHECK(!memcmp(&s, &t, sizeof(s)), "new record round trip");
    CHECK(sbset_crash_timeout(&s) == 120, "timeout 12 -> 120 s");
    for (unsigned i = 112; i < 4096; ++i)
        if (b2[i]) {
            CHECK(0, "byte %u of a new record is not zero", i);
            break;
        }
    spit("set-c-new.bin", b2, 4096);
    /* damaged records */
    b2[40] ^= 1;
    CHECK(sbset_check(b2) == -1, "flipped bit: bad CRC");
    b2[40] ^= 1;
    b2[8] = 2;
    CHECK(sbset_check(b2) == -1, "version 2: refused");
    memset(b2, 0, 16);
    CHECK(sbset_check(b2) == 0, "no magic: no record");
    /* values from a future tool read as defaults */
    s.flags = SET_SD_STORE | 3U << SET_POST_SHIFT | SET_LOG_DEBUG << SET_LOG_SHIFT | 0xff000000U;
    CHECK(sbset_post(&s) == SET_POST_FULL && sbset_log(&s) == SET_LOG_DEBUG, "POST level 3 reads as full");
    sbset_set_post(&s, SET_POST_OFF);
    sbset_set_log(&s, SET_LOG_QUIET);
    CHECK(s.flags == (SET_SD_STORE | 2U << 4 | 0xff000000U), "setters keep the other bits (0x%x)", s.flags);
    CHECK(SET_POST_MASK == 0x30U && SET_LOG_MASK == 0x300U && SET_POST_OF(0x20U) == SET_POST_OFF &&
          SET_LOG_OF(0x300U) == SET_LOG_DEBUG, "the shared flag definitions");
    s.crash_timeout = 3;
    CHECK(sbset_crash_timeout(&s) == 30, "3 -> 30 s");
    s.crash_timeout = 200;
    CHECK(sbset_crash_timeout(&s) == 600, "capped at 600 s");
    /* what sbstore2.py settings wrote */
    char p[1024];
    size_t n;
    snprintf(p, sizeof(p), "%s/set-py.bin", outdir);
    uint8_t *py = slurp(p, &n);
    CHECK(py && n == 4096 && sbset_check(py) == 1, "sbstore2.py settings block valid");
    if (py && n == 4096 && sbset_check(py) == 1) {
        sbset_decode(py, &s);
        CHECK(sbset_post(&s) == SET_POST_QUICK && sbset_log(&s) == SET_LOG_VERBOSE && s.crash_mode == SET_CRASH_LOG &&
              s.crash_detail == 3 && s.crash_action == SET_AFTER_RETRY && sbset_crash_timeout(&s) == 120 &&
              s.wdt_mode == SET_WDT_FIXED && s.wdt_s == 45 && !strcmp(s.default_key, "linux-pmos"),
              "sbstore2.py settings fields as the loader reads them");
    }
    free(py);
}

static void make_report(struct cl_report *r, unsigned seq, unsigned kind)
{
    memset(r, 0, sizeof(*r));
    r->seq = seq;
    r->kind = (uint8_t)kind;
    r->severity = SEV_CRASH;
    r->method = M_LINUX;
    r->source = RS_DMESG;
    r->epoch_detect = 1794200000ULL + seq;
    r->epoch_launch = 1794199000ULL;
    r->epoch_event = seq & 1 ? 1794199990ULL : 0;
    snprintf(r->key, sizeof(r->key), "linux-pmos");
    snprintf(r->name, sizeof(r->name), "postmarketOS");
    for (unsigned i = 0; i < 16; ++i)
        r->reg[i] = 0x1000U * seq + i;
    snprintf(r->reason, sizeof(r->reason), "panic: report %u", seq);
    const char *t = "[   12.000000] line one\n[   12.100000] Kernel panic - not syncing: test\n";
    r->text_len = (uint16_t)cl_tail(r->text, CR_TEXT_MAX, t, (unsigned)strlen(t), &r->text_lines);
}

static void test_crashlog(void)
{
    static uint8_t log[CL_BYTES];
    static struct cl_report r, q;
    struct cl_head h, g;
    memset(log, 0, sizeof(log));
    CHECK(cl_head_decode(log, &h) == 0, "zeros: no header");
    cl_head_init(&h);
    CHECK(cl_newest(&h, 0) == -1, "empty ring");
    /* 17 reports into 15 slots: the ring wraps, the 15 newest stay */
    for (unsigned k = 1; k <= 17; ++k) {
        make_report(&r, k, k % 2 ? CK_PANIC : CK_WATCHDOG);
        unsigned slot = h.next % CL_SLOTS;
        cl_report_encode(log + CL_BLOCK * (1 + slot), &r);
        h.next = (slot + 1) % CL_SLOTS;
        ++h.count;
    }
    h.boots = 17;
    h.launch_seq = 18;
    h.launch_state = CL_RUNNING;
    h.method = M_ANDROID;
    h.wdt_armed = 1;
    h.wdt_s = 60;
    strcpy(h.key, "android");
    strcpy(h.name, "One UI 8");
    h.launch_epoch = 1794200100ULL;
    h.log_level = 1;
    h.lflags = CLF_RETRY;
    h.seen_dmesg = 0x11111111U;
    h.seen_pmsg = 0x44444444U;
    cl_head_encode(log, &h);
    CHECK(cl_head_decode(log, &g) == 1 && !memcmp(&g, &h, sizeof(g)), "header round trip");
    CHECK(cl_newest(&h, 0) == 1 && cl_newest(&h, 14) == 2 && cl_newest(&h, 15) == -1, "ring order (newest in slot 1)");
    CHECK(cl_report_decode(log + CL_BLOCK * (1 + 1), &q) == 1 && q.seq == 17, "newest is #17");
    CHECK(cl_report_decode(log + CL_BLOCK * (1 + 2), &q) == 1 && q.seq == 3, "oldest kept is #3");
    make_report(&r, 17, CK_PANIC);
    cl_report_decode(log + CL_BLOCK * 2, &q);
    CHECK(!memcmp(&q, &r, sizeof(q)), "report round trip");
    spit("crashlog.bin", log, sizeof(log));
    /* damage */
    log[CL_BLOCK * 3 + 300] ^= 0x20;
    CHECK(cl_report_decode(log + CL_BLOCK * 3, &q) == -1, "report bad CRC");
    log[100] ^= 1;
    CHECK(cl_head_decode(log, &g) == -1, "header bad CRC");
    /* the tail: last lines that fit, from a line start, printable */
    static char big[20000], out[CR_TEXT_MAX];
    unsigned n = 0;
    for (unsigned i = 0; i < 400; ++i)
        n += (unsigned)snprintf(big + n, sizeof(big) - n, "line %03u \x01\x1b[1mxyz\ttab\r\n", i);
    uint16_t lines;
    unsigned m = cl_tail(out, sizeof(out), big, n, &lines);
    CHECK(m <= sizeof(out) && m > 3000, "tail size %u", m);
    CHECK(!strncmp(out, "line ", 5) && out[m - 1] == '\n', "tail starts at a line, ends with a newline");
    CHECK(strstr(out, "line 399 ..[1mxyz tab\n") != 0, "control bytes made printable");
    CHECK(lines > 100 && lines < 200, "tail lines %u", lines);
    m = cl_tail(out, sizeof(out), "no newline at all", 17, &lines);
    CHECK(m == 18 && out[17] == '\n' && lines == 1, "a final newline is added");
}

struct ccase {
    const char *what;
    uint32_t rst, inform3;
    int32_t rr, pwron;
    uint32_t ev;
    uint8_t launched, method, wdt;
    unsigned kind, sev;
};

static void test_classify(void)
{
    static const struct ccase cs[] = {
        { "nothing ran, clean reset", RST_SWRESET, 0x12345670U, RR_R, 0x8, 0, 0, 0, 0, CK_NONE, SEV_INFO },
        { "nothing ran, watchdog", RST_WDT_CL1, 0, RR_W, -1, 0, 0, 0, 0, CK_LOADERWDT, SEV_WARN },
        { "linux panic (panic=10)", RST_SWRESET, 0x12345670U, RR_R, 0, EV_LX_PANIC, 1, M_LINUX, 1, CK_PANIC, SEV_CRASH },
        { "linux panic then the watchdog", RST_WDT_CL1, 0x12345670U, RR_W, 0, EV_LX_PANIC, 1, M_LINUX, 1, CK_PANIC, SEV_CRASH },
        { "linux hang, our watchdog", RST_WDT_CL1, 0x12345670U, -1, -1, 0, 1, M_LINUX, 1, CK_WATCHDOG, SEV_CRASH },
        { "cluster 0 watchdog", RST_WDT_CL0, 0, -1, -1, 0, 1, M_ANDROID, 0, CK_WATCHDOG, SEV_CRASH },
        { "S-Boot says watchdog only", RST_SWRESET, 0, RR_W, -1, 0, 1, M_ANDROID, 0, CK_WATCHDOG, SEV_CRASH },
        { "android panic (sec_debug)", RST_SWRESET, SEC_CAUSE_PANIC, RR_K, 0, EV_AND_PANIC, 1, M_ANDROID, 0, CK_PANIC, SEV_CRASH },
        { "android panic, S-Boot code only", RST_SWRESET, 0x12345670U, RR_K, 0, 0, 1, M_ANDROID, 0, CK_PANIC, SEV_CRASH },
        { "xnu panic dump", RST_SWRESET, 0x12345674U, -1, -1, EV_XNU_PANIC, 1, M_XNU, 0, CK_PANIC, SEV_CRASH },
        { "xnu sweetreboot timer", RST_SWRESET, 0x12345670U, -1, -1, EV_XNU_HANG, 1, M_XNU, 0, CK_SWHANG, SEV_CRASH },
        { "forced restart (MPON)", RST_SWRESET, 0x12345670U, RR_M, 0x8, 0, 1, M_LINUX, 0, CK_FORCED, SEV_WARN },
        { "forced restart (WRESET)", RST_WRESET, 0, -1, -1, 0, 1, M_UEFI, 0, CK_FORCED, SEV_WARN },
        { "linux oops, then a clean reboot", RST_SWRESET, 0x12345670U, RR_R, 0, EV_LX_OOPS, 1, M_LINUX, 1, CK_OOPS, SEV_WARN },
        { "S-Boot fault SPON", RST_SWRESET, 0, RR_S, 0, 0, 1, M_ANDROID, 0, CK_FAULT, SEV_CRASH },
        { "S-Boot TPON", RST_SWRESET, 0, RR_T, 0, 0, 1, M_ANDROID, 0, CK_FAULT, SEV_WARN },
        { "clean reboot", RST_SWRESET, 0x12345670U, RR_R, 0, 0, 1, M_LINUX, 1, CK_CLEAN, SEV_INFO },
        { "reboot to recovery", RST_SWRESET, 0x12345674U, -1, -1, 0, 1, M_XNU, 0, CK_CLEAN, SEV_INFO },
        { "power off, power on", RST_PINRESET, 0, RR_N, 0x1, 0, 1, M_LINUX, 1, CK_POWER, SEV_INFO },
        { "power lost (SMPL)", RST_PINRESET, 0, RR_N, 0x40, 0, 1, M_LINUX, 1, CK_POWER, SEV_WARN },
        { "nothing explains it", 0, 0, -1, -1, 0, 1, M_BARE, 0, CK_UNKNOWN, SEV_WARN },
    };
    for (unsigned i = 0; i < sizeof(cs) / sizeof(cs[0]); ++i) {
        const struct ccase *c = &cs[i];
        struct sb_evidence e = { c->rst, c->inform3, c->rr, c->pwron, c->ev, c->launched, c->method, c->wdt };
        unsigned sev = 99, k = sb_classify(&e, &sev);
        CHECK(k == c->kind && sev == c->sev, "%s: kind %u sev %u, want %u %u", c->what, k, sev, c->kind, c->sev);
    }
    CHECK(sb_classify(&(struct sb_evidence){ RST_SWRESET, 0, -1, -1, 0, 1, M_LINUX, 0 }, 0) == CK_CLEAN, "no severity pointer");
}

static void test_names(void)
{
    FILE *f = open_out("names-c.txt");
    for (unsigned k = 0; k <= 10; ++k)
        fprintf(f, "kind %u %s\n", k, sb_kind_name(k));
    for (int r = 1; r <= 10; ++r)
        fprintf(f, "rr %d %s\n", r, sb_rr_name(r));
    for (unsigned b = 0; b < 32; ++b)
        if (sb_rst_bit_name(b))
            fprintf(f, "rst %u %s\n", b, sb_rst_bit_name(b));
    static const uint32_t i3[] = { 0, 0x12345670U, 0x12345671U, 0x12345672U, 0x12345673U, 0x12345674U, 0x12345675U,
                                   0x1234567dU, 0xC8U, 0x66U, 0x85U, 0xCAFEBABEU };
    for (unsigned i = 0; i < sizeof(i3) / sizeof(i3[0]); ++i)
        fprintf(f, "inform3 0x%x %s\n", i3[i], sb_inform3_name(i3[i]));
    for (unsigned s = 0; s <= 5; ++s)
        fprintf(f, "source %u %s\n", s, sb_source_name(s));
    fclose(f);
    CHECK(!strcmp(sb_rst_bit_name(29), "SWRESET") && !strcmp(sb_rst_bit_name(23), "CLUSTER1_WDTRESET") &&
          !strcmp(sb_rst_bit_name(24), "CLUSTER0_WDTRESET") && !sb_rst_bit_name(27) && !sb_rst_bit_name(17),
          "RST_STAT bit names");
    CHECK(!strcmp(sb_rr_name(-1), "NONE") && !strcmp(sb_rr_name(RR_K), "KPON KERNEL PANIC"), "S-Boot reset codes");
    CHECK(sb_inform3_name(0x12345679U) == 0, "unknown INFORM3");
}

static void test_wdt(void)
{
    unsigned ms;
    CHECK(sb_wdt_count(30, &ms) == 23803 && ms == 29999, "30 s = 23803 counts (%u ms)", ms);
    CHECK(sb_wdt_count(60, &ms) == 47607 && ms / 1000 == 59, "60 s = 47607 counts");
    CHECK(sb_wdt_count(82, &ms) == 65063 && ms == 81999, "82 s = 65063 counts, the most");
    CHECK(sb_wdt_count(83, &ms) == 65063, "83 s clamps to 82");
    CHECK(sb_wdt_count(3000, 0) == 65063, "3000 s clamps to 82");
    CHECK(sb_wdt_count(5, &ms) == 7934 && ms / 1000 == 9, "5 s clamps to 10");
    CHECK(sb_wdt_count(0, &ms) == 0 && ms == 0, "0 = off");
    CHECK(WDT_WTCON == (255U << 8 | 3U << 3 | 1U << 5 | 1U), "WTCON 0xFF39");
    CHECK((uint64_t)0xffff * 256 * 128 / WDT_CLK_HZ == WDT_MAX_S, "the 16-bit counter's limit is 82 s");
}

static void ring(uint8_t *z, unsigned zone, uint32_t start, uint32_t size, char fill_base)
{
    memset(z, 0, zone);
    uint32_t sig = RING_SIG;
    memcpy(z, &sig, 4);
    memcpy(z + 4, &start, 4);
    memcpy(z + 8, &size, 4);
    for (unsigned i = 0; i < zone - 12; ++i)
        z[12 + i] = (uint8_t)(fill_base + i % 26);
}

static void test_rings(void)
{
    static uint8_t z[0x4000];
    static char out[0x4000];
    ring(z, sizeof(z), 5, 5, 'a');
    CHECK(sb_ring_text(z, sizeof(z), out, sizeof(out)) == 5 && !memcmp(out, "abcde", 5), "linear ring");
    ring(z, sizeof(z), 100, 0x3ff4, 'a');                 /* Linux, full: oldest at start */
    unsigned n = sb_ring_text(z, sizeof(z), out, sizeof(out));
    CHECK(n == 0x3ff4 && out[0] == (char)('a' + 100 % 26) && out[n - 1] == (char)('a' + 99 % 26), "Linux ring wrapped");
    ring(z, sizeof(z), 10, RING_PMSG, 'A');                /* the loader's 0x3F00 ring, full */
    n = sb_ring_text(z, sizeof(z), out, sizeof(out));
    CHECK(n == RING_PMSG && out[0] == (char)('A' + 10) && out[RING_PMSG - 10] == 'A', "pmsg ring wrapped at 0x3F00");
    ring(z, sizeof(z), 0x4000, 4, 'a');
    CHECK(sb_ring_text(z, sizeof(z), out, sizeof(out)) == 0, "start past the data: no ring");
    z[0] = 0;
    CHECK(sb_ring_text(z, sizeof(z), out, sizeof(out)) == 0, "no DBGC: no ring");
    ring(z, sizeof(z), 3, 3, 'a');
    CHECK(sb_ring_text(z, sizeof(z), out, 2) == 2, "output capped");
}

static void test_text(void)
{
    char l[128];
    strcpy(l, "debug=0xc serial=3 -v keepsyms=1 a5fb_noconsole=1 sweetquiet=1");
    unsigned n = sb_line_edit(l, sizeof(l), "!-v !serial !a5fb_noconsole !sweetquiet serial=0 -x", 1);
    CHECK(!strcmp(l, "debug=0xc keepsyms=1 serial=0 -x") && n == strlen(l), "remove and append: %s", l);
    strcpy(l, "a b");
    sb_line_edit(l, sizeof(l), "!a c", 0);
    CHECK(!strcmp(l, "a b c"), "without removal '!' tokens are dropped: %s", l);
    strcpy(l, "");
    sb_line_edit(l, sizeof(l), "  quiet   loglevel=3 ", 0);
    CHECK(!strcmp(l, "quiet loglevel=3"), "spaces: %s", l);
    char s[12] = "abc";
    sb_line_edit(s, sizeof(s), "defg hijklmn", 0);
    CHECK(!strcmp(s, "abc defg"), "stops at the cap: %s", s);
    strcpy(l, "-verbose -v");
    sb_line_edit(l, sizeof(l), "!-v", 1);
    CHECK(!strcmp(l, "-verbose"), "'!-v' leaves -verbose: %s", l);

    char v[32];
    int64_t x;
    const char *args = "console=ram sec_debug.reset_reason=5 sec_debug.rst_stat=0x20000000 xsec_debug.pwronsrc=9 sec_debug.pwronsrc=0x8";
    CHECK(sb_arg(args, (unsigned)strlen(args), "sec_debug.reset_reason", v, sizeof(v)) && sb_num(v, &x) && x == 5, "reset_reason");
    CHECK(sb_arg(args, (unsigned)strlen(args), "sec_debug.rst_stat", v, sizeof(v)) && sb_num(v, &x) && x == 0x20000000, "rst_stat");
    CHECK(sb_arg(args, (unsigned)strlen(args), "sec_debug.pwronsrc", v, sizeof(v)) && sb_num(v, &x) && x == 8, "word boundary");
    CHECK(!sb_arg(args, (unsigned)strlen(args), "sec_debug.reset", v, sizeof(v)), "prefix is not a key");
    CHECK(sb_num("-12", &x) && x == -12 && !sb_num("12x", &x) && !sb_num("", &x) && !sb_num("0x", &x), "numbers");

    char d[20];
    sb_date(0, d);
    CHECK(!strcmp(d, "UNKNOWN TIME"), "no time");
    sb_date(1794200000ULL, d);
    CHECK(!strcmp(d, "2026-11-09 04:53"), "date %s", d);
    sb_date(951782400ULL, d);
    CHECK(!strcmp(d, "2000-02-29 00:00"), "leap day %s", d);
    sb_date(1709164800ULL, d);
    CHECK(!strcmp(d, "2024-02-29 00:00"), "leap day 2024 %s", d);
    sb_date(4102444800ULL, d);
    CHECK(!strcmp(d, "2100-01-01 00:00"), "2100 %s", d);
    const char *rec = "====1794200000.123456-D\nPanic#1 Part1\n<0>[ 5.0] Kernel panic - not syncing: VFS: Unable to mount root\n";
    CHECK(sb_dmesg_time(rec, (unsigned)strlen(rec)) == 1794200000ULL, "ramoops record time");
    CHECK(sb_dmesg_time("====x", 5) == 0 && sb_dmesg_time("Panic", 5) == 0, "not a record");
    CHECK(sb_line_after(rec, (unsigned)strlen(rec), "Kernel panic - not syncing: ", v, sizeof(v)) &&
          !strcmp(v, "VFS: Unable to mount root"), "panic reason: %s", v);
    CHECK(sb_find(rec, (unsigned)strlen(rec), "Panic#") == 24, "find");
    CHECK(sb_line_at(rec, (unsigned)strlen(rec), 30, v, sizeof(v)) == 13 && !strcmp(v, "Panic#1 Part1"), "line at");
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void merge_one(const char *tag, const uint8_t *blob, size_t n, const uint8_t *sb, const char *extra)
{
    uint32_t chip, rev, off, fsz;
    unsigned idx, cnt;
    CHECK(fdt_board_id(sb, &chip, &rev) && chip == 0x2652 && rev == 26, "%s: S-Boot board 0x%x rev %u", tag, chip, rev);
    CHECK(dtbh_pick(blob, n, chip, rev, &off, &fsz, &idx, &cnt) && off == 0x800 && idx == 0 && cnt == 1 &&
          fsz <= n - off, "%s: DTBH entry 0 at 0x800", tag);
    static uint8_t out[2 << 20];
    uint32_t m = fdt_merge_sboot(blob + off, sb, out, sizeof(out), extra);
    CHECK(m > fsz && be32(out) == 0xd00dfeedU && be32(out + 4) == m, "%s: merged %u bytes", tag, m);
    char name[64];
    snprintf(name, sizeof(name), "merged-%s.dtb", tag);
    spit(name, out, m);
    snprintf(name, sizeof(name), "base-%s.dtb", tag);
    spit(name, blob + off, fsz);
    /* the merge is a function of its inputs */
    static uint8_t out2[2 << 20];
    CHECK(fdt_merge_sboot(blob + off, sb, out2, sizeof(out2), extra) == m && !memcmp(out, out2, m), "%s: repeatable", tag);
    CHECK(fdt_merge_sboot(blob + off, sb, out2, m - 1, extra) == 0, "%s: one byte short: refused", tag);
}

static void test_dtmerge(const char *sbp, const char *un1p, const char *linp, const char *lxp)
{
    size_t ns, nu, nl, nx;
    uint8_t *sb = slurp(sbp, &ns), *un = slurp(un1p, &nu);
    CHECK(sb && un, "fixtures %s %s", sbp, un1p);
    if (!sb || !un)
        return;
    uint32_t off, fsz;
    unsigned idx, cnt;
    CHECK(dtbh_pick(un, nu, 0x2652, 26, &off, &fsz, &idx, &cnt) && cnt == 1, "UN1CA DTBH");
    CHECK(dtbh_pick(un, nu, 0x1234, 3, &off, &fsz, &idx, &cnt) && off == 0x800, "no match: the first entry");
    CHECK(dtbh_pick(sb, ns, 0x2652, 26, &off, &fsz, &idx, &cnt) && off == 0 && cnt == 0 && fsz == ns, "a plain FDT");
    CHECK(!dtbh_pick(un, 100, 0x2652, 26, &off, &fsz, &idx, &cnt), "truncated DTBH refused");
    uint8_t junk[64] = "DTBH";
    CHECK(!dtbh_pick(junk, sizeof(junk), 0x2652, 26, &off, &fsz, &idx, &cnt), "empty DTBH refused");
    merge_one("un1ca", un, nu, sb, "androidboot.selinux=permissive sb.test=1");
    merge_one("un1ca-noextra", un, nu, sb, "");
    if (strcmp(linp, "-") && (nl = 0, 1)) {
        uint8_t *li = slurp(linp, &nl);
        CHECK(li != 0, "LineageOS dt.img %s", linp);
        if (li)
            merge_one("lineage", li, nl, sb, "androidboot.selinux=permissive");
        free(li);
    } else {
        printf("skip: no LineageOS dt.img\n");
    }
    if (strcmp(lxp, "-")) {
        uint8_t *lx = slurp(lxp, &nx);
        CHECK(lx != 0, "Linux DTB %s", lxp);
        if (lx) {
            CHECK(fdt_patch_pairs(lx, "s9p,set-registers", 0x1406080cU, 0x12345674U, 0x12345670U) == 1, "INFORM3 patched once");
            CHECK(fdt_patch_pairs(lx, "s9p,set-registers", 0x1406080cU, 0x12345674U, 0x12345670U) == 0, "nothing left to patch");
            CHECK(fdt_patch_pairs(lx, "s9p,set-registers", 0x14060808U, 0x12345678U, 0x12345678U) == 1, "INFORM2 pair seen");
            spit("linux-patched.dtb", lx, nx);
        }
        free(lx);
    } else {
        printf("skip: no Linux DTB\n");
    }
    free(sb);
    free(un);
}

/* SBMAINT1 headers judged by tools/maint/sbmaint.py (gen_maint.py): sbm_check must agree. */
static void test_maint(void)
{
    char p[1024], line[512];
    snprintf(p, sizeof(p), "%s/maint-cases.txt", outdir);
    FILE *f = fopen(p, "r");
    CHECK(f != 0, "maint-cases.txt from gen_maint.py");
    if (!f)
        return;
    unsigned cases = 0, valid_seen = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[64];
        unsigned long long lim, v[15];
        if (!strncmp(line, "slotb ", 6)) {
            unsigned long long size, b;
            if (sscanf(line + 6, "%llu %llu", &size, &b) == 2)
                CHECK(sbm_slot_b(size) == b, "slot B of %llu: %llu, want %llu", size, (unsigned long long)sbm_slot_b(size), b);
            continue;
        }
        if (!strncmp(line, "order ", 6)) {
            int va, vb, prev;
            unsigned sa, sb;
            char want[8];
            if (sscanf(line + 6, "%d %d %u %u %d %7s", &va, &vb, &sa, &sb, &prev, want) == 6) {
                int valid[2] = { va, vb };
                uint32_t seq[2] = { sa, sb };
                unsigned o[2];
                unsigned n = sbm_order(valid, seq, prev, o);
                char got[4] = "-";
                for (unsigned k = 0; k < n; ++k)
                    got[k] = o[k] ? 'b' : 'a';
                if (n)
                    got[n] = 0;
                CHECK(!strcmp(got, want), "slot order %s: %s, want %s", line + 6, got, want);
            }
            continue;
        }
        if (sscanf(line, "%63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", name, &lim,
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13],
                   &v[14]) != 17)
            continue;
        size_t n;
        snprintf(p, sizeof(p), "%s/maint-%s.bin", outdir, name);
        uint8_t *h = slurp(p, &n);
        if (!h || n != SBM_HDR) {
            CHECK(0, "maint header %s", name);
            free(h);
            continue;
        }
        struct sbm_info m;
        const char *why = 0;
        int ok = sbm_check(h, lim, &m, &why);
        ++cases;
        CHECK(ok == (int)v[0], "SBMAINT1 %s: loader %s (%s), reference %s", name, ok ? "valid" : "invalid", ok ? "" : why,
              v[0] ? "valid" : "invalid");
        if (ok && v[0]) {
            ++valid_seen;
            int same = m.seq == v[1] && m.total == v[2];
            for (unsigned k = 0; k < 4; ++k)
                same &= m.off[k] == v[3 + 3 * k] && m.size[k] == v[4 + 3 * k] && m.crc[k] == v[5 + 3 * k];
            CHECK(same, "SBMAINT1 %s: fields as the reference reads them", name);
        }
        if (!strcmp(name, "valid"))
            CHECK(ok && !strcmp(m.cmdline, "console=tty0 sb.x=1") && !strcmp(m.version, "sbmaint test"), "cmdline, version");
        free(h);
    }
    fclose(f);
    CHECK(cases >= 15 && valid_seen == 2, "maint cases %u, valid %u", cases, valid_seen);
}

static void dump_catalog(const char *name)
{
    FILE *f = open_out(name);
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *e = cat_get(i);
        fprintf(f, "%s|%u|%u|%u|%u|%s|%s|%s|%s|%s\n", e->key, e->method, e->wdt_s, e->wdt_feed, e->cflags, e->log[0],
                e->log[1], e->log[2], e->log[3], e->file[3]);
    }
    fclose(f);
}

static void test_catalog(void)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/cat.sbc", outdir);
    cat_file = slurp(p, &cat_file_n);
    CHECK(cat_file != 0, "cat.sbc from sbstore2.py");
    if (!cat_file)
        return;
    CHECK(cat_load() > 10 && !cat_builtin(), "catalog parsed (%u entries)", cat_count());
    dump_catalog("cat-c.txt");
    int i = cat_find("linux-pmos");
    CHECK(i >= 0 && cat_get((unsigned)i)->wdt_s == 60 && cat_get((unsigned)i)->wdt_feed == CW_KERNEL &&
          (cat_get((unsigned)i)->cflags & CE_LINUX_INFORM3) && !strncmp(cat_get((unsigned)i)->log[0], "quiet ", 6),
          "linux-pmos: defaults.linux applied");
    i = cat_find("linux-bringup");
    CHECK(i >= 0 && !cat_get((unsigned)i)->log[0][0] && !cat_get((unsigned)i)->wdt_s, "linux-bringup overrides");
    i = cat_find("lab-failtest");
    CHECK(i >= 0 && (cat_get((unsigned)i)->cflags & CE_NO_LAUNCH), "lab-failtest: no launch record");
    i = cat_find("mac13");
    CHECK(i >= 0 && !strcmp(cat_get((unsigned)i)->log[2], "!a5fb_noconsole !sweetquiet"), "xnu fragments");
}

/* catalog.c parses once per process: the old-format and damaged catalogs run in child processes */
static int catalog_variant(const char *mode)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s/cat.sbc", outdir);
    cat_file = slurp(p, &cat_file_n);
    if (!cat_file)
        return 1;
    uint32_t ext;
    memcpy(&ext, cat_file + 28, 4);
    if (!strcmp(mode, "old"))
        memset(cat_file + 28, 0, 4);          /* a catalog packed before 2026-11-06 */
    else
        cat_file[ext] ^= 0xff;                /* a damaged SBCATEX1 magic */
    if (cat_load() < 10 || cat_builtin())
        return 2;
    for (unsigned k = 0; k < cat_count(); ++k) {
        const struct cat_ent *e = cat_get(k);
        if (e->log[0][0] || e->log[3][0] || e->wdt_s || e->wdt_feed || e->cflags)
            return 3;
    }
    return !(cat_find("linux-pmos") >= 0 && cat_find("mac13") >= 0) ? 4 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--catalog-variant")) {
        outdir = argv[2];
        return catalog_variant(argv[3]);
    }
    if (argc < 4) {
        printf("usage: host_test OUTDIR SBOOT_FDT UN1CA_DTBH [LINEAGE_DTIMG|-] [LINUX_DTB|-]\n");
        return 2;
    }
    outdir = argv[1];
    test_crc();
    test_settings();
    test_crashlog();
    test_classify();
    test_names();
    test_wdt();
    test_rings();
    test_text();
    test_dtmerge(argv[2], argv[3], argc > 4 ? argv[4] : "-", argc > 5 ? argv[5] : "-");
    test_maint();
    test_catalog();
    char cmd[2048];
    for (unsigned v = 0; v < 2; ++v) {
        snprintf(cmd, sizeof(cmd), "'%s' --catalog-variant '%s' %s", argv[0], outdir, v ? "damaged" : "old");
        int r = system(cmd);
        CHECK(r == 0, "catalog %s: extension ignored, entries kept (exit %d)", v ? "with a damaged extension" : "without the extension", r);
    }
    printf("%u checks, %u failed\n", nchk, nfail);
    return nfail != 0;
}
