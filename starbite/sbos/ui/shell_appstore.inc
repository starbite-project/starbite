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
#define TB_SD 62.0f                              /* (no separate circle since A1.5: Search is the capsule's 4th slot) */
#define TB_X 18.0f                               /* spec 2.1.1: one capsule x 18, w 354, h 62, r 31; 4 slots of 86.5 */
#define TB_W 354.0f
#define TB_TABW ((TB_W - 8) / 4)
#define RH 76.0f                                 /* an app's row: the 48 pt icon, two lines, the pill */
#define PILL_W 74.0f
#define PILL_X (CONTENT_R - PILL_W)
#define AQ_FIELD_Y 138.0f                        /* spec 2.5.1: the field under Search's large title, y 138..182 */
#define AQ_FIELD_FY 44.0f                        /* focused: in the nav row, y 44..88, with the x circle beside it */
#define AQ_FY() (AQ.kb ? AQ_FIELD_FY : AQ_FIELD_Y)
#define AQ_PAD 110.0f                            /* under a page's last row: the tab bar's room */
enum { AT_TODAY, AT_GAMES, AT_APPS, AT_SEARCH };   /* iOS 27's App Store: Today / Games / Apps + the search circle */
#define AQ_ACCT "@account"                       /* AQ.cat of the Account page (pushed from the account button) */
enum { H_NONE, H_APP, H_ACT, H_CAT, H_CHECK, H_UPALL, H_ROLLBACK, H_METHOD, H_MAIN, H_OPEN, H_REINSTALL, H_REMOVE, H_SUGGEST, H_OPTS, H_ACCT, H_MORE, H_STORY, H_TERM, H_RECENT, H_RECCLR };
static const char *const AQ_CATS[10] = { "Internet", "Office", "Graphics", "Media", "Games", "Utilities", "Development",
                                         "Education", "Science", "System" };
static const uint32_t AQ_CATC[11] = { 0x0a84ff, 0xff9f0a, 0xff375f, 0xff453a, 0xbf5af2, 0x8e8e93, 0x5e5ce6, 0x30d158,
                                      0x32ade6, 0x636366, 0x2c7be5 };   /* [10]: All Apps */
static const char *const AQ_SUGG[6] = { "Games", "Text editor", "Graphics", "Office", "Terminal", "Media" };
static struct {
    int tab, cat_on, app, pick, kb, shift, num, rm_arm, badge, empty, opts;   /* opts: Install Options expanded */
    int more;                                     /* the product page's description opened ("more") */
    int story;                                    /* a Today story open: its package + 1 (0: none) */
    int sub;                                      /* Search: the query submitted (results), else suggestions while typing */
    float px, pv;                                 /* the page's slide (pt) and its speed: push / pop / the edge swipe */
    int edge;                                     /* a back swipe from the left edge is following the finger */
    float ex0;
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
static tk_layer aqc[AQ_NCH], aqh, aqtb, aqtg;
/* the cold start (Linux not up yet): a determinate bar under the words, two solid layers (no drawing per frame), paced
 * by the last start this boot (the phone's first: ~35 s to HELLO + the store's table, 7u's self test) */
static tk_layer aqbt, aqbf;
static tk_layer aqit;
static tk_layer aqtp;
/* the product page's screenshots (spec 2.3.4): Track E's captures on card p1 /starbite/linux/shots/<id>-<n>.jpg
 * (720 x 1280 baseline JPEG), decoded one after another through Photos' decoder (dj_*) at the shown size into their
 * own layers (a sideways swipe moves layers, nothing is drawn again); at most three, freed when the page goes */
#define SS_DIR "/starbite/linux/shots"
#define SS_H 235.0f
#define SS_W (SS_H * 9 / 16)
#define SS_GAP 9.0f
static tk_layer aqss[3];
static struct { char pkg[40]; int n, k, active, loading, hdrag; char file[3][64]; uint64_t size[3]; float y, x, v, to, x0, sx0; } SS = { "", 0, 0, 0, -1, 0, { "", "", "" }, { 0, 0, 0 }, 0, 0, 0, 0, 0, 0 };
static struct { char n[48]; uint64_t sz; } SSD[160];
static int ss_nd = -1;                            /* the tab bar's selected pill: a solid that slides (spec 2.1.1) */
static struct { float x, v, to; int set; } TP;
/* the story's zoom (iOS: the Today card grows into the story, matched geometry, spring response 0.45 damping 0.85;
 * the story shrinks back into its card on close or a drag down): aqz the card's face redrawn once at card size and
 * scaled, aqzb the black page growing with it; phase 1 opening, 2 open (aqz until the story's chunks are drawn), 3 closing */
static tk_layer aqz, aqzb;
static struct { int phase, pkg; float t, v; tk_rect card; float sroot, drag, dv; } ZS;
static void aq_story_open(int i, tk_rect card);
static void aq_story_close(void);                            /* the inline title (17 / 600, centred) a root page's large title collapses into */
static float aq_st0, aq_st_exp = 35000, aq_bar_y;
static int aq_st_late;    /* the chunks, the fixed head, the tab bar's glass and its glyphs */
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
/* the methods in plain words (cp7z, the user's first minute: no "release / source / distro"): the card's title, the
 * segment's label, one line on what it means */
static const char *aq_mname(unsigned m) { return m == SH_M_RELEASE ? "Newest from the developer" : m == SH_M_SOURCE ? "Build on this phone" : m == SH_M_DISTRO ? "From Debian" : "Default"; }
static const char *aq_mseg(unsigned m) { return m == SH_M_RELEASE ? "Newest" : m == SH_M_SOURCE ? "Build" : "Debian"; }
static const char *aq_mwhy(unsigned m)
{
    return m == SH_M_RELEASE ? "The developer's own build of the newest version, checked before it installs." :
           m == SH_M_SOURCE ? "Compiled on this phone from the developer's code: takes a while, needs Wi-Fi." :
                              "Debian's package: tested with the rest of Linux, sometimes an older version.";
}
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
        char t[96];
        aq_dec(y, (int)(p->pct > 99 ? 99 : p->pct));      /* cp6: the step moves visibly (the engine's overall %) */
        if (!p->eta_s) AQS(t, "Building ", y, " %");
        else if (p->eta_s < 60) AQS(t, "Building ", y, " % \xc2\xb7 <1 min");
        else { aq_dec(x, (int)((p->eta_s + 59) / 60)); AQS(t, "Building ", y, " % \xc2\xb7 ", x, " min left"); }
        lx_scopy(b, t, cap);
    } else if (p->pct) { char t[96]; aq_dec(y, (int)(p->pct > 99 ? 99 : p->pct)); AQS(t, "Installing ", y, " %"); lx_scopy(b, t, cap); }
    else lx_scopy(b, "Installing\xe2\x80\xa6", cap);
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
typedef struct { dc *d; float y0, y1, hx, hy; int L, hit, hpkg, harg; tk_rect hr; float hrad; int hbtn; } pctx;
static int vis(const pctx *c, float y, float h) { return c->d && y < c->y1 && y + h > c->y0; }
static int at(const pctx *c, float x, float y, float w, float h) { return !c->d && c->hx >= x && c->hx < x + w && c->hy >= y && c->hy < y + h; }
static void aq_mark(pctx *c, int i) { if (c->L >= 0 && i >= 0 && i < SH_LPKGS) AQ.pk[c->L][i >> 5] |= 1u << (i & 31); }
static void aq_hit(pctx *c, int what, int pkg, int arg) { if (!c->hit) { c->hit = what; c->hpkg = pkg; c->harg = arg; } }
/* aq_hit plus the lit rect for the pressed state (cp6: aq_press; page coordinates, rad, btn: a round button) */
static void aq_hitr(pctx *c, int what, int pkg, int arg, float x, float y, float w, float h, float rad, int btn)
{
    if (c->hit) return;
    aq_hit(c, what, pkg, arg);
    c->hr = (tk_rect){ x, y, w, h }; c->hrad = rad; c->hbtn = btn;
}
static void aq_live_add(int kind, int pkg, float x, float y, float w)
{
    if (!aq_collect || aq_nl >= 32) return;
    AQL[aq_nl].kind = kind; AQL[aq_nl].pkg = pkg; AQL[aq_nl].x = x; AQL[aq_nl].y = y; AQL[aq_nl].w = w;
    aq_nl++;
}
static void aq_blit(dc *d, const tk_bmp *b, float x, float y) { if (b && b->px) tk_blit(d->b, b, (int)tk_floorf(PT(x) - d->ox + .5f), (int)tk_floorf(PT(y) - d->oy + .5f), 1); }
/* the row's button (PILL_W x 30): GET / OPEN / UPDATE / RETRY, or the progress ring with its stop square */
/* an arc from angle a0 (radians, 0 = 12 o'clock, clockwise) over sweep: the waiting spinner */
static void aq_darc(dc *d, float cx, float cy, float r, float sw, float a0, float sweep, tk_col c)
{
    int bx = (int)tk_floorf(PT(cx - r - sw) - d->ox) - 2, by = (int)tk_floorf(PT(cy - r - sw) - d->oy) - 2;
    int n = (int)PT(2 * (r + sw)) + 5;
    tk_fm m;
    tk_fm_new(&m, n, n);
    tk_fm_arc(&m, d->ox + bx, d->oy + by, PT(cx), PT(cy), PT(r), PT(sw), a0, sweep, 1);
    tk_paint pp = tk_solid(c);
    tk_draw_fm(d->b, &m, bx, by, &pp, 1);
    tk_fm_free(&m);
}
/* iOS's states after GET: "waiting" (an indeterminate spinner: queued, or started with nothing measurable yet), then
 * the progress ring with its stop square (tap = cancel), then OPEN */
static int aq_waiting(const sh_lpkg_t *p)
{
    if (p->state == SH_PKG_QUEUED) return 1;
    if (p->state != SH_PKG_INSTALLING) return 0;
    if (p->phase == SH_PKG_DOWNLOADING && p->total_kb) return 0;
    return p->pct < 2;
}
static float aq_spin_a(void) { return (float)((long long)now_ms % 900) * 6.2831853f / 900.0f; }   /* one turn per 0.9 s (spec 2.2.3) */
static void aq_ring(dc *d, float cx, float cy, const sh_lpkg_t *p, int light)
{
    tk_col fg = light ? W(1) : ACCENT();
    d_ring(d, cx, cy, 13, 2.6f, 1, W(.18f), 0);
    if (aq_waiting(p) && p->state != SH_PKG_REMOVING) { aq_darc(d, cx, cy, 13, 2.6f, aq_spin_a(), 1.7f, fg); return; }
    d_ring(d, cx, cy, 13, 2.6f, aq_frac(p), fg, 1);
    if (p->state != SH_PKG_REMOVING) d_rrect(d, cx - 4, cy - 4, 8, 8, 1.5f, fg);
}
static void aq_pill(dc *d, float x, float y, const sh_lpkg_t *p, int light)
{
    if (st_busy(p)) { aq_ring(d, x + PILL_W / 2, y + 15, p, light); return; }
    int inst = aq_inst(p), dim = !inst && (p->flags & SH_PKG_NO_SPACE);   /* (never for "the network looked down": GET tries) */
    /* the spec (appstore-ios27-spec 2.2.1): title case 15 / 600 systemBlue on tertiarySystemFill (white 24 % on a card) */
    const char *t = aq_upd(p) ? "Update" : inst ? "Open" : p->state == SH_PKG_FAILED ? "Retry" : "Get";
    d_rrect(d, x, y, PILL_W, 30, 15, light ? W(.24f) : tk_hex(0x767680, .24f));
    tk_col c = light ? W(1) : ACCENT();
    if (dim) c.a = .4f;
    d_text_box(d, TK_IT600, 15, x + PILL_W / 2, bl(TK_IT600, 15, y + 15 - 15 * .62f), PILL_W - 8, 1, t, 0, 0, c, 0);
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
        if (c->hx >= PILL_X - 10 && c->hy >= y + 12 && c->hy < y + 64) aq_hitr(c, H_ACT, i, 0, PILL_X, y + 23, PILL_W, 30, 15, 1);
        else if (rb && c->hy >= y + 52 && c->hx >= tx && c->hx < tx + 250) aq_hitr(c, H_ROLLBACK, i, 0, tx - 6, y + 54, 256, 24, 8, 1);
        else aq_hitr(c, H_APP, i, 0, 0, y, 390, h, 0, 0);
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
    if (link && at(c, CONTENT_R - 110, y, 110, 40)) aq_hitr(c, what, -1, arg, CONTENT_R - 110, y + 4, 116, 32, 10, 1);
    return y + 40;
}
static int aq_badge(void);
/* a root page's large title, the account button at its right (iOS: a 36 pt circle level with the title, the updates'
 * badge on it; it scrolls away with the title) */
#define ACCT_D 36.0f
#define ACCT_X (CONTENT_R - ACCT_D)
#define ACCT_Y (LARGE_BASE - 29)
static void aq_title(pctx *c, const char *t)
{
    if (vis(c, SAFE_TOP + NAVBAR_H, LARGE_H)) d_h1(c->d, t);
}
static void aq_title_acct(pctx *c, const char *t)
{
    if (vis(c, SAFE_TOP + NAVBAR_H, LARGE_H)) {
        dc *d = c->d;
        d_text_box(d, TK_IT700, 34, MARGIN, LARGE_BASE, CONTENT_W - ACCT_D - 12, 0, t, 0, 0, INK, 0);
        float cx = ACCT_X + ACCT_D / 2, cy = ACCT_Y + ACCT_D / 2;
        d_rrect(d, ACCT_X, ACCT_Y, ACCT_D, ACCT_D, ACCT_D / 2, tk_hex(0x8e8e93, 1));   /* (spec 2.1.3: systemGray disc) */
        d_rrect(d, cx - 5, cy - 10, 10, 10, 5, W(1));                 /* no owner's name kept: a person glyph */
        d_rrect(d, cx - 9.5f, cy + 2, 19, 10, 5, W(1));
        int b = aq_badge();
        if (b > 0) {                              /* an 18 pt red badge, centred at (right - 3, top + 3) */
            char nb[8];
            aq_dec(nb, tk_mini(b, 99));
            float bw = tk_maxf(18, tw(TK_IT600, 12, nb, 0, 0) + 9), bx = ACCT_X + ACCT_D - 3 - bw / 2, by = ACCT_Y + 3 - 9;
            d_rrect(d, bx, by, bw, 18, 9, tk_hex(0xff383c, 1));
            d_text_box(d, TK_IT600, 12, bx + bw / 2, bl(TK_IT600, 12, by + 2.5f), bw, 1, nb, 0, TK_TNUM, W(1), 0);
        }
    }
    if (at(c, ACCT_X - 6, ACCT_Y - 6, ACCT_D + 12, ACCT_D + 12)) aq_hitr(c, H_ACCT, -1, 0, ACCT_X, ACCT_Y, ACCT_D, ACCT_D, ACCT_D / 2, 1);
}

/* Today: the date and the large title, the featured cards, "Top Apps in" the two biggest categories */
#define CARD_H 440.0f                            /* spec 2.4.1: the Today card 350 x 440, r 24 */
#define CARD_R 24.0f
/* a story's art (ours, no screenshots yet): the app's icon as a staggered pattern over its colour, darkening to the
 * bottom so the words and the app row read; the card (x, y, w, h, r) and the story's hero use it */
static void aq_art(dc *d, int i, float x, float y, float w, float h, float r, float top)
{
    uint32_t col = aq_card_col(i);
    tk_col c0 = tk_hex(col, 1), c1 = tk_hex(col, 1);
    c0.r *= .78f; c0.g *= .78f; c0.b *= .78f;
    c1.r *= .30f; c1.g *= .30f; c1.b *= .30f;
    tk_stop s[2] = { { 0, c0 }, { 1, c1 } };
    tk_paint g = tk_linear(160, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), 2, s);
    tk_fill_rrect(d->b, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r), &g, 1);
    const tk_bmp *ic = aq_icon(i);
    if (ic && ic->px) {                           /* the pattern: 48 pt icons every 74 pt, rows offset by half a step */
        float st = 74, ox = x + 14, oy = top;
        for (int row = 0; oy + row * st < y + h - 110; row++)
            for (int k = -1; ox + k * st + (row & 1) * st / 2 < x + w - 10; k++) {
                float ix = ox + k * st + (row & 1) * st / 2, iy = oy + row * st;
                if (ix < x + 4 || iy + 48 > y + h - 96) continue;
                int bx = (int)tk_floorf(PT(ix) - d->ox + .5f), by = (int)tk_floorf(PT(iy) - d->oy + .5f);
                if (bx + ic->w < 0 || by + ic->h < 0 || bx > d->b->w || by > d->b->h) continue;
                tk_blit(d->b, ic, bx, by, row == 1 && k == 1 ? 1 : .32f);   /* (one of them at full strength) */
            }
    }
    tk_stop sh[2] = { { 0, tk_hex(0x000000, 0) }, { 1, tk_hex(0x000000, .55f) } };   /* the app row's band */
    tk_paint gb = tk_linear(180, PT(x) - d->ox, PT(y + h - 150) - d->oy, PT(w), PT(150), 2, sh);
    tk_fill_rrect(d->b, PT(x) - d->ox, PT(y + h - 150) - d->oy, PT(w), PT(150), 0, &gb, 1);
}
/* the card's face at (x, y) scaled by k (1: the card; the story's hero is the same face at 390 / 350): its art, the
 * label, the headline, the app row (with its pill when pill) */
