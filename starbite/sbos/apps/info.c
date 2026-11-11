/* SPDX-License-Identifier: MIT
 * Info (docs/STARBITE-OS.md 9): the emergency card.
 *
 * Two sources: /starbite/emergency.txt on the microSD card (read only: your name on the first line, then one fact
 * per line; lines starting with '#' are skipped), or, from 0.5.3, a card written here on the phone with the
 * keyboard: name, blood type, allergies, medicines, conditions and whom to call. The written card is the app's
 * own state (state_put): kept until a restart, or across restarts where the phone has its state slot (doc 6.5).
 * The card file, when there is one, is shown first. */
#include "sbapp.h"

SBAPP_DEFINE("info", "Info", SBOS_P_FILES, 0xffff6b6bU, 206);

#define GREY 0xff8e8e93U
#define ORANGE 0xffff9f0aU
#define CAP 2048
static char txt[CAP + 1];
static int n = -1;                          /* bytes read from the card file, < 0 the error */

/* the card written on the phone (the app's state) */
/* 1.2-alpha27: "CRD2" adds two more people to call and notes (kern/survival.c reads the same record for the lock
 * screen and Starbite OS Next); a "CRD1" card (the first six fields) is taken over as it is */
#define NF 9
#define NF1 6
#define FL 64
static const char *const label[NF] = { "Name", "Blood type", "Allergies", "Medicines", "Conditions", "Call",
                                       "Call 2", "Call 3", "Notes" };
static const char *const hint[NF] = { "Your full name", "e.g. 0+", "e.g. penicillin", "What you take",
                                      "e.g. asthma, diabetes", "A name and a number", "A name and a number",
                                      "A name and a number", "Anything a helper should know" };
static const int maxlen[NF] = { 40, 8, 64, 64, 64, 64, 64, 64, 64 };
static struct {
    char magic[4];                          /* "CRD2" */
    char f[NF][FL + 1];
} card;
static int page, field = -1;                /* page 0 the card, 1 the editor, 2 one field being typed */
static int readonly;                        /* opened from the lock screen's medical card: nothing to edit */

static int has_card(void)
{
    for (int i = 0; i < NF; ++i)
        if (card.f[i][0])
            return 1;
    return 0;
}

static void load(void)
{
    n = file_read("/starbite/emergency.txt", txt, CAP);
    if (n >= 0)
        txt[n] = 0;
}

static void load_card(void)
{
    int got = state_get(&card, sizeof(card));
    int v1 = got == (int)(4 + NF1 * (FL + 1)) && card.magic[3] == '1', v2 = got == (int)sizeof(card) && card.magic[3] == '2';
    if (!(v1 || v2) || card.magic[0] != 'C' || card.magic[1] != 'R' || card.magic[2] != 'D') {
        for (unsigned i = 0; i < sizeof(card); ++i)
            ((char *)&card)[i] = 0;
    } else if (v1) {
        for (int i = NF1; i < NF; ++i)
            card.f[i][0] = 0;               /* the new fields empty; saved as CRD2 at the next change */
    }
    for (int i = 0; i < NF; ++i)
        card.f[i][FL] = 0;
}

static void save_card(void)
{
    card.magic[0] = 'C', card.magic[1] = 'R', card.magic[2] = 'D', card.magic[3] = '2';
    state_put(&card, sizeof(card));
}

static void show_file(void)
{
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
        ui_big("EMERGENCY CARD", "The file is empty", ORANGE);
}

static void show(void)
{
    ui_begin();
    if (page == 2 && field >= 0) {
        ui_title(label[field]);
        ui_input_init((uint32_t)(20 + field), hint[field], field == 0 || field >= 2 ? 4 : 0, maxlen[field], card.f[field]);
        ui_textline("Done keeps it. Back leaves it as it was.", GREY);
        ui_end();
        return;
    }
    if (page == 1) {
        ui_title("Your card");
        for (int i = 0; i < NF; ++i)
            ui_button((uint32_t)(10 + i), label[i], card.f[i][0] ? card.f[i] : hint[i], 0);
        ui_button(3, "Done", 0, UI_HALF);
        ui_end();
        return;
    }
    ui_title("Info");
    if (n >= 0) {
        show_file();
    } else if (has_card()) {
        ui_big("EMERGENCY CARD", card.f[0][0] ? card.f[0] : "(no name)", 0);
        for (int i = 1; i < NF; ++i)
            if (card.f[i][0]) {
                char l[FL + 16];
                unsigned k = 0;
                for (const char *s = label[i]; *s && k < sizeof(l) - 3; ++s)
                    l[k++] = *s;
                l[k++] = ':';
                l[k++] = ' ';
                for (const char *s = card.f[i]; *s && k + 1 < sizeof(l); ++s)
                    l[k++] = *s;
                l[k] = 0;
                ui_textline(l, 0xffffffffU);
            }
        if (!readonly) {
            ui_button(2, "Edit the card", 0, UI_HALF);
            ui_textline(setting_get(SET_SAVED) == 1 ? "Saved on this phone." : "Kept until a restart (no save slot yet).", GREY);
        }
    } else {
        ui_big("EMERGENCY CARD", "Not set up", ORANGE);
        ui_textline("Write it here: your name, blood type, allergies, medicines, conditions and whom to call.", 0);
        ui_button(2, "Write the card", 0, 0);
        ui_textline("Or put /starbite/emergency.txt on a microSD card (one fact per line).", GREY);
        if (n == SBOS_EAGAIN)
            ui_textline("The card could not be read.", GREY);
    }
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        load_card();
        ui_tile(has_card() ? "Emergency card" : "Not set up");
        break;
    case EV_APP_SHOW:
        readonly = ev->value == 1;          /* the lock screen's medical card */
        if (readonly || ev->value == 0)
            page = readonly ? 0 : page;
        if (readonly)
            field = -1;
        if (n < 0)
            load();
        if (n >= 0)
            ui_tile("Emergency card");      /* a card file counts too (the lock screen shows the card then) */
        show();
        break;
    case EV_APP_HIDE:
    case EV_APP_BACK:
        page = 0;
        field = -1;
        readonly = 0;
        break;
    case EV_APP_BUTTON:
        if (ev->id == 1) {
            load();
        } else if (ev->id == 2 && !readonly) {
            page = 1;
        } else if (ev->id == 3) {
            page = 0;
        } else if (ev->id >= 10 && ev->id < 10 + NF) {
            field = (int)ev->id - 10;
            page = 2;
        } else if (ev->id >= 20 && ev->id < 20 + NF && page == 2) {     /* the keyboard's Done */
            char t[FL + 1];
            int k = ui_input_get(t, sizeof(t));
            if (k >= 0) {
                for (int i = 0; i <= k && i <= FL; ++i)
                    card.f[field][i] = t[i];
                card.f[field][FL] = 0;
                save_card();
                ui_tile(has_card() ? "Emergency card" : "Not set up");
            }
            page = 1;
            field = -1;
        }
        show();
        break;
    default:
        break;
    }
}
