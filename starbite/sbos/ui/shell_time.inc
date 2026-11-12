/* SPDX-License-Identifier: MIT
 * Track A alarms and time (2026-11-12), included by shell_lock.c after shell_settings.inc. On the platform's API
 * (shell.h: sh_tz_*, sh_clock24*, sh_alarm_*). P3x merge (Track B): the Clock's Alarms tab is B5's (shell_apps.inc,
 * on sh_alarm_count / get / put / del, using ta_days_text) and Settings > Date & Time is B2's (shell_settings.inc:
 * 24-Hour Time, Time Zone); what stays here:
 *   - the zone list (AS.sub TA_SUB_TZ: regions, a vertical swipe scrolls, the zone in use checked; a tap chooses it
 *     and goes back to Date & Time);
 *   - the stills' states: clock24=:0|1, tz=Europe/Paris, sub=datetime|tz.
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
    int tzg, tztop;                         /* the zone list: its region, its first row shown */
    int own;                                /* a touch on the zone list (its scroll) */
    float x0, y0;
    int moved;
} TA = { 0, 0, 0, 0, 0, 0 };

/* the time text was restyled (12 / 24-hour): every time on screen again (sh_status's rebuilds) */
static void ta_restyle(void)
{
    int h = ST.h, p = ST.pct;
    ST.h = -1;
    ST.pct = -1;
    sh_status(h, ST.mi, ST.wday, ST.day, ST.mon, p, ST.temp);
}

static void ta_check(dc *d, float cx, float cy, tk_col c)
{
    d_seg(d, cx - 7, cy + .5f, cx - 2, cy + 5.5f, 2.4f, c);
    d_seg(d, cx - 2, cy + 5.5f, cx + 7.5f, cy - 6, 2.4f, c);
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

/* ---- Settings > Date & Time and the zone list ---------------------------------------------------------------- */
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
    d_rrect(d, GROUP_X, TA_ZTOP, GROUP_W, TA_ZROW * shown - 1, 16, C_CARD);
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
    return 0;
}
