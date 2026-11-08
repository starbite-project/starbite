/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS hardware layer on the phone (docs/STARBITE-OS.md 2.2, 2.3): the CPU's EL0 hardening, the GIC-400,
 * the EL1 virtual timer and its boot self test, sleeping, the watchdog, the app slots' mapping, the listed
 * DVFS path and the heater's workers. Everything else the kernel needs comes from libsb.
 *
 * The kernel runs with PSTATE.I = 1 at all times: wfi still wakes on a pending interrupt and the loop reads
 * GICC_IAR itself. Interrupts are taken as exceptions only from EL0 (vec.S).
 */
#include "sb.h"
#include "kern.h"
#include "kapp.h"

#define GICD 0x10101000UL
#define GICC 0x10102000UL
#define INTID_VTIMER 27U
#define WDT1 0x10060000UL

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

extern char sbos_vectors[];
void sbos_spsel1(void);

static uint64_t vct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static void vtimer_arm(uint64_t cval)
{
    __asm__ volatile("msr cntv_cval_el0, %0\n\tmov x9, #1\n\tmsr cntv_ctl_el0, x9\n\tisb" : : "r"(cval) : "x9", "memory");
}

static void vtimer_off(void)
{
    __asm__ volatile("msr cntv_ctl_el0, xzr\n\tisb" ::: "memory");
}

/* 0.5: one device interrupt besides the timer: the Wi-Fi chip's INTx, SPI 253 = INTID 285, level, cpu0 */
static unsigned spi_on;

int hw_spi(unsigned intid, int on)
{
    if (!K.irq_mode || intid < 32 || intid >= 1020)
        return 0;
    unsigned bit = 1U << (intid % 32);
    if (!on) {
        wr(GICD + 0x180 + 4 * (intid / 32), bit);         /* ICENABLER */
        wr(GICD + 0x280 + 4 * (intid / 32), bit);         /* ICPENDR */
        if (spi_on == intid)
            spi_on = 0;
        return 1;
    }
    uintptr_t cfg = GICD + 0xc00 + 4 * (intid / 16);
    wr(cfg, rd(cfg) & ~(2U << (2 * (intid % 16))));       /* ICFGR: level-sensitive */
    uintptr_t tg = GICD + 0x800 + (intid & ~3U);
    unsigned sh = 8 * (intid & 3U);
    uint32_t me = rd(GICD + 0x800) & 0xffU;                /* ITARGETSR0 byte 0: this CPU interface's own bit */
    if (!me)
        me = 1;
    wr(tg, (rd(tg) & ~(0xffU << sh)) | (me << sh));        /* ITARGETSR: this cpu */
    spi_on = intid;
    wr(GICD + 0x100 + 4 * (intid / 32), bit);             /* ISENABLER */
    if (!(rd(GICD + 0x100 + 4 * (intid / 32)) & bit)) {    /* not taken (a secure-group line?): polling only */
        spi_on = 0;
        return 0;
    }
    return 1;
}

/* every interrupt pending at the CPU interface acknowledged; the timer's disabled (the loop re-arms it) */
static unsigned irq_drain(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t iar = rd(GICC + 0x0c);
        uint32_t id = iar & 0x3ffU;
        if (id >= 1020)
            break;
        if (id == INTID_VTIMER) {
            vtimer_off();
            K.timer_irqs++;
        } else if (id == spi_on && id) {
            wifi_irq();                     /* the radio (SPI 253): the chip masked, the tick run at once */
        } else {
            /* nothing else is enabled: a stray from before the reset; disable it again */
            if (id >= 16)
                wr(GICD + 0x180 + 4 * (id / 32), 1U << (id % 32));
        }
        wr(GICC + 0x10, iar);
        K.irqs++;
        ++n;
    }
    return n;
}

static void gic_reset(void)
{
    unsigned lines = ((rd(GICD + 0x004) & 0x1fU) + 1U) * 32U;
    if (lines > 1020)
        lines = 1020;
    wr(GICD + 0x000, 0);                                /* distributor off while configuring */
    for (unsigned i = 32; i < lines; i += 32) {
        wr(GICD + 0x180 + i / 8, 0xffffffffU);          /* ICENABLER: every SPI off */
        wr(GICD + 0x280 + i / 8, 0xffffffffU);          /* ICPENDR: none pending */
    }
    for (unsigned i = 32; i < lines; i += 4) {
        wr(GICD + 0x800 + i, 0x01010101U);              /* ITARGETSR: cpu0 */
        wr(GICD + 0x400 + i, 0xa0a0a0a0U);              /* IPRIORITYR */
    }
    wr(GICD + 0x180, 0xffff0000U);                      /* PPIs off (S-Boot's timers among them) */
    wr(GICD + 0x280, 0xffffffffU);
    for (unsigned i = 0; i < 32; i += 4)
        wr(GICD + 0x400 + i, 0xa0a0a0a0U);
    wr(GICD + 0x000, 1);                                /* EnableGrp1 (the non-secure view) */
    wr(GICC + 0x04, 0xffU);                             /* PMR: everything */
    wr(GICC + 0x08, 0);                                 /* BPR */
    wr(GICC + 0x00, 1);                                 /* EnableGrp1, EOImodeNS = 0 */
    __asm__ volatile("dsb sy; isb" ::: "memory");
    irq_drain();
    wr(GICD + 0x100, 1U << INTID_VTIMER);               /* ISENABLER0: the virtual timer only */
}

