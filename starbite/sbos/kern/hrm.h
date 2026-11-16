/* SPDX-License-Identifier: MIT
 * The S9's heart-rate sensor, MAX86915 (Track D, 2026-11-16; kern/hrm.c): a pulse check with a fingertip on the
 * sensor next to the rear camera. Not a medical device.
 *
 * Wiring (Samsung's starlte DT and GPL drivers/optics/): hrmsensor @ 0x57 on HSI2C 0x108E0000 (USI08 of PERIC1, mode
 * register 0x10811034, pins gpp5-0/1 function 2); VDD 1.8 V = S2MPB02 LDO8 (L8CTRL 0x25 via fled.c, on 0xB0); the I2C
 * pull-ups = S2MPS18 LDO38 (L38CTRL 0x6F, 700 mV + 25 mV steps, on at 1.8 V = 0xEC; shared with the NFC chip, which
 * Starbite OS does not drive); the LED boost enable gpg1-6 (output high while on). Neither rail is on at boot.
 *
 * Everything is put back as found at the stop: the chip shut down, the boost pin, both rails, the bus's pins, its USI
 * mode and its controller. It runs only while asked (the Survival kit's pulse page, a keep-alive each second) and at
 * most 2 minutes at a time. */
#ifndef SBOS_HRM_H
#define SBOS_HRM_H
#include <stdint.h>

/* ---- the pulse from the IR samples (pure; host-tested in kern/host/survkit-test.c) ---- */
#define HRM_IV 8
#define HRM_LP 8                            /* the smoothing: 80 ms at 100 samples a second */
#define HRM_DS 4                            /* the slope's span: 40 ms */
#define HRM_FLOOR 40                        /* the smallest upstroke slope taken (counts in 40 ms; a fingertip's pulse
                                             * of ~1000-5000 counts rises ~300-1500 in 40 ms) */
struct hrm_algo {
    int64_t dc16;                           /* the first sample: the reference */
    int32_t lp[HRM_LP], sh[HRM_DS];
    unsigned lpi, shi;
    int armed;
    int32_t f1, f2;                         /* the filtered signal one and two samples back */
    int32_t env;                            /* its decaying peak level */
    uint32_t t, last_peak, rate_hz;
    uint16_t iv[HRM_IV];                    /* beat intervals in ms, newest at ivh - 1 */
    unsigned ivh, n_iv, beats;
    int bpm, quality;                       /* quality 0 none yet, 1 settling, 2 steady */
};
void hrm_algo_reset(struct hrm_algo *a, unsigned rate_hz);
/* one IR sample (the chip's 19-bit count; more light back = higher); 1 when it completed a beat */
int hrm_algo_push(struct hrm_algo *a, int32_t ir);

/* ---- the driver ---- */
enum { HRM_OFF, HRM_RUN, HRM_FAILED };
struct hrm_status {
    int state;                              /* HRM_* */
    int finger;                             /* 1 a fingertip on the sensor (enough light back, not saturated) */
    int bpm, quality;                       /* as hrm_algo */
    int seconds;                            /* since the finger was found (0 none) */
    int32_t dc;                             /* the IR level (19-bit counts) */
    int led_ma10;                           /* the IR LED's current, 0.1 mA */
    unsigned rate_hz;                       /* samples a second as counted */
    unsigned samples;                       /* read since the start */
};
int hrm_want(void);                         /* a keep-alive: starts it (0) or -1 (why: hrm_why()) */
void hrm_stop(const char *why);
void hrm_status(struct hrm_status *s);
const char *hrm_why(void);
void hrm_tick(void);                        /* survkit_timer's KT_SURV_KIT_HRM: every 100 ms while it runs */
#endif
