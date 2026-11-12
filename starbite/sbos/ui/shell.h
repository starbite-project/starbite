/* SPDX-License-Identifier: MIT
 * The Starbite OS shell on sbtk (P1: the lock screen, its Systems flows and the unlock to a placeholder home),
 * after the approved concept (docs/concepts/starbite-os-shell/index.html). Coordinates in concept pt. */
#ifndef SB_SHELL_H
#define SB_SHELL_H
#include "sbtk.h"

int sh_init(tk_comp *c);                  /* builds the wallpaper, the lock, the home; needs tk_pack_open first */
void sh_init_begin(tk_comp *c);           /* the same in steps: begin, then sh_init_step() until it returns 0 */
int sh_init_step(void);
/* deterministic states, as the concept's sbSet(): "unlock" v; "group" key ("" = none); "focus" key with p;
 * "scroll" v (the list's scroll in pt); "hold" which (0 torch, 1 Emergency) with p (0..1, the ring) */
void sh_set(const char *what, const char *key, float v);
/* touch: phase 0 down, 1 move, 2 up; t in ms */
void sh_touch(int phase, float x, float y, float t_ms);
/* advance time by dt s (the springs, holds, transitions); returns 1 while anything moves */
int sh_tick(float dt, float t_ms);
int sh_moving(void);
void sh_idle(void);
int sh_idle_pending(void);                /* idle work left (call sh_idle again soon) */
/* the live values: local time (wday 0 = Sunday, mon 1..12), battery %, temperature C; rebuilds what changed */
void sh_status(int h, int mi, int wday, int day, int mon, int pct, int temp);                       /* idle time: prepare the caches the next gesture needs */

/* provided by the platform: slide to start finished (the kernel: the loader's one-shot next boot) */
void sh_boot_os(const char *key);
/* provided by the platform: the torch hold toggled the flashlight */
void sh_torch(int lit);
/* the status bar's left text from the modem's status (default "No SIM") */
void sh_carrier(const char *text);
/* the modem's status for the UI: the fields of the modem session's snapshot that the shell shows (docs/MODEM.md
 * 7.7, struct sbm_modem_snap): state, SIM status (0x80 absent, -1 not known), emergency-only, RAT (1 GSM .. 7 LTE,
 * 0 unknown), bars 0..5 (-1 unknown). Never the PLMN or a cell id: nothing here is logged. Redraws only on a change. */
enum { SH_CELL_OFF, SH_CELL_ON, SH_CELL_RESTARTING, SH_CELL_GUARD, SH_CELL_FAILED, SH_CELL_STARTING };
/* mobile data (Settings > Mobile Data / the Control Centre's cellular circle: the user's switch, off at every boot):
 * data = SH_DATA_* (the kernel's KCELL_* order), oper the operator's name (screen only) */
enum { SH_DATA_OFF, SH_DATA_STARTING, SH_DATA_NO_SIM, SH_DATA_LOCKED, SH_DATA_SEARCHING, SH_DATA_REGISTERED,
       SH_DATA_UP, SH_DATA_STOPPING, SH_DATA_FAILED };
typedef struct { int state, sim, emergency_only, rat, bars, data; char oper[24]; char why[48]; } sh_cell_t;   /* why: a failure */
void sh_cell(const sh_cell_t *c);
int sh_cellular(int on);                /* the switch: 1 on (radio, registration, data), 0 off (detach, park) */
/* the platform's modem power (the kernel: kmodem_power: 1 asks for its hold confirmation, 0 stops); < 0 refused */
int sh_modem(int on);
/* a hold confirmation drawn by the shell (the kernel's kui_confirm): the answer goes to the platform's
 * sh_confirmed(1) on a full hold, sh_confirmed(0) on a tap outside the card */
void sh_confirm(const char *title, const char *l1, const char *l2, const char *l3, const char *hold);
void sh_confirmed(int ok);
/* P2r, sleep and wake: the screen off as a fade to black (now: black at once), on as the lock screen from black;
 * the shell calls sh_slept (the platform) when the fade has ended: the panel off after that frame */
void sh_sleep(int now);
void sh_wake(void);
int sh_sleeping(void);
void sh_slept(void);
/* P2r: the Control Centre's brightness slider (the panel's levels 1 dim .. 4 bright), and the level at start */
void sh_brightness(int level);
void sh_bright_now(int level);
/* P2y: the apps' live values besides sh_status: charging or not, the software's name and version */
void sh_sysinfo(int charging, const char *version);
void sh_alarm(int on, int h, int m);
/* B5: the Clock's alarm list to the kernel (alarm.c's alarm_set: slot i of 4, days a bit per weekday, 0 once); < 0
 * refused (the self test) */
int sh_alarm_set(int i, int h, int m, int days, int on);
/* P2z, the Clock app: when to come back (ms; < 0 nothing pending), the redraw then (1: something changed), and the
 * platform's vibration for a timer that ran out (no sound) */
int sh_next_update(float t_ms);
int sh_refresh(float t_ms);
void sh_vibrate(void);
void sh_year(int y);
void sh_haptic(void);                     /* P3h: a key's light tap (the vibration motor, ~10 ms) */
/* B3: the clipboard of the text fields' edit menu (Copy / Cut / Paste): one UTF-8 text, shared with the Linux apps
 * (defined in shell_linux.inc: Track E's, offered to Linux). sh_clip_get: "" when empty. */
void sh_clip_set(const char *utf8);
const char *sh_clip_get(void);
/* B4: iOS's feedback generators as motor pulses (docs/design/IOS-REFERENCE.md 17; lengths to tune on the phone) */
enum { SH_HAP_LIGHT, SH_HAP_MEDIUM, SH_HAP_HEAVY, SH_HAP_SOFT, SH_HAP_RIGID, SH_HAP_SELECT, SH_HAP_SUCCESS, SH_HAP_WARNING,
       SH_HAP_ERROR };
