/* SPDX-License-Identifier: GPL-2.0-only
 * sblinkd host build: the injector records events instead of calling SendInput (test_host.c reads them).
 */
#include "host.h"

#include <stdio.h>
#include <string.h>

#define REC_MAX 8192
static plat_mutex rk;
static int rinit, rprint;
static sbl_ev rev[REC_MAX];
static int rn;
static int32_t vx, vy, vw, vh;

static void once(void) { if (!rinit) { rinit = 1; plat_mutex_init(&rk); } }

void rec_clear(void) { once(); plat_mutex_lock(&rk); rn = 0; plat_mutex_unlock(&rk); }
void rec_set_print(int on) { rprint = on; }
void rec_set_virt(int32_t x, int32_t y, int32_t w, int32_t h) { vx = x; vy = y; vw = w; vh = h; }

int rec_get(sbl_ev *out, int max)
{
    once();
    plat_mutex_lock(&rk);
    int n = rn < max ? rn : max;
    memcpy(out, rev, (size_t)n * sizeof *out);
    plat_mutex_unlock(&rk);
    return n;
}

static sbl_inj_stats rst;

int sbl_inject(const sbl_ev *ev, int n)
{
    once();
    plat_mutex_lock(&rk);
    for (int i = 0; i < n; i++) {
        if (rn < REC_MAX) rev[rn++] = ev[i];
        if (rprint)
            sbl_log("inject: kind %u code 0x%x flags %u x %d y %d data %d", ev[i].kind, ev[i].code, ev[i].flags,
                    ev[i].x, ev[i].y, ev[i].data);
    }
    rst.calls++;
    rst.events += (uint32_t)n;
    rst.injected += (uint32_t)n;
    snprintf(rst.desk, sizeof rst.desk, "host");
    plat_mutex_unlock(&rk);
    return n;
}

void sbl_inject_take_stats(sbl_inj_stats *out)
{
    once();
    plat_mutex_lock(&rk);
    *out = rst;
    rst.calls = rst.events = rst.injected = rst.short_calls = rst.first_err = rst.last_err = 0;
    rst.desk_moves = rst.desk_fails = 0;
    plat_mutex_unlock(&rk);
}

void sbl_inject_geom(sbl_geom *g)
{
    if (vw > 0) { g->virt_x = vx; g->virt_y = vy; g->virt_w = vw; g->virt_h = vh; }
    else { g->virt_x = g->out_x; g->virt_y = g->out_y; g->virt_w = g->out_w; g->virt_h = g->out_h; }
}

int sbl_follow_input_desktop(int force) { (void)force; return 0; }
