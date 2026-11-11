/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 1.2-alpha8: wake-up alarms. The Clock app keeps the list (its saved state) and gives it to the kernel at
 * every start (SYS_ALARM_SET); the kernel checks it on the minute tick (KT_CLOCK), and at a match: the screen on,
 * Clock to the front on its ringing page (EV_APP_SERVICE id 3), the motor buzzing every 2.5 s until Stop or Snooze
 * (SYS_ALARM_CANCEL 0 / 1, or the Power key: snooze), at most 10 minutes. The phone does not switch itself off
 * while an alarm is set (power.c's auto-off asks alarm_any()). */
#include "sb.h"
#include "kern.h"

#define NALARM 4
#define SNOOZE_MIN 9
#define RING_MAX_MS 600000U
#define RING_EVERY_MS 2500U

static struct {
    uint8_t on, h, m, days;                 /* days: bit 0 Sunday .. bit 6 Saturday; 0 once */
} al[NALARM];
static int ringing = -1;                    /* the alarm ringing (or -1) */
static uint64_t ring_t0;
static int64_t snooze_utc;                  /* a snoozed alarm rings again at this time (0 none) */
static int snooze_idx = -1;

int alarm_set(int i, int h, int m, int days, int on)
{
    if (i < 0 || i >= NALARM || h < 0 || h > 23 || m < 0 || m > 59 || days < 0 || days > 0x7f)
        return SBOS_EINVAL;
    al[i].on = (uint8_t)!!on;
    al[i].h = (uint8_t)h;
    al[i].m = (uint8_t)m;
    al[i].days = (uint8_t)days;
    if (!on && snooze_idx == i) {
        snooze_idx = -1;
        snooze_utc = 0;
    }
    return 0;
}

/* P2y (Next's lock widget): the earliest alarm that is on, by time of day; 1 found, 0 none. Read only. */
int alarm_first(int *h, int *m)
{
    int best = -1;
    for (int i = 0; i < NALARM; ++i)
        if (al[i].on && (best < 0 || al[i].h * 60 + al[i].m < al[best].h * 60 + al[best].m))
            best = i;
    if (best < 0)
        return 0;
    *h = al[best].h;
    *m = al[best].m;
    return 1;
}

int alarm_any(void)
{
    for (int i = 0; i < NALARM; ++i)
        if (al[i].on)
            return 1;
    return snooze_idx >= 0;
}

int alarm_ringing(void)
{
    return ringing;
}

static void ring(int i, const char *why)
{
    int a = app_by_id("clock");
    ringing = i;
    ring_t0 = sb_ms();
    struct ub u;
    ub_init(&u);
    ub_s(&u, "alarm: ");
    ub_s(&u, why);
    ub_s(&u, " ");
    ub_dec2(&u, al[i].h);
    ub_c(&u, ':');
    ub_dec2(&u, al[i].m);
    sb_log(u.b);
    power_screen_set(1);
    power_input();
    if (a >= 0) {
        kui_open_app(a);
        app_event_send(a, EV_APP_SERVICE, 3, i | al[i].h << 8 | al[i].m << 16);
    }
    (void)vib_buzz(3, 500, 300);
    kt_set(KT_CLOCK, 1, sb_ms(), RING_EVERY_MS, 0);
}

/* the minute tick: a match rings (once a minute at most: the tick is at the minute's start) */
void alarm_minute(const struct sbos_time *t)
{
    if (!t || !t->utc || ringing >= 0)
        return;
    if (snooze_idx >= 0 && t->utc >= snooze_utc) {
        int i = snooze_idx;
        snooze_idx = -1;
        snooze_utc = 0;
        ring(i, "snoozed alarm again");
        return;
    }
    for (int i = 0; i < NALARM; ++i) {
        if (!al[i].on || al[i].h != t->h || al[i].m != t->mi)
            continue;
        if (al[i].days && !(al[i].days & (1U << (t->wday % 7))))
            continue;
        if (!al[i].days)
            al[i].on = 0;                   /* once: off (the app is told by the ringing event and saves it) */
        ring(i, "ringing");
        return;
    }
}

/* KT_CLOCK 1 while it rings: buzz again, or end after 10 minutes */
void alarm_timer(void)
{
    if (ringing < 0)
        return;
    if (sb_ms() - ring_t0 >= RING_MAX_MS) {
        sb_log("alarm: 10 minutes without an answer: stopped");
        {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "Not answered: ");
            ub_dec2(&u, al[ringing].h);
            ub_c(&u, ':');
            ub_dec2(&u, al[ringing].m);
            notif_post("clock", "Missed alarm", u.b);
        }
        alarm_stop(0);
        return;
    }
    power_input();                          /* the screen stays on while it rings */
    (void)vib_buzz(3, 500, 300);
    kt_set(KT_CLOCK, 1, sb_ms(), RING_EVERY_MS, 0);
}

/* 0 stop, 1 snooze 9 minutes */
int alarm_stop(int snooze)
{
    if (ringing < 0)
        return SBOS_EINVAL;
    int i = ringing;
    ringing = -1;
    kt_cancel(KT_CLOCK, 1);
    vib_off();
    if (snooze) {
        struct sbos_time t;
        if (clock_now(&t) && t.utc) {
            snooze_idx = i;
            snooze_utc = t.utc + SNOOZE_MIN * 60 - t.s;
        }
        sb_log("alarm: snoozed 9 minutes");
    } else {
        sb_log("alarm: stopped");
    }
    int a = app_by_id("clock");
    if (a >= 0)
        app_event_send(a, EV_APP_SERVICE, 4, snooze);
    return 0;
}
