/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS 1.2-alpha8: wake-up alarms. The Clock app keeps the list (its saved state) and gives it to the kernel at
 * every start (SYS_ALARM_SET); the kernel checks it on the minute tick (KT_CLOCK), and at a match: the screen on,
 * Clock to the front on its ringing page (EV_APP_SERVICE id 3), the motor buzzing every 2.5 s until Stop or Snooze
 * (SYS_ALARM_CANCEL 0 / 1, or the Power key: snooze), at most 10 minutes. The phone does not switch itself off
 * while an alarm is set (power.c's auto-off asks alarm_any()). */
#include "sb.h"
#include "kern.h"

#define NALARM ALARM_SLOTS             /* 8a: 32 (was 4); the classic Clock app's state still covers the first four */
#define SNOOZE_MIN 9
#define RING_MAX_MS 600000U
#define RING_EVERY_MS 2500U

static struct {
    uint8_t on, h, m, days;                 /* days: bit 0 Sunday .. bit 6 Saturday; 0 once */
} al[NALARM];
/* Track A (2026-11-12): Next's Alarm tab (ui/shell.h sh_alarm_*): which of the four slots are in its list, and their
 * labels. Kept in the kernel's own record "sbos.alarm" (settings.c); the classic Clock app's saved state is kept the
 * same as the list (alarm_app_state), so its push at every start gives the kernel back what it had. */
#define LABEL_MAX 20
static uint8_t used[NALARM];
static char label[NALARM][LABEL_MAX];
static int kept;                            /* the list came from "sbos.alarm" (or was edited in Next) */
static int ringing = -1;                    /* the alarm ringing (or -1) */
/* cp7x (Track A): the alarm's sound. The Next shell plays snd's ALARM tone (kern_next.c -> shell_lock.c's
 * sh_alarm_sound, the shell's ring owner 4) from the ring until Stop / Snooze / the 10 minutes; other shells: none */
__attribute__((weak)) void alarm_sound(int on) { (void)on; }
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
    if (on)
        used[i] = 1;                        /* Track A: an alarm the classic app turned on is in Next's list too */
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
    alarm_sound(1);
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
    alarm_sound(0);
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

/* ---- Track A: the Alarm tab's list (ui/kern_next.c sh_alarm_*) ------------------------------------------------- */
static int slot_of(int i)                   /* the i-th alarm of the list (slot order), or -1 */
{
    for (int k = 0; k < NALARM; ++k)
        if (used[k] && i-- == 0)
            return k;
    return -1;
}

int alarm_count(void)
{
    int n = 0;
    for (int k = 0; k < NALARM; ++k)
        n += used[k] != 0;
    return n;
}

int alarm_get(int i, int *hm, int *days, int *on, char *lab, int cap)
{
    int k = slot_of(i);
    if (k < 0)
        return SBOS_EINVAL;
    if (hm)
        *hm = al[k].h * 100 + al[k].m;
    if (days)
        *days = al[k].days;
    if (on)
        *on = al[k].on;
    if (lab && cap > 0)
        sb_strcpy(lab, (uint32_t)cap, label[k]);
    return 0;
}

/* i == alarm_count() adds one (the first free slot); 0, or SBOS_EINVAL (a bad time, or all four in use) */
int alarm_put(int i, int hm, int days, int on, const char *lab)
{
    int n = alarm_count(), k = -1;
    if (i < 0 || i > n || hm < 0 || hm / 100 > 23 || hm % 100 > 59 || days < 0 || days > 0x7f)
        return SBOS_EINVAL;
    if (i == n) {
        for (int j = 0; j < NALARM && k < 0; ++j)
            if (!used[j])
                k = j;
    } else {
        k = slot_of(i);
    }
    if (k < 0)
        return SBOS_EINVAL;
    if (ringing == k && (al[k].h * 100 + al[k].m != hm || !on))
        alarm_stop(0);                      /* the ringing alarm changed: it stops */
    if (snooze_idx == k && al[k].h * 100 + al[k].m != hm) {
        snooze_idx = -1;                    /* a new time: the snooze of the old one is gone */
        snooze_utc = 0;
    }
    int r = alarm_set(k, hm / 100, hm % 100, days, on);
    if (r)
        return r;
    used[k] = 1;
    sb_strcpy(label[k], LABEL_MAX, lab ? lab : "");
    kept = 1;
    return 0;
}

int alarm_del(int i)
{
    int k = slot_of(i);
    if (k < 0)
        return SBOS_EINVAL;
    if (ringing == k)
        alarm_stop(0);
    (void)alarm_set(k, al[k].h, al[k].m, al[k].days, 0);   /* off (and its snooze gone) */
    used[k] = 0;
    label[k][0] = 0;
    kept = 1;
    return 0;
}

/* the record "sbos.alarm": "ALK1", then per slot used, on, h, m, days, 3 spare, the label (20) */
#define REC_MAGIC 0x314b4c41U
int alarm_pack(uint8_t out[ALARM_REC_SIZE])
{
    memset(out, 0, ALARM_REC_SIZE);
    uint32_t m = REC_MAGIC;
    memcpy(out, &m, 4);
    for (int k = 0; k < NALARM; ++k) {
        uint8_t *p = out + 4 + k * 28;
        p[0] = used[k];
        p[1] = al[k].on;
        p[2] = al[k].h;
        p[3] = al[k].m;
        p[4] = al[k].days;
        memcpy(p + 8, label[k], LABEL_MAX);
    }
    return kept;
}

void alarm_unpack(const uint8_t *in, unsigned len)
{
    uint32_t m;
    if (len < 4)
        return;
    memcpy(&m, in, 4);
    if (m != REC_MAGIC)
        return;
    int slots = (int)((len - 4) / 28);      /* 4 in a record from before 8a */
    for (int k = 0; k < NALARM && k < slots; ++k) {
        const uint8_t *p = in + 4 + k * 28;
        if (p[2] > 23 || p[3] > 59 || p[4] > 0x7f)
            continue;
        used[k] = p[0] != 0;
        al[k].on = (uint8_t)(p[1] != 0);
        al[k].h = p[2];
        al[k].m = p[3];
        al[k].days = p[4];
        memcpy(label[k], p + 8, LABEL_MAX);
        label[k][LABEL_MAX - 1] = 0;
    }
    kept = 1;
}

/* the classic Clock app's saved state (apps/clock.c: "ALM1", then on, h, m, days per slot) as the list is now; 0 when
 * the kernel has no list of its own yet (the app's state is left as it is) */
int alarm_app_state(uint8_t out[20])
{
    if (!kept)
        return 0;
    uint32_t m = 0x314d4c41U;
    memcpy(out, &m, 4);
    for (int k = 0; k < 4; ++k) {             /* the classic app knows four */
        out[4 + 4 * k] = al[k].on;
        out[5 + 4 * k] = al[k].h;
        out[6 + 4 * k] = al[k].m;
        out[7 + 4 * k] = al[k].days;
    }
    return 20;
}
