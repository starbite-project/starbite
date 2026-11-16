/* SPDX-License-Identifier: MIT
 * Emergency (docs/STARBITE-OS.md 9.3): one place for the alarm, the card and the survival tools.
 *
 * 1.0: the alarm (the kernel's SOS service) and the medical card. Calls and messages come with the modem driver.
 * 1.2-alpha27 (kern/survival.c): USB power out (5 V from the USB-C port, hold to arm, 10 s bursts), SOS options
 * (Morse on the torch and the motor, the screen beacon), sunrise and sunset for a typed-in location, Last 2 % mode,
 * and the compass / level's status (they need the sensor hub driver). Nothing here can call or send.
 * 1.2-alpha30: Fire starter (docs/USB-FIRE-STARTER.md): 0.5 / 1 / 2 s pulses of 5 V for a pencil lead, once or
 * repeated, at the chip's 0.9 / 1.2 / 1.5 A step; armed by a switch, fired by a hold that keeps firing only while the
 * finger stays down (the kernel cuts it at the lift), with the last pulse's current, watts, port voltage and the
 * battery's temperature. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("emergency", "Emergency", SBOS_P_ALARM | SBOS_P_LIGHT, 0xffff453aU, 207);

#define RED 0xffff453aU
#define GREY 0xff8e8e93U
#define ORANGE 0xffff9f0aU
#define GREEN 0xff30d158U

enum { P_MAIN, P_USB, P_SOS, P_SUN, P_LAST2, P_COMPASS, P_LIGHT, P_FIRE };
static int page;
static int loc_field;                       /* 1 latitude, 2 longitude being typed */
static int whistle_k;                       /* 1.2-alpha29: the whistle's 3 blasts (1 s on, 0.5 s off): the step */

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

static int fire_ok;                         /* 1.2-alpha29: "I understand" for Extreme fire mode (this visit only) */

static void show_usb(void)
{
    int st = setting_get(SET_SURV_USB), info = setting_get(SET_SURV_USB_INFO), fire = setting_get(SET_SURV_FIRE) > 0;
    int secs = info & 0xff, ma = (info >> 8) & 0xfff, v = (info >> 20) & 0xf, c = (info >> 24) & 0x7f;
    ui_title(fire ? "Extreme fire mode" : "USB power out");
    if (st == 1) {
        line(fire ? "ON (continuous): " : "ON: ", secs, " s left", GREEN);
        if (ma != 0xfff)
            line("About ", ma, " mA out of the port (from the battery's draw)", 0xffffffffU);
        if (v != 0xf)
            line("Port voltage: up to about ", v, " V", 0xffffffffU);
        if (c)
            line("Battery ", c, " \xc2\xb0" "C", 0xffffffffU);
        {
            int il = setting_get(SET_SURV_USB_ILIM);
            if (il > 0)
                line("The chip's current limit: ", il, il >= 1500 ? " mA (its top step)" : " mA (not the top step)",
                     il >= 1500 ? 0xffffffffU : ORANGE);
        }
        if (fire) {
            int fi = setting_get(SET_SURV_FIRE_INFO);
            int trips = fi & 0x3ff, ml = (fi >> 16) & 0x7fff;
            if (ml != 0x7fff)
                line("Battery for about ", ml, " min of this", 0xffffffffU);
            if (trips)
                line("The chip limited the current ", trips, " times: if it keeps counting, the element needs to be "
                     "longer or thinner", ORANGE);
        }
        ui_button(2, "Stop", 0, UI_RED);
    } else {
        if (st == 2)
            line("Cooling down: ", secs, " s", ORANGE);
        else if (st < 0)
            ui_textline("The last start was refused: the reason is in the notifications.", ORANGE);
        ui_hold(1, "Hold to start 10 s of power", 1500, UI_RED);
        ui_toggle(13, "Extreme fire mode: I understand", fire_ok);
        if (fire_ok)
            ui_hold(14, "Hold for continuous power (Stop ends it, 2 min at most)", 1500, UI_RED);
    }
    ui_textline("5 V from the USB-C port, at most 1.5 A (the charger chip's own limit, never more).", 0xffffffffU);
    ui_textline("Best: nichrome wire (~15 cm coiled) or a 0.3 mm pencil lead (~2 cm), each bare wire wrapped 3-4 "
                "tight turns round it. 0.5 mm lead: start 6 cm apart.", 0xffffffffU);
    ui_textline("If the chip limits the current every second, the element conducts too well: make it longer or "
                "thinner (it then gets power only 1/10 of the time).", 0xffffffffU);
    ui_textline("A copper strand or foil is a short: the chip cuts it to 30 ms pulses, it will not glow. Sparks can "
                "still light char cloth or cotton fluff held at the contact.", 0xffffffffU);
    ui_textline("Never short the phone's port directly. Unplug any charger first. Hold only insulated parts, over "
                "stone or soil, paper ready.", ORANGE);
    ui_textline("It stops by itself on any fault, a hot battery, connector or charger, or a low battery.", GREY);
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
    ui_button(6, f.b, "Tap to type it in", 0);
    fmt_init(&f);
    fmt_s(&f, "Longitude ");
    deg(&f, lon, 'E', 'W');
    ui_button(7, f.b, "Tap to type it in", 0);
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
    ui_textline("No GPS in Starbite OS itself: type the location in (south and west are negative),", GREY);
    ui_textline("or take a GPS fix in Android first: it leaves it on the card for this page.", GREY);
}

