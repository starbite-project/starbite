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
 * 1.0-rc3: the LED-probe variant (starbite-os-ledprobe.img: also -DSBOS_SELFTEST_LED) runs only fled_probe() (the
 * S2MPB02's bus bring-up and its reads; nothing written to the chip, no screens, no heat, no Wi-Fi) and restarts.
 *
 * 1.2-alpha1: the Linux variant (starbite-os-guesttest.img: -DSBOS_SELFTEST_GUEST) arms its own 30 s watchdog (fed by
 * the loop), turns Linux apps on (no confirmation: the self-test image), runs guest.c's guest_selftest_step (two
 * cycles: the view shown with a tap that must reach the app's pixels, home, stop, the M3 cluster off; then a start
 * in the background and a stop; the heater refused while Linux runs) and restarts into recovery (TWRP: pmsg).
 *
 * Never: the flashlight LED, the heater, the alarm, brightness or panel settings, a call or a message.
 * The only write: where the store has the volatile "sbos-state" entry, one save of the current settings into it
 * (after a logged dry run), read back (settings.c settings_selftest). Any
 * touch or key stops the test at once (someone is there): the phone then just runs normally. */
#include "sb.h"
#include "kern.h"
#include "kapp.h"
#include "sbapp.h"
#include "survival.h"
#if defined(SBOS_SELFTEST_SHUBPROBE) || defined(SBOS_SELFTEST_SHUB)
#include "shub.h"
#endif
#ifdef SB_HOST
#include <stdlib.h>
#endif

#ifdef SBOS_SELFTEST_ON
const int sbos_selftest_enabled = 1;
#else
const int sbos_selftest_enabled = 0;
#endif

#define STEP_MS 700U

#if defined(SBOS_SELFTEST_GUEST) || defined(SB_HOST)
#define GST_BUILD 1
#include "sbui.h"
/* the Linux variant: the phone's starbite-os-guesttest.img, or the host's SBOS_HOST_SELFTEST_GUEST */
static int guest_variant(void)
{
#ifdef SBOS_SELFTEST_GUEST
    return 1;
#else
    return getenv("SBOS_HOST_SELFTEST_GUEST") != 0;
#endif
}
static void gst_open(void) { kui_guest_open(GUEST_APP); }
static void gst_open_term(void) { kui_guest_open(GUEST_TERM); }
static void gst_home(void) { kui_home(); }
static const uint32_t *gst_panel(void) { return ui_back(); }
static const struct guest_st_ops gst_ops = { gst_open, gst_home, gst_open_term, gst_panel };
#endif

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
    { "calc", "key 1", EV_APP_LEVEL, 12, 0 },        /* 1.2-alpha20: 1 / 3 = (the 128-bit division on the phone) */
    { "calc", "key /", EV_APP_LEVEL, 10, 3 },
    { "calc", "key 3", EV_APP_LEVEL, 12, 2 },
    { "calc", "key =", EV_APP_LEVEL, 13, 2 },
    { "calc", "C", EV_APP_BUTTON, 1, 0 },
    { "notes", "the new-note editor (nothing typed or saved)", EV_APP_BUTTON, 2, 0 },
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
    if (getenv("SBOS_HOST_SELFTEST") || getenv("SBOS_HOST_SELFTEST_GUEST"))
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
#if defined(SBOS_SELFTEST_SHUBPROBE) || defined(SBOS_SELFTEST_SHUB)
    shub_leave();                           /* stopped mid-probe: the hub in standby, every register as found */
#endif
    sb_log("selftest: a touch or key: stopped; back to the menu after 10 minutes without input");
}

int selftest_warnings;                      /* 1.2-alpha25 */

static int shown_app(int i)
{
    return i < napps && !app_is_test(i);     /* 1.2-alpha25: every app, Emergency Mode's too */
}

