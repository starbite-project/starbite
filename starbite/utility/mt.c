/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS multi-touch and raw keys (mt.h). */
#include "sb.h"
#include "sbui.h"
#include "ut.h"
#include "mt.h"

void mt_reset(struct mt *m)
{
    for (int i = 0; i < MT_N; ++i)
        m->down[i] = 0;
    m->ignore = (1U << MT_N) - 1U;      /* fingers already on the glass count only after they lift */
    m->keys = sb_keys();                /* keys held now (the one that opened the screen) are not presses */
    m->pressed = m->released = 0;
    for (int i = 0; i < 4; ++i)
        m->kt0[i] = sb_ms();
    m->wakes = ut_saver_wakes;
}

static int lift_all(struct mt *m, struct mt_ev *ev, int max)
{
    int n = 0;
    for (int i = 0; i < MT_N && n < max; ++i) {
        if (!m->down[i])
            continue;
        m->down[i] = 0;
        ev[n].id = i;
        ev[n].st = MT_UP;
        ev[n].x = m->x[i];
        ev[n].y = m->y[i];
        ++n;
    }
    return n;
}

int mt_poll(struct mt *m, struct mt_ev *ev, int max)
{
    ut_service();
    m->pressed = m->released = 0;
    if (m->wakes != ut_saver_wakes) {   /* the saver polled through ui_poll: fingers and keys are stale */
        m->wakes = ut_saver_wakes;
        m->keys = sb_keys();
        m->ignore = (1U << MT_N) - 1U;  /* the waking finger goes nowhere, also when it moves */
        return lift_all(m, ev, max);
    }
    unsigned k = sb_keys();
    if (k & ~m->keys) {                 /* debounce a new press, as sbui_input does */
        sb_mdelay(15);
        k = (sb_keys() & (k & ~m->keys)) | (k & m->keys);
    }
    m->pressed = k & ~m->keys;
    m->released = m->keys & ~k;
    for (int b = 0; b < 4; ++b)
        if (m->pressed & (1U << b))
            m->kt0[b] = sb_ms();
    m->keys = k;
    int id[MT_N], st[MT_N], x[MT_N], y[MT_N];
    int n = sb_touch_events(id, st, x, y, max < MT_N ? max : MT_N), out = 0;
    for (int i = 0; i < n; ++i) {
        int f = id[i];
        if (f < 0 || f >= MT_N)
            continue;
        if (st[i] == MT_DOWN) {
            m->ignore &= ~(1U << f);
        } else if (m->ignore & (1U << f)) {  /* a finger from before: its moves and lift go nowhere */
            if (st[i] == MT_UP)
                m->ignore &= ~(1U << f);
            continue;
        }
        if (st[i] == MT_DOWN && m->down[f] && out < max) {
            ev[out].id = f;                 /* down again without a lift (a lost event): lift it first */
            ev[out].st = MT_UP;
            ev[out].x = m->x[f];
            ev[out].y = m->y[f];
            ++out;
        }
        if (st[i] == MT_DOWN || (st[i] == MT_MOVE && !m->down[f])) {
            m->down[f] = 1;
            m->x0[f] = x[i];
            m->y0[f] = y[i];
            m->t0[f] = sb_ms();
            st[i] = MT_DOWN;
        } else if (st[i] == MT_UP) {
            if (!m->down[f])
                continue;
            m->down[f] = 0;
        }
        if (out >= max)
            break;
        m->x[f] = x[i];
        m->y[f] = y[i];
        ev[out].id = f;
        ev[out].st = st[i];
        ev[out].x = x[i];
        ev[out].y = y[i];
        ++out;
    }
    if (out || m->pressed)
        ui_idle_restart();              /* input: the saver and the idle return count from now */
    return out;
}

uint64_t mt_held_ms(const struct mt *m, unsigned key)
{
    for (int b = 0; b < 4; ++b)
        if (key == (1U << b))
            return (m->keys & key) ? sb_ms() - m->kt0[b] : 0;
    return 0;
}

int mt_count(const struct mt *m)
{
    int n = 0;
    for (int i = 0; i < MT_N; ++i)
        n += m->down[i];
    return n;
}
