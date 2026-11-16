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
#include "../../guest/sbshm.h"           /* 8t: the Linux guest's shared region (the GPU maps all of it) */
#include "tzcities.h"                   /* 8a: World Clock's and Weather's cities (GeoNames) */
#include "bt.h"                         /* Track A: drivers for the shell (the end of this file) */
#include "cam.h"
#include "snd.h"                         /* Track A, cp7: sound for the shell (the end of this file) */
#include "usb.h"                         /* 8b: the USB console (Settings > Developer) */
#include "survkit.h"                     /* 9l: the Survival Kit (Track D) */
#include "hrm.h"
#include "shub_more.h"
#include "bathealth.h"
#include "haptic.h"
#include "nledpol.h"
#include "face_svc.h"                     /* Face Unlock (ui/face_svc.c on kern/face.c) */
#include "face.h"
static void drv_timer(unsigned id);             /* Track A: drivers for the shell (the end of this file) */
static void snd_tick(unsigned id);              /* Track A, cp7: sound (the end of this file) */
static void snd_boot(void);                     /* cp7y: snd up (Calliope's files into RAM) at the bake's end */
static void drv_screen(int on);
static void drv_after_bake(void);
static int drv_touch_dark(void);
#define KT_DRV_BT 30                            /* KT_UI ids: Bluetooth's steps, the camera's poll, Auto-Brightness, */
#define KT_DRV_CAM 31                           /* the in-call proximity */
#define KT_DRV_AUTO 32
#define KT_DRV_PROX 33
#define KT_DRV_SND 34                           /* cp7: sound's poll (5 ms while busy) */
#define KT_DRV_JACK 35                          /* cp7: the headphone jack (50 ms while sound plays or the screen is on) */
#define KT_DRV_PREWARM 36                       /* 8a: Linux prewarmed in the dark (Track E's guest_prewarm) */
#define KT_DRV_FACE 37                          /* Face Unlock: the camera's poll + the face's work (~10 ms while busy) */
static void prewarm_arm(int screen_on);
static void face_tick(void);                    /* Face Unlock (its block after the camera's) */
static int cam_running;                         /* (the camera's block: the Camera app has it) */
static void face_init(void);

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
static uint32_t kpar_n;                     /* cp7s: parallel jobs and their time (a slow shell call's line names them) */
static uint64_t kpar_us;
static void kpar_(void (*fn)(void *, int), void *arg, int n);
static void kpar(void (*fn)(void *, int), void *arg, int n)
{
    uint64_t t0 = sb_us();
    kpar_(fn, arg, n);
    kpar_n++;
    kpar_us += sb_us() - t0;
}
static void kpar_(void (*fn)(void *, int), void *arg, int n)
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
static uint64_t bd_us_frame;                /* (the frame log reads it before its definition below: a tentative one here) */
static uint32_t cache_us_frame;             /* 8f: the same (its definition below) */
static uint64_t last_tick, boot_t0;
/* the frame statistics of the current gesture (logged per film in the self test, per gesture otherwise) */
static struct { uint32_t n, nhalf, over16, maxus, nboost, nwait; uint64_t sumus, sumwait; uint32_t hist[64]; uint8_t ms[120];
                 uint32_t shmax, shover; uint32_t tpn, tpmax, tpdrop; uint64_t tpsum; } FS;   /* P3i: the shell's own time (a touch's handling, a tick) */
/* 8a (the top ten's item 3, B's B9): touch to photon, from a finger's down or up to the next frame handed to the
 * display (queued for its latch: up to one refresh more on the glass). One touch at a time; none counted when no
 * frame follows within 500 ms (the touch changed nothing on the screen) */
static uint64_t tp_t;
static void touch_mark(int ph) { if (ph != 1 && !tp_t) tp_t = sb_us(); }

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
/* Track A, 1.0 (the user: "normal Starbite should have animations and be fast, unlike Emergency mode"): the top
 * clocks hold 1.5 s past the last motion frame in normal mode, 0.4 s (as before) when the shell says it is in a
 * conservative mode (Emergency, Survival, Extreme saving: sh_perf_mode) */
static int perf_conservative, perf_low;     /* perf_low: the user's Low Power Mode (pref power.low) */
static uint32_t perf_tail_ms(void) { return perf_conservative || perf_low ? 400U : 1500U; }
static void boost(int on)
{
    /* Track D: Extreme power saving and Last 2 % (surv_saver(), set by the Emergency switch, the 3 % trigger or a
     * remembered setting, with or without the shell) keep the A55 at its 455 MHz step: no boost, and the clock-down
     * goes to 455 MHz, not 1053 MHz (it would undo services.c's power_last2_clock) */
    if (on && surv_saver()) on = 0;
    if (on == boosted) return;
    boosted = on;
    if (!hw_dvfs(DVFS_A55, on ? 1794000U : surv_saver() ? 455000U : 1053000U))
        sb_log(on ? "next: the A55 did not take 1794 MHz" : "next: the A55 did not go back down (ACPM)");
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
    static sh_cell_t last = { -1, 0, 0, 0, 0, 0, "", "", 0, 0 };
    struct sbos_phone ph;
    kmodem_status(&ph);
    sh_cell_t c = { SH_CELL_OFF, ph.sim, 0, 0, ph.bars, 0, "", "", 0, 0 };
    struct kcell kc;
    kmodem_cell(&kc);
    c.data = kc.state;                      /* KCELL_* = SH_DATA_* */
    c.data_want = kc.want;                  /* 8z: the switches */
    c.calls = kc.calls;
    sb_strcpy(c.oper, sizeof(c.oper), kc.oper);
    if (kc.state == KCELL_FAILED)
        sb_strcpy(c.why, sizeof(c.why), kc.why);
    if (kc.want || kc.calls || kc.state != KCELL_OFF) {
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
    return wifi_user_power(on);                                  /* Track H: the switch is remembered */
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
int sh_cell_calls(int on) { return kmodem_calls(on); }   /* 8z */
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
    {
        /* Track A 9c: the real charge rate (before sh_sysinfo: a plug-in's widget shows it at once) */
        struct kcharge kc;
        charge_get(&kc);
        int st = !(b.ok && b.charger) ? 0 : kc.stopped_hot ? 2 : kc.stopped_cold ? 3 :
                 (b.pct >= 100 || kc.dtls == 3 || kc.dtls == 4) ? 4 : 1;
        sh_charge(b.ok ? b.avg_ma : 0, b.ok ? b.full_mah : 0, st);
    }
    sh_sysinfo(b.ok && b.charger, "Starbite OS Next P4a");     /* P2y: the apps' live values */
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
    if (comp.last_px && tp_t) {
        uint64_t d = sb_us() - tp_t;
        tp_t = 0;
        if (d > 500000) FS.tpdrop++;
        else { FS.tpn++; FS.tpsum += d; if (d > FS.tpmax) FS.tpmax = (uint32_t)d; }
    }
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
            ub_s(&u, " flush ");                 /* 1.0: layers made ready for the GPU in this frame: n, us, KiB */
            ub_dec(&u, comp.n_flush);
            ub_s(&u, "/");
            ub_dec(&u, comp.us_flush);
            ub_s(&u, "us/");
            ub_dec(&u, comp.b_flush >> 10);
            ub_s(&u, "KiB platform ");
            ub_dec(&u, comp.us_platform);
            ub_s(&u, " us (backdrops ");
            ub_dec(&u, bd_us_frame);
            ub_s(&u, ")");
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
            uint32_t tb = 0, tu = 0, tr = 0;         /* 8c: the CPU's binning, the bins' upload, the job's run */
            gpucomp_last_times(&tb, &tu, &tr);
            ub_s(&u, "; bin ");
            ub_dec(&u, tb);
            ub_s(&u, " up ");
            ub_dec(&u, tu);
            ub_s(&u, " run ");
            ub_dec(&u, tr);
            ub_s(&u, " cache ");                    /* 8f: a bottom-cache compose in this frame (0 none) */
            ub_dec(&u, cache_us_frame);
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
#ifndef SBOS_SELFTEST_NEXT                         /* the self test's twelve films would fill pmsg's 16 KB */
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
    if (FS.tpn) {                                /* 8a: touch to photon (to the frame queued) */
        ub_s(&u, "; touch to frame: ");
        ub_dec(&u, FS.tpn);
        ub_s(&u, " touches, mean ");
        ub_dec(&u, FS.tpsum / FS.tpn / 1000);
        ub_c(&u, '.');
        ub_dec(&u, FS.tpsum / FS.tpn / 100 % 10);
        ub_s(&u, " ms, max ");
        ub_dec(&u, FS.tpmax / 1000);
        ub_s(&u, " ms");
        if (FS.tpdrop) { ub_s(&u, ", no frame "); ub_dec(&u, FS.tpdrop); }
    }
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
/* cp7s: the platform's share of a shell call, from st_mark (the touch paths) to shell_time: ACPM exchanges, parallel
 * jobs (and whether the band workers were on), the GPU's waits; named when the call took over 50 ms (7r: a 157 ms
 * touch-down at Emergency's hold that is 0.04 ms of shell code on the Mac) */
static uint32_t stm_acpm_n, stm_kpar_n;
static uint64_t stm_acpm_us, stm_kpar_us;
static void st_mark(void) { stm_acpm_n = sb_acpm_calls; stm_acpm_us = sb_acpm_us; stm_kpar_n = kpar_n; stm_kpar_us = kpar_us; }
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
    if (us > 50000) {
        ub_s(&u, " (ACPM ");
        ub_dec(&u, sb_acpm_calls - stm_acpm_n);
        ub_s(&u, " / ");
        ub_dec(&u, (sb_acpm_us - stm_acpm_us) / 1000U);
        ub_s(&u, " ms, parallel jobs ");
        ub_dec(&u, kpar_n - stm_kpar_n);
        ub_s(&u, " / ");
        ub_dec(&u, (kpar_us - stm_kpar_us) / 1000U);
        ub_s(&u, workers_on ? " ms, workers on)" : " ms, workers off)");
    }
    sb_log(u.b);
}
/* cp7f: a named piece inside the shell's idle work (7e's self test: "idle (after a gesture)" 245-273 ms on the
 * phone, 3-4 ms on the Mac: which piece) */
void sh_slow_piece(const char *what, unsigned us)
{
    if (us / 1000 > k_turn_piece_ms) {           /* 8v: the turn's slowest named piece (the turn list keeps it) */
        k_turn_piece = what;
        k_turn_piece_ms = us / 1000;
    }
    if (us <= 20000) return;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: shell piece ");
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
    uint64_t fb0 = sb_us(), fb_boost = 0, fb_tick = 0, fb_comp = 0, fb_pres = 0, fb_idle = 0;   /* cp7: a slow frame's parts */
    next_input_now();                            /* the finger where it is now */
    if (!boosted) { uint64_t b0 = sb_us(); boost(1); fb_boost = sb_us() - b0; }   /* a wake: the kernel set the screen-on clock after the hook */
    uint64_t now = sb_ms();
    float dt = last_tick ? (float)(now - last_tick) / 1000.0f : 1.0f / 60;
    if (flip_on && comp.acquire && last_us) {
        uint64_t te = (nus - last_us + 8333) / 16667;          /* whole TE periods since the last frame */
        dt = (float)(te ? te : 1) / 60.0f;
    }
    last_tick = now;
    last_us = nus;
    st_mark();
    uint64_t st0 = tk_us();
    int mv = sh_tick(dt, (float)now);
    shell_time("tick", st0);
    fb_tick = tk_us() - st0;
    int motion = mv || sh_moving();
    uint64_t c0 = sb_us();
    tk_comp_frame(&comp, motion, 0);
    fb_comp = sb_us() - c0;
    c0 = sb_us();
    present();
    fb_pres = sb_us() - c0;
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
        {   /* cp7: the idle work timed like the shell's other calls (7b's self test: 580-1180 ms turns were here) */
            uint64_t si0 = tk_us();
            sh_idle();
            shell_time("idle (after a gesture)", si0);
            fb_idle = tk_us() - si0;
        }
        if (sh_idle_pending()) kt_set(KT_UI, 24, sb_ms(), 20, 0);   /* the rest of the idle work, a turn at a time (cp7l: 20 ms
                                                                       * apart, was 40: ~30 pieces after Emergency took 1.2 s) */
        clock_again();                                               /* P2z: the Clock's next digit */
        kt_set(KT_UI, 21, sb_ms(), perf_tail_ms(), 0);   /* back to the screen-on clock unless something moves again (1.0: 1.5 s in normal mode) */
        kt_set(KT_UI, 23, sb_ms(), 5000, 0);        /* and the band workers off after 5 s */
#ifndef SBOS_SELFTEST_NEXT
        stats_log("gesture");
        stats_reset();
#endif
    }
    uint64_t fall = sb_us() - fb0;
    if (fall > 100000) {                         /* cp7: a frame over 100 ms: which part (7b: 580-680 ms frames) */
        struct ub u;
        ub_init(&u);
        ub_s(&u, "next: a slow frame ");
        ub_dec(&u, fall / 1000);
        ub_s(&u, " ms: boost ");
        ub_dec(&u, fb_boost / 1000);
        ub_s(&u, ", shell tick ");
        ub_dec(&u, fb_tick / 1000);
        ub_s(&u, ", compose ");
        ub_dec(&u, fb_comp / 1000);
        ub_s(&u, ", present ");
        ub_dec(&u, fb_pres / 1000);
        ub_s(&u, ", idle ");
        ub_dec(&u, fb_idle / 1000);
        ub_s(&u, " ms");
        sb_log(u.b);
    }
}

