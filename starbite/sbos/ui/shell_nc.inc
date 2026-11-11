/* SPDX-License-Identifier: MIT
 * Starbite OS Next (helper D): notifications, as iOS 26 shows them. Included by shell_lock.c after the other parts (it
 * uses the shell's drawing helpers, layers and app state).
 *   - the store: the last NC_MAX notifications from the platform (sh_notify: the kernel's notif.c posts, copied by
 *     kern_next.c's bridge outside the frames; the host's films post pretend ones), newest first, RAM only;
 *   - the Notification Centre: pulled down from the top-left half of the status bar (home or an app), or opened from
 *     the lock's "N notifications" pill. Lock-screen style: the wallpaper, the lock's clock and date, then the list:
 *     platters grouped by app, a group stacked with the lock cards' peeks (a tap fans it out with "Show less" and
 *     "Clear"), a swipe left clears (a short one shows the Clear button), a tap opens the app. It follows the finger
 *     (rubber past full) and hands the finger's speed to its spring as the Control Centre's drag does; a swipe up, or
 *     the home bar, closes it. "No Older Notifications" when the list is empty;
 *   - the banner: a new notification on home or in an app slides down as a platter, goes away after 4 s (a timed
 *     refresh: no frames while it rests), a tap opens the app, a swipe up dismisses it;
 *   - the lock: its list stays the Systems list; a small pill above the hold buttons says how many are unread.
 * Nothing here bakes glass: a platter is a view of a lock card's glass face (made at boot), drawn at any height as
 * its 3-slice (a solid rounded platter while Emergency has those cards freed); its words are drawn when it comes into
 * view (two a frame), and everything is freed when the Centre closes. */

static void set_lock(float y);
static float rubber(float over, float dim);
static int sw_on(void);                    /* shell_switcher.inc */

/* ---- the store --------------------------------------------------------------------------------------------- */
#define NC_MAX 20
typedef struct { char who[16], title[40], body[72]; int hm; unsigned id; } nc_item;
static nc_item NI[NC_MAX];                 /* newest first */
static int nin, nc_unread;
static unsigned nc_ids;

static void nc_cpy(char *d, int cap, const char *s)
{
    int i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    if (s && s[i]) {                        /* cut: whole characters, then an ellipsis */
        i = cap - 4;
        while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
        while (i > 0 && s[i - 1] == ' ') i--;
        d[i++] = (char)0xe2; d[i++] = (char)0x80; d[i++] = (char)0xa6;
    }
    d[i] = 0;
}
/* the poster's app (it opens on a tap) and the name shown: the kernel's posters, else an app of that key */
typedef struct { const char *who, *app, *name; } nc_src;
static const nc_src NCSRC[] = { { "clock", "clock", "Clock" }, { "battery", "battery", "Battery" }, { "modem", "settings", "Cellular" },
                                { "heat", "heat", "Survival Heat" }, { "survival", "usb", "USB Power" }, { "wifi", "settings", "Wi-Fi" } };
static int nc_app(const char *who)
{
    const char *k = who;
    for (unsigned i = 0; i < sizeof NCSRC / sizeof NCSRC[0]; i++) if (!tk_strcmp(NCSRC[i].who, who)) k = NCSRC[i].app;
    for (int a = 0; a < NAPPS; a++) if (!tk_strcmp(APPS[a].k, k)) return a;
    return -1;
}
static const char *nc_name(const char *who)
{
    for (unsigned i = 0; i < sizeof NCSRC / sizeof NCSRC[0]; i++) if (!tk_strcmp(NCSRC[i].who, who)) return NCSRC[i].name;
    int a = nc_app(who);
    return a >= 0 ? APPS[a].name : who;
}
static int nc_find(unsigned id) { for (int i = 0; i < nin; i++) if (NI[i].id == id) return i; return -1; }

/* ---- drawing a platter's words ------------------------------------------------------------------------------- */
#define NC_W CARD_W
#define NC_TXW (NC_W - 64 - 16)            /* the words' column */
/* a premultiplied icon at f x its size, its top-left at (X, Y) px of dst (2 x 2 bilinear taps a px: a 62 pt tile
 * down to 38 or 26 pt stays smooth) */
static void nc_blit_scaled(tk_bmp *dst, const tk_bmp *src, float X, float Y, float f)
{
    int x0 = (int)tk_floorf(X), y0 = (int)tk_floorf(Y), w = (int)(src->w * f) + 2, h = (int)(src->h * f) + 2;
    for (int j = 0; j < h; j++) {
        int dy = y0 + j;
        if (dy < 0 || dy >= dst->h) continue;
        uint32_t *o = dst->px + (size_t)dy * dst->stride;
        for (int i = 0; i < w; i++) {
            int dx = x0 + i;
            if (dx < 0 || dx >= dst->w) continue;
            float acc[4] = { 0, 0, 0, 0 };
            for (int q = 0; q < 4; q++) {
                float sx = (dx + .25f + .5f * (q & 1) - X) / f - .5f, sy = (dy + .25f + .5f * (q >> 1) - Y) / f - .5f;
                int ix = (int)tk_floorf(sx), iy = (int)tk_floorf(sy);
                float fx = sx - ix, fy = sy - iy;
                for (int t = 0; t < 4; t++) {
                    int u = ix + (t & 1), v = iy + (t >> 1);
                    if (u < 0 || v < 0 || u >= src->w || v >= src->h) continue;
                    float wgt = ((t & 1) ? fx : 1 - fx) * ((t >> 1) ? fy : 1 - fy) * .25f;
                    uint32_t p = src->px[(size_t)v * src->stride + u];
                    for (int c = 0; c < 4; c++) acc[c] += (float)(p >> (8 * c) & 255) * wgt;
                }
            }
            if (acc[3] < .5f) continue;
            uint32_t d = o[dx], r = 0;
            float ia = 1 - acc[3] / 255;
            for (int c = 0; c < 4; c++) {
                float v = acc[c] + (float)(d >> (8 * c) & 255) * ia;
                r |= (uint32_t)(v > 255 ? 255 : v + .5f) << (8 * c);
            }
            o[dx] = r;
        }
    }
}
/* an app's icon at s pt (Emergency's drawn tiles, the pack's 62 pt tiles scaled, else a grey tile with an initial) */
static void nc_icon(dc *d, int a, const char *name, float x, float y, float s)
{
    if (a >= 0 && sv_tile(d, APPS[a].k, x, y, s)) return;
    float pad;
    const tk_bmp *ic = a >= 0 ? app_icon(APPS[a].k, &pad) : 0;
    if (ic) { float f = s / 62; nc_blit_scaled(d->b, ic, PT(x) - pad * f - d->ox, PT(y) - pad * f - d->oy, f); return; }
    d_rrect(d, x, y, s, s, s * .225f, tk_hex(0x8e8e93, 1));
    char in[2] = { name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0 };
    d_text(d, TK_IT600, s * .5f, x + s / 2 - tw(TK_IT600, s * .5f, in, 0, 0) / 2, y + s * .68f, in, 0, 0, W(1), 0);
}
/* "now", "5m ago", "2h ago" (iOS's), else the time it came */
static void nc_when(char *b, int hm)
{
    b[0] = 0;
    if (hm < 0) return;
    int d = ST.h * 60 + ST.mi - (hm / 100 * 60 + hm % 100), n = 0;
    if (d == 0) { tk_memcpy(b, "now", 4); return; }
    if (d > 0 && d < 24 * 60) {
        n = put_dec(b, d < 60 ? d : d / 60);
        tk_memcpy(b + n, d < 60 ? "m ago" : "h ago", 6);
        return;
    }
    b[0] = (char)('0' + hm / 1000 % 10); b[1] = (char)('0' + hm / 100 % 10); b[2] = ':';
    b[3] = (char)('0' + hm / 10 % 10); b[4] = (char)('0' + hm % 10); b[5] = 0;
}
static int nc_lines(const nc_item *n, char ln[4][96])
{
    float wm;
    return n->body[0] ? wrapn(ln, 2, TK_IT400, 15, n->body, NC_TXW, &wm) : 0;
}
static float nc_hh(int nl) { return nl >= 2 ? 86 : 68; }   /* the lock card's glass is 68 pt at the least (its 3-slice) */
/* iOS 26's platter: the app's icon (38 pt), the title (15 / 600) with the time (13, secondary) on its right, up to
 * two lines of text (15 / 400) */
