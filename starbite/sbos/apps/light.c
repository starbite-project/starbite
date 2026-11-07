/* SPDX-License-Identifier: MIT
 * Light (docs/STARBITE-OS.md 9): the flashlight LED (on or off: its driver is a GPIO, no current levels yet,
 * doc 9.2) and the screen as a light, white, warm or red, at three strengths. */
#include "sbapp.h"

SBAPP_DEFINE("light", "Light", SBOS_P_LIGHT, 0xffffd60aU, 200);

static int strength = 2;                    /* 0 low, 1 half, 2 full */

/* the colour at the chosen strength (the panel's own brightness stays: Settings > Display) */
static uint32_t scaled(uint32_t argb)
{
    static const unsigned pct[3] = { 25, 55, 100 };
    uint32_t r = (argb >> 16) & 0xffU, g = (argb >> 8) & 0xffU, b = argb & 0xffU;
    unsigned p = pct[strength];
    return 0xff000000U | ((r * p / 100U) << 16) | ((g * p / 100U) << 8) | (b * p / 100U);
}

static void tile(void)
{
    int led = torch(-1);
    ui_tile(led == 1 ? "LED on" : led == 0 ? "LED and screen" : "Screen light");
}

static void show(void)
{
    int led = torch(-1);                    /* 1 on, 0 off, < 0 not usable */
    ui_begin();
    ui_title("Light");
    ui_button(4, led == 1 ? "Flashlight: ON" : "Flashlight",
              led == 1 ? "Tap to turn it off" : led == 0 ? "The LED on the back" : "Not available on this phone",
              led == 1 ? UI_ACCENT : led == 0 ? 0 : UI_DIM);
    ui_textline("Screen light", 0xff8e8e93U);
    ui_level(5, "Low|Half|Full", strength);
    ui_button(1, "White", "The whole screen", 0);
    ui_button(2, "Warm", "Softer", UI_HALF);
    ui_button(3, "Red", "Night vision", UI_HALF);
    ui_textline("Tap or press a key to end the screen light. Both turn off after 10 minutes.", 0);
    ui_end();
    tile();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        tile();
        break;
    case EV_APP_SHOW:
        show();
        break;
    case EV_APP_LEVEL:
        if (ev->id == 5 && ev->value >= 0 && ev->value < 3)
            strength = ev->value;
        show();
        break;
    case EV_APP_BUTTON:
        if (ev->id == 1)
            ui_light(scaled(SBOS_COLOR_WHITE));
        else if (ev->id == 2)
            ui_light(scaled(SBOS_COLOR_WARM));
        else if (ev->id == 3)
            ui_light(scaled(SBOS_COLOR_RED));
        else if (ev->id == 4) {
            int r = torch(torch(-1) == 1 ? 0 : 3);
            if (r == SBOS_EAGAIN)
                ui_toast_app("The LED is not available", 2500);
            show();
        }
        break;
    case EV_APP_SERVICE:
        tile();
        break;
    default:
        break;
    }
}
