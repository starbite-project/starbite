/* SPDX-License-Identifier: MIT
 * sbtk-host: a pretend Linux for the stills and films (L3, docs/STARBITE-LINUX-APPS.md; the kernel's platform is
 * starbite/guest/next/gwin.c). A store of 31 packages from the App Store's catalog (starbite/appstore/catalog: real
 * names, summaries, descriptions, licences) with the v3 fields (Track F: methods, versions, phases) in every state, the
 * catalog of the installed ones, icons drawn
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
    float now, t_state, win_t, op_t[SH_LPKGS], op_t0[SH_LPKGS];
    unsigned op_m[SH_LPKGS], op_k[SH_LPKGS];     /* Track F: the running op's method and kind (SH_PKG_OP_*) */
    char prev[SH_LPKGS][24];                     /* the version a rollback goes back to */
    char pend[40];
    unsigned seq, drawn;
    char text[SH_LAPPS][160];
    int loaded[SH_LAPPS];
    uint8_t *foot;
    tk_bmp w;                                   /* the window as the app drew it (720 x 1344) */
} LF = { .want = -1, .win = -1, .vp = 1344 };
/* 8s: the fake draws its windows at its own 720 x 1344 (2x to the panel, as before); the platform's surface is
 * SH_LINUX_W x SH_LINUX_H (1080 x 2016): the viewport and touches come in its px and are taken at 2/3 */
