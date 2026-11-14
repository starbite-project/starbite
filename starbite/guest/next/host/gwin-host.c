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
 * 3. contract v3 (Track E, "apps work fully"; the toolkit's coming API through run.sh's sw-ext-compat.patch): a third app
 *    (Calculator); two Linux cards and a Starbite one in the switcher; a card's tap re-opens the right app (ACTIVATE
 *    its index); a card thrown away sends APP_CLOSE with its index; the key bar (Ctrl + c: KEYMOD 1, the key, KEYMOD 0;
 *    the arrows as keys); a Linux notification as a banner and in the Centre, its tap (NOTIF_ACT 0, the app opened), a
 *    withdrawal, a dismissal (NOTIF_ACT none); the clipboard both ways; the zero-copy window (Track A: one external
 *    layer, sbg_surf_reading / sbg_surf_done); Linux kept up while cards exist (guest_apps). Stills as PNG in
 *    $GWIN_STILLS (run.sh: build/trackE-stills).
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
static uint32_t frot, frot_rows;            /* cp7: the fake guest turns at once (a new guest: 0, as sbghost's load) */
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
static uint32_t icons[5][SBSHM_ICON_PX * SBSHM_ICON_PX];   /* the catalog's 0..2, the store's 64, 65 */
static unsigned changes;
static uint32_t win = SBSHM_NONE, act = SBSHM_NONE, vp = 0, focus;
static int has_win;
static uint64_t t_win, t_pkg;
static int published;
static struct { int launch, activate, activate_none, viewport, text, keys, pkg, pkg_seq_ok; uint32_t last_vp; char text_s[64];
                int close, reading, done, acts; uint32_t last_act, close_idx, act_id, act_action, read_idx; } got;
/* v3: the messages the shell's key bar and notifications send, in order ("KEYMOD 1;KEY 46 1;...") */
static char ev[4096];
static int evon, apps_n = -1, zc;
static void evlog(const char *f, unsigned a, int b)
{
    if (!evon) return;
    char t[48];
    snprintf(t, sizeof t, f, a, b);
    strncat(ev, t, sizeof ev - strlen(ev) - 1);
}

