/* SPDX-License-Identifier: MIT
 * cp6 (Track B, the motion helper): iOS's push and pop between an app's pages, included by shell_lock.c after the
 * pages (it reads their state: AS, NS, CK, WV, SU). Normal mode only: Emergency and Last 2 % change pages at once,
 * as before.
 *
 * Which page shows is a key (ps_key: the app, Settings' AS.sub with Startup's own level, Messages' thread, Notes'
 * editor; Clock's alarm editor and Wi-Fi's password are modal). build_app_page asks ps_will_build first: when the key
 * differs from the one the last frame showed, the page on the screen (shown_frame, before anything is rebuilt) is
 * kept as a picture at half resolution (4.2 MB, drawn 2x) and the new page is built as before, then:
 *   - a push: the new page (pushg: the window's content over its own ground) slides in from the right (x 390 -> 0)
 *     with a soft shadow at its edge, the picture of the old one slides to -30 % under a black .14;
 *   - a pop: the picture of the page that goes slides off to the right over the parent (from -30 %, the dim going);
 *   - a modal page (Clock's editor, Wi-Fi's password) rises from the bottom over the picture, and its picture goes
 *     down again;
 * on iOS's navigation spring (response .35 s, damping 1: no overshoot). The picture of the page under a pushed one is
 * kept (the same 4.2 MB, freed with the app, at a pop to the root or when the app changes) for the left-edge swipe:
 * from x < 24 pt on a pushed page the page follows the finger over its parent's picture; let go past half (or with a
 * flick) it goes and the back runs (the page's own back circle, tapped for it) once it is off the screen, else it
 * springs back. Without the parent's picture (after a pop to a pushed page) the ground shows under the drag, and the
 * back runs at the let-go with the page's picture finishing the slide over it. A touch that stays still or goes
 * vertical is handed to the page as it came (taps on the back circle, a list's scroll).
 *
 * The tree: pushg (a group in the window, appg) holds the window's content (appc, the keypad, the keyboard ...) over
 * pushbg (the page's ground, a solid); the picture (pshot), its dim (pdim) and the edge's shadow (pshad) go under
 * pushg for a push and over it for a pop. The status bar (apsb) and the press light (apphl) stay on top, unmoved. */

#define PS_EDGE 24.0f                       /* the edge swipe's zone, pt from the left */
#define PS_DIM .14f                         /* the black over the page underneath */
#define PS_PAR .3f                          /* how far the page underneath sits to the left (of the width) */
#define PS_SHW 18.0f                        /* the shadow left of a moving page's edge, pt */
#define PS_MODAL (1 << 19)
enum { PSM_NONE, PSM_PUSH, PSM_POP, PSM_UP, PSM_DOWN, PSM_DRAG, PSM_BACK, PSM_CANCEL };

static tk_layer pushg, pushbg, pshot, pdim, pshad;
static struct {
    int app, cur;                           /* the page shown at the last frame at rest (-1: not known) */
    int st[8], n;                           /* the pages under it (st[n - 1] = cur) */
    int mode, under;                        /* mode: PSM_*; under: the picture under pushg (1) or over it (0) */
    int kept;                               /* pshot holds the parent's picture (for the edge swipe) */
    int silent;                             /* the edge's back running: no slide of its own */
    int armed;                              /* the slide's first step waits for the frame after the build */
    float p, p0;                            /* progress 0..1; p0: where the picture's page edge was (an edge let-go) */
    tk_spring sp;
    int edge;                               /* the edge touch: 1 deciding, 2 dragging */
    int passing;                            /* a touch handed on (sh_touch called again) */
    float x0, y0, t0;
    tk_tracker tr;
} PS = { .app = -1, .cur = -1 };

static int ps_quiet(void) { return EMG || sh_last2(); }
static int ps_moving(void) { return PS.mode != PSM_NONE || PS.edge == 2; }

static int ps_key(void)
{
    if (!AP.on) return -1;
    const char *k = APPS[AP.a].k;
    int v = 0;
    if (!tk_strcmp(k, "settings")) {
        v = AS.sub & 0xff;
#ifdef SH_HAS_STARTUP
        if (AS.sub == SE_STARTUP_PAGE) v |= (SU.view & 0xff) << 8;       /* Track J's levels */
#endif
        if (AS.sub == SE_WIFI && WV.view == 1) v |= PS_MODAL;              /* the password: a sheet on iOS */
    } else if (!tk_strcmp(k, "messages")) v = AS.thread >= 0 ? 1 + (AS.thread & 0xff) : 0;
    else if (!tk_strcmp(k, "notes")) v = NS.view ? 1 : 0;
    else if (!tk_strcmp(k, "clock")) v = CK.edit >= 0 ? PS_MODAL | 1 : 0;
    return AP.a << 20 | v;
}
static int ps_pushed(int k) { return k >= 0 && (k & 0xffff) && !(k & PS_MODAL); }

