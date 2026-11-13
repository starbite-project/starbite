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
#include "passcode.h"                /* Track A: the passcode (pure: passcode.c) */
int sbos_safe_start(void);           /* Track A, 1.0: this start is a safe start (settings.c): nothing saved */

#define SBOS_VERSION "1.2-alpha26"
/* 1.2-alpha26: alpha25's lock screen (OS cards, swipe up, the Emergency hold, Heat only in Emergency) is off: the user
 * rejected it; the UI is being redesigned (docs/concepts/starbite-os-shell). 0 = alpha24's lock screen and home */
#ifndef SBOS_LOCK_V25
#define SBOS_LOCK_V25 0
#endif

/* ---- timers (timer.c, pure) ------------------------------------------------------------------ */
/* Owners: kernel tasks 0..15, apps 16 + app index. */
enum { KT_INPUT = 0, KT_UI, KT_POWER, KT_BATTERY, KT_HEATER, KT_SOS, KT_HOUSE, KT_CLOCK, KT_SELFTEST, KT_STATE, KT_WIFI, KT_GUEST, KT_MODEM, KT_SURV, KT_USB /* usb.c */, KT_APP0 = 16 };
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
    int no_high_stop;               /* 1.2-alpha17: Survival's 4.5 A stop off (the user): a 6 A / 1 s backstop instead */
    uint64_t t_over;                /* when the current first went above the backstop (0 below) */
    int all_out;                    /* 1.2-alpha23 (the user): Survival never holds at a temperature limit and never
                                       throttles for the current or the voltage: every load at its top to the battery
                                       limit or 2 %. Kept: the battery's hold and stop, the 4.5 A switch (its 4.2 A
                                       ceiling while on, the 6 A / 1 s backstop while off), the 3.5 A switch, the chip's
                                       own power cut in Survival with the trip on. A brownout may switch the phone off */
    int gov_ma, backstop_ma;        /* 1.2-alpha18: the governor's current and the backstop (0: the defaults) */
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
    int trim;                       /* 1.2-alpha16: Survival's fine hold: 0 everything at its top, each step the M3
                                       and GPU clocks one level down before a whole load is shed */
    unsigned gpu_mhz;               /* 1.2-alpha16: the GPU clock Survival asks for (heater.c: gpuheat_set_mhz) */
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
    /* 1.2-alpha24 (the user: "unlimited: bypass everything ... except battery temp"): heater.c also forces the trip
     * off, All out, the 4.5 A and 3.5 A switches off (B2SOVRC off); here: a lost SoC reading no longer stops it.
     * Kept: the battery's hold and stop, 2 %, the 6 / 6.5 A for 1 s backstop, the lost battery / gauge readings */
    int unlimited;
    /* 1.2-alpha24: the brownout guard: heater.c samples the gauge every 100 ms (heat_guard_sample); below the floor
     * or on a fast drop, the next step sheds the smallest step (a clock trim, then the GPU, then one M3); nothing is
     * added back until the cell stayed at the floor + 100 mV for 3 s. It also learns the cell's resistance and its
     * open-circuit voltage, and keeps the fast minimum (logged, guard on or off) */
    int guard_on, guard_mv;
    int g_shed, g_last_mv, g_last_d, g_min_mv, g_max_d, g_r_mohm, g_ocv_mv;
    uint64_t g_last_t, g_t_low;
    uint32_t g_samples, g_sheds, g_fast;
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
extern int heat_no_high_stop;
extern int heat_all_out;                   /* 1.2-alpha23: Survival "All out" (on by default): no hold, no throttle */
extern int heat_unlimited;                 /* 1.2-alpha24: Survival "Unlimited": every software limit off but the battery's */
extern int heat_guard_on, heat_guard_mv;   /* 1.2-alpha24: the brownout guard (on by default) and its floor */
void heat_guard_sample(struct heat_ctl *c, int mv, int ma, uint64_t now);
void heater_guard_live(void);
extern int heat_screen_white;       /* 1.2-alpha19: Survival's screen white (1) or dark (0, default); remembered */
extern int heat_b2s_note;           /* 1.2-alpha18: 0x100 | CHG_CNFG_05 as found while Survival has B2SOVRC off (slot) */
void heater_boot_forensics(void);   /* 1.2-alpha18: read only, at every boot */       /* 1.2-alpha17: Survival's 4.5 A stop off (remembered) */
#define HEAT_BACKSTOP_MA 6000       /* 1.2-alpha17: with the 4.5 A stop off: 6 A for 1 s still stops */
#define HEAT_GOV_MA 5000            /* 1.2-alpha18: with it off, Survival steps down from 5.0 A (the MAX77705 cuts at 5.6) */
#define HEAT_GOV_MV 3350            /* and from 3.35 V on the cell (a brownout's margin) */
#define HEAT_LEAK_C10 1650          /* 1.2-alpha19: from 165 C (the leakage runaway) the governor steps every 250 ms, */
#define HEAT_LEAK_MA 5600           /* from 5.6 A */
#define HEAT_LEAK_MV 3360           /* or 3.36 V */
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
#define HLOG_MAGIC 0x32474c48U      /* "HLG2" (1.2-alpha18: ma = the second's peak, mv = its lowest) */
struct hlog {
    uint32_t magic, n;              /* n: samples written (up to HLOG_N; past HLOG_FAST the ring wraps) */
    uint32_t level, a55_mhz;
    uint16_t m3_n, gpu_mhz;
    uint32_t pad;
    struct hlog_sample s[HLOG_N];
};
extern struct hlog heat_log;
void heater_log_summary(struct sbos_heat_log *o);   /* 1.2-alpha22: SYS_HEAT_LOG */
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
/* Track A: walk a valid half's records (*at = 0 first): 1 a record (id, data, n), 0 the end. */
int state_next(const uint8_t *half, uint32_t *at, char id[24], const uint8_t **data, uint32_t *n);

