/* SPDX-License-Identifier: GPL-2.0-only
 * sbsim: the QEMU stand-in for Starbite OS in the 0.9 app-support prototype (docs/STARBITE-OS-LINUX-M3.md 7).
 *
 * QEMU virt, gic-version=2, -smp 8. sbsim plays Starbite OS on CPU 0 (CPUs 1..3 stand for the other A55s
 * and stay off here) and drives the guest through sbghost (../host), the same OS-independent core the phone
 * probe and Starbite OS use:
 *   - owns the GIC distributor: resets it exactly as Starbite OS's gic_reset (kern/hw.c), keeps the virtual
 *     timer and its own SPI (INTID 40, the stand-in for the Wi-Fi line) and checks every 20 ms that the SPI
 *     still reaches CPU 0 (software-pended through GICD_ISPENDR): the measurement of the GIC hazard;
 *   - loads the guest from the "store" (Image, DTB, initrd put in RAM by -device loader at 0x48000000) and
 *     starts the first "M3" (CPU 4) through sbghost; Linux brings up CPUs 5..7 itself;
 *   - prints the guest's kernel log, composites the guest surface into its panel between Starbite's bars,
 *     sends a scripted tap / drag / key, checks the touch mark landed in the app's pixels, writes the panel as
 *     a PPM (semihosting), stops the guest, waits for PSCI AFFINITY_INFO OFF on all four guest CPUs, and boots
 *     it again (cold) to show it restarts. Last line: "sbsim: RESULT PASS|FAIL ...".
 */
#include <stdint.h>
#include "../sbshm.h"
#include "../host/sbghost.h"

#define UART      0x09000000UL
#define GICD      SBG_QEMU_GICD
#define GICC      SBG_QEMU_GICC
#define STORE     SBG_QEMU_STORE
#define PANEL     0x44000000UL
#define INTID_VT  27U
#define INTID_OWN 40U                   /* sbsim's own device line (Starbite: Wi-Fi SPI 253 = INTID 285) */
#define BAR       40U                   /* Starbite's status / navigation bars around the guest surface */

#define PSCI_AFFINITY_INFO 0xc4000004UL
#define PSCI_CPU_ON        0xc4000003UL
#define PSCI_SYSTEM_OFF    0x84000008UL
#define PSCI_VERSION       0x84000000UL

enum { F_REPAIR = 1, F_STRICT = 2, F_STUB = 4 };