static void nc_draw_item(dc *d, const nc_item *n, float x, float top, float h, int nl, char ln[4][96])
{
    int a = nc_app(n->who);
    nc_icon(d, a, nc_name(n->who), x + 14, top + (h - 38) / 2, 38);
    float tx = x + 64, right = x + NC_W - 16;
    char when[16];
    nc_when(when, n->hm);
    float ww = when[0] ? tw(TK_IT400, 13, when, 0, 0) : 0, t0 = top + (h - (20 + 19.0f * nl)) / 2, b1 = bl(TK_IT600, 15, t0);
    d_text_box(d, TK_IT600, 15, tx, b1, right - tx - (ww ? ww + 8 : 0), 0, n->title[0] ? n->title : nc_name(n->who), 0, 0, INK, 0);
    if (ww) d_text(d, TK_IT400, 13, right - ww, b1, when, 0, 0, W(.55f), 0);
    for (int k = 0; k < nl; k++) d_text(d, TK_IT400, 15, tx, bl(TK_IT400, 15, t0 + 20 + 19.0f * k), ln[k], 0, 0, W(.85f), 0);
}

/* ---- platters: views of the lock cards' glass ------------------------------------------------------------------
 * A view shares the card's bitmap and half copy (both made at boot, freed only when Emergency frees the cards:
 * free_vendor_cards calls nc_cards_gone). A view is never relayer()ed or made dirty (that would free the card's). */
static void nc_view(tk_layer *v, tk_layer *m)
{
    tk_layer_prepare(m);                    /* its half copy now, shared (a view never makes its own) */
    v->bmp = m->bmp;
    v->half = m->half;
    v->hspan = m->hspan;
    v->glass = (tk_a8){ 0, 0, 0 };
    v->ghalf = (tk_a8){ 0, 0, 0 };
    v->slice = m->slice; v->slice_s0 = m->slice_s0; v->slice_s1 = m->slice_s1;
    v->solid = 0;
    v->gen++;
}
static void nc_unview(tk_layer *v)
{
    v->bmp = (tk_bmp){ 0, 0, 0, 0 };
    v->half = (tk_bmp){ 0, 0, 0, 0 };
    v->hspan = 0;
    v->solid = 0;
    v->slice = 0;
    v->gen++;
}
static int nc_face(void) { for (int g = 0; g < NG; g++) if (K[g].face.bmp.px) return g; return -1; }
static int nc_peeksrc(void) { for (int g = 0; g < NG; g++) if (stack(g) && K[g].gh.bmp.px && K[g].face.bmp.px) return g; return -1; }
/* a platter's glass at (x, y), h tall, opacity a (pt; in its parent's space) */
static void pl_glass(tk_layer *v, float x, float y, float h, float a)
{
    int g = nc_face();
    if (g >= 0) {
        if (v->bmp.px != K[g].face.bmp.px) nc_view(v, &K[g].face);
        float top = LIST_TOP + K[g].y;          /* where the card was baked */
        v->x = tk_floorf(PT(x - CARD_X) + K[g].face.bx + .5f);
        v->y = tk_floorf(PT(y - top) + K[g].face.by + .5f);
        v->slice_h = PT(h + 2 * FACE_MG) - K[g].fcut;
        v->clip = 0;
    } else {
        if (v->bmp.px) nc_unview(v);
        v->solid = tk_premul(TK_RGBA(52, 58, 84, .8f));
        v->x = tk_floorf(PT(x) + .5f);
        v->y = tk_floorf(PT(y) + .5f);
        v->solid_w = PT(NC_W);
        v->solid_h = PT(h);
        v->clip = 1;
        v->clip_r = (tk_rect){ v->x, v->y, PT(NC_W), PT(h) };
        v->clip_radius = PT(26);
    }
    v->alpha = a;
    v->hidden = a <= .004f;
}
/* the stack's lower cards under a platter (the lock card's peek band) */
static void pl_peek(tk_layer *v, float x, float y, float h, float a)
{
    int g = nc_peeksrc();
    if (g < 0 || a <= .004f) { v->hidden = 1; if (g < 0 && v->bmp.px) nc_unview(v); return; }
    if (v->bmp.px != K[g].gh.bmp.px) nc_view(v, &K[g].gh);
    float top = LIST_TOP + K[g].y;
    v->x = tk_floorf(PT(x - CARD_X) + K[g].gh.bx + .5f);
    v->y = tk_floorf(PT(y + h - 68 - top) + K[g].gh.by + .5f);
    v->alpha = a;
    v->hidden = 0;
}

/* ---- the Notification Centre --------------------------------------------------------------------------------- */
#define NC_LT 232.0f                        /* the list's area under the clock (screen pt) */
#define NC_PY (NC_LT + 44)                  /* the first platter (under the header) */
#define NC_BOT 786.0f
#define NC_SLOTS 20
#define NC_GAP 8.0f
typedef struct {
    unsigned id;                            /* 0: free */
    tk_layer gl, tx, pk;                    /* the glass (a view), the words, the stack's peeks (a view) */
    float y, ty, a, ta, pa, tpa, dx, tdx, h, yb;   /* now and the targets (list pt at scroll 0) */
    int nl, ref, dying, built;
} nc_slot;
static nc_slot NSL[NC_SLOTS];
static tk_layer ncl, ncwall, ncstar, ncdim, nccont, ncsb, ncclk, nchead, nclist, ncempty, ncgh, ncclr, ncclrt, ncpill;
static tk_layer bnl, bnund, bngl, bntx;
enum { NG_NONE, NG_PULL, NG_DRAG, NG_LIST, NG_SCROLL, NG_SWIPE, NG_TAP, NG_BANNER, NG_PILL };
static struct {
    int on, built, closing, anim, g, moved, sl, clrall, reveal, momentum, instant;
    float p, scroll, sv, x0, y0, base, ghy, ghty, gha, ghta;
    char exp[16];                           /* the fanned-out group's poster ("" none) */
    float clr_x[2], clr_w[2];               /* the fanned-out group's pills: Show less, Clear (pt, in its header) */
    tk_tracker tr;
} NC = { .reveal = -1 };
static tk_spring sp_nc, sp_ncs;
static struct { int on, closing, drag; unsigned id; float p, h, dy, until; } BN;
static tk_spring sp_bn;
static int nc_on(void) { return NC.on; }

