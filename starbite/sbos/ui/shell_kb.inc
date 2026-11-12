/* ---- B3: the shell keyboard, as iOS 26's dark keyboard (included by shell_lock.c before Notes) -------------------
 *
 * One keyboard for every text field: Notes, Search, Messages, the Settings editors (Wi-Fi's password, the Medical
 * Card), Sun's place, a Linux app's text focus and the App Store's search.
 *
 * - Look: a dark panel (two solid layers, no bitmap) with rounded top corners; the keys in one layer drawn once when
 *   the keyboard comes up and kept while it is up. Letters and space on the lighter grey, the function keys darker;
 *   shift / caps lock / delete are the pack's kb-shift, kb-shift-on and kb-delete glyphs; return is blue with the
 *   field's word (search, join, done) or dark "return". The 123 and #+= layers are complete (the font has no euro
 *   sign: #+='s second row has nine keys). The emoji key is there but dimmed: the font has no emoji, and a tap says so.
 * - Behaviour: the key balloon over a pressed letter (its own small layer); shift on touch down, a double tap locks
 *   it (caps lock); a sentence's first letter capitalised; two spaces make ". "; delete fires on touch down and
 *   repeats while held, faster the longer it is held; a light haptic tap per key (kb_haptics()).
 * - Speed: a key never redraws the keyboard. Shift and auto-capitalisation redraw only the letter keys and shift (in
 *   place), a layer change the keys; the page under it is not built again; the caller redraws only its field.
 *
 * How a page uses it:
 * - while drawing its page (any strip), kb_draw(d, shift, num) / kb_draw_r(...) says "the keyboard is up here"
 *   (it draws nothing into d: shift / num are the start state when no kb_focus() came first);
 * - kb_focus(flags, return word, text) when a field takes the focus (the start layer, auto-capitalisation);
 * - the keyboard takes its own touches (sh_touch's G_KB) and sends each key to the page as a tap in the key's
 *   middle (app_tap); inside it the page asks kb_edit(buf, max) (a C string edited in place) or kb_take() (the
 *   key itself), or the old kb_hit(x, y, shift, num, &c) (1 a character, 4 delete, 5 return; shift and 123 are the
 *   keyboard's own now and come back as 0).
 * ------------------------------------------------------------------------------------------------------------------ */
static void app_tap(float x, float y);
static int is_app(const char *k);
static int ap_pending;                 /* (an app page drawn over frames: defined with app_build_step) */
static void kb_panel(void);
static int em_take_sel(const char *buf);

/* the field: kb_focus's flags */
#define KB_AUTOCAP 1        /* sentences start with a capital; two spaces make ". " */
#define KB_DIGITS 2         /* it opens on the 123 layer */
#define KB_NL 4             /* return is a new line in the text (kb_edit adds it) */
#define KB_ASCII 8          /* no £ ¥ • (a Wi-Fi password, a Linux terminal: ASCII only) */
#define KB_PHONE 16         /* 123's second row has + where $ was (a phone number) */

/* the metrics (pt; 390 wide): docs/design/IOS-REFERENCE.md §4 (design-ref bfe513f). Keys 31.6 x 44, r 8, 6 pt gaps,
 * 4 pt side margins, rows 54 apart; in iOS 26 every key has the same fill; return is the ⏎ glyph (a field's blue
 * word key where it has one: search, join, done); the emoji glyph sits under the keys at x 24. */
#define KB_TOP 540.0f                      /* the panel's top (the fields above it stop here) */
#define KB_PR 30.0f                        /* its top corners */
#define KB_RY(r) (547.0f + 54.0f * (r))    /* a row's top (the last ends at 753: a 49 pt strip under it, iOS's 50) */
#define KB_KW 31.6f                        /* a letter key's width */
#define KB_KP 37.6f                        /* its pitch (6 pt gaps) */
#define KB_KH 44.0f                        /* a key's height */
#define KB_KR 8.0f                         /* a key's corners */
#define KB_EMO_Y 764.0f                    /* the emoji glyph's middle (over the bottom bar's close swipe at 772) */
#define KB_LET 0x6b6b6fu                   /* every key (dark) */
#define KB_FN KB_LET                       /* iOS 26: shift, delete, 123 and return the same as the letters */
#define KB_PAN 0x2b2b2eu                   /* the panel (dark) */
#define KB_BLUE 0x0091ffu                  /* a return key with a word: the accent blue (dark) */

static const char *const KR[2][3] = { { "qwertyuiop", "asdfghjkl", "zxcvbnm" }, { "1234567890", "-/:;()$&@\"", ".,?!'" } };
static const char *const KR123P[3] = { "1234567890", "-/:;()+&@\"", ".,?!'" };          /* KB_PHONE */
static const char *const KR_SYM1 = "[]{}#%^*+=";
static const char *const KR_SYM2[9] = { "_", "\\", "|", "~", "<", ">", "\xc2\xa3", "\xc2\xa5", "\xe2\x80\xa2" };   /* £ ¥ • */

enum { KK_CHAR, KK_SHIFT, KK_DEL, KK_MODE, KK_SYM, KK_EMOJI, KK_SPACE, KK_RET };
#define KM(k) (1u << (k))
#define KM_ALL 0xffu
typedef struct { float x, y, w; uint8_t kind, off; char s[4]; } kbkey;   /* off: a key this field can't type */
enum { KBE_NONE, KBE_TEXT, KBE_DEL = 4, KBE_RET = 5, KBE_DOT = 6 };
static struct {
    int shown, want, focused;          /* up; asked for by the page being drawn; kb_focus came first */
    int layer, shift, flags;           /* 0 letters, 1 123, 2 #+=; shift 0 off, 1 once, 2 caps lock */
    char ret[12];                      /* return's word ("" = dark "return") */
    int d_layer, d_shift, d_flags;     /* what the bitmap shows */
    char d_ret[12];
    int nk;
    kbkey k[40];
    /* the finger */
    int down, key;
    float t_shift, t_space, t_rep;     /* the last shift on, the last space, the next delete */
    int reps;
    /* the key sent to the page (inside its app_tap) */
    int ev;
    char evs[8];
} KB = { .key = -1, .t_space = -1e9f, .t_shift = -1e9f };
static tk_layer kbg, kbrim, kbpan, kbl, kbhl, kbpop;   /* a group: the panel's rim and face, the keys, the lit key, the balloon */

