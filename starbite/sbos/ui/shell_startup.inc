/* SPDX-License-Identifier: MIT
 * Track J: Settings > Startup (docs/STARBITE-FIRMWARE-LAYER.md 10: Starbite OS as the boot menu), on Track B's hook:
 * included after shell_settings.inc, which calls page_startup (each of the page's four strips) and startup_tap, and
 * startup_tick if it wants (nothing here moves). One pushed page with sub-views, each the Settings rows' look (an
 * inset group, 47 pt rows, chevrons, iOS's switch), the glass back at the top left:
 *   Startup          Operating Systems >, Firmware >, Recovery >; Starbite OS as the boot menu (the switch)
 *   OS list          every catalog OS (hidden ones not), the default marked, one that cannot start greyed
 *   OS detail        Start Once, Make Default, "Start once with" the one-shots that fit its kind (Windows: Advanced
 *                    Startup greyed, plan only)
 *   Firmware         the loader's settings: default OS, menu timeout, debug level, POST, watchdog, crash detection,
 *                    after a crash; each opens a picker (a check on the current one)
 *   Recovery         the last crash reports, this start (why, store, loader), restart to the boot menu / Starbite
 *                    Recovery / Download mode
 * The platform's calls are shell.h's sh_os_*, sh_fw_*, sh_req_*, sh_crash_*, sh_restart_*, sh_fw_info (the kernel:
 * kern/sbfw_os.c; the host: host/startup_fake.inc). Every restart and one-shot asks with the platform's hold
 * confirmation. What the page shows is read once when a view opens (su_load), never per strip. */
#define SHELL_STARTUP 1

enum { SU_ROOT, SU_OSL, SU_OSD, SU_FW, SU_PICK, SU_REC };
enum { PK_DEFAULT, PK_TIMEOUT, PK_DEBUG, PK_POST, PK_WDT, PK_CRASH, PK_AFTER };
#define SU_ROWS 12                          /* OS rows that fit the screen */
#define SU_CRASHES 3
#define SU_ROW 47.0f
typedef struct { char key[24], name[32]; int cat, method, ready, def; } su_os_t;
static struct {
    int view, os, pick, loaded;
    int nos, more, have_fw, ncrash;
    su_os_t o[SU_ROWS];
    sh_fw_t fw;
    sh_crash_t cr[SU_CRASHES];
    sh_fwinfo_t inf;
} SU;

/* ---- the data, read when a view opens ------------------------------------------------------------------------ */
static void su_load(void)
{
    SU.loaded = 1;
    int n = sh_os_count(0);
    SU.nos = n < SU_ROWS ? n : SU_ROWS;
    SU.more = n - SU.nos;
    for (int i = 0; i < SU.nos; i++) {
        su_os_t *o = &SU.o[i];
        if (sh_os_get(i, o->key, o->name, &o->cat, &o->method, &o->ready, &o->def) < 0) { SU.nos = i; break; }
    }
    SU.have_fw = sh_fw_get(&SU.fw);
    if (SU.have_fw < 0) tk_memset(&SU.fw, 0, sizeof SU.fw);
    int c = sh_crash_count();
    SU.ncrash = c < 0 ? -1 : c;
    for (int i = 0; i < SU_CRASHES && i < c; i++)
        if (sh_crash_get(i, &SU.cr[i])) { SU.ncrash = i; break; }
    sh_fw_info(&SU.inf);
}

/* ---- words ----------------------------------------------------------------------------------------------------- */
static const char *const SU_DEBUG[4] = { "Off", "Normal", "High", "Highest" };
static const char *const SU_POST[3] = { "Full", "Quick", "Off" };
static const char *const SU_CRASH[3] = { "On", "Save Only", "Off" };
static const char *const SU_AFTER[3] = { "Boot Menu", "Starbite Recovery", "Retry Once" };
static const int SU_TMO[5] = { 0, 3, 5, 10, 30 };
static const char *const SU_TMO_T[5] = { "Wait", "3 s", "5 s", "10 s", "30 s" };
static const char *const SU_WDT_T[4] = { "Per Entry", "Off", "30 s", "60 s" };
static const struct { int act; const char *name; } SU_REQ[5] = {
    { SH_REQ_VERBOSE, "Verbose" }, { SH_REQ_SAFE, "Safe Mode" }, { SH_REQ_SINGLE, "Single User" },
    { SH_REQ_PREV, "Previous Kernel" }, { SH_REQ_XNU_SINGLE, "Single User (XNU)" } };

