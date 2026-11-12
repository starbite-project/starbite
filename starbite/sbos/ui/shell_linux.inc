/* SPDX-License-Identifier: MIT
 * Linux apps and the App Store for Starbite OS Next (docs/STARBITE-LINUX-APPS.md 5, 7, 8; milestones L2 + L3),
 * included by shell_lock.c after shell_media.inc. The platform (shell.h sh_linux_*: the kernel's
 * starbite/guest/next/gwin.c, the host's host/linux_fake.inc) runs Linux on the M3 cores. Here:
 *   - home page 3: the App Store, then the catalog's apps (each the guest's icon on a tile with a small Tux badge),
 *     on E's lattice (COL_CX, HOME_TOP2, ROW_PITCH); the Terminal stays on page 2 (it is the catalog's foot);
 *   - a Linux app's page: its window (the active window slot) from 44 to 772 pt, 540 with the keyboard up (a focused
 *     text field, or a KEYBOARD app); touches go to Linux, the keyboard's characters as text or keys. The window is
 *     four strip layers of 672 px rows (3.9 MiB each, filled through a row table), freed whenever the app closes;
 *     with the keyboard up the last strip is not kept. No page strips under it;
 *   - the App Store: a large-title page (its own layers, no strips: the header with the search field and the
 *     featured cards, the list in 2-row chunks re-used as it scrolls) and a pushed page per package (the page's
 *     strips, the glass back circle) with install / remove / cancel and progress.
 * shell_lock.c calls only the lx_* hooks at the end of this file (and the shell.h callbacks come here). ---- */
static struct { int state, got, first, shift, num, kb, text; unsigned flags; char why[64], id[40], name[32]; } LX;
static sh_lcat_t LCAT;                           /* the shell's copy of the catalog (kept while Linux is off) */
static const sh_lstore_t *LST;                   /* the platform's packages (sh_linux_store) */
static tk_layer aplin[4], aplst;
static tk_bmp lx_ic;                             /* the open Linux app's icon (the open and close springs) */
static int lx_pick = -1;                         /* the catalog index the next app_open of "linux" opens */
#define LHOME 19                                 /* page 3: the App Store and up to 19 apps (5 rows of 4) */
static int lx_baked;                            /* the home exists (catalog pushes before it wait) */
static struct { int n, cat[LHOME], ix[LHOME + 1], iy[LHOME + 1]; uint32_t sig; } LH;   /* ix, iy: each icon in hparts[5]'s px */
static int lx_ic_slot = -1;                      /* page 3's slot lx_ic was copied from (-1 none, -2 drawn itself) */
static tk_bmp store_ic;
static void st_build(void);
static void st_changed(void);
static int lx_term_id(const char *id) { return !tk_strcmp(id, SH_LINUX_TERMINAL) || !tk_strcmp(id, "foot"); }
static int lx_is_lin(void) { return is_app("terminal") || is_app("linux"); }
static int lx_cat_find(const char *id)
{
    for (int i = 0; i < LCAT.n; i++) if (!tk_strcmp(LCAT.app[i].id, id)) return i;
    if (lx_term_id(id)) for (int i = 0; i < LCAT.n; i++) if (lx_term_id(LCAT.app[i].id)) return i;
    return -1;
}
static int lx_app_index(const char *k) { for (int a = 0; a < NAPPS; a++) if (!tk_strcmp(APPS[a].k, k)) return a; return 0; }
static void lx_scopy(char *d, const char *s, int cap) { int k = 0; while (s[k] && k < cap - 1) { d[k] = s[k]; k++; } d[k] = 0; }
/* sizes as the store shows them: 850 KB, 72 MB, 1.4 GB (a no-break space: a wrapped note keeps them whole) */
static void lx_fmt_kb(char *b, unsigned kb)
{
    int n;
    if (kb >= 1000u * 1000u) {
        unsigned t = (kb + 50000u) / 100000u;            /* tenths of a GB */
        n = put_dec(b, (int)(t / 10)); b[n++] = '.'; b[n++] = (char)('0' + t % 10); tk_memcpy(b + n, "\xc2\xa0GB", 5);
    } else if (kb >= 1000u) { n = put_dec(b, (int)((kb + 500u) / 1000u)); tk_memcpy(b + n, "\xc2\xa0MB", 5); }
    else { n = put_dec(b, (int)kb); tk_memcpy(b + n, "\xc2\xa0KB", 5); }
}

/* ---- icons: a premultiplied bitmap scaled into another (an area average down, bilinear up) ---- */
static void lx_bmp_fit(tk_bmp *dst, const tk_bmp *src)
{
    if (!dst->px || !src->px) return;
    if (dst->w >= src->w) { tk_bmp_up(dst, src, 0, 0, (float)dst->w / (float)src->w); return; }
    float s = (float)src->w / (float)dst->w;
    for (int j = 0; j < dst->h; j++)
        for (int i = 0; i < dst->w; i++) {
            int x0 = (int)(i * s), x1 = tk_maxi(x0 + 1, (int)((i + 1) * s)), y0 = (int)(j * s), y1 = tk_maxi(y0 + 1, (int)((j + 1) * s));
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1 && y < src->h; y++)
                for (int x = x0; x < x1 && x < src->w; x++) {
                    uint32_t p = src->px[(size_t)y * src->stride + x];
                    a += p >> 24; r += p >> 16 & 255; g += p >> 8 & 255; b += p & 255; n++;
                }
            if (n) dst->px[(size_t)j * dst->stride + i] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
}
/* a Linux app's icon as the home draws icons: the pool's icon at 74 % on a dark rounded tile (iOS's 22 % corner), a
 * small Tux badge in the corner (Linux apps beside Starbite's own); no icon: the name's first letter */
