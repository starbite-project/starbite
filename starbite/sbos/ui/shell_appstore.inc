/* SPDX-License-Identifier: MIT
 * The App Store of Starbite OS Next (Track F): Linux apps for the big cores, as iOS 26's App Store looks and behaves,
 * but ours: every app offers one to three install methods (the project's latest release, a build from source on the
 * M3 cores, the Debian package; starbite/appstore/FORMAT.md) and the user picks one on the app's page. Included by
 * shell_linux.inc (its helpers: lx_make_icon, lx_fmt_kb, lx_scopy, lx_build_kb, lx_open_app); the data is the
 * platform's sh_lstore_t (shell.h, v3 from starbite/appstore/sbshm-store3.h through gwin.c / host/linux_fake.inc).
 *   - a Liquid Glass tab bar (Today, Browse, Updates with a red badge; Search as iOS 26's separate glass circle),
 *     baked once per opening (never in a tap): a glass layer that never changes and a small glyph layer over it;
 *   - Today (the date, the large title, the featured cards, "Top Apps in ..."), Browse (the categories, then a
 *     category's apps), Search (the field, Next's keyboard), Updates (check, Update All, roll back), and the app's
 *     page pushed over any of them (the glass back circle; the main button, the info strip, the method picker, the
 *     description, Information, Roll Back / Remove);
 *   - the page scrolls: it is drawn in chunks of AQ_CH pt (at most AQ_NCH layers, re-used as it scrolls, cut to what
 *     they hold) by one painter that also answers the taps (aq_paint: draw mode or hit mode), so a page is never a
 *     whole-screen bitmap and never built in a tap: a new page's chunks are drawn over the next frames (two a frame,
 *     aq_tick), then it slides in; progress redraws only the chunks that show the package that moved.
 * Hooks (shell_linux.inc's lx_*): aq_open, aq_close, aq_tap, aq_touch, aq_tick, aq_moving, aq_layers, aq_set;
 * st_build / st_changed / sh_linux_store keep their names (the Linux side calls them). ---- */
static void lx_open_app(int ci);

#define AQ_CH 128.0f                             /* a chunk: 128 pt of the page (1440 x 472 px, 2.7 MB) */
#define AQ_NCH 8
#define TB_Y 718.0f                              /* the tab bar: a 62 pt capsule and the search circle beside it */
#define TB_H 62.0f
#define TB_SD 62.0f
#define TB_W (CONTENT_W - TB_SD - 10)
#define TB_TABW (TB_W / 3)
#define RH 76.0f                                 /* an app's row: the 48 pt icon, two lines, the pill */
#define PILL_W 74.0f
#define PILL_X (CONTENT_R - PILL_W)
#define AQ_FIELD_Y CONTENT_TOP                   /* the search field, under Search's large title */
#define AQ_PAD 110.0f                            /* under a page's last row: the tab bar's room */
enum { AT_TODAY, AT_BROWSE, AT_UPDATES, AT_SEARCH };
enum { H_NONE, H_APP, H_ACT, H_CAT, H_CHECK, H_UPALL, H_ROLLBACK, H_METHOD, H_MAIN, H_OPEN, H_REINSTALL, H_REMOVE, H_SUGGEST };
static const char *const AQ_CATS[10] = { "Internet", "Office", "Graphics", "Media", "Games", "Utilities", "Development",
                                         "Education", "Science", "System" };
static const uint32_t AQ_CATC[11] = { 0x0a84ff, 0xff9f0a, 0xff375f, 0xff453a, 0xbf5af2, 0x8e8e93, 0x5e5ce6, 0x30d158,
                                      0x32ade6, 0x636366, 0x2c7be5 };   /* [10]: All Apps */
static const char *const AQ_SUGG[6] = { "Games", "Text editor", "Graphics", "Office", "Terminal", "Media" };
static struct {
    int tab, cat_on, app, pick, kb, shift, num, rm_arm, badge, empty;
    char cat[16], pkg[40], q[24];
    float scroll, v, top, bot, h, rm_t, sroot[4], scat;
    int ck[AQ_NCH], dirty[AQ_NCH];               /* each chunk layer's chunk (-1 none), to be drawn again */
    uint32_t pk[AQ_NCH][5];                      /* the packages each chunk shows (progress redraws only those) */
    int hide, dir;                               /* a new page's chunks hidden until drawn; then slid in from dir */
    float rev;                                   /* the slide's time, 0..1 (1: at rest) */
    int drag, moved, left;                       /* left: chunks still to draw (frames keep coming) */
    float y0, s0, ly, lt;                        /* the finger: down y, the scroll then, the last move's y and time */
    uint32_t sig, psig[SH_LPKGS];                /* what the pages show (structure), each package's progress */
} AQ;
static tk_layer aqc[AQ_NCH], aqh, aqtb, aqtg;    /* the chunks, the fixed head, the tab bar's glass and its glyphs */
static tk_bmp aq_dic;                            /* the app page's 100 pt icon */
static char aq_dic_pkg[40];
static struct { int icon; uint32_t key; tk_bmp b; } SIC[24];   /* the packages' 48 pt icons */
static struct { uint32_t key; uint32_t col; } AQC[16];         /* a featured card's colour (its icon's mean) */
static int aq_tb_baked;
/* the live parts (a busy package's ring and its status line): small layers over the chunks, the only thing a
 * progress step redraws (a chunk is ~20 ms of text on the Mac) */
#define AQ_NLIVE 6
enum { LV_ROW, LV_CARD, LV_HEAD };
static struct { int pkg, kind; float x, y, w; } AQL[32];
static int aq_nl, aq_collect;
static tk_layer aql[AQ_NLIVE];
static int aql_it[AQ_NLIVE] = { -1, -1, -1, -1, -1, -1 }, aql_dirty[AQ_NLIVE];

/* ---- the data ---- */
static int st_find(const char *pkg)
{
    if (!LST) return -1;
    for (int i = 0; i < LST->n && i < SH_LPKGS; i++) if (!tk_strcmp(LST->pkg[i].pkg, pkg)) return i;
    return -1;
}
static int st_busy(const sh_lpkg_t *p) { return p->state == SH_PKG_QUEUED || p->state == SH_PKG_INSTALLING || p->state == SH_PKG_REMOVING; }
static int aq_inst(const sh_lpkg_t *p) { return (p->flags & SH_PKG_INSTALLED) != 0; }
static int aq_upd(const sh_lpkg_t *p) { return aq_inst(p) && (p->flags & SH_PKG_UPDATE); }
static int aq_n(void) { return LST ? tk_mini(LST->n, SH_LPKGS) : 0; }
static uint32_t aq_hash(uint32_t h, const char *s) { for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }
static uint32_t aq_hu(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
/* the catalog app a package brought (its OPEN), or -1 */
static int st_app(int i)
{
    for (int a = 0; a < LCAT.n; a++) if (LCAT.app[a].pkg == i) return a;
    return -1;
}
static int aq_badge(void)
{
    int n = 0;
    for (int i = 0; i < aq_n(); i++) n += aq_upd(&LST->pkg[i]);
    return n;
}
static int aq_cat_n(const char *cat)
{
    int n = 0;
    for (int i = 0; i < aq_n(); i++) n += !cat[0] || !tk_strcmp(LST->pkg[i].category, cat);
    return n;
}
static const char *aq_mname(unsigned m) { return m == SH_M_RELEASE ? "Latest release" : m == SH_M_SOURCE ? "Build from source" : m == SH_M_DISTRO ? "Debian package" : "Default"; }
static const char *aq_mver(const sh_lpkg_t *p, unsigned m) { return m == SH_M_RELEASE ? p->ver_rel : m == SH_M_SOURCE ? p->ver_src : p->ver_distro; }
/* a string built of parts (NULL-terminated), cut at cap */
static void aq_cat(char *b, int cap, const char *const *parts)
{
    int n = 0;
    for (int k = 0; parts[k]; k++) for (const char *c = parts[k]; *c && n < cap - 1; c++) b[n++] = *c;
    b[n] = 0;
}
#define AQS(b, ...) aq_cat(b, (int)sizeof b, (const char *const[]){ __VA_ARGS__, 0 })
static char *aq_dec(char *b, int v) { b[put_dec(b, v)] = 0; return b; }
/* MB with one decimal (12.3), for "Downloading 12.3 of 40 MB" */
static void aq_mb1(char *b, unsigned kb)
{
    unsigned t = (kb + 50u) / 100u;
    int n = put_dec(b, (int)(t / 10));
    b[n++] = '.'; b[n++] = (char)('0' + t % 10); b[n] = 0;
}
/* the busy / failed line of a package (its phase in words); 0: none (idle) */
static int aq_status(const sh_lpkg_t *p, char *b, int cap, tk_col *c)
{
    char x[24], y[24];
    *c = C_LABEL2;
    if (p->state == SH_PKG_QUEUED) { lx_scopy(b, "Waiting\xe2\x80\xa6", cap); return 1; }
    if (p->state == SH_PKG_REMOVING) { lx_scopy(b, "Removing\xe2\x80\xa6", cap); return 1; }
    if (p->state == SH_PKG_FAILED) { lx_scopy(b, p->err[0] ? p->err : "Didn't install", cap); *c = C_RED; return 1; }
    if (p->state != SH_PKG_INSTALLING) return 0;
    if (p->phase == SH_PKG_DOWNLOADING && p->total_kb) {
        if (p->total_kb >= 1000000) {
            aq_mb1(x, p->done_kb / 1000); aq_mb1(y, p->total_kb / 1000);
            char t[96]; AQS(t, "Downloading ", x, " of ", y, " GB"); lx_scopy(b, t, cap);
        } else {
            aq_mb1(x, p->done_kb);
            aq_dec(y, (int)((p->total_kb + 500) / 1000));
            char t[96]; AQS(t, "Downloading ", x, " of ", y, " MB"); lx_scopy(b, t, cap);
        }
    } else if (p->phase == SH_PKG_DOWNLOADING) lx_scopy(b, "Downloading\xe2\x80\xa6", cap);
    else if (p->phase == SH_PKG_VERIFYING) lx_scopy(b, "Verifying\xe2\x80\xa6", cap);
    else if (p->phase == SH_PKG_BUILDING) {
        if (!p->eta_s) lx_scopy(b, "Building\xe2\x80\xa6", cap);
        else if (p->eta_s < 60) lx_scopy(b, "Building\xe2\x80\xa6 less than a minute left", cap);
        else { aq_dec(x, (int)((p->eta_s + 59) / 60)); char t[96]; AQS(t, "Building\xe2\x80\xa6 about ", x, " min left"); lx_scopy(b, t, cap); }
    } else lx_scopy(b, "Installing\xe2\x80\xa6", cap);
    *c = W(.7f);
    return 1;
}
static float aq_frac(const sh_lpkg_t *p)
{
    if (p->state == SH_PKG_QUEUED) return .06f;
    if (p->state == SH_PKG_INSTALLING && p->phase == SH_PKG_DOWNLOADING && p->total_kb) return tk_clampf((float)p->done_kb / (float)p->total_kb, .02f, 1);
    return tk_clampf(p->pct / 100.0f, .02f, 1);
}
/* the packages' 48 pt icons (cached; a list keeps at most 24) */
static const tk_bmp *aq_icon(int i)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    uint32_t key = aq_hash(2166136261u, p->pkg);
    int free_k = -1;
    for (int k = 0; k < 24; k++) {
        if (SIC[k].b.px && SIC[k].icon == p->icon && SIC[k].key == key) return &SIC[k].b;
        if (!SIC[k].b.px && free_k < 0) free_k = k;
    }
    if (free_k < 0) { free_k = (int)(key % 24); tk_bmp_free(&SIC[free_k].b); }
    SIC[free_k].icon = p->icon;
    SIC[free_k].key = key;
    lx_make_icon(&SIC[free_k].b, (int)(PT(48) + .5f), p->icon, p->name, 0);
    return &SIC[free_k].b;
}
/* a featured card's colour: its icon's mean colour (the pool's icon, transparent ground), kept dark enough for
 * white text */
