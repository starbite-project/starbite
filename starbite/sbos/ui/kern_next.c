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
}
void tk_heap_oom(uint64_t n, uint64_t used)
{
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
static struct { uint32_t n, nhalf, over16, maxus, nboost; uint64_t sumus; uint32_t hist[64]; } FS;
static int frame_log;                        /* the self test: one line per frame of the films it names */

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

static void status_now(void)
{
    struct sbos_time t;
    struct sbos_battery b;
    int h = 0, mi = 0, wd = 0, d = 1, mo = 1;
    if (clock_now(&t)) { h = t.h; mi = t.mi; wd = t.wday; d = t.d; mo = t.mo; }
    battery_get(&b);
    int pct = b.ok ? b.pct : 0, temp = b.ok && b.temp_c10 > -1000 ? b.temp_c10 / 10 : 0;
    sh_status(h, mi, wd, d, mo, pct, temp);
}

void next_kdraw(void);
static void present(void)
{
    if (comp.last_px) {
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
    if (frame_log) {
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
        sb_log(u.b);
    }
    FS.sumus += us;
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
    ub_s(&u, ")");
    sb_log(u.b);
}

/* one frame: the springs and transitions advanced by the real time since the last, composed, presented */
static void frame(void)
{
    uint64_t now = sb_ms();
    float dt = last_tick ? (float)(now - last_tick) / 1000.0f : 1.0f / 60;
    last_tick = now;
    int mv = sh_tick(dt, (float)now);
    int motion = mv || sh_moving();
    tk_comp_frame(&comp, motion, 0);
    present();
    stats_add();
    if (!motion) {
        kt_cancel(KT_UI, 20);
        ticking = 0;
        last_tick = 0;
        sh_idle();
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
    kt_set(KT_UI, 20, sb_ms(), 16, 1);
}

/* ---- the kernel's UI entry points in this image ------------------------------------------------------------- */
void next_kinit(void)
{
    boot_t0 = sb_ms();
    log_mb("arena free before the UI heap: ", sb_arena_free());
    heap_init(160ULL << 20);
    log_mb("UI heap: ", heap_size);
    if (!tk_pack_open(sbtk_pack_start, (uint32_t)(sbtk_pack_end - sbtk_pack_start))) {
        sb_log("next: the sbtk pack is missing or bad: the regular UI runs");
        return;
    }
    if (!tk_comp_init(&comp, ui_back(), (uint32_t *)SB_FB)) {
        sb_log("next: no memory for the compositor: the regular UI runs");
        return;
    }
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
static const char *const FILM_NAME[7] = { "unlock", "group", "focus", "holds", "pages", "cc", "app" };
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
static struct { const step *s; int n, i, down; uint64_t t0; float s0; int film; } F;
static int st_started, st_failed;
static uint64_t st_t0;
int next_selftest_failed(void) { return st_failed; }

static void film_start(int film)
{
    F.film = film;
    F.i = 0;
    F.down = 0;
    F.s0 = 0;
    F.t0 = sb_ms();
    frame_log = film == 0 || film == 2 || film == 6;          /* the unlock and the focus sheet: every frame's split */
    switch (film) {
    case 0: F.s = S_UNLOCK; F.n = 3; break;
    case 1: F.s = S_GROUP; F.n = 9; break;
    case 2: F.s = S_FOCUS; F.n = 7; break;
    case 3: F.s = S_HOLDS; F.n = 5; break;
    case 4: F.s = S_PAGES; F.n = 7; break;
    case 5: F.s = S_CC; F.n = 11; break;
    default: F.s = S_APP; F.n = 7; break;
    }
    /* each film from the plain lock screen */
    sh_set("reset", "", 0);
    need_full = 1;
    next_kdraw();
    stats_reset();
}

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
            status_now();
        }
        sb_log("selftest: next: the seven films (unlock, group, focus, holds, pages, cc, app) with synthetic touches");
        film_start(0);
        return 1;
    }
    float t = (float)(sb_ms() - F.t0);
    while (F.i < F.n && t >= F.s0 + F.s[F.i].ms) {
        const step *p = &F.s[F.i];
        if (F.down) {
            sh_touch(2, p->kind == 2 ? p->x1 : p->x0, p->kind == 2 ? p->y1 : p->y0, (float)sb_ms());
            F.down = 0;
            start_ticking();
        }
        F.s0 += p->ms;
        F.i++;
    }
    if (F.i < F.n) {
        const step *p = &F.s[F.i];
        if (p->kind) {
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
    frame_log = 0;
    if (F.film < 6) { film_start(F.film + 1); return 1; }
    {
        /* the present's kernels against uibench's loops, on these buffers and cores (boosted, workers on) */
        static const char *const BN[5] = { "the motion present (2x, centred)", "the rest copy (tk_memcpy rows)",
                                           "uibench's copy", "uibench's 2x", "the half canvas following (2x2 down)" };
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
