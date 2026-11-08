/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 0.9 / P3: the guest service, Linux apps on the M3 cluster (docs/STARBITE-OS-LINUX-M3.md).
 * A drop-in for kern/ (the starbite-os session wires it in: starbite/guest/sbos/INTEGRATION.md). Everything
 * the guest needs from the hardware goes through sbghost (starbite/guest/host), the core the phone payloads
 * P2..P2d ran: the carve-out, the trampoline below 4 GiB, CPU_ON, the shared header, the rings, the log zone.
 *
 * Life: off until an app needs Linux (the M3 cluster powered down) -> guest_start: the store's three files
 * straight into the carve-out (store_load), the M3 words back to the boot state, the vendor PMU defaults,
 * cluster/core on, CPU_ON(0x100) -> the poll timer (10 ms while starting or shown, 50 ms in the background)
 * -> guest_stop: MSG_STOP, the guest powers its cores and the cluster off itself (P2c) -> off. Hidden for 60 s
 * or the screen off for 60 s: stopped (power). The heater and the guest exclude each other (both own the M3s).
 * Never blocks the loop: start and stop return at once, the timer does the rest.
 */
#include "sb.h"
#include "store.h"
#include "kern.h"
#include "kinput.h"
#include "guest.h"
#include "../../guest/host/sbghost.h"

#define GUEST_DB_GUEST 511U                 /* P1: the doorbells (idle, non-secure, both directions, P1c) */
#define GUEST_DB_HOST 510U
#define SW 720U
#define SH 1440U
#define IDLE_STOP_MS 60000U

static int st = GUEST_OFF, shown, screen = 1;
static const char *why = "";
static uint32_t seq_seen = 0, seq_blit = 0;
static uint64_t t_hidden, t_screen_off;
static int sb_up;

#ifndef SB_HOST
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
                                       "failed", "error", "smp: Brought up"};
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
/* the boot state, the vendor PMU defaults, then the vendor's hotplug-in for cluster 1's first core */
static int m3_prep(void)
{
    (void)hw_m3_fp_reset();
    if (!hw_m3_prep())
        return 0;
    hw_m3_core_on(4, 1);
    return 1;
}
static const struct sbg_os shim = {
    .ms = ms, .log = host_log, .guest_log = guest_log, .cpu_on = cpu_on, .cluster_off = cluster_off,
    .m3_prep = m3_prep,
};
#endif

void guest_init(void)
{
#ifndef SB_HOST
    uint8_t me = *(volatile uint8_t *)(SBG_S9_GICD + 0x800);   /* this core's interface (banked) */
    struct sbg_cfg cfg = {
        .linux_base = SBG_S9_LINUX_BASE, .linux_size = SBG_S9_LINUX_SIZE,
        .shm_base = SBG_S9_SHM_BASE, .shm_size = SBG_S9_SHM_SIZE,
        .gicd = SBG_S9_GICD, .first_mpidr = 0x100, .tramp_pa = SBG_S9_TRAMP,
        .surf_w = SW, .surf_h = SH,
        .db_guest = GUEST_DB_GUEST, .db_host = GUEST_DB_HOST,
        .host_lines = {0},                  /* the guest kernel has the shared-distributor patch */
        .repair = 0,
        .boot_timeout_ms = 30000, .stop_timeout_ms = 5000,
    };
    sbg_init(&shim, &cfg, me ? me : 1);
    sbg_gic_prepare();                      /* the doorbells: edge; the host's routed here and enabled */
    sb_up = 1;
#endif
}

unsigned guest_db_host(void) { return GUEST_DB_HOST; }
int guest_state(void) { return st; }
const char *guest_why(void) { return why; }
int guest_owns_m3(void) { return st != GUEST_OFF && st != GUEST_FAILED ? 1 : hw_m3_on() && st == GUEST_FAILED; }
int guest_shown(void) { return shown && st == GUEST_RUNNING; }

