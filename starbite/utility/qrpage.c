/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS QR codes on the screen (qr.c encodes): the lines of /starbite/qr.txt on the card, one code
 * each ("Label = text", or the text alone; # comments), for a Wi-Fi login (WIFI:T:WPA;S:name;P:pass;;), a
 * link or a note to hand to another phone's camera. Without the file: an example. Read only. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "ut.h"
#include "exfat.h"
#include "qr.h"

#define QMAX 12
static struct { char label[40]; char text[272]; int too_long; } q[QMAX];
static int nq, nlines;                                /* nlines: codes in the file (more than QMAX are not shown) */

static int is_sp(char c) { return c == ' ' || c == '\t'; }

/* "Label = text" or "text": the label is what comes before " = " (with the spaces) */
static void add_line(const char *s, unsigned n)
{
    while (n && is_sp(*s))
        ++s, --n;
    while (n && (is_sp(s[n - 1]) || s[n - 1] == '\r'))
        --n;
    if (!n || *s == '#')
        return;
    if (++nlines > QMAX)
        return;
    /* a label is a short start without ':' before " = " (so "WIFI:...;S:a = b;;" stays one text) */
    unsigned eq = 0;
    for (unsigned i = 0; i + 2 < n && i < sizeof(q[nq].label) && s[i] != ':'; ++i)
        if (s[i] == ' ' && s[i + 1] == '=' && s[i + 2] == ' ') {
            eq = i;
            break;
        }
    const char *t = eq ? s + eq + 3 : s;
    unsigned tn = eq ? n - eq - 3 : n, ln = eq ? eq : n;
    unsigned i;
    for (i = 0; i < ln && i + 1 < sizeof(q[nq].label); ++i)
        q[nq].label[i] = s[i];
    if (!eq && i == sizeof(q[nq].label) - 1)
        q[nq].label[i - 1] = '.';                     /* text as its own label: cut */
    q[nq].label[i] = 0;
    for (i = 0; i < tn && i + 1 < sizeof(q[nq].text); ++i)
        q[nq].text[i] = t[i];
    q[nq].text[i] = 0;
    q[nq].too_long = tn > qr_capacity(QR_ECC_L);       /* never a code of a cut text */
    ++nq;
}

static void load(void)
{
    nq = nlines = 0;
    struct exfat *fs = ut_card();
    static struct exfat_ent scratch[40];
    static char src[4096] __attribute__((aligned(64)));
    struct exfat_ent e;
    if (fs && !exfat_walk(fs, "/starbite/qr.txt", &e, scratch, 40) && !e.dir) {
        uint32_t n = e.size < sizeof(src) ? (uint32_t)e.size : (uint32_t)sizeof(src);
        if (!exfat_read_chain(fs, e.first, e.nofat, 0, (uint8_t *)src, n)) {
            unsigned a = 0;
            if (n >= 3 && (uint8_t)src[0] == 0xef && (uint8_t)src[1] == 0xbb && (uint8_t)src[2] == 0xbf)
                a = 3;                                /* UTF-8 byte order mark */
            unsigned end = n;
            if (e.size > sizeof(src))                 /* read only so far: drop the cut last line */
                while (end > a && src[end - 1] != '\n')
                    --end;
            for (unsigned i = a; i <= end; ++i)
                if (i == end || src[i] == '\n') {
                    add_line(src + a, i - a);
                    a = i + 1;
                }
        }
    }
    if (!nq) {
        static const char ex[] = "Example = Put lines in /starbite/qr.txt on the card: Label = text";
        add_line(ex, sizeof(ex) - 1);
    }
}

static void show(int i, struct qr *c)
{
    unsigned len = 0;
    while (q[i].text[len])
        ++len;
    ui_fill(0, UI_VIEW_Y0, UI_W, UI_H - UI_VIEW_Y0, C_BG);
    if (q[i].too_long || (!qr_encode((const uint8_t *)q[i].text, len, QR_ECC_M, c) &&
                          !qr_encode((const uint8_t *)q[i].text, len, QR_ECC_L, c))) {
        ut_text(F_TITLE_S, 70, UI_VIEW_Y0 + 100, q[i].label, C_L1);
        ui_text(F_ROW, 70, UI_VIEW_Y0 + 200, "Too long for a code (271 bytes at most)", C_ORANGE);
        ui_dirty(0, UI_VIEW_Y0, UI_W, UI_H - UI_VIEW_Y0);
        return;
    }
    int quiet = 4, mod = (UI_W - 120) / (c->size + 2 * quiet);
    int side = mod * (c->size + 2 * quiet), x0 = (UI_W - side) / 2, y0 = UI_VIEW_Y0 + 60;
    ui_fill(x0, y0, side, side, 0xffffffffU);         /* white with the quiet zone */
    for (int y = 0; y < c->size; ++y)
        for (int x = 0; x < c->size; ++x)
            if (c->m[y][x])
                ui_fill(x0 + (quiet + x) * mod, y0 + (quiet + y) * mod, mod, mod, 0xff000000U);
    int ty = y0 + side + 110;
    ut_text(F_TITLE_S, 70, ty, q[i].label, C_L1);
    ui_text_cut(F_MONO, 70, ty + 90, q[i].text, UI_W - 140, C_L2);
    struct ub u;
    ub_init(&u);
    ub_dec(&u, (unsigned)i + 1);
    ub_s(&u, " of ");
    ub_dec(&u, (unsigned)nq);
    if (nlines > nq) {
        ub_s(&u, " (the file has ");
        ub_dec(&u, (unsigned)nlines);
        ub_s(&u, ")");
    }
    ub_s(&u, "   version ");
    ub_dec(&u, (unsigned)c->ver);
    ub_s(&u, c->ecc == QR_ECC_M ? ", error correction M" : ", error correction L");
    ui_text(F_SMALL, 70, ty + 170, u.b, C_L3);
    ui_text(F_SMALL, 70, UI_H - 90, "Volume or tap: next code   Bixby: back", C_L3);
    ui_dirty(0, UI_VIEW_Y0, UI_W, UI_H - UI_VIEW_Y0);
}

void ut_qr(void)
{
    static struct qr c;
    load();
    int i = 0;
    ut_awake = 1;                                     /* someone is scanning it */
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    ui_header_draw("QR codes", "Everyday", "", 0);
    ut_status_bar();
    show(i, &c);
    ui_present();
    uint64_t next = sb_ms() + 1000;
    for (;;) {
        struct ui_ev e;
        ui_poll(&e, 20);
        ut_service();
        if (ut_back_event(&e))
            break;
        ut_awake = ui_idle_ms() < UT_WATCH_MS;        /* not forever: a code on the OLED, a password under it */
        if (e.type == EV_UP || e.type == EV_DOWN || e.type == EV_TAP || e.type == EV_SELECT) {
            i = (i + (e.type == EV_UP ? nq - 1 : 1)) % nq;
            show(i, &c);
        }
        if (sb_ms() >= next) {
            ut_status_bar();
            next = sb_ms() + 1000;
        }
        ui_present();
    }
    ut_awake = 0;
}
