/* SPDX-License-Identifier: MIT
 * Modem (Starbite OS 1.2-alpha21): the modem's status, receive only (the kernel's modem.c over modem-core's session
 * API). Start asks the kernel, which shows its own confirmation; Stop parks the CP. It shows the SIM, the circuit
 * (calls) and packet (data) registration, the signal and the counters; never the operator, an IMEI or a number.
 * There are no calls, SMS or data in this version. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("modem", "Modem", SBOS_P_PHONE, 0xff64d2ffU, 211);

static int visible;
static struct sbos_phone ph;

static const char *state_name(int s)
{
    switch (s) {
    case SBOS_MODEM_STARTING: return "Starting";
    case SBOS_MODEM_ON: return "On";
    case SBOS_MODEM_PARKED: return "Parked";
    case SBOS_MODEM_FAILED: return "Failed";
    default: return "Off";
    }
}

static void tile(void)
{
    struct fmt f;
    fmt_init(&f);
    fmt_s(&f, state_name(ph.state));
    if (ph.state == SBOS_MODEM_ON && ph.sim >= 0) {
        fmt_s(&f, "  SIM ");
        fmt_s(&f, ph.sim_name);
    } else if (ph.state == SBOS_MODEM_OFF) {
        fmt_s(&f, "  receive only");
    }
    ui_tile(f.b);
}

static void row(const char *label, const char *value)
{
    struct fmt f;
    fmt_init(&f);
    fmt_s(&f, label);
    fmt_s(&f, value);
    ui_textline(f.b, 0xffffffffU);
}

static void show(void)
{
    phone_state(&ph);
    tile();
    if (!visible)
        return;
    ui_begin();
    ui_title("Modem");
    uint32_t col = ph.state == SBOS_MODEM_ON ? 0xff30d158U : ph.state == SBOS_MODEM_FAILED ? 0xffff453aU
                 : ph.state == SBOS_MODEM_PARKED ? 0xffff9f0aU : 0;
    ui_big("Receive only", state_name(ph.state), col);
    if (ph.state == SBOS_MODEM_ON) {
        struct fmt f;
        row("SIM: ", ph.sim >= 0 ? ph.sim_name : "not reported yet");
        row("Calls (CS): ", ph.cs);
        row("Data (PS): ", ph.ps);
        fmt_init(&f);
        fmt_s(&f, "Signal: ");
        if (ph.bars >= 0) {
            fmt_d(&f, ph.bars);
            fmt_s(&f, ph.bars == 1 ? " bar" : " bars");
        } else {
            fmt_s(&f, "not reported yet");
        }
        ui_textline(f.b, 0xffffffffU);
        fmt_init(&f);
        fmt_s(&f, "On for ");
        fmt_dur(&f, ph.secs / 60);
        fmt_s(&f, ", ");
        fmt_d(&f, ph.queries);
        fmt_s(&f, ph.queries == 1 ? " question, " : " questions, ");
        fmt_d(&f, ph.answers);
        fmt_s(&f, ph.answers == 1 ? " answer, " : " answers, ");
        fmt_d(&f, ph.msgs);
        fmt_s(&f, ph.msgs == 1 ? " message" : " messages");
        ui_textline(f.b, 0xff8e8e93U);
        ui_button(2, "Stop", "Park the modem", UI_RED);
    } else if (ph.state == SBOS_MODEM_STARTING) {
        ui_textline("Loading the modem's software and starting it (a few seconds)...", 0xff8e8e93U);
    } else {
        if (ph.why[0])
            ui_textline(ph.why, ph.state == SBOS_MODEM_OFF ? 0xff8e8e93U : 0xffff9f0aU);
        ui_button(1, "Start", "Its status only: asks first", UI_ACCENT);
    }
    ui_textline("No calls, SMS or data in this version: for an emergency use another phone.", 0xff8e8e93U);
    ui_textline("The network's name, the IMEI and numbers are never shown.", 0xff8e8e93U);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        visible = 0;
        show();
        break;
    case EV_APP_SHOW:
        visible = 1;
        timer_set(1, 2000, 1);              /* the counters while the page is open */
        show();
        break;
    case EV_APP_HIDE:
        visible = 0;
        timer_cancel(1);
        break;
    case EV_APP_TIMER:
    case EV_APP_SERVICE:
        show();
        break;
    case EV_APP_BUTTON:
        if (ev->id == 1) {
            if (modem_power(1) == SBOS_EAGAIN)
                ui_toast_app("Not now (Heat, Linux or a self test)", 2500);
        } else if (ev->id == 2) {
            modem_power(0);
        }
        show();
        break;
    default:
        break;
    }
}
