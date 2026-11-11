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

static void cell_toggle(void)
{
    if (data_on() || CELL.data == SH_DATA_STOPPING) { sh_cellular(0); toast("Cellular data off"); }
    else if (sh_cellular(1) < 0) toast("Cellular data can't start now");
    else toast("Cellular data on: connecting\xe2\x80\xa6");
    if (is_app("settings")) build_app_page(AP.a);
}

static const char *cell_rat(void)
{
    return CELL.rat == 7 ? "LTE" : CELL.rat == 4 ? "3G" : CELL.rat >= 1 && CELL.rat <= 3 ? "2G" : "\xe2\x80\x94";
}

#define CL_SW 150.0f                        /* the switch row */
#define CL_ST (CL_SW + 47 + 8)              /* the state line under it */
#define CL_NET (CL_SW + 47 + 30 + 32)       /* the network group (while on) */

static void cl_row(dc *d, float y, const char *l, const char *r, int last)
{
    d_text(d, TK_IT400, 16, 32, y + 13 + 16, l, 0, 0, INK, 0);
    float w = tw(TK_IT400, 16, r, 0, TK_TNUM);
    d_text(d, TK_IT400, 16, 358 - w, y + 13 + 16, r, 0, TK_TNUM, W(.55f), 0);
    if (!last) d_rrect(d, 32, y + 46, 342, .5f, 0, W(.08f));
}

static void page_cell(dc *d)
{
    d_back(d, W(1));                       /* P3p (P3j-13): iOS 26's glass back and the inline title */
    d_navtitle(d, "Cellular");
    /* the switch */
    int on = data_on();
    d_rrect(d, 16, CL_SW, 358, 47, 16, tk_hex(0x151c33, 1));
    d_text(d, TK_IT400, 16, 32, CL_SW + 13 + 16, "Cellular Data", 0, 0, INK, 0);
    d_rrect(d, 307, CL_SW + 8, 51, 31, 15.5f, on ? tk_hex(0x34c759, 1) : W(.16f));
    d_rrect(d, on ? 329 : 309, CL_SW + 10, 27, 27, 13.5f, W(1));
    int bad = CELL.data == SH_DATA_FAILED || CELL.data == SH_DATA_LOCKED || CELL.data == SH_DATA_NO_SIM;
    d_text(d, TK_IT400, 13, 36, bl(TK_IT400, 13, CL_ST), md_text(), 0, 0, bad ? tk_hex(0xff453a, 1) : W(.55f), 0);
    float y = CL_ST + 24;
    if (CELL.data == SH_DATA_REGISTERED || CELL.data == SH_DATA_UP || CELL.data == SH_DATA_SEARCHING) {
        char sig[16];
        int n = CELL.bars < 0 ? -1 : CELL.bars >= 4 ? 4 : CELL.bars, k = 0;
        if (n < 0) { sig[k++] = '-'; }
        else { k += put_dec(sig, n); const char *m = " of 4"; while (*m) sig[k++] = *m++; }
        sig[k] = 0;
        d_text(d, TK_IT400, 13, 36, y + 10 + 13, "NETWORK", .52f, 0, W(.55f), 0);
        d_rrect(d, 16, CL_NET, 358, 47.0f * 4 - 1, 16, tk_hex(0x151c33, 1));
        cl_row(d, CL_NET, "Operator", CELL.oper[0] ? CELL.oper : "\xe2\x80\x94", 0);
        cl_row(d, CL_NET + 47, "Network", cell_rat(), 0);
        cl_row(d, CL_NET + 94, "Signal", sig, 0);
        cl_row(d, CL_NET + 141, "Data", CELL.data == SH_DATA_UP ? "Connected" : CELL.data == SH_DATA_REGISTERED ?
                                        "Connecting\xe2\x80\xa6" : "Searching\xe2\x80\xa6", 1);
        y = CL_NET + 47 * 4 + 8;
    }
    d_text(d, TK_IT400, 13, 36, bl(TK_IT400, 13, y + 14), "Calls and SMS are not available yet.", 0, 0, W(.4f), 0);
    d_text(d, TK_IT400, 13, 36, bl(TK_IT400, 13, y + 32), "Cellular Data is off at every start.", 0, 0, W(.4f), 0);
}

static void cell_tap(float x, float y)
{
    if (x < 120 && y >= 40 && y < 86) { AS.sub = 0; build_app_page(AP.a); return; }
    if (y >= CL_SW && y < CL_SW + 47 && x >= 16 && x < 374) cell_toggle();
}
