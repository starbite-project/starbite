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
    if (domain == 3)                                     /* 1.1-beta6: the fake M3 PLL, as the A55's */
        *(volatile uint32_t *)0x1d100120UL = 0xa0000010U | (khz / 13000U) << 16 | 2U << 8;
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
void hw_m3_core_on(unsigned w, int first) { printf("host: flexpmu core %u on%s\n", w, first ? " (first: cluster on)" : ""); }
void hw_m3_core_off(unsigned w, int last) { (void)last; printf("host: flexpmu core %u off (C2)\n", w); }
int hw_m3_fp_reset(void) { printf("host: flexpmu reset to the boot state\n"); return 0; }
void hw_m3_fp_log(const char *what) { sb_logf(what, ": (host)"); }
/* 1.1: the short watchdog (Survival, trip off): SBOS_HOST_NOWDT=1 = the loader armed none */
static int host_wdt_short;
static uint64_t host_wdt_fed;
unsigned hw_wdt_short(unsigned secs)
{
    if (secs && getenv("SBOS_HOST_NOWDT"))
        return 0;
    host_wdt_short = secs != 0;
    host_wdt_fed = sb_ms();
    printf(secs ? "host: watchdog shortened to %u s (fed by the heat loop only)\n" : "host: watchdog back to the loader's%.0u\n", secs);
    return secs ? secs * 1000U : 1U;
}
/* 1.2-alpha1: the Linux self-test image's own watchdog */
unsigned hw_wdt_selftest(unsigned secs)
{
    printf("host: watchdog %u s for the self test (fed by the loop)\n", secs);
    return secs;
}
void hw_wdt_feed_heat(void)
{
    uint64_t now = sb_ms();
    if (host_wdt_short && now - host_wdt_fed > 4000U)
        printf("host: WATCHDOG WOULD HAVE FIRED: %llu ms without a heat-loop feed\n", (unsigned long long)(now - host_wdt_fed));
    host_wdt_fed = now;
}
int hw_dvfs(unsigned domain, uint32_t khz) { return sb_acpm_dvfs_listed(domain, khz); }
/* 1.1-beta7: libsb ocp.c's PMIC read and IRP formula (host.c has its own sb_ocp_worker fake for Engineer OS) */
int sb_ocp_pmic(uint8_t *warn1, uint8_t *warn2) { *warn1 = 0x80; *warn2 = 0xc0; return 1; }
unsigned sb_ocp_irp(uint32_t khz, uint32_t min_khz)
{
    if (!min_khz)
        min_khz = SB_OCP_FMIN_KHZ;
    uint64_t irp = (uint64_t)khz * 2U / min_khz;
    irp = irp >= 2 ? irp - 2 : 0;
    return irp > SB_OCP_IRP_MASK ? SB_OCP_IRP_MASK : (unsigned)irp;
}
/* 1.1-beta7: the OCP probe on the host: fake register values (the real mrs would trap here);
 * SBOS_HOST_OCP=trap: the worker faults as an undefined register would */
static void host_ocp_fake(unsigned w, void *arg)
{
    struct sb_wslot *s = sb_smp_own_slot(w);
    (void)arg;
    if (!s)
        return;
    if (getenv("SBOS_HOST_OCP") && !strcmp(getenv("SBOS_HOST_OCP"), "trap")) {
        s->v[0] = 0x531f0020U;
        s->esr = 0x02000000U;
        s->state = SB_WK_FAULT;             /* held until the stop (the host's wrapper marks DONE when we return) */
        while (!sb_worker_stop())
            ;
        return;
    }
    s->v[0] = 0x531f0020U, s->v[1] = 0x0000007cU, s->v[2] = 0x1, s->v[3] = 0x30U, s->v[4] = 0x01000000U, s->v[5] = 0x100U;
    s->v[7] = 1;
    sb_worker_publish(s);
    while (!sb_worker_stop())
        ;
}
#include <unistd.h>
int64_t hw_ocp_launch(unsigned w)
{
    int64_t r = sb_smp_launch(w, host_ocp_fake, 0);
    /* the host's sb_mdelay is the simulated clock: give the fake worker thread real time to report */
    for (unsigned i = 0; r == 0 && i < 200; ++i) {
        const struct sb_wslot *s = sb_smp_slot(w);
        if (s && (s->v[7] == 1 || s->state == SB_WK_FAULT))
            break;
        usleep(5000);
    }
    return r;
}
int hw_dvfs_m3_ocp(uint32_t khz, unsigned busy)
{
    if (!sb_m3_ocp_level_ok(khz, busy)) {
        printf("host: M3 OCP TABLE VIOLATION: %u kHz with %u busy cores\n", khz, busy);
        return 0;
    }
    printf("host: acpm dvfs domain 3 to %u kHz (m3 ocp table, %u busy)\n", khz, busy);
    *(volatile uint32_t *)0x1d100120UL = 0xa0000010U | (khz / 13000U) << 16 | 2U << 8;
    return 1;
}
int hw_m3_prep(void) { return sb_m3_pmu_defaults(); }
uint32_t hw_a55_khz(void) { return (uint32_t)(sb_a55_hz() / 1000U); }
uint32_t hw_m3_khz(void) { int off; uint64_t h = sb_m3_hz(&off); return off ? 0 : (uint32_t)(h / 1000U); }
int hw_m3_on(void) { return sb_m3_on(); }
void hw_screen(int on) { panel_screen(on); }
int hw_spi(unsigned intid, int on) { (void)intid; (void)on; return 0; }   /* the host polls */


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
