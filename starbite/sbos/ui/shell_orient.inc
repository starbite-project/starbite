/* cp7 #5 (the user: "landscape where it makes sense"; Track A's sh_orientation, shell.h): the screens that turn with
 * the phone as on iPhone: Photos' viewer, the Camera (its glyphs and the photos it keeps) and the Calculator (its
 * landscape layout, shell_apps.inc); home, the lock and
 * every other screen stay portrait. The compositor has no turn: a screen that turns has its bitmaps turned exactly
 * (tk_bmp_turn) into the portrait panel, and its touches mapped into the landscape screen's points (ori_map: 802 x 390,
 * the landscape top-left at the origin). Polled every 200 ms while such a screen shows (each ask keeps the sensor
 * hub's accelerometer on; it stops itself 60 s after the last); upside down keeps the last answer (iPhone); the
 * Control Centre, the Notification Centre and the switcher over the app keep it as it is; Emergency mode never asks
 * (it stays portrait and quiet). A change: the app's content
 * fades in again (.22 s) in its new layout, without the status bar (iPhone hides it in landscape).
 * Included by shell_lock.c after the apps (shell_media.inc, shell_camera.inc). */

static int ori_cur;                      /* what the open screen shows: SH_ORI_PORTRAIT / _TURNED_LEFT / _TURNED_RIGHT */
static float ori_t = -1e9f, ori_fade = 1;
/* the quarter turns clockwise that make the content upright */
static int ori_q(int o) { return o == SH_ORI_TURNED_LEFT ? 1 : o == SH_ORI_TURNED_RIGHT ? 3 : 0; }
/* a portrait point (pt) into the landscape screen's points */
static void ori_map(int o, float *x, float *y)
{
    float px = *x, py = *y;
    if (o == SH_ORI_TURNED_LEFT) { *x = py; *y = 390 - px; }
    else if (o == SH_ORI_TURNED_RIGHT) { *x = H_PT - py; *y = px; }
}
/* a landscape offset (pt) as the portrait panel's */
static void ori_off(int o, float lx, float ly, float *dx, float *dy)
{
    *dx = lx; *dy = ly;
    if (o == SH_ORI_TURNED_LEFT) { *dx = -ly; *dy = lx; }
    else if (o == SH_ORI_TURNED_RIGHT) { *dx = ly; *dy = -lx; }
}
/* 1: a screen that turns shows; 2: something is over the app (keep what it is); 0: none */
static int ori_screen(void)
{
    if (!AP.on || AP.closing || locked || sp_app.live || EMG) return 0;    /* Emergency: no sensor polls (quiet) */
    if (CCS.on || nc_on() || SW.on) return 2;
    if (is_app("photos")) return MD.view >= 0 && MD.vanim != 3;
    return is_app("camera") || is_app("calc");
}
static void med_turn(int from, int to);
static void cam_turn(int from, int to);
static void ori_set(int o, int fade)
{
    if (o == ori_cur) return;
    int from = ori_cur;
    ori_cur = o;
    med_turn(from, o);
    cam_turn(from, o);
    if (is_app("calc") && AP.on && !AP.closing) {       /* the Calculator's page and display in the new layout */
        for (int i = 0; i < NST; i++) own_strip(AP.a, i);
        own_extras(AP.a);
        relink();
    }
    if (fade) { ori_fade = 0; appc.alpha = 0; }
    apsb.alpha = o ? 0 : appc.alpha;
    if (!o) apsb.hidden = 0;
}
static int ori_poll(float t)
{
    int want = ori_screen();
    if (want == 2) return 0;
    if (!want) {
        if (ori_cur == SH_ORI_PORTRAIT) return 0;
        ori_set(SH_ORI_PORTRAIT, AP.on && !AP.closing && !locked);
        return 1;
    }
    if (t - ori_t < 190) return 0;
    ori_t = t;
    int o = sh_orientation();
    if (o == SH_ORI_UPSIDE_DOWN || o == ori_cur) return 0;
    ori_set(o, 1);
    return 1;
}
static float ori_next_update(void) { return ori_screen() == 1 || ori_cur ? 200 : -1; }
static int ori_tick(float dt)
{
    if (ori_fade >= 1) return 0;
    ori_fade = tk_minf(1, ori_fade + dt / .22f);
    if (!sp_app.live) {
        appc.alpha = tk_bezier(.25f, .1f, .25f, 1, ori_fade);
        apsb.alpha = ori_cur ? 0 : appc.alpha;
    }
    return 1;
}
static int ori_moving(void) { return ori_fade < 1; }
