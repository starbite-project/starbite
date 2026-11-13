/* SPDX-License-Identifier: MIT
 * sbtk motion: the concept's springs and velocity tracker (index.html: SPR, spring(), tracker()).
 * Mass 1; semi-implicit Euler in 4 substeps per tick; dt capped at 0.05 s. Velocities in units per second.
 * B4 (the iOS retune, audit "Later"): the constants are iOS's (response, damping ratio) pairs, k = (2 pi / response)^2,
 * c = 4 pi zeta / response (WWDC23 10158). At rest when |x - to| < eps and |v| < 20 eps, eps = max(1e-4, |to - from| x
 * 0.0005): the last frame moves well under a px (0.002 snapped 1.6 pt at the end of an unlock). */
#include "sbtk.h"

static const float SPR[TK_NSPR][2] = {
    [TK_SPR_UNLOCK] = { 246.7f, 31.4f },   /* response .40, zeta 1.0 (was 320 / 32) */
    [TK_SPR_OPEN] = { 195.0f, 27.9f },     /* .45, 1.0: an app grows from its icon (was 380 / 34) */
    [TK_SPR_CLOSE] = { 195.0f, 23.7f },    /* .45, .85: back into the icon with a trace of settle (was 360 / 30) */
    [TK_SPR_SHEET] = { 195.0f, 27.9f },    /* .45, 1.0: sheets (was 420 / 36) */
    [TK_SPR_ROWS] = { 300, 30 },           /* .363, .866: rows and pages (kept) */
    [TK_SPR_CC] = { 246.7f, 31.4f },       /* .40, 1.0 (was 400 / 36) */
    [TK_SPR_PRESS] = { 438.6f, 25.1f },    /* .30, .6: a pressed control's gel (the glass spec's press) */
    [TK_SPR_POP] = { 322.3f, 26.9f },      /* .35, .75: a toast */
    /* docs/design/IOS-REFERENCE.md (v1) 1, 2, 5, 16 */
    [TK_SPR_MORPH] = { 246.7f, 25.1f },    /* .40, .80: the Control Centre's modules, a module's expand morph */
    [TK_SPR_FAST] = { 385.5f, 39.3f },     /* .32, 1.0: the Control Centre's close */
    [TK_SPR_BANNER] = { 195.0f, 22.3f },   /* .45, .80: a banner in (a slight overshoot) */
    [TK_SPR_FAN] = { 246.7f, 26.7f },      /* .40, .85: a stack fans out / folds */
    [TK_SPR_CARD] = { 322.3f, 30.5f },     /* .35, .85: switcher cards close a gap, the home bar's app-to-app slide */
    [TK_SPR_GROW] = { 246.7f, 28.3f },     /* .40, .90: a switcher card grows back to the screen */
};

void tk_spring_start(tk_spring *s, float from, float to, float v0, int kind)
{
    s->x = from;
    s->v = v0;
    s->to = to;
    s->k = SPR[kind][0];
    s->c = SPR[kind][1];
    s->eps = tk_maxf(1e-4f, tk_fabsf(to - from) * 0.0005f);
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

/* 8b: the pinch (sbtk.h): the scale is the fingers' distance over the first (at least 8 pt, so a near-touch start
 * can't explode); its speed from the tracker (the scale x 1000 tracked: tk_tracker_v gives it per second) */
int tk_pinch_touch(tk_pinch *p, int finger, int ph, float x, float y, float t_ms)
{
    if (finger < 0 || finger > 1) return TK_PINCH_NONE;
    p->x[finger] = x; p->y[finger] = y;
    if (ph == 0) p->down[finger] = 1;
    float dx = p->x[1] - p->x[0], dy = p->y[1] - p->y[0], d = tk_sqrtf(dx * dx + dy * dy);
    if (ph == 2) {
        p->down[finger] = 0;
        if (!p->on) return TK_PINCH_NONE;
        p->on = 0;
        p->v = tk_tracker_v(&p->tr);
        return TK_PINCH_ENDED;
    }
    if (!p->down[0] || !p->down[1]) return TK_PINCH_NONE;
    p->cx = (p->x[0] + p->x[1]) / 2; p->cy = (p->y[0] + p->y[1]) / 2;
    if (!p->on) {
        p->on = 1;
        p->d0 = tk_maxf(8, d);
        p->s = 1; p->v = 0;
        p->cx0 = p->cx; p->cy0 = p->cy;
        tk_tracker_reset(&p->tr);
        tk_tracker_add(&p->tr, 1000, t_ms);
        return TK_PINCH_BEGAN;
    }
    p->s = tk_maxf(8, d) / p->d0;
    tk_tracker_add(&p->tr, p->s * 1000, t_ms);
    return TK_PINCH_CHANGED;
}
