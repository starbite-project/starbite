/* SPDX-License-Identifier: MIT
 * Settings for Starbite OS Next (helper B2, Track B), included by shell_lock.c after the other parts (it uses the
 * shell's helpers, the kit and every part's state). iOS 26's structure:
 *   - the root: the large title, the Starbite profile card (Galaxy S9, Starbite OS), then Wi-Fi / Bluetooth / Cellular /
 *     Battery;
 *     General / Display & Brightness / Wallpaper / Sounds & Haptics / Notifications; Passcode / Emergency / Privacy /
 *     Startup; Linux Apps / App Store;
 *   - pushed pages with the glass back circle and an inline title: General (About, Software Update, Date & Time,
 *     Keyboard, Language & Region, Reset), Display & Brightness (the live brightness slider), Wallpaper, Sounds &
 *     Haptics, Notifications (per app), Battery (the level, the history since the phone started, Last 2 %), Privacy,
 *     Emergency (Medical Card, Emergency mode), Linux Apps;
 *   - on Track A's drivers (sbos-core-api.md 11): Bluetooth (the switch, a scan while the page is open, Other Devices;
 *     scanning only), Battery > Battery Health (Maximum Capacity, Cycle Count), Display's Auto-Brightness (with the
 *     light sensor's reading), Sounds & Haptics' Haptic Strength (the motor's level, a sample on each choice).
 *     Bluetooth and Display poll their drivers on the platform's refresh turns (se_refresh): a page is built again only
 *     when what it shows changed, at most ~3 times a second; the lux reading redraws its footer only.
 * Wi-Fi (shell_wifi.inc), Cellular (shell_cell.inc) and the Medical Card's editor (shell_survival.inc) keep their own
 * pages (AS.sub 2, 3, 4: the page strips). Passcode is Track A's (shell_passcode.inc), Startup Track J's
 * (shell_startup.inc): their rows open SE_PASSCODE_PAGE / SE_STARTUP_PAGE in the strips. Date & Time is here on
 * Track A's time API (sh_clock24, sh_tz); its Time Zone row opens Track A's zone list (shell_time.inc, SE_TZ_PAGE),
 * whose back comes here again. Display's Auto-Lock is Track A's sh_autolock.
 *
 * The pages here are not the four strips: one layer of the page's own height (cut to its groups' width, ~20 MB for
 * the root with its half copy made at idle), scrolled under a fixed nav row (the back circle, the inline title) with
 * iOS's physics, clipped with a soft edge under the nav row. A switch redraws only its group and the kit animates
 * its knob over it; the brightness slider moves two small layers while dragged.
 *
 * Values: the ones the platform already keeps go through its calls (brightness, Last 2 % at 3 %, the Medical Card).
 * The rest are the shell's own, behind se_get / se_put, kept across restarts in Track A's prefs (sh_pref_*):
 * haptic.system, kb.haptic, kb.autocap, clock.timervib, and notif.<app>.allow / .banners (shell_nc.inc's nc_pref
 * keys, the ones a post checks). */

enum { SE_ROOT = 0, SE_DISPLAY = 1, SE_WIFI = 2, SE_CELL = 3, SE_CARD = 4,
       SE_GENERAL = 10, SE_ABOUT, SE_UPDATE, SE_DATETIME, SE_KEYBOARD, SE_LANG, SE_RESET, SE_WALL, SE_SOUNDS, SE_NOTIF,
       SE_NOTIF_APP, SE_BATTERY, SE_PRIVACY, SE_EMERGENCY, SE_LINUX, SE_SEARCH, SE_BT, SE_HEALTH, SE_NPAGES };
/* actions (row ids from 100; a row id < 100 pushes that page) */
enum { A_NONE = 100, A_CARD, A_PASSCODE, A_STARTUP, A_STORE, A_LAST2, A_TEXTSIZE, A_RESET, A_EMG, A_BRIGHT,
       A_RANGE, A_TERMINAL, A_TZ, A_AUTOLOCK, A_UPDATE, A_BTDEV, A_BTKBD, A_HAPLVL, A_VOLUME, A_TONE_RING, A_TONE_TEXT, A_TONE_ALARM,
       T_FIRST = 200, T_SYSHAP = T_FIRST, T_KBHAP, T_AUTOCAP, T_TIMERVIB, T_LAST2AUTO, T_NALLOW, T_NBANNER, T_CALLVIB, T_SMSVIB,
       T_CLOCK24, T_BT, T_AUTOBRIGHT, T_KBSND, T_LOCKSND, T_SILENT };

/* ---- the shell's own settings: Track A's prefs (kept across restarts; every one defaults to on) ------------------ */
enum { SK_SYSHAP, SK_KBHAP, SK_AUTOCAP, SK_TIMERVIB, SK_KBSND, SK_LOCKSND, SK_NALLOW, SK_NBANNER = SK_NALLOW + 8, SK_N = SK_NBANNER + 8 };
static const char *const SK_KEY[SK_NALLOW] = { "haptic.system", "kb.haptic", "kb.autocap", "clock.timervib", "kb.click", "snd.lock" };
static const char *se_notif_who(int i);
static int se_get(int k)
{
    if (k < 0 || k >= SK_N) return 0;
    if (k < SK_NALLOW) return sh_pref_int(SK_KEY[k], 1) != 0;
    return nc_pref(se_notif_who(k < SK_NBANNER ? k - SK_NALLOW : k - SK_NBANNER), k < SK_NBANNER ? "allow" : "banners") != 0;
}
static void se_put(int k, int v)
{
    if (k < 0 || k >= SK_N) return;
    if (k < SK_NALLOW) { sh_pref_set_int(SK_KEY[k], v != 0); return; }
    char key[40];
    nc_pref_key(se_notif_who(k < SK_NBANNER ? k - SK_NALLOW : k - SK_NBANNER), k < SK_NBANNER ? "allow" : "banners", key);
    sh_pref_set_int(key, v != 0);
}
static void se_defaults(void) { for (int k = 0; k < SK_N; k++) se_put(k, 1); }
/* hooks the rest of the shell calls (declared in shell_lock.c's kit block) */
static int se_haptic_ok(int key) { return se_get(SK_SYSHAP) && (!key || se_get(SK_KBHAP)); }
static int se_autocap(void) { return se_get(SK_AUTOCAP); }
static int se_timer_vib(void) { return se_get(SK_TIMERVIB); }
static int se_kb_click(void) { return se_get(SK_KBSND); }         /* cp7w: Keyboard Clicks */
static int se_lock_snd(void) { return se_get(SK_LOCKSND); }       /* 8a: Lock Sound (iOS's, on by default) */
static float se_tone_until;                     /* cp7w: a ringtone's preview stops then (ms; 0 none) */
/* the notification posters Settings lists (shell_nc.inc's NCSRC, plus Phone and Messages) */
static const struct { const char *who, *name; } SEN[] = { { "clock", "Clock" }, { "battery", "Battery" }, { "modem", "Cellular" },
    { "wifi", "Wi-Fi" }, { "messages", "Messages" }, { "phone", "Phone" }, { "heat", "Survival Heat" }, { "survival", "USB Power" } };
#define SEN_N ((int)(sizeof SEN / sizeof SEN[0]))
static const char *se_notif_who(int i) { return i >= 0 && i < SEN_N ? SEN[i].who : "system"; }
/* the battery since the phone started (sh_status): a sample each 10 minutes and at each change, 24 h kept */
#define SE_HIST 144
static struct { int n, last_m, range; uint8_t pct[SE_HIST], chg[SE_HIST]; int16_t min[SE_HIST]; } BH;
static void se_batt_sample(int h, int mi, int pct)
{
    int m = h * 60 + mi;
    if (BH.n && m / 10 == BH.last_m / 10 && BH.pct[BH.n - 1] == pct) return;
    if (BH.n && m / 10 == BH.last_m / 10) BH.n--;                 /* the same 10 minutes: the newest value */
    if (BH.n == SE_HIST) {
        for (int i = 1; i < SE_HIST; i++) { BH.pct[i - 1] = BH.pct[i]; BH.chg[i - 1] = BH.chg[i]; BH.min[i - 1] = BH.min[i]; }
        BH.n--;
    }
    BH.pct[BH.n] = (uint8_t)tk_clampf((float)pct, 0, 100);
    BH.chg[BH.n] = (uint8_t)(SYS.charging != 0);
    BH.min[BH.n] = (int16_t)m;
    BH.n++;
    BH.last_m = m;
}

/* ---- the page's rows ------------------------------------------------------------------------------------------- */
enum { SR_NAV, SR_VAL, SR_TOG, SR_ACT, SR_SLIDER, SR_SEG, SR_PROFILE, SR_BIG, SR_GRAPH, SR_WALL };
enum { SEI_NONE, SEI_WIFI, SEI_CELL, SEI_BATT, SEI_GENERAL, SEI_DISPLAY, SEI_WALL, SEI_SOUNDS, SEI_NOTIF, SEI_PASS,
       SEI_SOS, SEI_PRIV, SEI_START, SEI_LINUX, SEI_STORE, SEI_HEART, SEI_BT };
typedef struct {
    int kind, id, on, dis, ico, red;
    const char *l, *v;
    float y, h;                                   /* content pt at scroll 0 */
    const char *sub;                              /* a second line (the row is 66 tall) */
    int segn;                                     /* SR_SEG: its segments (0: two) */
    const char *why;                              /* cp6: a disabled row's words when it is tapped */
    int dev;                                      /* cp7: A_BTDEV: the scan's device index */
} se_row;
typedef struct { int first, n; const char *head, *foot; float y, h, fy; int fl; } se_sec;
#define SE_MAXR 32
#define SE_MAXS 10
static struct {
    int page, napp, nr, ns, built, bld_page;
    se_row r[SE_MAXR];
    se_sec s[SE_MAXS];
    char pool[2048];
    int pn;
    float content_h, scroll, v, base, save[SE_NPAGES];
    int momentum, down, moved, press, slider, axis;
    float x0, y0, t0;
    tk_tracker tr;
    tk_spring sp;
    float bright;                                 /* the slider's place (0..1) */
    float vol;                                    /* cp7w: Sounds & Haptics' volume slider's place (0..1) */
    char q[24];                                   /* Search: what is typed */
    int shift, num;
    int stack[8], ns_, popping;                   /* the pages pushed on the way here (back pops them, iOS) */
} SE;
/* IOS-REFERENCE 6 / 10: Settings' search at the bottom: a 48 glass capsule (x 20..370, 38 above the bottom); on its
 * page the field rides above the keyboard with a 44 "x" circle beside it */
#define SE_SB_Y (H_PT - 30 - 8 - 48)
#define SE_SF_Y (540 - 8 - 48)
/* the page in chunks of SE_CH panel rows (5.5 MB each: no 23 MB block a fragmented heap may not have) */
#define SE_NCH 8
#define SE_CH 1024
static tk_layer sepg[SE_NCH];
static tk_layer seclip, sescr, senav, sesb, sekb;
/* 8a (gaps-c #9): the root's bottom scroll edge: the rows fade into the black ground under the search capsule (iOS
 * 26's soft edge; nothing legible below the bar), a half-resolution bitmap drawn 2x (as the NC's wallpaper view) */
static tk_layer seedge;
#define SE_EDGE_Y (SE_SB_Y - 30)   /* sesb: the root's search capsule (the field above the keyboard); sekb: its keyboard */
#define SE_TOP (SAFE_TOP + NAVBAR_H)              /* the content shows below the nav row (a soft edge) */

static const char *se_str(const char *a, const char *b)
{
    int n = SE.pn;
    char *o = SE.pool + n;
    for (const char *p = a; p && *p && n < (int)sizeof SE.pool - 2; p++) SE.pool[n++] = *p;
    for (const char *p = b; p && *p && n < (int)sizeof SE.pool - 2; p++) SE.pool[n++] = *p;
    SE.pool[n++] = 0;
    SE.pn = n;
    return o;
}
/* a + b as a sentence: a full stop after the kernel's words when they have none */
static const char *se_sentence(const char *a, const char *b)
{
    char *o = (char *)se_str(a, b);
    int n = (int)tk_strlen(o);
    if (n && o[n - 1] != '.' && SE.pn < (int)sizeof SE.pool - 1) { o[n] = '.'; o[n + 1] = 0; SE.pn++; }
    return o;
}
static const char *se_num(int v, const char *unit)
{
    char b[16];
    put_dec(b, v);
    return se_str(b, unit);
}
static void sec(const char *head)
{
    if (SE.ns >= SE_MAXS) return;
    se_sec *s = &SE.s[SE.ns++];
    s->first = SE.nr;
    s->n = 0;
    s->head = head;
    s->foot = 0;
}
static void foot(const char *t) { if (SE.ns) SE.s[SE.ns - 1].foot = t; }
static se_row *row(int kind, int id, const char *l, const char *v)
{
    if (SE.nr >= SE_MAXR || !SE.ns) return &SE.r[SE_MAXR - 1];
    se_row *r = &SE.r[SE.nr++];
    *r = (se_row){ kind, id, 0, 0, SEI_NONE, 0, l, v ? v : "", 0, 0, 0, 0, 0, 0 };
    SE.s[SE.ns - 1].n++;
    return r;
}
static se_row *nav(int id, int ico, const char *l, const char *v) { se_row *r = row(SR_NAV, id, l, v); r->ico = ico; return r; }
static se_row *val(const char *l, const char *v) { return row(SR_VAL, A_NONE, l, v); }
static se_row *tog(int id, const char *l, int on) { se_row *r = row(SR_TOG, id, l, 0); r->on = on; return r; }
/* cp6: a disabled row (.3 alpha, no press) and what a tap on it says */
static se_row *se_off(se_row *r, const char *why) { r->dis = 1; r->why = why; return r; }

