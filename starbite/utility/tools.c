/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS tools (docs/UTILITY-OS.md): Measure (USB charger tester, ruler at the panel's real ppi,
 * colour fields for dead pixels) and Everyday (clock, stopwatch, timer). Read-only on the hardware: the
 * charger and fuel gauge are read with Lab's helpers (libsb max77705.c), nothing is written. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "ut.h"
#include "calc.h"

#define PPI 570U                                  /* Galaxy S9: 1440 x 2960, 5.8 in (Samsung: 570 ppi) */

static int back_event(const struct ui_ev *e)
{
    return e->type == EV_BACK || e->type == EV_SWIPE_R || e->type == EV_HOME ||
           (e->type == EV_TAP && ui_header_back_hit(e->x, e->y));
}

/* ---- Measure: the charger tester page with two full-screen tools --------------------------------- */
static const char *chg_state(int d)
{
    switch (d) {
    case 0x0: return "pre-charge";
    case 0x1: return "fast charge (CC)";
    case 0x2: return "fast charge (CV)";
    case 0x3: return "top-off";
    case 0x4: return "done (full)";
    case 0x5: return "timer fault";
    case 0x6: return "thermal suspend";
    case 0x7: return "off (input invalid)";
    case 0x8: return "off";
    case 0xa: return "reverse boost (OTG)";
    case 0xb: return "off (watchdog expired)";
    default: return "?";
    }
}

static int row_ruler, row_colours;

static void measure_fill(struct page *p)
{
    struct sb_bat b;
    struct ub u;
    uint8_t c;
    page_hdr(p, "USB charger");
    if (!sb_bat_read(&b)) {
        page_kv(p, "Charger", "the MAX77705 did not answer", C_ORANGE);
    } else {
        page_kv(p, "Input", b.input == 2 ? "wireless" : b.input == 1 ? "USB cable" : "none (on battery)",
                b.input ? C_GREEN : C_L1);
        if (sb_chg_read(0xb3, &c)) {
            unsigned d = (c >> 5) & 3U;
            page_kv(p, "VBUS", d == 3 ? "valid" : d == 2 ? "over voltage" : d == 1 ? "low" : "none",
                    d == 3 ? C_GREEN : d == 2 ? C_RED : C_L1);
        }
        page_kv(p, "Charge state", chg_state(b.chg_dtls), b.chg_dtls >= 0 && b.chg_dtls <= 2 ? C_GREEN : C_L1);
        ub_init(&u);
        ub_fix(&u, (uint64_t)b.soc_x10 * 100U, 1);
        ub_s(&u, " %");
        page_kv(p, "Battery", u.b, C_L1);
        ub_init(&u);
        ub_fix(&u, (uint64_t)b.mv, 3);
        ub_s(&u, " V");
        page_kv(p, "Voltage", u.b, C_L1);
        ub_init(&u);
        ub_sdec(&u, b.avg_ma);
        ub_s(&u, " mA (now ");
        ub_sdec(&u, b.ma);
        ub_s(&u, ")");
        page_kv(p, b.avg_ma >= 0 ? "Current in" : "Current out", u.b, b.avg_ma > 0 ? C_GREEN : C_L1);
        ub_init(&u);
        int64_t mw = (int64_t)b.avg_mv * b.avg_ma / 1000;
        ub_sdec(&u, mw);
        ub_s(&u, " mW");
        page_kv(p, "Power", u.b, C_L1);
        if (sb_chg_read(0xc0, &c)) {
            unsigned r = c & 0x7fU;
            ub_init(&u);
            ub_dec(&u, r <= 3 ? 100U : r >= 0x7f ? 3200U : (r + 1U) * 25U);
            ub_s(&u, " mA");
            page_kv(p, "Input current limit", u.b, C_L2);
        }
        if (sb_chg_read(0xb9, &c)) {
            ub_init(&u);
            ub_dec(&u, (c & 0x3fU) <= 2 ? 100U : (c & 0x3fU) * 50U);
            ub_s(&u, " mA");
            page_kv(p, "Charge current limit", u.b, C_L2);
        }
    }
    page_note(p, "Battery current as the fuel gauge measures it (+ into the battery). Read only: nothing here "
                 "changes the charger.", C_L3);
    page_hdr(p, "Screen");
    row_ruler = page_button(p, "Ruler (cm / in)", C_STAR);
    row_colours = page_button(p, "Colour fields (dead pixels)", C_STAR);
}

