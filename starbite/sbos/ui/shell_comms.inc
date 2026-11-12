/* SPDX-License-Identifier: MIT
 * helper I: Phone and Messages on the modem session's calls and SMS (shell.h's sh_call ... sh_sms_*), as iOS 26 shows
 * them. Included by shell_lock.c after the other parts (it uses the shell's drawing helpers, layers and app state).
 *   - Phone: the keypad dials with a 0.6 s hold on the green button (a ring fills round it; a tap only says "Hold to
 *     call"). Its line under the number says what stands in the way: Cellular off (a tap opens Settings > Cellular),
 *     an emergency number (the kernel's own words; such a number is never sent down), the kernel's last refusal.
 *     Recents are the shell's call log (Track A: kept across restarts, the blob "calls"); a tap puts the number on
 *     the keypad;
 *   - the call screen over everything, the lock too: incoming (the number, Decline and Accept; over the lock a slide
 *     or a tap on the thumb answers), Calling / Ringing / the timer / Call Ended, Speaker (the route), Mute, the
 *     keypad's tones and the red End; "No audio yet" while the kernel says the call has no audio path. Its round
 *     buttons and the slider are made at idle while Cellular is on, so an incoming call shows at once;
 *   - Messages: the threads by number (the last message, its time, an unread dot), a thread in iOS's bubbles (blue
 *     sent, grey received) with Sending... / Sent / Not Delivered under the sent ones, the compose field over the
 *     keyboard, New Message with a To: field on the keyboard's 123 layer (with +). A received SMS posts a
 *     notification (the Notification Centre's store and its banner) unless its thread is open.
 * Nothing dials, answers or sends but the user's own hold or tap; no glass is baked here (the kept glass circles are
 * copied); numbers and texts go to the screen only, never to the log. */

#define CM_EMERGENCY "Emergency calls are not supported on Starbite OS: use another phone"
static void hl_set(tk_layer *l, tk_rect r, tk_rect clip, float rad, tk_col c);   /* shell_lock.c's press light */
static void hl_off(void);

/* ---- state ------------------------------------------------------------------------------------------------- */
static sh_call_t CALL;                     /* the platform's last push */
#define CM_NREC 20
enum { RC_OUT, RC_IN, RC_MISSED, RC_DECLINED, RC_NOANSWER };
typedef struct { char num[24]; int kind, secs, hm; } cm_rec;
static cm_rec RC[CM_NREC];                 /* Recents: the calls, newest first (Track A: kept, the blob "calls") */
static int nrc;
static int cm_missed_new;                  /* B8: missed calls not yet seen in Recents (home's Phone badge) */
/* the inbox: the platform's (newest last) with what the shell adds: when it was first seen, unread */
typedef struct { sh_sms_t m; int hm, day, unread; } cm_msg;
static cm_msg MSG[SH_SMS_MAX];
static int nmsg, msg_maxid, msg_syncs;
static struct {
    int own;                               /* this touch is ours: 1 the call screen, 2 the call button's hold */
    float down_t;                          /* the last touch-down (a held 0 types +) */
    int hold, hold_ok;                     /* the call button's hold (hold_ok: it can dial) */
    float hold_t0, hx, hy;
    char why[96];                          /* the keypad's red line: a refusal */
    int rec_on, rec_out, rec_conn, rec_secs, decl;   /* the call followed for Recents; decl: declined here */
    char rec_num[24];
    char thr[24], to[24], txt[480];        /* Messages: the open thread's number, New Message's To, the field */
    int focus, shift, num;                 /* 0 no keyboard, 1 To, 2 the message; the keyboard's shift and 123 */
    int want_sub, want_tab, want_view;     /* what an opening app shows (Settings > Cellular, a notification) */
    char want_thr[24];
} CM;
static tk_layer cmhint, cmring, cmfld, cmto;          /* in the app's page: the keypad's line, the hold's ring,
                                                       * the compose field, the To: field */

static void cm_cpy(char *d, int cap, const char *s)
{
    int i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    while (i > 0 && s[i] && ((unsigned char)d[i] & 0xc0) == 0x80) i--;   /* never half a character */
    d[i] = 0;
}
static int cm_digits(const char *n) { int c = 0; for (; *n; n++) c += *n >= '0' && *n <= '9'; return c; }
/* the kernel's rule, mirrored for the screen: these numbers (and any behind +CC / 00CC) are never sent down */
static int cm_emergency(const char *n)
{
    static const char *const E[] = { "112", "911", "999", "000", "110", "118", "119", "08" };
    char d[32];
    int k = 0;
    for (const char *p = n; *p && k < 31; p++) if ((*p >= '0' && *p <= '9') || *p == '+') d[k++] = *p;
    d[k] = 0;
    for (int s = -1; s < 3; s++) {                       /* no prefix, then a country code of 1..3 digits */
        int at = 0;
        if (s >= 0) {
            if (d[0] == '+') at = 2 + s;
            else if (d[0] == '0' && d[1] == '0') at = 3 + s;
            else break;
            if (at > k) break;
        }
        for (unsigned i = 0; i < sizeof E / sizeof E[0]; i++) if (!tk_strcmp(d + at, E[i])) return 1;
    }
    return 0;
}
static int cm_live(int st) { return st != SH_CALL_IDLE && st != SH_CALL_ENDED; }
static int cm_now_hm(void) { return ST.h * 100 + ST.mi; }
static void cm_hm(char *b, int hm)
{
    b[0] = (char)('0' + hm / 1000 % 10); b[1] = (char)('0' + hm / 100 % 10); b[2] = ':';
    b[3] = (char)('0' + hm / 10 % 10); b[4] = (char)('0' + hm % 10); b[5] = 0;
}
static int cm_cell_on(void) { return data_on(); }
static int cm_registered(void) { return CELL.data == SH_DATA_REGISTERED || CELL.data == SH_DATA_UP; }
static void cm_open_cellular(void);

/* ---- drawing helpers --------------------------------------------------------------------------------------- */
/* the handset (the pack's "call" glyph), turned down (135 degrees: End, Decline) or not, centred on (cx, cy) */
static void d_handset(dc *d, float cx, float cy, int down, tk_col c)
{
    const tk_bmp *ic = tk_icon("call");
    if (!ic) return;
    int s = (ic->w > ic->h ? ic->w : ic->h) * 3 / 2;
    tk_bmp a, b = { 0, 0, 0, 0 };
    if (!tk_bmp_new(&a, s, s)) return;
    tk_blit(&a, ic, (s - ic->w) / 2, (s - ic->h) / 2, 1);
    const tk_bmp *src = &a;
    if (down && tk_bmp_new(&b, s, s)) { tk_bmp_rotate(&b, &a, 135); src = &b; }
    tk_blit_tinted(d->b, src, (int)tk_floorf(PT(cx) - s / 2.0f - d->ox + .5f), (int)tk_floorf(PT(cy) - s / 2.0f - d->oy + .5f), c);
    tk_bmp_free(&b);
    tk_bmp_free(&a);
}
/* an arc in pt (a0 radians from 12 o'clock, clockwise) */
static void d_arc(dc *d, float cx, float cy, float r, float sw, float a0, float sweep, tk_col c)
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
/* the call screen's glyphs: 0 speaker, 1 mute (mic, slashed), 2 keypad dots */
static void d_cglyph(dc *d, int g, float cx, float cy, tk_col c)
{
    if (g == 0) {
        d_rrect(d, cx - 13, cy - 5, 6, 10, 1.5f, c);
        for (int k = 0; k <= 8; k++) {            /* the cone, in strokes */
            float x = cx - 7.5f + k * .9f, h = 5 + k * .75f;
            d_seg(d, x, cy - h, x, cy + h, 1.4f, c);
        }
        d_arc(d, cx, cy, 5.5f, 1.8f, .85f, 1.44f, c);
        d_arc(d, cx, cy, 10.5f, 1.8f, .75f, 1.64f, c);
    } else if (g == 1) {
        d_rrect(d, cx - 5, cy - 14, 10, 17, 5, c);
        d_arc(d, cx, cy - 4.5f, 9, 1.8f, 1.5708f, 3.1416f, c);
        d_seg(d, cx, cy + 5, cx, cy + 11, 1.8f, c);
        d_seg(d, cx - 5, cy + 11.5f, cx + 5, cy + 11.5f, 1.8f, c);
        d_seg(d, cx - 12, cy - 13, cx + 12, cy + 12, 2.2f, c);
    } else {
        for (int k = 0; k < 10; k++) {
            int r = k / 3, q = k % 3;
            if (k == 9) q = 1;
            float x = cx - 8.5f + 8.5f * q, y = cy - 12 + 8 * r;
            d_rrect(d, x - 2.6f, y - 2.6f, 5.2f, 5.2f, 2.6f, c);
        }
    }
}
/* an unknown caller's avatar: a grey disc with a person (iOS's) */
static void d_person(dc *d, float cx, float cy, float r)
{
    tk_stop st[2] = { { 0, tk_hex(0xa5a9b4, 1) }, { 1, tk_hex(0x7c808c, 1) } };
    tk_paint g = tk_linear(180, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), 2, st);
    tk_fill_rrect(d->b, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), PT(r), &g, 1);
    d_rrect(d, cx - r * .3f, cy - r * .58f, r * .6f, r * .6f, r * .3f, W(1));
    int bx = (int)tk_floorf(PT(cx - r) - d->ox), by = (int)tk_floorf(PT(cy - r) - d->oy), n = (int)PT(2 * r) + 2;
    tk_fm a, b;
    if (!tk_fm_new(&a, n, n)) return;
    if (!tk_fm_new(&b, n, n)) { tk_fm_free(&a); return; }
    tk_fm_circle(&a, d->ox + bx, d->oy + by, PT(cx), PT(cy + r * .78f), PT(r * .56f));
    tk_fm_circle(&b, d->ox + bx, d->oy + by, PT(cx), PT(cy), PT(r));
    tk_fm_mul(&a, &b, 0);
    tk_paint p = tk_solid(W(1));
    tk_draw_fm(d->b, &a, bx, by, &p, 1);
    tk_fm_free(&b);
    tk_fm_free(&a);
}
/* iOS's compose glyph (a square and a pencil), as Notes' */
static void d_compose(dc *d, float cx, float cy, tk_col c)
{
    float x0 = cx - 10, y0 = cy - 8, x1 = cx + 7, y1 = cy + 9;
    d_seg(d, x0 + 2, y0, x0 + 8, y0, 2, c);
    d_seg(d, x0, y0 + 2, x0, y1 - 2, 2, c);
    d_seg(d, x0, y0 + 2, x0 + 2, y0, 2, c);
    d_seg(d, x0, y1 - 2, x0 + 2, y1, 2, c);
    d_seg(d, x0 + 2, y1, x1 - 2, y1, 2, c);
    d_seg(d, x1 - 2, y1, x1, y1 - 2, 2, c);
    d_seg(d, x1, y1 - 2, x1, y0 + 9, 2, c);
    d_seg(d, x0 + 7, y0 + 10, x1 + 3, y0 - 6, 2.6f, c);
}
/* a greedy word wrap of a message into at most CM_ML lines of maxw (16 pt); '\n' breaks a line; a word wider than the
 * line breaks by character; text past the last line ends it with an ellipsis */