static uint32_t aq_card_col(int i)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    uint32_t key = aq_hu(aq_hash(2166136261u, p->pkg), (uint32_t)p->icon);
    for (int k = 0; k < 16; k++) if (AQC[k].key == key) return AQC[k].col;
    uint32_t col = 0x2c3e66;
    uint32_t *px = p->icon >= 0 ? tk_alloc((size_t)SH_LINUX_ICON * SH_LINUX_ICON * 4) : 0;
    if (px && sh_linux_icon(p->icon, px)) {
        float r = 0, g = 0, b = 0, a = 0;
        for (int k = 0; k < SH_LINUX_ICON * SH_LINUX_ICON; k += 3) {
            uint32_t q = px[k];
            float al = (float)(q >> 24) / 255.0f, sat;
            float R = (float)(q >> 16 & 255), G2 = (float)(q >> 8 & 255), B = (float)(q & 255);
            sat = tk_maxf(R, tk_maxf(G2, B)) - tk_minf(R, tk_minf(G2, B));   /* colourful px count more than grey */
            float w = al * (.15f + sat / 255.0f);
            r += R * w; g += G2 * w; b += B * w; a += w * al;
        }
        if (a > 1) {
            float R = r / a, G2 = g / a, B = b / a, m = tk_maxf(R, tk_maxf(G2, B)), k = m > 1 ? 150.0f / m : 1;
            col = (uint32_t)tk_clampf(R * k, 20, 255) << 16 | (uint32_t)tk_clampf(G2 * k, 20, 255) << 8 | (uint32_t)tk_clampf(B * k, 20, 255);
        }
    }
    if (px) tk_free(px);
    int s = (int)(key % 16);
    AQC[s].key = key;
    AQC[s].col = col;
    return col;
}
static void aq_icons_free(void)
{
    for (int k = 0; k < 24; k++) tk_bmp_free(&SIC[k].b);
    tk_bmp_free(&aq_dic);
    aq_dic_pkg[0] = 0;
}

/* ---- small glyphs (drawn: no pack icons needed) ---- */
static void d_rrect_stroke(dc *d, float x, float y, float w, float h, float r, float sw, tk_col c)
{
    int X0 = (int)tk_floorf(PT(x - sw) - d->ox) - 1, Y0 = (int)tk_floorf(PT(y - sw) - d->oy) - 1;
    int n = (int)PT(w + 2 * sw) + 4, m2 = (int)PT(h + 2 * sw) + 4;
    tk_fm m;
    if (!tk_fm_new(&m, n, m2)) return;
    float ax = PT(x) - d->ox - X0, ay = PT(y) - d->oy - Y0, hw = PT(sw) / 2;
    for (int j = 0; j < m.h; j++)
        for (int i = 0; i < m.w; i++) {
            float s = tk_sd_rrect(i + .5f, j + .5f, ax, ay, PT(w), PT(h), PT(r));
            m.v[j * m.w + i] = tk_clampf(hw - tk_fabsf(s) + .5f, 0, 1);
        }
    tk_paint p = tk_solid(c);
    tk_draw_fm(d->b, &m, X0, Y0, &p, 1);
    tk_fm_free(&m);
}
static void d_chev(dc *d, float x, float cy, tk_col c) { d_seg(d, x, cy - 6, x + 6, cy, 2.2f, c); d_seg(d, x + 6, cy, x, cy + 6, 2.2f, c); }
/* a category's 30 pt glyph square (Settings' style: a colour square, a white glyph) */
static void d_catglyph(dc *d, int k, float x, float y)
{
    tk_col bg = tk_hex(AQ_CATC[k], 1), w = W(1);
    float cx = x + 15, cy = y + 15;
    d_rrect(d, x, y, 30, 30, 7, bg);
    switch (k) {
    case 0: d_ring(d, cx, cy, 8.5f, 1.8f, 1, w, 0); d_seg(d, cx - 8, cy, cx + 8, cy, 1.6f, w); d_seg(d, cx, cy - 8, cx, cy + 8, 1.6f, w);
            d_ring(d, cx, cy, 4, 1.6f, 1, w, 0); break;                                   /* Internet: a globe */
    case 1: d_rrect(d, cx - 7, cy - 9, 14, 18, 2.5f, w);
            for (int i = 0; i < 3; i++) d_rrect(d, cx - 4, cy - 4.5f + 4.5f * i, i == 2 ? 5 : 8, 1.8f, .9f, bg);   /* Office: a page */
            break;
    case 2: d_rrect(d, cx - 9, cy - 8, 18, 16, 8, w);
            d_rrect(d, cx - 5.5f, cy - 4.5f, 3.5f, 3.5f, 1.75f, bg); d_rrect(d, cx - .5f, cy - 5.5f, 3.5f, 3.5f, 1.75f, bg);
            d_rrect(d, cx + 3.5f, cy - 1.5f, 3.5f, 3.5f, 1.75f, bg); break;            /* Graphics: a palette */
    case 3: d_tri(d, cx - 5, cy - 8, cx + 8, cy, cx - 5, cy + 8, w); break;              /* Media: play */
    case 4: d_rrect(d, cx - 10.5f, cy - 6, 21, 12, 6, w); d_seg(d, cx - 6.5f, cy, cx - 2.5f, cy, 1.6f, bg);
            d_seg(d, cx - 4.5f, cy - 2, cx - 4.5f, cy + 2, 1.6f, bg);
            d_rrect(d, cx + 2.5f, cy - 1.5f, 3, 3, 1.5f, bg); d_rrect(d, cx + 5.5f, cy - 1.5f, 3, 3, 1.5f, bg); break;   /* Games: a gamepad */
    case 5: for (int i = 0; i < 3; i++) { float ly = cy - 6 + 6 * i, kx = cx + (i == 1 ? 3 : -3);
                d_seg(d, cx - 8, ly, cx + 8, ly, 1.6f, w); d_rrect(d, kx - 2.5f, ly - 2.5f, 5, 5, 2.5f, w); } break;   /* Utilities: sliders */
    case 6: d_seg(d, cx - 4, cy - 6, cx - 9, cy, 1.9f, w); d_seg(d, cx - 9, cy, cx - 4, cy + 6, 1.9f, w);
            d_seg(d, cx + 4, cy - 6, cx + 9, cy, 1.9f, w); d_seg(d, cx + 9, cy, cx + 4, cy + 6, 1.9f, w);
            d_seg(d, cx + 1.5f, cy - 7, cx - 1.5f, cy + 7, 1.7f, w); break;            /* Development: </> */
    case 7: d_rrect(d, cx - 9, cy - 7, 8, 15, 1.5f, w); d_rrect(d, cx + 1, cy - 7, 8, 15, 1.5f, w); break;   /* Education: a book */
    case 8: d_rrect(d, cx - 2.5f, cy - 9, 5, 7, 1, w); d_tri(d, cx - 3, cy - 3, cx + 3, cy - 3, cx + 8.5f, cy + 8, w);
            d_tri(d, cx - 3, cy - 3, cx + 8.5f, cy + 8, cx - 8.5f, cy + 8, w); break;   /* Science: a flask */
    case 9: d_seg(d, cx - 8, cy - 5, cx - 3, cy, 1.9f, w); d_seg(d, cx - 3, cy, cx - 8, cy + 5, 1.9f, w);
            d_seg(d, cx, cy + 6, cx + 8, cy + 6, 1.9f, w); break;                       /* System: >_ */
    default: for (int i = 0; i < 4; i++) d_rrect(d, cx - 8 + 9 * (i % 2), cy - 8 + 9 * (i / 2), 7, 7, 2, w);   /* All Apps */
    }
}

/* ---- the painter: the open page, in draw mode (c->d: one chunk's band) or hit mode (c->d NULL: what is at hx, hy).
 * Everything in screen pt as at scroll 0 ---- */
typedef struct { dc *d; float y0, y1, hx, hy; int L, hit, hpkg, harg; } pctx;
static int vis(const pctx *c, float y, float h) { return c->d && y < c->y1 && y + h > c->y0; }
static int at(const pctx *c, float x, float y, float w, float h) { return !c->d && c->hx >= x && c->hx < x + w && c->hy >= y && c->hy < y + h; }
static void aq_mark(pctx *c, int i) { if (c->L >= 0 && i >= 0 && i < SH_LPKGS) AQ.pk[c->L][i >> 5] |= 1u << (i & 31); }
static void aq_hit(pctx *c, int what, int pkg, int arg) { if (!c->hit) { c->hit = what; c->hpkg = pkg; c->harg = arg; } }
static void aq_live_add(int kind, int pkg, float x, float y, float w)
{
    if (!aq_collect || aq_nl >= 32) return;
    AQL[aq_nl].kind = kind; AQL[aq_nl].pkg = pkg; AQL[aq_nl].x = x; AQL[aq_nl].y = y; AQL[aq_nl].w = w;
    aq_nl++;
}
static void aq_blit(dc *d, const tk_bmp *b, float x, float y) { if (b && b->px) tk_blit(d->b, b, (int)tk_floorf(PT(x) - d->ox + .5f), (int)tk_floorf(PT(y) - d->oy + .5f), 1); }
/* the row's button (PILL_W x 30): GET / OPEN / UPDATE / RETRY, or the progress ring with its stop square */
static void aq_pill(dc *d, float x, float y, const sh_lpkg_t *p, int light)
{
    if (st_busy(p)) {
        float cx = x + PILL_W / 2, cy = y + 15;
        d_ring(d, cx, cy, 13, 2.6f, 1, W(.18f), 0);
        d_ring(d, cx, cy, 13, 2.6f, aq_frac(p), light ? W(1) : ACCENT(), 1);
        if (p->state != SH_PKG_REMOVING) d_rrect(d, cx - 4, cy - 4, 8, 8, 1.5f, light ? W(1) : ACCENT());
        return;
    }
    int inst = aq_inst(p), dim = !inst && (p->flags & (SH_PKG_NO_SPACE | SH_PKG_NEEDS_NET));
    const char *t = aq_upd(p) ? "UPDATE" : inst ? "OPEN" : p->state == SH_PKG_FAILED ? "RETRY" : "GET";
    d_rrect(d, x, y, PILL_W, 30, 15, light ? W(.24f) : W(.14f));
    tk_col c = light ? W(1) : ACCENT();
    if (dim) c.a = .4f;
    float fs = aq_upd(p) ? 13.5f : 15;
    d_text_box(d, TK_IT700, fs, x + PILL_W / 2, bl(TK_IT700, fs, y + 15 - fs * .62f), PILL_W - 8, 1, t, .2f, 0, c, 0);
}
/* an app's row: the 48 pt icon, the name, a line (the status while busy, else sub), the pill; sep: the rule under */
static float aq_row(pctx *c, float y, int i, const char *sub, int sep, int rollback)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    int rb = rollback && (p->flags & SH_PKG_ROLLBACK) && aq_inst(p) && !st_busy(p);
    float h = rb ? 96 : RH, tx = MARGIN + 62, tw_ = PILL_X - 10 - tx;
    if (vis(c, y, h)) {
        dc *d = c->d;
        aq_blit(d, aq_icon(i), MARGIN, y + 14);
        d_text_box(d, TK_IT600, 16, tx, bl(TK_IT600, 16, y + 16), tw_, 0, p->name, 0, 0, INK, 0);
        char ln[96];
        tk_col col;
        if (!st_busy(p)) {                       /* (busy: the live layer has the status and the ring) */
            if (!aq_status(p, ln, sizeof ln, &col)) { lx_scopy(ln, sub ? sub : p->summary, sizeof ln); col = C_LABEL2; }
            d_text_box(d, TK_IT400, 13, tx, bl(TK_IT400, 13, y + 39), tw_, 0, ln, 0, 0, col, 0);
            aq_pill(d, PILL_X, y + 23, p, 0);
        }
        if (rb) d_text_box(d, TK_IT500, 13, tx, bl(TK_IT500, 13, y + 62), tw_, 0, "Roll Back to the Previous Version", 0, 0, ACCENT(), 0);
        if (sep) d_rrect(d, tx, y + h - .5f, CONTENT_R - tx, .5f, 0, W(.12f));
        aq_mark(c, i);
    }
    if (st_busy(p)) aq_live_add(LV_ROW, i, tx, y, tw_);
    if (!c->d && c->hy >= y && c->hy < y + h) {
        if (c->hx >= PILL_X - 10 && c->hy >= y + 12 && c->hy < y + 64) aq_hit(c, H_ACT, i, 0);
        else if (rb && c->hy >= y + 52 && c->hx >= tx && c->hx < tx + 250) aq_hit(c, H_ROLLBACK, i, 0);
        else aq_hit(c, H_APP, i, 0);
    }
    return y + h;
}
/* a section's header: 22 pt Bold, an optional accent link at the right ("See All", "Update All") */
static float aq_sec(pctx *c, float y, const char *t, const char *link, int what, int arg)
{
    if (vis(c, y, 40)) {
        d_text_box(c->d, TK_IT700, 22, MARGIN, bl(TK_IT700, 22, y + 8), link ? CONTENT_W - 100 : CONTENT_W, 0, t, 0, 0, INK, 0);
        if (link) d_text_box(c->d, TK_IT400, 17, CONTENT_R, bl(TK_IT400, 17, y + 12), 120, 2, link, 0, 0, ACCENT(), 0);
    }
    if (link && at(c, CONTENT_R - 110, y, 110, 40)) aq_hit(c, what, -1, arg);
    return y + 40;
}
static void aq_title(pctx *c, const char *t) { if (vis(c, SAFE_TOP + NAVBAR_H, LARGE_H)) d_h1(c->d, t); }