static const char *se_autolock_text(int s)
{
    return s <= 0 ? "Never" : s < 60 ? "30 Seconds" : s < 120 ? "1 Minute" : s < 180 ? "2 Minutes" : s < 240 ? "3 Minutes" :
           s < 300 ? "4 Minutes" : "5 Minutes";
}
static const char *se_title(int p)
{
    switch (p) {
    case SE_DISPLAY: return "Display & Brightness";
    case SE_GENERAL: return "General";
    case SE_ABOUT: return "About";
    case SE_UPDATE: return "Software Update";
    case SE_DATETIME: return "Date & Time";
    case SE_KEYBOARD: return "Keyboard";
    case SE_LANG: return "Language & Region";
    case SE_RESET: return "Reset";
    case SE_WALL: return "Wallpaper";
    case SE_SOUNDS: return "Sounds & Haptics";
    case SE_NOTIF: return "Notifications";
    case SE_NOTIF_APP: return SE.napp >= 0 && SE.napp < SEN_N ? SEN[SE.napp].name : "Notifications";
    case SE_BATTERY: return "Battery";
    case SE_PRIVACY: return "Privacy";
    case SE_EMERGENCY: return "Emergency";
    case SE_LINUX: return "Linux Apps";
    case SE_SEARCH: return "Search";
    case SE_BT: return "Bluetooth";
    case SE_HEALTH: return "Battery Health";
    default: return "Settings";
    }
}
static int se_parent(int p)
{
    if (p >= SE_ABOUT && p <= SE_RESET) return SE_GENERAL;
    if (p == SE_NOTIF_APP) return SE_NOTIF;
    if (p == SE_CARD) return SE_EMERGENCY;
    if (p == SE_HEALTH) return SE_BATTERY;
    return SE_ROOT;
}
static const char *se_card_words(void)
{
    const char *why = 0;
    int s = sh_card_status(&why);
    return s == SH_CARD_READY ? "microSD card ready" : s == SH_CARD_NONE ? "No card" : s == SH_CARD_MOUNTING ? "Not checked yet" :
           why && why[0] ? why : "The card has an error";
}

/* ---- Track A's drivers (sbos-core-api.md 11) ----------------------------------------------------------------------- */
/* Bluetooth: what the page showed when it was built (the poll compares), the scan this page asked for, the spinner */
static tk_layer sespin;
static struct {
    int st, cnt, scan, spin, spin_k, lux_h, bright;
    float scan_t, build_t, spin_t, poll_t, spin_cx, spin_cy;
} SEB = { 0, 0, 0, 0, 0, 0, 0, -1, -1e9f, 0, 0, 0, 0 };
/* cp7: a keyboard's pairing (Track A's sh_bt_pair, the drivers track's bt_pair.c): the device asked, the state last
 * seen, the code alert up, the name of the row tapped */
static struct { int dev, st, alert; char name[48]; } SBP = { -1, 0, 0, "" };
/* the paired keyboard's name (Track A keeps it across a restart; "Keyboard" when unknown) */
static const char *se_bt_kname(void) { const char *n = sh_bt_kbd_name(); return n && n[0] ? n : "Keyboard"; }
#define SE_BT_HEAD "Other Devices"
#define SE_BT_MAXD 12                             /* named devices listed (the rest: a count in the footer) */
static int se_awake(void) { return !locked && !SL.black; }
static const char *se_bt_row_text(void)
{
    int st = sh_bt_state();
    return st == 2 ? "On" : st == 1 ? "Turning On\xe2\x80\xa6" : st < 0 ? "Not Available" : "Off";
}
/* the lux reading in Display's footer (a buffer of its own: the footer is redrawn alone when it changes) */
static char se_lux_foot[200];
static int se_lux_shown = -2;
static int se_lux_round(int lux) { return lux < 0 ? -1 : lux < 100 ? lux : (lux + 5) / 10 * 10; }   /* tens above 100 lux */
static void se_lux_text(int lux)
{
    char b[16];
    int n = 0;
    const char *a = "Four steps of the panel's own levels, as in the Control Centre. Auto-Brightness picks the step from the "
                    "light around the phone every 2 seconds while the screen is on. ";
    while (*a && n < (int)sizeof se_lux_foot - 1) se_lux_foot[n++] = *a++;
    se_lux_shown = se_lux_round(lux);
    if (lux < 0) a = "The light sensor isn't available yet.";
    else {
        put_dec(b, se_lux_shown);
        const char *c = "Ambient light: ";
        while (*c && n < (int)sizeof se_lux_foot - 1) se_lux_foot[n++] = *c++;
        for (c = b; *c && n < (int)sizeof se_lux_foot - 1; c++) se_lux_foot[n++] = *c;
        a = " lux.";
    }
    while (*a && n < (int)sizeof se_lux_foot - 1) se_lux_foot[n++] = *a++;
    se_lux_foot[n] = 0;
}
static const char *const SE_HAP[4] = { "Off", "Light", "Medium", "Strong" };   /* haptic.level 0..3 (shell.h) */

