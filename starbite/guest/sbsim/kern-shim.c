/* SPDX-License-Identifier: GPL-2.0-only
 * kern-shim: Starbite OS 1.2-alpha1's guest service (starbite/guest/sbos/guest.c, built -DGUEST_QEMU) on QEMU's
 * CPU 0, with the few kernel calls it makes given here: the store (the run's manifest at SBG_QEMU_STORE), the
 * kernel timers (kt_set / kt_cancel), the doorbell from a polled GICC_IAR (Starbite OS's irq_drain), the M3
 * cluster as PSCI AFFINITY_INFO of CPUs 4..7, the heater's exclusion, the log. The scenario run-qemu.sh `kern`.
 *
 * It runs the same guest_selftest_step the phone's self-test image runs (two cycles: the view shown and blitted
 * at 2x into a 1440 x 2960 panel between 40 px bars, a tap that must land on the app's touch mark, home, stop,
 * every guest CPU off; then a start in the background and a stop), after checking that guest_start refuses
 * while the heater is busy. Like sbsim it keeps a line of its own (INTID 40) and checks every 20 ms that it
 * still reaches CPU 0. Last line: "kern-shim: RESULT PASS|FAIL ...".
 */
#include "sb.h"
#include "store.h"
#include "kern.h"
#include "kinput.h"
#include "guest.h"
#include "../sbshm.h"

#define UART      0x09000000UL
#define GICD      SBG_QEMU_GICD
#define GICC      SBG_QEMU_GICC
#define STORE     SBG_QEMU_STORE
#define STORE_TERM 0x4c000000UL             /* run-qemu.sh kern: the terminal's store (Image-guest, its DT, initrd) */
#define PANEL     0x44000000UL              /* 1440 x 2960 x 4 = 17 MB, below the trampoline page (0x4700_0000) */
#define PW        1440U
#define PH        2960U
#define INTID_VT  27U
#define INTID_OWN 40U

#define PSCI_AFFINITY_INFO 0xc4000004UL
#define PSCI_SYSTEM_OFF    0x84000008UL
#define PSCI_VERSION       0x84000000UL

struct manifest {
    uint64_t magic;                         /* "SBSTORE1" */
    uint32_t flags, runs;
    uint64_t img_off, img_size, dtb_off, dtb_size, rd_off, rd_size;
    uint32_t db_guest, db_host;
    uint32_t surf_w, surf_h;
    uint32_t touch_x, touch_y;
    uint32_t boot_timeout_s, pad;
};

uint64_t sbsim_hvc(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);
uint64_t sbsim_semi(uint64_t op, void *blk);
void sbsim_main(void);

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wrb(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline uint8_t rdb(uintptr_t a) { return *(volatile uint8_t *)a; }

/* memcpy / memset: libsb str.c (alignment-checked: safe on Device memory with the MMU off) */

/* ---- console, time ----------------------------------------------------------------------------- */
static void putc_(char c)
{
    while (rd(UART + 0x18) & (1U << 5))
        ;
    wr(UART, (uint32_t)(uint8_t)c);
}
static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n')
            putc_('\r');
        putc_(*s++);
    }
}
static uint64_t frq;
static uint64_t cnt(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
static void vt_arm(uint64_t ticks)
{
    __asm__ volatile("msr cntv_cval_el0, %0\n\tmov x9, #1\n\tmsr cntv_ctl_el0, x9\n\tisb"
                     : : "r"(cnt() + ticks) : "x9", "memory");
}
static void vt_off(void) { __asm__ volatile("msr cntv_ctl_el0, xzr\n\tisb" ::: "memory"); }

/* ---- what guest.c calls from the kernel and libsb ------------------------------------------------- */
struct kstate K;
uint64_t sb_ms(void) { return cnt() * 1000U / frq; }
void sb_mdelay(uint32_t ms)
{
    uint64_t t = sb_ms() + ms;
    while (sb_ms() < t)
        ;
}
void sb_log(const char *l)
{
    puts_("log: ");
    puts_(l);
    puts_("\n");
}
void sb_logf(const char *a, const char *b)
{
    puts_("log: ");
    puts_(a);
    puts_(b);
    puts_("\n");
}
int64_t sb_psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3) { return (int64_t)sbsim_hvc(fn, a1, a2, a3); }
int hw_m3_on(void)
{
    for (unsigned c = 4; c < 8; ++c)
        if (sbsim_hvc(PSCI_AFFINITY_INFO, c, 0, 0) != 1)
            return 1;
    return 0;
}
static unsigned fp_resets, preps, core_ons;
int hw_m3_fp_reset(void) { ++fp_resets; return 0; }
int hw_m3_prep(void) { ++preps; return 1; }
void hw_m3_core_on(unsigned w, int first) { (void)w, (void)first; ++core_ons; }
void hw_wdt_feed(void) {}
static int dirty, heater_fake_busy, released;
void ui_mark_dirty(void) { dirty = 1; }
int heater_m3_busy(void) { return heater_fake_busy; }
int heater_guest_refuses(void) { return guest_owns_m3(); }      /* as heater.c */
void heater_m3_released(void) { ++released; }