/* the layout of the current layer (and field) */
static void kb_layout(void)
{
    KB.nk = 0;
    kbkey *k = KB.k;
#define KADD(X, Y, W, KIND, S) do { kbkey *q = &k[KB.nk++]; q->x = (X); q->y = (Y); q->w = (W); q->kind = (KIND); q->off = 0; \
        int _n = 0; const char *_s = (S); while (_s && _s[_n] && _n < 3) { q->s[_n] = _s[_n]; _n++; } q->s[_n] = 0; } while (0)
    const char *const *R = KB.layer == 0 ? KR[0] : KB.layer == 1 ? (KB.flags & KB_PHONE ? KR123P : KR[1]) : 0;
    for (int r = 0; r < 2; r++) {
        int n = KB.layer == 2 ? (r ? 9 : 10) : (int)tk_strlen(R[r]);
        float x0 = 4 + (10 - n) * KB_KP / 2;                 /* row 2 (nine keys) half a key in */
        for (int i = 0; i < n; i++) {
            char one[2] = { KB.layer == 2 ? KR_SYM1[i] : R[r][i], 0 };
            KADD(x0 + KB_KP * i, KB_RY(r), KB_KW, KK_CHAR, KB.layer == 2 && r ? KR_SYM2[i] : one);
            if (KB.layer == 2 && r && i >= 6 && (KB.flags & KB_ASCII)) k[KB.nk - 1].off = 1;
        }
    }
    if (!KB.layer) {
        KADD(4, KB_RY(2), 44, KK_SHIFT, "");
        float x0 = (390 - (7 * KB_KW + 6 * 6)) / 2;           /* the seven letters centred between shift and delete */
        for (int i = 0; i < 7; i++) { char one[2] = { KR[0][2][i], 0 }; KADD(x0 + KB_KP * i, KB_RY(2), KB_KW, KK_CHAR, one); }
    } else {
        KADD(4, KB_RY(2), 44, KK_SYM, "");
        const char *p = ".,?!'";
        float w = (282 - 4 * 6) / 5.0f;                       /* five wide keys between the two function keys */
        for (int i = 0; i < 5; i++) { char one[2] = { p[i], 0 }; KADD(54 + (w + 6) * i, KB_RY(2), w, KK_CHAR, one); }
    }
    KADD(342, KB_RY(2), 44, KK_DEL, "");
    KADD(4, KB_RY(3), 88, KK_MODE, "");
    KADD(98, KB_RY(3), 194, KK_SPACE, " ");
    KADD(298, KB_RY(3), 88, KK_RET, "\n");
    KADD(4, KB_RY(3) + KB_KH + 2, 40, KK_EMOJI, "");          /* under the keys (its glyph at x 24) */
