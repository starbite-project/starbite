/* SPDX-License-Identifier: MIT
 * "Starbite OS Next" (P2 of the UI port plan): the kernel's UI driver for the sbtk shell. Built only into the
 * starbite-next images (build.sh: main.c with -DSBOS_NEXT maps ui_kinit / ui_kdraw / ui_kevent / ui_ktimer /
 * ui_kminute here); the rest of Starbite OS (power, battery, input, timers, the store, the loader's one-shot next
 * boot) is as in the regular image. What it does:
 *   - sbtk's platform: a first-fit heap with coalescing inside one block of the arena (sb_alloc never frees),
 *     the clock (sb_ticks), the log;
 *   - the frames: the compositor composes into the old UI's back buffer (RAM) and presents into the scanout
 *     buffer (SB_FB), motion frames at half resolution scaled 2x straight into it; a 60 Hz frame timer runs only
 *     while something moves; a frame that drew anything kicks the panel (the refresh-saving mode's next TE);
 *   - the A55 at its top step (1794 MHz) while a finger is down or anything moves, back to 1053 MHz 400 ms after
 *     the last motion (uibench-1: 60 fps needs it);
 *   - slide to start: the loader's one-shot next boot (power_boot_os), a dry run in the self test;
 *   - the live clock, date, battery and temperature (clock_now, battery_get) once a minute;
 *   - the self test (SBOS_SELFTEST_NEXT): the bake's times, then the four films of the host tool played through
 *     the same gestures with the clock's own time, each frame timed, a summary per film, then recovery (pmsg). */
#include "sb.h"
#include "sbui.h"
#include "kern.h"
#include "kinput.h"
#include "sbtk.h"
#include "shell.h"

/* ---- the heap (tk_heap.c) on one block of the arena ---------------------------------------------------- */
void tk_heap_init(void *mem, uint64_t size);
uint64_t tk_heap_size(void);
static uint64_t heap_size;
static void *heap_mem;
static void heap_init(uint64_t want)
{
    uint64_t avail = sb_arena_free();
    uint64_t keep = 24ULL << 20;            /* the rest of the kernel's arena users (apps, Wi-Fi, pools) */
    if (avail > keep && avail - keep < want) want = avail - keep;
    want &= ~63ULL;
    void *m = sb_alloc(want);
    if (!m) { sb_log("next: no arena for the UI heap"); return; }
    tk_heap_init(m, want);
    heap_size = want;
    heap_mem = m;
}
static unsigned heap_full;          /* refusals: in the self test a failure (a part of the screen stayed empty) */
void tk_heap_oom(uint64_t n, uint64_t used)
{
    heap_full++;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: the UI heap is full: ");
    ub_dec(&u, n);
    ub_s(&u, " bytes asked, ");
    ub_dec(&u, used >> 20);
    ub_s(&u, " MiB in use");
    sb_log(u.b);
}
uint64_t tk_us(void) { return sb_ticks() * 1000000ULL / sb_freq(); }
void tk_log(const char *s) { sb_logf("next: ", s); }

/* ---- the other three A55 (cores 1..3) as band workers (P2b) ---------------------------------------------
 * A frame's bands go to whichever cores are running: each takes the next band until none are left (the boot core
 * too), so a core that is not up yet only means fewer helpers. Workers start with the first touch (sb_smp_launch:
 * the loop never waits for a core) and stop after 5 s without motion, so idle phones do not spin three cores. */
static struct {
    volatile uint32_t gen, next, done, n;
    void (*fn)(void *, int);
    void *arg;
    uint32_t pad[8];
} JOB __attribute__((aligned(64)));
static int workers_on;
static uint32_t worker_bands[4];

static void take_bands(unsigned core)
{
    for (;;) {
        uint32_t i = __atomic_fetch_add(&JOB.next, 1, __ATOMIC_ACQ_REL);
        if (i >= __atomic_load_n(&JOB.n, __ATOMIC_ACQUIRE)) break;
        JOB.fn(JOB.arg, (int)i);
        worker_bands[core & 3]++;
        __atomic_fetch_add(&JOB.done, 1, __ATOMIC_ACQ_REL);
    }
}
static void band_worker(unsigned w, void *arg)
{
    (void)arg;
    uint32_t seen = __atomic_load_n(&JOB.gen, __ATOMIC_ACQUIRE);
    unsigned spins = 0;
    for (;;) {
        uint32_t g = __atomic_load_n(&JOB.gen, __ATOMIC_ACQUIRE);
        if (g != seen) {
            seen = g;
            take_bands(w);
            continue;
        }
        if (++spins >= 1024) {
            spins = 0;
            if (sb_worker_stop()) return;
        }
        __asm__ volatile("yield");
    }
}
static void kpar(void (*fn)(void *, int), void *arg, int n)
{
    if (!workers_on) {
        for (int i = 0; i < n; i++) fn(arg, i);
        return;
    }
    JOB.fn = fn;
    JOB.arg = arg;
    __atomic_store_n(&JOB.n, (uint32_t)n, __ATOMIC_RELEASE);
    __atomic_store_n(&JOB.done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&JOB.next, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&JOB.gen, 1, __ATOMIC_ACQ_REL);
    take_bands(0);
    while (__atomic_load_n(&JOB.done, __ATOMIC_ACQUIRE) < (uint32_t)n)
        __asm__ volatile("yield");
}
static void workers_start(void)
{
    if (workers_on) return;
    int any = 0;
    for (unsigned w = 1; w <= 3; w++) {
        int64_t r = sb_smp_launch(w, band_worker, 0);
        if (r == 0) any = 1;
        else {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "next: core ");
            ub_dec(&u, w);
            ub_s(&u, " not launched: ");
            ub_sdec(&u, r);
            sb_log(u.b);
        }
    }
    workers_on = any;
}
static void workers_stop(void)
{
    if (!workers_on) return;
    workers_on = 0;
    unsigned left = sb_smp_stop(50);
    if (left) sb_logx("next: workers that did not stop: ", left);
}

/* ---- the pack (next_pack.S) -------------------------------------------------------------------------------- */
extern const uint8_t sbtk_pack_start[], sbtk_pack_end[];

/* ---- state ------------------------------------------------------------------------------------------------- */
static tk_comp comp;
static int ready, baking, need_full, ticking, boosted;
static uint64_t last_tick, boot_t0;
/* the frame statistics of the current gesture (logged per film in the self test, per gesture otherwise) */
static struct { uint32_t n, nhalf, over16, maxus, nboost, nwait; uint64_t sumus, sumwait; uint32_t hist[64]; uint8_t ms[120]; } FS;