static int lx_make_icon(tk_bmp *out, int px, int icon, const char *name, int badge)
{
    tk_bmp_free(out);
    if (!tk_bmp_new(out, px, px)) return 0;
    tk_stop s[2] = { { 0, tk_hex(0x3d4763, 1) }, { 1, tk_hex(0x1b2133, 1) } };
    tk_paint p = tk_linear(180, 0, 0, (float)px, (float)px, 2, s);
    tk_fill_rrect(out, 0, 0, (float)px, (float)px, px * .2237f, &p, 1);
    tk_box_shadow(out, 0, 0, (float)px, (float)px, px * .2237f, 0, px * .006f, 0, 0, W(.16f), 1);
    int done = 0;
    tk_bmp src = { SH_LINUX_ICON, SH_LINUX_ICON, SH_LINUX_ICON, 0 };
    if (icon >= 0 && (src.px = tk_alloc((size_t)SH_LINUX_ICON * SH_LINUX_ICON * 4)) && sh_linux_icon(icon, src.px)) {
        tk_bmp sc;
        int n = (int)(px * .74f + .5f);
        if (tk_bmp_new(&sc, n, n)) {
            lx_bmp_fit(&sc, &src);
            tk_blit(out, &sc, (px - n) / 2, (px - n) / 2, 1);
            tk_bmp_free(&sc);
            done = 1;
        }
    }
    if (src.px) tk_free(src.px);
    if (!done && name && name[0]) {
        char c[2] = { name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0 };
        float fs = px * .5f, w = tk_text_w(TK_IT700, fs, c, 0, 0);
        tk_paint wp = tk_solid(W(.92f));
        tk_text(out, TK_IT700, fs, (px - w) / 2, px * .5f + tk_ascent(TK_IT700, fs) * .36f, c, 0, 0, &wp, 0);
    }
    const tk_bmp *tux = badge ? tk_icon("t46-linux") : 0;
    if (tux) {
        tk_bmp b;
        int n = (int)(px * .3f + .5f), m = (int)(px * .035f);
        if (tk_bmp_new(&b, n, n)) {
            lx_bmp_fit(&b, tux);
            tk_box_shadow(out, (float)(px - n - m), (float)(px - n - m), (float)n, (float)n, n * .24f, 0, 0, n * .12f, 0, TK_RGBA(0, 0, 0, .45f), 0);
            tk_blit(out, &b, px - n - m, px - n - m, 1);
            tk_bmp_free(&b);
        }
    }
    return 1;
}
/* the App Store's icon: a blue tile with a white "A" of three strokes (round caps) */
static void lx_make_store_icon(tk_bmp *out, int px)
{
    tk_bmp_free(out);
    if (!tk_bmp_new(out, px, px)) return;
    tk_stop s[2] = { { 0, tk_hex(0x1cb4ff, 1) }, { 1, tk_hex(0x0a5ad6, 1) } };
    tk_paint p = tk_linear(180, 0, 0, (float)px, (float)px, 2, s);
    tk_fill_rrect(out, 0, 0, (float)px, (float)px, px * .2237f, &p, 1);
    tk_fm m;
    if (!tk_fm_new(&m, px, px)) return;
    static const float SEG[3][4] = { { .305f, .735f, .515f, .275f }, { .485f, .275f, .695f, .735f }, { .27f, .595f, .73f, .595f } };
    float hw = px * .042f;
    for (int j = 0; j < px; j++)
        for (int i = 0; i < px; i++) {
            float x = i + .5f, y = j + .5f, best = 1e9f;
            for (int k = 0; k < 3; k++) {
                float ax = SEG[k][0] * px, ay = SEG[k][1] * px, bx = SEG[k][2] * px, by = SEG[k][3] * px;
                float dx = bx - ax, dy = by - ay, t = tk_clampf(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0, 1);
                float ex = x - ax - t * dx, ey = y - ay - t * dy, dd = tk_sqrtf(ex * ex + ey * ey);
                if (dd < best) best = dd;
            }
            m.v[j * px + i] = tk_clampf(hw - best + .5f, 0, 1);
        }
    tk_paint wp = tk_solid(W(1));
    tk_draw_fm(out, &m, 0, 0, &wp, 1);
    tk_fm_free(&m);
}


