/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB: the Exynos 9810 USB 2.0 PHY glue (USBCON) brought up for a high-speed device link, with the
 * SuperSpeed side held idle. Clock gates and PMU isolation only where the nub carries them (sbusb_io.h).
 */
#ifndef SBUSB_PHY_H
#define SBUSB_PHY_H
#include "sbusb_io.h"

struct sbusb_phy_report {
	uint32_t missing;       /* bit per SBUSB_BLK_* that was not mapped and therefore skipped */
	uint32_t utmi, hsp_test;        /* as read back at the end */
	unsigned line;          /* D+/D- as the PHY sees them: 0 SE0, 1 J, 2 K, 3 SE1 */
};

/* 0, -1 when USBCON did not keep the settings, -2 when the usb-phy block is not mapped at all. */
int sbusb_phy_start(const struct sbusb_io *io, struct sbusb_phy_report *rep);
unsigned sbusb_phy_line(const struct sbusb_io *io);
/* how many register steps the sequence has (the host test checks the mock saw each once) */
unsigned sbusb_phy_step_count(void);

#endif
