/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: the notification LED (the RGB LED block of the MAX77705), nled.c.
 *
 * Wiring: MAX77705 on HSI2C 0x14360000 (libsb max77705.c's bus), the chip's top / PMIC address 7-bit 0x66 (Samsung's
 * leds-max77705-rgb.c uses the parent's client, max77705->i2c = 0xCC >> 1; the "0x94" in max77705-private.h is a stale
 * comment: no client at 0x4A exists, see docs/drivers/NLED.md). Registers LEDEN 0x30, LED0BRT..LED3BRT 0x31..0x34,
 * LEDRMP 0x36, LEDBLNK 0x38. Channels (Samsung's enum max77705_led_color): LED0 white (not fitted on the S9 as far as
 * is known), LED1 red, LED2 green, LED3 blue.
 *
 * The kernel-facing calls (single-threaded, each bounded by a few I2C messages; 1 done, 0 refused / failed, with
 * nled_why() in plain words). The LED keeps blinking by itself (the chip's own blink timer): nothing runs on the AP. */
#ifndef SBOS_NLED_H
#define SBOS_NLED_H
#include <stdint.h>

/* The patterns the OS uses (Samsung's store_max77705_rgb_pattern, at the DT's own current). */
enum nled_pattern {
    NLED_OFF = 0,
    NLED_MISSED_CALL,       /* blue, slow blink (0.5 s on, 5 s off) */
    NLED_MESSAGE,           /* blue, slow blink (0.5 s on, 5 s off) */
    NLED_CHARGING,          /* red, steady */
    NLED_CHARGED,           /* green, steady */
    NLED_LOW_BATTERY,       /* red, slow blink (0.5 s on, 5 s off) */
    NLED_CHARGE_ERROR,      /* red, fast blink (0.5 s on, 0.5 s off) */
    NLED_NPATTERNS
};

int nled_init(void);                    /* read the block once and keep it "as found"; 1 the LED answers */
/* Steady colour: r, g, b 0..255 (the colour), pct 0..100 (the brightness). 255 at 100 % is the DT's own current for
 * that colour (red 7.0 mA, green 0.6 mA, blue 1.2 mA); nothing above it is ever written. All zero = off. */
int nled_color(uint8_t r, uint8_t g, uint8_t b, unsigned pct);
/* The same colour, blinking by the chip's timer: on_ms 100..3250, off_ms 500..12000 (rounded to the chip's steps). */
int nled_blink(uint8_t r, uint8_t g, uint8_t b, unsigned pct, unsigned on_ms, unsigned off_ms);
int nled_off(void);                     /* every channel off (LEDEN 0) */
int nled_pattern(enum nled_pattern p);
const char *nled_pattern_name(enum nled_pattern p);    /* "Missed call", "Charging", ... */
void nled_restore(void);                /* the block back as nled_init found it */
const char *nled_why(void);             /* the last refusal or failure, or the state, in plain words */

/* The probe (drvprobe.h): level 0 reads the block; level 1 PASS "skipped (attended)"; level 2 red, green, blue,
 * white (red + green + blue), LED0, 1 s each at a quarter of the DT's current, a blink for 3 s, then the block as
 * found. Budget NLED_PROBE_BUDGET_MS. */
#define NLED_PROBE_BUDGET_MS 12000U
int drv_nled_probe(int level, unsigned *wait_ms);
void drv_nled_abort(void);

/* For the host test: the block's address and registers, the per-register caps (LED0..LED3 BRT codes, 0.1 mA). */
#define NLED_ADDR 0x66U
#define NLED_LEDEN 0x30U
#define NLED_BRT0 0x31U
#define NLED_RMP 0x36U
#define NLED_BLNK 0x38U
extern const uint8_t nled_cap[4];
int nled_allowed(unsigned addr7, unsigned reg);        /* 1 only for the seven LED registers at NLED_ADDR */
uint8_t nled_blnk_code(unsigned on_ms, unsigned off_ms);

#endif
