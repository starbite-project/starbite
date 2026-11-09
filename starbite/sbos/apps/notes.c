/* SPDX-License-Identifier: MIT
 * Notes (Starbite OS 1.2-alpha20): up to 8 short notes (64 characters each, the system keyboard), saved on the phone
 * with the app's own state (the sbos-state slot). The list, a note's page (Edit, hold to delete), New note. */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("notes", "Notes", 0, 0xffffd60aU, 210);

#define NN 8
#define NL 64
#define NOTES_MAGIC 0x3153544eU             /* "NTS1" */

static struct {
    uint32_t magic, n;
    char t[NN][NL + 1];
} st;
static int page, cur = -1;                  /* page 0 the list, 1 a note, 2 editing (cur = st.n: a new one) */

static void save(void)
{
    st.magic = NOTES_MAGIC;
    state_put(&st, sizeof(st));
    struct fmt f;                                   /* for the host tests: the count only, never the text */
    fmt_init(&f);
    fmt_s(&f, "notes: saved, ");
    fmt_d(&f, st.n);
    sb_log_app(f.b);
}

static void tile(void)
{
    struct fmt f;
    fmt_init(&f);
    if (!st.n) {
        fmt_s(&f, "No notes");
    } else {
        fmt_d(&f, st.n);
        fmt_s(&f, st.n == 1 ? " note" : " notes");
    }
    ui_tile(f.b);
}

static void show(void)
{
    ui_begin();
    if (page == 2) {
        ui_title(cur < (int)st.n ? "Edit the note" : "New note");
        ui_input_init(30, "A note", 4, NL, cur < (int)st.n ? st.t[cur] : "");
        ui_textline("Done keeps it. Back leaves it as it was.", 0xff8e8e93U);
        ui_end();
        return;
    }
    if (page == 1 && cur >= 0 && cur < (int)st.n) {
        ui_title("Note");
        ui_textline(st.t[cur], 0xffffffffU);
        ui_button(20, "Edit", 0, UI_HALF);
        ui_hold(21, "Hold to delete", 1200, 0);
        ui_end();
        return;
    }
    ui_title("Notes");
    for (uint32_t i = 0; i < st.n; ++i)
        ui_button(10 + i, st.t[i], 0, 0);
    if (st.n < NN)
        ui_button(2, "New note", 0, UI_ACCENT);
    else
        ui_textline("Eight notes is the most: delete one to write another.", 0xff8e8e93U);
    ui_textline(setting_get(SET_SAVED) == 1 ? "Saved on this phone." : "Kept until a restart (no save slot yet).",
                0xff8e8e93U);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        if (state_get(&st, sizeof(st)) != (int)sizeof(st) || st.magic != NOTES_MAGIC || st.n > NN) {
            st.magic = NOTES_MAGIC;
            st.n = 0;
        } else {
            struct fmt f;
            fmt_init(&f);
            fmt_s(&f, "notes: loaded ");
            fmt_d(&f, st.n);
            sb_log_app(f.b);
        }
        for (int i = 0; i < NN; ++i)
            st.t[i][NL] = 0;
        tile();
        break;
    case EV_APP_SHOW:
        show();
        break;
    case EV_APP_HIDE:
    case EV_APP_BACK:
        page = 0;
        cur = -1;
        break;
    case EV_APP_BUTTON:
        if (ev->id == 2 && st.n < NN) {
            cur = (int)st.n;
            page = 2;
        } else if (ev->id >= 10 && ev->id < 10 + st.n) {
            cur = (int)ev->id - 10;
            page = 1;
        } else if (ev->id == 20 && cur >= 0) {
            page = 2;
        } else if (ev->id == 30 && page == 2) {     /* the keyboard's Done */
            char t[NL + 1];
            int was_new = cur == (int)st.n;
            int k = ui_input_get(t, sizeof(t));
            if (k > 0 && cur >= 0 && cur <= (int)st.n && cur < NN) {
                for (int i = 0; i <= k && i <= NL; ++i)
                    st.t[cur][i] = t[i];
                st.t[cur][NL] = 0;
                if (cur == (int)st.n)
                    ++st.n;
                save();
                tile();
            }
            page = !was_new && cur < (int)st.n ? 1 : 0;     /* a new note: back to the list, where it now is */
        }
        show();
        break;
    case EV_APP_HOLD:
        if (ev->id == 21 && cur >= 0 && cur < (int)st.n) {
            for (uint32_t i = (uint32_t)cur; i + 1 < st.n; ++i)
                for (int k = 0; k <= NL; ++k)
                    st.t[i][k] = st.t[i + 1][k];
            --st.n;
            save();
            tile();
            page = 0;
            cur = -1;
        }
        show();
        break;
    default:
        break;
    }
}
