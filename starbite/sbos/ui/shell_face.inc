/* SPDX-License-Identifier: MIT
 * Face Unlock in the shell (docs/FACE-UNLOCK.md; the platform: shell.h's sh_face_*). Included at the end of
 * shell_passcode.inc (it uses the passcode screen's state).
 *
 * The lock: the screen's wake starts a scan when Face Unlock is ready (sh_face_status SH_FACE_READY); the padlock
 * above the clock shows while it looks and opens on a match (iOS's Face ID: the swipe up then goes straight in, no
 * passcode). A swipe up before the match shows the passcode screen and the scan goes on behind it: a match there
 * opens the phone too. A failed scan taps the motor and leaves the padlock closed; the passcode always works. The
 * Security mode's hint ("Turn your head slightly", come closer, more light) shows where "Swipe up to open" is.
 *
 * Settings > Face ID & Passcode: a "Face Unlock" group (Set Up Face Unlock; then Unlock with Face, Speed / Security,
 * Reset Face Unlock). Set-up: the choice of Speed or Security (an alert), then a full-screen circle with the camera's
 * picture (mirrored, as a mirror) and five arcs that fill as the head turns (centre, both sides, up, down). */

static struct {
    int scan;                       /* a scan runs (the lock) */
    int ok;                         /* matched: the swipe up needs no passcode */
    int hint;                       /* the scan's hint shown (SH_FH_*) */
    /* set-up */
    int on, built, poses, done, fail, ehint;
    int wait;                       /* set-up asked for while the pack loads: its mode + 1 */
    int test, count;                /* the face test image's screen: 1 the countdown (count), 2 looking, 3 done */
    int noface;                     /* (2 with "No Face Yet") */
    int was_locked;                 /* the lock state last told to the platform (sh_face_lock) */
    unsigned seq;
    float a, at;
    char msg[96];
} FX;
static tk_layer fxl, fxbg, fxcam, fxring, fxtext, fxbot;

#define FX_CY 300.0f                /* the circle's centre (pt) */
#define FX_R 118.0f                 /* the picture's radius; the arcs just outside it */
#define FX_PIC 160                  /* the picture's bitmap (px a side), scaled up by the compositor */
#define FX_BOT 735.0f

