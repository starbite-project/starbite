/* SPDX-License-Identifier: GPL-2.0-only
 * Project Starbite boot menu and Starbite Engineer mode, run by the loader
 * before it copies XNU (MMU off, one CPU, S-Boot's state).
 *
 * Keys are the S9's ALIVE GPIOs, active low (starlte DTB gpio_keys;
 * a5xelte_keys.c STARLTE): GPA0 DAT 0x14050024 bit 3 Volume Up, bit 4 Volume
 * Down, bit 6 Bixby; GPA2 DAT 0x14050064 bit 4 Power. Volume moves, Power (or
 * Bixby) selects. Reboots go through the Exynos9810 PMU 0x14060000 the way
 * the vendor kernel does: INFORM2 0x12345678, INFORM3 0x12345671 Download or
 * 0x12345674 Recovery, then SWRESET.
 *
 * Engineer mode only reads hardware, except where marked RISKY: those items
 * change the boot-args (UFS multi-block, CPU probe) or write two PMIC
 * regulators with the kernel's own safety checks (touch rails).
 * No pointer tables here: the Image must have no absolute relocations.
 */
#include "menu.h"
#include "fbtext.h"

#define ALIVE_GPA0_DAT 0x14050024UL
#define ALIVE_GPA2_DAT 0x14050064UL
#define PMU_BASE 0x14060000UL
#define INFORM_DOWNLOAD 0x12345671U
#define INFORM_RECOVERY 0x12345674U

#define K_UP 1U
#define K_DOWN 2U
#define K_BIXBY 4U
#define K_POWER 8U
#define K_SELECT (K_BIXBY | K_POWER)

#define C_WHITE 0xffffffffU
#define C_BLACK 0xff000000U
#define C_BLUE 0xff2a5bf0U      /* Starbite mark gradient */
#define C_MUTED 0xffb1bad3U
#define C_GOLD 0xffffd43bU
#define C_RED 0xffff4d4dU
#define C_GREEN 0xff3ddc84U

/* ---- time, keys, reboot ------------------------------------------------ */

static uint64_t ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r" (v));
    return v;
}

static uint64_t freq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (f));
    return f ? f : 26000000U;   /* S-Boot leaves CNTFRQ 0; the timer is 26 MHz */
}

static void wait_us(uint64_t us)
{
    uint64_t t0 = ticks(), n = freq() / 1000000U * us;
    while (ticks() - t0 < n)
        ;
}

static void wait_ms(unsigned ms)
{
    wait_us((uint64_t)ms * 1000U);
}

static unsigned keys(void)
{
    uint32_t a = *(volatile uint32_t *)ALIVE_GPA0_DAT;
    uint32_t b = *(volatile uint32_t *)ALIVE_GPA2_DAT;
    return (!((a >> 3) & 1U) ? K_UP : 0) | (!((a >> 4) & 1U) ? K_DOWN : 0) |
           (!((a >> 6) & 1U) ? K_BIXBY : 0) | (!((b >> 4) & 1U) ? K_POWER : 0);
}

/* One debounced press, returned once every key is released again. */
static unsigned key_wait(void)
{
    while (keys())
        wait_ms(20);
    for (;;) {
        unsigned k = keys();
        if (k) {
            wait_ms(40);
            if (keys() == k) {
                while (keys())
                    wait_ms(20);
                return k;
            }
        }
        wait_ms(10);
    }
}

static void reboot(uint32_t inform3)
{
    volatile uint32_t *pmu = (volatile uint32_t *)PMU_BASE;
    pmu[0x808 / 4] = 0x12345678U;
    pmu[0x80c / 4] = inform3;
    __asm__ volatile("dsb sy" : : : "memory");
    pmu[0x400 / 4] = 1;
    __asm__ volatile("dsb sy" : : : "memory");
    for (;;)
        __asm__ volatile("wfe");
}

/* ---- text ------------------------------------------------------------ */

struct sb { char b[STAR_FB_COLS + 2]; unsigned n; };

static void sb_init(struct sb *s)
{
    s->n = 0;
    s->b[0] = 0;
}

static void sb_s(struct sb *s, const char *t)
{
    while (*t && s->n < STAR_FB_COLS)
        s->b[s->n++] = *t++;
    s->b[s->n] = 0;
}

static void sb_c(struct sb *s, char c)
{
    if (s->n < STAR_FB_COLS)
        s->b[s->n++] = c;
    s->b[s->n] = 0;
}