/* ---- prefs and blobs (prefs.c, pure; Track A, sbos-core-api.md 1-2): kept in the state slot with the settings -- */
#define PREFS_KEY_MAX 31U           /* keys: [a-z0-9._-], 1..31 characters */
#define PREFS_VAL_MAX 255U          /* values: C strings up to 255 bytes */
#define PREFS_KEYS 400
#define PREFS_BYTES (64U << 10)     /* the packed keys and values (17 records at most) */
#define PREFS_BLOB_NAME 16U         /* blob names: as keys, up to 16 ("blob:<name>#7" fits a record id) */
#define PREFS_BLOB_MAX (32U << 10)
#define PREFS_BLOBS 16
#define PREFS_BLOB_BYTES (160U << 10)
#define PREFS_MEM (PREFS_BYTES + PREFS_BLOB_BYTES)   /* prefs_init's block */
void prefs_init(void *mem);         /* PREFS_MEM bytes; until then (or with 0) every call refuses */
void prefs_reset(void);             /* empty (RAM only) */
int prefs_key_ok(const char *key);
int prefs_get(const char *key, char *val, int cap);              /* the value's length, -1 not set */
int prefs_set(const char *key, const char *val);                 /* 1 changed, 0 the same, -1 refused; val 0 deletes */
int prefs_blob_put(const char *name, const void *p, uint32_t n);  /* 1 changed, 0 the same, -1 refused; n 0 deletes */
int prefs_blob_get(const char *name, void *p, uint32_t cap);     /* bytes copied (up to cap), -1 none */
void prefs_counts(int *keys, uint32_t *bytes, int *blobs, uint32_t *blob_bytes);
/* the slot: save adds the records (all prefs or none, each blob whole or not at all): 0, -1 something left out;
 * restore reads a valid half: 0, -1 something was not taken (a newer version, a broken entry) */
int prefs_save(struct state_buf *b);
int prefs_restore(const uint8_t *half);

/* ---- documents (docs.c, pure; Track A, 8a): Notes' and Contacts' stores, kept in the state slot -------------- */
#define DOC_COLLS 2
enum { DOC_NOTES = 0, DOC_CONTACTS = 1 };
#define DOC_MAX (32U << 10)                 /* one document */
#define DOC_BUDGET_NOTES (96U << 10)
#define DOC_BUDGET_CONTACTS (64U << 10)
#define DOC_ENTRIES 768                      /* documents in all */
#define DOCS_MEM (DOC_BUDGET_NOTES + DOC_BUDGET_CONTACTS)
void docs_init(void *mem);          /* DOCS_MEM bytes; until then every call refuses */
void docs_reset(void);
int docs_put(int c, uint32_t id, const void *p, uint32_t n);   /* 1 changed, 0 the same, -1 refused (full, too big); n 0 deletes */
int docs_get(int c, uint32_t id, void *p, uint32_t cap);       /* the document's length (copied up to cap), -1 none */
int docs_count(int c);
uint32_t docs_id_at(int c, int k);  /* the k-th id in id order, 0 none */
uint32_t docs_new_id(int c);        /* an unused id (the highest + 1) */
void docs_usage(int c, uint32_t *used, uint32_t *budget);
int docs_save(struct state_buf *b);  /* each document whole or not at all: 0, -1 something left out */
int docs_restore(const uint8_t *half);   /* 0, -1 something was not taken */

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
    /* cp7k: the same without the self test's own timer (KT_SELFTEST: its film setup), for its summary: the slowest
     * turn, its slowest piece ("timer", "input", ...) with a timer's owner / id, and the turns of 100 ms or more (the
     * gate; the log line is >= 200 ms) */
    uint32_t user_turn_ms, user_turn_owner, user_turn_id, turns_100;
    const char *user_turn_what;
    uint16_t turns_100_by[18];      /* cp7p: those turns by their slowest piece: a timer's owner 0..15 (KT_*), 16 an
                                     * app timer (KT_APP0+), 17 not a timer (input, apps, draw) */
    uint64_t deadman_t0;            /* the 10-minute return to the menu counts from here (the start; the self-test
                                     * image: the last input) */
};
extern struct kstate K;
extern int pocket_lock;             /* 0.6: heating with the screen kept on: touch and keys locked until Power */

