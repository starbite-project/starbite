/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS calculator (Everyday): scientific (double precision, + - × ÷ ^, parentheses, sin cos tan sqrt
 * ln log, π, e; degrees) and programmer (64-bit integers: + - × ÷ mod and or xor shl shr not; hex, decimal and
 * binary views). No libm on bare metal: sqrt is the FSQRT instruction, the rest are written here with range
 * reduction (accurate to about 1e-14 relative, plenty for 12 shown digits). Tap the keys; Bixby leaves. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "ut.h"
#include "calc.h"

/* ---- programmer mode: a left-to-right integer calculator ----------------------------------------- */
static uint64_t pacc, pcur;
static int pop = 0, pbase = 16, pfresh = 1;

static uint64_t papply(uint64_t a, int op, uint64_t b)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b ? a / b : 0;
    case '%': return b ? a % b : 0;
    case '&': return a & b;
    case '|': return a | b;
    case '^': return a ^ b;
    case '<': return b < 64 ? a << b : 0;
    case '>': return b < 64 ? a >> b : 0;
    default: return b;
    }
}

/* ---- UI ---------------------------------------------------------------------------------------- */
#define KX0 40
#define KY0 1040
#define KW 260
#define KH 250
#define KG 20

static const char *const sci_keys[30] = {
    "sin", "cos", "tan", "sqrt", "^",
    "ln", "log", "(", ")", "/",
    "7", "8", "9", "pi", "\xc3\x97",
    "4", "5", "6", "e", "-",
    "1", "2", "3", "C", "+",
    "0", ".", "DEL", "PROG", "=",
};
static const char *const prog_keys[30] = {
    "A", "B", "C", "D", "AND",
    "E", "F", "SHL", "SHR", "OR",
    "7", "8", "9", "NOT", "XOR",
    "4", "5", "6", "MOD", "/",
    "1", "2", "3", "\xc3\x97", "-",
    "0", "HEX", "CLR", "SCI", "+",
};

static char line[96];
static char result[64];
static int prog;

static void key_rect(int i, int *x, int *y)
{
    *x = KX0 + (i % 5) * (KW + KG);
    *y = KY0 + (i / 5) * (KH + KG);
}

static void prog_text(void)
{
    struct ub u;
    ub_init(&u);
    if (pbase == 16) {
        ub_s(&u, "0x");
        ub_hexn(&u, pcur, pcur >> 32 ? 16 : pcur >> 16 ? 8 : 4);
    } else if (pbase == 10) {
        ub_dec(&u, pcur);
    } else {
        ub_s(&u, "0b");
        int top = 63;
        while (top > 0 && !(pcur >> top & 1)) --top;
        for (int b = top; b >= 0; --b) ub_c(&u, (char)('0' + (pcur >> b & 1)));
    }
    sb_strcpy(result, sizeof(result), u.b);
}

static void draw(void)
{
    ui_fill(0, UI_VIEW_Y0, UI_W, UI_H - UI_VIEW_Y0, C_BG);
    ui_round(40, UI_VIEW_Y0 + 10, UI_W - 80, 640, 48, C_CARD);
    ui_text(F_HEAD, 90, UI_VIEW_Y0 + 80, prog ? "PROGRAMMER (64-BIT)" : "SCIENTIFIC (DEGREES)", C_L2);
    ui_text_cut(F_TILE, 90, UI_VIEW_Y0 + 220, prog ? "" : line, UI_W - 180, C_L2);
    ui_text_cut(F_BIG, 90, UI_VIEW_Y0 + 420, result[0] ? result : "0", UI_W - 180, C_L1);
    if (prog) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "dec ");
        ub_dec(&u, pcur);
        ub_s(&u, "   hex ");
        ub_hexn(&u, pcur, 16);
        ui_text_cut(F_MONO, 90, UI_VIEW_Y0 + 560, u.b, UI_W - 180, C_L3);
    }
    const char *const *k = prog ? prog_keys : sci_keys;
    for (int i = 0; i < 30; ++i) {
        int x, y;
        key_rect(i, &x, &y);
        int fn = (i % 5 == 4) || (k[i][0] == '=' );
        uint32_t bg = k[i][0] == '=' ? C_STAR : fn ? C_CARD3 : C_CARD2;
        ui_round(x, y, KW, KH, 36, bg);
        int w = (int)ui_text_fit(F_BTN, k[i], KW - 20);
        (void)w;
        ui_text_cut(F_BTN, x + 40, y + KH / 2 + 22, k[i], KW - 60, k[i][0] == '=' ? C_BG : C_L1);
    }
    ui_dirty(0, UI_VIEW_Y0, UI_W, UI_H - UI_VIEW_Y0);
}

