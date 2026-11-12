/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 0.9 / P3: the guest service, Linux apps on the M3 cluster (docs/STARBITE-OS-LINUX-M3.md).
 * The sbos-linux session's drop-in, wired into Starbite OS 1.2-alpha1 by the starbite-os session
 * (starbite/guest/sbos/INTEGRATION.md lists the hooks; docs/STARBITE-OS.md 12.22). Everything the guest needs
 * from the hardware goes through sbghost (starbite/guest/host), the core the phone payloads P2..P2d ran: the
 * carve-out, the trampoline below 4 GiB, CPU_ON, the shared header, the rings, the log zone.
 *
 * Life: off until the user opens Linux (the M3 cluster powered down) -> guest_start: the store's three files
 * into the bounce window below 4 GiB (store_load, CRC checked), then word copies into the carve-out (sbg_load:
 * the copy path P2..P2d ran; UFS DMA above 4 GiB never ran on the phone), the M3 words back to the boot state,
 * the vendor PMU defaults, cluster/core on, CPU_ON(0x100) -> the poll timer (10 ms while starting or shown,
 * 50 ms in the background) -> guest_stop: MSG_STOP, the guest powers its cores and the cluster off itself (P2c)
 * -> off. Hidden for 60 s or the screen off for 60 s: stopped (power). The heater and the guest exclude each
 * other (both own the M3s). guest_start only checks and claims the big cores; the load runs at the guest timer's first
 * tick (after the caller drew "Starting Linux..."), blocking the loop for as long as UFS takes for about 28 MiB
 * (logged: "read in N ms, copied in M ms"); the watchdog is fed around it.
 *
 * Builds: the phone (SBOS_KERNEL), the QEMU run of this file (GUEST_QEMU: starbite/guest/sbsim/kern-shim.c
 * gives the kernel calls; QEMU virt's addresses), the host preview / tests (SB_HOST: a fake guest, no sbghost).
 */
#include "sb.h"
#include "store.h"
#include "kern.h"
#include "kinput.h"
#include "guest.h"
#ifndef SB_HOST
#include "../../guest/host/sbghost.h"
#else
#include <stdlib.h>
#endif

#ifdef GUEST_QEMU
#ifndef GUEST_DB_GUEST
#define GUEST_DB_GUEST 132U                 /* run-qemu.sh: SPIs 100 / 101 */
#endif
#ifndef GUEST_DB_HOST
#define GUEST_DB_HOST 133U
#endif
#define G_LINUX_BASE SBG_QEMU_LINUX_BASE
#define G_LINUX_SIZE SBG_QEMU_LINUX_SIZE
#define G_SHM_BASE SBG_QEMU_SHM_BASE
#define G_SHM_SIZE SBG_QEMU_SHM_SIZE
#define G_GICD SBG_QEMU_GICD
#define G_TRAMP SBG_QEMU_TRAMP
#define G_FIRST_MPIDR 4U
#define G_BOUNCE 0x50000000UL               /* QEMU RAM between the store (0x4800_0000) and the carve-out */
#define G_BOUNCE_END 0x7f000000UL
#else
#define GUEST_DB_GUEST 511U                 /* P1: the doorbells (idle, non-secure, both directions, P1c) */
#define GUEST_DB_HOST 510U
#define G_LINUX_BASE SBG_S9_LINUX_BASE
#define G_LINUX_SIZE SBG_S9_LINUX_SIZE
#define G_SHM_BASE SBG_S9_SHM_BASE
#define G_SHM_SIZE SBG_S9_SHM_SIZE
#define G_GICD SBG_S9_GICD
#define G_TRAMP SBG_S9_TRAMP
#define G_FIRST_MPIDR 0x100U
/* libsb's non-cacheable RAM-test window (sb.h SB_TEST_BASE..+SB_TEST_SIZE), which Starbite OS never uses (Engineer
 * OS's memory stress does, in its own boots); its first MiB keeps the CPU_ON trampolines (0xa010_0000..0xa010_007f) */
#define G_BOUNCE (SB_TEST_BASE + 0x100000UL)
#define G_BOUNCE_END 0xa6000000UL          /* 1.2-alpha7: below the radio's DMA (wifi/sb 0xa6000000..0xa6800000) */
#endif
#define SW 720U
#define SH 1440U
#define VIEW_Y 40U                          /* the view below a 40 px bar, at 2x */
#define IDLE_STOP_MS 60000U

static int st = GUEST_OFF, shown, screen = 1;
static const char *why = "";
/* cp7 #1: what a start is doing, in the user's words ("" = the usual "opens in a few seconds") */
static const char *stage = "";
static uint64_t t_go;                       /* this start's CPU_ON (0: not yet) */
#define GUEST_SLOW_MS 30000U                /* "taking longer than usual" from here (the host waits 90 s) */
#define GUEST_FSCK_EXTEND_MS 180000U        /* the disk check: the host waits this much longer for HELLO */
static uint32_t seq_seen = 0, seq_blit = 0;
static uint32_t blits, blit_ms_max, rows_copied;   /* the view's frames drawn, its slowest copy, rows copied */
static uint64_t t_hidden, t_screen_off;
/* Linux busy (cp3 2026-11-12: a build and installs broke when the phone slept): the last time its CPU load
 * (hdr.guest_load, sbguest.ko) was >= GUEST_BUSY_LOAD % of a core or its block traffic >= GUEST_BUSY_KIB in a 5 s
 * window. The idle stops wait while it was busy in the last IDLE_STOP_MS; with the screen off at most
 * GUEST_BUSY_SCREEN_OFF_MS (a job that never ends must not hold the M3 cluster all night) */
#define GUEST_BUSY_LOAD 10U
#define GUEST_BUSY_KIB 256U
#define GUEST_BUSY_SCREEN_OFF_MS (3U * 60U * 60U * 1000U)
static uint64_t t_busy;
static int busy_said;
static int sb_up;
static int kept;                            /* L3: guest_keep: no idle stop */
static int apps;                            /* v3: guest_apps: Linux apps with a switcher card */
/* 1.2-alpha3: two guests from the same service: the app demo (the S9 Linux, 720 x 1440 at 2x) and the terminal
 * (the guest kernel of its own, a shell on fbcon in Terminus 16 x 32, 720 x 1060 at 2x above Starbite's keyboard) */
static int mode = GUEST_APP, term_ok;
static const char *const names[3][3] __attribute__((unused)) = {
    {GUEST_STORE_IMAGE, GUEST_STORE_DTB, GUEST_STORE_RD},
    {GUEST_TERM_IMAGE, GUEST_TERM_DTB, GUEST_TERM_RD},
    {GUEST_WL_IMAGE, GUEST_WL_DTB, GUEST_WL_RD},
};
static unsigned surf_w(void) { return mode == GUEST_TERM ? GUEST_TERM_W : mode == GUEST_WL ? GUEST_WL_W : SW; }
static unsigned surf_h(void) { return mode == GUEST_TERM ? GUEST_TERM_H : mode == GUEST_WL ? GUEST_WL_H : SH; }
int guest_mode(void) { return mode; }
int guest_scale(void) { return 2; }      /* (the terminal: a 16 x 32 font instead of 8 x 16 at 4x) */
int guest_term_ok(void) { return term_ok; }
void guest_full_next(void) { seq_blit = 0; }     /* the next blit copies every row */

static void arm(uint32_t period)
{
    kt_cancel(KT_GUEST, KT_GUEST_ID_POLL);
    kt_set(KT_GUEST, KT_GUEST_ID_POLL, sb_ms(), period, 1);
}

int guest_state(void) { return st; }
const char *guest_why(void) { return why; }
const char *guest_stage(void)
{
    if (st != GUEST_STARTING)
        return "";
    if (stage[0])
        return stage;
    if (t_go && sb_ms() - t_go > GUEST_SLOW_MS)
        return "Linux is taking longer than usual to start.";
    return "";
}
unsigned guest_db_host(void) { return GUEST_DB_HOST; }
int guest_shown(void) { return shown && st == GUEST_RUNNING; }
void guest_keep(int on)
{
    if (kept && !on && !shown)
        t_hidden = sb_ms();                 /* the 60 s from now (an install that ran for minutes) */
    kept = on ? 1 : 0;
}
void guest_apps(int n)
{
    if (apps && n <= 0 && !shown)
        t_hidden = sb_ms();                 /* the last card gone: the 60 s from now */
    apps = n > 0 ? n : 0;
}
/* the idle stop (guest_timer, real and fake): hidden 60 s, or the screen off 60 s; with Linux apps on cards (v3) not for
 * being hidden, and the screen off 30 min; never while kept (the App Store, a package operation) */
static int idle_stop_due(uint64_t now)
{
    if (kept)
        return 0;
    uint64_t off = apps ? GUEST_APPS_SCREEN_OFF_MS : IDLE_STOP_MS;
    int busy = t_busy && now - t_busy < IDLE_STOP_MS;
    if (busy && (screen || !t_screen_off || now - t_screen_off < GUEST_BUSY_SCREEN_OFF_MS)) {
        if (!busy_said && ((!apps && !shown && t_hidden && now - t_hidden > IDLE_STOP_MS) ||
                           (!screen && t_screen_off && now - t_screen_off > off))) {
            busy_said = 1;                  /* once per busy stretch: the idle stop waits */
            sb_log("guest: Linux is busy (a job runs): the idle stop waits");
        }
        return 0;
    }
    busy_said = 0;
    return (!apps && !shown && t_hidden && now - t_hidden > IDLE_STOP_MS) ||
           (!screen && t_screen_off && now - t_screen_off > off);
}

#ifndef SB_HOST
/* ===================================== the guest (phone, QEMU) ===================================== */
static uint64_t t_busy_win, busy_sectors;    /* the busy sample's window (idle_stop_due) */
static uint64_t ms(void) { return sb_ms(); }
static void host_log(const char *l)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "guest: ");
    ub_s(&u, l + (sb_prefix(l, "guest: ") ? 7 : 0));
    sb_log(u.b);
}
/* the guest kernel's log: only what matters goes to pmsg (it holds 16 KB) */
static void guest_log(const char *l)
{
    static const char *const keep[] = {"guestd", "sbguest", "guest-init", "panic", "Oops", "BUG", "Unable",
                                       "failed", "error", "smp: Brought up", "STARBITE"};
    int has_echo = 0;
    for (const char *s = l; *s; ++s)
        has_echo |= sb_prefix(s, "echo");
    for (const char *s = l; *s && !has_echo; ++s)
        if (sb_prefix(s, "STARBITE-TERM-OK"))
            term_ok = 1;                    /* the typed command ran (its kmsg line, not the command's echo) */
    for (const char *s = l; *s; ++s)
        if (sb_prefix(s, "guest-init: desktop: overlay check: ")) {
            s += 36;                        /* initramfs/init's fsck_overlay: "started", then "e2fsck RC (...)" */
            if (sb_prefix(s, "started")) {
                stage = "Checking Linux's disk.";
                sbg_boot_extend(GUEST_FSCK_EXTEND_MS);   /* a long check is not a hang */
            } else if (sb_prefix(s, "e2fsck 1") || sb_prefix(s, "e2fsck 2") || sb_prefix(s, "e2fsck 3")) {
                stage = "Linux repaired its disk after an unexpected stop.";
            } else if (sb_prefix(s, "e2fsck 0")) {
                stage = "";
            } else {
                stage = "Linux's disk has errors: this start keeps nothing.";
            }
            ui_mark_dirty();
            break;
        }
    for (unsigned i = 0; i < sizeof(keep) / sizeof(keep[0]); ++i) {
        const char *k = keep[i];
        for (const char *s = l; *s; ++s) {
            unsigned j = 0;
            while (k[j] && s[j] == k[j])
                ++j;
            if (!k[j]) {
                struct ub u;
                ub_init(&u);
                ub_s(&u, "linux| ");
                ub_s(&u, l);
                sb_log(u.b);
                return;
            }
        }
    }
}
static int64_t cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx)
{
    return (int32_t)sb_psci(SB_PSCI_CPU_ON, mpidr, entry, ctx);
}
static int cluster_off(void) { return hw_m3_on() ? 0 : 1; }
/* the boot state, the vendor PMU defaults, then the vendor's hotplug-in for cluster 1's first core (libsb's
 * sb_m3_fp_*: the one FlexPMU implementation, shared with the heater and Engineer OS) */
