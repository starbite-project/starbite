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
#include "survival.h"
#include "../guide/sbguide.h"

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
static int workers_on, workers_ever;   /* workers_ever: a launch has worked (P3o: the self test checks it) */
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
    if (any) workers_ever = 1;
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
static struct { uint32_t n, nhalf, over16, maxus, nboost, nwait; uint64_t sumus, sumwait; uint32_t hist[64]; uint8_t ms[120];
                 uint32_t shmax, shover; } FS;   /* P3i: the shell's own time (a touch's handling, a tick) */

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
    static sh_cell_t last = { -1, 0, 0, 0, 0, 0, "", "" };
    struct sbos_phone ph;
    kmodem_status(&ph);
    sh_cell_t c = { SH_CELL_OFF, ph.sim, 0, 0, ph.bars, 0, "", "" };
    struct kcell kc;
    kmodem_cell(&kc);
    c.data = kc.state;                      /* KCELL_* = SH_DATA_* */
    sb_strcpy(c.oper, sizeof(c.oper), kc.oper);
    if (kc.state == KCELL_FAILED)
        sb_strcpy(c.why, sizeof(c.why), kc.why);
    if (kc.want || kc.state != KCELL_OFF) {
        c.state = kc.state == KCELL_STARTING || kc.state == KCELL_STOPPING ? SH_CELL_STARTING :
                  kc.state == KCELL_SEARCHING || kc.state == KCELL_REGISTERED || kc.state == KCELL_DATA ? SH_CELL_ON :
                  kc.state == KCELL_OFF ? SH_CELL_OFF : SH_CELL_FAILED;
        c.bars = kc.bars;
        c.rat = has(kc.rat, "LTE") ? 7 : has(kc.rat, "UMTS") ? 4 : has(kc.rat, "EDGE") ? 3 : has(kc.rat, "GPRS") ? 2 :
                has(kc.rat, "GSM") ? 1 : 0;
        sh_cell(&c);
        int ch = memcmp(&c, &last, sizeof(c)) != 0;
        last = c;
        return ch;
    }
    if (ph.state == SBOS_MODEM_ON) c.state = SH_CELL_ON;
    else if (ph.state == SBOS_MODEM_FAILED) c.state = SH_CELL_FAILED;
    else if (ph.state == SBOS_MODEM_PARKED) c.state = SH_CELL_GUARD;
    else if (ph.state == SBOS_MODEM_STARTING) c.state = has(ph.why, "Restarting") ? SH_CELL_RESTARTING : SH_CELL_STARTING;
    c.emergency_only = has(ph.cs, "emergency");
    c.rat = has(ph.cs, "LTE") ? 7 : has(ph.cs, "UMTS") ? 4 : has(ph.cs, "EDGE") ? 3 : has(ph.cs, "GPRS") ? 2 : has(ph.cs, "GSM") ? 1 : 0;
    sh_cell(&c);
    int changed = c.state != last.state || c.sim != last.sim || c.emergency_only != last.emergency_only ||
                  c.rat != last.rat || c.bars != last.bars || c.data != last.data;
    last = c;
    return changed;
}
/* wifi-sbos-next: Wi-Fi's state for Settings > Wi-Fi, the status bar and the Control Centre: the kernel's status, its
 * scan list and the last Internet test, pushed to the shell when anything in it changed (names and addresses go to
 * the screen only, never to the log) */
