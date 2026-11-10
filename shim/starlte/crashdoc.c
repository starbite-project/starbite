/* SPDX-License-Identifier: GPL-2.0-only
 * The crash report as a document, and its keys: see crashdoc.h. No hardware, no pointer tables;
 * built into the loader and into the host test (tools/loader-test), the same code.
 */
#include "crashdoc.h"
#include "catalog.h"

/* ---- text builder ----------------------------------------------------------------------------- */

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

static unsigned cstrlen(const char *s, unsigned max)
{
    unsigned n = 0;
    while (n < max && s[n])
        ++n;
    return n;
}

/* ---- the document ---------------------------------------------------------------------------- */

static struct cd_doc *D;            /* the one being built (cd_build) */

static void doc_reset(void)
{
    D->n = D->txt_n = D->nsect = D->sect = 0;
}

/* A line of n bytes as rows: hard wrapped at the page width, continuation rows indented by 2. */
static void doc_add(const char *p, unsigned n, unsigned color, unsigned indent)
{
    unsigned ind = indent, w = CD_COLS - indent, at = 0;
    do {
        if (D->n >= D->max)
            return;
        unsigned take = n - at > w ? w : n - at;
        if (n - at > w)                     /* break after a space in the row's second half */
            for (unsigned k = w; k > w / 2U; --k)
                if (p[at + k - 1U] == ' ') {
                    take = k;
                    break;
                }
        struct cd_row *r = &D->rows[D->n++];
        r->p = (uintptr_t)(p + at);
        r->n = (uint16_t)take;
        r->color = (uint8_t)color;
        r->sect = (uint8_t)D->sect;
        r->indent = (uint8_t)ind;
        at += take;
        ind = indent + 2U;
        w = CD_COLS - ind;
    } while (at < n);
}

/* A composed line in the caller's text buffer */
static struct tb *dl_begin(struct tb *t)
{
    t->b = D->txt + D->txt_n;
    t->n = 0;
    t->cap = D->txt_cap - D->txt_n > 600U ? 600U : D->txt_cap - D->txt_n;
    if (t->cap)
        t->b[0] = 0;
    return t;
}

static void dl_end(struct tb *t, unsigned color)
{
    doc_add(t->b, t->n, color, 0);
    D->txt_n += t->n + 1U;
}

static void dl_line(const char *s, unsigned color)
{
    struct tb t;
    tb_s(dl_begin(&t), s);
    dl_end(&t, color);
}

static void tb_pad(struct tb *t, unsigned col)
{
    while (t->n < col)
        tb_c(t, ' ');
}

/* "-- TITLE ---------" */
static void doc_section(const char *title, const char *more)
{
    if (D->nsect < sizeof(D->sect_row) / sizeof(D->sect_row[0]))
        D->sect_row[D->nsect++] = D->n;
    D->sect = D->nsect;
    dl_line("", CD_WHITE);
    struct tb t;
    dl_begin(&t);
    tb_s(&t, "-- ");
    tb_s(&t, title);
    if (more) {
        tb_s(&t, " ");
        tb_s(&t, more);
    }
    tb_c(&t, ' ');
    while (t.n < CD_COLS)
        tb_c(&t, '-');
    dl_end(&t, CD_GOLD);
}

static unsigned kind_color(unsigned k)
{
    return k == CT_MSG ? CD_RED : k == CT_REGS ? CD_CYAN : k == CT_TRACE ? CD_WHITE : k == CT_INFO ? CD_MUTED : CD_LOG;
}

/* The lines of text whose kind is want (or every line for want = 0xff) from line `from` on, at
 * most max of them; returns how many were added. Lines lose their "<N>" prefix; the extracted
 * sections (want != 0xff) their "[ time] " too, so a register line fits one row. */
static unsigned doc_lines(const char *text, unsigned n, const uint8_t *kinds, unsigned nk, unsigned want,
                          unsigned from, unsigned max)
{
    unsigned i = 0, added = 0;
    for (unsigned s = 0; s < n && added < max;) {
        unsigned e = s;
        while (e < n && text[e] != '\n')
            ++e;
        unsigned len = e - s;
        while (len && (text[s + len - 1] == '\r' || !text[s + len - 1]))
            --len;
        unsigned k = i < nk ? kinds[i] : CT_LOG;
        if (i >= from && (want == 0xffU || k == want)) {
            unsigned p = sb_crash_prefix(text + s, len);
            if (want != 0xffU)
                p += sb_crash_stamp(text + s + p, len - p);
            doc_add(text + s + p, len - p, kind_color(k), 0);
            ++added;
        }
        ++i;
        s = e + 1U;
    }
    return added;
}

