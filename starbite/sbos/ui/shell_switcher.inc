/* SPDX-License-Identifier: MIT
 * Starbite OS Next (helper D): the app switcher, as iOS 26's. Included by shell_lock.c after the other parts.
 *   - recents: the last SW_MAX apps opened from home (not Search or the Linux windows), each with a snapshot of its
 *     page at a quarter of the panel each way (360 x 740, 1.07 MB), taken in idle time while the app rests and kept
 *     until the app is thrown away or falls off the end;
 *   - from an app, a swipe up from the home bar that pauses (held still ~0.2 s), or a slow one past 30 % of the
 *     height, lands the window as the first card (.6 of the screen) with the others to its left; a quick swipe up
 *     still goes home;
 *   - sideways through the cards (snapping to each), a tap opens one (the window grows from the card, its snapshot
 *     over it until the page is drawn), a card thrown up is closed, a tap beside the cards or a swipe up from the home
 *     bar goes home.
 * Nothing is baked: the cards are views of the snapshots, the labels a few small words drawn when it opens. */

/* ---- recents and their snapshots ----------------------------------------------------------------------------- */
#define SW_MAX 4
#define SNAP_W (TK_W / 4)
#define SNAP_H (TK_H / 4)
typedef struct { int a; tk_bmp snap; } sw_ent;
static sw_ent SWR[SW_MAX];
static int swn, sw_dirty = 1;
static int sw_keeps(int a) { const char *k = APPS[a].k; return !lx_is(k) && tk_strcmp(k, "search") && tk_strcmp(k, "linux"); }
static int sw_find(int a) { for (int i = 0; i < swn; i++) if (SWR[i].a == a) return i; return -1; }
/* Track A: the recents (their apps' keys only; snapshots are made again) kept in the blob "recents": version 1, the
 * count, then each key with its 0. Restored cards show the app's ground until their first snapshot. */
static void sw_save(void)
{
    char b[2 + SW_MAX * 24];
    unsigned n = 2;
    b[0] = 1;
    b[1] = (char)swn;
    for (int i = 0; i < swn; i++) {
        unsigned l = (unsigned)tk_strlen(APPS[SWR[i].a].k);
        if (l > 22) l = 22;
        tk_memcpy(b + n, APPS[SWR[i].a].k, l);
        b[n + l] = 0;
        n += l + 1;
    }
    (void)sh_blob_put("recents", b, n);
}
static void sw_restore(void)
{
    char b[2 + SW_MAX * 24];
    int n = sh_blob_get("recents", b, sizeof b - 1);
    if (n < 2 || b[0] != 1 || swn) return;
    b[n] = 0;
    for (int i = 0, at = 2; i < b[1] && at < n && swn < SW_MAX; i++) {
        const char *k = b + at;
        at += (int)tk_strlen(k) + 1;
        for (int a = 0; a < NAPPS; a++)
            if (!tk_strcmp(APPS[a].k, k) && sw_keeps(a) && sw_find(a) < 0) SWR[swn++] = (sw_ent){ a, { 0, 0, 0, 0 } };
    }
    sw_dirty = 1;
}
static void sw_remove(int i)
{
    if (i < 0 || i >= swn) return;
    tk_bmp_free(&SWR[i].snap);
    for (int k = i; k + 1 < swn; k++) SWR[k] = SWR[k + 1];
    swn--;
    SWR[swn].snap = (tk_bmp){ 0, 0, 0, 0 };
    sw_save();                                   /* Track A */
}
/* the open app's page now, a quarter of the panel each way (idle time; the switcher's cards) */
static void sw_snap(void)
{
    if (!AP.on || AP.closing || ap_pending || sp_app.live) return;
    int i = sw_find(AP.a);
    if (i < 0) return;
    if (!SWR[i].snap.px && !tk_bmp_new(&SWR[i].snap, SNAP_W, SNAP_H)) return;
    float ca = appc.alpha, sa = apsb.alpha;
    appc.alpha = apsb.alpha = 1;                 /* as it is at rest */
    tk_flatten(&appg, &SWR[i].snap, 0, .25f);
    appc.alpha = ca; apsb.alpha = sa;
    sw_dirty = 0;
}
/* app_open: the app goes first (a new one pushes the oldest, and its snapshot, off the end) */
static void sw_opened(int a)
{
    if (AP.on && AP.a != a && !AP.closing && !ap_pending) sw_snap();   /* (the host's states switch apps at once) */
    sw_dirty = 1;
    if (!sw_keeps(a)) return;
    int i = sw_find(a);
    sw_ent e = { a, { 0, 0, 0, 0 } };
    if (i < 0) {
        if (swn == SW_MAX) sw_remove(SW_MAX - 1);
        i = swn++;
    } else e = SWR[i];
    for (int k = i; k > 0; k--) SWR[k] = SWR[k - 1];
    SWR[0] = e;
    sw_save();                                   /* Track A */
}

