/* SPDX-License-Identifier: GPL-2.0-only
 * gwin-host: the kernel glue (starbite/guest/next/gwin.c, L2 + L3) and its unattended check on the host, with the real
 * Next shell (starbite/sbos/ui: shell_lock.c, the toolkit, host/plat.c, host/main.c's other platform fakes) and a fake
 * guest in place of kern/guest.c + sbghost (contract v2): it comes up 1.5 s after the start and publishes a catalog
 * (foot, Text Editor; their icons) and a store (foot, gnome-text-editor, gimp) 200 ms later; a LAUNCH + ACTIVATE puts
 * the app's window into the surface 400 ms later (SBG_MSG_WINDOW); foot's frame is GWIN_PPM (a 720 x 916 P6 PPM from
 * the QEMU run; else a dark surface) with each typed character as a block (a new frame per key); a tap in the Text
 * Editor's window focuses its field (SBG_MSG_TEXT_FOCUS); foot reports a focused field too (text-input-v3, purpose
 * terminal) and must still get keys; the catalog's seq moves every 2 s (rss) while its layout_seq, which LAUNCH
 * carries, stays; a package install runs 10 % a tick. The marker is set when
 * a typed line holds STARBITE-TERM-OK; off 0.7 s after the stop. A fake clock steps 100 ms per turn.
 *   1. gwin.c's self test (the Terminal) until it passes;
 *   2. the L3 paths through the shell: the home's Linux page, a Linux app opened by its id (launch, activate, the
 *      viewport), its text field (the keyboard up, viewport 916, the typed text as SBG_MSG_TEXT, Return as a key),
 *      home (activate none), the App Store's detail page and its Install button (SBG_MSG_PKG with the store's seq),
 *      the progress back in the shell, a refused remove of a system package.
 * Writes OUT/gwin-typed.ppm (the Terminal after the command), OUT/gwin-home3.ppm, OUT/gwin-editor.ppm and
 * OUT/gwin-store.ppm; prints the log; exit 0 when everything passed.
 *   starbite/guest/next/host/run.sh UI_TREE PAK OUT     (UI_TREE: a checkout of the toolkit with next-linux.patch)
 * GWIN_STORE=none: the store has no guest-wl files (the check must fail with that reason).
 */
#include "sb.h"
#include "kern.h"
#include "guest.h"
#include "sbtk.h"
#include "shell.h"
#include "../../host/sbghost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gwin_now(void);
int gwin_selftest_step(void);

/* ---- the fake clock and log ---- */
static uint64_t now_ms = 1000;
uint64_t sb_ms(void) { return now_ms; }
void sb_log(const char *s) { printf("[%6llu ms] %s\n", (unsigned long long)now_ms, s); }
void sb_logf(const char *a, const char *b) { printf("[%6llu ms] %s%s\n", (unsigned long long)now_ms, a, b); }
void ub_init(struct ub *u) { u->n = 0; u->b[0] = 0; }
void ub_s(struct ub *u, const char *s) { while (*s && u->n + 1 < sizeof u->b) u->b[u->n++] = *s++; u->b[u->n] = 0; }
void ub_dec(struct ub *u, uint64_t v)
{
    char t[24];
    snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    ub_s(u, t);
}

/* ---- the fake guest (kern/guest.c's API as gwin.c uses it) ---- */
static int st = GUEST_OFF, shown, mode = GUEST_APP, term_ok, busy;
static uint64_t t_state;
static uint32_t seq, drawn;
static char line[128], typed[512];
static int nline, ntyped;
static uint8_t *rgb;
static struct sbg_stats stats;
static const char *why = "";

/* contract v2 in plain memory */
static struct sbshm_cat fcat;
/* Track F: store3: the fake store is the whole store region (store, descriptions, store3 up to the icon pool), so
 * gwin.c's store3 read beside it stays inside (zero: no store3 magic = a v2 guest) */