static void sb_hex(struct sb *s, uint64_t v, unsigned digits)
{
    sb_s(s, "0X");
    for (int sh = (int)digits * 4 - 4; sh >= 0; sh -= 4) {
        unsigned d = (unsigned)(v >> sh) & 15U;
        sb_c(s, (char)(d < 10 ? '0' + d : 'A' + d - 10));
    }
}

static void sb_dec(struct sb *s, uint64_t v)
{
    char t[24];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v);
    while (n)
        sb_c(s, t[--n]);
}

/* "label value" in one row. */
static void row_hex(unsigned row, const char *label, uint64_t v, unsigned digits)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, label);
    sb_c(&s, ' ');
    sb_hex(&s, v, digits);
    star_fb_text(row, s.b);
}

static void row_dec(unsigned row, const char *label, uint64_t v, const char *unit)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, label);
    sb_c(&s, ' ');
    sb_dec(&s, v);
    sb_s(&s, unit);
    star_fb_text(row, s.b);
}

/* ---- menus ----------------------------------------------------------- */

#define MAX_ITEMS 10
static char items[MAX_ITEMS][STAR_FB_COLS + 2];
static uint32_t item_fg[MAX_ITEMS];

static void item(unsigned i, const char *s, uint32_t fg)
{
    unsigned n = 0;
    while (s[n] && n < STAR_FB_COLS - 2) {
        items[i][n] = s[n];
        ++n;
    }
    items[i][n] = 0;
    item_fg[i] = fg;
}

static void draw_item(unsigned row, unsigned i, int selected)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, selected ? "> " : "  ");
    sb_s(&s, items[i]);
    if (selected)
        star_fb_text_c(row, s.b, C_WHITE, C_BLUE);
    else
        star_fb_text_c(row, s.b, item_fg[i], C_BLACK);
}

/* Items 0..n-1 at rows row0..; Volume moves, Power/Bixby picks. */
static unsigned choose(unsigned row0, unsigned n, unsigned sel)
{
    for (unsigned i = 0; i < n; ++i)
        draw_item(row0 + i, i, i == sel);
    for (;;) {
        unsigned k = key_wait(), old = sel;
        if (k & K_SELECT)
            return sel;
        if (k & K_UP)
            sel = sel ? sel - 1 : n - 1;
        else if (k & K_DOWN)
            sel = (sel + 1) % n;
        if (sel != old) {
            draw_item(row0 + old, old, 0);
            draw_item(row0 + sel, sel, 1);
        }
    }
}

/* ---- boot-args ------------------------------------------------------- */

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        ++n;
    return n;
}

/* Does the token t[0..n) have key `key` (before '=', or the whole token)? */
static int tok_is(const char *t, unsigned n, const char *key)
{
    unsigned i = 0;
    while (key[i] && i < n && t[i] == key[i])
        ++i;
    return !key[i] && (i == n || t[i] == '=');
}

static const char *arg_find(const char *c, const char *key, unsigned *len)
{
    for (const char *p = c; *p;) {
        while (*p == ' ')
            ++p;
        unsigned n = 0;
        while (p[n] && p[n] != ' ')
            ++n;
        if (n && tok_is(p, n, key)) {
            *len = n;
            return p;
        }
        p += n;
    }
    return 0;
}

/* Remove every `key` / `key=...` token. */
static void arg_del(char *c, const char *key)
{
    char *w = c;
    for (const char *p = c; *p;) {
        while (*p == ' ')
            ++p;
        unsigned n = 0;
        while (p[n] && p[n] != ' ')
            ++n;
        if (n && !tok_is(p, n, key)) {
            if (w != c)
                *w++ = ' ';
            for (unsigned i = 0; i < n; ++i)
                *w++ = p[i];
        }
        p += n;
    }
    *w = 0;
}

/* Replace `key` with `key=val` (val 0: the bare flag). */
static void arg_set(char *c, unsigned len, const char *key, const char *val)
{
    arg_del(c, key);
    unsigned n = slen(c), need = 1 + slen(key) + (val ? 1 + slen(val) : 0);
    if (n + need >= len)
        return;
    if (n)
        c[n++] = ' ';
    for (const char *k = key; *k;)
        c[n++] = *k++;
    if (val) {
        c[n++] = '=';
        while (*val)
            c[n++] = *val++;
    }
    c[n] = 0;
}

/* Decimal value of `key=N`, or dflt. */
static unsigned arg_num(const char *c, const char *key, unsigned dflt)
{
    unsigned n;
    const char *t = arg_find(c, key, &n);
    unsigned k = slen(key);
    if (!t || n <= k + 1)
        return dflt;
    unsigned v = 0;
    for (unsigned i = k + 1; i < n && t[i] >= '0' && t[i] <= '9'; ++i)
        v = v * 10U + (unsigned)(t[i] - '0');
    return v;
}

