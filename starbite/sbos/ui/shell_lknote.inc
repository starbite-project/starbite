/* SPDX-License-Identifier: MIT
 * Starbite OS Next (B9): the lock screen's notifications, as iOS 26 shows them (the user's decision 2026-11-12: the
 * notifications above the buttons, the Systems widget group under the clock). Included by shell_lock.c after
 * shell_nc.inc, whose store it reads (NI / nin / nc_unread, nc_app / nc_name / nc_icon / nc_when, nc_launch, nc_view):
 *   - bottom-anchored on y 677 (20.5 pt above the buttons); the unread ones (the Centre marks them read), newest in
 *     front, at most LKN_MAX;
 *   - collapsed: iOS's stack: the newest platter with the lower cards peeking 8 and 15 pt below it (one peek for two);
 *     a new one slides up into the front; a tap on a single notification opens it (nc_launch: the app over home),
 *     on a stack fans it out;
 *   - expanded: the platters 8 pt apart, newest at the bottom, the older ones above it, a "Show less" capsule on top;
 *     the list scrolls (rubber at the ends) when it is taller than the room under the Systems tiles; a tap opens a
 *     notification, "Show less" or a tap beside the platters folds it back;
 *   - the open vendor's platter (place_sys) and the expanded list take turns: one opening closes the other, and the
 *     stack fades out of the platter's way while it grows;
 *   - iOS 26 platters (IOS-REFERENCE 5, measured): x 14.5, w 361, r 23; 24.6 + 18 pt a line; the icon 38 at 13.7 in,
 *     vertically centred; the words from 65; title 15 / 600, the time 13 at 18.3 from the right on its baseline, body
 *     15 / 400 on 18 pt lines (two at most here).
 * Nothing is baked here: a platter is a view of the lock's platter glass (K[0].face, made at boot), the peeks are
 * K[0].gh; the words (one small layer a platter) are drawn when shown (the front one at once, the others two a frame
 * while the stack fans out) and freed when it folds back. */

#define LKN_MAX 8
#define LKN_GAP 8.0f
#define LKN_TXX (PL_X + 65)                     /* the words' column */
#define LKN_TXR (PL_X + PL_W - 18.3f)           /* their right edge (the time's) */
#define LKN_LESS_H 28.0f
typedef struct { unsigned id; tk_layer gl, tx; float h, yb; int nl, built; } lkn_slot;
static lkn_slot LKS[LKN_MAX];
static tk_layer lkng, lknless;
enum { LG_NONE, LG_STACK, LG_ITEM, LG_LESS, LG_AREA, LG_SCROLL };
static struct {
    int n, exp, g, moved, hit;
    unsigned sig;                               /* the store (and the glass) as last shown */
    float p, off, base, in, x0, y0, sa;         /* p: fanned out 0..1; off: the list pulled down; in: a new one's slide */
    tk_spring sp, spo;
    tk_tracker tr;
} LK = { .in = 1, .sa = 1 };