static void start_ticking(void)
{
    kt_cancel(KT_UI, 21);
    kt_cancel(KT_UI, 23);
    uint64_t sb0 = sb_us();
    boost(1);
    uint64_t sb1 = sb_us();
    workers_start();
    uint64_t sb2 = sb_us();
    if (sb2 - sb0 > 50000) {                     /* cp7: a slow start of motion: the clocks (ACPM) or the band workers (PSCI) */
        struct ub u;
        ub_init(&u);
        ub_s(&u, "next: a slow motion start: boost ");
        ub_dec(&u, (sb1 - sb0) / 1000);
        ub_s(&u, " ms, band workers ");
        ub_dec(&u, (sb2 - sb1) / 1000);
        ub_s(&u, " ms");
        sb_log(u.b);
    }
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
    drv_screen(on);                                  /* Track A: the LED's seen events, Auto-Brightness, proximity */
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
        prewarm_arm(0);                              /* 8a: Linux prewarmed after 15 s of dark (once a boot) */
        sh_idle();                                   /* P2v: the half canvas and the lock's caches now, in the dark
                                                      * (the wake's first frame spent 8 ms on them) */
#ifndef SBOS_SELFTEST_NEXT
        stats_reset();
#endif
        return;
    }
    prewarm_arm(1);
    status_now();
    sh_wake();
    if (sh_idle_pending()) kt_set(KT_UI, 24, sb_ms(), 200, 0);   /* cp7: the idle work the dark left (after the wake's frames) */
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
static int next_power_held(void);
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
    /* a big move's frame: one level a frame. 8f (Track A, 8c2's split: a full-resolution motion frame's backdrops
     * took ~4.8 ms of its ~17-25, the GPU's run ~11.4): any frame while something moves, too; the glass over moving
     * content catches up a level a frame, all of it at rest */
    int remade = 0, budget = comp.gpu_half || sh_moving() ? 1 : GPUCOMP_LEVELS;
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

