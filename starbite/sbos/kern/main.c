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
#include "kinput.h"

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
    case KT_HOUSE:
        hw_wdt_feed();
        /* 0.1: started and never touched for 10 minutes (an unattended test boot): back to the menu */
        if (!K.any_input && !heater_running_ui() && !sos_active() && sb_ms() - boot_ms >= 600000U) {
            sb_log("sbos: no touch or key for 10 minutes since the start: back to the menu");
            sb_reboot(SB_INFORM_NORMAL);
        }
        kt_set(KT_HOUSE, 0, sb_ms(), K.screen_on ? 1000 : 10000, 0);
        break;
    case KT_CLOCK:
        ui_kminute();
        {
            struct sbos_time t;
            uint32_t ms = 60000;
            if (clock_now(&t))
                ms = (uint32_t)(60 - t.s) * 1000U + 50U;
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
        if (e.type == KIN_POWER) {
            power_key_short();
            continue;
        }
        if (e.type == KIN_POWER_LONG) {
            power_key_long();
            continue;
        }
        if (!K.screen_on)
            continue;                       /* the screen is off: only the Power key counts */
        power_input();
        ui_kevent(&e);
    }
}

void sb_main(void)
{
    if (!ui_init())
        sb_crash_show(ui_init_why(), 0);
    sb_log("Starbite OS " SBOS_VERSION);
    hw_wdt_feed();
    kt_reset();
    K.screen_on = 1;
    battery_init();                         /* one gauge read so the lock screen has the battery */
    ui_kinit();
    ui_kdraw();                             /* the first frame */
    {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "sbos: first frame at ");
        ub_dec(&u, K.first_frame_ms);
        ub_s(&u, " ms since power-on");
        sb_log(u.b);
    }
    boot_ms = sb_ms();
    hw_irq_init();
    if (!sb_touch_start())
        sb_log("sbos: no touch");
    input_init();
    power_init();
    heater_init();
    sos_init();
    apps_load_builtin();
    kt_set(KT_HOUSE, 0, sb_ms(), 1000, 0);
    kt_set(KT_CLOCK, 0, sb_ms(), 1000, 0);
    hw_wdt_feed();
    for (;;) {
        hw_irq_poll();
        unsigned owner, id;
        uint64_t now = sb_ms();
        for (unsigned n = 0; n < 32 && kt_pop(now, &owner, &id); ++n)
            dispatch_timer(owner, id);
        handle_input();
        apps_service();
        handle_input();
        ui_kdraw();
        hw_irq_poll();
        uint64_t next = kt_next();
        if (next > sb_ms())
            hw_sleep_until(next);
    }
}