/* tasks */
void input_init(void);
int input_poll(void);               /* 1 if something happened */
extern int input_fast;              /* P3k: Next keeps touch reads at 16 ms while the screen is on */
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
uint64_t sos_next_ms(uint64_t now); /* 1.2-alpha28: when sos_frame can change next (sb_ms time; 0 off) */
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
int panel_blank(int on);           /* Track A: the in-call proximity: display off / on, the screen kept; 1 done */
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
/* drivers track (haptic.c): the strength (PWM duty, per mille, 500..700), a gap, the pattern player's hook */
unsigned vib_set_duty(unsigned pm);
void vib_wait(unsigned ms);
void vib_set_end_hook(void (*fn)(int ended));
/* 1.2-alpha8: wake-up alarms (alarm.c) */
int alarm_set(int i, int h, int m, int days, int on);
int alarm_any(void);
int alarm_first(int *h, int *m);    /* P2y: the earliest alarm on (time of day); 0 none */
int alarm_ringing(void);
void alarm_minute(const struct sbos_time *t);
void alarm_timer(void);
int alarm_stop(int snooze);
void alarm_sound(int on);                     /* cp7x: the alarm's sound (weak no-op; the Next shell plays it) */
/* Track A (2026-11-12): Next's Alarm tab (the list of up to four, labels), kept in the record "sbos.alarm" */
#define ALARM_SLOTS 32               /* 8a (Track A; iOS has no small limit): was 4 */
#define ALARM_REC_SIZE (4 + ALARM_SLOTS * 28)
#define ALARM_REC_SIZE_V1 (4 + 4 * 28)   /* a record from before 8a (four slots): still read */
int alarm_count(void);
int alarm_get(int i, int *hm, int *days, int *on, char *label, int cap);
int alarm_put(int i, int hm, int days, int on, const char *label);   /* i == alarm_count(): add */
int alarm_del(int i);
int alarm_pack(uint8_t out[ALARM_REC_SIZE]);        /* 1: the list is the kernel's (save it) */
void alarm_unpack(const uint8_t *in, unsigned len);   /* ALARM_REC_SIZE or ALARM_REC_SIZE_V1 */
int alarm_app_state(uint8_t out[20]);               /* the classic Clock app's state as the list (0: none yet) */
/* Track A: the time zone in use (tzdb.c's index) and 12 / 24-hour; services.c, saved by settings.c */
int clock_zone(void);
void clock_zone_set(int i);
extern int clock_24h;
/* 1.2-alpha9: notifications (notif.c) */
struct notif {
    char who[16], title[40], text[72];
    int hm;                         /* local hh * 100 + mm, -1 unknown */
    unsigned flags;                 /* Track A: NOTIF_* (0 from notif_post) */
};
void notif_post(const char *who, const char *title, const char *text);
/* Track A (sbos-core-api.md 7): a post by an app id ("phone", a Linux app's bridge, ...): who = app. Next's shell
 * applies notif.dnd and notif.<app>.allow / .banners before it shows a banner */