static int m3_prep(void)
{
    (void)hw_m3_fp_reset();
    if (!hw_m3_prep())
        return 0;
    hw_m3_core_on(4, 1);
    return 1;
}
#include "guest-card.h"                     /* sbblk: the guest's block devices from files on the microSD card */
/* ---- contract v3 (Track E): what Starbite serves the Linux apps (sbshm.h "contract v3") ----
 * sbnet: the guest's IP packets go to Track G's NAT, a weak symbol until it is linked in (then NULL: no SVC_NET) */
void sbos_nat_from_guest(const uint8_t *pkt, unsigned len) __attribute__((weak));
/* Track G (kern/guestnet.c): after the start, it tells the guest about an uplink that is already up */
void guestnet_guest_started(void) __attribute__((weak));
#ifndef GUEST_QEMU                          /* (QEMU's kern-shim has no clock and no card: neither is offered) */
/* the wall clock: UTC and the user's offset (the clock settings' zone) for the guest's time */
static uint32_t unix_s(int32_t *tz_min)
{
    struct sbos_time t;
    if (!clock_now(&t) || t.utc <= 0) {
        *tz_min = 0;
        return 0;
    }
    *tz_min = t.off_min;
    return (uint32_t)t.utc;
}
/* sbp9: Starbite's files for the guest (/home/user/Starbite) through guest-fs.h's adapter over kfile_* (another
 * session's; GUEST_FS_OPS names its struct sbg_fs). HOOK: without guest-fs.h, no SVC_P9. */
