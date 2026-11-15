/* SPDX-License-Identifier: MIT
 * sbtk-host: the toolkit and the lock shell on the host (P1 of the UI port plan).
 *   sbtk-host PAK still OUT.ppm [state ...]      one frame at rest, full resolution; a state is what=key:value
 *                                               (unlock=:0.5, group=apple, focus=win11:1, scroll=:120, hold=t:0.6)
 *   sbtk-host PAK film OUT.bgra SCRIPT [half]   60 fps: the script's touches through the gestures and springs,
 *                                               every frame composed as the phone would (motion frames at half
 *                                               resolution + the 2x present, the first frame at rest full); frames
 *                                               as raw BGRA (720 x 1480 with half), one "F" line per frame on stdout
 *   sbtk-host PAK bench                         uibench's kernels on this host (the A55 calibration)
 * A script: "wait MS", "tap X Y [MS]", "drag X0 Y0 X1 Y1 MS", "hold X Y MS" (pt, ms), "ori N" (cp7: the phone turned, 0..3), "keys TEXT" (cp7: a hardware keyboard), separated by ';'; also "power", "powerhold",
 * "notify WHO|TITLE|BODY", "dragh X0 Y0 X1 Y1 MS HOLD", "state WHAT=KEY:V" (a still's state, now), "ring MS" (the
 * pretend network's incoming call MS ms later) and "sms TEXT" (a message received now from a made-up number). */
#include "../sbtk.h"
#include "../shell.h"
#include "../face_svc.h"
#include "face.h"                 /* Face Unlock (ui/face_svc.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void sh_torch(int lit) { fprintf(stderr, "torch: %s\n", lit ? "on" : "off"); }
void sh_boot_os(const char *key) { fprintf(stderr, "slide to start: would restart into %s\n", key); }
int sh_modem(int on)
{
    if (on) sh_confirm("Start the modem?", "Receive only: its status, no calls, SMS or data.",
                       "A SIM or a network registration parks it.", "The first start is a test: be there.", "Hold to start");
    else fprintf(stderr, "modem: would stop\n");
    return 0;
}
/* Track J: Settings > Startup's pretend platform (the restarts' confirmations come back through sh_confirmed) */
#include "startup_fake.inc"
void sh_confirmed(int ok)
{
    if (startup_fake_confirmed(ok)) return;
    fprintf(stderr, "confirm: %s\n", ok ? "held: the modem would start" : "cancelled");
}
/* a pretend network for the previews: on -> searching -> data up on "Operator" LTE, 3 bars */
static int fc_net;                          /* helper I: comms_fake.inc's calls and SMS work while it is on */
static int fc_data, fc_calls;               /* 8z: the two switches */
static void fc_cell_push(void)
{
    int on = fc_data || fc_calls;
    fc_net = on;
    sh_cell_t c = { on ? 1 : 0, 0, 0, on ? 7 : 0, on ? 3 : -1, fc_data ? SH_DATA_UP : fc_calls ? SH_DATA_REGISTERED : SH_DATA_OFF, "", "", 0, 0 };
    c.data_want = fc_data;
    c.calls = fc_calls;
    if (on) memcpy(c.oper, "Operator", 9);
    sh_cell(&c);
}
int sh_cellular(int on)
{
    fc_data = !!on;
    fprintf(stderr, "cellular: %s\n", on ? "on (pretend: data up)" : "off");
    fc_cell_push();
    return 0;
}
int sh_cell_calls(int on)
{
    fc_calls = !!on;
    fprintf(stderr, "cellular: calls and SMS %s\n", on ? "on (pretend: registered)" : "off");
    fc_cell_push();
    return 0;
}
void sh_brightness(int level) { fprintf(stderr, "brightness: level %d\n", level); }
/* Track A: prefs and blobs (the kernel's kern/prefs.c) in RAM, with its limits; SBTK_PREFS=FILE also keeps them in a
 * file, read at the start and written at every change, so a still or film run after another with the same file
 * shows what a restart keeps */
#define HP_KEYS 400
static struct { char k[32], v[256]; } HP[HP_KEYS];
static int hpn;
static struct { char n[17]; unsigned char *p; unsigned len; } HB[16];
static int hbn;
static int hp_name_ok(const char *k, size_t max)
{
    size_t n = 0;
    if (!k || !k[0]) return 0;
    for (; k[n]; n++)
        if (n >= max || !((k[n] >= 'a' && k[n] <= 'z') || (k[n] >= '0' && k[n] <= '9') || strchr("._-", k[n])))
            return 0;
    return 1;
}
static void hp_write(void)
{
    const char *f = getenv("SBTK_PREFS");
    FILE *o = f ? fopen(f, "wb") : 0;
    if (!o) return;
    fputs("SBTKPREFS1\n", o);
    for (int i = 0; i < hpn; i++) fprintf(o, "P %zu %s %zu %s\n", strlen(HP[i].k), HP[i].k, strlen(HP[i].v), HP[i].v);
    for (int i = 0; i < hbn; i++) {
        fprintf(o, "B %zu %s %u ", strlen(HB[i].n), HB[i].n, HB[i].len);
        fwrite(HB[i].p, 1, HB[i].len, o);
        fputc('\n', o);
    }
    fclose(o);
}
static void hp_load(void)
{
    const char *f = getenv("SBTK_PREFS");
    FILE *in = f ? fopen(f, "rb") : 0;
    char line[16];
    if (!in) return;
    if (!fgets(line, sizeof line, in) || strcmp(line, "SBTKPREFS1\n")) { fclose(in); return; }
    int c;
    while ((c = fgetc(in)) == 'P' || c == 'B') {
        size_t kl = 0, vl = 0;
        char k[40];
        if (fscanf(in, " %zu ", &kl) != 1 || kl > 31 || fread(k, 1, kl, in) != kl) break;
        k[kl] = 0;
        if (fscanf(in, " %zu", &vl) != 1 || fgetc(in) != ' ' || vl > (c == 'P' ? 255u : 32768u)) break;
        char *v = malloc(vl + 1);
        if (!v || fread(v, 1, vl, in) != vl) { free(v); break; }
        v[vl] = 0;
        if (c == 'P' && hpn < HP_KEYS) { strcpy(HP[hpn].k, k); strcpy(HP[hpn].v, v); hpn++; free(v); }
        else if (c == 'B' && hbn < 16 && kl <= 16) {
            strcpy(HB[hbn].n, k);
            HB[hbn].p = (unsigned char *)v;
            HB[hbn++].len = (unsigned)vl;
        }
        else free(v);
        if (fgetc(in) != '\n') break;
    }
    fclose(in);
    fprintf(stderr, "prefs: %d keys, %d blobs from %s\n", hpn, hbn, f);
}
int sh_pref_get(const char *key, char *val, int cap)
{
    if (val && cap > 0) val[0] = 0;
    for (int i = 0; i < hpn; i++)
        if (!strcmp(HP[i].k, key)) {
            if (val && cap > 0) snprintf(val, (size_t)cap, "%s", HP[i].v);
            return (int)strlen(HP[i].v);
        }
    return -1;
}
int sh_pref_set(const char *key, const char *val)
{
    if (!hp_name_ok(key, 31) || (val && strlen(val) > 255)) return -1;
    int i = 0;
    while (i < hpn && strcmp(HP[i].k, key)) i++;
    if (!val) { if (i < hpn) { HP[i] = HP[--hpn]; hp_write(); } return 0; }
    if (i == hpn) { if (hpn == HP_KEYS) return -1; strcpy(HP[hpn++].k, key); }
    else if (!strcmp(HP[i].v, val)) return 0;
    strcpy(HP[i].v, val);
    hp_write();
    return 0;
}
int sh_pref_int(const char *key, int def)
{
    char b[16], *e;
    int n = sh_pref_get(key, b, sizeof b);
    if (n <= 0 || n >= (int)sizeof b) return def;
    long v = strtol(b, &e, 10);
    return *e ? def : (int)v;
}
void sh_pref_set_int(const char *key, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); (void)sh_pref_set(key, b); }
/* 8a: Notes' and Contacts' documents (kern/docs.c on the phone) in RAM, with the phone's limits */
static struct { int c; unsigned id, n; unsigned char *p; } HD[256];
static int hdn;
static unsigned hd_used(int c) { unsigned u = 0; for (int i = 0; i < hdn; i++) if (HD[i].c == c) u += HD[i].n; return u; }
int sh_doc_put(int coll, unsigned id, const void *p, unsigned n)
{
    unsigned budget = coll == 0 ? 96u << 10 : 64u << 10;
    if (coll < 0 || coll > 1 || !id || n > (32u << 10)) return -1;
    int k = -1;
    for (int i = 0; i < hdn; i++) if (HD[i].c == coll && HD[i].id == id) k = i;
    if (!n) { if (k >= 0) { free(HD[k].p); HD[k] = HD[--hdn]; } return 0; }
    if (hd_used(coll) - (k >= 0 ? HD[k].n : 0) + n > budget) return -1;
    if (k < 0) { if (hdn >= 256) return -1; k = hdn++; HD[k].c = coll; HD[k].id = id; HD[k].p = 0; }
    free(HD[k].p);
    HD[k].p = malloc(n);
    memcpy(HD[k].p, p, n);
    HD[k].n = n;
    return 0;
}
int sh_doc_get(int coll, unsigned id, void *p, unsigned cap)
{
    for (int i = 0; i < hdn; i++)
        if (HD[i].c == coll && HD[i].id == id) { if (p) memcpy(p, HD[i].p, HD[i].n < cap ? HD[i].n : cap); return (int)HD[i].n; }
    return -1;
}
int sh_doc_count(int coll) { int n = 0; for (int i = 0; i < hdn; i++) n += HD[i].c == coll; return n; }
unsigned sh_doc_id_at(int coll, int k)
{
    unsigned last = 0;
    for (int s = 0; s <= k; s++) {
        unsigned best = 0;
        for (int i = 0; i < hdn; i++) if (HD[i].c == coll && HD[i].id > last && (!best || HD[i].id < best)) best = HD[i].id;
        if (!best) return 0;
        last = best;
    }
    return last;
}
unsigned sh_doc_new_id(int coll) { unsigned m = 0; for (int i = 0; i < hdn; i++) if (HD[i].c == coll && HD[i].id > m) m = HD[i].id; return m + 1; }
void sh_doc_usage(int coll, unsigned *used, unsigned *budget)
{
    if (used) *used = hd_used(coll);
    if (budget) *budget = coll == 0 ? 96u << 10 : 64u << 10;
}
int sh_blob_put(const char *name, const void *p, unsigned n)
{
    if (!hp_name_ok(name, 16) || n > 32768 || (n && !p)) return -1;
    int i = 0;
    while (i < hbn && strcmp(HB[i].n, name)) i++;
    if (i < hbn && HB[i].len == n && !memcmp(HB[i].p, p, n)) return 0;
    if (i < hbn) { free(HB[i].p); HB[i] = HB[--hbn]; }
    if (n) {
        if (hbn == 16) return -1;
        strcpy(HB[hbn].n, name);
        HB[hbn].p = malloc(n);
        memcpy(HB[hbn].p, p, n);
        HB[hbn++].len = n;
    }
    hp_write();
    return 0;
}
int sh_blob_get(const char *name, void *p, unsigned cap)
{
    for (int i = 0; i < hbn; i++)
        if (!strcmp(HB[i].n, name)) {
            unsigned n = HB[i].len < cap ? HB[i].len : cap;
            if (p) memcpy(p, HB[i].p, n);
            return (int)n;
        }
    return -1;
}
void sh_vibrate(void) { fprintf(stderr, "vibrate: the timer ran out\n"); }
void sh_haptic(void) { }
/* (B3's clipboard: sh_clip_set / sh_clip_get are Track E's, in shell_linux.inc, linked here with shell_lock.c) */
void sh_haptic_k(int kind) { (void)kind; }
/* B5: the Clock's alarms (the kernel's alarm.c on the phone) */
int sh_alarm_set(int i, int h, int m, int days, int on) { fprintf(stderr, "alarm %d: %02d:%02d %s (days %x)\n", i, h, m, on ? "on" : "off", (unsigned)days); return 0; }
/* L2 + L3: Linux apps and the App Store (the kernel's platform is starbite/guest/next/gwin.c): host/linux_fake.inc,
 * a pretend Linux with a catalog and a store. SBTK_NO_LINUX_FAKE: none (starbite/guest/next/host/gwin-host.c links
 * gwin.c itself and drives the shell) */
