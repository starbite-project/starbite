/* SPDX-License-Identifier: MIT
 * Track A alarms and time (2026-11-12), included by shell_lock.c after shell_comms.inc. Minimal pages on the
 * platform's API (shell.h: sh_tz_*, sh_clock24*, sh_alarm_*); the UI track restyles them:
 *   - the Clock app's tabs (Clock | Alarms) in its nav row, and the Alarm tab: up to four alarms with their switch,
 *     "+" to add, a tap to edit (hour and minute steppers, the days, Delete);
 *   - Settings > Date & Time: 24-Hour Time, Time Zone (the zone list: regions, a vertical swipe scrolls, the zone in
 *     use checked; a tap chooses it and goes back);
 *   - the stills' states: clock24=:0|1, tz=Europe/Paris, sub=datetime|tz, clocktab=alarms|clock, alarmedit=:i.
 * Ringing, snooze and the lock's next alarm stay the platform's (sh_alarm). */

#define TA_SEG_X 95.0f                      /* the Clock app's tabs in its nav row */
#define TA_SEG_W 200.0f
#define TA_SEG_Y (SAFE_TOP + 6)
#define TA_SEG_H 32.0f
#define TA_ROW_H 84.0f                      /* an alarm's row */
#define TA_ZROW 44.0f                       /* a zone's row */
#define TA_ZSEG_Y PUSHED_TOP                /* the zone list's region tabs */
#define TA_ZTOP (PUSHED_TOP + 48)           /* its first row */
#define TA_ZBOT 740.0f
#define TA_ZN ((int)((TA_ZBOT - TA_ZTOP) / TA_ZROW))

static struct {
    int tab;                                /* the Clock app: 0 Clock, 1 Alarms */
    int edit;                               /* the alarm being edited (-1 none; sh_alarm_count(): a new one) */
    int ehm, edays, eon;
    char elabel[20];
    int tzg, tztop;                         /* the zone list: its region, its first row shown */
    int own;                                /* a touch on the zone list (its scroll) */
    float x0, y0;
    int moved;
} TA = { 0, -1, 700, 0x3e, 1, "", 0, 0, 0, 0, 0, 0 };

static int ta_alarm_tab(void) { return TA.tab == 1; }

/* the time text was restyled (12 / 24-hour): every time on screen again (sh_status's rebuilds) */
static void ta_restyle(void)
{
    int h = ST.h, p = ST.pct;
    ST.h = -1;
    ST.pct = -1;
    sh_status(h, ST.mi, ST.wday, ST.day, ST.mon, p, ST.temp);
}

