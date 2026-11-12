/* SPDX-License-Identifier: MIT
 * Settings > Cellular for Starbite OS Next (mobile data, docs/MODEM.md 7.10), included by shell_lock.c after
 * shell_wifi.inc (it uses the shell's drawing helpers, layers and app state). The kernel pushes the modem's state with
 * sh_cell(); the page shows it:
 *   - the Cellular Data switch (the only thing that lets the radio transmit; off at every boot), the state in words;
 *   - while on: the operator (the screen only), the network type, the signal and the connection;
 *   - a line on what is not there yet (calls, SMS).
 * The Control Centre's cellular circle (a hold) is the same switch (cell_toggle). Nothing here logs. */

/* the switch is on (anything from starting to data up) */
static int data_on(void)
{
    return CELL.data == SH_DATA_STARTING || CELL.data == SH_DATA_SEARCHING || CELL.data == SH_DATA_REGISTERED ||
           CELL.data == SH_DATA_UP;
}

/* helper B2: turning it on asks first (iOS's alert): the radio transmits and data may cost money */
static void cell_on_answer(int i)
{
    if (i != 1) return;
    if (sh_cellular(1) < 0) toast("Cellular data can't start now");
    else toast("Cellular data on: connecting\xe2\x80\xa6");
    if (is_app("settings")) build_app_page(AP.a);
}
static void cell_toggle(void)
{
    if (data_on() || CELL.data == SH_DATA_STOPPING) { sh_cellular(0); toast("Cellular data off"); }
    else {
        static const kit_btn B[2] = { { "Cancel", KIT_CANCEL }, { "Turn On", KIT_PREFERRED } };
        kit_alert("Turn On Cellular Data?", "The modem connects to the mobile network. Your operator may charge for data.", B, 2, cell_on_answer);
        return;
    }
    if (is_app("settings")) build_app_page(AP.a);
}

static const char *cell_rat(void)
{
    return CELL.rat == 7 ? "LTE" : CELL.rat == 4 ? "3G" : CELL.rat >= 1 && CELL.rat <= 3 ? "2G" : "\xe2\x80\x94";
}

/* helper B2: on the kit's list pair (IOS-REFERENCE 6: cards x 16 w 358 r 26, rows 52) and iOS 26's switch, where the
 * other Settings pages start theirs */
#define CL_SW (PUSHED_TOP - 8)              /* the switch row */
#define CL_NET (CL_SW + LG_ROW + 35 + 18 + 24)   /* the network group (while on): under the state line and its header */

static void page_cell(dc *d)
{
    d_back(d, W(1));                       /* P3p (P3j-13): iOS 26's glass back and the inline title */
    d_navtitle(d, "Cellular");
    /* the switch */
    int on = data_on();
    d_group(d, CL_SW, LG_ROW);
    d_row(d, CL_SW, LG_ROW, 0, "Cellular Data", 0, 0, 1, INK, LG_RE - 16 - KSW_W - 12);
    kit_d_switch(d, LG_RE - 16 - KSW_W, CL_SW + (LG_ROW - KSW_H) / 2, on, 1);
    int bad = CELL.data == SH_DATA_FAILED || CELL.data == SH_DATA_LOCKED || CELL.data == SH_DATA_NO_SIM;
    d_text(d, TK_IT400, 13, LG_TX, bl(TK_IT400, 13, CL_SW + LG_ROW + 8), md_text(), 0, 0, bad ? KIT_RED() : KIT_SEC(), 0);
    float y = CL_SW + LG_ROW + 8 + 18;
    if (CELL.data == SH_DATA_REGISTERED || CELL.data == SH_DATA_UP || CELL.data == SH_DATA_SEARCHING) {
        char sig[16];
        int n = CELL.bars < 0 ? -1 : CELL.bars >= 4 ? 4 : CELL.bars, k = 0;
        if (n < 0) { sig[k++] = '-'; }
        else { k += put_dec(sig, n); const char *m = " of 4"; while (*m) sig[k++] = *m++; }
        sig[k] = 0;
        d_group_head(d, CL_NET, "Network");
        d_group(d, CL_NET, LG_ROW * 4);
        d_row(d, CL_NET, LG_ROW, 0, "Operator", CELL.oper[0] ? CELL.oper : "\xe2\x80\x94", 0, 0, INK, 0);
        d_row(d, CL_NET + LG_ROW, LG_ROW, 0, "Network", cell_rat(), 0, 0, INK, 0);
        d_row(d, CL_NET + 2 * LG_ROW, LG_ROW, 0, "Signal", sig, 0, 0, INK, 0);
        d_row(d, CL_NET + 3 * LG_ROW, LG_ROW, 0, "Data", CELL.data == SH_DATA_UP ? "Connected" : CELL.data == SH_DATA_REGISTERED ?
                                                         "Connecting\xe2\x80\xa6" : "Searching\xe2\x80\xa6", 0, 1, INK, 0);
        y = CL_NET + LG_ROW * 4;
    }
    d_group_foot(d, y + 4, sh_comms_ok() ? "Calls and SMS work while Cellular is on (no call audio yet). Cellular Data is off at every start." :
                                            "Calls and SMS are not available yet. Cellular Data is off at every start.");
}

static void cell_tap(float x, float y)
{
    if (BACK_HIT(x, y)) { settings_back(); return; }
    if (y >= CL_SW && y < CL_SW + LG_ROW && x >= LG_X && x < LG_RE) cell_toggle();
}
