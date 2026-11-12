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
#include "crash.h"
#include "post.h"
#include "sd.h"

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

/* a card range (sd:lba FIRST+COUNT in 512-byte sectors) through the same 4 KiB cache */
struct cio {
    uint64_t first, count;
};
static int crd(void *u, uint64_t off, void *buf, uint32_t len)
{
    const struct cio *c = (const struct cio *)u;
    uint8_t *d = (uint8_t *)buf;
    while (len) {
        uint64_t sec = (off / 4096U) * 8U;
        uint32_t in = (uint32_t)(off % 4096U), n = 4096U - in;
        if (n > len)
            n = len;
        if (sec + 8 > c->count)
            return -1;
        uint64_t key = (1ULL << 63) | (c->first + sec);
        if (key != blk_lba) {
            uint8_t *dst = blk;
            __asm__("" : "+r"(dst));
            if (sd_read(c->first + sec, 8, (uintptr_t)dst))
                return -1;
            blk_lba = key;
        }
        for (uint32_t i = 0; i < n; ++i)
            d[i] = blk[in + i];
        d += n;
        off += n;
        len -= n;
    }
    return 0;
}

static uint64_t parse_u(const char **p)
{
    uint64_t v = 0;
    while (**p >= '0' && **p <= '9')
        v = v * 10U + (uint64_t)(*(*p)++ - '0');
    return v;
}

