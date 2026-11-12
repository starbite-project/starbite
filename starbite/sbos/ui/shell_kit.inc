/* SPDX-License-Identifier: MIT
 * The component kit for Starbite OS Next (helper B2, Track B): iOS 26's system components, shared by every page, to
 * docs/design/IOS-REFERENCE.md (3.1 alert, 3.2 action sheet, 3.4 sheets, 12 controls). Included by shell_lock.c
 * before shell_wifi.inc (it uses the shell's drawing helpers, layers and app state); its API is declared in
 * shell_lock.c's kit block so earlier code can call it.
 *
 *   kit_alert(title, msg, buttons, n, cb)   a centred glass card 300 wide (r 36): the title and the message
 *                                           left-aligned, 1-3 capsule buttons (two side by side, 132 + 8 + 132; three
 *                                           stacked), the destructive one's words red, the preferred one filled blue;
 *                                           the screen dimmed; from 1.10 on a spring, out with a fade. Modal: only its
 *                                           buttons answer (cb(i), i the button's index).
 *   kit_actions(title, buttons, n, cb)      an action sheet: a glass panel at the bottom (inset 6) with capsule rows;
 *                                           no Cancel row (iOS 26): a tap outside is the KIT_CANCEL button (or -1).
 *   kit_sheet(title, content_h, draw, tap, done)
 *                                           a sheet with a grabber, a close circle and two detents (medium: top y 379,
 *                                           inset 6, glass; large: y 52, edge to edge, opaque); the top drags it,
 *                                           down past the medium detent dismisses, a tap above it dismisses. draw(d,
 *                                           x, y, w) draws its content once (at open), tap(x, y) gets a tap in it.
 *   kit_d_switch(d, x, y, on, enabled)      the switch drawn into a page: 64 x 28, a 38 x 24 white pill, green on;
 *   kit_switch_anim(x, y, to_on, ground)    its thumb sliding over the page's new state (a small layer, 0.3 s);
 *                                           x, y in the content's space (the caller links ksw into its scroll group);
 *   kit_d_segment(d, x, y, w, labels, n, sel), kit_segment_hit(x, x0, w, n): the segmented control;
 *   kit_d_slider(d, x, y, w, frac); kit_slider_drag(x, y, w, frac, ground) / kit_slider_end(): the slider (a 6 pt
 *                                           track, the accent fill, a 38 x 24 lozenge); while dragged its track is
 *                                           redrawn in a small layer and the lozenge is a layer that moves.
 *
 * Glass: nothing is baked in a tap, and no card bake is kept. Every card is the compositor's Liquid Glass lens (lens.h:
 * comp2 on the GPU over the live backdrop, draw_lens on the CPU): the card's layer records its shape (tk_gstyle
 * regular, untinted) and holds only its words, buttons and the alert's near-opaque body. The switch's and the
 * slider's thumbs are drawn once (their first use) and kept (~90 KB). Only SBTK_NOLENS (the host's comparison with
 * the concept's baked glass) bakes a card flat, as the hold confirmation does. */

#define KA_W 300.0f                 /* 3.1 (M, iOS 26's HIG art) */
#define KA_R 36.0f
#define KA_TX 24.0f                 /* the words' inset */
#define KA_T1 37.0f                 /* the title's first baseline from the top */
#define KA_TM 32.0f                 /* title -> message baseline pitch */
#define KA_MB 28.0f                 /* the message's last baseline -> the buttons */
#define KA_BH 48.0f
#define KA_BM 14.0f                 /* the buttons from the card's edges (left, right, bottom) */
#define KA_BG 8.0f                  /* between buttons */
#define KA_DIM .5f                  /* dark: the screen to ~#0a0a0a */
#define KAS_IN 6.0f                 /* 3.2 / 3.4: the medium detent's inset */
#define KAS_R 28.0f
#define KAS_RH 40.0f                /* an action's capsule, 8 apart, 11 in from the panel */
#define KAS_RG 8.0f
#define KAS_RI 11.0f
#define KAS_DIM .25f
#define KSH_R 33.0f
#define KSH_HDR 64.0f               /* a sheet's grabber, its close circle and title */
#define KSH_MED (H_PT - 6 - 417)    /* the medium detent's top (y 379); the large one's (y 52) */
#define KSH_LARGE (SAFE_TOP + 8)
#define KSW_W 64.0f                 /* 12.4 */
#define KSW_H 28.0f
#define KSW_KW 38.0f
#define KSW_KH 24.0f
#define KSL_H 6.0f                  /* 12.3 */
#define KSL_KW 38.0f
#define KSL_KH 24.0f
#define KIT_MAXB 6

/* B8's palette (sbtk_colors.h) under the kit's names */
static tk_col KIT_GREEN(void) { return C_GREEN; }
static tk_col KIT_RED(void) { return C_RED; }
static tk_col KIT_BLUE(void) { return C_ACCENT; }                  /* 15: the accent, dark */
static tk_col KIT_SEC(void) { return C_LABEL2; }                   /* secondaryLabel, dark */

static void isl_close(void);        /* B7's pill (shell_island.inc) */
enum { KIT_NONE, KIT_ALERT, KIT_ACTIONS, KIT_SHEET };
static tk_layer kitl, kdim, kitg, kglass, kfill, kcard, kithl, ksw, kslf, kslk;   /* cp6: kithl was kbhl: the same object as shell_kb.inc's lit key (one TU), in two lists */
static tk_bmp kknob, kslknob;       /* the switch's thumb and the slider's, drawn once */
static struct {
    int kind, closing, press, n, cancel;
    kit_btn b[KIT_MAXB];
    char lab[KIT_MAXB][40];
    kit_cb cb;
    tk_rect br[KIT_MAXB];                     /* the buttons (screen pt, at rest) */
    float x, y, w, h, r;                      /* the card */
    float a, at, p;                           /* the fade, its target; the spring's progress */
    tk_spring sp;
    /* the sheet */
    float top, med, large, cont_h;
    int drag, moved, landed;
    float y0, top0;
    tk_tracker tr;
    void (*sdraw)(dc *d, float x, float y, float w);
    void (*stap)(float x, float y);
    void (*sdone)(void);
    /* the switch's animation */
    float sw_t, sw_x, sw_y;
    int sw_to;
    uint32_t sw_ground;
    /* the slider */
    int sl_on;
} KT;