struct manifest {
    uint64_t magic;                     /* "SBSTORE1" */
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

/* the compiler may call these for struct copies (byte-wise: the MMU is off, every access is Device memory) */
void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n)
{
    volatile uint8_t *p = d;
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n)
{
    volatile uint8_t *p = d;
    const volatile uint8_t *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

/* ---- console ------------------------------------------------------------------------------------- */
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
static void putu(uint64_t v)
{
    char b[24];
    int i = 0;
    do {
        b[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i)
        putc_(b[--i]);
}
static void putx(uint64_t v)
{
    puts_("0x");
    int started = 0;
    for (int s = 60; s >= 0; s -= 4) {
        unsigned d = (unsigned)(v >> s) & 15U;
        if (d || started || !s) {
            putc_("0123456789abcdef"[d]);
            started = 1;
        }
    }
}

/* ---- time ---------------------------------------------------------------------------------------- */
static uint64_t frq;
static uint64_t cnt(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
static uint64_t now_ms(void) { return cnt() * 1000U / frq; }
static void vt_arm(uint64_t ticks)
{
    __asm__ volatile("msr cntv_cval_el0, %0\n\tmov x9, #1\n\tmsr cntv_ctl_el0, x9\n\tisb"
                     : : "r"(cnt() + ticks) : "x9", "memory");
}
static void vt_off(void) { __asm__ volatile("msr cntv_ctl_el0, xzr\n\tisb" ::: "memory"); }

/* ---- GIC: Starbite OS's reset (kern/hw.c gic_reset), then the line sbsim owns ---------------------- */
static uint8_t me;                      /* this CPU interface's bit */
static struct manifest *mf;

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
    wr(cfg, rd(cfg) | (2U << (2 * (INTID_OWN % 16))));                 /* edge: a software pend latches */
    wrb(GICD + 0x800 + INTID_OWN, me);
    wr(GICD + 0x100 + 4 * (INTID_OWN / 32), 1U << (INTID_OWN % 32));
    puts_("sbsim: GIC ");
    putu(lines);
    puts_(" lines, CPU interface bit ");
    putx(me);
    puts_(", own SPI INTID 40 + timer PPI 27\n");
}

/* ---- semihosting: write the panel as a PPM next to QEMU ------------------------------------------- */
static int semi_open(const char *name)
{
    unsigned long n = 0;
    while (name[n])
        ++n;
    uint64_t blk[3] = {(uint64_t)(uintptr_t)name, 5 /* "wb" */, n};
    return (int)sbsim_semi(0x01, blk);
}
static void semi_write(int h, const void *p, uint64_t n)
{
    uint64_t blk[3] = {(uint64_t)h, (uint64_t)(uintptr_t)p, n};
    sbsim_semi(0x05, blk);
}
static void semi_close(int h)
{
    uint64_t blk[1] = {(uint64_t)h};
    sbsim_semi(0x02, blk);
}

/* ---- the panel ------------------------------------------------------------------------------------- */
static uint32_t pw, ph;
static uint8_t rowbuf[4096 * 3];

static void write_ppm(const char *name)
{
    int h = semi_open(name);
    if (h < 0) {
        puts_("sbsim: semihosting open failed (QEMU without -semihosting?)\n");
        return;
    }
    char head[32];
    int n = 0;
    head[n++] = 'P'; head[n++] = '6'; head[n++] = '\n';
    uint32_t v[2] = {pw, ph};
    for (int k = 0; k < 2; ++k) {
        char t[12];
        int i = 0;
        uint32_t x = v[k];
        do {
            t[i++] = (char)('0' + x % 10);
            x /= 10;
        } while (x);
        while (i)
            head[n++] = t[--i];
        head[n++] = k ? '\n' : ' ';
    }
    head[n++] = '2'; head[n++] = '5'; head[n++] = '5'; head[n++] = '\n';
    semi_write(h, head, (uint64_t)n);
    for (uint32_t y = 0; y < ph; ++y) {
        const volatile uint32_t *s = (const volatile uint32_t *)(PANEL + (uintptr_t)y * pw * 4);
        for (uint32_t x = 0; x < pw; ++x) {
            uint32_t c = s[x];
            rowbuf[3 * x] = (uint8_t)(c >> 16);
            rowbuf[3 * x + 1] = (uint8_t)(c >> 8);
            rowbuf[3 * x + 2] = (uint8_t)c;
        }
        semi_write(h, rowbuf, (uint64_t)pw * 3);
    }
    semi_close(h);
    puts_("sbsim: wrote ");
    puts_(name);
    puts_("\n");
}

static void fill(uint32_t y0, uint32_t h, uint32_t color)
{
    uint64_t c2 = ((uint64_t)color << 32) | color;
    for (uint32_t y = y0; y < y0 + h; ++y) {
        volatile uint64_t *d = (volatile uint64_t *)(PANEL + (uintptr_t)y * pw * 4);
        for (uint32_t x = 0; x < pw / 2; ++x)
            d[x] = c2;
    }
}

/* the guest's front buffer into the panel between Starbite's bars */
static void composite(const volatile uint32_t *px, uint32_t stride)
{
    uint32_t w = mf->surf_w < pw ? mf->surf_w : pw;
    for (uint32_t y = 0; y < mf->surf_h && BAR + y < ph - BAR; ++y) {
        const volatile uint64_t *s = (const volatile uint64_t *)((uintptr_t)px + (uintptr_t)y * stride);
        volatile uint64_t *d = (volatile uint64_t *)(PANEL + (uintptr_t)(BAR + y) * pw * 4);
        for (uint32_t x = 0; x < w / 2; ++x)
            d[x] = s[x];
    }
}

static uint32_t panel_px(uint32_t x, uint32_t y) { return rd(PANEL + ((uintptr_t)y * pw + x) * 4); }

/* ---- interrupts: polled from GICC_IAR (PSTATE.I stays set), as Starbite OS does -------------------- */
static struct {
    uint64_t timer_irqs, own_ok, own_lost, db_from_guest, own_sent_ms;
    int own_pending;
} st;
static uint32_t db_host_id;

static void irq_poll(void)
{
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t iar = rd(GICC + 0x0c), id = iar & 0x3ffU;
        if (id >= 1020)
            break;
        if (id == INTID_VT) {
            vt_off();
            st.timer_irqs++;
        } else if (id == INTID_OWN) {
            st.own_ok++;
            st.own_pending = 0;
        } else if (id == db_host_id && id) {
            st.db_from_guest++;
            sbg_doorbell();
        }
        wr(GICC + 0x10, iar);
    }
}

/* the functional check of sbsim's own line: pend it, it must arrive within 10 ms (sbghost repairs) */
static void own_check(uint64_t t)
{
    if (st.own_pending && t - st.own_sent_ms > 10) {
        st.own_pending = 0;
        st.own_lost++;
        puts_("sbsim: OWN SPI LOST: enabled ");
        putu((rd(GICD + 0x100 + 4 * (INTID_OWN / 32)) >> (INTID_OWN % 32)) & 1U);
        puts_(", ITARGETSR byte ");
        putx(rdb(GICD + 0x800 + INTID_OWN));
        puts_(" (mine ");
        putx(me);
        puts_("), GICD_CTLR ");
        putx(rd(GICD));
        puts_("\n");
    }
    if (!st.own_pending && (t / 20) != (st.own_sent_ms / 20)) {
        st.own_pending = 1;
        st.own_sent_ms = t;
        wr(GICD + 0x200 + 4 * (INTID_OWN / 32), 1U << (INTID_OWN % 32));       /* ISPENDR */
    }
}

/* ---- the shim for sbghost ------------------------------------------------------------------------- */
static void host_log(const char *l) { puts_("sbsim: "); puts_(l); puts_("\n"); }
static void guest_log(const char *l) { puts_("  linux| "); puts_(l); puts_("\n"); }
static int64_t cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx)
{
    return (int32_t)sbsim_hvc(PSCI_CPU_ON, mpidr, entry, ctx);
}
static int cluster_off(void)
{
    for (unsigned c = 4; c < 8; ++c)
        if (sbsim_hvc(PSCI_AFFINITY_INFO, c, 0, 0) != 1)
            return 0;
    return 1;
}
static const struct sbg_os shim = {
    .ms = now_ms, .log = host_log, .guest_log = guest_log, .cpu_on = cpu_on, .cluster_off = cluster_off,
};

