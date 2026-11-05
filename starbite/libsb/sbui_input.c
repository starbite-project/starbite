/* SPDX-License-Identifier: GPL-2.0-only
 * sbui input: keys and touch merged into events (after shim/starlte/em.c ev_wait), with volume
 * auto-repeat, Bixby short / long, Power short / long, drag with release velocity for flings,
 * sideways swipes.
 *
 * Power selects when it is released within UI_POWER_LONG_MS; held that long it is "back" instead
 * (EV_BACK, once, nothing on the release). With that, Volume + Power alone reach every page and
 * leave it again: Bixby and touch are conveniences, not requirements.
 */
#include "sbui.h"

#define Q 16
static struct ui_ev q[Q];
static unsigned qh, qt;
static unsigned held, vol_key;
static uint64_t held_t0, vol_next, bixby_t0, last_input;
static int bixby_long;
static uint64_t power_t0;
static int power_armed;              /* Power went down after the last reset: select or back pending */
static int t_down, t_mode, t_x0, t_y0, t_lx, t_ly;     /* t_mode: 0 none, 1 drag, 2 swipe */
static int64_t t_vy;                                    /* px/s, smoothed */
static uint64_t t_lt;

void (*ui_tick)(void);

static void push(int type, int x, int y, int d)
{
    unsigned n = (qh + 1) % Q;
    if (n == qt)
        return;
    q[qh].type = type;
    q[qh].x = x;
    q[qh].y = y;
    q[qh].d = d;
    q[qh].x0 = t_x0;
    q[qh].y0 = t_y0;
    qh = n;
    last_input = sb_ms();
}

unsigned ui_keys(void)
{
    return sb_keys();
}

uint64_t ui_idle_ms(void)
{
    return sb_ms() - last_input;
}

void ui_input_reset(void)
{
    qh = qt = 0;
    held = sb_keys();       /* keys still down are ignored until released */
    vol_key = 0;
    bixby_long = 1;
    power_armed = 0;
    t_down = 0;
    t_mode = 0;
    sb_touch_clear();
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static void poll_keys(void)
{
    unsigned k = sb_keys();
    uint64_t now = sb_ms();
    unsigned pressed = k & ~held, released = held & ~k;
    if (pressed) {
        sb_mdelay(15);                          /* debounce */
        unsigned k2 = sb_keys();
        pressed &= k2;
        k = k2 | (k & held);
    }
    if (pressed & SB_K_UP) {
        push(EV_UP, 0, 0, 0);
        vol_key = SB_K_UP;
        vol_next = now + 420;
    }
    if (pressed & SB_K_DOWN) {
        push(EV_DOWN, 0, 0, 0);
        vol_key = SB_K_DOWN;
        vol_next = now + 420;
    }
    if (pressed & SB_K_POWER) {
        power_t0 = now;
        power_armed = 1;
    }
    if (power_armed && (k & SB_K_POWER) && now - power_t0 >= UI_POWER_LONG_MS) {
        power_armed = 0;
        push(EV_BACK, 0, 0, 1);                 /* d = 1: from a long Power */
    }
    if (power_armed && (released & SB_K_POWER)) {
        power_armed = 0;
        push(EV_SELECT, 0, 0, 0);
    }
    if (pressed & SB_K_BIXBY) {
        bixby_t0 = now;
        bixby_long = 0;
    }
    if (vol_key && (k & vol_key) && now >= vol_next) {
        push(vol_key == SB_K_UP ? EV_UP : EV_DOWN, 0, 0, 1);
        vol_next = now + 90;
    }
    if (vol_key && !(k & vol_key))
        vol_key = 0;
    if ((k & SB_K_BIXBY) && !bixby_long && now - bixby_t0 >= 700) {
        bixby_long = 1;
        push(EV_HOME, 0, 0, 0);
    }
    if ((released & SB_K_BIXBY) && !bixby_long) {
        bixby_long = 1;
        push(EV_BACK, 0, 0, 0);
    }
    if (pressed)
        held_t0 = now;
    held = k;
}

static void poll_touch(void)
{
    int st, x, y;
    for (unsigned n = 0; n < 8 && sb_touch_event(&st, &x, &y); ++n) {
        uint64_t now = sb_us();
        if (st == 1) {
            t_down = 1;
            t_mode = 0;
            t_x0 = t_lx = x;
            t_y0 = t_ly = y;
            t_vy = 0;
            t_lt = now;
            push(EV_PRESS, x, y, 0);
        } else if (st == 2 && t_down) {
            if (!t_mode && iabs(y - t_y0) > 36)
                t_mode = 1;
            else if (!t_mode && iabs(x - t_x0) > 60)
                t_mode = 2;
            if (t_mode == 1 && y != t_ly) {
                uint64_t dt = now - t_lt;
                if (dt > 0 && dt < 200000) {
                    int64_t v = (int64_t)(y - t_ly) * 1000000 / (int64_t)dt;
                    t_vy = (t_vy + 3 * v) / 4;
                }
                push(EV_DRAG, x, y, y - t_ly);
                t_lt = now;
            }
            t_lx = x;
            t_ly = y;
        } else if (st == 3 && t_down) {
            int dx = x - t_x0, dy = y - t_y0;
            t_down = 0;
            if (!t_mode && iabs(dx) < 60 && iabs(dy) < 60)
                push(EV_TAP, t_x0, t_y0, 0);
            else if (t_mode == 1)
                push(EV_RELEASE, x, y, sb_us() - t_lt > 80000 ? 0 : (int)t_vy);
            else if (t_mode == 2 && iabs(dx) > 240 && iabs(dy) < 300)
                push(dx < 0 ? EV_SWIPE_L : EV_SWIPE_R, x, y, dx);
            push(EV_TOUCH_UP, x, y, 0);
            t_mode = 0;
        }
    }
}

void ui_poll(struct ui_ev *e, unsigned timeout_ms)
{
    uint64_t t0 = sb_ms();
    for (;;) {
        if (qt != qh) {
            *e = q[qt];
            qt = (qt + 1) % Q;
            return;
        }
        poll_keys();
        poll_touch();
        if (ui_tick)
            ui_tick();
        if (qt != qh)
            continue;
        if (sb_ms() - t0 >= timeout_ms) {
            e->type = EV_NONE;
            e->d = 0;
            return;
        }
        sb_udelay(2000);
    }
}