#define NOTIF_QUIET 1U              /* the Notification Centre only: never a banner */
#define NOTIF_URGENT 2U             /* a banner also in Do Not Disturb (an alarm, SOS) */
void notif_post_app(const char *app, const char *title, const char *text, unsigned flags);
void power_last2_clock(int on);      /* 1.2-alpha27: Last 2 % (survival.c): the A55 at 455 MHz, screen on or off */
int notif_count(void);
unsigned notif_seq(void);           /* posts since boot (Starbite OS Next's shell copies the new ones) */
int notif_unread(void);
const struct notif *notif_get(int i);
void notif_seen(void);
void notif_clear(void);
int heatprobe_step(void);
int gpuprobe_step(void);
uint64_t gpuheat_jobs(void);
int gpuheat_set_mhz(unsigned mhz);  /* 1.2-alpha16: PLL_G3D above ACPM's 598 (S 0, the vendor's sequence) */
int gpu_pms_ok(uint32_t p, uint32_t m, uint32_t s);
int gpu_pll_set(uint32_t p, uint32_t m, uint32_t s);
#define SURV_GPU_MHZ 858U           /* 1.2-alpha16: H2b/H2c on the phone: 858 MHz held 30 s at 950 mV (cycle counter 857) */
extern int gpuheat_alu, gpuprobe_alu;
int panel_dump(void);
int audio_probe(void);              /* 0.7: the codec's state read only, its ID if S-Boot left it powered (audio.c) */               /* 0.6.5: read only: the panel's ID, gamma, ELVSS, VINT, AOR, IRC (logged) */
extern int panel_in_lpm;
extern int em_aod;                  /* 0.6.5: Emergency Mode: a dim clock (the panel's low-power mode) instead of black */
void kui_aod_frame(void);           /* draws and sends the dim clock's picture */
/* files on the card, read only (files.c) */
int kfile_read(const char *path, void *dst, uint32_t cap);
/* 1.2-alpha21: entries [first, first + max) of a folder: stored n, *total all (read only) */
int kfile_list(const char *path, struct sbos_dirent *out, int max, int first, int *total);
/* Next's Photos / Files / Music (read only): the state without touching the card (1 mounted, 0 not tried, -1 no card,
 * -2 a card that cannot be read; *why in words), the mount (blocks: from an idle turn), a folder of any size a page
 * at a time with dates (exFAT's stamp, see exfat.h), a file's bytes from an offset (how many were read) */
struct kfile_ent { char name[128]; uint64_t size; int32_t dir; uint32_t mtime; };
int kfile_state(const char **why);
int kfile_mount(void);
int kfile_list_at(const char *path, struct kfile_ent *out, int max, int first);
int kfile_read_at(const char *path, uint64_t off, void *dst, uint32_t len);
extern int sos_led_on;              /* settings: the LED flashes with SOS */

/* apps (app.c; struct kapp in kapp.h) */
#define APP_MAX 20                      /* 1.2-alpha21: was 12 (alpha20 had 13 in its pack: test-fault was dropped) */
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
extern int kui_hold_awake;              /* Track A, 1.0: long jobs keeping the screen on (Next: sh_hold_awake) */
extern int (*kui_confirm_hook)(const char *title, const char *l1, const char *l2, const char *l3, const char *hold,
                               void (*done)(int ok));
void kui_confirm(const char *title, const char *l1, const char *l2, const char *l3, const char *hold,
                 void (*done)(int ok));
void kui_power_menu(void);
extern void (*kui_finger_hook)(int type, int id, int x, int y);   /* 8a: fingers 1.. (KIN_PRESS / MOVE / RELEASE, panel px) */
extern int (*kui_power_hook)(void);     /* Track A, 1.0: a UI with its own power sheet (Next); 1 = handled */
void kui_screen_changed(int on);
/* P2r (Starbite OS Next): the UI's own screen off / on (no classic black frame) and the Power key's or the screen-off
 * timer's fade first (1: the UI turns the screen off itself, through power_screen_set, when the fade has ended;
 * key: the Power key, whose second press during the fade undoes it) */
extern void (*kui_screen_hook)(int on);
extern int (*kui_sleep_hook)(int key);
/* Track A lifecycle: the UI's save before a restart or a switch-off (Next: every app's on_save, sh_save_all); 0 none */
extern void (*kui_save_hook)(void);
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
void clock_restore_saved(uint32_t utc);  /* 1.2-alpha26: the last known time from the state slot */
uint32_t clock_saved_value(void);       /* the time to keep in the slot */
void clock_boot_log(void);              /* what the PMIC clock says at boot */
int wifi_time_only(void);               /* 1.2-alpha26: Wi-Fi is on only to get the time (at boot) */
void wifi_time_only_end(const char *why);
void wifi_boot_time_check(void);
void power_boot_os(const char *key);    /* 1.2-alpha25: the lock screen's OS card (sb_boot_next) */
int kui_test_os_card(void);
int gpucomp_selftest_step(void);        /* P4: the GPU compositor's test (gpucomp.c); 1 when done */
/* P4: the GPU compositor's backend for the UI (gpucomp.c). A quad is one of the toolkit's draw items: a bitmap (or a
 * solid of w x h), scaled (isx / isy: source px per screen px) from the screen point (dx0, dy0), alpha, clipped to a
 * rect with a rounded edge, faded by screen y, 3-sliced, and glass (the backdrop bd through the 8-bit mask) under it */