static void log_mb(const char *what, uint64_t v)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: ");
    ub_s(&u, what);
    ub_dec(&u, v >> 20);
    ub_s(&u, " MiB");
    sb_log(u.b);
}

static void boost(int on)
{
    if (on == boosted) return;
    boosted = on;
    if (!hw_dvfs(DVFS_A55, on ? 1794000U : 1053000U))
        sb_log(on ? "next: the A55 did not take 1794 MHz" : "next: the A55 did not go back to 1053 MHz");
}

/* the modem's status for the status bar, the Control Centre and Settings (kmodem_status: the session API v2's state;
 * a stall or crash restarting shows STARTING with "Restarting: ..." as its reason) */
static int has(const char *s, const char *w)
{
    for (; *s; s++) {
        int k = 0;
        while (w[k] && s[k] == w[k]) k++;
        if (!w[k]) return 1;
    }
    return 0;
}
static int cell_now(void)
{
    static sh_cell_t last = { -1, 0, 0, 0, 0 };
    struct sbos_phone ph;
    kmodem_status(&ph);
    sh_cell_t c = { SH_CELL_OFF, ph.sim, 0, 0, ph.bars };
    if (ph.state == SBOS_MODEM_ON) c.state = SH_CELL_ON;
    else if (ph.state == SBOS_MODEM_FAILED) c.state = SH_CELL_FAILED;
    else if (ph.state == SBOS_MODEM_PARKED) c.state = SH_CELL_GUARD;
    else if (ph.state == SBOS_MODEM_STARTING) c.state = has(ph.why, "Restarting") ? SH_CELL_RESTARTING : SH_CELL_STARTING;
    c.emergency_only = has(ph.cs, "emergency");
    c.rat = has(ph.cs, "LTE") ? 7 : has(ph.cs, "UMTS") ? 4 : has(ph.cs, "EDGE") ? 3 : has(ph.cs, "GPRS") ? 2 : has(ph.cs, "GSM") ? 1 : 0;
    sh_cell(&c);
    int changed = c.state != last.state || c.sim != last.sim || c.emergency_only != last.emergency_only ||
                  c.rat != last.rat || c.bars != last.bars;
    last = c;
    return changed;
}
/* the modem's Start / Stop from the new UI: the kernel's own path (its hold confirmation, its refusals: never in
 * a self-test image, not while Heat or Linux runs) */
int sh_modem(int on) { return kmodem_power(on); }
static void (*cf_done)(int);
static int next_confirm(const char *t, const char *l1, const char *l2, const char *l3, const char *hold, void (*done)(int))
{
    if (!ready) return 0;                        /* the bake: the classic confirmation (never: nothing asks then) */
    cf_done = done;
    sh_confirm(t, l1, l2, l3, hold);
    return 1;
}
void sh_confirmed(int ok)
{
    void (*d)(int) = cf_done;
    cf_done = 0;
    if (d) d(ok);
}
static void status_now(void)
{
    struct sbos_time t;
    struct sbos_battery b;
    int h = 0, mi = 0, wd = 0, d = 1, mo = 1;
    if (clock_now(&t)) { h = t.h; mi = t.mi; wd = t.wday; d = t.d; mo = t.mo; }
    battery_get(&b);
    int pct = b.ok ? b.pct : 0, temp = b.ok && b.temp_c10 > -1000 ? b.temp_c10 / 10 : 0;
    sh_status(h, mi, wd, d, mo, pct, temp);
    cell_now();
}

/* ---- tear-free frames (P2r): two scanout buffers and DECON flips ------------------------------------------
 * Before P2r the compositor wrote straight into the buffer DECON was sending to the panel: tearing. Now it writes
 * into the buffer DECON does not read and points DECON's IDMA channel 0 at it; the panel takes it at the next TE
 * (the shadow update), the same sequence as s9p-decon on Linux and sbwddm's T24 flips on Windows (3482 flips, no
 * fallback). S-Boot's pipeline is checked first (the full-screen window, channel 0 scanning 0xcc000000); anything
 * else, or a flip that does not latch in 50 ms with the screen on, keeps the one buffer (logged). Before a restart,
 * a power-off or the crash screen, DECON goes back to 0xcc000000 (the next OS and the crash screen expect it). */
#define DECON 0x16030000UL
#define DMA_CH0 0x16071000UL                         /* DPU_DMA channel 0 (S-Boot's fetch channel) */
#define SCAN_B 0xcd200000UL                          /* the second buffer: free RAM after S-Boot's framebuffer, in the
                                                      * non-cacheable window (mmu.c), below 4 GiB (32-bit base) */
#define SCAN_C 0xce400000UL                          /* P2t: the third (to 0xcf442800; reserved memory starts at
                                                      * 0xe1900000) */
static uint32_t scan_pa[3] = { SB_FB, SCAN_B, SCAN_C };
/* P2t, three buffers: DECON shows scan_shown, the last flip asked for scan_cur (queued until the next TE); a frame
 * goes into the third, so it never waits for the display (P2s waited 4-6 ms on average, 61-91 times a film: those
 * frames missed their TE). A flip asked while one is still queued replaces it (that frame is never shown: frames
 * come at most 16-17 ms apart, so only when the timer and the TE drift together). */
static int scan_shown, buf_fno[3] = { 0, -1, -1 };
static uint32_t scan_fno = 1;
static int flip_on, scan_cur, scan_win;
static unsigned flips, flip_waits, flip_timeouts, flips_replaced;
static int scan_next;
static uint32_t drd(uintptr_t a) { uint32_t v = *(volatile uint32_t *)a; __asm__ volatile("dmb oshld" ::: "memory"); return v; }
static void dwr(uintptr_t a, uint32_t v) { __asm__ volatile("dmb oshst" ::: "memory"); *(volatile uint32_t *)a = v; }
static int flip_pending(void) { return (drd(DECON + 0x60) & (1U << scan_win)) != 0; }
static void scan_point(uint32_t pa)
{
    __asm__ volatile("dsb sy" ::: "memory");        /* the frame's stores reach DRAM first */
    dwr(DMA_CH0 + 0x40, pa);
    dwr(DMA_CH0 + 0x44, pa);
    dwr(DECON + 0x60, 1U << scan_win);              /* the window's shadow update: taken at the next TE */
    __asm__ volatile("dsb sy" ::: "memory");
}
static void (*prev_before_reset)(void), (*prev_crash)(void);
static void scan_home(void)
{
    if (flip_on && (scan_cur || scan_shown)) scan_point(SB_FB);
    flip_on = 0;
    scan_cur = scan_shown = 0;
}
static void home_before_reset(void) { scan_home(); if (prev_before_reset) prev_before_reset(); }
static void home_crash(void) { scan_home(); if (prev_crash) prev_crash(); }
static int scan_identify(void)
{
    uint32_t path0 = drd(DECON + 0x214), end = (SB_FB_H - 1U) << 16 | (SB_FB_W - 1U);
    scan_win = -1;
    for (int w = 0; w < 6; w++)
        if ((path0 & (1U << (4 * w))) && drd(DECON + 0x1008 + 0x30 * w) == 0 && drd(DECON + 0x100c + 0x30 * w) == end)
            scan_win = w;
    if (scan_win < 0) return -2;
    if (drd(DMA_CH0 + 0x800 + 0x40) != SB_FB) return -3;
    if (drd(DMA_CH0 + 0x18) != (SB_FB_H << 16 | SB_FB_W)) return -4;
    if (drd(DMA_CH0 + 0x40) != SB_FB) return -5;
    return 0;
}
/* the compositor's acquire: the buffer DECON neither shows nor has queued, the oldest of those (no wait: three
 * buffers); its age in presented frames. A wait only if the bookkeeping says none is free (never expected). */