/* Today: the date and the large title, the featured cards, "Top Apps in" the two biggest categories */
#define CARD_H 264.0f
static float aq_card(pctx *c, float y, int i)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    if (vis(c, y, CARD_H + 20)) {
        dc *d = c->d;
        uint32_t col = aq_card_col(i);
        tk_col c0 = tk_hex(col, 1), c1 = tk_hex(col, 1);
        c0.r *= .62f; c0.g *= .62f; c0.b *= .62f;
        c1.r *= .22f; c1.g *= .22f; c1.b *= .22f;
        tk_stop s[2] = { { 0, c0 }, { 1, c1 } };
        tk_paint g = tk_linear(180, PT(MARGIN) - d->ox, PT(y) - d->oy, PT(CONTENT_W), PT(CARD_H), 2, s);
        tk_fill_rrect(d->b, PT(MARGIN) - d->ox, PT(y) - d->oy, PT(CONTENT_W), PT(CARD_H), PT(22), &g, 1);
        char up[40];
        int k = 0;
        const char *lab = p->flags & SH_PKG_INSTALLED ? "ON YOUR PHONE" : "FEATURED APP";
        for (const char *q = lab; *q && k < 38; q++) up[k++] = *q;
        up[k] = 0;
        d_text(d, TK_IT600, 13, MARGIN + 20, bl(TK_IT600, 13, y + 20), up, .3f, 0, W(.7f), 0);
        char ln[4][96];
        float wm;
        int n = wrapn(ln, 2, TK_IT700, 27, p->summary[0] ? p->summary : p->name, CONTENT_W - 40, &wm);
        for (int j = 0; j < n; j++) d_text(d, TK_IT700, 27, MARGIN + 20, bl(TK_IT700, 27, y + 42 + 32.0f * j), ln[j], 0, 0, W(1), 0);
        if (p->desc[0]) {                        /* the description's start under it (iOS's editorial line) */
            int m = wrapn(ln, 3, TK_IT400, 15, p->desc, CONTENT_W - 40, &wm);
            for (int j = 0; j < m; j++) d_text(d, TK_IT400, 15, MARGIN + 20, bl(TK_IT400, 15, y + 52 + 32.0f * n + 20.0f * j), ln[j], 0, 0, W(.72f), 0);
        }
        float by = y + CARD_H - 72;
        d_rrect(d, MARGIN + 20, by, CONTENT_W - 40, .5f, 0, W(.18f));
        aq_blit(d, aq_icon(i), MARGIN + 20, by + 12);
        float tx = MARGIN + 80, tw_ = CONTENT_R - 20 - PILL_W - 10 - tx;
        d_text_box(d, TK_IT600, 16, tx, bl(TK_IT600, 16, by + 15), tw_, 0, p->name, 0, 0, W(1), 0);
        if (!st_busy(p)) {
            char l2[96];
            tk_col lc;
            if (!aq_status(p, l2, sizeof l2, &lc)) lx_scopy(l2, p->category, sizeof l2);
            d_text_box(d, TK_IT400, 13, tx, bl(TK_IT400, 13, by + 38), tw_, 0, l2, 0, 0, W(.7f), 0);
            aq_pill(d, CONTENT_R - 20 - PILL_W, by + 21, p, 1);
        }
        aq_mark(c, i);
    }
    if (st_busy(p)) aq_live_add(LV_CARD, i, MARGIN + 80, y + CARD_H - 72, CONTENT_R - 20 - PILL_W - 10 - MARGIN - 80);
    if (at(c, MARGIN, y, CONTENT_W, CARD_H)) aq_hit(c, c->hx >= CONTENT_R - 20 - PILL_W - 8 && c->hy >= y + CARD_H - 60 ? H_ACT : H_APP, i, 0);
    return y + CARD_H + 20;
}
/* the two categories with the most apps (ties: the catalog's order) */
static void aq_topcats(int *a, int *b)
{
    int n[10];
    *a = *b = -1;
    for (int k = 0; k < 10; k++) n[k] = aq_cat_n(AQ_CATS[k]);
    for (int k = 0; k < 10; k++) if (n[k] && (*a < 0 || n[k] > n[*a])) *a = k;
    for (int k = 0; k < 10; k++) if (n[k] && k != *a && (*b < 0 || n[k] > n[*b])) *b = k;
}
static float aq_top(pctx *c, float y, int k)
{
    char t[48];
    AQS(t, "Top Apps in ", AQ_CATS[k]);
    y = aq_sec(c, y, t, "See All", H_CAT, k);
    int shown = 0, tot = tk_mini(3, aq_cat_n(AQ_CATS[k]));
    for (int i = 0; i < aq_n() && shown < 3; i++)
        if (!tk_strcmp(LST->pkg[i].category, AQ_CATS[k])) { y = aq_row(c, y, i, 0, shown + 1 < tot, 0); shown++; }
    return y + 22;
}
static float pg_today(pctx *c)
{
    if (vis(c, SAFE_TOP, NAVBAR_H + 4)) {
        char t[48], dd[4];
        aq_dec(dd, ST.day);
        AQS(t, WD[ST.wday % 7], " ", dd, " ", MO[(ST.mon + 11) % 12]);
        for (char *q = t; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        d_text(c->d, TK_IT600, 13, MARGIN, LARGE_BASE - 38, t, .3f, 0, C_LABEL2, 0);
    }
    aq_title(c, "Today");
    float y = CONTENT_TOP + 4;
    int ka, kb, nf = 0;
    aq_topcats(&ka, &kb);
    for (int i = 0; i < aq_n(); i++) {
        if (!(LST->pkg[i].flags & SH_PKG_FEATURED)) continue;
        y = aq_card(c, y, i);
        if (++nf == 2 && ka >= 0) y = aq_top(c, y + 4, ka);
        if (nf == 5) break;
    }
    if (nf < 2 && ka >= 0) y = aq_top(c, y + 4, ka);
    if (kb >= 0) y = aq_top(c, y + 4, kb);
    return y;
}
/* Browse: the categories (with All Apps first) as an inset group */
static float pg_browse(pctx *c)
{
    aq_title(c, "Browse");
    float y = CONTENT_TOP + 4;
    int rows[11], n = 0;
    rows[n++] = 10;
    for (int k = 0; k < 10; k++) if (aq_cat_n(AQ_CATS[k])) rows[n++] = k;
    if (vis(c, y, 52.0f * n)) d_rrect(c->d, GROUP_X, y, GROUP_W, 52.0f * n, 16, C_CARD);
    for (int r = 0; r < n; r++) {
        float ry = y + 52 * r;
        int k = rows[r];
        if (vis(c, ry, 52)) {
            dc *d = c->d;
            d_catglyph(d, k, GROUP_X + 14, ry + 11);
            d_text(d, TK_IT400, 17, GROUP_X + 58, bl(TK_IT400, 17, ry + 15), k == 10 ? "All Apps" : AQ_CATS[k], 0, 0, INK, 0);
            char nb[8];
            aq_dec(nb, aq_cat_n(k == 10 ? "" : AQ_CATS[k]));
            d_text_box(d, TK_IT400, 17, CONTENT_R - 34, bl(TK_IT400, 17, ry + 15), 60, 2, nb, 0, TK_TNUM, W(.5f), 0);
            d_chev(d, CONTENT_R - 24, ry + 26, W(.3f));
            if (r + 1 < n) d_rrect(d, GROUP_X + 58, ry + 51.5f, GROUP_W - 58, .5f, 0, W(.1f));
        }
        if (at(c, GROUP_X, ry, GROUP_W, 52)) aq_hit(c, H_CAT, -1, k);
    }
    y += 52.0f * n + 18;
    if (vis(c, y, 40)) {
        char ln[4][96];
        float wm;
        int m = wrapn(ln, 2, TK_IT400, 13, "Apps run in Linux on the phone's big cores. Each one installs from its latest release, from source or as a Debian package.", GROUP_W - 32, &wm);
        for (int j = 0; j < m; j++) d_text(c->d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y + 18.0f * j), ln[j], 0, 0, W(.5f), 0);
    }
    return y + 44;
}
/* a category's apps (pushed from Browse; "" all) */
static float pg_cat(pctx *c)
{
    aq_title(c, AQ.cat[0] ? AQ.cat : "All Apps");
    float y = CONTENT_TOP;
    int tot = aq_cat_n(AQ.cat), k = 0;
    for (int i = 0; i < aq_n(); i++)
        if (!AQ.cat[0] || !tk_strcmp(LST->pkg[i].category, AQ.cat)) { y = aq_row(c, y, i, 0, ++k < tot, 0); }
    return y;
}
/* Search: the field is in the fixed head; the results (or Discover's suggestions) scroll under it */
static int aq_match(const sh_lpkg_t *p)
{
    return ci_has(p->name, AQ.q) || ci_has(p->pkg, AQ.q) || ci_has(p->summary, AQ.q) || ci_has(p->category, AQ.q);
}
static float pg_search(pctx *c)
{
    float y = AQ.top + 6;
    if (!AQ.q[0]) {
        y = aq_sec(c, y, "Discover", 0, 0, 0);
        for (int k = 0; k < 6; k++) {
            if (vis(c, y, 44)) {
                dc *d = c->d;
                d_ring(d, MARGIN + 7, y + 20, 6, 1.8f, 1, W(.45f), 0);
                d_seg(d, MARGIN + 11.5f, y + 24.5f, MARGIN + 15.5f, y + 28.5f, 2, W(.45f));
                d_text(d, TK_IT400, 19, MARGIN + 28, bl(TK_IT400, 19, y + 10), AQ_SUGG[k], 0, 0, ACCENT(), 0);
                if (k < 5) d_rrect(d, MARGIN + 28, y + 43.5f, CONTENT_W - 28, .5f, 0, W(.12f));
            }
            if (at(c, MARGIN, y, CONTENT_W, 44)) aq_hit(c, H_SUGGEST, -1, k);
            y += 44;
        }
        return y;
    }
    int tot = 0, k = 0;
    for (int i = 0; i < aq_n(); i++) tot += aq_match(&LST->pkg[i]);
    if (!tot) {
        if (vis(c, y, 120)) {
            char l[64];
            AQS(l, "for \xe2\x80\x9c", AQ.q, "\xe2\x80\x9d");
            d_empty(c->d, y, y + 140, "No Results", l);
        }
        return y + 140;
    }
    for (int i = 0; i < aq_n(); i++)
        if (aq_match(&LST->pkg[i])) y = aq_row(c, y, i, 0, ++k < tot, 0);
    return y;
}
/* Updates: check, Update All, the apps with an update, the installed ones (with roll back), the free space */
static void aq_checked(char *b, int cap)
{
    long long now = sh_utc();
    unsigned t = LST ? LST->checked_at : 0;
    if (!t) { lx_scopy(b, "Not checked yet", cap); return; }
    char n[12];
    if (now > 0 && (long long)t <= now && now - t < 86400LL * 30) {
        long long d = now - t;
        if (d < 120) { lx_scopy(b, "Checked just now", cap); return; }
        aq_dec(n, (int)(d < 3600 ? d / 60 : d < 86400 ? d / 3600 : d / 86400));
        char s[48];
        AQS(s, "Checked ", n, d < 3600 ? " min ago" : d < 86400 ? " h ago" : " days ago");
        lx_scopy(b, s, cap);
        return;
    }
    lx_scopy(b, "Checked", cap);
}
static float pg_updates(pctx *c)
{
    aq_title(c, "Updates");
    float y = CONTENT_TOP + 4;
    if (vis(c, y, 50)) {
        dc *d = c->d;
        char ck[40];
        aq_checked(ck, sizeof ck);
        d_rrect(d, GROUP_X, y, GROUP_W, 50, 16, C_CARD);
        d_text(d, TK_IT400, 17, ROW_TX, bl(TK_IT400, 17, y + 14), "Check for Updates", 0, 0, ACCENT(), 0);
        d_text_box(d, TK_IT400, 15, CONTENT_R - 16, bl(TK_IT400, 15, y + 16), 150, 2, ck, 0, 0, W(.5f), 0);
    }
    if (at(c, GROUP_X, y, GROUP_W, 50)) aq_hit(c, H_CHECK, -1, 0);
    y += 50;
    if (LST && LST->v3 && !LST->net_ok) {
        if (vis(c, y, 30))
            d_text_box(c->d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y + 8), GROUP_W - 32, 0,
                       LST->mirror_ok ? "No network: the memory card's mirror is used." : "No network and no mirror: connect to Wi-Fi to check.", 0, 0, W(.5f), 0);
        y += 26;
    }
    y += 18;
    int nu = aq_badge(), k = 0;
    y = aq_sec(c, y, "Available", nu > 1 ? "Update All" : 0, H_UPALL, 0);
    if (!nu) {
        if (vis(c, y, 30)) d_text(c->d, TK_IT400, 15, MARGIN, bl(TK_IT400, 15, y + 4), "Every app is up to date.", 0, 0, W(.5f), 0);
        y += 34;
    }
    for (int i = 0; i < aq_n(); i++) {
        const sh_lpkg_t *p = &LST->pkg[i];
        if (!aq_upd(p)) continue;
        char s[96];
        AQS(s, p->ver_inst[0] ? p->ver_inst : "?", " \xe2\x86\x92 ", p->ver_new[0] ? p->ver_new : "newer", p->method ? " \xc2\xb7 " : "", p->method ? aq_mname(p->method) : "");
        if (tw(TK_IT400, 13, s, 0, 0) > PILL_X - 10 - MARGIN - 62)   /* too long: the versions alone */
            AQS(s, p->ver_inst[0] ? p->ver_inst : "?", " \xe2\x86\x92 ", p->ver_new[0] ? p->ver_new : "newer");
        y = aq_row(c, y, i, s, ++k < nu, 1);
    }
    y += 22;
    int ni = 0;
    for (int i = 0; i < aq_n(); i++) ni += aq_inst(&LST->pkg[i]) && !aq_upd(&LST->pkg[i]);
    if (ni) {
        y = aq_sec(c, y, "Installed", 0, 0, 0);
        k = 0;
        for (int i = 0; i < aq_n(); i++) {
            const sh_lpkg_t *p = &LST->pkg[i];
            if (!aq_inst(p) || aq_upd(p)) continue;
            char s[96];
            if (p->flags & SH_PKG_SYSTEM) AQS(s, p->ver_inst[0] ? p->ver_inst : "", p->ver_inst[0] ? " \xc2\xb7 " : "", "Part of the system");
            else AQS(s, p->ver_inst[0] ? p->ver_inst : "Installed", p->method ? " \xc2\xb7 " : "", p->method ? aq_mname(p->method) : "");
            y = aq_row(c, y, i, s, ++k < ni, 1);
        }
        y += 18;
    }
    if (LST && LST->repo_ok && vis(c, y, 24)) {
        char f[24], b[64];
        lx_fmt_kb(f, LST->overlay_free_kb);
        AQS(b, f, " free on the memory card");
        d_text_box(c->d, TK_IT400, 13, 195, bl(TK_IT400, 13, y), CONTENT_W, 1, b, 0, 0, W(.45f), 0);
    }
    return y + 24;
}
/* the app's page */
static int aq_needs_net(const sh_lpkg_t *p) { return !aq_inst(p) && (p->flags & SH_PKG_NEEDS_NET) && (!AQ.pick || (unsigned)AQ.pick == p->def_method); }
static int aq_mrow_n(const sh_lpkg_t *p) { int n = 0; for (unsigned m = 1; m <= 3; m++) n += (p->methods >> m) & 1; return n; }
static float aq_textblock(pctx *c, float y, int f, float size, float lhh, const char *s, tk_col col)
{
    /* greedy word wrap of a long text (whole words; a word wider than the line by characters) */
    float maxw = CONTENT_W;
    while (*s) {
        char t[200];
        int st = 0, last = -1, k = 0;
        for (;;) {
            int e = k;
            while (s[e] && s[e] != ' ' && s[e] != '\n' && e < 190) e++;
            tk_memcpy(t, s, (size_t)e);
            t[e] = 0;
            if (tw(f, size, t, 0, 0) > maxw) {
                if (last < 0) {                    /* one word wider than the line */
                    int m = e;
                    while (m > 1) { m--; while (m > 1 && ((unsigned char)s[m] & 0xc0) == 0x80) m--; t[m] = 0; if (tw(f, size, t, 0, 0) <= maxw) break; }
                    last = m;
                }
                break;
            }
            last = e;
            if (!s[e] || s[e] == '\n' || e >= 190) break;
            k = e + 1;
        }
        (void)st;
        tk_memcpy(t, s, (size_t)last);
        t[last] = 0;
        if (vis(c, y - size, size * 1.6f)) d_text(c->d, f, size, MARGIN, y, t, 0, 0, col, 0);
        y += lhh;
        s += last;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
        if (!last) break;
    }
    return y;
}
/* a coloured note under the app's head (two lines at most) */
static float aq_note(pctx *c, float y, const char *t, uint32_t col)
{
    char ln[4][96];
    float wm;
    int n = wrapn(ln, 2, TK_IT500, 13, t, CONTENT_W, &wm);
    for (int k = 0; k < n; k++)
        if (vis(c, y - 4 + 18.0f * k, 22)) d_text(c->d, TK_IT500, 13, MARGIN, bl(TK_IT500, 13, y - 2 + 18.0f * k), ln[k], 0, 0, tk_hex(col, 1), 0);
    return y + 6 + 18.0f * n;
}
static float pg_app(pctx *c)
{
    int i = st_find(AQ.pkg);
    if (i < 0) return AQ.top;
    const sh_lpkg_t *p = &LST->pkg[i];
    float y0 = PUSHED_TOP, tx = 136, tw_ = CONTENT_R - tx;
    int inst = aq_inst(p), busy = st_busy(p), net = aq_needs_net(p), full = !inst && (p->flags & SH_PKG_NO_SPACE);
    /* the head: the icon, the name, the summary, the main button (or the progress) */
    if (vis(c, y0, 104)) {
        dc *d = c->d;
        if (tk_strcmp(aq_dic_pkg, p->pkg) || !aq_dic.px) { lx_make_icon(&aq_dic, (int)(PT(100) + .5f), p->icon, p->name, 0); lx_scopy(aq_dic_pkg, p->pkg, 40); }
        aq_blit(d, &aq_dic, MARGIN, y0);
        d_text_box(d, TK_IT700, 22, tx, bl(TK_IT700, 22, y0 + 2), tw_, 0, p->name, 0, 0, INK, 0);
        char ln[4][96];
        float wm;
        int n = wrapn(ln, 2, TK_IT400, 14, p->summary[0] ? p->summary : p->category, tw_, &wm);
        for (int k = 0; k < n; k++) d_text(d, TK_IT400, 14, tx, bl(TK_IT400, 14, y0 + 30 + 18.0f * k), ln[k], 0, 0, C_LABEL2, 0);
        float by = y0 + 70;
        if (busy) {
            /* the live layer: the ring, the phase, Cancel */
        } else {
            const char *t = net ? "Needs Wi-Fi" : full ? "Not Enough Space" : aq_upd(p) ? "UPDATE" : inst ? "OPEN" : p->state == SH_PKG_FAILED ? "RETRY" : "GET";
            int dis = net || full;
            float w = tk_maxf(84, tw(TK_IT700, 15, t, .2f, 0) + 36);
            d_rrect(d, tx, by, w, 32, 16, dis ? W(.1f) : ACCENT());
            d_text_box(d, TK_IT700, 15, tx + w / 2, bl(TK_IT700, 15, by + 7), w - 8, 1, t, .2f, 0, dis ? W(.4f) : W(1), 0);
            if (aq_upd(p)) {                     /* the update's OPEN beside it */
                float ox = tx + w + 10;
                d_rrect(d, ox, by, 84, 32, 16, W(.14f));
                d_text_box(d, TK_IT700, 15, ox + 42, bl(TK_IT700, 15, by + 7), 76, 1, "OPEN", .2f, 0, ACCENT(), 0);
            }
        }
        aq_mark(c, i);
    }
    if (busy) aq_live_add(LV_HEAD, i, tx, y0 + 70, tw_);
    if (!c->d && c->hy >= y0 + 64 && c->hy < y0 + 108 && c->hx >= tx) {
        float w = tk_maxf(84, tw(TK_IT700, 15, aq_upd(p) ? "UPDATE" : "OPEN", .2f, 0) + 36);
        aq_hit(c, aq_upd(p) && !busy && c->hx >= tx + w + 4 ? H_OPEN : H_MAIN, i, 0);
    }
    float y = y0 + 116;
    /* a one-line note: Wi-Fi, space, the failure, memory */
    const char *note = 0;
    uint32_t ncol = 0xffb340;
    char nb[160];
    if (net) note = "Needs Wi-Fi: the memory card's mirror doesn't have it.";
    else if (full) {
        char s[24], f[24];
        lx_fmt_kb(s, p->inst_kb);
        lx_fmt_kb(f, LST->overlay_free_kb);
        AQS(nb, "It needs ", s, " on the memory card; ", f, " is free.");
        note = nb; ncol = 0xff6961;
    } else if (p->state == SH_PKG_FAILED) { AQS(nb, "Didn't install: ", p->err[0] ? p->err : "the store stopped with an error."); note = nb; ncol = 0xff6961; }
    if (note) y = aq_note(c, y, note, ncol);
    if (p->flags & SH_PKG_TOO_BIG) {
        char s[16], m[16];
        lx_fmt_kb(s, p->ram_mb * 1000u);
        lx_fmt_kb(m, LST->mem_total_kb ? LST->mem_total_kb : 1500000);
        AQS(nb, "Uses about ", s, " of memory; Linux has ", m, ". It may run slowly.");
        y = aq_note(c, y, nb, 0xffb340);
    }
    /* the info strip: four cells between rules */
    if (vis(c, y, 72)) {
        dc *d = c->d;
        char v[4][24], u[4][24];
        static const char *const CAP[4] = { "SIZE", "MEMORY", "TOUCH", "LICENCE" };
        char s[24];
        lx_fmt_kb(s, p->rel_size_mb && (p->method == SH_M_RELEASE || (!inst && AQ.pick == SH_M_RELEASE)) ? p->rel_size_mb * 1000u : p->inst_kb);
        int k = 0;
        while (s[k] && (unsigned char)s[k] != 0xc2) k++;
        tk_memcpy(v[0], s, (size_t)k); v[0][k] = 0;
        lx_scopy(u[0], s[k] ? s + k + 2 : "", 24);
        lx_fmt_kb(s, p->ram_mb * 1000u);
        k = 0;
        while (s[k] && (unsigned char)s[k] != 0xc2) k++;
        tk_memcpy(v[1], s, (size_t)k); v[1][k] = 0;
        AQS(u[1], s[k] ? s + k + 2 : "", " in use");
        static const char *const TCH[4] = { "\xe2\x80\x94", "Poor", "OK", "Good" };
        lx_scopy(v[2], TCH[p->touch & 3], 24);
        lx_scopy(u[2], p->touch ? "with touch" : "not rated", 24);
        /* the licence: its first id, without "-or-later" / "-only" */
        k = 0;
        const char *L = p->licence[0] ? p->licence : "\xe2\x80\x94";
        while (L[k] && L[k] != ' ' && k < 23) k++;
        tk_memcpy(v[3], L, (size_t)k); v[3][k] = 0;
        int open = !ci_has(p->licence, "Proprietary") && p->licence[0];
        for (int q = 0; v[3][q]; q++)
            if (v[3][q] == '-' && (!tk_strcmp(v[3] + q, "-or-later") || !tk_strcmp(v[3] + q, "-only"))) { v[3][q] = 0; break; }
        if (L[k] == ' ' && (L[k + 1] == 'O' || L[k + 1] == 'A')) {   /* "MIT OR Apache-2.0": "or Apache-2.0" */
            const char *r = L + k + (L[k + 1] == 'O' ? 4 : 5);
            AQS(u[3], L[k + 1] == 'O' ? "or " : "and ", r);
        } else lx_scopy(u[3], ci_has(p->licence, "-or-later") ? "or later" : open ? "open source" : "", 24);
        d_rrect(d, MARGIN, y, CONTENT_W, .5f, 0, W(.14f));
        float cw = CONTENT_W / 4;
        for (int q = 0; q < 4; q++) {
            float cx = MARGIN + cw * q + cw / 2;
            d_text_box(d, TK_IT600, 11, cx, bl(TK_IT600, 11, y + 12), cw - 6, 1, CAP[q], .4f, 0, W(.45f), 0);
            float vs = tw(TK_IT700, 19, v[q], 0, 0) <= cw - 8 ? 19 : tw(TK_IT700, 15, v[q], 0, 0) <= cw - 8 ? 15 : 13;
            d_text_box(d, TK_IT700, vs, cx, bl(TK_IT700, vs, y + 28 + (19 - vs) * .7f), cw - 8, 1, v[q], 0, 0, W(.8f), 0);
            d_text_box(d, TK_IT400, 11, cx, bl(TK_IT400, 11, y + 52), cw - 6, 1, u[q], 0, 0, W(.45f), 0);
            if (q) d_rrect(d, MARGIN + cw * q, y + 14, .5f, 44, 0, W(.14f));
        }
        d_rrect(d, MARGIN, y + 72, CONTENT_W, .5f, 0, W(.14f));
    }
    y += 72;
    if (p->flags3 & SH_PKG3_TERMINAL) {
        if (vis(c, y, 30)) {
            dc *d = c->d;
            d_rrect(d, MARGIN, y + 9, 22, 18, 4, W(.16f));
            d_seg(d, MARGIN + 5, y + 14, MARGIN + 8.5f, y + 17.5f, 1.4f, W(.8f));
            d_seg(d, MARGIN + 8.5f, y + 17.5f, MARGIN + 5, y + 21, 1.4f, W(.8f));
            d_seg(d, MARGIN + 11, y + 22, MARGIN + 16, y + 22, 1.4f, W(.8f));
            d_text(d, TK_IT400, 13, MARGIN + 32, bl(TK_IT400, 13, y + 10), "Opens in Terminal: it runs with the keyboard up.", 0, 0, C_LABEL2, 0);
        }
        y += 34;
    }
    y += 14;
    /* the install method picker (B2's kit): a segmented control of the offered methods, the chosen one's card */
    int nm = aq_mrow_n(p);
    if (nm) {
        y = aq_sec(c, y, "Install Method", 0, 0, 0);
        static const unsigned ORD[3] = { SH_M_RELEASE, SH_M_SOURCE, SH_M_DISTRO };
        static const char *const LBL[4] = { "", "Debian", "Latest Release", "From Source" };
        const char *labels[3];
        unsigned ms[3];
        int n = 0, sel = -1;
        for (int q = 0; q < 3; q++) {
            unsigned m = ORD[q];
            if (!((p->methods >> m) & 1)) continue;
            if ((unsigned)AQ.pick == m) sel = n;
            ms[n] = m, labels[n++] = LBL[m];
        }
        if (n > 1) {
            float sw = (GROUP_W - 4) / n;
            if (vis(c, y, 32)) kit_d_segment(c->d, GROUP_X, y, GROUP_W, labels, n, sel);
            for (int k = 0; k < n; k++)
                if (at(c, GROUP_X + 2 + sw * k, y - 6, sw, 44)) aq_hit(c, H_METHOD, i, (int)ms[k]);
            y += 32 + 10;
        }
        unsigned m = sel >= 0 ? ms[sel] : ms[0];
        if (vis(c, y, 62)) {
            dc *d = c->d;
            char s[120], bm[8];
            const char *v = aq_mver(p, m);
            if (inst && p->method == m) {
                AQS(s, "Installed ", p->ver_inst[0] ? p->ver_inst : "", aq_upd(p) ? " \xc2\xb7 " : "", aq_upd(p) ? p->ver_new : "", aq_upd(p) ? " available" : "");
            } else if (m == SH_M_RELEASE) {
                int deb = p->source[0] == 'D' && p->source[1] == 'e';
                AQS(s, v[0] ? v : "Newest", p->source[0] && !deb ? " \xc2\xb7 " : "", p->source[0] && !deb ? p->source : "");
            } else if (m == SH_M_SOURCE) {
                aq_dec(bm, (int)(p->build_min ? p->build_min : 10));
                AQS(s, "About ", bm, " min on the M3 cores", v[0] ? " \xc2\xb7 " : "", v);
            } else AQS(s, v[0] ? v : "Debian", " \xc2\xb7 Debian trixie");
            d_rrect(d, GROUP_X, y, GROUP_W, 62, 16, C_CARD);
            d_text(d, TK_IT400, 17, ROW_TX, bl(TK_IT400, 17, y + 11), aq_mname(m), 0, 0, INK, 0);
            d_text_box(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y + 35), GROUP_W - 32, 0, s, 0, 0, C_LABEL2, 0);
        }
        y += 62 + 10;
        if (inst && !busy && AQ.pick && (unsigned)AQ.pick != p->method && !(p->flags & SH_PKG_SYSTEM)) {
            char t[64];
            AQS(t, "Reinstall: ", aq_mname(AQ.pick));
            if (vis(c, y, 50)) {
                d_rrect(c->d, GROUP_X, y, GROUP_W, 50, 16, C_CARD);
                d_text_box(c->d, TK_IT600, 17, 195, bl(TK_IT600, 17, y + 14), GROUP_W - 32, 1, t, 0, 0, ACCENT(), 0);
            }
            if (at(c, GROUP_X, y, GROUP_W, 50)) aq_hit(c, H_REINSTALL, i, 0);
            y += 60;
        } else if (busy) y += 4;
        else if (vis(c, y, 20)) {
            const char *f = inst ? "Choose another method, then Reinstall to switch." : "GET installs it the way chosen above.";
            d_text_box(c->d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y), GROUP_W - 32, 0, f, 0, 0, W(.45f), 0);
            y += 26;
        } else y += 26;
        y += 14;
    }
    /* the description, what it needs */
    y = aq_textblock(c, y + 18, TK_IT400, 16, 22, p->desc[0] ? p->desc : p->summary[0] ? p->summary : p->name, INK) - 4;
    if (p->needs) {
        static const char *const NN[6] = { "Network", "Audio", "GPU", "Camera", "Files", "Keyboard" };
        char s[96];
        int n = 0;
        s[0] = 0;
        for (int k = 0; k < 6; k++)
            if (p->needs & 1u << k) {
                const char *parts[3] = { n ? ", " : "Needs: ", NN[k], 0 };
                int l = (int)tk_strlen(s);
                aq_cat(s + l, (int)sizeof s - l, parts);
                n++;
            }
        if (vis(c, y, 24)) d_text_box(c->d, TK_IT400, 14, MARGIN, bl(TK_IT400, 14, y + 6), CONTENT_W, 0, s, 0, 0, C_LABEL2, 0);
        y += 28;
    }
    y += 16;
    /* Information: label and value rows */
    y = aq_sec(c, y, "Information", 0, 0, 0);
    {
        const char *lab[7];
        char val[7][64];
        int n = 0;
        if (inst) { lab[n] = "Installed"; AQS(val[n], p->ver_inst[0] ? p->ver_inst : "Yes", p->method ? " \xc2\xb7 " : "", p->method ? aq_mname(p->method) : ""); n++; }
        if (p->ver_new[0]) { lab[n] = "Newest"; lx_scopy(val[n], p->ver_new, 64); n++; }
        lab[n] = "Category"; lx_scopy(val[n], p->category[0] ? p->category : "Linux", 64); n++;
        if (p->source[0]) { lab[n] = "Source"; lx_scopy(val[n], p->source, 64); n++; }
        if (p->licence[0]) { lab[n] = "Licence"; lx_scopy(val[n], p->licence, 64); n++; }
        lab[n] = "Download"; lx_fmt_kb(val[n], p->dl_kb); n++;
        lab[n] = "Package"; lx_scopy(val[n], p->pkg, 64); n++;
        for (int r = 0; r < n; r++) {
            float ry = y + 44.0f * r;
            if (vis(c, ry, 44)) {
                d_text(c->d, TK_IT400, 15, MARGIN, bl(TK_IT400, 15, ry + 13), lab[r], 0, 0, W(.5f), 0);
                d_text_box(c->d, TK_IT400, 15, CONTENT_R, bl(TK_IT400, 15, ry + 13), CONTENT_W - 100, 2, val[r], 0, 0, INK, 0);
                if (r + 1 < n) d_rrect(c->d, MARGIN, ry + 43.5f, CONTENT_W, .5f, 0, W(.1f));
            }
        }
        y += 44.0f * n + 22;
    }
    /* Roll Back / Remove */
    if (inst && !busy) {
        if (p->flags & SH_PKG_SYSTEM) {
            if (vis(c, y, 24)) d_text(c->d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y), "Part of the system: it can't be removed.", 0, 0, W(.45f), 0);
            y += 30;
        } else {
            int rb = (p->flags & SH_PKG_ROLLBACK) != 0, n = 1 + rb;
            if (vis(c, y, 50.0f * n)) d_rrect(c->d, GROUP_X, y, GROUP_W, 50.0f * n, 16, C_CARD);
            if (rb) {
                if (vis(c, y, 50)) {
                    d_text(c->d, TK_IT400, 17, ROW_TX, bl(TK_IT400, 17, y + 14), "Roll Back to the Previous Version", 0, 0, ACCENT(), 0);
                    d_rrect(c->d, ROW_TX, y + 49.5f, CONTENT_R - ROW_TX, .5f, 0, W(.1f));
                }
                if (at(c, GROUP_X, y, GROUP_W, 50)) aq_hit(c, H_ROLLBACK, i, 0);
                y += 50;
            }
            if (vis(c, y, 50)) d_text(c->d, TK_IT400, 17, ROW_TX, bl(TK_IT400, 17, y + 14), "Remove App", 0, 0, C_RED, 0);
            if (at(c, GROUP_X, y, GROUP_W, 50)) aq_hit(c, H_REMOVE, i, 0);
            y += 50;
        }
    }
    return y;
}
static float aq_paint(pctx *c)
{
    if (!LST || LST->n <= 0) return AQ.top;
    if (AQ.app) return pg_app(c);
    if (AQ.tab == AT_TODAY) return pg_today(c);
    if (AQ.tab == AT_BROWSE) return AQ.cat_on ? pg_cat(c) : pg_browse(c);
    if (AQ.tab == AT_UPDATES) return pg_updates(c);
    return pg_search(c);
}

