/* SPDX-License-Identifier: MIT
 * Emergency mode's tools for Starbite OS Next (survival-next), included by shell_lock.c after shell_cell.inc (it uses
 * the shell's drawing helpers, layers and app state). The screens, after survival-next-integration.md:
 *   - SOS: a hold (1 s, the concept's ring) starts the kernel's SOS (its 3 s countdown, then Morse on the torch and
 *     the motor); the screen is then the beacon (a full-screen solid layer in sos_frame's colour, polled 20 times a
 *     second without 60 Hz frames) with a Stop; the options: Torch / Vibration Morse, the beacon's colours;
 *   - Medical Card: the kernel's card (the Info app's record), shown without unlocking; edited in Settings > Medical
 *     Card with the shell's keyboard;
 *   - Last 2 % (it replaces Power Saving): the mode and "automatically at 3 %";
 *   - USB Power: the guidance, a 1.5 s red hold with the ring, then the countdown, the estimated mA, the port's
 *     voltage, the battery against its limit, a big Stop; the cooldown; why it stopped or was refused (polled at
 *     2 Hz while on or cooling only);
 *   - Sun: today's sunrise and sunset, daylight left, the typed location;
 *   - Guide: chapters, pages, the text (a "! " line red), Previous / Next;
 *   - Compass / Level (helper J): the sensor hub on demand: the dial, the level, the steps (started at the page's
 *     open, kept alive while it shows, stopped at its close);
 *   - Sun (helper J): Android's last GPS fix, its age, "Use" (fills the location editor's fields).
 * Nothing here starts by itself: SOS and USB power need the user's own hold, every page is drawn at its open (over
 * frames, as the other own pages) and a tap redraws only small layers or asks for the page over the next frames
 * (sv_rebuild: no full page inside a tap's handler). The lock's Emergency list opens the screens over the lock (the
 * medical card needs no unlock) and scrolls; its four new cards are drawn one per idle turn.
 * survival-next (E's bands): the pages in the shell's iOS bands (shell_lock.c): the large title in its row (d_h1, LARGE_BASE), content from
 * CONTENT_TOP, groups at GROUP_X / GROUP_W with their text at ROW_TX; the pushed pages (a guide chapter and page,
 * Settings > Medical Card) with the glass back circle and an inline title (d_back, d_navtitle); the editors' Cancel /
 * Done in the nav row (NAV_BASE); rows and keys light under the finger (sv_press). */

static void set_lock(float y);
static int kb_rect(float x, float y, const char *const *R, int r2x, tk_rect *o);   /* (sv_press: a key lights) */

#define SV_GROUND 0x1a0c0eU             /* the tools' ground: the Emergency wallpaper's darkest red */
#define SV_GROUP 0x2c1719U              /* grouped rows on it */
#define SV_RED 0xff453aU
#define SV_WARN 0xff6961U               /* a warning line */
#define SV_TINT 0xff8a7aU               /* the concept's Emergency ring */
static tk_col SVC(uint32_t c) { return tk_hex(c, 1); }

/* the layers: the page's live parts (a group appended to the page's strips) and SOS's beacon (over everything but
 * the toast) */
enum { SVL_A, SVL_B, SVL_SEG, SVL_CTL, SVL_TXT, SVL_RING, SVL_FLD, SVL_DIAL, SVL_N };   /* helper J: the compass's dial */
static tk_layer svgrp, svl[SVL_N], sosl, sosbg, sostx, soshl;   /* soshl (8c): the beacon's Stop pressed */
static struct {
    int from_lock;                      /* opened from the lock's Emergency list: it closes back into that card */
    arect lock_r;
    int lw_stale, l2_seen;              /* the lock's widgets show Last 2 % again at the next idle */
    int rb;                             /* a page asked for over the next frames (sv_rebuild) */
    int hold, hold_done;                /* 1 SOS, 2 USB: the finger is on its button */
    float hold_t0, hold_x, hold_y;
    int sos_on, sos_secs, sos_down;
    uint32_t sos_col;
    sh_usb_t usb;
    float usb_next;
    int usb_secs, usb_ma, usb_c, usb_vl, usb_vh;
    const char *usb_why;
    unsigned usb_ev;                    /* acb417e: sh_usb's events at the last draw */
    int gch, gpg;                       /* the guide: chapter (-1 the list), page (-1 the chapter's list) */
    int cf;                             /* Settings > Medical Card: the field edited (-1 the list) */
    sh_card_t card;
    char ebuf[SH_CARD_FL + 1];
    int shift, num;
    int sun_edit, sun_f;                /* the Sun's location editor, its field (0 latitude, 1 longitude) */
    char sun_v[2][12];
    int fix_asked, fix_have;            /* helper J: Android's last GPS fix (sh_gnss: the card's, cached by the kernel) */
    float fix_t;
    sh_gnss_t fix;
} SV = { .gch = -1, .gpg = -1, .cf = -1 };

static int sv_key(const char *k)
{
    return !tk_strcmp(k, "sos") || !tk_strcmp(k, "medical") || !tk_strcmp(k, "power") || !tk_strcmp(k, "usb") ||
           !tk_strcmp(k, "sun") || !tk_strcmp(k, "guide") || !tk_strcmp(k, "compass");
}
static int sv_own(const char *k) { return sv_key(k); }
static uint32_t sv_ground(int a) { return sv_key(APPS[a].k) ? SV_GROUND : sbc_rgb(SBC_GROUND); }
static int sv_app(const char *k)
{
    for (int a = 0; a < NAPPS; a++) if (!tk_strcmp(APPS[a].k, k)) return a;
    return -1;
}
/* the page again over the next frames (two strips a frame, then the live layers): a tap never draws a whole page */
static void sv_rebuild(void)
{
    if (!AP.on || AP.closing) return;
    ap_pending = 1;
    SV.rb = 1;
}

/* ---- text ---------------------------------------------------------------------------------------------------- */
/* word-wrapped text from top, lines lh apart, at most maxw wide; d NULL: only counts the lines */
static int sv_text(dc *d, int f, float size, float x, float top, float maxw, float lh, const char *s, tk_col c)
{
    char line[200];
    int rows = 0, i = 0;
    while (s[i]) {
        while (s[i] == ' ') i++;
        if (!s[i]) break;
        int n = 0;
        line[0] = 0;
        while (s[i]) {
            int e = i;
            while (s[e] && s[e] != ' ') e++;
            int n0 = n, m = n + (n > 0) + (e - i);
            if (m >= (int)sizeof line) { if (n) break; e = i + (int)sizeof line - 1; m = (int)sizeof line - 1; }
            if (n) line[n++] = ' ';
            tk_memcpy(line + n, s + i, (size_t)(e - i));
            n = m;
            line[n] = 0;
            if (n0 && tw(f, size, line, 0, 0) > maxw) { n = n0; line[n] = 0; break; }
            i = e;
            while (s[i] == ' ') i++;
        }
        if (d) d_text(d, f, size, x, bl(f, size, top + rows * lh), line, 0, 0, c, 0);
        rows++;
    }
    return rows;
}
static void sv_centre(dc *d, int f, float size, float top, const char *s, tk_col c)
{
    d_text(d, f, size, (390 - tw(f, size, s, 0, 0)) / 2, bl(f, size, top), s, 0, 0, c, 0);
}
static int sv_cat(char *b, int n, const char *s) { while (*s) b[n++] = *s++; b[n] = 0; return n; }
static int sv_2(char *b, int n, int v) { b[n++] = (char)('0' + v / 10 % 10); b[n++] = (char)('0' + v % 10); b[n] = 0; return n; }
static void sv_hm(char *b, int min) { int n = sv_2(b, 0, min / 60 % 24); b[n++] = ':'; sv_2(b, n, min % 60); }
/* "2 h 10 min" */
static int sv_dur(char *b, int n, int min)
{
    if (min >= 60) { n += put_dec(b + n, min / 60); n = sv_cat(b, n, " h"); if (min % 60) n = sv_cat(b, n, " "); }
    if (min % 60 || min < 60) { n += put_dec(b + n, min % 60); n = sv_cat(b, n, " min"); }
    return n;
}
/* a value in tenths ("31.2"), hundredths ("60.17") */
static int sv_dec(char *b, int n, int v, int digits)
{
    int div = digits == 2 ? 100 : 10;
    if (v < 0) { b[n++] = '-'; v = -v; }
    n += put_dec(b + n, v / div);
    b[n++] = '.';
    if (digits == 2) n = sv_2(b, n, v % 100); else { b[n++] = (char)('0' + v % 10); b[n] = 0; }
    return n;
}
/* a ring only where this bitmap (a page's strip) has it: its coverage plane is the ring's whole box */
static void sv_ring_d(dc *d, float cx, float cy, float r, float sw, float frac, tk_col c, int caps)
{
    if (PT(cy + r + sw) - d->oy < 0 || PT(cy - r - sw) - d->oy > d->b->h) return;
    d_ring(d, cx, cy, r, sw, frac, c, caps);
}

/* ---- the tiles of the new tools (as the concept's T(): a 143 degree gradient, white glyphs, the sheen, the rim) -- */
static int sv_tile(dc *d, const char *k, float x, float y, float s)
{
    uint32_t a, b;
    int kind;
    if (!tk_strcmp(k, "usb")) { a = 0xffb340; b = 0xe0461f; kind = 0; }
    else if (!tk_strcmp(k, "sun")) { a = 0xffd60a; b = 0xff8a00; kind = 1; }
    else if (!tk_strcmp(k, "guide")) { a = 0x5bd77a; b = 0x1f8a4c; kind = 2; }
    else if (!tk_strcmp(k, "compass")) { a = 0xa1a1a8; b = 0x55555c; kind = 3; }
    else return 0;
    float u = s / 100, r = s * .225f, X = PT(x) - d->ox, Y = PT(y) - d->oy;
    tk_stop st[2] = { { 0, tk_hex(a, 1) }, { 1, tk_hex(b, 1) } };
    tk_paint g = tk_linear(143, X, Y, PT(s), PT(s), 2, st);
    tk_fill_rrect(d->b, X, Y, PT(s), PT(s), PT(r), &g, 1);
#define G(gx, gy, gw, gh, gr) d_rrect(d, x + (gx) * u, y + (gy) * u, (gw) * u, (gh) * u, (gr) * u, W(1))
#define C(gx, gy, gw, gh, gr) tk_fill_rrect(d->b, PT(x + (gx) * u) - d->ox, PT(y + (gy) * u) - d->oy, PT((gw) * u), PT((gh) * u), PT((gr) * u), &g, 1)
    if (kind == 0) {                    /* a USB-C plug */
        G(35, 14, 30, 15, 7.5f); C(40, 19.5f, 20, 4, 2);
        G(30, 29, 40, 28, 7);
        G(44, 56, 12, 9, 3);
        G(47, 63, 6, 24, 3);
    } else if (kind == 1) {             /* the sun on the horizon */
        G(32, 42, 36, 36, 18);
        C(0, 61, 100, 39, r / u);
        G(17, 62, 66, 5, 2.5f); G(31, 73, 38, 5, 2.5f);
        G(47.5f, 20, 5, 13, 2.5f); G(14, 52, 13, 5, 2.5f); G(73, 52, 13, 5, 2.5f);
        G(23.5f, 30.5f, 7, 7, 3.5f); G(69.5f, 30.5f, 7, 7, 3.5f);
    } else if (kind == 2) {             /* an open book */
        G(16, 26, 32, 50, 5); G(52, 26, 32, 50, 5);
        for (int i = 0; i < 3; i++) { C(22, 36 + 9 * i, i == 2 ? 13 : 20, 3.5f, 1.75f); C(58, 36 + 9 * i, i == 2 ? 13 : 20, 3.5f, 1.75f); }
    } else {                            /* a compass rose's ring */
        sv_ring_d(d, x + 50 * u, y + 52 * u, 27 * u, 5 * u, 1, W(1), 0);
        G(45, 47, 10, 10, 5);
        float w = tw(TK_IT700, 17 * u, "N", 0, 0);
        d_text(d, TK_IT700, 17 * u, x + 50 * u - w / 2, y + 41 * u, "N", 0, 0, W(1), 0);
    }
#undef G
#undef C
    tk_stop sh[3] = { { 0, TK_RGBA(255, 255, 255, .28f) }, { .45f, TK_RGBA(255, 255, 255, 0) }, { 1, TK_RGBA(0, 0, 0, .14f) } };
    tk_paint q = tk_linear(180, X, Y, PT(s), PT(s), 3, sh);
    tk_fill_rrect(d->b, X, Y, PT(s), PT(s), PT(r), &q, 1);
    tk_box_shadow(d->b, X, Y, PT(s), PT(s), PT(r), 0, 0, 0, PT(1.6f * u), TK_RGBA(255, 255, 255, .22f), 1);
    return 1;
}

/* ---- the lock's Emergency list: eight cards (0..3 at entering, 4..7 one per idle turn), Exit at the end -------- */
#define SV_EC_EXIT (LIST_TOP + 10 + 78 * 8 - 10 + 12)          /* the Exit pill's top at scroll 0 */
static float sv_ec_max_scroll(void) { return tk_maxf(0, (SV_EC_EXIT - LIST_TOP) + 32 + 44 - LIST_H); }
static const char *sv_ec_sub(int i, char *b)
{
    if (!tk_strcmp(EC[i].act, "sun")) {
        int rise, set, r = sh_sun(&rise, &set);
        if (r == 0) {
            int n = sv_cat(b, 0, "Sunrise ");
            sv_hm(b + n, rise);
            n = sv_cat(b, (int)tk_strlen(b), " \xc2\xb7 sunset ");
            sv_hm(b + n, set);
            return b;
        }
        return r == 1 ? "The sun stays up today" : r == 2 ? "The sun stays down today" :
               r == -2 ? "Sunrise and sunset: type where you are" : "Sunrise and sunset (the clock isn't set)";
    }
    return "";
}
static int sv_ec_hit(float x, float y)
{
    float cy = y - LIST_TOP + scroll;
    if (x < CARD_X || x >= CARD_X + CARD_W) return -1;
    for (int i = 0; i < 8; i++) if (cy >= 10 + 78 * i && cy < 10 + 78 * i + 68) return i;
    if (cy >= SV_EC_EXIT - LIST_TOP - 6 && cy < SV_EC_EXIT - LIST_TOP + 38 && x >= 110 && x < 280) return 8;
    return -1;
}
static void sv_exit_answer(int i) { if (i == 1 && EMG) set_emergency(0, 0); }
static void sv_ec_tap(int i)
{
    if (i == 8) {                       /* helper B2: Exit Emergency asks first (iOS's alert) */
        static const kit_btn B[2] = { { "Cancel", KIT_CANCEL }, { "Exit", KIT_DESTRUCTIVE } };
        kit_alert("Exit Emergency Mode?", "The lock screen and home go back to normal. SOS stops if it is on.", B, 2, sv_exit_answer);
        return;
    }
    if (i < 0 || i >= 8) return;
    int a = sv_app(EC[i].act);
    if (a < 0) {                        /* Survival Heat: as it was (it runs in the classic Starbite OS) */
        char m[48] = "Opens ";
        tk_memcpy(m + 6, EC[i].act, tk_strlen(EC[i].act) + 1);
        toast(m);
        return;
    }
    SV.from_lock = 1;
    SV.lock_r = (arect){ CARD_ICON, LIST_TOP + 10 + 78 * i + 11 - scroll, 46, 46, 10 };
    app_open(a);
}
static int sv_icon_rect(int a, arect *r)
{
    (void)a;
    if (!locked || !SV.from_lock) return 0;
    *r = SV.lock_r;
    return 1;
}
/* idle time: the next of the lock's cards 4..7 (one glass card a turn), the lock's widgets after Last 2 % changed
 * (the switch, or the kernel at 3 %: looked at every idle turn, a minute at the latest) */
