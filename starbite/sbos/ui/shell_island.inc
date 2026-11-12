/* SPDX-License-Identifier: MIT
 * B7 (GAP-LIST #12, docs/design IOS-REFERENCE §11): Starbite's Dynamic Island equivalent. Included by shell_lock.c
 * after shell_comms.inc (it reads the call, SOS, USB power and the modem's state).
 *   - the status capsule: a coloured capsule behind the status bar's time while something is live, as on iOS's
 *     non-island iPhones (24 tall, centred on SB_MID, the text's width + 12; the status bars leave their left text
 *     out while it shows, sb_cap). In order:
 *       - a call (the call screen minimised): green #34C759 with the call's timer; a tap opens the call screen again;
 *       - SOS: red #FF383C, "SOS" (over the beacon too);
 *       - Survival Heat: orange #FF8D28 (Heat runs in the classic Starbite OS only, the shell has no getter for it:
 *         only the host's island=heat:1 shows it);
 *       - USB power out: amber (iOS yellow #FFCC00, dark text); a tap opens USB Power;
 *       - mobile data starting (starting, searching, registered without data yet): blue #0088FF; a tap opens
 *         Settings > Cellular.
 *     It morphs in from a 6 pt dot and back (spring response .35, damping .75).
 *   - the live pill: a black capsule centred at the top (y 6), compact 126 x 28. toast() is its transient expanded
 *     state: the pill springs out to the message's size (at most 374 wide, 3 lines of 15 / 600 white, radius up to
 *     44; response .45, damping .75: the island's bounce), the words fade in once it is nearly open, it holds 2.5 s
 *     (longer for a long message; toast_left: 6 s for the Clock's timer), then the words go and it springs back to 126 x 28 and fades. The pill is
 *     a solid layer clipped round (no bitmap, nothing to bake); only its words are a small layer, made at the toast
 *     and freed when the pill has gone. Over the lock it is the same.
 *   - the call screen: a swipe up from its bottom edge minimises a connected call into the capsule.
 * Nothing here sounds, transmits or starts anything: a tap only opens a screen. */

enum { ISL_NONE, ISL_CALL, ISL_SOS, ISL_HEAT, ISL_USB, ISL_DATA, ISL_JOB };   /* cp6: ISL_JOB a Linux / App Store job (iOS: a Live Activity) */
#define ISL_CAP_H 24.0f
#define ISL_CAP_X (MARGIN + 16 - 6)          /* the text where the time starts (MARGIN + 16) */
#define ISL_CAP_Y (SB_MID - ISL_CAP_H / 2)
#define ISL_Y 6.0f                           /* the pill's top */
#define ISL_CW 126.0f                        /* compact */
#define ISL_CH 28.0f
#define ISL_MAXW 374.0f                      /* expanded at most (§11) */
#define ISL_PADX 18.0f
#define ISL_LH 20.0f                         /* a line of 15 pt */
#define ISL_PADY 10.0f
static tk_layer islcap, islpill, isltxt;     /* in toastl (the island's group, over SOS's beacon) */
static void isl_link(void);
static struct {
    int kind;                                /* the capsule drawn (ISL_*) */
    char txt[16];
    float capw;
    int cap_to;                              /* the capsule's morph target (1 shown, 0 gone) */
    float cs, cv;                            /* its spring: 0 a 6 pt dot .. 1 the capsule */
    int bars;                                /* the status bars to draw again (sb_cap changed) */
    int heat;                                /* the host's island=heat:1 (no getter on the phone) */
    float usb_next;                          /* USB power's state while its page is closed (2 Hz while on) */
    int own, replay;                         /* the touch: 1 the capsule's tap, 2 the call screen's swipe */
    float x0, y0, t0;
    int pst;                                 /* the pill: 0 gone, 1 opening / open, 2 closing */
    float e, ev;                             /* its spring: 0 the from size .. 1 the to size */
    float fw, fh, tw_, th_;                  /* from and to sizes (pt) */
    float a;                                 /* its opacity */
    float hold_end;                          /* when it closes (now_ms), -1 not yet armed */
} IS = { .hold_end = -1 };

/* a spring on x towards to (§16: k = (2 pi / response)^2, c = 4 pi damping / response, mass 1); 1 while it moves */
static int isl_spring(float *x, float *v, float to, float resp, float damp, float dt)
{
    float k = (6.2831853f / resp) * (6.2831853f / resp), c = 12.566371f * damp / resp;
    while (dt > 0) {
        float h = tk_minf(dt, .004f);
        *v += (k * (to - *x) - c * *v) * h;
        *x += *v * h;
        dt -= h;
    }
    if (tk_fabsf(to - *x) < .002f && tk_fabsf(*v) < .02f) { *x = to; *v = 0; return 0; }
    return 1;
}