static void ip_text(char *b, const uint8_t ip[4])
{
    int n = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = ip[i];
        if (v >= 100) b[n++] = (char)('0' + v / 100);
        if (v >= 10) b[n++] = (char)('0' + v / 10 % 10);
        b[n++] = (char)('0' + v % 10);
        if (i < 3) b[n++] = '.';
    }
    b[n] = 0;
}
static int wifi_now(void)
{
    static sh_wifi_t last;
    static int pushed;
    static sh_wifi_t w;
    struct sbos_wifi s;
    struct sbos_wifi_net nets[SBOS_WIFI_NETS];
    struct sbos_wifi_fetch f;
    memset(&w, 0, sizeof(w));
    wifi_status(&s);
    w.state = s.state;
    w.rssi = s.rssi;
    w.clock_checked = s.clock_checked;
    memcpy(w.ssid, s.ssid, s.ssid_len < 32 ? s.ssid_len : 32);
    sb_strcpy(w.why, sizeof(w.why), s.why);
    if (s.state == SBOS_WIFI_CONNECTED) ip_text(w.ip, s.ip);
    int n = wifi_list(nets, SBOS_WIFI_NETS);
    w.nnets = n < SH_WIFI_NETS ? n : SH_WIFI_NETS;
    for (int i = 0; i < w.nnets; i++) {
        memcpy(w.nets[i].ssid, nets[i].ssid, nets[i].ssid_len < 32 ? nets[i].ssid_len : 32);
        w.nets[i].rssi = nets[i].rssi;
        w.nets[i].secure = nets[i].secure;
        w.nets[i].saved = nets[i].saved;
        w.nets[i].band5 = nets[i].band5;
    }
    wifi_fetch_status(&f);
    w.fetch_state = f.state;
    w.fetch_ok = f.state == 2 && f.result == 1 && f.status >= 200 && f.status < 400;
    w.fetch_status = f.status;
    w.fetch_bytes = f.bytes;
    w.fetch_ms = f.total_ms;
    w.fetch_tls_ms = f.tls_ms;
    sb_strcpy(w.fetch_host, sizeof(w.fetch_host), f.host);
    if (f.state == 2 && f.result == 1 && !w.fetch_ok)
        sb_strcpy(w.fetch_why, sizeof(w.fetch_why), "the server answered with an error status");
    else if (f.state == 2 && f.result == 1)
        sb_strcpy(w.fetch_why, sizeof(w.fetch_why), f.tls ? "HTTPS through the phone's own TCP and TLS" : "HTTP");
    else
        sb_strcpy(w.fetch_why, sizeof(w.fetch_why), f.why);
    if (pushed && !memcmp(&w, &last, sizeof(w)))
        return 0;
    last = w;
    pushed = 1;
    sh_wifi(&w);
    return 1;
}
int sh_wifi_power(int on) { return wifi_power(on); }
const char *sh_wifi_why(void)
{
    static struct sbos_wifi s;
    wifi_status(&s);
    return s.why;
}
int sh_wifi_scan(void) { return wifi_scan(); }
int sh_wifi_join(const char *ssid, const char *pass)
{
    struct sbos_wifi_net nets[SBOS_WIFI_NETS];
    int n = wifi_list(nets, SBOS_WIFI_NETS);
    unsigned len = (unsigned)sb_strlen(ssid);
    for (int i = 0; i < n; i++)
        if (nets[i].ssid_len == len && !memcmp(nets[i].ssid, ssid, len))
            return wifi_join(i, pass && pass[0] ? pass : 0);
    return SBOS_EINVAL;                              /* not in the list any more (a new scan) */
}
int sh_wifi_fetch(const char *url) { return wifi_fetch(url); }

/* the modem's Start / Stop from the new UI: the kernel's own path (its hold confirmation, its refusals: never in
 * a self-test image, not while Heat or Linux runs) */
int sh_modem(int on) { return kmodem_power(on); }
int sh_cellular(int on) { return kmodem_cellular(on); }
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
/* helper D: notifications for the shell (shell_nc.inc): the kernel's posts since the last look (notif_seq), oldest
 * first; polled from the draw hook, never inside a frame. The kernel's own list keeps its unread count (the classic UI's
 * dot): Next's shell keeps its own. */
