/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 0.1 (docs/STARBITE-OS.md): the kernel's entry and its event loop.
 *
 * Start: the lock screen is drawn and presented first (fonts, the back buffer), then the interrupts and the
 * timer self test, touch, the gauge, the services and the built-in apps. The loop: acknowledge interrupts,
 * run every due timer in priority order (timer.c), hand input to the power task and the UI, deliver app
 * events (resuming frames), draw what changed, then sleep until the next deadline. Never writes storage. */
#include "sb.h"
#include "sbui.h"
#include "kern.h"
#ifdef SB_HOST
#include <stdlib.h>
#endif
#include "kinput.h"
#include "survival.h"

struct kstate K;
static uint64_t boot_ms;

static void dispatch_timer(unsigned owner, unsigned id)
{
    switch (owner) {
    case KT_INPUT:
        input_poll();
        break;
    case KT_UI:
        ui_ktimer(id);
        break;
    case KT_POWER:
        power_timer(id);
        break;
    case KT_BATTERY:
        battery_timer(id);
        break;
    case KT_HEATER:
        heater_timer(id);
        break;
    case KT_SOS:
        sos_timer(id);
        break;
    case KT_SURV:
        surv_timer(id);                     /* 1.2-alpha27: survival.c */
        break;
    case KT_WIFI:
        wifi_timer(id);
        break;
    case KT_STATE:
        settings_timer(id);
        break;
    case KT_MODEM:
        kmodem_timer(id);                   /* 1.2-alpha21: the modem's start, poll and status GETs */
        break;
    case KT_GUEST:
        guest_timer(id);                    /* 1.2: Linux on the M3 cluster */
        break;
    case KT_SELFTEST:
        selftest_timer(id);
        break;
    case KT_HOUSE:
        hw_wdt_feed();
        /* 0.1: started and never touched for 10 minutes (an unattended test boot): back to the menu */
        if (!K.any_input && !heater_running_ui() && !sos_active() && sb_ms() - K.deadman_t0 >= 600000U) {
            sb_log("sbos: no touch or key for 10 minutes since the start: back to the menu");
            power_restart(1);               /* the LED, the heater and the radio off first (0.5.12) */
        }
        kt_set(KT_HOUSE, 0, sb_ms(), K.screen_on ? 1000 : 10000, 0);
        break;
    case KT_CLOCK:
        if (id == 1) {
            alarm_timer();                  /* 1.2-alpha8: an alarm rings: buzz again */
            break;
        }
        ui_kminute();
        {
            struct sbos_time t;
            uint32_t ms = 60000;
            if (clock_now(&t)) {
                ms = (uint32_t)(60 - t.s) * 1000U + 50U;
                alarm_minute(&t);           /* 1.2-alpha8: wake-up alarms */
            }
            kt_set(KT_CLOCK, 0, sb_ms(), ms, 0);
        }
        break;
    default:
        if (owner >= KT_APP0 && owner < KT_APP0 + APP_MAX)
            app_event_send((int)(owner - KT_APP0), EV_APP_TIMER, id, 0);
        break;
    }
}

static void handle_input(void)
{
    struct kin e;
    while (kin_pop(&e)) {
        K.any_input = 1;
        selftest_input();
        if (e.type == KIN_POWER && pocket_lock && K.screen_on) {
            pocket_lock = 0;                /* the heating pocket lock: Power unlocks (the screen stays on) */
            sb_log("power: touch unlocked (Power)");
            power_input();
            kui_redraw_all();
            continue;
        }
        if (e.type == KIN_POWER) {
            power_key_short();
            continue;
        }
        if (e.type == KIN_POWER_LONG) {
            power_key_long();
            continue;
        }
        if (!K.screen_on || pocket_lock)
            continue;                       /* the screen is off, or touch locked: only the Power key counts */
        power_input();
        ui_kevent(&e);
    }
}

#ifndef SB_HOST
void sbos_spsel1(void);
#endif