static void aq_card_face(dc *d, int i, float x, float y, float k, int pill)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    float w = CONTENT_W * k, h = CARD_H * k;
    char ln[4][96];
    float wm;
    int n = wrapn(ln, 3, TK_IT700, 30 * k, p->summary[0] ? p->summary : p->name, w - 40 * k, &wm);
    aq_art(d, i, x, y, w, h, pill ? CARD_R : 0, y + (44 + 34.0f * n + 14) * k);
    char up[40];
    int q = 0;
    const char *lab = p->flags & SH_PKG_INSTALLED ? "ON YOUR PHONE" : !tk_strcmp(p->category, "Games") ? "GAME OF THE DAY" : "APP OF THE DAY";
    for (const char *z = lab; *z && q < 38; z++) up[q++] = *z;
    up[q] = 0;
    d_text(d, TK_IT600, 13 * k, x + 20 * k, bl(TK_IT600, 13 * k, y + 22 * k), up, .3f, 0, W(.75f), 0);
    for (int j = 0; j < n; j++) d_text(d, TK_IT700, 30 * k, x + 20 * k, bl(TK_IT700, 30 * k, y + (44 + 34.0f * j) * k), ln[j], 0, 0, W(1), 0);
    float by = y + h - 72 * k;
    aq_blit(d, aq_icon(i), x + 20 * k, by + 12 * k);
    float tx = x + 80 * k, tw_ = x + w - (20 + PILL_W + 10) * k - tx;
    d_text_box(d, TK_IT600, 16, tx, bl(TK_IT600, 16, by + 15 * k), tw_, 0, p->name, 0, 0, W(1), 0);
    if (!st_busy(p)) {
        char l2[96];
        tk_col lc;
        if (!aq_status(p, l2, sizeof l2, &lc)) lx_scopy(l2, p->category, sizeof l2);
        d_text_box(d, TK_IT400, 13, tx, bl(TK_IT400, 13, by + 38 * k), tw_, 0, l2, 0, 0, W(.7f), 0);
        if (pill) aq_pill(d, x + w - (20 + PILL_W) * k, by + 21 * k, p, 1);
    }
}
static float aq_card(pctx *c, float y, int i)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    if (vis(c, y, CARD_H + 20)) {
        aq_card_face(c->d, i, MARGIN, y, 1, 1);
        aq_mark(c, i);
    }
    if (st_busy(p)) aq_live_add(LV_CARD, i, MARGIN + 80, y + CARD_H - 72, CONTENT_R - 20 - PILL_W - 10 - MARGIN - 80);
    if (at(c, MARGIN, y, CONTENT_W, CARD_H)) {
        if (c->hx >= CONTENT_R - 20 - PILL_W - 8 && c->hy >= y + CARD_H - 60) aq_hitr(c, H_ACT, i, 0, CONTENT_R - 20 - PILL_W, y + CARD_H - 51, PILL_W, 30, 15, 1);
        else aq_hitr(c, H_STORY, i, 0, MARGIN, y, CONTENT_W, CARD_H, CARD_R, 0);   /* (iOS: the card opens its story) */
    }
    return y + CARD_H + 20;
}
/* the two categories with the most apps (ties: the catalog's order) */
static void aq_topcats(int *a, int *b)
{
    int n[10];
    *a = *b = -1;
    for (int k = 0; k < 10; k++) n[k] = aq_cat_n(AQ_CATS[k]);
    n[4] = 0;                                      /* (Games has its own tab) */
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
    aq_title_acct(c, "Today");
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
/* the categories as an inset group (Apps' end: iOS's "Top Categories"), All Apps first; games: 1 only the Games row */
static float aq_cats(pctx *c, float y)
{
    int rows[11], n = 0;
    rows[n++] = 10;
    for (int k = 0; k < 10; k++) if (k != 4 && aq_cat_n(AQ_CATS[k])) rows[n++] = k;
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
        if (at(c, GROUP_X, ry, GROUP_W, 52)) aq_hitr(c, H_CAT, -1, k, GROUP_X, ry, GROUP_W, 52, 0, 0);
    }
    return y + 52.0f * n;
}
/* Apps (iOS 27): the featured non-game apps' cards, every category's top three (See All), the categories */
static float pg_apps(pctx *c)
{
    aq_title_acct(c, "Apps");
    float y = CONTENT_TOP + 4;
    int nf = 0;
    for (int i = 0; i < aq_n() && nf < 2; i++) {
        const sh_lpkg_t *p = &LST->pkg[i];
        if (!(p->flags & SH_PKG_FEATURED) || !tk_strcmp(p->category, "Games")) continue;
        y = aq_card(c, y, i);
        nf++;
    }
    for (int k = 0; k < 10; k++) if (k != 4 && aq_cat_n(AQ_CATS[k])) y = aq_top(c, y + 4, k);
    y = aq_sec(c, y + 4, "Top Categories", 0, 0, 0);
    y = aq_cats(c, y) + 18;
    if (vis(c, y, 40)) {
        char ln[4][96];
        float wm;
        int m = wrapn(ln, 2, TK_IT400, 13, "Apps run in Linux on the phone's big cores. Each one installs from the developer, builds on the phone or comes from Debian.", GROUP_W - 32, &wm);
        for (int j = 0; j < m; j++) d_text(c->d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y + 18.0f * j), ln[j], 0, 0, W(.5f), 0);
    }
    return y + 44;
}
/* Games: a featured game's card, then every game */
static float pg_games(pctx *c)
{
    aq_title_acct(c, "Games");
    float y = CONTENT_TOP + 4;
    int first = -1, tot = aq_cat_n("Games"), k = 0;
    for (int i = 0; i < aq_n() && first < 0; i++)
        if (!tk_strcmp(LST->pkg[i].category, "Games") && (LST->pkg[i].flags & SH_PKG_FEATURED)) first = i;
    for (int i = 0; i < aq_n() && first < 0; i++) if (!tk_strcmp(LST->pkg[i].category, "Games")) first = i;
    if (first >= 0) y = aq_card(c, y, first);
    y = aq_sec(c, y + 4, "Top Games", 0, 0, 0);
    for (int i = 0; i < aq_n(); i++)
        if (!tk_strcmp(LST->pkg[i].category, "Games")) y = aq_row(c, y, i, 0, ++k < tot, 0);
    return y + 22;
}
static float pg_updates(pctx *c);
/* a category's apps (pushed from Apps' See All / Top Categories; "" all); the Account page */
static float pg_cat(pctx *c)
{
    if (!tk_strcmp(AQ.cat, AQ_ACCT)) return pg_updates(c);
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
/* Search (spec 2.5): idle: Discover (trending terms), Suggested (four apps), Categories (tiles); focused and empty:
 * Recent (with Clear) or Discover; typing: completions (the typed part regular, the rest semibold) and up to two
 * apps whose name starts so; submitted: the results, names first, then summaries, then categories */
static char AQ_TERMS[8][32];
static int aq_nterms;
static char aq_rec[8][32];
static int aq_nrec = -1;
static void aq_rec_load(void)
{
    char b[256];
    aq_nrec = 0;
    int n = sh_pref_get("store.recent", b, sizeof b);
    for (int k = 0, o = 0; n > 0 && k <= n && aq_nrec < 8; k++)
        if (k == n || b[k] == '\n') {
            if (k > o) { int l = tk_mini(k - o, 31); tk_memcpy(aq_rec[aq_nrec], b + o, (size_t)l); aq_rec[aq_nrec][l] = 0; aq_nrec++; }
            o = k + 1;
        }
}
static void aq_rec_add(const char *q)
{
    if (!q[0]) return;
    if (aq_nrec < 0) aq_rec_load();
    char t[8][32];
    int n = 0;
    lx_scopy(t[n++], q, 32);
    for (int k = 0; k < aq_nrec && n < 8; k++) if (!ci_has(aq_rec[k], q) || tk_strlen(aq_rec[k]) != tk_strlen(q)) lx_scopy(t[n++], aq_rec[k], 32);
    char b[256];
    int o = 0;
    for (int k = 0; k < n; k++) {
        int l = (int)tk_strlen(t[k]);
        if (o + l + 1 >= (int)sizeof b) break;
        if (o) b[o++] = '\n';
        tk_memcpy(b + o, t[k], (size_t)l); o += l;
    }
    b[o] = 0;
    (void)sh_pref_set("store.recent", b);
    aq_rec_load();
}
static int aq_starts(const char *s, const char *q)
{
    for (int k = 0; q[k]; k++) if (!s[k] || (s[k] | 32) != (q[k] | 32)) return 0;
    return 1;
}
static float aq_srow(pctx *c, float y, int glyph, const char *t, int typed, int blue, int what, int arg)
{
    if (vis(c, y, 44)) {
        dc *d = c->d;
        tk_col gc = blue ? ACCENT() : C_LABEL2;
        if (glyph == 0) { d_ring(d, MARGIN + 7, y + 20, 6, 1.8f, 1, gc, 0); d_seg(d, MARGIN + 11.5f, y + 24.5f, MARGIN + 15.5f, y + 28.5f, 2, gc); }
        else { d_ring(d, MARGIN + 8, y + 22, 7.5f, 1.6f, 1, gc, 0); d_seg(d, MARGIN + 8, y + 22, MARGIN + 8, y + 17, 1.6f, gc); d_seg(d, MARGIN + 8, y + 22, MARGIN + 11.5f, y + 24, 1.6f, gc); }
        tk_col tc = blue ? ACCENT() : INK;
        if (typed > 0) {                          /* the typed part regular, the completion semibold */
            char a[32], r[32];
            int l = tk_mini(typed, 31);
            tk_memcpy(a, t, (size_t)l); a[l] = 0;
            lx_scopy(r, t + l, 32);
            float aw = tw(TK_IT400, 17, a, 0, 0);
            d_text(d, TK_IT400, 17, MARGIN + 26, bl(TK_IT400, 17, y + 11), a, 0, 0, tc, 0);
            d_text(d, TK_IT600, 17, MARGIN + 26 + aw, bl(TK_IT600, 17, y + 11), r, 0, 0, tc, 0);
        } else d_text(d, TK_IT400, 17, MARGIN + 26, bl(TK_IT400, 17, y + 11), t, 0, 0, tc, 0);
        d_rrect(d, MARGIN, y + 43.5f, CONTENT_W, .5f, 0, W(.12f));
    }
    if (at(c, MARGIN, y, CONTENT_W, 44)) aq_hitr(c, what, -1, arg, 0, y, 390, 44, 0, 0);
    return y + 44;
}
static float pg_search(pctx *c)
{
    float y = AQ.top + 4;
    if (AQ.kb && !AQ.q[0]) {                      /* focused, empty: Recent */
        if (aq_nrec < 0) aq_rec_load();
        if (aq_nrec > 0) {
            if (vis(c, y, 40)) {
                d_text(c->d, TK_IT600, 17, MARGIN, bl(TK_IT600, 17, y + 10), "Recent", 0, 0, INK, 0);
                d_text_box(c->d, TK_IT400, 17, CONTENT_R, bl(TK_IT400, 17, y + 10), 80, 2, "Clear", 0, 0, ACCENT(), 0);
            }
            if (at(c, CONTENT_R - 70, y, 70, 40)) aq_hitr(c, H_RECCLR, -1, 0, CONTENT_R - 60, y + 6, 60, 28, 8, 1);
            y += 40;
            for (int k = 0; k < aq_nrec; k++) y = aq_srow(c, y, 1, aq_rec[k], 0, 0, H_RECENT, k);
            return y + 20;
        }
    }
    if (!AQ.q[0]) {                               /* idle: Discover, Suggested, Categories */
        y = aq_sec(c, y, "Discover", 0, 0, 0);
        for (int k = 0; k < 6; k++) y = aq_srow(c, y, 0, AQ_SUGG[k], 0, 1, H_SUGGEST, k);
        if (AQ.kb) return y + 20;
        y = aq_sec(c, y + 18, "Suggested", 0, 0, 0);
        int n = 0, tot = 0;
        for (int i = 0; i < aq_n() && tot < 4; i++) tot += !aq_inst(&LST->pkg[i]);
        for (int i = 0; i < aq_n() && n < 4; i++) if (!aq_inst(&LST->pkg[i])) { n++; y = aq_row(c, y, i, 0, n < tot, 0); }
        y = aq_sec(c, y + 18, "Categories", 0, 0, 0);
        int k2 = 0;
        for (int k = 0; k < 10; k++) {
            if (!aq_cat_n(AQ_CATS[k])) continue;
            float tx = MARGIN + (k2 & 1) * 180, ty = y + (k2 / 2) * 110;
            if (vis(c, ty, 100)) {
                dc *d = c->d;
                d_rrect(d, tx, ty, 170, 100, 16, tk_hex(AQ_CATC[k], 1));
                d_catglyph(d, k, tx + 170 - 44, ty + 12);
                d_text(d, TK_IT600, 17, tx + 12, bl(TK_IT600, 17, ty + 70), AQ_CATS[k], 0, 0, W(1), 0);
            }
            if (at(c, tx, ty, 170, 100)) aq_hitr(c, H_CAT, -1, k, tx, ty, 170, 100, 16, 0);
            k2++;
        }
        return y + ((k2 + 1) / 2) * 110 + 10;
    }
    if (!AQ.sub) {                                /* typing: completions, then up to two apps */
        aq_nterms = 0;
        for (int i = 0; i < aq_n() && aq_nterms < 6; i++)
            if (aq_starts(LST->pkg[i].name, AQ.q)) lx_scopy(AQ_TERMS[aq_nterms++], LST->pkg[i].name, 32);
        for (int k = 0; k < 10 && aq_nterms < 8; k++)
            if (aq_cat_n(AQ_CATS[k]) && aq_starts(AQ_CATS[k], AQ.q)) lx_scopy(AQ_TERMS[aq_nterms++], AQ_CATS[k], 32);
        for (int k = 0; k < 6 && aq_nterms < 8; k++)
            if (aq_starts(AQ_SUGG[k], AQ.q)) lx_scopy(AQ_TERMS[aq_nterms++], AQ_SUGG[k], 32);
        int ql = (int)tk_strlen(AQ.q);
        for (int k = 0; k < aq_nterms; k++) y = aq_srow(c, y, 0, AQ_TERMS[k], ql, 0, H_TERM, k);
        int na = 0;
        for (int i = 0; i < aq_n() && na < 2; i++) if (aq_starts(LST->pkg[i].name, AQ.q)) { na++; y = aq_row(c, y + (na == 1 ? 8 : 0), i, 0, 0, 0); }
        return y + 20;
    }
    int tot = 0, k = 0;
    for (int i = 0; i < aq_n(); i++) tot += aq_match(&LST->pkg[i]);
    if (!tot) {                                   /* spec: a magnifier 48, "No Results for ...", a hint */
        if (vis(c, y, 220)) {
            dc *d = c->d;
            float cy = y + 70;
            d_ring(d, 195 - 6, cy - 6, 17, 4, 1, W(.3f), 0);
            d_seg(d, 195 + 7, cy + 7, 195 + 19, cy + 19, 5, W(.3f));
            char l[64];
            AQS(l, "No Results for \xe2\x80\x9c", AQ.q, "\xe2\x80\x9d");
            d_text_box(d, TK_IT700, 22, 195, bl(TK_IT700, 22, cy + 40), CONTENT_W, 1, l, 0, 0, INK, 0);
            d_text_box(d, TK_IT400, 15, 195, bl(TK_IT400, 15, cy + 72), CONTENT_W, 1, "Check the spelling or try another word.", 0, 0, C_LABEL2, 0);
        }
        return y + 240;
    }
    for (int pass = 0; pass < 3; pass++)          /* names, then summaries / ids, then categories */
        for (int i = 0; i < aq_n(); i++) {
            const sh_lpkg_t *p = &LST->pkg[i];
            int nm = ci_has(p->name, AQ.q), sm = ci_has(p->summary, AQ.q) || ci_has(p->pkg, AQ.q), cm = ci_has(p->category, AQ.q);
            if ((pass == 0 && nm) || (pass == 1 && !nm && sm) || (pass == 2 && !nm && !sm && cm)) y = aq_row(c, y, i, 0, ++k < tot, 0);
        }
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
    aq_title(c, "App Updates");
    float y = CONTENT_TOP + 4;
    if (vis(c, y, 50)) {
        dc *d = c->d;
        char ck[40];
        aq_checked(ck, sizeof ck);
        d_rrect(d, GROUP_X, y, GROUP_W, 50, 16, C_CARD);
        d_text(d, TK_IT400, 17, ROW_TX, bl(TK_IT400, 17, y + 14), "Check for Updates", 0, 0, ACCENT(), 0);
        d_text_box(d, TK_IT400, 15, CONTENT_R - 16, bl(TK_IT400, 15, y + 16), 150, 2, ck, 0, 0, W(.5f), 0);
    }
    if (at(c, GROUP_X, y, GROUP_W, 50)) aq_hitr(c, H_CHECK, -1, 0, GROUP_X, y, GROUP_W, 50, 16, 0);
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
static float aq_textblock_n(pctx *c, float y, int f, float size, float lhh, const char *s, tk_col col, int maxl, int *cut);
static float aq_textblock(pctx *c, float y, int f, float size, float lhh, const char *s, tk_col col)
{
    return aq_textblock_n(c, y, f, size, lhh, s, col, 0, 0);
}
/* maxl > 0: at most maxl lines; if text is left, the last line ends in "…" and iOS's blue "more" sits at its right
 * (*cut = 1, a tap on it is the caller's) */
static float aq_textblock_n(pctx *c, float y, int f, float size, float lhh, const char *s, tk_col col, int maxl, int *cut)
{
    /* greedy word wrap of a long text (whole words; a word wider than the line by characters) */
    float maxw = CONTENT_W;
    int nl = 0;
    if (cut) *cut = 0;
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
        if (maxl > 0 && ++nl == maxl && s[last] && s[last + 1]) {   /* the last allowed line, with text after it */
            float mw = tw(f, size, "more", 0, 0);
            char e[200];
            ellipsize(e, sizeof e, f, size, s, maxw - mw - 14);
            if (vis(c, y - size, size * 1.6f)) {
                d_text(c->d, f, size, MARGIN, y, e, 0, 0, col, 0);
                d_text(c->d, f, size, CONTENT_R - mw, y, "more", 0, 0, ACCENT(), 0);
            }
            if (cut) *cut = 1;
            return y + lhh;
        }
        if (vis(c, y - size, size * 1.6f)) d_text(c->d, f, size, MARGIN, y, t, 0, 0, col, 0);
        y += lhh;
        s += last;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
        if (!last) break;
    }
    return y;
}
/* a coloured note under the app's head (two lines at most; aq_note_n: up to four) */
static float aq_note_n(pctx *c, float y, const char *t, uint32_t col, int maxl)
{
    char ln[4][96];
    float wm;
    int n = wrapn(ln, maxl, TK_IT500, 13, t, CONTENT_W, &wm);
    for (int k = 0; k < n; k++)
        if (vis(c, y - 4 + 18.0f * k, 22)) d_text(c->d, TK_IT500, 13, MARGIN, bl(TK_IT500, 13, y - 2 + 18.0f * k), ln[k], 0, 0, tk_hex(col, 1), 0);
    return y + 6 + 18.0f * n;
}
static float aq_note(pctx *c, float y, const char *t, uint32_t col) { return aq_note_n(c, y, t, col, 2); }
/* why a release / source build can't come from the memory card now, from the engine's network check (store3 net_why) */
static const char *aq_net_text(void)
{
    switch (LST ? LST->net_why : 0) {
    case SH_NET_NO_CONNECTION: return "Needs the internet: Linux isn't connected yet. Join Wi-Fi in Settings, then tap Get.";
    case SH_NET_NO_DNS: return "Needs the internet: Linux is connected but can't look up names. Tap Get to try again.";
    case SH_NET_NO_ANSWER: return "Needs the internet: Linux is connected but the internet doesn't answer. Tap Get to try again.";
    default: return "Needs the internet: it isn't on the memory card. Tap Get to try, or choose Debian.";
    }
}
static float pg_app(pctx *c)
{
    int i = st_find(AQ.pkg);
    if (i < 0) return AQ.top;
    const sh_lpkg_t *p = &LST->pkg[i];
    float y0 = PUSHED_TOP, tx = 154, tw_ = CONTENT_R - tx;
    int inst = aq_inst(p), busy = st_busy(p), net = aq_needs_net(p), full = !inst && (p->flags & SH_PKG_NO_SPACE);
    /* the head: the icon, the name, the summary, the main button (or the progress) */
    if (vis(c, y0, 104)) {
        dc *d = c->d;
        if (tk_strcmp(aq_dic_pkg, p->pkg) || !aq_dic.px) { lx_make_icon(&aq_dic, (int)(PT(118) + .5f), p->icon, p->name, 0); lx_scopy(aq_dic_pkg, p->pkg, 40); }
        aq_blit(d, &aq_dic, MARGIN, y0);
        d_text_box(d, TK_IT700, 22, tx, bl(TK_IT700, 22, y0 + 2), tw_, 0, p->name, 0, 0, INK, 0);
        char ln[4][96];
        float wm;
        int n = wrapn(ln, 2, TK_IT400, 14, p->summary[0] ? p->summary : p->category, tw_, &wm);
        for (int k = 0; k < n; k++) d_text(d, TK_IT400, 14, tx, bl(TK_IT400, 14, y0 + 30 + 18.0f * k), ln[k], 0, 0, C_LABEL2, 0);
        float by = y0 + 86;
        if (busy) {
            /* the live layer: the ring, the phase, Cancel */
        } else {
            /* (needing the internet doesn't disable GET: the engine looks again at once, the network may just be up) */
            const char *t = full ? "Not Enough Space" : aq_upd(p) ? "Update" : inst ? "Open" : p->state == SH_PKG_FAILED ? "Retry" : "Get";
            int dis = full;
            float w = tk_maxf(74, tw(TK_IT600, 17, t, 0, 0) + 40);   /* (spec 2.2.1: min 74 x 32, label + 2 x 20) */
            d_rrect(d, tx, by, w, 32, 16, dis ? tk_hex(0x767680, .24f) : ACCENT());
            d_text_box(d, TK_IT600, 17, tx + w / 2, bl(TK_IT600, 17, by + 6), w - 8, 1, t, 0, 0, dis ? W(.3f) : W(1), 0);
            if (aq_upd(p)) {                     /* the update's Open beside it */
                float ox = tx + w + 10;
                d_rrect(d, ox, by, 74, 32, 16, tk_hex(0x767680, .24f));
                d_text_box(d, TK_IT600, 17, ox + 37, bl(TK_IT600, 17, by + 6), 70, 1, "Open", 0, 0, ACCENT(), 0);
            }
        }
        aq_mark(c, i);
    }
    if (busy) aq_live_add(LV_HEAD, i, tx, y0 + 86, tw_);
    if (!c->d && c->hy >= y0 + 80 && c->hy < y0 + 124 && c->hx >= tx) {
        float w = tk_maxf(74, tw(TK_IT600, 17, aq_upd(p) ? "Update" : "Open", 0, 0) + 40);
        if (aq_upd(p) && !busy && c->hx >= tx + w + 4) aq_hitr(c, H_OPEN, i, 0, tx + w + 10, y0 + 86, 74, 32, 16, 1);
        else {
            const char *t = full ? "Not Enough Space" : aq_upd(p) ? "Update" : inst ? "Open" : p->state == SH_PKG_FAILED ? "Retry" : "Get";
            aq_hitr(c, H_MAIN, i, 0, tx, y0 + 86, busy ? 32 : tk_maxf(74, tw(TK_IT600, 17, t, 0, 0) + 40), 32, 16, 1);
        }
    }
    float y = y0 + 134;   /* (spec 2.3.1: a 118 pt icon, Get 74 x 32 flush with its bottom) */
    /* a one-line note: Wi-Fi, space, the failure, memory */
    const char *note = 0;
    uint32_t ncol = 0xffb340;
    char nb[160];
    if (p->state == SH_PKG_FAILED && !busy) {
        /* the failure first: the engine's short words, then what to do (sbstore.py HINTS) */
        AQS(nb, "Didn't install: ", p->err[0] ? p->err : "the store stopped with an error.");
        y = aq_note(c, y, nb, 0xff6961);
        if (p->hint[0]) y = aq_note_n(c, y, p->hint, 0xebebf5, 4);
    } else if (net) note = aq_net_text();
    else if (full) {
        char s[24], f[24];
        lx_fmt_kb(s, p->inst_kb);
        lx_fmt_kb(f, LST->overlay_free_kb);
        AQS(nb, "It needs ", s, " on the memory card; ", f, " is free.");
        note = nb; ncol = 0xff6961;
    }
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
    /* Preview (spec 2.3.4): the screenshots, 235 tall, r 22, 9 apart; placeholders until each is decoded */
    if (SS.n && !tk_strcmp(SS.pkg, p->pkg)) {
        y = aq_sec(c, y + 4, "Preview", 0, 0, 0);
        SS.y = y;
        if (vis(c, y, SS_H))
            for (int k = 0; k < SS.n; k++) {
                float x = MARGIN + k * (SS_W + SS_GAP);
                if (x < 390 && !aqss[k].bmp.px) d_rrect(c->d, x, y, SS_W, SS_H, 22, C_CARD);
            }
        y += SS_H + 18;
    }
    /* What's New (iOS: the section, the version and its notes); ours: the newest version and where it comes from */
    {
        unsigned m = AQ.pick ? (unsigned)AQ.pick : p->def_method;
        const char *nv = p->ver_new[0] ? p->ver_new : aq_mver(p, m);
        if (nv[0]) {
            y = aq_sec(c, y + 4, "What's New", 0, 0, 0);
            char vl[48], note[160];
            AQS(vl, "Version ", nv);
            if (vis(c, y, 22)) d_text(c->d, TK_IT400, 15, MARGIN, bl(TK_IT400, 15, y), vl, 0, 0, C_LABEL2, 0);
            int deb = p->source[0] == 'D' && p->source[1] == 'e';
            if (inst && aq_upd(p)) AQS(note, p->ver_inst, " is on this phone. ", nv, " is ready: tap UPDATE.");
            else if (inst) AQS(note, "You have the newest version, ", p->ver_inst[0] ? p->ver_inst : nv, ".");
            else if (m == SH_M_DISTRO || deb) AQS(note, "Debian 13's package, updated with Debian's security fixes.");
            else AQS(note, "The developer's newest release", p->source[0] ? ", from " : ".", p->source[0] ? p->source : "", p->source[0] ? "." : "");
            y = aq_textblock(c, y + 44, TK_IT400, 16, 22, note, INK) + 6;
        }
    }
    /* the description: three lines and "more" (iOS), all of it once opened */
    {
        int cut = 0;
        float y0 = y + 18;
        y = aq_textblock_n(c, y0, TK_IT400, 16, 22, p->desc[0] ? p->desc : p->summary[0] ? p->summary : p->name, INK,
                           AQ.more ? 0 : 3, &cut) - 4;
        if (cut && at(c, MARGIN, y0 - 20, CONTENT_W, y - y0 + 26)) aq_hitr(c, H_MORE, i, 0, MARGIN, y0 - 18, CONTENT_W, y - y0 + 22, 10, 0);
    }
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
    /* Install Options (cp7z): collapsed under the description; GET uses the preselected (recommended, or the user's
     * last) method without asking. Open: B2's segmented control of the offered methods and the chosen one's card */
    int nm = aq_mrow_n(p);
    if (nm) {
        y += 6;
        unsigned cur = AQ.pick ? (unsigned)AQ.pick : p->def_method;
        if (vis(c, y, 52)) {
            dc *d = c->d;
            d_rrect(d, GROUP_X, y, GROUP_W, 52, 16, C_CARD);
            d_text(d, TK_IT400, 17, ROW_TX, bl(TK_IT400, 17, y + 15), "Install Options", 0, 0, INK, 0);
            {   /* the chosen way, right-aligned before the chevron, cut to the room the label leaves */
                float lw = tw(TK_IT400, 17, "Install Options", 0, 0), room = CONTENT_R - 40 - (ROW_TX + lw + 14);
                char v[48];
                ellipsize(v, sizeof v, TK_IT400, 17, aq_mname(cur), room);
                d_text_box(d, TK_IT400, 17, CONTENT_R - 40, bl(TK_IT400, 17, y + 15), room, 2, v, 0, 0, C_LABEL2, 0);
            }
            /* the chevron: right when shut, down when open */
            if (AQ.opts) { d_seg(d, CONTENT_R - 34, y + 23, CONTENT_R - 28, y + 29, 2.2f, W(.3f)); d_seg(d, CONTENT_R - 28, y + 29, CONTENT_R - 22, y + 23, 2.2f, W(.3f)); }
            else d_chev(d, CONTENT_R - 30, y + 26, W(.3f));
        }
        if (at(c, GROUP_X, y, GROUP_W, 52)) aq_hitr(c, H_OPTS, i, 0, GROUP_X, y, GROUP_W, 52, 16, 0);
        y += 52 + 10;
        if (AQ.opts) {
        static const unsigned ORD[3] = { SH_M_RELEASE, SH_M_SOURCE, SH_M_DISTRO };
        const char *labels[3];
        unsigned ms[3];
        int n = 0, sel = -1;
        for (int q = 0; q < 3; q++) {
            unsigned m = ORD[q];
            if (!((p->methods >> m) & 1)) continue;
            if ((unsigned)AQ.pick == m) sel = n;
            ms[n] = m, labels[n++] = aq_mseg(m);
        }
        if (n > 1) {
            float sw = (GROUP_W - 4) / n;
            if (vis(c, y, 32)) kit_d_segment(c->d, GROUP_X, y, GROUP_W, labels, n, sel);
            for (int k = 0; k < n; k++)
                if (at(c, GROUP_X + 2 + sw * k, y - 6, sw, 44)) aq_hitr(c, H_METHOD, i, (int)ms[k], GROUP_X + 2 + sw * k, y + 2, sw, 28, 14, 1);
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
                AQS(s, "About ", bm, " min on this phone", v[0] ? " \xc2\xb7 " : "", v);
            } else AQS(s, v[0] ? v : "Debian", " \xc2\xb7 Debian 13");
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
            if (at(c, GROUP_X, y, GROUP_W, 50)) aq_hitr(c, H_REINSTALL, i, 0, GROUP_X, y, GROUP_W, 50, 16, 0);
            y += 60;
        } else if (busy) y += 4;
        else {
            char ln[4][96];                       /* (wrapn's array is 4 rows: gcc 13 checks the size) */
            float wm;
            const char *f = inst ? "Choose another way, then Reinstall to switch." : aq_mwhy(m);
            int k = wrapn(ln, 2, TK_IT400, 13, f, GROUP_W - 32, &wm);
            for (int q = 0; q < k; q++)
                if (vis(c, y + 17.0f * q, 20)) d_text(c->d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y + 17.0f * q), ln[q], 0, 0, W(.45f), 0);
            y += 9 + 17.0f * k;
        }
        y += 6;
        }
        y += 8;
    }
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
                if (at(c, GROUP_X, y, GROUP_W, 50)) aq_hitr(c, H_ROLLBACK, i, 0, GROUP_X, y, GROUP_W, 50, 16, 0);
                y += 50;
            }
            if (vis(c, y, 50)) d_text(c->d, TK_IT400, 17, ROW_TX, bl(TK_IT400, 17, y + 14), "Remove App", 0, 0, C_RED, 0);
            if (at(c, GROUP_X, y, GROUP_W, 50)) aq_hitr(c, H_REMOVE, i, 0, GROUP_X, y, GROUP_W, 50, 16, 0);
            y += 50;
        }
    }
    return y;
}
/* ---- the screenshots ---- */
static void ss_free(void)
{
    if (SS.loading >= 0 && DJ.kind) dj_free();
    SS.loading = -1;
    for (int k = 0; k < 3; k++) { relayer(&aqss[k]); aqss[k].hidden = 1; }
    SS.pkg[0] = 0; SS.n = 0; SS.x = SS.v = SS.to = 0; SS.hdrag = 0;
}
static void ss_list(void)
{
    if (sh_card_mount() != SH_CARD_READY) return;   /* (still mounting, or no card: asked again at the next page) */
    ss_nd = 0;
    sh_dirent_t *e = tk_alloc(sizeof(sh_dirent_t) * 32);
    if (!e) return;
    for (int first = 0; ss_nd < 160;) {
        int n = sh_card_list(SS_DIR, e, 32, first);
        if (n <= 0) break;
        for (int k = 0; k < n && ss_nd < 160; k++)
            if (!e[k].dir && tk_strlen(e[k].name) < 48) { lx_scopy(SSD[ss_nd].n, e[k].name, 48); SSD[ss_nd].sz = e[k].size; ss_nd++; }
        if (n < 32) break;
        first += n;
    }
    tk_free(e);
}
static void ss_next(void)
{
    SS.loading = -1;
    for (int k = 0; k < SS.n; k++)
        if (!aqss[k].bmp.px) {
            SS.loading = k;
            dj_begin(1, SS.file[k], SS.size[k], (int)(PT(SS_W) + .5f), (int)(PT(SS_H) + .5f));
            return;
        }
}
/* the page's shots: the card's files for this app (the listing read once per opening of the store) */
static void ss_sync(void)
{
    if (!AQ.app || AQ.story) { if (SS.pkg[0]) ss_free(); return; }
    if (!tk_strcmp(SS.pkg, AQ.pkg)) return;
    ss_free();
    lx_scopy(SS.pkg, AQ.pkg, 40);
    if (ss_nd < 0) ss_list();
    for (int n = 1; n <= 3 && ss_nd > 0; n++) {
        char want[64], nb[4];
        aq_dec(nb, n);
        AQS(want, AQ.pkg, "-", nb, ".jpg");
        for (int k = 0; k < ss_nd; k++)
            if (!tk_strcmp(SSD[k].n, want)) { AQS(SS.file[SS.n], SS_DIR, "/", want); SS.size[SS.n] = SSD[k].sz; SS.n++; break; }
    }
    if (SS.n) ss_next();
}
/* a few decoder steps a frame; a finished picture becomes its layer's bitmap */
static int ss_tick(void)
{
    if (SS.loading < 0 || !DJ.kind) return 0;
    for (int st = 0; st < 6; st++) {
        int rc = dj_step();
        if (rc == 0) continue;
        int k = SS.loading;
        if (rc > 0 && k >= 0 && k < 3) {
            tk_layer *l = &aqss[k];
            if (!l->name) tk_layer_init(l, "store-shot");
            relayer(l);
            l->bmp = DJ.out;                      /* (the decoder's bitmap is the layer's now) */
            tk_memset(&DJ.out, 0, sizeof DJ.out);
            tk_layer_dirty(l);
        }
        dj_free();
        ss_next();
        relink();
        return 1;
    }
    return 1;
}
static float ss_max(void) { return tk_maxf(0, SS.n * SS_W + (SS.n - 1) * SS_GAP + 2 * MARGIN - 390); }
/* the shots where the page and the sideways scroll put them (aq_place) */
static void ss_place(float s, float off, float al)
{
    for (int k = 0; k < 3; k++) {
        tk_layer *l = &aqss[k];
        if (!l->bmp.px || !AQ.app || k >= SS.n) { l->hidden = 1; continue; }
        float x = MARGIN + k * (SS_W + SS_GAP) - SS.x, y = SS.y - s;
        l->x = PT(x) + off; l->y = PT(y);
        l->sx = l->sy = 1; l->alpha = al;
        l->hidden = AQ.hide || y + SS_H < AQ.top || y > AQ.bot;
        float y0 = tk_maxf(y, AQ.top), y1 = tk_minf(y + SS_H, AQ.bot >= H_PT - 1 ? TB_Y + 16 : AQ.bot);
        l->clip = 1;
        l->clip_r = (tk_rect){ PT(x) + off, PT(y0), PT(SS_W), PT(tk_maxf(0, y1 - y0)) };
        l->clip_radius = PT(22);
    }
}
/* a Today story (iOS's card opened): the card's face full width as the hero, the full text, the app's row */
#define HERO_K (390.0f / CONTENT_W)
static float pg_story(pctx *c)
{
    int i = AQ.story - 1;
    if (i < 0 || i >= aq_n()) return AQ.top;
    const sh_lpkg_t *p = &LST->pkg[i];
    float hh = CARD_H * HERO_K;
    if (vis(c, 0, hh)) aq_card_face(c->d, i, 0, 0, HERO_K, 0);
    float y = hh + 26;
    y = aq_textblock(c, y + 18, TK_IT400, 17, 24, p->desc[0] ? p->desc : p->summary, INK) + 10;
    /* the app's row in a card, as iOS ends a story: icon, name, category, GET; the row opens the product page */
    if (vis(c, y, 76)) {
        dc *d = c->d;
        d_rrect(d, GROUP_X, y, GROUP_W, 76, 18, C_CARD);
        aq_blit(d, aq_icon(i), GROUP_X + 14, y + 14);
        float tx = GROUP_X + 76, tw_ = PILL_X - 14 - tx;
        d_text_box(d, TK_IT600, 16, tx, bl(TK_IT600, 16, y + 18), tw_, 0, p->name, 0, 0, INK, 0);
        d_text_box(d, TK_IT400, 13, tx, bl(TK_IT400, 13, y + 41), tw_, 0, p->category, 0, 0, C_LABEL2, 0);
        if (!st_busy(p)) aq_pill(d, PILL_X - 8, y + 23, p, 0);
        aq_mark(c, i);
    }
    if (at(c, GROUP_X, y, GROUP_W, 76)) {
        if (c->hx >= PILL_X - 18) aq_hitr(c, H_ACT, i, 0, PILL_X - 8, y + 23, PILL_W, 30, 15, 1);
        else aq_hitr(c, H_APP, i, 0, GROUP_X, y, GROUP_W, 76, 18, 0);
    }
    return y + 76 + 40;
}
static float aq_paint(pctx *c)
{
    if (AQ.story) return pg_story(c);
    if (!LST || LST->n <= 0) return AQ.top;
    if (AQ.app) return pg_app(c);
    if (AQ.cat_on) return pg_cat(c);
    if (AQ.tab == AT_TODAY) return pg_today(c);
    if (AQ.tab == AT_GAMES) return pg_games(c);
    if (AQ.tab == AT_APPS) return pg_apps(c);
    return pg_search(c);
}

