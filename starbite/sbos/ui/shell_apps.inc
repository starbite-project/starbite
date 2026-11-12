/* SPDX-License-Identifier: MIT
 * helper B5: the core apps' shared parts (included by shell_lock.c before shell_comms.inc):
 *   - SCV, a scrolling list of drawn items (Messages' threads and bubbles, Phone's Recents and Favourites, Clock's
 *     lists): each item is a small layer of its own, drawn when it comes into view and freed when it leaves, so a list
 *     costs about the screen's area at most and a change redraws only the item that changed (a sent bubble, a row's
 *     dot), never the page. iOS's scrolling: the finger, momentum (.998 / ms), rubber bands and the rows spring;
 *   - the contact-like avatar (a monogram on iOS's grey gradient, or the person glyph for a number);
 *   - the dock's badges (missed calls, unread messages): one small layer over home's dock, never the dock's glass.
 * Nothing here bakes glass or blurs; every list redraw is per item. */

/* ---- the list ---------------------------------------------------------------------------------------------- */
#define SCV_POOL 40                         /* items on screen at most (a 30 pt row fits 25 in a page) */
#define SCV_MAXI 216
typedef struct {
    int on;
    float top, bot;                         /* the viewport (screen pt) */
    float fade_t, fade_b;                   /* iOS's scroll edges: content fades out under the bars (pt) */
    int n;
    float iy[SCV_MAXI], ih[SCV_MAXI];       /* items' tops in the content (0 = the viewport's top at scroll 0), heights */
    uint32_t ik[SCV_MAXI];                  /* items' keys: a kept bitmap is reused while its key and height stay */
    float h;                                /* the content's height */
    float s, v, base, x0, y0;               /* the scroll (pt; 0 = the top) */
    int momentum, down, moved;
    tk_spring sp;
    tk_tracker tr;
    void (*draw)(dc *d, int i, float top);  /* item i with its top at screen y top (the scroll at 0) */
    tk_layer g;                             /* the clipped group */
    tk_layer L[SCV_POOL];
    uint32_t sk[SCV_POOL];                  /* the slot's item key (0: free) */
    float sy[SCV_POOL];                     /* the content y it was drawn at */
    float sh[SCV_POOL];
} scv_t;
static scv_t LS;                            /* the open app's list (one app is open at a time) */

