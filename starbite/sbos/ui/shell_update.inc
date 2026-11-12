/* SPDX-License-Identifier: MIT
 * Track A: Software Update (2026-11-12), included by shell_lock.c after shell_time.inc. A minimal page on the
 * platform's API (shell.h: sh_update_check / _stage / _install; the kernel: kern/update.c through kern_next.c); the UI
 * track restyles it and moves it under General when Settings is restructured. Until then Settings > About > Software
 * opens it (AS.sub SU_SUB):
 *   - "Starbite OS is up to date" (the running version, the reason when there is one: no card, no loader table);
 *   - an update on the card: its version, size and notes, "Download and Install";
 *   - staging: a progress bar, a chunk per refresh turn (sh_next_update asks for one every few ms while it runs);
 *   - "Restart to Install" once it is staged and checked; failures in words with "Try Again";
 *   - "Updated to <version>" on the start that committed it.
 * The stills' states: update=open (the page, checked), update=stage:N (N chunks), update=ready, update=install. */

/* SU_SUB (33, shell_lock.c): AS.sub of this page. P3x (Track B): on B2's kit, as iOS 26's Software Update: the
 * update's card (the Starbite tile, the version, "Starbite · size", the notes or the progress) on a #1C1C1E group,
 * the action as a full-width capsule under it (filled accent for Download and Install / Restart to Install, the card
 * grey with accent words for Try Again / Check Again), the footer under that; up to date: the tile, the version and
 * "Starbite OS is up to date" centred */
#define SU_CARD_Y (PUSHED_TOP + 8)          /* the version's card */
#define SU_CARD_H 160.0f
#define SU_BTN_Y (SU_CARD_Y + SU_CARD_H + 16)   /* the action capsule */
#define SU_BTN_H 50.0f
#define SU_MID_Y (SU_CARD_Y + 96)           /* up to date: the tile's top */
static struct { struct sh_update u; int checked, running, checking; float check_t; } SWU;   /* cp6: checking (Check Again's) */
#define SU_CHECK_MS 600.0f                   /* cp6: "Checking\xe2\x80\xa6" shows at least this long */

