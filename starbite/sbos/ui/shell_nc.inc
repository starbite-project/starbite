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
static int hx_set(const char *what, const char *key, float v);   /* the home's widgets and Maps (below) */

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
/* B4: docs/design/IOS-REFERENCE.md 5 (v1.1, measured): x 14.5, w 361, r 23, 8 apart; 24.6 + 18 pt a line; the icon 38
 * at 13.7, centred; the words from 65; the time 18.3 in from the right on the title's baseline */
#define NC_X 14.5f
#define NC_W 361.0f
#define NC_R 23.0f
#define NC_TXW (NC_W - 65 - 18.3f)         /* the words' column */
#define NC_ACT 46.0f                       /* B4: an expanded platter's actions row (Open / Clear) */
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
static int nc_lines_n(const nc_item *n, char ln[4][96], int max)
{
    float wm;
    return n->body[0] ? wrapn(ln, max, TK_IT400, 15, n->body, NC_TXW, &wm) : 0;
}
static int nc_lines(const nc_item *n, char ln[4][96]) { return nc_lines_n(n, ln, 2); }
/* B4: 24.6 + 18 a line (the title and the body's), 60.6 at the least (the icon and its insets) */
static float nc_hh(int nl) { return tk_maxf(60.6f, 24.6f + 18.0f * (1 + nl)); }
/* iOS 26's platter: the app's icon (38 pt), the title (15 / 600) with the time (13, secondary) on its right, up to
 * two lines of text (15 / 400) at 18 pt; act: an expanded one's Open / Clear capsules along its bottom (B4) */
