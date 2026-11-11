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
    uint8_t byp_dtls, chg_dtls; /* the chip's own status nibbles (logged) */
    const char *why;            /* refused / last stop, plain words */
};
/* after the UI's hold-to-arm: start one burst. 0 started, < 0 refused (why in the status) */
int surv_usb_start(void);
void surv_usb_stop(const char *why);
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
int surv_sensors_state(void);

/* ---- the kernel ---- */
void surv_init(void);
void surv_timer(unsigned id);
void surv_pack(uint8_t out[16]);                   /* settings: the saved options */
void surv_unpack(const uint8_t in[16]);
int surv_selftest_step(void);                      /* uitest.c's survival variant: 1 done */
#endif
