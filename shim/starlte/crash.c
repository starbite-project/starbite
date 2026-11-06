/* SPDX-License-Identifier: GPL-2.0-only
 * Crash and hang detection below every OS: see crash.h and docs/STARBITE-HEALTH.md.
 *
 * RAM used (non-cacheable, mmu.c; free until an OS is loaded, outside POST's RAM windows):
 *   CR_SNAP  0x9F000000  32 KiB  the ramoops area 0xFED10000..0xFED18000 as the loader found it
 *   CR_SECPG 0x9F008000   4 KiB  Samsung's sec_debug page 0x80000000 (panic magic + string)
 *   CR_LOG   0x9F010000  64 KiB  the store's crash log (UFS DMA target)
 *   CR_T0/1  0x9F020000  2 x 16 KiB  the two ramoops zones as text
 *   CR_PANEL 0x9A000000  16.3 MiB  the panel under the crash screen (post.c's copy, free by then)
 *   LAUNCH_BUF 0xAF400000  4 KiB  the launch record, written after the OS is in RAM: above UFS
 *                                 DMA's buffers, below RAM_END, above every OS load area
 * Writes: the crash log entry's blocks (store.c checks the range), the panel, the pmsg ring and
 * the log page (slots 220..223). Nothing in the ramoops zones is touched here; the boot-next
 * mailbox (0xFED17FE0) is only read, by menu.c, as before.
 * No pointer tables (no absolute relocations).
 */
#include "crash.h"
#include "sbfmt.h"
#include "store.h"
#include "ufs.h"
#include "fbtext.h"
#include "hw.h"
#include "catalog.h"
#include "recovery.h"
#include "wdt.h"
#include "../fdt.h"

#define CR_SNAP    0x9F000000UL
#define CR_SECPG   0x9F008000UL
#define CR_LOG     0x9F010000UL
#define CR_T0      0x9F020000UL
#define CR_T1      0x9F024000UL
#define CR_PANEL   0x9A000000UL
#define LAUNCH_BUF (UFS_DMA + 0x400000UL)
#define RAMOOPS    0xFED10000UL
#define ZONE       0x4000U
#define SECDBG     0x80000000UL
#define CR_LOGPAGE ((volatile uint64_t *)0x91fff000UL)
#define CR_SLOT    220U
#define SCREEN_CAP_S 600U          /* the crash screen never stays longer than this */

#define C_WHITE 0xffffffffU
#define C_BLACK 0xff000000U
#define C_BLUE  0xff2a5bf0U
#define C_MUTED 0xffb1bad3U
#define C_GOLD  0xffffd43bU
#define C_RED   0xffff4d4dU
#define C_GREEN 0xff3ddc84U

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void copy64(uintptr_t dst, uintptr_t src, unsigned n)
{
    for (unsigned i = 0; i < n; i += 8)
        *(volatile uint64_t *)(dst + i) = *(volatile const uint64_t *)(src + i);
    __asm__ volatile("dsb sy" : : : "memory");
}

/* ---- text builder (one screen row, a pmsg line, fastboot text) -------------------------------- */

struct tb { char *b; unsigned n, cap; };

static void tb_c(struct tb *t, char c)
{
    if (t->n + 1 < t->cap)
        t->b[t->n++] = c;
    if (t->cap)
        t->b[t->n] = 0;
}

static void tb_s(struct tb *t, const char *s)
{
    while (s && *s)
        tb_c(t, *s++);
}