/* ---- the layers ---- */
static void aq_area(void)
{
    AQ.top = AQ.tab == AT_SEARCH && !AQ.app ? AQ_FIELD_Y + 46 : SAFE_TOP;
    AQ.bot = AQ.kb && AQ.tab == AT_SEARCH && !AQ.app ? 540 : H_PT;
    pctx c = { 0, 0, 0, -1e9f, -1e9f, -1, 0, 0, 0 };
    aq_nl = 0;
    aq_collect = 1;
    float end = aq_paint(&c);
    aq_collect = 0;
    AQ.h = tk_maxf(0, end + AQ_PAD - AQ.top);
    for (int k = 0; k < AQ_NLIVE; k++) aql_it[k] = -1;    /* the items again: every live layer drawn again */
}
static float aq_max(void) { return tk_maxf(0, AQ.top + AQ.h - AQ.bot); }
/* chunk c into layer L: exactly its px rows (the next chunk's start where this one ends: no seam) */
static void aq_chunk_draw(int L, int c)
{
    tk_layer *l = &aqc[L];
    relayer(l);
    l->hidden = 1;
    AQ.ck[L] = c;
    AQ.dirty[L] = 0;
    for (int k = 0; k < 5; k++) AQ.pk[L][k] = 0;
    float y0 = AQ.top + AQ_CH * c;
    if (c < 0 || y0 >= AQ.top + AQ.h) return;
    int Y0 = (int)tk_floorf(PT(y0)), Y1 = (int)tk_floorf(PT(y0 + AQ_CH));
    tk_layer_init(l, "store-page");
    if (!tk_bmp_new(&l->bmp, TK_W, Y1 - Y0)) return;
    l->x = l->bx = 0;
    l->y = l->by = (float)Y0;
    dc d = { &l->bmp, 0, 0, (float)Y0 };
    pctx p = { &d, y0, y0 + AQ_CH, 0, 0, L, 0, 0, 0 };
    aq_paint(&p);
    trim_layer(l, 0, 0);
    l->hidden = 0;
}
/* a live item into layer k: a busy package's ring and status (its row, its card, the app page's head) */
static void aq_live_draw(int k, int it)
{
    tk_layer *l = &aql[k];
    relayer(l);
    l->hidden = 1;
    aql_it[k] = it;
    aql_dirty[k] = 0;
    if (it < 0 || it >= aq_nl || !LST || AQL[it].pkg >= aq_n()) return;
    const sh_lpkg_t *p = &LST->pkg[AQL[it].pkg];
    float x = AQL[it].x, y = AQL[it].y, w = AQL[it].w;
    char s[96];
    tk_col sc;
    if (!aq_status(p, s, sizeof s, &sc)) s[0] = 0;
    if (AQL[it].kind == LV_ROW) {
        dc d = mk(l, "store-live", x, y + 20, CONTENT_R - x, 40, 1, 0);
        d_text_box(&d, TK_IT400, 13, x, bl(TK_IT400, 13, y + 39), w, 0, s, 0, 0, sc, 0);
        aq_pill(&d, PILL_X, y + 23, p, 0);
    } else if (AQL[it].kind == LV_CARD) {
        dc d = mk(l, "store-live", x, y + 18, CONTENT_R - 20 - x, 42, 1, 0);
        d_text_box(&d, TK_IT400, 13, x, bl(TK_IT400, 13, y + 38), w, 0, s, 0, 0, W(.75f), 0);
        aq_pill(&d, CONTENT_R - 20 - PILL_W, y + 21, p, 1);
    } else {
        dc d = mk(l, "store-live", x, y - 2, w, 40, 1, 0);
        d_ring(&d, x + 16, y + 16, 14, 2.8f, 1, W(.18f), 0);
        d_ring(&d, x + 16, y + 16, 14, 2.8f, aq_frac(p), ACCENT(), 1);
        if (p->state != SH_PKG_REMOVING) d_rrect(&d, x + 11.5f, y + 11.5f, 9, 9, 2, ACCENT());
        d_text_box(&d, TK_IT500, 13, x + 42, bl(TK_IT500, 13, y + 1), w - 42, 0, s, 0, 0, W(.75f), 0);
        if (p->state != SH_PKG_REMOVING) d_text(&d, TK_IT600, 13, x + 42, bl(TK_IT600, 13, y + 18), "Cancel", 0, 0, ACCENT(), 0);
    }
    l->hidden = 0;
}
/* the chunks the page shows at its scroll: positioned, clipped, faded; up to max missing / old ones drawn (re-using
 * the ones out of view); returns how many are still to draw */