static float scv_max(const scv_t *v) { return tk_maxf(0, v->h - (v->bot - v->top)); }
static void scv_free_slot(scv_t *v, int k)
{
    relayer(&v->L[k]);
    v->L[k].hidden = 1;
    v->sk[k] = 0;
}
/* every item's bitmap gone (a new page) */
static void scv_clear(scv_t *v)
{
    for (int k = 0; k < SCV_POOL; k++) if (v->sk[k] || v->L[k].bmp.px) scv_free_slot(v, k);
}
static void scv_off(scv_t *v)
{
    scv_clear(v);
    v->on = 0;
    v->g.hidden = 1;
    v->momentum = v->down = 0;
    v->sp.live = 0;
}
static int scv_round_px(float pt) { return (int)tk_floorf(PT(pt) + .5f); }
/* the items in view drawn (if they are not), the rest freed, everything placed at the scroll */
static void scv_place(scv_t *v)
{
    if (!v->on) return;
    float pre = 24;
    float a = v->s - pre, b = v->s + (v->bot - v->top) + pre;
    int sp = scv_round_px(v->s);
    /* slots: keep the ones whose item is still here (by key) and in view; move them to its place */
    int at[SCV_POOL];
    for (int k = 0; k < SCV_POOL; k++) {
        at[k] = -1;
        if (!v->sk[k]) continue;
        int i = 0;
        while (i < v->n && v->ik[i] != v->sk[k]) i++;
        if (i == v->n || v->ih[i] != v->sh[k] || v->iy[i] + v->ih[i] < a || v->iy[i] > b) { scv_free_slot(v, k); continue; }
        at[k] = i;
    }
    for (int i = 0; i < v->n; i++) {
        if (v->iy[i] + v->ih[i] < a || v->iy[i] > b) continue;
        int k = 0;
        while (k < SCV_POOL && at[k] != i) k++;
        if (k == SCV_POOL) {
            for (k = 0; k < SCV_POOL && v->sk[k]; k++) ;
            if (k == SCV_POOL) break;                       /* (never: the pool covers a page) */
            float top = v->top + v->iy[i];
            tk_layer *nx = v->L[k].next;               /* (mk inits the layer: its place in the list is kept) */
            dc d = mk(&v->L[k], "list-item", 0, top, 390, v->ih[i], 0, 0);
            v->L[k].next = nx;
            v->sk[k] = v->ik[i]; v->sy[k] = v->iy[i]; v->sh[k] = v->ih[i];
            at[k] = i;
            if (d.b->px) { v->draw(&d, i, top); trim(&v->L[k]); }
        }
        tk_layer *l = &v->L[k];
        l->hidden = !l->bmp.px;
        l->x = l->bx;
        l->y = l->by + (float)(scv_round_px(v->iy[i]) - scv_round_px(v->sy[k]) - sp);
        l->gen++;
    }
    v->g.clip = 1;
    v->g.clip_r = (tk_rect){ 0, PT(v->top), TK_W, PT(v->bot - v->top) };
    v->g.fade_top = v->s > 1 ? PT(v->fade_t) : 0;          /* nothing under the bar at the top: no fade there */
    v->g.fade_bot = v->s < scv_max(v) - 1 ? PT(v->fade_b) : 0;
    v->g.hidden = 0;
    v->g.gen++;
}
/* a new list (the items' iy / ih / ik filled by the caller, n items): the content's height, the scroll kept in range */
static void scv_layout(scv_t *v, int n)
{
    v->n = n < SCV_MAXI ? n : SCV_MAXI;
    v->h = v->n ? v->iy[v->n - 1] + v->ih[v->n - 1] : 0;
    v->s = tk_clampf(v->s, 0, scv_max(v));
    v->on = 1;
}
/* item i drawn again (its look changed but not its key) */
__attribute__((unused)) static void scv_redraw(scv_t *v, int i)
{
    for (int k = 0; k < SCV_POOL; k++) if (v->sk[k] && v->sk[k] == v->ik[i]) scv_free_slot(v, k);
}
static void scv_to_end(scv_t *v) { v->s = scv_max(v); v->momentum = 0; v->sp.live = 0; }
static int scv_at_end(const scv_t *v) { return v->s >= scv_max(v) - 2; }
/* the item under screen y (or -1), its screen top in *top */
static int scv_hit(const scv_t *v, float y, float *top)
{
    if (!v->on || y < v->top || y >= v->bot) return -1;
    float cy = y - v->top + v->s;
    for (int i = 0; i < v->n; i++)
        if (cy >= v->iy[i] && cy < v->iy[i] + v->ih[i]) { if (top) *top = v->top + v->iy[i] - v->s; return i; }
    return -1;
}
__attribute__((unused)) static float scv_screen_y(const scv_t *v, int i) { return v->top + v->iy[i] - v->s; }
static int scv_contains(const scv_t *v, float x, float y)
{
    (void)x;
    return v->on && v->n && y >= v->top && y < v->bot && y < H_PT - 30;
}
/* the finger: 0 down, 1 move, 2 up; returns 1 at an up that was a tap (no move) */
static int scv_touch(scv_t *v, int ph, float x, float y, float t)
{
    if (ph == 0) {
        v->down = 1; v->moved = 0; v->x0 = x; v->y0 = y; v->base = v->s;
        v->momentum = 0; v->sp.live = 0;
        tk_tracker_reset(&v->tr);
        tk_tracker_add(&v->tr, y, t);
        return 0;
    }
    float dy = y - v->y0;
    if (ph == 1) {
        tk_tracker_add(&v->tr, y, t);
        if (!v->moved && (tk_fabsf(dy) > 6 || tk_fabsf(x - v->x0) > 8)) { v->moved = 1; v->base = v->s + (dy > 0 ? 6 : dy < 0 ? -6 : 0); }
        if (!v->moved) return 0;
        float s = v->base - dy, m = scv_max(v), vp = v->bot - v->top;
        if (s < 0) s = -md_rubber(-s, vp);
        else if (s > m) s = m + md_rubber(s - m, vp);
        v->s = s;
        scv_place(v);
        return 0;
    }
    v->down = 0;
    if (!v->moved) return 1;
    float vel = -tk_tracker_v(&v->tr), m = scv_max(v);
    if (v->s < 0 || v->s > m) tk_spring_start(&v->sp, v->s, v->s < 0 ? 0 : m, vel * 1000, TK_SPR_ROWS);
    else if (tk_fabsf(vel) > .05f) { v->v = vel; v->momentum = 1; }
    return 0;
}
static int scv_moving(const scv_t *v) { return v->on && ((v->down && v->moved) || v->momentum || v->sp.live); }
static int scv_tick(scv_t *v, float dt)
{
    if (!v->on) return 0;
    if (v->sp.live) {
        tk_spring_step(&v->sp, dt);
        v->s = v->sp.x;
        scv_place(v);
        return 1;
    }
    if (v->momentum) {
        float ms = dt * 1000, m = scv_max(v), decay = tk_powf(.998f, ms);
        v->s += v->v * (1 - decay) / (1 - .998f);
        v->v *= decay;
        if (v->s < 0 || v->s > m) {
            v->momentum = 0;
            /* past an end: a short rubber overshoot from its speed, then the spring back */
            tk_spring_start(&v->sp, v->s, v->s < 0 ? 0 : m, v->v * 1000, TK_SPR_ROWS);
        } else if (tk_fabsf(v->v) < .01f) v->momentum = 0;
        scv_place(v);
        return 1;
    }
    return 0;
}
static int scv_layers(scv_t *v, tk_layer **o)
{
    if (!v->g.name) { tk_layer_init(&v->g, "list"); v->g.hidden = 1; }
    for (int k = 0; k < SCV_POOL; k++) if (!v->L[k].name) { tk_layer_init(&v->L[k], "list-item"); v->L[k].hidden = 1; }
    tk_layer *c[SCV_POOL];
    for (int k = 0; k < SCV_POOL; k++) c[k] = &v->L[k];
    link(&v->g, c, SCV_POOL);
    o[0] = &v->g;
    return 1;
}