static void ruler(void)
{
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    const unsigned pxmm100 = PPI * 1000U / 254U;  /* px per mm x 100: 2244 = 22.44 px */
    for (unsigned mm = 0;; ++mm) {
        int y = (int)(mm * pxmm100 / 100U);
        if (y >= UI_H)
            break;
        int len = mm % 10 == 0 ? 160 : mm % 5 == 0 ? 110 : 70;
        ui_fill(0, y, len, 4, C_L1);
        if (mm % 10 == 0 && mm) {
            struct ub u;
            ub_init(&u);
            ub_dec(&u, mm / 10);
            ui_text(F_TILE, 180, y + 22, u.b, C_L1);
        }
    }
    for (unsigned e = 0;; ++e) {                   /* eighths of an inch on the right */
        int y = (int)(e * PPI / 8U);
        if (y >= UI_H)
            break;
        int len = e % 8 == 0 ? 160 : e % 4 == 0 ? 120 : e % 2 == 0 ? 90 : 60;
        ui_fill(UI_W - len, y, len, 4, C_STAR);
        if (e % 8 == 0 && e) {
            struct ub u;
            ub_init(&u);
            ub_dec(&u, e / 8);
            ub_s(&u, " in");
            ui_text_r(F_TILE, UI_W - 180, y + 22, u.b, C_STAR);
        }
    }
    ui_text(F_DETAIL, 420, UI_H / 2, "cm from the top edge (left)", C_L2);
    ui_text(F_DETAIL, 420, UI_H / 2 + 70, "inches (right), 570 ppi", C_L2);
    ui_text(F_SMALL, 420, UI_H / 2 + 160, "Power held or Bixby: back", C_L3);
    ui_dirty_all();
    ui_present();
    for (;;) {
        struct ui_ev e;
        ui_poll(&e, 20);
        ut_service();
        if (back_event(&e))
            break;
    }
}

static void colours(void)
{
    static const uint32_t col[] = { 0xffffffffU, 0xff000000U, 0xffff0000U, 0xff00ff00U, 0xff0000ffU, 0xff808080U };
    static const char *const nm[] = { "white", "black", "red", "green", "blue", "grey" };
    unsigned i = 0;
    uint64_t label_until = 0;
    for (;;) {
        ui_fill(0, 0, UI_W, UI_H, col[i]);
        label_until = sb_ms() + 1200;
        ui_text(F_TITLE_S, 80, UI_H - 200, nm[i], i == 0 ? 0xff000000U : 0xffffffffU);
        ui_text(F_SMALL, 80, UI_H - 120, "tap or Volume: next   Bixby: back", i == 0 ? 0xff404040U : 0xffc0c0c0U);
        ui_dirty_all();
        ui_present();
        int labelled = 1, next = 0;
        for (;;) {
            struct ui_ev e;
            ui_poll(&e, 20);
            ut_service();
            if (back_event(&e))
                return;
            if (e.type == EV_TAP || e.type == EV_UP || e.type == EV_DOWN || e.type == EV_SELECT) {
                i = e.type == EV_UP ? (i + 5) % 6 : (i + 1) % 6;
                next = 1;
                break;
            }
            if (labelled && sb_ms() > label_until) {  /* the label goes: nothing covers a pixel */
                ui_fill(0, UI_H - 300, UI_W, 300, col[i]);
                ui_dirty(0, UI_H - 300, UI_W, 300);
                ui_present();
                labelled = 0;
            }
        }
        (void)next;
    }
}

void ut_measure(void)
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_H);
    uint64_t next = 0;
    int focus = -1;
    ut_awake = 1;                                     /* live readings, the ruler, the colour fields: watched */
    for (;;) {
        if (sb_ms() >= next) {
            ui_fill(0, 0, UI_W, UI_VIEW_Y0, C_BG);
            ui_header_draw("Measure", "Utility", "LIVE", C_GREEN);
            ut_status_bar();
            page_begin(&p);
            measure_fill(&p);
            page_end(&p);
            page_set_focus(&p, focus);
            page_redraw(&p);
            ui_present();
            next = sb_ms() + 700;
        }
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        if (back_event(&e))
            break;
        int pick = -1;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if (e.type == EV_UP || e.type == EV_DOWN) {
            focus = focus == row_ruler && e.type == EV_DOWN ? row_colours : row_ruler;
            page_set_focus(&p, focus);
            page_keep_visible(&p, focus);
            ui_present();
        } else if (e.type == EV_SELECT && focus >= 0) {
            pick = focus;
        } else if (e.type == EV_TAP) {
            pick = page_hit(&p, e.x, e.y, 0);
        }
        if (pick >= 0 && (pick == row_ruler || pick == row_colours)) {
            if (pick == row_ruler)
                ruler();
            else
                colours();
            ui_input_reset();
            ui_fill(0, 0, UI_W, UI_H, C_BG);
            next = 0;
        }
        page_fling_step(&p);
    }
    ut_awake = 0;
    page_free(&p);
}