int guest_available_mode(int m) { return m == GUEST_WL && !(getenv("GWIN_STORE") && !strcmp(getenv("GWIN_STORE"), "none")); }
int guest_start_mode(int m)
{
    if (st != GUEST_OFF && st != GUEST_FAILED) return -1;
    if (!guest_available_mode(m)) { why = "the Linux apps' files are not in the store"; return -4; }
    mode = m;
    st = GUEST_STARTING;
    t_state = now_ms;
    term_ok = 0;
    frot = frot_rows = 0;
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
static const char *gstage = "";            /* cp7 #1: the start's words (guest.c: the disk check, a slow start) */
const char *guest_stage(void) { return st == GUEST_STARTING ? gstage : ""; }
int guest_stop(void) { if (st == GUEST_RUNNING || st == GUEST_STARTING) { st = GUEST_STOPPING; t_state = now_ms; } return 0; }
int guest_owns_m3(void) { return st != GUEST_OFF && st != GUEST_FAILED; }
int hw_m3_on(void) { return guest_owns_m3(); }
const char *guest_net_line(void) { return ""; }        /* 8l: no network check here (no data, no Wi-Fi) */
void guest_show(int on) { shown = on; drawn = 0; }
void guest_keep(int on) { busy = on; }
void guest_apps(int n) { if (n != apps_n) printf("[%6llu ms] fake guest: guest_apps(%d) (Linux kept while hidden: %s)\n", (unsigned long long)now_ms, n, n > 0 ? "yes" : "no"); apps_n = n; }
int guest_key(unsigned code, int down) { if (!guest_shown()) return 0; evlog("KEY %u %d;", code, down); return 1; }
int sbg_keymod(uint32_t m) { evlog("KEYMOD %u;", m, 0); return 1; }
int sh_linux_zero_copy(void) { return zc; }
int clock_now(struct sbos_time *t)                 /* 21:05, the host stills' clock (sh_status) */
{
    memset(t, 0, sizeof *t);
    t->utc = 1794517500;
    t->h = 21;
    t->mi = 5;
    return 1;
}
int guest_shown(void) { return shown && st == GUEST_RUNNING; }
void guest_full_next(void) { drawn = 0; }
int guest_frame_new(void) { return guest_shown() && seq != drawn; }
int guest_term_ok(void) { return term_ok; }
int guest_type(char c)
{
    if (!guest_shown()) return 0;
    got.keys++;
    evlog("TYPE %c;", (unsigned)c, 0);
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
    return i < 3 ? icons[i] : i >= SBSHM_ICONS_CAT && i < SBSHM_ICONS_CAT + 2 ? icons[3 + i - SBSHM_ICONS_CAT] : 0;
}
uint32_t sbg_window(int *h) { if (h) *h = has_win; return win; }
int sbg_text_focus(uint32_t *purpose) { if (purpose) *purpose = 0; return (int)focus; }
int sbg_launch(uint32_t idx, uint32_t cat_seq)
{
    printf("[%6llu ms] fake guest: LAUNCH %u (layout_seq %u)\n", (unsigned long long)now_ms, idx, cat_seq);
    if (cat_seq != fcat.layout_seq || idx >= fcat.n) return 1;   /* stale: ignored, as the contract says */
    got.launch++;
    fcat.seq++;                              /* sbwl: the app runs (and soon has a window) */
    fcat.app[idx].flags |= SBSHM_APP_RUNNING | SBSHM_APP_WINDOW;
    fcat.seq++;
    changes |= SBG_CH_CATALOG;
    return 1;
}
int sbg_activate(uint32_t idx)
{
    printf("[%6llu ms] fake guest: ACTIVATE %d\n", (unsigned long long)now_ms, (int)idx);
    if (idx == SBSHM_NONE) { got.activate_none++; act = win = SBSHM_NONE; has_win = 0; changes |= SBG_CH_WINDOW; focus = 0; return 1; }
    got.activate++;
    got.last_act = idx;
    act = idx;
    focus = 0;
    t_win = now_ms + 400;
    return 1;
}
int sbg_app_close(uint32_t idx)
{
    printf("[%6llu ms] fake guest: APP_CLOSE %u\n", (unsigned long long)now_ms, idx);
    got.close++;
    got.close_idx = idx;
    if (idx < fcat.n) { fcat.seq++; fcat.app[idx].flags &= ~(uint32_t)(SBSHM_APP_RUNNING | SBSHM_APP_WINDOW); fcat.seq++; changes |= SBG_CH_CATALOG; }
    if (act == idx) { act = win = SBSHM_NONE; has_win = 0; changes |= SBG_CH_WINDOW; }
    return 1;
}
/* ---- v3: the notifications' queue (sbghost copies each event), the clipboard's two slots, the zero-copy surface ---- */
static struct { int kind; struct sbshm_notif n; } nq[16];
static int nq_n;
static void fake_notify(uint32_t id, uint32_t app, const char *name, const char *summary, const char *body)
{
    struct sbshm_notif *n = &nq[nq_n].n;
    memset(n, 0, sizeof *n);
    n->id = id; n->app = app; n->urgency = SBSHM_NOTIF_NORMAL; n->icon = SBSHM_NONE;
    snprintf(n->app_name, sizeof n->app_name, "%s", name);
    snprintf(n->summary, sizeof n->summary, "%s", summary);
    snprintf(n->body, sizeof n->body, "%s", body);
    nq[nq_n++].kind = 1;
    changes |= SBG_CH_NOTIF;
    printf("[%6llu ms] fake guest: NOTIFY id %u (app %u) \"%s\"\n", (unsigned long long)now_ms, id, app, summary);
}
static void fake_notif_close(uint32_t id)
{
    memset(&nq[nq_n].n, 0, sizeof nq[nq_n].n);
    nq[nq_n].n.id = id;
    nq[nq_n++].kind = 2;
    changes |= SBG_CH_NOTIF;
    printf("[%6llu ms] fake guest: NOTIF_CLOSE id %u\n", (unsigned long long)now_ms, id);
}
int sbg_notif_next(struct sbshm_notif *n)
{
    if (!nq_n) return 0;
    int k = nq[0].kind;
    *n = nq[0].n;
    memmove(nq, nq + 1, sizeof nq[0] * (size_t)--nq_n);
    return k;
}
int sbg_notif_act(uint32_t id, uint32_t action)
{
    printf("[%6llu ms] fake guest: NOTIF_ACT id %u action %d\n", (unsigned long long)now_ms, id, (int)action);
    got.acts++; got.act_id = id; got.act_action = action;
    return 1;
}
static char g2h[256], h2g[256];
static void fake_copy(const char *t) { snprintf(g2h, sizeof g2h, "%s", t); changes |= SBG_CH_CLIP; printf("[%6llu ms] fake guest: CLIP_SET \"%s\"\n", (unsigned long long)now_ms, t); }
unsigned sbg_clip_get(char mime[40], uint8_t *buf, unsigned cap)
{
    snprintf(mime, 40, "text/plain;charset=utf-8");
    unsigned n = (unsigned)strlen(g2h);
    if (n > cap) return 0;
    memcpy(buf, g2h, n);
    return n;
}
int sbg_clip_set(const char *mime, const uint8_t *data, unsigned len)
{
    unsigned n = len < sizeof h2g - 1 ? len : (unsigned)sizeof h2g - 1;
    memcpy(h2g, data, n);
    h2g[n] = 0;
    printf("[%6llu ms] fake guest: CLIP (%s) \"%s\"\n", (unsigned long long)now_ms, mime, h2g);
    return 1;
}
void sbg_surf_reading(int idx) { got.reading++; got.read_idx = (uint32_t)idx; }
void sbg_surf_done(void) { got.done++; }
int sbg_rotate(uint32_t q, uint32_t rows)
{
    printf("[%6llu ms] fake guest: ROTATE %u, %u rows\n", (unsigned long long)now_ms, q, rows);
    if (st != GUEST_RUNNING) return 0;
    frot = q, frot_rows = rows;
    return 1;
}
uint32_t sbg_rot_shown(void) { return st == GUEST_RUNNING ? frot : 0; }
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
    fcat.n = 3;
    fcat.mem_total_kb = 1490000;
    put_s(fcat.app[0].id, "org.codeberg.dnkl.foot", 40); put_s(fcat.app[0].name, "Foot", 32);
    fcat.app[0].flags = SBSHM_APP_KEYBOARD | SBSHM_APP_SYSTEM; fcat.app[0].icon = 0; fcat.app[0].pkg = 0;
    put_s(fcat.app[1].id, "org.gnome.TextEditor", 40); put_s(fcat.app[1].name, "Text Editor", 32);
    fcat.app[1].flags = 0; fcat.app[1].icon = 1; fcat.app[1].pkg = 1;
    put_s(fcat.app[2].id, "org.gnome.Calculator", 40); put_s(fcat.app[2].name, "Calculator", 32);
    fcat.app[2].flags = 0; fcat.app[2].icon = 2; fcat.app[2].pkg = SBSHM_NONE;
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
    static const uint32_t ic_col[5] = { 0xff2a2f3au, 0xff3584e4u, 0xffe66100u, 0xff2a2f3au, 0xff3584e4u };
    for (int k = 0; k < 5; k++)
        for (int i = 0; i < (int)(SBSHM_ICON_PX * SBSHM_ICON_PX); i++) {
            int x = i % (int)SBSHM_ICON_PX - 80, y = i / (int)SBSHM_ICON_PX - 80;
            icons[k][i] = (k == 2 ? x > -60 && x < 60 && y > -70 && y < 70 : x * x + y * y < 60 * 60) ? ic_col[k] : 0;
        }
    changes |= SBG_CH_CATALOG | SBG_CH_STORE;
    published = 1;
    sb_log("fake guest: the catalog (3 apps) and the store (3 packages) published");
}
/* the surface at 2x, opaque: foot's frame and the typed characters as 16 x 28 blocks from row 260 (45 a row); the
 * Text Editor: white, its typed text as blocks */
