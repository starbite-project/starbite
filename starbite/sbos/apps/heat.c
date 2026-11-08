/* SPDX-License-Identifier: MIT
 * Heat (docs/STARBITE-OS.md 8): asks the kernel's heater for a level and shows what it does. Every limit is in
 * the kernel; this app only asks and shows. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("heat", "Heat", SBOS_P_HEAT, 0xffff9f0aU, 201);

#define ORANGE 0xffff9f0aU
#define GREY 0xff8e8e93U

static int visible;
static int surv_tripoff;                    /* 1.1-beta2: Survival's "Hardware trip OFF" switch (until a restart) */
/* 0.5.6: the battery's temperature during this heating session, every 10 s (the last 10 minutes) */
static int16_t th[60];
static int thn, was_on;
static uint64_t th_t;

static int sample_due;                      /* set by the 10 s timer: one sample per tick (a fixed grid) */

static void sample(const struct sbos_heat *h, int on)
{
    uint64_t now = uptime_ms();
    if (on && !was_on)
        thn = 0;                            /* a new session: a new curve */
    was_on = on;
    if (!on || h->bat_c10 <= -1000 || (thn && !sample_due))
        return;
    sample_due = 0;
    if (thn == 60) {
        for (int i = 1; i < 60; ++i)
            th[i - 1] = th[i];
        --thn;
    }
    th[thn++] = (int16_t)h->bat_c10;
    th_t = now;
}

static void temp(struct fmt *f, int c10)
{
    if (c10 <= -1000)
        fmt_s(f, "--");
    else
        fmt_c10(f, c10);
    fmt_s(f, " \xc2\xb0" "C");
}

static const char *const lvl[7] = { "Off", "Low", "Medium", "High", "Extreme", "Manual", "Keep warm" };
static const int targets[3] = { 100, 200, 300 };    /* Keep warm's battery targets, 0.1 C */
static const int reserves[3] = { 5, 15, 30 };       /* the call / SOS reserve, % (0: overridden by a hold) */
static const int socstops[5] = { 0, 900, 1000, 1100, 1200 };   /* Manual's SoC stop, 0.1 C (0 off) */

/* Manual (0.6.1): the vendor tables' steps (the kernel checks them again); 0 = the top normal step / GPU not used */
static const int st_a55[] = { 0, 455000, 598000, 715000, 832000, 949000, 1053000, 1248000, 1456000, 1690000, 1794000,
                              1898000, 1950000 };
static const int st_m3[] = { 0, 650000, 741000, 858000, 962000, 1066000, 1170000, 1261000, 1469000, 1586000, 1690000,
                             1794000 };
static const int st_gpu[] = { 0, 260000, 299000, 338000, 455000, 546000, 572000, 598000 };
static int ma55, mm3, mgpu = 6;            /* indices into the tables */
static int manual_page;                     /* the Manual set-up page (picked in the level row) */

static void step_text(struct fmt *f, int khz, const char *top)
{
    if (!khz) {
        fmt_s(f, top);
        return;
    }
    fmt_d(f, khz / 1000);
    fmt_s(f, " MHz");
}

static void show_manual(int running)
{
    struct fmt f;
    ui_textline(running ? "Manual runs with these steps. Off stops it." : "Pick a table step for each part, then Start.", GREY);
    fmt_init(&f);
    step_text(&f, st_a55[ma55], "1794 MHz (Samsung's cap)");
    if (st_a55[ma55] > 1794000)
        fmt_s(&f, ": above Samsung's 1794 cap");
    ui_button(10, "Small cores (A55)", f.b, 0);
    fmt_init(&f);
    step_text(&f, st_m3[mm3], "1794 MHz (the top without OCP)");
    ui_button(11, "Big cores (M3)", f.b, 0);
    fmt_init(&f);
    step_text(&f, st_gpu[mgpu], "not used");
    if (st_gpu[mgpu] == 598000)
        fmt_s(&f, ": above Android's 572 cap");
    ui_button(12, "Graphics (GPU)", f.b, 0);
    if (!running) {
        struct sbos_heat h;
        heat_status(&h);
        if (!h.no_current_stop) {
            ui_hold(20, "Hold: turn the 3.5 A current stop off", 1500, 0);
            ui_textline("The 3.5 A stop catches a short or a failing cell. Off, nothing of ours stops a runaway current.", GREY);
        } else {
            ui_button(21, "Current stop: OFF", "Tap to turn the 3.5 A stop back on", 0);
        }
        struct fmt g;
        fmt_init(&g);
        if (h.soc_stop_c10 > 0) {
            fmt_s(&g, "Stops at ");
            fmt_d(&g, h.soc_stop_c10 / 10);
            fmt_s(&g, " \xc2\xb0" "C inside (tap: off, 90..120)");
        } else {
            fmt_s(&g, "Off: the chip's own power cut only (tap: 90..120)");
        }
        ui_button(22, "SoC stop", g.b, 0);
        ui_button(13, "Start Manual", 0, UI_HALF);
        ui_button(14, "Cancel", 0, UI_HALF);
    }
    ui_textline("Only the chip's own table steps with its own voltages. Above a cap: may crash, which stops the heat.", GREY);
}

