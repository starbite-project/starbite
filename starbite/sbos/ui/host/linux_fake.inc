/* SPDX-License-Identifier: MIT
 * sbtk-host: a pretend Linux for the stills and films (L3, docs/STARBITE-LINUX-APPS.md; the kernel's platform is
 * starbite/guest/next/gwin.c). A catalog of six apps and a store of fourteen packages in every state, icons drawn
 * here, app windows drawn with the pack's fonts (foot: SBTK_LINUX = a 720 x 916 P6 PPM, its real frame from QEMU).
 * Every platform call is logged ("linux: ..."). Time is the film's (lin_fake_poll); a still settles first.
 *   SBTK_LINUX unset or a PPM: Linux ran earlier in this boot (running, the catalog and the store known)
 *   SBTK_LINUX=cold: off, nothing known yet (the first open starts it: 1.5 s, then the catalog and the store)
 *   SBTK_LINUX=starting / fail: stays starting / fails ("The Linux files are not on this phone yet.")
 * Host-only states for stills: lx=focus:1 (the open app's text field focused), lx=type:TEXT (typed into it). */
static sh_lcat_t FCAT;
static sh_lstore_t FST;
static struct {
    int state, shown, hold, want, win, vp, focus, settling, nstart, pends;
    float now, t_state, win_t, op_t[SH_LPKGS];
    char pend[40];
    unsigned seq, drawn;
    char text[SH_LAPPS][160];
    int loaded[SH_LAPPS];
    uint8_t *foot;
    tk_bmp w;                                   /* the window as the app drew it (720 x 1344) */
} LF = { .want = -1, .win = -1, .vp = SH_LINUX_H };