static int aq_place(int max)
{
    float area = AQ.bot - AQ.top, m = aq_max();
    float s = AQ.scroll;                                   /* (the rubber band may take it past the ends) */
    float sc = tk_clampf(s, 0, m);
    int c0 = (int)(sc / AQ_CH), c1 = (int)((sc + area) / AQ_CH);
    if (s < 0) c0 = 0;
    if (c1 > c0 + AQ_NCH - 1) c1 = c0 + AQ_NCH - 1;
    int left = 0, drew = 0;
    for (int c = c0; c <= c1 && AQ.top + AQ_CH * c < AQ.top + AQ.h; c++) {
        int have = -1, spare = -1;
        for (int L = 0; L < AQ_NCH; L++) {
            if (AQ.ck[L] == c) have = L;
            else if (spare < 0 && (AQ.ck[L] < c0 || AQ.ck[L] > c1)) spare = L;
        }
        int L = have >= 0 ? have : spare;
        if (L < 0 || (have >= 0 && !AQ.dirty[L])) continue;
        if (drew < max) { aq_chunk_draw(L, c); drew++; }
        else left++;
    }
    float off = 0, al = 1;
    if (AQ.rev < 1) {                                       /* the new page slides in (iOS's push: from the right) */
        float e = 1 - (1 - AQ.rev) * (1 - AQ.rev) * (1 - AQ.rev);
        off = (float)AQ.dir * PT(70) * (1 - e);
        al = AQ.dir ? e : 1;
    }
    for (int L = 0; L < AQ_NCH; L++) {
        tk_layer *l = &aqc[L];
        if (!l->bmp.px) continue;
        l->hidden = AQ.hide || AQ.ck[L] < c0 || AQ.ck[L] > c1;
        l->y = l->by - tk_floorf(PT(s) + .5f);
        l->x = l->bx + tk_floorf(off + .5f);
        l->alpha = al;
        l->clip = 1;
        l->clip_r = (tk_rect){ 0, PT(AQ.top), TK_W, PT(area) };
        l->fade_top = s > 1 ? PT(30) : 0;           /* iOS 26's soft edge under the status bar and the back circle */
        /* the bottom: over the keyboard a short fade; else iOS 26's scroll edge under the tab bar (the page fades
         * out behind the glass, nothing shows round its rounded ends) */
        l->fade_bot = AQ.bot < H_PT - 1 ? (AQ.top + AQ.h - s > AQ.bot + 1 ? PT(24) : 0) : PT(64);
        if (AQ.bot >= H_PT - 1) l->clip_r.h = PT(TB_Y + 16 - AQ.top);
    }
    /* the live layers: the visible busy items (drawn when new or moved on) */
    for (int k = 0; k < AQ_NLIVE; k++)
        if (aql_it[k] >= 0 && (aql_it[k] >= aq_nl || AQL[aql_it[k]].y + 60 < AQ.top + s || AQL[aql_it[k]].y > AQ.bot + s)) { aql_it[k] = -1; aql[k].hidden = 1; }
    for (int it = 0; it < aq_nl; it++) {
        if (AQL[it].y + 60 < AQ.top + s || AQL[it].y > AQ.bot + s) continue;
        int have = -1, spare = -1;
        for (int k = 0; k < AQ_NLIVE; k++) { if (aql_it[k] == it) have = k; else if (aql_it[k] < 0 && spare < 0) spare = k; }
        if (have >= 0 && !aql_dirty[have]) continue;
        if (have < 0 && spare < 0) continue;
        aq_live_draw(have >= 0 ? have : spare, it);
        drew++;
    }
    for (int k = 0; k < AQ_NLIVE; k++) {
        tk_layer *l = &aql[k];
        if (!l->bmp.px) continue;
        if (aql_it[k] < 0) { l->hidden = 1; continue; }
        l->hidden = AQ.hide;
        l->y = l->by - tk_floorf(PT(s) + .5f);
        l->x = l->bx + tk_floorf(off + .5f);
        l->alpha = al;
        l->clip = 1;
        l->clip_r = (tk_rect){ 0, PT(AQ.top), TK_W, PT(area) };
        if (AQ.bot >= H_PT - 1) l->clip_r.h = PT(TB_Y + 16 - AQ.top);
    }
    if (AQ.app || AQ.cat_on) { aqh.x = aqh.bx + tk_floorf(off + .5f); aqh.alpha = al; }
    if (drew) relink();
    AQ.left = left;
    return left;
}
/* the fixed head: the back circle on a pushed page; Search's title and field; an empty store's words */
static void aq_head(void)
{
    relayer(&aqh);
    aqh.hidden = 1;
    if (!is_app("store")) return;
    if (AQ.empty) {
        const char *t, *l;
        if (LX.state == SH_LINUX_FAILED) { t = "Linux Didn't Start"; l = LX.why[0] ? LX.why : "The big cores did not start."; }
        else if (LX.state != SH_LINUX_RUNNING) { t = "Starting Linux"; l = "The App Store opens when Linux is up, in a few seconds."; }
        else if (!LST) { t = "Reading the Catalog"; l = "The apps' list comes from the memory card."; }
        else { t = "No Apps"; l = LST->repo_ok ? "The catalog is empty." : "The App Store's catalog isn't on the memory card yet."; }
        dc d = mk(&aqh, "store-head", 0, SAFE_TOP, 390, TB_Y - SAFE_TOP, 0, 0);
        d_h1(&d, "App Store");
        d_empty(&d, CONTENT_TOP, TB_Y, t, l);
        trim_layer(&aqh, 0, 0);
        aqh.hidden = 0;
        return;
    }
    if (AQ.app || AQ.cat_on) {
        dc d = mk(&aqh, "store-nav", 0, SAFE_TOP - 20, 390, NAVBAR_H + 40, 0, 0);
        d_back(&d, ACCENT());
        trim_layer(&aqh, 0, 0);
        aqh.hidden = 0;
        return;
    }
    if (AQ.tab != AT_SEARCH) return;
    float fy = AQ_FIELD_Y, fw = AQ.kb ? CONTENT_W - 74 : CONTENT_W;
    dc d = mk(&aqh, "store-search", 0, SAFE_TOP, 390, fy + 46 - SAFE_TOP, 0, 0);
    tk_fill_rect_raw(d.b, 0, 0, d.b->w, d.b->h, 0xff0a0f1fu);   /* opaque: the results scroll under it */
    d_h1(&d, "Search");
    d_rrect(&d, MARGIN, fy, fw, 38, 19, C_FILL);
    d_ring(&d, MARGIN + 18, fy + 17.5f, 6, 1.8f, 1, C_LABEL2, 0);
    d_seg(&d, MARGIN + 22.5f, fy + 22, MARGIN + 26.5f, fy + 26, 2, C_LABEL2);
    float tx = MARGIN + 36;
    if (AQ.q[0]) {
        d_text_box(&d, TK_IT400, 17, tx, bl(TK_IT400, 17, fy + 8), fw - 50, 0, AQ.q, 0, 0, INK, 0);
        if (AQ.kb) d_rrect(&d, tx + tk_minf(tw(TK_IT400, 17, AQ.q, 0, 0), fw - 50) + 1, fy + 9, 2, 21, 1, ACCENT());
    } else {
        d_text(&d, TK_IT400, 17, tx, bl(TK_IT400, 17, fy + 8), "Apps, categories and more", 0, 0, W(.42f), 0);
        if (AQ.kb) d_rrect(&d, tx - 1, fy + 9, 2, 21, 1, ACCENT());
    }
    if (AQ.kb) d_text_box(&d, TK_IT400, 17, CONTENT_R, bl(TK_IT400, 17, fy + 8), 70, 2, "Cancel", 0, 0, ACCENT(), 0);
    aqh.hidden = 0;
}
/* the tab bar's glass (baked once per opening, from the open's build: never in a tap) */
static void aq_tb_bake(void)
{
    if (aq_tb_baked && aqtb.bmp.px) return;
    relayer(&aqtb);
    dc d = mk(&aqtb, "store-tabbar", 0, TB_Y - 20, 390, TB_H + 40, 0, 0);
    d_lg_flat(&d, MARGIN, TB_Y, TB_W, TB_H, TB_H / 2, 1);
    d_lg_flat(&d, CONTENT_R - TB_SD, TB_Y, TB_SD, TB_SD, TB_SD / 2, 1);
    trim_layer(&aqtb, 0, 0);
    aq_tb_baked = 1;
}
/* the tab bar's glyphs and labels, the selected tab's lighter pill, the Updates badge (redrawn on a change) */
static void aq_tabs(void)
{
    relayer(&aqtg);
    aqtg.hidden = aqtb.hidden = 1;
    if (!is_app("store") || (AQ.kb && AQ.tab == AT_SEARCH && !AQ.app)) return;
    aqtb.hidden = 0;
    dc d = mk(&aqtg, "store-tabs", MARGIN, TB_Y, CONTENT_W, TB_H, 2, 0);
    static const char *const TL[3] = { "Today", "Browse", "Updates" };
    for (int t = 0; t < 3; t++) {
        float bx = MARGIN + TB_TABW * t, cx = bx + TB_TABW / 2, cy = TB_Y + 22;
        int on = AQ.tab == t;
        tk_col c = on ? ACCENT() : W(.75f);
        if (on) d_rrect(&d, bx + 5, TB_Y + 5, TB_TABW - 10, TB_H - 10, (TB_H - 10) / 2, W(.1f));
        if (t == 0) {
            d_rrect_stroke(&d, cx - 9, cy - 11, 18, 22, 4, 1.8f, c);
            d_rrect(&d, cx - 5, cy - 7, 10, 6, 1.5f, c);
            d_seg(&d, cx - 5, cy + 3, cx + 5, cy + 3, 1.6f, c);
            d_seg(&d, cx - 5, cy + 7, cx + 2, cy + 7, 1.6f, c);
        } else if (t == 1) {
            for (int q = 0; q < 4; q++) d_rrect(&d, cx - 10 + 11 * (q % 2), cy - 10 + 11 * (q / 2), 9, 9, 2.5f, c);
        } else {
            d_ring(&d, cx, cy, 10.5f, 1.8f, 1, c, 0);
            d_seg(&d, cx, cy - 5.5f, cx, cy + 5, 1.9f, c);
            d_seg(&d, cx - 4.5f, cy + .5f, cx, cy + 5, 1.9f, c);
            d_seg(&d, cx + 4.5f, cy + .5f, cx, cy + 5, 1.9f, c);
            if (AQ.badge > 0) {
                char nb[8];
                aq_dec(nb, tk_mini(AQ.badge, 99));
                float bw = tk_maxf(19, tw(TK_IT600, 12, nb, 0, 0) + 10);
                d_rrect(&d, cx + 6, cy - 16, bw, 19, 9.5f, C_RED);
                d_text_box(&d, TK_IT600, 12, cx + 6 + bw / 2, bl(TK_IT600, 12, cy - 13.5f), bw, 1, nb, 0, TK_TNUM, W(1), 0);
            }
        }
        d_text_box(&d, TK_IT600, 10.5f, cx, bl(TK_IT600, 10.5f, TB_Y + 39), TB_TABW - 8, 1, TL[t], 0, 0, c, 0);
    }
    float sx = CONTENT_R - TB_SD / 2, sy = TB_Y + TB_SD / 2;
    tk_col sc = AQ.tab == AT_SEARCH ? ACCENT() : W(.85f);
    d_ring(&d, sx - 2, sy - 2, 8, 2.2f, 1, sc, 0);
    d_seg(&d, sx + 3.8f, sy + 3.8f, sx + 8.5f, sy + 8.5f, 2.6f, sc);
    trim_layer(&aqtg, 0, 0);
    aqtg.hidden = 0;
}
/* what the open page shows (structure, not progress): a change redraws the page in place */
static uint32_t aq_sig(void)
{
    uint32_t h = 2166136261u;
    if (!LST) return 1;
    h = aq_hu(h, (uint32_t)LST->n); h = aq_hu(h, LST->checked_at); h = aq_hu(h, LST->net_ok);
    h = aq_hu(h, (uint32_t)LX.state); h = aq_hu(h, (uint32_t)LCAT.n);
    for (int i = 0; i < aq_n(); i++) {
        const sh_lpkg_t *p = &LST->pkg[i];
        h = aq_hu(h, p->flags); h = aq_hu(h, p->state); h = aq_hu(h, p->method); h = aq_hu(h, p->methods);
        h = aq_hu(h, (uint32_t)(p->state == SH_PKG_INSTALLING ? 0 : p->phase));
        h = aq_hash(h, p->ver_inst); h = aq_hash(h, p->ver_new);
    }
    return h;
}
static uint32_t aq_psig(const sh_lpkg_t *p) { return aq_hu(aq_hu(aq_hu(aq_hu(2166136261u, p->pct), p->done_kb / 100), p->eta_s / 30), p->phase); }