#ifndef SBTK_NO_LINUX_FAKE
#include "linux_fake.inc"
#else
static void lin_fake_init(void) {}
static void lin_fake_poll(float t) { (void)t; }
static void lin_fake_settle(void) {}
static void lin_fake_state(const char *st) { (void)st; }
#endif
/* helper I: calls and SMS on a pretend network (made-up numbers; nothing logs a number or a text) */
#include "comms_fake.inc"
/* wifi-sbos-next: a pretend Wi-Fi for the previews (made-up network names; nothing is joined): on lists four
 * networks, a join connects, the Internet test answers 200 */
static sh_wifi_t HW;
static void hw_push(void) { sh_wifi(&HW); }
int sh_wifi_power(int on)
{
    /* 8b: a busy street's worth (12), so the page's list scrolls on the host too */
    static const char *const N[12] = { "Home Wi-Fi", "Guest", "Upstairs 5G", "Cafe Corner", "Neighbour", "Library Free",
                                       "Printer-Direct", "Kitchen", "Garage", "Studio 5G", "Bakery", "Hotspot 7" };
    static const int R[12] = { -48, -55, -67, -80, -70, -72, -74, -76, -78, -82, -84, -86 };
    static const int S[12] = { 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1 }, B[12] = { 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0 };
    memset(&HW, 0, sizeof(HW));
    HW.state = on ? SH_WIFI_UP : SH_WIFI_OFF;
    if (on) {
        HW.nnets = 12 < SH_WIFI_NETS ? 12 : SH_WIFI_NETS;
        for (int i = 0; i < HW.nnets; i++) {
            strcpy(HW.nets[i].ssid, N[i]);
            HW.nets[i].rssi = R[i], HW.nets[i].secure = S[i], HW.nets[i].band5 = B[i];
        }
    }
    fprintf(stderr, "wifi: %s\n", on ? "on" : "off");
    hw_push();
    return 0;
}
int sh_wifi_scan(void) { fprintf(stderr, "wifi: scan\n"); hw_push(); return 0; }
const char *sh_wifi_why(void) { return ""; }
int sh_wifi_join(const char *ssid, const char *pass)
{
    fprintf(stderr, "wifi: join (a passphrase of %d characters)\n", pass ? (int)strlen(pass) : 0);
    HW.state = SH_WIFI_CONNECTED;
    strncpy(HW.ssid, ssid, 32);
    strcpy(HW.ip, "192.168.1.23");
    hw_push();
    return 0;
}
int sh_wifi_fetch(const char *url)
{
    fprintf(stderr, "wifi: fetch %s\n", url);
    HW.fetch_state = 2, HW.fetch_ok = 1, HW.fetch_status = 200, HW.fetch_bytes = 1256, HW.fetch_ms = 520;
    HW.fetch_tls_ms = 380;
    strcpy(HW.fetch_host, "example.com");
    strcpy(HW.fetch_why, "HTTPS through the phone's own TCP and TLS");
    hw_push();
    return 0;
}
/* Photos, Files, Music: a folder on this computer stands in for the microSD card, read only. The default is
 * "card" beside the pack (a made-up card: tools/sbtk-fakecard.py); SBTK_CARD=DIR another folder, SBTK_CARD=none no
 * card; SBTK_CARD_MS=N makes every call take N ms (a slow card: the progress, the frame times) */
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static char card_root[600];
static int card_up;
static void card_init(const char *pak)
{
    const char *e = getenv("SBTK_CARD");
    struct stat st;
    if (e && !strcmp(e, "none")) return;
    if (e) snprintf(card_root, sizeof card_root, "%s", e);
    else {
        const char *sl = strrchr(pak, '/');
        snprintf(card_root, sizeof card_root, "%.*s/card", sl ? (int)(sl - pak) : 1, sl ? pak : ".");
    }
    if (stat(card_root, &st) || !S_ISDIR(st.st_mode)) card_root[0] = 0;
}
static void card_slow(void) { const char *e = getenv("SBTK_CARD_MS"); if (e) usleep((useconds_t)atoi(e) * 1000); }
int sh_card_status(const char **why)
{
    if (why) *why = "";
    return !card_root[0] ? SH_CARD_NONE : card_up ? SH_CARD_READY : SH_CARD_MOUNTING;
}
int sh_card_mount(void) { card_slow(); card_up = 1; return sh_card_status(0); }
static int card_name_cmp(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }
int sh_card_list(const char *path, sh_dirent_t *out, int max, int first)
{
    char p[1200];
    struct dirent **nl;
    card_slow();
    if (!card_up) return -1;
    snprintf(p, sizeof p, "%s%s", card_root, path);
    int n = scandir(p, &nl, 0, card_name_cmp), k = 0, at = 0;
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        struct stat st;
        char f[1600];
        snprintf(f, sizeof f, "%s/%s", p, nl[i]->d_name);
        if (strcmp(nl[i]->d_name, ".") && strcmp(nl[i]->d_name, "..") && strlen(nl[i]->d_name) < sizeof out[0].name && !stat(f, &st)) {
            if (at++ >= first && k < max) {
                struct tm tm;
                time_t t = st.st_mtime;
                localtime_r(&t, &tm);
                memset(&out[k], 0, sizeof out[k]);
                strcpy(out[k].name, nl[i]->d_name);
                out[k].dir = S_ISDIR(st.st_mode);
                out[k].size = out[k].dir ? 0 : (uint64_t)st.st_size;
                out[k].date = (uint32_t)(tm.tm_year - 80) << 25 | (uint32_t)(tm.tm_mon + 1) << 21 | (uint32_t)tm.tm_mday << 16 |
                              (uint32_t)tm.tm_hour << 11 | (uint32_t)tm.tm_min << 5 | (uint32_t)(tm.tm_sec / 2);
                k++;
            }
        }
        free(nl[i]);
    }
    free(nl);
    return k;
}
int sh_card_read(const char *path, uint64_t off, void *dst, uint32_t len)
{
    char p[1200];
    card_slow();
    if (!card_up) return -1;
    snprintf(p, sizeof p, "%s%s", card_root, path);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, (long)off, SEEK_SET);
    int n = (int)fread(dst, 1, len, f);
    fclose(f);
    return n;
}
static int screen_off;
void sh_slept(void) { screen_off = 1; fprintf(stderr, "sleep: the screen would go off now\n"); }

/* survival-next: Emergency mode's tools on guide/survival_host.c's simulated state (its clock: surv_host_now_ms, set
 * every film frame); SOS here: the kernel's 3 s countdown, then the beacon (services.c's colours, 250 ms phases) */
#include "../../kern/survival.h"
#include "../../guide/sbguide.h"
extern uint64_t surv_host_now_ms;
static int hsos;
static uint64_t hsos_t0;
int sh_sos_set(int on)
{
    fprintf(stderr, "sos: %s\n", on ? "on (pretend: the countdown, then the beacon)" : "off");
    if (!!on != hsos) hsos_t0 = surv_host_now_ms;
    hsos = !!on;
    return 0;
}
int sh_sos_frame(uint32_t *argb)
{
    if (!hsos) return 0;
    uint64_t e = surv_host_now_ms - hsos_t0;
    if (e < 3000) { *argb = 0xff1c0606U; return 1; }
    e -= 3000;
    unsigned b = surv_sos_opts >> SURV_BEACON_SHIFT & 3U;
    int ph = (int)(e / 250 & 1);
    *argb = b == 1 ? (ph ? 0xff000000U : 0xffffffffU) : b == 2 ? (ph ? 0xff000000U : 0xffff1a1aU) :
            b == 3 ? (surv_morse_at((uint32_t)e, 0) ? 0xffffffffU : 0xff000000U) : (ph ? 0xffff1a1aU : 0xffffffffU);
    return 1;
}
int sh_sos_secs(void)
{
    uint64_t e = surv_host_now_ms - hsos_t0;
    return hsos && e < 3000 ? (int)((3000 - e + 999) / 1000) : 0;
}
unsigned sh_sos_opts(void) { return surv_sos_opts; }
void sh_sos_opts_set(unsigned o) { surv_sos_opts = o; fprintf(stderr, "sos: options %#x\n", o); }
int sh_usb_start(void)
{
    int r = surv_usb_start();
    fprintf(stderr, "usb power: %s\n", r ? "refused" : "on (pretend: 10 s)");
    return r;
}
void sh_usb_stop(void) { fprintf(stderr, "usb power: stop\n"); surv_usb_stop("Stop on the screen"); }
void sh_usb(sh_usb_t *u)
{
    struct surv_usb s;
    surv_usb_status(&s);
    u->state = s.state == SURV_USB_ON ? SH_USB_ON : s.state == SURV_USB_COOL ? SH_USB_COOL : SH_USB_OFF;
    u->ms_left = s.ms_left, u->est_ma = s.est_ma, u->vbus_lo_mv = s.vbus_lo_mv, u->vbus_hi_mv = s.vbus_hi_mv;
    u->bat_c10 = s.bat_c10, u->limit_c10 = s.limit_c10, u->ilim_ma = s.ilim_ma, u->refused = s.refused, u->why = s.why, u->events = s.events;
}
const char *sh_usb_guide(int i)
{
    for (int k = 0; k <= i; k++) if (!surv_usb_guide[k]) return 0;
    return surv_usb_guide[i];
}
int sh_sun(int *rise, int *set) { return surv_sun_today(rise, set); }
void sh_loc(int *lat, int *lon) { *lat = surv_lat_x100; *lon = surv_lon_x100; }
void sh_loc_set(int lat, int lon) { surv_lat_x100 = lat, surv_lon_x100 = lon; fprintf(stderr, "sun: location %d %d\n", lat, lon); }
int sh_card(sh_card_t *c) { struct surv_card k; int r = surv_card_get(&k); memcpy(c, &k, sizeof k); return r; }
int sh_card_set(const sh_card_t *c) { struct surv_card k; memcpy(&k, c, sizeof k); fprintf(stderr, "card: saved\n"); return surv_card_set(&k); }
const char *sh_card_label(int i) { return i >= 0 && i < SURV_CARD_NF ? surv_card_label[i] : ""; }
int sh_last2(void) { return surv_last2(); }
int sh_sos_next(void) { return 50; }      /* the host: the old 20 Hz poll */
/* Track A: the passcode on kern/passcode.c itself (in RAM, on the film's clock: surv_host_now_ms; no wall clock),
 * Auto-Lock a number. States: pass=DIGITS sets a passcode, passfail=:N makes N wrong tries now */