void sh_haptic_k(int kind);
/* B4: the app switcher's external entries (one per running Linux app) are shell-internal since P3x: shell_linux.inc
 * (Track E's v3, SH_EXT_API) is the provider and calls the switcher's static sw_ext_add / sw_ext_remove */
/* wifi-sbos-next: Wi-Fi for Settings > Wi-Fi, the status bar and the Control Centre. The platform pushes its state
 * with sh_wifi (only on a change); the shell asks with sh_wifi_power / _scan / _join / _fetch (< 0 refused). The
 * passphrase goes straight to the kernel's join (kept in RAM until a restart, never stored or logged); names and
 * addresses are shown, never logged. */
enum { SH_WIFI_OFF, SH_WIFI_STARTING, SH_WIFI_UP, SH_WIFI_SCANNING, SH_WIFI_JOINING, SH_WIFI_DHCP, SH_WIFI_CONNECTED,
       SH_WIFI_FAILED };
#define SH_WIFI_NETS 12
typedef struct { char ssid[33]; int rssi, secure, saved, band5; } sh_wifi_net_t;
typedef struct {
    int state, nnets, rssi, clock_checked;
    char ssid[33], why[64], ip[16];
    sh_wifi_net_t nets[SH_WIFI_NETS];
    /* the last Internet test (sh_wifi_fetch): 0 none, 1 running, 2 finished; its result in words */
    int fetch_state, fetch_ok, fetch_status;
    unsigned fetch_bytes, fetch_ms, fetch_tls_ms;
    char fetch_host[48], fetch_why[64];
} sh_wifi_t;
void sh_wifi(const sh_wifi_t *w);
const char *sh_wifi_why(void);          /* the kernel's reason right now (after a refused start), "" if none */
int sh_wifi_power(int on);
int sh_wifi_scan(void);
int sh_wifi_join(const char *ssid, const char *pass);   /* pass: NULL or "" for open or known networks */
int sh_wifi_fetch(const char *url);
/* Photos, Files, Music (shell_media.inc): the microSD card's exFAT partition, read only, from the platform (the
 * kernel: kern/files.c; the host: a folder standing in for the card). Nothing here writes, and the card's other
 * partition (the OS store) is never read. The card is slow: the shell calls these from idle turns only, never from a
 * touch (sh_card_status alone never touches the card).
 *   sh_card_status  the state now; *why a failure in words ("" if none)
 *   sh_card_mount   brings the card up (blocks, once: while the state is SH_CARD_MOUNTING); the state after
 *   sh_card_list    entries [first, first + max) of a folder ("/" the root): how many were stored (fewer than max:
 *                   the folder's end), < 0 the folder could not be read
 *   sh_card_read    up to len bytes of a file from off: how many were read (0 past its end), < 0 an error */
enum { SH_CARD_MOUNTING, SH_CARD_READY, SH_CARD_NONE, SH_CARD_ERROR };
typedef struct {
    char name[128];             /* UTF-8 */
    uint64_t size;
    int dir;
    uint32_t date;              /* last modified, exFAT's stamp (0 unknown): year - 1980 << 25 | month << 21 | day << 16 |
                                 * hour << 11 | minute << 5 | seconds / 2 */
} sh_dirent_t;
int sh_card_status(const char **why);
int sh_card_mount(void);
int sh_card_list(const char *path, sh_dirent_t *out, int max, int first);
int sh_card_read(const char *path, uint64_t off, void *dst, uint32_t len);                      /* P3c: the year (the Calendar's month) */      /* the next alarm for the lock's widget (on 0: "No alarm") */

/* Linux apps (docs/STARBITE-LINUX-APPS.md 5, 7, 8; L2 + L3): every Linux app's window (an app under sbwl on the M3
 * cores) is a layer of its app page: SH_LINUX_W x SH_LINUX_H surface px at 2x from y 162 (44 pt) to 2850 (772 pt);
 * with the keyboard up the guest's viewport is SH_LINUX_HKB rows (44 to 540 pt) and the rest is under the keyboard.
 * The platform (the kernel: starbite/guest/next/gwin.c on kern/guest.c and sbghost's contract v2) starts the guest,
 * keeps the catalog (the home's Linux page) and the App Store's packages, copies the active window's new rows into
 * the shell's bitmap and takes the input. Apps are named by their desktop id ("org.gnome.TextEditor"); the shell's
 * Terminal (page 2) is the catalog's foot, SH_LINUX_TERMINAL. States are the kernel's GUEST_* order. */
enum { SH_LINUX_OFF, SH_LINUX_STARTING, SH_LINUX_RUNNING, SH_LINUX_STOPPING, SH_LINUX_FAILED };
#define SH_LINUX_W 720
#define SH_LINUX_H 1344                                   /* the surface: 44 to 772 pt at 2x */
#define SH_LINUX_HKB 916                                  /* the rows shown with the keyboard up: 44 to 540 pt */
#define SH_LINUX_Y 162                                    /* the window's first panel row (44 pt) */
#define SH_LINUX_ICON 160                                 /* catalog and store icons: 160 x 160 premultiplied ARGB */
#define SH_LINUX_TERMINAL "org.codeberg.dnkl.foot"        /* the Terminal app's desktop id (also "foot") */
/* the catalog (the guest's .desktop apps): flags as sbshm.h's SBSHM_APP_* */
enum { SH_LAPP_RUNNING = 1, SH_LAPP_WINDOW = 2, SH_LAPP_TERMINAL = 4, SH_LAPP_KEYBOARD = 8, SH_LAPP_SYSTEM = 16 };
#define SH_LAPPS 64
typedef struct { char id[40], name[32]; unsigned flags; int icon, pkg; unsigned rss_kb; } sh_lapp_t;   /* icon, pkg: -1 none */
typedef struct { unsigned seq; int n; unsigned mem_total_kb, mem_avail_kb; sh_lapp_t app[SH_LAPPS]; } sh_lcat_t;
/* the App Store's packages: state, flags and ops as sbshm.h's SBG_PKG_*, SBSHM_PKG_*, SBG_PKG_OP_* */
enum { SH_PKG_IDLE, SH_PKG_QUEUED, SH_PKG_INSTALLING, SH_PKG_REMOVING, SH_PKG_FAILED };
enum { SH_PKG_INSTALLED = 1, SH_PKG_SYSTEM = 2, SH_PKG_TOO_BIG = 4, SH_PKG_NO_SPACE = 8, SH_PKG_FEATURED = 16 };
enum { SH_PKG_OP_INSTALL = 1, SH_PKG_OP_REMOVE = 2, SH_PKG_OP_CANCEL = 3 };
/* Track F, the App Store v3 (starbite/appstore/sbshm-store3.h; FORMAT.md): install methods, phases, updates. The op
 * word sh_linux_pkg takes is op | method << 8 (method 0: the entry's def_method); CHECK with SH_PKG_ALL checks every
 * installed app. state stays SH_PKG_INSTALLING during the DOWNLOADING / BUILDING / VERIFYING phases. */
