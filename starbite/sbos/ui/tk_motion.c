/* SPDX-License-Identifier: MIT
 * sbtk motion: the concept's springs and velocity tracker, line for line (index.html: SPR, spring(), tracker()).
 * Mass 1; semi-implicit Euler in 4 substeps per tick; dt capped at 0.05 s; at rest when |x - to| < eps and
 * |v| < 20 eps, eps = max(1e-3, |to - from| x 0.002). Velocities in units per second. */
#include "sbtk.h"

static const float SPR[TK_NSPR][2] = {
    [TK_SPR_UNLOCK] = { 320, 32 }, [TK_SPR_OPEN] = { 380, 34 }, [TK_SPR_CLOSE] = { 360, 30 },
    [TK_SPR_SHEET] = { 420, 36 }, [TK_SPR_ROWS] = { 300, 30 }, [TK_SPR_CC] = { 400, 36 },
};

void tk_spring_start(tk_spring *s, float from, float to, float v0, int kind)
{
    s->x = from;
    s->v = v0;
    s->to = to;
    s->k = SPR[kind][0];
    s->c = SPR[kind][1];
    s->eps = tk_maxf(1e-3f, tk_fabsf(to - from) * 0.002f);
    s->live = 1;
}

int tk_spring_step(tk_spring *s, float dt)
{
    if (!s->live) return 0;
    dt = tk_minf(dt, 0.05f);
    for (int i = 0; i < 4; i++) {
        float h = dt / 4, f = -s->k * (s->x - s->to) - s->c * s->v;
        s->v += f * h;
        s->x += s->v * h;
    }
    if (tk_fabsf(s->x - s->to) < s->eps && tk_fabsf(s->v) < s->eps * 20) {
        s->x = s->to;
        s->v = 0;
        s->live = 0;
        return 0;
    }
    return 1;
}

void tk_tracker_reset(tk_tracker *t) { t->n = 0; }
void tk_tracker_add(tk_tracker *t, float y, float t_ms)
{
    if (t->n == 64) {
        for (int i = 1; i < 64; i++) { t->y[i - 1] = t->y[i]; t->t[i - 1] = t->t[i]; }
        t->n = 63;
    }
    t->y[t->n] = y;
    t->t[t->n] = t_ms;
    t->n++;
    while (t->n > 2 && t_ms - t->t[0] > 100) {
        for (int i = 1; i < t->n; i++) { t->y[i - 1] = t->y[i]; t->t[i - 1] = t->t[i]; }
        t->n--;
    }
}
float tk_tracker_v(const tk_tracker *t)
{
    if (t->n < 2) return 0;
    return (t->y[t->n - 1] - t->y[0]) / tk_maxf(1, t->t[t->n - 1] - t->t[0]);
}
