/* SPDX-License-Identifier: MIT
 * Calculator (Starbite OS 1.2-alpha20): + - x / with a decimal point, C and Delete, chained as a pocket calculator
 * does (3 + 4 x 2 = 14). Numbers are fixed point (6 decimals, 64-bit, 128-bit products): no floating point in an app.
 * The keys are four rows of level pickers (one tap = one key). */
#include "sbapp.h"
#include "sbfmt.h"

SBAPP_DEFINE("calc", "Calculator", 0, 0xffbf5af2U, 209);

#define SCALE 1000000LL
#define LIMIT 999999999999LL                /* 12 digits before the point */

static int64_t acc, cur;                    /* the running value and the number being typed (both x SCALE) */
static int op;                              /* 0 none, '+', '-', '*', '/' */
static int typing, decimals, err;           /* decimals: digits typed after the point (-1: no point yet) */
static char shown[40];

static void fmt_fixed(char *o, unsigned cap, int64_t v)
{
    struct fmt f;
    fmt_init(&f);
    if (v < 0) {
        fmt_c(&f, '-');
        v = -v;
    }
    fmt_u(&f, (uint64_t)(v / SCALE));
    int64_t fr = v % SCALE;
    if (fr) {
        char d[8];
        int n = 6;
        for (int i = 5; i >= 0; --i, fr /= 10)
            d[i] = (char)('0' + fr % 10);
        while (n > 0 && d[n - 1] == '0')
            --n;
        fmt_c(&f, '.');
        for (int i = 0; i < n; ++i)
            fmt_c(&f, d[i]);
    }
    unsigned i = 0;
    for (; f.b[i] && i + 1 < cap; ++i)
        o[i] = f.b[i];
    o[i] = 0;
}

/* unsigned 128 / 64 by long division: apps link no libgcc (no __udivti3); shifts, compares and subtractions on
 * unsigned __int128 are inline */
static unsigned __int128 udiv128(unsigned __int128 n, uint64_t d)
{
    unsigned __int128 q = 0, r = 0;
    for (int i = 127; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1U);
        if (r >= d) {
            r -= d;
            q |= (unsigned __int128)1 << i;
        }
    }
    return q;
}

static int64_t apply(int64_t a, int64_t b, int o)
{
    int neg = 0;
    unsigned __int128 m;
    if (o == '+' || o == '-') {
        int64_t r = o == '+' ? a + b : a - b;   /* |a|, |b| <= 1e18: no overflow in int64 */
        if (r > LIMIT * SCALE || r < -LIMIT * SCALE) {
            err = 2;
            return 0;
        }
        return r;
    }
    if (o != '*' && o != '/')
        return b;
    uint64_t ua = (uint64_t)(a < 0 ? -a : a), ub = (uint64_t)(b < 0 ? -b : b);
    neg = (a < 0) != (b < 0);
    if (o == '*') {
        m = udiv128((unsigned __int128)ua * ub, (uint64_t)SCALE);
    } else {
        if (!ub) {
            err = 1;
            return 0;
        }
        m = udiv128((unsigned __int128)ua * (uint64_t)SCALE, ub);
    }
    if (m > (unsigned __int128)LIMIT * SCALE) {
        err = 2;
        return 0;
    }
    return neg ? -(int64_t)m : (int64_t)m;
}

static void clear(void)
{
    acc = cur = 0;
    op = 0;
    typing = 0;
    decimals = -1;
    err = 0;
}

static void digit(int d)
{
    if (err)
        clear();
    if (!typing) {
        cur = 0;
        decimals = -1;
        typing = 1;
    }
    if (decimals >= 6)
        return;
    if (decimals < 0) {
        if (cur / SCALE >= LIMIT / 10)
            return;
        cur = (cur / SCALE * 10 + d) * SCALE;
    } else {
        int64_t step = SCALE;
        for (int i = 0; i <= decimals; ++i)
            step /= 10;
        cur += d * step;
        ++decimals;
    }
}