enum { SH_PKG_UPDATE = 32, SH_PKG_ROLLBACK = 64, SH_PKG_NEEDS_NET = 128 };          /* more flags */
enum { SH_PKG_OP_UPDATE = 4, SH_PKG_OP_ROLLBACK = 5, SH_PKG_OP_CHECK = 6 };
enum { SH_M_DEFAULT = 0, SH_M_DISTRO = 1, SH_M_RELEASE = 2, SH_M_SOURCE = 3 };     /* methods: bit 1 << SH_M_* */
enum { SH_PKG_DOWNLOADING = 5, SH_PKG_BUILDING = 6, SH_PKG_VERIFYING = 7 };       /* phases (besides the states) */
enum { SH_NEED_NETWORK = 1, SH_NEED_AUDIO = 2, SH_NEED_GPU = 4, SH_NEED_CAMERA = 8, SH_NEED_FILES = 16, SH_NEED_KEYBOARD = 32 };
enum { SH_PKG3_VERIFIED = 1, SH_PKG3_MIRROR = 4, SH_PKG3_TERMINAL = 8 };            /* flags3 */
#define SH_PKG_OPWORD(op, m) ((int)((unsigned)(op) | (unsigned)(m) << 8))
#define SH_PKG_ALL (-1)                                   /* sh_linux_pkg's index for CHECK: every installed app */
#define SH_LPKGS 160
typedef struct {
    char pkg[40], name[32], summary[96], category[16], err[32];
    unsigned dl_kb, inst_kb, ram_mb, flags, state, pct;
    int icon;                                             /* -1 none */
    /* v3 (all 0 / "" when the guest has no store3) */
    unsigned methods, method, def_method, phase;          /* methods offered (bits), installed with, preselected, phase */
    unsigned done_kb, total_kb, eta_s, build_min, touch, needs, flags3, rel_size_mb;   /* touch 0 ?, 1 poor, 2 ok, 3 good */
    char ver_inst[24], ver_new[24], ver_rel[24], ver_distro[24], ver_src[24], licence[24], source[48];
    char desc[480];                                       /* the long description (UTF-8, cut at 479 bytes) */
} sh_lpkg_t;
typedef struct {
    unsigned seq; int n, repo_ok; unsigned overlay_total_kb, overlay_free_kb, mem_total_kb; sh_lpkg_t pkg[SH_LPKGS];
    int v3;                                               /* 1: the guest wrote store3 (the v3 fields are valid) */
    unsigned net_ok, mirror_ok, checked_at;               /* checked_at: unix time of the last update check (0 never) */
} sh_lstore_t;

/* the platform */
int sh_linux_open(const char *id);       /* start Linux if off, launch the app (once the catalog has it), put its window
                                          * in the surface; < 0 refused (sh_linux_state says why) */
void sh_linux_hide(void);                /* home: no window in the surface (ACTIVATE none); the apps keep running */
/* the active app's rows newer than the last copy, at 2x (pixel-doubled), into the shell's window: rows[y] is its
 * bitmap row y (2 SH_LINUX_W px; 2 SH_LINUX_H rows, kept in strips); *y0..*y1 the bitmap rows written; returns the
 * surface rows copied (0: nothing new, or not the app's window yet). first: copy every row. */
int sh_linux_blit(uint32_t *const *rows, int first, int *y0, int *y1);
int sh_linux_touch(int phase, int x, int y);   /* surface px, phase 0 down / 1 move / 2 up */
int sh_linux_char(char c);               /* one character typed: committed text for a focused text field, else keys
                                          * (Shift around capitals); '\n' Return and '\b' BackSpace are always keys */
int sh_linux_viewport(int rows);         /* the rows the shell shows (SH_LINUX_H, or SH_LINUX_HKB with the keyboard) */
int sh_linux_icon(int icon, uint32_t *px);     /* a pool icon into px (SH_LINUX_ICON squared); 0: none */
int sh_linux_zero_copy(void);              /* 1.0: 1 when the GPU samples the guest's window surface in place: show it with
                                            * tk_layer_external (the front buffer from the header) instead of the strips */
int sh_linux_hold(int on);               /* 1: the App Store is open: Linux started if off and not stopped while idle */
int sh_linux_pkg(int pkg, int op);       /* install / remove / cancel / update / ... a store package (op: the op word,
                                          * SH_PKG_OPWORD; pkg SH_PKG_ALL for CHECK); < 0 refused */