#define CM_ML 14
static int cm_wrap(char ln[CM_ML][100], const char *s, float maxw, float *wmax)
{
    int nl = 0, i = 0;
    *wmax = 0;
    while (s[i] && nl < CM_ML) {
        int st = i, end = i, any = 0, k = i;
        char t[100];
        for (;;) {
            int e = k;
            while (s[e] && s[e] != ' ' && s[e] != '\n') e++;
            int len = e - st > 99 ? 99 : e - st;
            tk_memcpy(t, s + st, (size_t)len);
            t[len] = 0;
            if (tw(TK_IT400, 16, t, 0, 0) > maxw || e - st > 99) {
                if (!any) {                       /* one word wider than the line: what fits of it */
                    int m = len;
                    while (m > 1) {
                        m--;
                        while (m > 1 && ((unsigned char)t[m] & 0xc0) == 0x80) m--;
                        t[m] = 0;
                        if (tw(TK_IT400, 16, t, 0, 0) <= maxw) break;
                    }
                    end = st + (m > 0 ? m : 1);
                }
                break;
            }
            any = 1;
            end = e;
            if (!s[e] || s[e] == '\n') break;
            k = e + 1;
        }
        if (end <= st) end = st + 1;
        int len = end - st > 99 ? 99 : end - st;
        tk_memcpy(ln[nl], s + st, (size_t)len);
        ln[nl][len] = 0;
        i = end;
        if (s[i] == '\n') i++;
        else while (s[i] == ' ') i++;
        if (nl == CM_ML - 1 && s[i]) {            /* the last line: the rest, cut at its tail */
            char e2[100];
            ellipsize(e2, sizeof e2, TK_IT400, 16, ln[nl], maxw - tw(TK_IT400, 16, "\xe2\x80\xa6", 0, 0));
            int q = (int)tk_strlen(e2);
            if (q < 96 && !(q >= 3 && (unsigned char)e2[q - 3] == 0xe2)) { e2[q] = (char)0xe2; e2[q + 1] = (char)0x80; e2[q + 2] = (char)0xa6; e2[q + 3] = 0; }
            tk_memcpy(ln[nl], e2, tk_strlen(e2) + 1);
        }
        float w = tw(TK_IT400, 16, ln[nl], 0, 0);
        if (w > *wmax) *wmax = w;
        nl++;
    }
    return nl;
}

/* ---- Phone: the keypad's line, the call button's hold, Recents ------------------------------------------------- */
#define PAD_CX 195.0f
#define PAD_CY 605.0f                       /* the call button's centre (78 pt) */
#define HINT_Y 134.0f                       /* the line under the number (two lines at most) */
static int hint_cell;                       /* the line is "Cellular is off" (its tap opens Settings > Cellular) */
static void cm_hint_build(void)
{
    relayer(&cmhint);
    hint_cell = 0;
    if (!is_app("phone") || AS.tab != 3) { cmhint.hidden = 1; return; }
    const char *s = 0;
    int red = 0;
    if (CM.why[0]) { s = CM.why; red = 1; }
    else if (AS.num[0] && cm_emergency(AS.num)) { s = CM_EMERGENCY; red = 1; }
    else if (!sh_comms_ok()) s = "Calls aren't in this version yet";
    else if (!cm_cell_on()) hint_cell = 1;
    else if (!cm_registered()) s = md_text();               /* Starting, Searching, No SIM, SIM PIN on ... */
    if (!s && !hint_cell) { cmhint.hidden = 1; return; }
    dc d = mk(&cmhint, "dial-line", MARGIN, HINT_Y, CONTENT_W, 40, 2, 0);
    if (hint_cell) {
        /* "Cellular is off · Turn On in Settings", the second part in the accent (it opens Settings > Cellular) */
        const char *a = "Cellular is off \xc2\xb7 ", *b = "Turn On in Settings";
        float wa = tw(TK_IT400, 14, a, 0, 0), wb = tw(TK_IT600, 14, b, 0, 0), x = 195 - (wa + wb) / 2;
        d_text(&d, TK_IT400, 14, x, bl(TK_IT400, 14, HINT_Y + 9), a, 0, 0, C_LABEL2, 0);
        d_text(&d, TK_IT600, 14, x + wa, bl(TK_IT400, 14, HINT_Y + 9), b, 0, 0, ACCENT(), 0);
    } else {
        char ln[4][96];
        float wm;
        int n = wrapn(ln, 2, TK_IT400, 13, s, CONTENT_W - 20, &wm);
        for (int k = 0; k < n; k++)
            d_text_box(&d, TK_IT400, 13, 195, bl(TK_IT400, 13, HINT_Y + (n == 1 ? 9 : 2) + 17 * k), CONTENT_W, 1, ln[k], 0, 0,
                       red ? C_RED : C_LABEL2, 0);
    }
    trim(&cmhint);
    cmhint.hidden = 0;
}
/* the hold's ring round the call button: only the ring's pixels kept (their place, angle, coverage), drawn per frame */
static struct { int *idx; float *ang, *cov; int n; } CR;
static void cm_ring_free(void)
{
    tk_free(CR.idx); tk_free(CR.ang); tk_free(CR.cov);
    CR.idx = 0; CR.ang = CR.cov = 0; CR.n = 0;
    relayer(&cmring);
    cmring.hidden = 1;
}
static void cm_ring_build(void)
{
    cm_ring_free();
    float r = 45, sw = 3.5f, R = r + sw, cx = PAD_CX, cy = PAD_CY;
    dc d = mk(&cmring, "dial-ring", cx - R, cy - R, 2 * R, 2 * R, 1, 0);
    cmring.hidden = 1;
    if (!cmring.bmp.px) return;
    int n = 0, N = cmring.bmp.w * cmring.bmp.h;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < N; k++) {
            float px = k % cmring.bmp.w + .5f + d.ox - PT(cx), py = k / cmring.bmp.w + .5f + d.oy - PT(cy);
            float c = tk_clampf(.5f - (tk_fabsf(tk_sqrtf(px * px + py * py) - PT(r)) - PT(sw / 2)), 0, 1);
            if (c <= 0) continue;
            if (pass) {
                float a = tk_atan2f(px, -py);
                CR.idx[CR.n] = k; CR.ang[CR.n] = a < 0 ? a + 6.2831853f : a; CR.cov[CR.n] = c; CR.n++;
            } else n++;
        }
        if (!pass) {
            CR.idx = tk_alloc((size_t)n * 4); CR.ang = tk_alloc((size_t)n * 4); CR.cov = tk_alloc((size_t)n * 4);
            if (!CR.idx || !CR.ang || !CR.cov) { cm_ring_free(); return; }
        }
    }
    relink();
}
static void cm_ring_set(float p)
{
    if (!CR.n || !cmring.bmp.px) return;
    uint32_t col = 0xffffffffu;
    float sweep = 6.2831853f * p, aa = 1.0f / (cmring.bmp.w * .45f);
    for (int i = 0; i < CR.n; i++) {
        float e = tk_clampf((sweep - CR.ang[i]) / aa + .5f, 0, 1) * CR.cov[i];
        unsigned A = (unsigned)(e * 255 + .5f);
        cmring.bmp.px[CR.idx[i]] = A == 255 ? col : (A << 24 | A << 16 | A << 8 | A);
    }
    cmring.hidden = p <= 0;
    tk_layer_dirty(&cmring);
}
static void cm_refuse(const char *why)
{
    cm_cpy(CM.why, sizeof CM.why, why && why[0] ? why : "The call didn't start");
    cm_hint_build();
    relink();
}
static void cv_show(int mode);
/* the hold is full: the number goes to the kernel (never an emergency number: the screen says why instead) */
static void cm_dial(void)
{
    if (cm_emergency(AS.num)) { cm_refuse(CM_EMERGENCY); toast("Can't call this number"); return; }
    char why[SH_WHY] = "";
    if (sh_dial(AS.num, why) < 0) { cm_refuse(why); toast("The call didn't start"); return; }
    CM.why[0] = 0;
    cm_hint_build();
    if (!cm_live(CALL.state)) {               /* the platform's push follows; the screen now, as dialled */
        sh_call_t l;
        tk_memset(&l, 0, sizeof l);
        l.state = SH_CALL_DIALING; l.outgoing = 1; l.audio = SH_AUDIO_EARPIECE;
        cm_cpy(l.number, sizeof l.number, AS.num);
        sh_call(&l);
    }
}
/* the keypad's call button: the touch's start (ok: it may dial), its end */
static int cm_pad_ready(void)
{
    return AP.on && !AP.closing && !sp_app.live && !ap_pending && is_app("phone") && AS.tab == 3 && !sw_on() && !nc_on();
}
static void cm_hold_begin(float x, float y, float t)
{
    CM.hold_t0 = t; CM.hx = x; CM.hy = y;
    CM.hold_ok = AS.num[0] && sh_comms_ok() && cm_cell_on() && !cm_emergency(AS.num);
    tk_rect r = { PAD_CX - 39, PAD_CY - 39, 78, 78 };
    hl_set(&apphl, r, r, 39, W(.22f));
    if (CM.hold_ok) { CM.hold = 1; cm_ring_build(); cm_ring_set(0); }
}
static void cm_hold_end(void)
{
    CM.hold = 0;
    cm_ring_free();
    hl_off();
}
static void cm_hold_up(int moved)
{
    int was = CM.hold;
    cm_hold_end();
    if (moved) return;
    if (!AS.num[0]) toast("Enter a number");
    else if (cm_emergency(AS.num)) { cm_refuse(CM_EMERGENCY); }
    else if (!sh_comms_ok()) toast("Calls aren't in this version yet");
    else if (!cm_cell_on()) toast("Cellular is off: turn it on in Settings");
    else if (was) toast("Hold to call");
}

/* Track A: Recents kept across restarts in the blob "calls": version 1, the count, then each call's number (24 bytes),
 * kind (1), seconds (4) and hm (2) */
#define CM_REC 31
static void cm_rec_save(void)
{
    uint8_t b[2 + CM_NREC * CM_REC];
    b[0] = 1;
    b[1] = (uint8_t)nrc;
    for (int i = 0; i < nrc; i++) {
        uint8_t *p = b + 2 + i * CM_REC;
        tk_memcpy(p, RC[i].num, 24);
        p[24] = (uint8_t)RC[i].kind;
        for (int k = 0; k < 4; k++) p[25 + k] = (uint8_t)((unsigned)RC[i].secs >> (8 * k));
        p[29] = (uint8_t)RC[i].hm; p[30] = (uint8_t)((unsigned)RC[i].hm >> 8);
    }
    (void)sh_blob_put("calls", b, 2 + (unsigned)nrc * CM_REC);
}
static void cm_rec_restore(void)
{
    uint8_t b[2 + CM_NREC * CM_REC];
    int n = sh_blob_get("calls", b, sizeof b);
    if (n < 2 || b[0] != 1 || b[1] > CM_NREC || n < 2 + b[1] * CM_REC) return;
    nrc = b[1];
    for (int i = 0; i < nrc; i++) {
        const uint8_t *p = b + 2 + i * CM_REC;
        tk_memcpy(RC[i].num, p, 24);
        RC[i].num[23] = 0;
        RC[i].kind = p[24] <= RC_NOANSWER ? p[24] : RC_OUT;
        RC[i].secs = (int)(p[25] | p[26] << 8 | p[27] << 16 | (unsigned)p[28] << 24);
        RC[i].hm = (int16_t)(p[29] | p[30] << 8);
    }
}
/* Recents: a call followed from its first push to its end */
static void cm_rec_add(const char *num, int kind, int secs)
{
    if (nrc == CM_NREC) nrc--;
    for (int k = nrc; k > 0; k--) RC[k] = RC[k - 1];
    cm_cpy(RC[0].num, sizeof RC[0].num, num);
    RC[0].kind = kind; RC[0].secs = secs; RC[0].hm = cm_now_hm();
    nrc++;
    cm_rec_save();
}
static void cm_track(const sh_call_t *c)
{
    if (!CM.rec_on && cm_live(c->state)) {
        CM.rec_on = 1; CM.rec_out = c->outgoing; CM.rec_conn = 0; CM.rec_secs = 0;
        cm_cpy(CM.rec_num, sizeof CM.rec_num, c->number);
    }
    if (!CM.rec_on) return;
    if (c->number[0] && !CM.rec_num[0]) cm_cpy(CM.rec_num, sizeof CM.rec_num, c->number);
    if (c->state == SH_CALL_ACTIVE || c->state == SH_CALL_HELD) { CM.rec_conn = 1; CM.rec_secs = c->secs; }
    if (cm_live(c->state)) return;
    CM.rec_on = 0;
    int kind = CM.rec_out ? (CM.rec_conn ? RC_OUT : RC_NOANSWER) : CM.rec_conn ? RC_IN : CM.decl ? RC_DECLINED : RC_MISSED;
    cm_rec_add(CM.rec_num, kind, CM.rec_secs);
    if (kind == RC_MISSED) { sh_notify("phone", "Missed Call", CM.rec_num[0] ? CM.rec_num : "Unknown", cm_now_hm()); cm_missed_new++; }
    if (is_app("phone") && AS.tab == 1 && !AP.closing) build_app_page(AP.a);
}