#if defined(__has_include) && !defined(GUEST_QEMU)
#if __has_include("guest-fs.h")
#include "guest-fs.h"
#endif
#endif
#define GUEST_UNIX_S unix_s
#else
#define GUEST_UNIX_S 0
#endif
#ifndef GUEST_FS_OPS
#define GUEST_FS_OPS 0
#endif
/* sbsnd: Linux's sound through Track I's driver (starbite/sbos/kern/snd.h: one output stream, the main mic), when
 * the kernel has it. Single-threaded: everything runs from this service's tick, every 5 ms while sound flows. */
#if defined(__has_include) && !defined(GUEST_QEMU)
#if __has_include("snd.h")
#include "snd.h"
#define GUEST_SND 1
#endif
#endif
#ifdef GUEST_SND
static const struct sbg_snd snd_cfg = {
    .caps = SBG_SND_PLAY | SBG_SND_CAPTURE, .block_frames = 240, .latency_frames = 1536,
};
#define GUEST_SND_OPS (&snd_cfg)
static int snd_id = -1, snd_cap, snd_fast;
static uint64_t snd_cap_t;
static int16_t snd_buf[2 * 480];
/* returns 1 while sound flows (the tick must then run every 5 ms) */
static int guest_snd_tick(void)
{
    int play = 0, cap = 0;
    sbg_snd_active(&play, &cap);
    if (play && snd_id < 0) {
        snd_id = snd_open();                /* the amp powers up only now */
        if (snd_id < 0)
            snd_id = -1;
        else
            sbg_snd_set_latency(1536);
    } else if (!play && snd_id >= 0) {
        snd_close(snd_id);
        snd_id = -1;
    }
    if (snd_id >= 0) {
        unsigned n = snd_space(snd_id);
        while (n) {
            unsigned k = n < 480U ? n : 480U;
            sbg_snd_pull(snd_buf, k);
            snd_write(snd_id, snd_buf, k);
            n -= k;
        }
    }
    if (cap && !snd_cap) {
        snd_cap = snd_cap_open() == 0;
        snd_cap_t = sb_ms();
    } else if (!cap && snd_cap) {
        snd_cap_close();
        snd_cap = 0;
    }
    if (snd_cap) {                          /* the mic's mono to L/R; silence for what it does not have */
        uint64_t now = sb_ms();
        unsigned want = (unsigned)(now - snd_cap_t) * 48U;
        snd_cap_t = now;
        while (want) {
            unsigned k = want < 480U ? want : 480U;
            int16_t *mono = snd_buf + 480;  /* (the upper half: read into it, widen downwards) */
            unsigned got = snd_cap_read(mono, k);
            for (unsigned i = 0; i < k; i++) {
                int16_t v = i < got ? mono[i] : 0;
                snd_buf[2 * i] = snd_buf[2 * i + 1] = v;
            }
            sbg_snd_push(snd_buf, k);
            want -= k;
        }
    }
    return snd_id >= 0 || snd_cap;
}
static void guest_snd_off(void)
{
    if (snd_id >= 0)
        snd_close(snd_id);
    if (snd_cap)
        snd_cap_close();
    snd_id = -1;
    snd_cap = snd_fast = 0;
}
#else
#define GUEST_SND_OPS 0
static int guest_snd_tick(void) { return 0; }
static void guest_snd_off(void) {}
static int snd_fast;
#endif
static const struct sbg_os shim = {
    .ms = ms, .log = host_log, .guest_log = guest_log, .cpu_on = cpu_on, .cluster_off = cluster_off,
    .m3_prep = m3_prep,
    .blk_io = gcard_io,
    .unix_s = GUEST_UNIX_S,
    .net_rx = sbos_nat_from_guest,          /* NULL until Track G's NAT is linked */
    .fs = GUEST_FS_OPS,
    .snd = GUEST_SND_OPS,                   /* NULL until Track I's snd.h is in the kernel */
};

static uint8_t me_if;
/* sbghost's configuration for the current mode (the surface size differs); again before each load */
static void cfg_init(void)
{
    struct sbg_cfg cfg = {
        .linux_base = G_LINUX_BASE, .linux_size = G_LINUX_SIZE,
        .shm_base = G_SHM_BASE, .shm_size = G_SHM_SIZE,
        .gicd = G_GICD, .first_mpidr = G_FIRST_MPIDR, .tramp_pa = G_TRAMP,
        .surf_w = surf_w(), .surf_h = surf_h(),
        .db_guest = GUEST_DB_GUEST, .db_host = GUEST_DB_HOST,
        .host_lines = {0},                  /* the guest kernel has the shared-distributor patch */
        .repair = 0,
        /* the desktop: initramfs, the base and the overlay off the card, sbinit, sbwl (cp3 2026-11-12: 30 s was not
         * enough for a new base's first start) */
        .boot_timeout_ms = mode == GUEST_WL ? 90000 : 30000, .stop_timeout_ms = 5000,
        .boot_warn_ms = mode == GUEST_WL ? 30000 : 0,   /* a slow start says so (and the guest's last lines) */
        .v2 = mode == GUEST_WL,             /* L3: the catalog, the store and the icons (sbshm.h contract v2) */
        /* v3 (Linux apps: hdr.version 3): sbghost leaves out what has no sbg_os part (NET without the NAT, P9
         * without guest-fs.h) */
        .services = mode == GUEST_WL ? SBSHM_SVC_CLIP | SBSHM_SVC_NOTIF | SBSHM_SVC_CLOCK | SBSHM_SVC_NET |
                                           SBSHM_SVC_P9 | SBSHM_SVC_SND : 0,
    };
    sbg_init(&shim, &cfg, me_if ? me_if : 1);
}

void guest_init(void)
{
    if (!K.irq_mode) {                      /* the doorbells need the GIC: polling mode runs no guest */
        why = "Linux needs interrupts (this boot runs in polling mode)";
        return;
    }
    me_if = *(volatile uint8_t *)(uintptr_t)(G_GICD + 0x800);   /* this core's interface (banked) */
    cfg_init();
    sbg_gic_prepare();                      /* the doorbells: edge; the host's routed here and enabled */
    sb_up = 1;
}

int guest_owns_m3(void) { return st != GUEST_OFF && st != GUEST_FAILED ? 1 : hw_m3_on() && st == GUEST_FAILED; }

