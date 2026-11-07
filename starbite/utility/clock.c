/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS clock: the status bar's local time and battery, and the world clock page (tz.c). The RTC
 * (S2MPS18, kept in UTC by Android and Linux) is read once a minute and carried on with the timer in between;
 * the fuel gauge every 10 s. Read only. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "ut.h"
#include "tz.h"

static const char *const wday_name[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const mon_name[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int64_t rtc_t;                                   /* the RTC's epoch at rtc_ms */
static uint64_t rtc_ms, rtc_try;
static int rtc_state;                                   /* 0 not read, 1 read, -1 never readable */

int ut_now(int64_t *t)
{
    uint64_t now = sb_ms();
    if (!rtc_state || now - rtc_try >= 60000U) {
        uint64_t e;
        rtc_try = now;
        if (sb_rtc_epoch(&e) && e > 1577836800ULL) {   /* after 2020: a clock someone set */
            rtc_t = (int64_t)e;
            rtc_ms = now;
            rtc_state = 1;
        } else if (rtc_state != 1) {
            rtc_state = -1;                            /* until a read works: the uptime instead */
        }
    }
    if (rtc_state != 1)
        return 0;
    *t = rtc_t + (int64_t)((now - rtc_ms) / 1000U);
    return 1;
}

static int bat_pct = -1, bat_in;
static uint64_t bat_ms;

static void bat_poll(void)
{
    uint64_t now = sb_ms();
    if (bat_ms && now - bat_ms < 10000U)
        return;
    bat_ms = now ? now : 1;
    struct sb_bat b;
    if (sb_bat_read(&b) && b.ok) {
        bat_pct = b.soc_pct;
        bat_in = b.input;
    }
}

void ut_clock_status(struct ub *u)
{
    int64_t t;
    if (ut_now(&t)) {
        struct tz_time l;
        tz_local(&tz_zones[0], t, &l);
        ub_dec2(u, (unsigned)l.h);
        ub_s(u, ":");
        ub_dec2(u, (unsigned)l.mi);
        ub_s(u, " ");
        ub_s(u, l.abbr);
    } else {
        ub_dec(u, sb_ms() / 60000U);
        ub_s(u, " MIN");
    }
    bat_poll();
    if (bat_pct >= 0) {
        ub_s(u, "   ");
        ub_dec(u, (uint64_t)bat_pct);
        ub_s(u, bat_in ? "% CHARGING" : "%");
    }
}

/* "Sat 7 Nov" */
static void day_text(struct ub *u, const struct tz_time *l)
{
    ub_s(u, wday_name[l->wday]);
    ub_s(u, " ");
    ub_dec(u, (uint64_t)l->d);
    ub_s(u, " ");
    ub_s(u, mon_name[l->mo - 1]);
}

void ut_clock_card(char *big, unsigned big_n, char *small, unsigned small_n)
{
    struct ub u;
    int64_t t;
    if (!ut_now(&t)) {
        sb_strcpy(big, big_n, "--:--:--");
        sb_strcpy(small, small_n, "the RTC did not read");
        return;
    }
    struct tz_time l;
    tz_local(&tz_zones[0], t, &l);
    ub_init(&u);
    ub_dec2(&u, (unsigned)l.h);
    ub_s(&u, ":");
    ub_dec2(&u, (unsigned)l.mi);
    ub_s(&u, ":");
    ub_dec2(&u, (unsigned)l.s);
    sb_strcpy(big, big_n, u.b);
    ub_init(&u);
    day_text(&u, &l);
    ub_s(&u, "  ");
    ub_s(&u, tz_zones[0].city);
    ub_s(&u, " ");
    ub_s(&u, l.abbr);
    ub_s(&u, "   tap: world clock");
    sb_strcpy(small, small_n, u.b);
}

static void world_fill(struct page *p)
{
    struct ub u;
    int64_t t;
    if (!ut_now(&t)) {
        page_note(p, "The RTC did not read (or holds a date before 2020).", C_ORANGE);
        return;
    }
    struct tz_time home, l;
    tz_local(&tz_zones[0], t, &home);
    ub_init(&u);
    ub_dec2(&u, (unsigned)home.h);
    ub_s(&u, ":");
    ub_dec2(&u, (unsigned)home.mi);
    ub_s(&u, ":");
    ub_dec2(&u, (unsigned)home.s);
    struct ub c;
    ub_init(&c);
    ub_s(&c, tz_zones[0].city);
    ub_s(&c, "  ");
    ub_s(&c, home.abbr);
    ub_s(&c, "  ");
    day_text(&c, &home);
    page_big(p, c.b, u.b, C_L1);
    page_hdr(p, "World");
    int64_t hd = tz_days(home.y, home.mo, home.d);
    for (int i = 1; i < tz_nzones; ++i) {
        tz_local(&tz_zones[i], t, &l);
        ub_init(&u);
        ub_dec2(&u, (unsigned)l.h);
        ub_s(&u, ":");
        ub_dec2(&u, (unsigned)l.mi);
        ub_s(&u, "  ");
        ub_s(&u, l.abbr);
        int64_t dd = tz_days(l.y, l.mo, l.d) - hd;
        ub_s(&u, dd > 0 ? "  tomorrow" : dd < 0 ? "  yesterday" : "");
        page_kv(p, tz_zones[i].city, u.b, 0);
    }
    page_note(p, "From the phone's RTC (UTC) with the EU and US daylight-saving rules. Android and Linux keep the "
                 "RTC in UTC; if Windows ran last it may hold local time.", C_L3);
}

void ut_world(void)
{
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_H);
    uint64_t next = 0;
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    for (;;) {
        if (sb_ms() >= next) {
            ui_fill(0, 0, UI_W, UI_VIEW_Y0, C_BG);
            ui_header_draw("World clock", "Everyday", "", 0);
            ut_status_bar();
            page_begin(&p);
            world_fill(&p);
            page_end(&p);
            page_redraw(&p);
            ui_present();
            next = sb_ms() + 1000U;
        }
        struct ui_ev e;
        ui_poll(&e, 20);
        ut_service();
        if (e.type == EV_BACK || e.type == EV_HOME || e.type == EV_SWIPE_R || (e.type == EV_TAP && ui_header_back_hit(e.x, e.y)))
            break;
        (void)page_scroll_event(&p, &e);
        page_fling_step(&p);
    }
    page_free(&p);
}
