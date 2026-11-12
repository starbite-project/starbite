/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: who may be called or messaged (docs/MODEM.md 8). Two locks, both needed:
 *   1. never an emergency number: a built-in world list, plus the SIM's and the network's own lists when the caller
 *      has them; also any number that is an emergency number behind an international prefix ("+44999"), and every
 *      short code (fewer than 7 digits), so a forgotten local emergency number is still refused;
 *   2. only a number on the user's own allow-list, compared digit for digit, or (Starbite OS Next) a number the
 *      user typed and confirmed with a hold (sbm_dial_policy.user_confirmed).
 * There is no switch that turns either off. This file builds nothing: the call and SMS frame builders are not part of
 * this core yet, and when they are, they must call sbm_dial_check first.
 */
#include "sbm.h"

/* Emergency and service numbers in use around the world (a superset: refusing too much is the safe direction). */
static const char *const world[] = {
    "112", "911", "999", "000", "08", "110", "118", "119", "100", "101", "102", "103", "104", "108", "15", "17", "18",
    "060", "061", "065", "120", "122", "113", "115", "117", "123", "190", "191", "192", "193", "194", "197", "199",
    "911", "933", "991", "995", "996", "997", "998", "10111", "10177", "112112", "116000", "116111", "116117", "1122",
    "1414", "16000", "112911",
};

/* digits of s (a leading '+' kept as flag); 0 = malformed (letters etc.), else the digit count */
static unsigned norm(const char *s, char *out, unsigned cap, int *plus)
{
    unsigned n = 0;
    *plus = 0;
    for (; s && *s; ++s) {
        char c = *s;
        if (c == ' ' || c == '-' || c == '(' || c == ')' || c == '.')
            continue;
        if (c == '+' && n == 0 && !*plus) {
            *plus = 1;
            continue;
        }
        if (c < '0' || c > '9' || n + 1 >= cap)
            return 0;
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

int sbm_is_emergency(const char *number, const char *const *extra, unsigned n_extra)
{
    char d[40];
    int plus;
    unsigned n = norm(number, d, sizeof(d), &plus);
    if (n == 0)
        return 1;                                   /* not a plain number: treat as refused */
    const char *tail[5] = { d, 0, 0, 0, 0 };
    unsigned nt = 1;
    /* behind an international prefix: +CC<emergency> or 00CC<emergency> (CC 1 to 3 digits) */
    const char *body = plus ? d : (d[0] == '0' && d[1] == '0' ? d + 2 : 0);
    for (unsigned k = 1; body && k <= 3 && body[k]; ++k)
        tail[nt++] = body + k;
    for (unsigned t = 0; t < nt; ++t) {
        for (unsigned i = 0; i < sizeof(world) / sizeof(world[0]); ++i)
            if (eq(tail[t], world[i]))
                return 1;
        for (unsigned i = 0; extra && i < n_extra; ++i) {
            char e[40];
            int ep;
            if (norm(extra[i], e, sizeof(e), &ep) && eq(tail[t], e))
                return 1;
        }
    }
    return 0;
}

int sbm_dial_check(const struct sbm_dial_policy *pol, const char *number)
{
    char d[40], a[40];
    int plus, ap;
    unsigned n = norm(number, d, sizeof(d), &plus);
    if (!pol || n == 0)
        return SBM_EINVAL;
    if (sbm_is_emergency(number, pol->extra_emergency, pol->n_extra))
        return SBM_EPERM;
    if (n < SBM_DIAL_MIN_DIGITS)
        return SBM_EPERM;                            /* short codes: never */
    if (pol->user_confirmed == SBM_DIAL_USER_CONFIRMED)
        return 0;                                    /* the user's own dial (Next's Phone / Messages) */
    for (unsigned i = 0; i < pol->n_allowed && i < SBM_DIAL_ALLOW_MAX; ++i)
        if (norm(pol->allowed[i], a, sizeof(a), &ap) && ap == plus && eq(a, d))
            return 0;
    return SBM_EPERM;
}