static uint32_t *scan_acquire(void *ctx, int *age)
{
    (void)ctx;
    if (!flip_on) { *age = 0; return (uint32_t *)SB_FB; }
    if (!flip_pending()) scan_shown = scan_cur;      /* the last flip latched */
    int b = -1;
    for (int i = 0; i < 3; i++)
        if (i != scan_shown && i != scan_cur && (b < 0 || buf_fno[i] < buf_fno[b])) b = i;
    if (b < 0) {                                     /* shown == queued == the only other: wait for the latch */
        flip_waits++;
        uint64_t t0 = sb_us();
        while (flip_pending() && sb_us() - t0 < 50000 && K.screen_on) sb_udelay(20);
        if (flip_pending() && K.screen_on) {
            flip_timeouts++;
            sb_log("next: flips: DECON did not latch in 50 ms; one buffer from now on (tearing possible)");
            scan_point(SB_FB);
            flip_on = 0;
            scan_cur = scan_shown = 0;
            comp.acquire = 0;
            comp.panel.px = (uint32_t *)SB_FB;
            need_full = 1;
            *age = 0;
            return (uint32_t *)SB_FB;
        }
        scan_shown = scan_cur;
        for (int i = 0; i < 3; i++) if (i != scan_shown && (b < 0 || buf_fno[i] < buf_fno[b])) b = i;
    }
    *age = buf_fno[b] < 0 ? 0 : (int)(scan_fno - 1 - (uint32_t)buf_fno[b]);
    scan_next = b;
    return (uint32_t *)(uintptr_t)scan_pa[b];
}
static void scan_init(void)
{
    int id = scan_identify();
    if (id) {
        sb_logf("next: flips: S-Boot's pipeline not as expected; one buffer (tearing possible): ", id == -2 ? "no full-screen window" :
                id == -3 ? "channel 0 not scanning 0xcc000000" : id == -4 ? "channel 0's size" : "channel 0's next address");
        return;
    }
    prev_before_reset = sb_before_reset;
    sb_before_reset = home_before_reset;
    prev_crash = sb_crash_hook;
    sb_crash_hook = home_crash;
    flip_on = 1;
    scan_cur = 0;
    comp.acquire = scan_acquire;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: flips: on (window ");
    ub_dec(&u, (uint64_t)scan_win);
    ub_s(&u, ", trigger 0x");
    ub_hexn(&u, drd(DECON + 0x70), 8);
    ub_s(&u, "); frames go to 0xcc000000 / 0xcd200000 / 0xce400000, shown at the panel's next TE");
    sb_log(u.b);
}

void next_kdraw(void);
static void present(void)
{
    if (comp.last_px) {
        if (flip_on && comp.acquire) {
            if (flip_pending()) flips_replaced++;    /* the queued frame is never shown */
            scan_cur = scan_next;
            buf_fno[scan_cur] = (int)scan_fno++;
            scan_point(scan_pa[scan_cur]);
            flips++;
        } else
            __asm__ volatile("dsb sy" ::: "memory");
        panel_kick();
    }
}

static void stats_reset(void) { tk_memset(&FS, 0, sizeof FS); }
static void stats_add(void)
{
    if (!comp.last_px) return;
    uint32_t us = (uint32_t)comp.last_us;
    FS.n++;
    FS.nhalf += comp.last_half;
    FS.nboost += boosted;
    if (us > 16700) {                             /* P2u: the frames over budget only, in every image (pmsg: 16 KB) */
        struct ub u;
        ub_init(&u);
        ub_s(&u, "next: f ");
        ub_dec(&u, FS.n);
        ub_s(&u, comp.last_half ? " half " : " full ");
        ub_dec(&u, (uint64_t)comp.last_px / 1000);
        ub_s(&u, "k px compose ");
        ub_dec(&u, comp.us_comp);
        ub_s(&u, " us present ");
        ub_dec(&u, comp.us_present);
        ub_s(&u, " us");
        /* P2u: where a slow frame's time went: the half canvas catching up, half copies made in the frame, the
         * catch-up of an older buffer (inside present), the rects, the A55's clock */
        ub_s(&u, " (idle ");
        ub_dec(&u, comp.us_idle);
        ub_s(&u, " halfs ");
        ub_dec(&u, comp.us_halfs);
        ub_s(&u, " catch ");
        ub_dec(&u, comp.us_catch);
        ub_s(&u, " rects ");
        ub_dec(&u, (uint64_t)comp.nd);
        ub_s(&u, " MHz ");
        ub_dec(&u, hw_a55_khz() / 1000);
        ub_s(&u, ")");
        if (comp.gpu) {                          /* P4: the GPU's own time in it, and the frames it gave back */
            ub_s(&u, " gpu ");
            ub_dec(&u, comp.us_gpu);
            ub_s(&u, " us (fallbacks ");
            ub_dec(&u, comp.gpu_fallbacks);
            /* v4: the quads, the bins' entries (a tile visits entries / 4185 on average), culled under opaque
             * layers, skipped over empty blocks, the GPU's clock */
            uint32_t ent = 0, cul = 0, emp = 0;
            gpucomp_last_stats(&ent, &cul, &emp);
            ub_s(&u, " q ");
            ub_dec(&u, comp.gpu_items);
            ub_s(&u, " ent ");
            ub_dec(&u, ent);
            ub_s(&u, " cul ");
            ub_dec(&u, cul);
            ub_s(&u, " emp ");
            ub_dec(&u, emp);
            ub_s(&u, " GPU MHz ");
            ub_dec(&u, gpucomp_mhz());
            ub_s(&u, ")");
        }
        sb_log(u.b);
    }
    FS.sumus += us;
    FS.sumwait += comp.us_wait;
    if (comp.us_wait > 1000) FS.nwait++;
    if (FS.n <= sizeof FS.ms) FS.ms[FS.n - 1] = (uint8_t)(us / 1000 > 99 ? 99 : (us + 500) / 1000);
    if (us > FS.maxus) FS.maxus = us;
    if (us > 16700) FS.over16++;
    unsigned b = us / 1000;
    FS.hist[b > 63 ? 63 : b]++;
}
static void stats_log(const char *what)
{
    if (!FS.n) return;
    uint32_t p90 = 0, k = 0;
    for (unsigned b = 0; b < 64; b++) { k += FS.hist[b]; if (k * 10 >= FS.n * 9) { p90 = b + 1; break; } }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: frames: ");
    ub_s(&u, what);
    ub_s(&u, ": ");
    ub_dec(&u, FS.n);
    ub_s(&u, " (");
    ub_dec(&u, FS.nhalf);
    ub_s(&u, " motion), mean ");
    ub_dec(&u, FS.sumus / FS.n / 1000);
    ub_c(&u, '.');
    ub_dec(&u, FS.sumus / FS.n / 100 % 10);
    ub_s(&u, " ms, p90 <= ");
    ub_dec(&u, p90);
    ub_s(&u, " ms, max ");
    ub_dec(&u, FS.maxus / 1000);
    ub_s(&u, " ms, over 16.7 ms: ");
    ub_dec(&u, FS.over16);
    ub_s(&u, "; at the top step: ");
    ub_dec(&u, FS.nboost);
    ub_s(&u, "; A55 kHz ");
    ub_dec(&u, hw_a55_khz());
    ub_s(&u, "; bands by core 0/1/2/3 ");
    for (int k = 0; k < 4; k++) { if (k) ub_c(&u, '/'); ub_dec(&u, worker_bands[k]); worker_bands[k] = 0; }
    ub_s(&u, "; heap ");
    ub_dec(&u, tk_heap_used() >> 20);
    ub_s(&u, " MiB (peak ");
    ub_dec(&u, tk_heap_peak() >> 20);
    ub_s(&u, "), largest free ");
    ub_dec(&u, tk_heap_largest() >> 20);
    ub_s(&u, " MiB");
    sb_log(u.b);
    /* every frame's work in ms, in order (P2r: the numbers the main session sends back; 60 a line), then the
     * waits for the display (the last flip latching at the panel's TE) */
#ifndef SBOS_SELFTEST_NEXT                         /* the self test's eleven films would fill pmsg's 16 KB */
    uint32_t nf = FS.n < sizeof FS.ms ? FS.n : (uint32_t)sizeof FS.ms;
    for (uint32_t i0 = 0; i0 < nf; i0 += 60) {
        ub_init(&u);
        ub_s(&u, "next: frame ms: ");
        for (uint32_t i = i0; i < nf && i < i0 + 60; i++) { if (i > i0) ub_c(&u, ' '); ub_dec(&u, FS.ms[i]); }
        if (i0 + 60 >= nf && FS.n > nf) ub_s(&u, " ...");
        sb_log(u.b);
    }
#endif
    ub_init(&u);
    ub_s(&u, "next: display waits > 1 ms: ");
    ub_dec(&u, FS.nwait);
    ub_s(&u, ", mean wait ");
    ub_dec(&u, FS.sumwait / FS.n / 1000);
    ub_c(&u, '.');
    ub_dec(&u, FS.sumwait / FS.n / 100 % 10);
    ub_s(&u, " ms; flips ");
    ub_dec(&u, flips);
    ub_s(&u, flip_on ? " (three buffers), replaced while queued " : " (one buffer)");
    if (flip_on) ub_dec(&u, flips_replaced);
    if (flip_timeouts) { ub_s(&u, "; latch timeouts "); ub_dec(&u, flip_timeouts); }
    sb_log(u.b);
}

/* one frame: the springs and transitions advanced by the real time since the last, composed, presented */
static int slept_now;                        /* the shell's fade to black ended: the screen off after its frame */
void sh_slept(void) { slept_now = 1; }
static uint64_t tick3;                      /* the next frame's time in thirds of a ms: 16.67 ms apart (the TE's) */
static void frame(void)
{
    /* the next frame first (a one-shot at the next 16.67 ms step: P2t, the panel's rate, so with three buffers
     * a frame is neither replaced while queued nor shown twice, except as the two clocks drift) */
    {
        uint64_t n3 = sb_ms() * 3;
        if (!tick3) tick3 = n3;
        tick3 += 50;
        if (tick3 <= n3) tick3 = n3 + 50;           /* fell behind: from now */
        kt_set(KT_UI, 20, tick3 / 3, 0, 0);
    }
    if (!boosted) boost(1);                      /* a wake: the kernel set the screen-on clock after the hook */
    uint64_t now = sb_ms();
    float dt = last_tick ? (float)(now - last_tick) / 1000.0f : 1.0f / 60;
    last_tick = now;
    int mv = sh_tick(dt, (float)now);
    int motion = mv || sh_moving();
    tk_comp_frame(&comp, motion, 0);
    present();
    stats_add();
    if (slept_now) {
        slept_now = 0;
        power_screen_set(0);                     /* the black frame is out (its flip at the next TE) */
        return;
    }
    if (!motion) {
        kt_cancel(KT_UI, 20);
        ticking = 0;
        last_tick = 0;
        sh_idle();
        if (sh_idle_pending()) kt_set(KT_UI, 24, sb_ms(), 40, 0);   /* the rest of the idle work, a turn at a time */
        kt_set(KT_UI, 21, sb_ms(), 400, 0);         /* back to the screen-on clock unless something moves again */
        kt_set(KT_UI, 23, sb_ms(), 5000, 0);        /* and the band workers off after 5 s */
#ifndef SBOS_SELFTEST_NEXT
        stats_log("gesture");
        stats_reset();
#endif
    }
}

static void start_ticking(void)
{
    kt_cancel(KT_UI, 21);
    kt_cancel(KT_UI, 23);
    boost(1);
    workers_start();
    if (ticking) return;
    ticking = 1;
    last_tick = sb_ms();
    tick3 = 0;
    kt_set(KT_UI, 20, sb_ms() + 16, 0, 0);
}

/* ---- sleep and wake (P2r): the Power key's screen off is the shell's fade to black first, the screen on its fade
 * in from black; the classic UI's black frame never runs (Next's buffers are its own) ---- */
static int next_sleep(int key)
{
    if (!ready) return 0;
    if (sh_sleeping()) {
        if (key) sh_wake();                          /* Power again during the fade: back */
    } else
        sh_sleep(0);
    start_ticking();
    return 1;
}
static void next_screen(int on)
{
    if (!ready) return;
    if (!on) {
        kt_cancel(KT_UI, 20);
        ticking = 0;
        last_tick = 0;
        boosted = 0;                                 /* the kernel takes the A55 down with the screen */
        kt_set(KT_UI, 23, sb_ms(), 5000, 0);         /* the band workers off in 5 s */
        if (!sh_sleeping()) {
            sh_sleep(1);                             /* not through the fade (a timer path): black at once */
            tk_comp_frame(&comp, 0, 0);
            present();
        }
        sh_idle();                                   /* P2v: the half canvas and the lock's caches now, in the dark
                                                      * (the wake's first frame spent 8 ms on them) */
#ifndef SBOS_SELFTEST_NEXT
        stats_reset();
#endif
        return;
    }
    status_now();
    sh_wake();
    boosted = 0;                                     /* the kernel sets the screen-on clock after this: the first
                                                      * frame takes the top step (frame()) */
    workers_start();
    ticking = 1;
    last_tick = 0;
    tick3 = 0;
    kt_set(KT_UI, 20, sb_ms() + 16, 0, 0);
}

/* ---- P4: the GPU compositor (kern/gpucomp.c): the frames on the Mali-G72, full resolution ---------------------- */
_Static_assert(sizeof(tk_gpu_item) == sizeof(struct gc_quad), "tk_gpu_item is gpucomp's gc_quad");
static int gpu_started;
/* the wallpaper at full resolution in the GPU's pool, made once per ground (base_half 2x, bilinear, on the GPU):
 * every frame then reads it 1:1 (one load a pixel instead of a bilinear's four) */
static uint32_t wall_gen, wall_seen, wall_made;
static int wall_ok, wall_still;
static tk_gpu_item gx[TK_GPU_MAX];
_Static_assert(sizeof(tk_irect) == sizeof(struct gc_rect), "tk_irect is gpucomp's gc_rect");
static int next_gpu(void *ctx, uint32_t *dst, const tk_gpu_item *it, int n, const tk_irect *dmg, int ndmg, uint64_t *us)
{
    (void)ctx;
    const tk_gpu_item *use = it;
    uint32_t *wall = (uint32_t *)gpucomp_pool();
    if (n > 0 && n <= TK_GPU_MAX && it[0].src == comp.base_half.px && gpucomp_pool_bytes() >= (uint64_t)TK_W * TK_H * 4U) {
        /* a ground that changes every frame (Emergency's cross-fade) is read bilinearly as it is; the copy is made
         * once it has kept still for two frames */
        if (comp.ground_gen != wall_seen) { wall_seen = comp.ground_gen; wall_still = 0; }
        else if (wall_still < 2) wall_still++;
        if ((!wall_ok || wall_gen != comp.ground_gen) && wall_still >= 2) {
            uint64_t u2 = 0;
            wall_ok = gpucomp_compose(wall, 0xff000000U, (const struct gc_quad *)it, 1, &u2);
            wall_gen = comp.ground_gen;
            if (wall_ok && !wall_made++) sb_logx("next: gpu: the wallpaper at full resolution (the first), us: ", u2);
        }
        if (wall_ok && wall_gen == comp.ground_gen) {
            tk_memcpy(gx, it, sizeof(tk_gpu_item) * (size_t)n);
            gx[0].src = wall;
            gx[0].stride = TK_W;
            gx[0].w = TK_W;
            gx[0].h = TK_H;
            gx[0].flags = 0;
            gx[0].isx = gx[0].isy = 1;
            use = gx;
        }
    }
    int ok = gpucomp_compose_damage(dst, 0xff000000U, (const struct gc_quad *)use, (unsigned)n, (const struct gc_rect *)dmg,
                                    (unsigned)(ndmg > 0 ? ndmg : 0), us);
    if (!ok && !gpucomp_ready()) {               /* a fault (gpucomp logged it, the GPU is off): the CPU from now on */
        comp.gpu = 0;
        sb_log("next: the GPU stopped: the CPU composes the frames from now on");
    }
    return ok;
}
static void next_gpu_clean(void *ctx, const void *p, uint64_t n)
{
    (void)ctx;
    gpucomp_clean(p, n);
}

/* ---- the kernel's UI entry points in this image ------------------------------------------------------------- */
void next_kinit(void)
{
    boot_t0 = sb_ms();
    log_mb("arena free before the UI heap: ", sb_arena_free());
    heap_init(160ULL << 20);
    log_mb("UI heap: ", heap_size);
    kui_confirm_hook = next_confirm;             /* the kernel's hold confirmations (the modem's Start) in this UI */
    kui_screen_hook = next_screen;               /* P2r: sleep and wake as animations */
    kui_sleep_hook = next_sleep;
    if (!tk_pack_open(sbtk_pack_start, (uint32_t)(sbtk_pack_end - sbtk_pack_start))) {
        sb_log("next: the sbtk pack is missing or bad: the regular UI runs");
        return;
    }
    if (!tk_comp_init(&comp, ui_back(), (uint32_t *)SB_FB)) {
        sb_log("next: no memory for the compositor: the regular UI runs");
        return;
    }
    scan_init();
#ifndef SBOS_NEXT_NOGPU
    /* P4: with two buffers (tear-free flips) and the GPU up (normal clocks, gpuheat's checks), the GPU composes */
    gpucomp_add_ro(sbtk_pack_start, (uint64_t)(sbtk_pack_end - sbtk_pack_start));  /* icons point into the pack */
    if (flip_on && heap_mem && gpucomp_init(heap_mem, heap_size)) {
        comp.gpu = next_gpu;
        comp.gpu_clean = next_gpu_clean;
        gpu_started = 1;
        /* 572 MHz, a step of the vendor's table (ACPM sets its own voltage): a full frame in the time budget; the
         * GPU idles clock-gated between frames */
        sb_log(hw_dvfs(DVFS_GPU, 572000U) ? "next: the GPU at 572 MHz (the vendor table's step)" :
                                                  "next: 572 MHz not taken: the GPU stays at the boot clock");
        sb_log("next: the GPU composes the frames (Mali-G72 compute jobs, full resolution; the CPU if it refuses)");
    } else {
        sb_log(flip_on ? "next: the CPU composes the frames (the GPU did not come up)" :
                         "next: the CPU composes the frames (one buffer: no GPU frames without flips)");
    }
#endif
#ifndef SBOS_NEXT_ONECORE
    comp.par = kpar;
    tk_par = kpar;                                   /* the bakes' row loops too */
#endif
    /* the bake one step per loop turn (the watchdog is fed between them; the screen fills in); the top step from
     * the second step on, once the kernel's interrupts and ACPM are up */
    sh_init_begin(&comp);
    (void)sh_init_step();                            /* the wallpaper */
    baking = need_full = 1;
    kt_set(KT_UI, 22, sb_ms(), 1, 1);
}

static void bake_step(void)
{
    hw_wdt_feed();
    boost(1);
    workers_start();
    if (sh_init_step()) {
        tk_comp_frame(&comp, 0, 1);
        present();
        return;
    }
    kt_cancel(KT_UI, 22);
    baking = 0;
    status_now();
    sh_bright_now(panel_bright_level);
    ready = need_full = 1;
    boost(0);
    kt_set(KT_UI, 23, sb_ms(), 5000, 0);            /* the workers off after 5 s unless a finger comes */
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: ready; the bake took ");
    ub_dec(&u, sb_ms() - boot_t0);
    ub_s(&u, " ms");
    sb_log(u.b);
    log_mb("UI heap in use: ", tk_heap_used());
    next_kdraw();
    sh_idle();                                       /* the unlock's cache and the half copies, before a finger */
    log_mb("UI heap with the caches: ", tk_heap_used());
}

void next_kdraw(void)
{
    if (!ready && !baking) { ui_kdraw(); return; }
    if (!K.screen_on) return;
    if (need_full) {
        need_full = 0;
        tk_comp_frame(&comp, 0, 1);
        present();
        if (!K.first_frame_ms) K.first_frame_ms = sb_ms();
    }
    /* the modem's status twice a second (a copy of a few hundred bytes); a change is drawn at once */
    static uint64_t cell_t;
    if (ready && sb_ms() - cell_t >= 500) {
        cell_t = sb_ms();
        if (cell_now() && !ticking) {
            tk_comp_frame(&comp, 0, 0);
            present();
            sh_idle();
        }
    }
}

void next_kevent(const void *evp)
{
    if (baking) return;                              /* touches before the lock screen is whole: ignored */
    if (!ready) { ui_kevent(evp); return; }
    const struct kin *e = evp;
    int ph = e->type == KIN_PRESS ? 0 : e->type == KIN_MOVE ? 1 : e->type == KIN_RELEASE ? 2 : -1;
    if (ph < 0) return;
    sh_touch(ph, e->x / TK_PT, e->y / TK_PT, (float)sb_ms());
    start_ticking();
}

void next_ktimer(unsigned id)
{
    if (baking && id == 22) { bake_step(); return; }
    if (ready && id == 20) { frame(); return; }
    if (ready && id == 21) {
        if (!ticking && !sh_moving()) boost(0);
        return;
    }
    if (ready && id == 24) {
        if (!ticking && !sh_moving()) {
            sh_idle();
            if (sh_idle_pending()) kt_set(KT_UI, 24, sb_ms(), 40, 0);
        } else
            kt_set(KT_UI, 24, sb_ms(), 200, 0);
        return;
    }
    if (ready && id == 23) {
        if (!ticking && !sh_moving()) workers_stop();
        return;
    }
    ui_ktimer(id);
}

void next_kminute(void)
{
    if (baking) return;
    if (!ready) { ui_kminute(); return; }
    status_now();
    if (!ticking) {
        tk_comp_frame(&comp, 0, 0);
        present();
        sh_idle();
    }
}

/* slide to start (shell_lock.c): the loader's one-shot next boot */
void sh_boot_os(const char *key)
{
#ifdef SBOS_SELFTEST_NEXT
    sb_logf("next: slide to start: would restart into (a dry run: the self test): ", key);
#else
    sb_logf("next: slide to start: restarting into ", key);
    power_boot_os(key);
#endif
}

void sh_brightness(int level)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)level;
    sb_log("next: brightness: would set the panel's level (a dry run: the self test)");