static void ta_switch(dc *d, float y, int on)       /* an iOS switch at the row's right */
{
    float x = CONTENT_R - 16 - 51;
    d_rrect(d, x, y + 8, 51, 31, 15.5f, on ? tk_hex(0x34c759, 1) : W(.16f));
    d_rrect(d, on ? x + 22 : x + 2, y + 10, 27, 27, 13.5f, W(1));
}
static void ta_check(dc *d, float cx, float cy, tk_col c)
{
    d_seg(d, cx - 7, cy + .5f, cx - 2, cy + 5.5f, 2.4f, c);
    d_seg(d, cx - 2, cy + 5.5f, cx + 7.5f, cy - 6, 2.4f, c);
}
static void ta_plus(dc *d, float cx, float cy, tk_col c)
{
    d_seg(d, cx - 8, cy, cx + 8, cy, 2.4f, c);
    d_seg(d, cx, cy - 8, cx, cy + 8, 2.4f, c);
}
static void ta_pill(dc *d, float x, float y, float w, float h, const char *t, tk_col bg, tk_col fg)
{
    d_rrect(d, x, y, w, h, h / 2, bg);
    float tw0 = tw(TK_IT600, 15, t, 0, 0);
    d_text(d, TK_IT600, 15, x + (w - tw0) / 2, bl(TK_IT600, 15, y + h / 2 - 9.5f), t, 0, 0, fg, 0);
}
static int ta_in(float x, float y, float rx, float ry, float rw, float rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

/* "Every day", "Weekdays", "Weekends", "Never" (once) or "Mon Wed Fri" */
static void ta_days_text(char *b, int days)
{
    static const char *const D3[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    const char *w = days == 0x7f ? "Every day" : days == 0x3e ? "Weekdays" : days == 0x41 ? "Weekends" : !days ? "Never" : 0;
    int n = 0;
    if (w) { while (*w) b[n++] = *w++; b[n] = 0; return; }
    for (int i = 1; i < 8; i++)                     /* Monday first */
        if (days & 1 << (i % 7)) {
            if (n) b[n++] = ' ';
            for (const char *p = D3[i % 7]; *p;) b[n++] = *p++;
        }
    b[n] = 0;
}
/* "UTC+2", "UTC+5:30", "UTC-3", "UTC" */
static int ta_off_text(char *b, int off)
{
    int n = 0;
    for (const char *p = "UTC"; *p;) b[n++] = *p++;
    if (off) {
        b[n++] = off < 0 ? '-' : '+';
        if (off < 0) off = -off;
        n += put_dec(b + n, off / 60);
        if (off % 60) { b[n++] = ':'; b[n++] = (char)('0' + off % 60 / 10); b[n++] = (char)('0' + off % 10); }
    }
    b[n] = 0;
    return n;
}

/* ---- the Clock app: the tabs and the Alarm tab -------------------------------------------------------------- */
static void ta_tabs(dc *d)
{
    d_rrect(d, TA_SEG_X, TA_SEG_Y, TA_SEG_W, TA_SEG_H, TA_SEG_H / 2, W(.12f));
    d_rrect(d, TA_SEG_X + 2 + TA.tab * (TA_SEG_W / 2), TA_SEG_Y + 2, TA_SEG_W / 2 - 4, TA_SEG_H - 4, (TA_SEG_H - 4) / 2, W(.26f));
    static const char *const L[2] = { "Clock", "Alarms" };
    for (int i = 0; i < 2; i++) {
        float w = tw(TK_IT600, 15, L[i], 0, 0);
        d_text(d, TK_IT600, 15, TA_SEG_X + TA_SEG_W / 4 * (2 * i + 1) - w / 2, bl(TK_IT600, 15, TA_SEG_Y + 6.5f), L[i], 0, 0,
               TA.tab == i ? INK : W(.6f), 0);
    }
}
static void ta_alarm_list(dc *d)
{
    d_h1(d, "Alarms");
    d_gcircle(d, GC_DONE);                          /* "+" on the glass circle at the nav row's right */
    ta_plus(d, CONTENT_R - BACK_D / 2, NAV_MID, ACCENT());
    int n = sh_alarm_count();
    if (!n) { d_empty(d, CONTENT_TOP, EMPTY_BOT, "No Alarms", "Tap + to add an alarm. It rings with the vibration motor."); return; }
    d_rrect(d, GROUP_X, CONTENT_TOP, GROUP_W, TA_ROW_H * n - 1, 16, tk_hex(0x151c33, 1));
    for (int i = 0; i < n; i++) {
        int hm = 0, days = 0, on = 0;
        char lab[20] = "", t[12], sub[64];
        if (sh_alarm_get(i, &hm, &days, &on, lab, sizeof lab) < 0) continue;
        float y = CONTENT_TOP + TA_ROW_H * i;
        fmt_time(t, hm / 100, hm % 100, 0);
        tk_col c = on ? INK : W(.45f);
        d_text(d, TK_IT400, 44, ROW_TX, bl(TK_IT400, 44, y + 8), t, -1, TK_TNUM, c, 0);
        if (!sh_clock24()) {
            float w = tw(TK_IT400, 44, t, -1, TK_TNUM);
            d_text(d, TK_IT500, 20, ROW_TX + w + 4, bl(TK_IT400, 44, y + 8), hm / 100 < 12 ? "AM" : "PM", 0, 0, c, 0);
        }
        int m = 0;
        for (const char *p = lab[0] ? lab : "Alarm"; *p;) sub[m++] = *p++;
        sub[m++] = ','; sub[m++] = ' ';
        ta_days_text(sub + m, days);
        d_text_box(d, TK_IT400, 15, ROW_TX, bl(TK_IT400, 15, y + 56), 240, 0, sub, 0, 0, on ? W(.7f) : W(.4f), 0);
        ta_switch(d, y + (TA_ROW_H - 47) / 2, on);
        if (i + 1 < n) d_rrect(d, ROW_TX, y + TA_ROW_H - 1, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
    }
    if (n >= 4) d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, CONTENT_TOP + TA_ROW_H * n + 10), "Four alarms is the most.", 0, 0, W(.4f), 0);
}
#define TA_E_TIME 150.0f                    /* the editor: the time, the steppers, the days, Delete */
#define TA_E_STEP 270.0f
#define TA_E_DAYS 420.0f
#define TA_E_DEL 520.0f
static void ta_alarm_editor(dc *d)
{
    int add = TA.edit >= sh_alarm_count();
    d_back(d, ACCENT());                            /* back: Cancel */
    d_navtitle(d, add ? "Add Alarm" : "Edit Alarm");
    d_gcircle(d, GC_DONE);                          /* Save */
    ta_check(d, CONTENT_R - BACK_D / 2, NAV_MID, ACCENT());
    char t[12];
    fmt_time(t, TA.ehm / 100, TA.ehm % 100, 0);
    const char *ap = sh_clock24() ? "" : TA.ehm / 100 < 12 ? "AM" : "PM";
    float w = tw(TK_IT400, 72, t, -1, TK_TNUM), wa = ap[0] ? tw(TK_IT500, 28, ap, 0, 0) + 8 : 0;
    d_text(d, TK_IT400, 72, (390 - w - wa) / 2, bl(TK_IT400, 72, TA_E_TIME), t, -1, TK_TNUM, INK, 0);
    if (ap[0]) d_text(d, TK_IT500, 28, (390 - w - wa) / 2 + w + 8, bl(TK_IT400, 72, TA_E_TIME), ap, 0, 0, INK, 0);
    /* Hour: - +; Minute: -10 -1 +1 +10 */
    d_rrect(d, GROUP_X, TA_E_STEP, GROUP_W, 2 * 56 - 1, 16, tk_hex(0x151c33, 1));
    d_text(d, TK_IT400, 16, ROW_TX, bl(TK_IT400, 16, TA_E_STEP + 18), "Hour", 0, 0, INK, 0);
    d_text(d, TK_IT400, 16, ROW_TX, bl(TK_IT400, 16, TA_E_STEP + 56 + 18), "Minute", 0, 0, INK, 0);
    d_rrect(d, ROW_TX, TA_E_STEP + 55, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
    static const char *const HS[2] = { "\xe2\x88\x92", "+" }, *const MS[4] = { "\xe2\x88\x92" "10", "\xe2\x88\x92" "1", "+1", "+10" };
    for (int i = 0; i < 2; i++) ta_pill(d, CONTENT_R - 12 - 2 * 56 + 4 + 56 * i, TA_E_STEP + 10, 52, 36, HS[i], W(.14f), INK);
    for (int i = 0; i < 4; i++) ta_pill(d, CONTENT_R - 12 - 4 * 52 + 4 + 52 * i, TA_E_STEP + 66, 48, 36, MS[i], W(.14f), INK);
    /* the days: S M T W T F S */
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, TA_E_DAYS - 22), "REPEAT", .52f, 0, W(.55f), 0);
    static const char *const DL[7] = { "S", "M", "T", "W", "T", "F", "S" };
    for (int i = 0; i < 7; i++) {
        float cx = GROUP_X + 25 + i * (GROUP_W - 50) / 6;
        int on = TA.edays >> i & 1;
        d_rrect(d, cx - 21, TA_E_DAYS, 42, 42, 21, on ? ACCENT() : W(.12f));
        float lw = tw(TK_IT600, 16, DL[i], 0, 0);
        d_text(d, TK_IT600, 16, cx - lw / 2, bl(TK_IT600, 16, TA_E_DAYS + 11), DL[i], 0, 0, on ? tk_hex(0x0a0f1f, 1) : INK, 0);
    }
    char dt[64];
    ta_days_text(dt, TA.edays);
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, TA_E_DAYS + 52), TA.edays ? dt : "Once (then it turns itself off)", 0, 0, W(.45f), 0);
    if (!add) {
        d_rrect(d, GROUP_X, TA_E_DEL, GROUP_W, 47, 16, tk_hex(0x151c33, 1));
        float dw = tw(TK_IT400, 16, "Delete Alarm", 0, 0);
        d_text(d, TK_IT400, 16, 195 - dw / 2, bl(TK_IT400, 16, TA_E_DEL + 13), "Delete Alarm", 0, 0, tk_hex(0xff453a, 1), 0);
    }
}
static void ta_edit_begin(int i)
{
    TA.edit = i;
    TA.ehm = 700; TA.edays = 0x3e; TA.eon = 1;
    tk_memcpy(TA.elabel, "Alarm", 6);
    if (i < sh_alarm_count()) {
        TA.elabel[0] = 0;
        if (sh_alarm_get(i, &TA.ehm, &TA.edays, &TA.eon, TA.elabel, sizeof TA.elabel) < 0) TA.edit = -1;
    }
}
static int ta_clock_page(dc *d)
{
    if (TA.edit >= 0 && TA.tab == 1) { ta_alarm_editor(d); return 1; }
    ta_tabs(d);
    if (TA.tab == 1) { ta_alarm_list(d); return 1; }
    return 0;                                       /* the Clock tab: page_clock as it was */
}
static int ta_clock_tap(float x, float y)
{
    if (TA.tab == 1 && TA.edit >= 0) {
        int n = sh_alarm_count();
        if (BACK_HIT(x, y)) TA.edit = -1;                               /* Cancel */
        else if (DONE_HIT(x, y)) {                                       /* Save: on */
            if (sh_alarm_put(TA.edit, TA.ehm, TA.edays, 1, TA.elabel) < 0) toast("The alarm can't be set now");
            TA.edit = -1;
        } else if (TA.edit < n && ta_in(x, y, GROUP_X, TA_E_DEL, GROUP_W, 47)) {
            if (sh_alarm_del(TA.edit) < 0) toast("The alarm can't be deleted now");
            TA.edit = -1;
        } else if (y >= TA_E_STEP && y < TA_E_STEP + 112) {
            int t = TA.ehm / 100 * 60 + TA.ehm % 100, dm = 0;
            if (y < TA_E_STEP + 56) {
                float x0 = CONTENT_R - 12 - 2 * 56 + 4;
                if (x >= x0 && x < x0 + 112) dm = x < x0 + 56 ? -60 : 60;
            } else {
                static const int M[4] = { -10, -1, 1, 10 };
                float x0 = CONTENT_R - 12 - 4 * 52 + 4;
                if (x >= x0 && x < x0 + 208) dm = M[(int)((x - x0) / 52)];
            }
            if (!dm) return 1;
            if (dm == 60 || dm == -60) t = ((t / 60 + dm / 60 + 24) % 24) * 60 + t % 60;   /* the hour alone */
            else t = t / 60 * 60 + ((t % 60 + dm) % 60 + 60) % 60;                             /* the minute alone */
            TA.ehm = t / 60 * 100 + t % 60;
        } else if (y >= TA_E_DAYS - 4 && y < TA_E_DAYS + 46) {
            int i = (int)((x - GROUP_X + (GROUP_W - 50) / 12 - 25) / ((GROUP_W - 50) / 6));
            if (i < 0 || i > 6) return 1;
            TA.edays ^= 1 << i;
        } else return 1;
        build_app_page(AP.a);
        return 1;
    }
    if (IN_NAV(y) && x >= TA_SEG_X && x < TA_SEG_X + TA_SEG_W) {
        int t = x >= TA_SEG_X + TA_SEG_W / 2;
        if (t != TA.tab) { TA.tab = t; TA.edit = -1; build_app_page(AP.a); }
        return 1;
    }
    if (TA.tab != 1) return 0;
    if (DONE_HIT(x, y)) {                                                /* + */
        if (sh_alarm_count() >= 4) { toast("Four alarms is the most"); return 1; }
        ta_edit_begin(sh_alarm_count());
        build_app_page(AP.a);
        return 1;
    }
    int n = sh_alarm_count();
    if (x >= GROUP_X && x < CONTENT_R && y >= CONTENT_TOP && y < CONTENT_TOP + TA_ROW_H * n) {
        int i = (int)((y - CONTENT_TOP) / TA_ROW_H);
        if (x >= CONTENT_R - 16 - 51 - 8) {                              /* the switch */
            int hm = 0, days = 0, on = 0;
            char lab[20] = "";
            if (sh_alarm_get(i, &hm, &days, &on, lab, sizeof lab) == 0 && sh_alarm_put(i, hm, days, !on, lab) < 0)
                toast("The alarm can't be changed now");
        } else ta_edit_begin(i);
        build_app_page(AP.a);
    }
    return 1;
}

