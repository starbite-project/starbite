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
static struct { int state, got, first, shift, num, kb, text, zc; unsigned flags, mods; char why[64], id[40], name[32]; } LX;
static sh_lcat_t LCAT;                           /* the shell's copy of the catalog (kept while Linux is off) */
static const sh_lstore_t *LST;                   /* the platform's packages (sh_linux_store) */
static tk_layer aplin[4], aplst;
#ifdef TK_HAS_LAYER_EXTERNAL
static tk_layer apext;                           /* v3 + Track A: the window sampled in place by the GPU */
static void lx_ext_off(void) { if (apext.external) tk_layer_external(&apext, 0, 0, 0, 0); apext.hidden = 1; }
#endif
static void lx_acc_show(int on);                 /* v3 (the section "v3" below): the key bar, the cards */
static void lx_cards_sync(void);
static void lx_card_add(const char *id, const char *name);
static void lx_card_snap_open(void);
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
/* ---- contract v3 (Track E, "apps work fully"): each running Linux app its own switcher card, the key bar over the
 * keyboard, Linux notifications in the Notification Centre, the shell-wide clipboard (the section "v3" below).
 * The switcher's external cards, the keyboard's accessory bar and the Centre's withdrawal are the toolkit's API
 * (Track B): built only when it defines SH_EXT_API (before this file), else Linux apps keep L3's behaviour (no cards,
 * no key bar; notifications and the clipboard work without it). The toolkit's side, which this file calls: */
#ifdef SH_EXT_API
static void sw_ext_add(int subid);               /* a card of ours (shell_switcher.inc) */
static void sw_ext_remove(int subid);
static void kb_accessory(const char *const *keys, int n, void (*tap)(int i));   /* the bar over the keyboard */
static int kb_accessory_tap(float x, float y);   /* a tap on the bar: 1 taken (tap(i) called) */
static void nc_withdraw(const char *who);        /* the Centre's items of who removed (no dismiss callback) */
#ifndef KB_ACC_H
#define KB_ACC_H 40.0f                           /* the bar's height in pt, from 540 - KB_ACC_H to 540 */
#endif
/* the window's rows shown with the keyboard up: above the bar (44 pt .. 540 - KB_ACC_H) */
#define LX_KBROWS (SH_LINUX_HKB - (int)(KB_ACC_H * SH_LINUX_HKB / (540.0f - 44.0f) + .5f))
#else
#define LX_KBROWS SH_LINUX_HKB
#endif
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
/* cp7u (Track A's 7s: a 418 ms "UI's draw" at Linux's HELLO, the screen off, the A55 at 455 MHz: page 3 built with
 * all its icons inside sh_linux_catalog): while page 3 is not in view the icons are made one per idle turn
 * (lx_idle, from sh_idle; none while the screen is off) into a cache that lives only until the page is baked from it */
