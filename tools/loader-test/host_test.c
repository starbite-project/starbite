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
#include "crashdoc.h"

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
    /* [defaults.linux] wdt = 60: a watchdog reset was seen to reach the loader on 2026-11-06 (catalog.toml) */
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

/* ---- the watchdog's arguments (xnu_boot.c: not armed when they did not fit) -------------------- */

static void test_wdt_args(void)
{
    /* XNU: boot-args of BOOT_LINE_LENGTH 608 (607 + NUL), as boot_xnu edits them */
    static char cmd[608];
    memset(cmd, 0, sizeof(cmd));
    for (unsigned i = 0; i < 590; i += 10)
        memcpy(cmd + i, "debug=0xc ", 10);
    cmd[589] = 0;                            /* 589 bytes: ios26-sdt is 593 */
    sb_line_edit(cmd, sizeof(cmd), "sbcrash=loader sbwdt=60", 1);
    CHECK(!sb_wdt_args_ok(cmd, sizeof(cmd), SB_WDT_XNU, 60), "XNU: sbwdt=60 dropped (full boot-args): not armed");
    strcpy(cmd, "-v debug=0xc rd=disk0s1 sweetquiet=1");
    sb_line_edit(cmd, sizeof(cmd), "sbcrash=loader sbwdt=60", 1);
    CHECK(sb_wdt_args_ok(cmd, sizeof(cmd), SB_WDT_XNU, 60), "XNU: sbwdt=60 in: armed (%s)", cmd);
    CHECK(!sb_wdt_args_ok(cmd, sizeof(cmd), SB_WDT_XNU, 30), "XNU: another value is not ours");
    CHECK(!sb_wdt_args_ok(cmd, sizeof(cmd), SB_WDT_XNU, 0), "no watchdog asked: nothing to arm");
    strcpy(cmd, "sbwdt=60 a=1 sbwdt=0");
    CHECK(!sb_wdt_args_ok(cmd, sizeof(cmd), SB_WDT_XNU, 60), "XNU: the last sbwdt= wins (0: never fed)");
    strcpy(cmd, "xsbwdt=60 sbwdt=600");
    CHECK(!sb_wdt_args_ok(cmd, sizeof(cmd), SB_WDT_XNU, 60), "XNU: word boundary, value");
    char v[16];
    CHECK(sb_arg_last("a=1 b=2 a=3", 11, "a", v, sizeof(v)) && !strcmp(v, "3") && !sb_arg_last("ab=1", 4, "a", v, sizeof(v)),
          "last key=value");
    /* Linux: the entry's cmdline (catalog.h cmdline[512]) after the DTB's own bootargs */
    static char lx[512];
    strcpy(lx, "sb.root=card sb.distro=pmos");
    sb_line_edit(lx, sizeof(lx), "quiet loglevel=3", 0);
    sb_line_edit(lx, sizeof(lx), "s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=60", 0);
    CHECK(sb_wdt_args_ok(lx, sizeof(lx), SB_WDT_LINUX, 60), "Linux: both arguments: armed");
    char dtb_and_entry[700];
    snprintf(dtb_and_entry, sizeof(dtb_and_entry), "console=tty0 loglevel=4 s3c2410_wdt.tmr_atboot=0 %s", lx);
    CHECK(sb_wdt_args_ok(dtb_and_entry, (unsigned)strlen(dtb_and_entry), SB_WDT_LINUX, 60),
          "Linux: the DTB's tmr_atboot=0 comes first, ours wins");
    memset(lx, 0, sizeof(lx));
    for (unsigned i = 0; i < 480; i += 8)
        memcpy(lx + i, "sb.x=12 ", 8);
    lx[479] = 0;
    sb_line_edit(lx, sizeof(lx), "s3c2410_wdt.tmr_atboot=1", 0);
    sb_line_edit(lx, sizeof(lx), "s3c2410_wdt.tmr_margin=60", 0);
    CHECK(!sb_wdt_args_ok(lx, sizeof(lx), SB_WDT_LINUX, 60), "Linux: tmr_margin dropped (long cmdline): not armed");
    strcpy(lx, "s3c2410_wdt.tmr_margin=60");
    CHECK(!sb_wdt_args_ok(lx, sizeof(lx), SB_WDT_LINUX, 60), "Linux: no tmr_atboot=1: the driver stops it, not armed");
    strcpy(lx, "s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=60 s3c2410_wdt.tmr_atboot=0");
    CHECK(!sb_wdt_args_ok(lx, sizeof(lx), SB_WDT_LINUX, 60), "Linux: a later tmr_atboot=0 wins");
}

/* ---- POST: the logo, the live log, skipping, the failure screen (post.c through sbfmt.c pv_*) -- */

/* Polls every step ms from t0 to t1 with the keys given; the events seen, OR'ed. */
static unsigned pv_run(struct pv_keys *k, int up, int down, uint64_t t0, uint64_t t1, unsigned step)
{
    unsigned ev = 0;
    for (uint64_t t = t0; t < t1; t += step)
        ev |= pv_poll(k, up, down, t);
    return ev;
}

