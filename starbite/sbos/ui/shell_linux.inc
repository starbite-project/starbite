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
 *   - the App Store: shell_appstore.inc (Track F: tabs, Today, Browse, Search, Updates, the app's page with its
 *     install methods; its own layers, no strips).
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
static void st_build(void);                     /* shell_appstore.inc (Track F) */
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

/* ---- the App Store (Track F): shell_appstore.inc ---- */
#include "shell_appstore.inc"
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
/* own_strip: no page strips (the window, the App Store: own layers) */
static int lx_strips(const char *k) { (void)k; return 0; }
static void lx_page(dc *d) { (void)d; }
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
    if (!tk_strcmp(k, "store")) { aq_open(); return; }   /* the App Store opens on Today (Track F) */
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
    aq_close();                                   /* Track F: the App Store's layers */
}
/* app_tap: 1 taken */
static int lx_tap(float x, float y)
{
    if (lx_is_lin()) { lx_kb_tap(x, y); return 1; }
    if (is_app("store")) { aq_tap(x, y); return 1; }
    return 0;
}
/* sh_touch inside an open app (T.kind G_LINUX while it returns 1 for the down): the window's touches, the App
 * Store's page (a drag scrolls it, a tap is aq_tap: shell_appstore.inc) */
static struct { int mode; } LT;
static int lx_touch(int ph, float x, float y)
{
    if (ph == 0) {
        LT.mode = 0;
        if (lx_is_lin() && lx_at(x, y)) { LT.mode = 1; lx_win_touch(0, x, y); }
        else if (is_app("store") && aq_touch(0, x, y)) LT.mode = 2;
        return LT.mode != 0;
    }
    if (LT.mode == 1) lx_win_touch(ph, x, y);
    else if (LT.mode == 2) aq_touch(ph, x, y);
    if (ph == 2) LT.mode = 0;
    return 1;
}
static int lx_moving(void) { return LT.mode == 2 || aq_moving(); }
/* sh_tick: the App Store's scroll, its pages drawn over frames, the slide (1 while anything moves) */
static int lx_tick(float dt) { (void)aq_tick(dt); return lx_moving(); }
/* relink: our layers under the page (appc), over its strips */
static int lx_layers(tk_layer **o)
{
    int n = 0;
    for (int k = 0; k < 4; k++) o[n++] = &aplin[k];
    o[n++] = &aplst;
    n += aq_layers(o + n);                        /* Track F: the App Store */
    o[n++] = &apkb;
    return n;
}
/* sh_set: "lapp=ID:v" a Linux app by its desktop id; the App Store's store=, storecat=, storeq=, storeapp=,
 * storescroll=, storepick= (shell_appstore.inc's aq_set) */
static int lx_set(const char *what, const char *key, float v)
{
    if (!tk_strcmp(what, "lapp")) {
        int ci = lx_cat_find(key);
        if (ci < 0) return 1;
        lx_pick = ci;
        sh_set("app", lx_term_id(key) ? "terminal" : "linux", v);
        return 1;
    }
    return aq_set(what, key, v);                  /* Track F */
}