#else
    if (!panel_brightness(level)) sb_log("next: brightness: the panel did not take the level");
#endif
}

void sh_torch(int lit)
{
#ifdef SBOS_SELFTEST_NEXT
    sb_log(lit ? "next: torch: would light (a dry run: the self test)" : "next: torch: would go off (a dry run)");
#else
    int r = torch_set(lit);
    if (r < 0) sb_log("next: torch: the LED did not answer");
#endif
}

/* ---- the self test --------------------------------------------------------------------------------------- */
#ifdef SBOS_SELFTEST_NEXT
typedef struct { int kind; float x0, y0, x1, y1, ms; } step;
static const char *const FILM_NAME[11] = { "unlock", "group", "focus", "holds", "pages", "cc", "app", "phone", "messages", "emergency", "sleep" };
/* tools/films.txt's scripts (pt, ms) */
static const step S_UNLOCK[] = { { 0, 0, 0, 0, 0, 400 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 1300 } };
static const step S_GROUP[] = { { 0, 0, 0, 0, 0, 300 }, { 1, 195, 354, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1000 }, { 1, 195, 354, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 900 }, { 2, 200, 650, 200, 380, 250 }, { 0, 0, 0, 0, 0, 1200 }, { 1, 195, 578, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 1800 } };
static const step S_FOCUS[] = { { 0, 0, 0, 0, 0, 300 }, { 1, 195, 444, 0, 0, 80 }, { 0, 0, 0, 0, 0, 900 }, { 1, 195, 506, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 1000 }, { 2, 65, 412, 345, 412, 550 }, { 0, 0, 0, 0, 0, 2600 } };
static const step S_HOLDS[] = { { 0, 0, 0, 0, 0, 300 }, { 3, 72, 720, 0, 0, 650 }, { 0, 0, 0, 0, 0, 700 }, { 3, 318, 720, 0, 0, 950 },
                                { 0, 0, 0, 0, 0, 900 } };