/* the shell, called by the platform */
void sh_linux_state(int state, const char *why);   /* the guest's state changed (why: the reason when FAILED) */
int sh_linux_frame(void);                /* the active window has new rows; 1 when the window took them (draw) */
void sh_linux_catalog(const sh_lcat_t *c);         /* the catalog changed (also its RUNNING flags); copied */
void sh_linux_store(const sh_lstore_t *s);         /* the packages changed (progress too); the platform's copy,
                                                    * kept by the shell: valid until the next call */
void sh_linux_textfocus(int on);         /* the active app's text field wants the keyboard (1) or not (0) */
/* Track E hook (contract v3, "apps work fully"; starbite/guest/next/INTEGRATION-NEXT.md "v3 hooks"). The platform
 * (gwin.c; shell_linux.inc has weak no-op stand-ins for platforms without it, e.g. the host preview): */
enum { SH_LMOD_CTRL = 1, SH_LMOD_ALT = 2 };       /* as sbshm.h's SBSHM_MOD_* */
int sh_linux_close(const char *id);      /* a Linux app's card swiped away: SBG_MSG_APP_CLOSE for its catalog index */
int sh_linux_key(int code, unsigned mods);         /* a Linux key code (down, up) with SH_LMOD_* held around it */
int sh_linux_char_mods(char c, unsigned mods);     /* a typed character as keys with SH_LMOD_* held (Ctrl+C) */
void sh_linux_cards(int n);              /* Linux apps with a switcher card: n > 0 keeps Linux up while hidden */
int sh_linux_notif_act(unsigned id, int action);   /* a Linux notification tapped (0) or dismissed (-1) */
int sh_linux_clip(const char *text);     /* offer Starbite's clipboard (UTF-8 text) to Linux's apps */
/* Track A's zero-copy window: the guest's front buffer (720 px wide, *stride_px), marked as being read (the guest
 * won't render into it) until the next front or sh_linux_hide; NULL: no window of the app's yet */
const uint32_t *sh_linux_front(int *stride_px);
/* the shell, called by the platform: a Linux notification (who "linux:<id in hex>"), its withdrawal, a Linux copy */
void sh_linux_notify(unsigned id, const char *app_id, const char *app, const char *title, const char *body, int hm);
void sh_linux_notify_close(unsigned id);
void sh_linux_clip_in(const char *text);
/* the shell-wide clipboard: sh_clip_set / sh_clip_get above (B3), defined in shell_linux.inc (offered to Linux) */
/* survival-next: Emergency mode's tools (shell_survival.inc), from the platform: the kernel's survival API
 * (kern/survival.h, services.c's SOS) on the phone, guide/survival_host.c on the host. Nothing starts by itself:
 * the shell calls sh_sos_set(1) and sh_usb_start() only at the end of the user's own hold, and the self test's
 * build refuses both and every setter (kern_next.c, SBOS_SELFTEST_NEXT). */
#define SH_SOS_TORCH 1U                   /* the torch in Morse */
#define SH_SOS_VIB 2U                     /* the vibration motor in Morse */
#define SH_BEACON_SHIFT 4                 /* the screen: 0 white / red, 1 white, 2 red, 3 lit with the Morse */
int sh_sos_set(int on);                   /* 1: the kernel's SOS (a 3 s countdown, then flashing); 0 stops; < 0 refused */
int sh_sos_frame(uint32_t *argb);         /* 1 while SOS owns the screen: the beacon's colour now (0xAARRGGBB) */
int sh_sos_next(void);                    /* ms until the beacon can change next; -1 SOS off */
int sh_sos_secs(void);                    /* the countdown's seconds left (> 0); 0 while flashing */
unsigned sh_sos_opts(void);
void sh_sos_opts_set(unsigned opts);      /* kept with the settings */
enum { SH_USB_OFF, SH_USB_ON, SH_USB_COOL };
typedef struct {
    int state, ms_left, est_ma;           /* est_ma: -1 unknown */
    int vbus_lo_mv, vbus_hi_mv;           /* -1 unknown */
    int bat_c10, limit_c10, ilim_ma, refused;
    const char *why;                      /* refused / the last stop, plain words (NULL none) */
    unsigned events;                      /* bumped at every refusal and stop (a new why) */
} sh_usb_t;
int sh_usb_start(void);                   /* USB power out: one burst after the hold; < 0 refused (why in sh_usb) */
void sh_usb_stop(void);
void sh_usb(sh_usb_t *u);                 /* ~2 Hz while ON or COOL only */
const char *sh_usb_guide(int i);          /* the guidance's line i (NULL past the last) */
#define SH_NOLOC 0x7fffffff
int sh_sun(int *rise, int *set);          /* local minutes: 0 both, 1 the sun stays up, 2 stays down, -2 no location, -3 no clock */
void sh_loc(int *lat_x100, int *lon_x100);   /* 0.01 degrees, N / E positive; SH_NOLOC unset */
void sh_loc_set(int lat_x100, int lon_x100);
#define SH_CARD_NF 9
#define SH_CARD_FL 64
typedef struct { char f[SH_CARD_NF][SH_CARD_FL + 1]; } sh_card_t;   /* Name, Blood type, ... Notes (sh_card_label) */
int sh_card(sh_card_t *c);                /* 1 there is a card */
int sh_card_set(const sh_card_t *c);      /* 1 kept (the Info app's record, saved with the settings) */
const char *sh_card_label(int i);
int sh_last2(void);
const char *sh_last2_why(void);           /* why the last sh_last2_set was refused (NULL none) */
int sh_last2_set(int on);                 /* everything off but the clock, the torch and SOS */
int sh_last2_auto(void);
void sh_last2_auto_set(int on);           /* "automatically at 3 %", kept with the settings */
int sh_guide_n(void);                     /* the offline guide (guide/sbguide.h): chapters, pages, lines */
const char *sh_guide_chapter(int c, const char **sub, unsigned *argb, int *npages);
const char *sh_guide_page(int c, int p, const char *const **lines);   /* lines NULL-terminated; "! " a warning */
const char *sh_sensors_why(void);         /* the sensor hub's state in words (why it is not running) */
/* helper J: the Compass page (the sensor hub in the GNSS chip; about 4 s to start). sh_sensors_on(1) starts it or
 * keeps it running (at least every 30 s while the values show: it stops 60 s after the last call); 0 stops it now;
 * < 0 refused (Last 2 %, the self test). The readings are valid (1) only while it runs: the heading in 0.1 degrees
 * clockwise from magnetic north (accuracy 0..3; below 2: calibrate with a figure 8), pitch / roll in 0.1 degrees
 * (the top / the right edge above the horizon), the steps since the hub started. */
