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

/* survival-next: Emergency mode's tools (shell_survival.inc), from the platform: the kernel's survival API
 * (kern/survival.h, services.c's SOS) on the phone, guide/survival_host.c on the host. Nothing starts by itself:
 * the shell calls sh_sos_set(1) and sh_usb_start() only at the end of the user's own hold, and the self test's
 * build refuses both and every setter (kern_next.c, SBOS_SELFTEST_NEXT). */
#define SH_SOS_TORCH 1U                   /* the torch in Morse */
#define SH_SOS_VIB 2U                     /* the vibration motor in Morse */
#define SH_BEACON_SHIFT 4                 /* the screen: 0 white / red, 1 white, 2 red, 3 lit with the Morse */
int sh_sos_set(int on);                   /* 1: the kernel's SOS (a 3 s countdown, then flashing); 0 stops; < 0 refused */
int sh_sos_frame(uint32_t *argb);         /* 1 while SOS owns the screen: the beacon's colour now (0xAARRGGBB) */
int sh_sos_secs(void);                    /* the countdown's seconds left (> 0); 0 while flashing */
unsigned sh_sos_opts(void);
void sh_sos_opts_set(unsigned opts);      /* kept with the settings */
enum { SH_USB_OFF, SH_USB_ON, SH_USB_COOL };
typedef struct {
    int state, ms_left, est_ma;           /* est_ma: -1 unknown */
    int vbus_lo_mv, vbus_hi_mv;           /* -1 unknown */
    int bat_c10, limit_c10, ilim_ma, refused;
    const char *why;                      /* refused / the last stop, plain words (NULL none) */
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
int sh_last2_set(int on);                 /* everything off but the clock, the torch and SOS */
int sh_last2_auto(void);
void sh_last2_auto_set(int on);           /* "automatically at 3 %", kept with the settings */
int sh_guide_n(void);                     /* the offline guide (guide/sbguide.h): chapters, pages, lines */
const char *sh_guide_chapter(int c, const char **sub, unsigned *argb, int *npages);
const char *sh_guide_page(int c, int p, const char *const **lines);   /* lines NULL-terminated; "! " a warning */
const char *sh_sensors_why(void);         /* why there is no compass or level yet */

#endif
