/* SPDX-License-Identifier: GPL-2.0-only
 * Engineer OS: start-up, the chrome (status bar, header, tab bar), the screens of eng.h, the tab
 * roots and the navigation between them.
 *
 * Keys: Volume moves, Power selects (on release), Power held 0.6 s or short Bixby goes back, long
 * Bixby goes to the tab's root (at a root: the Dashboard). The Dashboard lists the other tabs as
 * rows, so Volume + Power alone reach every page. Touch: tap, drag and fling to scroll, swipe
 * right to go back, the tab bar switches tabs. Leaving Engineer OS is a reboot (Dashboard or
 * Tools > Exit to the boot menu; Tools > Reboot and power for the other modes).
 */
#include "eng.h"

struct eng E;
int cur_tab;
const char *const tab_label[NTABS] = { "Home", "Tests", "Power", "Clocks", "Storage", "Starbite", "Logs", "Tools" };
static const char *const tab_title[NTABS] = { "Dashboard", "Tests", "Power", "Clocks", "Storage", "Starbite",
                                              "Logs", "Tools" };

const char *tab_title_of(int t)
{
    return t >= 0 && t < NTABS ? tab_title[t] : "";
}

/* ---- status bar --------------------------------------------------------------------------------- */

static uint64_t rtc_epoch, rtc_at_ms, bat_at_ms;
static int rtc_ok;
static struct sb_bat bat;

static void clock_text(char *out)
{
    struct ub u;
    ub_init(&u);
    if (rtc_ok) {
        uint64_t t = rtc_epoch + (sb_ms() - rtc_at_ms) / 1000U;
        unsigned s = (unsigned)(t % 86400U);
        ub_dec2(&u, s / 3600U);
        ub_c(&u, ':');
        ub_dec2(&u, (s / 60U) % 60U);
    } else {
        uint64_t s = sb_ms() / 1000U;
        ub_s(&u, "up ");
        ub_dec(&u, s / 60U);
        ub_c(&u, ':');
        ub_dec2(&u, (unsigned)(s % 60U));
    }
    sb_strcpy(out, 24, u.b);
}

void status_refresh(int force)
{
    uint64_t now = sb_ms();
    if (!rtc_at_ms || now - rtc_at_ms > 60000) {
        rtc_ok = sb_rtc_epoch(&rtc_epoch);
        rtc_at_ms = now;
    }
    if (!bat_at_ms || now - bat_at_ms > 10000) {
        (void)sb_bat_read(&bat);
        bat_at_ms = now;
    }
    struct ui_status s;
    clock_text(s.left);
    s.mid[0] = 0;
    s.mid_c = 0;
    if (armed_tier() > TIER0) {
        struct ub u;
        uint64_t left = armed_left_ms() / 1000U;
        ub_init(&u);
        ub_s(&u, "ARMED ");
        ub_s(&u, sb_fence_is_dry_run() ? U_DOT " DRY RUN " U_DOT " " : U_DOT " ");
        ub_dec(&u, left / 60U);
        ub_c(&u, ':');
        ub_dec2(&u, (unsigned)(left % 60U));
        sb_strcpy(s.mid, sizeof(s.mid), u.b);
        s.mid_c = C_RED;
    } else {
        sb_strcpy(s.mid, sizeof(s.mid), sb_fence_is_dry_run() ? "STARBITE LAB " U_DOT " DRY RUN" : "STARBITE LAB");
        s.mid_c = sb_fence_is_dry_run() ? C_ORANGE : C_L3;
    }
    struct ub r;
    ub_init(&r);
    if (bat.ok) {
        ub_dec(&r, (uint64_t)bat.soc_pct);
        ub_c(&r, '%');
    }
    sb_strcpy(s.right, sizeof(s.right), r.b);
    s.bat = bat.ok ? bat.soc_pct : -1;
    s.charging = bat.ok && bat.input;
    ui_status_draw(&s, force);
}

static uint64_t tick_last;

static void tick(void)
{
    uint64_t now = sb_ms();
    armed_tick();
    if (now - tick_last >= 1000) {
        tick_last = now;
        dash_sample();                  /* the dashboard's and the battery graph's history */
        status_refresh(0);
        ui_present();
    }
}

/* ---- navigation ------------------------------------------------------------------------------------ */

void screen_push(const char *title)
{
    if (E.depth < 8)
        E.titles[E.depth] = title;
    E.depth++;
}

void screen_pop(void)
{
    if (E.depth > 0)
        E.depth--;
}

const char *back_label(void)
{
    if (E.depth >= 2 && E.depth - 2 < 8)
        return E.titles[E.depth - 2];
    return E.depth >= 1 ? tab_title[cur_tab] : "";
}

void chrome(const char *title, const char *note, uint32_t note_c)
{
    int armed = armed_tier() > TIER0;
    ui_guard(armed ? 14 : 0);
    if (armed)
        ui_border_draw(C_RED, 14);
    status_refresh(1);
    ui_header_draw(title, E.depth ? back_label() : "", note && note[0] ? note : armed ? "ARMED" : "",
                   note && note[0] ? note_c : C_RED);
    ui_tabbar_draw(tab_label, NTABS, cur_tab);
    E.redraw = 0;
}