/* ---- the lock ------------------------------------------------------------------------------------------------- */
static int fx_ok(void) { return FX.ok && locked && !EMG; }
/* the lock's state to the platform at each change: the pack is kept only while locked (home and apps get the heap) */
static void fx_lockstate(void)
{
    int l = locked ? 1 : 2;
    if (l == FX.was_locked) return;
    FX.was_locked = l;
    sh_face_lock(locked);
}
static void fx_glyph(void)
{
    if (EMG) return;
    if (FX.ok || FX.scan) glyph_t = 1;
    if (FX.ok) glyph_open = 1;
}
static void fx_hint_text(const char *s)
{
    relayer(&hint);
    dc d = mk(&hint, "hint", 0, 756, 390, 16, 4, 0);
    if (!d.b->px) return;
    float w = tw(TK_IT600, 13, s, 0, 0);
    d_text(&d, TK_IT600, 13, (390 - w) / 2, 756 + 13, s, 0, 0, W(.75f), 1);
    trim(&hint);
    tk_layer_prepare(&hint);
}
static void fx_set_hint(int h)
{
    if (h == FX.hint) return;
    FX.hint = h;
    fx_hint_text(h == SH_FH_TURN ? "Turn your head slightly" : h == SH_FH_CLOSER ? "Move a little closer"
                 : h == SH_FH_LIGHT ? "More light is needed" : "Swipe up to open");
    if (h) hint_left = 2500;        /* shown (sh_tick's tween) while it lasts */
}
static void fx_wake(void)
{
    fx_lockstate();
    FX.ok = 0;
    if (!locked || EMG || FX.on) return;
    if (FX.scan) return;
    if ((sh_face_status() & SH_FACE_READY) && sh_face_scan(1) == 0) FX.scan = 1;
    fx_glyph();
}
static void fx_stop_scan(void)
{
    if (FX.scan) sh_face_scan(0);
    FX.scan = 0;
    fx_set_hint(SH_FH_NONE);
}
static void fx_sleep(void)
{
    fx_stop_scan();
    FX.ok = 0;
    fx_lockstate();
}
/* the scan's state: 1 something changed on the screen */
static int fx_poll(void)
{
    if (!FX.scan) return 0;
    int h = 0, r = sh_face_poll(&h);
    if (r == SH_FS_RUN) {
        if (h == FX.hint) return 0;
        fx_set_hint(h);
        return 1;
    }
    FX.scan = 0;
    fx_set_hint(SH_FH_NONE);
    if (r == SH_FS_MATCH && locked && !EMG) {
        sh_haptic_k(SH_HAP_LIGHT);
        if (PC.on && (PC.purpose == PCP_UNLOCK || PC.purpose == PCP_LAUNCH)) {   /* the passcode screen was up */
            pc_hide();
            pc_unlocked();
        } else {
            FX.ok = 1;
            fx_glyph();
            hint_left = 2200;                        /* "Swipe up to open" */
        }
    } else if (r == SH_FS_FAIL) {
        sh_haptic_k(SH_HAP_ERROR);
        if (PC.on && PC.mode == PCM_ENTER && !PC.n) {
            tk_memcpy(PC.msg, "Face not recognised", 20);
            PC.err = 0;
            pc_draw_head();
            pc_kids();
        }
    }
    return 1;
}
/* the passcode screen came up for the unlock: one more scan behind it (iOS looks again on the swipe) */
static void fx_rescan(void)
{
    if (FX.scan || FX.ok || EMG || !(sh_face_status() & SH_FACE_READY)) return;
    if (sh_face_scan(1) == 0) FX.scan = 1;
}