static void phone_tabs(dc *d)
{
    static const char *const TK[4] = { "fav", "rec", "con", "pad" };
    static const char *const TL[4] = { "Favourites", "Recents", "Contacts", "Keypad" };
    d_lg_flat(d, GROUP_X, 718, GROUP_W, 62, 31, 1);   /* P3i: over the flat ground (the backdrop's blur cost ~40 ms a build) */
    for (int i = 0; i < 4; i++) {
        float bx = GROUP_X + TAB_W * i;
        char n[16] = "tab-";
        int k = 4;
        for (const char *c = TK[i]; *c; c++) n[k++] = *c;
        if (i == AS.tab) { n[k++] = '-'; n[k++] = 'o'; n[k++] = 'n'; }
        n[k] = 0;
        d_icon(d, n, bx + (TAB_W - 22) / 2, 730.5f, 1);
        float w = tw(TK_IT600, 10.5f, TL[i], 0, 0);
        d_text(d, TK_IT600, 10.5f, bx + (TAB_W - w) / 2, bl(TK_IT600, 10.5f, 754.5f), TL[i], 0, 0, i == AS.tab ? ACCENT() : C_LABEL2, 0);
    }
}
/* an empty state's bottom (d_empty's own layout), for a button under it */
static float cm_empty_bot(float area_top, float area_bot, const char *line)
{
    char ln[4][96];
    float wm;
    int n = line ? wrapn(ln, 3, TK_IT400, 15, line, 300, &wm) : 0;
    float h = 36 + 20.0f * n - (n ? 2 : 8);
    return tk_floorf((area_top + area_bot - h) / 2) + h;
}
#define CM_BTN_W 190.0f
static void cm_cell_button(dc *d, float y)
{
    d_rrect(d, 195 - CM_BTN_W / 2, y, CM_BTN_W, 40, 20, W(.12f));
    d_text_box(d, TK_IT600, 15, 195, bl(TK_IT600, 15, y + 11), CM_BTN_W - 20, 1, "Cellular Settings", 0, 0, ACCENT(), 0);
}
#define REC_H 60.0f
#define REC_MAX 9
static const char *cm_rec_word(int k)
{
    return k == RC_OUT ? "Outgoing" : k == RC_IN ? "Incoming" : k == RC_MISSED ? "Missed" : k == RC_DECLINED ? "Declined" : "No answer";
}
static int cm_dur(char *b, int s)
{
    /* the call's time as iOS: m:ss, h:mm:ss from an hour */
    int n = 0;
    if (s < 0) s = 0;
    if (s >= 3600) {
        n += put_dec(b + n, s / 3600);
        b[n++] = ':';
        b[n++] = (char)('0' + s % 3600 / 600);
        b[n++] = (char)('0' + s % 600 / 60);
    } else
        n += put_dec(b + n, s / 60);
    b[n++] = ':';
    b[n++] = (char)('0' + s % 60 / 10);
    b[n++] = (char)('0' + s % 10);
    b[n] = 0;
    return n;
}
static void page_phone(dc *d)
{
    if (AS.tab == 3) {
        for (int i = 0; i < 12; i++) {
            float x = 52 + 104.0f * (i % 3), y = 182 + 96.0f * (i / 3);
            d_rrect(d, x, y, 78, 78, 39, W(.14f));
            float dw = tw(TK_IT500, 32, KD[i], 0, 0);
            d_text(d, TK_IT500, 32, x + (78 - dw) / 2, y + 12 + tk_floorf(tk_ascent(TK_IT500, PT(32)) / TK_PT + .5f), KD[i], 0, 0, W(1), 0);
            if (KL[i][0]) {
                float lw = tw(TK_IT700, 10, KL[i], 1.4f, 0);
                d_text(d, TK_IT700, 10, x + (78 - lw) / 2, bl(TK_IT700, 10, y + 50), KL[i], 1.4f, 0, W(.75f), 0);
            }
        }
        d_rrect(d, PAD_CX - 39, PAD_CY - 39, 78, 78, 39, C_GREEN);
        d_icon(d, "call", PAD_CX - 15, PAD_CY - 15, 1);
    } else if (AS.tab == 1) {
        d_h1(d, "Recents");
        cm_missed_new = 0;                  /* B8: Recents shown: the missed calls are seen */
        if (!nrc) {
            const char *l = !sh_comms_ok() ? "Calls aren't in this version yet." :
                            cm_cell_on() ? "Calls you make and receive show here until the phone restarts." :
                            "Calls you make and receive show here. Turn on Cellular to call.";
            d_empty(d, CONTENT_TOP, EMPTY_TAB, "No Recent Calls", l);
            if (sh_comms_ok() && !cm_cell_on()) cm_cell_button(d, cm_empty_bot(CONTENT_TOP, EMPTY_TAB, l) + 18);
        } else {
            int n = nrc < REC_MAX ? nrc : REC_MAX;
            d_rrect(d, GROUP_X, CONTENT_TOP, GROUP_W, REC_H * n, 16, C_CARD);
            for (int i = 0; i < n; i++) {
                const cm_rec *r = &RC[i];
                float y = CONTENT_TOP + REC_H * i;
                char when[8], sub[48];
                cm_hm(when, r->hm);
                int k = 0;
                for (const char *w = cm_rec_word(r->kind); *w; w++) sub[k++] = *w;
                if ((r->kind == RC_OUT || r->kind == RC_IN) && r->secs > 0) {
                    const char *dot = " \xc2\xb7 ";
                    for (; *dot; dot++) sub[k++] = *dot;
                    k += cm_dur(sub + k, r->secs);
                }
                sub[k] = 0;
                float ww = tw(TK_IT400, 15, when, 0, TK_TNUM);
                d_text_box(d, TK_IT600, 17, ROW_TX, bl(TK_IT600, 17, y + 10), CONTENT_R - 16 - ww - 12 - ROW_TX, 0, r->num[0] ? r->num : "Unknown",
                           0, 0, r->kind == RC_MISSED ? C_RED : INK, 0);
                d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, y + 33), sub, 0, TK_TNUM, C_LABEL2, 0);
                d_text(d, TK_IT400, 15, CONTENT_R - 16 - ww, bl(TK_IT400, 15, y + 20), when, 0, TK_TNUM, C_LABEL2, 0);
                if (i + 1 < n) d_rrect(d, ROW_TX, y + REC_H - .5f, CONTENT_R - ROW_TX, .5f, 0, C_SEP);
            }
        }
    } else if (AS.tab == 2) {
        page_contacts(d);
    } else {
        d_h1(d, "Favourites");
        d_empty(d, CONTENT_TOP, EMPTY_TAB, "No Favourites", "Favourites come from Contacts, which can't be added on this phone yet.");
    }
    phone_tabs(d);
}
static void cm_phone_tap(float x, float y)
{
    if (y >= 718 && y <= 780 && x >= GROUP_X && x < GROUP_X + GROUP_W) {
        int t = tk_mini(3, (int)((x - GROUP_X) / TAB_W));
        if (t != AS.tab) { AS.tab = t; if (t != 3) AS.num[0] = 0; CM.why[0] = 0; build_app_page(AP.a); }
        return;
    }
    if (AS.tab == 1) {
        if (!nrc) {
            float by = cm_empty_bot(CONTENT_TOP, EMPTY_TAB, "Calls you make and receive show here. Turn on Cellular to call.") + 18;
            if (sh_comms_ok() && !cm_cell_on() && y >= by && y < by + 40 && tk_fabsf(x - 195) < CM_BTN_W / 2) cm_open_cellular();
            return;
        }
        int n = nrc < REC_MAX ? nrc : REC_MAX;
        if (x < GROUP_X || x >= CONTENT_R || y < CONTENT_TOP || y >= CONTENT_TOP + REC_H * n) return;
        const cm_rec *r = &RC[(int)((y - CONTENT_TOP) / REC_H)];
        if (!r->num[0]) return;
        /* iOS calls at once; here the number goes to the keypad and the hold still dials */
        AS.tab = 3;
        cm_cpy(AS.num, sizeof AS.num, r->num);
        CM.why[0] = 0;
        build_app_page(AP.a);
        return;
    }
    if (AS.tab != 3) return;
    if (hint_cell && y >= HINT_Y - 4 && y < HINT_Y + 34) { cm_open_cellular(); return; }
    int i = key_hit(x, y);
    int n = (int)tk_strlen(AS.num);
    if (i >= 0) {
        sh_haptic();
        char c = KD[i][0];
        if (i == 10 && !n && now_ms - CM.down_t >= 450) c = '+';   /* iOS: a held 0 types + */
        if (n < 20) { AS.num[n] = c; AS.num[n + 1] = 0; }
        CM.why[0] = 0;
        build_apnum();
        cm_hint_build();
        relink();
        return;
    }
    float ex = x - 299, ey = y - 605;
    if (n && ex * ex + ey * ey <= 30 * 30) {
        sh_haptic();
        AS.num[n - 1] = 0;
        CM.why[0] = 0;
        build_apnum();
        cm_hint_build();
        relink();
    }
}

/* ---- the call screen --------------------------------------------------------------------------------------- */
enum { CVM_NONE, CVM_INCOMING, CVM_CALL, CVM_ENDED };
enum { CVB_NONE, CVB_END, CVB_DEC, CVB_ACC, CVB_THUMB, CVB_SPK, CVB_MUTE, CVB_PAD, CVB_HIDE, CVB_KEY };
static tk_layer cvl, cvbg, cvsb, cvtop, cvst, cvctl, cvhide, cvdec, cvacc, cvend, cvtrbg, cvtrack, cvthumb, cvhl;
static struct {
    int on, mode, keypad, pre, answering, ending, pend_close, slide, btn, moved;
    float a, at, close_at, thumb, x0, y0, tx0;
    int d_state, d_secs, d_ok, d_audio, d_muted, d_mode, d_keypad;   /* what is drawn */
    char d_num[24], dtmf[24], d_why[48];
} CV = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -1, "", "", "" };
#define CV_GROUND 0xff141a2cU
#define CV_NUM_Y 150.0f                     /* the number's top (the avatar over it) */
#define CV_ROW_Y 470.0f                     /* Speaker, Mute, Keypad */
#define CV_KEY_Y 262.0f                     /* the tones' keypad (four rows of 96 pt) */
#define CV_END_Y 668.0f                     /* End, Decline, Accept: 78 pt circles */
#define CV_DEC_Y 566.0f                     /* Decline over the lock (the slider under it) */
#define CV_TR_X 40.0f
#define CV_TR_Y 682.0f
#define CV_TR_W 310.0f
#define CV_TR_H 72.0f
#define CV_TH_D 62.0f
#define CV_TH_MAX (CV_TR_W - CV_TH_D - 10)
static float cv_col_x(int i) { return 91 + 104.0f * i; }      /* a 78 pt circle's centre in a row of three */
static void cv_round(dc *d, float cx, float cy, tk_col c) { d_rrect(d, cx - 39, cy - 39, 78, 78, 39, c); }
/* the static parts: Decline, Accept, End, the slider's track and thumb (made at idle while Cellular is on: an incoming
 * call needs only its words drawn) */
