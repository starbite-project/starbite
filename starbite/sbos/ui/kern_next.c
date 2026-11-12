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
#include "gnssfix.h"
#include "shub.h"
#include "store.h"
#include "../guide/sbguide.h"
#include "tzdb.h"                       /* Track A: time zones (kern/tzdb.c) */

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

static int gpu_started;
static uint32_t gpu_clock_steps, gpu_clock_told;
static void boost(int on)
{
    if (on == boosted) return;
    boosted = on;
    if (!hw_dvfs(DVFS_A55, on ? 1794000U : 1053000U))
        sb_log(on ? "next: the A55 did not take 1794 MHz" : "next: the A55 did not go back to 1053 MHz");
    /* P4 v10: the GPU at 572 MHz while frames come, 260 MHz (the vendor table's lowest step) when nothing moves */
    if (gpu_started && comp.gpu) {
        if (hw_dvfs(DVFS_GPU, on ? 572000U : 260000U)) gpu_clock_steps++;
        else if (!gpu_clock_told++) sb_log("next: the GPU clock did not change (it stays where it is)");
    }
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
int sh_wifi_power(int on)
{
    /* Track A: the user (or the shell's kept cc.wifi) wants Wi-Fi on while the boot's time check holds it: it stays
     * on (else the check's end would switch it off under them) */
    if (on && wifi_time_only()) wifi_time_only_end("the user wants Wi-Fi on: it stays on");
    return wifi_power(on);
}
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
        if (q) sh_notify_flags(q->who, q->title, q->text, q->hm, q->flags);   /* Track A: notif_post_app's flags */
    }
    return n > 0;
}
/* helper D: the boot menu's last choice (the loader's settings record on UFS) for the Systems widget: read once, at
 * the first minute (UFS is up by then); the self test reads nothing */
static void lastos_now(void)
{
#ifndef SBOS_SELFTEST_NEXT
    static int done;
    if (done || !ready) return;
    done = 1;
    struct star_settings st;
    if (settings_load(&st)) sh_lastos(st.last_key);
#endif
}
/* helper D: Maps' last fix (kern/gnssfix.c: Android's fix on the card, cached 5 minutes); the shell asks from idle */
int sh_gnss(sh_gnss_t *o)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)o;
    return 0;                                        /* the self test never touches the card */
#else
    struct gnss_fix f;
    if (gnss_lastfix(&f, 0) != 1) return 0;
    o->lat_e6 = f.lat_e6;
    o->lon_e6 = f.lon_e6;
    o->acc_dm = f.acc_dm;
    o->sats = f.sats;
    o->utc = (long long)f.utc;
    sb_strcpy(o->src, sizeof(o->src), f.src);
    return 1;
#endif
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
    sh_sysinfo(b.ok && b.charger, "Starbite OS Next P3w");     /* P2y: the apps' live values */
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
/* (B3's clipboard: sh_clip_set / sh_clip_get are Track E's, in shell_linux.inc) */
/* B4: the feedback kinds (impact light / medium / heavy / soft / rigid, selection) as pulse lengths (the motor's PWM
 * duty is fixed: a length stands for the strength); success / warning / error as short patterns (vib_buzz) */