/* where the window can slide: whole, still, not the switcher's card */
static int ps_ready(void)
{
    return AP.on && !AP.closing && !sp_app.live && !ap_pending && !AP.following && !SW.on && AP.R.w >= 389.5f && AP.R.y > -.5f &&
           AP.R.y < .5f;
}

/* the order of the window's layers for the mode (relink calls it with the window's list: ag[0] the ground, then the
 * content, the status bar and the press light last) */
static void ps_link(tk_layer **ag, int n)
{
    if (!pushg.name) {
        tk_layer_init(&pushg, "push");
        tk_layer_init(&pushbg, "push-ground");
        tk_layer_init(&pshot, "push-picture");
        tk_layer_init(&pdim, "push-dim");
        tk_layer_init(&pshad, "push-shadow");
        pushbg.hidden = pshot.hidden = pdim.hidden = pshad.hidden = 1;
    }
    tk_layer *g[24];
    int m = 0;
    g[m++] = &pushbg;
    for (int i = 1; i < n - 2 && m < 24; i++) if (ag[i] != &kbg) g[m++] = ag[i];
    link(&pushg, g, m);
    tk_layer *w[10];
    m = 0;
    w[m++] = ag[0];
    if (PS.under) { w[m++] = &pshot; w[m++] = &pdim; w[m++] = &pshad; w[m++] = &pushg; }
    else { w[m++] = &pushg; w[m++] = &pdim; w[m++] = &pshad; w[m++] = &pshot; }
    w[m++] = &kbg;                                  /* the keyboard is the window's, not the page's: it rises and
                                                     * goes down on its own spring while the pages slide (iOS) */
    w[m++] = ag[n - 2];
    w[m++] = ag[n - 1];
    link(&appg, w, m);
}

/* the shadow: a column 18 pt wide, black from 0 to .1 towards the page's edge (4 rows, stretched over the height) */
static void ps_shadow_make(void)
{
    if (pshad.bmp.px) return;
    int w = (int)PT(PS_SHW);
    if (!tk_bmp_new(&pshad.bmp, w, 4)) return;
    for (int i = 0; i < w; i++) {
        float f = (float)(i + 1) / (float)w, a = .10f * f * f;
        uint32_t A = (uint32_t)(a * 255 + .5f);
        tk_fill_rect_raw(&pshad.bmp, i, 0, 1, 4, A << 24);
    }
    pshad.slice = 1;
    pshad.slice_s0 = pshad.slice_s1 = 1;
    pshad.slice_h = TK_H;
    tk_layer_dirty(&pshad);
}

/* the layers for the mode and the progress */
static void ps_place(void)
{
    float W = TK_W, H = TK_H, p = PS.p;
    pushg.x = pushg.y = 0;
    pshot.x = pshot.y = 0;
    pshot.clip = 0;
    pdim.alpha = 0;
    pshad.hidden = 1;
    int vert = PS.mode == PSM_UP || PS.mode == PSM_DOWN;
    float edge = -1;                                    /* the moving page's left edge on the screen (px), -1 none */
    if (PS.mode == PSM_NONE) { pshot.hidden = pdim.hidden = 1; pushbg.hidden = 1; return; }
    pushbg.hidden = 0;
    if (PS.under) {
        /* p: how far the new page is in (a push: 0 -> 1; the edge drag: 1 - the finger's way) */
        if (vert) pushg.y = tk_floorf(H * (1 - p) + .5f);
        else {
            pushg.x = tk_floorf(W * (1 - p) + .5f);
            pshot.x = tk_floorf(-PS_PAR * W * p + .5f) / 2;
            edge = pushg.x;
        }
        pdim.alpha = (vert ? .2f : PS_DIM) * tk_clampf(p, 0, 1);
        /* the tap's frame (the build's): 2 px of the new page on the screen, so its layers go to the GPU (cache
         * clean, block maps: ~2 ms a MB on the phone) in that frame, not in the slide's first */
        if (PS.armed && p <= 0) { if (vert) pushg.y = H - 2; else pushg.x = W - 2; }
    } else {
        /* p: how far the old page's picture is out (a pop: 0 -> 1) */
        float e = (vert ? H : W) * p;
        if (vert) pshot.y = tk_floorf(e + .5f) / 2;
        else {
            pushg.x = tk_floorf(-PS_PAR * W * (1 - p) + .5f);
            pshot.x = tk_floorf(e - W * PS.p0 + .5f) / 2;
            edge = tk_floorf(e + .5f);
            if (PS.p0 > 0) {                             /* the picture holds the ground left of its page: cut */
                pshot.clip = 1;
                pshot.clip_r = (tk_rect){ edge, 0, W - edge, H };
            }
        }
        pdim.alpha = (vert ? .2f : PS_DIM) * tk_clampf(1 - p, 0, 1);
    }
    pshot.hidden = !PS.kept && PS.mode == PSM_DRAG ? 1 : !pshot.bmp.px;
    pdim.hidden = pshot.hidden || pdim.alpha <= .003f;
    if (PS.mode == PSM_DRAG || PS.mode == PSM_BACK || PS.mode == PSM_CANCEL) pdim.hidden = !PS.kept || pdim.alpha <= .003f;
    if (edge > 0 && edge < W && pshad.bmp.px) {
        pshad.hidden = 0;
        pshad.x = edge - pshad.bmp.w;
        pshad.y = 0;
    }
}