/* the store: the run's manifest (Image, DTB, initrd placed by -device loader) */
static struct manifest *mf, *mt;
int store_find(const char *name, struct store_ent *e)
{
    uint64_t off, size;
    if (sb_streq(name, GUEST_STORE_IMAGE))
        off = STORE + mf->img_off, size = mf->img_size;
    else if (sb_streq(name, GUEST_STORE_DTB))
        off = STORE + mf->dtb_off, size = mf->dtb_size;
    else if (sb_streq(name, GUEST_STORE_RD))
        off = STORE + mf->rd_off, size = mf->rd_size;
    else if (mt && sb_streq(name, GUEST_TERM_IMAGE))
        off = STORE_TERM + mt->img_off, size = mt->img_size;
    else if (mt && sb_streq(name, GUEST_TERM_DTB))
        off = STORE_TERM + mt->dtb_off, size = mt->dtb_size;
    else if (mt && sb_streq(name, GUEST_TERM_RD))
        off = STORE_TERM + mt->rd_off, size = mt->rd_size;
    else
        return 0;
    memset(e, 0, sizeof(*e));
    sb_strcpy(e->name, sizeof(e->name), name);
    e->lba = off;
    e->size = size;
    return 1;
}
int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap)
{
    if (e->size > cap)
        return -3;
    const volatile uint64_t *s = (const volatile uint64_t *)(uintptr_t)e->lba;    /* (an address here) */
    volatile uint64_t *d = (volatile uint64_t *)dst;
    for (uint64_t i = 0; i < (e->size + 7) / 8; ++i)
        d[i] = s[i];
    return 0;
}

/* kernel timers: owner KT_GUEST only */
static struct { int on, repeat; uint64_t at; uint32_t period; } kt[2];
int kt_set(unsigned owner, unsigned id, uint64_t now, uint32_t ms, int repeat)
{
    if (owner != KT_GUEST || id > 1)
        return -1;
    kt[id].on = 1;
    kt[id].repeat = repeat;
    kt[id].at = now + ms;
    kt[id].period = ms;
    return 0;
}
void kt_cancel(unsigned owner, unsigned id)
{
    if (owner == KT_GUEST && id <= 1)
        kt[id].on = 0;
}
static void kt_run(void)
{
    uint64_t now = sb_ms();
    for (unsigned id = 0; id < 2; ++id)
        if (kt[id].on && now >= kt[id].at) {
            if (kt[id].repeat && kt[id].period)
                kt[id].at = now + kt[id].period;
            else
                kt[id].on = 0;
            guest_timer(id);
        }
}

/* ---- GIC: Starbite OS's reset (kern/hw.c gic_reset) and the polled interrupt loop ------------------ */
static uint8_t me;
static uint64_t timer_irqs, own_ok, own_lost, db_in, own_sent;
static int own_pending;