static uint32_t surf_px(int x, int y)
{
    uint32_t c = win == 1 ? 0xffffffffu : 0xff242424u;
    if (win == 2) {                 /* the Calculator: a display and a 4 x 5 grid of keys */
        int kx = x % 180, ky = (y - 420) % 180;
        c = y < 400 ? 0xff1c1c1cu : kx > 12 && kx < 168 && ky > 12 && ky < 168 ? (x >= 540 ? 0xffe66100u : 0xff3a3a3au) : 0xff1c1c1cu;
        if (y > 250 && y < 360 && x > 560 && x < 680) c = 0xffffffffu;   /* "0" */
        if (vp && y >= (int)vp * 2 / 3) c = 0xff000000u;
        return c;
    }
    if (win != 1 && rgb && y < 916) {
        const uint8_t *p = rgb + ((size_t)y * 720 + x) * 3;
        c = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    int cell = y >= 260 ? (y - 260) / 32 * 45 + x / 16 : -1, n = win == 1 ? (int)strlen(got.text_s) : ntyped;
    if (cell >= 0 && cell < n && (win == 1 ? got.text_s[cell] : typed[cell]) != ' ' && x % 16 < 12 && (y - 260) % 32 > 4 && (y - 260) % 32 < 28)
        c = win == 1 ? 0xff202020u : 0xffddddddu;
    if (vp && y >= (int)vp * 2 / 3) c = 0xff000000u;   /* (8s: vp in the 1080 surface's rows) */
    return c;
}
/* 8s (Track A): the fake still draws at 720 x 1344 (surf_px); the platform's surface is 1080 x 2016 at 4/3, so the
 * panel rows are the old picture at 2x and the zero-copy front its 3/2 */
int guest_blit_rows(uint32_t *const *rows, int *py0, int *py1)
{
    *py0 = *py1 = -1;
    if (!guest_shown()) return 0;
    drawn = seq;
    for (int y = 0; y < SH_LINUX_PH; y++)
        for (int x = 0; x < SH_LINUX_PW; x++) rows[y][x] = surf_px(x / 2, y / 2);
    *py0 = 0;
    *py1 = SH_LINUX_PH - 1;
    return SH_LINUX_H;
}
/* v3 + Track A: the zero-copy front buffer (two surfaces at 1x; a new frame goes into the other one) */
static uint32_t fsurf[2][SH_LINUX_W * SH_LINUX_H];
static int ffront;
static uint32_t fseq = ~0u;
const volatile uint32_t *guest_front(int *idx, unsigned *stride_px)
{
    if (!guest_shown()) return 0;
    if (fseq != seq) {
        ffront ^= 1;
        for (int y = 0; y < SH_LINUX_H; y++)
            for (int x = 0; x < SH_LINUX_W; x++) fsurf[ffront][y * SH_LINUX_W + x] = surf_px(x * 2 / 3, y * 2 / 3);
        fseq = seq;
    }
    drawn = seq;
    *idx = ffront;
    *stride_px = SH_LINUX_W;
    return fsurf[ffront];
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
/* the panel as a PNG (stored deflate blocks: no zlib) */
static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n)
{
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *b++) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    be32(h, n); memcpy(h + 4, t, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc(0, h + 4, 4);
    c = crc(c, d, n);
    be32(h, c);
    fwrite(h, 1, 4, f);
}
static void write_png(const char *path, const tk_bmp *b, int k)   /* k: 1 full, 2 half size */
{
    int w = b->w / k, h = b->h / k;
    size_t raw = (size_t)h * (1 + (size_t)w * 3), nb = (raw + 65534) / 65535, zn = 2 + raw + nb * 5 + 4;
    uint8_t *r = malloc(raw), *z = malloc(zn);
    if (!r || !z) return;
    for (int y = 0; y < h; y++) {
        uint8_t *o = r + (size_t)y * (1 + (size_t)w * 3);
        *o++ = 0;
        for (int x = 0; x < w; x++) { uint32_t c = b->px[(size_t)(y * k) * b->stride + x * k]; *o++ = c >> 16 & 255; *o++ = c >> 8 & 255; *o++ = c & 255; }
    }
    size_t o = 0, left = raw, at = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, bb = 0;
    for (size_t i = 0; i < raw; i++) { a = (a + r[i]) % 65521; bb = (bb + a) % 65521; }
    while (left) {
        size_t n = left > 65535 ? 65535 : left;
        z[o++] = left == n; z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, r + at, n); o += n; at += n; left -= n;
    }
    be32(z + o, bb << 16 | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (f) {
        static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        uint8_t ih[13];
        fwrite(sig, 1, 8, f);
        be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
        chunk(f, "IHDR", ih, 13);
        chunk(f, "IDAT", z, (uint32_t)o);
        chunk(f, "IEND", 0, 0);
        fclose(f);
    }
    free(r); free(z);
}
static tk_comp C;
static void png_still(const char *dir, const char *name)
{
    char out[512];
    snprintf(out, sizeof out, "%s/trackE-%s.png", dir, name);
    write_png(out, &C.panel, 2);
    printf("[%6llu ms] gwin-host: still %s\n", (unsigned long long)now_ms, out);
}
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
        if (!sh_moving() && sh_idle_pending()) sh_idle();   /* cp7u: as kern_next's idle timer (KT_UI 24), a piece a step */
    }
}
static int fails, nchecks;
/* the rows shown with the keyboard (and v3's 40 pt key bar) up: shell_linux.inc's LX_KBROWS */
#define CARD_XPT 20.0f                     /* shell_lock.c CARD_X (MARGIN) */
#ifndef GWIN_NO_EXT
#define KBROWS (SH_LINUX_HKB - (int)(40.0f * SH_LINUX_HKB / (540.0f - 44.0f) + .5f))
#else
#define KBROWS SH_LINUX_HKB                         /* (no key bar without SH_EXT_API) */
#endif
/* the shell's test views (sw-ext-compat.patch, or the toolkit's own); a tree without one (run.sh: GWIN_NO_SWC /
 * GWIN_NO_NCC) skips only the comparisons that need it, and says so */
