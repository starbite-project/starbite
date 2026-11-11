/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite firmware layer, the loader side (sbfw.h, docs/STARBITE-FIRMWARE-LAYER.md).
 *
 * The per-OS page: drawn with ui.c on the framebuffer, keys from hw_keys() (the Mac's
 * `fastboot oem key` too: the background USB link is served while the page waits). Every check
 * here only reads: partitions through ufs_read into UFS_BUF (non-cacheable), copied out. Every
 * result line also goes to pmsg ("STARBITE fw: ..."), so `fastboot oem log pmsg` shows it.
 *
 * The debug console: at High / Highest the bottom of the panel shows the newest pmsg lines;
 * the menu's renderer is told to leave those rows alone (g7_set_reserve), so the console is
 * only redrawn when the ring changes.
 */
#include <stdint.h>
#include "sbfw.h"
#include "sbfwfs.h"
#include "ui.h"
#include "hw.h"
#include "ufs.h"
#include "store.h"
#include "sbfmt.h"
#include "menu.h"
#include "catalog.h"
#include "fbbg.h"

void star_pmsg(const char *s);

/* ---- the test images' dead-man (SBFW_DEADMAN builds only) ----
 * A chain-loaded test loader (tools/menu/chain, pack.py --wdt) runs with cluster 1's watchdog armed.
 * These builds feed it from the menu's and the page's key loops (a hang anywhere stops the feeding:
 * the SoC resets within the stub's time), and stop it again before any OS starts, so an OS is never
 * reset by the test's dead-man. Other builds: no-ops. */
#define WDT1_CON 0x10060000UL
#define WDT1_DAT 0x10060004UL
#define WDT1_CNT 0x10060008UL
#define PMU_WDT_DIS 0x14060408UL
#define PMU_WDT_MSK 0x1406040CUL
static int wdt_armed(void)
{
    return (*(volatile uint32_t *)WDT1_CON & 0x21U) == 0x21U && !(*(volatile uint32_t *)PMU_WDT_MSK & (1U << 23));
}
void sbfw_wdt_feed(void)
{
#ifdef SBFW_DEADMAN
    if (wdt_armed())
        *(volatile uint32_t *)WDT1_CNT = *(volatile uint32_t *)WDT1_DAT;
#endif
}
void sbfw_wdt_release(void)
{
#ifdef SBFW_DEADMAN
    if (wdt_armed()) {
        *(volatile uint32_t *)WDT1_CON = 0;
        *(volatile uint32_t *)PMU_WDT_MSK |= 1U << 23;      /* masked again, as S-Boot leaves it */
        *(volatile uint32_t *)PMU_WDT_DIS |= 1U << 23;
        __asm__ volatile("dsb sy" ::: "memory");
        star_pmsg("STARBITE fw: test dead-man stopped before the hand-off\n");
    }
#else
    (void)wdt_armed;
#endif
}

/* ---- the debug level ---- */

unsigned sbfw_level(void)
{
    int valid = 0;
    struct star_settings *s = star_menu_settings(&valid);
    return s ? sbset_log(s) : 0;
}

static const char *level_name(unsigned l)
{
    return l == 0 ? "Off" : l == 1 ? "Normal" : l == 2 ? "High" : "Highest";
}

int sbfw_console_y0(unsigned level)
{
    return level >= 3 ? 1776 : level == 2 ? 2368 : UI_H;
}

/* ---- the console ---- */

#define PMSG_ZONE 0xFED14000UL
#define PMSG_ZONE_BYTES 0x4000U      /* the 16 KiB zone (crash.c ZONE): header, then the ring */
static char ctext[0x4000];
static uint32_t c_start = 0xffffffffU, c_size = 0xffffffffU;
static int c_y0_drawn = -1;

void sbfw_console_note(const char *a, const char *b)
{
    if (sbfw_level() < 3)
        return;
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, "STARBITE stage: ");
    fwb_s(&s, a);
    if (b && b[0]) {
        fwb_s(&s, " - ");
        fwb_s(&s, b);
    }
    fwb_s(&s, "\n");
    star_pmsg(s.b);
}