#define GPUCOMP_MAX_QUADS 200U              /* 1.0: was 127; comp2's quad area holds 204 (bins keep byte indices <= 254) */
enum { GC_SOLID = 1, GC_BILINEAR = 2, GC_GLASS = 4, GC_FADE = 8, GC_SLICE_V = 16, GC_SLICE_H = 32,
       GC_OPAQUE = 64,                  /* (CPU only) every px of src is opaque: what is under it is culled */
       GC_LENS = 256 };                 /* v9: Liquid Glass over the shape lx..lh (lens.h), the backdrop bd: a quad of its own */
#define GC_OCC_PX 32                    /* gc_quad.occ: per 32 x 32 px block of src, 0 empty, 1 partial, 2 opaque */
struct gc_quad {
    const void *src;
    uint32_t stride, w, h, flags, solid;
    float alpha, dx0, dy0, isx, isy, cx0, cy0, cx1, cy1, radius, ft0, ft1, fb0, fb1, len, s0, s1;
    const void *mask;
    uint32_t mstride;
    const void *bd;
    uint32_t bstride;
    float bk;
    uint32_t bw, bh;
    float lx, ly, lw, lh;               /* v9 (GC_LENS): the glass shape (screen px; radius in radius); the quad is a
                                         * transparent solid (its src unused) reaching LENS_APRON px past the shape */
    float lsat;                         /* v10 (GC_LENS): the backdrop's saturation (0 = 1) */
    uint32_t live;                      /* v10 (GC_LENS, the caller's): the backdrop is its first `live` quads, blurred
                                         * (gpucomp_backdrop); 0: bd as given */
    const uint8_t *occ;                 /* (CPU only, may be 0) src's block map, occ_w x occ_h: bins skip empty blocks */
    uint32_t occ_w, occ_h;
    uint32_t gen;                       /* (CPU only) the caller's content stamp of src / mask / backdrop */
    float lq[16];                       /* 1.0 (GC_LENS, glass V3): the lens's record (lens.h lens_make, LQ_N) */
};
int gpucomp_init(void *heap, uint64_t bytes);
/* the last compose's bins: entries drawn, entries culled under opaque quads, entries skipped over empty blocks */
void gpucomp_last_stats(uint32_t *entries, uint32_t *culled, uint32_t *empty);
void gpucomp_last_times(uint32_t *bin_us, uint32_t *up_us, uint32_t *run_us);   /* 8c: the last compose's CPU parts */
unsigned gpucomp_mhz(void);
int gpucomp_ready(void);
void *gpucomp_pool(void);
void gpucomp_add_ro(const void *p, uint64_t bytes);   /* before gpucomp_init: read-only memory layers point into */
void gpucomp_add_ext(const void *p, uint64_t bytes);  /* 1.0, before gpucomp_init: memory sampled in place (guest surfaces) */
int gpucomp_ext_ok(void);                              /* 1.0: that range is mapped (else the shell copies the window) */
uint64_t gpucomp_pool_bytes(void);
uint64_t gpucomp_va(const void *p);
void gpucomp_clean(const void *p, uint64_t n);
int gpucomp_compose(void *dst, uint32_t bg, const struct gc_quad *q, unsigned n, uint64_t *us);
struct gc_rect { int x0, y0, x1, y1; };     /* half-open, screen px (the toolkit's tk_irect) */
/* as gpucomp_compose, but only the 32 x 32 px tiles the rects touch are drawn; the rest of dst stays */
int gpucomp_compose_damage(void *dst, uint32_t bg, const struct gc_quad *q, unsigned n, const struct gc_rect *r,
                           unsigned nr, uint64_t *us);
/* v7: a big move's frame: composed at half resolution (the pool's top), then 2x into dst (the whole screen) */
int gpucomp_compose_half(void *dst, uint32_t bg, const struct gc_quad *q, unsigned n, uint64_t *us);
/* v10, a live glass backdrop: the quads composed at a quarter of the screen, then blurred (dual filter down to 1/16,
 * up to 1/8) into backdrop `level` (0 .. GPUCOMP_LEVELS - 1); *bd etc. are what a GC_LENS quad points at. 1 done */
#define GPUCOMP_LEVELS 3
int gpucomp_backdrop(unsigned level, uint32_t bg, const struct gc_quad *q, unsigned n, const void **bd,
                     uint32_t *bstride, uint32_t *bw, uint32_t *bh, float *bk, uint64_t *us);
int uibench_setup(void);                /* 1.2-alpha26 P0: the UI compositor's benchmark (uibench.c) */
void uibench_set(const char *label, unsigned ncores, const unsigned *cores, int worker_only);
int app_is_test(int a);
extern int selftest_warnings;           /* 1.2-alpha25: the self test's warnings (the router's DHCP) */                 /* 1.2-alpha25: the test apps (app_hidden adds Heat outside Emergency) */             /* 1.2-alpha25: the self test's dry run of a card (the count of cards) */
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
void hw_slot_lend(void);            /* Track A update: the fence's one volatile grant was taken (slot.c takes it back) */
/* Track A: Software Update (update.c; docs/STARBITE-FIRMWARE-LAYER.md 10.5): the card's /STARBITE/UPDATE/MANIFEST.TXT,
 * staged into the inactive update slot a chunk per call, armed for the loader, committed 10 s into the next start */