/* ---- the home's third page: the App Store, then the catalog's apps (not the Terminal: page 2 has it) ---- */
static void lx_build_page3(void)
{
    if (!store_ic.px) lx_make_store_icon(&store_ic, (int)(PT(ICON_PT) + .5f));
    relayer(&hparts[5]);
    int n = 1 + LH.n, rows = (n + 3) / 4;
    dc d = mk(&hparts[5], "home-apps3", 0, HOME_TOP2 - 5, 390, ROW_PITCH * rows + 24, 4, 0);
    for (int i = 0; i < n; i++) {
        float cx = COL_CX(i % 4), y = HOME_TOP2 + ROW_PITCH * (i / 4);
        /* the icons are drawn once into the page (none kept: a press or an open copies its one back out) */
        tk_bmp t = { 0, 0, 0, 0 };
        if (i) lx_make_icon(&t, (int)(PT(ICON_PT) + .5f), LCAT.app[LH.cat[i - 1]].icon, LCAT.app[LH.cat[i - 1]].name, 1);
        const tk_bmp *ic = i ? &t : &store_ic;
        LH.ix[i] = (int)tk_floorf(PT(cx - ICON_PT / 2) - d.ox + .5f);
        LH.iy[i] = (int)tk_floorf(PT(y) - d.oy + .5f);
        if (ic->px) tk_blit(d.b, ic, LH.ix[i], LH.iy[i], 1);
        tk_bmp_free(&t);
        d_text_box(&d, TK_IT500, 12, cx, y + 68 + 12, 84, 1, i ? LCAT.app[LH.cat[i - 1]].name : "App Store", 0, 0, INK, 1);
    }
    hparts[5].x = hparts[5].bx = hparts[5].x + PT(780);   /* drawn as if on screen, then two pages right */
    /* P3s: cut to what it holds; the icons' places (copied out for a press or an open) move with the cut */
    float ox = hparts[5].x, oy = hparts[5].y;
    if (trim_layer(&hparts[5], 0, 0))
        for (int i = 0; i < n; i++) { LH.ix[i] -= (int)(hparts[5].x - ox); LH.iy[i] -= (int)(hparts[5].y - oy); }
}
/* the catalog's apps for page 3 again when what it shows changed (ids, names, icons; not the RUNNING flags) */
static void lx_page3_update(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < LCAT.n; i++) {
        const sh_lapp_t *a = &LCAT.app[i];
        for (const char *s = a->id; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
        for (const char *s = a->name; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
        h = (h ^ (uint32_t)a->icon) * 16777619u;
    }
    h = (h ^ (uint32_t)LCAT.n) * 16777619u;
    if (h == LH.sig && LH.sig) return;
    LH.sig = h;
    LH.n = 0;
    for (int i = 0; i < LCAT.n && LH.n < LHOME; i++)
        if (!lx_term_id(LCAT.app[i].id)) LH.cat[LH.n++] = i;
    if (lx_ic_slot >= 0 && !is_app("linux")) lx_ic_slot = -1;   /* (an open app keeps its icon) */
    lx_build_page3();
    relink();
}
/* page 3's icon of slot s (an app) copied out of the page into lx_ic (the press's and the open's): 1 done */
static int lx_slot_icon(int s)
{
    int n = (int)(PT(ICON_PT) + .5f), X = LH.ix[s + 1], Y = LH.iy[s + 1];
    if (lx_ic_slot == s && lx_ic.px) return 1;
    if (!hparts[5].bmp.px || X < 0 || Y < 0 || X + n > hparts[5].bmp.w || Y + n > hparts[5].bmp.h) return 0;
    if (lx_ic.w != n && (tk_bmp_free(&lx_ic), !tk_bmp_new(&lx_ic, n, n))) return 0;
    if (!lx_ic.px && !tk_bmp_new(&lx_ic, n, n)) return 0;
    tk_bmp_copy(&lx_ic, 0, 0, &hparts[5].bmp, X, Y, n, n);
    lx_ic_slot = s;
    return 1;
}
/* ---- the window: four strip layers of 672 bitmap rows (3.9 MiB each: no 15.5 MiB block in the UI heap), filled
 * through a row table; with the keyboard up the last strip is not kept (its rows are under the keyboard) ---- */
#define LSTRIP (2 * SH_LINUX_H / 4)
static uint32_t *lrow[2 * SH_LINUX_H];           /* the window's bitmap rows (a hidden strip's: lsink) */
static uint32_t lsink[2 * SH_LINUX_W] __attribute__((aligned(16)));
static tk_layer apkb;                            /* the keyboard (Linux apps, the App Store's search) */
static void lx_build_kb(int on, int shift, int num)
{
    static int drawn = -1;                       /* the layout in the bitmap (redrawn only when it changes) */
    int key = on ? 1 + 2 * !!shift + 4 * !!num : 0;
    if (on && key == drawn && apkb.bmp.px) { apkb.hidden = 0; return; }
    relayer(&apkb);
    apkb.hidden = 1;
    drawn = key;
    if (!on) return;
    dc d = mk(&apkb, "keyboard", 0, 540, 390, 262, 0, 0);
    kb_draw(&d, shift, num);
    apkb.hidden = 0;
}
static void lx_build_state(void)
{
    relayer(&aplst);
    aplst.hidden = 1;
    for (int k = 0; k < 4; k++) {
        aplin[k].hidden = LX.state != SH_LINUX_RUNNING || !aplin[k].bmp.px || !LX.got;
        aplin[k].clip = LX.kb;                   /* the keyboard up: the rows under it are not shown */
        aplin[k].clip_r = (tk_rect){ 0, SH_LINUX_Y, TK_W, 2.0f * SH_LINUX_HKB };
    }
    if (!lx_is_lin() || (LX.state == SH_LINUX_RUNNING && LX.got)) return;
    char t[64], l[96];
    const char *nm = LX.name[0] ? LX.name : "the app";
    if (LX.state == SH_LINUX_FAILED) { lx_scopy(t, "Linux Didn't Start", 64); lx_scopy(l, LX.why[0] ? LX.why : "The big cores did not start.", 96); }
    else if (LX.state == SH_LINUX_OFF || LX.state == SH_LINUX_STOPPING) {
        lx_scopy(t, LX.state == SH_LINUX_OFF ? "Linux Is Off" : "Linux Is Stopping", 64);
        lx_scopy(l, "Open the app again to start it.", 96);
    } else if (LX.state == SH_LINUX_STARTING) {
        lx_scopy(t, "Starting Linux", 64);
        int k = 0;
        for (const char *s = nm; *s && k < 40; s++) l[k++] = *s;
        lx_scopy(l + k, " opens on the big cores in a few seconds.", 96 - k);
    } else {
        tk_memcpy(t, "Opening ", 8);
        char e[40];
        ellipsize(e, sizeof e, TK_IT700, 22, nm, 250);
        lx_scopy(t + 8, e, 56);
        l[0] = 0;
    }
    float a0 = SAFE_TOP, a1 = LX.kb ? 540 : EMPTY_BOT, mid = tk_floorf((a0 + a1) / 2);
    dc d = mk(&aplst, "linux-state", 0, mid - 60, 390, 120, 0, 0);   /* the text's band only */
    d_empty(&d, a0, a1, t, l[0] ? l : 0);
    aplst.hidden = 0;
}
static void lx_build_win(void)
{
    for (int k = 0; k < 4; k++) {
        int want = k < 3 || !LX.kb;
        if (want && !aplin[k].bmp.px) {
            relayer(&aplin[k]);
            tk_layer_init(&aplin[k], "linux-window");
            if (!tk_bmp_new(&aplin[k].bmp, 2 * SH_LINUX_W, LSTRIP)) {
                LX.state = SH_LINUX_FAILED;
                lx_scopy(LX.why, "No memory for the window.", 64);
            }
            aplin[k].x = aplin[k].bx = 0;
            aplin[k].y = aplin[k].by = (float)(SH_LINUX_Y + LSTRIP * k);
            aplin[k].opaque = 1;                  /* the guest's pixels are opaque (alpha forced): nothing under */
            LX.first = 1;                         /* a new strip: every row again */
        } else if (!want && aplin[k].bmp.px) {
            relayer(&aplin[k]);
            aplin[k].hidden = 1;
        }
        for (int r = 0; r < LSTRIP; r++)
            lrow[LSTRIP * k + r] = aplin[k].bmp.px ? aplin[k].bmp.px + (size_t)r * aplin[k].bmp.stride : lsink;
    }
    lx_build_kb(LX.kb, LX.shift, LX.num);
    lx_build_state();
    relink();
    (void)sh_linux_frame();                       /* what the app has drawn so far */
}
int sh_linux_frame(void)
{
    if (!aplin[0].bmp.px || !lx_is_lin()) return 0;
    int y0 = -1, y1 = -1;
    if (sh_linux_blit(lrow, LX.first, &y0, &y1) <= 0) return 0;
    LX.first = 0;
    for (int k = 0; k < 4; k++)                   /* only the strips the new rows are in */
        if (aplin[k].bmp.px && (y0 < 0 || (y1 >= LSTRIP * k && y0 < LSTRIP * (k + 1)))) tk_layer_dirty(&aplin[k]);
    if (!LX.got) {
        LX.got = 1;
        lx_build_state();
    }
    return 1;
}
void sh_linux_state(int state, const char *why)
{
    int k = 0;
    if (!why) why = "";
    if (state == LX.state && !tk_strcmp(why, LX.why)) return;
    LX.state = state;
    while (why[k] && k < 63) { LX.why[k] = why[k]; k++; }
    LX.why[k] = 0;
    if (state != SH_LINUX_RUNNING) { LX.got = 0; LX.first = 1; }   /* a new guest: every row again */
    if (lx_is_lin() && !ap_pending) lx_build_state();
    if (is_app("store") && !ap_pending) st_changed();
}
/* the keyboard: up for a terminal (foot, Terminal=true apps) and while a text field has the focus; the guest's
 * viewport follows it (the app gets a configure with the new size) */
static void lx_kb_update(int rebuild)
{
    int kb = is_app("terminal") || (LX.flags & SH_LAPP_KEYBOARD) || LX.text;
    if (kb == LX.kb) return;
    LX.kb = kb;
    LX.shift = LX.num = 0;
    sh_linux_viewport(kb ? SH_LINUX_HKB : SH_LINUX_H);
    if (rebuild && !ap_pending) lx_build_win();
}
void sh_linux_textfocus(int on)
{
    LX.text = !!on;
    if (lx_is_lin()) lx_kb_update(1);
}
void sh_linux_catalog(const sh_lcat_t *c)
{
    /* the apps and their packages (the store's "Open"): the store redrawn only when these changed (the catalog
     * itself changes every ~2 s while apps run: their memory) */
    static uint32_t pkgsig;
    uint32_t h = 2166136261u;
    for (int i = 0; i < c->n && i < SH_LAPPS; i++) {
        for (const char *t = c->app[i].id; *t; t++) h = (h ^ (uint8_t)*t) * 16777619u;
        h = (h ^ (uint32_t)c->app[i].pkg) * 16777619u;
    }
    int pk = h != pkgsig;
    pkgsig = h;
    LCAT = *c;
    if (LCAT.n > SH_LAPPS) LCAT.n = SH_LAPPS;
    if (LCAT.n < 0) LCAT.n = 0;
    if (!lx_baked) return;
    lx_page3_update();
    if (lx_is_lin()) {
        int i = lx_cat_find(LX.id);
        if (i >= 0) { LX.flags = LCAT.app[i].flags; lx_kb_update(1); }
    }
    if (pk && is_app("store") && !ap_pending) st_changed();   /* "Open" follows what is installed */
}
static int lx_at(float x, float y)
{
    (void)x;
    return aplin[0].bmp.px && !aplin[0].hidden && PT(y) >= SH_LINUX_Y && PT(y) < SH_LINUX_Y + 2 * (LX.kb ? SH_LINUX_HKB : SH_LINUX_H);
}
static void lx_win_touch(int phase, float x, float y)
{
    int sx = (int)(PT(x) / 2), sy = (int)((PT(y) - SH_LINUX_Y) / 2), hh = LX.kb ? SH_LINUX_HKB : SH_LINUX_H;
    sx = sx < 0 ? 0 : sx >= SH_LINUX_W ? SH_LINUX_W - 1 : sx;
    sy = sy < 0 ? 0 : sy >= hh ? hh - 1 : sy;
    sh_linux_touch(phase, sx, sy);
}
static void lx_kb_tap(float x, float y)
{
    if (!LX.kb || y < 548) return;                /* the window's own touches went to Linux (G_LINUX) */
    char c = 0;
    int k = kb_hit(x, y, LX.shift, LX.num, &c);
    if (k) sh_haptic();
    if (k == 2) { LX.shift = !LX.shift; lx_build_kb(1, LX.shift, LX.num); relink(); }
    else if (k == 3) { LX.num = !LX.num; lx_build_kb(1, LX.shift, LX.num); relink(); }
    else if (k == 4) (void)sh_linux_char('\b');
    else if (k == 1 || k == 5) {
        (void)sh_linux_char(c);
        if (LX.shift && c >= 'A' && c <= 'Z') { LX.shift = 0; lx_build_kb(1, LX.shift, LX.num); relink(); }
    }
}
/* app_open of the Terminal or a Linux app (lx_pick): which app, its icon, the keyboard and the viewport first */
static void lx_prepare(int a)
{
    int term = !tk_strcmp(APPS[a].k, "terminal");
    int ci = term ? lx_cat_find(SH_LINUX_TERMINAL) : lx_pick;
    lx_pick = -1;
    if (!term && (ci < 0 || ci >= LCAT.n)) ci = 0;
    LX.shift = LX.num = LX.text = 0;
    LX.first = 1;
    LX.got = 0;
    lx_scopy(LX.id, term ? SH_LINUX_TERMINAL : ci < LCAT.n ? LCAT.app[ci].id : "", 40);
    lx_scopy(LX.name, term ? "Terminal" : ci < LCAT.n ? LCAT.app[ci].name : "", 32);
    LX.flags = ci >= 0 && ci < LCAT.n ? LCAT.app[ci].flags : SH_LAPP_KEYBOARD;
    LX.kb = -1;
    lx_kb_update(0);
    if (!term) {
        int s = -1;
        for (int k = 0; k < LH.n; k++) if (!tk_strcmp(LCAT.app[LH.cat[k]].id, LX.id)) s = k;
        if (s < 0 || !lx_slot_icon(s)) {
            lx_make_icon(&lx_ic, (int)(PT(ICON_PT) + .5f), ci < LCAT.n ? LCAT.app[ci].icon : -1, LX.name, 1);
            lx_ic_slot = -2;
        }
    }
}

/* ---- the App Store ---- */
/* the list is drawn in chunks of two rows (64 pt each), five layers re-used as it scrolls (a whole index never
 * lives in the heap: 2.5 MiB a chunk) */
#define ST_CH 5
#define ST_CHR 2
#define DT PUSHED_TOP                            /* a package's page: its content under the nav row */
#define FIELD_Y CONTENT_TOP                      /* the search field, under the large title */
#define FEAT_W ((CONTENT_W - 18) / 3)
#define ROW_BTN (CONTENT_R - 68)                 /* a row's GET / OPEN */
static struct {
    int det, kb, shift, num, n, rows[SH_LPKGS], nfeat, feat[3], rm_arm, dkey, chunk[ST_CH];
    float scroll, top, bot, rm_y, rm_t;
    char q[24], pkg[40];
} SS;
static tk_layer apsth, apstl[ST_CH], apstb;      /* the list's head, its rows' chunks (scrolled, clipped), the detail's button */
static tk_bmp st_dic;                            /* the detail's 100 pt icon */
static tk_bmp st_field[2];                       /* the search field's glass (full width, beside Cancel): baked once
                                                  * per opening in the page's first build, copied at each key */
static struct { int icon; uint32_t key; tk_bmp b; } SIC[24];   /* the packages' 48 pt icons */
static int st_find(const char *pkg)
{
    if (!LST) return -1;
    for (int i = 0; i < LST->n && i < SH_LPKGS; i++) if (!tk_strcmp(LST->pkg[i].pkg, pkg)) return i;
    return -1;
}
static int st_busy(const sh_lpkg_t *p) { return p->state == SH_PKG_QUEUED || p->state == SH_PKG_INSTALLING || p->state == SH_PKG_REMOVING; }
static const tk_bmp *st_icon(int i)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    uint32_t key = 2166136261u;
    for (const char *s = p->pkg; *s; s++) key = (key ^ (uint8_t)*s) * 16777619u;
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
static void st_icons_free(void)
{
    for (int k = 0; k < 24; k++) tk_bmp_free(&SIC[k].b);
    tk_bmp_free(&st_dic);
    tk_bmp_free(&st_field[0]);
    tk_bmp_free(&st_field[1]);
}
/* the catalog app a package brought (its "Open"), or -1 */
static int st_app(int i)
{
    for (int a = 0; a < LCAT.n; a++) if (LCAT.app[a].pkg == i) return a;
    return -1;
}
static void st_filter(void)
{
    SS.n = SS.nfeat = 0;
    if (!LST) return;
    for (int i = 0; i < LST->n && i < SH_LPKGS; i++) {
        const sh_lpkg_t *p = &LST->pkg[i];
        if (!SS.q[0] && (p->flags & SH_PKG_FEATURED) && SS.nfeat < 3) SS.feat[SS.nfeat++] = i;
        if (SS.q[0] && !ci_has(p->name, SS.q) && !ci_has(p->pkg, SS.q) && !ci_has(p->summary, SS.q) && !ci_has(p->category, SS.q)) continue;
        SS.rows[SS.n++] = i;
    }
}
static void st_line(const sh_lpkg_t *p, char *b, tk_col *c)
{
    char s[16];
    int n;
    *c = W(.55f);
    if (p->state == SH_PKG_QUEUED) { lx_scopy(b, "Waiting to install", 48); return; }
    if (p->state == SH_PKG_INSTALLING || p->state == SH_PKG_REMOVING) {
        lx_scopy(b, p->state == SH_PKG_INSTALLING ? "Installing \xc2\xb7 " : "Removing \xc2\xb7 ", 48);
        n = (int)tk_strlen(b);
        n += put_dec(b + n, (int)tk_mini((int)p->pct, 100));
        tk_memcpy(b + n, " %", 3);
        *c = ACCENT();
        return;
    }
    if (p->state == SH_PKG_FAILED) { lx_scopy(b, p->err[0] ? p->err : "Didn't install", 48); *c = tk_hex(0xff6961, 1); return; }
    lx_scopy(b, p->category[0] ? p->category : "Linux", 48);
    n = (int)tk_strlen(b);
    tk_memcpy(b + n, " \xc2\xb7 ", 4);
    n += 4;
    if (p->flags & SH_PKG_INSTALLED) lx_scopy(b + n, "Installed", 48 - n);
    else { lx_fmt_kb(s, p->dl_kb); lx_scopy(b + n, s, 48 - n); }
}
/* the row's button (68 x 30 at x, y): GET / OPEN / RETRY, or a progress ring with a stop square */
static void st_button(dc *d, float x, float y, const sh_lpkg_t *p)
{
    if (st_busy(p)) {
        float cx = x + 52, cy = y + 15;
        d_ring(d, cx, cy, 13, 3, 1, W(.18f), 0);
        float f = p->state == SH_PKG_QUEUED ? .08f : tk_clampf(p->pct / 100.0f, .02f, 1);
        d_ring(d, cx, cy, 13, 3, f, ACCENT(), 1);
        if (p->state != SH_PKG_REMOVING) d_rrect(d, cx - 4, cy - 4, 8, 8, 1.5f, ACCENT());
        return;
    }
    int inst = (p->flags & SH_PKG_INSTALLED) != 0, dim = !inst && (p->flags & SH_PKG_NO_SPACE);
    const char *t = inst ? "OPEN" : p->state == SH_PKG_FAILED ? "RETRY" : "GET";
    d_rrect(d, x, y, 68, 30, 15, W(.14f));
    float w = tw(TK_IT700, 15, t, .3f, 0);
    tk_col c = ACCENT();
    if (dim) c.a = .35f;
    d_text(d, TK_IT700, 15, x + (68 - w) / 2, bl(TK_IT700, 15, y + 6), t, .3f, 0, c, 0);
}
static void st_chunk_draw(int L, int c)
{
    relayer(&apstl[L]);
    apstl[L].hidden = 1;
    SS.chunk[L] = c;
    int r0 = c * ST_CHR, r1 = tk_mini(SS.n, r0 + ST_CHR);
    if (c < 0 || r0 >= r1) return;
    dc d = mk(&apstl[L], "store-rows", MARGIN, SS.top + 64.0f * r0, CONTENT_W, 64.0f * (r1 - r0), 0, 0);
    for (int k = r0; k < r1; k++) {
        const sh_lpkg_t *p = &LST->pkg[SS.rows[k]];
        float y = SS.top + 64.0f * k;
        const tk_bmp *ic = st_icon(SS.rows[k]);
        if (ic->px) tk_blit(d.b, ic, (int)tk_floorf(PT(MARGIN) - d.ox + .5f), (int)tk_floorf(PT(y + 8) - d.oy + .5f), 1);
        d_text_box(&d, TK_IT600, 16, MARGIN + 60, bl(TK_IT600, 16, y + 12), ROW_BTN - 8 - MARGIN - 60, 0, p->name, 0, 0, INK, 0);
        char ln[48];
        tk_col col;
        st_line(p, ln, &col);
        d_text_box(&d, TK_IT400, 13, MARGIN + 60, bl(TK_IT400, 13, y + 35), ROW_BTN - 8 - MARGIN - 60, 0, ln, 0, 0, col, 0);
        st_button(&d, ROW_BTN, y + 17, p);
        if (k + 1 < SS.n) d_rrect(&d, MARGIN + 60, y + 63.5f, CONTENT_R - MARGIN - 60, .5f, 0, W(.1f));
    }
    apstl[L].hidden = 0;
}
/* the chunks the list shows at its scroll: drawn where missing (re-using the ones out of view), moved and clipped */
static void st_place(void)
{
    float h = 64.0f * SS.n, area = SS.bot - SS.top, m = tk_maxf(0, h - area);
    SS.scroll = tk_clampf(SS.scroll, 0, m);
    int c0 = (int)(SS.scroll / (64 * ST_CHR)), c1 = (int)((SS.scroll + area) / (64 * ST_CHR));
    if (c1 > c0 + ST_CH - 1) c1 = c0 + ST_CH - 1;
    for (int c = c0; c <= c1 && c * ST_CHR < SS.n; c++) {
        int have = -1, spare = -1;
        for (int L = 0; L < ST_CH; L++) {
            if (SS.chunk[L] == c && apstl[L].bmp.px) have = L;
            else if (spare < 0 && (SS.chunk[L] < c0 || SS.chunk[L] > c1 || !apstl[L].bmp.px)) spare = L;
        }
        if (have < 0 && spare >= 0) st_chunk_draw(spare, c);
    }
    for (int L = 0; L < ST_CH; L++) {
        tk_layer *l = &apstl[L];
        if (!l->bmp.px) continue;
        l->hidden = SS.chunk[L] < c0 || SS.chunk[L] > c1;
        l->y = l->by - tk_floorf(PT(SS.scroll) + .5f);
        l->clip = 1;
        l->clip_r = (tk_rect){ 0, PT(SS.top), TK_W, PT(area) };
        l->fade_top = SS.scroll > 0 ? PT(12) : 0;   /* the rows fade out at the clip's edges */
        l->fade_bot = h - SS.scroll > area + 1 ? PT(28) : 0;
    }
    relink();
}
static void st_head(void)
{
    relayer(&apsth);
    relayer(&aplst);                              /* (the Linux page's state line: the empty store's words here) */
    apsth.hidden = aplst.hidden = 1;
    int empty = !LST || LST->n <= 0;
    float bottom = empty ? CONTENT_TOP : SS.top;
    dc d = mk(&apsth, "store-head", 0, SAFE_TOP, 390, bottom - SAFE_TOP, 0, 0);
    if (!empty) {
        /* the field's glass, baked in the page's first build (not in a tap) and copied in (it overwrites: first) */
        int k = SS.kb != 0;
        float fw = k ? CONTENT_W - 74 : CONTENT_W;
        for (int f = 0; f < 2; f++)
            if (!st_field[f].px && tk_bmp_new(&st_field[f], d.b->w, (int)(PT(FIELD_Y + 36 + 16) - d.oy))) {
                dc g = { &st_field[f], 0, d.ox, d.oy };
                d_lg_flat(&g, MARGIN, FIELD_Y, f ? CONTENT_W - 74 : CONTENT_W, 36, 18, 1);
            }
        if (st_field[k].px) tk_bmp_copy(d.b, 0, 0, &st_field[k], 0, 0, st_field[k].w, tk_mini(st_field[k].h, d.b->h));
        d_icon(&d, "search15", MARGIN + 13, FIELD_Y + 10.5f, 1);
        float tx = MARGIN + 37;
        if (SS.q[0]) {
            d_text_box(&d, TK_IT400, 17, tx, bl(TK_IT400, 17, FIELD_Y + 7), fw - 50, 0, SS.q, 0, 0, INK, 0);
            if (SS.kb) d_rrect(&d, tx + tk_minf(tw(TK_IT400, 17, SS.q, 0, 0), fw - 50) + 1, FIELD_Y + 8, 2, 21, 1, ACCENT());
        } else
            d_text(&d, TK_IT400, 17, tx, bl(TK_IT400, 17, FIELD_Y + 7), "Apps and Packages", 0, 0, tk_hex(0xa9a9a9, 1), 0);
        if (SS.kb) d_text_box(&d, TK_IT400, 17, CONTENT_R, bl(TK_IT400, 17, FIELD_Y + 7), 64, 2, "Cancel", 0, 0, ACCENT(), 0);
    }
    d_h1(&d, "App Store");
    if (LST && LST->repo_ok) {
        char f[24], b[40];
        lx_fmt_kb(f, LST->overlay_free_kb);
        lx_scopy(b, f, 40);
        lx_scopy(b + tk_strlen(b), " free", 40 - (int)tk_strlen(b));
        d_text_box(&d, TK_IT400, 13, CONTENT_R, LARGE_BASE - 4, 120, 2, b, 0, 0, W(.55f), 0);
    }
    if (empty) {
        const char *t, *l;
        if (LX.state == SH_LINUX_FAILED) { t = "Linux Didn't Start"; l = LX.why[0] ? LX.why : "The big cores did not start."; }
        else if (LX.state != SH_LINUX_RUNNING) { t = "Starting Linux"; l = "The App Store opens when Linux is up, in a few seconds."; }
        else if (!LST) { t = "Reading the Index"; l = "The package list comes from the memory card."; }
        else { t = "No Packages"; l = LST->repo_ok ? "The package index is empty." : "The package source isn't on the memory card yet."; }
        float mid = tk_floorf((CONTENT_TOP + EMPTY_BOT) / 2);
        dc e = mk(&aplst, "store-empty", 0, mid - 60, 390, 120, 0, 0);
        d_empty(&e, CONTENT_TOP, EMPTY_BOT, t, l);
        aplst.hidden = 0;
        apsth.hidden = 0;
        return;
    }
    int searching = SS.q[0] || SS.kb;
    if (!searching && SS.nfeat) {
        d_text(&d, TK_IT600, 13, ROW_TX, bl(TK_IT600, 13, FIELD_Y + 52), "FEATURED", .4f, 0, W(.55f), 0);
        for (int k = 0; k < SS.nfeat; k++) {
            const sh_lpkg_t *p = &LST->pkg[SS.feat[k]];
            float x = MARGIN + (FEAT_W + 9) * k, cx = x + FEAT_W / 2, y = FIELD_Y + 70;
            d_rrect(&d, x, y, FEAT_W, 128, 16, tk_hex(0x151c33, 1));
            const tk_bmp *ic = st_icon(SS.feat[k]);
            if (ic->px) tk_blit(d.b, ic, (int)tk_floorf(PT(cx - 24) - d.ox + .5f), (int)tk_floorf(PT(y + 14) - d.oy + .5f), 1);
            d_text_box(&d, TK_IT600, 14, cx, bl(TK_IT600, 14, y + 72), FEAT_W - 12, 1, p->name, 0, 0, INK, 0);
            d_text_box(&d, TK_IT400, 12, cx, bl(TK_IT400, 12, y + 94), FEAT_W - 12, 1, p->category, 0, 0, W(.55f), 0);
        }
    }
    const char *cap = !SS.n ? "NO RESULTS" : searching ? "RESULTS" : "ALL APPS";
    d_text(&d, TK_IT600, 13, ROW_TX, bl(TK_IT600, 13, SS.top - 22), cap, .4f, 0, W(.55f), 0);
    apsth.hidden = 0;
}
static void st_list(void)
{
    for (int L = 0; L < ST_CH; L++) { relayer(&apstl[L]); apstl[L].hidden = 1; SS.chunk[L] = -1; }
    if (!LST || !SS.n) return;
    st_place();
}
/* the detail's button and progress (rebuilt alone while only the progress moves) */
static void st_btn(void)
{
    relayer(&apstb);
    apstb.hidden = 1;
    int i = st_find(SS.pkg);
    if (!SS.det || i < 0) return;
    const sh_lpkg_t *p = &LST->pkg[i];
    dc d = mk(&apstb, "store-button", 136, DT + 62, CONTENT_R - 136, 50, 0, 0);
    int inst = (p->flags & SH_PKG_INSTALLED) != 0, busy = st_busy(p), dim = !inst && !busy && (p->flags & SH_PKG_NO_SPACE);
    float bx = 136;
    if (p->state != SH_PKG_REMOVING) {
        const char *t = busy ? "CANCEL" : inst ? "OPEN" : p->state == SH_PKG_FAILED ? "RETRY" : "GET";
        tk_col fill = busy ? W(.14f) : ACCENT(), fg = busy ? ACCENT() : W(1);
        if (dim) { fill = W(.1f); fg = W(.35f); }
        d_rrect(&d, 136, DT + 68, 84, 32, 16, fill);
        d_text_box(&d, TK_IT700, 15, 136 + 42, bl(TK_IT700, 15, DT + 75), 80, 1, t, .3f, 0, fg, 0);
        bx = 232;
    }
    if (busy) {
        float bw = CONTENT_R - bx, f = p->state == SH_PKG_QUEUED ? 0 : tk_clampf(p->pct / 100.0f, 0, 1);
        d_rrect(&d, bx, DT + 72, bw, 6, 3, W(.15f));
        if (f > 0) d_rrect(&d, bx, DT + 72, tk_maxf(6, bw * f), 6, 3, ACCENT());
        char ln[48];
        tk_col c;
        st_line(p, ln, &c);
        d_text_box(&d, TK_IT400, 13, bx, bl(TK_IT400, 13, DT + 85), bw, 0, ln, 0, 0, W(.6f), 0);
    } else if (p->state == SH_PKG_FAILED)
        d_text_box(&d, TK_IT600, 13, 232, bl(TK_IT600, 13, DT + 76), CONTENT_R - 232, 0, "Didn't install", 0, 0, tk_hex(0xff6961, 1), 0);
    else if (inst)
        d_text_box(&d, TK_IT400, 13, 232, bl(TK_IT400, 13, DT + 76), CONTENT_R - 232, 0, (p->flags & SH_PKG_SYSTEM) ? "Part of the system" : "Installed", 0, 0, W(.55f), 0);
    else if (dim)
        d_text_box(&d, TK_IT600, 13, 232, bl(TK_IT600, 13, DT + 76), CONTENT_R - 232, 0, "Not enough space", 0, 0, tk_hex(0xff6961, 1), 0);
    apstb.hidden = 0;
}
/* a coloured note (a warning) at y: its title and wrapped lines; returns the y after it */
static float st_note(dc *d, float y, uint32_t col, const char *title, const char *text)
{
    char ln[4][96];
    float wm;
    int n = wrap(ln, TK_IT400, 14, text, GROUP_W - 32, &wm);
    float h = 14 + 19 + 19.0f * n + 10;
    d_rrect(d, GROUP_X, y, GROUP_W, h, 16, tk_hex(col, .16f));
    d_text_box(d, TK_IT600, 15, ROW_TX, bl(TK_IT600, 15, y + 11), GROUP_W - 32, 0, title, 0, 0, tk_hex(col, 1), 0);
    for (int k = 0; k < n; k++) d_text(d, TK_IT400, 14, ROW_TX, bl(TK_IT400, 14, y + 33 + 19.0f * k), ln[k], 0, 0, W(.85f), 0);
    return y + h + 12;
}
static void st_page(dc *d)
{
    int i = SS.det ? st_find(SS.pkg) : -1;
    if (!SS.det || i < 0) return;                 /* the list: its own layers (st_head, the chunks) */
    const sh_lpkg_t *p = &LST->pkg[i];
    char b[96], s[24];
    (void)b;
    d_back(d, ACCENT());                          /* P3p's pushed page: the glass back circle */
    if (st_dic.px) tk_blit(d->b, &st_dic, (int)tk_floorf(PT(MARGIN) - d->ox + .5f), (int)tk_floorf(PT(DT) - d->oy + .5f), 1);
    d_text_box(d, TK_IT700, 24, 136, bl(TK_IT700, 24, DT + 2), CONTENT_R - 136, 0, p->name, 0, 0, INK, 0);
    d_text_box(d, TK_IT400, 15, 136, bl(TK_IT400, 15, DT + 34), CONTENT_R - 136, 0, p->pkg, 0, 0, W(.55f), 0);
    float y = DT + 124;
    int inst = (p->flags & SH_PKG_INSTALLED) != 0;
    if (p->flags & SH_PKG_TOO_BIG) {
        char t[200], m[16];
        lx_fmt_kb(s, p->ram_mb * 1000u);
        lx_fmt_kb(m, LST->mem_total_kb);
        int n = 0;
        const char *parts[] = { "About ", s, " of memory while in use; Linux has ", m[0] != '0' ? m : "1.5 GB",
                                " in all. It may run slowly, and other Linux apps may be closed while it runs." };
        for (unsigned k = 0; k < 5; k++) for (const char *c = parts[k]; *c && n < 198; c++) t[n++] = *c;
        t[n] = 0;
        y = st_note(d, y, 0xffb340, "Needs a lot of memory", t);
    }
    if ((p->flags & SH_PKG_NO_SPACE) && !inst) {
        char t[200], f[16];
        lx_fmt_kb(s, p->inst_kb);
        lx_fmt_kb(f, LST->overlay_free_kb);
        int n = 0;
        const char *parts[] = { "It needs ", s, " on the memory card and ", f, " is free. Remove an app to make room." };
        for (unsigned k = 0; k < 5; k++) for (const char *c = parts[k]; *c && n < 198; c++) t[n++] = *c;
        t[n] = 0;
        y = st_note(d, y, 0xff6961, "Not enough space", t);
    }
    if (p->state == SH_PKG_FAILED) y = st_note(d, y, 0xff6961, "Didn't install", p->err[0] ? p->err : "The package manager stopped with an error.");
    {
        char ln[4][96];
        float wm;
        int n = wrapn(ln, 3, TK_IT400, 16, p->summary[0] ? p->summary : p->name, CONTENT_W, &wm);
        for (int k = 0; k < n; k++) d_text(d, TK_IT400, 16, MARGIN, bl(TK_IT400, 16, y + 2 + 22.0f * k), ln[k], 0, 0, INK, 0);
        y += 22.0f * tk_mini(n, 3) + 16;
    }
    static const char *const L[5] = { "Category", "Download", "Installed size", "Memory in use", "Free space" };
    d_rrect(d, GROUP_X, y, GROUP_W, 47 * 5 - 1, 16, tk_hex(0x151c33, 1));
    for (int r = 0; r < 5; r++) {
        float ry = y + 47.0f * r;
        if (r == 0) lx_scopy(s, p->category[0] ? p->category : "Linux", 24);
        else if (r == 1) lx_fmt_kb(s, p->dl_kb);
        else if (r == 2) lx_fmt_kb(s, p->inst_kb);
        else if (r == 3) { char m[16]; lx_fmt_kb(m, p->ram_mb * 1000u); lx_scopy(s, "about ", 24); lx_scopy(s + 6, m, 18); }
        else lx_fmt_kb(s, LST->overlay_free_kb);
        d_text(d, TK_IT400, 16, ROW_TX, ry + 13 + 16, L[r], 0, 0, INK, 0);
        d_text_box(d, TK_IT400, 16, CONTENT_R - 16, ry + 13 + 16, 170, 2, s, 0, TK_TNUM, W(.55f), 0);
        if (r < 4) d_rrect(d, ROW_TX, ry + 46, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
    }
    y += 47 * 5 + 14;
    SS.rm_y = -1;
    if (inst && !st_busy(p)) {
        if (p->flags & SH_PKG_SYSTEM)
            d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y), "Part of the system: it can't be removed.", 0, 0, W(.45f), 0);
        else {
            d_rrect(d, GROUP_X, y, GROUP_W, 46, 16, tk_hex(0x151c33, 1));
            d_text(d, TK_IT400, 16, ROW_TX, y + 13 + 16, "Remove App", 0, 0, tk_hex(0xff453a, 1), 0);
            SS.rm_y = y;
        }
    }
}
/* the detail's look depends on these; only the progress changed: the button layer alone */
static int st_dkey(const sh_lpkg_t *p)
{
    return (int)(p->flags & 0x1f) | (st_busy(p) ? 32 : 0) | (p->state == SH_PKG_FAILED ? 64 : 0);
}
static void st_detail(int i)
{
    SS.det = 1;
    lx_scopy(SS.pkg, LST->pkg[i].pkg, 40);
    SS.dkey = st_dkey(&LST->pkg[i]);
    SS.rm_arm = 0;
    lx_make_icon(&st_dic, (int)(PT(100) + .5f), LST->pkg[i].icon, LST->pkg[i].name, 0);
}
/* the store's layers for what SS says (the page strips are drawn by st_page) */
static void st_build(void)
{
    relayer(&apsth); relayer(&apstb);
    apsth.hidden = apstb.hidden = 1;
    for (int L = 0; L < ST_CH; L++) { relayer(&apstl[L]); apstl[L].hidden = 1; }
    if (!is_app("store")) return;
    if (SS.det && st_find(SS.pkg) < 0) SS.det = 0;
    lx_build_kb(SS.kb && !SS.det, SS.shift, SS.num);
    if (SS.det) { st_btn(); relink(); return; }
    st_filter();
    int searching = SS.q[0] || SS.kb;
    SS.top = searching ? FIELD_Y + 70 : FIELD_Y + 230;
    SS.bot = SS.kb ? 540 : 772;
    st_head();
    st_list();
    relink();
}
static void st_changed(void)
{
    if (!is_app("store")) return;
    int i = SS.det ? st_find(SS.pkg) : -1;
    if (SS.det && i >= 0 && st_dkey(&LST->pkg[i]) == SS.dkey) { st_btn(); relink(); return; }
    if (SS.det && i >= 0) SS.dkey = st_dkey(&LST->pkg[i]);
    build_app_page(AP.a);                         /* the strips (a detail's notes) and the layers */
}
void sh_linux_store(const sh_lstore_t *s)
{
    LST = s && s->n >= 0 ? s : 0;
    if (lx_baked && is_app("store") && !ap_pending) st_changed();
}
static void lx_open_app(int ci);
static void st_act(int i)
{
    const sh_lpkg_t *p = &LST->pkg[i];
    char m[64];
    if (p->state == SH_PKG_REMOVING) return;
    if (st_busy(p)) { sh_linux_pkg(i, SH_PKG_OP_CANCEL); return; }
    if (p->flags & SH_PKG_INSTALLED) {
        int a = st_app(i);
        if (a >= 0) lx_open_app(a);
        else toast("Installed. It has no window of its own");
        return;
    }
    if (p->flags & SH_PKG_NO_SPACE) { toast("Not enough space on the memory card"); return; }
    if (sh_linux_pkg(i, SH_PKG_OP_INSTALL) < 0) { toast("Linux isn't running: try again in a moment"); return; }
    if (p->flags & SH_PKG_TOO_BIG) toast("It needs a lot of memory: it may run slowly");
    else { lx_scopy(m, "Installing ", 64); lx_scopy(m + 11, p->name, 53); toast(m); }
}
static int st_list_at(float x, float y)
{
    (void)x;
    return !SS.det && SS.n && y >= SS.top && y < SS.bot;
}
static void st_tap(float x, float y)
{
    if (!LST || LST->n <= 0) return;
    if (SS.det) {
        int i = st_find(SS.pkg);
        if (BACK_HIT(x, y)) { SS.det = 0; tk_bmp_free(&st_dic); build_app_page(AP.a); return; }
        if (i < 0) return;
        if (x >= 130 && x < 226 && y >= DT + 62 && y < DT + 106) { st_act(i); return; }
        if (SS.rm_y > 0 && y >= SS.rm_y && y < SS.rm_y + 46 && x >= GROUP_X && x < GROUP_X + GROUP_W) {
            char m[64];
            if (SS.rm_arm && now_ms - SS.rm_t < 3000) {
                SS.rm_arm = 0;
                if (sh_linux_pkg(i, SH_PKG_OP_REMOVE) < 0) { toast("Linux isn't running: try again in a moment"); return; }
                lx_scopy(m, "Removing ", 64); lx_scopy(m + 9, LST->pkg[i].name, 55);
            } else {
                SS.rm_arm = 1; SS.rm_t = now_ms;
                lx_scopy(m, "Tap again to remove ", 64); lx_scopy(m + 20, LST->pkg[i].name, 44);
            }
            toast(m);
        }
        return;
    }
    if (y >= FIELD_Y - 4 && y < FIELD_Y + 40) {   /* the field: the keyboard; Cancel: the list again */
        if (SS.kb && x >= CONTENT_R - 70) { SS.kb = 0; SS.q[0] = 0; SS.scroll = 0; st_build(); return; }
        if (!SS.kb) { SS.kb = 1; SS.shift = SS.num = 0; SS.scroll = 0; st_build(); }
        return;
    }
    if (SS.kb && y >= 548) {
        char c = 0;
        int k = kb_hit(x, y, SS.shift, SS.num, &c), n = (int)tk_strlen(SS.q), relayout = 0;
        if (k) sh_haptic();
        if (k == 2) SS.shift = !SS.shift;
        else if (k == 3) SS.num = !SS.num;
        else if (k == 4 && n) SS.q[n - 1] = 0;
        else if (k == 5) SS.kb = 0;               /* return: the results without the keyboard */
        else if (k == 1 && n < 22 && !(c == ' ' && !n)) {
            if (SS.shift) SS.shift = 0;
            SS.q[n] = c; SS.q[n + 1] = 0;
        }
        (void)relayout;
        SS.scroll = 0;
        st_build();
        return;
    }
    if (!SS.q[0] && !SS.kb && y >= FIELD_Y + 70 && y < FIELD_Y + 198) {   /* a featured card */
        int k = (int)((x - MARGIN) / (FEAT_W + 9));
        if (x >= MARGIN && k < SS.nfeat && x - MARGIN - (FEAT_W + 9) * k < FEAT_W) { st_detail(SS.feat[k]); build_app_page(AP.a); }
        return;
    }
    if (y >= SS.top && y < SS.bot) {
        int k = (int)((y - SS.top + SS.scroll) / 64);
        if (k < 0 || k >= SS.n) return;
        if (x >= ROW_BTN - 8) { st_act(SS.rows[k]); return; }
        st_detail(SS.rows[k]);
        build_app_page(AP.a);
    }
}
/* a catalog app opened from the store's "Open" (the Terminal's own page for foot) */
static void lx_open_app(int ci)
{
    int a = lx_term_id(LCAT.app[ci].id) ? lx_app_index("terminal") : lx_app_index("linux");
    lx_pick = ci;
    if (AP.on) app_finish_close();
    app_open(a);
}