#define LFW 720
#define LFH 1344

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
enum { K_FOOT, K_EDIT, K_CALC, K_FILES, K_HTOP, K_MINES, K_FOX, K_GIMP, K_CHROM, K_WRITER, K_INK, K_VLC, K_TUX, K_GEANY, K_LETTER };
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
typedef struct {
    const char *pkg, *name, *summary, *cat, *err; unsigned dl, inst, ram, flags, state, pct; int kind; const char *app;
    unsigned methods, method, def, phase, done, total, eta, build_min, touch, needs, flags3, rel_mb, col; const char *txt;
    const char *vi, *vn, *vr, *vd, *vs, *lic, *src, *desc;
} fpkg;
static const fpkg FP[] = {
    { "firefox", "Firefox", "Mozilla's web browser", "Internet", "", 120000, 300000, 800, SH_PKG_FEATURED | SH_PKG_NEEDS_NET, 0, 0, K_FOX, "firefox-esr",
      6, 0, 2, 0, 0, 0, 0, 0, 2, 3, 0, 300, 0x000000, "",
      "", "", "146.0.1", "140.6.0esr", "", "MPL-2.0", "archive.mozilla.org",
      "The Firefox web browser. The release method installs Mozilla's own aarch64 build (latest Firefox); the Debian method installs Firefox ESR. Runs natively on Wayland; with no GPU, pages, WebGL and video are drawn in software." },
    { "neovim", "Neovim", "Hyperextensible Vim-based text editor", "Development", "", 16000, 40000, 30, SH_PKG_INSTALLED|SH_PKG_UPDATE|SH_PKG_ROLLBACK|SH_PKG_FEATURED, 0, 0, K_LETTER, "nvim",
      14, 2, 2, 0, 0, 0, 0, 20, 1, 32, 8, 40, 0x57a143, "Nv",
      "0.11.5", "0.12.6", "0.12.6", "0.10.4", "0.12.6", "Apache-2.0 AND Vim", "github.com/neovim/neovim",
      "Neovim is a modern Vim: modal editing, Lua plugins, built-in LSP client and Tree-sitter highlighting. Runs in the terminal." },
    { "gimp", "GIMP", "The GNU Image Manipulation Program", "Graphics", "", 92000, 230000, 350, SH_PKG_FEATURED, SH_PKG_INSTALLING, 40, K_GIMP, "gimp",
      14, 0, 2, SH_PKG_DOWNLOADING, 94200, 235500, 0, 60, 1, 16, 0, 230, 0x000000, "",
      "", "", "3.2.0", "3.0.4", "3.2.0", "GPL-3.0-or-later", "download.gimp.org",
      "GIMP is a full raster image editor: layers, masks, filters, colour tools, painting and photo retouching. The release method installs GIMP's official aarch64 AppImage (3.2 series); Debian has 3.0." },
    { "helix", "Helix", "A post-modern modal text editor", "Development", "", 32000, 80000, 40, 0, SH_PKG_INSTALLING, 35, K_LETTER, "helix",
      14, 0, 2, SH_PKG_BUILDING, 0, 0, 480, 25, 1, 32, 8, 80, 0x7c5cc4, "Hx",
      "", "", "25.07.1", "25.01", "25.07.1", "MPL-2.0", "github.com/helix-editor/helix",
      "Helix is a modal editor with multiple selections, built-in Tree-sitter highlighting and language-server support, working out of the box with no plugins. Runs in the terminal as hx." },
    { "xournalpp", "Xournal++", "Handwritten notes and PDF annotation", "Office", "", 16000, 40000, 120, SH_PKG_FEATURED, SH_PKG_INSTALLING, 92, K_LETTER, "com.github.xournalpp.xournalpp",
      14, 0, 2, SH_PKG_VERIFYING, 0, 0, 0, 20, 3, 16, 0, 40, 0x3d7fd6, "X+",
      "", "", "1.2.8", "1.2.6", "1.2.8", "GPL-2.0-or-later", "github.com/xournalpp/xournalpp",
      "Xournal++ is a notebook for handwriting, sketching and annotating PDFs, with pen, highlighter, shapes, text and LaTeX. Works with a finger or a stylus." },
    { "inkscape", "Inkscape", "Vector graphics editor (SVG)", "Graphics", "Download failed its check", 8000, 20000, 400, 0, SH_PKG_FAILED, 0, K_INK, "org.inkscape.Inkscape",
      10, 0, 1, 0, 0, 0, 0, 70, 1, 16, 0, 0, 0x000000, "",
      "", "", "", "1.4", "1.4.2", "GPL-3.0-or-later", "Debian (inkscape)",
      "Inkscape draws and edits vector graphics: shapes, paths, text, gradients and clones, saved as standard SVG and exported to PNG or PDF." },
    { "vscodium", "VSCodium", "VS Code without Microsoft's telemetry", "Development", "", 220000, 550000, 600, SH_PKG_NEEDS_NET, 0, 0, K_LETTER, "codium",
      4, 0, 2, 0, 0, 0, 0, 0, 1, 48, 0, 550, 0x2f80c8, "Vs",
      "", "", "1.106.3", "", "", "MIT", "github.com/VSCodium/vscodium",
      "VSCodium is the open-source build of Visual Studio Code: editor, terminal, Git, debugging and extensions from Open VSX. An Electron app, so it is the heaviest editor here." },
    { "libreoffice", "LibreOffice", "Office suite: Writer, Calc and Impress", "Office", "", 260000, 1400000, 350, SH_PKG_NO_SPACE|SH_PKG_FEATURED, 0, 0, K_WRITER, "libreoffice-writer",
      6, 0, 1, 0, 0, 0, 0, 0, 1, 16, 0, 650, 0x000000, "",
      "", "", "25.8.3", "25.2.3", "", "MPL-2.0", "download.documentfoundation.org",
      "LibreOffice Writer (documents), Calc (spreadsheets) and Impress (presentations), with the GTK 3 front end so it runs natively on Wayland. Opens and saves Microsoft Office and OpenDocument files." },
    { "gnome-weather", "Weather", "The forecast for your places: today, hourly and ten days", "Utilities", "", 3000, 9000, 120, SH_PKG_FEATURED, 0, 0, K_LETTER, "org.gnome.Weather",
      10, 0, 1, 0, 0, 0, 0, 4, 3, 1, 0, 0, 0xf2b630, "We",
      "", "", "", "48.0", "48.0", "GPL-2.0-or-later", "Debian (gnome-weather)",
      "GNOME Weather shows the conditions and forecast for the places you add: now, hour by hour and for the next days, with sunrise and sunset. Search a town to add it. Forecasts come from MET Norway over the internet." },
    { "kdenlive", "Kdenlive", "Video editor with a multi-track timeline", "Media", "", 8000, 20000, 700, SH_PKG_TOO_BIG, 0, 0, K_LETTER, "org.kde.kdenlive",
      2, 0, 1, 0, 0, 0, 0, 0, 1, 18, 0, 0, 0x2c3e50, "Kd",
      "", "", "", "25.04.2", "", "GPL-3.0-or-later", "Debian (kdenlive)",
      "Kdenlive is a non-linear video editor: multi-track timeline, titles, transitions, effects and rendering to common formats. Heavy for this phone: fine for short 720p clips, slow beyond that." },
    { "lazygit", "lazygit", "A simple terminal UI for Git", "Development", "", 8000, 20000, 40, SH_PKG_INSTALLED, 0, 0, K_LETTER, "lazygit",
      14, 3, 2, 0, 0, 0, 0, 6, 2, 32, 8, 20, 0xe05d44, "lg",
      "0.56.0", "0.56.0", "0.56.0", "0.50.0", "0.56.0", "MIT", "github.com/jesseduffield/lazygit",
      "lazygit shows a repository's status, branches, commits and stashes in panels and lets you stage lines, commit, rebase and push with single keys or taps." },
    { "btop", "btop", "A resource monitor for CPU, memory, disks and network", "System", "", 1200, 3000, 10, SH_PKG_INSTALLED|SH_PKG_UPDATE, 0, 0, K_HTOP, "btop",
      14, 2, 2, 0, 0, 0, 0, 6, 3, 0, 8, 3, 0x000000, "",
      "1.4.4", "1.4.5", "1.4.5", "1.4.0", "1.4.5", "Apache-2.0", "github.com/aristocratos/btop",
      "btop shows CPU per core, memory, disks, network and processes in a colourful terminal dashboard. Tap to sort, select and signal processes." },
    { "yt-dlp", "yt-dlp", "Download video and audio from YouTube and 1000+ sites", "Media", "", 16000, 40000, 60, SH_PKG_INSTALLED|SH_PKG_UPDATE|SH_PKG_ROLLBACK, 0, 0, K_LETTER, "yt-dlp",
      14, 2, 2, 0, 0, 0, 0, 3, 2, 17, 8, 40, 0xd8262c, "yt",
      "2025.10.22", "2025.11.12", "2025.11.12", "2025.09.26", "2025.11.12", "Unlicense", "github.com/yt-dlp/yt-dlp",
      "yt-dlp is a command-line downloader for video and audio from YouTube and many other sites, with format selection and subtitles. Sites change often, so the upstream release (updated every few weeks) is the default." },
    { "gnome-text-editor", "Text Editor", "A simple text editor", "Utilities", "", 8000, 20000, 90, SH_PKG_INSTALLED, 0, 0, K_EDIT, "org.gnome.TextEditor",
      10, 1, 1, 0, 0, 0, 0, 5, 3, 48, 0, 0, 0x000000, "",
      "48.3", "48.3", "", "48.3", "49.1", "GPL-3.0-or-later", "Debian (gnome-text-editor)",
      "GNOME Text Editor edits plain text and code with tabs, syntax highlighting, search and replace, and autosave of unsaved drafts." },
    { "gnome-calculator", "Calculator", "Basic, scientific and financial calculator", "Utilities", "", 8000, 20000, 70, SH_PKG_INSTALLED, 0, 0, K_CALC, "org.gnome.Calculator",
      10, 1, 1, 0, 0, 0, 0, 8, 3, 0, 0, 0, 0x000000, "",
      "48.1", "48.1", "", "48.1", "49.0", "GPL-3.0-or-later", "Debian (gnome-calculator)",
      "GNOME Calculator does everyday sums, scientific and programming calculations, and unit and currency conversion." },
    { "nautilus", "Files", "Browse and manage your files (Nautilus)", "Utilities", "", 8000, 20000, 150, SH_PKG_INSTALLED|SH_PKG_FEATURED, 0, 0, K_FILES, "org.gnome.Nautilus",
      10, 1, 1, 0, 0, 0, 0, 15, 3, 16, 0, 0, 0x000000, "",
      "48.3", "48.3", "", "48.3", "49.1", "GPL-3.0-or-later", "Debian (nautilus)",
      "GNOME Files (Nautilus) browses folders, copies, moves, renames and searches files, with previews and an adaptive layout for narrow screens." },
    { "foot", "foot", "A fast, lightweight Wayland terminal", "System", "", 8000, 20000, 20, SH_PKG_INSTALLED|SH_PKG_SYSTEM, 0, 0, K_FOOT, "org.codeberg.dnkl.foot",
      10, 1, 1, 0, 0, 0, 0, 4, 2, 32, 0, 0, 0x000000, "",
      "1.21.0", "1.21.0", "", "1.21.0", "1.24.0", "MIT", "Debian (foot)",
      "foot is a small, fast terminal emulator made for Wayland. It is the terminal the store opens command-line apps in." },
    { "gnome-mines", "Mines", "Clear hidden mines from a minefield", "Games", "", 8000, 20000, 60, SH_PKG_INSTALLED, SH_PKG_REMOVING, 60, K_MINES, "org.gnome.Mines",
      10, 1, 1, 0, 0, 0, 0, 4, 3, 0, 0, 0, 0x000000, "",
      "48.1", "48.1", "", "48.1", "48.1", "GPL-3.0-or-later", "Debian (gnome-mines)",
      "GNOME Mines is the classic minesweeper puzzle in small, medium and large boards. Tap to reveal, long-press to flag." },
    { "supertux", "SuperTux", "Classic 2D jump'n'run with Tux the penguin", "Games", "", 8000, 20000, 150, 0, 0, 0, K_TUX, "supertux2",
      10, 0, 1, 0, 0, 0, 0, 30, 1, 34, 0, 0, 0x000000, "",
      "", "", "", "0.6.3", "0.6.3", "GPL-3.0-or-later", "Debian (supertux)",
      "SuperTux is a side-scrolling platformer in the style of the classic console games: run, jump and collect coins across icy worlds. Plays with the arrow keys and space." },
    { "vlc", "VLC", "The VLC media player", "Media", "", 8000, 20000, 200, 0, SH_PKG_QUEUED, 0, K_VLC, "vlc",
      10, 0, 1, 0, 0, 0, 0, 50, 2, 18, 0, 0, 0x000000, "",
      "", "", "", "3.0.21", "3.0.21", "GPL-2.0-or-later", "Debian (vlc)",
      "VLC plays most video and audio formats, discs and network streams, with playlists, subtitles and equalizer. Uses the Qt interface on Wayland." },
    { "openttd", "OpenTTD", "Build a transport empire (Transport Tycoon)", "Games", "", 8000, 20000, 200, SH_PKG_FEATURED, 0, 0, K_LETTER, "openttd",
      10, 0, 1, 0, 0, 0, 0, 25, 2, 2, 0, 0, 0x2e6b2f, "TT",
      "", "", "", "14.1", "15.0", "GPL-2.0-only", "Debian (openttd)",
      "OpenTTD is an open remake of Transport Tycoon Deluxe: build railways, roads, ports and airports and grow a transport company. Comes with the free OpenGFX graphics, OpenSFX sounds and OpenMSX music." },
    { "gnome-maps", "Maps", "Find places and get directions", "Utilities", "", 8000, 20000, 250, SH_PKG_FEATURED, 0, 0, K_LETTER, "org.gnome.Maps",
      10, 0, 1, 0, 0, 0, 0, 5, 3, 1, 0, 0, 0x4a90d9, "Mp",
      "", "", "", "48.4", "49.1", "GPL-2.0-or-later", "Debian (gnome-maps)",
      "GNOME Maps shows OpenStreetMap maps, searches places and gives walking, cycling, driving and public-transport routes." },
    { "celluloid", "Celluloid", "A simple video player built on mpv", "Media", "", 8000, 20000, 150, SH_PKG_FEATURED, 0, 0, K_LETTER, "io.github.celluloid_player.Celluloid",
      10, 0, 1, 0, 0, 0, 0, 5, 3, 18, 0, 0, 0x4d4d9e, "Ce",
      "", "", "", "0.27", "0.29", "GPL-3.0-or-later", "Debian (celluloid)",
      "Celluloid is a GTK front end for mpv with big, touch-friendly controls, playlists and drag-and-drop. Same playback engine as mpv, friendlier interface." },
    { "ripgrep", "ripgrep", "Search file contents very fast (rg)", "Utilities", "", 2400, 6000, 20, 0, 0, 0, K_LETTER, "rg",
      14, 0, 2, 0, 0, 0, 0, 10, 1, 48, 8, 6, 0x5b5b5b, "rg",
      "", "", "15.1.0", "14.1.1", "15.1.0", "Unlicense OR MIT", "github.com/BurntSushi/ripgrep",
      "ripgrep (rg) searches folders for a pattern, respecting .gitignore and skipping binary files. Much faster than grep on large trees." },
    { "thunderbird", "Thunderbird", "Email, calendar and contacts from Mozilla", "Internet", "", 8000, 20000, 500, 0, 0, 0, K_LETTER, "thunderbird",
      2, 0, 1, 0, 0, 0, 0, 0, 2, 1, 0, 0, 0x1f6fd1, "Tb",
      "", "", "", "140.5.0esr", "", "MPL-2.0", "Debian (thunderbird)",
      "Mozilla Thunderbird: mail (IMAP/POP/SMTP), calendars, contacts, RSS and chat in one app. Installed from Debian (the ESR line); Mozilla does not publish an aarch64 Linux build." },
    { "gnome-chess", "Chess", "Play chess against the computer or a friend", "Games", "", 8000, 20000, 90, 0, 0, 0, K_LETTER, "org.gnome.Chess",
      10, 0, 1, 0, 0, 0, 0, 5, 3, 0, 0, 0, 0x8a6a3c, "Ch",
      "", "", "", "48.0", "49.0", "GPL-3.0-or-later", "Debian (gnome-chess)",
      "GNOME Chess is a simple chess board: play against a chess engine at several levels or another person on the same phone, with move hints and history." },
    { "syncthing", "Syncthing", "Continuous peer-to-peer file sync between your devices", "Internet", "", 12000, 30000, 80, 0, 0, 0, K_LETTER, "syncthing",
      6, 0, 2, 0, 0, 0, 0, 0, 2, 17, 8, 30, 0x0891d1, "St",
      "", "", "2.0.11", "1.29.5", "", "MPL-2.0", "github.com/syncthing/syncthing",
      "Syncthing keeps folders in sync between this phone and your other computers, directly and encrypted, with no cloud in the middle. It runs as a service with a web interface in the browser." },
    { "stellarium", "Stellarium", "A planetarium: the night sky in real time", "Education", "", 8000, 20000, 400, 0, 0, 0, K_LETTER, "org.stellarium.Stellarium",
      10, 0, 1, 0, 0, 0, 0, 45, 2, 4, 0, 0, 0x1b2a4a, "St",
      "", "", "", "25.2", "25.3", "GPL-2.0-or-later", "Debian (stellarium)",
      "Stellarium shows a realistic sky with stars, planets, constellations and deep-sky objects for any place and time." },
    { "gcompris-qt", "GCompris", "Educational games for children aged 2 to 10", "Education", "", 8000, 20000, 250, 0, 0, 0, K_LETTER, "org.kde.gcompris",
      10, 0, 1, 0, 0, 0, 0, 30, 3, 2, 0, 0, 0xf0a030, "Gc",
      "", "", "", "25.0", "25.1", "AGPL-3.0-only", "Debian (gcompris-qt)",
      "GCompris has more than 180 activities for young children: reading, counting, clocks, geography, science, puzzles and games. Designed for touch screens." },
    { "qalculate-gtk", "Qalculate!", "A powerful calculator with units and symbols", "Science", "", 8000, 20000, 70, 0, 0, 0, K_LETTER, "qalculate-gtk",
      10, 0, 1, 0, 0, 0, 0, 8, 2, 0, 0, 0, 0x3a6ea5, "Q!",
      "", "", "", "5.5.1", "5.8.2", "GPL-2.0-or-later", "Debian (qalculate-gtk)",
      "Qalculate! understands plain expressions with units, currencies, constants, functions, symbolic algebra and plotting. Results update as you type." },
    { "calibre", "calibre", "E-book library manager, converter and viewer", "Office", "", 260000, 650000, 350, 0, 0, 0, K_LETTER, "calibre-gui",
      14, 0, 1, 0, 0, 0, 0, 25, 1, 16, 0, 650, 0x7a3e1d, "Ca",
      "", "", "8.15.0", "8.5.0", "8.15.0", "GPL-3.0-only", "github.com/kovidgoyal/calibre",
      "calibre organises an e-book library, converts between formats, edits EPUBs, fetches news and has its own viewer. The release method installs the upstream arm64 bundle (newest version, but large)." },
    { "aisleriot", "AisleRiot Solitaire", "More than 80 solitaire card games", "Games", "", 8000, 20000, 80, 0, 0, 0, K_LETTER, "sol",
      10, 0, 1, 0, 0, 0, 0, 10, 3, 0, 0, 0, 0x2f7d3a, "Ai",
      "", "", "", "3.22.35", "3.22.35", "GPL-3.0-or-later", "Debian (aisleriot)",
      "AisleRiot plays Klondike, Spider, FreeCell and over 80 other solitaire card games. Drag cards with a finger or tap to move them." },
};
#define NFP ((int)(sizeof FP / sizeof FP[0]))
static void fake_push_store(void)
{
    FST.seq += 2;
    sh_linux_store(&FST);
}
static int fp_find(const char *pkg) { for (int i = 0; i < NFP; i++) if (!strcmp(FP[i].pkg, pkg)) return i; return -1; }
/* the .desktop Name of a Debian-installed app where it isn't the store's name (Debian trixie, 2026-11-14) */
static const char *fake_desk_name(const char *id, const char *name)
{
    static const char *const T[][2] = { { "firefox-esr", "Firefox ESR" }, { "gimp", "GNU Image Manipulation Program" },
                                        { "vlc", "VLC media player" }, { "supertux2", "SuperTux 2" } };
    for (unsigned k = 0; k < sizeof T / sizeof T[0]; k++) if (!strcmp(id, T[k][0])) return T[k][1];
    return name;
}
static int fcat_pkg[SH_LAPPS];                  /* the fake's own link (sbwl publishes none) */
static void fake_catalog_rebuild(void)
{
    /* the installed packages' apps, in the store's order (foot first: the Terminal) */
    sh_lcat_t old = FCAT;
    memset(&FCAT, 0, sizeof FCAT);
    FCAT.seq = old.seq + 2;
    FCAT.mem_total_kb = 1490000;
    FCAT.mem_avail_kb = 1180000;
    int foot = fp_find("foot");
    for (int o = -1; o < NFP; o++) {
        int i = o < 0 ? foot : o;
        if (i < 0 || (o >= 0 && i == foot) || !(FST.pkg[i].flags & SH_PKG_INSTALLED)) continue;
        sh_lapp_t *a = &FCAT.app[FCAT.n];
        snprintf(a->id, sizeof a->id, "%s", FP[i].app);
        snprintf(a->name, sizeof a->name, "%s", i == foot ? "Foot" : fake_desk_name(FP[i].app, FP[i].name));
        a->flags = i == foot || (FP[i].flags3 & SH_PKG3_TERMINAL) ? SH_LAPP_KEYBOARD : 0;
        if (FP[i].flags3 & SH_PKG3_TERMINAL) a->flags |= SH_LAPP_TERMINAL;
        if (FP[i].flags & SH_PKG_SYSTEM) a->flags |= SH_LAPP_SYSTEM;
        a->icon = FCAT.n;                       /* the catalog's pool icons 0..63 */
        a->pkg = -1;                            /* as sbwl's cat_publish: the store finds its app by id / name */
        fcat_pkg[FCAT.n] = i;
        for (int k = 0; k < old.n; k++)
            if (!strcmp(old.app[k].id, a->id)) { a->flags |= old.app[k].flags & (SH_LAPP_RUNNING | SH_LAPP_WINDOW); a->rss_kb = old.app[k].rss_kb; }
        FCAT.n++;
    }
}
/* the store's package of a pool icon (64 + i), or of a catalog icon; -1 none */
static int fake_pkg_of_icon(int icon)
{
    if (icon >= 64 && icon < 64 + NFP) return icon - 64;
    if (icon >= 0 && icon < FCAT.n) return fcat_pkg[icon];
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
        FST.v3 = 1;                              /* Track F: store3 */
        FST.net_ok = 0;                          /* no network in the previews: the memory card's mirror */
        FST.net_why = SH_NET_NO_CONNECTION;
        FST.mirror_ok = 1;
        FST.checked_at = 0;
        for (int i = 0; i < NFP; i++) {
            sh_lpkg_t *p = &FST.pkg[i];
            const fpkg *f = &FP[i];
            snprintf(p->pkg, sizeof p->pkg, "%s", f->pkg);
            snprintf(p->name, sizeof p->name, "%s", f->name);
            snprintf(p->summary, sizeof p->summary, "%s", f->summary);
            snprintf(p->category, sizeof p->category, "%s", f->cat);
            snprintf(p->err, sizeof p->err, "%s", f->err);
            if (f->state == SH_PKG_FAILED)       /* the engine's what-to-do sentence (sbstore.py HINTS) */
                snprintf(p->hint, sizeof p->hint, "%s", "The file didn't match the project's published checksum, so it "
                         "was not installed. Try again later, or choose another install method.");
            p->dl_kb = f->dl; p->inst_kb = f->inst; p->ram_mb = f->ram;
            p->flags = f->flags; p->state = f->state; p->pct = f->pct;
            p->icon = 64 + i;
            p->methods = f->methods; p->method = f->method; p->def_method = f->method ? f->method : f->phase == SH_PKG_BUILDING ? SH_M_SOURCE : f->def;   /* a build: the user picked source */ p->phase = f->phase;
            p->done_kb = f->done; p->total_kb = f->total; p->eta_s = f->eta; p->build_min = f->build_min;
            p->touch = f->touch; p->needs = f->needs; p->flags3 = f->flags3; p->rel_size_mb = f->rel_mb;
            snprintf(p->ver_inst, sizeof p->ver_inst, "%s", f->vi);
            snprintf(p->ver_new, sizeof p->ver_new, "%s", f->vn[0] ? f->vn : f->def == SH_M_RELEASE ? f->vr : f->def == SH_M_SOURCE ? f->vs : f->vd);
            snprintf(p->ver_rel, sizeof p->ver_rel, "%s", f->vr);
            snprintf(p->ver_distro, sizeof p->ver_distro, "%s", f->vd);
            snprintf(p->ver_src, sizeof p->ver_src, "%s", f->vs);
            snprintf(p->licence, sizeof p->licence, "%s", f->lic);
            snprintf(p->source, sizeof p->source, "%s", f->src);
            snprintf(p->desc, sizeof p->desc, "%s", f->desc);
            if (f->flags & SH_PKG_ROLLBACK) snprintf(LF.prev[i], sizeof LF.prev[i], "%s", !strcmp(f->pkg, "neovim") ? "0.11.4" : "2025.09.26");
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
        LF.w.px = calloc((size_t)LFW * LFH, 4);
        if (!LF.w.px) return;
        LF.w.w = LF.w.stride = LFW;
        LF.w.h = LFH;
    }
    const char *id = FCAT.app[ci].id, *t = LF.text[ci];
    int vp = LF.vp;
    wfill(0, 0, 720, LFH, 0xffffff);
    if (!strcmp(id, SH_LINUX_TERMINAL)) {
        wfill(0, 0, 720, LFH, 0x242424);
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
        wfill(0, 0, 720, LFH, 0x000000);
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
    if (vp < LFH) wfill(0, vp, 720, LFH - vp, 0x000000);   /* below the viewport: under the keyboard */
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
    for (int y = 0; y < LFH; y++)
        for (int x = 0; x < LFW; x++) {
            uint32_t c = LF.w.px[(size_t)y * LF.w.stride + x] | 0xff000000u;
            rows[2 * y][2 * x] = rows[2 * y][2 * x + 1] = rows[2 * y + 1][2 * x] = rows[2 * y + 1][2 * x + 1] = c;
        }
    *y0 = 0;
    *y1 = 2 * LFH - 1;
    return LFH;
}
int sh_linux_touch(int phase, int x, int y)
{
    static int dy;
    fprintf(stderr, "linux: touch %s at %d,%d\n", phase == 0 ? "down" : phase == 1 ? "move" : "up", x, y);
    x = x * 2 / 3, y = y * 2 / 3;                 /* (8s: into the fake's 720 x 1344) */
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
    rows = rows * 2 / 3;                          /* (8s: into the fake's 720 x 1344) */
    if (rows != LF.vp) { LF.vp = rows; LF.seq++; }
    return 1;
}
int sh_linux_icon(int icon, uint32_t *px)
{
    int i = fake_pkg_of_icon(icon);
    if (i < 0) return 0;
    if (FP[i].kind != K_LETTER) { draw_icon(FP[i].kind, px); return 1; }
    /* Track F: the catalog's other apps: a colour tile with two letters (the real icons come from the guest) */
    memset(px, 0, 160 * 160 * 4);
    ic_rrect(px, 10, 10, 140, 140, 30, pm(FP[i].col, 1));
    ic_rrect(px, 10, 80, 140, 70, 30, pm(0x000000, .14f));
    ic_rrect(px, 10, 10, 140, 90, 30, pm(FP[i].col, 1));
    ic_text(px, 64, 80, 104, FP[i].txt, 0xffffff, TK_IT800);
    return 1;
}
int sh_linux_hold(int on)
{
    fprintf(stderr, "linux: hold %d (the App Store %s)\n", on, on ? "open" : "closed");
    LF.hold = on;
    return on ? fake_start() : 0;
}
/* Track F: the op word (op | method << 8; method 0: the entry's def_method); i SH_PKG_ALL: CHECK every installed app */
int sh_linux_pkg(int i, int opw)
{
    int op = opw & 255;
    unsigned m = (unsigned)opw >> 8 & 255;
    static const char *const ON[7] = { "?", "install", "remove", "cancel", "update", "rollback", "check" };
    static const char *const MN[4] = { "default", "distro", "release", "source" };
    if (LF.state != SH_LINUX_RUNNING || op < 1 || op > 6 || m > 3) return -1;
    if (i == SH_PKG_ALL && op == SH_PKG_OP_CHECK) {
        fprintf(stderr, "linux: pkg check (every installed app)\n");
        FST.checked_at = 1762981200u;            /* 2025-11-12 21:00 UTC (the previews' day) */
        int lg = fp_find("lazygit");             /* the check finds one more update */
        if (lg >= 0 && (FST.pkg[lg].flags & SH_PKG_INSTALLED) && !(FST.pkg[lg].flags & SH_PKG_UPDATE)) {
            FST.pkg[lg].flags |= SH_PKG_UPDATE;
            snprintf(FST.pkg[lg].ver_new, sizeof FST.pkg[lg].ver_new, "%s", "0.57.0");
            snprintf(FST.pkg[lg].ver_src, sizeof FST.pkg[lg].ver_src, "%s", "0.57.0");
            snprintf(FST.pkg[lg].ver_rel, sizeof FST.pkg[lg].ver_rel, "%s", "0.57.0");
        }
        fake_push_store();
        return 0;
    }
    if (i < 0 || i >= FST.n) return -1;
    sh_lpkg_t *p = &FST.pkg[i];
    if (!m) m = op == SH_PKG_OP_UPDATE || op == SH_PKG_OP_ROLLBACK ? p->method : p->def_method;
    fprintf(stderr, "linux: pkg %s %s (%s)\n", ON[op], p->pkg, MN[m & 3]);
    if (op == SH_PKG_OP_CHECK) { fake_push_store(); return 0; }
    if (op == SH_PKG_OP_CANCEL) { p->state = SH_PKG_IDLE; p->phase = 0; p->pct = 0; LF.op_t[i] = 0; fake_push_store(); return 0; }
    if (op != SH_PKG_OP_REMOVE && m && !(p->methods & 1u << m) && p->methods) return -1;   /* not offered */
    if (op == SH_PKG_OP_REMOVE) { p->state = SH_PKG_REMOVING; p->phase = SH_PKG_REMOVING; }
    else { p->state = SH_PKG_QUEUED; p->phase = SH_PKG_QUEUED; if (op == SH_PKG_OP_INSTALL) p->def_method = m; }
    p->pct = 0; p->done_kb = 0; p->eta_s = 0; p->err[0] = 0;
    LF.op_k[i] = (unsigned)op;
    LF.op_m[i] = m;
    LF.op_t[i] = LF.op_t0[i] = LF.now > 0 ? LF.now : 1;
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
        if (!LF.op_t[i] || t - LF.op_t[i] < 70) continue;
        LF.op_t[i] = t;
        changed = 1;
        unsigned m = LF.op_m[i], k = LF.op_k[i];
        float e = t - LF.op_t0[i];               /* ms since the op started */
        if (p->state == SH_PKG_REMOVING) {
            p->pct = (unsigned)(e / 12 > 100 ? 100 : e / 12);
            if (p->pct >= 100) {
                p->flags &= ~(unsigned)(SH_PKG_INSTALLED | SH_PKG_UPDATE | SH_PKG_ROLLBACK);
                FST.overlay_free_kb += p->inst_kb;
                p->method = 0; p->ver_inst[0] = 0;
                fprintf(stderr, "linux: pkg %s removed\n", p->pkg);
                p->state = p->phase = SH_PKG_IDLE; p->pct = 0; LF.op_t[i] = 0; cat = 1;
            }
            continue;
        }
        /* the queue (400 ms), then by method: a release / distro download (2 s), verify (500 ms); a source build
         * (a short source download, then 3.5 s for build_min, the ETA counting down); install (600 ms) */
        float dl = m == SH_M_SOURCE ? 600 : 2000, vf = m == SH_M_SOURCE ? 0 : 500, bd = m == SH_M_SOURCE ? 3500 : 0, in = 600;
        if (k == SH_PKG_OP_ROLLBACK) dl = vf = bd = 0;
        float a = e - 400;
        unsigned tot = m == SH_M_SOURCE ? 9000 : p->dl_kb ? p->dl_kb : 20000;
        if (a < 0) { p->state = SH_PKG_QUEUED; p->phase = SH_PKG_QUEUED; continue; }
        p->state = SH_PKG_INSTALLING;
        if (a < dl) { p->phase = SH_PKG_DOWNLOADING; p->total_kb = tot; p->done_kb = (unsigned)(tot * (a / dl)); p->pct = (unsigned)(100 * a / dl); }
        else if (a < dl + vf) { p->phase = SH_PKG_VERIFYING; p->done_kb = p->total_kb; p->pct = 100; }
        else if (a < dl + vf + bd) {
            float f = (a - dl - vf) / bd;
            p->phase = SH_PKG_BUILDING; p->pct = (unsigned)(100 * f);
            p->eta_s = (unsigned)((1 - f) * (p->build_min ? p->build_min : 10) * 60);
        } else if (a < dl + vf + bd + in) { p->phase = SH_PKG_INSTALLING; p->pct = (unsigned)(100 * (a - dl - vf - bd) / in); p->eta_s = 0; }
        else {
            const char *v = m == SH_M_RELEASE ? p->ver_rel : m == SH_M_SOURCE ? p->ver_src : p->ver_distro;
            if (k == SH_PKG_OP_ROLLBACK) {
                char cur[24];
                snprintf(cur, sizeof cur, "%s", p->ver_inst);
                snprintf(p->ver_inst, sizeof p->ver_inst, "%s", LF.prev[i][0] ? LF.prev[i] : p->ver_inst);
                snprintf(p->ver_new, sizeof p->ver_new, "%s", cur);
                p->flags = (p->flags & ~(unsigned)SH_PKG_ROLLBACK) | SH_PKG_UPDATE;
            } else {
                if (k == SH_PKG_OP_UPDATE || (p->flags & SH_PKG_INSTALLED)) { snprintf(LF.prev[i], sizeof LF.prev[i], "%s", p->ver_inst); if (p->ver_inst[0]) p->flags |= SH_PKG_ROLLBACK; }
                if (!(p->flags & SH_PKG_INSTALLED)) FST.overlay_free_kb -= p->inst_kb;
                snprintf(p->ver_inst, sizeof p->ver_inst, "%s", v[0] ? v : p->ver_new);
                snprintf(p->ver_new, sizeof p->ver_new, "%s", p->ver_inst);
                p->flags = (p->flags | SH_PKG_INSTALLED) & ~(unsigned)SH_PKG_UPDATE;
                p->method = m;
            }
            fprintf(stderr, "linux: pkg %s %s %s\n", p->pkg, k == SH_PKG_OP_ROLLBACK ? "rolled back to" : "installed", p->ver_inst);
            p->state = p->phase = SH_PKG_IDLE; p->pct = 0; p->done_kb = p->total_kb = p->eta_s = 0; LF.op_t[i] = 0; cat = 1;
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
/* cp6: Track E's store job in words (gwin.c); SBTK_JOB="Building btop from source, 45 %" pretends one runs */
int sh_linux_job(char *buf, int cap)
{
    const char *j = getenv("SBTK_JOB");
    if (buf && cap > 0) snprintf(buf, (size_t)cap, "%s", j ? j : "");
    return j && j[0];
}
