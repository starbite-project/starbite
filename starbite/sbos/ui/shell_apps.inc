/* SPDX-License-Identifier: MIT
 * helper B5: the core apps' shared parts (included by shell_lock.c before shell_comms.inc):
 *   - SCV, a scrolling list of drawn items (Messages' threads and bubbles, Phone's Recents and Favourites, Clock's
 *     lists): each item is a small layer of its own, drawn when it comes into view and freed when it leaves, so a list
 *     costs about the screen's area at most and a change redraws only the item that changed (a sent bubble, a row's
 *     dot), never the page. iOS's scrolling: the finger, momentum (.998 / ms), rubber bands and the rows spring;
 *   - the contact-like avatar (a monogram on iOS's grey gradient, or the person glyph for a number);
 *   - the dock's badges (missed calls, unread messages): one small layer over home's dock, never the dock's glass.
 * Nothing here bakes glass or blurs; every list redraw is per item. */

/* ---- the list ---------------------------------------------------------------------------------------------- */
#define SCV_POOL 40                         /* items on screen at most (a 30 pt row fits 25 in a page) */
#define SCV_MAXI 216
typedef struct {
    int on;
    float top, bot;                         /* the viewport (screen pt) */
    float fade_t, fade_b;                   /* iOS's scroll edges: content fades out under the bars (pt) */
    int n;
    float iy[SCV_MAXI], ih[SCV_MAXI];       /* items' tops in the content (0 = the viewport's top at scroll 0), heights */
    uint32_t ik[SCV_MAXI];                  /* items' keys: a kept bitmap is reused while its key and height stay */
    float h;                                /* the content's height */
    float s, v, base, x0, y0;               /* the scroll (pt; 0 = the top) */
    int momentum, down, moved;
    tk_spring sp;
    tk_tracker tr;
    void (*draw)(dc *d, int i, float top);  /* item i with its top at screen y top (the scroll at 0) */
    tk_layer g;                             /* the clipped group */
    tk_layer L[SCV_POOL];
    uint32_t sk[SCV_POOL];                  /* the slot's item key (0: free) */
    float sy[SCV_POOL];                     /* the content y it was drawn at */
    float sh[SCV_POOL];
} scv_t;
static scv_t LS;                            /* the open app's list (one app is open at a time) */

static float scv_max(const scv_t *v) { return tk_maxf(0, v->h - (v->bot - v->top)); }
static void scv_free_slot(scv_t *v, int k)
{
    relayer(&v->L[k]);
    v->L[k].hidden = 1;
    v->sk[k] = 0;
}
/* every item's bitmap gone (a new page) */
static void scv_clear(scv_t *v)
{
    for (int k = 0; k < SCV_POOL; k++) if (v->sk[k] || v->L[k].bmp.px) scv_free_slot(v, k);
}
static void scv_off(scv_t *v)
{
    scv_clear(v);
    v->on = 0;
    v->g.hidden = 1;
    v->momentum = v->down = 0;
    v->sp.live = 0;
}
static int scv_round_px(float pt) { return (int)tk_floorf(PT(pt) + .5f); }
/* the items in view drawn (if they are not), the rest freed, everything placed at the scroll */
static void scv_place(scv_t *v)
{
    if (!v->on) return;
    float pre = 24;
    float a = v->s - pre, b = v->s + (v->bot - v->top) + pre;
    int sp = scv_round_px(v->s);
    /* slots: keep the ones whose item is still here (by key) and in view; move them to its place */
    int at[SCV_POOL];
    for (int k = 0; k < SCV_POOL; k++) {
        at[k] = -1;
        if (!v->sk[k]) continue;
        int i = 0;
        while (i < v->n && v->ik[i] != v->sk[k]) i++;
        if (i == v->n || v->ih[i] != v->sh[k] || v->iy[i] + v->ih[i] < a || v->iy[i] > b) { scv_free_slot(v, k); continue; }
        at[k] = i;
    }
    for (int i = 0; i < v->n; i++) {
        if (v->iy[i] + v->ih[i] < a || v->iy[i] > b) continue;
        int k = 0;
        while (k < SCV_POOL && at[k] != i) k++;
        if (k == SCV_POOL) {
            for (k = 0; k < SCV_POOL && v->sk[k]; k++) ;
            if (k == SCV_POOL) break;                       /* (never: the pool covers a page) */
            float top = v->top + v->iy[i];
            tk_layer *nx = v->L[k].next;               /* (mk inits the layer: its place in the list is kept) */
            dc d = mk(&v->L[k], "list-item", 0, top, 390, v->ih[i], 0, 0);
            v->L[k].next = nx;
            v->sk[k] = v->ik[i]; v->sy[k] = v->iy[i]; v->sh[k] = v->ih[i];
            at[k] = i;
            if (d.b->px) { v->draw(&d, i, top); trim(&v->L[k]); }
        }
        tk_layer *l = &v->L[k];
        l->hidden = !l->bmp.px;
        l->x = l->bx;
        l->y = l->by + (float)(scv_round_px(v->iy[i]) - scv_round_px(v->sy[k]) - sp);
        l->gen++;
    }
    v->g.clip = 1;
    v->g.clip_r = (tk_rect){ 0, PT(v->top), TK_W, PT(v->bot - v->top) };
    v->g.fade_top = v->s > 1 ? PT(v->fade_t) : 0;          /* nothing under the bar at the top: no fade there */
    v->g.fade_bot = v->s < scv_max(v) - 1 ? PT(v->fade_b) : 0;
    v->g.hidden = 0;
    v->g.gen++;
}
/* a new list (the items' iy / ih / ik filled by the caller, n items): the content's height, the scroll kept in range */
static void scv_layout(scv_t *v, int n)
{
    v->n = n < SCV_MAXI ? n : SCV_MAXI;
    v->h = v->n ? v->iy[v->n - 1] + v->ih[v->n - 1] : 0;
    v->s = tk_clampf(v->s, 0, scv_max(v));
    v->on = 1;
}
/* item i drawn again (its look changed but not its key) */
__attribute__((unused)) static void scv_redraw(scv_t *v, int i)
{
    for (int k = 0; k < SCV_POOL; k++) if (v->sk[k] && v->sk[k] == v->ik[i]) scv_free_slot(v, k);
}
static void scv_to_end(scv_t *v) { v->s = scv_max(v); v->momentum = 0; v->sp.live = 0; }
static int scv_at_end(const scv_t *v) { return v->s >= scv_max(v) - 2; }
/* the item under screen y (or -1), its screen top in *top */
static int scv_hit(const scv_t *v, float y, float *top)
{
    if (!v->on || y < v->top || y >= v->bot) return -1;
    float cy = y - v->top + v->s;
    for (int i = 0; i < v->n; i++)
        if (cy >= v->iy[i] && cy < v->iy[i] + v->ih[i]) { if (top) *top = v->top + v->iy[i] - v->s; return i; }
    return -1;
}
__attribute__((unused)) static float scv_screen_y(const scv_t *v, int i) { return v->top + v->iy[i] - v->s; }
static int scv_contains(const scv_t *v, float x, float y)
{
    (void)x;
    return v->on && v->n && y >= v->top && y < v->bot && y < H_PT - 30;
}
/* the finger: 0 down, 1 move, 2 up; returns 1 at an up that was a tap (no move) */
static int scv_touch(scv_t *v, int ph, float x, float y, float t)
{
    if (ph == 0) {
        v->down = 1; v->moved = 0; v->x0 = x; v->y0 = y; v->base = v->s;
        v->momentum = 0; v->sp.live = 0;
        tk_tracker_reset(&v->tr);
        tk_tracker_add(&v->tr, y, t);
        return 0;
    }
    float dy = y - v->y0;
    if (ph == 1) {
        tk_tracker_add(&v->tr, y, t);
        if (!v->moved && (tk_fabsf(dy) > 6 || tk_fabsf(x - v->x0) > 8)) { v->moved = 1; v->base = v->s + (dy > 0 ? 6 : dy < 0 ? -6 : 0); }
        if (!v->moved) return 0;
        float s = v->base - dy, m = scv_max(v), vp = v->bot - v->top;
        if (s < 0) s = -md_rubber(-s, vp);
        else if (s > m) s = m + md_rubber(s - m, vp);
        v->s = s;
        scv_place(v);
        return 0;
    }
    v->down = 0;
    if (!v->moved) return 1;
    float vel = -tk_tracker_v(&v->tr), m = scv_max(v);
    if (v->s < 0 || v->s > m) tk_spring_start(&v->sp, v->s, v->s < 0 ? 0 : m, vel * 1000, TK_SPR_ROWS);
    else if (tk_fabsf(vel) > .05f) { v->v = vel; v->momentum = 1; }
    return 0;
}
static int scv_moving(const scv_t *v) { return v->on && ((v->down && v->moved) || v->momentum || v->sp.live); }
static int scv_tick(scv_t *v, float dt)
{
    if (!v->on) return 0;
    if (v->sp.live) {
        tk_spring_step(&v->sp, dt);
        v->s = v->sp.x;
        scv_place(v);
        return 1;
    }
    if (v->momentum) {
        float ms = dt * 1000, m = scv_max(v), decay = tk_powf(.998f, ms);
        v->s += v->v * (1 - decay) / (1 - .998f);
        v->v *= decay;
        if (v->s < 0 || v->s > m) {
            v->momentum = 0;
            /* past an end: a short rubber overshoot from its speed, then the spring back */
            tk_spring_start(&v->sp, v->s, v->s < 0 ? 0 : m, v->v * 1000, TK_SPR_ROWS);
        } else if (tk_fabsf(v->v) < .01f) v->momentum = 0;
        scv_place(v);
        return 1;
    }
    return 0;
}
static int scv_layers(scv_t *v, tk_layer **o)
{
    if (!v->g.name) { tk_layer_init(&v->g, "list"); v->g.hidden = 1; }
    for (int k = 0; k < SCV_POOL; k++) if (!v->L[k].name) { tk_layer_init(&v->L[k], "list-item"); v->L[k].hidden = 1; }
    tk_layer *c[SCV_POOL];
    for (int k = 0; k < SCV_POOL; k++) c[k] = &v->L[k];
    link(&v->g, c, SCV_POOL);
    o[0] = &v->g;
    return 1;
}