static void test_post_view(void)
{
    struct pv_keys k;
    /* Volume Up held from power on (fastboot's gesture, checked after POST): the logo stays */
    pv_init(&k, PV_LOGO, 1, 0);
    CHECK(pv_run(&k, 1, 0, 0, 10000, 4) == 0 && k.view == PV_LOGO && !k.up_used && k.up_blocked,
          "Volume Up held from the start: no log, still fastboot's");
    CHECK(pv_end(&k, 0) == PV_END_GO, "... and straight on to fastboot's own check");
    /* ... a 10 ms bounce in it is not a release */
    CHECK(pv_run(&k, 0, 0, 10000, 10010, 4) == 0 && pv_run(&k, 1, 0, 10010, 12000, 4) == 0 && k.view == PV_LOGO,
          "a bounce (released 10 ms) does not make it a new press");
    /* ... released for real, then pressed again: that press is made during POST */
    CHECK(pv_run(&k, 0, 0, 12000, 12100, 4) == 0 && !k.up_blocked, "released 100 ms: unblocked");
    unsigned ev = pv_run(&k, 1, 0, 12100, 12300, 4);
    CHECK(ev == PV_EV_LOG && k.view == PV_LOG && k.up_used, "pressed again during POST: the log");

    /* a press that starts during POST, debounced */
    pv_init(&k, PV_LOGO, 0, 0);
    CHECK(pv_run(&k, 0, 0, 0, 3000, 5) == 0, "no keys: nothing");
    CHECK(pv_run(&k, 1, 0, 3000, 3030, 5) == 0 && k.view == PV_LOGO, "30 ms of Volume Up: not yet");
    CHECK(pv_run(&k, 0, 0, 3030, 3100, 5) == 0 && k.view == PV_LOGO, "a 30 ms blip never opens the log");
    ev = pv_run(&k, 1, 0, 3100, 3200, 5);
    CHECK(ev == PV_EV_LOG && k.view == PV_LOG, "40 ms held: the log (once)");
    CHECK(pv_run(&k, 0, 0, 3200, 4000, 5) == 0 && pv_run(&k, 1, 0, 4000, 4500, 5) == 0 && k.view == PV_LOG,
          "released: it stays in the log, no second event");
    CHECK(pv_end(&k, 0) == PV_END_READ, "the log view ends with a pause to read");
    CHECK(pv_hold(&k, 0, 0, 0) && pv_hold(&k, 0, 0, PV_READ_MS - 1) && !pv_hold(&k, 0, 0, PV_READ_MS),
          "the pause: PV_READ_MS");
    CHECK(pv_hold(&k, 1, 0, 30000) && !pv_hold(&k, 1, 0, PV_HOLD_MAX_MS), "Volume Up held: longer, capped");
    CHECK(!pv_hold(&k, 1, 1, 100), "Power / Volume Down end it at once");
    CHECK(pv_end(&k, 1) == PV_END_FAIL, "a red check: the failure screen, log or not");

    /* sparse polls (a long check between them): two reads 400 ms apart still count */
    pv_init(&k, PV_LOGO, 0, 0);
    CHECK(pv_poll(&k, 1, 0, 1000) == 0 && pv_poll(&k, 1, 0, 1400) == PV_EV_LOG, "sparse polls");

    /* log level 2-3: the log from the start; fastboot's held Volume Up does not hold it on screen */
    pv_init(&k, PV_LOG, 1, 0);
    CHECK(pv_run(&k, 1, 0, 0, 9000, 5) == 0 && k.view == PV_LOG && !k.up_used, "log from the start, Up held: no event");
    CHECK(pv_end(&k, 0) == PV_END_READ && !pv_hold(&k, 1, 0, PV_READ_MS + 10), "fastboot's Up does not keep the log up");

    /* quick POST: nothing drawn until Volume Up */
    pv_init(&k, PV_NONE, 0, 0);
    CHECK(pv_end(&k, 0) == PV_END_GO, "quick, nothing pressed: straight on");
    CHECK(pv_run(&k, 1, 0, 0, 100, 5) == PV_EV_LOG && k.view == PV_LOG, "quick: Volume Up shows the lines");

    /* Volume Down skips, once; it may be held from the start */
    pv_init(&k, PV_LOGO, 0, 0);
    CHECK(pv_run(&k, 0, 1, 0, 15, 5) == 0 && !k.skip, "Volume Down 15 ms: not yet");
    CHECK(pv_run(&k, 0, 1, 15, 100, 5) == PV_EV_SKIP && k.skip, "Volume Down held: skip (once)");
    CHECK(pv_run(&k, 1, 1, 100, 300, 5) == PV_EV_LOG && k.skip, "Volume Up still opens the log after a skip");
    CHECK(pv_end(&k, 0) == PV_END_READ, "skipped, in the log: the pause");
    pv_init(&k, PV_LOGO, 0, 0);
    CHECK(pv_run(&k, 0, 1, 0, 50, 5) == PV_EV_SKIP && pv_end(&k, 0) == PV_END_GO, "skipped behind the logo: straight on");
    CHECK(pv_end(&k, 2) == PV_END_FAIL, "... unless a check failed before the skip");

    /* the failure screen's list: failures only, in display order */
    static const uint8_t order[8] = { 0, 1, 11, 8, 2, 7, 12, 9 };
    uint8_t st[16] = { 0 }, ids[3];
    for (unsigned i = 0; i < 16; ++i)
        st[i] = 1;                                   /* POST_OK */
    CHECK(pv_failures(st, order, 8, 3, ids, 3) == 0, "clean: no failure screen");
    st[9] = 3;                                       /* touch FAIL */
    st[2] = 2;                                       /* PMIC WARN: never on it */
    st[1] = 3;                                       /* timer FAIL */
    unsigned nf = pv_failures(st, order, 8, 3, ids, 3);
    CHECK(nf == 2 && ids[0] == 1 && ids[1] == 9, "two failures in display order (%u)", nf);
    st[11] = 3;
    st[12] = 3;
    st[7] = 3;
    nf = pv_failures(st, order, 8, 3, ids, 3);
    CHECK(nf == 5 && ids[0] == 1 && ids[1] == 11 && ids[2] == 7, "five failed, three shown: %u", nf);
    st[15] = 3;
    CHECK(pv_failures(st, order, 8, 3, ids, 3) == 5, "a check not in this mode's order is not counted");
}

/* ---- the crash screen's parser and the saved excerpt, on real-looking captures ----------------- */

static unsigned kinds_of(const char *t, unsigned n, unsigned want)
{
    struct sb_crashinfo ci;
    static uint8_t k[4096];
    unsigned nl = sb_crash_parse(t, n, &ci, k, sizeof(k)), c = 0;
    for (unsigned i = 0; i < nl && i < sizeof(k); ++i)
        c += k[i] == want;
    return c;
}

/* The kind of the line that contains needle. */
static int kind_at(const char *t, unsigned n, const char *needle)
{
    struct sb_crashinfo ci;
    static uint8_t k[4096];
    unsigned nl = sb_crash_parse(t, n, &ci, k, sizeof(k)), line = 0;
    int at = sb_find(t, n, needle);
    if (at < 0)
        return -1;
    for (int i = 0; i < at; ++i)
        line += t[i] == '\n';
    return line < nl ? k[line] : -1;
}

/* The text through a Linux persistent_ram zone (16 KiB, "DBGC", full and wrapped when it is long),
 * back out with sb_ring_text, as crash.c gets it. */
static unsigned through_ring(const char *t, unsigned n, char *out, unsigned cap, unsigned rot)
{
    static uint8_t z[0x4000];
    const unsigned data = sizeof(z) - 12;
    memset(z, 0, sizeof(z));
    memcpy(z, "DBGC", 4);
    if (n > data) {
        t += n - data;
        n = data;
    }
    unsigned start = n == data ? rot % data : n;      /* not full: data at 0..n, next write at n */
    for (unsigned i = 0; i < n; ++i)
        z[12 + (n == data ? (start + i) % data : i)] = (uint8_t)t[i];   /* full: oldest byte at start */
    uint32_t st = start % data, sz = n;
    memcpy(z + 4, &st, 4);
    memcpy(z + 8, &sz, 4);
    return sb_ring_text(z, sizeof(z), out, cap);
}