/* ---- pages: going to one, the open's build, the platform's changes ---- */
static void aq_redraw_all(void) { for (int L = 0; L < AQ_NCH; L++) if (AQ.ck[L] >= 0) AQ.dirty[L] = 1; }
static void aq_flush(void)
{
    aq_place(AQ_NCH * 2);
    AQ.hide = 0;
    AQ.rev = 1;
    aq_place(0);
}
/* a new page: its chunks hidden and drawn over the next frames, then slid in from dir (0: at once) */
static void aq_go(int dir)
{
    AQ.rm_arm = 0;
    AQ.v = 0;
    for (int L = 0; L < AQ_NCH; L++) { relayer(&aqc[L]); aqc[L].hidden = 1; AQ.ck[L] = -1; AQ.dirty[L] = 0; }
    for (int k = 0; k < AQ_NLIVE; k++) { relayer(&aql[k]); aql[k].hidden = 1; aql_it[k] = -1; }
    aq_area();
    AQ.scroll = tk_clampf(AQ.scroll, 0, aq_max());
    AQ.hide = 1;
    AQ.dir = dir;
    AQ.rev = dir ? 0 : 1;
    AQ.sig = aq_sig();
    lx_build_kb(AQ.kb && AQ.tab == AT_SEARCH && !AQ.app, AQ.shift, AQ.num);
    aq_head();
    aq_tabs();
    relink();
}
static void st_build(void)
{
    if (!is_app("store")) return;
    AQ.empty = !LST || LST->n <= 0;
    if (AQ.app && st_find(AQ.pkg) < 0) AQ.app = 0;
    AQ.badge = AQ.empty ? 0 : aq_badge();
    aq_tb_bake();
    aq_go(0);
    if (ap_pending) { AQ.hide = 0; aq_place(1); }   /* the open's build: the top chunk now, the rest over frames */
    else aq_flush();                             /* (a still, a rebuild: every chunk now) */
    for (int i = 0; i < aq_n(); i++) AQ.psig[i] = aq_psig(&LST->pkg[i]);
}
static void st_changed(void)
{
    if (!is_app("store") || ap_pending) return;
    int empty = !LST || LST->n <= 0;
    if (empty != AQ.empty) { st_build(); return; }
    if (empty) { aq_head(); relink(); return; }
    int b = aq_badge();
    if (b != AQ.badge) { AQ.badge = b; aq_tabs(); relink(); }
    uint32_t s = aq_sig();
    if (s != AQ.sig) {                            /* what the page shows changed: in place, over the next frames */
        AQ.sig = s;
        if (AQ.app && st_find(AQ.pkg) < 0) { AQ.app = 0; aq_go(0); }
        else {
            float old = AQ.h;
            aq_area();
            if (AQ.h != old) AQ.scroll = tk_clampf(AQ.scroll, 0, aq_max());
            aq_redraw_all();
        }
    }
    for (int i = 0; i < aq_n(); i++) {            /* progress: only the live layers that show the package */
        uint32_t q = aq_psig(&LST->pkg[i]);
        if (q == AQ.psig[i]) continue;
        AQ.psig[i] = q;
        for (int k = 0; k < AQ_NLIVE; k++) if (aql_it[k] >= 0 && aql_it[k] < aq_nl && AQL[aql_it[k]].pkg == i) aql_dirty[k] = 1;
    }
}
void sh_linux_store(const sh_lstore_t *s)
{
    LST = s && s->n >= 0 ? s : 0;
    if (lx_baked && is_app("store") && !ap_pending) st_changed();
}
/* an app's page pushed (the method preselected: the installed one, else the store's default) */
static void aq_push(int i)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    if (!AQ.app) { if (AQ.cat_on && AQ.tab == AT_BROWSE) AQ.scat = AQ.scroll; else AQ.sroot[AQ.tab] = AQ.scroll; }
    AQ.app = 1;
    lx_scopy(AQ.pkg, p->pkg, 40);
    AQ.pick = aq_inst(p) && p->method ? (int)p->method : p->def_method && (p->methods >> p->def_method & 1) ? (int)p->def_method : 0;
    if (!AQ.pick && p->methods) for (unsigned m = 1; m <= 3 && !AQ.pick; m++) if (p->methods >> m & 1) AQ.pick = (int)m;
    AQ.scroll = 0;
    AQ.kb = 0;
    aq_go(1);
}
static void aq_back(void)
{
    if (AQ.app) { AQ.app = 0; tk_bmp_free(&aq_dic); aq_dic_pkg[0] = 0; AQ.scroll = AQ.cat_on && AQ.tab == AT_BROWSE ? AQ.scat : AQ.sroot[AQ.tab]; }
    else if (AQ.cat_on) { AQ.cat_on = 0; AQ.scroll = AQ.sroot[AQ.tab]; }
    aq_go(-1);
}
static void aq_tab(int t)
{
    if (t == AQ.tab) {                            /* the open tab again: back to its root, or to its top */
        if (AQ.app || AQ.cat_on) { AQ.app = 0; AQ.cat_on = 0; AQ.scroll = 0; aq_go(-1); }
        else if (AQ.scroll > 0) { AQ.scroll = 0; AQ.v = 0; aq_place(0); }
        return;
    }
    if (!AQ.app && !(AQ.cat_on && AQ.tab == AT_BROWSE)) AQ.sroot[AQ.tab] = AQ.scroll;
    AQ.tab = t;
    AQ.app = 0;
    AQ.cat_on = t == AT_BROWSE && AQ.cat_on;
    if (t != AT_BROWSE) AQ.cat_on = 0;
    AQ.kb = 0;
    AQ.scroll = AQ.sroot[t];
    tk_bmp_free(&aq_dic);
    aq_dic_pkg[0] = 0;
    aq_go(0);
}

