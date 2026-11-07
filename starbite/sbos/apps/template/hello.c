/* SPDX-License-Identifier: MIT
 * A template app (docs/STARBITE-OS.md 5.1 "Writing an app"): copy this file to starbite/sbos/apps/<id>.c,
 * change the SBAPP_DEFINE line, add <id> to the app list in starbite/sbos/build.sh (and, for the host preview,
 * to starbite/host/sbos-preview.sh and sbos-host-stubs.c). Not built by default. */
#include "sbapp.h"
#include "sbfmt.h"

/* id (unique, a-z and -), name on the tile, permissions, tile colour, icon (200..206 of the system's set) */
SBAPP_DEFINE("hello", "Hello", 0, 0xff6fb3ffU, 203);

static int count;

static void show(void)
{
    struct fmt f;
    fmt_init(&f);
    fmt_d(&f, count);
    ui_begin();
    ui_title("Hello");
    ui_big("TAPS", f.b, 0);
    ui_button(1, "Tap me", "Counts the taps", 0);
    ui_button(2, "Reset", 0, UI_HALF);
    ui_textline("Everything on a screen is described between ui_begin and ui_end.", 0);
    ui_end();
    ui_tile(count ? f.b : "");              /* the line under the tile on home */
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:                      /* once, after loading */
        break;
    case EV_APP_SHOW:                       /* the app came to the front: describe the screen */
        show();
        break;
    case EV_APP_BUTTON:
        count = ev->id == 1 ? count + 1 : 0;
        show();
        break;
    default:
        break;
    }
}