static const step S_PAGES[] = { { 0, 0, 0, 0, 0, 300 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 900 },
                                { 2, 320, 450, 60, 450, 200 }, { 0, 0, 0, 0, 0, 900 }, { 2, 60, 450, 330, 460, 220 }, { 0, 0, 0, 0, 0, 1200 } };
static const step S_CC[] = { { 0, 0, 0, 0, 0, 300 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 900 }, { 2, 320, 20, 320, 330, 260 },
                             { 0, 0, 0, 0, 0, 900 }, { 1, 70, 290, 0, 0, 80 }, { 0, 0, 0, 0, 0, 500 }, { 2, 240, 230, 240, 120, 300 },
                             { 0, 0, 0, 0, 0, 600 }, { 1, 195, 600, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1000 } };
static const step S_APP[] = { { 0, 0, 0, 0, 0, 300 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 900 }, { 1, 158, 290, 0, 0, 80 },
                              { 0, 0, 0, 0, 0, 1100 }, { 2, 195, 795, 200, 450, 260 }, { 0, 0, 0, 0, 0, 1200 } };
static const step S_PHONE[] = { { 0, 0, 0, 0, 0, 300 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 900 }, { 1, 65, 736, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 1000 }, { 1, 91, 221, 0, 0, 80 }, { 0, 0, 0, 0, 0, 300 }, { 1, 91, 221, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 300 }, { 1, 195, 221, 0, 0, 80 }, { 0, 0, 0, 0, 0, 300 }, { 1, 195, 605, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 2600 }, { 1, 150, 749, 0, 0, 80 }, { 0, 0, 0, 0, 0, 600 }, { 2, 195, 795, 200, 450, 260 },
                                { 0, 0, 0, 0, 0, 1200 } };
