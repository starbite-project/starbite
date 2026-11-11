/* SPDX-License-Identifier: MIT
 * Emergency (docs/STARBITE-OS.md 9.3): one place for the alarm, the card and the survival tools.
 *
 * 1.0: the alarm (the kernel's SOS service) and the medical card. Calls and messages come with the modem driver.
 * 1.2-alpha27 (kern/survival.c): USB power out (5 V from the USB-C port, hold to arm, 10 s bursts), SOS options
 * (Morse on the torch and the motor, the screen beacon), sunrise and sunset for a typed-in location, Last 2 % mode,
 * and the compass / level's status (they need the sensor hub driver). Nothing here can call or send. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("emergency", "Emergency", SBOS_P_ALARM | SBOS_P_LIGHT, 0xffff453aU, 207);

#define RED 0xffff453aU
#define GREY 0xff8e8e93U
#define ORANGE 0xffff9f0aU
#define GREEN 0xff30d158U

enum { P_MAIN, P_USB, P_SOS, P_SUN, P_LAST2 };
static int page;
static int loc_field;                       /* 1 latitude, 2 longitude being typed */

static void line(const char *a, int v, const char *b, uint32_t c)
{
    struct fmt f;
    fmt_init(&f);
    fmt_s(&f, a);
    fmt_d(&f, v);
    fmt_s(&f, b);
    ui_textline(f.b, c);
}

static void hhmm(struct fmt *f, int m)
{
    fmt_2(f, (unsigned)(m / 60));
    fmt_c(f, ':');
    fmt_2(f, (unsigned)(m % 60));
}

static void deg(struct fmt *f, int v, char pos, char neg)
{
    if (v == 0x7fffffff) {
        fmt_s(f, "not set");
        return;
    }
    int a = v < 0 ? -v : v;
    fmt_u(f, (uint64_t)(a / 100));
    fmt_c(f, '.');
    fmt_2(f, (unsigned)(a % 100));
    fmt_c(f, ' ');
    fmt_c(f, v < 0 ? neg : pos);
}

static void show_usb(void)
{
    int st = setting_get(SET_SURV_USB), info = setting_get(SET_SURV_USB_INFO);
    int secs = info & 0xff, ma = (info >> 8) & 0xfff, v = (info >> 20) & 0xf, c = (info >> 24) & 0x7f;
    ui_title("USB power out");
    if (st == 1) {
        line("ON: ", secs, " s left", GREEN);
        if (ma != 0xfff)
            line("About ", ma, " mA out of the port (from the battery's draw)", 0xffffffffU);
        if (v != 0xf)
            line("Port voltage: up to about ", v, " V", 0xffffffffU);
        if (c)
            line("Battery ", c, " \xc2\xb0" "C", 0xffffffffU);
        ui_button(2, "Stop", 0, UI_RED);
    } else {
        if (st == 2)
            line("Cooling down: ", secs, " s", ORANGE);
        else if (st < 0)
            ui_textline("The last start was refused: the reason is in the notifications.", ORANGE);
        ui_hold(1, "Hold to start 10 s of power", 1500, UI_RED);
    }
    ui_textline("5 V from the USB-C port, at most 1.5 A (the charger chip's own limit), 10 s a burst.", 0xffffffffU);
    ui_textline("Fire: fine steel wool (0000) or a thin wire on dry tinder, across a cable's bare ends.", 0xffffffffU);
    ui_textline("Never short the phone's port directly. Unplug any charger first.", ORANGE);
    ui_textline("It stops by itself on any fault, a hot battery or a low one.", GREY);
}

static void show_sos(void)
{
    int o = setting_get(SET_SURV_SOS);
    ui_title("SOS");
    ui_hold(1, "Hold to start SOS", 1000, UI_RED);
    ui_toggle(3, "Torch: Morse S-O-S", o & 1);
    ui_toggle(4, "Vibration: Morse S-O-S", (o >> 1) & 1);
    ui_textline("Screen beacon", GREY);
    ui_level(5, "White/red|White|Red|Morse", (o >> 4) & 3);
    ui_textline("3 seconds to cancel. Hold the screen to stop it.", GREY);
}

