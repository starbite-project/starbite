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
#include "crashdoc.h"
#include "fbbg.h"
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

/* What evaluate() found this boot, all of it, for the crash screen (CR_T0 / CR_T1 stay as they
 * are until the OS is loaded). */
static const char *live_src, *live_other;
static unsigned live_n, live_other_n, live_other_src;

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
static struct sb_found found;       /* evaluate's evidence, consumed by settle */

/* The judged evidence marked in the live ramoops zones (sbfmt.c sb_consume), cleaned to RAM as
 * the loader's pmsg writes are (xnu_boot.c pmsg_put). */
static unsigned consume(struct cl_head *h)
{
    unsigned d = sb_consume((volatile uint8_t *)RAMOOPS, (volatile uint8_t *)(RAMOOPS + ZONE),
                            (const uint8_t *)CR_SNAP, (const uint8_t *)(CR_SNAP + ZONE), ZONE, &found, h);
    for (uintptr_t a = RAMOOPS; a < RAMOOPS + 2U * ZONE; a += 64)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
    return d;
}

static unsigned evaluate(struct cl_head *h, unsigned *sev)
{
    char *t0 = (char *)CR_T0, *t1 = (char *)CR_T1;
    unsigned n0 = sb_ring_text((const uint8_t *)CR_SNAP, ZONE, t0, ZONE);
    unsigned n1 = sb_ring_text((const uint8_t *)(CR_SNAP + ZONE), ZONE, t1, ZONE);
    int launched = h->launch_state == CL_RUNNING;
    unsigned m = launched ? h->method : 0;
    uint32_t ev = 0;

    /* Linux: a ramoops dmesg record "====<time>-D" in zone 0 not judged yet; XNU: starlte_pmsg.c's
     * dump in the pmsg ring (its panic or timer header). sbfmt.c sb_evidence: what the loader has
     * judged it marks in RAM (sb_consume, in settle), so an identical later crash is still new. */
    int xp = sb_find(t1, n1, "STARBITE: XNU panic (sweetreboot)");
    int xh = sb_find(t1, n1, "STARBITE: sweetreboot timer (no panic)");
    sb_evidence(t0, n0, t1, n1, xp, xh, m == M_LINUX, m == M_XNU, h, &found);
    ev = found.ev;
    int dmesg = found.dmesg, xat = found.xat;
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
        if (n0 && !(n0 >= 4 && t0[0] == '=' && t0[1] == '=' && t0[2] == '=' && t0[3] == '=')) {   /* a record (judged or not) is not a console */
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
    /* the report keeps the message, registers and trace lines and as much of the end as fits;
     * the crash screen of this boot shows all of it (live_*) */
    rep.text_len = (uint16_t)cl_excerpt(rep.text, CR_TEXT_MAX, src, sn_, &rep.text_lines);
    live_src = src;
    live_n = sn_;
    live_other = 0;
    live_other_n = 0;
    if (m == M_LINUX && n0 && n1) {
        live_other = src == t0 ? t1 : t0;
        live_other_n = src == t0 ? n1 : n0;
        live_other_src = src == t0 ? RS_CONSOLE : (dmesg ? RS_DMESG : RS_CONSOLE);
    }

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

    /* what has now been seen, so it is reported once (dmesg and XNU: marked in RAM by settle) */
    (void)dmesg;
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

/* ---- the crash screen ------------------------------------------------------------------------- *
 * A scrolling report in the loader's 5x7 text style, small (fbtext.c star_fb_mini: 76 columns,
 * 78 rows a page). The document is built once per level from the report and, right after the
 * crash, from everything the loader captured this boot (the ramoops zones as text, CR_T0/CR_T1);
 * a report opened later from Starbite Recovery has the excerpt the store keeps (cl_excerpt):
 *   the header       report number, OS and entry, kernel version, when (wall time, uptime), task,
 *                    reason, the reset registers (RST_STAT bits, INFORM2/3, S-Boot's code), the
 *                    watchdog, where the text came from and where the report is stored
 *   PANIC MESSAGE    every message line (sbfmt.c sb_crash_parse: CT_MSG)
 *   CONTEXT          CPU/PID/Comm, hardware, modules, XNU's version lines (details and up)
 *   REGISTERS        the CPU register dump the OS printed (details and up)
 *   CALL TRACE       the frames (summary: the first 12)
 *   RESET AND WATCHDOG REGISTERS   all 16 saved registers (everything)
 *   LOG              all of it, at every level (so it always scrolls)
 *   OTHER RING       Linux: the other ramoops zone (everything)
 * The document and the keys are crashdoc.c (no hardware, host-tested on real captures); this file
 * draws them and polls the keys.
 * Keys: Volume Up / Down scroll a line (held: repeats, then 8 lines at a time), Bixby a page or to
 * the next section (held 0.7 s: the next detail level), Power continues. One footer line: the
 * keys, the rows shown, the time left. */

#define DOC_TXT       0x9F030000UL     /* 64 KiB: the composed header lines */
#define DOC_TXT_SIZE  0x10000U
#define DOC_ROWS      0x9F040000UL     /* the rows (struct cd_row), DOC_MAX of them */
#define DOC_KINDS     0x9F060000UL     /* sb_crash_parse's kind of each line of the text */
#define DOC_MAX       6144U
#define DOC_KINDS_MAX 16384U
#define SCR_X     36U
#define SCR_Y     60U
#define SCR_PAGE  78U                  /* rows a page: 60 + 78 x 36 = 2868 */
#define SCR_FOOT  2880U                /* the footer row's y */
#define C_LOG     0xffc9cfdcU
#define C_CYAN    0xff8fd6ffU

_Static_assert(sizeof(struct cd_row) == 16, "cd_row");
_Static_assert(DOC_ROWS + DOC_MAX * 16U <= DOC_KINDS, "crash screen rows overlap the kinds");
_Static_assert(DOC_KINDS + DOC_KINDS_MAX <= 0xA0000000UL, "crash screen buffers reach S-Boot's ECT");
_Static_assert(CS_K_UP == K_UP && CS_K_DOWN == K_DOWN && CS_K_BIXBY == K_BIXBY && CS_K_POWER == K_POWER, "keys");
_Static_assert(CD_COLS * STAR_FB_MINI_W + SCR_X <= STAR_FB_WIDTH, "a row fits the panel");

static void text_row(unsigned r, const char *s, uint32_t fg)
{
    star_fb_text_c(r, s, fg, C_BLACK);
}

static uint32_t dcolor(unsigned c)
{
    switch (c) {
    case CD_MUTED: return C_MUTED;
    case CD_GOLD:  return C_GOLD;
    case CD_RED:   return C_RED;
    case CD_GREEN: return C_GREEN;
    case CD_LOG:   return C_LOG;
    case CD_CYAN:  return C_CYAN;
    default:       return C_WHITE;
    }
}

static struct cd_doc doc;

static void doc_build(const struct cl_report *r, unsigned level, const struct cd_shown *sh)
{
    doc.rows = (struct cd_row *)DOC_ROWS;
    doc.max = DOC_MAX;
    doc.txt = (char *)DOC_TXT;
    doc.txt_cap = DOC_TXT_SIZE;
    doc.kinds = (uint8_t *)DOC_KINDS;
    doc.kinds_max = DOC_KINDS_MAX;
    cd_build(&doc, r, level, sh);
}

static void draw_page(unsigned top)
{
    char b[CD_COLS + 1];
    for (unsigned i = 0; i < SCR_PAGE; ++i) {
        unsigned row = top + i, n = 0;
        uint32_t fg = C_WHITE;
        if (row < doc.n) {
            const struct cd_row *r = &doc.rows[row];
            const char *p = (const char *)(uintptr_t)r->p;
            for (unsigned k = 0; k < r->indent && n < CD_COLS; ++k)
                b[n++] = ' ';
            for (unsigned k = 0; k < r->n && n < CD_COLS; ++k)
                b[n++] = p[k] == '\t' ? ' ' : p[k];
            fg = dcolor(r->color);
        }
        star_fb_mini(SCR_X, SCR_Y + i * STAR_FB_MINI_H, b, n, CD_COLS, fg, C_BLACK);
    }
}

static void draw_footer(unsigned top, unsigned level, unsigned left)
{
    char b[CD_COLS + 1];
    struct tb t = { b, 0, sizeof(b) };
    tb_s(&t, "VOL SCROLL  BIXBY PAGE (HOLD: DETAIL ");
    tb_u(&t, level);
    tb_s(&t, "/3)  POWER CONTINUE  ");
    unsigned last = top + SCR_PAGE < doc.n ? top + SCR_PAGE : doc.n;
    tb_u(&t, doc.n ? top + 1U : 0U);
    tb_c(&t, '-');
    tb_u(&t, last);
    tb_c(&t, '/');
    tb_u(&t, doc.n);
    if (left) {
        tb_s(&t, "  ");
        tb_u(&t, left);
        tb_c(&t, 's');
    }
    star_fb_mini(SCR_X, SCR_FOOT, b, t.n, CD_COLS, C_MUTED, C_BLACK);
}

static uint64_t ms_now(void)
{
    return hw_ticks() * 1000U / hw_freq();
}

/* The crash screen. timeout_s counts from the last key (0: wait for one, still capped at
 * SCREEN_CAP_S). The keys are crashdoc.c's cs_poll (host-tested with real timing). */
static void crash_screen(const struct cl_report *r, unsigned level, unsigned timeout_s, const struct cd_shown *sh)
{
    if (level < 1 || level > 3)
        level = 1;
    for (unsigned g = 0; g < 300 && hw_keys(); ++g)   /* 3 s at most */
        hw_wait_ms(10);
    doc_build(r, level, sh);
    struct cs_nav nav;
    cs_init(&nav, SCR_PAGE);
    cs_doc(&nav, &doc);
    star_fb_fill(C_BLACK);
    draw_page(0);
    unsigned shown_left = ~0U, shown_top = ~0U;
    uint64_t start = ms_now(), last = start;
    for (;;) {
        uint64_t now = ms_now();
        if (now - start >= SCREEN_CAP_S * 1000U)
            break;
        unsigned idle = (unsigned)((now - last) / 1000U);
        unsigned left = timeout_s > idle ? timeout_s - idle : 0;
        if (timeout_s && !left)
            break;
        /* the Mac over USB (fbbg.h): its presses come in through hw_keys(); an image it
         * placed ends the screen (the loader then starts it) */
        if (fbbg_poll(FBBG_CRASH) == FBBG_BOOT)
            break;
        unsigned ev = cs_poll(&nav, hw_keys(), now);
        if (ev & CS_EV_KEY)
            last = now;
        if (ev & CS_EV_EXIT)
            break;
        if (ev & CS_EV_LEVEL) {
            level = level % 3U + 1U;
            doc_build(r, level, sh);
            cs_doc(&nav, &doc);
            draw_page(nav.top);
            shown_top = ~0U;
        } else if (ev & CS_EV_MOVED) {
            draw_page(nav.top);
        }
        if (nav.top != shown_top || left != shown_left) {
            draw_footer(nav.top, level, left);
            shown_top = nav.top;
            shown_left = left;
        }
        hw_wait_ms(5);
    }
    for (unsigned g = 0; g < 300 && hw_keys(); ++g)
        hw_wait_ms(10);
}

/* ---- after the boot's POST -------------------------------------------------------------------- */

static int read_log(struct store_ent *e, struct cl_head *h)
{
    if (!store_crashlog(e) || store_read(e, 0, CL_BYTES / CL_BLOCK, CR_LOG))
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
    uint32_t seen0 = h.seen_dmesg, seen1 = h.seen_pmsg, seen2 = h.seen_secmagic, hf = h.hflags;
    unsigned kind = evaluate(&h, &sev);
    unsigned marked = consume(&h);
    if (marked) {
        char b[96];
        struct tb t = { b, 0, sizeof(b) };
        tb_s(&t, "STARBITE: crash: judged evidence marked in RAM:");
        tb_s(&t, marked & 1U ? " dmesg record" : "");
        tb_s(&t, marked & 2U ? " xnu dump" : "");
        tb_s(&t, "\n");
        star_pmsg(b);
    }
    int dirty = launched || seen0 != h.seen_dmesg || seen1 != h.seen_pmsg || seen2 != h.seen_secmagic || hf != h.hflags;
    if (launched) {
        h.launch_state = CL_IDLE;
        ++h.boots;
    }
    int saved = -1;              /* the report's slot once it is in the crash log */
    if (sev >= SEV_WARN) {
        unsigned slot = h.next % CL_SLOTS;
        cl_report_encode((uint8_t *)(CR_LOG + CL_BLOCK * (1U + slot)), &rep);
        if (!store_crashlog_write(&e, 1U + slot, CR_LOG + CL_BLOCK * (1U + slot))) {
            h.next = (slot + 1U) % CL_SLOTS;
            ++h.count;
            saved = (int)slot;
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
        struct cd_shown sh = { live_src, live_n, 1, live_other, live_other_n, live_other_src, saved };
        if (!live_src) {
            sh.text = rep.text;
            sh.n = rep.text_len;
            sh.live = 0;
        }
        crash_screen(&rep, cs.crash_detail ? cs.crash_detail : 1U, sbset_crash_timeout(&cs), &sh);
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
    if (cs.crash_mode == SET_CRASH_OFF || !store_crashlog(&e) || store_read(&e, 0, 1, LAUNCH_BUF))
        return;
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
        for (unsigned g = 0; g < 300 && hw_keys(); ++g) {
            (void)fbbg_poll(FBBG_RECOVERY);
            hw_wait_ms(10);
        }
        for (;;) {
            if (fbbg_poll(FBBG_RECOVERY) == FBBG_BOOT)
                return;                 /* an image from USB: back up to recovery_main */
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
            if (slot >= 0 && cl_report_decode((const uint8_t *)(CR_LOG + CL_BLOCK * (1U + (unsigned)slot)), &r) == 1) {
                struct cd_shown sh = { r.text, r.text_len, 0, 0, 0, 0, slot };
                crash_screen(&r, 3, 0, &sh);
            }
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
