/* SPDX-License-Identifier: MIT
 * Starbite OS app API, version 1 (docs/STARBITE-OS.md section 5). The one header an app includes.
 *
 * An app is an event handler: the kernel calls app_event(ev) at EL0 and the app answers with the calls
 * below (svc #0, x8 = number). The numbers never change meaning; new calls get new numbers. Calls of a
 * group whose driver does not exist yet return SBOS_ENOSYS, so an app can ask safely.
 *
 * MIT licensed (unlike the GPL-2.0 kernel) so that apps may use any licence.
 */
#pragma once
#include <stdint.h>

#define SBOS_API 1

/* ---- call numbers ---------------------------------------------------------------------------- */
enum {
    SYS_RETURN = 0, SYS_LOG = 1, SYS_VERSION = 2, SYS_ALLOC = 3, SYS_EXIT = 4,
    SYS_APP_OPEN = 5,                                       /* 0.5.11: another app to the front */
    SYS_NOTIFY = 6,                                         /* 1.2-alpha9: a notification (title, text) */
    /* UI 10..29 */
    SYS_UI_BEGIN = 10, SYS_UI_TITLE = 11, SYS_UI_BUTTON = 12, SYS_UI_BIG = 13, SYS_UI_TEXT = 14,
    SYS_UI_LEVEL = 15, SYS_UI_TOGGLE = 16, SYS_UI_HOLD = 17, SYS_UI_END = 18, SYS_UI_TOAST = 19,
    SYS_UI_LIGHT = 20, SYS_UI_AWAKE = 21,
    SYS_UI_TILE = 22, SYS_UI_GAUGE = 23,                   /* API 1, added in 0.3 */
    SYS_UI_INPUT = 24, SYS_UI_INPUT_GET = 25,              /* 0.5: a text field and the keyboard */
    SYS_UI_CHART = 26,                                      /* 0.5.6: a line chart */
    /* time 30..39 */
    SYS_TIME_NOW = 30, SYS_TIMER_SET = 31, SYS_TIMER_CANCEL = 32, SYS_UPTIME_MS = 33,
    /* power 40..49 */
    SYS_BATTERY = 40, SYS_ESTIMATE = 41, SYS_POWER_REQUEST = 42, SYS_SYSINFO = 43,
    SYS_SETTING_GET = 44, SYS_SETTING_SET = 45,            /* API 1, added in 0.2 */
    SYS_BATTERY_HISTORY = 46,                               /* 0.5.6 */
    /* light 50..59 */
    SYS_TORCH = 50, SYS_STROBE = 51,
    /* heat 60..69 */
    SYS_HEAT_REQUEST = 60, SYS_HEAT_STATUS = 61,
    SYS_HEAT_LOG = 62,                                      /* 1.2-alpha22: the last session's heat log */
    /* alarm and sound 70..79 */
    SYS_SIREN = 70, SYS_VIBRATE = 71, SYS_ALARM_SET = 72, SYS_ALARM_CANCEL = 73, SYS_SOS_STATUS = 74,
    /* state 80..89 */
    SYS_STATE_PUT = 80, SYS_STATE_GET = 81,
    /* files 90..99 (read only), sensors 100..119, phone 120..139, network 140..159, camera 160..179:
     * reserved, SBOS_ENOSYS in API 1 */
    SYS_FILE_OPEN = 90, SYS_FILE_READ = 91,                 /* 91: added in 0.2 */
    SYS_FILE_LIST = 92,                                     /* 1.2-alpha21: a folder's entries */
    SYS_SENSOR_READ = 100,                                  /* 0.3: temperatures */
    SYS_PHONE_STATE = 120, SYS_CALL_DIAL = 121, SYS_CALL_HANGUP = 122, SYS_SMS_SEND = 123, SYS_SMS_READ = 124,
    SYS_MODEM_POWER = 125,                                  /* 1.2-alpha21: 120 and 125 only (receive only) */
    SYS_NET_STATE = 140,                                    /* 0.4: Wi-Fi's state (struct sbos_wifi) */
    SYS_WIFI_POWER = 141, SYS_WIFI_SCAN = 142, SYS_WIFI_LIST = 143, SYS_WIFI_JOIN = 144,   /* 0.4 */
    SYS_WIFI_KNOWN = 145, SYS_WIFI_FORGET = 146,           /* 1.2-alpha21 */
    SYS_CAM_OPEN = 160,
    SYS_MAX = 180
};

#define SBOS_OK 0
#define SBOS_ENOSYS (-38)
#define SBOS_EPERM (-1)
#define SBOS_EINVAL (-22)
#define SBOS_EBUSY (-16)
#define SBOS_EAGAIN (-11)           /* refused for now (a precondition: see the status call) */

/* permissions (app header) */
#define SBOS_P_LIGHT   (1U << 0)
#define SBOS_P_HEAT    (1U << 1)
#define SBOS_P_ALARM   (1U << 2)
#define SBOS_P_FILES   (1U << 3)
#define SBOS_P_SENSORS (1U << 4)
#define SBOS_P_PHONE   (1U << 5)
#define SBOS_P_NET     (1U << 6)
#define SBOS_P_CAMERA  (1U << 7)

/* ---- events ---------------------------------------------------------------------------------- */
enum {
    EV_APP_START = 1,   /* once, after loading: set up timers, read the state */
    EV_APP_SHOW,        /* the app came to the front: describe the screen */
    EV_APP_HIDE,        /* it left the front (timers keep running) */
    EV_APP_BUTTON,      /* id: a button was pressed */
    EV_APP_LEVEL,       /* id, value: a level picker changed */
    EV_APP_TOGGLE,      /* id, value 0/1 */
    EV_APP_HOLD,        /* id: a hold button was held to the end */
    EV_APP_TIMER,       /* id: one of the app's timers */
    EV_APP_BACK,        /* Back (Bixby, the Back button): return 1 from ... the kernel closes the app */
    EV_APP_SAVE,        /* the phone is about to switch off: SYS_STATE_PUT now */
    EV_APP_POWER,       /* value: the power state changed (screen off / on) */
    EV_APP_SERVICE,     /* id: a service the app uses changed (heat, sos) */
};