#undef KADD
}
/* the key under (x, y): rows split at the gaps' middle, a key the nearest in its row (gaps go to the closer key) */
static int kb_at(float x, float y)
{
    if (y < KB_TOP || y >= H_PT) return -1;
    int r = y < KB_RY(1) - 5 ? 0 : y < KB_RY(2) - 5 ? 1 : y < KB_RY(3) - 5 ? 2 : 3;
    if (y >= KB_RY(3) + KB_KH + 2) {                         /* the strip under the keys: the emoji glyph */
        for (int i = 0; i < KB.nk; i++) if (KB.k[i].kind == KK_EMOJI) return x < 60 ? i : -1;
        return -1;
    }
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < KB.nk; i++) {
        const kbkey *q = &KB.k[i];
        if (q->y != KB_RY(r) || q->kind == KK_EMOJI) continue;
        float d = x < q->x ? q->x - x : x > q->x + q->w ? x - q->x - q->w : 0;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* the haptic tap of a key (Settings' keyboard feedback once it has a switch for it: wire it here) */
static int kb_haptics(void) { return 1; }
/* helper B2's Settings: a key's tap follows Sounds & Haptics > Keyboard Feedback (kb_key_tap: shell_lock.c's
 * sh_haptic wrapper reads it) */
static void kb_feel(void) { if (kb_haptics()) { kb_key_tap = 1; sh_haptic(); } }

/* ---- drawing ---------------------------------------------------------------------------------------------- */
static void kb_glyph(dc *d, const char *name, float x, float y, tk_col c)
{
    const tk_bmp *ic = tk_icon(name);
    if (!ic) { tk_log(name); return; }
    float pad = tk_icon_pad(name);
    tk_blit_tinted(d->b, ic, (int)tk_floorf(PT(x) - pad - d->ox + .5f), (int)tk_floorf(PT(y) - pad - d->oy + .5f), c);
}
/* a word centred on a key (16 pt, the function keys' size) */
static void kb_word(dc *d, const kbkey *q, const char *t, tk_col c)
{
    float w = tw(TK_IT400, 16, t, 0, 0);
    d_text(d, TK_IT400, 16, q->x + (q->w - w) / 2, q->y + KB_KH / 2 + 5.5f, t, 0, 0, c, 0);
}
/* the emoji key's face: a ring, two eyes, a smile (the font has no emoji glyph) */
static void kb_smiley(dc *d, float cx, float cy, tk_col c)
{
    int X = (int)tk_floorf(PT(cx - 12) - d->ox), Y = (int)tk_floorf(PT(cy - 12) - d->oy), n = (int)PT(24) + 2;
    tk_fm m;
    if (!tk_fm_new(&m, n, n)) return;
    float ox = d->ox + X, oy = d->oy + Y;
    tk_fm_arc(&m, ox, oy, PT(cx), PT(cy), PT(9.5f), PT(1.6f), 0, 6.2831853f, 0);
    tk_fm_arc(&m, ox, oy, PT(cx), PT(cy), PT(5.2f), PT(1.6f), 2.0f, 2.2832f, 1);     /* the smile, under the middle */
    tk_fm_circle(&m, ox, oy, PT(cx - 3.3f), PT(cy - 2.8f), PT(1.35f));
    tk_fm_circle(&m, ox, oy, PT(cx + 3.3f), PT(cy - 2.8f), PT(1.35f));
    tk_paint p = tk_solid(c);
    tk_draw_fm(d->b, &m, X, Y, &p, 1);
    tk_fm_free(&m);
}
static const char *kb_label(const kbkey *q, char *buf)
{
    if (q->kind != KK_CHAR) return q->s;
    int n = 0;
    while (q->s[n]) { buf[n] = q->s[n]; n++; }
    buf[n] = 0;
    if (!KB.layer && KB.shift && buf[0] >= 'a' && buf[0] <= 'z') buf[0] = (char)(buf[0] - 32);
    return buf;
}
static void kb_draw_key(dc *d, const kbkey *q)
{
    /* the key's own px first (nothing of the last look stays round its corners) */
    int X0 = (int)tk_floorf(PT(q->x) - d->ox) - 2, Y0 = (int)tk_floorf(PT(q->y) - d->oy) - 2;
    int X1 = (int)(PT(q->x + q->w) - d->ox) + 3, Y1 = (int)(PT(q->y + (q->kind == KK_EMOJI ? 28 : KB_KH)) - d->oy) + 3;
    X0 = tk_maxi(X0, 0); Y0 = tk_maxi(Y0, 0); X1 = tk_mini(X1, d->b->w); Y1 = tk_mini(Y1, d->b->h);
    if (X1 > X0 && Y1 > Y0) tk_fill_rect_raw(d->b, X0, Y0, X1 - X0, Y1 - Y0, 0);
    float cx = q->x + q->w / 2, cy = q->y + KB_KH / 2;
    switch (q->kind) {
    case KK_CHAR: {
        char b[8];
        d_rrect(d, q->x, q->y, q->w, KB_KH, KB_KR, tk_hex(KB_LET, 1));
        const char *t = kb_label(q, b);
        float w = tw(TK_IT400, 22, t, 0, 0);
        d_text(d, TK_IT400, 22, q->x + (q->w - w) / 2, cy + 7.5f, t, 0, 0, q->off ? W(.3f) : W(1), 0);
        break;
    }
    case KK_SHIFT: {
        int on = KB.shift != 0;
        d_rrect(d, q->x, q->y, q->w, KB_KH, KB_KR, on ? W(.94f) : tk_hex(KB_FN, 1));
        tk_col g = on ? tk_hex(0x1c1c1e, 1) : W(1);
        float gy = cy - 9 - (KB.shift == 2 ? 2 : 0);
        kb_glyph(d, on ? "kb-shift-on" : "kb-shift", cx - 10, gy, g);
        if (KB.shift == 2) d_rrect(d, cx - 6, gy + 19.5f, 12, 1.8f, .9f, g);     /* caps lock: the bar under the arrow */
        break;
    }
    case KK_DEL:
        d_rrect(d, q->x, q->y, q->w, KB_KH, KB_KR, tk_hex(KB_FN, 1));
        kb_glyph(d, "kb-delete", cx - 12, cy - 9, W(1));
        break;
    case KK_MODE:
        d_rrect(d, q->x, q->y, q->w, KB_KH, KB_KR, tk_hex(KB_FN, 1));
        kb_word(d, q, KB.layer ? "ABC" : "123", W(1));
        break;
    case KK_SYM:
        d_rrect(d, q->x, q->y, q->w, KB_KH, KB_KR, tk_hex(KB_FN, 1));
        kb_word(d, q, KB.layer == 2 ? "123" : "#+=", W(1));
        break;
    case KK_EMOJI:
        kb_smiley(d, 24, KB_EMO_Y, W(.4f));                   /* on the panel, dimmed: there are no emoji to type */
        break;
    case KK_SPACE:
        d_rrect(d, q->x, q->y, q->w, KB_KH, KB_KR, tk_hex(KB_LET, 1));
        kb_word(d, q, "space", W(1));
        break;
    case KK_RET:
        d_rrect(d, q->x, q->y, q->w, KB_KH, KB_KR, KB.ret[0] ? tk_hex(KB_BLUE, 1) : tk_hex(KB_FN, 1));
        if (KB.ret[0]) kb_word(d, q, KB.ret, W(1));
        else {                                                /* ⏎ (the font has none): down, then back left to an arrowhead */
            d_seg(d, cx + 7, cy - 6, cx + 7, cy + 2, 1.7f, W(1));
            d_seg(d, cx + 7, cy + 2, cx - 7, cy + 2, 1.7f, W(1));
            d_seg(d, cx - 7, cy + 2, cx - 3, cy - 2, 1.7f, W(1));
            d_seg(d, cx - 7, cy + 2, cx - 3, cy + 6, 1.7f, W(1));
        }
        break;
    }
}
/* the keys whose kind is in mask, drawn again in place */
static void kb_paint(unsigned mask)
{
    if (!kbl.bmp.px) return;
    dc d = { &kbl.bmp, 0, kbl.bx, kbl.by };
    if (mask == KM_ALL) tk_fill_rect_raw(&kbl.bmp, 0, 0, kbl.bmp.w, kbl.bmp.h, 0);   /* another layer: keys move */
    for (int i = 0; i < KB.nk; i++) if (mask & KM(KB.k[i].kind)) kb_draw_key(&d, &KB.k[i]);
    KB.d_layer = KB.layer; KB.d_shift = KB.shift; KB.d_flags = KB.flags;
    tk_memcpy(KB.d_ret, KB.ret, sizeof KB.ret);
    tk_layer_dirty(&kbl);
}
/* the bitmap follows the state: what changed, and only that */
static void kb_sync(void)
{
    if (!KB.shown) return;
    kb_panel();                                               /* the strip: with the field's kind */
    if (KB.layer != KB.d_layer || ((KB.flags ^ KB.d_flags) & (KB_PHONE | KB_ASCII))) { kb_layout(); kb_paint(KM_ALL); return; }
    unsigned m = 0;
    if (KB.shift != KB.d_shift) m |= KB.layer ? KM(KK_SHIFT) : KM(KK_CHAR) | KM(KK_SHIFT);
    if (tk_strcmp(KB.ret, KB.d_ret)) m |= KM(KK_RET);
    if (m) kb_paint(m);
}

/* ---- the layers ---------------------------------------------------------------------------------------------- */
/* §4's 40 pt strip over the keys: the predictive bar's place (empty: there are no suggestions yet, and an honest empty
 * strip beats a made-up one), or a Linux window's accessory keys. A password or a terminal without them has none. */
#define KB_STRIP 40.0f
static struct { int n; char lab[12][8]; uint8_t lit[12]; void (*tap)(int); int dirty; } KA;   /* the accessory row */
static tk_layer kbacc;
static int kb_strip_on(void) { return KA.n > 0 || !(KB.flags & KB_ASCII); }
static float kb_top(void) { return KB_TOP - (kb_strip_on() ? KB_STRIP : 0); }
static void kb_solid(tk_layer *l, const char *name, float top, float r, uint32_t pm)
{
    tk_layer_init(l, name);
    l->solid = pm;
    l->x = 0; l->y = PT(top);
    l->solid_w = TK_W; l->solid_h = TK_H - PT(top);
    l->clip = 1;
    l->clip_r = (tk_rect){ 0, PT(top), TK_W, TK_H - PT(top) + PT(80) };   /* the bottom corners below the panel */
    l->clip_radius = PT(r);
}
static float kb_dtop = -1;            /* the panel's top as placed */
static void kb_panel(void)
{
    float t = kb_top();
    if (t == kb_dtop) return;
    kb_dtop = t;
    kb_solid(&kbrim, "kb-rim", t, KB_PR, tk_premul(W(.16f)));            /* a hairline of light along the top edge */
    kb_solid(&kbpan, "kb-panel", t + .5f, KB_PR, tk_premul(tk_hex(KB_PAN, 1)));
    relink();
}
static void kb_link(void)
{
    tk_layer *k[] = { &kbrim, &kbpan, &kbacc, &kbl, &kbhl, &kbpop };
    kbg.child = 0;
    for (int i = 0; i < 6; i++) tk_layer_add(&kbg, k[i]);
}
static void kb_hide_pop(void)
{
    if (!kbpop.hidden) { kbpop.hidden = 1; kbpop.gen++; }
    if (!kbhl.hidden) { kbhl.hidden = 1; kbhl.gen++; }
}
/* the slide (§16: 0.38 s, damping 1.0) and the layers that ride on it (a field over the keyboard) */
static tk_spring sp_kb;
static float kb_off;                  /* pt below the rest place */
static int kb_leaving;
static tk_layer *KB_RIDE[4];
static int kb_nride;
static void kb_place(void)
{
    kbg.y = PT(kb_off);
    for (int i = 0; i < kb_nride; i++) KB_RIDE[i]->y = KB_RIDE[i]->by + PT(kb_off);
}
/* a layer that moves with the keyboard while it slides (placed at once for where it is now) */
static void kb_ride(tk_layer *l)
{
    int i = 0;
    while (i < kb_nride && KB_RIDE[i] != l) i++;
    if (i == kb_nride && kb_nride < 4) KB_RIDE[kb_nride++] = l;
    l->y = l->by + PT(kb_off);
}
static void kb_slide(float from, float to)
{
    tk_spring_start(&sp_kb, from, to, 0, TK_SPR_SHEET);
    sp_kb.k = 273.4f;                  /* (2 pi / 0.38)^2 */
    sp_kb.c = 33.07f;                  /* 4 pi x 1.0 / 0.38 */
    kb_off = from;
    kb_place();
}
/* the app is open and still (a slide); else the keyboard comes and goes with the page */
static int kb_settled(void) { return AP.on && !AP.closing && !sp_app.live && !ap_pending && AP.R.w >= 389; }
static void kb_free(void)
{
    relayer(&kbl); relayer(&kbpop); relayer(&kbacc);
    kbg.hidden = 1;
    kb_leaving = 0;
    sp_kb.live = 0;
    kb_off = 0;
    kb_nride = 0;
    kbg.y = 0;
    relink();
}
/* up or down (the page's build calls it through kb_commit; a Linux app's text focus directly): a slide when the app
 * is open and still, else at once */
static void kb_set(int on)
{
    if (on == KB.shown) return;
    float hgt = H_PT - kb_top() + 6;
    if (!on) {
        KB.shown = KB.focused = KB.down = 0;
        KB.key = -1;
        kb_hide_pop();
        kb_nride = 0;
        if (kb_settled() && !kbg.hidden) { kb_leaving = 1; kb_slide(kb_off, hgt); return; }
        kb_free();
        return;
    }
    float from = kb_leaving ? kb_off : hgt;
    kb_leaving = 0;
    relayer(&kbl);
    tk_layer_init(&kbg, "keyboard");
    kb_dtop = -1;
    kb_panel();
    tk_layer_init(&kbhl, "kb-lit"); kbhl.hidden = 1;
    tk_layer_init(&kbpop, "kb-balloon"); kbpop.hidden = 1;
    if (!kbacc.bmp.px) { tk_layer_init(&kbacc, "kb-accessory"); kbacc.hidden = 1; }
    KA.dirty = KA.n > 0;
    kb_layout();
    mk(&kbl, "kb-keys", 0, KB_RY(0) - 1, 390, KB_EMO_Y + 13 - (KB_RY(0) - 1), 0, 0);
    KB.shown = 1;
    kb_paint(KM_ALL);
    kbg.alpha = appc.alpha;
    kbg.hidden = 0;
    relink();
    if (kb_settled()) kb_slide(from, 0);
    else { sp_kb.live = 0; kb_off = 0; kb_place(); }
}
static void kb_commit(void) { kb_set(KB.want); }
static void kb_close(void) { KB.want = 0; kb_set(0); kb_free(); KB.focused = 0; KB.ev = 0; }

/* ---- the page's side ----------------------------------------------------------------------------------------- */
/* a field takes the focus: its layer, its return word, a capital if its text is at a sentence's start */
static void kb_ctx(const char *t);
static void kb_focus(int flags, const char *ret, const char *text)
{
    KB.flags = se_autocap() ? flags : flags & ~KB_AUTOCAP;   /* helper B2: General > Keyboard > Auto-Capitalization */
    int n = 0;
    while (ret && ret[n] && n < 11) { KB.ret[n] = ret[n]; n++; }
    KB.ret[n] = 0;
    KB.layer = flags & KB_DIGITS ? 1 : 0;
    KB.shift = 0;
    KB.focused = 1;
    KB.t_space = -1e9f;
    kb_ctx(text ? text : "");
    kb_sync();
}
/* the page shows the keyboard (it is drawn on its own layer: nothing goes into d) */
static void kb_draw_r(dc *d, const char *const *R, int shift, int num)
{
    (void)d; (void)R;
    KB.want = 1;
    if (!KB.shown && !KB.focused) { KB.shift = !!shift; KB.layer = num ? 1 : 0; KB.flags = 0; KB.ret[0] = 0; }
}
static void kb_draw(dc *d, int shift, int num) { kb_draw_r(d, 0, shift, num); }
/* a capital next? (the text so far: empty, after a new line, or after . ! ? and a space) */
static void kb_ctx(const char *t)
{
    if (!(KB.flags & KB_AUTOCAP) || KB.shift == 2 || KB.layer) return;
    int n = (int)tk_strlen(t), i = n;
    while (i > 0 && t[i - 1] == ' ') i--;
    int cap = !i || t[n - 1] == '\n' || (i < n && (t[i - 1] == '.' || t[i - 1] == '!' || t[i - 1] == '?'));
    if (cap != KB.shift) { KB.shift = cap; kb_sync(); }   /* the letters and shift drawn again (only those) */
}
/* the key sent to the page now: 0 none, KBE_TEXT (*s the text), KBE_DEL, KBE_RET, KBE_DOT (". " for the last space) */
static int kb_take(const char **s)
{
    if (s) *s = KB.evs;
    return KB.ev;
}
static int kb_utf8_back(const char *t, int n)
{
    if (n <= 0) return 0;
    n--;
    while (n > 0 && ((unsigned char)t[n] & 0xc0) == 0x80) n--;
    return n;
}
/* the key applied to buf (a C string of at most max bytes): 0 nothing, 1 changed, 5 return (not added: the page's) */
static int kb_edit(char *buf, int max)
{
    if (KB.ev && em_take_sel(buf)) {                      /* the edit menu's Select All: a key types over it all */
        buf[0] = 0;
        if (KB.ev == KBE_DEL) { kb_ctx(buf); return 1; }
    }
    int n = (int)tk_strlen(buf), r = 0;
    switch (KB.ev) {
    case KBE_TEXT: {
        int m = (int)tk_strlen(KB.evs);
        if (n + m > max) return 0;
        tk_memcpy(buf + n, KB.evs, (size_t)m + 1);
        r = 1;
        break;
    }
    case KBE_DOT:
        if (n >= 2 && buf[n - 1] == ' ' && buf[n - 2] != ' ' && buf[n - 2] != '.' && buf[n - 2] != '\n' && n + 1 <= max) {
            buf[n - 1] = '.'; buf[n] = ' '; buf[n + 1] = 0;
        } else if (n + 1 <= max) { buf[n] = ' '; buf[n + 1] = 0; }
        else return 0;
        r = 1;
        break;
    case KBE_DEL:
        if (!n) return 0;
        buf[kb_utf8_back(buf, n)] = 0;
        r = 1;
        break;
    case KBE_RET:
        if (!(KB.flags & KB_NL)) return 5;
        if (n + 1 > max) return 0;
        buf[n] = '\n'; buf[n + 1] = 0;
        r = 1;
        break;
    default: return 0;
    }
    kb_ctx(buf);
    return r;
}
/* the old call: 1 a character (*c), 4 delete, 5 return (*c '\n'), 0 nothing (shift and 123 are the keyboard's) */
static int kb_hit_r(float x, float y, const char *const *R, int shift, int num, char *c)
{
    (void)x; (void)y; (void)R; (void)shift; (void)num;
    switch (KB.ev) {
    case KBE_TEXT: if ((unsigned char)KB.evs[0] < 0x80 && !KB.evs[1]) { *c = KB.evs[0]; return 1; } return 0;
    case KBE_DOT: *c = ' '; return 1;
    case KBE_DEL: return 4;
    case KBE_RET: *c = '\n'; return 5;
    }
    return 0;
}
static int kb_hit(float x, float y, int shift, int num, char *c) { return kb_hit_r(x, y, 0, shift, num, c); }
/* a key's rect under (x, y) (the old press light's; the keyboard lights its own keys now) */
__attribute__((unused)) static int kb_rect(float x, float y, const char *const *R, int r2x, tk_rect *o)
{
    (void)R; (void)r2x;
    int i = KB.shown ? kb_at(x, y) : -1;
    if (i < 0) return 0;
    *o = (tk_rect){ KB.k[i].x, KB.k[i].y, KB.k[i].w, KB_KH };
    return 1;
}
/* a key drawn into a page (the old look's helper: nothing calls it now) */
__attribute__((unused)) static void kb_key(dc *d, float x, float y, float w, const char *t, int special)
{
    kbkey q = { x, y, w, KK_CHAR, 0, { 0 } };
    d_rrect(d, x, y, w, KB_KH, KB_KR, tk_hex(special ? KB_FN : KB_LET, 1));
    if (special) { kb_word(d, &q, t, W(1)); return; }
    float tw0 = tw(TK_IT400, 23, t, 0, 0);
    d_text(d, TK_IT400, 23, x + (w - tw0) / 2, y + KB_KH / 2 + 7.5f, t, 0, 0, W(1), 0);
}

/* ---- the finger ----------------------------------------------------------------------------------------------- */
/* the balloon over a pressed letter: the key's grey, a 1.5x head with the letter at 32 pt, a neck down over the key
 * (the two shapes' smooth union), a soft shadow; one small layer drawn per press */
#define POP_W 112.0f
#define POP_H 124.0f
static float kb_smin(float a, float b, float k)
{
    float h = tk_clampf(.5f + .5f * (b - a) / k, 0, 1);
    return b + (a - b) * h - k * h * (1 - h);
}
static void kb_balloon(int i)
{
    const kbkey *q = &KB.k[i];
    float hw = q->w * 1.4f, hx = q->x + q->w / 2 - hw / 2;     /* §4: 44 x 96 with the key, letter 32 */
    hx = tk_clampf(hx, 1, 389 - hw);                         /* the edge keys' heads lean in, as iOS's */
    float hy = q->y + KB_KH - 96, hh = 50;
    float bx0 = tk_minf(hx, q->x) - 12, by0 = hy - 10;        /* the layer: the shape and its shadow */
    if (!kbpop.bmp.px) {
        tk_layer_init(&kbpop, "kb-balloon");
        if (!tk_bmp_new(&kbpop.bmp, (int)PT(POP_W) + 2, (int)PT(POP_H) + 2)) return;
        relink();
    }
    tk_bmp *b = &kbpop.bmp;
    tk_fill_rect_raw(b, 0, 0, b->w, b->h, 0);
    float ox = tk_floorf(PT(bx0)), oy = tk_floorf(PT(by0));
    tk_fm m, s;
    if (!tk_fm_new(&m, b->w, b->h)) return;
    float k = PT(6), hr = PT(KB_KR), kr = PT(KB_KR);
    for (int j = 0; j < m.h; j++)
        for (int u = 0; u < m.w; u++) {
            float px = ox + u + .5f, py = oy + j + .5f;
            float d1 = tk_sd_rrect(px, py, PT(hx), PT(hy), PT(hw), PT(hh), hr);
            float d2 = tk_sd_rrect(px, py, PT(q->x), PT(hy + hh - 12), PT(q->w), PT(q->y + KB_KH - (hy + hh - 12)), kr);
            m.v[j * m.w + u] = tk_clampf(.5f - kb_smin(d1, d2, k), 0, 1);
        }
    if (tk_fm_new(&s, b->w, b->h)) {
        tk_fm_shift(&s, &m, 0, PT(1.5f));
        tk_fm_blur(&s, PT(3));
        tk_paint sp = tk_solid(TK_RGBA(0, 0, 0, .45f));
        tk_draw_fm(b, &s, 0, 0, &sp, 1);
        tk_fm_free(&s);
    }
    tk_paint p = tk_solid(tk_hex(KB_LET, 1));
    tk_draw_fm(b, &m, 0, 0, &p, 1);
    tk_fm_free(&m);
    char lb[8];
    const char *t = kb_label(q, lb);
    dc d = { b, 0, ox, oy };
    float w = tw(TK_IT400, 32, t, 0, 0);
    d_text(&d, TK_IT400, 32, hx + (hw - w) / 2, hy + hh / 2 + 11.5f, t, 0, 0, q->off ? W(.3f) : W(1), 0);
    kbpop.x = kbpop.bx = ox;
    kbpop.y = kbpop.by = oy;
    kbpop.hidden = 0;
    tk_layer_dirty(&kbpop);
}
/* what a finger on key i shows: a letter its balloon, the rest the key lit */
static void kb_light(int i)
{
    kb_hide_pop();
    if (i < 0) return;
    const kbkey *q = &KB.k[i];
    if (q->kind == KK_CHAR) { kb_balloon(i); return; }
    if (q->kind == KK_SHIFT || q->kind == KK_EMOJI) return;  /* shift shows its new state at once */
    tk_col c = q->kind == KK_SPACE ? TK_RGBA(0, 0, 0, .28f) : W(.2f);   /* iOS: space darkens, the rest light up */
    kbhl.solid = tk_premul(c);
    kbhl.x = PT(q->x); kbhl.y = PT(q->y);
    kbhl.solid_w = PT(q->w); kbhl.solid_h = PT(KB_KH);
    kbhl.clip = 1;
    kbhl.clip_r = (tk_rect){ PT(q->x), PT(q->y), PT(q->w), PT(KB_KH) };
    kbhl.clip_radius = PT(KB_KR);
    kbhl.alpha = 1;
    kbhl.hidden = 0;
    kbhl.gen++;
}
/* a key to the page: a tap in its middle, the key in KB.ev while the page's app_tap runs */
static void kb_emit(int ev, const char *s, int i)
{
    if (i < 0 || i >= KB.nk) return;
    float cx = KB.k[i].x + KB.k[i].w / 2, cy = KB.k[i].y + KB_KH / 2;
    KB.ev = ev;
    int n = 0;
    while (s && s[n] && n < 7) { KB.evs[n] = s[n]; n++; }
    KB.evs[n] = 0;
    app_tap(cx, cy);
    KB.ev = 0;
    KB.evs[0] = 0;
    kb_sync();
}
/* a key on touch down: shift, 123 / #+= and delete act now (as iOS); the rest at the lift */
static void kb_press(int i, float t)
{
    const kbkey *q = &KB.k[i];
    switch (q->kind) {
    case KK_SHIFT:
        if (KB.shift == 0) { KB.shift = 1; KB.t_shift = t; }
        else if (KB.shift == 1 && t - KB.t_shift < 350) KB.shift = 2;    /* a double tap: caps lock */
        else KB.shift = 0;
        kb_sync();
        break;
    case KK_MODE:
        KB.layer = KB.layer ? 0 : 1;
        KB.shift = 0;
        kb_sync();
        break;
    case KK_SYM:
        KB.layer = KB.layer == 2 ? 1 : 2;
        kb_sync();
        break;
    case KK_DEL:
        KB.reps = 0;
        KB.t_rep = t + 450;
        kb_emit(KBE_DEL, 0, i);
        break;
    }
}
static void kb_release(int i, float t)
{
    const kbkey *q = &KB.k[i];
    switch (q->kind) {
    case KK_CHAR: {
        if (q->off) { toast("This field takes ASCII only"); return; }
        char lb[8];
        const char *s = kb_label(q, lb);
        if (KB.shift == 1) KB.shift = 0;                     /* shift once: this letter only */
        int back = KB.layer && !(KB.flags & KB_DIGITS) && s[0] == '\'' && !s[1];   /* an apostrophe: back to letters */
        kb_emit(KBE_TEXT, s, i);
        if (back && KB.shown) { KB.layer = 0; kb_sync(); }
        KB.t_space = -1e9f;
        break;
    }
    case KK_SPACE: {
        int dot = (KB.flags & KB_AUTOCAP) && t - KB.t_space < 900;
        KB.t_space = dot ? -1e9f : t;
        if (KB.layer && !(KB.flags & KB_DIGITS)) KB.layer = 0;   /* iOS: a space after a number or a sign: letters */
        kb_emit(dot ? KBE_DOT : KBE_TEXT, " ", i);
        break;
    }
    case KK_RET:
        KB.t_space = -1e9f;
        kb_emit(KBE_RET, "\n", i);
        break;
    case KK_EMOJI:
        toast("No emoji yet: the phone's font has none");
        break;
    }
}
/* ---- the accessory row (a Linux window's Esc, Tab, Ctrl, Alt, arrows) in the strip over the keys ------------------ */
#define KA_Y (KB_TOP - KB_STRIP + 4)
#define KA_H 32.0f
static void ka_rect(int i, float *x, float *w)
{
    *w = (382 - (KA.n - 1) * 6.0f) / KA.n;
    *x = 4 + (*w + 6) * i;
}
/* an arrow key: ← → ↑ ↓ or the words (they repeat while held, and are drawn as chevrons: the font has no ↑ ↓) */
static int ka_arrow(const char *s)
{
    static const char *const A[8] = { "\xe2\x86\x90", "\xe2\x86\x92", "\xe2\x86\x91", "\xe2\x86\x93", "Left", "Right", "Up", "Down" };
    for (int i = 0; i < 8; i++) if (!tk_strcmp(s, A[i])) return 1 + (i & 3);    /* 1 left, 2 right, 3 up, 4 down */
    return 0;
}
static void ka_build(void)
{
    KA.dirty = 0;
    relayer(&kbacc);
    if (!KA.n || !KB.shown) { kbacc.hidden = 1; return; }
    dc d = mk(&kbacc, "kb-accessory", 0, KA_Y, 390, KA_H, 0, 0);
    for (int i = 0; i < KA.n; i++) {
        float x, w;
        ka_rect(i, &x, &w);
        d_rrect(&d, x, KA_Y, w, KA_H, KB_KR, KA.lit[i] ? W(.94f) : tk_hex(KB_FN, 1));
        tk_col g = KA.lit[i] ? tk_hex(0x1c1c1e, 1) : W(1);
        float cx = x + w / 2, cy = KA_Y + KA_H / 2;
        int ar = ka_arrow(KA.lab[i]);
        if (ar == 1 || ar == 2) {
            float s = ar == 1 ? 1 : -1;
            d_seg(&d, cx + 3 * s, cy - 6, cx - 3 * s, cy, 1.8f, g);
            d_seg(&d, cx - 3 * s, cy, cx + 3 * s, cy + 6, 1.8f, g);
        } else if (ar) {
            float s = ar == 3 ? 1 : -1;
            d_seg(&d, cx - 6, cy + 3 * s, cx, cy - 3 * s, 1.8f, g);
            d_seg(&d, cx, cy - 3 * s, cx + 6, cy + 3 * s, 1.8f, g);
        } else {
            float tw0 = tw(TK_IT400, 15, KA.lab[i], 0, 0);
            d_text(&d, TK_IT400, 15, cx - tw0 / 2, cy + 5.2f, KA.lab[i], 0, 0, g, 0);
        }
    }
    kbacc.hidden = 0;
    relink();
}
/* the keys over the keyboard for a Linux window (keys[i] its labels, tap(i) a key); kb_accessory(0, 0, 0) clears it.
 * The row takes the strip over the keys: the keyboard's top is kb_top() (500 pt with it). Drawn at idle. */
__attribute__((unused)) static void kb_accessory(const char *const *keys, int n, void (*tap)(int i))
{
    if (n > 12) n = 12;
    if (!keys || !tap) n = 0;
    KA.n = n;
    KA.tap = tap;
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (keys[i][k] && k < 7) { KA.lab[i][k] = keys[i][k]; k++; }
        KA.lab[i][k] = 0;
        KA.lit[i] = 0;
    }
    KA.dirty = 1;
    if (!n) { relayer(&kbacc); kbacc.hidden = 1; KA.dirty = 0; }
    if (KB.shown) kb_panel();
}
/* a key of the row shown held (Ctrl or Alt latched): drawn again now (a small layer) */
__attribute__((unused)) static void kb_accessory_lit(int i, int on)
{
    if (i < 0 || i >= KA.n || KA.lit[i] == !!on) return;
    KA.lit[i] = (uint8_t)!!on;
    if (KB.shown && kbacc.bmp.px) ka_build(); else KA.dirty = 1;
}
static int ka_at(float x, float y)
{
    if (!KA.n || y < KB_TOP - KB_STRIP || y >= KB_TOP) return -1;
    for (int i = 0; i < KA.n; i++) {
        float kx, w;
        ka_rect(i, &kx, &w);
        if (x >= kx - 3 && x < kx + w + 3) return i;
    }
    return -1;
}
static void ka_light(int i)
{
    float x, w;
    ka_rect(i, &x, &w);
    kbhl.solid = tk_premul(W(.2f));
    kbhl.x = PT(x); kbhl.y = PT(KA_Y);
    kbhl.solid_w = PT(w); kbhl.solid_h = PT(KA_H);
    kbhl.clip = 1;
    kbhl.clip_r = (tk_rect){ PT(x), PT(KA_Y), PT(w), PT(KA_H) };
    kbhl.clip_radius = PT(KB_KR);
    kbhl.alpha = 1;
    kbhl.hidden = 0;
    kbhl.gen++;
}

/* sh_touch's G_KB: the keyboard's own touches (phase 0 down, 1 move, 2 up). KB.key: a key, 100 + i the accessory
 * row's key i, -1 none */
static void kb_touch(int phase, float x, float y, float t)
{
    if (!KB.shown) { KB.down = 0; return; }
    if (phase == 0) {
        int a = ka_at(x, y), i = a >= 0 ? 100 + a : kb_at(x, y);
        KB.down = 1;
        KB.key = i;
        if (i < 0) return;
        kb_feel();
        if (a >= 0) {                                       /* an arrow at once and again while held; the rest at the lift */
            ka_light(a);
            if (ka_arrow(KA.lab[a])) { KB.reps = 0; KB.t_rep = t + 450; if (KA.tap) KA.tap(a); }
            return;
        }
        kb_light(i);
        int kind = KB.k[i].kind;
        kb_press(i, t);
        if (kind == KK_SHIFT || kind == KK_MODE || kind == KK_SYM) KB.key = -1;   /* done on down (the layout may be new) */
        return;
    }
    if (phase == 1) {
        if (KB.key < 0) return;
        if (KB.key >= 100) { if (ka_at(x, y) != KB.key - 100) { KB.key = -1; kb_hide_pop(); } return; }
        int kind = KB.k[KB.key].kind;
        /* a letter, space or return follows the finger to the key it ends on (off the keyboard: none) */
        if (kind == KK_CHAR || kind == KK_SPACE || kind == KK_RET || kind == KK_EMOJI) {
            int i = y < kb_top() - 24 ? -1 : kb_at(x, y);
            if (i >= 0 && KB.k[i].kind != KK_CHAR && KB.k[i].kind != KK_SPACE && KB.k[i].kind != KK_RET) i = KB.key;
            if (i != KB.key) { KB.key = i; kb_light(i); }
        } else if (kind == KK_DEL && kb_at(x, y) != KB.key) { KB.key = -1; kb_hide_pop(); }   /* slid off: no more repeats */
        return;
    }
    int i = KB.key;
    KB.down = 0;
    KB.key = -1;
    kb_hide_pop();
    if (i >= 100) { if (i - 100 < KA.n && !ka_arrow(KA.lab[i - 100]) && KA.tap) KA.tap(i - 100); return; }
    if (i >= 0 && i < KB.nk) kb_release(i, t);
}
static int kb_repeating(void)
{
    if (!KB.shown || !KB.down || KB.key < 0) return 0;
    if (KB.key >= 100) return KB.key - 100 < KA.n && ka_arrow(KA.lab[KB.key - 100]);
    return KB.k[KB.key].kind == KK_DEL;
}
/* sh_tick: the slide; delete (or an arrow) held repeats, sooner and sooner (450 ms, then 100 ms down to 35 ms) */
static int kb_tick(float dt, float t)
{
    int mv = 0;
    if (sp_kb.live) {
        tk_spring_step(&sp_kb, dt);
        kb_off = sp_kb.x;
        kb_place();
        if (!sp_kb.live && kb_leaving) kb_free();
        mv = 1;
    }
    if (!kb_repeating() || t < KB.t_rep) return mv;
    KB.reps++;
    KB.t_rep = t + tk_maxf(35, 100 - 5.0f * KB.reps);
    kb_feel();
    if (KB.key >= 100) { if (KA.tap) KA.tap(KB.key - 100); }
    else kb_emit(KBE_DEL, 0, KB.key);
    return 1;
}
static int kb_moving(void) { return sp_kb.live || kb_repeating(); }
/* sh_idle: the accessory row, drawn while nothing moves */
static void kb_idle(void) { if (KA.dirty && KB.shown && !sp_kb.live) ka_build(); }
/* sh_touch's down: is this the keyboard's (above the bottom bar's close swipe) */
static int kb_owns(float x, float y)
{
    (void)x;
    return KB.shown && !kbg.hidden && !sp_kb.live && y >= kb_top() && y < H_PT - 30;
}
/* a strip of a page with the keyboard: cut to what it holds, or gone if empty (the keys are not in the page) */
static void kb_slim(tk_layer *l)
{
    const tk_bmp *b = &l->bmp;
    if (!b->px) return;
    size_t n = (size_t)b->stride * b->h, i = 0;
    while (i < n && !b->px[i]) i++;
    if (i == n) { relayer(l); l->hidden = 1; return; }
    trim(l);
}

/* ---- the edit menu: a long press on a text field (Notes, Messages' message, Search) shows iOS's small dark pill with
 * Copy / Paste / Select All (with all selected: Cut / Copy / Paste) over the press. The fields have no caret moves or
 * part selections yet: Copy and Cut take the whole text, Paste adds at the end (or replaces all that is selected).
 * The clipboard is the platform's (sh_clip_set / sh_clip_get, shared with the Linux apps). ------------------------- */
typedef struct { char *buf; int max; tk_rect sel; void (*redraw)(void); } emt;   /* a field: its text, its text's box */
#define EM_H 38.0f
static struct { int on, sel, n; emt E; char item[3][12]; float x0[4], px, py, pw, ax, ay; } EM;
static tk_layer emp, emsel;                  /* the pill; the selection's tint over the text */
static void em_tint(int on)
{
    EM.sel = on;
    if (!on) { if (!emsel.hidden) { emsel.hidden = 1; emsel.gen++; } return; }
    tk_layer_init(&emsel, "edit-selection");
    emsel.solid = tk_premul(tk_hex(KB_BLUE, .32f));
    emsel.x = PT(EM.E.sel.x); emsel.y = PT(EM.E.sel.y);
    emsel.solid_w = PT(EM.E.sel.w); emsel.solid_h = PT(EM.E.sel.h);
    emsel.clip = 1;
    emsel.clip_r = (tk_rect){ PT(EM.E.sel.x), PT(EM.E.sel.y), PT(EM.E.sel.w), PT(EM.E.sel.h) };
    emsel.clip_radius = PT(3);
    relink();
}
static void em_close(int keep_sel)
{
    if (EM.on) { relayer(&emp); emp.hidden = 1; }
    EM.on = 0;
    if (!keep_sel) em_tint(0);
}
/* the pill over (x, y): its items for what the field holds now */
static void em_show(float x, float y)
{
    const char *clip = sh_clip_get();
    int text = EM.E.buf[0] != 0, paste = clip && clip[0];
    EM.n = 0;
#define EMI(S) do { int _k = 0; while ((S)[_k]) { EM.item[EM.n][_k] = (S)[_k]; _k++; } EM.item[EM.n++][_k] = 0; } while (0)
    if (EM.sel) { EMI("Cut"); EMI("Copy"); if (paste) EMI("Paste"); }
    else { if (text) EMI("Copy"); if (paste) EMI("Paste"); if (text) EMI("Select All"); }
#undef EMI
    if (!EM.n) { em_close(0); return; }
    float w = 0;
    for (int i = 0; i < EM.n; i++) { EM.x0[i] = w; w += tw(TK_IT400, 15, EM.item[i], 0, 0) + 28; }
    EM.x0[EM.n] = w;
    EM.pw = w;
    EM.px = tk_clampf(x - w / 2, 8, 382 - w);
    EM.ax = x; EM.ay = y;
    EM.py = y - EM_H - 22;                                  /* over the finger; under it near the top (the nav row) */
    if (EM.py < SAFE_TOP + 50) EM.py = y + 26;
    relayer(&emp);
    dc d = mk(&emp, "edit-menu", EM.px, EM.py, w, EM_H, 12, 0);
    tk_box_shadow(d.b, PT(EM.px) - d.ox, PT(EM.py) - d.oy, PT(w), PT(EM_H), PT(EM_H / 2), 0, PT(3), PT(10), 0, TK_RGBA(0, 0, 0, .35f), 0);
    d_rrect(&d, EM.px, EM.py, w, EM_H, EM_H / 2, tk_hex(0x2c2c2e, 1));      /* systemGray5 (dark) */
    for (int i = 0; i < EM.n; i++) {
        d_text(&d, TK_IT400, 15, EM.px + EM.x0[i] + 14, EM.py + EM_H / 2 + 5.2f, EM.item[i], 0, 0, W(1), 0);
        if (i) d_rrect(&d, EM.px + EM.x0[i] - .25f, EM.py + 9, .5f, EM_H - 18, 0, W(.25f));
    }
    EM.on = 1;
    emp.hidden = 0;
    relink();
}
/* a long press on field E at (x, y) */
static void em_open(const emt *E, float x, float y)
{
    EM.E = *E;
    em_tint(0);
    sh_haptic();                                            /* §17: the long press's commit (medium on iOS) */
    em_show(x, y);
}
static int em_take_sel(const char *buf)
{
    if (!EM.sel || buf != EM.E.buf) return 0;
    em_close(0);
    return 1;
}
static void em_paste(void)
{
    const char *c = sh_clip_get();
    char *b = EM.E.buf;
    if (EM.sel) b[0] = 0;
    int n = (int)tk_strlen(b), m = 0;
    while (c && c[m] && n + m < EM.E.max) m++;
    while (m > 0 && c[m] && ((unsigned char)c[m] & 0xc0) == 0x80) m--;    /* whole characters */
    if (m) tk_memcpy(b + n, c, (size_t)m);
    b[n + m] = 0;
}
/* a touch down while the pill is up: an item (1: taken), or anywhere else closes it (0: the touch goes on) */
static int em_down(float x, float y)
{
    if (!EM.on && !EM.sel) return 0;
    if (EM.on && y >= EM.py - 6 && y < EM.py + EM_H + 6 && x >= EM.px && x < EM.px + EM.pw) {
        int i = 0;
        while (i + 1 < EM.n && x - EM.px >= EM.x0[i + 1]) i++;
        const char *it = EM.item[i];
        sh_haptic();
        if (!tk_strcmp(it, "Select All")) { em_tint(1); em_show(EM.ax, EM.ay); return 1; }
        if (!tk_strcmp(it, "Copy") || !tk_strcmp(it, "Cut")) sh_clip_set(EM.E.buf);
        if (!tk_strcmp(it, "Cut")) EM.E.buf[0] = 0;
        if (!tk_strcmp(it, "Paste")) em_paste();
        em_close(0);
        kb_ctx(EM.E.buf);
        kb_sync();
        if (EM.E.redraw && (it[0] == 'C' && it[1] == 'u' ? 1 : it[0] == 'P')) EM.E.redraw();
        return 1;
    }
    if (EM.sel && kb_owns(x, y)) { em_close(1); return 0; }    /* a key: it types over the selection */
    em_close(0);
    return 0;
}
