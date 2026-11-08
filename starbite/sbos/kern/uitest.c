/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS self test (docs/STARBITE-OS.md 12.6): an unattended check of every screen and app on the phone.
 * Only in the self-test image (starbite-os-selftest.img: build.sh links uitest.c a second time with
 * -DSBOS_SELFTEST_ON); the normal image has sbos_selftest_enabled = 0 and never runs it.
 *
 * 5 s after the start it opens the lock screen, home, every app (and a few of their own pages through the
 * same events a tap sends), the power menu and the screen light, one step every 700 ms, and logs to pmsg what
 * each did: "selftest: <screen>: N elements, X ms to draw, app ok / dead, fits / scrolls N px / ELEMENTS CUT OFF" (0.6.2: pages scroll; "too tall" in the summary
 * now counts pages that lost elements). Then a summary
 * and a restart to the loader's menu (where `sbfb.py reboot recovery` brings TWRP back for the log).
 *
 * Never: the flashlight LED, the heater, the alarm, brightness or panel settings, a call or a message.
 * The only write: where the store has the volatile "sbos-state" entry, one save of the current settings into it
 * (after a logged dry run), read back (settings.c settings_selftest). Any
 * touch or key stops the test at once (someone is there): the phone then just runs normally. */
#include "sb.h"
#include "kern.h"
#include "kapp.h"
#include "sbapp.h"
#ifdef SB_HOST
#include <stdlib.h>
#endif

#ifdef SBOS_SELFTEST_ON
const int sbos_selftest_enabled = 1;
#else
const int sbos_selftest_enabled = 0;
#endif

#define STEP_MS 700U

/* the in-app steps: only events that change what is shown (never a light, heat, alarm or setting) */
struct st_ev {
    const char *app, *what;
    uint32_t type, id;
    int32_t value;
};
static const struct st_ev script[] = {
    { "info", "the card editor", EV_APP_BUTTON, 2, 0 },
    { "info", "a field with the keyboard (nothing typed or saved)", EV_APP_BUTTON, 10, 0 },
    { "info", "back to the card", EV_APP_BUTTON, 3, 0 },
    { "light", "screen light strength low", EV_APP_LEVEL, 5, 0 },
    { "light", "screen light strength full", EV_APP_LEVEL, 5, 2 },
    { "clock", "world clock", EV_APP_BUTTON, 1, 0 },
    { "heat", "the Manual page (nothing started)", EV_APP_BUTTON, 9, 0 },
    { "heat", "the Manual page closed", EV_APP_BUTTON, 14, 0 },
    { "settings", "section Alarm", EV_APP_LEVEL, 9, 1 },
    { "settings", "section Heat", EV_APP_LEVEL, 9, 2 },
    { "settings", "section About", EV_APP_LEVEL, 9, 3 },
    { "settings", "section Display", EV_APP_LEVEL, 9, 0 },
};
#define NSCRIPT (int)(sizeof(script) / sizeof(script[0]))

static int active, phase, app_i, scr_i, steps, fails, overflows, dead_n, apps_ok;
static int slow_max;
static uint64_t t0;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

static int enabled(void)
{
#ifdef SB_HOST
    if (getenv("SBOS_HOST_SELFTEST"))
        return 1;
#endif
    return sbos_selftest_enabled;
}

static void next(void)
{
    kt_set(KT_SELFTEST, 0, sb_ms(), STEP_MS, 0);
}

static void logline(const char *what, int a)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "selftest: ");
    ub_s(&u, what);
    ub_s(&u, ": ");
    ub_s(&u, kui_screen_name());
    ub_s(&u, ", ");
    ub_dec(&u, (uint64_t)kui_stat_n);
    ub_s(&u, " elements, slowest draw ");
    ub_dec(&u, (uint64_t)kui_stat_ms);
    ub_s(&u, " ms");
    if (kui_stat_ms > slow_max)
        slow_max = kui_stat_ms;
    if (a >= 0) {
        ub_s(&u, apps[a].dead ? ", app DEAD" : ", app ok");
        ub_s(&u, ui_overflow ? ", ELEMENTS CUT OFF" : ui_scroll_max > 0 ? ", scrolls" : ", fits");
        if (!ui_overflow && ui_scroll_max > 0) {
            ub_c(&u, ' ');
            ub_dec(&u, (uint64_t)ui_scroll_max);
            ub_s(&u, " px");
        }
        ub_s(&u, ", handler ");
        ub_dec(&u, apps[a].used_ms);
        ub_s(&u, " ms so far");
        if (apps[a].dead)
            ++fails;
        if (ui_overflow)
            ++overflows;
    }
    if (kui_stat_n <= 0) {
        ub_s(&u, " (EMPTY)");
        ++fails;
    }
    sb_log(u.b);
    kui_stat_ms = 0;
    ++steps;
}

void selftest_start(void)
{
    if (!enabled())
        return;
    active = 1;
    phase = 0;
    t0 = sb_ms();
    sb_log("selftest: armed: starts in 5 s, a touch or key stops it");
    kt_set(KT_SELFTEST, 0, sb_ms(), 5000, 0);
}

void selftest_input(void)
{
    if (enabled()) {
        /* the self-test image: a stray touch or key must not strand an unattended phone; the 10-minute
         * return to the menu counts from the last input instead of the start */
        K.any_input = 0;
        K.deadman_t0 = sb_ms();
    }
    if (!active)
        return;
    active = 0;
    kt_cancel(KT_SELFTEST, 0);
    if (phase == 13)
        wifi_power(0);                      /* stopped during its Wi-Fi step: the radio off again */
    sb_log("selftest: a touch or key: stopped; back to the menu after 10 minutes without input");
}

static int shown_app(int i)
{
    return i < napps && !app_hidden(i);
}

