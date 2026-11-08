/* SPDX-License-Identifier: MIT
 * Settings (docs/STARBITE-OS.md 9): four sections chosen at the top: Display, Alarm, Heat, About. Settings
 * last until the next restart (saving them comes with the state slot). */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("settings", "Settings", 0, 0xff8e8e93U, 205);

#define GREY 0xff8e8e93U
static const int secs[4] = { 15, 30, 60, 120 };
static int section;

static void about(void)
{
    struct sbos_sysinfo s;
    struct fmt f;
    sysinfo(&s);
    ui_big("STARBITE OS", s.version, 0);
    ui_textline("An emergency system for the Galaxy S9: light, heat, alarm, battery, the emergency card.", 0xffffffffU);
    fmt_init(&f);
    fmt_s(&f, s.irq_mode ? "Interrupts on: the processor sleeps between events" : "Polling: not saving power");
    ui_textline(f.b, s.irq_mode ? 0 : 0xffff9f0aU);
    fmt_init(&f);
    fmt_s(&f, "First frame ");
    fmt_d(&f, s.first_frame_ms);
    fmt_s(&f, " ms after power-on   ");
    fmt_d(&f, s.apps);
    fmt_s(&f, " apps");
    ui_textline(f.b, 0);
    fmt_init(&f);
    fmt_s(&f, "Clocks: A55 ");
    fmt_d(&f, s.a55_mhz);
    fmt_s(&f, " MHz, M3 ");
    if (s.m3_mhz) {
        fmt_d(&f, s.m3_mhz);
        fmt_s(&f, " MHz");
    } else {
        fmt_s(&f, "off");
    }
    fmt_s(&f, "   up ");
    fmt_d(&f, s.uptime_s / 60);
    fmt_s(&f, " min");
    ui_textline(f.b, 0);
    ui_textline("Calls and messages need the modem driver: not in this version.", GREY);
}

static int led_refused;                     /* 0.6.3: the LED chip did not answer */

static void show(void)
{
    ui_begin();
    ui_title("Settings");
    ui_level(9, "Display|Alarm|Heat|About", section);
    if (section == 0) {
        struct sbos_sysinfo s;
        sysinfo(&s);
        int cur = 1;
        for (int i = 0; i < 4; ++i)
            if (secs[i] == s.screen_off_s)
                cur = i;
        int br = setting_get(SET_BRIGHTNESS);
        ui_textline("Brightness", GREY);
        ui_level(6, "Boot|Low|Mid|Normal|High", br < 0 ? 0 : br);
        ui_textline("Screen off after", GREY);
        ui_level(1, "15 s|30 s|1 min|2 min", cur);
        ui_toggle(2, "Refresh only on change", setting_get(SET_PANEL_SAVE) == 1);
        ui_toggle(3, "Panel off with the screen", setting_get(SET_PANEL_OFF) == 1);
        int ao = setting_get(SET_AUTO_OFF_MIN);
        ui_textline("Switch off when the screen has been off", GREY);
        ui_level(7, "Never|5 min|15 min|1 h", ao == 5 ? 1 : ao == 15 ? 2 : ao == 60 ? 3 : 0);
    } else if (section == 1) {
        ui_toggle(4, "LED strobes in the alarm", setting_get(SET_SOS_LED) == 1);
        ui_textline("The alarm flashes the screen white and red. With this on, the LED on the back strobes too.", 0);
        ui_toggle(8, "LED at rated power", setting_get(SET_LED_FULL) == 1);
        ui_textline(led_refused ? "The LED chip did not answer: the LED stays as it was (logged)." :
                    "The light at 240 mA and, in the alarm, 1250 mA camera-flash pulses once a second (Samsung's values). Until a restart.",
                    led_refused ? 0xffff9f0aU : GREY);
        ui_textline("Sound and vibration: not available yet.", GREY);
    } else if (section == 2) {
        ui_toggle(5, "GPU in Heat High (test)", setting_get(SET_GPU_HEAT) == 1);
        ui_textline("High also loads the graphics processor: more heat, faster battery use. Asks first.", 0);
        ui_textline("Heat stops by itself at 42 \xc2\xb0" "C battery, 46 \xc2\xb0" "C case, 15 % battery or after 30 minutes.", GREY);
    } else {
        about();
    }
    ui_textline(setting_get(SET_SAVED) == 1 ? "Saved on this phone." : "Settings last until a restart (no save slot yet).", GREY);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    if (ev->type == EV_APP_START) {
        ui_tile("Display, alarm, about");
    } else if (ev->type == EV_APP_SHOW) {
        show();
    } else if (ev->type == EV_APP_TOGGLE) {
        int id = ev->id == 2 ? SET_PANEL_SAVE : ev->id == 3 ? SET_PANEL_OFF : ev->id == 4 ? SET_SOS_LED :
                 ev->id == 5 ? SET_GPU_HEAT : ev->id == 8 ? SET_LED_FULL : 0;
        if (id) {
            int r = setting_set(id, ev->value);
            if (id == SET_LED_FULL)
                led_refused = ev->value && r != 0;
        }
        show();
    } else if (ev->type == EV_APP_LEVEL) {
        if (ev->id == 9 && ev->value >= 0 && ev->value < 4)
            section = ev->value;
        else if (ev->id == 1 && ev->value >= 0 && ev->value < 4)
            setting_set(SET_SCREEN_OFF_S, secs[ev->value]);
        else if (ev->id == 7 && ev->value >= 0 && ev->value < 4) {
            static const int mins[4] = { 0, 5, 15, 60 };
            setting_set(SET_AUTO_OFF_MIN, mins[ev->value]);
        } else if (ev->id == 6 && ev->value >= 0 && ev->value <= 4 && setting_set(SET_BRIGHTNESS, ev->value) < 0)
            ui_toast_app("The panel did not take it", 2500);
        show();
    }
}