/* ---- Everyday: clock, stopwatch, timer ---------------------------------------------------------- */
static void hms(struct ub *u, uint64_t ms, int tenths)
{
    uint64_t s = ms / 1000;
    if (s >= 3600) {
        ub_dec(u, s / 3600);
        ub_c(u, ':');
    }
    ub_dec2(u, (unsigned)(s / 60 % 60));
    ub_c(u, ':');
    ub_dec2(u, (unsigned)(s % 60));
    if (tenths) {
        ub_c(u, '.');
        ub_dec(u, ms / 100 % 10);
    }
}

#define EV_ROW_Y(i) (UI_VIEW_Y0 + 40 + (i) * 580)
#define EV_ROW_H 520

static struct { int run; uint64_t t0, acc; } sw;                 /* stopwatch */
static struct { int run, done; uint64_t end, left, set; } tm = { 0, 0, 0, 0, 5 * 60 * 1000 };

static void card(int i, const char *title, const char *big, const char *hint, uint32_t c, int focused)
{
    int y = EV_ROW_Y(i);
    if (focused)
        ui_round(52, y - 8, UI_W - 104, EV_ROW_H + 16, 56, C_STAR);
    ui_round(60, y, UI_W - 120, EV_ROW_H, 48, focused ? C_CARD2 : C_CARD);
    ui_text(F_HEAD, 110, y + 80, title, C_L2);
    ui_text(F_BIG, 110, y + 260, big, c);
    ui_text(F_SMALL, 110, y + 450, hint, C_L3);
}

static void everyday_draw(int focus)
{
    struct ub u;
    char big[16], small[96];
    ui_fill(0, UI_VIEW_Y0, UI_W, UI_H - UI_VIEW_Y0, C_BG);
    ut_clock_card(big, sizeof(big), small, sizeof(small));     /* local time (clock.c, tz.c) */
    card(0, "CLOCK", big, small, C_L1, focus == 0);
    uint64_t now = sb_ms();
    ub_init(&u);
    hms(&u, sw.acc + (sw.run ? now - sw.t0 : 0), 1);
    card(1, "STOPWATCH", u.b, sw.run ? "Power: stop" : "Power: start   tap: reset (when stopped)",
         sw.run ? C_GREEN : C_L1, focus == 1);
    uint64_t left = tm.run ? (tm.end > now ? tm.end - now : 0) : tm.left ? tm.left : tm.set;
    ub_init(&u);
    hms(&u, left + 999, 0);
    int flash = tm.done && (now / 400) % 2;
    card(2, tm.done ? "TIMER: TIME!" : "TIMER", u.b,
         tm.run ? "Power: pause" : "Power: start   tap: +1 min   Bixby: back",
         tm.done ? (flash ? C_ORANGE : C_RED) : tm.run ? C_GREEN : C_L1, focus == 2);
    card(3, "CALCULATOR", "Open", "scientific (degrees) and programmer (64-bit)", C_STAR, focus == 3);
    ui_dirty(0, UI_VIEW_Y0, UI_W, UI_H - UI_VIEW_Y0);
}

