/* P3x: Restart and Shut Down, iOS's power-off screen (included by shell_lock.c).
 *
 * The kernel's Power key held for 1 s (kern/input.c POWER_LONG_MS: KIN_POWER_LONG, power_key_long, kui_power_menu)
 * opens it through kui_power_hook (kern_next.c) -> sh_power_held(), and so does the Control Centre's Power circle.
 * The screen behind blurs and darkens (a snapshot) and shows:
 *   - at the top, "slide to power off": a glass track with a white thumb holding a red power glyph. The slide is
 *     the confirmation (iOS): let go past 85 % and the screen goes black, then sh_power_off() (Track A's 5391eee: it
 *     saves first). Let go before that and the thumb springs back;
 *   - at the bottom, two glass circles: Restart (sh_restart(): this OS again, or the boot menu) and Cancel.
 * A tap anywhere else, or the Power key (sleep), cancels. A refusal (< 0: the self test's dry run, a heat session)
 * fades the black back out and says so in a toast; the screen stays.
 * Everything is a few small layers drawn at open (the backdrop a 1 MB quarter-resolution blur, ~20 ms on the phone;
 * no glass bake: the lens draws the glass on the GPU row, a flat fill on the CPU one) and freed at close. The sheet is modal: sh_touch gives it every touch while it is up. */

#define PW_TR_X 40.0f
#define PW_TR_Y 92.0f
#define PW_TR_W 310.0f
#define PW_TR_H 72.0f
#define PW_TH_D 62.0f
#define PW_TH_MAX (PW_TR_W - PW_TH_D - 10)
#define PW_BTN_D 64.0f
#define PW_BTN_Y (H_PT - 150)              /* the circles' top (iOS: Cancel ~90 pt above the bottom, its label under) */
static float pw_btn_cx(int i) { return i ? 265.0f : 125.0f; }   /* 0 Restart, 1 Cancel */

static tk_layer pwl, pwdim, pwtrack, pwwords, pwthumb, pwbtns, pwhl, pwblack;
static struct {
    int on, slide, btn, moved, go;          /* go: 1 power off, 2 restart (the black fading in, then the call) */
    float a, at, thumb, tx0, x0, y0, gt;    /* a: the sheet's fade; gt: the black's */
    tk_spring sp;                            /* the thumb going back */
} PW;

static void pw_arc(dc *d, float cx, float cy, float r, float sw, float a0, float sweep, tk_col c)
{
    int bx = (int)tk_floorf(PT(cx - r - sw) - d->ox) - 2, by = (int)tk_floorf(PT(cy - r - sw) - d->oy) - 2;
    int n = (int)PT(2 * (r + sw)) + 5;
    tk_fm m;
    if (!tk_fm_new(&m, n, n)) return;
    tk_fm_arc(&m, d->ox + bx, d->oy + by, PT(cx), PT(cy), PT(r), PT(sw), a0, sweep, 1);
    tk_paint p = tk_solid(c);
    tk_draw_fm(d->b, &m, bx, by, &p, 1);
    tk_fm_free(&m);
}
/* SF Symbols' power: a ring open at the top and the stroke through the gap */
static void pw_glyph_power(dc *d, float cx, float cy, float s, tk_col c)
{
    float r = 9 * s, sw = 2.6f * s;
    pw_arc(d, cx, cy + 1 * s, r, sw, .72f, 6.2831853f - 1.44f, c);
    d_seg(d, cx, cy - 11.5f * s, cx, cy - 1 * s, sw, c);
}
/* arrow.clockwise: a ring open at the top right, its head at the top pointing on round */
static void pw_glyph_restart(dc *d, float cx, float cy, float s, tk_col c)
{
    float r = 10 * s, sw = 2.4f * s, a1 = 6.2831853f - .25f;
    pw_arc(d, cx, cy, r, sw, .55f, a1 - .55f, c);
    float tx = cx + r * tk_sinf(a1), ty = cy - r * tk_cosf(a1);          /* the arc's end */
    float dx = tk_cosf(a1), dy = tk_sinf(a1);                            /* the way round (clockwise) */
    float hx = tx + dx * 3.2f * s, hy = ty + dy * 3.2f * s, L = 6.2f * s;   /* the tip a little past the end */
    for (int k = -1; k <= 1; k += 2) {
        float ca = tk_cosf(2.45f * k), sa = tk_sinf(2.45f * k);         /* back from the tip, 40 degrees either side */
        d_seg(d, hx, hy, hx + (dx * ca - dy * sa) * L, hy + (dx * sa + dy * ca) * L, sw, c);
    }
}
static void pw_glyph_x(dc *d, float cx, float cy, float s, tk_col c)
{
    float h = 7.5f * s, sw = 2.6f * s;
    d_seg(d, cx - h, cy - h, cx + h, cy + h, sw, c);
    d_seg(d, cx - h, cy + h, cx + h, cy - h, sw, c);
}
static void pw_glass(tk_layer *l, dc *d, float x, float y, float w, float h, float r)
{
    if (tk_glass_lens) {
        kit_shape(l, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r));
        d_rrect(d, x, y, w, h, r, W(.08f));
    } else
        d_rrect(d, x, y, w, h, r, W(.2f));           /* the CPU row: flat (a bake would pause the hold) */
}