/* ---- the status capsule -------------------------------------------------------------------------------------- */
/* cp6: Track E's running job (sh_linux_job), asked at most once a second: "Linux 45 %" (its percentage, else "Linux") */
static struct { float t; int on; char pct[8]; } IJ = { -1e9f, 0, "" };
static int isl_job(float t)
{
    if (t - IJ.t < 1000 && t >= IJ.t) return IJ.on;
    IJ.t = t;
    char j[96];
    IJ.on = sh_linux_job(j, (int)sizeof j) == 1;
    IJ.pct[0] = 0;
    if (IJ.on) {                                 /* the first "N %" in the words */
        for (int i = 0; j[i]; i++)
            if (j[i] >= '0' && j[i] <= '9' && (i == 0 || j[i - 1] == ' ')) {
                int k = i, n = 0;
                while (j[k] >= '0' && j[k] <= '9' && n < 3) { IJ.pct[n++] = j[k++]; }
                if (j[k] == ' ' && j[k + 1] == '%' && n) { IJ.pct[n++] = ' '; IJ.pct[n++] = '%'; IJ.pct[n] = 0; break; }
                IJ.pct[0] = 0;
            }
    }
    return IJ.on;
}
static int isl_want(char *b)
{
    if (cm_live(CALL.state) && !CM.decl && !(CV.on && CV.at > 0)) {
        if (CALL.state == SH_CALL_ACTIVE) cm_dur(b, CALL.secs);
        else fmt_hm(b);
        return ISL_CALL;
    }
    if (SV.sos_on) { tk_memcpy(b, "SOS", 4); return ISL_SOS; }
    fmt_hm(b);
    if (IS.heat) return ISL_HEAT;
    if (SV.usb.state == SH_USB_ON) return ISL_USB;
    if (CELL.data == SH_DATA_STARTING || CELL.data == SH_DATA_SEARCHING || CELL.data == SH_DATA_REGISTERED) return ISL_DATA;
    if (!EMG && isl_job(now_ms)) {                   /* cp6: a Linux build / install runs (not in Emergency) */
        int k = 0;
        for (const char *p = "Linux"; *p;) b[k++] = *p++;
        if (IJ.pct[0]) { b[k++] = ' '; for (const char *p = IJ.pct; *p && k < 15;) b[k++] = *p++; }
        b[k] = 0;
        return ISL_JOB;
    }
    b[0] = 0;
    return ISL_NONE;
}
static tk_col isl_col(int k)
{
    return k == ISL_CALL ? C_GREEN : k == ISL_SOS ? C_RED : k == ISL_HEAT ? C_ORANGE :     /* B8's palette */
           k == ISL_USB ? C_YELLOW : C_ACCENT;
}
static void isl_cap_place(void)
{
    float s = tk_maxf(IS.cs, 0), w = tk_maxf(IS.capw, 1);
    islcap.sx = tk_maxf(.01f, 6 / w + (1 - 6 / w) * s);
    islcap.sy = tk_maxf(.01f, 6 / ISL_CAP_H + (1 - 6 / ISL_CAP_H) * s);
    islcap.ox = PT(ISL_CAP_X + w / 2);
    islcap.oy = PT(SB_MID);
    islcap.alpha = tk_clampf(IS.cs * 2.5f, 0, 1);
    islcap.hidden = !islcap.bmp.px || IS.cs <= .001f || booting;
}
static void isl_cap_draw(int k, const char *t)
{
    relayer(&islcap);
    int f = k == ISL_SOS ? TK_IT700 : TK_IT600;
    float w = tk_maxf(tw(f, 15, t, 0, TK_TNUM) + 12, 44);
    dc d = mk(&islcap, "isl-capsule", ISL_CAP_X, ISL_CAP_Y, w, ISL_CAP_H, 1, 0);
    d_rrect(&d, ISL_CAP_X, ISL_CAP_Y, w, ISL_CAP_H, ISL_CAP_H / 2, isl_col(k));
    float tx = ISL_CAP_X + (w - tw(f, 15, t, 0, TK_TNUM)) / 2;
    d_text(&d, f, 15, tx, SB_MID + 15 * .364f, t, 0, TK_TNUM, k == ISL_USB ? TK_RGBA(0, 0, 0, .85f) : W(1), 0);
    IS.capw = w;
    isl_link();                              /* (mk made the layer again: its siblings' links) */
}
/* the status bars without (or with) their left text again */
static void isl_bars(void)
{
    IS.bars = 0;
    build_lstatus();
    build_home_sb();
    tk_layer_prepare(&hparts[0]);
    if (AP.on && !AP.closing) build_apsb();
    relayer(&ccsb);
    if (CCS.built) {                         /* (as build_cc makes it) */
        dc d = mk(&ccsb, "cc-status", 0, 0, 390, 48, 2, 0);
        status_bar(&d, carrier);
        trim(&ccsb);
        cc_link();
        set_cc(CCS.p);
    } else cc_stale = 1;
    if (CV.on) cv_sb();
    relink();
    lock_ver++;
}
/* the capsule as the state says now (cheap when nothing changed); 1 if anything was drawn */
static int isl_sync(void)
{
    if (!toastl.name) return 0;              /* (before the bake's layers) */
    char b[16];
    int k = isl_want(b), ch = 0;
    if (k != ISL_NONE && (k != IS.kind || tk_strcmp(b, IS.txt))) {
        isl_cap_draw(k, b);
        if (IS.kind == ISL_NONE) { IS.cap_to = 1; if (IS.cs <= .001f) { IS.cs = 0; IS.cv = 0; } }
        ch = 1;
    } else if (k == ISL_NONE && IS.kind != ISL_NONE) { IS.cap_to = 0; ch = 1; }
    if ((k != ISL_NONE) != sb_cap) { sb_cap = k != ISL_NONE; IS.bars = 1; }
    IS.kind = k;
    tk_memcpy(IS.txt, b, sizeof IS.txt);
    if (IS.bars) { isl_bars(); ch = 1; }
    if (!cm_live(CALL.state) || (CV.on && CV.at > 0)) isl_call_min = 0;   /* over, or its screen is back */
    isl_cap_place();
    return ch;
}
static int isl_dirty(void)
{
    if (SL.black) return 0;                  /* asleep: at the wake (never a frame for it in the dark) */
    char b[16];
    int k = isl_want(b);
    return k != IS.kind || (k != ISL_NONE && tk_strcmp(b, IS.txt)) || IS.bars;
}
static int isl_cap_hit(float x, float y)
{
    if (IS.kind == ISL_NONE || IS.kind == ISL_SOS || IS.kind == ISL_HEAT || IS.cs < .5f || booting || CF.on || focus_on) return 0;
    if (IS.kind != ISL_CALL && locked) return 0;
    return y < 48 && x >= ISL_CAP_X - 10 && x < ISL_CAP_X + IS.capw + 10;
}
static void isl_cap_tap(void)
{
    sh_haptic();
    if (IS.kind == ISL_CALL) { isl_call_min = 0; cv_reopen(); return; }
    if (IS.kind == ISL_DATA) { cm_open_cellular(); return; }
    if (IS.kind == ISL_JOB && !is_app("store")) {    /* cp6: the job's page: the App Store */
        if (AP.on) app_finish_close();
        (void)open_app_key("store");
        return;
    }
    if (IS.kind == ISL_USB && !is_app("usb")) {
        int a = sv_app("usb");
        if (a < 0) return;
        app_finish_close();
        app_open(a);
    }
}