static int next_gpu_(void *ctx, uint32_t *dst, const tk_gpu_item *it, int n, const tk_irect *dmg, int ndmg, uint64_t *us);
static uint64_t bd_us_frame;               /* 1.0: this frame's live backdrops (the frame log) */
static uint32_t cache_us_frame;            /* 8f: this frame's bottom-cache compose, if one ran (the frame log) */
static int next_gpu(void *ctx, uint32_t *dst, const tk_gpu_item *it, int n, const tk_irect *dmg, int ndmg, uint64_t *us)
{
    uint64_t t = sb_us();
    cache_us_frame = 0;
    int r = next_gpu_(ctx, dst, it, n, dmg, ndmg, us);
    comp.us_platform = sb_us() - t;
    return r;
}
static int next_gpu_(void *ctx, uint32_t *dst, const tk_gpu_item *it, int n, const tk_irect *dmg, int ndmg, uint64_t *us)
{
    (void)ctx;
    uint64_t tb = sb_us();
    if (n > 0 && n <= TK_GPU_MAX) it = live_backdrops(it, n, &dmg, &ndmg);
    bd_us_frame = sb_us() - tb;
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
        /* 8f: a valid cache isn't grown while anything moves (a grow is a second full compose in that frame: 8c2's
         * emergency frames at 17-22 ms with a GPU run of 3-6 ms); a broken one is rebuilt as before */
        if (stable_k >= 2 && want >= 1 && (!valid || (want > cache_n && !sh_moving()))) {
            uint64_t u2 = 0, tc0 = sb_us();
            int okc = gpucomp_compose(cache, 0xff000000U, (const struct gc_quad *)it, (unsigned)want, &u2);
            cache_us_frame = (uint32_t)(sb_us() - tc0);
            if (okc) {
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
/* 8a (Track A, multi-touch): fingers 1.. (finger 0 stays sh_touch's single touch). The shell defines sh_touch_finger
 * when it has two-finger gestures (pinch); until then they are dropped. ph: 0 down, 1 move, 2 up, as sh_touch */
__attribute__((weak)) void sh_touch_finger(int finger, int ph, float x, float y, float t_ms) { (void)finger; (void)ph; (void)x; (void)y; (void)t_ms; }
static void next_finger(int type, int id, int x, int y)
{
    if (!ready) return;
    int ph = type == KIN_PRESS ? 0 : type == KIN_MOVE ? 1 : 2;
    sh_touch_finger(id, ph, x / TK_PT, y / TK_PT, (float)sb_ms());
    start_ticking();
}
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
    kui_power_hook = next_power_held;            /* Track A, 1.0: Power held: the shell's power sheet */
    kui_finger_hook = next_finger;               /* 8a: fingers 1.. for the shell's pinch and other two-finger gestures */
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
    /* 8t: the whole shared region (8s put its 1080 surfaces in the v3 free tail, outside the old 8 MiB window: the
     * front buffer had no GPU address, so the window fell back to the CPU's scaled copy: soft and slow) */
    gpucomp_add_ext((const void *)(uintptr_t)SBG_S9_SHM_BASE, SBG_S9_SHM_SIZE);
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
    cam_isp_set_par(kpar);                           /* the camera's pipeline v2 on the band workers (9i: 77 ms on one A55) */
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
    snd_boot();                                      /* cp7y: ~0.3 s once, still behind the boot screen */
    kt_cancel(KT_UI, 22);
    baking = 0;
    status_now();
    sh_bright_now(panel_bright_level);
    sh_restore();                                    /* Track A: what the shell keeps (settings_restore ran before) */
    boot_notes();                                    /* Track A, 1.0: a safe start, or the last start's crash */
    perf_low = sh_pref_int("power.low", 0) == 1;     /* Track A, 1.0: Low Power Mode kept */
    drv_after_bake();                                /* Track A: the haptic level, Auto-Brightness, the LED policy */
    face_init();                                     /* Face Unlock: the pack checked, the kept face found */
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
    if (ready) {
        uint64_t g0 = tk_us();
        int gn = gwin_now();
        shell_time("gwin_now (Linux's window state)", g0);   /* cp7t: 7s: a 418 ms "UI's draw" turn with the screen off at
                                                              * Linux's HELLO (455 MHz): named when it is this */
        if (gn && !ticking && K.screen_on) {
            uint64_t f0 = tk_us();
            tk_comp_frame(&comp, 0, 0);
            present();
            shell_time("a Linux window frame", f0);
        }
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
    if (e->type == KIN_UP || e->type == KIN_DOWN) {   /* 8w: the volume keys: the shell's volume (and its slider) */
        sh_key(e->type == KIN_UP ? SH_KEY_VOL_UP : SH_KEY_VOL_DOWN);
        start_ticking();
        return;
    }
    if (e->type == KIN_SHOT) {                       /* 8w: Power + Volume Up: the frame on screen into Photos */
        const tk_bmp *f = tk_comp_shown(&comp);
        int r = f && f->px && f->stride == f->w ? sh_photo_save(f->px, f->w, f->h) : -1;
        sb_log(r == 0 ? "next: a screenshot, kept in Photos" : "next: a screenshot was not kept");
        sh_key(r == 0 ? SH_KEY_SHOT : SH_KEY_SHOT_FAILED);
        start_ticking();
        return;
    }
    int ph = e->type == KIN_PRESS ? 0 : e->type == KIN_MOVE ? 1 : e->type == KIN_RELEASE ? 2 : -1;
    if (ph < 0) return;
    static int dark_gesture;                         /* Track A: a touch that began with the panel dark at the ear */
    if (ph == 0) dark_gesture = drv_touch_dark();
    if (dark_gesture) {
        if (ph == 2) dark_gesture = 0;
        return;                                      /* the cheek: dropped, press to release */
    }
    st_mark();
    touch_mark(ph);
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
        if (fsvc_busy()) { kt_set(KT_UI, 21, sb_ms(), 300, 0); return; }   /* Face Unlock's work: the top clock */
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
        if (!K.screen_on)                            /* cp7 (7c's self test: 1854 ms here at 455 MHz with the screen
                                                      * off): no idle UI work nobody sees; the wake starts it again */
            return;
        if (!ticking && !sh_moving()) {
            uint64_t si0 = tk_us();
            sh_idle();
            shell_time("idle", si0);                  /* cp7: named when it is slow */
            if (sh_idle_pending()) kt_set(KT_UI, 24, sb_ms(), 20, 0);   /* cp7l: 20 ms apart (was 40) */
        } else
            kt_set(KT_UI, 24, sb_ms(), 200, 0);
        return;
    }
    if (ready && id == 23) {
        if (fsvc_busy() || cam_running) { kt_set(KT_UI, 23, sb_ms(), 2000, 0); return; }   /* Face Unlock / the camera use the cores */
        if (!ticking && !sh_moving()) workers_stop();
        return;
    }
    if (ready && id == 26) { (void)kupdate_after_start(); return; }   /* Track A update: 10 s after the home screen */
    if (id >= KT_DRV_BT && id <= KT_DRV_FACE) { drv_timer(id); return; }   /* Track A: drivers for the shell */
    ui_ktimer(id);
}

void next_kminute(void)
{
    if (baking) return;
    if (!ready) { ui_kminute(); return; }
    status_now();
    lastos_now();                                    /* helper D: once */
    if (!K.screen_on)                                /* 8a (the drivers' run 9: "timer 7/0" 496-1079 ms every minute
                                                      * with the screen off at 455 MHz: a frame nobody sees and the
                                                      * idle work rebuilding Control Centre's pictures): the time is
                                                      * noted; the wake draws it and starts the idle work again */
        return;
    if (!ticking) {
        tk_comp_frame(&comp, 0, 0);
        present();
        /* 8a2's self test: "timer 7/0" 283 ms with the screen on: the whole idle work in the minute's turn. Now the
         * idle timer takes it in slices between frames, as everywhere else */
        if (sh_idle_pending()) kt_set(KT_UI, 24, sb_ms(), 20, 0);
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
        fsvc_passcode(digits);                       /* Face Unlock's key (its own turn, ~45 ms) */
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
    if (!r) fsvc_passcode_set(old, nw);              /* Face Unlock's key rewrapped (or forgotten with the passcode) */
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
/* 8a (Track A): Notes' and Contacts' documents (kern/docs.c), saved with the settings; the self test: dry runs */
int sh_doc_put(int coll, unsigned id, const void *p, unsigned n)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)coll; (void)id; (void)p;
    if (n > DOC_MAX) return -1;
    prefs_dry();
    return 0;
#else
    int r = docs_put(coll, id, p, n);
    if (r > 0) settings_changed();
    if (r < 0) sb_log("next: docs: a document was not kept (too big, or its collection is full)");
    return r < 0 ? -1 : 0;
#endif
}
int sh_doc_get(int coll, unsigned id, void *p, unsigned cap) { return docs_get(coll, id, p, cap); }
int sh_doc_count(int coll) { return docs_count(coll); }
unsigned sh_doc_id_at(int coll, int k) { return docs_id_at(coll, k); }
unsigned sh_doc_new_id(int coll) { return docs_new_id(coll); }
void sh_doc_usage(int coll, unsigned *used, unsigned *budget)
{
    uint32_t u = 0, b = 0;
    docs_usage(coll, &u, &b);
    if (used) *used = u;
    if (budget) *budget = b;
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
/* 8a: the cities (kern/tzcities.c, GeoNames CC BY 4.0) */
int sh_city_count(void) { return tzcities_n; }
int sh_city_find(const char *query, int *out, int max) { return tzc_find(query, out, max); }
int sh_city_get(int i, const char **name, const char **country, int *tz, int *lat_e4, int *lon_e4)
{
    if (i < 0 || i >= tzcities_n) return -1;
    const struct tzcity *c = &tzcities[i];
    if (name) *name = c->name;
    if (country) *country = tzc_country[c->country];
    if (tz) *tz = c->zone;
    if (lat_e4) *lat_e4 = c->lat_e4;
    if (lon_e4) *lon_e4 = c->lon_e4;
    return 0;
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
static int call_declined;                            /* Track A: the LED: a declined call is not a missed one */
int sh_hangup(void)
{
    sb_log("next: comms: hang up (on the screen)");
    struct kcall c;
    kmodem_call(&c);
    if (c.state == KCALL_INCOMING) call_declined = 1;
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
_Static_assert((int)KSMS_RECEIVED == (int)SH_SMS_RECEIVED && (int)KSMS_SENDING == (int)SH_SMS_SENDING &&
               (int)KSMS_SENT == (int)SH_SMS_SENT && (int)KSMS_FAILED == (int)SH_SMS_FAILED &&
               (int)KSMS_DELIVERED == (int)SH_SMS_DELIVERED && (int)KSMS_NOT_DELIVERED == (int)SH_SMS_UNDELIVERED,
               "P3x: shell.h's SH_SMS_* are kern.h's KSMS_* (Track G 1c2e473)");
_Static_assert(KSMS_MAX == SH_SMS_MAX, "P3x: the shell lists what the kernel's store keeps");
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
        if (pushed && last.state == KCALL_INCOMING && c.state != KCALL_INCOMING && c.state != KCALL_ACTIVE &&
            c.state != KCALL_HELD && !call_declined)
            nledpol_event(NLEDPOL_MISSED);           /* Track A: the LED: it rang and was not answered */
        if (c.state == KCALL_INCOMING && last.state != KCALL_INCOMING) call_declined = 0;
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
    static int unread = -1;                          /* Track A: the LED: a new unread message (the first look, at */
    if (g != gen || unread < 0) {                    /* the home screen: the restored store, only counts) */
        if (g != gen) { gen = g; sh_sms_changed(g); ch = 1; }
        static struct ksms m;
        int n = 0;
        for (int i = 0; i < kmodem_sms_count(); i++)
            if (kmodem_sms_get(i, &m) >= 0 && m.incoming && m.unread) n++;
        if (unread >= 0 && n > unread) nledpol_event(NLEDPOL_MESSAGE);
        unread = n;
    }
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
static const char *const FILM_NAME[14] = { "unlock", "group", "focus", "holds", "pages", "cc", "app", "phone", "messages", "emergency", "wheels", "sleep",
                                           "pages-half", "pages-full" };   /* 8a: the home swipe with and without half-resolution GPU frames */
static uint32_t film_halves0;                   /* comp.gpu_halves at the film's start */
/* 8c (Track A, spec-glass-8c.md rank 1): the glass calibration stills after the films, for the phone photos: the
 * lock, home and Control Centre, each at LENS_GAIN .33, .66 and 1, 3 s apiece, each logged with its time so the
 * camera's shots can be matched ("next: glass still ... at +N ms"). gcal: -1 the films, 0.. the stills, 10 done */
static int gcal = -1;
static uint64_t gcal_t;
/* tools/films.txt's scripts (pt, ms) */
static const step S_UNLOCK[] = { { 0, 0, 0, 0, 0, 400 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 1300 } };
/* B10: the vendor cards again (B4's list from y 320: Apple 320, Microsoft 413, Google 491, Linux 584, Starbite 677):
 * Apple opens (its head 36 lower, "Show less" above it at x >= 285), "Show less" folds it, the list scrolls to its
 * end, Starbite opens (and scrolls into view), its first row (Starbite OS, "You are in Starbite OS"). The focus film:
 * Microsoft opens, its Windows 11 row (525..593) opens the focus sheet, the slide starts it */
/* 8a: S_GROUP and S_FOCUS open the lock's Systems card first (the user's lock: one card that expands in place) */
static const step S_GROUP[] = { { 0, 0, 0, 0, 0, 300 }, { 1, 195, 645, 0, 0, 80 }, { 0, 0, 0, 0, 0, 700 }, { 1, 195, 354, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1000 }, { 1, 330, 334, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 900 }, { 2, 200, 650, 200, 380, 250 }, { 0, 0, 0, 0, 0, 1200 }, { 1, 195, 578, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 1800 }, { 1, 195, 464, 0, 0, 80 }, { 0, 0, 0, 0, 0, 1200 } };
static const step S_FOCUS[] = { { 0, 0, 0, 0, 0, 300 }, { 1, 195, 645, 0, 0, 80 }, { 0, 0, 0, 0, 0, 700 }, { 1, 195, 444, 0, 0, 80 }, { 0, 0, 0, 0, 0, 900 }, { 1, 195, 559, 0, 0, 80 },
                                { 0, 0, 0, 0, 0, 1000 }, { 2, 65, 743, 345, 743, 550 }, { 0, 0, 0, 0, 0, 2600 } };
static const step S_HOLDS[] = { { 0, 0, 0, 0, 0, 300 }, { 3, 71, 727, 0, 0, 650 }, { 0, 0, 0, 0, 0, 700 }, { 3, 319, 727, 0, 0, 950 },
                                { 0, 0, 0, 0, 0, 900 } };
static const step S_PAGES[] = { { 0, 0, 0, 0, 0, 300 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 900 },
                                { 2, 320, 450, 60, 450, 200 }, { 0, 0, 0, 0, 0, 900 }, { 2, 60, 450, 330, 460, 220 }, { 0, 0, 0, 0, 0, 1200 } };
/* 8b: S_CC on iOS 27's page 1 (the Flashlight at row 4, the Brightness slider under Now Playing) */
static const step S_CC[] = { { 0, 0, 0, 0, 0, 300 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 900 }, { 2, 320, 20, 320, 330, 260 },
                             { 0, 0, 0, 0, 0, 900 }, { 1, 70, 483, 0, 0, 80 }, { 0, 0, 0, 0, 0, 500 }, { 2, 236, 400, 236, 300, 300 },
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
/* cp7 (Track B, films.txt's wheels): the Clock's Add Alarm wheels: the hours dragged up, the minutes down (each a
 * momentum and a settle at 60 fps), Cancel, home */
static const step S_WHEELS[] = { { 0, 0, 0, 0, 0, 300 }, { 2, 195, 795, 195, 330, 220 }, { 0, 0, 0, 0, 0, 900 }, { 1, 238, 289, 0, 0, 80 },
                                 { 0, 0, 0, 0, 0, 1100 }, { 1, 151, 749, 0, 0, 80 }, { 0, 0, 0, 0, 0, 900 }, { 1, 348, 66, 0, 0, 80 },
                                 { 0, 0, 0, 0, 0, 900 }, { 2, 162, 290, 162, 200, 220 }, { 0, 0, 0, 0, 0, 1300 },
                                 { 2, 248, 280, 248, 360, 220 }, { 0, 0, 0, 0, 0, 1300 }, { 1, 58, 66, 0, 0, 80 }, { 0, 0, 0, 0, 0, 900 },
                                 { 2, 195, 795, 200, 450, 260 }, { 0, 0, 0, 0, 0, 1200 } };
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

extern float lens_gain;                         /* kern/lens.h (weak, 1 by default) */
static int glass_cal_step(void)
{
    static const float GAIN[3] = { .33f, .66f, 1.0f };
    static const char *const VIEW[3] = { "lock", "home", "control centre" };
    if (gcal >= 10) return 0;
    if (gcal > 0 && sb_ms() - gcal_t < 3000) return 1;
    if (gcal == 9) {
        lens_gain = 1.0f;
        sh_set("reset", "", 0);
        need_full = 1;
        next_kdraw();
        sb_log("next: glass stills: done (gain back to 1)");
        {   /* 8c+: the arena at the end (the drivers: snd's 3 MiB firmware cache at boot comes from it; the pmsg ring
             * drops the boot's own "arena free" line) */
            struct ub u;
            ub_init(&u);
            ub_s(&u, "next: arena free at the self test's end: ");
            ub_dec(&u, sb_arena_free() >> 10);
            ub_s(&u, snd_reserved() ? " KiB; snd's firmware cache reserved at boot: yes (its 3 MiB already out of this)"
                                    : " KiB; snd's firmware cache reserved at boot: NO");
            sb_log(u.b);
        }
        gcal = 10;
        return 0;
    }
    int v = gcal / 3, g = gcal % 3;
    if (!sbos_film_now || sbos_film_now[0] != 'g' || sbos_film_now[1] != 'l') sbos_film_t0 = sb_ms();
    sbos_film_now = "glass stills";
    lens_gain = GAIN[g];
    if (g == 0) {
        sh_set("reset", "", 0);
        if (v >= 1) sh_set("unlock", "", 1);
        if (v == 2) sh_set("cc", "", 1);
    }
    need_full = 1;
    next_kdraw();
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: glass still ");
    ub_s(&u, VIEW[v]);
    ub_s(&u, ", glass strength ");
    ub_dec(&u, (uint64_t)(GAIN[g] * 100 + .5f));
    ub_s(&u, " %");
    ub_s(&u, " at +");
    ub_dec(&u, sb_ms() - st_t0);
    ub_s(&u, " ms (3 s)");
    sb_log(u.b);
    gcal_t = sb_ms();
    gcal++;
    return 1;
}

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
    case 10: F.s = S_WHEELS; F.n = (int)(sizeof S_WHEELS / sizeof S_WHEELS[0]); break;
    case 11: F.s = S_SLEEP; F.n = 13; break;
    default: F.s = S_PAGES; F.n = 7; break;     /* 8a: 12 pages-half, 13 pages-full (a measurement, not the gate's) */
    }
    tk_gpu_no_half = film == 13;
    sbos_film_now = FILM_NAME[film];
    /* each film from the plain lock screen */
    sh_set("reset", "", 0);
    need_full = 1;
    next_kdraw();
    for (int i = 0; i < 8 && sh_idle_pending(); i++) { hw_wdt_feed(); sh_idle(); }   /* as the idle time would */
    sh_idle();
    F.t0 = sb_ms();   /* cp7: the film's clock after its reset and idle work (~0.5 s): its first steps no longer pass
                       * "during a stall" (the unlock swipe of the phone film was skipped so, Track B's finding) */
    sbos_film_t0 = F.t0;                   /* 8f: the slow turns' times count from here (the films' scripts do) */
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
    film_halves0 = comp.gpu_halves;
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
        K.user_turn_ms = 0; K.user_turn_what = 0; K.turns_100 = 0; K.tl_n = 0;
        for (unsigned i_ = 0; i_ < 18; i_++) K.turns_100_by[i_] = 0;   /* cp7m: the summary counts from the films (7l: the
                                                                     * bake's own turns, timer 1/22, counted 40 / 515 ms) */
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
        sb_log("selftest: next: the fourteen films (unlock, group, focus, holds, pages, cc, app, phone, messages, emergency, wheels, sleep, pages-half, pages-full) with synthetic touches; sleep presses the real Power path; pages-full is a measurement (no half-resolution GPU frames), not the gate's");
        film_start(0);
        return 1;
    }
    float t = (float)(sb_ms() - F.t0);
    while (F.i < F.n && t >= F.s0 + F.s[F.i].ms) {
        const step *p = &F.s[F.i];
        if (F.down && p->kind == 5) F.down = 0;
        else if (F.down) {
            { st_mark(); touch_mark(2); uint64_t st0 = tk_us(); sh_touch(2, p->kind == 2 ? p->x1 : p->x0, p->kind == 2 ? p->y1 : p->y0, (float)sb_ms()); shell_time("touch up", st0); }
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
            if (!F.down) { st_mark(); touch_mark(0); uint64_t st0 = tk_us(); sh_touch(0, p->x0, p->y0, (float)sb_ms()); shell_time("touch down", st0); F.down = 1; start_ticking(); }
            else if (p->kind == 2) {
                float u = (t - F.s0) / p->ms;
                sh_touch(1, p->x0 + (p->x1 - p->x0) * u, p->y0 + (p->y1 - p->y0) * u, (float)sb_ms());
            }
        }
        return 1;
    }
    if (ticking || sh_moving()) return 1;          /* let it settle */
    if (gcal < 0) {                              /* the films (until the last one ends) */
    stats_log(FILM_NAME[F.film]);
    if (F.film >= 12) {                          /* 8a: what full-resolution motion costs (the top ten's item 4) */
        struct ub u;
        ub_init(&u);
        ub_s(&u, "next: frames: ");
        ub_s(&u, FILM_NAME[F.film]);
        ub_s(&u, ": GPU frames at half resolution ");
        ub_dec(&u, comp.gpu_halves - film_halves0);
        ub_s(&u, F.film == 13 ? " (none allowed: a measurement, not part of the gate)" : " (allowed, as in use)");
        sb_log(u.b);
    }
    if (F.film == 11 && !K.screen_on) {          /* sleep: the last film (cp7: wheels is 10) */
        sb_log("selftest: FAILED: next: the sleep film ended with the screen off");
        st_failed = 1;
        power_screen_set(1);
    }
    if (F.film < 13) { film_start(F.film + 1); return 1; }
    tk_gpu_no_half = 0;
    gcal = 0;
    }
    if (glass_cal_step()) return 1;
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
int sh_linux_gpu_reaches(const void *p)
{
#ifndef SBOS_NEXT_NOGPU
    return p && gpucomp_va(p) != 0;
#else
    (void)p;
    return 0;
#endif
}
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
    kt_set(KT_UI, 26, sb_ms(), 10000, 0);           /* Track A update: the commit (or the rollback's notice) in 10 s */
    if (sbos_safe_start())
        notif_post("Starbite OS", "Safe Start",
                   "Started without restoring Wi-Fi. Nothing is saved until you restart.");
    else if (sb_fw.valid && sb_fw.crash_sev >= 2U && sb_fw.crash_key[0] &&
             (!tk_strcmp(sb_fw.crash_key, "starbite-os") || !tk_strcmp(sb_fw.crash_key, sb_fw.key)))
        notif_post("Starbite OS", "Restarted After a Problem",
                   "The last start stopped unexpectedly. Details: Settings > Startup > Crash Reports.");
}

/* ---- Track A, 1.0: Restart and Shut Down from the shell ---- */
int sh_restart(void)
{
#ifdef SBOS_SELFTEST_NEXT
    sb_log("next: restart: refused (a dry run: the self test)");
    return -1;
#else
    sb_log(sb_fw.valid && sb_fw.key[0] ? "next: restart (this OS again)" : "next: restart (to the boot menu: an older loader)");
    if (sb_fw.valid && sb_fw.key[0])
        power_boot_os(sb_fw.key);       /* does not return */
    power_restart(1);
    return -1;                          /* back here: refused */
#endif
}
int sh_power_off(void)
{
#ifdef SBOS_SELFTEST_NEXT
    sb_log("next: power off: refused (a dry run: the self test)");
    return -1;
#else
    sb_log("next: power off (the user's slide)");
    power_off_now();
    return -1;                          /* back here: refused (power_off_now said why) */
#endif
}
/* Power held: the shell's power sheet (Track B's P3x defines sh_power_held; this weak one is the fallback) */
__attribute__((weak)) void sh_power_held(void) { sb_log("next: Power held: no power sheet in this shell yet"); }
static int next_power_held(void)
{
    if (!ready) return 0;                        /* the bake: the classic menu (never drawn: nothing to do) */
    sh_power_held();
    start_ticking();                             /* P3x: the sheet fades in (the screen may have just woken) */
    return 1;
}

/* ---- Track A: Software Update ---- (sbos-core-api.md 8) shell.h's sh_update_* on kern/update.c, which does the work on
 * Track J's libsb half (sbfw_api.h: the slots, the one-shot, the commit). Staging goes a 1 MiB chunk per call (read from
 * the card, hashed, written into the inactive slot through a window grant) through a buffer taken from the UI heap for
 * the staging only. The commit is 10 s after the home screen (boot_notes arms KT_UI 26): a start that crashes or hangs
 * before it never commits, and the loader rolls back. In the self-test build update.c reads no card and every write,
 * the arming and the restart are logged dry runs (sbos_selftest_enabled). */
#define UPD_CHUNK (1U << 20)
static struct kupdate kup;
static uint8_t *upd_buf;
static void upd_out(struct sh_update *u)
{
    if (!u) return;
    tk_memset(u, 0, sizeof *u);
    sb_strcpy(u->version, sizeof u->version, kup.version);
    sb_strcpy(u->notes, sizeof u->notes, kup.notes);
    sb_strcpy(u->why, sizeof u->why, kup.why);
    u->size = kup.size > 0xffffffffULL ? 0xffffffffU : (unsigned)kup.size;
    u->state = kup.state;
    u->pct = kup.pct;
}
int sh_update_check(struct sh_update *u)
{
    hw_wdt_feed();
    int r = kupdate_check(&kup);
    upd_out(u);
    return r == 1;
}
int sh_update_stage(struct sh_update *u)
{
    if ((kup.state == KUP_FOUND || kup.state == KUP_STAGING) && !upd_buf) {
        upd_buf = tk_alloc(UPD_CHUNK);
        if (!upd_buf) sb_log("next: update: no 1 MiB in the UI heap for the copy");
    }
    hw_wdt_feed();
    kupdate_stage(&kup, upd_buf, upd_buf ? UPD_CHUNK : 0);
    if (kup.state != KUP_STAGING && upd_buf) {
        tk_free(upd_buf);
        upd_buf = 0;
    }
    upd_out(u);
    return kup.state;
}
int sh_update_install(void)
{
    char why[80];
#ifdef SBOS_SELFTEST_NEXT
    sb_log("next: update: would arm the staged update and restart (a dry run: the self test)");
#endif
    int r = kupdate_install(why, sizeof why);
    if (r < 0) sb_logf("next: update: not installed: ", why);
    return r < 0 ? -1 : 0;
}
/* ---- end of Track A: Software Update ---- */

/* ---- Track A: drivers for the shell ---- shell.h's sh_bt_*, sh_cam_*, the sensors, Auto-Brightness, the in-call
 * proximity, battery health and the haptic patterns on the drivers track's kernel drivers (docs/drivers/), plus
 * the notification LED's policy (kern/nledpol.c). Nothing here runs until the shell calls it, except, after the bake:
 * the haptic level from the pref haptic.level (RAM only), Auto-Brightness when the pref display.auto is 1 (0 by
 * default) and the LED policy; none of those three in any self-test image. Each running part has its own KT_UI timer
 * (ids 30..33: Bluetooth's steps at bt_wait_ms, the camera's poll every 10 ms while open, Auto-Brightness every 2 s
 * while the screen is on, the proximity every 200 ms in a call), so the UI loop never waits on hardware. In the
 * self-test build (SBOS_SELFTEST_NEXT) every call that would power hardware is refused with a logged dry run.
 * Not wired: sound (snd_*) and USB (their probes have not passed), Bluetooth pairing. */
#ifdef SBOS_SELFTEST_NEXT
static void drv_dry(const char *what)
{
    static const char *said[16];                     /* each kind once: the sensors are asked every frame */
    for (int i = 0; i < 16; i++) {
        if (said[i] == what) return;
        if (!said[i]) { said[i] = what; break; }
    }
    sb_logf("next: drivers: refused (a dry run: the self test): ", what);
}
#define DRV_DRY(what, ret) do { drv_dry(what); return ret; } while (0)
#define DRV_DRYV(what) do { drv_dry(what); return; } while (0)
#define DRV_DRY_WHY "A dry run: the self test"
#else
#define DRV_DRY(what, ret) do { } while (0)
#define DRV_DRYV(what) do { } while (0)
#endif

/* -- Bluetooth: scanning only. bt_set_module(wifi_module()) once Wi-Fi has read its module (wifi-track-h's
 * kern/wifi.c; until that is in the trunk this weak one says "unknown" and bt.c keeps its default, SEMCO: S9 #1's) */
int wifi_module(void);
__attribute__((weak)) int wifi_module(void) { return -1; }
#define BT_SCAN_LE_MS 6000U
#define BT_SCAN_INQ_MS 5000U
#define BT_SCAN_FOR_MS 60000U                        /* sh_bt_scan(1) repeats scans for at most this long */
#define BT_SCAN_REST_MS 2000U
static struct {
    int scan, scanning;                              /* the shell wants scans; one of ours runs */
    uint64_t until, next_at;
    unsigned nsnap;                                  /* the last whole scan's list, shown while the next one fills */
    struct { char name[33]; int rssi; } snap[BT_MAX_DEV];
} BTS;
static void bt_arm(unsigned ms) { kt_set(KT_UI, KT_DRV_BT, sb_ms(), ms ? ms : 1, 0); }
static void bt_snapshot(void)
{
    unsigned n = bt_ndev();
    if (n > BT_MAX_DEV) n = BT_MAX_DEV;
    BTS.nsnap = 0;
    for (unsigned i = 0; i < n; i++) {
        const struct bt_dev *d = bt_dev_at(i);
        if (!d) break;
        sb_strcpy(BTS.snap[BTS.nsnap].name, sizeof BTS.snap[0].name, d->name);
        BTS.snap[BTS.nsnap].rssi = d->has_rssi ? d->rssi : -127;
        BTS.nsnap++;
    }
}
static unsigned bt_next_ms;                          /* the soonest step this tick asked for (0: none) */
static void bt_want(unsigned ms) { ms = ms ? ms : 1; if (!bt_next_ms || ms < bt_next_ms) bt_next_ms = ms; }
static void bt_tick_scan(void)
{
    if (!bt_step()) { bt_want(bt_wait_ms()); return; }  /* starting or scanning: again when it asks */
    int s = bt_state();
    if (BTS.scanning) {
        BTS.scanning = 0;
        bt_snapshot();
        BTS.next_at = sb_ms() + BT_SCAN_REST_MS;
    }
    if (s != BT_READY) { BTS.scan = 0; return; }      /* off or failed (sh_bt_why) */
    if (!BTS.scan) return;
    uint64_t now = sb_ms();
    if (now >= BTS.until) { BTS.scan = 0; sb_log("next: bluetooth: scanning stopped (60 s)"); return; }
    if (now < BTS.next_at) { bt_want((unsigned)(BTS.next_at - now)); return; }
    if (bt_scan(BT_SCAN_LE_MS, BT_SCAN_INQ_MS) < 0) { BTS.scan = 0; return; }
    BTS.scanning = 1;
    bt_want(1);
}
/* the drivers track's keyboard (kern/bt_pair.c): while one is pairing, connected or paired, bt_poll_ms() > 0 and the
 * chip is stepped that often even when no scan runs (its events and key reports are read in bt_step) */
static void bt_tick(void)
{
    bt_next_ms = 0;
    bt_tick_scan();
    unsigned p = bt_poll_ms();
    if (p) bt_want(p);
    if (bt_next_ms) bt_arm(bt_next_ms);
}
int sh_bt_on(int on)
{
    if (!on) {
        kt_cancel(KT_UI, KT_DRV_BT);
        tk_memset(&BTS, 0, sizeof BTS);
        if (bt_state() != BT_OFF) bt_off();          /* pins and registers back as found */
        return 0;
    }
    DRV_DRY("Bluetooth on", -1);
    static struct bt_hal h;
    static int hal;
    if (!hal) { bt_hal_libsb(&h); bt_hal_set(&h); hal = 1; }
    int m = wifi_module();
    if (m >= 0) bt_set_module(m);
    if (bt_on() < 0) return -1;
    sb_log("next: bluetooth: on (the shell)");
    bt_arm(1);                                       /* (a paired keyboard: page scan goes on when the chip is up) */
    return 0;
}
int sh_bt_state(void)
{
    int s = bt_state();
    return s == BT_STARTING ? 1 : s == BT_READY || s == BT_SCANNING ? 2 : s == BT_FAILED ? -1 : 0;
}
const char *sh_bt_why(void) { return bt_why(); }
int sh_bt_scan(int on)
{
    if (!on) { BTS.scan = 0; return 0; }             /* the scan that runs ends by itself */
    DRV_DRY("Bluetooth scan", -1);
    int s = bt_state();
    if (s != BT_STARTING && s != BT_READY && s != BT_SCANNING) return -1;
    BTS.until = sb_ms() + BT_SCAN_FOR_MS;
    if (!BTS.scan) BTS.next_at = 0;
    BTS.scan = 1;
    if (s == BT_READY) bt_arm(1);                    /* else: the start's (or the scan's) own end goes on */
    return 0;
}
static int bt_live(void) { return bt_state() == BT_SCANNING && bt_ndev() >= BTS.nsnap; }
int sh_bt_count(void)
{
    if (bt_state() == BT_OFF) return 0;
    return bt_live() ? (int)bt_ndev() : (int)BTS.nsnap;
}
int sh_bt_get(int i, char *name, int cap, int *rssi)
{
    if (i < 0 || i >= sh_bt_count()) return -1;
    const char *n;
    int r;
    if (bt_live()) {
        const struct bt_dev *d = bt_dev_at((unsigned)i);
        if (!d) return -1;
        n = d->name;
        r = d->has_rssi ? d->rssi : -127;
    } else {
        n = BTS.snap[i].name;
        r = BTS.snap[i].rssi;
    }
    if (name && cap > 0) sb_strcpy(name, (unsigned)cap, n);
    if (rssi) *rssi = r;
    return 0;
}

/* -- the front camera: cam_poll every 10 ms while it runs (each poll at most ~100 ms) */
static int cam_running;
static int face_cam;                            /* Face Unlock has the camera (its block below) */
static void cam_tick(void)
{
    workers_start();                                 /* the camera's pipeline in four bands */
    int r = cam_poll();
    if (r > 0) { kt_set(KT_UI, KT_DRV_CAM, sb_ms(), 10, 0); return; }
    cam_running = 0;
    if (r < 0) sb_logf("next: camera: stopped: ", cam_why());
}
int sh_cam_open(int w, int h)
{
    DRV_DRY("the front camera", -1);
    if (w < 8 || h < 8) return CAM_EARG;
    if (face_cam) return CAM_ESTATE;               /* Face Unlock has it (the lock or its set-up) */
    struct cam_req q = { CAM_FRONT, (unsigned)w, (unsigned)h, CAM_PORTRAIT_SELFIE, 1, 0, 0 };
    int r = cam_open(&q);
    if (r < 0) { sb_logf("next: camera: not opened: ", cam_why()); return r; }
    cam_running = 1;
    kt_set(KT_UI, KT_DRV_CAM, sb_ms(), 1, 0);
    return 0;
}
const uint32_t *sh_cam_frame(int *w, int *h, unsigned *seq)
{
    struct cam_pic p;
    if (!cam_running || !cam_frame(&p) || p.stride != p.w) return 0;
    if (w) *w = (int)p.w;
    if (h) *h = (int)p.h;
    if (seq) *seq = p.seq;
    return p.px;
}
int sh_cam_still(void)
{
    DRV_DRY("a still", -1);
    return cam_running && cam_still() == 0 ? 0 : -1;
}
const uint32_t *sh_cam_still_get(int *w, int *h, const unsigned char **bmp, unsigned *bmp_len)
{
    struct cam_shot s;
    if (!cam_running || !cam_still_get(&s) || s.stride != s.w) return 0;
    if (w) *w = (int)s.w;
    if (h) *h = (int)s.h;
    if (bmp) *bmp = s.bmp;
    if (bmp_len) *bmp_len = s.bmp_len;
    return s.px;
}
const char *sh_cam_why(void)
{
#ifdef SBOS_SELFTEST_NEXT
    return DRV_DRY_WHY;
#else
    return cam_why();
#endif
}
void sh_cam_close(void)
{
    kt_cancel(KT_UI, KT_DRV_CAM);
    if (cam_running || cam_state() != CAM_ST_OFF) cam_close();   /* synchronous: everything off, as found */
    cam_running = 0;
}

/* -- Face Unlock (docs/FACE-UNLOCK.md): ui/face_svc.c's platform. The face pack (tools/face-pack.py, 7.3 MB) is a
 * file on the card, /starbite/face/sbface.pak (the image stays within its 32 MiB budget): loaded into the UI heap
 * when Face Unlock is set up (at the start) or its Settings page shows, 256 KiB a turn, and kept; its SHA-256 (the
 * pack's own, in its header) checked as it comes. The camera portrait 360 x 480 NOT mirrored; the face's work on the
 * band workers (kpar) from this timer, <= ~30 ms a turn; the prefs' blobs face.k / face.t. The self test (the dry
 * build) never reads the card: Face Unlock is "not there" in it; the face test image (SBOS_SELFTEST_NEXT_FACE in
 * uitest.c, the normal kern_next) runs face_selftest_step. */
#define FACE_PAK_DIR "/starbite/face"
#define FACE_PAK_NAME "sbface.pak"
#define FACE_PAK_CHUNK (256U << 10)
static struct {
    int st;                             /* -1 not there, 0 not tried, 1 loading, 2 in memory */
    uint8_t *p;
    uint32_t len, got;
    struct pass_sha sha;
    uint64_t t0, t_fail;
    uint32_t load_ms;                   /* the last full load's time */
} FP;
static void fp_fail(const char *why)
{
    if (FP.p) tk_free(FP.p);
    FP.p = 0;
    FP.st = -1;
    FP.t_fail = sb_ms();
    sb_logf("next: face: the pack: ", why);
}
/* one turn of the load: the card mounted, the file found, then a chunk read and hashed */
static void fp_step(void)
{
#ifdef SBOS_SELFTEST_NEXT
    fp_fail("not read (the self test never reads the card)");
#else
    if (!FP.p) {
        const char *why = 0;
        int cs = kfile_state(&why);
        if (cs == 0) { hw_wdt_feed(); kfile_mount(); cs = kfile_state(&why); }
        if (cs <= 0) { fp_fail(cs == -1 ? "no card" : "the card cannot be read"); return; }
        static struct kfile_ent e[16];
        int n = kfile_list_at(FACE_PAK_DIR, e, 16, 0);
        uint64_t size = 0;
        for (int i = 0; i < n; i++)
            if (!e[i].dir && !tk_strcmp(e[i].name, FACE_PAK_NAME)) size = e[i].size;
        if (size < 64 || size > (16ULL << 20)) { fp_fail("no " FACE_PAK_DIR "/" FACE_PAK_NAME " on the card"); return; }
        FP.p = tk_alloc((size_t)size);
        if (!FP.p) { fp_fail("no memory for it"); return; }
        FP.len = (uint32_t)size;
        FP.got = 0;
        FP.t0 = sb_ms();
        pass_sha_init(&FP.sha);
    }
    uint32_t want = FP.len - FP.got < FACE_PAK_CHUNK ? FP.len - FP.got : FACE_PAK_CHUNK;
    int r = kfile_read_at(FACE_PAK_DIR "/" FACE_PAK_NAME, FP.got, FP.p + FP.got, want);
    if (r != (int)want) { fp_fail("a read failed"); return; }
    /* the digest covers bytes 64.. (the header holds it at 32..63) */
    uint32_t from = FP.got < 64 ? 64 : FP.got;
    if (FP.got + want > from) pass_sha_update(&FP.sha, FP.p + from, FP.got + want - from);
    FP.got += want;
    if (FP.got < FP.len) return;
    uint8_t d[32];
    pass_sha_final(&FP.sha, d);
    int bad = 0;
    for (int i = 0; i < 32; i++) bad |= d[i] ^ FP.p[32 + i];
    if (bad || fn_pack_check(FP.p, FP.len, 0) < 0) { fp_fail("its SHA-256 or header is wrong (a damaged copy)"); return; }
    FP.st = 2;
    FP.load_ms = (uint32_t)(sb_ms() - FP.t0);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: face: the pack in memory: ");
    ub_dec(&u, FP.len >> 10);
    ub_s(&u, " KiB from the card in ");
    ub_dec(&u, sb_ms() - FP.t0);
    ub_s(&u, " ms, SHA-256 good");
    sb_log(u.b);
#endif
}
static int fs_pack_state(void) { return FP.st; }
/* the unlock (Track A: the pack must not stay while home and apps run; a1btn peaks at 134 MiB of the 135 allowed) */
static void fs_pack_release(void)
{
    uint32_t kib = FP.st == 2 ? FP.len >> 10 : FP.got >> 10;
    int was = FP.st;
    if (FP.p) tk_free(FP.p);
    FP.p = 0;
    FP.len = FP.got = 0;
    FP.st = 0;
    if (was == 2) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "next: face: pack freed (unlock): ");
        ub_dec(&u, kib);
        ub_s(&u, " KiB back");
        sb_log(u.b);
    }
    else if (was == 1) sb_log("next: face: the pack's load stopped (unlock)");
}
static int fs_pack_get(const uint8_t **p, size_t *len)
{
    if (FP.st == 2) { *p = FP.p; *len = FP.len; return 1; }
    if (FP.st < 0 && sb_ms() - FP.t_fail > 60000) FP.st = 0;   /* a card put in later: try again after a minute */
    if (FP.st < 0) return -1;
    if (FP.st == 0) { FP.st = 1; FP.p = 0; kt_set(KT_UI, KT_DRV_FACE, sb_ms(), 1, 0); }
    return 0;
}
static void fs_arm(unsigned ms) { kt_set(KT_UI, KT_DRV_FACE, sb_ms(), ms ? ms : 1, 0); }
static int fs_cam_open(unsigned w, unsigned h)
{
    DRV_DRY("the front camera (Face Unlock)", -1);
    if (cam_running || face_cam) return CAM_ESTATE;        /* the Camera app has it */
    struct cam_req q = { CAM_FRONT, w, h, CAM_ROT270, 1, 0, 0 };   /* CAM_PORTRAIT_SELFIE without the mirror */
    int r = cam_open(&q);
    if (r < 0) { sb_logf("next: face: the camera did not open: ", cam_why()); return r; }
    face_cam = 1;
    return 0;
}
static const uint32_t *fs_cam_frame(unsigned *seq)
{
    struct cam_pic p;
    if (!face_cam || !cam_frame(&p) || p.stride != p.w) return 0;
    *seq = p.seq;
    return p.px;
}
static void fs_cam_close(void)
{
    if (face_cam && cam_state() != CAM_ST_OFF) cam_close();
    face_cam = 0;
}
/* random bytes: the counter's jitter over memory walks, the clocks, a running count, hashed (pass_salt's way) */
static void fs_random(uint8_t *out, unsigned n)
{
    static volatile uint8_t walk[4096];
    static uint32_t made;
    while (n) {
        uint8_t pool[256 + 24], h[32];
        for (unsigned i = 0; i < 256; i++) {
            uint64_t a = sb_ticks();
            for (unsigned k = 0; k < 24; k++) walk[(i * 131u + k * 523u + made) & 4095u] += (uint8_t)(a >> (k & 7));
            pool[i] = (uint8_t)(sb_ticks() - a) ^ (uint8_t)a ^ walk[(i * 7u) & 4095u];
        }
        uint64_t t = sb_ticks(), ms = sb_ms();
        made++;
        tk_memcpy(pool + 256, &t, 8);
        tk_memcpy(pool + 264, &ms, 8);
        tk_memcpy(pool + 272, &made, 4);
        tk_memset(pool + 276, 0, 4);
        pass_sha256(pool, sizeof pool, h);
        unsigned k = n < 32 ? n : 32;
        tk_memcpy(out, h, k);
        out += k;
        n -= k;
        tk_memset(h, 0, sizeof h);
    }
}
static void fs_log(const char *line) { sb_logf("next: ", line); }
static void fs_cam_roi(int valid, unsigned x, unsigned y, unsigned w, unsigned h)
{
    struct cam_roi r = { valid, x, y, w, h };
    cam_set_roi(&r);
}
static int fs_cam_settled(void)
{
    struct cam_ae_info i;
    cam_ae_info(&i);
    return i.settled;
}
static uint64_t fs_ms(void) { return sb_ms(); }
static void *fs_alloc(size_t n) { return tk_alloc(n); }
static void fs_par(void (*fn)(void *, int), void *arg, int n) { kpar(fn, arg, n); }
static int face_up;
static void face_init(void)
{
    struct fsvc_plat p;
    tk_memset(&p, 0, sizeof p);
    p.pack_get = fs_pack_get;
    p.pack_state = fs_pack_state;
    p.pack_release = fs_pack_release;
    p.fn.alloc = fs_alloc;
    p.fn.free = tk_free;
    p.fn.par = fs_par;
    p.fn.now_us = tk_us;
    p.now_ms = fs_ms;
    p.cam_open = fs_cam_open;
    p.cam_frame = fs_cam_frame;
    p.cam_close = fs_cam_close;
    p.cam_roi = fs_cam_roi;
    p.cam_settled = fs_cam_settled;
    p.blob_get = sh_blob_get;
    p.blob_put = sh_blob_put;
    p.pref_int = sh_pref_int;
    p.pref_set_int = sh_pref_set_int;
    p.random = fs_random;
    p.pass_on = sh_pass_on;
    p.pass_wait = sh_pass_wait;
    p.log = fs_log;
    p.arm = fs_arm;
    uint64_t t0 = sb_ms();
    fsvc_init(&p);
    face_up = 1;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: face: up in ");
    ub_dec(&u, sb_ms() - t0);
    ub_s(&u, " ms; status ");
    ub_dec(&u, (uint64_t)sh_face_status());
    ub_s(&u, FP.st == 1 ? " (the pack loading from the card)" : "");
    sb_log(u.b);
}
static void face_tick(void)
{
    if (!face_up) return;
    if (FP.st == 1) {                                /* the pack's load: a chunk a turn */
        fp_step();
        if (FP.st == 1) { kt_set(KT_UI, KT_DRV_FACE, sb_ms(), 2, 0); return; }
    }
    if (face_cam) {
        int r = cam_poll();
        if (r <= 0) {
            if (r < 0) sb_logf("next: face: the camera stopped: ", cam_why());
            face_cam = 0;
            sh_face_stop();
            return;
        }
    }
    if (fsvc_busy()) {
        workers_start();
        if (!boosted) boost(1);                     /* the A55 at its top step while the face's work runs */
    }
    fsvc_tick();
    if (!fsvc_busy()) kt_set(KT_UI, 21, sb_ms(), perf_tail_ms(), 0);   /* then down as after a gesture */
}
#ifndef SBOS_SELFTEST_NEXT
/* the face test image (uitest.c SBOS_SELFTEST_NEXT_FACE, unattended, nothing written): the pack from the card, the
 * networks on a made-up frame (the A55's times), the camera for 4 s with nobody enrolled (its first frame's time, the
 * detections); 1 passed, -1 failed, 0 running. The loop's turns of 100 ms or more are counted by uitest.c's caller. */
static int ftest_phase;
static uint64_t ftest_t;
/* round 2 (2026-11-16, attended: the user looks at the phone from the boot's ~20 s for ~10 s): after the pack and the
 * bench, the camera runs 9 s with the face test's detections; at 0.4, 1.5, 3, 5 and 7.5 s the whole raw frame
 * (1632 x 1224, 16-bit: two slots) and the preview the phone shows (v2, 360 x 480) are copied into the heap; then
 * saved into the store entry "sbos-facedbg" (photos.c's format, never the user's photos; made with store-update.sh
 * --volatile sbos-facedbg=67108864). The Mac pulls them (tools/face/facedbg-pull.py) and runs v1 / v2 / new tunings
 * on the raw. The saves block (~0.3 s each): their turns are not counted (face_selftest_excluded). */
#define FT_CAPS 5
static const uint32_t ft_at[FT_CAPS] = { 400, 1500, 3000, 5000, 7500 };
static uint16_t *ft_raw[FT_CAPS];
static uint32_t *ft_pv[FT_CAPS];
static uint32_t ft_lines[FT_CAPS], ft_gain[FT_CAPS], ft_luma[FT_CAPS];
static int ft_cap, ft_save, ft_saved, ft_save_fail;
static uint32_t ft_turns0, ft_excluded;
static uint64_t ft_cam_t0, ft_count_t, ft_face_t;   /* (ft_face_t: the first face seen; the captures count from it) */
static int ft_count;
/* the test's own screen (shell_face.inc fx_test): face=test-count:N, test-run, test-done */
static void ft_screen(const char *key, int n)
{
    sh_set("face", key, (float)n);
    need_full = 1;
    next_kdraw();
}
uint32_t face_selftest_excluded(void) { return ft_excluded; }
static void ft_free(void)
{
    for (int i = 0; i < FT_CAPS; i++) {
        if (ft_raw[i]) tk_free(ft_raw[i]);
        if (ft_pv[i]) tk_free(ft_pv[i]);
        ft_raw[i] = 0;
        ft_pv[i] = 0;
    }
}
/* the item k of the saves: per capture the raw's top half, its bottom half, the preview */
static int ft_save_one(int k)
{
    int c = k / 3, part = k % 3;
    if (!ft_raw[c]) return 0;
    if (part == 2) return ft_pv[c] ? sh_photo_save(ft_pv[c], FACE_FRAME_W, FACE_FRAME_H) : 0;
    /* 1632 x 612 samples (16-bit) as 1632 x 408 pixels of three bytes: the raw's bytes in order */
    uint32_t *px = tk_alloc((size_t)1632 * 408 * 4);
    if (!px) return -1;
    const uint8_t *b = (const uint8_t *)(ft_raw[c] + (size_t)part * 1632 * 612);
    for (size_t i = 0; i < (size_t)1632 * 408; i++)
        px[i] = 0xFF000000u | (uint32_t)b[3 * i] << 16 | (uint32_t)b[3 * i + 1] << 8 | b[3 * i + 2];
    int r = sh_photo_save(px, 1632, 408);
    tk_free(px);
    return r;
}
int face_selftest_step(void)
{
    switch (ftest_phase) {
    case 0:
        if (!face_up) return 0;
        ftest_t = sb_ms();
        ft_screen("test-count", 0);                  /* "Look at the phone": over the lock (touches do nothing) */
        sb_log("selftest: next-face: the test's screen is up (Look at the Phone); touches are ignored in this image");
        {
            const uint8_t *p;
            size_t n;
            (void)fs_pack_get(&p, &n);
        }
        ftest_phase = 1;
        return 0;
    case 1:
        if (FP.st == 1 || FP.st == 0) return sb_ms() - ftest_t < 60000 ? 0 : (sb_log("selftest: next-face: FAILED: the pack did not load within 60 s"), -1);
        if (FP.st < 0) { sb_log("selftest: next-face: FAILED: no face pack (" FACE_PAK_DIR "/" FACE_PAK_NAME " on the card?)"); return -1; }
        if (!fsvc_bench_start()) { sb_log("selftest: next-face: FAILED: the bench did not start"); return -1; }
        ftest_phase = 2;
        return 0;
    case 2:
        if (fsvc_busy()) return 0;
        ft_count = 3;                                /* 3, 2, 1 on the screen, a second each */
        ft_count_t = sb_ms();
        ft_screen("test-count", 3);
        ftest_phase = 6;
        return 0;
    case 6:
        if (sb_ms() - ft_count_t < 1000) return 0;
        ft_count_t = sb_ms();
        if (--ft_count > 0) { ft_screen("test-count", ft_count); return 0; }
        sb_log("selftest: next-face: LOOK AT THE PHONE now (about 30 cm), for 10 s: the camera runs 9 s");
        if (!fsvc_camera_test(40000)) { sb_log("selftest: next-face: FAILED: the camera test did not start (the camera?)"); return -1; }
        ft_screen("test-noface", 0);                 /* until a face is seen: "hold the phone up in front of your face" */
        ft_cam_t0 = sb_ms();
        ft_face_t = 0;
        ft_cap = 0;
        ftest_phase = 3;
        return 0;
    case 3:
        /* a face first (three frames with one), up to 30 s; then the captures, one a step, 9 s from it */
        if (!ft_face_t) {
            if (fsvc_camtest_faces() >= 3) {
                ft_face_t = sb_ms();
                fsvc_camtest_stop_after(9500);
                ft_screen("test-run", 0);
                struct ub u;
                ub_init(&u);
                ub_s(&u, "selftest: next-face: a face is seen after ");
                ub_dec(&u, sb_ms() - ft_cam_t0);
                ub_s(&u, " ms of camera: the captures start");
                sb_log(u.b);
            } else if (sb_ms() - ft_cam_t0 > 30000) {
                ft_face_t = sb_ms();                 /* no face in 30 s: capture what is there anyway */
                fsvc_camtest_stop_after(9500);
                ft_screen("test-run", 0);
                sb_log("selftest: next-face: NO FACE in 30 s; capturing whatever the camera sees");
            }
            if (!ft_face_t) return 0;
        }
        if (ft_cap < FT_CAPS && face_cam && sb_ms() - ft_face_t >= ft_at[ft_cap]) {
            struct cam_ae_info ai;
            cam_ae_info(&ai);
            ft_raw[ft_cap] = tk_alloc((size_t)1632 * 1224 * 2);
            ft_pv[ft_cap] = tk_alloc((size_t)FACE_FRAME_W * FACE_FRAME_H * 4);
            unsigned seq = 0;
            const uint32_t *pv = fs_cam_frame(&seq);
            if (ft_raw[ft_cap] && cam_raw_crop(ft_raw[ft_cap], 0, 0, 1632, 1224) < 0) { tk_free(ft_raw[ft_cap]); ft_raw[ft_cap] = 0; }
            if (ft_pv[ft_cap] && pv) tk_memcpy(ft_pv[ft_cap], pv, (size_t)FACE_FRAME_W * FACE_FRAME_H * 4);
            ft_lines[ft_cap] = ai.lines, ft_gain[ft_cap] = ai.again * ai.dgain / 256u, ft_luma[ft_cap] = ai.luma;
            struct ub u;
            ub_init(&u);
            ub_s(&u, "next: face: capture ");
            ub_dec(&u, (uint64_t)ft_cap);
            ub_s(&u, " at ");
            ub_dec(&u, sb_ms() - ft_face_t);
            ub_s(&u, " ms after the first face: raw ");
            ub_s(&u, ft_raw[ft_cap] ? "ok" : "MISSING");
            ub_s(&u, ", exposure ");
            ub_dec(&u, ai.lines);
            ub_s(&u, " lines, gain x");
            ub_dec(&u, ai.again * ai.dgain / 8192u);
            ub_s(&u, ", luma ");
            ub_dec(&u, ai.luma);
            ub_s(&u, ai.settled ? " (settled)" : " (moving)");
            ub_s(&u, ", wb r ");
            ub_dec(&u, ai.wb_r);
            ub_s(&u, " b ");
            ub_dec(&u, ai.wb_b);
            sb_log(u.b);
            ft_cap++;
        }
        if (fsvc_busy()) return 0;
        if (!fsvc_result_first_frame_ms()) { sb_log("selftest: next-face: FAILED: no camera frame"); ft_free(); return -1; }
        /* the saves: their turns are excluded from the test's count */
        ft_screen("test-done", 0);
        kphotos_entry("sbos-facedbg");
        ft_turns0 = K.turns_100;
        ft_save = ft_saved = ft_save_fail = 0;
        ftest_phase = 4;
        return 0;
    case 4:
        if (ft_save < FT_CAPS * 3) {
            int r = ft_save_one(ft_save);
            if (r == 0 && ft_raw[ft_save / 3]) ft_saved++;
            else if (r < 0) {
                ft_save_fail++;
                if (ft_save_fail == 1) sb_logf("next: face: a frame was not saved: ", sh_photo_why());
            }
            ft_save++;
            return 0;
        }
        ft_excluded = K.turns_100 - ft_turns0;
        kphotos_entry(0);
        ft_free();
        ftest_phase = 5;
        {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "selftest: next-face: the pack ");
            ub_dec(&u, FP.len >> 10);
            ub_s(&u, " KiB from the card in ");
            ub_dec(&u, FP.load_ms);
            ub_s(&u, " ms, SHA-256 good; the camera's first frame after ");
            ub_dec(&u, (uint64_t)fsvc_result_first_frame_ms());
            ub_s(&u, " ms; frames saved into sbos-facedbg: ");
            ub_dec(&u, (uint64_t)ft_saved);
            ub_s(&u, " of ");
            ub_dec(&u, FT_CAPS * 3);
            ub_s(&u, " (their ");
            ub_dec(&u, ft_excluded);
            ub_s(&u, " slow turns not counted)");
            sb_log(u.b);
        }
        if (!ft_saved) sb_log("selftest: next-face: NOTE: no frame saved (no sbos-facedbg entry? store-update.sh --volatile sbos-facedbg=67108864)");
        return 1;
    }
    return -1;
}
#endif

