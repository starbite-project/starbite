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

/* SU_SUB (10, shell_lock.c): AS.sub of this page (5 passcode, 7 / 8 date and time, 9 startup) */
#define SU_CARD_Y (PUSHED_TOP + 8)          /* the version's card */
#define SU_CARD_H 150.0f
#define SU_BTN_Y (SU_CARD_Y + SU_CARD_H + 20)   /* the action row */

static struct { struct sh_update u; int checked, running; } SU;

static void su_check(void)
{
    (void)sh_update_check(&SU.u);
    SU.checked = 1;
    SU.running = SU.u.state == 2;
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
    switch (SU.u.state) {
    case 1: return "Download and Install";
    case 3: return "Restart to Install";
    case -1: return "Try Again";
    case 0: return "Check Again";
    default: return 0;
    }
}
static void su_lines(dc *d, float y, const char *s, int max, tk_col c)   /* wrapped grey text under the card */
{
    char ln[4][96];
    float wm;
    int n = wrapn(ln, max < 4 ? max : 4, TK_IT400, 13, s, CONTENT_R - 16 - ROW_TX, &wm);
    for (int k = 0; k < n; k++) d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y + 17 * k), ln[k], 0, 0, c, 0);
}
static float su_btn_y(void) { return SU.u.state == 0 || (SU.u.state == -1 && !SU.u.version[0]) ? SU_CARD_Y + 200 : SU_BTN_Y; }
static void su_page(dc *d)
{
    if (!SU.checked) su_check();
    const struct sh_update *u = &SU.u;
    d_back(d, W(1));
    d_navtitle(d, "Software Update");
    char t[96], sz[16];
    if (u->state == -1 && !u->version[0]) {
        /* nothing to show of an update (no loader table, a broken manifest): why, centred */
        float cy = SU_CARD_Y + 40;
        d_text_box(d, TK_IT600, 20, 195, bl(TK_IT600, 20, cy), CONTENT_W, 1, "Can't Check for Updates", 0, 0, INK, 0);
        d_text_box(d, TK_IT400, 15, 195, bl(TK_IT400, 15, cy + 32), CONTENT_W, 1, SYS.ver, 0, 0, C_LABEL2, 0);
        d_text_box(d, TK_IT400, 13, 195, bl(TK_IT400, 13, cy + 56), CONTENT_W, 1, u->why, 0, 0, tk_hex(0xff453a, 1), 0);
    } else if (u->state == 0 || u->state == 4) {
        /* up to date: the running version centred, the detail under it */
        float cy = SU_CARD_Y + 40;
        if (u->state == 4) {
            int k = 0;
            for (const char *p = "Updated to "; *p;) t[k++] = *p++;
            for (const char *p = u->version; *p && k < 80;) t[k++] = *p++;
            t[k] = 0;
            d_rrect(d, 195 - 22, cy, 44, 44, 22, tk_hex(0x34c759, 1));
            d_seg(d, 195 - 9, cy + 23, 195 - 3, cy + 29, 3, W(1));
            d_seg(d, 195 - 3, cy + 29, 195 + 10, cy + 15, 3, W(1));
            cy += 60;
        }
        d_text_box(d, TK_IT600, 20, 195, bl(TK_IT600, 20, cy), CONTENT_W, 1, u->state == 4 ? t : "Starbite OS is up to date", 0, 0, INK, 0);
        d_text_box(d, TK_IT400, 15, 195, bl(TK_IT400, 15, cy + 32), CONTENT_W, 1, SYS.ver, 0, 0, C_LABEL2, 0);
        if (u->why[0] && u->state == 0) d_text_box(d, TK_IT400, 13, 195, bl(TK_IT400, 13, cy + 56), CONTENT_W, 1, u->why, 0, 0, W(.45f), 0);
    } else {
        /* the update's card: the version, its size, the notes; the progress while staging */
        d_rrect(d, GROUP_X, SU_CARD_Y, GROUP_W, SU_CARD_H, 16, tk_hex(0x151c33, 1));
        d_rrect(d, ROW_TX, SU_CARD_Y + 16, 44, 44, 11, tk_hex(0x0a84ff, 1));
        d_text_box(d, TK_IT700, 20, ROW_TX + 22, bl(TK_IT700, 20, SU_CARD_Y + 26), 40, 1, "S", 0, 0, W(1), 0);
        int k = 0;
        for (const char *p = "Starbite OS "; *p;) t[k++] = *p++;
        for (const char *p = u->version; *p && k < 80;) t[k++] = *p++;
        t[k] = 0;
        d_text_box(d, TK_IT600, 17, ROW_TX + 58, bl(TK_IT600, 17, SU_CARD_Y + 18), CONTENT_R - ROW_TX - 74, 0, t, 0, 0, INK, 0);
        su_size(sz, u->size);
        d_text_box(d, TK_IT400, 13, ROW_TX + 58, bl(TK_IT400, 13, SU_CARD_Y + 42), 200, 0, sz, 0, TK_TNUM, C_LABEL2, 0);
        if (u->state == 2) {
            float bx = ROW_TX, bw = CONTENT_R - 16 - ROW_TX, by = SU_CARD_Y + 82;
            int p = u->pct < 0 ? 0 : u->pct > 100 ? 100 : u->pct;
            d_rrect(d, bx, by, bw, 6, 3, W(.14f));
            if (p) d_rrect(d, bx, by, bw * (float)p / 100, 6, 3, ACCENT());
            k = 0;
            for (const char *s = p < 90 ? "Copying from the card\xe2\x80\xa6 " : "Checking\xe2\x80\xa6 "; *s;) t[k++] = *s++;
            k += put_dec(t + k, p);
            t[k++] = ' '; t[k++] = '%'; t[k] = 0;
            d_text(d, TK_IT400, 13, bx, bl(TK_IT400, 13, by + 16), t, 0, TK_TNUM, C_LABEL2, 0);
        } else if (u->state == 3) {
            su_lines(d, SU_CARD_Y + 76, "Ready. The phone restarts and starts the new version; if it doesn't start, the previous version comes back.", 3, C_LABEL2);
        } else if (u->state == -1) {
            d_text(d, TK_IT600, 15, ROW_TX, bl(TK_IT600, 15, SU_CARD_Y + 74), "Update failed", 0, 0, tk_hex(0xff453a, 1), 0);
            su_lines(d, SU_CARD_Y + 98, u->why, 2, C_LABEL2);
        } else
            su_lines(d, SU_CARD_Y + 76, u->notes[0] ? u->notes : "No notes.", 3, C_LABEL2);
    }
    const char *a = su_action();
    if (a) {
        float y = su_btn_y();
        d_rrect(d, GROUP_X, y, GROUP_W, 47, 16, tk_hex(0x151c33, 1));
        d_text_box(d, TK_IT400, 16, 195, y + 29, GROUP_W - 32, 1, a, 0, 0, ACCENT(), 0);
    }
    if (u->state == 1 || u->state == 3)
        su_lines(d, SU_BTN_Y + 47 + 10, "From the microSD card's STARBITE/UPDATE folder. Starbite OS keeps the previous version until the new one has started.", 3, W(.45f));
}
/* the About section's "Software" row on the Settings list (until Track B's General > Software Update) */
static float su_row_main(void)
{
    const appdef *A = &APPS[AP.a];
    for (int s = 0; s < A->ns; s++)
        for (int i = 0; i < A->sec[s].n; i++)
            if (!tk_strcmp(A->sec[s].rows[i].l, "Software")) return gen_row_y(A, s, i);
    return -1;
}
static void su_open(void)
{
    AS.sub = SU_SUB;
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
    if (BACK_HIT(x, y)) { AS.sub = 0; build_app_page(AP.a); return 1; }
    float by = su_btn_y();
    if (!su_action() || x < GROUP_X || x >= CONTENT_R || y < by || y >= by + 47) return 1;
    if (SU.u.state == 1) {                          /* Download and Install: the staging runs on refresh turns */
        SU.running = 1;
        (void)sh_update_stage(&SU.u);
    } else if (SU.u.state == 3) {
        if (sh_update_install() < 0) toast("The update can't be installed now");
    } else
        su_check();                                 /* Check Again / Try Again */
    build_app_page(AP.a);
    return 1;
}
static int su_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    (void)c;
    if (!is_app("settings") || x < GROUP_X || x >= CONTENT_R) return 0;
    float top = -1;
    if (!AS.sub) {
        float ry = su_row_main();
        if (ry >= 0 && y >= ry && y < ry + 47) top = ry;
    } else if (AS.sub == SU_SUB && su_action()) {
        float by = su_btn_y();
        if (y >= by && y < by + 47) top = by;
    }
    if (top < 0) return 0;
    *r = *g = (tk_rect){ GROUP_X, top, GROUP_W, 47 };
    *rad = 16;
    return 1;
}
/* the staging's turns: a chunk each, the page redrawn (sh_refresh / sh_next_update) */
static float su_next_update(float t)
{
    (void)t;
    return SU.running ? 4 : -1;
}
static int su_refresh(float t)
{
    (void)t;
    if (!SU.running || sh_moving()) return 0;
    int st = sh_update_stage(&SU.u);
    if (st != 2) SU.running = 0;
    if (AP.on && is_app("settings") && AS.sub == SU_SUB) { build_app_page(AP.a); return 1; }
    return 0;
}
/* the stills' states */
static int su_set(const char *what, const char *key, float v)
{
    if (tk_strcmp(what, "update")) return 0;
    if (!tk_strcmp(key, "open")) su_open();
    else if (!tk_strcmp(key, "stage")) {
        if (!SU.checked) su_check();
        for (int i = 0; i < (int)v; i++) (void)sh_update_stage(&SU.u);
        SU.running = SU.u.state == 2;
        AS.sub = SU_SUB;
    } else if (!tk_strcmp(key, "ready")) {
        if (!SU.checked) su_check();
        for (int i = 0; i < 1000 && (SU.u.state == 1 || SU.u.state == 2); i++) (void)sh_update_stage(&SU.u);
        SU.running = 0;
        AS.sub = SU_SUB;
    } else if (!tk_strcmp(key, "install"))
        (void)sh_update_install();
    if (AP.on) build_app_page(AP.a);
    return 1;
}