static const step S_MSGS[] = { { 0, 0, 0, 0, 0, 300 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 900 }, { 1, 152, 736, 0, 0, 80 },
                               { 0, 0, 0, 0, 0, 1000 }, { 1, 195, 204, 0, 0, 80 }, { 0, 0, 0, 0, 0, 800 }, { 1, 50, 63, 0, 0, 80 },
                               { 0, 0, 0, 0, 0, 600 }, { 2, 195, 795, 200, 450, 260 }, { 0, 0, 0, 0, 0, 1200 } };
static const step S_EMERG[] = { { 0, 0, 0, 0, 0, 300 }, { 3, 318, 720, 0, 0, 950 }, { 0, 0, 0, 0, 0, 1200 }, { 2, 195, 795, 195, 330, 220 },
                                { 0, 0, 0, 0, 0, 900 }, { 2, 320, 450, 60, 450, 200 }, { 0, 0, 0, 0, 0, 900 }, { 2, 80, 20, 80, 640, 300 },
                                { 0, 0, 0, 0, 0, 1200 }, { 1, 195, 650, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1200 } };
/* P2r: the Power key (kind 5, the kernel's own power_key_short): the fade to black and the screen off, the wake, a
 * fade undone by a second press */
static const step S_SLEEP[] = { { 0, 0, 0, 0, 0, 300 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 900 }, { 1, 158, 290, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 1100 }, { 5, 0, 0, 0, 0, 20 }, { 0, 0, 0, 0, 0, 900 }, { 5, 0, 0, 0, 0, 20 },
                                { 0, 0, 0, 0, 0, 1200 }, { 5, 0, 0, 0, 0, 20 }, { 0, 0, 0, 0, 0, 120 }, { 5, 0, 0, 0, 0, 20 },
                                { 0, 0, 0, 0, 0, 1000 } };
