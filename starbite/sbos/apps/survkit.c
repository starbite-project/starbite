/* SPDX-License-Identifier: MIT
 * Survival kit (Track D, 2026-11-16; kern/survkit.c): the barometer (pressure, altitude, the storm trend and Storm
 * watch), north from the sun (and the shadow stick), the water-purification timers, the heart-rate check, and the
 * knots (in the Survival guide). Opened from Emergency or from home. Nothing here calls, sends or makes a sound. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("survkit", "Survival kit", SBOS_P_SENSORS, 0xff30d158U, 207);

#define GREY 0xff8e8e93U
#define ORANGE 0xffff9f0aU
#define GREEN 0xff30d158U
#define RED 0xffff453aU
#define WHITE 0xffffffffU

enum { P_MAIN, P_BARO, P_NORTH, P_WATER, P_PULSE };
static int page, alt_typing, compass_check;

static void dec1(struct fmt *f, int v_x10)
{
    if (v_x10 < 0) {
        fmt_c(f, '-');
        v_x10 = -v_x10;
    }
    fmt_u(f, (uint64_t)(v_x10 / 10));
    fmt_c(f, '.');
    fmt_c(f, (char)('0' + v_x10 % 10));
}

static void hms(struct fmt *f, int s)
{
    if (s >= 3600) {
        fmt_u(f, (uint64_t)(s / 3600));
        fmt_s(f, " h ");
        fmt_2(f, (unsigned)(s / 60 % 60));
        fmt_s(f, " min");
        return;
    }
    fmt_u(f, (uint64_t)(s / 60));
    fmt_c(f, ':');
    fmt_2(f, (unsigned)(s % 60));
}

static const char *const pt8[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };

/* ---- the barometer ---- */
static void show_baro(void)
{
    struct fmt f;
    int r = setting_get(SET_KIT_BARO);
    ui_title("Barometer");
    if (alt_typing) {
        ui_input(21, "Your altitude now in metres (from a map or a sign), e.g. 450", 0, 6);
        return;
    }
    if (r == -2) {
        ui_big("PRESSURE", "Off", ORANGE);
        ui_textline("The sensors are off in Extreme power saving and Last 2 %. Switch it off in Emergency > "
                    "Power saving to read the barometer.", GREY);
        return;
    }
    if (r == -3) {
        ui_big("PRESSURE", "Not running", ORANGE);
        ui_textline("The sensor hub did not start: the reason is in the notifications. It needs its firmware on the "
                    "card: /STARBITE/FW/BCM4775.BIN.", GREY);
        return;
    }
    fmt_init(&f);
    if (r > 0) {
        dec1(&f, r);
        fmt_s(&f, " hPa");
    } else {
        fmt_s(&f, "Starting...");
    }
    ui_big("AIR PRESSURE", f.b, WHITE);
    int alt = setting_get(SET_KIT_ALT), ref = setting_get(SET_KIT_REF);
    if (alt > -100000) {
        fmt_init(&f);
        fmt_s(&f, "Altitude about ");
        fmt_d(&f, alt);
        fmt_s(&f, ref >> 16 ? " m (set from your known altitude)" : " m (from the standard 1013.25 hPa: weather "
                                                                     "moves it by up to a few hundred metres)");
        ui_textline(f.b, WHITE);
    }
    int t = setting_get(SET_KIT_TREND), h = setting_get(SET_KIT_HIST);
    int lv = t & 7, h3 = (t >> 3) & 1, h1 = (t >> 4) & 1;
    int d3 = ((t >> 8) & 0xfff) - 2048, d1 = ((t >> 20) & 0xfff) - 2048;
    static const char *const word[6] = { "Steady", "Rising: better weather", "Falling: weather getting worse",
                                         "Falling fast: worse weather likely within a day",
                                         "Falling very fast: a STORM is likely", "Rising fast: clearing, often windy" };
    static const uint32_t col[6] = { WHITE, GREEN, ORANGE, ORANGE, RED, GREEN };
    if (h3 || h1) {
        fmt_init(&f);
        fmt_s(&f, word[lv > 5 ? 0 : lv]);
        fmt_s(&f, h3 ? " (" : " (1 h: ");
        if ((h3 ? d3 : d1) > 0)
            fmt_c(&f, '+');
        dec1(&f, h3 ? d3 : d1);
        fmt_s(&f, h3 ? " hPa in 3 h)" : " hPa)");
        ui_textline(f.b, col[lv > 5 ? 0 : lv]);
    } else {
        ui_textline("The trend needs readings over at least an hour: switch Storm watch on, or keep this page "
                    "open.", GREY);
    }
    int n = h & 0xff;
    if (n >= 2) {                           /* the chart: up to the last 60 readings, oldest first */
        int16_t v[60];
        int k = n > 60 ? 60 : n, lo = 0x7fff, hi = -0x7fff, oldest_min = 0;
        for (int i = 0; i < k; ++i) {
            setting_set(SET_KIT_HIST_AT, k - 1 - i);
            int x = setting_get(SET_KIT_HIST_AT);
            v[i] = x < 0 ? (int16_t)-32768 : (int16_t)(x & 0xffff);
            if (x >= 0) {
                lo = v[i] < lo ? v[i] : lo;
                hi = v[i] > hi ? v[i] : hi;
                if (!i)
                    oldest_min = (x >> 16) & 0x7fff;
            }
        }
        if (hi - lo < 20) {                 /* at least 2 hPa tall, so noise does not look like weather */
            int m = (hi + lo) / 2;
            lo = m - 10;
            hi = m + 10;
        }
        fmt_init(&f);
        if (oldest_min >= 120) {
            fmt_u(&f, (uint64_t)(oldest_min / 60));
            fmt_s(&f, " h ago|now");
        } else {
            fmt_u(&f, (uint64_t)oldest_min);
            fmt_s(&f, " min ago|now");
        }
        ui_chart("Pressure (hPa x10)", v, k, lo - 2, hi + 2, 0xff64d2ffU, f.b);
    }
    int w = setting_get(SET_KIT_WATCH);
    ui_toggle(20, "Storm watch (a reading every 15 min)", w & 1);
    if (w & 2)
        ui_textline("Storm watch is paused while Extreme power saving is on.", ORANGE);
    else if (w & 1)
        ui_textline("A fast fall gives a notification and two buzzes. Costs a few seconds of the sensor hub every "
                    "15 min.", GREY);
    ui_button(22, "Set my altitude", "Type a known altitude: the barometer then shows height changes", UI_HALF);
    if (ref >> 16)
        ui_button(23, "Standard reference", "Back to 1013.25 hPa", UI_HALF);
    ui_textline("Keep the phone at one height for the trend: climbing 10 m reads like a 1.2 hPa fall.", GREY);
}