static tk_bmp lx_icache[LHOME];
static uint32_t lx_ikey[LHOME];
static int lx_p3_want;                           /* page 3 is to be built again (its icons first, in idle turns) */
static uint32_t lx_icon_key(int s, int px)
{
    const sh_lapp_t *a = &LCAT.app[LH.cat[s]];
    uint32_t h = 2166136261u ^ (uint32_t)px;
    for (const char *t = a->name; *t; t++) h = (h ^ (uint8_t)*t) * 16777619u;
    h = (h ^ (uint32_t)a->icon) * 16777619u;
    return h ? h : 1;
}
static void lx_icache_free(void)
{
    for (int s = 0; s < LHOME; s++) { tk_bmp_free(&lx_icache[s]); lx_ikey[s] = 0; }
}
static int lx_p3_seen(void) { return !sh_sleeping() && !locked && !AP.on && page_x < -585; }
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
        int px = (int)(PT(ICON_PT) + .5f), cached = i && lx_ikey[i - 1] == lx_icon_key(i - 1, px) && lx_icache[i - 1].px;
        if (i && !cached) lx_make_icon(&t, px, LCAT.app[LH.cat[i - 1]].icon, LCAT.app[LH.cat[i - 1]].name, 1);
        const tk_bmp *ic = !i ? &store_ic : cached ? &lx_icache[i - 1] : &t;
        LH.ix[i] = (int)tk_floorf(PT(cx - ICON_PT / 2) - d.ox + .5f);
        LH.iy[i] = (int)tk_floorf(PT(y) - d.oy + .5f);
        if (ic->px) tk_blit(d.b, ic, LH.ix[i], LH.iy[i], 1);
        tk_bmp_free(&t);
        d_text_box(&d, TK_IT400, 12, cx, y + 68 + 12, 84, 1, i ? LCAT.app[LH.cat[i - 1]].name : "App Store", 0, 0, INK, 1);
    }
    lx_icache_free();                            /* drawn into the page: none kept */
    lx_p3_want = 0;
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
    if (!lx_p3_seen()) {                         /* not in view: its icons in idle turns, then the page (lx_idle) */
        lx_p3_want = 1;
        return;
    }
    lx_build_page3();
    relink();
}
/* sh_idle: page 3's next icon, or the page once all are made; 1 if it did something */
static int lx_idle_pending(void) { return lx_p3_want && lx_baked && !sh_sleeping(); }
static int lx_idle(void)
{
    if (!lx_idle_pending()) return 0;
    if (lx_p3_seen()) { lx_build_page3(); relink(); return 1; }   /* in view now: the rest at once, as before */
    int px = (int)(PT(ICON_PT) + .5f);
    if (!store_ic.px) { lx_make_store_icon(&store_ic, px); return 1; }
    for (int s = 0; s < LH.n; s++) {
        uint32_t k = lx_icon_key(s, px);
        if (lx_ikey[s] == k && lx_icache[s].px) continue;
        if (lx_make_icon(&lx_icache[s], px, LCAT.app[LH.cat[s]].icon, LCAT.app[LH.cat[s]].name, 1)) lx_ikey[s] = k;
        else lx_p3_want = 0;                     /* no memory: the page is built whole when next seen */
        return 1;
    }
    lx_build_page3();                            /* every icon made: the page from them (labels and blits) */
    relink();
    return 1;
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
/* B3: the shell keyboard (shell_kb.inc) on its own layers; apkb stays empty (kept for the layer list) */
static void lx_build_kb(int on, int shift, int num)
{
    relayer(&apkb);
    apkb.hidden = 1;
    if (on && !KB.shown && !KB.focused)        /* a terminal: ASCII, return is return; the store's search: "search" */
        kb_focus(KB_ASCII, is_app("store") ? "search" : 0, "");
    (void)shift; (void)num;
    KB.want = on;
    kb_set(on);
}
static void lx_build_state(void)
{
    relayer(&aplst);
    aplst.hidden = 1;
    for (int k = 0; k < 4; k++) {
        aplin[k].hidden = LX.state != SH_LINUX_RUNNING || !aplin[k].bmp.px || !LX.got;
        aplin[k].clip = LX.kb;                   /* the keyboard up: the rows under it are not shown */
        aplin[k].clip_r = (tk_rect){ 0, SH_LINUX_Y, TK_W, 2.0f * LX_KBROWS };   /* (v3: above the key bar) */
    }
#ifdef TK_HAS_LAYER_EXTERNAL
    apext.hidden = LX.state != SH_LINUX_RUNNING || !apext.external || !LX.got;   /* (its rows: the viewport's) */
#endif
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
        if (LX.why[0]) {                         /* the start's own words: a disk check, a slow start (cp7 #1) */
            lx_scopy(l, LX.why, 96);
        } else {
            for (const char *s = nm; *s && k < 40; s++) l[k++] = *s;
            lx_scopy(l + k, " opens once Linux has started: about 30 seconds.", 96 - k);   /* (7u: HELLO 31.8 s) */
        }
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
#ifdef TK_HAS_LAYER_EXTERNAL
    /* Track A's zero-copy path: the GPU samples the guest's front buffer in place (one external layer, no strips);
     * chosen at each build of the window (an open, the keyboard up / down), so a GPU that goes away mid-show falls
     * back to the strips at the next build */
    LX.zc = sh_linux_zero_copy();
    if (LX.zc) {
        for (int k = 0; k < 4; k++) { relayer(&aplin[k]); aplin[k].hidden = 1; }
        for (int r = 0; r < 2 * SH_LINUX_H; r++) lrow[r] = lsink;
        if (!apext.name) tk_layer_init(&apext, "linux-window-gpu");
        apext.x = apext.bx = 0;
        apext.y = apext.by = SH_LINUX_Y / 2.0f;   /* (scaled 2x about the origin) */
        apext.sx = apext.sy = 2;
        LX.first = 1;
        lx_build_kb(LX.kb, LX.shift, LX.num);
        lx_acc_show(LX.kb && lx_is_lin());
        lx_build_state();
        relink();
        (void)sh_linux_frame();
        return;
    }
    lx_ext_off();
#endif
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
    lx_acc_show(LX.kb && lx_is_lin());            /* v3: the key bar while the window has the keyboard */
    lx_build_state();
    relink();
    (void)sh_linux_frame();                       /* what the app has drawn so far */
}
int sh_linux_frame(void)
{
#ifdef TK_HAS_LAYER_EXTERNAL
    if (LX.zc) {                                  /* the new front buffer handed to the layer (sbg_surf_reading) */
        int stride = 0;
        const uint32_t *px = lx_is_lin() ? sh_linux_front(&stride) : 0;
        if (!px) return 0;
        tk_layer_external(&apext, px, SH_LINUX_W, LX.kb ? LX_KBROWS : SH_LINUX_H, stride);
        LX.first = 0;
        if (!LX.got) { LX.got = 1; lx_build_state(); }
        return 1;
    }
#endif
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
#ifdef TK_HAS_LAYER_EXTERNAL
    if (state != SH_LINUX_RUNNING) lx_ext_off();  /* the guest's memory is no longer a window */
#endif
    lx_cards_sync();                              /* v3: Linux gone: its cards too */
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
    sh_linux_viewport(kb ? LX_KBROWS : SH_LINUX_H);
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
    lx_cards_sync();                              /* v3: an app that quit by itself loses its card */
    if (pk && is_app("store") && !ap_pending) st_changed();   /* "Open" follows what is installed */
}
static int lx_at(float x, float y)
{
    (void)x;
    return aplin[0].bmp.px && !aplin[0].hidden && PT(y) >= SH_LINUX_Y && PT(y) < SH_LINUX_Y + 2 * (LX.kb ? LX_KBROWS : SH_LINUX_H);
}
static void lx_win_touch(int phase, float x, float y)
{
    int sx = (int)(PT(x) / 2), sy = (int)((PT(y) - SH_LINUX_Y) / 2), hh = LX.kb ? LX_KBROWS : SH_LINUX_H;
    sx = sx < 0 ? 0 : sx >= SH_LINUX_W ? SH_LINUX_W - 1 : sx;
    sy = sy < 0 ? 0 : sy >= hh ? hh - 1 : sy;
    sh_linux_touch(phase, sx, sy);
}
static int lx_acc_taken(float x, float y);     /* v3: the key bar (below) */
static int lx_mods_char(char c);
static void lx_kb_tap(float x, float y)
{
    if (LX.kb && lx_acc_taken(x, y)) return;      /* v3: the key bar over the keyboard */
    if (!LX.kb || y < 548) return;                /* the window's own touches went to Linux (G_LINUX) */
    char c = 0;
    int k = kb_hit(x, y, LX.shift, LX.num, &c);
    if (k == 4) { if (!lx_mods_char('\b')) (void)sh_linux_char('\b'); }   /* B3: shift and 123 are the keyboard's own */
    else if (k == 1 || k == 5) { if (!lx_mods_char(c)) (void)sh_linux_char(c); }   /* v3: Ctrl / Alt latched */
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


/* ==== v3 (Track E, contract v3: "apps work fully") ================================================================
 * The clipboard and the notifications work everywhere; the switcher cards and the key bar need the toolkit's
 * SH_EXT_API (see the declarations at the top of this file). */

/* ---- the platform's v3 calls: weak stand-ins for a platform without them (the host preview's linux_fake.inc);
 * gwin.c's are the real ones ---- */
#define LX_WEAK __attribute__((weak))
LX_WEAK int sh_linux_close(const char *id) { (void)id; return 0; }
LX_WEAK int sh_linux_key(int code, unsigned mods) { (void)code; (void)mods; return 0; }
LX_WEAK int sh_linux_char_mods(char c, unsigned mods) { (void)mods; return sh_linux_char(c); }
LX_WEAK void sh_linux_cards(int n) { (void)n; }
LX_WEAK int sh_linux_notif_act(unsigned id, int action) { (void)id; (void)action; return 0; }
LX_WEAK int sh_linux_clip(const char *text) { (void)text; return 0; }
LX_WEAK const uint32_t *sh_linux_front(int *stride_px) { *stride_px = 0; return 0; }

/* ---- the shell-wide clipboard: UTF-8 text in RAM (cut at a whole character); Starbite's copies are offered to Linux,
 * Linux's copies land here (sh_linux_clip_in: not offered back) ---- */
#define LX_CLIP 8192
static char lx_clip[LX_CLIP];
static void lx_clip_put(const char *t)
{
    int n = 0;
    while (t && t[n] && n < LX_CLIP - 1) n++;
    while (n > 0 && t[n] && ((unsigned char)t[n] & 0xc0) == 0x80) n--;   /* not half a character */
    if (n) tk_memcpy(lx_clip, t, (size_t)n);
    lx_clip[n] = 0;
}
void sh_clip_set(const char *text) { lx_clip_put(text); (void)sh_linux_clip(lx_clip); }   /* B3's signature */
const char *sh_clip_get(void) { return lx_clip; }
void sh_linux_clip_in(const char *text) { lx_clip_put(text); }

/* ---- notifications: each Linux notification is the Centre's item of who "linux:<the guest's id in hex>" (at most
 * 14 characters); what it is about is kept here (the app to open, its name and icon) ---- */
#define LX_NOTES 20
static struct { unsigned id; char app[40], name[32]; } LN[LX_NOTES];   /* newest first */
static int lnn;
static void lx_who(char *w, unsigned id)
{
    char h[8];
    int n = 0, k = 6;
    do { h[n++] = "0123456789abcdef"[id & 15]; id >>= 4; } while (id && n < 8);
    tk_memcpy(w, "linux:", 6);
    while (n) w[k++] = h[--n];
    w[k] = 0;
}
static unsigned lx_who_id(const char *id)
{
    unsigned v = 0;
    if (id && id[0] == 'l' && id[1] == 'i' && id[2] == 'n' && id[3] == 'u' && id[4] == 'x' && id[5] == ':') id += 6;
    for (; id && *id; id++) v = v << 4 | (unsigned)(*id <= '9' ? *id - '0' : (*id | 32) - 'a' + 10);
    return v;
}
static int lx_note_find(unsigned id) { for (int i = 0; i < lnn; i++) if (LN[i].id == id) return i; return -1; }
static void lx_note_drop(int i)
{
    if (i < 0 || i >= lnn) return;
    for (int k = i; k + 1 < lnn; k++) LN[k] = LN[k + 1];
    lnn--;
}
static void lx_note_withdraw(unsigned id)
{
    char w[16];
    lx_who(w, id);
#ifdef SH_EXT_API
    nc_withdraw(w);
#endif
    lx_note_drop(lx_note_find(id));
}
void sh_linux_notify(unsigned id, const char *app_id, const char *app, const char *title, const char *body, int hm)
{
    char w[16];
    if (lx_note_find(id) >= 0) lx_note_withdraw(id);   /* replaced (the same id): the old one goes */
    if (lnn == LX_NOTES) lnn--;
    for (int k = lnn; k > 0; k--) LN[k] = LN[k - 1];
    lnn++;
    LN[0].id = id;
    lx_scopy(LN[0].app, app_id ? app_id : "", 40);
    int ci = LN[0].app[0] ? lx_cat_find(LN[0].app) : -1;
    lx_scopy(LN[0].name, app && app[0] ? app : ci >= 0 ? LCAT.app[ci].name : "Linux", 32);
    lx_who(w, id);
    sh_notify(w, title && title[0] ? title : LN[0].name, body ? body : "", hm);
}
void sh_linux_notify_close(unsigned id) { lx_note_withdraw(id); }
/* a catalog app opened from elsewhere (a notification, a card); the open app stays as it is */
static int lx_open_id_app(const char *app)
{
    if (AP.on && !AP.closing && lx_is_lin() && (!tk_strcmp(LX.id, app) || (lx_term_id(app) && lx_term_id(LX.id)))) return 1;
    int ci = lx_cat_find(app);
    if (ci >= 0) { lx_open_app(ci); return 1; }
    if (!lx_term_id(app)) return 0;
    if (AP.on) app_finish_close();
    app_open(lx_app_index("terminal"));
    return 1;
}
/* the toolkit's Centre (SH_EXT_API), for its items of who "linux:<id>" (id: the part after "linux:", or the whole who):
 *   lx_open_id      a tap: NOTIF_ACT 0 (the default action) and the app opened; 1 opened
 *   lx_nc_dismissed a swipe away / Clear: NOTIF_ACT none
 *   lx_nc_name      the app's name for the platter's header
 *   lx_nc_icon      the app's icon (ICON_PT, as app_icon's with pad 0; NULL: none) */
static __attribute__((unused)) int lx_open_id(const char *id)
{
    unsigned nid = lx_who_id(id);
    int i = lx_note_find(nid);
    if (i < 0) return 0;
    char app[40];
    lx_scopy(app, LN[i].app, 40);
    lx_note_drop(i);
    (void)sh_linux_notif_act(nid, 0);
    return app[0] ? lx_open_id_app(app) : 0;
}
static __attribute__((unused)) void lx_nc_dismissed(const char *id)
{
    unsigned nid = lx_who_id(id);
    int i = lx_note_find(nid);
    if (i < 0) return;
    lx_note_drop(i);
    (void)sh_linux_notif_act(nid, -1);
}
static __attribute__((unused)) const char *lx_nc_name(const char *id)
{
    int i = lx_note_find(lx_who_id(id));
    return i >= 0 ? LN[i].name : "Linux";
}
static struct { char app[40]; tk_bmp b; } LNI[4];       /* the notifying apps' icons */
static __attribute__((unused)) const tk_bmp *lx_nc_icon(const char *id)
{
    int i = lx_note_find(lx_who_id(id));
    if (i < 0 || !LN[i].app[0]) return 0;
    int k = 0;
    for (; k < 4; k++) if (LNI[k].b.px && !tk_strcmp(LNI[k].app, LN[i].app)) return &LNI[k].b;
    for (k = 0; k < 4 && LNI[k].b.px; k++) ;
    if (k == 4) {                                /* the oldest goes */
        tk_bmp_free(&LNI[3].b);
        for (int j = 3; j > 0; j--) LNI[j] = LNI[j - 1];
        LNI[0].b = (tk_bmp){ 0, 0, 0, 0 };
        k = 0;
    }
    int ci = lx_cat_find(LN[i].app);
    lx_scopy(LNI[k].app, LN[i].app, 40);
    if (!lx_make_icon(&LNI[k].b, (int)(PT(ICON_PT) + .5f), ci >= 0 ? LCAT.app[ci].icon : -1, LN[i].name, 1)) return 0;
    return &LNI[k].b;
}

#ifdef SH_EXT_API
/* ---- the switcher: each Linux app the user opened is a card of its own (subid: a slot here, keyed by the desktop id,
 * so it survives the catalog's re-layouts), until it is swiped away (APP_CLOSE), quits by itself (its catalog flags
 * clear after they were set) or Linux stops. Cards keep Linux up while hidden (sh_linux_cards -> guest_apps). ---- */
#define LX_CARDS 8
#define LSNW (TK_W / 4)                          /* the snapshot: a quarter of the panel each way, as the switcher's */
#define LSNH (TK_H / 4)
static struct { int used, seen; uint32_t age; char id[40], name[32]; tk_bmp ic, snap; } LC[LX_CARDS];
static uint32_t lc_age;
static int lx_ncards(void) { int n = 0; for (int s = 0; s < LX_CARDS; s++) n += LC[s].used; return n; }
static int lx_card_find(const char *id)
{
    for (int s = 0; s < LX_CARDS; s++)
        if (LC[s].used && (!tk_strcmp(LC[s].id, id) || (lx_term_id(id) && lx_term_id(LC[s].id)))) return s;
    return -1;
}
static void lx_card_drop(int s, int tell_sw, int close_app)
{
    if (s < 0 || s >= LX_CARDS || !LC[s].used) return;
    if (close_app) (void)sh_linux_close(LC[s].id);
    tk_bmp_free(&LC[s].ic);
    tk_bmp_free(&LC[s].snap);
    LC[s].used = 0;
    if (tell_sw) sw_ext_remove(s);
    sh_linux_cards(lx_ncards());
}
static void lx_card_add(const char *id, const char *name)
{
    int s = lx_card_find(id);
    if (s < 0) {
        for (s = 0; s < LX_CARDS && LC[s].used; s++) ;
        if (s == LX_CARDS) {                     /* full: the oldest card's app is closed (as memory pressure would) */
            s = 0;
            for (int k = 1; k < LX_CARDS; k++) if (LC[k].age < LC[s].age) s = k;
            lx_card_drop(s, 1, 1);
        }
        LC[s].used = 1;
        LC[s].seen = 0;
        lx_scopy(LC[s].id, id, 40);
        lx_scopy(LC[s].name, lx_term_id(id) ? "Terminal" : name, 32);
    }
    LC[s].age = ++lc_age;
    sw_ext_add(s);                               /* (again: first in the recents) */
    sh_linux_cards(lx_ncards());
}
static void lx_cards_sync(void)
{
    if (LX.state == SH_LINUX_OFF || LX.state == SH_LINUX_FAILED || LX.state == SH_LINUX_STOPPING) {
        for (int s = 0; s < LX_CARDS; s++) lx_card_drop(s, 1, 0);
        return;
    }
    if (LX.state != SH_LINUX_RUNNING) return;
    for (int s = 0; s < LX_CARDS; s++) {
        if (!LC[s].used) continue;
        int ci = lx_cat_find(LC[s].id);
        unsigned f = ci >= 0 ? LCAT.app[ci].flags : 0;
        if (f & (SH_LAPP_RUNNING | SH_LAPP_WINDOW)) LC[s].seen = 1;
        else if (LC[s].seen && !(AP.on && lx_is_lin() && lx_card_find(LX.id) == s)) lx_card_drop(s, 1, 0);   /* it quit */
    }
}
/* the open app's card: a quarter-size picture of its window's last frame (the strips: 2 x 2 px averaged; the GPU's
 * in-place surface: one px each, it is uncached), the bars dark, the keyboard's ground under a keyboard */
static uint32_t lx_avg4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t r = 0;
    for (int sh = 0; sh < 32; sh += 8) r |= (((a >> sh & 255) + (b >> sh & 255) + (c >> sh & 255) + (d >> sh & 255) + 2) / 4) << sh;
    return r;
}
static void lx_card_snap_open(void)
{
    int s = lx_card_find(LX.id);
    if (s < 0 || !LX.got || LX.state != SH_LINUX_RUNNING) return;
    if (!LC[s].snap.px && !tk_bmp_new(&LC[s].snap, LSNW, LSNH)) return;
    int rows = LX.kb ? LX_KBROWS : SH_LINUX_H;
#ifdef TK_HAS_LAYER_EXTERNAL
    const uint32_t *ext = LX.zc && apext.external ? apext.bmp.px : 0;
    int es = apext.bmp.stride;
#endif
    for (int j = 0; j < LSNH; j++) {
        uint32_t *o = LC[s].snap.px + (size_t)j * LC[s].snap.stride;
        int sy = (4 * j + 2 - SH_LINUX_Y) / 2;
        uint32_t ground = LX.kb && 4 * j >= SH_LINUX_Y + 2 * rows && 4 * j < (int)PT(802) ? tk_premul(tk_hex(0x1e2030, 1)) :
                          0xff000000u | sv_ground(lx_app_index("linux"));
        if (4 * j + 2 < SH_LINUX_Y || sy >= rows) {
            for (int i = 0; i < LSNW; i++) o[i] = ground;
            /* the status bar (the app's apsb, still built while it closes) over its ground */
            int by = 4 * j + 2 - (int)apsb.y;
            if (4 * j + 2 < SH_LINUX_Y && apsb.bmp.px && !apsb.hidden && by >= 0 && by < apsb.bmp.h)
                for (int i = 0; i < LSNW; i++) {
                    int bx = 4 * i + 2 - (int)apsb.x;
                    if (bx < 0 || bx >= apsb.bmp.w) continue;
                    uint32_t p = apsb.bmp.px[(size_t)by * apsb.bmp.stride + bx], ia = 255 - (p >> 24), r = 0xff000000u;
                    for (int sh = 0; sh < 24; sh += 8) r |= (((p >> sh) & 255) + (((ground >> sh) & 255) * ia + 127) / 255) << sh;
                    o[i] = r;
                }
            continue;
        }
        for (int i = 0; i < LSNW; i++) {
#ifdef TK_HAS_LAYER_EXTERNAL
            if (ext) { o[i] = ext[(size_t)sy * es + 2 * i] | 0xff000000u; continue; }
#endif
            const uint32_t *r0 = lrow[2 * sy], *r1 = lrow[2 * sy + 1];
            o[i] = lx_avg4(r0[4 * i], r0[4 * i + 2], r1[4 * i], r1[4 * i + 2]) | 0xff000000u;
        }
    }
}
/* the switcher's provider (the toolkit's shell_switcher.inc calls these for its kind-1 entries) */
static __attribute__((unused)) const char *sw_ext_title(int subid)
{
    return subid >= 0 && subid < LX_CARDS && LC[subid].used ? LC[subid].name : "";
}
static __attribute__((unused)) const tk_bmp *sw_ext_icon(int subid)
{
    if (subid < 0 || subid >= LX_CARDS || !LC[subid].used) return 0;
    if (lx_term_id(LC[subid].id)) {              /* the Terminal: its own icon on page 2 */
        float pad = 0;
        const tk_bmp *t = app_icon("terminal", &pad);
        if (t && pad == 0) return t;
    }
    if (!LC[subid].ic.px) {
        int ci = lx_cat_find(LC[subid].id);
        lx_make_icon(&LC[subid].ic, (int)(PT(ICON_PT) + .5f), ci >= 0 ? LCAT.app[ci].icon : -1, LC[subid].name, 1);
    }
    return LC[subid].ic.px ? &LC[subid].ic : 0;
}
/* *out = a view of the card's snapshot (LSNW x LSNH, owned here: never freed by the caller; valid until the card
 * goes); 0: none yet (the icon on a plain card) */
static __attribute__((unused)) int sw_ext_snapshot(int subid, tk_bmp *out)
{
    if (subid < 0 || subid >= LX_CARDS || !LC[subid].used) return 0;
    if (AP.on && !AP.closing && lx_is_lin() && lx_card_find(LX.id) == subid) lx_card_snap_open();
    if (!LC[subid].snap.px) return 0;
    *out = LC[subid].snap;
    return 1;
}
static __attribute__((unused)) void sw_ext_open(int subid)
{
    if (subid < 0 || subid >= LX_CARDS || !LC[subid].used) return;
    char id[40];
    lx_scopy(id, LC[subid].id, 40);
    (void)lx_open_id_app(id);
}
/* thrown away: the app closed (APP_CLOSE), the card forgotten (the switcher drops its own entry) */
static __attribute__((unused)) void sw_ext_close(int subid) { lx_card_drop(subid, 0, 1); }
/* the open Linux app's card (the switcher's live first card), -1 none */
static __attribute__((unused)) int lx_sw_current(void) { return AP.on && lx_is_lin() ? lx_card_find(LX.id) : -1; }

/* ---- the key bar over the keyboard while a Linux window has it: Esc Tab Ctrl Alt and the arrows; Ctrl / Alt latch
 * for the next key (shown as CTRL / ALT while latched): KEYMOD, the key down + up, KEYMOD 0 ---- */
static const char *const LXK[8] = { "Esc", "Tab", "Ctrl", "Alt", "\xe2\x86\x90", "\xe2\x86\x91", "\xe2\x86\x93", "\xe2\x86\x92" };
static const int LXKC[8] = { 1, 15, 0, 0, 105, 103, 108, 106 };   /* KEY_ESC, KEY_TAB, -, -, LEFT, UP, DOWN, RIGHT */
static const char *lxk_now[8];
static int lx_acc_on;
static void lx_acc_tap(int i);
static void lx_acc_show(int on)
{
    if (!on) { if (lx_acc_on) kb_accessory(0, 0, 0); lx_acc_on = 0; return; }
    for (int i = 0; i < 8; i++) lxk_now[i] = LXK[i];
    if (LX.mods & SH_LMOD_CTRL) lxk_now[2] = "CTRL";
    if (LX.mods & SH_LMOD_ALT) lxk_now[3] = "ALT";
    kb_accessory(lxk_now, 8, lx_acc_tap);
    lx_acc_on = 1;
}
static void lx_acc_tap(int i)
{
    if (i == 2 || i == 3) { LX.mods ^= i == 2 ? SH_LMOD_CTRL : SH_LMOD_ALT; lx_acc_show(1); relink(); return; }
    if (i < 0 || i >= 8) return;
    (void)sh_linux_key(LXKC[i], LX.mods);
    if (LX.mods) { LX.mods = 0; lx_acc_show(1); relink(); }
}
static int lx_acc_taken(float x, float y) { return lx_acc_on && kb_accessory_tap(x, y); }
/* a key of the keyboard while Ctrl / Alt is latched: as keys with the modifiers (BackSpace too); 1 taken */
static int lx_mods_char(char c)
{
    if (!LX.mods) return 0;
    if (c == '\b') (void)sh_linux_key(14, LX.mods);
    else (void)sh_linux_char_mods(c, LX.mods);
    LX.mods = 0;
    lx_acc_show(1);
    relink();
    return 1;
}
#else
static void lx_acc_show(int on) { (void)on; }
static int lx_acc_taken(float x, float y) { (void)x; (void)y; return 0; }
static int lx_mods_char(char c) { (void)c; return 0; }
static void lx_cards_sync(void) {}
static void lx_card_add(const char *id, const char *name) { (void)id; (void)name; }
static void lx_card_snap_open(void) {}
#endif

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
/* app_build_step's strip frames (none of ours draws a strip): the App Store's heavy parts there (8h) */
static void lx_prestep(int st) { st_prebuild(st); }
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
    if (sh_linux_open(LX.id) >= 0) lx_card_add(LX.id, LX.name);   /* refused: sh_linux_state says why; v3: its card */
}
/* app_finish_close: every layer of ours freed (the window's 15.5 MiB the moment the app leaves the screen) */
static void lx_close(void)
{
    if (AP.on && lx_is_lin()) lx_card_snap_open();   /* v3: its card's snapshot from the window's last frame */
#ifdef TK_HAS_LAYER_EXTERNAL
    lx_ext_off();                                 /* detached before sh_linux_hide's sbg_surf_done */
    LX.zc = 0;
#endif
    lx_acc_show(0);
    LX.mods = 0;
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
static struct { int mode, lit; float x0, y0; } LT;
/* cp6 (Track B, the user's "no visual feedback on buttons"): what lights under the finger on the App Store's page
 * (G_LINUX touches never reach app_press). The rect comes from shell_appstore.inc's aq_press (the *_press hooks'
 * shape: r the lit rect, g its group's clip, rad, c), when it has one (AQ_HAS_PRESS); off when the finger moves (a
 * scroll) or lifts */
static void lx_press(float x, float y)
{
    LT.lit = 0;
    LT.x0 = x, LT.y0 = y;
#ifdef AQ_HAS_PRESS
    tk_rect r = { 0, 0, 0, 0 }, g = r;
    float rad = 0;
    tk_col c = W(.1f);
    if (aq_press(x, y, &r, &g, &rad, &c)) { hl_set(&apphl, r, g, rad, c); LT.lit = 1; }
#endif
}
static void lx_unpress(void) { if (LT.lit) { LT.lit = 0; hl_off(); } }
static int lx_touch(int ph, float x, float y)
{
    if (ph == 0) {
        LT.mode = 0;
        if (lx_is_lin() && lx_at(x, y)) { LT.mode = 1; lx_win_touch(0, x, y); }
        else if (is_app("store") && aq_touch(0, x, y)) { LT.mode = 2; lx_press(x, y); }
        return LT.mode != 0;
    }
    if (LT.lit && (ph == 2 || tk_fabsf(x - LT.x0) > 8 || tk_fabsf(y - LT.y0) > 8)) lx_unpress();
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
#ifdef TK_HAS_LAYER_EXTERNAL
    if (apext.name) o[n++] = &apext;
#endif
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