static void sv_ec_skip(void) { while (EMG && ec_next < 8 && !EC_LAYER(ec_next)->bmp.px) ec_next++; }   /* a refused layer's cards */
static int sv_idle_pending(void) { sv_ec_skip(); return (EMG && ec_next < 8) || (SV.lw_stale && !AP.on); }
static void sun_fix_ask(void);
static void sv_idle(void)
{
    /* helper J: the Sun page's GPS fix asked again a minute later (the page again only if the fix changed) */
    if (AP.on && !AP.closing && !ap_pending && is_app("sun") && !SV.sun_edit && SV.fix_asked && (now_ms - SV.fix_t > 60000 || now_ms < SV.fix_t)) {
        sh_gnss_t o = SV.fix;
        int oh = SV.fix_have;
        sun_fix_ask();
        if (oh != SV.fix_have || (oh && (o.utc != SV.fix.utc || o.lat_e6 != SV.fix.lat_e6 || o.lon_e6 != SV.fix.lon_e6))) sv_rebuild();
    }
    int l2 = sh_last2();
    if (l2 != SV.l2_seen) { SV.l2_seen = l2; if (EMG) SV.lw_stale = 1; }
    sv_ec_skip();
    if (EMG && ec_next < 8) {
        /* cp7i (7g: sv_idle 26-39 ms on the phone, a card a turn): a card in two turns, its glass then its words; the
         * layer is marked changed only when the card is whole (its glass alone never shows) */
        static int part, part_of = -1;
        static const uint32_t *part_px;
        tk_layer *l = EC_LAYER(ec_next);
        if (part_of != ec_next || part_px != l->bmp.px) { part_of = ec_next; part_px = l->bmp.px; part = 0; }   /* (a new layer: from its glass) */
        if (!part) {
            tk_a8_new(&l->glass, l->bmp.w, l->bmp.h);   /* P3q: the card's glass shape recorded, the mask freed at once */
            dc d = { &l->bmp, l->glass.v ? &l->glass : 0, l->x, l->y };
            ec_card_part(&d, ec_next, LIST_TOP + 10 + 78 * ec_next, 0);
            tk_a8_free(&l->glass);
            part = 1;
            return;
        }
        dc d = { &l->bmp, 0, l->x, l->y };
        ec_card_part(&d, ec_next, LIST_TOP + 10 + 78 * ec_next, 1);
        part_of = -1;
        ec_next++;
        tk_layer_dirty(l);
        l->hidden = 0;
        if (ec_next == 6 || ec_next == 8) { trim(l); tk_layer_prepare(l); }   /* P3s: cut to its two cards */
        lock_ver++;
        return;
    }
    if (SV.lw_stale && !AP.on) {
        SV.lw_stale = 0;
        build_lwidgets();
        relink();
        lock_ver++;
    }
}

/* ---- the live layers: a switch, the beacon picker, the hold ring ------------------------------------------------ */
#define SV_SW_X (CONTENT_R - 16 - 51)   /* a row's switch (iOS: 16 pt from the group's right edge) */
#define SV_RULE(d, y) d_rrect(d, ROW_TX, y, CONTENT_R - ROW_TX, .5f, 0, W(.1f))   /* a rule under a row */
static void sv_switch(int which, float y, int on)
{
    tk_layer *l = &svl[which];
    relayer(l);
    dc d = mk(l, "sv-switch", SV_SW_X, y + 8, 51, 31, 1, 0);
    d_rrect(&d, SV_SW_X, y + 8, 51, 31, 15.5f, on ? tk_hex(0x34c759, 1) : W(.16f));
    d_rrect(&d, on ? SV_SW_X + 22 : SV_SW_X + 2, y + 10, 27, 27, 13.5f, W(1));
}
static void sv_switch_row(dc *d, float y, const char *label, int last)
{
    d_text(d, TK_IT400, 16, ROW_TX, y + 13 + 16, label, 0, 0, INK, 0);
    if (!last) SV_RULE(d, y + 46);
}
#define SOS_SEG_Y 610.0f
#define SOS_SEG_W ((GROUP_W - 4) / 4)   /* a segment */
static const char *const SOS_BEACON[4] = { "White / red", "White", "Red", "Morse" };
static void sv_seg(void)
{
    tk_layer *l = &svl[SVL_SEG];
    relayer(l);
    dc d = mk(l, "sv-seg", GROUP_X, SOS_SEG_Y, GROUP_W, 36, 1, 0);
    d_rrect(&d, GROUP_X, SOS_SEG_Y, GROUP_W, 36, 10, W(.1f));
    int b = (int)(sh_sos_opts() >> SH_BEACON_SHIFT & 3U);
    float w = SOS_SEG_W;
    d_rrect(&d, GROUP_X + 2 + w * b, SOS_SEG_Y + 2, w, 32, 8, tk_hex(0x5a3a3c, 1));
    for (int i = 0; i < 4; i++) {
        float tx = tw(TK_IT600, 13, SOS_BEACON[i], 0, 0);
        d_text(&d, TK_IT600, 13, GROUP_X + 2 + w * i + (w - tx) / 2, bl(TK_IT600, 13, SOS_SEG_Y + 9.5f), SOS_BEACON[i], 0, 0, i == b ? INK : W(.65f), 0);
    }
}
/* the hold's ring around a button: only the ring's pixels are kept (their place, angle, coverage), drawn per frame */
static struct { int *idx; float *ang, *cov; int n; } SR_;
static void sv_ring_free(void)
{
    tk_free(SR_.idx); tk_free(SR_.ang); tk_free(SR_.cov);
    SR_.idx = 0; SR_.ang = SR_.cov = 0; SR_.n = 0;
    relayer(&svl[SVL_RING]);
    svl[SVL_RING].hidden = 1;
}
static void sv_ring_build(float cx, float cy, float r, float sw)
{
    sv_ring_free();
    float R = r + sw;
    tk_layer *l = &svl[SVL_RING];
    dc d = mk(l, "sv-ring", cx - R, cy - R, 2 * R, 2 * R, 1, 0);
    l->hidden = 1;
    if (!l->bmp.px) return;
    int n = 0, N = l->bmp.w * l->bmp.h;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < N; k++) {
            float px = k % l->bmp.w + .5f + d.ox - PT(cx), py = k / l->bmp.w + .5f + d.oy - PT(cy);
            float c = tk_clampf(.5f - (tk_fabsf(tk_sqrtf(px * px + py * py) - PT(r)) - PT(sw / 2)), 0, 1);
            if (c <= 0) continue;
            if (pass) {
                float a = tk_atan2f(px, -py);
                SR_.idx[SR_.n] = k; SR_.ang[SR_.n] = a < 0 ? a + 6.2831853f : a; SR_.cov[SR_.n] = c; SR_.n++;
            } else n++;
        }
        if (!pass) {
            SR_.idx = tk_alloc((size_t)n * 4); SR_.ang = tk_alloc((size_t)n * 4); SR_.cov = tk_alloc((size_t)n * 4);
            if (!SR_.idx || !SR_.ang || !SR_.cov) { sv_ring_free(); return; }
        }
    }
}
static void sv_ring_set(float p)
{
    tk_layer *l = &svl[SVL_RING];
    if (!SR_.n || !l->bmp.px) return;
    uint32_t col = tk_premul(tk_hex(SV_TINT, 1));
    float sweep = 6.2831853f * p, aa = 1.0f / (l->bmp.w * .45f);
    for (int i = 0; i < SR_.n; i++) {
        float e = tk_clampf((sweep - SR_.ang[i]) / aa + .5f, 0, 1) * SR_.cov[i];
        unsigned A = (unsigned)(e * 255 + .5f);
        l->bmp.px[SR_.idx[i]] = A == 255 ? col : (((col >> 24) * A / 255) << 24 | ((col >> 16 & 255) * A / 255) << 16 |
                                                  ((col >> 8 & 255) * A / 255) << 8 | ((col & 255) * A / 255));
    }
    l->hidden = p <= 0;
    tk_layer_dirty(l);
}
/* a big round hold button (drawn into the page or a layer) */
static void sv_hold_button(dc *d, float cx, float cy, float r, const char *label, float size, int enabled)
{
    if (enabled) {
        tk_stop st[2] = { { 0, tk_hex(0xff5a4e, 1) }, { 1, tk_hex(0xc8180e, 1) } };
        tk_paint p = tk_linear(160, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), 2, st);
        tk_box_shadow(d->b, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), PT(r), 0, PT(6), PT(18), 0, TK_RGBA(255, 40, 30, .35f), 0);
        tk_fill_rrect(d->b, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), PT(r), &p, 1);
        tk_box_shadow(d->b, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), PT(r), 0, PT(1.5f), 0, 0, W(.35f), 1);
    } else
        d_rrect(d, cx - r, cy - r, 2 * r, 2 * r, r, W(.12f));
    float w = tw(TK_IT700, size, label, 0, 0);
    d_text(d, TK_IT700, size, cx - w / 2, bl(TK_IT700, size, cy - lh(TK_IT700, size) / 2), label, 0, 0, enabled ? W(1) : W(.4f), 0);
}
/* a pushed page's nav row (as shell_lock.c's): the glass back circle, the title inline */
static void sv_back(dc *d, const char *title)
{
    d_back(d, W(1));
    d_navtitle(d, title);
}
static int sv_back_hit(float x, float y) { return BACK_HIT(x, y); }
/* an editor's nav row: Cancel, the title inline, Done (as Wi-Fi's password page) */
static void sv_editbar(dc *d, const char *title, tk_col c)
{
    d_text(d, TK_IT400, 17, MARGIN, NAV_BASE, "Cancel", 0, 0, c, 0);
    d_navtitle(d, title);
    float w = tw(TK_IT600, 17, "Done", 0, 0);
    d_text(d, TK_IT600, 17, CONTENT_R - w, NAV_BASE, "Done", 0, 0, c, 0);
}
#define SV_CANCEL_HIT(x, y) (IN_NAV(y) && (x) < MARGIN + 90)

/* ---- SOS ------------------------------------------------------------------------------------------------------ */
#define SOS_CX 195.0f
#define SOS_CY 330.0f
#define SOS_R 64.0f
#define SOS_ROW 470.0f
static void page_sos(dc *d)
{
    d_h1(d, "Emergency SOS");
    sv_text(d, TK_IT400, 15, MARGIN, CONTENT_TOP, CONTENT_W, 20,
            "Hold the button for a second. After a 3 s countdown the torch and the vibration motor send S-O-S in Morse "
            "and the screen flashes as a beacon. Stop ends it.", W(.7f));
    sv_hold_button(d, SOS_CX, SOS_CY, SOS_R, "SOS", 34, 1);
    sv_centre(d, TK_IT400, 15, SOS_CY + SOS_R + 20, "Hold for 1 s", W(.6f));
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, SOS_ROW - 22), "MORSE S-O-S", .52f, 0, W(.55f), 0);
    d_rrect(d, GROUP_X, SOS_ROW, GROUP_W, 93, 16, SVC(SV_GROUP));
    sv_switch_row(d, SOS_ROW, "Torch", 0);
    sv_switch_row(d, SOS_ROW + 47, "Vibration", 1);
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, SOS_SEG_Y - 22), "THE SCREEN", .52f, 0, W(.55f), 0);
    sv_text(d, TK_IT400, 13, ROW_TX, SOS_SEG_Y + 50, CONTENT_R - 16 - ROW_TX, 18, "Calls to 112 need a SIM and the modem: not in this version.", W(.4f));
}
static void sos_extras(void)
{
    unsigned o = sh_sos_opts();
    sv_switch(SVL_A, SOS_ROW, (o & SH_SOS_TORCH) != 0);
    sv_switch(SVL_B, SOS_ROW + 47, (o & SH_SOS_VIB) != 0);
    sv_seg();
    sv_ring_build(SOS_CX, SOS_CY, SOS_R + 9, 4);
}
/* the beacon: the colour full screen, a dark card with the state and Stop */
#define SOS_STOP_Y 676.0f
static void sv_sos_text(void)
{
    relayer(&sostx);
    dc d = mk(&sostx, "sos-text", 24, 556, 342, 214, 1, 0);
    d_rrect(&d, 24, 556, 342, 214, 32, TK_RGBA(12, 6, 6, .88f));
    d_text(&d, TK_IT700, 24, 48, bl(TK_IT700, 24, 580), "Emergency SOS", 0, 0, INK, 0);
    char b[80];
    unsigned o = sh_sos_opts();
    if (SV.sos_secs > 0) {
        int n = sv_cat(b, 0, "Starting in ");
        n += put_dec(b + n, SV.sos_secs);
        sv_cat(b, n, " s \xc2\xb7 Stop cancels it");
    } else
        sv_cat(b, 0, (o & SH_SOS_TORCH) && (o & SH_SOS_VIB) ? "Morse S-O-S on the torch and the motor" :
                     o & SH_SOS_TORCH ? "Morse S-O-S on the torch" : o & SH_SOS_VIB ? "Morse S-O-S on the motor" :
                     "The screen flashes");
    sv_text(&d, TK_IT400, 15, 48, 616, 300, 20, b, W(.8f));
    d_rrect(&d, 44, SOS_STOP_Y, 302, 64, 32, tk_hex(0xff3b30, 1));
    float w = tw(TK_IT700, 22, "Stop", 0, 0);
    d_text(&d, TK_IT700, 22, 195 - w / 2, bl(TK_IT700, 22, SOS_STOP_Y + 18), "Stop", 0, 0, W(1), 0);
    tk_layer_prepare(&sostx);
}
static void sv_sos_show(void)
{
    uint32_t c = 0xff1c0606U;
    if (!sh_sos_frame(&c)) { SV.sos_on = 0; sosl.hidden = 1; return; }
    SV.sos_on = 1;
    SV.sos_col = c;
    SV.sos_secs = sh_sos_secs();
    sosbg.solid = c | 0xff000000U;
    sosbg.solid_w = TK_W;
    sosbg.solid_h = TK_H;
    sosbg.opaque = 1;
    sosbg.gen++;
    sv_sos_text();
    sosl.hidden = 0;
    relink();
}
static void sv_sos_hide(void)
{
    SV.sos_on = 0;
    sosl.hidden = 1;
    relayer(&sostx);
}
static void sv_sos_stop(void)
{
    sh_sos_set(0);
    sv_sos_hide();
    toast("SOS stopped");
}
static void sos_tap(float x, float y)
{
    unsigned o = sh_sos_opts();
    if (x >= GROUP_X && x < CONTENT_R && y >= SOS_ROW && y < SOS_ROW + 94) {
        o ^= y < SOS_ROW + 47 ? SH_SOS_TORCH : SH_SOS_VIB;
        sh_sos_opts_set(o);
        o = sh_sos_opts();
        sv_switch(SVL_A, SOS_ROW, (o & SH_SOS_TORCH) != 0);
        sv_switch(SVL_B, SOS_ROW + 47, (o & SH_SOS_VIB) != 0);
        relink();
    } else if (x >= GROUP_X && x < CONTENT_R && y >= SOS_SEG_Y && y < SOS_SEG_Y + 36) {
        int b = (int)((x - GROUP_X - 2) / SOS_SEG_W);
        b = b < 0 ? 0 : b > 3 ? 3 : b;
        sh_sos_opts_set((o & ~(3U << SH_BEACON_SHIFT)) | (unsigned)b << SH_BEACON_SHIFT);
        sv_seg();
        relink();
    }
}