/* ---- icons (160 x 160 premultiplied): simple shapes, anti-aliased, and the pack's fonts ---- */
static uint32_t pm(uint32_t rgb, float a) { return tk_premul(tk_hex(rgb, a)); }
static void ic_over(uint32_t *px, int i, uint32_t c, float cov)
{
    if (cov <= 0) return;
    if (cov > 1) cov = 1;
    uint32_t d = px[i], ca = (uint32_t)((c >> 24) * cov + .5f), ia = 255 - ca;
    uint32_t o = 0;
    for (int s = 0; s < 32; s += 8) {
        uint32_t sc = (uint32_t)(((c >> s) & 255) * cov + .5f), dc = (d >> s) & 255;
        o |= ((sc + dc * ia / 255) & 255) << s;
    }
    (void)ca;
    px[i] = o;
}
static void ic_rrect(uint32_t *px, float x, float y, float w, float h, float r, uint32_t c)
{
    for (int j = 0; j < 160; j++) for (int i = 0; i < 160; i++)
        ic_over(px, j * 160 + i, c, .5f - tk_sd_rrect(i + .5f, j + .5f, x, y, w, h, r));
}
static void ic_circle(uint32_t *px, float cx, float cy, float r, uint32_t c)
{
    for (int j = 0; j < 160; j++) for (int i = 0; i < 160; i++) {
        float dx = i + .5f - cx, dy = j + .5f - cy;
        ic_over(px, j * 160 + i, c, r + .5f - tk_sqrtf(dx * dx + dy * dy));
    }
}
static void ic_line(uint32_t *px, float ax, float ay, float bx, float by, float r, uint32_t c)
{
    for (int j = 0; j < 160; j++) for (int i = 0; i < 160; i++) {
        float x = i + .5f, y = j + .5f, dx = bx - ax, dy = by - ay;
        float t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float ex = x - ax - t * dx, ey = y - ay - t * dy;
        ic_over(px, j * 160 + i, c, r + .5f - tk_sqrtf(ex * ex + ey * ey));
    }
}
static void ic_text(uint32_t *px, float size, float cx, float base, const char *s, uint32_t rgb, int font)
{
    tk_bmp b = { 160, 160, 160, px };
    tk_paint p = tk_solid(tk_hex(rgb, 1));
    tk_text(&b, font, size, cx - tk_text_w(font, size, s, 0, 0) / 2, base, s, 0, 0, &p, 0);
}
enum { K_FOOT, K_EDIT, K_CALC, K_FILES, K_HTOP, K_MINES, K_FOX, K_GIMP, K_CHROM, K_WRITER, K_INK, K_VLC, K_TUX, K_GEANY };
static void draw_icon(int k, uint32_t *px)
{
    memset(px, 0, 160 * 160 * 4);
    switch (k) {
    case K_FOOT:
        ic_rrect(px, 14, 22, 132, 116, 18, pm(0x2a2f3a, 1));
        ic_rrect(px, 14, 22, 132, 22, 18, pm(0x4a5266, 1));
        ic_line(px, 42, 70, 62, 88, 6, pm(0x7dd87a, 1)); ic_line(px, 62, 88, 42, 106, 6, pm(0x7dd87a, 1));
        ic_line(px, 74, 106, 112, 106, 6, pm(0xe6e6e6, 1));
        break;
    case K_EDIT:
        ic_rrect(px, 30, 14, 100, 132, 12, pm(0xf6f5f4, 1));
        for (int i = 0; i < 6; i++) ic_line(px, 46, 44 + 15 * i, i % 3 == 2 ? 92 : 114, 44 + 15 * i, 3, pm(0x9a9996, 1));
        ic_line(px, 92, 128, 136, 66, 8, pm(0x3584e4, 1));
        ic_line(px, 136, 66, 142, 58, 8, pm(0xe5a50a, 1));
        break;
    case K_CALC:
        ic_rrect(px, 26, 12, 108, 136, 16, pm(0x3d3846, 1));
        ic_rrect(px, 38, 24, 84, 30, 6, pm(0xc0bfbc, 1));
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
            ic_rrect(px, 38 + 30.0f * c, 64 + 26.0f * r, 22, 18, 5, c == 2 ? pm(0xff7800, 1) : pm(0x77767b, 1));
        break;
    case K_FILES:
        ic_rrect(px, 14, 30, 64, 30, 10, pm(0x438de6, 1));
        ic_rrect(px, 14, 44, 132, 96, 12, pm(0x62a0ea, 1));
        ic_rrect(px, 14, 58, 132, 82, 12, pm(0x99c1f1, 1));
        break;
    case K_HTOP:
        ic_rrect(px, 14, 18, 132, 124, 16, pm(0x111111, 1));
        for (int i = 0; i < 4; i++) {
            ic_rrect(px, 30, 36 + 24.0f * i, 100, 12, 3, pm(0x333333, 1));
            ic_rrect(px, 30, 36 + 24.0f * i, 30 + 18.0f * ((i * 7) % 4), 12, 3, pm(i == 3 ? 0xe01b24 : i ? 0xf6d32d : 0x33d17a, 1));
        }
        break;
    case K_MINES:
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
            ic_rrect(px, 18 + 43.0f * c, 18 + 43.0f * r, 38, 38, 6, pm((r + c) % 2 ? 0x8ff0a4 : 0x57e389, 1));
        ic_circle(px, 80, 80, 16, pm(0x241f31, 1));
        ic_line(px, 58, 80, 102, 80, 3, pm(0x241f31, 1)); ic_line(px, 80, 58, 80, 102, 3, pm(0x241f31, 1));
        break;
    case K_FOX:
        ic_circle(px, 80, 80, 66, pm(0xff9500, 1));
        ic_circle(px, 86, 74, 46, pm(0x7542e5, 1));
        ic_circle(px, 92, 68, 30, pm(0x2b1a6b, 1));
        ic_line(px, 34, 50, 60, 30, 14, pm(0xffcf33, 1));
        break;
    case K_GIMP:
        ic_circle(px, 80, 90, 56, pm(0x8a6d4b, 1));
        ic_circle(px, 62, 76, 16, pm(0xffffff, 1)); ic_circle(px, 98, 76, 16, pm(0xffffff, 1));
        ic_circle(px, 64, 78, 7, pm(0x000000, 1)); ic_circle(px, 100, 78, 7, pm(0x000000, 1));
        ic_line(px, 112, 40, 140, 16, 7, pm(0x8a6d4b, 1));
        ic_rrect(px, 64, 108, 32, 10, 5, pm(0xe5a50a, 1));
        break;
    case K_CHROM:
        ic_circle(px, 80, 80, 66, pm(0x5c8fdc, 1));
        ic_circle(px, 80, 80, 32, pm(0xffffff, 1));
        ic_circle(px, 80, 80, 24, pm(0x1a5fc8, 1));
        break;
    case K_WRITER:
        ic_rrect(px, 30, 14, 100, 132, 10, pm(0x2a7ad6, 1));
        ic_text(px, 64, 80, 104, "W", 0xffffff, TK_IT800);
        break;
    case K_INK:
        ic_circle(px, 80, 80, 64, pm(0x1c1c1c, 1));
        ic_line(px, 44, 104, 80, 44, 10, pm(0xffffff, 1)); ic_line(px, 80, 44, 116, 104, 10, pm(0xffffff, 1));
        ic_circle(px, 80, 122, 9, pm(0xffffff, 1));
        break;
    case K_VLC:
        ic_line(px, 80, 22, 50, 118, 12, pm(0xff8800, 1)); ic_line(px, 80, 22, 110, 118, 12, pm(0xff8800, 1));
        ic_rrect(px, 50, 50, 60, 70, 6, pm(0xff8800, 1));
        ic_rrect(px, 50, 66, 60, 8, 2, pm(0xffffff, 1)); ic_rrect(px, 44, 96, 72, 8, 2, pm(0xffffff, 1));
        ic_rrect(px, 26, 120, 108, 22, 6, pm(0xff8800, 1));
        break;
    case K_TUX:
        ic_circle(px, 80, 92, 48, pm(0x1c1c1c, 1)); ic_circle(px, 80, 100, 34, pm(0xf4f1ea, 1));
        ic_circle(px, 80, 46, 30, pm(0x1c1c1c, 1));
        ic_circle(px, 70, 42, 7, pm(0xffffff, 1)); ic_circle(px, 90, 42, 7, pm(0xffffff, 1));
        ic_rrect(px, 66, 54, 28, 10, 5, pm(0xffb627, 1));
        break;
    default:
        ic_rrect(px, 14, 14, 132, 132, 24, pm(0x2190a4, 1));
        ic_text(px, 60, 80, 102, "{ }", 0xffffff, TK_IT700);
        break;
    }
}