static void cv_pre(void)
{
    if (CV.pre) return;
    uint64_t t0 = tk_us();
    dc d = mk(&cvdec, "cv-decline", 91 - 50, CV_END_Y, 100, 106, 2, 0);
    cv_round(&d, 91, CV_END_Y + 39, C_RED);
    d_handset(&d, 91, CV_END_Y + 39, 1, W(1));
    d_text_box(&d, TK_IT500, 13, 91, bl(TK_IT500, 13, CV_END_Y + 86), 96, 1, "Decline", 0, 0, W(.9f), 0);
    d = mk(&cvacc, "cv-accept", 299 - 50, CV_END_Y, 100, 106, 2, 0);
    cv_round(&d, 299, CV_END_Y + 39, C_GREEN);
    d_handset(&d, 299, CV_END_Y + 39, 0, W(1));
    d_text_box(&d, TK_IT500, 13, 299, bl(TK_IT500, 13, CV_END_Y + 86), 96, 1, "Accept", 0, 0, W(.9f), 0);
    d = mk(&cvend, "cv-end", 195 - 39, CV_END_Y, 78, 78, 2, 0);
    cv_round(&d, 195, CV_END_Y + 39, C_RED);
    d_handset(&d, 195, CV_END_Y + 39, 1, W(1));
    /* the track: a solid rounded layer (no bitmap), its words a small one */
    tk_layer_init(&cvtrbg, "cv-track");
    cvtrbg.solid = tk_premul(W(.14f));
    cvtrbg.x = PT(CV_TR_X); cvtrbg.y = PT(CV_TR_Y);
    cvtrbg.solid_w = PT(CV_TR_W); cvtrbg.solid_h = PT(CV_TR_H);
    cvtrbg.clip = 1;
    cvtrbg.clip_r = (tk_rect){ cvtrbg.x, cvtrbg.y, cvtrbg.solid_w, cvtrbg.solid_h };
    cvtrbg.clip_radius = PT(CV_TR_H / 2);
    cvtrbg.bx = cvtrbg.x; cvtrbg.by = cvtrbg.y;
    d = mk(&cvtrack, "cv-track-words", CV_TR_X + CV_TH_D + 10, CV_TR_Y + 18, CV_TR_W - CV_TH_D - 20, 36, 2, 0);
    d_text_box(&d, TK_IT400, 19, CV_TR_X + CV_TH_D + 10 + (CV_TR_W - CV_TH_D - 10) / 2, bl(TK_IT400, 19, CV_TR_Y + 24), CV_TR_W - CV_TH_D - 30, 1,
               "slide to answer", 0, 0, W(.75f), 0);
    d = mk(&cvthumb, "cv-thumb", CV_TR_X + 5, CV_TR_Y + 5, CV_TH_D, CV_TH_D, 2, 0);
    d_rrect(&d, CV_TR_X + 5, CV_TR_Y + 5, CV_TH_D, CV_TH_D, CV_TH_D / 2, C_GREEN);
    d_handset(&d, CV_TR_X + 5 + CV_TH_D / 2, CV_TR_Y + 5 + CV_TH_D / 2, 0, W(1));
    tk_layer *P[5] = { &cvdec, &cvacc, &cvend, &cvtrack, &cvthumb };
    for (int i = 0; i < 5; i++) { trim(P[i]); P[i]->hidden = 1; }
    cvtrbg.hidden = 1;
    tk_layer_prepare(&cvthumb);               /* it moves under the finger (the rest only fades, rarely) */
    CV.pre = 1;
    lap_us("call screen: its static parts made (idle)", tk_us() - t0);
}
static void cv_pre_free(void)
{
    tk_layer *P[5] = { &cvdec, &cvacc, &cvend, &cvtrack, &cvthumb };
    for (int i = 0; i < 5; i++) { relayer(P[i]); P[i]->hidden = 1; }
    CV.pre = 0;
}
static void cv_sb(void)
{
    relayer(&cvsb);
    dc d = mk(&cvsb, "cv-sb", 0, 0, 390, 44, 0, 0);
    char hm[8];
    fmt_hm(hm);
    status_bar(&d, hm);
    trim(&cvsb);
}
/* the number (or the tones typed), the avatar, "No audio yet" */
static void cv_top(void)
{
    relayer(&cvtop);
    dc d = mk(&cvtop, "cv-top", MARGIN, CV_NUM_Y - 92, CONTENT_W, 300, 2, 0);
    int pad = CV.keypad && CV.mode == CVM_CALL;
    const char *n = pad && CV.dtmf[0] ? CV.dtmf : CALL.number[0] ? CALL.number : "Unknown";
    if (!pad) d_person(&d, 195, CV_NUM_Y - 46, 38);
    d_text_box(&d, TK_IT500, 32, 195, bl(TK_IT500, 32, CV_NUM_Y), CONTENT_W, 1, n, 0, TK_TNUM, INK, 0);
    if (!CALL.audio_ok && CV.mode != CVM_ENDED) {
        /* the kernel says the call has no audio path: plainly, naming the missing part when it knows (fa4ec7f) */
        const char *t = CALL.audio_why[0] ? CALL.audio_why : "No audio yet";
        float pw = tw(TK_IT600, 13, t, 0, 0) + 24, py = CV_NUM_Y + (pad ? 74 : 84);
        d_rrect(&d, 195 - pw / 2, py, pw, 26, 13, sbca(SBC_ORANGE, .2f));
        d_text(&d, TK_IT600, 13, 195 - pw / 2 + 12, bl(TK_IT600, 13, py + 6), t, 0, 0, C_ORANGE, 0);
        if (!pad && !CALL.audio_why[0]) {
            char ln[4][96];
            float wm;
            int k = wrapn(ln, 3, TK_IT400, 14, "The call connects, but this phone's earpiece, speaker and microphone don't work in calls yet.", 300, &wm);
            for (int i = 0; i < k; i++)
                d_text_box(&d, TK_IT400, 14, 195, bl(TK_IT400, 14, py + 38 + 19 * i), CONTENT_W, 1, ln[i], 0, 0, C_LABEL2, 0);
        }
    }
    if (CALL.audio_ok && CALL.audio_why[0] && CV.mode != CVM_ENDED) {
        /* audio works but a part doesn't (the microphone): a small line */
        float py = CV_NUM_Y + (pad ? 74 : 84);
        d_text_box(&d, TK_IT400, 14, 195, bl(TK_IT400, 14, py + 4), CONTENT_W, 1, CALL.audio_why, 0, 0, C_ORANGE, 0);
    }
    trim(&cvtop);
    cm_cpy(CV.d_why, sizeof CV.d_why, CALL.audio_why);
    CV.d_ok = CALL.audio_ok;
    CV.d_keypad = CV.keypad;
    cm_cpy(CV.d_num, sizeof CV.d_num, n);
}
static void cv_status(void)
{
    char b[96];
    int k = 0;
    const char *s = 0;
    if (CV.mode == CVM_ENDED) s = "Call Ended";
    else if (CV.ending) s = "Ending\xe2\x80\xa6";
    else if (CV.answering && CALL.state == SH_CALL_INCOMING) s = "Connecting\xe2\x80\xa6";
    else switch (CALL.state) {
        case SH_CALL_DIALING: s = "Calling\xe2\x80\xa6"; break;
        case SH_CALL_ALERTING: s = "Ringing\xe2\x80\xa6"; break;
        case SH_CALL_INCOMING: s = "Incoming call"; break;
        case SH_CALL_HELD: s = "On Hold"; break;
        case SH_CALL_ENDING: s = "Ending\xe2\x80\xa6"; break;
        case SH_CALL_ACTIVE: k = cm_dur(b, CALL.secs); break;
        default: s = "Call Ended"; break;
        }
    if (s) { while (*s && k < 60) b[k++] = *s++; b[k] = 0; }
    if (CV.mode == CVM_ENDED && CALL.why[0]) {
        const char *dot = " \xc2\xb7 ";
        while (*dot) b[k++] = *dot++;
        for (const char *w = CALL.why; *w && k < 94; w++) b[k++] = *w;
        b[k] = 0;
    }
    relayer(&cvst);
    dc d = mk(&cvst, "cv-status", MARGIN, CV_NUM_Y + 42, CONTENT_W, 26, 2, 0);
    d_text_box(&d, TK_IT400, 17, 195, bl(TK_IT400, 17, CV_NUM_Y + 46), CONTENT_W, 1, b, 0, TK_TNUM, W(.7f), 0);
    trim(&cvst);
    CV.d_state = CALL.state; CV.d_secs = CALL.secs; CV.d_mode = CV.mode;
}
/* Speaker, Mute and Keypad (lit: white with a dark glyph), or the tones' keypad and Hide */
static void cv_ctl(void)
{
    relayer(&cvctl);
    relayer(&cvhide);
    cvhide.hidden = 1;
    if (CV.mode != CVM_CALL && CV.mode != CVM_ENDED) { cvctl.hidden = 1; return; }
    if (CV.keypad && CV.mode == CVM_CALL) {
        dc d = mk(&cvctl, "cv-keys", 52, CV_KEY_Y, 286, 366, 2, 0);
        for (int i = 0; i < 12; i++) {
            float x = 52 + 104.0f * (i % 3), y = CV_KEY_Y + 96.0f * (i / 3);
            d_rrect(&d, x, y, 78, 78, 39, W(.14f));
            float dw = tw(TK_IT500, 32, KD[i], 0, 0);
            d_text(&d, TK_IT500, 32, x + (78 - dw) / 2, y + 12 + tk_floorf(tk_ascent(TK_IT500, PT(32)) / TK_PT + .5f), KD[i], 0, 0, W(1), 0);
            if (KL[i][0]) {
                float lw = tw(TK_IT700, 10, KL[i], 1.4f, 0);
                d_text(&d, TK_IT700, 10, x + (78 - lw) / 2, bl(TK_IT700, 10, y + 50), KL[i], 1.4f, 0, W(.75f), 0);
            }
        }
        dc h = mk(&cvhide, "cv-hide", 299 - 40, CV_END_Y + 27, 80, 24, 2, 0);
        d_text_box(&h, TK_IT400, 17, 299, bl(TK_IT400, 17, CV_END_Y + 29), 80, 1, "Hide", 0, 0, INK, 0);
        cvhide.hidden = 0;
    } else {
        static const char *const L[3] = { "Speaker", "Mute", "Keypad" };
        dc d = mk(&cvctl, "cv-buttons", 40, CV_ROW_Y, 310, 104, 2, 0);
        for (int i = 0; i < 3; i++) {
            int lit = (i == 0 && CALL.audio == SH_AUDIO_SPEAKER) || (i == 1 && CALL.muted);
            float cx = cv_col_x(i), cy = CV_ROW_Y + 39;
            cv_round(&d, cx, cy, lit ? W(.92f) : W(.16f));
            d_cglyph(&d, i, cx, cy, lit ? tk_hex(0x141a2c, 1) : W(1));
            d_text_box(&d, TK_IT500, 13, cx, bl(TK_IT500, 13, CV_ROW_Y + 86), 100, 1, L[i], 0, 0, W(.9f), 0);
        }
    }
    trim(&cvctl);
    cvctl.hidden = 0;
    CV.d_audio = CALL.audio; CV.d_muted = CALL.muted; CV.d_keypad = CV.keypad;
}
/* where everything is for the mode (and the lock: the slider) */
static void cv_place(void)
{
    int inc = CV.mode == CVM_INCOMING, slider = inc && locked;
    tk_layer *P[5] = { &cvdec, &cvacc, &cvend, &cvtrack, &cvthumb };
    for (int i = 0; i < 5; i++) { P[i]->x = P[i]->bx; P[i]->y = P[i]->by; }
    cvdec.hidden = !inc || !CV.pre;
    cvacc.hidden = !inc || slider || !CV.pre;
    cvtrbg.hidden = cvtrack.hidden = cvthumb.hidden = !slider || !CV.pre;
    if (slider) {                             /* Decline over the slider, centred */
        cvdec.x = cvdec.bx + PT(195 - 91);
        cvdec.y = cvdec.by + PT(CV_DEC_Y - CV_END_Y);
        cvthumb.x = cvthumb.bx + tk_floorf(PT(CV.thumb) + .5f);
    }
    cvend.hidden = CV.mode != CVM_CALL || !CV.pre;
    cvctl.alpha = CV.mode == CVM_ENDED ? .35f : 1;
    cvtrack.alpha = 1 - tk_clampf(CV.thumb / (CV_TH_MAX * .5f), 0, 1);   /* the words fade under the thumb */
    cvl.alpha = CV.a;
    cvl.hidden = !CV.on || CV.a <= 0;
    for (int i = 0; i < 5; i++) P[i]->gen++;
}
static void cv_light(float cx, float cy, float r, tk_col c)
{
    tk_rect q = { cx - r, cy - r, 2 * r, 2 * r };
    hl_set(&cvhl, q, q, r, c);
}
static void cv_free(void)
{
    tk_layer *P[6] = { &cvsb, &cvtop, &cvst, &cvctl, &cvhide, &cvbg };
    for (int i = 0; i < 6; i++) { relayer(P[i]); P[i]->hidden = 1; }
    cvbg.solid = 0;
    cvhl.hidden = 1;
    CV.on = 0; CV.mode = CVM_NONE; CV.keypad = 0; CV.dtmf[0] = 0; CV.close_at = -1; CV.pend_close = 0;
    CV.slide = 0; CV.thumb = 0;
    cv_place();
    if (!cm_cell_on()) cv_pre_free();
    relink();
}
static void cv_show(int mode)
{
    uint64_t t0 = tk_us();
    int was = CV.on;
    if (!was) { CV.keypad = 0; CV.dtmf[0] = 0; CV.answering = CV.ending = 0; CV.thumb = 0; }
    CV.on = 1; CV.a = CV.at = 1;              /* at once: a call never waits for a fade */
    CV.pend_close = 0; CV.close_at = -1;
    CV.mode = mode;
    cv_pre();
    tk_layer_init(&cvbg, "cv-ground");
    cvbg.solid = CV_GROUND;
    cvbg.solid_w = TK_W;
    cvbg.solid_h = TK_H;
    cvbg.opaque = 1;
    if (!was || !cvsb.bmp.px) cv_sb();
    cvsb.hidden = 0;
    cv_top();
    cvtop.hidden = 0;
    cv_status();
    cvst.hidden = 0;
    cv_ctl();
    cvhl.hidden = 1;
    cv_place();
    relink();
    if (!was) lap_us(mode == CVM_INCOMING ? "call screen: incoming shown" : "call screen: shown", tk_us() - t0);
}
static void cv_hide(void)
{
    if (!CV.on) return;
    CV.at = 0;                                /* a .25 s fade (cm_tick), then everything goes */
    CV.close_at = -1; CV.pend_close = 0;
    cvhl.hidden = 1;
}
static void cv_ended(void)
{
    if (CV.mode == CVM_INCOMING) { cv_hide(); return; }   /* the caller gave up: no screen, a missed call */
    CV.mode = CVM_ENDED;
    CV.keypad = 0; CV.ending = 0;
    cv_top();
    cv_status();
    cv_ctl();
    cv_place();
    relink();
    CV.pend_close = 1;                        /* 1.5 s from the next look at the clock (cm_next_update) */
}
/* what changed since it was drawn */
static void cv_refresh(void)
{
    const char *n = CV.keypad && CV.mode == CVM_CALL && CV.dtmf[0] ? CV.dtmf : CALL.number[0] ? CALL.number : "Unknown";
    if (CALL.audio_ok != CV.d_ok || CV.keypad != CV.d_keypad || tk_strcmp(n, CV.d_num) || tk_strcmp(CALL.audio_why, CV.d_why)) cv_top();
    if (CALL.state != CV.d_state || CALL.secs != CV.d_secs || CV.mode != CV.d_mode || CV.ending || CV.answering) cv_status();
    if (CV.mode == CVM_CALL && (CALL.audio != CV.d_audio || CALL.muted != CV.d_muted || CV.keypad != CV.d_keypad)) cv_ctl();
    cv_place();
    relink();
}
void sh_call(const sh_call_t *c)
{
    sh_call_t o = CALL;
    CALL = *c;
    if (c->state != SH_CALL_INCOMING) CV.answering = 0;
    if (!cm_live(c->state) || c->state == SH_CALL_ENDING) CV.ending = 0;
    cm_track(c);
    if (CM.decl) {                            /* declined here: no screen for its last pushes (ENDING ...) */
        if (!cm_live(c->state)) CM.decl = 0;
        lock_ver++;
        return;
    }
    if (c->state == SH_CALL_INCOMING) {
        if (CV.on && CV.at > 0 && (CV.mode == CVM_INCOMING || CV.answering)) cv_refresh();
        else cv_show(CVM_INCOMING);
    } else {
        if (cm_live(c->state)) {
            if (CV.on && CV.at > 0 && CV.mode == CVM_CALL) cv_refresh();
            else cv_show(CVM_CALL);
        } else if (CV.on && CV.at > 0 && CV.mode != CVM_ENDED && cm_live(o.state)) cv_ended();
        else if (CV.on && CV.at > 0) cv_refresh();
    }
    lock_ver++;
}
/* the call screen's buttons under (x, y) */
static int cv_hit(float x, float y, int *key)
{
    float dx, dy;
#define IN_ROUND(cx, cy, r) (dx = x - (cx), dy = y - (cy), dx * dx + dy * dy <= (r) * (r))
    if (CV.mode == CVM_INCOMING) {
        if (locked) {
            if (IN_ROUND(195, CV_DEC_Y + 39, 44)) return CVB_DEC;
            if (x >= CV_TR_X + CV.thumb && x < CV_TR_X + CV.thumb + CV_TH_D + 10 && y >= CV_TR_Y - 6 && y < CV_TR_Y + CV_TR_H + 6) return CVB_THUMB;
            return CVB_NONE;
        }
        if (IN_ROUND(91, CV_END_Y + 39, 44)) return CVB_DEC;
        if (IN_ROUND(299, CV_END_Y + 39, 44)) return CVB_ACC;
        return CVB_NONE;
    }
    if (CV.mode != CVM_CALL) return CVB_NONE;
    if (IN_ROUND(195, CV_END_Y + 39, 44)) return CVB_END;
    if (CV.keypad) {
        if (x >= 260 && x < 340 && y >= CV_END_Y + 16 && y < CV_END_Y + 62) return CVB_HIDE;
        for (int i = 0; i < 12; i++)
            if (IN_ROUND(52 + 104.0f * (i % 3) + 39, CV_KEY_Y + 96.0f * (i / 3) + 39, 40)) { *key = i; return CVB_KEY; }
        return CVB_NONE;
    }
    for (int i = 0; i < 3; i++) if (IN_ROUND(cv_col_x(i), CV_ROW_Y + 39, 42)) return CVB_SPK + i;
    return CVB_NONE;
#undef IN_ROUND
}
static int cv_key;
static void cv_down(float x, float y)
{
    CV.x0 = x; CV.y0 = y; CV.moved = 0;
    CV.btn = cv_hit(x, y, &cv_key);
    if (CV.btn == CVB_THUMB) { CV.slide = 1; CV.tx0 = CV.thumb; return; }
    float cx = 0, cy = 0;
    switch (CV.btn) {
    case CVB_END: cx = 195; cy = CV_END_Y + 39; break;
    case CVB_DEC: cx = locked ? 195 : 91; cy = (locked ? CV_DEC_Y : CV_END_Y) + 39; break;
    case CVB_ACC: cx = 299; cy = CV_END_Y + 39; break;
    case CVB_SPK: case CVB_MUTE: case CVB_PAD: cx = cv_col_x(CV.btn - CVB_SPK); cy = CV_ROW_Y + 39; break;
    case CVB_KEY: cx = 52 + 104.0f * (cv_key % 3) + 39; cy = CV_KEY_Y + 96.0f * (cv_key / 3) + 39; break;
    default: return;
    }
    cv_light(cx, cy, 39, CV.btn == CVB_END || CV.btn == CVB_DEC || CV.btn == CVB_ACC ? TK_RGBA(0, 0, 0, .25f) : W(.22f));
}
static void cv_answer(void)
{
    char why[SH_WHY] = "";
    sh_haptic();
    if (sh_answer(why) < 0) { CV.thumb = 0; cv_place(); toast(why[0] ? why : "The call couldn't be answered"); return; }
    if (CALL.state == SH_CALL_INCOMING) CV.answering = 1;   /* "Connecting..." until the kernel says so */
    if (CV.mode != CVM_CALL) cv_show(CVM_CALL);
}
static void cv_up(void)
{
    cvhl.hidden = 1;
    if (CV.slide) {
        CV.slide = 0;
        if (CV.thumb >= CV_TH_MAX * .8f || !CV.moved) { CV.thumb = CV_TH_MAX; cv_place(); cv_answer(); }
        return;                               /* else it springs back (cm_tick) */
    }
    if (CV.moved) return;
    char why[SH_WHY] = "";
    switch (CV.btn) {
    case CVB_END:
        sh_haptic();
        sh_hangup();
        CV.ending = 1;
        cv_status();
        break;
    case CVB_DEC:
        sh_haptic();
        CM.decl = 1;                          /* Recents: declined, no missed-call notification */
        {
            int r = sh_hangup();
            if (!cm_live(CALL.state)) CM.decl = 0;                    /* over already (its push came) */
            else if (r < 0) { CM.decl = 0; toast("The call couldn't be declined"); break; }
        }
        cv_hide();
        break;
    case CVB_ACC:
        cv_answer();
        break;
    case CVB_SPK: {
        int want = CALL.audio == SH_AUDIO_SPEAKER ? SH_AUDIO_EARPIECE : SH_AUDIO_SPEAKER;
        if (sh_route(want, why) < 0) { toast(why[0] ? why : "The audio route didn't change"); break; }
        CALL.audio = want;                    /* the push confirms */
        cv_ctl();
        break;
    }
    case CVB_MUTE:
        if (sh_mute(!CALL.muted, why) < 0) { toast(why[0] ? why : "Mute didn't change"); break; }
        CALL.muted = !CALL.muted;
        cv_ctl();
        break;
    case CVB_PAD: case CVB_HIDE:
        CV.keypad = CV.btn == CVB_PAD;
        cv_top();
        cv_ctl();
        break;
    case CVB_KEY: {
        if (CALL.state != SH_CALL_ACTIVE) { toast("Tones work once the call is connected"); break; }
        sh_haptic();
        if (sh_dtmf(KD[cv_key][0]) < 0) { toast("The tone wasn't sent"); break; }
        int n = (int)tk_strlen(CV.dtmf);
        if (n >= 20) { for (int i = 0; i < n; i++) CV.dtmf[i] = CV.dtmf[i + 1]; n--; }
        CV.dtmf[n] = KD[cv_key][0]; CV.dtmf[n + 1] = 0;
        cv_top();
        break;
    }
    default: break;
    }
    cv_place();
    relink();
}