static void show_sun(void)
{
    int lat = setting_get(SET_SURV_LAT), lon = setting_get(SET_SURV_LON), r = setting_get(SET_SURV_SUN);
    struct fmt f;
    ui_title("Sun");
    if (loc_field) {
        ui_input((uint32_t)(20 + loc_field), loc_field == 1 ? "Latitude, e.g. 60.17 or -33.87" :
                                                           "Longitude, e.g. 24.94 or -70.65", 2, 10);
        return;
    }
    fmt_init(&f);
    if (r >= 0) {
        fmt_s(&f, "Sunrise ");
        hhmm(&f, r & 0xffff);
        fmt_s(&f, "   Sunset ");
        hhmm(&f, (r >> 16) & 0xffff);
        ui_big("TODAY", f.b, ORANGE);
        int day = ((r >> 16) & 0xffff) - (r & 0xffff);
        if (day > 0) {
            fmt_init(&f);
            fmt_s(&f, "Daylight ");
            fmt_u(&f, (uint64_t)(day / 60));
            fmt_s(&f, " h ");
            fmt_u(&f, (uint64_t)(day % 60));
            fmt_s(&f, " min (local time, about 2 min)");
            ui_textline(f.b, 0xffffffffU);
        }
    } else {
        ui_big("TODAY", r == -1 ? "The sun stays up" : r == -2 ? "The sun stays down" : r == -3 ? "Set your location" :
                        "The clock is not set", ORANGE);
    }
    fmt_init(&f);
    fmt_s(&f, "Latitude ");
    deg(&f, lat, 'N', 'S');
    ui_button(6, f.b, "Type it in", UI_HALF);
    fmt_init(&f);
    fmt_s(&f, "Longitude ");
    deg(&f, lon, 'E', 'W');
    ui_button(7, f.b, "Type it in", UI_HALF);
    int age = setting_get(SET_SURV_GNSS);
    if (age >= 0) {
        fmt_init(&f);
        fmt_s(&f, "Use the GPS fix (");
        if (age < 120) {
            fmt_u(&f, (uint64_t)age);
            fmt_s(&f, " min old)");
        } else if (age < 2880) {
            fmt_u(&f, (uint64_t)(age / 60));
            fmt_s(&f, " h old)");
        } else {
            fmt_u(&f, (uint64_t)(age / 1440));
            fmt_s(&f, " days old)");
        }
        ui_button(8, f.b, "Left on the card by Android", 0);
    }
    ui_textline("No GPS in Starbite OS itself: type the location in, or take a fix in Android first "
                "(it leaves it on the card). South and west are negative.", GREY);
}

static void show_last2(void)
{
    int on = setting_get(SET_SURV_LAST2), au = setting_get(SET_SURV_LAST2_AUTO);
    ui_title("Last 2 %");
    ui_toggle(8, "Last 2 % mode now", on > 0);
    ui_toggle(9, "Automatically at 3 %", au > 0);
    ui_textline("Everything off except the clock, the torch and SOS; the slowest processor speed; a dim screen.",
                0xffffffffU);
    ui_textline("Instead of switching off at 3 %, the phone runs on to 1 %.", 0xffffffffU);
}

static void show(void)
{
    ui_begin();
    switch (page) {
    case P_USB: show_usb(); break;
    case P_SOS: show_sos(); break;
    case P_SUN: show_sun(); break;
    case P_LAST2: show_last2(); break;
    default:
        break;
    }
    if (page != P_MAIN) {
        ui_button(40, "Back to Emergency", 0, 0);
        ui_end();
        return;
    }
    {
        ui_title("Emergency");
        ui_hold(3, "Hold for the alarm", 1000, UI_RED);
        ui_button(5, "Medical card", "Your card in Info", 0);
        ui_button(30, "USB power out", "5 V for a fire starter", UI_HALF);
        ui_button(31, "SOS options", "Morse, beacon", UI_HALF);
        ui_button(32, "Sun", "Sunrise, sunset", UI_HALF);
        ui_button(33, "Last 2 %", "Only the essentials", UI_HALF);
        ui_button(34, "Survival guide", "First aid, water, shelter, signals", 0);
        ui_textline("Compass and level: they need the sensor hub driver (not yet).", GREY);
        ui_textline("Calls and messages: emergency calls need the modem; nothing here calls or sends.", GREY);
    }
    ui_end();
}