#ifdef GWIN_NO_SWC
static int sw_compat_dump(char *out, int cap) { if (cap > 0) out[0] = 0; return -1; }
#define SWC_OK(e) 1
#else
int sw_compat_dump(char *out, int cap);            /* sw-ext-compat.patch's view of the recents */
#define SWC_OK(e) (e)
#endif
#ifdef GWIN_NO_NCC
#define NCC_OK(e) 1
#else
int nc_compat_count(const char *who);
#define NCC_OK(e) (e)
#endif
int sh_linux_job(char *b, int cap);                /* gwin.c (cp6): the running store job in words */
/* a tap, a horizontal drag (dx pt over n 16 ms steps, then still), a quick throw up */
static void tap(float x, float y) { sh_touch(0, x, y, (float)now_ms); sh_touch(2, x, y, (float)now_ms + 60); }
static void drag(float x, float y, float dx, float dy, int n, int still)
{
    float t = (float)now_ms;
    sh_touch(0, x, y, t);
    for (int i = 1; i <= n; i++) { t += 16; sh_touch(1, x + dx * i / n, y + dy * i / n, t); sh_tick(.016f, t); }
    if (still) { t += 150; sh_touch(1, x + dx, y + dy, t); }
    sh_touch(2, x + dx, y + dy, t + 16);
}
static void png_still(const char *dir, const char *name);
static void check(int ok, const char *what)
{
    printf("[%6llu ms] gwin-host: %s: %s\n", (unsigned long long)now_ms, ok ? "ok" : "FAILED", what);
    fails += !ok;
    nchecks++;
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
    /* 1. gwin.c's own check: the Terminal (GWIN_ZC=1: on the zero-copy path, as 8d's phone with the GPU layer) */
    int r = 0, shot = 0;
    if (getenv("GWIN_ZC")) zc = 1;
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
    zc = 0;
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
    gstage = "Checking Linux's disk.";                      /* cp7 #1: the guest's init checks the overlay */
    run(2);
    tk_comp_frame(&C, 0, 1);
    write_ppm(argv[2], "gwin-starting-check.ppm", &C.panel);
    check(st == GUEST_STARTING, "cp7: the start's words shown while starting (gwin-starting-check.ppm)");
    gstage = "";
    run(30);
    check(got.launch == 1 && got.activate == 1, "LAUNCH (with the catalog's layout_seq) + ACTIVATE of the Text Editor");
    check(got.viewport >= 1 && got.last_vp == SH_LINUX_H, "the viewport 1344 rows (no keyboard)");
    check(win == 1 && has_win && draws > 0, "its window in the surface and drawn");
    sh_touch(0, 195, 300, (float)now_ms); sh_touch(2, 195, 300, (float)now_ms);   /* a tap in its text area */
    run(30);                                                 /* (B3: the keyboard takes keys once its slide rests; run() steps springs 50 ms) */
    check(got.last_vp == KBROWS, "the field focused: the keyboard up, the viewport above the keyboard and its key bar");
    sh_touch(0, 195, 735, (float)now_ms); sh_touch(2, 195, 735, (float)now_ms);   /* the keyboard's space */
    sh_touch(0, 156, 627, (float)now_ms); sh_touch(2, 156, 627, (float)now_ms);   /* 'f' */
    sh_touch(0, 343, 736, (float)now_ms); sh_touch(2, 343, 736, (float)now_ms);   /* return */
    run(3);
    check(got.text == 2 && !strcmp(got.text_s, " f"), "typed characters as SBG_MSG_TEXT (\" f\")");
    check(got.keys == 1, "Return as a key");
    tk_comp_frame(&C, 0, 1);
    write_ppm(argv[2], "gwin-editor.ppm", &C.panel);
    setenv("SBTK_ORI", "1", 1);                             /* cp7: the phone turned, its top to the left (sh_orientation) */
    run(6);
    check(frot == 0, "cp7: the keyboard up: the window stays portrait");
    sh_linux_viewport(SH_LINUX_H);                          /* the keyboard down */
    run(6);
    check(frot == 1 && frot_rows == SH_LINUX_W && sh_linux_rot_shown() == 1, "cp7: sh_orientation TURNED_LEFT -> ROTATE 1, the turned frame on show");
    setenv("SBTK_ORI", "3", 1);
    run(6);
    check(frot == 3, "cp7: TURNED_RIGHT -> ROTATE 3");
    setenv("SBTK_ORI", "2", 1);
    run(6);
    check(frot == 0, "cp7: upside down -> portrait");
    setenv("SBTK_ORI", "1", 1);
    run(6);
    unsetenv("SBTK_ORI");
    sh_set("app", "", 0);                                   /* home */
    run(2);
    check(got.activate_none == 1 && !shown, "home: ACTIVATE none, the window hidden (the app keeps running)");
    check(frot == 0, "cp7: home: the guest portrait again");
    sh_set("store", "gimp", 1);                             /* the App Store on GIMP's page (Linux starts again) */
    run(25);
    check(busy, "the App Store open: Linux kept from its idle stop");
    sh_touch(0, 178, 190, (float)now_ms); sh_touch(2, 178, 190, (float)now_ms);   /* Get (Track F's A1 page: y 190, its films') */
    run(4);
    check(got.pkg == 1 && got.pkg_seq_ok == 1, "GET: SBG_MSG_PKG install with the store's seq");
    {
        char jb[96];
        int jr = sh_linux_job(jb, sizeof jb);
        printf("[%6llu ms] gwin-host: the job in words: \"%s\"\n", (unsigned long long)now_ms, jb);
        check(jr == 1 && jb[0] && strstr(jb, "GIMP"), "cp6: the running job in words for the shell (sh_linux_job)");
    }
    tk_comp_frame(&C, 0, 1);
    write_ppm(argv[2], "gwin-store.ppm", &C.panel);
    run(15);
    check(fstore.pkg[2].flags & SBSHM_PKG_INSTALLED, "the install ran to the end");
    {
        char jb[96];
        check(sh_linux_job(jb, sizeof jb) == 0 && !jb[0] && NCC_OK(nc_compat_count("store") == 1),
              "cp6: no job left, its end in the Centre (store: \"... Is Installed\")");
    }
    sh_set("store", "foot", 1);
    run(2);
    sh_touch(0, 195, 640, (float)now_ms); sh_touch(2, 195, 640, (float)now_ms);   /* where Remove would be */
    run(2);
    check(got.pkg == 1, "a system package offers no Remove");
    sh_set("app", "", 0);
    run(2);
    check(!busy, "the App Store closed: the idle stop allowed again");
    printf("gwin-host: %s (the self test and %d L3 checks)\n", fails ? "FAILED" : "PASSED", nchecks);
    if (fails) return 1;

    /* 3. contract v3 (Track E): cards, the key bar, notifications, the clipboard, the zero-copy window */
    int l3 = nchecks;
    const char *stills = getenv("GWIN_STILLS") ? getenv("GWIN_STILLS") : argv[2];
#ifndef GWIN_NO_EXT
    char dump[256];
    check(apps_n == 1, "the Text Editor's card keeps Linux up while hidden (guest_apps 1)");
    sh_set("lapp", "org.gnome.Calculator", 1);              /* a third app */
    run(25);
    check(win == 2 && got.last_act == 2 && apps_n == 2, "the Calculator opened (ACTIVATE 2), two Linux cards");
    sh_set("app", "", 0);                                   /* home, then a Starbite app */
    run(6);
    sh_set("app", "notes", 1);
    run(12);
    for (int i = 0; i < 4; i++) sh_idle();                  /* its snapshot (idle time) */
    sh_set("switcher", "", 1);
    run(3);
    sw_compat_dump(dump, sizeof dump);
    printf("gwin-host: the recents: %s\n", dump);
    check(SWC_OK(!strcmp(dump, "S:notes,L:Calculator,L:Text Editor")), "the switcher: Notes, then the Calculator's and the Text Editor's own cards");
    sh_tick(.016f, (float)now_ms);
    tk_comp_frame(&C, 0, 1);
    png_still(stills, "switcher");
    /* the Text Editor's card (third): two swipes right, then a tap on it */
    for (int k = 0; k < 2; k++) { drag(120, 420, 214, 0, 8, 1); run(8); }
    sh_tick(.016f, (float)now_ms);
    tk_comp_frame(&C, 0, 1);
    png_still(stills, "switcher-scrolled");
    int acts = got.activate;
    tap(195, 390);
    run(12);
    check(got.activate == acts + 1 && got.last_act == 1 && win == 1, "a tap on the Text Editor's card: ACTIVATE 1, its window back");
    sw_compat_dump(dump, sizeof dump);
    check(SWC_OK(!strcmp(dump, "L:Text Editor,S:notes,L:Calculator")), "the Text Editor's card first again");
    /* throw the Calculator's card (third) away */
    sh_set("switcher", "", 1);
    run(3);
    for (int k = 0; k < 2; k++) { drag(120, 420, 214, 0, 8, 1); run(8); }
    drag(195, 420, 0, -300, 5, 0);
    run(25);
    sw_compat_dump(dump, sizeof dump);
    printf("gwin-host: the recents: %s\n", dump);
    check(got.close == 1 && got.close_idx == 2, "the Calculator's card thrown away: APP_CLOSE 2");
    check(SWC_OK(!strcmp(dump, "L:Text Editor,S:notes")) && apps_n == 1, "its card gone, one Linux card left (guest_apps 1)");
    sh_set("switcher", "", 0);
    run(3);
    /* the key bar: the Text Editor's field focused (the keyboard up), Ctrl latched, c; an arrow; Alt + right */
    sh_set("lapp", "org.gnome.TextEditor", 1);
    run(8);
    if (!focus) { tap(195, 300); run(3); }
    check(got.last_vp == KBROWS, "the keyboard up: the viewport above the key bar");
    run(25);                                                /* (P3x, B3's keyboard: its keys and bar once its slide rests) */
    evon = 1;
    ev[0] = 0;
    /* (8q: nine keys since the bar's Done: 390 / 9 pt each) */
    tap(108.3f, 520);                                       /* Ctrl */
    run(2);
    sh_tick(.016f, (float)now_ms);
    tk_comp_frame(&C, 0, 1);
    png_still(stills, "keybar-ctrl-latched");
    tap(156, 681);                                          /* c */
    run(1);
    tap(195, 520);                                          /* left */
    tap(151.7f, 520);                                       /* Alt */
    tap(325, 520);                                          /* right */
    tap(21.7f, 520);                                        /* Esc */
    run(2);
    evon = 0;
    printf("gwin-host: the key bar sent: %s\n", ev);
    check(!strcmp(ev, "KEYMOD 1;TYPE c;KEYMOD 0;KEY 105 1;KEY 105 0;KEYMOD 2;KEY 106 1;KEY 106 0;KEYMOD 0;KEY 1 1;KEY 1 0;"),
          "the key bar: Ctrl + c (KEYMOD 1, c, KEYMOD 0), left, Alt + right, Esc");
    sh_tick(.016f, (float)now_ms);
    tk_comp_frame(&C, 0, 1);
    png_still(stills, "keybar");
    /* the clipboard: Linux copies, Starbite has it; Starbite copies, Linux is offered it */
    fake_copy("Hello from Linux");
    run(2);
    check(!strcmp(sh_clip_get(), "Hello from Linux"), "a Linux copy in the shell's clipboard (sh_clip_get)");
    sh_clip_set("Hello from Starbite");
    check(!strcmp(h2g, "Hello from Starbite"), "a Starbite copy offered to Linux (sbg_clip_set)");
    /* notifications: home, the Text Editor posts; banner, Centre; a tap opens it with NOTIF_ACT 0 */
    sh_set("app", "", 0);
    run(6);
    fake_notify(0x1a, 1, "Text Editor", "Document saved", "notes.txt was saved to Documents.");
    run(2);
    check(NCC_OK(nc_compat_count("linux:1a") == 1), "the notification in the Centre's store as linux:1a");
    run(4);
    sh_tick(.016f, (float)now_ms);
    tk_comp_frame(&C, 0, 1);
    png_still(stills, "banner");
    fake_notify(0x1b, 2, "Calculator", "Withdrawn", "This one is taken back.");
    run(2);
    fake_notif_close(0x1b);
    run(2);
    check(NCC_OK(nc_compat_count("linux:1b") == 0), "a withdrawn notification left the Centre");
    fake_notify(0x1c, 0, "", "Build finished", "make: all targets done.");
    run(60);                                                /* (its banner gone by itself) */
    sh_set("nc", "", 1);
    run(2);
    sh_tick(.016f, (float)now_ms);
    tk_comp_frame(&C, 0, 1);
    png_still(stills, "nc-list");
    sh_set("ncswipe", "", 92);                              /* the newest's Clear button */
    run(1);
    tap(CARD_XPT + 350 - 40, 276 + 34);         /* (NC_W = CONTENT_W 350) */
    run(4);
    check(got.acts == 1 && got.act_id == 0x1c && got.act_action == SBSHM_NONE, "a notification cleared: NOTIF_ACT id 0x1c none");
    sh_set("nc", "", 0);
    run(3);
    sh_set("banner", "", 1);                                /* the Text Editor's again (the newest left) */
    run(1);
    int a0 = got.activate;
    tap(195, 40);
    run(12);
    check(got.acts == 2 && got.act_id == 0x1a && got.act_action == 0, "the banner's tap: NOTIF_ACT id 0x1a action 0");
    check(got.activate > a0 && got.last_act == 1 && shown, "the banner's tap opened the Text Editor (ACTIVATE 1)");
#else
    printf("gwin-host: cards / key bar / notifications / clipboard: SKIPPED (no SH_EXT_API in this UI tree and the stand-in does not apply)\n");
#endif
    /* Track A's zero-copy window: one external layer on the guest's front buffer */
#ifndef GWIN_NO_ZC
    sh_set("app", "", 0);
    run(4);
    zc = 1;
    int rd = got.reading, dn = got.done;
    sh_set("lapp", "org.gnome.TextEditor", 1);
    run(10);
    tk_comp_frame(&C, 0, 1);
    uint32_t p = C.panel.px[(size_t)(162 + 2 * 120) * C.panel.stride + 2 * 600] | 0xff000000u, want = surf_px(600, 120);
    printf("gwin-host: zero-copy: surf_reading %d (buffer %u), the panel px %08x, the surface's %08x\n", got.reading - rd, got.read_idx, p, want);
    check(got.reading > rd && p == want, "zero-copy: the window drawn from the front buffer (sbg_surf_reading)");
    sh_linux_frame();                                       /* (the newest front first) */
    uint32_t *fp = &fsurf[ffront][180 * SH_LINUX_W + 900], keep[9];   /* (8s: panel 1200, 402 at 4/3) */
    for (int k = 0; k < 9; k++) { keep[k] = fp[(k / 3 - 1) * SH_LINUX_W + k % 3 - 1]; fp[(k / 3 - 1) * SH_LINUX_W + k % 3 - 1] = 0xffff00ffu; }
    /* (3 x 3 px of the guest's front buffer changed in place: the 2x scale filters) */
    sh_linux_frame();                                       /* (the same buffer handed again: no copy anywhere) */
    tk_comp_frame(&C, 0, 1);
    p = C.panel.px[(size_t)(162 + 2 * 120) * C.panel.stride + 2 * 600] | 0xff000000u;
    for (int k = 0; k < 9; k++) fp[(k / 3 - 1) * SH_LINUX_W + k % 3 - 1] = keep[k];
    printf("gwin-host: zero-copy: hand-offs %d, the panel px after the in-place change %08x\n", got.reading - rd, p);
    check(p == 0xffff00ffu, "zero-copy: the panel samples the guest's buffer in place (no copy)");
    png_still(stills, "zerocopy");
    sh_set("app", "", 0);
    run(3);
    check(got.done > dn, "zero-copy: home detaches it (sbg_surf_done)");
    zc = 0;
#else
    printf("gwin-host: zero-copy: SKIPPED (this UI tree has no tk_layer_external and the compat patch does not apply)\n");
#endif
    /* Linux stops: every Linux card goes, the 60 s rule again */
    guest_stop();
    run(12);
    check(apps_n <= 0, "Linux stopped: its cards gone (guest_apps 0, or never called without SH_EXT_API)");
    printf("gwin-host: v3 %s (%d checks)\n", fails ? "FAILED" : "PASSED", nchecks - l3);
#ifdef GWIN_NO_SWC
    printf("gwin-host: (this UI tree has no sw_compat_dump: the switcher's card order was not compared)\n");
#endif
#ifdef GWIN_NO_NCC
    printf("gwin-host: (this UI tree has no nc_compat_count: the Centre's item counts were not compared)\n");
#endif
    printf("gwin-host: the UI heap: %.1f MB in use, peak %.1f MB%s\n", tk_heap_used() / 1e6, tk_heap_peak() / 1e6,
           getenv("SBTK_HEAP_MB") ? " (SBTK_HEAP_MB: the phone's heap)" : "");
    printf("gwin-host: %s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