static struct {
    unsigned cpus;
    int rw, verbose, touch, touchpwr;   /* boot options */
    int multi, smpprobe;                /* risky */
} opt;

static void opt_load(const char *c)
{
    unsigned n;
    opt.cpus = arg_num(c, "cpus", 1);
    opt.rw = arg_num(c, "ufsrw", 0) == 1;
    opt.verbose = arg_find(c, "-v", &n) != 0;
    opt.touch = arg_num(c, "startouch", 1) != 0;
    opt.touchpwr = arg_num(c, "startouchpwr", 1) != 0;
    opt.multi = arg_num(c, "ufsmulti", 0) != 0;
    opt.smpprobe = arg_num(c, "starsmpprobe", 0) != 0;
}

static void opt_apply(char *c, unsigned len)
{
    char num[4];
    num[0] = (char)('0' + opt.cpus);
    num[1] = 0;
    arg_set(c, len, "cpus", num);
    /* Multi-block UFS corrupted reads twice: never let it write the root. */
    int rw = opt.rw && !opt.multi;
    arg_set(c, len, "ufsrw", rw ? "1" : "0");
    arg_set(c, len, "rootrw", rw ? "1" : "0");
    if (opt.verbose)
        arg_set(c, len, "-v", 0);
    else
        arg_del(c, "-v");
    if (opt.touch)
        arg_del(c, "startouch");
    else
        arg_set(c, len, "startouch", "0");
    if (opt.touchpwr)
        arg_del(c, "startouchpwr");
    else
        arg_set(c, len, "startouchpwr", "0");
    if (opt.multi)
        arg_set(c, len, "ufsmulti", "1");
    else
        arg_del(c, "ufsmulti");
    if (opt.smpprobe)
        arg_set(c, len, "starsmpprobe", "1");
    else
        arg_del(c, "starsmpprobe");
}

/* ---- ACPM / PMIC (as bsd/dev/starlte_touch.c) ------------------------ */

#define ACPM_MBOX 0x14100000UL
#define ACPM_SRAM 0x02039000UL
#define ACPM_SRAM_SIZE 0x30000U
#define ACPM_INITDATA 0x7f00U
#define ACPM_PMIC_CH 2U

static uint32_t ard(uint32_t off)
{
    return *(volatile uint32_t *)(ACPM_SRAM + off);
}

static void awr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(ACPM_SRAM + off) = v;
}

static uint32_t acpm_seq;

/* SRAM offset of channel id's descriptor, or 0. */
static uint32_t acpm_find(uint32_t id)
{
    uint32_t chans = ard(ACPM_INITDATA + 0x08), ap_max = ard(ACPM_INITDATA + 0x18);
    if (!ap_max || ap_max > 32 || chans >= ACPM_SRAM_SIZE - ap_max * 0x48)
        return 0;
    for (uint32_t i = 0; i < ap_max; ++i) {
        uint32_t c = chans + i * 0x48;
        if (ard(c) == id) {
            uint32_t q = ard(c + 0x34), e = ard(c + 0x38);
            return (q && q <= 64 && e >= 16 && e <= 64 && !(e & 3)) ? c : 0;
        }
    }
    return 0;
}

static int acpm_send(uint32_t c, uint32_t id, uint32_t cmd[4])
{
    uint32_t qlen = ard(c + 0x34), esz = ard(c + 0x38);
    uint32_t tx_rear = ard(c + 0x10), tx_front = ard(c + 0x14), tx_base = ard(c + 0x18);
    uint32_t rx_rear = ard(c + 0x28), rx_front = ard(c + 0x2c), rx_base = ard(c + 0x30);
    uint32_t front = ard(tx_front), next = (front + 1) % qlen;
    for (unsigned us = 0; next == ard(tx_rear); ++us) {
        if (us > 100000)
            return 0;
        wait_us(1);
    }
    if (++acpm_seq >= 64)
        acpm_seq = 1;
    uint32_t seq = acpm_seq;
    cmd[0] = (cmd[0] & ~(0x3fU << 16)) | (seq << 16);
    uint32_t slot = tx_base + esz * front;
    for (uint32_t i = 0; i < esz / 4; ++i)
        awr(slot + 4 * i, i < 4 ? cmd[i] : 0);
    __asm__ volatile("dsb sy" : : : "memory");
    awr(tx_front, next);
    __asm__ volatile("dsb sy" : : : "memory");
    *(volatile uint32_t *)(ACPM_MBOX + 0x08) = (1U << id) << 16;    /* INTGR0 */
    for (unsigned us = 0; us < 100000; ++us) {
        uint32_t rear = ard(rx_rear), rfront = ard(rx_front);
        for (uint32_t i = rear; i != rfront; i = (i + 1) % qlen) {
            uint32_t e = rx_base + esz * i;
            if (((ard(e) >> 16) & 0x3fU) != seq)
                continue;
            for (uint32_t w = 0; w < 4; ++w)
                cmd[w] = ard(e + 4 * w);
            if (i != rear)
                for (uint32_t w = 0; w < esz / 4; ++w)
                    awr(e + 4 * w, ard(rx_base + esz * rear + 4 * w));
            rear = (rear + 1) % qlen;
            awr(rx_rear, rear);
            __asm__ volatile("dsb sy" : : : "memory");
            if (rear == ard(rx_front))
                *(volatile uint32_t *)(ACPM_MBOX + 0x20) = 1U << id;  /* INTCR1 */
            return 1;
        }
        wait_us(1);
    }
    return 0;
}