void selftest_timer(unsigned id)
{
    if (id == 1) {
        if (!active)
            return;
        active = 0;
        sb_logx("selftest: STALLED: still running after 180 s, phase ", (uint64_t)phase);
        sb_logx("selftest: STALLED: app index / screen index / steps ", (uint64_t)app_i << 32 | (uint64_t)scr_i << 16 | (uint64_t)steps);
        sb_log("selftest: STALLED: restart into recovery for pmsg");
        power_restart_recovery();
        return;
    }
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
        ub_s(&u, " apps, entered at EL");
        ub_dec(&u, (sb_boot.el >> 2) & 3U);     /* 1.0: the exception level the loader handed over (EL2 = a hypervisor) */
        sb_log(u.b);
#ifdef SBOS_SELFTEST_LED
        sb_log("selftest: LED probe only: the S2MPB02's bus (USI10) and its reads; nothing written to the chip, no heat");
        if (fled_probe()) {
            sb_log(fled_r63() == 0x7f ? "selftest: passed: LED probe: the chip answers, flash pulses allowed" :
                                     "selftest: passed: LED probe: the chip answers (torch; flash pulses off: 0x63)");
        } else {
            sb_logf("selftest: FAILED: LED probe: ", fled_why());
        }
        sb_log("selftest: restart to the menu in 3 s");
        phase = 10;
        kt_set(KT_SELFTEST, 0, sb_ms(), 3000, 0);
        return;
#else
#if defined(SBOS_SELFTEST_GPU) || defined(SBOS_SELFTEST_GPUALU)
        {
#ifdef SBOS_SELFTEST_GPUALU
            gpuprobe_alu = 1;
#endif
            unsigned w = hw_wdt_selftest(30);
            sb_logx(w ? "selftest: GPU ladder H2: the watchdog at 30 s, fed by the loop: seconds " :
                        "selftest: GPU ladder H2: the watchdog could not be set (the loader's stays): ", w);
            phase = 50;
            kt_set(KT_SELFTEST, 0, sb_ms(), 1000, 0);
            return;
        }
#endif
#ifdef SBOS_SELFTEST_SURVIVAL
        {
            /* 1.2-alpha27: the survival tools (survival.c): the sun, the Morse, USB power out (read only, or a 2 s
             * no-load burst when nothing is plugged in), Last 2 %, one SOS on the torch and the motor; then recovery */
            unsigned w = hw_wdt_selftest(30);
            sb_logx(w ? "selftest: survival: the watchdog at 30 s, fed by the loop: seconds " :
                        "selftest: survival: the watchdog could not be set (the loader's stays): ", w);
            phase = 100;
            kt_set(KT_SELFTEST, 0, sb_ms(), 1000, 0);
            return;
        }
#endif
#if defined(SBOS_SELFTEST_SHUBPROBE) || defined(SBOS_SELFTEST_SHUB)
        {
            /* the sensor hub (shub.c; docs/SENSOR-HUB.md): the read-only probe (section 5), or the full start with
             * 10 s of samples; every register back as found; then recovery (pmsg) */
            unsigned w = hw_wdt_selftest(30);
            sb_logx(w ? "selftest: shub: the watchdog at 30 s, fed by the loop: seconds " :
                        "selftest: shub: the watchdog could not be set (the loader's stays): ", w);
            phase = 110;
            kt_set(KT_SELFTEST, 0, sb_ms(), 1000, 0);
            return;
        }
#endif
#ifdef SBOS_SELFTEST_GPUCOMP
        {
            /* P4: the GPU compositor (gpucomp.c): fill, composite, blur, 120 motion frames; then recovery (pmsg) */
            unsigned w = hw_wdt_selftest(30);
            sb_logx(w ? "selftest: gpucomp: the watchdog at 30 s, fed by the loop and each GPU job: seconds " :
                        "selftest: gpucomp: the watchdog could not be set (the loader's stays): ", w);
            phase = 90;
            kt_set(KT_SELFTEST, 0, sb_ms(), 1000, 0);
            return;
        }
#endif
#ifdef SBOS_SELFTEST_UIBENCH
        {
            /* 1.2-alpha26 P0: what the CPU can composite for the new UI (uibench.c), then recovery (pmsg) */
            unsigned w = hw_wdt_selftest(30);
            sb_logx(w ? "selftest: uibench: the watchdog at 30 s, fed by the loop and the runs: seconds " :
                        "selftest: uibench: the watchdog could not be set (the loader's stays): ", w);
            phase = 60;
            kt_set(KT_SELFTEST, 0, sb_ms(), 1000, 0);
            return;
        }
#endif
#ifdef SBOS_SELFTEST_VIB
        {
            /* 1.2-alpha7: the vibration motor, unattended: its reads, the PWM's rate, three buzzes, a stop in the
             * middle of a pattern, then a restart into recovery (TWRP: pmsg) */
            unsigned w = hw_wdt_selftest(30);
            sb_logx(w ? "selftest: vibration: the watchdog at 30 s, fed by the loop: seconds " :
                        "selftest: vibration: the watchdog could not be set (the loader's stays): ", w);
            phase = 40;
            kt_set(KT_SELFTEST, 0, sb_ms(), 1000, 0);
            return;
        }
#endif
#ifdef SBOS_SELFTEST_PROBE
        {
            unsigned w = hw_wdt_selftest(30);
            sb_logx(w ? "selftest: heat probe H1: the watchdog at 30 s, fed by the loop: seconds " :
                        "selftest: heat probe H1: the watchdog could not be set (the loader's stays): ", w);
            phase = 30;
            kt_set(KT_SELFTEST, 0, sb_ms(), 1000, 0);
            return;
        }
#endif
#ifdef GST_BUILD
        if (guest_variant()) {
            unsigned w = hw_wdt_selftest(30);
            sb_logx(w ? "selftest: Linux (P3a): the watchdog at 30 s, fed by the loop: seconds " :
                        "selftest: Linux (P3a): the watchdog could not be set (the loader's stays): ", w);
            kui_linux_on = 1;
            sb_log("selftest: Linux apps on for the test (the self-test image: no confirmation); then a restart into recovery");
            kui_home();
            phase = 20;
            kt_set(KT_SELFTEST, 0, sb_ms(), 1500, 0);
            return;
        }
#endif
        kui_lock();
        phase = 1;
        /* 1.2-alpha19: the alpha18 self test stalled on the phone (no return in 240 s, its pmsg overwritten by the next
         * boot): the regular self test still running after 180 s logs where it is and restarts into recovery (TWRP
         * keeps pmsg). The variants have their own watchdogs and lengths */
        kt_set(KT_SELFTEST, 1, sb_ms(), 180000, 0);
        break;
#endif
    }
    case 1: {
        logline("lock screen", -1);
#if SBOS_LOCK_V25
        int nc = kui_test_os_card();        /* 1.2-alpha25: a card's restart, a dry run (logged, nothing restarts) */
        sb_logx(nc ? "selftest: lock screen: OS cards (the first one's restart, a dry run): " :
                     "selftest: lock screen: no OS cards (the catalog lists none): ", (uint64_t)nc);
#endif
        kui_home();
        phase = 2;
        break;
    }
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
        {                                   /* 1.2-alpha21: the modem is never started here; its RAM need is checked */
            struct ub m;
            ub_init(&m);
            ub_s(&m, "selftest: modem: not started (only the user starts it); free RAM ");
            ub_size(&m, sb_arena_free());
            ub_s(&m, sb_arena_free() >= (64ULL << 20) ? " (enough: 64 MiB needed)" : " (NOT enough: 64 MiB needed)");
            sb_log(m.b);
        }
        if (selftest_warnings)              /* 1.2-alpha25: the environment's (the router's DHCP), not a failure */
            sb_logx("selftest: warnings (see the WARNING lines above): ", (uint64_t)selftest_warnings);
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
#ifdef GST_BUILD
    case 20: {                              /* Linux: one step every 100 ms until it passes or fails */
        int r = guest_selftest_step(&gst_ops);
        if (!r) {
            kt_set(KT_SELFTEST, 0, sb_ms(), 100, 0);
            return;
        }
        {
            struct ub u;
            ub_init(&u);
            ub_s(&u, r > 0 ? "selftest: passed: Linux (P3a), " : "selftest: FAILED: Linux (P3a), ");
            ub_dec(&u, (sb_ms() - t0) / 1000U);
            ub_s(&u, " s; slowest loop turn ");
            ub_dec(&u, K.slow_turn_ms);
            ub_s(&u, " ms; restart into recovery in 3 s");
            sb_log(u.b);
        }
        phase = 21;
        kt_set(KT_SELFTEST, 0, sb_ms(), 3000, 0);
        return;
    }
    case 21:
        active = 0;
        kui_home();
        guest_power_off();                  /* (already off after a pass; after a failure: MSG_STOP, up to 1 s) */
        sb_logx("selftest: Linux state at the restart (0 off): ", (uint64_t)guest_state());
        power_restart_recovery();
        return;