/* -- the hub's sensors (each getter keeps its sensor on for 10 s; the hub starts through survival.c) */
int sh_light_lux(void)
{
    DRV_DRY("the light sensor", -1);
    uint32_t lux;
    return sens_lux(&lux) ? (int)(lux > 1000000U ? 1000000U : lux) : -1;
}
int sh_pressure_hpa10(void)
{
    DRV_DRY("the barometer", -1);
    int32_t p, t;
    return sens_pressure_hpa(&p, &t) ? (int)((p + 5) / 10) : -1;
}
int sh_proximity_near(void)
{
    DRV_DRY("the proximity sensor", -1);
    return sens_prox_near();
}
int sh_gyro_mdps(int *x, int *y, int *z)
{
    DRV_DRY("the gyroscope", 0);
    int32_t g[3];
    if (!sens_gyro_mdps(g)) return 0;
    if (x) *x = g[0];
    if (y) *y = g[1];
    if (z) *z = g[2];
    return 1;
}

/* -- Auto-Brightness: sens_auto_brightness (0..255, a log curve of the lux) to the panel's four levels; a new level
 * only after it was the same two looks in a row, and only when the light moved to another level (the user's own pick
 * stays until then) */
static int ab_on, ab_last = -1, ab_cand = -1;
static int ab_level(int v) { return v < 28 ? 1 : v < 75 ? 2 : v < 150 ? 3 : 4; }   /* ~4, ~50, ~500 lux */
static void ab_tick(void)
{
    if (!ab_on || !K.screen_on) return;              /* the screen's on arms it again */
    kt_set(KT_UI, KT_DRV_AUTO, sb_ms(), 2000, 0);
    int v = sens_auto_brightness();
    if (v < 0) return;
    int l = ab_level(v);
    if (l != ab_cand) { ab_cand = l; return; }
    if (l == ab_last) return;
    ab_last = l;
    if (l == panel_bright_level) return;
    if (panel_brightness(l)) {
        sh_bright_now(l);
        sb_logx("next: Auto-Brightness: the panel's level ", (uint64_t)l);
    }
}
static void ab_set(int on)
{
    ab_on = !!on;
    ab_last = ab_cand = -1;
    if (ab_on && K.screen_on) kt_set(KT_UI, KT_DRV_AUTO, sb_ms(), 1, 0);
    if (!ab_on) {
        kt_cancel(KT_UI, KT_DRV_AUTO);
        sens_release(SENS_LIGHT);
    }
}
void sh_auto_brightness(int on)
{
    DRV_DRYV("Auto-Brightness");
    sh_pref_set_int("display.auto", !!on);
    ab_set(on);
}
int sh_auto_brightness_on(void) { return ab_on; }