/* the large-title lists: the viewport's top under the nav row, the title the first item; the inline title in the
 * nav row once the large one has scrolled under it */
#define LIST_T0 (SAFE_TOP + NAVBAR_H)
#define TITLE_H (LARGE_H + 10)
static tk_layer lsnav;
static int ls_used;                          /* a page of this build used LS (else cm_extras, the last, turns it off) */
static void ls_moved(void)
{
    int show = lsnav.bmp.px && LS.on && LS.s > TITLE_H - 16;
    if (lsnav.hidden == show) { lsnav.hidden = !show; lsnav.gen++; }
}
static void ls_nav_build(const char *t)
{
    relayer(&lsnav);
    lsnav.hidden = 1;
    if (!t) return;
    dc d = mk(&lsnav, "nav-title", 80, SAFE_TOP, 230, NAVBAR_H, 0, 0);
    d_text_box(&d, TK_IT600, 17, 195, NAV_BASE, 220, 1, t, 0, 0, INK, 0);
    trim(&lsnav);
}
static int ls_layers(tk_layer **o)
{
    int n = scv_layers(&LS, o);
    if (!lsnav.name) { tk_layer_init(&lsnav, "nav-title"); lsnav.hidden = 1; }
    o[n++] = &lsnav;
    return n;
}

/* ---- a page's strips cut to what they hold (the apps here draw their lists as items: a strip is mostly empty) -- */
static int ap_slim(const char *k)
{
    return !tk_strcmp(k, "phone") || !tk_strcmp(k, "messages") || !tk_strcmp(k, "clock") || !tk_strcmp(k, "calc") ||
           !tk_strcmp(k, "notes") || !tk_strcmp(k, "camera") || !tk_strcmp(k, "contacts");
}
static void slim_strip(tk_layer *l)
{
    const tk_bmp *b = &l->bmp;
    if (!b->px) return;
    size_t n = (size_t)b->w * b->h;
    size_t i = 0;
    while (i < n && !b->px[i]) i++;
    if (i == n) { relayer(l); l->hidden = 1; return; }      /* nothing drawn in it: no bitmap at all */
    trim(l);
}

/* ---- the contact-like avatar ------------------------------------------------------------------------------- */
/* iOS's monogram: a grey gradient disc with the initials (a name), or the person (a number or nothing) */
static void d_person(dc *d, float cx, float cy, float r);
static void d_monogram(dc *d, float cx, float cy, float r, const char *who)
{
    int letters = who && ((who[0] >= 'A' && who[0] <= 'Z') || (who[0] >= 'a' && who[0] <= 'z'));
    if (!letters) { d_person(d, cx, cy, r); return; }
    tk_stop st[2] = { { 0, tk_hex(0xa5a9b4, 1) }, { 1, tk_hex(0x7c808c, 1) } };
    tk_paint g = tk_linear(180, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), 2, st);
    tk_fill_rrect(d->b, PT(cx - r) - d->ox, PT(cy - r) - d->oy, PT(2 * r), PT(2 * r), PT(r), &g, 1);
    char in[3];
    int k = 0;
    in[k++] = (char)(who[0] >= 'a' ? who[0] - 32 : who[0]);
    for (const char *c = who; *c && k < 2; c++)
        if (*c == ' ' && c[1] && c[1] != ' ') in[k++] = (char)(c[1] >= 'a' && c[1] <= 'z' ? c[1] - 32 : c[1]);
    in[k] = 0;
    float fs = r * .82f, w = tw(TK_IT600, fs, in, 0, 0);
    d_text(d, TK_IT600, fs, cx - w / 2, bl(TK_IT600, fs, cy - lh(TK_IT600, fs) / 2), in, 0, 0, W(1), 0);
}

/* ---- the dock's badges ----------------------------------------------------------------------------------------- */
static int dk_drawn[2] = { -1, -1 };
static int cm_badge_calls(void);           /* shell_comms.inc */
static int cm_badge_msgs(void);
/* the badge (docs/design GAP 21): a red #FF4245 circle 18 (a capsule past one digit), 12 / 600 white */
static void d_badge(dc *d, float cx, float cy, int n)
{
    char b[8];
    int k = n > 99 ? (b[0] = '9', b[1] = '9', b[2] = '+', b[3] = 0, 3) : put_dec(b, n);
    b[k] = 0;
    float w = tw(TK_IT600, 12, b, 0, TK_TNUM), bw = tk_maxf(18, w + 10);
    d_rrect(d, cx - bw / 2, cy - 9, bw, 18, 9, tk_hex(0xff4245, 1));
    d_text(d, TK_IT600, 12, cx - w / 2, bl(TK_IT600, 12, cy - lh(TK_IT600, 12) / 2), b, 0, TK_TNUM, W(1), 0);
}
static void dk_badges(void)
{
    int c = cm_badge_calls(), m = cm_badge_msgs();
    if (c == dk_drawn[0] && m == dk_drawn[1] && (dkbadge.bmp.px || (!c && !m))) return;
    dk_drawn[0] = c; dk_drawn[1] = m;
    relayer(&dkbadge);
    dkbadge.hidden = 1;
    if (!c && !m) return;
    /* the dock's first two icons (Phone, Messages): their top-right corners */
    dc d = mk(&dkbadge, "dock-badges", COL_CX(0), DOCK_Y - 14, COL_CX(1) - COL_CX(0) + 60, 30, 0, 0);
    if (!d.b->px) return;
    if (c) d_badge(&d, COL_CX(0) + ICON_PT / 2 - 5, DOCK_Y + 5, c);
    if (m) d_badge(&d, COL_CX(1) + ICON_PT / 2 - 5, DOCK_Y + 5, m);
    trim(&dkbadge);
    dkbadge.hidden = 0;
    relink();
}

/* ---- iOS 26's controls drawn here: the toggle (§12.4), a glass circle button, a text capsule ------------------- */
static void d_toggle(dc *d, float x, float y, int on)
{
    d_rrect(d, x, y, 64, 28, 14, on ? tk_hex(0x30d158, 1) : tk_hex(0x39393d, 1));
    d_rrect(d, on ? x + 24 : x + 2, y + 2, 38, 24, 12, W(1));
}
static void d_capsule_btn(dc *d, float x, float y, const char *t, tk_col fg, int bold)
{
    float w = tw(bold ? TK_IT600 : TK_IT500, 17, t, 0, 0) + 28;
    d_rrect(d, x, y, w, 44, 22, W(.14f));
    d_text(d, bold ? TK_IT600 : TK_IT500, 17, x + 14, bl(TK_IT500, 17, y + 12), t, 0, 0, fg, 0);
}
static float capsule_w(const char *t, int bold) { return tw(bold ? TK_IT600 : TK_IT500, 17, t, 0, 0) + 28; }

/* ---- the Clock (iOS 26: World Clock, Alarms, Stopwatch, Timers in a glass tab bar) --------------------------- *
 * The stopwatch's and the timer's times are CLK's (shell_lock.c: sh_next_update comes back for them); the alarms are
 * the kernel's four (alarm.c, sh_alarm_set: kept by the shell while it runs, the lock's widget from the earliest).
 * The page's static parts are in the strips; the digits, the buttons, the ring and the wheels are layers of their
 * own, redrawn only when their text changes; the lists (cities, alarms, laps) are LS's items. */