static void gic_reset(void)
{
    unsigned lines = ((rd(GICD + 4) & 0x1fU) + 1U) * 32U;
    if (lines > 1020)
        lines = 1020;
    wr(GICD, 0);
    for (unsigned i = 32; i < lines; i += 32) {
        wr(GICD + 0x180 + i / 8, 0xffffffffU);
        wr(GICD + 0x280 + i / 8, 0xffffffffU);
    }
    for (unsigned i = 32; i < lines; i += 4) {
        wr(GICD + 0x800 + i, 0x01010101U);
        wr(GICD + 0x400 + i, 0xa0a0a0a0U);
    }
    wr(GICD + 0x180, 0xffff0000U);
    wr(GICD + 0x280, 0xffffffffU);
    for (unsigned i = 0; i < 32; i += 4)
        wr(GICD + 0x400 + i, 0xa0a0a0a0U);
    wr(GICD, 1);
    wr(GICC + 0x04, 0xffU);
    wr(GICC + 0x08, 0);
    wr(GICC + 0x00, 1);
    me = rdb(GICD + 0x800);
    if (!me)
        me = 1;
    wr(GICD + 0x100, 1U << INTID_VT);
    uintptr_t cfg = GICD + 0xc00 + 4 * (INTID_OWN / 16);
    wr(cfg, rd(cfg) | (2U << (2 * (INTID_OWN % 16))));
    wrb(GICD + 0x800 + INTID_OWN, me);      /* a byte store, as kern/hw.c hw_spi since 1.2 */
    wr(GICD + 0x100 + 4 * (INTID_OWN / 32), 1U << (INTID_OWN % 32));
}

static void irq_drain(void)
{
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t iar = rd(GICC + 0x0c), id = iar & 0x3ffU;
        if (id >= 1020)
            break;
        if (id == INTID_VT) {
            vt_off();
            timer_irqs++;
        } else if (id == INTID_OWN) {
            own_ok++;
            own_pending = 0;
        } else if (id == guest_db_host()) {
            db_in++;
            guest_irq();
        } else if (id >= 16) {
            wr(GICD + 0x180 + 4 * (id / 32), 1U << (id % 32));   /* a stray: disabled again (irq_drain) */
        }
        wr(GICC + 0x10, iar);
    }
}

static void own_check(uint64_t t)
{
    if (own_pending && t - own_sent > 10) {
        own_pending = 0;
        own_lost++;
    }
    if (!own_pending && (t / 20) != (own_sent / 20)) {
        own_pending = 1;
        own_sent = t;
        wr(GICD + 0x200 + 4 * (INTID_OWN / 32), 1U << (INTID_OWN % 32));
    }
}

/* ---- the panel, as Starbite OS's ui.c composes the Linux view; a PPM through semihosting ------------- */
static void bars(void)
{
    for (unsigned y = 0; y < 40; ++y)
        for (unsigned x = 0; x < PW; ++x) {
            wr(PANEL + ((uintptr_t)y * PW + x) * 4, 0xff101828U);
            wr(PANEL + ((uintptr_t)(PH - 40 + y) * PW + x) * 4, 0xff101828U);
        }
}
static uint8_t rowbuf[PW * 3];
static void write_ppm(const char *name)
{
    unsigned long n = 0;
    while (name[n])
        ++n;
    uint64_t ob[3] = {(uint64_t)(uintptr_t)name, 5, n};
    int h = (int)sbsim_semi(0x01, ob);
    if (h < 0)
        return;
    static const char head[] = "P6\n1440 2960\n255\n";
    uint64_t wb[3] = {(uint64_t)h, (uint64_t)(uintptr_t)head, sizeof(head) - 1};
    sbsim_semi(0x05, wb);
    for (uint32_t y = 0; y < PH; ++y) {
        for (uint32_t x = 0; x < PW; ++x) {
            uint32_t c = rd(PANEL + ((uintptr_t)y * PW + x) * 4);
            rowbuf[3 * x] = (uint8_t)(c >> 16);
            rowbuf[3 * x + 1] = (uint8_t)(c >> 8);
            rowbuf[3 * x + 2] = (uint8_t)c;
        }
        uint64_t rb[3] = {(uint64_t)h, (uint64_t)(uintptr_t)rowbuf, PW * 3};
        sbsim_semi(0x05, rb);
    }
    uint64_t cb[1] = {(uint64_t)h};
    sbsim_semi(0x02, cb);
    puts_("kern-shim: wrote ");
    puts_(name);
    puts_("\n");
}

static uint64_t blits;
static void st_open(void)
{
    if (guest_start() == 0)
        guest_show(1);
}
static void st_home(void) { guest_show(0); }
static void st_open_term(void)
{
    if (guest_start_mode(GUEST_TERM) == 0)
        guest_show(1);
}
static const uint32_t *st_panel(void) { return (const uint32_t *)PANEL; }
static const struct guest_st_ops ops = {st_open, st_home, st_open_term, st_panel};