/* -- a call at the ear: sens_incall + the panel dark while near (panel_blank: the display off, the screen on); the
 * touches of that time dropped (next_kevent). Ends by itself with the call (kmodem_call, every 200 ms), or after 10 s
 * when no call came. The Power key works as always: its screen on lights the panel again. */
static int px_on, px_dark, px_call;
static uint64_t px_t0;
static void px_dark_set(int dark)
{
    if (dark == px_dark || !K.screen_on) return;    /* screen off: the screen's on lights it (drv_screen) */
    if (panel_blank(dark)) px_dark = dark;
    else sb_log(dark ? "next: proximity: the panel did not go dark" : "next: proximity: the panel did not light");
}
static void px_set(int on)
{
    on = !!on;
    if (on == px_on) return;
    px_on = on;
    px_call = 0;
    px_t0 = sb_ms();
    sens_incall(on);
    if (on) { kt_set(KT_UI, KT_DRV_PROX, sb_ms(), 200, 0); return; }
    kt_cancel(KT_UI, KT_DRV_PROX);
    px_dark_set(0);
}
static void px_tick(void)
{
    if (!px_on) return;
    struct kcall c;
    kmodem_call(&c);
    if (c.state >= KCALL_DIALING && c.state <= KCALL_HELD) px_call = 1;
    else if (px_call) { sb_log("next: proximity: the call ended: off"); px_set(0); return; }
    else if (sb_ms() - px_t0 > 10000) { sb_log("next: proximity: no call came: off"); px_set(0); return; }
    kt_set(KT_UI, KT_DRV_PROX, sb_ms(), 200, 0);
    px_dark_set(sens_incall_screen_off());
}
void sh_call_proximity(int on)
{
    if (on) DRV_DRYV("the in-call proximity");
    px_set(on);
}
static int drv_touch_dark(void) { return px_dark && K.screen_on; }