/* ---- actions ---- */
static int aq_op(int i, int op, int m)
{
    if (sh_linux_pkg(i, SH_PKG_OPWORD(op, m)) < 0) { toast("Linux isn't running: try again in a moment"); return -1; }
    return 0;
}
/* GET / OPEN / UPDATE / RETRY / cancel; m: the method (0: the store's default) */
static void aq_act(int i, int m)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    if (p->state == SH_PKG_REMOVING) return;
    if (st_busy(p)) { (void)aq_op(i, SH_PKG_OP_CANCEL, 0); return; }
    if (aq_upd(p)) { (void)aq_op(i, SH_PKG_OP_UPDATE, 0); return; }
    if (aq_inst(p)) {
        int a = st_app(i);
        if (a >= 0) lx_open_app(a);
        else toast("Installed. It has no window of its own");
        return;
    }
    if ((p->flags & SH_PKG_NEEDS_NET) && (!m || m == (int)p->def_method)) { toast("Needs Wi-Fi: turn it on in Settings"); return; }
    if (p->flags & SH_PKG_NO_SPACE) { toast("Not enough space on the memory card"); return; }
    if (aq_op(i, SH_PKG_OP_INSTALL, m) < 0) return;
    if (p->flags & SH_PKG_TOO_BIG) toast("It needs a lot of memory: it may run slowly");
}
static int aq_rm_pkg = -1;                       /* the package the Remove sheet is about */
static void aq_rm_cb(int b)
{
    if (b == 0 && aq_rm_pkg >= 0 && aq_rm_pkg < aq_n()) (void)aq_op(aq_rm_pkg, SH_PKG_OP_REMOVE, 0);
    aq_rm_pkg = -1;
}
static void aq_tap(float x, float y)
{
    if (!is_app("store")) return;
    int search = AQ.tab == AT_SEARCH && !AQ.app, kbup = AQ.kb && search;
    /* the tab bar (not while the keyboard is up) */
    if (!kbup && y >= TB_Y - 4 && y < TB_Y + TB_H + 6) {
        if (x >= CONTENT_R - TB_SD - 4) { aq_tab(AT_SEARCH); return; }
        if (x >= MARGIN && x < MARGIN + TB_W) { aq_tab(tk_mini(2, (int)((x - MARGIN) / TB_TABW))); return; }
        return;
    }
    if (AQ.empty || AQ.hide) return;              /* (a page still being drawn: nothing to tap yet) */
    if ((AQ.app || AQ.cat_on) && BACK_HIT(x, y)) { aq_back(); return; }
    if (search && y >= AQ_FIELD_Y - 6 && y < AQ_FIELD_Y + 44) {   /* the field: the keyboard; Cancel: all again */
        if (AQ.kb && x >= CONTENT_R - 72) { AQ.kb = 0; AQ.q[0] = 0; AQ.scroll = 0; aq_go(0); return; }
        if (!AQ.kb) { AQ.kb = 1; AQ.shift = AQ.num = 0; aq_area(); lx_build_kb(1, 0, 0); aq_head(); aq_tabs(); aq_place(0); relink(); }
        return;
    }
    if (kbup && y >= 548) {
        char ch = 0;
        int k = kb_hit(x, y, AQ.shift, AQ.num, &ch), n = (int)tk_strlen(AQ.q);
        if (!k) return;                           /* B3: shift / 123 are the keyboard's own (and its haptic on touch down) */
        if (k == 2) { AQ.shift = !AQ.shift; lx_build_kb(1, AQ.shift, AQ.num); relink(); return; }
        if (k == 3) { AQ.num = !AQ.num; lx_build_kb(1, AQ.shift, AQ.num); relink(); return; }
        if (k == 5) { AQ.kb = 0; aq_area(); lx_build_kb(0, 0, 0); aq_head(); aq_tabs(); aq_redraw_all(); aq_place(0); relink(); return; }
        if (k == 4 && n) AQ.q[n - 1] = 0;
        else if (k == 1 && n < 22 && !(ch == ' ' && !n)) {
            if (AQ.shift) { AQ.shift = 0; lx_build_kb(1, 0, AQ.num); }
            AQ.q[n] = ch; AQ.q[n + 1] = 0;
        } else return;
        AQ.scroll = 0;                            /* the results again (in place, over the next frames) */
        aq_area();
        aq_head();
        aq_redraw_all();
        aq_place(0);
        relink();
        return;
    }
    if (y < AQ.top || y >= AQ.bot) return;
    pctx c = { 0, 0, 0, x, y + AQ.scroll, -1, 0, -1, 0 };
    aq_paint(&c);
    int i = c.hpkg;
    switch (c.hit) {
    case H_APP: sh_haptic(); aq_push(i); break;
    case H_ACT: aq_act(i, 0); break;
    case H_OPEN: { int a = st_app(i); if (a >= 0) lx_open_app(a); } break;
    case H_MAIN: aq_act(i, AQ.pick); break;
    case H_CAT:
        AQ.sroot[AQ.tab] = AQ.scroll;
        if (AQ.tab != AT_BROWSE) { AQ.tab = AT_BROWSE; aq_tabs(); }
        AQ.cat_on = 1;
        lx_scopy(AQ.cat, c.harg >= 10 ? "" : AQ_CATS[c.harg], 16);
        AQ.scroll = 0;
        aq_go(1);
        break;
    case H_CHECK:
        if (sh_linux_pkg(SH_PKG_ALL, SH_PKG_OPWORD(SH_PKG_OP_CHECK, 0)) < 0) toast("Linux isn't running: try again in a moment");
        else toast("Checking for updates");
        break;
    case H_UPALL:
        for (int k = 0; k < aq_n(); k++) if (aq_upd(&LST->pkg[k]) && !st_busy(&LST->pkg[k])) if (aq_op(k, SH_PKG_OP_UPDATE, 0) < 0) break;
        break;
    case H_ROLLBACK: (void)aq_op(i, SH_PKG_OP_ROLLBACK, 0); break;
    case H_METHOD:
        if (AQ.pick != c.harg) { AQ.pick = c.harg; sh_haptic(); aq_area(); aq_redraw_all(); aq_place(0); }
        break;
    case H_REINSTALL: (void)aq_op(i, SH_PKG_OP_INSTALL, AQ.pick); break;
    case H_REMOVE: {                              /* B2's action sheet: one red row, a tap outside cancels */
        static const kit_btn B[1] = { { "Remove App", KIT_DESTRUCTIVE } };
        static char t[64];
        AQS(t, "Remove ", LST->pkg[i].name, "?");
        aq_rm_pkg = i;
        if (kit_actions(t, B, 1, aq_rm_cb) < 0) aq_rm_pkg = -1;
    } break;
    case H_SUGGEST: lx_scopy(AQ.q, AQ_SUGG[c.harg], 24); AQ.scroll = 0; aq_area(); aq_head(); aq_redraw_all(); aq_place(0); relink(); break;
    default: break;
    }
}
/* the finger on the page: a drag scrolls it (a rubber band past the ends, momentum after), a tap is aq_tap */
static int aq_touch(int ph, float x, float y)
{
    if (ph == 0) {
        AQ.drag = 1; AQ.moved = 0; AQ.y0 = y; AQ.s0 = AQ.scroll; AQ.v = 0; AQ.ly = y; AQ.lt = now_ms;
        return 1;
    }
    if (!AQ.drag) return 0;
    if (tk_fabsf(y - AQ.y0) > 6) AQ.moved = AQ.moved ? AQ.moved : 1 + (AQ.y0 >= AQ.top && AQ.y0 < AQ.bot && AQ.y0 < TB_Y);
    if (ph == 1 && AQ.moved == 2) {          /* (1: moved off a tap, not on the page: no scroll) */
        float s = AQ.s0 - (y - AQ.y0), m = aq_max();
        if (s < 0) s = s * .4f;                   /* the rubber band */
        else if (s > m) s = m + (s - m) * .4f;
        float dt = now_ms - AQ.lt;
        if (dt > 0) AQ.v = .6f * AQ.v + .4f * (-(y - AQ.ly) / dt * 1000);
        AQ.ly = y; AQ.lt = now_ms;
        AQ.scroll = s;
        aq_place(1);
    }
    if (ph == 2) {
        AQ.drag = 0;
        if (!AQ.moved) { AQ.v = 0; aq_tap(x, y); }
        else if (AQ.moved != 2 || now_ms - AQ.lt > 80) AQ.v = 0;   /* held still before letting go: no flick */
    }
    return 1;
}
static int aq_moving(void)
{
    if (!is_app("store")) return 0;
    return AQ.drag || AQ.hide || AQ.left > 0 || AQ.rev < 1 || tk_fabsf(AQ.v) > 1 || AQ.scroll < 0 || AQ.scroll > aq_max() + .5f;
}
static int aq_tick(float dt)
{
    if (!is_app("store") || ap_pending || AQ.empty) return 0;
    int pend = 0;
    if (!AQ.drag) {
        float m = aq_max();
        if (tk_fabsf(AQ.v) > 1) {                 /* momentum: iOS's 0.998 a ms */
            AQ.scroll += AQ.v * dt;
            AQ.v *= tk_expf(dt * 1000 * -.002f);
            if (AQ.scroll < -40 || AQ.scroll > m + 40) AQ.v = 0;
            if (tk_fabsf(AQ.v) < 20) AQ.v = 0;
        }
        if (tk_fabsf(AQ.v) <= 1 && (AQ.scroll < 0 || AQ.scroll > m)) {   /* the rubber band springs back */
            float to = AQ.scroll < 0 ? 0 : m, k = 1 - tk_expf(-dt * 14);
            AQ.scroll += (to - AQ.scroll) * k;
            if (tk_fabsf(AQ.scroll - to) < .4f) AQ.scroll = to;
        }
    }
    if (AQ.hide) {                                /* a new page: two chunks a frame, then it shows */
        pend = aq_place(3);
        if (!pend) { AQ.hide = 0; AQ.rev = AQ.dir ? 0 : 1; aq_place(0); }
    } else {
        if (AQ.rev < 1) AQ.rev = tk_minf(1, AQ.rev + dt / .32f);
        pend = aq_place(1);                       /* (a chunk is the frame's work: ~20 ms of text on the Mac) */
    }
    return pend > 0 || aq_moving();
}
static void aq_open(void)
{
    tk_memset(&AQ, 0, sizeof AQ);
    for (int L = 0; L < AQ_NCH; L++) AQ.ck[L] = -1;
    AQ.rev = 1;
    AQ.top = SAFE_TOP; AQ.bot = H_PT;
    aq_tb_baked = 0;
    (void)sh_linux_hold(1);                       /* Linux started for the catalog, kept while open */
}
static void aq_close(void)
{
    for (int L = 0; L < AQ_NCH; L++) { relayer(&aqc[L]); aqc[L].hidden = 1; aqc[L].clip = 0; AQ.ck[L] = -1; }
    for (int k = 0; k < AQ_NLIVE; k++) { relayer(&aql[k]); aql[k].hidden = 1; aql[k].clip = 0; aql_it[k] = -1; }
    relayer(&aqh); relayer(&aqtb); relayer(&aqtg);
    aqh.hidden = aqtb.hidden = aqtg.hidden = 1;
    aq_tb_baked = 0;
    AQ.kb = AQ.drag = AQ.hide = 0;
    AQ.v = 0;
    AQ.rev = 1;
    aq_icons_free();
}
static int aq_layers(tk_layer **o)
{
    int n = 0;
    for (int L = 0; L < AQ_NCH; L++) o[n++] = &aqc[L];
    for (int k = 0; k < AQ_NLIVE; k++) o[n++] = &aql[k];
    o[n++] = &aqh;
    o[n++] = &aqtb;
    o[n++] = &aqtg;
    return n;
}
/* the stills: "store=" (Today) / today / browse / search / updates / a package's id (its page); "storecat=NAME"
 * (Browse > NAME, "All" every app); "storeq=TEXT:v" (Search with TEXT typed, v 1 the keyboard up); "storeapp=PKG";
 * "storescroll=:PT" (the open page scrolled); "storepick=:M" (the app page's method picked, 1 distro, 2 release,
 * 3 source) */