static void tb_ms(struct tb *t, uint64_t ms)
{
    tb_u(t, ms / 1000U);
    tb_c(t, '.');
    tb_c(t, (char)('0' + ms / 100U % 10U));
    tb_c(t, (char)('0' + ms / 10U % 10U));
    tb_c(t, (char)('0' + ms % 10U));
    tb_s(t, " s");
}

static const char *reg_name(unsigned i)
{
    switch (i) {
    case CR_RST_STAT:   return "PMU RST_STAT";
    case CR_INFORM2:    return "PMU INFORM2";
    case CR_INFORM3:    return "PMU INFORM3";
    case CR_WDT_DIS:    return "PMU WDT disable";
    case CR_WDT_MASK:   return "PMU WDT mask";
    case CR_SB_RR:      return "S-Boot reset_reason";
    case CR_SB_RST:     return "S-Boot rst_stat";
    case CR_SB_PWRON:   return "S-Boot pwronsrc";
    case CR_SB_PWROFF:  return "S-Boot pwroffsrc";
    case CR_SEC_MAGIC:  return "sec_debug magic";
    case CR_WDT1_CON:   return "WDT1 WTCON";
    case CR_WDT1_DAT:   return "WDT1 WTDAT";
    case CR_WDT1_CNT:   return "WDT1 WTCNT";
    case CR_WDT0_CON:   return "WDT0 WTCON";
    case CR_LAUNCH_WDT: return "launch watchdog s";
    default:            return "flags (launch | evidence << 16)";
    }
}