#include "../../kern/passcode.h"
static int host_autolock = 30;
/* Track A update: what the host's card holds (hupd=; the fakes are near the end) */
static int hupd_mode;                       /* 0 none, 1 found, 2 bad, 3 old loader, 4 installed */
static struct sh_update HU;
int sh_pass_on(void) { return pass_len() > 0; }
int sh_pass_len(void) { return pass_len(); }
int sh_pass_wait(void) { return (int)pass_wait(surv_host_now_ms); }
int sh_pass_fails(void) { return pass_fails(); }
int sh_pass_check(const char *d)
{
    int r = pass_check(d, surv_host_now_ms, 0);
    if (r == 1) fsvc_passcode(d);
    fprintf(stderr, "passcode: %s (wrong tries in a row %d, wait %u s)\n", r == 1 ? "right" : r == 0 ? "wrong" : "refused: a wait runs",
            pass_fails(), pass_wait(surv_host_now_ms));
    return r;
}
int sh_pass_set(const char *old, const char *nw)
{
    uint8_t salt[PASS_SALT];
    for (unsigned i = 0; i < PASS_SALT; i++) salt[i] = (uint8_t)rand();
    int r = pass_set(old, nw, salt, surv_host_now_ms, 0);
    if (!r) fsvc_passcode_set(old, nw);
    fprintf(stderr, "passcode: %s\n", r ? "not changed" : pass_len() ? "set" : "removed");
    return r;
}
int sh_autolock(void) { return host_autolock; }
void sh_autolock_set(int s) { host_autolock = s; fprintf(stderr, "auto-lock: %d s (0 never)\n", s); }
const char *sh_last2_why(void) { return surv_last2_why(); }
int sh_last2_set(int on) { fprintf(stderr, "last 2 %%: %s\n", on ? "on" : "off"); return surv_last2_set(on, "the switch in Emergency"); }
int sh_last2_auto(void) { return surv_last2_auto; }
void sh_last2_auto_set(int on) { surv_last2_auto = !!on; }
int sh_guide_n(void) { int n = 0; (void)surv_guide(&n); return n; }
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
/* helper J: the Compass page's hub (survival_host.c: 4 s starting, then a slowly turning heading) */
int sh_sensors_on(int on)
{
    static int was;
    int r = surv_sensors_on(on);
    if (!!on != was) fprintf(stderr, "sensors: %s\n", on ? (r < 0 ? "refused" : "start (pretend: 4 s)") : "stop");
    was = on && r >= 0;
    return r;
}
int sh_sensors_state(void) { return surv_sensors_state(); }
int sh_heading(int *deg_x10, int *accuracy) { return surv_sensors_state() == 2 ? surv_heading(deg_x10, accuracy) : 0; }
static int host_tilt, host_p10, host_r10;       /* a state's tilt (tilt=P,R) instead of the simulation's */
int sh_tilt(int *pitch_x10, int *roll_x10)
{
    if (surv_sensors_state() != 2) return 0;
    if (host_tilt) { *pitch_x10 = host_p10; *roll_x10 = host_r10; return 1; }
    return surv_tilt(pitch_x10, roll_x10);
}
int sh_steps(unsigned *n) { uint32_t v = 0; int r = surv_sensors_state() == 2 && surv_steps(&v); *n = v; return r; }
long long sh_utc(void) { return 1794500000LL + 25 * 60 + (long long)(surv_host_now_ms / 1000); }   /* the pretend fix: 25 min old */
/* Track A: the time zones on kern/tzdb.c itself (linked in), 12 / 24-hour and four alarms in RAM (two made up). The
 * host's sh_status stays the concept's 21:05 whatever the zone */
#include "tzdb.h"
#include "tzcities.h"
static int host_tz = -1, host_24 = 1;
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
static int host_usb;
void sh_usb_console(int on) { host_usb = !!on; }
int sh_usb_console_on(void) { return host_usb; }
int sh_tz_count(void) { return tzdb_n; }
const char *sh_tz_name(int i, const char **city)
{
    if (i < 0 || i >= tzdb_n) { if (city) *city = ""; return ""; }
    if (city) *city = tzdb[i].city;
    return tzdb[i].name;
}
int sh_tz_offset_min(int i) { return i >= 0 && i < tzdb_n ? tzdb_offset(i, sh_utc()) : 0; }
int sh_tz(void) { return host_tz >= 0 ? host_tz : tzdb_home(); }
void sh_tz_set(int i) { if (i >= 0 && i < tzdb_n) { host_tz = i; fprintf(stderr, "time zone: %s\n", tzdb[i].name); } }
int sh_clock24(void) { return host_24; }
void sh_clock24_set(int on) { host_24 = !!on; fprintf(stderr, "clock: %s\n", on ? "24-hour" : "12-hour"); }
static struct { int hm, days, on; char label[20]; } HA[4] = { { 700, 0x3e, 1, "Alarm" }, { 830, 0x41, 0, "Gym" } };
static int nha = 2;
static void host_alarm_push(void)
{
    int best = -1;
    for (int i = 0; i < nha; i++) if (HA[i].on && (best < 0 || HA[i].hm < HA[best].hm)) best = i;
    sh_alarm(best >= 0, best >= 0 ? HA[best].hm / 100 : 0, best >= 0 ? HA[best].hm % 100 : 0);
}
int sh_alarm_count(void) { return nha; }
int sh_alarm_get(int i, int *hm, int *days, int *on, char *label, int cap)
{
    if (i < 0 || i >= nha) return -1;
    if (hm) *hm = HA[i].hm;
    if (days) *days = HA[i].days;
    if (on) *on = HA[i].on;
    if (label && cap > 0) snprintf(label, (size_t)cap, "%s", HA[i].label);
    return 0;
}
int sh_alarm_put(int i, int hm, int days, int on, const char *label)
{
    if (i < 0 || i > nha || i >= 4 || hm < 0 || hm / 100 > 23 || hm % 100 > 59 || days < 0 || days > 0x7f) return -1;
    if (i == nha) nha++;
    HA[i].hm = hm; HA[i].days = days; HA[i].on = !!on;
    snprintf(HA[i].label, sizeof HA[i].label, "%s", label ? label : "");
    fprintf(stderr, "alarm %d: %02d:%02d days 0x%02x %s\n", i, hm / 100, hm % 100, days, on ? "on" : "off");
    host_alarm_push();
    return 0;
}
int sh_alarm_del(int i)
{
    if (i < 0 || i >= nha) return -1;
    for (int k = i; k + 1 < nha; k++) HA[k] = HA[k + 1];
    nha--;
    fprintf(stderr, "alarm %d: deleted\n", i);
    host_alarm_push();
    return 0;
}
/* helper D: Maps' last GNSS fix: none unless a state gives one (gnss=LAT,LON in degrees: a pretend fix from Android) */
static sh_gnss_t host_fix;
static int host_have_fix;
int sh_gnss(sh_gnss_t *f)
{
    if (!host_have_fix) return 0;
    *f = host_fix;
    return 1;
}

/* SBTK_CORES=n: the bands over n threads (the phone's four A55), to check that a split frame is bit for bit the
 * serial one and to time it */
#include <pthread.h>
static struct { void (*fn)(void *, int); void *arg; int n, next; pthread_mutex_t m; } PJ = { 0, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER };
static void *pworker(void *x)
{
    (void)x;
    for (;;) {
        pthread_mutex_lock(&PJ.m);
        int i = PJ.next++;
        pthread_mutex_unlock(&PJ.m);
        if (i >= PJ.n) return 0;
        PJ.fn(PJ.arg, i);
    }
}
static int ncores = 1;
static void host_par(void (*fn)(void *, int), void *arg, int n)
{
    PJ.fn = fn; PJ.arg = arg; PJ.n = n; PJ.next = 0;
    pthread_t t[8];
    for (int k = 0; k < ncores - 1; k++) pthread_create(&t[k], 0, pworker, 0);
    pworker(0);
    for (int k = 0; k < ncores - 1; k++) pthread_join(t[k], 0);
}

static uint8_t *readall(const char *p, uint32_t *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = tk_alloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { perror(p); exit(1); }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

static void write_ppm(const char *path, const tk_bmp *b)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", b->w, b->h);
    uint8_t *row = malloc((size_t)b->w * 3);
    for (int j = 0; j < b->h; j++) {
        for (int i = 0; i < b->w; i++) {
            uint32_t p = b->px[j * b->stride + i];
            row[3 * i] = p >> 16 & 255; row[3 * i + 1] = p >> 8 & 255; row[3 * i + 2] = p & 255;
        }
        fwrite(row, 1, (size_t)b->w * 3, f);
    }
    free(row);
    fclose(f);
}

static void apply_state(const char *st)
{
    char what[32] = { 0 }, key[256] = { 0 };
    float v = 1;
    const char *eq = strchr(st, '=');
    if (!eq) return;
    if (!strncmp(st, "lx=", 3)) { lin_fake_state(st + 3); return; }   /* L3: the pretend Linux's own states */
    memcpy(what, st, (size_t)(eq - st));
    const char *colon = strchr(eq + 1, ':');
    if (colon) { memcpy(key, eq + 1, (size_t)(colon - eq - 1 < 255 ? colon - eq - 1 : 255)); v = (float)atof(colon + 1); }
    else snprintf(key, sizeof key, "%s", eq + 1);   /* helper D: a long notification text fits */
    /* date=WDAY:DAY (P3p): a new day as the phone's clock reports it (the Calendar icon follows) */
    if (!strcmp(what, "calls")) { sh_cell_calls((int)v); return; }   /* 8z: calls=:1 "Calls and SMS" on (pretend registered) */
    if (!strcmp(what, "chg")) {                     /* 9c: chg=:MA charging at MA mA (chg=:-1 hot, :-2 full, :0 unplugged) */
        int st = v == 0 ? 0 : v == -1 ? 2 : v == -2 ? 4 : 1;
        sh_charge((int)v, 3000, st);
        sh_sysinfo(st != 0, "Starbite OS Next");
        return;
    }
    if (!strcmp(what, "batt")) { sh_status(21, 6, 2, 10, 11, (int)v, 41); return; }   /* 8x: batt=:N a new charge (the widgets' minute rebuild) */
    if (!strcmp(what, "date")) { sh_status(21, 6, atoi(key), (int)v, 11, 87, 41); return; }
    if (!strcmp(what, "hostms")) { surv_host_now_ms = (uint64_t)v; return; }   /* survival-next: the simulation's clock */
    if (!strcmp(what, "last2")) { surv_last2_set(v > 0, "a state"); return; }   /* helper J: last2=:1 (the Compass refused) */
    if (!strcmp(what, "tilt")) {                 /* helper J: tilt=PITCH,ROLL in degrees (the Level's stills: upright, level) */
        double p = 0, r = 0;
        host_tilt = sscanf(eq + 1, "%lf,%lf", &p, &r) == 2;
        host_p10 = (int)(p * 10); host_r10 = (int)(r * 10);
        return;
    }
    if (!strcmp(what, "tap")) {                  /* Track J: tap=X,Y (pt): a tap through the gestures, then settled */
        static float tt = 1000;                  /* the stills' own clock, ms */
        float x = 0, y = 0;
        if (sscanf(eq + 1, "%f,%f", &x, &y) != 2) return;
        sh_touch(0, x, y, tt);
        sh_tick(1 / 60.0f, tt += 1000 / 60.0f);
        sh_touch(2, x, y, tt += 64);
        for (int i = 0; i < 600 && (i < 30 || sh_moving()); i++) sh_tick(1 / 60.0f, tt += 1000 / 60.0f);
        return;
    }
    if (!strcmp(what, "wifistate")) {            /* cp6: wifistate=:N (SH_WIFI_*: 1 starting, 3 scanning, 4 joining) */
        HW.state = (int)v;
        hw_push();
        return;
    }
    if (!strcmp(what, "down")) {                 /* cp6: down=X,Y (pt): a finger put down and held there (the press's still) */
        static float td = 500000;
        float x = 0, y = 0;
        if (sscanf(eq + 1, "%f,%f", &x, &y) != 2) return;
        sh_touch(0, x, y, td);
        for (int i = 0; i < 6; i++) sh_tick(1 / 60.0f, td += 1000 / 60.0f);
        return;
    }
    if (!strcmp(what, "comms")) { comms_fake_state(key, v); return; }          /* helper I: comms=incoming, ... */
    if (!strcmp(what, "pass")) { (void)sh_pass_set("", key); return; }         /* Track A: pass=246810 */
    if (!strcmp(what, "faceset")) { fsvc_host_enrolled(); return; }            /* Face Unlock: faceset=: set up */
    if (!strcmp(what, "hupd")) {                 /* Track A update: hupd=none|found|bad|old|installed (the host's card) */
        static const char *const M[5] = { "none", "found", "bad", "old", "installed" };
        for (int i = 0; i < 5; i++) if (!strcmp(key, M[i])) hupd_mode = i;
        memset(&HU, 0, sizeof HU);
        return;
    }
    if (!strcmp(what, "passfail")) { for (int i = 0; i < (int)v; i++) (void)sh_pass_check("x"); return; }   /* passfail=:6 */
    if (!strcmp(what, "gnss")) {                 /* helper D: gnss=60.1699,24.9384 (a pretend fix: 8.5 m, 9 satellites) */
        double la = 0, lo = 0;
        if (sscanf(eq + 1, "%lf,%lf", &la, &lo) == 2) {
            host_fix.lat_e6 = (int)(la * 1e6); host_fix.lon_e6 = (int)(lo * 1e6);
            host_fix.acc_dm = 85; host_fix.sats = 9; host_fix.utc = 1794500000LL;
            strcpy(host_fix.src, "gps");
            host_have_fix = 1;
        }
        return;
    }
    sh_set(what, key, v);
}

