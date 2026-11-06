/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite POST, the power-on self test (docs/STARBITE-HEALTH.md section 2).
 *
 * Levels (settings record, store.h SET_POST_*; the recovery menu changes it):
 *   full (default)  about 10 s on its own screen: RAM patterns over the free windows (time boxed),
 *                   the timer against the PMIC RTC, CPU clusters, temperatures, PMIC, battery and
 *                   charger, display, touch, USB, UFS, GPT CRCs, BOOT header, OS store, catalog,
 *                   settings record, microSD
 *   quick           about 0.3 s, lines at the top over S-Boot's splash: the checks of 2026-11-05
 *   off             nothing (the menu brings UFS and touch up when it needs them)
 * Volume Down held during a run skips the rest of it, for this boot only. Bixby held during it
 * opens Starbite Recovery afterwards.
 *
 * Every check is bounded: RAM by its time budget, the timer by loop counts, ACPM by acpm_send's
 * 100 ms waits, I2C by batt.c's per-message deadlines, register reads are single reads, UFS has
 * ufs.c's limits (2 s per command), touch menu.c's (150 ms power-up + bounded I2C). Only RAM and
 * timer failures are fatal (Starbite Recovery comes up instead of the menu).
 *
 * Read only, except: the RAM windows below (nothing lives there before an OS is loaded), the
 * DECON frame-done pending bit (write-1-to-clear, the Linux driver's own handshake) and what
 * touch and the battery bus setup already wrote before this change.
 *
 * RAM windows (all Normal non-cacheable in mmu.c, so every access reaches DRAM):
 *   A  0x08C000000 +  64 MiB   STAGE below 0x90000000 (S-Boot's firmware log windows start there)
 *   B  0x092000000 + 128 MiB   the XNU window below this file's panel copy (0x9A000000)
 *   C  0x880000000 +   1 GiB   DRAM bank 2 (no reserved-memory in any DT), as time allows
 * Never 0xA0000000 (S-Boot's ECT, read by the Android kernel), never 0x9A000000..0xA0000000
 * (panel copies, recovery, the crash snapshot), never below 0x8C000000 (the loader, menu assets,
 * S-Boot's FDT).
 *
 * Results: post_last(), the log page (0x91fff000: slot 200 "POST" summary, 201 total us, 240 + id
 * per check: state << 56 | value << 32 | us) and the pmsg ring. No pointer tables.
 */
#include "post.h"
#include "fbtext.h"
#include "hw.h"
#include "ufs.h"
#include "store.h"
#include "catalog.h"
#include "recovery.h"
#include "batt.h"
#include "sd.h"
#include "sbfmt.h"

uint32_t star_crc32(uintptr_t p, uint64_t n);   /* store.c */

#define POST_ROWS_SAVE  0x9E000000UL   /* quick: the rows it draws over */
#define POST_PANEL_SAVE 0x9A000000UL   /* full: the whole panel (16.3 MiB) */
#define POST_RAM_QUICK_BASE 0x8C000000UL
#define POST_RAM_QUICK  (1UL << 20)
#define POST_RAM_MS     6000U          /* full: the RAM test's time budget */
#define POST_LOG        ((volatile uint64_t *)0x91fff000UL)
#define POST_LOG_SLOT   200U
#define POST_LOG_CHECK  240U
#define POST_HOLD_MS    1500U
#define CHUNK           (4UL << 20)

#define C_WHITE 0xffffffffU
#define C_BLACK 0xff000000U
#define C_MUTED 0xffb1bad3U
#define C_GOLD  0xffffd43bU
#define C_RED   0xffff4d4dU
#define C_GREEN 0xff3ddc84U
#define C_TRACK 0xff2a2f3aU

static struct post_report rep;
static int rep_valid;

const struct post_report *post_last(void)
{
    return rep_valid ? &rep : 0;
}

unsigned post_ok_count(const struct post_report *r)
{
    unsigned n = 0;
    for (unsigned i = 0; i < r->n && i < POST_MAX; ++i)
        n += r->c[i].state == POST_OK;
    return n;
}

/* ---- time ----------------------------------------------------------------------------------- */

static uint64_t p_ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static uint64_t p_freq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 26000000U;
}

static uint64_t p_us(uint64_t t0)
{
    return (p_ticks() - t0) * 1000000U / p_freq();
}

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

/* ---- text ----------------------------------------------------------------------------------- */

struct pl { char b[STAR_FB_COLS + 1]; unsigned n; };

static void pl_init(struct pl *l)
{
    l->n = 0;
    l->b[0] = 0;
}

static void pl_s(struct pl *l, const char *s)
{
    while (s && *s && l->n < STAR_FB_COLS)
        l->b[l->n++] = *s++;
    l->b[l->n] = 0;
}

static void pl_u(struct pl *l, uint64_t v)
{
    char d[20];
    unsigned k = 0;
    do {
        d[k++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && k < sizeof(d));
    while (k && l->n < STAR_FB_COLS)
        l->b[l->n++] = d[--k];
    l->b[l->n] = 0;
}

static void pl_i(struct pl *l, int64_t v)
{
    if (v < 0) {
        pl_s(l, "-");
        v = -v;
    }
    pl_u(l, (uint64_t)v);
}

static void pl_hex(struct pl *l, uint64_t v, unsigned digits)
{
    pl_s(l, "0X");
    while (digits-- && l->n < STAR_FB_COLS) {
        unsigned d = (unsigned)(v >> (4 * digits)) & 15U;
        l->b[l->n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    l->b[l->n] = 0;
}

static void pl_pad(struct pl *l, unsigned col)
{
    while (l->n < col && l->n < STAR_FB_COLS)
        l->b[l->n++] = ' ';
    l->b[l->n] = 0;
}

static void str_copy(char *d, unsigned cap, const char *s)
{
    unsigned k = 0;
    while (s && s[k] && k + 1 < cap) {
        d[k] = s[k];
        ++k;
    }
    d[k] = 0;
}

/* ---- the run's layout ------------------------------------------------------------------------ */

/* Display order per mode (ids; plain bytes, no pointers). */
static const uint8_t order_quick[11] = {
    POST_RAM, POST_TIMER, POST_PMIC, POST_UFS, POST_GPT, POST_STORE, POST_CATALOG, POST_BATTERY,
    POST_TEMP, POST_TOUCH, POST_MICROSD,
};
static const uint8_t order_full[POST_N] = {
    POST_RAM, POST_TIMER, POST_CPU, POST_TEMP, POST_PMIC, POST_BATTERY, POST_DISPLAY, POST_TOUCH,
    POST_USB, POST_UFS, POST_GPT, POST_BOOT, POST_STORE, POST_CATALOG, POST_SETTINGS, POST_MICROSD,
};

static unsigned p_mode, p_row0, p_screen, p_count, p_done;
static uint8_t p_pos[POST_N];       /* id -> display row, 0xff = not in this mode */
static int *p_bixby;
static int p_skip;
static uint64_t p_t0;

#define FULL_ROW_TITLE 0U
#define FULL_ROW_BAR   2U
#define FULL_ROW0      4U           /* first check line */
#define FULL_ROW_SUM   (FULL_ROW0 + POST_N + 1U)
#define FULL_ROW_LIST  (FULL_ROW_SUM + 2U)
#define FULL_ROW_HINT  44U

static const char *check_name(unsigned id)
{
    switch (id) {
    case POST_RAM:      return "RAM";
    case POST_TIMER:    return "TIMER";
    case POST_PMIC:     return "PMIC";
    case POST_UFS:      return "UFS";
    case POST_GPT:      return "GPT";
    case POST_STORE:    return "STORE";
    case POST_CATALOG:  return "CATALOG";
    case POST_BATTERY:  return "BATTERY";
    case POST_TEMP:     return "TEMP";
    case POST_TOUCH:    return "TOUCH";
    case POST_CPU:      return "CPU";
    case POST_DISPLAY:  return "DISPLAY";
    case POST_USB:      return "USB";
    case POST_BOOT:     return "BOOT";
    case POST_SETTINGS: return "SETTINGS";
    default:            return "MICROSD";
    }
}

static unsigned check_row(unsigned id)
{
    return p_screen ? FULL_ROW0 + p_pos[id] : p_row0 + 1U + p_pos[id];
}

/* mark, name (8), detail, time "9999MS" right aligned: 38 columns */
static void draw_check(unsigned id)
{
    const struct post_check *c = &rep.c[id];
    if (p_pos[id] == 0xff)
        return;
    struct pl l;
    pl_init(&l);
    pl_s(&l, c->state == POST_OK ? FBT_CHECK : c->state == POST_FAIL ? FBT_CROSS : c->state == POST_WARN ? "!" : "-");
    pl_s(&l, " ");
    pl_s(&l, c->name);
    pl_pad(&l, 11);
    pl_s(&l, c->detail);
    if (c->state != POST_SKIP) {
        uint64_t ms = (c->us + 500U) / 1000U;
        unsigned digits = 1;
        for (uint64_t v = ms; v >= 10; v /= 10)
            ++digits;
        if (ms > 9999)
            ms = 9999, digits = 4;
        if (l.n + 2 + digits + 1 <= STAR_FB_COLS) {
            pl_pad(&l, STAR_FB_COLS - 2 - digits);
            pl_u(&l, ms);
            pl_s(&l, "MS");
        }
    }
    uint32_t fg = c->state == POST_OK ? C_WHITE : c->state == POST_FAIL ? C_RED : c->state == POST_WARN ? C_GOLD : C_MUTED;
    star_fb_text_c(check_row(id), l.b, fg, C_BLACK);
}

/* The progress bar of the full screen: permille done. Plain framebuffer writes. */
static void bar(unsigned pm)
{
    if (!p_screen)
        return;
    if (pm > 1000)
        pm = 1000;
    unsigned y0 = star_fb_row_y(FULL_ROW_BAR) + 18U, x0 = 36U, x1 = STAR_FB_WIDTH - 36U;
    unsigned xf = x0 + (x1 - x0) * pm / 1000U;
    volatile uint32_t *fb = (volatile uint32_t *)STAR_FB_BASE;
    for (unsigned y = y0; y < y0 + 24U && y < STAR_FB_HEIGHT; ++y)
        for (unsigned x = x0; x < x1; ++x)
            fb[y * STAR_FB_WIDTH + x] = x < xf ? (rep.fails ? C_RED : C_GOLD) : C_TRACK;
}

static void progress(unsigned ram_pm)
{
    /* RAM is about 60 % of the time, the other checks share the rest */
    unsigned after = p_count > 1 ? (p_done > 0 ? p_done - 1U : 0U) * 400U / (p_count - 1U) : 0U;
    bar(ram_pm * 600U / 1000U + after);
}

/* Volume Down held (two reads 5 ms apart): skip the rest. Bixby: recovery afterwards. */
static void poll_keys(void)
{
    if (p_bixby && !*p_bixby && rec_bixby_now())
        *p_bixby = 1;
    if (!p_skip && (hw_keys() & K_DOWN)) {
        hw_wait_us(5000);
        if (hw_keys() & K_DOWN) {
            p_skip = 1;
            if (p_screen)
                star_fb_text_c(FULL_ROW_HINT, "SKIPPING THE REST OF POST", C_GOLD, C_BLACK);
        }
    }
}

static void check_begin(unsigned id)
{
    struct post_check *c = &rep.c[id];
    str_copy(c->name, sizeof(c->name), check_name(id));
    c->state = POST_SKIP;
    c->fatal = 0;
    c->value = 0;
    c->us = 0;
    str_copy(c->detail, sizeof(c->detail), "...");
    draw_check(id);
}

/* The check's result; over cap_ms an OK becomes a warning ("SLOW"). */
static void check_end(unsigned id, unsigned state, int32_t value, const char *detail, uint64_t t0, unsigned cap_ms)
{
    struct post_check *c = &rep.c[id];
    uint64_t us = p_us(t0);
    c->us = us > 0xffffffffU ? 0xffffffffU : (uint32_t)us;
    c->state = (uint8_t)state;
    c->value = value;
    str_copy(c->detail, sizeof(c->detail), detail);
    if (state == POST_OK && cap_ms && us > (uint64_t)cap_ms * 1000U) {
        c->state = POST_WARN;
        struct pl l;
        pl_init(&l);
        pl_s(&l, detail);
        pl_s(&l, " SLOW");
        str_copy(c->detail, sizeof(c->detail), l.b);
    }
    if (c->state == POST_FAIL)
        ++rep.fails;
    else if (c->state == POST_WARN)
        ++rep.warns;
    draw_check(id);
    ++p_done;
    progress(1000);
    poll_keys();
}

/* ---- RAM -------------------------------------------------------------------------------------- */

/* Word i's value in pattern k: its address, the address inverted, 0101..., 1010... */
static inline uint64_t ram_word(uintptr_t a, unsigned k)
{
    return k == 0 ? a : k == 1 ? ~(uint64_t)a : k == 2 ? 0x5555555555555555ULL : 0xaaaaaaaaaaaaaaaaULL;
}

static void ram_fill(uintptr_t base, uint64_t n, unsigned k)
{
    volatile uint64_t *p = (volatile uint64_t *)base;
    for (uint64_t i = 0; i < n / 8U; ++i)
        p[i] = ram_word(base + 8U * i, k);
    __asm__ volatile("dsb sy" : : : "memory");
}

/* Eight loads in flight at a time; mismatching words. */
static uint64_t ram_cmp(uintptr_t base, uint64_t n, unsigned k)
{
    volatile uint64_t *p = (volatile uint64_t *)base;
    uint64_t words = n / 8U & ~7ULL, bad = 0;
    for (uint64_t i = 0; i < words; i += 8) {
        uint64_t v[8];
        for (unsigned j = 0; j < 8; ++j)
            v[j] = p[i + j];
        for (unsigned j = 0; j < 8; ++j)
            bad += v[j] != ram_word(base + 8U * (i + j), k);
    }
    return bad;
}

static void check_ram(void)
{
    uint64_t t0 = p_ticks();
    uint64_t bad = 0, tested = 0;
    unsigned passes = 0, windows = 0;
    struct pl l;
    check_begin(POST_RAM);
    if (p_mode != POST_FULL) {
        for (unsigned k = 0; k < 2; ++k) {
            ram_fill(POST_RAM_QUICK_BASE, POST_RAM_QUICK, k);
            bad += ram_cmp(POST_RAM_QUICK_BASE, POST_RAM_QUICK, k);
        }
        tested = POST_RAM_QUICK;
        passes = 2;
    } else {
        static const uint64_t win[3][2] = {
            { 0x8C000000ULL, 64ULL << 20 },
            { 0x92000000ULL, 128ULL << 20 },
            { 0x880000000ULL, 1024ULL << 20 },
        };
        uint64_t budget = p_freq() / 1000U * POST_RAM_MS;
        int out = 0;
        /* address, then inverted address, per window: write it all, then read it all, so a word
         * that aliases another anywhere in the window shows */
        for (unsigned w = 0; w < 3 && !out && !p_skip; ++w) {
            uint64_t done_w = 0;
            for (unsigned k = 0; k < 2 && !out && !p_skip; ++k) {
                uint64_t n = 0;
                for (; n < win[w][1] && !p_skip; n += CHUNK) {
                    if (p_ticks() - t0 > budget) {
                        out = 1;
                        break;
                    }
                    ram_fill(win[w][0] + n, CHUNK, k);
                    progress((unsigned)((p_ticks() - t0) * 1000U / budget));
                    poll_keys();
                }
                uint64_t m = 0;
                for (; m < n && !p_skip; m += CHUNK) {
                    bad += ram_cmp(win[w][0] + m, CHUNK, k);
                    progress((unsigned)((p_ticks() - t0) * 1000U / budget));
                    poll_keys();
                }
                if (k == 1 || out)
                    done_w = m;
                if (!out && !p_skip)
                    ++passes;
            }
            if (done_w) {
                tested += done_w;
                ++windows;
            }
        }
        /* 0101... and 1010... over window A when time is left */
        for (unsigned k = 2; k < 4 && !p_skip && p_ticks() - t0 < budget; ++k)
            for (uint64_t n = 0; n < win[0][1] && !p_skip; n += CHUNK) {
                ram_fill(win[0][0] + n, CHUNK, k);
                bad += ram_cmp(win[0][0] + n, CHUNK, k);
                poll_keys();
            }
        progress(1000);
    }
    pl_init(&l);
    pl_u(&l, tested >> 20);
    pl_s(&l, " MIB ");
    if (bad) {
        pl_u(&l, bad);
        pl_s(&l, " BAD WORDS");
    } else if (p_mode == POST_FULL) {
        pl_u(&l, windows);
        pl_s(&l, windows == 1 ? " WINDOW OK" : " WINDOWS OK");
    } else {
        pl_u(&l, passes);
        pl_s(&l, " PATTERNS OK");
    }
    rep.c[POST_RAM].fatal = bad != 0;
    check_end(POST_RAM, bad ? POST_FAIL : (p_skip && p_mode == POST_FULL) ? POST_WARN : POST_OK,
              (int32_t)(bad > 0x7fffffff ? 0x7fffffff : bad), l.b, t0, p_mode == POST_FULL ? 9000U : 60U);
}

/* ---- timer ------------------------------------------------------------------------------------ */

static void check_timer(void)
{
    uint64_t t0 = p_ticks(), f;
    check_begin(POST_TIMER);
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    uint64_t a = p_ticks(), b = a;
    for (unsigned i = 0; i < 100000 && b == a; ++i)
        b = p_ticks();
    struct pl l;
    pl_init(&l);
    if (b == a) {
        rep.c[POST_TIMER].fatal = 1;
        check_end(POST_TIMER, POST_FAIL, 0, "COUNTER STOPPED", t0, 0);
        return;
    }
    uint64_t hz = p_freq();
    unsigned state = POST_OK;
    if (p_mode == POST_FULL) {
        /* Ticks between two PMIC RTC second edges (each read about 3 ms: +-0.3 %). */
        uint64_t e0 = 0, e = 0, ta = 0, tb = 0;
        int ok = em_rtc_epoch(&e0);
        for (unsigned i = 0; ok && i < 600 && !ta && !p_skip; ++i)
            if (em_rtc_epoch(&e) && e != e0)
                ta = p_ticks(), e0 = e;
        for (unsigned i = 0; ta && i < 600 && !tb && !p_skip; ++i)
            if (em_rtc_epoch(&e) && e != e0)
                tb = p_ticks();
        if (tb) {
            uint64_t m = tb - ta;   /* ticks per RTC second */
            pl_u(&l, m / 1000000U);
            pl_s(&l, ".");
            pl_u(&l, m / 100000U % 10U);
            pl_s(&l, " MHZ BY RTC");
            if (m * 100U < hz * 97U || m * 100U > hz * 103U)
                state = POST_WARN;
            check_end(POST_TIMER, state, (int32_t)(m / 100000U), l.b, t0, 3000U);
            return;
        }
        check_end(POST_TIMER, POST_WARN, (int32_t)(hz / 100000U), p_skip ? "SKIPPED" : "RTC UNREAD", t0, 0);
        return;
    }
    pl_u(&l, hz / 1000000U);
    pl_s(&l, ".");
    pl_u(&l, hz / 100000U % 10U);
    pl_s(&l, f ? " MHZ" : " MHZ (DEFAULT)");
    check_end(POST_TIMER, state, (int32_t)(hz / 100000U), l.b, t0, 20U);
}

/* ---- CPU clusters (read only; em.c fill_clocks) ----------------------------------------------- */

/* PLL_CON0: [31] enable, [29] locked, [25:16] M, [13:8] P, [4] mux (0 = 26 MHz), [2:0] S */
static uint64_t pll_hz(uintptr_t con0, int *unlocked)
{
    uint32_t v = rd(con0);
    if (!(v & 0x10U))
        return 26000000U;
    if (!(v & 0x80000000U))
        return 0;
    if (unlocked && !(v & 0x20000000U))
        *unlocked = 1;
    uint64_t m = (v >> 16) & 0x3ffU, p = (v >> 8) & 0x3fU, s = v & 7U;
    return p ? 26000000ULL * m / (p << s) : 0;
}

static uint64_t top_switch(uintptr_t user, uintptr_t mux, uintptr_t div)
{
    uint64_t src;
    if (!((rd(user) >> 4) & 1U))
        return 26000000U;
    switch (rd(mux) & 3U) {
    case 0: src = pll_hz(0x1a240120UL, 0) / ((rd(0x1a2418d4UL) & 1U) + 1U); break;
    case 1: src = pll_hz(0x1a2401a0UL, 0) / ((rd(0x1a2418e0UL) & 1U) + 1U); break;
    case 2: src = pll_hz(0x1a240220UL, 0); break;
    default: src = pll_hz(0x1a240320UL, 0); break;
    }
    return src / ((rd(div) & 7U) + 1U);
}

static void check_cpu(void)
{
    uint64_t t0 = p_ticks(), midr;
    int ul = 0;
    check_begin(POST_CPU);
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    uint64_t a55src = (rd(0x1d00100cUL) & 1U) ? top_switch(0x1d000120UL, 0x1a241034UL, 0x1a241830UL)
                                              : pll_hz(0x1d000140UL, &ul);
    uint64_t a55 = a55src / ((rd(0x1d001818UL) & 0xfffU) + 1U);
    struct pl l;
    pl_init(&l);
    pl_s(&l, ((midr >> 4) & 0xfffU) == 0xd05U ? "A55 " : "CPU? ");
    pl_u(&l, a55 / 1000000U);
    pl_s(&l, " M3 ");
    if ((rd(0x14062044UL) & 0xfU) == 0xfU) {
        uint64_t src = (rd(0x1d101000UL) & 1U) ? top_switch(0x1d100100UL, 0x1a241038UL, 0x1a241834UL)
                                               : pll_hz(0x1d100120UL, &ul);
        pl_u(&l, src / ((rd(0x1d10180cUL) & 0xfffU) + 1U) / 1000000U);
        pl_s(&l, " MHZ");
    } else {
        pl_s(&l, "OFF");
    }
    check_end(POST_CPU, ul || !a55 ? POST_WARN : POST_OK, (int32_t)(a55 / 1000000U), l.b, t0, 5U);
}

/* ---- PMIC through ACPM ------------------------------------------------------------------------ */

static void check_pmic(void)
{
    uint64_t t0 = p_ticks();
    char build[25];
    uint8_t id = 0;
    check_begin(POST_PMIC);
    if (!hw_acpm_build(build)) {
        check_end(POST_PMIC, POST_FAIL, 0, "NO ACPM PMIC CHANNEL", t0, 0);
        return;
    }
    if (!hw_pmic_read(0, 0x00, &id)) {
        check_end(POST_PMIC, POST_FAIL, 0, "ACPM: NO ANSWER", t0, 0);
        return;
    }
    struct pl l;
    pl_init(&l);
    pl_s(&l, "S2MPS18 ID ");
    pl_hex(&l, id, 2);
    check_end(POST_PMIC, POST_OK, id, l.b, t0, 250U);
}

/* ---- UFS, GPT, BOOT, store, catalog, settings ------------------------------------------------- */

static uint32_t b32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t b64(const uint8_t *p)
{
    return (uint64_t)b32(p) | (uint64_t)b32(p + 4) << 32;
}

static void buf_copy(uint8_t *d, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        d[i] = *(volatile uint8_t *)(UFS_BUF + i);
}

/* A GPT header at lba: 1 header and entry CRCs good, 0 bad, -1 unreadable. *alt its other copy. */
static int gpt_header_ok(uint64_t lba, uint64_t *alt)
{
    uint8_t h[512];
    if (ufs_read(lba, 1))
        return -1;
    buf_copy(h, sizeof(h));
    uint32_t hs = b32(h + 12), want = b32(h + 16), ne = b32(h + 80), es = b32(h + 84), ecrc = b32(h + 88);
    if (b64(h) != 0x5452415020494645ULL || hs < 92 || hs > sizeof(h))   /* "EFI PART" */
        return 0;
    h[16] = h[17] = h[18] = h[19] = 0;
    if (sb_crc32(h, hs) != want)
        return 0;
    *alt = b64(h + 32);
    uint64_t elba = b64(h + 72), bytes = (uint64_t)ne * es;
    if (!bytes || bytes > 256U * 4096U)
        return 0;
    if (ufs_read(elba, (uint32_t)((bytes + 4095U) / 4096U)))
        return -1;
    return star_crc32(UFS_BUF, bytes) == ecrc;
}

static void check_storage(void)
{
    struct pl l;
    uint64_t t0 = p_ticks();
    check_begin(POST_UFS);
    int up = ufs_up();
    if (!up) {
        const char *w = ufs_why == 1 ? "HOST NOT READY" : ufs_why == 2 ? "HOST BUSY" : ufs_why == 3 ? "LIST REFUSED" : "DEVICE NO ANSWER";
        check_end(POST_UFS, POST_FAIL, ufs_why, w, t0, 0);
    } else {
        check_end(POST_UFS, POST_OK, 0, "READY", t0, 2000U);
    }

    t0 = p_ticks();
    check_begin(POST_GPT);
    uint64_t first, last;
    int n = up && !p_skip ? gpt_load() : 0;
    pl_init(&l);
    if (!up || p_skip) {
        check_end(POST_GPT, POST_SKIP, 0, p_skip ? "SKIPPED" : "NO UFS", t0, 0);
    } else if (n <= 0) {
        check_end(POST_GPT, POST_FAIL, n, "UNREADABLE", t0, 0);
    } else {
        int ud = gpt_find("USERDATA", &first, &last);
        unsigned st = ud ? POST_OK : POST_FAIL;
        pl_u(&l, (uint64_t)n);
        pl_s(&l, ud ? " PARTS" : " NO USERDATA");
        if (p_mode == POST_FULL && ud) {
            uint64_t alt = 0, alt2 = 0;
            int a = gpt_header_ok(1, &alt);
            int b = a > 0 && alt > 1 ? gpt_header_ok(alt, &alt2) : -1;
            pl_s(&l, a > 0 && b > 0 ? " CRC OK" : a > 0 ? " BACKUP BAD" : " CRC BAD");
            if (a <= 0)
                st = POST_FAIL;
            else if (b <= 0)
                st = POST_WARN;
        }
        check_end(POST_GPT, st, n, l.b, t0, 1500U);
    }

    if (p_pos[POST_BOOT] != 0xff) {
        t0 = p_ticks();
        check_begin(POST_BOOT);
        if (!up || n <= 0 || p_skip || !gpt_find("BOOT", &first, &last)) {
            check_end(POST_BOOT, POST_SKIP, 0, p_skip ? "SKIPPED" : "NO BOOT PARTITION", t0, 0);
        } else if (ufs_read(first, 1)) {
            check_end(POST_BOOT, POST_FAIL, 0, "UNREADABLE", t0, 0);
        } else {
            uint8_t h[48];
            buf_copy(h, sizeof(h));
            uint32_t ksz = b32(h + 8), rsz = b32(h + 16), ssz = b32(h + 24), page = b32(h + 36), dsz = b32(h + 40);
            uint32_t karm = *(volatile uint32_t *)(UFS_BUF + 2048U + 56U);
            if (b64(h) != 0x2144494f52444e41ULL || page != 2048U) {   /* "ANDROID!" */
                check_end(POST_BOOT, POST_FAIL, 0, "NO ANDROID! HEADER", t0, 0);
            } else if (karm != 0x644d5241U) {
                check_end(POST_BOOT, POST_FAIL, 0, "KERNEL NOT ARM64", t0, 0);
            } else {
                uint64_t al = 2048U, off = al + (ksz + al - 1) / al * al + (rsz + al - 1) / al * al + (ssz + al - 1) / al * al;
                uint32_t dt = 0;
                if (dsz && !ufs_read(first + off / 4096U, 1))
                    dt = *(volatile uint32_t *)(UFS_BUF + off % 4096U);
                pl_init(&l);
                pl_s(&l, "KERNEL ");
                pl_u(&l, ksz >> 20);
                pl_s(&l, " MB");
                pl_s(&l, dt == 0x48425444U ? " DTBH" : " NO DTBH");
                check_end(POST_BOOT, dt == 0x48425444U ? POST_OK : POST_WARN, (int32_t)(ksz >> 20), l.b, t0, 500U);
            }
        }
    }

    t0 = p_ticks();
    check_begin(POST_STORE);
    int cnt = up ? store_open(STORE_UFS) : 0, ver = up ? store_version(STORE_UFS) : 0;
    pl_init(&l);
    if (!up) {
        check_end(POST_STORE, POST_SKIP, 0, "NO UFS", t0, 0);
    } else if (!ver) {
        check_end(POST_STORE, POST_FAIL, 0, "NO STORE OR BAD CRC", t0, 0);
    } else {
        struct store_ent ce;
        int crash = p_mode == POST_FULL && store_crashlog(&ce);
        pl_s(&l, "V");
        pl_u(&l, (uint64_t)ver);
        pl_s(&l, " ");
        pl_u(&l, (uint64_t)cnt);
        pl_s(&l, p_mode == POST_FULL ? (crash ? " CRASHLOG" : " NO CRASHLOG") : " ENTRIES");
        check_end(POST_STORE, !cnt || (p_mode == POST_FULL && !crash) ? POST_WARN : POST_OK, cnt, l.b, t0, 1000U);
    }

    t0 = p_ticks();
    check_begin(POST_CATALOG);
    int ce = cat_load();
    struct store_ent e;
    pl_init(&l);
    pl_u(&l, (uint64_t)ce);
    if (!cat_builtin()) {
        unsigned missing = 0;
        if (p_mode == POST_FULL)
            for (unsigned i = 0; i < cat_count(); ++i) {
                const struct cat_ent *c = cat_get(i);
                if (c && !(c->flags & CF_HIDDEN) && !star_entry_ready(i, 0))
                    ++missing;
            }
        if (missing) {
            pl_s(&l, " CRC OK, ");
            pl_u(&l, missing);
            pl_s(&l, " NOT READY");
        } else {
            pl_s(&l, " ENTRIES CRC OK");
        }
        check_end(POST_CATALOG, missing ? POST_WARN : POST_OK, ce, l.b, t0, 1000U);
    } else if (up && ver && store_find("catalog", &e)) {
        check_end(POST_CATALOG, POST_FAIL, ce, "BAD CRC: BUILT-IN", t0, 0);
    } else {
        check_end(POST_CATALOG, POST_WARN, ce, "NONE: BUILT-IN", t0, 0);
    }

    if (p_pos[POST_SETTINGS] != 0xff) {
        t0 = p_ticks();
        check_begin(POST_SETTINGS);
        struct star_settings s;
        int ok = up && ver ? settings_load(&s) : 0;
        int st = settings_status();
        if (!ok)
            sbset_defaults(&s);
        pl_init(&l);
        pl_s(&l, ok ? "OK " : st == 0 ? "NONE " : st == -1 ? "BAD CRC " : "NO STORE ");
        pl_s(&l, "POST ");
        pl_s(&l, sbset_post_name(sbset_post(&s)));
        pl_s(&l, " LOG ");
        pl_u(&l, sbset_log(&s));
        check_end(POST_SETTINGS, st == -1 || st == -2 ? POST_WARN : POST_OK, st, l.b, t0, 200U);
    }
}

/* ---- battery and charger: MAX77705 through batt.c (read only) --------------------------------- */

static void check_battery(void)
{
    uint64_t t0 = p_ticks();
    struct batt_info b;
    check_begin(POST_BATTERY);
    if (!batt_init()) {
        check_end(POST_BATTERY, POST_FAIL, -1, "MAX77705 NO ANSWER", t0, 0);
        return;
    }
    if (!batt_read(&b) || !b.valid) {
        check_end(POST_BATTERY, POST_FAIL, -1, "FUEL GAUGE UNREAD", t0, 0);
        return;
    }
    struct pl l;
    pl_init(&l);
    pl_u(&l, b.pct);
    pl_s(&l, " %");
    if (p_mode == POST_FULL) {
        if (b.mv) {
            pl_s(&l, " ");
            pl_u(&l, b.mv);
            pl_s(&l, "MV");
        }
        pl_s(&l, " ");
        pl_i(&l, b.ma);
        pl_s(&l, "MA");
        pl_s(&l, b.charging ? " CHG" : b.plugged ? " CABLE" : "");
    }
    unsigned st = b.pct > 100 || b.pct < 10 ? POST_WARN : POST_OK;
    if (p_mode == POST_FULL && b.mv && (b.mv < 3300 || b.mv > 4450))
        st = POST_WARN;
    check_end(POST_BATTERY, st, (int32_t)b.pct, l.b, t0, p_mode == POST_FULL ? 120U : 40U);
}

/* ---- TMU (em.c fill_thermal), read only ------------------------------------------------------- */
#define TMU_TOP 0x10070000UL
#define TMU_SUB 0x10074000UL

static int tmu_temp(uintptr_t base, unsigned p)
{
    uint32_t tri = rd(base + 4U * p);
    int te1 = (int)(tri & 0x1ffU), te2 = (int)((tri >> 9) & 0x1ffU);
    int two = (int)((rd(base) >> 23) & 1U);
    if (!te1)
        te1 = 55;
    uintptr_t off = p < 2 ? 0x40U : 0x44U + 4U * ((p - 2U) / 3U);
    unsigned sh = p < 2 ? 9U * p : 9U * ((p - 2U) % 3U);
    int code = (int)((rd(base + off) >> sh) & 0x1ffU);
    return (two && te2 > te1) ? (code - te1) * 60 / (te2 - te1) + 25 : code - te1 + 25;
}

static int tmu_on(uintptr_t base)
{
    return (rd(base + 0x20) & 1U) != 0;
}

static void check_temp(void)
{
    uint64_t t0 = p_ticks();
    check_begin(POST_TEMP);
    int top = tmu_on(TMU_TOP), sub = tmu_on(TMU_SUB), cpu = -100, gpu = -100, little = -100;
    if (top) {
        static const unsigned char probes[6] = { 1, 2, 3, 4, 6, 5 };   /* big (DT mask 0x5E), little */
        for (unsigned i = 0; i < (p_mode == POST_FULL ? 5U : 2U); ++i) {
            int t = tmu_temp(TMU_TOP, probes[i]);
            if (t > cpu)
                cpu = t;
        }
        if (p_mode == POST_FULL)
            little = tmu_temp(TMU_TOP, probes[5]);
    }
    if (sub) {
        gpu = tmu_temp(TMU_SUB, 1);
        if (p_mode == POST_FULL) {
            int isp = tmu_temp(TMU_SUB, 2);
            if (isp > gpu)
                gpu = isp;
        }
    }
    if (!top && !sub) {
        check_end(POST_TEMP, POST_WARN, 0, "TMU OFF", t0, 0);
        return;
    }
    int hot = cpu > gpu ? cpu : gpu;
    if (little > hot)
        hot = little;
    struct pl l;
    pl_init(&l);
    if (top) {
        pl_s(&l, p_mode == POST_FULL ? "BIG " : "CPU ");
        pl_i(&l, cpu);
        pl_s(&l, "C ");
        if (p_mode == POST_FULL) {
            pl_s(&l, "LIT ");
            pl_i(&l, little);
            pl_s(&l, "C ");
        }
    }
    if (sub) {
        pl_s(&l, "GPU ");
        pl_i(&l, gpu);
        pl_s(&l, "C");
    }
    unsigned st = (hot >= 90 || hot < -20) ? POST_FAIL : hot >= 70 ? POST_WARN : POST_OK;
    check_end(POST_TEMP, st, hot, l.b, t0, 5U);
}

/* ---- display: DECON0 0x16030000 (read; the frame-done pending bit is write-1-to-clear) -------- */

static void check_display(void)
{
    uint64_t t0 = p_ticks();
    check_begin(POST_DISPLAY);
    uint32_t trig = rd(0x16030070UL);
    uint32_t base = rd(0x16071000UL + 0x40U), size = rd(0x16071000UL + 0x10U);
    /* the panel's TE drives the hardware trigger: frames keep completing while it answers */
    *(volatile uint32_t *)0x1603004cUL = 1U << 13;
    __asm__ volatile("dsb sy" : : : "memory");
    int seen = 0;
    for (unsigned i = 0; i < 60 && !seen; ++i) {
        hw_wait_us(1000);
        seen = (rd(0x1603004cUL) >> 13) & 1U;
    }
    struct pl l;
    pl_init(&l);
    pl_u(&l, size & 0xffffU);
    pl_s(&l, "X");
    pl_u(&l, size >> 16);
    pl_s(&l, seen ? " FRAMES OK" : " NO FRAME SEEN");
    unsigned st = seen && base == STAR_FB_BASE && (trig & 1U) ? POST_OK : POST_WARN;
    if (base != STAR_FB_BASE) {
        pl_init(&l);
        pl_s(&l, "SCANOUT AT ");
        pl_hex(&l, base, 8);
    }
    check_end(POST_DISPLAY, st, seen, l.b, t0, 100U);
}

/* ---- touch, USB, microSD ---------------------------------------------------------------------- */

static void check_touch(int touch)
{
    uint64_t t0 = p_ticks();
    check_begin(POST_TOUCH);
    if (!touch || p_skip) {
        check_end(POST_TOUCH, POST_SKIP, 0, p_skip ? "SKIPPED" : "NOT STARTED", t0, 0);
        return;
    }
    hw_tp_start();
    if (hw_tp_ok())
        check_end(POST_TOUCH, POST_OK, 1, "S6SY761 READY", t0, 600U);
    else
        check_end(POST_TOUCH, POST_FAIL, 0, "S6SY761 NO ANSWER", t0, 0);
}

/* The USB PHY's PMU isolation (0x1406072c bit 0: 1 = powered, set by the loader's fastboot or an
 * OS) and the charger's view of the cable (CHGIN). Nothing is written: the PHY's clocks may be
 * off here, so its own registers are not read. */
static void check_usb(void)
{
    uint64_t t0 = p_ticks();
    struct batt_info b;
    check_begin(POST_USB);
    uint32_t phy = rd(0x1406072cUL);
    int bi = batt_read(&b) && b.valid;
    struct pl l;
    pl_init(&l);
    pl_s(&l, !bi ? "CABLE ?" : b.plugged ? "CABLE IN" : "NO CABLE");
    pl_s(&l, phy & 1U ? " PHY ON" : " PHY ISOLATED");
    check_end(POST_USB, POST_OK, (int32_t)(phy & 1U), l.b, t0, 50U);
}

static void check_microsd(void)
{
    uint64_t t0 = p_ticks();
    check_begin(POST_MICROSD);
    int present = !(rd(0x14050044UL) & (1U << 5));          /* card detect GPA1-5, active low */
    if (p_mode != POST_FULL || !present || p_skip) {
        check_end(POST_MICROSD, POST_OK, present, present ? "CARD PRESENT" : "NO CARD", t0, 2U);
        return;
    }
    /* the card store's header: only where the loader may touch the card (sd.h) */
    struct star_settings s;
    if (!sd_allowed() && settings_load(&s) && (s.flags & SET_SD_STORE))
        sd_allow();
    if (!sd_allowed()) {
        check_end(POST_MICROSD, POST_OK, present, "PRESENT, CARD STORE OFF", t0, 0);
        return;
    }
    int n = store_open(STORE_SD), v = store_version(STORE_SD);
    struct pl l;
    pl_init(&l);
    pl_s(&l, "CARD STORE ");
    if (v) {
        pl_s(&l, "V");
        pl_u(&l, (uint64_t)v);
        pl_s(&l, " ");
        pl_u(&l, (uint64_t)n);
    } else {
        pl_s(&l, "MISSING");
    }
    check_end(POST_MICROSD, v ? POST_OK : POST_WARN, n, l.b, t0, 3000U);
}

/* ---- logs ------------------------------------------------------------------------------------- */

static void post_log(void)
{
    volatile uint64_t *log = POST_LOG;
    log[POST_LOG_SLOT] = (uint64_t)POST_MAGIC << 32 | (uint64_t)rep.mode << 24 | (uint64_t)rep.n << 16 |
                         (uint64_t)(rep.fails & 0xffU) << 8 | (rep.warns & 0xffU);
    log[POST_LOG_SLOT + 1] = rep.total_us;
    for (unsigned i = 0; i < rep.n; ++i) {
        const struct post_check *c = &rep.c[i];
        log[POST_LOG_CHECK + i] = (uint64_t)c->state << 56 | (uint64_t)((uint32_t)c->value & 0xffffffU) << 32 | c->us;
    }
    struct pl l;
    pl_init(&l);
    pl_s(&l, "STARBITE: POST ");
    pl_s(&l, rep.mode == POST_FULL ? "full " : "quick ");
    pl_u(&l, rep.total_us / 1000U);
    pl_s(&l, "ms fail ");
    pl_u(&l, rep.fails);
    pl_s(&l, " warn ");
    pl_u(&l, rep.warns);
    if (rep.skipped)
        pl_s(&l, " (skipped)");
    star_pmsg(l.b);
    star_pmsg("\n");
    for (unsigned i = 0; i < rep.n; ++i) {
        const struct post_check *c = &rep.c[i];
        if (c->state != POST_FAIL && c->state != POST_WARN)
            continue;
        pl_init(&l);
        pl_s(&l, c->state == POST_FAIL ? "STARBITE: POST fail " : "STARBITE: POST warn ");
        pl_s(&l, c->name);
        star_pmsg(l.b);
        star_pmsg(": ");
        star_pmsg(c->detail);
        star_pmsg("\n");
    }
}

/* ---- entry points ----------------------------------------------------------------------------- */

static void skip_rest(unsigned id)
{
    uint64_t t = p_ticks();
    if (p_pos[id] == 0xff || rep.c[id].state != POST_SKIP || rep.c[id].detail[0] != '.')
        return;
    check_begin(id);
    check_end(id, POST_SKIP, 0, "SKIPPED", t, 0);
}

static void run_checks(int touch)
{
    unsigned full = p_mode == POST_FULL;
    check_ram();
    check_timer();
    if (rep.c[POST_RAM].fatal || rep.c[POST_TIMER].fatal || p_skip) {
        for (unsigned id = 0; id < POST_N; ++id)
            skip_rest(id);
        return;
    }
    if (full) {
        check_cpu();
        if (!p_skip) check_temp();
        if (!p_skip) check_pmic();
        if (!p_skip) check_battery();
        if (!p_skip) check_display();
        check_touch(touch);
        if (!p_skip) check_usb();
        check_storage();          /* UFS, GPT, BOOT, STORE, CATALOG, SETTINGS: also when skipping */
        check_microsd();
    } else {
        if (!p_skip) check_pmic();
        check_storage();
        if (!p_skip) check_battery();
        if (!p_skip) check_temp();
        check_touch(touch);
        check_microsd();
    }
    for (unsigned id = 0; id < POST_N; ++id)
        skip_rest(id);
}

static void layout(unsigned mode)
{
    for (unsigned i = 0; i < POST_N; ++i)
        p_pos[i] = 0xff;
    if (mode == POST_FULL) {
        for (unsigned i = 0; i < POST_N; ++i)
            p_pos[order_full[i]] = (uint8_t)i;
        p_count = POST_N;
    } else {
        for (unsigned i = 0; i < sizeof(order_quick); ++i)
            p_pos[order_quick[i]] = (uint8_t)i;
        p_count = sizeof(order_quick);
    }
    for (unsigned id = 0; id < POST_N; ++id) {
        struct post_check *c = &rep.c[id];
        str_copy(c->name, sizeof(c->name), check_name(id));
        c->state = POST_SKIP;
        c->fatal = 0;
        c->value = 0;
        c->us = 0;
        str_copy(c->detail, sizeof(c->detail), p_pos[id] == 0xff ? "NOT IN QUICK POST" : "...");
    }
}

static void summary_line(unsigned row)
{
    struct pl l;
    pl_init(&l);
    pl_u(&l, post_ok_count(&rep));
    pl_s(&l, " OK  ");
    pl_u(&l, rep.warns);
    pl_s(&l, " WARN  ");
    pl_u(&l, rep.fails);
    pl_s(&l, " FAILED  ");
    pl_u(&l, rep.total_us / 1000U);
    pl_s(&l, " MS");
    star_fb_text_c(row, l.b, rep.fails ? C_RED : rep.warns ? C_GOLD : C_GREEN, C_BLACK);
}

/* The full screen's list of what went wrong, failures first. */
static void problem_list(void)
{
    unsigned r = FULL_ROW_LIST;
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = 0; i < POST_N && r < FULL_ROW_HINT - 1U; ++i) {
            unsigned id = order_full[i];
            const struct post_check *c = &rep.c[id];
            if (c->state != (pass ? POST_WARN : POST_FAIL))
                continue;
            struct pl l;
            pl_init(&l);
            pl_s(&l, pass ? "WARN " : "FAIL ");
            pl_s(&l, c->name);
            pl_pad(&l, 14);
            pl_s(&l, c->detail);
            star_fb_text_c(r++, l.b, pass ? C_GOLD : C_RED, C_BLACK);
        }
}

static int run(unsigned mode, unsigned row0, int screen, int touch, int *bixby)
{
    p_t0 = p_ticks();
    p_mode = mode;
    p_row0 = row0;
    p_screen = (unsigned)screen;
    p_bixby = bixby;
    p_skip = 0;
    p_done = 0;
    rep.magic = 0;
    rep.mode = mode;
    rep.n = POST_N;
    rep.fails = rep.warns = rep.fatal = rep.skipped = 0;
    layout(mode);
    if (screen) {
        star_fb_fill(C_BLACK);
        star_fb_text_c(FULL_ROW_TITLE, "STARBITE POST", C_GOLD, C_BLACK);
        star_fb_text_c(FULL_ROW_TITLE + 1U, "SELF TEST OF THE WHOLE PHONE", C_MUTED, C_BLACK);
        star_fb_text_c(FULL_ROW_HINT, "VOL DOWN: SKIP     BIXBY: RECOVERY", C_MUTED, C_BLACK);
        bar(0);
        for (unsigned id = 0; id < POST_N; ++id)
            draw_check(id);
    } else {
        star_fb_text_c(row0, mode == POST_FULL ? "STARBITE POST (FULL)" : "STARBITE POST", C_GOLD, C_BLACK);
    }
    run_checks(touch);
    rep.skipped = (uint32_t)p_skip;
    rep.fatal = rep.c[POST_RAM].fatal || rep.c[POST_TIMER].fatal;
    rep.total_us = p_us(p_t0);
    if (screen) {
        bar(1000);
        summary_line(FULL_ROW_SUM);
        problem_list();
    } else {
        summary_line(row0 + 1U + p_count);
    }
    rep.magic = POST_MAGIC;
    rep_valid = 1;
    post_log();
    return (int)rep.fatal;
}

int post_run(unsigned mode, unsigned row0, int touch, int *bixby)
{
    int f = run(mode, row0, 0, touch, bixby);
    p_bixby = 0;
    return f;
}

/* A pause to read the result: Bixby still enters recovery; Volume Down or Power cut it short. */
static void hold(unsigned ms, int *bixby)
{
    uint64_t t0 = p_ticks(), n = p_freq() / 1000U * ms;
    for (unsigned g = 0; g < ms + 100U && p_ticks() - t0 < n; ++g) {
        if (bixby && !*bixby && rec_bixby_now())
            *bixby = 1;
        if (hw_keys() & (K_DOWN | K_POWER))
            break;
        hw_wait_us(1000);
    }
}

int post_boot(unsigned level, int touch, int *bixby, int *skipped, const char **why)
{
    if (skipped)
        *skipped = 0;
    if (why)
        *why = 0;
    if (level == SET_POST_OFF) {
        star_pmsg("STARBITE: POST off\n");
        return 0;
    }
    unsigned base = rec_row_base();
    star_fb_row_base(0);
    int fatal;
    if (level == SET_POST_FULL) {
        rec_rows_save(0, STAR_FB_HEIGHT, POST_PANEL_SAVE);
        fatal = run(POST_FULL, 0, 1, touch, bixby);
        if (!fatal)
            hold(rep.fails ? 3000U : rep.warns ? 1500U : 500U, bixby);
        rec_rows_restore(0, STAR_FB_HEIGHT, POST_PANEL_SAVE);
    } else {
        unsigned y0 = star_fb_row_y(0), y1 = star_fb_row_y(sizeof(order_quick) + 2U);
        rec_rows_save(y0, y1, POST_ROWS_SAVE);
        fatal = run(POST_QUICK, 0, 0, touch, bixby);
        if (rep.fails && !fatal)
            hold(POST_HOLD_MS, bixby);
        rec_rows_restore(y0, y1, POST_ROWS_SAVE);
    }
    p_bixby = 0;
    star_fb_row_base(base);
    if (skipped)
        *skipped = (int)rep.skipped;
    if (why)
        *why = rep.c[POST_RAM].fatal ? "POST: RAM PATTERN FAILED" : rep.c[POST_TIMER].fatal ? "POST: TIMER STOPPED" : 0;
    return fatal;
}