void ut_everyday(void)
{
    int focus = 1;
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    ui_header_draw("Everyday", "Utility", "", 0);
    uint64_t next = 0;
    for (;;) {
        uint64_t now = sb_ms();
        if (tm.run && now >= tm.end) {
            tm.run = 0;
            tm.left = 0;
            tm.done = 1;
        }
        /* a running stopwatch or timer, or a finished timer for 10 minutes: no screen saver, no idle return */
        ut_awake = sw.run || tm.run || (tm.done && now < tm.end + 10U * 60U * 1000U);
        if (now >= next) {
            everyday_draw(focus);
            ut_status_bar();
            ui_present();
            next = now + 100;
        }
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        if (back_event(&e))
            break;
        if (e.type == EV_UP || e.type == EV_DOWN) {
            focus = (focus + (e.type == EV_UP ? 3 : 1)) % 4;
        } else if (e.type == EV_SELECT || e.type == EV_TAP) {
            int which = focus;
            if (e.type == EV_TAP)
                for (int i = 0; i < 4; ++i)
                    if (e.y >= EV_ROW_Y(i) && e.y < EV_ROW_Y(i) + EV_ROW_H)
                        which = focus = i;
            if (which == 0) {
                ut_awake = 0;                         /* recomputed by this loop; not held on other pages */
                ut_world();
                ui_input_reset();
                ui_fill(0, 0, UI_W, UI_H, C_BG);
                ui_header_draw("Everyday", "Utility", "", 0);
            } else if (which == 3) {
                ut_awake = 0;
                ut_calc();
                ui_input_reset();
                ui_fill(0, 0, UI_W, UI_H, C_BG);
                ui_header_draw("Everyday", "Utility", "", 0);
            } else if (which == 1) {
                if (e.type == EV_TAP && !sw.run && sw.acc) {
                    sw.acc = 0;                       /* tap when stopped: reset */
                } else if (sw.run) {
                    sw.acc += now - sw.t0;
                    sw.run = 0;
                } else {
                    sw.t0 = now;
                    sw.run = 1;
                }
            } else if (which == 2) {
                if (tm.done) {
                    tm.done = 0;
                    tm.left = 0;
                } else if (e.type == EV_TAP && !tm.run) {
                    tm.set = tm.left ? tm.left + 60000 : tm.set + 60000;   /* +1 min */
                    if (tm.set > 99ULL * 60 * 1000)
                        tm.set = 60000;
                    tm.left = 0;
                } else if (tm.run) {
                    tm.left = tm.end > now ? tm.end - now : 0;
                    tm.run = 0;
                } else {
                    tm.end = now + (tm.left ? tm.left : tm.set);
                    tm.run = 1;
                }
            }
            next = 0;
        }
    }
    ut_awake = 0;
}

/* ---- Files (UO4, first part): SHA-256 + CRC32 of a partition or the card, read only --------------- */
#include "hash.h"
#include "ufs.h"
#include "sd.h"

static int files_rows[GPT_MAX + 1];
static int files_nrows;
static int row_browse = -1;

/* ---- the card browser (read only) ---- */
#include "exfat.h"
static struct exfat cfs;
static uint8_t *cbuf;
static int cfs_state;                                    /* 0 not tried, 1 mounted, < 0 the error */
#define BMAX 120
static struct exfat_ent bents[BMAX];
static struct exfat_ent bstack[8];
static int bdepth, bn, btotal, brow[BMAX];

static int card_rd(uint64_t sector, uint32_t count, void *dst) { return sd_read(sector, count, (uintptr_t)dst); }

static int card_mount(void)
{
    if (cfs_state > 0)
        return 1;
    if (!cbuf)
        cbuf = sb_alloc(1U << 17);
    uint64_t start;
    sd_allow();
    if (!cbuf || !sd_up()) {
        cfs_state = -1;
        return 0;
    }
    int r = exfat_find_volume(card_rd, cbuf, &start);
    if (!r)
        r = exfat_mount(&cfs, card_rd, start, cbuf, 1U << 17);
    cfs_state = r ? (r < 0 ? r : -9) - 10 : 1;
    return cfs_state > 0;
}

/* the card's exFAT for other screens (the macro pad): 0 when not readable */
struct exfat *ut_card(void)
{
    return card_mount() ? &cfs : 0;
}

static void browse_fill(struct page *p)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "/");
    for (int i = 0; i < bdepth; ++i) {
        ub_s(&u, bstack[i].name);
        ub_s(&u, "/");
    }
    page_hdr(p, u.b);
    if (!card_mount()) {
        page_note(p, "The card is not readable (no card, or not exFAT).", C_ORANGE);
        bn = 0;
        return;
    }
    bn = exfat_list(&cfs, bdepth ? &bstack[bdepth - 1] : 0, bents, BMAX, &btotal);
    if (bn < 0)
        bn = 0;
    for (int i = 0; i < bn; ++i) {
        ub_init(&u);
        if (bents[i].dir)
            ub_s(&u, "folder");
        else
            ub_size(&u, bents[i].size);
        brow[i] = page_row(p, bents[i].name, u.b, 0);
    }
    if (btotal > bn) {
        ub_init(&u);
        ub_dec(&u, (uint64_t)(btotal - bn));
        ub_s(&u, " more entries not shown");
        page_note(p, u.b, C_L3);
    }
    if (!bn)
        page_note(p, "Empty folder.", C_L3);
    page_note(p, "Read only. Pick a file to read it (text, or its bytes) or hash it; Bixby goes up.", C_L3);
}

