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
#include "snd.h"
#ifdef SB_HOST
#include <stdlib.h>
#endif
#include "kinput.h"
#ifdef SBOS_NEXT
/* "Starbite OS Next" (starbite-next images only): the sbtk shell is the UI (ui/kern_next.c) */
void next_kinit(void), next_kdraw(void), next_kevent(const void *e), next_ktimer(unsigned id), next_kminute(void);
#define ui_kinit next_kinit
#define ui_kdraw next_kdraw
#define ui_kevent next_kevent
#define ui_ktimer next_ktimer
#define ui_kminute next_kminute
#endif
#include "survival.h"
#include "usb.h"
#include "sd.h"
#include "sdclk.h"

struct kstate K;
const char *sbos_film_now;
uint64_t sbos_film_t0;          /* 8c+: the self test's current film (kern_next.c); 0 outside the films */
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
#ifndef SB_HOST
    case KT_CHARGE:
        charge_timer(id);                   /* Track A 9c: the charger watchdog, the stock currents (charge.c) */
        break;
    case KT_USB:
        usb_timer(id);                      /* USB device mode: the cable, the DWC3, the console (usb.c) */
        break;
#endif
    case KT_HOUSE:
        hw_wdt_feed();
        /* 0.1: started and never touched for 10 minutes (an unattended test boot): back to the menu */
        /* Track A, 1.0: not in shell mode, where Starbite OS is the phone's own system (an untouched phone stays on) */
        if (!K.any_input && !heater_running_ui() && !sos_active() && sb_ms() - K.deadman_t0 >= 600000U &&
            !(sb_fw.valid && (sb_fw.flags & SBFW_TF_SHELL))) {
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

const char *k_turn_piece;                   /* 8v: (kern.h) */
unsigned k_turn_piece_ms;

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
        /* 8q: a touch on the dimmed screen brings the brightness back and is not the UI's (iOS): dropped until lifted */
        static int undim_touch;
        if (e.id == 0 && e.type == KIN_PRESS && power_undim()) {
            undim_touch = 1;
            continue;
        }
        if (undim_touch && e.id == 0) {
            if (e.type == KIN_RELEASE)
                undim_touch = 0;
            if (e.type == KIN_PRESS || e.type == KIN_MOVE || e.type == KIN_RELEASE)
                continue;
        }
        power_input();
        if (e.id != 0 && (e.type == KIN_PRESS || e.type == KIN_MOVE || e.type == KIN_RELEASE)) {
            if (kui_finger_hook)            /* 8a: a second (third...) finger: the UI's multi-touch, if it has one */
                kui_finger_hook(e.type, e.id, e.x, e.y);
            continue;
        }
        ui_kevent(&e);
    }
}
#ifdef SBOS_NEXT
/* P3k (Next only): the touch read and handed to the UI now, at the start of a frame, so every frame sees the
 * finger where it is (the 16 ms input timer alone beat against the 60 Hz frames: a frame without a new position
 * every ~0.4 s) */
void next_input_now(void)
{
    input_poll();
    handle_input();
}
#endif

#ifndef SB_HOST
void sbos_spsel1(void);
#endif

/* 8d (Track A, the drivers' go: run 13 "same data, fast path 23.25 MB/s (old 3.01 MB/s)", CRC-equal over 8 MiB of the
 * store; run 14 a write through sd_write's wrapper verified): the card's fast reads on. Writes keep 6.25 MHz default
 * speed (sd.c's wrapper); a card without High Speed gets S-Boot's divider back (6.25 MHz, as before) */
#define SBOS_SD_FAST

void sb_main(void)
{
#ifndef SB_HOST
    sbos_spsel1();                          /* first: libsb's probe and crash vectors then use SP_EL1 */
#if defined(SBOS_SD_FAST) && !defined(SBOS_SD_CLK)
#define SBOS_SD_CLK                         /* fast reads need the raised card clock (run 11: 6 MHz on both paths) */
#endif
#ifdef SBOS_SD_CLK                          /* 8a2: off until the drivers' probe (sdprobe.c, which raises it for its own
                                             * fast-path read and puts it back) passes on the phone */
    (void)sdclk_raise();                    /* kern/sdclk.c: the card's CMU divider /16 -> /1 (bus 12.5 -> 200 MHz) at
                                             * its first sd_up, the card clock stopped; nothing is touched here */
#endif
#ifdef SBOS_SD_FAST                         /* Track A (8a2): off until the drivers' probe says the fast path is ready (run 10:
                                             * "under 10 MB/s"); the OS store is on this card, so no default change before that */
    sd_set_fast(SD_FAST_ON);                /* the card's fast reads (libsb sd.c: CMD6 high speed + IDMAC) from its first
                                             * sd_up on; nothing touches the card here. A failure: 25 MHz PIO, one log line */
#endif
#endif
    uint64_t t_entry = sb_ms();             /* the counter runs from power-on: S-Boot and the loader before this */
    kmodem_reserve();                       /* P3n: the modem's 2.25 MiB before the UI's heap takes the arena */
#ifdef SBOS_KERNEL
    snd_reserve();                          /* the drivers' sound: Calliope's files' 3 MiB RAM copy, same reason */
#endif
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
    sb_gpu_wait_hook = guest_pump;          /* cp7n: a booting guest's small reads served while a frame waits on the GPU, with
                                             * Track E's budget (one read <= 64 KiB a call, at most 2 ms per 16.7 ms). 7l's
                                             * unbudgeted pump made 25-31 ms GPU waits while Linux booted (7m: off) */
    if (!sb_touch_start())
        sb_log("sbos: no touch");
    input_init();
    power_init();
    heater_init();
    sos_init();
    surv_init();                            /* 1.2-alpha27 */