static void test_crash_parse(const char *fxdir)
{
    char p[1100];                   /* fxdir (under 1024) + a file name */
    size_t n;
    struct sb_crashinfo ci;
    static char ring_text[0x4000], ex[CR_TEXT_MAX];
    uint16_t lines;

    /* Linux: an oops that panics (lkdtm EXCEPTION), the ramoops dmesg record */
    snprintf(p, sizeof(p), "%s/linux-oops-panic.txt", fxdir);
    char *t = (char *)slurp(p, &n);
    CHECK(t != 0, "fixture %s", p);
    if (t) {
        unsigned rn = through_ring(t, (unsigned)n, ring_text, sizeof(ring_text), 0);
        CHECK(rn == n && !memcmp(ring_text, t, n), "oops: through a ramoops zone unchanged");
        unsigned nl = sb_crash_parse(ring_text, rn, &ci, 0, 0);
        CHECK(nl == 121 && ci.lines == nl, "oops: %u lines", nl);
        CHECK(!strcmp(ci.message, "Kernel panic - not syncing: Oops: Fatal exception"), "oops: message '%s'", ci.message);
        CHECK(!strcmp(ci.version, "7.1.0-starbite-23527494"), "oops: kernel '%s'", ci.version);
        CHECK(!strcmp(ci.task, "CPU 2 PID 1 init"), "oops: task '%s'", ci.task);
        CHECK(ci.uptime_ms == 312405, "oops: uptime %llu ms", (unsigned long long)ci.uptime_ms);
        CHECK(ci.regs == 15, "oops: register lines %u (pstate, pc, lr, sp, 10 x-lines, Code)", ci.regs);
        CHECK(ci.trace == 14, "oops: trace lines %u (Call trace: + 13 frames)", ci.trace);
        CHECK(ci.msg == 13, "oops: message lines %u (Unable, Mem abort + 5, Data abort + 3, Internal error, panic)", ci.msg);
        CHECK(kind_at(t, (unsigned)n, "x2 : 0000000000000000") == CT_REGS && kind_at(t, (unsigned)n, "pc : lkdtm") == CT_REGS,
              "oops: x2 / pc lines are registers");
        CHECK(kind_at(t, (unsigned)n, " el0t_64_sync+0x190") == CT_TRACE && kind_at(t, (unsigned)n, "Call trace:") == CT_TRACE,
              "oops: frames are the trace");
        CHECK(kind_at(t, (unsigned)n, "---[ end trace") == CT_LOG && kind_at(t, (unsigned)n, "SMP: stopping") == CT_LOG,
              "oops: the trace ends at the next unindented line");
        CHECK(kind_at(t, (unsigned)n, "  FSC = 0x05") == CT_MSG && kind_at(t, (unsigned)n, "user pgtable") == CT_LOG,
              "oops: the abort info block belongs to the message");
        CHECK(kind_at(t, (unsigned)n, "Hardware name:") == CT_INFO && kind_at(t, (unsigned)n, "wlan0: associated") == CT_LOG,
              "oops: info and log lines");
        /* wrapped ring: rotate, only the order of the zone changes */
        rn = through_ring(t, (unsigned)n, ring_text, sizeof(ring_text), 5000);
        CHECK(rn == n && !memcmp(ring_text, t, n), "oops: a short record never wraps");

        /* the saved excerpt: everything that matters, then the end; at most CR_TEXT_MAX */
        unsigned en = cl_excerpt(ex, CR_TEXT_MAX, t, (unsigned)n, &lines);
        struct sb_crashinfo e2;
        CHECK(en <= CR_TEXT_MAX && en > 3000, "oops excerpt: %u bytes", en);
        sb_crash_parse(ex, en, &e2, 0, 0);
        CHECK(e2.regs == ci.regs && e2.trace == ci.trace && e2.msg == ci.msg && !strcmp(e2.message, ci.message) &&
              !strcmp(e2.version, ci.version) && !strcmp(e2.task, ci.task) && e2.uptime_ms == ci.uptime_ms,
              "oops excerpt: the same message, registers (%u), trace (%u), task, kernel, uptime", e2.regs, e2.trace);
        CHECK(sb_find(ex, en, "[...]\n") >= 0 && sb_find(ex, en, "<4>") < 0 && sb_find(ex, en, "<0>[") < 0,
              "oops excerpt: skipped lines marked, no <N> prefixes");
        {
            const char *last = "---[ end Kernel panic - not syncing: Oops: Fatal exception ]---\n";
            unsigned ll = (unsigned)strlen(last);
            CHECK(en > ll && !memcmp(ex + en - ll, last, ll), "oops excerpt: ends with the record's last line");
        }
        CHECK(ex[en - 1] == '\n' && lines == e2.lines, "oops excerpt: %u lines", lines);
        CHECK(!memcmp(ex, "[...]\n", 6) && sb_find(ex, en, "Panic#1 Part1") < 0,
              "oops excerpt: the start (record header, old lines) is cut");
        CHECK(sb_crash_stamp("[  312.402558] pc : x", 21) == 15 && sb_crash_stamp("[x] a", 5) == 0 &&
              sb_crash_stamp("[ 1.5]", 6) == 6, "time stamps");
        /* a small report: the message and registers come first, the end is cut */
        en = cl_excerpt(ex, 1200, t, (unsigned)n, &lines);
        CHECK(en <= 1200 && sb_find(ex, en, "Unable to handle kernel NULL pointer") >= 0, "small excerpt %u: the message", en);
        spit("excerpt-oops.txt", ex, en);
        free(t);
    }

    /* Linux: `echo c > /proc/sysrq-trigger` (the phone's test, 2026-11-06): no registers */
    snprintf(p, sizeof(p), "%s/linux-sysrq-panic.txt", fxdir);
    t = (char *)slurp(p, &n);
    CHECK(t != 0, "fixture %s", p);
    if (t) {
        sb_crash_parse(t, (unsigned)n, &ci, 0, 0);
        CHECK(!strcmp(ci.message, "Kernel panic - not syncing: sysrq triggered crash"), "sysrq: message '%s'", ci.message);
        CHECK(!strcmp(ci.task, "CPU 4 PID 1234 sh") && !strcmp(ci.version, "7.1.0-starbite-23527494"),
              "sysrq: task '%s' kernel '%s'", ci.task, ci.version);
        CHECK(ci.regs == 0 && ci.trace == 18 && ci.msg == 1, "sysrq: regs %u trace %u msg %u", ci.regs, ci.trace, ci.msg);
        CHECK(ci.uptime_ms == 1032118, "sysrq: uptime %llu", (unsigned long long)ci.uptime_ms);
        unsigned en = cl_excerpt(ex, CR_TEXT_MAX, t, (unsigned)n, &lines);
        CHECK(en < n && sb_find(ex, en, "[...]") < 0 && kinds_of(ex, en, CT_TRACE) == 18,
              "sysrq: short enough to be kept whole (%u of %zu bytes, prefixes off)", en, n);
        free(t);
    }

    /* XNU (0399): the panic dump in the pmsg ring, through the loader's own 0x3F00 ring */
    snprintf(p, sizeof(p), "%s/xnu-panic.txt", fxdir);
    t = (char *)slurp(p, &n);
    CHECK(t != 0, "fixture %s", p);
    if (t) {
        sb_crash_parse(t, (unsigned)n, &ci, 0, 0);
        CHECK(!strncmp(ci.message, "panic(cpu 2 caller 0xfffffff0081d3a5c): Kernel data abort. at pc 0xfffffff008b2c4e8", 83),
              "xnu: message '%s'", ci.message);
        CHECK(!strcmp(ci.version, "xnu-12377.1.9~1/DEVELOPMENT_ARM64_T8101"), "xnu: kernel '%s'", ci.version);
        CHECK(!strcmp(ci.task, "CPU 2 PID 0 kernel_task"), "xnu: task '%s'", ci.task);
        CHECK(ci.uptime_ms == 1234000, "xnu: uptime %llu (the crash marker)", (unsigned long long)ci.uptime_ms);
        CHECK(ci.regs == 12, "xnu: register lines %u (x0..x27 in 7, x28/fp/lr/sp, pc/cpsr/esr/far, 3 CORE)", ci.regs);
        CHECK(ci.trace == 15, "xnu: trace lines %u (Panicked thread + 14 lr/fp)", ci.trace);
        CHECK(kind_at(t, (unsigned)n, "CORE 2 is the one") == CT_LOG && kind_at(t, (unsigned)n, "--- console before") == CT_LOG,
              "xnu: plain lines stay log");
        CHECK(kind_at(t, (unsigned)n, "STARBITE: xnu crash v1") == CT_INFO && kind_at(t, (unsigned)n, "Kernel version:") == CT_INFO,
              "xnu: the marker and version are info");
        /* the dump as crash.c finds it: the needle, then to the end of the ring */
        static uint8_t z[0x4000];
        memset(z, 0, sizeof(z));
        memcpy(z, "DBGC", 4);
        unsigned pre = 300;
        for (unsigned i = 0; i < pre; ++i)
            z[12 + i] = (uint8_t)("STARBITE: loader line before the OS\n"[i % 36]);
        memcpy(z + 12 + pre, t, n);
        uint32_t st = pre + (uint32_t)n, sz = pre + (uint32_t)n;
        memcpy(z + 4, &st, 4);
        memcpy(z + 8, &sz, 4);
        unsigned rn = sb_ring_text(z, sizeof(z), ring_text, sizeof(ring_text));
        int at = sb_find(ring_text, rn, "STARBITE: XNU panic (sweetreboot)");
        CHECK(at == (int)pre, "xnu: the needle at %d", at);
        struct sb_crashinfo c2;
        sb_crash_parse(ring_text + at, rn - (unsigned)at, &c2, 0, 0);
        CHECK(!strcmp(c2.message, ci.message) && c2.regs == ci.regs && c2.trace == ci.trace, "xnu: from the ring");
        unsigned en = cl_excerpt(ex, CR_TEXT_MAX, ring_text + at, rn - (unsigned)at, &lines);
        sb_crash_parse(ex, en, &c2, 0, 0);
        CHECK(en <= CR_TEXT_MAX && c2.regs == ci.regs && c2.trace == ci.trace && !strcmp(c2.task, ci.task) &&
              c2.uptime_ms == ci.uptime_ms && !strcmp(c2.version, ci.version),
              "xnu excerpt (%u bytes): registers, trace, task, uptime, kernel kept", en);
        CHECK(sb_find(ex, en, "STARBITE HEALTH: watchdog feeder running") > 0, "xnu excerpt: the console tail's end");
        spit("excerpt-xnu.txt", ex, en);
        free(t);
    }

    /* prefixes, nothing, junk */
    CHECK(sb_crash_prefix("<4>[ 1.0] x", 11) == 3 && sb_crash_prefix("<12>x", 5) == 4 && sb_crash_prefix("<x>", 3) == 0 &&
          sb_crash_prefix("4>", 2) == 0, "syslog prefixes");
    CHECK(sb_crash_parse("", 0, &ci, 0, 0) == 0 && !ci.message[0] && !ci.uptime_ms, "empty text");
    const char *junk = "\x01\x02garbage\n\xff\xfe\nKernel panic - not syncing: x\n";
    sb_crash_parse(junk, (unsigned)strlen(junk), &ci, 0, 0);
    CHECK(!strcmp(ci.message, "Kernel panic - not syncing: x") && ci.lines == 3 && !ci.uptime_ms, "junk around a panic line");
    unsigned en = cl_excerpt(ex, 8, junk, (unsigned)strlen(junk), &lines);
    CHECK(en <= 8, "excerpt: a cap smaller than any line (%u)", en);
}