/* ---- the film ------------------------------------------------------------------------------------------- */
typedef struct { int kind; float x0, y0, x1, y1, ms, dms; char txt[120]; } step;   /* dms: dragh's drag (then held) */
static int host_ori_film = -1;          /* cp7: a film's "ori N" (the pretend accelerometer), -1 none */
static int parse(const char *s, step *st, int max)
{
    int n = 0;
    char buf[8192];
    strncpy(buf, s, sizeof buf - 1);
    for (char *tok = strtok(buf, ";"); tok && n < max; tok = strtok(0, ";")) {
        step *p = &st[n];
        memset(p, 0, sizeof *p);
        char w[16];
        if (sscanf(tok, " %15s", w) != 1) continue;
        if (!strcmp(w, "wait")) { p->kind = 0; sscanf(tok, " %*s %f", &p->ms); }
        else if (!strcmp(w, "tap")) {        /* cp6: "tap X Y [MS]": MS the finger's time down (80); under the touch events' step
                                              * (8.3 ms: "tap X Y 1") the down and the up come in the same event, before one frame */
            float m = 0;
            p->kind = 1;
            p->ms = sscanf(tok, " %*s %f %f %f", &p->x0, &p->y0, &m) == 3 && m >= 1 ? m : 80;
        }
        else if (!strcmp(w, "drag")) { p->kind = 2; sscanf(tok, " %*s %f %f %f %f %f", &p->x0, &p->y0, &p->x1, &p->y1, &p->ms); }
        else if (!strcmp(w, "hold")) { p->kind = 3; sscanf(tok, " %*s %f %f %f", &p->x0, &p->y0, &p->ms); }
        else if (!strcmp(w, "pinch")) {     /* 8b: "pinch CX CY D0 D1 MS": two fingers on a level line round (CX, CY), D0 apart
                                             * to D1 apart over MS: finger 0 through sh_touch, finger 1 sh_touch_finger */
            p->kind = 13;
            sscanf(tok, " %*s %f %f %f %f %f", &p->x0, &p->y0, &p->x1, &p->y1, &p->ms);
        }
        else if (!strcmp(w, "state") || !strcmp(w, "sms")) {   /* helper I: a state now; an SMS received now */
            p->kind = w[1] == 't' ? 4 : 9;
            p->ms = 20;
            const char *a = strstr(tok, w) + strlen(w);
            while (*a == ' ') a++;
            snprintf(p->txt, sizeof p->txt, "%s", a);
        } else if (!strcmp(w, "ring")) { p->kind = 8; p->ms = 20; sscanf(tok, " %*s %f", &p->x0); }   /* helper I */
        else if (!strcmp(w, "power")) { p->kind = 5; p->ms = 20; }      /* the Power key: sleep, or wake if off */
        else if (!strcmp(w, "powerhold")) { p->kind = 10; p->ms = 20; }   /* P3x: the Power key held 1 s: the power sheet */
        else if (!strcmp(w, "ori")) { p->kind = 11; p->ms = 20; sscanf(tok, " %*s %f", &p->x0); }
        else if (!strcmp(w, "keys")) {      /* cp7: "keys TEXT": a Bluetooth keyboard types TEXT now ('|' Return, '<' Delete) */
            p->kind = 12; p->ms = 20;
            const char *a = strstr(tok, "keys") + 4;
            while (*a == ' ') a++;
            snprintf(p->txt, sizeof p->txt, "%s", a);
        }   /* cp7: the phone turned (0..3) */
        else if (!strcmp(w, "notify")) {     /* helper D: "notify WHO|TITLE|BODY": a pretend notification from the platform */
            p->kind = 6; p->ms = 20;
            const char *a = strstr(tok, "notify") + 6;
            while (*a == ' ') a++;
            snprintf(p->txt, sizeof p->txt, "%s", a);
        } else if (!strcmp(w, "dragh")) {   /* helper D: "dragh X0 Y0 X1 Y1 MS HOLD": a drag, then the finger held still */
            float hold = 0;
            p->kind = 7;
            sscanf(tok, " %*s %f %f %f %f %f %f", &p->x0, &p->y0, &p->x1, &p->y1, &p->dms, &hold);
            p->ms = p->dms + hold;
        }
        n++;
    }
    return n;
}

static double memwalk(tk_layer *l, int depth);
void host_memdump(const char *tag);
/* the calling thread's CPU time, ms (SBTK_CPU: the shell's work in a frame, without a loaded Mac's waits) */
static double cpu_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
/* 8c: the phone's "touch to frame" on the host (SBTK_TTF=1): a touch's down or up to the next frame that drew, said
 * when over 50 ms (a touch whose first frame shows nothing: no pressed state, or a stall); the film's touches only */