/* each page's rows */
static void se_rows(int p)
{
    SE.nr = SE.ns = 0;
    SE.pn = 0;
    char b[48];
    switch (p) {
    case SE_ROOT:
        sec(0);
        row(SR_PROFILE, SE_ABOUT, "Galaxy S9", "Starbite OS");
        sec(0);
        nav(SE_WIFI, SEI_WIFI, "Wi-Fi", wf_row_text());
        nav(SE_BT, SEI_BT, "Bluetooth", se_bt_row_text());   /* Track B: iOS's order (Wi-Fi, Bluetooth, Cellular) */
        nav(SE_CELL, SEI_CELL, "Cellular", md_text());
        nav(SE_BATTERY, SEI_BATT, "Battery", se_num(ST.pct, " %"));
        sec(0);
        nav(SE_GENERAL, SEI_GENERAL, "General", 0);
        nav(SE_DISPLAY, SEI_DISPLAY, "Display & Brightness", 0);
        nav(SE_WALL, SEI_WALL, "Wallpaper", 0);
        nav(SE_SOUNDS, SEI_SOUNDS, "Sounds & Haptics", 0);
        nav(SE_NOTIF, SEI_NOTIF, "Notifications", 0);
        sec(0);
        nav(A_PASSCODE, SEI_PASS, "Passcode", 0);
        nav(SE_EMERGENCY, SEI_SOS, "Emergency", EMG ? "On" : 0);
        nav(SE_PRIVACY, SEI_PRIV, "Privacy", 0);
        nav(A_STARTUP, SEI_START, "Startup", 0)->sub = "OS, firmware, recovery";
        sec(0);
        nav(SE_LINUX, SEI_LINUX, "Linux Apps", 0);
        nav(A_STORE, SEI_STORE, "App Store", 0);
        break;
    case SE_GENERAL:
        sec(0);
        nav(SE_ABOUT, 0, "About", 0);
        nav(A_UPDATE, 0, "Software Update", 0);   /* Track A update: its own page (shell_update.inc) */
        sec(0);
        nav(SE_DATETIME, 0, "Date & Time", 0);
        nav(SE_KEYBOARD, 0, "Keyboard", 0);
        nav(SE_LANG, 0, "Language & Region", 0);
        sec(0);
        nav(SE_RESET, 0, "Reset", 0);
        break;
    case SE_ABOUT:
        sec(0);
        val("Name", "Galaxy S9");
        val("Software Version", SYS.ver);
        val("Model Name", "Galaxy S9");
        val("Model Number", "SM-G960F");
        sec(0);
        val("Memory", "4 GB");
        val("Storage", se_card_words());
        val("Processor", "Exynos 9810");
        foot("Starbite OS runs from the memory card. The serial number and the modem's numbers are not shown.");
        break;
    case SE_DATETIME: {                          /* Track A's time: 12 / 24-hour, the zone (sbos-core-api.md 4) */
        char t[12];
        const char *city = "";
        fmt_time(t, ST.h, ST.mi, 1);
        (void)sh_tz_name(sh_tz(), &city);
        sec(0);
        tog(T_CLOCK24, "24-Hour Time", sh_clock24());
        sec(0);
        se_off(tog(A_NONE, "Set Automatically", 1), "The time always comes from the network");
        nav(A_TZ, 0, "Time Zone", se_str(city, 0));
        foot("The time comes from the network; daylight saving time follows the zone's own rules.");
        sec(0);
        val("Time", se_str(t, 0));
        val("Date", se_str(live_text("@date", b), 0));
        val("Year", se_num(cal_year, 0));
        break;
    }
    case SE_KEYBOARD:
        sec(0);
        val("Keyboards", "English");
        sec(0);
        tog(T_AUTOCAP, "Auto-Capitalization", se_get(SK_AUTOCAP));
        tog(T_KBHAP, "Haptic Feedback", se_get(SK_KBHAP));
        foot("Haptic Feedback is a light tap of the vibration motor at each key.");
        break;
    case SE_LANG:
        sec("Preferred Language");
        val("English", "");
        sec(0);
        val("Region", "\xe2\x80\x94");
        val("Calendar", "Gregorian");
        foot("English is the only language in this version.");
        break;
    case SE_RESET:
        sec(0);
        row(SR_ACT, A_RESET, "Reset Settings", 0)->red = 1;
        foot("Puts this page's switches back as they were at the start: haptics, the keyboard and notifications. Your "
             "Medical Card and Wi-Fi networks are kept. Erasing the phone isn't in Starbite OS.");
        break;
    case SE_DISPLAY: {
        sec("Brightness");
        row(SR_SLIDER, A_BRIGHT, 0, 0);
        tog(T_AUTOBRIGHT, "Auto-Brightness", sh_auto_brightness_on());   /* (iOS: Accessibility > Display & Text Size) */
        se_lux_text(sh_light_lux());
        foot(se_lux_foot);
        SEB.lux_h = (int)d_group_foot(0, 0, se_lux_foot);
        SEB.bright = cc_bright;
        sec(0);
        nav(A_TEXTSIZE, 0, "Text Size", "Default");
        val("Appearance", "Dark");
        nav(A_AUTOLOCK, 0, "Auto-Lock", se_autolock_text(sh_autolock()));
        foot("The screen turns off and the phone locks after this long without a touch.");
        break;
    }
    case SE_WALL:
        sec(0);
        row(SR_WALL, A_NONE, 0, 0);
        foot("Starbite OS has one wallpaper, the night sky; Emergency mode shows its own red one. Choosing a photo "
             "isn't in this version.");
        break;
    case SE_SOUNDS:
        sec("Haptics");
        tog(T_SYSHAP, "System Haptics", se_get(SK_SYSHAP));
        tog(T_KBHAP, "Keyboard Feedback", se_get(SK_KBHAP));
        tog(T_TIMERVIB, "Clock Timer", se_get(SK_TIMERVIB));
        foot("System Haptics: the taps at holds and keys. Clock Timer: the buzz when a timer ends.");
        sec("Haptic Strength");
        {
            se_row *r = row(SR_SEG, A_HAPLVL, 0, 0);
            r->on = sh_haptic_level();
            r->segn = 4;
            r->v = (const char *)SE_HAP;
        }
        foot("How hard the motor buzzes for calls, alarms and notifications; each choice plays a sample. Off: no "
             "buzz for them at all. The light taps of System Haptics stay as they are.");
        sec("Vibrate");
        se_off(tog(T_CALLVIB, "Calls", 1), "Set by the modem: it can't be changed yet");
        se_off(tog(T_SMSVIB, "Messages", 1), "Set by the modem: it can't be changed yet");
        foot("The modem buzzes for calls and messages by itself; turning that off needs the modem's own setting.");
        /* cp7w: the loudspeaker (Track A's sh_snd_*): the volume (the Control Centre's too), the tones (a tap plays one) */
        sec("Ringer and Alerts");
        row(SR_SLIDER, A_VOLUME, 0, 0);
        foot("The loudspeaker's volume for ringtones, alarms, timers and notifications, the same as the Control "
             "Centre's. Calls and music have their own.");
        sec("Sounds");
        { se_row *r = row(SR_VAL, A_TONE_RING, "Ringtone", "Ring"); (void)r; }
        row(SR_VAL, A_TONE_TEXT, "Text Tone", "Note");
        row(SR_VAL, A_TONE_ALARM, "Alarms and Timers", "Beeps");
        foot("A tap plays the sound.");
        sec(0);
        tog(T_SILENT, "Silent Mode", snd_silent());
        foot("Silent Mode quiets key clicks, the lock, notifications, the shutter and the charging chime, and calls "
             "only vibrate (the Control Centre's bell too). Alarms and timers still ring.");
        sec(0);
        tog(T_KBSND, "Keyboard Clicks", se_get(SK_KBSND));
        tog(T_LOCKSND, "Lock Sound", se_get(SK_LOCKSND));
        break;
    case SE_NOTIF:
        sec("Notification Style");
        for (int i = 0; i < SEN_N; i++)
            nav(SE_NOTIF_APP + 1000 * (i + 1), 0, SEN[i].name, !se_get(SK_NALLOW + i) ? "Off" : se_get(SK_NBANNER + i) ? "Banners" : "Centre only");
        break;
    case SE_NOTIF_APP: {
        int i = SE.napp;
        if (i < 0 || i >= SEN_N) i = 0;
        sec(0);
        tog(T_NALLOW, "Allow Notifications", se_get(SK_NALLOW + i));
        sec("Alerts");
        se_row *r = tog(T_NBANNER, "Banners", se_get(SK_NBANNER + i));
        if (!se_get(SK_NALLOW + i)) se_off(r, "Turn on Allow Notifications first");
        val("Notification Centre", se_get(SK_NALLOW + i) ? "On" : "Off");
        foot("Off: nothing from it is shown. Banners off: it goes to the Notification Centre only.");
        break;
    }
    case SE_BATTERY:
        sec(0);
        row(SR_BIG, A_NONE, se_num(ST.pct, " %"), SYS.charging ? "Charging" : "On battery");
        sec(0);
        val("Battery Temperature", se_str(live_text("@temp", b), 0));
        {
            int mx = 0, cy = 0;
            char w[160];
            nav(SE_HEALTH, 0, "Battery Health", sh_battery_health(&mx, &cy, w, sizeof w) ? se_num(mx, " %") : "Not Available");
        }
        sec(BH.range ? "Since the phone started" : "Last hour");
        {
            static const char *const RG[2] = { "1 Hour", "24 Hours" };
            se_row *r = row(SR_SEG, A_RANGE, 0, 0);
            r->on = BH.range;
            r->v = (const char *)RG;
        }
        row(SR_GRAPH, A_NONE, 0, 0);
        foot(BH.n > 1 ? "Kept in memory since the phone started (a value each 10 minutes); green while charging." :
                        "Nothing recorded yet: the level is kept in memory each 10 minutes from the phone's start.");
        sec(0);
        nav(A_LAST2, 0, "Last 2 %", sh_last2() ? "On" : "Off");
        tog(T_LAST2AUTO, "Last 2 % at 3 %", sh_last2_auto());
        foot("Everything off but the clock, the torch and SOS. Kept with the phone's settings.");
        break;
    case SE_PRIVACY:
        sec(0);
        val("Wi-Fi", WF_ON() ? "On" : "Off");
        val("Cellular Data", data_on() ? "On" : "Off");
        val("Location", "Kept on the phone");
        val("Analytics", "None");
        val("Accounts", "None");
        foot("Nothing leaves this phone except what you turn on; Wi-Fi and Cellular Data are off at every start. No "
             "accounts, no analytics, no tracking: your notes, cards and places stay on the phone.");
        break;
    case SE_EMERGENCY:
        sec(0);
        { se_row *r = nav(A_CARD, SEI_HEART, "Medical Card", 0); (void)r; }
        tog(A_EMG, "Emergency Mode", EMG);
        foot("Emergency mode puts the survival tools on the lock screen and home: SOS, the torch, the compass, the guide, "
             "Last 2 % and USB power.");
        sec(0);
        val("Emergency SOS", "Hold on the lock screen");
        tog(T_LAST2AUTO, "Last 2 % at 3 %", sh_last2_auto());
        break;
    case SE_LINUX: {
        int nl = 0, run = 0;
        for (int i = 0; i < LCAT.n; i++) { if (!(LCAT.app[i].flags & SH_LAPP_SYSTEM)) nl++; if (LCAT.app[i].flags & SH_LAPP_RUNNING) run++; }
        sec(0);
        val("Linux", LX.state == SH_LINUX_RUNNING ? "Running" : LX.state == SH_LINUX_STARTING ? "Starting\xe2\x80\xa6" :
                     LX.state == SH_LINUX_FAILED ? "Could not start" : "Off");
        val("Apps", se_num(nl, 0));
        val("Running", se_num(run, 0));
        if (LCAT.mem_total_kb) val("Memory", se_num((int)(LCAT.mem_total_kb >> 10), " MB"));
        sec(0);
        nav(A_STORE, SEI_STORE, "App Store", 0);
        nav(A_TERMINAL, SEI_LINUX, "Terminal", 0);
        foot("Linux apps run on the phone's big cores in their own window. Install them from the App Store.");
        break;
    }
    case SE_BT: {
        /* Track A's Bluetooth: scanning only. The switch is on while starting (1) and on (2); a failure (-1) shows off
         * with the kernel's words. Other Devices: the named ones strongest first, as the list stood at this build */
        int st = sh_bt_state();
        SEB.st = st;
        SEB.cnt = 0;
        sec(0);
        tog(T_BT, "Bluetooth", st == 1 || st == 2);
        if (st == 1) foot("Turning On\xe2\x80\xa6 The Bluetooth chip loads its firmware, about 10 seconds.");
        else if (st < 0) {
            const char *why = sh_bt_why();
            foot(se_sentence("Bluetooth didn't start. ", why && why[0] ? why : "The chip didn't answer."));
        } else foot("Starbite OS can pair one Bluetooth keyboard. Audio and file transfer aren't in this version yet.");
        if (st != 2) break;
        /* cp7: My Devices: the paired keyboard (one), connected or not; a tap: Connect / Forget This Device */
        int paired = sh_bt_kbd_paired();
        if (paired) {
            sec("My Devices");
            row(SR_VAL, A_BTKBD, se_str(se_bt_kname(), 0), sh_bt_kbd_connected() ? "Connected" : "Not Connected");
        }
        sec(SE_BT_HEAD);
        int n = sh_bt_count(), named = 0, unnamed = 0;
        SEB.cnt = n;
        for (int i = 0; i < n && i < 64; i++) {
            char nm[48];
            int rssi = 0;
            if (sh_bt_get(i, nm, sizeof nm, &rssi) < 0) continue;
            if (!nm[0] || named >= SE_BT_MAXD) { unnamed++; continue; }   /* iOS lists named devices: the rest counted */
            if (paired && !tk_strcmp(nm, se_bt_kname())) continue;   /* it is under My Devices */
            int busy = SBP.dev == i && SBP.st != SH_BTP_FAILED;
            se_row *r = row(SR_VAL, A_BTDEV, se_str(nm, 0), busy ? "Connecting\xe2\x80\xa6" : "");
            r->dev = i;
            named++;
        }
        if (!n) foot("Looking for devices nearby\xe2\x80\xa6 The scan runs while this page is open.");
        else if (unnamed) {
            char b[12];
            put_dec(b, unnamed);
            foot(se_str(b, unnamed == 1 ? " more device without a name. Tap a keyboard to pair it." :
                                          " more devices without a name. Tap a keyboard to pair it."));
        } else foot("Tap a keyboard to pair it. Pairing works for keyboards only in this version.");
        break;
    }
    case SE_HEALTH: {
        /* Track A's battery health: the fuel gauge's capacity estimate and cycle count, or the kernel's words why not */
        int mx = 0, cy = 0;
        char w[160] = "";
        int ok = sh_battery_health(&mx, &cy, w, sizeof w);
        sec(0);
        val("Maximum Capacity", ok ? se_num(mx, " %") : "Not Available");
        foot("This is a measure of battery capacity relative to when it was new. Lower capacity may result in fewer hours "
             "of usage between charges.");
        sec(0);
        val("Cycle Count", ok && cy >= 0 ? se_num(cy, 0) : "Not Available");
        if (ok) foot(se_sentence(w[0] ? w : "Read from the battery's charge chip.", 0));
        else foot(se_sentence("Starbite OS can't read the battery's wear: ", w[0] ? w : "the charge chip didn't answer."));
        break;
    }
    case SE_SEARCH: {
        /* the pages and settings whose names hold what is typed (all of them while it is empty: the first seven) */
        static const struct { const char *t; int id, ico; const char *where; } IX[] = {
            { "Wi-Fi", SE_WIFI, SEI_WIFI, 0 }, { "Bluetooth", SE_BT, SEI_BT, 0 }, { "Cellular", SE_CELL, SEI_CELL, 0 },
            { "Battery", SE_BATTERY, SEI_BATT, 0 },
            { "General", SE_GENERAL, SEI_GENERAL, 0 }, { "Display & Brightness", SE_DISPLAY, SEI_DISPLAY, 0 },
            { "Wallpaper", SE_WALL, SEI_WALL, 0 }, { "Sounds & Haptics", SE_SOUNDS, SEI_SOUNDS, 0 },
            { "Notifications", SE_NOTIF, SEI_NOTIF, 0 }, { "Passcode", A_PASSCODE, SEI_PASS, 0 }, { "Emergency", SE_EMERGENCY, SEI_SOS, 0 },
            { "Privacy", SE_PRIVACY, SEI_PRIV, 0 }, { "Startup", A_STARTUP, SEI_START, 0 }, { "Linux Apps", SE_LINUX, SEI_LINUX, 0 },
            { "App Store", A_STORE, SEI_STORE, 0 }, { "About", SE_ABOUT, 0, "General" }, { "Software Update", A_UPDATE, 0, "General" },
            { "Date & Time", SE_DATETIME, 0, "General" }, { "Keyboard", SE_KEYBOARD, 0, "General" },
            { "Language & Region", SE_LANG, 0, "General" }, { "Reset", SE_RESET, 0, "General" }, { "Brightness", SE_DISPLAY, 0, "Display" },
            { "Auto-Lock", SE_DISPLAY, 0, "Display" }, { "Time Zone", SE_DATETIME, 0, "General" }, { "Text Size", SE_DISPLAY, 0, "Display" }, { "Haptics", SE_SOUNDS, 0, "Sounds" },
            { "Vibration", SE_SOUNDS, 0, "Sounds" }, { "Medical Card", A_CARD, SEI_HEART, "Emergency" },
            { "Last 2 %", SE_BATTERY, 0, "Battery" }, { "Battery Health", SE_HEALTH, 0, "Battery" },
            { "Auto-Brightness", SE_DISPLAY, 0, "Display" }, { "Haptic Strength", SE_SOUNDS, 0, "Sounds" } };
        sec(0);
        int n = 0;
        for (unsigned i = 0; i < sizeof IX / sizeof IX[0] && n < 7; i++)
            if (ci_has(IX[i].t, SE.q)) { nav(IX[i].id, IX[i].ico, IX[i].t, IX[i].where); n++; }
        if (!n) foot("No Results");
        break;
    }
    default: break;
    }
}