static float nc_content_h;
static int nc_scrollable(void) { return NC_PY - NC_LT + nc_content_h > NC_BOT - NC_LT; }
static float nc_max_scroll(void) { return tk_maxf(0, NC_PY - NC_LT + nc_content_h + 12 - (NC_BOT - NC_LT)); }

static void nc_slot_free(nc_slot *s)
{
    relayer(&s->tx);
    s->tx.hidden = 1;
    nc_unview(&s->gl); s->gl.hidden = 1;
    nc_unview(&s->pk); s->pk.hidden = 1;
    s->id = 0;
    s->built = 0;
}
static nc_slot *nc_slot_of(unsigned id) { for (int i = 0; i < NC_SLOTS; i++) if (NSL[i].id == id) return &NSL[i]; return 0; }
static nc_slot *nc_slot_new(unsigned id, float y)
{
    int i = nc_find(id);
    if (i < 0) return 0;
    for (int k = 0; k < NC_SLOTS; k++) {
        nc_slot *s = &NSL[k];
        if (s->id) continue;
        char ln[4][96];
        tk_layer_init(&s->tx, "nc-item"); s->tx.hidden = 1;
        tk_layer_init(&s->gl, "nc-glass"); s->gl.hidden = 1;
        tk_layer_init(&s->pk, "nc-peek"); s->pk.hidden = 1;
        s->id = id;
        s->nl = nc_lines(&NI[i], ln);
        s->h = nc_hh(s->nl);
        s->y = s->ty = y;
        s->a = s->ta = 0;
        s->pa = s->tpa = 0;
        s->dx = s->tdx = 0;
        s->dying = s->built = 0;
        return s;
    }
    return 0;
}
/* the words of a platter (when it comes into view) */
static void nc_slot_build(nc_slot *s)
{
    int i = nc_find(s->id);
    if (i < 0) return;
    char ln[4][96];
    int nl = nc_lines(&NI[i], ln);
    relayer(&s->tx);
    dc d = mk(&s->tx, "nc-item", CARD_X, s->ty, NC_W, s->h, 2, 0);
    nc_draw_item(&d, &NI[i], CARD_X, s->ty, s->h, nl, ln);
    trim(&s->tx);
    tk_layer_prepare(&s->tx);
    s->yb = s->ty;
    s->built = 1;
}

/* the groups (by poster), newest first */
typedef struct { const char *who; int first, n; } nc_grp;
static int nc_groups(nc_grp *g)
{
    int ng = 0;
    for (int i = 0; i < nin; i++) {
        int k = 0;
        while (k < ng && tk_strcmp(g[k].who, NI[i].who)) k++;
        if (k == ng) { g[ng].who = NI[i].who; g[ng].first = i; g[ng].n = 0; ng++; }
        g[k].n++;
    }
    return ng;
}
static void nc_head_build(void);
static void nc_ghead_build(void);
/* every platter's place: a group fanned out under its header, the others as one platter (its newest) with peeks;
 * what is no longer listed fades (or flies left when cleared) and is freed when it has gone */
static void nc_layout(void)
{
    nc_grp gr[NC_MAX];
    int ng = nc_groups(gr), exp_ok = 0;
    for (int k = 0; k < NC_SLOTS; k++) NSL[k].ref = 0;
    float y = NC_PY;
    for (int g = 0; g < ng; g++) {
        int ex = NC.exp[0] && !tk_strcmp(NC.exp, gr[g].who) && gr[g].n > 1;
        if (ex) { exp_ok = 1; NC.ghty = y; NC.ghta = 1; if (NC.gha <= 0) NC.ghy = y; y += 40; }
        float top = y;
        nc_slot *head = 0;
        for (int k = 0, i = gr[g].first; i < nin && (k < gr[g].n); i++) {
            if (tk_strcmp(NI[i].who, gr[g].who)) continue;
            nc_slot *s = nc_slot_of(NI[i].id);
            if (!s && (ex || k == 0)) s = nc_slot_new(NI[i].id, k ? (head ? head->y : top) : y);
            if (s && !s->dying) {
                s->ref = 1;
                if (ex || k == 0) {
                    s->ty = y; s->ta = 1;
                    s->tpa = !ex && gr[g].n > 1 ? 1 : 0;
                    y += s->h + (ex ? NC_GAP : 0);
                    if (!k) head = s;
                } else {
                    s->ty = head ? head->ty : top; s->ta = 0; s->tpa = 0;    /* folding back under the newest */
                }
            }
            k++;
        }
        if (!ex) y += (gr[g].n > 1 ? PEEK2 : 0) + NC_GAP;      /* the peeks, then the gap */
    }
    if (!exp_ok) { NC.exp[0] = 0; NC.ghta = 0; }
    for (int k = 0; k < NC_SLOTS; k++) {
        nc_slot *s = &NSL[k];
        if (s->id && !s->ref && !s->dying) { s->ta = 0; s->tpa = 0; }
    }
    nc_content_h = y - NC_PY;
    float m = nc_max_scroll();
    if (NC.scroll > m && !sp_ncs.live) tk_spring_start(&sp_ncs, NC.scroll, m, 0, TK_SPR_ROWS);
    NC.anim = 1;
    relink();                               /* new platters (and freed ones) in the tree */
}

/* the layers' places now */
static void nc_place_list(void)
{
    nclist.y = tk_floorf(PT(-NC.scroll) + .5f);
    nclist.clip = 1;
    nclist.clip_r = (tk_rect){ 0, PT(NC_LT), PT(390), PT(NC_BOT - NC_LT) };
    nclist.fade_bot = PT(30);
    nclist.fade_top = PT(10);
    for (int k = 0; k < NC_SLOTS; k++) {
        nc_slot *s = &NSL[k];
        if (!s->id) continue;
        pl_peek(&s->pk, CARD_X + s->dx, s->y, s->h, s->pa * s->a);
        pl_glass(&s->gl, CARD_X + s->dx, s->y, s->h, s->built ? s->a : 0);
        if (s->built && s->tx.bmp.px) {
            s->tx.x = s->tx.bx + tk_floorf(PT(s->dx) + .5f);
            s->tx.y = s->tx.by + tk_floorf(PT(s->y - s->yb) + .5f);
            s->tx.alpha = s->a;
            s->tx.hidden = s->a <= .004f;
        } else
            s->tx.hidden = 1;
    }
    if (ncgh.bmp.px) {
        ncgh.y = ncgh.by + tk_floorf(PT(NC.ghy - NC_PY) + .5f);
        ncgh.alpha = NC.gha;
        ncgh.hidden = NC.gha <= .004f;
    }
    /* the Clear button under the platter a short swipe moved */
    nc_slot *r = NC.reveal >= 0 ? &NSL[NC.reveal] : 0;
    if (r && r->id && r->dx < -2) {
        float w = 76, x = CARD_X + NC_W - w, a = tk_clampf(-r->dx / 92, 0, 1);
        ncclr.solid = tk_premul(W(.2f));
        ncclr.x = tk_floorf(PT(x) + .5f); ncclr.y = tk_floorf(PT(r->y) + .5f);
        ncclr.solid_w = PT(w); ncclr.solid_h = PT(r->h);
        ncclr.clip = 1; ncclr.clip_r = (tk_rect){ ncclr.x, ncclr.y, PT(w), PT(r->h) }; ncclr.clip_radius = PT(22);
        ncclr.alpha = a; ncclr.hidden = 0;
        if (ncclrt.bmp.px) { ncclrt.y = ncclrt.by + tk_floorf(PT(r->y + r->h / 2 - 10) + .5f); ncclrt.alpha = a; ncclrt.hidden = 0; }
    } else { ncclr.hidden = 1; ncclrt.hidden = 1; }
    nchead.hidden = !nin || !nchead.bmp.px;
    ncempty.hidden = nin > 0;
}
static void nc_place(void)
{
    float p = NC.p;
    ncl.hidden = !NC.on;
    ncl.y = tk_floorf(PT(-(1 - tk_minf(p, 1)) * H_PT) + .5f);
    nccont.y = p > 1 ? tk_floorf(PT((p - 1) * H_PT) + .5f) : 0;     /* past full: the words come down (rubber) */
    if (NC.on) nc_place_list();
}