static float host_tp = -1;
static void ht_mark(int ph, float t) { if (ph != 1 && host_tp < 0) host_tp = t; }
#define sh_touch(p_, x_, y_, t_) (ht_mark((p_), (t_)), sh_touch((p_), (x_), (y_), (t_)))
static int film(tk_comp *c, const char *out, const char *script, int half)
{
    step st[192];               /* survival-next: the survival film is long */
    int n = parse(script, st, 192);
    float total = 0;
    for (int i = 0; i < n; i++) total += st[i].ms;
    total += 1500;     /* let everything settle */
    int none = !strcmp(out, "none");
    FILE *f = none ? 0 : !strcmp(out, "-") ? stdout : fopen(out, "wb");
    FILE *log = f == stdout ? stderr : stdout;
    tk_bmp h = { 0 };
    if (!none) tk_bmp_new(&h, TK_W / 2, TK_H / 2);
    const float dt = 1000.0f / 60;
    float t = 0, s0 = 0;
    int si = 0, phase = -1, frame = 0, rest_frames = 0;
    tk_comp_frame(c, 0, 1);
    sh_idle();
    while (t < total || sh_moving()) {
        /* the finger: events at 120 Hz between frames (the touch controller's rate) */
        uint64_t sh0 = tk_us();          /* P3i: the shell's own time in the frame (touch handling + tick) */
        double cpu0 = cpu_ms();
        for (int sub = 1; sub <= 2; sub++) {
            float te = t + dt * sub / 2;
            while (si < n && te >= s0 + st[si].ms) {
                if (phase == -1 && st[si].kind == 1) {   /* cp6: a tap shorter than the touch events' step: down and up now */
                    sh_touch(0, st[si].x0, st[si].y0, te);
                    sh_touch(2, st[si].x0, st[si].y0, te);
                }
                if (phase >= 0 && st[si].kind == 13) {   /* 8b: the pinch's two fingers up (the second first) */
                    step *p = &st[si];
                    sh_touch_finger(1, 2, p->x0 + p->y1 / 2, p->y0, s0 + p->ms);
                    sh_touch(2, p->x0 - p->y1 / 2, p->y0, s0 + p->ms);
                }
                if (phase >= 0 && st[si].kind && st[si].kind != 4 && st[si].kind != 5 && st[si].kind != 6 && st[si].kind < 8) {
                    step *p = &st[si];
                    float x = p->kind == 2 || p->kind == 7 ? p->x1 : p->x0, y = p->kind == 2 || p->kind == 7 ? p->y1 : p->y0;
                    sh_touch(2, x, y, s0 + p->ms);
                }
                phase = -1;
                s0 += st[si].ms;
                si++;
            }
            if (si >= n) break;
            step *p = &st[si];
            if (!p->kind) continue;
            float u = (te - s0) / p->ms;
            if (p->kind == 6) {
                if (phase < 0) { fprintf(stderr, "notify at %.0f ms: %s\n", te, p->txt); sh_set("notify", p->txt, 0); phase = 0; }
                continue;
            }
            if (p->kind == 4 || p->kind == 8 || p->kind == 9) {     /* helper I: a state, a ring, an SMS (once) */
                if (phase < 0) {
                    fc_now = te;
                    if (p->kind == 4) { fprintf(stderr, "state at %.0f ms: %s\n", te, p->txt); apply_state(p->txt); }
                    else if (p->kind == 8) { fprintf(stderr, "ring at %.0f ms: in %.0f ms\n", te, p->x0); comms_fake_ring(p->x0); }
                    else { fprintf(stderr, "sms at %.0f ms (the text is not logged)\n", te); comms_fake_sms(p->txt); }
                    phase = 0;
                }
                continue;
            }
            if (p->kind == 12) {
                if (phase < 0) {
                    for (const char *c = p->txt; *c; c++) {
                        uint32_t u = *c == '|' ? '\n' : *c == '<' ? 0x08 : (uint32_t)(unsigned char)*c;
                        sh_hw_key(0, 1, u);
                        sh_hw_key(0, 0, u);
                    }
                    phase = 0;
                }
                continue;
            }
            if (p->kind == 11) {
                if (phase < 0) { host_ori_film = (int)p->x0 & 3; phase = 0; }
                continue;
            }
            if (p->kind == 13) {                 /* 8b: the pinch */
                float d = p->x1 + (p->y1 - p->x1) * (u > 1 ? 1 : u);
                if (phase < 0) { sh_touch(0, p->x0 - d / 2, p->y0, te); sh_touch_finger(1, 0, p->x0 + d / 2, p->y0, te); phase = 0; }
                else { sh_touch(1, p->x0 - d / 2, p->y0, te); sh_touch_finger(1, 1, p->x0 + d / 2, p->y0, te); }
                continue;
            }
            if (p->kind == 10) {
                if (phase < 0) { fprintf(stderr, "power held at %.0f ms\n", te); sh_power_held(); phase = 0; }
                continue;
            }
            if (p->kind == 5) {
                if (phase < 0) {
                    fprintf(stderr, "power at %.0f ms: %s\n", te, screen_off || sh_sleeping() ? "wake" : "sleep");
                    if (screen_off || sh_sleeping()) { screen_off = 0; sh_wake(); } else sh_sleep(0);
                    phase = 0;
                }
                continue;
            }
            if (phase < 0 && p->kind == 1 && p->ms < dt / 2) {   /* cp6: the quick tap's down and up in this one event */
                if (phase == -1) { sh_touch(0, p->x0, p->y0, te); sh_touch(2, p->x0, p->y0, te); }
                phase = -2;
                continue;
            }
            if (phase < 0) { sh_touch(0, p->x0, p->y0, te); phase = 0; continue; }
            if (p->kind == 7) {
                float e = (te - s0) / p->dms;
                if (e > 1) e = 1;
                sh_touch(1, p->x0 + (p->x1 - p->x0) * e, p->y0 + (p->y1 - p->y0) * e, te);
            }
            if (p->kind == 2) {
                float e = u < 1 ? u : 1;
                e = 1 - (1 - e) * (1 - e) * .3f - (1 - e) * .7f;     /* a finger: fast start, slight ease */
                e = u;
                sh_touch(1, p->x0 + (p->x1 - p->x0) * e, p->y0 + (p->y1 - p->y0) * e, te);
            }
        }
        lin_fake_poll(t);                     /* L3: the pretend Linux (gwin_now's part on the phone) */
        comms_fake_poll(t);                   /* helper I: the pretend network (comms_now's part on the phone) */
        surv_host_now_ms = (uint64_t)t;          /* survival-next: the simulated USB burst, SOS and the clock */
        if (fsvc_busy()) fsvc_tick();            /* Face Unlock (the phone: its own timer) */
        int mv = sh_tick(dt / 1000, t);
        double shell_ms = (tk_us() - sh0) / 1000.0, shell_cpu = cpu_ms() - cpu0;
        int motion = mv || sh_moving();
        tk_comp_frame(c, motion, 0);
        if (host_tp >= 0 && c->last_px) { if (t + dt - host_tp > 50 && getenv("SBTK_TTF")) fprintf(stderr, "TTF %.0f ms (touch at %.0f)\n", t + dt - host_tp, host_tp); host_tp = -1; }
        fprintf(log, "F %d %.1f %s px %d comp %.2f present %.2f total %.2f", frame, t, c->last_half ? "half" : "full",
               c->last_px, c->us_comp / 1000.0, c->us_present / 1000.0, c->last_us / 1000.0);
        fprintf(log, " idle %.2f halfs %.2f shell %.2f", c->us_idle / 1000.0, c->us_halfs / 1000.0, shell_ms);
        if (getenv("SBTK_CPU")) fprintf(log, " cpu %.2f", shell_cpu);   /* the shell's CPU time in the frame */
        if (getenv("SBTK_HASH")) {             /* what the display shows now (two buffers: the one just presented) */
            uint64_t hh = 1469598103934665603ULL;
            for (size_t i = 0; i < (size_t)TK_W * TK_H; i++) hh = (hh ^ (c->panel.px[i] & 0xffffffu)) * 1099511628211ULL;   /* what shows: no alpha */
            fprintf(log, " hash %016llx", (unsigned long long)hh);
        }
        fputc('\n', log);
        if (getenv("SBTK_DUMP") && frame == atoi(getenv("SBTK_DUMP"))) write_ppm(getenv("SBTK_DUMPTO"), &c->panel);
        if (getenv("SBTK_DUMPS")) {                   /* B5: "N,M,...": frames N, M ... to SBTK_DUMPTO-N.ppm */
            for (const char *q = getenv("SBTK_DUMPS"); *q; q++)
                if ((q == getenv("SBTK_DUMPS") || q[-1] == ',') && atoi(q) == frame) {
                    char fn[600];
                    snprintf(fn, sizeof fn, "%s-%d.ppm", getenv("SBTK_DUMPTO"), frame);
                    write_ppm(fn, &c->panel);
                }
        }
        if (none) ;
        else if (half) {
            tk_bmp_down2(&h, &c->panel);
            fwrite(h.px, 4, (size_t)h.w * h.h, f);
        } else
            fwrite(c->panel.px, 4, (size_t)TK_W * TK_H, f);
        t += dt;
        frame++;
        rest_frames = motion ? 0 : rest_frames + 1;
        if (getenv("SBTK_CHECK") && rest_frames == 4) {
            /* P3s: at rest a while: everything composed again at full resolution, its hash (what the screen should
             * show, without the damage borders a motion frame's present leaves) */
            tk_comp_frame(c, 0, 1);
            uint64_t hh = 1469598103934665603ULL;
            for (size_t i = 0; i < (size_t)TK_W * TK_H; i++) hh = (hh ^ (c->panel.px[i] & 0xffffffu)) * 1099511628211ULL;
            fprintf(log, "R %d %.1f hash %016llx\n", frame, t, (unsigned long long)hh);
            if (getenv("SBTK_DUMPR") && frame == atoi(getenv("SBTK_DUMPR"))) write_ppm(getenv("SBTK_DUMPTO"), &c->panel);
        }
        if (!motion) {               /* between frames: on the phone, the time left in the frame */
            uint64_t i0 = tk_us();
            sh_idle();
            if (getenv("SBTK_IDLEMS") && tk_us() - i0 > 1000) fprintf(stderr, "IDLE %d %.1f ms\n", frame, (tk_us() - i0) / 1000.0);   /* cp7c */
        }
        /* the phone's refresh timer (the Clock's digits, the card's work a turn at a time): a frame if it drew */
        if (!motion && sh_next_update(t) >= 0 && sh_refresh(t)) tk_comp_frame(c, 0, 0);
        if (t > total + 8000) break;
    }
    (void)rest_frames;
    tk_bmp_free(&h);
    if (none) ;
    else if (f != stdout) fclose(f); else fflush(f);
    if (getenv("SBTK_MEM")) fprintf(stderr, "mem: layers %.1f MB\n", memwalk(c->root, 0) / 1e6);
    host_memdump("film");
    fprintf(stderr, "film: %d frames; heap %.1f MB (peak %.1f MB)\n", frame, tk_heap_used() / 1e6, tk_heap_peak() / 1e6);
    return 0;
}