/* ---- the layers ---- */
static void aq_area(void)
{
    AQ.top = AQ.story ? 0 : AQ.tab == AT_SEARCH && !AQ.app && !AQ.cat_on ? AQ_FY() + 54 : SAFE_TOP;
    AQ.bot = AQ.kb && AQ.tab == AT_SEARCH && !AQ.app ? 540 : H_PT;
    pctx c = { 0, 0, 0, -1e9f, -1e9f, -1, 0, 0, 0, { 0, 0, 0, 0 }, 0, 0 };
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
    pctx p = { &d, y0, y0 + AQ_CH, 0, 0, L, 0, 0, 0, { 0, 0, 0, 0 }, 0, 0 };
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
        {   /* the row's old status line and pill hidden (the page's black), the name above them left alone */
            int lx0 = (int)tk_floorf(PT(x) - d.ox), ly0 = (int)tk_floorf(PT(y + 37) - d.oy);
            int px0 = (int)tk_floorf(PT(PILL_X - 6) - d.ox), py0 = (int)tk_floorf(PT(y + 21) - d.oy);
            if (ly0 >= 0 && ly0 < d.b->h) tk_fill_rect_raw(d.b, tk_maxi(lx0, 0), ly0, d.b->w - tk_maxi(lx0, 0), d.b->h - ly0, 0xff000000u);
            if (px0 >= 0 && px0 < d.b->w) tk_fill_rect_raw(d.b, px0, tk_maxi(py0, 0), d.b->w - px0, d.b->h - tk_maxi(py0, 0), 0xff000000u);
        }
        d_text_box(&d, TK_IT400, 13, x, bl(TK_IT400, 13, y + 39), w, 0, s, 0, 0, sc, 0);
        aq_pill(&d, PILL_X, y + 23, p, 0);
    } else if (AQL[it].kind == LV_CARD) {
        dc d = mk(l, "store-live", x, y + 18, CONTENT_R - 20 - x, 42, 1, 0);
        d_text_box(&d, TK_IT400, 13, x, bl(TK_IT400, 13, y + 38), w, 0, s, 0, 0, W(.75f), 0);
        aq_pill(&d, CONTENT_R - 20 - PILL_W, y + 21, p, 1);
    } else {
        dc d = mk(l, "store-live", x, y - 2, w, 40, 1, 0);
        tk_fill_rect_raw(d.b, 0, 0, d.b->w, d.b->h, 0xff000000u);   /* (the page's black over the GET under it until
                                                                       * the page's own chunk is drawn again) */
        aq_ring(&d, x + 16, y + 16, p, 0);        /* (where GET was: a tap on it cancels, as iOS) */
        d_text_box(&d, TK_IT500, 13, x + 42, bl(TK_IT500, 13, y + 9), w - 42, 0, s, 0, 0, W(.75f), 0);
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
    if (AQ.px != 0) {                                       /* spec 2.1.6: a push slides the full width; a pop comes from
                                                             * -30 % (the page that was under), faded from 0.6 */
        off = PT(AQ.px);
        al = AQ.px < 0 ? 1 - .4f * tk_minf(1, -AQ.px / 117) : 1;
    }
    if (ZS.phase == 1 || ZS.phase == 3) al *= 1 - .35f * tk_clampf(ZS.t, 0, 1);   /* (the page dims under the zoom) */
    float dsc = 1, dty = 0;
    if (AQ.story && ZS.drag > 0) { dsc = 1 - .2f * tk_minf(ZS.drag / 300, 1); dty = ZS.drag * .35f; }
    for (int L = 0; L < AQ_NCH; L++) {
        tk_layer *l = &aqc[L];
        if (!l->bmp.px) continue;
        l->hidden = AQ.hide || AQ.ck[L] < c0 || AQ.ck[L] > c1;
        l->y = l->by - tk_floorf(PT(s) + .5f);
        l->x = l->bx + tk_floorf(off + .5f);
        l->alpha = al;
        l->clip = 1;
        l->clip_r = (tk_rect){ 0, PT(AQ.top), TK_W, PT(area) };
        /* iOS 26's scroll edge: a soft fade under the status bar and the back circle; once the inline title shows,
         * deep enough (the nav row) that the page fades out before it reaches the title */
        l->fade_top = s > 1 ? PT(30) : 0;
        /* the bottom: over the keyboard a short fade; else iOS 26's scroll edge under the tab bar (the page fades
         * out behind the glass, nothing shows round its rounded ends) */
        l->fade_bot = AQ.bot < H_PT - 1 ? (AQ.top + AQ.h - s > AQ.bot + 1 ? PT(24) : 0) : PT(64);
        if (AQ.bot >= H_PT - 1) l->clip_r.h = PT(TB_Y + 16 - AQ.top);
        if (AQ.story) l->clip_r = (tk_rect){ 0, 0, TK_W, PT(H_PT) };
        l->sx = l->sy = dsc; l->ox = PT(195); l->oy = 0; l->clip_radius = 0;
        if (dsc < 1) {
            l->y += PT(dty) / dsc;
            l->clip_r = (tk_rect){ PT(195 - 195 * dsc), PT(dty), PT(390 * dsc), PT(H_PT * dsc) };
            l->clip_radius = PT(32);
            l->fade_top = 0;
        }
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
    ss_place(s, off, al);
    if (AQ.app || AQ.cat_on) { aqh.x = aqh.bx + tk_floorf(off + .5f); aqh.alpha = al; }
    if (aqit.bmp.px) {                            /* iOS: the inline title cross-fades in over 20 pt once the large one is under the bar */
        aqit.alpha = tk_clampf((s - 30) / 20, 0, 1) * al;
        aqit.x = aqit.bx + tk_floorf(off + .5f);
    }
    if (drew) relink();
    AQ.left = left;
    return left;
}
/* the fixed head: the back circle on a pushed page; Search's title and field; an empty store's words */
static const char *aq_inline_t(void)
{
    if (AQ.app || AQ.empty || AQ.story) return 0;
    if (AQ.cat_on) return !tk_strcmp(AQ.cat, AQ_ACCT) ? "App Updates" : AQ.cat[0] ? AQ.cat : "All Apps";
    return AQ.tab == AT_GAMES ? "Games" : AQ.tab == AT_APPS ? "Apps" : 0;   /* (Today has none: spec 2.1.4) */
}
static void aq_head(void)
{
    relayer(&aqh);
    relayer(&aqit);
    aqh.hidden = aqit.hidden = 1;
    if (!is_app("store")) return;
    const char *it = aq_inline_t();
    if (it) {                                     /* (shown by aq_place as the large title scrolls under the bar) */
        /* iOS 26's scroll edge under the bar: the page's black over it, fading out 24 pt below the nav row */
        float hh = SAFE_TOP + NAVBAR_H + 24;
        dc d = mk(&aqit, "store-inline", 0, 0, 390, hh, 0, 0);
        tk_stop st[3] = { { 0, tk_hex(0x000000, 1) }, { (SAFE_TOP + NAVBAR_H - 6) / hh, tk_hex(0x000000, .92f) }, { 1, tk_hex(0x000000, 0) } };
        tk_paint pg = tk_linear(180, 0, 0, (float)d.b->w, (float)d.b->h, 3, st);
        tk_fill_rrect(d.b, 0, 0, (float)d.b->w, (float)d.b->h, 0, &pg, 1);
        d_text_box(&d, TK_IT600, 17, 195, NAV_BASE, 250, 1, it, 0, 0, INK, 0);
        aqit.alpha = 0;
        aqit.hidden = 0;
    }
    if (AQ.empty) {
        const char *t, *l;
        if (LX.state == SH_LINUX_FAILED) { t = "Linux Didn't Start"; l = LX.why[0] ? LX.why : "The big cores did not start."; }
        else if (LX.state != SH_LINUX_RUNNING) {
            static char sl[120];
            char n[8];
            aq_dec(n, (int)((aq_st_exp + 2500) / 5000) * 5);
            t = "Starting Linux";
            if (LX.why[0]) l = LX.why;            /* the start's own words (a disk check: Track E) */
            else if (aq_st_late) l = "Taking longer than usual. After a restart, Linux checks its disk first: this can take a minute.";
            else { AQS(sl, "The App Store's apps run in Linux on the big cores. Starting it takes about ", n, " seconds."); l = sl; }
        }
        else if (!LST) { t = "Reading the Catalog"; l = "The apps' list comes from the memory card."; }
        else { t = "No Apps"; l = LST->repo_ok ? "The catalog is empty." : "The App Store's catalog isn't on the memory card yet."; }
        dc d = mk(&aqh, "store-head", 0, SAFE_TOP, 390, TB_Y - SAFE_TOP, 0, 0);
        d_h1(&d, "App Store");
        d_empty(&d, CONTENT_TOP, TB_Y, t, l);
        {   /* (where d_empty put the words: the bar 22 pt under them) */
            char ln[4][96];
            float wm;
            int n = wrapn(ln, 3, TK_IT400, 15, l, 300, &wm);
            float h = 36 + 20.0f * n - (n ? 2 : 8);
            aq_bar_y = tk_floorf((CONTENT_TOP + TB_Y - h) / 2) + h + 22;
        }
        trim_layer(&aqh, 0, 0);
        aqh.hidden = 0;
        return;
    }
    if (AQ.story) {                               /* the story's close: a dark glass circle with an X, top right */
        float cx = CONTENT_R - 17, cy = SAFE_TOP + 19;
        dc d = mk(&aqh, "store-story-x", CONTENT_R - 40, SAFE_TOP - 4, 46, 46, 0, 0);
        d_rrect(&d, cx - 17, cy - 17, 34, 34, 17, tk_hex(0x1c1c1e, .72f));
        d_seg(&d, cx - 5.5f, cy - 5.5f, cx + 5.5f, cy + 5.5f, 2.4f, W(.9f));
        d_seg(&d, cx + 5.5f, cy - 5.5f, cx - 5.5f, cy + 5.5f, 2.4f, W(.9f));
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
    if (AQ.tab != AT_SEARCH || AQ.cat_on) return;
    /* spec 2.5.1: idle: the large title, the Account button, the field (a 44 capsule, x 16..374) under them; focused:
     * the field in the nav row (x 16..322) and a 44 glass "x" circle at x 330..374 */
    float fy = AQ_FY(), fx = 16, fw = AQ.kb ? 306 : 358;
    dc d = mk(&aqh, "store-search", 0, SAFE_TOP - 10, 390, fy + 54 - (SAFE_TOP - 10), 0, 0);
    tk_fill_rect_raw(d.b, 0, 0, d.b->w, d.b->h, 0xff000000u);   /* opaque: the results scroll under it */
    if (!AQ.kb) {
        d_text_box(&d, TK_IT700, 34, MARGIN, LARGE_BASE, CONTENT_W - ACCT_D - 12, 0, "Search", 0, 0, INK, 0);
        float cx = ACCT_X + ACCT_D / 2, cy = ACCT_Y + ACCT_D / 2;
        d_rrect(&d, ACCT_X, ACCT_Y, ACCT_D, ACCT_D, ACCT_D / 2, tk_hex(0x8e8e93, 1));
        d_rrect(&d, cx - 5, cy - 10, 10, 10, 5, W(1));
        d_rrect(&d, cx - 9.5f, cy + 2, 19, 10, 5, W(1));
    }
    d_rrect(&d, fx, fy, fw, 44, 22, tk_hex(0x767680, .24f));
    d_ring(&d, fx + 20, fy + 20, 6.5f, 1.9f, 1, C_LABEL2, 0);
    d_seg(&d, fx + 25, fy + 25, fx + 29, fy + 29, 2.1f, C_LABEL2);
    float tx = fx + 38;
    if (AQ.q[0]) {
        d_text_box(&d, TK_IT400, 17, tx, bl(TK_IT400, 17, fy + 11), fw - 74, 0, AQ.q, 0, 0, INK, 0);
        if (AQ.kb) d_rrect(&d, tx + tk_minf(tw(TK_IT400, 17, AQ.q, 0, 0), fw - 74) + 1, fy + 11, 2, 22, 1, ACCENT());
        float cx = fx + fw - 20, cy = fy + 22;      /* the clear (x) button inside the field */
        d_rrect(&d, cx - 9, cy - 9, 18, 18, 9, W(.35f));
        d_seg(&d, cx - 3.5f, cy - 3.5f, cx + 3.5f, cy + 3.5f, 1.8f, tk_hex(0x000000, 1));
        d_seg(&d, cx + 3.5f, cy - 3.5f, cx - 3.5f, cy + 3.5f, 1.8f, tk_hex(0x000000, 1));
    } else {
        d_text(&d, TK_IT400, 17, tx, bl(TK_IT400, 17, fy + 11), "Apps, Games and More", 0, 0, C_LABEL2, 0);
        if (AQ.kb) d_rrect(&d, tx - 1, fy + 11, 2, 22, 1, ACCENT());
    }
    if (AQ.kb) {                                  /* the cancel circle */
        float cx = 352, cy = fy + 22;
        d_lg_flat(&d, 330, fy, 44, 44, 22, 1);
        d_seg(&d, cx - 6, cy - 6, cx + 6, cy + 6, 2.2f, W(.9f));
        d_seg(&d, cx + 6, cy - 6, cx - 6, cy + 6, 2.2f, W(.9f));
    }
    aqh.hidden = 0;
}
/* the tab bar's glass (baked once per opening, from the open's build: never in a tap) */
static void aq_tb_bake(void)
{
    if (aq_tb_baked && aqtb.bmp.px) return;
    relayer(&aqtb);
    dc d = mk(&aqtb, "store-tabbar", 0, TB_Y - 20, 390, TB_H + 40, 0, 0);
    d_lg_flat(&d, TB_X, TB_Y, TB_W, TB_H, TB_H / 2, 1);
    trim_layer(&aqtb, 0, 0);
    aq_tb_baked = 1;
}
/* the tab bar's glyphs and labels, the selected tab's lighter pill, the Updates badge (redrawn on a change) */
/* the selected pill: 86.5 x 54 at TP.x, stretched up to 1.15x toward its motion */
static void aq_pill_place(void)
{
    if (aqtg.hidden || !TP.set) { aqtp.hidden = 1; return; }
    float st = 1 + tk_minf(tk_fabsf(TP.v) / 1500, .15f), w = TB_TABW * st, x = TP.x - (TP.v > 0 ? 0 : w - TB_TABW);
    tk_rect r = { x, TB_Y + 4, w, TB_H - 8 };
    hl_set(&aqtp, r, r, (TB_H - 8) / 2, W(.14f));
    aqtp.alpha = ZS.phase ? aqtb.alpha : 1;     /* (faded only by the story's zoom) */
}
/* one frame of its spring (response 0.35, damping 0.75: k 322.3, c 26.9) */
static int aq_pill_tick(float dt)
{
    if (!TP.set || (tk_fabsf(TP.x - TP.to) < .2f && tk_fabsf(TP.v) < 5)) { if (TP.set && TP.x != TP.to) { TP.x = TP.to; TP.v = 0; aq_pill_place(); } return 0; }
    for (float left = dt; left > 0; left -= .004f) {
        float h = tk_minf(left, .004f);
        TP.v += (322.3f * (TP.to - TP.x) - 26.9f * TP.v) * h;
        TP.x += TP.v * h;
    }
    aq_pill_place();
    return 1;
}
static void aq_tabs(void)
{
    relayer(&aqtg);
    aqtg.hidden = aqtb.hidden = aqtp.hidden = 1;
    if (!is_app("store") || (AQ.kb && AQ.tab == AT_SEARCH && !AQ.app) || AQ.story) return;
    aqtb.hidden = 0;
    dc d = mk(&aqtg, "store-tabs", TB_X, TB_Y, TB_W, TB_H, 2, 0);
    static const char *const TL[4] = { "Today", "Games", "Apps", "Search" };
    for (int t = 0; t < 4; t++) {
        float bx = TB_X + 4 + TB_TABW * t, cx = bx + TB_TABW / 2, cy = TB_Y + 22;
        int on = AQ.tab == t;
        tk_col c = on ? tk_hex(0x0091ff, 1) : tk_hex(0xf5f5f5, 1);   /* (spec: selected #0091FF, others #F5F5F5) */
        if (on) { TP.to = bx; if (!TP.set) { TP.x = bx; TP.v = 0; TP.set = 1; } }   /* (the pill is aqtp: aq_pill_place) */
        if (t == 0) {                             /* Today: a page with a picture and lines */
            d_rrect_stroke(&d, cx - 9, cy - 11, 18, 22, 4, 1.8f, c);
            d_rrect(&d, cx - 5, cy - 7, 10, 6, 1.5f, c);
            d_seg(&d, cx - 5, cy + 3, cx + 5, cy + 3, 1.6f, c);
            d_seg(&d, cx - 5, cy + 7, cx + 2, cy + 7, 1.6f, c);
        } else if (t == 1) {                      /* Games: our controller (a rounded body, a cross and two buttons) */
            d_rrect_stroke(&d, cx - 12, cy - 7, 24, 15, 7.5f, 1.8f, c);
            d_seg(&d, cx - 7.5f, cy + .5f, cx - 2.5f, cy + .5f, 1.7f, c);
            d_seg(&d, cx - 5, cy - 2, cx - 5, cy + 3, 1.7f, c);
            d_rrect(&d, cx + 3, cy - 2.2f, 3, 3, 1.5f, c);
            d_rrect(&d, cx + 6.2f, cy + .8f, 3, 3, 1.5f, c);
        } else if (t == 3) {                      /* Search: the magnifier */
            d_ring(&d, cx - 2, cy - 2, 7.5f, 2, 1, c, 0);
            d_seg(&d, cx + 3.5f, cy + 3.5f, cx + 8, cy + 8, 2.4f, c);
        } else {                                  /* Apps: three stacked layers */
            for (int q = 0; q < 3; q++) {
                float yy = cy - 8 + 5.5f * q;
                d_seg(&d, cx - 10, yy + 3, cx, yy - 1, 1.7f, c); d_seg(&d, cx, yy - 1, cx + 10, yy + 3, 1.7f, c);
                d_seg(&d, cx - 10, yy + 3, cx, yy + 7, 1.7f, c); d_seg(&d, cx, yy + 7, cx + 10, yy + 3, 1.7f, c);
            }
        }
        d_text_box(&d, TK_IT600, 10, cx, bl(TK_IT600, 10, TB_Y + 39), TB_TABW - 6, 1, TL[t], 0, 0, c, 0);
    }
    trim_layer(&aqtg, 0, 0);
    aqtg.hidden = 0;
    aq_pill_place();
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
    ss_sync();                                    /* (the product page's screenshots: before its layout) */
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
static char aq_want[40];                        /* (defined with sh_linux_store below: st_route_app's page) */
static sh_lstore_t aq_cache;                     /* (the kept table: defined with sh_linux_store below) */
static int aq_cached;
static int aqk_load(void);
static void st_build(void)
{
    if (!is_app("store")) return;
    if ((!LST || LST->n <= 0) && LX.state != SH_LINUX_FAILED && aqk_load()) { LST = &aq_cache; aq_cached = 1; }
    AQ.empty = !LST || LST->n <= 0;
    if (aq_want[0] && !AQ.empty && st_find(aq_want) >= 0) {   /* Track B, cp6: opened for this page (st_route_app) */
        const sh_lpkg_t *p = &LST->pkg[st_find(aq_want)];
        AQ.app = 1;
        lx_scopy(AQ.pkg, p->pkg, 40);
        AQ.pick = aq_inst(p) && p->method ? (int)p->method : p->def_method && (p->methods >> p->def_method & 1) ? (int)p->def_method : 0;
        if (!AQ.pick && p->methods) for (unsigned m = 1; m <= 3 && !AQ.pick; m++) if (p->methods >> m & 1) AQ.pick = (int)m;
        AQ.scroll = 0;
        AQ.kb = 0;
        aq_want[0] = 0;
    }
    if (AQ.app && st_find(AQ.pkg) < 0) AQ.app = 0;
    AQ.badge = AQ.empty ? 0 : aq_badge();
    aq_tb_bake();
    aq_go(0);
    if (ap_pending) { AQ.hide = 0; aq_place(1); }   /* the open's build: the top chunk now, the rest over frames */
    else aq_flush();                             /* (a still, a rebuild: every chunk now) */
    for (int i = 0; i < aq_n(); i++) AQ.psig[i] = aq_psig(&LST->pkg[i]);
}
/* 8h (Track A; 8g's phone: "app t1/20 127" and "sleep t1/20 126", the store's open: its st_build in the extras
 * frame was 7 ms on the Mac: the tab bar's glass 3, the top chunk 2): the open's strip frames draw nothing for the
 * store (its own layers), so the glass is baked in strip frame 1 and the kept table read in frame 2; the extras
 * frame keeps the page's top chunk. aq_tb_bake keeps a bake for the opening (aq_open clears it) */
static void st_prebuild(int st)
{
    if (!is_app("store")) return;
    if (st == 1) {
        int h = aqtb.hidden;
        aq_tb_bake();
        aqtb.hidden = h;                          /* (shown by aq_tabs, from st_build, as before) */
    } else if (st == 2 && (!LST || LST->n <= 0) && LX.state != SH_LINUX_FAILED && aqk_load()) {
        LST = &aq_cache;
        aq_cached = 1;
    }
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
static char aq_want[40];                        /* st_route_app: the page to push once the store's table has it */
static void aq_push(int i);
/* ---- cp7z (gaps-f-store.md #1): the last table kept in the home store (one sh_blob, ~10 KB for 55 apps: no long
 * descriptions, no icons), drawn at once when the store opens before Linux is up; a GET then waits for the real table.
 * Packed: "AKC1", n (u16), then per app u32 flags, inst_kb, dl_kb; u16 build_min, ram_mb, rel_size_mb; u8 methods,
 * method, def_method, touch, needs, flags3; then 11 NUL-ended strings (pkg, name, summary, category, the five versions,
 * licence, source). Written only when it differs from what is kept. ---- */
#define AQK_MAX 32768u
static sh_lstore_t aq_cache;                     /* (the platform's table shape, filled from the blob) */
static int aq_cached;                            /* LST is aq_cache: Linux not up yet */
static uint32_t aqk_kept, aqk_dkept;             /* the kept blobs' hashes (0: not read yet this boot) */
static struct { char pkg[40]; int op, m; } aq_pend;   /* one action asked for while cached */
static unsigned aqk_str(uint8_t *b, unsigned o, const char *s)
{
    while (*s && o < AQK_MAX - 1) b[o++] = (uint8_t)*s++;
    if (o < AQK_MAX) b[o++] = 0;
    return o;
}
static uint32_t aqk_rd(const uint8_t *b, unsigned *o, int w) { uint32_t v = 0; for (int k = 0; k < w; k++) v |= (uint32_t)b[(*o)++] << (8 * k); return v; }
static unsigned aqk_u(uint8_t *b, unsigned o, uint32_t v, int w) { for (int k = 0; k < w && o < AQK_MAX; k++) b[o++] = (uint8_t)(v >> (8 * k)); return o; }
static void aqk_save(const sh_lstore_t *t)
{
    if (!t || !t->v3 || t->n <= 0) return;
    static uint8_t b[AQK_MAX];                      /* (every table update packs: progress changes nothing kept, so the
                                                     * hash matches and nothing is written) */
    unsigned o = aqk_u(b, 0, 0x31434b41u, 4);      /* "AKC1" */
    o = aqk_u(b, o, (uint32_t)t->n, 2);
    int n = 0;
    for (int i = 0; i < t->n && i < SH_LPKGS; i++) {
        const sh_lpkg_t *p = &t->pkg[i];
        if (o > AQK_MAX - 600) break;               /* (room for one more at most: the rest are left out) */
        unsigned f = p->flags & ~(SH_PKG_NO_SPACE);  /* (space is the guest's to say, live) */
        o = aqk_u(b, o, f, 4); o = aqk_u(b, o, p->inst_kb, 4); o = aqk_u(b, o, p->dl_kb, 4);
        o = aqk_u(b, o, p->build_min, 2); o = aqk_u(b, o, p->ram_mb, 2); o = aqk_u(b, o, p->rel_size_mb, 2);
        o = aqk_u(b, o, p->methods, 1); o = aqk_u(b, o, p->method, 1); o = aqk_u(b, o, p->def_method, 1);
        o = aqk_u(b, o, p->touch, 1); o = aqk_u(b, o, p->needs, 1); o = aqk_u(b, o, p->flags3, 1);
        const char *ss[11] = { p->pkg, p->name, p->summary, p->category, p->ver_inst, p->ver_new, p->ver_rel,
                               p->ver_distro, p->ver_src, p->licence, p->source };
        for (int k = 0; k < 11; k++) o = aqk_str(b, o, ss[k]);
        n++;
    }
    b[4] = (uint8_t)n; b[5] = (uint8_t)(n >> 8);
    uint32_t h = aq_hash(2166136261u, "");
    for (unsigned k = 0; k < o; k++) h = aq_hu(h, b[k]);
    if (!aqk_kept) {                                /* the kept one's hash, once a boot */
        uint8_t *k2 = tk_alloc(AQK_MAX);
        int r = k2 ? sh_blob_get("store.cache", k2, AQK_MAX) : -1;
        aqk_kept = 1;
        if (r > 0) { uint32_t h2 = aq_hash(2166136261u, ""); for (int k = 0; k < r; k++) h2 = aq_hu(h2, k2[k]); aqk_kept = h2 | 1; }
        if (k2) tk_free(k2);
    }
    if ((h | 1) != aqk_kept && sh_blob_put("store.cache", b, o) == 0) aqk_kept = h | 1;
    /* the long descriptions: a second blob, "store.desc", in the same order, NUL-ended, cut so all fit in 32 KiB */
    static uint8_t d[AQK_MAX];
    unsigned q = 0, per = n ? (AQK_MAX - 64) / (unsigned)n : 0;
    if (per > sizeof t->pkg[0].desc) per = sizeof t->pkg[0].desc;
    for (int i = 0; i < n; i++) {
        const char *ds = t->pkg[i].desc;
        unsigned k = 0;
        while (ds[k] && k + 1 < per) k++;
        while (k > 0 && ds[k] && ((uint8_t)ds[k] & 0xc0) == 0x80) k--;   /* (not half a UTF-8 character) */
        for (unsigned j = 0; j < k && q < AQK_MAX - 1; j++) d[q++] = (uint8_t)ds[j];
        d[q++] = 0;
    }
    uint32_t hd = aq_hash(2166136261u, "");
    for (unsigned k = 0; k < q; k++) hd = aq_hu(hd, d[k]);
    if (!aqk_dkept) {
        static uint8_t k2[AQK_MAX];
        int r = sh_blob_get("store.desc", k2, AQK_MAX);
        aqk_dkept = 1;
        if (r > 0) { uint32_t h2 = aq_hash(2166136261u, ""); for (int k = 0; k < r; k++) h2 = aq_hu(h2, k2[k]); aqk_dkept = h2 | 1; }
    }
    if ((hd | 1) != aqk_dkept && sh_blob_put("store.desc", d, q) == 0) aqk_dkept = hd | 1;
}
static int aqk_load(void)
{
    uint8_t *b = tk_alloc(AQK_MAX);
    if (!b) return 0;
    int r = sh_blob_get("store.cache", b, AQK_MAX), n = 0;
    if (r >= 6 && b[0] == 'A' && b[1] == 'K' && b[2] == 'C' && b[3] == '1') {
        int want = b[4] | b[5] << 8;
        unsigned o = 6;
        tk_memset(&aq_cache, 0, sizeof aq_cache);
        for (; n < want && n < SH_LPKGS && o + 27 <= (unsigned)r; n++) {
            sh_lpkg_t *p = &aq_cache.pkg[n];
            p->flags = aqk_rd(b, &o, 4); p->inst_kb = aqk_rd(b, &o, 4); p->dl_kb = aqk_rd(b, &o, 4);
            p->build_min = aqk_rd(b, &o, 2); p->ram_mb = aqk_rd(b, &o, 2); p->rel_size_mb = aqk_rd(b, &o, 2);
            p->methods = aqk_rd(b, &o, 1); p->method = aqk_rd(b, &o, 1); p->def_method = aqk_rd(b, &o, 1);
            p->touch = aqk_rd(b, &o, 1); p->needs = aqk_rd(b, &o, 1); p->flags3 = aqk_rd(b, &o, 1);
            char *ds[11] = { p->pkg, p->name, p->summary, p->category, p->ver_inst, p->ver_new, p->ver_rel,
                             p->ver_distro, p->ver_src, p->licence, p->source };
            const unsigned cap[11] = { sizeof p->pkg, sizeof p->name, sizeof p->summary, sizeof p->category,
                                       sizeof p->ver_inst, sizeof p->ver_new, sizeof p->ver_rel, sizeof p->ver_distro,
                                       sizeof p->ver_src, sizeof p->licence, sizeof p->source };
            for (int k = 0; k < 11; k++) {
                unsigned j = 0;
                while (o < (unsigned)r && b[o]) { if (j < cap[k] - 1) ds[k][j++] = (char)b[o]; o++; }
                ds[k][j] = 0;
                o++;
            }
            p->icon = -1;                           /* (icons come with Linux: a tile with the initials until then) */
        }
        aq_cache.n = n;
        aq_cache.v3 = 1;
        aq_cache.repo_ok = 1;
        int rd = sh_blob_get("store.desc", b, AQK_MAX);   /* (b is free again: the descriptions, if kept) */
        for (int i = 0, o2 = 0; i < n && o2 < rd; i++) {
            unsigned j = 0;
            while (o2 < rd && b[o2]) { if (j < sizeof aq_cache.pkg[i].desc - 1) aq_cache.pkg[i].desc[j++] = (char)b[o2]; o2++; }
            aq_cache.pkg[i].desc[j] = 0;
            o2++;
        }
    }
    tk_free(b);
    return n > 0;
}
void sh_linux_store(const sh_lstore_t *s)
{
    if (s && s->n > 0) {                          /* the real table: it replaces the kept one, and is kept */
        int was = aq_cached;
        aq_cached = 0;
        LST = s;
        aqk_save(s);
        {   /* iOS: a success haptic when an install / update the user is watching ends, a warning when it fails */
            static char prev[SH_LPKGS];
            static int prev_n;
            for (int i = 0; i < s->n && i < SH_LPKGS; i++) {
                int st = (int)s->pkg[i].state, busy = st == SH_PKG_QUEUED || st == SH_PKG_INSTALLING;
                if (!was && i < prev_n && prev[i] && !busy && is_app("store") && !ap_pending)
                    sh_haptic_k(st == SH_PKG_FAILED ? SH_HAP_WARNING : SH_HAP_SUCCESS);
                prev[i] = (char)busy;
            }
            prev_n = s->n;
        }
        if (aq_pend.pkg[0] && st_find(aq_pend.pkg) >= 0) {   /* the GET asked for while Linux started */
            int i = st_find(aq_pend.pkg);
            aq_pend.pkg[0] = 0;
            if (!st_busy(&LST->pkg[i]) && sh_linux_pkg(i, SH_PKG_OPWORD(aq_pend.op, aq_pend.m)) < 0)
                toast("Linux didn't take the install: tap Get again");   /* (never silent: Track A, 8i) */
        }
        (void)was;
    } else if (!aq_cached) LST = s && s->n >= 0 ? s : 0;
    if (lx_baked && is_app("store") && !ap_pending) st_changed();
    if (aq_want[0] && is_app("store") && st_find(aq_want) >= 0) {   /* (Linux just started for it: its page now) */
        aq_push(st_find(aq_want));
        aq_want[0] = 0;
    }
}
/* cp6 / cp7 (Track B, Track A's list of dead ends): the home's Browser and Weather keys. The installed Linux app opens
 * (1); none: 2, the App Store opens on its page (GET). "browser": Firefox (or firefox-esr / chromium / epiphany-browser
 * when installed); "weather": GNOME Weather (needs the network for forecasts). Never blocks: opening the store only
 * asks for Linux (gwin starts it). */
static int st_route_app(const char *key)   /* 1 handled (a Linux app opened); 2 open the App Store instead (shell_lock.c app_open) */
{
    static const char *const BROWSERS[] = { "firefox", "firefox-esr", "chromium", "epiphany-browser", 0 };
    static const char *const WEATHER[] = { "gnome-weather", 0 };
    const char *const *ids = !tk_strcmp(key, "browser") ? BROWSERS : !tk_strcmp(key, "weather") ? WEATHER : 0;
    if (!ids) return 0;
    for (int k = 0; ids[k]; k++) {
        int i = st_find(ids[k]);
        if (i >= 0 && aq_inst(&LST->pkg[i]) && st_app(i) >= 0) { lx_open_app(st_app(i)); return 1; }
    }
    /* Track B, cp6: the caller opens the App Store as any app opens (its zoom, its page over frames); the page is
     * the app's from the open's build (st_build), or once the store's table arrives (sh_linux_store) */
    lx_scopy(aq_want, ids[0], sizeof aq_want);
    return 2;
}
/* an app's page pushed (the method preselected: the installed one, else the store's default) */
static void aq_push(int i)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    if (!AQ.app) { if (AQ.cat_on) AQ.scat = AQ.scroll; else AQ.sroot[AQ.tab] = AQ.scroll; }
    AQ.app = 1;
    lx_scopy(AQ.pkg, p->pkg, 40);
    AQ.pick = aq_inst(p) && p->method ? (int)p->method : p->def_method && (p->methods >> p->def_method & 1) ? (int)p->def_method : 0;
    if (!AQ.pick && p->methods) for (unsigned m = 1; m <= 3 && !AQ.pick; m++) if (p->methods >> m & 1) AQ.pick = (int)m;
    AQ.scroll = 0;
    AQ.kb = 0;
    AQ.opts = 0;                                  /* (Install Options shut on every page) */
    AQ.more = 0;
    aq_go(1);
}
static void aq_back(void)
{
    if (AQ.story) { aq_story_close(); return; }
    if (AQ.app) { AQ.app = 0; tk_bmp_free(&aq_dic); aq_dic_pkg[0] = 0; AQ.scroll = AQ.cat_on ? AQ.scat : AQ.sroot[AQ.tab]; }
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
    if (!AQ.app && !AQ.cat_on) AQ.sroot[AQ.tab] = AQ.scroll;
    AQ.tab = t;
    AQ.app = 0;
    AQ.cat_on = 0;                                /* (a tab opens at its root) */
    AQ.kb = 0;
    AQ.scroll = AQ.sroot[t];
    tk_bmp_free(&aq_dic);
    aq_dic_pkg[0] = 0;
    aq_go(0);
}

/* ---- actions ---- */
static int aq_op(int i, int op, int m)
{
    if (aq_cached) {                              /* Linux still starting: the GET waits for the real table */
        if (op == SH_PKG_OP_CANCEL) {             /* the waiting GET / UPDATE taken back */
            if (!tk_strcmp(aq_pend.pkg, LST->pkg[i].pkg)) aq_pend.pkg[0] = 0;
            aq_cache.pkg[i].state = 0;
            st_changed();
            return 0;
        }
        if (op == SH_PKG_OP_REMOVE) { toast("Linux is starting: try again in a moment"); return -1; }
        if (aq_pend.pkg[0] && st_find(aq_pend.pkg) >= 0) aq_cache.pkg[st_find(aq_pend.pkg)].state = 0;   /* (one waits) */
        lx_scopy(aq_pend.pkg, LST->pkg[i].pkg, sizeof aq_pend.pkg);
        aq_pend.op = op; aq_pend.m = m;
        aq_cache.pkg[i].state = SH_PKG_QUEUED;
        toast(op == SH_PKG_OP_INSTALL ? "It installs as soon as Linux has started" : "It updates as soon as Linux has started");
        st_changed();
        return 0;
    }
    if (sh_linux_pkg(i, SH_PKG_OPWORD(op, m)) < 0) { toast("Linux isn't running: try again in a moment"); return -1; }
    return 0;
}
/* GET / OPEN / UPDATE / RETRY / cancel; m: the method (0: the store's default) */
static const unsigned IS_ORD[3] = { SH_M_RELEASE, SH_M_SOURCE, SH_M_DISTRO };   /* the Install sheet's rows' order */
static int is_open(int i, unsigned m);
static void aq_act(int i, int m)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    if (p->state == SH_PKG_REMOVING) return;
    sh_haptic_k(SH_HAP_LIGHT);                    /* (iOS: a light impact on GET / OPEN / UPDATE / the ring) */
    if (st_busy(p)) { (void)aq_op(i, SH_PKG_OP_CANCEL, 0); return; }
    if (aq_upd(p)) { (void)aq_op(i, SH_PKG_OP_UPDATE, 0); return; }
    if (aq_inst(p)) {
        if (aq_cached) { toast("Linux is starting: it opens in a moment"); return; }
        int a = st_app(i);
        if (a >= 0) lx_open_app(a);
        else toast("Installed. It has no window of its own");
        return;
    }
    if (p->flags & SH_PKG_NO_SPACE) { toast("Not enough space on the memory card"); return; }
    {   /* the Install sheet: more than one way on the product page, or a source build anywhere (never by surprise) */
        int n = 0;
        for (int q = 0; q < 3; q++) n += (p->methods >> IS_ORD[q]) & 1;
        unsigned way = m ? (unsigned)m : p->def_method;
        if (!aq_cached && p->state != SH_PKG_FAILED && ((AQ.app && m && n > 1) || way == SH_M_SOURCE) && is_open(i, way) == 0) return;
    }
    if (aq_op(i, SH_PKG_OP_INSTALL, m) < 0) return;
    if (p->flags & SH_PKG_TOO_BIG) toast("It needs a lot of memory: it may run slowly");
}
/* ---- the Install sheet (spec 2.2.5: iOS's purchase sheet, ours the method choice): on Get when the app offers more
 * than one way on its page, or anywhere when its way is a source build. The kit's medium sheet: the app's line, one
 * row per way with a checkmark, a line on the chosen one, Install. ---- */
static struct { int pkg; unsigned m; float top; tk_bmp ic; } AIS = { -1, 0, 0, { 0, 0, 0, 0 } };
static const char *is_note(unsigned m)
{
    return m == SH_M_RELEASE ? "Downloads the developer's newest release." :
           m == SH_M_SOURCE ? "Builds the newest code on the big cores. Your phone stays usable but warm." :
                              "Installs Debian's tested package.";
}
static void is_draw(dc *d, float x, float y, float w)
{
    if (AIS.pkg < 0 || !LST || AIS.pkg >= aq_n()) return;
    const sh_lpkg_t *p = &LST->pkg[AIS.pkg];
    AIS.top = y;
    if (!AIS.ic.px) lx_make_icon(&AIS.ic, (int)(PT(60) + .5f), p->icon, p->name, 0);
    aq_blit(d, &AIS.ic, x, y + 4);
    d_text_box(d, TK_IT600, 17, x + 72, bl(TK_IT600, 17, y + 14), w - 72, 0, p->name, 0, 0, INK, 0);
    d_text_box(d, TK_IT400, 15, x + 72, bl(TK_IT400, 15, y + 37), w - 72, 0, p->summary, 0, 0, C_LABEL2, 0);
    float cy = y + 78;
    int n = 0;
    for (int q = 0; q < 3; q++) n += (p->methods >> IS_ORD[q]) & 1;
    d_rrect(d, x - 10, cy, w + 20, 56.0f * n, 20, tk_hex(0x2c2c2e, 1));
    int r = 0;
    for (int q = 0; q < 3; q++) {
        unsigned m = IS_ORD[q];
        if (!((p->methods >> m) & 1)) continue;
        float ry = cy + 56.0f * r;
        char l2[96], bm[8];
        const char *v = aq_mver(p, m);
        if (m == SH_M_SOURCE) { aq_dec(bm, (int)(p->build_min ? p->build_min : 10)); AQS(l2, "About ", bm, " min on this phone", v[0] ? " \xc2\xb7 " : "", v); }
        else if (m == SH_M_RELEASE) { char sz[16]; aq_dec(sz, (int)p->rel_size_mb); AQS(l2, v[0] ? v : "Newest", p->rel_size_mb ? " \xc2\xb7 " : "", p->rel_size_mb ? sz : "", p->rel_size_mb ? " MB" : ""); }
        else AQS(l2, v[0] ? v : "Debian", " \xc2\xb7 Debian 13");
        d_text(d, TK_IT400, 17, x + 6, bl(TK_IT400, 17, ry + 9), aq_mname(m), 0, 0, INK, 0);
        d_text_box(d, TK_IT400, 13, x + 6, bl(TK_IT400, 13, ry + 33), w - 50, 0, l2, 0, 0, C_LABEL2, 0);
        if (m == AIS.m) {                          /* the checkmark, systemBlue */
            float kx = x + w - 16, ky = ry + 28;
            d_seg(d, kx - 7, ky, kx - 2.5f, ky + 5, 2.4f, ACCENT());
            d_seg(d, kx - 2.5f, ky + 5, kx + 6, ky - 6, 2.4f, ACCENT());
        }
        if (++r < n) d_rrect(d, x + 6, ry + 55.5f, w, .5f, 0, W(.12f));
    }
    float fy = cy + 56.0f * n + 10;
    char ln[4][96];
    float wm;
    int k = wrapn(ln, 2, TK_IT400, 13, is_note(AIS.m), w, &wm);
    for (int j = 0; j < k; j++) d_text(d, TK_IT400, 13, x, bl(TK_IT400, 13, fy + 17.0f * j), ln[j], 0, 0, C_LABEL2, 0);
    float by = fy + 40;
    d_rrect(d, x, by, w, 50, 25, ACCENT());
    d_text_box(d, TK_IT600, 17, x + w / 2, bl(TK_IT600, 17, by + 15), w - 20, 1, "Install", 0, 0, W(1), 0);
}
static void is_done(void) { tk_bmp_free(&AIS.ic); AIS.pkg = -1; }
static void is_tap(float x, float y)
{
    if (AIS.pkg < 0 || !LST || AIS.pkg >= aq_n()) return;
    const sh_lpkg_t *p = &LST->pkg[AIS.pkg];
    float cy = AIS.top + 78;
    int n = 0;
    for (int q = 0; q < 3; q++) n += (p->methods >> IS_ORD[q]) & 1;
    if (y >= cy && y < cy + 56.0f * n) {          /* a way: the checkmark moves */
        int r = (int)((y - cy) / 56), k = 0;
        for (int q = 0; q < 3; q++) {
            unsigned m = IS_ORD[q];
            if (!((p->methods >> m) & 1)) continue;
            if (k++ == r && m != AIS.m) { AIS.m = m; sh_haptic_k(SH_HAP_SELECT); kit_sheet_redraw(); }
        }
        return;
    }
    char ln[4][96];
    float wm;
    float by = cy + 56.0f * n + 10 + 40;
    (void)ln; (void)wm;
    if (y >= by - 4 && y < by + 54 && x >= 6 && x < 384) {   /* Install: the sheet goes, the button starts waiting */
        int i = AIS.pkg;
        unsigned m = AIS.m;
        sh_haptic_k(SH_HAP_LIGHT);
        kit_dismiss(0);
        AQ.pick = (int)m;
        (void)aq_op(i, SH_PKG_OP_INSTALL, (int)m);
    }
}
/* what lights under the finger (kit_sheet_press, B's 295b517b): a way's row, or the Install capsule */
static int is_press(float x, float y, tk_rect *r, float *rad)
{
    if (AIS.pkg < 0 || !LST || AIS.pkg >= aq_n()) return 0;
    const sh_lpkg_t *p = &LST->pkg[AIS.pkg];
    float cy = AIS.top + 78;
    int n = 0;
    for (int q = 0; q < 3; q++) n += (p->methods >> IS_ORD[q]) & 1;
    if (y >= cy && y < cy + 56.0f * n && x >= 16 && x < 374) {
        int k = (int)((y - cy) / 56);
        *r = (tk_rect){ 16, cy + 56.0f * k, 358, 56 }; *rad = 0;
        return 1;
    }
    float by = cy + 56.0f * n + 50;
    if (y >= by - 4 && y < by + 54 && x >= 6 && x < 384) { *r = (tk_rect){ 26, by, 338, 50 }; *rad = 25; return 1; }
    return 0;
}
static int is_open(int i, unsigned m)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    int n = 0;
    for (int q = 0; q < 3; q++) n += (p->methods >> IS_ORD[q]) & 1;
    tk_bmp_free(&AIS.ic);
    AIS.pkg = i;
    AIS.m = m && (p->methods >> m & 1) ? m : p->def_method && (p->methods >> p->def_method & 1) ? p->def_method : IS_ORD[0];
    if (!((p->methods >> AIS.m) & 1)) for (int q = 0; q < 3; q++) if ((p->methods >> IS_ORD[q]) & 1) { AIS.m = IS_ORD[q]; break; }
    int rc = kit_sheet("Install", 78 + 56.0f * n + 10 + 40 + 56, is_draw, is_tap, is_done);
    if (rc == 0) kit_sheet_press(is_press);
    return rc;
}
/* ---- the Account sheet (spec 2.7, "Your Apps"): the owner's card, App Updates and Installed (each opens the App
 * Updates page), the memory card's free space, Linux's state ---- */
static float ACS_top;
static void acs_open_updates(void);
static void acs_row(dc *d, float x, float y, float w, int k, const char *t, const char *v, int chev, int sep)
{
    if (k == 0) {                                 /* App Updates: a blue tile with an arrow down */
        d_rrect(d, x, y + 11, 30, 30, 7, ACCENT());
        float cx = x + 15, cy = y + 26;
        d_seg(d, cx, cy - 7, cx, cy + 6, 2.2f, W(1));
        d_seg(d, cx - 5, cy + 1, cx, cy + 6, 2.2f, W(1));
        d_seg(d, cx + 5, cy + 1, cx, cy + 6, 2.2f, W(1));
    }
    float tx = k == 0 ? x + 42 : x;
    d_text(d, TK_IT400, 17, tx, bl(TK_IT400, 17, y + 15), t, 0, 0, INK, 0);
    d_text_box(d, TK_IT400, 17, x + w - (chev ? 20 : 0), bl(TK_IT400, 17, y + 15), 180, 2, v, 0, TK_TNUM, C_LABEL2, 0);
    if (chev) d_chev(d, x + w - 8, y + 26, W(.3f));
    if (sep) d_rrect(d, tx, y + 51.5f, x + w - tx, .5f, 0, W(.12f));
}
static void acs_draw(dc *d, float x, float y, float w)
{
    ACS_top = y;
    /* the owner's card: our disc and glyph (no name kept), the system and the phone */
    d_rrect(d, x, y + 4, 60, 60, 30, tk_hex(0x8e8e93, 1));
    d_rrect(d, x + 30 - 8, y + 4 + 12, 16, 16, 8, W(1));
    d_rrect(d, x + 30 - 15, y + 4 + 32, 30, 16, 8, W(1));
    d_text(d, TK_IT600, 20, x + 76, bl(TK_IT600, 20, y + 14), "Starbite OS", 0, 0, INK, 0);
    d_text(d, TK_IT400, 15, x + 76, bl(TK_IT400, 15, y + 42), "Galaxy S9 \xc2\xb7 Linux apps", 0, 0, C_LABEL2, 0);
    float gy = y + 84;
    d_rrect(d, x - 10, gy, w + 20, 52 * 4, 20, tk_hex(0x2c2c2e, 1));
    char nu[8], ni[8], fr[24], b[48];
    int inst = 0;
    for (int i = 0; i < aq_n(); i++) inst += aq_inst(&LST->pkg[i]);
    aq_dec(nu, aq_badge()); aq_dec(ni, inst);
    lx_fmt_kb(fr, LST ? LST->overlay_free_kb : 0);
    AQS(b, fr, " free");
    const char *ls = LX.state == SH_LINUX_RUNNING ? (aq_cached ? "Starting\xe2\x80\xa6" : "Running") : LX.state == SH_LINUX_STARTING ? "Starting\xe2\x80\xa6" : LX.state == SH_LINUX_FAILED ? "Didn't start" : "Stopped";
    acs_row(d, x, gy, w, 0, "App Updates", aq_badge() ? nu : "None", 1, 1);
    acs_row(d, x, gy + 52, w, 1, "Installed", ni, 1, 1);
    acs_row(d, x, gy + 104, w, 2, "Storage", b, 0, 1);
    acs_row(d, x, gy + 156, w, 3, "Linux", ls, 0, 0);
}
static void acs_tap(float x, float y)
{
    float gy = ACS_top + 84;
    (void)x;
    if (y >= gy && y < gy + 104) { sh_haptic_k(SH_HAP_LIGHT); kit_dismiss(0); acs_open_updates(); }   /* the updates and installed apps */
}
static int acs_press(float x, float y, tk_rect *r, float *rad)
{
    float gy = ACS_top + 84;
    if (y >= gy && y < gy + 104 && x >= 16 && x < 374) { *r = (tk_rect){ 16, gy + (y < gy + 52 ? 0 : 52), 358, 52 }; *rad = 0; return 1; }
    return 0;
}
static void acs_open_updates(void)
{
    if (!AQ.cat_on) AQ.sroot[AQ.tab] = AQ.scroll;
    AQ.cat_on = 1;
    lx_scopy(AQ.cat, AQ_ACCT, 16);
    AQ.scroll = 0;
    aq_go(1);
}
static int aq_rm_pkg = -1;                       /* the package the Remove sheet is about */
static void aq_rm_cb(int b)
{
    if (b == 0 && aq_rm_pkg >= 0 && aq_rm_pkg < aq_n()) (void)aq_op(aq_rm_pkg, SH_PKG_OP_REMOVE, 0);
    aq_rm_pkg = -1;
}
/* cp6 (the user's "no visual feedback on buttons"): what lights under the finger, for shell_linux.inc's lx_press (G_LINUX
 * touches never reach app_press): the same hit pass as aq_tap, with the matched element's rect; the *_press shape */
#define AQ_HAS_PRESS 1
static int aq_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *col)
{
    if (!is_app("store") || !LST) return 0;
    if ((AQ.app || AQ.cat_on) && !AQ.story && x < 20 && y > SAFE_TOP) return 0;   /* (a back swipe starts there: nothing lights) */
    if (!aqtg.hidden && y >= TB_Y && y < TB_Y + TB_H && x >= TB_X && x < TB_X + TB_W) {   /* spec 2.1.1: the slot lights */
        int k = tk_maxi(0, tk_mini(3, (int)((x - TB_X - 4) / TB_TABW)));
        *r = *g = (tk_rect){ TB_X + 4 + TB_TABW * k, TB_Y + 4, TB_TABW, TB_H - 8 };
        *rad = (TB_H - 8) / 2; *col = W(.1f);
        return 1;
    }
    if (AQ.kb && AQ.tab == AT_SEARCH && !AQ.app && y >= 548) return 0;          /* (the keyboard lights its own keys) */
    if (AQ.story && x >= CONTENT_R - 46 && y < SAFE_TOP + 46) {
        *r = *g = (tk_rect){ CONTENT_R - 34, SAFE_TOP + 2, 34, 34 }; *rad = 17; *col = W(.16f);
        return 1;
    }
    if ((AQ.app || AQ.cat_on) && BACK_HIT(x, y)) {
        *r = *g = (tk_rect){ BACK_X, SAFE_TOP, BACK_D, BACK_D }; *rad = BACK_D / 2; *col = W(.16f);
        return 1;
    }
    if (AQ.empty || AQ.hide || y < AQ.top || y >= AQ.bot) return 0;
    pctx c = { 0, 0, 0, x, y + AQ.scroll, -1, 0, -1, 0, { 0, 0, 0, 0 }, 0, 0 };
    aq_paint(&c);
    if (!c.hit || c.hr.w <= 0) return 0;
    *r = (tk_rect){ c.hr.x, c.hr.y - AQ.scroll, c.hr.w, c.hr.h };
    *g = (tk_rect){ 0, AQ.top, 390, AQ.bot - AQ.top };
    if (c.hrad > 0) {                              /* (the layer rounds its clip: a rounded element clips to itself, kept in view) */
        float y0 = tk_maxf(r->y, AQ.top), y1 = tk_minf(r->y + r->h, AQ.bot);
        if (y1 <= y0) return 0;
        *g = (tk_rect){ r->x, y0, r->w, y1 - y0 };
    }
    *rad = c.hrad;
    *col = c.hbtn ? W(.16f) : W(.1f);
    return 1;
}
static void aq_tap(float x, float y)
{
    if (!is_app("store")) return;
    int search = AQ.tab == AT_SEARCH && !AQ.app, kbup = AQ.kb && search;
    /* the tab bar (not while the keyboard is up) */
    if (!kbup && y >= TB_Y - 4 && y < TB_Y + TB_H + 6) {
        if (x >= TB_X && x < TB_X + TB_W) { sh_haptic_k(SH_HAP_SELECT); aq_tab(tk_maxi(0, tk_mini(3, (int)((x - TB_X - 4) / TB_TABW)))); return; }
        return;
    }
    if (AQ.empty || AQ.hide) return;              /* (a page still being drawn: nothing to tap yet) */
    if (AQ.story && x >= CONTENT_R - 46 && y < SAFE_TOP + 46) { sh_haptic_k(SH_HAP_LIGHT); aq_story_close(); return; }
    if ((AQ.app || AQ.cat_on) && BACK_HIT(x, y)) { aq_back(); return; }
    if (search && !AQ.cat_on && !AQ.kb && x >= ACCT_X - 6 && y >= ACCT_Y - 6 && y < ACCT_Y + ACCT_D + 6) {   /* Account */
        sh_haptic_k(SH_HAP_LIGHT);
        if (kit_sheet("Your Apps", 84 + 52 * 4 + 16, acs_draw, acs_tap, 0) < 0) acs_open_updates();
        else kit_sheet_press(acs_press);
        return;
    }
    if (search && !AQ.cat_on && y >= AQ_FY() - 6 && y < AQ_FY() + 50) {   /* the field, its clear, the cancel circle */
        if (AQ.kb && x >= 326) { AQ.kb = 0; AQ.q[0] = 0; AQ.sub = 0; AQ.scroll = 0; aq_go(0); return; }
        float fw = AQ.kb ? 306 : 358;
        if (AQ.q[0] && x >= 16 + fw - 40 && x < 16 + fw) {
            AQ.q[0] = 0; AQ.sub = 0;
            if (!AQ.kb) { AQ.kb = 1; AQ.shift = AQ.num = 0; lx_build_kb(1, 0, 0); }
            AQ.scroll = 0; aq_area(); aq_head(); aq_tabs(); aq_redraw_all(); aq_place(0); relink();
            return;
        }
        if (!AQ.kb) { AQ.kb = 1; AQ.sub = 0; AQ.shift = AQ.num = 0; AQ.scroll = 0; aq_area(); lx_build_kb(1, 0, 0); aq_head(); aq_tabs(); aq_redraw_all(); aq_place(0); relink(); }
        return;
    }
    if (kbup && y >= 548) {
        char ch = 0;
        int k = kb_hit(x, y, AQ.shift, AQ.num, &ch), n = (int)tk_strlen(AQ.q);
        if (!k) return;                           /* B3: shift / 123 are the keyboard's own (and its haptic on touch down) */
        if (k == 2) { AQ.shift = !AQ.shift; lx_build_kb(1, AQ.shift, AQ.num); relink(); return; }
        if (k == 3) { AQ.num = !AQ.num; lx_build_kb(1, AQ.shift, AQ.num); relink(); return; }
        if (k == 5) {                             /* Return ("search"): the results; the term into Recent */
            if (AQ.q[0]) { AQ.sub = 1; aq_rec_add(AQ.q); }
            AQ.kb = 0; AQ.scroll = 0; aq_area(); lx_build_kb(0, 0, 0); aq_head(); aq_tabs(); aq_redraw_all(); aq_place(0); relink();
            return;
        }
        AQ.sub = 0;                               /* (typing: completions again) */
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
    pctx c = { 0, 0, 0, x, y + AQ.scroll, -1, 0, -1, 0, { 0, 0, 0, 0 }, 0, 0 };
    aq_paint(&c);
    int i = c.hpkg;
    switch (c.hit) {
    case H_APP: sh_haptic(); if (AQ.story) { AQ.story = 0; ZS.phase = 0; } aq_push(i); break;
    case H_STORY: aq_story_open(i, (tk_rect){ c.hr.x, c.hr.y - AQ.scroll, c.hr.w, c.hr.h }); break;
    case H_ACT: aq_act(i, 0); break;
    case H_OPEN: { int a = st_app(i); if (a >= 0) lx_open_app(a); } break;
    case H_MAIN: aq_act(i, AQ.pick); break;
    case H_CAT:
        AQ.sroot[AQ.tab] = AQ.scroll;           /* (pushed in this tab, as iOS's See All) */
        AQ.cat_on = 1;
        lx_scopy(AQ.cat, c.harg >= 10 ? "" : AQ_CATS[c.harg], 16);
        AQ.scroll = 0;
        aq_go(1);
        break;
    case H_ACCT:                                  /* the account button: the "Your Apps" sheet (spec 2.7) */
        sh_haptic_k(SH_HAP_LIGHT);
        if (kit_sheet("Your Apps", 84 + 52 * 4 + 16, acs_draw, acs_tap, 0) < 0) acs_open_updates();
        else kit_sheet_press(acs_press);
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
    case H_MORE: AQ.more = 1; aq_area(); aq_redraw_all(); aq_place(0); break;
    case H_OPTS: AQ.opts = !AQ.opts; sh_haptic(); aq_area(); aq_redraw_all(); aq_place(0); break;
    case H_REMOVE: {                              /* B2's action sheet: one red row, a tap outside cancels */
        static const kit_btn B[1] = { { "Remove App", KIT_DESTRUCTIVE } };
        static char t[64];
        AQS(t, "Remove ", LST->pkg[i].name, "?");
        aq_rm_pkg = i;
        if (kit_actions(t, B, 1, aq_rm_cb) < 0) aq_rm_pkg = -1;
    } break;
    case H_SUGGEST: case H_TERM: case H_RECENT: {  /* a term: searched (the keyboard down, Recent kept) */
        const char *t = c.hit == H_SUGGEST ? AQ_SUGG[c.harg] : c.hit == H_TERM ? AQ_TERMS[c.harg] : aq_rec[c.harg];
        lx_scopy(AQ.q, t, 24);
        aq_rec_add(AQ.q);
        AQ.sub = 1; AQ.kb = 0; AQ.scroll = 0;
        lx_build_kb(0, 0, 0); aq_area(); aq_head(); aq_tabs(); aq_redraw_all(); aq_place(0); relink();
        break;
    }
    case H_RECCLR: sh_haptic_k(SH_HAP_SELECT); (void)sh_pref_set("store.recent", 0); aq_nrec = 0; aq_area(); aq_redraw_all(); aq_place(0); break;
    default: break;
    }
}
/* the finger on the page: a drag scrolls it (a rubber band past the ends, momentum after), a tap is aq_tap */
static void aq_back(void);
static int aq_touch(int ph, float x, float y)
{
    if (ph == 0 && (AQ.app || AQ.cat_on) && !AQ.story && x < 20 && y > SAFE_TOP) {   /* iOS's back swipe from the edge */
        AQ.edge = 1; AQ.ex0 = x; AQ.drag = 1; AQ.moved = 0; AQ.lt = now_ms; AQ.ly = x; AQ.pv = 0;
        return 1;
    }
    if (AQ.edge) {
        float dt = now_ms - AQ.lt;
        if (ph == 1) {
            if (dt > 0) AQ.pv = .6f * AQ.pv + .4f * ((x - AQ.ly) / dt * 1000);
            AQ.ly = x; AQ.lt = now_ms;
            AQ.px = tk_maxf(0, x - AQ.ex0);
            aq_place(0);
        } else if (ph == 2) {
            AQ.edge = 0; AQ.drag = 0;
            if (AQ.px > 130 || (AQ.pv > 500 && AQ.px > 20)) { AQ.px = 0; AQ.pv = 0; sh_haptic_k(SH_HAP_LIGHT); aq_back(); }   /* (pops: the page under slides in) */
            else AQ.pv = 0;                       /* (springs back: aq_tick) */
        }
        return 1;
    }
    if (ph == 0) {
        SS.hdrag = 0;
        SS.x0 = AQ.app && SS.n && y >= SS.y - AQ.scroll && y < SS.y - AQ.scroll + SS_H ? x : -1;   /* (in the carousel) */
        SS.sx0 = SS.x;
        AQ.drag = 1; AQ.moved = 0; AQ.y0 = y; AQ.s0 = AQ.scroll; AQ.v = 0; AQ.ly = SS.x0 >= 0 ? x : y; AQ.lt = now_ms;
        return 1;
    }
    if (!AQ.drag) return 0;
    if (SS.x0 >= 0 && !SS.hdrag && ph == 1 && tk_fabsf(x - SS.x0) > 8 && tk_fabsf(x - SS.x0) > tk_fabsf(y - AQ.y0)) SS.hdrag = 1;
    if (SS.hdrag) {                               /* the screenshots scroll sideways; on release they snap to a shot */
        float m = ss_max(), sx = SS.sx0 - (x - SS.x0), dt = now_ms - AQ.lt;
        if (sx < 0) sx *= .4f; else if (sx > m) sx = m + (sx - m) * .4f;
        if (ph == 1) {
            if (dt > 0) SS.v = .6f * SS.v + .4f * (-(x - AQ.ly) / dt * 1000);
            AQ.ly = x; AQ.lt = now_ms;
            SS.x = sx;
            aq_place(0);
        } else if (ph == 2) {
            float st = SS_W + SS_GAP, proj = SS.x + SS.v * .15f;
            SS.to = tk_clampf(tk_floorf(proj / st + .5f) * st, 0, m);
            SS.hdrag = 0; AQ.drag = 0; AQ.moved = 1;
        }
        return 1;
    }
    if (tk_fabsf(y - AQ.y0) > 6) AQ.moved = AQ.moved ? AQ.moved : 1 + (AQ.y0 >= AQ.top && AQ.y0 < AQ.bot && AQ.y0 < TB_Y);
    if (ph == 1 && AQ.moved == 2 && AQ.story && ZS.phase == 2 && AQ.s0 <= 0 && (y > AQ.y0 || ZS.drag > 0)) {
        ZS.drag = tk_maxf(0, y - AQ.y0);          /* iOS: at the story's top, a drag down shrinks it toward its card */
        AQ.scroll = 0;
        aq_place(0);
        return 1;
    }
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
    if (ph == 2 && ZS.drag > 0) {
        AQ.drag = 0;
        if (ZS.drag > 110) { ZS.drag = 0; aq_story_close(); }   /* (past the threshold: closes; else springs back) */
        return 1;
    }
    if (ph == 2) {
        AQ.drag = 0;
        if (!AQ.moved) { AQ.v = 0; aq_tap(x, y); }
        else if (AQ.moved != 2 || now_ms - AQ.lt > 80) AQ.v = 0;   /* held still before letting go: no flick */
    }
    return 1;
}
static int aq_spinning(void);
static int aq_moving(void)
{
    if (!is_app("store")) return 0;
    if (!aqbf.hidden) return 1;                   /* (the cold start's bar moves) */
    if (aq_spinning()) return 1;                  /* (a waiting spinner turns) */
    if (ZS.phase == 1 || ZS.phase == 3 || aqz.bmp.px || ZS.drag > 0) return 1;   /* (the story's zoom) */
    if (TP.set && (tk_fabsf(TP.x - TP.to) > .2f || tk_fabsf(TP.v) > 5)) return 1;   /* (the tab pill) */
    return AQ.drag || AQ.hide || AQ.left > 0 || AQ.px != 0 || tk_fabsf(AQ.v) > 1 || AQ.scroll < 0 || AQ.scroll > aq_max() + .5f;
}
/* the cold start's bar: elapsed / the expected start, held at 95 % until Linux is up; the words change once when late */
static int aq_bar(void)
{
    int on = is_app("store") && !ap_pending && AQ.empty && LX.state != SH_LINUX_RUNNING && LX.state != SH_LINUX_FAILED;
    if (!on) {
        if (aq_st0 > 0 && LX.state == SH_LINUX_RUNNING && now_ms > aq_st0)       /* this boot's real start: next time's pace */
            aq_st_exp = tk_clampf(now_ms - aq_st0, 5000, 90000);
        if (!AQ.empty || LX.state == SH_LINUX_RUNNING) aq_st0 = 0, aq_st_late = 0;
        aqbt.hidden = aqbf.hidden = 1;
        return 0;
    }
    if (aq_st0 <= 0) aq_st0 = now_ms;
    float el = now_ms - aq_st0, p = tk_clampf(el / aq_st_exp, .02f, .95f);
    if (!aq_st_late && el > aq_st_exp + 10000) { aq_st_late = 1; aq_head(); }
    tk_rect tr = { 95, aq_bar_y, 200, 5 }, fr = { 95, aq_bar_y, tk_maxf(5, 200 * p), 5 };
    hl_set(&aqbt, tr, tr, 2.5f, W(.16f));
    hl_set(&aqbf, fr, fr, 2.5f, ACCENT());
    return 1;
}
static void aq_go(int dir);
static void aq_z_face(void)
{
    relayer(&aqz);
    dc d = mk(&aqz, "store-zoom", ZS.card.x, ZS.card.y, CONTENT_W, CARD_H, 0, 0);
    aq_card_face(&d, ZS.pkg, ZS.card.x, ZS.card.y, 1, 0);
    aqz.hidden = 0;
}
/* the zoom's frame at t (0 the card, 1 the story's hero full width) */
static void aq_z_apply(void)
{
    float t = ZS.t, hh = CARD_H * HERO_K;
    tk_rect R = { ZS.card.x * (1 - t), ZS.card.y * (1 - t), CONTENT_W + (390 - CONTENT_W) * t, CARD_H + (hh - CARD_H) * t };
    float sc = R.w / CONTENT_W, rad = CARD_R + (32 - CARD_R) * tk_clampf(t, 0, 1);
    if (aqz.bmp.px) {
        aqz.sx = aqz.sy = sc; aqz.ox = aqz.oy = 0;
        aqz.x = PT(R.x) / sc + (aqz.bx - PT(ZS.card.x));
        aqz.y = PT(R.y) / sc + (aqz.by - PT(ZS.card.y));
        aqz.clip = 1;
        aqz.clip_r = (tk_rect){ PT(R.x), PT(R.y), PT(R.w), PT(R.h) };
        aqz.clip_radius = PT(rad);
    }
    tk_rect P = { R.x, R.y, R.w, CARD_H + (H_PT - CARD_H) * t };   /* the black page grows to the screen */
    hl_set(&aqzb, P, P, rad, tk_hex(0x000000, 1));
    float tb = 1 - tk_clampf(t, 0, 1);
    aqtb.alpha = aqtg.alpha = aqtp.alpha = tb;
}
static void aq_story_open(int i, tk_rect card)
{
    if (ZS.phase) return;
    ZS.pkg = i; ZS.card = card; ZS.t = 0; ZS.v = 0; ZS.phase = 1; ZS.drag = 0;
    ZS.sroot = AQ.scroll;
    aq_z_face();
    aq_z_apply();
    relink();
}
static void aq_story_close(void)
{
    if (ZS.phase != 2 || !AQ.story) return;
    ZS.pkg = AQ.story - 1;
    aq_z_face();
    ZS.t = 1; ZS.v = 0; ZS.phase = 3; ZS.drag = 0;
    AQ.story = 0;
    AQ.scroll = ZS.sroot;
    aq_tabs();
    aq_go(0);
    aq_z_apply();
    relink();
}
/* one frame of the zoom's spring (k = (2 pi / 0.45)^2, c = 4 pi 0.85 / 0.45, IOS-REFERENCE 16) */
static int aq_z_tick(float dt)
{
    if (!ZS.phase) return 0;
    if (ZS.phase == 1 || ZS.phase == 3) {
        float tgt = ZS.phase == 1 ? 1 : 0, k = 194.95f, cd = 23.74f;
        for (float left = dt; left > 0; left -= .004f) {
            float h = tk_minf(left, .004f);
            ZS.v += (k * (tgt - ZS.t) - cd * ZS.v) * h;
            ZS.t += ZS.v * h;
        }
        if (tk_fabsf(ZS.t - tgt) < .002f && tk_fabsf(ZS.v) < .05f) {
            ZS.t = tgt;
            if (ZS.phase == 1) {                  /* the story's own page now, drawn under the zoom's face */
                ZS.phase = 2;
                AQ.story = ZS.pkg + 1;
                AQ.scroll = 0;
                aq_tabs();
                aq_go(0);
            } else {
                ZS.phase = 0;
                relayer(&aqz); relayer(&aqzb);
                aqz.hidden = aqzb.hidden = 1;
                aqtb.alpha = aqtg.alpha = 1;
                relink();
                return 1;
            }
        }
        aq_z_apply();
        aq_place(0);
        return 1;
    }
    /* open: the face stays until the story's chunks are drawn; a drag down that didn't close springs back */
    if (aqz.bmp.px && !AQ.hide) { relayer(&aqz); relayer(&aqzb); aqz.hidden = aqzb.hidden = 1; relink(); }
    if (!AQ.drag && ZS.drag > 0) {
        ZS.drag *= tk_expf(-dt * 14);
        if (ZS.drag < .5f) ZS.drag = 0;
        aq_place(0);
        return 1;
    }
    return aqz.bmp.px != 0;
}
static int aq_spinning(void)
{
    for (int k = 0; k < AQ_NLIVE; k++)
        if (aql_it[k] >= 0 && aql_it[k] < aq_nl && LST && AQL[aql_it[k]].pkg < aq_n() && aq_waiting(&LST->pkg[AQL[aql_it[k]].pkg]))
            return 1;
    return 0;
}
static int aq_tick(float dt)
{
    if (aq_bar()) return 1;
    int zm = is_app("store") ? aq_z_tick(dt) : 0;   /* (the zoom moves; the page under it keeps drawing) */
    if (is_app("store") && aq_pill_tick(dt)) zm = 1;   /* (the tab pill slides) */
    if (is_app("store") && !ap_pending && ss_tick()) { aq_place(0); zm = 1; }   /* (the screenshots decode) */
    if (!SS.hdrag && SS.n && tk_fabsf(SS.x - SS.to) > .2f) {   /* the snap: response 0.35, damping 1 */
        for (float left = dt; left > 0; left -= .004f) {
            float h = tk_minf(left, .004f);
            SS.v += (322.3f * (SS.to - SS.x) - 35.9f * SS.v) * h;
            SS.x += SS.v * h;
        }
        if (tk_fabsf(SS.x - SS.to) < .3f && tk_fabsf(SS.v) < 20) { SS.x = SS.to; SS.v = 0; }
        aq_place(0);
        zm = 1;
    }
    if (!is_app("store") || ap_pending || AQ.empty) return 0;
    static float spin_t;
    if (aq_spinning() && now_ms - spin_t >= 33) {   /* the waiting spinners turn (~30 frames a second) */
        spin_t = now_ms;
        for (int k = 0; k < AQ_NLIVE; k++)
            if (aql_it[k] >= 0 && aql_it[k] < aq_nl && AQL[aql_it[k]].pkg < aq_n() && aq_waiting(&LST->pkg[AQL[aql_it[k]].pkg])) aql_dirty[k] = 1;
        aq_place(0);
    }
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
        if (!pend) { AQ.hide = 0; AQ.rev = 1; AQ.px = AQ.dir > 0 ? 390 : AQ.dir < 0 ? -117 : 0; AQ.pv = 0; aq_place(0); }
    } else {
        if (!AQ.edge && AQ.px != 0) {             /* the slide's spring: response 0.45, damping 1.0 (critical) */
            for (float left = dt; left > 0; left -= .004f) {
                float h = tk_minf(left, .004f);
                AQ.pv += (194.95f * -AQ.px - 27.93f * AQ.pv) * h;
                AQ.px += AQ.pv * h;
            }
            if (tk_fabsf(AQ.px) < .3f && tk_fabsf(AQ.pv) < 20) { AQ.px = 0; AQ.pv = 0; }
            aq_place(0);
        }
        pend = aq_place(1);                       /* (a chunk is the frame's work: ~20 ms of text on the Mac) */
    }
    return pend > 0 || aq_moving() || zm;
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
    relayer(&aqh); relayer(&aqtb); relayer(&aqtg); relayer(&aqbt); relayer(&aqbf); relayer(&aqit); relayer(&aqz); relayer(&aqzb);
    aqh.hidden = aqtb.hidden = aqtg.hidden = aqbt.hidden = aqbf.hidden = aqit.hidden = aqz.hidden = aqzb.hidden = 1;
    relayer(&aqtp); aqtp.hidden = 1; TP.set = 0;
    tk_memset(&ZS, 0, sizeof ZS);
    AQ.story = 0;
    ss_free();
    ss_nd = -1;                                   /* (the card's shots listed again at the next opening) */
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
    for (int k = 0; k < 3; k++) o[n++] = &aqss[k];
    o[n++] = &aqh;
    o[n++] = &aqit;
    o[n++] = &aqbt;
    o[n++] = &aqbf;
    o[n++] = &aqtb;
    o[n++] = &aqtp;
    o[n++] = &aqtg;
    o[n++] = &aqzb;
    o[n++] = &aqz;
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
        static const char *const TN[4] = { "today", "games", "apps", "search" };
        for (int t = 0; t < 4; t++) if (!tk_strcmp(key, TN[t])) { AQ.tab = t; AQ.app = AQ.cat_on = 0; AQ.scroll = 0; aq_still(); return 1; }
        if (!tk_strcmp(key, "browse")) { AQ.tab = AT_APPS; AQ.app = AQ.cat_on = 0; AQ.scroll = 0; aq_still(); return 1; }
        if (!tk_strcmp(key, "updates") || !tk_strcmp(key, "account")) {   /* (the old Updates tab: the Account page) */
            AQ.tab = AT_TODAY; AQ.app = 0; AQ.cat_on = 1; lx_scopy(AQ.cat, AQ_ACCT, 16); AQ.scroll = 0; aq_still(); return 1;
        }
        if (key[0] && st_find(key) >= 0) { aq_push(st_find(key)); aq_flush(); }
        return 1;
    }
    if (cat) {
        AQ.tab = AT_APPS; AQ.app = 0; AQ.cat_on = 1; AQ.scroll = 0;
        lx_scopy(AQ.cat, tk_strcmp(key, "All") ? key : "", 16);
        aq_tabs();
        aq_still();
        return 1;
    }
    if (q) {
        AQ.tab = AT_SEARCH; AQ.app = 0; AQ.scroll = 0;
        lx_scopy(AQ.q, key, 23);
        AQ.kb = v > 0;
        AQ.sub = !AQ.kb && AQ.q[0];               /* (the keyboard down: the results; up: the completions) */
        aq_still();
        return 1;
    }
    if (ap) { if (st_find(key) >= 0) { aq_push(st_find(key)); aq_flush(); } return 1; }
    if (sc) { aq_area(); AQ.scroll = tk_clampf(v, 0, aq_max()); aq_redraw_all(); aq_flush(); return 1; }
    if (pk && AQ.app) { AQ.pick = (int)v; AQ.opts = 1; aq_area(); aq_redraw_all(); aq_flush(); return 1; }
    return 1;
}
