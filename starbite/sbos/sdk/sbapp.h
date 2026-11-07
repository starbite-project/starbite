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
    /* UI 10..29 */
    SYS_UI_BEGIN = 10, SYS_UI_TITLE = 11, SYS_UI_BUTTON = 12, SYS_UI_BIG = 13, SYS_UI_TEXT = 14,
    SYS_UI_LEVEL = 15, SYS_UI_TOGGLE = 16, SYS_UI_HOLD = 17, SYS_UI_END = 18, SYS_UI_TOAST = 19,
    SYS_UI_LIGHT = 20, SYS_UI_AWAKE = 21,
    SYS_UI_TILE = 22, SYS_UI_GAUGE = 23,                   /* API 1, added in 0.3 */
    SYS_UI_INPUT = 24, SYS_UI_INPUT_GET = 25,              /* 0.5: a text field and the keyboard */
    /* time 30..39 */
    SYS_TIME_NOW = 30, SYS_TIMER_SET = 31, SYS_TIMER_CANCEL = 32, SYS_UPTIME_MS = 33,
    /* power 40..49 */
    SYS_BATTERY = 40, SYS_ESTIMATE = 41, SYS_POWER_REQUEST = 42, SYS_SYSINFO = 43,
    SYS_SETTING_GET = 44, SYS_SETTING_SET = 45,            /* API 1, added in 0.2 */
    /* light 50..59 */
    SYS_TORCH = 50, SYS_STROBE = 51,
    /* heat 60..69 */
    SYS_HEAT_REQUEST = 60, SYS_HEAT_STATUS = 61,
    /* alarm and sound 70..79 */
    SYS_SIREN = 70, SYS_VIBRATE = 71, SYS_ALARM_SET = 72, SYS_ALARM_CANCEL = 73, SYS_SOS_STATUS = 74,
    /* state 80..89 */
    SYS_STATE_PUT = 80, SYS_STATE_GET = 81,
    /* files 90..99 (read only), sensors 100..119, phone 120..139, network 140..159, camera 160..179:
     * reserved, SBOS_ENOSYS in API 1 */
    SYS_FILE_OPEN = 90, SYS_FILE_READ = 91,                 /* 91: added in 0.2 */
    SYS_SENSOR_READ = 100,                                  /* 0.3: temperatures */
    SYS_PHONE_STATE = 120, SYS_CALL_DIAL = 121, SYS_CALL_HANGUP = 122, SYS_SMS_SEND = 123, SYS_SMS_READ = 124,
    SYS_NET_STATE = 140,                                    /* 0.4: Wi-Fi's state (struct sbos_wifi) */
    SYS_WIFI_POWER = 141, SYS_WIFI_SCAN = 142, SYS_WIFI_LIST = 143, SYS_WIFI_JOIN = 144,   /* 0.4 */
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

enum { HEAT_OFF = 0, HEAT_LOW, HEAT_MEDIUM, HEAT_HIGH };
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
struct sbos_wifi {
    int32_t state, nets, rssi, secs;    /* secs: in this state */
    uint8_t ssid[33], ssid_len, ip[4], gateway[4];
    int32_t ping_ms;                    /* the gateway's answer, -1 none yet */
    char why[64];                       /* the last failure, in words */
};

/* settings (SYS_SETTING_GET / SET): until a restart in 0.2 */
enum { SET_PANEL_SAVE = 1, SET_PANEL_OFF = 2, SET_SOS_LED = 3, SET_SCREEN_OFF_S = 4, SET_GPU_HEAT = 5,
       SET_BRIGHTNESS = 6 /* 0.3: 0 as the boot left it, 1 low, 2 medium, 3 normal, 4 high */,
       SET_SAVED = 7 /* 0.3.1, read only: 1 settings are saved across a restart on this phone */,
       SET_AUTO_OFF_MIN = 8 /* 0.3.2: switch off after this many minutes with the screen off: 0 never, 5, 15, 60 */ };

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
/* A text field with the on-screen keyboard (0.5): id (not 0) names it; flags 1 = secret (dots, with Show); max
 * characters (1..64). The keyboard's action key sends EV_APP_BUTTON with this id (value: the length). The text
 * stays in the kernel: ui_input_get copies it (and it is wiped when the app leaves the front). */
static inline void ui_input(uint32_t id, const char *label, uint32_t flags, int max)
{
    sbos_call(SYS_UI_INPUT, id, (uintptr_t)label, flags, (uint64_t)max, 0);
}
static inline int ui_input_get(char *buf, uint32_t cap) { return (int)sbos_call(SYS_UI_INPUT_GET, (uintptr_t)buf, cap, 0, 0, 0); }
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
static inline int wifi_state(struct sbos_wifi *s) { return (int)sbos_call(SYS_NET_STATE, (uintptr_t)s, 0, 0, 0, 0); }
static inline int wifi_power(int on) { return (int)sbos_call(SYS_WIFI_POWER, (uint64_t)on, 0, 0, 0, 0); }
static inline int wifi_scan(void) { return (int)sbos_call(SYS_WIFI_SCAN, 0, 0, 0, 0, 0); }
static inline int wifi_list(struct sbos_wifi_net *n, int max) { return (int)sbos_call(SYS_WIFI_LIST, (uintptr_t)n, (uint64_t)max, 0, 0, 0); }
/* pass: NULL for an open or the saved network, else the WPA2 passphrase (8..63 characters, or 64 hex) */
static inline int wifi_join(int i, const char *pass) { return (int)sbos_call(SYS_WIFI_JOIN, (uint64_t)i, (uintptr_t)pass, 0, 0, 0); }
static inline int sensor_read(int id) { return (int)sbos_call(SYS_SENSOR_READ, (uint64_t)id, 0, 0, 0, 0); }
static inline int heat_request(int level, int minutes) { return (int)sbos_call(SYS_HEAT_REQUEST, (uint64_t)level, (uint64_t)minutes, 0, 0, 0); }
static inline int heat_status(struct sbos_heat *h) { return (int)sbos_call(SYS_HEAT_STATUS, (uintptr_t)h, 0, 0, 0, 0); }
static inline int sos(int on) { return (int)sbos_call(SYS_SIREN, (uint64_t)on, 0, 0, 0, 0); }
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