struct sbos_event {
    uint32_t type, id;
    int32_t value, pad;
    uint64_t uptime_ms;
};

/* ---- the structures calls fill --------------------------------------------------------------- */
struct sbos_time {
    int64_t utc;                /* seconds since 1970, 0 if the clock is unknown */
    int32_t y, mo, d, h, mi, s, wday, dst;
    int32_t off_min;            /* local offset from UTC */
    char abbr[8];               /* "EET" */
    char zone[24];              /* "Helsinki" */
    int32_t approx;             /* 1.2-alpha26: 1 the clock is the last known time (the PMIC's clock was lost) */
};

struct sbos_battery {
    int32_t ok, pct, pct_x10, mv, ma, avg_ma;   /* ma: + charging, - discharging */
    int32_t temp_c10;           /* battery temperature, 0.1 C; -1000 unknown */
    int32_t charger;            /* 0 none, 1 USB, 2 wireless */
    int32_t cap_mah, full_mah;  /* remaining, full */
};

struct sbos_estimate {
    int32_t now_min;            /* at the current use */
    int32_t screen_off_min;     /* if left with the screen off */
    int32_t off_days;           /* switched off */
    int32_t learned;            /* 1 measured on this phone, 0 typical values */
    int32_t now_ma, screen_off_ma;
};

enum { HEAT_OFF = 0, HEAT_LOW, HEAT_MEDIUM, HEAT_HIGH, HEAT_EXTREME /* 0.6: everything at its rated top */,
       HEAT_MANUAL /* 0.6.1: the clocks picked per cluster, flat out as Extreme */,
       HEAT_KEEPWARM /* 0.6.2: the battery kept at a temperature target (deep cold), no other limits */,
       HEAT_SURVIVAL /* 1.1: a last resort: everything at the tables' tops, held just below the chip's own power cut,
                        until 2 % battery (triple confirmation) */,
       HEAT_SURVIVAL_X /* 1.1: Survival with the chip's hardware heat trip OFF: up to the user's SoC stop, a 5 s
                          watchdog fed only by the heat loop (quadruple confirmation) */ };
enum {
    HEAT_ST_OFF = 0, HEAT_ST_RUNNING, HEAT_ST_HOLDING, /* at the target temperature, load paused */
    HEAT_ST_PAUSED,             /* a charger is connected */
    HEAT_ST_STOPPED,            /* a hard stop: why says which */
    HEAT_ST_REFUSED,            /* could not start: why */
    HEAT_ST_CONFIRM             /* first run: the kernel asks the user before the first start */
};
struct sbos_heat {
    int32_t state, level, cores;
    int32_t bat_c10, soc_c10;   /* battery and hottest SoC temperature, 0.1 C (-1000 unknown) */
    int32_t secs_left, secs_run;
    int32_t a55_mhz, m3_mhz;
    char why[64];
    int32_t skin_c10, gpu_mhz;  /* 0.6.1: the back of the phone (-1000 unknown); the GPU's clock while it heats */
    int32_t ma, mv, pct;        /* 0.6.2: the battery's current (- discharging) and voltage while it heats; charge % */
    int32_t reserve_pct, target_c10;    /* 0.6.2: the call / SOS reserve (0 overridden); Keep warm's target */
    int32_t no_current_stop;    /* 0.6.2: Manual's 3.5 A stop turned off */
    int32_t trip_c;             /* 0.6.3: the SoC's hardware trip as decoded (C; the 0.6.2 tour saw the power cut at 128) */
    int32_t soc_stop_c10;       /* 0.6.3: Manual's SoC stop (0 off) */
    int32_t last_unclean, last_peak_soc_c10, last_secs;     /* 0.6.3: the last session ended without a stop */
    int32_t trip_off;           /* 1.1: the chip's hardware heat trip is switched off now (Survival, trip off) */
    int32_t surv_target_c10;    /* 1.1: the SoC reading Survival holds below (0.1 C) */
    int32_t surv_stop_c10;      /* 1.1: Survival (trip off)'s SoC stop (0.1 C) */
    int32_t gpu_on;             /* 1.1: the GPU's share is running */
    int32_t surv_bat_c10;       /* 1.1-beta5: Survival's battery limit (0.1 C) */
    int32_t no_high_stop;       /* 1.2-alpha17: Survival's 4.5 A stop turned off (a 6 A / 1 s backstop instead) */
    int32_t all_out;            /* 1.2-alpha23: Survival "All out" (this session's while it runs, else the setting) */
    int32_t unlimited;          /* 1.2-alpha24: Survival "Unlimited" (as all_out) */
    int32_t guard_on, guard_mv; /* 1.2-alpha24: the brownout guard and its floor (mV) */
    int32_t fast_min_mv;        /* 1.2-alpha24: this session's lowest 100 ms cell voltage (0 none) */
};

/* 1.2-alpha22 (SBOS_P_HEAT): the last heating session's log (the state slot's heat log), summed up and in 60 time
 * buckets oldest first (-32768 = no sample in that bucket): the hottest SoC and battery, the highest discharge
 * current (0.1 A), the lowest voltage (mV). samples 0: no log. */
