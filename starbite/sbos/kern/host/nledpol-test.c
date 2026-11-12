/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/nledpol.c (Track A: the notification LED's policy) against a fake LED: the priority order
 * (missed call, message, charging, charged, low battery, off), nothing written while disabled or unchanged, the
 * screen clearing the seen events, the quiet and battery posts ignored, three failures stopping it, the LED off when
 * disabled. -v lists every check. */
#include <stdio.h>
#include <string.h>
#include "nled.h"
#include "nledpol.h"

static int verbose, checks, failed;
#define CHECK(c, what) do { ++checks; if (!(c)) { ++failed; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } \
                            else if (verbose) printf("ok   %s\n", what); } while (0)

/* the fake LED */
static int writes, offs, last = -1, fail_next;
int nled_pattern(enum nled_pattern p)
{
    ++writes;
    if (fail_next > 0) { --fail_next; return 0; }
    last = (int)p;
    return 1;
}
int nled_off(void) { ++offs; last = NLED_OFF; return 1; }
const char *nled_why(void) { return "the fake LED refused"; }
const char *nled_pattern_name(enum nled_pattern p)
{
    static const char *const n[NLED_NPATTERNS] = { "Off", "Missed call", "New message", "Charging", "Charged",
                                                   "Low battery", "Charging problem" };
    return (unsigned)p < NLED_NPATTERNS ? n[p] : "?";
}
void sb_log(const char *s) { if (verbose) printf("     log: %s\n", s); }
void sb_logf(const char *a, const char *b) { if (verbose) printf("     log: %s%s\n", a, b); }

static int pick(int missed, int message, int ok, int pct, int charger)
{
    struct nledpol_in in = { missed, message, ok, pct, charger };
    return nledpol_pick(&in);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    /* the pure choice */
    CHECK(pick(1, 1, 1, 50, 1) == NLED_MISSED_CALL, "a missed call wins over a message and charging");
    CHECK(pick(0, 1, 1, 50, 1) == NLED_MESSAGE, "a message wins over charging");
    CHECK(pick(0, 1, 1, 5, 0) == NLED_MESSAGE, "a message wins over a low battery");
    CHECK(pick(0, 0, 1, 50, 1) == NLED_CHARGING, "charging");
    CHECK(pick(0, 0, 1, 100, 1) == NLED_CHARGED, "charged at 100 %");
    CHECK(pick(0, 0, 1, 10, 1) == NLED_CHARGING, "low but charging: charging");
    CHECK(pick(0, 0, 1, 15, 0) == NLED_LOW_BATTERY, "15 % on battery: low");
    CHECK(pick(0, 0, 1, 16, 0) == NLED_OFF, "16 % on battery: off");
    CHECK(pick(0, 0, 1, 5, -1) == NLED_OFF, "the charger unknown: neither charging nor low");
    CHECK(pick(0, 0, 0, 5, 0) == NLED_OFF, "the battery not read: off");
    CHECK(pick(1, 0, 0, 0, 0) == NLED_MISSED_CALL, "a missed call without a battery reading");

    /* disabled: inputs are kept, nothing is written */
    nledpol_battery(1, 50, 1);
    nledpol_event(NLEDPOL_MESSAGE);
    CHECK(writes == 0 && nledpol_shown() == NLED_OFF, "disabled: nothing written");
    nledpol_screen(1);                                  /* the message seen before the policy came on */

    /* on: the state as it is now, written once */
    nledpol_enable(1);
    CHECK(writes == 1 && last == NLED_CHARGING && nledpol_shown() == NLED_CHARGING, "on: charging written");
    nledpol_battery(1, 51, 1);
    nledpol_battery(1, 52, 1);
    CHECK(writes == 1, "unchanged: no write");
    nledpol_notified("Mail", 0);
    CHECK(last == NLED_MESSAGE && writes == 2, "a post: the message pattern");
    nledpol_event(NLEDPOL_MISSED);
    CHECK(last == NLED_MISSED_CALL && writes == 3, "a missed call over the message");
    nledpol_screen(0);
    CHECK(writes == 3, "the screen going off changes nothing");
    nledpol_screen(1);
    CHECK(last == NLED_CHARGING && writes == 4, "the screen on: seen, back to charging");
    nledpol_screen(1);
    CHECK(writes == 4, "the screen on again: no write");
    nledpol_notified("Mail", 1);
    CHECK(writes == 4, "a quiet post: ignored");
    nledpol_notified("battery", 0);
    CHECK(writes == 4, "the battery's own post: ignored");
    nledpol_event(99);
    CHECK(writes == 4, "an unknown event: ignored");
    nledpol_battery(1, 100, 1);
    CHECK(last == NLED_CHARGED && writes == 5, "full: charged");
    nledpol_battery(1, 15, 0);
    CHECK(last == NLED_LOW_BATTERY && writes == 6, "unplugged at 15 %: low battery");
    nledpol_battery(1, 40, 1);
    nledpol_battery(1, 60, 0);
    CHECK(last == NLED_OFF && writes == 8, "unplugged at 60 %: off");

    /* failures: three in a row stop it until it is turned on again */
    fail_next = 3;
    nledpol_battery(1, 60, 1);
    nledpol_battery(1, 61, 1);
    nledpol_battery(1, 62, 1);
    CHECK(writes == 11 && last == NLED_OFF, "three failed writes");
    nledpol_battery(1, 63, 1);
    CHECK(writes == 11, "stopped: no fourth try");
    nledpol_enable(1);
    CHECK(writes == 12 && last == NLED_CHARGING, "turned on again: it tries again");

    /* off: the LED off when this lit it, then nothing */
    nledpol_enable(0);
    CHECK(offs == 1 && last == NLED_OFF && nledpol_shown() == NLED_OFF, "off: the LED off");
    nledpol_event(NLEDPOL_MISSED);
    CHECK(writes == 12, "off: nothing written");
    nledpol_enable(0);
    CHECK(offs == 1, "off twice: one LED off");

    printf("nledpol-test: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