static void ps_free(void)
{
    relayer(&pshot);
    pshot.hidden = 1;
    PS.kept = 0;
}
/* the slide's end: the layers at rest (the picture kept under a pushed page, else freed) */
static void ps_rest(void)
{
    PS.mode = PSM_NONE;
    PS.p0 = 0;
    pushg.x = pushg.y = 0;
    pshot.hidden = pdim.hidden = pshad.hidden = pushbg.hidden = 1;
    if (!PS.kept) ps_free();
    if (!PS.under) { PS.under = 1; relink(); }
}

/* the page shown, the stack and the direction of a change: 1 push, -1 pop, 2 up (modal), -2 down */
static int ps_dir(int old, int nk)
{
    if ((nk & PS_MODAL) && !(old & PS_MODAL)) return 2;
    if ((old & PS_MODAL) && !(nk & PS_MODAL)) return -2;
    if (PS.n < 1) { PS.st[0] = old; PS.n = 1; }
    if (!(nk & 0xffff)) { PS.st[0] = nk; PS.n = 1; return -1; }
    for (int i = 0; i < PS.n - 1; i++) if (PS.st[i] == nk) { PS.n = i + 1; return -1; }
    if (is_app("settings") && se_parent(old & 0xff) == (nk & 0xff) && !((old | nk) & 0xff00)) { PS.st[PS.n - 1] = nk; return -1; }
    if (PS.n < 8) PS.st[PS.n++] = nk;
    else PS.st[7] = nk;
    return 1;
}

static void ps_spring(float from, float to, float v)
{
    tk_spring_start(&PS.sp, from, to, v, TK_SPR_CARD);
    PS.sp.k = 322.3f;                          /* iOS's navigation: response .35 s, damping 1 (no overshoot) */
    PS.sp.c = 35.9f;
}

/* the log's "push: picture N us" (where a slow tap's time went, on the phone too) */
static void ps_lap(uint64_t us)
{
    char b[48] = "push: picture ";
    int n = 14;
    char d[20];
    int k = 0;
    do { d[k++] = (char)('0' + us % 10); us /= 10; } while (us && k < 19);
    while (k) b[n++] = d[--k];
    b[n++] = ' '; b[n++] = 'u'; b[n++] = 's'; b[n] = 0;
    tk_log(b);
}
/* cp7g: the GPU path's picture without tk_comp_shown (a whole CPU frame, ~200 ms on the phone, in the tap): the app's
 * page flattened at half, its glass layers apart (the flatten has no lens: their bodies would show the page through):
 * pass 1 the page without them, then each one's shapes filled with the dark glass the lens gives over a dark page,
 * and its own pixels (rim, words, glyphs) over that, where the compositor would put them */
