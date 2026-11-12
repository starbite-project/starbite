/* SPDX-License-Identifier: MIT
 * Settings > Wi-Fi for Starbite OS Next (wifi-sbos-next), included by shell_lock.c after sh_cell (it uses the shell's
 * drawing helpers, layers and app state). The kernel pushes Wi-Fi's state with sh_wifi(); the page shows it:
 *   - the switch, the state in words;
 *   - when joined: the network, its address, "Test the Internet" (an HTTPS GET of TEST_URL through the phone's own
 *     TCP/TLS stack) and its result;
 *   - the networks heard (strongest first, a lock on secured ones, three bars), "Scan Again";
 *   - a secured network's password: a field over iOS's dark keyboard with a third layer (#+=) so every printable
 *     character of a WPA2 passphrase can be typed; Show / Hide; Join from the key or the top bar.
 * The passphrase lives in WV.pass only until the join call (then it is wiped); the kernel keeps it in RAM for
 * reconnecting until a restart. Nothing here logs. Kept in one file so the toolkit's own Settings work can take
 * it over or restyle it without touching the rest.
 * cp6 (Track B): the list on the kit's cards (shell_kit.inc), a spinner where it waits (shell_settings.inc's strip
 * spinner), a press on every row and the switch, a toast when a busy row is tapped; and the press colours every
 * Settings page uses (SEP_*, sep_row / sep_switch / sep_word below). */

#define TEST_URL "https://example.com/"

static sh_wifi_t WF;
static struct { int view, shift, layer, show; unsigned plen; char pass[65]; char ssid[33]; } WV;
#define WF_ON() (WF.state != SH_WIFI_OFF && WF.state != SH_WIFI_FAILED)

static void wf_wipe(void)
{
    volatile char *p = WV.pass;
    for (unsigned i = 0; i < sizeof(WV.pass); i++) p[i] = 0;
    WV.plen = 0;
}

static const char *wf_state_text(void)
{
    switch (WF.state) {
    case SH_WIFI_OFF: return WF.why[0] ? WF.why : "Off";
    case SH_WIFI_STARTING: return "Starting the radio\xe2\x80\xa6";
    case SH_WIFI_UP: return WF.why[0] ? WF.why : WF.nnets ? "Not connected" : "No networks heard";
    case SH_WIFI_SCANNING: return "Looking for networks\xe2\x80\xa6";
    case SH_WIFI_JOINING: return "Joining\xe2\x80\xa6";
    case SH_WIFI_DHCP: return "Getting an address\xe2\x80\xa6";
    case SH_WIFI_CONNECTED: return "Connected";
    default: return WF.why[0] ? WF.why : "The radio did not start";
    }
}

/* Settings' own row */
static const char *wf_row_text(void)
{
    if (WF.state == SH_WIFI_CONNECTED) return WF.ssid;
    if (!WF_ON()) return "Off";
    return WF.state == SH_WIFI_JOINING || WF.state == SH_WIFI_DHCP ? "Joining\xe2\x80\xa6" : "Not connected";
}

/* a name cut to fit w pt */
static void wf_fit(char *out, const char *s, float size, float w)
{
    int n = 0;
    while (s[n] && n < 32) { out[n] = s[n]; n++; }
    out[n] = 0;
    if (tw(TK_IT400, size, out, 0, 0) <= w) return;
    while (n > 0) {
        n--;
        out[n] = 0;
        char t[40];
        tk_memcpy(t, out, (size_t)n);
        tk_memcpy(t + n, "\xe2\x80\xa6", 4);
        if (tw(TK_IT400, size, t, 0, 0) <= w) { tk_memcpy(out, t, (size_t)n + 4); return; }
    }
}


static void wf_bars(dc *d, float x, float y, int rssi)
{
    int n = rssi >= -60 ? 3 : rssi >= -72 ? 2 : 1;
    for (int i = 0; i < 3; i++) d_rrect(d, x + 5 * i, y + 9 - 3 * i, 3.5f, 4 + 3 * i, 1, W(i < n ? .9f : .25f));
}

static void wf_lock(dc *d, float x, float y)
{
    d_rrect(d, x + 2, y, 6, 1.6f, .8f, W(.6f));
    d_rrect(d, x + 2, y, 1.6f, 6, .8f, W(.6f));
    d_rrect(d, x + 6.4f, y, 1.6f, 6, .8f, W(.6f));
    d_rrect(d, x, y + 5, 10, 8, 2, W(.6f));
}