/* ---- the real panic of 2026-11-06 (health3 on the phone): capture, stored report, screen, keys ---- */

static struct cd_row hd_rows[6144];
static char hd_txt[65536];
static uint8_t hd_kinds[16384];

static void hd_build(struct cd_doc *d, const struct cl_report *r, unsigned level, const struct cd_shown *sh)
{
    memset(d, 0, sizeof(*d));
    d->rows = hd_rows;
    d->max = 6144;
    d->txt = hd_txt;
    d->txt_cap = sizeof(hd_txt);
    d->kinds = hd_kinds;
    d->kinds_max = sizeof(hd_kinds);
    cd_build(d, r, level, sh);
}

/* The first row at or after from whose text contains needle, or -1. */
static int hd_find(const struct cd_doc *d, unsigned from, const char *needle)
{
    for (unsigned i = from; i < d->n; ++i) {
        char b[CD_COLS + 1];
        unsigned n = d->rows[i].n;
        memcpy(b, (const char *)(uintptr_t)d->rows[i].p, n);
        b[n] = 0;
        if (strstr(b, needle))
            return (int)i;
    }
    return -1;
}

/* crash.c's loop with the phone's timing: hw_wait_ms(5) a turn, plus draw_ms when it redraws.
 * keys(t) for t in [t0, t1); returns the events seen, OR'ed; *moves counts the redraws. */
struct hd_press { uint64_t from, to; unsigned keys; };

static unsigned hd_drive(struct cs_nav *n, const struct hd_press *p, unsigned np, uint64_t t0, uint64_t t1,
                         unsigned draw_ms, unsigned *moves, unsigned *levels)
{
    unsigned all = 0;
    for (uint64_t t = t0; t < t1;) {
        unsigned k = 0;
        for (unsigned i = 0; i < np; ++i)
            if (t >= p[i].from && t < p[i].to)
                k |= p[i].keys;
        unsigned ev = cs_poll(n, k, t);
        all |= ev;
        if (ev & CS_EV_MOVED)
            ++*moves;
        if (ev & CS_EV_LEVEL)
            ++*levels;
        t += 5U + ((ev & (CS_EV_MOVED | CS_EV_LEVEL)) ? draw_ms : 0U);
    }
    return all;
}