/* Shared by every screen: 1 when the screen should close. */
int nav_event(const struct ui_ev *e)
{
    switch (e->type) {
    case EV_BACK:
    case EV_SWIPE_R:
        return 1;
    case EV_HOME:
        E.home = 1;
        return 1;
    case EV_TAP: {
        int t = ui_tabbar_hit(NTABS, e->x, e->y);
        if (t >= 0) {
            E.home = 1;
            E.tab_req = t;
            return 1;
        }
        if (E.depth && ui_header_back_hit(e->x, e->y))
            return 1;
        return 0;
    }
    default:
        return 0;
    }
}

/* ---- page building (em.c's kv / row / tv API) -------------------------------------------------- */

static struct page *cur;

void kv_hdr(const char *t) { page_hdr(cur, t); }
void kv_add(const char *k, const char *v) { page_kv(cur, k, v, C_L1); }
void kv_addc(const char *k, const char *v, uint32_t c) { page_kv(cur, k, v, c); }
void kv_mono(const char *t) { page_mono(cur, t, C_L1); }
void kv_monoc(const char *t, uint32_t c) { page_mono(cur, t, c); }
void kv_big(const char *k, const char *v) { page_big(cur, k, v, C_L1); }
void kv_bigc(const char *k, const char *v, uint32_t c) { page_big(cur, k, v, c); }
void kv_note(const char *t) { page_note(cur, t, C_L3); }
void kv_gap(int h) { page_gap(cur, h); }
void kv_custom(int h, pi_draw_fn draw, uintptr_t arg, uint32_t hash) { page_custom(cur, h, draw, arg, hash); }

void kv_hex(const char *k, uint64_t v, unsigned digits)
{
    struct ub u;
    ub_init(&u);
    ub_hex(&u, v, digits);
    kv_add(k, u.b);
}

void kv_dec(const char *k, uint64_t v, const char *unit)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, v);
    if (unit)
        ub_s(&u, unit);
    kv_add(k, u.b);
}

static char row_titles[64][72];

void row_add(const char *t, const char *s, uint32_t f)
{
    int r = page_row(cur, t, s, f);
    if (r >= 0 && r < 64)
        sb_strcpy(row_titles[r], sizeof(row_titles[r]), t);
}

void row_value(const char *t, const char *v, uint32_t f)
{
    row_add(t, v, f | R_VALUE);
}

const char *row_title(int i)
{
    return i >= 0 && i < 64 ? row_titles[i] : "";
}

/* text viewer lines, kept for text_screen */
#define TV_CAP 96000
#define TV_LINES 1300
#define TV_COLS 66
static char tv_buf[TV_CAP];
static uint32_t tv_off[TV_LINES];
static int tv_n;
static uint32_t tv_len;

void tv_reset(void)
{
    tv_n = 0;
    tv_len = 0;
}

void tv_line(const char *s)
{
    unsigned n = sb_strlen(s), pos = 0;
    do {
        if (tv_n >= TV_LINES || tv_len + TV_COLS * 3 + 2 >= TV_CAP)
            return;
        tv_off[tv_n++] = tv_len;
        unsigned k = 0;
        while (k < TV_COLS && pos < n) {
            unsigned char c = (unsigned char)s[pos];
            unsigned len = c < 0xc0 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;   /* keep UTF-8 whole */
            for (unsigned b = 0; b < len && pos < n; ++b)
                tv_buf[tv_len++] = s[pos++];
            ++k;
        }
        tv_buf[tv_len++] = 0;
    } while (pos < n);
}

int dialog(const char *title, const char *l1, const char *l2, const char *ok)
{
    return ui_dialog(title, l1, l2, ok, C_STAR);
}

int dialog_danger(const char *title, const char *l1, const char *l2, const char *ok)
{
    return ui_dialog(title, l1, l2, ok, C_RED);
}

/* ---- screens -------------------------------------------------------------------------------------- */

static void build_with(struct page *p, void (*fill)(void))
{
    struct page *save = cur;
    cur = p;
    page_begin(p);
    fill();
    page_end(p);
    cur = save;
}

void eng_build_page(struct page *p, void (*fill)(void))
{
    build_with(p, fill);
}

void show_now(const char *title, void (*fill)(void))
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_VIEW_Y1);
    screen_push(title);
    chrome(title, "RUNNING", C_ORANGE);
    build_with(&p, fill);
    ui_present();
    screen_pop();
    page_free(&p);
}

static void key_scroll(struct page *p, int dir)
{
    page_set_scroll(p, p->scroll + dir * 560);
}