enum { SH_SENS_OFF, SH_SENS_STARTING, SH_SENS_RUNNING, SH_SENS_FAILED };
int sh_sensors_on(int on);
int sh_sensors_state(void);
int sh_heading(int *deg_x10, int *accuracy);
int sh_tilt(int *pitch_x10, int *roll_x10);
int sh_steps(unsigned *n);
long long sh_utc(void);                   /* the clock in seconds since 1970 (0 unknown): a GPS fix's age */
/* Track A (sbos-core-api.md 4, 5): time zone, 12 / 24-hour, alarms. The platform applies the zone to every local
 * time it pushes (sh_status, sh_alarm, the lock's times, notifications' hm); sh_utc stays UTC. Zones are indexed
 * 0 .. sh_tz_count() - 1 (about 70; in regions, west to east). Kept across restarts. */
int sh_tz_count(void);
const char *sh_tz_name(int i, const char **city);   /* "Europe/Helsinki", city "Helsinki" */
int sh_tz_offset_min(int i);                        /* the offset now, DST included */
int sh_tz(void);
void sh_tz_set(int i);                              /* the zone in use (index): the platform pushes sh_status again */
int sh_clock24(void);
void sh_clock24_set(int on);                        /* the shell redraws its times itself */
/* the Clock app's alarms (up to 4): hm = hh * 100 + mm (local); days bit 0 Sunday .. bit 6 Saturday (0 once); label
 * up to 19 bytes. put with i == sh_alarm_count() adds; 0 ok, -1 refused (full, bad values, the self test). Ringing and
 * snooze stay the platform's; the lock's next alarm still comes through sh_alarm */
int sh_alarm_count(void);
int sh_alarm_get(int i, int *hm, int *days, int *on, char *label, int cap);
int sh_alarm_put(int i, int hm, int days, int on, const char *label);
int sh_alarm_del(int i);

/* Starbite OS Next (helper D, shell_nc.inc): notifications. The platform pushes each new one (the kernel: notif.c's
 * posts, copied by kern_next.c's bridge outside the frames); the shell keeps the last 20 (RAM only) for the
 * Notification Centre, the banner and the lock's "N notifications" pill. who: the poster ("clock", "battery", "modem",
 * "heat", "survival" or an app's name); hm: local hh * 100 + mm (-1 unknown). Titles and lines go to the screen only. */
void sh_notify(const char *who, const char *title, const char *body, int hm);
/* Track A (sbos-core-api.md 7): the same with the kernel's NOTIF_* flags (notif_post_app). Before a banner the shell
 * applies the prefs notif.<app>.allow (0: dropped), notif.<app>.banners (0: the Centre only) and notif.dnd (1: no
 * banners but SH_NOTIF_URGENT's); the list is kept across restarts (the blob "nc") */
#define SH_NOTIF_QUIET 1U                 /* the Centre only */
#define SH_NOTIF_URGENT 2U                /* a banner also in Do Not Disturb */
void sh_notify_flags(const char *who, const char *title, const char *body, int hm, unsigned flags);

/* ---- Track A (sbos-core-api.md 1, 2): prefs and blobs, kept across restarts (the kernel: kern/prefs.c in the state
 * slot, saved 3 s after the last change and before a restart or switch-off; the host: in RAM, or the file
 * SBTK_PREFS). The self test's build reads nothing (defaults) and writes nothing (a logged dry run). ---- */
int sh_pref_get(const char *key, char *val, int cap);   /* length, or -1 when not set; keys [a-z0-9._-] <= 31 */
int sh_pref_set(const char *key, const char *val);      /* 0, or -1 (store full / bad key); val 0 = delete; <= 255 bytes */
int sh_pref_int(const char *key, int def);              /* parsed decimal, or def */
void sh_pref_set_int(const char *key, int v);
int sh_blob_put(const char *name, const void *p, unsigned n);   /* 0 / -1; n <= 32 KiB; n 0 = delete; names <= 16 */
int sh_blob_get(const char *name, void *p, unsigned cap);        /* bytes read, or -1 when there is none */
/* the shell, called by the platform once its store is ready (the kernel: after the bake; settings_restore ran before):
 * Notes, the Notification Centre, call recents, the switcher's recents and the Control Centre come back */
void sh_restore(void);
/* the boot menu's last choice (the loader's settings record: a catalog key, "" unknown): the Systems widget's first */
void sh_lastos(const char *key);
/* the last GNSS fix (Maps; the kernel: kern/gnssfix.c, Android's fix on the card): 1 a fix, 0 none. Called from idle
 * turns only (it may read the card) */
typedef struct { int lat_e6, lon_e6, acc_dm, sats; long long utc; char src[12]; } sh_gnss_t;   /* acc_dm, sats: -1 unknown */
int sh_gnss(sh_gnss_t *f);

/* helper I (shell_comms.inc): calls and SMS, on the modem session's kernel API (kern.h's kmodem_call_* and
 * kmodem_sms_*; the kernel's KCALL_* / KAUDIO_* orders). The platform pushes the call with sh_call (on any change,
 * also each second while connected) and sh_sms_changed (its kmodem_sms_gen bumped); the shell asks with the calls
 * below, and only at the end of the user's own action: the call button's 0.6 s hold, Answer (a tap or the slide),
 * Send's tap. Each returns < 0 when refused, with the kernel's reason in why (SH_WHY bytes, plain words: an
 * emergency number, Cellular off, a dry run in the self test). Numbers and message text go to the screen only:
 * nothing here logs them. */