#endif
#if defined(SBOS_SELFTEST_GPU) || defined(SBOS_SELFTEST_GPUALU)
    case 50:                                /* 1.2-alpha12 H2: one step every 100 ms (probe.c) */
        if (!gpuprobe_step()) {
            kt_set(KT_SELFTEST, 0, sb_ms(), 100, 0);
            return;
        }
        sb_logx("selftest: GPU ladder H2 done, seconds: ", (sb_ms() - t0) / 1000U);
        phase = 51;
        kt_set(KT_SELFTEST, 0, sb_ms(), 3000, 0);
        return;
    case 51:
        active = 0;
        power_restart_recovery();
        return;
#endif
#ifdef SBOS_SELFTEST_SURVIVAL
    case 100:
        if (!surv_selftest_step()) {
            kt_set(KT_SELFTEST, 0, sb_ms(), 100, 0);
            return;
        }
        phase = 101;
        kt_set(KT_SELFTEST, 0, sb_ms(), 3000, 0);
        return;
    case 101:
        sb_log("selftest: survival: done; a restart into recovery (pmsg)");
        active = 0;
        power_restart_recovery();
        return;
#endif
#if defined(SBOS_SELFTEST_SHUBPROBE) || defined(SBOS_SELFTEST_SHUB)
    case 110: {                             /* one slice a turn (at most about 100 ms: the loop feeds the watchdog) */
#ifdef SBOS_SELFTEST_SHUBPROBE
        int done = shub_probe_selftest_step();
#else
        int done = shub_selftest_step();
#endif
        if (!done) {
            kt_set(KT_SELFTEST, 0, sb_ms(), shub_wait_ms(), 0);
            return;
        }
        phase = 111;
        kt_set(KT_SELFTEST, 0, sb_ms(), 3000, 0);
        return;
    }
    case 111:
        sb_log("selftest: shub: done; a restart into recovery (pmsg)");
        active = 0;
        shub_leave();                       /* (already left; a no-op then) */
        power_restart_recovery();
        return;