void sh_haptic_k(int kind)
{
#ifndef SBOS_SELFTEST_NEXT
    static const unsigned MS[6] = { 8, 12, 18, 9, 6, 4 };
    if (kind == SH_HAP_SUCCESS) (void)vib_buzz(2, 12, 60);
    else if (kind == SH_HAP_WARNING) (void)vib_buzz(2, 12, 80);
    else if (kind == SH_HAP_ERROR) (void)vib_buzz(3, 10, 50);
    else (void)vib_pulse(MS[kind >= 0 && kind < 6 ? kind : 0]);
#else
    (void)kind;
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

/* ---- P4: the GPU compositor (kern/gpucomp.c): the frames on the Mali-G72, full resolution ---------------------- */
_Static_assert(sizeof(tk_gpu_item) == sizeof(struct gc_quad), "tk_gpu_item is gpucomp's gc_quad");
/* v5.1, the bottom cache: the frame's bottom items that have not changed for two frames (the wallpaper; the lock
 * screen under the sleep fade; home under Control Centre) composed once into the GPU's pool, and frames draw that
 * (one load a pixel) plus the items above it. A ground that changes every frame (Emergency's cross-fade) is never
 * cached: its frames read base_half bilinearly as they come. */
static int same_item(const tk_gpu_item *a, const tk_gpu_item *b)
{
    int same = a->src == b->src && a->stride == b->stride && a->w == b->w && a->h == b->h && a->flags == b->flags &&
           a->solid == b->solid && a->alpha == b->alpha && a->dx0 == b->dx0 && a->dy0 == b->dy0 && a->isx == b->isx &&
           a->isy == b->isy && a->cx0 == b->cx0 && a->cy0 == b->cy0 && a->cx1 == b->cx1 && a->cy1 == b->cy1 &&
           a->radius == b->radius && a->ft0 == b->ft0 && a->ft1 == b->ft1 && a->fb0 == b->fb0 && a->fb1 == b->fb1 &&
           a->len == b->len && a->s0 == b->s0 && a->s1 == b->s1 && a->mask == b->mask && a->mstride == b->mstride &&
           a->bd == b->bd && a->bstride == b->bstride && a->bk == b->bk && a->bw == b->bw && a->bh == b->bh &&
           a->gen == b->gen && a->lx == b->lx && a->ly == b->ly && a->lw == b->lw && a->lh == b->lh &&
           a->lsat == b->lsat && a->live == b->live;
    for (unsigned k = 0; same && k < sizeof a->lq / sizeof a->lq[0]; k++)   /* glass V3: the lens's record */
        same = a->lq[k] == b->lq[k];
    return same;
}
static void boot_notes(void);                    /* Track A, 1.0 (the end of this file) */
static tk_gpu_item prevq[TK_GPU_MAX], cacheq[TK_GPU_MAX], gx[TK_GPU_MAX];
static int prev_n, cache_n, stable_p = -1, stable_k;
static uint32_t wall_made, cache_made;
_Static_assert(sizeof(tk_irect) == sizeof(struct gc_rect), "tk_irect is gpucomp's gc_rect");
/* v10, live glass backdrops: a lens quad with `live` gets the blur of the frame's first `live` items (glass over
 * moving content, glass over glass). Up to GPUCOMP_LEVELS distinct prefixes a frame (more: the last level's); a
 * level is made again only when its items (or a lower level) changed, and then the glass over it is redrawn in the
 * next three frames (every buffer) */
static tk_gpu_item lq[TK_GPU_MAX], lvq[GPUCOMP_LEVELS][TK_GPU_MAX];
static int lvn[GPUCOMP_LEVELS], lv_ok[GPUCOMP_LEVELS], lv_redraw;
static uint32_t lv_made, lv_us_max;
static tk_irect ldmg[96];
static const tk_gpu_item *live_backdrops(const tk_gpu_item *it, int n, const tk_irect **dmg, int *ndmg)
{
    int lv[GPUCOMP_LEVELS], nl = 0, any = 0;
    for (int i = 0; i < n; i++)
        if ((it[i].flags & TK_GPU_LENS) && it[i].live > 1 && it[i].live <= (uint32_t)n) {
            any = 1;
            if (!nl || (lv[nl - 1] != (int)it[i].live && nl < GPUCOMP_LEVELS)) lv[nl++] = (int)it[i].live;
        }
    if (!any) return it;
    tk_memcpy(lq, it, sizeof(tk_gpu_item) * (size_t)n);
    int remade = 0, budget = comp.gpu_half ? 1 : GPUCOMP_LEVELS;   /* a big move's frame: one level a frame */
    for (int L = 0; L < nl; L++) {
        int p = lv[L];
        int same = lv_ok[L] && lvn[L] == p && !remade;
        for (int k = 0; same && k < p; k++) same = same_item(&lq[k], &lvq[L][k]);
        if (!same && !budget && lv_ok[L]) same = 1;     /* over budget: last frame's (it is made next frame) */
        if (!same) budget--;
        const void *bd = 0;
        uint32_t bs = 0, bw = 0, bh = 0;
        float bk = 0;
        if (!same) {
            uint64_t u = 0;
            /* the level's backdrop pointers (stable) even when it fails: then this frame's glass shows the wallpaper */
            if (!gpucomp_backdrop((unsigned)L, 0xff000000U, (const struct gc_quad *)lq, (unsigned)p, &bd, &bs, &bw, &bh, &bk, &u)) {
                lv_ok[L] = 0;
                return lq;
            }
            tk_memcpy(lvq[L], lq, sizeof(tk_gpu_item) * (size_t)p);
            lvn[L] = p;
            lv_ok[L] = 1;
            remade = 1;
            lv_made++;
            if (u > lv_us_max) lv_us_max = (uint32_t)u;
        }
        /* the level's result: where gpucomp keeps it (the same every frame) */
        static const void *lbd[GPUCOMP_LEVELS];
        static uint32_t lbs[GPUCOMP_LEVELS], lbw[GPUCOMP_LEVELS], lbh[GPUCOMP_LEVELS];
        static float lbk[GPUCOMP_LEVELS];
        if (!same) { lbd[L] = bd; lbs[L] = bs; lbw[L] = bw; lbh[L] = bh; lbk[L] = bk; }
        /* its glass: the quads whose prefix is this level's (the last level takes the deeper ones too) */
        for (int i = p; i < n; i++) {
            tk_gpu_item *q = &lq[i];
            if (!(q->flags & TK_GPU_LENS) || q->live <= 1) continue;
            int mine = (int)q->live == p || (L == nl - 1 && (int)q->live > p);
            if (!mine) continue;
            q->bd = lbd[L];
            q->bstride = lbs[L];
            q->bk = lbk[L];
            q->bw = lbw[L];
            q->bh = lbh[L];
            q->lsat = 1.2f;                     /* the audit's regular glass */
        }
    }
    if (remade) lv_redraw = 3;
    if (lv_redraw > 0 && *ndmg > 0) {
        /* the glass over a level that changed: in this frame's damage (and the next two, for the other buffers) */
        int m = 0;
        for (int i = 0; i < *ndmg && m < 96; i++) ldmg[m++] = (*dmg)[i];
        for (int i = 0; i < n && m < 96; i++)
            if ((lq[i].flags & TK_GPU_LENS) && lq[i].live > 1)
                ldmg[m++] = (tk_irect){ (int)lq[i].cx0, (int)lq[i].cy0, (int)lq[i].cx1 + 1, (int)lq[i].cy1 + 1 };
        *dmg = ldmg;
        *ndmg = m;
    }
    if (lv_redraw > 0) lv_redraw--;
    return lq;
}

static int next_gpu(void *ctx, uint32_t *dst, const tk_gpu_item *it, int n, const tk_irect *dmg, int ndmg, uint64_t *us)
{
    (void)ctx;
    if (n > 0 && n <= TK_GPU_MAX) it = live_backdrops(it, n, &dmg, &ndmg);
    const tk_gpu_item *use = it;
    int un = n;
    uint32_t *cache = (uint32_t *)gpucomp_pool();
    if (n > 0 && n <= TK_GPU_MAX && gpucomp_pool_bytes() >= (uint64_t)TK_W * TK_H * 4U && it[0].src == comp.base_half.px) {
        /* the prefix unchanged since the last frame, and how long it has been that length */
        int p = 0;
        while (p < n && p < prev_n && same_item(&it[p], &prevq[p])) p++;
        tk_memcpy(prevq, it, sizeof(tk_gpu_item) * (size_t)n);
        prev_n = n;
        if (p == stable_p) { if (stable_k < 2) stable_k++; }
        else { stable_p = p; stable_k = 0; }
        int valid = cache_n > 0 && cache_n <= n;
        for (int i = 0; valid && i < cache_n; i++) valid = same_item(&it[i], &cacheq[i]);
        /* a longer still prefix than the cache holds, still for two frames: the cache again (all of it, at most
         * n - 1 items: the top one is usually what moves) */
        int want = p < n ? p : n - 1;
        if (stable_k >= 2 && want >= 1 && (!valid || want > cache_n)) {
            uint64_t u2 = 0;
            if (gpucomp_compose(cache, 0xff000000U, (const struct gc_quad *)it, (unsigned)want, &u2)) {
                tk_memcpy(cacheq, it, sizeof(tk_gpu_item) * (size_t)want);
                cache_n = want;
                valid = 1;
                cache_made++;
                if (want == 1 && !wall_made++) sb_logx("next: gpu: the wallpaper at full resolution (the first), us: ", u2);
            } else
                cache_n = 0, valid = 0;
        }
        if (valid) {
            /* the cache as one opaque 1:1 quad, then the items above it */
            tk_memset(&gx[0], 0, sizeof gx[0]);
            gx[0].src = cache;
            gx[0].stride = TK_W;
            gx[0].w = TK_W;
            gx[0].h = TK_H;
            gx[0].flags = TK_GPU_OPAQUE;
            gx[0].alpha = 1;
            gx[0].isx = gx[0].isy = 1;
            gx[0].cx1 = TK_W;
            gx[0].cy1 = TK_H;
            tk_memcpy(&gx[1], &it[cache_n], sizeof(tk_gpu_item) * (size_t)(n - cache_n));
            use = gx;
            un = n - cache_n + 1;
        }
    }
    /* v7: a big move's frame at half resolution, 2x into dst (all of it) */
    int ok = comp.gpu_half ? gpucomp_compose_half(dst, 0xff000000U, (const struct gc_quad *)use, (unsigned)un, us)
                           : gpucomp_compose_damage(dst, 0xff000000U, (const struct gc_quad *)use, (unsigned)un,
                                                    (const struct gc_rect *)dmg, (unsigned)(ndmg > 0 ? ndmg : 0), us);
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
static void next_save(void);                         /* Track A lifecycle (below) */
void next_kinit(void)
{
    boot_t0 = sb_ms();
    log_mb("arena free before the UI heap: ", sb_arena_free());
    heap_init(160ULL << 20);                /* v9: the lens needs no maps (back to 160) */
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
    kui_save_hook = next_save;                   /* Track A lifecycle: the pages' on_save before a restart / switch-off */
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
#ifndef SBOS_NEXT_NOGPU
    /* P4: with two buffers (tear-free flips) and the GPU up (normal clocks, gpuheat's checks), the GPU composes */
    gpucomp_add_ro(sbtk_pack_start, (uint64_t)(sbtk_pack_end - sbtk_pack_start));  /* icons point into the pack */
    /* 1.0 (Track E): the Linux guest's two window surfaces (guest/sbos/guest.h GUEST_WL: SBG_S9_SHM_BASE + 0x200000,
     * 8 MiB covers both), sampled by the GPU in place through tk_layer_external; Normal non-cacheable on both sides */
    gpucomp_add_ext((const void *)(uintptr_t)0x8e0200000ULL, 0x800000ULL);
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
    sh_restore();                                    /* Track A: what the shell keeps (settings_restore ran before) */
    boot_notes();                                    /* Track A, 1.0: a safe start, or the last start's crash */
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
static int comms_now(void);                          /* helper I: calls and SMS (below) */
int next_ready(void) { return ready; }               /* L2: the shell baked (uitest.c's next-linux variant) */
void next_kdraw(void)
{
    if (!ready && !baking) { ui_kdraw(); return; }
    /* L2 + L3: Linux apps: a state change, the catalog or the store, or a new frame (rows copied) drawn at once */
    if (ready && gwin_now() && !ticking && K.screen_on) {
        tk_comp_frame(&comp, 0, 0);
        present();
    }
    /* helper I: the call's state and the inbox ten times a second (a copy of ~150 bytes and a counter), also with the
     * screen off (an incoming call wakes it); a change is drawn at once */
    static uint64_t comms_t;
    if (ready && sb_ms() - comms_t >= 100 && (comms_t = sb_ms(), comms_now()) && !ticking && K.screen_on) {
        if (sh_moving()) start_ticking();
        else {
            tk_comp_frame(&comp, 0, 0);
            present();
            sh_idle();
            clock_again();                           /* an ended call's screen goes after its 1.5 s */
        }
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
    lastos_now();                                    /* helper D: once */
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
    else settings_changed();                         /* Track A: kept (the settings record's level, as the classic UI's) */
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

/* ---- Track A: passcode ----
 * shell.h's sh_pass_* and sh_autolock* (sbos-core-api.md 3) on kern/passcode.c: the salted, iterated SHA-256 and the
 * back-off, kept in the state slot's "sbos.pass" record (settings.c; written about 1 s after a change, so a wrong try
 * is on the storage before anyone can restart around it). The digits go into passcode.c and nowhere else: nothing
 * here logs them. The self test (SBOS_SELFTEST_NEXT) runs without the passcode (its films swipe to home and write
 * nothing): sh_pass_on is 0 there and every setter is a logged dry run. */
#ifndef SBOS_SELFTEST_NEXT
static int64_t pass_utc(void)
{
    struct sbos_time t;
    return clock_now(&t) ? (int64_t)t.utc : 0;
}
/* a new passcode's salt: the counter's jitter over memory walks (the arrival of the user's taps already makes the
 * start unpredictable), the clocks, hashed; uniqueness is what a salt needs */
static void pass_salt(uint8_t salt[PASS_SALT])
{
    static volatile uint8_t walk[4096];
    static uint32_t made;
    uint8_t pool[256 + 32], h[32];
    for (unsigned i = 0; i < 256; i++) {
        uint64_t a = sb_ticks();
        for (unsigned k = 0; k < 24; k++) walk[(i * 131u + k * 523u) & 4095u] += (uint8_t)(a >> (k & 7));
        pool[i] = (uint8_t)(sb_ticks() - a) ^ (uint8_t)a;
    }
    uint64_t t = sb_ticks(), ms = sb_ms();
    int64_t u = pass_utc();
    made++;
    tk_memcpy(pool + 256, &t, 8);
    tk_memcpy(pool + 264, &ms, 8);
    tk_memcpy(pool + 272, &u, 8);
    tk_memcpy(pool + 280, &made, 4);
    tk_memset(pool + 284, 0, 4);
    pass_sha256(pool, sizeof pool, h);
    tk_memcpy(salt, h, PASS_SALT);
    tk_memset(h, 0, sizeof h);
}
#endif
int sh_pass_on(void)
{
#ifdef SBOS_SELFTEST_NEXT
    return 0;
#else
    return pass_len() > 0;
#endif
}
int sh_pass_len(void) { return pass_len(); }
int sh_pass_wait(void) { return (int)pass_wait(sb_ms()); }
int sh_pass_fails(void) { return pass_fails(); }
int sh_pass_check(const char *digits)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)digits;
    sb_log("next: passcode: a check (a dry run: the self test has none)");
    return 1;
#else
    int f0 = pass_fails();
    uint64_t t0 = sb_ms();
    int r = pass_check(digits, t0, pass_utc());
    if (r == 1) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "next: passcode: right (the hash took ");
        ub_dec(&u, sb_ms() - t0);
        ub_s(&u, " ms)");
        sb_log(u.b);
    } else if (r == 0) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "next: passcode: wrong; tries in a row ");
        ub_dec(&u, (uint64_t)pass_fails());
        if (pass_wait(sb_ms())) { ub_s(&u, ", the next in "); ub_dec(&u, pass_wait(sb_ms())); ub_s(&u, " s"); }
        sb_log(u.b);
    }
    if (pass_fails() != f0) settings_changed_soon(1000);     /* the tries on the storage within ~1 s */
    return r;