/* -- battery health (read only: five gauge words, ~2 ms) */
int sh_battery_health(int *max_pct, int *cycles, char *words, int cap)
{
    struct bathealth b;
    int r = bathealth_read(&b) == 1;
    if (max_pct) *max_pct = r ? b.shown_pct : -1;
    if (cycles) *cycles = r ? (int)b.cycles : -1;
    if (words && cap > 0) sb_strcpy(words, (unsigned)cap, r ? (b.words ? b.words : "") : bathealth_why());
    return r;
}

/* -- haptic patterns at the kept intensity */
_Static_assert((int)SH_VIB_RING == (int)HAPTIC_RING && (int)SH_VIB_NOTIFY == (int)HAPTIC_NOTIFY &&
               (int)SH_VIB_ALARM == (int)HAPTIC_ALARM && (int)SH_VIB_TICK == (int)HAPTIC_TICK &&
               (int)SH_VIB_SUCCESS == (int)HAPTIC_SUCCESS && (int)SH_VIB_WARNING == (int)HAPTIC_WARNING &&
               (int)SH_VIB_ERROR == (int)HAPTIC_ERROR, "SH_VIB_* are HAPTIC_*");
void sh_haptic_kind(int kind)
{
    DRV_DRYV("a haptic pattern");
    int r = haptic_play(kind);
    if (r < 0 && r != SBOS_EBUSY) sb_logf("next: haptic: not played: ", haptic_why());   /* busy: a ring / alarm plays */
}
void sh_haptic_stop(void) { haptic_stop(); }
int sh_haptic_level(void) { return haptic_intensity(); }
void sh_haptic_level_set(int level)
{
    level = level < 0 ? 0 : level > 3 ? 3 : level;
    DRV_DRYV("the haptic level");
    haptic_set_intensity(level);
    sh_pref_set_int("haptic.level", level);
}

