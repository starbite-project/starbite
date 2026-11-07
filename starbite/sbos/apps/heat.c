/* SPDX-License-Identifier: MIT
 * Heat (docs/STARBITE-OS.md 8): asks the kernel's heater for a level and shows what it does. Every limit is in
 * the kernel; this app only asks and shows. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("heat", "Heat", SBOS_P_HEAT, 0xffff9f0aU, 201);

static int visible;

static void temp(struct fmt *f, int c10)
{
    if (c10 <= -1000)
        fmt_s(f, "--");
    else
        fmt_c10(f, c10);
    fmt_s(f, " \xc2\xb0" "C");
}

static void show(void)
{
    struct sbos_heat h;
    struct fmt f;
    heat_status(&h);
    int on = h.state == HEAT_ST_RUNNING || h.state == HEAT_ST_HOLDING || h.state == HEAT_ST_PAUSED;
    ui_begin();
    ui_title("Heat");
    ui_level(1, "Off|Low|Medium|High", on ? h.level : 0);
    fmt_init(&f);
    temp(&f, h.bat_c10);
    ui_big("BATTERY", f.b, h.bat_c10 >= 390 ? 0xffff9f0aU : 0);
    fmt_init(&f);
    fmt_s(&f, "Processor ");
    temp(&f, h.soc_c10);
    if (on) {
        fmt_s(&f, "   ");
        fmt_d(&f, h.cores);
        fmt_s(&f, h.cores == 1 ? " core busy" : " cores busy");
    }
    ui_textline(f.b, 0);
    fmt_init(&f);
    if (on) {
        fmt_s(&f, "Stops in ");
        fmt_d(&f, h.secs_left / 60);
        fmt_s(&f, " min");
    } else {
        fmt_s(&f, "Runs 30 minutes, then stops by itself");
    }
    ui_textline(f.b, 0);
    if (h.why[0])
        ui_textline(h.why, h.state == HEAT_ST_STOPPED || h.state == HEAT_ST_REFUSED ? 0xffff9f0aU :
                           h.state == HEAT_ST_PAUSED ? 0xffffd60aU : 0xff30d158U);
    ui_textline("Stops at 42 \xc2\xb0" "C battery or 15 % battery.", 0);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_SHOW:
        visible = 1;
        timer_set(1, 1000, 1);
        show();
        break;
    case EV_APP_HIDE:
        visible = 0;
        timer_cancel(1);
        break;
    case EV_APP_LEVEL:
        heat_request(ev->value, 30);
        show();
        break;
    case EV_APP_TIMER:
    case EV_APP_SERVICE:
        if (visible)
            show();
        break;
    default:
        break;
    }
}
