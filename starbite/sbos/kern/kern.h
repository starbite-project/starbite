/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS kernel, internal interfaces (docs/STARBITE-OS.md). The pure parts (timers, the app file
 * check, the heater's controller, the battery model, the state slot, the DVFS allow-list) are plain C that
 * the host test runs; hw_*() is the thin hardware layer (hw.c on the phone, the host stubs in the preview).
 */
#pragma once
#include "guest.h"                  /* 1.2: Linux on the M3 cluster (starbite/guest/sbos) */
#include <stdint.h>
#include <stddef.h>
#include "sbapp.h"

#define SBOS_VERSION "1.2-alpha15"

/* ---- timers (timer.c, pure) ------------------------------------------------------------------ */
/* Owners: kernel tasks 0..15, apps 16 + app index. */
enum { KT_INPUT = 0, KT_UI, KT_POWER, KT_BATTERY, KT_HEATER, KT_SOS, KT_HOUSE, KT_CLOCK, KT_SELFTEST, KT_STATE, KT_WIFI, KT_GUEST, KT_APP0 = 16 };
#define KT_MAX 64
#define KT_RESERVE 24                /* entries apps cannot take: the kernel's tasks always get theirs */
struct ktimer {
    uint64_t at;                    /* ms (sb_ms) */
    uint32_t period;                /* 0 one-shot */
    uint16_t owner, id;
    uint8_t used, prio;             /* lower = first when due together */
};
void kt_reset(void);
int kt_set(unsigned owner, unsigned id, uint64_t now, uint32_t ms, int repeat);   /* 0 or -1 full */
void kt_cancel(unsigned owner, unsigned id);
void kt_cancel_owner(unsigned owner);
int kt_pending(unsigned owner, unsigned id);
uint64_t kt_next(void);             /* earliest deadline, UINT64_MAX none */
/* The due timer that comes first (deadline, then owner priority): 1 and its owner / id; a repeating
 * timer is re-armed from its deadline (no drift, no burst after a long sleep: skipped periods are
 * dropped). */
int kt_pop(uint64_t now, unsigned *owner, unsigned *id);

/* ---- app files (appfmt.c, pure) --------------------------------------------------------------- */
#define APP_SLOT_SIZE (4ULL << 20)
#define APP_MAILBOX 0x1000U         /* the top 4 KiB of the slot: events and the call buffers */
#define APP_STACK_MAX (256U << 10)
/* Check an app file of n bytes for a slot: magic, API, header, sizes inside the slot (file + bss +
 * stack + mailbox), entry inside the code, relocations inside the file and their targets inside the
 * image, the CRC. 0 or a reason. */
const char *app_check(const void *file, uint64_t n, uint64_t slot_size);
/* Copy the checked file to slot (an address), zero the bss and apply the relocations
 * (*(slot + off) = slot + addend). */
void app_place(const void *file, uint64_t n, uintptr_t slot);
uint32_t app_crc(const void *file, uint64_t n);

/* ---- DVFS allow-list (dvfs.c, pure part) ------------------------------------------------------- */
#define DVFS_A55 2U
#define DVFS_M3  3U
#define DVFS_GPU 4U
/* 1 if the kernel may ask ACPM for khz on domain (domains 2, 3 at their table frequencies up to the
 * vendor's all-core ceiling, 4 from v0.3); MIF (0), INT (1) and everything else: never. */
int dvfs_allowed(unsigned domain, uint32_t khz);
int dvfs_request(unsigned domain, uint32_t khz);   /* checked, then hw_dvfs: 1 answered */

/* ---- heater controller (heatctl.c, pure; docs/STARBITE-OS.md 8.2) ---------------------------- */
struct heat_limits {
    int start_bat_max_c10;          /* start only below (38.0 C) */
    int agree_c10;                  /* battery vs USB / charger thermistor at start (4.0 C) */
    int hard_bat_c10, hard_soc_c10, hard_skin_c10;   /* latched stops: 42, 85, 46 C */
    int resume_bat_c10;             /* after a battery stop, a new start only below (36 C) */
    int band_bat_c10;               /* regulation: a core off at or above, back below it minus 1 C (40 C) */
    int soft_skin_c10, soft_soc_c10;/* regulation: 43 C skin, 75 C SoC */
    int soft_ma, hard_ma;           /* |current|: 3200 a core off, 3500 stop */
    int soft_sag_mv, cold_sag_mv;   /* VCELL below its resting value: 300 a core off, 150 the cold limit */
    int min_mv;                     /* stop below (3450) */
    int min_pct[9];                 /* per level to start */
    /* Extreme and Manual (0.6.1): no regulation, everything flat out until the user stops it; the stops left: the
     * battery (55 C, its safety), the SoC just below its 115 C hardware trip (a clean stop instead of a reset), the
     * sensors' integrity, a core fault, the 15 % reserve, the voltage floor and the current limit */
    int ext_hard_bat_c10, ext_hard_soc_c10, ext_start_bat_max_c10;     /* 55, 110, 50 C */
    int stop_pct;                   /* stop below (15) */
    unsigned blind_ms;              /* no valid battery or SoC sample this long: stop (3000) */
    unsigned gauge_blind_ms;        /* 1.0-rc2: no fuel-gauge reading this long: stop (10000) */
    unsigned stuck_ms;              /* the same battery raw value this long while the SoC moved 5 C (60000) */
    unsigned window_ms;             /* SoC up 20 C, battery moved < 0.5 C in this window: stop (300000) */
    unsigned step_ms;               /* regulation step (1000) */
    unsigned ramp_ms;               /* M3 1469 -> 1794 MHz after this (60000) */
    unsigned max_minutes, def_minutes;
    /* Survival (1.1): no stop for the SoC's temperature but the chip's own trip; the load is shed just below it */
    int surv_target_c10;            /* hold the SoC reading below this (122.0 C: the trip acted at our 128 twice) */
    int surv_band_c10;              /* add load again only below target minus this (2.0 C) */
    int surv_floor_pct;             /* run to this charge (2 %) */
    int surv_shed_mv;               /* shed a load while VCELL is below this (3400 mV): no stop, the PMIC's own floor stays */
    int surv_stop_ma;               /* stop above this battery current even after shedding (4500 mA: a short) */
};
extern const struct heat_limits heat_default_limits;

struct heat_in {
    uint64_t now;                   /* ms */
    int bat_ok, bat_c10, bat_raw;   /* battery thermistor (ADC 4), 0.1 C, its raw value */
    int skin_ok, skin_c10;          /* AP thermistor (ADC 0) */
    int usb_ok, usb_c10, chg_ok, chg_c10;   /* ADC 1, 3: the start cross-check */
    int soc_ok, soc_c10;            /* hottest valid TMU probe */
    int tmu_on, trip_armed;
    int gauge_ok, pct, mv, ma;      /* %, VCELL, instantaneous current (- discharging) */
    int charger;                    /* 0 none, 1 present, -1 the read failed (unknown) */
    int bad_results, worker_fault;  /* from the workers since the start */
};

struct heat_ctl {
    int state, level, minutes;
    int cores, max_cores;           /* worker cores wanted now / for the level (A55 1-3, then M3 4-7) */
    uint32_t a55_khz, m3_khz;       /* clocks wanted (0: leave / restore) */
    uint32_t man_a55, man_m3;       /* Manual (0.6.1): the picked steps (0: the top normal step) */
    int reserve_pct;                /* 0.6.2: stop at this charge (a call / SOS reserve; 0 = overridden) */
    int target_c10;                 /* 0.6.2: Keep warm's battery target */
    int no_current_stop;            /* 0.6.2: Manual only: the 3.5 A battery-current stop turned off by the user */
    int soc_stop_c10;               /* 0.6.3: Manual only: the user's SoC stop (0 off) */
    int gpu_want;                   /* 1.1: the GPU's share may run (Survival sheds it last, adds it last) */
    int trip_off;                   /* 1.1: heater.c switched the chip's trip off (Survival, trip off) */
    int surv_stop_c10;              /* 1.1: Survival (trip off)'s SoC stop */
    /* 1.1-beta6: Survival measures the legal M3 combinations (Samsung's core-count table) and keeps the most watts */
    int cal;                        /* 0 waiting, 1..4 measuring candidate cal - 1, 5 done */
    uint64_t cal_t;
    int64_t cal_acc;
    int cal_n;
    int cal_mw[4];                  /* each candidate's mean battery power, mW (0 not measured) */
    int m3_n;                       /* the M3 cores Survival may run (the pick) */
    uint32_t surv_m3_khz;           /* their clock (the pick) */
    int surv_bat_c10;               /* 1.1-beta5: Survival's battery limit: 550 / 580 / 600 (shed from 3 C below, stop at it) */
    uint64_t t_shed;
    int x_held;                     /* 1.1-beta7: the trip-off hold has shed at least once (its slower add-back applies) */
    int cold;
    uint64_t t_start, t_end, t_step, t_ramp, last_bat_ok, last_soc_ok, last_gauge_ok;
    int rest_mv;
    int last_raw, raw_soc_c10;
    uint64_t raw_since, t_win;
    int win_bat_c10, win_soc_c10;
    int bat_latched;
    char why[64];
    const struct heat_limits *lim;
};
void heat_ctl_init(struct heat_ctl *c, const struct heat_limits *lim);
/* level 0 stops; else starts if every precondition holds: 0 or SBOS_EAGAIN with c->why. */
int heat_ctl_request(struct heat_ctl *c, int level, int minutes, const struct heat_in *in);
/* 4 Hz with fresh readings: state, cores, clocks, why. */
void heat_ctl_step(struct heat_ctl *c, const struct heat_in *in);
int heat_max_cores(int level);      /* Low 3, Medium 5, High 7 */
int heat_surv_target(const struct heat_ctl *c);    /* 1.1: the SoC reading Survival holds below (0.1 C) */
int heat_surv_estop(const struct heat_ctl *c);
struct heat_m3_cand { int n; uint32_t khz; };
extern const struct heat_m3_cand heat_m3_cands[4];   /* 1.1-beta6: 4@1794, 3@1794, 2@2314, 1@2704 */     /* 1.1-beta4: trip off: the stop 5 C past the limit (<= 149 C) */
/* After a stop: is worker core w's load gone? psci = AFFINITY_INFO's raw x0 (only w0 counts), slot = the
 * worker's SB_WK_* state, start_fail = CPU_ON said yes but the core never checked in. 1 off, 2 no load of ours
 * but PSCI says on / the core never checked in (logged, harmless), 0 still in the heat kernel (a restart after
 * 1.5 s). */
int heat_core_state(int64_t psci, unsigned slot, int start_fail);

struct heatk_cfg {
    volatile uint64_t heartbeat;    /* counter ticks of the controller's last valid evaluation */
    uint64_t max_age;               /* ticks: older heartbeat stops a worker (2 s) */
    uint64_t gold;                  /* the fixed reference block's result */
    volatile uint32_t run[8];       /* per worker: 1 keep going */
    volatile uint32_t ocp_read;     /* 1.2-alpha6: core 4 reads the GCU throttle registers (the OCP probe passed) */
    uint32_t pad;
    /* second line: written before any launch (the image is cleaned to DRAM at every launch) */
    uint64_t *buf[8];               /* per core (0 = cpu0): HEATK_WORDS words, write-back RAM */
};
#define HEATK_WORDS (512U * 1024U)  /* 4 MiB per core: larger than the L2s and the shared L3 */
uint64_t heatk_batch(void);         /* the fixed reference block (the boot core computes the gold with it) */
/* One streaming batch on core w's buffer: 512 KiB read, mixed into eight FMA streams and written back changed
 * (the data never repeats); every 16th call also runs the reference block: 1 if it did not match. */
unsigned heatk_mixed(struct heatk_cfg *c, unsigned w, uint64_t it);
int heater_cpu0_active(void);
/* the GPU's share on High (gpuheat.c), off until Settings allows it */
extern int gpu_heat_allowed;
extern uint32_t gpuheat_khz;        /* 0.6.1: the GPU's table step for the session */
extern uint32_t heat_man_a55, heat_man_m3, heat_man_gpu;    /* 0.6.1: Manual's picks (kHz) */
extern int heat_reserve_pct, heat_target_c10, heat_no_current_stop;   /* 0.6.2 */
extern int heat_reserve_override;    /* 1.0-rc2: the next session's reserve overridden (once) */
extern int heat_soc_stop_c10;       /* 0.6.3: Manual's optional SoC stop (0 off) */
extern int heat_surv_bat_c10;       /* 1.1-beta5: Survival's battery limit (550 default, 580, 600 with a confirmation) */
extern int heat_surv_stop_c10;      /* 1.1: Survival (trip off)'s SoC limit (1.1-beta4: held there; default 135 C) */
void heater_trip_restore_now(void); /* 1.1: before a restart or a power-off: the chip's trip back on (waits <= 3 s to cool) */
/* 0.6.3: the S2MPB02 over I2C (fled.c): the torch at 240 mA, SOS flash pulses at 1250 mA (the chip's 125 ms timer) */
extern int led_full;                /* Settings > Alarm: 0 the pins only as before (default), 1 the rated currents */
int fled_init(void);
const char *fled_why(void);
int fled_r63(void);                 /* 1.1-beta2: the last read of 0x63 (-1 none), no chip access */
int fled_flash_ok(void);            /* 1.1-beta2: the chip answers and 0x63 = 0x7F (its flash timer confirmed) */
int fled_code(void);                /* 1.0-rc3: 0 ok or untried, else the refusal (FLED_*), for Settings */
void fled_retry(void);              /* 1.0-rc3: a refused chip is tried again at the next request */
int fled_probe(void);               /* 1.0-rc3: the bring-up and the chip's reads only (no chip writes), logged */
enum { FLED_GATE = 1, FLED_NOANSWER_NOACK, FLED_NOANSWER_NOCLOCK, FLED_NOANSWER, FLED_NOTCHIP, FLED_R63, FLED_CTRL1, FLED_FLASHSET };
int fled_torch_max(void);
int fled_pulse(void);
void fled_timer(void);
void fled_off(void);
/* 0.6.3: the heating session's record in the state slot ("heat"), every 30 s and at the stop: a forced power-off
 * (the 0.6.2 tour: the SoC at 128 C) leaves its last state for the next start */
struct heat_record {
    uint32_t magic;                 /* "HRC1" */
    uint32_t running;               /* 1 while heating: still 1 at the next start = it never stopped cleanly */
    int32_t level, secs, cores;
    int32_t bat_c10, skin_c10, soc_c10, ma, mv, pct;    /* the last readings */
    int32_t peak_bat_c10, peak_soc_c10, peak_ma;
    int32_t trip_c;                 /* the hardware trip's decoded temperature */
    char why[48];
    int32_t peak_mw, mwh;           /* 1.1-beta4: the battery's peak power and the energy taken this session */
};
extern struct heat_record heat_rec;
/* 1.2-alpha6: the heat log in the state slot ("heatlog0".."heatlog3", 4 KiB each): one sample a second for the
 * first HLOG_FAST samples of a session, then one every 10 s in a ring over the rest; kept until the next session.
 * starbite/tools/heatlog.py prints it from a pulled store image. */
struct hlog_sample {
    uint16_t t;                     /* seconds since the session's start */
    int16_t soc_c10, bat_c10, ma;   /* the hottest SoC probe, the battery, its current (- discharging) */
    uint16_t mv, m3_mhz;            /* the battery's voltage; the M3 clock (PLL) */
    uint8_t cores, flags;           /* workers running (cpu0 not counted); 1 GPU on, 2 trip off, 4 white screen,
                                       8 holding, 16 paused, 32 the M3 throttle count is read */
    uint8_t ocp_d3, thr;            /* the PMIC's OCP_WARN1 (0xD3); the M3 GCU throttle counter's low byte */
};
#define HLOG_N 1024U
#define HLOG_FAST 600U
#define HLOG_MAGIC 0x31474c48U      /* "HLG1" */
struct hlog {
    uint32_t magic, n;              /* n: samples written (up to HLOG_N; past HLOG_FAST the ring wraps) */
    uint32_t level, a55_mhz;
    uint16_t m3_n, gpu_mhz;
    uint32_t pad;
    struct hlog_sample s[HLOG_N];
};
extern struct hlog heat_log;
extern int heat_last_unclean;       /* the last start found a record still "running" */
extern struct heat_record heat_last;
extern int gpu_heat_session;        /* 0.6: Extreme's own confirmation allows the GPU for that session */
int gpuheat_start(void);
void gpuheat_tick(void);
void gpuheat_stop(void);
void gpuheat_fail_stop(const char *why);
int gpuheat_on(void);
unsigned gpuheat_mhz(void);
void gpuheat_new_session(void);
void heater_cpu0_run(uint64_t until_ms);  /* cpu0's share between the kernel's events */

/* ---- battery model (battery.c, pure part) ---------------------------------------------------------- */
enum { PS_ACTIVE = 0, PS_SCREEN_OFF, PS_HEAT, PS_N };
struct bat_model {
    int32_t ma[PS_N];               /* learned mean discharge current per state (mA, positive) */
    uint32_t samples[PS_N];
    int32_t version;
};
void bat_model_init(struct bat_model *m);
/* One reading in state st (avg_ma: the gauge's average, negative = discharging), taken settle_ms after
 * the state began. Readings while charging or before 30 s in the state are ignored. */
void bat_model_feed(struct bat_model *m, int st, int avg_ma, uint64_t in_state_ms);
int32_t bat_model_ma(const struct bat_model *m, int st, int *learned);
/* Minutes the remaining capacity lasts at ma; capped at 60 days. */
int32_t bat_minutes(int cap_mah, int32_t ma);

/* ---- state slot (state.c, pure) -------------------------------------------------------------- */
#define STATE_HALF (512U << 10)
#define STATE_REC_MAX 4096U
struct state_buf {
    uint8_t *b;                     /* STATE_HALF bytes */
    uint32_t len;                   /* bytes used, header included */
    uint64_t seq;
};
void state_begin(struct state_buf *s, uint8_t *mem, uint64_t seq);
int state_add(struct state_buf *s, const char *id, const void *data, uint32_t n);
uint32_t state_finish(struct state_buf *s);          /* fills the header (length, CRC): bytes to write */
/* Validate a half: 1 valid (*seq), 0 not. */
int state_valid(const uint8_t *half, uint64_t *seq);
/* Find a record in a valid half: its length (and *data), -1 none. */
int state_find(const uint8_t *half, const char *id, const uint8_t **data);
/* Of two halves, the valid one with the higher sequence: 0, 1, or -1 neither. */
int state_pick(const uint8_t *a, const uint8_t *b);

/* ---- the running kernel ------------------------------------------------------------------------ */
struct kstate {
    int irq_mode;                   /* 1 interrupts work, 0 polling */
    uint64_t irqs, timer_irqs;
    uint64_t first_frame_ms;
    int screen_on;
    uint32_t screen_off_s, sleep_off_s;
    uint64_t state_since;
    int power_state;                /* PS_* for the battery model */
    int heat_confirmed;             /* the first-run heater confirmation this boot */
    int any_input;                  /* a touch or key since the start (the 0.1 dead-man return) */
    uint32_t slow_turn_ms;          /* the slowest loop turn so far */
    uint64_t deadman_t0;            /* the 10-minute return to the menu counts from here (the start; the self-test
                                     * image: the last input) */
};
extern struct kstate K;
extern int pocket_lock;             /* 0.6: heating with the screen kept on: touch and keys locked until Power */

/* tasks */
void input_init(void);
int input_poll(void);               /* 1 if something happened */
void ui_kinit(void);
void ui_kdraw(void);                /* draw what changed and present */
void ui_kevent(const void *ev);     /* struct ui_ev from sbui */
void ui_mark_dirty(void);
void power_init(void);
void power_timer(unsigned id);
void power_input(void);             /* any input: the idle timers restart, the screen comes on */
void power_key_short(void);         /* screen on/off */
void battery_init(void);
void battery_timer(unsigned id);
void battery_get(struct sbos_battery *b);
void battery_estimate(struct sbos_estimate *e);
int battery_history(int16_t *out, int cap);   /* 0.1 %, oldest first */
void heater_init(void);
void heater_timer(unsigned id);
int heater_request(int level, int minutes);
void heater_status(struct sbos_heat *h);
void heater_stop_all(const char *why);
void sos_init(void);
void sos_timer(unsigned id);
int sos_set(int on);
void sos_status(struct sbos_sos *s);
int sos_active(void);
int sos_frame(uint32_t *argb);      /* 1 while SOS owns the screen: the colour to show */
int clock_now(struct sbos_time *t);
int64_t clock_set_net(int64_t utc, int *was_set);  /* NTP: the correction, s (never written to the PMIC) */
/* the flashlight LED (torch.c) */
int torch_ready(void);
const char *torch_why(void);
int torch_on(void);
int torch_set(int lit);
void torch_timer(unsigned id);
/* the panel's power (panel.c), settings off until turned on */
extern int panel_save_on, panel_off_on;
void panel_kick(void);
void panel_idle(void);
void panel_save_set(int on);
void panel_screen(int on);
unsigned panel_dcs_failures(void);
extern int panel_bright_level;
int panel_brightness(int level);    /* the user's level: 0 untouched, 1..4 low..high: 1 done */
void panel_boost(int who, int on);  /* 1 Extreme, 2 SOS: the top level while any asks */
void panel_emergency(int on);       /* 0.6.1: Emergency Mode's power profile */
int panel_lpm(int on);              /* 0.6.5: the panel's low-power mode (ALPM 10 nits); 1 done */
int panel_dump_acl(void);
/* 1.2-alpha7: the vibration motor (vib.c) */
int vib_ready(void);
const char *vib_why(void);
int vib_pulse(unsigned ms);
int vib_buzz(unsigned n, unsigned on_ms, unsigned off_ms);
void vib_off(void);
void vib_timer(void);
int vib_on(void);
/* 1.2-alpha8: wake-up alarms (alarm.c) */
int alarm_set(int i, int h, int m, int days, int on);
int alarm_any(void);
int alarm_ringing(void);
void alarm_minute(const struct sbos_time *t);
void alarm_timer(void);
int alarm_stop(int snooze);
/* 1.2-alpha9: notifications (notif.c) */
struct notif {
    char who[16], title[40], text[72];
    int hm;                         /* local hh * 100 + mm, -1 unknown */
};
void notif_post(const char *who, const char *title, const char *text);
int notif_count(void);
int notif_unread(void);
const struct notif *notif_get(int i);
void notif_seen(void);
void notif_clear(void);
int heatprobe_step(void);
int gpuprobe_step(void);
uint64_t gpuheat_jobs(void);
extern int gpuheat_alu, gpuprobe_alu;
int panel_dump(void);
int audio_probe(void);              /* 0.7: the codec's state read only, its ID if S-Boot left it powered (audio.c) */               /* 0.6.5: read only: the panel's ID, gamma, ELVSS, VINT, AOR, IRC (logged) */
extern int panel_in_lpm;
extern int em_aod;                  /* 0.6.5: Emergency Mode: a dim clock (the panel's low-power mode) instead of black */
void kui_aod_frame(void);           /* draws and sends the dim clock's picture */
/* files on the card, read only (files.c) */
int kfile_read(const char *path, void *dst, uint32_t cap);
extern int sos_led_on;              /* settings: the LED flashes with SOS */

/* apps (app.c; struct kapp in kapp.h) */
#define APP_MAX 12
extern int napps;
extern int app_front;               /* -1: the system screens (lock, home) */
void apps_load_builtin(void);
void app_event_send(int a, uint32_t type, uint32_t id, int32_t value);
void apps_service(void);            /* deliver queued events, resume running frames */
int app_by_id(const char *id);
int app_hidden(int a);             /* the boot self test's apps: not on home */
const char *app_name(int a);
uint32_t app_color(int a), app_icon(int a);
int64_t sys_dispatch(int a, uint64_t nr, const uint64_t *arg);

/* the UI element list an app built (ui.c) */
void kui_begin(int a);

void kui_title(const char *t);
void kui_button(uint32_t id, const char *label, const char *sub, uint32_t flags);
void kui_big(const char *cap, const char *val, uint32_t c);
void kui_text(const char *t, uint32_t c);
void kui_level(uint32_t id, const char *labels, int cur);
void kui_toggle(uint32_t id, const char *label, int on);
void kui_hold(uint32_t id, const char *label, uint32_t ms, uint32_t flags);
void kui_end(void);
extern int kui_stat_n, kui_stat_ms, kui_stat_frames, ui_overflow, ui_scroll_max;
const char *kui_screen_name(void);
/* the self test (uitest.c, docs/STARBITE-OS.md 12.6): only in starbite-os-selftest.img */
extern const int sbos_selftest_enabled;
void selftest_start(void);
void selftest_timer(unsigned id);
void selftest_input(void);
void kui_input(uint32_t id, const char *label, uint32_t flags, int max, const char *init);
int kui_input_get(char *out, unsigned cap);
void kui_input_wipe(void);
void kui_chart(const char *caption, const int16_t *v, int n, int lo, int hi, uint32_t c, const char *lr);
/* look.c (0.6): the normal mode's wallpaper and glass */
extern int look_emergency;
int look_glass_on(void);
int look_style(void);
int look_is_ready(void);
extern int look_saved_style;
extern int em_set_auto;             /* Emergency Mode turned the 5-minute switch-off on (it takes it back) */
void look_init(int style);
void look_bg(int x, int y, int w, int h);
void look_glass(int x, int y, int w, int h, int r, uint32_t tint, unsigned a);
void look_glass_disc(int cx, int cy, int r, uint32_t tint, unsigned a);
void kui_redraw_all(void);
void kui_control_open(void);
void kui_set_emergency(int on);
void kui_gauge(const char *caption, const char *value, int permille, uint32_t c);
const char *app_tile(int a);        /* the line an app shows under its home tile */
void kui_toast(const char *t, uint32_t ms);
void kui_light(uint32_t argb);
void kui_heat_light(int on);             /* 1.2-alpha5: Survival's white screen with a status strip */
int heater_white(void);
void kui_awake(int on);
void kui_open_app(int a);
void kui_home(void);
void kui_guest_open(int mode);      /* 1.2: the Linux view (GUEST_APP / GUEST_TERM; started if it is off) */
extern int kui_linux_on;            /* 1.2: Settings > Linux apps: the home tile (off at every start) */
void kui_lock(void);
int kui_awake_on(void);
void kui_confirm(const char *title, const char *l1, const char *l2, const char *l3, const char *hold,
                 void (*done)(int ok));
void kui_power_menu(void);
void kui_screen_changed(int on);
void kui_sos_changed(void);
int kui_screen(void);
void ui_ktimer(unsigned id);
void ui_kminute(void);
int heater_running_ui(void);
int heater_busy(void);              /* running, or its first-run confirmation is up */
int heater_m3_busy(void);           /* 1.2: heater_busy, a stop in progress or an M3 worker / probe on a big core */
int heater_guest_refuses(void);     /* 1.2: 1 while Linux owns the M3 cluster: no heat, no OCP probe */
void heater_m3_released(void);      /* 1.2: the guest powered the M3 cluster off: prepare it again before use */
int heater_sensor(int id);          /* SYS_SENSOR_READ: 0.1 C or -1000 */
void power_restart(int to_menu);
void power_restart_recovery(void);  /* 1.2: the Linux self-test image: everything off, then TWRP */
void power_off_now(void);
void power_key_long(void);

/* ---- hardware layer (hw.c on the phone; the host preview's stubs) ------------------------------ */
int hw_irq_init(void);              /* EL0 hardening, GIC + virtual timer + self test: 1 interrupts, 0 polling */
void hw_irq_poll(void);             /* acknowledge what is pending (the loop, every turn) */
void hw_sleep_until(uint64_t at_ms);/* wfi (or a short delay in polling mode) until at_ms or an interrupt */
void hw_arm_until(uint64_t at_ms);  /* the timer for an app frame's budget */
void hw_wdt_feed(void);
void hw_m3_core_on(unsigned w, int first);  /* 1.1-beta2: FlexPMU before an M3 worker's CPU_ON (first: cluster on) */
void hw_m3_core_off(unsigned w, int last);  /* 1.1-beta2: FlexPMU before it leaves (last: cluster off) */
void hw_m3_fp_log(const char *what);
int hw_m3_fp_reset(void);           /* 1.1-beta3: the M3 FlexPMU words back to the boot state (1 if changed) */
int heater_trip_off(void);          /* 1.1-beta2: the chip's trip is off by Survival's switch now */
int hw_dvfs_m3_ocp(uint32_t khz, unsigned busy);
int64_t hw_ocp_launch(unsigned w);  /* 1.1-beta7: sb_ocp_worker on M3 core w (read only) */
int heater_ocp_probe(void);         /* 1.1-beta7: Settings > About: the M3 OCP registers, read only (0 read, < 0 not) */
extern int heater_ocp_state;        /* 0 not probed, 1 readable (Samsung core, values logged), -1 trapped, -2 no start */   /* 1.1-beta6: the M3 above 1794 within Samsung's core-count table */
void hw_wdt_feed_heat(void);
unsigned hw_wdt_selftest(unsigned secs);   /* 1.2: a self-test image's own watchdog at secs (fed by the loop); 0 refused */        /* 1.1: the heat loop's feed (the only one while the trip is off) */
unsigned hw_wdt_short(unsigned secs);   /* 1.1: the loader's watchdog shortened (0: back); 0 = refused */
int hw_dvfs(unsigned domain, uint32_t khz);   /* sb_acpm_dvfs_listed */
int hw_m3_prep(void);               /* the vendor PMU defaults for the M3 cluster: 1 done */
uint32_t hw_a55_khz(void), hw_m3_khz(void);
int hw_m3_on(void);
void hw_screen(int on);             /* v0.1: nothing (the UI draws black); v0.2: panel sleep */
void hw_app_map(uintptr_t slot, int on);
void hw_tls_clear(void);           /* TPIDR_EL0 / TPIDRRO_EL0 zero for a new handler frame */
void hw_code_sync(uintptr_t a, uint64_t n);
int64_t hw_heat_launch(unsigned w, void *cfg);    /* sb_smp_launch: never waits for the core */
void hw_heat_stop_all(void);
unsigned hw_heat_running(void);
int64_t hw_core_psci(unsigned w);   /* PSCI AFFINITY_INFO's raw answer for worker core w */
/* the state slot (doc 6.2): 1 read both halves (STATE_HALF bytes each), 0 no slot; write: 1 written and read back,
 * 0 failed, -1 a dry run (nothing written) */
int hw_slot_read(uint8_t *h0, uint8_t *h1);
int hw_slot_write(int half, const uint8_t *src, uint32_t n);
/* settings that last across a restart (settings.c) */
void settings_restore(void);
void settings_changed(void);
void settings_timer(unsigned id);
int settings_persistent(void);
int settings_selftest(void);       /* uitest.c: 1 passed, 0 failed, -1 no slot */
void settings_save_now(void);
void settings_app_restore(int a);  /* app.c at load: the app's record from the slot into its state */       /* write the settings at once (switch-off), where the slot exists */
extern uint32_t auto_off_min;       /* services.c: switch off after this many minutes with the screen off */
int power_auto_off(int rehearse);
void power_screen_set(int on);
void power_heater_took_clocks(void);
/* Wi-Fi (wifi.c, 0.4) */
struct sbos_wifi;
struct sbos_wifi_net;
int wifi_on(void);
int wifi_power(int on);
int wifi_scan(void);
int wifi_join(int i, const char *pass);    /* pass: NULL open / saved */
void wifi_status(struct sbos_wifi *s);
int wifi_list(struct sbos_wifi_net *out, int max);
int wifi_bar(void);
void wifi_timer(unsigned id);
int wifi_selftest_phase(int *done);
void wifi_irq(void);                /* hw.c's interrupt drain: the radio's INTx */
int hw_spi(unsigned intid, int on); /* one device interrupt (level, cpu0) on or off: 1 done, 0 no interrupts */
void heatk_worker(unsigned w, void *arg);