void sbfw_console_draw(int force)
{
    unsigned lvl = sbfw_level();
    int y0 = sbfw_console_y0(lvl);
    if (y0 >= UI_H)
        return;
    volatile const uint32_t *h = (volatile const uint32_t *)PMSG_ZONE;
    uint32_t st = h[1], sz = h[2];
    if (!force && st == c_start && sz == c_size && c_y0_drawn == y0)
        return;
    c_start = st;
    c_size = sz;
    c_y0_drawn = y0;
    if (!ui_init())
        return;
    ui_target(0);
    ui_clip(y0, UI_H);
    ui_fill(0, y0, UI_W, UI_H - y0, 0xff05070aU);
    ui_fill(0, y0, UI_W, 4, lvl >= 3 ? UI_ORANGE : UI_HOLO);
    int lh = ui_line(F_MONO), top = y0 + 8 + ui_ascent(F_SMALL);
    struct fwb t;
    fwb_init(&t);
    fwb_s(&t, "DEBUG  ");
    fwb_s(&t, level_name(lvl));
    fwb_s(&t, "  -  Starbite firmware layer  -  pmsg tail");
    ui_text(F_SMALL, 24, top, t.b, UI_TEXT2, 0xff05070aU);
    unsigned n = sb_ring_text((const uint8_t *)PMSG_ZONE, PMSG_ZONE_BYTES, ctext, sizeof(ctext) - 1);
    ctext[n] = 0;
    int rows = (UI_H - (top + 12) - 8) / lh;
    if (rows < 1) {
        ui_clip(0, UI_H);
        return;
    }
    /* the last `rows` lines (long lines cut at the panel's width) */
    int cw = ui_text_w(F_MONO, "0");
    unsigned maxc = cw > 0 ? (unsigned)((UI_W - 48) / cw) : 80;
    if (maxc > 150)
        maxc = 150;
    unsigned starts[128];
    unsigned ns = 0, i = n;
    while (i > 0 && ns < (unsigned)rows && ns < 128) {
        unsigned e = i;
        while (e > 0 && ctext[e - 1] == '\n')
            e--;
        unsigned b = e;
        while (b > 0 && ctext[b - 1] != '\n')
            b--;
        if (e > b)
            starts[ns++] = b;
        i = b;
    }
    int y = top + 12 + lh;
    for (int k = (int)ns - 1; k >= 0; --k) {
        char ln[152];
        unsigned m = 0;
        for (unsigned j = starts[k]; j < n && ctext[j] != '\n' && m < maxc; j++)
            ln[m++] = (ctext[j] >= 32 && ctext[j] < 127) ? ctext[j] : '.';
        ln[m] = 0;
        uint32_t c = UI_TEXT;
        if (ln[0] == 'P' && ln[1] == 'R')
            c = UI_RED;
        ui_text(F_MONO, 24, y, ln, c, 0xff05070aU);
        y += lh;
    }
    ui_clip(0, UI_H);
}

/* ---- partitions ---- */

struct pio {
    uint64_t first, last;
};
static uint8_t blk[4096];
static uint64_t blk_lba = ~0ULL;

static int prd(void *u, uint64_t off, void *buf, uint32_t len)
{
    const struct pio *p = (const struct pio *)u;
    uint8_t *d = (uint8_t *)buf;
    while (len) {
        uint64_t lba = p->first + off / 4096U;
        uint32_t in = (uint32_t)(off % 4096U), n = 4096U - in;
        if (n > len)
            n = len;
        if (lba > p->last)
            return -1;
        if (lba != blk_lba) {
            if (ufs_read(lba, 1))
                return -1;
            uintptr_t src = UFS_BUF;
            __asm__("" : "+r"(src));    /* (no folded "blk - UFS_BUF" constant: an absolute relocation) */
            for (unsigned i = 0; i < 4096; ++i)
                blk[i] = *(volatile const uint8_t *)(src + i);
            blk_lba = lba;
        }
        for (uint32_t i = 0; i < n; ++i)
            d[i] = blk[in + i];
        d += n;
        off += n;
        len -= n;
    }
    return 0;
}