/* The OS's root (the catalog's root field): its file system checked, read only. */
static void root_health(struct fw_out *o, const char *root)
{
    struct fw_io io;
    if (root[0] == 's' && root[1] == 'd' && root[2] == ':' && root[3] == 'l' && root[4] == 'b' && root[5] == 'a') {
        const char *p = root + 6;
        while (*p == ' ')
            ++p;
        static struct cio c;
        c.first = parse_u(&p);
        c.count = *p == '+' ? (++p, parse_u(&p)) : 0;
        sd_allow();
        if (!c.count || !sd_up()) {
            o->line(o->u, "PROBLEM: the card range does not read");
            ++o->bad;
            return;
        }
        io.rd = crd;
        io.u = &c;
        io.size = c.count * 512U;
        blk_lba = ~0ULL;
        o->line(o->u, "Root: a microSD card range");
        (void)fs_health(&io, o);
        return;
    }
    if (root[0] == 'u' && root[1] == 'f' && root[2] == 's' && root[3] == ':') {
        char name[24];
        unsigned k = 0;
        for (const char *p = root + 4; k + 1 < sizeof(name) &&
             ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'); ++p)
            name[k++] = *p;
        name[k] = 0;
        static struct pio pp;
        if (!k || !part_io(name, &pp, &io)) {
            o->line(o->u, "PROBLEM: the root partition is not in the GPT");
            ++o->bad;
            return;
        }
        struct fwb s;
        fwb_init(&s);
        fwb_s(&s, "Root: UFS partition ");
        fwb_s(&s, name);
        o->line(o->u, s.b);
        (void)fs_health(&io, o);
        return;
    }
    o->line(o->u, "Root: a file inside the card's exFAT, or RAM only: not checked here");
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
    if (e && e->root[0])
        root_health(o, e->root);
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

/* ---- Starbite OS as the boot menu: requests, shell mode, the table (section 10) ---------------- */

static int str_eq(const char *a, const char *b)
{
    unsigned k = 0;
    while (k < 23 && a[k] && a[k] == b[k])
        ++k;
    return k == 23 || a[k] == b[k];
}

static void fw_log(const char *a, const char *b)
{
    struct fwb s;
    fwb_init(&s);
    fwb_s(&s, "STARBITE fw: ");
    fwb_s(&s, a);
    if (b)
        fwb_s(&s, b);
    fwb_s(&s, "\n");
    star_pmsg(s.b);
}

/* the record as the store has it, and a save of a changed copy */
static struct star_settings fw_set;
static struct sbfw_rec fw_rec;
static int fw_have;

static void fw_load(void)
{
    if (!settings_load(&fw_set))
        sbset_defaults(&fw_set);
    (void)settings_fw(&fw_rec);
    fw_have = 1;
}

static void fw_save(const char *what)
{
    int r = settings_fw_save(&fw_set, &fw_rec);
    fw_log(what, r ? " (settings NOT saved)" : " (saved)");
    crash_reload_settings();
}

void sbfw_boot_start(void)
{
    fw_load();
    unsigned a = sbfw_boot_age(&fw_rec);
    if (!a)
        return;
    fw_save(a == SBFW_AGE_SEEN ? "request armed for this boot" : a == SBFW_AGE_EXPIRED ? "request expired unused"
            : a == SBFW_AGE_DONE ? "request done (one boot)"
            : a == SBFW_AGE_ROLLBACK ? "update rolled back: its start was never confirmed, the previous version stays"
                                     : "update expired unused: the previous version stays");
}

/* ---- the update slots (section 10.5) ---- */
static uint8_t slot_hdr[4096];
static int slot_state[3];               /* per slot: 0 not read, 1 valid header, -1 none */
static struct store_ent slot_ent[3];
static struct sbslot_info slot_inf[3];
static unsigned slot_last;              /* the slot the last "@slot" resolve used (0 none) */

static void ent_cpy(struct store_ent *d, const struct store_ent *s)   /* no struct copies: no libc memcpy */
{
    volatile const uint8_t *a = (volatile const uint8_t *)s;
    uint8_t *b = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof(*s); ++i)
        b[i] = a[i];
}

void sbfw_slot_reset(void)
{
    slot_last = 0;
}

int sbfw_slot_source(const char *key, struct store_ent *out)
{
    if (!fw_have)
        fw_load();
    unsigned slot = fw_rec.slot_active;
    if (fw_rec.req_action == SBFW_REQ_UPDATE && fw_rec.slot_try &&
        (fw_rec.req_state == SBFW_ST_ARMED || fw_rec.req_state == SBFW_ST_SEEN) && str_eq(fw_rec.req_key, key))
        slot = fw_rec.slot_try;
    if (slot != 1 && slot != 2)
        return 0;
    if (!slot_state[slot]) {
        slot_state[slot] = -1;
        struct store_ent e;
        uint8_t *buf = slot_hdr;
        __asm__("" : "+r"(buf));
        if (store_find(slot == 1 ? "sbos-slot-a" : "sbos-slot-b", &e) && (e.flags & SF_VOLATILE) &&
            !store_head(&e, (uintptr_t)buf) && sbslot_check(buf, e.size, key, &slot_inf[slot])) {
            ent_cpy(&slot_ent[slot], &e);
            slot_state[slot] = 1;
        }
    }
    if (slot_state[slot] < 0 || !str_eq(slot_inf[slot].key, key))
        return 0;
    ent_cpy(out, &slot_ent[slot]);
    out->lba += 1;                       /* the image after the header block */
    out->size = slot_inf[slot].size;
    out->crc = slot_inf[slot].crc;       /* store_load checks the image's CRC */
    out->flags = 0;
    slot_last = slot;
    return 1;
}

static int req_live(void)
{
    return fw_rec.req_state == SBFW_ST_ARMED || fw_rec.req_state == SBFW_ST_SEEN;
}

/* the boot-next mailbox (menu.c's): a pending key means the menu path starts it */
#define FW_NEXT_MAILBOX 0xFED17FE0UL
#define FW_NEXT2_MAGIC  0x21325458454e4253ULL

/* the shell entry: the record's shell key, else the first Starbite OS row the catalog has (FW4: today's phone
 * calls the 1.0 line starbite-next-gpu; the classic one is starbite-os) */
static int shell_row(void)
{
    if (fw_rec.shell_key[0])
        return cat_find(fw_rec.shell_key);
    int i = cat_find("starbite-next-gpu");
    if (i < 0)
        i = cat_find("starbite-next");
    if (i < 0)
        i = cat_find("starbite-os");
    return i;
}

/* the last chance for the plain menu: Volume Down held (or pressed) during this window; POST's own skip only
 * works while POST runs (and not at all with POST off) */
#ifndef SBFW_SHELL_GRACE_MS
#define SBFW_SHELL_GRACE_MS 700U
#endif
static int shell_grace(void)
{
    for (unsigned t = 0; t < SBFW_SHELL_GRACE_MS; t += 20U) {
        if (hw_keys() & K_DOWN)
            return 1;
        hw_wait_ms(20);
    }
    return 0;
}

unsigned sbfw_shell_flags(void)
{
    if (!fw_have)
        fw_load();
    return sbfw_shell_eff(fw_rec.fw_flags, SBFW_SHELL_DEFAULT);
}

int sbfw_shell_pick(int skip, const char **why)
{
    if (!fw_have)
        fw_load();
    unsigned fl = sbfw_shell_eff(fw_rec.fw_flags, SBFW_SHELL_DEFAULT);
    int i = (fl & SBFW_FLAG_SHELL) ? shell_row() : -1;
    const char *nr = 0;
    int ready = i >= 0 && star_entry_ready((unsigned)i, &nr);
    int next = *(volatile uint64_t *)FW_NEXT_MAILBOX == FW_NEXT2_MAGIC || fw_set.next_key[0];
    unsigned d = sbfw_shell_decide(fl, skip, fw_rec.req_state, fw_rec.req_action, crash_hold_autoboot(),
                                   next, i, ready);
    if (d == SBFW_SH_START && shell_grace())
        d = SBFW_SH_SKIP;
    *why = d == SBFW_SH_NOT_READY && nr ? nr : sbfw_shell_why(d);
    if (d == SBFW_SH_NO_SHELL) {
        fw_rec.req_state = SBFW_ST_DONE;
        fw_save("no-shell request: the plain menu this boot");
    }
    if (d == SBFW_SH_OFF)
        *why = 0;
    return d == SBFW_SH_START ? i : -1;
}

unsigned sbfw_req_for(const struct cat_ent *e)
{
    if (!fw_have)
        fw_load();
    if (!req_live() || !str_eq(fw_rec.req_key, e->key) || fw_rec.req_action == SBFW_REQ_NO_SHELL)
        return 0;
    return fw_rec.req_action;
}

static int has_store(const char *name)
{
    struct store_ent se;
    return store_find(name, &se);
}

unsigned sbfw_req_apply(unsigned req, struct cat_ent *e, char *xfrag, unsigned xcap)
{
    int ok = sbfw_req_fits(req, e->method);
    if (ok && req == SBFW_REQ_SINGLE)
        ok = sb_line_edit(e->cmdline, sizeof(e->cmdline), "single systemd.unit=rescue.target", 0) != 0;
    if (ok && req == SBFW_REQ_XNU_SINGLE)
        ok = sb_line_edit(xfrag, xcap, "-s", 0) != 0;
    if (ok && req == SBFW_REQ_PREV) {
        /* the previous kernel: "linux-prev.image" in the store */
        ok = has_store("linux-prev.image");
        if (ok) {
            const char *p = "linux-prev.image";
            unsigned k = 0;
            for (; p[k] && k + 1 < sizeof(e->file[0]); ++k)
                e->file[0][k] = p[k];
            e->file[0][k] = 0;
        }
    }
    if (!ok) {
        fw_rec.req_state = SBFW_ST_REFUSED;
        fw_save("request refused for this entry: ");
        fw_log("  request ", sbfw_req_name(req));
        return 0;
    }
    fw_log("request applied to this start: ", sbfw_req_name(req));
    return req;
}

static uint8_t tbl_lines[0x4000];
static void build_table(uint8_t *page, const char *key, unsigned req, unsigned why, unsigned level);

void sbfw_handoff(const char *key, unsigned req, unsigned why, unsigned level)
{
    if (req == SBFW_REQ_UPDATE && req_live() && fw_rec.req_action == req && slot_last != fw_rec.slot_try) {
        fw_rec.slot_try = 0;               /* the new image did not load (bad header / CRC): the old one runs */
        fw_rec.req_state = SBFW_ST_REFUSED;
        fw_save("update image did not load: the previous version started");
        req = 0;
    }
    if (req && req_live() && fw_rec.req_action == req) {
        fw_rec.req_state = SBFW_ST_APPLIED;
        fw_save("request applied, undone at the next start");
    }
    uint8_t *page = (uint8_t *)SBFW_TABLE_PA;
    __asm__("" : "+r"(page));
    build_table(page, key, req, why, level);
    __asm__ volatile("dsb sy" ::: "memory");
}

unsigned sbfw_table_preview(uint8_t *out, unsigned cap)
{
    if (cap < SBFW_TABLE_SIZE)
        return 0;
    if (!fw_have)
        fw_load();
    build_table(out, "", 0, SBFW_WHY_FASTBOOT, sbfw_level());
    return SBFW_TABLE_SIZE;
}

static void build_table(uint8_t *page, const char *key, unsigned req, unsigned why, unsigned level)
{
    struct sbfw_table_in in;
    uint8_t *q = (uint8_t *)&in;
    for (unsigned i = 0; i < sizeof(in); ++i)
        q[i] = 0;
    in.debug_level = level;
    in.req_action = req;
    in.key = key;
    int y0 = sbfw_console_y0(level);
    if (y0 < UI_H) {
        in.console_y0 = (uint32_t)y0;
        in.console_y1 = UI_H;
    }
    in.fb_base = 0xcc000000ULL;
    in.stride = 1440U * 4U;
    in.fb_fmt = 0;
    in.loader_version = 10;               /* SBFW ABI v1, loader 1.0 */
    in.boot_reason = why;
    int h = store_home();
    if (h >= 0) {
        in.store_medium = (uint32_t)h;
        in.store_flags = store_marked_home((unsigned)h) ? 1U : 0U;
        in.store_base = store_base((unsigned)h);
        in.store_capacity = store_capacity((unsigned)h);
    } else {
        in.store_medium = 0xffffffffU;
    }
    unsigned sev = 0;
    char ck[24];
    in.crash_kind = crash_last(&sev, ck, sizeof(ck));
    in.crash_sev = sev;
    in.crash_key = ck;
    const struct post_report *pr = post_last();
    if (pr && pr->magic == POST_MAGIC) {
        in.post_fails = pr->fails;
        in.post_warns = pr->warns;
    }
    in.settings_status = (uint32_t)settings_status();
    in.flags = (store_card_only() ? SBFW_TF_CARD_ONLY : 0U) | ((sbfw_shell_eff(fw_rec.fw_flags, SBFW_SHELL_DEFAULT) & SBFW_FLAG_SHELL) ? SBFW_TF_SHELL : 0U);
    unsigned n = sb_ring_text((const uint8_t *)PMSG_ZONE, PMSG_ZONE_BYTES, (char *)tbl_lines, sizeof(tbl_lines) - 1);
    sbfw_table_build(page, &in, (const char *)tbl_lines, n);
}