#endif
#ifdef SBOS_SELFTEST_GPUCOMP
    case 90:
        if (!gpucomp_selftest_step()) {
            kt_set(KT_SELFTEST, 0, sb_ms(), 100, 0);
            return;
        }
        phase = 91;
        kt_set(KT_SELFTEST, 0, sb_ms(), 3000, 0);
        return;
    case 91:
        sb_log("selftest: gpucomp: done; a restart into recovery (pmsg)");
        active = 0;
        power_restart_recovery();
        return;
#endif
#ifdef SBOS_SELFTEST_UIBENCH
    case 60:
        if (!uibench_setup()) {
            phase = 66;
            kt_set(KT_SELFTEST, 0, sb_ms(), 2000, 0);
            return;
        }
        phase = 61;
        kt_set(KT_SELFTEST, 0, sb_ms(), 500, 0);
        return;
    case 61:
        uibench_set("one A55 (the boot core) at the screen-on clock", 0, 0, 0);
        phase = 62;
        kt_set(KT_SELFTEST, 0, sb_ms(), 500, 0);
        return;
    case 62: {
        static const unsigned a55[3] = { 1, 2, 3 };
        uibench_set("four A55 at the screen-on clock", 3, a55, 0);
        phase = 63;
        kt_set(KT_SELFTEST, 0, sb_ms(), 500, 0);
        return;
    }
    case 63: {
        static const unsigned a55[3] = { 1, 2, 3 };
        if (!hw_dvfs(DVFS_A55, 1794000U))
            sb_log("uibench: the A55 did not take 1794 MHz (ACPM): this set is at the screen-on clock");
        sb_logx("uibench: A55 now, kHz: ", hw_a55_khz());
        uibench_set("four A55 at the top step", 3, a55, 0);
        (void)hw_dvfs(DVFS_A55, 1053000U);
        sb_logx("uibench: A55 back, kHz: ", hw_a55_khz());
        phase = 66;                         /* the M3s: not here (their FlexPMU path is the heater's; a later step) */
        kt_set(KT_SELFTEST, 0, sb_ms(), 500, 0);
        return;
    }
    case 66:
        sb_log("selftest: uibench: done; a restart into recovery (pmsg)");
        active = 0;
        power_restart_recovery();
        return;