static int notif_now(void)
{
    static unsigned seen;
    unsigned s = notif_seq();
    if (s == seen) return 0;
    int n = (int)(s - seen);
    if (n > notif_count()) n = notif_count();
    seen = s;
    for (int i = n - 1; i >= 0; i--) {
        const struct notif *q = notif_get(i);
        if (q) sh_notify(q->who, q->title, q->text, q->hm);
    }
    return n > 0;
}
static void status_now(void)
{
    struct sbos_time t;
    struct sbos_battery b;
    int h = 0, mi = 0, wd = 0, d = 1, mo = 1;
    if (clock_now(&t)) { h = t.h; mi = t.mi; wd = t.wday; d = t.d; mo = t.mo; sh_year(t.y); }
    battery_get(&b);
    int pct = b.ok ? b.pct : 0, temp = b.ok && b.temp_c10 > -1000 ? b.temp_c10 / 10 : 0;
    sh_status(h, mi, wd, d, mo, pct, temp);
    sh_sysinfo(b.ok && b.charger, "Starbite OS Next P3s");     /* P2y: the apps' live values */
    {
        int ah = 0, am = 0, on = alarm_first(&ah, &am);
        sh_alarm(on, ah, am);
    }
    cell_now();
    wifi_now();                                      /* wifi-sbos-next */
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
static uint64_t present_us, last_us;   /* P3k: the last flip's request; the last frame's start */
static unsigned pace_looks;            /* P3k: 1 ms looks for a queued flip's latch */
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
            present_us = sb_us();
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
    ub_s(&u, " MiB; latch looks ");
    ub_dec(&u, pace_looks);
    pace_looks = 0;
    ub_s(&u, "; shell max ");
    ub_dec(&u, FS.shmax / 1000);
    ub_s(&u, " ms, over 8 ms: ");
    ub_dec(&u, FS.shover);
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
/* P2z: the Clock app's stopwatch and timer without 60 Hz frames: a one-shot at the time the shell asks for, a
 * frame only if its digits changed (a timer that runs out vibrates even with the screen off) */
static void clock_again(void)
{
    int ms = sh_next_update((float)sb_ms());
    if (ms >= 0) kt_set(KT_UI, 25, sb_ms(), (uint32_t)(ms < 15 ? 15 : ms), 0);
    else kt_cancel(KT_UI, 25);
}
void sh_vibrate(void)
{
#ifdef SBOS_SELFTEST_NEXT
    sb_log("next: timer done: would vibrate (a dry run: the self test)");
#else
    if (vib_buzz(3, 400, 300) < 0) sb_log("next: timer done: the vibration motor did not answer");
#endif
    if (!K.screen_on) power_screen_set(1);           /* P3f: as iOS, the alert wakes the screen (its fade in) */
}
/* P3h: a key's light tap (Notes, Search, the Calculator, the keypad): a 10 ms pulse; the motor's own timer
 * (KT_UI 9, the classic ui_ktimer's vib_timer) ends it. The self test stays still. */
void sh_haptic(void)
{
#ifndef SBOS_SELFTEST_NEXT
    (void)vib_pulse(10);
#endif
}
/* P3i: the shell's time outside the compositor (a tap that builds a page runs before the next frame: the pause
 * the user sees is this, not the frame's own time); logged when over 8 ms, counted per gesture */
static void shell_time(const char *what, uint64_t t0)
{
    uint32_t us = (uint32_t)(tk_us() - t0);
    if (us > FS.shmax) FS.shmax = us;
    if (us <= 8000) return;
    FS.shover++;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: shell ");
    ub_s(&u, what);
    ub_c(&u, ' ');
    ub_dec(&u, us / 1000);
    ub_s(&u, " ms");
    sb_log(u.b);
}
static uint64_t tick3;                      /* the next frame's time in thirds of a ms (one buffer: a free timer) */
/* P3k: frames paced by the panel. A frame starts just after the last flip has latched at a TE (looks 1 ms apart
 * from 14.7 ms after the last start), so each TE gets one new frame: the free 16.67 ms timer drifted against the
 * TE (P3h: frames replaced while queued, 4-25 a film, and TEs showing an old frame). The animations advance by
 * whole TE periods (dt a multiple of 16.67 ms), and the touch is read at the frame's start. */
void next_input_now(void);
static void frame(void)
{
    if (flip_on && comp.acquire && K.screen_on && flip_pending() && sb_us() - present_us < 40000) {
        pace_looks++;
        kt_set(KT_UI, 20, sb_ms() + 1, 0, 0);
        return;
    }
    uint64_t nus = sb_us();
    if (flip_on && comp.acquire)
        kt_set(KT_UI, 20, (nus + 14700) / 1000, 0, 0);
    else {
        /* one buffer: the next frame at the next 16.67 ms step (P2t) */
        uint64_t n3 = sb_ms() * 3;
        if (!tick3) tick3 = n3;
        tick3 += 50;
        if (tick3 <= n3) tick3 = n3 + 50;           /* fell behind: from now */
        kt_set(KT_UI, 20, tick3 / 3, 0, 0);
    }
    next_input_now();                            /* the finger where it is now */
    if (!boosted) boost(1);                      /* a wake: the kernel set the screen-on clock after the hook */
    uint64_t now = sb_ms();
    float dt = last_tick ? (float)(now - last_tick) / 1000.0f : 1.0f / 60;
    if (flip_on && comp.acquire && last_us) {
        uint64_t te = (nus - last_us + 8333) / 16667;          /* whole TE periods since the last frame */
        dt = (float)(te ? te : 1) / 60.0f;
    }
    last_tick = now;
    last_us = nus;
    uint64_t st0 = tk_us();
    int mv = sh_tick(dt, (float)now);
    shell_time("tick", st0);
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
        last_tick = 0; last_us = 0;
        sh_idle();
        if (sh_idle_pending()) kt_set(KT_UI, 24, sb_ms(), 40, 0);   /* the rest of the idle work, a turn at a time */
        clock_again();                                               /* P2z: the Clock's next digit */
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
    last_us = 0;
    tick3 = 0;
    kt_set(KT_UI, 20, sb_ms() + 1, 0, 0);       /* P3k: the first frame now (it waits for a queued flip's latch) */
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
        last_tick = 0; last_us = 0;
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
    last_tick = 0; last_us = 0;
    tick3 = 0;
    kt_set(KT_UI, 20, sb_ms() + 16, 0, 0);
}

/* ---- the kernel's UI entry points in this image ------------------------------------------------------------- */
void next_kinit(void)
{
    boot_t0 = sb_ms();
    log_mb("arena free before the UI heap: ", sb_arena_free());
    heap_init(160ULL << 20);
    log_mb("UI heap: ", heap_size);
    /* P3o: the band workers' stacks (3 x 64 KiB from libsb's arena, taken at their first start and kept) now,
     * before the services fill the arena. P3n skipped the old wallpaper, so the boot's Wi-Fi time check got its
     * buffers and took the rest: every later start failed (-102, no stack) and the frames ran on one core. */
    workers_start();
    {
        struct ub a;
        ub_init(&a);
        ub_s(&a, "next: arena free after the UI heap and the workers: ");
        ub_dec(&a, sb_arena_free() >> 10);
        ub_s(&a, " KiB");
        sb_log(a.b);
    }
    kui_confirm_hook = next_confirm;             /* the kernel's hold confirmations (the modem's Start) in this UI */
    kui_screen_hook = next_screen;               /* P2r: sleep and wake as animations */
    kui_sleep_hook = next_sleep;
    input_fast = 1;                              /* P3k: touch read every 16 ms while the screen is on (no 50 ms first touch) */
    if (!tk_pack_open(sbtk_pack_start, (uint32_t)(sbtk_pack_end - sbtk_pack_start))) {
        sb_log("next: the sbtk pack is missing or bad: the regular UI runs");
        return;
    }
    if (!tk_comp_init(&comp, ui_back(), (uint32_t *)SB_FB)) {
        sb_log("next: no memory for the compositor: the regular UI runs");
        return;
    }
    scan_init();
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
    {
        /* P3n: the kernel's arena left once the services have started (in KiB: P3m was at 54 KiB when Cellular
         * and Wi-Fi asked for memory), so the next large consumer shows in the log */
        struct ub a;
        ub_init(&a);
        ub_s(&a, "next: arena free when ready: ");
        ub_dec(&a, sb_arena_free() >> 10);
        ub_s(&a, " KiB");
        sb_log(a.b);
    }
    next_kdraw();
    sh_idle();                                       /* the unlock's cache and the half copies, before a finger */
    log_mb("UI heap with the caches: ", tk_heap_used());
}

int gwin_now(void);                                  /* L2 + L3: starbite/guest/next/gwin.c */
int next_ready(void) { return ready; }               /* L2: the shell baked (uitest.c's next-linux variant) */
void next_kdraw(void)
{
    if (!ready && !baking) { ui_kdraw(); return; }
    /* L2 + L3: Linux apps: a state change, the catalog or the store, or a new frame (rows copied) drawn at once */
    if (ready && gwin_now() && !ticking && K.screen_on) {
        tk_comp_frame(&comp, 0, 0);
        present();
    }
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
    /* wifi-sbos-next: Wi-Fi's state ten times a second (a copy of about 2 KB; this runs every loop turn); a change
     * is drawn at once */
    static uint64_t wifi_t;
    if (ready && sb_ms() - wifi_t >= 100 && (wifi_t = sb_ms(), wifi_now()) && !ticking) {
        tk_comp_frame(&comp, 0, 0);
        present();
        sh_idle();
    }
    /* helper D: new notifications four times a second (a counter compare); a banner slides in (frames), the lock's
     * pill is drawn at once */
    static uint64_t notif_t;
    if (ready && sb_ms() - notif_t >= 250 && (notif_t = sb_ms(), notif_now()) && !ticking) {
        if (sh_moving()) start_ticking();
        else {
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
    uint64_t st0 = tk_us();
    sh_touch(ph, e->x / TK_PT, e->y / TK_PT, (float)sb_ms());
    shell_time(ph == 0 ? "touch down" : ph == 1 ? "touch move" : "touch up", st0);
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
    if (ready && id == 25) {
        if (!ticking) {
            if (sh_refresh((float)sb_ms()) && K.screen_on) {
                tk_comp_frame(&comp, 0, 0);
                present();
            }
            if (sh_moving() && K.screen_on) start_ticking();   /* helper D: a timed move (the banner going away) */
            clock_again();
        }
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

/* Photos, Files, Music (shell_media.inc): the card's exFAT partition through kern/files.c, read only. The self test
 * never touches the card: there it is "no card". */
int sh_card_status(const char **why)
{
#ifdef SBOS_SELFTEST_NEXT
    if (why) *why = "";
    return SH_CARD_NONE;
#else
    int s = kfile_state(why);
    return s > 0 ? SH_CARD_READY : !s ? SH_CARD_MOUNTING : s == -1 ? SH_CARD_NONE : SH_CARD_ERROR;
#endif
}
int sh_card_mount(void)
{
#ifndef SBOS_SELFTEST_NEXT
    hw_wdt_feed();
    kfile_mount();
#endif
    return sh_card_status(0);
}
int sh_card_list(const char *path, sh_dirent_t *out, int max, int first)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)path; (void)out; (void)max; (void)first;
    return -1;
#else
    static struct kfile_ent e[32];
    int k = 0;
    while (k < max) {
        int want = max - k < 32 ? max - k : 32, n = kfile_list_at(path, e, want, first + k);
        if (n < 0) return -1;
        for (int i = 0; i < n; i++, k++) {
            tk_memcpy(out[k].name, e[i].name, sizeof out[k].name);
            out[k].size = e[i].size;
            out[k].dir = e[i].dir;
            out[k].date = e[i].mtime;
        }
        if (n < want) break;
    }
    return k;
#endif
}
int sh_card_read(const char *path, uint64_t off, void *dst, uint32_t len)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)path; (void)off; (void)dst; (void)len;
    return -1;
#else
    return kfile_read_at(path, off, dst, len);
#endif
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

/* ---- survival-next: Emergency mode's tools (shell_survival.inc) on the kernel's survival API (kern/survival.h)
 * and services.c's SOS. The shell starts SOS and USB power out only at the end of the user's hold; the self test's
 * build refuses both and changes no setting (its films never reach them; this is the second line). ---- */
#ifdef SBOS_SELFTEST_NEXT
#define SURV_DRY(what) do { sb_log("next: survival: " what ": refused (a dry run: the self test)"); } while (0)
#endif
int sh_sos_set(int on)
{
#ifdef SBOS_SELFTEST_NEXT
    if (on) { SURV_DRY("SOS"); return -1; }
#endif
    sb_log(on ? "next: survival: SOS on (held on the screen)" : "next: survival: SOS stopped on the screen");
    return sos_set(on);
}
int sh_sos_frame(uint32_t *argb) { return sos_frame(argb); }
int sh_sos_secs(void)
{
    struct sbos_sos s;
    sos_status(&s);
    return s.state == 2 && s.secs < 0 ? -s.secs : 0;
}
unsigned sh_sos_opts(void) { return surv_sos_opts; }
void sh_sos_opts_set(unsigned o)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)o;
    SURV_DRY("the SOS options");
#else
    surv_sos_opts = o & (SURV_SOS_TORCH | SURV_SOS_VIB | 3U << SURV_BEACON_SHIFT);
    settings_changed();
#endif
}
int sh_usb_start(void)
{
#ifdef SBOS_SELFTEST_NEXT
    SURV_DRY("USB power out");
    return -1;
#else
    sb_log("next: survival: USB power out held on the screen");
    return surv_usb_start();
#endif
}
void sh_usb_stop(void) { surv_usb_stop("Stop on the screen"); }
void sh_usb(sh_usb_t *u)
{
    struct surv_usb s;
    surv_usb_status(&s);
    u->state = s.state == SURV_USB_ON ? SH_USB_ON : s.state == SURV_USB_COOL ? SH_USB_COOL : SH_USB_OFF;
    u->ms_left = s.ms_left;
    u->est_ma = s.est_ma;
    u->vbus_lo_mv = s.vbus_lo_mv;
    u->vbus_hi_mv = s.vbus_hi_mv;
    u->bat_c10 = s.bat_c10;
    u->limit_c10 = s.limit_c10;
    u->ilim_ma = s.ilim_ma;
    u->refused = s.refused;
    u->why = s.why;
}
const char *sh_usb_guide(int i)
{
    for (int k = 0; k <= i; k++) if (!surv_usb_guide[k]) return 0;
    return surv_usb_guide[i];
}
int sh_sun(int *rise, int *set) { return surv_sun_today(rise, set); }
void sh_loc(int *lat, int *lon) { *lat = surv_lat_x100; *lon = surv_lon_x100; }
void sh_loc_set(int lat, int lon)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)lat, (void)lon;
    SURV_DRY("the location");