/* ---- the live pill (toasts) ---------------------------------------------------------------------------------- */
static void isl_pill_place(void)
{
    float e = IS.e, w = tk_maxf(1, IS.fw + (IS.tw_ - IS.fw) * e), h = tk_maxf(1, IS.fh + (IS.th_ - IS.fh) * e);
    float x = 195 - w / 2, r = tk_minf(h / 2, 44);
    tk_rect q = { PT(x), PT(ISL_Y), PT(w), PT(h) };
    islpill.solid = tk_premul(TK_RGBA(0, 0, 0, 1));
    islpill.x = q.x; islpill.y = q.y;
    islpill.solid_w = q.w; islpill.solid_h = q.h;
    islpill.clip = 1;
    islpill.clip_r = q;
    islpill.clip_radius = PT(r);
    islpill.alpha = IS.a;
    islpill.hidden = IS.pst == 0 || IS.a <= 0;
    islpill.gen++;
    isltxt.clip = 1;
    isltxt.clip_r = q;
    isltxt.clip_radius = PT(r);
    float ta = IS.pst == 1 ? tk_clampf((e - .55f) / .35f, 0, 1) : tk_clampf((e - .7f) / .3f, 0, 1);
    isltxt.alpha = ta * IS.a;
    isltxt.hidden = islpill.hidden || !isltxt.bmp.px || isltxt.alpha <= 0;
}
static void isl_pill_gone(void)
{
    IS.pst = 0; IS.e = IS.ev = 0; IS.a = 0; IS.hold_end = -1;
    toast_left = 0;
    relayer(&isltxt);
    isl_pill_place();
}
static void toast(const char *msg)
{
    char ln[4][96];
    float wm;
    int n = wrapn(ln, 3, TK_IT600, 15, msg, ISL_MAXW - 2 * ISL_PADX, &wm);
    if (n < 1) n = 1;
    float w = tk_minf(ISL_MAXW, tk_maxf(ISL_CW, wm + 2 * ISL_PADX)), h = ISL_PADY * 2 + ISL_LH * n;
    /* from where the pill is now (a toast over a toast), else from the compact pill */
    float cw = IS.fw + (IS.tw_ - IS.fw) * IS.e, ch = IS.fh + (IS.th_ - IS.fh) * IS.e;
    if (IS.pst == 0) { cw = ISL_CW; ch = ISL_CH; IS.a = 0; }
    IS.fw = cw; IS.fh = ch; IS.tw_ = w; IS.th_ = h;
    IS.e = 0; IS.ev = 0;
    IS.pst = 1;
    relayer(&isltxt);
    dc d = mk(&isltxt, "isl-text", 195 - w / 2, ISL_Y, w, h, 1, 0);
    for (int i = 0; i < n; i++)
        d_text_box(&d, TK_IT600, 15, 195, ISL_Y + ISL_PADY + ISL_LH * i + 15.45f, w - 2 * ISL_PADX + 1, 1, ln[i], 0, 0, W(1), 0);
    trim(&isltxt);
    isl_link();
    isltxt.clip = 1;
    /* cp6: long enough to read: 2.5 s, and 60 ms more a character past 24 (6 s at most); armed at the next look (the
     * Clock's timer sets 6000 after this) */
    toast_left = tk_minf(6000, 2500 + 60 * (float)tk_maxi(0, (int)tk_strlen(msg) - 24));
    IS.hold_end = -1;
    isl_pill_place();
}
/* the hold: toast_left (ms from now) becomes the closing time */
static void isl_arm(void)
{
    if (toast_left > 0) { IS.hold_end = now_ms + toast_left; toast_left = 0; }
}
static void isl_close(void)
{
    if (IS.pst != 1) return;
    IS.fw = ISL_CW; IS.fh = ISL_CH;          /* back to the compact pill, then gone */
    IS.e = 1; IS.ev = 0;
    IS.pst = 2;
    IS.hold_end = -1;
}
static int isl_pill_tick(float dt, float t)
{
    if (!IS.pst) return 0;
    isl_arm();
    int mv = 0;
    if (IS.pst == 1) {
        mv |= isl_spring(&IS.e, &IS.ev, 1, .45f, .75f, dt);
        if (IS.a < 1) { IS.a = tk_minf(1, IS.a + dt / .12f); mv = 1; }
        if (IS.hold_end >= 0 && t >= IS.hold_end) isl_close();
    }
    if (IS.pst == 2) {
        mv = isl_spring(&IS.e, &IS.ev, 0, .38f, 1, dt);
        IS.a = tk_clampf(IS.e / .3f, 0, 1);
        if (!mv || IS.e <= .005f) { isl_pill_gone(); return 1; }
        mv = 1;
    }
    isl_pill_place();
    return mv;
}