/* ---- set-up --------------------------------------------------------------------------------------------------- */
static void fx_draw_text(void)
{
    relayer(&fxtext);
    dc d = mk(&fxtext, "fx-text", 20, 70, 350, 120, 0, 0);
    if (!d.b->px) return;
    if (FX.test) {
        const char *t = FX.test == 1 ? "Look at the Phone" : FX.noface ? "No Face Yet" : FX.test == 2 ? "Keep Looking"
                      : "Thank You";
        const char *m = FX.test == 1 ? "Hold it upright, about 30 cm away."
                      : FX.noface ? "Hold the phone up in front of your face."
                      : FX.test == 2 ? "Look at the circle. Don't touch the screen."
                      : "Saving. The phone restarts by itself.";
        d_text_box(&d, TK_IT600, 30, 195, 112, 340, 1, t, 0, 0, INK, 0);
        d_text_box(&d, TK_IT400, 17, 195, 150, 330, 1, m, 0, 0, W(.85f), 0);
        trim(&fxtext);
        tk_layer_prepare(&fxtext);
        return;
    }
    d_text_box(&d, TK_IT600, 28, 195, 112, 340, 1, FX.done ? "Face Unlock Is Set Up" : "Face Unlock", 0, 0, INK, 0);
    const char *m = FX.fail ? FX.msg
                  : FX.done ? (sh_face_mode() == SH_FACE_SECURITY ? "Security: look at the phone and blink or turn your head slightly." : "Speed: look at the phone to unlock.")
                  : FX.ehint == SH_FH_CLOSER ? "Move a little closer."
                  : FX.ehint == SH_FH_LIGHT ? "More light is needed."
                  : FX.wait ? "Loading Face Unlock..."
                  : !(FX.poses & 1) ? "Look straight at the phone."
                  : "Move your head slowly in a circle.";
    d_text_box(&d, TK_IT400, 17, 195, 150, 330, 1, m, 0, 0, FX.fail ? tk_hex(0xff6961, 1) : W(.8f), 0);
    trim(&fxtext);
    tk_layer_prepare(&fxtext);
}
static void fx_draw_ring(void)
{
    /* at half resolution (a quarter of the memory: ~1 MB), drawn in halved pt and shown 2x */
    relayer(&fxring);
    const float x0 = 195 - FX_R - 22, y0 = FX_CY - FX_R - 22, sz = 2 * FX_R + 44;
    dc d = mk(&fxring, "fx-ring", x0 / 2, y0 / 2, sz / 2, sz / 2, 0, 0);
    if (!d.b->px) return;
    fxring.x = fxring.bx = tk_floorf(PT(x0));
    fxring.y = fxring.by = tk_floorf(PT(y0));
    fxring.ox = fxring.x;                           /* the scale's pivot: its own corner */
    fxring.oy = fxring.y;
    fxring.sx = fxring.sy = 2;
    d.ox = tk_floorf(PT(x0 / 2));                    /* the bitmap's origin in the halved space */
    d.oy = tk_floorf(PT(y0 / 2));
    const float PI = 3.14159265f, rr = (FX_R + 12) / 2, cx = 195.0f / 2, cy = FX_CY / 2;
    tk_col dim = W(.22f), lit = C_GREEN;
    /* the arcs (from 12 o'clock, clockwise; the picture is mirrored: the person's right is the screen's right):
     * up at the top, the person's right (FB_LEFT, bit 1) right, down at the bottom, the person's left left */
    struct { int bit; float a0; } A[4] = { { 1 << 3, -PI / 4 }, { 1 << 1, PI / 4 }, { 1 << 4, 3 * PI / 4 }, { 1 << 2, 5 * PI / 4 } };
    for (int i = 0; i < 4; i++)
        d_arc(&d, cx, cy, rr, 3, A[i].a0 + .08f, PI / 2 - .16f, FX.done || (FX.poses & A[i].bit) ? lit : dim);
    if (FX.poses & 1 || FX.done) d_arc(&d, cx, cy, (FX_R + 2) / 2, 1, 0, 2 * PI, FX.done ? lit : W(.5f));
    if (FX.test == 1 && FX.count > 0) {                 /* the countdown's number (halved space: 60 pt shown as 120) */
        char n[2] = { (char)('0' + (FX.count > 9 ? 9 : FX.count)), 0 };
        d_text_box(&d, TK_IT600, 60, cx, cy + 21, 100, 1, n, 0, 0, INK, 0);
    }
    tk_layer_prepare(&fxring);
}
static void fx_draw_bot(void)
{
    relayer(&fxbot);
    dc d = mk(&fxbot, "fx-bot", 20, FX_BOT - 30, 350, 44, 0, 0);
    if (!d.b->px) return;
    if (FX.test) { trim(&fxbot); tk_layer_prepare(&fxbot); return; }   /* the face test: no button */
    const char *t = FX.done ? "Done" : FX.fail ? "Try Again" : "Cancel";
    float w = capsule_w(t, 1);
    d_capsule_btn(&d, 195 - w / 2, FX_BOT - 30, t, FX.done || FX.fail ? ACCENT() : W(1), 1);
    if (FX.fail) d_text(&d, TK_IT400, 15, 30, FX_BOT - 2, "Cancel", 0, 0, W(.8f), 0);
    trim(&fxbot);
    tk_layer_prepare(&fxbot);
}
/* the newest frame into the circle: the middle square, mirrored, FX_PIC a side, the outside transparent */
static int fx_pull(void)
{
    int w = 0, h = 0;
    unsigned seq = 0;
    const uint32_t *px = sh_face_frame(&w, &h, &seq);
    if (!px || seq == FX.seq || !fxcam.bmp.px || w < 8 || h < 8) return 0;
    FX.seq = seq;
    tk_bmp *b = &fxcam.bmp;
    int side = w < h ? w : h, x0 = (w - side) / 2, y0 = (h - side) / 2;
    float c = (FX_PIC - 1) / 2.0f, r2 = (FX_PIC / 2.0f) * (FX_PIC / 2.0f);
    for (int y = 0; y < FX_PIC; y++) {
        const uint32_t *s = px + (size_t)(y0 + y * side / FX_PIC) * (size_t)w + x0;
        uint32_t *o = b->px + (size_t)y * b->stride;
        float dy = y - c;
        for (int x = 0; x < FX_PIC; x++) {
            float dx = x - c;
            o[x] = dx * dx + dy * dy <= r2 ? (s[(FX_PIC - 1 - x) * side / FX_PIC] | 0xff000000u) : 0;
        }
    }
    tk_layer_dirty(&fxcam);
    fxcam.hidden = 0;
    return 1;
}
static void fx_kids(void)
{
    tk_layer *k[] = { &fxbg, &fxcam, &fxring, &fxtext, &fxbot };
    link(&fxl, k, 5);
}
static void fx_free(void)
{
    relayer(&fxcam); relayer(&fxring); relayer(&fxtext); relayer(&fxbot);
    fxl.child = 0;
    fxl.hidden = 1;
    FX.built = 0;
}
static void fx_build(void)
{
    tk_layer_init(&fxl, "face-setup");
    tk_layer_init(&fxbg, "fx-ground");
    fxbg.solid = 0xff000000u;
    fxbg.opaque = 1;
    fxbg.solid_w = TK_W;
    fxbg.solid_h = TK_H;
    tk_layer_init(&fxcam, "fx-cam");
    if (tk_bmp_new(&fxcam.bmp, FX_PIC, FX_PIC)) {
        fxcam.x = fxcam.bx = tk_floorf(PT(195 - FX_R));
        fxcam.y = fxcam.by = tk_floorf(PT(FX_CY - FX_R));
        fxcam.ox = fxcam.x;
        fxcam.oy = fxcam.y;
        fxcam.sx = fxcam.sy = PT(2 * FX_R) / FX_PIC;
    }
    fxcam.hidden = 1;                               /* until the first frame */
    fx_draw_text();
    fx_draw_ring();
    fx_draw_bot();
    FX.built = 1;
}
static void fx_link(void)
{
    if (!FX.built) return;
    fx_kids();
    tk_layer *p = 0;
    for (tk_layer *l = root.child; l; p = l, l = l->next)
        if (l == &cvl) break;
    fxl.next = p ? p->next : root.child;
    if (p) p->next = &fxl; else root.child = &fxl;
}
static void fx_close(void)
{
    sh_face_stop();
    FX.on = 0;
    FX.at = 0;
    if (AP.on && is_app("settings")) build_app_page(AP.a);
}
static void fx_start(int mode)
{
    int r = sh_face_enrol(mode);
    if (r < 0 && r != -2) {
        const char *w = sh_face_why();
        toast(w && w[0] ? w : "Face Unlock could not start");
        return;
    }
    FX.wait = r == -2 ? mode + 1 : 0;              /* the pack still coming from the card: the screen waits for it */
    FX.on = 1;
    FX.poses = FX.done = FX.fail = FX.ehint = 0;
    FX.seq = 0;
    FX.msg[0] = 0;
    if (FX.built) fx_free();
    fx_build();
    FX.at = 1;
    fxl.alpha = FX.a;
    fxl.hidden = 0;
    relink();
}
static void fx_mode_cb(int i)
{
    if (i == 0 || i == 1) fx_start(i == 0 ? SH_FACE_SPEED : SH_FACE_SECURITY);
}
static void fx_setup(void)
{
    if (!sh_pass_on()) { toast("Set a passcode first"); return; }
    sh_face_prepare();                               /* the pack from the card while the alert is read (~2.5 s) */
    static const kit_btn B[3] = { { "Speed", KIT_PREFERRED }, { "Security", KIT_DEFAULT }, { "Cancel", KIT_CANCEL } };
    kit_alert("Speed or Security?",
              "Speed unlocks as soon as it sees you, with a more lenient match. Security matches more strictly and "
              "wants a blink or a slight turn of your head each time.", B, 3, fx_mode_cb);
}
static int fx_touch(int phase, float x, float y)
{
    if (!FX.on) return 0;
    if (FX.test) return 1;                          /* the face test's screen: every touch is its own (ignored) */
    if (phase != 2) return 1;
    if (y >= FX_BOT - 34 && y < FX_BOT + 20) {
        if (FX.fail && x < 110) { fx_close(); return 1; }
        if (FX.done) { FX.on = 0; FX.at = 0; if (AP.on && is_app("settings")) build_app_page(AP.a); toast("Face Unlock is on"); return 1; }
        if (FX.fail) { fx_start(sh_face_mode()); return 1; }
        fx_close();
    }
    return 1;
}
static int fx_setup_poll(void)
{
    if (FX.on && FX.test) { int ch = fx_pull(); if (ch) fx_kids(); return ch; }   /* the face test: the preview only */
    if (!FX.on || FX.done || FX.fail) return 0;
    if (FX.wait) {                                   /* the pack arrived? then the camera */
        int r = sh_face_enrol(FX.wait - 1);
        if (r == -2) return 0;
        FX.wait = 0;
        if (r < 0) {
            FX.fail = 1;
            const char *w = sh_face_why();
            int n = 0;
            while (w && w[n] && n < (int)sizeof FX.msg - 1) { FX.msg[n] = w[n]; n++; }
            FX.msg[n] = 0;
            fx_draw_text(); fx_draw_bot(); fx_kids();
            return 1;
        }
    }
    int poses = 0, h = 0, r = sh_face_enrol_poll(&poses, &h);
    int ch = fx_pull();
    if (poses != FX.poses) {
        if (poses & ~FX.poses) sh_haptic_k(SH_HAP_SELECT);
        FX.poses = poses;
        fx_draw_ring();
        fx_draw_text();
        ch = 1;
    }
    if (h != FX.ehint) { FX.ehint = h; fx_draw_text(); ch = 1; }
    if (r == 1) {
        FX.done = 1;
        sh_haptic_k(SH_HAP_SUCCESS);
        fx_draw_ring(); fx_draw_text(); fx_draw_bot();
        ch = 1;
    } else if (r < 0) {
        FX.fail = 1;
        const char *w = sh_face_why();
        int n = 0;
        while (w && w[n] && n < (int)sizeof FX.msg - 1) { FX.msg[n] = w[n]; n++; }
        FX.msg[n] = 0;
        if (!n) tk_memcpy(FX.msg, "Set-up stopped. Try again.", 27);
        sh_haptic_k(SH_HAP_ERROR);
        fx_draw_text(); fx_draw_bot();
        ch = 1;
    }
    if (ch) fx_kids();
    return ch;
}