/* ---- one card file: its details, a text or hex view of its start, the hash ---- */
#define VIEW_MAX (64U << 10)                           /* text: the first 64 KiB */
#define VIEW_HEX (8U << 10)                            /* bytes: the first 8 KiB, 8 per line */
#define VIEW_W   (UI_W - 2 * 40 - 60)                  /* sbui_page's mono line width, with room */
static uint8_t *vbuf;

/* text: no NUL and at most 2 % other control bytes in the start */
static int is_text(const uint8_t *b, uint32_t n)
{
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!b[i])
            return 0;
        if (b[i] < 0x20 && b[i] != '\n' && b[i] != '\r' && b[i] != '\t' && b[i] != 0x0c)
            ++bad;
    }
    return bad * 50U <= n;
}

static int view_text(struct page *p, const uint8_t *b, uint32_t n)
{
    static char line[1024];
    int rows = 0;
    uint32_t i = 0;
    while (i < n && p->n < PAGE_MAX - 8) {
        unsigned k = 0;
        while (i < n && b[i] != '\n' && k + 8 < sizeof(line)) {
            uint8_t c = b[i++];
            if (c == '\r')
                continue;
            if (c == '\t') {
                do
                    line[k++] = ' ';
                while (k & 3U);
            } else {
                line[k++] = c < 0x20 ? '.' : (char)c;
            }
        }
        if (i < n && b[i] == '\n')
            ++i;
        line[k] = 0;
        const char *s = line;
        do {                                          /* wrap at the panel width, whole UTF-8 characters */
            unsigned f = ui_text_fit(F_MONO, s, VIEW_W);
            if (f > 170)
                f = 170;
            while (f && ((uint8_t)s[f] & 0xc0U) == 0x80U)
                --f;
            if (!f && *s)
                f = 1;
            char row[176];
            for (unsigned j = 0; j < f; ++j)
                row[j] = s[j];
            row[f] = 0;
            page_mono(p, row, C_L1);
            ++rows;
            s += f;
        } while (*s && p->n < PAGE_MAX - 8);
    }
    return rows;
}

static int view_hex(struct page *p, const uint8_t *b, uint32_t n)
{
    static const char hx[] = "0123456789abcdef";
    int rows = 0;
    for (uint32_t o = 0; o < n && p->n < PAGE_MAX - 8; o += 8) {
        char row[64];
        unsigned k = 0;
        for (int sh = 28; sh >= 0; sh -= 4)
            row[k++] = hx[(o >> sh) & 15U];
        row[k++] = ' ';
        row[k++] = ' ';
        for (uint32_t j = 0; j < 8; ++j) {
            row[k++] = o + j < n ? hx[b[o + j] >> 4] : ' ';
            row[k++] = o + j < n ? hx[b[o + j] & 15U] : ' ';
            row[k++] = ' ';
        }
        row[k++] = ' ';
        for (uint32_t j = 0; j < 8 && o + j < n; ++j)
            row[k++] = b[o + j] >= 0x20 && b[o + j] < 0x7f ? (char)b[o + j] : '.';
        row[k] = 0;
        page_mono(p, row, C_L1);
        ++rows;
    }
    return rows;
}