enum { KUP_FAILED = -1, KUP_NONE = 0, KUP_FOUND, KUP_STAGING, KUP_READY, KUP_INSTALLED };
struct kupdate {
    char key[24], version[32], image[64], notes[160];
    uint64_t size;
    uint8_t sha[32];
    int state, pct;                 /* KUP_*; staging 0..100 */
    char why[80];                   /* KUP_FAILED: why; otherwise a detail ("No microSD card.") */
};
int kupdate_parse(const char *txt, int n, struct kupdate *u);   /* 0: every field there and well formed */
unsigned kupdate_target(unsigned slot_active);                  /* the slot to stage into: 1 A, 2 B */
int kupdate_check(struct kupdate *u);                           /* 1 an update for this row found */
int kupdate_stage(struct kupdate *u, uint8_t *buf, uint32_t cap);   /* one chunk (cap <= 4 MiB): the new state */
int kupdate_install(char *why, unsigned cap);                   /* arms and restarts; back here: refused (-1) */
int kupdate_after_start(void);      /* 10 s after the home screen: 1 committed, 2 a rollback told, 0 nothing, -1 */
int kupdate_committed(void);        /* 1 this start committed an update */
void kupdate_dry(int on);           /* the host test: every write a dry run (the self test always is) */
void kupdate_reset(void);           /* forget what was found or staged (the host test's next phone) */
/* Track A: the Camera's photo store (photos.c): the home store's volatile entry "sbos-photos" (64 MiB), a ring of 32
 * self-describing 2 MiB slots, 24-bit RGB; larger pictures scaled down to KPHOTO_MAX_PX. work: KPHOTO_WORK bytes */
#define KPHOTO_SLOTS 32
#define KPHOTO_SLOT_BYTES (2U << 20)
#define KPHOTO_MAX_PX ((KPHOTO_SLOT_BYTES - 4096U) / 3U)         /* 697685: 612 x 816 kept, 1080 x 1440 -> 723 x 964 */
#define KPHOTO_MAX_SIDE 4096
#define KPHOTO_WORK (KPHOTO_SLOT_BYTES - 4096U)
int kphotos_save(const uint32_t *argb, int w, int h, int64_t utc, uint8_t *work, uint32_t cap);   /* 0, -1 (why) */
int kphotos_count(void);
/* i 0 = the newest: pixels written (argb 0: the count only, w / h / utc filled), -1 (why) */
int kphotos_get(int i, uint32_t *argb, int cap_px, int *w, int *h, int64_t *utc, uint8_t *work, uint32_t cap);
int kphotos_delete(int i);          /* 0, -1 (why) */
const char *kphotos_why(void);
void kphotos_fit(int w, int h, int *sw, int *sh);   /* the size a w x h picture is kept at */
void kphotos_dry(int on);           /* the host test: every write a dry run (the self test always is) */
void kphotos_reset(void);           /* forget the headers read (the host test's restart) */
/* settings that last across a restart (settings.c) */
void settings_restore(void);
void settings_changed(void);
void settings_changed_soon(uint32_t ms);
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
extern int wifi_autojoin;              /* 1.2-alpha21: join a known network automatically (remembered) */
int wifi_known(struct sbos_wifi_known *out, int max);
int wifi_forget(int k);
int wifi_selftest_phase(int *done);
/* Track H: a listed network's security (from its RSN / WPA elements), the firmware's capabilities, leaving */
enum { KWIFI_SEC_OPEN, KWIFI_SEC_WEP, KWIFI_SEC_WPA1, KWIFI_SEC_WPA2, KWIFI_SEC_WPA23, KWIFI_SEC_WPA3, KWIFI_SEC_ENT,
       KWIFI_SEC_OWE, KWIFI_SEC_OTHER };
