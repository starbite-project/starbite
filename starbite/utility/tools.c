/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS tools (docs/UTILITY-OS.md): Measure (USB charger tester, ruler at the panel's real ppi,
 * colour fields for dead pixels) and Everyday (clock, stopwatch, timer). Read-only on the hardware: the
 * charger and fuel gauge are read with Lab's helpers (libsb max77705.c), nothing is written. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "ut.h"

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

#define EV_ROW_Y(i) (UI_VIEW_Y0 + 60 + (i) * 700)

static struct { int run; uint64_t t0, acc; } sw;                 /* stopwatch */
static struct { int run, done; uint64_t end, left, set; } tm = { 0, 0, 0, 0, 5 * 60 * 1000 };

static void card(int i, const char *title, const char *big, const char *hint, uint32_t c, int focused)
{
    int y = EV_ROW_Y(i);
    if (focused)
        ui_round(52, y - 8, UI_W - 104, 640 + 16, 56, C_STAR);
    ui_round(60, y, UI_W - 120, 640, 48, focused ? C_CARD2 : C_CARD);
    ui_text(F_HEAD, 110, y + 80, title, C_L2);
    ui_text(F_BIG, 110, y + 280, big, c);
    ui_text(F_SMALL, 110, y + 560, hint, C_L3);
}

static void everyday_draw(int focus)
{
    struct ub u;
    struct sb_rtc r;
    ui_fill(0, UI_VIEW_Y0, UI_W, UI_H - UI_VIEW_Y0, C_BG);
    sb_rtc_read(&r);
    ub_init(&u);
    if (r.ok) {
        ub_dec2(&u, r.h);
        ub_c(&u, ':');
        ub_dec2(&u, r.mi);
        ub_c(&u, ':');
        ub_dec2(&u, r.s);
    } else {
        ub_s(&u, "--:--:--");
    }
    struct ub d;
    ub_init(&d);
    if (r.ok) {
        ub_dec(&d, r.y);
        ub_c(&d, '-');
        ub_dec2(&d, r.mo);
        ub_c(&d, '-');
        ub_dec2(&d, r.d);
        ub_s(&d, "  (the phone's RTC, UTC)");
    } else {
        ub_s(&d, "the RTC did not answer");
    }
    card(0, "CLOCK", u.b, d.b, C_L1, focus == 0);
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
            focus = (focus + (e.type == EV_UP ? 2 : 1)) % 3;
        } else if (e.type == EV_SELECT || e.type == EV_TAP) {
            int which = focus;
            if (e.type == EV_TAP)
                for (int i = 0; i < 3; ++i)
                    if (e.y >= EV_ROW_Y(i) && e.y < EV_ROW_Y(i) + 640)
                        which = focus = i;
            if (which == 1) {
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
}

/* ---- Files (UO4, first part): SHA-256 + CRC32 of a partition or the card, read only --------------- */
#include "hash.h"
#include "ufs.h"
#include "sd.h"

static int files_rows[GPT_MAX + 1];
static int files_nrows;

static void files_fill(struct page *p)
{
    struct ub u;
    const struct hash_job *j = &hash_job;
    page_hdr(p, "Hash (read only)");
    if (j->state == HASH_IDLE) {
        page_note(p, "Pick a partition or the card: its SHA-256 and CRC32 are computed without writing anything. "
                     "The shell's `hash NAME BYTES` hashes only the first BYTES (to compare with an image).", C_L2);
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
            while (i < files_nrows && files_rows[i] != focus)
                ++i;
            i = focus < 0 ? 0 : (i + (e.type == EV_UP ? files_nrows - 1 : 1)) % files_nrows;
            focus = files_rows[i];
            page_set_focus(&p, focus);
            page_keep_visible(&p, focus);
            ui_present();
        } else if (e.type == EV_SELECT && focus >= 0) {
            pick = focus;
        } else if (e.type == EV_TAP) {
            pick = page_hit(&p, e.x, e.y, 0);
        }
        if (pick >= 0 && hash_job.state != HASH_RUNNING) {
            for (int i = 0; i < files_nrows; ++i)
                if (files_rows[i] == pick) {
                    if (i == 0)
                        (void)hash_start_card(0);
                    else
                        (void)hash_start_part(gpt_name[i - 1], 0);
                    next = 0;
                }
        }
        page_fling_step(&p);
    }
    page_free(&p);
}
