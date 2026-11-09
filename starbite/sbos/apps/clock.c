/* SPDX-License-Identifier: GPL-2.0-only
 * Clock (docs/STARBITE-OS.md 9): local time (Helsinki rules), the world clock, a stopwatch and a timer. Uses the
 * Utility OS's time zones (starbite/utility/tz.c, linked into this app). */
#include "sbapp.h"
#include "sbfmt.h"
#include "tz.h"

SBAPP_DEFINE("clock", "Clock", SBOS_P_ALARM, 0xff6fb3ffU, 203);   /* 1.2-alpha8: alarms and the motor */

enum { M_MAIN, M_WORLD, M_STOPWATCH, M_TIMER, M_ALARMS, M_EDIT, M_RING };

/* 1.2-alpha8: wake-up alarms, kept in this app's saved state and given to the kernel at every start */
#define NAL 4
#define AL_MAGIC 0x314d4c41U                /* "ALM1" */
static struct {
    uint32_t magic;
    struct { uint8_t on, h, m, days; } a[NAL];
} al;
static int edit_i, ring_i = -1;
static const char *const rep_name[3] = { "Once", "Every day", "Weekdays" };

static int rep_of(int days)
{
    return days == 0x7f ? 1 : days == 0x3e ? 2 : 0;
}

static void al_push(int i)
{
    alarm_set(i, al.a[i].h, al.a[i].m, al.a[i].days, al.a[i].on);
}

static void al_save(void)
{
    al.magic = AL_MAGIC;
    state_put(&al, sizeof(al));
}
static int mode, visible;
static uint64_t sw_t0, sw_acc;
static int sw_run;
static int64_t tm_left_ms = 5 * 60000;
static uint64_t tm_end;
static int tm_run, tm_done;