/* ---- the switcher ------------------------------------------------------------------------------------------- */
#define SW_S .6f
#define SW_CW (390 * SW_S)
#define SW_CH (H_PT * SW_S)
#define SW_CY 150.0f
#define SW_PITCH 214.0f
#define SW_R 20.0f
enum { SWG_NONE, SWG_CARD, SWG_BG, SWG_HOME, SWG_SCROLL, SWG_THROW };
static struct {
    int on, closing, live, armed, dragging, g, sl, moved, opening, thrown, cov;
    float p, scroll, base, x0, y0, slow_t0, dy, dyv;
    tk_tracker tr;
} SW = { .sl = -1, .thrown = -1 };
static tk_spring sp_sw, sp_sws;
static tk_layer swg, swdim, swcard[SW_MAX], swlab[SW_MAX], swcover;
static int sw_on(void) { return SW.on; }
static float sw_max_scroll(void) { return swn > 1 ? (swn - 1) * SW_PITCH : 0; }
static arect sw_rect(int i)
{
    float dy = i == SW.thrown ? SW.dy : 0;
    return (arect){ (390 - SW_CW) / 2 - i * SW_PITCH + SW.scroll, SW_CY + dy, SW_CW, SW_CH, SW_R };
}
/* the cards' names: the app's icon (26 pt) and name over each card, drawn once when the switcher opens */
static void sw_labels(void)
{
    for (int i = 0; i < SW_MAX; i++) relayer(&swlab[i]);
    float x = (390 - SW_CW) / 2;
    for (int i = 0; i < swn; i++) {
        int a = SWR[i].a;
        dc d = mk(&swlab[i], "sw-label", x, SW_CY - 36, SW_CW, 30, 2, 0);
        float pad;
        const tk_bmp *ic = app_icon(APPS[a].k, &pad);
        if (!sv_tile(&d, APPS[a].k, x, SW_CY - 34, 26) && ic)
            nc_blit_scaled(d.b, ic, PT(x) - pad * 26 / 62 - d.ox, PT(SW_CY - 34) - pad * 26 / 62 - d.oy, 26.0f / 62);
        d_text_box(&d, TK_IT600, 15, x + 34, bl(TK_IT600, 15, SW_CY - 32), SW_CW - 34, 0, APPS[a].name, 0, 0, INK, 1);
        trim(&swlab[i]);
        tk_layer_prepare(&swlab[i]);
    }
}
static void sw_card_view(int i)
{
    tk_layer *l = &swcard[i];
    if (SWR[i].snap.px) {
        if (l->bmp.px != SWR[i].snap.px) { l->bmp = SWR[i].snap; l->solid = 0; l->gen++; }
    } else if (!l->solid) {
        /* no snapshot yet (closed before an idle turn): its ground */
        l->bmp = (tk_bmp){ 0, 0, 0, 0 };
        l->solid = 0xff000000u | sv_ground(SWR[i].a);
        l->solid_w = TK_W; l->solid_h = TK_H;
        l->gen++;
    }
}
static void sw_place(void)
{
    swg.hidden = !SW.on;
    swdim.solid = tk_premul(TK_RGBA(0, 0, 0, .72f));
    swdim.solid_w = TK_W; swdim.solid_h = TK_H;
    swdim.alpha = tk_clampf(SW.p, 0, 1);
    for (int i = 0; i < SW_MAX; i++) {
        tk_layer *c = &swcard[i], *lb = &swlab[i];
        if (i >= swn || !SW.on) { c->hidden = lb->hidden = 1; continue; }
        arect R = sw_rect(i);
        float a = tk_clampf(SW.p, 0, 1);
        if (!(i == 0 && SW.live)) R.x -= (1 - a) * 60;   /* the others come in from the left */
        float s = PT(R.w) / SNAP_W;
        sw_card_view(i);
        c->sx = c->sy = s;
        c->ox = c->oy = 0;
        c->x = PT(R.x) / s;
        c->y = PT(R.y) / s;
        if (c->solid) { c->sx = c->sy = SW_S; c->x = PT(R.x) / SW_S; c->y = PT(R.y) / SW_S; }
        c->clip = 1;
        c->clip_r = (tk_rect){ PT(R.x), PT(R.y), PT(R.w), PT(R.h) };
        c->clip_radius = PT(SW_R);
        c->alpha = a;
        c->hidden = (i == 0 && SW.live) || a <= .004f;
        if (lb->bmp.px) {
            lb->x = lb->bx + tk_floorf(PT(R.x - (390 - SW_CW) / 2) + .5f);
            lb->y = lb->by + tk_floorf(PT(R.y - SW_CY) + .5f);
            lb->alpha = a;
            lb->hidden = a <= .004f;
        }
    }
    /* the open app is the first card: its window */
    if (SW.on && !SW.closing && !SW.opening && SW.live && AP.on && !sp_app.live && !AP.closing && !SW.dragging) set_app_rect(sw_rect(0));
}
static void sw_free(void)
{
    for (int i = 0; i < SW_MAX; i++) {
        relayer(&swlab[i]); swlab[i].hidden = 1;
        swcard[i].bmp = (tk_bmp){ 0, 0, 0, 0 };   /* a view of a snapshot: never freed here */
        swcard[i].solid = 0;
        swcard[i].hidden = 1;
    }
    swg.hidden = 1;
}
static void sw_show(void)
{
    if (SW.on && !SW.closing) return;
    tk_layer_init(&swg, "switcher");
    tk_layer_init(&swdim, "sw-dim");
    for (int i = 0; i < SW_MAX; i++) { tk_layer_init(&swcard[i], "sw-card"); swcard[i].hidden = 1; }
    sw_labels();
    SW.on = 1; SW.closing = 0;
    SW.scroll = 0; sp_sws.live = 0;
    SW.thrown = -1; SW.dy = 0;
    SW.live = AP.on && !AP.closing && swn && SWR[0].a == AP.a;
    relink();
    tk_spring_start(&sp_sw, SW.p, 1, 0, TK_SPR_CC);
}
static void sw_hide(void)
{
    if (!SW.on) return;
    SW.closing = 1;
    SW.armed = 0;
    tk_spring_start(&sp_sw, SW.p, 0, 0, TK_SPR_CC);
}
static void sw_reset(void)
{
    SW.on = SW.closing = SW.armed = SW.dragging = SW.opening = 0;
    SW.g = SWG_NONE;
    SW.p = 0;
    sp_sw.live = sp_sws.live = 0;
    SW.cov = 0; swcover.hidden = 1; swcover.bmp = (tk_bmp){ 0, 0, 0, 0 };
    sw_free();
}

