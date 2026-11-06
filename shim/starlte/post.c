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
 * What the full POST shows (2026-11-07, the user: "normally just starbite logo with some little
 * animation", Volume Up for the live log): the Starbite logo ("One mark", post_logo.h) exactly
 * where S-Boot draws its splash, the star breathing a soft glow and three small dots pulsing under
 * the word; no text, no bar. A Volume Up press that starts during POST switches to the live log
 * (every check with its state, the RAM test's own MiB and percent) for the rest of the run; Volume
 * Up held from power on is fastboot's gesture and is left alone (sbfmt.c pv_*). Log level 2-3
 * starts in the log. A red check ends in a short failure screen (Power continues); gold ones are
 * only logged. At the end the clean logo stays on the panel for menu v7's intro, which hands it
 * over like S-Boot's One mark splash (POST_F_KEEP_LOGO); otherwise the saved panel comes back.
 * Quick POST draws nothing over S-Boot's splash unless Volume Up or log level 2-3 asks for it.
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
 * Results: post_last(), the log page (0x91fff000: slot 200 "POST" summary, 201 total us, 202 + id
 * for checks 0..10 and 240 + id - 11 for 11..15: state << 56 | value << 32 | us) and the pmsg ring. No pointer tables.
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
#include "post_logo.h"

uint32_t star_crc32(uintptr_t p, uint64_t n);   /* store.c */

#define POST_ROWS_SAVE  0x9E000000UL   /* quick: the rows it draws over */
#define POST_PANEL_SAVE 0x9A000000UL   /* full: the whole panel (16.3 MiB, to 0x9B042000) */
#define POST_LOGO_BUF   0x9B100000UL   /* the logo unpacked: word A8 here, star RGB at +64 KiB */
#define POST_RAM_QUICK_BASE 0x8C000000UL
#define POST_RAM_QUICK  (1UL << 20)
#define POST_RAM_MS     6000U          /* full: the RAM test's time budget, reads included */
#define POST_LOG        ((volatile uint64_t *)0x91fff000UL)
#define POST_LOG_SLOT   200U
#define POST_LOG_CHECK  202U            /* checks 0..10, as before 2026-11-06 (SBMAINT decodes them) */
#define POST_LOG_CHECK2 240U            /* checks 11..15: 202 + 11.. would reach recovery's slot 216 */
#define POST_LOG_VIEW   245U            /* how it was shown (POSTV_*) | RAM ms << 32 */
#define CHUNK           (4UL << 20)

/* log slot 245 bits */
#define POSTV_LOG_KEY   1U              /* Volume Up opened the log */
#define POSTV_LOG_SET   2U              /* the log level started it in the log */
#define POSTV_FAILSCR   4U              /* the failure screen was shown */
#define POSTV_KEPT      8U              /* the logo stayed on the panel for the menu */
#define POSTV_SBOOT     16U             /* S-Boot's splash already was the logo (no fade) */
#define POSTV_UP_START  32U             /* Volume Up was held at the start (fastboot's gesture) */
#define POSTV_SKIPPED   64U

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
static struct pv_keys p_k;          /* the view and the keys (sbfmt.c, host-tested) */
static unsigned p_vflags;           /* POSTV_* */
static uint64_t p_ram_ms;

#define FULL_ROW_TITLE 0U
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
    if (p_pos[id] == 0xff || p_k.view != PV_LOG)
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

static uint64_t p_ms(void)
{
    return p_us(p_t0) / 1000U;
}

/* ---- the logo (post_logo.h, the One mark splash) and its animation ---------------------------- */

#define LW_A8   ((volatile uint8_t *)POST_LOGO_BUF)
#define LS_RGB  ((volatile uint8_t *)(POST_LOGO_BUF + 0x10000UL))
#define FB      ((volatile uint32_t *)STAR_FB_BASE)
#define GLOW_R  84                  /* the glow's radius around the star's centre, px */
#define GLOW_X0 ((PL_STAR_CX16 >> 4) - GLOW_R)
#define GLOW_Y0 ((PL_STAR_CY16 >> 4) - GLOW_R)
#define GLOW_N  (2 * GLOW_R + 1)
#define DOT_R16 112                 /* dots: radius 7 px (1/16 px) */
#define DOT_BOX 16
#define DOT_GAP 34
#define DOT_CX  (PL_WORD_X + PL_WORD_W / 2)
#define DOT_CY  (PL_WORD_Y + PL_WORD_H + 120)
#define FADE_MS 360U
#define FRAME_MS 33U

static int logo_ok;                 /* unpacked */
static unsigned logo_fade;          /* 1: fade in from black; 0: it is already on the panel */
static uint64_t logo_t0, logo_last;
static uint8_t dot_cov[DOT_BOX * DOT_BOX];   /* 0..16 */

static unsigned unpack(volatile uint8_t *dst, unsigned size, const uint8_t *src, unsigned n)
{
    unsigned i = 0, o = 0;
    while (i < n && o < size) {
        unsigned c = src[i++];
        if (c < 128) {
            for (unsigned k = 0; k <= c && i < n && o < size; ++k)
                dst[o++] = src[i++];
        } else if (i < n) {
            uint8_t v = src[i++];
            for (unsigned k = 0; k < c - 127U && o < size; ++k)
                dst[o++] = v;
        }
    }
    return o;
}

static void logo_unpack(void)
{
    if (logo_ok)
        return;
    logo_ok = unpack(LW_A8, PL_WORD_W * PL_WORD_H, pl_word, sizeof(pl_word)) == PL_WORD_W * PL_WORD_H &&
              unpack(LS_RGB, PL_STAR_W * PL_STAR_H * 3U, pl_star, sizeof(pl_star)) == PL_STAR_W * PL_STAR_H * 3U;
    /* the dots' coverage: 4 x 4 samples per pixel */
    for (int y = 0; y < DOT_BOX; ++y)
        for (int x = 0; x < DOT_BOX; ++x) {
            unsigned c = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    int dx = (x * 16 + sx * 4 + 2) - DOT_BOX * 8, dy = (y * 16 + sy * 4 + 2) - DOT_BOX * 8;
                    c += dx * dx + dy * dy <= DOT_R16 * DOT_R16;
                }
            dot_cov[y * DOT_BOX + x] = (uint8_t)c;
        }
}

/* 0..4096: a smooth rise and fall over one period (smoothstep of a triangle) */
static unsigned wave(uint64_t t, unsigned period)
{
    unsigned p = (unsigned)(t % period), half = period / 2U;
    unsigned tri = (p < half ? p : period - p) * 4096U / half;
    return (unsigned)((uint64_t)tri * tri / 4096U * (3U * 4096U - 2U * tri) / 4096U);
}

/* The static logo's pixel at (x, y): its colour and alpha (0..255), both scaled by k / 256 for
 * the star (the word by w / 256). */
static uint32_t logo_px(int x, int y, unsigned w, unsigned k, unsigned *alpha)
{
    *alpha = 0;
    if (x >= PL_WORD_X && x < PL_WORD_X + PL_WORD_W && y >= PL_WORD_Y && y < PL_WORD_Y + PL_WORD_H) {
        unsigned a = LW_A8[(unsigned)(y - PL_WORD_Y) * PL_WORD_W + (unsigned)(x - PL_WORD_X)] * w >> 8;
        *alpha = a;
        return a << 16 | a << 8 | a;
    }
    if (x >= PL_STAR_X && x < PL_STAR_X + PL_STAR_W && y >= PL_STAR_Y && y < PL_STAR_Y + PL_STAR_H) {
        volatile uint8_t *s = LS_RGB + ((unsigned)(y - PL_STAR_Y) * PL_STAR_W + (unsigned)(x - PL_STAR_X)) * 3U;
        unsigned r = s[0] * k >> 8, g = s[1] * k >> 8, b = s[2] * k >> 8;
        *alpha = s[2];
        return r << 16 | g << 8 | b;
    }
    return 0;
}

/* The word (faded to w / 256). */
static void draw_word(unsigned w)
{
    for (int y = PL_WORD_Y; y < PL_WORD_Y + PL_WORD_H; ++y)
        for (int x = PL_WORD_X; x < PL_WORD_X + PL_WORD_W; ++x) {
            unsigned a;
            FB[(unsigned)y * STAR_FB_WIDTH + (unsigned)x] = C_BLACK | logo_px(x, y, w, 256, &a);
        }
}

/* The star and its glow (strength g / 4096, the star at k / 256, all faded to w / 256): the glow
 * is #6FB3FF at g * (1 - d^2 / R^2)^2 under the star, dithered by +-1/2 level (4 x 4 Bayer) so
 * the dim rings do not band. */
static void draw_star(unsigned g, unsigned k, unsigned w)
{
    static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    const int64_t r2 = (int64_t)GLOW_R * GLOW_R * 256;
    uint64_t gw = (uint64_t)g * w / 256U;          /* 0..4096 */
    for (int j = 0; j < GLOW_N; ++j) {
        int y = GLOW_Y0 + j;
        int64_t dy = (int64_t)y * 16 + 8 - PL_STAR_CY16;
        for (int i = 0; i < GLOW_N; ++i) {
            int x = GLOW_X0 + i;
            int64_t dx = (int64_t)x * 16 + 8 - PL_STAR_CX16, d2 = dx * dx + dy * dy;
            unsigned a;
            uint32_t base = logo_px(x, y, w, k * w >> 8, &a);
            if (d2 < r2 && gw) {
                uint64_t f = (uint64_t)(r2 - d2) * 4096U / (uint64_t)r2;      /* 1 - d^2/R^2 */
                uint64_t h = gw * (f * f / 4096U) / 4096U * (255U - a) / 255U; /* 0..4096 */
                unsigned dz = bayer[(y & 3) * 4 + (x & 3)] * 16U + 8U;          /* 0..255 */
                unsigned r = (unsigned)((0x6fU * h * 256U / 4096U + dz) >> 8);
                unsigned gg = (unsigned)((0xb3U * h * 256U / 4096U + dz) >> 8);
                unsigned b = (unsigned)((0xffU * h * 256U / 4096U + dz) >> 8);
                r += (base >> 16) & 255U;
                gg += (base >> 8) & 255U;
                b += base & 255U;
                base = (r > 255U ? 255U : r) << 16 | (gg > 255U ? 255U : gg) << 8 | (b > 255U ? 255U : b);
            }
            FB[(unsigned)y * STAR_FB_WIDTH + (unsigned)x] = C_BLACK | base;
        }
    }
}

/* Three dots under the word, brightness b[i] / 4096 (#6FB3FF), faded to w / 256. */
static void draw_dots(const unsigned *b, unsigned w)
{
    for (int d = 0; d < 3; ++d) {
        int x0 = DOT_CX + (d - 1) * DOT_GAP - DOT_BOX / 2, y0 = DOT_CY - DOT_BOX / 2;
        uint64_t s = (uint64_t)b[d] * w / 256U;
        for (int y = 0; y < DOT_BOX; ++y)
            for (int x = 0; x < DOT_BOX; ++x) {
                uint64_t a = s * dot_cov[y * DOT_BOX + x] / 16U;   /* 0..4096 */
                unsigned r = (unsigned)(0x6fU * a / 4096U), g = (unsigned)(0xb3U * a / 4096U), bb = (unsigned)(0xffU * a / 4096U);
                FB[(unsigned)(y0 + y) * STAR_FB_WIDTH + (unsigned)(x0 + x)] = C_BLACK | r << 16 | g << 8 | bb;
            }
    }
}

/* One frame at t ms: the fade in, the star's breath (2.4 s) and the dots' ripple (1.5 s). */
static void logo_frame(uint64_t t)
{
    unsigned w = 256;
    if (logo_fade && t < FADE_MS) {
        unsigned e = wave(t, 2U * FADE_MS);     /* half a wave: 0 -> 4096 over FADE_MS */
        w = e / 16U;
    }
    unsigned br = wave(t + 600U, 2400U);         /* starts a little way into the breath */
    if (logo_fade && (t < FADE_MS || logo_last < FADE_MS))
        draw_word(w);
    draw_star(br * 300U / 1000U, 222U + br * 34U / 4096U, w);
    unsigned b[3];
    for (unsigned d = 0; d < 3; ++d) {
        uint64_t u = (t + 1500U * 4U - 180U * d) % 1500U;
        unsigned bump = u < 700U ? wave(u, 700U) : 0U;
        b[d] = 700U + bump * 2900U / 4096U;
    }
    draw_dots(b, w);
    __asm__ volatile("dsb sy" : : : "memory");
}

/* The clean logo on black (what S-Boot's One mark splash shows): the menu's intro starts from it. */
static void logo_rest(int clear)
{
    if (!logo_ok)
        return;
    if (clear)
        star_fb_fill(C_BLACK);
    draw_word(256);
    draw_star(0, 256, 256);
    unsigned none[3] = { 0, 0, 0 };
    draw_dots(none, 256);
    __asm__ volatile("dsb sy" : : : "memory");
}

/* 1 if the panel already shows the logo on black (S-Boot's One mark splash): the word matches
 * and a coarse grid elsewhere is black. */
static int logo_on_panel(void)
{
    unsigned ink = 0, same = 0;
    for (int y = PL_WORD_Y; y < PL_WORD_Y + PL_WORD_H; y += 3)
        for (int x = PL_WORD_X; x < PL_WORD_X + PL_WORD_W; x += 3) {
            unsigned want = LW_A8[(unsigned)(y - PL_WORD_Y) * PL_WORD_W + (unsigned)(x - PL_WORD_X)];
            if (want < 96)
                continue;
            uint32_t c = FB[(unsigned)y * STAR_FB_WIDTH + (unsigned)x];
            unsigned m = (c >> 16) & 255U;
            if (((c >> 8) & 255U) > m) m = (c >> 8) & 255U;
            if ((c & 255U) > m) m = c & 255U;
            ++ink;
            same += m + 40U >= want && m <= want + 40U;
        }
    if (!ink || same * 100U < ink * 95U)
        return 0;
    for (unsigned y = 8; y < STAR_FB_HEIGHT; y += 24)
        for (unsigned x = 8; x < STAR_FB_WIDTH; x += 24) {
            if ((int)y >= PL_STAR_Y - 24 && (int)y < PL_WORD_Y + PL_WORD_H + 24)
                continue;
            if (FB[y * STAR_FB_WIDTH + x] & 0x00f0f0f0U)
                return 0;
        }
    return 1;
}

static void logo_open(void)
{
    logo_unpack();
    if (!logo_ok) {
        p_k.view = PV_NONE;
        return;
    }
    logo_fade = !logo_on_panel();
    if (!logo_fade)
        p_vflags |= POSTV_SBOOT;
    else
        star_fb_fill(C_BLACK);
    logo_t0 = p_ms();
    logo_last = 0;
    logo_frame(0);
}

static void logo_tick(void)
{
    if (p_k.view != PV_LOGO)
        return;
    uint64_t t = p_ms() - logo_t0;
    if (t - logo_last < FRAME_MS && t >= logo_last)
        return;
    logo_frame(t);
    logo_last = t;
}

/* ---- the live log ----------------------------------------------------------------------------- */

static void log_open(void)
{
    if (p_screen) {
        star_fb_fill(C_BLACK);
        star_fb_text_c(FULL_ROW_TITLE, "STARBITE POST", C_GOLD, C_BLACK);
        star_fb_text_c(FULL_ROW_TITLE + 1U, "SELF TEST OF THE WHOLE PHONE", C_MUTED, C_BLACK);
        star_fb_text_c(FULL_ROW_HINT, p_skip ? "SKIPPING THE REST OF POST" : "VOL DOWN: SKIP     BIXBY: RECOVERY",
                       p_skip ? C_GOLD : C_MUTED, C_BLACK);
    } else {
        star_fb_text_c(p_row0, p_mode == POST_FULL ? "STARBITE POST (FULL)" : "STARBITE POST", C_GOLD, C_BLACK);
    }
    for (unsigned id = 0; id < POST_N; ++id)
        draw_check(id);
}

/* The RAM line while it runs: window, what it does, MiB, percent of its time. */
static uint64_t ram_shown;
static void ram_live(char win, const char *what, uint64_t done, uint64_t size, unsigned pct)
{
    if (p_k.view != PV_LOG || p_pos[POST_RAM] == 0xff)
        return;
    uint64_t now = p_ms();
    if (ram_shown && now - ram_shown < 80U)
        return;
    ram_shown = now;
    struct pl l;
    pl_init(&l);
    pl_s(&l, "  RAM      ");
    char w[2] = { win, 0 };
    pl_s(&l, w);
    pl_s(&l, ": ");
    pl_s(&l, what);
    pl_s(&l, " ");
    pl_u(&l, done >> 20);
    pl_s(&l, "/");
    pl_u(&l, size >> 20);
    pl_s(&l, " MIB");
    pl_pad(&l, STAR_FB_COLS - 4U);
    pl_u(&l, pct > 99 ? 99 : pct);
    pl_s(&l, "%");
    star_fb_text_c(check_row(POST_RAM), l.b, C_WHITE, C_BLACK);
}

/* Between and inside the checks: the keys (Volume Up: the log; Volume Down: skip; Bixby:
 * recovery afterwards) and the logo's next frame. */
static void tick(void)
{
    if (p_bixby && !*p_bixby && rec_bixby_now())
        *p_bixby = 1;
    unsigned k = hw_keys();
    unsigned ev = pv_poll(&p_k, (k & K_UP) != 0, (k & K_DOWN) != 0, p_ms());
    if (ev & PV_EV_SKIP) {
        p_skip = 1;
        p_vflags |= POSTV_SKIPPED;
        if (p_k.view == PV_LOG && p_screen)
            star_fb_text_c(FULL_ROW_HINT, "SKIPPING THE REST OF POST", C_GOLD, C_BLACK);
    }
    if (ev & PV_EV_LOG) {
        p_vflags |= POSTV_LOG_KEY;
        star_pmsg("STARBITE: POST: Volume Up: the live log\n");
        log_open();
    }
    logo_tick();
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
    tick();
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
        static const char wname[3] = { 'A', 'B', 'C' };
        uint64_t budget = p_freq() / 1000U * POST_RAM_MS, end = t0 + budget;
        uint64_t tw = 0, tr = 0;        /* ticks per chunk written and read back, as measured */
        int out = 0;
        /* address, then inverted address, per window: write it all, then read it all, so a word
         * that aliases another anywhere in the window shows. The budget covers the reads too
         * (before 2026-11-07 it was only checked before each write, so reading back what window C
         * had written took seconds more: the old bar was full long before RAM was done). A chunk
         * is written only while there is time left to read back everything written so far, at the
         * measured speeds (non-cacheable reads are slower than writes; 3 x until one was timed),
         * with 1/8 to spare for the keys and the logo's frames. */
        for (unsigned w = 0; w < 3 && !out && !p_skip; ++w) {
            uint64_t done_w = 0, size = win[w][1];
            for (unsigned k = 0; k < 2 && !out && !p_skip; ++k) {
                uint64_t n = 0;
                for (; n < size && !p_skip; n += CHUNK) {
                    uint64_t est_r = tr ? tr : tw * 3U;
                    uint64_t need = (tw + (n / CHUNK + 1U) * est_r) * 9U / 8U;
                    if ((n || w || k) && p_ticks() + need > end) {
                        out = 1;
                        break;
                    }
                    uint64_t a = p_ticks();
                    ram_fill(win[w][0] + n, CHUNK, k);
                    tw = p_ticks() - a;
                    ram_live(wname[w], "WRITE", n + CHUNK, size, (unsigned)((p_ticks() - t0) * 100U / budget));
                    tick();
                }
                uint64_t m = 0;
                for (; m < n && !p_skip; m += CHUNK) {
                    uint64_t a = p_ticks();
                    bad += ram_cmp(win[w][0] + m, CHUNK, k);
                    tr = p_ticks() - a;
                    ram_live(wname[w], "CHECK", m + CHUNK, n, (unsigned)((p_ticks() - t0) * 100U / budget));
                    tick();
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
        /* 0101... and 1010... over window A while time is left */
        for (unsigned k = 2; k < 4 && !p_skip && !out; ++k)
            for (uint64_t n = 0; n < win[0][1] && !p_skip; n += CHUNK) {
                if (p_ticks() + (tw + tr) * 9U / 8U > end) {
                    out = 1;
                    break;
                }
                ram_fill(win[0][0] + n, CHUNK, k);
                bad += ram_cmp(win[0][0] + n, CHUNK, k);
                ram_live('A', k == 2 ? "0X55" : "0XAA", n + CHUNK, win[0][1], (unsigned)((p_ticks() - t0) * 100U / budget));
                tick();
            }
        p_ram_ms = (p_ticks() - t0) * 1000U / p_freq();
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
        /* the logo's frames in between (one every 33 ms, about 1 ms each: well inside the 3 %) */
        for (unsigned i = 0; ok && i < 600 && !ta && !p_skip; ++i) {
            if (em_rtc_epoch(&e) && e != e0)
                ta = p_ticks(), e0 = e;
            else
                tick();
        }
        for (unsigned i = 0; ta && i < 600 && !tb && !p_skip; ++i) {
            if (em_rtc_epoch(&e) && e != e0)
                tb = p_ticks();
            else
                tick();
        }
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
        log[i <= POST_MICROSD ? POST_LOG_CHECK + i : POST_LOG_CHECK2 + i - POST_CPU] = (uint64_t)c->state << 56 | (uint64_t)((uint32_t)c->value & 0xffffffU) << 32 | c->us;
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

/* screen: the full-screen layout for the log (else rows from row0 over what is there); view: how
 * it starts (PV_*); up_at_start: Volume Up was down as it began (fastboot's gesture). */
static int run(unsigned mode, unsigned row0, int screen, unsigned view, int touch, int *bixby, int up_at_start)
{
    p_t0 = p_ticks();
    p_mode = mode;
    p_row0 = row0;
    p_screen = (unsigned)screen;
    p_bixby = bixby;
    p_skip = 0;
    p_done = 0;
    p_vflags = up_at_start ? POSTV_UP_START : 0U;
    p_ram_ms = 0;
    ram_shown = 0;
    rep.magic = 0;
    rep.mode = mode;
    rep.n = POST_N;
    rep.fails = rep.warns = rep.fatal = rep.skipped = 0;
    pv_init(&p_k, view, up_at_start, 0);
    if (view == PV_LOG)
        p_vflags |= POSTV_LOG_SET;
    layout(mode);
    if (view == PV_LOG)
        log_open();
    else if (view == PV_LOGO)
        logo_open();
    run_checks(touch);
    rep.skipped = (uint32_t)p_skip;
    rep.fatal = rep.c[POST_RAM].fatal || rep.c[POST_TIMER].fatal;
    rep.total_us = p_us(p_t0);
    if (p_k.view == PV_LOG) {
        if (screen) {
            summary_line(FULL_ROW_SUM);
            problem_list();
        } else {
            summary_line(row0 + 1U + p_count);
        }
    }
    rep.magic = POST_MAGIC;
    rep_valid = 1;
    post_log();
    return (int)rep.fatal;
}

/* Starbite Recovery's "Run POST": the lines on its own screen, as before. */
int post_run(unsigned mode, unsigned row0, int touch, int *bixby)
{
    int f = run(mode, row0, 0, PV_LOG, touch, bixby, (hw_keys() & K_UP) != 0);
    p_bixby = 0;
    return f;
}

/* Keys pressed now, read twice 30 ms apart (one press). */
static unsigned keys_debounced(void)
{
    unsigned k = hw_keys();
    if (!k)
        return 0;
    hw_wait_ms(30);
    return k & hw_keys();
}

/* The log view after the run: PV_READ_MS to read it, longer while Volume Up (pressed during
 * POST) stays held; Power or Volume Down end it, Bixby still asks for recovery. */
static void read_pause(int *bixby)
{
    if (p_screen)
        star_fb_text_c(FULL_ROW_HINT, p_k.up_used ? "HOLD VOL UP: READ   POWER: CONTINUE" : "POWER: CONTINUE",
                       C_MUTED, C_BLACK);
    uint64_t t0 = p_ms();
    for (unsigned g = 0; g < (PV_HOLD_MAX_MS + 1000U) / 10U; ++g) {
        if (bixby && !*bixby && rec_bixby_now())
            *bixby = 1;
        unsigned k = hw_keys();
        if (!pv_hold(&p_k, (k & K_UP) != 0, (k & (K_POWER | K_DOWN)) != 0, p_ms() - t0))
            break;
        hw_wait_ms(10);
    }
}

/* A red check: what failed and why, one line each, until Power (or PV_FAIL_WAIT_S). Bixby: Starbite
 * Recovery afterwards. Volume Up is not a key here (it may be fastboot's gesture, still held). */
static void fail_screen(int *bixby)
{
    const uint8_t *order = p_mode == POST_FULL ? order_full : order_quick;
    unsigned n = p_mode == POST_FULL ? POST_N : sizeof(order_quick);
    uint8_t st[POST_N], ids[5];
    for (unsigned id = 0; id < POST_N; ++id)
        st[id] = rep.c[id].state;
    unsigned nf = pv_failures(st, order, n, POST_FAIL, ids, sizeof(ids));
    struct pl l;
    star_fb_fill(C_BLACK);
    unsigned r = 15;
    star_fb_text_c(r++, "STARBITE POST", C_MUTED, C_BLACK);
    pl_init(&l);
    if (nf == 1) {
        pl_s(&l, "A CHECK FAILED");
    } else {
        pl_u(&l, nf);
        pl_s(&l, " CHECKS FAILED");
    }
    star_fb_text_c(r++, l.b, C_RED, C_BLACK);
    ++r;
    for (unsigned i = 0; i < nf && i < sizeof(ids); ++i) {
        const struct post_check *c = &rep.c[ids[i]];
        pl_init(&l);
        pl_s(&l, FBT_CROSS " ");
        pl_s(&l, c->name);
        star_fb_text_c(r++, l.b, C_WHITE, C_BLACK);
        pl_init(&l);
        pl_s(&l, "  ");
        pl_s(&l, c->detail);
        star_fb_text_c(r++, l.b, C_RED, C_BLACK);
    }
    if (nf > sizeof(ids)) {
        pl_init(&l);
        pl_s(&l, "+ ");
        pl_u(&l, nf - sizeof(ids));
        pl_s(&l, " MORE IN THE CRASH LOG");
        star_fb_text_c(r++, l.b, C_MUTED, C_BLACK);
    }
    ++r;
    star_fb_text_c(r++, rep.fatal ? "POWER: STARBITE RECOVERY" : "POWER: CONTINUE", C_WHITE, C_BLACK);
    if (!rep.fatal)
        star_fb_text_c(r++, "BIXBY: STARBITE RECOVERY", C_MUTED, C_BLACK);
    unsigned rc = r + 1U, shown = ~0U;
    star_pmsg("STARBITE: POST: failure screen\n");
    /* the keys of the run let go first (bounded: a stuck key cannot hold the boot) */
    for (unsigned g = 0; g < 300 && (hw_keys() & (K_POWER | K_BIXBY | K_DOWN)); ++g)
        hw_wait_ms(10);
    uint64_t t0 = p_ms();
    for (;;) {
        uint64_t t = p_ms() - t0;
        if (t >= PV_FAIL_WAIT_S * 1000U)
            break;
        unsigned left = PV_FAIL_WAIT_S - (unsigned)(t / 1000U);
        if (left != shown) {
            pl_init(&l);
            pl_s(&l, "CONTINUING IN ");
            pl_u(&l, left);
            pl_s(&l, " S");
            star_fb_text_c(rc, l.b, C_MUTED, C_BLACK);
            shown = left;
        }
        unsigned k = hw_keys() & (K_POWER | K_BIXBY);
        if (k && (k = keys_debounced() & (K_POWER | K_BIXBY)) != 0) {
            if ((k & K_BIXBY) && bixby)
                *bixby = 1;
            break;
        }
        hw_wait_ms(10);
    }
    for (unsigned g = 0; g < 300 && (hw_keys() & (K_POWER | K_BIXBY)); ++g)
        hw_wait_ms(10);
}

static void post_view_log(void)
{
    volatile uint64_t *log = POST_LOG;
    log[POST_LOG_VIEW] = p_ram_ms << 32 | p_vflags;
}

int post_boot(unsigned level, unsigned flags, int *bixby, int *skipped, const char **why)
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
    int touch = (flags & POST_F_TOUCH) != 0, up0 = (hw_keys() & K_UP) != 0;
    int fatal;
    if (level == SET_POST_FULL) {
        rec_rows_save(0, STAR_FB_HEIGHT, POST_PANEL_SAVE);
        fatal = run(POST_FULL, 0, 1, (flags & POST_F_LOG) ? PV_LOG : PV_LOGO, touch, bixby, up0);
        unsigned end = pv_end(&p_k, rep.fails);
        unsigned was = p_k.view;
        if (end == PV_END_READ)
            read_pause(bixby);
        else if (end == PV_END_FAIL) {
            fail_screen(bixby);
            p_vflags |= POSTV_FAILSCR;
        }
        if (flags & POST_F_KEEP_LOGO)
            logo_unpack();
        if ((flags & POST_F_KEEP_LOGO) && logo_ok) {
            /* the clean logo, as S-Boot's One mark splash shows it: menu v7's intro hands it over */
            logo_rest(was != PV_LOGO || end != PV_END_GO);
            p_vflags |= POSTV_KEPT;
        } else {
            rec_rows_restore(0, STAR_FB_HEIGHT, POST_PANEL_SAVE);
        }
    } else {
        unsigned y0 = star_fb_row_y(0), y1 = star_fb_row_y(sizeof(order_quick) + 2U);
        rec_rows_save(y0, y1, POST_ROWS_SAVE);
        fatal = run(POST_QUICK, 0, 0, (flags & POST_F_LOG) ? PV_LOG : PV_NONE, touch, bixby, up0);
        unsigned end = pv_end(&p_k, rep.fails);
        if (end == PV_END_READ)
            read_pause(bixby);
        rec_rows_restore(y0, y1, POST_ROWS_SAVE);
        if (end == PV_END_FAIL) {
            rec_rows_save(0, STAR_FB_HEIGHT, POST_PANEL_SAVE);
            fail_screen(bixby);
            p_vflags |= POSTV_FAILSCR;
            rec_rows_restore(0, STAR_FB_HEIGHT, POST_PANEL_SAVE);
        }
    }
    post_view_log();
    p_bixby = 0;
    star_fb_row_base(base);
    if (skipped)
        *skipped = (int)rep.skipped;
    if (why)
        *why = rep.c[POST_RAM].fatal ? "POST: RAM PATTERN FAILED" : rep.c[POST_TIMER].fatal ? "POST: TIMER STOPPED" : 0;
    return fatal;
}