/* ---- Medical Card ----------------------------------------------------------------------------------------------- */
#define MC_NAME CONTENT_TOP                     /* the name's line, under the large title */
#define MC_TOP (CONTENT_TOP + 46)               /* the fields' group */
#define MC_VX (GROUP_X + 128)                   /* a field's value */
#define MC_VW (CONTENT_R - 16 - MC_VX)
static void page_medical(dc *d)
{
    d_h1(d, "Medical Card");
    sh_card_t *c = &SV.card;
    if (!sh_card(c)) { d_empty(d, CONTENT_TOP, EMPTY_BOT, "No Medical Card", "Add one in Settings > Medical Card. It shows here without unlocking."); return; }
    char nm[80];
    ellipsize(nm, sizeof nm, TK_IT700, 26, c->f[0][0] ? c->f[0] : "No name", CONTENT_W);
    d_text(d, TK_IT700, 26, MARGIN, bl(TK_IT700, 26, MC_NAME), nm, 0, 0, INK, 0);
    float y = MC_TOP, h = 0;
    for (int i = 1; i < SH_CARD_NF; i++)
        if (c->f[i][0]) h += 20 + 21.0f * sv_text(0, TK_IT400, 16, 0, 0, MC_VW, 21, c->f[i], INK);
    if (h > 0) {
        d_rrect(d, GROUP_X, y, GROUP_W, h, 16, SVC(SV_GROUP));
        for (int i = 1; i < SH_CARD_NF; i++) {
            if (!c->f[i][0]) continue;
            d_text(d, TK_IT400, 15, ROW_TX, bl(TK_IT400, 15, y + 11), sh_card_label(i), 0, 0, i == 2 ? SVC(SV_WARN) : W(.55f), 0);
            int n = sv_text(d, TK_IT400, 16, MC_VX, y + 10, MC_VW, 21, c->f[i], INK);
            y += 20 + 21.0f * n;
            if (y < MC_TOP + h - 1) SV_RULE(d, y);
        }
    }
    sv_text(d, TK_IT400, 13, ROW_TX, y + 14, CONTENT_R - 16 - ROW_TX, 18, "Edit it in Settings > Medical Card (the Info app shows the same card).", W(.4f));
}

/* Settings > Medical Card: the fields, then one field over the keyboard (B3: the shell keyboard, shell_kb.inc;
 * a phone number's + is on its 123 layer, KB_PHONE) */
static void sv_kb(dc *d, int shift, int num, int plus)
{
    (void)plus;
    kb_draw(d, shift, num);
}
/* a key: 1 the text changed, 5 return; 0 nothing (shift and 123 are the keyboard's own) */
static int sv_kb_tap(float x, float y, char *buf, int cap, int digits_only)
{
    (void)x; (void)y;
    const char *s;
    int ev = kb_take(&s);
    if (ev == KBE_RET) return 5;
    if (digits_only && ev == KBE_TEXT && !((s[0] >= '0' && s[0] <= '9') || s[0] == '-' || s[0] == '.')) return 0;
    return kb_edit(buf, cap) ? 1 : 0;
}
#define CE_TOP DSP_Y                    /* the fields' group (where Display's, Wi-Fi's and Cellular's start) */
#define CE_FLD PUSHED_TOP               /* the editor's field */
static void sv_page_card_edit(dc *d)
{
    sh_card_t *c = &SV.card;
    if (SV.cf < 0) {
        sh_card(c);
        sv_back(d, "Medical Card");
        d_text(d, TK_IT400, 13, ROW_TX, CE_TOP - 10, "SHOWN IN EMERGENCY WITHOUT UNLOCKING", .52f, 0, W(.55f), 0);
        d_rrect(d, GROUP_X, CE_TOP, GROUP_W, 47.0f * SH_CARD_NF - 1, 16, C_CARD);
        for (int i = 0; i < SH_CARD_NF; i++) {
            float y = CE_TOP + 47.0f * i;
            d_text(d, TK_IT400, 16, ROW_TX, y + 13 + 16, sh_card_label(i), 0, 0, INK, 0);
            char v[80];
            ellipsize(v, sizeof v, TK_IT400, 16, c->f[i][0] ? c->f[i] : "Add", 190);
            float w = tw(TK_IT400, 16, v, 0, 0);
            d_text(d, TK_IT400, 16, CONTENT_R - 36 - w, y + 13 + 16, v, 0, 0, c->f[i][0] ? W(.55f) : ACCENT(), 0);
            d_icon(d, "chev", CONTENT_R - 30, y + 16.5f, .45f);
            if (i + 1 < SH_CARD_NF) d_rrect(d, ROW_TX, y + 46, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
        }
        sv_text(d, TK_IT400, 13, ROW_TX, CE_TOP + 47.0f * SH_CARD_NF + 10, CONTENT_R - 16 - ROW_TX, 18,
                "Kept on this phone with the settings. Emergency mode shows it on the lock screen; the Info app edits the same card.", W(.4f));
        return;
    }
    sv_editbar(d, sh_card_label(SV.cf), ACCENT());
    d_rrect(d, GROUP_X, CE_FLD, GROUP_W, 52, 14, C_CARD);
    d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, CE_FLD + 64), "Up to 64 characters. Return or Done saves it.", 0, 0, W(.45f), 0);
    sv_kb(d, SV.shift, SV.num, 1);
}
/* the field's text and caret (a key redraws only this) */
static void sv_field(float x, float y, float w, const char *t, int caret, tk_col col)
{
    tk_layer *l = &svl[SVL_FLD];
    relayer(l);
    dc d = mk(l, "sv-field", x, y, w, 32, 1, 0);
    char e[96];
    const char *s = t;
    if (tw(TK_IT400, 17, t, 0, 0) > w - 12) {             /* the end of a long text, as a field scrolls */
        int n = (int)tk_strlen(t), k = 0;
        while (k < n && tw(TK_IT400, 17, t + k, 0, 0) > w - 24) { k++; while ((t[k] & 0xc0) == 0x80) k++; }
        e[0] = (char)0xe2; e[1] = (char)0x80; e[2] = (char)0xa6;
        tk_memcpy(e + 3, t + k, (size_t)(n - k) + 1);
        s = e;
    }
    d_text(&d, TK_IT400, 17, x, bl(TK_IT400, 17, y + 4), s, 0, 0, col, 0);
    if (caret) d_rrect(&d, x + tw(TK_IT400, 17, s, 0, 0) + 1, y + 4, 2, 22, 1, ACCENT());
}
static void card_tap(float x, float y)
{
    if (SV.cf < 0) {
        if (sv_back_hit(x, y)) { AS.sub = se_back_from_card(); build_app_page(AP.a); return; }   /* helper B2: back to Emergency */
        if (x >= GROUP_X && x < CONTENT_R && y >= CE_TOP && y < CE_TOP + 47.0f * SH_CARD_NF) {
            SV.cf = (int)((y - CE_TOP) / 47);
            tk_memcpy(SV.ebuf, SV.card.f[SV.cf], sizeof SV.ebuf);
            SV.shift = !SV.ebuf[0] && SV.cf != 1;
            SV.num = SV.cf >= 5 && SV.cf <= 7;               /* Call, Call 2, Call 3: a number first */
            kb_focus((SV.cf != 1 ? KB_AUTOCAP : 0) | (SV.num ? KB_DIGITS | KB_PHONE : 0), "done", SV.ebuf);   /* B3 */
            sv_rebuild();
        }
        return;
    }
    int save = DONE_HIT(x, y);
    if (!save && SV_CANCEL_HIT(x, y)) { SV.cf = -1; sv_rebuild(); return; }
    int k = save ? 5 : y >= 540 ? sv_kb_tap(x, y, SV.ebuf, SH_CARD_FL, 0) : 0;
    if (k == 5) {
        tk_memcpy(SV.card.f[SV.cf], SV.ebuf, sizeof SV.ebuf);
        toast(sh_card_set(&SV.card) ? "Medical Card saved" : "The card could not be saved");
        SV.cf = -1;
        sv_rebuild();
    } else if (k == 2) sv_rebuild();
    else if (k == 1) { sv_field(ROW_TX, CE_FLD + 10, CONTENT_R - 16 - ROW_TX, SV.ebuf, 1, INK); relink(); }
}

/* ---- Last 2 % ----------------------------------------------------------------------------------------------------- */
#define L2_ROW (CONTENT_TOP + 148)       /* the switches' group, under the two paragraphs */
static void page_last2(dc *d)
{
    d_h1(d, "Last 2 %");
    int n = sv_text(d, TK_IT600, 18, MARGIN, CONTENT_TOP, CONTENT_W, 24, "Everything off except the clock, the torch and SOS. Runs on to 1 %.", INK);
    sv_text(d, TK_IT400, 15, MARGIN, CONTENT_TOP + 8 + 24.0f * n, CONTENT_W, 20,
            "The slowest processor speed and a dim screen; Wi-Fi, the modem, Survival Heat and USB power stay off.", W(.6f));
    d_rrect(d, GROUP_X, L2_ROW, GROUP_W, 93, 16, SVC(SV_GROUP));
    sv_switch_row(d, L2_ROW, "Last 2 %", 0);
    sv_switch_row(d, L2_ROW + 47, "Automatically at 3 %", 1);
    sv_text(d, TK_IT400, 13, ROW_TX, L2_ROW + 104, CONTENT_R - 16 - ROW_TX, 18, "At 3 % the phone goes into Last 2 % instead of switching off.", W(.45f));
}
static void last2_extras(void)
{
    sv_switch(SVL_A, L2_ROW, sh_last2());
    sv_switch(SVL_B, L2_ROW + 47, sh_last2_auto());
}
static void last2_tap(float x, float y)
{
    if (x < GROUP_X || x >= CONTENT_R || y < L2_ROW || y >= L2_ROW + 94) return;
    if (y < L2_ROW + 47) {
        int on = !sh_last2();
        if (sh_last2_set(on) < 0) { const char *w = sh_last2_why(); toast(w && w[0] ? w : "Last 2 % can't change now"); }
        else toast(on ? "Last 2 % on" : "Last 2 % off");     /* (the lock's SAVING widget: sv_idle) */
    } else
        sh_last2_auto_set(!sh_last2_auto());
    last2_extras();
    relink();
}

