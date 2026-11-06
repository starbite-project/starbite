/* SPDX-License-Identifier: GPL-2.0-only
 * The hardware watchdog: see wdt.h. Only wdt_arm writes anything, and only right before an OS
 * starts (xnu_boot.c launch_go), never on the way to the Bixby check.
 */
#include "wdt.h"
#include "sbfmt.h"

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" : : : "memory");
}

unsigned wdt_arm(unsigned secs)
{
    unsigned ms;
    uint32_t count = sb_wdt_count(secs, &ms);
    if (!count)
        return 0;
    /* Stopped first: S-Boot left it counting (masked), and its reset must not fire mid-load. */
    wr(WDT1_BASE + 0x00, 0);
    wr(WDT1_BASE + 0x04, count);
    wr(WDT1_BASE + 0x08, count);
    wr(WDT1_BASE + 0x0c, 0);
    wr(WDT1_BASE + 0x00, WDT_WTCON);
    /* Its reset reaches the SoC only with both PMU bits clear (mainline s3c2410wdt_enable). */
    wr(PMU_WDT_DISABLE, rd(PMU_WDT_DISABLE) & ~(1U << WDT1_PMU_BIT));
    wr(PMU_WDT_MASK, rd(PMU_WDT_MASK) & ~(1U << WDT1_PMU_BIT));
    return ms;
}

void wdt_regs(uint32_t w1[3], uint32_t w0[3], uint32_t *dis, uint32_t *mask)
{
    for (unsigned i = 0; i < 3; ++i) {
        w1[i] = rd(WDT1_BASE + 4U * i);
        w0[i] = rd(WDT0_BASE + 4U * i);
    }
    *dis = rd(PMU_WDT_DISABLE);
    *mask = rd(PMU_WDT_MASK);
}