static struct { const step *s; int n, i, down; uint64_t t0; float s0; int film; } F;
int next_selftest_flips_ok(void) { return flip_on && flips > 0 && !flip_timeouts; }
static int st_started, st_failed, clock_ok;
int next_selftest_clock_ok(void) { return clock_ok; }
static uint64_t st_t0;
int next_selftest_failed(void) { return st_failed || heap_full; }

static void film_start(int film)
{
    F.film = film;
    F.i = 0;
    F.down = 0;
    F.s0 = 0;
    F.t0 = sb_ms();
    switch (film) {
    case 0: F.s = S_UNLOCK; F.n = 3; break;
    case 1: F.s = S_GROUP; F.n = 9; break;
    case 2: F.s = S_FOCUS; F.n = 7; break;
    case 3: F.s = S_HOLDS; F.n = 5; break;
    case 4: F.s = S_PAGES; F.n = 7; break;
    case 5: F.s = S_CC; F.n = 11; break;
    case 6: F.s = S_APP; F.n = 7; break;
    case 7: F.s = S_PHONE; F.n = 17; break;
    case 8: F.s = S_MSGS; F.n = 11; break;
    case 9: F.s = S_EMERG; F.n = 11; break;
    default: F.s = S_SLEEP; F.n = 13; break;
    }
    /* each film from the plain lock screen */
    sh_set("reset", "", 0);
    need_full = 1;
    next_kdraw();
    for (int i = 0; i < 8 && sh_idle_pending(); i++) { hw_wdt_feed(); sh_idle(); }   /* as the idle time would */
    sh_idle();
    {   /* P2w: a marker for the camera (the main session times its shots from the films' starts) */
        struct ub u;
        ub_init(&u);
        ub_s(&u, "next: film ");
        ub_s(&u, FILM_NAME[film]);
        ub_s(&u, " starts at +");
        ub_dec(&u, sb_ms() - st_t0);
        ub_s(&u, " ms (since the self test's first step)");
        sb_log(u.b);
    }
    stats_reset();
}

#ifndef SBOS_NEXT_NOGPU
/* P4: the GPU's frame against the CPU's, the lock screen at rest: the CPU draws it whole into one buffer, the GPU
 * into the other, and every 8th pixel of every 8th row is compared (known small differences: the GPU's anti-aliased
 * rounded corners and its bilinear wallpaper). Fails on a mean error over 8 levels or over 5 % of points far off */