/* ---- north from the sun ---- */
static void show_north(void)
{
    struct fmt f;
    int s = setting_get(SET_KIT_SUN);
    ui_title("North from the sun");
    if (s == -2) {
        ui_big("SUN", "Location needed", ORANGE);
        ui_textline("Type your rough location in Emergency > Sun (a degree or two is enough), or use Android's last "
                    "GPS fix there.", GREY);
        ui_button(30, "Open Emergency", 0, 0);
        return;
    }
    if (s == -3 || s < 0) {
        ui_big("SUN", "The clock is not set", ORANGE);
        ui_textline("Set the time in Settings: the sun's direction comes from the time and your location.", GREY);
        return;
    }
    int az = s & 0xfff, el = ((s >> 12) & 0x7ff) - 900;
    fmt_init(&f);
    fmt_u(&f, (uint64_t)((az + 5) / 10));
    fmt_s(&f, "\xc2\xb0 ");
    fmt_s(&f, pt8[((az + 225) / 450) % 8]);
    ui_big("THE SUN IS AT", f.b, ORANGE);
    fmt_init(&f);
    if (el < -8) {
        fmt_s(&f, "The sun is down (");
        dec1(&f, -el);
        fmt_s(&f, "\xc2\xb0 below the horizon).");
        ui_textline(f.b, WHITE);
        ui_textline("At night: the Pole Star shows north (northern half of the world); the Southern Cross shows "
                    "south. See the Survival guide: Navigation.", GREY);
    } else {
        dec1(&f, el);
        fmt_s(&f, "\xc2\xb0 above the horizon.");
        ui_textline(f.b, WHITE);
        int rel = (3600 - az) % 3600;       /* turning from facing the sun to facing north, clockwise */
        fmt_init(&f);
        fmt_s(&f, "Face the sun: north is ");
        if (rel <= 1800) {
            fmt_u(&f, (uint64_t)((rel + 5) / 10));
            fmt_s(&f, "\xc2\xb0 to your right");
        } else {
            fmt_u(&f, (uint64_t)((3600 - rel + 5) / 10));
            fmt_s(&f, "\xc2\xb0 to your left");
        }
        fmt_s(&f, rel > 1350 && rel < 2250 ? " (behind you)." : ".");
        ui_big("", f.b, GREEN);
        ui_textline("The sun's direction from your location and the phone's clock: within about 1 degree. Low sun "
                    "is easiest to face; never look straight at it.", GREY);
        if (compass_check) {
            int h = setting_get(SET_SURV_HEADING);
            if (h >= 0) {
                int d = ((h & 0xffff) - az + 5400) % 3600 - 1800;
                fmt_init(&f);
                fmt_s(&f, "Top of the phone at the sun: the compass reads ");
                fmt_u(&f, (uint64_t)(((h & 0xffff) + 5) / 10 % 360));
                fmt_s(&f, "\xc2\xb0, so it is off by ");
                if (d > 0)
                    fmt_c(&f, '+');
                dec1(&f, d);
                fmt_s(&f, "\xc2\xb0 here (declination and errors).");
                ui_textline(f.b, WHITE);
            } else {
                ui_textline("The compass is starting (about 4 s)...", GREY);
            }
        }
        ui_button(31, compass_check ? "Stop the compass check" : "Check the compass",
                  "Point the top of the phone at the sun", UI_HALF);
    }
    int tm = setting_get(SET_KIT_TIMER);
    if ((tm & 0xff) == KIT_T_SHADOW_) {
        fmt_init(&f);
        fmt_s(&f, "Shadow stick: mark the tip again in ");
        hms(&f, tm >> 8);
        ui_textline(f.b, ORANGE);
        ui_button(33, "Stop the shadow stick", 0, UI_HALF);
    } else {
        ui_button(32, "Shadow stick (15 min)", "Mark the tip now; you'll be told when again", UI_HALF);
    }
    ui_textline("Shadow stick: push a stick upright in flat ground, mark the tip of its shadow, wait 15 minutes, mark "
                "again. First mark to second points east; north is to its left.", GREY);
    ui_textline("With a watch (northern half): point the hour hand at the sun; south is halfway between it and 12 "
                "(1 in summer time).", GREY);
}