/* the large-title lists: the viewport's top under the nav row, the title the first item; the inline title in the
 * nav row once the large one has scrolled under it */
#define LIST_T0 (SAFE_TOP + NAVBAR_H)
#define TITLE_H (LARGE_H + 10)
static tk_layer lsnav;
static int ls_used;                          /* a page of this build used LS (else cm_extras, the last, turns it off) */
static void ls_moved(void)
{
    int show = lsnav.bmp.px && LS.on && LS.s > TITLE_H - 16;
    if (lsnav.hidden == show) { lsnav.hidden = !show; lsnav.gen++; }
}
static void ls_nav_build(const char *t)
{
    relayer(&lsnav);
    lsnav.hidden = 1;
    if (!t) return;
    dc d = mk(&lsnav, "nav-title", 80, SAFE_TOP, 230, NAVBAR_H, 0, 0);
    d_text_box(&d, TK_IT600, 17, 195, NAV_BASE, 220, 1, t, 0, 0, INK, 0);
    trim(&lsnav);
}
static int ls_layers(tk_layer **o)
{
    int n = scv_layers(&LS, o);
    if (!lsnav.name) { tk_layer_init(&lsnav, "nav-title"); lsnav.hidden = 1; }
    o[n++] = &lsnav;
    return n;
}

