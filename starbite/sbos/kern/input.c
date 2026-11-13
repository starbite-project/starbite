/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS input (docs/STARBITE-OS.md 2.1, 3.1): keys and touch read on the input task's timer and turned
 * into kernel input events. v0.1 polls (16 ms with the screen on; only the Power key, every 100 ms, with
 * the screen off); v0.2 moves both to their interrupt lines.
 *
 * Keys: Volume Up / Down move the focus (auto-repeat), Bixby short selects, Bixby held 0.7 s is Back,
 * Power short turns the screen off / on, Power held 1 s opens the power menu. Touch: press, move, release
 * with the position; the UI decides what is a tap or a hold. A key needs two polls in the same state
 * (debounce). */
#include "sb.h"
#include "kern.h"
#include "kinput.h"

#define POWER_LONG_MS 1000U
#define BIXBY_LONG_MS 700U

static struct kin q[32];
static unsigned qh, qt;
static unsigned keys_stable, keys_last, keys_seen;
static uint64_t t_power, t_bixby, vol_next;
static unsigned vol_key;
static int power_long_sent, bixby_long_sent, t_down;
static uint64_t last_input;
int input_fast;                    /* P3k: set by Starbite OS Next: 16 ms reads while the screen is on (no 50 ms first touch) */

void kin_push_id(int type, int id, int x, int y)
{
    unsigned n = (qh + 1) % 32;
    if (n == qt)
        return;
    q[qh].type = type;
    q[qh].x = x;
    q[qh].y = y;
    q[qh].id = id;
    qh = n;
}

void kin_push(int type, int x, int y)
{
    kin_push_id(type, 0, x, y);
}

int kin_pop(struct kin *e)
{
    if (qt == qh)
        return 0;
    *e = q[qt];
    qt = (qt + 1) % 32;
    return 1;
}

uint64_t kin_idle_ms(void)
{
    return sb_ms() - last_input;
}

void kin_touch_held(int *down)
{
    *down = t_down;
}

void input_init(void)
{
    keys_stable = keys_last = sb_keys();
    keys_seen = keys_stable;            /* keys held at start are ignored until released */
    last_input = sb_ms();
    qh = qt = 0;
    t_down = 0;
    sb_touch_clear();
}

static void keys_poll(uint64_t now)
{
    unsigned k = sb_keys();
    /* 0.5.2: with the screen off, Power wakes on its first sight (keys are read every 50 ms then; the two-read
     * debounce made a quick press of under about 200 ms go unseen). Its release is not a second press. */
    if (!K.screen_on && (k & SB_K_POWER) && !(keys_stable & SB_K_POWER) && !(keys_seen & SB_K_POWER)) {
        keys_stable |= SB_K_POWER;
        keys_last = k;
        keys_seen |= SB_K_POWER;            /* held: not pressed again until released */
        t_power = 0;
        last_input = now;
        kin_push(KIN_POWER, 0, 0);
        return;
    }
    unsigned stable = (k == keys_last) ? k : keys_stable;
    keys_last = k;
    unsigned pressed = stable & ~keys_stable, released = keys_stable & ~stable;
    keys_stable = stable;
    pressed &= ~keys_seen;              /* still held from before the start or a reset */
    keys_seen &= stable;
    if (pressed | released)
        last_input = now;
    if (pressed & SB_K_POWER) {
        t_power = now;
        power_long_sent = 0;
        keys_seen |= 0;
    }
    if ((stable & SB_K_POWER) && !power_long_sent && t_power && now - t_power >= POWER_LONG_MS) {
        power_long_sent = 1;
        kin_push(KIN_POWER_LONG, 0, 0);
    }
    if ((released & SB_K_POWER) && t_power) {
        if (!power_long_sent)
            kin_push(KIN_POWER, 0, 0);
        t_power = 0;
    }
    if (pressed & SB_K_BIXBY) {
        t_bixby = now;
        bixby_long_sent = 0;
    }
    if ((stable & SB_K_BIXBY) && t_bixby && !bixby_long_sent && now - t_bixby >= BIXBY_LONG_MS) {
        bixby_long_sent = 1;
        kin_push(KIN_BACK, 0, 0);
    }
    if ((released & SB_K_BIXBY) && t_bixby) {
        if (!bixby_long_sent)
            kin_push(KIN_SELECT, 0, 0);
        t_bixby = 0;
    }
    if (pressed & (SB_K_UP | SB_K_DOWN)) {
        vol_key = pressed & SB_K_UP ? SB_K_UP : SB_K_DOWN;
        kin_push(vol_key == SB_K_UP ? KIN_UP : KIN_DOWN, 0, 0);
        vol_next = now + 420;
    }
    if (vol_key && !(stable & vol_key))
        vol_key = 0;
    if (vol_key && now >= vol_next) {
        kin_push(vol_key == SB_K_UP ? KIN_UP : KIN_DOWN, 0, 0);
        vol_next = now + 120;
    }
}

/* 8a (Track A, multi-touch): every finger's events (sb_touch_events keeps the touch IC's TID), each with its id; finger
 * 0 is the first, as before (the UIs' single touch); the others go to the UI's finger hook (main.c). t_down: any
 * finger down; fdown: which */
static void touch_poll(uint64_t now)
{
    static unsigned fdown;
    int id[16], st[16], x[16], y[16];
    int n = sb_touch_events(id, st, x, y, 16);
    for (int k = 0; k < n; ++k) {
        unsigned bit = id[k] >= 0 && id[k] < 16 ? 1U << id[k] : 0;
        if (!bit)
            continue;
        last_input = now;
        if (st[k] == 1) {
            fdown |= bit;
            kin_push_id(KIN_PRESS, id[k], x[k], y[k]);
        } else if (st[k] == 2 && (fdown & bit)) {
            kin_push_id(KIN_MOVE, id[k], x[k], y[k]);
        } else if (st[k] == 3 && (fdown & bit)) {
            fdown &= ~bit;
            kin_push_id(KIN_RELEASE, id[k], x[k], y[k]);
        }
    }
    t_down = fdown != 0;
}

int input_poll(void)
{
    uint64_t now = sb_ms();
    unsigned before = qh;
    keys_poll(now);
    if (K.screen_on)
        touch_poll(now);
    else
        sb_touch_clear();
    /* 0.5.10: with the screen on, 60 reads a second only while something happens; after 5 s without a touch or a
     * key, 20 (the first touch then waits up to 50 ms), back to 60 at once. Screen off: 20 (services.c). */
    if (K.screen_on) {
        static unsigned period = 16;
        unsigned want = (input_fast || t_down || t_power || t_bixby || vol_key || now - last_input < 5000U) ? 16U : 50U;
        if (want != period) {
            period = want;
            kt_set(KT_INPUT, 1, now, want, 1);
        }
    }
    return qh != before;
}

void input_reset(void)
{
    keys_seen = keys_stable = keys_last = sb_keys();
    vol_key = 0;
    t_power = t_bixby = 0;
    t_down = 0;
    qh = qt = 0;
    sb_touch_clear();
}