static void append(const char *s)
{
    unsigned n = sb_strlen(line);
    for (unsigned i = 0; s[i] && n + 1 < sizeof(line); ++i)
        line[n++] = s[i];
    line[n] = 0;
}

static void sci_key(const char *k)
{
    if (sb_streq(k, "C")) {
        line[0] = result[0] = 0;
    } else if (sb_streq(k, "DEL")) {
        unsigned n = sb_strlen(line);
        while (n && (line[n - 1] & 0xc0) == 0x80) --n;    /* a whole UTF-8 character */
        if (n) line[n - 1] = 0;
    } else if (sb_streq(k, "=")) {
        double v;
        if (calc_eval(line, &v))
            sb_strcpy(result, sizeof(result), "check the expression");
        else
            calc_format(v, result, sizeof(result));
    } else if (sb_streq(k, "sin") || sb_streq(k, "cos") || sb_streq(k, "tan") || sb_streq(k, "sqrt") ||
               sb_streq(k, "ln") || sb_streq(k, "log")) {
        append(k);
        append("(");
    } else {
        append(k);
    }
}

static void prog_key(const char *k)
{
    if (k[0] && !k[1] && ((k[0] >= '0' && k[0] <= '9') || (k[0] >= 'A' && k[0] <= 'F'))) {
        unsigned d = k[0] <= '9' ? (unsigned)(k[0] - '0') : (unsigned)(k[0] - 'A' + 10);
        if (d >= (unsigned)pbase)
            return;
        pcur = pfresh ? d : pcur * (uint64_t)pbase + d;
        pfresh = 0;
    } else if (sb_streq(k, "HEX")) {
        pbase = pbase == 16 ? 10 : pbase == 10 ? 2 : 16;
    } else if (sb_streq(k, "CLR")) {
        pacc = pcur = 0;
        pop = 0;
        pfresh = 1;
    } else if (sb_streq(k, "NOT")) {
        pcur = ~pcur;
    } else {
        int op = sb_streq(k, "+") ? '+' : sb_streq(k, "-") ? '-' : sb_streq(k, "\xc3\x97") ? '*' :
                 sb_streq(k, "/") ? '/' : sb_streq(k, "MOD") ? '%' : sb_streq(k, "AND") ? '&' :
                 sb_streq(k, "OR") ? '|' : sb_streq(k, "XOR") ? '^' : sb_streq(k, "SHL") ? '<' :
                 sb_streq(k, "SHR") ? '>' : 0;
        pacc = pop ? papply(pacc, pop, pcur) : pcur;
        pcur = pacc;
        pop = op;
        pfresh = 1;
    }
    prog_text();
}

void ut_calc(void)
{
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    ui_header_draw("Calculator", "Everyday", "", 0);
    ut_status_bar();
    if (prog)
        prog_text();
    draw();
    ui_present();
    for (;;) {
        struct ui_ev e;
        ui_poll(&e, 20);
        ut_service();
        if (e.type == EV_BACK || e.type == EV_SWIPE_R || e.type == EV_HOME ||
            (e.type == EV_TAP && ui_header_back_hit(e.x, e.y)))
            break;
        if (e.type != EV_TAP)
            continue;
        for (int i = 0; i < 30; ++i) {
            int x, y;
            key_rect(i, &x, &y);
            if (e.x < x || e.x >= x + KW || e.y < y || e.y >= y + KH)
                continue;
            const char *k = prog ? prog_keys[i] : sci_keys[i];
            if (sb_streq(k, "PROG") || sb_streq(k, "SCI")) {
                prog = !prog;
                result[0] = 0;
                if (prog) prog_text();
            } else if (prog) {
                prog_key(k);
            } else {
                sci_key(k);
            }
            ui_round(x, y, KW, KH, 36, C_STAR);           /* the press, briefly */
            ui_dirty(x, y, KW, KH);
            ui_present();
            sb_mdelay(40);
            draw();
            ui_present();
        }
    }
}