/* ---- drawing ---------------------------------------------------------------------------------------------------- */
static float se_row_h(const se_row *r)
{
    switch (r->kind) {
    case SR_PROFILE: return 66;                   /* 6: the account row, a 56 avatar */
    case SR_NAV: return r->sub ? 66 : LG_ROW;     /* a row with a subtitle */
    case SR_SLIDER: return 60;
    case SR_SEG: return LG_ROW;
    case SR_BIG: return 96;
    case SR_GRAPH: return 150;
    case SR_WALL: return 300;
    default: return LG_ROW;
    }
}
static float se_layout(void)
{
    float y = SE.page == SE_ROOT ? CONTENT_TOP - 4 : PUSHED_TOP - 8;
    for (int i = 0; i < SE.ns; i++) {
        se_sec *s = &SE.s[i];
        if (i) y += LG_GAP + (s->head && SE.s[i - 1].foot ? 12 : 0);   /* 35 between cards, a header in it (more after words) */
        else if (s->head) y += 26;
        s->y = y;
        float h = 0;
        for (int k = 0; k < s->n; k++) {
            se_row *r = &SE.r[s->first + k];
            r->y = y + h;
            r->h = se_row_h(r);
            h += r->h;
        }
        s->h = h;
        y += h;
        s->fy = y;
        y += d_group_foot(0, y, s->foot);
    }
    return y + 40;
}
/* the gear (General): a ring and eight teeth */
static void se_gear(dc *d, float cx, float cy, tk_col c)
{
    for (int i = 0; i < 8; i++) {
        float a = .7853982f * i, s = tk_sinf(a), co = tk_cosf(a);
        d_seg(d, cx + s * 6.5f, cy - co * 6.5f, cx + s * 9, cy - co * 9, 3.4f, c);
    }
    int bx = (int)tk_floorf(PT(cx - 10) - d->ox), by = (int)tk_floorf(PT(cy - 10) - d->oy), n = (int)PT(20) + 4;
    tk_fm m;
    if (!tk_fm_new(&m, n, n)) return;
    tk_fm_arc(&m, d->ox + bx, d->oy + by, PT(cx), PT(cy), PT(5.2f), PT(3), 0, 6.2831853f, 0);
    tk_paint p = tk_solid(c);
    tk_draw_fm(d->b, &m, bx, by, &p, 1);
    tk_fm_free(&m);
}
static void se_glyph(dc *d, const char *name, float cx, float cy)
{
    const tk_bmp *ic = tk_icon(name);
    if (!ic) return;
    tk_blit_tinted(d->b, ic, (int)tk_floorf(PT(cx) - ic->w / 2.0f - d->ox + .5f), (int)tk_floorf(PT(cy) - ic->h / 2.0f - d->oy + .5f), W(1));
}
/* iOS Settings' 30 pt tile (r 7.5) with its white glyph, centred in the row */
static void se_tile(dc *d, float ry, float rh, int ico)
{
    static const uint32_t COL[] = { 0, 0x0a84ff, 0x30d158, 0x30d158, 0x8e8e93, 0x0a84ff, 0x32ade6, 0xff375f, 0xff3b30, 0xff3b30,
                                    0xff3b30, 0x0a84ff, 0x636366, 0x2c2c2e, 0x0a84ff, 0xff3b30, 0x0a84ff };
    if (ico <= SEI_NONE || ico > SEI_BT) return;
    d_icon_tile(d, ry, rh, COL[ico]);
    float x = LG_X + 16 + .5f, y = ry + (rh - LG_ICON) / 2 + .5f;   /* the glyphs below are placed in a 29 pt box */
    float cx = LG_X + 16 + LG_ICON / 2, cy = ry + rh / 2;
    switch (ico) {
    case SEI_WIFI: se_glyph(d, "cc-wifi", cx, cy); break;
    case SEI_BT: se_glyph(d, "cc-bt", cx, cy); break;          /* the Control Centre's glyph (no new icon) */
    case SEI_CELL: se_glyph(d, "cc-cell", cx, cy); break;
    case SEI_BATT: se_glyph(d, "cc-save", cx, cy); break;
    case SEI_DISPLAY: se_glyph(d, "cc-sun", cx, cy); break;
    case SEI_SOUNDS: se_glyph(d, "cc-vol", cx, cy); break;
    case SEI_WALL: se_glyph(d, "home-star", cx, cy); break;
    case SEI_PASS: se_glyph(d, "lock-c", cx, cy); break;
    case SEI_START: se_glyph(d, "recovery", cx, cy); break;
    case SEI_HEART: se_glyph(d, "heart", cx, cy); break;
    case SEI_GENERAL: se_gear(d, cx, cy, W(1)); break;
    case SEI_NOTIF:
        d_rrect(d, x + 6, y + 8, 17, 15, 4, W(1));
        d_rrect(d, x + 17.5f, y + 4.5f, 8, 8, 4, tk_hex(COL[ico], 1));
        d_rrect(d, x + 19, y + 6, 5, 5, 2.5f, W(1));
        break;
    case SEI_SOS: d_text_box(d, TK_IT700, 9, cx, cy + 3.3f, 28, 1, "SOS", 0, 0, W(1), 0); break;
    case SEI_PRIV:
        /* a hand: the palm and four fingers */
        d_rrect(d, x + 8.5f, y + 13, 12, 10, 5, W(1));
        for (int i = 0; i < 4; i++) d_rrect(d, x + 8.5f + 3.1f * i, y + 6.5f + (i == 0 || i == 3 ? 2 : 0), 2.6f, 10, 1.3f, W(1));
        d_seg(d, x + 8, y + 16, x + 5.5f, y + 12.5f, 2.4f, W(1));
        break;
    case SEI_LINUX: d_text_box(d, TK_IT700, 13, cx, cy + 4.6f, 28, 1, ">_", 0, 0, W(1), 0); break;
    case SEI_STORE:
        d_seg(d, cx - 5, cy + 6, cx + 1.5f, cy - 6, 2.2f, W(1));
        d_seg(d, cx + 5, cy + 6, cx - 1.5f, cy - 6, 2.2f, W(1));
        d_seg(d, cx - 7.5f, cy + 2.5f, cx + 7.5f, cy + 2.5f, 2.2f, W(1));
        break;
    default: break;
    }
}
#define SE_SWX (LG_RE - 16 - KSW_W)
static void se_graph(dc *d, const se_row *r)
{
    float x0 = LG_X + 16, x1 = LG_RE - 16, top = r->y + 16, bot = r->y + r->h - 26, w = x1 - x0;
    for (int k = 0; k <= 2; k++) {
        float gy = top + (bot - top) * k / 2;
        d_rrect(d, x0, gy, w, .5f, 0, C_SEP);
        d_text(d, TK_IT400, 11, x1 - tw(TK_IT400, 11, k == 0 ? "100 %" : k == 1 ? "50 %" : "0 %", 0, 0), gy - 3,
               k == 0 ? "100 %" : k == 1 ? "50 %" : "0 %", 0, 0, W(.4f), 0);
    }
    int span = BH.range ? 24 * 60 : 60, now = BH.n ? BH.min[BH.n - 1] : ST.h * 60 + ST.mi;
    float bw = BH.range ? w / 144 : w / 60 * 10;
    int drawn = 0;
    for (int i = 0; i < BH.n; i++) {
        int age = now - BH.min[i];
        if (age < 0) age += 24 * 60;
        if (age >= span) continue;
        float x = x1 - 40 - (age + 10) * (w - 40) / span, hgt = (bot - top) * BH.pct[i] / 100;
        if (x < x0) continue;
        d_rrect(d, x, bot - hgt, tk_maxf(2, bw - 1.5f), hgt, tk_minf(2, bw / 2), BH.chg[i] ? KIT_GREEN() : W(.75f));
        drawn++;
    }
    if (!drawn) d_text_box(d, TK_IT400, 13, (x0 + x1 - 40) / 2, (top + bot) / 2 + 4, w - 60, 1, "No values yet", 0, 0, C_LABEL2, 0);
    d_text(d, TK_IT400, 11, x0, r->y + r->h - 10, BH.range ? "24 h ago" : "1 h ago", 0, 0, C_LABEL2, 0);
    d_text(d, TK_IT400, 11, x1 - 40 - tw(TK_IT400, 11, "Now", 0, 0), r->y + r->h - 10, "Now", 0, 0, C_LABEL2, 0);
}
static void se_wall(dc *d, const se_row *r)
{
    /* the lock and home as thumbnails (the half-resolution wallpaper scaled down; no blur) */
    float tw0 = 120, th = tw0 * H_PT / 390;
    for (int k = 0; k < 2; k++) {
        float x = 195 + (k ? 12 : -12 - tw0), y = r->y + 18;
        int X = (int)tk_floorf(PT(x) - d->ox), Y = (int)tk_floorf(PT(y) - d->oy), Wd = (int)PT(tw0), Hd = (int)PT(th);
        tk_bmp t;
        if (C->base_half.px && tk_bmp_new(&t, Wd, Hd)) {
            tk_bmp_up(&t, &C->base_half, 0, 0, (float)Wd / C->base_half.w);
            tk_bmp_copy(d->b, X, Y, &t, 0, 0, Wd, Hd);
            tk_bmp_free(&t);
        }
        if (k == 0) {
            char hm[8];
            fmt_hm(hm);
            d_text_box(d, TK_IT600, 30, x + tw0 / 2, y + 60, tw0, 1, hm, 0, TK_TNUM, INK, 1);
        } else
            for (int i = 0; i < 8; i++) d_rrect(d, x + 14 + 24 * (i % 4), y + 40 + 28 * (i / 4), 18, 18, 5, W(.3f));
        d_text_box(d, TK_IT400, 13, x + tw0 / 2, y + th + 18, tw0, 1, k ? "Home Screen" : "Lock Screen", 0, 0, C_LABEL2, 0);
    }
}
static void se_draw_row(dc *d, const se_row *r, int last)
{
    float y = r->y;
    tk_col lc = r->dis ? C_LABEL3 : INK;
    if (r->ico) se_tile(d, y, r->h, r->ico);
    switch (r->kind) {
    case SR_PROFILE: {
        /* the account row: iOS's initials avatar (56: systemGray's gradient, white initials 22 / 600; 8a, gaps-c #8:
         * the Starbite tile was missing from the pack and left an empty navy circle), the device's name 20 / 600,
         * "Starbite OS" */
        float ax = LG_X + 12, ay = y + 5;
        {
            tk_stop gs[2] = { { 0, tk_hex(0xa5a5ab, 1) }, { 1, tk_hex(0x6c6c72, 1) } };
            tk_paint gp = tk_linear(180, PT(ax) - d->ox, PT(ay) - d->oy, PT(56), PT(56), 2, gs);
            tk_fm m;
            int bx = (int)tk_floorf(PT(ax) - d->ox) - 1, by = (int)tk_floorf(PT(ay) - d->oy) - 1, n = (int)PT(56) + 3;
            if (tk_fm_new(&m, n, n)) {
                tk_fm_rrect(&m, d->ox + bx, d->oy + by, PT(ax), PT(ay), PT(56), PT(56), PT(28));
                tk_draw_fm(d->b, &m, bx, by, &gp, 1);
                tk_fm_free(&m);
            }
            float iw = tw(TK_IT600, 22, "S9", 0, 0);
            d_text(d, TK_IT600, 22, ax + 28 - iw / 2, ay + 28 + 22 * INTER_CAP / 2, "S9", 0, 0, W(1), 0);
        }
        d_text_box(d, TK_IT600, 20, ax + 56 + 13, y + 30, LG_RE - 40 - ax - 69, 0, r->l, 0, 0, INK, 0);
        d_text_box(d, TK_IT400, 15, ax + 56 + 13, y + 50, LG_RE - 40 - ax - 69, 0, r->v, 0, 0, KIT_SEC(), 0);
        d_icon(d, "chev", LG_RE - 16 - 8, y + r->h / 2 - 6.5f, .35f);
        return;
    }
    case SR_BIG:
        d_text(d, TK_IT700, 44, LG_TX, y + 58, r->l, 0, TK_TNUM, INK, 0);
        d_text(d, TK_IT400, 15, LG_TX, y + 82, r->v, 0, 0, SYS.charging ? KIT_GREEN() : C_LABEL2, 0);
        {
            /* a battery glyph at the right: the body, its fill */
            float bx = LG_RE - 16 - 72, by = y + 32;
            d_rrect(d, bx, by, 66, 32, 8, W(.25f));
            d_rrect(d, bx + 3, by + 3, 60 * tk_clampf(ST.pct / 100.0f, .04f, 1), 26, 5.5f,
                    ST.pct <= 20 && !SYS.charging ? KIT_RED() : SYS.charging ? KIT_GREEN() : W(1));
            d_rrect(d, bx + 67.5f, by + 11, 3.5f, 10, 1.75f, W(.35f));
        }
        return;
    case SR_SLIDER:
        if (r->id == A_VOLUME) {                  /* cp7w: the volume's (a speaker small and loud) */
            d_icon_s(d, "cc-vol", LG_TX - 4, y + 19, .6f);
            kit_d_slider(d, LG_TX + 30, y + 16, LG_RE - 16 - 30 - LG_TX - 30, SE.vol);
            d_icon(d, "cc-vol", LG_RE - 16 - 22, y + 19, 1);
            return;
        }
        d_icon(d, "cc-sun", LG_TX - 4, y + 19, .45f);
        kit_d_slider(d, LG_TX + 30, y + 16, LG_RE - 16 - 30 - LG_TX - 30, SE.bright);
        d_icon(d, "cc-sun", LG_RE - 16 - 22, y + 19, 1);
        return;
    case SR_SEG:
        kit_d_segment(d, LG_TX, y + 10, LG_RE - 16 - LG_TX, (const char *const *)(const void *)r->v, r->segn ? r->segn : 2, r->on);
        return;
    case SR_GRAPH: se_graph(d, r); return;
    case SR_WALL: se_wall(d, r); return;
    default: break;
    }
    tk_col ac = r->red ? KIT_RED() : KIT_BLUE();
    if (r->sub) {
        /* two lines: the label 17 / 400, the subtitle 15 / 400 secondary, the chevron and the rule as any row's */
        float tx = r->ico != SEI_NONE ? LG_ITX : LG_TX;
        d_text_box(d, TK_IT400, 17, tx, y + 28, LG_RE - 40 - tx, 0, r->l, 0, 0, lc, 0);
        d_text_box(d, TK_IT400, 15, tx, y + 48, LG_RE - 40 - tx, 0, r->sub, 0, 0, KIT_SEC(), 0);
        d_row(d, y, r->h, r->ico != SEI_NONE, "", r->v, 1, last, lc, 0);
        return;
    }
    d_row(d, y, r->h, r->ico != SEI_NONE, r->l, r->v, r->kind == SR_NAV, last, r->kind == SR_ACT ? ac : lc,
          r->kind == SR_TOG ? SE_SWX - 12 : 0);
    if (r->kind == SR_TOG) kit_d_switch(d, SE_SWX, y + (r->h - KSW_H) / 2, r->on, !r->dis);
}
static void se_draw_sec(dc *d, int i)
{
    se_sec *s = &SE.s[i];
    if (s->head) d_group_head(d, s->y, s->head);       /* sentence case, 15 / 600 */
    if (s->n) d_group(d, s->y, s->h);
    for (int k = 0; k < s->n; k++) se_draw_row(d, &SE.r[s->first + k], k + 1 == s->n);
    if (s->foot) d_group_foot(d, s->fy, s->foot);
}
/* one group again (a switch's new state): its rect cleared and drawn over in each chunk it crosses */
static void se_redraw_sec(int i)
{
    se_sec *s = &SE.s[i];
    for (int k = 0; k < SE_NCH; k++) {
        tk_layer *l = &sepg[k];
        if (!l->bmp.px) continue;
        dc d = { &l->bmp, 0, l->bx, l->by };
        int y0 = tk_maxi(0, (int)tk_floorf(PT(s->y) - d.oy) - 1), y1 = tk_mini(l->bmp.h, (int)(PT(s->y + s->h) - d.oy) + 2);
        int x0 = tk_maxi(0, (int)tk_floorf(PT(LG_X) - d.ox) - 1), x1 = tk_mini(l->bmp.w, (int)(PT(LG_RE) - d.ox) + 2);
        if (y1 <= y0 || x1 <= x0) continue;
        tk_fill_rect_raw(&l->bmp, x0, y0, x1 - x0, y1 - y0, 0);
        const char *f = s->foot, *h = s->head;
        s->foot = 0; s->head = 0;                     /* the words around it stay as they are */
        se_draw_sec(&d, i);
        s->foot = f; s->head = h;
        tk_layer_dirty(l);
    }
}
/* one footer again (Display's lux reading): its lines cleared and drawn over in each chunk it crosses */
static void se_redraw_foot(int i, float h)
{
    se_sec *s = &SE.s[i];
    for (int k = 0; k < SE_NCH; k++) {
        tk_layer *l = &sepg[k];
        if (!l->bmp.px) continue;
        dc d = { &l->bmp, 0, l->bx, l->by };
        int y0 = tk_maxi(0, (int)tk_floorf(PT(s->fy) - d.oy)), y1 = tk_mini(l->bmp.h, (int)(PT(s->fy + h + 6) - d.oy) + 1);
        int x0 = tk_maxi(0, (int)tk_floorf(PT(LG_X) - d.ox)), x1 = tk_mini(l->bmp.w, (int)(PT(LG_RE) - d.ox) + 1);
        if (y1 <= y0 || x1 <= x0) continue;
        tk_fill_rect_raw(&l->bmp, x0, y0, x1 - x0, y1 - y0, 0);
        d_group_foot(&d, s->fy, s->foot);
        tk_layer_dirty(l);
    }
}
/* the lux reading changed: its footer alone (the page again if its lines changed) */
static void se_build(void);
static void se_lux_redraw(int lux)
{
    se_lux_text(lux);
    if ((int)d_group_foot(0, 0, se_lux_foot) != SEB.lux_h) { se_build(); return; }
    for (int i = 0; i < SE.ns; i++) if (SE.s[i].foot == se_lux_foot) se_redraw_foot(i, (float)SEB.lux_h);
}
/* iOS's activity indicator (8 spokes, the bright one going round clockwise) in a 22 pt layer of the scroll group:
 * beside the switch while Bluetooth starts, beside Other Devices' header while it scans */