static int kit_on(void) { return KT.kind != KIT_NONE; }

/* ---- the layers ---------------------------------------------------------------------------------------------------- */
static void kit_init(void)
{
    if (kitl.name) return;
    tk_layer_init(&kitl, "kit");
    kitl.hidden = 1;
    tk_layer_init(&kdim, "kit-dim");
    kdim.solid = 0xff000000u;
    kdim.solid_w = TK_W;
    kdim.solid_h = TK_H;
    tk_layer_init(&kitg, "kit-card-group");
    tk_layer_init(&kglass, "kit-glass");
    kglass.hidden = 1;
    tk_layer_init(&kfill, "kit-fill");
    kfill.hidden = 1;
    tk_layer_init(&kcard, "kit-card");
    tk_layer_init(&kithl, "kit-press");
    kithl.hidden = 1;
    tk_layer_init(&ksw, "kit-switch");
    ksw.hidden = 1;
    tk_layer_init(&kslf, "kit-slider-fill");
    kslf.hidden = 1;
    tk_layer_init(&kslk, "kit-slider-knob");
    kslk.hidden = 1;
}
/* the kit's subtree (relink: over the confirmation, under the call screen) */
static void kit_link(void)
{
    kit_init();
    tk_layer *g[] = { &kglass, &kfill, &kcard, &kithl };
    link(&kitg, g, 4);
    tk_layer *k[] = { &kdim, &kitg };
    link(&kitl, k, 2);
}
static void kit_free(void)
{
    relayer(&kcard); relayer(&kglass);
    kcard.ngsh = kglass.ngsh = 0;
    kglass.hidden = kfill.hidden = 1;
    kithl.hidden = 1;
    kitl.hidden = 1;
    KT.kind = KIT_NONE;
    KT.closing = 0;
    KT.press = -1;
}
/* glass: the compositor's lens over a shape recorded on the layer (no bake); SBTK_NOLENS bakes it flat instead */
static void kit_shape(tk_layer *l, float x, float y, float w, float h, float r)
{
    tk_gstyle_t s = tk_gstyle;
    tk_gstyle = (tk_gstyle_t){ 0, 0, 0 };          /* regular, untinted, at rest */
    tk_layer_glass_shape(l, x, y, w, h, r);
    tk_gstyle = s;
}
static void kit_glass(tk_layer *l, dc *d, float x, float y, float w, float h, float r)
{
    if (!tk_glass_lens) { d_lg_flat(d, x, y, w, h, r, 1); return; }
    kit_shape(l, PT(x) - d->ox, PT(y) - d->oy, PT(w), PT(h), PT(r));
}

/* ---- the alert ------------------------------------------------------------------------------------------------------ */
static void kit_copy(const kit_btn *b, int n)
{
    KT.n = tk_mini(n, KIT_MAXB);
    KT.cancel = -1;
    for (int i = 0; i < KT.n; i++) {
        int k = 0;
        while (b[i].label && b[i].label[k] && k < 39) { KT.lab[i][k] = b[i].label[k]; k++; }
        KT.lab[i][k] = 0;
        KT.b[i].label = KT.lab[i];
        KT.b[i].style = b[i].style;
        if (b[i].style == KIT_CANCEL) KT.cancel = i;
    }
}
/* an alert's capsule (#363639; the preferred one filled blue; the destructive one's words red) or an action sheet's
 * (#2c2c2e, 17 / 400) */