static int part_io(const char *name, struct pio *p, struct fw_io *io)
{
    if (!gpt_load() || !gpt_find(name, &p->first, &p->last) || p->last < p->first)
        return 0;
    io->rd = prd;
    io->u = p;
    io->size = (p->last - p->first + 1) * 4096U;
    blk_lba = ~0ULL;
    return 1;
}

/* ---- results ---- */

#define NRES 64
static char res[NRES][120];
static unsigned nres;

static void res_line(void *u, const char *s)
{
    (void)u;
    if (nres < NRES) {
        unsigned i = 0;
        for (; s[i] && i < sizeof(res[0]) - 1; ++i)
            res[nres][i] = s[i];
        res[nres][i] = 0;
        nres++;
    }
    struct fwb t;
    fwb_init(&t);
    fwb_s(&t, "STARBITE fw: ");
    fwb_s(&t, s);
    fwb_s(&t, "\n");
    star_pmsg(t.b);
}
static void res_clear(void) { nres = 0; }

/* ---- checks ---- */

/* UI_SCRATCH's upper half (free while the menu runs). Through a register: a folded constant address
 * would become an absolute relocation (the loader Image must have none) */
static uint8_t *bcd_buf(void)
{
    uintptr_t a = 0x88C00000UL;
    __asm__("" : "+r"(a));
    return (uint8_t *)a;
}
#define BCD_BUF bcd_buf()
#define BCD_CAP (1U << 20)

static int is_windows(const struct cat_ent *e)
{
    return e && e->method == M_UEFI && e->key[0] == 'w' && e->key[1] == 'i' && e->key[2] == 'n';
}

static void check_gpt(struct fw_out *o, const char *n0, const char *n1)
{
    unsigned n = 2;
    if (!gpt_load()) {
        o->bad++;
        o->line(o->u, "PROBLEM: the UFS partition table could not be read");
        return;
    }
    for (unsigned i = 0; i < n; ++i) {
        uint64_t a, b;
        struct fwb s;
        const char *nm = i ? n1 : n0;
        fwb_init(&s);
        fwb_s(&s, "GPT: ");
        fwb_s(&s, nm);
        if (gpt_find(nm, &a, &b) && b >= a) {
            fwb_s(&s, " blocks ");
            fwb_u(&s, a);
            fwb_s(&s, "..");
            fwb_u(&s, b);
            fwb_s(&s, " (");
            fwb_mb(&s, (b - a + 1) * 4096U);
            fwb_s(&s, ")");
            o->line(o->u, s.b);
        } else {
            fwb_s(&s, " MISSING");
            o->bad++;
            o->line(o->u, s.b);
        }
    }
}

static void check_store(struct fw_out *o)
{
    int st = settings_status();
    o->line(o->u, st == 1 ? "Store: settings record valid" : st == 0 ? "Store: no settings record (defaults)" :
                  st == -1 ? "PROBLEM: store settings record bad (CRC / version)" : "Store: no store found");
    if (st == -1)
        o->bad++;
}