static void tb_u(struct tb *t, uint64_t v)
{
    char d[20];
    unsigned k = 0;
    do {
        d[k++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && k < sizeof(d));
    while (k)
        tb_c(t, d[--k]);
}

static void tb_x(struct tb *t, uint64_t v, unsigned digits)
{
    tb_s(t, "0x");
    while (digits--)
        tb_c(t, "0123456789abcdef"[(v >> (4 * digits)) & 15U]);
}

static void tb_i(struct tb *t, int64_t v)
{
    if (v < 0) {
        tb_c(t, '-');
        v = -v;
    }
    tb_u(t, (uint64_t)v);
}

struct row { char b[STAR_FB_COLS + 1]; struct tb t; };

static struct tb *row_init(struct row *r)
{
    r->t.b = r->b;
    r->t.n = 0;
    r->t.cap = sizeof(r->b);
    r->b[0] = 0;
    return &r->t;
}

static const char *method_name(unsigned m)
{
    switch (m) {
    case M_XNU:     return "xnu";
    case M_LINUX:   return "linux";
    case M_ANDROID: return "android";
    case M_UEFI:    return "uefi";
    case M_BARE:    return "bare";
    default:        return "?";
    }
}

/* ---- the snapshot ----------------------------------------------------------------------------- */

static struct {
    int taken;
    uint32_t rst_stat, inform2, inform3, wdt_dis, wdt_mask, w1[3], w0[3];
    int32_t sb_rr, sb_rst, sb_pwron, sb_pwroff;
} sn;
static char sb_args[4096];

static int32_t arg_num(const char *key)
{
    char v[24];
    int64_t x;
    if (!sb_arg(sb_args, sizeof(sb_args), key, v, sizeof(v)) || !sb_num(v, &x))
        return -1;
    return (int32_t)x;
}

void crash_snapshot(uintptr_t fdt)
{
    copy64(CR_SNAP, RAMOOPS, 2U * ZONE);
    copy64(CR_SECPG, SECDBG, 4096U);
    sn.rst_stat = rd(PMU_BASE + 0x404);
    sn.inform2 = rd(PMU_BASE + 0x808);
    sn.inform3 = rd(PMU_BASE + 0x80c);
    wdt_regs(sn.w1, sn.w0, &sn.wdt_dis, &sn.wdt_mask);
    uintptr_t v;
    uint32_t len;
    sb_args[0] = 0;
    if (fdt && !(fdt & 7) && fdt_chosen_raw(fdt, "bootargs", &v, &len)) {
        unsigned k = 0;
        for (; k + 1 < sizeof(sb_args) && k < len && *(volatile const char *)(v + k); ++k)
            sb_args[k] = *(volatile const char *)(v + k);
        sb_args[k] = 0;
    }
    sn.sb_rr = arg_num("sec_debug.reset_reason");
    sn.sb_rst = arg_num("sec_debug.rst_stat");
    sn.sb_pwron = arg_num("sec_debug.pwronsrc");
    sn.sb_pwroff = arg_num("sec_debug.pwroffsrc");
    sn.taken = 1;
}

/* ---- settings --------------------------------------------------------------------------------- */

static struct star_settings cs;
static int cs_loaded;

static void cs_load(void)
{
    if (cs_loaded)
        return;
    if (!settings_load(&cs))
        sbset_defaults(&cs);
    cs_loaded = 1;
}

void crash_reload_settings(void)
{
    cs_loaded = 0;
}

/* ---- evaluation ------------------------------------------------------------------------------- */

static struct cl_report rep;
static int hold_autoboot;

int crash_hold_autoboot(void)
{
    return hold_autoboot;
}

static unsigned min_u(unsigned a, unsigned b)
{
    return a < b ? a : b;
}

static unsigned cstrlen(const char *s, unsigned max)
{
    unsigned n = 0;
    while (n < max && s[n])
        ++n;
    return n;
}

static void scopy(char *d, unsigned cap, const char *s)
{
    unsigned k = 0;
    for (; s && s[k] && k + 1 < cap; ++k)
        d[k] = s[k];
    d[k] = 0;
}

static void reason_from(const char *src, unsigned n, const char *needle, const char *prefix)
{
    char line[96];
    if (!sb_line_after(src, n, needle, line, sizeof(line)))
        return;
    struct tb t = { rep.reason, 0, sizeof(rep.reason) };
    tb_s(&t, prefix);
    tb_s(&t, line);
}

/* The decision and the report for a launch (h: the crash log header as read). *sev out. */
static unsigned evaluate(struct cl_head *h, unsigned *sev)
{
    char *t0 = (char *)CR_T0, *t1 = (char *)CR_T1;
    unsigned n0 = sb_ring_text((const uint8_t *)CR_SNAP, ZONE, t0, ZONE);
    unsigned n1 = sb_ring_text((const uint8_t *)(CR_SNAP + ZONE), ZONE, t1, ZONE);
    int launched = h->launch_state == CL_RUNNING;
    unsigned m = launched ? h->method : 0;
    uint32_t ev = 0;

    /* Linux: a ramoops dmesg record "====<time>-D" in zone 0 that was not there before */
    int dmesg = n0 >= 4 && t0[0] == '=' && t0[1] == '=' && t0[2] == '=' && t0[3] == '=';
    uint32_t c0 = dmesg ? sb_crc32(t0, n0) : 0;
    if (m == M_LINUX && dmesg && c0 != h->seen_dmesg)
        ev |= sb_find(t0, n0, "Panic#") >= 0 ? EV_LX_PANIC : sb_find(t0, n0, "Oops#") >= 0 ? EV_LX_OOPS : EV_LX_OTHER;
    /* XNU: starlte_pmsg.c's dump at the start of the pmsg ring */
    int xp = sb_find(t1, n1, "STARBITE: XNU panic (sweetreboot)");
    int xh = sb_find(t1, n1, "STARBITE: sweetreboot timer (no panic)");
    int xat = xp >= 0 ? xp : xh;
    uint32_t cx = xat >= 0 ? sb_crc32(t1 + xat, min_u(512U, n1 - (unsigned)xat)) : 0;
    if (m == M_XNU && xat >= 0 && cx != h->seen_pmsg)
        ev |= xp >= 0 ? EV_XNU_PANIC : EV_XNU_HANG;
    /* Samsung's Android kernel: sec_debug magic + panic string at 0x80000000 */
    const char *ps = (const char *)(CR_SECPG + 4);
    uint32_t magic = *(volatile const uint32_t *)CR_SECPG;
    unsigned pn = magic == SEC_MAGIC_PANIC ? cstrlen(ps, 1020) : 0;
    uint32_t cps = pn ? sb_crc32(ps, pn) : 0;
    if (m == M_ANDROID && ((pn && cps != h->seen_secmagic) || sn.inform3 == SEC_CAUSE_PANIC))
        ev |= EV_AND_PANIC;

    struct sb_evidence e;
    e.rst_stat = sn.rst_stat;
    e.inform3 = sn.inform3;
    e.sboot_rr = sn.sb_rr;
    e.sboot_pwron = sn.sb_pwron;
    e.ev = ev;
    e.launched = (uint8_t)launched;
    e.method = (uint8_t)m;
    e.wdt_armed = h->wdt_armed;
    unsigned kind = sb_classify(&e, sev);

    /* the report */
    uint8_t *z = (uint8_t *)&rep;
    for (unsigned i = 0; i < sizeof(rep); ++i)
        z[i] = 0;
    rep.seq = h->count + 1U;
    rep.kind = (uint8_t)kind;
    rep.severity = (uint8_t)*sev;
    rep.method = (uint8_t)m;
    uint64_t now = 0;
    (void)em_rtc_epoch(&now);
    rep.epoch_detect = now;
    rep.epoch_launch = launched ? h->launch_epoch : 0;
    scopy(rep.key, sizeof(rep.key), launched ? h->key : "-");
    scopy(rep.name, sizeof(rep.name), launched ? h->name : "NO OS (THE LOADER)");
    rep.reg[CR_RST_STAT] = sn.rst_stat;
    rep.reg[CR_INFORM2] = sn.inform2;
    rep.reg[CR_INFORM3] = sn.inform3;
    rep.reg[CR_WDT_DIS] = sn.wdt_dis;
    rep.reg[CR_WDT_MASK] = sn.wdt_mask;
    rep.reg[CR_SB_RR] = (uint32_t)sn.sb_rr;
    rep.reg[CR_SB_RST] = (uint32_t)sn.sb_rst;
    rep.reg[CR_SB_PWRON] = (uint32_t)sn.sb_pwron;
    rep.reg[CR_SB_PWROFF] = (uint32_t)sn.sb_pwroff;
    rep.reg[CR_SEC_MAGIC] = magic;
    rep.reg[CR_WDT1_CON] = sn.w1[0];
    rep.reg[CR_WDT1_DAT] = sn.w1[1];
    rep.reg[CR_WDT1_CNT] = sn.w1[2];
    rep.reg[CR_WDT0_CON] = sn.w0[0];
    rep.reg[CR_LAUNCH_WDT] = launched && h->wdt_armed ? h->wdt_s : 0;
    rep.reg[CR_FLAGS] = (launched ? h->lflags & 0xffffU : 0) | ev << 16;

    /* where the last lines come from */
    const char *src = t1;
    unsigned sn_ = n1;
    rep.source = RS_PMSG;
    if (m == M_LINUX) {
        if (ev & (EV_LX_PANIC | EV_LX_OOPS | EV_LX_OTHER)) {
            src = t0;
            sn_ = n0;
            rep.source = RS_DMESG;
            rep.epoch_event = sb_dmesg_time(t0, n0);
        } else {
            rep.source = RS_CONSOLE;
        }
    } else if (m == M_ANDROID) {
        if (n0 && !dmesg) {
            src = t0;
            sn_ = n0;
            rep.source = RS_CONSOLE;
        }
        if (ev & EV_AND_PANIC && pn)
            rep.source = RS_SECDBG;
    } else if (m == M_XNU && xat >= 0) {
        src = t1 + xat;
        sn_ = n1 - (unsigned)xat;
    }
    if (!sn_ && sn.sb_rr >= 0)
        rep.source = RS_SBOOT;
    rep.text_len = (uint16_t)cl_tail(rep.text, CR_TEXT_MAX, src, sn_, &rep.text_lines);

    /* one line: why */
    if (ev & EV_LX_PANIC)
        reason_from(t0, n0, "Kernel panic - not syncing: ", "panic: ");
    else if (ev & EV_LX_OOPS) {
        int at = sb_find(t0, n0, "Internal error:");
        if (at < 0)
            at = sb_find(t0, n0, "Unable to handle");
        if (at < 0)
            at = sb_find(t0, n0, "BUG:");
        if (at >= 0)
            (void)sb_line_at(t0, n0, (unsigned)at, rep.reason, sizeof(rep.reason));
    } else if (ev & EV_XNU_PANIC) {
        int at = sb_find(t1 + xat, n1 - (unsigned)xat, "panic(");
        if (at >= 0)
            (void)sb_line_at(t1 + xat, n1 - (unsigned)xat, (unsigned)at, rep.reason, sizeof(rep.reason));
    } else if (ev & EV_XNU_HANG) {
        scopy(rep.reason, sizeof(rep.reason), "XNU's sweetreboot timer fired before it was cancelled");
    } else if (ev & EV_AND_PANIC && pn) {
        unsigned k = 0;
        for (; k < pn && k + 1 < sizeof(rep.reason) && ps[k] != '\n'; ++k)
            rep.reason[k] = (ps[k] >= 32 && ps[k] < 127) ? ps[k] : '.';
        rep.reason[k] = 0;
    }
    if (!rep.reason[0]) {
        struct tb t = { rep.reason, 0, sizeof(rep.reason) };
        if (kind == CK_WATCHDOG || kind == CK_LOADERWDT) {
            tb_s(&t, sn.rst_stat & RST_WDT_CL1 ? "cluster 1 watchdog" : sn.rst_stat & RST_WDT_CL0 ? "cluster 0 watchdog"
                                                                                               : "watchdog");
            if (launched && h->wdt_armed) {
                tb_s(&t, " not fed for ");
                tb_u(&t, h->wdt_s);
                tb_s(&t, " s");
            }
            tb_s(&t, ": the system stopped responding");
        } else if (kind == CK_FORCED) {
            tb_s(&t, "restart forced with the keys (Volume Down + Power)");
        } else {
            tb_s(&t, sb_kind_name(kind));
            if (sn.sb_rr >= 0) {
                tb_s(&t, ", S-Boot ");
                tb_s(&t, sb_rr_name(sn.sb_rr));
            }
        }
    }

    /* what has now been seen, so it is reported once */
    if (dmesg)
        h->seen_dmesg = c0;
    if (xat >= 0)
        h->seen_pmsg = cx;
    if (pn)
        h->seen_secmagic = cps;
    return kind;
}

static void log_line(unsigned kind, unsigned sev, const struct cl_head *h, int launched)
{
    char b[200];
    struct tb t = { b, 0, sizeof(b) };
    tb_s(&t, "STARBITE: crash: last launch ");
    tb_s(&t, launched ? h->key : "-");
    tb_s(&t, " (");
    tb_s(&t, launched ? method_name(h->method) : "none");
    tb_s(&t, "): ");
    tb_s(&t, sb_kind_name(kind));
    tb_s(&t, sev == SEV_CRASH ? " [crash]" : sev == SEV_WARN ? " [warn]" : "");
    tb_s(&t, " rst ");
    tb_x(&t, sn.rst_stat, 8);
    tb_s(&t, " inform3 ");
    tb_x(&t, sn.inform3, 8);
    tb_s(&t, " sboot_rr ");
    tb_i(&t, sn.sb_rr);
    tb_s(&t, "\n");
    star_pmsg(b);
    volatile uint64_t *lp = CR_LOGPAGE;
    lp[CR_SLOT] = 0x4352534bULL << 32 | (uint64_t)kind << 24 | (uint64_t)sev << 16 | (rep.reg[CR_FLAGS] >> 16);
    lp[CR_SLOT + 1] = (uint64_t)sn.rst_stat << 32 | sn.inform3;
    lp[CR_SLOT + 2] = (uint64_t)(uint32_t)sn.sb_rr << 32 | h->launch_seq;
    lp[CR_SLOT + 3] = h->count;
}

/* ---- the crash screen ------------------------------------------------------------------------- */

static void text_row(unsigned r, const char *s, uint32_t fg)
{
    star_fb_text_c(r, s, fg, C_BLACK);
}

/* Wrapped log rows of a report: count them (draw = 0), or draw rows [from, from + rows) at r0. */
static unsigned log_rows(const struct cl_report *r, unsigned from, unsigned rows, unsigned r0, int draw)
{
    unsigned idx = 0, n = 0;
    int wrapped = 0;
    char b[STAR_FB_COLS + 1];
    for (unsigned i = 0; i <= r->text_len; ++i) {
        int end = i == r->text_len;
        char c = end ? '\n' : r->text[i];
        if (c == '\n' && !n && (wrapped || end)) {
            wrapped = 0;        /* a line that filled its row exactly, or the text's end */
            continue;
        }
        if (c != '\n')
            b[n++] = c;
        if (c == '\n' || n == STAR_FB_COLS) {
            b[n] = 0;
            if (draw && idx >= from && idx < from + rows)
                text_row(r0 + idx - from, b, C_WHITE);
            ++idx;
            wrapped = c != '\n';
            n = 0;
        }
    }
    return idx;
}

/* Log rows the screen has room for at this level (screen_draw's layout). */
static unsigned rows_avail(unsigned level)
{
    return level >= 3 ? 43U - 20U : 43U - 11U;
}

static void reg_row(unsigned r, const char *a, uint32_t va, const char *b, uint32_t vb)
{
    struct row x;
    struct tb *t = row_init(&x);
    tb_s(t, a);
    tb_x(t, va, 8);
    if (b) {
        tb_s(t, " ");
        tb_s(t, b);
        tb_x(t, vb, 8);
    }
    text_row(r, x.b, C_MUTED);
}

static void screen_draw(const struct cl_report *r, unsigned level, unsigned page, int saved)
{
    struct row x;
    struct tb *t;
    star_fb_fill(C_BLACK);
    uint32_t hot = r->severity >= SEV_CRASH ? C_RED : C_GOLD;
    text_row(0, r->kind == CK_LOADERWDT ? "STARBITE: WATCHDOG RESET" :
                r->severity >= SEV_CRASH ? "STARBITE: THE LAST START CRASHED" : "STARBITE: THE LAST START ENDED BADLY", hot);
    t = row_init(&x);
    tb_s(t, r->name);
    if (r->key[0] && r->key[0] != '-') {
        tb_s(t, " (");
        tb_s(t, r->key);
        tb_s(t, ")");
    }
    text_row(1, x.b, C_WHITE);
    text_row(2, sb_kind_name(r->kind), hot);
    /* the reason over two rows */
    char a[STAR_FB_COLS + 1], b[STAR_FB_COLS + 1];
    unsigned rl = cstrlen(r->reason, sizeof(r->reason)), k;
    for (k = 0; k < STAR_FB_COLS && k < rl; ++k)
        a[k] = r->reason[k];
    a[k] = 0;
    unsigned j = 0;
    for (; k < rl && j < STAR_FB_COLS; ++k)
        b[j++] = r->reason[k];
    b[j] = 0;
    text_row(3, a, C_WHITE);
    text_row(4, b, C_WHITE);
    char d[20];
    t = row_init(&x);
    tb_s(t, "AT ");
    sb_date(r->epoch_event ? r->epoch_event : r->epoch_detect, d);
    tb_s(t, d);
    tb_s(t, r->epoch_event ? " (OS CLOCK)" : " UTC");
    text_row(5, x.b, C_MUTED);
    t = row_init(&x);
    tb_s(t, "STARTED ");
    sb_date(r->epoch_launch, d);
    tb_s(t, d);
    text_row(6, x.b, C_MUTED);
    t = row_init(&x);
    tb_s(t, "FROM: ");
    tb_s(t, sb_source_name(r->source));
    text_row(7, x.b, C_MUTED);
    t = row_init(&x);
    tb_s(t, "RESET:");
    for (unsigned bit = 0; bit < 32; ++bit)
        if (r->reg[CR_RST_STAT] & (1U << bit) && sb_rst_bit_name(bit)) {
            tb_s(t, " ");
            tb_s(t, sb_rst_bit_name(bit));
        }
    text_row(8, x.b, C_MUTED);
    t = row_init(&x);
    tb_s(t, "S-BOOT: ");
    tb_s(t, sb_rr_name((int32_t)r->reg[CR_SB_RR]));
    text_row(9, x.b, C_MUTED);

    unsigned row0 = 11;
    if (level == 1) {
        t = row_init(&x);
        tb_s(t, saved ? "SAVED AS REPORT #" : "REPORT #");
        tb_u(t, r->seq);
        text_row(row0, x.b, C_GREEN);
        text_row(row0 + 2, "VOL UP: THE LAST LOG LINES", C_MUTED);
        text_row(row0 + 3, "HOST: SBSTORE2.PY CRASHLOG LIVE", C_MUTED);
    } else {
        if (level >= 3) {
            const char *i3 = sb_inform3_name(r->reg[CR_INFORM3]);
            reg_row(row0++, "RST_STAT ", r->reg[CR_RST_STAT], "INF2 ", r->reg[CR_INFORM2]);
            t = row_init(&x);
            tb_s(t, "INFORM3 ");
            tb_x(t, r->reg[CR_INFORM3], 8);
            tb_s(t, " ");
            tb_s(t, i3 ? i3 : "?");
            text_row(row0++, x.b, C_MUTED);
            reg_row(row0++, "WDT DIS ", r->reg[CR_WDT_DIS], "MASK ", r->reg[CR_WDT_MASK]);
            reg_row(row0++, "WDT1 CON ", r->reg[CR_WDT1_CON], "CNT ", r->reg[CR_WDT1_CNT]);
            reg_row(row0++, "WDT1 DAT ", r->reg[CR_WDT1_DAT], "WDT0 ", r->reg[CR_WDT0_CON]);
            reg_row(row0++, "SB RST ", r->reg[CR_SB_RST], "ON ", r->reg[CR_SB_PWRON]);
            reg_row(row0++, "SB OFF ", r->reg[CR_SB_PWROFF], "SEC ", r->reg[CR_SEC_MAGIC]);
            t = row_init(&x);
            tb_s(t, "LAUNCH WDT ");
            tb_u(t, r->reg[CR_LAUNCH_WDT]);
            tb_s(t, " S FLAGS ");
            tb_x(t, r->reg[CR_FLAGS], 8);
            text_row(row0++, x.b, C_MUTED);
            ++row0;
        }
        unsigned rows = rows_avail(level), total = log_rows(r, 0, 0, 0, 0);
        unsigned end = total > page * rows ? total - page * rows : 0;
        unsigned from = end > rows ? end - rows : 0;
        if (!total)
            text_row(row0, "NO LOG LINES", C_MUTED);
        else
            (void)log_rows(r, from, end - from, row0, 1);
    }
    t = row_init(&x);
    tb_s(t, "VOL UP/DOWN: DETAIL ");
    tb_u(t, level);
    tb_s(t, " OF 3");
    if (level > 1)
        tb_s(t, "  BIXBY: OLDER");
    text_row(44, x.b, C_MUTED);
}

static void hint_timer(unsigned left)
{
    struct row x;
    struct tb *t = row_init(&x);
    tb_s(t, "POWER: CONTINUE");
    if (left) {
        tb_s(t, "   AUTO IN ");
        tb_u(t, left);
        tb_s(t, " S");
    }
    text_row(45, x.b, C_MUTED);
}

/* Released keys first (bounded: a stuck key cannot hold the boot), then one press. */
static unsigned keys_now(void)
{
    return hw_keys();
}

/* The crash screen: Volume Up/Down the level, Bixby older lines, Power leaves. timeout_s from the
 * last key press (0: wait for a key, still capped at SCREEN_CAP_S). Returns when it is done. */
static void crash_screen(const struct cl_report *r, unsigned level, unsigned timeout_s, int saved)
{
    if (level < 1 || level > 3)
        level = 1;
    unsigned page = 0;
    uint64_t f = hw_freq(), start = hw_ticks(), last = start;
    for (unsigned g = 0; g < 300 && keys_now(); ++g)   /* 3 s at most */
        hw_wait_ms(10);
    screen_draw(r, level, page, saved);
    unsigned shown = ~0U;
    unsigned prev = 0;
    for (;;) {
        uint64_t now = hw_ticks();
        if ((now - start) / f >= SCREEN_CAP_S)
            return;
        unsigned idle = (unsigned)((now - last) / f);
        unsigned left = timeout_s > idle ? timeout_s - idle : 0;
        if (timeout_s && !left)
            return;
        if (left != shown) {
            hint_timer(left);
            shown = left;
        }
        unsigned k = keys_now();
        if (k && !prev) {
            hw_wait_ms(30);
            k = keys_now();
            if (k) {
                last = hw_ticks();
                if (k & K_POWER)
                    break;
                if ((k & K_UP) && level < 3)
                    ++level, page = 0;
                else if ((k & K_DOWN) && level > 1)
                    --level, page = 0;
                else if (k & K_BIXBY)
                    page = level > 1 ? page + 1 : 0;
                if (page && page * rows_avail(level) >= log_rows(r, 0, 0, 0, 0))
                    page = 0;
                screen_draw(r, level, page, saved);
                shown = ~0U;
            }
        }
        prev = k;
        hw_wait_ms(20);
    }
    for (unsigned g = 0; g < 300 && keys_now(); ++g)
        hw_wait_ms(10);
}

/* ---- after the boot's POST -------------------------------------------------------------------- */

static int read_log(struct store_ent *e, struct cl_head *h)
{
    if (!store_crashlog(e) || ufs_read_to(e->lba, CL_BYTES / CL_BLOCK, CR_LOG))
        return -2;
    return cl_head_decode((const uint8_t *)CR_LOG, h);
}

/* Evaluate the pending launch once per boot. show: the crash screen and the configured action
 * (the boot path); 0: only save (a start from Starbite Recovery or fastboot comes first). */
static int settled;

static int settle(int show, int *retry)
{
    struct store_ent e;
    struct cl_head h;
    if (settled)
        return CRASH_GO_MENU;
    settled = 1;
    cs_load();
    if (cs.crash_mode == SET_CRASH_OFF || !sn.taken) {
        star_pmsg("STARBITE: crash: detection off\n");
        return CRASH_GO_MENU;
    }
    int hv = read_log(&e, &h);
    if (hv == -2) {
        star_pmsg("STARBITE: crash: no crashlog entry in the store (tools/store/sbstore2.py build adds it)\n");
        return CRASH_GO_MENU;
    }
    if (hv != 1) {
        cl_head_init(&h);
        cl_head_encode((uint8_t *)CR_LOG, &h);
        int w = store_crashlog_write(&e, 0, CR_LOG);
        star_pmsg(w ? "STARBITE: crash: could not start the crash log\n"
                    : hv ? "STARBITE: crash: crash log header was bad: started again\n" : "STARBITE: crash: crash log started\n");
        return CRASH_GO_MENU;
    }
    int launched = h.launch_state == CL_RUNNING;
    unsigned lflags = h.lflags;
    char key[24];
    scopy(key, sizeof(key), h.key);
    unsigned sev = SEV_INFO;
    uint32_t seen0 = h.seen_dmesg, seen1 = h.seen_pmsg, seen2 = h.seen_secmagic;
    unsigned kind = evaluate(&h, &sev);
    int dirty = launched || seen0 != h.seen_dmesg || seen1 != h.seen_pmsg || seen2 != h.seen_secmagic;
    if (launched) {
        h.launch_state = CL_IDLE;
        ++h.boots;
    }
    int saved = 0;
    if (sev >= SEV_WARN) {
        unsigned slot = h.next % CL_SLOTS;
        cl_report_encode((uint8_t *)(CR_LOG + CL_BLOCK * (1U + slot)), &rep);
        if (!store_crashlog_write(&e, 1U + slot, CR_LOG + CL_BLOCK * (1U + slot))) {
            h.next = (slot + 1U) % CL_SLOTS;
            ++h.count;
            saved = 1;
        }
        dirty = 1;
    }
    if (dirty) {
        cl_head_encode((uint8_t *)CR_LOG, &h);
        if (store_crashlog_write(&e, 0, CR_LOG))
            star_pmsg("STARBITE: crash: crash log header write failed\n");
    }
    log_line(kind, sev, &h, launched);
    if (sev < SEV_WARN)
        return CRASH_GO_MENU;
    hold_autoboot = 1;
    if (!show)
        return CRASH_GO_MENU;
    if (cs.crash_mode == SET_CRASH_ON) {
        rec_rows_save(0, STAR_FB_HEIGHT, CR_PANEL);
        unsigned base = rec_row_base();
        star_fb_row_base(0);
        star_fb_mute(0);
        crash_screen(&rep, cs.crash_detail ? cs.crash_detail : 1U, sbset_crash_timeout(&cs), saved);
        rec_rows_restore(0, STAR_FB_HEIGHT, CR_PANEL);
        star_fb_row_base(base);
    }
    if (cs.crash_action == SET_AFTER_RECOVERY)
        return CRASH_GO_RECOVERY;
    if (cs.crash_action == SET_AFTER_RETRY && launched && !(lflags & (CLF_RETRY | CLF_FASTBOOT | CLF_RESCUE))) {
        int i = cat_find(key);
        if (i >= 0 && star_entry_ready((unsigned)i, 0)) {
            *retry = i;
            star_pmsg("STARBITE: crash: starting it once more (settings: retry)\n");
            return CRASH_GO_RETRY;
        }
    }
    return CRASH_GO_MENU;
}

int crash_after(int *retry)
{
    return settle(1, retry);
}

/* ---- the launch record ------------------------------------------------------------------------ */

void crash_launch(const char *key, const char *name, unsigned method, unsigned wdt_s, unsigned wdt_armed,
                  unsigned log_level, unsigned lflags)
{
    struct store_ent e;
    struct cl_head h;
    (void)settle(0, 0);           /* a launch still pending from the last boot is judged first */
    cs_load();
    if (cs.crash_mode == SET_CRASH_OFF || !store_crashlog(&e) || ufs_read(e.lba, 1))
        return;
    copy64(LAUNCH_BUF, UFS_BUF, CL_BLOCK);
    if (cl_head_decode((const uint8_t *)LAUNCH_BUF, &h) != 1)
        cl_head_init(&h);
    ++h.launch_seq;
    h.launch_state = CL_RUNNING;
    h.method = (uint8_t)method;
    h.wdt_armed = (uint8_t)(wdt_armed != 0);
    h.wdt_s = (uint16_t)wdt_s;
    scopy(h.key, sizeof(h.key), key);
    scopy(h.name, sizeof(h.name), name);
    uint64_t now = 0;
    (void)em_rtc_epoch(&now);
    h.launch_epoch = now;
    h.log_level = log_level;
    h.lflags = lflags;
    cl_head_encode((uint8_t *)LAUNCH_BUF, &h);
    char b[120];
    struct tb t = { b, 0, sizeof(b) };
    tb_s(&t, "STARBITE: launch ");
    tb_s(&t, key);
    tb_s(&t, " #");
    tb_u(&t, h.launch_seq);
    tb_s(&t, wdt_armed ? " watchdog " : " no watchdog");
    if (wdt_armed) {
        tb_u(&t, wdt_s);
        tb_s(&t, " s");
    }
    tb_s(&t, store_crashlog_write(&e, 0, LAUNCH_BUF) ? " (launch record NOT written)\n" : "\n");
    star_pmsg(b);
}

/* ---- the recovery menu's list ----------------------------------------------------------------- */

void crash_browse(void)
{
    struct store_ent e;
    struct cl_head h;
    static struct cl_report r;
    (void)settle(0, 0);
    cs_load();
    star_fb_row_base(0);
    star_fb_mute(0);
    star_fb_fill(C_BLACK);
    text_row(0, "STARBITE RECOVERY: CRASH REPORTS", C_GOLD);
    int hv = read_log(&e, &h);
    if (hv == -2) {
        text_row(2, "NO CRASH LOG IN THE OS STORE", C_RED);
        text_row(3, "REBUILD THE STORE: SBSTORE2.PY BUILD", C_MUTED);
    } else if (hv != 1) {
        text_row(2, "THE CRASH LOG IS EMPTY", C_MUTED);
    }
    unsigned n = hv == 1 ? (h.count < CL_SLOTS ? h.count : CL_SLOTS) : 0;
    if (hv == 1 && !n)
        text_row(2, "NO REPORTS YET", C_MUTED);
    unsigned sel = 0;
    for (;;) {
        for (unsigned i = 0; i <= n; ++i) {
            struct row x;
            struct tb *t = row_init(&x);
            tb_s(t, i == sel ? "> " : "  ");
            if (i == n) {
                tb_s(t, "BACK");
            } else {
                int slot = cl_newest(&h, i);
                if (slot < 0 || cl_report_decode((const uint8_t *)(CR_LOG + CL_BLOCK * (1U + (unsigned)slot)), &r) != 1) {
                    tb_s(t, "(UNREADABLE)");
                } else {
                    char d[20];
                    tb_s(t, "#");
                    tb_u(t, r.seq);
                    tb_s(t, " ");
                    sb_date(r.epoch_detect, d);
                    tb_s(t, d + 5);
                    tb_s(t, " ");
                    tb_s(t, r.key);
                }
            }
            star_fb_text_c(4 + i, x.b, C_WHITE, i == sel ? C_BLUE : C_BLACK);
        }
        text_row(44, "VOL UP/DOWN: MOVE   POWER: OPEN", C_MUTED);
        unsigned k;
        for (unsigned g = 0; g < 300 && hw_keys(); ++g)
            hw_wait_ms(10);
        for (;;) {
            k = hw_keys();
            if (k) {
                hw_wait_ms(30);
                if (hw_keys() == k)
                    break;
            }
            hw_wait_ms(10);
        }
        if (k & K_UP)
            sel = sel ? sel - 1 : n;
        else if (k & K_DOWN)
            sel = sel < n ? sel + 1 : 0;
        else if (k & (K_POWER | K_BIXBY)) {
            if (sel == n)
                return;
            int slot = cl_newest(&h, sel);
            if (slot >= 0 && cl_report_decode((const uint8_t *)(CR_LOG + CL_BLOCK * (1U + (unsigned)slot)), &r) == 1)
                crash_screen(&r, 3, 0, 1);
            star_fb_fill(C_BLACK);
            text_row(0, "STARBITE RECOVERY: CRASH REPORTS", C_GOLD);
        }
    }
}

/* ---- fastboot ---------------------------------------------------------------------------------- */

static void text_report(struct tb *t, const struct cl_report *r)
{
    char d[20];
    tb_s(t, "--- report #");
    tb_u(t, r->seq);
    tb_s(t, ": ");
    tb_s(t, r->name);
    tb_s(t, " (");
    tb_s(t, r->key);
    tb_s(t, ", ");
    tb_s(t, method_name(r->method));
    tb_s(t, ")\nkind: ");
    tb_s(t, sb_kind_name(r->kind));
    tb_s(t, r->severity >= SEV_CRASH ? " [crash]" : " [warn]");
    tb_s(t, "\nreason: ");
    tb_s(t, r->reason);
    tb_s(t, "\ndetected: ");
    sb_date(r->epoch_detect, d);
    tb_s(t, d);
    tb_s(t, "  launched: ");
    sb_date(r->epoch_launch, d);
    tb_s(t, d);
    if (r->epoch_event) {
        tb_s(t, "  event (OS clock): ");
        sb_date(r->epoch_event, d);
        tb_s(t, d);
    }
    tb_s(t, "\nsource: ");
    tb_s(t, sb_source_name(r->source));
    tb_s(t, "\nRST_STAT ");
    tb_x(t, r->reg[CR_RST_STAT], 8);
    for (unsigned bit = 0; bit < 32; ++bit)
        if (r->reg[CR_RST_STAT] & (1U << bit) && sb_rst_bit_name(bit)) {
            tb_s(t, " ");
            tb_s(t, sb_rst_bit_name(bit));
        }
    tb_s(t, "\nINFORM2 ");
    tb_x(t, r->reg[CR_INFORM2], 8);
    tb_s(t, " INFORM3 ");
    tb_x(t, r->reg[CR_INFORM3], 8);
    tb_s(t, "\nS-Boot reset_reason ");
    tb_i(t, (int32_t)r->reg[CR_SB_RR]);
    tb_s(t, " (");
    tb_s(t, sb_rr_name((int32_t)r->reg[CR_SB_RR]));
    tb_s(t, ") rst_stat ");
    tb_x(t, r->reg[CR_SB_RST], 8);
    tb_s(t, " pwronsrc ");
    tb_x(t, r->reg[CR_SB_PWRON], 2);
    tb_s(t, " pwroffsrc ");
    tb_x(t, r->reg[CR_SB_PWROFF], 2);
    tb_s(t, "\nPMU WDT disable ");
    tb_x(t, r->reg[CR_WDT_DIS], 8);
    tb_s(t, " mask ");
    tb_x(t, r->reg[CR_WDT_MASK], 8);
    tb_s(t, "  WDT1 con ");
    tb_x(t, r->reg[CR_WDT1_CON], 8);
    tb_s(t, " dat ");
    tb_x(t, r->reg[CR_WDT1_DAT], 8);
    tb_s(t, " cnt ");
    tb_x(t, r->reg[CR_WDT1_CNT], 8);
    tb_s(t, "  WDT0 con ");
    tb_x(t, r->reg[CR_WDT0_CON], 8);
    tb_s(t, "\nsec_debug magic ");
    tb_x(t, r->reg[CR_SEC_MAGIC], 8);
    tb_s(t, "  launch watchdog ");
    tb_u(t, r->reg[CR_LAUNCH_WDT]);
    tb_s(t, " s  flags ");
    tb_x(t, r->reg[CR_FLAGS], 8);
    tb_s(t, "\nlast lines:\n");
    for (unsigned i = 0; i < r->text_len; ++i)
        tb_c(t, r->text[i]);
    tb_s(t, "\n");
}

unsigned crash_text(char *out, unsigned cap)
{
    struct store_ent e;
    struct cl_head h;
    static struct cl_report r;
    struct tb t = { out, 0, cap };
    int hv = read_log(&e, &h);
    if (hv == -2)
        return 0;
    tb_s(&t, "crash log: ");
    if (hv != 1) {
        tb_s(&t, "no valid header yet\n");
        return t.n;
    }
    tb_u(&t, h.count);
    tb_s(&t, " reports written, ");
    tb_u(&t, h.boots);
    tb_s(&t, " launches evaluated; last launch #");
    tb_u(&t, h.launch_seq);
    tb_s(&t, " ");
    tb_s(&t, h.key);
    tb_s(&t, h.launch_state == CL_RUNNING ? " (running)\n" : " (ended)\n");
    for (unsigned k = 0; k < CL_SLOTS; ++k) {
        int slot = cl_newest(&h, k);
        if (slot < 0)
            break;
        if (cl_report_decode((const uint8_t *)(CR_LOG + CL_BLOCK * (1U + (unsigned)slot)), &r) == 1)
            text_report(&t, &r);
    }
    return t.n;
}

unsigned crash_raw(char *out, unsigned cap)
{
    struct store_ent e;
    struct cl_head h;
    if (cap < CL_BYTES || read_log(&e, &h) == -2)
        return 0;
    copy64((uintptr_t)out, CR_LOG, CL_BYTES);
    return CL_BYTES;
}