static void gpu_vs_cpu(void)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: gpu: ");
    ub_dec(&u, comp.gpu_frames);
    ub_s(&u, " frames on the GPU, ");
    ub_dec(&u, comp.gpu_fallbacks);
    ub_s(&u, " given back to the CPU; full-resolution wallpapers made ");
    ub_dec(&u, wall_made);
    sb_log(u.b);
    if (!gpu_started) {
        sb_log("selftest: FAILED: next: gpu: the GPU did not come up (see the gpucomp lines)");
        st_failed = 1;
        return;
    }
    if (!comp.gpu || !gpucomp_ready()) {
        sb_log("selftest: FAILED: next: gpu: the GPU stopped during the films (a fault: see the gpucomp lines)");
        st_failed = 1;
        return;
    }
    if (!K.screen_on) power_screen_set(1);
    sh_set("reset", "", 0);
    __typeof__(comp.gpu) g = comp.gpu;
    comp.gpu = 0;
    tk_comp_frame(&comp, 0, 1);
    present();
    uint64_t cpu_us = comp.last_us;
    int cpu_buf = scan_cur;
    comp.gpu = g;
    uint32_t gf = comp.gpu_frames;
    tk_comp_frame(&comp, 0, 1);
    present();
    hw_wdt_feed();
    if (scan_cur == cpu_buf || !comp.last_px || comp.gpu_frames != gf + 1) {
        sb_log("selftest: FAILED: next: gpu: the GPU did not draw the comparison frame");
        st_failed = 1;
        return;
    }
    const volatile uint32_t *A = (const volatile uint32_t *)(uintptr_t)scan_pa[cpu_buf];
    const volatile uint32_t *B = (const volatile uint32_t *)(uintptr_t)scan_pa[scan_cur];
    uint64_t sum = 0, n = 0, far = 0;
    int fx = -1, fy = -1;
    uint32_t fa = 0, fb = 0;
    for (int y = 4; y < TK_H; y += 8)
        for (int x = 4; x < TK_W; x += 8) {
            uint32_t a = A[(uint32_t)y * TK_W + (uint32_t)x], b = B[(uint32_t)y * TK_W + (uint32_t)x];
            int m = 0;
            for (int k = 0; k < 24; k += 8) {
                int d = (int)(a >> k & 255) - (int)(b >> k & 255);
                if (d < 0) d = -d;
                sum += (uint64_t)d;
                if (d > m) m = d;
            }
            n += 3;
            if (m > 48) {
                if (!far) { fx = x; fy = y; fa = a; fb = b; }
                far++;
            }
        }
    uint64_t pts = n / 3, mean100 = sum * 100 / n;
    int bad = mean100 > 800 || far * 20 > pts;
    ub_init(&u);
    ub_s(&u, bad ? "selftest: FAILED: next: gpu: the GPU's lock screen is not the CPU's: mean error " :
                   "next: gpu: the GPU's lock screen matches the CPU's: mean error ");
    ub_dec(&u, mean100 / 100);
    ub_s(&u, ".");
    ub_dec(&u, mean100 % 100 / 10);
    ub_s(&u, " levels, far off ");
    ub_dec(&u, far);
    ub_s(&u, " of ");
    ub_dec(&u, pts);
    ub_s(&u, " points; the whole frame: CPU ");
    ub_dec(&u, cpu_us);
    ub_s(&u, " us, GPU ");
    ub_dec(&u, comp.us_gpu);
    ub_s(&u, " us");
    sb_log(u.b);
    if (far) {
        ub_init(&u);
        ub_s(&u, "next: gpu: the first far point x ");
        ub_dec(&u, (uint64_t)fx);
        ub_s(&u, " y ");
        ub_dec(&u, (uint64_t)fy);
        ub_s(&u, ": CPU 0x");
        ub_hexn(&u, fa, 8);
        ub_s(&u, ", GPU 0x");
        ub_hexn(&u, fb, 8);
        sb_log(u.b);
    }
    if (bad) st_failed = 1;
}
#endif

/* every 8 ms (the touch controller's rate is about 120 Hz): the finger's next position; 1 while the film runs */
int next_selftest_step(void)
{
    hw_wdt_feed();
    if (!st_t0) st_t0 = sb_ms();
    if (sb_ms() - st_t0 > 150000) {                  /* a stall must not keep the unattended phone here */
        sb_logx("selftest: FAILED: next: STALLED after 150 s; ready / film / step ", (uint64_t)ready << 32 | (uint64_t)F.film << 16 | (uint64_t)F.i);
        st_failed = 1;
        return 0;
    }
    if (!ready) return 1;                            /* the bake is still running */
    if (!st_started) {
        st_started = 1;
        /* the clock: a new minute must reach the panel through an ordinary frame (P2a-P2g kept the old one) */
        {
            sh_status(12, 34, 3, 11, 11, 50, 30);
            tk_comp_frame(&comp, 0, 0);
            present();
            sb_logx(comp.last_px ? "next: clock check: the new minute drawn, px: " : "selftest: FAILED: next: clock check: nothing redrawn ", (uint64_t)comp.last_px);
            if (!comp.last_px) st_failed = 1;
            clock_ok = comp.last_px != 0;
            status_now();
        }
        sb_log("selftest: next: the eleven films (unlock, group, focus, holds, pages, cc, app, phone, messages, emergency, sleep) with synthetic touches; sleep presses the real Power path");
        film_start(0);
        return 1;
    }
    float t = (float)(sb_ms() - F.t0);
    while (F.i < F.n && t >= F.s0 + F.s[F.i].ms) {
        const step *p = &F.s[F.i];
        if (F.down && p->kind == 5) F.down = 0;
        else if (F.down) {
            sh_touch(2, p->kind == 2 ? p->x1 : p->x0, p->kind == 2 ? p->y1 : p->y0, (float)sb_ms());
            F.down = 0;
            start_ticking();
        }
        F.s0 += p->ms;
        F.i++;
    }
    if (F.i < F.n) {
        const step *p = &F.s[F.i];
        if (p->kind == 5) {
            if (!F.down) {
                F.down = 1;
                sb_log(K.screen_on ? "next: sleep film: Power (the screen is on)" : "next: sleep film: Power (the screen is off)");
                power_key_short();
            }
        } else if (p->kind) {
            if (!F.down) { sh_touch(0, p->x0, p->y0, (float)sb_ms()); F.down = 1; start_ticking(); }
            else if (p->kind == 2) {
                float u = (t - F.s0) / p->ms;
                sh_touch(1, p->x0 + (p->x1 - p->x0) * u, p->y0 + (p->y1 - p->y0) * u, (float)sb_ms());
            }
        }
        return 1;
    }
    if (ticking || sh_moving()) return 1;          /* let it settle */
    stats_log(FILM_NAME[F.film]);
    if (F.film == 10 && !K.screen_on) {
        sb_log("selftest: FAILED: next: the sleep film ended with the screen off");
        st_failed = 1;
        power_screen_set(1);
    }
    if (F.film < 10) { film_start(F.film + 1); return 1; }
#ifndef SBOS_NEXT_NOGPU
    gpu_vs_cpu();
#endif
    {
        /* the present's kernels against uibench's loops, on these buffers and cores (boosted, workers on) */
        static const char *const BN[5] = { "the motion present (2x, rows in pairs)", "the rest copy (tk_memcpy rows)",
                                           "uibench's copy", "uibench's 2x", "the half canvas following (2x2 down)" };
        if (heap_full) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "selftest: FAILED: next: the UI heap was full ");
            ub_dec(&u, heap_full);
            ub_s(&u, " times (parts of the screen stayed empty)");
            sb_log(u.b);
        }
        boost(1);
        workers_start();
        for (int k = 0; k < 5; k++) {
            hw_wdt_feed();
            uint64_t us = tk_comp_bench(&comp, k);
            struct ub u;
            ub_init(&u);
            ub_s(&u, "next: bench: ");
            ub_s(&u, BN[k]);
            ub_s(&u, ": ");
            ub_dec(&u, us);
            ub_s(&u, " us");
            sb_log(u.b);
        }
    }
    return 0;
}

void next_selftest_begin(void)
{
    if (!ready && !baking) { sb_log("selftest: FAILED: next: the new UI did not start"); return; }
    st_started = 0;
}
int next_selftest_ready(void) { return ready || baking; }
#endif