#endif
#ifdef SBOS_SELFTEST_VIB
    case 40:
        if (!vib_ready()) {
            sb_logf("selftest: FAILED: vibration: ", vib_why());
            phase = 43;
            kt_set(KT_SELFTEST, 0, sb_ms(), 2000, 0);
            return;
        }
        sb_log("selftest: vibration: three buzzes of 400 ms (400 ms apart)");
        if (vib_buzz(3, 400, 400))
            sb_logf("selftest: FAILED: vibration: ", vib_why());
        phase = 41;
        kt_set(KT_SELFTEST, 0, sb_ms(), 3000, 0);
        return;
    case 41:
        sb_log("selftest: vibration: a 5-buzz pattern stopped after the second");
        (void)vib_buzz(5, 300, 300);
        phase = 42;
        kt_set(KT_SELFTEST, 0, sb_ms(), 900, 0);
        return;
    case 42:
        vib_off();
        if (!vib_ready())                   /* 1.2-alpha11: alpha8 said "passed" after a refusal */
            sb_logf("selftest: FAILED: vibration: ", vib_why());
        else
            sb_logx(vib_on() ? "selftest: FAILED: vibration: still on after the stop " :
                               "selftest: passed: vibration (the motor ran, then off after the stop); felt? seconds ",
                    (sb_ms() - t0) / 1000U);
        phase = 43;
        kt_set(KT_SELFTEST, 0, sb_ms(), 2000, 0);
        return;
    case 43:
        active = 0;
        vib_off();
        power_restart_recovery();
        return;
#endif
#ifdef SBOS_SELFTEST_PROBE
    case 30:                                /* 1.2-alpha5 probe H1: one step every 100 ms (probe.c) */
        if (!heatprobe_step()) {
            kt_set(KT_SELFTEST, 0, sb_ms(), 100, 0);
            return;
        }
        sb_logx("selftest: heat probe H1 done, seconds: ", (sb_ms() - t0) / 1000U);
        phase = 31;
        kt_set(KT_SELFTEST, 0, sb_ms(), 3000, 0);
        return;
    case 31:
        active = 0;
        kui_light(0);
        panel_boost(4, 0);
        power_restart_recovery();
        return;
#endif
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