#define SBOS_HEAT_LOG_PTS 60
struct sbos_heat_log {
    int32_t samples, secs, level, gpu_mhz, a55_mhz, m3_n;
    int32_t peak_soc_c10, peak_bat_c10, peak_ma, min_mv, peak_mw;   /* peak_ma: discharge, positive */
    int32_t unclean;                    /* the session ended without a stop (the power went) */
    int32_t points;
    char why[48];                       /* how it ended, when known */
    int16_t soc[SBOS_HEAT_LOG_PTS], bat[SBOS_HEAT_LOG_PTS], amps10[SBOS_HEAT_LOG_PTS], mv[SBOS_HEAT_LOG_PTS];
};

enum { SOS_OFF = 0, SOS_FLASH = 1 };
struct sbos_sos {
    int32_t state, secs;
    int32_t torch;              /* 1 if the LED flashes too */
    int32_t siren;              /* 1 if sound works */
};

struct sbos_sysinfo {
    char version[24];
    int32_t irq_mode;           /* 1 interrupts, 0 polling fallback */
    int32_t first_frame_ms;
    int32_t a55_mhz, m3_mhz;
    int32_t apps, uptime_s;
    int32_t screen_off_s, sleep_off_s;
};

/* Wi-Fi (0.4): open networks; secured ones are listed (their passphrase entry comes later) */
enum { SBOS_WIFI_OFF = 0, SBOS_WIFI_STARTING, SBOS_WIFI_UP, SBOS_WIFI_SCANNING, SBOS_WIFI_JOINING, SBOS_WIFI_DHCP,
       SBOS_WIFI_CONNECTED, SBOS_WIFI_FAILED };
#define SBOS_WIFI_NETS 12
struct sbos_wifi_net {
    uint8_t ssid[33], ssid_len, bssid[6];
    int32_t rssi, secure, channel, band5;
    int32_t saved;                      /* 0.4.2: the phone's saved network (store entry "wifinet"): joinable */
};
/* 1.2-alpha21: a folder's entry on the card (SYS_FILE_LIST, SBOS_P_FILES, read only) */
struct sbos_dirent {
    char name[64];                      /* UTF-8, cut to 63 bytes */
    uint64_t size;
    int32_t dir;
};

/* 1.2-alpha21: a known network (SYS_WIFI_KNOWN): the saved one (set from the computer) or one joined since the start */
struct sbos_wifi_known {
    uint8_t ssid[33], ssid_len;
    int32_t saved, heard;               /* heard: in the last scan */
};
struct sbos_wifi {
    int32_t state, nets, rssi, secs;    /* secs: in this state */
    uint8_t ssid[33], ssid_len, ip[4], gateway[4];
    int32_t ping_ms;                    /* the gateway's answer, -1 none yet */
    int32_t clock_checked;              /* 0.5.8: 0 not yet, 1 right, 2 corrected, 3 set (there was none), -1 no answer */
    int32_t clock_off_s;                /* the correction (2) */
    char why[64];                       /* the last failure, in words */
};

/* 1.2-alpha21 (SBOS_P_PHONE): the modem, receive only (docs/MODEM.md 7.6): its status, never a call, an SMS or
 * data. Started only by the user (the kernel's own confirmation), never by the self test; a SIM or a network
 * registration parks it (the guards). The operator (PLMN) is never given to apps. */
enum { SBOS_MODEM_OFF = 0, SBOS_MODEM_STARTING, SBOS_MODEM_ON, SBOS_MODEM_PARKED, SBOS_MODEM_FAILED };
struct sbos_phone {
    int32_t state, secs;                /* secs: in this state */
    int32_t sim;                        /* the CP's SIM status code, -1 not known yet */
    int32_t bars, rssi;                 /* -1 not known */
    uint32_t msgs, queries, answers, rfs;
    int32_t starts;                     /* since the boot */
    char sim_name[24], cs[40], ps[40];  /* "absent", "emergency-only (GSM)", ... */
    char why[64];                       /* the last stop or failure, in words */
    char line[112];                     /* the masked status line (no PLMN) */
};