/* ---- USB Power ---------------------------------------------------------------------------------------------------- */
#define USB_CY 560.0f
#define USB_R 58.0f
#define USB_GUIDE CONTENT_TOP           /* the guidance's group (the caption is under the large title, as Photos') */
static float usb_guide_h(dc *d)
{
    float y = USB_GUIDE + 12;
    for (int i = 0; sh_usb_guide(i); i++) {
        const char *g = sh_usb_guide(i);
        int warn = g[0] == 'N' && g[1] == 'e' && g[2] == 'v';       /* "Never ..." */
        y += 19.0f * sv_text(d, warn ? TK_IT600 : TK_IT400, 14, ROW_TX, y, CONTENT_R - 16 - ROW_TX, 19, g, warn ? SVC(SV_WARN) : W(.8f)) + 7;
    }
    return y + 5;
}
static void page_usb(dc *d)
{
    d_h1(d, "USB Power");
    d_text(d, TK_IT400, 13, MARGIN, LARGE_BASE + 20, "A fire starter: 5 V out of the USB-C port", 0, 0, W(.55f), 0);
    d_rrect(d, GROUP_X, USB_GUIDE + 6, GROUP_W, usb_guide_h(0) - USB_GUIDE - 6, 16, SVC(SV_GROUP));
    usb_guide_h(d);
}
static int usb_secs(void) { return SV.usb.ms_left > 0 ? (SV.usb.ms_left + 999) / 1000 : 0; }
/* the button (hold, Stop, or the cooldown's disabled hold) */
static void usb_controls(void)
{
    tk_layer *l = &svl[SVL_CTL];
    relayer(l);
    if (SV.usb.state == SH_USB_ON) {
        dc d = mk(l, "sv-usb-ctl", 24, 690, 342, 66, 1, 0);
        d_rrect(&d, 24, 690, 342, 66, 33, tk_hex(0xff3b30, 1));
        float w = tw(TK_IT700, 24, "Stop", 0, 0);
        d_text(&d, TK_IT700, 24, 195 - w / 2, bl(TK_IT700, 24, 690 + 18), "Stop", 0, 0, W(1), 0);
        sv_ring_free();
    } else {
        dc d = mk(l, "sv-usb-ctl", 195 - USB_R - 30, USB_CY - USB_R - 30, 2 * USB_R + 60, 2 * USB_R + 60, 1, 0);
        sv_hold_button(&d, 195, USB_CY, USB_R, SV.usb.state == SH_USB_COOL ? "Wait" : "Hold", 26, SV.usb.state == SH_USB_OFF);
        if (SV.usb.state == SH_USB_OFF && !SR_.n) sv_ring_build(195, USB_CY, USB_R + 9, 4);
        if (SV.usb.state != SH_USB_OFF) sv_ring_free();
    }
}
static void usb_text(void)
{
    tk_layer *l = &svl[SVL_TXT];
    relayer(l);
    char b[96];
    int n;
    SV.usb_secs = usb_secs();
    SV.usb_ma = SV.usb.est_ma; SV.usb_c = SV.usb.bat_c10; SV.usb_vl = SV.usb.vbus_lo_mv; SV.usb_vh = SV.usb.vbus_hi_mv;
    SV.usb_why = SV.usb.why;
    SV.usb_ev = SV.usb.events;
    if (SV.usb.state == SH_USB_ON) {
        dc d = mk(l, "sv-usb-txt", 24, 474, 342, 210, 1, 0);
        put_dec(b, SV.usb_secs);
        float w = tw(TK_IT700, 72, b, 0, TK_TNUM);
        d_text(&d, TK_IT700, 72, 195 - w / 2, bl(TK_IT700, 72, 470), b, 0, TK_TNUM, INK, 0);
        sv_centre(&d, TK_IT400, 15, 556, "seconds left", W(.6f));
        float y = 590;
        if (SV.usb.est_ma >= 0) {
            n = sv_cat(b, 0, "About ");
            int ma = SV.usb.est_ma;
            if (ma >= 1000) { n += put_dec(b + n, ma / 1000); b[n++] = ','; n = sv_2(b, n, ma % 1000 / 10); b[n++] = (char)('0' + ma % 10); b[n] = 0; }
            else n += put_dec(b + n, ma);
            sv_cat(b, n, " mA out");
            sv_centre(&d, TK_IT600, 17, y, b, INK);
            y += 26;
        }
        if (SV.usb.vbus_lo_mv >= 0 && SV.usb.vbus_hi_mv >= 0) {
            n = sv_cat(b, 0, "The port: ");
            n = sv_dec(b, n, SV.usb.vbus_lo_mv / 100, 1);
            n = sv_cat(b, n, " \xe2\x80\x93 ");
            n = sv_dec(b, n, SV.usb.vbus_hi_mv / 100, 1);
            sv_cat(b, n, " V");
            sv_centre(&d, TK_IT400, 15, y, b, W(.75f));
            y += 22;
        }
        n = sv_cat(b, 0, "Battery ");
        n = sv_dec(b, n, SV.usb.bat_c10, 1);
        n = sv_cat(b, n, " \xc2\xb0" "C \xc2\xb7 it stops at ");
        n += put_dec(b + n, SV.usb.limit_c10 / 10);
        sv_cat(b, n, " \xc2\xb0" "C");
        sv_centre(&d, TK_IT400, 15, y, b, SV.usb.bat_c10 >= SV.usb.limit_c10 - 50 ? SVC(SV_WARN) : W(.75f));
        return;
    }
    dc d = mk(l, "sv-usb-txt", 24, 636, 342, 130, 1, 0);
    if (SV.usb.state == SH_USB_COOL) {
        n = sv_cat(b, 0, "Cooling down \xc2\xb7 ");
        n += put_dec(b + n, SV.usb_secs);
        sv_cat(b, n, " s");
        sv_centre(&d, TK_IT600, 16, 640, b, INK);
    } else
        sv_centre(&d, TK_IT400, 15, 640, "Hold 1.5 s to start: 10 s of power", W(.65f));
    if (SV.usb.why) {
        char w[96];
        n = sv_cat(w, 0, SV.usb.refused ? "Refused: " : "Last burst ended: ");
        int k = 0;
        while (SV.usb.why[k] && n < 94) w[n++] = SV.usb.why[k++];
        w[n] = 0;
        sv_text(&d, TK_IT400, 14, ROW_TX, 668, CONTENT_R - 16 - ROW_TX, 19, w, SV.usb.refused ? SVC(SV_WARN) : W(.6f));
    } else
        sv_text(&d, TK_IT400, 14, ROW_TX, 668, CONTENT_R - 16 - ROW_TX, 19, "Then 30 s to cool down. It stops by itself on a fault, a hot or low battery, or a charger.", W(.45f));
}
static void usb_extras(void)
{
    sh_usb(&SV.usb);                    /* once at the open; then 2 Hz only while on or cooling */
    SV.usb_next = now_ms + 500;
    usb_controls();
    usb_text();
}
static void usb_poll(float t)
{
    SV.usb_next = t + 500;
    int was = SV.usb.state;
    sh_usb(&SV.usb);
    if (SV.usb.state != was) { usb_controls(); usb_text(); relink(); return; }
    if (usb_secs() != SV.usb_secs || SV.usb.est_ma / 10 != SV.usb_ma / 10 || SV.usb.bat_c10 != SV.usb_c ||
        SV.usb.vbus_lo_mv != SV.usb_vl || SV.usb.vbus_hi_mv != SV.usb_vh || SV.usb.events != SV.usb_ev) {
        usb_text();
        relink();
    }
}
static void usb_tap(float x, float y)
{
    if (SV.usb.state == SH_USB_ON && x >= 24 && x < 366 && y >= 690 && y < 756) {
        sh_usb_stop();
        usb_poll(now_ms);
        toast("USB power off");
    }
}

/* ---- Sun ----------------------------------------------------------------------------------------------------------- */
#define SUN_TOP CONTENT_TOP             /* today's group */
#define SUN_LOC (SUN_TOP + 128 + 40)    /* the location's row (its caption above) */
#define SUN_ED PUSHED_TOP               /* the editor's two rows */
static int sv_parse_x100(const char *s, int *out)
{
    int neg = 0, v = 0, frac = -1, any = 0;
    if (*s == '-') { neg = 1; s++; }
    for (; *s; s++) {
        if (*s == '.') { if (frac >= 0) return 0; frac = 0; continue; }
        if (*s < '0' || *s > '9') return 0;
        any = 1;
        if (frac < 0) { v = v * 10 + (*s - '0'); if (v > 400) return 0; }
        else if (frac < 2) { v = v * 10 + (*s - '0'); frac++; }
    }
    if (!any) return 0;
    if (frac < 0) frac = 0;
    while (frac < 2) { v *= 10; frac++; }
    *out = neg ? -v : v;
    return 1;
}
static void sv_fmt_x100(char *b, int v)
{
    int n = 0;
    if (v < 0) { b[n++] = '-'; v = -v; }
    n += put_dec(b + n, v / 100);
    b[n++] = '.';
    sv_2(b, n, v % 100);
}
static void sun_loc_text(char *b)
{
    int la, lo;
    sh_loc(&la, &lo);
    if (la == SH_NOLOC || lo == SH_NOLOC) { sv_cat(b, 0, "Not set"); return; }
    int n = sv_dec(b, 0, la < 0 ? -la : la, 2);
    n = sv_cat(b, n, la < 0 ? "\xc2\xb0 S, " : "\xc2\xb0 N, ");
    n = sv_dec(b, n, lo < 0 ? -lo : lo, 2);
    sv_cat(b, n, lo < 0 ? "\xc2\xb0 W" : "\xc2\xb0 E");
}
/* helper J: Android's last GPS fix (sh_gnss: /STARBITE/GNSS/LASTFIX.TXT on the card, cached by the kernel); asked at a
 * page's build (Sun, Compass) and again from idle time a minute later while Sun shows */
static void sun_fix_ask(void)
{
    sh_gnss_t f;
    tk_memset(&f, 0, sizeof f);
    SV.fix_have = sh_gnss(&f) == 1;
    if (SV.fix_have) SV.fix = f;
    SV.fix_asked = 1;
    SV.fix_t = now_ms;
}
static int sv_e6_x100(int e6) { return (e6 < 0 ? -1 : 1) * (((e6 < 0 ? -e6 : e6) + 5000) / 10000); }
/* "60.17° N, 24.94° E" */
static int sun_fix_place(char *b, int n)
{
    int la = sv_e6_x100(SV.fix.lat_e6), lo = sv_e6_x100(SV.fix.lon_e6);
    n = sv_dec(b, n, la < 0 ? -la : la, 2);
    n = sv_cat(b, n, la < 0 ? "\xc2\xb0 S, " : "\xc2\xb0 N, ");
    n = sv_dec(b, n, lo < 0 ? -lo : lo, 2);
    return sv_cat(b, n, lo < 0 ? "\xc2\xb0 W" : "\xc2\xb0 E");
}
/* "25 min old" (from the phone's clock; "age unknown" without one) */
static int sun_fix_age(char *b, int n)
{
    long long now = sh_utc(), s = now - SV.fix.utc;
    if (!now || s < -120) return sv_cat(b, n, "age unknown");
    long long m = s / 60;
    if (m < 1) return sv_cat(b, n, "just now");
    if (m < 120) { n += put_dec(b + n, (int)m); return sv_cat(b, n, " min old"); }
    if (m < 48 * 60) { n += put_dec(b + n, (int)(m / 60)); return sv_cat(b, n, " h old"); }
    n += put_dec(b + n, (int)tk_mini((int)(m / 1440), 99999));
    return sv_cat(b, n, " days old");
}
/* the Compass's line under the heading: its coordinates when there is a fix */
static void cp_fix_line(char *b, int n)
{
    if (!SV.fix_have) return;
    n = sv_cat(b, n, " \xc2\xb7 ");
    sun_fix_place(b, n);
}
/* the editor's two fields from the fix (Done saves them) */
static void sun_fill_fix(void)
{
    sv_fmt_x100(SV.sun_v[0], sv_e6_x100(SV.fix.lat_e6));
    sv_fmt_x100(SV.sun_v[1], sv_e6_x100(SV.fix.lon_e6));
}
#define SUN_FIX_ROW 62.0f               /* the main page's GPS row */
#define SUN_FIX_BTN (SUN_ED + 176)      /* the editor's Use GPS Fix button */
static void page_sun(dc *d)
{
    if (!SV.fix_asked) sun_fix_ask();
    if (SV.sun_edit) {
        sv_editbar(d, "Location", SVC(SV_TINT));
        if (SV.fix_have) {
            char b[64];
            int n = sv_cat(b, 0, "Use GPS Fix \xc2\xb7 ");
            sun_fix_age(b, n);
            d_rrect(d, GROUP_X, SUN_FIX_BTN, GROUP_W, 44, 22, W(.12f));
            sv_centre(d, TK_IT600, 16, SUN_FIX_BTN + 12, b, SVC(SV_TINT));
        }
        d_rrect(d, GROUP_X, SUN_ED, GROUP_W, 93, 16, SVC(SV_GROUP));
        for (int i = 0; i < 2; i++) {
            d_text(d, TK_IT400, 16, ROW_TX, SUN_ED + 47 * i + 29, i ? "Longitude" : "Latitude", 0, 0, i == SV.sun_f ? INK : W(.6f), 0);
            if (i == SV.sun_f) d_rrect(d, GROUP_X + 4, SUN_ED + 47 * i + 12, 4, 23, 2, SVC(SV_TINT));
        }
        SV_RULE(d, SUN_ED + 46);
        sv_text(d, TK_IT400, 13, ROW_TX, SUN_ED + 106, CONTENT_R - 16 - ROW_TX, 18, "Degrees with up to two decimals, south and west negative: Helsinki 60.17 and 24.94, "
                "Sydney -33.87 and 151.21. Tap a row to type into it.", W(.5f));
        sv_kb(d, 0, 1, 0);
        return;
    }
    d_h1(d, "Sun");
    int rise = 0, set = 0, r = sh_sun(&rise, &set);
    char b[64], c[64];
    d_rrect(d, GROUP_X, SUN_TOP, GROUP_W, 128, 16, SVC(SV_GROUP));
    if (r == 0) {
        for (int i = 0; i < 2; i++) {
            float x = ROW_TX + GROUP_W / 2 * i;
            d_text(d, TK_IT400, 15, x, bl(TK_IT400, 15, SUN_TOP + 14), i ? "Sunset" : "Sunrise", 0, 0, W(.6f), 0);
            sv_hm(b, i ? set : rise);
            d_text(d, TK_IT700, 44, x, bl(TK_IT700, 44, SUN_TOP + 36), b, 0, TK_TNUM, i ? SVC(0xff9f0a) : SVC(0xffd60a), 0);
        }
        int now = ST.h * 60 + ST.mi, n;
        if (now < rise) { n = sv_cat(b, 0, "Sunrise in "); sv_dur(b, n, rise - now); }
        else if (now < set) { n = sv_cat(b, 0, "Daylight left: "); sv_dur(b, n, set - now); }
        else { n = sv_cat(b, 0, "The sun has set \xc2\xb7 day length "); sv_dur(b, n, set - rise); }
        d_text(d, TK_IT600, 16, ROW_TX, bl(TK_IT600, 16, SUN_TOP + 94), b, 0, 0, INK, 0);
    } else {
        const char *m = r == 1 ? "The sun stays up all day today (polar day)." :
                        r == 2 ? "The sun stays down all day today (polar night)." :
                        r == -2 ? "No location yet: tap Edit and type where you are." :
                        "The phone's clock isn't set, so the times can't be worked out.";
        sv_text(d, TK_IT600, 17, ROW_TX, SUN_TOP + 28, CONTENT_R - 16 - ROW_TX, 24, m, INK);
    }
    d_text(d, TK_IT400, 13, ROW_TX, SUN_LOC - 10, "LOCATION", .52f, 0, W(.55f), 0);
    float gh = 47 + (SV.fix_have ? SUN_FIX_ROW : 0);
    d_rrect(d, GROUP_X, SUN_LOC, GROUP_W, gh, 16, SVC(SV_GROUP));
    sun_loc_text(c);
    d_text(d, TK_IT400, 16, ROW_TX, SUN_LOC + 29, c, 0, 0, INK, 0);
    float w = tw(TK_IT600, 16, "Edit", 0, 0);
    d_text(d, TK_IT600, 16, CONTENT_R - 16 - w, SUN_LOC + 29, "Edit", 0, 0, SVC(SV_TINT), 0);
    if (SV.fix_have) {                  /* helper J: Android's last fix, its age, Use (fills the editor's fields) */
        float y = SUN_LOC + 47;
        SV_RULE(d, y - 1);
        int n = sv_cat(c, 0, "GPS fix: ");
        sun_fix_place(c, n);
        d_text(d, TK_IT400, 16, ROW_TX, bl(TK_IT400, 16, y + 10), c, 0, 0, INK, 0);
        n = sun_fix_age(c, 0);
        sv_cat(c, n, " \xc2\xb7 Android's, from the memory card");
        d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y + 34), c, 0, 0, W(.5f), 0);
        w = tw(TK_IT600, 16, "Use", 0, 0);
        d_text(d, TK_IT600, 16, CONTENT_R - 16 - w, bl(TK_IT600, 16, y + 20), "Use", 0, 0, SVC(SV_TINT), 0);
    }
    sv_text(d, TK_IT400, 13, ROW_TX, SUN_LOC + gh + 13, CONTENT_R - 16 - ROW_TX, 18, "From the phone's clock and the place you type or Android's last GPS fix; "
            "checked against NOAA's calculator to a minute. The sun is in the south at midday north of the tropics.", W(.45f));
}
static void sun_fields(void)
{
    tk_layer *l = &svl[SVL_FLD];
    relayer(l);
    dc d = mk(l, "sv-sun", CONTENT_R - 220, SUN_ED + 6, 210, 88, 1, 0);
    for (int i = 0; i < 2; i++) {
        float y = SUN_ED + 47 * i;
        const char *t = SV.sun_v[i];
        float w = tw(TK_IT400, 17, t, 0, TK_TNUM);
        d_text(&d, TK_IT400, 17, CONTENT_R - 20 - w, bl(TK_IT400, 17, y + 13), t, 0, TK_TNUM, INK, 0);
        if (i == SV.sun_f) d_rrect(&d, CONTENT_R - 18, y + 12, 2, 23, 1, SVC(SV_TINT));
    }
}
static void sun_tap(float x, float y)
{
    if (!SV.sun_edit) {
        if (x >= CONTENT_R - 120 && y >= SUN_LOC && y < SUN_LOC + 47) {
            int la, lo;
            sh_loc(&la, &lo);
            SV.sun_v[0][0] = SV.sun_v[1][0] = 0;
            if (la != SH_NOLOC) sv_fmt_x100(SV.sun_v[0], la);
            if (lo != SH_NOLOC) sv_fmt_x100(SV.sun_v[1], lo);
            SV.sun_edit = 1; SV.sun_f = 0; SV.shift = 0; SV.num = 1;
            kb_focus(KB_DIGITS | KB_ASCII, "done", "");    /* B3 */
            sv_rebuild();
        } else if (SV.fix_have && x >= GROUP_X && x < CONTENT_R && y >= SUN_LOC + 47 && y < SUN_LOC + 47 + SUN_FIX_ROW) {
            sun_fill_fix();                 /* helper J: the editor with the fix's place in its fields: Done saves it */
            SV.sun_edit = 1; SV.sun_f = 0; SV.shift = 0; SV.num = 1;
            kb_focus(KB_DIGITS | KB_ASCII, "done", "");    /* B3 */
            toast("Filled in from the GPS fix");
            sv_rebuild();
        }
        return;
    }
    int save = DONE_HIT(x, y);
    if (!save && SV_CANCEL_HIT(x, y)) { SV.sun_edit = 0; sv_rebuild(); return; }
    if (SV.fix_have && y >= SUN_FIX_BTN && y < SUN_FIX_BTN + 44 && x >= GROUP_X && x < CONTENT_R) {
        sun_fill_fix();                     /* helper J: Use GPS Fix */
        sh_haptic();
        sun_fields();
        relink();
        return;
    }
    if (y >= SUN_ED && y < SUN_ED + 93 && x >= GROUP_X && x < CONTENT_R) { SV.sun_f = y >= SUN_ED + 47; sv_rebuild(); return; }
    int k = save ? 5 : y >= 540 ? sv_kb_tap(x, y, SV.sun_v[SV.sun_f], 9, 1) : 0;
    if (k == 5) {
        int la, lo;
        if (!sv_parse_x100(SV.sun_v[0], &la) || !sv_parse_x100(SV.sun_v[1], &lo) || la < -9000 || la > 9000 || lo < -18000 || lo > 18000) {
            toast("Latitude -90..90, longitude -180..180");
            return;
        }
        sh_loc_set(la, lo);
        toast("Location saved");
        SV.sun_edit = 0;
        sv_rebuild();
    } else if (k == 1) { sun_fields(); relink(); }
}