static int pmic_read(uint32_t type, uint32_t reg, uint8_t *val)
{
    uint32_t c = acpm_find(ACPM_PMIC_CH), cmd[4];
    cmd[0] = (type & 0xfU) << 8 | (reg & 0xffU);
    cmd[1] = cmd[2] = cmd[3] = 0;
    if (!c || !acpm_send(c, ACPM_PMIC_CH, cmd) || (cmd[1] >> 24))
        return 0;
    *val = (uint8_t)(cmd[1] >> 8);
    return 1;
}

static int pmic_write(uint32_t type, uint32_t reg, uint8_t val)
{
    uint32_t c = acpm_find(ACPM_PMIC_CH), cmd[4];
    cmd[0] = (type & 0xfU) << 8 | (reg & 0xffU);
    cmd[1] = 1U | (uint32_t)val << 8;
    cmd[2] = cmd[3] = 0;
    return c && acpm_send(c, ACPM_PMIC_CH, cmd) && !(cmd[1] >> 24);
}

/* "LDO43 0XF0 ON 3.000 V": S2MPS18 LDO CTRL, bits 7:6 enable, 5:0 25 mV steps. */
static void row_ldo(unsigned row, const char *name, int ok, uint8_t v, unsigned base_mv)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, name);
    sb_c(&s, ' ');
    if (!ok) {
        sb_s(&s, "READ FAILED");
        star_fb_text_c(row, s.b, C_RED, C_BLACK);
        return;
    }
    unsigned mv = base_mv + (v & 0x3fU) * 25U;
    sb_hex(&s, v, 2);
    sb_s(&s, (v & 0xc0U) ? " ON  " : " OFF ");
    sb_dec(&s, mv / 1000U);
    sb_c(&s, '.');
    sb_c(&s, (char)('0' + mv / 100U % 10U));
    sb_c(&s, (char)('0' + mv / 10U % 10U));
    sb_c(&s, (char)('0' + mv % 10U));
    sb_s(&s, " V");
    star_fb_text_c(row, s.b, (v & 0xc0U) ? C_GREEN : C_GOLD, C_BLACK);
}

/* ---- Engineer pages --------------------------------------------------- */

#define ROW_TITLE 3U
#define ROW_BODY 5U
#define ROW_HINT 43U

static void page(const char *title, const char *hint)
{
    star_fb_fill(C_BLACK);
    star_fb_text_c(0, "STARBITE ENGINEER", C_WHITE, C_BLUE);
    star_fb_text_c(1, "PROJECT STARBITE  -  BY STARBITE", C_MUTED, C_BLACK);
    star_fb_text_c(ROW_TITLE, title, C_GOLD, C_BLACK);
    star_fb_text_c(ROW_HINT, hint, C_MUTED, C_BLACK);
}

#define HINT_MENU "VOL: MOVE   POWER: SELECT"
#define HINT_BACK "PRESS POWER TO GO BACK"

static void wait_select(void)
{
    while (!(key_wait() & K_SELECT))
        ;
}