/* ---- cp6: what a touch shows on Settings' pages (Track B's press language until the core engine's kinds land) ------
 * Rows on the #1C1C1E cards light to #3A3A3C (W .135 over the card: the words stay on top), clipped to their card;
 * a switch's track darkens under the finger (iOS); a segmented control's pressed segment lights; text buttons in the
 * nav row light a capsule around their word. Every one is a solid rect (hl_set): nothing is drawn in a touch.
 * Used by shell_cell.inc, shell_settings.inc, shell_startup.inc, shell_update.inc, shell_time.inc and the passcode page. */
#define SEP_ROW W(.135f)                    /* #3A3A3C over the card */
#define SEP_SW TK_RGBA(0, 0, 0, .28f)        /* a switch's track, darker */
#define SEP_SEG W(.12f)                     /* a segment */
#define SEP_TXT W(.12f)                     /* a text button's capsule */
#define SEP_FILL TK_RGBA(0, 0, 0, .22f)      /* a filled capsule (Update's), darker */
/* a row (ry, rh) of the card (gy, gh) lit: the press protocol's rect, clip, radius and colour */
static int sep_row(tk_rect *r, tk_rect *g, float *rad, tk_col *c, float ry, float rh, float gy, float gh)
{
    *r = (tk_rect){ LG_X, ry, LG_W, rh };
    *g = (tk_rect){ LG_X, gy, LG_W, gh };
    *rad = LG_RAD;
    *c = SEP_ROW;
    return 1;
}
/* the kit's switch at (sx, sy): its track darkens */
static int sep_switch(tk_rect *r, tk_rect *g, float *rad, tk_col *c, float sx, float sy)
{
    *r = *g = (tk_rect){ sx, sy, KSW_W, KSW_H };
    *rad = KSW_H / 2;
    *c = SEP_SW;
    return 1;
}
/* a word in the nav row (Cancel, Join): a capsule around it */
static int sep_word(tk_rect *r, tk_rect *g, float *rad, tk_col *c, float x, float w)
{
    *r = *g = (tk_rect){ x - 10, SAFE_TOP + 6, w + 20, 32 };
    *rad = 16;
    *c = SEP_TXT;
    return 1;
}

/* the list view's layout (drawing, taps and presses share it): iOS 26's Wi-Fi on the kit's cards (shell_kit.inc's
 * d_group / d_row, 52 pt rows, the kit's switch): the switch and the joined network in the first card, the state in
 * words under it; "Test the Internet" in its own card with the result under it; "Networks" (a spinner beside it while
 * scanning) with the networks heard and "Scan Again" */
#define WF_Y0 (PUSHED_TOP - 8)
#define WF_SWX (LG_RE - 16 - KSW_W)
typedef struct { float sw, conn, foot, test, tfoot, net, scan; int n, idx[8]; } wlay;
static int wf_busy(void) { return WF.state == SH_WIFI_JOINING || WF.state == SH_WIFI_DHCP; }
static float wf_foot_h(const char *t) { return t && t[0] ? d_group_foot(0, 0, t) : 0; }
static const char *wf_status(void) { return WF.state == SH_WIFI_CONNECTED ? 0 : wf_state_text(); }
static void wf_lay(wlay *L)
{
    float y = WF_Y0;
    L->sw = y;
    y += LG_ROW;
    L->conn = L->test = L->tfoot = L->net = L->scan = -1;
    if (WF.state == SH_WIFI_CONNECTED) { L->conn = y; y += LG_ROW; }
    L->foot = y;
    y += wf_foot_h(wf_status());
    int foot = wf_status() != 0;
    if (L->conn >= 0) {
        y += LG_GAP;
        L->test = y;
        y += LG_ROW;
        L->tfoot = y;
        y += WF.fetch_state == 2 ? 8 + 18 * 3 : 0;
        foot = WF.fetch_state == 2;
    }
    L->n = 0;
    if (!WF_ON()) return;
    y += LG_GAP + (foot ? 12 : 0);
    L->net = y;
    int room = (int)((H_PT - 40 - y) / LG_ROW) - 1;      /* the rows that fit above the home bar ("Scan Again" last) */
    for (int i = 0; i < WF.nnets && L->n < 8 && L->n < room; i++) {
        if (WF.state == SH_WIFI_CONNECTED && !tk_strcmp(WF.nets[i].ssid, WF.ssid)) continue;
        L->idx[L->n++] = i;
    }
    L->scan = L->net + LG_ROW * L->n;
}