void sb_main(void)
{
#ifndef SB_HOST
    sbos_spsel1();                          /* first: libsb's probe and crash vectors then use SP_EL1 */
#endif
    uint64_t t_entry = sb_ms();             /* the counter runs from power-on: S-Boot and the loader before this */
    if (!ui_init())
        sb_crash_show(ui_init_why(), 0);
    uint64_t t_ui = sb_ms();
    sb_log("Starbite OS " SBOS_VERSION);
    hw_wdt_feed();
    kt_reset();
    K.screen_on = 1;
    battery_init();                         /* one gauge read so the lock screen has the battery */
    uint64_t t_bat = sb_ms();
    ui_kinit();
    ui_kdraw();                             /* the first frame */
    {
        /* where the time to the first frame went: before Starbite OS (S-Boot, the loader's POST and launch),
         * then each of its own steps (docs/STARBITE-OS.md 7) */
        struct ub u;
        ub_init(&u);
        ub_s(&u, "sbos: first frame at ");
        ub_dec(&u, K.first_frame_ms);
        ub_s(&u, " ms since power-on: started at ");
        ub_dec(&u, t_entry);
        ub_s(&u, ", fonts and back buffer ");
        ub_dec(&u, t_ui - t_entry);
        ub_s(&u, ", battery ");
        ub_dec(&u, t_bat - t_ui);
        ub_s(&u, ", draw ");
        ub_dec(&u, K.first_frame_ms > t_bat ? K.first_frame_ms - t_bat : 0);
        ub_s(&u, " ms");
        sb_log(u.b);
    }
    boot_ms = sb_ms();
    K.deadman_t0 = boot_ms;
    hw_irq_init();
    guest_init();                           /* 1.2: the guest's doorbell (INTID 510) routed here; nothing runs yet */
    if (!sb_touch_start())
        sb_log("sbos: no touch");
    input_init();
    power_init();
    heater_init();
    sos_init();
    surv_init();                            /* 1.2-alpha27 */
    settings_restore();                     /* after power_init: the screen-off time it restores re-arms the timer */
    clock_boot_log();                       /* 1.2-alpha26: the PMIC clock, or the last known time */
    heater_boot_forensics();                /* 1.2-alpha18: the last run's lowest voltage / highest current; a
                                             * B2SOVRC left raised by a crash put back (the slot's note) */
    if (look_emergency && !sbos_selftest_enabled)
        panel_emergency(1);                 /* 0.6.1: the saved Emergency Mode's power profile (not in the self test) */
#ifdef SB_HOST
    look_emergency = getenv("SBOS_HOST_NORMAL") ? 0 : 1;   /* the host tests' layout (Emergency Mode) unless asked */
#endif
    look_init(look_saved_style);            /* 0.6: the wallpaper, once, after the first frame (the saved one) */
    kui_redraw_all();
    apps_load_builtin();
    wifi_boot_time_check();                 /* 1.2-alpha26: no clock: Wi-Fi once for the time, then off */
    {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "sbos: ready (interrupts, touch, apps) at ");
        ub_dec(&u, sb_ms());
        ub_s(&u, " ms");
        sb_log(u.b);
    }
    selftest_start();                       /* the self-test image only */
    kt_set(KT_HOUSE, 0, sb_ms(), 1000, 0);
    kt_set(KT_CLOCK, 0, sb_ms(), 1000, 0);
    hw_wdt_feed();
    uint64_t slow_logged = 0;
    for (;;) {
        hw_irq_poll();
        unsigned owner, id;
        uint64_t now = sb_ms(), turn0 = now;
        for (unsigned n = 0; n < 32 && kt_pop(now, &owner, &id); ++n)
            dispatch_timer(owner, id);
        handle_input();
        apps_service();
        handle_input();
        ui_kdraw();
        hw_irq_poll();
        /* a loop turn that takes long starves the UI and the watchdog: log it (at most once a second) */
        uint64_t took = sb_ms() - turn0;
        if (took > K.slow_turn_ms)
            K.slow_turn_ms = (uint32_t)took;
        if (took >= 200 && sb_ms() - slow_logged >= 1000) {
            slow_logged = sb_ms();
            struct ub u;
            ub_init(&u);
            ub_s(&u, "sbos: a loop turn took ");
            ub_dec(&u, took);
            ub_s(&u, " ms");
            sb_log(u.b);
        }
        uint64_t next = kt_next();
        if (next > sb_ms()) {
            if (heater_cpu0_active())
                heater_cpu0_run(next);          /* heating: cpu0 works instead of sleeping */
            else
                hw_sleep_until(next);
        }
    }
}