/* ---- the shell's hooks --------------------------------------------------------------------------------------- */
static void isl_init(void)
{
    tk_layer_init(&toastl, "island");
    tk_layer_init(&islcap, "isl-capsule");
    islcap.hidden = 1;
    tk_layer_init(&islpill, "isl-pill");
    islpill.hidden = 1;
    tk_layer_init(&isltxt, "isl-text");
    isltxt.hidden = 1;
}
static void isl_link(void)
{
    tk_layer *k[] = { &islcap, &islpill, &isltxt };
    link(&toastl, k, 3);
}
/* every frame: the springs, the capsule's state; 1 to keep frames */
static int isl_tick(float dt, float t)
{
    int mv = isl_pill_tick(dt, t);
    if (isl_sync()) mv = 1;
    if (IS.cs != (float)IS.cap_to || IS.cv != 0) {
        int live = isl_spring(&IS.cs, &IS.cv, (float)IS.cap_to, .35f, .75f, dt);
        if (!live && !IS.cap_to) { relayer(&islcap); IS.cs = 0; }
        isl_cap_place();
        mv = 1;
    }
    return mv;
}
static int isl_moving(void)
{
    return (IS.pst == 1 && (IS.e != 1 || IS.ev != 0 || IS.a < 1)) || IS.pst == 2 ||
           IS.cs != (float)IS.cap_to || IS.cv != 0 || isl_dirty();
}
/* the platform's timer: the pill's hold, USB power's state while its page is closed (2 Hz while on) */
static float isl_next_update(float t)
{
    float best = -1;
    isl_arm();
    if (IS.pst == 1 && IS.hold_end >= 0) best = tk_maxf(1, IS.hold_end - t);
    if (SV.usb.state != SH_USB_OFF && !is_app("usb")) {
        float n = tk_maxf(1, IS.usb_next - t);
        if (best < 0 || n < best) best = n;
    }
    if (IJ.on || IS.kind == ISL_JOB) {               /* cp6: a running job's percentage, once a second */
        float n = tk_maxf(1, IJ.t + 1000 - t);
        if (best < 0 || n < best) best = n;
    }
    return best;
}
static int isl_refresh(float t)
{
    int ch = 0;
    if (SV.usb.state != SH_USB_OFF && !is_app("usb") && t >= IS.usb_next) { IS.usb_next = t + 500; sh_usb(&SV.usb); }
    if (IS.pst == 1 && IS.hold_end >= 0 && t >= IS.hold_end) { isl_close(); ch = 1; }
    ch |= isl_sync();
    return ch;
}
/* every touch first: the capsule's tap, the call screen's swipe up (minimise). 1 taken, 2 given on (the gesture was
 * replayed into sh_touch: its state stays) */
