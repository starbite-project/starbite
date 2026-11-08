/* SPDX-License-Identifier: MIT
 * Heat (docs/STARBITE-OS.md 8): asks the kernel's heater for a level and shows what it does. Every limit is in
 * the kernel; this app only asks and shows. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("heat", "Heat", SBOS_P_HEAT, 0xffff9f0aU, 201);

#define ORANGE 0xffff9f0aU
#define GREY 0xff8e8e93U

static int visible;
/* 0.5.6: the battery's temperature during this heating session, every 10 s (the last 10 minutes) */
static int16_t th[60];
static int thn, was_on;
static uint64_t th_t;

static void sample(const struct sbos_heat *h, int on)
{
    uint64_t now = uptime_ms();
    if (on && !was_on)
        thn = 0;                            /* a new session: a new curve */
    was_on = on;
    if (!on || h->bat_c10 <= -1000 || (thn && now - th_t < 10000))
        return;
    if (thn == 60) {
        for (int i = 1; i < 60; ++i)
            th[i - 1] = th[i];
        --thn;
    }
    th[thn++] = (int16_t)h->bat_c10;
    th_t = now;
}

static void temp(struct fmt *f, int c10)
{
    if (c10 <= -1000)
        fmt_s(f, "--");
    else
        fmt_c10(f, c10);
    fmt_s(f, " \xc2\xb0" "C");
}

static const char *const lvl[4] = { "Off", "Low", "Medium", "High" };

static void tile(const struct sbos_heat *h, int on)
{
    struct fmt f;
    fmt_init(&f);
    if (on) {
        fmt_s(&f, lvl[h->level & 3]);
        fmt_s(&f, "  ");
        fmt_d(&f, (h->secs_left + 59) / 60);
        fmt_s(&f, " min");
    } else {
        fmt_s(&f, "Off");
        if (h->bat_c10 > -1000) {
            fmt_s(&f, "  ");
            temp(&f, h->bat_c10);
        }
    }
    ui_tile(f.b);
}

static void show(void)
{
    struct sbos_heat h;
    struct fmt f;
    heat_status(&h);
    int on = h.state == HEAT_ST_RUNNING || h.state == HEAT_ST_HOLDING || h.state == HEAT_ST_PAUSED;
    if (h.bat_c10 <= -1000)
        h.bat_c10 = sensor_read(SENSOR_BATTERY_C10);
    if (h.soc_c10 <= -1000)
        h.soc_c10 = sensor_read(SENSOR_SOC_C10);
    tile(&h, on);
    sample(&h, on);
    if (!visible)
        return;
    ui_begin();
    ui_title("Heat");
    ui_level(1, "Off|Low|High", on ? (h.level >= HEAT_HIGH ? 2 : 1) : 0);
    fmt_init(&f);
    temp(&f, h.bat_c10);
    ui_big(on ? "BATTERY  (STOPS AT 42 \xc2\xb0" "C)" : "BATTERY", f.b, h.bat_c10 >= 390 ? ORANGE : 0);
    fmt_init(&f);
    fmt_s(&f, "Processor ");
    temp(&f, h.soc_c10);
    if (on) {
        fmt_s(&f, "   ");
        fmt_d(&f, h.cores);
        fmt_s(&f, h.cores == 1 ? " core" : " cores");
    }
    ui_textline(f.b, 0xffffffffU);
    fmt_init(&f);
    if (on) {
        fmt_s(&f, lvl[h.level & 3]);
        fmt_s(&f, ": stops by itself in ");
        fmt_d(&f, (h.secs_left + 59) / 60);
        fmt_s(&f, " min");
        ui_textline(f.b, 0xff30d158U);
    }
    if (h.why[0])
        ui_textline(h.why, h.state == HEAT_ST_STOPPED || h.state == HEAT_ST_REFUSED ? ORANGE :
                           h.state == HEAT_ST_PAUSED ? 0xffffd60aU : 0xff30d158U);
    if (thn >= 2) {
        struct fmt lr;
        fmt_init(&lr);
        fmt_d(&lr, (thn - 1) * 10 / 60 ? (thn - 1) * 10 / 60 : (thn - 1) * 10);
        fmt_s(&lr, (thn - 1) * 10 / 60 ? " min ago|now (stops at 42)" : " s ago|now (stops at 42)");
        ui_chart("BATTERY TEMPERATURE", th, thn, 200, 450, ORANGE, lr.b);
    }
    ui_textline("WARNING: the phone gets hot to hold. Keep it in a pocket or a glove, not on bare skin.", ORANGE);
    ui_textline("Uses the battery fast: about 1 % a minute on High. Not while charging.", GREY);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        timer_set(2, 10000, 1);             /* the home tile's line and the temperature curve */
        show();
        break;
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
        heat_request(ev->value == 2 ? HEAT_HIGH : ev->value == 1 ? HEAT_LOW : HEAT_OFF, 30);
        show();
        break;
    case EV_APP_TIMER:
    case EV_APP_SERVICE:
        show();
        break;
    default:
        break;
    }
}