void selftest_timer(unsigned id)
{
    (void)id;
    if (!active)
        return;
    power_input();                          /* keep the screen on through the test (not counted as a touch) */
    switch (phase) {
    case 0: {                               /* the start */
        struct ub u;
        ub_init(&u);
        ub_s(&u, "selftest: start, Starbite OS " SBOS_VERSION ", ");
        ub_s(&u, K.irq_mode ? "interrupts" : "POLLING");
        ub_s(&u, ", first frame ");
        ub_dec(&u, K.first_frame_ms);
        ub_s(&u, " ms, ");
        ub_dec(&u, (uint64_t)napps);
        ub_s(&u, " apps");
        sb_log(u.b);
        kui_lock();
        phase = 1;
        break;
    }
    case 1:
        logline("lock screen", -1);
        kui_home();
        phase = 2;
        break;
    case 2:
        logline("home", -1);
        app_i = 0;
        phase = 3;
        /* fall through: open the first app */
    case 3:                                 /* open the next app */
        while (app_i < napps && !shown_app(app_i))
            ++app_i;
        if (app_i >= napps) {
            kui_power_menu();
            phase = 6;
            break;
        }
        ui_overflow = 0;
        kui_open_app(app_i);
        scr_i = 0;
        phase = 4;
        break;
    case 4: {                               /* log the app's screen, then its own pages */
        struct ub u;
        ub_init(&u);
        ub_s(&u, "app ");
        ub_s(&u, apps[app_i].id);
        if (scr_i > 0) {
            ub_s(&u, " / ");
            ub_s(&u, script[scr_i - 1].what);
        }
        logline(u.b, app_i);
        if (!apps[app_i].dead && scr_i == 0)
            ++apps_ok;
        dead_n += apps[app_i].dead && scr_i == 0;
        while (scr_i < NSCRIPT && !streq(script[scr_i].app, apps[app_i].id))
            ++scr_i;
        if (scr_i < NSCRIPT && !apps[app_i].dead) {
            ui_overflow = 0;
            app_event_send(app_i, script[scr_i].type, script[scr_i].id, script[scr_i].value);
            ++scr_i;
            break;
        }
        app_event_send(app_i, EV_APP_BACK, 0, 0);
        ++app_i;
        phase = 3;
        selftest_timer(0);                  /* straight on to the next app (one step) */
        return;
    }
    case 6:
        logline("power menu", -1);
        kui_home();
        kui_open_app(app_by_id("light"));
        phase = 7;
        break;
    case 7:
        kui_light(0xff8c8c8cU);             /* the screen light (half white): no LED */
        phase = 8;
        break;
    case 8:
        logline("screen light", -1);
        kui_light(0);
        kui_home();
        phase = 9;
        break;
    case 9:
        logline("home again", -1);
        power_screen_set(0);                /* the screen off as the Power key does it: the A55 drops to 455 MHz */
        phase = 11;
        break;
    case 11: {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "selftest: screen off: A55 ");
        ub_dec(&u, hw_a55_khz() / 1000U);
        ub_s(&u, " MHz");
        sb_log(u.b);
        if (hw_a55_khz() > 470000U)
            ++fails;
        power_auto_off(1);                  /* the switch-off path up to the power-off: saved, "would switch off" */
        power_screen_set(1);
        ub_init(&u);
        ub_s(&u, "selftest: screen on: A55 ");
        ub_dec(&u, hw_a55_khz() / 1000U);
        ub_s(&u, " MHz");
        sb_log(u.b);
        if (hw_a55_khz() < 1000000U)
            ++fails;
        kui_home();
        phase = 13;
        sb_log("selftest: wifi: on, the bring-up, one scan, the saved network's join if heard, then off");
        break;
    }
    case 13: {                              /* Wi-Fi: on, the bring-up, one scan, the saved network if heard, off */
        int done = 0;
        if (!wifi_selftest_phase(&done))
            ++fails;
        if (done)
            phase = 14;
        break;
    }
    case 14:                                /* 0.7: the audio codec, read only (audio.c) */
        sb_log("selftest: audio: the codec's state, read only (an ID read only if S-Boot left it powered)");
        if (!audio_probe())
            ++fails;
        phase = 12;
        break;
    case 12: {
        {
            int sr = settings_selftest();   /* the state slot: only where the "sbos-state" entry exists */
            if (!sr)
                ++fails;
            if (sr < 0)
                sb_log("selftest: slot: no \"sbos-state\" entry: nothing written");
        }
        struct ub u;
        ub_init(&u);
        ub_s(&u, fails || overflows ? "selftest: FAILED: " : "selftest: passed: ");
        ub_dec(&u, (uint64_t)steps);
        ub_s(&u, " screens, apps ok ");
        ub_dec(&u, (uint64_t)apps_ok);
        ub_s(&u, ", dead ");
        ub_dec(&u, (uint64_t)dead_n);
        ub_s(&u, ", too tall ");
        ub_dec(&u, (uint64_t)overflows);
        ub_s(&u, ", problems ");
        ub_dec(&u, (uint64_t)fails);
        ub_s(&u, ", slowest draw ");
        ub_dec(&u, (uint64_t)slow_max);
        ub_s(&u, " ms, slowest loop turn ");
        ub_dec(&u, K.slow_turn_ms);
        ub_s(&u, " ms, ");
        ub_dec(&u, (sb_ms() - t0) / 1000U);
        ub_s(&u, " s");
        sb_log(u.b);
        sb_log("selftest: restart to the menu in 3 s");
        phase = 10;
        kt_set(KT_SELFTEST, 0, sb_ms(), 3000, 0);
        return;
    }
    case 10:
        active = 0;
        power_restart(1);
        return;
    default:
        active = 0;
        return;
    }
    next();
}