void info_screen(const char *title, void (*fill)(void), unsigned refresh_ms)
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_VIEW_Y1);
    screen_push(title);
    chrome(title, refresh_ms ? "LIVE" : "", C_GREEN);
    build_with(&p, fill);
    ui_present();
    uint64_t next = sb_ms() + refresh_ms;
    for (;;) {
        struct ui_ev e;
        ui_poll(&e, p.fling_v ? 8 : 40);
        if (E.redraw) {
            chrome(title, refresh_ms ? "LIVE" : "", C_GREEN);
            page_redraw(&p);
        }
        if (nav_event(&e))
            break;
        if (!page_scroll_event(&p, &e)) {
            if (e.type == EV_UP)
                key_scroll(&p, -1);
            else if (e.type == EV_DOWN)
                key_scroll(&p, 1);
            else if (e.type == EV_SELECT)
                break;
        }
        page_fling_step(&p);
        if (refresh_ms && sb_ms() >= next && !p.fling_v) {
            build_with(&p, fill);
            next = sb_ms() + refresh_ms;
        }
        ui_present();
    }
    screen_pop();
    page_free(&p);
}

/* Lists: the screen of a tab root and of every list page. root: no back, the tab's rows. */
static void list_loop(const char *title, void (*build)(void), int (*act)(int), int root)
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_VIEW_Y1);
    if (!root)
        screen_push(title);
    chrome(title, "", 0);
    build_with(&p, build);
    page_set_focus(&p, -1);
    ui_present();
    uint64_t next_live = sb_ms() + 1000;
    for (;;) {
        struct ui_ev e;
        int pick = -1;
        ui_poll(&e, p.fling_v ? 8 : 40);
        if (E.redraw) {
            chrome(title, "", 0);
            page_redraw(&p);
        }
        if (root && (e.type == EV_HOME || e.type == EV_BACK)) {
            if (cur_tab != 0) {
                E.tab_req = 0;
                break;
            }
            continue;
        }
        if (nav_event(&e))
            break;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if (e.type == EV_UP || e.type == EV_DOWN) {
            int f = p.focus < 0 ? page_next_row(&p, -1, 1) : page_next_row(&p, p.focus, e.type == EV_UP ? -1 : 1);
            page_set_focus(&p, f);
            page_keep_visible(&p, f);
        } else if (e.type == EV_SELECT) {
            pick = p.focus < 0 ? page_next_row(&p, -1, 1) : p.focus;
            if (p.focus < 0) {
                page_set_focus(&p, pick);
                pick = -1;
            }
        } else if (e.type == EV_PRESS) {
            page_press(&p, page_hit(&p, e.x, e.y, 0), 1);
        } else if (e.type == EV_TOUCH_UP) {
            page_press(&p, -1, 0);
        } else if (e.type == EV_TAP) {
            int r = page_hit(&p, e.x, e.y, 0);
            if (r >= 0) {
                page_press(&p, r, 1);
                ui_present();
                pick = r;
            }
        }
        page_fling_step(&p);
        if (pick >= 0) {
            ui_present();
            int close = act(pick);
            page_press(&p, -1, 0);
            if (close || E.home || E.tab_req >= 0)
                break;
            chrome(title, "", 0);
            p.drawn = 0;
            build_with(&p, build);
            ui_input_reset();
        } else if (root && cur_tab == 0 && sb_ms() >= next_live && !p.fling_v) {
            build_with(&p, build);                  /* live tiles: only changed tiles redraw */
            next_live = sb_ms() + 1000;
        }
        ui_present();
    }
    if (!root)
        screen_pop();
    page_free(&p);
}

void list_screen(const char *title, void (*build)(void), int (*act)(int))
{
    list_loop(title, build, act, 0);
}

static void tv_fill(void)
{
    for (int i = 0; i < tv_n; ++i)
        kv_mono(tv_buf + tv_off[i]);
    if (!tv_n)
        kv_note("(empty)");
}

void text_screen(const char *title)
{
    info_screen(title, tv_fill, 0);
}

/* ---- tab roots ----------------------------------------------------------------------------------- */

static int root_act(int row)
{
    tab_open(cur_tab, row);
    return 0;
}

static void root_build(void)
{
    tab_build(cur_tab);
}

static void welcome(void)
{
    ui_noclip();
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    ui_text_c(F_TITLE, UI_W / 2, 1380, "Engineer OS", C_L1);
    ui_text_c(F_ROW, UI_W / 2, 1470, "Starbite " U_DOT " Galaxy S9 " U_DOT " libsb", C_L2);
    ui_present();
}

void sb_main(void)
{
    if (!ui_init())
        sb_crash_show(ui_init_why(), 0);
    welcome();
    sb_logf("Engineer OS ", ENG_VERSION);
    if (!sb_touch_start())
        sb_logf("touch: ", sb_tp.why ? sb_tp.why : "?");
    tests_load_history();
    (void)cat_load();
    dash_sample();
    ui_tick = tick;
    ui_input_reset();
    E.tab_req = -1;
    cur_tab = 0;
    for (;;) {
        E.home = 0;
        E.depth = 0;
        list_loop(tab_title[cur_tab], root_build, root_act, 1);
        if (E.tab_req >= 0) {
            cur_tab = E.tab_req;
            E.tab_req = -1;
        }
    }
}
