/* SPDX-License-Identifier: GPL-2.0-only
 * guest-probe P2 (docs/STARBITE-OS-LINUX-M3.md 10): Linux on the M3 cluster on the phone, without Starbite OS.
 * A bare libsb payload with the guest inside (Image, DTB, initrd: p2blob.S), driven by sbghost exactly as sbsim
 * drives it on QEMU. Unattended, about 30 s plus the guest's boot, then a restart into recovery (pull pmsg).
 *
 *   1. the GIC reset as Starbite OS does; sbghost: the guest loaded into the carve-out, the M3 vendor PMU
 *      defaults, CPU_ON(0x100) through the trampoline;
 *   2. the guest's kernel log into pmsg, filtered ("linux| ", at most LOG_MAX lines: pmsg holds 16 KB); on a
 *      failure the last lines of the guest's log follow;
 *   3. the guest surface (720x1440) on the panel at 2x between Starbite-coloured bars (1440x2960 exactly);
 *   4. after a few frames: a scripted tap / drag / key; the touch mark must appear in the guest's own pixels;
 *   5. 8 s of the app on screen, then MSG_STOP; the M3 cluster must power off (PMU, sb_m3_on).
 * The watchdog (catalog wdt, fed here every 50 ms) covers a hang of the A55; an M3 hang is reported, and the
 * cluster stays on until the restart this image does anyway.
 */
#include "sb.h"
#include "../sbshm.h"
#include "../host/sbghost.h"

#ifndef P2_DB_GUEST
#define P2_DB_GUEST 0
#endif
#ifndef P2_DB_HOST
#define P2_DB_HOST 0
#endif
#define GICD SBG_S9_GICD
#define GICC SBG_S9_GICC
#define WDT1 0x10060000UL
#define SW 720U
#define SH 1440U
#define BAR 40U                                     /* (2960 - 2 * 1440) / 2 */
#define LOG_MAX 60U

extern const char p2_image[], p2_image_end[], p2_dtb[], p2_dtb_end[], p2_rd[], p2_rd_end[];

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void feed(void)
{
    volatile uint32_t *w = (volatile uint32_t *)WDT1;
    if (w[0] & (1U << 5))
        w[2] = w[1];
}

static struct ub u;
static void L(void) { sb_log(u.b); ub_init(&u); ub_s(&u, "gp2: "); }

/* ---- the shim ------------------------------------------------------------------------------------- */
static uint64_t ms(void) { return sb_ms(); }
static void host_log(const char *l) { ub_s(&u, l); L(); }

static int has(const char *s, const char *w)
{
    for (; *s; ++s) {
        unsigned i = 0;
        while (w[i] && s[i] == w[i])
            ++i;
        if (!w[i])
            return 1;
    }
    return 0;
}
static char tail[48][120];
static unsigned tail_n, fwd, seen;
static void guest_log(const char *l)
{
    sb_strcpy(tail[tail_n % 48], sizeof(tail[0]), l);
    ++tail_n;
    ++seen;
    static const char *const keep[] = {"guestd", "sbguest", "guest-init", "GIC", "psci", "smp:", "SMP:", "panic",
                                       "Oops", "BUG", "Unable", "rror", "ail", "Kernel command", "Run /init",
                                       "Memory:", "Machine model", "arch_timer", "Booted secondary", "reboot"};
    int k = seen <= 3;
    for (unsigned i = 0; !k && i < sizeof(keep) / sizeof(keep[0]); ++i)
        k = has(l, keep[i]);
    if (!k || fwd >= LOG_MAX)
        return;
    ++fwd;
    ub_s(&u, "linux| ");
    ub_s(&u, l);
    L();
}
static int64_t cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx)
{
    return (int32_t)sb_psci(SB_PSCI_CPU_ON, mpidr, entry, ctx);
}
static int cluster_off(void) { return sb_m3_on() ? 0 : 1; }
static int m3_prep(void) { return sb_m3_pmu_defaults() ? 1 : 0; }
static const struct sbg_os shim = {
    .ms = ms, .log = host_log, .guest_log = guest_log, .cpu_on = cpu_on, .cluster_off = cluster_off,
    .m3_prep = m3_prep,
};