static void pw_place(void)
{
    float e = tk_bezier(.25f, .1f, .25f, 1, PW.a);
    pwl.alpha = e;
    pwl.hidden = !PW.on && PW.a <= 0;
    pwthumb.x = pwthumb.bx + tk_floorf(PT(PW.thumb) + .5f);
    pwwords.alpha = tk_clampf(1 - PW.thumb / (PW_TH_MAX * .6f), 0, 1);
    pwblack.alpha = PW.gt;
    pwblack.hidden = PW.gt <= 0;
}
static void pw_free(void)
{
    tk_layer *P[] = { &pwdim, &pwtrack, &pwwords, &pwthumb, &pwbtns, &pwhl };
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) { relayer(P[i]); P[i]->ngsh = 0; P[i]->hidden = 1; }
    pwl.child = 0;
    pwl.hidden = 1;
}
static void pw_link(void)
{
    if (!pwl.name) { tk_layer_init(&pwl, "power"); pwl.hidden = 1; }
    if (PW.on || PW.a > 0) {
        tk_layer *c[] = { &pwdim, &pwtrack, &pwwords, &pwthumb, &pwbtns, &pwhl, &pwblack };
        link(&pwl, c, (int)(sizeof c / sizeof c[0]));
    } else
        pwl.child = 0;
}
static void pw_build(void)
{
    uint64_t t0 = tk_us();
    /* the backdrop: the screen as it is now at quarter resolution (1 MB), blurred 20 pt and darkened, drawn 4x (the
     * Control Centre's scrim, cheaper: one level); no memory: a plain dim */
    relayer(&pwdim);
    tk_layer_init(&pwdim, "pw-dim");
    tk_bmp h;
    int ok = 0;
    if (tk_bmp_new(&h, TK_W / 2, TK_H / 2)) {
        tk_comp_frame(C, 0, 0);
        tk_bmp_down2(&h, shown_frame());
        if (tk_bmp_new(&pwdim.bmp, TK_W / 4, TK_H / 4)) {
            tk_bmp_down2(&pwdim.bmp, &h);
            tk_bmp_blur(&pwdim.bmp, PT(20) / 4);
            tk_bmp_satbright(&pwdim.bmp, 1.2f, 1);
            tk_bmp_tint(&pwdim.bmp, TK_RGBA(0, 0, 0, .55f));
            pwdim.sx = pwdim.sy = 4;
            pwdim.opaque = 1;
            ok = 1;
        }
        tk_bmp_free(&h);
    }
    if (!ok) {
        pwdim.solid = 0xb3000000u;                   /* black at .7 (premultiplied) */
        pwdim.solid_w = TK_W;
        pwdim.solid_h = TK_H;
    }
    tk_layer_init(&pwblack, "pw-black");
    pwblack.solid = 0xff000000u;
    pwblack.solid_w = TK_W;
    pwblack.solid_h = TK_H;
    pwblack.hidden = 1;
    dc d = mk(&pwtrack, "pw-track", PW_TR_X, PW_TR_Y, PW_TR_W, PW_TR_H, 2, 0);
    pw_glass(&pwtrack, &d, PW_TR_X, PW_TR_Y, PW_TR_W, PW_TR_H, PW_TR_H / 2);
    d = mk(&pwwords, "pw-words", PW_TR_X + PW_TH_D + 10, PW_TR_Y + 16, PW_TR_W - PW_TH_D - 20, 40, 2, 0);
    d_text_box(&d, TK_IT400, 20, PW_TR_X + PW_TH_D + 10 + (PW_TR_W - PW_TH_D - 10) / 2, bl(TK_IT400, 20, PW_TR_Y + 23),
               PW_TR_W - PW_TH_D - 30, 1, "slide to power off", 0, 0, W(.85f), 0);
    trim(&pwwords);
    d = mk(&pwthumb, "pw-thumb", PW_TR_X + 5, PW_TR_Y + 5, PW_TH_D, PW_TH_D, 2, 0);
    d_rrect(&d, PW_TR_X + 5, PW_TR_Y + 5, PW_TH_D, PW_TH_D, PW_TH_D / 2, W(1));
    pw_glyph_power(&d, PW_TR_X + 5 + PW_TH_D / 2, PW_TR_Y + 5 + PW_TH_D / 2, 1.15f, C_RED);
    tk_layer_prepare(&pwthumb);                      /* it moves under the finger */
    d = mk(&pwbtns, "pw-buttons", 60, PW_BTN_Y, 270, PW_BTN_D + 34, 2, 0);
    static const char *lab[2] = { "Restart", "Cancel" };
    for (int i = 0; i < 2; i++) {
        float cx = pw_btn_cx(i), cy = PW_BTN_Y + PW_BTN_D / 2;
        pw_glass(&pwbtns, &d, cx - PW_BTN_D / 2, PW_BTN_Y, PW_BTN_D, PW_BTN_D, PW_BTN_D / 2);
        if (i) pw_glyph_x(&d, cx, cy, 1, W(1));
        else pw_glyph_restart(&d, cx, cy, 1, W(1));
        d_text_box(&d, TK_IT500, 15, cx, bl(TK_IT500, 15, PW_BTN_Y + PW_BTN_D + 10), 110, 1, lab[i], 0, 0, W(.9f), 0);
    }
    tk_layer_init(&pwhl, "pw-press");
    pwhl.hidden = 1;
    lap_us("power sheet: made", tk_us() - t0);
}
/* the kernel's Power key held (kern_next.c next_power -> kui_power_hook): the sheet, over everything */
void sh_power_held(void)
{
    if (PW.on) return;
    if (sh_sleeping()) sh_wake();
    if (CF.on) cf_close(0);                          /* the kernel's own question goes: this one replaces it */
    pw_build();
    PW.on = 1;
    PW.at = 1;
    PW.a = 0;
    PW.slide = PW.moved = PW.go = 0;
    PW.btn = -1;
    PW.thumb = 0;
    PW.gt = 0;
    PW.sp.live = 0;
    sh_haptic_k(SH_HAP_MEDIUM);
    relink();
    pw_place();
}
static void pw_close(void)
{
    if (!PW.on) return;
    PW.on = 0;
    PW.at = 0;
    PW.slide = 0;
    pwhl.hidden = 1;
}
static void pw_reset(void)                        /* sleep, a film's start: gone at once */
{
    if (!PW.on && PW.a <= 0) return;
    PW.on = 0;
    PW.a = PW.at = 0;
    PW.go = 0;
    PW.gt = 0;
    pw_free();
    pw_place();
    relink();
}
static int pw_hit_btn(float x, float y)
{
    for (int i = 0; i < 2; i++) {
        float dx = x - pw_btn_cx(i), dy = y - (PW_BTN_Y + PW_BTN_D / 2);
        if (dx * dx + dy * dy <= 40.0f * 40.0f) return i;
    }
    return -1;
}
static void pw_press(int i)
{
    relayer(&pwhl);
    if (i < 0) { pwhl.hidden = 1; return; }
    float cx = pw_btn_cx(i);
    dc d = mk(&pwhl, "pw-press", cx - PW_BTN_D / 2, PW_BTN_Y, PW_BTN_D, PW_BTN_D, 1, 0);
    d_rrect(&d, cx - PW_BTN_D / 2, PW_BTN_Y, PW_BTN_D, PW_BTN_D, PW_BTN_D / 2, W(.22f));
    pwhl.hidden = 0;
}
static void pw_start(int what)                     /* the slide's end, or Restart: the black first */
{
    PW.go = what;
    PW.slide = 0;
    sh_haptic_k(SH_HAP_MEDIUM);
    pwblack.hidden = 0;
}
static int pw_touch(int ph, float x, float y, float t)
{
    (void)t;
    if (!PW.on) return 0;
    if (PW.go || PW.at <= 0) return 1;              /* going dark: nothing more */
    if (ph == 0) {
        PW.x0 = x; PW.y0 = y; PW.moved = 0;
        PW.btn = -1;
        float tx = PW_TR_X + 5 + PW.thumb;
        if (x >= tx - 8 && x <= tx + PW_TH_D + 8 && y >= PW_TR_Y - 8 && y <= PW_TR_Y + PW_TR_H + 8) {
            PW.slide = 1; PW.tx0 = PW.thumb; PW.sp.live = 0;
            return 1;
        }
        PW.btn = pw_hit_btn(x, y);
        pw_press(PW.btn);
        return 1;
    }
    if (tk_fabsf(x - PW.x0) > 8 || tk_fabsf(y - PW.y0) > 8) PW.moved = 1;
    if (PW.slide) {
        PW.thumb = tk_clampf(PW.tx0 + (x - PW.x0), 0, PW_TH_MAX);
        pw_place();
        if (ph == 2) {
            PW.slide = 0;
            if (PW.thumb >= PW_TH_MAX * .85f) { PW.thumb = PW_TH_MAX; pw_place(); pw_start(1); }
            else tk_spring_start(&PW.sp, PW.thumb, 0, 0, TK_SPR_FAST);
        }
        return 1;
    }
    if (ph == 1 && PW.btn >= 0 && pw_hit_btn(x, y) != PW.btn) { PW.btn = -1; pw_press(-1); }
    if (ph == 2) {
        int b = PW.btn;
        PW.btn = -1;
        pw_press(-1);
        if (b == 0) pw_start(2);
        else if (b == 1 || !PW.moved) pw_close();     /* Cancel, or a tap anywhere else */
    }
    return 1;
}
static int pw_tick(float dt, float t)
{
    (void)t;
    int mv = 0;
    if (PW.sp.live) { tk_spring_step(&PW.sp, dt); PW.thumb = tk_maxf(0, PW.sp.x); pw_place(); mv = 1; }
    if (PW.a != PW.at) {
        PW.a = PW.at > PW.a ? tk_minf(PW.at, PW.a + dt / .25f) : tk_maxf(PW.at, PW.a - dt / .2f);
        pw_place();
        if (!PW.on && PW.a <= 0) { pw_free(); relink(); }
        mv = 1;
    }
    if (PW.go > 0) {
        PW.gt = tk_minf(1, PW.gt + dt / .3f);
        pw_place();
        mv = 1;
        if (PW.gt >= 1) PW.go = -PW.go;             /* the black frame shows first; the call on the next tick */
    } else if (PW.go < 0) {
        int what = -PW.go;
        /* both save first and do not come back when they work (the host's fakes and the self test's dry runs do) */
        int r = what == 1 ? sh_power_off() : sh_restart();
        PW.go = 0;
        if (r < 0) toast(what == 1 ? "Can't switch off now" : "Can't restart now");   /* the thumb goes back after the fade */
        else { PW.on = 0; PW.at = 0; }                /* the host's fakes: done (the phone never gets here) */
        PW.gt = .999f;                               /* back: the black fades out again */
        mv = 1;
    } else if (PW.gt > 0) {
        PW.gt = tk_maxf(0, PW.gt - dt / .25f);
        if (PW.gt <= 0 && PW.on) { tk_spring_start(&PW.sp, PW.thumb, 0, 0, TK_SPR_FAST); }
        pw_place();
        mv = 1;
    }
    return mv;
}
static int pw_moving(void) { return PW.slide || PW.sp.live || PW.a != PW.at || PW.go || PW.gt > 0; }
/* the stills and films: "power=:V" opens the sheet with the thumb at V (0..1); "power=:-1" closes it */
static int pw_set(const char *what, const char *key, float v)
{
    (void)key;
    if (tk_strcmp(what, "power")) return 0;
    if (v < 0) { pw_reset(); return 1; }
    if (!PW.on) sh_power_held();
    PW.a = PW.at = 1;
    PW.thumb = PW_TH_MAX * tk_clampf(v, 0, 1);
    pw_place();
    return 1;
}