#define CK_ORANGE 0xff9230                   /* the Clock's tint (iOS's orange, dark) */
#define CK_NAL 4
#define CK_WP 36.0f                          /* a wheel's row pitch */
static tk_layer ckwheel;
static struct {
    int tab;                                 /* 0 World Clock, 1 Alarms, 2 Stopwatch, 3 Timers */
    int nal, seeded;
    struct { int h, m, on; } al[CK_NAL];
    int edit;                                /* the alarm editor: -1 shut, 0..3 an alarm, CK_NAL a new one */
    int wv[3], ev[2];                        /* the wheels: the timer's h, m, s; the alarm editor's h, m */
    float woff[3];                           /* a wheel's drag (pt; + down) */
    int wcol, wdown;
    float wy0, wbase;
    tk_tracker wtr;
    float wvel[3];
    int tm_paused;
    float tm_left, tm_total;                 /* the paused timer's time left; its whole length (ms) */
    int nlap;
    float lap[100];                          /* each finished lap's length (ms) */
    int drawn_lap_ds;
} CK = { .tab = 3, .edit = -1 };
#define CKWV (CK.edit >= 0 ? CK.ev : CK.wv)          /* the wheels shown now */
static void ck_seed(void)
{
    if (CK.seeded) return;
    CK.seeded = 1;
    if (ALM.on && !CK.nal) { CK.al[0].h = ALM.h; CK.al[0].m = ALM.m; CK.al[0].on = 1; CK.nal = 1; }
}
/* the kernel's list from the shell's (refused in the self test: then only the screen changes) */
static void ck_alarms_push(void)
{
    int fh = 0, fm = 0, any = 0;
    for (int i = 1; i < CK.nal; i++)                 /* by time of day, as iOS lists them */
        for (int k = i; k > 0 && CK.al[k - 1].h * 60 + CK.al[k - 1].m > CK.al[k].h * 60 + CK.al[k].m; k--) {
            int h = CK.al[k].h, m = CK.al[k].m, on = CK.al[k].on;
            CK.al[k] = CK.al[k - 1];
            CK.al[k - 1].h = h; CK.al[k - 1].m = m; CK.al[k - 1].on = on;
        }
    for (int i = 0; i < CK_NAL; i++) {
        int on = i < CK.nal && CK.al[i].on;
        sh_alarm_set(i, i < CK.nal ? CK.al[i].h : 0, i < CK.nal ? CK.al[i].m : 0, 0, on);
        if (on && (!any || CK.al[i].h * 60 + CK.al[i].m < fh * 60 + fm)) { fh = CK.al[i].h; fm = CK.al[i].m; any = 1; }
    }
    sh_alarm(any, fh, fm);                   /* the lock's widget (the kernel says the same at its next status) */
}
static void ck_hm(char *b, int h, int m)
{
    b[0] = (char)('0' + h / 10); b[1] = (char)('0' + h % 10); b[2] = ':'; b[3] = (char)('0' + m / 10); b[4] = (char)('0' + m % 10); b[5] = 0;
}
/* the stopwatch's time as iOS: mm:ss,hh (hundredths) */
static int ck_fmt_sw(char *b, float ms)
{
    int cs = (int)(ms / 10), s = cs / 100, m = s / 60, n = 0;
    if (m >= 60) { n += put_dec(b, m / 60); b[n++] = ':'; m %= 60; }
    b[n++] = (char)('0' + m / 10 % 10); b[n++] = (char)('0' + m % 10); b[n++] = ':';
    b[n++] = (char)('0' + s % 60 / 10); b[n++] = (char)('0' + s % 10); b[n++] = ',';
    b[n++] = (char)('0' + cs % 100 / 10); b[n++] = (char)('0' + cs % 10);
    b[n] = 0;
    return n;
}

/* the world's clocks: offsets and summer time from the date in UTC (EU: last Sundays of March and October at 01:00
 * UTC; US: the second Sunday of March and the first of November at 02:00 local) */