static void su_check(void)
{
    (void)sh_update_check(&SWU.u);
    SWU.checked = 1;
    SWU.running = SWU.u.state == 2;
}
static void su_size(char *b, unsigned n)    /* "18.4 MB" */
{
    unsigned t = (unsigned)((n + 50000u) / 100000u);  /* tenths of MB */
    int k = put_dec(b, (int)(t / 10));
    b[k++] = '.';
    b[k++] = (char)('0' + t % 10);
    b[k++] = ' '; b[k++] = 'M'; b[k++] = 'B'; b[k] = 0;
}
static const char *su_action(void)
{
    if (SWU.checking) return "Checking\xe2\x80\xa6";
    switch (SWU.u.state) {
    case 1: return "Download and Install";
    case 3: return "Restart to Install";
    case -1: return "Try Again";
    case 0: return "Check Again";
    default: return 0;
    }
}
static void su_lines(dc *d, float y, const char *s, int max, tk_col c)   /* wrapped text in the card, 15 pt */
{
    char ln[4][96];
    float wm;
    int n = wrapn(ln, max < 4 ? max : 4, TK_IT400, 15, s, LG_W - 32, &wm);
    for (int k = 0; k < n; k++) d_text(d, TK_IT400, 15, LG_TX, bl(TK_IT400, 15, y + 20 * k), ln[k], 0, 0, c, 0);
}
static int su_primary(void) { return !SWU.checking && (SWU.u.state == 1 || SWU.u.state == 3); }
static float su_btn_y(void)
{
    if (SWU.u.state == 0 || SWU.u.state == 4 || (SWU.u.state == -1 && !SWU.u.version[0])) return SU_MID_Y + 52 + 112;
    return SU_BTN_Y;
}
static void swu_cat(char *t, const char *a, const char *b)
{
    int k = 0;
    while (*a && k < 80) t[k++] = *a++;
    while (b && *b && k < 94) t[k++] = *b++;
    t[k] = 0;
}
static void su_page(dc *d)
{
    if (!SWU.checked) su_check();
    const struct sh_update *u = &SWU.u;
    d_back(d, W(1));
    d_navtitle(d, "Software Update");
    char t[96], sz[16];
    if (u->state == 0 || u->state == 4 || (u->state == -1 && !u->version[0])) {
        /* up to date (or nothing to check): the tile, the running version, the line under it, centred */
        float y = SU_MID_Y;
        if (u->state == 4) {                          /* installed: a green check in place of the tile */
            d_rrect(d, 195 - 26, y, 52, 52, 26, C_GREEN);
            d_seg(d, 195 - 11, y + 27, 195 - 3, y + 35, 3.4f, W(1));
            d_seg(d, 195 - 3, y + 35, 195 + 12, y + 18, 3.4f, W(1));
        } else
            d_systile(d, "starbite", 195 - 26, y);
        swu_cat(t, "Starbite OS ", u->state == 4 ? u->version : 0);
        d_text_box(d, TK_IT600, 17, 195, bl(TK_IT600, 17, y + 68), CONTENT_W, 1, u->state == 4 ? t : SYS.ver, 0, 0, INK, 0);
        const char *l = u->state == 4 ? "Updated" : u->state == 0 ? "Starbite OS is up to date" : "Can't check for updates";
        d_text_box(d, TK_IT400, 15, 195, bl(TK_IT400, 15, y + 94), CONTENT_W, 1, l, 0, 0, KIT_SEC(), 0);
        if (u->why[0] && u->state != 4) {
            char ln[4][96];                 /* wrapn takes char[4][96] (gcc: stringop-overflow) */
            float wm;
            int n = wrapn(ln, 2, TK_IT400, 13, u->why, CONTENT_W - 20, &wm);
            for (int k = 0; k < n; k++)
                d_text_box(d, TK_IT400, 13, 195, bl(TK_IT400, 13, y + 120 + 18.0f * k), CONTENT_W, 1, ln[k], 0, 0, u->state == -1 ? C_RED : C_LABEL3, 0);
        }
    } else {
        /* the update's card: the tile, "Starbite OS <version>", "Starbite · size"; the notes, the progress, ready, failed */
        d_group(d, SU_CARD_Y, SU_CARD_H);
        d_systile(d, "starbite", LG_TX, SU_CARD_Y + 16);
        swu_cat(t, "Starbite OS ", u->version);
        d_text_box(d, TK_IT600, 17, LG_TX + 64, bl(TK_IT600, 17, SU_CARD_Y + 22), LG_W - 96, 0, t, 0, 0, INK, 0);
        su_size(sz, u->size);
        swu_cat(t, "Starbite \xc2\xb7 ", sz);
        d_text_box(d, TK_IT400, 15, LG_TX + 64, bl(TK_IT400, 15, SU_CARD_Y + 45), LG_W - 96, 0, t, 0, TK_TNUM, KIT_SEC(), 0);
        float ty = SU_CARD_Y + 84;
        if (u->state == 2) {
            float bx = LG_TX, bw = LG_W - 32, by = ty + 4;
            int p = u->pct < 0 ? 0 : u->pct > 100 ? 100 : u->pct;
            d_rrect(d, bx, by, bw, 6, 3, W(.16f));
            if (p) d_rrect(d, bx, by, tk_maxf(6, bw * (float)p / 100), 6, 3, ACCENT());
            int k = 0;
            for (const char *s2 = p < 90 ? "Copying from the card\xe2\x80\xa6 " : "Verifying\xe2\x80\xa6 "; *s2;) t[k++] = *s2++;
            k += put_dec(t + k, p);
            t[k++] = ' '; t[k++] = '%'; t[k] = 0;
            d_text(d, TK_IT400, 13, bx, bl(TK_IT400, 13, by + 18), t, 0, TK_TNUM, KIT_SEC(), 0);
        } else if (u->state == 3) {
            su_lines(d, ty, "Ready to install. The phone restarts into the new version; if it doesn't start, the previous version comes back.", 3, INK);
        } else if (u->state == -1) {
            d_text(d, TK_IT600, 15, LG_TX, bl(TK_IT600, 15, ty), "Update Failed", 0, 0, C_RED, 0);
            su_lines(d, ty + 24, u->why, 2, KIT_SEC());
        } else
            su_lines(d, ty, u->notes[0] ? u->notes : "No release notes.", 3, INK);
    }
    const char *a = su_action();
    if (a) {
        float y = su_btn_y();
        int pr = su_primary();
        d_rrect(d, LG_X, y, LG_W, SU_BTN_H, SU_BTN_H / 2, pr ? ACCENT() : C_CARD);
        d_text_box(d, TK_IT600, 17, 195, bl(TK_IT600, 17, y + (SU_BTN_H - lh(TK_IT600, 17)) / 2), LG_W - 32, 1, a, 0, 0,
                   SWU.checking ? KIT_SEC() : pr ? W(1) : ACCENT(), 0);   /* cp6: grey while it checks (the spinner beside) */
    }
    if (u->state == 1 || u->state == 3)
        d_group_foot(d, SU_BTN_Y + SU_BTN_H, "From the memory card's STARBITE/UPDATE folder. Starbite OS keeps the previous version until the new one has started.");
}
/* the About section's "Software" row on the Settings list (until Track B's General > Software Update) */
static float su_row_main(void)
{
    return -1;                                      /* P3x: B2's Settings > General > Software Update row opens it (A_UPDATE) */
}
static void su_open(void)
{
    AS.sub = SU_SUB;
    SWU.checking = 0;
    su_check();
    build_app_page(AP.a);
}
/* app_tap: 1 taken (Settings' row, the page) */
static int su_app_tap(float x, float y)
{
    if (!is_app("settings")) return 0;
    if (!AS.sub) {
        float ry = su_row_main();
        if (ry < 0 || x < GROUP_X || x >= CONTENT_R || y < ry || y >= ry + 47) return 0;
        su_open();
        return 1;
    }
    if (AS.sub != SU_SUB) return 0;
    if (BACK_HIT(x, y)) { settings_back(); return 1; }   /* B2's Settings, where it was */
    float by = su_btn_y();
    if (!su_action() || x < LG_X || x >= LG_RE || y < by || y >= by + SU_BTN_H) return 1;
    if (SWU.u.state == 1) {                          /* Download and Install: the staging runs on refresh turns */
        SWU.running = 1;
        (void)sh_update_stage(&SWU.u);
    } else if (SWU.u.state == 3) {
        if (sh_update_install() < 0) toast("The update can't be installed now");
    } else if (!SWU.checking) {
        SWU.checking = 1;                           /* cp6: Check Again / Try Again: "Checking\xe2\x80\xa6" at once, the check */
        SWU.check_t = now_ms;                       /* on a refresh turn (su_refresh), a spinner meanwhile */
    } else
        toast("Already checking for updates\xe2\x80\xa6");
    build_app_page(AP.a);
    return 1;
}
static int su_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    (void)c;
    if (!is_app("settings") || x < LG_X || x >= LG_RE) return 0;
    float top = -1;
    if (!AS.sub) {
        float ry = su_row_main();
        if (ry >= 0 && y >= ry && y < ry + 47) top = ry;
    } else if (AS.sub == SU_SUB && su_action() && !SWU.checking) {
        float by = su_btn_y();
        if (y >= by && y < by + SU_BTN_H) top = by;
    }
    if (top < 0) return 0;
    *r = *g = (tk_rect){ LG_X, top, LG_W, SU_BTN_H };
    *rad = SU_BTN_H / 2;
    *c = su_primary() ? SEP_FILL : SEP_ROW;           /* cp6: the filled capsule darkens, the grey one lights */
    return 1;
}
/* cp6: the spinner beside "Checking\xe2\x80\xa6" (shell_settings.inc's strip spinner) */
static int su_spin_at(float *cx, float *cy)
{
    if (!SWU.checking || AS.sub != SU_SUB) return 0;
    *cx = 195 - tw(TK_IT600, 17, su_action(), 0, 0) / 2 - 18;
    *cy = su_btn_y() + SU_BTN_H / 2;
    return 1;
}
/* the staging's turns: a chunk each, the page redrawn (sh_refresh / sh_next_update) */
static float su_next_update(float t)
{
    if (SWU.checking) return tk_maxf(15, SU_CHECK_MS - (t - SWU.check_t));
    return SWU.running ? 4 : -1;
}
static int su_refresh(float t)
{
    if (SWU.checking && t - SWU.check_t >= SU_CHECK_MS && !sh_moving()) {
        SWU.checking = 0;
        su_check();
        if (AP.on && is_app("settings") && AS.sub == SU_SUB) { build_app_page(AP.a); return 1; }
        return 0;
    }
    if (!SWU.running || sh_moving()) return 0;
    int st = sh_update_stage(&SWU.u);
    if (st != 2) SWU.running = 0;
    if (AP.on && is_app("settings") && AS.sub == SU_SUB) { build_app_page(AP.a); return 1; }
    return 0;
}
/* the stills' states */
static int su_set(const char *what, const char *key, float v)
{
    if (tk_strcmp(what, "update")) return 0;
    if (!tk_strcmp(key, "open")) su_open();
    else if (!tk_strcmp(key, "stage")) {
        if (!SWU.checked) su_check();
        for (int i = 0; i < (int)v; i++) (void)sh_update_stage(&SWU.u);
        SWU.running = SWU.u.state == 2;
        AS.sub = SU_SUB;
    } else if (!tk_strcmp(key, "ready")) {
        if (!SWU.checked) su_check();
        for (int i = 0; i < 1000 && (SWU.u.state == 1 || SWU.u.state == 2); i++) (void)sh_update_stage(&SWU.u);
        SWU.running = 0;
        AS.sub = SU_SUB;
    } else if (!tk_strcmp(key, "install"))
        (void)sh_update_install();
    if (AP.on) build_app_page(AP.a);
    return 1;
}