static void aq_still(void) { aq_go(0); aq_flush(); }
static int aq_set(const char *what, const char *key, float v)
{
    int st = !tk_strcmp(what, "store"), cat = !tk_strcmp(what, "storecat"), q = !tk_strcmp(what, "storeq"), ap = !tk_strcmp(what, "storeapp");
    int sc = !tk_strcmp(what, "storescroll"), pk = !tk_strcmp(what, "storepick");
    if (!st && !cat && !q && !ap && !sc && !pk) return 0;
    if (st || ((cat || ap || q) && !is_app("store"))) sh_set("app", "store", st ? v : 1);
    if (!is_app("store")) return 1;
    if (st) {
        static const char *const TN[4] = { "today", "browse", "updates", "search" };
        for (int t = 0; t < 4; t++) if (!tk_strcmp(key, TN[t])) { AQ.tab = t; AQ.app = AQ.cat_on = 0; AQ.scroll = 0; aq_still(); return 1; }
        if (key[0] && st_find(key) >= 0) { aq_push(st_find(key)); aq_flush(); }
        return 1;
    }
    if (cat) {
        AQ.tab = AT_BROWSE; AQ.app = 0; AQ.cat_on = 1; AQ.scroll = 0;
        lx_scopy(AQ.cat, tk_strcmp(key, "All") ? key : "", 16);
        aq_tabs();
        aq_still();
        return 1;
    }
    if (q) {
        AQ.tab = AT_SEARCH; AQ.app = 0; AQ.scroll = 0;
        lx_scopy(AQ.q, key, 23);
        AQ.kb = v > 0;
        aq_still();
        return 1;
    }
    if (ap) { if (st_find(key) >= 0) { aq_push(st_find(key)); aq_flush(); } return 1; }
    if (sc) { aq_area(); AQ.scroll = tk_clampf(v, 0, aq_max()); aq_redraw_all(); aq_flush(); return 1; }
    if (pk && AQ.app) { AQ.pick = (int)v; aq_area(); aq_redraw_all(); aq_flush(); return 1; }
    return 1;
}
