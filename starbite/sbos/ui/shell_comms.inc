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
#define CM_NREC 50
enum { RC_OUT, RC_IN, RC_MISSED, RC_DECLINED, RC_NOANSWER };
typedef struct { char num[24]; int kind, secs, hm, id; } cm_rec;
static cm_rec RC[CM_NREC];                 /* Recents: the calls, newest first (Track A: kept, the blob "calls") */
static int nrc, rc_ids, rc_missed;         /* rc_missed: missed calls since Recents was last seen (the dock's badge) */
#define CM_NFAV 12
typedef struct { char num[24]; int id; } cm_fav;
static cm_fav FAV[CM_NFAV];                /* Favourites: numbers starred in Recents (this boot) */
static int nfav;
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
    int to_end;                            /* the thread's next layout scrolls to its newest message */
    int read_due;                          /* the open thread got a message: the kernel's read mark */
} CM;
/* the lists' items (Phone's Calls, Messages' threads and bubbles: LS, shell_apps.inc) */
enum { CL_NONE, CL_CALLS, CL_THREADS, CL_BUBBLES };
enum { CF_HDR = 0x100, CF_FAV = 0x200 };            /* a Calls item: a section's header, a favourite (else a call) */
static int cl_kind;
static int CLI[SCV_MAXI];                   /* item -> the RC / FAV index, a thread's last message, a bubble's message */
static uint16_t CLF[SCV_MAXI];               /* a bubble's parts (below) */
enum { BF_TAIL = 1, BF_TIME = 2, BF_STATUS = 4, BF_INTRO = 8, BF_GROUP = 16, BF_FIRST = 32, BF_LAST = 64 };
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
static void cm_hm(char *b, int hm) { fmt_time(b, hm / 100, hm % 100, 1); }   /* Track A: "9:41 AM" in 12-hour (b: 9) */
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
/* a greedy word wrap of a message into at most CM_ML lines of maxw (fs pt); '\n' breaks a line; a word wider than the
 * line breaks by character; text past the last line ends it with an ellipsis */
#define CM_ML 24
static int cm_wrap(char ln[CM_ML][100], const char *s, float fs, float maxw, float *wmax)
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
            if (tw(TK_IT400, fs, t, 0, 0) > maxw || e - st > 99) {
                if (!any) {                       /* one word wider than the line: what fits of it */
                    int m = len;
                    while (m > 1) {
                        m--;
                        while (m > 1 && ((unsigned char)t[m] & 0xc0) == 0x80) m--;
                        t[m] = 0;
                        if (tw(TK_IT400, fs, t, 0, 0) <= maxw) break;
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
            ellipsize(e2, sizeof e2, TK_IT400, fs, ln[nl], maxw - tw(TK_IT400, fs, "\xe2\x80\xa6", 0, 0));
            int q = (int)tk_strlen(e2);
            if (q < 96 && !(q >= 3 && (unsigned char)e2[q - 3] == 0xe2)) { e2[q] = (char)0xe2; e2[q + 1] = (char)0x80; e2[q + 2] = (char)0xa6; e2[q + 3] = 0; }
            tk_memcpy(ln[nl], e2, tk_strlen(e2) + 1);
        }
        float w = tw(TK_IT400, fs, ln[nl], 0, 0);
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
                       red ? C_RED : W(.5f), 0);
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

/* Recents: a call followed from its first push to its end */
static void cm_list_build(void);
/* Track A: Recents kept across restarts in the blob "calls": version 1, the count, then each call's number (24 bytes),
 * kind (1), seconds (4) and hm (2); B5's row ids are made again at the restore */
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
        RC[i].id = ++rc_ids;
    }
}
static void cm_rec_add(const char *num, int kind, int secs)
{
    if (nrc == CM_NREC) nrc--;
    for (int k = nrc; k > 0; k--) RC[k] = RC[k - 1];
    cm_cpy(RC[0].num, sizeof RC[0].num, num);
    RC[0].kind = kind; RC[0].secs = secs; RC[0].hm = cm_now_hm(); RC[0].id = ++rc_ids;
    nrc++;
    if (kind == RC_MISSED && !(is_app("phone") && AS.tab == 1 && !AP.closing)) rc_missed++;
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
    if (kind == RC_MISSED) sh_notify("phone", "Missed Call", CM.rec_num[0] ? CM.rec_num : "Unknown", cm_now_hm());
    if (is_app("phone") && AS.tab == 1 && !AP.closing && !ap_pending) {
        if (nrc == 1 && !nfav) build_app_page(AP.a); /* the empty state goes */
        else { cm_list_build(); relink(); }          /* the new row; the others only move */
    }
    home_badges(0);                              /* B8's dock badges, B5's counts (cm_badge) */
}
static int cm_fav_has(const char *num)
{
    for (int i = 0; i < nfav; i++) if (!tk_strcmp(FAV[i].num, num)) return i;
    return -1;
}
/* Favourites: numbers starred from Recents (this boot only: there are no contacts to keep them with) */
static void cm_fav_toggle(const char *num)
{
    int i = cm_fav_has(num);
    if (i >= 0) {
        for (int k = i; k + 1 < nfav; k++) FAV[k] = FAV[k + 1];
        nfav--;
        toast("Removed from Favourites");
    } else if (nfav == CM_NFAV) toast("Up to twelve Favourites in this version");
    else {
        cm_cpy(FAV[nfav].num, sizeof FAV[nfav].num, num);
        FAV[nfav].id = ++rc_ids;
        nfav++;
        toast("Added to Favourites");
    }
}