static void tile(const struct sbos_heat *h, int on)
{
    struct fmt f;
    fmt_init(&f);
    if (on) {
        fmt_s(&f, lvl[h->level >= 0 && h->level <= 6 ? h->level : 0]);
        if (h->level < HEAT_EXTREME) {
            fmt_s(&f, "  ");
            fmt_d(&f, (h->secs_left + 59) / 60);
            fmt_s(&f, " min");
        }
    } else {
        fmt_s(&f, "Off");
        if (h->bat_c10 > -1000) {
            fmt_s(&f, "  ");
            temp(&f, h->bat_c10);
        }
    }
    ui_tile(f.b);
}

static void show(void)
{
    struct sbos_heat h;
    struct fmt f;
    heat_status(&h);
    int on = h.state == HEAT_ST_RUNNING || h.state == HEAT_ST_HOLDING || h.state == HEAT_ST_PAUSED;
    if (h.bat_c10 <= -1000)
        h.bat_c10 = sensor_read(SENSOR_BATTERY_C10);
    if (h.soc_c10 <= -1000)
        h.soc_c10 = sensor_read(SENSOR_SOC_C10);
    tile(&h, on);
    sample(&h, on);
    if (!visible)
        return;
    ui_begin();
    ui_title("Heat");
    int flat = on && h.level >= HEAT_EXTREME;
    if (on)
        manual_page = 0;
    ui_level(1, "Off|Low|High|Extreme", on ? (h.level >= HEAT_EXTREME && h.level != HEAT_KEEPWARM ? 3 :
                                              h.level == HEAT_KEEPWARM ? 0 : h.level >= HEAT_HIGH ? 2 : 1) : 0);
    if (on && h.level == HEAT_KEEPWARM)
        ui_button(19, "Stop keeping warm", 0, UI_HALF);
    if (!on && !manual_page) {
        fmt_init(&f);
        fmt_s(&f, "Near ");
        fmt_d(&f, h.target_c10 / 10);
        fmt_s(&f, " \xc2\xb0" "C (tap Target to change)");
        ui_button(15, "Keep the battery warm", f.b, 0);
        fmt_init(&f);
        fmt_s(&f, "Target ");
        fmt_d(&f, h.target_c10 / 10);
        fmt_s(&f, " \xc2\xb0" "C");
        ui_button(16, f.b, 0, UI_HALF);
        ui_button(9, "Manual", "Pick the clocks", UI_HALF);
        fmt_init(&f);
        if (h.reserve_pct > 0) {
            fmt_d(&f, h.reserve_pct);
            fmt_s(&f, " % kept for a call / SOS (tap: 5, 15, 30)");
        } else {
            fmt_s(&f, "OFF: full heat runs the battery empty (tap: 15 %)");
        }
        ui_button(17, "Call / SOS reserve", f.b, 0);
        /* 1.1: the last resort (three confirmations; four with the chip's trip off) */
        fmt_init(&f);
        if (surv_tripoff) {
            fmt_s(&f, "Trip OFF: flat out, held at ");
            fmt_d(&f, h.surv_stop_c10 / 10);
            fmt_s(&f, " \xc2\xb0" "C. Asks 4 times.");
        } else {
            fmt_s(&f, "Last resort: all the battery as heat. Asks 3 times.");
        }
        ui_button(23, "Survival heat", f.b, 0);
        fmt_init(&f);
        fmt_s(&f, "Battery limit ");
        fmt_d(&f, h.surv_bat_c10 / 10);
        fmt_s(&f, " \xc2\xb0" "C (tap: 55, 58, 60)");
        ui_button(26, f.b, 0, 0);
        ui_toggle(24, "Hardware trip OFF", surv_tripoff);
        if (surv_tripoff) {
            fmt_init(&f);
            fmt_s(&f, "Limit ");
            fmt_d(&f, h.surv_stop_c10 / 10);
            fmt_s(&f, h.surv_stop_c10 >= SBOS_HEAT_SOC_HARD_C10 ? " \xc2\xb0" "C: held at 148 (tap: 125 to 150)" :
                                                                  " \xc2\xb0" "C (tap: 125 to 150)");
            ui_button(25, f.b, 0, 0);
        }
    }
    if (on && h.level >= HEAT_SURVIVAL) {
        fmt_init(&f);
        if (h.level == HEAT_SURVIVAL_X) {
            fmt_s(&f, h.trip_off ? "SURVIVAL, THE CHIP'S POWER CUT OFF: held at " :
                                   "SURVIVAL (trip off asked): held at ");
            fmt_d(&f, h.surv_stop_c10 / 10);
            fmt_s(&f, " \xc2\xb0" "C, stops 5 past it.");
        } else {
            fmt_s(&f, "SURVIVAL: holds the processor below ");
            fmt_d(&f, h.surv_target_c10 / 10);
            fmt_s(&f, " \xc2\xb0" "C (the chip cuts near 128).");
        }
        fmt_s(&f, h.gpu_on ? " GPU on." : " GPU off.");
        ui_textline(f.b, ORANGE);
    }
    fmt_init(&f);
    temp(&f, h.bat_c10);
    ui_big(on ? (h.level >= HEAT_SURVIVAL ? (h.surv_bat_c10 >= 600 ? "BATTERY  (STOPS AT 60 \xc2\xb0" "C)" :
                                             h.surv_bat_c10 >= 580 ? "BATTERY  (STOPS AT 58 \xc2\xb0" "C)" :
                                                                     "BATTERY  (STOPS AT 55 \xc2\xb0" "C)") :
                 flat ? "BATTERY  (STOPS AT 55 \xc2\xb0" "C)" : "BATTERY  (STOPS AT 42 \xc2\xb0" "C)") : "BATTERY",
           f.b, h.bat_c10 >= (flat ? 480 : 390) ? ORANGE : 0);
    fmt_init(&f);
    fmt_s(&f, "Processor ");
    temp(&f, h.soc_c10);
    if (on) {
        fmt_s(&f, "   ");
        fmt_d(&f, h.cores);
        fmt_s(&f, h.cores == 1 ? " core" : " cores");
    }
    ui_textline(f.b, 0xffffffffU);
    if (h.trip_c > 0) {
        fmt_init(&f);
        fmt_s(&f, "The chip's own power cut: reads ");
        fmt_d(&f, h.trip_c);
        fmt_s(&f, " \xc2\xb0" "C (it acted at 128 \xc2\xb0" "C in a test)");
        ui_textline(f.b, h.soc_c10 >= 1050 ? ORANGE : GREY);
    }
    if (!on && h.last_unclean) {
        fmt_init(&f);
        fmt_s(&f, "Last time the phone switched itself off while heating: ");
        fmt_d(&f, h.last_peak_soc_c10 / 10);
        fmt_s(&f, " \xc2\xb0" "C inside after ");
        fmt_d(&f, (h.last_secs + 59) / 60);
        fmt_s(&f, " min.");
        ui_textline(f.b, ORANGE);
    }
    if (h.skin_c10 > -1000) {
        fmt_init(&f);
        fmt_s(&f, "Back of the phone ");
        temp(&f, h.skin_c10);
        if (h.skin_c10 >= 460)
            fmt_s(&f, ": hot enough to burn skin");
        ui_textline(f.b, h.skin_c10 >= 460 ? ORANGE : 0xffffffffU);
        if (h.skin_c10 > 600)               /* 1.1-beta5: the beta3 run reached 74 C on the back */
            ui_big("HOT: HOLD BY THE EDGES", "Not the back or the screen", ORANGE);
    }
    fmt_init(&f);
    if (on && !flat) {
        fmt_s(&f, lvl[h.level >= 0 && h.level <= 5 ? h.level : 0]);
        fmt_s(&f, ": stops by itself in ");
        fmt_d(&f, (h.secs_left + 59) / 60);
        fmt_s(&f, " min");
        ui_textline(f.b, 0xff30d158U);
    } else if (flat) {
        fmt_init(&f);
        fmt_s(&f, h.level == HEAT_KEEPWARM ? "Until you stop it. " : "Flat out until you press Off. ");
        fmt_s(&f, "Stops only at ");
        fmt_d(&f, h.level >= HEAT_SURVIVAL ? h.surv_bat_c10 / 10 : 55);
        fmt_s(&f, " \xc2\xb0" "C battery");
        if (h.level >= HEAT_SURVIVAL) {
            fmt_s(&f, " or 2 %.");
        } else if (h.reserve_pct > 0) {
            fmt_s(&f, " or ");
            fmt_d(&f, h.reserve_pct);
            fmt_s(&f, " %.");
        } else {
            fmt_s(&f, " (no reserve).");
        }
        ui_textline(f.b, 0xff30d158U);
    }
    if (on && h.ma < -50 && h.mv > 0) {
        /* 0.6.2: what it draws and how long that lasts (to the reserve); 1.0-rc3: the gauge's measured percent under
         * full heat (sbapp.h SBOS_HEAT_MAH_PER_PCT, 16 mAh), not 30 */
        int a = -h.ma;
        fmt_init(&f);
        fmt_s(&f, "Drawing ");
        fmt_d(&f, a);
        fmt_s(&f, " mA, ");
        int mw = (int)((int64_t)a * h.mv / 1000);
        fmt_d(&f, mw / 1000);
        fmt_c(&f, '.');
        fmt_c(&f, (char)('0' + mw % 1000 / 100));
        fmt_s(&f, " W");
        int usable = h.pct - (h.reserve_pct > 0 ? h.reserve_pct : 0);
        if (h.pct >= 0 && usable > 0) {
            fmt_s(&f, "   about ");
            fmt_d(&f, usable * SBOS_HEAT_MAH_PER_PCT * 60 / a);
            fmt_s(&f, h.reserve_pct > 0 ? " min to the reserve" : " min to empty");
        }
        ui_textline(f.b, 0xffffffffU);
    }
    if (flat || manual_page)
        ui_textline("The heat is the battery's power (about 10 W at most), not the clocks: faster clocks would not warm more.", GREY);
    if (!on && h.state == HEAT_ST_STOPPED && h.why[0] == 'R')     /* "Reserve reached" */
        ui_hold(18, "Hold to go on below the reserve", 1500, 0);
    if (manual_page || (on && h.level == HEAT_MANUAL))
        show_manual(on);
    if (h.why[0])
        ui_textline(h.why, h.state == HEAT_ST_STOPPED || h.state == HEAT_ST_REFUSED ? ORANGE :
                           h.state == HEAT_ST_PAUSED ? 0xffffd60aU : 0xff30d158U);
    if (thn >= 2) {
        struct fmt lr;
        fmt_init(&lr);
        fmt_d(&lr, (thn - 1) * 10 / 60 ? (thn - 1) * 10 / 60 : (thn - 1) * 10);
        fmt_s(&lr, (thn - 1) * 10 / 60 ? (flat ? " min ago|now (stops at 55)" : " min ago|now (stops at 42)")
                                       : (flat ? " s ago|now (stops at 55)" : " s ago|now (stops at 42)"));
        int lo = 200;                       /* 20 C, or lower when the phone was colder */
        for (int i = 0; i < thn; ++i)
            if (th[i] - 20 < lo)
                lo = th[i] - 20;
        if (lo < -300)
            lo = -300;
        ui_chart("BATTERY TEMPERATURE", th, thn, lo, flat ? 560 : 450, ORANGE, lr.b);
    }
    ui_textline("WARNING: the phone gets hot to hold. Keep it in a pocket or a glove, not on bare skin.", ORANGE);
    ui_textline("About 1 % battery a minute on High, 3 % on Extreme or Survival. Not while charging.", GREY);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        timer_set(2, 10000, 1);             /* the home tile's line and the temperature curve */
        show();
        break;
    case EV_APP_SHOW:
        visible = 1;
        timer_set(1, 1000, 1);
        show();
        break;
    case EV_APP_HIDE:
        visible = 0;
        timer_cancel(1);
        break;
    case EV_APP_LEVEL:
        manual_page = 0;
        if (ev->value == 0) {
            struct sbos_heat h;
            heat_status(&h);
            if (h.level == HEAT_KEEPWARM) {
                heat_request(HEAT_OFF, 0);
                show();
                break;
            }
        }
        heat_request(ev->value == 3 ? HEAT_EXTREME : ev->value == 2 ? HEAT_HIGH : ev->value == 1 ? HEAT_LOW : HEAT_OFF, 30);
        show();
        break;
    case EV_APP_TOGGLE:
        if (ev->id == 24)
            surv_tripoff = ev->value != 0;  /* the four warnings come at the start */
        show();
        break;
    case EV_APP_HOLD:
        if (ev->id == 20)
            setting_set(SET_HEAT_CURRENT_STOP, 0);
        if (ev->id == 18) {
            struct sbos_heat h;
            heat_status(&h);
            setting_set(SET_HEAT_RESERVE_ONCE, 1);  /* 1.0-rc2: the override for this one session only */
            heat_request(h.level >= HEAT_EXTREME ? h.level : HEAT_EXTREME, 0);
        }
        show();
        break;
    case EV_APP_BUTTON: {
        struct sbos_heat h;
        heat_status(&h);
        int running = h.state == HEAT_ST_RUNNING || h.state == HEAT_ST_HOLDING || h.state == HEAT_ST_PAUSED;
        if (ev->id == 23 && !running)
            heat_request(surv_tripoff ? HEAT_SURVIVAL_X : HEAT_SURVIVAL, 0);
        else if (ev->id == 26 && !running)
            setting_set(SET_HEAT_SURV_BAT_C10, h.surv_bat_c10 >= 600 ? 550 : h.surv_bat_c10 >= 580 ? 600 : 580);
        else if (ev->id == 25 && !running)
            setting_set(SET_HEAT_SURV_STOP_C10, h.surv_stop_c10 >= SBOS_HEAT_SOC_HARD_C10 ? 1250 : (h.surv_stop_c10 / 50 + 1) * 50);
        else if (ev->id == 22 && !running) {
            int k = 0;
            for (int i = 0; i < 5; ++i)
                if (socstops[i] == h.soc_stop_c10)
                    k = i;
            setting_set(SET_HEAT_SOC_STOP_C10, socstops[(k + 1) % 5]);
        } else if (ev->id == 21 && !running)
            setting_set(SET_HEAT_CURRENT_STOP, 1);
        else if (ev->id == 19 && running)
            heat_request(HEAT_OFF, 0);
        else if (!running && ev->id == 15)
            heat_request(HEAT_KEEPWARM, 0);
        else if (!running && ev->id == 16) {
            int k = 0;
            for (int i = 0; i < 3; ++i)
                if (targets[i] == h.target_c10)
                    k = i;
            setting_set(SET_HEAT_TARGET_C10, targets[(k + 1) % 3]);
        } else if (!running && ev->id == 17) {
            int k = -1;
            for (int i = 0; i < 3; ++i)
                if (reserves[i] == h.reserve_pct)
                    k = i;
            setting_set(SET_HEAT_RESERVE, k < 0 ? 15 : reserves[(k + 1) % 3]);
        } else if (!running && ev->id == 9)
            manual_page = 1;
        else if (!running && ev->id == 14)
            manual_page = 0;
        else if (!running && ev->id == 10)
            ma55 = (ma55 + 1) % (int)(sizeof(st_a55) / sizeof(st_a55[0]));
        else if (!running && ev->id == 11)
            mm3 = (mm3 + 1) % (int)(sizeof(st_m3) / sizeof(st_m3[0]));
        else if (!running && ev->id == 12)
            mgpu = (mgpu + 1) % (int)(sizeof(st_gpu) / sizeof(st_gpu[0]));
        else if (!running && ev->id == 13) {
            setting_set(SET_HEAT_MAN_A55, st_a55[ma55]);
            setting_set(SET_HEAT_MAN_M3, st_m3[mm3]);
            setting_set(SET_HEAT_MAN_GPU, st_gpu[mgpu]);
            heat_request(HEAT_MANUAL, 0);
        }
        show();
        break;
    }
    case EV_APP_TIMER:
        if (ev->id == 2)
            sample_due = 1;
        show();
        break;
    case EV_APP_SERVICE:
        show();
        break;
    default:
        break;
    }
}