/* ---- Guide --------------------------------------------------------------------------------------------------------- */
#define GD_LIST (CONTENT_TOP + 46)      /* the chapters' group, under the two-line note */
#define GD_PAGES PUSHED_TOP             /* a chapter's pages (a pushed page) */
#define GD_TITLE PUSHED_TOP             /* a page's title */
#define GD_TEXT_TOP (PUSHED_TOP + 46)
#define GD_TEXT_BOT 690.0f
#define GD_NAV 706.0f
#define GD_BTN 120.0f                   /* Previous / Next */
static float guide_body(dc *d, const char *const *ln, float size, float top)
{
    float lhh = size * 1.38f, y = top;
    for (int i = 0; ln && ln[i]; i++) {
        int warn = ln[i][0] == '!' && ln[i][1] == ' ';
        const char *s = warn ? ln[i] + 2 : ln[i];
        if (warn && d) d_rrect(d, MARGIN - 2, y + 2, 3, lhh * sv_text(0, TK_IT600, size, 0, 0, 340, lhh, s, INK) - 4, 1.5f, SVC(SV_WARN));
        y += lhh * sv_text(d, warn ? TK_IT600 : TK_IT400, size, MARGIN + 6, y, CONTENT_W - 6, lhh, s, warn ? SVC(SV_WARN) : W(.88f)) + size * .7f;
    }
    return y;
}
static int guide_total(void)
{
    int t = 0, n = sh_guide_n(), np;
    for (int c = 0; c < n; c++) { (void)sh_guide_chapter(c, 0, 0, &np); t += np; }
    return t;
}
static void page_guide(dc *d)
{
    const char *sub;
    unsigned argb;
    int np, n = sh_guide_n();
    if (SV.gch < 0) {
        d_h1(d, "Survival Guide");
        sv_text(d, TK_IT400, 13, MARGIN, CONTENT_TOP, CONTENT_W, 18, "Offline first aid and survival. Not a substitute for training: call 112 if any network works.", W(.55f));
        d_rrect(d, GROUP_X, GD_LIST, GROUP_W, 62.0f * n - 1, 16, SVC(SV_GROUP));
        for (int c = 0; c < n; c++) {
            float y = GD_LIST + 62.0f * c, tx = GROUP_X + 58;
            const char *t = sh_guide_chapter(c, &sub, &argb, &np);
            d_rrect(d, GROUP_X + 14, y + 16, 30, 30, 8, tk_hex(argb & 0xffffff, 1));
            char num[4];
            put_dec(num, c + 1);
            float w = tw(TK_IT700, 15, num, 0, 0);
            d_text(d, TK_IT700, 15, GROUP_X + 29 - w / 2, bl(TK_IT700, 15, y + 22), num, 0, 0, W(1), 0);
            d_text_box(d, TK_IT600, 17, tx, bl(TK_IT600, 17, y + 11), CONTENT_R - 36 - tx, 0, t, 0, 0, INK, 0);
            d_text_box(d, TK_IT400, 13, tx, bl(TK_IT400, 13, y + 34), CONTENT_R - 36 - tx, 0, sub, 0, 0, W(.55f), 0);
            d_icon(d, "chev", CONTENT_R - 30, y + 24, .45f);
            if (c + 1 < n) d_rrect(d, tx, y + 61, CONTENT_R - tx, .5f, 0, W(.08f));
        }
        char b[48];
        int k = put_dec(b, guide_total());
        sv_cat(b, k, " pages, kept on the phone");
        d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, GD_LIST + 62.0f * n + 12), b, 0, 0, W(.4f), 0);
        return;
    }
    const char *ct = sh_guide_chapter(SV.gch, &sub, &argb, &np);
    if (SV.gpg < 0) {
        sv_back(d, ct);
        d_rrect(d, GROUP_X, GD_PAGES, GROUP_W, 47.0f * np - 1, 16, SVC(SV_GROUP));
        for (int p = 0; p < np; p++) {
            float y = GD_PAGES + 47.0f * p;
            d_text_box(d, TK_IT400, 16, ROW_TX, y + 29, CONTENT_R - 36 - ROW_TX, 0, sh_guide_page(SV.gch, p, 0), 0, 0, INK, 0);
            d_icon(d, "chev", CONTENT_R - 30, y + 16.5f, .45f);
            if (p + 1 < np) d_rrect(d, ROW_TX, y + 46, CONTENT_R - ROW_TX, .5f, 0, W(.08f));
        }
        return;
    }
    const char *const *ln = 0;
    const char *pt = sh_guide_page(SV.gch, SV.gpg, &ln);
    sv_back(d, ct);                                    /* the chapter inline; the page's title in its colour's bar */
    d_rrect(d, MARGIN, GD_TITLE + 4, 4, 28, 2, tk_hex(argb & 0xffffff, 1));
    d_text_box(d, TK_IT700, 28, MARGIN + 14, bl(TK_IT700, 28, GD_TITLE), CONTENT_W - 14, 0, pt, 0, 0, INK, 0);
    int k;
    float size = 16;
    while (size > 12 && guide_body(0, ln, size, GD_TEXT_TOP) > GD_TEXT_BOT) size -= .5f;
    guide_body(d, ln, size, GD_TEXT_TOP);
    /* Previous / Next (across chapters), where we are */
    int first = SV.gch == 0 && SV.gpg == 0, last = SV.gch == n - 1 && SV.gpg == np - 1;
    d_rrect(d, MARGIN, GD_NAV, GD_BTN, 46, 23, W(first ? .05f : .12f));
    d_rrect(d, CONTENT_R - GD_BTN, GD_NAV, GD_BTN, 46, 23, W(last ? .05f : .12f));
    d_text(d, TK_IT600, 16, MARGIN + GD_BTN / 2 - tw(TK_IT600, 16, "Previous", 0, 0) / 2, bl(TK_IT600, 16, GD_NAV + 13), "Previous", 0, 0, W(first ? .3f : 1), 0);
    d_text(d, TK_IT600, 16, CONTENT_R - GD_BTN / 2 - tw(TK_IT600, 16, "Next", 0, 0) / 2, bl(TK_IT600, 16, GD_NAV + 13), "Next", 0, 0, W(last ? .3f : 1), 0);
    char b[16];
    k = put_dec(b, SV.gpg + 1);
    k = sv_cat(b, k, " of ");
    put_dec(b + k, np);
    d_text(d, TK_IT400, 14, 195 - tw(TK_IT400, 14, b, 0, 0) / 2, bl(TK_IT400, 14, GD_NAV + 14), b, 0, 0, W(.5f), 0);
}
static void guide_tap(float x, float y)
{
    int np, n = sh_guide_n();
    if (SV.gch < 0) {
        if (x >= GROUP_X && x < CONTENT_R && y >= GD_LIST && y < GD_LIST + 62.0f * n) { SV.gch = (int)((y - GD_LIST) / 62); SV.gpg = -1; sv_rebuild(); }
        return;
    }
    (void)sh_guide_chapter(SV.gch, 0, 0, &np);
    if (SV.gpg < 0) {
        if (sv_back_hit(x, y)) { SV.gch = -1; sv_rebuild(); return; }
        if (x >= GROUP_X && x < CONTENT_R && y >= GD_PAGES && y < GD_PAGES + 47.0f * np) { SV.gpg = (int)((y - GD_PAGES) / 47); sv_rebuild(); }
        return;
    }
    if (sv_back_hit(x, y)) { SV.gpg = -1; sv_rebuild(); return; }
    if (y < GD_NAV - 6 || y >= GD_NAV + 52) return;
    if (x < MARGIN + GD_BTN + 10) {
        if (SV.gpg > 0) SV.gpg--;
        else if (SV.gch > 0) { SV.gch--; (void)sh_guide_chapter(SV.gch, 0, 0, &np); SV.gpg = np - 1; }
        else return;
    } else if (x >= CONTENT_R - GD_BTN - 10) {
        if (SV.gpg + 1 < np) SV.gpg++;
        else if (SV.gch + 1 < n) { SV.gch++; SV.gpg = 0; }
        else return;
    } else return;
    sv_rebuild();
}

/* ---- Compass / Level (helper J): the sensor hub on demand (kern/survival.c on shub.c) ------------------------------
 * As iOS's Compass: a dial turning with the heading (a tick every 2 degrees, the 30s' numbers outside, N E S W inside,
 * north's tick red), the fixed lubber line at the top, the heading big under it ("275° W"); a Level view (flat: two
 * circles; held upright: the horizon; green when level); the steps since the hub started. The hub starts at the page's
 * open (~4 s: "Starting..."), is kept alive every 20 s while the page shows and stopped at its close. Its values are
 * read at 10 Hz only while the page shows (sv_refresh / sv_next_update, no frames for that); frames run only while the
 * dial eases to a new reading. The dial is one layer drawn again per change, in place: the ticks per px over their ring
 * only (in 8 bands), the labels rendered once at the open and put upright; the readout, the status and the steps are
 * small layers drawn in place when their words change. */
#define CP_CX 195.0f
#define CP_CY 318.0f
#define CP_BOX 146.0f                   /* the dial's layer: the centre +- this */
#define CP_TO 122.0f                    /* the ticks' outer radius */
#define CP_TI 102.0f                    /* the longest tick's inner end (north's) */
#define CP_NUM_R 136.0f                 /* the numbers' centres */
#define CP_LET_R 82.0f                  /* the letters' centres */
#define CP_SEG_X 95.0f                  /* Compass | Level */
#define CP_SEG_Y 52.0f
#define CP_SEG_W 200.0f
#define CP_RO_X 45.0f                   /* the readout's layer: the heading big, a line under it */
#define CP_RO_Y 474.0f
#define CP_RO_W 300.0f
#define CP_RO_H 86.0f
#define CP_ST_Y 566.0f                  /* the status line's layer (Starting..., Calibrate..., why) */
#define CP_ST_H 64.0f
#define CP_STEPS 650.0f                 /* the steps' group */
#define CP_SV_W 140.0f                  /* the steps' value layer */
#define CP_LV_R 70.0f                   /* the level's circles */
#define CP_LV_K 4.0f                    /* pt per degree of tilt */
#define CP_HZ_R 128.0f                  /* the horizon's window */
#define CPL_N 16                        /* the labels: the numbers 0, 30 .. 330, then N E S W */
static struct {
    int view;                           /* 0 compass, 1 level */
    int on, refused;                    /* the hub asked for (stopped at the close); refused (Last 2 %, the self test) */
    int state;                          /* SH_SENS_* at the last poll */
    float next, keep;                   /* the next poll; the last start / keep-alive (now_ms) */
    int hv, acc, tv, sv;                /* heading / tilt / steps valid */
    float hdg, hshow, hdrawn;           /* degrees: the reading, shown (eased), drawn */
    float pitch, roll, pshow, rshow, pdrawn, rdrawn;
    unsigned steps;
    int drawn_lit, drawn_view;          /* the dial as drawn (-1 never) */
    char ro[48], ro2[96], st[200], stp[24];
    int st_warn;
    tk_bmp lab[CPL_N];
    float lab_cx[CPL_N], lab_cy[CPL_N]; /* a label's centre in its bitmap, px */
} CP = { .drawn_view = -1 };