/* settings (SYS_SETTING_GET / SET): until a restart in 0.2 */
enum { SET_PANEL_SAVE = 1, SET_PANEL_OFF = 2, SET_SOS_LED = 3, SET_SCREEN_OFF_S = 4, SET_GPU_HEAT = 5,
       SET_BRIGHTNESS = 6 /* 0.3: 0 as the boot left it, 1 low, 2 medium, 3 normal, 4 high */,
       SET_SAVED = 7 /* 0.3.1, read only: 1 settings are saved across a restart on this phone */,
       SET_AUTO_OFF_MIN = 8 /* 0.3.2: switch off after this many minutes with the screen off: 0 never, 5, 15, 60 */,
       SET_HEAT_MAN_A55 = 9, SET_HEAT_MAN_M3 = 10, SET_HEAT_MAN_GPU = 11 /* 0.6.1: Manual's ACPM steps in kHz (a table
       step; A55/M3 0 = the top normal step, GPU 0 = not used); until a restart */,
       SET_HEAT_RESERVE = 12 /* 0.6.2: the call / SOS reserve for full heat, % (0 = overridden, at most 50) */,
       SET_HEAT_TARGET_C10 = 13 /* 0.6.2: Keep warm's battery target, 0.1 C (50..350) */,
       SET_HEAT_CURRENT_STOP = 14 /* 0.6.2: Manual's 3.5 A battery-current stop: 1 on (default), 0 off; until a restart */,
       SET_HEAT_SOC_STOP_C10 = 15 /* 0.6.3: Manual's SoC stop, 0.1 C: 0 off (default) or 700..1250 */,
       SET_LED_FULL = 16 /* 0.6.3: the LED at its rated currents over I2C (torch 240 mA, SOS flash pulses 1250 mA);
                            1.0-rc3: reads 1 on, 0 off, negative = why the chip was refused (kern.h FLED_*);
                            1.1-beta2: 2 on for the torch only (the flash pulses off: the chip's flash timer unconfirmed) */,
       SET_PANEL_DUMP = 17 /* 0.6.5: write 1: read the panel's registers (read only, logged) */,
       SET_HEAT_RESERVE_ONCE = 18 /* 1.0-rc2: write 1: the next full-heat session runs without the reserve (once) */,
       SET_HEAT_SURV_STOP_C10 = 19 /* 1.1: Survival (trip off)'s SoC limit, 0.1 C: 1250..1500 (held there, 150 held at 148; default 1350) */,
       SET_HEAT_SURV_BAT_C10 = 20 /* 1.1-beta5: Survival's battery limit, 0.1 C: 550 / 580 / 600 / 650 (1.2-alpha23); above 550 the kernel asks first */,
       SET_OCP_PROBE = 21 /* 1.1-beta7: write 1: read the M3 OCP registers on one M3 core (read only, logged); reads the result:
                             0 not run, 1 readable, -1 trapped, -2 the core did not start */,
       SET_LINUX = 22 /* 1.2-alpha1: Linux apps on the big cores (a test): write 1 asks first (the kernel's
                         confirmation), then shows the home tile "Linux" until a restart; 0 hides it and stops Linux.
                         Reads 1 on, 0 off, -1 the Linux files are not in the store */,
       SET_HEAT_WHITE = 23 /* 1.2-alpha5: write 1 while Survival runs: the screen white again (Survival's heat);
                              0 the normal screen */,
       SET_HEAT_HIGH_STOP = 25 /* 1.2-alpha17: Survival's 4.5 A battery-current stop: 0 asks once, then off (a 6 A over
                                   1 s backstop stays); 1 on again. Remembered. Reads 1 on, 0 off */,
       SET_HEAT_SCREEN_WHITE = 26 /* 1.2-alpha19: Survival's screen white at the panel's top level (1) or dark with the
                                      readings (0, the default: the panel's ~1 W goes to the chips). Remembered */,
       SET_WIFI_AUTOJOIN = 27 /* 1.2-alpha21: join a known network the scan heard, once per power-on (1, the default)
                                  or never (0). Remembered */,
       SET_HEAT_UNLIMITED = 29 /* 1.2-alpha24: Survival "Unlimited" (0 the default): the trip off, All out, the 3.5 / 4.5 A
                                  switches off (B2SOVRC off), no SoC stop; 1 asks first (the kernel's confirmation).
                                  Kept: the battery limit, 2 %, the 6 / 6.5 A for 1 s backstop. Remembered */,
       SET_HEAT_GUARD = 30 /* 1.2-alpha24: the brownout guard on (1, the default) or off (0). Remembered */,
       SET_HEAT_GUARD_MV = 31 /* 1.2-alpha24: the guard's floor: 3200, 3350 (the default) or 3500 mV. Remembered */,
       SET_HEAT_ALL_OUT = 28 /* 1.2-alpha23: Survival "All out" (1, the default): no hold at a temperature limit and no
                                current / voltage throttle (a brownout may switch the phone off); 0 holds as before.
                                Remembered */,
       SET_VIBRATE = 24 /* 1.2-alpha7: write (count << 16) | ms: count pulses (0 = 1) of ms (<= 5000) with ms gaps; 0 stops.
                           Reads 1 when the motor works, 0 not tried yet, -1 refused */,
       /* 1.2-alpha27: the survival tools (kern/survival.h) */
       SET_SURV_USB = 32 /* write 1 after the app's hold-to-arm: one burst of 5 V out of the USB-C port (at most 10 s);
                            0 stops it. Reads 0 off, 1 on, 2 cooling down, -1 the last start was refused (the kernel
                            posts the reason as a notification) */,
       SET_SURV_USB_INFO = 33 /* read: seconds left (burst or cooldown) | the estimated output mA << 8 (0xfff unknown)
                                 | the VBUS range's top in V << 20 (0xf unknown) | the battery C << 24 */,
       SET_SURV_SOS = 34 /* SOS's options: 1 the torch in Morse, 2 the motor in Morse; beacon << 4: 0 white / red,
                            1 white, 2 red, 3 with the Morse. Remembered */,
       SET_SURV_LAT = 35, SET_SURV_LON = 36 /* the location for the sun, 0.01 degrees (N / E positive); 0x7fffffff
                                               none. Remembered */,
       SET_SURV_SUN = 37 /* read: sunrise | sunset << 16 in local minutes; -1 the sun stays up, -2 it stays down,
                            -3 no location, -4 no clock */,
       SET_SURV_LAST2 = 38 /* Last 2 % mode: 1 on, 0 off */,
       SET_SURV_LAST2_AUTO = 39 /* Last 2 % at 3 % instead of switching off: 1 (the default) or 0. Remembered */,
       SET_SURV_GNSS = 40 /* 1.2-alpha28: read: the age in minutes of the GPS fix Android left on the card
                             (/STARBITE/GNSS/LASTFIX.TXT), -1 none, -2 no card; write 1: use it as the location */,
       SET_SURV_SENSORS = 41 /* write 1: start the sensor hub or keep it running (stops 60 s after the last 1), 0: stop.
                                Reads 0 off, 1 starting, 2 running, 3 failed (the reason as a notification) */,
       SET_SURV_HEADING = 42 /* read: the heading in 0.1 degrees from magnetic north | the calibration (0..3) << 16;
                                -1 not valid now */,
       SET_SURV_TILT = 43 /* read: (pitch + 1000) | (roll + 1000) << 16, 0.1 degrees; -1 not valid now */,
       SET_SURV_STEPS = 44 /* read: steps since the hub started; -1 not running */,
       /* 1.2-alpha29 (Track D) */
       SET_SURV_SAVER = 45 /* Extreme power saving: 1 on, 0 off (reads 1 while it applies, by hand or Last 2 %) */,
       SET_SURV_EST = 46 /* read: minutes left to 1 % with the screen off as usual; SET_SURV_EST_ON / _SAVER / _HEAT:
                            the screen on, Extreme power saving, Survival heat; -1 unknown or charging */,
       SET_SURV_EST_ON = 47, SET_SURV_EST_SAVER = 48 /* | 1 << 30 when measured on this phone */, SET_SURV_EST_HEAT = 49,
       SET_SURV_LIGHT = 50 /* write 1: the panel at its top level (the app draws white with ui_light), 0 back */,
       SET_SURV_WHISTLE = 51 /* write 1 / 0: the whistle tone; reads -1 while there is no speaker driver */,
       SET_SURV_FIRE = 52 /* Extreme fire mode: write 1 to start (after the app's confirmation and hold), 2 to keep it
                             running (at least every 1.5 s), 0 to stop. Reads as SET_SURV_USB while a fire session runs,
                             else 0 */,
       SET_SURV_FIRE_INFO = 53 /* read: the chip's limit events (10 bits) | its own switch-offs << 10 (6 bits) | the
                                  minutes left << 16 (0x7fff unknown) */,
       SET_SURV_USB_ILIM = 54 /* read: the chip's OTG current limit as read back at the start, mA (-1 unknown) */,
       /* 1.2-alpha30: the fire starter (kern/survival.h surv_fs_start; docs/USB-FIRE-STARTER.md) */
       SET_SURV_FS = 55 /* write after the app's arming hold, while the finger is STILL down: the pulse (0 0.5 s,
                           1 1 s, 2 2 s) | repeat << 4 | the limit (1 0.9 A, 2 1.2 A, 3 1.5 A) << 8 | 1 << 12
                           | S9 #2's override << 16 (0 off, 1 fast re-arm, 2 + VBYPSET; pulses <= 1 s) | the
                           VBYPSET code << 20 (7 bits, override 2 only); the
                           pulses run only while the finger stays on the screen. 0 stops. Reads as SET_SURV_USB while
                           a fire-starter hold runs (or cools down), else 0 */,
       SET_SURV_FS_INFO = 56 /* read: pulses done (4 bits) | the most (4 bits) << 4 | output on now << 8 | the
                                SURV_FS_WHY_* of the last end << 9 (5 bits) | the time left of this pulse or rest,
                                0.1 s, << 14 (8 bits) | the battery 0.1 C << 22 (10 bits, 1023 unknown) */,
       SET_SURV_FS_LAST = 57 /* read: the last pulse: its mean mA at the port (12 bits, 0xfff unknown) | its peak mA
                                << 12 (12 bits) | its lowest VBUS code << 24 (4 bits: 0 below 3.5 V, 2 about 5 V,
                                0xf none) */,
       SET_SURV_FS_W10 = 58 /* read: the last pulse's about W at the port x10 | its lowest battery mV << 8 */,
       SET_SURV_FS_KEYS = 59 /* write the fire page's hold id (refresh at least every 2 s; 0 ends it): Power short =
                                Select, Power held 1 s = that hold (EV_APP_HOLD), the output only while Power stays
                                down; no screen-off / power menu from Power meanwhile */,
       SET_SURV_FS_VBYP = 60 /* read: the last pulse's mean boost output (the gauge's VBYP) mV | 0.1 W from the cell
                                << 16; SET_SURV_FS's pulse codes 3 = 3 s, 4 = 5 s (no override); SET_SURV_FS
                                bit 13: the steel-wool preset (the chip's own retry let run, no override) */,
       SET_SURV_FS_SPLIT = 61 /* read: the last pulse's 0.1 W into the load | 0.1 W lost in the chip's switch << 16 */,
       SET_SURV_FS_LIVE = 62 /* read: re-arms in this hold (14 bits) | seconds on << 14 (10 bits) | the boost's
                                VBYPSET code << 24 (6 bits) | the load open (under 50 mA) << 30; override 3 = MAX
                                (continuous, no cooldown; also the heater-wire mode: steady while held) */,
       SET_SURV_FS_OHM = 63 /* read: the last window's load, 0.1 ohm (VBYP / I with no limit event); -1 unknown,
                               -2 at the chip's limit (under ~3.4 ohm: lengthen the coil) */ };