static int lkn_count(void)
{
    int u = nc_unread < nin ? nc_unread : nin;
    return u < LKN_MAX ? u : LKN_MAX;
}
static unsigned lkn_sig(void)
{
    int n = lkn_count();
    unsigned h = 2166136261u ^ (unsigned)n ^ (EMG ? 0x80000000u : 0) ^ (K[0].face.bmp.px ? 0x40000000u : 0);
    for (int i = 0; i < n; i++) h = (h ^ NI[i].id) * 16777619u;
    return h;
}
static float lkn_h(int nl) { return tk_maxf(48, 24.6f + 18 * (1 + nl)); }
static int lkn_lines(const nc_item *n, char ln[4][96])
{
    float wm;
    return n->body[0] ? wrapn(ln, 2, TK_IT400, 15, n->body, LKN_TXR - LKN_TXX, &wm) : 0;
}
/* a platter's words: the app's icon, the title with the time on its right, the body (top: its platter's top) */
static void lkn_draw(lkn_slot *s, const nc_item *n)
{
    char ln[4][96];
    int nl = lkn_lines(n, ln), a = nc_app(n->who);
    float h = s->h, top = LKN_BOT - h;
    relayer(&s->tx);
    dc d = mk(&s->tx, "lkn-item", PL_X, top, PL_W, h, 2, 0);
    nc_icon_who(&d, n->who, a, nc_name(n->who), PL_X + 13.7f, top + (h - 38) / 2, 38);   /* Track E: a Linux app's icon */
    char when[16];
    nc_when(when, n->hm);
    float ww = when[0] ? tw(TK_IT400, 13, when, 0, 0) : 0;
    /* the first line's cap top 15 below the platter's top (centred when the platter is at its least) */
    float b = top + (h - (24.6f + 18 * (1 + nl))) / 2 + 15 + 15 * INTER_CAP;
    d_text_box(&d, TK_IT600, 15, LKN_TXX, b, LKN_TXR - LKN_TXX - (ww ? ww + 8 : 0), 0, n->title[0] ? n->title : nc_name(n->who), 0, 0, INK, 0);
    if (ww) d_text(&d, TK_IT400, 13, LKN_TXR - ww, b, when, 0, 0, W(.6f), 0);
    for (int k = 0; k < nl; k++) d_text(&d, TK_IT400, 15, LKN_TXX, b + 18 * (k + 1), ln[k], 0, 0, W(.92f), 0);
    trim(&s->tx);
    tk_layer_prepare(&s->tx);
    s->yb = top;
    s->built = 1;
}
static void lkn_slot_free(lkn_slot *s)
{
    relayer(&s->tx);
    s->tx.hidden = 1;
    s->built = 0;
    if (s->gl.bmp.px) nc_unview(&s->gl);
    s->gl.hidden = 1;
}
static void lkn_less_build(void)
{
    if (lknless.bmp.px) return;
    float w = tw(TK_IT600, 13, "Show less", 0, 0) + 24, x = PL_X + PL_W - w;
    dc d = mk(&lknless, "lkn-less", x, 0, w, LKN_LESS_H, 2, 0);
    d_rrect(&d, x, 0, w, LKN_LESS_H, LKN_LESS_H / 2, W(.22f));
    d_text(&d, TK_IT600, 13, x + 12, bl(TK_IT600, 13, 6), "Show less", 0, 0, INK, 0);
    trim(&lknless);
    tk_layer_prepare(&lknless);
    lknless.hidden = 1;
}

static void lkn_init(void)
{
    tk_layer_init(&lkng, "lock-notes");
    tk_layer_init(&lknless, "lkn-less"); lknless.hidden = 1;
    for (int i = 0; i < LKN_MAX; i++) { tk_layer_init(&LKS[i].gl, "lkn-glass"); LKS[i].gl.hidden = 1; tk_layer_init(&LKS[i].tx, "lkn-item"); LKS[i].tx.hidden = 1; }
    LK.sig = 0;
}
/* the tree: the peeks under the oldest, the front platter last (on top) */
static void lkn_link(void)
{
    tk_layer *c[2 * LKN_MAX + 2];
    int k = 0;
    c[k++] = &K[0].gh;
    for (int i = LKN_MAX - 1; i >= 0; i--) if (LKS[i].id) { c[k++] = &LKS[i].gl; c[k++] = &LKS[i].tx; }
    c[k++] = &lknless;
    link(&lkng, c, k);
}