static int cp_visible(void) { return AP.on && !AP.closing && is_app("compass") && !ap_pending && !sh_sleeping(); }
static float cp_wrap(float d) { while (d > 180) d -= 360; while (d < -180) d += 360; return d; }
static uint32_t cp_pm(uint32_t rgb, float a)
{
    unsigned A = (unsigned)(a * 255 + .5f);
    return A << 24 | ((rgb >> 16 & 255) * A / 255) << 16 | ((rgb >> 8 & 255) * A / 255) << 8 | (rgb & 255) * A / 255;
}
/* a small layer drawn again in place (made once at the open: no new bitmap, no relink per change) */
static dc cp_clear(tk_layer *l)
{
    if (l->bmp.px) tk_memset(l->bmp.px, 0, (size_t)l->bmp.stride * l->bmp.h * 4);
    dc d = { &l->bmp, 0, l->x, l->y };
    return d;
}
static void cp_labels_free(void)
{
    for (int i = 0; i < CPL_N; i++) tk_bmp_free(&CP.lab[i]);
}
static void cp_label(int i, const char *s, int f, float size)
{
    float w = PT(tw(f, size, s, 0, 0)), a = tk_ascent(f, PT(size)), de = tk_descent(f, PT(size));
    tk_bmp *b = &CP.lab[i];
    if (!tk_bmp_new(b, (int)w + 8, (int)(a + de) + 8)) return;
    tk_paint p = tk_solid(W(1));
    tk_text(b, f, PT(size), 4, 4 + tk_floorf(a + .5f), s, 0, 0, &p, 0);
    CP.lab_cx[i] = 4 + w / 2;
    CP.lab_cy[i] = 4 + tk_floorf(a + .5f) - PT(size) * .36f;     /* the middle of a capital or a digit */
}
static void cp_labels(void)
{
    cp_labels_free();
    char b[8];
    for (int i = 0; i < 12; i++) { put_dec(b, 30 * i); cp_label(i, b, TK_IT400, 14); }
    static const char *const L[4] = { "N", "E", "S", "W" };
    for (int i = 0; i < 4; i++) cp_label(12 + i, L[i], TK_IT400, 26);
}