static union { uint8_t region[SBSHM_ICON_OFF - SBSHM_STORE_OFF]; struct sbshm_store s; } fstore_region
    __attribute__((aligned(64)));
#define fstore (fstore_region.s)
static uint32_t icons[4][SBSHM_ICON_PX * SBSHM_ICON_PX];
static unsigned changes;
static uint32_t win = SBSHM_NONE, act = SBSHM_NONE, vp = 0, focus;
static int has_win;
static uint64_t t_win, t_pkg;
static int published;
static struct { int launch, activate, activate_none, viewport, text, keys, pkg, pkg_seq_ok; uint32_t last_vp; char text_s[64]; } got;

int guest_available_mode(int m) { return m == GUEST_WL && !(getenv("GWIN_STORE") && !strcmp(getenv("GWIN_STORE"), "none")); }
int guest_start_mode(int m)
{
    if (st != GUEST_OFF && st != GUEST_FAILED) return -1;
    if (!guest_available_mode(m)) { why = "the Linux apps' files are not in the store"; return -4; }
    mode = m;
    st = GUEST_STARTING;
    t_state = now_ms;
    term_ok = 0;
    seq = drawn = 0;
    ntyped = nline = 0;
    why = "";
    memset(&fcat, 0, sizeof fcat);       /* sbghost's load clears the v2 tables */
    memset(&fstore, 0, sizeof fstore);
    published = 0;
    win = act = SBSHM_NONE;
    has_win = 0;
    return 0;
}
int guest_state(void) { return st; }
int guest_mode(void) { return mode; }
const char *guest_why(void) { return why; }
int guest_stop(void) { if (st == GUEST_RUNNING || st == GUEST_STARTING) { st = GUEST_STOPPING; t_state = now_ms; } return 0; }
int guest_owns_m3(void) { return st != GUEST_OFF && st != GUEST_FAILED; }
int hw_m3_on(void) { return guest_owns_m3(); }
void guest_show(int on) { shown = on; drawn = 0; }
void guest_keep(int on) { busy = on; }
int guest_shown(void) { return shown && st == GUEST_RUNNING; }
void guest_full_next(void) { drawn = 0; }
int guest_frame_new(void) { return guest_shown() && seq != drawn; }
int guest_term_ok(void) { return term_ok; }
int guest_type(char c)
{
    if (!guest_shown()) return 0;
    got.keys++;
    if (c == '\n') {
        line[nline] = 0;
        if (strstr(line, "STARBITE-TERM-OK")) { term_ok = 1; sb_log("linux| STARBITE-TERM-OK (fake)"); }
        nline = 0;
    } else if (nline + 1 < (int)sizeof line)
        line[nline++] = c;
    if (ntyped + 1 < (int)sizeof typed) typed[ntyped++] = c;
    seq++;
    return 1;
}
int sbg_touch(uint32_t slot, uint32_t phase, int32_t x, int32_t y)
{
    (void)slot;
    printf("[%6llu ms] fake guest: touch phase %u at %d,%d\n", (unsigned long long)now_ms, phase, x, y);
    stats.touches_sent++;
    if (phase == SBG_TOUCH_UP && win == 1 && y > 60 && !focus) {   /* the Text Editor: its text area */
        focus = 1;
        changes |= SBG_CH_TEXT;
        sb_log("fake guest: the Text Editor's field focused (TEXT_FOCUS 1)");
    }
    return 1;
}
const struct sbg_stats *sbg_stats(void) { return &stats; }