#define PSG_MAX 48
static struct { tk_layer *l; float a, tx, ty, al; } PSG[PSG_MAX];
static int psg_n;
static void ps_glass_hide(tk_layer *l, float a, float tx, float ty, float al)
{
    for (; l; l = l->next) {
        if (l->hidden || l->alpha <= 0) continue;
        float A = a * l->sx, TX = a * (l->sx * l->x + (1 - l->sx) * l->ox) + tx, TY = a * (l->sy * l->y + (1 - l->sy) * l->oy) + ty;
        float AL = al * l->alpha;
        if ((l->ngsh > 0 || l->glass.v) && psg_n < PSG_MAX) {
            PSG[psg_n++] = (typeof(PSG[0])){ l, A, TX, TY, AL };
            l->hidden = 1;
            continue;
        }
        if (l->child) ps_glass_hide(l->child, A, TX, TY, AL);
    }
}
static void ps_flat_page(tk_bmp *out)
{
    float ca = appc.alpha, sa = apsb.alpha;
    appc.alpha = apsb.alpha = 1;                     /* as it is at rest */
    int ph = pshot.hidden;
    pshot.hidden = 1;                                /* (the picture's own layer is in appg: never into itself) */
    psg_n = 0;
    ps_glass_hide(appg.child, 1, 0, 0, 1);
    flat_cuts++;
    tk_flatten(&appg, out, 0, .5f);
    tk_paint dark = tk_solid(tk_hex(0x202126, 1));
    for (int i = 0; i < psg_n; i++) {
        tk_layer *l = PSG[i].l;
        l->hidden = 0;
        float a = PSG[i].a * .5f;
        for (int q = 0; q < l->ngsh; q++)
            tk_fill_rrect(out, PSG[i].tx * .5f + a * l->gsh[q][0], PSG[i].ty * .5f + a * l->gsh[q][1], a * l->gsh[q][2],
                          a * l->gsh[q][3], a * l->gsh[q][4], &dark, PSG[i].al);
        if (tk_fabsf(PSG[i].a - 1) < .01f && l->bmp.px) {
            tk_layer_prepare(l);
            if (l->half.px) tk_blit(out, &l->half, (int)tk_floorf(PSG[i].tx * .5f + .5f), (int)tk_floorf(PSG[i].ty * .5f + .5f), PSG[i].al);
        }
    }
    /* the row or button lit under the finger (apphl: not linked while the push builds; its place is on the layer):
     * its rect inside its rounded clip (the card) */
    if (!apphl.hidden && apphl.solid && apphl.alpha > 0) {
        float x0 = apphl.x, y0 = apphl.y, x1 = x0 + apphl.solid_w, y1 = y0 + apphl.solid_h;
        if (apphl.clip) {
            x0 = tk_maxf(x0, apphl.clip_r.x); y0 = tk_maxf(y0, apphl.clip_r.y);
            x1 = tk_minf(x1, apphl.clip_r.x + apphl.clip_r.w); y1 = tk_minf(y1, apphl.clip_r.y + apphl.clip_r.h);
        }
        int X = (int)tk_floorf(x0 * .5f), Y = (int)tk_floorf(y0 * .5f), w = (int)(x1 * .5f) + 1 - X, h = (int)(y1 * .5f) + 1 - Y;
        tk_bmp t;
        if (w > 0 && h > 0) {
            uint32_t c = apphl.solid;
            float a = (c >> 24) / 255.0f;
            tk_paint pt = tk_solid(TK_RGBA(a > 0 ? (c >> 16 & 255) / a : 0, a > 0 ? (c >> 8 & 255) / a : 0, a > 0 ? (c & 255) / a : 0, a));
            tk_rect r = apphl.clip ? apphl.clip_r : (tk_rect){ apphl.x, apphl.y, apphl.solid_w, apphl.solid_h };
            /* the clip's rounded rect, cut to the lit rect by the bitmap's own edges */
            if (tk_bmp_new(&t, w, h)) {
                tk_fill_rrect(&t, r.x * .5f - X, r.y * .5f - Y, r.w * .5f, r.h * .5f, apphl.clip ? apphl.clip_radius * .5f : 0, &pt, 1);
                tk_blit(out, &t, X, Y, apphl.alpha);
                tk_bmp_free(&t);
            }
        }
    }
    pshot.hidden = ph;
    appc.alpha = ca; apsb.alpha = sa;
}
/* the picture of the screen now (the page about to go), half resolution, the status band painted as the ground
 * (clear: see-through, for a modal page going down: no dark bar on its top edge) */