/* 1 when the hash started (Files shows its progress) */
static int ut_file(const struct exfat_ent *f)
{
    if (!vbuf)
        vbuf = sb_alloc(VIEW_MAX);
    uint32_t n = f->size < VIEW_MAX ? (uint32_t)f->size : VIEW_MAX;
    int err = !vbuf ? -1 : n ? exfat_read_chain(&cfs, f->first, f->nofat, 0, vbuf, n) : 0;
    int text = !err && is_text(vbuf, n < 4096U ? n : 4096U);
    if (!err && !text && n > VIEW_HEX)
        n = VIEW_HEX;
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_H);
    int focus = -1, redo = 1, hrow = -1;
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    for (;;) {
        if (redo) {
            ui_fill(0, 0, UI_W, UI_VIEW_Y0, C_BG);
            ui_header_draw("File", "Card files", "READ ONLY", C_L2);
            ut_status_bar();
            page_begin(&p);
            struct ub u;
            page_hdr(&p, "File");
            page_kv(&p, "Name", f->name, 0);
            ub_init(&u);
            ub_size(&u, f->size);
            if (f->size >= 1024U) {
                ub_s(&u, " (");
                ub_dec(&u, f->size);
                ub_s(&u, " bytes)");
            }
            page_kv(&p, "Size", u.b, 0);
            page_kv(&p, "Stored", f->nofat ? "one run (no FAT chain)" : "FAT chain", 0);
            hrow = page_button(&p, hash_job.state == HASH_RUNNING ? "A hash is running" : "SHA-256 + CRC32 of the file",
                               hash_job.state == HASH_RUNNING ? C_L3 : C_STAR);
            if (err) {
                page_note(&p, "The card did not read.", C_ORANGE);
            } else if (!n) {
                page_note(&p, "Empty file.", C_L3);
            } else {
                page_hdr(&p, text ? "Text" : "Bytes (hex)");
                int rows = text ? view_text(&p, vbuf, n) : view_hex(&p, vbuf, n);
                if (f->size > n || p.n >= PAGE_MAX - 8) {
                    ub_init(&u);
                    ub_s(&u, "The start of the file: ");
                    ub_dec(&u, (uint64_t)rows);
                    ub_s(&u, " lines.");
                    page_note(&p, u.b, C_L3);
                }
            }
            page_end(&p);
            page_set_focus(&p, focus);
            page_redraw(&p);
            ui_present();
            redo = 0;
        }
        struct ui_ev e;
        ui_poll(&e, 20);
        ut_service();
        if (e.type == EV_BACK || e.type == EV_HOME || e.type == EV_SWIPE_R || (e.type == EV_TAP && ui_header_back_hit(e.x, e.y)))
            break;
        int pick = -1;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if (e.type == EV_UP || e.type == EV_DOWN) {
            focus = focus < 0 ? hrow : -1;
            page_set_focus(&p, focus);
            ui_present();
        } else if (e.type == EV_SELECT && focus >= 0) {
            pick = focus;
        } else if (e.type == EV_TAP) {
            pick = page_hit(&p, e.x, e.y, 0);
        }
        if (pick == hrow && hrow >= 0 && hash_job.state != HASH_RUNNING) {
            (void)hash_start_file(&cfs, f);
            page_free(&p);
            return 1;
        }
        page_fling_step(&p);
    }
    page_free(&p);
    return 0;
}

/* ---- a partition or the whole card: size, its first 8 KiB as hex (only what the USB drives may offer), the hash ---- */
/* 1 when the hash started */
static int ut_part(int gi)                               /* gi: GPT index, -1 the card */
{
    if (!vbuf)
        vbuf = sb_alloc(VIEW_MAX);
    const char *name = gi < 0 ? "microSD card" : gpt_name[gi];
    uint64_t bytes = gi < 0 ? sd_sectors_hint() * 512ULL : (gpt_last[gi] - gpt_first[gi] + 1) * 4096ULL;
    int show = gi < 0 || ut_ufs_allowed(gpt_name[gi]);
    int err = 0;
    if (gi < 0)
        sd_allow();
    if (show && vbuf)
        err = gi < 0 ? (sd_up() ? sd_read(0, VIEW_HEX / 512U, (uintptr_t)vbuf) : -1)
                     : ufs_read_to(gpt_first[gi], VIEW_HEX / 4096U, (uintptr_t)vbuf);
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_H);
    int focus = -1, redo = 1, hrow = -1;
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    for (;;) {
        if (redo) {
            ui_fill(0, 0, UI_W, UI_VIEW_Y0, C_BG);
            ui_header_draw(gi < 0 ? "Card" : "Partition", "Files", "READ ONLY", C_L2);
            ut_status_bar();
            page_begin(&p);
            struct ub u;
            page_hdr(&p, gi < 0 ? "Card" : "Partition");
            page_kv(&p, "Name", name, 0);
            ub_init(&u);
            ub_size(&u, bytes);
            page_kv(&p, "Size", u.b, 0);
            if (gi >= 0) {
                ub_init(&u);
                ub_dec(&u, gpt_first[gi]);
                ub_s(&u, " .. ");
                ub_dec(&u, gpt_last[gi]);
                page_kv(&p, "UFS blocks (4 KiB)", u.b, 0);
            }
            hrow = page_button(&p, hash_job.state == HASH_RUNNING ? "A hash is running" : "SHA-256 + CRC32 of all of it",
                               hash_job.state == HASH_RUNNING ? C_L3 : C_STAR);
            if (!show) {
                page_note(&p, "Bytes are shown only for what the USB drives may offer (BOOT, SYSTEM, VENDOR, ODM, WINESP, "
                              "WINDOWS, A16DATA, USERDATA) and the card.", C_L3);
                page_note(&p, "EFS, PARAM and the like hold the phone's identity: hash only.", C_L3);
            } else if (err || !vbuf) {
                page_note(&p, "It did not read.", C_ORANGE);
            } else {
                page_hdr(&p, "The first 8 KiB (hex)");
                (void)view_hex(&p, vbuf, VIEW_HEX);
            }
            page_end(&p);
            page_set_focus(&p, focus);
            page_redraw(&p);
            ui_present();
            redo = 0;
        }
        struct ui_ev e;
        ui_poll(&e, 20);
        ut_service();
        if (e.type == EV_BACK || e.type == EV_HOME || e.type == EV_SWIPE_R || (e.type == EV_TAP && ui_header_back_hit(e.x, e.y)))
            break;
        int pick = -1;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if (e.type == EV_UP || e.type == EV_DOWN) {
            focus = focus < 0 ? hrow : -1;
            page_set_focus(&p, focus);
            ui_present();
        } else if (e.type == EV_SELECT && focus >= 0) {
            pick = focus;
        } else if (e.type == EV_TAP) {
            pick = page_hit(&p, e.x, e.y, 0);
        }
        if (pick == hrow && hrow >= 0 && hash_job.state != HASH_RUNNING) {
            if (gi < 0)
                (void)hash_start_card(0);
            else
                (void)hash_start_part(gpt_name[gi], 0);
            page_free(&p);
            return 1;
        }
        page_fling_step(&p);
    }
    page_free(&p);
    return 0;
}