unsigned sbg_changes(void) { unsigned c = changes; changes = 0; return c; }
const volatile struct sbshm_cat *sbg_cat(void) { return &fcat; }
const volatile struct sbshm_store *sbg_store(void) { return &fstore; }
const volatile uint32_t *sbg_icon(uint32_t i)
{
    return i < 2 ? icons[i] : i >= SBSHM_ICONS_CAT && i < SBSHM_ICONS_CAT + 2 ? icons[2 + i - SBSHM_ICONS_CAT] : 0;
}
uint32_t sbg_window(int *h) { if (h) *h = has_win; return win; }
int sbg_text_focus(uint32_t *purpose) { if (purpose) *purpose = 0; return (int)focus; }
int sbg_launch(uint32_t idx, uint32_t cat_seq)
{
    printf("[%6llu ms] fake guest: LAUNCH %u (layout_seq %u)\n", (unsigned long long)now_ms, idx, cat_seq);
    if (cat_seq != fcat.layout_seq || idx >= fcat.n) return 1;   /* stale: ignored, as the contract says */
    got.launch++;
    return 1;
}
int sbg_activate(uint32_t idx)
{
    printf("[%6llu ms] fake guest: ACTIVATE %d\n", (unsigned long long)now_ms, (int)idx);
    if (idx == SBSHM_NONE) { got.activate_none++; act = win = SBSHM_NONE; has_win = 0; changes |= SBG_CH_WINDOW; focus = 0; return 1; }
    got.activate++;
    act = idx;
    focus = 0;
    t_win = now_ms + 400;
    return 1;
}
int sbg_app_close(uint32_t idx) { (void)idx; return 1; }
int sbg_viewport(uint32_t rows, uint32_t width)
{
    printf("[%6llu ms] fake guest: VIEWPORT %u x %u\n", (unsigned long long)now_ms, width, rows);
    got.viewport++;
    got.last_vp = rows;
    vp = rows;
    seq++;
    return 1;
}
int sbg_text(const char *s)
{
    printf("[%6llu ms] fake guest: TEXT \"%s\"\n", (unsigned long long)now_ms, s);
    got.text++;
    strncat(got.text_s, s, sizeof got.text_s - strlen(got.text_s) - 1);
    seq++;
    return 1;
}
int sbg_pkg(uint32_t idx, uint32_t op, uint32_t store_seq)
{
    printf("[%6llu ms] fake guest: PKG %u op %u (seq %u)\n", (unsigned long long)now_ms, idx, op, store_seq);
    got.pkg++;
    got.pkg_seq_ok += store_seq == fstore.seq;
    if (store_seq != fstore.seq || idx >= fstore.n) return 1;
    volatile struct sbshm_pkg *p = &fstore.pkg[idx];
    fstore.seq++;
    if (op == SBG_PKG_OP_INSTALL) { p->state = SBG_PKG_QUEUED; p->pct = 0; t_pkg = now_ms; }
    fstore.seq++;
    changes |= SBG_CH_PKG;
    return 1;
}

