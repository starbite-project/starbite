/* SPDX-License-Identifier: MIT
 * Battery (docs/STARBITE-OS.md 4.5): charge as a ring, the estimates the kernel learns per power state, and
 * the details. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("battery", "Battery", SBOS_P_SENSORS, 0xff30d158U, 204);

static int visible;

static void show(void)
{
    struct sbos_battery b;
    struct sbos_estimate e;
    struct fmt f, g;
    battery(&b);
    estimate(&e);
    fmt_init(&g);
    if (b.ok) {
        fmt_d(&g, b.pct);
        fmt_s(&g, " %");
        if (!b.charger && e.now_min > 0) {
            fmt_s(&g, "  ");
            fmt_dur(&g, e.now_min);
        } else if (b.charger) {
            fmt_s(&g, "  charging");
        }
    }
    ui_tile(g.b);
    if (!visible)
        return;
    uint32_t c = !b.ok ? 0xff8e8e93U : b.pct < 15 ? 0xffff453aU : b.pct < 30 ? 0xffff9f0aU : 0xff30d158U;
    ui_begin();
    ui_title("Battery");
    fmt_init(&f);
    if (b.ok) {
        fmt_d(&f, b.pct);
        fmt_s(&f, "%");
    } else {
        fmt_s(&f, "--");
    }
    ui_gauge(b.charger ? "CHARGING" : "CHARGE", f.b, b.ok ? b.pct * 10 : 0, c);
    fmt_init(&f);
    fmt_dur(&f, e.now_min);
    ui_big(e.learned ? "LEFT AT THIS USE" : "LEFT AT THIS USE (TYPICAL)", f.b, 0);
    {
        /* 0.5.6: the charge over the last hours (one value every 10 minutes since the start) */
        int16_t h[48];
        int hn = battery_history(h, 48);
        if (hn >= 2) {
            struct fmt lr;
            fmt_init(&lr);
            int mins = (hn - 1) * 10;
            if (mins >= 120) {
                fmt_d(&lr, mins / 60);
                fmt_s(&lr, " h ago|now");
            } else {
                fmt_d(&lr, mins);
                fmt_s(&lr, " min ago|now");
            }
            ui_chart("CHARGE OVER TIME", h, hn, 0, 1000, c, lr.b);
        }
    }
    fmt_init(&f);
    fmt_s(&f, "Screen off: ");
    fmt_dur(&f, e.screen_off_min);
    fmt_s(&f, "   switched off: months");
    ui_textline(f.b, 0xffffffffU);
    fmt_init(&f);
    fmt_d(&f, b.cap_mah);
    fmt_s(&f, " of ");
    fmt_d(&f, b.full_mah);
    fmt_s(&f, " mAh   ");
    fmt_s(&f, b.ma >= 0 ? "in " : "out ");
    fmt_d(&f, b.ma >= 0 ? b.ma : -b.ma);
    fmt_s(&f, " mA   ");
    fmt_d(&f, b.mv);
    fmt_s(&f, " mV");
    ui_textline(f.b, 0);
    fmt_init(&f);
    fmt_s(&f, "Battery ");
    if (b.temp_c10 > -1000)
        fmt_c10(&f, b.temp_c10);
    else
        fmt_s(&f, "--");
    fmt_s(&f, " \xc2\xb0" "C   phone ");
    int skin = sensor_read(SENSOR_SKIN_C10);
    if (skin > -1000)
        fmt_c10(&f, skin);
    else
        fmt_s(&f, "--");
    fmt_s(&f, " \xc2\xb0" "C");
    ui_textline(f.b, 0);
    ui_textline("Counted to 5 %: the rest is kept for Light and the alarm.", 0xff8e8e93U);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        timer_set(2, 60000, 1);             /* the home tile's line */
        show();
        break;
    case EV_APP_SHOW:
        visible = 1;
        timer_set(1, 5000, 1);
        show();
        break;
    case EV_APP_HIDE:
        visible = 0;
        timer_cancel(1);
        break;
    case EV_APP_TIMER:
        show();
        break;
    default:
        break;
    }
}
