/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS survival tools (1.2-alpha27, survival.c): the kernel side of Emergency mode, for both UIs (the classic
 * Emergency app through SET_SURV_* settings, Starbite OS Next's Emergency mode by calling these directly).
 *
 *   USB power out  the MAX77705's own USB OTG boost (5 V on the USB-C port) at the chip's top rated current limit
 *                  (CHG_CNFG_02 OTG_ILIM 1.5 A), in bursts of at most 10 s after a hold-to-arm, then a cooldown.
 *                  Every protection stays as the chip has it (its current limit, short-circuit, thermal shutdown;
 *                  B2SOVRC, CHGPROT and the battery pack are never written). Off at once on: a fault the chip
 *                  reports, the output collapsed for 3 s (a short), the battery at the Survival limit, below 3.3 V or
 *                  3 %, a charger plugged in, the screen's Stop, a restart or power-off.
 *   SOS            Morse S-O-S (200 ms unit) on the torch (its normal current) and on the vibration motor, with a
 *                  full-screen beacon (white / red alternating, white, red, or lit with the Morse).
 *   Sun            sunrise and sunset for a location typed in (no GPS yet), from the phone's clock.
 *   Card           the emergency card (the Info app's: name, blood type, allergies, medicines, conditions, three
 *                  people to call, notes), readable and writable from the kernel for the lock screen.
 *   Last 2 %       everything off but the clock, the torch and SOS; the A55 at 455 MHz; the screen dim; the phone
 *                  runs to 1 % or 3.4 V instead of switching off at 3 %.
 *   Sensors        compass and level need the sensor hub inside the BCM4775 GNSS chip (firmware over SPI): not yet. */
#ifndef SBOS_SURVIVAL_H
#define SBOS_SURVIVAL_H
#include <stdint.h>

/* ---- USB power out ---- */
enum { SURV_USB_OFF = 0, SURV_USB_ON = 1, SURV_USB_COOL = 2, SURV_USB_REFUSED = -1 };
#define SURV_USB_BURST_MS 10000U
#define SURV_USB_COOL_MS 30000U
#define SURV_USB_COOL_FAULT_MS 60000U
struct surv_usb {
    int32_t state;              /* SURV_USB_* */
    int32_t ms_left;            /* ON: the burst's time left; COOL: the cooldown's */
    int32_t est_ma;             /* the output current, estimated from the battery's extra draw (-1 unknown) */
    int32_t bat_ma, bat_mv;     /* the battery now (mA: - discharging) */
    int32_t vbus_lo_mv, vbus_hi_mv; /* the USB-C controller's VBUS range (VBADC); -1 unknown */
    int32_t bat_c10, limit_c10; /* the battery's temperature and the Survival battery limit, 0.1 C */
    int32_t ilim_ma;            /* the chip's current limit as read back */
    uint32_t bursts;            /* since the start */
    int32_t refused;            /* 1: the last start was refused (why) */
    uint32_t events;            /* bumped at every refusal and stop: a new `why` (why is always a string literal) */
    int32_t fire;               /* 1.2-alpha29: this session is Extreme fire mode */
    uint32_t trips, resets;     /* the chip's current-limit events; the times it switched OTG off by itself */
    int32_t paused;             /* fire mode resting (the duty cap, or after the chip switched off) */
    int32_t usb_c10, chg_c10;   /* the USB connector's and the charger's thermistors, 0.1 C (-1000 unread) */
    int32_t fire_min_left;      /* fire mode: minutes of output left at the measured draw (-1 not yet) */
    uint8_t byp_dtls, chg_dtls; /* the chip's own status nibbles (logged) */
    const char *why;            /* refused / last stop, plain words */
    /* 1.2-alpha30: the fire starter (fire == 2) */
    int32_t fs_pulse_ms, fs_repeat, fs_n, fs_max;   /* the mode; pulses done in this hold, at most */
    int32_t fs_out;             /* 1 the output is on now (a pulse), 0 between pulses */
    int32_t fs_avg_ma, fs_peak_ma, fs_w10;          /* the last pulse: mean / peak mA out of the port, 0.1 W */
    int32_t fs_vmin;            /* the last pulse: the lowest VBUS code seen (0 below 3.5 V, 2 ~5 V; -1 none) */
    int32_t fs_sag_mv;          /* the last pulse: the battery's lowest voltage */
    int32_t fs_why;             /* SURV_FS_WHY_*: why the last hold ended (or was refused) */
    int32_t fs_vbyp_mv, fs_cellw10;
    int32_t fs_load_w10, fs_chip_w10;
    int32_t fs_rearms, fs_vb_code, fs_elapsed_ms, fs_open;
    int32_t bank_floor, bank_mah;   /* Track D 11-16, the power bank (fire == 3): its battery floor %; mAh given */
    int32_t fs_ohm10;           /* the last window's load in 0.1 ohm (VBYP / I, no limit event); -1 ?, -2 at the limit */   /* MAX: re-arms in this hold, the boost's code, the time on */ /* the last pulse: 0.1 W into the load (mA x the port band's middle), in the chip */ /* the last pulse: the boost's mean output (the gauge's VBYP); 0.1 W from the cell */
};
/* after the UI's hold-to-arm: start one burst. 0 started, < 0 refused (why in the status) */
int surv_usb_start(void);
void surv_usb_stop(const char *why);
/* 1.2-alpha29, Extreme fire mode: continuous output while held, after the UI's stronger confirmation. The chip's own
 * 1.5 A limit and protections stay; it runs while surv_usb_hold() comes at least every 1.5 s (the finger), at most
 * 2 min (then 15 s to cool); everything else off and the screen dim; floors at 1 % and a 3.2 V sag; the 65 C battery,
 * 70 C USB connector and 75 C charger stops; 2 s on / 2 s off within 5 C of a stop; if the chip switches the output
 * off by itself it rests 2 s and tries again, 3 times at most. Never during a self test. */
int surv_usb_fire_start(void);
void surv_usb_hold(void);
/* Track D 11-16, the power bank (fire == 3): steady 5 V at the chip's own 1.5 A limit to charge another device, with
 * the screen on or off, until it is stopped, the battery reaches floor_pct (5..80 %), `minutes` pass (0: no limit),
 * or nothing draws current for 30 s while the screen is on (LOAD OPEN, after 15 s for the device to negotiate). The
 * chip switching its output off (a device asking above 1.5 A, a short) stops it, as do the normal mode's
 * temperature stops, a short (under 3.5 V for 3 s) and 3.4 V. Not in Extreme power saving or a self test. */
int surv_usb_bank_start(int floor_pct, int minutes);
int surv_usb_active(void);                         /* 1 while the output is on (or being put back) */
/* 1.2-alpha30, the fire starter (docs/USB-FIRE-STARTER.md): pulses of 0.5 / 1 / 2 s at the chip's 0.9 / 1.2 / 1.5 A
 * step, once or repeated (at most SURV_FS_MAX_PULSES, a rest of max(1 s, the pulse) between), ONLY while a finger
 * stays on the screen (kui_finger_down(); the UI calls surv_fs_release at the lift). Stricter than the other modes:
 * off at once at the battery's 45 C, the port collapsing below 3.5 V, any flag the chip raises (its current limit,
 * the boost's limit, its thermal shutdown, battery overcurrent, OTG dropped by itself), a 3.3 V battery, 60 C on the
 * USB connector, 65 C on the charger. Then a cooldown of 5 s + 3x the time on (30 s after a fault). The chip's own
 * protections are never changed. pulse_ms 500 / 1000 / 2000, ilim_ma 900 / 1200 / 1500. 0 started, < 0 refused */
#define SURV_FS_MAX_PULSES 6
#define SURV_FS_BAT_STOP_C10 450
#define SURV_FS_BAT_START_C10 430
enum { SURV_FS_WHY_NONE, SURV_FS_WHY_DONE, SURV_FS_WHY_RELEASED, SURV_FS_WHY_ILIM, SURV_FS_WHY_VBUS,
       SURV_FS_WHY_FAULT, SURV_FS_WHY_HOT, SURV_FS_WHY_LOWBAT, SURV_FS_WHY_STOP, SURV_FS_WHY_REFUSED,
       SURV_FS_WHY_COOL, SURV_FS_WHY_FINGER };
int surv_fs_start(uint32_t pulse_ms, int repeat, int ilim_ma);
/* S9 #2's override (attended, the user's test phone; docs/USB-FIRE-STARTER.md 5): ov 1 fast re-arm after the chip's
 * limit or switch-off (instead of its own ~300 ms rest), 2 the same plus VBYPSET = vbyp (the boost's target; 0 is
 * 5.0 V). Pulses at most 1 s; a port collapse is logged, not a stop; at most 50 re-arms a pulse; the battery's 45 C,
 * 3.3 V, the chip's thermal shutdown and battery-overcurrent flags still stop it. Everything back after every hold,
 * before a restart, and at the next start after a crash. */
#define SURV_FS_OV_MAX_MS 1000U
#define SURV_FS_OV_MAX_REARMS 50U
int surv_fs_start_ov(uint32_t pulse_ms, int repeat, int ilim_ma, int ov, int vbyp);
/* tol 1, the steel-wool preset (no override): the chip's own limit-and-retry (~30 ms on, ~300 ms off) and the port's
 * dips are let run as the chip designs them; its switch-off, thermal shutdown, battery overcurrent, 45 C, 3.3 V stop */
int surv_fs_start_x(uint32_t pulse_ms, int repeat, int ilim_ma, int ov, int vbyp, int tol);
void surv_fs_release(void);                        /* the finger lifted (ui.c): the output off now */
/* Starbite OS Next's shell: its fire control's finger (1 down, 0 lifted: the output off at once). Classic uses
 * kern/ui.c's kui_finger_down (touch) and input.c's fire key mode (Power) instead */
void surv_fs_finger(int down);
int surv_user_finger(void);     /* Track D 11-16: 1 while a finger is on the screen (kui_finger_down or surv_fs_finger) */
/* 1 when the override modes may run on this phone (the card's /STARBITE/FIRE/OVERRIDE.TXT says "I accept the risk";
 * only the test phone S9 #2's card has it). surv_fs_start_* refuse ov > 0 otherwise. UIs only hide the choices */
int surv_fs_override_allowed(void);
void surv_usb_status(struct surv_usb *s);
extern const char *const surv_usb_guide[];   /* the on-screen guidance, NULL-terminated */

/* ---- SOS (services.c runs it; these are its options) ---- */
#define SURV_SOS_TORCH 1U       /* the torch in Morse (else the classic strobe) */
#define SURV_SOS_VIB 2U         /* the vibration motor in Morse */
#define SURV_BEACON_SHIFT 4     /* 0 white / red, 1 white, 2 red, 3 lit with the Morse */
extern uint32_t surv_sos_opts;   /* default SURV_SOS_TORCH | SURV_SOS_VIB, beacon 0 */
#define SURV_MORSE_UNIT_MS 200U
int surv_morse_at(uint32_t ms, uint32_t *next_ms);  /* SOS's pattern: lit at ms into the cycle; when it changes */
uint32_t surv_morse_cycle_ms(void);

/* ---- sun ---- */
#define SURV_NOLOC 0x7fffffff
extern int32_t surv_lat_x100, surv_lon_x100;      /* 0.01 degrees, N and E positive; SURV_NOLOC unset */
/* sunrise / sunset in local minutes after midnight for a date and a UTC offset: 0 both, 1 the sun stays up (polar
 * day), 2 it stays down (polar night), -1 bad input */
int surv_sun(int32_t lat_x100, int32_t lon_x100, int y, int mo, int d, int off_min, int *rise, int *set);
int surv_sun_today(int *rise, int *set);           /* today, here: as surv_sun; -2 no location, -3 no clock */

/* ---- the emergency card ---- */
#define SURV_CARD_NF 9
#define SURV_CARD_FL 64
struct surv_card { char f[SURV_CARD_NF][SURV_CARD_FL + 1]; };
extern const char *const surv_card_label[SURV_CARD_NF];
int surv_card_get(struct surv_card *c);            /* 1 a card, 0 none */
int surv_card_set(const struct surv_card *c);      /* 1 kept (saved with the settings) */

/* ---- extreme power saving (1.2-alpha29) ---- */
/* Everything off but the clock, the torch and SOS; the A55 at 455 MHz; the screen low and off after 15 s with the
 * panel off; fewer wake-ups. Last 2 % includes it. surv_saver(): 1 while it applies (by hand or by Last 2 %) */
int surv_saver_set(int on, const char *why);
int surv_saver(void);
int surv_saver_saved(uint32_t *screen_off_s, int *bright, int *panel_off);   /* settings.c: the user's own values */
uint32_t surv_input_poll_ms(void);                 /* services.c: the screen-off key poll */
uint32_t surv_battery_poll_ms(void);               /* services.c: the screen-off gauge read */
void surv_battery_sample(int avg_ma, int charger, int screen_on, uint64_t in_state_ms);

/* the battery estimate: minutes left to 1 % at each use; ma_saver is measured once learned_saver */
struct surv_est {
    int32_t pct, charging, usable_mah;
    int32_t ma_on, ma_off, ma_saver, ma_heat;
    int32_t min_on, min_off, min_saver, min_heat;
    int32_t learned_normal, learned_saver;
};
int surv_estimate(struct surv_est *e);             /* 1 filled, 0 the battery is not read */

/* ---- light and sound ---- */
void surv_light_boost(int on);                     /* the panel at its top level (the UI draws the white) */
extern int (*surv_tone_hook)(int hz, int on);      /* Track I's speaker: set it when the driver lands */
int surv_whistle(int on);                          /* -1: no speaker yet (surv_whistle_why) */
const char *surv_whistle_why(void);

/* ---- last 2 % ---- */
extern int surv_last2_auto;     /* enter it at 3 % instead of switching off (1, the default) */
int surv_last2(void);
int surv_last2_set(int on, const char *why);       /* 0 done (or already so), -1 refused: surv_last2_why() */
const char *surv_last2_why(void);
int surv_last2_battery(int pct, int avg_mv, int charger); /* services.c: 1 = switch off now */

/* ---- the offline survival guide (guide/sbguide.h; kern/guide.c) ---- */
struct sbg_chapter;
const struct sbg_chapter *surv_guide(int *n);      /* the chapters (struct in guide/sbguide.h) */

/* ---- sensors ---- */
/* The compass, the level and the step counter come from the sensor hub inside the BCM4775 (shub.c, shub.h): valid
 * only while the hub runs (shub_state() == SHUB_RUNNING and samples younger than 2 s). Each returns 1 when its
 * values are valid, 0 when not (the reason: surv_sensors_why(), and shub_why() for the hub's own state).
 * Axes as Android's: heading = where the top edge points, 0.1 degrees clockwise from magnetic north (0..3599),
 * tilt-compensated from the accelerometer and the magnetometer; accuracy = the hub's magnetometer calibration level
 * (0 unreliable .. 3 high). Pitch = the top edge above the horizon, roll = the right edge above the horizon, 0.1
 * degrees, -900..900 (flat, face up or down: 0, 0). Steps: counted since the hub was started. */
int surv_heading(int *deg_x10, int *accuracy);
int surv_tilt(int *pitch_x10, int *roll_x10);
int surv_steps(uint32_t *n);
const char *surv_sensors_why(void);
/* 1.2-alpha28: the hub on demand. surv_sensors_on(1) starts it (about 4 s: its firmware from the card's
 * /STARBITE/FW/BCM4775.BIN, then the download) or keeps it running; call it at least every 30 s while the values are
 * shown: it stops 60 s after the last call, at Last 2 % (refused: -1) and before a restart. 0 stops it now.
 * surv_sensors_state(): SHUB_OFF / _STARTING / _RUNNING / _FAILED (shub.h). Samples come at 10 Hz. */
int surv_sensors_on(int on);
int surv_sensors_on_mask(unsigned mask);           /* only the SHUB_SENS_* asked for (shub.h); else as surv_sensors_on */
int surv_sensors_state(void);

/* ---- the kernel ---- */
void surv_init(void);
void surv_timer(unsigned id);
void surv_pack(uint8_t out[16]);                   /* settings: the saved options */
void surv_unpack(const uint8_t in[16]);
int surv_selftest_step(void);                      /* uitest.c's survival variant: 1 done */
int surv_power_selftest_step(void);                /* uitest.c's power bench variant (attended: the cable out) */
#endif