/* ---- the catalog and the store ---- */
typedef struct { const char *pkg, *name, *summary, *cat, *err; unsigned dl, inst, ram, flags, state, pct; int kind; const char *app; } fpkg;
static const fpkg FP[] = {
    { "firefox-esr", "Firefox ESR", "Mozilla's web browser, the extended support release. Software rendering on the big cores.", "Internet", "", 72000, 262000, 520, SH_PKG_FEATURED, 0, 0, K_FOX, "firefox-esr" },
    { "gnome-text-editor", "Text Editor", "A simple editor for notes, lists and code.", "Utilities", "", 2100, 7800, 90, SH_PKG_INSTALLED | SH_PKG_FEATURED, 0, 0, K_EDIT, "org.gnome.TextEditor" },
    { "gimp", "GIMP", "The GNU Image Manipulation Program: photos, painting, layers.", "Graphics", "", 58000, 236000, 300, SH_PKG_FEATURED, SH_PKG_INSTALLING, 40, K_GIMP, "gimp" },
    { "chromium", "Chromium", "The open-source browser behind Chrome.", "Internet", "", 98000, 352000, 900, SH_PKG_TOO_BIG, 0, 0, K_CHROM, "chromium" },
    { "libreoffice-writer", "LibreOffice Writer", "Word processing with the LibreOffice suite: documents, letters, books.", "Office", "", 160000, 1400000, 330, SH_PKG_NO_SPACE, 0, 0, K_WRITER, "libreoffice-writer" },
    { "inkscape", "Inkscape", "Vector graphics: drawings, logos, diagrams.", "Graphics", "Download stopped: no network", 41000, 168000, 220, 0, SH_PKG_FAILED, 0, K_INK, "org.inkscape.Inkscape" },
    { "vlc", "VLC", "Plays most video and audio files.", "Utilities", "", 24000, 96000, 140, 0, SH_PKG_QUEUED, 0, K_VLC, "vlc" },
    { "gnome-calculator", "Calculator", "Basic, advanced and financial calculations.", "Utilities", "", 1600, 6200, 60, SH_PKG_INSTALLED, 0, 0, K_CALC, "org.gnome.Calculator" },
    { "nautilus", "Files", "Browse the files in Linux's home folder.", "Utilities", "", 4300, 16500, 110, SH_PKG_INSTALLED, 0, 0, K_FILES, "org.gnome.Nautilus" },
    { "htop", "htop", "What runs in Linux and how much memory it takes, in a terminal.", "Development", "", 180, 420, 8, SH_PKG_INSTALLED, 0, 0, K_HTOP, "htop" },
    { "gnome-mines", "Mines", "Clear the minefield without stepping on a mine.", "Games", "", 900, 3100, 45, SH_PKG_INSTALLED, SH_PKG_REMOVING, 60, K_MINES, "org.gnome.Mines" },
    { "foot", "Foot", "The terminal: a fast Wayland terminal emulator.", "Development", "", 350, 1100, 20, SH_PKG_INSTALLED | SH_PKG_SYSTEM, 0, 0, K_FOOT, SH_LINUX_TERMINAL },
    { "supertux", "SuperTux", "A classic 2D jump-and-run with Tux.", "Games", "", 61000, 98000, 120, 0, 0, 0, K_TUX, "supertux2" },
    { "geany", "Geany", "A small, fast code editor with build and run.", "Development", "", 3800, 13900, 70, 0, 0, 0, K_GEANY, "geany" },
};
#define NFP ((int)(sizeof FP / sizeof FP[0]))
static void fake_push_store(void)
{
    FST.seq += 2;
    sh_linux_store(&FST);
}
static void fake_catalog_rebuild(void)
{
    /* the installed packages' apps, in the store's order (foot first: the Terminal) */
    sh_lcat_t old = FCAT;
    memset(&FCAT, 0, sizeof FCAT);
    FCAT.seq = old.seq + 2;
    FCAT.mem_total_kb = 1490000;
    FCAT.mem_avail_kb = 1180000;
    static const int order[] = { 11, 1, 7, 8, 9, 10, 0, 2, 3, 4, 5, 6, 12, 13 };
    for (int o = 0; o < NFP; o++) {
        int i = order[o];
        if (!(FST.pkg[i].flags & SH_PKG_INSTALLED)) continue;
        sh_lapp_t *a = &FCAT.app[FCAT.n];
        snprintf(a->id, sizeof a->id, "%s", FP[i].app);
        snprintf(a->name, sizeof a->name, "%s", i == 11 ? "Foot" : FP[i].name);
        a->flags = i == 11 || i == 9 ? SH_LAPP_KEYBOARD : 0;
        if (i == 9) a->flags |= SH_LAPP_TERMINAL;
        if (FP[i].flags & SH_PKG_SYSTEM) a->flags |= SH_LAPP_SYSTEM;
        a->icon = FCAT.n;                       /* the catalog's pool icons 0..63 */
        a->pkg = i;
        for (int k = 0; k < old.n; k++)
            if (!strcmp(old.app[k].id, a->id)) { a->flags |= old.app[k].flags & (SH_LAPP_RUNNING | SH_LAPP_WINDOW); a->rss_kb = old.app[k].rss_kb; }
        FCAT.n++;
    }
}
static int fake_kind_of_icon(int icon)
{
    if (icon >= 64 && icon < 64 + NFP) return FP[icon - 64].kind;
    if (icon >= 0 && icon < FCAT.n) return FP[FCAT.app[icon].pkg].kind;
    return -1;
}
static void fake_publish(void)
{
    if (!FST.n) {
        FST.n = NFP;
        FST.repo_ok = 1;
        FST.overlay_total_kb = 3900000;
        FST.overlay_free_kb = 1100000;
        FST.mem_total_kb = 1490000;
        for (int i = 0; i < NFP; i++) {
            sh_lpkg_t *p = &FST.pkg[i];
            snprintf(p->pkg, sizeof p->pkg, "%s", FP[i].pkg);
            snprintf(p->name, sizeof p->name, "%s", FP[i].name);
            snprintf(p->summary, sizeof p->summary, "%s", FP[i].summary);
            snprintf(p->category, sizeof p->category, "%s", FP[i].cat);
            snprintf(p->err, sizeof p->err, "%s", FP[i].err);
            p->dl_kb = FP[i].dl; p->inst_kb = FP[i].inst; p->ram_mb = FP[i].ram;
            p->flags = FP[i].flags; p->state = FP[i].state; p->pct = FP[i].pct;
            p->icon = 64 + i;
        }
    }
    fake_catalog_rebuild();
    sh_linux_catalog(&FCAT);
    fake_push_store();
}
static int fcat_find(const char *id)
{
    for (int i = 0; i < FCAT.n; i++) if (!strcmp(FCAT.app[i].id, id) || (!strcmp(id, "foot") && !strcmp(FCAT.app[i].id, SH_LINUX_TERMINAL))) return i;
    return -1;
}
static void fake_state(int s, const char *why)
{
    LF.state = s;
    LF.t_state = LF.now;
    fprintf(stderr, "linux: state %s\n", s == SH_LINUX_STARTING ? "starting" : s == SH_LINUX_RUNNING ? "running" : s == SH_LINUX_FAILED ? "failed" : s == SH_LINUX_OFF ? "off" : "stopping");
    sh_linux_state(s, why);
}
static int fake_start(void)
{
    const char *e = getenv("SBTK_LINUX");
    if (LF.state == SH_LINUX_RUNNING || LF.state == SH_LINUX_STARTING) return 0;
    if (e && !strcmp(e, "fail")) { fake_state(SH_LINUX_FAILED, "The Linux files are not on this phone yet."); return -4; }
    LF.nstart++;
    fake_state(SH_LINUX_STARTING, "");
    return 0;
}