#else
    surv_lat_x100 = lat;
    surv_lon_x100 = lon;
    settings_changed();
#endif
}
_Static_assert(sizeof(sh_card_t) == sizeof(struct surv_card) && SH_CARD_NF == SURV_CARD_NF && SH_CARD_FL == SURV_CARD_FL,
               "sh_card_t is struct surv_card");
int sh_card(sh_card_t *c)
{
    struct surv_card k;
    int r = surv_card_get(&k);
    tk_memcpy(c, &k, sizeof(k));
    return r;
}
int sh_card_set(const sh_card_t *c)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)c;
    SURV_DRY("the emergency card");
    return 0;
#else
    struct surv_card k;
    tk_memcpy(&k, c, sizeof(k));
    return surv_card_set(&k);
#endif
}
const char *sh_card_label(int i) { return i >= 0 && i < SURV_CARD_NF ? surv_card_label[i] : ""; }
int sh_last2(void) { return surv_last2(); }
int sh_last2_set(int on)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)on;
    SURV_DRY("Last 2 %");
    return -1;
#else
    return surv_last2_set(on, "the switch in Emergency");
#endif
}
int sh_last2_auto(void) { return surv_last2_auto; }
void sh_last2_auto_set(int on)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)on;
    SURV_DRY("Last 2 % at 3 %");
