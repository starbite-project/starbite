/* SPDX-License-Identifier: MIT
 * Starbite OS Next (helper D; B4 the deck): the app switcher, as iOS 26's. Included by shell_lock.c after the other parts.
 *   - recents: the last SW_MAX apps opened from home (not Search or the Linux windows), each with a snapshot of its
 *     page at a quarter of the panel each way (360 x 740, 1.07 MB), taken in idle time while the app rests and kept
 *     until the app is thrown away or falls off the end; plus external entries (kind 1, e.g. one per running Linux
 *     app) that a provider adds and removes (sw_ext_add / sw_ext_remove, shell.h);
 *   - from an app, a swipe up from the home bar that pauses (held still ~0.2 s), or a slow one past 30 % of the
 *     height, lands the window as the focused card with the older ones under it to its left; a quick swipe up
 *     still goes home;
 *   - B4, docs/design/IOS-REFERENCE.md 2 (v1.1, measured): the deck. The focused card is .69 of the screen (269 x 553,
 *     centred at y 391, radius 30); the one behind it .65, peeking 70 pt to the left; the one in front .70, 209 pt to
 *     the right and on top. Every card's place, scale and shade follow one continuous position, so a pan moves the
 *     front card with the finger and the ones behind with a parallax. A flick carries on by its speed (several cards)
 *     and snaps on a spring. Each card has its icon (25.5) above it; only the focused one has its name (17 / 600);
 *     the ones behind darken a little;
 *   - a card thrown up leaves with the finger's speed and fades; the ones behind it close the gap on a spring
 *     (.35 / .85); one let go comes back on the same spring; a tap opens one (the window grows from the card, its
 *     snapshot over it until the page is drawn), a tap beside the cards or a swipe up from the home bar goes home;
 *   - B4: the home bar's quick switch: a sideways swipe along the bottom edge of an app slides it away and the
 *     previous (or the next) app in, full screen with a 12 pt gap, following the finger, and lands on the card
 *     spring with the finger's speed. The recents keep their order until the app is used (or the switcher opens),
 *     so a swipe back returns.
 * Nothing is baked: the cards are views of the snapshots, the labels a few small words drawn when it opens. */

/* ---- recents and their snapshots ----------------------------------------------------------------------------- */
#define SW_MAX 4
#define SNAP_W (TK_W / 4)
#define SNAP_H (TK_H / 4)
typedef struct { int a; tk_bmp snap; int kind, subid; } sw_ent;   /* kind 0: a Starbite app (a); 1: external (subid) */
static sw_ent SWR[SW_MAX];
static int swn, sw_dirty = 1;
static int sw_keeps(int a) { const char *k = APPS[a].k; return !lx_is(k) && tk_strcmp(k, "search") && tk_strcmp(k, "linux"); }
static int sw_find(int a) { for (int i = 0; i < swn; i++) if (!SWR[i].kind && SWR[i].a == a) return i; return -1; }

/* ---- external entries (B4): the provider's side. A provider (the Linux session's gwin.c) defines SW_EXT_PROVIDER
 * and these five; without one they are no-ops and nothing is ever added ---- */
#ifdef SW_EXT_PROVIDER
const char *sw_ext_title(int subid);
const tk_bmp *sw_ext_icon(int subid);          /* a 62 pt app icon (its pad as the pack's), or 0 */
int sw_ext_snapshot(int subid, tk_bmp *out);   /* draw the window into out (SNAP_W x SNAP_H); 0: none (a plain card) */
void sw_ext_open(int subid);
void sw_ext_close(int subid);
#else
static const char *sw_ext_title(int subid) { (void)subid; return "App"; }
static const tk_bmp *sw_ext_icon(int subid) { (void)subid; return 0; }
static int sw_ext_snapshot(int subid, tk_bmp *out) { (void)subid; (void)out; return 0; }
static void sw_ext_open(int subid) { (void)subid; }
static void sw_ext_close(int subid) { (void)subid; }
#endif
static int sw_find_ext(int subid) { for (int i = 0; i < swn; i++) if (SWR[i].kind && SWR[i].subid == subid) return i; return -1; }