#endif
}
int sh_pass_set(const char *old, const char *nw)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)old; (void)nw;
    sb_log("next: passcode: would be set / changed / removed (a dry run: the self test)");
    return -1;
#else
    uint8_t salt[PASS_SALT];
    int had = pass_len(), f0 = pass_fails();
    pass_salt(salt);
    int r = pass_set(old, nw, salt, sb_ms(), pass_utc());
    tk_memset(salt, 0, sizeof salt);
    if (!r) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, !pass_len() ? "next: passcode: removed" : had ? "next: passcode: changed (" : "next: passcode: set (");
        if (pass_len()) { ub_dec(&u, (uint64_t)pass_len()); ub_s(&u, " digits)"); }
        sb_log(u.b);
    } else
        sb_log("next: passcode: not changed (the old code was wrong, a wait runs, or the new one is not 4 or 6 digits)");
    if (!r || pass_fails() != f0) settings_changed_soon(1000);
    if (!settings_persistent()) sb_log("next: passcode: no state slot: it lasts until a restart");
    return r;
#endif
}
/* Auto-Lock: the kernel's screen-off time (services.c; kept in the settings record); 0 = Never */
int sh_autolock(void) { return (int)K.screen_off_s; }
void sh_autolock_set(int s)
{
    if (s != 0 && (s < 10 || s > 600)) return;
#ifdef SBOS_SELFTEST_NEXT
    sb_log("next: Auto-Lock: would change (a dry run: the self test)");
#else
    K.screen_off_s = (uint32_t)s;
    power_input();                                   /* the timer from now (none for Never) */
    settings_changed();
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: Auto-Lock: ");
    if (s) { ub_dec(&u, (uint64_t)s); ub_s(&u, " s"); } else ub_s(&u, "Never");
    sb_log(u.b);
#endif
}
/* Track A lifecycle: the kernel's pre-restart / switch-off path (services.c's kui_save_hook): every page's on_save */
static void next_save(void)
{
    if (ready) sh_save_all();
}