static void diag_sysinfo(const struct star_info *in, const char *cmdline)
{
    uint64_t midr, mpidr;
    __asm__ volatile("mrs %0, midr_el1" : "=r" (midr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r" (mpidr));
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (f));
    page("SYSTEM INFO", HINT_BACK);
    unsigned r = ROW_BODY;
    row_hex(r++, "MIDR     ", midr, 8);
    star_fb_text_c(r++, ((midr >> 4) & 0xfffU) == 0xd05 ? "         CORTEX-A55 (LITTLE)" :
                   "         NOT A CORTEX-A55", C_MUTED, C_BLACK);
    row_hex(r++, "MPIDR    ", mpidr, 8);
    row_dec(r++, "EL       ", in->el >> 2, "");
    row_hex(r++, "SCTLR    ", in->sctlr, 8);
    if (f)
        row_dec(r++, "CNTFRQ   ", f, " HZ");
    else
        star_fb_text(r++, "CNTFRQ   0 (26 MHZ ASSUMED)");
    row_hex(r++, "FDT      ", in->fdt, 8);
    row_hex(r++, "PAYLOAD  ", in->rd_start, 8);
    row_dec(r++, "  SIZE   ", (in->rd_end - in->rd_start) >> 10, " KIB");
    row_hex(r++, "XNU RAM  ", in->phys_base, 8);
    row_dec(r++, "  SIZE   ", in->mem_size >> 20, " MIB");
    row_hex(r++, "ENTRY VA ", in->entry_va, 16);
    row_dec(r++, "SEGMENTS ", in->nsegs, "");
    ++r;
    star_fb_text_c(r++, "BOOT-ARGS:", C_GOLD, C_BLACK);
    /* Wrap the boot-args over the remaining rows. */
    for (const char *p = cmdline; *p && r < ROW_HINT - 1;) {
        struct sb s;
        sb_init(&s);
        while (*p && s.n < STAR_FB_COLS)
            sb_c(&s, *p++);
        star_fb_text(r++, s.b);
    }
    wait_select();
}

/* Bars written as 0xAARRGGBB; a swapped label shows an R/B swap. */
static void diag_display(void)
{
    page("DISPLAY COLOUR TEST", "ANY KEY: BACK");
    static const uint32_t bars[6] = {
        0xffff0000U, 0xff00ff00U, 0xff0000ffU, 0xffffffffU, 0xff808080U, 0xffffd43bU,
    };
    static const char names[6][16] = {
        "RED", "GREEN", "BLUE", "WHITE", "50% GREY", "GOLD",
    };
    for (unsigned i = 0; i < 6; ++i) {
        unsigned row = ROW_BODY + i * 6U;
        star_fb_text(row, names[i]);
        star_fb_fill_rows(star_fb_row_y(row + 1), star_fb_row_y(row + 5), bars[i]);
    }
    star_fb_text_c(ROW_BODY + 36, "EACH BAR SHOULD MATCH ITS NAME.", C_MUTED, C_BLACK);
    key_wait();
}

static void diag_buttons(void)
{
    page("BUTTON TEST", "HOLD VOL UP + VOL DOWN: BACK");
    star_fb_text_c(ROW_BODY, "PRESS ANY BUTTON:", C_MUTED, C_BLACK);
    unsigned last = ~0U;
    for (;;) {
        unsigned k = keys();
        if ((k & (K_UP | K_DOWN)) == (K_UP | K_DOWN))
            break;
        if (k != last) {
            star_fb_text_c(ROW_BODY + 2, (k & K_UP) ? "VOLUME UP     PRESSED" : "VOLUME UP     -",
                           (k & K_UP) ? C_GREEN : C_WHITE, C_BLACK);
            star_fb_text_c(ROW_BODY + 3, (k & K_DOWN) ? "VOLUME DOWN   PRESSED" : "VOLUME DOWN   -",
                           (k & K_DOWN) ? C_GREEN : C_WHITE, C_BLACK);
            star_fb_text_c(ROW_BODY + 4, (k & K_BIXBY) ? "BIXBY         PRESSED" : "BIXBY         -",
                           (k & K_BIXBY) ? C_GREEN : C_WHITE, C_BLACK);
            star_fb_text_c(ROW_BODY + 5, (k & K_POWER) ? "POWER         PRESSED" : "POWER         -",
                           (k & K_POWER) ? C_GREEN : C_WHITE, C_BLACK);
            row_hex(ROW_BODY + 7, "GPA0 DAT", *(volatile uint32_t *)ALIVE_GPA0_DAT, 8);
            row_hex(ROW_BODY + 8, "GPA2 DAT", *(volatile uint32_t *)ALIVE_GPA2_DAT, 8);
            last = k;
        }
        wait_ms(20);
    }
    while (keys())
        wait_ms(20);
}

static void show_pmic(unsigned r)
{
    uint8_t id = 0, l35 = 0, l43 = 0;
    char build[26];
    for (unsigned i = 0; i < 24; i += 4) {
        uint32_t w = ard(ACPM_INITDATA + 0x60 + i);
        for (unsigned b = 0; b < 4; ++b) {
            char c = (char)(w >> (8 * b));
            build[i + b] = (c >= 32 && c < 127) ? c : '.';
        }
    }
    build[24] = 0;
    struct sb s;
    sb_init(&s);
    sb_s(&s, "ACPM ");
    sb_s(&s, build);
    star_fb_text(r++, s.b);
    if (!acpm_find(ACPM_PMIC_CH)) {
        star_fb_text_c(r, "ACPM PMIC CHANNEL NOT FOUND", C_RED, C_BLACK);
        return;
    }
    int ok = pmic_read(0, 0x00, &id);
    if (ok)
        row_hex(r++, "S2MPS18 ID", id, 2);
    else
        star_fb_text_c(r++, "S2MPS18 ID READ FAILED", C_RED, C_BLACK);
    row_ldo(r++, "LDO35 TSP_IO  ", pmic_read(1, 0x6c, &l35), l35, 700);
    row_ldo(r++, "LDO43 TSP_AVDD", pmic_read(1, 0x74, &l43), l43, 1800);
    star_fb_text_c(r + 1, "TOUCH WANTS LDO35 1.800 V, LDO43 3.000 V", C_MUTED, C_BLACK);
}

static void diag_pmic(void)
{
    page("PMIC / TOUCH RAILS (READ ONLY)", HINT_BACK);
    show_pmic(ROW_BODY);
    wait_select();
}

/* 16 MiB of the XNU window, before XNU is copied there. */
#define RAMTEST_BASE 0x92000000UL
#define RAMTEST_SIZE (16UL << 20)

static void diag_ram(void)
{
    page("RAM TEST (16 MIB AT 0X92000000)", HINT_BACK);
    volatile uint64_t *m = (volatile uint64_t *)RAMTEST_BASE;
    uint64_t n = RAMTEST_SIZE / 8, errors = 0, t0 = ticks();
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint64_t inv = pass ? ~0ULL : 0;
        star_fb_text(ROW_BODY + pass, pass ? "PASS 2: INVERTED PATTERN..." : "PASS 1: ADDRESS PATTERN...");
        for (uint64_t i = 0; i < n; ++i)
            m[i] = (i * 0x9e3779b97f4a7c15ULL) ^ inv;
        __asm__ volatile("dsb sy" : : : "memory");
        for (uint64_t i = 0; i < n; ++i)
            if (m[i] != ((i * 0x9e3779b97f4a7c15ULL) ^ inv))
                ++errors;
    }
    uint64_t ms = (ticks() - t0) * 1000U / freq();
    if (errors)
        row_dec(ROW_BODY + 3, "FAIL: BAD WORDS", errors, "");
    else
        star_fb_text_c(ROW_BODY + 3, "PASS: NO ERRORS", C_GREEN, C_BLACK);
    row_dec(ROW_BODY + 4, "TIME", ms, " MS");
    row_dec(ROW_BODY + 5, "SPEED", ms ? (RAMTEST_SIZE * 4 / 1024 / 1024) * 1000 / ms : 0, " MIB/S (MMU OFF)");
    wait_select();
}