static void ut_browse(void)
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_H);
    int focus = -1, redo = 1;
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    for (;;) {
        if (redo) {
            ui_fill(0, 0, UI_W, UI_VIEW_Y0, C_BG);
            ui_header_draw("Card files", "Files", "READ ONLY", C_L2);
            ut_status_bar();
            page_begin(&p);
            browse_fill(&p);
            page_end(&p);
            page_set_focus(&p, focus);
            page_redraw(&p);
            ui_present();
            redo = 0;
        }
        struct ui_ev e;
        ui_poll(&e, 20);
        ut_service();
        if (e.type == EV_BACK || e.type == EV_SWIPE_R || (e.type == EV_TAP && ui_header_back_hit(e.x, e.y))) {
            if (!bdepth)
                break;
            --bdepth;
            focus = -1;
            page_set_scroll(&p, 0);
            redo = 1;
            continue;
        }
        if (e.type == EV_HOME)
            break;
        int pick = -1;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if ((e.type == EV_UP || e.type == EV_DOWN) && bn) {
            int i = 0;
            while (i < bn && brow[i] != focus)
                ++i;
            i = focus < 0 ? 0 : (i + (e.type == EV_UP ? bn - 1 : 1)) % bn;
            focus = brow[i];
            page_set_focus(&p, focus);
            page_keep_visible(&p, focus);
            ui_present();
        } else if (e.type == EV_SELECT && focus >= 0) {
            pick = focus;
        } else if (e.type == EV_TAP) {
            pick = page_hit(&p, e.x, e.y, 0);
        }
        for (int i = 0; pick >= 0 && i < bn; ++i) {
            if (brow[i] != pick)
                continue;
            if (bents[i].dir && bdepth < 8) {
                bstack[bdepth++] = bents[i];
                focus = -1;
                page_set_scroll(&p, 0);
                redo = 1;
            } else if (!bents[i].dir) {
                struct exfat_ent f = bents[i];
                if (ut_file(&f)) {
                    page_free(&p);
                    return;                              /* Files shows the hash's progress */
                }
                ui_input_reset();
                ui_fill(0, 0, UI_W, UI_H, C_BG);
                redo = 1;
            }
        }
        page_fling_step(&p);
    }
    page_free(&p);
}

