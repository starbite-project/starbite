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

/* uitest.c's variants: one step a call, 1 when done (each restores every register before it ends) */
int shub_probe_selftest_step(void);  /* starbite-os-shubprobe.img */
int shub_selftest_step(void);        /* starbite-os-shub.img */
#endif