/* ---- uibench's kernels on the host: the same NEON as kern/uibench.c, one core --------------------------- */
#include <arm_neon.h>
static void blend_rows(uint8_t *d, const uint8_t *s, uint32_t w, uint32_t y0, uint32_t y1)
{
    for (uint32_t y = y0; y < y1; ++y) {
        const uint8_t *a = s + (size_t)y * w * 4;
        uint8_t *b = d + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x += 8) {
            uint8x8x4_t sp = vld4_u8(a + x * 4), dp = vld4_u8(b + x * 4);
            uint8x8_t ia = vmvn_u8(sp.val[3]);
            for (int c = 0; c < 4; ++c) {
                uint16x8_t m = vmull_u8(dp.val[c], ia);
                uint8x8_t r = vrshrn_n_u16(vrsraq_n_u16(m, m, 8), 8);
                dp.val[c] = vqadd_u8(sp.val[c], r);
            }
            vst4_u8(b + x * 4, dp);
        }
    }
}
static void up2x_rows(uint8_t *d, const uint8_t *s, uint32_t w, uint32_t y0, uint32_t y1)
{
    uint32_t sw = w / 2, sh = 2960 / 2;
    for (uint32_t y = y0 & ~1U; y < y1; y += 2) {
        uint32_t sy = y / 2, sy1 = sy + 1 < sh ? sy + 1 : sy;
        const uint8_t *r0 = s + (size_t)sy * sw * 4, *r1 = s + (size_t)sy1 * sw * 4;
        uint8_t *o0 = d + (size_t)y * w * 4, *o1 = o0 + (size_t)w * 4;
        for (uint32_t x = 0; x + 4 < sw; x += 4) {
            uint8x16_t a = vld1q_u8(r0 + x * 4), an = vld1q_u8(r0 + x * 4 + 4);
            uint8x16_t b = vld1q_u8(r1 + x * 4), bn = vld1q_u8(r1 + x * 4 + 4);
            uint8x16_t ah = vrhaddq_u8(a, an), bh = vrhaddq_u8(b, bn);
            uint32x4x2_t top = vzipq_u32(vreinterpretq_u32_u8(a), vreinterpretq_u32_u8(ah));
            uint8x16_t va = vrhaddq_u8(a, b), vh = vrhaddq_u8(ah, bh);
            uint32x4x2_t mid = vzipq_u32(vreinterpretq_u32_u8(va), vreinterpretq_u32_u8(vh));
            vst1q_u32((uint32_t *)(o0 + x * 8), top.val[0]);
            vst1q_u32((uint32_t *)(o0 + x * 8 + 16), top.val[1]);
            if (y + 1 < y1) { vst1q_u32((uint32_t *)(o1 + x * 8), mid.val[0]); vst1q_u32((uint32_t *)(o1 + x * 8 + 16), mid.val[1]); }
        }
    }
}
static void bench(void)
{
    const uint32_t FW = 1440, FH = 2960, HW = 720, HH = 1480;
    uint8_t *f0 = tk_alloc((size_t)FW * FH * 4), *f1 = tk_alloc((size_t)FW * FH * 4);
    uint8_t *h0 = tk_alloc((size_t)HW * HH * 4), *h1 = tk_alloc((size_t)HW * HH * 4);
    for (size_t i = 0; i < (size_t)HW * HH; i++) { uint32_t a = 128 + (uint32_t)(i % 128); ((uint32_t *)h1)[i] = a << 24 | (a / 2) << 16 | (a / 3) << 8 | (a / 4); }
    for (size_t i = 0; i < (size_t)FW * FH; i++) ((uint32_t *)f1)[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
    struct { const char *n; int k; } K[] = { { "fill, a full frame in RAM", 0 }, { "copy, a full frame RAM to RAM", 1 },
        { "blend, a half-resolution layer over another", 2 }, { "blend, a full-resolution layer over another", 3 },
        { "2x upscale, half frame to a full frame in RAM", 4 } };
    for (int k = 0; k < 5; k++) {
        uint64_t best = ~0ull, bytes = 0;
        for (int r = 0; r < 5; r++) {
            uint64_t t0 = tk_us();
            switch (K[k].k) {
            case 0: { uint32x4_t v = vdupq_n_u32(0xff203040u); for (size_t i = 0; i < (size_t)FW * FH; i += 4) vst1q_u32((uint32_t *)f0 + i, v); bytes = (uint64_t)FW * FH * 4; break; }
            case 1: memcpy(f0, f1, (size_t)FW * FH * 4); bytes = (uint64_t)FW * FH * 8; break;
            case 2: blend_rows(h0, h1, HW, 0, HH); bytes = (uint64_t)HW * HH * 12; break;
            case 3: blend_rows(f0, f1, FW, 0, FH); bytes = (uint64_t)FW * FH * 12; break;
            case 4: up2x_rows(f0, h0, FW, 0, FH); bytes = (uint64_t)FW * FH * 4 + (uint64_t)HW * HH * 4; break;
            }
            uint64_t t = tk_us() - t0;
            if (t < best) best = t;
        }
        printf("hostbench: %s: %llu.%llu ms, %llu MB/s\n", K[k].n, (unsigned long long)(best / 1000), (unsigned long long)(best % 1000 / 100),
               (unsigned long long)(bytes / (best ? best : 1)));
    }
}

static double memwalk(tk_layer *l, int depth)
{
    double t = 0;
    for (; l; l = l->next) {
        double b = (double)l->bmp.w * l->bmp.h * 4, h = (double)l->half.w * l->half.h * 4;
        double g = (double)l->glass.w * l->glass.h + (double)l->ghalf.w * l->ghalf.h;
        if (b + h + g > 2e5) {
            int x0 = l->bmp.w, y0 = l->bmp.h, x1 = 0, y1 = 0;
            for (int j = 0; j < l->bmp.h; j++) for (int i = 0; i < l->bmp.w; i++)
                if (l->bmp.px[(size_t)j * l->bmp.stride + i] || (l->glass.v && l->glass.v[(size_t)j * l->glass.w + i])) {
                    if (i < x0) x0 = i; if (i >= x1) x1 = i + 1; if (j < y0) y0 = j; if (j >= y1) y1 = j + 1; }
            double used = x1 > x0 ? (double)(x1 - x0) * (y1 - y0) * 4 : 0;
            fprintf(stderr, "mem %*s%-14s %6.1f MB (half %.1f, glass %.1f)%s %dx%d bounds %d,%d..%d,%d trim-> %.1f MB\n", depth * 2, "", l->name, (b + h + g) / 1e6, h / 1e6, g / 1e6, l->hidden ? " hidden" : "",
                    l->bmp.w, l->bmp.h, x0, y0, x1, y1, used / 1e6);
        }
        t += b + h + g + memwalk(l->child, depth + 1);
    }
    return t;
}

/* SBTK_DBL=2: two buffers in turn; 3: three, as the phone (P2t): the display shows `shown`, the last flip `req`
 * latches unless the frame number says it missed a TE (every 5th, every 7th: a pattern), the free one is the oldest
 * that is neither */
static uint32_t *dbl[3];
static int nbuf, shown, req = 0, bfno[3] = { 0, -1, -1 }, fno_h = 1;
static uint32_t *dbl_acquire(void *ctx, int *age)
{
    (void)ctx;
    if (nbuf == 2) {
        int b = req ^ 1;
        *age = bfno[b] < 0 ? 0 : fno_h - 1 - bfno[b];
        req = b; bfno[b] = fno_h++;
        return dbl[b];
    }
    if (fno_h % 5 && fno_h % 7) shown = req;          /* the last flip latched (else it is still queued) */
    int b = -1;
    for (int i = 0; i < 3; i++) if (i != shown && i != req && (b < 0 || bfno[i] < bfno[b])) b = i;
    *age = bfno[b] < 0 ? 0 : fno_h - 1 - bfno[b];
    req = b; bfno[b] = fno_h++;
    return dbl[b];
}

/* Track A, 1.0: the host is never a safe start (SBTK_SAFE=1 pretends one) */
int sh_safe_mode(void) { const char *e = getenv("SBTK_SAFE"); return e && *e == '1'; }
int sh_boot_reason(char *why, int cap) { if (why && cap > 0) snprintf(why, (size_t)cap, "Started on the host"); return -1; }

/* Track A, 1.0: the host logs a restart / power off and stays up */
int sh_restart(void) { fprintf(stderr, "host: restart\n"); return 0; }
int sh_power_off(void) { fprintf(stderr, "host: power off\n"); return 0; }

__attribute__((weak)) void sh_power_held(void) { fprintf(stderr, "host: Power held (no power sheet yet)\n"); }

/* Track A: Software Update on the host. hupd=none (up to date) | found | bad (the SHA-256 differs: staging fails) |
 * old (an older loader) | installed (this start committed it) picks what the card and the loader say; staging moves
 * 12 % a call (the phone: 1 MiB a call); install only prints. Nothing is read or written. */
static void hupd_fill(struct sh_update *u) { if (u) *u = HU; }
int sh_update_check(struct sh_update *u)
{
    if (HU.state == 2 || HU.state == 3) { hupd_fill(u); return 1; }
    memset(&HU, 0, sizeof HU);
    if (hupd_mode == 3) { HU.state = -1; snprintf(HU.why, sizeof HU.why, "Updates need the new boot loader."); }
    else if (hupd_mode == 4) { HU.state = 4; snprintf(HU.version, sizeof HU.version, "1.0.1"); }
    else if (hupd_mode == 1 || hupd_mode == 2) {
        HU.state = 1;
        snprintf(HU.version, sizeof HU.version, "1.0.1");
        snprintf(HU.notes, sizeof HU.notes, "Software Update, faster Wi-Fi joins and the new Startup settings. Fixes for the card and the lock screen.");
        HU.size = 19293184;
    } else snprintf(HU.why, sizeof HU.why, "No update on the card.");
    fprintf(stderr, "update: check: state %d\n", HU.state);
    hupd_fill(u);
    return HU.state == 1;
}
int sh_update_stage(struct sh_update *u)
{
    if (HU.state == 1) { HU.state = 2; HU.pct = 0; }
    if (HU.state == 2) {
        HU.pct += 12;
        if (hupd_mode == 2 && HU.pct >= 90) {
            HU.state = -1;
            snprintf(HU.why, sizeof HU.why, "The update file is damaged (its SHA-256 doesn't match). Nothing was installed.");
        } else if (HU.pct >= 100) { HU.pct = 100; HU.state = 3; }
    }
    hupd_fill(u);
    return HU.state;
}
int sh_update_install(void)
{
    if (HU.state != 3) return -1;
    fprintf(stderr, "update: would arm 1.0.1 in the inactive slot and restart (the host stays up)\n");
    return 0;
}

/* Track A: drivers for the shell, simulated (the phone: kern_next.c's block on kern/bt.c, cam.c, shub_more.c,
 * bathealth.c, haptic.c). Time is the film's clock (surv_host_now_ms) plus 50 ms a call, so a still's polls move on
 * too. SBTK_BT=fail: Bluetooth's start fails; SBTK_CAM=fail: the camera does not answer; SBTK_PROX=1: something near.
 * Nothing touches hardware; every change prints a line. */
static uint64_t hd_now(void) { static uint64_t calls; return surv_host_now_ms + 50 * ++calls; }
static int hd_env(const char *k, const char *v) { const char *e = getenv(k); return e && !strcmp(e, v); }
/* Bluetooth: 1.5 s to start, then scans of 11 s (one device more every 0.4 s), repeated while asked, 60 s at most */
static struct { int on, failed, scan; uint64_t t0, scan_t0, until; } HBT;
static const struct { const char *name; int rssi; } hbt_dev[] = {
    { "AirPods Pro", -48 }, { "JBL Flip 5", -61 }, { "Galaxy Buds2", -66 }, { "Mi Smart Band 6", -72 }, { "", -80 },
    { "LE-Bose QC35 II", -84 },
};
#define HBT_N (int)(sizeof hbt_dev / sizeof hbt_dev[0])
int sh_bt_on(int on)
{
    fprintf(stderr, "bluetooth: %s\n", on ? "on" : "off");
    memset(&HBT, 0, sizeof HBT);
    if (!on) return 0;
    HBT.on = 1;
    HBT.t0 = hd_now();
    HBT.failed = hd_env("SBTK_BT", "fail");
    return 0;
}
int sh_bt_state(void)
{
    if (!HBT.on) return 0;
    if (hd_now() - HBT.t0 < 1500) return 1;
    return HBT.failed ? -1 : 2;
}
const char *sh_bt_why(void)
{
    int s = sh_bt_state();
    return s == 1 ? "Bluetooth is starting" : s == 2 ? "Bluetooth is on" :
           s < 0 ? "The Bluetooth firmware is missing (VENDOR firmware/BCM4361B0_semco.hcd)" : "Bluetooth is off";
}
int sh_bt_scan(int on)
{
    if (!on) { HBT.scan = 0; return 0; }
    if (sh_bt_state() < 1) return -1;
    if (!HBT.scan) HBT.scan_t0 = hd_now();
    HBT.scan = 1;
    HBT.until = hd_now() + 60000;
    fprintf(stderr, "bluetooth: scan\n");
    return 0;
}
int sh_bt_count(void)
{
    if (sh_bt_state() != 2 || !HBT.scan_t0) return 0;
    uint64_t now = hd_now();
    if (HBT.scan && now >= HBT.until) HBT.scan = 0;
    int n = (int)((now - HBT.scan_t0) / 400);
    return n > HBT_N ? HBT_N : n;
}
int sh_bt_get(int i, char *name, int cap, int *rssi)
{
    if (i < 0 || i >= sh_bt_count()) return -1;
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s", hbt_dev[i].name);
    if (rssi) *rssi = hbt_dev[i].rssi + (int)(hd_now() / 1000 % 3) - 1;
    return 0;
}
/* the front camera: a moving test picture at 30 fps; a still 0.3 s after it is asked */
static struct { int on, w, h, still; unsigned seq; uint64_t t0, still_t; uint32_t *px, *spx; uint8_t *bmp; unsigned bmp_len; } HCAM;
static void hcam_draw(uint32_t *px, int w, int h, unsigned seq)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned r = (unsigned)(x * 255 / w), g = (unsigned)(y * 255 / h), b = (seq * 4 + (unsigned)(x + y) / 4) & 255;
            int bar = ((unsigned)y + seq * 6) % (unsigned)h < (unsigned)h / 12;
            px[y * w + x] = 0xFF000000u | (bar ? 0xF0F0F0u : r << 16 | g << 8 | b);
        }
}
int sh_cam_open(int w, int h)
{
    if (HCAM.on) return -1;
    if (w < 8 || h < 8 || w > 816 || h > 816 || w * h * 4 > 2 * 1024 * 1024) return -2;
    fprintf(stderr, "camera: open %d x %d\n", w, h);
    if (hd_env("SBTK_CAM", "fail")) return -5;
    free(HCAM.px);
    HCAM.px = malloc((size_t)w * (size_t)h * 4);
    if (!HCAM.px) return -8;
    HCAM.on = 1; HCAM.w = w; HCAM.h = h; HCAM.seq = 0; HCAM.still = 0;
    HCAM.t0 = hd_now();
    return 0;
}
const uint32_t *sh_cam_frame(int *w, int *h, unsigned *seq)
{
    if (!HCAM.on) return 0;
    uint64_t e = hd_now() - HCAM.t0;
    if (e < 600) return 0;                                         /* the sensor's start */
    unsigned s = (unsigned)((e - 600) / 33) + 1;
    if (s != HCAM.seq) { HCAM.seq = s; hcam_draw(HCAM.px, HCAM.w, HCAM.h, s); }
    if (w) *w = HCAM.w;
    if (h) *h = HCAM.h;
    if (seq) *seq = HCAM.seq;
    return HCAM.px;
}
int sh_cam_still(void)
{
    if (!HCAM.on || HCAM.still == 1) return -1;
    HCAM.still = 1;
    HCAM.still_t = hd_now() + 300;
    fprintf(stderr, "camera: a still\n");
    return 0;
}
const uint32_t *sh_cam_still_get(int *w, int *h, const unsigned char **bmp, unsigned *bmp_len)
{
    enum { SW = 612, SH = 816 };
    if (!HCAM.on || !HCAM.still || (HCAM.still == 1 && hd_now() < HCAM.still_t)) return 0;
    if (HCAM.still == 1) {
        if (!HCAM.spx) HCAM.spx = malloc((size_t)SW * SH * 4);
        HCAM.bmp_len = 54 + SW * 3 * SH;                           /* 612 x 3 = 1836: rows need no padding */
        if (!HCAM.bmp) HCAM.bmp = malloc(HCAM.bmp_len);
        if (!HCAM.spx || !HCAM.bmp) return 0;
        hcam_draw(HCAM.spx, SW, SH, HCAM.seq);
        uint8_t *b = HCAM.bmp;
        memset(b, 0, 54);
        b[0] = 'B'; b[1] = 'M';
        uint32_t v[] = { HCAM.bmp_len, 0, 54, 40, SW, SH };
        for (int k = 0; k < 6; k++) memcpy(b + 2 + 4 * k, &v[k], 4);
        b[26] = 1; b[28] = 24;
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++) {
                uint32_t p = HCAM.spx[(SH - 1 - y) * SW + x];
                uint8_t *d = b + 54 + (y * SW + x) * 3;
                d[0] = (uint8_t)p; d[1] = (uint8_t)(p >> 8); d[2] = (uint8_t)(p >> 16);
            }
        HCAM.still = 2;
    }
    if (w) *w = SW;
    if (h) *h = SH;
    if (bmp) *bmp = HCAM.bmp;
    if (bmp_len) *bmp_len = HCAM.bmp_len;
    return HCAM.spx;
}
const char *sh_cam_why(void)
{
    if (hd_env("SBTK_CAM", "fail")) return "The front camera does not answer";
    return HCAM.on ? (hd_now() - HCAM.t0 < 600 ? "Starting the front camera" : "Front camera on") : "Camera off";
}
void sh_cam_close(void)
{
    if (HCAM.on) fprintf(stderr, "camera: close\n");
    HCAM.on = 0;
    HCAM.still = 0;
}
/* Face Unlock (ui/face_svc.c, the phone's own logic): the host's camera fake has no face, so without a pack scans are
 * faked: SBTK_FACE=fail makes them fail, anything else matches after 0.3 s. SBTK_FACE_PACK=build/face/sbface.pak runs
 * the real networks on the fake frames (no face: "no face"). */
