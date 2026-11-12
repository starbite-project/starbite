/* SPDX-License-Identifier: MIT
 * Track J: Settings > Startup (docs/STARBITE-FIRMWARE-LAYER.md 10: Starbite OS as the boot menu), on Track B's hook:
 * included by shell_lock.c after shell_settings.inc, whose "Startup" row opens it (AS.sub SE_STARTUP_PAGE, the page
 * strips); shell_lock.c calls page_startup (each strip) and startup_tap; nothing here moves, so no startup_tick. One
 * pushed page with sub-views on Track B's kit (shell_kit.inc: d_group / d_group_head / d_group_foot / d_row, the
 * switch kit_d_switch, kit_alert), the glass back circle and the inline title as Settings' other pages:
 *   Startup          Operating Systems >, Firmware >, Recovery >; Starbite OS as Boot Menu (the switch)
 *   OS list          every catalog OS (hidden ones not), the default marked, one that cannot start greyed
 *   OS detail        Start Once, Make Default, "Start once with" the one-shots that fit its kind (Windows: Advanced
 *                    Startup greyed, plan only)
 *   Firmware         the loader's settings: default OS, menu timeout, debug level, POST, watchdog, crash detection,
 *                    after a crash; each opens a picker (a check on the current one)
 *   Recovery         the last crash reports, this start (why, store, loader), restart to the boot menu / Starbite
 *                    Recovery / Download mode
 * The back circle: settings_back() at the root, else one level up. The platform's calls are shell.h's sh_os_*,
 * sh_fw_*, sh_req_*, sh_crash_*, sh_restart_*, sh_fw_info (the kernel: kern/sbfw_os.c; the host:
 * host/startup_fake.inc). Every restart and one-shot asks with the platform's hold confirmation. What the page shows
 * is read once when a view opens (su_load), never per strip. */
#define SHELL_STARTUP 1

enum { SU_ROOT, SU_OSL, SU_OSD, SU_FW, SU_PICK, SU_REC };
enum { PK_DEFAULT, PK_TIMEOUT, PK_DEBUG, PK_POST, PK_WDT, PK_CRASH, PK_AFTER };
#define SU_ROWS 12                          /* OS rows that fit the screen */
#define SU_CRASHES 3
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
/* a then b into o (cap bytes with the 0) */
static void su_catn(char *o, int cap, const char *a, const char *b)
{
    int n = 0;
    while (*a && n < cap - 1) o[n++] = *a++;
    while (b && *b && n < cap - 1) o[n++] = *b++;
    o[n] = 0;
}
static void su_cat(char *o, const char *a, const char *b) { su_catn(o, 96, a, b); }
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

/* ---- the list: the kit's d_group / d_row (cards LG_X / LG_W / LG_RAD, LG_ROW rows, LG_GAP between cards) -------------
 * One layout serves the drawing and the taps: su_lay(d) with d 0 only measures and fills the hit table (SUH), so a tap
 * finds the row the strips drew. Sections as shell_settings.inc's se_layout: the first card at PUSHED_TOP - 8 (26 more
 * with a header), LG_GAP between cards (12 more when a header follows a footer). */
#define SU_Y0 (PUSHED_TOP - 8)
#define SU_TWO 66.0f                        /* a two-line row (the crash reports), as Settings' rows with a subtitle */
#define SU_SWX (LG_RE - 16 - KSW_W)          /* the switch, where Settings puts its switches */
#define SU_MAXHIT 32
enum { R_NONE, R_OSL, R_FW, R_REC, R_SHELL, R_ONCE, R_MKDEF, R_WINRE, R_RST_MENU, R_RST_REC, R_RST_DL,
       R_OS0 = 20, R_REQ0 = 40, R_PICK0 = 50, R_OPT0 = 60 };