static void sw_remove(int i)
{
    if (i < 0 || i >= swn) return;
    tk_bmp_free(&SWR[i].snap);
    for (int k = i; k + 1 < swn; k++) SWR[k] = SWR[k + 1];
    swn--;
    SWR[swn].snap = (tk_bmp){ 0, 0, 0, 0 };
    SWR[swn].kind = 0;
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
/* the deck's state (declared early: the recents' order waits for a quick switch to be kept, below) */
#define SW_S .69f
#define SW_CW (390 * SW_S)
#define SW_CH (H_PT * SW_S)
#define SW_MID 391.0f
#define SW_CY (SW_MID - SW_CH / 2)
#define SW_PITCH 209.0f                          /* a position's step: the front card moves this far */
#define SW_R 30.0f
enum { SWG_NONE, SWG_CARD, SWG_BG, SWG_HOME, SWG_SCROLL, SWG_THROW, SWG_EDGE };
static struct {
    int on, closing, live, armed, dragging, g, sl, moved, opening, thrown, cov, hold, gapi, ek, en;
    float p, scroll, base, x0, y0, slow_t0, dy, dyv, ta, ex;
    tk_tracker tr;
} SW = { .sl = -1, .thrown = -1, .gapi = -1, .en = -1 };
static tk_spring sp_sw, sp_sws, sp_swdy, sp_swgap, sp_swe;
/* move the open app to the front of the recents (a quick switch keeps the order until then) */
static void sw_commit(void)
{
    if (!SW.hold) return;
    SW.hold = 0;
    if (!AP.on) return;
    int i = sw_find(AP.a);
    if (i <= 0) return;
    sw_ent e = SWR[i];
    for (int k = i; k > 0; k--) SWR[k] = SWR[k - 1];
    SWR[0] = e;
}
/* app_open: the app goes first (a new one pushes the oldest, and its snapshot, off the end) */
static void sw_opened(int a)
{
    if (AP.on && AP.a != a && !AP.closing && !ap_pending) sw_snap();   /* (the host's states switch apps at once) */
    sw_dirty = 1;
    if (!sw_keeps(a)) return;
    int i = sw_find(a);
    if (SW.hold && i >= 0) return;               /* B4: a quick switch: the order stays (sw_commit) */
    SW.hold = 0;
    sw_ent e = { a, { 0, 0, 0, 0 }, 0, 0 };
    if (i < 0) {
        if (swn == SW_MAX) sw_remove(SW_MAX - 1);
        i = swn++;
    } else e = SWR[i];
    for (int k = i; k > 0; k--) SWR[k] = SWR[k - 1];
    SWR[0] = e;
}
/* B4: the provider's entries (one per running Linux app): in front, as a newly opened app */
void sw_ext_add(int subid)
{
    if (sw_find_ext(subid) >= 0) return;
    if (swn == SW_MAX) sw_remove(SW_MAX - 1);
    for (int k = swn; k > 0; k--) SWR[k] = SWR[k - 1];
    SWR[0] = (sw_ent){ -1, { 0, 0, 0, 0 }, 1, subid };
    swn++;
}
static void sw_labels(void);
static void relink(void);
void sw_ext_remove(int subid)
{
    int i = sw_find_ext(subid);
    if (i < 0) return;
    sw_remove(i);
    if (SW.on) { sw_labels(); relink(); }
}

/* ---- the deck ------------------------------------------------------------------------------------------------ */
static tk_layer swg, swdim, swcard[SW_MAX], swlab[SW_MAX], swname[SW_MAX], swshade[SW_MAX], swcover;
static int sw_on(void) { return SW.on; }
static float sw_max_scroll(void) { return swn > 1 ? (swn - 1) * SW_PITCH : 0; }
/* a card's left edge and scale at position d (0 focused, 1 behind to the left, -1 in front to the right) */
static void sw_geom(float d, float *x, float *s)
{
    static const float AX[6] = { 549.5f, 269.5f, 60.5f, -9.5f, -49.5f, -79.5f };   /* d = -2 .. 3 */
    static const float AS[6] = { .70f, .70f, .69f, .65f, .62f, .60f };
    float u = tk_clampf(d + 2, 0, 4.999f);
    int k = (int)u;
    float f = u - k;
    *x = AX[k] + (AX[k + 1] - AX[k]) * f;
    *s = AS[k] + (AS[k + 1] - AS[k]) * f;
    if (d < -2) *x += (-2 - d) * 280;
    if (d > 3) *x -= (d - 3) * 30;
}
static float sw_pos(int i)
{
    float d = i - SW.scroll / SW_PITCH;
    if (SW.gapi >= 0 && i >= SW.gapi) d += sp_swgap.x;    /* closing a thrown card's gap */
    return d;
}
static arect sw_rect(int i)
{
    float x, s;
    sw_geom(sw_pos(i), &x, &s);
    float dy = i == SW.thrown ? SW.dy : 0;
    return (arect){ x, SW_MID - H_PT * s / 2 + dy, 390 * s, H_PT * s, SW_R };
}
static const tk_bmp *sw_icon_of(int i, float *pad)
{
    if (SWR[i].kind) { *pad = 0; return sw_ext_icon(SWR[i].subid); }
    return app_icon(APPS[SWR[i].a].k, pad);
}
static const char *sw_title_of(int i) { return SWR[i].kind ? sw_ext_title(SWR[i].subid) : APPS[SWR[i].a].name; }
/* the cards' icons (25.5 pt, 24 in from the card's left, 7 above it) and names (17 / 600), drawn once when it opens;
 * drawn at the focused card's place and moved with each card */
static void sw_labels(void)
{
    for (int i = 0; i < SW_MAX; i++) { relayer(&swlab[i]); relayer(&swname[i]); swlab[i].hidden = swname[i].hidden = 1; }
    float x = (390 - SW_CW) / 2 + 24, y = SW_CY - 7 - 25.5f;
    for (int i = 0; i < swn; i++) {
        dc d = mk(&swlab[i], "sw-icon", x, y, 25.5f, 25.5f, 2, 0);
        float pad;
        const tk_bmp *ic = sw_icon_of(i, &pad);
        if (!(!SWR[i].kind && sv_tile(&d, APPS[SWR[i].a].k, x, y, 25.5f)) && ic)
            nc_blit_scaled(d.b, ic, PT(x) - pad * 25.5f / 62 - d.ox, PT(y) - pad * 25.5f / 62 - d.oy, 25.5f / 62);
        trim(&swlab[i]);
        tk_layer_prepare(&swlab[i]);
        dc n = mk(&swname[i], "sw-name", x + 33.5f, y, 200, 25.5f, 2, 0);
        d_text_box(&n, TK_IT600, 17, x + 33.5f, bl(TK_IT600, 17, y + 2), 200, 0, sw_title_of(i), 0, 0, INK, 1);
        trim(&swname[i]);
        tk_layer_prepare(&swname[i]);
    }
}
static void sw_card_view(int i)
{
    tk_layer *l = &swcard[i];
    if (SWR[i].snap.px) {
        if (l->bmp.px != SWR[i].snap.px) { l->bmp = SWR[i].snap; l->solid = 0; l->gen++; }
    } else if (!l->solid) {
        /* no snapshot yet (closed before an idle turn; an external entry without one): its ground */
        l->bmp = (tk_bmp){ 0, 0, 0, 0 };
        l->solid = 0xff000000u | (SWR[i].kind ? 0x1c1c1eu : sv_ground(SWR[i].a));
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
    float a = tk_clampf(SW.p, 0, 1);
    for (int i = 0; i < SW_MAX; i++) {
        tk_layer *c = &swcard[i], *lb = &swlab[i], *nm = &swname[i], *sh = &swshade[i];
        if (i >= swn || !SW.on) { c->hidden = lb->hidden = nm->hidden = sh->hidden = 1; continue; }
        arect R = sw_rect(i);
        float d = sw_pos(i);
        if (!(i == 0 && SW.live)) R.x -= (1 - a) * 60;   /* the others come in from the left */
        float ca = i == SW.thrown ? SW.ta * a : a;
        float s = PT(R.w) / SNAP_W;
        sw_card_view(i);
        c->sx = c->sy = s;
        c->ox = c->oy = 0;
        c->x = PT(R.x) / s;
        c->y = PT(R.y) / s;
        if (c->solid) { float k = R.w / 390; c->sx = c->sy = k; c->x = PT(R.x) / k; c->y = PT(R.y) / k; }
        c->clip = 1;
        c->clip_r = (tk_rect){ PT(R.x), PT(R.y), PT(R.w), PT(R.h) };
        c->clip_radius = PT(SW_R);
        c->alpha = ca;
        c->hidden = (i == 0 && SW.live) || ca <= .004f || R.x > 400 || R.x + R.w < -10;
        /* the ones behind darken (0 .. 15 % by how far behind) */
        float shade = .15f * tk_clampf(d, 0, 1.5f) / 1.5f;
        sh->solid = tk_premul(TK_RGBA(0, 0, 0, 1));
        sh->x = PT(R.x); sh->y = PT(R.y); sh->solid_w = PT(R.w); sh->solid_h = PT(R.h);
        sh->clip = 1; sh->clip_r = (tk_rect){ sh->x, sh->y, sh->solid_w, sh->solid_h }; sh->clip_radius = PT(SW_R);
        sh->alpha = shade * ca;
        sh->hidden = c->hidden || sh->alpha <= .004f;
        if (lb->bmp.px) {
            float ox = PT(R.x - (390 - SW_CW) / 2), oy = PT(R.y - SW_CY);
            lb->x = lb->bx + tk_floorf(ox + .5f);
            lb->y = lb->by + tk_floorf(oy + .5f);
            lb->alpha = ca;
            lb->hidden = ca <= .004f || R.x > 400 || R.x + R.w < -10;
            if (nm->bmp.px) {
                nm->x = nm->bx + tk_floorf(ox + .5f);
                nm->y = nm->by + tk_floorf(oy + .5f);
                nm->alpha = ca * tk_clampf(1 - tk_fabsf(d) * 2.5f, 0, 1);
                nm->hidden = nm->alpha <= .004f;
            }
        }
    }
    /* the open app is its card: its window */
    if (SW.on && !SW.closing && !SW.opening && SW.live && AP.on && !sp_app.live && !AP.closing && !SW.dragging) set_app_rect(sw_rect(0));
}
static void sw_free(void)
{
    for (int i = 0; i < SW_MAX; i++) {
        relayer(&swlab[i]); swlab[i].hidden = 1;
        relayer(&swname[i]); swname[i].hidden = 1;
        swcard[i].bmp = (tk_bmp){ 0, 0, 0, 0 };   /* a view of a snapshot: never freed here */
        swcard[i].solid = 0;
        swcard[i].hidden = 1;
        swshade[i].hidden = 1;
    }
    swg.hidden = 1;
}
static void sw_show(void)
{
    if (SW.on && !SW.closing) return;
    sw_commit();
    tk_layer_init(&swg, "switcher");
    tk_layer_init(&swdim, "sw-dim");
    for (int i = 0; i < SW_MAX; i++) {
        tk_layer_init(&swcard[i], "sw-card"); swcard[i].hidden = 1;
        tk_layer_init(&swshade[i], "sw-shade"); swshade[i].hidden = 1;
        if (i < swn && SWR[i].kind) {             /* an external entry's window now */
            if (!SWR[i].snap.px) tk_bmp_new(&SWR[i].snap, SNAP_W, SNAP_H);
            if (SWR[i].snap.px && !sw_ext_snapshot(SWR[i].subid, &SWR[i].snap)) tk_bmp_free(&SWR[i].snap);
        }
    }
    sw_labels();
    SW.on = 1; SW.closing = 0;
    SW.scroll = 0; sp_sws.live = 0;
    SW.thrown = -1; SW.dy = 0; SW.ta = 1; sp_swdy.live = 0;
    SW.gapi = -1; sp_swgap.live = 0; sp_swgap.x = 0;
    SW.live = AP.on && !AP.closing && swn && !SWR[0].kind && SWR[0].a == AP.a;
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
    sp_sw.live = sp_sws.live = sp_swdy.live = sp_swgap.live = sp_swe.live = 0;
    SW.gapi = -1; SW.thrown = -1; SW.en = -1;
    SW.cov = 0; swcover.hidden = 1; swcover.bmp = (tk_bmp){ 0, 0, 0, 0 }; swcover.solid = 0;
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
/* a card's snapshot over the window while its page is drawn (two strips a frame) */
static void sw_cover(int i)
{
    if (!SWR[i].snap.px) return;
    tk_layer_init(&swcover, "sw-cover");
    swcover.bmp = SWR[i].snap;
    SW.cov = 1;
}
static void sw_open(int i)
{
    if (i < 0 || i >= swn) return;
    arect R = sw_rect(i);
    if (SWR[i].kind) {                           /* an external entry: its provider brings it up */
        if (AP.on) { sp_app.live = 0; app_finish_close(); }
        sw_ext_open(SWR[i].subid);
        SW.live = 0;
        sw_hide();
        return;
    }
    if (i == 0 && SW.live && AP.on) {
        AP.from = AP.R; AP.to = FULLR;
        AP.closing = 0;
        tk_spring_start(&sp_app, 0, 1, 0, TK_SPR_GROW);
    } else {
        int a = SWR[i].a;
        if (AP.on) { sp_app.live = 0; app_finish_close(); }
        sw_cover(i);
        ap_from_on = 1;
        ap_from = R;
        app_open(a);                              /* (it moves the app first in the recents) */
        tk_spring_start(&sp_app, 0, 1, 0, TK_SPR_GROW);
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
    SW.dy = 0; SW.ta = 1;
    if (i < 0 || i >= swn) return;
    if (i == 0 && SW.live && AP.on) { sp_app.live = 0; app_finish_close(); SW.live = 0; }
    if (SWR[i].kind) sw_ext_close(SWR[i].subid);
    sw_remove(i);
    for (int k = 0; k < SW_MAX; k++) { swcard[k].bmp = (tk_bmp){ 0, 0, 0, 0 }; swcard[k].solid = 0; }
    sw_labels();
    relink();
    if (!swn) { sw_hide(); return; }
    /* the ones behind close the gap (from where they were: one position further) */
    SW.gapi = i;
    tk_spring_start(&sp_swgap, 1, 0, 0, TK_SPR_CARD);
    float m = sw_max_scroll();
    if (SW.scroll > m) tk_spring_start(&sp_sws, SW.scroll, m, 0, TK_SPR_CARD);
}

/* ---- B4: the quick switch (the home bar's sideways swipe in an app) ------------------------------------------- */
/* the neighbour a swipe that way would bring: dir 1 (to the right: the older one), -1 (the newer one); -1 none */
static int sw_edge_next(int dir)
{
    int k = SW.ek + dir;
    if (k < 0 || k >= swn || SWR[k].kind) return -1;
    return k;
}
static void sw_edge_place(void)
{
    float gap = 12, ex = SW.ex;
    set_app_rect((arect){ ex, 0, 390, H_PT, S9_CORNER_PT });
    int dir = ex > 0 ? 1 : -1, k = sw_edge_next(dir);
    if (k != SW.en) {
        SW.en = k;
        swcover.bmp = (tk_bmp){ 0, 0, 0, 0 };
        swcover.solid = 0;
        swcover.gen++;
        if (k >= 0) {
            if (SWR[k].snap.px) swcover.bmp = SWR[k].snap;
            else { swcover.solid = 0xff000000u | sv_ground(SWR[k].a); swcover.solid_w = TK_W; swcover.solid_h = TK_H; }
        }
    }
    if (k < 0 || tk_fabsf(ex) < .5f) { swcover.hidden = 1; return; }
    float x = dir > 0 ? ex - 390 - gap : ex + 390 + gap;
    float s = swcover.solid ? 1 : (float)TK_W / SNAP_W;
    swcover.sx = swcover.sy = s;
    swcover.ox = swcover.oy = 0;
    swcover.x = PT(x) / s; swcover.y = 0;
    if (swcover.solid) swcover.x = PT(x);
    swcover.clip = 1;
    swcover.clip_r = (tk_rect){ PT(x), 0, PT(390), PT(H_PT) };
    swcover.clip_radius = PT(S9_CORNER_PT);
    swcover.alpha = 1;
    swcover.hidden = 0;
}
static int sw_edge_ok(void) { return !locked && AP.on && !AP.closing && !SW.on && sw_keeps(AP.a) && sw_find(AP.a) >= 0 && swn > 1; }
/* shell_lock.c's G_APP: the finger went sideways first along the bottom edge */
static void sw_edge_begin(float x, float t)
{
    if (!SW.cov) { tk_layer_init(&swcover, "sw-cover"); swcover.hidden = 1; }
    SW.ek = sw_find(AP.a);
    SW.en = -2;
    SW.ex = 0;
    SW.x0 = x;
    sp_swe.live = 0;
    sp_app.live = 0;
    SW.g = SWG_EDGE;
    tk_tracker_reset(&SW.tr);
    tk_tracker_add(&SW.tr, x, t);
    relink();
    sw_edge_place();
}
static void sw_edge_land(void)
{
    int k = SW.en;
    swcover.hidden = 1;
    if (SW.ex == 0 || k < 0) { SW.en = -1; set_app_rect(FULLR); appg.clip = 0; return; }
    /* the neighbour's page under its snapshot, at once full screen; the recents keep their order for now */
    int a = SWR[k].a;
    sp_app.live = 0;
    app_finish_close();
    if (SWR[k].snap.px) { swcover.bmp = SWR[k].snap; swcover.solid = 0; SW.cov = 1; }
    SW.hold = 1;
    ap_from_on = 1;
    ap_from = FULLR;
    SW.opening = 1;
    app_open(a);
    SW.en = -1;
}

/* ---- touches (sh_touch: G_SW) ---------------------------------------------------------------------------------- */
static int sw_down(float x, float y, float t)
{
    if (!SW.on || SW.closing) return 0;
    tk_tracker_reset(&SW.tr);
    SW.x0 = x; SW.y0 = y; SW.moved = 0;
    SW.sl = -1;
    for (int i = 0; i < swn && SW.sl < 0; i++) {           /* the top one: newer cards lie on older ones */
        arect R = sw_rect(i);
        if (x >= R.x && x < R.x + R.w && y >= R.y - 40 && y < R.y + R.h) SW.sl = i;
    }
    SW.g = y >= H_PT - 34 ? SWG_HOME : SW.sl >= 0 ? SWG_CARD : SWG_BG;
    sp_sws.live = 0;
    SW.base = SW.scroll;
    tk_tracker_add(&SW.tr, x, t);
    return 1;
}
static void sw_touch(int ph, float x, float y, float t)
{
    float dx = x - SW.x0, dy = y - SW.y0;
    if (SW.g == SWG_EDGE) {
        if (ph == 1) {
            tk_tracker_add(&SW.tr, x, t);
            int k = sw_edge_next(dx > 0 ? 1 : -1);
            SW.ex = k >= 0 ? dx : (dx > 0 ? rubber(dx, 390) : -rubber(-dx, 390)) * .5f;
            sw_edge_place();
            return;
        }
        if (ph != 2) return;
        float v = tk_tracker_v(&SW.tr), proj = SW.ex + v * 200;
        int dir = SW.ex > 0 ? 1 : -1, k = sw_edge_next(dir);
        float to = k >= 0 && proj * dir > 120 && v * dir > -.2f ? dir * (390 + 12.0f) : 0;
        SW.g = SWG_NONE;
        tk_spring_start(&sp_swe, SW.ex, to, v * 1000, TK_SPR_CARD);
        if (to != 0) sh_haptic_k(SH_HAP_LIGHT);
        return;
    }
    if (ph == 1) {
        if (!SW.moved && dx * dx + dy * dy > 64) {
            SW.moved = 1;
            if (SW.g == SWG_HOME) ;
            else if (tk_fabsf(dx) > tk_fabsf(dy)) { SW.g = SWG_SCROLL; SW.x0 = x; dx = 0; }
            else if (SW.g == SWG_CARD && SW.thrown < 0) {
                SW.g = SWG_THROW; SW.thrown = SW.sl; SW.ta = 1; sp_swdy.live = 0;
                SW.y0 = y; dy = 0;
                tk_tracker_reset(&SW.tr);
            }
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
            SW.dy = dy < 0 ? dy : rubber(dy, H_PT) * .4f;   /* up 1:1, down held back */
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
        /* B4: a flick carries on by its speed (iOS's deceleration: about a quarter second's travel) and snaps to a
         * card, as many as it reaches; the finger's speed goes to the spring */
        float m = sw_max_scroll(), to = tk_floorf((SW.scroll + v * 260) / SW_PITCH + .5f) * SW_PITCH;
        to = tk_clampf(to, 0, m);
        tk_spring_start(&sp_sws, SW.scroll, to, v * 1000, TK_SPR_CC);
    } else if (g == SWG_THROW) {
        /* IOS-REFERENCE 2: off above 30 % of the height, or faster than .8 pt/ms upward; else back on the card spring */
        if (SW.dy < -H_PT * .3f || v < -.8f || (SW.dy < -110 && v < -.3f)) { SW.dyv = tk_minf(v, -.8f); sh_haptic_k(SH_HAP_LIGHT); }
        else { SW.dyv = 0; tk_spring_start(&sp_swdy, SW.dy, 0, v * 1000, TK_SPR_CARD); }
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
        sh_haptic_k(SH_HAP_LIGHT);
        sw_show();
        SW.live = 1;
    }
    if (SW.dragging) mv = 1;
    if (sp_swe.live) {                           /* B4: the quick switch lands */
        int live = tk_spring_step(&sp_swe, dt);
        SW.ex = sp_swe.x;
        if (AP.on) sw_edge_place();
        if (!live) sw_edge_land();
        mv = 1;
    }
    if (SW.g == SWG_EDGE) mv = 1;
    if (sp_sw.live) {
        int live = tk_spring_step(&sp_sw, dt);
        SW.p = tk_maxf(0, sp_sw.x);
        if (!live && SW.closing) { SW.on = SW.closing = 0; SW.p = 0; sw_free(); relink(); }
        mv = 1;
    }
    if (sp_sws.live) { tk_spring_step(&sp_sws, dt); SW.scroll = sp_sws.x; mv = 1; }
    if (sp_swgap.live) { if (!tk_spring_step(&sp_swgap, dt)) SW.gapi = -1; mv = 1; }
    if (SW.thrown >= 0 && SW.g != SWG_THROW) {
        if (SW.dyv < 0) {
            /* off with the finger's speed (a little faster as it goes), fading */
            SW.dyv -= .6f * dt;
            SW.dy += SW.dyv * dt * 1000;
            SW.ta = tk_clampf(1 + (SW.dy + 120) / 380, 0, 1);
            if (SW.dy < -SW_CY - SW_CH - 40 || SW.ta <= 0) sw_throw_done();
        } else {
            if (sp_swdy.live) { if (!tk_spring_step(&sp_swdy, dt)) SW.thrown = -1; SW.dy = sp_swdy.x; }
            else { SW.dy = 0; SW.thrown = -1; }
        }
        if (SW.thrown == 0 && SW.live && AP.on) set_app_rect(sw_rect(0));
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
            swcover.solid = 0;
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
static int sw_moving(void)
{
    return sp_sw.live || sp_sws.live || sp_swdy.live || sp_swgap.live || sp_swe.live || SW.g == SWG_SCROLL || SW.g == SWG_THROW ||
           SW.g == SWG_EDGE || SW.thrown >= 0 || SW.cov || SW.dragging;
}
static void sw_idle(void)
{
    if (sw_dirty && AP.on && !SW.on && !SW.opening) sw_snap();
}
static void sw_link(void)
{
    if (!swg.name) { tk_layer_init(&swg, "switcher"); swg.hidden = 1; }
    if (!swcover.name) { tk_layer_init(&swcover, "sw-cover"); swcover.hidden = 1; }
    tk_layer *c[1 + 4 * SW_MAX];
    int n = 0;
    c[n++] = &swdim;
    for (int i = SW_MAX - 1; i >= 0; i--) { c[n++] = &swcard[i]; c[n++] = &swshade[i]; c[n++] = &swlab[i]; c[n++] = &swname[i]; }   /* the older under the newer */
    if (SW.on) link(&swg, c, n);
    else swg.child = 0;
}
/* the host's stills: "switcher" p (the open app as the focused card), "swscroll" positions, "swsnap" (the open app's
 * snapshot now), "swedge" pt (the quick switch dragged that far) */
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
    if (!tk_strcmp(what, "swscroll")) {
        if (!SW.on) return 1;
        SW.scroll = tk_clampf(v, 0, swn - 1) * SW_PITCH;
        sw_place();
        if (SW.live && AP.on) set_app_rect(sw_rect(0));
        return 1;
    }
    if (!tk_strcmp(what, "swedge")) {
        if (!sw_edge_ok()) return 1;
        sw_edge_begin(0, now_ms);
        SW.g = SWG_NONE;
        SW.ex = v;
        sw_edge_place();
        return 1;
    }
    return 0;
}