/* SET_SURV_FS_INFO's reason codes (kern/survival.h SURV_FS_WHY_*) */
enum { FS_WHY_NONE, FS_WHY_DONE, FS_WHY_RELEASED, FS_WHY_ILIM, FS_WHY_VBUS, FS_WHY_FAULT, FS_WHY_HOT, FS_WHY_LOWBAT,
       FS_WHY_STOP, FS_WHY_REFUSED, FS_WHY_COOL, FS_WHY_FINGER };

/* 1.0-rc3: the full-heat battery data for the minutes-left estimates (Heat's confirmation and its live readout).
 * Measured on the phone (rc2, 2026-11-08, Manual flat out, 7 cores + GPU): 2.8 A at 3.67 V (about 10 W; peak 2.86 A),
 * and the fuel gauge fell 100 -> 91 % in 183 s and to 90 % at the hardware cut: about 3 % a minute. In coulombs that is
 * only about 5 % of 3000 mAh, so the gauge's percent under this load is worth about 16 mAh, not 30: the estimates use
 * 16 (half what the 0.6.2 estimate assumed, so they no longer promise twice the time). The 0.6.2 tour (Extreme, at the
 * caps) drew the same ~10 W. One run from a full charge: the gauge may fall faster near 100 %; more runs refine it. */