/* where the platters are: slot i's top (screen pt) at fan-out p, and the "Show less" row's */
static float lkn_front_c(void) { return LKN_BOT - LKS[0].h - (LK.n >= 3 ? PEEK2 : LK.n == 2 ? PEEK1 : 0); }
static float lkn_top_e(int i)
{
    float y = LKN_BOT - LKS[0].h;
    for (int k = 1; k <= i && k < LK.n; k++) y -= LKN_GAP + LKS[k].h;
    return y + LK.off;
}
static float lkn_less_e(void) { return lkn_top_e(LK.n - 1) - 8 - LKN_LESS_H; }
static float lkn_max_off(void) { return LK.n ? tk_maxf(0, LKN_TOP - (lkn_less_e() - LK.off)) : 0; }
static float lkn_y(int i)
{
    float c = lkn_front_c() + (1 - LK.in) * 34;
    return c + (lkn_top_e(i) - c) * LK.p;
}
/* a view of the platter glass at (PL_X, y), h tall */
static void lkn_glass(tk_layer *v, float y, float h, float a)
{
    card_t *k = &K[0];
    if (!k->face.bmp.px || a <= .004f) { v->hidden = 1; return; }
    if (v->bmp.px != k->face.bmp.px) {
        nc_view(v, &k->face);
        /* the face's glass shapes too: without them a view is only the faint baked tint (no lens, no backdrop) */
        v->ngsh = k->face.ngsh;
        tk_memcpy(v->gsh, k->face.gsh, sizeof v->gsh);
        tk_memcpy(v->gvar, k->face.gvar, sizeof v->gvar);
        tk_memcpy(v->gtint, k->face.gtint, sizeof v->gtint);
        tk_memcpy(v->gpress, k->face.gpress, sizeof v->gpress);
    }
    platter_at(v, y, h);
    v->alpha = a;
    v->hidden = 0;
}
static void lkn_place(void)
{
    int on = !EMG && LK.n > 0 && K[0].face.bmp.px;
    lkng.hidden = !on;
    if (!on) { lock_ver++; return; }
    /* out of the open vendor's way: it fades as the vendor's platter reaches it */
    float cf = lkn_front_c();
    LK.sa = SY.g >= 0 ? tk_clampf((cf - 4 - (SYC_Y + SY.h + 12)) / 16, 0, 1) : 1;
    float p = LK.p, sa = LK.sa;
    for (int i = 0; i < LKN_MAX; i++) {
        lkn_slot *s = &LKS[i];
        if (!s->id) continue;
        float y = lkn_y(i), a = (i ? tk_clampf(p * 2.2f - .6f - .1f * i, 0, 1) : LK.in) * sa;   /* the others once apart */
        lkn_glass(&s->gl, y, s->h, a);
        if (s->built && s->tx.bmp.px) {
            s->tx.y = s->tx.by + tk_floorf(PT(y - s->yb) + .5f);
            s->tx.alpha = a;
            s->tx.hidden = a <= .004f;
        } else
            s->tx.hidden = 1;
    }
    /* the lower cards under the front (one for two notifications): they fade as the stack fans out */
    tk_layer *g = &K[0].gh;
    float pa = LK.n >= 2 ? tk_clampf(1 - p * 2.5f, 0, 1) * sa * LK.in : 0;
    if (g->bmp.px && pa > .004f) {
        float y = lkn_y(0), h = LKS[0].h;
        g->y = tk_floorf(PT(y + h - 68 - (LIST_TOP + K[0].y)) + g->by + .5f);
        g->alpha = pa;
        g->clip = 1;
        g->clip_r = (tk_rect){ 0, PT(y), (float)TK_W, PT(h + (LK.n == 2 ? PEEK1 + .5f : PEEK2 + 4)) };
        g->hidden = 0;
    } else
        g->hidden = 1;
    if (lknless.bmp.px) {
        float a = tk_clampf(p * 2 - 1, 0, 1);
        lknless.y = lknless.by + tk_floorf(PT(lkn_less_e()) + .5f);
        lknless.alpha = a;
        lknless.hidden = a <= .004f || LK.n < 2;
    }
    /* the list's room: under the Systems tiles; faded at its top when it is pulled under them */
    lkng.clip = 1;
    lkng.clip_r = (tk_rect){ 0, PT(LKN_TOP - 6), (float)TK_W, PT(LKN_BOT + 44 - (LKN_TOP - 6)) };
    lkng.fade_top = p > 0 && lkn_max_off() > 0 ? PT(16) : 0;
    lock_ver++;
}