/* ---- the windows: what each app draws (720 x vp, 1x surface px; the shell shows them at 2x) ---- */
static void wfill(int x, int y, int w, int h, uint32_t rgb) { tk_fill_rect_raw(&LF.w, x, y, w, h, 0xff000000u | rgb); }
static void wrr(float x, float y, float w, float h, float r, uint32_t rgb) { tk_paint p = tk_solid(tk_hex(rgb, 1)); tk_fill_rrect(&LF.w, x, y, w, h, r, &p, 1); }
static void wtext(int font, float size, float x, float base, const char *s, uint32_t rgb)
{
    tk_paint p = tk_solid(tk_hex(rgb, 1));
    tk_text(&LF.w, font, size, x, base, s, 0, 0, &p, 0);
}
static float wtw(int font, float size, const char *s) { return tk_text_w(font, size, s, 0, 0); }
static void headerbar(const char *title, int h)
{
    wfill(0, 0, 720, h, 0xebebeb);
    wfill(0, h - 1, 720, 1, 0xd0d0d0);
    wtext(TK_IT700, 24, 360 - wtw(TK_IT700, 24, title) / 2, h / 2 + 9, title, 0x2e2e2e);
    for (int i = 0; i < 3; i++) wrr(660, h / 2 - 11 + 9.0f * i, 30, 4, 2, 0x5e5c64);
}
static void draw_window(int ci)
{
    if (!LF.w.px) {                              /* the guest's memory, not the UI heap */
        LF.w.px = calloc((size_t)SH_LINUX_W * SH_LINUX_H, 4);
        if (!LF.w.px) return;
        LF.w.w = LF.w.stride = SH_LINUX_W;
        LF.w.h = SH_LINUX_H;
    }
    const char *id = FCAT.app[ci].id, *t = LF.text[ci];
    int vp = LF.vp;
    wfill(0, 0, 720, SH_LINUX_H, 0xffffff);
    if (!strcmp(id, SH_LINUX_TERMINAL)) {
        wfill(0, 0, 720, SH_LINUX_H, 0x242424);
        if (LF.foot) {
            for (int y = 0; y < 916 && y < vp; y++)
                for (int x = 0; x < 720; x++) {
                    const uint8_t *p = LF.foot + ((size_t)y * 720 + x) * 3;
                    LF.w.px[(size_t)y * LF.w.stride + x] = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
                }
        } else {
            char l[200];
            snprintf(l, sizeof l, "user@starbite:~$ %s", t);
            wtext(TK_IT500, 26, 12, 40, l, 0xdddddd);
        }
    } else if (!strcmp(id, "org.gnome.TextEditor")) {
        headerbar(t[0] ? "Untitled Document 1 \xe2\x80\xa2" : "Untitled Document 1", 92);
        wrr(20, 26, 88, 40, 8, 0xdcdcdc);
        wtext(TK_IT600, 22, 36, 54, "Open", 0x2e2e2e);
        float y = 150, x = 24;
        char line[160];
        int n = 0;
        for (const char *c = t;; c++) {                      /* the text, its own lines, then the caret */
            if (*c == '\n' || !*c) {
                line[n] = 0;
                wtext(TK_IT400, 30, 24, y, line, 0x1e1e1e);
                x = 24 + wtw(TK_IT400, 30, line);
                if (!*c) break;
                y += 42; n = 0;
                continue;
            }
            if (n < 150) line[n++] = *c;
        }
        if (LF.focus) wfill((int)x + 2, (int)y - 30, 3, 38, 0x3584e4);
        if (!t[0] && !LF.focus) wtext(TK_IT400, 24, 24, 150, "Tap to write", 0xa0a0a0);
        char st[64];
        snprintf(st, sizeof st, "Ln 1, Col %d", (int)strlen(t) + 1);
        wfill(0, vp - 52, 720, 52, 0xf6f5f4);
        wtext(TK_IT400, 20, 700 - wtw(TK_IT400, 20, st), vp - 18, st, 0x77767b);
    } else if (!strcmp(id, "firefox-esr")) {
        wfill(0, 0, 720, 60, 0xf0f0f4);
        wrr(10, 10, 230, 44, 8, 0xffffff);
        wtext(TK_IT500, 20, 30, 40, LF.loaded[ci] ? "Problem loading page" : "New Tab", 0x15141a);
        wfill(0, 60, 720, 74, 0xf9f9fb);
        wrr(16, 72, 688, 50, 12, LF.focus ? 0xffffff : 0xe8e8ed);
        if (LF.focus) { wrr(16, 72, 688, 50, 12, 0x0060df); wrr(19, 75, 682, 44, 10, 0xffffff); }
        if (t[0]) wtext(TK_IT400, 24, 40, 105, t, 0x15141a);
        else wtext(TK_IT400, 24, 40, 105, "Search or enter address", 0x8f8f9d);
        if (LF.focus) wfill(40 + (int)wtw(TK_IT400, 24, t) + 2, 82, 3, 30, 0x0060df);
        if (LF.loaded[ci]) {
            wtext(TK_IT700, 36, 60, 300, "Hmm. We're having trouble", 0x15141a);
            wtext(TK_IT700, 36, 60, 346, "finding that site.", 0x15141a);
            wtext(TK_IT400, 24, 60, 410, "Linux has no network yet: it comes", 0x5b5b66);
            wtext(TK_IT400, 24, 60, 444, "through Starbite's Wi-Fi in L5.", 0x5b5b66);
        } else {
            for (int r = 0; r < 70; r++) for (int c = 0; c < 140; c++) {
                float dx = c - 70.0f, dy = r * 2 - 70.0f;
                if (dx * dx + dy * dy < 70 * 70) LF.w.px[(size_t)(260 + r * 2) * LF.w.stride + 290 + c] = LF.w.px[(size_t)(261 + r * 2) * LF.w.stride + 290 + c] = 0xffff9500u;
            }
            wtext(TK_IT700, 40, 360 - wtw(TK_IT700, 40, "Firefox") / 2, 470, "Firefox", 0x15141a);
            wrr(60, 520, 600, 64, 32, 0xf0f0f4);
            wtext(TK_IT400, 24, 100, 561, "Search the web", 0x8f8f9d);
        }
    } else if (!strcmp(id, "org.gnome.Calculator")) {
        headerbar("Calculator", 92);
        wtext(TK_IT400, 72, 680 - wtw(TK_IT400, 72, "1,337"), 300, "1,337", 0x2e2e2e);
        static const char *const K[16] = { "7", "8", "9", "\xc3\xb7", "4", "5", "6", "\xc3\x97", "1", "2", "3", "\xe2\x88\x92", "0", ".", "=", "+" };
        for (int i = 0; i < 16; i++) {
            float x = 24 + 172.0f * (i % 4), y = 380 + 120.0f * (i / 4);
            wrr(x, y, 156, 104, 14, i == 14 ? 0xff7800 : i % 4 == 3 ? 0xdeddda : 0xf6f5f4);
            wtext(TK_IT500, 40, x + 78 - wtw(TK_IT500, 40, K[i]) / 2, y + 66, K[i], i == 14 ? 0xffffff : 0x2e2e2e);
        }
    } else if (!strcmp(id, "htop")) {
        wfill(0, 0, 720, SH_LINUX_H, 0x000000);
        static const char *const P[] = { "  PID USER   RES CPU% Command", "  412 user  22M  3.1 foot", "  398 user  14M  1.2 sbwl",
                                         "  431 user  88M  0.4 gnome-text-editor", "    1 root 1.2M  0.0 sbinit" };
        for (int i = 0; i < 4; i++) {
            char l[48];
            snprintf(l, sizeof l, "%d", i);
            wtext(TK_IT500, 22, 12, 36 + 34.0f * i, l, 0x33c7de);
            wfill(40, 16 + 34 * i, 600, 24, 0x262626);
            wfill(40, 16 + 34 * i, 120 + 90 * ((i * 5) % 4), 24, 0x33d17a);
        }
        for (int i = 0; i < 5; i++) {
            if (!i) wfill(0, 182, 720, 34, 0x33d17a);
            wtext(TK_IT500, 22, 12, 208 + 40.0f * i, P[i], i ? 0xdddddd : 0x000000);
        }
    } else {
        headerbar(FCAT.app[ci].name, 92);
        float w = wtw(TK_IT700, 40, FCAT.app[ci].name);
        wtext(TK_IT700, 40, 360 - w / 2, vp / 2, FCAT.app[ci].name, 0x77767b);
    }
    if (vp < SH_LINUX_H) wfill(0, vp, 720, SH_LINUX_H - vp, 0x000000);   /* below the viewport: under the keyboard */
}