static int ps_snap(int clear)
{
    uint64_t t0 = tk_us();
    if (!pshot.bmp.px) {
        relayer(&pshot);
        tk_layer_init(&pshot, "push-picture");
        if (!tk_bmp_new(&pshot.bmp, TK_W / 2, TK_H / 2)) return 0;
    }
    if (C->gpu) ps_flat_page(&pshot.bmp);             /* cp7g: no whole CPU frame in the tap */
    else {
        if (C->last_half) tk_comp_frame(C, 0, 0);     /* the CPU's RAM frame after a moving one: whole first */
        tk_bmp_down2(&pshot.bmp, shown_frame());
    }
    tk_fill_rect_raw(&pshot.bmp, 0, 0, pshot.bmp.w, (int)(PT(SAFE_TOP) / 2), clear ? 0 : appbg.solid);   /* (the status bar stays) */
    if (KB.shown && !kbg.hidden) {                     /* the keyboard stays too (it goes down on its own) */
        int y0 = tk_maxi(0, (int)(PT(kb_top()) / 2) - 2);
        if (y0 < pshot.bmp.h) tk_fill_rect_raw(&pshot.bmp, 0, y0, pshot.bmp.w, pshot.bmp.h - y0, appbg.solid);
    }
    pshot.sx = pshot.sy = 2;
    pshot.ox = pshot.oy = 0;
    pshot.opaque = !clear;
    pshot.alpha = 1;
    tk_layer_dirty(&pshot);
    ps_lap(tk_us() - t0);
    return 1;
}

static void ps_begin(int mode, int under, float p0)
{
    PS.mode = mode;
    PS.p0 = p0;
    PS.armed = 1;
    if (PS.under != under) PS.under = under;
    pushbg.solid = appbg.solid;
    pushbg.solid_w = TK_W; pushbg.solid_h = TK_H;
    pushbg.x = pushbg.y = 0;
    pushbg.opaque = 1;
    pushbg.gen++;
    pdim.solid = tk_premul(TK_RGBA(0, 0, 0, 1));
    pdim.solid_w = TK_W; pdim.solid_h = TK_H;
    pdim.gen++;
    ps_shadow_make();
    apphl.hidden = 1; apphl.gen++;            /* the press light was in the picture; it goes with it */
    ps_place();
}

/* build_app_page's first line: a page change (the key differs from the one on the screen) slides */
static void ps_will_build(void)
{
    if (!AP.on || PS.app != AP.a || PS.cur < 0) return;
    int nk = ps_key(), old = PS.cur;
    if (nk == old) return;
    int dir = ps_dir(old, nk);
    PS.cur = nk;
    if (PS.silent) return;
    if (PS.mode != PSM_NONE || PS.edge == 2) { PS.edge = 0; ps_rest(); }
    if (ps_quiet() || !ps_ready()) { ps_free(); return; }
    if (!ps_snap(dir == -2)) { ps_free(); return; }
    PS.kept = dir == 1;                        /* a pushed page keeps its parent's picture (the edge swipe) */
    PS.p = 0;
    if (dir == 1) ps_begin(PSM_PUSH, 1, 0);
    else if (dir == 2) ps_begin(PSM_UP, 1, 0);
    else ps_begin(dir == -2 ? PSM_DOWN : PSM_POP, 0, 0);
    ps_spring(0, 1, 0);
    relink();
}

/* the page's own back, as a tap on its back circle (the touch goes through the shell as the finger's would) */
static void ps_do_back(float t)
{
    float bx = BACK_X + BACK_D / 2, by = NAV_MID;
    PS.silent = 1;
    PS.passing = 1;
    sh_touch(0, bx, by, t);
    sh_touch(2, bx, by, t + 30);
    PS.passing = 0;
    PS.silent = 0;
    PS.cur = ps_key();
    apphl.hidden = 1; apphl.gen++;            /* (the back circle's light: the page it was on is gone) */
}

static void ps_end(void)
{
    if (PS.mode == PSM_BACK) {
        /* the page is off the screen, its parent's picture whole: the back now (its build shows the same) */
        PS.kept = 0;
        ps_do_back(now_ms);
        ps_free();
    }
    if (PS.mode == PSM_POP || PS.mode == PSM_DOWN || (PS.mode == PSM_CANCEL && !PS.kept)) ps_free();
    if (!ps_pushed(PS.cur)) ps_free();
    ps_rest();
}
/* a touch while a slide runs: the slide's end now */
static void ps_finish(void)
{
    if (PS.mode == PSM_NONE) return;
    PS.p = PS.sp.to;
    ps_place();
    ps_end();
}