/* the store as it is now: slot i holds the i-th newest unread (its words made again only if it changed) */
static void lkn_sync(int animate)
{
    unsigned old0 = LKS[0].id;
    int n = lkn_count(), glass = !EMG && K[0].face.bmp.px;
    LK.sig = lkn_sig();
    if (!glass) n = 0;
    for (int i = 0; i < LKN_MAX; i++) {
        lkn_slot *s = &LKS[i];
        unsigned id = i < n ? NI[i].id : 0;
        if (s->id == id && (s->built || !id)) continue;
        if (s->id != id) lkn_slot_free(s);
        s->id = id;
        if (!id) continue;
        char ln[4][96];
        s->nl = lkn_lines(&NI[i], ln);
        s->h = lkn_h(s->nl);
        if (i == 0 || LK.exp || LK.p > 0) lkn_draw(s, &NI[i]);
    }
    LK.n = n;
    if (n < 2 && LK.exp) { LK.exp = 0; LK.sp.live = 0; LK.p = 0; LK.off = 0; }
    /* a new notification slides up into the front (iOS); none while the stack is fanned out or for a still */
    if (animate && n && LKS[0].id != old0 && LKS[0].id > old0 && !LK.exp) LK.in = 0;
    else if (!animate) LK.in = 1;
    if (n && !lknless.bmp.px && glass) lkn_less_build();
    relink();
    lkn_place();
}
static void lkn_free(void)
{
    for (int i = 0; i < LKN_MAX; i++) { lkn_slot_free(&LKS[i]); LKS[i].id = 0; }
    relayer(&lknless); lknless.hidden = 1;
    LK.n = 0; LK.exp = 0; LK.p = 0; LK.off = 0; LK.in = 1;
    LK.sp.live = LK.spo.live = 0;
    LK.sig = 0;
    lkng.hidden = 1;
}
static void lkn_expand(void)
{
    if (LK.n < 2) return;
    sys_close(0);                                /* the vendor's platter makes room */
    LK.exp = 1;
    LK.off = 0; LK.spo.live = 0;
    tk_spring_start(&LK.sp, LK.p, 1, 0, TK_SPR_SHEET);
}
static void lkn_collapse(int instant)
{
    if (!LK.exp && LK.p <= 0) return;
    LK.exp = 0;
    if (instant) { LK.p = 0; LK.off = 0; LK.sp.live = LK.spo.live = 0; for (int i = 1; i < LKN_MAX; i++) if (LKS[i].built) { relayer(&LKS[i].tx); LKS[i].tx.hidden = 1; LKS[i].built = 0; } lkn_place(); return; }
    tk_spring_start(&LK.sp, LK.p, 0, 0, TK_SPR_SHEET);
    if (LK.off != 0) tk_spring_start(&LK.spo, LK.off, 0, 0, TK_SPR_SHEET);
}
/* the words still to draw for a fanned-out stack (two a frame; all at once for a still) */
static int lkn_build_some(int max)
{
    int b = 0;
    for (int i = 0; i < LK.n && b < max; i++)
        if (LKS[i].id && !LKS[i].built) { lkn_draw(&LKS[i], &NI[i]); b++; }
    if (b) relink();
    return b;
}
static int lkn_tick(float dt, float t)
{
    (void)t;
    int mv = 0;
    if (LK.sig != lkn_sig()) { lkn_sync(locked && lockY == 0); mv = 1; }
    if (LK.sp.live) {
        int live = tk_spring_step(&LK.sp, dt);
        LK.p = tk_maxf(0, LK.sp.x);
        if (!live && !LK.exp) {
            LK.p = 0;
            for (int i = 1; i < LKN_MAX; i++) if (LKS[i].built) { relayer(&LKS[i].tx); LKS[i].tx.hidden = 1; LKS[i].built = 0; }
        }
        mv = 1;
    }
    if (LK.spo.live) { tk_spring_step(&LK.spo, dt); LK.off = LK.spo.x; mv = 1; }
    if (LK.in < 1) { LK.in = tk_minf(1, LK.in + dt / .45f); mv = 1; }
    if ((LK.exp || LK.p > 0) && lkn_build_some(2)) mv = 1;
    if (LK.g == LG_SCROLL) mv = 1;
    if (mv) lkn_place();
    return mv;
}
static int lkn_moving(void)
{
    return LK.sp.live || LK.spo.live || LK.in < 1 || LK.g == LG_SCROLL || LK.sig != lkn_sig();
}
/* idle: the store as it is (no slide: nothing moves now), the half copies of the words */
static void lkn_idle(void)
{
    if (LK.sig != lkn_sig()) lkn_sync(0);
    for (int i = 0; i < LKN_MAX; i++) if (LKS[i].built && LKS[i].tx.bmp.px) tk_layer_prepare(&LKS[i].tx);
}

/* the minute: the times on the platters ("5m ago") */
static void lkn_minute(void)
{
    int any = 0;
    for (int i = 0; i < LK.n; i++) if (LKS[i].built) { lkn_draw(&LKS[i], &NI[i]); any = 1; }
    if (any) { relink(); lkn_place(); }
}