/* the watchdog's picker row (-1: a time the loader's own menu set, not 30 or 60 s) */
static int su_wdt_i(void)
{
    return SU.fw.wdt_mode == 1 ? 1 : SU.fw.wdt_mode == 2 ? (SU.fw.wdt_s == 30 ? 2 : SU.fw.wdt_s == 60 ? 3 : -1) : 0;
}
static const char *su_wdt_text(char *b)
{
    int i = su_wdt_i();
    if (i >= 0) return SU_WDT_T[i];
    int n = put_dec(b, SU.fw.wdt_s);
    b[n++] = ' '; b[n++] = 's'; b[n] = 0;
    return b;
}
static const char *su_os_name(const char *key)
{
    if (!key[0]) return "None";
    for (int i = 0; i < SU.nos; i++) if (!tk_strcmp(SU.o[i].key, key)) return SU.o[i].name;
    return key;
}
static const char *su_tmo_text(char *b)
{
    for (int i = 0; i < 5; i++) if (SU_TMO[i] == SU.fw.timeout_s) return SU_TMO_T[i];
    int n = put_dec(b, SU.fw.timeout_s);
    b[n++] = ' '; b[n++] = 's'; b[n] = 0;
    return b;
}
/* "Nov 12, 14:02 UTC" (days to civil, H. Hinnant; the loader's clock is UTC), "" unknown */
static void su_date(long long t, char *o)
{
    if (t <= 0) { o[0] = 0; return; }
    long long days = t / 86400, z = days + 719468, era = z / 146097;
    unsigned sod = (unsigned)(t % 86400), doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int y = (int)(yoe + era * 400 + (m <= 2));
    static const char *const MO[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int n = 0;
    for (const char *s = MO[m - 1]; *s; s++) o[n++] = *s;
    o[n++] = ' ';
    n += put_dec(o + n, (int)d);
    (void)y;
    o[n++] = ',';
    o[n++] = ' ';
    o[n++] = (char)('0' + sod / 36000); o[n++] = (char)('0' + sod / 3600 % 10); o[n++] = ':';
    o[n++] = (char)('0' + sod / 60 % 60 / 10); o[n++] = (char)('0' + sod / 60 % 10);
    for (const char *u = " UTC"; *u; u++) o[n++] = *u;
    o[n] = 0;
}
static void su_cat(char *o, const char *a, const char *b)
{
    int n = 0;
    while (*a && n < 95) o[n++] = *a++;
    while (b && *b && n < 95) o[n++] = *b++;
    o[n] = 0;
}
/* the loader's kind names (sbfmt.c sb_kind_name) in words */
static const char *su_kind(const char *k)
{
    static const char *const K[][2] = {
        { "KERNEL PANIC", "Kernel panic" }, { "HANG (WATCHDOG)", "Hang (watchdog)" }, { "HANG (OS TIMER)", "Hang (OS timer)" },
        { "FORCED RESTART", "Forced restart" }, { "FAULT (S-BOOT)", "Fault (S-Boot)" }, { "WATCHDOG, NO OS", "Watchdog, no OS" },
        { "KERNEL OOPS", "Kernel oops" }, { "UNEXPLAINED RESET", "Unexplained reset" }, { "CLEAN RESTART", "Clean restart" },
        { "POWER ON", "Power on" }, { "NO OS WAS RUNNING", "No OS was running" } };
    for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) if (!tk_strcmp(K[i][0], k)) return K[i][1];
    return k;
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */
static float su_cap(dc *d, float y, const char *t)
{
    d_text(d, TK_IT400, 13, ROW_TX, y + 22, t, .52f, 0, W(.55f), 0);
    return y + 30;
}
static void su_group(dc *d, float y, int n) { d_rrect(d, GROUP_X, y, GROUP_W, SU_ROW * n - 1, 16, tk_hex(0x151c33, 1)); }
/* a row: label (colour lc), value on the right, a chevron (nav), a rule under it unless last */
static void su_row(dc *d, float ry, const char *l, tk_col lc, const char *v, int nav, int last)
{
    float right = nav ? CONTENT_R - 36 : CONTENT_R - 16, room = right - ROW_TX;
    float lw = tw(TK_IT400, 16, l, 0, 0), vw = v && v[0] ? tw(TK_IT400, 16, v, 0, TK_TNUM) : 0;
    float lmax = vw > 0 ? tk_maxf(room - vw - 16, room / 2) : room;
    d_text_box(d, TK_IT400, 16, ROW_TX, ry + 29, lmax, 0, l, 0, 0, lc, 0);
    if (vw > 0) d_text_box(d, TK_IT400, 16, right, ry + 29, room - tk_minf(lw, lmax) - 16, 2, v, 0, TK_TNUM, W(.55f), 0);
    if (nav) d_icon(d, "chev", CONTENT_R - 30, ry + 16.5f, .45f);
    if (!last) d_rrect(d, ROW_TX, ry + 46, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
}
static void su_switch(dc *d, float ry, int on)
{
    d_rrect(d, CONTENT_R - 67, ry + 8, 51, 31, 15.5f, on ? tk_hex(0x34c759, 1) : W(.16f));
    d_rrect(d, on ? CONTENT_R - 45 : CONTENT_R - 65, ry + 10, 27, 27, 13.5f, W(1));
}
static void su_check(dc *d, float ry)
{
    float x = CONTENT_R - 34, y = ry + 24;
    d_seg(d, x, y, x + 5, y + 5, 2.2f, ACCENT());
    d_seg(d, x + 5, y + 5, x + 14, y - 6, 2.2f, ACCENT());
}
/* grey lines under a group, wrapped; returns the y after them */
static float su_note(dc *d, float y, const char *t)
{
    char ln[4][96];
    float wm;
    int n = wrapn(ln, 4, TK_IT400, 13, t, CONTENT_W - 32, &wm);
    for (int k = 0; k < n; k++) d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y + 7 + 18.0f * k), ln[k], 0, 0, W(.45f), 0);
    return y + 7 + 18.0f * n;
}
static void su_head(dc *d, const char *title)
{
    d_back(d, W(1));
    d_navtitle(d, title);
}
#define SU_TOP (PUSHED_TOP + 12)