/* ---- the platform (shell.h) ---- */
int sh_linux_open(const char *id)
{
    fprintf(stderr, "linux: open %s\n", id);
    snprintf(LF.pend, sizeof LF.pend, "%s", id);
    LF.shown = 1;
    LF.focus = 0;
    int r = fake_start();
    return r;
}
void sh_linux_hide(void)
{
    fprintf(stderr, "linux: hide (activate none; the apps keep running)\n");
    LF.shown = 0;
    LF.want = LF.win = -1;
    LF.pend[0] = 0;
    LF.focus = 0;
}
int sh_linux_blit(uint32_t *const *rows, int first, int *y0, int *y1)
{
    *y0 = *y1 = -1;
    if (!LF.shown || LF.win < 0 || LF.win != LF.want) return 0;
    if (!first && LF.drawn == LF.seq) return 0;
    LF.drawn = LF.seq;
    draw_window(LF.win);
    for (int y = 0; y < SH_LINUX_H; y++)
        for (int x = 0; x < SH_LINUX_W; x++) {
            uint32_t c = LF.w.px[(size_t)y * LF.w.stride + x] | 0xff000000u;
            rows[2 * y][2 * x] = rows[2 * y][2 * x + 1] = rows[2 * y + 1][2 * x] = rows[2 * y + 1][2 * x + 1] = c;
        }
    *y0 = 0;
    *y1 = 2 * SH_LINUX_H - 1;
    return SH_LINUX_H;
}
int sh_linux_touch(int phase, int x, int y)
{
    static int dy;
    fprintf(stderr, "linux: touch %s at %d,%d\n", phase == 0 ? "down" : phase == 1 ? "move" : "up", x, y);
    if (phase == 0) dy = y;
    if (phase != 2 || LF.win < 0) return 1;
    const char *id = FCAT.app[LF.win].id;
    int f = LF.focus;
    if (!strcmp(id, "org.gnome.TextEditor")) f = dy > 92;              /* the text area / the header bar */
    else if (!strcmp(id, "firefox-esr")) f = dy >= 60 && dy < 134;      /* the address field / the page */
    if (f != LF.focus) {
        LF.focus = f;
        LF.seq++;
        fprintf(stderr, "linux: text focus %d (text-input-v3)\n", f);
        sh_linux_textfocus(f);
    }
    return 1;
}
int sh_linux_char(char c)
{
    if (LF.win < 0) return 0;
    char *t = LF.text[LF.win];
    size_t n = strlen(t);
    int keys = (FCAT.app[LF.win].flags & SH_LAPP_KEYBOARD) || !LF.focus;
    if (c == '\n' || c == '\b' || keys) fprintf(stderr, "linux: key %s\n", c == '\n' ? "Return" : c == '\b' ? "BackSpace" : (char[2]){ c, 0 });
    else fprintf(stderr, "linux: text '%c'\n", c);
    if (c == '\b') { if (n) t[n - 1] = 0; }
    else if (c == '\n' && !strcmp(FCAT.app[LF.win].id, "firefox-esr")) {
        LF.loaded[LF.win] = 1;
        LF.focus = 0;
        fprintf(stderr, "linux: text focus 0 (text-input-v3)\n");
        sh_linux_textfocus(0);
    } else if (n + 1 < sizeof LF.text[0] && (LF.focus || keys)) { t[n] = c; t[n + 1] = 0; }
    LF.seq++;
    return 1;
}
int sh_linux_viewport(int rows)
{
    fprintf(stderr, "linux: viewport %d x %d\n", SH_LINUX_W, rows);
    if (rows != LF.vp) { LF.vp = rows; LF.seq++; }
    return 1;
}
int sh_linux_icon(int icon, uint32_t *px)
{
    int k = fake_kind_of_icon(icon);
    if (k < 0) return 0;
    draw_icon(k, px);
    return 1;
}
int sh_linux_hold(int on)
{
    fprintf(stderr, "linux: hold %d (the App Store %s)\n", on, on ? "open" : "closed");
    LF.hold = on;
    return on ? fake_start() : 0;
}
int sh_linux_pkg(int i, int op)
{
    if (LF.state != SH_LINUX_RUNNING || i < 0 || i >= FST.n) return -1;
    sh_lpkg_t *p = &FST.pkg[i];
    fprintf(stderr, "linux: pkg %s %s\n", op == SH_PKG_OP_INSTALL ? "install" : op == SH_PKG_OP_REMOVE ? "remove" : "cancel", p->pkg);
    if (op == SH_PKG_OP_INSTALL) { p->state = SH_PKG_QUEUED; p->pct = 0; p->err[0] = 0; }
    else if (op == SH_PKG_OP_REMOVE) { p->state = SH_PKG_REMOVING; p->pct = 0; }
    else { p->state = SH_PKG_IDLE; p->pct = 0; }
    LF.op_t[i] = LF.now;
    fake_push_store();
    return 0;
}

