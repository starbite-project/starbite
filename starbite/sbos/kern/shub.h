/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the sensor hub inside the BCM4775 GNSS chip (shub.c; docs/SENSOR-HUB.md). The accelerometer, the
 * magnetometer and the step counter hang off the hub; the hub's firmware is loaded into its RAM over SPI at every
 * start. The compass / level / step API for the rest of the kernel is in survival.h ("sensors"). */
#ifndef SBOS_SHUB_H
#define SBOS_SHUB_H
#include <stdint.h>

enum { SHUB_OFF = 0, SHUB_STARTING, SHUB_RUNNING, SHUB_FAILED };

/* Start the bring-up. probe_only: docs/SENSOR-HUB.md section 5 (look, bus up, wake, autobaud + sync, the ROM's
 * version), then everything back as found; else the full start (the patch download, SSP, the three sensors on).
 * Nothing happens here: shub_step does the work. */
void shub_begin(int probe_only);
/* drivers track: shub_begin with only the listed sensors started (shub_begin(p) = shub_begin_ex(p, SHUB_SENS_ALL)) */
#define SHUB_SENS_ACCEL 1U
#define SHUB_SENS_MAG 2U
#define SHUB_SENS_STEPS 4U
#define SHUB_SENS_ALL 7U
void shub_begin_ex(int probe_only, unsigned mask);
/* One slice of the bring-up (at most about 100 ms). 0: call again after shub_wait_ms(); 1: finished (the probe:
 * done and left; the full start: running or failed: see shub_state) */
int shub_step(void);
unsigned shub_wait_ms(void);
int shub_state(void);
const char *shub_why(void);          /* why it is not running, plain words */
/* Running: fetch what the hub has (every 20..50 ms; at most a few ms) */
void shub_poll(void);
/* The chip into standby and every register this driver changed back as it was found. Safe at any point. */
void shub_leave(void);

/* drivers track (shub_more.c, docs/drivers/SENSORS.md): more of the hub's sensors on a running hub.
 * shub_enable_type: one SSP request for Samsung's sensor number type (sensor_list.h): on, "add sensor" 0xA1
 * [type][s32 period_ms][s32 0][0]; off, "remove sensor" 0xA2 [type][s32 period_ms]. 0 sent; -1 not now (the hub is
 * not running, or the previous request still waits for its echo: try again after the next shub_poll). */
int shub_enable_type(unsigned type, unsigned period_ms, int on);
/* sample: every sample of a sensor other than the accelerometer, magnetometer and step counter, as the hub sent it
 * (d: the sample bytes, n their count, ts the hub's nanoseconds); poll: called at the end of every shub_poll. NULL:
 * none. */
typedef void (*shub_sample_fn)(unsigned type, const uint8_t *d, unsigned n, uint64_t ts);
void shub_set_hooks(shub_sample_fn sample, void (*poll)(void));
unsigned shub_starts(void);          /* how many times the hub reached "running" (a new start = sensors off again) */

/* uitest.c's variants: one step a call, 1 when done (each restores every register before it ends) */
int shub_probe_selftest_step(void);  /* starbite-os-shubprobe.img */
int shub_selftest_step(void);        /* starbite-os-shub.img */
#endif