static void se_spin_paint(tk_layer *l, float cx, float cy, int k)
{
    if (!l->bmp.px) return;
    dc d = { &l->bmp, 0, l->bx, l->by };
    tk_fill_rect_raw(&l->bmp, 0, 0, l->bmp.w, l->bmp.h, 0);
    for (int i = 0; i < 8; i++) {
        float a = .7853982f * i, sn = tk_sinf(a), co = tk_cosf(a);
        int age = (k - i + 8) & 7;
        d_seg(&d, cx + sn * 4.4f, cy - co * 4.4f, cx + sn * 8.4f, cy - co * 8.4f, 2.4f, TK_RGBA(235, 235, 245, .9f - .09f * age));
    }
    tk_layer_dirty(l);
}
static void se_spin_draw(void) { se_spin_paint(&sespin, SEB.spin_cx, SEB.spin_cy, SEB.spin_k); }
/* cp6: the same spinner over the strip pages (not scrolled: screen pt), where something loads: Wi-Fi starting /
 * scanning / joining (shell_wifi.inc's wf_spin_at), Cellular connecting (cell_spin_at), Software Update checking
 * (shell_update.inc's su_spin_at); stepped on the platform's refresh turns as Bluetooth's */
static tk_layer sepspin;
static struct { int on, k; float t, cx, cy; } SPS;
static int su_spin_at(float *cx, float *cy);
static int se_strip_spin_where(float *cx, float *cy)
{
    if (!AP.on || AP.closing || !is_app("settings") || !se_awake()) return 0;
    if (AS.sub == SE_WIFI) return wf_spin_at(cx, cy);
    if (AS.sub == SE_CELL) return cell_spin_at(cx, cy);
    if (AS.sub == SU_SUB) return su_spin_at(cx, cy);
    return 0;
}
/* where the page wants it now: 1 when the layer changed (the caller relinks) */
static int se_strip_spin_set(void)
{
    float cx = 0, cy = 0;
    int want = sepspin.name && se_strip_spin_where(&cx, &cy);
    if (!want) {
        int was = SPS.on;
        if (sepspin.bmp.px) relayer(&sepspin);
        sepspin.hidden = 1;
        SPS.on = 0;
        return was;
    }
    if (!sepspin.bmp.px || cx != SPS.cx || cy != SPS.cy) {
        relayer(&sepspin);
        SPS.cx = cx;
        SPS.cy = cy;
        (void)mk(&sepspin, "settings-strip-spin", cx - 11, cy - 11, 22, 22, 0, 0);
    }
    sepspin.hidden = !sepspin.bmp.px;
    SPS.on = 1;
    se_spin_paint(&sepspin, SPS.cx, SPS.cy, SPS.k);
    return 1;
}
static int se_spin_want(void) { return SE.page == SE_BT && se_awake() && (SEB.st == 1 || (SEB.st == 2 && SEB.scan)); }
static int se_spin_where(float *cx, float *cy)
{
    for (int i = 0; i < SE.nr; i++)
        if (SEB.st == 1 && SE.r[i].id == T_BT) { *cx = SE_SWX - 22; *cy = SE.r[i].y + SE.r[i].h / 2; return 1; }
    for (int i = 0; i < SE.ns; i++)
        if (SEB.st == 2 && SE.s[i].head && !tk_strcmp(SE.s[i].head, SE_BT_HEAD)) {
            *cx = LG_TX + tw(TK_IT600, 15, SE_BT_HEAD, 0, 0) + 8 + 10;
            *cy = SE.s[i].y - 18;
            return 1;
        }
    return 0;
}
/* the spinner where the page wants it (the caller relinks) */
static void se_spin_set(int want)
{
    float cx = 0, cy = 0;
    if (want) want = se_spin_where(&cx, &cy);
    if (!want) { relayer(&sespin); sespin.hidden = 1; SEB.spin = 0; return; }
    if (!sespin.bmp.px || cx != SEB.spin_cx || cy != SEB.spin_cy) {
        relayer(&sespin);
        SEB.spin_cx = cx;
        SEB.spin_cy = cy;
        (void)mk(&sespin, "settings-spin", cx - 11, cy - 11, 22, 22, 0, 0);
    }
    sespin.hidden = !sespin.bmp.px;
    SEB.spin = 1;
    se_spin_draw();
}
/* the Bluetooth page left (another page, the app closed): its scan stops (Bluetooth itself stays as it is, iOS) */
static void se_bt_leave(void)
{
    if (SEB.scan) sh_bt_scan(0);
    SEB.scan = 0;
    SEB.scan_t = -1;
    relayer(&sespin);
    sespin.hidden = 1;
    SEB.spin = 0;
}