/* ---- the panel ------------------------------------------------------------------------------------- */
static void bars(void)
{
    for (uint32_t y = 0; y < SB_FB_H; ++y) {
        if (y == BAR)
            y = SB_FB_H - BAR;
        volatile uint64_t *d = (volatile uint64_t *)(SB_FB + (uintptr_t)y * SB_FB_W * 4);
        for (uint32_t x = 0; x < SB_FB_W / 2; ++x)
            d[x] = 0x00202a4400202a44ULL;
    }
}
/* the guest's front buffer at 2x: each source pixel becomes 2x2 */
static void composite(const volatile uint32_t *px, uint32_t stride)
{
    for (uint32_t y = 0; y < SH; ++y) {
        const volatile uint32_t *s = (const volatile uint32_t *)((uintptr_t)px + (uintptr_t)y * stride);
        volatile uint64_t *d0 = (volatile uint64_t *)(SB_FB + (uintptr_t)(BAR + 2 * y) * SB_FB_W * 4);
        volatile uint64_t *d1 = (volatile uint64_t *)(SB_FB + (uintptr_t)(BAR + 2 * y + 1) * SB_FB_W * 4);
        for (uint32_t x = 0; x < SW; ++x) {
            uint64_t c = s[x];
            c |= c << 32;
            d0[x] = c;
            d1[x] = c;
        }
        if (!(y & 255))
            feed();
    }
}

static void wait_ms(unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (!(i % 50))
            feed();
        sb_mdelay(1);
    }
}