static void nc_pill_build(void);
/* the lock's clock as the Centre's (a view: rebuilt with it, every minute) */
static void nc_clock_view(void)
{
    if (!NC.built) return;
    tk_layer_prepare(&lclock);
    nc_view(&ncclk, &lclock);
    ncclk.x = lclock.x; ncclk.y = lclock.y;
}
static void nc_clock_stale(void) { nc_clock_view(); }
static void nc_sb_build(void)
{
    relayer(&ncsb);
    dc d = mk(&ncsb, "nc-status", 0, 0, 390, 48, 2, 0);
    status_bar(&d, carrier);
    trim(&ncsb);
}
static void nc_sb_stale(void) { if (NC.built) { nc_sb_build(); relink(); } }
/* "Notification Centre" and its clear-all (x; a tap turns it into "Clear") */
static void nc_head_build(void)
{
    relayer(&nchead);
    dc d = mk(&nchead, "nc-head", MARGIN, NC_LT + 4, CONTENT_W, 32, 2, 0);
    d_text(&d, TK_IT700, 20, MARGIN + 4, NC_LT + 4 + 23, "Notification Centre", 0, 0, INK, 1);
    if (NC.clrall) {
        float w = tw(TK_IT600, 13, "Clear", 0, 0) + 24;
        d_rrect(&d, CONTENT_R - w, NC_LT + 6, w, 28, 14, W(.22f));
        d_text(&d, TK_IT600, 13, CONTENT_R - w + 12, bl(TK_IT600, 13, NC_LT + 6 + 6), "Clear", 0, 0, INK, 0);
    } else {
        d_rrect(&d, CONTENT_R - 28, NC_LT + 6, 28, 28, 14, W(.22f));
        d_seg(&d, CONTENT_R - 18.5f, NC_LT + 15.5f, CONTENT_R - 9.5f, NC_LT + 24.5f, 2, W(.9f));
        d_seg(&d, CONTENT_R - 9.5f, NC_LT + 15.5f, CONTENT_R - 18.5f, NC_LT + 24.5f, 2, W(.9f));
    }
    trim(&nchead);
    if (NC.built) relink();                 /* made again: back in its place */
}
/* the fanned-out group's header: its name, "Show less" and "Clear" */
static void nc_ghead_build(void)
{
    relayer(&ncgh);
    if (!NC.exp[0]) return;
    dc d = mk(&ncgh, "nc-ghead", MARGIN, NC_PY, CONTENT_W, 32, 2, 0);
    d_text_box(&d, TK_IT700, 20, MARGIN + 4, NC_PY + 23, 160, 0, nc_name(NC.exp), 0, 0, INK, 1);
    const char *L[2] = { "Show less", "Clear" };
    float x = CONTENT_R;
    for (int i = 1; i >= 0; i--) {
        float w = tw(TK_IT600, 13, L[i], 0, 0) + 24;
        x -= w;
        d_rrect(&d, x, NC_PY + 2, w, 28, 14, W(.22f));
        d_text(&d, TK_IT600, 13, x + 12, bl(TK_IT600, 13, NC_PY + 2 + 6), L[i], 0, 0, INK, 0);
        NC.clr_x[i] = x; NC.clr_w[i] = w;
        x -= 8;
    }
    trim(&ncgh);
    tk_layer_prepare(&ncgh);
    relink();
}
/* the Centre's layers (no glass: the wallpaper, the star and the clock are views; the words are small) */
static void nc_build(void)
{
    if (NC.built) return;
    tk_layer_init(&ncl, "nc");
    tk_layer_init(&ncwall, "nc-wall");
    ncwall.bmp = C->base_half;              /* a view of the wallpaper (half resolution, drawn 2x): never freed here */
    ncwall.sx = ncwall.sy = 2;
    ncwall.opaque = 1;
    tk_layer_init(&ncstar, "nc-star");
    tk_layer *st = EMG && star_e.bmp.px ? &star_e : &star;
    nc_view(&ncstar, st);
    ncstar.x = st->x; ncstar.y = st->y;
    tk_layer_init(&ncdim, "nc-dim");
    ncdim.solid = tk_premul(TK_RGBA(0, 0, 0, .18f));
    ncdim.solid_w = TK_W; ncdim.solid_h = TK_H;
    tk_layer_init(&nccont, "nc-content");
    tk_layer_init(&ncclk, "nc-clock");
    tk_layer_init(&nclist, "nc-list");
    tk_layer_init(&ncclr, "nc-clear"); ncclr.hidden = 1;
    NC.built = 1;
    nc_clock_view();
    nc_sb_build();
    NC.clrall = 0;
    nc_head_build();
    {
        dc d = mk(&ncempty, "nc-empty", 0, 492, 390, 24, 2, 0);
        d_text_box(&d, TK_IT400, 15, 195, 492 + 17, CONTENT_W, 1, "No Older Notifications", 0, 0, W(.55f), 1);
        trim(&ncempty);
    }
    {
        float w = tw(TK_IT600, 15, "Clear", 0, 0);
        dc d = mk(&ncclrt, "nc-clear-word", CARD_X + NC_W - 38 - w / 2, 0, w, 20, 2, 0);
        d_text(&d, TK_IT600, 15, CARD_X + NC_W - 38 - w / 2, 15, "Clear", 0, 0, INK, 0);
        trim(&ncclrt);
        ncclrt.hidden = 1;
    }
    NC.scroll = 0; sp_ncs.live = 0; NC.momentum = 0;
    NC.exp[0] = 0; NC.gha = NC.ghta = 0;
    NC.reveal = -1;
    nc_layout();
    for (int k = 0; k < NC_SLOTS; k++) if (NSL[k].id) { NSL[k].y = NSL[k].ty; NSL[k].a = NSL[k].ta; NSL[k].pa = NSL[k].tpa; }
    relink();
}
static void nc_free(void)
{
    if (!NC.built) return;
    for (int k = 0; k < NC_SLOTS; k++) if (NSL[k].id) nc_slot_free(&NSL[k]);
    nc_unview(&ncstar);
    nc_unview(&ncclk);
    ncwall.bmp = (tk_bmp){ 0, 0, 0, 0 };    /* the wallpaper is the compositor's */
    relayer(&ncsb); relayer(&nchead); relayer(&ncempty); relayer(&ncgh); relayer(&ncclrt);
    NC.built = 0;
    NC.on = 0;
    ncl.hidden = 1;
    relink();
}
/* the platters still to draw (two a frame; all at once for a still) */
static int nc_build_some(int max)
{
    int n = 0;
    for (int k = 0; k < NC_SLOTS && n < max; k++) {
        nc_slot *s = &NSL[k];
        if (!s->id || s->built || s->dying) continue;
        float y = s->ty - NC.scroll;
        if (y + s->h < NC_LT - 60 || y > NC_BOT + 60) continue;
        nc_slot_build(s);
        n++;
    }
    if (n) relink();                        /* a layer made again by mk() has lost its place in the tree */
    return n;
}