enum { SH_CALL_IDLE, SH_CALL_DIALING, SH_CALL_ALERTING, SH_CALL_INCOMING, SH_CALL_ACTIVE, SH_CALL_HELD, SH_CALL_ENDING,
       SH_CALL_ENDED };
enum { SH_AUDIO_NONE, SH_AUDIO_EARPIECE, SH_AUDIO_SPEAKER };
#define SH_WHY 48
typedef struct {
    int state, outgoing, secs;          /* SH_CALL_*; 1 we dialled; seconds since connected (ACTIVE) */
    int audio, muted, audio_ok;         /* SH_AUDIO_*; audio_ok 0: connected, but no audio path yet */
    char number[24];                    /* "" withheld */
    char why[SH_WHY];                   /* how it ended, in words */
    char audio_why[SH_WHY];             /* fa4ec7f: audio_ok 0: the missing part; audio_ok 1: a warning (the mic) */
} sh_call_t;
#define SH_SMS_MAX 200                  /* the newest this many (the kernel's store keeps up to 200, across restarts) */
enum { SH_SMS_RECEIVED, SH_SMS_SENDING, SH_SMS_SENT, SH_SMS_FAILED, SH_SMS_DELIVERED, SH_SMS_UNDELIVERED };
typedef struct {
    int id, incoming, state;            /* SH_SMS_* (the kernel's ksms states) */
    int unread;                         /* the kernel's (its store): 1 / 0; -1 not known (the shell keeps its own) */
    long long utc;
    char number[24];
    char text[512];                     /* UTF-8 */
    char why[SH_WHY];                   /* a failure in words */
} sh_sms_t;
/* the shell, called by the platform */
void sh_call(const sh_call_t *c);
void sh_sms_changed(unsigned gen);
/* the platform */
int sh_comms_ok(void);                  /* 1: this build has the kernel's calls and SMS (else every call refuses) */
int sh_dial(const char *number, char *why);
int sh_answer(char *why);
int sh_hangup(void);                    /* also declines an incoming call */
int sh_route(int audio, char *why);     /* SH_AUDIO_EARPIECE / SH_AUDIO_SPEAKER */
int sh_mute(int on, char *why);
int sh_dtmf(char digit);                /* 0-9 * # while connected */
int sh_sms_send(const char *number, const char *utf8, char *why);
int sh_sms_count(void);                 /* in RAM, newest last */
int sh_sms_get(int i, sh_sms_t *m);     /* 0 ok */
/* the kernel's SMS store (KMODEM_SMS_STORE; without it: read does nothing, the deletes refuse "Not in this version"):
 * a thread shown is marked read; a message or a whole thread deleted by the user's own tap */
int sh_sms_read(const char *number);
int sh_sms_delete(int id, char *why);
int sh_sms_delete_thread(const char *number, char *why);

/* Track A (sbos-core-api.md 3): the passcode and Auto-Lock, from the platform (the kernel: kern/passcode.c through
 * kern_next.c, kept in the state slot as a salted, iterated SHA-256; the host: the same passcode.c in RAM). Digits go
 * in and never come out; nothing logs them. Back-off: the 6th wrong try in a row waits 1 min, the 7th 5, the 8th 15,
 * the 9th and later 1 h (kept across a restart). Never behind the passcode: Emergency mode, SOS, the medical card,
 * the torch. The shell's page: shell_passcode.inc. */
int sh_pass_on(void);                         /* 1 when a passcode is set */
int sh_pass_len(void);                        /* 4 or 6 (digits), 0 when none */
int sh_pass_check(const char *digits);        /* 1 right; 0 wrong; -N locked out for N more seconds */
int sh_pass_set(const char *old, const char *nw);   /* set (old "" when none) / change / remove (nw ""): 0 or -1 */
int sh_pass_wait(void);                       /* seconds until the next try is allowed (0 = now) */
int sh_pass_fails(void);                      /* wrong tries in a row */
int sh_autolock(void);                        /* the screen-off time, s; 0 = never */
void sh_autolock_set(int seconds);
/* Track A lifecycle (sbos-core-api.md 6): the platform calls this right before a restart or a switch-off: every app
 * in the shell's table with an on_save runs it (pages keep their data with sh_blob_put) */
void sh_save_all(void);

/* Track J (shell_startup.inc, Settings > Startup): Starbite OS as the boot menu, on the firmware layer
 * (docs/STARBITE-FIRMWARE-LAYER.md 10; the kernel: kern/sbfw_os.c on libsb/sbfw_api.h; the host: host/startup_fake.inc).
 * The catalog's OS rows, the loader's settings (the home store's record), one-shot requests, the crash reports and
 * the restarts. sh_os_start_once, sh_req_start and sh_restart_* only ask: the platform's hold confirmation (the
 * kernel's kui_confirm, drawn by sh_confirm) restarts on a full hold. In the self-test build every write and restart
 * is a dry run that logs a line. < 0: refused. */
#define SH_OS_ALL 1                       /* sh_os_count: hidden catalog rows too */
int sh_os_count(int flags);               /* the rows sh_os_get(0 .. n - 1) gives (flags as last passed here) */
/* key (24 bytes), name (32); category: catalog.h CAT_* (0 Apple, 1 Microsoft, 2 Android, 3 Linux, 4 Starbite, 5 other,
 * 6 lab); method: M_* (1 XNU, 2 Linux, 3 Android, 4 UEFI, 5 bare); ready: its files are there; is_default */