/* sh_tick: the slide's steps; the page at rest recorded */
static int ps_tick(float dt)
{
    if (!AP.on) {
        if (PS.mode != PSM_NONE || pshot.bmp.px || PS.edge) { PS.edge = 0; PS.kept = 0; ps_rest(); ps_free(); }
        PS.cur = -1; PS.app = -1; PS.n = 0;
        return 0;
    }
    if (PS.mode == PSM_NONE && PS.edge != 2 && !ap_pending) {
        int k = ps_key();
        if (PS.app != AP.a) { PS.app = AP.a; PS.n = 0; if (pshot.bmp.px) ps_free(); }
        if (k != PS.cur) {                       /* changed without a build here (a state, a rebuild elsewhere) */
            PS.cur = k;
            int i = 0;
            while (i < PS.n && PS.st[i] != k) i++;
            if (i < PS.n) PS.n = i + 1;
            else if (!(k & 0xffff) || PS.n >= 8) { PS.st[0] = k; PS.n = 1; }
            else if (!(k & PS_MODAL)) PS.st[PS.n++] = k;
            if (!ps_pushed(k) && pshot.bmp.px) ps_free();
        }
    }
    if (PS.edge == 2) return 1;
    if (PS.mode == PSM_NONE) return 0;
    if (PS.armed) { PS.armed = 0; dt = tk_minf(dt, 1 / 60.0f); }
    int live = tk_spring_step(&PS.sp, dt);
    PS.p = PS.sp.x;
    ps_place();
    if (!live) ps_end();
    return 1;
}

/* sh_touch's hook: the left-edge swipe on a pushed page; 1: the touch is taken */
static int ps_touch(int phase, float x, float y, float t)
{
    if (PS.passing) return 0;
    if (phase == 0) {
        PS.edge = 0;
        ps_finish();                                    /* a slide running ends now (iOS lets the next tap through) */
        if (x >= PS_EDGE || y < SAFE_TOP - 4 || y >= H_PT - 30 || ps_quiet() || !ps_ready() || !ps_pushed(PS.cur) || ps_key() != PS.cur) return 0;
        if (kit_on() || kb_owns(x, y) || CCS.on || focus_on) return 0;
        PS.edge = 1;
        PS.x0 = x; PS.y0 = y; PS.t0 = t;
        tk_tracker_reset(&PS.tr);
        tk_tracker_add(&PS.tr, x, t);
        T.kind = G_NONE;
        return 1;
    }
    if (!PS.edge) return 0;
    float dx = x - PS.x0, dy = y - PS.y0;
    if (PS.edge == 1) {
        if (phase == 1 && dx > 10 && dx > tk_fabsf(dy) * 1.2f) {
            /* the page follows the finger from here (over its parent's picture, or the ground) */
            PS.edge = 2;
            PS.x0 = x;                                  /* (no jump: it follows from where the finger is) */
            PS.mode = PSM_DRAG;
            PS.p = 1;
            ps_begin(PSM_DRAG, 1, 0);
            PS.armed = 0;
            relink();
        } else if (phase == 2 || tk_fabsf(dy) > 10 || dx < -10) {
            /* a tap or a scroll after all: the page gets the touch as it came */
            PS.edge = 0;
            PS.passing = 1;
            sh_touch(0, PS.x0, PS.y0, PS.t0);
            sh_touch(phase, x, y, t);
            PS.passing = 0;
            return 1;
        } else return 1;
    }
    tk_tracker_add(&PS.tr, x, t);
    float w = tk_maxf(0, x - PS.x0);
    if (phase == 1) {
        PS.p = 1 - w / 390;
        ps_place();
        return 1;
    }
    /* let go: past half, or a flick to the right, goes; else back */
    PS.edge = 0;
    float v = tk_tracker_v(&PS.tr), vq = -v * 1000 / 390;    /* pt / ms -> progress / s */
    int go = (w + v * 150 > 195 && v > -.2f) || v > .5f;
    if (!go) {
        PS.mode = PSM_CANCEL;
        ps_spring(PS.p, 1, vq);
        return 1;
    }
    if (PS.kept) {                                     /* slides off over the parent's picture, the back at the end */
        PS.mode = PSM_BACK;
        ps_spring(PS.p, 0, vq);
        return 1;
    }
    /* no parent's picture: the page's picture as it is now, the back now, the picture finishing the slide over it */
    float p0 = w / 390;
    if (!ps_snap(0)) { ps_rest(); ps_do_back(t); return 1; }
    ps_do_back(t);
    PS.p = p0;
    ps_begin(PSM_POP, 0, p0);
    ps_spring(p0, 1, -vq);
    relink();
    return 1;
}