static struct { float y, h, gy, gh; int id, sw, dim; } SUH[SU_MAXHIT];   /* cp6: gy / gh its card; sw a switch row; dim no press */
static struct { float y, rh, gy; int nh, i, n, first, foot; } SUL;
static void su_begin(void) { SUL.y = SU_Y0; SUL.nh = 0; SUL.first = 1; SUL.foot = 0; SUL.n = SUL.i = 0; }
/* a card of n rows rh high, under an optional header */
static void su_sec(dc *d, const char *head, int n, float rh)
{
    if (!SUL.first) SUL.y += LG_GAP + (head && SUL.foot ? 12 : 0);
    else if (head) SUL.y += 26;
    SUL.first = 0;
    SUL.foot = 0;
    if (d && head) d_group_head(d, SUL.y, head);
    if (d && n) d_group(d, SUL.y, rh * n);
    SUL.i = 0;
    SUL.n = n;
    SUL.rh = rh;
    SUL.gy = SUL.y;
}
/* the next row's top (id: what a tap on it does, R_NONE nothing) */
static float su_next(int id)
{
    float y = SUL.y;
    if (id && SUL.nh < SU_MAXHIT) {
        SUH[SUL.nh].y = y; SUH[SUL.nh].h = SUL.rh; SUH[SUL.nh].id = id;
        SUH[SUL.nh].gy = SUL.gy; SUH[SUL.nh].gh = SUL.rh * SUL.n; SUH[SUL.nh].sw = SUH[SUL.nh].dim = 0;
        SUL.nh++;
    }
    SUL.y += SUL.rh;
    SUL.i++;
    return y;
}
static int su_dim(tk_col c) { tk_col l3 = C_LABEL3; return c.a == l3.a && c.r == l3.r && c.g == l3.g && c.b == l3.b; }
static void su_row(dc *d, int id, const char *l, tk_col lc, const char *v, int chev)
{
    int last = SUL.i + 1 >= SUL.n;
    float y = su_next(id);
    if (id && SUL.nh && SUH[SUL.nh - 1].id == id) SUH[SUL.nh - 1].dim = su_dim(lc) && !chev;   /* cp6: a greyed action: no press */
    if (d) d_row(d, y, SUL.rh, 0, l, v, chev, last, lc, 0);
}
/* a row with the kit's switch at the right */
static void su_switch_row(dc *d, int id, const char *l, int on, int enabled)
{
    int last = SUL.i + 1 >= SUL.n;
    float y = su_next(id);
    if (id && SUL.nh && SUH[SUL.nh - 1].id == id) { SUH[SUL.nh - 1].sw = 1; SUH[SUL.nh - 1].dim = !enabled; }
    if (!d) return;
    d_row(d, y, SUL.rh, 0, l, 0, 0, last, enabled ? INK : C_LABEL3, SU_SWX - 12);
    kit_d_switch(d, SU_SWX, y + (SUL.rh - KSW_H) / 2, on, enabled);
}
/* a picker's row: the check (accent) at the right when it is the current one */
static void su_check_row(dc *d, int id, const char *l, tk_col lc, const char *v, int on)
{
    int last = SUL.i + 1 >= SUL.n;
    float y = su_next(id);
    if (id && SUL.nh && SUH[SUL.nh - 1].id == id) SUH[SUL.nh - 1].dim = su_dim(lc);   /* cp6: a greyed choice: no press */
    if (!d) return;
    d_row(d, y, SUL.rh, 0, l, v, 0, last, lc, on ? LG_RE - 16 - 14 - 12 : 0);
    if (on) {
        float x = LG_RE - 16 - 14, cy = y + SUL.rh / 2 + 1;
        d_seg(d, x, cy, x + 5, cy + 5, 2.2f, KIT_BLUE());
        d_seg(d, x + 5, cy + 5, x + 14, cy - 6, 2.2f, KIT_BLUE());
    }
}
static void su_foot(dc *d, const char *t)
{
    SUL.y += d_group_foot(d, SUL.y, t);
    SUL.foot = 1;
}
static void su_head(dc *d, const char *title)
{
    if (!d) return;
    d_back(d, W(1));                       /* iOS 26's glass back circle and the inline title, as Settings' pages */
    d_navtitle(d, title);
}

static void su_page_root(dc *d)
{
    su_head(d, "Startup");
    char b[96];
    su_sec(d, 0, 3, LG_ROW);
    int n = put_dec(b, SU.nos + SU.more);
    b[n] = 0;
    su_row(d, R_OSL, "Operating Systems", INK, b, 1);
    su_row(d, R_FW, "Firmware", INK, "", 1);
    if (SU.ncrash > 0) { n = put_dec(b, SU.ncrash); su_cat(b + n, SU.ncrash == 1 ? " crash" : " crashes", 0); }
    else b[0] = 0;
    su_row(d, R_REC, "Recovery", INK, b, 1);
    su_sec(d, "At power-on", 1, LG_ROW);
    su_switch_row(d, R_SHELL, "Starbite OS as Boot Menu", SU.fw.shell, SU.have_fw >= 0);
    su_foot(d, SU.have_fw < 0 ? "No settings store: the loader keeps its defaults."
               : SU.fw.shell ? "Starbite OS starts first and is the boot menu. Hold Volume Down at power-on for the "
                               "loader's own menu; after a crash it comes back by itself."
                             : "The loader's menu starts first, then the default OS after its timeout.");
}