static void nc_draw_item_x(dc *d, const nc_item *n, float x, float top, float h, int nl, char ln[4][96], int act)
{
    int a = nc_app(n->who);
    float hb = h - (act ? NC_ACT : 0);          /* the words' part */
    nc_icon(d, a, nc_name(n->who), x + 13.7f, act ? top + 13.7f : top + (hb - 38) / 2, 38);
    float tx = x + 65, right = x + NC_W - 18.3f;
    char when[16];
    nc_when(when, n->hm);
    /* the first line's cap top 15 below the platter's top (centred when the platter is taller than its words) */
    float cap = tk_ascent(TK_IT600, PT(15)) / TK_PT * .78f, used = 24.6f + 18.0f * (1 + nl);
    float b1 = top + 15 + cap + (act ? 0 : tk_maxf(0, (hb - used) / 2));
    float ww = when[0] ? tw(TK_IT400, 13, when, 0, 0) : 0;
    d_text_box(d, TK_IT600, 15, tx, b1, right - tx - (ww ? ww + 8 : 0), 0, n->title[0] ? n->title : nc_name(n->who), 0, 0, INK, 0);
    if (ww) d_text(d, TK_IT400, 13, right - ww, b1, when, 0, 0, W(.6f), 0);
    for (int k = 0; k < nl; k++) d_text(d, TK_IT400, 15, tx, b1 + 18.0f * (k + 1), ln[k], 0, 0, W(.92f), 0);
    if (act) {
        float bw = (NC_W - 2 * 13.7f - 8) / 2, by = top + h - NC_ACT + 2;
        static const char *const L[2] = { "Open", "Clear" };
        for (int i = 0; i < 2; i++) {
            float bx = x + 13.7f + i * (bw + 8);
            d_rrect(d, bx, by, bw, 34, 17, W(.16f));
            d_text_box(d, TK_IT600, 15, bx + bw / 2, bl(TK_IT600, 15, by + 8), bw - 16, 1, L[i], 0, 0, i ? tk_hex(0xff6961, 1) : INK, 0);
        }
    }
}
static void nc_draw_item(dc *d, const nc_item *n, float x, float top, float h, int nl, char ln[4][96]) { nc_draw_item_x(d, n, x, top, h, nl, ln, 0); }

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
        v->x = PT(x - CARD_X) + K[g].face.bx;
        v->y = PT(y - top) + K[g].face.by;
        /* B4: the lock card's glass stretched to the platter's width; under its 3-slice's 68 pt it is scaled down */
        v->slice_h = PT(tk_maxf(68, h) + 2 * FACE_MG) - K[g].fcut;
        v->sx = NC_W / CARD_W;
        v->sy = h < 68 ? h / 68 : 1;
        v->ox = PT(x); v->oy = PT(y);
        v->clip = 0;
    } else {
        if (v->bmp.px) nc_unview(v);
        v->solid = tk_premul(TK_RGBA(48, 52, 74, .95f));
        v->x = tk_floorf(PT(x) + .5f);
        v->y = tk_floorf(PT(y) + .5f);
        v->solid_w = PT(NC_W);
        v->solid_h = PT(h);
        v->clip = 1;
        v->clip_r = (tk_rect){ v->x, v->y, PT(NC_W), PT(h) };
        v->clip_radius = PT(NC_R);
        v->sx = v->sy = 1;
    }
    v->alpha = a;
    v->hidden = a <= .004f;
}
/* the stack's lower cards under a platter (the lock card's peek band) */
static void pl_peek(tk_layer *v, float x, float y, float h, float a)
{
    int g = nc_peeksrc();
    if (a <= .004f) { v->hidden = 1; return; }
    if (g < 0) {
        /* no lock cards (Emergency): a solid lower card peeking under the solid platter */
        if (v->bmp.px) nc_unview(v);
        v->solid = tk_premul(TK_RGBA(48, 52, 74, .6f));
        v->x = tk_floorf(PT(x + 10) + .5f);
        v->y = tk_floorf(PT(y + h - 16) + .5f);
        v->solid_w = PT(NC_W - 20);
        v->solid_h = PT(16 + PEEK1);
        v->clip = 1;
        v->clip_r = (tk_rect){ v->x, v->y, v->solid_w, v->solid_h };
        v->clip_radius = PT(14);
        v->alpha = a;
        v->hidden = 0;
        return;
    }
    v->clip = 0;
    if (v->bmp.px != K[g].gh.bmp.px) nc_view(v, &K[g].gh);
    float top = LIST_TOP + K[g].y;
    v->sx = NC_W / CARD_W; v->sy = 1; v->ox = PT(x); v->oy = 0;
    v->x = PT(x - CARD_X) + K[g].gh.bx;
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
    float y, ty, a, ta, pa, tpa, dx, tdx, h, yb, hv;   /* now and the targets (list pt at scroll 0); hv: the drawn height */
    int nl, ref, dying, built, ex;          /* ex: expanded by a long press (B4) */
    tk_spring sdx;                          /* B4: a let-go swipe's way back (or on), with the finger's speed */
} nc_slot;
static nc_slot NSL[NC_SLOTS];
static tk_layer ncl, ncwall, ncstar, ncdim, nccont, ncsb, ncclk, nchead, nclist, ncempty, ncgh, ncclr, ncclrt, ncpill;
static tk_layer bnl, bnund, bngl, bntx;
static tk_layer bkl, bkund, bkgl, bktx;     /* B4: the banner pushed behind a newer one (the stacked look) */
enum { NG_NONE, NG_PULL, NG_DRAG, NG_LIST, NG_SCROLL, NG_SWIPE, NG_TAP, NG_BANNER, NG_PILL };
static struct {
    int on, built, closing, anim, g, moved, sl, clrall, reveal, momentum, instant, xp, swc;   /* xp: the expanded slot (B4) */
    float p, scroll, sv, x0, y0, base, ghy, ghty, gha, ghta, t0, xd;
    char exp[16];                           /* the fanned-out group's poster ("" none) */
    float clr_x[2], clr_w[2];               /* the fanned-out group's pills: Show less, Clear (pt, in its header) */
    tk_tracker tr;
} NC = { .reveal = -1, .xp = -1 };
static tk_spring sp_nc, sp_ncs;
static struct { int on, closing, drag, ex; unsigned id; float p, h, dy, until, h0, ge; } BN;   /* ex: expanded (B4) */
static struct { int on; unsigned id; float h, s; tk_spring sp; } BK;                                /* B4: the one behind */
static tk_spring sp_bn, sp_bndy, sp_bnge;
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
        s->hv = s->h;
        s->ex = 0;
        s->sdx.live = 0;
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
    int ex = s->ex;
    if (ex) nl = nc_lines_n(&NI[i], ln, 4);
    dc d = mk(&s->tx, "nc-item", NC_X, s->ty, NC_W, s->h, 2, 0);
    nc_draw_item_x(&d, &NI[i], NC_X, s->ty, s->h, nl, ln, ex);
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
        /* B4: while one is expanded the others step back (dimmed) */
        float a = s->a * (NC.xp >= 0 && &NSL[NC.xp] != s ? 1 - .7f * NC.xd : 1);
        pl_peek(&s->pk, NC_X + s->dx, s->y, s->hv, s->pa * a);
        pl_glass(&s->gl, NC_X + s->dx, s->y, s->hv, s->built ? a : 0);
        if (s->built && s->tx.bmp.px) {
            s->tx.x = s->tx.bx + tk_floorf(PT(s->dx) + .5f);
            s->tx.y = s->tx.by + tk_floorf(PT(s->y - s->yb) + .5f);
            s->tx.alpha = a;
            s->tx.hidden = a <= .004f;
            s->tx.clip = 1;                     /* (an expanding one's words show as its platter grows) */
            s->tx.clip_r = (tk_rect){ PT(NC_X + s->dx), PT(s->y), PT(NC_W), PT(s->hv) };
            s->tx.clip_radius = PT(NC_R);
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
        float w = 76, x = NC_X + NC_W - w, a = tk_clampf(-r->dx / 92, 0, 1);
        ncclr.solid = tk_premul(W(.2f));
        ncclr.x = tk_floorf(PT(x) + .5f); ncclr.y = tk_floorf(PT(r->y) + .5f);
        ncclr.solid_w = PT(w); ncclr.solid_h = PT(r->h);
        ncclr.clip = 1; ncclr.clip_r = (tk_rect){ ncclr.x, ncclr.y, PT(w), PT(r->h) }; ncclr.clip_radius = PT(NC_R);
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
        dc d = mk(&ncclrt, "nc-clear-word", NC_X + NC_W - 38 - w / 2, 0, w, 20, 2, 0);
        d_text(&d, TK_IT600, 15, NC_X + NC_W - 38 - w / 2, 15, "Clear", 0, 0, INK, 0);
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
    NC.xp = -1; NC.xd = 0;
    NC.on = NC.closing = 0;
    NC.p = 0;
    sp_nc.live = 0;
    nc_free();
}