/* 1.1-beta7: the ABSOLUTE FINAL limits (docs/STARBITE-OS.md 8.0 "Hard limits"): the SoC limit setting is at most
 * 150 C, the battery limit at most 60 C; nothing in the code may set, hold or regulate above them (unit-tested). The
 * user asked for 175, then 155 C; 150 C was agreed on 2026-11-08 with the main session. A SoC reading of 150 C or
 * more counts as invalid (the lost-reading stop), so the hold itself never targets above 148 C (HOLD_MAX) and the
 * stop past it is at most 149 C. */
#define SBOS_HEAT_SOC_HARD_C10 1500
#define SBOS_HEAT_SOC_HOLD_MAX_C10 1480
/* 1.2-alpha4 (the user, 2026-11-09): Survival (trip off) "No limit", behind 8 holds: nothing of ours holds or stops
 * the SoC by temperature; the battery limit (55/58/60/65 C) and 2 % still stop. A reading is trusted up to 199 C (the
 * TMU is trimmed at 25 and 85 C; solder melts near 217 C): above it the reading counts as lost and Survival stops */
#define SBOS_HEAT_SOC_NOLIMIT_C10 9990
#define SBOS_TEMP_READ_MAX_C 200
#define SBOS_HEAT_BAT_HARD_C10 650     /* 1.2-alpha23 (the user, 2026-11-10): 65 C, was 60; never higher */
#define SBOS_HEAT_FULL_MA 2800
#define SBOS_HEAT_MAH_PER_PCT 16

/* sensor_read ids (0.3): temperatures in 0.1 C, -1000 unknown */
enum { SENSOR_BATTERY_C10 = 1, SENSOR_SKIN_C10 = 2, SENSOR_SOC_C10 = 3, SENSOR_GPU_C10 = 4 };

/* power_request what */
enum { PWR_SCREEN_OFF = 1, PWR_SET_SCREEN_OFF_S = 2, PWR_SET_SLEEP_OFF_S = 3 };

/* UI flags */
#define UI_HALF   1U            /* button: half width (two per row) */
#define UI_RED    2U
#define UI_ACCENT 4U
#define UI_DIM    8U            /* greyed, not selectable */

#define SBOS_COLOR_WHITE 0xffffffffU
#define SBOS_COLOR_WARM  0xffffe2b8U
#define SBOS_COLOR_RED   0xffff2020U

/* ---- the calls -------------------------------------------------------------------------------- */
int64_t sbos_call(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);

#ifndef SBOS_KERNEL    /* the kernel has its own functions of these names */

static inline void sb_log_app(const char *s) { sbos_call(SYS_LOG, (uintptr_t)s, 0, 0, 0, 0); }
static inline void *sb_alloc_app(uint64_t n) { return (void *)(uintptr_t)sbos_call(SYS_ALLOC, n, 0, 0, 0, 0); }
static inline void sb_exit_app(void) { sbos_call(SYS_EXIT, 0, 0, 0, 0, 0); }
/* another app to the front by its id (the caller must be in front): 0, or SBOS_EINVAL (no such app) */
static inline int app_open(const char *id) { return (int)sbos_call(SYS_APP_OPEN, (uintptr_t)id, 0, 0, 0, 0); }

static inline void ui_begin(void) { sbos_call(SYS_UI_BEGIN, 0, 0, 0, 0, 0); }
static inline void ui_title(const char *t) { sbos_call(SYS_UI_TITLE, (uintptr_t)t, 0, 0, 0, 0); }
static inline void ui_button(uint32_t id, const char *label, const char *sub, uint32_t flags)
{
    sbos_call(SYS_UI_BUTTON, id, (uintptr_t)label, (uintptr_t)sub, flags, 0);
}
static inline void ui_big(const char *caption, const char *value, uint32_t argb)
{
    sbos_call(SYS_UI_BIG, (uintptr_t)caption, (uintptr_t)value, argb, 0, 0);
}
static inline void ui_textline(const char *t, uint32_t argb) { sbos_call(SYS_UI_TEXT, (uintptr_t)t, argb, 0, 0, 0); }
/* n labels separated by '|', e.g. "Off|Low|Medium|High" */
static inline void ui_level(uint32_t id, const char *labels, int current)
{
    sbos_call(SYS_UI_LEVEL, id, (uintptr_t)labels, (uint64_t)(int64_t)current, 0, 0);
}
static inline void ui_toggle(uint32_t id, const char *label, int on) { sbos_call(SYS_UI_TOGGLE, id, (uintptr_t)label, (uint64_t)on, 0, 0); }
static inline void ui_hold(uint32_t id, const char *label, uint32_t ms, uint32_t flags)
{
    sbos_call(SYS_UI_HOLD, id, (uintptr_t)label, ms, flags, 0);
}
static inline void ui_end(void) { sbos_call(SYS_UI_END, 0, 0, 0, 0, 0); }
static inline void ui_toast_app(const char *t, uint32_t ms) { sbos_call(SYS_UI_TOAST, (uintptr_t)t, ms, 0, 0, 0); }
/* A full-screen colour field (0 = off). Any touch or key ends it (EV_APP_BACK is not sent: the app
 * gets EV_APP_SHOW again). */