static void test_real_panic2(const char *fxdir)
{
    char p[1100];
    size_t n, pn, rn;
    snprintf(p, sizeof(p), "%s/real-panic2-dmesg.txt", fxdir);
    char *t = (char *)slurp(p, &n);
    snprintf(p, sizeof(p), "%s/real-panic2-pmsg.txt", fxdir);
    char *pm = (char *)slurp(p, &pn);
    snprintf(p, sizeof(p), "%s/real-panic2-report2.bin", fxdir);
    uint8_t *rb = slurp(p, &rn);
    CHECK(t && pm && rb && rn == CL_BLOCK, "real panic2 fixtures");
    if (!t || !pm || !rb || rn != CL_BLOCK)
        return;
    static char ring_text[0x4000], ex[CR_TEXT_MAX];
    unsigned tn = through_ring(t, (unsigned)n, ring_text, sizeof(ring_text), 0);
    CHECK(tn == n && !memcmp(ring_text, t, n), "real: the dmesg record through zone 0 unchanged (%u bytes)", tn);

    struct sb_crashinfo ci;
    unsigned nl = sb_crash_parse(t, (unsigned)n, &ci, 0, 0);
    CHECK(nl == 204 && !strcmp(ci.message, "Kernel panic - not syncing: sysrq triggered crash"), "real: %u lines, '%s'",
          nl, ci.message);
    CHECK(!strcmp(ci.version, "7.1.0-starbite-f797be6b") && !strcmp(ci.task, "CPU 6 PID 4029 sh") &&
          ci.uptime_ms == 76972, "real: kernel '%s' task '%s' uptime %llu", ci.version, ci.task,
          (unsigned long long)ci.uptime_ms);
    CHECK(ci.trace == 18 && ci.regs == 0 && ci.msg == 1, "real: trace %u regs %u msg %u", ci.trace, ci.regs, ci.msg);

    /* what the loader stored on the phone is exactly what cl_excerpt makes of the capture, and it
     * ends with the panic, its trace and the last line */
    static struct cl_report r;
    CHECK(cl_report_decode(rb, &r) == 1 && r.seq == 2 && r.kind == CK_PANIC &&
          !strcmp(r.reason, "panic: sysrq triggered crash"), "real: report #2 decodes (%s)", r.reason);
    uint16_t lines;
    unsigned en = cl_excerpt(ex, CR_TEXT_MAX, t, (unsigned)n, &lines);
    CHECK(en == r.text_len && lines == r.text_lines && !memcmp(ex, r.text, en),
          "real: the stored text = cl_excerpt of the capture (%u / %u bytes, %u lines)", en, r.text_len, lines);
    CHECK(sb_find(r.text, r.text_len, "[   76.972237] Kernel panic - not syncing: sysrq triggered crash\n") > 0 &&
          sb_find(r.text, r.text_len, "[   76.972288] Call trace:\n") > 0 &&
          sb_find(r.text, r.text_len, "[   76.972506]  el0t_64_sync+0x198/0x19c\n") > 0,
          "real: the stored report has the panic and its call trace");
    {
        const char *last = "[   76.972553] Memory Limit: none\n";
        unsigned ll = (unsigned)strlen(last);
        CHECK(r.text_len > ll && !memcmp(r.text + r.text_len - ll, last, ll), "real: the stored report ends with the record");
    }
    struct sb_crashinfo c2;
    sb_crash_parse(r.text, r.text_len, &c2, 0, 0);
    CHECK(c2.trace == 18 && !strcmp(c2.message, ci.message), "real: the stored report parses to the same trace");

    /* a tighter report still keeps message, trace and the end */
    en = cl_excerpt(ex, 1000, t, (unsigned)n, &lines);
    CHECK(en <= 1000 && sb_find(ex, en, "Kernel panic - not syncing") >= 0 &&
          sb_find(ex, en, "Memory Limit: none\n") == (int)(en - 19), "real: 1000-byte excerpt: panic and the end kept");
    en = cl_excerpt(ex, 600, t, (unsigned)n, &lines);
    CHECK(en <= 600 && sb_find(ex, en, "Kernel panic - not syncing") >= 0 && sb_find(ex, en, "Memory Limit: none\n") > 0,
          "real: 600 bytes (the important lines alone overflow): the end still kept (%u)", en);

    /* the screen right after the crash (everything captured) and from the store, every level */
    static struct cd_doc d;
    struct cd_shown live = { t, (unsigned)n, 1, pm, (unsigned)pn, RS_CONSOLE, 1 };
    struct cd_shown saved = { r.text, r.text_len, 0, 0, 0, 0, 1 };
    for (unsigned v = 0; v < 2; ++v)
        for (unsigned level = 1; level <= 3; ++level) {
            const struct cd_shown *sh = v ? &saved : &live;
            hd_build(&d, &r, level, sh);
            int head = hd_find(&d, 0, "STARBITE CRASH REPORT #2");
            int msg = hd_find(&d, 0, "-- PANIC MESSAGE");
            int tr = hd_find(&d, 0, "-- CALL TRACE");
            int lg = hd_find(&d, 0, v ? "-- LOG (SAVED EXCERPT, 65 LINES)" : "-- LOG (ALL 204 LINES)");
            int pan = lg >= 0 ? hd_find(&d, (unsigned)lg, "Kernel panic - not syncing: sysrq triggered crash") : -1;
            int end = lg >= 0 ? hd_find(&d, (unsigned)lg, "Memory Limit: none") : -1;
            int other = hd_find(&d, 0, "-- OTHER RING");
            CHECK(head == 0 && msg > 0 && tr > msg && lg > tr && pan > lg && end > pan &&
                  (other >= 0 ? (unsigned)end + 2U == (unsigned)other : (unsigned)end + 3U == d.n),
                  "real %s, level %u: header, message, trace, the log with the panic, its last line (%d %d %d %d %d %d of %u)",
                  v ? "saved" : "live", level, head, msg, tr, lg, pan, end, d.n);
            CHECK(d.n > 78, "real %s, level %u: %u rows, more than a page: Volume has something to scroll", v ? "saved" : "live",
                  level, d.n);
            CHECK(hd_find(&d, (unsigned)tr, " el0t_64_sync+0x198/0x19c") > tr || level == 1,
                  "real %s, level %u: the whole trace", v ? "saved" : "live", level);
            CHECK((hd_find(&d, 0, "-- OTHER RING") >= 0) == (level == 3 && !v), "real: the pmsg ring only live, at 3");
            CHECK(hd_find(&d, 0, "KERNEL   7.1.0-starbite-f797be6b") > 0 && hd_find(&d, 0, "UPTIME 76.972 s") > 0 &&
                  hd_find(&d, 0, "TASK CPU 6 PID 4029 sh") > 0 && hd_find(&d, 0, "report #2 (slot 2)") > 0,
                  "real %s, level %u: kernel, uptime, task, store slot in the header", v ? "saved" : "live", level);
            for (unsigned i = 0; i < d.n; ++i)
                if (d.rows[i].n + d.rows[i].indent > CD_COLS) {
                    CHECK(0, "real: row %u wider than the panel", i);
                    break;
                }
        }

    /* the keys, as crash.c polls them, on the real document (level 1, as the phone opened it) */
    hd_build(&d, &r, 1, &live);
    struct cs_nav nv;
    cs_init(&nv, 78);
    cs_doc(&nv, &d);
    unsigned m = cs_max_top(&nv), moves = 0, levels = 0;
    CHECK(m == d.n - 78 && m > 0, "keys: max top %u", m);
    struct hd_press tap15[] = { { 1000, 1015, CS_K_DOWN } };
    unsigned ev = hd_drive(&nv, tap15, 1, 900, 1300, 25, &moves, &levels);
    CHECK(nv.top == 0 && !moves, "keys: Volume Down for 15 ms is not a press (top %u)", nv.top);
    struct hd_press tap60[] = { { 2000, 2060, CS_K_DOWN } };
    ev = hd_drive(&nv, tap60, 1, 1900, 2400, 25, &moves, &levels);
    CHECK(nv.top == 1 && moves == 1 && (ev & CS_EV_KEY), "keys: a 60 ms press of Volume Down: one line (top %u)", nv.top);
    struct hd_press hold[] = { { 3000, 4000, CS_K_DOWN } };
    moves = 0;
    hd_drive(&nv, hold, 1, 2900, 4100, 25, &moves, &levels);
    CHECK(nv.top > 10 && nv.top < 40, "keys: Volume Down held 1 s (25 ms redraws): repeats, top %u", nv.top);
    struct hd_press hold3[] = { { 5000, 8000, CS_K_DOWN } };
    hd_drive(&nv, hold3, 1, 4900, 8100, 25, &moves, &levels);
    CHECK(nv.top == m, "keys: held 3 s: fast, to the end (top %u of %u)", nv.top, m);
    struct hd_press up[] = { { 9000, 9500, CS_K_UP } };
    unsigned before = nv.top;
    hd_drive(&nv, up, 1, 8900, 9600, 25, &moves, &levels);
    CHECK(nv.top < before && before - nv.top >= 5, "keys: Volume Up held 0.5 s: back up (%u -> %u)", before, nv.top);
    /* slow redraws (120 ms each) still scroll */
    struct cs_nav slow;
    cs_init(&slow, 78);
    cs_doc(&slow, &d);
    moves = 0;
    struct hd_press hold2[] = { { 100, 2100, CS_K_DOWN } };
    hd_drive(&slow, hold2, 1, 0, 2200, 120, &moves, &levels);
    CHECK(slow.top > 10 && moves > 10, "keys: with 120 ms redraws, held 2 s: top %u, %u redraws", slow.top, moves);
    /* Bixby: a page (stopping at the next section), held: the level; Power: continue */
    cs_init(&nv, 78);
    cs_doc(&nv, &d);
    struct hd_press bix[] = { { 1000, 1100, CS_K_BIXBY } };
    hd_drive(&nv, bix, 1, 900, 1300, 25, &moves, &levels);
    CHECK(nv.top > 0 && nv.top <= 78, "keys: Bixby tap: a page or the next section (top %u)", nv.top);
    levels = 0;
    struct hd_press bixl[] = { { 2000, 2900, CS_K_BIXBY } };
    ev = hd_drive(&nv, bixl, 1, 1900, 3100, 25, &moves, &levels);
    CHECK(levels == 1 && (ev & CS_EV_LEVEL) && nv.top == 0, "keys: Bixby held 0.9 s: one level change, top 0");
    struct hd_press pw[] = { { 4000, 4050, CS_K_POWER } };
    ev = hd_drive(&nv, pw, 1, 3900, 4100, 25, &moves, &levels);
    CHECK(ev & CS_EV_EXIT, "keys: Power continues");
    struct hd_press both[] = { { 5000, 6000, CS_K_UP | CS_K_DOWN } };
    before = nv.top;
    hd_drive(&nv, both, 1, 4900, 6100, 25, &moves, &levels);
    CHECK(nv.top == before, "keys: both Volume keys: nothing");
    /* the time origin may be 0 (the host); the phone's is seconds after power on */
    struct cs_nav z0;
    cs_init(&z0, 78);
    cs_doc(&z0, &d);
    struct hd_press at0[] = { { 0, 80, CS_K_DOWN } };
    moves = 0;
    hd_drive(&z0, at0, 1, 0, 200, 25, &moves, &levels);
    CHECK(z0.top == 1, "keys: a press at time 0 counts");
    free(t);
    free(pm);
    free(rb);
}

/* ---- crash de-duplication: identical crashes twice are two reports (sb_evidence / sb_consume) ---- */

#define DZ 0x4000U
static uint8_t dz_live[2 * DZ];           /* the phone's 0xFED10000.. as RAM */
static uint8_t dz_snap[2 * DZ];

static void dz_put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

/* XNU 0399's writer: the pmsg ring (zone 1) reset, the dump from data offset 0 */
static void dz_xnu_dump(const char *t, unsigned n)
{
    uint8_t *z = dz_live + DZ;
    if (n > RING_PMSG)
        n = RING_PMSG;
    memcpy(z, "DBGC", 4);
    dz_put32(z + 4, n % RING_PMSG);
    dz_put32(z + 8, n);
    memcpy(z + 12, t, n);
}

