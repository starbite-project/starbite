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
    if (domain == 4)                                     /* the fake G3D PLL: 26 MHz * M / (4 << 2) */
        *(volatile uint32_t *)0x17400140UL = 0xa0000010U | (khz * 16U / 26000U) << 16 | 4U << 8 | 2U;
    return 1;
}

/* the card's files: SBOS_HOST_FILES=dir maps "/starbite/emergency.txt" to dir/starbite/emergency.txt */
#include <stdlib.h>
int kfile_read(const char *path, void *dst, uint32_t cap)
{
    const char *dir = getenv("SBOS_HOST_FILES");
    if (!dir)
        return SBOS_EINVAL;
    char p[512];
    snprintf(p, sizeof(p), "%s%s", dir, path);
    FILE *f = fopen(p, "rb");
    if (!f)
        return SBOS_EINVAL;
    size_t n = fread(dst, 1, cap, f);
    fclose(f);
    return (int)n;
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
void hw_screen(int on) { panel_screen(on); }


void hw_app_map(uintptr_t slot, int on) { (void)slot; (void)on; }
void hw_tls_clear(void) {}
void hw_code_sync(uintptr_t a, uint64_t n) { (void)a; (void)n; }
int64_t hw_heat_launch(unsigned w, void *cfg) { return sb_smp_launch(w, heatk_worker, cfg); }
void hw_heat_stop_all(void) { (void)sb_smp_stop(0); }
unsigned hw_heat_running(void) { return sb_smp_running(); }
int64_t hw_core_psci(unsigned w) { (void)w; return 0xffffffffLL; }   /* as the phone: not supported, in w0 */

/* ---- the apps linked in (sbos-preview.sh renames each app's symbols) ---- */
#define APP(n)                                                                                                 \
    extern const struct sbapp_header n##_header;                                                               \
    void n##_app_event(const struct sbos_event *ev);
APP(emergency)
APP(wifi)
APP(light)
APP(heat)
APP(sos)
APP(info)
APP(clock)
APP(battery)
APP(settings)
struct host_app {
    const struct sbapp_header *hdr;
    void (*entry)(const struct sbos_event *);
};
const struct host_app sbos_host_apps[] = {
    { &emergency_header, emergency_app_event }, { &light_header, light_app_event }, { &heat_header, heat_app_event }, { &sos_header, sos_app_event }, { &info_header, info_app_event },
    { &clock_header, clock_app_event }, { &battery_header, battery_app_event }, { &settings_header, settings_app_event }, { &wifi_header, wifi_app_event },
};
const int sbos_host_napps = 9;
