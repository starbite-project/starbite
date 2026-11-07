/* SPDX-License-Identifier: MIT
 * Battery (docs/STARBITE-OS.md 4.5): charge, capacity, current, temperature and the estimates the kernel
 * learns per power state. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("battery", "Battery", 0, 0xff30d158U, 204);

static int visible;

static void show(void)
{
    struct sbos_battery b;
    struct sbos_estimate e;
    struct fmt f;
    battery(&b);
    estimate(&e);
    ui_begin();
    ui_title("Battery");
    fmt_init(&f);
    if (b.ok) {
        fmt_d(&f, b.pct);
        fmt_s(&f, " %");
    } else {
        fmt_s(&f, "--");
    }
    ui_big(b.charger ? "CHARGING" : "CHARGE", f.b, b.pct < 15 ? 0xffff453aU : b.charger ? 0xff30d158U : 0);
    fmt_init(&f);
    fmt_d(&f, b.cap_mah);
    fmt_s(&f, " mAh left of ");
    fmt_d(&f, b.full_mah);
    ui_textline(f.b, 0);
    fmt_init(&f);
    fmt_s(&f, b.ma >= 0 ? "Charging " : "Using ");
    fmt_d(&f, b.ma >= 0 ? b.ma : -b.ma);
    fmt_s(&f, " mA   ");
    fmt_d(&f, b.mv);
    fmt_s(&f, " mV   ");
    if (b.temp_c10 > -1000) {
        fmt_c10(&f, b.temp_c10);
        fmt_s(&f, " \xc2\xb0" "C");
    }
    ui_textline(f.b, 0);
    fmt_init(&f);
    fmt_dur(&f, e.now_min);
    ui_big(e.learned ? "AT THIS USE" : "AT THIS USE (TYPICAL)", f.b, 0);
    fmt_init(&f);
    fmt_s(&f, "Screen off: ");
    fmt_dur(&f, e.screen_off_min);
    ui_textline(f.b, 0);
    fmt_init(&f);
    fmt_s(&f, "Switched off: months (typical)");
    ui_textline(f.b, 0);
    ui_textline("Counted to 5 %: the rest is kept for Light and SOS.", 0);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
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
        if (visible)
            show();
        break;
    default:
        break;
    }
}