static void nc_mark_read(void)
{
    if (!nc_unread) return;
    nc_unread = 0;
    nc_pill_build();
}
static void nc_show(int over_lock, float v)
{
    nc_build();
    NC.on = 1;
    NC.closing = 0;
    NC.instant = 0;
    tk_spring_start(&sp_nc, NC.p, 1, v, TK_SPR_UNLOCK);
    (void)over_lock;
    nc_place();
}
static void nc_close(float v)
{
    NC.closing = 1;
    NC.reveal = -1;
    tk_spring_start(&sp_nc, NC.p, 0, v, TK_SPR_UNLOCK);
}
static void nc_gone(void)
{
    NC.on = NC.closing = 0;
    NC.p = 0;
    sp_nc.live = 0;
    nc_free();
}

/* ---- the store's changes -------------------------------------------------------------------------------------- */
static void bn_show(const nc_item *n);
static void bn_hide(float v);
static void nc_remove(int i)
{
    if (i < 0 || i >= nin) return;
    unsigned id = NI[i].id;
    for (int k = i; k + 1 < nin; k++) NI[k] = NI[k + 1];
    nin--;
    nc_slot *s = nc_slot_of(id);
    if (s && !s->dying) s->ta = 0;
    if (BN.on && BN.id == id && !BN.closing) bn_hide(0);
    if (NC.built) { nc_head_build(); nc_layout(); relink(); }
}
static void nc_clear_group(const char *who, int fly)
{
    char w[16];
    nc_cpy(w, sizeof w, who);
    for (int i = nin - 1; i >= 0; i--) {
        if (tk_strcmp(NI[i].who, w)) continue;
        nc_slot *s = nc_slot_of(NI[i].id);
        if (s && fly) { s->dying = 1; s->tdx = -440; }
        nc_remove(i);
    }
    if (!tk_strcmp(NC.exp, w)) { NC.exp[0] = 0; nc_ghead_build(); }
    if (NC.built) nc_layout();
}
void sh_notify(const char *who, const char *title, const char *body, int hm)
{
    if (nin == NC_MAX) {
        nc_slot *s = nc_slot_of(NI[NC_MAX - 1].id);
        if (s) s->ta = 0;
        nin--;
    }
    for (int k = nin; k > 0; k--) NI[k] = NI[k - 1];
    nc_item *n = &NI[0];
    nc_cpy(n->who, sizeof n->who, who && who[0] ? who : "system");
    nc_cpy(n->title, sizeof n->title, title);
    nc_cpy(n->body, sizeof n->body, body);
    n->hm = hm;
    n->id = ++nc_ids ? nc_ids : ++nc_ids;
    nin++;
    if (NC.on && !NC.closing) {             /* the Centre is open: it is read now */
        nc_head_build();
        nc_layout();
        relink();
        return;
    }
    nc_unread++;
    nc_pill_build();
    if (NC.built) { nc_head_build(); nc_layout(); }
    /* a banner on home and in apps (not on the lock, in the dark, over a sheet, or in the switcher) */
    if (!locked && !SL.black && !SL.dir && !focus_on && !CF.on && !sw_on()) bn_show(n);
}

/* ---- the lock's pill ------------------------------------------------------------------------------------------- */
#define PILL_Y 707.0f
static float pill_w;
static void nc_pill_build(void)
{
    relayer(&ncpill);
    tk_layer_init(&ncpill, "nc-pill");
    ncpill.hidden = 1;
    if (nc_unread > 0) {
        char b[40];
        int n = put_dec(b, nc_unread);
        tk_memcpy(b + n, nc_unread == 1 ? " Notification" : " Notifications", 15);
        pill_w = tw(TK_IT600, 13, b, 0, 0) + 28;
        float x = 195 - pill_w / 2;
        dc d = mk(&ncpill, "nc-pill", x, PILL_Y, pill_w, 28, 2, 0);
        d_rrect(&d, x, PILL_Y, pill_w, 28, 14, W(.18f));
        d_text(&d, TK_IT600, 13, x + 14, bl(TK_IT600, 13, PILL_Y + 6), b, 0, 0, INK, 0);
        trim(&ncpill);
        ncpill.hidden = 0;
    }
    relink();
    lock_ver++;
}
static int nc_pill_hit(float x, float y)
{
    return !ncpill.hidden && nc_unread > 0 && y >= PILL_Y - 8 && y < PILL_Y + 36 && tk_fabsf(x - 195) < pill_w / 2 + 8;
}

/* ---- the banner ------------------------------------------------------------------------------------------------- */
#define BN_Y 8.0f
static void bn_place(void)
{
    bnl.hidden = !BN.on;
    float off = (1 - BN.p) * -(BN.h + BN_Y + 30) + BN.dy;
    bnl.y = tk_floorf(PT(off) + .5f);
}
static void bn_show(const nc_item *n)
{
    char ln[4][96];
    int nl = nc_lines(n, ln);
    float h = nc_hh(nl);
    relayer(&bntx);
    tk_layer_init(&bnl, "banner");
    dc d = mk(&bntx, "banner-words", CARD_X, BN_Y, NC_W, h, 2, 0);
    nc_draw_item(&d, n, CARD_X, BN_Y, h, nl, ln);
    trim(&bntx);
    tk_layer_prepare(&bntx);                /* it moves at once: its half copy now, not in the first frame */
    tk_layer_init(&bnund, "banner-under");  /* a dark ground under the glass: legible over any page */
    bnund.solid = tk_premul(TK_RGBA(16, 20, 36, .93f));
    bnund.x = PT(CARD_X); bnund.y = PT(BN_Y);
    bnund.solid_w = PT(NC_W); bnund.solid_h = PT(h);
    bnund.clip = 1; bnund.clip_r = (tk_rect){ bnund.x, bnund.y, PT(NC_W), PT(h) }; bnund.clip_radius = PT(26);
    if (!bngl.name) tk_layer_init(&bngl, "banner-glass");
    pl_glass(&bngl, CARD_X, BN_Y, h, 1);
    if (!BN.on) BN.p = 0;
    BN.on = 1; BN.closing = 0; BN.drag = 0; BN.dy = 0;
    BN.h = h; BN.id = n->id;
    BN.until = -1;                          /* set at its first frame (the platform's clock) */
    tk_spring_start(&sp_bn, BN.p, 1, 0, TK_SPR_SHEET);
    relink();
    bn_place();
}
static void bn_free(void)
{
    BN.on = BN.closing = BN.drag = 0;
    BN.p = 0;
    sp_bn.live = 0;
    relayer(&bntx);
    nc_unview(&bngl);
    bnl.hidden = 1;
    relink();
}
static void bn_hide(float v)
{
    if (!BN.on) return;
    BN.closing = 1;
    tk_spring_start(&sp_bn, BN.p, 0, v, TK_SPR_SHEET);
}
/* Emergency freed the lock cards: the platters become solid ones (no view may point at freed glass) */
static void nc_cards_gone(void)
{
    for (int k = 0; k < NC_SLOTS; k++) { nc_unview(&NSL[k].gl); nc_unview(&NSL[k].pk); NSL[k].pk.hidden = 1; }
    nc_unview(&bngl);
    if (BN.on) pl_glass(&bngl, CARD_X, BN_Y, BN.h, 1);
    if (NC.on) nc_place_list();
}