typedef struct { const char *city; int off_min, dst; } wcity;   /* dst: 0 none, 1 EU, 2 US */
static const wcity WC[5] = { { "Cupertino", -480, 2 }, { "New York", -300, 2 }, { "London", 0, 1 }, { "Helsinki", 120, 1 }, { "Tokyo", 540, 0 } };
static void civil(long long days, int *y, int *m, int *d)
{
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}
static long long days_of(int y, int m, int d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
/* the day of month of the n-th (n > 0) or last (n = 0) Sunday of month m */
static int sunday(int y, int m, int n)
{
    if (n) { long long d1 = days_of(y, m, 1); int wd = (int)((d1 % 7 + 11) % 7); return 1 + (7 - wd) % 7 + 7 * (n - 1); }   /* 1970-01-01 a Thursday */
    int last = m == 12 ? 31 : (int)(days_of(y, m + 1, 1) - days_of(y, m, 1));
    long long dl = days_of(y, m, last);
    int wd = (int)((dl % 7 + 11) % 7);
    return last - wd;
}
static int wc_off(const wcity *c, long long utc)
{
    int y, mo, d;
    civil(utc / 86400, &y, &mo, &d);
    long long a = 0, b = 0;
    if (c->dst == 1) { a = days_of(y, 3, sunday(y, 3, 0)) * 86400 + 3600; b = days_of(y, 10, sunday(y, 10, 0)) * 86400 + 3600; }
    else if (c->dst == 2) {
        a = days_of(y, 3, sunday(y, 3, 2)) * 86400 + 7200 - c->off_min * 60LL;
        b = days_of(y, 11, sunday(y, 11, 1)) * 86400 + 7200 - (c->off_min + 60) * 60LL;
    }
    return c->off_min + (c->dst && utc >= a && utc < b ? 60 : 0);
}
static int local_off(long long now)
{
    long long loc = (long long)ST.h * 3600 + ST.mi * 60, off = loc - now % 86400;
    while (off > 14 * 3600) off -= 86400;
    while (off < -12 * 3600) off += 86400;
    return (int)((off + 450) / 900 * 15);
}

/* the lists' items (LS): cities, alarms, laps */
enum { CKL_NONE, CKL_CITIES, CKL_ALARMS, CKL_LAPS };
static int ckl_kind;
static void ck_city_row(dc *d, int i, float y)
{
    long long now = sh_utc();
    const wcity *c = &WC[i];
    char ln[48], hm[8];
    if (now > 0) {
        int lo = local_off(now), co = wc_off(c, now), dmin = co - lo;
        long long t = now + co * 60LL;
        int s = (int)(t % 86400 + 86400) % 86400, ld = (int)((now + lo * 60LL) / 86400), cd = (int)(t / 86400);
        ck_hm(hm, s / 3600, s / 60 % 60);
        int k = md_cat(ln, 0, sizeof ln, cd > ld ? "Tomorrow, " : cd < ld ? "Yesterday, " : "Today, ");
        if (dmin) {
            ln[k++] = dmin > 0 ? '+' : '-';
            int a = dmin > 0 ? dmin : -dmin;
            k += put_dec(ln + k, a / 60);
            if (a % 60) { ln[k++] = ':'; ln[k++] = (char)('0' + a % 60 / 10); ln[k++] = (char)('0' + a % 10); }
            md_cat(ln, k, sizeof ln, "HRS");
        } else md_cat(ln, k, sizeof ln, "+0HRS");
    } else { md_cat(hm, 0, sizeof hm, "--:--"); md_cat(ln, 0, sizeof ln, "The clock isn't set"); }
    d_text(d, TK_IT400, 14, MARGIN, bl(TK_IT400, 14, y + 18), ln, 0, 0, W(.55f), 0);
    d_text(d, TK_IT400, 24, MARGIN, bl(TK_IT400, 24, y + 40), c->city, 0, 0, INK, 0);
    float w = tw(TK_IT400, 50, hm, 0, TK_TNUM);
    d_text(d, TK_IT400, 50, CONTENT_R - w, bl(TK_IT400, 50, y + 20), hm, 0, TK_TNUM, INK, 0);
    if (i < 4) d_rrect(d, MARGIN, y + 95.5f, 390 - MARGIN, .5f, 0, W(.12f));
}
static void ck_alarm_row(dc *d, int i, float y)
{
    char hm[8];
    ck_hm(hm, CK.al[i].h, CK.al[i].m);
    tk_col c = W(CK.al[i].on ? 1 : .5f);
    d_text(d, TK_IT400, 52, MARGIN, bl(TK_IT400, 52, y + 12), hm, 0, TK_TNUM, c, 0);
    d_text(d, TK_IT400, 15, MARGIN, bl(TK_IT400, 15, y + 70), "Alarm", 0, 0, c, 0);
    d_toggle(d, CONTENT_R - 64, y + 34, CK.al[i].on);
    if (i < CK.nal - 1) d_rrect(d, MARGIN, y + 95.5f, 390 - MARGIN, .5f, 0, W(.12f));
}
static void ck_lap_row(dc *d, int i, float y)
{
    /* row 0: the lap running now; then the finished ones, the newest first; the best green, the worst red */
    char t[24], l[16];
    int n = CK.nlap, li = i == 0 ? n : n - i;              /* lap number - 1 */
    float ms;
    if (i == 0) { float acc = 0; for (int k = 0; k < n; k++) acc += CK.lap[k]; ms = clk_sw_ms(now_ms) - acc; }
    else ms = CK.lap[li];
    tk_col c = INK;
    if (i && n >= 2) {
        int best = 0, worst = 0;
        for (int k = 1; k < n; k++) { if (CK.lap[k] < CK.lap[best]) best = k; if (CK.lap[k] > CK.lap[worst]) worst = k; }
        if (li == best) c = tk_hex(0x30d158, 1); else if (li == worst) c = tk_hex(0xff4245, 1);
    }
    int k = md_cat(l, 0, sizeof l, "Lap ");
    put_dec(l + k, li + 1);
    ck_fmt_sw(t, ms);
    d_text(d, TK_IT400, 17, MARGIN, bl(TK_IT400, 17, y + 12), l, 0, 0, c, 0);
    float w = tw(TK_IT400, 17, t, 0, TK_TNUM);
    d_text(d, TK_IT400, 17, CONTENT_R - w, bl(TK_IT400, 17, y + 12), t, 0, TK_TNUM, c, 0);
    d_rrect(d, MARGIN, y + 43.5f, CONTENT_W, .5f, 0, W(.12f));
}
static void ck_draw(dc *d, int i, float top)
{
    if (ckl_kind == CKL_CITIES) { if (!i) d_h1(d, "World Clock"); else ck_city_row(d, i - 1, top); }
    else if (ckl_kind == CKL_ALARMS) { if (!i) d_h1(d, "Alarms"); else ck_alarm_row(d, i - 1, top); }
    else if (ckl_kind == CKL_LAPS) ck_lap_row(d, i, top);
}
static uint32_t ck_key(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = 2166136261u;
    h = (h ^ a) * 16777619u; h = (h ^ b) * 16777619u; h = (h ^ c) * 16777619u;
    return h | 1;
}
#define LAP_TOP 520.0f
static void ck_list_build(void)
{
    int kind = !is_app("clock") || CK.edit >= 0 ? CKL_NONE : CK.tab == 0 ? CKL_CITIES : CK.tab == 1 && CK.nal ? CKL_ALARMS :
               CK.tab == 2 && (CLK.sw_run || clk_sw_ms(now_ms) > 0) ? CKL_LAPS : CKL_NONE;
    if (kind == CKL_NONE) { ckl_kind = CKL_NONE; return; }       /* (cm_extras turns LS off: unused this build) */
    if (kind != ckl_kind) { scv_clear(&LS); LS.s = 0; }
    ckl_kind = kind;
    ls_used = 1;
    LS.draw = ck_draw;
    LS.fade_t = 16; LS.fade_b = 16;
    int n = 0;
    float y = 0;
    if (kind == CKL_LAPS) {
        LS.top = LAP_TOP; LS.bot = 718;
        int ds = (int)(clk_sw_ms(now_ms) / 10);
        for (int i = 0; i <= CK.nlap && n < SCV_MAXI; i++, n++) {
            LS.iy[n] = y; LS.ih[n] = 44;
            LS.ik[n] = ck_key(3, (uint32_t)(i ? CK.nlap - i : 1000), i ? (uint32_t)CK.nlap * 7 + (CK.nlap >= 2 ? 1u : 0u) : (uint32_t)ds);
            y += 44;
        }
        ls_nav_build(0);
    } else {
        LS.top = LIST_T0; LS.bot = 718;
        LS.iy[0] = 0; LS.ih[0] = TITLE_H; LS.ik[0] = ck_key((uint32_t)kind, 0x7171u, 0);
        y = TITLE_H; n = 1;
        int cnt = kind == CKL_CITIES ? 5 : CK.nal;
        for (int i = 0; i < cnt; i++, n++) {
            uint32_t look = kind == CKL_CITIES ? (uint32_t)(ST.h * 60 + ST.mi) : (uint32_t)(CK.al[i].h * 1000 + CK.al[i].m * 10 + CK.al[i].on + (i == CK.nal - 1 ? 5 : 0));
            LS.iy[n] = y; LS.ih[n] = 96; LS.ik[n] = ck_key((uint32_t)kind, (uint32_t)i, look);
            y += 96;
        }
        LS.ih[n - 1] += 24;
        ls_nav_build(kind == CKL_CITIES ? "World Clock" : "Alarms");
    }
    scv_layout(&LS, n);
    scv_place(&LS);
    ls_moved();
}

/* the tab bar: World Clock, Alarms, Stopwatch, Timers (glyphs drawn: the pack has none of them) */
static void ck_glyph(dc *d, int k, float cx, float cy, tk_col c)
{
    if (k == 0) {                                          /* a globe */
        d_ring(d, cx, cy, 10, 1.8f, 1, c, 0);
        d_seg(d, cx - 10, cy, cx + 10, cy, 1.5f, c);
        d_seg(d, cx, cy - 10, cx, cy + 10, 1.5f, c);
        d_ring(d, cx, cy, 5, 1.5f, 1, c, 0);
    } else if (k == 1) {                                   /* an alarm clock */
        d_ring(d, cx, cy + 1, 9, 1.8f, 1, c, 0);
        d_seg(d, cx, cy + 1, cx, cy - 4, 1.8f, c); d_seg(d, cx, cy + 1, cx + 4, cy + 3, 1.8f, c);
        d_seg(d, cx - 10, cy - 7, cx - 6, cy - 11, 2, c); d_seg(d, cx + 10, cy - 7, cx + 6, cy - 11, 2, c);
    } else if (k == 2) {                                   /* a stopwatch */
        d_ring(d, cx, cy + 2, 9, 1.8f, 1, c, 0);
        d_seg(d, cx, cy + 2, cx + 4, cy - 2, 1.8f, c);
        d_seg(d, cx - 3, cy - 11, cx + 3, cy - 11, 2, c); d_seg(d, cx, cy - 11, cx, cy - 7, 2, c);
    } else {                                               /* a timer */
        d_ring(d, cx, cy, 10, 1.8f, .8f, c, 1);
        d_seg(d, cx, cy, cx - 5, cy - 5, 1.8f, c);
        d_rrect(d, cx - 1.6f, cy - 1.6f, 3.2f, 3.2f, 1.6f, c);
    }
}
static void ck_tabs(dc *d)
{
    static const char *const TL[4] = { "World Clock", "Alarms", "Stopwatch", "Timers" };
    float tw4 = GROUP_W / 4;
    d_lg_flat(d, GROUP_X, 718, GROUP_W, 62, 31, 1);
    for (int i = 0; i < 4; i++) {
        float bx = GROUP_X + tw4 * i;
        int on = i == CK.tab;
        if (on) d_rrect(d, bx + 4, 722, tw4 - 8, 54, 27, W(.14f));
        tk_col c = on ? tk_hex(CK_ORANGE, 1) : W(.92f);
        ck_glyph(d, i, bx + tw4 / 2, 741, c);
        float w = tw(TK_IT500, 10, TL[i], 0, 0);
        d_text(d, TK_IT500, 10, bx + (tw4 - w) / 2, bl(TK_IT500, 10, 755), TL[i], 0, 0, c, 0);
    }
}
/* a stopwatch / timer button: an 80 pt disc in its tint with a 2 pt gap ring (iOS) */
static void ck_button(dc *d, float cx, float cy, const char *t, uint32_t tint, int dim)
{
    tk_col bg = tint ? tk_hex(tint, dim ? .12f : .26f) : W(dim ? .07f : .16f), fg = tint ? tk_hex(tint, dim ? .45f : 1) : W(dim ? .4f : 1);
    d_ring(d, cx, cy, 39, 1.6f, 1, bg, 0);
    d_rrect(d, cx - 36, cy - 36, 72, 72, 36, bg);
    float w = tw(TK_IT500, 17, t, 0, 0);
    d_text(d, TK_IT500, 17, cx - w / 2, bl(TK_IT500, 17, cy - lh(TK_IT500, 17) / 2), t, 0, 0, fg, 0);
}
#define CKB_Y 452.0f                         /* the buttons' centre */
#define CKB_L 64.0f
#define CKB_R 326.0f
#define WHEEL_Y 268.0f                       /* the wheels' centre (the timer's; the editor's lower) */
static float wheel_cy(void) { return CK.edit >= 0 ? 290 : WHEEL_Y; }
static int wheel_cols(void) { return CK.edit >= 0 ? 2 : 3; }
static float wheel_cx(int c) { return CK.edit >= 0 ? (c ? 228 : 162) : 78 + 117.0f * c; }
static int wheel_mod(int c) { return (CK.edit >= 0 ? c == 0 : c == 0) ? 24 : 60; }
static int ck_wheel_on(void) { return is_app("clock") && (CK.edit >= 0 || (CK.tab == 3 && !CLK.tm_run && !CK.tm_paused)); }
/* the wheels: the values round the selection band, fading and shrinking away from it */
static void ck_wheel_build(void)
{
    relayer(&ckwheel);
    ckwheel.hidden = 1;
    if (!ck_wheel_on()) return;
    float cy = wheel_cy(), top = cy - 2.5f * CK_WP;
    tk_layer *nx = ckwheel.next;
    dc d = mk(&ckwheel, "clock-wheels", 0, top, 390, 5 * CK_WP, 0, 0);
    ckwheel.next = nx;
    if (!d.b->px) return;
    d_rrect(&d, MARGIN - 4, cy - CK_WP / 2, CONTENT_W + 8, CK_WP, 10, W(.12f));
    int nc = wheel_cols();
    for (int c = 0; c < nc; c++) {
        int mod = wheel_mod(c), base = CKWV[c];
        float off = CK.woff[c];
        int sh = (int)tk_floorf(off / CK_WP + .5f);
        float fr = off - sh * CK_WP;
        for (int k = -3; k <= 3; k++) {
            int v = ((base - sh + k) % mod + mod) % mod;
            float y = cy + k * CK_WP + fr, dist = tk_fabsf(y - cy) / (2.5f * CK_WP);
            if (dist >= 1) continue;
            char b[4];
            b[0] = (char)('0' + v / 10); b[1] = (char)('0' + v % 10); b[2] = 0;
            if (CK.edit < 0 && v < 10) { b[0] = (char)('0' + v); b[1] = 0; }
            float fs = 23 - 4 * dist, w = tw(TK_IT400, fs, b, 0, TK_TNUM);
            float x = CK.edit >= 0 ? (c ? 208 : 182 - w) : wheel_cx(c) + 8 - w;
            d_text(&d, TK_IT400, fs, x, bl(TK_IT400, fs, y - lh(TK_IT400, fs) / 2), b, 0, TK_TNUM, W(1 - .7f * dist), 0);
        }
        if (CK.edit < 0) {
            static const char *const U[3] = { "hours", "min", "sec" };
            d_text(&d, TK_IT600, 17, wheel_cx(c) + 16, bl(TK_IT600, 17, cy - lh(TK_IT600, 17) / 2), U[c], 0, 0, INK, 0);
        }
    }
    if (CK.edit >= 0) d_text_box(&d, TK_IT400, 23, 195, bl(TK_IT400, 23, cy - lh(TK_IT400, 23) / 2), 20, 1, ":", 0, 0, INK, 0);
    trim(&ckwheel);
    ckwheel.hidden = 0;
}
static void page_clock(dc *d)
{
    ck_seed();
    if (CK.edit >= 0) {
        /* Add / Edit Alarm (a sheet in iOS; here a page): Cancel, the title, Save; the wheels; Delete */
        d_capsule_btn(d, MARGIN, SAFE_TOP, "Cancel", tk_hex(CK_ORANGE, 1), 0);
        float sw = capsule_w("Save", 1);
        d_capsule_btn(d, CONTENT_R - sw, SAFE_TOP, "Save", tk_hex(CK_ORANGE, 1), 1);
        d_navtitle(d, CK.edit < CK_NAL ? "Edit Alarm" : "Add Alarm");
        d_rrect(d, GROUP_X, 420, GROUP_W, 47, 16, tk_hex(0x151c33, 1));
        d_text(d, TK_IT400, 17, ROW_TX, bl(TK_IT400, 17, 433), "Label", 0, 0, INK, 0);
        d_text_box(d, TK_IT400, 17, CONTENT_R - 16, bl(TK_IT400, 17, 433), 150, 2, "Alarm", 0, 0, W(.55f), 0);
        d_text(d, TK_IT400, 13, ROW_TX, bl(TK_IT400, 13, 476), "It vibrates (no sound yet) and wakes the screen.", 0, 0, W(.5f), 0);
        if (CK.edit < CK_NAL) {
            d_rrect(d, GROUP_X, 520, GROUP_W, 47, 16, tk_hex(0x151c33, 1));
            d_text_box(d, TK_IT400, 17, 195, bl(TK_IT400, 17, 533), CONTENT_W, 1, "Delete Alarm", 0, 0, tk_hex(0xff4245, 1), 0);
        }
        return;
    }
    if (CK.tab == 1) {
        d_gcircle(d, GC_DONE);                           /* + : a new alarm */
        float cx = CONTENT_R - BACK_D / 2, cy = NAV_MID;
        d_seg(d, cx - 8, cy, cx + 8, cy, 2.2f, tk_hex(CK_ORANGE, 1));
        d_seg(d, cx, cy - 8, cx, cy + 8, 2.2f, tk_hex(CK_ORANGE, 1));
        if (!CK.nal) { d_h1(d, "Alarms"); d_empty(d, CONTENT_TOP, EMPTY_TAB, "No Alarms", "Tap + to add one. It vibrates and wakes the screen."); }
    } else if (CK.tab == 3 && !CLK.tm_run && !CK.tm_paused) {
        d_rrect(d, GROUP_X, 540, GROUP_W, 52, 16, tk_hex(0x151c33, 1));
        d_text(d, TK_IT400, 17, ROW_TX, bl(TK_IT400, 17, 555), "When Timer Ends", 0, 0, INK, 0);
        d_text_box(d, TK_IT400, 17, CONTENT_R - 16, bl(TK_IT400, 17, 555), 150, 2, "Vibrate", 0, 0, W(.55f), 0);
    }
    ck_tabs(d);
}
/* the big digits (the stopwatch's, the running timer's) */
static void clk_digits(tk_layer *l, float y, float size, const char *txt)
{
    relayer(l);
    tk_layer *nx = l->next;
    dc d = mk(l, "clock-digits", 0, y, 390, size * 1.25f, 0, 0);
    l->next = nx;
    float w = tw(TK_IT400, size, txt, 0, TK_TNUM);
    d_text(&d, TK_IT400, size, (390 - w) / 2, bl(TK_IT400, size, y), txt, 0, TK_TNUM, INK, 0);
    trim(l);
    l->hidden = 0;
}
static void clk_controls(void)
{
    relayer(&apctl);
    tk_layer *nx = apctl.next;
    dc d = mk(&apctl, "clock-buttons", 0, CKB_Y - 44, 390, 88, 0, 0);
    apctl.next = nx;
    if (CK.tab == 2) {
        float sw = clk_sw_ms(now_ms);
        ck_button(&d, CKB_L, CKB_Y, CLK.sw_run || sw <= 0 ? "Lap" : "Reset", 0, !CLK.sw_run && sw <= 0);
        ck_button(&d, CKB_R, CKB_Y, CLK.sw_run ? "Stop" : "Start", CLK.sw_run ? 0xff4245 : 0x30d158, 0);
    } else if (CK.tab == 3) {
        int set = CK.wv[0] || CK.wv[1] || CK.wv[2];
        ck_button(&d, CKB_L, CKB_Y, "Cancel", 0, !CLK.tm_run && !CK.tm_paused);
        if (CLK.tm_run) ck_button(&d, CKB_R, CKB_Y, "Pause", CK_ORANGE, 0);
        else ck_button(&d, CKB_R, CKB_Y, CK.tm_paused ? "Resume" : "Start", 0x30d158, !CK.tm_paused && !set);
    }
    trim(&apctl);
    apctl.hidden = 0;
    CLK.ctl_drawn = CLK.ctl_ver;
}
/* the running timer's ring: what is left of its length, orange */
static void ck_ring(float left_frac)
{
    relayer(&aptm);
    tk_layer *nx = aptm.next;
    dc d = mk(&aptm, "timer-ring", 195 - 140, 270 - 140, 280, 280, 4, 0);
    aptm.next = nx;
    d_ring(&d, 195, 270, 132, 7, 1, W(.12f), 0);
    if (left_frac > 0) d_ring(&d, 195, 270, 132, 7, left_frac, tk_hex(CK_ORANGE, 1), 1);
    trim(&aptm);
    aptm.hidden = 0;
}
static float ck_tm_left(float t) { return CLK.tm_run ? tk_maxf(0, CLK.tm_end - t) : CK.tm_left; }
/* the digits and the buttons as of t (ms); 1 if anything was redrawn. A timer that ran out vibrates (sh_vibrate). */
static int clk_update(float t)
{
    int any = 0;
    if (CLK.tm_run && t >= CLK.tm_end) {
        CLK.tm_run = 0; CK.tm_paused = 0;
        CLK.ctl_ver++;
        sh_vibrate();                       /* the platform also wakes the screen (P3f) */
        toast("Timer done");
        toast_left = 6000;                  /* an alert stays longer than a toast */
        if (is_app("clock") && CK.tab == 3 && AP.on && !AP.closing) build_app_page(AP.a);
        any = 1;
    }
    if (!is_app("clock") || !AP.on || CK.edit >= 0) return any;
    if (CK.tab == 2) {
        float sw = clk_sw_ms(t);
        int cs = (int)(sw / 10);
        if (cs != CLK.sw_ds || !apsw.bmp.px) {
            CLK.sw_ds = cs;
            char b[24];
            ck_fmt_sw(b, sw);
            clk_digits(&apsw, 196, 84, b);
            if (ckl_kind == CKL_LAPS) ck_list_build();  /* the running lap's row */
            any = 1;
        }
    } else if (CK.tab == 3 && (CLK.tm_run || CK.tm_paused)) {
        float left = ck_tm_left(t);
        int ts = (int)((left + 999) / 1000);
        if (ts != CLK.tm_s || !aptm.bmp.px) {
            CLK.tm_s = ts;
            char b[16];
            fmt_ms(b, ts * 1000.0f, 0);
            clk_digits(&apsw, 236, 64, b);
            ck_ring(CK.tm_total > 0 ? left / CK.tm_total : 0);
            any = 1;
        }
    }
    if (CLK.ctl_drawn != CLK.ctl_ver || !apctl.bmp.px) {
        if (CK.tab >= 2) { clk_controls(); any = 1; }
    }
    if (any) relink();
    return any;
}
static void ck_extras(void)
{
    relayer(&apsw); relayer(&aptm); relayer(&apctl);
    apsw.hidden = aptm.hidden = apctl.hidden = 1;
    CLK.sw_ds = CLK.tm_s = -1;
    clk_update(now_ms);
    ck_wheel_build();
    ck_list_build();
}
static void ck_tm_start(void)
{
    float len = (CK.wv[0] * 3600 + CK.wv[1] * 60 + CK.wv[2]) * 1000.0f;
    if (len <= 0) return;
    CLK.tm_end = now_ms + len; CLK.tm_run = 1; CK.tm_paused = 0; CK.tm_total = len;
    CLK.ctl_ver++;
}
static void clk_tap(float x, float y)
{
    float t = now_ms;
    if (CK.edit >= 0) {
        if (IN_NAV(y) && x < MARGIN + capsule_w("Cancel", 0) + 6) { CK.edit = -1; build_app_page(AP.a); return; }
        if (IN_NAV(y) && x > CONTENT_R - capsule_w("Save", 1) - 6) {
            int i = CK.edit;
            if (i == CK_NAL) { if (CK.nal == CK_NAL) { toast("Up to four alarms"); return; } i = CK.nal++; }
            CK.al[i].h = CK.ev[0]; CK.al[i].m = CK.ev[1]; CK.al[i].on = 1;
            CK.edit = -1;
            ck_alarms_push();
            build_app_page(AP.a);
            return;
        }
        if (CK.edit < CK_NAL && y >= 520 && y < 567 && x >= GROUP_X && x < CONTENT_R) {
            for (int k = CK.edit; k + 1 < CK.nal; k++) CK.al[k] = CK.al[k + 1];
            CK.nal--;
            CK.edit = -1;
            ck_alarms_push();
            build_app_page(AP.a);
        }
        return;
    }
    if (y >= 718 && y <= 780 && x >= GROUP_X && x < GROUP_X + GROUP_W) {
        int k = tk_mini(3, (int)((x - GROUP_X) / (GROUP_W / 4)));
        if (k != CK.tab) { sh_haptic(); CK.tab = k; LS.s = 0; build_app_page(AP.a); }
        return;
    }
    if (CK.tab == 1) {
        if (DONE_HIT(x, y)) {
            if (CK.nal == CK_NAL) { toast("Up to four alarms"); return; }
            CK.edit = CK_NAL; CK.ev[0] = (ST.h + 1) % 24; CK.ev[1] = 0; CK.woff[0] = CK.woff[1] = 0;
            build_app_page(AP.a);
            return;
        }
        int i = scv_hit(&LS, y, 0) - 1;
        if (ckl_kind != CKL_ALARMS || i < 0 || i >= CK.nal) return;
        if (x >= CONTENT_R - 76) {                      /* the toggle */
            sh_haptic();
            CK.al[i].on = !CK.al[i].on;
            ck_alarms_push();
            ck_list_build();
            relink();
        } else {
            CK.edit = i; CK.ev[0] = CK.al[i].h; CK.ev[1] = CK.al[i].m; CK.woff[0] = CK.woff[1] = 0;
            build_app_page(AP.a);
        }
        return;
    }
    float dl = (x - CKB_L) * (x - CKB_L) + (y - CKB_Y) * (y - CKB_Y), dr = (x - CKB_R) * (x - CKB_R) + (y - CKB_Y) * (y - CKB_Y);
    if (CK.tab == 2) {
        if (dr <= 42 * 42) {                            /* Start / Stop */
            sh_haptic();
            if (CLK.sw_run) { CLK.sw_acc += t - CLK.sw_t0; CLK.sw_run = 0; }
            else { CLK.sw_t0 = t; CLK.sw_run = 1; }
            CLK.ctl_ver++;
            int had = ckl_kind == CKL_LAPS;
            clk_update(t);
            if (!had) { ck_list_build(); relink(); }
        } else if (dl <= 42 * 42) {                     /* Lap / Reset */
            float sw = clk_sw_ms(t);
            if (CLK.sw_run) {
                float acc = 0;
                for (int k = 0; k < CK.nlap; k++) acc += CK.lap[k];
                if (CK.nlap < 100) CK.lap[CK.nlap++] = sw - acc;
                CLK.laps = CK.nlap; CLK.lap_at = sw;
            } else if (sw > 0) { CLK.sw_acc = 0; CLK.laps = 0; CK.nlap = 0; CLK.sw_ds = -1; }
            else return;
            sh_haptic();
            CLK.ctl_ver++;
            CLK.sw_ds = -1;
            clk_update(t);
            ck_list_build();
            if (ckl_kind == CKL_NONE && LS.on) { scv_off(&LS); }
            relink();
        }
        return;
    }
    if (CK.tab == 3) {
        if (dl <= 42 * 42 && (CLK.tm_run || CK.tm_paused)) {          /* Cancel */
            sh_haptic();
            CLK.tm_run = 0; CK.tm_paused = 0;
            CLK.ctl_ver++;
            build_app_page(AP.a);
        } else if (dr <= 42 * 42) {
            sh_haptic();
            if (CLK.tm_run) { CK.tm_left = tk_maxf(0, CLK.tm_end - t); CLK.tm_run = 0; CK.tm_paused = 1; CLK.ctl_ver++; clk_update(t); }
            else if (CK.tm_paused) { CLK.tm_end = t + CK.tm_left; CLK.tm_run = 1; CK.tm_paused = 0; CLK.ctl_ver++; clk_update(t); }
            else { ck_tm_start(); if (CLK.tm_run) build_app_page(AP.a); }
        }
    }
}
/* a wheel under the finger (the timer's, the alarm editor's): it follows, then settles on a value with its speed */
static int ck_touch(int ph, float x, float y, float t)
{
    if (ph == 0) {
        if (!ck_wheel_on() || !AP.on || AP.closing || ap_pending) return 0;
        float cy = wheel_cy();
        if (y < cy - 2.5f * CK_WP || y >= cy + 2.5f * CK_WP) return 0;
        int nc = wheel_cols(), c = 0;
        float best = 1e9f;
        for (int k = 0; k < nc; k++) { float dd = tk_fabsf(x - wheel_cx(k)); if (dd < best) { best = dd; c = k; } }
        CK.wcol = c; CK.wdown = 1; CK.wy0 = y; CK.wbase = CK.woff[c]; CK.wvel[c] = 0;
        tk_tracker_reset(&CK.wtr);
        tk_tracker_add(&CK.wtr, y, t);
        return 1;
    }
    int c = CK.wcol;
    tk_tracker_add(&CK.wtr, y, t);
    CK.woff[c] = CK.wbase + (y - CK.wy0);
    if (ph == 2) {
        CK.wdown = 0;
        CK.wvel[c] = tk_tracker_v(&CK.wtr) * 1000;      /* pt / s */
    }
    ck_wheel_build();
    relink();
    return 1;
}
/* the wheels' settling: their speed decays, then they snap to the nearest value (a selection tick each value) */
static int ck_tick(float dt)
{
    int mv = 0;
    for (int c = 0; c < 3; c++) {
        if (CK.wdown && c == CK.wcol) { mv = 1; continue; }
        if (CK.woff[c] == 0 && CK.wvel[c] == 0) continue;
        if (tk_fabsf(CK.wvel[c]) > 40) {
            CK.woff[c] += CK.wvel[c] * dt;
            CK.wvel[c] *= tk_powf(.03f, dt);
        } else {
            CK.wvel[c] = 0;
            float target = tk_floorf(CK.woff[c] / CK_WP + .5f) * CK_WP, k = tk_minf(1, dt * 16);
            CK.woff[c] += (target - CK.woff[c]) * k;
            if (tk_fabsf(target - CK.woff[c]) < .3f) {
                /* settled: the value is the base's minus the rows moved */
                int mod = wheel_mod(c), steps = (int)tk_floorf(target / CK_WP + .5f);
                CKWV[c] = ((CKWV[c] - steps) % mod + mod) % mod;
                CK.woff[c] = 0;
                sh_haptic();
                CLK.ctl_ver++;
                if (is_app("clock") && CK.tab == 3) clk_update(now_ms);
            }
        }
        mv = 1;
    }
    if (mv && ck_wheel_on()) { ck_wheel_build(); relink(); }
    return mv;
}
static int ck_moving(void)
{
    if (CK.wdown) return 1;
    for (int c = 0; c < 3; c++) if (CK.woff[c] != 0 || CK.wvel[c] != 0) return 1;
    return 0;
}

/* ---- the Calculator (iOS 26: delete, AC, %, divide on top; plus-minus, 0, point, = at the bottom; grey functions
 * with white glyphs, dark digits, orange operators; the pending operator lit white; the expression over the
 * result) ------------------------------------------------------------------------------------------------------ */
static const char *const CALK[20] = { "", "AC", "%", "\xc3\xb7", "7", "8", "9", "\xc3\x97", "4", "5", "6", "\xe2\x88\x92",
                                      "1", "2", "3", "+", "\xc2\xb1", "0", ".", "=" };
static tk_layer calop;
static char cal_expr[64];
static void calc_key_rect(int i, float *x, float *y, float *w)
{
    *x = 17 + 92.0f * (i % 4);
    *y = 318 + 92.0f * (i / 4);
    *w = 80;
}
static int calc_is_op(int i) { return i % 4 == 3; }
static void calc_key(dc *d, int i, int lit)
{
    float x, y, w;
    calc_key_rect(i, &x, &y, &w);
    int fn = i < 3 || i == 16, op = calc_is_op(i);
    tk_col bg = lit ? W(1) : op ? tk_hex(0xff9230, 1) : fn ? tk_hex(0x5c5c5f, 1) : tk_hex(0x2a2a2c, 1);
    tk_col fg = lit ? tk_hex(0xff9230, 1) : W(1);
    d_rrect(d, x, y, 80, 80, 40, bg);
    float cx = x + 40, cy = y + 40;
    if (i == 0) {                                     /* delete.left: a pentagon with an x */
        d_seg(d, cx - 13, cy, cx - 6, cy - 8, 2, fg); d_seg(d, cx - 13, cy, cx - 6, cy + 8, 2, fg);
        d_seg(d, cx - 6, cy - 8, cx + 12, cy - 8, 2, fg); d_seg(d, cx - 6, cy + 8, cx + 12, cy + 8, 2, fg);
        d_seg(d, cx + 12, cy - 8, cx + 12, cy + 8, 2, fg);
        d_seg(d, cx - 1, cy - 4, cx + 7, cy + 4, 2, fg); d_seg(d, cx + 7, cy - 4, cx - 1, cy + 4, 2, fg);
        return;
    }
    float fs = op ? 40 : fn ? 30 : 36;
    int f = op ? TK_IT500 : TK_IT400;
    float w0 = tw(f, fs, CALK[i], 0, 0);
    d_text(d, f, fs, cx - w0 / 2, bl(f, fs, cy - lh(f, fs) / 2 - (op ? 2 : 0)), CALK[i], 0, 0, fg, 0);
}
static void page_calc(dc *d)
{
    for (int i = 0; i < 20; i++) calc_key(d, i, 0);
}
static int calc_fmt(char *b, double v)
{
    if (v != v || v > 1e100 || v < -1e100) { tk_memcpy(b, "Error", 6); return 5; }
    int n = 0;
    if (v < 0) { b[n++] = '-'; v = -v; }
    if (v >= 1e9 || (v > 0 && v < 1e-8)) {
        /* iOS's scientific form past nine digits ("9.99999e14"), never a clipped number */
        int e = 0;
        while (v >= 10) { v /= 10; e++; }
        while (v < 1) { v *= 10; e--; }
        long long M = (long long)(v * 100000 + .5);
        if (M >= 1000000) { M /= 10; e++; }
        char t[8];
        for (int i = 5; i >= 0; i--) { t[i] = (char)('0' + M % 10); M /= 10; }
        int last = 5;
        while (last > 0 && t[last] == '0') last--;
        b[n++] = t[0];
        if (last > 0) { b[n++] = '.'; for (int i = 1; i <= last; i++) b[n++] = t[i]; }
        b[n++] = 'e';
        n += put_dec(b + n, e);
        b[n] = 0;
        return n;
    }
    double ip = (double)(long long)v, fp = v - ip;
    long long I = (long long)ip;
    char t[24];
    int k = 0;
    do { t[k++] = (char)('0' + I % 10); I /= 10; } while (I);
    int intd = k;
    while (k) { b[n++] = t[--k]; if (k && k % 3 == 0) b[n++] = ','; }
    int room = 9 - intd;
    if (room > 0) {
        long long f = 1;
        for (int i = 0; i < room; i++) f *= 10;
        long long F = (long long)(fp * (double)f + .5);
        if (F >= f) F = f - 1;
        int m = room;
        while (m > 0 && F % 10 == 0) { F /= 10; m--; }
        if (m > 0) {
            b[n++] = '.';
            char u[16];
            for (int i = m - 1; i >= 0; i--) { u[i] = (char)('0' + F % 10); F /= 10; }
            for (int i = 0; i < m; i++) b[n++] = u[i];
        }
    }
    b[n] = 0;
    return n;
}
static const char *calc_opch(int op) { return op == 1 ? "\xc3\xb7" : op == 2 ? "\xc3\x97" : op == 3 ? "\xe2\x88\x92" : op == 4 ? "+" : ""; }
static void build_calc(void)
{
    relayer(&apcalc);
    char b[32];
    if (CAL.err) tk_memcpy(b, "Error", 6);
    else {
        int n = calc_fmt(b, CAL.cur);
        if (CAL.entering && CAL.has_dot && CAL.frac == 0) { b[n++] = '.'; b[n] = 0; }
        else if (CAL.entering && CAL.has_dot && CAL.frac > 0) {   /* keep the typed trailing zeros */
            int have = 0;
            for (int i = 0; b[i]; i++) if (b[i] == '.') { have = (int)tk_strlen(b + i + 1); break; }
            if (!have) { b[n++] = '.'; }
            for (; have < CAL.frac && n < 30; have++) b[n++] = '0';
            b[n] = 0;
        }
    }
    float size = 86, w = tw(TK_IT400, size, b, 0, TK_TNUM);
    while (w > 352 && size > 36) { size -= 2; w = tw(TK_IT400, size, b, 0, TK_TNUM); }
    tk_layer *nx = apcalc.next;
    dc d = mk(&apcalc, "calc-display", 0, 150, 390, 160, 0, 0);
    apcalc.next = nx;
    if (cal_expr[0]) {
        float ew = tw(TK_IT400, 26, cal_expr, 0, TK_TNUM);
        d_text(&d, TK_IT400, 26, 373 - ew, bl(TK_IT400, 26, 186), cal_expr, 0, TK_TNUM, W(.5f), 0);
    }
    d_text(&d, TK_IT400, size, 373 - w, bl(TK_IT400, size, 304 - size * 1.21f), b, 0, TK_TNUM, INK, 0);
    trim(&apcalc);
    apcalc.hidden = 0;
    /* the pending operator: its key lit (white, the glyph orange) until a digit comes */
    relayer(&calop);
    calop.hidden = 1;
    if (CAL.op && !CAL.entering && !CAL.err) {
        int k = CAL.op == 1 ? 3 : CAL.op == 2 ? 7 : CAL.op == 3 ? 11 : 15;
        float x, y, kw;
        calc_key_rect(k, &x, &y, &kw);
        nx = calop.next;
        dc o = mk(&calop, "calc-op", x, y, 80, 80, 1, 0);
        calop.next = nx;
        calc_key(&o, k, 1);
        calop.hidden = 0;
    }
    relink();
}
static double calc_apply(double a, double b, int op, int *err)
{
    switch (op) {
    case 1: if (b == 0) { *err = 1; return 0; } return a / b;
    case 2: return a * b;
    case 3: return a - b;
    case 4: return a + b;
    default: return b;
    }
}
static void calc_tap(float x, float y)
{
    int k = -1;
    for (int i = 0; i < 20; i++) {
        float kx, ky, kw;
        calc_key_rect(i, &kx, &ky, &kw);
        float dx = x - (kx + 40), dy = y - (ky + 40);
        if (dx * dx + dy * dy <= 44 * 44) { k = i; break; }
    }
    if (k < 0) return;
    sh_haptic();
    const char *c = CALK[k];
    if (k == 1) { CAL.acc = CAL.cur = 0; CAL.op = 0; CAL.entering = CAL.err = CAL.has_dot = CAL.ndig = CAL.frac = 0; cal_expr[0] = 0; }
    else if (CAL.err) return;
    else if (k == 0) {                                   /* delete: the last digit typed */
        if (!CAL.entering) return;
        if (CAL.has_dot && CAL.frac > 0) {
            double f = 1;
            for (int i = 0; i < CAL.frac - 1; i++) f *= 10;
            CAL.cur = (double)(long long)(CAL.cur * f) / f;
            CAL.frac--; CAL.ndig--;
        } else if (CAL.has_dot) CAL.has_dot = 0;
        else { CAL.cur = (double)(long long)(CAL.cur / 10); if (CAL.ndig) CAL.ndig--; }
    }
    else if (k == 16) { CAL.cur = -CAL.cur; }
    else if (k == 2) { CAL.cur /= 100; CAL.entering = 0; }
    else if (c[0] >= '0' && c[0] <= '9' && !c[1]) {
        if (!CAL.entering) { CAL.cur = 0; CAL.entering = 1; CAL.has_dot = CAL.ndig = CAL.frac = 0; if (!CAL.op) cal_expr[0] = 0; }
        if (CAL.ndig >= 9) return;
        int dg = c[0] - '0';
        if (CAL.has_dot) {
            double f = 1;
            for (int i = 0; i <= CAL.frac; i++) f /= 10;
            CAL.cur += (CAL.cur < 0 ? -dg : dg) * f;
            CAL.frac++;
        } else CAL.cur = CAL.cur * 10 + (CAL.cur < 0 ? -dg : dg);
        if (CAL.cur != 0 || CAL.has_dot) CAL.ndig++;
    } else if (k == 18) {
        if (!CAL.entering) { CAL.cur = 0; CAL.entering = 1; CAL.ndig = CAL.frac = 0; }
        CAL.has_dot = 1;
    } else {
        /* an operator or =: the pending operation first (left to right, as iOS's basic calculator chains) */
        char a[24], bb[24];
        int n = 0;
        if (k == 19 && CAL.op) {                          /* the expression stays over the result: "12 × 3 =" */
            calc_fmt(a, CAL.acc); calc_fmt(bb, CAL.cur);
            n = md_cat(cal_expr, 0, sizeof cal_expr, a); n = md_cat(cal_expr, n, sizeof cal_expr, calc_opch(CAL.op));
            md_cat(cal_expr, n, sizeof cal_expr, bb);
        }
        if (CAL.op && (CAL.entering || k == 19)) CAL.cur = calc_apply(CAL.acc, CAL.cur, CAL.op, &CAL.err);
        CAL.acc = CAL.cur;
        CAL.op = k == 19 ? 0 : k == 3 ? 1 : k == 7 ? 2 : k == 11 ? 3 : 4;
        CAL.entering = 0;
        if (CAL.op) { calc_fmt(a, CAL.acc); n = md_cat(cal_expr, 0, sizeof cal_expr, a); md_cat(cal_expr, n, sizeof cal_expr, calc_opch(CAL.op)); }
    }
    build_calc();
}

/* ---- the Camera: no driver yet (Track I has none for the S9's sensors): iOS's screen, honest about it --------- */
static void page_camera(dc *d)
{
    d_rrect(d, 0, 0, 390, H_PT, 0, tk_hex(0x000000, 1));
    /* the top row: flash and Live as glass circles, dimmed */
    for (int k = 0; k < 2; k++) {
        float cx = k ? CONTENT_R - 18 : MARGIN + 18, cy = SAFE_TOP + 22;
        d_rrect(d, cx - 18, cy - 18, 36, 36, 18, W(.12f));
        if (!k) { d_seg(d, cx + 2, cy - 9, cx - 4, cy + 1, 2, W(.45f)); d_seg(d, cx - 4, cy + 1, cx + 4, cy - 1, 2, W(.45f)); d_seg(d, cx + 4, cy - 1, cx - 2, cy + 9, 2, W(.45f)); }
        else { d_ring(d, cx, cy, 8, 1.6f, 1, W(.45f), 0); d_ring(d, cx, cy, 3.5f, 1.6f, 1, W(.45f), 0); }
    }
    /* the viewfinder (4:3) */
    d_rrect(d, 0, 112, 390, 520, 0, tk_hex(0x121417, 1));
    d_text_box(d, TK_IT600, 17, 195, bl(TK_IT600, 17, 344), CONTENT_W, 1, "Camera driver not ready yet", 0, 0, W(.85f), 0);
    char ln[4][96];
    float wm;
    int n = wrapn(ln, 3, TK_IT400, 14, "The Galaxy S9's camera sensors need a Starbite OS driver before photos can be taken.", 300, &wm);
    for (int k = 0; k < n; k++) d_text_box(d, TK_IT400, 14, 195, bl(TK_IT400, 14, 372 + 19 * k), CONTENT_W, 1, ln[k], 0, 0, W(.5f), 0);
    /* the modes: Video and Photo (iOS 26 shows these two), Photo in yellow */
    float wv = tw(TK_IT600, 14, "VIDEO", .6f, 0), wp = tw(TK_IT600, 14, "PHOTO", .6f, 0);
    d_text(d, TK_IT600, 14, 195 - wp / 2 - 26 - wv, bl(TK_IT600, 14, 652), "VIDEO", .6f, 0, W(.55f), 0);
    d_text(d, TK_IT600, 14, 195 - wp / 2, bl(TK_IT600, 14, 652), "PHOTO", .6f, 0, tk_hex(0xffd600, 1), 0);
    /* the shutter (a white ring with its disc, dimmed: it can't take a photo), the thumbnail, the flip circle */
    d_ring(d, 195, 718, 37, 4, 1, W(.45f), 0);
    d_rrect(d, 195 - 31, 718 - 31, 62, 62, 31, W(.35f));
    d_rrect(d, 48 - 22, 718 - 22, 44, 44, 10, W(.1f));
    d_rrect(d, 342 - 22, 718 - 22, 44, 44, 22, W(.12f));
    d_ring(d, 342, 718, 9, 1.8f, .75f, W(.45f), 1);
    d_seg(d, 342 + 9, 718 - 6, 342 + 9, 718 + 1, 1.8f, W(.45f));
}
static void camera_tap(float x, float y)
{
    float dx = x - 195, dy = y - 718;
    if (dx * dx + dy * dy <= 44 * 44) { sh_haptic(); toast("The camera needs its driver first"); }
    else if (y >= 696 && y < 740 && (x < 80 || x > 310)) toast("Nothing to show: no photos can be taken yet");
}

/* ---- the shell's hooks for these apps (app_press, own_extras, cm_layers / cm_touch / cm_tick / cm_closed) ----- */
static int ap_press(float x, float y, tk_rect *r, tk_rect *g, float *rad, tk_col *c)
{
    if (is_app("calc")) {
        for (int i = 0; i < 20; i++) {
            float kx, ky, kw;
            calc_key_rect(i, &kx, &ky, &kw);
            float dx = x - (kx + 40), dy = y - (ky + 40);
            if (dx * dx + dy * dy <= 44 * 44) { *r = *g = (tk_rect){ kx, ky, 80, 80 }; *rad = 40; *c = W(.3f); return 1; }
        }
        return 0;
    }
    if (!is_app("clock")) return 0;
    *c = W(.18f);
    if (CK.edit >= 0) return 0;
    if (CK.tab >= 2) {
        float cx[2] = { CKB_L, CKB_R };
        for (int k = 0; k < 2; k++) {
            float dx = x - cx[k], dy = y - CKB_Y;
            if (dx * dx + dy * dy <= 42 * 42) { *r = *g = (tk_rect){ cx[k] - 40, CKB_Y - 40, 80, 80 }; *rad = 40; return 1; }
        }
    }
    if (CK.tab == 1 && DONE_HIT(x, y)) { *r = *g = (tk_rect){ CONTENT_R - BACK_D, SAFE_TOP, BACK_D, BACK_D }; *rad = BACK_D / 2; *c = W(.16f); return 1; }
    if (ckl_kind == CKL_ALARMS && scv_contains(&LS, x, y) && x < CONTENT_R - 76) {
        float top;
        int i = scv_hit(&LS, y, &top);
        if (i >= 1) { *r = (tk_rect){ 0, top, 390, 96 }; *g = (tk_rect){ 0, LS.top, 390, LS.bot - LS.top }; *rad = 0; *c = W(.08f); return 1; }
    }
    return 0;
}
static void ck_extras_of(const char *k)
{
    if (!tk_strcmp(k, "clock")) ck_extras();
    else if (!tk_strcmp(k, "calc")) build_calc();
    else { relayer(&ckwheel); ckwheel.hidden = 1; relayer(&calop); calop.hidden = 1; }
}
static int ck_layers(tk_layer **o)
{
    if (!ckwheel.name) { tk_layer_init(&ckwheel, "clock-wheels"); ckwheel.hidden = 1; }
    if (!calop.name) { tk_layer_init(&calop, "calc-op"); calop.hidden = 1; }
    o[0] = &ckwheel; o[1] = &calop;
    return 2;
}
static void ck_closed(void)
{
    relayer(&ckwheel); ckwheel.hidden = 1;
    relayer(&calop); calop.hidden = 1;
    CK.edit = -1; CK.wdown = 0;
    for (int c = 0; c < 3; c++) { CK.woff[c] = 0; CK.wvel[c] = 0; }
    ckl_kind = CKL_NONE;
}
/* the host's stills: "cktab" v (0..3), "ckedit" v (an alarm; 4 a new one), "ckalarm" HHMM with v on (one added), "cktimer" v minutes (running) */
static int ck_set(const char *what, const char *key, float v)
{
    if (!tk_strcmp(what, "cktab")) { CK.tab = (int)v; CK.edit = -1; if (AP.on) build_app_page(AP.a); return 1; }
    if (!tk_strcmp(what, "ckedit")) {
        CK.edit = (int)v;
        if (CK.edit < CK.nal) { CK.ev[0] = CK.al[CK.edit].h; CK.ev[1] = CK.al[CK.edit].m; } else { CK.edit = CK_NAL; CK.ev[0] = 8; CK.ev[1] = 30; }
        if (AP.on) build_app_page(AP.a);
        return 1;
    }
    if (!tk_strcmp(what, "ckalarm") && CK.nal < CK_NAL && key[0] && key[1] && key[2] && key[3]) {   /* key HHMM */
        ck_seed();
        CK.al[CK.nal].h = (key[0] - '0') * 10 + key[1] - '0'; CK.al[CK.nal].m = (key[2] - '0') * 10 + key[3] - '0'; CK.al[CK.nal].on = v > 0;
        CK.nal++;
        ck_alarms_push();
        if (AP.on) build_app_page(AP.a);
        return 1;
    }
    if (!tk_strcmp(what, "cktimer")) { CK.wv[0] = 0; CK.wv[1] = (int)v; CK.wv[2] = 0; ck_tm_start(); if (AP.on) build_app_page(AP.a); return 1; }
    return 0;
}