/* ---- Track A: prefs and blobs ---- (sbos-core-api.md 1, 2) on kern/prefs.c: the store lives in RAM and goes into
 * the state slot with the settings (settings_changed: 3 s after the last change, at most one write in 10 s, held
 * while the modem runs; settings_save_now before a restart or switch-off). The self test's build reads nothing (its
 * films start from defaults whatever the phone keeps) and writes nothing: a logged dry run, the store untouched. */
#ifdef SBOS_SELFTEST_NEXT
static void prefs_dry(void)
{
    static int said;
    if (!said) { said = 1; sb_log("next: prefs: a change would be kept (a dry run: the self test writes nothing)"); }
}
#endif
int sh_pref_get(const char *key, char *val, int cap)
{
    if (val && cap > 0) val[0] = 0;
#ifdef SBOS_SELFTEST_NEXT
    (void)key;
    return -1;
#else
    return prefs_get(key, val, cap);
#endif
}
int sh_pref_set(const char *key, const char *val)
{
    if (!prefs_key_ok(key) || (val && sb_strlen(val) > PREFS_VAL_MAX)) return -1;
#ifdef SBOS_SELFTEST_NEXT
    prefs_dry();
    return 0;
#else
    int r = prefs_set(key, val);
    if (r > 0) settings_changed();
    if (r < 0) sb_logf("next: prefs: the store is full: not kept: ", key);
    return r < 0 ? -1 : 0;
#endif
}
int sh_pref_int(const char *key, int def)
{
    char b[16];
    int n = sh_pref_get(key, b, sizeof b), neg = b[0] == '-', v = 0, i = neg;
    if (n <= neg || n >= (int)sizeof b) return def;
    for (; b[i]; i++) {
        if (b[i] < '0' || b[i] > '9' || v > 214748363) return def;
        v = v * 10 + (b[i] - '0');
    }
    return neg ? -v : v;
}
void sh_pref_set_int(const char *key, int v)
{
    char b[16];
    int i = 15;
    unsigned u = v < 0 ? 0U - (unsigned)v : (unsigned)v;
    b[i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    (void)sh_pref_set(key, b + i);
}
int sh_blob_put(const char *name, const void *p, unsigned n)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)name; (void)p;
    if (n > PREFS_BLOB_MAX) return -1;
    prefs_dry();
    return 0;