/* the dial: the ticks' ring per px (each band clears its own rows first) */
typedef struct { tk_bmp *b; float cx, cy, hdg, dim; } cp_job;
static void cp_tick_rows(void *ctx, int y0, int y1)
{
    const cp_job *J = ctx;
    tk_bmp *b = J->b;
    float ro = PT(CP_TO), ri = PT(CP_TI);
    for (int j = y0; j < y1; j++) {
        uint32_t *row = b->px + (size_t)j * b->stride;
        tk_memset(row, 0, (size_t)b->w * 4);
        float py = j + .5f - J->cy, ay = tk_fabsf(py);
        if (ay > ro + 1) continue;
        float xo = tk_sqrtf(tk_maxf(0, (ro + 1) * (ro + 1) - py * py));
        float xi = ay < ri - 1 ? tk_sqrtf((ri - 1) * (ri - 1) - py * py) : 0;
        for (int s = 0; s < 2; s++) {
            int a0 = (int)tk_floorf(s ? J->cx + xi : J->cx - xo), a1 = (int)tk_floorf(s ? J->cx + xo : J->cx - xi) + 1;
            a0 = tk_maxi(a0, 0);
            a1 = tk_mini(a1, b->w);
            for (int i = a0; i < a1; i++) {
                float px = i + .5f - J->cx, r = tk_sqrtf(px * px + py * py);
                if (r < ri - 1 || r > ro + 1) continue;
                float m = (tk_atan2f(px, -py) * 57.2957795f + J->hdg) * .5f, k = tk_floorf(m + .5f);
                float dist = tk_fabsf(m - k) * 2 * .0174532925f * r;          /* to the nearest tick, px along the ring */
                int deg = (int)k * 2 % 360;
                if (deg < 0) deg += 360;
                float hw, rin, a;
                if (!deg) { hw = PT(1.5f); rin = ri; a = 1; }
                else if (deg % 30 == 0) { hw = PT(.9f); rin = PT(CP_TO - 16); a = 1; }
                else if (deg % 10 == 0) { hw = PT(.6f); rin = PT(CP_TO - 12); a = .7f; }
                else { hw = PT(.5f); rin = PT(CP_TO - 9); a = .42f; }
                float c = tk_clampf(hw - dist + .5f, 0, 1) * tk_clampf(r - rin + .5f, 0, 1) * tk_clampf(ro - r + .5f, 0, 1) * a * J->dim;
                if (c > 0) row[i] = cp_pm(deg ? 0xffffffU : SV_RED, c);
            }
        }
    }
}
/* the level: flat, two circles (one fixed, one moved by the tilt); upright, the horizon in a round window */
typedef struct { tk_bmp *b; float cx, cy, mx, my, ang, dim; int upright, level; } cp_ljob;
static void cp_level_rows(void *ctx, int y0, int y1)
{
    const cp_ljob *J = ctx;
    tk_bmp *b = J->b;
    uint32_t col = J->level ? 0x30d158U : 0xffffffU;
    float R = PT(CP_LV_R), HR = PT(CP_HZ_R), sw = PT(1), rim = PT(1.4f);
    float sn = tk_sinf(J->ang * .0174532925f), cs = tk_cosf(J->ang * .0174532925f);
    for (int j = y0; j < y1; j++) {
        uint32_t *row = b->px + (size_t)j * b->stride;
        tk_memset(row, 0, (size_t)b->w * 4);
        float py = j + .5f - J->cy;
        int x0 = 0, x1 = b->w;
        if (J->upright) {
            if (tk_fabsf(py) > HR + 2) continue;
            float h = tk_sqrtf(tk_maxf(0, (HR + 2) * (HR + 2) - py * py));
            x0 = tk_maxi(0, (int)(J->cx - h)); x1 = tk_mini(b->w, (int)(J->cx + h) + 1);
        }
        for (int i = x0; i < x1; i++) {
            float px = i + .5f - J->cx, a;
            if (J->upright) {
                float r = tk_sqrtf(px * px + py * py), in = tk_clampf(HR - r + .5f, 0, 1);
                if (in <= 0 && r > HR + 2) continue;
                float sd = -px * sn + py * cs;                                /* > 0: under the horizon */
                float below = tk_clampf(sd + .5f, 0, 1) * in, line = tk_clampf(PT(1.2f) - tk_fabsf(sd) + .5f, 0, 1) * in;
                float ring = tk_clampf(sw - tk_fabsf(r - HR) + .5f, 0, 1);
                a = 1 - (1 - below * .2f) * (1 - line) * (1 - ring * .35f);
            } else {
                float r1 = tk_sqrtf(px * px + py * py), qx = px - J->mx, qy = py - J->my, r2 = tk_sqrtf(qx * qx + qy * qy);
                float fill = tk_clampf(R - r2 + .5f, 0, 1), ring1 = tk_clampf(sw - tk_fabsf(r1 - R) + .5f, 0, 1);
                float ring2 = tk_clampf(rim - tk_fabsf(r2 - R) + .5f, 0, 1);
                if (fill <= 0 && ring1 <= 0 && ring2 <= 0) continue;
                a = 1 - (1 - fill * .2f) * (1 - ring1 * .45f) * (1 - ring2 * .95f);
            }
            if (a > 0) row[i] = cp_pm(col, a * J->dim);
        }
    }
}
static int cp_lit(void) { return CP.view ? CP.tv : CP.hv; }
static int cp_upright(void) { return tk_fabsf(CP.pshow) > 50; }
static int cp_level_now(void)
{
    if (!CP.tv) return 0;
    if (cp_upright()) return tk_fabsf(CP.rshow) < 1;
    return CP.pshow * CP.pshow + CP.rshow * CP.rshow < 1;
}
static void cp_dial(void)
{
    tk_layer *l = &svl[SVL_DIAL];
    if (!l->bmp.px) return;
    int lit = cp_lit();
    float dim = lit ? 1 : .3f, cx = PT(CP_CX) - l->x, cy = PT(CP_CY) - l->y;
    if (!CP.view) {
        cp_job J = { &l->bmp, cx, cy, lit ? CP.hshow : 0, dim };
        tk_par_rows(0, l->bmp.h, cp_tick_rows, &J);
        for (int i = 0; i < CPL_N; i++) {               /* upright, where their degree is now */
            const tk_bmp *s = &CP.lab[i];
            if (!s->px) continue;
            float deg = i < 12 ? 30.0f * i : 90.0f * (i - 12), r = PT(i < 12 ? CP_NUM_R : CP_LET_R);
            float a = (deg - (lit ? CP.hshow : 0)) * .0174532925f;
            float x = cx + r * tk_sinf(a) - CP.lab_cx[i], y = cy - r * tk_cosf(a) - CP.lab_cy[i];
            tk_blit(&l->bmp, s, (int)tk_floorf(x + .5f), (int)tk_floorf(y + .5f), (i < 12 ? .8f : 1) * dim);
        }
        dc d = { &l->bmp, 0, l->x, l->y };              /* the lubber line: fixed, at the top */
        d_rrect(&d, CP_CX - 1.5f, CP_CY - CP_TO - 6, 3, 30, 1.5f, W(lit ? 1 : .4f));
        CP.hdrawn = CP.hshow;
    } else {
        int up = lit && cp_upright();
        float mx = 0, my = 0;
        if (lit && !up) {                                 /* the bubble goes to the high side, as a spirit level's */
            mx = CP.rshow * PT(CP_LV_K); my = -CP.pshow * PT(CP_LV_K);
            float m = tk_sqrtf(mx * mx + my * my), lim = PT(CP_BOX - CP_LV_R - 4);
            if (m > lim) { mx *= lim / m; my *= lim / m; }
        }
        cp_ljob J = { &l->bmp, cx, cy, mx, my, lit ? CP.rshow : 0, dim, up, cp_level_now() };
        tk_par_rows(0, l->bmp.h, cp_level_rows, &J);
        CP.pdrawn = CP.pshow; CP.rdrawn = CP.rshow;
    }
    CP.drawn_lit = lit;
    CP.drawn_view = CP.view;
    tk_layer_dirty(l);
}
static void cp_seg(void)
{
    tk_layer *l = &svl[SVL_SEG];
    dc d = cp_clear(l);
    if (!l->bmp.px) return;
    static const char *const S[2] = { "Compass", "Level" };
    float w = CP_SEG_W / 2 - 2;
    d_rrect(&d, CP_SEG_X, CP_SEG_Y, CP_SEG_W, 32, 9, W(.1f));
    d_rrect(&d, CP_SEG_X + 2 + w * CP.view, CP_SEG_Y + 2, w, 28, 7, tk_hex(0x5a3a3c, 1));
    for (int i = 0; i < 2; i++) {
        float tx = tw(TK_IT600, 13, S[i], 0, 0);
        d_text(&d, TK_IT600, 13, CP_SEG_X + 2 + w * i + (w - tx) / 2, bl(TK_IT600, 13, CP_SEG_Y + 7.5f), S[i], 0, 0, i == CP.view ? INK : W(.65f), 0);
    }
    tk_layer_dirty(l);
}
/* the words: the readout (the heading or the tilt big, a line under it), the status, the steps; 1 if any drawn */
static int cp_texts(void)
{
    char ro[48], ro2[96], st[200], stp[24];
    int n, warn = 0, lit = cp_lit();
    ro[0] = ro2[0] = st[0] = 0;
    if (CP.refused) {
        sv_cat(ro, 0, "Compass Unavailable");
        sv_cat(st, 0, sh_last2() ? "Not available in Last 2 %. It comes back when Last 2 % is off." : "The sensors can't start now.");
        warn = 1;
    } else if (CP.state == SH_SENS_FAILED) {
        sv_cat(ro, 0, "Compass Unavailable");
        n = 0;
        const char *w = sh_sensors_why();
        while (w && *w && n < 150) st[n++] = *w++;
        st[n] = 0;
        sv_cat(st, n, n ? " Tap here to try again." : "The sensors didn't start. Tap here to try again.");
        warn = 1;
    } else if (!lit) {
        sv_cat(ro, 0, CP.state == SH_SENS_RUNNING ? "Waiting for the Sensors\xe2\x80\xa6" : "Starting\xe2\x80\xa6");
        sv_cat(st, 0, CP.state == SH_SENS_RUNNING ? "" : "The sensors take about 4 seconds to start.");
    } else if (!CP.view) {
        int h = (int)tk_floorf(CP.hshow + .5f) % 360;
        static const char *const D8[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
        n = put_dec(ro, h);
        n = sv_cat(ro, n, "\xc2\xb0 ");
        sv_cat(ro, n, D8[(int)((CP.hshow + 22.5f) / 45) & 7]);
        n = sv_cat(ro2, 0, "Magnetic north");
        cp_fix_line(ro2, n);
        if (CP.acc < 2) { sv_cat(st, 0, "Calibrate: move the phone in a figure-8"); warn = 2; }
    } else {
        int up = cp_upright(), lv = cp_level_now();
        float v = up ? tk_fabsf(CP.rshow) : tk_sqrtf(CP.pshow * CP.pshow + CP.rshow * CP.rshow);
        n = put_dec(ro, lv ? 0 : tk_maxi(1, (int)(v + .5f)));
        sv_cat(ro, n, "\xc2\xb0");
        if (lv) sv_cat(ro2, 0, "Level");
        else if (up) sv_cat(ro2, 0, "Upright: the horizon's tilt");
        else {
            n = sv_cat(ro2, 0, "Front ");
            n += put_dec(ro2 + n, (int)(CP.pshow + (CP.pshow < 0 ? -.5f : .5f)));
            n = sv_cat(ro2, n, "\xc2\xb0 \xc2\xb7 side ");
            n += put_dec(ro2 + n, (int)(CP.rshow + (CP.rshow < 0 ? -.5f : .5f)));
            sv_cat(ro2, n, "\xc2\xb0");
        }
    }
    if (CP.sv) {
        unsigned v = CP.steps;
        n = 0;
        if (v >= 1000) { n += put_dec(stp, (int)(v / 1000)); stp[n++] = ','; n = sv_2(stp, n, (int)(v % 1000 / 10)); stp[n++] = (char)('0' + v % 10); stp[n] = 0; }
        else put_dec(stp, (int)v);
    } else sv_cat(stp, 0, "\xe2\x80\x94");
    int any = 0;
    if (tk_strcmp(ro, CP.ro) || tk_strcmp(ro2, CP.ro2)) {
        tk_memcpy(CP.ro, ro, sizeof ro); tk_memcpy(CP.ro2, ro2, sizeof ro2);
        tk_layer *l = &svl[SVL_TXT];
        dc d = cp_clear(l);
        if (l->bmp.px) {
            int big = lit && !CP.refused && CP.state != SH_SENS_FAILED;
            float sz = big ? 54 : 24, w = tw(big ? TK_IT400 : TK_IT600, sz, ro, 0, big ? TK_TNUM : 0);
            d_text(&d, big ? TK_IT400 : TK_IT600, sz, 195 - w / 2, bl(big ? TK_IT400 : TK_IT600, sz, big ? CP_RO_Y + 2 : CP_RO_Y + 18), ro, 0, big ? TK_TNUM : 0,
                   CP.view && cp_level_now() ? tk_hex(0x30d158, 1) : INK, 0);
            if (ro2[0]) sv_centre(&d, TK_IT400, 15, CP_RO_Y + 64, ro2, W(.6f));
            tk_layer_dirty(l);
        }
        any = 1;
    }
    if (tk_strcmp(st, CP.st) || warn != CP.st_warn) {
        tk_memcpy(CP.st, st, sizeof st);
        CP.st_warn = warn;
        tk_layer *l = &svl[SVL_A];
        dc d = cp_clear(l);
        if (l->bmp.px && st[0]) {
            char ln[4][96];
            float wm;
            int k = wrapn(ln, 3, TK_IT400, 15, st, CONTENT_W - 20, &wm);
            for (int i = 0; i < k; i++) sv_centre(&d, TK_IT400, 15, CP_ST_Y + 4 + 20.0f * i, ln[i], warn == 1 ? SVC(SV_WARN) : warn == 2 ? SVC(SV_TINT) : W(.55f));
        }
        tk_layer_dirty(l);
        any = 1;
    }
    if (tk_strcmp(stp, CP.stp)) {
        tk_memcpy(CP.stp, stp, sizeof stp);
        tk_layer *l = &svl[SVL_B];
        dc d = cp_clear(l);
        if (l->bmp.px) {
            float w = tw(TK_IT400, 17, stp, 0, TK_TNUM);
            d_text(&d, TK_IT400, 17, CONTENT_R - 16 - w, CP_STEPS + 13 + 16, stp, 0, TK_TNUM, W(.6f), 0);
        }
        tk_layer_dirty(l);
        any = 1;
    }
    if (lit != CP.drawn_lit || CP.view != CP.drawn_view) { cp_dial(); any = 1; }
    return any;
}
static void cp_start(float t)
{
    CP.keep = t;
    CP.refused = sh_sensors_on(1) < 0;
    CP.on = 1;                                      /* stopped at the close either way (a refused start is a no-op) */
}
/* 10 Hz while the page shows: the hub kept alive (every 20 s; again at once if it stopped, every 2 s while refused;
 * not after a failure: a tap tries again), the readings as targets (a dead band against the hub's jitter) */
static int cp_poll(float t)
{
    CP.next = t + 100;
    int st = sh_sensors_state();
    if (CP.refused) { if (t - CP.keep >= 2000 || t < CP.keep) cp_start(t); }
    else if (st != SH_SENS_FAILED && (st == SH_SENS_OFF || t - CP.keep >= 20000 || t < CP.keep)) cp_start(t);
    CP.state = sh_sensors_state();
    int d10 = 0, acc = 0, p10 = 0, r10 = 0;
    unsigned n = 0;
    int hv = sh_heading(&d10, &acc), tv = sh_tilt(&p10, &r10), sv = sh_steps(&n);
    if (hv) {
        float h = d10 / 10.0f;
        if (!CP.hv) CP.hdg = CP.hshow = h;
        else if (tk_fabsf(cp_wrap(h - CP.hdg)) >= .6f) CP.hdg = h;
        CP.acc = acc;
    }
    if (tv) {
        float p = p10 / 10.0f, r = r10 / 10.0f;
        if (!CP.tv) { CP.pitch = CP.pshow = p; CP.roll = CP.rshow = r; }
        if (tk_fabsf(p - CP.pitch) >= .3f) CP.pitch = p;
        if (tk_fabsf(r - CP.roll) >= .3f) CP.roll = r;
    }
    CP.hv = hv; CP.tv = tv; CP.sv = sv;
    if (sv) CP.steps = n;
    return cp_texts();
}
static int cp_moving(void)
{
    if (!cp_visible()) return 0;
    if (!CP.view) return CP.hv && tk_fabsf(cp_wrap(CP.hdg - CP.hshow)) > .15f;
    return CP.tv && (tk_fabsf(CP.pitch - CP.pshow) > .15f || tk_fabsf(CP.roll - CP.rshow) > .15f);
}
/* a frame: the shown values ease to the readings (as iOS's dial follows); the dial drawn again when it moved */
static int cp_step(float dt)
{
    if (!cp_visible()) return 0;
    float k = 1 - tk_expf(-dt / .09f);
    int mv = cp_moving();
    if (!CP.view && CP.hv) {
        float d = cp_wrap(CP.hdg - CP.hshow);
        CP.hshow = tk_fabsf(d) > .15f ? CP.hshow + d * k : CP.hdg;
        if (CP.hshow < 0) CP.hshow += 360;
        if (CP.hshow >= 360) CP.hshow -= 360;
        if (tk_fabsf(cp_wrap(CP.hshow - CP.hdrawn)) >= .1f) cp_dial();
    } else if (CP.view && CP.tv) {
        float dp = CP.pitch - CP.pshow, dr = CP.roll - CP.rshow;
        CP.pshow = tk_fabsf(dp) > .15f ? CP.pshow + dp * k : CP.pitch;
        CP.rshow = tk_fabsf(dr) > .15f ? CP.rshow + dr * k : CP.roll;
        if (tk_fabsf(CP.pshow - CP.pdrawn) >= .1f || tk_fabsf(CP.rshow - CP.rdrawn) >= .1f) cp_dial();
    }
    cp_texts();
    return mv;
}
static void page_compass(dc *d)
{
    /* the crosshair at the centre (under the dial's layer: its middle is clear) */
    d_rrect(d, CP_CX - 16, CP_CY - .75f, 32, 1.5f, .75f, W(.4f));
    d_rrect(d, CP_CX - .75f, CP_CY - 16, 1.5f, 32, .75f, W(.4f));
    d_rrect(d, GROUP_X, CP_STEPS, GROUP_W, 47, 16, SVC(SV_GROUP));
    d_text(d, TK_IT400, 17, ROW_TX, CP_STEPS + 13 + 16, "Steps", 0, 0, INK, 0);
    sv_text(d, TK_IT400, 13, ROW_TX, CP_STEPS + 56, CONTENT_R - 16 - ROW_TX, 18,
            "Counted since the sensors started. Compass shows magnetic north; the sensors stop when you leave.", W(.45f));
}
static void compass_extras(void)
{
    cp_labels();
    dc d = mk(&svl[SVL_DIAL], "sv-dial", CP_CX - CP_BOX, CP_CY - CP_BOX, 2 * CP_BOX, 2 * CP_BOX, 0, 0);
    (void)d;
    d = mk(&svl[SVL_SEG], "sv-seg", CP_SEG_X, CP_SEG_Y, CP_SEG_W, 32, 1, 0);
    d = mk(&svl[SVL_TXT], "sv-cp-ro", CP_RO_X, CP_RO_Y, CP_RO_W, CP_RO_H, 0, 0);
    d = mk(&svl[SVL_A], "sv-cp-st", GROUP_X, CP_ST_Y, GROUP_W, CP_ST_H, 0, 0);
    d = mk(&svl[SVL_B], "sv-cp-steps", CONTENT_R - 16 - CP_SV_W, CP_STEPS + 6, CP_SV_W, 34, 0, 0);
    if (!SV.fix_asked) sun_fix_ask();               /* the coordinates under the heading */
    CP.ro[0] = CP.ro2[0] = CP.st[0] = CP.stp[0] = 1;  /* (no string is \1: every layer drawn now) */
    CP.ro[1] = CP.ro2[1] = CP.st[1] = CP.stp[1] = 0;
    CP.drawn_view = -1;
    cp_seg();
    if (!CP.on) cp_start(now_ms);
    cp_poll(now_ms);
}
static void compass_tap(float x, float y)
{
    if (y >= CP_SEG_Y - 6 && y < CP_SEG_Y + 38 && x >= CP_SEG_X && x < CP_SEG_X + CP_SEG_W) {
        int v = x >= CP_SEG_X + CP_SEG_W / 2;
        if (v == CP.view) return;
        CP.view = v;
        CP.hshow = CP.hdg; CP.pshow = CP.pitch; CP.rshow = CP.roll;
        cp_seg();
        cp_texts();
        return;
    }
    if (CP.state == SH_SENS_FAILED && !CP.refused && y >= CP_ST_Y - 8 && y < CP_ST_Y + CP_ST_H) {
        cp_start(now_ms);
        toast("Starting the sensors again");
        cp_poll(now_ms);
    }
}
static void compass_close(void)
{
    if (CP.on) { sh_sensors_on(0); CP.on = 0; }
    CP.refused = 0;
    CP.hv = CP.tv = CP.sv = 0;
    CP.state = SH_SENS_OFF;
}

/* ---- the hooks shell_lock.c calls -------------------------------------------------------------------------------- */
static int sv_page(const char *k, dc *d)
{
    if (!tk_strcmp(k, "sos")) page_sos(d);
    else if (!tk_strcmp(k, "medical")) page_medical(d);
    else if (!tk_strcmp(k, "power")) page_last2(d);
    else if (!tk_strcmp(k, "usb")) page_usb(d);
    else if (!tk_strcmp(k, "sun")) page_sun(d);
    else if (!tk_strcmp(k, "guide")) page_guide(d);
    else if (!tk_strcmp(k, "compass")) page_compass(d);
    else return 0;
    return 1;
}
static void sv_free(void)
{
    for (int i = 0; i < SVL_N; i++) { relayer(&svl[i]); svl[i].hidden = 1; }
    sv_ring_free();
    cp_labels_free();
    SV.hold = 0;
}
/* the page's live layers (at its open and after a sv_rebuild): the previous page's go first */
static void sv_extras(const char *k)
{
    sv_free();
    if (!tk_strcmp(k, "sos")) sos_extras();
    else if (!tk_strcmp(k, "power")) last2_extras();
    else if (!tk_strcmp(k, "usb")) usb_extras();
    else if (!tk_strcmp(k, "sun") && SV.sun_edit) sun_fields();
    else if (!tk_strcmp(k, "compass")) compass_extras();    /* helper J: starts the sensor hub */
    else if (!tk_strcmp(k, "settings") && AS.sub == 4 && SV.cf >= 0) sv_field(ROW_TX, CE_FLD + 10, CONTENT_R - 16 - ROW_TX, SV.ebuf, 1, INK);
}
/* an app opened (app_open): every tool opens on its first view */
static void sv_opened(void)
{
    SV.gch = SV.gpg = -1;
    SV.cf = -1;
    SV.sun_edit = 0;
    SV.hold = 0;
    SV.fix_asked = 0;                   /* helper J: the GPS fix asked again at the page's build */
    if (CP.on) compass_close();         /* (another app straight over the Compass: its hub off) */
    CP.view = 0;
}
/* the app closed (app_finish_close): over the lock, the lock is as it was */
static void sv_closed(void)
{
    if (CP.on) compass_close();         /* helper J: the sensor hub off when the Compass closes */
    sv_free();
    if (SV.from_lock) { SV.from_lock = 0; if (locked) set_lock(lockY); }
}
static void sv_settings_card(void) { AS.sub = 4; SV.cf = -1; sv_rebuild(); }
/* Settings' Medical Card row, where build_app_page draws it (0: none); *sec its group */
static float sv_card_row(int *sec)
{
    const appdef *A = &APPS[AP.a];
    for (int s = 0; s < A->ns; s++)
        for (int i = 0; i < A->sec[s].n; i++)
            if (!tk_strcmp(A->sec[s].rows[i].l, "Medical Card")) { if (sec) *sec = s; return gen_row_y(A, s, i); }
    return 0;
}
static float sv_card_row_y(void) { return sv_card_row(0); }
/* survival-next (E's bands): what lights under a finger on a tool's page (app_press): the back, a list's row, a key; 0 none */
static int sv_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    int kb = (is_app("settings") && AS.sub == 4 && SV.cf >= 0) || (is_app("sun") && SV.sun_edit);
    if (kb) {
        if (y >= 548 && kb_rect(x, y, KR[SV.num], !SV.num, r)) { *g = *r; *rad = 6; *c = W(.35f); return 1; }
        /* 8c: the editors' Cancel (a word) and Done (the glass check), and Sun's two rows and its GPS button */
        if (DONE_HIT(x, y)) { *r = *g = (tk_rect){ CONTENT_R - BACK_D, SAFE_TOP, BACK_D, BACK_D }; *rad = BACK_D / 2; *c = W(.16f); return 1; }
        if (SV_CANCEL_HIT(x, y)) return sep_word(r, g, rad, c, MARGIN, tw(TK_IT400, 17, "Cancel", 0, 0));
        if (is_app("sun") && x >= GROUP_X && x < CONTENT_R) {
            if (SV.fix_have && y >= SUN_FIX_BTN && y < SUN_FIX_BTN + 44) { *r = *g = (tk_rect){ GROUP_X, SUN_FIX_BTN, GROUP_W, 44 }; *rad = 22; *c = W(.16f); return 1; }
            if (y >= SUN_ED && y < SUN_ED + 94) { *r = (tk_rect){ GROUP_X, SUN_ED + (y < SUN_ED + 47 ? 0 : 47), GROUP_W, 47 }; *g = (tk_rect){ GROUP_X, SUN_ED, GROUP_W, 93 }; *rad = 16; *c = W(.1f); return 1; }
        }
        return 0;
    }
    /* 8c (the films' SBTK_TTF: the tools' own controls showed nothing until the lift): switches' tracks, SOS's beacon
     * choices, USB's Stop, Sun's place, the Guide's Previous / Next and back, the Compass's halves */
    if (AP.on && !is_app("settings")) {
        const char *k = APPS[AP.a].k;
        tk_rect q = { 0, 0, 0, 0 };
        float qr = 0;
        int hit = 0, in = x >= GROUP_X && x < CONTENT_R;
        if (!tk_strcmp(k, "sos") && in && y >= SOS_ROW && y < SOS_ROW + 94) { q = (tk_rect){ SV_SW_X, (y < SOS_ROW + 47 ? SOS_ROW : SOS_ROW + 47) + 8, 51, 31 }; qr = 15.5f; hit = 1; }
        else if (!tk_strcmp(k, "sos") && in && y >= SOS_SEG_Y && y < SOS_SEG_Y + 36) {
            int b = (int)((x - GROUP_X - 2) / SOS_SEG_W);
            b = b < 0 ? 0 : b > 3 ? 3 : b;
            q = (tk_rect){ GROUP_X + 2 + SOS_SEG_W * b, SOS_SEG_Y + 2, SOS_SEG_W, 32 }; qr = 9; hit = 1;
        }
        else if (!tk_strcmp(k, "power") && in && y >= L2_ROW && y < L2_ROW + 94) { q = (tk_rect){ SV_SW_X, (y < L2_ROW + 47 ? L2_ROW : L2_ROW + 47) + 8, 51, 31 }; qr = 15.5f; hit = 1; }
        else if (!tk_strcmp(k, "usb") && SV.usb.state == SH_USB_ON && x >= 24 && x < 366 && y >= 690 && y < 756) { q = (tk_rect){ 24, 690, 342, 66 }; qr = 33; hit = 1; }
        else if (!tk_strcmp(k, "sun") && x >= CONTENT_R - 120 && y >= SUN_LOC && y < SUN_LOC + 47) { q = (tk_rect){ GROUP_X, SUN_LOC, GROUP_W, 47 }; qr = 16; hit = 2; }
        else if (!tk_strcmp(k, "sun") && SV.fix_have && in && y >= SUN_LOC + 47 && y < SUN_LOC + 47 + SUN_FIX_ROW) { q = (tk_rect){ GROUP_X, SUN_LOC + 47, GROUP_W, SUN_FIX_ROW }; qr = 16; hit = 2; }
        else if (!tk_strcmp(k, "guide") && SV.gpg >= 0 && BACK_HIT(x, y)) { q = (tk_rect){ BACK_X, SAFE_TOP, BACK_D, BACK_D }; qr = BACK_D / 2; hit = 1; }
        else if (!tk_strcmp(k, "guide") && SV.gpg >= 0 && y >= GD_NAV - 6 && y < GD_NAV + 52) {
            if (x < MARGIN + GD_BTN + 10) { q = (tk_rect){ MARGIN, GD_NAV, GD_BTN, 46 }; qr = 23; hit = 1; }
            else if (x >= CONTENT_R - GD_BTN - 10) { q = (tk_rect){ CONTENT_R - GD_BTN, GD_NAV, GD_BTN, 46 }; qr = 23; hit = 1; }
        }
        else if (!tk_strcmp(k, "compass") && y >= CP_SEG_Y - 6 && y < CP_SEG_Y + 38 && x >= CP_SEG_X && x < CP_SEG_X + CP_SEG_W) {
            q = (tk_rect){ CP_SEG_X + (x >= CP_SEG_X + CP_SEG_W / 2 ? CP_SEG_W / 2 : 0), CP_SEG_Y, CP_SEG_W / 2, 32 }; qr = 16; hit = 1;
        }
        if (hit) { *r = *g = q; *rad = qr; *c = hit == 2 ? W(.1f) : W(.16f); return 1; }
    }
    if (((is_app("settings") && AS.sub == 4) || (is_app("guide") && SV.gch >= 0)) && BACK_HIT(x, y)) {
        *r = *g = (tk_rect){ BACK_X, SAFE_TOP, BACK_D, BACK_D };
        *rad = BACK_D / 2; *c = W(.16f);
        return 1;
    }
    float top = 0, rh = 0;
    int n = 0, np, sec = 0;
    if (is_app("settings") && AS.sub == 4) { top = CE_TOP; rh = 47; n = SH_CARD_NF; }
    else if (is_app("settings") && !AS.sub) { top = sv_card_row(&sec); rh = 47; n = top > 0; }
    else if (is_app("guide") && SV.gch < 0) { top = GD_LIST; rh = 62; n = sh_guide_n(); }
    else if (is_app("guide") && SV.gpg < 0) { (void)sh_guide_chapter(SV.gch, 0, 0, &np); top = GD_PAGES; rh = 47; n = np; }
    if (!n || x < GROUP_X || x >= CONTENT_R || y < top || y >= top + rh * n) return 0;
    int i = (int)((y - top) / rh);
    *r = (tk_rect){ GROUP_X, top + rh * i, GROUP_W, rh };
    *g = is_app("settings") && !AS.sub ? (tk_rect){ GROUP_X, gen_row_y(&APPS[AP.a], sec, 0), GROUP_W, 47.0f * APPS[AP.a].sec[sec].n - 1 }
                                       : (tk_rect){ GROUP_X, top, GROUP_W, rh * n - 1 };
    *rad = 16; *c = W(.1f);
    return 1;
}
/* a tap on an app's page: 1 if it was a tool's */
static int sv_tap(float x, float y)
{
    if (is_app("settings")) {
        if (AS.sub == 4) { card_tap(x, y); return 1; }
        float ry = sv_card_row_y();
        if (!AS.sub && ry > 0 && x >= GROUP_X && x < CONTENT_R && y >= ry && y < ry + 47) { sv_settings_card(); return 1; }   /* the row */
        return 0;
    }
    if (!AP.on || !sv_key(APPS[AP.a].k)) return 0;
    const char *k = APPS[AP.a].k;
    if (!tk_strcmp(k, "sos")) sos_tap(x, y);
    else if (!tk_strcmp(k, "power")) last2_tap(x, y);
    else if (!tk_strcmp(k, "usb")) usb_tap(x, y);
    else if (!tk_strcmp(k, "sun")) sun_tap(x, y);
    else if (!tk_strcmp(k, "guide")) guide_tap(x, y);
    else if (!tk_strcmp(k, "compass")) compass_tap(x, y);
    return 1;
}
/* which hold is under the finger: 1 SOS, 2 USB power (only while it can start) */
static int sv_hold_hit(float x, float y)
{
    if (is_app("sos")) { float dx = x - SOS_CX, dy = y - SOS_CY; return dx * dx + dy * dy <= (SOS_R + 8) * (SOS_R + 8) ? 1 : 0; }
    if (is_app("usb") && SV.usb.state == SH_USB_OFF) { float dx = x - 195, dy = y - USB_CY; return dx * dx + dy * dy <= (USB_R + 8) * (USB_R + 8) ? 2 : 0; }
    return 0;
}
static void sv_hold_end(void)
{
    SV.hold = 0;
    sv_ring_set(0);
}
/* every touch first: SOS's beacon owns the screen (only its Stop); a hold on a tool's button is followed here (the tap
 * that ends it goes on to app_tap, which ignores the button). 1: the touch is taken (sh_touch drops its gesture) */