/* EL0 can neither mask interrupts nor reach the counters, caches or the PMU (docs 2.3) */
static void el0_harden(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr &= ~((1ULL << 9) |    /* UMA */
               (1ULL << 14) |   /* DZE */
               (1ULL << 15) |   /* UCT */
               (1ULL << 16) |   /* nTWI: wfi at EL0 traps */
               (1ULL << 18) |   /* nTWE */
               (1ULL << 24) |   /* E0E */
               (1ULL << 26));   /* UCI */
    sctlr &= ~(1ULL << 19);     /* WXN: app code is writable and executable at EL0 */
    sctlr |= (1ULL << 4) |      /* SA0 */
             (1ULL << 23);      /* SPAN = 1: an exception does not set PAN (the kernel reads app memory) */
    __asm__ volatile("msr sctlr_el1, %0\n\tisb" : : "r"(sctlr) : "memory");
    __asm__ volatile("msr pan, #0\n\tisb" ::: "memory");      /* cpu0 is an A55 (ARMv8.2) */
    __asm__ volatile("msr cntkctl_el1, xzr\n\tisb" ::: "memory");
    uint64_t v;
    if (SB_SAFE_MRS(pmuserenr_el0, &v))
        __asm__ volatile("msr pmuserenr_el0, xzr\n\tisb" ::: "memory");
}

static uint64_t freq_hz;

int hw_irq_init(void)
{
    __asm__ volatile("msr daifset, #3" ::: "memory");   /* IRQ and FIQ masked: the kernel's rule */
    sbos_spsel1();
    el0_harden();
    __asm__ volatile("msr vbar_el1, %0\n\tisb" : : "r"((uint64_t)(uintptr_t)sbos_vectors) : "memory");
    freq_hz = sb_freq();
    vtimer_off();
    gic_reset();
    /* self test 1: exactly one timer interrupt within 50 ms, then 5 ms of silence */
    uint64_t t0 = vct(), per_ms = freq_hz / 1000U;
    unsigned got = 0, other = 0;
    vtimer_arm(t0 + per_ms);
    while (vct() - t0 < 50 * per_ms && !got) {
        uint32_t iar = rd(GICC + 0x0c), id = iar & 0x3ffU;
        if (id >= 1020)
            continue;
        if (id == INTID_VTIMER) {
            vtimer_off();
            ++got;
        } else {
            ++other;
        }
        wr(GICC + 0x10, iar);
    }
    uint64_t t1 = vct();
    while (vct() - t1 < 5 * per_ms) {
        uint32_t iar = rd(GICC + 0x0c), id = iar & 0x3ffU;
        if (id >= 1020)
            continue;
        ++other;
        wr(GICC + 0x10, iar);
    }
    if (got != 1 || other) {
        sb_logx("sbos: timer interrupt test failed, others ", other);
        vtimer_off();
        wr(GICC + 0x00, 0);                             /* polling mode: no interrupt reaches the CPU */
        K.irq_mode = 0;
        return 0;
    }
    /* self test 2: a 10 ms wfi sleeps */
    uint64_t t2 = vct();
    vtimer_arm(t2 + 10 * per_ms);
    __asm__ volatile("dsb sy; wfi" ::: "memory");
    uint64_t slept = vct() - t2;
    irq_drain();
    vtimer_off();
    if (slept < 8 * per_ms) {
        sb_logx("sbos: wfi returned early, ticks ", slept);
        wr(GICC + 0x00, 0);
        K.irq_mode = 0;
        return 0;
    }
    K.irq_mode = 1;
    sb_log("sbos: interrupts: GIC + virtual timer, wfi sleeps");
    return 1;
}

static uint64_t ms_to_vct(uint64_t at_ms)
{
    uint64_t now = sb_ms();
    uint64_t d = at_ms > now ? at_ms - now : 0;
    if (d > 3600000U)
        d = 3600000U;
    return vct() + d * (freq_hz / 1000U);
}

void hw_irq_poll(void)
{
    if (K.irq_mode)
        irq_drain();
}

void hw_sleep_until(uint64_t at_ms)
{
    uint64_t now = sb_ms();
    if (at_ms <= now)
        return;
    if (!K.irq_mode) {
        uint64_t d = at_ms - now;
        sb_udelay((d > 20 ? 20 : d) * 1000U);
        return;
    }
    vtimer_arm(ms_to_vct(at_ms));
    __asm__ volatile("dsb sy; wfi" ::: "memory");
    irq_drain();
}