/* minutes as "N min", "N h", "N.N days" */
static void dur(struct fmt *f, int m)
{
    if (m < 120) {
        fmt_u(f, (uint64_t)m);
        fmt_s(f, " min");
    } else if (m < 2880) {
        fmt_u(f, (uint64_t)(m / 60));
        fmt_s(f, " h");
    } else {
        fmt_u(f, (uint64_t)(m / 1440));
        fmt_c(f, '.');
        fmt_c(f, (char)('0' + m % 1440 * 10 / 1440));
        fmt_s(f, " days");
    }
}

static void est_line(const char *what, int m, int measured)
{
    struct fmt f;
    fmt_init(&f);
    fmt_s(&f, what);
    if (m < 0) {
        fmt_s(&f, "not known");
    } else {
        fmt_s(&f, "about ");
        dur(&f, m);
        if (!measured)
            fmt_s(&f, " (typical, not yet measured)");
    }
    ui_textline(f.b, 0xffffffffU);
}

static void show_last2(void)
{
    int on = setting_get(SET_SURV_LAST2), au = setting_get(SET_SURV_LAST2_AUTO), sv = setting_get(SET_SURV_SAVER);
    ui_title("Power saving");
    ui_toggle(10, "Extreme power saving", sv > 0);
    ui_textline("Everything off except the clock, the torch and SOS; the slowest processor speed; a dim screen "
                "that turns off after 15 s.", GREY);
    ui_toggle(8, "Last 2 % mode now", on > 0);
    ui_toggle(9, "Last 2 % automatically at 3 %", au > 0);
    ui_textline("Last 2 %: Extreme power saving, and instead of switching off at 3 % the phone runs on to 1 %.", GREY);
    int off = setting_get(SET_SURV_EST), son = setting_get(SET_SURV_EST_ON), ssv = setting_get(SET_SURV_EST_SAVER),
        ht = setting_get(SET_SURV_EST_HEAT);
    if (off < 0 && son < 0) {
        ui_textline("Time left: shown when the phone is not charging.", GREY);
        return;
    }
    ui_textline("Time left on this charge (to 1 %):", ORANGE);
    est_line("Screen on: ", son, 1);
    est_line("Screen off: ", off, 1);
    est_line("Extreme saving, screen off: ", ssv < 0 ? -1 : ssv & 0x3fffffff, ssv >= 0 && (ssv >> 30));
    est_line("Survival heat: ", ht, 1);
}