int guest_available(void)
{
#ifdef SB_HOST
    return 0;
#else
    struct store_ent e;
    return store_find(GUEST_STORE_IMAGE, &e) && store_find(GUEST_STORE_DTB, &e) && store_find(GUEST_STORE_RD, &e);
#endif
}

static void arm(uint32_t period)
{
    kt_cancel(KT_GUEST, KT_GUEST_ID_POLL);
    kt_set(KT_GUEST, KT_GUEST_ID_POLL, sb_ms(), period, 1);
}

int guest_start(void)
{
#ifdef SB_HOST
    why = "no guest in the host preview";
    return -1;
#else
    if (!sb_up || (st != GUEST_OFF && st != GUEST_FAILED))
        return -1;
    if (heater_busy()) {
        why = "the heater has the big cores";
        return -2;
    }
    if (st == GUEST_FAILED && hw_m3_on()) {
        why = "the last guest did not power off: restart the phone first";
        return -3;
    }
    struct store_ent ei, ed, er;
    if (!store_find(GUEST_STORE_IMAGE, &ei) || !store_find(GUEST_STORE_DTB, &ed) || !store_find(GUEST_STORE_RD, &er)) {
        why = "the Linux files are not in the store";
        return -4;
    }
    /* straight into the carve-out (non-cacheable here: no cache maintenance needed for the M3s) */
    uint64_t base = SBG_S9_LINUX_BASE;
    if (store_load(&ei, (uintptr_t)(base + SBG_IMAGE_AT), SBG_DTB_OFF - SBG_IMAGE_AT) ||
        store_load(&ed, (uintptr_t)(base + SBG_DTB_OFF), SBG_RD_OFF - SBG_DTB_OFF) ||
        store_load(&er, (uintptr_t)(base + SBG_RD_OFF), SBG_S9_LINUX_SIZE - SBG_RD_OFF)) {
        why = "a Linux file did not load (CRC or size)";
        st = GUEST_FAILED;
        return -5;
    }
    hw_wdt_feed();
    if (sbg_load_placed(ei.size, ed.size, er.size) < 0 || sbg_start() < 0) {
        why = sbg_stats()->why ? sbg_stats()->why : "the guest did not start";
        st = GUEST_FAILED;
        return -6;
    }
    st = GUEST_STARTING;
    seq_seen = seq_blit = 0;
    arm(10);
    return 0;
#endif
}

int guest_stop(void)
{
#ifndef SB_HOST
    if (st == GUEST_STARTING || st == GUEST_RUNNING) {
        sbg_stop();
        st = GUEST_STOPPING;
        shown = 0;
        arm(10);
        return 0;
    }
#endif
    return -1;
}

void guest_irq(void)
{
#ifndef SB_HOST
    sbg_doorbell();
    if (st != GUEST_OFF)
        kt_set(KT_GUEST, KT_GUEST_ID_IDLE, sb_ms(), 0, 0);   /* a poll at once (the ring has something) */
#endif
}

void guest_timer(unsigned id)
{
#ifndef SB_HOST
    (void)id;
    if (st == GUEST_OFF || st == GUEST_FAILED)
        return;
    sbg_poll();
    const struct sbg_stats *s = sbg_stats();
    if (s->state == SBG_RUNNING && st == GUEST_STARTING) {
        st = GUEST_RUNNING;
        arm(shown ? 10 : 50);
        ui_mark_dirty();
    } else if (s->state == SBG_OFF) {
        st = GUEST_OFF;
        kt_cancel(KT_GUEST, KT_GUEST_ID_POLL);
        ui_mark_dirty();
        return;
    } else if (s->state == SBG_FAILED) {
        st = GUEST_FAILED;
        why = s->why ? s->why : "the guest failed";
        kt_cancel(KT_GUEST, KT_GUEST_ID_POLL);
        ui_mark_dirty();
        return;
    }
    if (st == GUEST_RUNNING) {
        volatile struct sbshm_hdr *h = sbg_hdr();
        if (h->surf_seq != seq_seen && shown)
            ui_mark_dirty();
        uint64_t now = sb_ms();
        if ((!shown && t_hidden && now - t_hidden > IDLE_STOP_MS) ||
            (!screen && t_screen_off && now - t_screen_off > IDLE_STOP_MS)) {
            sb_log("guest: unused for 60 s: stopping (the M3 cluster off)");
            guest_stop();
        }
    }
#endif
}

