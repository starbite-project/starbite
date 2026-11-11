/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS timers: one sorted-by-scan list of deadlines in ms (docs/STARBITE-OS.md 2.2). The kernel
 * programs the hardware timer to kt_next() and sleeps; nothing ticks when nothing is due. Pure C. */
#include "kern.h"

static struct ktimer kt[KT_MAX];

static unsigned prio_of(unsigned owner)
{
    /* heater first, then power, input, the UI, the rest, apps last */
    switch (owner) {
    case KT_HEATER: return 0;
    case KT_POWER: return 1;
    case KT_INPUT: return 2;
    case KT_SOS: return 3;
    case KT_SURV: return 1;             /* 1.2-alpha27: USB power out's 100 ms watch */
    case KT_UI: return 4;
    default: return owner < KT_APP0 ? 5 : 6;
    }
}

void kt_reset(void)
{
    for (unsigned i = 0; i < KT_MAX; ++i)
        kt[i].used = 0;
}

int kt_set(unsigned owner, unsigned id, uint64_t now, uint32_t ms, int repeat)
{
    struct ktimer *f = 0;
    for (unsigned i = 0; i < KT_MAX; ++i) {
        if (kt[i].used && kt[i].owner == owner && kt[i].id == id) {
            f = &kt[i];
            break;
        }
        if (!kt[i].used && !f)
            f = &kt[i];
    }
    if (!f)
        return -1;
    if (!f->used && owner >= KT_APP0) {     /* apps never take the last KT_RESERVE free entries */
        unsigned free_n = 0;
        for (unsigned i = 0; i < KT_MAX; ++i)
            free_n += !kt[i].used;
        if (free_n <= KT_RESERVE)
            return -1;
    }
    if (repeat && ms == 0)
        ms = 1;
    f->at = now + ms;
    f->period = repeat ? ms : 0;
    f->owner = (uint16_t)owner;
    f->id = (uint16_t)id;
    f->prio = (uint8_t)prio_of(owner);
    f->used = 1;
    return 0;
}

void kt_cancel(unsigned owner, unsigned id)
{
    for (unsigned i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].owner == owner && kt[i].id == id)
            kt[i].used = 0;
}

void kt_cancel_owner(unsigned owner)
{
    for (unsigned i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].owner == owner)
            kt[i].used = 0;
}

int kt_pending(unsigned owner, unsigned id)
{
    for (unsigned i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].owner == owner && kt[i].id == id)
            return 1;
    return 0;
}

uint64_t kt_next(void)
{
    uint64_t n = UINT64_MAX;
    for (unsigned i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].at < n)
            n = kt[i].at;
    return n;
}

int kt_pop(uint64_t now, unsigned *owner, unsigned *id)
{
    struct ktimer *b = 0;
    for (unsigned i = 0; i < KT_MAX; ++i) {
        struct ktimer *t = &kt[i];
        if (!t->used || t->at > now)
            continue;
        if (!b || t->at < b->at || (t->at == b->at && t->prio < b->prio))
            b = t;
    }
    if (!b)
        return 0;
    *owner = b->owner;
    *id = b->id;
    if (b->period) {
        b->at += b->period;
        if (b->at <= now)                       /* slept through periods: next one from now */
            b->at = now + b->period - (now - b->at) % b->period;
    } else {
        b->used = 0;
    }
    return 1;
}