#else
    int r = prefs_blob_put(name, p, n);
    if (r > 0) settings_changed();
    if (r < 0) sb_logf("next: prefs: a blob was not kept (too big, a bad name or the store full): ", name);
    return r < 0 ? -1 : 0;
#endif
}
int sh_blob_get(const char *name, void *p, unsigned cap)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)name; (void)p; (void)cap;
    return -1;
#else
    return prefs_blob_get(name, p, cap);
#endif
}
_Static_assert(SH_NOTIF_QUIET == NOTIF_QUIET && SH_NOTIF_URGENT == NOTIF_URGENT, "SH_NOTIF_* are NOTIF_*");
/* ---- end of Track A: prefs and blobs ---- */

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
int sh_sos_next(void)
{
    uint64_t now = sb_ms(), n = sos_next_ms(now);
    return !n ? -1 : n > now ? (int)(n - now) : 1;
}
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
    u->events = s.events;
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
const char *sh_last2_why(void) { return surv_last2_why(); }
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
/* helper J: the Compass page's sensor hub (kern/survival.c on shub.c): started at the page's open, kept alive while it
 * shows (every 20 s), stopped at its close; the values are cached copies (no I/O), read at 10 Hz while the page shows.
 * The self test's build never starts it (its films never open the page; this is the second line). */
_Static_assert((int)SH_SENS_OFF == (int)SHUB_OFF && (int)SH_SENS_STARTING == (int)SHUB_STARTING &&
               (int)SH_SENS_RUNNING == (int)SHUB_RUNNING && (int)SH_SENS_FAILED == (int)SHUB_FAILED, "SH_SENS_* are SHUB_*");
int sh_sensors_on(int on)
{
#ifdef SBOS_SELFTEST_NEXT
    static int said;
    if (on && !said) { said = 1; SURV_DRY("the sensor hub"); }
    return on ? -1 : 0;
#else
    static int was;
    if (on && !was) sb_log("next: survival: the compass page starts the sensor hub");
    if (!on && was) sb_log("next: survival: the compass page closed: the sensor hub off");
    int r = surv_sensors_on(on);
    was = on && r >= 0;
    return r;
#endif
}
int sh_sensors_state(void) { return surv_sensors_state(); }
int sh_heading(int *deg_x10, int *accuracy) { return surv_heading(deg_x10, accuracy); }
int sh_tilt(int *pitch_x10, int *roll_x10) { return surv_tilt(pitch_x10, roll_x10); }
int sh_steps(unsigned *n)
{
    uint32_t v = 0;
    int r = surv_steps(&v);
    *n = v;
    return r;
}
long long sh_utc(void)
{
    struct sbos_time t;
    return clock_now(&t) ? (long long)t.utc : 0;
}

/* ---- Track A: time and alarms ---- */
/* The zone and 12 / 24-hour (sbos-core-api.md 4): the kernel's clock_now applies the zone to every local time it
 * hands out (sh_status, the alarms' minute, notifications, Sun); sh_utc stays UTC. Both are kept with the settings
 * (settings.c ext[3]). The alarms (5): kern/alarm.c's four, the list and labels in its record "sbos.alarm"; ringing
 * stays the kernel's, and the lock's next alarm goes out through sh_alarm. The self test writes nothing: the zone and
 * 12 / 24-hour change in RAM only, and alarms are refused (they would buzz). */