/* ---- Settings > Date & Time and the zone list ---------------------------------------------------------------- */
#define TA_DT_Y (PUSHED_TOP + 8)            /* 24-Hour Time */
#define TA_TZ_Y (TA_DT_Y + 47 + 24)         /* Time Zone */
static void ta_page_dt(dc *d)
{
    d_back(d, W(1));
    d_navtitle(d, "Date & Time");
    d_rrect(d, GROUP_X, TA_DT_Y, GROUP_W, 47, 16, tk_hex(0x151c33, 1));
    d_text(d, TK_IT400, 16, ROW_TX, TA_DT_Y + 13 + 16, "24-Hour Time", 0, 0, INK, 0);
    ta_switch(d, TA_DT_Y, sh_clock24());
    int z = sh_tz();
    const char *city = "";
    (void)sh_tz_name(z, &city);
    d_rrect(d, GROUP_X, TA_TZ_Y, GROUP_W, 47, 16, tk_hex(0x151c33, 1));
    d_text(d, TK_IT400, 16, ROW_TX, TA_TZ_Y + 13 + 16, "Time Zone", 0, 0, INK, 0);
    d_icon(d, "chev", CONTENT_R - 30, TA_TZ_Y + 16.5f, .45f);
    d_text_box(d, TK_IT400, 16, CONTENT_R - 36, TA_TZ_Y + 13 + 16, 180, 2, city, 0, 0, W(.55f), 0);
    char ln[96], hm[12];
    int n = 0;
    const char *name = sh_tz_name(z, 0);
    while (*name && n < 40) ln[n++] = *name++;
    ln[n++] = ','; ln[n++] = ' ';
    n += ta_off_text(ln + n, sh_tz_offset_min(z));
    for (const char *p = ", now "; *p;) ln[n++] = *p++;
    fmt_time(hm, ST.h, ST.mi, 1);
    for (const char *p = hm; *p;) ln[n++] = *p++;
    ln[n] = 0;
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, TA_TZ_Y + 47 + 8), ln, 0, 0, W(.45f), 0);
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, TA_TZ_Y + 47 + 26), "The time comes from the network; daylight saving time", 0, 0, W(.4f), 0);
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, TA_TZ_Y + 47 + 44), "follows the zone's own rules.", 0, 0, W(.4f), 0);
}
/* the list's regions by the IANA name's first part (the platform lists each region west to east) */
#define TZG_N_SH 5
static int ta_pre(const char *nm, const char *p) { while (*p && *p == *nm) p++, nm++; return !*p; }
static int ta_region(const char *nm)
{
    return ta_pre(nm, "America/") ? 0 : ta_pre(nm, "Africa/") ? 2 : ta_pre(nm, "Asia/") ? 3 :
           ta_pre(nm, "Pacific/") || ta_pre(nm, "Australia/") ? 4 : 1;            /* Europe/, Etc/UTC */
}
static int ta_zones(int g, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < sh_tz_count() && n < max; i++)
        if (ta_region(sh_tz_name(i, 0)) == g) out[n++] = i;
    return n;
}
static void ta_tz_open(void)                        /* the list at the zone in use */
{
    int z = sh_tz(), ix[96];
    TA.tzg = 1;
    TA.tztop = 0;
    for (int g = 0; g < TZG_N_SH; g++) {
        int n = ta_zones(g, ix, 96);
        for (int k = 0; k < n; k++)
            if (ix[k] == z) { TA.tzg = g; TA.tztop = k >= TA_ZN ? k - TA_ZN / 2 : 0; }
    }
}
static void ta_page_tz(dc *d)
{
    static const char *const GN[TZG_N_SH] = { "Americas", "Europe", "Africa", "Asia", "Pacific" };
    d_back(d, W(1));
    d_navtitle(d, "Time Zone");
    float sw = GROUP_W / TZG_N_SH;
    d_rrect(d, GROUP_X, TA_ZSEG_Y, GROUP_W, 32, 16, W(.12f));
    d_rrect(d, GROUP_X + 2 + TA.tzg * sw, TA_ZSEG_Y + 2, sw - 4, 28, 14, W(.26f));
    for (int g = 0; g < TZG_N_SH; g++) {
        float w = tw(TK_IT600, 12, GN[g], 0, 0);
        d_text(d, TK_IT600, 12, GROUP_X + sw * g + (sw - w) / 2, bl(TK_IT600, 12, TA_ZSEG_Y + 8.5f), GN[g], 0, 0, g == TA.tzg ? INK : W(.6f), 0);
    }
    int ix[96], n = ta_zones(TA.tzg, ix, 96), cur = sh_tz();
    int shown = n - TA.tztop < TA_ZN ? n - TA.tztop : TA_ZN;
    if (shown <= 0) return;
    d_rrect(d, GROUP_X, TA_ZTOP, GROUP_W, TA_ZROW * shown - 1, 16, tk_hex(0x151c33, 1));
    for (int r = 0; r < shown; r++) {
        int i = ix[TA.tztop + r];
        float y = TA_ZTOP + TA_ZROW * r;
        const char *city = "";
        (void)sh_tz_name(i, &city);
        char off[16];
        ta_off_text(off, sh_tz_offset_min(i));
        d_text_box(d, TK_IT400, 16, ROW_TX, bl(TK_IT400, 16, y + 12), 200, 0, city, 0, 0, i == cur ? ACCENT() : INK, 0);
        d_text_box(d, TK_IT400, 15, CONTENT_R - 44, bl(TK_IT400, 15, y + 13), 120, 2, off, 0, TK_TNUM, W(.55f), 0);
        if (i == cur) ta_check(d, CONTENT_R - 24, y + TA_ZROW / 2, ACCENT());
        if (r + 1 < shown) d_rrect(d, ROW_TX, y + TA_ZROW - 1, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
    }
    if (n > TA_ZN) {                                    /* where the list is: "8-21 of 21" under it */
        char b[32];
        int k = put_dec(b, TA.tztop + 1);
        b[k++] = '-';
        k += put_dec(b + k, TA.tztop + shown);
        for (const char *p = " of "; *p;) b[k++] = *p++;
        put_dec(b + k, n);
        float w = tw(TK_IT400, 12, b, 0, TK_TNUM);
        d_text(d, TK_IT400, 12, 195 - w / 2, bl(TK_IT400, 12, TA_ZTOP + TA_ZROW * shown + 6), b, 0, TK_TNUM, W(.4f), 0);
    }
}
static int ta_page(dc *d)
{
    if (AS.sub == TA_SUB_DT) { ta_page_dt(d); return 1; }
    if (AS.sub == TA_SUB_TZ) { ta_page_tz(d); return 1; }
    return 0;
}
static void ta_tz_scroll(int rows)
{
    int ix[96], n = ta_zones(TA.tzg, ix, 96), top = TA.tztop + rows, max = n > TA_ZN ? n - TA_ZN : 0;
    top = top < 0 ? 0 : top > max ? max : top;
    if (top != TA.tztop) { TA.tztop = top; build_app_page(AP.a); }
}
static int ta_tap(float x, float y)
{
    if (AS.sub == TA_SUB_DT) {
        if (BACK_HIT(x, y)) { AS.sub = 0; build_app_page(AP.a); return 1; }
        if (ta_in(x, y, GROUP_X, TA_DT_Y, GROUP_W, 47)) {
            sh_clock24_set(!sh_clock24());
            ta_restyle();
            build_app_page(AP.a);
        } else if (ta_in(x, y, GROUP_X, TA_TZ_Y, GROUP_W, 47)) {
            AS.sub = TA_SUB_TZ;
            ta_tz_open();
            build_app_page(AP.a);
        }
        return 1;
    }
    if (AS.sub != TA_SUB_TZ) return 0;
    if (BACK_HIT(x, y)) { AS.sub = TA_SUB_DT; build_app_page(AP.a); return 1; }
    if (ta_in(x, y, GROUP_X, TA_ZSEG_Y - 4, GROUP_W, 40)) {
        int g = (int)((x - GROUP_X) / (GROUP_W / TZG_N_SH));
        if (g >= 0 && g < TZG_N_SH && g != TA.tzg) { TA.tzg = g; TA.tztop = 0; build_app_page(AP.a); }
        return 1;
    }
    int ix[96], n = ta_zones(TA.tzg, ix, 96), r = (int)((y - TA_ZTOP) / TA_ZROW);
    if (x >= GROUP_X && x < CONTENT_R && y >= TA_ZTOP && r < TA_ZN && TA.tztop + r < n) {
        sh_tz_set(ix[TA.tztop + r]);
        AS.sub = TA_SUB_DT;                          /* chosen: back to Date & Time, as iOS */
        build_app_page(AP.a);
    }
    return 1;
}
/* the zone list's vertical swipe: the rows move by the finger's travel at the lift (a page rebuild per swipe, not
 * per frame); a touch that did not move is a tap */
static int ta_touch(int ph, float x, float y, float t)
{
    (void)t;
    if (ph == 0) {
        TA.own = AP.on && !AP.closing && !sp_app.live && is_app("settings") && AS.sub == TA_SUB_TZ && y >= TA_ZTOP && y < H_PT - 30;
        TA.x0 = x; TA.y0 = y; TA.moved = 0;
        return TA.own;
    }
    if (!TA.own) return 0;
    if (tk_fabsf(y - TA.y0) > 10 || tk_fabsf(x - TA.x0) > 10) TA.moved = 1;
    if (ph == 2) {
        TA.own = 0;
        if (!TA.moved) ta_tap(x, y);
        else {
            float dy = TA.y0 - y;
            ta_tz_scroll((int)(dy / TA_ZROW + (dy < 0 ? -.5f : .5f)));
        }
    }
    return 1;
}
/* the stills' states */
static int ta_set(const char *what, const char *key, float v)
{
    if (!tk_strcmp(what, "clock24")) { sh_clock24_set(v > 0); ta_restyle(); if (AP.on) build_app_page(AP.a); return 1; }
    if (!tk_strcmp(what, "tz")) {
        for (int i = 0; i < sh_tz_count(); i++) if (!tk_strcmp(sh_tz_name(i, 0), key)) sh_tz_set(i);
        if (AP.on) build_app_page(AP.a);
        return 1;
    }
    if (!tk_strcmp(what, "sub") && (!tk_strcmp(key, "datetime") || !tk_strcmp(key, "tz"))) {
        AS.sub = key[0] == 'd' ? TA_SUB_DT : TA_SUB_TZ;
        if (AS.sub == TA_SUB_TZ) { ta_tz_open(); if (v > 1) ta_tz_scroll((int)v); }
        if (AP.on) build_app_page(AP.a);
        return 1;
    }
    if (!tk_strcmp(what, "clocktab")) { TA.tab = !tk_strcmp(key, "alarms"); TA.edit = -1; if (AP.on) build_app_page(AP.a); return 1; }
    if (!tk_strcmp(what, "alarmedit")) { TA.tab = 1; ta_edit_begin((int)v); if (AP.on) build_app_page(AP.a); return 1; }
    return 0;
}