int guest_available_mode(int m)
{
    struct store_ent e;
    m = m == GUEST_TERM || m == GUEST_WL ? m : GUEST_APP;
    return store_find(names[m][0], &e) && store_find(names[m][1], &e) && store_find(names[m][2], &e);
}
int guest_available(void) { return guest_available_mode(GUEST_APP); }

static uintptr_t up_mib(uintptr_t a) { return (a + 0xfffffUL) & ~0xfffffUL; }

/* 1.2-alpha1: the load runs at the guest timer's first tick, not inside guest_start: the caller (the home tile)
 * draws "Starting Linux..." first, and a slow UFS read never looks like a hang */
static int loading;
static struct store_ent ld_i, ld_d, ld_r;

int guest_start(void) { return guest_start_mode(GUEST_APP); }

int guest_start_mode(int m)
{
    m = m == GUEST_TERM || m == GUEST_WL ? m : GUEST_APP;
    if (!sb_up)
        return -1;
    if (st != GUEST_OFF && st != GUEST_FAILED)
        return -1;
    if (heater_m3_busy()) {
        why = "the heater has the big cores";
        return -2;
    }
    if (st == GUEST_FAILED && hw_m3_on()) {
        why = "the last guest did not power off: restart the phone first";
        return -3;
    }
    if (!store_find(names[m][0], &ld_i) || !store_find(names[m][1], &ld_d) || !store_find(names[m][2], &ld_r)) {
        why = m == GUEST_TERM ? "the terminal's Linux files are not in the store" :
              m == GUEST_WL ? "the Linux apps' files are not in the store" : "the Linux files are not in the store";
        return -4;
    }
    why = "";
    mode = m;
    term_ok = 0;
    st = GUEST_STARTING;                    /* the big cores are the guest's from here (the heater refuses) */
    loading = 1;
    seq_seen = seq_blit = 0;
    arm(10);
    return 0;
}

/* the guest timer's first tick: the store's files into the bounce window, the copy, CPU_ON. 0, or < 0 and why */
static int load_and_start(void)
{
    t_busy = t_busy_win = busy_sectors = 0;
    busy_said = 0;
    stage = "";
    t_go = 0;
    cfg_init();                             /* this mode's surface */
    gcard_offer();                          /* the card's files as block devices, for this start (guest-card.h) */
#ifdef GUEST_FS_H
    if (mode == GUEST_WL)
        (void)gfs_premount();               /* the card's exFAT for ~/Starbite: here, not in a 9P request */
#endif
    /* the bounce window below 4 GiB: the Image, then the DTB and the initrd, each at a MiB boundary */
    uintptr_t pi = G_BOUNCE, pd = up_mib(pi + ld_i.size), pr = up_mib(pd + ld_d.size);
    if (ld_i.size > G_LINUX_SIZE || ld_d.size > G_LINUX_SIZE || ld_r.size > G_LINUX_SIZE ||
        up_mib(pr + ld_r.size) > G_BOUNCE_END) {
        why = "the Linux files are too big for the load window";
        return -5;
    }
    uint64_t t0 = sb_ms();
    hw_wdt_feed();
    int r = store_load(&ld_i, pi, pd - pi);
    hw_wdt_feed();
    if (!r)
        r = store_load(&ld_d, pd, pr - pd);
    if (!r)
        r = store_load(&ld_r, pr, up_mib(pr + ld_r.size) - pr);
    hw_wdt_feed();
    if (r) {
        why = "a Linux file did not load (CRC or size)";
        return -5;
    }
    uint64_t t1 = sb_ms();
    int lr = sbg_load((const void *)pi, ld_i.size, (const void *)pd, ld_d.size, (const void *)pr, ld_r.size);
    hw_wdt_feed();
    {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "guest: the store's files read in ");
        ub_dec(&u, t1 - t0);
        ub_s(&u, " ms, copied into the carve-out in ");
        ub_dec(&u, sb_ms() - t1);
        ub_s(&u, " ms (Image ");
        ub_dec(&u, ld_i.size);
        ub_s(&u, ", DTB ");
        ub_dec(&u, ld_d.size);
        ub_s(&u, ", initrd ");
        ub_dec(&u, ld_r.size);
        ub_s(&u, " bytes)");
        sb_log(u.b);
    }
    if (lr < 0 || sbg_start() < 0) {
        why = sbg_stats()->why ? sbg_stats()->why : "the guest did not start";
        return -6;
    }
    t_go = sb_ms();
    if (guestnet_guest_started && (sbg_hdr()->host_services & SBSHM_SVC_NET))
        guestnet_guest_started();           /* sbg_net_config for an uplink already up (the guest starts at link 0) */
    return 0;
}

int guest_stop(void)
{
    if (st == GUEST_STARTING && loading) {  /* stopped before its load: nothing ran */
        loading = 0;
        st = GUEST_OFF;
        shown = 0;
        kt_cancel(KT_GUEST, KT_GUEST_ID_POLL);
        sb_log("guest: stopped before the load");
        return 0;
    }
    if (st == GUEST_STARTING || st == GUEST_RUNNING) {
        sbg_stop();
        st = GUEST_STOPPING;
        shown = 0;
        arm(10);
        return 0;
    }
    return -1;
}

void guest_irq(void)
{
    sbg_doorbell();
    if (st != GUEST_OFF)
        kt_set(KT_GUEST, KT_GUEST_ID_IDLE, sb_ms(), 0, 0);   /* a poll at once (the ring has something) */
}

void guest_timer(unsigned id)
{
    (void)id;
    if (st == GUEST_OFF || st == GUEST_FAILED)
        return;
    if (loading) {
        loading = 0;
        if (load_and_start() < 0) {
            st = GUEST_FAILED;
            shown = 0;
            kt_cancel(KT_GUEST, KT_GUEST_ID_POLL);
            heater_m3_released();           /* m3_prep may have run: the heater prepares the cluster again (it still
                                             * refuses while a failed guest leaves the cluster powered) */
            sb_logf("guest: did not start: ", why);
            ui_mark_dirty();
        }
        return;
    }
    sbg_poll();
    const struct sbg_stats *s = sbg_stats();
    if (s->state == SBG_RUNNING && st == GUEST_STARTING) {
        st = GUEST_RUNNING;
        arm(shown ? 10 : 50);
        if (shown)
            sbg_send(SBG_MSG_FOCUS, 1, 0);
        ui_mark_dirty();
    } else if (s->state == SBG_OFF) {
        guest_snd_off();
        st = GUEST_OFF;
        shown = 0;
        kt_cancel(KT_GUEST, KT_GUEST_ID_POLL);
        heater_m3_released();               /* the heater prepares the cluster again before its next start */
        ui_mark_dirty();
        return;
    } else if (s->state == SBG_FAILED) {
        guest_snd_off();
        st = GUEST_FAILED;
        shown = 0;
        why = s->why ? s->why : "the guest failed";
        kt_cancel(KT_GUEST, KT_GUEST_ID_POLL);
        heater_m3_released();
        ui_mark_dirty();
        return;
    }
    if (st == GUEST_RUNNING) {
        volatile struct sbshm_hdr *h = sbg_hdr();
        if (h->surf_seq != seq_seen && shown)
            ui_mark_dirty();
        int flowing = guest_snd_tick();     /* sound: a 5 ms tick while it flows, the normal one otherwise */
        if (flowing != snd_fast) {
            snd_fast = flowing;
            arm(flowing ? 5 : shown ? 10 : 50);
        }
        uint64_t now = sb_ms();
        if (now - t_busy_win >= 5000) {     /* the busy sample: the guest's load, the block traffic of 5 s */
            uint64_t sec = s->blk_rd_sectors + s->blk_wr_sectors;
            if (h->guest_load >= GUEST_BUSY_LOAD || (t_busy_win && sec - busy_sectors >= GUEST_BUSY_KIB * 2U))
                t_busy = now;
            busy_sectors = sec;
            t_busy_win = now;
        }
        if (idle_stop_due(now)) {
            sb_log(apps ? "guest: the screen off for 30 min: stopping (the M3 cluster off)" :
                          "guest: unused for 60 s: stopping (the M3 cluster off)");
            guest_stop();
        }
    }
}