/* Windows: GPT, WINESP (FAT32 + the BCD), WINDOWS (NTFS) */
static void windows_health(struct fw_out *o, struct bcd_plan *plan, int want_plan)
{
    struct pio p;
    struct fw_io io;
    struct fat32 f;
    plan->ok = 0;
    if (!want_plan)
        check_gpt(o, "WINESP", "WINDOWS");
    if (!part_io("WINESP", &p, &io) || !fat32_mount(&f, &io)) {
        o->bad++;
        o->line(o->u, "PROBLEM: WINESP is not a readable FAT32 volume");
    } else {
        if (!want_plan)
            fat32_health(&f, o);
        uint32_t c, sz;
        if (!fat32_find(&f, "/EFI/Microsoft/Boot/BCD", &c, &sz)) {
            o->bad++;
            o->line(o->u, "PROBLEM: \\EFI\\Microsoft\\Boot\\BCD not found on WINESP");
        } else {
            int64_t n = fat32_read(&f, c, sz, BCD_BUF, BCD_CAP);
            if (n < 8192) {
                o->bad++;
                o->line(o->u, "PROBLEM: the BCD could not be read");
            } else {
                bcd_check(BCD_BUF, (uint32_t)n, plan, o);
                uint64_t po;
                if (plan->ok && fat32_map(&f, c, plan->off, &po)) {
                    struct fwb s;
                    fwb_init(&s);
                    fwb_s(&s, "Plan: WINESP block ");
                    fwb_u(&s, po / 4096U);
                    fwb_s(&s, " (UFS LBA ");
                    fwb_u(&s, p.first + po / 4096U);
                    fwb_s(&s, ") byte ");
                    fwb_u(&s, po % 4096U);
                    fwb_s(&s, ", 76 bytes");
                    o->line(o->u, s.b);
                    if (po % 4096U + 76U > 4096U) {
                        plan->ok = 0;
                        o->line(o->u, "Plan: the GUID crosses a block edge: not applied in place");
                    }
                }
            }
        }
    }
    if (want_plan)
        return;
    if (!part_io("WINDOWS", &p, &io)) {
        o->bad++;
        o->line(o->u, "PROBLEM: no WINDOWS partition");
    } else {
        ntfs_health(&io, o);
    }
    check_store(o);
}

static void generic_health(struct fw_out *o, const struct cat_ent *e)
{
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, "Files: ");
    fwb_s(&s, e ? e->file[0] : "-");
    o->line(o->u, s.b);
    ufs_up();
    o->line(o->u, gpt_load() ? "GPT: LU0 partition table read" : "PROBLEM: LU0 partition table unreadable");
    check_store(o);
    o->line(o->u, "(file-system checks for this OS: docs/STARBITE-FIRMWARE-LAYER.md 5, later)");
}

/* ---- the page ---- */

enum { IT_LEVEL, IT_HEALTH, IT_ADV, IT_BACK, IT_N };

static unsigned wait_key(void)
{
    /* every key must be up first, then a press (debounced) */
    for (unsigned g = 0; g < 2000 && hw_keys(); ++g) {
        (void)fbbg_poll(FBBG_MENU);
        sbfw_wdt_feed();
        hw_wait_us(10000);
    }
    for (;;) {
        sbfw_wdt_feed();
        if (fbbg_poll(FBBG_MENU) == FBBG_BOOT)
            return K_BIXBY;          /* the Mac placed an image: leave, the menu returns it */
        unsigned k = hw_keys();
        if (k) {
            hw_wait_us(40000);
            if (hw_keys() == k)
                return k;
        }
        hw_wait_us(10000);
    }
}

static void draw_page(const struct cat_ent *e, int win, int focus, const char *res_title)
{
    ui_target(0);
    ui_clip(0, UI_H);
    ui_fill(0, 0, UI_W, UI_H, UI_BG);
    ui_fill(0, 0, UI_W, 300, UI_BAR);
    ui_text(F_TITLE, 72, 170, e ? e->name : "System", UI_TEXT, UI_BAR);
    ui_text(F_SMALL, 72, 250, "Starbite firmware options", UI_HOLO, UI_BAR);
    int y = 360;
    for (int i = 0; i < IT_N; ++i) {
        if (i == IT_ADV && !win)
            continue;
        uint32_t bg = i == focus ? UI_FOCUS : UI_BG;
        if (i == focus)
            ui_round(36, y, UI_W - 72, 140, 24, UI_FOCUS);
        const char *lb = i == IT_LEVEL ? "Debug level (all systems)" : i == IT_HEALTH ? "Disk health (read-only)" :
                         i == IT_ADV ? "Advanced Startup next boot" : "Back";
        ui_text(F_BODY, 84, y + 88, lb, UI_TEXT, bg);
        const char *v = i == IT_LEVEL ? level_name(sbfw_level()) : i == IT_HEALTH ? "Check" :
                        i == IT_ADV ? "Plan (dry run)" : "";
        ui_text_r(F_BODY, UI_W - 84, y + 88, v, i == IT_LEVEL ? UI_HOLO : UI_TEXT2, bg);
        y += 150;
    }
    ui_fill(36, y + 20, UI_W - 72, 2, UI_DIV);
    int lh = ui_line(F_MONO), ry = y + 40 + ui_ascent(F_SMALL);
    ui_text(F_SMALL, 72, ry, res_title, UI_TEXT2, UI_BG);
    ry += 30 + lh;
    int cw = ui_text_w(F_MONO, "0");
    unsigned maxc = cw > 0 ? (unsigned)((UI_W - 120) / cw) : 80;
    for (unsigned i = 0; i < nres && ry < UI_H - 60; ++i) {
        char ln[120];
        unsigned m = 0;
        for (; res[i][m] && m < maxc && m < sizeof(ln) - 1; ++m)
            ln[m] = res[i][m];
        ln[m] = 0;
        uint32_t c = (res[i][0] == 'P' && res[i][1] == 'R') ? UI_RED : UI_TEXT;
        ui_text(F_MONO, 72, ry, ln, c, UI_BG);
        ry += lh;
    }
    ui_text_c(F_SMALL, UI_W / 2, UI_H - 60, "Volume: move   Power: choose   Bixby: back", UI_TEXT3, UI_BG);
}