static void su_page_osl(dc *d)
{
    su_head(d, "Operating Systems");
    if (!SU.nos) { if (d) d_empty(d, CONTENT_TOP, EMPTY_BOT, "No Systems", "The store's catalog lists no OS."); return; }
    su_sec(d, 0, SU.nos, LG_ROW);
    for (int i = 0; i < SU.nos; i++) {
        const su_os_t *o = &SU.o[i];
        su_row(d, R_OS0 + i, o->name, o->ready ? INK : C_LABEL3, o->def ? "Default" : o->ready ? "" : "Not Installed", 1);
    }
    char b[96], f[192];
    b[0] = 0;
    if (SU.more > 0) {
        int n = put_dec(b, SU.more);
        su_catn(b + n, (int)sizeof b - n, " more in the catalog: the loader's menu lists them all. ", 0);
    }
    su_catn(f, (int)sizeof f, b, "The default starts by itself after the menu's timeout. Tap a system to start it once or make it the default.");
    su_foot(d, f);
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
    su_sec(d, 0, 2, LG_ROW);
    su_row(d, R_ONCE, "Start Once", o->ready ? KIT_BLUE() : C_LABEL3, "", 0);
    su_check_row(d, R_MKDEF, o->def ? "Default System" : "Make Default", o->def || !o->ready ? C_LABEL3 : KIT_BLUE(), "", o->def);
    if (!o->ready) su_foot(d, "Its files are not in the store, so it cannot start.");
    int acts[5], n = su_req_n(o, acts), win = su_is_windows(o);
    if (n || win) {
        su_sec(d, "Start once with", n + win, LG_ROW);
        for (int i = 0; i < n; i++) su_row(d, R_REQ0 + i, SU_REQ[acts[i]].name, o->ready ? KIT_BLUE() : C_LABEL3, "", 0);
        if (win) su_row(d, R_WINRE, "Advanced Startup", C_LABEL3, "Plan Only", 0);
        su_foot(d, "For the next start only: the loader applies it, then clears it.");
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
    su_sec(d, 0, 3, LG_ROW);
    su_switch_row(d, R_SHELL, "Starbite OS as Boot Menu", SU.fw.shell, SU.have_fw >= 0);
    su_row(d, R_PICK0 + PK_DEFAULT, "Default System", INK, su_os_name(SU.fw.default_key), 1);
    su_row(d, R_PICK0 + PK_TIMEOUT, "Menu Timeout", INK, su_tmo_text(b), 1);
    su_sec(d, "Diagnostics", 3, LG_ROW);
    su_row(d, R_PICK0 + PK_DEBUG, "Debug Level", INK, SU_DEBUG[SU.fw.debug_level & 3], 1);
    su_row(d, R_PICK0 + PK_POST, "POST", INK, SU_POST[SU.fw.post < 3 ? SU.fw.post : 0], 1);
    su_row(d, R_PICK0 + PK_WDT, "Watchdog", INK, su_wdt_text(b), 1);
    su_sec(d, "Crashes", 2, LG_ROW);
    su_row(d, R_PICK0 + PK_CRASH, "Crash Detection", INK, SU_CRASH[SU.fw.crash_mode < 3 ? SU.fw.crash_mode : 0], 1);
    su_row(d, R_PICK0 + PK_AFTER, "After a Crash", INK, SU_AFTER[SU.fw.crash_action < 3 ? SU.fw.crash_action : 0], 1);
    su_foot(d, SU.have_fw < 0 ? "No settings store: nothing can be changed." : "The loader reads these at its next start.");
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
    su_sec(d, 0, n, LG_ROW);
    for (int i = 0; i < n; i++) {
        int off = SU.pick == PK_DEFAULT && !SU.o[i].ready;
        su_check_row(d, R_OPT0 + i, lab[i], off ? C_LABEL3 : INK, off ? "Not Installed" : "", i == cur);
    }
    su_foot(d, su_pick_note(SU.pick));
}

/* a crash report: the OS and the time on the first line, the kind (red for a crash) and its message on the second */
static void su_crash_row(dc *d, const sh_crash_t *c)
{
    int last = SUL.i + 1 >= SUL.n;
    float y = su_next(R_NONE), h = SUL.rh;
    if (!d) return;
    char when[40];
    su_date(c->utc, when);
    float room = LG_RE - 16 - LG_TX, ww = when[0] ? tw(TK_IT400, 15, when, 0, TK_TNUM) : 0;
    d_text_box(d, TK_IT400, 17, LG_TX, y + 28, room - ww - (ww > 0 ? 12 : 0), 0, c->os, 0, 0, INK, 0);
    if (ww > 0) d_text_box(d, TK_IT400, 15, LG_RE - 16, y + 28, ww + 2, 2, when, 0, TK_TNUM, KIT_SEC(), 0);
    const char *kind = su_kind(c->kind);
    float kw = tk_minf(tw(TK_IT600, 15, kind, 0, 0), room);
    d_text_box(d, TK_IT600, 15, LG_TX, y + 48, room, 0, kind, 0, 0, c->severity >= 2 ? KIT_RED() : KIT_SEC(), 0);
    if (c->line[0] && room - kw > 40) {
        char l2[112];
        su_cat(l2, "\xc2\xb7 ", c->line);
        d_text_box(d, TK_IT400, 15, LG_TX + kw + 5, y + 48, room - kw - 5, 0, l2, 0, 0, KIT_SEC(), 0);
    }
    if (!last) d_rrect(d, LG_TX, y + h - .5f, LG_RE - LG_TX, .5f, 0, C_SEP);
}
static void su_page_rec(dc *d)
{
    su_head(d, "Recovery");
    int nc = SU.ncrash > 0 ? (SU.ncrash < SU_CRASHES ? SU.ncrash : SU_CRASHES) : 0;
    if (!nc) {
        su_sec(d, "Crash reports", 1, LG_ROW);
        su_row(d, R_NONE, SU.ncrash < 0 ? "No crash log on this store" : "No crashes", KIT_SEC(), "", 0);
    } else {
        su_sec(d, "Crash reports", nc, SU_TWO);
        for (int i = 0; i < nc; i++) su_crash_row(d, &SU.cr[i]);
    }
    su_sec(d, "This start", 3, LG_ROW);
    char b[96];
    su_row(d, R_NONE, "Why", INK, SU.inf.why, 0);
    su_cat(b, SU.inf.medium == 1 ? "Memory Card" : SU.inf.medium == 0 ? "Phone (UFS)" : "None",
           SU.inf.card_only ? " \xc2\xb7 card only" : 0);
    su_row(d, R_NONE, "Store", INK, b, 0);
    if (SU.inf.valid) { su_cat(b, "Version ", 0); int n = 8; n += put_dec(b + n, SU.inf.loader_version); b[n] = 0; }
    else su_cat(b, "Older (no table)", 0);
    su_row(d, R_NONE, "Loader", INK, b, 0);
    su_sec(d, "Restart", 3, LG_ROW);
    su_row(d, R_RST_MENU, "Restart to Boot Menu", KIT_BLUE(), "", 0);
    su_row(d, R_RST_REC, "Restart to Starbite Recovery", KIT_BLUE(), "", 0);
    su_row(d, R_RST_DL, "Restart to Download Mode", KIT_RED(), "", 0);
    su_foot(d, "Each asks first. The boot menu and Recovery are this once; the next start is as before.");
}

/* the view's layout: drawn into d, or with d 0 only the hit table */
static void su_lay(dc *d)
{
    su_begin();
    switch (SU.view) {
    case SU_OSL: su_page_osl(d); break;
    case SU_OSD: su_page_osd(d); break;
    case SU_FW: su_page_fw(d); break;
    case SU_PICK: su_page_pick(d); break;
    case SU_REC: su_page_rec(d); break;
    default: su_page_root(d); break;
    }
}
static void page_startup(dc *d)
{
    if (!SU.loaded) su_load();               /* the first strip of a fresh open reads; the other three draw */
    su_lay(d);
}

/* ---- taps -------------------------------------------------------------------------------------------------------- */
static void su_go(int view)
{
    SU.view = view;
    su_load();
    build_app_page(AP.a);
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
static void su_ok(int i) { (void)i; }
/* the row id under a tap, R_NONE for none */
static int su_hit(float x, float y)
{
    if (x < LG_X || x >= LG_RE) return R_NONE;
    su_lay(0);
    for (int i = 0; i < SUL.nh; i++) if (y >= SUH[i].y && y < SUH[i].y + SUH[i].h) return SUH[i].id;
    return R_NONE;
}

static int startup_tap(float x, float y)
{
    if (kit_on()) return 0;                  /* an alert of the kit's is up: only its buttons answer */
    if (BACK_HIT(x, y)) {
        switch (SU.view) {
        case SU_ROOT: SU.loaded = 0; SU.os = -1; settings_back(); return 1;
        case SU_OSD: su_go(SU_OSL); return 1;
        case SU_PICK: su_go(SU_FW); return 1;
        default: su_go(SU_ROOT); return 1;
        }
    }
    if (!SU.loaded) su_load();
    int id = su_hit(x, y);
    switch (id) {
    case R_NONE: return 0;
    case R_OSL: su_go(SU_OSL); return 1;
    case R_FW: su_go(SU_FW); return 1;
    case R_REC: su_go(SU_REC); return 1;
    case R_SHELL: su_toggle_shell(); return 1;
    case R_RST_MENU: su_asked(sh_restart_menu(), "Can't restart now"); return 1;
    case R_RST_REC: su_asked(sh_restart_recovery(), "Can't restart now"); return 1;
    case R_RST_DL: su_asked(sh_restart_download(), "Can't restart now"); return 1;
    default: break;
    }
    if (id >= R_OS0 && id < R_OS0 + SU.nos) { SU.os = id - R_OS0; SU.view = SU_OSD; build_app_page(AP.a); return 1; }
    if (id >= R_PICK0 && id <= R_PICK0 + PK_AFTER) { SU.pick = id - R_PICK0; SU.view = SU_PICK; build_app_page(AP.a); return 1; }
    if (SU.view == SU_OSD && SU.os >= 0 && SU.os < SU.nos) {
        const su_os_t *o = &SU.o[SU.os];
        if (id == R_ONCE) {
            if (o->ready) su_asked(sh_os_start_once(o->key), "Can't start it now");
            else toast("Its files are missing: it can't start");   /* cp6: never a silent tap */
            return 1;
        }
        if (id == R_MKDEF) {
            if (!o->ready) { toast("Its files are missing"); return 1; }
            if (o->def) { toast("It is the default already"); return 1; }   /* cp6 */
            if (!o->def && sh_os_make_default(o->key) < 0) toast("Can't change the default now");
            su_load();
            build_app_page(AP.a);
            return 1;
        }
        if (id == R_WINRE) {
            static const kit_btn OK[1] = { { "OK", KIT_PREFERRED } };
            kit_alert("Advanced Startup", "Windows' recovery start is a plan only: the loader can't start it yet.", OK, 1, su_ok);
            return 1;
        }
        int acts[5], n = su_req_n(o, acts);
        if (id >= R_REQ0 && id < R_REQ0 + n) {
            if (o->ready) su_asked(sh_req_start(o->key, SU_REQ[acts[id - R_REQ0]].act), "Can't start it now");
            else toast("Its files are missing: it can't start");   /* cp6 */
            return 1;
        }
        return 0;
    }
    if (SU.view == SU_PICK && id >= R_OPT0) {
        const char *lab[SU_ROWS];
        int cur, n = su_pick_opts(SU.pick, lab, &cur), r = id - R_OPT0;
        if (r >= n) return 0;
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
    return 0;
}

/* cp6: what lights under the finger (shell_settings.inc's se_strip_press asks; shell_wifi.inc's press colours): a row
 * its rect in its card, a switch row its switch's track; greyed actions and choices don't light (a tap says why) */
static int startup_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    if (kit_on() || x < LG_X || x >= LG_RE) return 0;
    if (!SU.loaded) su_load();
    su_lay(0);
    for (int i = 0; i < SUL.nh; i++)
        if (y >= SUH[i].y && y < SUH[i].y + SUH[i].h) {
            if (SUH[i].dim) return 0;
            if (SUH[i].sw) return sep_switch(r, g, rad, c, SU_SWX, SUH[i].y + (SUH[i].h - KSW_H) / 2);
            return sep_row(r, g, rad, c, SUH[i].y, SUH[i].h, SUH[i].gy, SUH[i].gh);
        }
    return 0;
}
