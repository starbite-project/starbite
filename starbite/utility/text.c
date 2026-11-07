/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS text with the Nordic letters: menu v7's fonts carry ASCII and a few symbols only (the shared
 * container, also the loader's), so å ä ö Å Ä Ö, the euro sign and ¤ are drawn here as their base letter
 * with the marks (dots, ring, bars) on top. Everything else goes to ui_text unchanged. */
#include "sb.h"
#include "sbui.h"
#include "ut.h"
#include "kbdmap.h"

/* base letter for a composite, 0 when the font has the character itself */
static char base_of(uint32_t cp)
{
    switch (cp) {
    case 0xe4: case 0xe5: return 'a';
    case 0xf6: return 'o';
    case 0xc4: case 0xc5: return 'A';
    case 0xd6: return 'O';
    case 0x20ac: return 'C';
    case 0xa4: return 'o';
    default: return 0;
    }
}

static int marks(int font, int x, int y, uint32_t cp, uint32_t c)
{
    char b[2] = { base_of(cp), 0 };
    int px = ui_px(font), w = ui_text_w(font, b);
    int upper = b[0] >= 'A' && b[0] <= 'Z';
    int top = upper ? px * 73 / 100 : px * 55 / 100;     /* cap height / x height of Inter */
    int t = px / 22 > 2 ? px / 22 : 2;
    int cx = x + w / 2;
    if (cp == 0xa4) {                                      /* ¤: a ring with four short spokes */
        int r = px * 17 / 100, cy = y - px * 30 / 100;
        ui_ring(cx, cy, r, t, c);
        int d = r * 7 / 10, e = r * 13 / 10;
        ui_line(cx - d, cy - d, cx - e, cy - e, t, c);
        ui_line(cx + d, cy - d, cx + e, cy - e, t, c);
        ui_line(cx - d, cy + d, cx - e, cy + e, t, c);
        ui_line(cx + d, cy + d, cx + e, cy + e, t, c);
        return x + w;
    }
    ui_text(font, x, y, b, c);
    if (cp == 0x20ac) {                                    /* euro: C with two bars */
        int x0 = x - px / 25, x1 = x + w * 7 / 10;
        ui_fill(x0, y - top * 62 / 100 - t / 2, x1 - x0, t, c);
        ui_fill(x0, y - top * 40 / 100 - t / 2, x1 - x0, t, c);
    } else if (cp == 0xe5 || cp == 0xc5) {                /* ring */
        int r = px * 8 / 100 > 3 ? px * 8 / 100 : 3;
        ui_ring(cx, y - top - r - px * 6 / 100, r, t * 3 / 4 > 1 ? t * 3 / 4 : 1, c);
    } else {                                               /* diaeresis */
        int r = px * 6 / 100 > 2 ? px * 6 / 100 : 2, dy = y - top - r - px * 6 / 100, dx = px * 12 / 100;
        ui_disc(cx - dx, dy, r, c);
        ui_disc(cx + dx, dy, r, c);
    }
    return x + w;
}

static int run(int font, int x, int y, const char *s, uint32_t c, int draw)
{
    char seg[160];
    unsigned n = 0;
    while (s && *s) {
        uint32_t cp;
        unsigned k = kbd_utf8(s, &cp);
        if (base_of(cp) || n + k + 1 >= sizeof(seg)) {
            seg[n] = 0;
            if (n)
                x = draw ? ui_text(font, x, y, seg, c) : x + ui_text_w(font, seg);
            n = 0;
            if (base_of(cp)) {
                if (draw) {
                    x = marks(font, x, y, cp, c);
                } else {
                    char b[2] = { base_of(cp), 0 };
                    x += ui_text_w(font, b);
                }
                s += k;
                continue;
            }
        }
        for (unsigned i = 0; i < k; ++i)
            seg[n++] = s[i];
        s += k;
    }
    seg[n] = 0;
    if (n)
        x = draw ? ui_text(font, x, y, seg, c) : x + ui_text_w(font, seg);
    return x;
}

int ut_text(int font, int x, int y, const char *s, uint32_t c) { return run(font, x, y, s, c, 1); }
int ut_text_w(int font, const char *s) { return run(font, 0, 0, s, 0, 0); }
int ut_text_c(int font, int cx, int y, const char *s, uint32_t c)
{
    return run(font, cx - ut_text_w(font, s) / 2, y, s, c, 1);
}
