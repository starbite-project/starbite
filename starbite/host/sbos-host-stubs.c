/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS host preview (starbite/host/sbos-preview.sh): what host.c's Lab tours reference (never run here),
 * the card stubs, the kernel's hardware layer on host.c's fakes, and the table of the apps linked in.
 * The fake clock advances in sb_udelay, so sleeping is a delay; the heater's workers are host.c's threads
 * (the A55s; the preview has no M3 cores); ACPM DVFS writes the fake A55 PLL so the readback check sees it. */
#include <stdio.h>
#include <string.h>
#include "sd.h"
#include "sb.h"
#include "kern.h"
#include "kapp.h"


void sd_allow(void) {}
int sd_allowed(void) { return 0; }
int sd_up(void) { return 0; }
int sd_read(uint64_t s, uint32_t c, uintptr_t d) { (void)s; (void)c; (void)d; return -1; }
int sd_store_area(uint64_t *f, uint64_t *b) { (void)f; (void)b; return 0; }
int sd_read4k(uint64_t l, uint64_t c, uintptr_t d) { (void)l; (void)c; (void)d; return -1; }
int sd_write(uint64_t s, uint32_t c, uintptr_t d) { (void)s; (void)c; (void)d; return -9; }
int sd_write4k(uint64_t l, uint64_t c, uintptr_t d) { (void)l; (void)c; (void)d; return -9; }
uint64_t sd_sectors_hint(void) { return 0; }

/* ---- libsb additions the preview's host.c does not have ---- */
int sb_mmu_el0(uint64_t pa, int on) { (void)pa; (void)on; return 0; }
int sb_m3_pmu_defaults(void) { return 1; }
int64_t sb_smp_affinity(unsigned w) { (void)w; return -1; }
int sb_acpm_dvfs_listed(uint32_t domain, uint32_t khz)
{
    if (!sb_dvfs_level_listed(domain, khz))
        return 0;
    printf("host: acpm dvfs domain %u to %u kHz (listed)\n", domain, khz);
    if (domain == 2)                                     /* the fake A55 PLL: 26 MHz * M / 2 */
        *(volatile uint32_t *)0x1d000140UL = 0xa0000010U | (khz / 13000U) << 16 | 2U << 8;
    return 1;
}

/* ---- the kernel's hardware layer ---- */
int hw_irq_init(void) { K.irq_mode = 1; return 1; }
void hw_irq_poll(void) {}
void hw_sleep_until(uint64_t at_ms)
{
    uint64_t now = sb_ms();
    if (at_ms > now)
        sb_udelay((at_ms - now > 16 ? 16 : at_ms - now) * 1000U);
    else
        sb_udelay(200);
}
void hw_arm_until(uint64_t at_ms) { (void)at_ms; }
void hw_wdt_feed(void) {}
int hw_dvfs(unsigned domain, uint32_t khz) { return sb_acpm_dvfs_listed(domain, khz); }
int hw_m3_prep(void) { return sb_m3_pmu_defaults(); }
uint32_t hw_a55_khz(void) { return (uint32_t)(sb_a55_hz() / 1000U); }
uint32_t hw_m3_khz(void) { int off; uint64_t h = sb_m3_hz(&off); return off ? 0 : (uint32_t)(h / 1000U); }
int hw_m3_on(void) { return sb_m3_on(); }
void hw_screen(int on) { (void)on; }
void hw_app_map(uintptr_t slot, int on) { (void)slot; (void)on; }
void hw_tls_clear(void) {}
void hw_code_sync(uintptr_t a, uint64_t n) { (void)a; (void)n; }
int hw_heat_start(unsigned w, void *cfg) { return (int)sb_smp_start(w, heatk_worker, cfg); }
void hw_heat_stop_all(void) { (void)sb_smp_stop(500); }
unsigned hw_heat_running(void) { return sb_smp_running(); }
int hw_core_off(unsigned w)
{
    const struct sb_wslot *s = sb_smp_slot(w);
    return !s || s->state != SB_WK_RUNNING;
}

/* ---- the apps linked in (sbos-preview.sh renames each app's symbols) ---- */
#define APP(n)                                                                                                 \
    extern const struct sbapp_header n##_header;                                                               \
    void n##_app_event(const struct sbos_event *ev);
APP(light)
APP(heat)
APP(sos)
APP(clock)
APP(battery)
APP(settings)
struct host_app {
    const struct sbapp_header *hdr;
    void (*entry)(const struct sbos_event *);
};
const struct host_app sbos_host_apps[] = {
    { &light_header, light_app_event }, { &heat_header, heat_app_event }, { &sos_header, sos_app_event },
    { &clock_header, clock_app_event }, { &battery_header, battery_app_event }, { &settings_header, settings_app_event },
};
const int sbos_host_napps = 6;