static void show_light(void)
{
    ui_title("Light and sound");
    ui_button(11, "Bright white screen", "The panel at its brightest: a signal or a lamp. Tap to end.", 0);
    ui_textline("As a signal mirror: in sunshine, aim the phone's back (or the screen) at the target over your "
                "outstretched fingers; the flash is seen for kilometres.", GREY);
    ui_textline("Strobe and Morse: Emergency > SOS options.", GREY);
    if (setting_get(SET_SURV_WHISTLE) < 0)
        ui_button(12, "Whistle", "Not in a test image", UI_DIM);
    else
        ui_hold(12, "Hold for the whistle (3 blasts)", 500, 0);
    if (setting_get(SET_KIT_SIREN) >= 0) {      /* Track D 11-16: the loudspeaker at full volume */
        ui_toggle(39, "Siren (loud, until switched off)", setting_get(SET_KIT_SIREN) == 1);
        ui_textline("Three blasts, a pause, three again: the international distress signal. Save the battery: "
                    "whistle in bursts.", GREY);
    }
}

/* 1.2-alpha28: the sensor hub (compass, level, steps); started while this page is open */
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

static void show_compass(void)
{
    static const char *const pt[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int st = setting_get(SET_SURV_SENSORS), h = setting_get(SET_SURV_HEADING), t = setting_get(SET_SURV_TILT),
        n = setting_get(SET_SURV_STEPS);
    struct fmt f;
    ui_title("Compass and level");
    if (st == 1) {
        ui_big("COMPASS", "Starting...", ORANGE);
        ui_textline("The sensor hub is starting (about 4 seconds).", GREY);
        return;
    }
    if (st == 3 || st == 0) {
        ui_big("COMPASS", "Not running", ORANGE);
        ui_textline("The sensor hub did not start: the reason is in the notifications.", GREY);
        ui_textline("It needs its firmware on the card: /STARBITE/FW/BCM4775.BIN.", GREY);
        ui_button(9, "Try again", 0, 0);
        return;
    }
    fmt_init(&f);
    if (h >= 0) {
        int d = h & 0xffff;
        fmt_u(&f, (uint64_t)((d + 5) / 10 % 360));
        fmt_s(&f, "\xc2\xb0  ");
        fmt_s(&f, pt[((d + 225) / 450) % 8]);
        ui_big("HEADING (MAGNETIC)", f.b, 0xffffffffU);
        if (((h >> 16) & 3) < 2)
            ui_textline("Calibrate: move the phone in a figure 8 a few times, away from metal.", ORANGE);
        ui_textline("Where the top of the phone points. Hold it flat for the best reading.", GREY);
    } else {
        ui_big("HEADING (MAGNETIC)", "--", 0xffffffffU);
        ui_textline("No heading yet (a magnet or metal nearby, or still starting).", GREY);
    }
    if (t >= 0) {
        int pitch = (t & 0xffff) - 1000, roll = ((t >> 16) & 0xffff) - 1000;
        fmt_init(&f);
        fmt_s(&f, "Level: front ");
        dec1(&f, pitch);
        fmt_s(&f, "\xc2\xb0, side ");
        dec1(&f, roll);
        fmt_s(&f, "\xc2\xb0");
        ui_textline(f.b, (pitch > -10 && pitch < 10 && roll > -10 && roll < 10) ? GREEN : 0xffffffffU);
    }
    if (n >= 0) {
        fmt_init(&f);
        fmt_s(&f, "Steps since this page opened: ");
        fmt_u(&f, (uint64_t)n);
        ui_textline(f.b, 0xffffffffU);
    }
}

/* ---- 1.2-alpha30: Fire starter. One fixed layout (the same elements in the same order in every state), so the
 * 5-a-second redraw never moves the hold under the finger. ---- */
static int fs_pulse = 1, fs_rep, fs_lim = 2, fs_armed;     /* 1 s, single, 1.5 A; the arming switch (this visit) */
static int fs_ov, fs_wool;      /* fs_wool: the steel-wool preset (3 s, single, 1.5 A, no override) */

static const char *fs_why_text(int why, uint32_t *c)
{
    *c = ORANGE;
    switch (why) {
    case FS_WHY_DONE: *c = GREEN; return "Done. Hold again for more (after the cooldown).";
    case FS_WHY_RELEASED: *c = GREY; return "Stopped: you lifted your finger.";
    case FS_WHY_ILIM: return "Stopped: the lead drew more than the limit. Move the wires 5 mm further apart "
                             "(or pick a higher limit).";
    case FS_WHY_VBUS: return "Stopped: the port's voltage collapsed. Check the two wires are not touching.";
    case FS_WHY_FAULT: return "Stopped: the charger chip flagged a fault (details in the notifications).";
    case FS_WHY_HOT: return "Stopped: too hot (battery 45 \xc2\xb0" "C, or the connector / charger). Let it cool.";
    case FS_WHY_LOWBAT: return "Stopped: the battery is too low.";
    case FS_WHY_STOP: *c = GREY; return "Stopped.";
    case FS_WHY_REFUSED: return "Did not start: the reason is in the notifications.";
    case FS_WHY_COOL: return "Not yet: it is cooling down (see the countdown).";
    case FS_WHY_FINGER: return "Did not start: keep your finger on the bar (or Power held) until it fires.";
    default: *c = GREY; return "Ready when armed.";
    }
}

static void show_fire(void)
{
    int st = setting_get(SET_SURV_FS), info = setting_get(SET_SURV_FS_INFO), last = setting_get(SET_SURV_FS_LAST);
    int w = setting_get(SET_SURV_FS_W10);
    int n = info & 0xf, mx = (info >> 4) & 0xf, out = (info >> 8) & 1, why = (info >> 9) & 0x1f;
    int left = (info >> 14) & 0xff, c10 = (info >> 22) & 0x3ff;
    int avg = last & 0xfff, peak = (last >> 12) & 0xfff, vmin = (last >> 24) & 0xf;
    int w10 = w & 0xff, sag = (w >> 8) & 0xffff;
    struct fmt f;
    uint32_t col;
    ui_title("Fire starter");
    fmt_init(&f);
    int live = setting_get(SET_SURV_FS_LIVE);
    int rearms = live & 0x3fff, secs = (live >> 14) & 0x3ff, vbc = (live >> 24) & 0x3f, open = (live >> 30) & 1;
    if (st == 1 && out && open) {
        ui_big("NOTHING IS DRAWING POWER", "LOAD OPEN: check the lead", ORANGE);
    } else if (st == 1 && out && fs_ov == 3 && !fs_wool) {
        fmt_s(&f, "MAX ");
        fmt_d(&f, secs);
        fmt_s(&f, " s");
        ui_big("KEEP HOLDING - LIFT TO STOP", f.b, RED);
    } else if (st == 1 && out) {
        fmt_s(&f, "FIRING ");
        fmt_d(&f, n + 1);
        fmt_s(&f, "/");
        fmt_d(&f, mx);
        fmt_s(&f, "  ");
        dec1(&f, left);
        fmt_s(&f, " s");
        ui_big("KEEP HOLDING - LIFT TO STOP", f.b, RED);
    } else if (st == 1) {
        fmt_s(&f, "Rest ");
        dec1(&f, left);
        fmt_s(&f, " s");
        ui_big("KEEP HOLDING FOR THE NEXT PULSE", f.b, ORANGE);
    } else if (st == 2) {
        fmt_s(&f, "Cooling ");
        fmt_d(&f, (left + 9) / 10);
        fmt_s(&f, " s");
        ui_big("WAIT", f.b, ORANGE);
    } else {
        ui_big(fs_armed ? "ARMED" : "SAFE", fs_armed ? "Hold the red bar" : "Switch on to arm", fs_armed ? RED : GREEN);
    }
    ui_toggle(53, "Arm: lead wired, tinder on it, over stone, hands on insulated parts only", fs_armed);
    ui_hold(54, fs_armed ? "HOLD TO FIRE: 1 s to start, it fires while you keep holding" :
                           "Arm first (the switch above)", 1000, UI_RED);
    /* (Power held 1 s fires too: SET_SURV_FS_KEYS) */
    ui_textline(fs_why_text(why, &col), col);
    fmt_init(&f);
    if (avg != 0xfff) {
        fmt_s(&f, "Last pulse: about ");
        fmt_d(&f, avg);
        fmt_s(&f, " mA, ");
        dec1(&f, w10);
        fmt_s(&f, " W (peak ");
        fmt_d(&f, peak == 0xfff ? 0 : peak);
        fmt_s(&f, " mA)");
    } else {
        fmt_s(&f, "Last pulse: no reading yet (pulses under 0.2 s show none)");
    }
    ui_textline(f.b, 0xffffffffU);
    fmt_init(&f);
    fmt_s(&f, "Port: ");
    fmt_s(&f, vmin == 0xf ? "not read yet" : vmin == 0 ? "COLLAPSED below 3.5 V" : vmin == 1 ? "sagged to 3.5-4.5 V" :
                                                                                         "about 5 V (good)");
    if (sag > 0) {
        fmt_s(&f, "; battery low point ");
        fmt_d(&f, sag);
        fmt_s(&f, " mV");
    }
    ui_textline(f.b, vmin == 0 ? ORANGE : 0xffffffffU);
    {
        int vb = setting_get(SET_SURV_FS_VBYP), vmv = vb & 0xffff, cw = (vb >> 16) & 0x7fff;
        fmt_init(&f);
        fmt_s(&f, "Boost (inside the phone): ");
        if (vmv > 0) {
            dec1(&f, vmv / 100);
            fmt_s(&f, " V; from the battery ");
            dec1(&f, cw);
            fmt_s(&f, " W");
        } else {
            fmt_s(&f, "not read yet");
        }
        ui_textline(f.b, 0xffffffffU);
        int sp = setting_get(SET_SURV_FS_SPLIT), lw = sp & 0x7fff, cwl = (sp >> 16) & 0x7fff;
        fmt_init(&f);
        if (avg != 0xfff && (lw || cwl)) {
            fmt_s(&f, "Into the lead about ");
            dec1(&f, lw);
            fmt_s(&f, " W, lost in the chip about ");
            dec1(&f, cwl);
            fmt_s(&f, " W");
        } else {
            fmt_s(&f, "Into the lead / in the chip: after the first pulse");
        }
        ui_textline(f.b, lw >= cwl ? GREEN : ORANGE);
        if (vmin == 0 && avg != 0xfff)
            ui_textline("Port under 3.5 V: the lead is too short / too conductive, the chip limits the current and "
                        "burns the rest inside itself. Longer gap, or a thinner spot.", ORANGE);
        else
            ui_textline(" ", GREY);     /* the layout stays fixed */
    }
    {
        int oh = setting_get(SET_SURV_FS_OHM);
        fmt_init(&f);
        fmt_s(&f, "Load: ");
        if (oh >= 0) {
            fmt_s(&f, "about ");
            dec1(&f, oh);
            fmt_s(&f, oh < 34 ? " ohm (at the edge: a little longer)" : oh <= 45 ? " ohm (ideal: 3.4-4.5)" :
                                                                       " ohm (high: shorten the coil for more heat)");
        } else {
            fmt_s(&f, oh == -2 ? "under ~3.4 ohm, the chip limits it: make the wire / coil longer" :
                                 "not measured yet (needs a pulse over 100 mA)");
        }
        ui_textline(f.b, oh >= 34 && oh <= 45 ? GREEN : oh == -2 ? ORANGE : 0xffffffffU);
    }
    fmt_init(&f);
    fmt_s(&f, "Re-arms ");
    fmt_d(&f, rearms);
    fmt_s(&f, ", time on ");
    fmt_d(&f, secs);
    fmt_s(&f, " s, boost code ");
    fmt_d(&f, vbc);
    ui_textline(f.b, GREY);
    fmt_init(&f);
    fmt_s(&f, "Battery ");
    if (c10 == 1023)
        fmt_s(&f, "?");
    else
        dec1(&f, c10);
    fmt_s(&f, " \xc2\xb0" "C (it stops at 45 \xc2\xb0" "C)");
    ui_textline(f.b, c10 != 1023 && c10 >= 400 ? ORANGE : 0xffffffffU);
    ui_toggle(57, "Steel wool preset: 3 s at 1.5 A, the chip's own retry, no override (ignores the choices below)",
              fs_wool);
    ui_textline("Pulse", GREY);
    ui_level(50, "0.5|1|2|3|5 s", fs_pulse);
    ui_textline("Pulses per hold", GREY);
    ui_level(51, "Single|Repeat (up to 6)", fs_rep);
    ui_textline("Current limit (the charger chip's own steps; 1.5 A is its top, it trips near 1.6 A)", GREY);
    ui_level(52, "0.9 A|1.2 A|1.5 A", fs_lim);
    ui_textline("Override (test phone S9 #2 only; pulses 1 s at most; risks the charger chip)", ORANGE);
    if (setting_get(SET_SURV_FS_OVOK) == 1) {
        ui_level(55, "Off|Re-arm|MAX / heater wire", fs_ov == 3 ? 2 : fs_ov ? 1 : 0);
    } else {
        fs_ov = 0;                          /* the kernel refuses it anyway: the test phone only */
        ui_level(55, "Off (Re-arm / MAX: test phone only)", 0);
    }
    ui_textline("The ceiling: about 5.0 V x 1.5 A = 7 W (measured: the boost voltage cannot be raised in this mode).",
                GREY);
    ui_textline("Keys: Volume Up / Down move, Power short selects (Arm, the choices), hold Power 1 s to fire: it fires "
                "while Power stays down. Bixby held = back.", 0xffffffffU);
    ui_textline("Heater wire: nichrome / kanthal (a hair dryer's or a vape coil), trimmed to 3.4-4.5 ohm by the Load "
                "line, MAX: steady about 7 W while held. A USB arc lighter also runs on MAX.", 0xffffffffU);
    ui_textline("Wire: 0.5 mm pencil lead, each bare wire wrapped 3-4 tight turns round it, 5-7 cm apart to start. "
                "Tinder (toilet paper) loosely round the middle.", 0xffffffffU);
    ui_textline("Tune: 'drew more than the limit' = wires 5 mm apart. Under ~1000 mA and no glow = 5 mm closer. Aim "
                "for the most mA that does not stop it.", 0xffffffffU);
    ui_textline("Lift your finger to stop at once. It also stops on any chip fault, a collapsed port, a 45 \xc2\xb0"
                "C battery or a low battery, then cools down (5 s + 3x the time on).", GREY);
    ui_textline("Unplug any charger first. Never short the port. Over stone or soil, water ready.", ORANGE);
}

static void show(void)
{
    ui_begin();
    switch (page) {
    case P_USB: show_usb(); break;
    case P_SOS: show_sos(); break;
    case P_SUN: show_sun(); break;
    case P_LAST2: show_last2(); break;
    case P_COMPASS: show_compass(); break;
    case P_LIGHT: show_light(); break;
    case P_FIRE: show_fire(); break;
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
        ui_button(33, "Power saving", "Extreme, Last 2 %, time left", UI_HALF);
        ui_button(36, "Light and sound", "Bright screen, whistle", UI_HALF);
        ui_button(37, "Fire starter", "Pulses for a pencil lead", UI_HALF);
        ui_button(34, "Survival guide", "First aid, water, shelter, signals", 0);
        ui_button(35, "Compass and level", "Heading, tilt, steps", 0);
        ui_button(38, "Survival kit", "Barometer, north from the sun, water timers, pulse", 0);   /* Track D 11-16 */
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
        if (page == P_LIGHT)
            setting_set(SET_SURV_LIGHT, 0);     /* the white screen ended */
        show();
        break;
    case EV_APP_HOLD:
        if (ev->id == 3 || (page == P_SOS && ev->id == 1))
            sos(1);
        else if (page == P_USB && ev->id == 1) {
            setting_set(SET_SURV_USB, 1);
            timer_set(1, 500, 1);           /* the live readout */
        } else if (page == P_USB && ev->id == 14 && fire_ok) {
            if (setting_set(SET_SURV_FIRE, 1) == 0)
                timer_set(1, 500, 1);       /* the readout and the keep-alive */
        } else if (page == P_FIRE && ev->id == 54 && fs_armed) {
            /* the finger is still down: the kernel fires only while it stays */
            if (fs_wool)                    /* the preset: 3 s, single, 1.5 A, the chip's own retry, no override */
                (void)setting_set(SET_SURV_FS, 3 | 3 << 8 | 1 << 12 | 1 << 13);
            else
                (void)setting_set(SET_SURV_FS, fs_pulse | (fs_rep ? 1 : 0) << 4 | (fs_lim + 1) << 8 | 1 << 12 |
                                               fs_ov << 16);
        } else if (page == P_LIGHT && ev->id == 12 && setting_set(SET_SURV_WHISTLE, 1) == 0) {
            whistle_k = 1;
            timer_set(3, 1000, 0);
        }
        show();
        break;
    case EV_APP_TIMER:
        if (ev->id == 3) {                  /* the whistle: odd steps end a blast, even ones start the next */
            if (whistle_k & 1) {
                setting_set(SET_SURV_WHISTLE, 0);
                if (++whistle_k < 6)
                    timer_set(3, 500, 0);
            } else {
                setting_set(SET_SURV_WHISTLE, 1);
                ++whistle_k;
                timer_set(3, 1000, 0);
            }
            break;
        }
        if (ev->id == 4) {                  /* the fire starter: 5 redraws a second while its page is open */
            if (page != P_FIRE) {
                timer_cancel(4);
                setting_set(SET_SURV_FS_KEYS, 0);
                break;
            }
            setting_set(SET_SURV_FS_KEYS, 54);   /* Power: short = Select, held 1 s = fire (while held) */
            show();
            break;
        }
        if (ev->id == 2) {                  /* the compass page: 5 redraws a second; a keep-alive each second */
            static unsigned k;
            if (page != P_COMPASS) {
                timer_cancel(2);
                break;
            }
            if (++k % 5 == 0)
                setting_set(SET_SURV_SENSORS, 1);
            show();
            break;
        }
        if (ev->id == 1) {
            if (setting_get(SET_SURV_FIRE) == 1)
                setting_set(SET_SURV_FIRE, 2);   /* this page in front: keep fire mode running (Stop ends it) */
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
        else if (ev->id == 10)
            setting_set(SET_SURV_SAVER, (int)ev->value);
        else if (ev->id == 13)
            fire_ok = (int)ev->value;
        else if (ev->id == 39 && setting_set(SET_KIT_SIREN, (int)ev->value) != 0)
            ui_toast_app("The speaker is busy (a call?)", 2500);
        else if (ev->id == 57)
            fs_wool = (int)ev->value;
        else if (ev->id == 53) {
            fs_armed = (int)ev->value;
            if (!fs_armed && setting_get(SET_SURV_FS) == 1)
                setting_set(SET_SURV_FS, 0);
        }
        show();
        break;
    }
    case EV_APP_LEVEL:
        if (ev->id == 55) {
            int v = (int)ev->value;
            fs_ov = v <= 0 ? 0 : v == 1 ? 1 : 3;   /* Off / Re-arm / MAX */
        } else if (ev->id >= 50 && ev->id <= 52) {
            int v = (int)ev->value;
            if (ev->id == 50)
                fs_pulse = v < 0 ? 0 : v > 4 ? 4 : v;
            else if (ev->id == 51)
                fs_rep = v != 0;
            else
                fs_lim = v < 0 ? 0 : v > 2 ? 2 : v;
        } else if (ev->id == 5) {
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
        else if (ev->id == 38)
            app_open("survkit");            /* Track D 2026-11-16 */
        else if (ev->id == 36)
            page = P_LIGHT;
        else if (ev->id == 37) {
            page = P_FIRE;
            fs_armed = 0;
            setting_set(SET_SURV_FS_KEYS, 54);
            timer_set(4, 200, 1);
        }
        else if (ev->id == 11 && page == P_LIGHT) {
            setting_set(SET_SURV_LIGHT, 1);
            ui_light(0xffffffffU);          /* any touch or key ends it: EV_APP_SHOW then */
            break;
        } else if (ev->id == 35 || (ev->id == 9 && page == P_COMPASS)) {
            page = P_COMPASS;
            setting_set(SET_SURV_SENSORS, 1);
            timer_set(2, 200, 1);
        } else if (ev->id >= 30 && ev->id <= 33) {
            page = P_USB + (int)(ev->id - 30);
            if (page == P_USB && setting_get(SET_SURV_USB) > 0)
                timer_set(1, 500, 1);
        } else if (ev->id == 40) {          /* Back to Emergency */
            if (page == P_FIRE) {
                if (setting_get(SET_SURV_FS) == 1)
                    setting_set(SET_SURV_FS, 0);
                fs_armed = 0;
                timer_cancel(4);
                setting_set(SET_SURV_FS_KEYS, 0);
            }
            if (page == P_COMPASS) {
                timer_cancel(2);
                setting_set(SET_SURV_SENSORS, 0);
            }
            if (page == P_LIGHT)
                setting_set(SET_KIT_SIREN, 0);
            page = P_MAIN;
            loc_field = 0;
            timer_cancel(1);
        } else if (ev->id == 2 && page == P_USB) {
            setting_set(SET_SURV_USB, 0);
            fire_ok = 0;
        }
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
    case EV_APP_POWER:                      /* 1.2-alpha29: the screen went off: fire mode ends with it */
        if (!ev->value && setting_get(SET_SURV_FIRE) == 1)
            setting_set(SET_SURV_FIRE, 0);
        if (!ev->value && setting_get(SET_SURV_FS) == 1)
            setting_set(SET_SURV_FS, 0);
        if (!ev->value) {
            fs_armed = 0;
            setting_set(SET_SURV_FS_KEYS, 0);
        }
        break;
    case EV_APP_BACK:                       /* the kernel closes the app: start at the main page next time */
        if (setting_get(SET_KIT_SIREN) == 1)
            setting_set(SET_KIT_SIREN, 0);
        if (setting_get(SET_SURV_FIRE) == 1)
            setting_set(SET_SURV_FIRE, 0);
        if (setting_get(SET_SURV_FS) == 1)
            setting_set(SET_SURV_FS, 0);
        fire_ok = 0;
        fs_armed = 0;
        timer_cancel(4);
        setting_set(SET_SURV_FS_KEYS, 0);
        if (page == P_COMPASS) {
            timer_cancel(2);
            setting_set(SET_SURV_SENSORS, 0);
        }
        page = P_MAIN;
        loc_field = 0;
        timer_cancel(1);
        break;
    default:
        break;
    }
}