/* ---- one guest run ----------------------------------------------------------------------------------- */
static int run_guest(unsigned run)
{
    if (sbg_load((const void *)(STORE + mf->img_off), mf->img_size, (const void *)(STORE + mf->dtb_off),
                 mf->dtb_size, (const void *)(STORE + mf->rd_off), mf->rd_size) < 0 || sbg_start() < 0)
        return -1;
    fill(0, BAR, 0x00202a44U);                 /* Starbite's status bar */
    fill(ph - BAR, BAR, 0x00202a44U);          /* its navigation bar */
    uint32_t seq = 0, tstep = 0, touch_seq = 0, frames = 0;
    uint64_t last_comp = 0, t_touch = 0;
    int touch_ok = 0;
    const struct sbg_stats *s = sbg_stats();
    volatile struct sbshm_hdr *h = sbg_hdr();
    for (;;) {
        vt_arm(frq / 500);
        __asm__ volatile("wfi");
        irq_poll();
        uint64_t t = now_ms();
        own_check(t);
        sbg_poll();
        s = sbg_stats();
        if (s->state == SBG_FAILED)
            return -1;
        if (s->state == SBG_OFF)
            break;
        const volatile uint32_t *px;
        uint32_t stride;
        if (t - last_comp >= 100 && sbg_frame(&seq, &px, &stride)) {
            last_comp = t;
            composite(px, stride);
            frames++;
        }
        /* scripted input once the app has drawn a few frames: tap, drag, release, a key */
        if (s->state == SBG_RUNNING && frames >= 3 && tstep == 0) {
            sbg_touch(0, SBG_TOUCH_DOWN, (int32_t)mf->touch_x, (int32_t)mf->touch_y);
            tstep = 1;
            t_touch = t;
        } else if (tstep == 1 && t - t_touch > 150) {
            sbg_touch(0, SBG_TOUCH_MOVE, (int32_t)mf->touch_x + 60, (int32_t)mf->touch_y);
            tstep = 2;
        } else if (tstep == 2 && t - t_touch > 300) {
            sbg_touch(0, SBG_TOUCH_UP, (int32_t)mf->touch_x + 60, (int32_t)mf->touch_y);
            sbg_key(158 /* KEY_BACK */, 1);
            sbg_key(158, 0);
            tstep = 3;
        } else if (tstep == 3 && h->touches_seen >= 3) {
            tstep = 4;
            touch_seq = h->surf_seq;
        } else if (tstep == 4 && h->surf_seq - touch_seq >= 2 && seq == h->surf_seq) {
            uint32_t c = panel_px(mf->touch_x, BAR + mf->touch_y) & 0xffffffU;
            touch_ok = (c == 0xffd700U);
            puts_("sbsim: touch reached the app: pixel at the tap ");
            putx(c);
            puts_(touch_ok ? " (the app's touch mark)\n" : " (NOT the touch mark)\n");
            tstep = 5;
            char name[] = "sbsim-panel-run0.ppm";
            name[15] = (char)('0' + run % 10);
            write_ppm(name);
            sbg_stop();
        }
    }
    puts_("sbsim: run ");
    putu(run);
    puts_(": HELLO after ");
    putu(s->t_hello);
    puts_(" ms, frames composited ");
    putu(frames);
    puts_(", touch ");
    putu((uint64_t)touch_ok);
    puts_(", cluster off ");
    putu(s->t_off);
    puts_(" ms after MSG_STOP, guest GIC CPU interface ");
    putx(s->guest_cpuif);
    puts_("\n");
    return (s->hello && frames >= 3 && touch_ok) ? 0 : -1;
}