static void put_s(volatile char *d, const char *s, unsigned cap) { unsigned k = 0; for (; s[k] && k + 1 < cap; k++) d[k] = s[k]; d[k] = 0; }
static void publish(void)
{
    fcat.seq = 1;
    fcat.n = 2;
    fcat.mem_total_kb = 1490000;
    put_s(fcat.app[0].id, "org.codeberg.dnkl.foot", 40); put_s(fcat.app[0].name, "Foot", 32);
    fcat.app[0].flags = SBSHM_APP_KEYBOARD | SBSHM_APP_SYSTEM; fcat.app[0].icon = 0; fcat.app[0].pkg = 0;
    put_s(fcat.app[1].id, "org.gnome.TextEditor", 40); put_s(fcat.app[1].name, "Text Editor", 32);
    fcat.app[1].flags = 0; fcat.app[1].icon = 1; fcat.app[1].pkg = 1;
    fcat.layout_seq = 7;
    fcat.seq = 2;
    fstore.seq = 1;
    fstore.n = 3;
    fstore.repo_ok = 1;
    fstore.overlay_total_kb = 3900000;
    fstore.overlay_free_kb = 1100000;
    static const char *const P[3][4] = { { "foot", "Foot", "The terminal.", "Development" },
                                         { "gnome-text-editor", "Text Editor", "A simple editor.", "Utilities" },
                                         { "gimp", "GIMP", "Photos and painting.", "Graphics" } };
    for (int i = 0; i < 3; i++) {
        volatile struct sbshm_pkg *p = &fstore.pkg[i];
        put_s(p->pkg, P[i][0], 40); put_s(p->name, P[i][1], 32); put_s(p->summary, P[i][2], 96); put_s(p->category, P[i][3], 16);
        p->dl_kb = 1000u * (i + 1); p->inst_kb = 4000u * (i + 1); p->ram_mb = 50; p->icon = i < 2 ? SBSHM_ICONS_CAT + i : SBSHM_NONE;
        p->flags = i < 2 ? SBSHM_PKG_INSTALLED : SBSHM_PKG_FEATURED;
        if (!i) p->flags |= SBSHM_PKG_SYSTEM;
    }
    fstore.seq = 2;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < (int)(SBSHM_ICON_PX * SBSHM_ICON_PX); i++) {
            int x = i % (int)SBSHM_ICON_PX - 80, y = i / (int)SBSHM_ICON_PX - 80;
            icons[k][i] = x * x + y * y < 60 * 60 ? (k & 1 ? 0xff3584e4u : 0xff2a2f3au) : 0;
        }
    changes |= SBG_CH_CATALOG | SBG_CH_STORE;
    published = 1;
    sb_log("fake guest: the catalog (2 apps) and the store (3 packages) published");
}
/* the surface at 2x, opaque: foot's frame and the typed characters as 16 x 28 blocks from row 260 (45 a row); the
 * Text Editor: white, its typed text as blocks */
int guest_blit_rows(uint32_t *const *rows, int *py0, int *py1)
{
    *py0 = *py1 = -1;
    if (!guest_shown()) return 0;
    drawn = seq;
    for (int y = 0; y < SH_LINUX_H; y++)
        for (int x = 0; x < SH_LINUX_W; x++) {
            uint32_t c = win == 1 ? 0xffffffffu : 0xff242424u;
            if (win != 1 && rgb && y < 916) {
                const uint8_t *p = rgb + ((size_t)y * SH_LINUX_W + x) * 3;
                c = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            }
            int cell = y >= 260 ? (y - 260) / 32 * 45 + x / 16 : -1, n = win == 1 ? (int)strlen(got.text_s) : ntyped;
            if (cell >= 0 && cell < n && (win == 1 ? got.text_s[cell] : typed[cell]) != ' ' && x % 16 < 12 && (y - 260) % 32 > 4 && (y - 260) % 32 < 28)
                c = win == 1 ? 0xff202020u : 0xffddddddu;
            if (vp && y >= (int)vp) c = 0xff000000u;
            rows[2 * y][2 * x] = rows[2 * y][2 * x + 1] = rows[2 * y + 1][2 * x] = rows[2 * y + 1][2 * x + 1] = c;
        }
    *py0 = 0;
    *py1 = 2 * SH_LINUX_H - 1;
    return SH_LINUX_H;
}
static void guest_tick(void)
{
    if (st == GUEST_STARTING && now_ms - t_state >= 1500) { st = GUEST_RUNNING; t_state = now_ms; seq = 1; sb_log("fake guest: HELLO"); }
    if (st == GUEST_RUNNING && !published && now_ms - t_state >= 200 && !getenv("GWIN_L2")) publish();
    if (st == GUEST_RUNNING && getenv("GWIN_L2") && act == SBSHM_NONE) has_win = 0;   /* an L2 guest: foot always */
    if (st == GUEST_RUNNING && published && now_ms % 2000 == 0) {   /* sbwl's rss refresh: seq moves, layout_seq not */
        fcat.seq++;
        fcat.app[0].rss_kb += 4;
        fcat.seq++;
        changes |= SBG_CH_CATALOG;
    }
    if (st == GUEST_RUNNING && act != SBSHM_NONE && win != act && now_ms >= t_win) {
        win = act;
        has_win = 1;
        seq++;
        changes |= SBG_CH_WINDOW;
        if (win == 0) {                     /* foot binds text-input-v3 too: TEXT_FOCUS 1, purpose terminal */
            focus = 1;
            changes |= SBG_CH_TEXT;
        }
        printf("[%6llu ms] fake guest: WINDOW %u in the surface\n", (unsigned long long)now_ms, win);
    }
    if (t_pkg && now_ms - t_pkg >= 100) {
        volatile struct sbshm_pkg *p = &fstore.pkg[2];
        fstore.seq++;
        if (p->state == SBG_PKG_QUEUED) p->state = SBG_PKG_INSTALLING;
        else if (p->pct < 100) p->pct += 10;
        if (p->pct >= 100) { p->state = SBG_PKG_IDLE; p->flags |= SBSHM_PKG_INSTALLED; t_pkg = 0; }
        else t_pkg = now_ms;
        fstore.seq++;
        changes |= SBG_CH_PKG;
    }
    if (st == GUEST_STOPPING && now_ms - t_state >= 700) { st = GUEST_OFF; shown = 0; sb_log("fake guest: off"); }
}