void guest_show(int on)
{
    shown = on ? 1 : 0;
    t_hidden = on ? 0 : sb_ms();
    if (st == GUEST_RUNNING) {
        sbg_send(SBG_MSG_FOCUS, (uint32_t)shown, 0);
        arm(snd_fast ? 5 : shown ? 10 : 50);
    }
    seq_blit = 0;                           /* the next draw copies the whole surface */
}

int guest_frame_new(void)
{
    return guest_shown() && sbg_hdr()->surf_seq != seq_blit;
}

const volatile uint32_t *guest_front(int *idx, unsigned *stride_px)
{
    if (!guest_shown())
        return 0;
    volatile struct sbshm_hdr *h = sbg_hdr();
    uint32_t seq = h->surf_seq;
    sbshm_dmb();
    int f = (int)(h->surf_front & 1U);
    *idx = f;
    *stride_px = h->surf_stride / 4U;
    seq_seen = seq_blit = seq;
    ++blits;
    return (const volatile uint32_t *)(uintptr_t)(G_SHM_BASE + h->surf_off[f]);
}

/* the guest's front buffer at 2x (app: 1440 x 2880; terminal: 1440 x 2120) into the back buffer at (x0, y0).
 * 1.2-alpha3: damage per row (sbshm.h): only the rows newer than the frame drawn last (all after a show, when
 * seq_blit is 0, or when the guest keeps no table). Returns the rows copied; *py0 / *py1 the panel rows touched. */
static int blit_any(uint32_t *const *rowt, uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0, int *py0,
                    int *py1);
int guest_blit(uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0, int *py0, int *py1)
{
    return blit_any(0, back, stride_px, x0, y0, py0, py1);
}
int guest_blit_rows(uint32_t *const *rows, int *py0, int *py1) { return blit_any(rows, 0, 0, 0, 0, py0, py1); }
/* the target's row r: the table's (L3), else the back buffer's at (x0, y0 + r) */
#define TROW(r) (rowt ? rowt[r] : back + (uintptr_t)(y0 + (r)) * stride_px + x0)
static int blit_any(uint32_t *const *rowt, uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0, int *py0,
                    int *py1)
{
    const volatile uint32_t *px;
    uint32_t stride;
    uint32_t seq = seq_seen;
    *py0 = *py1 = -1;
    if (!guest_shown())
        return 0;
    if (!sbg_frame(&seq, &px, &stride)) {   /* nothing new: still draw the last front buffer */
        volatile struct sbshm_hdr *h = sbg_hdr();
        sbshm_dmb();
        px = (const volatile uint32_t *)(uintptr_t)(G_SHM_BASE + h->surf_off[h->surf_front & 1U]);
        stride = h->surf_stride;
        seq = h->surf_seq;
    }
    const volatile uint32_t *rows = sbg_rows();
    uint32_t drawn = seq_blit, w = surf_w(), hh = surf_h(), k = (uint32_t)guest_scale(), n = 0;
    seq_seen = seq_blit = seq;
    uint64_t t0 = sb_ms();
    for (uint32_t y = 0; y < hh; ++y) {
        if (rows && drawn && !sbshm_row_newer(rows[y], drawn))
            continue;
        const volatile uint32_t *s = (const volatile uint32_t *)((uintptr_t)px + (uintptr_t)y * stride);
        uint64_t *d = (uint64_t *)TROW(k * y);
        for (uint32_t x = 0; x < w; ++x) {
            uint64_t c = s[x] | 0xff000000U;
            c |= c << 32;
            for (uint32_t i = 0; i < k / 2; ++i)
                d[x * (k / 2) + i] = c;
        }
        for (uint32_t r = 1; r < k; ++r) {  /* the same row k times */
            uint64_t *e = (uint64_t *)TROW(k * y + r);
            for (uint32_t x = 0; x < w * k / 2; ++x)
                e[x] = d[x];
        }
        if (*py0 < 0)
            *py0 = (int)(y0 + k * y);
        *py1 = (int)(y0 + k * y + k - 1);
        ++n;
    }
    ++blits;
    if (sb_ms() - t0 > blit_ms_max)
        blit_ms_max = (uint32_t)(sb_ms() - t0);
    rows_copied += n;
    return (int)n;
}

/* touch in panel pixels (the view at (0, 40), 2x): surface pixels for the guest */
int guest_input(int kin_type, int x, int y)
{
    if (!guest_shown())
        return 0;
    int k = guest_scale(), sx = x / k, sy = (y - (int)VIEW_Y) / k;
    if (y < (int)VIEW_Y || sy >= (int)surf_h())
        return kin_type != KIN_PRESS;       /* the bars are Starbite's: a press there is not the guest's */
    uint32_t phase = kin_type == KIN_PRESS ? SBG_TOUCH_DOWN : kin_type == KIN_MOVE ? SBG_TOUCH_MOVE : SBG_TOUCH_UP;
    sbg_touch(0, phase, sx, sy);
    return 1;
}

int guest_key(unsigned linux_code, int down)
{
    return guest_shown() ? sbg_key(linux_code, down) : 0;
}

/* 1.2-alpha3: one character typed (Starbite's keyboard, the self test): its Linux key, with Shift around it */
int guest_type(char c)
{
    int shift = 0;
    unsigned code = sbshm_ascii_key(c, &shift);
    if (!code || !guest_shown())
        return 0;
    if (shift)
        sbg_key(SBSHM_KEY_LEFTSHIFT, 1);
    sbg_key(code, 1);
    sbg_key(code, 0);
    if (shift)
        sbg_key(SBSHM_KEY_LEFTSHIFT, 0);
    return 1;
}