/* ---- the phone probe's M3 stub (probe/m3stub.S, built with HVC) on CPUs 4..7 ------------------------ */
extern const char m3stub_start[], m3stub_lit[], m3stub_end[];
struct blk {
    uint64_t state, mpidr, el, x0, cpuif, intid, frq, wfis, t0, sctlr, t1, off_ret;
};
static int stub_test(void)
{
    const uint64_t at = SBG_QEMU_LINUX_BASE + 0x80000, blk = SBG_QEMU_SHM_BASE + 0x800;
    volatile struct blk *b = (volatile struct blk *)blk;
    unsigned dbg = mf->db_guest, dbh = mf->db_host, ok = 0;
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned i = 0; i < sizeof(struct blk) / 8; ++i)
            ((volatile uint64_t *)blk)[i] = 0;
        uint64_t n = (uint64_t)(m3stub_end - m3stub_start), lit = (uint64_t)(m3stub_lit - m3stub_start);
        for (uint64_t i = 0; i < n; i += 4)
            wr((uintptr_t)(at + i), *(const volatile uint32_t *)(m3stub_start + i));
        volatile uint64_t *l = (volatile uint64_t *)(uintptr_t)(at + lit);
        l[0] = blk; l[1] = GICD; l[2] = GICC; l[3] = dbh; l[4] = 2 * frq;
        volatile uint32_t *tr = (volatile uint32_t *)(uintptr_t)(SBG_QEMU_SHM_BASE + SBSHM_TRAMP_OFF);
        for (unsigned i = 0; i < SBG_TRAMP_WORDS; ++i)
            tr[i] = sbg_tramp[i];
        tr[SBG_TRAMP_IMG] = (uint32_t)at;
        tr[SBG_TRAMP_IMG + 1] = 0;
        tr[SBG_TRAMP_DTB] = 0x44544221U + c;
        tr[SBG_TRAMP_DTB + 1] = 0;
        uint64_t entry = c == 0 ? at : SBG_QEMU_SHM_BASE + SBSHM_TRAMP_OFF;
        int64_t r = (int32_t)sbsim_hvc(PSCI_CPU_ON, 4 + c, entry, 0x434f4e54U + c);
        uint64_t t0 = now_ms();
        while (b->state < 1 && now_ms() - t0 < 1000)
            ;
        uint8_t cif = (uint8_t)b->cpuif;
        if (b->state >= 1 && dbg && cif) {
            uintptr_t cfga = GICD + 0xc00 + 4 * (dbg / 16);
            wr(cfga, rd(cfga) | (2U << (2 * (dbg % 16))));
            wrb(GICD + 0x800 + dbg, cif);
            wr(GICD + 0x100 + 4 * (dbg / 32), 1U << (dbg % 32));
            wr(GICD + 0x200 + 4 * (dbg / 32), 1U << (dbg % 32));
        }
        while (b->state < 2 && now_ms() - t0 < 3000)
            ;
        int host_back = 0;
        for (uint64_t t1 = now_ms(); now_ms() - t1 < 100 && !host_back;) {
            uint32_t iar = rd(GICC + 0x0c), id = iar & 0x3ffU;
            if (id < 1020) {
                wr(GICC + 0x10, iar);
                host_back = id == dbh;
            }
        }
        uint64_t t2 = now_ms();
        while (sbsim_hvc(PSCI_AFFINITY_INFO, 4 + c, 0, 0) != 1 && now_ms() - t2 < 100)
            ;
        int off = sbsim_hvc(PSCI_AFFINITY_INFO, 4 + c, 0, 0) == 1;
        wr(GICD + 0x180 + 4 * (dbg / 32), 1U << (dbg % 32));
        puts_("sbsim: stub on CPU ");
        putu(4 + c);
        puts_(c == 0 ? " direct" : " trampoline");
        puts_(": CPU_ON ");
        putu((uint64_t)(r < 0 ? -r : r));
        puts_(", state ");
        putu(b->state);
        puts_(", MPIDR ");
        putx(b->mpidr);
        puts_(", EL");
        putu((b->el >> 2) & 3U);
        puts_(", x0 ");
        putx(b->x0);
        puts_(", cpuif ");
        putx(cif);
        puts_(", woke by ");
        putu(b->intid);
        puts_(" after ");
        putu(b->wfis);
        puts_(" WFI, host doorbell ");
        putu((uint64_t)host_back);
        puts_(", off ");
        putu((uint64_t)off);
        puts_("\n");
        uint64_t want_x0 = c == 0 ? 0x434f4e54U + c : 0x44544221U + c;
        uint64_t crumb = ((volatile uint64_t *)tr)[SBG_TRAMP_CRUMB / 2];
        if (c && crumb != SBG_QEMU_SHM_BASE + SBSHM_TRAMP_OFF + 4 * SBG_TRAMP_CRUMB) {
            puts_("sbsim: the trampoline breadcrumb is wrong: ");
            putx(crumb);
            puts_("\n");
            continue;
        }
        if (!r && b->state == 2 && b->intid == dbg && host_back && off && b->x0 == want_x0 && cif == (1U << (4 + c)))
            ++ok;
    }
    return ok == 4;
}