/* ---- the harness ---- */
static uint8_t *readall(const char *p, uint32_t *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *len = (uint32_t)n;
    return b;
}
static void write_ppm(const char *dir, const char *name, const tk_bmp *b)
{
    char out[512];
    snprintf(out, sizeof out, "%s/%s", dir, name);
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return; }
    fprintf(f, "P6\n%d %d\n255\n", b->w, b->h);
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            uint32_t c = b->px[(size_t)y * b->stride + x];
            fputc(c >> 16 & 255, f); fputc(c >> 8 & 255, f); fputc(c & 255, f);
        }
    fclose(f);
}
static tk_comp C;
static int draws;
/* n turns of 100 ms: the guest, the kernel's loop (gwin_now), the shell's springs */
static void run(int n)
{
    for (int i = 0; i < n; i++) {
        now_ms += 100;
        guest_tick();
        if (gwin_now()) draws++;
        sh_tick(.1f, (float)now_ms);
        tk_comp_frame(&C, 0, 0);
    }
}
static int fails;
static void check(int ok, const char *what)
{
    printf("[%6llu ms] gwin-host: %s: %s\n", (unsigned long long)now_ms, ok ? "ok" : "FAILED", what);
    fails += !ok;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: gwin-host PAK OUTDIR\n"); return 2; }
    uint32_t len;
    uint8_t *pak = readall(argv[1], &len);
    const char *pp = getenv("GWIN_PPM");
    if (pp) {
        uint32_t n;
        uint8_t *d = readall(pp, &n);
        const char *want = "P6\n720 916\n255\n";
        if (n == strlen(want) + (uint32_t)SH_LINUX_W * 916 * 3 && !memcmp(d, want, strlen(want))) rgb = d + strlen(want);
        else fprintf(stderr, "gwin-host: %s is not a 720x916 P6 PPM: a dark surface\n", pp);
    }
    if (!tk_pack_open(pak, len)) { fprintf(stderr, "bad pack\n"); return 1; }
    if (!tk_comp_init(&C, 0, 0)) return 1;
    sh_init(&C);
    sh_set("unlock", "", 1);
    /* 1. gwin.c's own check: the Terminal */
    int r = 0, shot = 0;
    for (int i = 0; i < 1500 && !r; i++) {
        now_ms += 100;
        guest_tick();
        if (gwin_now()) { tk_comp_frame(&C, 0, 0); draws++; }
        r = gwin_selftest_step();
        if (!shot && stats.touches_sent >= 2) {             /* the window after the typed command, as shown */
            tk_comp_frame(&C, 0, 1);
            write_ppm(argv[2], "gwin-typed.ppm", &C.panel);
            shot = 1;
        }
    }
    printf("gwin-host: the self test %s after %llu ms (fake clock); frames drawn for the window %d; the guest's state %d, M3 %d\n",
           r > 0 ? "PASSED" : r < 0 ? "FAILED" : "NOT DONE", (unsigned long long)(now_ms - 1000), draws, st, hw_m3_on());
    if (r <= 0 || getenv("GWIN_L2") || getenv("GWIN_STORE")) return r > 0 ? 0 : 1;

    /* 2. L3 through the shell (Linux off now: the catalog stays for the home) */
    sh_set("page", "", 2);
    run(2);
    tk_comp_frame(&C, 0, 1);
    write_ppm(argv[2], "gwin-home3.ppm", &C.panel);
    memset(&got, 0, sizeof got);
    sh_set("lapp", "org.gnome.TextEditor", 1);              /* as a tap on its icon (page 3) */
    check(st == GUEST_STARTING, "a Linux app's open started Linux");
    run(30);
    check(got.launch == 1 && got.activate == 1, "LAUNCH (with the catalog's layout_seq) + ACTIVATE of the Text Editor");
    check(got.viewport >= 1 && got.last_vp == SH_LINUX_H, "the viewport 1344 rows (no keyboard)");
    check(win == 1 && has_win && draws > 0, "its window in the surface and drawn");
    sh_touch(0, 195, 300, (float)now_ms); sh_touch(2, 195, 300, (float)now_ms);   /* a tap in its text area */
    run(3);
    check(got.last_vp == SH_LINUX_HKB, "the field focused: the keyboard up, the viewport 916 rows");
    sh_touch(0, 195, 735, (float)now_ms); sh_touch(2, 195, 735, (float)now_ms);   /* the keyboard's space */
    sh_touch(0, 156, 627, (float)now_ms); sh_touch(2, 156, 627, (float)now_ms);   /* 'f' */
    sh_touch(0, 343, 736, (float)now_ms); sh_touch(2, 343, 736, (float)now_ms);   /* return */
    run(3);
    check(got.text == 2 && !strcmp(got.text_s, " f"), "typed characters as SBG_MSG_TEXT (\" f\")");
    check(got.keys == 1, "Return as a key");
    tk_comp_frame(&C, 0, 1);
    write_ppm(argv[2], "gwin-editor.ppm", &C.panel);
    sh_set("app", "", 0);                                   /* home */
    run(2);
    check(got.activate_none == 1 && !shown, "home: ACTIVATE none, the window hidden (the app keeps running)");
    sh_set("store", "gimp", 1);                             /* the App Store on GIMP's page (Linux starts again) */
    run(25);
    check(busy, "the App Store open: Linux kept from its idle stop");
    sh_touch(0, 178, 176, (float)now_ms); sh_touch(2, 178, 176, (float)now_ms);   /* GET */
    run(4);
    check(got.pkg == 1 && got.pkg_seq_ok == 1, "GET: SBG_MSG_PKG install with the store's seq");
    tk_comp_frame(&C, 0, 1);
    write_ppm(argv[2], "gwin-store.ppm", &C.panel);
    run(15);
    check(fstore.pkg[2].flags & SBSHM_PKG_INSTALLED, "the install ran to the end");
    sh_set("store", "foot", 1);
    run(2);
    sh_touch(0, 195, 640, (float)now_ms); sh_touch(2, 195, 640, (float)now_ms);   /* where Remove would be */
    run(2);
    check(got.pkg == 1, "a system package offers no Remove");
    sh_set("app", "", 0);
    run(2);
    check(!busy, "the App Store closed: the idle stop allowed again");
    printf("gwin-host: %s (the self test and %d L3 checks)\n", fails ? "FAILED" : "PASSED", 12);
    return fails ? 1 : 0;
}