void sb_main(void)
{
    ub_init(&u);
    ub_s(&u, "gp2: ");
    ub_s(&u, "guest-probe P2 (Linux on the M3 cluster, no Starbite OS): EL");
    ub_dec(&u, (sb_boot.el >> 2) & 3U);
    ub_s(&u, ", guest Image ");
    ub_dec(&u, (uint64_t)(p2_image_end - p2_image));
    ub_s(&u, " B, DTB ");
    ub_dec(&u, (uint64_t)(p2_dtb_end - p2_dtb));
    ub_s(&u, " B, initrd ");
    ub_dec(&u, (uint64_t)(p2_rd_end - p2_rd));
    ub_s(&u, " B, doorbells ");
    ub_dec(&u, P2_DB_GUEST);
    ub_c(&u, '/');
    ub_dec(&u, P2_DB_HOST);
    ub_s(&u, ", watchdog ");
    ub_s(&u, (rd(WDT1) & (1U << 5)) ? "armed" : "NOT armed");
    ub_s(&u, ", M3 cluster ");
    ub_s(&u, sb_m3_on() ? "powered" : "off");
    L();
    feed();

    /* the GIC as Starbite OS leaves it (kern/hw.c gic_reset) */
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
    wr(GICD, 1);
    wr(GICC + 0x04, 0xffU);
    wr(GICC + 0x00, 1);
    uint8_t me = *(volatile uint8_t *)(GICD + 0x800);

    struct sbg_cfg cfg = {
        .linux_base = SBG_S9_LINUX_BASE, .linux_size = SBG_S9_LINUX_SIZE,
        .shm_base = SBG_S9_SHM_BASE, .shm_size = SBG_S9_SHM_SIZE,
        .gicd = GICD, .first_mpidr = 0x100,
        .tramp_pa = SB_TEST_BASE,                   /* below 4 GiB, non-cacheable here (P1: a high entry is lost) */
        .surf_w = SW, .surf_h = SH,
        .db_guest = P2_DB_GUEST, .db_host = P2_DB_HOST,
        .host_lines = {0},
        .repair = 1,
        .boot_timeout_ms = 60000, .stop_timeout_ms = 10000,
    };
    sbg_init(&shim, &cfg, me);
    sbg_gic_prepare();
    uint64_t t_load = sb_ms();
    int r = sbg_load(p2_image, (uint64_t)(p2_image_end - p2_image), p2_dtb, (uint64_t)(p2_dtb_end - p2_dtb),
                     p2_rd, (uint64_t)(p2_rd_end - p2_rd));
    feed();
    ub_s(&u, "loaded into the carve-out in ");
    ub_dec(&u, sb_ms() - t_load);
    ub_s(&u, " ms");
    L();
    if (r == 0) {
        bars();
        r = sbg_start();
    }
    const struct sbg_stats *s = sbg_stats();
    volatile struct sbshm_hdr *h = sbg_hdr();
    uint32_t seq = 0, tstep = 0, touch_seq = 0, frames = 0, stride = 0;
    const volatile uint32_t *px = 0;
    uint64_t last_comp = 0, t_touch = 0, t_run = 0;
    int touch_ok = 0;
    while (r == 0) {
        wait_ms(5);
        uint64_t t = sb_ms();
        sbg_poll();
        s = sbg_stats();
        if (s->state == SBG_FAILED || s->state == SBG_OFF)
            break;
        if (t - last_comp >= 200 && sbg_frame(&seq, &px, &stride)) {
            last_comp = t;
            composite(px, stride);
            frames++;
        }
        if (s->state == SBG_RUNNING && frames >= 3 && tstep == 0) {
            sbg_touch(0, SBG_TOUCH_DOWN, 360, 840);
            tstep = 1;
            t_touch = t;
        } else if (tstep == 1 && t - t_touch > 150) {
            sbg_touch(0, SBG_TOUCH_MOVE, 420, 840);
            tstep = 2;
        } else if (tstep == 2 && t - t_touch > 300) {
            sbg_touch(0, SBG_TOUCH_UP, 420, 840);
            sbg_key(158, 1);
            sbg_key(158, 0);
            tstep = 3;
        } else if (tstep == 3 && h->touches_seen >= 3) {
            tstep = 4;
            touch_seq = h->surf_seq;
        } else if (tstep == 4 && h->surf_seq - touch_seq >= 2) {
            sbshm_dmb();
            const volatile uint32_t *fr = (const volatile uint32_t *)(uintptr_t)(SBG_S9_SHM_BASE +
                                                                                 h->surf_off[h->surf_front & 1U]);
            uint32_t c = fr[840U * (h->surf_stride / 4) + 360U] & 0xffffffU;  /* the newest front buffer */
            touch_ok = c == 0xffd700U;
            ub_s(&u, "touch reached the app: pixel at the tap ");
            ub_hex(&u, c, 6);
            ub_s(&u, touch_ok ? " (its touch mark)" : " (NOT the touch mark)");
            L();
            tstep = 5;
            t_run = t;
        } else if (tstep == 5 && t - t_run > 8000) {
            tstep = 6;
            sbg_stop();
        }
    }
    s = sbg_stats();
    ub_s(&u, "guest: HELLO ");
    ub_dec(&u, s->hello);
    ub_s(&u, " after ");
    ub_dec(&u, s->t_hello);
    ub_s(&u, " ms, CPUs ");
    ub_dec(&u, s->guest_ncpus);
    ub_s(&u, ", interfaces ");
    ub_hex(&u, s->guest_cpuif, 2);
    ub_s(&u, ", first frame ");
    ub_dec(&u, s->t_first_frame);
    ub_s(&u, " ms, frames shown ");
    ub_dec(&u, frames);
    ub_s(&u, ", guest heartbeats ");
    ub_dec(&u, h->guest_heartbeat);
    ub_s(&u, ", log lines ");
    ub_dec(&u, seen);
    L();
    if (s->state != SBG_OFF) {
        ub_s(&u, "the guest's last log lines:");
        L();
        unsigned n = tail_n < 48 ? tail_n : 48;
        for (unsigned i = tail_n - n; i < tail_n && i < tail_n - n + 30; ++i) {
            ub_s(&u, "linux| ");
            ub_s(&u, tail[i % 48]);
            L();
        }
    }
    wait_ms(200);
    int off = !sb_m3_on();
    int pass = r == 0 && s->state == SBG_OFF && s->hello && frames >= 5 && touch_ok && off;
    ub_s(&u, "RESULT ");
    ub_s(&u, pass ? "PASS" : "FAIL");
    ub_s(&u, ": Linux on the M3s ");
    ub_s(&u, s->hello ? "ran" : "did NOT say HELLO");
    ub_s(&u, ", touch ");
    ub_s(&u, touch_ok ? "ok" : "no");
    ub_s(&u, ", stop: cluster ");
    ub_s(&u, off ? "off" : "STILL POWERED");
    if (s->state == SBG_OFF) {
        ub_s(&u, " after ");
        ub_dec(&u, s->t_off);
        ub_s(&u, " ms");
    }
    if (s->why) {
        ub_s(&u, ", why: ");
        ub_s(&u, s->why);
    }
    ub_s(&u, "; restarting into recovery in 5 s");
    L();
    wait_ms(5000);
    sb_reboot(SB_INFORM_RECOVERY);
}