void sbsim_main(void)
{
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    puts_("\nsbsim: Starbite OS stand-in on CPU 0, PSCI ");
    putx(sbsim_hvc(PSCI_VERSION, 0, 0, 0));
    puts_(", timer ");
    putu(frq);
    puts_(" Hz\n");
    mf = (struct manifest *)STORE;
    if (mf->magic != 0x3145524f54534253ULL) {
        puts_("sbsim: no store manifest at 0x48000000\n");
        sbsim_hvc(PSCI_SYSTEM_OFF, 0, 0, 0);
    }
    pw = mf->surf_w;
    ph = mf->surf_h + 2 * BAR;
    db_host_id = mf->db_host;
    gic_reset();
    struct sbg_cfg cfg = {
        .linux_base = SBG_QEMU_LINUX_BASE, .linux_size = SBG_QEMU_LINUX_SIZE,
        .shm_base = SBG_QEMU_SHM_BASE, .shm_size = SBG_QEMU_SHM_SIZE,
        .gicd = GICD, .first_mpidr = 4,
        .surf_w = mf->surf_w, .surf_h = mf->surf_h,
        .db_guest = mf->db_guest, .db_host = mf->db_host,
        .host_lines = {INTID_OWN, 0},
        .repair = (mf->flags & F_REPAIR) ? 1U : 0U,
        .boot_timeout_ms = (mf->boot_timeout_s ? mf->boot_timeout_s : 300) * 1000U,
        .stop_timeout_ms = 60000,
    };
    sbg_init(&shim, &cfg, me);
    sbg_gic_prepare();
    puts_("sbsim: flags ");
    putx(mf->flags);
    puts_(mf->flags & F_REPAIR ? " (repair: re-assert host lines)" : "");
    puts_(mf->flags & F_STRICT ? " (strict: no host line may be lost)" : "");
    puts_(", doorbells guest ");
    putu(mf->db_guest);
    puts_(" host ");
    putu(mf->db_host);
    puts_("\n");

    if (mf->flags & F_STUB) {
        int ok = stub_test();
        puts_("sbsim: RESULT ");
        puts_(ok ? "PASS" : "FAIL");
        puts_(": the probe's M3 stub on four CPUs (x0, cpuif, doorbell in WFI, host doorbell, CPU_OFF)\n");
        sbsim_hvc(PSCI_SYSTEM_OFF, 0, 0, 0);
    }
    unsigned runs = mf->runs ? (mf->runs > 4 ? 4 : mf->runs) : 1, done = 0;
    int ok = 1;
    for (unsigned r = 0; r < runs && ok; ++r) {
        if (run_guest(r) < 0)
            ok = 0;
        else
            ++done;
    }
    /* sbsim keeps running after the guest powered off: the timer and the own SPI must still work */
    uint64_t t_end = now_ms() + 300, ok0 = st.own_ok, tick0 = st.timer_irqs;
    while (now_ms() < t_end) {
        vt_arm(frq / 500);
        __asm__ volatile("wfi");
        irq_poll();
        own_check(now_ms());
    }
    int alive = st.own_ok > ok0 && st.timer_irqs > tick0;
    const struct sbg_stats *s = sbg_stats();
    if (!alive)
        ok = 0;
    if ((mf->flags & F_STRICT) && (st.own_lost || s->lines_lost))
        ok = 0;
    puts_("sbsim: RESULT ");
    puts_(ok ? "PASS" : "FAIL");
    puts_(": runs ");
    putu(done);
    puts_("/");
    putu(runs);
    puts_(", timer irqs ");
    putu(st.timer_irqs);
    puts_(", own SPI ok ");
    putu(st.own_ok);
    puts_(" lost ");
    putu(st.own_lost);
    puts_(", host lines taken ");
    putu(s->lines_lost);
    puts_(" re-asserted ");
    putu(s->lines_repaired);
    puts_(", doorbells to guest ");
    putu(s->doorbells_out);
    puts_(" from guest ");
    putu(st.db_from_guest);
    puts_(", host alive after guest off ");
    putu((uint64_t)alive);
    if (s->why) {
        puts_(", why: ");
        puts_(s->why);
    }
    puts_("\n");
    sbsim_hvc(PSCI_SYSTEM_OFF, 0, 0, 0);
    for (;;)
        __asm__ volatile("wfi");
}
