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
 *   - sbblk (contract v2, run-desktop.sh): when the store's manifest page carries a block-device table (struct
 *     blkman at STORE + BLKMAN_OFF: host file names), sbsim opens the files through semihosting and serves them
 *     as the guest's block devices (sbghost's blk_io: SYS_SEEK + SYS_READ / SYS_WRITE on the request's slot).
 *     Writes go straight to the overlay's file, so they persist across QEMU runs. Flag F_BLKTEST: no scripted
 *     input; the run ends when the guest's log says SBBLK-TEST-DONE (then MSG_STOP and the usual wait for the
 *     cluster to be off) and passes when no "sbblk-test: FAIL" line came before it.
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

#ifndef SBSIM_BOOT_WARN_MS
#define SBSIM_BOOT_WARN_MS 0U
#endif
enum { F_REPAIR = 1, F_STRICT = 2, F_STUB = 4, F_TERM = 8, F_POPUP = 32, F_BLKTEST = 64, F_V3 = 128, F_RETRY = 256,
       F_FREEZE = 512 };   /* instant start: after the terminal's check, two freeze / thaw cycles, a tap, then the stop */
/* F_RETRY: a failed run whose M3 cluster is off (a start given up and stopped) is followed by the next run, as the
 * next open on the phone (the verdict stays FAIL: the test reads the lines) */

struct manifest {
    uint64_t magic;                     /* "SBSTORE1" */
    uint32_t flags, runs;
    uint64_t img_off, img_size, dtb_off, dtb_size, rd_off, rd_size;
    uint32_t db_guest, db_host;
    uint32_t surf_w, surf_h;
    uint32_t touch_x, touch_y;
    uint32_t boot_timeout_s, pad;
};

/* the block devices (optional, in the manifest page): files next to QEMU, by semihosting */
#define BLKMAN_OFF   0x200U
#define BLKMAN_MAGIC 0x5645444b4c424253ULL      /* "SBBLKDEV" */
struct blkman {
    uint64_t magic;
    struct {
        uint32_t flags;                 /* SBSHM_BLKDEV_PRESENT | SBSHM_BLKDEV_RO; 0 = no device */
        uint32_t pad;
        char name[20];                  /* sbshm_blkdev.name */
        char path[228];                 /* the host file, NUL-terminated */
    } d[SBSHM_BLK_DEVS];
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

/* ---- sbblk: file-backed block devices ------------------------------------------------------------------- */
static struct sbshm_blkdev blkdevs[SBSHM_BLK_DEVS];
static int blk_fd[SBSHM_BLK_DEVS];
static unsigned blk_n;

static void blk_open(void)
{
    const struct blkman *bm = (const struct blkman *)(STORE + BLKMAN_OFF);
    if (bm->magic != BLKMAN_MAGIC)
        return;
    for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i) {
        blk_fd[i] = -1;
        if (!(bm->d[i].flags & SBSHM_BLKDEV_PRESENT) || !bm->d[i].path[0])
            continue;
        int ro = (bm->d[i].flags & SBSHM_BLKDEV_RO) != 0;
        unsigned long n = 0;
        while (bm->d[i].path[n])
            ++n;
        uint64_t ob[3] = {(uint64_t)(uintptr_t)bm->d[i].path, ro ? 1 /* "rb" */ : 3 /* "r+b" */, n};
        int h = (int)sbsim_semi(0x01, ob);
        uint64_t fb[1] = {(uint64_t)h};
        int64_t len = h < 0 ? -1 : (int64_t)sbsim_semi(0x0c, fb);      /* SYS_FLEN */
        puts_("sbsim: sbblk");
        putu(i);
        puts_(" \"");
        puts_(bm->d[i].name);
        puts_("\" = ");
        puts_(bm->d[i].path);
        if (h < 0 || len < 512) {
            puts_(": cannot open (the device is absent)\n");
            continue;
        }
        blk_fd[i] = h;
        blkdevs[i].sectors = (uint64_t)len / 512U;
        blkdevs[i].flags = SBSHM_BLKDEV_PRESENT | (ro ? SBSHM_BLKDEV_RO : 0U);
        for (unsigned k = 0; k < sizeof(blkdevs[i].name) - 1 && bm->d[i].name[k]; ++k)
            blkdevs[i].name[k] = bm->d[i].name[k];
        ++blk_n;
        puts_(", ");
        putu((uint64_t)len >> 20);
        puts_(ro ? " MiB, read-only\n" : " MiB, read-write\n");
    }
}

