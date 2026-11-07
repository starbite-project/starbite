/* SPDX-License-Identifier: MIT
 * Emergency (docs/STARBITE-OS.md 9.3): one place for a call, a message and the alarm.
 *
 * Calls and messages are the screens only in 0.3: there is no modem driver, and this app never asks for a
 * call or a message to be sent (no SYS_CALL_DIAL or SYS_SMS_SEND anywhere in it). Holding "Call 112" shows
 * what would happen and why it cannot yet. The alarm is the kernel's SOS service. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("emergency", "Emergency", SBOS_P_ALARM | SBOS_P_LIGHT | SBOS_P_PHONE, 0xffff453aU, 207);

#define RED 0xffff453aU
#define GREY 0xff8e8e93U

static int page;                            /* 0 main, 1 messages, 2 the call screen */
static int picked = -1;
static const char *const msgs[3] = {
    "I need help. Please call me.",
    "I am safe.",
    "Hurt, cannot talk. Send help.",
};

static void show(void)
{
    ui_begin();
    if (page == 1) {
        ui_title("Message");
        ui_textline("Pick a message:", GREY);
        for (int i = 0; i < 3; ++i)
            ui_button((uint32_t)(10 + i), msgs[i], 0, picked == i ? UI_ACCENT : 0);
        if (picked >= 0)
            ui_textline("Not sent: messages need the modem driver, which is not in this version.", 0xffff9f0aU);
        ui_button(2, "Back", 0, UI_HALF);
    } else if (page == 2) {
        ui_title("Call 112");
        ui_big("NOT AVAILABLE YET", "No call made", RED);
        ui_textline("This version has no modem driver, so it cannot call. It never dials.", 0xffffffffU);
        ui_textline("Use another phone, or start the alarm so people can find you.", 0);
        ui_hold(3, "Hold for the alarm", 1000, UI_RED);
        ui_button(2, "Back", 0, UI_HALF);
    } else {
        ui_title("Emergency");
        ui_hold(1, "Call 112", 1200, UI_RED);
        ui_button(4, "Message", "Help / safe / hurt", UI_HALF);
        ui_button(5, "Medical card", "The Info app", UI_HALF);
        ui_hold(3, "Hold for the alarm", 1000, 0);
        int ph = (int)sbos_call(SYS_PHONE_STATE, 0, 0, 0, 0, 0);
        ui_textline(ph == SBOS_ENOSYS ? "No mobile network driver yet: calls and messages are not available."
                                      : "Mobile network: unknown.", GREY);
    }
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        ui_tile("Call, message, alarm");
        break;
    case EV_APP_SHOW:
        show();
        break;
    case EV_APP_HIDE:
    case EV_APP_BACK:
        page = 0;
        picked = -1;
        break;
    case EV_APP_HOLD:
        if (ev->id == 1) {
            sb_log_app("emergency: Call 112 held: shown as not available, nothing dialled");
            page = 2;
        } else if (ev->id == 3) {
            sos(1);
        }
        show();
        break;
    case EV_APP_BUTTON:
        if (ev->id == 2)
            page = 0, picked = -1;
        else if (ev->id == 4)
            page = 1, picked = -1;
        else if (ev->id == 5)
            ui_toast_app("Open Info from home: the card is there", 3000);
        else if (ev->id >= 10 && ev->id < 13)
            picked = (int)ev->id - 10;
        show();
        break;
    default:
        break;
    }
}