int sh_os_get(int i, char *key, char *name, int *category, int *method, int *ready, int *is_default);
int sh_os_start_once(const char *key);
int sh_os_make_default(const char *key);
/* the loader's settings: debug_level 0 off .. 3 highest; post 0 full, 1 quick, 2 off; wdt_mode 0 per entry, 1 off,
 * 2 wdt_s (10..82) for every entry; crash_mode 0 on, 1 save only, 2 off; crash_action 0 boot menu, 1 Starbite
 * Recovery, 2 retry once; crash_detail 0..3; shell 1: Starbite OS (shell_key, "" = starbite-os) instead of the menu */
typedef struct {
    char default_key[24];
    int timeout_s, debug_level, post, wdt_mode, wdt_s, crash_mode, crash_action, crash_detail, shell;
    char shell_key[24];
} sh_fw_t;
int sh_fw_get(sh_fw_t *f);                /* 1 the record, 0 its defaults (none yet), < 0 no home store */
int sh_fw_set(const sh_fw_t *f);          /* written and read back: 0 */
/* one-shots for an OS's next start (the loader's SBFW_REQ_*); WINRE is plan only (never fits) */
enum { SH_REQ_VERBOSE = 1, SH_REQ_SAFE = 2, SH_REQ_SINGLE = 3, SH_REQ_PREV = 4, SH_REQ_XNU_SINGLE = 5, SH_REQ_WINRE = 6 };
int sh_req_fits(const char *key, int action);
int sh_req_start(const char *key, int action);
/* the crash reports (the home store's crash log), i = 0 the newest; severity 0 info, 1 warning, 2 crash; utc 0 unknown */
typedef struct { char key[24], os[32], kind[24], line[96]; int severity; long long utc; } sh_crash_t;
int sh_crash_count(void);                 /* < 0: no crash log on the home store */
int sh_crash_get(int i, sh_crash_t *c);   /* 0 ok */
int sh_restart_menu(void);
int sh_restart_recovery(void);
int sh_restart_download(void);
/* the loader's table for this start: medium 0 UFS, 1 the card, -1 none; why: why this start, in words */
typedef struct {
    int valid, loader_version, medium, card_only, shell_mode, req_action, post_fails, post_warns, crash_sev;
    char why[40], crash_key[24], crash_kind[24];
} sh_fwinfo_t;
int sh_fw_info(sh_fwinfo_t *i);           /* 1 the loader's table, 0 an older loader */

/* ---- Track A: drivers for the shell (the kernel: kern_next.c's block of that name on kern/bt.c, cam.c, shub_more.c,
 * bathealth.c, haptic.c; the host: simulated). Nothing here runs until the shell calls it, except the LED policy and
 * auto-brightness (when display.auto is 1). Calls that would power hardware are logged dry runs in the self-test
 * build (< 0 / 0 returned, why: "A dry run: the self test"). The kernel drives every running part from its own
 * timers: no call here blocks the UI for more than ~20 ms (the camera's poll: ~100 ms at most). ---- */
/* Bluetooth: scanning only (no pairing, no audio). sh_bt_on(1) starts the chip (firmware, patch: ~10 s); state 0 off,
 * 1 starting, 2 on, -1 failed (sh_bt_why: plain words). sh_bt_scan(1) scans (LE, then classic) again and again while
 * on, for at most 60 s; sh_bt_scan(0) stops after the scan that runs (they end by themselves, ~11 s). The devices:
 * strongest first, name ("" when none was heard) and RSSI in dBm; the list stays between scans. < 0: refused. */
int sh_bt_on(int on);
int sh_bt_state(void);
const char *sh_bt_why(void);
int sh_bt_scan(int on);
int sh_bt_count(void);
int sh_bt_get(int i, char *name, int cap, int *rssi);   /* 0 ok, -1 no such device */
/* the front camera (no flash: use a white screen): a portrait selfie preview w x h (each <= 816, w * h <= 512 K, e.g.
 * 360 x 480), mirrored; 0 started, < 0 refused (sh_cam_why). The newest frame: 0xFFRRGGBB, w * h, row after row
 * (stride = w), or 0 while none yet / closed / failed; seq counts up with each new frame. The memory is the camera's
 * (uncached): copy it within the turn. sh_cam_still asks for a still (816 x 612 rotated: 612 x 816, not mirrored);
 * sh_cam_still_get gives it once ready (until the next still or close): its pixels and its 24-bit BMP file. */
int sh_cam_open(int w, int h);
const uint32_t *sh_cam_frame(int *w, int *h, unsigned *seq);
int sh_cam_still(void);                               /* 0 asked, < 0 not running / one already pending */
const uint32_t *sh_cam_still_get(int *w, int *h, const unsigned char **bmp, unsigned *bmp_len);   /* 0 not (yet) */
const char *sh_cam_why(void);
void sh_cam_close(void);
/* the hub's sensors (each call keeps its sensor on for 10 s; the first may take ~5 s while the hub starts): -1 when
 * not known yet. Light in lux; pressure in hPa x 10 (10132 = 1013.2 hPa); proximity 1 near, 0 far; the gyroscope in
 * millidegrees a second (Android axes; 1 valid) */
int sh_light_lux(void);
int sh_pressure_hpa10(void);
int sh_proximity_near(void);
int sh_gyro_mdps(int *x, int *y, int *z);
/* Auto-Brightness (the pref display.auto, 0 by default): the panel's level (1..4) from the light every 2 s while the
 * screen is on; a level the user picks stays until the light moves to another level. sh_bright_now tells the shell. */
void sh_auto_brightness(int on);
int sh_auto_brightness_on(void);
/* a call at the ear: 1 while the call screen wants the proximity sensor (a call at the earpiece), 0 after. While
 * something is near, the panel is dark and touches are dropped; the Power key still works. The kernel ends it by
 * itself when the call ends. */