static void files_fill(struct page *p)
{
    struct ub u;
    const struct hash_job *j = &hash_job;
    page_hdr(p, "Hash (read only)");
    if (j->state == HASH_IDLE) {
        page_note(p, "Pick a partition or the card for its size, first bytes and SHA-256 + CRC32, all read only. "
                     "The shell's `hash NAME BYTES` hashes just the first BYTES.", C_L2);
    } else {
        page_kv(p, "Target", j->name, C_L1);
        ub_init(&u);
        ub_size(&u, j->done);
        ub_s(&u, " of ");
        ub_size(&u, j->bytes);
        page_kv(p, "Read", u.b, C_L1);
        uint64_t ms = (j->state == HASH_RUNNING ? sb_ms() : j->t1) - j->t0;
        if (ms) {
            ub_init(&u);
            ub_dec(&u, j->done / 1000 / ms);
            ub_s(&u, " MB/s");
            if (j->state == HASH_RUNNING && j->done) {
                ub_s(&u, ", ");
                ub_dec(&u, (j->bytes - j->done) / (j->done / ms + 1) / 1000);
                ub_s(&u, " s left");
            }
            page_kv(p, "Speed", u.b, C_L2);
        }
        if (j->state == HASH_DONE) {
            char hex[65];
            hash_hex(hex);
            page_kv(p, "State", "done", C_GREEN);
            char a[33], b[33];
            for (int i = 0; i < 32; ++i) {
                a[i] = hex[i];
                b[i] = hex[32 + i];
            }
            a[32] = b[32] = 0;
            page_mono(p, "SHA-256", C_L2);
            page_mono(p, a, C_L1);
            page_mono(p, b, C_L1);
            ub_init(&u);
            ub_hexn(&u, j->crc, 8);
            page_kv(p, "CRC32", u.b, C_L1);
        } else {
            page_kv(p, "State", j->state == HASH_RUNNING ? "hashing" : j->state == HASH_FAILED ? "read error" : "cancelled",
                    j->state == HASH_RUNNING ? C_STAR : C_ORANGE);
        }
    }
    page_hdr(p, "Card files");
    row_browse = page_button(p, "Browse the card (read only)", C_STAR);
    page_hdr(p, "Targets");
    files_nrows = 0;
    uint64_t card = sd_sectors_hint() * 512ULL;
    ub_init(&u);
    ub_size(&u, card);
    files_rows[files_nrows++] = page_row(p, "microSD card", card ? u.b : "-", 0);
    if (ufs_up() && gpt_load() > 0) {
        for (int i = 0; i < gpt_n && files_nrows <= GPT_MAX; ++i) {
            ub_init(&u);
            ub_size(&u, (gpt_last[i] - gpt_first[i] + 1) * 4096ULL);
            files_rows[files_nrows++] = page_row(p, gpt_name[i], u.b, 0);
        }
    } else {
        page_note(p, "UFS: not readable", C_ORANGE);
    }
}

void ut_files(void)
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_H);
    uint64_t next = 0;
    int focus = -1;
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    for (;;) {
        if (sb_ms() >= next) {
            ui_header_draw("Files", "Utility", hash_job.state == HASH_RUNNING ? "HASHING" : "",
                           hash_job.state == HASH_RUNNING ? C_STAR : 0);
            ut_status_bar();
            page_begin(&p);
            files_fill(&p);
            page_end(&p);
            page_set_focus(&p, focus);
            page_redraw(&p);
            ui_present();
            next = sb_ms() + 500;
        }
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        if (back_event(&e))
            break;
        int pick = -1;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if (e.type == EV_UP || e.type == EV_DOWN) {
            int i = 0;
            if (focus == row_browse)
                i = e.type == EV_DOWN ? 0 : files_nrows - 1;
            else {
                while (i < files_nrows && files_rows[i] != focus)
                    ++i;
                i = focus < 0 ? -1 : i + (e.type == EV_UP ? -1 : 1);
            }
            focus = i < 0 || i >= files_nrows ? row_browse : files_rows[i];
            page_set_focus(&p, focus);
            page_keep_visible(&p, focus);
            ui_present();
        } else if (e.type == EV_SELECT && focus >= 0) {
            pick = focus;
        } else if (e.type == EV_TAP) {
            pick = page_hit(&p, e.x, e.y, 0);
        }
        if (pick >= 0 && pick == row_browse) {
            ut_browse();
            ui_input_reset();
            ui_fill(0, 0, UI_W, UI_H, C_BG);
            next = 0;
            pick = -1;
        }
        for (int i = 0; pick >= 0 && i < files_nrows; ++i)
            if (files_rows[i] == pick) {
                (void)ut_part(i - 1);                 /* its page: the bytes and the hash button */
                ui_input_reset();
                ui_fill(0, 0, UI_W, UI_H, C_BG);
                next = 0;
                break;
            }
        page_fling_step(&p);
    }
    page_free(&p);
}
