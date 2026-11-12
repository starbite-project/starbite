/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: the rest of the S9's sensors (docs/drivers/SENSORS.md).
 *
 *   shub_more.c   behind the BCM4775 sensor hub (shub.c starts it; the accelerometer, magnetometer and step counter
 *                 are survival.h's): proximity + light (AMS TMD4906), pressure (ST LPS22H), gyroscope (ST LSM6DSL).
 *   senstat.c     small read-only status parts on AP buses / pins: the flip-cover hall sensors, the IDT P9320
 *                 wireless charger, the MAX86915 heart-rate sensor's part ID.
 *
 * Power: a hub sensor runs only while someone asks for it. Every getter below is also a keep-alive for its sensor;
 * a sensor nobody asked for in 10 s is switched off on the hub (the in-call proximity stays on until sens_incall(0)).
 * The hub itself is started and kept running through survival.c's surv_sensors_on (its firmware from the card). */
#ifndef SBOS_SHUB_MORE_H
#define SBOS_SHUB_MORE_H
#include <stdint.h>

/* the sensors, as a mask */
#define SENS_PROX 1U
#define SENS_LIGHT 2U
#define SENS_PRESSURE 4U
#define SENS_GYRO 8U
#define SENS_ALL 15U

/* Samsung's sensor numbers (sensor_list.h) and the sample lengths of starlte's firmware (ssp_data.c) */
#define SENS_T_GYRO 1U              /* 12 bytes: s32 x, y, z */
#define SENS_T_PRESSURE 5U          /* 6 bytes: s32 pressure, s16 temperature (0.01 degrees C) */
#define SENS_T_PROX 7U              /* 3 bytes: u8 1 = near, u16 adc */
#define SENS_T_LIGHT 9U             /* 18 bytes: u32 lux, s32 cct, u16 r g b w, u8 a_time, u8 a_gain */

/* ---- the hub's sensors (shub_more.c) ---- */
/* Ask for sensors (a keep-alive; starts the hub when it is off). 0 asked; -1 the hub cannot run now (sens_why). */
int sens_want(unsigned mask);
void sens_release(unsigned mask);           /* off now (the in-call proximity: sens_incall(0)) */
/* -1 unknown (not running yet, no sample yet) / 0 far / 1 near. Asks for the proximity sensor. */
int sens_prox_near(void);
/* 1 valid. Lux as the hub computes it (Samsung's LIGHT_REPORT_LUX). Asks for the light sensor. */
int sens_lux(uint32_t *lux);
/* 1 valid. hPa x 100 (1013.25 hPa = 101325) and the sensor's temperature in 0.01 degrees C. */
int sens_pressure_hpa(int32_t *hpa_x100, int32_t *temp_c_x100);
/* 1 valid. Rates in millidegrees per second around x, y, z (Android axes, as the accelerometer's). The scale is the
 * LSM6DSL's 2000 dps one (70 mdps a count, Samsung's DEF_GYRO_SENS): verify on the phone (docs/drivers/SENSORS.md). */
int sens_gyro_mdps(int32_t out[3]);
/* The in-call screen: sens_incall(1) when a call goes active (the proximity sensor only, every 200 ms, no 10 s
 * timeout), sens_incall(0) when it ends. The UI calls sens_incall_screen_off() each frame / timer tick: 1 = the
 * screen (and touch) should be off now (in a call and something near the earpiece); 0 otherwise, also when unknown. */
void sens_incall(int on);
int sens_incall_screen_off(void);
/* Auto-brightness helper: a backlight level 0..255 for the current light, -1 when unknown (log curve: 1 lux -> 10,
 * 10 -> 40, 100 -> 90, 1000 -> 170, 10000+ -> 255). Asks for the light sensor. */
int sens_auto_brightness(void);
const char *sens_why(void);                 /* plain words: why a value is not there */
/* Wired once at boot (main.c / the lead's probe image): the hooks into shub.c. Idempotent. */
void sens_init(void);

/* ---- status parts (senstat.c), all read only ---- */
/* Flip-cover hall sensor (gpa3-6) and the "certify" hall (gpa0-2): 1 a magnet (cover closed), 0 none, -1 unreadable.
 * The pin reads high with no magnet (Utility OS 2, 2026-11-07). */
int sens_hall_closed(void);
int sens_hall_certify(void);
/* The IDT P9320 wireless charger (idt,mfc-charger @ 0x3B on HSI2C 0x14370000). The chip is powered from the pad's
 * coil: off a pad it does not answer. sens_wpc_pad: the wpc_det pin, 1 a pad's field, 0 none, -1 unreadable. */
int sens_wpc_pad(void);
struct sens_wpc {
    int answered;                           /* the chip answered on I2C */
    uint16_t chip_id;                       /* 0x9320 expected */
    uint8_t rev;
    uint16_t status, vout_raw, vrect_raw, freq_khz;
    int vout_mv;                            /* raw x 12600 / 4095 [scale unverified] */
    int die_c;
};
/* A read-only look (brings the controller and its two pins up, reads, puts them back): 1 answered, 0 not (off a pad:
 * normal), -1 the bus could not be used (why: sens_why()). Takes a few ms. */
int sens_wpc_read(struct sens_wpc *w);
/* MAX86915 (hrmsensor @ 0x57 on HSI2C 0x108E0000): its rails' state. 1 both checkable rails on, 0 off, -1 unreadable.
 * The part ID is read only when they are on and its bus is up (no regulator write in this driver). */
int sens_hrm_rails(uint8_t *ldo38);
int sens_hrm_part_id(uint8_t *id);          /* 1 read (0x2B expected), 0 not readable now (why: sens_why()) */

/* ---- the on-phone probes (drvprobe.h) ---- */
int drv_sens_probe(int level, unsigned *wait_ms);
void drv_sens_abort(void);
#define DRV_SENS_BUDGET_L0 2000U
#define DRV_SENS_BUDGET_L1 25000U           /* hub start ~4 s (+3 s waiting for its first frame) + 6 s of samples */

/* the host test's window into the state (kern/host/shub-more-test.c) */
struct sens_stat {
    uint32_t n;
    int32_t mn, mx;
    int64_t sum;
};
enum { SST_PROX_ADC = 0, SST_PROX_NEAR, SST_LUX, SST_HPA, SST_TEMP, SST_GX, SST_GY, SST_GZ, SST_N };
const struct sens_stat *sens_stats(void);
unsigned sens_running_mask(void);           /* the sensors on at the hub now */
#endif