/* ---- time ------------------------------------------------------------------------------------------------------ */
static int fx_tick(float dt)
{
    fx_lockstate();
    int mv = 0;
    if (FX.a != FX.at) {
        FX.a = FX.at > FX.a ? tk_minf(FX.at, FX.a + dt / .25f) : tk_maxf(FX.at, FX.a - dt / .25f);
        fxl.alpha = FX.a;
        if (!FX.on && FX.a <= 0 && FX.built) { fx_free(); relink(); }
        mv = 1;
    }
    return mv;
}
static int fx_moving(void) { return FX.a != FX.at; }
static float fx_next_update(void) { return FX.scan ? 33 : FX.on && FX.test ? 50 : FX.on && !FX.done && !FX.fail ? (FX.wait ? 100 : 40) : -1; }
static int fx_refresh(void) { fx_lockstate(); return fx_poll() | fx_setup_poll(); }
static void fx_reset(void)
{
    fx_sleep();
    if (FX.on) { sh_face_stop(); FX.on = 0; }
    FX.at = FX.a = 0;
    if (FX.built) fx_free();
}

/* ---- Settings > Face ID & Passcode: the Face Unlock group --------------------------------------------------------- */
#define FX_FOOT_SET "Unlock with your face using the front camera. You choose Speed or Security when you set it up. " \
                    "Your face never leaves the phone."