int sbfw_page(int cat)
{
    const struct cat_ent *e = cat >= 0 ? cat_get((unsigned)cat) : 0;
    int win = is_windows(e), changed = 0;
    if (!ui_init())
        return 0;
    res_clear();
    const char *title = "Results";
    int focus = IT_LEVEL;
    struct fw_out o = { res_line, 0, 0 };
    star_pmsg("STARBITE fw: page open\n");
    for (;;) {
        draw_page(e, win, focus, title);
        unsigned k = wait_key();
        if (k & K_BIXBY)
            break;
        if (k & (K_UP | K_DOWN)) {
            int d = (k & K_DOWN) ? 1 : -1;
            do
                focus = (focus + d + IT_N) % IT_N;
            while (focus == IT_ADV && !win);
            continue;
        }
        if (!(k & K_POWER))
            continue;
        if (focus == IT_BACK)
            break;
        if (focus == IT_LEVEL) {
            int valid = 0;
            struct star_settings *s = star_menu_settings(&valid);
            unsigned l = (sbset_log(s) + 1U) % 4U;
            sbset_set_log(s, l);
            res_clear();
            o.bad = 0;
            int r = star_menu_settings_save();
            struct fwb t;
            fwb_init(&t);
            fwb_s(&t, "Debug level ");
            fwb_s(&t, level_name(l));
            fwb_s(&t, r ? ": NOT SAVED (store write failed)" : ": saved");
            res_line(0, t.b);
            res_line(0, l >= 2 ? "The bottom of the menu shows the loader's log from now on." :
                                 "The menu uses the whole screen.");
            title = "Debug level";
            changed = 1;
            continue;
        }
        res_clear();
        o.bad = 0;
        draw_page(e, win, focus, "Working...");
        struct bcd_plan plan;
        if (focus == IT_HEALTH) {
            title = "Disk health (read-only: nothing was written)";
            if (win)
                windows_health(&o, &plan, 0);
            else
                generic_health(&o, e);
        } else if (focus == IT_ADV) {
            title = "Advanced Startup: plan only (nothing was written)";
            windows_health(&o, &plan, 1);
            if (plan.ok) {
                res_line(0, "On apply: one boot into Windows Recovery (WinRE),");
                res_line(0, "then the loader puts the default back at its next start.");
                res_line(0, "NOTE: WinRE must match the woa firmware's build to boot.");
                res_line(0, "Applying is not built into this loader yet.");
            } else {
                res_line(0, "No plan: see the lines above.");
            }
        }
        struct fwb t;
        fwb_init(&t);
        fwb_s(&t, o.bad ? "Problems: " : "No problems found");
        if (o.bad)
            fwb_u(&t, (uint64_t)o.bad);
        res_line(0, t.b);
    }
    star_pmsg("STARBITE fw: page closed\n");
    return changed;
}