/* The report as rows at this level (1 summary, 2 details, 3 everything). */
void cd_build(struct cd_doc *d, const struct cl_report *r, unsigned level, const struct cd_shown *sh)
{
    D = d;
    struct sb_crashinfo *cp = &d->ci;
    uint8_t *kinds = D->kinds;
    unsigned nl = sb_crash_parse(sh->text, sh->n, cp, kinds, D->kinds_max);
    unsigned nk = nl < D->kinds_max ? nl : D->kinds_max;
    struct tb t;
    char dt[20];
    unsigned hot = r->severity >= SEV_CRASH ? CD_RED : CD_GOLD;
    doc_reset();

    /* the header */
    dl_begin(&t);
    tb_s(&t, "STARBITE CRASH REPORT");
    if (sh->slot >= 0) {
        tb_s(&t, " #");
        tb_u(&t, r->seq);
    } else {
        tb_s(&t, " (NOT SAVED)");
    }
    {
        const char *kn = r->kind == CK_LOADERWDT ? "WATCHDOG RESET" : sb_kind_name(r->kind);
        unsigned kl = cstrlen(kn, 40) + (r->severity >= SEV_CRASH ? 8U : 7U);
        tb_pad(&t, CD_COLS > kl + t.n + 1U ? CD_COLS - kl : t.n + 1U);
        tb_s(&t, kn);
        tb_s(&t, r->severity >= SEV_CRASH ? " [CRASH]" : " [WARN]");
    }
    dl_end(&t, hot);
    dl_begin(&t);
    tb_s(&t, "OS       ");
    tb_s(&t, r->name);
    if (r->key[0] && r->key[0] != '-') {
        tb_s(&t, " (");
        tb_s(&t, r->key);
        tb_s(&t, ", ");
        tb_s(&t, method_name(r->method));
        tb_s(&t, ")");
    }
    dl_end(&t, CD_WHITE);
    dl_begin(&t);
    tb_s(&t, "KERNEL   ");
    tb_s(&t, cp->version[0] ? cp->version : "(no version line in the captured text)");
    dl_end(&t, cp->version[0] ? CD_WHITE : CD_MUTED);
    dl_begin(&t);
    tb_s(&t, "CRASHED  ");
    if (r->epoch_event || r->epoch_detect) {
        sb_date(r->epoch_event ? r->epoch_event : r->epoch_detect, dt);
        tb_s(&t, dt);
        tb_s(&t, r->epoch_event ? " UTC (OS clock)" : " UTC (detected)");
    } else {
        tb_s(&t, "time unknown");
    }
    tb_pad(&t, 45);
    tb_s(&t, "UPTIME ");
    if (cp->uptime_ms)
        tb_ms(&t, cp->uptime_ms);
    else
        tb_s(&t, "?");
    dl_end(&t, CD_WHITE);
    dl_begin(&t);
    tb_s(&t, "STARTED  ");
    sb_date(r->epoch_launch, dt);
    tb_s(&t, dt);
    tb_s(&t, r->epoch_launch ? " UTC" : "");
    if (cp->task[0]) {
        tb_pad(&t, 45);
        tb_s(&t, "TASK ");
        tb_s(&t, cp->task);
    }
    dl_end(&t, CD_WHITE);
    dl_begin(&t);
    tb_s(&t, "REASON   ");
    tb_s(&t, r->reason[0] ? r->reason : sb_kind_name(r->kind));
    dl_end(&t, hot);
    dl_begin(&t);
    tb_s(&t, "RESET    RST_STAT ");
    tb_x(&t, r->reg[CR_RST_STAT], 8);
    for (unsigned bit = 0; bit < 32; ++bit)
        if (r->reg[CR_RST_STAT] & (1U << bit) && sb_rst_bit_name(bit)) {
            tb_s(&t, " ");
            tb_s(&t, sb_rst_bit_name(bit));
        }
    dl_end(&t, CD_WHITE);
    dl_begin(&t);
    tb_s(&t, "         INFORM2 ");
    tb_x(&t, r->reg[CR_INFORM2], 8);
    tb_s(&t, "   INFORM3 ");
    tb_x(&t, r->reg[CR_INFORM3], 8);
    {
        const char *i3 = sb_inform3_name(r->reg[CR_INFORM3]);
        tb_s(&t, " ");
        tb_s(&t, i3 ? i3 : "?");
    }
    dl_end(&t, CD_WHITE);
    dl_begin(&t);
    tb_s(&t, "S-BOOT   ");
    tb_s(&t, sb_rr_name((int32_t)r->reg[CR_SB_RR]));
    tb_s(&t, " (reset_reason ");
    tb_i(&t, (int32_t)r->reg[CR_SB_RR]);
    tb_s(&t, ")");
    dl_end(&t, CD_WHITE);
    dl_begin(&t);
    tb_s(&t, "         rst_stat ");
    tb_x(&t, r->reg[CR_SB_RST], 8);
    tb_s(&t, "   pwronsrc ");
    tb_x(&t, r->reg[CR_SB_PWRON], 2);
    for (unsigned b = 0; b < 8; ++b)           /* 2026-11-10: the vendor's names */
        if ((r->reg[CR_SB_PWRON] >> b & 1U) && sb_pwron_bit_name(b)) {
            tb_s(&t, " ");
            tb_s(&t, sb_pwron_bit_name(b));
        }
    tb_s(&t, "   pwroffsrc ");
    tb_x(&t, r->reg[CR_SB_PWROFF], 2);
    for (unsigned b = 0; b < 8; ++b)
        if ((r->reg[CR_SB_PWROFF] >> b & 1U) && sb_pwroff_bit_name(b)) {
            tb_s(&t, " ");
            tb_s(&t, sb_pwroff_bit_name(b));
        }
    if (r->reg[CR_SB_PWROFF])
        tb_s(&t, " (may be stale)");
    dl_end(&t, CD_MUTED);
    dl_begin(&t);
    tb_s(&t, "WATCHDOG ");
    if (r->reg[CR_LAUNCH_WDT]) {
        tb_u(&t, r->reg[CR_LAUNCH_WDT]);
        tb_s(&t, " s, armed at the launch");
    } else {
        tb_s(&t, "not armed at the launch");
    }
    tb_s(&t, "   WDT1 con ");
    tb_x(&t, r->reg[CR_WDT1_CON], 4);
    tb_s(&t, " dat ");
    tb_x(&t, r->reg[CR_WDT1_DAT], 4);
    tb_s(&t, " cnt ");
    tb_x(&t, r->reg[CR_WDT1_CNT], 4);
    dl_end(&t, CD_MUTED);
    dl_begin(&t);
    tb_s(&t, "SOURCE   ");
    tb_s(&t, sb_source_name(r->source));
    tb_s(&t, ", ");
    tb_u(&t, nl);
    tb_s(&t, " lines, ");
    tb_u(&t, sh->n);
    tb_s(&t, sh->live ? " bytes (this boot)" : " bytes (saved)");
    dl_end(&t, CD_MUTED);
    dl_begin(&t);
    tb_s(&t, "STORE    ");
    if (sh->slot >= 0) {
        tb_s(&t, "report #");
        tb_u(&t, r->seq);
        tb_s(&t, " (slot ");
        tb_u(&t, (unsigned)sh->slot + 1U);
        tb_s(&t, ")   on the Mac: sbstore2.py crashlog live");
    } else {
        tb_s(&t, "NOT saved (the crash log could not be written)");
    }
    dl_end(&t, sh->slot >= 0 ? CD_GREEN : CD_RED);

    /* the message */
    doc_section("PANIC MESSAGE", 0);
    if (!doc_lines(sh->text, sh->n, kinds, nk, CT_MSG, 0, 400))
        dl_line(r->reason[0] ? r->reason : "(the OS left no message: see the reset registers)", CD_MUTED);
    if (level >= 2 && cp->info) {
        doc_section("CONTEXT", 0);
        (void)doc_lines(sh->text, sh->n, kinds, nk, CT_INFO, 0, 200);
    }
    if (level >= 2) {
        char rc[24];
        struct tb w = { rc, 0, sizeof(rc) };
        tb_s(&w, "(");
        tb_u(&w, cp->regs);
        tb_s(&w, cp->regs == 1 ? " LINE)" : " LINES)");
        doc_section("REGISTERS", rc);
        if (!doc_lines(sh->text, sh->n, kinds, nk, CT_REGS, 0, 400))
            dl_line("(no register dump: a panic() call prints only its trace)", CD_MUTED);
    }
    {
        char tc[24];
        struct tb w = { tc, 0, sizeof(tc) };
        tb_s(&w, "(");
        tb_u(&w, cp->trace);
        tb_s(&w, cp->trace == 1 ? " LINE)" : " LINES)");
        doc_section("CALL TRACE", tc);
    }
    {
        unsigned max = level >= 2 ? 1000U : 13U;   /* "Call trace:" and 12 frames */
        unsigned got = doc_lines(sh->text, sh->n, kinds, nk, CT_TRACE, 0, max);
        if (!got)
            dl_line("(no call trace in the captured text)", CD_MUTED);
        else if (got == max && cp->trace > max) {
            dl_begin(&t);
            tb_s(&t, "... ");
            tb_u(&t, cp->trace - max);
            tb_s(&t, " more (hold Bixby: more detail)");
            dl_end(&t, CD_MUTED);
        }
    }
    if (level >= 3) {
        doc_section("RESET AND WATCHDOG REGISTERS", 0);
        for (unsigned i = 0; i < 16; ++i) {
            dl_begin(&t);
            tb_s(&t, reg_name(i));
            tb_pad(&t, 34);
            tb_x(&t, r->reg[i], 8);
            if (i == CR_SB_RR) {
                tb_s(&t, "  ");
                tb_s(&t, sb_rr_name((int32_t)r->reg[i]));
            } else if (i == CR_INFORM3 && sb_inform3_name(r->reg[i])) {
                tb_s(&t, "  ");
                tb_s(&t, sb_inform3_name(r->reg[i]));
            }
            dl_end(&t, CD_WHITE);
        }
    }
    {
        /* every level: all of it (the phone, 2026-11-06: a summary capped at 30 lines fitted one
         * page, so Volume had nothing to scroll) */
        char what[48];
        struct tb w = { what, 0, sizeof(what) };
        tb_s(&w, sh->live ? "(ALL " : "(SAVED EXCERPT, ");
        tb_u(&w, nl);
        tb_s(&w, " LINES)");
        doc_section("LOG", what);
        d->log_row = d->n - 2U;
        d->log_lines = nl;
        if (!doc_lines(sh->text, sh->n, kinds, nk, 0xffU, 0, nl))
            dl_line("(no log text: S-Boot's reset code is all there is)", CD_MUTED);
    }
    if (level >= 3 && sh->other && sh->other_n) {
        doc_section("OTHER RING:", sb_source_name(sh->other_src));
        (void)doc_lines(sh->other, sh->other_n, 0, 0, 0xffU, 0, 100000U);
    }
    dl_line("", CD_WHITE);
    dl_begin(&t);
    tb_s(&t, "-- END OF REPORT");
    if (sh->slot >= 0) {
        tb_s(&t, " #");
        tb_u(&t, r->seq);
    }
    tb_s(&t, " ");
    while (t.n < CD_COLS)
        tb_c(&t, '-');
    dl_end(&t, CD_GOLD);
}

