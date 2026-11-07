/* SPDX-License-Identifier: GPL-2.0-only
 * The Utility OS calculator's math (calc.c is the UI): no libm on bare metal, so sqrt is the FSQRT
 * instruction and exp, ln, sin, cos, pow are written here with range reduction; the expression parser
 * (precedence, parentheses, unary minus, functions in degrees, π, e) and a 12-significant-digit formatter.
 * Also built by the host test (starbite/tools/ut-host-test.c). */
#include "sb.h"
#include "calc.h"

/* ---- math -------------------------------------------------------------------------------------- */
#define PI 3.14159265358979323846
#define LN2 0.69314718055994530942

double cm_sqrt(double x) { return x < 0 ? 0.0 / 0.0 : __builtin_sqrt(x); }

double cm_exp(double x)
{
    if (x > 709)
        return 1.0 / 0.0;
    if (x < -745)
        return 0;
    int k = (int)(x / LN2 + (x < 0 ? -0.5 : 0.5));    /* x = k ln2 + r, |r| <= ln2/2 */
    double r = x - k * LN2, t = 1, s = 1;
    for (int i = 1; i < 25; ++i) {
        t *= r / i;
        s += t;
    }
    while (k > 0) { s *= 2; --k; }
    while (k < 0) { s /= 2; ++k; }
    return s;
}

double cm_ln(double x)
{
    if (x < 0)
        return 0.0 / 0.0;
    if (x == 0)
        return -1.0 / 0.0;
    int k = 0;
    while (x > 1.5) { x /= 2; ++k; }
    while (x < 0.75) { x *= 2; --k; }
    double z = (x - 1) / (x + 1), z2 = z * z, t = z, s = 0;   /* ln x = 2 atanh z */
    for (int i = 1; i < 60; i += 2) {
        s += t / i;
        t *= z2;
    }
    return 2 * s + k * LN2;
}

static double reduce(double x)                           /* to [-π, π] */
{
    double n = (double)(long long)(x / (2 * PI));
    x -= n * 2 * PI;
    if (x > PI) x -= 2 * PI;
    if (x < -PI) x += 2 * PI;
    return x;
}

double cm_sin(double x)
{
    x = reduce(x);
    double t = x, s = x, x2 = x * x;
    for (int i = 1; i < 14; ++i) {
        t *= -x2 / ((2 * i) * (2 * i + 1));
        s += t;
    }
    return s;
}

double cm_cos(double x) { return cm_sin(x + PI / 2); }

double cm_pow(double a, double b)
{
    if (b == (double)(long long)b && b >= -64 && b <= 64) {   /* integer powers exactly */
        long long n = (long long)b;
        double r = 1, p = a;
        int neg = n < 0;
        if (neg) n = -n;
        while (n) {
            if (n & 1) r *= p;
            p *= p;
            n >>= 1;
        }
        return neg ? 1 / r : r;
    }
    if (a <= 0)
        return a == 0 ? 0 : 0.0 / 0.0;
    return cm_exp(b * cm_ln(a));
}

/* ---- the expression ---------------------------------------------------------------------------- */
static const char *ep;
static int eerr;

static double expr(void);

static void skip(void) { while (*ep == ' ') ++ep; }

static int take(const char *w)
{
    skip();
    unsigned n = 0;
    while (w[n]) ++n;
    for (unsigned i = 0; i < n; ++i)
        if (ep[i] != w[i])
            return 0;
    ep += n;
    return 1;
}

static double number(void)
{
    double v = 0, f = 0.1;
    int any = 0;
    while (*ep >= '0' && *ep <= '9') { v = v * 10 + (*ep++ - '0'); any = 1; }
    if (*ep == '.') {
        ++ep;
        while (*ep >= '0' && *ep <= '9') { v += (*ep++ - '0') * f; f /= 10; any = 1; }
    }
    if (!any)
        eerr = 1;
    return v;
}

static double atom(void)
{
    skip();
    if (take("(")) {
        double v = expr();
        if (!take(")"))
            eerr = 1;
        return v;
    }
    if (take("-")) return -atom();
    if (take("pi") || take("\xcf\x80")) return PI;
    if (take("e")) return 2.71828182845904523536;
    if (take("sqrt")) return cm_sqrt(atom());
    if (take("sin")) return cm_sin(atom() * PI / 180);
    if (take("cos")) return cm_cos(atom() * PI / 180);
    if (take("tan")) { double a = atom() * PI / 180; return cm_sin(a) / cm_cos(a); }
    if (take("ln")) return cm_ln(atom());
    if (take("log")) return cm_ln(atom()) / 2.30258509299404568402;
    return number();
}

static double power(void)
{
    double a = atom();
    skip();
    if (take("^"))
        return cm_pow(a, power());                       /* right associative */
    return a;
}

static double term(void)
{
    double a = power();
    for (;;) {
        if (take("*") || take("\xc3\x97")) a *= power();
        else if (take("/") || take("\xc3\xb7")) a /= power();
        else return a;
    }
}

static double expr(void)
{
    double a = term();
    for (;;) {
        if (take("+")) a += term();
        else if (take("-")) a -= term();
        else return a;
    }
}

int calc_eval(const char *s, double *out)
{
    ep = s;
    eerr = 0;
    double v = expr();
    skip();
    if (*ep)
        eerr = 1;
    *out = v;
    return eerr ? -1 : 0;
}

/* 12 significant digits; scientific outside 1e-6 .. 1e12 */
void calc_format(double v, char *out, unsigned cap)
{
    struct ub u;
    ub_init(&u);
    if (v != v) {
        ub_s(&u, "not a number");
    } else if (v > 1e300 || v < -1e300) {
        ub_s(&u, v < 0 ? "-infinity" : "infinity");
    } else {
        if (v < 0) {
            ub_c(&u, '-');
            v = -v;
        }
        int ex = 0;
        if (v != 0 && (v >= 1e12 || v < 1e-6)) {
            while (v >= 10) { v /= 10; ++ex; }
            while (v < 1) { v *= 10; --ex; }
        }
        int ip = 0;                                       /* digits before the point */
        double t = v;
        while (t >= 1) { t /= 10; ++ip; }
        int lz = 0;                                       /* zeros after the point are not significant */
        if (!ip && v > 0) {
            double z = v;
            while (z < 0.1 && lz < 6) { z *= 10; ++lz; }
        }
        int dec = ip ? 12 - ip : 12 + lz;
        if (dec < 0) dec = 0;
        double r = 0.5;
        for (int i = 0; i < dec; ++i) r /= 10;
        v += r;
        unsigned long long whole = (unsigned long long)v;
        double frac = v - (double)whole;
        ub_dec(&u, whole);
        char fd[20];
        int n = 0;
        for (int i = 0; i < dec && i < 19; ++i) {
            frac *= 10;
            int d = (int)frac;
            frac -= d;
            fd[n++] = (char)('0' + d);
        }
        while (n && fd[n - 1] == '0') --n;
        if (n) {
            ub_c(&u, '.');
            for (int i = 0; i < n; ++i) ub_c(&u, fd[i]);
        }
        if (ex) {
            ub_s(&u, "e");
            ub_sdec(&u, ex);
        }
    }
    sb_strcpy(out, cap, u.b);
}