static void point(void)
{
    if (err)
        clear();
    if (!typing) {
        cur = 0;
        typing = 1;
        decimals = -1;
    }
    if (decimals < 0)
        decimals = 0;
}

static void backspace(void)
{
    if (!typing || err)
        return;
    if (decimals > 0) {
        int64_t keep = SCALE;               /* the last decimal dropped: truncate to decimals - 1 places */
        for (int i = 0; i < decimals - 1; ++i)
            keep /= 10;
        cur = cur / keep * keep;
        --decimals;
    } else if (decimals == 0) {
        decimals = -1;
    } else {
        cur = cur / SCALE / 10 * SCALE;
    }
}

static void operate(int o)
{
    if (err)
        return;
    if (typing || !op)
        acc = op ? apply(acc, cur, op) : (typing ? cur : acc);
    op = o;
    typing = 0;
    cur = acc;
}

static void equals(void)
{
    if (err || !op)
        return;
    acc = apply(acc, cur, op);
    op = 0;
    typing = 0;
    cur = acc;
    char r[40], line[56] = "calc: = ";              /* for the host tests (pmsg on the phone: one short line) */
    fmt_fixed(r, sizeof(r), acc);
    unsigned n = 8;
    if (err)
        for (const char *e = "error"; *e; ++e)
            line[n++] = *e;
    else
        for (unsigned i = 0; r[i] && n + 1 < sizeof(line); ++i)
            line[n++] = r[i];
    line[n] = 0;
    sb_log_app(line);
}

static void show(void)
{
    ui_begin();
    ui_title("Calculator");
    if (err) {
        ui_big(err == 1 ? "CANNOT DIVIDE BY ZERO" : "TOO LARGE", "Error", 0xffff453aU);
    } else {
        char caption[48] = "";
        if (op) {
            char a[40];
            fmt_fixed(a, sizeof(a), acc);
            struct fmt f;
            fmt_init(&f);
            fmt_s(&f, a);
            fmt_c(&f, ' ');
            fmt_s(&f, op == '*' ? "x" : op == '/' ? "/" : op == '+' ? "+" : "-");
            unsigned i = 0;
            for (; f.b[i] && i + 1 < sizeof(caption); ++i)
                caption[i] = f.b[i];
            caption[i] = 0;
        }
        fmt_fixed(shown, sizeof(shown), typing ? cur : (op ? cur : acc));
        if (typing && decimals == 0) {
            unsigned n = 0;
            while (shown[n])
                ++n;
            if (n + 1 < sizeof(shown)) {
                shown[n] = '.';
                shown[n + 1] = 0;
            }
        }
        ui_big(caption, shown, 0);
    }
    ui_level(10, "7|8|9|/", -1);
    ui_level(11, "4|5|6|x", -1);
    ui_level(12, "1|2|3|-", -1);
    ui_level(13, "0|.|=|+", -1);
    ui_button(1, "C", "Clear", UI_HALF);
    ui_button(2, "Delete", "The last digit", UI_HALF);
    ui_end();
}

void app_event(const struct sbos_event *ev)
{
    static const char rows[4][4] = { { '7', '8', '9', '/' }, { '4', '5', '6', '*' }, { '1', '2', '3', '-' },
                                     { '0', '.', '=', '+' } };
    switch (ev->type) {
    case EV_APP_START:
        clear();
        ui_tile("+ - x /");
        break;
    case EV_APP_SHOW:
        show();
        break;
    case EV_APP_LEVEL:
        if (ev->id >= 10 && ev->id <= 13 && ev->value >= 0 && ev->value < 4) {
            char k = rows[ev->id - 10][ev->value];
            if (k >= '0' && k <= '9')
                digit(k - '0');
            else if (k == '.')
                point();
            else if (k == '=')
                equals();
            else
                operate(k);
        }
        show();
        break;
    case EV_APP_BUTTON:
        if (ev->id == 1)
            clear();
        else if (ev->id == 2)
            backspace();
        show();
        break;
    default:
        break;
    }
}