static int fx_rows(void)
{
    int s = sh_face_status();
    if (!(s & SH_FACE_AVAIL)) return 0;
    return s & SH_FACE_ENROLLED ? 4 : 1;
}
static const char *fx_foot(void)
{
    static char b[200];
    int s = sh_face_status();
    if (!(s & SH_FACE_ENROLLED)) return FX_FOOT_SET;
    const char *m = sh_face_mode() == SH_FACE_SECURITY
                  ? "Security: a strict match on two looks, and a blink or a slight turn of your head."
                  : "Speed: unlocks at the first look, with a more lenient match.";
    const char *w = s & SH_FACE_READY || !(s & SH_FACE_ON) ? "" : sh_face_why();
    int n = 0;
    for (const char *p = m; *p && n < 190; p++) b[n++] = *p;
    if (w && w[0]) { b[n++] = ' '; for (const char *p = w; *p && n < 198; p++) b[n++] = *p; if (n < 199) b[n++] = '.'; }
    b[n] = 0;
    return b;
}
/* the group's top (after the passcode's group and its footer) and its whole height (0 when not shown) */
static float fx_g(void) { return PC_G1 + LG_ROW * pc_n1() + d_group_foot(0, 0, PC_FOOT1) + LG_GAP + 12; }
static float fx_h(void) { int n = fx_rows(); return n ? LG_ROW * n + d_group_foot(0, 0, fx_foot()) + LG_GAP + 12 : 0; }
static void fx_page(dc *d)
{
    int n = fx_rows();
    if (!n) return;
    float g = fx_g();
    d_group_head(d, g, "Face Unlock");
    d_group(d, g, LG_ROW * n);
    if (n == 1) d_row(d, g, LG_ROW, 0, "Set Up Face Unlock", 0, 0, 1, KIT_BLUE(), 0);
    else {
        int s = sh_face_status(), sec = sh_face_mode() == SH_FACE_SECURITY;
        d_row(d, g, LG_ROW, 0, "Unlock with Face", 0, 0, 0, INK, LG_RE - 16 - 64 - 12);
        kit_d_switch(d, LG_RE - 16 - 64, g + (LG_ROW - 28) / 2, !!(s & SH_FACE_ON), 1);
        for (int i = 0; i < 2; i++) {
            float ry = g + LG_ROW * (1 + i);
            d_row(d, ry, LG_ROW, 0, i ? "Security" : "Speed", 0, 0, 0, INK, 0);
            if (i == sec) {
                float x = LG_RE - 16 - 14, cy = ry + LG_ROW / 2 + 1;
                d_seg(d, x, cy, x + 5, cy + 5, 2.2f, KIT_BLUE());
                d_seg(d, x + 5, cy + 5, x + 14, cy - 6, 2.2f, KIT_BLUE());
            }
        }
        d_row(d, g + LG_ROW * 3, LG_ROW, 0, "Reset Face Unlock", 0, 0, 1, KIT_RED(), 0);
    }
    d_group_foot(d, g + LG_ROW * n, fx_foot());
}
static void fx_reset_cb(int i)
{
    if (i != 1) return;
    sh_face_reset();
    if (AP.on && is_app("settings")) build_app_page(AP.a);
    toast("Face Unlock reset");
}
/* a tap in Settings' page: 1 taken */
static int fx_page_tap(float x, float y)
{
    int n = fx_rows();
    float g = fx_g();
    if (!n || x < LG_X || x >= LG_RE || y < g || y >= g + LG_ROW * n) return 0;
    int i = (int)((y - g) / LG_ROW);
    if (n == 1) { fx_setup(); return 1; }
    if (i == 0) sh_face_set_on(!(sh_face_status() & SH_FACE_ON));
    else if (i == 1 || i == 2) {
        if ((i == 2) != (sh_face_mode() == SH_FACE_SECURITY)) sh_haptic_k(SH_HAP_SELECT);
        sh_face_set_mode(i == 2 ? SH_FACE_SECURITY : SH_FACE_SPEED);
    } else {
        static const kit_btn B[2] = { { "Cancel", KIT_CANCEL }, { "Reset", KIT_DESTRUCTIVE } };
        kit_alert("Reset Face Unlock?", "Your face is forgotten. You can set Face Unlock up again.", B, 2, fx_reset_cb);
        return 1;
    }
    build_app_page(AP.a);
    return 1;
}
static int fx_page_press(float x, float y, tk_rect *r, tk_rect *gr, float *rad, tk_col *c)
{
    int n = fx_rows();
    float g = fx_g();
    if (!n || x < LG_X || x >= LG_RE || y < g || y >= g + LG_ROW * n) return 0;
    return sep_row(r, gr, rad, c, g + LG_ROW * (int)((y - g) / LG_ROW), LG_ROW, g, LG_ROW * n);
}