/* a network's row: the name (and 5G), the lock and the bars at the right; dim while another one joins */
static void wf_net_row(dc *d, float y, const sh_wifi_net_t *N, int last, int check, int dim)
{
    char nm[40];
    float a = dim ? .3f : 1, tx = check ? LG_TX + 26 : LG_TX;
    wf_fit(nm, N->ssid, 17, check ? 170 : 220);
    tk_col ink = W(a);
    d_text(d, TK_IT400, 17, tx, y + LG_ROW / 2 + 6, nm, 0, 0, ink, 0);
    if (N->band5) d_text(d, TK_IT600, 10, tx + tw(TK_IT400, 17, nm, 0, 0) + 6, y + LG_ROW / 2 + 5, "5G", 0, 0, check ? C_LABEL2 : W(.55f * a), 0);
    if (check) {                                       /* iOS's check mark (accent) before the joined one */
        float cx = LG_TX + 7, cy = y + LG_ROW / 2;
        d_seg(d, cx - 6, cy + .5f, cx - 1.5f, cy + 5, 2.2f, KIT_BLUE());
        d_seg(d, cx - 1.5f, cy + 5, cx + 7, cy - 6, 2.2f, KIT_BLUE());
    }
    if (N->secure) wf_lock(d, LG_RE - 16 - 38, y + LG_ROW / 2 - 7);
    wf_bars(d, LG_RE - 16 - 18, y + LG_ROW / 2 - 7, N->rssi);
    if (!last) d_rrect(d, LG_TX, y + LG_ROW - .5f, LG_RE - LG_TX, .5f, 0, C_SEP);
}
/* a footer line of a colour (the kit's footers are grey) */
static void wf_foot_line(dc *d, float y, const char *t, int f, tk_col c)
{
    d_text_box(d, f, 13, LG_TX, bl(f, 13, y), LG_W - 32, 0, t, 0, TK_TNUM, c, 0);
}
static void wf_list(dc *d)
{
    wlay L;
    wf_lay(&L);
    d_back(d, W(1));                       /* P3p (P3j-13): iOS 26's glass back and the inline title */
    d_navtitle(d, "Wi-Fi");
    int on = WF_ON();
    d_group(d, L.sw, L.conn >= 0 ? 2 * LG_ROW : LG_ROW);
    d_row(d, L.sw, LG_ROW, 0, "Wi-Fi", 0, 0, L.conn < 0, INK, WF_SWX - 12);
    kit_d_switch(d, WF_SWX, L.sw + (LG_ROW - KSW_H) / 2, on, 1);
    if (L.conn >= 0) {
        sh_wifi_net_t me = { "", -50, 0, 1, 0 };
        tk_memcpy(me.ssid, WF.ssid, sizeof me.ssid);
        for (int i = 0; i < WF.nnets; i++) if (!tk_strcmp(WF.nets[i].ssid, WF.ssid)) me = WF.nets[i];
        wf_net_row(d, L.conn, &me, 1, 1, 0);
    }
    const char *st = wf_status();
    if (st) {
        if (WF.state == SH_WIFI_FAILED) wf_foot_line(d, L.foot + 8, st, TK_IT400, C_RED);
        else d_group_foot(d, L.foot, st);
    }
    if (L.test >= 0) {
        int busy = WF.fetch_state == 1;
        d_group(d, L.test, LG_ROW);
        d_row(d, L.test, LG_ROW, 0, busy ? "Testing the Internet\xe2\x80\xa6" : "Test the Internet", WF.ip, 0, 1,
              busy ? C_LABEL3 : KIT_BLUE(), 0);
        if (WF.fetch_state == 2) {
            /* the verdict in colour, the kernel's words, the numbers (each line cut to the card's width) */
            char s2[96];
            int n = 0;
            wf_foot_line(d, L.tfoot + 8, WF.fetch_ok ? "The Internet works" : "The test failed", TK_IT600, WF.fetch_ok ? C_GREEN : C_RED);
            wf_foot_line(d, L.tfoot + 26, WF.fetch_why, TK_IT400, C_LABEL2);
            char hb[40];
            wf_fit(hb, WF.fetch_host, 13, 120);
            for (const char *b = hb; *b; b++) s2[n++] = *b;
            if (WF.fetch_ok) {                 /* "example.com · 200 · 1256 bytes · 520 ms (TLS 380)" */
                const char *m = " \xc2\xb7 ";
                while (*m) s2[n++] = *m++;
                n += put_dec(s2 + n, WF.fetch_status);
                m = " \xc2\xb7 ";
                while (*m) s2[n++] = *m++;
                n += put_dec(s2 + n, (int)WF.fetch_bytes);
                m = " bytes \xc2\xb7 ";
                while (*m) s2[n++] = *m++;
                n += put_dec(s2 + n, (int)WF.fetch_ms);
                m = " ms (TLS ";
                while (*m) s2[n++] = *m++;
                n += put_dec(s2 + n, (int)WF.fetch_tls_ms);
                s2[n++] = ')';
            }
            s2[n] = 0;
            wf_foot_line(d, L.tfoot + 44, s2, TK_IT400, C_LABEL2);
        }
    }
    if (L.net >= 0) {
        d_group_head(d, L.net, "Networks");
        d_group(d, L.net, LG_ROW * (L.n + 1));
        for (int r = 0; r < L.n; r++) wf_net_row(d, L.net + LG_ROW * r, &WF.nets[L.idx[r]], 0, 0, wf_busy());
        int sc = WF.state == SH_WIFI_SCANNING;
        d_row(d, L.scan, LG_ROW, 0, sc ? "Scanning\xe2\x80\xa6" : "Scan Again", 0, 0, 1, sc ? C_LABEL3 : KIT_BLUE(), 0);
    }
}
/* the spinner's place (shell_settings.inc's strip spinner): beside the switch while the radio starts, beside
 * "Networks" while it scans, at the joining network's right while it joins */