/* ---- a page's strips cut to what they hold (the apps here draw their lists as items: a strip is mostly empty) -- */
static int ap_slim(const char *k)
{
    return !tk_strcmp(k, "phone") || !tk_strcmp(k, "messages") || !tk_strcmp(k, "clock") || !tk_strcmp(k, "calc") ||
           !tk_strcmp(k, "notes") || !tk_strcmp(k, "camera") || !tk_strcmp(k, "contacts");
}
static void slim_strip(tk_layer *l)
{
    const tk_bmp *b = &l->bmp;
    if (!b->px) return;
    size_t n = (size_t)b->w * b->h;
    size_t i = 0;
    while (i < n && !b->px[i]) i++;
    if (i == n) { relayer(l); l->hidden = 1; return; }      /* nothing drawn in it: no bitmap at all */
    trim(l);
}

/* ---- the contact-like avatar ------------------------------------------------------------------------------- */
/* iOS's monogram: a grey gradient disc with the initials (a name), or the person (a number or nothing) */
static void d_person(dc *d, float cx, float cy, float r);
static void d_monogram(dc *d, float cx, float cy, float r, const char *who)
{
    int letters = who && ((who[0] >= 'A' && who[0] <= 'Z') || (who[0] >= 'a' && who[0] <= 'z'));
    if (!letters) { d_person(d, cx, cy, r); return; }
    tk_stop st[2] = { { 0, tk_hex(0xa5a9b4, 1) }, { 1, tk_hex(0x7c808c, 1) } };
    tk_paint g = tk_linear(180, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), 2, st);
    tk_fill_rrect(d->b, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), PT(r), &g, 1);
    char in[3];
    int k = 0;
    in[k++] = (char)(who[0] >= 'a' ? who[0] - 32 : who[0]);
    for (const char *c = who; *c && k < 2; c++)
        if (*c == ' ' && c[1] && c[1] != ' ') in[k++] = (char)(c[1] >= 'a' && c[1] <= 'z' ? c[1] - 32 : c[1]);
    in[k] = 0;
    float fs = r * .82f, w = tw(TK_IT600, fs, in, 0, 0);
    d_text(d, TK_IT600, fs, cx - w / 2, bl(TK_IT600, fs, cy - lh(TK_IT600, fs) / 2), in, 0, 0, W(1), 0);
}

/* ---- the dock's badges ----------------------------------------------------------------------------------------- */
static int dk_drawn[2] = { -1, -1 };
static int cm_badge_calls(void);           /* shell_comms.inc */
static int cm_badge_msgs(void);
/* iOS's badge: a red capsule (23 pt high) at the icon's top-right, the count in white 16 pt */
static void d_badge(dc *d, float cx, float cy, int n)
{
    char b[8];
    int k = n > 99 ? (b[0] = '9', b[1] = '9', b[2] = '+', b[3] = 0, 3) : put_dec(b, n);
    b[k] = 0;
    float w = tw(TK_IT500, 15, b, 0, TK_TNUM), bw = tk_maxf(23, w + 14);
    d_rrect(d, cx - bw / 2, cy - 11.5f, bw, 23, 11.5f, tk_hex(0xff4245, 1));
    d_text(d, TK_IT500, 15, cx - w / 2, bl(TK_IT500, 15, cy - 9.5f), b, 0, TK_TNUM, W(1), 0);
}
static void dk_badges(void)
{
    int c = cm_badge_calls(), m = cm_badge_msgs();
    if (c == dk_drawn[0] && m == dk_drawn[1] && (dkbadge.bmp.px || (!c && !m))) return;
    dk_drawn[0] = c; dk_drawn[1] = m;
    relayer(&dkbadge);
    dkbadge.hidden = 1;
    if (!c && !m) return;
    /* the dock's first two icons (Phone, Messages): their top-right corners */
    dc d = mk(&dkbadge, "dock-badges", COL_CX(0), DOCK_Y - 14, COL_CX(1) - COL_CX(0) + 60, 30, 0, 0);
    if (!d.b->px) return;
    if (c) d_badge(&d, COL_CX(0) + ICON_PT / 2 - 5, DOCK_Y + 5, c);
    if (m) d_badge(&d, COL_CX(1) + ICON_PT / 2 - 5, DOCK_Y + 5, m);
    trim(&dkbadge);
    dkbadge.hidden = 0;
    relink();
}