/* the app budget: the timer at the earlier of the next deadline and the budget's end */
void hw_arm_until(uint64_t at_ms)
{
    if (K.irq_mode)
        vtimer_arm(ms_to_vct(at_ms));
}

static unsigned wdt_feeds;
static int wdt_heat_only;                               /* 1.1: Survival with the trip off: only the heat loop feeds */
static uint32_t wdt_saved;
void hw_wdt_feed(void)
{
    volatile uint32_t *w = (volatile uint32_t *)WDT1;
    if (wdt_heat_only)
        return;
    if (w[0] & (1U << 5)) {                             /* WTCON enable: WTCNT = WTDAT */
        w[2] = w[1];
        ++wdt_feeds;
    }
}

void hw_wdt_feed_heat(void)
{
    volatile uint32_t *w = (volatile uint32_t *)WDT1;
    if (w[0] & (1U << 5))
        w[2] = w[1];
}

/* 1.1: the loader's watchdog (cluster 1, 0x10060000) shortened to secs while the chip's heat trip is off, fed only by
 * the heat loop (hw_wdt_feed_heat); 0 puts the loader's reload back. Refuses (0) unless it counts with its reset on
 * and its reset reaches the SoC (PMU 0x14060408 / 0x1406040C bit 23 clear, as the loader leaves it). Returns the
 * timeout in ms. */
unsigned hw_wdt_short(unsigned secs)
{
    volatile uint32_t *w = (volatile uint32_t *)WDT1;
    if (!secs) {
        if (wdt_saved) {
            w[1] = wdt_saved;
            w[2] = wdt_saved;
            wdt_saved = 0;
        }
        wdt_heat_only = 0;
        return 1;
    }
    uint32_t con = w[0];
    uint32_t dis = *(volatile uint32_t *)0x14060408UL, mask = *(volatile uint32_t *)0x1406040CUL;
    if (!(con & (1U << 5)) || !(con & 1U) || ((dis | mask) & (1U << 23)))
        return 0;
    uint32_t pre = ((con >> 8) & 0xffU) + 1U, div = 16U << ((con >> 3) & 3U);
    uint64_t per_s = 26000000U / (pre * div);       /* OSCCLK 26 MHz */
    uint64_t c = per_s * secs;
    if (!c || c > 0xffffU)
        return 0;
    if (!wdt_saved)
        wdt_saved = w[1];
    w[1] = (uint32_t)c;
    w[2] = (uint32_t)c;
    wdt_heat_only = 1;
    return (unsigned)(c * 1000U / per_s);
}

int hw_dvfs(unsigned domain, uint32_t khz)
{
    return sb_acpm_dvfs_listed(domain, khz);
}

int hw_m3_prep(void)
{
    return sb_m3_pmu_defaults();
}

uint32_t hw_a55_khz(void)
{
    return (uint32_t)(sb_a55_hz() / 1000U);
}

uint32_t hw_m3_khz(void)
{
    int off = 0;
    uint64_t hz = sb_m3_hz(&off);
    return off ? 0 : (uint32_t)(hz / 1000U);
}

int hw_m3_on(void)
{
    return sb_m3_on();
}

void hw_screen(int on)
{
    panel_screen(on);                                   /* v0.2: the panel's own off / on (if turned on) */
}

void hw_tls_clear(void)
{
    __asm__ volatile("msr tpidr_el0, xzr\n\tmsr tpidrro_el0, xzr" ::: "memory");
}

void hw_app_map(uintptr_t slot, int on)
{
    sb_mmu_el0(slot, on);
    sb_mmu_el0(slot + (2U << 20), on);
}

void hw_code_sync(uintptr_t a, uint64_t n)
{
    uintptr_t p = a & ~(uintptr_t)63, end = a + n;
    for (; p < end; p += 64)
        __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" ::: "memory");
}

uint64_t sbos_app_resume(struct aframe *f);
uint64_t hw_app_resume(struct aframe *f)
{
    return sbos_app_resume(f);
}

/* ---- heater workers --------------------------------------------------------------------------- */
int64_t hw_heat_launch(unsigned w, void *cfg)
{
    return sb_smp_launch(w, heatk_worker, cfg);
}

/* signal every worker to leave and return at once: the heater's stop check confirms on its later ticks that each
 * core left (within 1.5 s), so the loop never waits here */
void hw_heat_stop_all(void)
{
    (void)sb_smp_stop(0);
}

unsigned hw_heat_running(void)
{
    return sb_smp_running();
}

int64_t hw_core_psci(unsigned w)
{
    return sb_smp_affinity(w);
}

const struct sb_wslot *hw_worker_slot(unsigned w)
{
    return sb_smp_slot(w);
}