static int wf_spin_at(float *cx, float *cy)
{
    if (WV.view) return 0;
    wlay L;
    wf_lay(&L);
    if (WF.state == SH_WIFI_STARTING) { *cx = WF_SWX - 22; *cy = L.sw + LG_ROW / 2; return 1; }
    if (WF.state == SH_WIFI_SCANNING && L.net >= 0) {
        *cx = LG_TX + tw(TK_IT600, 15, "Networks", 0, 0) + 18;
        *cy = L.net - 18;
        return 1;
    }
    if (wf_busy() && L.net >= 0) {
        for (int r = 0; r < L.n; r++)
            if (!tk_strcmp(WF.nets[L.idx[r]].ssid, WV.ssid)) { *cx = LG_RE - 16 - 62; *cy = L.net + LG_ROW * r + LG_ROW / 2; return 1; }
        *cx = WF_SWX - 22; *cy = L.sw + LG_ROW / 2;
        return 1;
    }
    return 0;
}

/* ---- the password view: a field over the shell keyboard (B3: shell_kb.inc; ASCII only, return is "join") ---- */
#define WF_PW_Y 130.0f                      /* the password's card */
static void wf_kb(dc *d) { kb_draw(d, 0, 0); }
static int wf_pass_ok(void)
{
    if (WV.plen == 64) {
        for (unsigned i = 0; i < 64; i++) {
            char c = WV.pass[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
        }
        return 1;
    }
    return WV.plen >= 8 && WV.plen <= 63;
}
static void wf_pass(dc *d)
{
    int ok = wf_pass_ok();
    d_text(d, TK_IT400, 17, MARGIN, NAV_BASE, "Cancel", 0, 0, ACCENT(), 0);     /* P3p: in the nav row */
    float w = tw(TK_IT600, 17, "Enter Password", 0, 0);
    d_text(d, TK_IT600, 17, (390 - w) / 2, NAV_BASE, "Enter Password", 0, 0, INK, 0);
    w = tw(TK_IT600, 17, "Join", 0, 0);
    d_text(d, TK_IT600, 17, CONTENT_R - w, NAV_BASE, "Join", 0, 0, ok ? ACCENT() : C_LABEL3, 0);
    char nm[40], line[64];
    wf_fit(nm, WV.ssid, 15, 300);
    int n = 0;
    const char *a = "for \xe2\x80\x9c";
    while (*a) line[n++] = *a++;
    for (const char *b = nm; *b; b++) line[n++] = *b;
    a = "\xe2\x80\x9d";
    while (*a) line[n++] = *a++;
    line[n] = 0;
    w = tw(TK_IT400, 15, line, 0, 0);
    d_text(d, TK_IT400, 15, (390 - w) / 2, bl(TK_IT400, 15, 92), line, 0, 0, C_LABEL2, 0);
    d_group(d, WF_PW_Y, LG_ROW);                  /* cp6: the kit's card (shell_kit.inc) */
    d_text(d, TK_IT400, 17, LG_TX, WF_PW_Y + LG_ROW / 2 + 6, "Password", 0, 0, INK, 0);
    /* the value: dots, or the text with Show; the end of a long one */
    char v[70];
    unsigned k = 0;
    for (unsigned i = 0; i < WV.plen && k < 66; i++) {
        if (WV.show) v[k++] = WV.pass[i];
        else { v[k++] = (char)0xe2; v[k++] = (char)0x80; v[k++] = (char)0xa2; }
    }
    v[k] = 0;
    const char *vs = v;
    while (*vs && tw(TK_IT400, 17, vs, 0, 0) > 170) {
        vs++;
        while ((*vs & 0xc0) == 0x80) vs++;
    }
    d_text(d, TK_IT400, 17, 120, WF_PW_Y + LG_ROW / 2 + 6, vs, 0, 0, INK, 0);
    d_rrect(d, 121 + tw(TK_IT400, 17, vs, 0, 0), WF_PW_Y + 15, 2, 22, 1, ACCENT());
    const char *sh = WV.show ? "Hide" : "Show";
    w = tw(TK_IT400, 17, sh, 0, 0);
    d_text(d, TK_IT400, 17, LG_RE - 16 - w, WF_PW_Y + LG_ROW / 2 + 6, sh, 0, 0, ACCENT(), 0);
    if (WV.plen && !ok) wf_foot_line(d, WF_PW_Y + LG_ROW + 8, "8 to 63 characters (or the 64-digit key)", TK_IT400, C_RED);
    else d_group_foot(d, WF_PW_Y + LG_ROW, "WPA2 \xc2\xb7 kept in memory until a restart, never stored.");
    wf_kb(d);
}

static void page_wifi(dc *d)
{
    if (WV.view == 1) wf_pass(d);
    else wf_list(d);
}

static void wf_join(void)
{
    if (!wf_pass_ok()) { toast("8 to 63 characters (or the 64-digit key)"); return; }
    int r = sh_wifi_join(WV.ssid, WV.pass);
    wf_wipe();
    WV.view = 0;
    WV.show = 0;
    toast(r < 0 ? "Can't join now" : "Joining\xe2\x80\xa6");
    build_app_page(AP.a);
}

#define WF_CANCEL_W (tw(TK_IT400, 17, "Cancel", 0, 0))
#define WF_JOIN_W (tw(TK_IT600, 17, "Join", 0, 0))
static void wifi_tap(float x, float y)
{
    if (WV.view == 1) {
        if (y >= 40 && y < 86 && x < 110) { wf_wipe(); WV.view = 0; WV.show = 0; build_app_page(AP.a); return; }
        if (y >= 40 && y < 86 && x >= 300) { wf_join(); return; }
        if (y >= WF_PW_Y && y < WF_PW_Y + LG_ROW && x >= 300) { WV.show = !WV.show; build_app_page(AP.a); return; }
        if (y < KB_TOP) return;                 /* B3: the keyboard's key; only ASCII reaches here (KB_ASCII) */
        int k = kb_take(0);
        if (k == KBE_RET) { wf_join(); return; }
        if (!kb_edit(WV.pass, 64)) return;
        WV.plen = (unsigned)tk_strlen(WV.pass);
        build_app_page(AP.a);
        return;
    }
    wlay L;
    wf_lay(&L);
    if (BACK_HIT(x, y)) { AS.sub = 0; build_app_page(AP.a); return; }
    if (x < LG_X || x >= LG_RE) return;
    if (y >= L.sw && y < L.sw + LG_ROW) {
        int on = !WF_ON();
        if (sh_wifi_power(on) < 0) {
            const char *y = sh_wifi_why();      /* P3n: the kernel's reason (it said "Heat?" whatever it was) */
            toast(WF_ON() ? "Wi-Fi can't stop now" : y && y[0] ? y : "Wi-Fi can't start now");
        } else sh_pref_set_int("cc.wifi", on);  /* Track A: the user's switch, kept (sh_restore) */
        return;
    }
    if (L.test >= 0 && y >= L.test && y < L.test + LG_ROW) {
        if (WF.fetch_state == 1) { toast("Already testing the Internet\xe2\x80\xa6"); return; }
        if (sh_wifi_fetch(TEST_URL) < 0) toast("Can't test now: Wi-Fi isn't ready");
        return;
    }
    if (L.net >= 0 && y >= L.net && y < L.scan + LG_ROW) {
        int r = (int)((y - L.net) / LG_ROW);
        if (r < L.n) {
            const sh_wifi_net_t *N = &WF.nets[L.idx[r]];
            if (wf_busy()) { toast("Joining another network\xe2\x80\xa6 Wait for it to finish."); return; }
            tk_memcpy(WV.ssid, N->ssid, sizeof(WV.ssid));
            if (N->secure && !N->saved) {
                wf_wipe();
                WV.view = 1;
                WV.shift = 0;
                WV.layer = 0;
                kb_focus(KB_ASCII, "join", "");      /* B3: the shell keyboard, ASCII only */
                build_app_page(AP.a);
                return;
            }
            toast(sh_wifi_join(N->ssid, 0) < 0 ? "Can't join now" : "Joining\xe2\x80\xa6");
            return;
        }
        if (WF.state == SH_WIFI_SCANNING) { toast("Already looking for networks\xe2\x80\xa6"); return; }
        if (sh_wifi_scan() < 0) toast("Can't scan now: Wi-Fi isn't ready");
    }
}
/* cp6: what lights under the finger (shell_settings.inc's se_strip_press asks): the switch's track, the test and
 * network rows and Scan Again unless they are busy (then a tap says why); the password view's Cancel / Join / Show */
static int wf_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    if (WV.view == 1) {
        if (y >= 40 && y < 86 && x < 110) return sep_word(r, g, rad, c, MARGIN, WF_CANCEL_W);
        if (y >= 40 && y < 86 && x >= 300 && wf_pass_ok()) return sep_word(r, g, rad, c, CONTENT_R - WF_JOIN_W, WF_JOIN_W);
        if (y >= WF_PW_Y && y < WF_PW_Y + LG_ROW && x >= 300) {
            float w = tw(TK_IT400, 17, WV.show ? "Hide" : "Show", 0, 0);
            *r = *g = (tk_rect){ LG_RE - 16 - w - 8, WF_PW_Y + 10, w + 16, 32 };
            *rad = 16; *c = SEP_TXT;
            return 1;
        }
        return 0;
    }
    if (x < LG_X || x >= LG_RE) return 0;
    wlay L;
    wf_lay(&L);
    if (y >= L.sw && y < L.sw + LG_ROW) return sep_switch(r, g, rad, c, WF_SWX, L.sw + (LG_ROW - KSW_H) / 2);
    if (L.test >= 0 && y >= L.test && y < L.test + LG_ROW && WF.fetch_state != 1) return sep_row(r, g, rad, c, L.test, LG_ROW, L.test, LG_ROW);
    if (L.net >= 0 && y >= L.net && y < L.scan + LG_ROW) {
        int k = (int)((y - L.net) / LG_ROW);
        if ((k < L.n && wf_busy()) || (k == L.n && WF.state == SH_WIFI_SCANNING)) return 0;
        return sep_row(r, g, rad, c, L.net + LG_ROW * k, LG_ROW, L.net, LG_ROW * (L.n + 1));
    }
    return 0;
}

void sh_wifi(const sh_wifi_t *w)
{
    int was_on = WF_ON(), was_bar = SB_WIFI;
    WF = *w;
    SB_WIFI = WF.state == SH_WIFI_CONNECTED;
    CCS.wifi = WF_ON();
    if (SB_WIFI != was_bar) {
        build_lstatus();
        build_home_sb();
        if (AP.on) build_apsb();
        lock_ver++;
    }
    if (CCS.built && WF_ON() != was_on) { relayer(&ccsb); build_cc_grid(); cc_link(); }
    if (AP.on && (is_app("settings") || is_app("maps"))) build_app_page(AP.a);   /* helper D: Maps shows it live */
    relink();
}