/* ---- the store's changes -------------------------------------------------------------------------------------- */
static void bn_show(const nc_item *n);
static void bn_hide(float v);
static void bk_free(void);
static void nc_remove(int i)
{
    if (i < 0 || i >= nin) return;
    unsigned id = NI[i].id;
    for (int k = i; k + 1 < nin; k++) NI[k] = NI[k + 1];
    nin--;
    nc_slot *s = nc_slot_of(id);
    if (s && !s->dying) s->ta = 0;
    if (BN.on && BN.id == id && !BN.closing) bn_hide(0);
    if (BK.on && BK.id == id) { bk_free(); relink(); }
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

/* ---- the banner -------------------------------------------------------------------------------------------------
 * B4 (IOS-REFERENCE 5): at y 10, in on the banner spring (.45 / .80) sliding and growing from .92; 5 s on screen. A
 * newer one pushes the shown one behind it (the stacked look: .94 and peeking 8 pt below, its words gone); both go
 * up together. A long press expands it (four lines and Open / Clear; it then stays until swiped away); a let-go drag
 * goes back on a spring with the finger's speed, a flick up (projected 200 ms) dismisses it. */
#define BN_Y 10.0f
static float bn_hcur(void) { return BN.ex ? BN.h0 + (BN.h - BN.h0) * tk_clampf(BN.ge, 0, 1.05f) : BN.h; }
static void bn_under(tk_layer *u, float h)
{
    u->solid = tk_premul(TK_RGBA(16, 20, 36, .93f));    /* a dark ground under the glass: legible over any page */
    u->x = PT(NC_X); u->y = PT(BN_Y);
    u->solid_w = PT(NC_W); u->solid_h = PT(h);
    u->clip = 1; u->clip_r = (tk_rect){ u->x, u->y, PT(NC_W), PT(h) }; u->clip_radius = PT(NC_R);
}
static void bn_place(void)
{
    bnl.hidden = !BN.on;
    float p = BN.p, off = (1 - p) * -(BN.h + BN_Y + 30) + BN.dy;
    bnl.y = PT(off);
    bnl.sx = bnl.sy = .92f + .08f * tk_clampf(p, 0, 1.1f);
    bnl.ox = PT(195); bnl.oy = PT(BN_Y);
    if (BN.ex) {                                /* growing into its expanded size */
        float h = bn_hcur();
        bn_under(&bnund, h);
        pl_glass(&bngl, NC_X, BN_Y, h, 1);
        bntx.clip = 1;
        bntx.clip_r = (tk_rect){ PT(NC_X), PT(BN_Y), PT(NC_W), PT(h) };
        bntx.clip_radius = PT(NC_R);
    }
    if (BK.on) {
        float k = tk_clampf(BK.s, 0, 1.1f), sc = 1 - .06f * k, top = BN_Y + (BN.h + 8 - BK.h * sc) * k;
        bkl.sx = bkl.sy = sc;
        bkl.ox = PT(195); bkl.oy = PT(BN_Y);
        bkl.y = PT(top - BN_Y) / sc;
        bkl.hidden = 0;
        bktx.alpha = tk_clampf(1 - k * 1.5f, 0, 1);
        bktx.hidden = bktx.alpha <= .004f;
        bkgl.alpha = 1 - .3f * tk_minf(1, k);
        bkund.alpha = 1 - .25f * tk_minf(1, k);
    } else bkl.hidden = 1;
}
static void bk_free(void)
{
    BK.on = 0;
    BK.sp.live = 0;
    relayer(&bktx);
    nc_unview(&bkgl);
    bkl.hidden = 1;
}
static void bn_show(const nc_item *n)
{
    char ln[4][96];
    int nl = nc_lines(n, ln);
    float h = nc_hh(nl);
    if (BN.on && !BN.closing && BN.p > .3f && !BN.ex) {
        /* the shown one goes behind (its words' bitmap moves with it), the new one comes down over it */
        tk_layer t = bntx;
        tk_layer_init(&bntx, "banner-words");
        relayer(&bktx);
        bktx = t;
        bktx.next = bktx.child = 0;
        BK.on = 1; BK.h = BN.h; BK.id = BN.id; BK.s = 0;
        tk_layer_init(&bkl, "banner-back");
        tk_layer_init(&bkund, "banner-back-under");
        bn_under(&bkund, BK.h);
        if (!bkgl.name) tk_layer_init(&bkgl, "banner-back-glass");
        pl_glass(&bkgl, NC_X, BN_Y, BK.h, 1);
        tk_spring_start(&BK.sp, 0, 1, 0, TK_SPR_BANNER);
        BN.p = 0;
    } else if (BK.on) bk_free();
    relayer(&bntx);
    tk_layer_init(&bnl, "banner");
    dc d = mk(&bntx, "banner-words", NC_X, BN_Y, NC_W, h, 2, 0);
    nc_draw_item(&d, n, NC_X, BN_Y, h, nl, ln);
    trim(&bntx);
    tk_layer_prepare(&bntx);                /* it moves at once: its half copy now, not in the first frame */
    tk_layer_init(&bnund, "banner-under");
    bn_under(&bnund, h);
    if (!bngl.name) tk_layer_init(&bngl, "banner-glass");
    pl_glass(&bngl, NC_X, BN_Y, h, 1);
    if (!BN.on) BN.p = 0;
    BN.on = 1; BN.closing = 0; BN.drag = 0; BN.dy = 0; BN.ex = 0;
    sp_bndy.live = sp_bnge.live = 0;
    BN.h = h; BN.id = n->id;
    BN.until = -1;                          /* set at its first frame (the platform's clock) */
    tk_spring_start(&sp_bn, BN.p, 1, 0, TK_SPR_BANNER);
    relink();
    bn_place();
}
/* B4: the long press: four lines and Open / Clear, grown on the pop spring (no glass bake: the words only) */
static void bn_expand(void)
{
    int i = nc_find(BN.id);
    if (i < 0 || BN.ex) return;
    char ln[4][96];
    int nl = nc_lines_n(&NI[i], ln, 4);
    float h = nc_hh(nl) + NC_ACT;
    relayer(&bntx);
    dc d = mk(&bntx, "banner-words", NC_X, BN_Y, NC_W, h, 2, 0);
    nc_draw_item_x(&d, &NI[i], NC_X, BN_Y, h, nl, ln, 1);
    trim(&bntx);
    tk_layer_prepare(&bntx);
    BN.h0 = BN.h; BN.h = h; BN.ex = 1; BN.ge = 0;
    tk_spring_start(&sp_bnge, 0, 1, 0, TK_SPR_POP);
    if (BK.on) bk_free();
    sh_haptic_k(SH_HAP_MEDIUM);
    relink();
    bn_place();
}
static void bn_free(void)
{
    BN.on = BN.closing = BN.drag = BN.ex = 0;
    BN.p = 0;
    sp_bn.live = sp_bndy.live = sp_bnge.live = 0;
    relayer(&bntx);
    nc_unview(&bngl);
    bk_free();
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
    nc_unview(&bkgl);
    if (BN.on) pl_glass(&bngl, NC_X, BN_Y, bn_hcur(), 1);
    if (BK.on) pl_glass(&bkgl, NC_X, BN_Y, BK.h, 1);
    if (NC.on) nc_place_list();
}

/* ---- opening what a notification is about ------------------------------------------------------------------------ */
static int nc_launch(unsigned id)
{
    int i = nc_find(id);
    if (i < 0) return 0;
    int a = nc_app(NI[i].who);
    if (a >= 0) cm_nc_launch(NI[i].who, NI[i].title, a);   /* helper I: a message's thread, a missed call's Recents */
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

/* ---- B4: a long press expands a platter (four lines, Open / Clear); the others step back ---------------------------- */
static void nc_expand(int k)
{
    nc_slot *s = &NSL[k];
    int i = s->id ? nc_find(s->id) : -1;
    if (i < 0 || s->dying) return;
    char ln[4][96];
    int nl = nc_lines_n(&NI[i], ln, 4);
    s->ex = 1;
    s->h = nc_hh(nl) + NC_ACT;
    NC.xp = k;
    nc_slot_build(s);
    nc_layout();
    sh_haptic_k(SH_HAP_MEDIUM);
}
static void nc_collapse(void)
{
    if (NC.xp < 0) return;
    nc_slot *s = &NSL[NC.xp];
    NC.xp = -1;
    if (!s->id) return;
    s->ex = 0;
    s->h = nc_hh(s->nl);
    nc_slot_build(s);
    nc_layout();
}

/* ---- touches ------------------------------------------------------------------------------------------------------- */
static int nc_slot_at(float x, float y)
{
    if (y < NC_LT || y >= NC_BOT) return -1;
    float cy = y + NC.scroll;
    for (int k = 0; k < NC_SLOTS; k++) {
        nc_slot *s = &NSL[k];
        if (!s->id || s->dying || s->ta < .5f) continue;
        if (cy >= s->ty && cy < s->ty + s->h && x >= NC_X && x < NC_X + NC_W) return k;
    }
    return -1;
}
static int bn_hit(float x, float y)
{
    if (!BN.on || BN.closing || BN.p < .3f) return 0;
    float top = BN_Y + (1 - BN.p) * -(BN.h + BN_Y + 30) + BN.dy;
    return x >= NC_X - 4 && x < NC_X + NC_W + 4 && y >= top - 6 && y < top + bn_hcur() + (BK.on ? 8 : 0) + 6;
}
/* a touch that starts on the banner, on the open Centre or on the lock's pill: 1 if the Centre's (sh_touch: G_NC) */
static int nc_down(float x, float y, float t)
{
    tk_tracker_reset(&NC.tr);
    NC.x0 = x; NC.y0 = y; NC.moved = 0;
    NC.t0 = t;
    if (bn_hit(x, y)) { NC.g = NG_BANNER; BN.drag = 1; sp_bn.live = 0; sp_bndy.live = 0; tk_tracker_add(&NC.tr, y, t); return 1; }
    if (NC.on && !NC.closing) {
        sp_nc.live = 0;
        NC.momentum = 0;
        tk_tracker_add(&NC.tr, y, t);
        if (y >= H_PT - 34) { NC.g = NG_DRAG; return 1; }
        NC.sl = nc_slot_at(x, y);
        if (NC.sl >= 0 && NSL[NC.sl].ex) NC.sl = -1;   /* (the expanded one: its buttons, at the release) */
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
                NC.swc = NC.base < -44;
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
            s->sdx.live = 0;
            if ((s->dx < -44) != NC.swc) { NC.swc = s->dx < -44; sh_haptic_k(SH_HAP_SELECT); }   /* B4: the Clear threshold */
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
        if (s->dx + v * 200 < -NC_W * .5f || (v < -.6f && s->dx < -30)) {
            /* cleared: it flies off; a stack goes as a whole */
            int i = nc_find(s->id);
            int whole = i >= 0 && tk_strcmp(NC.exp, NI[i].who) && s->tpa > .5f;
            s->dying = 1; s->tdx = -440;
            NC.reveal = -1;
            if (whole) nc_clear_group(NI[i].who, 1);
            else nc_remove(i);
        } else if (s->dx + v * 120 < -44) s->tdx = -92;          /* the Clear button stays */
        else { s->tdx = 0; NC.reveal = -1; }
        if (!s->dying) tk_spring_start(&s->sdx, s->dx, s->tdx, v * 1000, TK_SPR_CARD);   /* B4: on with the finger's speed */
        NC.anim = 1;
        break;
    }
    case NG_BANNER:
        BN.drag = 0;
        if (!NC.moved) {
            unsigned id = BN.id;
            if (BN.ex) {                         /* B4: the expanded one's Open / Clear */
                float top = BN_Y + BN.dy, by = top + BN.h - NC_ACT + 2, bw = (NC_W - 2 * 13.7f - 8) / 2;
                if (y >= by - 4 && y < by + 38 && x >= NC_X + 13.7f + bw + 4) { int i = nc_find(id); bn_hide(0); nc_remove(i); break; }
            }
            bn_hide(0);
            (void)nc_launch(id);
        } else if (BN.dy + v * 200 < -30 || v < -.3f) bn_hide(v * 1000 / (BN.h + 40));   /* B4: where the flick would carry it */
        else {
            tk_spring_start(&sp_bndy, BN.dy, 0, v * 1000, TK_SPR_CARD);                  /* back with the finger's speed */
            BN.until = now_ms + 2500;        /* a little longer on screen */
        }
        break;
    case NG_LIST: case NG_TAP: {
        if (NC.moved) break;
        float cy = y + NC.scroll;
        if (NC.xp >= 0) {
            /* B4: an expanded platter: its Open / Clear, else a tap anywhere folds it back */
            nc_slot *e = &NSL[NC.xp];
            int i = e->id ? nc_find(e->id) : -1;
            float by = e->ty + e->h - NC_ACT + 2, bw = (NC_W - 2 * 13.7f - 8) / 2;
            if (i >= 0 && cy >= by - 4 && cy < by + 38 && x >= NC_X && x < NC_X + NC_W) {
                if (x < NC_X + 13.7f + bw + 4) { NC.xp = -1; if (nc_launch(e->id)) nc_close(0); }
                else { e->dying = 1; e->tdx = -440; NC.xp = -1; nc_remove(i); }
            } else nc_collapse();
            break;
        }
        /* a short swipe's Clear button, else the swipe put back */
        if (NC.reveal >= 0) {
            nc_slot *r = &NSL[NC.reveal];
            if (r->id && cy >= r->ty && cy < r->ty + r->h && x >= NC_X + NC_W - 80) {
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
        if (NC.g == NG_LIST && !NC.moved && NC.sl >= 0 && NC.xp < 0 && t - NC.t0 >= 500) { nc_expand(NC.sl); NC.g = NG_NONE; any = 1; }
        any |= nc_ease(&NC.xd, NC.xp >= 0 ? 1 : 0, k, .01f);
        for (int i = 0; i < NC_SLOTS; i++) {
            nc_slot *s = &NSL[i];
            if (!s->id) continue;
            any |= nc_ease(&s->y, s->ty, k, .2f);
            any |= nc_ease(&s->hv, s->h, k, .2f);
            if (s->sdx.live) { tk_spring_step(&s->sdx, dt); s->dx = s->sdx.x; any = 1; }
            any |= nc_ease(&s->a, s->ta, k * 1.2f, .01f);
            any |= nc_ease(&s->pa, s->tpa, k * 1.2f, .01f);
            if (!(NC.g == NG_SWIPE && NC.sl == i) && !s->sdx.live) any |= nc_ease(&s->dx, s->tdx, s->dying ? 1 - tk_expf(-dt * 9) : k, .2f);
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
        if (BN.until < 0) BN.until = t + 5000;   /* B4 (IOS-REFERENCE 5): about 5 s */
        if (NC.g == NG_BANNER && !NC.moved && !BN.ex && t - NC.t0 >= 500) { BN.drag = 0; bn_expand(); NC.g = NG_NONE; }
        if (sp_bndy.live) { tk_spring_step(&sp_bndy, dt); BN.dy = sp_bndy.x; bn_place(); mv = 1; }
        if (sp_bnge.live) { tk_spring_step(&sp_bnge, dt); BN.ge = sp_bnge.x; bn_place(); mv = 1; }
        if (BK.sp.live) { tk_spring_step(&BK.sp, dt); BK.s = BK.sp.x; bn_place(); mv = 1; }
        if (sp_bn.live) {
            int live = tk_spring_step(&sp_bn, dt);
            BN.p = tk_maxf(0, sp_bn.x);
            if (!live && BN.closing) bn_free();
            else bn_place();
            mv = 1;
        } else if (!BN.drag && !BN.closing && !BN.ex && t >= BN.until) { bn_hide(0); mv = 1; }
    }
    return mv;
}
static int nc_moving(void)
{
    return sp_nc.live || sp_ncs.live || NC.momentum || (NC.on && NC.anim) || sp_bn.live || sp_bndy.live || sp_bnge.live || BK.sp.live ||
           (NC.g == NG_LIST && !NC.moved && NC.sl >= 0 && NC.xp < 0) ||
           NC.g == NG_PULL || NC.g == NG_DRAG || NC.g == NG_SCROLL || NC.g == NG_SWIPE || NC.g == NG_BANNER;
}
/* the banner's rest: when to come back (ms, < 0 nothing) and the timed slide away */
static float nc_next_update(float t)
{
    if (!BN.on || BN.closing || BN.drag || BN.ex || sp_bn.live || BN.until < 0) return -1;
    return tk_maxf(1, BN.until - t);
}
static int nc_refresh(float t)
{
    if (BN.on && !BN.closing && !BN.drag && !BN.ex && !sp_bn.live && BN.until >= 0 && t >= BN.until) { bn_hide(0); return 1; }
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
    if (BN.on) {
        if (!bkl.name) { tk_layer_init(&bkl, "banner-back"); bkl.hidden = 1; }
        tk_layer *b[] = { &bkl, &bnund, &bngl, &bntx };
        link(&bnl, b, 4);
        if (BK.on) { tk_layer *k[] = { &bkund, &bkgl, &bktx }; link(&bkl, k, 3); }
        else bkl.child = 0;
    } else bnl.child = 0;
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
        if (nin) { bn_show(&NI[0]); sp_bn.live = 0; BN.p = v; BN.until = now_ms + 5000; bn_place(); }
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
    return hx_set(what, key, v);
}

/* ---- home's live widgets and Maps (helper D) ----------------------------------------------------------------------
 *   - the Systems widget: the boot menu's last choice first (sh_lastos, the loader's settings record; the concept's
 *     four otherwise), redrawn only when it changes; the lock's "LAST USED" follows it;
 *   - the Clock widget (page 1, rows 3-4): iOS's small analog clock, its hands redrawn on the minute (sh_status);
 *     a tap opens Clock from it;
 *   - Maps: an honest screen, no map: what it needs and the state of each, live (Wi-Fi and mobile data as the
 *     platform pushes them; the last GNSS fix asked from idle turns only, at most once a minute). */
void sh_lastos(const char *key)
{
    if (!key || !key[0]) return;
    for (int g = 0; g < NG; g++)
        for (int i = 0; i < G[g].n; i++) {
            const sys_t *s = &G[g].it[i];
            if (tk_strcmp(s->k, key) || s->self) continue;
            if (LAST && !tk_strcmp(LAST, s->k)) return;
            LAST = s->k;
            build_home_w();
            relink();
            prep_tree(home.child);
            lock_ver++;
            return;
        }
}
#define WCK_Y (HOME_TOP1 + 2 * ROW_PITCH)
static void build_wclock(void)
{
    tk_layer *l = &hparts[7];
    relayer(l);
    float x = WID_X(0), y = WCK_Y, s = WID_W, cx = x + s / 2, cy = y + s / 2;
    dc d = mk(l, "home-clock", x, y, s, s + 30, 2, 0);
    tk_col ink = tk_hex(0x1c1c1e, 1);
    d_rrect(&d, x, y, s, s, 24, ink);                              /* iOS's Clock widget: a black plate, a white face */
    d_rrect(&d, cx - 66, cy - 66, 132, 132, 66, W(1));
    for (int i = 0; i < 60; i++) {
        float a = 6.2831853f * i / 60, sn = tk_sinf(a), cs = tk_cosf(a), r0 = i % 5 ? 61.5f : 58;
        d_seg(&d, cx + r0 * sn, cy - r0 * cs, cx + 63.5f * sn, cy - 63.5f * cs, i % 5 ? .7f : 1.8f, i % 5 ? tk_hex(0x9a9aa0, 1) : ink);
    }
    for (int h = 1; h <= 12; h++) {
        char n[4];
        n[put_dec(n, h)] = 0;
        float a = 6.2831853f * h / 12, w = tw(TK_IT600, 12, n, 0, 0);
        d_text(&d, TK_IT600, 12, cx + 47 * tk_sinf(a) - w / 2, cy - 47 * tk_cosf(a) + 4.4f, n, 0, 0, ink, 0);
    }
    float ha = 6.2831853f * ((ST.h % 12) + ST.mi / 60.0f) / 12, ma = 6.2831853f * ST.mi / 60;
    d_seg(&d, cx, cy, cx + 31 * tk_sinf(ha), cy - 31 * tk_cosf(ha), 5, ink);
    d_seg(&d, cx, cy, cx + 52 * tk_sinf(ma), cy - 52 * tk_cosf(ma), 3.4f, ink);
    d_rrect(&d, cx - 4.5f, cy - 4.5f, 9, 9, 4.5f, ink);
    d_rrect(&d, cx - 2, cy - 2, 4, 4, 2, tk_hex(0xff9500, 1));
    float cw = tw(TK_IT500, 12, "Clock", 0, 0);
    d_text(&d, TK_IT500, 12, cx - cw / 2, y + s + 18, "Clock", 0, 0, INK, 1);
    trim(l);
    tk_layer_prepare(l);
    l->hidden = 0;
    relink();
}
static int wclock_tap(float x, float y)
{
    float x0 = x - page_x;
    if (x0 < WID_X(0) || x0 >= WID_X(0) + WID_W || y < WCK_Y || y >= WCK_Y + WID_W || page_x < -195) return 0;
    for (int a = 0; a < NAPPS; a++)
        if (!tk_strcmp(APPS[a].k, "clock")) {
            ap_from_on = 1;
            ap_from = (arect){ WID_X(0) + page_x, WCK_Y, WID_W, WID_W, 24 };
            app_open(a);
            return 1;
        }
    return 0;
}

/* Maps */
static struct { int asked, have; float t; sh_gnss_t fix; } MP;
static void mp_ask(void)
{
    sh_gnss_t f;
    tk_memset(&f, 0, sizeof f);
    int have = sh_gnss(&f) == 1;
    int ch = !MP.asked || have != MP.have || (have && (f.lat_e6 != MP.fix.lat_e6 || f.lon_e6 != MP.fix.lon_e6 || f.utc != MP.fix.utc));
    MP.asked = 1;
    MP.have = have;
    if (have) MP.fix = f;
    MP.t = now_ms;
    if (ch && is_app("maps") && !AP.closing) build_app_page(AP.a);
}
static void mp_idle(void)
{
    if (!AP.on || AP.closing || !is_app("maps") || ap_pending) return;
    if (!MP.asked || now_ms - MP.t > 60000 || now_ms < MP.t) mp_ask();
}
/* degrees x 1e6 as "60.17° N" */
static int mp_deg(char *b, int e6, const char *pos, const char *neg)
{
    int v = ((e6 < 0 ? -e6 : e6) + 5000) / 10000, n = put_dec(b, v / 100), c = v % 100;   /* to 0.01 degrees */
    b[n++] = '.'; b[n++] = (char)('0' + c / 10); b[n++] = (char)('0' + c % 10);
    const char *s = e6 < 0 ? neg : pos;
    while (*s) b[n++] = *s++;
    b[n] = 0;
    return n;
}
static const char *mp_wifi(void)
{
    switch (WF.state) {
    case SH_WIFI_CONNECTED: return WF.ssid[0] ? WF.ssid : "Connected";
    case SH_WIFI_JOINING: case SH_WIFI_DHCP: return "Connecting\xe2\x80\xa6";
    case SH_WIFI_STARTING: case SH_WIFI_UP: case SH_WIFI_SCANNING: return "Not Connected";
    default: return "Off";
    }
}
static const char *mp_cell(char *b)
{
    switch (CELL.data) {
    case SH_DATA_UP: { const char *r = cell_rat(); int n = 0; for (const char *s = "On \xc2\xb7 "; *s;) b[n++] = *s++; while (*r) b[n++] = *r++; b[n] = 0; return b; }
    case SH_DATA_STARTING: case SH_DATA_SEARCHING: case SH_DATA_REGISTERED: return "Connecting\xe2\x80\xa6";
    case SH_DATA_NO_SIM: return "No SIM";
    case SH_DATA_LOCKED: return "SIM Locked";
    case SH_DATA_FAILED: return "Didn't Start";
    default: return "Off";
    }
}
static void page_maps(dc *d)
{
    /* the glyph: Maps' blue disc, the location arrow */
    d_rrect(d, 195 - 44, 136, 88, 88, 44, tk_hex(0x0a84ff, 1));
    const tk_bmp *lc = tk_icon("loc");
    if (lc) { float f = 40.0f / 24, pad = tk_icon_pad("loc") * f; nc_blit_scaled(d->b, lc, PT(195 - 20 - 1.5f) - pad - d->ox, PT(180 - 20 + 1.5f) - pad - d->oy, f); }
    char ln[4][96];
    float wm;
    int n = wrapn(ln, 2, TK_IT700, 22, "Maps Needs the Internet and Location", 300, &wm);
    for (int k = 0; k < n; k++) d_text_box(d, TK_IT700, 22, 195, bl(TK_IT700, 22, 248 + 28.0f * k), CONTENT_W, 1, ln[k], 0, 0, W(.95f), 0);
    float y = 248 + 28.0f * n + 10;
    n = wrapn(ln, 3, TK_IT400, 15, "There are no maps stored on this phone. Maps can show one when the phone is online and knows where it is.", 310, &wm);
    for (int k = 0; k < n; k++) d_text_box(d, TK_IT400, 15, 195, bl(TK_IT400, 15, y + 20.0f * k), CONTENT_W, 1, ln[k], 0, 0, W(.55f), 0);
    /* what it needs, live */
    float gy = 430;
    d_text(d, TK_IT400, 13, ROW_TX, gy - 8, "STATUS", .52f, 0, W(.55f), 0);
    d_rrect(d, GROUP_X, gy, GROUP_W, 47 * 3 - 1, 16, tk_hex(0x151c33, 1));
    char cb[32], lb[48];
    const char *L[3] = { "Wi-Fi", "Cellular Data", "Location" }, *G3[3] = { "cc-wifi", "cc-cell", "loc" };
    const uint32_t col[3] = { 0x0a84ff, 0x30d158, 0x0a84ff };
    const float gw[3] = { 19, 18, 18 }, gh[3] = { 14, 16, 18 };
    const char *R[3] = { mp_wifi(), mp_cell(cb), "No Fix" };
    if (MP.have) {
        int k = mp_deg(lb, MP.fix.lat_e6, "\xc2\xb0 N", "\xc2\xb0 S");
        lb[k++] = ','; lb[k++] = ' ';
        mp_deg(lb + k, MP.fix.lon_e6, "\xc2\xb0 E", "\xc2\xb0 W");
        R[2] = lb;
    } else if (!MP.asked) R[2] = "Checking\xe2\x80\xa6";
    for (int r = 0; r < 3; r++) {
        float ry = gy + 47 * r;
        d_rrect(d, ROW_TX, ry + 9, 29, 29, 7, tk_hex(col[r], 1));
        const tk_bmp *ic = tk_icon(G3[r]);
        if (ic) {
            float pad = tk_icon_pad(G3[r]);
            if (r == 2) nc_blit_scaled(d->b, ic, PT(ROW_TX + 5.5f) - pad * .75f - d->ox, PT(ry + 14.5f) - pad * .75f - d->oy, .75f);
            else tk_blit_tinted(d->b, ic, (int)tk_floorf(PT(ROW_TX + (29 - gw[r]) / 2) - pad - d->ox + .5f),
                                (int)tk_floorf(PT(ry + 9 + (29 - gh[r]) / 2) - pad - d->oy + .5f), W(1));
        }
        float lx = ROW_TX + 29 + 13;
        d_text_box(d, TK_IT400, 16, lx, ry + 13 + 16, 140, 0, L[r], 0, 0, INK, 0);
        d_text_box(d, TK_IT400, 16, CONTENT_R - 16, ry + 13 + 16, CONTENT_R - 16 - lx - 130, 2, R[r], 0, TK_TNUM, W(.55f), 0);
        if (r < 2) d_rrect(d, lx, ry + 46, CONTENT_R - lx, .5f, 0, W(.08f));
    }
    /* the fix's details, or why there is none */
    char ft[160];
    int k = 0;
    if (MP.have) {
        long long z = MP.fix.utc / 86400 + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
        long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        int dd = (int)(doy - (153 * mp + 2) / 5 + 1), mm = (int)(mp < 10 ? mp + 3 : mp - 9), yy = (int)(yoe + era * 400 + (mm <= 2));
        int sec = (int)(MP.fix.utc % 86400), hh = sec / 3600, mi = sec / 60 % 60;
        for (const char *s = "Android's last fix: "; *s;) ft[k++] = *s++;
        k += put_dec(ft + k, dd); ft[k++] = ' ';
        for (int i = 0; i < 3; i++) ft[k++] = MO[(mm + 11) % 12][i];
        ft[k++] = ' '; k += put_dec(ft + k, yy); ft[k++] = ','; ft[k++] = ' ';
        ft[k++] = (char)('0' + hh / 10); ft[k++] = (char)('0' + hh % 10); ft[k++] = ':';
        ft[k++] = (char)('0' + mi / 10); ft[k++] = (char)('0' + mi % 10);
        for (const char *s = " UTC"; *s;) ft[k++] = *s++;
        if (MP.fix.acc_dm >= 0) {
            for (const char *s = " \xc2\xb7 \xc2\xb1"; *s;) ft[k++] = *s++;
            k += put_dec(ft + k, MP.fix.acc_dm / 10);
            if (MP.fix.acc_dm < 100) { ft[k++] = '.'; ft[k++] = (char)('0' + MP.fix.acc_dm % 10); }
            ft[k++] = ' '; ft[k++] = 'm';
        }
        if (MP.fix.sats >= 0) { ft[k++] = ' '; ft[k++] = (char)0xc2; ft[k++] = (char)0xb7; ft[k++] = ' '; k += put_dec(ft + k, MP.fix.sats); for (const char *s = " satellites"; *s;) ft[k++] = *s++; }
        ft[k] = 0;
    } else {
        const char *s = "Starbite OS can't use the GPS yet. The last fix Android saves to the memory card shows here.";
        while (*s) ft[k++] = *s++;
        ft[k] = 0;
    }
    n = wrapn(ln, 3, TK_IT400, 13, ft, CONTENT_W - 32, &wm);
    for (int i = 0; i < n; i++) d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, gy + 47 * 3 + 8 + 17.0f * i), ln[i], 0, 0, W(.45f), 0);
}
/* the host's stills: "lastos" KEY (sh_lastos), "mapsfix" (Maps asks for the fix now) */
static int hx_set(const char *what, const char *key, float v)
{
    (void)v;
    if (!tk_strcmp(what, "lastos")) { sh_lastos(key); return 1; }
    if (!tk_strcmp(what, "mapsfix")) { mp_ask(); return 1; }
    return 0;
}
