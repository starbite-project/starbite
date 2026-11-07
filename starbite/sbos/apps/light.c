/* SPDX-License-Identifier: MIT
 * Light (docs/STARBITE-OS.md 9): the screen as a light, white, warm or red, full screen. The LED torch comes
 * when its driver exists (torch() says SBOS_ENOSYS until then). */
#include "sbapp.h"

SBAPP_DEFINE("light", "Light", SBOS_P_LIGHT, 0xffffd60aU, 200);

static void show(void)
{
    int led = torch(-1);                    /* 1 on, 0 off, < 0 not usable */
    ui_begin();
    ui_title("Light");
    ui_button(4, led == 1 ? "Flashlight LED: on" : "Flashlight LED",
              led == 1 ? "Tap to turn it off" : led == 0 ? "The camera's light, on the back" : "Not available on this phone",
              led == 1 ? UI_ACCENT : led == 0 ? 0 : UI_DIM);
    ui_button(1, "Screen: white", "The whole screen, full brightness", 0);
    ui_button(2, "Warm", "Softer on the eyes", UI_HALF);
    ui_button(3, "Red", "Keeps night vision", UI_HALF);
    ui_textline("The screen light ends with a tap or a key. Both lights turn off by themselves after 10 minutes.", 0);
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
        else if (ev->id == 4) {
            int r = torch(torch(-1) == 1 ? 0 : 3);
            if (r == SBOS_EAGAIN)
                ui_toast_app("The LED is not available", 2500);
            show();
        }
        break;
    default:
        break;
    }
}