static uint64_t hf_now_ms(void) { return surv_host_now_ms; }
static uint64_t hf_now_us(void) { return tk_us(); }
static void *hf_alloc(size_t n) { return tk_alloc(n); }
static int hf_cam_open(unsigned w, unsigned h) { return sh_cam_open((int)w, (int)h); }
static const uint32_t *hf_cam_frame(unsigned *seq) { return sh_cam_frame(0, 0, seq); }
static void hf_cam_close(void) { sh_cam_close(); }
static void hf_random(uint8_t *p, unsigned n) { for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)rand(); }
static void hf_log(const char *l) { fprintf(stderr, "%s\n", l); }
static const uint8_t *hf_pak;                   /* the file (outside the UI heap) */
static size_t hf_pak_len;
static uint8_t *hf_res;                         /* the copy in the UI heap while resident (the phone's load) */
static struct face_eng hf_eng;                  /* SBTK_FACE_HOLD: a scan's buffers held with it (the heap check's worst case) */
static int hf_eng_on;
static struct fsvc_plat hf_plat;
static int hf_pack_get(const uint8_t **p, size_t *n)
{
    if (!hf_pak) return -1;
    if (!hf_res) {
        hf_res = tk_alloc(hf_pak_len);
        if (!hf_res) return -1;
        memcpy(hf_res, hf_pak, hf_pak_len);
        fprintf(stderr, "face: the pack in the UI heap: %zu KiB\n", hf_pak_len >> 10);
        if (getenv("SBTK_FACE_HOLD") && face_eng_open(&hf_eng, hf_res, hf_pak_len, &hf_plat.fn) == 0) {
            hf_eng_on = 1;
            fprintf(stderr, "face: a scan's buffers held too (SBTK_FACE_HOLD)\n");
        }
    }
    *p = hf_res;
    *n = hf_pak_len;
    return 1;
}
static int hf_pack_state(void) { return !hf_pak ? -1 : hf_res ? 2 : 0; }
static void hf_pack_release(void)
{
    if (hf_eng_on) face_eng_close(&hf_eng);
    hf_eng_on = 0;
    if (hf_res) { tk_free(hf_res); fprintf(stderr, "face: pack freed (unlock): %zu KiB back\n", hf_pak_len >> 10); }
    hf_res = 0;
}
static void face_host_init(void)
{
    struct fsvc_plat p;
    memset(&p, 0, sizeof p);
    const char *fp = getenv("SBTK_FACE_PACK");
    if (fp) {
        FILE *f = fopen(fp, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *b = NULL;
            if (n > 0 && !posix_memalign((void **)&b, 64, (size_t)n) && fread(b, 1, (size_t)n, f) == (size_t)n) { hf_pak = b; hf_pak_len = (size_t)n; }
            fclose(f);
        }
    }
    p.fake = hf_pak ? 0 : hd_env("SBTK_FACE", "fail") ? 2 : 1;
    p.pack_get = hf_pack_get;
    p.pack_state = hf_pack_state;
    p.pack_release = hf_pack_release;
    p.fn.alloc = hf_alloc;
    p.fn.free = tk_free;
    p.fn.now_us = hf_now_us;
    p.now_ms = hf_now_ms;
    p.cam_open = hf_cam_open;
    p.cam_frame = hf_cam_frame;
    p.cam_close = hf_cam_close;
    p.blob_get = sh_blob_get;
    p.blob_put = sh_blob_put;
    p.pref_int = sh_pref_int;
    p.pref_set_int = sh_pref_set_int;
    p.random = hf_random;
    p.pass_on = sh_pass_on;
    p.pass_wait = sh_pass_wait;
    p.log = hf_log;
    hf_plat = p;
    fsvc_init(&p);
    if (getenv("SBTK_FACE_HOLD")) sh_face_prepare();   /* the heap check: the pack (+ a scan's buffers) from the start, as when locked */
}
/* the sensors: an office (about 320 lux, 1013.2 hPa), the phone on a desk */
int sh_light_lux(void) { return 320 + (int)(hd_now() / 700 % 9) - 4; }
int sh_pressure_hpa10(void) { return 10132 + (int)(hd_now() / 5000 % 3) - 1; }
int sh_proximity_near(void) { return hd_env("SBTK_PROX", "1"); }
int sh_gyro_mdps(int *x, int *y, int *z)
{
    int j = (int)(hd_now() / 100 % 7) - 3;
    if (x) *x = 140 + j * 10;
    if (y) *y = -70 - j * 5;
    if (z) *z = 35;
    return 1;
}
void sh_auto_brightness(int on) { sh_pref_set_int("display.auto", !!on); fprintf(stderr, "Auto-Brightness: %s\n", on ? "on" : "off"); }
int sh_auto_brightness_on(void) { return sh_pref_int("display.auto", 0) == 1; }
void sh_call_proximity(int on) { fprintf(stderr, "call proximity: %s\n", on ? "on" : "off"); }
int sh_battery_health(int *max_pct, int *cycles, char *words, int cap)
{
    if (max_pct) *max_pct = 91;
    if (cycles) *cycles = 212;
    if (words && cap > 0) snprintf(words, (size_t)cap, "Your battery is in good health.");
    return 1;
}
static const char *const hvib[] = { "ring", "notify", "alarm", "tick", "success", "warning", "error" };
void sh_haptic_kind(int kind)
{
    if (kind >= 0 && kind < (int)(sizeof hvib / sizeof hvib[0])) fprintf(stderr, "haptic: %s\n", hvib[kind]);
}
void sh_haptic_stop(void) { fprintf(stderr, "haptic: stop\n"); }
int sh_haptic_level(void) { return sh_pref_int("haptic.level", 3); }
void sh_haptic_level_set(int level)
{
    level = level < 0 ? 0 : level > 3 ? 3 : level;
    sh_pref_set_int("haptic.level", level);
    fprintf(stderr, "haptic: level %d\n", level);
}

/* Track A: photos (the kernel's kern/photos.c) in RAM, with its limits: the newest 32; a picture of more than 697685
 * pixels kept at the kernel's smaller size (the kernel averages a box, this picks the nearest pixel). SBTK_PHOTOS=FILE
 * also keeps them in a file (read at the first call, written at every change), so a run after another shows what a
 * restart keeps; SBTK_PHOTOS=none: a phone without photo storage; SBTK_PHOTOS_HELD=1: storage busy (the modem on) */
#define HPH_MAX 32
#define HPH_MAX_PX 697685ULL
static struct { int w, h; long long utc; unsigned long long seq; uint32_t *px; } HPH[HPH_MAX];   /* the newest last */
static int hphn, hph_loaded;
static unsigned long long hph_seq;
static const char *hph_why = "";
static int hph_none(void) { return hd_env("SBTK_PHOTOS", "none"); }
static void hph_fit(int w, int h, int *sw, int *sh)
{
    if ((unsigned long long)w * (unsigned long long)h <= HPH_MAX_PX) { *sw = w; *sh = h; return; }
    unsigned long long lo = 1, hi = (unsigned long long)w;
    while (lo < hi) {
        unsigned long long mid = (lo + hi + 1) / 2, dh = ((unsigned long long)h * mid + (unsigned long long)w / 2) / (unsigned long long)w;
        if (mid * (dh ? dh : 1) <= HPH_MAX_PX) lo = mid; else hi = mid - 1;
    }
    unsigned long long dh = ((unsigned long long)h * lo + (unsigned long long)w / 2) / (unsigned long long)w;
    *sw = (int)lo;
    *sh = dh ? (int)dh : 1;
}
static void hph_write(void)
{
    const char *f = getenv("SBTK_PHOTOS");
    FILE *o = f && !hph_none() ? fopen(f, "wb") : 0;
    if (!o) return;
    fputs("SBTKPHOTOS1\n", o);
    for (int i = 0; i < hphn; i++) {
        fprintf(o, "%d %d %lld %llu\n", HPH[i].w, HPH[i].h, HPH[i].utc, HPH[i].seq);
        fwrite(HPH[i].px, 4, (size_t)HPH[i].w * (size_t)HPH[i].h, o);
    }
    fclose(o);
}
static void hph_load(void)
{
    if (hph_loaded) return;
    hph_loaded = 1;
    const char *f = getenv("SBTK_PHOTOS");
    FILE *in = f && !hph_none() ? fopen(f, "rb") : 0;
    char line[16];
    if (!in) return;
    if (!fgets(line, sizeof line, in) || strcmp(line, "SBTKPHOTOS1\n")) { fclose(in); return; }
    int w, h;
    long long utc;
    unsigned long long seq;
    while (hphn < HPH_MAX && fscanf(in, "%d %d %lld %llu", &w, &h, &utc, &seq) == 4 && fgetc(in) == '\n') {
        if (w < 1 || h < 1 || (unsigned long long)w * (unsigned long long)h > HPH_MAX_PX) break;
        uint32_t *px = malloc((size_t)w * (size_t)h * 4);
        if (!px || fread(px, 4, (size_t)w * (size_t)h, in) != (size_t)w * (size_t)h) { free(px); break; }
        HPH[hphn].w = w; HPH[hphn].h = h; HPH[hphn].utc = utc; HPH[hphn].seq = seq; HPH[hphn].px = px;
        if (seq > hph_seq) hph_seq = seq;
        hphn++;
    }
    fclose(in);
    fprintf(stderr, "photos: %d from %s\n", hphn, f);
}
static int hph_refused(void)
{
    hph_load();
    if (hph_none()) { hph_why = "This phone has no photo storage (the store entry sbos-photos)."; return 1; }
    if (hd_env("SBTK_PHOTOS_HELD", "1")) { hph_why = "Storage is busy while the modem is on. Try again later."; return 1; }
    return 0;
}
int sh_photo_save(const uint32_t *argb, int w, int h)
{
    hph_why = "";
    if (!argb || w < 1 || h < 1 || w > 4096 || h > 4096) { hph_why = "This picture can't be saved (its size)."; return -1; }
    if (hph_refused()) return -1;
    int sw, sh;
    hph_fit(w, h, &sw, &sh);
    uint32_t *px = malloc((size_t)sw * (size_t)sh * 4);
    if (!px) { hph_why = "No memory to save the photo."; return -1; }
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            px[y * sw + x] = 0xff000000u | argb[(size_t)(y * h / sh) * (size_t)w + (size_t)(x * w / sw)];
    if (hphn == HPH_MAX) {                      /* the oldest goes */
        free(HPH[0].px);
        memmove(HPH, HPH + 1, sizeof HPH[0] * (HPH_MAX - 1));
        hphn--;
    }
    HPH[hphn].w = sw; HPH[hphn].h = sh; HPH[hphn].utc = (long long)time(0); HPH[hphn].seq = ++hph_seq; HPH[hphn].px = px;
    hphn++;
    hph_write();
    fprintf(stderr, "photos: saved %dx%d (%d kept)\n", sw, sh, hphn);
    return 0;
}
int sh_photo_count(void) { hph_load(); return hph_none() ? 0 : hphn; }
int sh_photo_get(int i, uint32_t *argb, int cap_px, int *w, int *h, long long *utc)
{
    hph_why = "";
    hph_load();
    if (hph_none() || i < 0 || i >= hphn) { hph_why = "No such photo."; return -1; }
    int k = hphn - 1 - i, px = HPH[k].w * HPH[k].h;
    if (w) *w = HPH[k].w;
    if (h) *h = HPH[k].h;
    if (utc) *utc = HPH[k].utc;
    if (!argb) return px;
    if (cap_px < px) { hph_why = "The photo is larger than the space for it."; return -1; }
    memcpy(argb, HPH[k].px, (size_t)px * 4);
    return px;
}
int sh_photo_delete(int i)
{
    hph_why = "";
    if (hph_refused()) return -1;
    if (i < 0 || i >= hphn) { hph_why = "No such photo."; return -1; }
    int k = hphn - 1 - i;
    free(HPH[k].px);
    memmove(HPH + k, HPH + k + 1, sizeof HPH[0] * (size_t)(hphn - 1 - k));
    hphn--;
    hph_write();
    fprintf(stderr, "photos: deleted (%d kept)\n", hphn);
    return 0;
}
const char *sh_photo_why(void) { return hph_why; }