/* -- the kernel's side */
static void drv_timer(unsigned id)
{
    if (id == KT_DRV_BT) bt_tick();
    else if (id == KT_DRV_CAM) cam_tick();
    else if (id == KT_DRV_AUTO) ab_tick();
    else if (id == KT_DRV_PROX) px_tick();
    else if (id == KT_DRV_SND || id == KT_DRV_JACK) snd_tick(id);
    else if (id == KT_DRV_PREWARM) prewarm_arm(-1);
    else if (id == KT_DRV_FACE) face_tick();
}
static void drv_screen(int on)
{
    if (!on) { kt_cancel(KT_UI, KT_DRV_AUTO); return; }
    if (px_dark) {                                   /* the Power key during a call: lit, the proximity decides again */
        if (!panel_blank(0)) sb_log("next: proximity: the panel did not light");
        px_dark = 0;
    }
    nledpol_screen(1);                               /* the missed call / message seen on the lock screen */
    if (ab_on) kt_set(KT_UI, KT_DRV_AUTO, sb_ms(), 300, 0);
}
static void bt_kbd_init(void);                       /* Track A, cp7: the keyboard's key store (below) */
/* 8b (Track A; the drivers' usb.c): the read-only USB console behind Settings > Developer > USB Console, off by
 * default (pref dev.usb, restored at the bake's end); never in a self test (the probe drives USB there) */
void sh_usb_console(int on)
{
    on = !!on;
    sh_pref_set_int("dev.usb", on);
#ifndef SBOS_SELFTEST_NEXT
    if (!sbos_selftest_enabled) usb_enable(on);
#endif
}
int sh_usb_console_on(void) { return sh_pref_int("dev.usb", 0) == 1; }
/* 9j: Survival Heat (kern/heater.c): its limits, refusals and confirmations are the kernel's; a dry run in the self test */
int sh_heat_start(int level, int minutes)
{
    if (level != SH_HEAT_LOW && level != SH_HEAT_MEDIUM && level != SH_HEAT_HIGH && level != SH_HEAT_EXTREME &&
        level != SH_HEAT_KEEPWARM && level != SH_HEAT_SURVIVAL && level != SH_HEAT_SURVIVAL_X)
        return -1;                                   /* (9k: Manual, its clocks picked per cluster, stays classic-only) */
#ifdef SBOS_SELFTEST_NEXT
    sb_log("next: heat: would start (a dry run: the self test)");
    (void)minutes;
    return -1;
#else
    if (sbos_selftest_enabled) return -1;
    return heater_request(level, minutes) < 0 ? -1 : 0;
#endif
}
void sh_heat_stop(void) { if (heater_running_ui()) (void)heater_request(0, 0); }
int sh_heat_opt(int which)
{
    return which == SH_HO_ALL_OUT ? heat_all_out : which == SH_HO_UNLIMITED ? heat_unlimited : heat_surv_bat_c10;
}
int sh_heat_opt_set(int which, int v)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)which; (void)v;
    sb_log("next: heat: an option would change (a dry run: the self test)");
    return -1;
#else
    if (sbos_selftest_enabled) return -1;
    uint64_t id = which == SH_HO_ALL_OUT ? SET_HEAT_ALL_OUT : which == SH_HO_UNLIMITED ? SET_HEAT_UNLIMITED : SET_HEAT_SURV_BAT_C10;
    return ksetting_set(id, (uint64_t)(int64_t)v) < 0 ? -1 : 0;
#endif
}
/* 9k: the fire starter (kern/survival.c's surv_fs_*): the kernel's gate, limits and finger cut; a dry run in the self test */
int sh_fire_start(int pulse_ms, int repeat, int ilim_ma, int ov, int wool)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)pulse_ms; (void)repeat; (void)ilim_ma; (void)ov; (void)wool;
    sb_log("next: fire starter: would start (a dry run: the self test)");
    return -1;
#else
    if (sbos_selftest_enabled) return -1;
    return surv_fs_start_x((uint32_t)pulse_ms, repeat, ilim_ma, ov, 0, wool) < 0 ? -1 : 0;
#endif
}
void sh_fire_finger(int down) { surv_fs_finger(down); }
void sh_fire_stop(void) { if (surv_usb_active()) surv_usb_stop("Stop on the screen"); }
void sh_fire(sh_fire_t *f)
{
    struct surv_usb u;
    surv_usb_status(&u);
    f->state = u.state == SURV_USB_ON ? SH_FIRE_ON : u.state == SURV_USB_COOL ? SH_FIRE_COOL : SH_FIRE_OFF;
    f->fire = u.fire, f->ms_left = u.ms_left, f->est_ma = u.est_ma;
    f->n = u.fs_n, f->max = u.fs_max, f->out = u.fs_out, f->rearms = u.fs_rearms, f->elapsed_ms = u.fs_elapsed_ms;
    f->avg_ma = u.fs_avg_ma, f->peak_ma = u.fs_peak_ma, f->w10 = u.fs_w10, f->load_w10 = u.fs_load_w10;
    f->ohm10 = u.fs_ohm10, f->open = u.fs_open, f->why_code = u.fs_why, f->bat_c10 = u.bat_c10, f->why = u.why;
    f->bank_floor = u.bank_floor, f->bank_mah = u.bank_mah;
}
/* 9l: the Survival Kit. The barometer and heart rate drive hardware: dry in the self test (its films and gates) */
#ifdef SBOS_SELFTEST_NEXT
#define KIT_DRY 1
#else
#define KIT_DRY sbos_selftest_enabled
#endif
int sh_baro(sh_baro_t *b)
{
    int32_t p = 0, d3 = 0, d1 = 0;
    int alt = 0, known = 0, lvl = 0, h3 = 0, h1 = 0;
    b->code = KIT_DRY ? 0 : surv_baro(&p);
    b->hpa_x10 = (int)p;
    b->alt_known = surv_baro_alt(&alt, &known) == 1 && known;
    b->alt_m = alt;
    (void)surv_baro_trend(&lvl, &d3, &d1, &h3, &h1);
    b->level = lvl, b->d3_x10 = (int)d3, b->d1_x10 = (int)d1, b->have3 = h3, b->have1 = h1;
    b->watch = surv_storm_watch(-1);
    b->n = surv_baro_hist_n();
    return b->code;
}
void sh_baro_close(void) { if (!KIT_DRY) surv_baro_close(); }
int sh_baro_hist(int i, int *hpa_x10, int *age_min) { int32_t p = 0; int r = surv_baro_hist(i, &p, age_min); *hpa_x10 = (int)p; return r; }
int sh_baro_watch(int on) { return on < 0 || !KIT_DRY ? surv_storm_watch(on) : -1; }
int sh_baro_alt_set(int alt_m) { return KIT_DRY ? -1 : surv_baro_alt_set(alt_m); }
int sh_sun_pos(int *az_x10, int *el_x10) { return surv_sun_pos_now(az_x10, el_x10); }
int sh_water(int kind) { return surv_water_timer(kind); }
int sh_water_left(int *kind) { return surv_water_left(kind); }
int sh_water_secs(int kind)
{
    int alt = -100000, known = 0;
    if (surv_baro_alt(&alt, &known) != 1) alt = -100000;
    return (int)kit_timer_seconds(kind, alt);
}
int sh_hrm(sh_hrm_t *h)
{
    int r = KIT_DRY ? -1 : hrm_want();
    struct hrm_status s;
    hrm_status(&s);
    h->state = s.state, h->finger = s.finger, h->bpm = s.bpm, h->quality = s.quality, h->seconds = s.seconds;
    return r;
}
void sh_hrm_stop(void) { if (!KIT_DRY) hrm_stop("the page closed"); }
const char *sh_hrm_why(void) { return KIT_DRY ? "A dry run: the self test" : hrm_why(); }
int sh_tone(int hz, int on) { return survkit_tone(hz, on); }   /* (the kernel refuses in the test images and without a finger) */
void sh_finger(int down) { surv_fs_finger(down); }
int sh_bank_start(int floor_pct, int minutes) { return KIT_DRY ? -1 : surv_usb_bank_start(floor_pct, minutes) < 0 ? -1 : 0; }
int sh_fire_override_ok(void)
{
#ifdef SBOS_SELFTEST_NEXT
    return 0;                                        /* (the self test never reads the card) */
#else
    return !sbos_selftest_enabled && surv_fs_override_allowed();
#endif
}
void sh_heat(sh_heat_t *o)
{
    struct sbos_heat h;
    heater_status(&h);
    o->state = h.state;
    o->level = h.level;
    o->secs_left = h.secs_left;
    o->secs_run = h.secs_run;
    o->bat_c10 = h.bat_c10;
    o->soc_c10 = h.soc_c10;
    o->skin_c10 = h.skin_c10;
    o->pct = h.pct;
    o->ma = h.ma;
    o->target_c10 = h.target_c10;
    o->surv_bat_c10 = h.surv_bat_c10;
    sb_strcpy(o->why, sizeof(o->why), h.why);
}
static void drv_after_bake(void)
{
#ifndef SBOS_SELFTEST_NEXT
    if (!sbos_selftest_enabled && sh_pref_int("dev.usb", 0) == 1) usb_enable(1);   /* 8b: the Developer switch kept */
#endif
    bt_kbd_init();
#ifdef SBOS_SELFTEST_NEXT
    sb_log("next: drivers: the haptic level, Auto-Brightness and the LED policy would start (a dry run: the self test)");
#else
    if (sbos_selftest_enabled) {
        sb_log("next: drivers: a self-test image: no haptic level, Auto-Brightness or LED policy");
        return;
    }
    haptic_set_intensity(sh_pref_int("haptic.level", 3));
    if (sh_pref_int("display.auto", 0) == 1) ab_set(1);
    struct sbos_battery b;
    battery_get(&b);
    nledpol_battery(b.ok, b.pct, b.charger);
    /* the notification LED's driver (nled.c) has not passed its phone probe yet: the policy runs only when the pref
     * led.notify is 1 (Settings or the probe's request turns it on); the default leaves the LED as today */
    if (sh_pref_int("led.notify", 0) == 1)
        nledpol_enable(1);
#endif
}
/* ---- end of Track A: drivers for the shell ---- */

/* ---- Track A: photos ---- shell.h's sh_photo_* on kern/photos.c: the home store's volatile entry "sbos-photos" (64
 * MiB, a ring of 32 slots; the fence's one grant lent from the state slot for each write). A save or a get takes a
 * 2 MiB work buffer from the UI heap for that call only (the pixels as stored) and gives it back. A save blocks the UI
 * for the write (~0.2..0.4 s for 612 x 816 on the card): it is the shutter's tap, once. Nothing waits: while storage
 * is held (the modem runs) or in a safe start, saves and deletes are refused with a why. In the self-test build (its
 * films start from nothing, whatever the phone keeps) nothing is read and saves / deletes are logged dry runs. */
static const char *ph_why;
#ifdef SBOS_SELFTEST_NEXT
static void ph_dry(const char *what)
{
    sb_logf("next: photos: a dry run (the self test writes nothing): ", what);
    ph_why = "A dry run: the self test.";
}
#endif
int sh_photo_save(const uint32_t *argb, int w, int h)
{
    ph_why = 0;
#ifdef SBOS_SELFTEST_NEXT
    (void)argb; (void)w; (void)h;
    ph_dry("save");
    return 0;
#else
    uint8_t *b = tk_alloc(KPHOTO_WORK);
    if (!b) {
        ph_why = "No memory to save the photo.";
        sb_log("next: photos: no 2 MiB in the UI heap for the save");
        return -1;
    }
    hw_wdt_feed();
    int r = kphotos_save(argb, w, h, (int64_t)sh_utc(), b, KPHOTO_WORK);
    tk_free(b);
    return r;
#endif
}
int sh_photo_count(void)
{
#ifdef SBOS_SELFTEST_NEXT
    return 0;
#else
    return kphotos_count();
#endif
}
int sh_photo_get(int i, uint32_t *argb, int cap_px, int *w, int *h, long long *utc)
{
    ph_why = 0;
#ifdef SBOS_SELFTEST_NEXT
    (void)i; (void)argb; (void)cap_px; (void)w; (void)h; (void)utc;
    ph_why = "No such photo.";
    return -1;
#else
    int64_t u = 0;
    uint8_t *b = 0;
    if (argb) {
        b = tk_alloc(KPHOTO_WORK);
        if (!b) {
            ph_why = "No memory to show the photo.";
            sb_log("next: photos: no 2 MiB in the UI heap for the read");
            return -1;
        }
        hw_wdt_feed();
    }
    int r = kphotos_get(i, argb, cap_px, w, h, &u, b, b ? KPHOTO_WORK : 0);
    if (b) tk_free(b);
    if (utc) *utc = (long long)u;
    return r;
#endif
}
int sh_photo_delete(int i)
{
    ph_why = 0;
#ifdef SBOS_SELFTEST_NEXT
    (void)i;
    ph_dry("delete");
    return 0;
#else
    return kphotos_delete(i);
#endif
}
const char *sh_photo_why(void) { return ph_why ? ph_why : kphotos_why(); }
/* ---- end of Track A: photos ---- */

