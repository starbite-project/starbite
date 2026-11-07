/* SPDX-License-Identifier: MIT
 * SOS (docs/STARBITE-OS.md 9): starts the kernel's SOS service: after a 3 s countdown the screen flashes white
 * and red with a big SOS until it is held to stop. The LED and the siren join when their drivers exist. */
#include "sbapp.h"

SBAPP_DEFINE("sos", "SOS", SBOS_P_ALARM | SBOS_P_LIGHT, 0xffff453aU, 202);

static void show(void)
{
    ui_begin();
    ui_title("SOS");
    ui_textline("The screen flashes white and red with a big SOS,", 0);
    ui_textline("so people can find you in the dark.", 0);
    ui_hold(1, "Hold to start SOS", 1000, UI_RED);
    ui_textline("You get 3 seconds to cancel. Hold to stop it.", 0);
    ui_textline(setting_get(SET_SOS_LED) == 1 ? "The flashlight LED flashes with it." :
                "The LED can flash too: Settings. A loud siren comes later.", 0);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    if (ev->type == EV_APP_SHOW)
        show();
    else if (ev->type == EV_APP_HOLD && ev->id == 1)
        sos(1);
}