/* ---- water ---- */
static const struct { int kind; const char *label, *sub; } water[] = {
    { KIT_T_BOIL_, "Boil", 0 },
    { KIT_T_CHLORINE_, "Chlorine tablets", "30 min, clear water" },
    { KIT_T_CHLORINE_COLD_, "Chlorine, cold or cloudy", "60 min" },
    { KIT_T_CLO2_, "Chlorine dioxide", "4 h (kills Cryptosporidium)" },
    { KIT_T_IODINE_, "Iodine", "30 min (60 when cold)" },
    { KIT_T_SODIS_, "Sun bottle (SODIS)", "6 h in full sun" },
    { KIT_T_SODIS_CLOUD_, "Sun bottle, cloudy", "2 days" },
};

static void show_water(void)
{
    struct fmt f;
    int tm = setting_get(SET_KIT_TIMER), kind = tm & 0xff;
    ui_title("Water purification");
    if (kind && kind != KIT_T_SHADOW_) {
        const char *name = "Timer";
        for (unsigned i = 0; i < sizeof(water) / sizeof(water[0]); ++i)
            if (water[i].kind == kind)
                name = water[i].label;
        fmt_init(&f);
        hms(&f, tm >> 8);
        fmt_s(&f, " left");
        ui_big(name, f.b, ORANGE);
        ui_textline("You'll get a notification and three buzzes when it is done, also with the screen off.", GREY);
        ui_button(41, "Stop", 0, 0);
    }
    ui_textline("Filter cloudy water through cloth first; let it settle. Then one of these:", WHITE);
    for (unsigned i = 0; i < sizeof(water) / sizeof(water[0]); ++i) {
        const char *sub = water[i].sub;
        char b[48];
        if (water[i].kind == KIT_T_BOIL_) {
            struct fmt g;
            fmt_init(&g);
            int s = setting_get(SET_KIT_BOIL_S);
            fmt_s(&g, s >= 180 ? "3 min rolling boil (above 2000 m)" : "1 min rolling boil (3 above 2000 m)");
            for (int k = 0; k <= (int)g.n && k < (int)sizeof(b); ++k)
                b[k] = g.b[k];
            b[sizeof(b) - 1] = 0;
            sub = b;
        }
        ui_button((uint32_t)(50 + i), water[i].label, sub, UI_HALF);
    }
    ui_textline("Tablets: follow the packet's dose; double the time for cold (under 5 C) or cloudy water. Sun bottle: "
                "clear PET bottles of 2 l or less, on their side, on a roof or a dark surface.", GREY);
    ui_textline("Boiling is the surest. None of these removes chemicals or salt.", GREY);
}

/* ---- the heart rate (MAX86915 on the back, next to the camera) ---- */
static void show_pulse(void)
{
    ui_title("Heart rate");
    ui_big("PULSE", "Not yet", ORANGE);
    ui_textline("The heart-rate sensor next to the camera needs its driver: it is being tested on the test phone. "
                "Until then, count by hand:", GREY);
    ui_textline("Two fingers on the wrist (thumb side) or the side of the neck. Count the beats for 15 seconds and "
                "multiply by 4. At rest an adult has 60-100 a minute.", WHITE);
    ui_textline("Over 120 at rest, under 50 with dizziness, or irregular with chest pain or breathlessness: get help.",
                ORANGE);
}