static void su_page_root(dc *d)
{
    su_head(d, "Startup");
    float y = SU_TOP + 34;
    char b[48];
    su_group(d, y, 3);
    int n = put_dec(b, SU.nos + SU.more);
    b[n] = 0;
    su_row(d, y, "Operating Systems", INK, b, 1, 0);
    su_row(d, y + SU_ROW, "Firmware", INK, "", 1, 0);
    if (SU.ncrash > 0) { n = put_dec(b, SU.ncrash); su_cat(b + n, SU.ncrash == 1 ? " crash" : " crashes", 0); }
    else b[0] = 0;
    su_row(d, y + 2 * SU_ROW, "Recovery", INK, b, 1, 1);
    y += 3 * SU_ROW + 6;
    y = su_cap(d, y, "AT POWER-ON");
    su_group(d, y, 1);
    su_row(d, y, "Starbite OS as Boot Menu", INK, "", 0, 1);
    su_switch(d, y, SU.fw.shell);
    y += SU_ROW;
    y = su_note(d, y, SU.fw.shell ? "Starbite OS starts first and is the boot menu. Hold Volume Down at power-on for the "
                                     "loader's own menu; after a crash it comes back by itself."
                                   : "The loader's menu starts first, then the default OS after its timeout.");
    if (SU.have_fw < 0) su_note(d, y + 6, "No settings store: the loader keeps its defaults.");
}

static void su_page_osl(dc *d)
{
    su_head(d, "Operating Systems");
    float y = SU_TOP + 34;
    if (!SU.nos) { d_empty(d, CONTENT_TOP, EMPTY_BOT, "No Systems", "The store's catalog lists no OS."); return; }
    su_group(d, y, SU.nos);
    for (int i = 0; i < SU.nos; i++) {
        const su_os_t *o = &SU.o[i];
        su_row(d, y + SU_ROW * i, o->name, o->ready ? INK : W(.4f), o->def ? "Default" : o->ready ? "" : "Not Installed", 1,
               i + 1 == SU.nos);
    }
    y += SU_ROW * SU.nos;
    char b[96];
    if (SU.more > 0) {
        int n = put_dec(b, SU.more);
        su_cat(b + n, " more in the catalog: the loader's menu lists them all.", 0);
        y = su_note(d, y, b);
    }
    su_note(d, y, "The default starts by itself after the menu's timeout. Tap a system to start it once.");
}

