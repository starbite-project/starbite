/* SPDX-License-Identifier: MIT
 * Info (docs/STARBITE-OS.md 9): the emergency card. Shows /starbite/emergency.txt from the microSD card (read
 * only): who you are, your conditions, medicines, allergies, whom to call. Plain text, one fact per line; the
 * first line is shown big (your name); lines starting with '#' are skipped. Nothing is ever written: edit the
 * file on a computer. */
#include "sbapp.h"

SBAPP_DEFINE("info", "Info", SBOS_P_FILES, 0xffff6b6bU, 206);

#define CAP 2048
static char txt[CAP + 1];
static int n = -1;                          /* bytes read, < 0 the error, -1000 not tried */

static void load(void)
{
    n = file_read("/starbite/emergency.txt", txt, CAP);
    if (n >= 0)
        txt[n] = 0;
}

static void show(void)
{
    ui_begin();
    ui_title("Info");
    if (n < 0) {
        ui_big("EMERGENCY CARD", "Not set up", 0xffff9f0aU);
        ui_textline(n == SBOS_EAGAIN ? "The card could not be read." : "There is no /starbite/emergency.txt on the card.", 0);
        ui_textline("Write one on a computer: your name on the first line, then one fact per line:", 0);
        ui_textline("conditions, medicines, allergies, blood type, who to call and their number.", 0);
        ui_button(1, "Read the card again", 0, 0);
        ui_end();
        return;
    }
    char line[96];
    int first = 1, shown = 0;
    for (int i = 0; i <= n && shown < 14;) {
        int k = 0;
        while (i < n && txt[i] != '\n' && txt[i] != '\r') {
            if (k + 1 < (int)sizeof(line))
                line[k++] = txt[i];
            ++i;
        }
        line[k] = 0;
        while (i < n && (txt[i] == '\n' || txt[i] == '\r'))
            ++i;
        if (i >= n && !k)
            break;
        if (!k || line[0] == '#')
            continue;
        if (first) {
            ui_big("EMERGENCY CARD", line, 0);
            first = 0;
        } else {
            ui_textline(line, 0xffffffffU);
        }
        ++shown;
    }
    if (first)
        ui_big("EMERGENCY CARD", "The file is empty", 0xffff9f0aU);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    if (ev->type == EV_APP_START) {
        ui_tile("Emergency card");
    } else if (ev->type == EV_APP_SHOW) {
        if (n < 0)
            load();
        show();
    } else if (ev->type == EV_APP_BUTTON && ev->id == 1) {
        load();
        show();
    }
}