/* ---- the layers ------------------------------------------------------------------------------------------------- */
static int se_owns(void)
{
    return is_app("settings") && AS.sub != SE_WIFI && AS.sub != SE_CELL && AS.sub != SE_CARD && AS.sub != SE_STARTUP_PAGE &&
           AS.sub != SE_PASSCODE_PAGE && AS.sub != SE_TZ_PAGE && AS.sub != SU_SUB;   /* (SU_SUB: Track A update) */
}
/* a strip page's back (Startup, Passcode): the Settings root, where it was */
void settings_back(void)
{
    if (!is_app("settings")) return;
    AS.sub = SE_ROOT;
    build_app_page(AP.a);
}
static int se_back_from_card(void) { return SE_EMERGENCY; }        /* the Medical Card's editor sits under Emergency */
static float se_max(void) { return tk_maxf(0, SE.content_h - H_PT + 20); }
static void se_place(void)
{
    sescr.y = -tk_floorf(PT(SE.scroll) + .5f);
    /* the root's inline title comes in as the large one goes under the nav row */
    if (SE.page == SE_ROOT) senav.alpha = tk_clampf((SE.scroll - 30) / 20, 0, 1);
    else senav.alpha = 1;
    senav.hidden = !senav.bmp.px || senav.alpha <= 0;
}
/* the search capsule (root) or the field over the keyboard (Search): glass (the lens), the glyph, the words */
static void se_search_build(void)
{
    relayer(&sesb);
    tk_layer_init(&sesb, "settings-search");
    sesb.hidden = 1;
    relayer(&seedge);
    tk_layer_init(&seedge, "settings-edge");
    seedge.hidden = 1;
    if (SE.page == SE_ROOT) {
        int h = (int)PT(H_PT - SE_EDGE_Y) / 2 + 1, top = (int)tk_floorf(PT(SE_EDGE_Y));
        if (tk_bmp_new(&seedge.bmp, TK_W / 2, h)) {
            for (int j = 0; j < h; j++) {
                float t = tk_clampf((j * 2.0f / TK_PT) / 54, 0, 1);         /* 0 at the edge's top, 1 at 54 pt */
                unsigned a = (unsigned)(255 * t * t * (3 - 2 * t) + .5f);
                for (int i = 0; i < seedge.bmp.w; i++) seedge.bmp.px[j * seedge.bmp.stride + i] = a << 24;
            }
            seedge.sx = seedge.sy = 2;
            seedge.x = seedge.ox = 0;
            seedge.y = seedge.oy = (float)top;
            seedge.hidden = 0;
        }
    }
    if (SE.page != SE_ROOT && SE.page != SE_SEARCH) return;
    int pg = SE.page == SE_SEARCH;
    float y = pg ? SE_SF_Y : SE_SB_Y, w = pg ? CONTENT_W - 52 : CONTENT_W;
    dc d = mk(&sesb, "settings-search", MARGIN, y, CONTENT_W, 48, 0, 0);
    if (!d.b->px) return;
    if (tk_glass_lens) {
        kit_shape(&sesb, PT(MARGIN) - d.ox, PT(y) - d.oy, PT(w), PT(48), PT(24));
        if (pg) kit_shape(&sesb, PT(CONTENT_R - 44) - d.ox, PT(y + 2) - d.oy, PT(44), PT(44), PT(22));
    } else {
        d_lg_flat(&d, MARGIN, y, w, 48, 24, 1);
        if (pg) d_lg_flat(&d, CONTENT_R - 44, y + 2, 44, 44, 22, 1);
    }
    d_rrect(&d, MARGIN, y, w, 48, 24, tk_hex(0x1e1e20, .35f));
    const tk_bmp *ic = tk_icon("search15");
    if (ic) tk_blit_tinted(d.b, ic, (int)tk_floorf(PT(MARGIN + 16) - d.ox + .5f), (int)tk_floorf(PT(y + 24) - ic->h / 2.0f - d.oy + .5f), W(.6f));
    const char *t = pg && SE.q[0] ? SE.q : "Search";
    d_text_box(&d, TK_IT400, 17, MARGIN + 42, y + 30, w - 58, 0, t, 0, 0, pg && SE.q[0] ? INK : C_LABEL2, 0);
    if (pg && SE.q[0]) d_rrect(&d, MARGIN + 43 + tw(TK_IT400, 17, SE.q, 0, 0), y + 13, 2, 22, 1, KIT_BLUE());   /* the caret */
    else if (pg) d_rrect(&d, MARGIN + 41, y + 13, 2, 22, 1, KIT_BLUE());
    if (pg) {
        float cx = CONTENT_R - 22, cy = y + 24;
        d_seg(&d, cx - 6, cy - 6, cx + 6, cy + 6, 2.2f, INK);
        d_seg(&d, cx - 6, cy + 6, cx + 6, cy - 6, 2.2f, INK);
    }
    sesb.hidden = 0;
}
static void se_kb_build(void)
{
    relayer(&sekb);
    tk_layer_init(&sekb, "settings-keyboard");
    sekb.hidden = 1;
    if (SE.page != SE_SEARCH) return;
    dc d = mk(&sekb, "settings-keyboard", 0, 540, 390, 262, 0, 0);
    if (!d.b->px) return;
    kb_draw(&d, SE.shift, SE.num);
    sekb.hidden = 0;
}
static void se_free(void)
{
    for (int k = 0; k < SE_NCH; k++) { relayer(&sepg[k]); sepg[k].hidden = 1; }
    relayer(&senav); relayer(&sesb); relayer(&sekb); relayer(&seedge);
    senav.hidden = sesb.hidden = sekb.hidden = seedge.hidden = 1;
    seclip.hidden = 1;
    kit_slider_end();
    relayer(&ksw); ksw.hidden = 1;
    se_bt_leave();
    SE.built = 0;
}
static void se_build(void)
{
    if (!seclip.name) {
        tk_layer_init(&seclip, "settings-clip");
        tk_layer_init(&sescr, "settings-scroll");
        for (int k = 0; k < SE_NCH; k++) { tk_layer_init(&sepg[k], "settings-page"); sepg[k].hidden = 1; }
        tk_layer_init(&senav, "settings-nav");
        tk_layer_init(&sespin, "settings-spin");
        sespin.hidden = 1;
        tk_layer_init(&sepspin, "settings-strip-spin");   /* cp6 */
        sepspin.hidden = 1;
    }
    (void)se_strip_spin_set();                    /* cp6: a strip page's spinner (or none) */
    if (!se_owns()) { se_free(); return; }
    int p = AS.sub;
    if (SE.bld_page != p || !SE.built) {
        if (p == SE_SEARCH && !SE.popping) { SE.q[0] = 0; SE.shift = 1; SE.num = 0; relayer(&sekb); }   /* Search opens empty */
        if (SE.built && SE.bld_page >= 0 && SE.bld_page < SE_NPAGES) SE.save[SE.bld_page] = SE.scroll;
        SE.scroll = (SE.popping || p == SE_ROOT) && p < SE_NPAGES ? SE.save[p] : 0;   /* back: where it was */
        SE.popping = 0;
        if (p == SE_ROOT) SE.ns_ = 0;
        SE.momentum = 0;
        SE.sp.live = 0;
    }
    SE.page = p;
    SE.bld_page = p;
    if (p != SE_BT) se_bt_leave();
    else SEB.build_t = now_ms;
    if (p == SE_DISPLAY && !SE.slider) SE.bright = ((cc_bright ? cc_bright : 3) - .5f) / 4;
    if (p == SE_SOUNDS && !SE.slider) SE.vol = sh_snd_volume() / 15.0f;   /* cp7w */
    se_rows(p);
    SE.content_h = se_layout() + (p == SE_ROOT ? 56 : 0);      /* the root's last card clears the search capsule */
    SE.scroll = tk_clampf(SE.scroll, 0, se_max());
    /* the page: chunks of SE_CH panel rows from the top, each drawn with what crosses it and cut to what it holds */
    for (int k = 0; k < SE_NCH; k++) { relayer(&sepg[k]); sepg[k].hidden = 1; }
    int X0 = (int)tk_floorf(PT(LG_X - 2)), Wd = (int)PT(LG_W + 4) + 2, Hp = (int)PT(SE.content_h) + 2;
    for (int k = 0; k < SE_NCH && k * SE_CH < Hp; k++) {
        tk_layer *l = &sepg[k];
        tk_layer_init(l, "settings-page");
        int rows = tk_mini(SE_CH, Hp - k * SE_CH);
        if (!tk_bmp_new(&l->bmp, Wd, rows)) { se_free(); return; }
        l->x = l->bx = (float)X0;
        l->y = l->by = (float)(k * SE_CH);
        dc d = { &l->bmp, 0, (float)X0, (float)(k * SE_CH) };
        if (p == SE_ROOT) d_h1(&d, "Settings");
        for (int i = 0; i < SE.ns; i++) se_draw_sec(&d, i);
        trim(l);
        l->hidden = 0;
    }
    /* the nav row: the back circle and the inline title (the root: the title only, once scrolled) */
    relayer(&senav);
    dc n = mk(&senav, "settings-nav", 0, SAFE_TOP, 390, NAVBAR_H, 0, 0);
    if (n.b->px) {
        if (p != SE_ROOT) d_back(&n, W(1));
        d_navtitle(&n, se_title(p));
        trim(&senav);
    }
    se_search_build();
    if (p != SE_SEARCH) { relayer(&sekb); sekb.hidden = 1; }
    else if (!sekb.bmp.px) se_kb_build();
    seclip.clip = 1;
    seclip.clip_r = (tk_rect){ 0, PT(SE_TOP - 6), (float)TK_W, PT(H_PT - SE_TOP + 6) };
    seclip.fade_top = PT(18);
    seclip.hidden = 0;
    SE.built = 1;
    se_spin_set(se_spin_want());
    se_place();
    relink();
}
static int se_layers(tk_layer **o)
{
    if (!seclip.name) return 0;
    tk_layer *s[SE_NCH + 4];
    for (int k = 0; k < SE_NCH; k++) s[k] = &sepg[k];
    s[SE_NCH] = &ksw; s[SE_NCH + 1] = &kslf; s[SE_NCH + 2] = &kslk; s[SE_NCH + 3] = &sespin;
    link(&sescr, s, SE_NCH + 4);
    tk_layer *c[] = { &sescr };
    link(&seclip, c, 1);
    o[0] = &seclip;
    o[1] = &seedge;                               /* 8a: under the nav row and the capsule */
    o[2] = &senav;
    o[3] = &sesb;
    o[4] = &sekb;
    o[5] = &sepspin;                              /* cp6: over the strip pages */
    return 6;
}
static void se_close(void) { relayer(&sepspin); sepspin.hidden = 1; SPS.on = 0; se_free(); SE.bld_page = -1; SE.press = -1; SE.ns_ = 0; for (int i = 0; i < SE_NPAGES; i++) SE.save[i] = 0; }
static void se_idle(void)
{
    /* the page's half copies (its first scroll frame would make them: 6 ms) */
    if (!SE.built || !is_app("settings") || AP.closing) return;
    for (int k = 0; k < SE_NCH; k++) if (sepg[k].bmp.px && !sepg[k].hidden) tk_layer_prepare(&sepg[k]);
}

/* ---- going places -------------------------------------------------------------------------------------------- */
static void se_push(int p)
{
    SE.save[SE.page] = SE.scroll;
    if (SE.ns_ < 8) SE.stack[SE.ns_++] = SE.page;
    AS.sub = p;
    build_app_page(AP.a);
}
static void se_open_app(const char *k)
{
    for (int a = 0; a < NAPPS; a++)
        if (!tk_strcmp(APPS[a].k, k)) {
            if (AP.on) { sp_app.live = 0; app_finish_close(); }
            app_open(a);
            return;
        }
}
static void se_ok(int i) { (void)i; }
/* Display > Auto-Lock: Track A's sh_autolock (seconds, 0 never), chosen in an action sheet (the kit's six rows) */
static const int SE_AL[5] = { 30, 60, 120, 300, 0 };
static void se_autolock_cb(int i)
{
    if (i < 0 || i > 4) return;
    sh_autolock_set(SE_AL[i]);
    if (is_app("settings") && se_owns()) build_app_page(AP.a);
}
__attribute__((unused)) static void se_soon(const char *what)
{
    static const kit_btn OK[1] = { { "OK", KIT_PREFERRED } };
    kit_alert(what, "Coming soon: this page arrives with Starbite OS 1.0.", OK, 1, se_ok);
}
static void se_reset_done(int i)
{
    if (i != 1) return;
    se_defaults();
    toast("Settings reset");
    if (se_owns()) build_app_page(AP.a);
}
static void se_emg_off(int i)
{
    if (i != 1) return;
    set_emergency(0, 0);
    if (se_owns()) build_app_page(AP.a);
}
/* the sheets (honest pages that don't need a whole page): Battery Health, Text Size */
static const char *se_sheet_text;
static void se_sheet_draw(dc *d, float x, float y, float w)
{
    char ln[4][96];
    float wm;
    const char *t = se_sheet_text;
    float yy = y + 10;
    /* up to two paragraphs ('\n' between) */
    for (int p = 0; p < 2 && t && *t; p++) {
        char para[300];
        int k = 0;
        while (t[k] && t[k] != '\n' && k < 299) { para[k] = t[k]; k++; }
        para[k] = 0;
        int n = wrapn(ln, 4, TK_IT400, 15, para, w, &wm);
        for (int i = 0; i < n; i++) d_text(d, TK_IT400, 15, x, bl(TK_IT400, 15, yy + 21.0f * i), ln[i], 0, 0, p ? C_LABEL2 : INK, 0);
        yy += 21.0f * n + 12;
        t += k;
        if (*t == '\n') t++;
    }
}
static void se_sheet(const char *title, const char *text)
{
    se_sheet_text = text;
    kit_sheet(title, 200, se_sheet_draw, 0, 0);
}
/* ---- cp7: pairing a Bluetooth keyboard (iOS: the row says Connecting..., then "Bluetooth Pairing Request" with the
 * code to type; My Devices once it is paired; Forget This Device) ---- */
