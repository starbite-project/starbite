/* SPDX-License-Identifier: GPL-2.0-only
 * The Exynos 9810 hardware watchdog, armed by the loader right before an OS starts (wdt.c,
 * docs/STARBITE-HEALTH.md section 5).
 *
 * Facts (sources in the doc): two S3C2410-type watchdogs in PERIS, counting OSCCLK (26 MHz):
 *   cluster 0 (little, A55)  0x10050000  samsung,exynos8-wdt  RST_STAT bit 24, PMU mask bit 24
 *   cluster 1 (big, M3)      0x10060000  samsung,exynos7-wdt  RST_STAT bit 23, PMU mask bit 23
 * Registers: WTCON +0x00 (prescaler [15:8], ENABLE bit 5, DIV [4:3], INTEN bit 2, RSTEN bit 0),
 * WTDAT +0x04 (reload), WTCNT +0x08 (16-bit down counter), WTCLRINT +0x0C.
 * PMU 0x14060000: AUTOMATIC_WDT_RESET_DISABLE +0x408 and MASK_WDT_RESET_REQUEST +0x40C, the
 * watchdog's bit set = its reset is blocked (S-Boot leaves both watchdogs counting with their
 * resets masked); RST_STAT +0x404 says which one reset the SoC.
 * The loader uses cluster 1's, the one mainline Linux's s3c2410_wdt node owns (exynos7 variant:
 * mask bit 23), so Linux takes it over in place.
 */
#pragma once
#include <stdint.h>

#define WDT0_BASE 0x10050000UL
#define WDT1_BASE 0x10060000UL
#define PMU_WDT_DISABLE 0x14060408UL
#define PMU_WDT_MASK    0x1406040CUL
#define PMU_RST_STAT    0x14060404UL
#define WDT1_PMU_BIT    23U

/* Arm cluster 1's watchdog for secs (clamped 10..82): stop, load WTDAT/WTCNT, clear the interrupt,
 * start with reset, then unmask its reset in the PMU. The last thing before an OS starts. Returns
 * the timeout in ms actually set (0 for secs 0: nothing touched). */
unsigned wdt_arm(unsigned secs);
/* Registers for a report: WTCON, WTDAT, WTCNT of both, the PMU disable and mask words. */
void wdt_regs(uint32_t w1[3], uint32_t w0[3], uint32_t *dis, uint32_t *mask);