/* ---- Track A, 1.0: performance mode and keep-awake holders (Track B's ask, cp6) ---- */
void sh_perf_mode(int conservative)
{
    if (perf_conservative == !!conservative) return;
    perf_conservative = !!conservative;
    sb_log(conservative ? "next: perf: conservative (Emergency / Survival / saving)" : "next: perf: normal (fast, animated)");
}
void sh_perf_hold(int ms)
{
    if (!ready || ms <= 0 || !K.screen_on) return;
    boost(1);
    kt_set(KT_UI, 21, sb_ms(), (uint32_t)(ms > 10000 ? 10000 : ms), 0);   /* the same clock-down check as a gesture's end */
}
#define HOLD_MAX 8
static char hold_who[HOLD_MAX][24];
int sh_hold_awake(const char *who, int on)
{
    if (!who || !*who) return -1;
    int i, free_i = -1;
    for (i = 0; i < HOLD_MAX; i++) {
        if (hold_who[i][0] && !tk_strcmp(hold_who[i], who)) break;
        if (!hold_who[i][0] && free_i < 0) free_i = i;
    }
    if (on) {
        if (i == HOLD_MAX) {
            if (free_i < 0) return -1;
            sb_strcpy(hold_who[free_i], sizeof hold_who[free_i], who);
            sb_logf("next: keep awake: on for ", who);
        }
    } else if (i < HOLD_MAX) {
        hold_who[i][0] = 0;
        sb_logf("next: keep awake: off for ", who);
    }
    int n = 0;
    for (i = 0; i < HOLD_MAX; i++) n += hold_who[i][0] != 0;
    kui_hold_awake = n;                     /* the kernel's screen-off timer waits while any holder is on */
    return n;
}


/* ---- Track A, 1.0: Low Power Mode (the Control Centre's tile; pref power.low): the conservative clock tail, kept
 * across a restart. Emergency / Survival set their own (sh_perf_mode); either one makes it conservative ---- */
void sh_low_power(int on)
{
    perf_low = !!on;
    sh_pref_set_int("power.low", perf_low);
    sb_log(on ? "next: Low Power Mode on" : "next: Low Power Mode off");
}
int sh_low_power_on(void) { return perf_low; }

/* ---- Track A, cp7: the screen's orientation for apps that rotate (the user: "landscape where it makes sense") ----
 * From the sensor hub's accelerometer (surv_tilt: pitch = the long axis against level, roll = the short one). Each call
 * keeps the accelerometer alive (surv_sensors_on_mask: a keep-alive; the hub switches itself off 60 s after the last
 * ask, and this never switches it off, so the Compass page is never cut). 10 degrees of hysteresis; flat on a table
 * (both under 25 degrees) keeps the last answer. The signs are the hub's (Android) convention: to be confirmed on the
 * phone (a still in each orientation). Orientation Lock (pref display.orientlock, off by default): always portrait. */
static int ori_last;
int sh_orientation(void)
{
#ifdef SBOS_SELFTEST_NEXT
    return SH_ORI_PORTRAIT;
#else
    if (sh_pref_int("display.orientlock", 0) == 1) return ori_last = SH_ORI_PORTRAIT;
    if (surv_sensors_on_mask(SHUB_SENS_ACCEL) < 0) return ori_last;   /* the saver: no hub, as it was */
    int p = 0, r = 0;
    if (!surv_tilt(&p, &r)) return ori_last;                          /* starting (about 4 s) or no data yet */
    int ap = p < 0 ? -p : p, ar = r < 0 ? -r : r;
    if (ap < 250 && ar < 250) return ori_last;                        /* flat */
    if (ap > ar + 100) ori_last = p > 0 ? SH_ORI_PORTRAIT : SH_ORI_UPSIDE_DOWN;
    else if (ar > ap + 100) ori_last = r > 0 ? SH_ORI_TURNED_LEFT : SH_ORI_TURNED_RIGHT;
    return ori_last;
#endif
}
int sh_orientation_lock(void) { return sh_pref_int("display.orientlock", 0) == 1; }
void sh_orientation_lock_set(int on) { sh_pref_set_int("display.orientlock", on ? 1 : 0); if (on) ori_last = SH_ORI_PORTRAIT; }

/* ---- Track A, cp7: the Bluetooth keyboard (the drivers track's bt_pair.c / bt_hid.c) for the shell ----
 * One paired keyboard. Its link key lives in the blob "bt.key" (6 bytes address + 16 bytes key; never logged): the
 * chip asks for it through bt_set_link_key_store. Keys typed on it go to the shell's sh_hw_key (a weak default drops
 * them until the UI handles them). */
static int bt_key_get(const uint8_t addr[6], uint8_t key[16])
{
    uint8_t b[22];
    if (sh_blob_get("bt.key", b, sizeof b) != 22) return 0;
    for (int i = 0; i < 6; i++) if (b[i] != addr[i]) return 0;
    tk_memcpy(key, b + 6, 16);
    tk_memset(b, 0, sizeof b);
    return 1;
}
static void bt_key_put(const uint8_t addr[6], const uint8_t *key16)
{
    if (!key16) { (void)sh_blob_put("bt.key", 0, 0); return; }
    uint8_t b[22];
    tk_memcpy(b, addr, 6);
    tk_memcpy(b + 6, key16, 16);
    (void)sh_blob_put("bt.key", b, sizeof b);
    tk_memset(b, 0, sizeof b);
}
__attribute__((weak)) void sh_hw_key(int usage, int down, uint32_t unicode) { (void)usage; (void)down; (void)unicode; }
/* cp7 (Track B): a key changes the page outside a frame: the frame that shows it now (the screen on, the shell ready) */
static void bt_key_cb(int usage, int down, uint32_t unicode)
{
    sh_hw_key(usage, down, unicode);
    if (down && ready && K.screen_on) start_ticking();
}
static void bt_kbd_init(void)
{
    bt_set_link_key_store(bt_key_get, bt_key_put);
    bt_hid_set_key_cb(bt_key_cb);
    uint8_t b[22];
    if (sh_blob_get("bt.key", b, sizeof b) == 22) bt_set_paired(b);   /* only that keyboard may come back */
    tk_memset(b, 0, sizeof b);
}
static char bt_pair_name[32];                        /* the row being paired: its name, kept once it connects */
int sh_bt_pair(int i)
{
    DRV_DRY("Bluetooth pairing", -1);
    const struct bt_dev *d = bt_dev_at(i);
    sb_strcpy(bt_pair_name, sizeof bt_pair_name, d && d->name[0] ? d->name : "Keyboard");
    int r = bt_pair(i);
    if (r >= 0) bt_arm(1);
    return r;
}
_Static_assert((int)SH_BTP_CODE == (int)BT_PAIR_CODE && (int)SH_BTP_CONNECTED == (int)BT_PAIR_CONNECTED &&
               (int)SH_BTP_FAILED == (int)BT_PAIR_FAILED,
               "shell.h's SH_BTP_* are bt.h's BT_PAIR_*");
int sh_bt_pair_state(void)
{
    int s = bt_pair_state();
    if (s == BT_PAIR_CONNECTED && bt_pair_name[0]) {   /* paired: its name kept with the key (pref bt.kbd.name) */
        (void)sh_pref_set("bt.kbd.name", bt_pair_name);
        bt_pair_name[0] = 0;
    }
    return s;
}
const char *sh_bt_kbd_name(void)
{
    static char n[32];
    if (sh_pref_get("bt.kbd.name", n, sizeof n) <= 0) sb_strcpy(n, sizeof n, "Keyboard");
    return n;
}
const char *sh_bt_pair_code(void) { return bt_pair_code(); }
const char *sh_bt_pair_why(void) { return bt_pair_why(); }
int sh_bt_kbd_paired(void) { uint8_t a[6]; return bt_paired(a); }
int sh_bt_kbd_connected(void) { return bt_hid_connected(); }
void sh_bt_unpair(void) { bt_unpair(); (void)sh_pref_set("bt.kbd.name", 0); bt_arm(1); }
int sh_bt_reconnect(void) { int r = bt_hid_reconnect(); if (r >= 0) bt_arm(1); return r; }

/* ---- Track A, cp7: sound for the shell (the drivers track's snd.c on Calliope's PCM path; drivers-integration.md) ----
 * Nothing runs until the shell first asks (snd_init_libsb then): the poll every 5 ms while snd_busy(), the jack every
 * 50 ms while sound plays or the screen is on. The volume is the output's, kept as pref snd.vol (0..15, default 8). In
 * the self test every call is a logged dry run. Calls and the modem keep ABOX through snd_suspend_for_call (modem-core). */
static int snd_up;
static void snd_arm(void)
{
    if (snd_busy()) kt_set(KT_UI, KT_DRV_SND, sb_ms(), 5, 0);
    if (snd_busy() || K.screen_on) kt_set(KT_UI, KT_DRV_JACK, sb_ms(), 50, 0);
}
static void snd_tick(unsigned id)
{
    if (!snd_up) return;
    if (id == KT_DRV_SND) snd_poll(sb_us());
    else (void)snd_jack_tick();
    snd_arm();
}
/* cp7y (the drivers' 4f735a44): snd_init_libsb prefetches Calliope's ~2.7 MB from VENDOR into RAM, so no Calliope
 * start reads VENDOR on the main loop. Called once at the bake's end, before "ready"; the self test refuses sound. */
static void snd_boot(void)
{
#ifndef SBOS_SELFTEST_NEXT
    if (sbos_selftest_enabled || snd_up) return;
    uint64_t t0 = sb_us();
    snd_init_libsb();
    snd_up = 1;
    int v = sh_pref_int("snd.vol", 8);
    snd_volume(SND_OUT_SPEAKER, (unsigned)(v < 0 ? 0 : v > 15 ? 15 : v));
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: sound: up at boot (Calliope's files in RAM) in ");
    ub_dec(&u, (sb_us() - t0) / 1000U);
    ub_s(&u, " ms");
    sb_log(u.b);
#endif
}
static int snd_ready(void)
{
#ifdef SBOS_SELFTEST_NEXT
    static int said;
    if (!said) { said = 1; sb_log("next: sound: refused (a dry run: the self test)"); }
    return 0;
#else
    if (sbos_selftest_enabled && !sbos_selftest_sound) return 0;   /* 9d: the cellsnd test's sounds are real */
    if (!snd_up) {
        snd_init_libsb();
        snd_up = 1;
        int v = sh_pref_int("snd.vol", 8);
        snd_volume(SND_OUT_SPEAKER, (unsigned)(v < 0 ? 0 : v > 15 ? 15 : v));
        sb_log("next: sound: ready (the shell's first ask)");
    }
    return 1;
#endif
}
/* 9g: modem.c's audio_prepare, before it boots Calliope: snd up first (its ring windows into the boot's tables);
 * 1 up, 0 refused (a self test without sound) */
int snd_media_early(void) { return snd_ready(); }
int sh_snd_tone(int kind)
{
    if (!snd_ready()) return -1;
    int r = snd_tone(kind);
    snd_arm();
    return r;
}
void sh_snd_tone_stop(void) { if (snd_up) snd_tone_stop(); }
int sh_snd_volume(void) { return sh_pref_int("snd.vol", 8); }
void sh_snd_volume_set(int v)
{
    v = v < 0 ? 0 : v > 15 ? 15 : v;
    sh_pref_set_int("snd.vol", v);
    if (snd_ready()) snd_volume(snd_output(), (unsigned)v);
}
int sh_snd_open(void) { if (!snd_ready()) return -1; return snd_open(); }
unsigned sh_snd_write(int id, const int16_t *frames, unsigned n) { if (!snd_up) return 0; unsigned k = snd_write(id, frames, n); snd_arm(); return k; }
unsigned sh_snd_space(int id) { return snd_up ? snd_space(id) : 0; }
void sh_snd_close(int id)
{
    if (!snd_up) return;
    snd_close(id);
    struct snd_stats t;                     /* cp7y: did the music drop out? (the mix runs ~21-32 ms ahead of the DMA) */
    snd_stats(&t);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "next: sound: a stream closed; since the start: periods ");
    ub_dec(&u, t.periods);
    ub_s(&u, ", ran dry ");
    ub_dec(&u, t.xruns);
    ub_s(&u, ", late polls ");
    ub_dec(&u, t.late_polls);
    sb_log(u.b);
}
const char *sh_snd_why(void) { return snd_up ? snd_why() : "Sound starts when something plays"; }
void alarm_sound(int on) { sh_alarm_sound(on); }   /* cp7x: alarm.c's ring -> the shell's ring owner 4 */

/* ---- 8a (Track E's instant start, 4cb49c26): Linux prewarmed, so even the first open after a restart only thaws
 * it. Once a boot, after 15 s with the screen off, only for someone who opened a Linux app before (pref linux.used,
 * gwin.c), never in Low Power Mode or a self test. The guest boots unseen, then its idle freeze takes it.
 * prewarm_arm: 0 the screen went off (armed), 1 it came on (cancelled), -1 the timer */
static void prewarm_arm(int screen_on)
{
#ifdef SBOS_SELFTEST_NEXT
    (void)screen_on;
#else
    static int pw_done;
    if (screen_on == 1) { kt_cancel(KT_UI, KT_DRV_PREWARM); return; }
    if (pw_done || sbos_selftest_enabled || perf_low || sh_pref_int("linux.used", 0) != 1) return;
    if (guest_state() != GUEST_OFF) { pw_done = 1; return; }
    if (screen_on == 0) { kt_set(KT_UI, KT_DRV_PREWARM, sb_ms(), 15000, 0); return; }
    if (K.screen_on) return;
    pw_done = 1;
    int r = guest_prewarm();
    sb_log(r > 0 ? "next: Linux prewarmed in the dark (it freezes once up; the first open thaws it)"
                 : r == 0 ? "next: Linux prewarm: nothing to do" : "next: Linux prewarm: refused");
#endif
}