static inline void ui_light(uint32_t argb) { sbos_call(SYS_UI_LIGHT, argb, 0, 0, 0, 0); }
static inline void ui_awake(int on) { sbos_call(SYS_UI_AWAKE, (uint64_t)on, 0, 0, 0, 0); }
/* A text field with the on-screen keyboard (0.5): id (not 0) names it; flags 1 = secret (dots, with Show),
 * 2 = the action key says "Join" (else "Done"), 4 = start with a capital (names); max
 * characters (1..64). The keyboard's action key sends EV_APP_BUTTON with this id (value: the length). The text
 * stays in the kernel: ui_input_get copies it (and it is wiped when the app leaves the front). */
static inline void ui_input(uint32_t id, const char *label, uint32_t flags, int max)
{
    sbos_call(SYS_UI_INPUT, id, (uintptr_t)label, flags, (uint64_t)max, 0);
}
/* the same, starting from init when the field is new (editing a saved value) */
static inline void ui_input_init(uint32_t id, const char *label, uint32_t flags, int max, const char *init)
{
    sbos_call(SYS_UI_INPUT, id, (uintptr_t)label, flags, (uint64_t)max, (uintptr_t)init);
}
static inline int ui_input_get(char *buf, uint32_t cap) { return (int)sbos_call(SYS_UI_INPUT_GET, (uintptr_t)buf, cap, 0, 0, 0); }
/* A line chart (0.5.6): n values (at most 60, oldest first) between lo and hi, drawn in argb; caption above, the
 * left and right labels under it (e.g. "8 h ago", "now"). A value of -32768 is a gap. */
static inline void ui_chart(const char *caption, const int16_t *v, int n, int lo, int hi, uint32_t argb,
                            const char *left_right)
{
    struct { const int16_t *v; int32_t n, lo, hi; uint32_t argb; const char *lr; } c = { v, n, lo, hi, argb, left_right };
    sbos_call(SYS_UI_CHART, (uintptr_t)caption, (uintptr_t)&c, 0, 0, 0);
}
/* the battery's charge over the last hours: one value (0.1 %) every 10 minutes, oldest first, -32768 unknown;
 * returns the count (at most cap) */
static inline int battery_history(int16_t *v, int cap) { return (int)sbos_call(SYS_BATTERY_HISTORY, (uintptr_t)v, (uint64_t)cap, 0, 0, 0); }
/* the short line under the app's tile on home (any time, front or not; "" clears it) */
static inline void ui_tile(const char *line) { sbos_call(SYS_UI_TILE, (uintptr_t)line, 0, 0, 0, 0); }
/* a ring gauge: permille 0..1000 of the ring in argb, value inside, caption under it */
static inline void ui_gauge(const char *caption, const char *value, int permille, uint32_t argb)
{
    sbos_call(SYS_UI_GAUGE, (uintptr_t)caption, (uintptr_t)value, (uint64_t)(int64_t)permille, argb, 0);
}

static inline int time_now(struct sbos_time *t) { return (int)sbos_call(SYS_TIME_NOW, (uintptr_t)t, 0, 0, 0, 0); }
static inline int timer_set(uint32_t id, uint32_t ms, int repeat) { return (int)sbos_call(SYS_TIMER_SET, id, ms, (uint64_t)repeat, 0, 0); }
static inline void timer_cancel(uint32_t id) { sbos_call(SYS_TIMER_CANCEL, id, 0, 0, 0, 0); }
static inline uint64_t uptime_ms(void) { return (uint64_t)sbos_call(SYS_UPTIME_MS, 0, 0, 0, 0, 0); }

static inline int battery(struct sbos_battery *b) { return (int)sbos_call(SYS_BATTERY, (uintptr_t)b, 0, 0, 0, 0); }
static inline int estimate(struct sbos_estimate *e) { return (int)sbos_call(SYS_ESTIMATE, (uintptr_t)e, 0, 0, 0, 0); }
static inline int sysinfo(struct sbos_sysinfo *s) { return (int)sbos_call(SYS_SYSINFO, (uintptr_t)s, 0, 0, 0, 0); }
static inline int power_request(int what, int value) { return (int)sbos_call(SYS_POWER_REQUEST, (uint64_t)what, (uint64_t)value, 0, 0, 0); }

