/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: which notification LED pattern shows (nledpol.c, Track A). nled.c drives the LED; this decides, one
 * pattern at a time, in this order: a missed call, an unread message, charging, charged, a low battery, else off.
 *
 * Inputs (each a cheap call; the LED is written only when the winner changes):
 *   nledpol_battery(ok, pct, charger)   the battery service (services.c battery_timer), every read
 *   nledpol_notified(who, quiet)        notif_post_app: a post that is not NOTIF_QUIET and not the battery's own
 *                                       counts as a message
 *   nledpol_event(NLEDPOL_MISSED / NLEDPOL_MESSAGE)   Next's call / SMS watch (ui/kern_next.c comms_now)
 *   nledpol_screen(on)                  the screen on: the missed call / message are seen (they clear); the battery
 *                                       patterns stay
 * Nothing is written until nledpol_enable(1) (Next, after its bake, never in a self-test image): the classic UI and
 * every self-test image keep the LED as they had it. Three failed writes in a row stop the policy until the next
 * nledpol_enable(1). Single-threaded, no locks. */
#ifndef SBOS_NLEDPOL_H
#define SBOS_NLEDPOL_H

#define NLEDPOL_MISSED 1
#define NLEDPOL_MESSAGE 2
#define NLEDPOL_LOW_PCT 15          /* the battery service's own "Battery at 15 %" */

struct nledpol_in {
    int missed, message;            /* not yet seen */
    int bat_ok, pct, charger;       /* charger: 0 none, 1 present, -1 unknown (as struct sbos_battery) */
};
/* the pure choice: an enum nled_pattern value (host-tested) */
int nledpol_pick(const struct nledpol_in *in);

void nledpol_enable(int on);        /* 1 apply from now on; 0 the LED off (if this lit it) and stop */
void nledpol_battery(int ok, int pct, int charger);
void nledpol_notified(const char *who, int quiet);   /* quiet: the post had NOTIF_QUIET */
void nledpol_event(int what);
void nledpol_screen(int on);
int nledpol_shown(void);            /* the pattern on the LED now (NLED_OFF when disabled) */
#endif
