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
/* P2z, the Clock app: when to come back (ms; < 0 nothing pending), the redraw then (1: something changed), and the
 * platform's vibration for a timer that ran out (no sound) */
int sh_next_update(float t_ms);
int sh_refresh(float t_ms);
void sh_vibrate(void);
void sh_year(int y);
void sh_haptic(void);                     /* P3h: a key's light tap (the vibration motor, ~10 ms) */
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
#define SH_LPKGS 160
typedef struct {
    char pkg[40], name[32], summary[96], category[16], err[32];
    unsigned dl_kb, inst_kb, ram_mb, flags, state, pct;
    int icon;                                             /* -1 none */
} sh_lpkg_t;
typedef struct { unsigned seq; int n, repo_ok; unsigned overlay_total_kb, overlay_free_kb, mem_total_kb; sh_lpkg_t pkg[SH_LPKGS]; } sh_lstore_t;

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
int sh_linux_pkg(int pkg, int op);       /* install / remove / cancel a store package; < 0 refused */
/* the shell, called by the platform */
void sh_linux_state(int state, const char *why);   /* the guest's state changed (why: the reason when FAILED) */
int sh_linux_frame(void);                /* the active window has new rows; 1 when the window took them (draw) */
void sh_linux_catalog(const sh_lcat_t *c);         /* the catalog changed (also its RUNNING flags); copied */
void sh_linux_store(const sh_lstore_t *s);         /* the packages changed (progress too); the platform's copy,
                                                    * kept by the shell: valid until the next call */
void sh_linux_textfocus(int on);         /* the active app's text field wants the keyboard (1) or not (0) */
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

/* Starbite OS Next (helper D, shell_nc.inc): notifications. The platform pushes each new one (the kernel: notif.c's
 * posts, copied by kern_next.c's bridge outside the frames); the shell keeps the last 20 (RAM only) for the
 * Notification Centre, the banner and the lock's "N notifications" pill. who: the poster ("clock", "battery", "modem",
 * "heat", "survival" or an app's name); hm: local hh * 100 + mm (-1 unknown). Titles and lines go to the screen only. */
void sh_notify(const char *who, const char *title, const char *body, int hm);
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
#define SH_SMS_MAX 32
enum { SH_SMS_RECEIVED, SH_SMS_SENDING, SH_SMS_SENT, SH_SMS_FAILED };
typedef struct {
    int id, incoming, state;            /* SH_SMS_* */
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

#endif
