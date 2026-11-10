/* SPDX-License-Identifier: GPL-2.0-only
 * sbkeys' portable core (sbk_core.h). */
#include "sbk_core.h"

#define SBK_MAX_GAP_MS 500U

sbk_u32 sbk_keys(sbk_u32 gpa0_dat, sbk_u32 gpa2_dat)
{
    return (!((gpa0_dat >> 3) & 1U) ? SBK_UP : 0U) | (!((gpa0_dat >> 4) & 1U) ? SBK_DOWN : 0U) |
           (!((gpa0_dat >> 6) & 1U) ? SBK_BIXBY : 0U) | (!((gpa2_dat >> 4) & 1U) ? SBK_POWER : 0U);
}

static int pin_in(sbk_u32 con, unsigned pin)
{
    sbk_u32 f = (con >> (4U * pin)) & 0xFU;
    return f == 0x0U || f == 0xFU;
}

int sbk_pins_ok(sbk_u32 gpa0_con, sbk_u32 gpa2_con)
{
    return pin_in(gpa0_con, 3) && pin_in(gpa0_con, 4) && pin_in(gpa0_con, 6) && pin_in(gpa2_con, 4);
}

void sbk_cfg_fix(struct sbk_cfg *c)
{
    if (c->hold_ms < 1000U || c->hold_ms > 10000U)
        c->hold_ms = 3000U;
    if (c->arm_ms < 500U || c->arm_ms > 10000U)
        c->arm_ms = 2000U;
    c->sas = c->sas ? 1U : 0U;
    if (c->sas_max_ms < 300U || c->sas_max_ms > 3000U)
        c->sas_max_ms = 1500U;
}

void sbk_init(struct sbk_state *s)
{
    s->armed = 0;
    s->up_ms = 0;
    s->bixby_ms = 0;
    s->fired = 0;
    s->sess_ms = 0;
    s->sess_union = 0;
    s->sess_both = 0;
    s->prev = 0;
}

enum sbk_action sbk_step(const struct sbk_cfg *c, struct sbk_state *s, sbk_u32 keys, sbk_u32 dt_ms)
{
    enum sbk_action a = SBK_NONE;
    int gap = dt_ms > SBK_MAX_GAP_MS;

    keys &= SBK_UP | SBK_DOWN | SBK_BIXBY | SBK_POWER;
    if (!s->armed) {
        if (keys) {
            s->up_ms = 0;
        } else if (!gap) {
            s->up_ms += dt_ms;
            if (s->up_ms >= c->arm_ms)
                s->armed = 1;
        }
        s->prev = keys;
        return SBK_NONE;
    }

    /* Bixby alone, held */
    if (keys == SBK_BIXBY && !gap) {
        if (s->prev == SBK_BIXBY)
            s->bixby_ms += dt_ms;
        if (s->bixby_ms >= c->hold_ms && !s->fired) {
            s->fired = 1;
            a = SBK_RESTART;
        }
    } else {
        s->bixby_ms = 0;
        if (!(keys & SBK_BIXBY))
            s->fired = 0;
    }

    /* press sessions: first key down .. all keys up */
    if (keys) {
        if (s->prev)
            s->sess_ms += gap ? c->sas_max_ms + 1U : dt_ms;
        else
            s->sess_ms = 0, s->sess_union = 0, s->sess_both = 0;
        s->sess_union |= keys;
        if ((keys & (SBK_UP | SBK_POWER)) == (SBK_UP | SBK_POWER))
            s->sess_both = 1;
    } else if (s->prev) {
        if (c->sas && a == SBK_NONE && s->sess_union == (SBK_UP | SBK_POWER) && s->sess_both &&
            s->sess_ms <= c->sas_max_ms && !gap)
            a = SBK_SAS;
        s->sess_ms = 0;
        s->sess_union = 0;
        s->sess_both = 0;
    }
    s->prev = keys;
    return a;
}
