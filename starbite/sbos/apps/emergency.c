/* SPDX-License-Identifier: MIT
 * Emergency (docs/STARBITE-OS.md 9.3): one place for a call, a message and the alarm.
 *
 * 1.0: the alarm (the kernel's SOS service) and the medical card. Calls and messages come with the modem driver
 * (0.8); until then this app shows them as coming soon and has no way to call or send. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("emergency", "Emergency", SBOS_P_ALARM | SBOS_P_LIGHT, 0xffff453aU, 207);

#define RED 0xffff453aU
#define GREY 0xff8e8e93U

/* 1.0: the alarm and the medical card; calls and messages are "coming soon" (no mobile network driver yet). Nothing
 * here can call or send. */
static void show(void)
{
    ui_begin();
    ui_title("Emergency");
    ui_hold(3, "Hold for the alarm", 1000, UI_RED);
    ui_textline("The screen flashes white and red with a big SOS so people can find you.", 0xffffffffU);
    ui_button(5, "Medical card", "Your card in Info", 0);
    ui_textline("Calls and messages: coming soon (they need the mobile network driver).", GREY);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        ui_tile("Alarm, medical card");
        break;
    case EV_APP_SHOW:
        show();
        break;
    case EV_APP_HOLD:
        if (ev->id == 3)
            sos(1);
        show();
        break;
    case EV_APP_BUTTON:
        if (ev->id == 5)
            app_open("info");               /* 0.5.11: the medical card */
        show();
        break;
    default:
        break;
    }
}
