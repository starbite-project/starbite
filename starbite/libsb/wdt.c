/* SPDX-License-Identifier: GPL-2.0-only
 * The cluster 1 hardware watchdog as a dead-man for risky steps (Engineer OS Overclocking): the
 * loader's sequence (shim/starlte/wdt.c, docs/STARBITE-HEALTH.md section 5), plus a feed and a
 * disarm that puts back exactly what was there (S-Boot leaves the watchdog counting with its reset
 * masked in the PMU; an entry with wdt in the catalog has it armed by the loader: then it is only
 * fed, and left armed).
 * WTCON 0xFF39: prescaler 255, enable, divider 128, reset enable; 26 MHz OSCCLK, 16-bit count.
 */
#include "sb.h"

#define WDT1 0x10060000UL
#define PMU_WDT_DISABLE (SB_PMU + 0x408)
#define PMU_WDT_MASK (SB_PMU + 0x40c)
#define WDT1_BIT (1U << 23)
#define WTCON_ARM 0xff39U

static int wdt_mine, wdt_on;
static uint32_t old_con, old_dat, old_dis, old_mask;
static uint64_t wdt_next;

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

static int armed_by_loader(void)
{
    return (rd(WDT1) & 0x21U) == 0x21U && !(rd(PMU_WDT_DISABLE) & WDT1_BIT) && !(rd(PMU_WDT_MASK) & WDT1_BIT);
}

unsigned sb_wdt_arm(unsigned secs)
{
    if (secs < 10)
        secs = 10;
    if (secs > 82)
        secs = 82;
    uint32_t count = (uint32_t)((uint64_t)secs * 26000000U / (256U * 128U));
    if (wdt_on) {
        sb_wdt_feed(1);
        return secs;
    }
    wdt_on = 1;
    if (armed_by_loader()) {
        wdt_mine = 0;
        sb_wdt_feed(1);
        sb_log("watchdog: already armed (the loader's), fed");
        return secs;
    }
    wdt_mine = 1;
    old_con = rd(WDT1);
    old_dat = rd(WDT1 + 4);
    old_dis = rd(PMU_WDT_DISABLE);
    old_mask = rd(PMU_WDT_MASK);
    wr(WDT1 + 0x00, 0);
    wr(WDT1 + 0x04, count);
    wr(WDT1 + 0x08, count);
    wr(WDT1 + 0x0c, 0);
    wr(WDT1 + 0x00, WTCON_ARM);
    wr(PMU_WDT_DISABLE, old_dis & ~WDT1_BIT);
    wr(PMU_WDT_MASK, old_mask & ~WDT1_BIT);
    wdt_next = sb_ms() + 1000;
    sb_logx("watchdog: cluster 1 armed, seconds ", secs);
    return secs;
}

void sb_wdt_feed(int now)
{
    if (!wdt_on)
        return;
    uint64_t t = sb_ms();
    if (!now && t < wdt_next)
        return;
    wdt_next = t + 1000;
    if (rd(WDT1) & (1U << 5))
        wr(WDT1 + 0x08, rd(WDT1 + 0x04));
}

void sb_wdt_disarm(void)
{
    if (!wdt_on)
        return;
    wdt_on = 0;
    if (!wdt_mine)
        return;
    /* the reset masked again first, then the counter as S-Boot left it */
    wr(PMU_WDT_MASK, rd(PMU_WDT_MASK) | (old_mask & WDT1_BIT));
    wr(PMU_WDT_DISABLE, rd(PMU_WDT_DISABLE) | (old_dis & WDT1_BIT));
    wr(WDT1 + 0x00, 0);
    wr(WDT1 + 0x04, old_dat);
    wr(WDT1 + 0x08, old_dat);
    wr(WDT1 + 0x0c, 0);
    wr(WDT1 + 0x00, old_con);
    sb_log("watchdog: disarmed (put back as S-Boot left it)");
}

int sb_wdt_armed(void)
{
    return wdt_on;
}