/* ---- Messages -------------------------------------------------------------------------------------------------- */
/* the inbox again from the platform; a new received message posts a notification (unless its thread is open) */
static int cm_viewing(const char *num)
{
    return is_app("messages") && !AP.closing && AS.thread == 0 && !tk_strcmp(CM.thr, num) && !SL.black;
}
static void cm_sync(void)
{
    static int oid[SH_SMS_MAX], ohm[SH_SMS_MAX], oday[SH_SMS_MAX], ounr[SH_SMS_MAX];
    int on = nmsg;
    for (int i = 0; i < on; i++) { oid[i] = MSG[i].m.id; ohm[i] = MSG[i].hm; oday[i] = MSG[i].day; ounr[i] = MSG[i].unread; }
    int n = sh_sms_count(), k = 0, top = msg_maxid;
    if (n > SH_SMS_MAX) n = SH_SMS_MAX;
    for (int i = 0; i < n; i++) {
        cm_msg *m = &MSG[k];
        if (sh_sms_get(i, &m->m) < 0) continue;
        m->m.number[sizeof m->m.number - 1] = 0;
        m->m.text[sizeof m->m.text - 1] = 0;
        m->m.why[sizeof m->m.why - 1] = 0;
        int j = 0;
        while (j < on && oid[j] != m->m.id) j++;
        if (j < on) { m->hm = ohm[j]; m->day = oday[j]; m->unread = ounr[j]; }
        else {
            m->hm = cm_now_hm(); m->day = ST.day;
            m->unread = m->m.incoming && !cm_viewing(m->m.number);
            if (m->m.incoming && m->m.id > msg_maxid && m->unread) sh_notify("messages", m->m.number, m->m.text, m->hm);
        }
        if (m->m.id > top) top = m->m.id;
        k++;
    }
    nmsg = k;
    msg_maxid = top;
    msg_syncs++;
    if (is_app("messages") && !AP.closing && !ap_pending) build_app_page(AP.a);
}
void sh_sms_changed(unsigned gen)
{
    (void)gen;
    cm_sync();
    lock_ver++;
}
static void cm_mark_read(const char *num)
{
    for (int i = 0; i < nmsg; i++) if (!tk_strcmp(MSG[i].m.number, num)) MSG[i].unread = 0;
}
/* the threads, newest first: a message index each (the thread's last) */
#define CM_THR_MAX 8
static int cm_threads(int *last, int max)
{
    int n = 0;
    for (int i = nmsg - 1; i >= 0 && n < max; i--) {
        int k = 0;
        while (k < n && tk_strcmp(MSG[last[k]].m.number, MSG[i].m.number)) k++;
        if (k == n) last[n++] = i;
    }
    return n;
}
static int cm_unread(const char *num)
{
    int u = 0;
    for (int i = 0; i < nmsg; i++) u += MSG[i].unread && !tk_strcmp(MSG[i].m.number, num);
    return u;
}
/* B8 (GAP #21): home's badge counts: Phone's missed calls not yet seen, Messages' unread; 0 for every other app */
static int cm_badge(const char *k)
{
    int u = 0;
    if (!tk_strcmp(k, "phone")) return cm_missed_new;
    if (tk_strcmp(k, "messages")) return 0;
    for (int i = 0; i < nmsg; i++) u += MSG[i].unread;
    return u;
}
static void cm_when(char *b, const cm_msg *m, int with_day)
{
    char hm[8];
    cm_hm(hm, m->hm);
    int k = 0;
    if (with_day || m->day != ST.day) {
        const char *w = m->day == ST.day ? "Today " : m->day == ST.day - 1 ? "Yesterday " : 0;
        if (w) while (*w) b[k++] = *w++;
        else { k += put_dec(b + k, m->day); b[k++] = '.'; b[k++] = ' '; }
    }
    for (const char *p = hm; *p; p++) b[k++] = *p;
    b[k] = 0;
}
#define THR_TOP CONTENT_TOP
#define THR_H 78.0f
#define CM_FY_KB (KB_TOP - KB_STRIP - 48)  /* the compose field over the keyboard (B3: over its 40 pt strip) */
#define CM_FY 726.0f                      /* without it */
#define TO_Y PUSHED_TOP                     /* New Message's To: row */
static float cm_field_y(void) { return CM.focus ? CM_FY_KB : CM_FY; }
static const char *const TOK[3] = { "1234567890", "-/:;()+&@\"", ".,?!'" };   /* the To: field's 123 layer */
static const char *const *cm_rows(void) { return CM.focus == 1 && CM.num ? TOK : KR[CM.num]; }
/* B3: the keyboard for the field with the focus (To: a phone number on 123; the message: sentences, return a new line) */
static void cm_kb_focus(void)
{
    if (CM.focus == 1) kb_focus(KB_DIGITS | KB_PHONE, 0, CM.to);
    else if (CM.focus == 2) kb_focus(KB_AUTOCAP | KB_NL, 0, CM.txt);
}
static int cm_can_send(void)
{
    int any = 0;
    for (const char *p = CM.txt; *p; p++) any |= *p != ' ' && *p != '\n';
    return any && (AS.thread == 0 || cm_digits(CM.to) >= 7);
}
/* the bubbles of the open thread, the newest at the bottom (bot), as many as fit under top */
static void cm_bubbles(dc *d, float top, float bot)
{
    int ix[SH_SMS_MAX], n = 0, lastout = -1;
    for (int i = 0; i < nmsg; i++) if (!tk_strcmp(MSG[i].m.number, CM.thr)) { ix[n] = i; if (!MSG[i].m.incoming) lastout = n; n++; }
    if (!n) {
        d_text_box(d, TK_IT400, 13, 195, bl(TK_IT400, 13, (top + bot) / 2), CONTENT_W, 1, "No messages with this number yet.", 0, 0, C_LABEL2, 0);
        return;
    }
    static char ln[CM_ML][100];
    /* each message's lines are counted once a page build (at its first strip), not once a strip */
    static int cnl[SH_SMS_MAX];
    static float cwm[SH_SMS_MAX];
    if (d->oy <= 0) for (int k = 0; k < n; k++) cnl[k] = cm_wrap(ln, MSG[ix[k]].m.text[0] ? MSG[ix[k]].m.text : " ", 267.88f, &cwm[k]);
    float y = bot;
    int first = n;
    for (int k = n - 1; k >= 0; k--) {
        const cm_msg *m = &MSG[ix[k]];
        int nl = cnl[k];
        float hh = 18 + 20.8f * nl;
        int st = !m->m.incoming && (m->m.state == SH_SMS_SENDING || m->m.state == SH_SMS_FAILED || (k == lastout && m->m.state == SH_SMS_SENT));
        if (st) hh += 18;
        if (k < n - 1) hh += MSG[ix[k + 1]].m.incoming == m->m.incoming ? 4 : 12;
        int gap = k > 0 && (MSG[ix[k - 1]].day != m->day || m->hm / 100 * 60 + m->hm % 100 - (MSG[ix[k - 1]].hm / 100 * 60 + MSG[ix[k - 1]].hm % 100) >= 60);
        if (gap) hh += 30;
        if (y - hh - (gap ? 0 : 30) < top) break;
        y -= hh;
        first = k;
    }
    if (first == n) first = n - 1, y = top + 30;
    y -= 30;                                  /* the first shown gets its time */
    for (int k = first; k < n; k++) {
        const cm_msg *m = &MSG[ix[k]];
        int gap = k == first || (MSG[ix[k - 1]].day != m->day || m->hm / 100 * 60 + m->hm % 100 - (MSG[ix[k - 1]].hm / 100 * 60 + MSG[ix[k - 1]].hm % 100) >= 60);
        if (gap) {
            char w[24];
            cm_when(w, m, 1);
            d_text_box(d, TK_IT500, 12, 195, bl(TK_IT500, 12, y + 8), CONTENT_W, 1, w, 0, 0, C_LABEL2, 0);
            y += 30;
        }
        int nl = cnl[k], out = !m->m.incoming;
        float bw = (nl > 1 ? 267.88f : cwm[k]) + 26, bh = 18 + 20.8f * nl, bx = out ? CONTENT_R - bw : MARGIN;
        if (y + bh > top && PT(y + bh) >= d->oy && PT(y) <= d->oy + d->b->h) {   /* in this strip: its lines */
            float wm;
            cm_wrap(ln, m->m.text[0] ? m->m.text : " ", 267.88f, &wm);
            tk_corner = (tk_r4){ PT(20), PT(20), PT(out ? 6 : 20), PT(out ? 20 : 6) };
            tk_use_r4 = 1;
            d_rrect(d, bx, y, bw, bh, 20, out ? (m->m.state == SH_SMS_FAILED ? sbca(SBC_ACCENT, .55f) : C_ACCENT) : sbc(SBC_GRAY5));
            tk_use_r4 = 0;
            for (int q = 0; q < nl; q++) d_text(d, TK_IT400, 16, bx + 13, bl(TK_IT400, 16, y + 9 + 20.8f * q), ln[q], 0, 0, INK, 0);
        }
        y += bh;
        const char *st = 0;
        tk_col sc = W(.45f);
        char fw[80];
        if (out && m->m.state == SH_SMS_SENDING) st = "Sending\xe2\x80\xa6";
        else if (out && m->m.state == SH_SMS_FAILED) {
            int q = 0;
            for (const char *p = "Not Delivered"; *p; p++) fw[q++] = *p;
            if (m->m.why[0]) { const char *dt = " \xc2\xb7 "; while (*dt) fw[q++] = *dt++; for (const char *p = m->m.why; *p && q < 78; p++) fw[q++] = *p; }
            fw[q] = 0;
            st = fw; sc = C_RED;
        } else if (out && k == lastout && m->m.state == SH_SMS_SENT) st = "Sent";
        if (st) {
            d_text_box(d, TK_IT500, 11.5f, CONTENT_R - 4, bl(TK_IT500, 11.5f, y + 3), CONTENT_W, 2, st, 0, 0, sc, 0);
            y += 18;
        }
        if (k + 1 < n) y += MSG[ix[k + 1]].m.incoming == m->m.incoming ? 4 : 12;
    }
}
static void page_messages(dc *d)
{
    if (AS.thread < 0) {
        d_h1(d, "Messages");
        d_gcircle(d, GC_DONE);                    /* iOS 26: compose, a glass circle in the nav bar */
        d_compose(d, CONTENT_R - BACK_D / 2 + 1, NAV_MID, ACCENT());
        int last[CM_THR_MAX], n = cm_threads(last, CM_THR_MAX);
        if (!n) {
            const char *l = !sh_comms_ok() ? "Text messages aren't in this version yet." :
                            cm_cell_on() ? "Texts you send and receive show here until the phone restarts." :
                            "Turn on Cellular to send and receive text messages.";
            d_empty(d, CONTENT_TOP, EMPTY_BOT, "No Messages", l);
            if (sh_comms_ok() && !cm_cell_on()) cm_cell_button(d, cm_empty_bot(CONTENT_TOP, EMPTY_BOT, l) + 18);
            return;
        }
        for (int i = 0; i < n; i++) {
            const cm_msg *m = &MSG[last[i]];
            float y = THR_TOP + THR_H * i;
            if (y + THR_H > EMPTY_BOT) break;
            if (cm_unread(m->m.number)) d_rrect(d, 24, y + 33, 10, 10, 5, C_ACCENT);
            d_person(d, 62, y + 39, 22);
            char when[24];
            cm_when(when, m, 0);
            float ww = tw(TK_IT400, 15, when, 0, TK_TNUM), tx = 96;
            d_text_box(d, TK_IT600, 17, tx, bl(TK_IT600, 17, y + 12), CONTENT_R - ww - 22 - tx, 0, m->m.number, 0, 0, INK, 0);
            d_text(d, TK_IT400, 15, CONTENT_R - 14 - ww, bl(TK_IT400, 15, y + 13), when, 0, TK_TNUM, C_LABEL2, 0);
            d_icon(d, "chev", CONTENT_R - 10, y + 15, .45f);
            char ln[4][96], flat[160];
            int q = 0;
            if (!m->m.incoming) { const char *p = "You: "; while (*p) flat[q++] = *p++; }
            for (const char *p = m->m.text; *p && q < 158; p++) flat[q++] = *p == '\n' ? ' ' : *p;
            flat[q] = 0;
            float wm;
            int nl = wrapn(ln, 2, TK_IT400, 15, flat, CONTENT_R - tx, &wm);
            for (int k = 0; k < nl; k++) d_text(d, TK_IT400, 15, tx, bl(TK_IT400, 15, y + 35 + 19 * k), ln[k], 0, 0, C_LABEL2, 0);
            if (i + 1 < n) d_rrect(d, tx, y + THR_H - .5f, CONTENT_R - tx, .5f, 0, C_SEP);
        }
        return;
    }
    d_back(d, W(1));
    d_navtitle(d, AS.thread == 0 ? CM.thr : "New Message");
    float fy = cm_field_y();
    if (AS.thread == 0) cm_bubbles(d, PUSHED_TOP - 12, fy - 10);
    else {
        d_text(d, TK_IT400, 16, MARGIN + 4, bl(TK_IT400, 16, TO_Y + 13), "To:", 0, 0, C_LABEL2, 0);
        d_rrect(d, MARGIN, TO_Y + 44, CONTENT_W, .5f, 0, C_SEP);
    }
    if (CM.focus) kb_draw_r(d, cm_rows(), CM.shift, CM.num);
}
/* the compose field (its text, the send button), redrawn per key */
static void cm_field_build(void)
{
    relayer(&cmfld);
    if (!is_app("messages") || AS.thread < 0) { cmfld.hidden = 1; return; }
    float fy = cm_field_y(), fw = CONTENT_W - 44;
    dc d = mk(&cmfld, "msg-field", MARGIN, fy, CONTENT_W, 40, 2, 0);
    d_rrect(&d, MARGIN, fy, fw, 40, 20, W(.08f));
    d_rrect(&d, MARGIN + .5f, fy + .5f, fw - 1, 39, 19.5f, TK_RGBA(0, 0, 0, 0));
    if (CM.txt[0]) {
        /* one line: the end of what is typed (the start cut), the caret after it while the keyboard is up */
        char flat[480];
        int q = 0;
        for (const char *p = CM.txt; *p && q < 478; p++) flat[q++] = *p == '\n' ? ' ' : *p;
        flat[q] = 0;
        const char *s = flat;
        float maxw = fw - 32;
        while (*s && tw(TK_IT400, 16, s, 0, 0) > maxw) { s++; while (((unsigned char)*s & 0xc0) == 0x80) s++; }
        d_text(&d, TK_IT400, 16, MARGIN + 16, bl(TK_IT400, 16, fy + 10), s, 0, 0, INK, 0);
        if (CM.focus == 2) d_rrect(&d, MARGIN + 16 + tw(TK_IT400, 16, s, 0, 0) + 1, fy + 10, 2, 21, 1, ACCENT());
    } else {
        d_text(&d, TK_IT400, 16, MARGIN + 16, bl(TK_IT400, 16, fy + 10), "Text Message", 0, 0, C_LABEL2, 0);
        if (CM.focus == 2) d_rrect(&d, MARGIN + 15, fy + 10, 2, 21, 1, ACCENT());
    }
    int ok = cm_can_send();
    d_rrect(&d, CONTENT_R - 38, fy + 1, 38, 38, 19, ok ? C_ACCENT : W(.16f));
    d_icon(&d, "send", CONTENT_R - 27, fy + 11, ok ? 1 : .5f);
    trim(&cmfld);
    if (CM.focus) kb_ride(&cmfld);               /* B3: it rides up with the keyboard */
    cmfld.hidden = 0;
}
static void cm_to_build(void)
{
    relayer(&cmto);
    if (!is_app("messages") || AS.thread != 1) { cmto.hidden = 1; return; }
    dc d = mk(&cmto, "msg-to", MARGIN + 34, TO_Y, CONTENT_W - 34, 44, 2, 0);
    float x = MARGIN + 40;
    if (CM.to[0]) d_text_box(&d, TK_IT400, 16, x, bl(TK_IT400, 16, TO_Y + 13), CONTENT_W - 50, 0, CM.to, 0, TK_TNUM, INK, 0);
    if (CM.focus == 1) d_rrect(&d, x + (CM.to[0] ? tw(TK_IT400, 16, CM.to, 0, TK_TNUM) + 1 : 0), TO_Y + 12, 2, 21, 1, ACCENT());
    if (CM.to[0] && cm_emergency(CM.to))
        d_text_box(&d, TK_IT400, 12, x, bl(TK_IT400, 12, TO_Y + 31), CONTENT_W - 50, 0, "Emergency numbers can't be texted from Starbite OS", 0, 0, C_RED, 0);
    trim(&cmto);
    cmto.hidden = 0;
}
static void cm_send(void)
{
    const char *num = AS.thread == 0 ? CM.thr : CM.to;
    if (!cm_can_send()) { if (AS.thread == 1 && cm_digits(CM.to) < 7) toast("Enter the whole number"); return; }
    if (cm_emergency(num)) { toast("Emergency numbers can't be texted from here"); return; }
    char why[SH_WHY] = "";
    int s0 = msg_syncs;
    if (sh_sms_send(num, CM.txt, why) < 0) { toast(why[0] ? why : "The message wasn't sent"); return; }
    sh_haptic();
    CM.txt[0] = 0;
    int was_new = AS.thread == 1;
    if (was_new) { cm_cpy(CM.thr, sizeof CM.thr, num); AS.thread = 0; CM.focus = 2; CM.num = 0; }
    kb_ctx(CM.txt);                              /* B3: the keyboard capitalises again */
    if (msg_syncs == s0 || was_new) { CM.shift = 1; if (was_new) cm_kb_focus(); build_app_page(AP.a); }
    else { cm_field_build(); relink(); }         /* a push drew the bubble already: only the field, emptied */
}
static void cm_open_thread(const char *num)
{
    cm_cpy(CM.thr, sizeof CM.thr, num);
    AS.thread = 0;
    CM.focus = 0;
    cm_mark_read(num);
}
static void cm_msgs_tap(float x, float y)
{
    if (AS.thread < 0) {
        if (DONE_HIT(x, y)) {                    /* compose: New Message, the To: field on the 123 layer */
            AS.thread = 1; CM.to[0] = 0; CM.focus = 1; CM.num = 1; CM.shift = 0;
            cm_kb_focus();
            build_app_page(AP.a);
            return;
        }
        int last[CM_THR_MAX], n = cm_threads(last, CM_THR_MAX);
        if (!n) {
            const char *l = "Turn on Cellular to send and receive text messages.";
            float by = cm_empty_bot(CONTENT_TOP, EMPTY_BOT, l) + 18;
            if (sh_comms_ok() && !cm_cell_on() && y >= by && y < by + 40 && tk_fabsf(x - 195) < CM_BTN_W / 2) cm_open_cellular();
            return;
        }
        if (y >= THR_TOP && x >= MARGIN && x < CONTENT_R) {
            int i = (int)((y - THR_TOP) / THR_H);
            if (i < n && THR_TOP + THR_H * (i + 1) <= EMPTY_BOT) { cm_open_thread(MSG[last[i]].m.number); build_app_page(AP.a); }
        }
        return;
    }
    if (BACK_HIT(x, y)) { AS.thread = -1; CM.focus = 0; build_app_page(AP.a); return; }
    float fy = cm_field_y();
    if (y >= fy - 4 && y < fy + 44) {
        if (x >= CONTENT_R - 44) { cm_send(); return; }
        if (CM.focus != 2) { CM.focus = 2; CM.num = 0; CM.shift = !CM.txt[0]; cm_kb_focus(); build_app_page(AP.a); }
        return;
    }
    if (AS.thread == 1 && y >= TO_Y && y < TO_Y + 44) {
        if (CM.focus != 1) { CM.focus = 1; CM.num = 1; CM.shift = 0; cm_kb_focus(); build_app_page(AP.a); }
        return;
    }
    if (!CM.focus) return;
    if (y < 540) {                               /* above the keyboard: it goes down */
        if (y > PUSHED_TOP && y < fy - 4) { CM.focus = 0; build_app_page(AP.a); }
        return;
    }
    /* B3: the keyboard's key (shift, 123 and the capitals are the keyboard's own: no page build for them) */
    const char *ks;
    int ev = kb_take(&ks);
    if (CM.focus == 1) {                         /* To: digits and a leading + only */
        int n = (int)tk_strlen(CM.to);
        char c = ev == KBE_TEXT && !ks[1] ? ks[0] : 0;
        if (ev == KBE_DEL && n) CM.to[n - 1] = 0;
        else if (ev == KBE_RET) { CM.focus = 2; CM.num = 0; CM.shift = !CM.txt[0]; cm_kb_focus(); build_app_page(AP.a); return; }
        else if (c && n < 20 && ((c >= '0' && c <= '9') || (c == '+' && !n))) { CM.to[n] = c; CM.to[n + 1] = 0; }
        else if (ev == KBE_TEXT && c != ' ') { toast("Numbers only: there are no contacts yet"); return; }
        else return;
        cm_to_build(); cm_field_build(); relink();
    } else if (kb_edit(CM.txt, (int)sizeof CM.txt - 2)) { cm_field_build(); relink(); }
}