static void se_build(void);
static void se_bt_rebuild(void) { if (SE.built && se_owns() && SE.page == SE_BT && !SE.down) se_build(); }
static void se_bt_code_cb(int b)
{
    (void)b;                                         /* Cancel (the only button): the pairing stops */
    SBP.alert = 0;
    if (SBP.dev >= 0) { sh_bt_unpair(); SBP.dev = -1; SBP.st = 0; se_bt_rebuild(); }
}
static void se_bt_close_alert(void)
{
    if (SBP.alert && kit_on()) { KT.cb = 0; kit_dismiss(-1); }   /* closed by the pairing's end, not a button */
    SBP.alert = 0;
}
static void se_bt_pair(const se_row *r)
{
    if (SBP.dev >= 0 && SBP.st != SH_BTP_FAILED) { toast("A device is pairing now"); return; }
    if (sh_bt_kbd_paired()) { toast("One keyboard at a time: forget the paired one first"); return; }
    int rc = sh_bt_pair(r->dev);
    if (rc < 0) {
        const char *w = sh_bt_pair_why();
        toast(w && w[0] ? w : "Pairing didn't start");
        return;
    }
    SBP.dev = r->dev;
    SBP.st = -1;
    md_cat(SBP.name, 0, sizeof SBP.name, r->l);
    sh_haptic_k(SH_HAP_LIGHT);
    se_bt_rebuild();
}
static void se_bt_kbd_cb(int b)
{
    int conn = sh_bt_kbd_connected();
    if (!conn && b == 0) {                           /* Connect */
        if (sh_bt_reconnect() < 0) toast("The keyboard didn't answer. Turn it on and try again");
        return;
    }
    if (b == (conn ? 0 : 1)) {                       /* Forget This Device */
        sh_bt_unpair();
        SBP.dev = -1;
        toast("Keyboard forgotten");
        se_bt_rebuild();
    }
}
static void se_bt_kbd(void)
{
    static const kit_btn B1[2] = { { "Forget This Device", KIT_DESTRUCTIVE }, { "Cancel", KIT_CANCEL } };
    static const kit_btn B2[3] = { { "Connect", KIT_DEFAULT }, { "Forget This Device", KIT_DESTRUCTIVE }, { "Cancel", KIT_CANCEL } };
    const char *t = se_bt_kname();
    if (sh_bt_kbd_connected()) kit_actions(t, B1, 2, se_bt_kbd_cb);
    else kit_actions(t, B2, 3, se_bt_kbd_cb);
}
/* the pairing's state, polled (se_refresh) while one runs, on any page: 1 something shown changed */
static int se_bt_pair_poll(void)
{
    if (SBP.dev < 0) return 0;
    int st = sh_bt_pair_state();
    if (st == SBP.st) return 0;
    SBP.st = st;
    if (st == SH_BTP_CODE) {
        static const kit_btn B[1] = { { "Cancel", KIT_CANCEL } };
        static char msg[160];
        int n = md_cat(msg, 0, sizeof msg, "Enter the code \xe2\x80\x9c");
        n = md_cat(msg, n, sizeof msg, sh_bt_pair_code());
        n = md_cat(msg, n, sizeof msg, "\xe2\x80\x9d on \xe2\x80\x9c");
        n = md_cat(msg, n, sizeof msg, SBP.name[0] ? SBP.name : "the keyboard");
        md_cat(msg, n, sizeof msg, "\xe2\x80\x9d, followed by the return or enter key.");
        if (kit_alert("Bluetooth Pairing Request", msg, B, 1, se_bt_code_cb) >= 0) SBP.alert = 1;
        return 1;
    }
    if (st == SH_BTP_CONNECTED) {
        se_bt_close_alert();
        SBP.dev = -1;
        sh_haptic_k(SH_HAP_SUCCESS);
        toast(se_str(SBP.name[0] ? SBP.name : se_bt_kname(), " Connected"));
        se_bt_rebuild();
        return 1;
    }
    if (st == SH_BTP_FAILED) {
        se_bt_close_alert();
        static const kit_btn B[1] = { { "OK", KIT_DEFAULT } };
        const char *w = sh_bt_pair_why();
        kit_alert("Pairing Unsuccessful", w && w[0] ? w : "The keyboard didn't finish pairing. Try again.", B, 1, 0);
        SBP.dev = -1;
        se_bt_rebuild();
        return 1;
    }
    if (st == SH_BTP_IDLE && SBP.alert) { se_bt_close_alert(); SBP.dev = -1; se_bt_rebuild(); return 1; }
    se_bt_rebuild();                                 /* connecting / securing: the row says so */
    return 1;
}
static void se_act(se_row *r)
{
    int id = r->id;
    if (id >= 1000) { SE.napp = id / 1000 - 1; id = SE_NOTIF_APP; }
    if (id < 100) {
        if (id == SE_WIFI || id == SE_CELL) { SE.save[SE.page] = SE.scroll; AS.sub = id; build_app_page(AP.a); return; }   /* their own pages */
        se_push(id);
        return;
    }
    switch (id) {
    case A_CARD: SE.save[SE.page] = SE.scroll; SE.popping = 1; sv_settings_card(); break;   /* (its back: Emergency, where it was) */
    case A_UPDATE:                               /* Track A update: Software Update's page, in the strips */
        SE.save[SE.page] = SE.scroll;
        su_open();
        break;
    case A_PASSCODE:
#ifdef SH_HAS_PASSCODE
        SE.save[SE.page] = SE.scroll;
        AS.sub = SE_PASSCODE_PAGE;               /* Track A's page, in the strips */
        build_app_page(AP.a);
#else
        se_soon("Passcode");
#endif
        break;
    case A_STARTUP:
#ifdef SH_HAS_STARTUP
        SE.save[SE.page] = SE.scroll;
        AS.sub = SE_STARTUP_PAGE;                /* Track J's page, in the strips */
        build_app_page(AP.a);
#else
        se_soon("Startup");
#endif
        break;
    case A_TZ:                                   /* Track A's zone list, in the strips; its back: Date & Time */
        SE.save[SE.page] = SE.scroll;
        SE.popping = 1;
        AS.sub = SE_TZ_PAGE;
        ta_tz_open();
        build_app_page(AP.a);
        break;
    case A_AUTOLOCK: {
        static const kit_btn B[6] = { { "30 Seconds", KIT_DEFAULT }, { "1 Minute", KIT_DEFAULT }, { "2 Minutes", KIT_DEFAULT },
                                      { "5 Minutes", KIT_DEFAULT }, { "Never", KIT_DEFAULT }, { "Cancel", KIT_CANCEL } };
        kit_actions("Auto-Lock", B, 6, se_autolock_cb);
        break;
    }
    case A_STORE: se_open_app("store"); break;
    case A_TERMINAL: se_open_app("terminal"); break;
    case A_LAST2: se_open_app("power"); break;
    case A_BTDEV: se_bt_pair(r); break;           /* cp7: a keyboard's pairing (the code in an alert) */
    case A_BTKBD: se_bt_kbd(); break;
    case A_TONE_RING: case A_TONE_ALARM:           /* cp7w: a repeating tone: three seconds of it */
        snd_ring_stop(3);
        snd_ring(3, id == A_TONE_RING ? SH_SND_RING : SH_SND_ALARM);
        if (snd_ring_who == 3) se_tone_until = now_ms + 3000;
        else { const char *w = sh_snd_why(); toast(w && w[0] ? w : "The sound didn't play"); }
        break;
    case A_TONE_TEXT:
        if (snd_ring_who) { toast("Another sound is playing"); break; }
        if (sh_snd_tone(SH_SND_NOTIFY) < 0) { const char *w = sh_snd_why(); toast(w && w[0] ? w : "The sound didn't play"); }
        break;
    case A_TEXTSIZE:
        se_sheet("Text Size", "Text has one size in this version: iOS's Body is 17 pt.\nLarger text needs every page "
                              "laid out again; it is planned.");
        break;
    case A_RESET: {
        static const kit_btn B[2] = { { "Cancel", KIT_CANCEL }, { "Reset", KIT_DESTRUCTIVE } };
        kit_alert("Reset Settings?", "Haptics, the keyboard and notifications go back to how they were at the start.", B, 2, se_reset_done);
        break;
    }
    default: break;
    }
}
static void se_toggle(int ri)
{
    se_row *r = &SE.r[ri];
    if (r->dis || r->kind != SR_TOG) return;
    int on = !r->on;
    switch (r->id) {
    case T_SYSHAP: se_put(SK_SYSHAP, on); break;
    case T_KBHAP: se_put(SK_KBHAP, on); break;
    case T_AUTOCAP: se_put(SK_AUTOCAP, on); break;
    case T_TIMERVIB: se_put(SK_TIMERVIB, on); break;
    case T_KBSND: se_put(SK_KBSND, on); break;          /* cp7w */
    case T_LOCKSND: se_put(SK_LOCKSND, on); break;      /* 8a */
    case T_SILENT: snd_silent_set(on); break;           /* 8a (the Control Centre's bell shows it too) */
    case T_LAST2AUTO: sh_last2_auto_set(on); on = sh_last2_auto(); break;
    case T_NALLOW: se_put(SK_NALLOW + SE.napp, on); break;
    case T_NBANNER: se_put(SK_NBANNER + SE.napp, on); break;
    case T_CLOCK24: sh_clock24_set(on); ta_restyle(); break;   /* Track A: every time on screen again */
    case T_BT:
        if (sh_bt_on(on) < 0) {
            const char *w = sh_bt_why();
            toast(w && w[0] ? w : on ? "Bluetooth can't start now" : "Bluetooth can't stop now");
            return;
        }
        if (!on) { SEB.scan = 0; SEB.scan_t = -1; }
        SEB.build_t = now_ms;                    /* the page again after the switch's slide (se_refresh) */
        break;
    case T_AUTOBRIGHT: sh_auto_brightness(on); on = sh_auto_brightness_on(); break;
    case A_EMG:
        if (!on) {
            static const kit_btn B[2] = { { "Cancel", KIT_CANCEL }, { "Exit", KIT_DESTRUCTIVE } };
            kit_alert("Exit Emergency Mode?", "The lock screen and home go back to normal. SOS stops if it is on.", B, 2, se_emg_off);
            return;
        }
        set_emergency(1, 0);
        break;
    default: return;
    }
    if (r->id == T_NALLOW || r->id == T_CLOCK24) { build_app_page(AP.a); return; }   /* Banners greys out with it; the Time row */
    r->on = on;
    for (int s = 0; s < SE.ns; s++) if (ri >= SE.s[s].first && ri < SE.s[s].first + SE.s[s].n) se_redraw_sec(s);
    if (se_get(SK_SYSHAP)) sh_haptic();
    kit_switch_anim(SE_SWX, r->y + (r->h - KSW_H) / 2, on, LG_GROUND);
    relink();
}

/* ---- touches ------------------------------------------------------------------------------------------------------ */
static int se_row_at(float x, float y)
{
    float cy = y + SE.scroll;
    if (y < SE_TOP || x < LG_X || x >= LG_RE) return -1;
    for (int i = 0; i < SE.nr; i++) if (cy >= SE.r[i].y && cy < SE.r[i].y + SE.r[i].h) return i;
    return -1;
}
static int se_row_live(const se_row *r)
{
    return !r->dis && (r->kind == SR_NAV || r->kind == SR_ACT || r->kind == SR_PROFILE || r->kind == SR_TOG ||
                       r->kind == SR_SEG || (r->kind == SR_VAL && (r->id == A_BTDEV || r->id == A_BTKBD || r->id == A_TONE_RING ||
                                                                   r->id == A_TONE_TEXT || r->id == A_TONE_ALARM)));
}
/* cp6: what lights under the finger (shell_wifi.inc's press colours): a row its rect in its card (#3A3A3C), a
 * switch row its switch's track (darker; the row itself doesn't light, iOS), a segmented control the segment under x */