static void kit_button(dc *d, int i, float x, float y, float w, float h, int alert)
{
    int st = KT.b[i].style;
    tk_col bg = st == KIT_PREFERRED ? KIT_BLUE() : tk_hex(alert ? 0x363639 : 0x2c2c2e, 1);
    d_rrect(d, x, y, w, h, h / 2, bg);
    tk_col fg = st == KIT_DESTRUCTIVE ? KIT_RED() : INK;
    int f = alert || st == KIT_PREFERRED ? TK_IT600 : TK_IT400;
    d_text_box(d, f, 17, x + w / 2, bl(f, 17, y + (h - lh(f, 17)) / 2), w - 20, 1, KT.lab[i], 0, 0, fg, 0);
    KT.br[i] = (tk_rect){ x, y, w, h };
}
static void kit_show(int kind)
{
    kit_init();
    KT.kind = kind;
    KT.closing = 0;
    KT.press = -1;
    KT.a = 0;
    KT.at = 1;
    kitl.hidden = 0;
    kithl.hidden = 1;
    isl_close();                                   /* a toast under it goes (B7: the pill closes) */
    press_cancel();
    kit_link();
    relink();
}
static int kit_alert(const char *title, const char *msg, const kit_btn *b, int n, kit_cb cb)
{
    if (n < 1) return -1;
    kit_free();
    kit_copy(b, n);
    KT.cb = cb;
    /* the words: a title of up to two lines, a message of up to four, left-aligned (iOS 26) */
    char tl[4][96], ml[4][96];
    float wm, tw0 = KA_W - 2 * KA_TX;
    int nt = title && title[0] ? wrapn(tl, 2, TK_IT600, 17, title, tw0, &wm) : 0;
    int nm = msg && msg[0] ? wrapn(ml, 4, TK_IT400, 15, msg, tw0, &wm) : 0;
    /* two buttons side by side when their words fit, else stacked full width (272) */
    int vert = KT.n > 2;
    float b2 = (KA_W - 2 * KA_BM - KA_BG) / 2;
    if (KT.n == 2) for (int i = 0; i < 2; i++) if (tw(TK_IT600, 17, KT.lab[i], 0, 0) > b2 - 20) vert = 1;
    float bh = vert ? KT.n * KA_BH + (KT.n - 1) * KA_BG : KA_BH;
    float tb = KA_T1 + 22.0f * (nt ? nt - 1 : 0), m0 = nt ? tb + KA_TM : KA_T1, mb = nm ? m0 + 20.0f * (nm - 1) : tb;
    float h = (nt || nm ? mb + KA_MB : KA_BM) + bh + KA_BM;
    KT.w = KA_W;
    KT.h = h;
    KT.r = KA_R;
    KT.x = (390 - KA_W) / 2;
    KT.y = tk_floorf((H_PT - h) / 2);
    relayer(&kcard);
    dc d = mk(&kcard, "kit-alert", KT.x, KT.y, KT.w, KT.h, 2, 0);
    if (!d.b->px) { kit_free(); return -1; }
    kit_glass(&kcard, &d, KT.x, KT.y, KT.w, KT.h, KT.r);
    d_rrect(&d, KT.x, KT.y, KT.w, KT.h, KT.r, tk_hex(0x1e1e20, .78f));        /* nearly opaque: #1a1a1a .. #232323 */
    float x = KT.x + KA_TX;
    for (int i = 0; i < nt; i++) d_text(&d, TK_IT600, 17, x, KT.y + KA_T1 + 22.0f * i, tl[i], 0, 0, INK, 0);
    for (int i = 0; i < nm; i++) d_text(&d, TK_IT400, 15, x, KT.y + m0 + 20.0f * i, ml[i], 0, 0, KIT_SEC(), 0);
    float y = KT.y + KT.h - KA_BM - bh, bx = KT.x + KA_BM, bw = KA_W - 2 * KA_BM;
    if (vert) {
        int k = 0;                                   /* stacked: Cancel last (iOS), the others in their order */
        for (int i = 0; i < KT.n; i++) if (i != KT.cancel) kit_button(&d, i, bx, y + (KA_BH + KA_BG) * k++, bw, KA_BH, 1);
        if (KT.cancel >= 0) kit_button(&d, KT.cancel, bx, y + (KA_BH + KA_BG) * k, bw, KA_BH, 1);
    } else if (KT.n == 2) {
        int first = KT.cancel == 1 ? 1 : 0;          /* Cancel on the leading side (iOS) */
        kit_button(&d, first, bx, y, b2, KA_BH, 1);
        kit_button(&d, 1 - first, bx + b2 + KA_BG, y, b2, KA_BH, 1);
    } else
        kit_button(&d, 0, bx, y, bw, KA_BH, 1);
    kdim.alpha = 0;
    kitg.alpha = 0;
    kitg.ox = PT(195);
    kitg.oy = PT(KT.y + KT.h / 2);
    kitg.x = kitg.y = 0;
    kcard.hidden = 0;
    kit_show(KIT_ALERT);
    /* from 1.10 to 1.0 with a fade in, on a spring of response .35 s, damping .85 */
    tk_spring_start(&KT.sp, 0, 1, 0, TK_SPR_SHEET);
    KT.sp.k = 322.3f;
    KT.sp.c = 30.5f;
    KT.p = 0;
    return 0;
}

/* ---- the action sheet (3.2 without a source button: the sheet's geometry, capsule rows, no Cancel row) ------------ */
static int kit_actions(const char *title, const kit_btn *b, int n, kit_cb cb)
{
    if (n < 1) return -1;
    kit_free();
    kit_copy(b, n);
    KT.cb = cb;
    int ng = KT.n - (KT.cancel >= 0);
    if (ng < 1) return -1;
    float th = title && title[0] ? 36 : 0;
    KT.x = KAS_IN;
    KT.w = 390 - 2 * KAS_IN;
    KT.h = KAS_RI + th + ng * KAS_RH + (ng - 1) * KAS_RG + KAS_RI;
    KT.y = H_PT - KAS_IN - KT.h;
    relayer(&kcard);
    dc d = mk(&kcard, "kit-actions", KT.x, KT.y, KT.w, KT.h, 2, 0);
    if (!d.b->px) { kit_free(); return -1; }
    kit_glass(&kcard, &d, KT.x, KT.y, KT.w, KT.h, KAS_R);
    d_rrect(&d, KT.x, KT.y, KT.w, KT.h, KAS_R, tk_hex(0x1e1e20, .55f));
    if (th > 0) d_text_box(&d, TK_IT400, 13, 195, KT.y + KAS_RI + 18, KT.w - 40, 1, title, 0, 0, KIT_SEC(), 0);
    int k = 0;
    for (int i = 0; i < KT.n; i++) {
        if (i == KT.cancel) { KT.br[i] = (tk_rect){ -1, -1, 0, 0 }; continue; }
        kit_button(&d, i, KT.x + KAS_RI, KT.y + KAS_RI + th + (KAS_RH + KAS_RG) * k++, KT.w - 2 * KAS_RI, KAS_RH, 0);
    }
    kdim.alpha = 0;
    kitg.alpha = 1;
    kitg.sx = kitg.sy = 1;
    kitg.x = 0;
    kitg.y = PT(KT.h + 20);
    kcard.hidden = 0;
    kit_show(KIT_ACTIONS);
    tk_spring_start(&KT.sp, 0, 1, 0, TK_SPR_SHEET);
    KT.sp.k = 195;                                 /* a sheet: response .45, damping 1 */
    KT.sp.c = 27.9f;
    KT.p = 0;
    return 0;
}