/* B3: the message field under a long press (the edit menu, shell_kb.inc) */
static void cm_field_redraw(void) { cm_field_build(); relink(); }
static int cm_em_target(float x, float y, emt *o)
{
    if (!is_app("messages") || AS.thread < 0) return 0;
    float fy = cm_field_y();
    if (y < fy - 4 || y >= fy + 44 || x >= CONTENT_R - 44) return 0;
    float w = tk_minf(tw(TK_IT400, 16, CM.txt, 0, 0), CONTENT_W - 44 - 32);
    *o = (emt){ CM.txt, (int)sizeof CM.txt - 2, { MARGIN + 14, fy + 8, w + 4, 24 }, cm_field_redraw };
    return 1;
}

/* ---- the shell's hooks --------------------------------------------------------------------------------------- */
static int cm_app_tap(float x, float y)
{
    if (is_app("phone")) { cm_phone_tap(x, y); return 1; }
    if (is_app("messages")) { cm_msgs_tap(x, y); return 1; }
    return 0;
}
/* a page was built (own_extras): the keypad's line, the fields */
static void cm_extras(const char *k)
{
    if (tk_strcmp(k, "phone")) { relayer(&cmhint); cmhint.hidden = 1; cm_ring_free(); }
    else cm_hint_build();
    cm_field_build();
    cm_to_build();
}
static int cm_layers(tk_layer **o)
{
    int n = 0;
    if (!cmhint.name) { tk_layer_init(&cmhint, "dial-line"); cmhint.hidden = 1; }
    if (!cmring.name) { tk_layer_init(&cmring, "dial-ring"); cmring.hidden = 1; }
    if (!cmfld.name) { tk_layer_init(&cmfld, "msg-field"); cmfld.hidden = 1; }
    if (!cmto.name) { tk_layer_init(&cmto, "msg-to"); cmto.hidden = 1; }
    o[n++] = &cmhint; o[n++] = &cmring; o[n++] = &cmfld; o[n++] = &cmto;
    return n;
}
static void cm_link(void)
{
    if (!cvl.name) { tk_layer_init(&cvl, "call"); cvl.hidden = 1; }
    if (!cvhl.name) { tk_layer_init(&cvhl, "press"); cvhl.hidden = 1; }
    tk_layer *P[] = { &cvdec, &cvacc, &cvend, &cvtrbg, &cvtrack, &cvthumb, &cvsb, &cvtop, &cvst, &cvctl, &cvhide };
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) if (!P[i]->name) { tk_layer_init(P[i], "cv"); P[i]->hidden = 1; }
    if (!cvbg.name) { tk_layer_init(&cvbg, "cv-ground"); cvbg.hidden = 1; }
    if (CV.on) {
        cvbg.hidden = 0;
        tk_layer *c[] = { &cvbg, &cvsb, &cvtop, &cvst, &cvctl, &cvhide, &cvend, &cvdec, &cvacc, &cvtrbg, &cvtrack, &cvthumb, &cvhl };
        link(&cvl, c, (int)(sizeof c / sizeof c[0]));
    } else
        cvl.child = 0;
}
/* an app opens: Settings > Cellular from Phone or Messages; a notification's thread or Recents */
static void cm_opened(int a)
{
    const char *k = APPS[a].k;
    if (!tk_strcmp(k, "settings") && CM.want_sub) AS.sub = CM.want_sub;
    if (!tk_strcmp(k, "phone") && CM.want_tab >= 0 && CM.want_view) AS.tab = CM.want_tab;
    if (!tk_strcmp(k, "messages")) {
        CM.focus = 0;
        if (CM.want_view && CM.want_thr[0]) cm_open_thread(CM.want_thr);
    }
    CM.why[0] = 0;
    CM.want_sub = 0; CM.want_view = 0; CM.want_thr[0] = 0; CM.want_tab = -1;
}
static void cm_closed(void)
{
    if (CM.own == 2) CM.own = 0;
    CM.hold = 0;
    cm_ring_free();
    relayer(&cmhint); cmhint.hidden = 1;
    relayer(&cmfld); cmfld.hidden = 1;
    relayer(&cmto); cmto.hidden = 1;
    CM.focus = 0;
}
static void cm_open_cellular(void)
{
    int s = -1;
    for (int a = 0; a < NAPPS; a++) if (!tk_strcmp(APPS[a].k, "settings")) s = a;
    if (s < 0) return;
    CM.want_sub = 3;
    app_finish_close();
    app_open(s);
}
/* a tap on a notification (shell_nc.inc's nc_launch, before it opens the app): Messages opens its thread, a missed
 * call Recents */
