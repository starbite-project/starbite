/* SPDX-License-Identifier: MIT
 * SOS (docs/STARBITE-OS.md 9): starts the kernel's alarm: after a 3 s countdown the screen flashes white and
 * red with a big SOS (and the LED strobes if Settings allows) until it is held to stop. Sound comes when the
 * speaker driver exists. */
#include "sbapp.h"

SBAPP_DEFINE("sos", "SOS", SBOS_P_ALARM | SBOS_P_LIGHT, 0xffff453aU, 202);

static void show(void)
{
    int led = setting_get(SET_SOS_LED) == 1;
    ui_begin();
    ui_title("Alarm");
    ui_hold(1, "Hold to start the alarm", 1000, UI_RED);
    ui_textline("The screen flashes white and red with a big SOS so people can find you.", 0xffffffffU);
    ui_textline(led ? "The flashlight strobes with it." : "The flashlight can strobe with it: Settings > Alarm.", 0);
    ui_textline("Sound: not available yet (no speaker driver).", 0xff8e8e93U);
    ui_textline("3 seconds to cancel. Hold the screen to stop it.", 0xff8e8e93U);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    if (ev->type == EV_APP_START)
        ui_tile("Flash and strobe");
    else if (ev->type == EV_APP_SHOW)
        show();
    else if (ev->type == EV_APP_HOLD && ev->id == 1)
        sos(1);
}