/* ---- the sheet (3.4) ------------------------------------------------------------------------------------------------
 * Three layers: its glass (a 3-slice of four transparent rows stretched to the sheet's height: it records the sheet's
 * shape, so the lens draws it at any height for 23 KB), the opaque ground the large detent fades in (a solid clipped
 * round), and the grabber, the close circle, the title and the content (drawn once, as tall as the content). The
 * inset is 6 at the medium detent, 0 at the large one, in between while dragged. */
static float kit_sheet_k(float top) { return tk_clampf((top - KT.large) / (KT.med - KT.large), 0, 1); }   /* 1 medium, 0 large */
static void kit_sheet_place(float top)
{
    KT.top = top;
    float k = kit_sheet_k(top), in = KAS_IN * k, x = in, w = 390 - 2 * in, bot = H_PT - in;
    float h = tk_maxf(2, bot - top);
    kcard.y = kcard.by + tk_floorf(PT(top - KT.y) + .5f);
    kglass.y = tk_floorf(PT(top) + .5f);
    kglass.slice_h = PT(h);
    if (kglass.ngsh) { kglass.gsh[0][0] = PT(x); kglass.gsh[0][2] = PT(w); kglass.gsh[0][4] = PT(KSH_R); }
    kglass.gen++;
    /* the large detent's opaque ground (#1c1c1e); the dim from 10 % (medium) to 25 % (large); below the medium
     * detent both follow the sheet away */
    float o = 1 - k, away = tk_clampf((H_PT - top) / (H_PT - KT.med), 0, 1);
    kfill.x = PT(x); kfill.y = PT(top);
    kfill.solid_w = PT(w); kfill.solid_h = PT(h);
    kfill.clip = 1;
    kfill.clip_r = (tk_rect){ PT(x), PT(top), PT(w), PT(h) };
    kfill.clip_radius = PT(KSH_R);
    kfill.alpha = o;
    kfill.hidden = o <= .01f;
    kfill.gen++;
    kdim.alpha = (.1f + .15f * o) * away;
}
static int kit_sheet(const char *title, float content_h, void (*draw)(dc *d, float x, float y, float w), void (*tap)(float x, float y),
                     void (*done)(void))
{
    kit_free();
    KT.n = 0;
    KT.cb = 0;
    KT.sdraw = draw;
    KT.stap = tap;
    KT.sdone = done;
    KT.large = KSH_LARGE;
    KT.med = KSH_MED;
    KT.cont_h = tk_clampf(content_h, 0, H_PT - KSH_MED - 6 - KSH_HDR);
    KT.x = KAS_IN;
    KT.w = 390 - 2 * KAS_IN;
    KT.y = KT.med;
    KT.r = KSH_R;
    relayer(&kcard);
    dc d = mk(&kcard, "kit-sheet", 0, KT.y, 390, KSH_HDR + KT.cont_h, 0, 0);
    if (!d.b->px) { kit_free(); return -1; }
    if (!tk_glass_lens) d_lg_flat(&d, KT.x, KT.y, KT.w, H_PT - KAS_IN - KT.y, KT.r, 1);
    d_rrect(&d, 195 - 18, KT.y + 5, 36, 5, 2.5f, tk_hex(0x5a5a5e, 1));       /* the grabber: 36 x 5, 5 down */
    {
        /* the close button: a 44 glass circle with an x, 16 from the top and the leading edge */
        float cx = KT.x + 16 + 22, cy = KT.y + 16 + 22;
        d_rrect(&d, cx - 22, cy - 22, 44, 44, 22, W(.12f));
        d_seg(&d, cx - 6, cy - 6, cx + 6, cy + 6, 2.2f, INK);
        d_seg(&d, cx - 6, cy + 6, cx + 6, cy - 6, 2.2f, INK);
    }
    if (title && title[0]) d_text_box(&d, TK_IT600, 17, 195, KT.y + 16 + 22 + 6, KT.w - 140, 1, title, 0, 0, INK, 0);
    if (KT.sdraw) KT.sdraw(&d, KT.x + 20, KT.y + KSH_HDR, KT.w - 40);
    trim(&kcard);
    relayer(&kglass);
    tk_layer_init(&kglass, "kit-glass");
    if (tk_glass_lens && tk_bmp_new(&kglass.bmp, TK_W, 4)) {
        kglass.x = kglass.bx = 0;
        kglass.slice = 1;
        kglass.slice_s0 = kglass.slice_s1 = 0;
        kit_shape(&kglass, PT(KT.x), 0, PT(KT.w), 4, PT(KT.r));
    }
    tk_layer_init(&kfill, "kit-fill");
    kfill.solid = 0xff1c1c1eu;
    kitg.alpha = 1;
    kitg.sx = kitg.sy = 1;
    kitg.x = kitg.y = 0;
    kcard.hidden = 0;
    kglass.hidden = !kglass.bmp.px;
    kit_show(KIT_SHEET);
    KT.a = KT.at = 1;
    KT.landed = 0;
    tk_spring_start(&KT.sp, H_PT, KT.med, 0, TK_SPR_SHEET);
    KT.sp.k = 195;                                 /* present: response .45, damping 1 */
    KT.sp.c = 27.9f;
    kit_sheet_place(H_PT);
    return 0;
}
static void kit_sheet_to(float to, float v)
{
    tk_spring_start(&KT.sp, KT.top, to, v, TK_SPR_SHEET);
    KT.sp.k = 246.7f;                              /* to a detent: response .40, damping .85 */
    KT.sp.c = 26.7f;
    KT.landed = 0;
}