/* ==== the hooks shell_lock.c calls (one or two lines at each place) ========================================== */
static int lx_is(const char *k) { return !tk_strcmp(k, "terminal") || !tk_strcmp(k, "linux") || !tk_strcmp(k, "store"); }
/* page 3's slot of "linux": the open app's, else the one a press on home picked (lx_hit); -1 none */
static int lx_slot(void)
{
    const char *id = is_app("linux") ? LX.id : !AP.on && lx_pick >= 0 && lx_pick < LCAT.n ? LCAT.app[lx_pick].id : 0;
    for (int i = 0; id && i < LH.n; i++) if (!tk_strcmp(LCAT.app[LH.cat[i]].id, id)) return i;
    return -1;
}
/* app_icon: the App Store's and the Linux apps' icons are bitmaps of ours (1: k is ours, *ic may be NULL: Search
 * lists the App Store, never "linux") */
static int lx_icon(const char *k, float *pad, const tk_bmp **ic)
{
    if (!tk_strcmp(k, "store")) { *pad = 0; *ic = store_ic.px ? &store_ic : 0; return 1; }
    if (tk_strcmp(k, "linux")) return 0;
    int s = lx_slot();
    *pad = 0;
    *ic = is_app("linux") && lx_ic.px ? &lx_ic : !AP.on && s >= 0 && lx_slot_icon(s) ? &lx_ic : 0;
    return 1;
}
/* build_home: page 3 (hparts[5]) */
static void lx_home(void) { lx_baked = 1; LH.sig = 0; lx_page3_update(); }
/* app_hit on page 3 (x0: the page's own x): the APPS index of the App Store or of "linux" (lx_pick set), or -1 */
static int lx_hit(float x0, float y)
{
    int col = (int)((x0 - 15) / 90), row = (int)((y - HOME_TOP2) / ROW_PITCH), i = row * 4 + col;
    if (x0 < 15 || y < HOME_TOP2 || col >= 4 || row >= 5 || y - HOME_TOP2 - row * ROW_PITCH >= 83 || i > LH.n) return -1;
    if (!i) return lx_app_index("store");
    lx_pick = LH.cat[i - 1];
    return lx_app_index("linux");
}
/* icon_rect: page 3's slots (the open Linux app's, the App Store's) */
static int lx_icon_rect(const char *k, arect *r)
{
    int s = !tk_strcmp(k, "store") ? 0 : !tk_strcmp(k, "linux") && lx_slot() >= 0 ? lx_slot() + 1 : -1;
    if (s < 0) return 0;
    *r = (arect){ 780 + COL_CX(s % 4) - ICON_PT / 2 + page_x, HOME_TOP2 + ROW_PITCH * (s / 4), ICON_PT, ICON_PT, 15 };
    return 1;
}
/* own_strip: the page strips only for a package's page (the window, the store's list: own layers) */
static int lx_strips(const char *k) { return !tk_strcmp(k, "store") && SS.det; }
static void lx_page(dc *d) { st_page(d); }
/* own_extras */
static void lx_build(void)
{
    if (lx_is_lin()) lx_build_win();
    else if (is_app("store")) st_build();
}
/* app_open, after the page's build is under way: which app, its keyboard and viewport, Linux started / held */
static void lx_open(int a)
{
    const char *k = APPS[a].k;
    if (!tk_strcmp(k, "store")) {                /* the App Store opens on its list */
        tk_memset(&SS, 0, sizeof SS);
        SS.top = FIELD_Y + 230; SS.bot = 772;
        (void)sh_linux_hold(1);                   /* Linux started for the package index, kept while open */
        return;
    }
    lx_prepare(a);
    (void)sh_linux_open(LX.id);                   /* refused: sh_linux_state says why */
}
/* app_finish_close: every layer of ours freed (the window's 15.5 MiB the moment the app leaves the screen) */
static void lx_close(void)
{
    if (AP.on && lx_is_lin()) sh_linux_hide();
    if (AP.on && is_app("store")) (void)sh_linux_hold(0);
    for (int k = 0; k < 4; k++) { relayer(&aplin[k]); aplin[k].hidden = 1; aplin[k].clip = 0; }
    relayer(&apkb); apkb.hidden = 1;
    relayer(&aplst); aplst.hidden = 1;
    LX.text = 0; LX.kb = 0;
    relayer(&apsth); relayer(&apstb);
    apsth.hidden = apstb.hidden = 1;
    for (int L = 0; L < ST_CH; L++) { relayer(&apstl[L]); apstl[L].hidden = 1; }
    st_icons_free();
}
/* app_tap: 1 taken */
static int lx_tap(float x, float y)
{
    if (lx_is_lin()) { lx_kb_tap(x, y); return 1; }
    if (is_app("store")) { st_tap(x, y); return 1; }
    return 0;
}
/* sh_touch inside an open app (T.kind G_LINUX while it returns 1 for the down): the window's touches, the store's
 * list (a drag scrolls it, a tap is st_tap) */