void guest_screen(int on)
{
    screen = on ? 1 : 0;
    t_screen_off = on ? 0 : sb_ms();
    if (st == GUEST_RUNNING)
        sbg_screen(on);
}

static uint32_t touches_seen(void) { return st == GUEST_RUNNING ? sbg_hdr()->touches_seen : 0; }
static uint32_t surf_seq(void) { return st == GUEST_RUNNING ? sbg_hdr()->surf_seq : 0; }
static const char *start_note(void)
{
    static char b[96];
    struct ub u;
    const struct sbg_stats *s = sbg_stats();
    ub_init(&u);
    ub_s(&u, "HELLO after ");
    ub_dec(&u, s->t_hello);
    ub_s(&u, " ms, ");
    ub_dec(&u, s->hello_ncpus);
    ub_s(&u, " CPUs, guest CPU interfaces ");
    ub_hex(&u, s->guest_cpuif, 2);
    sb_strcpy(b, sizeof(b), u.b);
    return b;
}
static uint32_t cluster_off_ms(void) { return (uint32_t)sbg_stats()->t_off; }
#else
/* ============================ the host's fake guest (preview, host tests) ============================ */
/* SBOS_HOST_GUEST=1: the three store files exist; =fail: they exist and the start fails. The fake starts in
 * 400 ms, draws a test pattern and the app's touch marks, and is off 100 ms after the stop. */
static uint64_t t_next;
static int fake_m3, fake_touches, fake_seq;
static int dots[16][2], ndots;
static char typed[96];                      /* the fake terminal: the line being typed, then shown as blocks */
static int ntyped, shown_chars;

void guest_init(void) { sb_up = 1; }
int guest_owns_m3(void) { return st != GUEST_OFF && st != GUEST_FAILED ? 1 : fake_m3 && st == GUEST_FAILED; }
/* SBOS_HOST_GUEST=1 (or fail): both guests' files; =app: only the app's */
int guest_available_mode(int m)
{
    const char *g = getenv("SBOS_HOST_GUEST");
    return g && (m == GUEST_APP || g[0] != 'a');
}
int guest_available(void) { return guest_available_mode(GUEST_APP); }
int guest_start(void) { return guest_start_mode(GUEST_APP); }

int guest_start_mode(int m)
{
    m = m == GUEST_TERM || m == GUEST_WL ? m : GUEST_APP;
    if (!sb_up || (st != GUEST_OFF && st != GUEST_FAILED))
        return -1;
    if (heater_m3_busy()) {
        why = "the heater has the big cores";
        return -2;
    }
    if (!guest_available_mode(m)) {
        why = m == GUEST_TERM ? "the terminal's Linux files are not in the store" :
              m == GUEST_WL ? "the Linux apps' files are not in the store" : "the Linux files are not in the store";
        return -4;
    }
    mode = m;
    term_ok = 0;
    ntyped = shown_chars = 0;
    const char *g = getenv("SBOS_HOST_GUEST");
    if (g && g[0] == 'f') {
        why = "the guest did not start (the host's fake: SBOS_HOST_GUEST=fail)";
        st = GUEST_FAILED;
        sb_log("guest: start failed (fake)");
        return -6;
    }
    sb_log("guest: the store's files read and copied (fake)");
    st = GUEST_STARTING;
    fake_m3 = 1;
    ndots = 0;
    fake_touches = 0;
    t_next = sb_ms() + 400;
    seq_seen = seq_blit = 0;
    arm(10);
    return 0;
}

int guest_stop(void)
{
    if (st == GUEST_STARTING || st == GUEST_RUNNING) {
        st = GUEST_STOPPING;
        shown = 0;
        t_next = sb_ms() + 100;
        sb_log("guest: MSG_STOP (fake)");
        arm(10);
        return 0;
    }
    return -1;
}

void guest_irq(void) {}

void guest_timer(unsigned id)
{
    (void)id;
    uint64_t now = sb_ms();
    if (st == GUEST_STARTING && now >= t_next) {
        st = GUEST_RUNNING;
        fake_seq++;
        sb_log("guest: HELLO after 400 ms, 4 CPUs (fake)");
        arm(shown ? 10 : 50);
        ui_mark_dirty();
    } else if (st == GUEST_STOPPING && now >= t_next) {
        st = GUEST_OFF;
        fake_m3 = 0;
        kt_cancel(KT_GUEST, KT_GUEST_ID_POLL);
        heater_m3_released();
        sb_log("guest: every guest core off, the cluster off after 100 ms (fake)");
        ui_mark_dirty();
    } else if (st == GUEST_RUNNING) {
        if (shown) {                        /* like guestd: a new frame every tick, faster than the view draws */
            fake_seq++;
            ui_mark_dirty();
        }
        if (idle_stop_due(now)) {
            sb_log(apps ? "guest: the screen off for 30 min: stopping (the M3 cluster off)" :
                          "guest: unused for 60 s: stopping (the M3 cluster off)");
            guest_stop();
        }
    }
}

void guest_show(int on)
{
    shown = on ? 1 : 0;
    t_hidden = on ? 0 : sb_ms();
    if (st == GUEST_RUNNING)
        arm(shown ? 10 : 50);
    seq_blit = 0;
}

int guest_frame_new(void) { return guest_shown() && (uint32_t)fake_seq != seq_blit; }
/* the host fake has no shared memory: no zero-copy front (the host's sh_linux_zero_copy is 0 anyway) */
const volatile uint32_t *guest_front(int *idx, unsigned *stride_px)
{
    *idx = 0;
    *stride_px = 0;
    return 0;
}

static int blit_any(uint32_t *const *rowt, uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0, int *py0,
                    int *py1);
int guest_blit(uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0, int *py0, int *py1)
{
    return blit_any(0, back, stride_px, x0, y0, py0, py1);
}
int guest_blit_rows(uint32_t *const *rows, int *py0, int *py1) { return blit_any(rows, 0, 0, 0, 0, py0, py1); }
#define TROW(r) (rowt ? rowt[r] : back + (uintptr_t)(y0 + (r)) * stride_px + x0)
static int blit_any(uint32_t *const *rowt, uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0, int *py0,
                    int *py1)
{
    *py0 = *py1 = -1;
    if (!guest_shown())
        return 0;
    seq_seen = seq_blit = (uint32_t)fake_seq;
    ++blits;
    uint32_t k = (uint32_t)guest_scale(), w = surf_w(), hh = surf_h();
    for (uint32_t y = 0; y < k * hh; ++y) {
        uint32_t *d = TROW(y);
        uint32_t sy = y / k;
        for (uint32_t x = 0; x < k * w; ++x) {
            uint32_t sx = x / k, c;
            if (mode == GUEST_TERM) {       /* black, the typed characters as 16 x 32 blocks, 45 a row */
                int cell = (int)(sy / 32) * 45 + (int)(sx / 16);
                c = cell < shown_chars && (sx % 16) < 12 && (sy % 32) > 4 && (sy % 32) < 28 ? 0xffaaaaaaU : 0xff000000U;
            } else {
                c = sy < 120 ? 0xff1e1e2eU : 0xff000000U | ((sx * 255 / w) << 16) | ((sy * 255 / hh) << 8) | 0x60U;
                for (int i = 0; i < ndots; ++i) {
                    int dx = (int)sx - dots[i][0], dy = (int)sy - dots[i][1];
                    if (dx * dx + dy * dy <= 14 * 14)
                        c = 0xffffd700U;
                }
            }
            d[x] = c;
        }
    }
    *py0 = (int)y0;
    *py1 = (int)(y0 + k * hh - 1);
    rows_copied += hh;
    return (int)hh;
}

