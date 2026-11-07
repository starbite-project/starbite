/* SPDX-License-Identifier: MIT
 * Settings (docs/STARBITE-OS.md 9): the screen-off time and what the system knows about itself. Settings
 * last until the next restart in v0.1 (the state slot comes in v0.2). */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("settings", "Settings", 0, 0xff8e8e93U, 205);

static const int secs[4] = { 15, 30, 60, 120 };

static void show(void)
{
    struct sbos_sysinfo s;
    struct fmt f;
    sysinfo(&s);
    int cur = 1;
    for (int i = 0; i < 4; ++i)
        if (secs[i] == s.screen_off_s)
            cur = i;
    ui_begin();
    ui_title("Settings");
    ui_textline("Screen off after", 0);
    ui_level(1, "15 s|30 s|1 min|2 min", cur);
    ui_toggle(2, "Refresh only on change", setting_get(SET_PANEL_SAVE) == 1);
    ui_toggle(3, "Panel off with the screen", setting_get(SET_PANEL_OFF) == 1);
    ui_toggle(4, "LED flashes with SOS", setting_get(SET_SOS_LED) == 1);
    ui_textline("These last until the next restart (saving them comes later).", 0);
    ui_textline(s.version, 0xffffffffU);
    fmt_init(&f);
    fmt_s(&f, s.irq_mode ? "Interrupts: on (the CPU sleeps)" : "Interrupts: polling (not saving power)");
    ui_textline(f.b, s.irq_mode ? 0 : 0xffff9f0aU);
    fmt_init(&f);
    fmt_s(&f, "First frame ");
    fmt_d(&f, s.first_frame_ms);
    fmt_s(&f, " ms after power-on");
    ui_textline(f.b, 0);
    fmt_init(&f);
    fmt_s(&f, "Clocks: A55 ");
    fmt_d(&f, s.a55_mhz);
    fmt_s(&f, " MHz, M3 ");
    if (s.m3_mhz)
        fmt_d(&f, s.m3_mhz);
    else
        fmt_s(&f, "off");
    ui_textline(f.b, 0);
    fmt_init(&f);
    fmt_d(&f, s.apps);
    fmt_s(&f, " apps   up ");
    fmt_d(&f, s.uptime_s / 60);
    fmt_s(&f, " min");
    ui_textline(f.b, 0);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    if (ev->type == EV_APP_SHOW) {
        show();
    } else if (ev->type == EV_APP_TOGGLE) {
        int id = ev->id == 2 ? SET_PANEL_SAVE : ev->id == 3 ? SET_PANEL_OFF : ev->id == 4 ? SET_SOS_LED : 0;
        if (id)
            setting_set(id, ev->value);
        show();
    } else if (ev->type == EV_APP_LEVEL && ev->id == 1 && ev->value >= 0 && ev->value < 4) {
        setting_set(SET_SCREEN_OFF_S, secs[ev->value]);
        show();
    }
}