void sh_call_proximity(int on);
/* Settings > Battery: Maximum Capacity (0..100 %), cycle count, plain words; 1 read, 0 not (words: why) */
int sh_battery_health(int *max_pct, int *cycles, char *words, int cap);
/* haptic patterns (kern/haptic.c) at the haptic.level intensity (0 off, 1 light, 2 medium, 3 strong: the default);
 * RING and ALARM loop until sh_haptic_stop (60 s at most). sh_haptic / sh_haptic_k above are unchanged. */
enum { SH_VIB_RING, SH_VIB_NOTIFY, SH_VIB_ALARM, SH_VIB_TICK, SH_VIB_SUCCESS, SH_VIB_WARNING, SH_VIB_ERROR };
void sh_haptic_kind(int kind);
void sh_haptic_stop(void);
int sh_haptic_level(void);
void sh_haptic_level_set(int level);                  /* kept as the pref haptic.level */

#endif

/* Track A, 1.0 (sbos-core-api.md 10): a safe start (the loader's one-shot from Settings > Startup or Starbite
 * Recovery): Wi-Fi isn't restored and nothing is saved until the next start. why: why this start, in words */
int sh_safe_mode(void);
int sh_boot_reason(char *why, int cap);

/* Track A, 1.0: Restart and Shut Down (the shell's own slide-to-power-off is the confirmation, as on iOS). Both save
 * first (the pages' on_save, the settings and prefs). Restart starts this same OS again when the loader said which it
 * is (its SBFW table: the key of this start), else it goes back to the boot menu. In the self-test build they only log.
 * < 0: refused (e.g. a heat session still running: the shell says so) */
int sh_restart(void);
int sh_power_off(void);
/* Track A (sbos-core-api.md 8, with Track J's loader half): Software Update. The card's /STARBITE/UPDATE/MANIFEST.TXT
 * (key=, version=, image=, size=, sha256=, notes=) names an image for this OS's catalog row; it is staged into the
 * inactive update slot (sbos-slot-a / -b), checked (SHA-256, then the slot's header and CRC32 read back), armed as
 * the loader's one-shot and started; 10 s after the home screen of that start it becomes the active version, else
 * the loader rolls back (a notification says either). In the self-test build every write and restart is a dry run.
 * state: 0 none found (why: a detail), 1 found, 2 staging (pct), 3 ready (staged, restart to install), 4 installed
 * (this start committed it), -1 failed (why) */
#ifndef SH_UPDATE_DEFINED
#define SH_UPDATE_DEFINED
struct sh_update { char version[32], notes[160]; unsigned size; int state; int pct; char why[80]; };
int sh_update_check(struct sh_update *u);   /* reads the manifest (may read the card: from a tap or an idle turn); 1 found */
int sh_update_stage(struct sh_update *u);   /* one chunk a call, so the UI stays live, until state 3 or -1 */
int sh_update_install(void);                /* arms and restarts (does not return on success); -1 refused */
#endif

/* Track A: the Camera's photos (the kernel: kern/photos.c on the home store's volatile entry "sbos-photos", 64 MiB:
 * a ring of the newest 32; the host: RAM, SBTK_PHOTOS=FILE also keeps them in a file). Pixels are 0xFFRRGGBB, row
 * after row (stride = w). Kept as 24-bit RGB; a picture of more than 697685 pixels (2 MiB - 4 KiB at 3 bytes) is
 * scaled down to the largest size that fits, the aspect kept: 612 x 816 (the front camera's still) is kept exactly,
 * 1080 x 1440 as 723 x 964. Sides up to 4096. Crash safe: a save that is cut off leaves the photos that were there
 * (the oldest one, when the 32 were full, may be gone); a damaged photo is skipped.
 * sh_photo_save: 0 kept as the newest (with 32 kept, the oldest goes), -1 refused / failed: sh_photo_why says why in
 * plain words (no photo storage on this phone, storage busy while the modem is on, a safe start, a storage error).
 * It blocks for the write: ~0.2..0.4 s for a 612 x 816 photo on the card (~30 ms on UFS); call it from the shutter's
 * tap, not every frame. sh_photo_count: how many (the first call reads the 32 headers: ~30 ms).
 * sh_photo_get: i 0 = the newest; writes w * h pixels to argb (cap_px at least w * h) and returns w * h; argb 0: only
 * w, h and utc (s since 1970, 0 unknown) and w * h (the size to allocate); -1: no such photo / too small / a read
 * error (why). Reads ~1.5 MB (~0.1 s on the card). A damaged photo found here is skipped (the next one is given in
 * its place) and the count drops by one: ask sh_photo_count again after a get.
 * sh_photo_delete: 0 deleted (the photos after i move up), -1 (why). In the self-test build saves and deletes are
 * logged dry runs (0 returned, nothing kept) and nothing is read: count 0. */
int sh_photo_save(const uint32_t *argb, int w, int h);
int sh_photo_count(void);
int sh_photo_get(int i, uint32_t *argb, int cap_px, int *w, int *h, long long *utc);
int sh_photo_delete(int i);
const char *sh_photo_why(void);

/* the platform: Power held about a second (the screen is on now): the shell shows its power sheet (slide to power off,
 * Restart, Cancel). Track B defines it; until then a weak default does nothing */
void sh_power_held(void);

/* Track A, 1.0: performance and keep-awake.
 * - sh_perf_mode(1) in Emergency / Survival / Extreme saving (the top clocks drop 0.4 s after motion), 0 in normal
 *   mode (they hold 1.5 s past the last motion frame, so springs and transitions never drop frames).
 * - sh_perf_hold(ms): top clocks now for ms (a transition that no touch started: a banner, the wake's scale-in).
 * - sh_hold_awake(who, on): a long job (an App Store install, a Linux build) keeps the screen on: no Auto-Lock while
 *   any holder is on; returns the holders now, < 0 refused. Off with the same who. */
void sh_perf_mode(int conservative);
void sh_perf_hold(int ms);
int sh_hold_awake(const char *who, int on);