static void ta_keep(const char *what)
{
#ifdef SBOS_SELFTEST_NEXT
    sb_logf("next: the self test keeps this in RAM only (no settings write): ", what);
#else
    (void)what;
    settings_changed();
#endif
}
int sh_tz_count(void) { return tzdb_n; }
const char *sh_tz_name(int i, const char **city)
{
    if (i < 0 || i >= tzdb_n) { if (city) *city = ""; return ""; }
    if (city) *city = tzdb[i].city;
    return tzdb[i].name;
}
int sh_tz_offset_min(int i)
{
    struct sbos_time t;
    if (i < 0 || i >= tzdb_n) return 0;
    return clock_now(&t) && t.utc ? tzdb_offset(i, t.utc) : tzdb[i].off_min;
}
int sh_tz(void) { return clock_zone(); }
void sh_tz_set(int i)
{
    if (i < 0 || i >= tzdb_n || i == clock_zone()) return;
    clock_zone_set(i);
    sb_logf("next: time zone ", tzdb[i].name);
    ta_keep("the time zone");
    if (ready) status_now();                         /* the local time, date and alarm's day again */
}
int sh_clock24(void) { return clock_24h; }
void sh_clock24_set(int on)
{
    if (!!on == clock_24h) return;
    clock_24h = !!on;
    sb_log(on ? "next: 24-hour time" : "next: 12-hour time");
    ta_keep("12 / 24-hour time");
}
int sh_alarm_count(void) { return alarm_count(); }
int sh_alarm_get(int i, int *hm, int *days, int *on, char *label, int cap)
{
    return alarm_get(i, hm, days, on, label, cap) ? -1 : 0;
}
#ifndef SBOS_SELFTEST_NEXT
static void ta_alarm_push(void)
{
    int h = 0, m = 0, on = alarm_first(&h, &m);
    sh_alarm(on, h, m);
}
#endif
int sh_alarm_put(int i, int hm, int days, int on, const char *label)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)i; (void)hm; (void)days; (void)on; (void)label;
    sb_log("next: the self test sets no alarm (dry run)");
    return -1;
#else
    if (alarm_put(i, hm, days, on, label)) return -1;
    settings_changed();
    ta_alarm_push();
    return 0;
#endif
}
int sh_alarm_del(int i)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)i;
    sb_log("next: the self test deletes no alarm (dry run)");
    return -1;
#else
    if (alarm_del(i)) return -1;
    settings_changed();
    ta_alarm_push();
    return 0;
#endif
}

/* ---- helper I: calls and SMS (shell_comms.inc) on the modem session's kmodem_call_* / kmodem_sms_* (kern.h defines
 * KMODEM_CALLS once that commit is in; without it every call refuses, "not in this version"). The shell asks only at
 * the end of the user's own action (the call button's hold, Answer, Send's tap); the self test's build refuses every
 * dial, answer, send, key tone, route and mute with a dry-run line (its films never get there: Cellular stays off in
 * it). Numbers and message text are never logged, here or in the kernel. ---- */
static void comms_why(char *why, const char *s)
{
    if (why) sb_strcpy(why, SH_WHY, s);
}
#ifdef SBOS_SELFTEST_NEXT
#define COMMS_DRY(what, why) do { sb_log("next: comms: " what ": refused (a dry run: the self test)"); \
                                  comms_why(why, "A dry run: the self test never calls or texts"); return -1; } while (0)
#else
#define COMMS_DRY(what, why) do { } while (0)
#endif
#ifdef KMODEM_CALLS
int sh_comms_ok(void) { return 1; }
/* a refusal: the kernel's own words (kcall.why) when it gave them, else the error's */
static int call_refused(int r, char *why)
{
    struct kcall c;
    kmodem_call(&c);
    comms_why(why, c.why[0] ? c.why : r == SBOS_EAGAIN ? "No network: Cellular is off or not registered" :
                   r == SBOS_EBUSY ? "A call or a message is already going" :
                   r == SBOS_EPERM ? "This number can't be called from Starbite OS" : "The modem refused");
    return r;
}
int sh_dial(const char *number, char *why)
{
    COMMS_DRY("dial", why);
    sb_log("next: comms: dialling (held on the screen)");
    int r = kmodem_call_dial(number);
    return r < 0 ? call_refused(r, why) : 0;
}
int sh_answer(char *why)
{
    COMMS_DRY("answer", why);
    sb_log("next: comms: answering (on the screen)");
    int r = kmodem_call_answer();
    return r < 0 ? call_refused(r, why) : 0;
}
int sh_hangup(void)
{
    sb_log("next: comms: hang up (on the screen)");
    return kmodem_call_hangup();
}
int sh_route(int audio, char *why)
{
    COMMS_DRY("audio route", why);
    int r = kmodem_call_route(audio == SH_AUDIO_SPEAKER ? KAUDIO_SPEAKER : KAUDIO_EARPIECE);
    return r < 0 ? call_refused(r, why) : 0;
}
int sh_mute(int on, char *why)
{
    COMMS_DRY("mute", why);
    int r = kmodem_call_mute(on);
    return r < 0 ? call_refused(r, why) : 0;
}
int sh_dtmf(char d)
{
    COMMS_DRY("key tone", (char *)0);
    return kmodem_call_dtmf(d);
}
int sh_sms_send(const char *number, const char *utf8, char *why)
{
    COMMS_DRY("SMS send", why);
    sb_log("next: comms: SMS send (tapped on the screen)");
    int r = kmodem_sms_send(number, utf8);
    if (r < 0) comms_why(why, r == SBOS_EAGAIN ? "No network: Cellular is off or not registered" :
                              r == SBOS_EBUSY ? "A message is still being sent: try again soon" :
                              r == SBOS_EPERM ? "This number can't get texts from Starbite OS" : "The modem refused");
    return r < 0 ? r : 0;
}
int sh_sms_count(void) { return kmodem_sms_count(); }
int sh_sms_get(int i, sh_sms_t *m)
{
    struct ksms k;
    if (kmodem_sms_get(i, &k) < 0) return -1;
    m->id = k.id; m->incoming = k.incoming; m->state = k.state; m->utc = k.utc;
#ifdef KMODEM_SMS_STORE
    m->unread = k.unread;                            /* states 4 delivered / 5 not delivered: SH_SMS_* */
#else
    m->unread = -1;
#endif
    sb_strcpy(m->number, sizeof(m->number), k.number);
    sb_strcpy(m->text, sizeof(m->text), k.text);
    sb_strcpy(m->why, sizeof(m->why), k.why);
    return 0;
}
/* B5: the Clock's alarms (kern/alarm.c): set by the user's own Save or switch; the self test never sets one */
int sh_alarm_set(int i, int h, int m, int days, int on)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)i; (void)h; (void)m; (void)days; (void)on;
    return -1;