/* torch(1..3) on, torch(0) off, torch(-1): 1 on / 0 off; SBOS_EAGAIN refused (no safe LED), SBOS_ENOSYS none */
static inline int torch(int level) { return (int)sbos_call(SYS_TORCH, (uint64_t)(int64_t)level, 0, 0, 0, 0); }
static inline int setting_get(int id) { return (int)sbos_call(SYS_SETTING_GET, (uint64_t)id, 0, 0, 0, 0); }
static inline int setting_set(int id, int v) { return (int)sbos_call(SYS_SETTING_SET, (uint64_t)id, (uint64_t)(int64_t)v, 0, 0, 0); }
/* the start of a file on the card (read only): bytes read, or < 0 */
static inline int file_read(const char *path, void *buf, uint32_t cap) { return (int)sbos_call(SYS_FILE_READ, (uintptr_t)path, (uintptr_t)buf, cap, 0, 0); }
/* 1.2-alpha21: a folder's entries from first (path "/" the card's root): how many were stored; *total all of them */
static inline int file_list(const char *path, struct sbos_dirent *d, int max, int first, int *total)
{
    int64_t r = sbos_call(SYS_FILE_LIST, (uintptr_t)path, (uintptr_t)d, (uint64_t)max, (uint64_t)first, 0);
    if (total)
        *total = r < 0 ? 0 : (int)(r >> 16);
    return r < 0 ? (int)r : (int)(r & 0xffff);
}
static inline int heat_log_get(struct sbos_heat_log *l) { return (int)sbos_call(SYS_HEAT_LOG, (uintptr_t)l, 0, 0, 0, 0); }
static inline int wifi_state(struct sbos_wifi *s) { return (int)sbos_call(SYS_NET_STATE, (uintptr_t)s, 0, 0, 0, 0); }
static inline int wifi_power(int on) { return (int)sbos_call(SYS_WIFI_POWER, (uint64_t)on, 0, 0, 0, 0); }
/* 1.2-alpha21: the modem's status; on 1 asks the kernel to start it (its own confirmation screen first), 0 stops it */
static inline int phone_state(struct sbos_phone *s) { return (int)sbos_call(SYS_PHONE_STATE, (uintptr_t)s, 0, 0, 0, 0); }
static inline int modem_power(int on) { return (int)sbos_call(SYS_MODEM_POWER, (uint64_t)on, 0, 0, 0, 0); }
/* 1.2-alpha21: the known networks (names only); forget one joined since the start (not the saved one) */
static inline int wifi_known(struct sbos_wifi_known *k, int max) { return (int)sbos_call(SYS_WIFI_KNOWN, (uintptr_t)k, (uint64_t)max, 0, 0, 0); }
static inline int wifi_forget(int i) { return (int)sbos_call(SYS_WIFI_FORGET, (uint64_t)i, 0, 0, 0, 0); }
static inline int wifi_scan(void) { return (int)sbos_call(SYS_WIFI_SCAN, 0, 0, 0, 0, 0); }
static inline int wifi_list(struct sbos_wifi_net *n, int max) { return (int)sbos_call(SYS_WIFI_LIST, (uintptr_t)n, (uint64_t)max, 0, 0, 0); }
/* pass: NULL for an open or the saved network, else the WPA2 passphrase (8..63 characters, or 64 hex) */
static inline int wifi_join(int i, const char *pass) { return (int)sbos_call(SYS_WIFI_JOIN, (uint64_t)i, (uintptr_t)pass, 0, 0, 0); }
static inline int sensor_read(int id) { return (int)sbos_call(SYS_SENSOR_READ, (uint64_t)id, 0, 0, 0, 0); }
static inline int heat_request(int level, int minutes) { return (int)sbos_call(SYS_HEAT_REQUEST, (uint64_t)level, (uint64_t)minutes, 0, 0, 0); }
static inline int heat_status(struct sbos_heat *h) { return (int)sbos_call(SYS_HEAT_STATUS, (uintptr_t)h, 0, 0, 0, 0); }
static inline int sos(int on) { return (int)sbos_call(SYS_SIREN, (uint64_t)on, 0, 0, 0, 0); }
/* 1.2-alpha8 (SBOS_P_ALARM): the motor; a wake-up alarm (index 0..3, days bit 0 Sunday .. bit 6, 0 once); stop or
 * snooze the ringing one. The ringing alarm comes as EV_APP_SERVICE id 3 (value: index | h << 8 | m << 16), its end
 * as id 4 (value 1 snoozed). */
/* 1.2-alpha9: a notification in the control center's list (the status bar shows a dot until it is seen) */
static inline int notify(const char *title, const char *text) { return (int)sbos_call(SYS_NOTIFY, (uintptr_t)title, (uintptr_t)text, 0, 0, 0); }
static inline int vibrate(unsigned ms, unsigned count) { return (int)sbos_call(SYS_VIBRATE, ms, count, 0, 0, 0); }
static inline int alarm_set(int i, int h, int m, int days, int on)
{ return (int)sbos_call(SYS_ALARM_SET, (uint64_t)i, (uint64_t)(h << 8 | m), (uint64_t)days, (uint64_t)on, 0); }
static inline int alarm_cancel(int snooze) { return (int)sbos_call(SYS_ALARM_CANCEL, (uint64_t)snooze, 0, 0, 0, 0); }
static inline int sos_status(struct sbos_sos *s) { return (int)sbos_call(SYS_SOS_STATUS, (uintptr_t)s, 0, 0, 0, 0); }
static inline int state_put(const void *p, uint32_t n) { return (int)sbos_call(SYS_STATE_PUT, (uintptr_t)p, n, 0, 0, 0); }
static inline int state_get(void *p, uint32_t cap) { return (int)sbos_call(SYS_STATE_GET, (uintptr_t)p, cap, 0, 0, 0); }
#endif

/* ---- what an app defines -------------------------------------------------------------------- */
void app_event(const struct sbos_event *ev);

/* The app's header (docs/STARBITE-OS.md 6.1): one per app, filled in by mk-sba.py after linking. */
struct sbapp_header {
    char magic[8];              /* "SBAPP001" */
    uint32_t api, header_size;
    char id[24], name[24];
    uint32_t permissions;
    uint32_t entry;             /* offset of the entry stub from the file's start */
    uint32_t file_size;         /* bytes of the file (header included) */
    uint32_t bss_size;          /* zeroed after the file */
    uint32_t rela_off, rela_count;
    uint32_t icon;              /* 0 or an SB7 icon id of the system */
    uint32_t stack_size;        /* 0 = 64 KiB */
    uint32_t crc32;             /* of the whole file with this field zero */
    uint32_t color;             /* the tile's accent */
    uint8_t zero[256 - 0x68];
};

_Static_assert(sizeof(struct sbapp_header) == 256, "app header is 256 bytes");

#define SBAPP_MAGIC "SBAPP001"
/* icon_: an icon id of the system's assets (200 light, 201 heat, 202 sos, 203 clock, 204 battery, 205 settings) */
#define SBAPP_DEFINE(id_, name_, perms_, color_, icon_)                                                  \
    __attribute__((section(".sbapp.head"), used)) const struct sbapp_header sbapp_header = {             \
        SBAPP_MAGIC, SBOS_API, 256, id_, name_, perms_, 0, 0, 0, 0, 0, icon_, 0, 0, color_, { 0 } }