static int su_req_n(const su_os_t *o, int *acts)
{
    int n = 0;
    for (int i = 0; i < 5; i++) if (sh_req_fits(o->key, SU_REQ[i].act)) acts[n++] = i;
    return n;
}
static int su_is_windows(const su_os_t *o) { return o->method == 4 && o->cat == 1; }

static void su_page_osd(dc *d)
{
    if (SU.os < 0 || SU.os >= SU.nos) { su_head(d, "System"); return; }
    const su_os_t *o = &SU.o[SU.os];
    su_head(d, o->name);
    float y = SU_TOP + 34;
    su_group(d, y, 2);
    su_row(d, y, "Start Once", o->ready ? ACCENT() : W(.3f), "", 0, 0);
    su_row(d, y + SU_ROW, o->def ? "Default System" : "Make Default", o->def || !o->ready ? W(.4f) : ACCENT(), "", 0, 1);
    if (o->def) su_check(d, y + SU_ROW);
    y += 2 * SU_ROW;
    if (!o->ready) y = su_note(d, y, "Its files are not in the store, so it cannot start.");
    int acts[5], n = su_req_n(o, acts), win = su_is_windows(o);
    if (n || win) {
        y = su_cap(d, y + 6, "START ONCE WITH");
        su_group(d, y, n + win);
        for (int i = 0; i < n; i++) su_row(d, y + SU_ROW * i, SU_REQ[acts[i]].name, o->ready ? ACCENT() : W(.3f), "", 0, i + 1 == n + win);
        if (win) su_row(d, y + SU_ROW * n, "Advanced Startup", W(.3f), "Plan Only", 0, 1);
        y += SU_ROW * (n + win);
        su_note(d, y, "For the next start only: the loader applies it, then clears it.");
    }
}

static const char *su_pick_title(int p)
{
    static const char *const T[7] = { "Default System", "Menu Timeout", "Debug Level", "POST", "Watchdog", "Crash Detection", "After a Crash" };
    return T[p];
}
static void su_page_fw(dc *d)
{
    su_head(d, "Firmware");
    char b[16];
    float y = SU_TOP + 34;
    su_group(d, y, 3);
    su_row(d, y, "Starbite OS as Boot Menu", INK, "", 0, 0);
    su_switch(d, y, SU.fw.shell);
    su_row(d, y + SU_ROW, "Default System", INK, su_os_name(SU.fw.default_key), 1, 0);
    su_row(d, y + 2 * SU_ROW, "Menu Timeout", INK, su_tmo_text(b), 1, 1);
    y += 3 * SU_ROW;
    y = su_cap(d, y + 6, "DIAGNOSTICS");
    su_group(d, y, 3);
    su_row(d, y, "Debug Level", INK, SU_DEBUG[SU.fw.debug_level & 3], 1, 0);
    su_row(d, y + SU_ROW, "POST", INK, SU_POST[SU.fw.post < 3 ? SU.fw.post : 0], 1, 0);
    su_row(d, y + 2 * SU_ROW, "Watchdog", INK, su_wdt_text(b), 1, 1);
    y += 3 * SU_ROW;
    y = su_cap(d, y + 6, "CRASHES");
    su_group(d, y, 2);
    su_row(d, y, "Crash Detection", INK, SU_CRASH[SU.fw.crash_mode < 3 ? SU.fw.crash_mode : 0], 1, 0);
    su_row(d, y + SU_ROW, "After a Crash", INK, SU_AFTER[SU.fw.crash_action < 3 ? SU.fw.crash_action : 0], 1, 1);
    y += 2 * SU_ROW;
    su_note(d, y, SU.have_fw < 0 ? "No settings store: nothing can be changed." : "The loader reads these at its next start.");
}