static int parse_x100(const char *s, int *out)
{
    int neg = 0, v = 0, frac = 0, digits = 0, any = 0;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    for (; *s >= '0' && *s <= '9'; ++s, any = 1)
        v = v * 10 + (*s - '0');
    if (*s == '.' || *s == ',') {
        ++s;
        for (; *s >= '0' && *s <= '9'; ++s, any = 1)
            if (digits < 2) {
                frac = frac * 10 + (*s - '0');
                ++digits;
            }
    }
    if (*s || !any || v > 180)
        return 0;
    if (digits == 1)
        frac *= 10;
    *out = (v * 100 + frac) * (neg ? -1 : 1);
    return 1;
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        ui_tile("SOS, USB power, sun");
        break;
    case EV_APP_SHOW:
        show();
        break;
    case EV_APP_HOLD:
        if (ev->id == 3 || (page == P_SOS && ev->id == 1))
            sos(1);
        else if (page == P_USB && ev->id == 1) {
            setting_set(SET_SURV_USB, 1);
            timer_set(1, 500, 1);           /* the live readout */
        }
        show();
        break;
    case EV_APP_TIMER:
        if (ev->id == 1) {
            if (page != P_USB || setting_get(SET_SURV_USB) == 0)
                timer_cancel(1);
            show();
        }
        break;
    case EV_APP_TOGGLE: {
        int o = setting_get(SET_SURV_SOS);
        if (ev->id == 3)
            setting_set(SET_SURV_SOS, ev->value ? o | 1 : o & ~1);
        else if (ev->id == 4)
            setting_set(SET_SURV_SOS, ev->value ? o | 2 : o & ~2);
        else if (ev->id == 8)
            setting_set(SET_SURV_LAST2, (int)ev->value);
        else if (ev->id == 9)
            setting_set(SET_SURV_LAST2_AUTO, (int)ev->value);
        show();
        break;
    }
    case EV_APP_LEVEL:
        if (ev->id == 5) {
            int o = setting_get(SET_SURV_SOS);
            setting_set(SET_SURV_SOS, (o & ~0x30) | (int)(ev->value & 3) << 4);
        }
        show();
        break;
    case EV_APP_BUTTON:
        if (ev->id == 5)
            app_open("info");
        else if (ev->id == 34)
            app_open("guide");              /* 1.2-alpha28 */
        else if (ev->id >= 30 && ev->id <= 33) {
            page = P_USB + (int)(ev->id - 30);
            if (page == P_USB && setting_get(SET_SURV_USB) > 0)
                timer_set(1, 500, 1);
        } else if (ev->id == 2 && page == P_USB)
            setting_set(SET_SURV_USB, 0);
        else if (ev->id == 6 || ev->id == 7)
            loc_field = (int)ev->id - 5;
        else if (ev->id == 8 && page == P_SUN && setting_set(SET_SURV_GNSS, 1) != 0)
            ui_toast_app("The GPS fix could not be read from the card", 2500);
        else if (ev->id == 21 || ev->id == 22) {
            char t[16];
            int v = 0, n = ui_input_get(t, sizeof(t));
            if (n > 0 && n < (int)sizeof(t)) {
                t[n] = 0;
                if (parse_x100(t, &v) && setting_set(ev->id == 21 ? SET_SURV_LAT : SET_SURV_LON, v) == 0)
                    loc_field = 0;
                else
                    ui_toast_app(ev->id == 21 ? "Latitude: -90 to 90, e.g. 60.17" : "Longitude: -180 to 180, e.g. 24.94",
                                 2500);
            }
        }
        show();
        break;
    case EV_APP_BACK:                       /* the kernel closes the app: start at the main page next time */
        page = P_MAIN;
        loc_field = 0;
        timer_cancel(1);
        break;
    default:
        break;
    }
}