#else
    return alarm_set(i, h, m, days, on);
#endif
}
/* B5: the store (the modem session's KMODEM_SMS_STORE): read marks, deletes at the user's own tap; refused in the
 * self test (it never changes what is stored) */
#ifdef KMODEM_SMS_STORE
int sh_sms_read(const char *number) { return kmodem_sms_read(number); }
int sh_sms_delete(int id, char *why)
{
    COMMS_DRY("SMS delete", why);
    int r = kmodem_sms_delete(id);
    if (r < 0) comms_why(why, "The message couldn't be deleted");
    return r < 0 ? r : 0;
}
int sh_sms_delete_thread(const char *number, char *why)
{
    COMMS_DRY("SMS delete", why);
    int r = kmodem_sms_delete_thread(number);
    if (r < 0) comms_why(why, "The conversation couldn't be deleted");
    return r < 0 ? r : 0;
}
#else
int sh_sms_read(const char *number) { (void)number; return 0; }
int sh_sms_delete(int id, char *why) { (void)id; comms_why(why, "Not in this version"); return -1; }
int sh_sms_delete_thread(const char *number, char *why) { (void)number; comms_why(why, "Not in this version"); return -1; }
#endif
/* the call's state and the inbox's generation to the shell when they change (from the draw hook: never inside a
 * frame); 1 if anything was pushed. An incoming call wakes the screen (the kernel notifies as well). */
static int comms_now(void)
{
    static struct kcall last;
    static int pushed;
    static uint32_t gen;
    int ch = 0;
    struct kcall c;
    kmodem_call(&c);
    if (!pushed || memcmp(&c, &last, sizeof(c))) {
        last = c;
        pushed = 1;
        sh_call_t s;
        memset(&s, 0, sizeof(s));
        s.state = c.state; s.outgoing = c.outgoing; s.secs = c.secs;          /* KCALL_* = SH_CALL_* */
        s.audio = c.audio; s.muted = c.muted; s.audio_ok = c.audio_ok;        /* KAUDIO_* = SH_AUDIO_* */
        sb_strcpy(s.number, sizeof(s.number), c.number);
        sb_strcpy(s.why, sizeof(s.why), c.why);
        sb_strcpy(s.audio_why, sizeof(s.audio_why), c.audio_why);
        sh_call(&s);
        if (c.state == KCALL_INCOMING && !K.screen_on) power_screen_set(1);
        ch = 1;
    }
    uint32_t g = kmodem_sms_gen();
    if (g != gen) { gen = g; sh_sms_changed(g); ch = 1; }
    return ch;
}
#else
int sh_comms_ok(void) { return 0; }
#define COMMS_NONE(why) do { comms_why(why, "Calls and SMS aren't in this version yet"); return -1; } while (0)
int sh_dial(const char *number, char *why) { (void)number; COMMS_DRY("dial", why); COMMS_NONE(why); }
int sh_answer(char *why) { COMMS_DRY("answer", why); COMMS_NONE(why); }
int sh_hangup(void) { return -1; }
int sh_route(int audio, char *why) { (void)audio; COMMS_DRY("audio route", why); COMMS_NONE(why); }
int sh_mute(int on, char *why) { (void)on; COMMS_DRY("mute", why); COMMS_NONE(why); }
int sh_dtmf(char d) { (void)d; COMMS_DRY("key tone", (char *)0); return -1; }
int sh_sms_send(const char *number, const char *utf8, char *why) { (void)number; (void)utf8; COMMS_DRY("SMS send", why); COMMS_NONE(why); }
int sh_sms_count(void) { return 0; }
int sh_sms_get(int i, sh_sms_t *m) { (void)i; (void)m; return -1; }
int sh_sms_read(const char *number) { (void)number; return 0; }
int sh_sms_delete(int id, char *why) { (void)id; comms_why(why, "Not in this version"); return -1; }
int sh_sms_delete_thread(const char *number, char *why) { (void)number; comms_why(why, "Not in this version"); return -1; }
static int comms_now(void) { return 0; }
#endif

