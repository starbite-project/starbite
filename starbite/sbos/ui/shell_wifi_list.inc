/* SPDX-License-Identifier: MIT
 * Settings > Wi-Fi's scrolling list (8b): shell_wifi.inc's page under the nav row as the shared list's items
 * (shell_apps.inc's SCV). Included by shell_lock.c after shell_apps.inc. */

/* 8b (gap list B8: the page didn't scroll; it showed the networks that fit, 8 at most): the page under the nav row
 * as the shared list's items (shell_apps.inc's SCV: each drawn when it comes into view, iOS's scrolling): 0 the
 * cards above the networks (or the whole page while Wi-Fi is off), then a row per network, then "Scan Again" */
static uint32_t wf_hash(uint32_t h, const char *s, uint32_t v)
{
    if (!h) h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return ((h ^ v) * 16777619u) | 1;
}
static wlay WFL;
static void wf_item(dc *d, int i, float top)
{
    (void)top;
    if (i == 0) { wf_list_body(d); return; }
    if (WFL.net < 0) return;
    d_group(d, WFL.net, LG_ROW * (WFL.n + 1));      /* the card's part in this item (the bitmap clips it) */
    if (i <= WFL.n) { wf_net_row(d, WFL.net + LG_ROW * (i - 1), &WF.nets[WFL.idx[i - 1]], 0, 0, wf_busy()); return; }
    int sc = WF.state == SH_WIFI_SCANNING;
    d_row(d, WFL.scan, LG_ROW, 0, sc ? "Scanning\xe2\x80\xa6" : "Scan Again", 0, 0, 1, sc ? C_LABEL3 : KIT_BLUE(), 0);
}
static void wf_list_build(void)
{
    wfl_on = is_app("settings") && AS.sub == 2 && !WV.view;
    if (!wfl_on) return;
    wf_lay(&WFL);
    ls_used = 1;
    LS.draw = wf_item;
    LS.fade_t = 16; LS.fade_b = 16;
    LS.top = LIST_T0; LS.bot = H_PT;
    int n = 0;
    float head = (WFL.net >= 0 ? WFL.net : WFL.end) - LIST_T0;
    LS.iy[n] = 0; LS.ih[n] = head; LS.ik[n] = wf_hash(0, WF.ssid, wf_gen * 7 + (uint32_t)WF.state); n++;
    if (WFL.net >= 0) {
        for (int r = 0; r < WFL.n && n < SCV_MAXI - 2; r++, n++) {
            const sh_wifi_net_t *N = &WF.nets[WFL.idx[r]];
            LS.iy[n] = WFL.net + LG_ROW * r - LIST_T0; LS.ih[n] = LG_ROW;
            LS.ik[n] = wf_hash(0, N->ssid, (uint32_t)(N->rssi / 8 + 64) | (uint32_t)N->secure << 8 | (uint32_t)N->band5 << 9 |
                               (uint32_t)wf_busy() << 10 | (uint32_t)r << 12 | (uint32_t)WFL.n << 20);
        }
        LS.iy[n] = WFL.scan - LIST_T0; LS.ih[n] = LG_ROW + 40;           /* "Scan Again", room over the home bar */
        LS.ik[n] = wf_hash(0, "scan", (uint32_t)(WF.state == SH_WIFI_SCANNING) | (uint32_t)WFL.n << 4); n++;
    }
    scv_layout(&LS, n);
    scv_place(&LS);
}
static float wf_y(float y) { return wfl_on && LS.on && y >= LS.top ? y + LS.s : y; }
static float wf_ls_s(void) { return wfl_on && LS.on ? LS.s : 0; }
