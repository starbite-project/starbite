/* SPDX-License-Identifier: MIT
 * Guide (1.2-alpha28): the offline survival guide (guide/sbguide.h): first aid, water, shelter and fire, signalling,
 * navigation, food and weather. Chapters, then pages, read with Next / Previous. Nothing to set; no permissions. */
#include "sbapp.h"
#include "sbfmt.h"
#include "../guide/sbguide.h"

SBAPP_DEFINE("guide", "Guide", 0, 0xff30d158U, 210);

#define RED 0xffff453aU
#define GREY 0xff8e8e93U
#define WRAP 118                            /* the classic UI shows a text line in at most 3 rows */

static int ch = -1, pg = -1;

/* a long line in pieces at a sentence (or clause) break before WRAP characters */
static void text(const char *s, uint32_t argb)
{
    char b[WRAP + 2];
    while (*s) {
        int n = 0;
        while (s[n] && n <= WRAP)
            ++n;
        if (s[n]) {
            int cut = 0;
            for (int i = n - 1; i > 20 && !cut; --i)
                if ((s[i] == '.' || s[i] == ':' || s[i] == ';') && s[i + 1] == ' ')
                    cut = i + 1;
            for (int i = n - 1; i > 20 && !cut; --i)
                if (s[i] == ',' && s[i + 1] == ' ')
                    cut = i + 1;
            for (int i = n - 1; i > 20 && !cut; --i)
                if (s[i] == ' ')
                    cut = i;
            n = cut ? cut : WRAP;
        }
        for (int i = 0; i < n; ++i)
            b[i] = s[i];
        b[n] = 0;
        ui_textline(b, argb);
        s += n;
        while (*s == ' ')
            ++s;
    }
}

static void show(void)
{
    struct fmt f;
    ui_begin();
    if (ch < 0) {
        ui_title("Survival guide");
        text("! In danger: call 112 first if any network works.", RED);
        for (int i = 0; i < SBG_NCH; ++i)
            ui_button((uint32_t)(10 + i), sbg_chapters[i].title, sbg_chapters[i].sub, 0);
        ui_textline("Offline help, not a substitute for training or a doctor.", GREY);
    } else if (pg < 0) {
        const struct sbg_chapter *c = &sbg_chapters[ch];
        ui_title(c->title);
        for (int i = 0; i < c->n; ++i)
            ui_button((uint32_t)(40 + i), c->pages[i].title, 0, 0);
        ui_button(2, "All chapters", 0, 0);
    } else {
        const struct sbg_chapter *c = &sbg_chapters[ch];
        const struct sbg_page *p = &c->pages[pg];
        ui_title(p->title);
        for (const char *const *l = p->lines; *l; ++l) {
            if ((*l)[0] == '!' && (*l)[1] == ' ')
                text(*l + 2, RED);
            else
                text(*l, 0xffffffffU);
        }
        fmt_init(&f);
        fmt_s(&f, c->title);
        fmt_s(&f, ", ");
        fmt_u(&f, (uint64_t)pg + 1U);
        fmt_s(&f, " of ");
        fmt_u(&f, (uint64_t)c->n);
        ui_textline(f.b, GREY);
        if (pg > 0)
            ui_button(3, "Previous", c->pages[pg - 1].title, UI_HALF);
        if (pg + 1 < c->n)
            ui_button(4, "Next", c->pages[pg + 1].title, UI_HALF);
        ui_button(5, c->title, "This chapter's pages", 0);
    }
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    switch (ev->type) {
    case EV_APP_START:
        ui_tile("First aid, water, signals");
        break;
    case EV_APP_SHOW:
        show();
        break;
    case EV_APP_BUTTON: {
        int id = (int)ev->id;
        if (id >= 10 && id < 10 + SBG_NCH) {
            ch = id - 10;
            pg = -1;
        } else if (ch >= 0 && id >= 40 && id < 40 + sbg_chapters[ch].n) {
            pg = id - 40;
        } else if (id == 2) {
            ch = pg = -1;
        } else if (id == 3 && pg > 0) {
            --pg;
        } else if (id == 4 && ch >= 0 && pg + 1 < sbg_chapters[ch].n) {
            ++pg;
        } else if (id == 5) {
            pg = -1;
        }
        show();
        break;
    }
    case EV_APP_BACK:                       /* the kernel closes the app: start at the chapters next time */
        ch = pg = -1;
        break;
    default:
        break;
    }
}