/* ---- from an app: the home bar's swipe (shell_lock.c's G_APP) ---------------------------------------------- */
static void sw_drag(float travel, float v, float t)
{
    if (locked || !AP.on || !sw_keeps(AP.a)) return;
    SW.dragging = 1;
    if (travel < -40 && tk_fabsf(v) < .3f) { if (SW.slow_t0 <= 0) SW.slow_t0 = t; }
    else {
        SW.slow_t0 = 0;
        if (SW.armed && v < -1.2f) { SW.armed = 0; sw_hide(); }      /* flicked on after all: home */
    }
}
/* the release: 1 if the switcher takes the window */
static int sw_release(float v)
{
    int keep = SW.dragging;
    SW.dragging = 0;
    SW.slow_t0 = 0;
    if (!keep || locked) return 0;
    if (!SW.armed && !(AP.travel < -H_PT * .3f && tk_fabsf(v) < .3f)) {
        if (SW.on) sw_hide();
        return 0;
    }
    SW.armed = 0;
    sw_show();
    SW.live = 1;
    AP.closing = 0;
    AP.from = AP.R;
    AP.to = sw_rect(0);
    AP.to.x = (390 - SW_CW) / 2;
    tk_spring_start(&sp_app, 0, 1, 0, TK_SPR_OPEN);
    return 1;
}
/* set_app_rect while the switcher shows the window, or it grows from a card: the page fully shown */
static int sw_app_alpha(void) { return SW.on || SW.opening; }
/* app_finish_close: the window is gone (thrown away, or home) */
static void sw_app_gone(void) { SW.live = 0; }