static void diag_timer(void)
{
    page("TIMER TEST", HINT_BACK);
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (f));
    row_dec(ROW_BODY, "CNTFRQ_EL0", f, f ? " HZ" : " (26 MHZ ASSUMED)");
    star_fb_text_c(ROW_BODY + 1, "START A STOPWATCH WITH THE COUNT:", C_MUTED, C_BLACK);
    uint64_t t0 = ticks(), hz = freq();
    for (unsigned s = 0; s <= 10; ++s) {
        while (ticks() - t0 < hz * s)
            ;
        row_dec(ROW_BODY + 3, "SECONDS", s, "");
    }
    star_fb_text_c(ROW_BODY + 5, "10 S HERE = 10 S ON THE STOPWATCH?", C_MUTED, C_BLACK);
    row_hex(ROW_BODY + 6, "CNTPCT", ticks(), 16);
    wait_select();
}

static void diagnostics(const struct star_info *in, const char *cmdline)
{
    unsigned sel = 0;
    for (;;) {
        page("DIAGNOSTICS", HINT_MENU);
        item(0, "SYSTEM INFO", C_WHITE);
        item(1, "DISPLAY COLOUR TEST", C_WHITE);
        item(2, "BUTTON TEST", C_WHITE);
        item(3, "PMIC / TOUCH RAILS", C_WHITE);
        item(4, "RAM TEST (16 MIB)", C_WHITE);
        item(5, "TIMER TEST (10 S)", C_WHITE);
        item(6, "BACK", C_MUTED);
        sel = choose(ROW_BODY, 7, sel);
        if (sel == 0) diag_sysinfo(in, cmdline);
        else if (sel == 1) diag_display();
        else if (sel == 2) diag_buttons();
        else if (sel == 3) diag_pmic();
        else if (sel == 4) diag_ram();
        else if (sel == 5) diag_timer();
        else return;
    }
}