static struct { int mode, moved; float y0, base; } LT;
static int lx_touch(int ph, float x, float y)
{
    if (ph == 0) {
        LT.mode = 0;
        if (lx_is_lin() && lx_at(x, y)) { LT.mode = 1; lx_win_touch(0, x, y); }
        else if (is_app("store") && st_list_at(x, y)) { LT.mode = 2; LT.moved = 0; LT.y0 = y; LT.base = SS.scroll; }
        return LT.mode != 0;
    }
    if (LT.mode == 1) lx_win_touch(ph, x, y);
    else if (LT.mode == 2) {
        if (tk_fabsf(y - LT.y0) > 5) LT.moved = 1;
        if (ph == 1 && LT.moved) { SS.scroll = LT.base - (y - LT.y0); st_place(); }
        if (ph == 2 && !LT.moved) st_tap(x, y);
    }
    if (ph == 2) LT.mode = 0;
    return 1;
}
static int lx_moving(void) { return LT.mode == 2; }
/* relink: our layers under the page (appc), over its strips */
static int lx_layers(tk_layer **o)
{
    int n = 0;
    for (int k = 0; k < 4; k++) o[n++] = &aplin[k];
    o[n++] = &aplst;
    o[n++] = &apsth;
    for (int L = 0; L < ST_CH; L++) o[n++] = &apstl[L];
    o[n++] = &apstb;
    o[n++] = &apkb;
    return n;
}
/* sh_set: "lapp=ID:v" a Linux app by its desktop id; "store=PKG:v" the App Store (on PKG's page if given);
 * "storeq=TEXT:v" its search typed (v 1: the keyboard up) */
static int lx_set(const char *what, const char *key, float v)
{
    if (!tk_strcmp(what, "lapp")) {
        int ci = lx_cat_find(key);
        if (ci < 0) return 1;
        lx_pick = ci;
        sh_set("app", lx_term_id(key) ? "terminal" : "linux", v);
        return 1;
    }
    if (!tk_strcmp(what, "store")) {
        sh_set("app", "store", v);
        if (key[0] && st_find(key) >= 0) { st_detail(st_find(key)); build_app_page(AP.a); }
        return 1;
    }
    if (!tk_strcmp(what, "storeq")) {
        if (!is_app("store")) return 1;
        lx_scopy(SS.q, key, 23);
        SS.kb = v > 0;
        SS.det = 0;
        build_app_page(AP.a);
        return 1;
    }
    return 0;
}