/* the picker's options: count, label i, current */
static int su_pick_opts(int p, const char **lab, int *cur)
{
    int n = 0;
    *cur = -1;
    switch (p) {
    case PK_DEFAULT:
        for (int i = 0; i < SU.nos && n < SU_ROWS; i++) {
            if (!tk_strcmp(SU.o[i].key, SU.fw.default_key)) *cur = n;
            lab[n++] = SU.o[i].name;
        }
        break;
    case PK_TIMEOUT: for (; n < 5; n++) { lab[n] = SU_TMO_T[n]; if (SU_TMO[n] == SU.fw.timeout_s) *cur = n; } break;
    case PK_DEBUG: for (; n < 4; n++) lab[n] = SU_DEBUG[n]; *cur = SU.fw.debug_level; break;
    case PK_POST: for (; n < 3; n++) lab[n] = SU_POST[n]; *cur = SU.fw.post; break;
    case PK_WDT: for (; n < 4; n++) lab[n] = SU_WDT_T[n]; *cur = su_wdt_i(); break;
    case PK_CRASH: for (; n < 3; n++) lab[n] = SU_CRASH[n]; *cur = SU.fw.crash_mode; break;
    case PK_AFTER: for (; n < 3; n++) lab[n] = SU_AFTER[n]; *cur = SU.fw.crash_action; break;
    }
    return n;
}
static const char *su_pick_note(int p)
{
    switch (p) {
    case PK_DEFAULT: return "Starts by itself when the menu's timeout runs out.";
    case PK_TIMEOUT: return "How long the loader's menu waits before it starts the default.";
    case PK_DEBUG: return "The loader's log and each OS's console. Highest: every message, slower starts.";
    case PK_POST: return "The loader's power-on self test: Full takes about 10 s, Quick 0.3 s.";
    case PK_WDT: return "Restarts a system that hangs. Per Entry: each catalog entry's own time.";
    case PK_CRASH: return "On: the crash screen at the next start. Save Only: the report, no screen.";
    default: return "What the loader does after it found a crash.";
    }
}
static void su_page_pick(dc *d)
{
    su_head(d, su_pick_title(SU.pick));
    const char *lab[SU_ROWS];
    int cur, n = su_pick_opts(SU.pick, lab, &cur);
    float y = SU_TOP + 34;
    su_group(d, y, n);
    for (int i = 0; i < n; i++) {
        int off = SU.pick == PK_DEFAULT && !SU.o[i].ready;
        su_row(d, y + SU_ROW * i, lab[i], off ? W(.4f) : INK, off ? "Not Installed" : "", 0, i + 1 == n);
        if (i == cur) su_check(d, y + SU_ROW * i);
    }
    su_note(d, y + SU_ROW * n, su_pick_note(SU.pick));
}

