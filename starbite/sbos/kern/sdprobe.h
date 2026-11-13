/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: the microSD card's fast-read probe (sdprobe.c, the drvprobe.h contract). Read only. */
#ifndef SBOS_SDPROBE_H
#define SBOS_SDPROBE_H

#define DRV_SD_BUDGET_MS 15000U

int drv_sd_probe(int level, unsigned *wait_ms);
void drv_sd_abort(void);

#endif