/* ---- the self test --------------------------------------------------------------------------------------- */
#ifdef SBOS_SELFTEST_NEXT
typedef struct { int kind; float x0, y0, x1, y1, ms; } step;
static const char *const FILM_NAME[11] = { "unlock", "group", "focus", "holds", "pages", "cc", "app", "phone", "messages", "emergency", "sleep" };
/* tools/films.txt's scripts (pt, ms) */
static const step S_UNLOCK[] = { { 0, 0, 0, 0, 0, 400 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 1300 } };
/* B9: the Systems tiles (centres x 47 + 74 g, y 336): Apple opens and closes, Linux opens, Starbite takes its place,
 * its first row (Starbite OS, "You are in Starbite OS"); Microsoft's Windows 11 row (y 420) opens the focus sheet */
static const step S_GROUP[] = { { 0, 0, 0, 0, 0, 300 }, { 1, 47, 336, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1000 }, { 1, 47, 336, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 900 }, { 1, 269, 336, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1000 }, { 1, 343, 336, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 1200 }, { 1, 195, 420, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1800 } };
static const step S_FOCUS[] = { { 0, 0, 0, 0, 0, 300 }, { 1, 121, 336, 0, 0, 80 }, { 0, 0, 0, 0, 0, 900 }, { 1, 195, 420, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 1000 }, { 2, 65, 743, 345, 743, 550 }, { 0, 0, 0, 0, 0, 2600 } };
static const step S_HOLDS[] = { { 0, 0, 0, 0, 0, 300 }, { 3, 71, 727, 0, 0, 650 }, { 0, 0, 0, 0, 0, 700 }, { 3, 319, 727, 0, 0, 950 },
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
static const step S_EMERG[] = { { 0, 0, 0, 0, 0, 300 }, { 3, 319, 727, 0, 0, 950 }, { 0, 0, 0, 0, 0, 1200 }, { 2, 195, 795, 195, 330, 220 },
                                { 0, 0, 0, 0, 0, 900 }, { 2, 320, 450, 60, 450, 200 }, { 0, 0, 0, 0, 0, 900 },
                                { 5, 0, 0, 0, 0, 20 }, { 0, 0, 0, 0, 0, 900 }, { 5, 0, 0, 0, 0, 20 },   /* helper D: the Power key relocks */
                                { 0, 0, 0, 0, 0, 1200 }, { 2, 195, 600, 195, 250, 300 }, { 0, 0, 0, 0, 0, 1200 },
                                { 1, 195, 624, 0, 0, 80 }, { 0, 0, 0, 0, 0, 900 },        /* survival-next: the list scrolls to Exit */
                                { 1, 265, 452, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1200 } };   /* helper B2: the alert's Exit (films.txt's emergency) */
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
    case 1: F.s = S_GROUP; F.n = (int)(sizeof S_GROUP / sizeof S_GROUP[0]); break;
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
    ub_s(&u, " frames on the GPU (");
    ub_dec(&u, comp.gpu_halves);
    ub_s(&u, " at half resolution), ");
    ub_dec(&u, comp.gpu_fallbacks);
    ub_s(&u, " given back to the CPU; bottom caches made ");
    ub_dec(&u, cache_made);
    ub_s(&u, "; GPU clock steps (572 / 260 MHz) ");
    ub_dec(&u, gpu_clock_steps);
    ub_s(&u, "; layers flushed in idle time ");
    ub_dec(&u, comp.gpu_idle_flushed);
    ub_s(&u, "; live glass backdrops made ");
    ub_dec(&u, lv_made);
    ub_s(&u, " (max us ");
    ub_dec(&u, lv_us_max);
    ub_c(&u, ')');
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
        } else if (p->kind == 5) {
            /* a Power step whose whole time passed in one stall: pressed now, so the next film doesn't start with the
             * screen off and fail for this one's slow frame */
            sb_log("next: film: a Power step passed during a stall: pressed late");
            power_key_short();
        } else if (p->kind)
            sb_log("next: film: a touch step passed during a stall: skipped (a slow frame, see the frames line)");
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

/* ---- Track A, 1.0: the Linux window sampled in place by the GPU (Track E's zero-copy path) ---- */
int sh_linux_zero_copy(void)
{
#ifndef SBOS_NEXT_NOGPU
    return comp.gpu != 0 && gpucomp_ext_ok();
#else
    return 0;
#endif
}

/* ---- Track A, 1.0: the safe start and why this start happened (the loader's SBFW table, Track J) ---- */
int sh_safe_mode(void) { return sbos_safe_start(); }
int sh_boot_reason(char *why, int cap)
{
    static const char *const W[6] = { "Started from the boot menu", "Started as the default", "Started once",
                                       "Started as the boot menu", "Started again after a problem", "Started from USB" };
    const char *w = !sb_fw.valid ? "Started (an older loader)" : sb_fw.boot_reason < 6U ? W[sb_fw.boot_reason] : "Started";
    if (why && cap > 0) sb_strcpy(why, (unsigned)cap, w);
    return sb_fw.valid ? (int)sb_fw.boot_reason : -1;
}
/* once at the end of the bake: say so when this is a safe start, or when the last start of Starbite OS crashed */
static void boot_notes(void)
{
    if (sbos_safe_start())
        notif_post("Starbite OS", "Safe Start",
                   "Started without restoring Wi-Fi. Nothing is saved until you restart.");
    else if (sb_fw.valid && sb_fw.crash_sev >= 2U && sb_fw.crash_key[0] &&
             (!tk_strcmp(sb_fw.crash_key, "starbite-os") || !tk_strcmp(sb_fw.crash_key, sb_fw.key)))
        notif_post("Starbite OS", "Restarted After a Problem",
                   "The last start stopped unexpectedly. Details: Settings > Startup > Crash Reports.");
}