#ifndef SB_HOST
    charge_init();                          /* Track A 9c: after surv_init (its USB output owns the charger) */
    usb_init();                             /* USB device mode (usb.c): watches the cable when SBOS_USB_AUTO */
#endif
    settings_restore();                     /* after power_init: the screen-off time it restores re-arms the timer */
    clock_boot_log();                       /* 1.2-alpha26: the PMIC clock, or the last known time */
    heater_boot_forensics();                /* 1.2-alpha18: the last run's lowest voltage / highest current; a
                                             * B2SOVRC left raised by a crash put back (the slot's note) */
    if (look_emergency && !sbos_selftest_enabled)
        panel_emergency(1);                 /* 0.6.1: the saved Emergency Mode's power profile (not in the self test) */
#ifdef SB_HOST
    look_emergency = getenv("SBOS_HOST_NORMAL") ? 0 : 1;   /* the host tests' layout (Emergency Mode) unless asked */
#endif
#ifndef SBOS_NEXT
    look_init(look_saved_style);            /* 0.6: the wallpaper, once, after the first frame (the saved one) */
#else
    /* P3n: not in Next. The sbtk shell draws its own wallpaper; look's two 1440 x 2960 buffers (17 MB each) took most
     * of the 24 MiB the UI heap leaves to the kernel (the first fits, the second fails, the first stays taken): the
     * arena was at 54 KiB when the user turned Cellular on (P3m) and Wi-Fi could not start either */
    sb_log("look: skipped in Next (the shell draws its own wallpaper)");
#endif
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
        k_turn_piece = 0;                   /* 8v: the turn's slowest named shell piece, from here */
        k_turn_piece_ms = 0;
        /* cp7 (Track B's finding: 490-670 ms turns in most self-test films, not the shell's): the slowest piece of a
         * turn, named in its log line (a timer's owner / id, input, the apps, the UI's draw) */
        uint64_t slow_ms = 0, t_p;
        unsigned slow_owner = 0, slow_id = 0;
        const char *slow_what = "-";
#define SLOW_PIECE(what, o, i) do { uint64_t d_ = sb_ms() - t_p; if (d_ > slow_ms) { slow_ms = d_; slow_what = (what); slow_owner = (o); slow_id = (i); } } while (0)
        for (unsigned n = 0; n < 32 && kt_pop(now, &owner, &id); ++n) {
            t_p = sb_ms();
            dispatch_timer(owner, id);
            SLOW_PIECE("timer", owner, id);
        }
        t_p = sb_ms();
        handle_input();
        SLOW_PIECE("input", 0, 0);
        t_p = sb_ms();
        apps_service();
        SLOW_PIECE("the apps", 0, 0);
        t_p = sb_ms();
        handle_input();
        SLOW_PIECE("input", 0, 0);
        t_p = sb_ms();
        ui_kdraw();
        SLOW_PIECE("the UI's draw", 0, 0);
#undef SLOW_PIECE
        hw_irq_poll();
        /* a loop turn that takes long starves the UI and the watchdog: log it (at most once a second) */
        uint64_t took = sb_ms() - turn0;
        if (took > K.slow_turn_ms)
            K.slow_turn_ms = (uint32_t)took;
        if (!(slow_what[0] == 't' && slow_owner == KT_SELFTEST)) {   /* cp7k: the self test's summary (the pmsg ring
                                                                       * may drop the line) */
            if (took > K.user_turn_ms) {
                K.user_turn_ms = (uint32_t)took;
                K.user_turn_what = slow_what;
                K.user_turn_owner = slow_owner;
                K.user_turn_id = slow_id;
            }
            if (took >= 100) {
                K.turns_100++;
                if (K.tl_n < 12) {                  /* 8c+: which film, which piece (the self test's end lists them) */
                    unsigned j_ = K.tl_n++;
                    K.tl_ms[j_] = took > 65535 ? 65535 : (uint16_t)took;
                    K.tl_film[j_] = sbos_film_now;
                    {
                        uint64_t at_ = sbos_film_now && turn0 > sbos_film_t0 ? turn0 - sbos_film_t0 : 0;
                        K.tl_at[j_] = at_ > 65535 ? 65535 : (uint16_t)at_;
                    }
                    K.tl_what[j_] = slow_what;
                    K.tl_piece[j_] = k_turn_piece;
                    K.tl_piece_ms[j_] = k_turn_piece_ms > 65535 ? 65535 : (uint16_t)k_turn_piece_ms;
                    K.tl_owner[j_] = (uint8_t)slow_owner;
                    K.tl_id[j_] = (uint8_t)slow_id;
                }
                unsigned b_ = slow_what[0] != 't' ? 17U : slow_owner < 16U ? slow_owner : 16U;
                if (K.turns_100_by[b_] < 65535U)
                    K.turns_100_by[b_]++;
            }
        }
        if (took >= 200 && sb_ms() - slow_logged >= 1000) {
            slow_logged = sb_ms();
            struct ub u;
            ub_init(&u);
            ub_s(&u, "sbos: a loop turn took ");
            ub_dec(&u, took);
            ub_s(&u, " ms (slowest: ");
            ub_s(&u, slow_what);
            if (slow_what[0] == 't') {          /* a timer: its owner (KT_*) and id */
                ub_s(&u, " ");
                ub_dec(&u, slow_owner);
                ub_s(&u, "/");
                ub_dec(&u, slow_id);
            }
            ub_s(&u, " ");
            ub_dec(&u, slow_ms);
            ub_s(&u, " ms)");
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