/* Track A, 1.0: the host has no clocks to raise; keep-awake holders are counted and logged */
void sh_perf_mode(int conservative) { (void)conservative; }
void sh_perf_hold(int ms) { (void)ms; }
int sh_hold_awake(const char *who, int on) { static int n; n += on ? 1 : (n > 0 ? -1 : 0); fprintf(stderr, "host: keep awake %s %s (%d)\n", who ? who : "?", on ? "on" : "off", n); return n; }

static int host_low;
void sh_low_power(int on) { host_low = !!on; }
int sh_low_power_on(void) { return host_low; }
void sh_slow_piece(const char *what, unsigned us)    /* cp7f: as the phone's, over SBTK_SLOWMS (default 20) */
{
    static int lim = -1;
    if (lim < 0) { const char *e = getenv("SBTK_SLOWMS"); lim = e ? atoi(e) : 20; }
    if (us > (unsigned)lim * 1000u) fprintf(stderr, "shell piece %s %u ms\n", what, us / 1000);
}

/* Track A, cp7: SBTK_ORI=0..3 pretends the phone is turned (stills / films of rotated apps); a film's "ori N" too */
static int host_ori_lock;
int sh_orientation(void) { const char *e = getenv("SBTK_ORI"); return host_ori_lock ? 0 : host_ori_film >= 0 ? host_ori_film : !e ? 0 : (atoi(e) & 3); }
int sh_orientation_lock(void) { return host_ori_lock; }
void sh_orientation_lock_set(int on) { host_ori_lock = !!on; }

/* Track A, cp7: the host's Bluetooth keyboard: SBTK_BTP=n pretends a pairing state (2 shows the code 042195) */
int sh_bt_pair(int i) { (void)i; return 0; }
int sh_bt_pair_state(void) { const char *e = getenv("SBTK_BTP"); return e ? atoi(e) : 0; }
const char *sh_bt_pair_code(void) { return sh_bt_pair_state() == 2 ? "042195" : ""; }
const char *sh_bt_pair_why(void) { return sh_bt_pair_state() == 6 ? "The keyboard didn't answer. Turn it on and try again." : ""; }
const char *sh_bt_kbd_name(void) { return "Magic Keyboard"; }
int sh_bt_kbd_paired(void) { return sh_bt_pair_state() == 4; }
int sh_bt_kbd_connected(void) { return sh_bt_pair_state() == 4; }
void sh_bt_unpair(void) { }
int sh_bt_reconnect(void) { return 0; }
__attribute__((weak)) void sh_hw_key(int usage, int down, uint32_t unicode) { (void)usage; (void)down; (void)unicode; }

/* Track A, cp7: the host has no sound: tones and streams are taken and dropped, the volume kept in RAM */
static int host_vol = 8;
int sh_snd_tone(int kind) { (void)kind; return 0; }
void sh_snd_tone_stop(void) { }
int sh_snd_volume(void) { return host_vol; }
void sh_snd_volume_set(int v) { host_vol = v < 0 ? 0 : v > 15 ? 15 : v; }
int sh_snd_open(void) { return 0; }
unsigned sh_snd_write(int id, const int16_t *frames, unsigned n)   /* cp7w: SBTK_SNDOUT=file keeps what is written (raw 48 kHz stereo s16) */
{
    (void)id;
    const char *o = getenv("SBTK_SNDOUT");
    if (o) { FILE *f = fopen(o, "ab"); if (f) { fwrite(frames, 4, n, f); fclose(f); } }
    return n;
}
unsigned sh_snd_space(int id) { (void)id; return 4096; }
void sh_snd_close(int id) { (void)id; }
const char *sh_snd_why(void) { return "The host has no sound"; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: sbtk-host PAK still|film|bench ...\n"); return 2; }
    uint32_t len;
    uint8_t *pak = readall(argv[1], &len);
    if (!strcmp(argv[2], "bench")) { bench(); return 0; }
    card_init(argv[1]);
    if (!tk_pack_open(pak, len)) { fprintf(stderr, "bad pack\n"); return 1; }
    static tk_comp c;
    /* the phone's frame buffers are not in its UI heap (the old UI's back buffer, the framebuffer) */
    if (!tk_comp_init(&c, getenv("SBTK_HEAP_MB") ? calloc((size_t)TK_W * TK_H, 4) : 0,
                      getenv("SBTK_HEAP_MB") ? calloc((size_t)TK_W * TK_H, 4) : 0)) return 1;
    { extern int getenv_dbg; getenv_dbg = getenv("SBTK_DBG") != 0; }
    if (getenv("SBTK_DBL")) {                  /* the phone's two scanout buffers (P2r): presents alternate */
        nbuf = atoi(getenv("SBTK_DBL")) == 3 ? 3 : 2;
        dbl[0] = c.panel.px;
        dbl[1] = calloc((size_t)TK_W * TK_H, 4);
        dbl[2] = calloc((size_t)TK_W * TK_H, 4);
        c.acquire = dbl_acquire;
    }
    if (getenv("SBTK_CORES")) { ncores = atoi(getenv("SBTK_CORES")); if (ncores > 1) { c.par = host_par; tk_par = host_par; } }
    if (getenv("SBTK_NOLENS")) tk_glass_lens = 0;       /* v6: the concept's baked glass (to compare) */
    uint64_t t0 = tk_us();
    sh_init(&c);
    fprintf(stderr, "bake: %.1f ms; heap %.1f MB (peak %.1f MB)\n", (tk_us() - t0) / 1000.0, tk_heap_used() / 1e6, tk_heap_peak() / 1e6);
    if (getenv("SBTK_MEM")) fprintf(stderr, "mem: layers %.1f MB\n", memwalk(c.root, 0) / 1e6);
    host_memdump("bake");
    lin_fake_init();
    hp_load();                                 /* Track A: SBTK_PREFS's store, then what the shell keeps */
    face_host_init();
    sh_restore();
    if (!strcmp(argv[2], "still") && argc >= 4) {
        for (int i = 4; i < argc; i++) { apply_state(argv[i]); lin_fake_settle(); }
        tk_comp_frame(&c, 0, 1);
        fprintf(stderr, "frame: %.2f ms; heap %.1f MB (peak %.1f MB)\n", c.last_us / 1000.0, tk_heap_used() / 1e6, tk_heap_peak() / 1e6);
        host_memdump("still");
        write_ppm(argv[3], &c.panel);
        return 0;
    }
    if (!strcmp(argv[2], "clocktest")) {
        /* a new minute must reach the panel through an ordinary (damage-only) frame */
        tk_comp_frame(&c, 0, 1);
        uint32_t before = 0, after = 0;
        for (int y = PT(96); y < PT(204); y++) for (int x = 0; x < TK_W; x += 3) before = before * 31 + c.panel.px[y * TK_W + x];
        uint64_t st0 = tk_us();
        sh_status(21, 6, 2, 10, 11, 86, 41);       /* 8c: a new minute and a new charge: its own time (what shows only) */
        fprintf(stderr, "clocktest: sh_status %.2f ms\n", (tk_us() - st0) / 1000.0);
        int px = tk_comp_frame(&c, 0, 0);
        for (int y = PT(96); y < PT(204); y++) for (int x = 0; x < TK_W; x += 3) after = after * 31 + c.panel.px[y * TK_W + x];
        printf("clocktest: %s (%d px redrawn)\n", before != after && px > 0 ? "PASSED: the clock changed on the panel" : "FAILED: the panel kept the old minute", px);
        return before != after && px > 0 ? 0 : 1;
    }
    if (!strcmp(argv[2], "frames") && argc >= 3) {
        /* the cost of one frame of a state with everything damaged: motion (half + present) and rest (full) */
        for (int i = 3; i < argc; i++) apply_state(argv[i]);
        for (int m = 1; m >= 0; m--) {
            double tc = 0, tp = 0;
            int n = 40;
            tk_comp_frame(&c, m, 1);
            for (int i = 0; i < n; i++) { tk_comp_frame(&c, m, 1); tc += c.us_comp; tp += c.us_present; }
            printf("frames %s: %s compose %.2f present %.2f ms\n", argc > 3 ? argv[3] : "lock", m ? "motion" : "rest", tc / n / 1000, tp / n / 1000);
        }
        return 0;
    }
    if (!strcmp(argv[2], "selftest") && argc >= 4) {
        /* the phone's self test: every film of FILMS.txt in one run, each from the reset lock screen; the heap after
         * each (with SBTK_HEAP_MB=160: the phone's heap) */
        FILE *ff = fopen(argv[3], "r");
        char line[8192];
        while (ff && fgets(line, sizeof line, ff)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = 0;
            extern int host_ooms;
            sh_set("reset", "", 0);
            if (getenv("SBTK_TICKS")) {          /* P3s: a new minute (and charge, temperature) before each film: rebuilds */
                static int m;
                m++;
                sh_status(21, m % 60, 2, 10, 11, 87 - m % 20, 41 + m % 5);
            }
            int oom0 = host_ooms;
            size_t rest0 = tk_heap_used();
            tk_heap_peak_reset();
            film(&c, "none", colon + 1, 1);
            fprintf(stderr, "selftest: %s: heap %zu MiB (peak %zu, start %zu), largest free %llu MiB, refusals %d\n", line, tk_heap_used() >> 20,
                    tk_heap_peak() >> 20, rest0 >> 20, (unsigned long long)(tk_heap_largest() >> 20), host_ooms - oom0);
        }
        return 0;
    }
    if (!strcmp(argv[2], "film") && argc >= 5) return film(&c, argv[3], argv[4], argc > 5 && !strcmp(argv[5], "half"));
    return 2;
}