void guest_show(int on)
{
#ifndef SB_HOST
    shown = on ? 1 : 0;
    t_hidden = on ? 0 : sb_ms();
    if (st == GUEST_RUNNING) {
        sbg_send(SBG_MSG_FOCUS, (uint32_t)shown, 0);
        arm(shown ? 10 : 50);
    }
    seq_blit = 0;                           /* the next draw copies the whole surface */
#else
    (void)on;
#endif
}

int guest_frame_new(void)
{
#ifndef SB_HOST
    return guest_shown() && sbg_hdr()->surf_seq != seq_blit;
#else
    return 0;
#endif
}

/* the guest's front buffer at 2x into the back buffer at (x0, y0): 1440 x 2880 */
void guest_blit(uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0)
{
#ifndef SB_HOST
    const volatile uint32_t *px;
    uint32_t stride;
    uint32_t seq = seq_seen;
    if (!guest_shown())
        return;
    if (!sbg_frame(&seq, &px, &stride)) {   /* nothing new: still draw the last front buffer */
        volatile struct sbshm_hdr *h = sbg_hdr();
        sbshm_dmb();
        px = (const volatile uint32_t *)(uintptr_t)(SBG_S9_SHM_BASE + h->surf_off[h->surf_front & 1U]);
        stride = h->surf_stride;
        seq = h->surf_seq;
    }
    seq_seen = seq_blit = seq;
    for (uint32_t y = 0; y < SH; ++y) {
        const volatile uint32_t *s = (const volatile uint32_t *)((uintptr_t)px + (uintptr_t)y * stride);
        uint64_t *d0 = (uint64_t *)(back + (uintptr_t)(y0 + 2 * y) * stride_px + x0);
        uint64_t *d1 = (uint64_t *)(back + (uintptr_t)(y0 + 2 * y + 1) * stride_px + x0);
        for (uint32_t x = 0; x < SW; ++x) {
            uint64_t c = s[x] | 0xff000000U;
            c |= c << 32;
            d0[x] = c;
            d1[x] = c;
        }
    }
#else
    (void)back, (void)stride_px, (void)x0, (void)y0;
#endif
}

/* touch in panel pixels (the view at (0, 40), 2x): surface pixels for the guest */
int guest_input(int kin_type, int x, int y)
{
#ifndef SB_HOST
    if (!guest_shown())
        return 0;
    int sx = x / 2, sy = (y - 40) / 2;
    if (sy < 0 || sy >= (int)SH)
        return kin_type != KIN_PRESS;       /* the bars are Starbite's: a press there is not the guest's */
    uint32_t phase = kin_type == KIN_PRESS ? SBG_TOUCH_DOWN : kin_type == KIN_MOVE ? SBG_TOUCH_MOVE : SBG_TOUCH_UP;
    sbg_touch(0, phase, sx, sy);
    return 1;
#else
    (void)kin_type, (void)x, (void)y;
    return 0;
#endif
}

int guest_key(unsigned linux_code, int down)
{
#ifndef SB_HOST
    return guest_shown() ? sbg_key(linux_code, down) : 0;
#else
    (void)linux_code, (void)down;
    return 0;
#endif
}

void guest_screen(int on)
{
#ifndef SB_HOST
    screen = on ? 1 : 0;
    t_screen_off = on ? 0 : sb_ms();
    if (st == GUEST_RUNNING)
        sbg_screen(on);
#else
    (void)on;
#endif
}

void guest_power_off(void)
{
#ifndef SB_HOST
    if (guest_stop() < 0)
        return;
    for (unsigned i = 0; i < 100 && st == GUEST_STOPPING; ++i) {
        sb_mdelay(10);
        guest_timer(KT_GUEST_ID_POLL);
    }
#endif
}