/* ---- time ---- */
static void lin_fake_init(void)
{
    const char *e = getenv("SBTK_LINUX");
    if (e && strcmp(e, "cold") && strcmp(e, "fail") && strcmp(e, "starting")) {
        FILE *f = fopen(e, "rb");
        int w = 0, h = 0, mx = 0;
        if (f && fscanf(f, "P6 %d %d %d", &w, &h, &mx) == 3 && w == 720 && h == 916 && fgetc(f) != EOF) {
            LF.foot = malloc((size_t)w * h * 3);
            if (LF.foot && fread(LF.foot, 3, (size_t)w * h, f) != (size_t)w * h) { free(LF.foot); LF.foot = 0; }
        }
        if (!LF.foot) fprintf(stderr, "linux: %s is not a 720x916 P6 PPM: foot drawn\n", e);
        if (f) fclose(f);
    }
    if (e && (!strcmp(e, "cold") || !strcmp(e, "fail") || !strcmp(e, "starting"))) return;
    LF.state = SH_LINUX_RUNNING;                  /* Linux ran earlier in this boot: still up, its tables known */
    sh_linux_state(SH_LINUX_RUNNING, "");
    fake_publish();
}
static void lin_fake_poll(float t)
{
    LF.now = t;
    const char *e = getenv("SBTK_LINUX");
    if (LF.state == SH_LINUX_STARTING && !(e && !strcmp(e, "starting")) && t - LF.t_state >= 1500) {
        fake_state(SH_LINUX_RUNNING, "");
        fake_publish();
    }
    if (LF.state != SH_LINUX_RUNNING) return;
    if (LF.pend[0]) {                            /* the open: once the catalog has the app */
        int ci = fcat_find(LF.pend);
        if (ci >= 0) {
            int warm = (FCAT.app[ci].flags & SH_LAPP_RUNNING) != 0;
            fprintf(stderr, "linux: launch %s (catalog %d, seq %u)%s; activate %d; viewport %d\n", FCAT.app[ci].id, ci, FCAT.seq,
                    warm ? " (running: raised)" : "", ci, LF.vp);
            LF.want = ci;
            LF.win = -1;
            LF.win_t = t + (LF.settling ? 0 : warm ? 80 : !strcmp(FCAT.app[ci].id, "firefox-esr") ? 2000 : !strcmp(FCAT.app[ci].id, SH_LINUX_TERMINAL) ? 600 : 1200);
            LF.pend[0] = 0;
        }
    }
    if (LF.want >= 0 && LF.win != LF.want && t >= LF.win_t) {
        LF.win = LF.want;
        FCAT.app[LF.win].flags |= SH_LAPP_RUNNING | SH_LAPP_WINDOW;
        FCAT.app[LF.win].rss_kb = 60000;
        FCAT.seq += 2;
        LF.seq++;
        fprintf(stderr, "linux: window of %s mapped\n", FCAT.app[LF.win].id);
        sh_linux_catalog(&FCAT);
    }
    if (LF.shown && LF.win >= 0 && LF.drawn != LF.seq) sh_linux_frame();
    /* the packages the user started advance (GIMP, VLC and Mines wait in the queue of the stills) */
    if (LF.settling) return;
    int changed = 0, cat = 0;
    for (int i = 0; i < FST.n; i++) {
        sh_lpkg_t *p = &FST.pkg[i];
        if (!LF.op_t[i]) continue;
        if (p->state == SH_PKG_QUEUED && t - LF.op_t[i] >= 400) { p->state = SH_PKG_INSTALLING; p->pct = 0; LF.op_t[i] = t; changed = 1; }
        else if ((p->state == SH_PKG_INSTALLING || p->state == SH_PKG_REMOVING) && t - LF.op_t[i] >= 70) {
            p->pct += p->state == SH_PKG_INSTALLING ? 3 : 5;
            LF.op_t[i] = t;
            changed = 1;
            if (p->pct >= 100) {
                p->pct = 100;
                if (p->state == SH_PKG_INSTALLING) { p->flags |= SH_PKG_INSTALLED; FST.overlay_free_kb -= p->inst_kb; }
                else { p->flags &= ~(unsigned)SH_PKG_INSTALLED; FST.overlay_free_kb += p->inst_kb; }
                fprintf(stderr, "linux: pkg %s %s\n", p->pkg, p->state == SH_PKG_INSTALLING ? "installed" : "removed");
                p->state = SH_PKG_IDLE;
                p->pct = 0;
                LF.op_t[i] = 0;
                cat = 1;
            }
        }
    }
    if (cat) { fake_catalog_rebuild(); sh_linux_catalog(&FCAT); }
    if (changed) fake_push_store();
}
/* a still: what would have happened in the next seconds (the start, the launches; not the packages) */
static void lin_fake_settle(void)
{
    LF.settling = 1;
    for (int i = 0; i < 40; i++) lin_fake_poll(LF.now + 100);
    LF.settling = 0;
}
/* the stills' host-only states: "focus:1" (the open app's text field focused), "type:TEXT" (typed into it) */
static void lin_fake_state(const char *st)
{
    lin_fake_settle();
    if (!strncmp(st, "focus:", 6) && LF.win >= 0) { LF.focus = atof(st + 6) > 0; LF.seq++; sh_linux_textfocus(LF.focus); }
    else if (!strncmp(st, "type:", 5)) for (const char *c = st + 5; *c; c++) sh_linux_char(*c);
    lin_fake_settle();
}

int sh_linux_zero_copy(void) { return 0; }   /* 1.0: the host has no GPU: the strips */