/* ---- opening what a notification is about ------------------------------------------------------------------------ */
static int nc_launch(unsigned id)
{
    int i = nc_find(id);
    if (i < 0) return 0;
    int a = nc_app(NI[i].who);
    nc_remove(i);
    if (a < 0) return 0;                    /* nothing to open (an unknown poster): read and cleared */
    if (locked) {
        /* from the lock's Centre: the lock goes away behind it (no passcode here), the app opens over home */
        sp_lock.live = 0;
        set_lock(-H_PT);
        locked = 0;
        lockflat.hidden = 1;
        home.sx = home.sy = 1;
    }
    if (AP.on && AP.a == a && !AP.closing) return 1;
    if (AP.on) { sp_app.live = 0; app_finish_close(); }
    app_open(a);
    return 1;
}

/* ---- touches ------------------------------------------------------------------------------------------------------- */
static int nc_slot_at(float x, float y)
{
    if (y < NC_LT || y >= NC_BOT) return -1;
    float cy = y + NC.scroll;
    for (int k = 0; k < NC_SLOTS; k++) {
        nc_slot *s = &NSL[k];
        if (!s->id || s->dying || s->ta < .5f) continue;
        if (cy >= s->ty && cy < s->ty + s->h && x >= CARD_X && x < CARD_X + NC_W) return k;
    }
    return -1;
}
static int bn_hit(float x, float y)
{
    if (!BN.on || BN.closing || BN.p < .3f) return 0;
    float top = BN_Y + (1 - BN.p) * -(BN.h + BN_Y + 30) + BN.dy;
    return x >= CARD_X - 4 && x < CARD_X + NC_W + 4 && y >= top - 6 && y < top + BN.h + 6;
}
/* a touch that starts on the banner, on the open Centre or on the lock's pill: 1 if the Centre's (sh_touch: G_NC) */
static int nc_down(float x, float y, float t)
{
    tk_tracker_reset(&NC.tr);
    NC.x0 = x; NC.y0 = y; NC.moved = 0;
    if (bn_hit(x, y)) { NC.g = NG_BANNER; BN.drag = 1; sp_bn.live = 0; tk_tracker_add(&NC.tr, y, t); return 1; }
    if (NC.on && !NC.closing) {
        sp_nc.live = 0;
        NC.momentum = 0;
        tk_tracker_add(&NC.tr, y, t);
        if (y >= H_PT - 34) { NC.g = NG_DRAG; return 1; }
        NC.sl = nc_slot_at(x, y);
        NC.g = y >= NC_LT && y < NC_BOT ? NG_LIST : NG_TAP;
        NC.base = NC.scroll;
        sp_ncs.live = 0;
        return 1;
    }
    if (NC.on) return 1;                    /* closing: nothing else takes it */
    if (locked && nc_pill_hit(x, y)) { NC.g = NG_PILL; tk_tracker_add(&NC.tr, y, t); return 1; }
    return 0;
}
/* a pull from the top-left half of the status bar (home or an app) */
static void nc_pull_begin(float x, float y, float t)
{
    nc_build();
    NC.on = 1; NC.closing = 0; NC.p = 0;
    sp_nc.live = 0;
    tk_tracker_reset(&NC.tr);
    tk_tracker_add(&NC.tr, y, t);
    NC.x0 = x; NC.y0 = y; NC.moved = 0;
    NC.g = NG_PULL;
    if (BN.on) bn_hide(0);
    nc_place();
}
/* the Centre's touches after the first: 2 hands the touch back to the lock's swipe (from the pill) */
static int nc_touch(int ph, float x, float y, float t)
{
    float dx = x - NC.x0, dy = y - NC.y0;
    if (ph == 1) {
        if (tk_fabsf(dx) > 8 || tk_fabsf(dy) > 8) NC.moved = 1;
        tk_tracker_add(&NC.tr, NC.g == NG_SWIPE ? x : y, t);
        switch (NC.g) {
        case NG_PILL:
            if (NC.moved) return 2;
            break;
        case NG_PULL:
            NC.p = dy <= H_PT ? tk_maxf(0, dy) / H_PT : 1 + rubber(dy - H_PT, H_PT) / H_PT;
            nc_place();
            break;
        case NG_TAP: case NG_DRAG:
            if (NC.g == NG_TAP && NC.moved && tk_fabsf(dy) > tk_fabsf(dx)) NC.g = NG_DRAG;
            if (NC.g != NG_DRAG) break;
            NC.p = dy < 0 ? tk_maxf(0, 1 + dy / H_PT) : 1 + rubber(dy, H_PT) / H_PT;
            nc_place();
            break;
        case NG_LIST:
            if (!NC.moved) break;
            if (NC.sl >= 0 && tk_fabsf(dx) > tk_fabsf(dy)) {
                NC.g = NG_SWIPE;
                if (NC.reveal >= 0 && NC.reveal != NC.sl) NSL[NC.reveal].tdx = 0;
                NC.reveal = NC.sl;
                NC.base = NSL[NC.sl].dx;
                tk_tracker_reset(&NC.tr);
                tk_tracker_add(&NC.tr, x, t);
            } else if (nc_scrollable()) NC.g = NG_SCROLL;
            else { NC.g = NG_DRAG; NC.p = 1; }
            if (NC.reveal >= 0 && NC.g != NG_SWIPE) { NSL[NC.reveal].tdx = 0; NC.reveal = -1; }
            break;
        case NG_SCROLL: {
            float s = NC.base - dy, m = nc_max_scroll();
            if (s < 0) s = -rubber(-s, NC_BOT - NC_LT);
            else if (s > m) s = m + rubber(s - m, NC_BOT - NC_LT);
            NC.scroll = s;
            nc_place_list();
            break;
        }
        case NG_SWIPE: {
            nc_slot *s = &NSL[NC.sl];
            float d = NC.base + dx;
            s->dx = s->tdx = d < 0 ? d : rubber(d, 120) * .5f;
            nc_place_list();
            break;
        }
        case NG_BANNER:
            if (dy > 40) {
                /* pulled down: the Centre comes with the finger (the banner goes into it) */
                BN.drag = 0;
                bn_hide(0);
                nc_build();
                NC.on = 1; NC.closing = 0; sp_nc.live = 0;
                NC.g = NG_PULL;
                NC.p = dy / H_PT;
                nc_place();
                break;
            }
            BN.dy = dy < 0 ? dy : rubber(dy, 300) * .6f;
            bn_place();
            break;
        }
        return 0;
    }
    if (ph != 2) return 0;
    float v = tk_tracker_v(&NC.tr);
    int g = NC.g;
    NC.g = NG_NONE;
    switch (g) {
    case NG_PILL:
        nc_build();
        NC.p = 0;
        nc_show(1, 0);
        break;
    case NG_PULL:
        /* where the pull would carry it (200 ms) decides; the finger's speed goes to the spring */
        if (NC.p * H_PT + v * 200 > H_PT * .5f) nc_show(0, v * 1000 / H_PT);
        else nc_close(v * 1000 / H_PT);
        break;
    case NG_DRAG: {
        float vp = v * 1000 / H_PT;
        if (!NC.moved && y >= H_PT - 34) nc_close(0);           /* the home bar: a tap closes it too */
        else if (NC.p + vp * .2f < .7f) nc_close(vp);
        else tk_spring_start(&sp_nc, NC.p, 1, vp, TK_SPR_UNLOCK);
        break;
    }
    case NG_SCROLL: {
        float m = nc_max_scroll();
        if (NC.scroll < 0 || NC.scroll > m) tk_spring_start(&sp_ncs, NC.scroll, NC.scroll < 0 ? 0 : m, 0, TK_SPR_ROWS);
        else { NC.sv = -v; NC.momentum = tk_fabsf(NC.sv) > .02f; }
        break;
    }
    case NG_SWIPE: {
        nc_slot *s = &NSL[NC.sl];
        if (!s->id) break;
        if (s->dx < -NC_W * .5f || (v < -.6f && s->dx < -30)) {
            /* cleared: it flies off; a stack goes as a whole */
            int i = nc_find(s->id);
            int whole = i >= 0 && tk_strcmp(NC.exp, NI[i].who) && s->tpa > .5f;
            s->dying = 1; s->tdx = -440;
            NC.reveal = -1;
            if (whole) nc_clear_group(NI[i].who, 1);
            else nc_remove(i);
        } else if (s->dx < -44) s->tdx = -92;                    /* the Clear button stays */
        else { s->tdx = 0; NC.reveal = -1; }
        NC.anim = 1;
        break;
    }
    case NG_BANNER:
        BN.drag = 0;
        if (!NC.moved) { unsigned id = BN.id; bn_hide(0); (void)nc_launch(id); }
        else if (BN.dy < -24 || v < -.3f) bn_hide(v * 1000 / (BN.h + 40));
        else BN.until = now_ms + 2500;       /* put back (nc_tick eases it up): a little longer on screen */
        break;
    case NG_LIST: case NG_TAP: {
        if (NC.moved) break;
        float cy = y + NC.scroll;
        /* a short swipe's Clear button, else the swipe put back */
        if (NC.reveal >= 0) {
            nc_slot *r = &NSL[NC.reveal];
            if (r->id && cy >= r->ty && cy < r->ty + r->h && x >= CARD_X + NC_W - 80) {
                int i = nc_find(r->id);
                int whole = i >= 0 && tk_strcmp(NC.exp, NI[i].who) && r->tpa > .5f;
                r->dying = 1; r->tdx = -440;
                if (whole) nc_clear_group(NI[i].who, 1); else nc_remove(i);
            } else r->tdx = 0;
            NC.reveal = -1;
            NC.anim = 1;
            break;
        }
        /* the header's clear-all (x, then Clear) */
        if (nin && cy >= NC_LT && cy < NC_LT + 40 && x >= CONTENT_R - (NC.clrall ? 70 : 40)) {
            if (!NC.clrall) { NC.clrall = 1; nc_head_build(); relink(); }
            else {
                for (int k = 0; k < NC_SLOTS; k++) if (NSL[k].id) NSL[k].ta = 0;
                nin = 0;
                NC.clrall = 0;
                NC.exp[0] = 0;
                nc_ghead_build();
                nc_head_build();
                nc_layout();
                relink();
            }
            break;
        }
        if (NC.clrall) { NC.clrall = 0; nc_head_build(); relink(); }
        /* the fanned-out group's Show less / Clear */
        if (NC.exp[0] && NC.gha > .5f && cy >= NC.ghty && cy < NC.ghty + 34) {
            for (int i = 0; i < 2; i++)
                if (x >= NC.clr_x[i] - 4 && x < NC.clr_x[i] + NC.clr_w[i] + 4) {
                    if (i) nc_clear_group(NC.exp, 1);
                    else { NC.exp[0] = 0; nc_layout(); }
                }
            break;
        }
        int k = nc_slot_at(x, y);
        if (k < 0) break;
        int i = nc_find(NSL[k].id);
        if (i < 0) break;
        if (NSL[k].tpa > .5f && tk_strcmp(NC.exp, NI[i].who)) {
            /* a stack: it fans out (one group at a time) */
            nc_cpy(NC.exp, sizeof NC.exp, NI[i].who);
            nc_ghead_build();
            NC.gha = 0;
            nc_layout();
            relink();
            break;
        }
        if (nc_launch(NSL[k].id)) nc_close(0);
        break;
    }
    }
    return 0;
}

