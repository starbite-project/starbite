/* SPDX-License-Identifier: MIT
 * Light (docs/STARBITE-OS.md 9): the screen as a light, white, warm or red, full screen. The LED torch comes
 * when its driver exists (torch() says SBOS_ENOSYS until then). */
#include "sbapp.h"

SBAPP_DEFINE("light", "Light", SBOS_P_LIGHT, 0xffffd60aU, 200);

static void show(void)
{
    int led = torch(0) != SBOS_ENOSYS;
    ui_begin();
    ui_title("Light");
    ui_button(1, "White", "The whole screen, full brightness", 0);
    ui_button(2, "Warm", "Softer on the eyes", UI_HALF);
    ui_button(3, "Red", "Keeps night vision", UI_HALF);
    ui_button(4, "Flashlight LED", led ? "The camera's light" : "Comes with its driver", led ? 0 : UI_DIM);
    ui_textline("Tap the screen or press a key to turn it off.", 0);
    ui_textline("It turns off by itself after 10 minutes.", 0);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_SHOW:
        show();
        break;
    case EV_APP_BUTTON:
        if (ev->id == 1)
            ui_light(SBOS_COLOR_WHITE);
        else if (ev->id == 2)
            ui_light(SBOS_COLOR_WARM);
        else if (ev->id == 3)
            ui_light(SBOS_COLOR_RED);
        else if (ev->id == 4)
            torch(3);
        break;
    default:
        break;
    }
}