static void label(unsigned i, const char *name, const char *value, uint32_t fg)
{
    struct sb s;
    sb_init(&s);
    sb_s(&s, name);
    sb_s(&s, value);
    item(i, s.b, fg);
}

static void boot_options(void)
{
    unsigned sel = 0;
    for (;;) {
        page("BOOT OPTIONS", "VOL: MOVE   POWER: CHANGE");
        label(0, "CPU CORES       ", opt.cpus == 4 ? "4" : opt.cpus == 2 ? "2" : "1", C_WHITE);
        label(1, "ROOT FILESYSTEM ", opt.rw && !opt.multi ? "READ-WRITE" : "READ-ONLY", C_WHITE);
        label(2, "VERBOSE BOOT    ", opt.verbose ? "ON" : "OFF", C_WHITE);
        label(3, "TOUCHSCREEN     ", opt.touch ? "ON" : "OFF", C_WHITE);
        label(4, "TOUCH POWER-ON  ", opt.touchpwr ? "ON" : "OFF", C_WHITE);
        item(5, "BACK", C_MUTED);
        star_fb_text_c(ROW_BODY + 7, "APPLIED WHEN YOU PICK", C_MUTED, C_BLACK);
        star_fb_text_c(ROW_BODY + 8, "BOOT IOS WITH THESE SETTINGS.", C_MUTED, C_BLACK);
        if (opt.multi)
            star_fb_text_c(ROW_BODY + 9, "UFS MULTI-BLOCK FORCES READ-ONLY.", C_GOLD, C_BLACK);
        sel = choose(ROW_BODY, 6, sel);
        if (sel == 0) opt.cpus = opt.cpus == 4 ? 1 : opt.cpus == 1 ? 2 : 4;
        else if (sel == 1) opt.rw = !opt.rw;
        else if (sel == 2) opt.verbose = !opt.verbose;
        else if (sel == 3) opt.touch = !opt.touch;
        else if (sel == 4) opt.touchpwr = !opt.touchpwr;
        else return;
    }
}

/* Second press of Power arms a risky item; Volume cancels. */
static int confirm(const char *what)
{
    star_fb_text_c(ROW_BODY + 9, what, C_GOLD, C_BLACK);
    star_fb_text_c(ROW_BODY + 10, "POWER AGAIN: DO IT   VOL: CANCEL", C_RED, C_BLACK);
    int yes = (key_wait() & K_SELECT) != 0;
    star_fb_text(ROW_BODY + 9, "");
    star_fb_text(ROW_BODY + 10, "");
    return yes;
}

/* The kernel's touch-rail sequence, with the same voltage checks. */
static void risky_rails(void)
{
    page("TOUCH RAILS ON NOW (PMIC WRITE)", HINT_BACK);
    uint8_t l35 = 0, l43 = 0;
    int ok35 = pmic_read(1, 0x6c, &l35), ok43 = pmic_read(1, 0x74, &l43);
    star_fb_text_c(ROW_BODY, "BEFORE:", C_MUTED, C_BLACK);
    row_ldo(ROW_BODY + 1, "LDO35", ok35, l35, 700);
    row_ldo(ROW_BODY + 2, "LDO43", ok43, l43, 1800);
    if (!ok35 || !ok43 || ((l35 & 0x3fU) != 0x2cU && (l35 & 0x3fU) != 0) ||
        ((l43 & 0x3fU) != 0 && ((l43 & 0x3fU) < 0x28U || (l43 & 0x3fU) > 0x3cU))) {
        star_fb_text_c(ROW_BODY + 4, "UNEXPECTED VOLTAGE: NOT WRITING", C_RED, C_BLACK);
        wait_select();
        return;
    }
    int ok = pmic_write(1, 0x6c, 0xec) && pmic_write(1, 0x74, 0xf0);
    ok35 = pmic_read(1, 0x6c, &l35);
    ok43 = pmic_read(1, 0x74, &l43);
    star_fb_text_c(ROW_BODY + 4, ok ? "AFTER (WRITE OK):" : "AFTER (WRITE FAILED):",
                   ok ? C_GREEN : C_RED, C_BLACK);
    row_ldo(ROW_BODY + 5, "LDO35", ok35, l35, 700);
    row_ldo(ROW_BODY + 6, "LDO43", ok43, l43, 1800);
    wait_select();
}