/* ---- the keys ---------------------------------------------------------------------------------- */

void cs_init(struct cs_nav *n, unsigned page)
{
    uint8_t *b = (uint8_t *)n;
    for (unsigned i = 0; i < sizeof(*n); ++i)
        b[i] = 0;
    n->page = page ? page : 1U;
}

void cs_doc(struct cs_nav *n, const struct cd_doc *d)
{
    n->rows = d->n;
    n->nsect = d->nsect < CD_SECT_MAX ? d->nsect : CD_SECT_MAX;
    for (unsigned i = 0; i < n->nsect; ++i)
        n->sect[i] = d->sect_row[i];
    if (n->top > cs_max_top(n))
        n->top = cs_max_top(n);
}

unsigned cs_max_top(const struct cs_nav *n)
{
    return n->rows > n->page ? n->rows - n->page : 0U;
}

unsigned cs_page_on(const struct cs_nav *n, unsigned top)
{
    unsigned m = cs_max_top(n);
    if (top >= m)
        return 0;
    unsigned to = top + n->page;
    for (unsigned s = 0; s < n->nsect; ++s)
        if (n->sect[s] > top && n->sect[s] < to)
            to = n->sect[s];
    return to > m ? m : to;
}

unsigned cs_poll(struct cs_nav *n, unsigned keys, uint64_t now_ms)
{
    unsigned ev = 0, old = n->top;
    uint64_t t = now_ms + 1U;           /* 0 means "not down" */
    unsigned vol = keys & (CS_K_UP | CS_K_DOWN);
    int dir = vol == CS_K_UP ? -1 : vol == CS_K_DOWN ? 1 : 0;
    if (dir != n->dir) {
        n->dir = dir;
        n->dir_t = t;
        n->rep_t = t + CS_FIRST_MS;
        n->first = 1;
    }
    if (dir && t >= n->rep_t) {
        unsigned step = t - n->dir_t >= CS_FAST_MS ? CS_FAST_STEP : 1U, m = cs_max_top(n);
        if (dir < 0)
            n->top = n->top > step ? n->top - step : 0U;
        else
            n->top = n->top + step < m ? n->top + step : m;
        n->rep_t = t + (n->first ? CS_REPEAT_MS : CS_RATE_MS);
        n->first = 0;
        ev |= CS_EV_KEY;
    }
    if (keys & CS_K_BIXBY) {
        if (!n->bix_t) {
            n->bix_t = t;
        } else if (!n->bix_long && t - n->bix_t >= CS_LEVEL_MS) {
            n->bix_long = 1;
            n->top = 0;
            ev |= CS_EV_LEVEL | CS_EV_KEY;
        }
    } else {
        if (n->bix_t && !n->bix_long && t - n->bix_t >= CS_FIRST_MS) {
            n->top = cs_page_on(n, n->top);
            ev |= CS_EV_KEY;
        }
        n->bix_t = 0;
        n->bix_long = 0;
    }
    if (keys & CS_K_POWER) {
        if (!n->pow_t)
            n->pow_t = t;
        else if (t - n->pow_t >= CS_FIRST_MS)
            ev |= CS_EV_EXIT | CS_EV_KEY;
    } else {
        n->pow_t = 0;
    }
    if (n->top != old)
        ev |= CS_EV_MOVED;
    return ev;
}