/* Linux ramoops after a panic: zone 0 zapped, the record written from data offset 0 */
static void dz_linux_record(const char *t, unsigned n)
{
    uint8_t *z = dz_live;
    const unsigned data = DZ - 12;
    if (n > data)
        n = data;
    memset(z, 0, DZ);
    memcpy(z, "DBGC", 4);
    dz_put32(z + 4, n % data);
    dz_put32(z + 8, n);
    memcpy(z + 12, t, n);
}

/* the loader's pmsg_put (xnu_boot.c): appends to zone 1, wrapping at RING_PMSG */
static void dz_loader_line(const char *s)
{
    uint8_t *z = dz_live + DZ;
    uint32_t start, size;
    memcpy(&start, z + 4, 4);
    memcpy(&size, z + 8, 4);
    if (memcmp(z, "DBGC", 4) || start >= RING_PMSG || size > RING_PMSG) {
        memcpy(z, "DBGC", 4);
        start = size = 0;
    }
    for (; *s; ++s) {
        z[12 + start] = (uint8_t)*s;
        start = (start + 1) % RING_PMSG;
        if (size < RING_PMSG)
            ++size;
    }
    dz_put32(z + 4, start);
    dz_put32(z + 8, size);
}

/* One loader boot after a launch of `method` (0 none): snapshot, judge, the loader's own pmsg
 * lines (lines of them, between the snapshot and the judging, as POST writes), mark; the header
 * goes through its block codec as on UFS. Returns the EV_* bits found new. */
static uint32_t dz_boot(uint8_t *hblk, unsigned method, unsigned lines, unsigned *marked)
{
    struct cl_head h;
    if (cl_head_decode(hblk, &h) != 1)
        cl_head_init(&h);
    memcpy(dz_snap, dz_live, sizeof(dz_snap));
    static char t0[DZ], t1[DZ];
    unsigned n0 = sb_ring_text(dz_snap, DZ, t0, DZ), n1 = sb_ring_text(dz_snap + DZ, DZ, t1, DZ);
    for (unsigned i = 0; i < lines; ++i)
        dz_loader_line("STARBITE: POST full 8133ms fail 0 warn 0 - a loader line between snapshot and judging\n");
    int xp = sb_find(t1, n1, "STARBITE: XNU panic (sweetreboot)");
    int xh = sb_find(t1, n1, "STARBITE: sweetreboot timer (no panic)");
    struct sb_found f;
    sb_evidence(t0, n0, t1, n1, xp, xh, method == M_LINUX, method == M_XNU, &h, &f);
    unsigned mk = sb_consume(dz_live, dz_live + DZ, dz_snap, dz_snap + DZ, DZ, &f, &h);
    if (marked)
        *marked = mk;
    cl_head_encode(hblk, &h);
    return f.ev;
}

static void test_dedup(const char *fxdir)
{
    char p[1100];
    size_t xn, ln;
    snprintf(p, sizeof(p), "%s/xnu-panic.txt", fxdir);
    char *xt = (char *)slurp(p, &xn);
    snprintf(p, sizeof(p), "%s/real-panic2-dmesg.txt", fxdir);
    char *lt = (char *)slurp(p, &ln);
    CHECK(xt && lt, "dedup fixtures");
    if (!xt || !lt)
        return;
    static uint8_t hb[CL_BLOCK];
    struct cl_head h0;
    cl_head_init(&h0);
    h0.hflags = CLH_CONSUMES;
    cl_head_encode(hb, &h0);
    unsigned mk;

    /* XNU: xnuh-panic twice, the same deterministic dump each time, then a clean run */
    memset(dz_live, 0, sizeof(dz_live));
    dz_xnu_dump(xt, (unsigned)xn);
    CHECK(dz_boot(hb, M_XNU, 3, &mk) == EV_XNU_PANIC && mk == 2, "xnu run 1: a report, the dump marked (%u)", mk);
    CHECK(sb_find((const char *)dz_live + DZ + 12, RING_PMSG, "STARSEEN: XNU panic (sweetreboot)") >= 0,
          "xnu: the judged dump reads STARSEEN in RAM");
    dz_xnu_dump(xt, (unsigned)xn);                       /* run 2: byte for byte the same dump */
    CHECK(dz_boot(hb, M_XNU, 3, &mk) == EV_XNU_PANIC, "xnu run 2, identical dump: a second report");
    CHECK(dz_boot(hb, M_XNU, 3, &mk) == 0, "xnu run 3, clean restart: no report");
    for (unsigned i = 0; i < 5; ++i)
        CHECK(dz_boot(hb, i & 1 ? M_XNU : 0, 2, &mk) == 0, "xnu: unrelated reset %u: the old dump is not reported", i);
    /* the loader's own lines wrap over the dump's header before it is judged: still one report */
    dz_xnu_dump(xt, (unsigned)xn);
    uint32_t evw = dz_boot(hb, M_XNU, 150, &mk);     /* 5684 + 150 x 86 bytes: wraps past 0x3F00 */
    CHECK(evw == EV_XNU_PANIC && mk == 0, "xnu: header overwritten by loader lines: reported, nothing to mark (ev %u mk %u)", evw, mk);
    CHECK(dz_boot(hb, M_XNU, 3, &mk) == 0, "xnu: and not again");
    /* a timer dump (sweetreboot, no panic) */
    {
        static char tm[1024];
        int n = snprintf(tm, sizeof(tm), "STARBITE: sweetreboot timer (no panic), console tail:\nwaiting for root device\n");
        dz_xnu_dump(tm, (unsigned)n);
        CHECK(dz_boot(hb, M_XNU, 1, &mk) == EV_XNU_HANG && mk == 2, "xnu timer dump: a hang report, marked");
        dz_xnu_dump(tm, (unsigned)n);
        CHECK(dz_boot(hb, M_XNU, 1, &mk) == EV_XNU_HANG, "xnu timer dump again: again");
        CHECK(dz_boot(hb, M_XNU, 1, &mk) == 0, "xnu timer: then nothing");
    }

    /* Linux: the real sysrq panic record twice, then clean runs */
    memset(dz_live, 0, sizeof(dz_live));
    cl_head_encode(hb, &h0);
    dz_linux_record(lt, (unsigned)ln);
    CHECK(dz_boot(hb, M_LINUX, 3, &mk) == EV_LX_PANIC && mk == 1, "linux run 1: a report, the record marked");
    {
        static char t0[DZ];
        unsigned n0 = sb_ring_text(dz_live, DZ, t0, DZ);
        CHECK(n0 == ln && !memcmp(t0, "====1793987680.553805-S\nPanic#1 Part1\n", 38) && !memcmp(t0 + 38, lt + 38, ln - 38),
              "linux: the judged record differs only in its flag (-D -> -S)");
        CHECK(sb_dmesg_time(t0, n0) == 1793987680ULL, "linux: its time still reads");
    }
    dz_linux_record(lt, (unsigned)ln);                   /* run 2: the identical panic */
    CHECK(dz_boot(hb, M_LINUX, 3, &mk) == EV_LX_PANIC, "linux run 2, identical record: a second report");
    CHECK(dz_boot(hb, M_LINUX, 3, &mk) == 0, "linux run 3, clean restart: no report");
    CHECK(dz_boot(hb, M_XNU, 3, &mk) == 0 && dz_boot(hb, M_LINUX, 3, &mk) == 0, "linux: unrelated resets: none");
    /* an oops record (no panic): a warning, once */
    {
        static char oo[2048];
        int n = snprintf(oo, sizeof(oo), "====1793990000.000001-D\nOops#1 Part1\n<0>[ 5.000000] Internal error: Oops: 96000045 [#1] SMP\n");
        dz_linux_record(oo, (unsigned)n);
        CHECK(dz_boot(hb, M_LINUX, 0, &mk) == EV_LX_OOPS && mk == 1, "linux oops: once");
        CHECK(dz_boot(hb, M_LINUX, 0, &mk) == 0, "linux oops: not twice");
    }
    /* a record found while another OS was launched: not that OS's, but marked so it never
     * shows up later as Linux's */
    dz_linux_record(lt, (unsigned)ln);
    CHECK(dz_boot(hb, M_XNU, 0, &mk) == 0 && mk == 1, "linux record after an XNU launch: not reported, marked");
    CHECK(dz_boot(hb, M_LINUX, 0, &mk) == 0, "... and not reported at the next Linux launch");

    /* the first boot of a consuming loader: old evidence is still compared with the seen CRCs */
    {
        memset(dz_live, 0, sizeof(dz_live));
        dz_xnu_dump(xt, (unsigned)xn);
        static char t1[DZ];
        unsigned n1 = sb_ring_text(dz_live + DZ, DZ, t1, DZ);
        int xp = sb_find(t1, n1, "STARBITE: XNU panic (sweetreboot)");
        struct cl_head old;
        cl_head_init(&old);
        old.seen_pmsg = sb_crc32(t1 + xp, n1 - (unsigned)xp < 512U ? n1 - (unsigned)xp : 512U);
        cl_head_encode(hb, &old);
        CHECK(dz_boot(hb, M_XNU, 1, &mk) == 0 && mk == 2, "old loader's judged dump: not reported again, marked now");
        struct cl_head now;
        CHECK(cl_head_decode(hb, &now) == 1 && (now.hflags & CLH_CONSUMES), "the header says consumes now");
        dz_xnu_dump(xt, (unsigned)xn);
        CHECK(dz_boot(hb, M_XNU, 1, &mk) == EV_XNU_PANIC, "then the identical next crash is reported");
    }
    /* ring positions: wrapped Linux console ring and the 0x3F00 pmsg ring map back */
    {
        static uint8_t z[DZ];
        memset(z, 0, sizeof(z));
        memcpy(z, "DBGC", 4);
        dz_put32(z + 4, 100);
        dz_put32(z + 8, RING_PMSG);
        CHECK(sb_ring_phys(z, DZ, 0) == 100 && sb_ring_phys(z, DZ, RING_PMSG - 100) == 0 &&
              sb_ring_phys(z, DZ, RING_PMSG) == -1, "pmsg ring positions");
        dz_put32(z + 8, 50);
        dz_put32(z + 4, 50);
        CHECK(sb_ring_phys(z, DZ, 49) == 49 && sb_ring_phys(z, DZ, 50) == -1, "linear ring positions");
    }
    free(xt);
    free(lt);
}