/* ---- opening, going home, throwing away ---------------------------------------------------------------------- */
static void sw_open(int i)
{
    if (i < 0 || i >= swn) return;
    arect R = sw_rect(i);
    if (i == 0 && SW.live && AP.on) {
        AP.from = AP.R; AP.to = FULLR;
        AP.closing = 0;
        tk_spring_start(&sp_app, 0, 1, 0, TK_SPR_OPEN);
    } else {
        int a = SWR[i].a;
        if (AP.on) { sp_app.live = 0; app_finish_close(); }
        /* its snapshot over the window while the page is drawn (two strips a frame) */
        if (SWR[i].snap.px) {
            tk_layer_init(&swcover, "sw-cover");
            swcover.bmp = SWR[i].snap;
            SW.cov = 1;
        }
        ap_from_on = 1;
        ap_from = R;
        app_open(a);                              /* (it moves the app first in the recents) */
    }
    SW.opening = 1;
    SW.live = 1;
    sw_hide();
}
static void sw_home(void)
{
    if (SW.live && AP.on) app_close(0);          /* the window into its icon */
    sw_hide();
}
static void sw_throw_done(void)
{
    int i = SW.thrown;
    SW.thrown = -1;
    SW.dy = 0;
    if (i < 0 || i >= swn) return;
    if (i == 0 && SW.live && AP.on) { sp_app.live = 0; app_finish_close(); SW.live = 0; }
    sw_remove(i);
    for (int k = 0; k < SW_MAX; k++) { swcard[k].bmp = (tk_bmp){ 0, 0, 0, 0 }; swcard[k].solid = 0; }
    sw_labels();
    relink();
    if (!swn) { sw_hide(); return; }
    float m = sw_max_scroll();
    if (SW.scroll > m) tk_spring_start(&sp_sws, SW.scroll, m, 0, TK_SPR_ROWS);
}

/* ---- touches (sh_touch: G_SW) ---------------------------------------------------------------------------------- */
static int sw_down(float x, float y, float t)
{
    if (!SW.on || SW.closing) return 0;
    tk_tracker_reset(&SW.tr);
    SW.x0 = x; SW.y0 = y; SW.moved = 0;
    SW.sl = -1;
    for (int i = 0; i < swn; i++) {
        arect R = sw_rect(i);
        if (x >= R.x && x < R.x + R.w && y >= R.y - 40 && y < R.y + R.h) SW.sl = i;
    }
    SW.g = y >= H_PT - 34 ? SWG_HOME : SW.sl >= 0 ? SWG_CARD : SWG_BG;
    sp_sws.live = 0;
    SW.base = SW.scroll;
    tk_tracker_add(&SW.tr, x, t);
    (void)t;
    return 1;
}
static void sw_touch(int ph, float x, float y, float t)
{
    float dx = x - SW.x0, dy = y - SW.y0;
    if (ph == 1) {
        if (!SW.moved && dx * dx + dy * dy > 64) {
            SW.moved = 1;
            if (SW.g == SWG_HOME) ;
            else if (tk_fabsf(dx) > tk_fabsf(dy)) SW.g = SWG_SCROLL;
            else if (SW.g == SWG_CARD && SW.thrown < 0) { SW.g = SWG_THROW; SW.thrown = SW.sl; tk_tracker_reset(&SW.tr); }
        }
        if (SW.g == SWG_SCROLL) {
            tk_tracker_add(&SW.tr, x, t);
            float s = SW.base + dx, m = sw_max_scroll();
            if (s < 0) s = -rubber(-s, 390);
            else if (s > m) s = m + rubber(s - m, 390);
            SW.scroll = s;
            sw_place();
        } else if (SW.g == SWG_THROW) {
            tk_tracker_add(&SW.tr, y, t);
            SW.dy = dy < 0 ? dy : rubber(dy, H_PT) * .4f;
            sw_place();
            if (SW.thrown == 0 && SW.live && AP.on) set_app_rect(sw_rect(0));
        }
        return;
    }
    if (ph != 2) return;
    float v = tk_tracker_v(&SW.tr);
    int g = SW.g;
    SW.g = SWG_NONE;
    if (g == SWG_SCROLL) {
        /* the card the flick would carry to (200 ms), the finger's speed to the spring */
        float m = sw_max_scroll(), to = tk_floorf((SW.scroll + v * 200) / SW_PITCH + .5f) * SW_PITCH;
        to = tk_clampf(to, 0, m);
        int bc = (int)tk_floorf(SW.base / SW_PITCH + .5f);
        to = tk_clampf(to, (bc - 1) * SW_PITCH, (bc + 1) * SW_PITCH);
        tk_spring_start(&sp_sws, SW.scroll, to, v * 1000, TK_SPR_ROWS);
    } else if (g == SWG_THROW) {
        if (SW.dy < -110 || v < -.5f) SW.dyv = v < -.5f ? v : -.5f;   /* away: up and off */
        else SW.dyv = 0;                                               /* back (nc_ease in the tick) */
    } else if (g == SWG_HOME) {
        if (!SW.moved || dy < -30) sw_home();
    } else if (!SW.moved) {
        if (g == SWG_CARD) sw_open(SW.sl);
        else sw_home();
    }
}

