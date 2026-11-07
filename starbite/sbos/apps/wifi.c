/* SPDX-License-Identifier: MIT
 * Wi-Fi (docs/STARBITE-OS.md 9.4), 0.4: the radio on and off, the networks around (strongest first), joining an
 * open one, its address and whether the gateway answers. Secured networks are listed but cannot be joined yet. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("wifi", "Wi-Fi", SBOS_P_NET, 0xff0a84ffU, 208);

#define GREY 0xff8e8e93U
#define ORANGE 0xffff9f0aU
#define GREEN 0xff30d158U

static struct sbos_wifi_net nets[SBOS_WIFI_NETS];
static int nn, visible;
static int page, pick = -1;                 /* page 1: the passphrase for nets[pick] */

static void name_of(char *o, unsigned cap, const uint8_t *s, unsigned n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < n && k + 1 < cap; ++i)
        o[k++] = s[i] < 0x20 || s[i] == 0x7f ? '?' : (char)s[i];
    o[k] = 0;
}

static const char *strength(int rssi)
{
    return rssi >= -55 ? "strong" : rssi >= -70 ? "good" : rssi >= -80 ? "weak" : "very weak";
}

static void ip_s(struct fmt *f, const uint8_t ip[4])
{
    for (int i = 0; i < 4; ++i) {
        if (i)
            fmt_c(f, '.');
        fmt_d(f, ip[i]);
    }
}

static void show(void)
{
    struct sbos_wifi s;
    struct fmt f;
    char nm[40];
    wifi_state(&s);
    ui_tile(s.state == SBOS_WIFI_CONNECTED ? "Connected" : s.state == SBOS_WIFI_OFF ? "Off" :
            s.state == SBOS_WIFI_FAILED ? "Did not start" : "On");
    if (!visible)
        return;
    nn = s.state >= SBOS_WIFI_UP && s.state != SBOS_WIFI_FAILED ? wifi_list(nets, SBOS_WIFI_NETS) : 0;
    if (nn < 0)
        nn = 0;
    if (page == 1 && pick >= 0 && pick < nn && s.state == SBOS_WIFI_UP) {
        ui_begin();
        ui_title("Join a network");
        name_of(nm, sizeof(nm), nets[pick].ssid, nets[pick].ssid_len);
        ui_big("SECURED NETWORK", nm, 0);
        ui_input(5, "Passphrase", 1, 63);
        ui_textline("Kept in this phone's memory until a restart. Never written to the log.", GREY);
        ui_button(3, "Cancel", 0, UI_HALF);
        ui_end();
        return;
    }
    page = 0;
    ui_begin();
    ui_title("Wi-Fi");
    ui_toggle(1, "Wi-Fi", s.state != SBOS_WIFI_OFF && s.state != SBOS_WIFI_FAILED);
    if (s.state == SBOS_WIFI_STARTING) {
        ui_big("STARTING", "Turning on", 0);
        ui_textline("The radio starts in a few seconds, then looks for networks.", GREY);
    } else if (s.state == SBOS_WIFI_CONNECTED || s.state == SBOS_WIFI_DHCP || s.state == SBOS_WIFI_JOINING) {
        name_of(nm, sizeof(nm), s.ssid, s.ssid_len <= 32 ? s.ssid_len : 32);
        ui_big(s.state == SBOS_WIFI_CONNECTED ? "CONNECTED" : s.state == SBOS_WIFI_DHCP ? "GETTING AN ADDRESS" : "JOINING",
               nm, s.state == SBOS_WIFI_CONNECTED ? GREEN : 0);
        if (s.state == SBOS_WIFI_CONNECTED) {
            fmt_init(&f);
            fmt_s(&f, "Address ");
            ip_s(&f, s.ip);
            ui_textline(f.b, 0xffffffffU);
            fmt_init(&f);
            if (s.ping_ms >= 0) {
                fmt_s(&f, "The router answers in ");
                fmt_d(&f, s.ping_ms);
                fmt_s(&f, " ms");
            } else {
                fmt_s(&f, "Asking the router...");
            }
            ui_textline(f.b, s.ping_ms >= 0 ? GREEN : GREY);
        }
    } else if (s.state == SBOS_WIFI_OFF) {
        ui_textline("Turn Wi-Fi on to see the networks around and join one.", GREY);
        ui_textline("Heating and Wi-Fi do not run at the same time in this version.", GREY);
    } else if (s.state == SBOS_WIFI_FAILED) {
        ui_textline(s.why[0] ? s.why : "The radio did not start.", ORANGE);
    }
    if (s.state == SBOS_WIFI_SCANNING)
        ui_textline("Looking for networks...", GREY);
    if (s.state == SBOS_WIFI_UP || s.state == SBOS_WIFI_SCANNING) {
        int shown = nn > 5 ? 5 : nn;
        for (int i = 0; i < shown; ++i) {
            name_of(nm, sizeof(nm), nets[i].ssid, nets[i].ssid_len);
            fmt_init(&f);
            fmt_s(&f, nets[i].saved ? "Saved \xc2\xb7 " : nets[i].secure ? "Secured \xc2\xb7 " : "Open \xc2\xb7 ");
            fmt_s(&f, strength(nets[i].rssi));
            fmt_s(&f, nets[i].band5 ? " \xc2\xb7 5 GHz" : " \xc2\xb7 2.4 GHz");
            ui_button((uint32_t)(10 + i), nm, f.b, nets[i].saved ? UI_ACCENT : 0);
        }
        if (s.state == SBOS_WIFI_UP && !nn)
            ui_textline("No networks heard.", GREY);
        if (s.state == SBOS_WIFI_UP)
            ui_button(2, "Look again", 0, UI_HALF);
        if (s.why[0])
            ui_textline(s.why, ORANGE);
    }
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
        show();
        break;
    case EV_APP_HIDE:
        visible = 0;
        page = 0;
        break;
    case EV_APP_TOGGLE:
        if (ev->id == 1 && wifi_power(ev->value) == SBOS_EAGAIN)
            ui_toast_app("Not now (heating?)", 2500);
        show();
        break;
    case EV_APP_BUTTON:
        if (ev->id == 2) {
            wifi_scan();
        } else if (ev->id == 3) {
            page = 0;
        } else if (ev->id == 5 && page == 1) {          /* the keyboard's Join */
            char pw[65];
            int n = ui_input_get(pw, sizeof(pw));
            int r = n >= 8 ? wifi_join(pick, pw) : SBOS_EINVAL;
            for (unsigned i = 0; i < sizeof(pw); ++i)
                pw[i] = 0;
            if (r == SBOS_EINVAL)
                ui_toast_app("A passphrase has 8 to 63 characters", 2500);
            else
                page = 0;
        } else if (ev->id >= 10 && ev->id < 10 + (uint32_t)nn) {
            int r = wifi_join((int)ev->id - 10, 0);
            if (r == SBOS_EPERM) {                      /* secured, no passphrase known: ask for it */
                page = 1;
                pick = (int)ev->id - 10;
            }
        }
        show();
        break;
    case EV_APP_SERVICE:
        show();
        break;
    default:
        break;
    }
}