#else
    surv_last2_auto = on != 0;
    settings_changed();
#endif
}
int sh_guide_n(void)
{
    int n = 0;
    (void)surv_guide(&n);
    return n;
}
const char *sh_guide_chapter(int c, const char **sub, unsigned *argb, int *npages)
{
    int n = 0;
    const struct sbg_chapter *ch = surv_guide(&n);
    if (c < 0 || c >= n) return 0;
    if (sub) *sub = ch[c].sub;
    if (argb) *argb = ch[c].argb;
    if (npages) *npages = ch[c].n;
    return ch[c].title;
}
const char *sh_guide_page(int c, int p, const char *const **lines)
{
    int n = 0;
    const struct sbg_chapter *ch = surv_guide(&n);
    if (c < 0 || c >= n || p < 0 || p >= ch[c].n) return 0;
    if (lines) *lines = ch[c].pages[p].lines;
    return ch[c].pages[p].title;
}
const char *sh_sensors_why(void) { return surv_sensors_why(); }

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
                                { 0, 0, 0, 0, 0, 1000 }, { 2, 65, 743, 345, 743, 550 }, { 0, 0, 0, 0, 0, 2600 } };
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
                                { 0, 0, 0, 0, 0, 900 }, { 2, 320, 450, 60, 450, 200 }, { 0, 0, 0, 0, 0, 900 },
                                { 5, 0, 0, 0, 0, 20 }, { 0, 0, 0, 0, 0, 900 }, { 5, 0, 0, 0, 0, 20 },   /* helper D: the Power key relocks */
                                { 0, 0, 0, 0, 0, 1200 }, { 2, 195, 600, 195, 250, 300 }, { 0, 0, 0, 0, 0, 1200 },
                                { 1, 195, 624, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1200 } };   /* survival-next: the list scrolls to Exit */
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
    case 9: F.s = S_EMERG; F.n = (int)(sizeof S_EMERG / sizeof S_EMERG[0]); break;
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
        if (!workers_ever) {
            sb_log("selftest: FAILED: next: the band workers never started (cores 1-3: no stack?); every frame on one core");
            st_failed = 1;
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
            { uint64_t st0 = tk_us(); sh_touch(2, p->kind == 2 ? p->x1 : p->x0, p->kind == 2 ? p->y1 : p->y0, (float)sb_ms()); shell_time("touch up", st0); }
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
            if (!F.down) { uint64_t st0 = tk_us(); sh_touch(0, p->x0, p->y0, (float)sb_ms()); shell_time("touch down", st0); F.down = 1; start_ticking(); }
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