/* ---- closing ------------------------------------------------------------------------------------------------------ */
static void kit_dismiss(int i)
{
    if (!kit_on() || KT.closing) return;
    KT.closing = 1;
    KT.press = -1;
    hl_release(&kithl);                                     /* cp6: the pressed button stays lit as the card fades */
    kit_cb cb = KT.cb;
    KT.cb = 0;
    if (KT.kind == KIT_ALERT) KT.at = 0;                     /* fade only, .2 s */
    else if (KT.kind == KIT_ACTIONS) { tk_spring_start(&KT.sp, KT.p, 0, 0, TK_SPR_SHEET); KT.sp.k = 195; KT.sp.c = 27.9f; }
    else { tk_spring_start(&KT.sp, KT.top, H_PT + 20, 0, TK_SPR_SHEET); KT.sp.k = 195; KT.sp.c = 27.9f; }
    if (cb) cb(i);                                         /* (it may show the next alert: that one takes over) */
}
static void kit_sheet_end(void)
{
    void (*done)(void) = KT.sdone;
    KT.sdone = 0;
    kit_free();
    relink();
    if (done) done();
}

/* ---- touches ---------------------------------------------------------------------------------------------------- */
static int kit_btn_at(float x, float y)
{
    for (int i = 0; i < KT.n; i++) {
        tk_rect r = KT.br[i];
        if (r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
    }
    return -1;
}
static void kit_press(int i)
{
    KT.press = i;
    if (i < 0) { hl_release(&kithl); return; }            /* cp6: it fades (a quick tap shows it first) */
    tk_rect r = KT.br[i];
    hl_set(&kithl, r, r, r.h / 2, W(.16f));               /* iOS: the capsule's own fill lights (#363639 -> ~#545456) */
}
static int kit_touch(int phase, float x, float y, float t)
{
    if (!kit_on()) return 0;
    if (KT.closing) return 1;                               /* nothing goes through while it leaves */
    if (KT.kind == KIT_SHEET) {
        float top = KT.top;
        if (phase == 0) {
            KT.y0 = y;
            KT.top0 = top;
            KT.moved = 0;
            tk_tracker_reset(&KT.tr);
            tk_tracker_add(&KT.tr, y, t);
            KT.drag = y >= top && y < top + KSH_HDR ? 1 : y < top ? 2 : 3;   /* the top drags it, above it dismisses */
            KT.sp.live = 0;
            {   /* cp6: the close circle lights (its content space: the sheet at KT.y) */
                float cx = KT.x + 16 + 22, cy = top + 16 + 22;
                if (KT.drag == 1 && (x - cx) * (x - cx) + (y - cy) * (y - cy) < 26 * 26) {
                    tk_rect c = { KT.x + 16, top + 16, 44, 44 };
                    hl_set(&kithl, c, c, 22, PRESS_BTN);
                }
            }
            return 1;
        }
        if (phase == 1) {
            tk_tracker_add(&KT.tr, y, t);
            if (tk_fabsf(y - KT.y0) > 6) { KT.moved = 1; hl_release(&kithl); }
            if (KT.drag == 1 && KT.moved) {
                float nt = KT.top0 + y - KT.y0;
                if (nt < KT.large) nt = KT.large - rubber(KT.large - nt, H_PT) * .5f;
                kit_sheet_place(nt);
            }
            return 1;
        }
        int dr = KT.drag;
        KT.drag = 0;
        hl_release(&kithl);
        float cx = KT.x + 16 + 22, cy = KT.top + 16 + 22;
        if (dr == 1 && KT.moved) {
            /* where the flick would carry it (200 ms): the nearer detent, or away below the medium one */
            float v = tk_tracker_v(&KT.tr), proj = KT.top + v * 200;
            if (proj > KT.med + 90) { kit_dismiss(-1); KT.sp.v = v * 1000; }
            else kit_sheet_to(proj < (KT.med + KT.large) / 2 ? KT.large : KT.med, v * 1000);
        } else if (dr == 1 && (x - cx) * (x - cx) + (y - cy) * (y - cy) < 26 * 26) kit_dismiss(-1);   /* the close circle */
        else if (dr == 2 && !KT.moved) kit_dismiss(-1);
        else if (dr == 1) kit_sheet_to(KT.top > KT.large + 1 ? KT.large : KT.med, 0);   /* a tap on the grabber */
        else if (dr == 3 && !KT.moved && KT.stap) KT.stap(x, y - (KT.top - KT.y));
        return 1;
    }
    if (KT.p < .5f) return 1;                               /* still coming in */
    if (phase == 0) { kit_press(kit_btn_at(x, y)); return 1; }
    if (phase == 1) {
        if (KT.press >= 0 && kit_btn_at(x, y) != KT.press) kit_press(-1);
        return 1;
    }
    int i = KT.press >= 0 && kit_btn_at(x, y) == KT.press ? KT.press : -1;
    kit_press(-1);
    if (i >= 0) kit_dismiss(i);
    else if (KT.kind == KIT_ACTIONS && (y < KT.y || y > KT.y + KT.h)) kit_dismiss(KT.cancel);
    return 1;
}

/* ---- the switch, the segmented control, the slider (drawn into a page) --------------------------------------------- */
static void kit_knob_draw(tk_bmp *b, float w, float h)
{
    /* a white pill with a soft shadow, drawn once into its own small bitmap (8 pt of room for the shadow) */
    if (b->px) return;
    if (!tk_bmp_new(b, (int)PT(w + 16), (int)PT(h + 16))) return;
    tk_box_shadow(b, PT(8), PT(8), PT(w), PT(h), PT(h / 2), 0, PT(1.5f), PT(5), 0, TK_RGBA(0, 0, 0, .25f), 0);
    tk_paint p = tk_solid(W(1));
    tk_fill_rrect(b, PT(8), PT(8), PT(w), PT(h), PT(h / 2), &p, 1);
}
static void kit_blit_knob(dc *d, const tk_bmp *k, float x, float y, float alpha)
{
    if (k->px) tk_blit(d->b, k, (int)tk_floorf(PT(x - 8) - d->ox + .5f), (int)tk_floorf(PT(y - 8) - d->oy + .5f), alpha);
}
/* the switch with its track at (x, y); p 0 off .. 1 on (the animation's frames): off #39393d, on #34c759 */
static void kit_switch_at(dc *d, float x, float y, float p, int enabled)
{
    kit_knob_draw(&kknob, KSW_KW, KSW_KH);
    float a = enabled ? 1 : .45f;
    d_rrect(d, x, y, KSW_W, KSW_H, KSW_H / 2, tk_hex(0x39393d, a));
    if (p > 0) { tk_col on = KIT_GREEN(); on.a = p * a; d_rrect(d, x, y, KSW_W, KSW_H, KSW_H / 2, on); }
    float kx = x + 2 + (KSW_W - 4 - KSW_KW) * p;
    kit_blit_knob(d, &kknob, kx, y + (KSW_H - KSW_KH) / 2, enabled ? 1 : .5f);
}
static void kit_d_switch(dc *d, float x, float y, int on, int enabled) { kit_switch_at(d, x, y, on ? 1 : 0, enabled); }
/* the thumb's slide over the page's new state: a small layer over the switch, redrawn each frame for 0.3 s (its
 * ground the row's colour, so the static switch under it does not show) */
static void kit_sw_draw(void)
{
    float e = tk_bezier(.3f, 1.3f, .4f, 1, tk_minf(1, KT.sw_t));       /* response .30, damping .70: a small overshoot */
    float p = KT.sw_to ? e : 1 - e;
    if (!ksw.bmp.px) (void)mk(&ksw, "kit-switch", KT.sw_x - 4, KT.sw_y - 4, KSW_W + 8, KSW_H + 8, 0, 0);
    if (!ksw.bmp.px) return;
    dc d = { &ksw.bmp, 0, ksw.bx, ksw.by };
    tk_fill_rect_raw(&ksw.bmp, 0, 0, ksw.bmp.w, ksw.bmp.h, 0);
    tk_paint g = tk_solid(TK_RGBA((float)(KT.sw_ground >> 16 & 255), (float)(KT.sw_ground >> 8 & 255), (float)(KT.sw_ground & 255), 1));
    tk_fill_rrect(d.b, PT(KT.sw_x - 2) - d.ox, PT(KT.sw_y - 2) - d.oy, PT(KSW_W + 4), PT(KSW_H + 4), PT(KSW_H / 2 + 2), &g, 1);
    kit_switch_at(&d, KT.sw_x, KT.sw_y, tk_clampf(p, -.05f, 1.05f), 1);
    tk_layer_dirty(&ksw);
}
static void kit_switch_anim(float x, float y, int to_on, uint32_t ground)
{
    kit_init();
    relayer(&ksw);
    tk_layer_init(&ksw, "kit-switch");
    KT.sw_x = x;
    KT.sw_y = y;
    KT.sw_to = to_on;
    KT.sw_t = 0;
    KT.sw_ground = ground;
    kit_sw_draw();
    ksw.hidden = !ksw.bmp.px;
}
static int kit_sw_step(float dt)
{
    if (ksw.hidden) return 0;
    KT.sw_t += dt / .3f;
    if (KT.sw_t >= 1) { relayer(&ksw); ksw.hidden = 1; return 1; }   /* the page under it shows the same */
    kit_sw_draw();
    return 1;
}
/* 12.2: a 32 pt capsule, the selection a #636366 capsule 2 in, 13 / 500 words */
static void kit_d_segment(dc *d, float x, float y, float w, const char *const *labels, int n, int sel)
{
    float h = 32, sw = (w - 4) / n;
    d_rrect(d, x, y, w, h, h / 2, TK_RGBA(118, 118, 128, .24f));
    if (sel >= 0 && sel < n) d_rrect(d, x + 2 + sw * sel, y + 2, sw, h - 4, (h - 4) / 2, tk_hex(0x636366, 1));
    for (int i = 0; i < n; i++) {
        if (i && i != sel && i != sel + 1) d_rrect(d, x + 2 + sw * i - .25f, y + 9, .5f, h - 18, 0, W(.18f));
        d_text_box(d, TK_IT500, 13, x + 2 + sw * i + sw / 2, bl(TK_IT500, 13, y + (h - lh(TK_IT500, 13)) / 2), sw - 10, 1, labels[i], 0, 0,
                   INK, 0);
    }
}
static int kit_segment_hit(float x, float x0, float w, int n)
{
    if (x < x0 || x >= x0 + w) return -1;
    return tk_mini(n - 1, (int)((x - x0 - 2) / ((w - 4) / n)));
}
/* 12.3: the track (6 pt), the accent fill to the thumb, the white 38 x 24 lozenge; y is the thumb's top */
static float kit_slider_kx(float x, float w, float frac) { return x + KSL_KW / 2 + (w - KSL_KW) * tk_clampf(frac, 0, 1); }
static void kit_slider_track(dc *d, float x, float y, float w, float kx)
{
    float cy = y + KSL_KH / 2;
    d_rrect(d, x, cy - KSL_H / 2, w, KSL_H, KSL_H / 2, TK_RGBA(120, 120, 128, .36f));
    d_rrect(d, x, cy - KSL_H / 2, kx - x, KSL_H, KSL_H / 2, KIT_BLUE());
}
static void kit_d_slider(dc *d, float x, float y, float w, float frac)
{
    kit_knob_draw(&kslknob, KSL_KW, KSL_KH);
    float kx = kit_slider_kx(x, w, frac);
    kit_slider_track(d, x, y, w, kx);
    kit_blit_knob(d, &kslknob, kx - KSL_KW / 2, y, 1);
}
/* while the finger moves it: the track in a small layer of its own (a 6 pt band drawn again per move, on the row's
 * ground, which also hides the page's own thumb) and the lozenge a layer that moves. x, y, w: the slider in the
 * content's space (scroll 0) */
static void kit_slider_drag(float x, float y, float w, float frac, uint32_t ground)
{
    kit_init();
    kit_knob_draw(&kslknob, KSL_KW, KSL_KH);
    float kx = kit_slider_kx(x, w, frac);
    tk_paint g = tk_solid(TK_RGBA((float)(ground >> 16 & 255), (float)(ground >> 8 & 255), (float)(ground & 255), 1));
    if (!KT.sl_on) {
        KT.sl_on = 1;
        relayer(&kslf);
        tk_layer_init(&kslf, "kit-slider");
        (void)mk(&kslf, "kit-slider", x - 4, y - 6, w + 8, KSL_KH + 12, 0, 0);
        tk_layer_init(&kslk, "kit-slider-knob");
        kslk.bmp = kslknob;                                  /* a view (kept) */
    }
    if (kslf.bmp.px) {
        dc d = { &kslf.bmp, 0, kslf.bx, kslf.by };
        tk_fill_rrect(d.b, 0, 0, (float)d.b->w, (float)d.b->h, 0, &g, 1);
        kit_slider_track(&d, x, y, w, kx);
        tk_layer_dirty(&kslf);
    }
    kslf.hidden = !kslf.bmp.px;
    kslk.hidden = !kslknob.px;
    kslk.x = tk_floorf(PT(kx - KSL_KW / 2 - 8) + .5f);
    kslk.y = tk_floorf(PT(y - 8) + .5f);
    kslk.gen++;
}
static void kit_slider_end(void)
{
    KT.sl_on = 0;
    relayer(&kslf);
    kslf.hidden = 1;
    tk_layer_init(&kslk, "kit-slider-knob");
    kslk.hidden = 1;
}

/* ---- time ---------------------------------------------------------------------------------------------------------- */
static int kit_tick(float dt)
{
    int mv = kit_sw_step(dt);
    if (!kit_on()) return mv;
    if (KT.kind == KIT_ALERT) {
        if (KT.sp.live) { tk_spring_step(&KT.sp, dt); mv = 1; }
        KT.p = KT.sp.live ? KT.sp.x : 1;
        if (KT.a != KT.at) {
            KT.a = KT.at > KT.a ? tk_minf(KT.at, KT.a + dt / .2f) : tk_maxf(KT.at, KT.a - dt / .2f);
            mv = 1;
        }
        kitg.sx = kitg.sy = 1 + .1f * (1 - KT.p);
        kitg.alpha = KT.a;
        kdim.alpha = KA_DIM * KT.a;
        if (KT.closing && KT.a <= 0) { kit_free(); relink(); }
        return mv;
    }
    if (KT.kind == KIT_ACTIONS) {
        if (KT.sp.live) { tk_spring_step(&KT.sp, dt); mv = 1; }
        KT.p = KT.sp.x;
        kitg.y = tk_floorf(PT((1 - KT.p) * (KT.h + 20)) + .5f);
        kdim.alpha = KAS_DIM * tk_clampf(KT.p, 0, 1);
        if (KT.closing && !KT.sp.live) { kit_free(); relink(); }
        return mv;
    }
    if (KT.sp.live) {                                        /* the sheet */
        int live = tk_spring_step(&KT.sp, dt);
        kit_sheet_place(KT.sp.x);
        if (!live && KT.closing) kit_sheet_end();
        else if (!KT.closing && !KT.landed && tk_fabsf(KT.sp.x - KT.sp.to) < 2) { KT.landed = 1; sh_haptic_k(SH_HAP_SOFT); }   /* soft, on a detent (cp6: iOS's .soft) */
        mv = 1;
    }
    return mv | (KT.drag == 1);
}
static int kit_moving(void)
{
    return !ksw.hidden || (kit_on() && (KT.sp.live || KT.a != KT.at || (KT.kind == KIT_SHEET && KT.drag == 1)));
}
/* ---- the inset-grouped list (IOS-REFERENCE 6 / 12.5): one d_group / d_row pair for every list ------------------------
 * Cards x 16 w 358 r 26, rows 52, 17 / 400 label and value, a 30 pt icon (r 7.5) at card + 16 with the text at card +
 * 59 (else card + 16), the separator from the text to the card's edge, the chevron 16 in; headers sentence case
 * 15 / 600 8 above the card, footers 13 / 400 8 below. The card is B8's C_CARD (#1C1C1E on the black ground, the
 * palette decision of gap list #1); LG_GROUND is its RGB for the switch / slider knobs' backgrounds. */
#define LG_X 16.0f
#define LG_W 358.0f
#define LG_RE (LG_X + LG_W)
#define LG_RAD 26.0f
#define LG_ROW 52.0f
#define LG_TX (LG_X + 16)
#define LG_ITX (LG_X + 59)
#define LG_ICON 30.0f
#define LG_GAP 35.0f
#define LG_GROUND sbc_rgb(SBC_CARD)
static void d_group(dc *d, float y, float h) { d_rrect(d, LG_X, y, LG_W, h, LG_RAD, C_CARD); }
static void d_group_head(dc *d, float y, const char *t)
{
    d_text_box(d, TK_IT600, 15, LG_TX, y - 8 - tk_floorf(tk_descent(TK_IT600, PT(15)) / TK_PT + .5f), LG_W - 32, 0, t, 0, 0, KIT_SEC(), 0);
}
/* a footer's lines (at most four): its height (0 none); d 0: only measured */
static float d_group_foot(dc *d, float y, const char *t)
{
    char ln[4][96];
    float wm;
    int n = t && t[0] ? wrapn(ln, 4, TK_IT400, 13, t, LG_W - 32, &wm) : 0;
    if (d) for (int k = 0; k < n; k++) d_text(d, TK_IT400, 13, LG_TX, bl(TK_IT400, 13, y + 8 + 18.0f * k), ln[k], 0, 0, KIT_SEC(), 0);
    return n ? 8 + 18.0f * n : 0;
}
/* a row: icon 1 (the caller draws the tile with d_icon_tile), the label (lc), the value, the chevron, the separator;
 * right: where the value ends when something else sits at the right (a switch), 0 the default */
static void d_row(dc *d, float y, float h, int icon, const char *l, const char *v, int chev, int last, tk_col lc, float right)
{
    float lx = icon ? LG_ITX : LG_TX, base = y + h / 2 + 6;
    if (right <= 0) right = chev ? LG_RE - 16 - 8 - 10 : LG_RE - 16;
    float room = right - lx, vw = v && v[0] ? tw(TK_IT400, 17, v, 0, TK_TNUM) : 0;
    float lmax = vw > 0 ? tk_maxf(room - vw - 16, room / 2) : room;
    if (l && l[0]) d_text_box(d, TK_IT400, 17, lx, base, lmax, 0, l, 0, 0, lc, 0);
    if (vw > 0) {
        float lw = l && l[0] ? tk_minf(tw(TK_IT400, 17, l, 0, 0), lmax) : 0;
        d_text_box(d, TK_IT400, 17, right, base, room - lw - 16, 2, v, 0, TK_TNUM, KIT_SEC(), 0);
    }
    if (chev) d_icon(d, "chev", LG_RE - 16 - 8, y + h / 2 - 6.5f, .35f);
    if (!last) d_rrect(d, lx, y + h - .5f, LG_RE - lx, .5f, 0, C_SEP);
}
static void d_icon_tile(dc *d, float y, float h, uint32_t col) { d_rrect(d, LG_X + 16, y + (h - LG_ICON) / 2, LG_ICON, LG_ICON, 7.5f, tk_hex(col, 1)); }

/* back to the lock (the Power key, a film's reset): whatever was up goes, unanswered */
static void kit_reset(void)
{
    if (kit_on()) { KT.cb = 0; KT.sdone = 0; kit_free(); }
    relayer(&ksw); ksw.hidden = 1;
    kit_slider_end();
}
/* the host's stills: kit=alert / actions / sheet (a sample of each) */
static void kit_sample_cb(int i) { (void)i; }
static void kit_sample_draw(dc *d, float x, float y, float w)
{
    char ln[4][96];
    float wm;
    int n = wrapn(ln, 4, TK_IT400, 15, "A sheet rises from the bottom. Drag its top up to the large detent or down to close it.", w, &wm);
    for (int i = 0; i < n; i++) d_text(d, TK_IT400, 15, x, bl(TK_IT400, 15, y + 10 + 21.0f * i), ln[i], 0, 0, INK, 0);
}
static int kit_set(const char *key, float v)
{
    if (!tk_strcmp(key, "alert")) {
        static const kit_btn B[2] = { { "Cancel", KIT_CANCEL }, { "Delete", KIT_DESTRUCTIVE } };
        kit_alert("Delete Note?", "This note will be deleted.", B, 2, kit_sample_cb);
    } else if (!tk_strcmp(key, "alert3")) {
        static const kit_btn B[3] = { { "Turn On", KIT_PREFERRED }, { "Not Now", KIT_DEFAULT }, { "Cancel", KIT_CANCEL } };
        kit_alert("Turn On Cellular Data?", "The modem connects to the mobile network.", B, 3, kit_sample_cb);
    } else if (!tk_strcmp(key, "actions")) {
        static const kit_btn B[3] = { { "Share Note", KIT_DEFAULT }, { "Delete Note", KIT_DESTRUCTIVE }, { "Cancel", KIT_CANCEL } };
        kit_actions("This note", B, 3, kit_sample_cb);
    } else if (!tk_strcmp(key, "sheet") || !tk_strcmp(key, "sheetl")) {
        kit_sheet("Sheet", 120, kit_sample_draw, 0, 0);
    } else if (!tk_strcmp(key, "confirm")) {
        /* the kernel's hold confirmation on the alert (the modem's Start, as the host's sh_modem words it) */
        sh_confirm("Start the modem?", "Receive only: its status, no calls, SMS or data.", "A SIM or a network registration parks it.",
                   "The first start is a test: be there.", "Hold to start");
        CF.a = CF.at;
        cf_place();
        return 1;
    } else
        return 0;
    /* a set state is whole at once (v: the spring's end; sheetl: the large detent) */
    KT.sp.live = 0;
    KT.sp.x = KT.kind == KIT_SHEET ? KT.sp.x : 1;
    if (KT.kind == KIT_SHEET) kit_sheet_place(!tk_strcmp(key, "sheetl") ? KT.large : KT.med);
    KT.p = 1;
    KT.a = KT.at = 1;
    (void)v;
    kit_tick(0);
    return 1;
}