/* ---- touches ---------------------------------------------------------------------------------------------------- */
static int lkn_slot_at(float x, float y)
{
    if (x < PL_X || x >= PL_X + PL_W) return -1;
    for (int i = 0; i < LK.n; i++) {
        float t = lkn_y(i);
        if (y >= t && y < t + LKS[i].h) return i;
    }
    return -1;
}
/* a touch's start on the lock (sh_touch, after the buttons and the Systems group): 1 if it is the notifications' */
static int lkn_down(float x, float y, float t)
{
    if (EMG || LK.n == 0 || lkng.hidden || LK.sa < .5f) return 0;
    LK.x0 = x; LK.y0 = y; LK.moved = 0; LK.hit = -1;
    tk_tracker_reset(&LK.tr);
    tk_tracker_add(&LK.tr, y, t);
    if (!LK.exp) {
        if (y < lkn_front_c() || y > LKN_BOT + 2 || x < PL_X || x >= PL_X + PL_W) return 0;
        LK.g = LG_STACK;
        return 1;
    }
    if (y < LKN_TOP - 6 || y > LKN_BOT + 4) return 0;
    float ly = lkn_less_e();
    if (!lknless.hidden && y >= ly - 6 && y < ly + LKN_LESS_H + 6 && x >= PL_X + PL_W - 120) { LK.g = LG_LESS; return 1; }
    LK.hit = lkn_slot_at(x, y);
    LK.g = LK.hit >= 0 ? LG_ITEM : LG_AREA;
    LK.base = LK.off;
    LK.spo.live = 0;
    return 1;
}
/* the rest of it: 2 hands the touch to the lock's swipe (a drag that is not the list's scroll) */
static int lkn_touch(int ph, float x, float y, float t)
{
    float dx = x - LK.x0, dy = y - LK.y0;
    if (ph == 1) {
        tk_tracker_add(&LK.tr, y, t);
        if (!LK.moved && (tk_fabsf(dx) > 8 || tk_fabsf(dy) > 8)) {
            LK.moved = 1;
            if (LK.exp && lkn_max_off() > 0 && tk_fabsf(dy) > tk_fabsf(dx)) LK.g = LG_SCROLL;
            else { LK.g = LG_NONE; return 2; }
        }
        if (LK.g == LG_SCROLL) {
            float o = LK.base + dy, m = lkn_max_off();
            LK.off = o < 0 ? -rubber(-o, LKN_BOT - LKN_TOP) : o > m ? m + rubber(o - m, LKN_BOT - LKN_TOP) : o;
            lkn_place();
        }
        return 0;
    }
    if (ph != 2) return 0;
    int g = LK.g;
    LK.g = LG_NONE;
    if (g == LG_SCROLL) {
        float m = lkn_max_off();
        if (LK.off < 0 || LK.off > m) tk_spring_start(&LK.spo, LK.off, LK.off < 0 ? 0 : m, 0, TK_SPR_ROWS);
        return 0;
    }
    if (LK.moved) return 0;
    switch (g) {
    case LG_STACK:
        if (LK.n == 1) nc_launch(LKS[0].id);      /* the one notification: its app (the lock goes, as from the Centre) */
        else lkn_expand();
        break;
    case LG_ITEM:
        if (LK.hit >= 0 && LK.hit < LK.n) nc_launch(LKS[LK.hit].id);
        break;
    case LG_LESS: case LG_AREA:
        lkn_collapse(0);
        break;
    }
    return 0;
}

/* the host's stills: "lkn" p (the stack fanned out to p: 0 folded, 1 open) */
static int lkn_set(const char *what, const char *key, float v)
{
    (void)key;
    if (tk_strcmp(what, "lkn")) return 0;
    if (LK.sig != lkn_sig()) lkn_sync(0);
    if (v > 0 && LK.n >= 2) { sys_close(1); LK.exp = 1; }
    else LK.exp = 0;
    LK.sp.live = LK.spo.live = 0;
    LK.p = LK.exp ? v : 0;
    LK.off = 0;
    lkn_build_some(LKN_MAX);
    lkn_place();
    return 1;
}