/* ---- keys from USB (`fastboot oem key`): the queue, and the menus' own rules on it ------------- */

static void test_kq(const char *fxdir)
{
    struct kq k;
    kq_init(&k);
    CHECK(kq_bits(&k, 5000) == 0, "kq: idle");
    CHECK(kq_push(&k, 2, 0) && k.ms[0] == KQ_PRESS_MS, "kq: a plain press");
    unsigned on = 0, first = ~0U, last = 0;
    for (unsigned t = 1000; t < 2000; ++t)
        if (kq_bits(&k, t) == 2) {
            ++on;
            first = t < first ? t : first;
            last = t;
        }
    CHECK(on == KQ_PRESS_MS && first == 1000 && last == 1000 + KQ_PRESS_MS - 1 && k.n == 0, "kq: down %u ms from the first read", on);
    /* three presses: each its time, a gap between */
    kq_init(&k);
    CHECK(kq_push(&k, 1, 0) && kq_push(&k, 4, 900) && kq_push(&k, 8, 0), "kq: queued");
    unsigned seq[4] = { 0 }, ns = 0, prev = 0, dur[4] = { 0 };
    for (unsigned t = 0; t < 3000; ++t) {
        unsigned b = kq_bits(&k, t);
        if (b && b != prev && ns < 4)
            seq[ns++] = b;
        if (b && ns)
            ++dur[ns - 1];
        prev = b;
    }
    CHECK(ns == 3 && seq[0] == 1 && seq[1] == 4 && seq[2] == 8 && dur[0] == 150 && dur[1] == 900 && dur[2] == 150,
          "kq: up, bixby held 900, power (%u: %u %u %u)", ns, dur[0], dur[1], dur[2]);
    kq_init(&k);
    for (unsigned i = 0; i < KQ_N; ++i)
        kq_push(&k, 1, 0);
    CHECK(!kq_push(&k, 1, 0) && !kq_push(&k, 0, 0), "kq: full, and no key, refused");
    kq_init(&k);
    kq_push(&k, 1, 99999);
    CHECK(k.ms[0] == KQ_MAX_MS, "kq: capped");
    CHECK(kq_key_bit("up") == 1 && kq_key_bit("down 900") == 2 && kq_key_bit("bixby") == 4 && kq_key_bit("power") == 8 &&
          kq_key_bit("vol+") == 1 && kq_key_bit("voldown") == 2 && !kq_key_bit("upx") && !kq_key_bit("") &&
          !kq_key_bit("home"), "kq: key names");
    /* a plain press against every menu's rules: menu v7 (menu7.h: debounce 20, Power hold 520,
     * Volume repeat 450, Bixby hold 700), recovery (30 ms stable), the crash report (crashdoc.h) */
    CHECK(KQ_PRESS_MS > 20U && KQ_PRESS_MS > 30U && KQ_PRESS_MS > CS_FIRST_MS && KQ_PRESS_MS < 450U && KQ_PRESS_MS < 520U &&
          KQ_PRESS_MS < CS_REPEAT_MS && KQ_PRESS_MS < CS_LEVEL_MS, "kq: a plain press is a press everywhere, never a hold");

    /* the crash report driven by injected presses, with crash.c's loop timing */
    char p[1100];
    size_t n, rn;
    snprintf(p, sizeof(p), "%s/real-panic2-dmesg.txt", fxdir);
    char *t = (char *)slurp(p, &n);
    snprintf(p, sizeof(p), "%s/real-panic2-report2.bin", fxdir);
    uint8_t *rb = slurp(p, &rn);
    if (!t || !rb)
        return;
    static struct cl_report r;
    cl_report_decode(rb, &r);
    static struct cd_doc d;
    struct cd_shown live = { t, (unsigned)n, 1, 0, 0, 0, 1 };
    hd_build(&d, &r, 1, &live);
    struct cs_nav nv;
    cs_init(&nv, 78);
    cs_doc(&nv, &d);
    kq_init(&k);
    kq_push(&k, CS_K_DOWN, 0);
    kq_push(&k, CS_K_DOWN, 0);
    kq_push(&k, CS_K_DOWN, 0);
    kq_push(&k, CS_K_BIXBY, 900);
    kq_push(&k, CS_K_POWER, 0);
    unsigned levels = 0, exits = 0, top_before_level = 0;
    for (uint64_t tm = 0; tm < 4000;) {
        unsigned ev = cs_poll(&nv, kq_bits(&k, tm), tm);
        if (ev & CS_EV_LEVEL) {
            ++levels;
            top_before_level = 3;
        }
        if (ev & CS_EV_EXIT) {
            ++exits;
            break;
        }
        tm += 5U + ((ev & (CS_EV_MOVED | CS_EV_LEVEL)) ? 25U : 0U);
        if (!levels)
            top_before_level = nv.top;
    }
    CHECK(top_before_level == 3 && levels == 1 && exits == 1,
          "injected: 3 x Volume Down = 3 lines, Bixby 900 ms = one level, Power = continue (top %u, levels %u)",
          top_before_level, levels);
    /* and POST's key machine: an injected Volume Up is a press made during POST */
    struct pv_keys pk;
    pv_init(&pk, PV_LOGO, 0, 0);
    kq_init(&k);
    kq_push(&k, 1, 0);
    unsigned pev = 0;
    for (uint64_t tm = 100; tm < 600; tm += 5)
        pev |= pv_poll(&pk, (kq_bits(&k, tm) & 1U) != 0, 0, tm);
    CHECK(pev == PV_EV_LOG, "injected Volume Up opens POST's log");
    free(t);
    free(rb);
}

