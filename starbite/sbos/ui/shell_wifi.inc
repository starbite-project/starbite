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
 * it over or restyle it without touching the rest. */

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

static void wf_caption(dc *d, float y, const char *t)
{
    d_text(d, TK_IT400, 13, 36, bl(TK_IT400, 13, y), t, .52f, 0, W(.55f), 0);
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

/* the list view's layout (drawing and taps share it) */
typedef struct { float sw, status, conn_cap, conn, result, net_cap, net, scan; int n, idx[8]; } wlay;
static void wf_lay(wlay *L)
{
    float y = 150;
    L->sw = y;
    y += 47;
    L->status = y + 8;
    y += 30;
    L->conn_cap = L->conn = L->result = -1;
    if (WF.state == SH_WIFI_CONNECTED) {
        L->conn_cap = y + 10;
        L->conn = y + 32;
        L->result = L->conn + 94 + 8;
        y = L->result + 38;
    }
    L->n = 0;
    for (int i = 0; i < WF.nnets && L->n < 8; i++) {
        if (WF.state == SH_WIFI_CONNECTED && !tk_strcmp(WF.nets[i].ssid, WF.ssid)) continue;
        L->idx[L->n++] = i;
    }
    L->net_cap = L->net = L->scan = -1;
    if (WF_ON()) {
        L->net_cap = y + 10;
        L->net = y + 32;
        L->scan = L->net + 47.0f * L->n;
    }
}

static void wf_list(dc *d)
{
    wlay L;
    wf_lay(&L);
    d_back(d, W(1));                       /* P3p (P3j-13): iOS 26's glass back and the inline title */
    d_navtitle(d, "Wi-Fi");
    /* the switch */
    d_rrect(d, 16, L.sw, 358, 47, 16, tk_hex(0x151c33, 1));
    d_text(d, TK_IT400, 16, 32, L.sw + 13 + 16, "Wi-Fi", 0, 0, INK, 0);
    int on = WF_ON();
    d_rrect(d, 307, L.sw + 8, 51, 31, 15.5f, on ? tk_hex(0x34c759, 1) : W(.16f));
    d_rrect(d, on ? 329 : 309, L.sw + 10, 27, 27, 13.5f, W(1));
    d_text(d, TK_IT400, 13, 36, bl(TK_IT400, 13, L.status), wf_state_text(), 0, 0,
           WF.state == SH_WIFI_FAILED ? tk_hex(0xff453a, 1) : W(.55f), 0);
    if (L.conn >= 0) {
        char nm[40];
        wf_caption(d, L.conn_cap, "CONNECTED");
        d_rrect(d, 16, L.conn, 358, 93, 16, tk_hex(0x151c33, 1));
        d_rrect(d, 34, L.conn + 19.5f, 9, 9, 4.5f, ACCENT());   /* the joined one (the fonts have no check mark) */
        wf_fit(nm, WF.ssid, 16, 190);
        d_text(d, TK_IT400, 16, 54, L.conn + 13 + 16, nm, 0, 0, INK, 0);
        float w = tw(TK_IT400, 15, WF.ip, 0, TK_TNUM);
        d_text(d, TK_IT400, 15, 358 - w, L.conn + 13 + 16, WF.ip, 0, TK_TNUM, W(.55f), 0);
        d_rrect(d, 32, L.conn + 46, 342, .5f, 0, W(.08f));
        d_text(d, TK_IT400, 16, 32, L.conn + 47 + 13 + 16,
               WF.fetch_state == 1 ? "Testing the Internet\xe2\x80\xa6" : "Test the Internet", 0, 0,
               WF.fetch_state == 1 ? W(.45f) : ACCENT(), 0);
        if (WF.fetch_state == 2) {
            char r[96];
            int n = 0;
            const char *a = WF.fetch_ok ? "Works: " : "Failed: ";
            while (*a) r[n++] = *a++;
            for (const char *b = WF.fetch_why; *b && n < 90; b++) r[n++] = *b;
            r[n] = 0;
            d_text(d, TK_IT600, 13, 36, bl(TK_IT600, 13, L.result), r, 0, 0,
                   WF.fetch_ok ? tk_hex(0x34c759, 1) : tk_hex(0xff453a, 1), 0);
            char s2[96];
            n = 0;
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
            d_text(d, TK_IT400, 13, 36, bl(TK_IT400, 13, L.result + 18), s2, 0, TK_TNUM, W(.5f), 0);
        }
    }
    if (L.net >= 0) {
        wf_caption(d, L.net_cap, "NETWORKS");
        d_rrect(d, 16, L.net, 358, 47.0f * (L.n + 1) - 1, 16, tk_hex(0x151c33, 1));
        for (int r = 0; r < L.n; r++) {
            const sh_wifi_net_t *N = &WF.nets[L.idx[r]];
            float ry = L.net + 47 * r;
            char nm[40];
            wf_fit(nm, N->ssid, 16, 230);
            d_text(d, TK_IT400, 16, 32, ry + 13 + 16, nm, 0, 0, INK, 0);
            if (N->band5) d_text(d, TK_IT600, 10, 32 + tw(TK_IT400, 16, nm, 0, 0) + 6, ry + 13 + 15, "5G", 0, 0, W(.4f), 0);
            if (N->secure) wf_lock(d, 318, ry + 17);
            wf_bars(d, 336, ry + 18, N->rssi);
            d_rrect(d, 32, ry + 46, 342, .5f, 0, W(.08f));
        }
        d_text(d, TK_IT400, 16, 32, L.scan + 13 + 16,
               WF.state == SH_WIFI_SCANNING ? "Looking\xe2\x80\xa6" : "Scan Again", 0, 0,
               WF.state == SH_WIFI_SCANNING ? W(.45f) : ACCENT(), 0);
    }
}

/* ---- the password view: a field and a keyboard with letters, numbers and symbols ------------------------------ */
static const char *const WK[3][3] = { { "qwertyuiop", "asdfghjkl", "zxcvbnm" }, { "1234567890", "-/:;()&@\"", ".,?!'" },
                                      { "[]{}#%^*+=", "_\\|~<>$`", ".,?!'" } };
static float wk_x0(int layer, int r)
{
    int n = (int)tk_strlen(WK[layer][r]);
    return r == 2 && !layer ? 61.5f : 3 + (10 - n) * 19.5f;
}
static void wf_kb(dc *d)
{
    d_rrect(d, 0, 540, 390, 262, 0, tk_hex(0x1e2030, 1));
    for (int r = 0; r < 3; r++) {
        const char *R = WK[WV.layer][r];
        int n = (int)tk_strlen(R);
        float x0 = wk_x0(WV.layer, r);
        for (int i = 0; i < n; i++) {
            char k[2] = { R[i], 0 };
            if (!WV.layer && WV.shift) k[0] = (char)(k[0] - 32);
            kb_key(d, x0 + 39 * i, 550 + 54 * r, 33, k, 0);
        }
    }
    const char *mode = WV.layer == 0 ? "shift" : WV.layer == 1 ? "#+=" : "123";
    float w = tw(TK_IT600, 13, mode, 0, 0);
    int lit = !WV.layer && WV.shift;
    d_rrect(d, 3, 658, 42, 46, 6, lit ? W(.9f) : tk_hex(0x3a3d4a, 1));
    d_text(d, TK_IT600, 13, 3 + (42 - w) / 2, bl(TK_IT600, 13, 672), mode, 0, 0, lit ? tk_hex(0x1e2030, 1) : INK, 0);
    d_rrect(d, 345, 658, 42, 46, 6, tk_hex(0x3a3d4a, 1));
    d_icon(d, "back", 359, 670, 1);
    kb_key(d, 3, 712, 88, WV.layer ? "ABC" : "123", 1);
    kb_key(d, 97, 712, 196, "space", 1);
    kb_key(d, 299, 712, 88, "join", 1);
}
/* 1 a character (*c), 2 the mode key, 3 ABC/123, 4 delete, 5 join, 0 nothing */
static int wf_kb_hit(float x, float y, char *c)
{
    for (int r = 0; r < 3; r++) {
        const char *R = WK[WV.layer][r];
        int n = (int)tk_strlen(R);
        float x0 = wk_x0(WV.layer, r), ky = 550 + 54 * r;
        if (y < ky || y >= ky + 50) continue;
        for (int i = 0; i < n; i++)
            if (x >= x0 + 39 * i - 3 && x < x0 + 39 * i + 36) {
                *c = R[i];
                if (!WV.layer && WV.shift) *c = (char)(*c - 32);
                return 1;
            }
        if (r == 2 && x < 55) return 2;
        if (r == 2 && x >= 340) return 4;
    }
    if (y >= 712 && y < 760) {
        if (x < 94) return 3;
        if (x < 296) { *c = ' '; return 1; }
        return 5;
    }
    return 0;
}
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
    d_text(d, TK_IT600, 17, CONTENT_R - w, NAV_BASE, "Join", 0, 0, ok ? ACCENT() : W(.3f), 0);
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
    d_text(d, TK_IT400, 15, (390 - w) / 2, bl(TK_IT400, 15, 92), line, 0, 0, W(.6f), 0);
    d_rrect(d, 16, 130, 358, 47, 16, tk_hex(0x151c33, 1));
    d_text(d, TK_IT400, 16, 32, 130 + 13 + 16, "Password", 0, 0, INK, 0);
    /* the value: dots, or the text with Show; the end of a long one */
    char v[70];
    unsigned k = 0;
    for (unsigned i = 0; i < WV.plen && k < 66; i++) {
        if (WV.show) v[k++] = WV.pass[i];
        else { v[k++] = (char)0xe2; v[k++] = (char)0x80; v[k++] = (char)0xa2; }
    }
    v[k] = 0;
    const char *vs = v;
    while (*vs && tw(TK_IT400, 16, vs, 0, 0) > 170) {
        vs++;
        while ((*vs & 0xc0) == 0x80) vs++;
    }
    d_text(d, TK_IT400, 16, 120, 130 + 13 + 16, vs, 0, 0, INK, 0);
    d_rrect(d, 121 + tw(TK_IT400, 16, vs, 0, 0), 130 + 13, 2, 21, 1, ACCENT());
    const char *sh = WV.show ? "Hide" : "Show";
    w = tw(TK_IT400, 15, sh, 0, 0);
    d_text(d, TK_IT400, 15, 358 - w, 130 + 13 + 16, sh, 0, 0, ACCENT(), 0);
    d_text(d, TK_IT400, 13, 36, bl(TK_IT400, 13, 186),
           WV.plen && !ok ? "8 to 63 characters (or the 64-digit key)" : "WPA2 \xc2\xb7 kept in memory until a restart, never stored",
           0, 0, W(.5f), 0);
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

static void wifi_tap(float x, float y)
{
    if (WV.view == 1) {
        char c = 0;
        if (y >= 40 && y < 86 && x < 110) { wf_wipe(); WV.view = 0; WV.show = 0; build_app_page(AP.a); return; }
        if (y >= 40 && y < 86 && x >= 300) { wf_join(); return; }
        if (y >= 130 && y < 177 && x >= 300) { WV.show = !WV.show; build_app_page(AP.a); return; }
        int k = wf_kb_hit(x, y, &c);
        if (!k) return;
        sh_haptic();
        if (k == 1 && WV.plen < 64) {
            WV.pass[WV.plen++] = c;
            WV.pass[WV.plen] = 0;
            if (!WV.layer && WV.shift) WV.shift = 0;
        } else if (k == 2) {
            if (!WV.layer) WV.shift = !WV.shift;
            else WV.layer = WV.layer == 1 ? 2 : 1;
        } else if (k == 3) {
            WV.layer = WV.layer ? 0 : 1;
        } else if (k == 4 && WV.plen) {
            WV.pass[--WV.plen] = 0;
        } else if (k == 5) {
            wf_join();
            return;
        }
        build_app_page(AP.a);
        return;
    }
    wlay L;
    wf_lay(&L);
    if (x < 120 && y >= 40 && y < 86) { AS.sub = 0; build_app_page(AP.a); return; }
    if (y >= L.sw && y < L.sw + 47 && x >= 16 && x < 374) {
        int on = !WF_ON();
        if (sh_wifi_power(on) < 0) {
            const char *y = sh_wifi_why();      /* P3n: the kernel's reason (it said "Heat?" whatever it was) */
            toast(WF_ON() ? "Wi-Fi can't stop now" : y && y[0] ? y : "Wi-Fi can't start now");
        } else sh_pref_set_int("cc.wifi", on);  /* Track A: the user's switch, kept (sh_restore) */
        return;
    }
    if (L.conn >= 0 && y >= L.conn + 47 && y < L.conn + 94 && x >= 16 && x < 374) {
        if (WF.fetch_state == 1) return;
        if (sh_wifi_fetch(TEST_URL) < 0) toast("Can't test now");
        return;
    }
    if (L.net >= 0 && y >= L.net && x >= 16 && x < 374) {
        int r = (int)((y - L.net) / 47);
        if (r < L.n) {
            const sh_wifi_net_t *N = &WF.nets[L.idx[r]];
            if (WF.state == SH_WIFI_JOINING || WF.state == SH_WIFI_DHCP) { toast("Joining another network\xe2\x80\xa6"); return; }
            tk_memcpy(WV.ssid, N->ssid, sizeof(WV.ssid));
            if (N->secure && !N->saved) {
                wf_wipe();
                WV.view = 1;
                WV.shift = 0;
                WV.layer = 0;
                build_app_page(AP.a);
                return;
            }
            toast(sh_wifi_join(N->ssid, 0) < 0 ? "Can't join now" : "Joining\xe2\x80\xa6");
            return;
        }
        if (r == L.n && WF.state != SH_WIFI_SCANNING) {
            if (sh_wifi_scan() < 0) toast("Can't scan now");
        }
    }
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