static void se_light(int i, float x)
{
    SE.press = i;
    if (i < 0) { hl_off(); return; }
    se_row *r = &SE.r[i];
    tk_rect rr = { LG_X, r->y - SE.scroll, LG_W, r->h }, g = rr;
    float rad = LG_RAD;
    tk_col c = SEP_ROW;
    if (r->kind == SR_TOG) {
        rr = g = (tk_rect){ SE_SWX, r->y + (r->h - KSW_H) / 2 - SE.scroll, KSW_W, KSW_H };
        rad = KSW_H / 2;
        c = SEP_SW;
    } else if (r->kind == SR_SEG) {
        int n = r->segn ? r->segn : 2, k = kit_segment_hit(x, LG_TX, LG_RE - 16 - LG_TX, n);
        float sw = (LG_RE - 16 - LG_TX - 4) / n;
        if (k < 0) { hl_off(); return; }
        rr = g = (tk_rect){ LG_TX + 2 + sw * k, r->y + 12 - SE.scroll, sw, 28 };
        rad = 14;
        c = SEP_SEG;
    } else
        for (int s = 0; s < SE.ns; s++)
            if (i >= SE.s[s].first && i < SE.s[s].first + SE.s[s].n) g = (tk_rect){ LG_X, SE.s[s].y - SE.scroll, LG_W, SE.s[s].h };
    if (g.y < SE_TOP) { g.h -= SE_TOP - g.y; g.y = SE_TOP; }   /* not over the nav row */
    if (g.h <= 0) { hl_off(); return; }
    hl_set(&apphl, rr, g, rad, c);
}
static float se_slider_frac(float x)
{
    float x0 = LG_TX + 30, w = LG_RE - 16 - 30 - LG_TX - 30;
    return tk_clampf((x - x0 - KSL_KW / 2) / (w - KSL_KW), 0, 1);
}
static void se_slider_set(float frac)
{
    if (SE.slider == A_VOLUME) {                  /* cp7w: the volume (0..15, kept; the Control Centre's follows) */
        SE.vol = frac;
        int v = (int)(frac * 15 + .5f);
        if (v != sh_snd_volume()) { sh_snd_volume_set(v); CCS.lvl[1] = v / 15.0f; if (CCS.built) cc_levels(); }
    } else {
    SE.bright = frac;
    int lv = 1 + (int)(frac * 3.999f);
    if (lv != cc_bright) {
        cc_bright = lv;
        sh_brightness(lv);
        CCS.lvl[0] = frac;
        if (CCS.built) cc_levels();
    }
    }
    for (int i = 0; i < SE.nr; i++)
        if (SE.r[i].kind == SR_SLIDER && SE.r[i].id == SE.slider) {
            kit_slider_drag(LG_TX + 30, SE.r[i].y + 16, LG_RE - 16 - 30 - LG_TX - 30, frac, LG_GROUND);
            /* cp6: the grabbed knob grows a little about its centre (the kit's knob bitmap has 8 pt of shadow room) */
            kslk.ox = kslk.x + PT(8 + KSL_KW / 2);
            kslk.oy = kslk.y + PT(8 + KSL_KH / 2);
            kslk.sx = kslk.sy = 1.12f;
        }
}
static void se_touch(int ph, float x, float y, float t)
{
    if (ph == 0) {
        SE.down = 1; SE.moved = 0; SE.axis = 0; SE.slider = 0;
        SE.x0 = x; SE.y0 = y; SE.t0 = t;
        SE.base = SE.scroll;
        SE.momentum = 0;
        SE.sp.live = 0;
        tk_tracker_reset(&SE.tr);
        tk_tracker_add(&SE.tr, y, t);
        if (SE.page != SE_ROOT && BACK_HIT(x, y)) {
            hl_set(&apphl, (tk_rect){ BACK_X, SAFE_TOP, BACK_D, BACK_D }, (tk_rect){ BACK_X, SAFE_TOP, BACK_D, BACK_D }, BACK_D / 2, W(.16f));
            SE.press = -2;
            return;
        }
        /* Search: the root's capsule (-3); on its page the keyboard (-4) and the field's x (-5) */
        if (SE.page == SE_ROOT && x >= MARGIN && x < CONTENT_R && y >= SE_SB_Y && y < SE_SB_Y + 48) {
            tk_rect c = { MARGIN, SE_SB_Y, CONTENT_W, 48 };
            hl_set(&apphl, c, c, 24, W(.1f));
            SE.press = -3;
            return;
        }
        if (SE.page == SE_SEARCH && y >= 540) {
            tk_rect k;
            if (kb_rect(x, y, KR[SE.num], !SE.num, &k)) hl_set(&apphl, k, k, 6, W(.35f));
            SE.press = -4;
            return;
        }
        if (SE.page == SE_SEARCH && y >= SE_SF_Y && y < SE_SF_Y + 48) {
            SE.press = x >= CONTENT_R - 48 ? -5 : -6;
            if (SE.press == -5) {                        /* cp6: the field's x circle lights */
                tk_rect c = { CONTENT_R - 44, SE_SF_Y + 2, 44, 44 };
                hl_set(&apphl, c, c, 22, W(.16f));
            }
            return;
        }
        int i = se_row_at(x, y);
        if (i >= 0 && SE.r[i].kind == SR_SLIDER) { SE.slider = SE.r[i].id; se_slider_set(se_slider_frac(x)); return; }   /* cp7w: which one */
        se_light(i >= 0 && se_row_live(&SE.r[i]) ? i : -1, x);
        return;
    }
    if (ph == 1) {
        tk_tracker_add(&SE.tr, y, t);
        if (SE.slider) { se_slider_set(se_slider_frac(x)); return; }
        float dy = y - SE.y0;
        if (!SE.moved && (tk_fabsf(dy) > 6 || tk_fabsf(x - SE.x0) > 6)) { SE.moved = 1; se_light(-1, 0); }
        if (SE.moved && SE.press > -3) {
            float s = SE.base - dy, m = se_max();
            if (s < 0) s = -rubber(-s, H_PT);
            else if (s > m) s = m + rubber(s - m, H_PT);
            SE.scroll = s;
            se_place();
        }
        return;
    }
    SE.down = 0;
    int pressed = SE.press;
    hl_off();
    SE.press = -1;
    if (SE.slider) {
        int vol = SE.slider == A_VOLUME;
        SE.slider = 0;
        kit_slider_end();
        build_app_page(AP.a);                            /* the page with the knob where it was left */
        if (vol) snd_once(SH_SND_NOTIFY);                /* cp7w: the new volume heard (iOS plays the ringer's sound) */
        return;
    }
    if (pressed <= -3) {
        if (SE.moved && pressed != -4) return;
        if (pressed == -3) { se_push(SE_SEARCH); return; }
        if (pressed == -5) { SE.q[0] = 0; se_build(); return; }
        if (pressed == -4) {
            /* a key: as Notes' keyboard (shift once after a capital, 123, delete; return opens the first result) */
            char c = 0;
            int k = kb_hit(x, y, SE.shift, SE.num, &c), n = (int)tk_strlen(SE.q), kb = 0;
            if (k) sh_haptic();
            if (k == 2) { SE.shift = !SE.shift; kb = 1; }
            else if (k == 3) { SE.num = !SE.num; kb = 1; }
            else if (k == 4 && n) SE.q[n - 1] = 0;
            else if (k == 1 && n < (int)sizeof SE.q - 1) {
                SE.q[n] = c; SE.q[n + 1] = 0;
                if (SE.shift) { SE.shift = 0; kb = 1; }
            } else if (k == 5) {
                if (SE.nr) se_act(&SE.r[0]);
                return;
            }
            if (!k) return;
            if (kb) se_kb_build();
            se_build();
        }
        return;
    }
    if (SE.moved) {
        float m = se_max();
        if (SE.scroll < 0 || SE.scroll > m) tk_spring_start(&SE.sp, SE.scroll, SE.scroll < 0 ? 0 : m, 0, TK_SPR_ROWS);
        else { SE.v = -tk_tracker_v(&SE.tr); SE.momentum = tk_fabsf(SE.v) > .02f; }
        return;
    }
    if (pressed == -2 && BACK_HIT(x, y)) {
        AS.sub = SE.ns_ ? SE.stack[--SE.ns_] : se_parent(SE.page);
        SE.popping = 1;
        build_app_page(AP.a);
        return;
    }
    int i = se_row_at(x, y);
    if (i < 0) return;
    se_row *r = &SE.r[i];
    if (r->dis) { if (r->why) toast(r->why); return; }   /* cp6: a disabled row says why (never a silent tap) */
    if (r->kind == SR_TOG) se_toggle(i);
    else if (r->kind == SR_SEG && r->id == A_RANGE) {
        int k = kit_segment_hit(x, LG_TX, LG_RE - 16 - LG_TX, 2);
        if (k >= 0 && k != BH.range) { BH.range = k; sh_haptic_k(SH_HAP_SELECT); build_app_page(AP.a); }
    } else if (r->kind == SR_SEG && r->id == A_HAPLVL) {
        /* Haptic Strength: the level kept (haptic.level), its group redrawn, a sample at the new level */
        int k = kit_segment_hit(x, LG_TX, LG_RE - 16 - LG_TX, 4);
        if (k < 0) return;
        if (k != r->on) {
            sh_haptic_level_set(k);
            r->on = sh_haptic_level();
            for (int s = 0; s < SE.ns; s++) if (i >= SE.s[s].first && i < SE.s[s].first + SE.s[s].n) se_redraw_sec(s);
            relink();
        }
        if (k > 0) sh_haptic_kind(SH_VIB_NOTIFY);
    } else if (se_row_live(r) && pressed == i) se_act(r);
}
static int se_tick(float dt)
{
    int mv = 0;
    if (!SE.built) return 0;
    if (SE.sp.live) {
        tk_spring_step(&SE.sp, dt);
        SE.scroll = SE.sp.x;
        se_place();
        mv = 1;
    } else if (SE.momentum) {
        float ms = dt * 1000, m = se_max(), decay = tk_powf(.998f, ms);
        SE.scroll += SE.v * (1 - decay) / (1 - .998f);
        SE.v *= decay;
        if (SE.scroll < 0 || SE.scroll > m) {
            SE.momentum = 0;
            tk_spring_start(&SE.sp, SE.scroll, SE.scroll < 0 ? 0 : m, SE.v * 1000, TK_SPR_ROWS);
        } else if (tk_fabsf(SE.v) < .01f) SE.momentum = 0;
        se_place();
        mv = 1;
    }
    return mv | (SE.down && (SE.moved || SE.slider));
}
static int se_moving(void) { return SE.built && (SE.sp.live || SE.momentum || (SE.down && (SE.moved || SE.slider))); }
/* the platform's refresh turns (sh_refresh / sh_next_update): Bluetooth's state, scan and list, the spinner's step;
 * Display's lux reading and the level Auto-Brightness set. Nothing here blocks; a page is built again only when what
 * it shows changed (at most every 0.3 s), never under a finger. */
static int se_refresh(float t)
{
    if (se_tone_until > 0 && t >= se_tone_until) { se_tone_until = 0; snd_ring_stop(3); }   /* cp7w: the preview's end */
    int ch = se_bt_pair_poll();                          /* cp7: a keyboard pairing (any page) */
    if (sepspin.name) {                                  /* cp6: a strip page's spinner: there or not, its step */
        float cx = 0, cy = 0;
        int want = se_strip_spin_where(&cx, &cy);
        if (want != SPS.on || (want && (cx != SPS.cx || cy != SPS.cy))) { if (se_strip_spin_set()) relink(); ch = 1; }
        else if (SPS.on && t - SPS.t >= 100) {
            SPS.k = (SPS.k + 1) & 7;
            SPS.t = t;
            se_spin_paint(&sepspin, SPS.cx, SPS.cy, SPS.k);
            ch = 1;
        }
    }
    if (!SE.built || !se_owns() || AP.closing) return ch;
    if (SE.page == SE_BT) {
        int st = sh_bt_state(), aw = se_awake();
        if (st == 2 && aw) {
            if (SEB.scan_t < 0 || t - SEB.scan_t > (SEB.scan ? 50000 : 5000)) {   /* a scan lasts 60 s: asked again */
                SEB.scan = sh_bt_scan(1) == 0;
                SEB.scan_t = t;
            }
        } else {
            if (SEB.scan) sh_bt_scan(0);
            SEB.scan = 0;
            SEB.scan_t = -1;
        }
        int cnt = st == 2 ? sh_bt_count() : 0;
        if ((st != SEB.st || cnt != SEB.cnt) && t - SEB.build_t >= 300 && !SE.down) { se_build(); return 1; }
        if (se_spin_want() != SEB.spin) { se_spin_set(se_spin_want()); relink(); ch = 1; }
        else if (SEB.spin && t - SEB.spin_t >= 100) {
            SEB.spin_k = (SEB.spin_k + 1) & 7;
            SEB.spin_t = t;
            se_spin_draw();
            ch = 1;
        }
    } else if (SE.page == SE_DISPLAY && se_awake() && t - SEB.poll_t >= 2000 && !SE.down) {
        SEB.poll_t = t;
        if (cc_bright && cc_bright != SEB.bright) { se_build(); return 1; }   /* the slider follows Auto-Brightness */
        int lux = sh_light_lux();
        if (se_lux_round(lux) != se_lux_shown) { se_lux_redraw(lux); ch = 1; }
    }
    return ch;
}
static float se_next_update(float t)
{
    if (se_tone_until > 0) return tk_maxf(1, se_tone_until - t);   /* cp7w */
    if (SPS.on) return tk_maxf(15, 100 - (t - SPS.t));   /* cp6: the strip spinner's step */
    if (SBP.dev >= 0) return 250;                        /* cp7: a keyboard pairing's state */
    if (!SE.built || !se_owns() || AP.closing) return -1;
    if (SE.page == SE_BT) {
        if (!se_awake()) return SEB.scan ? 15 : -1;     /* asleep: the scan stops, nothing more */
        if (SEB.spin) return tk_maxf(15, 100 - (t - SEB.spin_t));
        int st = sh_bt_state();
        return st == 1 || st == 2 || st != SEB.st ? 300 : -1;
    }
    if (SE.page == SE_DISPLAY && se_awake()) return tk_maxf(15, 2000 - (t - SEB.poll_t));
    return -1;
}
/* cp6: the strip pages' presses (shell_lock.c's app_press asks, over the press protocol: rect, clip, radius, colour) */
#ifdef SH_HAS_STARTUP
static int startup_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c);
#endif
static int ta_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c);
static int se_strip_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    if (!is_app("settings") || kit_on()) return 0;
    if (AS.sub == SE_WIFI) return wf_press(x, y, r, g, rad, c);
    if (AS.sub == SE_CELL) return cell_press(x, y, r, g, rad, c);
    if (AS.sub == SE_TZ_PAGE) return ta_press(x, y, r, g, rad, c);
#ifdef SH_HAS_STARTUP
    if (AS.sub == SE_STARTUP_PAGE) return startup_press(x, y, r, g, rad, c);
#endif
    return 0;
}
/* the host's stills: "sub" names (display, wifi, cell as before; the new pages by name) and "sescroll" pt */
static int se_sub_key(const char *k)
{
    static const struct { const char *k; int p; } M[] = { { "display", SE_DISPLAY }, { "wifi", SE_WIFI }, { "cell", SE_CELL },
        { "general", SE_GENERAL }, { "about", SE_ABOUT }, { "update", SU_SUB }, { "datetime", SE_DATETIME },
        { "keyboard", SE_KEYBOARD }, { "lang", SE_LANG }, { "reset", SE_RESET }, { "wallpaper", SE_WALL }, { "sounds", SE_SOUNDS },
        { "notif", SE_NOTIF }, { "notifapp", SE_NOTIF_APP }, { "battery", SE_BATTERY }, { "privacy", SE_PRIVACY },
        { "emergency", SE_EMERGENCY }, { "linux", SE_LINUX }, { "search", SE_SEARCH }, { "bluetooth", SE_BT },
        { "health", SE_HEALTH } };
    for (unsigned i = 0; i < sizeof M / sizeof M[0]; i++) if (!tk_strcmp(M[i].k, k)) return M[i].p;
    return 0;
}
static int se_set(const char *what, const char *key, float v)
{
    if (!tk_strcmp(what, "sescroll")) { SE.scroll = tk_clampf(v, 0, se_max()); se_place(); return 1; }
    if (!tk_strcmp(what, "bt")) {                 /* the stills: Bluetooth on (v 1: it starts) or off (v 0) */
        sh_bt_on(v > 0);
        if (se_owns()) build_app_page(AP.a);
        return 1;
    }
    if (!tk_strcmp(what, "sehist")) {
        /* a pretend day for the stills: discharging from 100 %, a charge in the afternoon */
        BH.n = 0;
        for (int i = 0; i < SE_HIST; i++) {
            int m = (ST.h * 60 + ST.mi - (SE_HIST - 1 - i) * 10 + 24 * 60) % (24 * 60), p = i < 90 ? 100 - i * 2 / 3 : i < 100 ? 40 + (i - 90) * 5 : 90 - (i - 100) / 2;
            BH.pct[i] = (uint8_t)p; BH.chg[i] = i >= 90 && i < 100; BH.min[i] = (int16_t)m;
        }
        BH.n = SE_HIST;
        BH.last_m = ST.h * 60 + ST.mi;
        BH.range = v > 0;
        if (se_owns()) build_app_page(AP.a);
        return 1;
    }
    (void)key;
    return 0;
}