/* ---- the menus' background USB link across one boot (sbfmt.c bgl_*, fastboot_dev.c fbbg_*) ----- */

struct bgsim { struct bgl b; unsigned starts, relinks, serves, idles; int fail_next; uint64_t cfg_at; int host; int vbus; };

/* Polls every `every` ms over [t0, t1): what fastboot_dev.c's fbbg_poll does with the policy. The
 * host configures the link `host` ms after each (re)start, if a host is there. */
static void bg_run(struct bgsim *s, uint64_t t0, uint64_t t1, unsigned every)
{
    for (uint64_t t = t0; t < t1; t += every) {
        int configured = s->b.up && s->host && s->cfg_at && t >= s->cfg_at;
        unsigned w = bgl_poll(&s->b, t, configured, s->vbus);
        if (w == BGL_IDLE) {
            ++s->idles;
            continue;
        }
        if (w != BGL_SERVE) {
            int ok = !s->fail_next;
            if (s->fail_next > 0)
                --s->fail_next;
            *(w == BGL_START ? &s->starts : &s->relinks) += 1;
            bgl_result(&s->b, w, ok, t);
            s->cfg_at = ok ? t + 300U : 0;
            if (!ok)
                continue;
        }
        ++s->serves;
        bgl_served(&s->b, t);
    }
}

static void test_bgl(void)
{
    struct bgsim s;
    memset(&s, 0, sizeof(s));
    bgl_init(&s.b);
    s.host = 1;
    s.vbus = 1;
    bg_run(&s, 0, 1000, 10);
    CHECK(s.idles == 100 && !s.starts, "bgl: not wanted (before fbbg_enable): nothing");
    bgl_enable(&s.b, 1);
    /* the crash report (a poll every 5 ms + redraws: say 35 ms) for 6 s, the Mac working with it */
    bg_run(&s, 1000, 7000, 35);
    CHECK(s.starts == 1 && s.relinks == 0 && s.b.up, "bgl: crash report: one start, served (%u starts)", s.starts);
    /* Power: menu v7's intro and wallpaper, nothing polls for 3.2 s (health5's dead link) */
    bg_run(&s, 10200, 70200, 16);
    CHECK(s.starts == 1 && s.relinks == 1, "bgl: the menu's first poll after the 3.2 s gap restarts the link (%u)", s.relinks);
    /* a minute of menu at 60 fps with an occasional slow frame: never restarted again */
    unsigned r0 = s.relinks;
    for (uint64_t t = 70200; t < 130200; t += 600) {
        bg_run(&s, t, t + 400, 16);
        bg_run(&s, t + 600, t + 601, 1);    /* a 200 ms frame: the poll after it */
    }
    CHECK(s.relinks == r0, "bgl: slow frames (200 ms) are not gaps");
    /* the menu starts an OS that fails: boot_entry stopped the link; the menu comes back */
    bgl_stopped(&s.b);
    bg_run(&s, 140000, 141000, 16);
    CHECK(s.starts == 2 && s.b.up, "bgl: after a start stopped it, the next menu poll starts it again");
    /* to Starbite Recovery (recovery polls every 10 ms), a full POST there (9 s without polls) */
    bg_run(&s, 141500, 150000, 10);
    unsigned r1 = s.relinks;
    bg_run(&s, 159000, 165000, 10);
    CHECK(s.relinks == r1 + 1, "bgl: recovery after a 9 s POST run: restarted once");
    /* fbbg_off (an OS starts): nothing more */
    bgl_enable(&s.b, 0);
    bgl_stopped(&s.b);
    unsigned st = s.starts, rl = s.relinks;
    bg_run(&s, 166000, 200000, 10);
    CHECK(s.starts == st && s.relinks == rl, "bgl: off: never started again");

    /* a start that fails is retried with back-off: 5 s, 10 s, 20 s ... at most 60 s */
    memset(&s, 0, sizeof(s));
    bgl_init(&s.b);
    s.host = 1;
    s.vbus = 1;
    bgl_enable(&s.b, 1);
    s.fail_next = 3;
    bg_run(&s, 0, 4900, 10);
    CHECK(s.starts == 1 && !s.b.up, "bgl: a failed start: no retry for 5 s (%u)", s.starts);
    bg_run(&s, 4900, 5100, 10);
    CHECK(s.starts == 2, "bgl: retried after 5 s");
    bg_run(&s, 5100, 15000, 10);
    CHECK(s.starts == 2, "bgl: then not for 10 s");
    bg_run(&s, 15000, 15200, 10);
    CHECK(s.starts == 3, "bgl: retried after 10 s");
    bg_run(&s, 15200, 35100, 10);
    CHECK(s.starts == 4 && s.b.up && s.b.fails == 3, "bgl: after 20 s, and it comes up (%u starts, %u fails)", s.starts, s.b.fails);
    CHECK(s.idles > 3000, "bgl: polls while it waits cost nothing (%u idle)", s.idles);
    s.fail_next = 100;
    bgl_stopped(&s.b);
    unsigned s0 = s.starts;
    bg_run(&s, 40000, 400000, 10);
    /* at 40, 45, 55, 75, 115, 175, 235, 295, 355 s */
    CHECK(s.starts - s0 == 9 && s.b.retry_ms == BGL_RETRY_MAX_MS,
          "bgl: failing for good: back-off reaches 60 s (%u starts in 6 min)", s.starts - s0);

    /* plugged in, but no host configures it (a charger, a prompt not answered): restarted after
     * 10 s, 20, 40, 60 s; with a host it stays; without VBUS it is left alone */
    memset(&s, 0, sizeof(s));
    bgl_init(&s.b);
    s.vbus = 1;
    bgl_enable(&s.b, 1);
    bg_run(&s, 0, 10500, 10);
    CHECK(s.starts == 1 && s.relinks == 1, "bgl: not configured for 10 s: restarted (%u)", s.relinks);
    bg_run(&s, 10500, 31000, 10);
    CHECK(s.relinks == 2, "bgl: then after 20 s (%u)", s.relinks);
    s.host = 1;
    s.cfg_at = 31000;
    bg_run(&s, 31000, 200000, 10);
    CHECK(s.relinks == 2 && s.b.unconf_ms == BGL_UNCONF_MS, "bgl: configured: kept, back-off reset");
    memset(&s, 0, sizeof(s));
    bgl_init(&s.b);
    bgl_enable(&s.b, 1);
    bg_run(&s, 0, 300000, 10);
    CHECK(s.starts == 1 && s.relinks == 0, "bgl: no VBUS: never restarted");
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
    test_wdt_args();
    test_post_view();
    {
        /* the crash text fixtures sit beside S-Boot's FDT fixture */
        char fx[1024];
        snprintf(fx, sizeof(fx), "%s", argv[2]);
        char *slash = strrchr(fx, '/');
        if (slash)
            *slash = 0;
        else
            strcpy(fx, ".");
        test_crash_parse(fx);
        test_real_panic2(fx);
        test_dedup(fx);
        test_kq(fx);
        test_bgl();
    }
    char cmd[2048];
    for (unsigned v = 0; v < 2; ++v) {
        snprintf(cmd, sizeof(cmd), "'%s' --catalog-variant '%s' %s", argv[0], outdir, v ? "damaged" : "old");
        int r = system(cmd);
        CHECK(r == 0, "catalog %s: extension ignored, entries kept (exit %d)", v ? "with a damaged extension" : "without the extension", r);
    }
    printf("%u checks, %u failed\n", nchk, nfail);
    return nfail != 0;
}