static void pump(void)
{
    vt_arm(frq / 500);
    __asm__ volatile("wfi");
    irq_drain();
    own_check(sb_ms());
    kt_run();
    static uint64_t last_blit;
    uint64_t t = sb_ms();
    if (guest_frame_new() && t - last_blit >= 150) {   /* ui.c presents on each new frame; here at most ~7/s */
        last_blit = t;
        int y0, y1;
        guest_blit((uint32_t *)PANEL, PW, 0, 40, &y0, &y1);
        ++blits;
        dirty = 0;
    }
}

static void logn(const char *a, uint64_t v)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, a);
    ub_dec(&u, v);
    puts_("kern-shim: ");
    puts_(u.b);
    puts_("\n");
}

void sbsim_main(void)
{
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    puts_("\nkern-shim: Starbite OS 1.2's guest.c on QEMU CPU 0 (GUEST_QEMU)\n");
    mf = (struct manifest *)STORE;
    mt = ((struct manifest *)STORE_TERM)->magic == 0x3145524f54534253ULL ? (struct manifest *)STORE_TERM : 0;
    if (!mt)
        puts_("kern-shim: no terminal store at 0x4c000000 (cycle 2 will fail)\n");
    if (mf->magic != 0x3145524f54534253ULL) {
        puts_("kern-shim: RESULT FAIL: no store manifest at 0x48000000\n");
        sbsim_hvc(PSCI_SYSTEM_OFF, 0, 0, 0);
    }
    gic_reset();
    bars();
    K.irq_mode = 1;
    guest_init();
    int ok = 1;

    /* the exclusion, before anything runs: a busy heater refuses the start (-2, nothing loaded) */
    heater_fake_busy = 1;
    int r = guest_start();
    heater_fake_busy = 0;
    if (r != -2 || guest_state() != GUEST_OFF) {
        logn("guest_start while the heater is busy did not refuse with -2: ", (uint64_t)(-r));
        ok = 0;
    } else {
        puts_("kern-shim: guest_start refused while the heater is busy (-2): ok\n");
    }

    int res = 0;
    uint64_t t_step = 0, t0 = sb_ms();
    while (ok && !res) {
        pump();
        uint64_t t = sb_ms();
        if (t - t_step >= 100) {
            t_step = t;
            res = guest_selftest_step(&ops);
        }
        if (t - t0 > 240000) {
            puts_("kern-shim: the self-test step did not finish in 240 s\n");
            res = -1;
        }
    }
    if (res < 0)
        ok = 0;
    write_ppm("kern-panel.ppm");            /* the last frame shown: the app with the tap's mark */
    /* the host keeps running after the guest powered off: its timer and its own line still work */
    uint64_t t_end = sb_ms() + 300, ok0 = own_ok, tick0 = timer_irqs;
    while (sb_ms() < t_end)
        pump();
    int alive = own_ok > ok0 && timer_irqs > tick0;
    if (!alive || own_lost)
        ok = 0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, ok ? "PASS" : "FAIL");
    ub_s(&u, ": guest.c's self-test step ");
    ub_s(&u, res > 0 ? "passed" : "FAILED");
    ub_s(&u, ", blits ");
    ub_dec(&u, blits);
    ub_s(&u, ", doorbells from the guest ");
    ub_dec(&u, db_in);
    ub_s(&u, ", own line ok ");
    ub_dec(&u, own_ok);
    ub_s(&u, " lost ");
    ub_dec(&u, own_lost);
    ub_s(&u, ", FlexPMU resets ");
    ub_dec(&u, fp_resets);
    ub_s(&u, " preps ");
    ub_dec(&u, preps);
    ub_s(&u, ", cluster released to the heater ");
    ub_dec(&u, released);
    ub_s(&u, ", host alive after ");
    ub_dec(&u, (uint64_t)alive);
    puts_("kern-shim: RESULT ");
    puts_(u.b);
    puts_("\n");
    sbsim_hvc(PSCI_SYSTEM_OFF, 0, 0, 0);
    for (;;)
        __asm__ volatile("wfi");
}