int guest_input(int kin_type, int x, int y)
{
    if (!guest_shown())
        return 0;
    int k = guest_scale(), sx = x / k, sy = (y - (int)VIEW_Y) / k;
    if (y < (int)VIEW_Y || sy >= (int)surf_h())
        return kin_type != KIN_PRESS;
    fake_touches++;
    if (kin_type == KIN_PRESS && ndots < 16 && mode == GUEST_APP) {
        dots[ndots][0] = sx;
        dots[ndots][1] = sy;
        ++ndots;
        fake_seq += 2;
        ui_mark_dirty();
    }
    return 1;
}

/* the fake terminal: Enter runs the line; "echo STARBITE-TERM-OK > /dev/kmsg" logs the marker as the kernel would */
int guest_type(char c)
{
    if (!guest_shown())
        return 0;
    if (c == '\n') {
        typed[ntyped] = 0;
        sb_logf("guest: the fake terminal ran: ", typed);
        int hit = 0;
        for (int i = 0; typed[i]; ++i)
            hit |= sb_prefix(typed + i, "STARBITE-TERM-OK");
        if (hit && mode != GUEST_APP) {
            term_ok = 1;
            sb_log("linux| STARBITE-TERM-OK (fake)");
        }
        ntyped = 0;
    } else if (c == '\b') {
        if (ntyped)
            --ntyped;
    } else if (ntyped + 1 < (int)sizeof(typed)) {
        typed[ntyped++] = c;
    }
    shown_chars++;
    fake_seq++;
    ui_mark_dirty();
    return 1;
}

int guest_key(unsigned linux_code, int down)
{
    (void)linux_code, (void)down;
    return guest_shown();
}

void guest_screen(int on)
{
    screen = on ? 1 : 0;
    t_screen_off = on ? 0 : sb_ms();
}

static uint32_t touches_seen(void) { return st == GUEST_RUNNING ? (uint32_t)fake_touches : 0; }
static uint32_t surf_seq(void) { return (uint32_t)fake_seq; }
static const char *start_note(void) { return "HELLO after 400 ms, 4 CPUs (fake)"; }
static uint32_t cluster_off_ms(void) { return 100; }
static int hw_m3_on_fake(void) { return fake_m3; }
#define hw_m3_on hw_m3_on_fake
#endif

void guest_power_off(void)
{
    if (guest_stop() < 0)
        return;
    for (unsigned i = 0; i < 100 && st == GUEST_STOPPING; ++i) {
        sb_mdelay(10);
        guest_timer(KT_GUEST_ID_POLL);
    }
}

/* ================================== the unattended check (P3a) ===================================== */
#define TAP_SX 360
#define TAP_SY 840
static int gs_phase, gs_fail;
static uint64_t gs_t;
static uint32_t gs_seq, gs_blits;
static int gs_typed;

static void gs_log(const char *a, const char *b)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "guest selftest: ");
    ub_s(&u, a);
    if (b)
        ub_s(&u, b);
    sb_log(u.b);
}
static int gs_failed(const char *what)
{
    gs_log("FAILED: ", what);
    if (st == GUEST_STARTING || st == GUEST_RUNNING)
        guest_stop();
    gs_phase = 99;
    gs_fail = 1;
    return -1;
}