static void risky(void)
{
    unsigned sel = 0;
    for (;;) {
        page("RISKY EXPERIMENTS", HINT_MENU);
        label(0, "UFS MULTI-BLOCK I/O  ", opt.multi ? "[ARMED]" : "OFF", opt.multi ? C_RED : C_WHITE);
        label(1, "CPU CORE PROBE       ", opt.smpprobe ? "[ARMED]" : "OFF", opt.smpprobe ? C_RED : C_WHITE);
        item(2, "TOUCH RAILS ON NOW (PMIC)", C_WHITE);
        item(3, "BACK", C_MUTED);
        star_fb_text_c(ROW_BODY + 6, "ARMED ITEMS APPLY TO THE NEXT BOOT", C_MUTED, C_BLACK);
        star_fb_text_c(ROW_BODY + 7, "FROM BOOT IOS WITH THESE SETTINGS.", C_MUTED, C_BLACK);
        sel = choose(ROW_BODY, 4, sel);
        if (sel == 0) {
            if (opt.multi)
                opt.multi = 0;
            else if (confirm("MAY PANIC. ROOT GOES READ-ONLY."))
                opt.multi = 1;
        } else if (sel == 1) {
            if (opt.smpprobe)
                opt.smpprobe = 0;
            else if (confirm("PARKS CORES: REBOOT AFTER IT."))
                opt.smpprobe = 1;
        } else if (sel == 2) {
            if (confirm("WRITES LDO35 + LDO43 NOW."))
                risky_rails();
        } else {
            return;
        }
    }
}

/* Returns 1 to boot iOS, 0 for the boot menu. */
static int engineer(const struct star_info *in, char *cmdline, unsigned len)
{
    unsigned sel = 0;
    star_fb_row_base(0);
    for (;;) {
        page("MAIN MENU", HINT_MENU);
        item(0, "DIAGNOSTICS", C_WHITE);
        item(1, "BOOT OPTIONS", C_WHITE);
        item(2, "RISKY EXPERIMENTS", C_GOLD);
        item(3, "BOOT IOS WITH THESE SETTINGS", C_GREEN);
        item(4, "BACK TO BOOT MENU", C_MUTED);
        item(5, "REBOOT TO RECOVERY (TWRP)", C_MUTED);
        item(6, "REBOOT TO DOWNLOAD MODE", C_MUTED);
        sel = choose(ROW_BODY, 7, sel);
        if (sel == 0) {
            diagnostics(in, cmdline);
        } else if (sel == 1) {
            boot_options();
        } else if (sel == 2) {
            risky();
        } else if (sel == 3) {
            opt_apply(cmdline, len);
            return 1;
        } else if (sel == 4) {
            return 0;
        } else {
            star_fb_fill(C_BLACK);
            star_fb_text(ROW_BODY, sel == 5 ? "REBOOTING TO RECOVERY" : "REBOOTING TO DOWNLOAD MODE");
            reboot(sel == 5 ? INFORM_RECOVERY : INFORM_DOWNLOAD);
        }
    }
}

/* ---- boot menu -------------------------------------------------------- */

/* In the gap under Samsung's logo: overlay row_base 23, rows 0-4. */
#define MENU_ROW_BASE 23U

int star_menu(const struct star_info *in, char *cmdline, unsigned len)
{
    unsigned sel = 0;
    opt_load(cmdline);
    for (;;) {
        star_fb_row_base(MENU_ROW_BASE);
        item(0, "IOS 16 (STARBITE)", C_WHITE);
        item(1, "RECOVERY (TWRP)", C_WHITE);
        item(2, "DOWNLOAD MODE", C_WHITE);
        item(3, "STARBITE ENGINEER", C_GOLD);
        star_fb_text_c(4, "VOL: MOVE   POWER: SELECT", C_MUTED, C_BLACK);
        sel = choose(0, 4, sel);
        if (sel == 0) {
            break;
        } else if (sel == 1 || sel == 2) {
            star_fb_text(4, sel == 1 ? "REBOOTING TO RECOVERY" : "REBOOTING TO DOWNLOAD MODE");
            reboot(sel == 1 ? INFORM_RECOVERY : INFORM_DOWNLOAD);
        } else {
            int boot = engineer(in, cmdline, len);
            star_fb_fill(C_BLACK);
            star_overlay_redraw();
            if (boot)
                break;
        }
    }
    star_fb_row_base(MENU_ROW_BASE);
    for (unsigned r = 0; r < 5; ++r)
        star_fb_text(r, "");
    /* The Starbite card belongs to the boot selector only: clear it
     * (overlay.mjs BAND_TOP .. the footer) before iOS boots. */
    star_fb_fill_rows(1880, 2500, C_BLACK);
    return 1;
}