#define SU_CR_ROW 62.0f
static void su_page_rec(dc *d)
{
    su_head(d, "Recovery");
    float y = SU_TOP;
    y = su_cap(d, y, "CRASH REPORTS");
    int nc = SU.ncrash > 0 ? (SU.ncrash < SU_CRASHES ? SU.ncrash : SU_CRASHES) : 0;
    if (!nc) {
        su_group(d, y, 1);
        su_row(d, y, SU.ncrash < 0 ? "No crash log on this store" : "No crashes", W(.5f), "", 0, 1);
        y += SU_ROW;
    } else {
        d_rrect(d, GROUP_X, y, GROUP_W, SU_CR_ROW * nc - 1, 16, tk_hex(0x151c33, 1));
        for (int i = 0; i < nc; i++) {
            const sh_crash_t *c = &SU.cr[i];
            float ry = y + SU_CR_ROW * i;
            char when[40];
            const char *kind = su_kind(c->kind);
            su_date(c->utc, when);
            float ww = when[0] ? tw(TK_IT400, 14, when, 0, TK_TNUM) : 0;
            d_text_box(d, TK_IT600, 16, ROW_TX, ry + 25, CONTENT_R - 16 - ROW_TX - ww - 10, 0, c->os, 0, 0, INK, 0);
            if (ww > 0) d_text(d, TK_IT400, 14, CONTENT_R - 16 - ww, ry + 25, when, 0, TK_TNUM, W(.5f), 0);
            /* the kind (red for a crash), then the message line in grey */
            float room = CONTENT_R - 16 - ROW_TX, kw = tk_minf(tw(TK_IT600, 13.5f, kind, 0, 0), room);
            d_text_box(d, TK_IT600, 13.5f, ROW_TX, ry + 46, room, 0, kind, 0, 0, c->severity >= 2 ? tk_hex(0xff6961, 1) : W(.7f), 0);
            if (c->line[0] && room - kw > 40) {
                char l2[112];
                su_cat(l2, "\xc2\xb7 ", c->line);
                d_text_box(d, TK_IT400, 13.5f, ROW_TX + kw + 5, ry + 46, room - kw - 5, 0, l2, 0, 0, W(.55f), 0);
            }
            if (i + 1 < nc) d_rrect(d, ROW_TX, ry + SU_CR_ROW - 1, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
        }
        y += SU_CR_ROW * nc;
    }
    y = su_cap(d, y + 6, "THIS START");
    su_group(d, y, 3);
    char b[48];
    su_row(d, y, "Why", INK, SU.inf.why, 0, 0);
    su_cat(b, SU.inf.medium == 1 ? "Memory Card" : SU.inf.medium == 0 ? "Phone (UFS)" : "None",
           SU.inf.card_only ? " \xc2\xb7 card only" : 0);
    su_row(d, y + SU_ROW, "Store", INK, b, 0, 0);
    if (SU.inf.valid) { su_cat(b, "Version ", 0); int n = 8; n += put_dec(b + n, SU.inf.loader_version); b[n] = 0; }
    else su_cat(b, "Older (no table)", 0);
    su_row(d, y + 2 * SU_ROW, "Loader", INK, b, 0, 1);
    y += 3 * SU_ROW;
    y = su_cap(d, y + 6, "RESTART");
    su_group(d, y, 3);
    su_row(d, y, "Restart to Boot Menu", ACCENT(), "", 0, 0);
    su_row(d, y + SU_ROW, "Restart to Starbite Recovery", ACCENT(), "", 0, 0);
    su_row(d, y + 2 * SU_ROW, "Restart to Download Mode", tk_hex(0xff6961, 1), "", 0, 1);
    y += 3 * SU_ROW;
    su_note(d, y, "Each asks first. The boot menu and Recovery are this once; the next start is as before.");
}

static void page_startup(dc *d)
{
    if (!SU.loaded) su_load();               /* the first strip of a fresh open reads; the other three draw */
    switch (SU.view) {
    case SU_OSL: su_page_osl(d); break;
    case SU_OSD: su_page_osd(d); break;
    case SU_FW: su_page_fw(d); break;
    case SU_PICK: su_page_pick(d); break;
    case SU_REC: su_page_rec(d); break;
    default: su_page_root(d); break;
    }
}

/* ---- taps -------------------------------------------------------------------------------------------------------- */
static void su_go(int view)
{
    SU.view = view;
    su_load();
    build_app_page(AP.a);
}
/* the row under y in a group of n rows starting at y0, or -1 */
static int su_hit(float x, float y, float y0, int n, float rh)
{
    if (x < GROUP_X || x >= CONTENT_R || y < y0 || y >= y0 + rh * n) return -1;
    return (int)((y - y0) / rh);
}
static void su_write(void)
{
    if (sh_fw_set(&SU.fw) < 0) toast("Can't change it now");
    su_load();
    build_app_page(AP.a);
}
static void su_asked(int r, const char *no)
{
    if (r < 0) toast(no);
}
static void su_toggle_shell(void)
{
    if (SU.have_fw < 0) { toast("No settings store"); return; }
    SU.fw.shell = !SU.fw.shell;
    su_write();
}

static int startup_tap(float x, float y)
{
    if (BACK_HIT(x, y)) {
        switch (SU.view) {
        case SU_ROOT: SU.loaded = 0; settings_back(); return 1;
        case SU_OSD: su_go(SU_OSL); return 1;
        case SU_PICK: su_go(SU_FW); return 1;
        default: su_go(SU_ROOT); return 1;
        }
    }
    float y0 = SU_TOP + 34;
    int r;
    switch (SU.view) {
    case SU_ROOT:
        r = su_hit(x, y, y0, 3, SU_ROW);
        if (r >= 0) { su_go(r == 0 ? SU_OSL : r == 1 ? SU_FW : SU_REC); return 1; }
        if (su_hit(x, y, y0 + 3 * SU_ROW + 6 + 30, 1, SU_ROW) == 0) { su_toggle_shell(); return 1; }
        return 0;
    case SU_OSL:
        r = su_hit(x, y, y0, SU.nos, SU_ROW);
        if (r >= 0) { SU.os = r; SU.view = SU_OSD; build_app_page(AP.a); return 1; }
        return 0;
    case SU_OSD: {
        if (SU.os < 0 || SU.os >= SU.nos) return 0;
        const su_os_t *o = &SU.o[SU.os];
        r = su_hit(x, y, y0, 2, SU_ROW);
        if (r == 0) { if (o->ready) su_asked(sh_os_start_once(o->key), "Can't start it now"); return 1; }
        if (r == 1) {
            if (!o->ready) { toast("Its files are missing"); return 1; }
            if (!o->def && sh_os_make_default(o->key) < 0) toast("Can't change the default now");
            su_load();
            build_app_page(AP.a);
            return 1;
        }
        int acts[5], n = su_req_n(o, acts), win = su_is_windows(o);
        float ry = y0 + 2 * SU_ROW + (o->ready ? 0 : 7 + 18) + 6 + 30;
        r = su_hit(x, y, ry, n + win, SU_ROW);
        if (r >= 0 && r < n) { if (o->ready) su_asked(sh_req_start(o->key, SU_REQ[acts[r]].act), "Can't start it now"); return 1; }
        if (r == n && win) { toast("Advanced Startup is a plan only"); return 1; }
        return 0;
    }
    case SU_FW: {
        r = su_hit(x, y, y0, 3, SU_ROW);
        if (r == 0) { su_toggle_shell(); return 1; }
        if (r > 0) { SU.pick = r == 1 ? PK_DEFAULT : PK_TIMEOUT; SU.view = SU_PICK; build_app_page(AP.a); return 1; }
        float y1 = y0 + 3 * SU_ROW + 6 + 30;
        r = su_hit(x, y, y1, 3, SU_ROW);
        if (r >= 0) { SU.pick = PK_DEBUG + r; SU.view = SU_PICK; build_app_page(AP.a); return 1; }
        r = su_hit(x, y, y1 + 3 * SU_ROW + 6 + 30, 2, SU_ROW);
        if (r >= 0) { SU.pick = PK_CRASH + r; SU.view = SU_PICK; build_app_page(AP.a); return 1; }
        return 0;
    }
    case SU_PICK: {
        const char *lab[SU_ROWS];
        int cur, n = su_pick_opts(SU.pick, lab, &cur);
        r = su_hit(x, y, y0, n, SU_ROW);
        if (r < 0) return 0;
        if (SU.have_fw < 0) { toast("No settings store"); return 1; }
        if (SU.pick == PK_DEFAULT && !SU.o[r].ready) { toast("Its files are missing"); return 1; }
        switch (SU.pick) {
        case PK_DEFAULT: tk_memcpy(SU.fw.default_key, SU.o[r].key, sizeof SU.fw.default_key); break;
        case PK_TIMEOUT: SU.fw.timeout_s = SU_TMO[r]; break;
        case PK_DEBUG: SU.fw.debug_level = r; break;
        case PK_POST: SU.fw.post = r; break;
        case PK_WDT: SU.fw.wdt_mode = r == 0 ? 0 : r == 1 ? 1 : 2; SU.fw.wdt_s = r == 2 ? 30 : r == 3 ? 60 : 0; break;
        case PK_CRASH: SU.fw.crash_mode = r; break;
        case PK_AFTER: SU.fw.crash_action = r; break;
        }
        SU.view = SU_FW;
        su_write();
        return 1;
    }
    case SU_REC: {
        int nc = SU.ncrash > 0 ? (SU.ncrash < SU_CRASHES ? SU.ncrash : SU_CRASHES) : 0;
        float yr = SU_TOP + 30 + (nc ? SU_CR_ROW * nc : SU_ROW) + 6 + 30 + 3 * SU_ROW + 6 + 30;
        r = su_hit(x, y, yr, 3, SU_ROW);
        if (r == 0) { su_asked(sh_restart_menu(), "Can't restart now"); return 1; }
        if (r == 1) { su_asked(sh_restart_recovery(), "Can't restart now"); return 1; }
        if (r == 2) { su_asked(sh_restart_download(), "Can't restart now"); return 1; }
        return 0;
    }
    }
    return 0;
}

/* the page on its root with fresh data (optional for Track B's Settings row: a fresh open reads by itself) */
__attribute__((unused)) static void startup_open(void)
{
    SU.view = SU_ROOT;
    SU.os = -1;
    su_load();
}
__attribute__((unused)) static int startup_tick(float dt) { (void)dt; return 0; }

#ifdef SBTK_STARTUP_PREVIEW
/* the host's stills (tools/startup-preview.sh): startup=root|os|osd:N|fw|pick:N|rec, opened as a tap would */
static void startup_set(const char *key, float v)
{
    startup_open();
    SU.view = !tk_strcmp(key, "os") ? SU_OSL : !tk_strcmp(key, "osd") ? SU_OSD : !tk_strcmp(key, "fw") ? SU_FW :
              !tk_strcmp(key, "pick") ? SU_PICK : !tk_strcmp(key, "rec") ? SU_REC : SU_ROOT;
    SU.os = (int)v;
    SU.pick = (int)v;
    build_app_page(AP.a);
}
#endif