int guest_selftest_step(const struct guest_st_ops *o)
{
    uint64_t now = sb_ms(), dt = now - gs_t;
    switch (gs_phase) {
    case 0:
        gs_log("start: two cycles (the app shown with a tap; the terminal with a typed command)", 0);
        if (!guest_available())
            return gs_failed("the Linux files are not in the store (guest-linux.img / .dtb / -rd.img)");
        if (heater_m3_busy())
            return gs_failed("the heater has the big cores");
        o->open();
        if (st != GUEST_STARTING)
            return gs_failed(why[0] ? why : "the start was refused");
        if (!guest_owns_m3() || !heater_guest_refuses())
            return gs_failed("the heater would not have been refused while Linux runs");
        gs_log("cycle 1: started; the heater refuses while Linux has the big cores", 0);
        gs_t = now;
        gs_phase = 1;
        return 0;
    case 1:
    case 7:
        if (st == GUEST_FAILED)
            return gs_failed(why);
        if (st == GUEST_RUNNING) {
            gs_log(gs_phase == 1 ? "cycle 1: running: " : "cycle 2 (terminal): running: ", start_note());
            gs_t = now;
            gs_phase = gs_phase == 7 ? 10 : 2;
        } else if (dt > 45000) {
            return gs_failed("no HELLO within 45 s");
        }
        return 0;
    case 2:                                 /* 3 s of the app on screen, then the tap */
        if (st != GUEST_RUNNING)
            return gs_failed("the guest stopped by itself");
        if (dt >= 3000) {
            if (!guest_shown())
                return gs_failed("the Linux view is not shown");
            gs_seq = surf_seq();
            gs_blits = blits;
            guest_input(KIN_PRESS, 2 * TAP_SX, (int)VIEW_Y + 2 * TAP_SY);
            gs_t = now;
            gs_phase = 3;
        }
        return 0;
    case 3:
        if (dt >= 150) {
            guest_input(KIN_RELEASE, 2 * TAP_SX, (int)VIEW_Y + 2 * TAP_SY);
            gs_t = now;
            gs_phase = 4;
        }
        return 0;
    case 4: {
        /* the mark in a frame the view actually drew: one published at least 2 frames after the tap (the app's dots
         * persist). P3a waited for "no frame pending" instead; on the phone the guest publishes faster than the 2x
         * view draws, so that moment never came (touches seen, the check timed out). */
        if (st != GUEST_RUNNING)
            return gs_failed("the guest stopped by itself");
        const uint32_t *p = o->panel ? o->panel() : 0;
        if (touches_seen() >= 2 && seq_blit && (int32_t)(seq_blit - gs_seq) >= 2 && p) {
            uint32_t c = p[(uintptr_t)(VIEW_Y + 2 * TAP_SY) * 1440U + 2 * TAP_SX] & 0xffffffU;
            struct ub u;
            ub_init(&u);
            ub_s(&u, "cycle 1: the tap reached the app: pixel ");
            ub_hex(&u, c, 6);
            ub_s(&u, c == 0xffd700U ? " (the app's touch mark)" : " (NOT the touch mark)");
            ub_s(&u, " in the drawn frame +");
            ub_dec(&u, seq_blit - gs_seq);
            ub_s(&u, " after the tap, ");
            ub_dec(&u, dt);
            ub_s(&u, " ms after the release");
            gs_log(u.b, 0);
            if (c != 0xffd700U)
                return gs_failed("the tap did not reach the app's pixels");
            gs_t = now;
            gs_phase = 5;
        } else if (dt > 5000) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "the tap's check timed out (5 s): touches seen ");
            ub_dec(&u, touches_seen());
            ub_s(&u, ", frames published since the tap ");
            ub_dec(&u, surf_seq() - gs_seq);
            ub_s(&u, ", the drawn frame ");
            ub_sdec(&u, (int32_t)(seq_blit - gs_seq));
            ub_s(&u, ", view draws since the tap ");
            ub_dec(&u, blits - gs_blits);
            gs_log(u.b, 0);
            return gs_failed("the tap was not seen in a drawn frame within 5 s");
        }
        return 0;
    }
    case 5:                                 /* 2 s more, then home (hidden) and the stop */
        if (dt >= 2000) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "cycle 1: the view drew ");
            ub_dec(&u, blits);
            ub_s(&u, " frames, the slowest 2x copy ");
            ub_dec(&u, blit_ms_max);
            ub_s(&u, " ms; the guest published ");
            ub_dec(&u, surf_seq());
            gs_log(u.b, 0);
            o->home();
            if (guest_shown())
                return gs_failed("the view is still shown after home");
            guest_stop();
            gs_t = now;
            gs_phase = 6;
        }
        return 0;
    case 6:
    case 8:
        if (st == GUEST_OFF) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, gs_phase == 6 ? "cycle 1" : "cycle 2");
            ub_s(&u, ": off, the cluster off ");
            ub_dec(&u, cluster_off_ms());
            ub_s(&u, " ms after MSG_STOP; M3 cluster powered ");
            ub_dec(&u, (uint64_t)hw_m3_on());
            gs_log(u.b, 0);
            if (hw_m3_on() || guest_owns_m3())
                return gs_failed("the M3 cluster still powered after the guest's stop");
            if (gs_phase == 8) {
                gs_log("passed: the app with the tap, the terminal with a typed command, the cluster off twice, "
                       "the heater refused meanwhile", 0);
                gs_phase = 99;
                return 1;
            }
            if (!guest_available_mode(GUEST_TERM))
                return gs_failed("the terminal's Linux files are not in the store (guest-term.img / .dtb / -rd.img)");
            o->open_term();
            if (st != GUEST_STARTING)
                return gs_failed(why[0] ? why : "the terminal's start was refused");
            gs_log("cycle 2: the terminal started (shown at 2x above the keyboard)", 0);
            gs_t = now;
            gs_phase = 7;
        } else if (st == GUEST_FAILED) {
            return gs_failed(why);
        } else if (dt > 8000) {
            return gs_failed("the guest did not power off within 8 s of MSG_STOP");
        }
        return 0;
    case 10:                                /* the shell's prompt first */
        if (st != GUEST_RUNNING)
            return gs_failed("the terminal stopped by itself");
        if (dt >= 1500) {
            gs_typed = 0;
            gs_phase = 11;
        }
        return 0;
    case 11: {                              /* the command, four characters a step (the ring stays far from full) */
        static const char cmd[] = "uname -a; grep -c processor /proc/cpuinfo; echo STARBITE-TERM-OK > /dev/kmsg\n";
        for (int i = 0; i < 4 && cmd[gs_typed]; ++i, ++gs_typed)
            if (!guest_type(cmd[gs_typed]))
                return gs_failed("a character could not be typed (no key, or the ring full)");
        if (!cmd[gs_typed]) {
            gs_log("cycle 2: typed: uname -a; grep -c processor /proc/cpuinfo; echo STARBITE-TERM-OK > /dev/kmsg", 0);
            gs_t = now;
            gs_blits = blits;
            gs_phase = 12;
        }
        return 0;
    }
    case 12: {                              /* the command ran (its kmsg line) and the terminal shows text */
        if (st != GUEST_RUNNING)
            return gs_failed("the terminal stopped by itself");
        const uint32_t *p = o->panel ? o->panel() : 0;
        /* the view drew since the typing and has nothing pending (a quiet terminal publishes only on change: after
         * its output no new frame comes), or kept drawing a guest that keeps publishing */
        if (term_ok && p && dt >= 500 && blits != gs_blits && (!guest_frame_new() || blits - gs_blits >= 3)) {
            uint32_t lit = 0, lit_rows = 0;
            for (uint32_t y = VIEW_Y; y < VIEW_Y + 2U * GUEST_TERM_H; y += 4) {
                uint32_t n = 0;
                for (uint32_t x = 0; x < 1440U; x += 2)
                    n += (p[(uintptr_t)y * 1440U + x] & 0xffffffU) != 0;
                lit += n;
                lit_rows += n != 0;
            }
            struct ub u;
            ub_init(&u);
            ub_s(&u, "cycle 2: the command ran (STARBITE-TERM-OK in the guest's log) after ");
            ub_dec(&u, dt);
            ub_s(&u, " ms; the terminal shows text: ");
            ub_dec(&u, lit);
            ub_s(&u, " lit samples in ");
            ub_dec(&u, lit_rows);
            ub_s(&u, " rows; the view drew ");
            ub_dec(&u, blits - gs_blits);
            ub_s(&u, " frames since the typing, ");
            ub_dec(&u, rows_copied);
            ub_s(&u, " rows copied in all, the slowest copy ");
            ub_dec(&u, blit_ms_max);
            ub_s(&u, " ms");
            gs_log(u.b, 0);
            if (lit_rows < 4)
                return gs_failed("the terminal's view shows no text");
            o->home();
            guest_stop();
            gs_t = now;
            gs_phase = 8;
        } else if (dt > 15000) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "the typed command's check timed out (15 s): marker ");
            ub_dec(&u, (uint64_t)term_ok);
            ub_s(&u, ", view draws since the typing ");
            ub_dec(&u, blits - gs_blits);
            gs_log(u.b, 0);
            return gs_failed(term_ok ? "the terminal's view did not draw after the command within 15 s" :
                                       "STARBITE-TERM-OK did not reach the guest's log within 15 s");
        }
        return 0;
    }
    default:
        return gs_fail ? -1 : 1;
    }
}