/* ---- time ---------------------------------------------------------------------------------------------------------- */
static int nc_ease(float *v, float to, float k, float eps)
{
    float d = to - *v;
    if (tk_fabsf(d) < eps) { *v = to; return 0; }
    *v += d * k;
    return 1;
}
static int nc_tick(float dt, float t)
{
    int mv = 0;
    if (sp_nc.live) {
        int live = tk_spring_step(&sp_nc, dt);
        NC.p = tk_maxf(0, sp_nc.x);
        if (NC.p > .6f) nc_mark_read();
        if (!live && NC.closing) nc_gone();
        else nc_place();
        mv = 1;
    }
    if (NC.g == NG_PULL || NC.g == NG_DRAG || NC.g == NG_SCROLL || NC.g == NG_SWIPE || NC.g == NG_BANNER) mv = 1;
    if (NC.on && NC.built) {
        if (NC.p >= 1 && !NC.closing) nc_mark_read();
        if (sp_ncs.live) { tk_spring_step(&sp_ncs, dt); NC.scroll = sp_ncs.x; mv = 1; }
        else if (NC.momentum) {
            float ms = dt * 1000, m = nc_max_scroll(), decay = tk_powf(.998f, ms);
            NC.scroll += NC.sv * (1 - decay) / (1 - .998f);
            NC.sv *= decay;
            if (NC.scroll < 0 || NC.scroll > m) { NC.momentum = 0; tk_spring_start(&sp_ncs, NC.scroll, NC.scroll < 0 ? 0 : m, NC.sv * 1000, TK_SPR_ROWS); }
            else if (tk_fabsf(NC.sv) < .01f) NC.momentum = 0;
            mv = 1;
        }
        /* the platters ease to their places (critically damped look, ~.25 s) */
        float k = 1 - tk_expf(-dt * 15);
        int any = 0;
        for (int i = 0; i < NC_SLOTS; i++) {
            nc_slot *s = &NSL[i];
            if (!s->id) continue;
            any |= nc_ease(&s->y, s->ty, k, .2f);
            any |= nc_ease(&s->a, s->ta, k * 1.2f, .01f);
            any |= nc_ease(&s->pa, s->tpa, k * 1.2f, .01f);
            if (!(NC.g == NG_SWIPE && NC.sl == i)) any |= nc_ease(&s->dx, s->tdx, s->dying ? 1 - tk_expf(-dt * 9) : k, .2f);
            if ((s->dying && s->dx < -400) || (!s->dying && s->ta <= 0 && s->a <= .01f)) nc_slot_free(s);
        }
        any |= nc_ease(&NC.ghy, NC.ghty, k, .2f);
        any |= nc_ease(&NC.gha, NC.ghta, k * 1.2f, .01f);
        if (NC.gha <= 0 && NC.ghta <= 0 && ncgh.bmp.px && !NC.exp[0]) relayer(&ncgh);
        if (nc_build_some(NC.instant ? NC_SLOTS : 2)) any = 1;
        NC.anim = any;
        mv |= any;
        nc_place();
    }
    if (BN.on) {
        if (BN.until < 0) BN.until = t + 4000;
        if (!BN.drag && BN.dy != 0) { mv |= nc_ease(&BN.dy, 0, 1 - tk_expf(-dt * 15), .2f); bn_place(); }
        if (sp_bn.live) {
            int live = tk_spring_step(&sp_bn, dt);
            BN.p = tk_maxf(0, sp_bn.x);
            if (!live && BN.closing) bn_free();
            else bn_place();
            mv = 1;
        } else if (!BN.drag && !BN.closing && t >= BN.until) { bn_hide(0); mv = 1; }
    }
    return mv;
}
static int nc_moving(void)
{
    return sp_nc.live || sp_ncs.live || NC.momentum || (NC.on && NC.anim) || sp_bn.live || (BN.on && !BN.drag && BN.dy != 0) ||
           NC.g == NG_PULL || NC.g == NG_DRAG || NC.g == NG_SCROLL || NC.g == NG_SWIPE || NC.g == NG_BANNER;
}
/* the banner's rest: when to come back (ms, < 0 nothing) and the timed slide away */
static float nc_next_update(float t)
{
    if (!BN.on || BN.closing || BN.drag || sp_bn.live || BN.until < 0) return -1;
    return tk_maxf(1, BN.until - t);
}
static int nc_refresh(float t)
{
    if (BN.on && !BN.closing && !BN.drag && !sp_bn.live && BN.until >= 0 && t >= BN.until) { bn_hide(0); return 1; }
    return 0;
}
/* the minute: the times on the platters ("5m ago") */
static void nc_minute(void)
{
    if (!NC.built) return;
    for (int k = 0; k < NC_SLOTS; k++) if (NSL[k].id && NSL[k].built && !NSL[k].dying) nc_slot_build(&NSL[k]);
    relink();
    nc_place();
}
/* back to the plain lock (sleep, reset): nothing of it is kept */
static void nc_reset(void)
{
    NC.g = NG_NONE;
    if (NC.built || NC.on) nc_gone();
    if (BN.on) bn_free();
}
static void nc_link(void)
{
    if (!ncl.name) { tk_layer_init(&ncl, "nc"); ncl.hidden = 1; }
    if (!bnl.name) { tk_layer_init(&bnl, "banner"); bnl.hidden = 1; }
    if (!ncpill.name) { tk_layer_init(&ncpill, "nc-pill"); ncpill.hidden = 1; }
    if (NC.built) {
        tk_layer *c[] = { &ncwall, &ncstar, &ncdim, &nccont };
        link(&ncl, c, 4);
        tk_layer *cc[] = { &ncsb, &ncclk, &nclist, &ncempty };
        link(&nccont, cc, 4);
        tk_layer *ls[3 * NC_SLOTS + 4];
        int n = 0;
        ls[n++] = &ncclr; ls[n++] = &ncclrt; ls[n++] = &nchead;
        for (int k = 0; k < NC_SLOTS; k++) if (NSL[k].id) { ls[n++] = &NSL[k].pk; ls[n++] = &NSL[k].gl; ls[n++] = &NSL[k].tx; }
        ls[n++] = &ncgh;
        link(&nclist, ls, n);
    } else
        ncl.child = 0;
    if (BN.on) { tk_layer *b[] = { &bnund, &bngl, &bntx }; link(&bnl, b, 3); }
    else bnl.child = 0;
}