int wifi_net_sec(int i);                    /* of wifi_list's i-th; -1 none */
#define KWIFI_CAP_SAE 1U
#define KWIFI_CAP_SAE_EXT 2U
#define KWIFI_CAP_MFP 4U
#define KWIFI_CAP_IDSUP 8U
#define KWIFI_CAP_WPA3 16U
unsigned wifi_caps(void);                   /* KWIFI_CAP_* from the firmware's "cap" (0 until the radio was up) */
int wifi_leave(void);
/* Track H: remembered networks (kept in the state slot), joining by name, details, the user's switch, Survival */
struct kwifi_known {
    uint8_t ssid[33], ssid_len;
    int32_t saved, heard, hidden, autojoin, sec, joined;   /* sec: KWIFI_SEC_*, -1 not known */
};
int wifi_known2(struct kwifi_known *out, int max);  /* the saved one first, then most recently joined first */
int wifi_known_auto(int k, int on);                 /* a remembered network's automatic join */
int wifi_join_name(const uint8_t *ssid, unsigned len, const char *pass);   /* Other...: hidden; pass "" open */
struct kwifi_details {
    int32_t state, rssi, channel, band5, sec, power_save, ping_ms;
    uint32_t lease_s;
    uint8_t ssid[33], ssid_len, ip[4], mask[4], gateway[4], dns[4];
};
void wifi_details(struct kwifi_details *d);
int wifi_user_power(int on);                /* the user's switch: as wifi_power, and remembered for the next start */
int wifi_user_on(void);
void wifi_survival(int on);                 /* Survival mode: Wi-Fi off and nothing automatic until it ends */
extern int wifi_pm;                         /* power save once joined: 2 (default) PM_FAST, 0 off */
struct state_buf;
int wifi_state_put(struct state_buf *b);    /* settings.c: the record "wifi.known" */
void wifi_state_get(const uint8_t *half);
struct sbnet;
struct sbtcp;
int wifi_uplink(struct sbnet **n, struct sbtcp **t);   /* 1 Wi-Fi, 2 mobile data, 0 none (Wi-Fi preferred) */
int wifi_module(void);                      /* the radio module: 1 SEMCO, 0 Murata, -1 not known (for Bluetooth) */
extern int wifi_selftest_scan_only;          /* Next: the Wi-Fi self test scans only (and runs the TLS self-check) */
/* Next (wifi-sbos-next): one HTTP(S) GET at a time over the joined network (starbite/wifi/http.h). wifi_fetch:
 * 0 started, SBOS_EAGAIN not joined (or no memory), SBOS_EBUSY one is running. The status says how it went; the
 * body (up to 64 KiB) stays until the next fetch. Never logged: the URL, the host, the body. */
struct sbos_wifi_fetch {
    int32_t state;                          /* 0 none yet, 1 running, 2 finished */
    int32_t result;                         /* SBHTTP_DONE (1) or an SBHTTP_E_* (< 0) */
    int32_t status, tls_err, tls, secs;     /* the HTTP status; BearSSL's error; 1 https; seconds so far */
    uint32_t tls_version, tls_suite, bytes, kept;
    uint32_t dns_ms, connect_ms, tls_ms, first_byte_ms, total_ms;
    char host[64];                          /* for the screen */
    char why[64];                           /* the result in words */
};
int wifi_fetch(const char *url);
void wifi_fetch_status(struct sbos_wifi_fetch *f);
const uint8_t *wifi_fetch_body(uint32_t *len);
void wifi_fetch_cancel(void);
int wifi_tls_selfcheck(void);               /* 0 passed, else the failed check's number (logged) */
/* 1.2-alpha21: the modem, receive only (modem.c) */
/* mobile data (kern/modem.c, Next): the user's Settings > Cellular switch is the only way the radio transmits.
 * kmodem_cellular(1) starts the modem (SIM, radio on, the LTE attach APN, registration, the PDP context, a raw-IP
 * interface); 0 deactivates, detaches and parks it. kmodem_netif gives the interface while data is up. */
enum { KCELL_OFF = 0, KCELL_STARTING, KCELL_NO_SIM, KCELL_SIM_LOCKED, KCELL_SEARCHING, KCELL_REGISTERED,
       KCELL_DATA, KCELL_STOPPING, KCELL_FAILED };
struct kcell {
    int32_t state, want;                    /* KCELL_*; want: the switch */
    int32_t bars, rssi_dbm, roaming;        /* bars -1 unknown */
    char rat[8];                            /* "LTE", "UMTS", "GSM", "" */
    char oper[24];                          /* the operator's name: SCREEN ONLY, never logged */
    char why[64];                           /* the last failure or step, in words */
    uint32_t tx_packets, rx_packets;
};
void kmodem_reserve(void);              /* boot, before the UI's heap: the modem's buffers (no I/O) */
/* calls + SMS (kern/modem.c, Next; the notes' modem-calls-sms-api.md). Only while Cellular is on and registered.
 * Emergency numbers are refused before any frame is built, always; numbers and text never reach the log. */
#define KMODEM_CALLS 1
enum { KCALL_IDLE = 0, KCALL_DIALING, KCALL_ALERTING, KCALL_INCOMING, KCALL_ACTIVE, KCALL_HELD, KCALL_ENDING,
       KCALL_ENDED };