/* sbghost's blk_io. SYS_READ / SYS_WRITE return the bytes NOT transferred. A flush has nothing to do: QEMU has
 * written the bytes to the host file already (its page cache; the test host does not lose power). */
static int blk_io(uint32_t dev, uint32_t op, uint64_t sector, uint32_t count, void *buf)
{
    if (dev >= SBSHM_BLK_DEVS || blk_fd[dev] < 0)
        return -SBG_ENODEV;
    if (op == SBG_BLK_FLUSH)
        return 0;
#ifdef SBSIM_CARD_US_PER_SECTOR
    /* the phone's card as it is today (~2.9 MB/s: 176 us a sector, -DSBSIM_CARD_US_PER_SECTOR=176): each request
     * takes as long as it would there, on the host's CPU, one at a time (the card is serial) */
    {
        uint64_t end = cnt() + (uint64_t)count * SBSIM_CARD_US_PER_SECTOR * frq / 1000000U;
        while (cnt() < end)
            ;
    }
#endif
    uint64_t sk[2] = {(uint64_t)blk_fd[dev], sector * 512U};
    if (sbsim_semi(0x0a, sk))                                           /* SYS_SEEK */
        return -SBG_EIO;
    uint64_t io[3] = {(uint64_t)blk_fd[dev], (uint64_t)(uintptr_t)buf, (uint64_t)count * 512U};
    return sbsim_semi(op == SBG_BLK_WRITE ? 0x05 : 0x06, io) ? -SBG_EIO : 0;
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

/* the guest's front buffer into the panel between Starbite's bars: only the rows newer than the frame drawn
 * last (sbshm.h "damage per row"), every row when the guest keeps no table */
static uint32_t drawn_seq;
static int damage_bad;
static uint64_t rows_copied, frames_drawn;
static void composite(const volatile uint32_t *px, uint32_t stride, uint32_t seq)
{
    uint32_t w = mf->surf_w < pw ? mf->surf_w : pw;
    const volatile uint32_t *rows = sbg_rows();
    frames_drawn++;
    for (uint32_t y = 0; y < mf->surf_h && BAR + y < ph - BAR; ++y) {
        if (rows && drawn_seq && !sbshm_row_newer(rows[y], drawn_seq))
            continue;
        rows_copied++;
        const volatile uint64_t *s = (const volatile uint64_t *)((uintptr_t)px + (uintptr_t)y * stride);
        volatile uint64_t *d = (volatile uint64_t *)(PANEL + (uintptr_t)(BAR + y) * pw * 4);
        for (uint32_t x = 0; x < w / 2; ++x)
            d[x] = s[x];
    }
    drawn_seq = seq;
}

/* every composited row against the front buffer of the frame drawn (after a composite with nothing newer): the
 * damage-per-row copy must leave the panel exactly equal to the guest's frame. Returns the rows that differ. */
static uint32_t verify_panel(const volatile uint32_t *px, uint32_t stride)
{
    uint32_t bad = 0, w = mf->surf_w < pw ? mf->surf_w : pw;
    for (uint32_t y = 0; y < mf->surf_h && BAR + y < ph - BAR; ++y) {
        const volatile uint64_t *s = (const volatile uint64_t *)((uintptr_t)px + (uintptr_t)y * stride);
        const volatile uint64_t *d = (const volatile uint64_t *)(PANEL + (uintptr_t)(BAR + y) * pw * 4);
        for (uint32_t x = 0; x < w / 2; ++x)
            if (d[x] != s[x]) {
                ++bad;
                break;
            }
    }
    return bad;
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
static int term_ok_seen, blktest_done, blktest_fail;
static int frz_logged, thaw_logged;         /* F_FREEZE: sbguest's "freezing" / "thawed" lines seen */
static uint32_t frz_ok, frz_cycles, frz_quiet, touch0;
static uint64_t frz_db0;
static int has(const char *l, const char *w)
{
    for (; *l; ++l) {
        unsigned i = 0;
        while (w[i] && l[i] == w[i])
            ++i;
        if (!w[i])
            return 1;
    }
    return 0;
}
static void guest_log(const char *l)
{
    puts_("  linux| ");
    puts_(l);
    puts_("\n");
    if (mf->flags & F_BLKTEST) {            /* the block tests' verdict lines (blk/test-sbinit) */
        blktest_done |= has(l, "SBBLK-TEST-DONE") | has(l, "V3-TEST-DONE");     /* (v3: the services' tests) */
        blktest_fail |= has(l, "sbblk-test: FAIL") | (has(l, "v3-test: ") && has(l, ": FAIL"));
    }
    frz_logged |= has(l, "sbguest: freezing");
    thaw_logged |= has(l, "sbguest: thawed");
    for (const char *p = l; *p; ++p) {      /* the terminal scenario's typed command, run by the guest's shell */
        const char *w = "STARBITE-TERM-OK";
        unsigned i = 0;
        while (w[i] && p[i] == w[i])
            ++i;
        if (!w[i] && p > l && p[-1] != 'O')  /* not the echo of the command line itself ("echo STARBITE...") */
            term_ok_seen = 1;
    }
}
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
/* contract v3 (F_V3): the services' stand-ins live in sim_v3.c (sim_net.c, sim_p9.c, sim_snd.c) */
#include "sim_v3.c"
static const struct sbg_os shim_blk = {     /* with block devices in the manifest */
    .ms = now_ms, .log = host_log, .guest_log = guest_log, .cpu_on = cpu_on, .cluster_off = cluster_off,
    .blk_io = blk_io,
    .unix_s = sim_unix_s, .net_rx = sim_net_rx, .fs = SIM_FS, .snd = SIM_SND,
};
static void blk_report(unsigned run)
{
    const struct sbg_stats *s = sbg_stats();
    puts_("sbsim: run ");
    putu(run);
    puts_(": sbblk reads ");
    putu(s->blk_reads);
    puts_(" (");
    putu(s->blk_rd_sectors >> 11);
    puts_(" MiB), writes ");
    putu(s->blk_writes);
    puts_(" (");
    putu(s->blk_wr_sectors >> 11);
    puts_(" MiB), flushes ");
    putu(s->blk_flushes);
    puts_(", errors ");
    putu(s->blk_errors);
    puts_(", completion doorbells ");
    putu(s->blk_doorbells);
    puts_(", most requests in one poll ");
    putu(s->blk_batch_max);
    puts_(", polls cut at the bound ");
    putu(s->blk_capped);
    puts_(", longest poll ");
    putu(s->blk_poll_ms_max);
    puts_(" ms (totals since sbsim started)\n");
}

/* ---- one guest run ----------------------------------------------------------------------------------- */
static int run_guest(unsigned run)
{
#ifdef SBSIM_STEPPED
    /* cp7o: Starbite OS's stepped start: the shared region cleared 2 MiB a step (sbg_prep_step; first poisoned, so a
     * byte it misses shows), the files put in place by the host, then sbg_load_placed (which clears nothing) */
    {
        volatile uint64_t *shm = (volatile uint64_t *)(uintptr_t)SBG_QEMU_SHM_BASE;
        for (uint64_t i = 0; i < 0x200000U / 8; ++i)
            shm[i] = 0xa5a5a5a5a5a5a5a5ULL;        /* the header, rings and ramoops zone poisoned */
        unsigned steps = 1;
        while (!sbg_prep_step(2U << 20))
            ++steps;
        uint64_t bad = 0;
        for (uint64_t i = 0; i < 0x200000U / 8; ++i)
            bad += shm[i] != 0;
        puts_("sbsim: stepped: the shared region cleared in ");
        putu(steps);
        puts_(" steps of 2 MiB, poisoned words left ");
        putu(bad);
        puts_("\n");
        const uint64_t toff = ((const volatile uint64_t *)(STORE + mf->img_off))[1];
        const uint64_t at[3] = {toff, SBG_DTB_OFF, SBG_RD_OFF};
        const uint64_t off[3] = {mf->img_off, mf->dtb_off, mf->rd_off}, len[3] = {mf->img_size, mf->dtb_size, mf->rd_size};
        for (unsigned f = 0; f < 3; ++f) {
            const volatile uint64_t *q = (const volatile uint64_t *)(STORE + off[f]);
            volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)(SBG_QEMU_LINUX_BASE + at[f]);
            for (uint64_t i = 0; i < (len[f] + 7) / 8; ++i)
                d[i] = q[i];
        }
        if (bad || sbg_load_placed(toff, mf->img_size, mf->dtb_size, mf->rd_size) < 0 || sbg_start() < 0)
            return -1;
    }
#else
    if (sbg_load((const void *)(STORE + mf->img_off), mf->img_size, (const void *)(STORE + mf->dtb_off),
                 mf->dtb_size, (const void *)(STORE + mf->rd_off), mf->rd_size) < 0 || sbg_start() < 0)
        return -1;
#endif
    fill(0, BAR, 0x00202a44U);                 /* Starbite's status bar */
    fill(ph - BAR, BAR, 0x00202a44U);          /* its navigation bar */
    drawn_seq = 0;
    rows_copied = frames_drawn = 0;
    const volatile uint32_t *px = 0;
    uint32_t stride = 0;
    uint32_t seq = 0, tstep = 0, touch_seq = 0, frames = 0, typed = 0, last_pub = 0;
    uint64_t t_pub = 0;
    term_ok_seen = 0;
    blktest_done = blktest_fail = 0;
    uint64_t last_comp = 0, t_touch = 0, t_load = now_ms();
    int touch_ok = 0;
    const struct sbg_stats *s = sbg_stats();
    volatile struct sbshm_hdr *h = sbg_hdr();
    for (;;) {
        vt_arm(frq / 500);
        __asm__ volatile("wfi");
        irq_poll();
        uint64_t t = now_ms();
        own_check(t);
#ifdef SBSIM_PUMP
        /* cp7m: Starbite OS's GPU-wait hook (guest_pump -> sbg_pump, one block read <= 64 KiB per call) every 2 ms,
         * the full poll only every 40 ms (a long frame's turn): the block service mostly through the pump */
        {
            static uint32_t it, pumped;
            pumped += sbg_pump(SBSHM_BLK_SLOT / 512U) > 0;
            if (++it % 20) {
                if (it % 5000 == 1) {
                    puts_("sbsim: pump: block reads served by sbg_pump so far ");
                    putu(pumped);
                    puts_("\n");
                }
                continue;
            }
        }
#endif
        sbg_poll();
        if (mf->flags & F_V3)
            sim_v3_tick(t);
        s = sbg_stats();
        if (s->state == SBG_FAILED)
            return -1;
        if (t - t_load >= 5000) {           /* hdr.guest_load (sbguest.ko, every 5 s): what guest.c's idle stop reads */
            t_load = t;
            puts_("sbsim: guest load ");
            putu(h->guest_load);
            puts_(" %\n");
        }
        if (s->state == SBG_OFF)
            break;
        if (t - last_comp >= 100 && sbg_frame(&seq, &px, &stride)) {
            last_comp = t;
            composite(px, stride, seq);
            frames++;
        }
        if (mf->flags & F_BLKTEST) {        /* the block tests run in the guest; its log says when they are done */
            if (blktest_done && tstep == 0) {
                puts_(blktest_fail ? "sbsim: the guest's block tests are done, with a FAIL line\n"
                                   : "sbsim: the guest's block tests are done\n");
                tstep = 5;
                sbg_stop();
            }
            continue;
        }
        if (mf->flags & F_POPUP) {          /* L2: popuptest's window (blue) and its popup (yellow) composited */
            if (s->state == SBG_RUNNING && !t_touch)
                t_touch = t;
            if (tstep == 0 && t_touch && px && seq == h->surf_seq) {
                uint32_t pop = panel_px(150, BAR + 150) & 0xffffffU, win = panel_px(500, BAR + 500) & 0xffffffU;
                if ((pop == 0xffd700U && win == 0x2040c0U) || t - t_touch > 20000) {
                    puts_("sbsim: the popup check: at the popup ");
                    putx(pop);
                    puts_(", in the window ");
                    putx(win);
                    touch_ok = pop == 0xffd700U && win == 0x2040c0U;
                    puts_(touch_ok ? " (popup over its window: PASS)\n" : " (NOT as expected)\n");
                    char name[] = "sbsim-term-run0.ppm";
                    name[14] = (char)('0' + run % 10);
                    write_ppm(name);
                    tstep = 5;
                    sbg_stop();
                }
            }
            continue;
        }
        if (mf->flags & F_TERM) {           /* the terminal: type a command, the shell runs it */
#ifdef SBSIM_TERM_STRESS
            /* the hard power-off test (cp7 #1): a writer that never stops (files made, rewritten, renamed, no
             * sync), started before the marker; the test kills QEMU mid-write and cold-starts on the same overlay */
            static const char cmd[] = "(i=0; while :; do i=$((i+1)); dd if=/dev/urandom of=$HOME/w$((i%24)) bs=64k "
                                      "count=16 2>/dev/null; cp /etc/services $HOME/s.tmp && mv $HOME/s.tmp "
                                      "$HOME/s$((i%50)); echo $i > $HOME/n; done) >/dev/null 2>&1 & "
                                      "echo STARBITE-TERM-OK > /dev/kmsg\n";
#elif defined(SBSIM_TERM_MEM)
            /* instant start (2026-11-13): the guest's memory with foot, then with a GTK app (the snapshot's size) */
            static const char cmd[] = "m() { grep -E 'MemTotal|MemFree|MemAvail|^Cached|AnonPages|Shmem:|Slab|KernelStack"
                                      "|PageTables|Mapped' /proc/meminfo | while read l; do echo \"MEM$1 $l\" > /dev/kmsg;"
                                      " done; }; m 0; (gnome-calculator >/dev/null 2>&1 &); sleep 9; m 1; "
                                      "echo STARBITE-TERM-OK > /dev/kmsg\n";
#else
            static const char cmd[] = "echo STARBITE-TERM-OK > /dev/kmsg\n";
#endif
            if (s->state == SBG_RUNNING && frames >= 1 && tstep == 0 && t_touch && t - t_touch > 2000) {
                tstep = 1;
                typed = 0;
                touch_seq = h->surf_seq;        /* any frame after the typing began shows the result */
            } else if (tstep == 0 && s->state == SBG_RUNNING && !t_touch) {
                t_touch = t;                    /* the shell needs a moment after HELLO */
            } else if (tstep == 1 && t - t_touch >= 20) {
                t_touch = t;
                if (cmd[typed]) {
                    int sh;
                    unsigned k = sbshm_ascii_key(cmd[typed++], &sh);
                    if (sh)
                        sbg_key(SBSHM_KEY_LEFTSHIFT, 1);
                    sbg_key(k, 1);
                    sbg_key(k, 0);
                    if (sh)
                        sbg_key(SBSHM_KEY_LEFTSHIFT, 0);
                } else {
                    puts_("sbsim: typed the command\n");
                    tstep = 2;
                }
            } else if (tstep == 2 && h->surf_seq != last_pub) {
                last_pub = h->surf_seq;         /* the live screen (fbcon) still changing */
                t_pub = t;
            } else if (tstep == 2 && term_ok_seen && px && h->surf_seq != touch_seq && seq == h->surf_seq &&
                       t - t_pub > 300) {       /* still for 300 ms: fbcon idle, guestd's last look published */
                uint32_t bad = verify_panel(px, stride), lit = 0;
                for (uint32_t y = BAR; y < ph - BAR; ++y)
                    for (uint32_t x = 0; x < pw; x += 4)
                        lit += (panel_px(x, y) & 0xffffffU) != 0;
                puts_("sbsim: the shell ran the typed command (STARBITE-TERM-OK in the guest's log); panel against the guest's frame: ");
                putu(bad);
                puts_(" rows differ, lit pixels (1 in 4) ");
                putu(lit);
                puts_("\n");
                touch_ok = !bad && lit > 100;
                if (bad)
                    damage_bad = 1;
                char name[] = "sbsim-term-run0.ppm";
                name[14] = (char)('0' + run % 10);
                write_ppm(name);
                /* a tap in the middle of the window (the guest logs where it went), then the stop */
                sbg_touch(0, SBG_TOUCH_DOWN, (int32_t)mf->surf_w / 2, (int32_t)mf->surf_h / 2);
                sbg_touch(0, SBG_TOUCH_UP, (int32_t)mf->surf_w / 2, (int32_t)mf->surf_h / 2);
                tstep = 4;
                t_touch = t;
            } else if (tstep == 4 && (h->touches_seen >= 2 || t - t_touch > 2000)) {
                puts_("sbsim: the tap: touches seen by the guest ");
                putu(h->touches_seen);
                puts_("\n");
                tstep = 5;
                if (mf->flags & F_FREEZE) {
                    tstep = 10;
                    t_touch = t;
                    frz_ok = 0;
                    frz_cycles = 0;
                    continue;
                }
#ifdef SBSIM_TERM_STRESS
                puts_("sbsim: stress: the writer runs on; no stop (the test kills QEMU: a hard power-off)\n");
                tstep = 6;
#else
                sbg_stop();
#endif
            } else if (tstep == 10) {          /* F_FREEZE: ask the freeze */
                int r = sbg_freeze(1);
                puts_(r > 0 ? "sbsim: freeze asked\n" : "sbsim: the guest cannot freeze (no SBSHM_CAP_FREEZE)\n");
                frz_logged = thaw_logged = 0;
                t_touch = t;
                tstep = r > 0 ? 11 : 5;
                if (r <= 0)
                    sbg_stop();
            } else if (tstep == 11 && sbg_frozen()) {
                puts_("sbsim: frozen ");
                putu(t - t_touch);
                puts_(" ms after the ask\n");
                t_touch = t;
                tstep = 12;
            } else if (tstep == 12 && t - t_touch >= 1000) {   /* (the freeze's own sync is over by now) */
                frz_db0 = st.db_from_guest;
                t_touch = t;
                tstep = 15;
            } else if (tstep == 15 && t - t_touch >= 2000) {
                puts_("sbsim: frozen, then 2 s quiet? doorbells from the guest ");
                putu(st.db_from_guest - frz_db0);
                puts_(", its log quiet ");
                putu(!thaw_logged);
                puts_("; thaw asked\n");
                frz_quiet = st.db_from_guest - frz_db0;
                (void)sbg_freeze(0);
                t_touch = t;
                tstep = 13;
            } else if (tstep == 13 && !sbg_frozen() && thaw_logged) {
                puts_("sbsim: thawed ");
                putu(t - t_touch);
                puts_(" ms after the ask (CPUs back)\n");
                if (frz_quiet <= 8)                 /* (the kernel answering a packet from the doorbell: no thaw) */
                    frz_ok++;
                if (++frz_cycles < 2) {
                    tstep = 10;
                } else {
                    touch0 = h->touches_seen;
                    sbg_touch(0, SBG_TOUCH_DOWN, (int32_t)mf->surf_w / 2, (int32_t)mf->surf_h / 2);
                    sbg_touch(0, SBG_TOUCH_UP, (int32_t)mf->surf_w / 2, (int32_t)mf->surf_h / 2);
                    t_touch = t;
                    tstep = 14;
                }
            } else if (tstep == 14 && (h->touches_seen >= touch0 + 2 || t - t_touch > 5000)) {
                puts_("sbsim: after the thaws a tap reached sbwl: ");
                putu(h->touches_seen >= touch0 + 2);
                puts_("; freeze cycles quiet ");
                putu(frz_ok);
                puts_("/2\n");
                if (h->touches_seen < touch0 + 2 || frz_ok < 2)
                    damage_bad = 1;          /* (the run fails) */
                tstep = 5;
                sbg_stop();
            } else if (((tstep >= 11 && tstep <= 13) || tstep == 15) && t - t_touch > 20000) {
                puts_("sbsim: FREEZE STEP TIMED OUT at step ");
                putu(tstep);
                puts_("\n");
                damage_bad = 1;
                tstep = 5;
                sbg_stop();
            } else if (tstep == 2 && t - t_touch > 20000) {
                puts_("sbsim: no STARBITE-TERM-OK from the guest's shell in 20 s\n");
                tstep = 5;
                sbg_stop();
            }
            continue;
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
        } else if (tstep == 4 && px && h->surf_seq - touch_seq >= 2 && seq == h->surf_seq) {
            uint32_t bad = verify_panel(px, stride);
            puts_("sbsim: the panel against the guest's frame ");
            putu(seq);
            puts_(": ");
            putu(bad);
            puts_(bad ? " rows DIFFER\n" : " rows differ (exact)\n");
            if (bad)
                damage_bad = 1;
            uint32_t c = panel_px(mf->touch_x, BAR + mf->touch_y) & 0xffffffU;
            touch_ok = (c == 0xffd700U);
            puts_("sbsim: touch reached the app: pixel at the tap ");
            putx(c);
            puts_(touch_ok ? " (the app's touch mark)\n" : " (NOT the touch mark)\n");
            tstep = 5;
            char name[] = "sbsim-panel-run0.ppm";
            name[15] = (char)('0' + run % 10);
            write_ppm(name);
            t_touch = t;
            if (run == 1)
                sbg_send(SBG_MSG_LOAD, 4, 2);       /* run 1: stop while four load threads spin */
            else
                sbg_stop();
        } else if (tstep == 5 && run == 1 && t - t_touch > 1000) {
            tstep = 6;
            puts_("sbsim: MSG_STOP during the guest's load\n");
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
    puts_(", rows copied per frame ");
    putu(frames_drawn ? rows_copied / frames_drawn : 0);
    puts_(sbg_rows() ? " (row table)" : " (whole surface)");
    puts_("\n");
    if (blk_n)
        blk_report(run);
    if (mf->flags & F_BLKTEST)
        return (blktest_done && !blktest_fail) ? 0 : -1;
    return (s->hello && frames >= 3 && touch_ok && !damage_bad) ? 0 : -1;
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
    blk_open();
    struct sbg_cfg cfg = {
        .linux_base = SBG_QEMU_LINUX_BASE, .linux_size = SBG_QEMU_LINUX_SIZE,
        .shm_base = SBG_QEMU_SHM_BASE, .shm_size = SBG_QEMU_SHM_SIZE,
        .gicd = GICD, .first_mpidr = 4, .tramp_pa = SBG_QEMU_TRAMP,
        .surf_w = mf->surf_w, .surf_h = mf->surf_h,
        .db_guest = mf->db_guest, .db_host = mf->db_host,
        .host_lines = {INTID_OWN, 0},
        .repair = (mf->flags & F_REPAIR) ? 1U : 0U,
        .boot_timeout_ms = (mf->boot_timeout_s ? mf->boot_timeout_s : 300) * 1000U,
        .boot_warn_ms = SBSIM_BOOT_WARN_MS,         /* -DSBSIM_BOOT_WARN_MS=N: the slow-start note (sbghost) */
        .stop_timeout_ms = 60000,
        .v2 = blk_n ? 1U : 0U,                  /* contract v2 only with block devices: the rest stays v1 */
        .services = (blk_n && (mf->flags & F_V3)) ? 0xffffffffU : 0U,   /* v3: every service sim_v3.c stands in for */
    };
    for (unsigned i = 0; i < SBSHM_BLK_DEVS; ++i)
        cfg.blk[i] = blkdevs[i];
    sbg_init(blk_n ? &shim_blk : &shim, &cfg, me);
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
    int ok = 1, failed_any = 0;
    for (unsigned r = 0; r < runs && ok; ++r) {
        if (run_guest(r) < 0) {
            int off = cluster_off();
            puts_("sbsim: run ");
            putu(r);
            puts_(off ? " failed; the M3 cluster is off\n" : " failed; the M3 cluster is still on\n");
            if (!(mf->flags & F_RETRY) || !off || r + 1 == runs)
                ok = 0;
            else
                ok = 1, failed_any = 1;
        } else
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
    if (failed_any)
        ok = 0;
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