/* the host's stills and films: "notify" WHO|TITLE|BODY (v: hhmm, else now), "nc" p, "ncgroup" WHO (fanned out) */
static int nc_set(const char *what, const char *key, float v)
{
    if (!tk_strcmp(what, "notify")) {
        char f[3][80] = { "", "", "" };
        int k = 0, n = 0;
        for (const char *s = key; *s && k < 3; s++) {
            if (*s == '|') { f[k][n] = 0; k++; n = 0; continue; }
            if (n < 79) f[k][n++] = *s;
        }
        if (k < 3) f[k][n] = 0;
        sh_notify(f[0], f[1], f[2], v > 1 ? (int)v : ST.h * 100 + ST.mi);
        if (BN.on && !sp_bn.live) {}        /* a still: the banner at rest (below) */
        return 1;
    }
    if (!tk_strcmp(what, "banner")) {       /* the newest as a banner, p v (at rest) */
        if (nin) { bn_show(&NI[0]); sp_bn.live = 0; BN.p = v; BN.until = now_ms + 4000; bn_place(); }
        return 1;
    }
    if (!tk_strcmp(what, "nc")) {
        if (v <= 0) { nc_reset(); return 1; }
        nc_build();
        NC.on = 1; NC.closing = 0; NC.p = v;
        sp_nc.live = 0;
        nc_build_some(NC_SLOTS);
        if (v >= 1) nc_mark_read();
        nc_place();
        return 1;
    }
    if (!tk_strcmp(what, "ncgroup")) {
        if (!NC.built) return 1;
        nc_cpy(NC.exp, sizeof NC.exp, key);
        nc_ghead_build();
        nc_layout();
        for (int k = 0; k < NC_SLOTS; k++) if (NSL[k].id) { NSL[k].y = NSL[k].ty; NSL[k].a = NSL[k].ta; NSL[k].pa = NSL[k].tpa; }
        for (int k = 0; k < NC_SLOTS; k++) if (NSL[k].id && NSL[k].ta <= 0) nc_slot_free(&NSL[k]);
        NC.ghy = NC.ghty; NC.gha = NC.ghta;
        nc_build_some(NC_SLOTS);
        relink();
        nc_place();
        return 1;
    }
    if (!tk_strcmp(what, "ncswipe")) {      /* the newest platter swiped v pt left (its Clear button) */
        if (!NC.built) return 1;
        for (int k = 0; k < NC_SLOTS; k++)
            if (NSL[k].id && NSL[k].ta > .5f) { NSL[k].dx = NSL[k].tdx = -v; NC.reveal = k; break; }
        nc_place();
        return 1;
    }
    return 0;
}