static void show(void)
{
    ui_begin();
    switch (page) {
    case P_BARO: show_baro(); break;
    case P_NORTH: show_north(); break;
    case P_WATER: show_water(); break;
    case P_PULSE: show_pulse(); break;
    default:
        ui_title("Survival kit");
        ui_button(10, "Barometer", "Pressure, altitude, storm warning", 0);
        ui_button(11, "North from the sun", "No compass needed; shadow stick", 0);
        ui_button(12, "Water purification", "Boil, tablets, sun: timers", 0);
        ui_button(13, "Heart rate", "Pulse check", UI_HALF);
        ui_button(14, "Knots", "In the Survival guide", UI_HALF);
        ui_textline("More in Emergency: SOS, the compass and level, the sun's times, power saving, USB power.", GREY);
        ui_end();
        return;
    }
    ui_button(40, "Back to the Survival kit", 0, 0);
    ui_end();
}

static void leave_page(void)
{
    if (page == P_BARO)
        setting_set(SET_KIT_BARO, 0);
    if (page == P_NORTH && compass_check)
        setting_set(SET_SURV_SENSORS, 0);
    compass_check = 0;
    alt_typing = 0;
    timer_cancel(1);
    page = P_MAIN;
}

static int parse_int(const char *s, int *out)
{
    int neg = 0, v = 0, any = 0;
    while (*s == ' ')
        ++s;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    for (; *s >= '0' && *s <= '9'; ++s, any = 1)
        if (v < 100000)
            v = v * 10 + (*s - '0');
    while (*s == ' ' || *s == 'm')
        ++s;
    if (*s || !any)
        return 0;
    *out = neg ? -v : v;
    return 1;
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        ui_tile("Barometer, north, water");
        break;
    case EV_APP_SHOW:
        show();
        break;
    case EV_APP_TIMER:
        if (ev->id == 1) {                  /* the open page: a keep-alive and a redraw each second */
            if (page == P_BARO && !alt_typing)
                setting_set(SET_KIT_BARO, 1);
            if (page == P_NORTH && compass_check)
                setting_set(SET_SURV_SENSORS, 1);
            if (page == P_MAIN || alt_typing)
                break;
            show();
        }
        break;
    case EV_APP_TOGGLE:
        if (ev->id == 20)
            setting_set(SET_KIT_WATCH, (int)ev->value);
        show();
        break;
    case EV_APP_BUTTON:
        if (ev->id >= 10 && ev->id <= 12) {
            page = P_BARO + (int)(ev->id - 10);
            if (page == P_BARO && setting_set(SET_KIT_BARO, 1) != 0)
                ui_toast_app("The sensors are off in Extreme power saving", 2500);
            timer_set(1, page == P_NORTH ? 2000 : 1000, 1);
        } else if (ev->id == 13) {
            page = P_PULSE;
        } else if (ev->id == 14) {
            app_open("guide");
        } else if (ev->id == 40) {
            leave_page();
        } else if (ev->id == 22) {
            alt_typing = 1;
        } else if (ev->id == 21) {
            char t[12];
            int v = 0, n = ui_input_get(t, sizeof(t));
            if (n > 0 && n < (int)sizeof(t)) {
                t[n] = 0;
                if (parse_int(t, &v) && setting_set(SET_KIT_ALT, v) == 0) {
                    alt_typing = 0;
                    ui_toast_app("Altitude set: changes of height now show", 2500);
                } else {
                    ui_toast_app("Type metres, -400 to 9000, while the pressure shows", 3000);
                }
            }
        } else if (ev->id == 23) {
            setting_set(SET_KIT_ALT, -100000);
        } else if (ev->id == 30) {
            app_open("emergency");
        } else if (ev->id == 31) {
            compass_check = !compass_check;
            setting_set(SET_SURV_SENSORS, compass_check);
        } else if (ev->id == 32) {
            if (setting_set(SET_KIT_TIMER, KIT_T_SHADOW_) == 0)
                ui_toast_app("Mark the shadow's tip now", 2500);
        } else if (ev->id == 33 || ev->id == 41) {
            setting_set(SET_KIT_TIMER, 0);
        } else if (ev->id >= 50 && ev->id < 50 + sizeof(water) / sizeof(water[0])) {
            int k = water[ev->id - 50].kind;
            if (setting_set(SET_KIT_TIMER, k) == 0)
                ui_toast_app("Started: you'll be told when it is done", 2000);
        }
        show();
        break;
    case EV_APP_BACK:                       /* the kernel closes the app: start at the main page next time */
        leave_page();
        break;
    default:
        break;
    }
}