#define TAB3_W (GROUP_W / 3)
static void phone_tabs(dc *d)
{
    static const char *const TK[3] = { "rec", "con", "pad" };
    static const char *const TL[3] = { "Calls", "Contacts", "Keypad" };
    d_lg_flat(d, GROUP_X, 718, GROUP_W, 62, 31, 1);   /* P3i: over the flat ground (the backdrop's blur cost ~40 ms a build) */
    for (int i = 0; i < 3; i++) {
        float bx = GROUP_X + TAB3_W * i;
        int on = i + 1 == AS.tab;
        char n[16] = "tab-";
        int k = 4;
        for (const char *c = TK[i]; *c; c++) n[k++] = *c;
        if (on) { n[k++] = '-'; n[k++] = 'o'; n[k++] = 'n'; }
        n[k] = 0;
        if (on) d_rrect(d, bx + 4, 722, TAB3_W - 8, 54, 27, W(.14f));   /* iOS 26: the selected tab's pill */
        d_icon(d, n, bx + (TAB3_W - 22) / 2, 730.5f, 1);
        float w = tw(TK_IT500, 10.5f, TL[i], 0, 0);
        d_text(d, TK_IT500, 10.5f, bx + (TAB3_W - w) / 2, bl(TK_IT500, 10.5f, 754.5f), TL[i], 0, 0, on ? ACCENT() : W(.92f), 0);
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
/* iOS's info button: an accent ring with an i */
#define INFO_X (CONTENT_R - 11)
static void d_info(dc *d, float cx, float cy)
{
    d_arc(d, cx, cy, 10.2f, 1.6f, 0, 6.2831853f, ACCENT());
    d_rrect(d, cx - 1.1f, cy - 2.5f, 2.2f, 8, 1.1f, ACCENT());
    d_rrect(d, cx - 1.3f, cy - 6.8f, 2.6f, 2.6f, 1.3f, ACCENT());
}
static void page_phone(dc *d)
{
    if (AS.tab == 0) AS.tab = 1;                      /* (Favourites are the Calls list's first section now) */
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
        if (!nrc && !nfav) {                         /* else the list (its title the first item) */
            d_h1(d, "Calls");
            const char *l = !sh_comms_ok() ? "Calls aren't in this version yet." :
                            cm_cell_on() ? "Calls you make and receive show here until the phone restarts." :
                            "Calls you make and receive show here. Turn on Cellular to call.";
            d_empty(d, CONTENT_TOP, EMPTY_TAB, "No Recent Calls", l);
            if (sh_comms_ok() && !cm_cell_on()) cm_cell_button(d, cm_empty_bot(CONTENT_TOP, EMPTY_TAB, l) + 18);
        }
    } else
        page_contacts(d);
    phone_tabs(d);
}
/* the lists' rows (Recents, Favourites): iOS 26's, a monogram, the number, its line, the time and the info button */
#define REC_H 64.0f
static void cl_title(dc *d, const char *t) { d_h1(d, t); }
static void cl_rec_row(dc *d, const cm_rec *r, float y, int last)
{
    char when[12], sub[48];                  /* Track A: room for "12:05 AM" */
    cm_hm(when, r->hm);
    int k = 0;
    for (const char *w = cm_rec_word(r->kind); *w; w++) sub[k++] = *w;
    if ((r->kind == RC_OUT || r->kind == RC_IN) && r->secs > 0) {
        const char *dot = " \xc2\xb7 ";
        for (; *dot; dot++) sub[k++] = *dot;
        k += cm_dur(sub + k, r->secs);
    }
    sub[k] = 0;
    float tx = MARGIN + 52, ww = tw(TK_IT400, 15, when, 0, TK_TNUM);
    d_monogram(d, MARGIN + 20, y + REC_H / 2, 20, r->num);
    d_text_box(d, TK_IT600, 17, tx, bl(TK_IT600, 17, y + 12), INFO_X - 18 - ww - 10 - tx, 0, r->num[0] ? r->num : "Unknown",
               0, 0, r->kind == RC_MISSED ? C_RED : INK, 0);
    d_text(d, TK_IT400, 14, tx, bl(TK_IT400, 14, y + 35), sub, 0, TK_TNUM, C_LABEL2, 0);
    d_text(d, TK_IT400, 15, INFO_X - 18 - ww, bl(TK_IT400, 15, y + 22), when, 0, TK_TNUM, C_LABEL2, 0);
    d_info(d, INFO_X, y + REC_H / 2);
    if (!last) d_rrect(d, tx, y + REC_H - .5f, 390 - tx, .5f, 0, C_SEP);
}
static void cl_fav_row(dc *d, const cm_fav *f, float y, int last)
{
    float tx = MARGIN + 52;
    d_monogram(d, MARGIN + 20, y + REC_H / 2, 20, f->num);
    d_text_box(d, TK_IT600, 17, tx, bl(TK_IT600, 17, y + 12), INFO_X - 18 - tx, 0, f->num, 0, 0, INK, 0);
    d_text(d, TK_IT400, 14, tx, bl(TK_IT400, 14, y + 35), "mobile", 0, 0, C_LABEL2, 0);
    d_info(d, INFO_X, y + REC_H / 2);
    if (!last) d_rrect(d, tx, y + REC_H - .5f, 390 - tx, .5f, 0, C_SEP);
}
static void cm_phone_tap(float x, float y)
{
    if (y >= 718 && y <= 780 && x >= GROUP_X && x < GROUP_X + GROUP_W) {
        int t = 1 + tk_mini(2, (int)((x - GROUP_X) / TAB3_W));
        if (t != AS.tab) { AS.tab = t; if (t != 3) AS.num[0] = 0; CM.why[0] = 0; LS.s = 0; build_app_page(AP.a); }
        return;
    }
    if (AS.tab == 1) {
        if (!nrc && !nfav) {
            float by = cm_empty_bot(CONTENT_TOP, EMPTY_TAB, "Calls you make and receive show here. Turn on Cellular to call.") + 18;
            if (sh_comms_ok() && !cm_cell_on() && y >= by && y < by + 40 && tk_fabsf(x - 195) < CM_BTN_W / 2) cm_open_cellular();
            return;
        }
        int i = scv_hit(&LS, y, 0);
        if (i < 1 || (CLF[i] & CF_HDR)) return;
        int fav = CLF[i] & CF_FAV;
        const char *num = fav ? FAV[CLI[i]].num : RC[CLI[i]].num;
        if (!num[0]) return;
        if (x >= INFO_X - 22) {                        /* the info button: in or out of Favourites */
            char n[24];
            cm_cpy(n, sizeof n, num);
            cm_fav_toggle(n);
            cm_list_build();
            relink();
            return;
        }
        /* iOS calls at once; here the number goes to the keypad and the hold still dials */
        AS.tab = 3;
        cm_cpy(AS.num, sizeof AS.num, num);
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
    int swipe;                              /* a home swipe (B5): it minimises the call into B7's status capsule */
} CV = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -1, "", "", "", 0 };
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
/* Track B: the proximity sensor while the call is at the ear (sbos-core-api.md 11): on while the call screen shows a
 * call that isn't ringing or ending and the audio isn't on the speaker; off when the speaker goes on or the screen
 * goes (the kernel ends it by itself with the call). The kernel darkens the panel: no UI here. Only changes reach
 * the kernel. */
static int cv_prox_on;
static void cv_prox_sync(void)
{
    int want = CV.on && CV.at > 0 && CV.mode == CVM_CALL && cm_live(CALL.state) && CALL.state != SH_CALL_INCOMING &&
               CALL.state != SH_CALL_ENDING && !CV.ending && CALL.audio != SH_AUDIO_SPEAKER;
    if (want == cv_prox_on) return;
    cv_prox_on = want;
    sh_call_proximity(want);
}
/* cp7w: the ringtone while the call screen shows an incoming call (until it is answered, declined or gone) */
static void cv_ring_sync(void)
{
    int want = CV.on && CV.at > 0 && CV.mode == CVM_INCOMING && !CV.answering && !CV.ending;
    if (want && snd_ring_who != 1) snd_ring(1, SH_SND_RING);
    else if (!want) snd_ring_stop(1);
}
/* where everything is for the mode (and the lock: the slider) */
static void cv_place(void)
{
    cv_prox_sync();
    cv_ring_sync();
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
/* B7 (shell_island.inc): the call screen minimised into the status capsule (isl_call_min: the pushes then leave the
 * screen closed); the capsule's tap brings it back (cv_reopen). B5's home swipe on the call screen minimises. */
static int isl_call_min;
static void cv_hide(void);
static void cv_minimise(void)
{
    if (!CV.on || CV.mode != CVM_CALL || !cm_live(CALL.state)) return;
    isl_call_min = 1;
    cvhl.hidden = 1;
    cv_hide();
}
static void cv_light(float cx, float cy, float r, tk_col c)
{
    tk_rect q = { cx - r, cy - r, 2 * r, 2 * r };
    hl_set(&cvhl, q, q, r, c);
}
static void cv_free(void)
{
    snd_ring_stop(1);                         /* cp7w */
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
    snd_ring_stop(1);                         /* cp7w: the ring stops with the screen's fade */
    CV.at = 0;                                /* a .25 s fade (cm_tick), then everything goes */
    CV.close_at = -1; CV.pend_close = 0;
    cvhl.hidden = 1;
    cv_prox_sync();                           /* Track B: the panel lights again at once */
}
static void cv_ended(void)
{
    if (CV.mode == CVM_INCOMING) { cv_hide(); return; }   /* the caller gave up (a missed call) */
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
static void cv_reopen(void)
{
    isl_call_min = 0;
    if (cm_live(CALL.state)) cv_show(CALL.state == SH_CALL_INCOMING && !CV.answering ? CVM_INCOMING : CVM_CALL);
}
void sh_call(const sh_call_t *c)
{
    sh_call_t o = CALL;
    CALL = *c;
    if (c->state != SH_CALL_INCOMING) { CV.answering = 0; snd_ring_stop(1); }   /* cp7w: answered, declined or gone */
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
            else if (!isl_call_min) cv_show(CVM_CALL);
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
    if (CALL.state == SH_CALL_INCOMING) { CV.answering = 1; snd_ring_stop(1); }   /* "Connecting..." until the kernel says so; cp7w: quiet */
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
        snd_ring_stop(1);                     /* cp7w */
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
static void cm_msgs_changed(void);
static int cm_viewing(const char *num)
{
    return is_app("messages") && !AP.closing && AS.thread == 0 && !tk_strcmp(CM.thr, num) && !SL.black;
}
/* a message's local time from its UTC stamp (the phone's offset now: the local clock against sh_utc); 0 unknown */
static int cm_local(long long utc, int *hm, int *day)
{
    long long now = sh_utc();
    if (utc <= 0 || now <= 0) return 0;
    long long loc = (long long)ST.h * 3600 + ST.mi * 60, off = loc - now % 86400;
    while (off > 14 * 3600) off -= 86400;
    while (off < -12 * 3600) off += 86400;
    off = (off + 450) / 900 * 900;                   /* zones are whole quarter hours */
    long long t = utc + off, tn = now + off;
    long long dd = (t >= 0 ? t / 86400 : (t - 86399) / 86400) - tn / 86400;
    int s = (int)(t % 86400 + 86400) % 86400;
    *hm = s / 3600 * 100 + s / 60 % 60;
    *day = ST.day + (int)dd;
    return 1;
}
static void cm_sync(void)
{
    static int oid[SH_SMS_MAX], ohm[SH_SMS_MAX], oday[SH_SMS_MAX], ounr[SH_SMS_MAX];
    int on = nmsg;
    for (int i = 0; i < on; i++) { oid[i] = MSG[i].m.id; ohm[i] = MSG[i].hm; oday[i] = MSG[i].day; ounr[i] = MSG[i].unread; }
    int n = sh_sms_count(), k = 0, top = msg_maxid, i0 = n > SH_SMS_MAX ? n - SH_SMS_MAX : 0;
    for (int i = i0; i < n && k < SH_SMS_MAX; i++) {
        cm_msg *m = &MSG[k];
        if (sh_sms_get(i, &m->m) < 0) continue;
        m->m.number[sizeof m->m.number - 1] = 0;
        m->m.text[sizeof m->m.text - 1] = 0;
        m->m.why[sizeof m->m.why - 1] = 0;
        int j = 0;
        while (j < on && oid[j] != m->m.id) j++;
        if (j < on) { m->hm = ohm[j]; m->day = oday[j]; m->unread = ounr[j]; }
        else {
            if (!cm_local(m->m.utc, &m->hm, &m->day)) { m->hm = cm_now_hm(); m->day = ST.day; }
            m->unread = m->m.incoming && !cm_viewing(m->m.number);
            /* (the first look at a store kept over a restart posts nothing) */
            if (m->m.incoming && m->m.id > msg_maxid && m->unread && msg_syncs) sh_notify("messages", m->m.number, m->m.text, m->hm);
        }
        if (m->m.unread >= 0) m->unread = m->m.unread && !cm_viewing(m->m.number);   /* the kernel's store knows */
        if (m->m.id > top) top = m->m.id;
        if (m->unread && cm_viewing(m->m.number)) { m->unread = 0; CM.read_due = 1; }
        k++;
    }
    nmsg = k;
    msg_maxid = top;
    msg_syncs++;
    if (CM.read_due) { CM.read_due = 0; sh_sms_read(CM.thr); }   /* a message into the open thread: read */
    cm_msgs_changed();
    home_badges(0);
}
void sh_sms_changed(unsigned gen)
{
    (void)gen;
    cm_sync();
    lock_ver++;
}
static void cm_mark_read(const char *num)
{
    int any = 0;
    for (int i = 0; i < nmsg; i++) if (!tk_strcmp(MSG[i].m.number, num)) { any |= MSG[i].unread; MSG[i].unread = 0; }
    if (any) sh_sms_read(num);                       /* the kernel's store: the thread read (survives a restart) */
    home_badges(0);
}
/* the threads, newest first: a message index each (the thread's last) */
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
static int cm_badge_msgs(void) { int u = 0; for (int i = 0; i < nmsg; i++) u += MSG[i].unread; return u; }
/* B8 (GAP #21): home's badge counts (its one layer, home_badges), B5's numbers: Phone's missed calls since Recents was
 * last seen (none while it is open), Messages' unread; 0 for every other app */
static int cm_badge(const char *k)
{
    if (!tk_strcmp(k, "phone")) return rc_missed;
    return tk_strcmp(k, "messages") ? 0 : cm_badge_msgs();
}
static void cm_when(char *b, const cm_msg *m, int with_day)
{
    char hm[12];                                 /* Track A: room for "12:05 AM" */
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
#define THR_H 78.0f
#define TO_Y PUSHED_TOP                     /* New Message's To: row */
#define THREAD_TOP 116.0f                   /* a thread's bubbles: under its header (the avatar, the number) */
#define BUB_M 16.0f                         /* a bubble's edge from the screen's (design-ref §7) */
#define CMF_PLUS_X BUB_M                     /* the + glass circle (36) */
#define CMF_X 60.0f                          /* the compose field: 60 .. 374, 36 tall (§7) */
#define CMF_W (390 - BUB_M - CMF_X)
#define CMF_TW (CMF_W - 14 - 40)              /* its text's width (the send button inside, at its right) */
#define CMF_LINES 5                          /* the field grows to five lines, then shows the last five */
/* B3: the keyboard for the field with the focus (To: a phone number on 123; the message: sentences, return a new line) */
static void cm_kb_focus(void)
{
    if (CM.focus == 1) kb_focus(KB_DIGITS | KB_PHONE, 0, CM.to);
    else if (CM.focus == 2) kb_focus(KB_AUTOCAP | KB_NL, 0, CM.txt);
}
static int cm_has_text(void)
{
    int any = 0;
    for (const char *p = CM.txt; *p; p++) any |= *p != ' ' && *p != '\n';
    return any;
}
static int cm_can_send(void) { return cm_has_text() && (AS.thread == 0 || cm_digits(CM.to) >= 7); }
/* the field's lines (its text wrapped at CMF_TW, 17 pt) */
static int cm_field_lines(char ln[CM_ML][100], float *wm) { return CM.txt[0] ? cm_wrap(ln, CM.txt, 17, CMF_TW, wm) : 1; }
static float cm_field_h(void)
{
    char ln[CM_ML][100];
    float wm;
    int n = tk_mini(CMF_LINES, cm_field_lines(ln, &wm));
    return 15 + 21.0f * n;                              /* one line: 36 pt, as iOS's capsule */
}
static float cm_field_bot(void) { return CM.focus ? kb_top() - 8 : H_PT - 30 - 8; }   /* 8 over B3's keyboard (its strip) or the home band */
static float cm_field_y(void) { return cm_field_bot() - cm_field_h(); }

/* ---- the lists' items: Recents and Favourites (Phone), the threads and a thread's bubbles (Messages) --------- */
static uint32_t cm_key(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = 2166136261u;
    h = (h ^ a) * 16777619u; h = (h ^ b) * 16777619u; h = (h ^ c) * 16777619u;
    return h | 1;                                       /* never 0 (a free slot) */
}
/* a message's lines, once per message (its text never changes): the line count and the widest line */
#define CM_BTW 268.0f                       /* a bubble's text width at most: 292 with its padding (§7) */
static struct { int id, nl; float wm; } CWC[SH_SMS_MAX];
static int cwc_n;
static void cm_measure(const cm_msg *m, int *nl, float *wm)
{
    for (int i = 0; i < cwc_n; i++) if (CWC[i].id == m->m.id) { *nl = CWC[i].nl; *wm = CWC[i].wm; return; }
    char ln[CM_ML][100];
    *nl = cm_wrap(ln, m->m.text[0] ? m->m.text : " ", 17, CM_BTW, wm);
    int k = cwc_n < SH_SMS_MAX ? cwc_n++ : 0;
    if (k == 0 && cwc_n == SH_SMS_MAX) { for (int i = 0; i + 1 < SH_SMS_MAX; i++) CWC[i] = CWC[i + 1]; k = SH_SMS_MAX - 1; }
    CWC[k].id = m->m.id; CWC[k].nl = *nl; CWC[k].wm = *wm;
}
static float cm_bubble_h(int nl) { return 14 + 22.0f * nl; }   /* one line: 36 */
static int cm_gap(const cm_msg *a, const cm_msg *b)
{
    return a->day != b->day || b->hm / 100 * 60 + b->hm % 100 - (a->hm / 100 * 60 + a->hm % 100) >= 60;
}
/* iMessage's tail (its CSS, in pt): a 16 pt square with the corner away from the bubble rounded 12, less a 10 pt
 * one rounded 8 beside the bubble: the bottom corner on the sender's side becomes the curl */
static void d_tail(dc *d, float xe, float yb, int out, tk_col col)
{
    float bx = out ? xe - 10.4f : xe - 5.6f, cx = out ? xe : xe - 10;
    tk_r4 kb = out ? (tk_r4){ 0, 0, 0, PT(12) } : (tk_r4){ 0, 0, PT(12), 0 };
    tk_r4 kc = out ? (tk_r4){ 0, 0, 0, PT(8) } : (tk_r4){ 0, 0, PT(8), 0 };
    int X0 = (int)tk_floorf(PT(xe - 12) - d->ox), X1 = (int)(PT(xe + 12) - d->ox) + 2;
    int Y0 = (int)tk_floorf(PT(yb - 17) - d->oy), Y1 = (int)(PT(yb) - d->oy) + 2;
    tk_fm m;
    if (X1 <= X0 || Y1 <= Y0 || !tk_fm_new(&m, X1 - X0, Y1 - Y0)) return;
    for (int j = 0; j < m.h; j++)
        for (int i = 0; i < m.w; i++) {
            float px = X0 + i + .5f + d->ox, py = Y0 + j + .5f + d->oy;
            float a = tk_clampf(.5f - tk_sd_rrect4(px, py, PT(bx), PT(yb - 16), PT(16), PT(16), &kb), 0, 1);
            if (a <= 0) continue;
            float c = tk_clampf(.5f - tk_sd_rrect4(px, py, PT(cx), PT(yb - 16.4f), PT(10), PT(16), &kc), 0, 1);
            m.v[j * m.w + i] = a * (1 - c);
        }
    tk_paint p = tk_solid(col);
    tk_draw_fm(d->b, &m, X0, Y0, &p, 1);
    tk_fm_free(&m);
}
#define BUB_OUT 0x30d158                    /* SMS: iOS's green (iMessage would be blue) */
#define BUB_IN 0x262628                     /* systemGray5 (dark), as iOS's received bubble */
/* the parts' heights above a bubble (the intro, the time, the gap to the one before) */
static float cm_bub_above(uint16_t f)
{
    return (f & BF_FIRST ? 8 : 0) + (f & BF_INTRO ? 20 : 0) + (f & BF_TIME ? 42 : 0) + (!(f & BF_TIME) && !(f & BF_FIRST) ? (f & BF_GROUP ? 10 : 2) : 0);
}
static void cl_bubble(dc *d, int i, float top)
{
    const cm_msg *m = &MSG[CLI[i]];
    uint16_t f = CLF[i];
    int out = !m->m.incoming, nl;
    float wm, y = top + (f & BF_FIRST ? 8 : 0);
    if (f & BF_INTRO) {
        d_text_box(d, TK_IT500, 12, 195, bl(TK_IT500, 12, y + 2), CONTENT_W, 1, "Text Message \xc2\xb7 SMS", 0, 0, C_LABEL2, 0);
        y += 20;
    }
    if (f & BF_TIME) {
        /* iOS: the day in semibold, the time regular, centred */
        char w[24], hm[12];
        cm_when(w, m, 1);
        cm_hm(hm, m->hm);
        int k = (int)tk_strlen(w) - (int)tk_strlen(hm);
        char day[20];
        for (int q = 0; q < k && q < 19; q++) day[q] = w[q];
        day[k < 19 ? k : 19] = 0;
        /* 11 / 600 + 11 / 400, secondaryLabel, 16 above and below (§7) */
        float w1 = tw(TK_IT600, 11, day, 0, 0), w2 = tw(TK_IT400, 11, hm, 0, TK_TNUM), x = 195 - (w1 + w2) / 2;
        d_text(d, TK_IT600, 11, x, bl(TK_IT600, 11, y + 15), day, 0, 0, C_LABEL2, 0);
        d_text(d, TK_IT400, 11, x + w1, bl(TK_IT400, 11, y + 15), hm, 0, TK_TNUM, C_LABEL2, 0);
        y += 42;
    } else if (!(f & BF_FIRST)) y += f & BF_GROUP ? 10 : 2;
    cm_measure(m, &nl, &wm);
    int bad = m->m.state == SH_SMS_FAILED || m->m.state == SH_SMS_UNDELIVERED;
    float bw = (nl > 1 ? CM_BTW : wm) + 24, bh = cm_bubble_h(nl), bx = out ? 390 - BUB_M - bw - (bad ? 30 : 0) : BUB_M;
    tk_col col = out ? tk_hex(BUB_OUT, bad ? .55f : 1) : tk_hex(BUB_IN, 1);
    if (bad) {                                        /* iOS's red ! beside a message that didn't go */
        float cx = 390 - BUB_M - 11, cy = y + bh / 2;
        d_rrect(d, cx - 11, cy - 11, 22, 22, 11, C_RED);
        d_rrect(d, cx - 1.2f, cy - 6.5f, 2.4f, 8, 1.2f, W(1));
        d_rrect(d, cx - 1.4f, cy + 3.2f, 2.8f, 2.8f, 1.4f, W(1));
    }
    d_rrect(d, bx, y, bw, bh, 18, col);
    if (f & BF_TAIL) d_tail(d, out ? bx + bw : bx, y + bh, out, col);
    char ln[CM_ML][100];
    cm_wrap(ln, m->m.text[0] ? m->m.text : " ", 17, CM_BTW, &wm);
    float l0 = (22 - lh(TK_IT400, 17)) / 2;
    for (int q = 0; q < nl; q++) d_text(d, TK_IT400, 17, bx + 12, bl(TK_IT400, 17, y + 7 + l0 + 22.0f * q), ln[q], 0, 0, out ? W(1) : INK, 0);
    y += bh;
    if (f & BF_STATUS) {
        const char *st = m->m.state == SH_SMS_DELIVERED ? "Delivered" : "Sent";
        tk_col sc = W(.6f);
        char fw[80];
        if (m->m.state == SH_SMS_SENDING) st = "Sending\xe2\x80\xa6";
        else if (bad) {
            int q = 0;
            for (const char *p = "Not Delivered"; *p; p++) fw[q++] = *p;
            if (m->m.why[0]) { const char *dt = " \xc2\xb7 "; while (*dt) fw[q++] = *dt++; for (const char *p = m->m.why; *p && q < 78; p++) fw[q++] = *p; }
            fw[q] = 0;
            st = fw; sc = C_RED;
        }
        d_text_box(d, TK_IT600, 11, 390 - BUB_M - 4, bl(TK_IT600, 11, y + 4), 390 - 2 * BUB_M, 2, st, 0, 0, sc, 0);
    }
}
/* a thread's row: the unread dot, the monogram, the number, the time and chevron, two lines of the last message */
static void cl_thread_row(dc *d, int i, float y)
{
    const cm_msg *m = &MSG[CLI[i]];
    if (cm_unread(m->m.number)) d_rrect(d, 5, y + 34, 10, 10, 5, C_ACCENT);
    d_monogram(d, MARGIN + 22, y + 39, 22, m->m.number);
    char when[24];
    cm_when(when, m, 0);
    float ww = tw(TK_IT400, 15, when, 0, TK_TNUM), tx = MARGIN + 56;
    d_text_box(d, TK_IT600, 17, tx, bl(TK_IT600, 17, y + 12), CONTENT_R - ww - 22 - tx, 0, m->m.number, 0, 0, INK, 0);
    d_text(d, TK_IT400, 15, CONTENT_R - 14 - ww, bl(TK_IT400, 15, y + 13), when, 0, TK_TNUM, C_LABEL2, 0);
    d_icon(d, "chev", CONTENT_R - 8, y + 15, .45f);
    char ln[4][96], flat[160];
    int q = 0;
    if (!m->m.incoming) { const char *p = "You: "; while (*p) flat[q++] = *p++; }
    for (const char *p = m->m.text; *p && q < 158; p++) flat[q++] = *p == '\n' ? ' ' : *p;
    flat[q] = 0;
    float wm;
    int nl = wrapn(ln, 2, TK_IT400, 15, flat, CONTENT_R - tx, &wm);
    for (int k = 0; k < nl; k++) d_text(d, TK_IT400, 15, tx, bl(TK_IT400, 15, y + 35 + 19 * k), ln[k], 0, 0, C_LABEL2, 0);
    if (!(CLF[i] & BF_LAST)) d_rrect(d, tx, y + THR_H - .5f, 390 - tx, .5f, 0, C_SEP);
}
static void cl_draw(dc *d, int i, float top)
{
    switch (cl_kind) {
    case CL_CALLS:
        if (!i) cl_title(d, "Calls");
        else if (CLF[i] & CF_HDR) d_text(d, TK_IT600, 15, MARGIN, bl(TK_IT600, 15, top + 12), CLI[i] ? "Recents" : "Favourites", 0, 0, C_LABEL2, 0);
        else if (CLF[i] & CF_FAV) cl_fav_row(d, &FAV[CLI[i]], top, CLF[i] & BF_LAST);
        else cl_rec_row(d, &RC[CLI[i]], top, CLF[i] & BF_LAST);
        break;
    case CL_THREADS: if (!i) cl_title(d, "Messages"); else cl_thread_row(d, i, top); break;
    case CL_BUBBLES: cl_bubble(d, i, top); break;
    default: break;
    }
}
/* the open page's list (Recents, Favourites, the threads, a thread's bubbles) from the state now: new items drawn,
 * changed ones redrawn, the rest moved (a page build or a change: never the whole page) */
static void swp_end(void);
static void mnu_close(void);
static void cm_list_build(void)
{
    swp_end();
    mnu_close();
    int kind = CL_NONE, last[SH_SMS_MAX];
    int nt = is_app("messages") && AS.thread < 0 ? cm_threads(last, SH_SMS_MAX) : 0;
    if (is_app("phone") && AS.tab == 1 && (nrc || nfav)) kind = CL_CALLS;
    else if (nt) kind = CL_THREADS;
    else if (is_app("messages") && AS.thread == 0) kind = CL_BUBBLES;
    if (kind == CL_NONE) {                            /* (another app's list is left alone) */
        if (cl_kind != CL_NONE) { scv_off(&LS); ls_nav_build(0); cl_kind = CL_NONE; }
        return;
    }
    if (kind != cl_kind) { scv_clear(&LS); LS.s = 0; }
    cl_kind = kind;
    ls_used = 1;
    LS.draw = cl_draw;
    LS.fade_t = 16; LS.fade_b = 16;
    int n = 0, end = 0;
    float y = 0;
    if (kind != CL_BUBBLES) {
        LS.top = LIST_T0;
        LS.bot = kind == CL_THREADS ? H_PT : EMPTY_TAB;
        LS.iy[0] = 0; LS.ih[0] = TITLE_H; LS.ik[0] = cm_key(kind, 0x7171u, 0); CLI[0] = -1; CLF[0] = 0;
        y = TITLE_H; n = 1;
        if (kind == CL_CALLS) {
            /* the favourites (with their header), then the calls (a header too when there are favourites) */
            for (int sct = 0; sct < 2; sct++) {
                int cnt = sct ? nrc : nfav;
                if (!cnt) continue;
                if (nfav && n < SCV_MAXI) {
                    CLI[n] = sct; CLF[n] = CF_HDR;
                    LS.iy[n] = y; LS.ih[n] = 34; LS.ik[n] = cm_key(kind, 0x4800u + (uint32_t)sct, (uint32_t)(sct ? 0 : 1));
                    y += 34; n++;
                }
                for (int i = 0; i < cnt && n < SCV_MAXI; i++, n++) {
                    int lst = i == cnt - 1;
                    CLI[n] = i; CLF[n] = (uint16_t)((sct ? 0 : CF_FAV) | (lst ? BF_LAST : 0));
                    uint32_t id = (uint32_t)(sct ? RC[i].id : FAV[i].id);
                    LS.iy[n] = y; LS.ih[n] = REC_H; LS.ik[n] = cm_key(kind, id, (uint32_t)(CLF[n] + (sct && cm_fav_has(RC[i].num) >= 0 ? 1024 : 0)));
                    y += REC_H;
                }
                if (!sct) y += 12;
            }
        } else
            for (int i = 0; i < nt && n < SCV_MAXI; i++, n++) {
                const cm_msg *m = &MSG[last[i]];
                uint32_t look = (uint32_t)(cm_unread(m->m.number) * 8 + m->m.state * 2 + (i == nt - 1)) ^ (uint32_t)(m->hm * 64 + m->day) << 8;
                CLI[n] = last[i];
                CLF[n] = i == nt - 1 ? BF_LAST : 0;
                LS.iy[n] = y; LS.ih[n] = THR_H; LS.ik[n] = cm_key(kind, (uint32_t)m->m.id, look);
                y += THR_H;
            }
        if (n > 1) LS.ih[n - 1] += 24;                  /* room at the end (over the tab bar / the home bar) */
        ls_nav_build(kind == CL_CALLS ? "Calls" : "Messages");
    } else {
        int ix[SH_SMS_MAX], c = 0, lastout = -1;
        for (int i = 0; i < nmsg; i++) if (!tk_strcmp(MSG[i].m.number, CM.thr)) { if (!MSG[i].m.incoming) lastout = c; ix[c++] = i; }
        end = CM.to_end || scv_at_end(&LS);
        CM.to_end = 0;
        LS.top = THREAD_TOP;
        LS.bot = cm_field_y() - 6;
        LS.fade_t = 14; LS.fade_b = 8;
        for (int k = 0; k < c && n < SCV_MAXI; k++, n++) {
            const cm_msg *m = &MSG[ix[k]];
            uint16_t f = 0;
            if (!k) f |= BF_FIRST | BF_INTRO | BF_TIME;
            else if (cm_gap(&MSG[ix[k - 1]], m)) f |= BF_TIME;
            else if (MSG[ix[k - 1]].m.incoming != m->m.incoming) f |= BF_GROUP;
            if (k == c - 1 || MSG[ix[k + 1]].m.incoming != m->m.incoming || cm_gap(m, &MSG[ix[k + 1]])) f |= BF_TAIL;
            if (!m->m.incoming && (m->m.state == SH_SMS_SENDING || m->m.state == SH_SMS_FAILED || m->m.state == SH_SMS_UNDELIVERED ||
                                   (k == lastout && (m->m.state == SH_SMS_SENT || m->m.state == SH_SMS_DELIVERED)))) f |= BF_STATUS;
            int nl;
            float wm;
            cm_measure(m, &nl, &wm);
            CLI[n] = ix[k]; CLF[n] = f;
            LS.iy[n] = y; LS.ih[n] = cm_bub_above(f) + cm_bubble_h(nl) + (f & BF_STATUS ? 18 : 0);
            LS.ik[n] = cm_key(CL_BUBBLES, (uint32_t)m->m.id, (uint32_t)(m->m.state * 256 + f));
            y += LS.ih[n];
        }
        if (n) LS.ih[n - 1] += 10;
        ls_nav_build(0);
    }
    scv_layout(&LS, n);
    if (end) scv_to_end(&LS);
    scv_place(&LS);
    ls_moved();
}
/* B3's keyboard (shell_kb.inc): up while a field has the focus (it slides; the compose field rides on it) */
static void cm_kb_build(void)
{
    if (!is_app("messages")) return;
    KB.want = AS.thread >= 0 && CM.focus;
    if (KB.want && !KB.focused) cm_kb_focus();
    kb_commit();
}
static void cm_field_build(void);
static void cm_to_build(void);
/* the keyboard came or went, the field grew: everything below the header moves, nothing else is drawn again */
static void cm_thread_relayout(int to_end)
{
    int end = to_end || scv_at_end(&LS);
    cm_field_build();
    cm_to_build();
    cm_kb_build();
    if (AS.thread == 0 && cl_kind == CL_BUBBLES) {
        LS.bot = cm_field_y() - 6;
        scv_layout(&LS, LS.n);
        if (end) scv_to_end(&LS);
        scv_place(&LS);
    }
    relink();
}
/* the inbox changed while Messages is open: the list's items again (a new bubble, a row's dot), not the page */
static void cm_msgs_changed(void)
{
    if (!is_app("messages") || AP.closing || ap_pending) return;
    if (AS.thread < 0) {
        int last[SH_SMS_MAX];
        int nt = cm_threads(last, SH_SMS_MAX);
        if ((nt > 0) != (cl_kind == CL_THREADS)) { build_app_page(AP.a); return; }   /* the empty state came or went */
    } else if (AS.thread != 0) return;
    cm_list_build();
    relink();
}
static void page_messages(dc *d)
{
    if (AS.thread < 0) {
        d_gcircle(d, GC_DONE);                    /* iOS 26: compose, a glass circle in the nav bar */
        d_compose(d, CONTENT_R - BACK_D / 2 + 1, NAV_MID, ACCENT());
        int last[SH_SMS_MAX];
        if (!cm_threads(last, SH_SMS_MAX)) {      /* else the threads are the list (its title the first item) */
            d_h1(d, "Messages");
            const char *l = !sh_comms_ok() ? "Text messages aren't in this version yet." :
                            cm_cell_on() ? "Texts you send and receive show here until the phone restarts." :
                            "Turn on Cellular to send and receive text messages.";
            d_empty(d, CONTENT_TOP, EMPTY_BOT, "No Messages", l);
            if (sh_comms_ok() && !cm_cell_on()) cm_cell_button(d, cm_empty_bot(CONTENT_TOP, EMPTY_BOT, l) + 18);
        }
        return;
    }
    d_back(d, W(1));
    if (AS.thread == 0) {
        /* iOS 26's thread header (§7): the 40 pt monogram centred over the name, 11 / 400 in a glass capsule */
        d_monogram(d, 195, SAFE_TOP + 20, 20, CM.thr);
        float nw = tk_minf(220, tw(TK_IT400, 11, CM.thr, 0, TK_TNUM)) + 20;
        d_rrect(d, 195 - nw / 2, SAFE_TOP + 44, nw, 20, 10, W(.16f));
        d_rrect(d, 195 - nw / 2 + .5f, SAFE_TOP + 44.5f, nw - 1, 19, 9.5f, sbc(SBC_GRAY6));
        d_text_box(d, TK_IT400, 11, 195, bl(TK_IT400, 11, SAFE_TOP + 48), 220, 1, CM.thr, 0, TK_TNUM, INK, 0);
        int any = 0;
        for (int i = 0; i < nmsg; i++) any |= !tk_strcmp(MSG[i].m.number, CM.thr);
        if (!any) d_text_box(d, TK_IT400, 13, 195, bl(TK_IT400, 13, 300), CONTENT_W, 1, "No messages with this number yet.", 0, 0, C_LABEL2, 0);
    } else {
        d_navtitle(d, "New Message");
        d_text(d, TK_IT400, 16, MARGIN + 4, bl(TK_IT400, 16, TO_Y + 13), "To:", 0, 0, C_LABEL2, 0);
        d_rrect(d, MARGIN, TO_Y + 44, CONTENT_W, .5f, 0, C_SEP);
    }
}
/* an up arrow in a circle (send) */
static void d_send(dc *d, float cx, float cy, float r, tk_col bg)
{
    d_rrect(d, cx - r, cy - r, 2 * r, 2 * r, r, bg);
    d_seg(d, cx, cy - r * .5f, cx, cy + r * .5f, 2.2f, W(1));
    d_seg(d, cx - r * .4f, cy - r * .1f, cx, cy - r * .5f, 2.2f, W(1));
    d_seg(d, cx + r * .4f, cy - r * .1f, cx, cy - r * .5f, 2.2f, W(1));
}
/* the compose field: the + button, the capsule growing with its lines (the last five shown), the caret, send */
static void cm_field_build(void)
{
    relayer(&cmfld);
    if (!is_app("messages") || AS.thread < 0) { cmfld.hidden = 1; return; }
    char ln[CM_ML][100];
    float wm;
    int nl = cm_field_lines(ln, &wm), first = nl > CMF_LINES ? nl - CMF_LINES : 0;
    float fh = cm_field_h(), fy = cm_field_bot() - fh;
    dc d = mk(&cmfld, "msg-field", CMF_PLUS_X, fy, 390 - 2 * CMF_PLUS_X, fh, 1, 0);
    /* the + (attachments: not for SMS here) */
    float pb = cm_field_bot();
    d_rrect(&d, CMF_PLUS_X, pb - 36, 36, 36, 18, W(.14f));
    d_rrect(&d, CMF_PLUS_X + .5f, pb - 35.5f, 35, 35, 17.5f, TK_RGBA(0, 0, 0, 0));
    d_seg(&d, CMF_PLUS_X + 18, pb - 25, CMF_PLUS_X + 18, pb - 11, 2.2f, W(.8f));
    d_seg(&d, CMF_PLUS_X + 11, pb - 18, CMF_PLUS_X + 25, pb - 18, 2.2f, W(.8f));
    d_rrect(&d, CMF_X, fy, CMF_W, fh, 18, W(.18f));                     /* a glass capsule: its rim, then its face */
    d_rrect(&d, CMF_X + 1, fy + 1, CMF_W - 2, fh - 2, 17, sbc(SBC_GRAY6));
    float l0 = (21 - lh(TK_IT400, 17)) / 2;
    if (CM.txt[0]) {
        float cx = CMF_X + 14, cy = fy + 7.5f + 21.0f * (nl - 1 - first);
        for (int q = first; q < nl; q++) d_text(&d, TK_IT400, 17, CMF_X + 14, bl(TK_IT400, 17, fy + 7.5f + l0 + 21.0f * (q - first)), ln[q], 0, 0, INK, 0);
        int n = (int)tk_strlen(CM.txt);
        if (n && CM.txt[n - 1] == '\n') cy += 21;      /* a new line typed last: the caret on it */
        else cx += tw(TK_IT400, 17, ln[nl - 1], 0, 0) + 1;
        if (CM.focus == 2 && cy + 21 <= fy + fh) d_rrect(&d, cx, cy + 1, 2, 19, 1, ACCENT());
    } else {
        d_text(&d, TK_IT400, 17, CMF_X + 14, bl(TK_IT400, 17, fy + 7.5f + l0), "Text Message", 0, 0, C_LABEL3, 0);
        if (CM.focus == 2) d_rrect(&d, CMF_X + 13, fy + 9, 2, 19, 1, ACCENT());
    }
    if (cm_can_send()) d_send(&d, CMF_X + CMF_W - 18, pb - 18, 15, tk_hex(BUB_OUT, 1));
    else if (cm_has_text()) d_send(&d, CMF_X + CMF_W - 18, pb - 18, 15, W(.2f));   /* cp6: dimmed until the number is whole */
    trim(&cmfld);
    cmfld.hidden = 0;
    if (CM.focus) kb_ride(&cmfld);               /* B3: it rides up with the keyboard */
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
    int was_new = AS.thread == 1;
    if (was_new) { cm_cpy(CM.thr, sizeof CM.thr, num); AS.thread = 0; CM.focus = 2; CM.num = 0; }
    CM.to_end = 1;                               /* the push draws the new bubble at the end, in view */
    if (sh_sms_send(num, CM.txt, why) < 0) {
        if (was_new) { AS.thread = 1; CM.focus = 1; CM.num = 1; }
        CM.to_end = 0;
        toast(why[0] ? why : "The message wasn't sent");
        return;
    }
    sh_haptic();
    CM.txt[0] = 0;
    CM.shift = 1;
    kb_ctx(CM.txt);                              /* B3: the keyboard capitalises again */
    if (was_new) { cm_kb_focus(); build_app_page(AP.a); }
    else cm_thread_relayout(1);                  /* the field empties (and shrinks); the push drew the bubble */
}
static void cm_open_thread(const char *num)
{
    cm_cpy(CM.thr, sizeof CM.thr, num);
    AS.thread = 0;
    CM.focus = 0;
    CM.to_end = 1;                               /* opened at its newest message */
    cm_mark_read(num);
}
static void cm_msgs_tap(float x, float y)
{
    if (AS.thread < 0) {
        if (DONE_HIT(x, y)) {                    /* compose: New Message, the To: field on the 123 layer */
            AS.thread = 1; CM.to[0] = 0; CM.focus = 1; CM.num = 1; CM.shift = 0;
            build_app_page(AP.a);
            return;
        }
        if (cl_kind != CL_THREADS) {
            const char *l = "Turn on Cellular to send and receive text messages.";
            float by = cm_empty_bot(CONTENT_TOP, EMPTY_BOT, l) + 18;
            if (sh_comms_ok() && !cm_cell_on() && y >= by && y < by + 40 && tk_fabsf(x - 195) < CM_BTN_W / 2) cm_open_cellular();
            return;
        }
        int i = scv_hit(&LS, y, 0);
        if (i >= 1) { cm_open_thread(MSG[CLI[i]].m.number); build_app_page(AP.a); }
        return;
    }
    if (BACK_HIT(x, y)) { AS.thread = -1; CM.focus = 0; build_app_page(AP.a); return; }
    float fy = cm_field_y(), fb = cm_field_bot();
    if (y >= fy - 4 && y < fb + 6) {
        if (x < CMF_X - 4) { toast("Photos and attachments can't be sent by SMS yet"); return; }
        if (x >= CMF_X + CMF_W - 40 && cm_has_text()) { cm_send(); return; }   /* cp6: a dimmed send says why (cm_send) */
        if (CM.focus != 2) { CM.focus = 2; CM.num = 0; CM.shift = !CM.txt[0]; cm_kb_focus(); cm_thread_relayout(0); }
        return;
    }
    if (AS.thread == 1 && y >= TO_Y && y < TO_Y + 44) {
        if (CM.focus != 1) { CM.focus = 1; CM.num = 1; CM.shift = 0; cm_kb_focus(); cm_thread_relayout(0); }
        return;
    }
    if (!CM.focus) return;
    if (y < KB_TOP) {                            /* above the keyboard: it goes down */
        if (y > PUSHED_TOP && y < fy - 4) { CM.focus = 0; cm_thread_relayout(0); }
        return;
    }
    /* B3: the keyboard's key (shift, 123 and the capitals are the keyboard's own: no page build for them) */
    const char *ks;
    int ev = kb_take(&ks);
    if (CM.focus == 1) {                         /* To: digits and a leading + only */
        int n = (int)tk_strlen(CM.to);
        char c = ev == KBE_TEXT && !ks[1] ? ks[0] : 0;
        if (ev == KBE_DEL && n) CM.to[n - 1] = 0;
        else if (ev == KBE_RET) { CM.focus = 2; CM.num = 0; CM.shift = !CM.txt[0]; cm_kb_focus(); cm_thread_relayout(0); return; }
        else if (c && n < 20 && ((c >= '0' && c <= '9') || (c == '+' && !n))) { CM.to[n] = c; CM.to[n + 1] = 0; }
        else if (ev == KBE_TEXT && c != ' ') { toast("Numbers only: there are no contacts yet"); return; }
        else return;
        cm_to_build(); cm_field_build(); relink();
    } else {
        float h0 = cm_field_h();
        if (!kb_edit(CM.txt, (int)sizeof CM.txt - 2)) return;
        if (cm_field_h() != h0) cm_thread_relayout(0);   /* the field grew or shrank: the bubbles above it move */
        else { cm_field_build(); relink(); }
    }
}

/* B3: the message field under a long press (the edit menu, shell_kb.inc) */
static void cm_field_redraw(void) { cm_field_build(); relink(); }
static int cm_em_target(float x, float y, emt *o)
{
    if (!is_app("messages") || AS.thread < 0) return 0;
    float fy = cm_field_y(), fb = cm_field_bot();
    if (y < fy - 4 || y >= fb + 6 || x < CMF_X || x >= CMF_X + CMF_W - 40) return 0;
    float w = tk_minf(tw(TK_IT400, 17, CM.txt, 0, 0), CMF_TW);
    *o = (emt){ CM.txt, (int)sizeof CM.txt - 2, { CMF_X + 14, fy + 7, w + 4, 22 }, cm_field_redraw };
    return 1;
}

/* ---- deleting: a thread's swipe (iOS's trailing action), a bubble's long press (a small menu) --------------- */
static tk_layer cmact, cmactw, cmmenu;
static struct { int on, open, item; float dx; char num[24]; } SWP;     /* a thread row swiped */
static struct { int on, id; float x, y, w, h; } MNU;                   /* a bubble's menu */
#define SWP_OPEN 90.0f
static tk_layer *cm_slot_of(int i)
{
    for (int k = 0; k < SCV_POOL; k++) if (LS.sk[k] && LS.sk[k] == LS.ik[i]) return &LS.L[k];
    return 0;
}
static void swp_place(void)
{
    tk_layer *l = SWP.on ? cm_slot_of(SWP.item) : 0;
    if (!SWP.on || !l) { cmact.hidden = cmactw.hidden = 1; cmact.gen++; cmactw.gen++; return; }
    float top = LS.top + LS.iy[SWP.item] - LS.s, w = -SWP.dx;
    l->x = l->bx + tk_floorf(PT(SWP.dx) + .5f);
    l->gen++;
    cmact.solid = 0xffff4245u;                       /* iOS's red, the revealed width */
    cmact.x = PT(390 - w); cmact.y = PT(top);
    cmact.solid_w = PT(w); cmact.solid_h = PT(THR_H);
    cmact.hidden = w < 1;
    cmact.gen++;
    if (cmactw.bmp.px) {
        cmactw.x = cmactw.bx - PT(w) / 2 + PT(SWP_OPEN) / 2;
        cmactw.y = cmactw.by + PT(top);
        cmactw.hidden = w < 40;
        cmactw.alpha = tk_clampf((w - 40) / 30, 0, 1);
        cmactw.gen++;
    }
}
static void swp_begin(int item)
{
    SWP.on = 1; SWP.open = 0; SWP.item = item; SWP.dx = 0;
    cm_cpy(SWP.num, sizeof SWP.num, MSG[CLI[item]].m.number);
    tk_layer_init(&cmact, "swipe-action");
    tk_layer *nx = cmactw.next;
    dc d = mk(&cmactw, "swipe-words", 390 - SWP_OPEN, 0, SWP_OPEN, THR_H, 0, 0);
    cmactw.next = nx;
    if (d.b->px) {
        d_text_box(&d, TK_IT600, 15, 390 - SWP_OPEN / 2, bl(TK_IT600, 15, THR_H / 2 - 9), SWP_OPEN - 8, 1, "Delete", 0, 0, W(1), 0);
        trim(&cmactw);
    }
    hl_off();
    relink();
}
static void swp_end(void)
{
    if (!SWP.on) return;
    SWP.dx = 0;
    swp_place();
    SWP.on = SWP.open = 0;
    relayer(&cmactw); cmactw.hidden = 1;
    cmact.hidden = 1; cmact.solid = 0;
    tk_layer *l = cm_slot_of(SWP.item);
    if (l) { l->x = l->bx; l->gen++; }
}
static void swp_delete(void)
{
    char why[SH_WHY] = "", num[24];
    cm_cpy(num, sizeof num, SWP.num);
    swp_end();
    sh_haptic();
    if (sh_sms_delete_thread(num, why) < 0) toast(why[0] ? why : "The conversation couldn't be deleted");
}
static void mnu_close(void)
{
    if (!MNU.on) return;
    MNU.on = 0;
    relayer(&cmmenu);
    cmmenu.hidden = 1;
}
/* a bubble held: the menu under it (over it near the field), Delete in red */
static void mnu_open(int item)
{
    const cm_msg *m = &MSG[CLI[item]];
    float top = LS.top + LS.iy[item] - LS.s + LS.ih[item];
    MNU.on = 1; MNU.id = m->m.id;
    MNU.w = 200; MNU.h = 48;
    MNU.x = m->m.incoming ? BUB_M : 390 - BUB_M - MNU.w;
    MNU.y = top + 8 + MNU.h > LS.bot ? LS.top + LS.iy[item] - LS.s - 8 - MNU.h : top + 8;
    if (MNU.y < LS.top) MNU.y = LS.top + 8;
    tk_layer *nx = cmmenu.next;
    dc d = mk(&cmmenu, "bubble-menu", MNU.x, MNU.y, MNU.w, MNU.h, 16, 0);
    cmmenu.next = nx;
    if (d.b->px) {
        /* a dark platter with a soft shadow (no glass bake in a touch) */
        for (int k = 4; k >= 1; k--) d_rrect(&d, MNU.x - k * 2, MNU.y - k * 2 + 4, MNU.w + k * 4, MNU.h + k * 4, 24 + k * 2, TK_RGBA(0, 0, 0, .07f));
        d_rrect(&d, MNU.x, MNU.y, MNU.w, MNU.h, 24, tk_hex(0x2c2c2e, 1));
        d_text(&d, TK_IT400, 17, MNU.x + 20, bl(TK_IT400, 17, MNU.y + 13), "Delete", 0, 0, C_RED, 0);
        /* a bin: its lid and body */
        float gx = MNU.x + MNU.w - 34, gy = MNU.y + 15;
        d_seg(&d, gx, gy + 2, gx + 14, gy + 2, 1.8f, C_RED);
        d_seg(&d, gx + 5, gy, gx + 9, gy, 1.8f, C_RED);
        d_seg(&d, gx + 2, gy + 4, gx + 3, gy + 17, 1.8f, C_RED);
        d_seg(&d, gx + 12, gy + 4, gx + 11, gy + 17, 1.8f, C_RED);
        d_seg(&d, gx + 3, gy + 17, gx + 11, gy + 17, 1.8f, C_RED);
        trim(&cmmenu);
        cmmenu.hidden = 0;
    }
    hl_off();
    sh_haptic();                                     /* iOS: the menu's lift (impact .medium) */
    relink();
}

/* ---- the shell's hooks --------------------------------------------------------------------------------------- */
static int cm_app_tap(float x, float y)
{
    if (is_app("phone")) { cm_phone_tap(x, y); return 1; }
    if (is_app("messages")) { cm_msgs_tap(x, y); return 1; }
    return 0;
}
/* a page was built (own_extras, the last of them): the keypad's line, the lists, the fields; the list goes when no
 * page of this build used it */
static void cm_extras(const char *k)
{
    if (tk_strcmp(k, "phone")) { relayer(&cmhint); cmhint.hidden = 1; cm_ring_free(); }
    else cm_hint_build();
    if (!tk_strcmp(k, "phone") && AS.tab == 1 && rc_missed) { rc_missed = 0; home_badges(0); }   /* iOS: Recents seen */
    if (!tk_strcmp(k, "phone") || !tk_strcmp(k, "messages")) cm_list_build();
    else if (cl_kind != CL_NONE) cl_kind = CL_NONE;
    cm_field_build();
    cm_to_build();
    cm_kb_build();
    if (!ls_used && LS.on) { scv_off(&LS); ls_nav_build(0); }
    ls_used = 0;
}
static int cm_layers(tk_layer **o)
{
    int n = 0;
    if (!cmhint.name) { tk_layer_init(&cmhint, "dial-line"); cmhint.hidden = 1; }
    if (!cmring.name) { tk_layer_init(&cmring, "dial-ring"); cmring.hidden = 1; }
    if (!cmfld.name) { tk_layer_init(&cmfld, "msg-field"); cmfld.hidden = 1; }
    if (!cmto.name) { tk_layer_init(&cmto, "msg-to"); cmto.hidden = 1; }
    n += ls_layers(o + n);                     /* the list and the inline title (shell_apps.inc) */
    n += ck_layers(o + n);                     /* the Clock's wheels, the Calculator's lit operator */
    if (!cmact.name) { tk_layer_init(&cmact, "swipe-action"); cmact.hidden = 1; }
    if (!cmactw.name) { tk_layer_init(&cmactw, "swipe-words"); cmactw.hidden = 1; }
    if (!cmmenu.name) { tk_layer_init(&cmmenu, "bubble-menu"); cmmenu.hidden = 1; }
    o[n++] = &cmact; o[n++] = &cmactw;
    o[n++] = &cmhint; o[n++] = &cmring; o[n++] = &cmfld; o[n++] = &cmto; o[n++] = &cmmenu;
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
    LS.s = 0;
    CM.why[0] = 0;
    CM.want_sub = 0; CM.want_view = 0; CM.want_thr[0] = 0; CM.want_tab = -1;
}
static void cm_closed(void)
{
    if (CM.own == 2 || CM.own == 3) CM.own = 0;
    CM.hold = 0;
    cm_ring_free();
    relayer(&cmhint); cmhint.hidden = 1;
    relayer(&cmfld); cmfld.hidden = 1;
    relayer(&cmto); cmto.hidden = 1;
    swp_end();
    mnu_close();
    ck_closed();
    scv_off(&LS);
    ls_nav_build(0);
    cl_kind = CL_NONE;
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
/* the list takes a touch: an app at rest with a list under the finger */
static void app_press(float x, float y);
static int cm_list_ready(void)
{
    return AP.on && !AP.closing && !sp_app.live && !ap_pending && !sw_on() && !nc_on() && !(CV.on && CV.at > 0);
}
/* every touch first: the call screen owns the screen (or its pill, while it is away); the call button's hold; a
 * list's drag (a tap on it goes to the app as any tap) */
static int cm_touch(int ph, float x, float y, float t)
{
    if (ph == 0) {
        CM.down_t = t;
        CM.own = 0;
        if (CV.on && CV.at > 0) {
            CM.own = 1;
            cv_down(x, y);
            CV.swipe = y >= H_PT - 34 && CV.mode == CVM_CALL;   /* iOS: the home gesture leaves the call (B7's capsule) */
            return 1;
        }
        if (MNU.on || (SWP.on && SWP.open)) { CM.own = 7; CM.hx = x; CM.hy = y; return 1; }
        if (!(CV.on && CV.at > 0) && ck_touch(0, x, y, t)) { CM.own = 8; return 1; }   /* a Clock wheel */
        float dx = x - PAD_CX, dy = y - PAD_CY;
        if (cm_pad_ready() && dx * dx + dy * dy <= 41 * 41) { CM.own = 2; cm_hold_begin(x, y, t); return 1; }
        if (cm_list_ready() && scv_contains(&LS, x, y)) {
            CM.own = 3;
            CM.hx = x; CM.hy = y;
            scv_touch(&LS, 0, x, y, t);
            app_press(x, y);
            return 1;
        }
        return 0;
    }
    if (!CM.own) return 0;
    if (CM.own == 8) { ck_touch(ph, x, y, t); if (ph == 2) CM.own = 0; return 1; }
    if (CM.own == 7) {                                /* the tap after an open swipe or menu: its action, or it closes */
        if (ph != 2) return 1;
        CM.own = 0;
        int moved = tk_fabsf(x - CM.hx) > 12 || tk_fabsf(y - CM.hy) > 12;
        if (MNU.on) {
            int in = !moved && x >= MNU.x && x < MNU.x + MNU.w && y >= MNU.y && y < MNU.y + MNU.h, id = MNU.id;
            mnu_close();
            relink();
            if (in) {
                char why[SH_WHY] = "";
                sh_haptic();
                if (sh_sms_delete(id, why) < 0) toast(why[0] ? why : "The message couldn't be deleted");
            }
            return 1;
        }
        float top = LS.top + LS.iy[SWP.item] - LS.s;
        if (!moved && x >= 390 - SWP_OPEN && y >= top && y < top + THR_H) swp_delete();
        else swp_end();
        return 1;
    }
    if (CM.own == 6) {                                /* a thread row slid sideways */
        float d = x - CM.hx;
        SWP.dx = d < 0 ? (d > -SWP_OPEN * 2.6f ? d : -SWP_OPEN * 2.6f + (d + SWP_OPEN * 2.6f) * .3f) : d * .2f;
        swp_place();
        if (ph == 2) {
            CM.own = 0;
            if (SWP.dx < -230) swp_delete();                    /* a full swipe: deleted at once (iOS) */
            else if (SWP.dx < -45) { SWP.open = 1; SWP.dx = -SWP_OPEN; swp_place(); }
            else swp_end();
        }
        return 1;
    }
    if (CM.own == 3 && ph == 1 && !LS.moved && cl_kind == CL_THREADS && tk_fabsf(x - CM.hx) > 10 && tk_fabsf(x - CM.hx) > 1.5f * tk_fabsf(y - CM.hy)) {
        int i = scv_hit(&LS, CM.hy, 0);
        if (i >= 1) { LS.down = 0; CM.own = 6; swp_begin(i); SWP.dx = x - CM.hx; swp_place(); return 1; }
    }
    if (CM.own == 3) {
        int tap = scv_touch(&LS, ph, x, y, t);
        if (LS.moved) { hl_off(); ls_moved(); }
        if (ph == 2) { CM.own = 0; hl_off(); if (tap && AP.on) app_tap(x, y); }
        return 1;
    }
    if (CM.own == 5) { if (ph == 2) CM.own = 0; return 1; }   /* the rest of the swipe that hid the call screen */
    int moved = tk_fabsf(x - (CM.own == 1 ? CV.x0 : CM.hx)) > 12 || tk_fabsf(y - (CM.own == 1 ? CV.y0 : CM.hy)) > 12;
    if (CM.own == 1) {
        if (!CV.on) { if (ph == 2) CM.own = 0; return 1; }
        if (CV.swipe && CV.y0 - y > 60 && CV.mode == CVM_CALL) { cv_minimise(); CM.own = ph == 2 ? 0 : 5; return 1; }
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
    if (scv_tick(&LS, dt)) { ls_moved(); mv = 1; }
    mv |= ck_tick(dt);
    if (CM.own == 3 && LS.down && !LS.moved && cl_kind == CL_BUBBLES && !MNU.on && t - CM.down_t >= 450) {
        int i = scv_hit(&LS, CM.hy, 0);                /* a bubble held: its menu (the lift then does nothing) */
        if (i >= 0) { LS.down = 0; CM.own = 5; mnu_open(i); }
        mv = 1;
    } else if (CM.own == 3 && LS.down && !LS.moved && cl_kind == CL_BUBBLES) mv = 1;
    if (CV.on && !CV.slide && CV.thumb > 0 && CV.mode == CVM_INCOMING && !CV.answering) {
        CV.thumb = CV.thumb < 1 ? 0 : CV.thumb * tk_powf(.001f, dt * 2.5f);
        cv_place();
        mv = 1;
    }
    return mv;
}
static int cm_moving(void)
{
    return CM.hold || (CV.on && CV.a != CV.at) || CV.slide || (CV.on && CV.thumb > 0 && CV.mode == CVM_INCOMING && !CV.answering) ||
           scv_moving(&LS) || ck_moving();
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
    if (!is_app("phone") && !is_app("messages")) return 0;
    if (CV.on && CV.at > 0) return 0;                     /* (the call screen lights its own: cv_light) */
    if (LS.on && (cl_kind == CL_CALLS || cl_kind == CL_THREADS) && scv_contains(&LS, x, y)) {
        float top;
        int i = scv_hit(&LS, y, &top);
        if (i < 1 || (cl_kind == CL_CALLS && (CLF[i] & CF_HDR))) return 0;
        float rh = cl_kind == CL_THREADS ? THR_H : REC_H;
        *g = (tk_rect){ 0, LS.top, 390, LS.bot - LS.top };
        if (cl_kind != CL_THREADS && x >= INFO_X - 22) { *r = (tk_rect){ INFO_X - 16, top + rh / 2 - 16, 32, 32 }; *rad = 16; *c = APR_CIRC; return 1; }
        *r = (tk_rect){ 0, top, 390, rh };
        *rad = 0; *c = APR_ROW;
        return 1;
    }
    if (is_app("phone")) {
        /* cp6: the tabs; the empty Calls' button; the keypad's "Turn On in Settings" and delete (its keys: key_down) */
        if (tab_press(x, y, 3, r, g, rad, c)) return 1;
        if (AS.tab == 1 && !nrc && !nfav && sh_comms_ok() && !cm_cell_on()) {
            float by = cm_empty_bot(CONTENT_TOP, EMPTY_TAB, "Calls you make and receive show here. Turn on Cellular to call.") + 18;
            if (y >= by && y < by + 40 && tk_fabsf(x - 195) < CM_BTN_W / 2) {
                *r = *g = (tk_rect){ 195 - CM_BTN_W / 2, by, CM_BTN_W, 40 }; *rad = 20; *c = APR_CIRC;
                return 1;
            }
        }
        if (AS.tab != 3) return 0;
        if (hint_cell && y >= HINT_Y - 4 && y < HINT_Y + 34) { *r = *g = (tk_rect){ MARGIN, HINT_Y, CONTENT_W, 30 }; *rad = 8; *c = APR_DIM; return 1; }
        float ex = x - 299, ey = y - 605;
        if (AS.num[0] && ex * ex + ey * ey <= 30 * 30) { *r = *g = (tk_rect){ 299 - 24, 605 - 18, 48, 36 }; *rad = 10; *c = APR_DIM; return 1; }
        return 0;
    }
    if (AS.thread < 0) {
        if (DONE_HIT(x, y)) { *r = *g = (tk_rect){ CONTENT_R - BACK_D, SAFE_TOP, BACK_D, BACK_D }; *rad = BACK_D / 2; *c = APR_CIRC; return 1; }
        if (cl_kind != CL_THREADS && sh_comms_ok() && !cm_cell_on()) {          /* cp6: the empty list's Cellular Settings */
            float by = cm_empty_bot(CONTENT_TOP, EMPTY_BOT, "Turn on Cellular to send and receive text messages.") + 18;
            if (y >= by && y < by + 40 && tk_fabsf(x - 195) < CM_BTN_W / 2) {
                *r = *g = (tk_rect){ 195 - CM_BTN_W / 2, by, CM_BTN_W, 40 }; *rad = 20; *c = APR_CIRC;
                return 1;
            }
        }
        return 0;
    }
    float fb = cm_field_bot();
    if (y >= cm_field_y() - 4 && y < fb + 6 && x >= CMF_X + CMF_W - 40 && cm_can_send()) {
        *r = *g = (tk_rect){ CMF_X + CMF_W - 33, fb - 33, 30, 30 }; *rad = 15; *c = APR_CELL;   /* the green disc darkens */
        return 1;
    }
    if (y >= fb - 40 && y < fb + 6 && x < CMF_X - 4) { *r = *g = (tk_rect){ CMF_PLUS_X, fb - 36, 36, 36 }; *rad = 18; *c = APR_CIRC; return 1; }
    return 0;
}
/* the host's stills: "thread" NUMBER (list, new) with v the focus (0 none, 1 To, 2 the message); "compose" TEXT;
 * "to" NUMBER; "dial" NUMBER; "recent" NUMBER with v the kind (RC_*); "callpad" DIGITS (the tones' keypad, v 1);
 * "callhold" v (the call button's ring at v); "slide" v (the answer slider's thumb at v) */
static int cm_set(const char *what, const char *key, float v)
{
    if (ck_set(what, key, v)) return 1;               /* B5: the Clock's stills */
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
    if (!tk_strcmp(what, "lscroll")) { LS.s = v < 0 ? scv_max(&LS) : tk_clampf(v, 0, scv_max(&LS)); scv_place(&LS); ls_moved(); return 1; }
    if (!tk_strcmp(what, "callmini")) {             /* (B7's island=min:1 settles the capsule too) */
        if (v > 0) { if (CV.on && cm_live(CALL.state)) { isl_call_min = 1; cv_free(); CV.a = CV.at = 0; } }
        else cv_reopen();
        return 1;
    }
    if (!tk_strcmp(what, "fav")) { if (cm_fav_has(key) < 0) cm_fav_toggle(key); if (AP.on) build_app_page(AP.a); return 1; }
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