static const char *const wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mn[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December" };

static void hm(struct fmt *f, int h, int m)
{
    fmt_2(f, (unsigned)h);
    fmt_c(f, ':');
    fmt_2(f, (unsigned)m);
}

static void mmss(struct fmt *f, uint64_t ms, int tenths)
{
    uint64_t s = ms / 1000U;
    if (s >= 3600) {
        fmt_u(f, s / 3600);
        fmt_c(f, ':');
        fmt_2(f, (unsigned)(s / 60 % 60));
    } else {
        fmt_2(f, (unsigned)(s / 60));
    }
    fmt_c(f, ':');
    fmt_2(f, (unsigned)(s % 60));
    if (tenths) {
        fmt_c(f, '.');
        fmt_c(f, (char)('0' + ms / 100U % 10U));
    }
}

static void show_main(void)
{
    struct sbos_time t;
    struct fmt f;
    int ok = time_now(&t) == 0;
    ui_title("Clock");
    fmt_init(&f);
    if (ok)
        hm(&f, t.h, t.mi);
    else
        fmt_s(&f, "--:--");
    ui_big(ok ? t.zone : "TIME NOT SET", f.b, 0);
    if (ok && t.mo >= 1 && t.mo <= 12) {
        fmt_init(&f);
        fmt_s(&f, wd[t.wday % 7]);
        fmt_c(&f, ' ');
        fmt_d(&f, t.d);
        fmt_c(&f, ' ');
        fmt_s(&f, mn[t.mo - 1]);
        fmt_c(&f, ' ');
        fmt_d(&f, t.y);
        fmt_s(&f, "   ");
        fmt_s(&f, t.abbr);
        ui_textline(f.b, 0);
    }
    ui_button(1, "World clock", "Six cities", 0);
    ui_button(2, "Stopwatch", sw_run ? "Running" : "", UI_HALF);
    ui_button(3, "Timer", tm_run ? "Running" : tm_done ? "Done" : "", UI_HALF);
    {
        int n = 0;
        for (int i = 0; i < NAL; ++i)
            n += al.a[i].on;
        fmt_init(&f);
        if (n) {
            fmt_d(&f, n);
            fmt_s(&f, n == 1 ? " alarm on" : " alarms on");
        } else {
            fmt_s(&f, "None on");
        }
        ui_button(4, "Alarms", f.b, 0);
    }
}

static void al_line(struct fmt *f, int i)
{
    hm(f, al.a[i].h, al.a[i].m);
    fmt_s(f, "  ");
    fmt_s(f, rep_name[rep_of(al.a[i].days)]);
}

static void show_alarms(void)
{
    ui_title("Alarms");
    for (int i = 0; i < NAL; ++i) {
        struct fmt f;
        fmt_init(&f);
        al_line(&f, i);
        ui_toggle(30U + (unsigned)i, f.b, al.a[i].on);
        ui_button(34U + (unsigned)i, "Set time", 0, UI_HALF);
    }
    ui_textline("It buzzes and shows this app. Power: snooze 9 min. The phone stays on while one is set.", 0);
}

static void show_edit(void)
{
    struct fmt f;
    ui_title("Alarm time");
    fmt_init(&f);
    hm(&f, al.a[edit_i].h, al.a[edit_i].m);
    ui_big("", f.b, 0);
    ui_button(40, "+1 hour", 0, UI_HALF);
    ui_button(41, "-1 hour", 0, UI_HALF);
    ui_button(42, "+10 min", 0, UI_HALF);
    ui_button(43, "-10 min", 0, UI_HALF);
    ui_button(44, "+1 min", 0, UI_HALF);
    ui_button(45, "-1 min", 0, UI_HALF);
    ui_level(46, "Once|Every day|Weekdays", rep_of(al.a[edit_i].days));
    ui_button(47, "Done: the alarm on", 0, UI_ACCENT);
}

static void show_ring(void)
{
    struct fmt f;
    ui_title("Alarm");
    fmt_init(&f);
    if (ring_i >= 0)
        hm(&f, al.a[ring_i].h, al.a[ring_i].m);
    ui_big("WAKE UP", f.b, 0xffff9f0aU);
    ui_button(50, "Stop", 0, UI_RED);
    ui_button(51, "Snooze 9 minutes", "Or press Power", UI_ACCENT);
}

static void show_world(void)
{
    struct sbos_time t;
    ui_title("World clock");
    if (time_now(&t)) {
        ui_textline("The time is not set.", 0);
        return;
    }
    for (int i = 0; i < tz_nzones && i < 7; ++i) {
        struct tz_time l;
        struct fmt f;
        tz_local(&tz_zones[i], t.utc, &l);
        fmt_init(&f);
        hm(&f, l.h, l.mi);
        fmt_s(&f, "   ");
        fmt_s(&f, tz_zones[i].city);
        if (l.d != t.d)
            fmt_s(&f, l.d > t.d || (l.d == 1 && t.d > 27) ? "  (tomorrow)" : "  (yesterday)");
        ui_textline(f.b, i == 0 ? 0xffffffffU : 0);
    }
}

static void show_stopwatch(void)
{
    struct fmt f;
    uint64_t ms = sw_acc + (sw_run ? uptime_ms() - sw_t0 : 0);
    ui_title("Stopwatch");
    fmt_init(&f);
    mmss(&f, ms, 1);
    ui_big("", f.b, 0);
    ui_button(10, sw_run ? "Stop" : "Start", 0, sw_run ? UI_RED : UI_ACCENT);
    ui_button(11, "Reset", 0, sw_run ? UI_DIM : 0);
}

static void show_timer(void)
{
    struct fmt f;
    int64_t left = tm_run ? (int64_t)tm_end - (int64_t)uptime_ms() : tm_left_ms;
    if (left < 0)
        left = 0;
    ui_title("Timer");
    fmt_init(&f);
    mmss(&f, (uint64_t)left + (tm_run ? 999 : 0), 0);
    ui_big(tm_done ? "DONE" : "", f.b, tm_done ? 0xffff9f0aU : 0);
    ui_button(20, "+1 min", 0, UI_HALF | (tm_run ? UI_DIM : 0));
    ui_button(21, "+5 min", 0, UI_HALF | (tm_run ? UI_DIM : 0));
    ui_button(22, tm_run ? "Pause" : "Start", 0, tm_run ? 0 : UI_ACCENT);
    ui_button(23, "Reset", 0, tm_run ? UI_DIM : 0);
}

static void show(void)
{
    ui_begin();
    if (mode == M_WORLD)
        show_world();
    else if (mode == M_STOPWATCH)
        show_stopwatch();
    else if (mode == M_TIMER)
        show_timer();
    else if (mode == M_ALARMS)
        show_alarms();
    else if (mode == M_EDIT)
        show_edit();
    else if (mode == M_RING)
        show_ring();
    else
        show_main();
    ui_end();
    ui_awake((mode == M_STOPWATCH && sw_run) || (mode == M_TIMER && (tm_run || tm_done)) || mode == M_RING);
}

static void tick(void)
{
    uint32_t ms = mode == M_STOPWATCH && sw_run ? 100 : mode == M_TIMER && tm_run ? 250 : 1000;
    if (visible)
        timer_set(1, ms, 1);
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        ui_tile("World, stopwatch, timer, alarms");
        if (state_get(&al, sizeof(al)) != (int)sizeof(al) || al.magic != AL_MAGIC) {
            for (int i = 0; i < NAL; ++i) {
                al.a[i].on = 0;
                al.a[i].h = (uint8_t)(i == 0 ? 7 : 8);
                al.a[i].m = (uint8_t)(i == 0 ? 0 : i == 1 ? 0 : i == 2 ? 30 : 45);
                al.a[i].days = 0x3e;
            }
        }
        for (int i = 0; i < NAL; ++i)
            al_push(i);
        break;
    case EV_APP_SERVICE:
        if (ev->id == 3) {                  /* the kernel rings alarm (value & 0xff) */
            ring_i = ev->value & 0xff;
            if (ring_i >= 0 && ring_i < NAL && !al.a[ring_i].days) {
                al.a[ring_i].on = 0;        /* once: off now */
                al_save();
            }
            mode = M_RING;
            show();
        } else if (ev->id == 4) {
            if (mode == M_RING)
                mode = M_MAIN;
            ring_i = -1;
            if (visible)
                show();
        }
        break;
    case EV_APP_SHOW:
        visible = 1;
        tick();
        show();
        break;
    case EV_APP_HIDE:
        visible = 0;
        timer_cancel(1);
        mode = M_MAIN;
        break;
    case EV_APP_BUTTON:
        switch (ev->id) {
        case 1: mode = M_WORLD; break;
        case 2: mode = M_STOPWATCH; break;
        case 3: mode = M_TIMER; tm_done = 0; break;
        case 4: mode = M_ALARMS; break;
        case 34: case 35: case 36: case 37:
            edit_i = (int)ev->id - 34;
            mode = M_EDIT;
            break;
        case 40: case 41: case 42: case 43: case 44: case 45: {
            static const int dm[6] = { 60, -60, 10, -10, 1, -1 };
            int t = al.a[edit_i].h * 60 + al.a[edit_i].m + dm[ev->id - 40];
            t = (t % 1440 + 1440) % 1440;
            al.a[edit_i].h = (uint8_t)(t / 60);
            al.a[edit_i].m = (uint8_t)(t % 60);
            break;
        }
        case 47:
            al.a[edit_i].on = 1;
            al_push(edit_i);
            al_save();
            mode = M_ALARMS;
            break;
        case 50: case 51:
            alarm_cancel(ev->id == 51);
            mode = M_MAIN;
            break;
        case 10:
            if (sw_run) {
                sw_acc += uptime_ms() - sw_t0;
                sw_run = 0;
            } else {
                sw_t0 = uptime_ms();
                sw_run = 1;
            }
            break;
        case 11:
            if (!sw_run)
                sw_acc = 0;
            break;
        case 20: case 21:
            if (!tm_run) {
                tm_left_ms += ev->id == 20 ? 60000 : 300000;
                if (tm_left_ms > 99 * 60000)
                    tm_left_ms = 99 * 60000;
                tm_done = 0;
            }
            break;
        case 22:
            if (tm_run) {
                tm_left_ms = (int64_t)tm_end - (int64_t)uptime_ms();
                if (tm_left_ms < 0)
                    tm_left_ms = 0;
                tm_run = 0;
            } else if (tm_left_ms > 0) {
                tm_end = uptime_ms() + (uint64_t)tm_left_ms;
                tm_run = 1;
                tm_done = 0;
                timer_set(2, (uint32_t)tm_left_ms, 0);
                ui_tile("Timer running");
            }
            break;
        case 23:
            if (!tm_run) {
                tm_left_ms = 5 * 60000;
                tm_done = 0;
            }
            break;
        }
        if (!tm_run)
            timer_cancel(2);
        tick();
        show();
        break;
    case EV_APP_TOGGLE:
        if (ev->id >= 30 && ev->id < 30 + NAL) {
            int i = (int)ev->id - 30;
            al.a[i].on = (uint8_t)!!ev->value;
            al_push(i);
            al_save();
            show();
        }
        break;
    case EV_APP_LEVEL:
        if (ev->id == 46 && ev->value >= 0 && ev->value < 3) {
            static const uint8_t dd[3] = { 0, 0x7f, 0x3e };
            al.a[edit_i].days = dd[ev->value];
            show();
        }
        break;
    case EV_APP_BACK:
        if (mode == M_RING) {
            alarm_cancel(1);                /* Back while ringing: snooze, never a silent stop */
        } else if (mode == M_EDIT) {
            al_push(edit_i);                /* the time kept, its on/off as it was */
            al_save();
        }
        mode = M_MAIN;
        break;
    case EV_APP_TIMER:
        if (ev->id == 2 && tm_run) {
            tm_run = 0;
            tm_done = 1;
            tm_left_ms = 0;
            ui_toast_app("The timer is done", 4000);
            setting_set(SET_VIBRATE, (3U << 16) | 500U);   /* 1.2-alpha7: three buzzes */
            ui_tile("Timer done");
        }
        if (visible)
            show();
        break;
    default:
        break;
    }
}