static int sv_touch(int phase, float x, float y, float t)
{
    if (SV.sos_on) {
        int in = x >= 44 && x < 346 && y >= SOS_STOP_Y - 8 && y < SOS_STOP_Y + 72;
        if (phase == 0) SV.sos_down = in;
        {   /* 8c: the Stop lights while the finger is on it (it showed nothing until the lift) */
            int lit = phase != 2 && SV.sos_down && in;
            if (lit && soshl.hidden) {
                soshl.solid = tk_premul(TK_RGBA(255, 255, 255, .22f));
                soshl.x = PT(44); soshl.y = PT(SOS_STOP_Y);
                soshl.solid_w = PT(302); soshl.solid_h = PT(64);
                soshl.clip = 1;
                soshl.clip_r = (tk_rect){ PT(44), PT(SOS_STOP_Y), PT(302), PT(64) };
                soshl.clip_radius = PT(32);
            }
            if (soshl.hidden == lit) { soshl.hidden = !lit; soshl.gen++; }
        }
        if (phase == 2 && SV.sos_down && in) sv_sos_stop();
        return 1;
    }
    if (!AP.on || AP.closing || sp_app.live || ap_pending) return 0;
    if (phase == 0) {
        SV.hold = sv_hold_hit(x, y);
        SV.hold_done = 0;
        SV.hold_t0 = t; SV.hold_x = x; SV.hold_y = y;
        if (SV.hold && y >= H_PT - 30) SV.hold = 0;
    } else if (phase == 1) {
        if (SV.hold && (tk_fabsf(x - SV.hold_x) > 14 || tk_fabsf(y - SV.hold_y) > 14)) sv_hold_end();
    } else if (SV.hold) {
        if (!SV.hold_done) toast("Press and hold");
        sv_hold_end();
    } else if (SV.hold_done) {
        SV.hold_done = 0;
        return 1;                       /* the finger that held: no tap after it */
    }
    return 0;
}
static void sv_fire(int which)
{
    sv_hold_end();
    SV.hold_done = 1;
    sh_haptic();
    if (which == 1) {
        if (sh_sos_set(1) < 0) { toast("SOS can't start now"); return; }
        sv_sos_show();
        if (!SV.sos_on) toast("SOS can't start now");
    } else {
        if (sh_usb_start() < 0) {
            sh_usb(&SV.usb);
            toast(SV.usb.why ? "USB power refused: see why below" : "USB power can't start now");
        } else {
            sh_usb(&SV.usb);
            toast("USB power on: 10 s");
        }
        SV.usb_next = now_ms + 500;
        usb_controls();
        usb_text();
        relink();
    }
}
/* the polled values, from a frame or the platform's timer (sh_refresh): 1 if anything was redrawn */
static int sv_refresh(float t)
{
    int any = 0;
    if (SV.sos_on) {
        uint32_t c = 0;
        if (!sh_sos_frame(&c)) { sv_sos_hide(); any = 1; }
        else {
            if ((c | 0xff000000U) != sosbg.solid) { sosbg.solid = c | 0xff000000U; sosbg.gen++; any = 1; }
            int s = sh_sos_secs();
            if (s != SV.sos_secs) { SV.sos_secs = s; sv_sos_text(); relink(); any = 1; }
        }
    }
    if (AP.on && !AP.closing && is_app("usb") && SV.usb.state != SH_USB_OFF && t >= SV.usb_next) { usb_poll(t); any = 1; }
    if (cp_visible() && (t >= CP.next || t < CP.next - 200)) any |= cp_poll(t);   /* helper J: the sensors at 10 Hz */
    return any;
}
/* every frame (the end of sh_tick): the hold's ring, a page drawn over frames done, the polls; 1 to keep frames */
static int sv_tick(float dt, float t)
{
    (void)dt;
    int mv = 0;
    if (SV.rb && !ap_pending) { SV.rb = 0; if (AP.on && !AP.closing && !sp_app.live) appg.clip = 0; }
    if (SV.hold) {
        float p = tk_minf(1, (t - SV.hold_t0) / (SV.hold == 1 ? 1000.0f : 1500.0f));
        sv_ring_set(p);
        if (p >= 1) sv_fire(SV.hold);
        mv = 1;
    }
    sv_refresh(t);
    mv |= cp_step(dt);                  /* helper J: the dial eases to the reading */
    return mv;
}
static int sv_moving(void) { return SV.hold != 0 || cp_moving(); }
/* when the platform should come back without frames: SOS's beacon 20 times a second, USB power's state twice */
static float sv_next_update(float t)
{
    float best = -1;
    if (SV.sos_on) {
        /* acb417e: the kernel says when the beacon changes next; the countdown's seconds at most 1 s apart */
        int n = sh_sos_next();
        best = n > 0 ? tk_minf((float)n, 1000) : 50;
    }
    if (AP.on && !AP.closing && is_app("usb") && SV.usb.state != SH_USB_OFF) {
        float n = tk_maxf(1, SV.usb_next - t);
        if (best < 0 || n < best) best = n;
    }
    if (cp_visible()) {                 /* helper J: the Compass's next poll (10 Hz) */
        float n = tk_maxf(1, CP.next - t);
        if (best < 0 || n < best) best = n;
    }
    return best;
}
/* the host's stills: sv=sos:1 (the beacon), sv=usb:1 (a burst), sv=hold:p (the ring), sv=guide:CPP (chapter C,
 * page PP; 99 its list; -1 the chapters), sv=card:N (Settings > Medical Card: -1 the list, N a field), sv=sunedit:1 */
static void sv_set(const char *k, float v)
{
    if (!tk_strcmp(k, "sos")) {         /* again with a later hostms: the beacon then */
        if (v <= 0) sv_sos_stop();
        else if (!SV.sos_on) { sh_sos_set(1); sv_sos_show(); }
        else sv_refresh(now_ms);
        return;
    }
    if (!tk_strcmp(k, "usbpoll")) { usb_poll(now_ms); return; }
    /* helper J: sv=cpoll:1 the Compass's sensors read now (after hostms=), the dial where they are; sv=cview:1 Level */
    if (!tk_strcmp(k, "cpoll") || !tk_strcmp(k, "cview")) {
        if (!is_app("compass")) return;
        if (k[1] == 'v') { CP.view = v > 0; cp_seg(); }
        cp_poll(now_ms);
        CP.hshow = CP.hdg; CP.pshow = CP.pitch; CP.rshow = CP.roll;
        cp_dial();
        cp_texts();
        return;
    }
    if (!tk_strcmp(k, "hold")) {
        if (!SR_.n && AP.on && !tk_strcmp(APPS[AP.a].k, "usb")) usb_extras();
        sv_ring_set(v);
        return;
    }
    if (!tk_strcmp(k, "usb")) { if (v > 0) sv_fire(2); else { sh_usb_stop(); usb_poll(now_ms); } return; }
    if (!tk_strcmp(k, "guide")) { int c = (int)v; SV.gch = c < 0 ? -1 : c / 100; SV.gpg = c < 0 || c % 100 == 99 ? -1 : c % 100; }
    else if (!tk_strcmp(k, "card")) {
        if (!is_app("settings")) return;
        AS.sub = 4;
        SV.cf = (int)v;
        sh_card(&SV.card);
        if (SV.cf >= 0) { tk_memcpy(SV.ebuf, SV.card.f[SV.cf], sizeof SV.ebuf); SV.shift = 0; SV.num = SV.cf >= 5 && SV.cf <= 7; }
    } else if (!tk_strcmp(k, "sunedit")) {
        int la, lo;
        sh_loc(&la, &lo);
        sv_fmt_x100(SV.sun_v[0], la);
        sv_fmt_x100(SV.sun_v[1], lo);
        SV.sun_edit = v > 0; SV.sun_f = 0; SV.num = 1; SV.shift = 0;
    } else return;
    if (AP.on) { build_app_page(AP.a); appg.clip = 0; }
}
/* the layers into the tree (the end of relink): the live layers after the page's strips, SOS's beacon under the
 * toast */
static void sv_relink(void)
{
    if (!svgrp.name) {
        tk_layer_init(&svgrp, "sv");
        for (int i = 0; i < SVL_N; i++) { tk_layer_init(&svl[i], "sv-live"); svl[i].hidden = 1; }
        tk_layer_init(&sosl, "sos");
        tk_layer_init(&sosbg, "sos-beacon");
        tk_layer_init(&sostx, "sos-text");
        sosl.hidden = 1;
    }
    tk_layer *k[SVL_N];
    for (int i = 0; i < SVL_N; i++) k[i] = &svl[i];
    link(&svgrp, k, SVL_N);
    tk_layer *p = appc.child;
    svgrp.next = 0;
    if (!p) appc.child = &svgrp;
    else { while (p->next) p = p->next; p->next = &svgrp; }
    if (!soshl.name) { tk_layer_init(&soshl, "sos-press"); soshl.hidden = 1; }
    tk_layer *s[] = { &sosbg, &sostx, &soshl };
    link(&sosl, s, 3);
    sosl.next = 0;
    for (p = root.child; p; p = p->next) if (p->next == &toastl) { sosl.next = &toastl; p->next = &sosl; break; }
}