static int isl_touch(int ph, float x, float y, float t)
{
    if (IS.replay) return 0;
    if (ph == 0) {
        IS.own = 0;
        /* (the call screen's home swipe that minimises it into the capsule is shell_comms.inc's: B5's gesture,
         * cv_minimise) */
        if (isl_cap_hit(x, y)) IS.own = 1;
        if (!IS.own) return 0;
        IS.x0 = x; IS.y0 = y; IS.t0 = t;
        return 1;
    }
    if (!IS.own) return 0;
    float dx = x - IS.x0, dy = y - IS.y0;
    if (IS.own == 1) {
        if (dx * dx + dy * dy > 10 * 10) {
            /* not a tap: the gesture as if the capsule were not there (the Notification Centre's pull) */
            IS.own = 0;
            IS.replay = 1;
            sh_touch(0, IS.x0, IS.y0, IS.t0);
            sh_touch(ph, x, y, t);
            IS.replay = 0;
            return 2;
        }
        if (ph == 2) { IS.own = 0; isl_cap_tap(); }
        return 1;
    }
    return 1;
}
/* the host's stills and films: island=TEXT (a toast, animated), island=heat:V, island=sync:1 (the capsule as the
 * state says, settled: after sv=usb:1 ...), island=open:1 (the pill settled open), island=min:1 (the call screen
 * minimised now) */
static void isl_set(const char *key, float v)
{
    if (!tk_strcmp(key, "heat") || !tk_strcmp(key, "sync")) {   /* (sync: the capsule as the state says, settled) */
        if (key[0] == 'h') IS.heat = v > 0;
        isl_sync();
        IS.cs = (float)IS.cap_to; IS.cv = 0;
        isl_cap_place();
        return;
    }
    if (!tk_strcmp(key, "open")) {
        if (IS.pst) { IS.e = 1; IS.ev = 0; IS.a = 1; IS.pst = 1; isl_pill_place(); }
        return;
    }
    if (!tk_strcmp(key, "min")) {
        if (CV.on && cm_live(CALL.state)) { isl_call_min = 1; cv_free(); CV.a = CV.at = 0; }
        isl_sync();
        IS.cs = (float)IS.cap_to; IS.cv = 0;
        isl_cap_place();
        return;
    }
    toast(key);
}
/* a still's toast, whole at once (sh_set's "toast", Emergency's) */
static void isl_settle(void)
{
    if (!IS.pst) return;
    IS.e = 1; IS.ev = 0; IS.a = 1; IS.pst = 1;
    isl_pill_place();
}
static void isl_toast_hide(void)
{
    isl_pill_gone();
}
static void isl_reset(void)
{
    isl_call_min = 0;
    IS.own = 0;
    isl_pill_gone();
}