enum { KAUDIO_NONE = 0, KAUDIO_EARPIECE, KAUDIO_SPEAKER };
struct kcall {
    int32_t state, outgoing;
    int32_t secs;
    int32_t audio, muted;
    int32_t audio_ok;                       /* 1: the call audio path is up (ABOX + VSS + the amp) */
    char number[24];                        /* SCREEN ONLY */
    char why[48];
    char audio_why[48];                     /* when audio_ok is 0: which part is missing, in words for the call screen */
};
int kmodem_call_dial(const char *number);
int kmodem_call_answer(void);
int kmodem_call_hangup(void);
int kmodem_call_route(int audio);
int kmodem_call_mute(int on);
int kmodem_call_dtmf(char digit);
void kmodem_call(struct kcall *c);
#define KMODEM_SMS_STORE 1                 /* Track G: the store, unread, delete, delivery reports (kmodem_sms_read ...) */
#define KSMS_MAX 200                        /* kept in RAM and in the state slot ("sms.NN"); the oldest go first */
enum { KSMS_RECEIVED = 0, KSMS_SENDING, KSMS_SENT, KSMS_FAILED, KSMS_DELIVERED, KSMS_NOT_DELIVERED };
struct ksms {
    int32_t id, incoming, state;            /* state: KSMS_* (sent = no delivery report yet) */
    int64_t utc;
    char number[24];                        /* SCREEN ONLY */
    char text[512];
    char why[48];
    int32_t unread;                         /* an incoming message not yet shown (kmodem_sms_read) */
    uint8_t mr[4], parts, rep;              /* kernel only: each part's reference, the parts delivered (bits) */
};
int kmodem_sms_send(const char *number, const char *utf8);
int kmodem_sms_count(void);                 /* oldest first, newest last */
int kmodem_sms_get(int i, struct ksms *m);
uint32_t kmodem_sms_gen(void);
int kmodem_sms_read(const char *number);    /* that thread's incoming marked read; the number of messages changed */
int kmodem_sms_delete(int32_t id);          /* 0, SBOS_EINVAL no such message, SBOS_EBUSY it is being sent */
int kmodem_sms_delete_thread(const char *number);   /* the number of messages deleted (never one being sent) */
struct state_buf;
void kmodem_sms_save(struct state_buf *b);  /* settings.c's save: the messages as "sms.NN" records */
void kmodem_sms_restore(const uint8_t *half);   /* settings.c's restore: the newest valid half */
#define KMODEM_APN 1                       /* Settings > Cellular > APN */
int kmodem_set_apn(const char *apn);       /* [A-Za-z0-9.-] 1..63, "" = "internet"; from the next Cellular on; saved */
void kmodem_get_apn(char *out, unsigned cap);
int kmodem_cellular(int on);
int kmodem_selftest_phase(int *done);  /* next-celltest (unattended data): every 500 ms; *done at the end, 0 = failed */
void kmodem_selftest_abort(void);      /* next-celltest stopped by a touch or key: Cellular off */
void kmodem_cell(struct kcell *c);
struct sbnet;
struct sbtcp;
int kmodem_netif(struct sbnet **net, struct sbtcp **tcp);   /* 1 = up (pointers filled when not NULL) */
/* guestnet.c (Track G): the Linux guest's network, NATed onto the active uplink (Wi-Fi first, else mobile data).
 * The uplinks report themselves (n = 0: down) and offer every received IPv4 packet first: 1 = it was the guest's. */
#define SBOS_HAVE_GUESTNET 1               /* wifi-core's hooks in kern/wifi.c compile in on this */
enum { KNET_UPLINK_WIFI = 1, KNET_UPLINK_CELL = 2 };   /* = sbshm-net.h's SBSHM_NET_UPLINK_* */
void guestnet_uplink(int which, struct sbnet *n);
int guestnet_rx(int which, const uint8_t *ip, uint32_t len);
void guestnet_guest_started(void);         /* the guest's service, once sbghost runs: the uplink that is up */
void wifi_fetch_poll_cell(void);
void wifi_clock_checked_by(int how);        /* 1 checked, 2 corrected, 3 set (as Wi-Fi's NTP) */
int kmodem_on(void);
int kmodem_power(int on);              /* 1: the confirmation (the start only on its hold); 0: stop */
void kmodem_stop(const char *why);     /* park now (no-op when off) */
void kmodem_timer(unsigned id);
void kmodem_status(struct sbos_phone *s);
void wifi_irq(void);                /* hw.c's interrupt drain: the radio's INTx */
int hw_spi(unsigned intid, int on); /* one device interrupt (level, cpu0) on or off: 1 done, 0 no interrupts */
void heatk_worker(unsigned w, void *arg);