static void cm_nc_launch(const char *who, const char *title, int a)
{
    int msgs = !tk_strcmp(who, "messages"), ph = !tk_strcmp(who, "phone");
    if (!msgs && !ph) return;
    if (AP.on && AP.a == a && !AP.closing) {           /* open already: there now */
        if (msgs) { cm_open_thread(title); build_app_page(a); }
        else if (AS.tab != 1) { AS.tab = 1; AS.num[0] = 0; build_app_page(a); }
        return;
    }
    CM.want_view = 1;
    if (msgs) cm_cpy(CM.want_thr, sizeof CM.want_thr, title);
    else CM.want_tab = 1;
}
/* every touch first: the call screen owns the screen; the call button's hold is followed here */
static int cm_touch(int ph, float x, float y, float t)
{
    if (ph == 0) {
        CM.down_t = t;
        CM.own = 0;
        if (CV.on && CV.at > 0) { CM.own = 1; cv_down(x, y); return 1; }
        float dx = x - PAD_CX, dy = y - PAD_CY;
        if (cm_pad_ready() && dx * dx + dy * dy <= 41 * 41) { CM.own = 2; cm_hold_begin(x, y, t); return 1; }
        return 0;
    }
    if (!CM.own) return 0;
    int moved = tk_fabsf(x - (CM.own == 1 ? CV.x0 : CM.hx)) > 12 || tk_fabsf(y - (CM.own == 1 ? CV.y0 : CM.hy)) > 12;
    if (CM.own == 1) {
        if (!CV.on) { if (ph == 2) CM.own = 0; return 1; }
        if (CV.slide) {
            if (moved) CV.moved = 1;
            CV.thumb = tk_clampf(CV.tx0 + x - CV.x0, 0, CV_TH_MAX);
            cv_place();
        } else if (moved && !CV.moved) { CV.moved = 1; cvhl.hidden = 1; }
        if (ph == 2) { CM.own = 0; cv_up(); }
        return 1;
    }
    if (ph == 1) { if (moved) { CM.own = 0; cm_hold_end(); } return 1; }
    CM.own = 0;
    cm_hold_up(moved);
    return 1;
}
/* every frame: the hold, the screen's fade, the thumb going back */
static int cm_tick(float dt, float t)
{
    int mv = 0;
    if (CM.hold) {
        float p = tk_minf(1, (t - CM.hold_t0) / 600);
        cm_ring_set(p);
        if (p >= 1) {
            sh_haptic();
            cm_hold_end();
            CM.own = 0;                          /* the finger's lift does nothing more */
            cm_dial();
        }
        mv = 1;
    }
    if (CV.on && CV.a != CV.at) {
        CV.a = CV.at > CV.a ? tk_minf(CV.at, CV.a + dt / .2f) : tk_maxf(CV.at, CV.a - dt / .25f);
        if (CV.a <= 0 && CV.at <= 0) cv_free();
        else cv_place();
        mv = 1;
    }
    if (CV.on && !CV.slide && CV.thumb > 0 && CV.mode == CVM_INCOMING && !CV.answering) {
        CV.thumb = CV.thumb < 1 ? 0 : CV.thumb * tk_powf(.001f, dt * 2.5f);
        cv_place();
        mv = 1;
    }
    return mv;
}
static int cm_moving(void)
{
    return CM.hold || (CV.on && CV.a != CV.at) || CV.slide || (CV.on && CV.thumb > 0 && CV.mode == CVM_INCOMING && !CV.answering);
}
/* idle: the call screen's static parts while Cellular is on (gone with it) */
static void cm_idle(void)
{
    if (cm_cell_on() && sh_comms_ok() && !CV.pre) cv_pre();
    else if (!cm_cell_on() && CV.pre && !CV.on) cv_pre_free();
}
/* the ended call's screen: 1.5 s, then its fade */
static float cm_next_update(float t)
{
    if (CV.pend_close) { CV.pend_close = 0; CV.close_at = t + 1500; }
    return CV.on && CV.close_at >= 0 ? tk_maxf(1, CV.close_at - t) : -1;
}
static int cm_refresh(float t)
{
    if (CV.on && CV.close_at >= 0 && t >= CV.close_at) { CV.close_at = -1; cv_hide(); return 1; }
    return 0;
}
static void cm_minute(void)
{
    if (CV.on && CV.at > 0) { cv_sb(); cvsb.hidden = 0; relink(); }
}
/* press feedback for rows and buttons (app_press) */
static int cm_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    if (is_app("phone") && AS.tab == 1 && nrc) {
        int n = nrc < REC_MAX ? nrc : REC_MAX;
        if (x >= GROUP_X && x < CONTENT_R && y >= CONTENT_TOP && y < CONTENT_TOP + REC_H * n) {
            int i = (int)((y - CONTENT_TOP) / REC_H);
            *r = (tk_rect){ GROUP_X, CONTENT_TOP + REC_H * i, GROUP_W, REC_H };
            *g = (tk_rect){ GROUP_X, CONTENT_TOP, GROUP_W, REC_H * n };
            *rad = 16;
            return 1;
        }
        return 0;
    }
    if (!is_app("messages")) return 0;
    if (AS.thread < 0) {
        if (DONE_HIT(x, y)) { *r = *g = (tk_rect){ CONTENT_R - BACK_D, SAFE_TOP, BACK_D, BACK_D }; *rad = BACK_D / 2; *c = W(.16f); return 1; }
        int last[CM_THR_MAX], n = cm_threads(last, CM_THR_MAX);
        if (y >= THR_TOP && y < THR_TOP + THR_H * n && x >= 0 && x < 390) {
            int i = (int)((y - THR_TOP) / THR_H);
            *r = *g = (tk_rect){ 0, THR_TOP + THR_H * i, 390, THR_H };
            *rad = 0; *c = W(.08f);
            return 1;
        }
        return 0;
    }
    float fy = cm_field_y();
    if (y >= fy - 4 && y < fy + 44 && x >= CONTENT_R - 44 && cm_can_send()) {
        *r = *g = (tk_rect){ CONTENT_R - 38, fy + 1, 38, 38 }; *rad = 19; *c = W(.25f);
        return 1;
    }
    if (CM.focus && y >= 548 && kb_rect(x, y, cm_rows(), !CM.num, r)) { *g = *r; *rad = 6; *c = W(.35f); return 1; }
    return 0;
}
/* the host's stills: "thread" NUMBER (list, new) with v the focus (0 none, 1 To, 2 the message); "compose" TEXT;
 * "to" NUMBER; "dial" NUMBER; "recent" NUMBER with v the kind (RC_*); "callpad" DIGITS (the tones' keypad, v 1);
 * "callhold" v (the call button's ring at v); "slide" v (the answer slider's thumb at v) */