/* ---- the host's stills: face=ok (matched on the lock), face=scan (looking), face=setup:POSES (the set-up screen with
 * those pose bits, no camera), face=done (set up) ------------------------------------------------------------- */
static void apply_tweens(void);
/* the face test image (kern_next.c face_selftest_step): face=test-count:N, face=test-run, face=test-done, face=test-off */
static int fx_test(const char *key, float v)
{
    int st = !tk_strcmp(key, "test-count") ? 1 : !tk_strcmp(key, "test-run") ? 2 : !tk_strcmp(key, "test-done") ? 3
           : !tk_strcmp(key, "test-noface") ? 4 : 0;
    if (!st && tk_strcmp(key, "test-off")) return 0;
    if (!st) { FX.test = 0; FX.on = 0; FX.at = 0; relink(); return 1; }
    int fresh = !FX.on || !FX.test;
    FX.test = st;
    FX.count = (int)v;
    FX.poses = 0;
    FX.done = st == 3;
    if (st == 4) FX.test = 2, FX.noface = 1; else FX.noface = 0;   /* (the camera's circle as in "looking") */
    FX.fail = FX.wait = FX.ehint = 0;
    if (fresh) {
        FX.on = 1;
        FX.seq = 0;
        if (FX.built) fx_free();
        fx_build();
        FX.a = FX.at = 1;
        fxl.alpha = 1;
        fxl.hidden = 0;
        relink();
    } else {
        fx_draw_text();
        fx_draw_ring();
        fx_draw_bot();
        fx_kids();
    }
    return 1;
}
static int fx_set(const char *key, float v)
{
    if (fx_test(key, v)) return 1;
    if (!tk_strcmp(key, "ok")) { FX.ok = 1; FX.scan = 0; fx_glyph(); glyph_a = glyph_t; apply_tweens(); return 1; }
    if (!tk_strcmp(key, "scan")) { FX.scan = 1; fx_glyph(); glyph_a = glyph_t; apply_tweens(); return 1; }
    if (!tk_strcmp(key, "setup") || !tk_strcmp(key, "done")) {
        FX.on = 1;
        FX.poses = (int)v;
        FX.done = !tk_strcmp(key, "done");
        FX.fail = 0;
        FX.ehint = 0;
        if (FX.built) fx_free();
        fx_build();
        FX.a = FX.at = 1;
        fxl.alpha = 1;
        fxl.hidden = 0;
        relink();
        return 1;
    }
    return 0;
}