/* ---- time ------------------------------------------------------------------------------------------------------ */
static int sw_tick(float dt, float t)
{
    int mv = 0;
    if (SW.dragging && !SW.armed && SW.slow_t0 > 0 && t - SW.slow_t0 >= 180) {
        /* the finger rests: the switcher (the others come in beside the window) */
        SW.armed = 1;
        sh_haptic();
        sw_show();
        SW.live = 1;
    }
    if (SW.dragging) mv = 1;
    if (sp_sw.live) {
        int live = tk_spring_step(&sp_sw, dt);
        SW.p = tk_maxf(0, sp_sw.x);
        if (!live && SW.closing) { SW.on = SW.closing = 0; SW.p = 0; sw_free(); relink(); }
        mv = 1;
    }
    if (sp_sws.live) { tk_spring_step(&sp_sws, dt); SW.scroll = sp_sws.x; mv = 1; }
    if (SW.thrown >= 0 && SW.g != SWG_THROW) {
        if (SW.dyv < 0) {
            SW.dy += SW.dyv * dt * 1000 * 1.6f - 900 * dt;
            if (SW.dy < -SW_CY - SW_CH - 40) sw_throw_done();
        } else {
            SW.dy += (0 - SW.dy) * (1 - tk_expf(-dt * 15));
            if (tk_fabsf(SW.dy) < .3f) { SW.dy = 0; SW.thrown = -1; }
        }
        if (SW.thrown >= 0 && SW.thrown == 0 && SW.live && AP.on) set_app_rect(sw_rect(0));
        mv = 1;
    }
    if (SW.opening && !sp_app.live && !ap_pending) {
        SW.opening = 0;
        if (AP.on && !AP.closing && AP.R.w >= 389) appg.clip = 0;    /* whole: no clip at rest (as an ordinary open) */
    }
    /* the snapshot over a window growing from its card, gone as the page shows */
    if (SW.cov) {
        if (!AP.on || (!sp_app.live && !ap_pending)) { SW.cov = 0; swcover.hidden = 1; swcover.bmp = (tk_bmp){ 0, 0, 0, 0 }; relink(); }
        else {
            arect R = AP.R;
            float s = PT(R.w) / SNAP_W, pw = (R.w - SW_CW) / (390 - SW_CW);
            swcover.sx = swcover.sy = s;
            swcover.x = PT(R.x) / s; swcover.y = PT(R.y) / s;
            swcover.clip = 1;
            swcover.clip_r = (tk_rect){ PT(R.x), PT(R.y), PT(R.w), PT(R.h) };
            swcover.clip_radius = PT(R.r);
            swcover.alpha = ap_pending ? 1 : tk_clampf(1.3f - pw * 1.5f, 0, 1);
            swcover.hidden = swcover.alpha <= .004f;
            mv = 1;
        }
    }
    if (SW.on) sw_place();
    return mv;
}
static int sw_moving(void) { return sp_sw.live || sp_sws.live || SW.g == SWG_SCROLL || SW.g == SWG_THROW || SW.thrown >= 0 || SW.cov || SW.dragging; }
static void sw_idle(void)
{
    if (sw_dirty && AP.on && !SW.on && !SW.opening) sw_snap();
}
static void sw_link(void)
{
    if (!swg.name) { tk_layer_init(&swg, "switcher"); swg.hidden = 1; }
    if (!swcover.name) { tk_layer_init(&swcover, "sw-cover"); swcover.hidden = 1; }
    tk_layer *c[1 + 2 * SW_MAX];
    int n = 0;
    c[n++] = &swdim;
    for (int i = SW_MAX - 1; i >= 0; i--) { c[n++] = &swcard[i]; c[n++] = &swlab[i]; }   /* the older under the newer */
    if (SW.on) link(&swg, c, n);
    else swg.child = 0;
}
/* the host's stills: "switcher" p (the open app as the first card), "swsnap" (the open app's snapshot now) */
static int sw_set(const char *what, const char *key, float v)
{
    (void)key;
    if (!tk_strcmp(what, "swsnap")) { sw_snap(); return 1; }
    if (!tk_strcmp(what, "switcher")) {
        if (v <= 0) { sw_reset(); relink(); return 1; }
        if (AP.on) sw_snap();
        sw_show();
        sp_sw.live = 0;
        SW.p = v;
        sp_app.live = 0;
        if (SW.live && AP.on) { appg.clip = 1; set_app_rect(sw_rect(0)); }
        sw_place();
        return 1;
    }
    return 0;
}