static int cm_set(const char *what, const char *key, float v)
{
    if (!tk_strcmp(what, "thread")) {
        if (!key[0] || !tk_strcmp(key, "list")) AS.thread = -1;
        else if (!tk_strcmp(key, "new")) { AS.thread = 1; CM.to[0] = 0; }
        else cm_open_thread(key);
        CM.focus = (int)v;
        CM.num = CM.focus == 1;
        CM.shift = CM.focus == 2 && !CM.txt[0];
        cm_kb_focus();                           /* B3 */
        if (AP.on) build_app_page(AP.a);
        return 1;
    }
    if (!tk_strcmp(what, "compose")) { cm_cpy(CM.txt, sizeof CM.txt, key); CM.shift = 0; kb_ctx(CM.txt); if (AP.on) build_app_page(AP.a); return 1; }
    if (!tk_strcmp(what, "to")) { cm_cpy(CM.to, sizeof CM.to, key); if (AP.on) build_app_page(AP.a); return 1; }
    if (!tk_strcmp(what, "dial")) {
        cm_cpy(AS.num, sizeof AS.num, key);
        CM.why[0] = 0;
        build_apnum();
        cm_hint_build();
        relink();
        return 1;
    }
    if (!tk_strcmp(what, "dialwhy")) { cm_refuse(key); return 1; }
    if (!tk_strcmp(what, "recent")) { cm_rec_add(key, (int)v, (int)v < 2 ? 125 : 0); if (AP.on) build_app_page(AP.a); return 1; }
    if (!tk_strcmp(what, "callpad")) {
        CV.keypad = v > 0;
        cm_cpy(CV.dtmf, sizeof CV.dtmf, key);
        if (CV.on) { cv_top(); cv_ctl(); cv_place(); relink(); }
        return 1;
    }
    if (!tk_strcmp(what, "callhold")) {
        if (cm_pad_ready() && AS.num[0]) { cm_hold_begin(PAD_CX, PAD_CY, now_ms); cm_ring_build(); cm_ring_set(v); CM.hold = 0; }
        return 1;
    }
    if (!tk_strcmp(what, "slide")) { CV.thumb = CV_TH_MAX * tk_clampf(v, 0, 1); if (CV.on) cv_place(); return 1; }
    return 0;
}
/* back to the plain lock for a film (sh_set "reset"): the call screen and the hold gone (a real call keeps its
 * screen through sleep: back_to_lock does not call this) */
static void cm_reset(void)
{
    CM.own = 0;
    cm_hold_end();
    if (CV.on) cv_free();
    CV.a = CV.at = 0;
}
