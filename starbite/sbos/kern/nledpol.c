/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the notification LED's policy (nledpol.h has the contract). Pure C apart from nled_pattern / nled_why
 * and the log, so the host test (kern/host/nledpol-test.c) drives it with a fake LED. */
#include "sb.h"
#include "nled.h"
#include "nledpol.h"

static struct nledpol_in in;
static int enabled, shown = NLED_OFF, fails;

int nledpol_pick(const struct nledpol_in *s)
{
    if (s->missed)
        return NLED_MISSED_CALL;
    if (s->message)
        return NLED_MESSAGE;
    if (s->bat_ok && s->charger > 0)
        return s->pct >= 100 ? NLED_CHARGED : NLED_CHARGING;
    if (s->bat_ok && s->charger == 0 && s->pct <= NLEDPOL_LOW_PCT)
        return NLED_LOW_BATTERY;
    return NLED_OFF;
}

static void apply(void)
{
    if (!enabled || fails >= 3)
        return;
    int p = nledpol_pick(&in);
    if (p == shown)
        return;
    if (nled_pattern((enum nled_pattern)p)) {
        shown = p;
        fails = 0;
        sb_logf("nledpol: the LED shows: ", nled_pattern_name((enum nled_pattern)p));
        return;
    }
    if (++fails >= 3)
        sb_logf("nledpol: the LED did not take three patterns in a row: the policy stops until it is turned on again: ",
                nled_why());
    else
        sb_logf("nledpol: the LED did not take the pattern: ", nled_why());
}

void nledpol_enable(int on)
{
    if (on) {
        if (!enabled)
            sb_log("nledpol: on (missed call, message, charging, charged, low battery)");
        enabled = 1;
        fails = 0;                          /* a stopped policy tries again */
        apply();
        return;
    }
    if (!enabled)
        return;
    enabled = 0;
    if (shown != NLED_OFF)
        (void)nled_off();
    shown = NLED_OFF;
    sb_log("nledpol: off");
}

void nledpol_battery(int ok, int pct, int charger)
{
    in.bat_ok = !!ok;
    in.pct = pct;
    in.charger = charger;
    apply();
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

void nledpol_notified(const char *who, int quiet)
{
    if (quiet || (who && same(who, "battery")))
        return;                             /* the Centre only / the battery's own pattern says it */
    in.message = 1;
    apply();
}

void nledpol_event(int what)
{
    if (what == NLEDPOL_MISSED)
        in.missed = 1;
    else if (what == NLEDPOL_MESSAGE)
        in.message = 1;
    else
        return;
    apply();
}

void nledpol_screen(int on)
{
    if (!on || (!in.missed && !in.message))
        return;
    in.missed = in.message = 0;             /* the lock screen shows them: seen */
    apply();
}

int nledpol_shown(void)
{
    return enabled ? shown : NLED_OFF;
}
