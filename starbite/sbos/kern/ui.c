/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS UI (docs/STARBITE-OS.md 3): every screen is a list of large elements; the renderer keeps the
 * list that is on the panel, hashes each element and redraws only those whose content or place changed,
 * then presents the dirty rectangles. Screens: the lock screen, home (big tiles), an app's screen (built by
 * the app through the UI calls), the power menu, the kernel's own confirmations, and two full-screen
 * fields: the light and SOS.
 *
 * Touch is primary: tap a button, hold a hold-button to its end (the ring or bar fills). Keys: Volume moves
 * the focus, Bixby selects (a hold element asks for a second Bixby within 3 s), Bixby held is Back, Power
 * short is the screen, Power held is the power menu. */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "kern.h"
#include "kinput.h"
#ifdef SB_HOST
#include <stdio.h>
#include <stdlib.h>
static int getenv_dbg(void) { return getenv("SBOS_DBG") != 0; }
#endif

#define F_CLOCK 30                          /* Inter Tight 300 380 (sbos-assets.py), fallback F_BIG */
#define F_VALUE 31                          /* Inter Tight 600 150 */
#define ICON_APP0 200
#define ICON_BACK 220
#define ICON_POWER 221
#define ICON_MOON 222
#define ICON_RESTART 223
#define ICON_MENU 224
#define ICON_LIGHT_S 226
#define ICON_SOS_S 227

enum { E_TITLE = 1, E_BACK, E_BUTTON, E_BIG, E_TEXT, E_LEVEL, E_TOGGLE, E_HOLD, E_CLOCK, E_TILE, E_ROUND,
       E_FIELD, E_TOAST, E_CTEXT, E_GAUGE, E_INPUT, E_KEYS, E_CHART, E_ICON, E_WIDGET, E_SCROLLBAR };
enum { SCR_LOCK = 1, SCR_HOME, SCR_APP, SCR_POWER, SCR_CONFIRM, SCR_LIGHT, SCR_SOS, SCR_OFF, SCR_CONTROL, SCR_GUEST };
#define GUEST_TILE 0xffffU                      /* 1.2: the home tile "Linux" (not an app index) */
#define GUEST_TILE_TERM 0xfffeU                 /* 1.2-alpha3: the home tile "Terminal" */
int kui_linux_on;                               /* 1.2: Settings > Linux apps (off at every start; asked first) */
static int in_guest, guest_drawn;               /* the Linux view told it is shown / its bars drawn */
static int gk_layer;                            /* 1.2-alpha3: the terminal keyboard's layer (as kb_layer) */
static void guest_view_sync(void);

struct el {
    uint8_t kind, interactive, scrolled;    /* scrolled (0.6.2): an app page's element, drawn below SCROLL_TOP only */
    uint32_t id, flags, color, ms, icon;
    int value, n;
    char a[48], b[128];
    int x, y, w, h;
    uint32_t hash;
};
#define EL_MAX 28
static struct el cur[EL_MAX], shown[EL_MAX];
static int ncur, nshown;
static int screen = SCR_LOCK, shown_screen;
static int focus = -1, pressed = -1;
static uint64_t t_press;
static int press_done;
static int press_y = -1;                    /* where the last press started (a status-bar tap opens the control center) */
static int key_hold_armed = -1;             /* a hold element selected once with Bixby */
static uint64_t key_hold_until;
static int dirty = 1, full = 1;
/* 0.6.2: app pages taller than the panel scroll (a vertical drag; the key focus scrolls to its element). Back stays;
 * the title and the content move under it */
#define SCROLL_TOP 300
static int app_scroll, scroll_max, scroll_drag, scroll_from, focus_follow;
int ui_scroll_max;                          /* the last app page's scroll range (the self test logs it) */
static int light_return = SCR_LOCK;
static uint32_t light_argb;
static uint64_t light_hint_until, light_off_at;
static int light_heat;                      /* 1.2-alpha5: the light is Survival's white screen (a status strip) */
static char toast[64];
static uint64_t toast_until;
static int app_awake;
static int status_forced = 1;

/* the app's elements, built between kui_begin and kui_end */
static struct el app_els[EL_MAX];
static int napp_els, app_building;
static char app_title[48];

/* confirmations */
static char cf_title[48], cf_l[3][128], cf_hold[32];
static void (*cf_done)(int ok);
static int cf_return;

int app_front = -1;
static int from_lock;                       /* the front app was opened from the lock screen (the medical card) */
int ui_overflow;                    /* the last app screen built did not fit (the self test reads it) */

static int sb_strcmp_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a != *b;
}

static int strcmp_k(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return (unsigned char)*a - (unsigned char)*b;
}

static uint32_t fnv(uint32_t h, const void *p, unsigned n)
{
    const uint8_t *b = p;
    for (unsigned i = 0; i < n; ++i)
        h = (h ^ b[i]) * 16777619U;
    return h;
}

static void scopy(char *d, unsigned cap, const char *s)
{
    unsigned i = 0;
    for (; s && s[i] && i + 1 < cap; ++i)
        d[i] = s[i];
    d[i] = 0;
}

void ui_mark_dirty(void)
{
    dirty = 1;
}

void kui_redraw_all(void)
{
    full = dirty = 1;
}

int kui_awake_on(void)
{
    return app_awake || screen == SCR_LIGHT || screen == SCR_SOS || pressed >= 0;
}

/* ---- building lists ---------------------------------------------------------------------------- */

static struct el *add(struct el *list, int *n, int kind)
{
    if (*n >= EL_MAX)
        return 0;
    struct el *e = &list[(*n)++];
    memset(e, 0, sizeof(*e));
    e->kind = (uint8_t)kind;
    e->interactive = kind == E_BACK || kind == E_BUTTON || kind == E_LEVEL || kind == E_TOGGLE ||
                     kind == E_HOLD || kind == E_TILE || kind == E_ROUND || kind == E_INPUT || kind == E_KEYS ||
                     kind == E_ICON;
    return e;
}

void kui_begin(int a)
{
    (void)a;
    napp_els = 0;
    app_building = 1;
    app_title[0] = 0;
}

void kui_title(const char *t)
{
    scopy(app_title, sizeof(app_title), t);
}

void kui_button(uint32_t id, const char *label, const char *sub, uint32_t flags)
{
    struct el *e = add(app_els, &napp_els, E_BUTTON);
    if (!e)
        return;
    e->id = id;
    e->flags = flags;
    scopy(e->a, sizeof(e->a), label);
    scopy(e->b, sizeof(e->b), sub);
    if (flags & UI_DIM)
        e->interactive = 0;
}

void kui_big(const char *cap, const char *val, uint32_t c)
{
    struct el *e = add(app_els, &napp_els, E_BIG);
    if (!e)
        return;
    scopy(e->a, sizeof(e->a), cap);
    scopy(e->b, sizeof(e->b), val);
    e->color = c ? c : C_L1;
}

void kui_text(const char *t, uint32_t c)
{
    struct el *e = add(app_els, &napp_els, E_TEXT);
    if (!e)
        return;
    scopy(e->b, sizeof(e->b), t);
    e->color = c ? c : C_L2;
}

void kui_level(uint32_t id, const char *labels, int cur_v)
{
    struct el *e = add(app_els, &napp_els, E_LEVEL);
    if (!e)
        return;
    e->id = id;
    scopy(e->b, sizeof(e->b), labels);
    e->n = 1;
    for (const char *p = e->b; *p; ++p)
        e->n += *p == '|';
    if (e->n > 5)
        e->n = 5;
    e->value = cur_v;
}

void kui_toggle(uint32_t id, const char *label, int on)
{
    struct el *e = add(app_els, &napp_els, E_TOGGLE);
    if (!e)
        return;
    e->id = id;
    scopy(e->a, sizeof(e->a), label);
    e->value = !!on;
}

void kui_hold(uint32_t id, const char *label, uint32_t ms, uint32_t flags)
{
    struct el *e = add(app_els, &napp_els, E_HOLD);
    if (!e)
        return;
    e->id = id;
    e->flags = flags;
    e->ms = ms < 300 ? 300 : ms > 5000 ? 5000 : ms;
    scopy(e->a, sizeof(e->a), label);
}

/* a line chart: up to 60 int16 values in b[] (120 bytes), the count in n, the range in value (lo) and ms (hi), the
 * left / right labels "left|right" in a */
void kui_chart(const char *caption, const int16_t *v, int n, int lo, int hi, uint32_t c, const char *lr)
{
    struct el *e = add(app_els, &napp_els, E_CHART);
    if (!e)
        return;
    if (n > 60)
        n = 60;
    memcpy(e->b, v, (unsigned)n * 2U);
    e->n = n;
    e->value = lo;
    e->ms = (uint32_t)hi;
    e->color = c ? c : C_GREEN;
    struct ub u;
    ub_init(&u);
    ub_s(&u, caption);
    ub_c(&u, '|');
    ub_s(&u, lr);
    scopy(e->a, sizeof(e->a), u.b);
}

void kui_gauge(const char *caption, const char *value, int permille, uint32_t c)
{
    struct el *e = add(app_els, &napp_els, E_GAUGE);
    if (!e)
        return;
    scopy(e->a, sizeof(e->a), caption);
    scopy(e->b, sizeof(e->b), value);
    e->value = permille;
    e->color = c ? c : C_GREEN;
}

/* ---- text input (0.5): one field per screen and the on-screen keyboard -------------------------------------
 * The text stays in the kernel (kb_text) until the app reads it with SYS_UI_INPUT_GET; it is wiped when the app
 * leaves the front, and never logged. Layers: 0 lower case, 1 upper case (one letter, then back), 2 digits and
 * signs, 3 more signs. Keys: touch only (Volume and Bixby do not type). */
static char kb_text[65];
static int kb_len, kb_layer, kb_secret, kb_show, kb_max, kb_on, kb_caps;
static uint32_t kb_id;

static void scrub_inputs(struct el *l, int n)
{
    for (int i = 0; i < n; ++i)
        if (l[i].kind == E_INPUT)
            memset(l[i].b, 0, sizeof(l[i].b));
}

void kui_input_wipe(void)
{
    memset(kb_text, 0, sizeof(kb_text));
    kb_len = kb_layer = kb_show = kb_on = 0;
    kb_id = 0;
    scrub_inputs(app_els, napp_els);        /* the shown copies of the field too (Show reveals the text) */
    scrub_inputs(cur, ncur);
    scrub_inputs(shown, nshown);
}

void kui_input(uint32_t id, const char *label, uint32_t flags, int max, const char *init)
{
    if (kb_id != id) {                      /* another field: a fresh text (or the app's starting text) */
        kui_input_wipe();
        kb_id = id;
        if (init) {
            int m = max < 1 ? 1 : max > 64 ? 64 : max;
            while (kb_len < m && init[kb_len])
                ++kb_len;
            memcpy(kb_text, init, (unsigned)kb_len);
            kb_text[kb_len] = 0;
        }
    }
    kb_on = 1;
    kb_secret = (flags & 1U) != 0;
    kb_caps = (flags & 4U) != 0;
    if (kb_caps && !kb_len && kb_layer == 0)
        kb_layer = 1;                       /* a name starts with a capital */
    kb_max = max < 1 ? 1 : max > 64 ? 64 : max;
    struct el *e = add(app_els, &napp_els, E_INPUT);
    if (e) {
        e->id = id;
        scopy(e->a, sizeof(e->a), label);
        e->value = kb_len;
        e->flags = (uint32_t)(kb_secret && !kb_show);
        unsigned k = 0;
        for (; k < (unsigned)kb_len && k + 1 < sizeof(e->b); ++k)    /* what is shown: dots while hidden */
            e->b[k] = e->flags ? '*' : kb_text[k];
        e->b[k] = 0;
        e->n = kb_secret;
    }
    struct el *k = add(app_els, &napp_els, E_KEYS);
    if (k) {
        k->id = id;
        k->value = kb_layer;
        k->flags = flags;                   /* 2: the action key says Join (else Done) */
    }
}

int kui_input_get(char *out, unsigned cap)
{
    unsigned n = (unsigned)kb_len < cap - 1 ? (unsigned)kb_len : cap - 1;
    memcpy(out, kb_text, n);
    out[n] = 0;
    return (int)n;
}

/* the keyboard's rows: caps (one byte each; \x01 shift, \x02 delete, \x03 123/abc, \x04 the action key,
 * \x05 space, \x06 more signs) and widths in half keys */
struct krow { const char *cap; const uint8_t *w; };
static const uint8_t W10[10] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 }, W9[9] = { 2, 2, 2, 2, 2, 2, 2, 2, 2 },
                     W9b[9] = { 3, 2, 2, 2, 2, 2, 2, 2, 3 }, W4[4] = { 3, 9, 3, 5 };
static const struct krow kb_rows[4][4] = {
    { { "qwertyuiop", W10 }, { "asdfghjkl", W9 }, { "\x01zxcvbnm\x02", W9b }, { "\x03\x05.\x04", W4 } },
    { { "QWERTYUIOP", W10 }, { "ASDFGHJKL", W9 }, { "\x01ZXCVBNM\x02", W9b }, { "\x03\x05.\x04", W4 } },
    { { "1234567890", W10 }, { "-/:;()$&@", W9 }, { "\x06.,?!'\"_\x02", W9b }, { "\x03\x05#\x04", W4 } },
    { { "[]{}#%^*+=", W10 }, { "_\\|~<>`$@", W9 }, { "\x06.,?!'\"-\x02", W9b }, { "\x03\x05+\x04", W4 } },
};
#define KB_TOP (UI_H - 800)
#define KB_ROW 172
#define KB_GAP 16
#define KB_UNIT 70                          /* a half key, px: 20 half keys = 1400 */

/* the key at x, y (its byte, 0 none) and its box */
static int kb_key(int layer, int x, int y, int *bx, int *by, int *bw)
{
    int r = (y - KB_TOP) / (KB_ROW + KB_GAP);
    if (y < KB_TOP || r < 0 || r > 3)
        return 0;
    const struct krow *row = &kb_rows[layer & 3][r];
    int n = (int)sb_strlen(row->cap), tot = 0;
    for (int i = 0; i < n; ++i)
        tot += row->w[i];
    int kx = 20 + (20 - tot) * KB_UNIT / 2;
    for (int i = 0; i < n; ++i) {
        int w = row->w[i] * KB_UNIT;
        if (x >= kx && x < kx + w) {
            *bx = kx, *by = KB_TOP + r * (KB_ROW + KB_GAP), *bw = w;
            return (uint8_t)row->cap[i];
        }
        kx += w;
    }
    return 0;
}

/* the field and the keyboard on the screen follow kb_* without a round trip to the app */
static void kb_refresh(void)
{
    for (int i = 0; i < napp_els; ++i) {
        struct el *e = &app_els[i];
        if (e->kind == E_INPUT) {
            e->value = kb_len;
            e->flags = (uint32_t)(kb_secret && !kb_show);
            unsigned k = 0;
            for (; k < (unsigned)kb_len && k + 1 < sizeof(e->b); ++k)
                e->b[k] = e->flags ? '*' : kb_text[k];
            e->b[k] = 0;
        } else if (e->kind == E_KEYS) {
            e->value = kb_layer;
        }
    }
    dirty = 1;
}

static void kb_press(int x, int y)
{
    int bx, by, bw, c = kb_key(kb_layer, x, y, &bx, &by, &bw);
    if (!c)
        return;
    if (c == 1)
        kb_layer = kb_layer >= 2 ? (kb_layer == 2 ? 3 : 2) : (kb_layer ? 0 : 1);
    else if (c == 6)
        kb_layer = kb_layer == 3 ? 2 : 3;
    else if (c == 3)
        kb_layer = kb_layer >= 2 ? 0 : 2;
    else if (c == 2) {
        if (kb_len > 0)
            kb_text[--kb_len] = 0;
        if (kb_caps && !kb_len && kb_layer == 0)
            kb_layer = 1;
    } else if (c == 4) {
        if (app_front >= 0)
            app_event_send(app_front, EV_APP_BUTTON, kb_id, kb_len);
    } else if (kb_len < kb_max) {
        kb_text[kb_len++] = c == 5 ? ' ' : (char)c;
        kb_text[kb_len] = 0;
        if (kb_layer == 1)
            kb_layer = 0;                   /* one capital, then lower case again */
    }
    kb_refresh();
}

void kui_end(void)
{
    int keep = 0;                           /* the typed text lives only while its field is on the screen */
    for (int i = 0; i < napp_els; ++i)
        keep |= app_els[i].kind == E_INPUT && app_els[i].id == kb_id;
    if (!keep && kb_id)
        kui_input_wipe();
    app_building = 0;
    {                                       /* 1.2-alpha22: a page with another title (a sub-page) starts at its top */
        static char done_title[sizeof(app_title)];
        static int done_app = -2;
        if (done_app == app_front && sb_strcmp_eq(done_title, app_title))  /* (non-zero: they differ) */
            app_scroll = 0;
        done_app = app_front;
        scopy(done_title, sizeof(done_title), app_title);
    }
    if (screen == SCR_APP)
        dirty = 1;
}

void kui_toast(const char *t, uint32_t ms)
{
    scopy(toast, sizeof(toast), t);
    toast_until = sb_ms() + (ms < 500 ? 500 : ms > 6000 ? 6000 : ms);
    kt_set(KT_UI, 2, sb_ms(), (uint32_t)(toast_until - sb_ms()), 0);
    dirty = 1;
}

static void light_on(uint32_t argb, int ret, uint32_t minutes)
{
    light_argb = argb;
    light_return = ret;
    screen = SCR_LIGHT;
    light_hint_until = sb_ms() + 3000;
    light_off_at = sb_ms() + (uint64_t)minutes * 60000U;
    kt_set(KT_UI, 3, sb_ms(), 3100, 0);
    kt_set(KT_UI, 4, sb_ms(), minutes * 60000U, 0);
    full = dirty = 1;
}

void kui_light(uint32_t argb)
{
    if (!argb) {
        if (screen == SCR_LIGHT) {
            screen = light_return;
            full = dirty = 1;
        }
        if (light_heat)
            kt_cancel(KT_UI, 8);
        light_heat = 0;
        return;
    }
    light_heat = 0;
    light_on(argb, SCR_APP, 10);
}

/* 1.2-alpha5 (the user, 2026-11-09): Survival's screen as heat: full white at the panel's top level (heater.c keeps the
 * boost), a dark strip at the top with the temperatures and the battery's current, refreshed every second; a tap or a
 * key brings back the screen it covered (the Heat page's "White screen" shows it again). Off when Survival ends. */
void kui_heat_light(int on)
{
    if (!on) {
        if (light_heat)
            kui_light(0);
        return;
    }
    if (screen == SCR_LIGHT && light_heat)
        return;
    int ret = screen == SCR_LIGHT ? light_return : screen;
    if (ret == SCR_CONFIRM || ret == SCR_OFF || ret == SCR_GUEST || ret == SCR_SOS)
        ret = SCR_APP;
    light_on(heat_screen_white ? 0xffffffffU : 0xff000000U, ret, 24U * 60U);   /* 1.2-alpha19: dark by default */
    light_heat = 1;
    kt_set(KT_UI, 8, sb_ms(), 1000, 1);
}

void kui_awake(int on)
{
    app_awake = !!on;
}

void kui_open_app(int a)
{
    if (a < 0 || a >= napps)
        return;
    if (app_front >= 0 && app_front != a)
        app_event_send(app_front, EV_APP_HIDE, 0, 0);
    kui_input_wipe();
    from_lock = 0;
    app_front = a;
    app_awake = 0;
    screen = SCR_APP;
    napp_els = 0;
    app_title[0] = 0;
    focus = -1;
    full = dirty = 1;
    app_event_send(a, EV_APP_SHOW, 0, 0);
}

void kui_home(void)
{
    kui_input_wipe();
    from_lock = 0;                       /* typed text never outlives its screen */
    if (app_front >= 0)
        app_event_send(app_front, EV_APP_HIDE, 0, 0);
    app_front = -1;
    app_awake = 0;
    screen = SCR_HOME;
    focus = -1;
    full = dirty = 1;
    guest_view_sync();                      /* 1.2: the Linux view left at once (hidden, still running) */
}

void kui_lock(void)
{
    kui_input_wipe();
    from_lock = 0;
    if (app_front >= 0)
        app_event_send(app_front, EV_APP_HIDE, 0, 0);
    app_front = -1;
    app_awake = 0;
    screen = SCR_LOCK;
    focus = -1;
    full = dirty = 1;
}

void kui_confirm(const char *title, const char *l1, const char *l2, const char *l3, const char *hold,
                 void (*done)(int ok))
{
    scopy(cf_title, sizeof(cf_title), title);
    scopy(cf_l[0], sizeof(cf_l[0]), l1);
    scopy(cf_l[1], sizeof(cf_l[1]), l2);
    scopy(cf_l[2], sizeof(cf_l[2]), l3);
    scopy(cf_hold, sizeof(cf_hold), hold);
    cf_done = done;
    cf_return = screen;
    screen = SCR_CONFIRM;
    focus = -1;
    full = dirty = 1;
}

/* ---- layout -------------------------------------------------------------------------------------- */

#define MX 70
#define CW (UI_W - 2 * MX)
#define HALF_W ((CW - 50) / 2)

/* E_TEXT wraps at spaces: up to 3 lines of F_ROW */
static unsigned wrap_at(const char *s, int w)
{
    unsigned fit = ui_text_fit(F_ROW, s, w);
    if (!s[fit])
        return fit;
    unsigned sp = fit;
    while (sp > 0 && s[sp] != ' ')
        --sp;
    return sp ? sp : fit;
}

static int text_lines(const char *s, int w)
{
    int n = 1;
    while (n < 3) {
        unsigned k = wrap_at(s, w);
        if (!s[k])
            break;
        s += k + (s[k] == ' ');
        ++n;
    }
    return n;
}

static void layout_flow(struct el *l, int n, int y)
{
    for (int i = 0; i < n; ++i) {
        struct el *e = &l[i];
        e->x = MX;
        e->w = CW;
        switch (e->kind) {
        case E_BUTTON:
            e->h = e->b[0] ? 270 : 230;
            if (e->flags & UI_HALF) {
                e->w = HALF_W;
                if (i + 1 < n && l[i + 1].kind == E_BUTTON && (l[i + 1].flags & UI_HALF)) {
                    struct el *f = &l[i + 1];
                    f->x = MX + HALF_W + 50;
                    f->w = HALF_W;
                    f->h = e->h = (e->b[0] || f->b[0]) ? 270 : 230;
                    e->y = f->y = y;
                    y += e->h + 40;
                    ++i;
                    continue;
                }
            }
            break;
        case E_HOLD: e->h = 230; break;
        case E_BIG: e->h = 280; break;
        case E_TEXT:
            e->n = text_lines(e->b, e->w);
            e->h = 76 * e->n + 10;
            break;
        case E_LEVEL: e->h = 300; break;
        case E_TOGGLE: e->h = 210; break;
        case E_GAUGE: e->h = 640; break;
        case E_INPUT: e->h = 250; break;
        case E_CHART: e->h = 420; break;
        case E_KEYS:
            e->x = 0, e->y = KB_TOP - 20, e->w = UI_W, e->h = UI_H - KB_TOP + 20;
            continue;                       /* fixed at the bottom: the flow above it goes on */
        default: e->h = 120; break;
        }
        e->y = y;
        /* text lines sit close together; a control after text gets air (0.4.1) */
        y += e->h + (e->kind == E_TEXT ? (i + 1 < n && l[i + 1].kind != E_TEXT ? 30 : 4) : 40);
    }
}

/* ---- the status bar (0.3): the time on the left; on the right the mobile signal and Wi-Fi (both shown off until
 * their drivers exist: an outline only), the battery % with the estimate and a battery glyph; a small pill in the
 * middle while a service runs (HEATING, SOS, LED). Drawn only when its content changes. */
static char st_sig[96];

static void glyph_signal(int x, int y, int on)
{
    for (int i = 0; i < 4; ++i) {
        int h = 12 + i * 8;
        uint32_t c = on ? C_L1 : C_L3;
        if (on)
            ui_round(x + i * 15, y + 40 - h, 10, h, 3, c);
        else
            ui_round(x + i * 15, y + 40 - h, 10, h, 3, ui_mix(C_L1, C_BG, 60));
    }
}

static void glyph_wifi(int x, int y, int on)
{
    uint32_t c = on ? C_L1 : ui_mix(C_L1, C_BG, 60);
    int cx = x + 24, cy = y + 40;
    ui_disc(cx, cy - 4, 5, c);
    for (int r = 16; r <= 36; r += 10)
        for (int d = -50; d <= 50; d += 4) {
            /* an arc of dots, -50..50 degrees around straight up (integer sine table) */
            static const int16_t q[14] = { 0, 70, 139, 208, 276, 342, 407, 469, 530, 588, 643, 695, 743, 788 };
            int ad = d < 0 ? -d : d, sv = q[ad / 4 < 14 ? ad / 4 : 13] * (d < 0 ? -1 : 1);
            int cv = 1000 - (ad * ad) / 6;
            ui_disc(cx + r * sv / 1000, cy - 4 - r * cv / 1000, 2, c);
        }
}

static void glyph_battery(int x, int y, int pct, int charging)
{
    uint32_t fill = pct < 10 ? C_RED : pct < 20 ? C_ORANGE : charging ? C_GREEN : C_L1;
    ui_round(x, y, 74, 38, 9, ui_mix(C_L1, C_BG, 110));
    ui_round(x + 3, y + 3, 68, 32, 7, C_BG);
    int w = pct < 0 ? 0 : 62 * (pct > 100 ? 100 : pct) / 100;
    if (w > 0)
        ui_round(x + 6, y + 6, w < 6 ? 6 : w, 26, 5, fill);
    ui_round(x + 76, y + 12, 6, 14, 3, ui_mix(C_L1, C_BG, 110));
}

static void build_status(void)
{
    struct sbos_time t;
    struct sbos_battery b;
    struct sbos_estimate est;
    char tm[8] = "--:--", right[32] = "", pill[16] = "";
    if (clock_now(&t)) {
        struct ub u;
        ub_init(&u);
        ub_dec2(&u, (unsigned)t.h);
        ub_c(&u, ':');
        ub_dec2(&u, (unsigned)t.mi);
        scopy(tm, sizeof(tm), u.b);
    }
    battery_get(&b);
    battery_estimate(&est);
    if (b.ok) {
        struct ub u;
        ub_init(&u);
        ub_dec(&u, (uint64_t)(b.pct < 0 ? 0 : b.pct));
        ub_s(&u, " %");
        if (!b.charger && est.now_min > 0) {
            ub_s(&u, "  ");
            if (est.now_min >= 48 * 60) {
                ub_dec(&u, (uint64_t)(est.now_min / (24 * 60)));
                ub_s(&u, " d");
            } else if (est.now_min >= 90) {
                ub_dec(&u, (uint64_t)((est.now_min + 30) / 60));
                ub_s(&u, " h");
            } else {
                ub_dec(&u, (uint64_t)est.now_min);
                ub_s(&u, " min");
            }
        }
        scopy(right, sizeof(right), u.b);
    }
    if (sos_active())
        scopy(pill, sizeof(pill), "SOS");
    else if (heater_running_ui())
        scopy(pill, sizeof(pill), "HEATING");
    else if (torch_on())
        scopy(pill, sizeof(pill), "LED ON");
    struct ub sig;
    ub_init(&sig);
    ub_s(&sig, tm);
    ub_s(&sig, "|");
    ub_s(&sig, right);
    ub_s(&sig, "|");
    ub_s(&sig, pill);
    ub_s(&sig, b.charger ? "|c" : "|-");
    ub_dec(&sig, (uint64_t)wifi_bar());
    ub_dec(&sig, (uint64_t)(b.pct < 0 ? 0 : b.pct));
    ub_s(&sig, notif_unread() ? "|n" : "|-");      /* 1.2-alpha9: the unread notifications' dot */
    if (!status_forced && !strcmp_k(sig.b, st_sig))
        return;
    scopy(st_sig, sizeof(st_sig), sig.b);
    status_forced = 0;
    look_bg(0, 0, UI_W, UI_STATUS_H);
    ui_text(F_STATUS, 64, 76, tm, C_L1);
    if (notif_unread())                     /* 1.2-alpha9: unread notifications (the control center lists them) */
        ui_disc(64 + ui_text_w(F_STATUS, tm) + 26, 62, 10, C_STAR);
    int x = UI_W - 64 - 84;
    if (b.ok)
        glyph_battery(x, 42, b.pct, b.charger != 0);
    if (right[0]) {
        int w = ui_text_w(F_STATUS, right);
        x -= w + 18;
        ui_text(F_STATUS, x, 76, right, C_L1);
    }
    x -= 70;
    glyph_wifi(x, 36, wifi_bar() == 2);
    x -= 78;
    glyph_signal(x, 36, 0);
    if (pill[0]) {
        int w = ui_text_w(F_HEAD, pill) + 48;
        ui_round((UI_W - w) / 2, 30, w, 56, 28, ui_mix(C_ORANGE, C_BG, 70));
        ui_text_c(F_HEAD, UI_W / 2, 70, pill, C_ORANGE);
    }
    ui_dirty(0, 0, UI_W, UI_STATUS_H);
}

static void fmt_clock(char *o, unsigned cap, int sec)
{
    struct sbos_time t;
    struct ub u;
    ub_init(&u);
    if (clock_now(&t)) {
        ub_dec2(&u, (unsigned)t.h);
        ub_c(&u, ':');
        ub_dec2(&u, (unsigned)t.mi);
        if (sec) {
            ub_c(&u, ':');
            ub_dec2(&u, (unsigned)t.s);
        }
    } else {
        ub_s(&u, "--:--");
    }
    scopy(o, cap, u.b);
}

static const char *const wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mn[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December" };

static void build_lock(void)
{
    struct el *e = add(cur, &ncur, E_CLOCK);
    fmt_clock(e->b, sizeof(e->b), 0);
    e->x = 0, e->y = 560, e->w = UI_W, e->h = 460;
    struct sbos_time t;
    e = add(cur, &ncur, E_CTEXT);
    if (clock_now(&t) && t.mo >= 1 && t.mo <= 12) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, wd[t.wday % 7]);
        ub_c(&u, ' ');
        ub_dec(&u, (uint64_t)t.d);
        ub_c(&u, ' ');
        ub_s(&u, mn[t.mo - 1]);
        scopy(e->b, sizeof(e->b), u.b);
    }
    e->value = F_TITLE_S;
    e->color = C_L1;
    e->x = 0, e->y = 1040, e->w = UI_W, e->h = 110;
    struct sbos_battery b;
    struct sbos_estimate est;
    battery_get(&b);
    battery_estimate(&est);
    e = add(cur, &ncur, E_CTEXT);
    {
        struct ub u;
        ub_init(&u);
        if (b.ok) {
            ub_dec(&u, (uint64_t)(b.pct < 0 ? 0 : b.pct));
            ub_s(&u, " %");
            if (b.charger)
                ub_s(&u, "  " U_DOT "  charging");
            else if (est.now_min > 0) {
                ub_s(&u, "  " U_DOT "  ");
                if (est.now_min >= 48 * 60) {
                    ub_s(&u, "about ");
                    ub_dec(&u, (uint64_t)(est.now_min / (24 * 60)));
                    ub_s(&u, " days");
                } else {
                    ub_s(&u, "about ");
                    ub_dec(&u, (uint64_t)(est.now_min / 60));
                    ub_s(&u, " h");
                }
            }
        }
        scopy(e->b, sizeof(e->b), u.b);
    }
    e->value = F_ROW_M;
    e->color = C_L2;
    e->x = 0, e->y = 1170, e->w = UI_W, e->h = 100;
    static const char *const nm[3] = { "Light", "SOS", "Open" };
    static const uint32_t ic[3] = { ICON_LIGHT_S, ICON_SOS_S, ICON_MENU };
    static const uint32_t col[3] = { 0xffffd60aU, C_RED, C_STAR };
    static const uint32_t ms[3] = { 500, 1000, 400 };
    for (int i = 0; i < 3; ++i) {
        e = add(cur, &ncur, E_ROUND);
        e->id = (uint32_t)i;
        scopy(e->a, sizeof(e->a), nm[i]);
        e->icon = ic[i];
        e->color = col[i];
        e->ms = ms[i];
        e->x = 110 + i * 430, e->y = 2130, e->w = 360, e->h = 450;   /* the box holds the 169 px ring */
    }
    e = add(cur, &ncur, E_CTEXT);
    scopy(e->b, sizeof(e->b), pocket_lock ? "Heating: touch is locked. Press Power" : "Hold a button");
    e->value = F_SMALL;
    e->color = pocket_lock ? C_L1 : C_L3;
    e->x = 0, e->y = 2660, e->w = UI_W, e->h = 80;
    /* 0.5.4: the medical card for whoever finds the phone (as iPhone's Medical ID): shown only when Info has a
     * card; a tap opens it (read only there: editing is a page further) */
    int info = app_by_id("info");
    if (info >= 0 && !sb_strcmp_eq(app_tile(info), "Emergency card")) {
        e = add(cur, &ncur, E_HOLD);       /* a hold, as the other lock-screen buttons (pockets) */
        e->id = 3;
        scopy(e->a, sizeof(e->a), "Hold for the medical card");
        e->flags = UI_RED;
        e->ms = 600;
        e->x = 170, e->y = 1600, e->w = UI_W - 340, e->h = 230;
    }
}

/* Home (0.3): the Emergency tile across the top (red), then the apps in two columns of large tiles, each with its
 * icon, its name and a live line the app sets (SYS_UI_TILE) or the kernel adds (heating, LED, SOS). */
static void build_home(void)
{
    int emerg = app_by_id("emergency");
    int heat = app_by_id("heat"), sos_a = app_by_id("sos"), light = app_by_id("light");
    int y0 = 140;
    if (emerg >= 0) {
        struct el *e = add(cur, &ncur, E_TILE);
        e->id = (uint32_t)emerg;
        scopy(e->a, sizeof(e->a), app_name(emerg));
        scopy(e->b, sizeof(e->b), app_tile(emerg)[0] ? app_tile(emerg) : "Alarm, medical card");
        e->icon = 228;                          /* the handset with a cross (0.3.2) */
        e->color = C_RED;
        e->flags = 1;                           /* the wide emergency tile */
        e->x = MX, e->y = y0, e->w = CW, e->h = 210;
        y0 += 210 + 40;
    }
    int shown_n = 0, linux_tile = kui_linux_on && guest_available();
    for (int i = 0; i < napps; ++i)
        shown_n += !app_hidden(i) && i != emerg;
    int term_tile = kui_linux_on && guest_available_mode(GUEST_TERM);
    shown_n += linux_tile + term_tile;
    int rows = (shown_n + 1) / 2, th = rows > 3 ? (2700 - y0 - (rows - 1) * 40) / rows : 600;
    if (th > 600)
        th = 600;
    int t = 0;
    for (int i = 0; i < napps && t < 14; ++i) {   /* 1.2-alpha21: 12 apps + Linux + Terminal */
        if (app_hidden(i) || i == emerg)
            continue;
        struct el *e = add(cur, &ncur, E_TILE);
        e->id = (uint32_t)i;
        scopy(e->a, sizeof(e->a), app_name(i));
        e->icon = app_icon(i);
        e->color = app_color(i) ? app_color(i) : C_STAR;
        e->w = 625;
        e->h = th;
        e->x = MX + (t % 2) * (625 + 50);
        e->y = y0 + (t / 2) * (th + 40);
        ++t;
        scopy(e->b, sizeof(e->b), app_tile(i));
        if (i == light && torch_on())
            scopy(e->b, sizeof(e->b), "LED on");
        if (i == heat && heater_running_ui())
            scopy(e->b, sizeof(e->b), "Heating");
        if (i == sos_a && sos_active())
            scopy(e->b, sizeof(e->b), "Alarm on");
    }
    if (linux_tile && t < 14) {                  /* 1.2: Linux apps on the big cores (Settings turned it on) */
        struct el *e = add(cur, &ncur, E_TILE);
        e->id = GUEST_TILE;
        scopy(e->a, sizeof(e->a), "Linux");
        scopy(e->b, sizeof(e->b), guest_mode() == GUEST_APP && guest_state() == GUEST_RUNNING ? "Running" : "Test");
        e->icon = 0;
        e->color = 0xff5ac8faU;
        e->w = 625;
        e->h = th;
        e->x = MX + (t % 2) * (625 + 50);
        e->y = y0 + (t / 2) * (th + 40);
        ++t;
    }
    if (term_tile && t < 14) {                  /* 1.2-alpha3: the Linux terminal with Starbite's keyboard */
        struct el *e = add(cur, &ncur, E_TILE);
        e->id = GUEST_TILE_TERM;
        scopy(e->a, sizeof(e->a), "Terminal");
        scopy(e->b, sizeof(e->b), guest_mode() == GUEST_TERM && guest_state() == GUEST_RUNNING ? "Running" : "Linux shell");
        e->icon = 0;
        e->color = 0xff32d74bU;
        e->w = 625;
        e->h = th;
        e->x = MX + (t % 2) * (625 + 50);
        e->y = y0 + (t / 2) * (th + 40);
        ++t;
    }
    struct el *e = add(cur, &ncur, E_CTEXT);
    int info = app_by_id("info");
    if (info >= 0 && !sb_strcmp_eq(app_tile(info), "Not set up")) {
        scopy(e->b, sizeof(e->b), "Tip: write your medical card in Info");   /* until there is one */
        e->value = F_DETAIL;
        e->color = C_STAR;
    } else {
        scopy(e->b, sizeof(e->b), "Bixby: open  " U_DOT "  Power: screen off  " U_DOT "  hold Power: menu");
        e->value = F_SMALL;
        e->color = C_L3;
    }
    e->x = 0, e->y = y0 + rows * (th + 40) + 4, e->w = UI_W, e->h = 80;
}

/* 0.6: home in normal mode: a clock widget, the apps as glass icons in four columns, Emergency as a pill near the
 * bottom (within the thumb's reach), the control center's hint */
static void build_home_normal(void)
{
    int emerg = app_by_id("emergency");
    struct el *e = add(cur, &ncur, E_WIDGET);
    struct sbos_time t;
    char tm[8] = "--:--";
    if (clock_now(&t)) {
        struct ub u;
        ub_init(&u);
        ub_dec2(&u, (unsigned)t.h);
        ub_c(&u, ':');
        ub_dec2(&u, (unsigned)t.mi);
        scopy(tm, sizeof(tm), u.b);
        ub_init(&u);
        if (t.mo >= 1 && t.mo <= 12) {
            ub_s(&u, wd[t.wday % 7]);
            ub_c(&u, ' ');
            ub_dec(&u, (uint64_t)t.d);
            ub_c(&u, ' ');
            ub_s(&u, mn[t.mo - 1]);
        }
        scopy(e->b, sizeof(e->b), u.b);
    }
    scopy(e->a, sizeof(e->a), tm);
    {
        struct sbos_battery b;
        struct sbos_estimate est;
        battery_get(&b);
        battery_estimate(&est);
        if (b.ok) {                         /* the battery at the widget's right: "76 %" and the estimate */
            struct ub u;
            ub_init(&u);
            ub_dec(&u, (uint64_t)(b.pct < 0 ? 0 : b.pct));
            ub_s(&u, " %");
            if (b.charger)
                ub_s(&u, "|charging");
            else if (est.now_min > 0) {
                ub_s(&u, "|about ");
                if (est.now_min >= 48 * 60) {
                    ub_dec(&u, (uint64_t)(est.now_min / (24 * 60)));
                    ub_s(&u, " days");
                } else {
                    ub_dec(&u, (uint64_t)((est.now_min + 30) / 60));
                    ub_s(&u, " h");
                }
            }
            scopy(e->b + 64, sizeof(e->b) - 64, u.b);
        }
    }
    e->x = MX, e->y = 150, e->w = CW, e->h = 360;
    int k = 0;
    for (int i = 0; i < napps && k < 14; ++i) {
        if (app_hidden(i) || i == emerg)
            continue;
        e = add(cur, &ncur, E_ICON);
        if (!e)
            break;
        e->id = (uint32_t)i;
        scopy(e->a, sizeof(e->a), app_name(i));
        e->icon = app_icon(i);
        e->color = app_color(i) ? app_color(i) : C_STAR;
        e->x = MX + (k % 4) * 325, e->y = 600 + (k / 4) * 360, e->w = 325, e->h = 340;
        ++k;
    }
    if (kui_linux_on && guest_available() && k < 14) {     /* 1.2: Linux apps (Settings turned it on) */
        e = add(cur, &ncur, E_ICON);
        if (e) {
            e->id = GUEST_TILE;
            scopy(e->a, sizeof(e->a), "Linux");
            e->icon = 0;
            e->color = 0xff5ac8faU;
            e->x = MX + (k % 4) * 325, e->y = 600 + (k / 4) * 360, e->w = 325, e->h = 340;
            ++k;
        }
    }
    if (kui_linux_on && guest_available_mode(GUEST_TERM) && k < 14) {     /* 1.2-alpha3: the terminal */
        e = add(cur, &ncur, E_ICON);
        if (e) {
            e->id = GUEST_TILE_TERM;
            scopy(e->a, sizeof(e->a), "Terminal");
            e->icon = 0;
            e->color = 0xff32d74bU;
            e->x = MX + (k % 4) * 325, e->y = 600 + (k / 4) * 360, e->w = 325, e->h = 340;
            ++k;
        }
    }
    if (emerg >= 0) {
        e = add(cur, &ncur, E_TILE);
        e->id = (uint32_t)emerg;
        scopy(e->a, sizeof(e->a), app_name(emerg));
        scopy(e->b, sizeof(e->b), app_tile(emerg)[0] ? app_tile(emerg) : "Alarm, medical card");
        e->icon = 228;
        e->color = C_RED;
        e->flags = 1;
        e->x = MX, e->y = 2420, e->w = CW, e->h = 210;
    }
    e = add(cur, &ncur, E_CTEXT);
    scopy(e->b, sizeof(e->b), "Tap the top bar: control center");
    e->value = F_SMALL;
    e->color = C_L2;
    e->x = 0, e->y = 2700, e->w = UI_W, e->h = 80;
}

static void build_app(void)
{
    struct el *e = add(cur, &ncur, E_BACK);
    scopy(e->a, sizeof(e->a), "Back");
    e->x = 50, e->y = 140, e->w = 340, e->h = 150;
    e = add(cur, &ncur, E_TITLE);
    scopy(e->a, sizeof(e->a), app_title[0] ? app_title : app_name(app_front));
    e->x = MX, e->y = 330, e->w = CW, e->h = 140;
    int base = ncur;
    for (int i = 0; i < napp_els && ncur < EL_MAX - 1; ++i)
        cur[ncur++] = app_els[i];
    layout_flow(cur + base, ncur - base, 520);
    /* an app screen taller than the panel: logged once per app (the self test counts it) */
    static uint32_t over_logged;
    int bottom = 0, keys = 0;
    for (int i = base; i < ncur; ++i) {
        if (cur[i].kind == E_KEYS) {
            keys = 1;
            continue;
        }
        if (cur[i].y + cur[i].h > bottom)
            bottom = cur[i].y + cur[i].h;
    }
    /* 0.6.2: taller than the panel scrolls; what still cannot be shown is elements beyond EL_MAX (cut off) */
    if (napp_els > EL_MAX - 1 - base) {
        ui_overflow = 1;
        if (app_front >= 0 && app_front < 32 && !(over_logged & (1U << app_front))) {
            over_logged |= 1U << app_front;
            sb_logf("ui: app page has more elements than fit (some cut off): ", app_name(app_front));
        }
    }
    static int last_app = -2;
    if (app_front != last_app) {
        last_app = app_front;
        app_scroll = 0;
    }
    int vb = keys ? KB_TOP - 20 : UI_H - 40;
    scroll_max = bottom > vb ? bottom - vb : 0;
    ui_scroll_max = scroll_max;
    if (focus_follow) {
        focus_follow = 0;
        int k = 0;
        for (int i = 0; i < ncur; ++i)
            if (cur[i].interactive) {
                if (k == focus && i >= base) {
                    if (cur[i].y - app_scroll < SCROLL_TOP + 20)
                        app_scroll = cur[i].y - SCROLL_TOP - 20;
                    else if (cur[i].y + cur[i].h - app_scroll > vb)
                        app_scroll = cur[i].y + cur[i].h - vb;
                }
                ++k;
            }
    }
    if (app_scroll > scroll_max)
        app_scroll = scroll_max;
    if (app_scroll < 0)
        app_scroll = 0;
    if (scroll_max > 0) {
        for (int i = base - 1; i < ncur; ++i)
            if (cur[i].kind != E_KEYS) {
                cur[i].y -= app_scroll;
                cur[i].scrolled = 1;
            }
        struct el *e = add(cur, &ncur, E_SCROLLBAR);
        if (e) {
            int view = vb - SCROLL_TOP, total = scroll_max + view;
            e->interactive = 0;
            e->x = UI_W - 22, e->w = 10;
            e->h = view * view / total;
            e->y = SCROLL_TOP + (view - e->h) * app_scroll / (scroll_max ? scroll_max : 1);
        }
    }
}

/* ---- the control center (0.6): a tap on the status bar; Emergency Mode, the LED, Wi-Fi, brightness, the wallpaper ---- */
static int cc_return = SCR_HOME;
int em_set_auto;                            /* Emergency Mode set the 5-minute switch-off (and takes it back; saved) */
static const char *const wp_names = "Aurora|Dusk|Ocean|Graphite";

void kui_control_open(void)
{
    if (screen == SCR_CONTROL || screen == SCR_SOS || screen == SCR_LIGHT || screen == SCR_CONFIRM || screen == SCR_OFF ||
        screen == SCR_POWER)
        return;
    cc_return = screen;
    screen = SCR_CONTROL;
    notif_seen();                           /* 1.2-alpha9: the list is shown now */
    focus = -1;
    full = dirty = 1;
}

static void control_close(void)
{
    screen = cc_return == SCR_CONTROL ? SCR_HOME : cc_return;
    full = dirty = 1;
}

void kui_set_emergency(int on)
{
    on = !!on;
    if (on == look_emergency)
        return;
    look_emergency = on;
    if (on && !auto_off_min) {
        auto_off_min = 5;                   /* Emergency Mode: save and switch off 5 minutes after the screen goes off */
        em_set_auto = 1;
    } else if (!on && em_set_auto) {
        auto_off_min = 0;
        em_set_auto = 0;
    }
    sb_log(on ? "ui: Emergency Mode on (the emergency screens; save and switch off 5 min after the screen goes off)"
              : "ui: Emergency Mode off (normal mode)");
    panel_emergency(on);
    settings_changed();
    full = dirty = 1;
}

static void build_control(void)
{
    struct el *e = add(cur, &ncur, E_BACK);
    scopy(e->a, sizeof(e->a), "Close");
    e->x = 50, e->y = 140, e->w = 340, e->h = 150;
    e = add(cur, &ncur, E_TITLE);
    scopy(e->a, sizeof(e->a), "Control center");
    e->x = MX, e->y = 330, e->w = CW, e->h = 140;
    int base = ncur;
    /* 1.2-alpha9: the notifications, newest first (opening this marks them read) */
    for (int i = 0; i < notif_count() && i < 4; ++i) {
        const struct notif *n = notif_get(i);
        struct ub u;
        ub_init(&u);
        if (n->hm >= 0) {
            ub_dec2(&u, (unsigned)(n->hm / 100));
            ub_c(&u, ':');
            ub_dec2(&u, (unsigned)(n->hm % 100));
            ub_s(&u, "  ");
        }
        ub_s(&u, n->title);
        e = add(cur, &ncur, E_BUTTON);     /* 1.2-alpha22: a tap opens the app that posted it */
        if (!e)
            break;
        e->id = 100U + (uint32_t)i;
        scopy(e->a, sizeof(e->a), u.b);
        scopy(e->b, sizeof(e->b), n->text);
    }
    if (notif_count()) {
        e = add(cur, &ncur, E_BUTTON);
        e->id = 7;
        scopy(e->a, sizeof(e->a), "Clear");
        e->flags = UI_HALF;
    }
    e = add(cur, &ncur, E_TOGGLE);
    e->id = 1;
    scopy(e->a, sizeof(e->a), "Emergency Mode");
    e->value = look_emergency;
    e = add(cur, &ncur, E_TEXT);
    scopy(e->b, sizeof(e->b), look_emergency ? "On: the big emergency screens; the phone saves and switches off 5 minutes after the screen goes off."
                                             : "The big emergency screens, and the phone saves and switches off 5 minutes after the screen goes off.");
    e->color = look_emergency ? C_RED : C_L2;
    if (look_emergency) {
        e = add(cur, &ncur, E_TOGGLE);
        e->id = 6;
        scopy(e->a, sizeof(e->a), "Dim clock when off (new)");
        e->value = em_aod;
    }
    e = add(cur, &ncur, E_TOGGLE);
    e->id = 2;
    scopy(e->a, sizeof(e->a), "Flashlight");
    e->value = torch_on();
    e = add(cur, &ncur, E_TOGGLE);
    e->id = 3;
    scopy(e->a, sizeof(e->a), "Wi-Fi");
    e->value = wifi_on();
    e = add(cur, &ncur, E_TEXT);
    scopy(e->b, sizeof(e->b), "Brightness");
    e->color = C_L2;
    e = add(cur, &ncur, E_LEVEL);
    e->id = 4;
    scopy(e->b, sizeof(e->b), "Boot|Low|Mid|Normal|High");
    e->n = 5;
    e->value = panel_bright_level;
    if (!look_emergency) {
        e = add(cur, &ncur, E_TEXT);
        scopy(e->b, sizeof(e->b), "Wallpaper");
        e->color = C_L2;
        e = add(cur, &ncur, E_LEVEL);
        e->id = 5;
        scopy(e->b, sizeof(e->b), wp_names);
        e->n = 4;
        e->value = look_style();
    }
    layout_flow(cur + base, ncur - base, 520);
}

static void control_pick(const struct el *e, int x)
{
    if (e->kind == E_BACK) {
        control_close();
        return;
    }
    if (e->kind == E_BUTTON && e->id == 7) {
        notif_clear();                      /* 1.2-alpha9 */
        full = dirty = 1;
        return;
    }
    if (e->kind == E_BUTTON && e->id >= 100 && e->id < 104) {   /* 1.2-alpha22: a notification: open its app */
        const struct notif *n = notif_get((int)e->id - 100);
        int a = n ? app_by_id(n->who) : -1;
        for (int k = 0; n && a < 0 && k < napps; ++k)   /* an app's own post carries its name */
            if (!sb_strcmp_eq(app_name(k), n->who))
                a = k;
        if (a >= 0 && !app_hidden(a)) {
            sb_log("ui: a notification opened its app");
            control_close();
            kui_open_app(a);
        }
        return;
    }
    if (e->kind == E_TOGGLE) {
        if (e->id == 1)
            kui_set_emergency(!e->value);
        else if (e->id == 6) {
            em_aod = !e->value;             /* 0.6.5: until a restart (a first run with the user) */
            sb_log(em_aod ? "ui: Emergency Mode's dim clock when the screen is off: on (the panel's low-power mode)"
                          : "ui: Emergency Mode's dim clock: off (black)");
        }
        else if (e->id == 2) {
            if (torch_set(!e->value) != 0 && !e->value)
                kui_toast("The LED is not available", 2000);
        } else if (e->id == 3) {
            if (wifi_power(!e->value) == SBOS_EAGAIN)
                kui_toast("Not now (heating?)", 2000);
        }
    } else if (e->kind == E_LEVEL) {
        int n = e->n < 1 ? 1 : e->n, v = x < 0 ? (e->value + 1) % n : (x - e->x) * n / (e->w > 0 ? e->w : 1);
        v = v < 0 ? 0 : v >= n ? n - 1 : v;
        if (e->id == 4) {
            if (!panel_brightness(v))
                kui_toast("The panel did not take it", 2000);
            settings_changed();
        } else if (e->id == 5) {
            if (heater_running_ui()) {
                kui_toast("Not while heating", 2000);   /* making one keeps the loop busy for a moment */
            } else {
                look_init(v);
                settings_changed();
            }
        }
    }
    full = dirty = 1;
}

static void build_power(void)
{
    struct el *e = add(cur, &ncur, E_TITLE);
    scopy(e->a, sizeof(e->a), "Power");
    e->x = MX, e->y = 250, e->w = CW, e->h = 140;
    static const char *const l[5] = { "Screen off", "Restart Starbite OS", "Boot menu", "Power off", "Cancel" };
    static const char *const s[5] = { "Power key wakes it", "", "The Starbite menu", "", "" };
    int base = ncur;
    for (int i = 0; i < 5; ++i) {
        e = add(cur, &ncur, E_BUTTON);
        e->id = (uint32_t)i;
        scopy(e->a, sizeof(e->a), l[i]);
        scopy(e->b, sizeof(e->b), s[i]);
        e->flags = i == 3 ? UI_RED : 0;
    }
    layout_flow(cur + base, ncur - base, 450);
}

static void build_confirm(void)
{
    struct el *e = add(cur, &ncur, E_TITLE);
    scopy(e->a, sizeof(e->a), cf_title);
    e->x = MX, e->y = 250, e->w = CW, e->h = 140;
    int base = ncur;
    for (int i = 0; i < 3; ++i)
        if (cf_l[i][0]) {
            e = add(cur, &ncur, E_TEXT);
            scopy(e->b, sizeof(e->b), cf_l[i]);
            e->color = C_L2;
        }
    e = add(cur, &ncur, E_HOLD);
    e->id = 1;
    e->ms = 1500;
    e->flags = UI_ACCENT;
    scopy(e->a, sizeof(e->a), cf_hold);
    e = add(cur, &ncur, E_BUTTON);
    e->id = 0;
    scopy(e->a, sizeof(e->a), "Cancel");
    layout_flow(cur + base, ncur - base, 470);
    /* a gap above the hold */
    for (int i = base; i < ncur; ++i)
        if (cur[i].kind != E_TEXT)
            cur[i].y += 60;
}

#define HEAT_STRIP_H 180
#define HEAT_STRIP_C 0xff1c1c1eU

static void ub_c10s(struct ub *u, int c10)
{
    if (c10 <= -1000) {
        ub_s(u, "--");
        return;
    }
    if (c10 < 0) {
        ub_c(u, '-');
        c10 = -c10;
    }
    ub_dec(u, (uint64_t)(c10 / 10));
    ub_c(u, '.');
    ub_dec(u, (uint64_t)(c10 % 10));
}

static void build_light(void)
{
    struct el *e = add(cur, &ncur, E_FIELD);
    e->x = 0, e->y = 0, e->w = UI_W, e->h = UI_H;
    e->color = light_argb;
    if (sb_ms() < light_hint_until)
        scopy(e->b, sizeof(e->b), light_heat ? (heat_screen_white ? "Survival: the screen as heat. Tap for the normal screen" :
                                                "Survival: the readings. Tap for the normal screen") :
                                               "Tap or press a key to turn it off");
    if (!light_heat)
        return;
    struct sbos_heat h;
    heater_status(&h);
    struct ub u;
    ub_init(&u);
    if (h.all_out && h.level >= HEAT_SURVIVAL)
        ub_s(&u, "ALL OUT  ");                  /* 1.2-alpha23: no hold, no throttle */
    ub_s(&u, "SoC ");
    ub_c10s(&u, h.soc_c10);
    ub_s(&u, " \xc2\xb0" "C  battery ");
    ub_c10s(&u, h.bat_c10);
    ub_s(&u, " \xc2\xb0" "C  ");
    ub_dec(&u, (uint64_t)(h.ma < 0 ? -h.ma : h.ma));
    ub_s(&u, " mA  ");
    ub_dec(&u, (uint64_t)(h.pct < 0 ? 0 : h.pct));
    ub_c(&u, '%');
    struct el *t = add(cur, &ncur, E_FIELD);
    t->x = 0, t->y = 0, t->w = UI_W, t->h = HEAT_STRIP_H;
    t->color = HEAT_STRIP_C;
    t->value = 1;                           /* the strip: white text at its own place */
    scopy(t->b, sizeof(t->b), u.b);
    scopy(t->a, sizeof(t->a), h.why);
}

static void build_sos(void)
{
    uint32_t c = 0;
    (void)sos_frame(&c);
    struct el *e = add(cur, &ncur, E_FIELD);
    e->x = 0, e->y = 0, e->w = UI_W, e->h = UI_H;
    e->color = c;
    struct sbos_sos s;
    sos_status(&s);
    e->value = s.state;
    {
        struct ub u;
        ub_init(&u);
        if (s.state == 2) {
            ub_s(&u, "SOS in ");
            ub_dec(&u, (uint64_t)(s.secs < 0 ? -s.secs : 0));
        } else {
            ub_dec(&u, (uint64_t)(s.secs / 60));
            ub_c(&u, ':');
            ub_dec2(&u, (unsigned)(s.secs % 60));
        }
        scopy(e->b, sizeof(e->b), u.b);
    }
    e = add(cur, &ncur, E_HOLD);
    e->id = 1;
    e->ms = 1500;
    e->flags = UI_RED;
    scopy(e->a, sizeof(e->a), s.state == 2 ? "Hold to cancel" : "Hold to stop SOS");
    e->x = MX, e->y = 2540, e->w = CW, e->h = 230;
}

/* 1.2: the Linux view while it is not showing the guest's frames: starting, failed or stopping */
static void build_guest(void)
{
    if (guest_shown())
        return;                                 /* the frames are drawn straight (ui_kdraw) */
    struct el *e = add(cur, &ncur, E_TITLE);
    scopy(e->a, sizeof(e->a), "Linux");
    e->x = MX, e->y = 250, e->w = CW, e->h = 140;
    int base = ncur, gs = guest_state();
    e = add(cur, &ncur, E_TEXT);
    scopy(e->b, sizeof(e->b), gs == GUEST_STARTING ? "Starting Linux on the big cores\xe2\x80\xa6" :
                              gs == GUEST_STOPPING ? "Linux is stopping\xe2\x80\xa6" :
                              gs == GUEST_FAILED ? "Linux did not start." : "Linux is off.");
    e->color = C_L1;
    if (gs == GUEST_FAILED && guest_why()[0]) {
        e = add(cur, &ncur, E_TEXT);
        scopy(e->b, sizeof(e->b), guest_why());
        e->color = C_L2;
    }
    e = add(cur, &ncur, E_TEXT);
    scopy(e->b, sizeof(e->b), "A test on the four big cores; no heat meanwhile. Hold Bixby: home.");
    e->color = C_L3;
    if (gs == GUEST_STARTING) {
        e = add(cur, &ncur, E_BUTTON);
        e->id = 2;
        scopy(e->a, sizeof(e->a), "Stop Linux");
    }
    e = add(cur, &ncur, E_BUTTON);
    e->id = 1;
    scopy(e->a, sizeof(e->a), "Home");
    layout_flow(cur + base, ncur - base, 450);
}

static void build(void)
{
    ncur = 0;
    switch (screen) {
    case SCR_LOCK: build_lock(); break;
    case SCR_HOME:
        if (look_glass_on())
            build_home_normal();            /* 0.6: normal mode */
        else
            build_home();                   /* Emergency Mode (and before the wallpaper exists) */
        break;
    case SCR_APP: build_app(); break;
    case SCR_POWER: build_power(); break;
    case SCR_CONTROL: build_control(); break;
    case SCR_CONFIRM: build_confirm(); break;
    case SCR_LIGHT: build_light(); break;
    case SCR_SOS: build_sos(); break;
    case SCR_GUEST: build_guest(); break;
    default: break;
    }
    if (toast[0] && sb_ms() < toast_until && screen != SCR_LIGHT && screen != SCR_SOS && screen != SCR_OFF &&
        !(screen == SCR_GUEST && guest_shown())) {
        struct el *e = add(cur, &ncur, E_TOAST);
        if (e) {
            scopy(e->b, sizeof(e->b), toast);
            int w = ui_text_w(F_ROW_M, toast) + 120;
            if (w > UI_W - 100)
                w = UI_W - 100;
            e->w = w;
            e->h = 150;
            e->x = (UI_W - w) / 2;
            e->y = 2760 - 150;
            e->interactive = 0;
        }
    } else {
        toast[0] = 0;
    }
    /* focus and press state into the elements (they change the hash) */
    int k = 0;
    for (int i = 0; i < ncur; ++i) {
        struct el *e = &cur[i];
        uint32_t h = fnv(2166136261U, &e->kind, 1);
        h = fnv(h, &e->id, sizeof(e->id));
        h = fnv(h, &e->flags, sizeof(e->flags));
        h = fnv(h, &e->color, sizeof(e->color));
        h = fnv(h, &e->value, sizeof(e->value));
        h = fnv(h, &e->icon, sizeof(e->icon));
        h = fnv(h, e->a, sizeof(e->a));
        h = fnv(h, e->b, sizeof(e->b));
        h = fnv(h, &e->n, sizeof(e->n));
        h = fnv(h, &e->ms, sizeof(e->ms));
        int st[4] = { e->x, e->y, e->w, e->h };
        h = fnv(h, st, sizeof(st));
        if (e->interactive) {
            int f = k == focus, p = i == pressed;
            unsigned prog = 0;
            if (p && (e->kind == E_HOLD || e->kind == E_ROUND) && e->ms)
                prog = (unsigned)((sb_ms() - t_press) * 24U / e->ms);
            int ka = key_hold_armed == i;
            h = fnv(h, &f, sizeof(f));
            h = fnv(h, &p, sizeof(p));
            h = fnv(h, &prog, sizeof(prog));
            h = fnv(h, &ka, sizeof(ka));
            ++k;
        }
        e->hash = h;
    }
}

/* ---- drawing --------------------------------------------------------------------------------------- */

static int is_focused(int idx)
{
    int k = 0;
    for (int i = 0; i < ncur; ++i)
        if (cur[i].interactive) {
            if (i == idx)
                return k == focus;
            ++k;
        }
    return 0;
}

static unsigned hold_prog(const struct el *e, int idx, unsigned scale)
{
    if (idx != pressed || !e->ms)
        return 0;
    uint64_t d = sb_ms() - t_press;
    return d >= e->ms ? scale : (unsigned)(d * scale / e->ms);
}

/* 0.6: grey cards are glass over the wallpaper in normal mode; every other colour (accents, red) stays solid */
static void card_round(int x, int y, int w, int h, int r, uint32_t c)
{
    if (look_glass_on() && (c == C_CARD || c == C_CARD2 || c == C_CARD3))
        look_glass(x, y, w, h, r, 0xffffffffU, c == C_CARD3 ? 96 : c == C_CARD2 ? 72 : 46);
    else
        ui_round(x, y, w, h, r, c);
}

static void card_disc(int cx, int cy, int r, uint32_t c)
{
    if (look_glass_on() && (c == C_CARD || c == C_CARD2 || c == C_CARD3))
        look_glass_disc(cx, cy, r, 0xffffffffU, c == C_CARD3 ? 96 : 46);
    else
        ui_disc(cx, cy, r, c);
}

static void draw_el_raw(const struct el *e, int idx);

/* 0.6.2: a scrolled element is clipped below SCROLL_TOP (Back stays above it) */
static void draw_el(const struct el *e, int idx)
{
    if (e->scrolled) {
        ui_clip(0, SCROLL_TOP, UI_W, UI_H - SCROLL_TOP);
        draw_el_raw(e, idx);
        ui_noclip();
    } else {
        draw_el_raw(e, idx);
    }
}

static void draw_el_raw(const struct el *e, int idx)
{
    int f = is_focused(idx), p = idx == pressed;
    uint32_t card = p ? C_CARD3 : f ? C_CARD2 : C_CARD;
    switch (e->kind) {
    case E_TITLE:
        ui_text_cut(F_TITLE, e->x, e->y + 100, e->a, e->w, C_L1);
        break;
    case E_BACK:
        if (f)
            card_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 83, C_STAR);
        card_round(e->x, e->y, e->w, e->h, 75, card);
        ui_icon(ICON_BACK, e->x + 50, e->y + (e->h - 64) / 2, C_STAR);
        ui_text(F_BTN, e->x + 140, e->y + 96, e->a, C_STAR);
        break;
    case E_BUTTON: {
        uint32_t lc = (e->flags & UI_RED) ? C_RED : (e->flags & UI_ACCENT) ? C_STAR : (e->flags & UI_DIM) ? C_L3 : C_L1;
        if (f)
            card_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 68, C_STAR);
        card_round(e->x, e->y, e->w, e->h, 60, (e->flags & UI_RED) && !p ? 0xff2a0f0eU : card);
        int ty = e->b[0] ? e->y + 118 : e->y + e->h / 2 + 26;
        ui_text_cut(F_TITLE_S, e->x + 70, ty, e->a, e->w - 140, lc);
        if (e->b[0])
            ui_text_cut(F_DETAIL, e->x + 72, e->y + 200, e->b, e->w - 140, C_L2);
        break;
    }
    case E_HOLD: {
        /* 0.4.1: a red hold (Call 112, the alarm) is a filled red-tinted card with a light label, so it reads as the
         * one emergency action on its screen; the fill grows brighter while it is held */
        int red = (e->flags & UI_RED) != 0;
        uint32_t ac = red ? C_RED : C_STAR, base = red ? 0xff3d1210U : C_CARD;
        if (f || key_hold_armed == idx)
            card_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 68, key_hold_armed == idx ? C_ORANGE : C_STAR);
        card_round(e->x, e->y, e->w, e->h, 60, base);
        unsigned pw = hold_prog(e, idx, (unsigned)e->w);
        if (pw >= 120)
            card_round(e->x, e->y, (int)pw, e->h, 60, ui_mix(ac, base, red ? 150 : 110));
        else if (pw)
            card_round(e->x, e->y, 120, e->h, 60, ui_mix(ac, base, (unsigned)((red ? 150 : 110) * pw / 120)));
        ui_text_c(F_TITLE_S, e->x + e->w / 2, e->y + e->h / 2 + 26, e->a, red ? 0xffffd6d2U : ac);
        if (key_hold_armed == idx)
            ui_text_c(F_SMALL, e->x + e->w / 2, e->y + e->h - 24, "press Bixby again", C_ORANGE);
        break;
    }
    case E_BIG:
        ui_text(F_HEAD, e->x + 4, e->y + 50, e->a, C_L2);
        ui_text_cut(ui_has_font(F_VALUE) ? F_VALUE : F_BIG, e->x, e->y + 230, e->b, e->w, e->color);
        break;
    case E_TEXT: {
        const char *p = e->b;
        for (int ln = 0; ln < (e->n ? e->n : 1) && *p; ++ln) {
            unsigned k = ln + 1 < (e->n ? e->n : 1) ? wrap_at(p, e->w) : (unsigned)sb_strlen(p);
            char line[128];
            unsigned m = k < sizeof(line) - 1 ? k : sizeof(line) - 1;
            memcpy(line, p, m);
            line[m] = 0;
            ui_text_cut(F_ROW, e->x + 4, e->y + 60 + 76 * ln, line, e->w, e->color);
            p += k;
            while (*p == ' ')
                ++p;
        }
        break;
    }
    case E_CTEXT:
        ui_text_c(e->value ? e->value : F_ROW, e->x + e->w / 2, e->y + e->h - 30, e->b, e->color);
        break;
    case E_LEVEL: {
        int n = e->n < 1 ? 1 : e->n, gap = 24, sw = (e->w - gap * (n - 1)) / n;
        const char *p0 = e->b;
        if (f)
            card_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 68, C_STAR);
        for (int i = 0; i < n; ++i) {
            char lab[24];
            unsigned k2 = 0;
            while (*p0 && *p0 != '|' && k2 + 1 < sizeof(lab))
                lab[k2++] = *p0++;
            lab[k2] = 0;
            if (*p0 == '|')
                ++p0;
            int x = e->x + i * (sw + gap), on = i == e->value;
            uint32_t bg = on ? (i == 0 ? C_CARD3 : C_ORANGE) : (p ? C_CARD2 : C_CARD);
            card_round(x, e->y, sw, e->h, 56, bg);
            ui_text_c(F_TITLE_S, x + sw / 2, e->y + e->h / 2 + 26, lab, on && i ? 0xff1a1000U : C_L1);
        }
        break;
    }
    case E_TOGGLE:
        if (f)
            card_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 68, C_STAR);
        card_round(e->x, e->y, e->w, e->h, 60, card);
        ui_text_cut(F_TITLE_S, e->x + 70, e->y + e->h / 2 + 26, e->a, e->w - 420, C_L1);
        card_round(e->x + e->w - 300, e->y + (e->h - 130) / 2, 230, 130, 65, e->value ? C_GREEN : C_CARD3);
        ui_disc(e->x + e->w - (e->value ? 135 : 235), e->y + e->h / 2, 55, C_L1);
        break;
    case E_CLOCK:
        if (ui_has_font(F_CLOCK))
            ui_text_c(F_CLOCK, e->x + e->w / 2, e->y + 380, e->b, C_L1);
        else
            ui_text_c(F_BIG, e->x + e->w / 2, e->y + 300, e->b, C_L1);
        break;
    case E_TILE: {
        if (f)
            card_round(e->x - 10, e->y - 10, e->w + 20, e->h + 20, 82, C_STAR);
        if (e->flags & 1) {                     /* the Emergency tile: red, the icon left, two lines */
            card_round(e->x, e->y, e->w, e->h, 72, p ? 0xff5a1a16U : 0xff3d1210U);
            ui_disc(e->x + 120, e->y + e->h / 2, 74, ui_mix(C_RED, 0xff3d1210U, 90));
            ui_icon(e->icon, e->x + 72, e->y + e->h / 2 - 48, 0xffffd0ccU);
            ui_text(F_TITLE, e->x + 240, e->y + 112, e->a, C_L1);
            ui_text_cut(F_DETAIL, e->x + 244, e->y + 170, e->b, e->w - 300, 0xffffb0a8U);
            break;
        }
        card_round(e->x, e->y, e->w, e->h, 72, card);
        if (e->h < 420) {                       /* 1.2-alpha21: 6 or 7 rows: the icon left, the name and its line right */
            int cy = e->y + e->h / 2;
            ui_disc(e->x + 115, cy, 92, ui_mix(e->color, C_CARD, 60));
            if (e->icon)
                ui_icon(e->icon, e->x + 40, cy - 75, e->color);
            int tf = ui_text_w(F_TITLE, e->a) <= e->w - 246 ? F_TITLE : F_TITLE_S;   /* "Calculator": one size down */
            ui_text_cut(tf, e->x + 226, cy + (e->b[0] ? 8 : 30), e->a, e->w - 246, C_L1);
            if (e->b[0])
                ui_text_cut(F_DETAIL, e->x + 228, cy + 70, e->b, e->w - 250, ui_mix(e->color, C_L1, 140));
            break;
        }
        int big = e->h >= 520, ic = big ? 160 : 130;
        ui_disc(e->x + 150, e->y + ic, 100, ui_mix(e->color, C_CARD, 60));
        if (e->icon)
            ui_icon(e->icon, e->x + 75, e->y + ic - 75, e->color);
        ui_text_cut(F_TITLE, e->x + 70, e->y + e->h - (e->b[0] ? 120 : 80), e->a, e->w - 120, C_L1);
        if (e->b[0])
            ui_text_cut(F_DETAIL, e->x + 72, e->y + e->h - 48, e->b, e->w - 130, ui_mix(e->color, C_L1, 140));
        break;
    }
    case E_GAUGE: {                             /* a ring with the value inside (Battery) */
        int cx = e->x + e->w / 2, cy = e->y + 300, r = 250;
        ui_ring(cx, cy, r, 36, ui_mix(e->color, C_BG, 50));
        unsigned deg = (unsigned)(e->value < 0 ? 0 : e->value > 1000 ? 1000 : e->value) * 360U / 1000U;
        static const int16_t q[25] = { 0, 65, 131, 195, 259, 321, 383, 442, 500, 556, 609, 659, 707, 752,
                                       793, 831, 866, 897, 924, 947, 966, 981, 991, 998, 1000 };
        for (unsigned a = 0; a < deg; a += 1) {
            unsigned d = a % 360, i = (d % 90) * 24U / 90U;
            int sv = q[i], cv = q[24 - i], sn, cs;
            if (d < 90) { sn = sv; cs = cv; }
            else if (d < 180) { sn = cv; cs = -sv; }
            else if (d < 270) { sn = -sv; cs = -cv; }
            else { sn = -cv; cs = sv; }
            ui_disc(cx + r * sn / 1000, cy - r * cs / 1000, 18, e->color);
        }
        ui_text_c(ui_has_font(F_VALUE) ? F_VALUE : F_BIG, cx, cy + 50, e->b, C_L1);
        ui_text_c(F_HEAD, cx, cy + 120, e->a, C_L2);
        break;
    }
    case E_ROUND: {
        int cx = e->x + e->w / 2, cy = e->y + 180;
        if (f)
            ui_ring(cx, cy, 168, 10, C_STAR);
        card_disc(cx, cy, 150, p ? C_CARD3 : C_CARD);
        unsigned pr = hold_prog(e, idx, 360);
        /* the ring fills clockwise from the top while held: dots every 3 degrees, an integer quarter-wave
         * sine table (one entry per 3.75 degrees, x1000) */
        static const int16_t q[25] = { 0, 65, 131, 195, 259, 321, 383, 442, 500, 556, 609, 659, 707, 752,
                                       793, 831, 866, 897, 924, 947, 966, 981, 991, 998, 1000 };
        for (unsigned a = 0; a < pr; a += 3) {
            unsigned d = a % 360, i = (d % 90) * 24U / 90U;
            int sv = q[i], cv = q[24 - i], sn, cs;
            if (d < 90) { sn = sv; cs = cv; }
            else if (d < 180) { sn = cv; cs = -sv; }
            else if (d < 270) { sn = -sv; cs = -cv; }
            else { sn = -cv; cs = sv; }
            ui_disc(cx + 160 * sn / 1000, cy - 160 * cs / 1000, 9, e->color);
        }
        ui_icon(e->icon, cx - 48, cy - 48, e->color);
        ui_text_c(F_BTN, cx, e->y + e->h - 40, e->a, C_L1);
        break;
    }
    case E_FIELD: {
        ui_fill(e->x, e->y, e->w, e->h, e->color);
        if (screen == SCR_LIGHT && e->value == 1) {     /* 1.2-alpha5: Survival's status strip */
            ui_text_c(F_ROW, UI_W / 2, e->y + 70, e->b, 0xffffffffU);
            ui_text_c(F_ROW, UI_W / 2, e->y + 140, e->a, 0xffaeaeb2U);
            break;
        }
        if (screen == SCR_LIGHT && e->b[0])
            ui_text_c(F_ROW, UI_W / 2, UI_H - 200, e->b, ui_mix(0xff000000U, e->color, 90));
        if (screen == SCR_SOS) {
            uint32_t tc = (e->color & 0xffffffU) == 0xffffffU ? C_RED : 0xffffffffU;
            ui_text_c(ui_has_font(F_VALUE) ? F_VALUE : F_BIG, UI_W / 2, 1100, "SOS", tc);
            ui_text_c(F_TITLE, UI_W / 2, 1350, e->b, tc);
        }
        break;
    }
    case E_CHART: {                             /* caption|left|right in a; values in b; the line, a dot at the end */
        char cap[48], lft[24], rgt[24];
        const char *s = e->a;
        unsigned k = 0;
        for (; *s && *s != '|' && k + 1 < sizeof(cap); ++s)
            cap[k++] = *s;
        cap[k] = 0;
        if (*s == '|')
            ++s;
        k = 0;
        for (; *s && *s != '|' && k + 1 < sizeof(lft); ++s)
            lft[k++] = *s;
        lft[k] = 0;
        if (*s == '|')
            ++s;
        k = 0;
        for (; *s && k + 1 < sizeof(rgt); ++s)
            rgt[k++] = *s;
        rgt[k] = 0;
        ui_text(F_HEAD, e->x + 4, e->y + 40, cap, C_L2);
        int cx = e->x, cy = e->y + 64, cw = e->w, ch = e->h - 64 - 60;
        card_round(cx, cy, cw, ch, 40, card);
        for (int g = 1; g < 4; ++g)             /* quarter lines */
            ui_fill(cx + 30, cy + ch * g / 4, cw - 60, 2, ui_mix(C_L3, card, 60));
        int16_t v[60];
        memcpy(v, e->b, sizeof(v));
        int lo = e->value, hi = (int)e->ms, n = e->n;
        int px = -1, py = -1;
        for (int i = 0; i < n; ++i) {
            if (v[i] == -32768) {
                px = -1;
                continue;
            }
            int val = v[i] < lo ? lo : v[i] > hi ? hi : v[i];
            int x = cx + 40 + (n > 1 ? (cw - 80) * i / (n - 1) : (cw - 80) / 2);
            int64_t span = (int64_t)hi - lo;
            int y = cy + ch - 30 - (int)((int64_t)(ch - 60) * ((int64_t)val - lo) / (span > 0 ? span : 1));
            if (y < cy + 30)
                y = cy + 30;
            if (y > cy + ch - 30)
                y = cy + ch - 30;
            if (px >= 0)
                ui_line(px, py, x, y, 8, e->color);
            px = x, py = y;
        }
        if (px >= 0)
            ui_disc(px, py, 14, e->color);
        ui_text(F_SMALL, e->x + 8, e->y + e->h - 14, lft, C_L3);
        ui_text_r(F_SMALL, e->x + e->w - 8, e->y + e->h - 14, rgt, C_L3);
        break;
    }
    case E_ICON: {                              /* 0.6: an app on the normal-mode home: tinted glass, a white glyph, the name */
        int s = 240, ix = e->x + (e->w - s) / 2, iy = e->y + 10;
        if (f)
            ui_round(ix - 10, iy - 10, s + 20, s + 20, 70, C_STAR);
        look_glass(ix, iy, s, s, 62, e->color, p ? 210 : 165);
        if (e->icon)
            ui_icon(e->icon, ix + (s - 150) / 2, iy + (s - 150) / 2, 0xffffffffU);
        ui_text_c(F_DETAIL, e->x + e->w / 2, iy + s + 64, e->a, C_L1);
        break;
    }
    case E_SCROLLBAR:
        ui_round(e->x, e->y, e->w, e->h, e->w / 2, C_L3);
        break;
    case E_WIDGET: {                            /* 0.6: the clock widget */
        look_glass(e->x, e->y, e->w, e->h, 72, 0xffffffffU, 40);
        ui_text(ui_has_font(F_VALUE) ? F_VALUE : F_BIG, e->x + 70, e->y + 200, e->a, C_L1);
        ui_text(F_TITLE_S, e->x + 74, e->y + 290, e->b, C_L2);
        if (e->b[64]) {                         /* the battery: "76 %|about 3 h" */
            char pc[24], est2[40];
            unsigned k = 0, j = 0;
            const char *s2 = e->b + 64;
            while (s2[k] && s2[k] != '|' && k + 1 < sizeof(pc))
                pc[k] = s2[k], ++k;
            pc[k] = 0;
            if (s2[k] == '|')
                ++k;
            while (s2[k] && j + 1 < sizeof(est2))
                est2[j++] = s2[k++];
            est2[j] = 0;
            ui_text_r(F_TITLE, e->x + e->w - 70, e->y + 190, pc, C_L1);
            ui_text_r(F_DETAIL, e->x + e->w - 72, e->y + 270, est2, C_L2);
        }
        break;
    }
    case E_INPUT: {                             /* the field: its label, the text (or dots), a cursor, Show / Hide */
        ui_text(F_HEAD, e->x + 4, e->y + 40, e->a, C_L2);
        card_round(e->x, e->y + 64, e->w, e->h - 64, 48, card);
        int tx = e->x + 50, ty = e->y + 64 + (e->h - 64) / 2 + 24;
        int shown_w = 0;
        int room = e->w - 330;
        if (e->flags) {
            int d = e->value < 20 ? e->value : 20;
            for (int i = 0; i < d; ++i)
                ui_disc(tx + 18 + i * 44, ty - 18, 14, C_L1);
            shown_w = d * 44;
            if (e->value > 20) {            /* more than fit: the count after the dots */
                char cnt[8] = { ' ', '(', 0 };
                int v = e->value, k = 2;
                if (v >= 10)
                    cnt[k++] = (char)('0' + v / 10);
                cnt[k++] = (char)('0' + v % 10);
                cnt[k++] = ')';
                cnt[k] = 0;
                shown_w += ui_text(F_HEAD, tx + shown_w, ty - 6, cnt, C_L2);
            }
        } else if (e->b[0]) {
            const char *t = e->b;           /* the end of the text stays in view (where the typing is) */
            while (*t && ui_text_w(F_TITLE_S, t) > room)
                ++t;
            ui_text(F_TITLE_S, tx, ty, t, C_L1);
            shown_w = ui_text_w(F_TITLE_S, t);
        }
        ui_fill(tx + shown_w + 8, ty - 52, 5, 64, C_STAR);              /* the cursor */
        if (e->n)
            ui_text_c(F_HEAD, e->x + e->w - 120, ty - 6, e->flags ? "Show" : "Hide", C_STAR);
        break;
    }
    case E_KEYS: {
        if (look_glass_on())
            look_glass(0, e->y, UI_W, e->h, 0, 0xff000000U, 120);
        else
            ui_fill(0, e->y, UI_W, e->h, 0xff101012U);
        for (int r = 0; r < 4; ++r) {
            const struct krow *row = &kb_rows[e->value & 3][r];
            int n = (int)sb_strlen(row->cap), tot = 0;
            for (int i = 0; i < n; ++i)
                tot += row->w[i];
            int kx = 20 + (20 - tot) * KB_UNIT / 2, ky = KB_TOP + r * (KB_ROW + KB_GAP);
            for (int i = 0; i < n; ++i) {
                int w = row->w[i] * KB_UNIT;
                uint8_t c = (uint8_t)row->cap[i];
                int special = c < 7;
                uint32_t bg = c == 4 ? C_STAR : special ? C_CARD3 : C_CARD;
                card_round(kx + 6, ky, w - 12, KB_ROW, 26, bg);
                char s[4] = { (char)c, 0, 0, 0 };
                const char *lab = s;
                if (c == 1)
                    lab = e->value >= 2 ? (e->value == 2 ? "#+=" : "123") : e->value == 1 ? "ABC" : "abc";
                if (c == 6)
                    lab = e->value == 3 ? "123" : "#+=";
                if (c == 2)
                    lab = "Delete";
                if (c == 3)
                    lab = e->value >= 2 ? "abc" : "123";
                if (c == 4)
                    lab = (e->flags & 8U) ? "Enter" : (e->flags & 2U) ? "Join" : "Done";
                if (c == 5)
                    lab = "space";
                if (c == 1 && e->value < 2)
                    lab = e->value == 1 ? "SHIFT" : "shift";
                int fnt = special ? F_HEAD : F_TITLE_S;
                if (!ui_has_font(fnt))
                    fnt = F_ROW;
                ui_text_c(fnt, kx + w / 2, ky + KB_ROW / 2 + (special ? 14 : 24), lab,
                          c == 4 ? 0xff000000U : c == 1 && e->value == 1 ? C_STAR : C_L1);
                kx += w;
            }
        }
        break;
    }
    case E_TOAST:
        card_round(e->x, e->y, e->w, e->h, 75, C_CARD3);
        ui_text_c(F_ROW_M, e->x + e->w / 2, e->y + 98, e->b, C_L1);
        break;
    default:
        break;
    }
}

/* clear what an element covered (with its focus ring margin) */
static void clear_rect(const struct el *e)
{
    int m = 14;
    int x = e->x - m, y = e->y - m, w = e->w + 2 * m, h = e->h + 2 * m;
    if (e->scrolled && y < SCROLL_TOP)
        h -= SCROLL_TOP - y, y = SCROLL_TOP;
    if (y < UI_STATUS_H && screen != SCR_LIGHT && screen != SCR_SOS)
        y = UI_STATUS_H, h -= UI_STATUS_H - (e->y - m);
    if (h > 0)
        look_bg(x, y, w, h);
}

static int overlaps(const struct el *a, const struct el *b)
{
    int m = 16;
    return a->x - m < b->x + b->w + m && b->x - m < a->x + a->w + m && a->y - m < b->y + b->h + m &&
           b->y - m < a->y + a->h + m;
}

/* the last drawn frame, for the self test */
int kui_stat_n, kui_stat_ms, kui_stat_frames;

const char *kui_screen_name(void)
{
    switch (screen) {
    case SCR_LOCK: return "lock";
    case SCR_HOME: return "home";
    case SCR_APP: return "app";
    case SCR_POWER: return "power menu";
    case SCR_CONFIRM: return "confirm";
    case SCR_LIGHT: return "light";
    case SCR_SOS: return "sos";
    case SCR_OFF: return "off";
    case SCR_CONTROL: return "control center";
    case SCR_GUEST: return "linux";
    default: return "?";
    }
}

/* 1.2: the guest is told when its view is shown or left, whatever left it (home, the power menu, SOS, the screen) */
static void guest_view_sync(void)
{
    if (screen == SCR_GUEST && !in_guest) {
        in_guest = 1;
        guest_drawn = 0;
        guest_show(1);
    } else if (screen != SCR_GUEST && in_guest) {
        in_guest = 0;
        guest_show(0);
    }
}

/* 1.2-alpha3: the terminal's keyboard (the apps' keyboard, its action key Enter) under the 2x view */
static void guest_keys_draw(void)
{
    struct el k;
    memset(&k, 0, sizeof(k));
    k.kind = E_KEYS;
    k.y = KB_TOP - 24;
    k.h = UI_H - k.y;
    k.value = gk_layer;
    k.flags = 8;
    draw_el_raw(&k, -1);
}

/* the guest's frame between Starbite's bars: the app at 2x (1440 x 2880, a 40 px bar below), the terminal at 2x
 * (1440 x 2120) above the keyboard. Only the rows the guest changed are copied and presented (damage per row). */
static void guest_draw(void)
{
    int term = guest_mode() == GUEST_TERM;
    ui_noclip();
    int all = !guest_drawn || full;
    if (all) {
        guest_full_next();                      /* the whole surface: the back buffer had other screens */
        ui_fill(0, 0, UI_W, 40, 0xff101828U);
        if (term) {
            if (ui_has_font(F_HEAD))
                ui_text_c(F_HEAD, UI_W / 2, 31, "Linux terminal  \xc2\xb7  hold Bixby: home", C_L2);
            guest_keys_draw();
        } else {
            ui_fill(0, UI_H - 40, UI_W, 40, 0xff101828U);
            if (ui_has_font(F_HEAD))
                ui_text_c(F_HEAD, UI_W / 2, UI_H - 9, "Linux  \xc2\xb7  hold Bixby or tap here: home", C_L2);
        }
    }
    int y0, y1;
    guest_blit(ui_back(), UI_W, 0, 40, &y0, &y1);
    if (all)
        ui_dirty_all();
    else if (y0 >= 0)
        ui_dirty(0, y0, UI_W, y1 - y0 + 1);
    if (ui_present())
        panel_kick();
    if (!K.first_frame_ms)
        K.first_frame_ms = sb_ms() ? sb_ms() : 1;
    guest_drawn = 1;
    full = 0;
    shown_screen = SCR_GUEST;
    ncur = nshown = 0;
    kui_stat_n = 1;
    ++kui_stat_frames;
}

void ui_kdraw(void)
{
    if (screen == SCR_OFF)
        return;
    guest_view_sync();
    if (screen == SCR_GUEST && guest_shown()) {
        if (dirty || guest_frame_new() || !guest_drawn) {
            dirty = 0;
            guest_draw();
        }
        return;
    }
    if (guest_drawn) {                          /* out of the guest's frames (it stopped or failed while shown) */
        guest_drawn = 0;
        full = dirty = 1;
    }
    if (!dirty && pressed < 0)
        return;
    dirty = 0;
    uint64_t t_draw = sb_ms();
    if (screen != shown_screen && shown_screen) {
        focus = -1;                             /* a new screen starts without a key focus */
        key_hold_armed = -1;
    }
    build();
    int fullscreen = screen == SCR_LIGHT || screen == SCR_SOS;
    int wipe = 0;
    if (full || screen != shown_screen) {
        /* a screen change between the lock screen, home and the apps slides in (a wipe in 4 bands, about 70 ms):
         * forward from the right, back from the left; full-screen fields and the first frame appear at once */
        if (screen != shown_screen && shown_screen && shown_screen != SCR_OFF && !fullscreen &&
            shown_screen != SCR_LIGHT && shown_screen != SCR_SOS && shown_screen != SCR_GUEST && screen != SCR_GUEST)
            wipe = (screen == SCR_LOCK || (screen == SCR_HOME && shown_screen == SCR_APP)) ? -1 : 1;
        ui_noclip();
        look_bg(0, 0, UI_W, UI_H);
        status_forced = 1;
        for (int i = 0; i < ncur; ++i)
            draw_el(&cur[i], i);
        full = 0;
    } else {
        /* elements that changed, vanished or moved: clear their old and new rects, then redraw every
         * element that touches a cleared rect */
        static uint8_t redraw[EL_MAX];
        memset(redraw, 0, sizeof(redraw));
        int any = 0;
        int n = ncur > nshown ? ncur : nshown;
        for (int i = 0; i < n; ++i) {
            int changed = i >= ncur || i >= nshown || cur[i].hash != shown[i].hash;
            if (!changed)
                continue;
            any = 1;
            if (i < nshown)
                clear_rect(&shown[i]);
            if (i < ncur) {
                clear_rect(&cur[i]);
                redraw[i] = 1;
            }
            for (int j = 0; j < ncur; ++j)
                if ((i < nshown && overlaps(&shown[i], &cur[j])) || (i < ncur && overlaps(&cur[i], &cur[j])))
                    redraw[j] = 1;
        }
        if (any)
            for (int i = 0; i < ncur; ++i)
                if (redraw[i])
                    draw_el(&cur[i], i);
    }
    if (!fullscreen && screen != SCR_CONFIRM)
        build_status();
    shown_screen = screen;
    memcpy(shown, cur, sizeof(cur[0]) * (size_t)ncur);
    nshown = ncur;
    if (wipe) {
        ui_dirty_clear();
        for (int k = 0; k < 4; ++k) {
            int bw = UI_W / 4, bx = wipe > 0 ? UI_W - (k + 1) * bw : k * bw;
            ui_dirty(bx, 0, bw, UI_H);
            (void)ui_present();
            panel_kick();
            if (k < 3)
                sb_mdelay(16);
        }
    }
    uint64_t px = wipe ? (uint64_t)UI_W * UI_H : ui_present();
    if (px)
        panel_kick();
    if (px && !K.first_frame_ms)
        K.first_frame_ms = sb_ms() ? sb_ms() : 1;
    kui_stat_n = ncur;
    if ((int)(sb_ms() - t_draw) > kui_stat_ms)
        kui_stat_ms = (int)(sb_ms() - t_draw);   /* the slowest since the self test last read it */
    ++kui_stat_frames;
    if (pressed >= 0 && !kt_pending(KT_UI, 1))
        kt_set(KT_UI, 1, sb_ms(), 40, 0);   /* animate the hold (not re-armed while pending: it would never fire) */
}

/* ---- events --------------------------------------------------------------------------------------- */

static int hit(int x, int y)
{
    for (int i = ncur - 1; i >= 0; --i) {
        const struct el *e = &cur[i];
        if (e->interactive && x >= e->x && x < e->x + e->w && y >= e->y && y < e->y + e->h &&
            !(e->scrolled && y < SCROLL_TOP))
            return i;
    }
    return -1;
}

static int interactive_count(void)
{
    int k = 0;
    for (int i = 0; i < ncur; ++i)
        k += cur[i].interactive;
    return k;
}

static int focus_el(void)
{
    int k = 0;
    for (int i = 0; i < ncur; ++i)
        if (cur[i].interactive) {
            if (k == focus)
                return i;
            ++k;
        }
    return -1;
}

static void power_menu_pick(int id);
static void lock_pick(int id);

static void activate(int i, int x, int y)
{
    const struct el *e = &cur[i];
    switch (screen) {
    case SCR_CONTROL:
        control_pick(e, x);
        break;
    case SCR_LOCK:
        lock_pick((int)e->id);
        break;
    case SCR_HOME:
        if (e->id == GUEST_TILE || e->id == GUEST_TILE_TERM)
            kui_guest_open(e->id == GUEST_TILE_TERM ? GUEST_TERM : GUEST_APP);
        else
            kui_open_app((int)e->id);
        break;
    case SCR_GUEST:
        if (e->id == 2)
            guest_stop();
        else
            kui_home();
        break;
    case SCR_POWER:
        power_menu_pick((int)e->id);
        break;
    case SCR_CONFIRM: {
        void (*d)(int) = cf_done;
        cf_done = 0;
        screen = cf_return;
        focus = -1;                             /* the focus was the confirmation's, not this screen's */
        full = dirty = 1;
        if (d)
            d(e->kind == E_HOLD && e->id == 1);
        break;
    }
    case SCR_SOS:
        if (e->kind == E_HOLD)
            sos_set(0);
        break;
    case SCR_APP:
        if (app_front < 0)
            break;
        if (e->kind == E_BACK) {
            app_event_send(app_front, EV_APP_BACK, 0, 0);
            if (from_lock)
                kui_lock();                 /* the card was opened from the lock screen: back there */
            else
                kui_home();
        } else if (e->kind == E_BUTTON) {
            app_event_send(app_front, EV_APP_BUTTON, e->id, 0);
        } else if (e->kind == E_HOLD) {
            app_event_send(app_front, EV_APP_HOLD, e->id, 0);
        } else if (e->kind == E_TOGGLE) {
            app_event_send(app_front, EV_APP_TOGGLE, e->id, !e->value);
        } else if (e->kind == E_KEYS) {
            if (x >= 0)
                kb_press(x, y);
        } else if (e->kind == E_INPUT) {
            if (x >= e->x + e->w - 260 && e->n) {   /* Show / Hide */
                kb_show = !kb_show;
                kb_refresh();
            }
        } else if (e->kind == E_LEVEL) {
            int n = e->n < 1 ? 1 : e->n, v;
            if (x < 0)
                v = (e->value + 1) % n;     /* keys: the next level */
            else
                v = (x - e->x) * n / (e->w > 0 ? e->w : 1);
            if (v < 0)
                v = 0;
            if (v >= n)
                v = n - 1;
            app_event_send(app_front, EV_APP_LEVEL, e->id, v);
        }
        break;
    default:
        break;
    }
    dirty = 1;
}

static void back(void)
{
    switch (screen) {
    case SCR_CONTROL:
        control_close();
        break;
    case SCR_APP:
        if (app_front >= 0)
            app_event_send(app_front, EV_APP_BACK, 0, 0);
        if (from_lock)
            kui_lock();
        else
            kui_home();
        break;
    case SCR_HOME:
        kui_lock();
        break;
    case SCR_POWER:
        screen = cf_return;
        full = dirty = 1;
        break;
    case SCR_CONFIRM: {
        void (*d)(int) = cf_done;
        cf_done = 0;
        screen = cf_return;
        full = dirty = 1;
        if (d)
            d(0);
        break;
    }
    case SCR_LIGHT:
        kui_light(0);
        break;
    case SCR_GUEST:
        kui_home();
        break;
    default:
        break;
    }
}

/* 1.2: the home tiles "Linux" (GUEST_APP) and "Terminal" (GUEST_TERM): start that guest (if it is off) and show
 * its view. The other one still running (hidden) is stopped first: one guest at a time on the big cores. */
void kui_guest_open(int mode)
{
    int gs = guest_state();
    if ((gs == GUEST_STARTING || gs == GUEST_RUNNING) && guest_mode() != mode) {
        guest_stop();
        sb_log("ui: the other Linux stopped first");
        kui_toast("Closing the other Linux: tap again in a moment", 2500);
        return;
    }
    if (gs == GUEST_STOPPING) {
        kui_toast("Linux is stopping: tap again in a moment", 2000);
        return;
    }
    if (gs == GUEST_OFF || gs == GUEST_FAILED) {
        gk_layer = 0;
        kmodem_stop("Linux started");      /* 1.2-alpha21 */
        int r = guest_start_mode(mode);
        if (r == -2) {
            sb_log("ui: Linux refused: the heater has the big cores");
            kui_toast("The heater has the big cores: stop it first", 2500);
            return;
        }
        if (r < 0)
            sb_logf("ui: Linux did not start: ", guest_why());
        else
            sb_log(mode == GUEST_TERM ? "ui: the Linux terminal starting (the home tile)" : "ui: Linux starting (the home tile)");
    }
    screen = SCR_GUEST;
    focus = -1;
    full = dirty = 1;
    guest_view_sync();
}

void kui_power_menu(void)
{
    if (screen == SCR_POWER || screen == SCR_SOS)
        return;
    cf_return = screen == SCR_LIGHT ? light_return : screen;
    screen = SCR_POWER;
    focus = -1;
    full = dirty = 1;
}

/* 0.6.5: the dim clock for Emergency Mode's screen-off (the panel's low-power mode shows it): mostly black */
int em_aod;

void kui_aod_frame(void)
{
    struct el els[3];
    memset(els, 0, sizeof(els));
    els[0].kind = E_CLOCK;
    fmt_clock(els[0].b, sizeof(els[0].b), 0);
    els[0].x = 0, els[0].y = 1000, els[0].w = UI_W, els[0].h = 460;
    struct sbos_battery b;
    battery_get(&b);
    els[1].kind = E_CTEXT;
    {
        struct ub u;
        ub_init(&u);
        if (b.ok) {
            ub_dec(&u, (uint64_t)(b.pct < 0 ? 0 : b.pct));
            ub_s(&u, " %");
        }
        scopy(els[1].b, sizeof(els[1].b), u.b);
    }
    els[1].value = F_ROW_M;
    els[1].color = C_L2;
    els[1].x = 0, els[1].y = 1500, els[1].w = UI_W, els[1].h = 100;
    els[2].kind = E_CTEXT;
    scopy(els[2].b, sizeof(els[2].b), "Emergency Mode: press Power");
    els[2].value = F_SMALL;
    els[2].color = C_L3;
    els[2].x = 0, els[2].y = 2500, els[2].w = UI_W, els[2].h = 80;
    ui_noclip();
    ui_fill(0, 0, UI_W, UI_H, 0xff000000U);
    for (int i = 0; i < 3; ++i)
        draw_el(&els[i], -1);
    ui_dirty_all();
    ui_present();
    panel_kick();
}

void kui_screen_changed(int on)
{
    if (on) {
        if (screen == SCR_OFF)
            screen = SCR_LOCK;
        full = dirty = 1;
    } else {
        pressed = -1;
        key_hold_armed = -1;
        if (screen != SCR_SOS) {
            screen = SCR_OFF;
            shown_screen = SCR_OFF;                 /* the wake draws at once (no transition from black) */
            if (em_aod && look_emergency) {
                kui_aod_frame();                /* 0.6.5: the dim clock (services.c puts the panel in low power) */
            } else {
                ui_noclip();
                ui_fill(0, 0, UI_W, UI_H, 0xff000000U);   /* v0.1: a black screen (OLED: those pixels are off) */
                ui_dirty_all();
                ui_present();
                panel_kick();                   /* the black frame must reach the panel */
            }
            nshown = 0;
        }
        guest_view_sync();                      /* 1.2: the Linux view hidden with the screen */
    }
}

void kui_sos_changed(void)
{
    if (sos_active()) {
        if (screen != SCR_SOS)
            cf_return = screen == SCR_OFF ? SCR_LOCK : screen;
        screen = SCR_SOS;
    } else if (screen == SCR_SOS) {
        screen = cf_return == SCR_SOS || cf_return == SCR_OFF ? SCR_LOCK : cf_return;
    }
    pressed = -1;
    focus = -1;
    key_hold_armed = -1;
    full = dirty = 1;
}

static void lock_pick(int id)
{
    if (id == 0)
        light_on(SBOS_COLOR_WHITE, SCR_LOCK, 10);
    else if (id == 1)
        sos_set(1);
    else if (id == 3 && app_by_id("info") >= 0) {
        /* the medical card, read only: the app is told (EV_APP_SHOW value 1) and Back returns here */
        kui_open_app(app_by_id("info"));
        from_lock = 1;
        app_event_send(app_front, EV_APP_SHOW, 0, 1);
    }
    else
        kui_home();
}

static void power_menu_pick(int id)
{
    int ret = cf_return;
    screen = ret;
    full = dirty = 1;
    switch (id) {
    case 0: power_key_short(); break;
    case 1: power_restart(0); break;
    case 2: power_restart(1); break;
    case 3: power_off_now(); break;
    default: break;
    }
}

/* 1.2-alpha3: a tap on the terminal's keyboard: the apps' keys and layers (kb_press), each character as Linux keys */
static void guest_term_key(int x, int y)
{
    int bx, by, bw, c = kb_key(gk_layer, x, y, &bx, &by, &bw), layer = gk_layer;
    if (!c)
        return;
    if (c == 1)
        gk_layer = gk_layer >= 2 ? (gk_layer == 2 ? 3 : 2) : (gk_layer ? 0 : 1);
    else if (c == 6)
        gk_layer = gk_layer == 3 ? 2 : 3;
    else if (c == 3)
        gk_layer = gk_layer >= 2 ? 0 : 2;
    else if (c == 2)
        guest_type('\b');
    else if (c == 4)
        guest_type('\n');
    else {
        guest_type(c == 5 ? ' ' : (char)c);
        if (gk_layer == 1)
            gk_layer = 0;                       /* one capital, then lower case again */
    }
    if (gk_layer != layer) {                    /* the keyboard redrawn with its new layer */
        ui_noclip();
        guest_keys_draw();
        ui_dirty(0, KB_TOP - 24, UI_W, UI_H - (KB_TOP - 24));
        if (ui_present())
            panel_kick();
    }
}

/* 1.2: input while the guest's frames are shown: touches to Linux, Volume and Bixby as its keys, held Bixby or
 * a tap on the bottom bar home (the guest keeps running hidden; 60 s later it is stopped). The terminal: its
 * keyboard below the view; Volume as Up / Down (the shell's history), short Bixby as Tab. */
static void guest_kevent(const struct kin *ev)
{
    int term = guest_mode() == GUEST_TERM;
    switch (ev->type) {
    case KIN_PRESS:
    case KIN_MOVE:
    case KIN_RELEASE:
        if (term && ev->y >= KB_TOP - 24) {
            if (ev->type == KIN_PRESS)
                guest_term_key(ev->x, ev->y);
            break;
        }
        if (guest_input(ev->type, ev->x, ev->y))
            break;
        if (!term && ev->type == KIN_PRESS && ev->y >= UI_H - 40)
            kui_home();
        break;
    case KIN_UP:
    case KIN_DOWN: {
        unsigned code = term ? (ev->type == KIN_UP ? 103U : 108U) :          /* KEY_UP / KEY_DOWN */
                               (ev->type == KIN_UP ? 115U : 114U);           /* KEY_VOLUMEUP / KEY_VOLUMEDOWN */
        guest_key(code, 1);
        guest_key(code, 0);
        break;
    }
    case KIN_SELECT:
        guest_key(term ? 15U : 158U, 1);        /* KEY_TAB / KEY_BACK */
        guest_key(term ? 15U : 158U, 0);
        break;
    case KIN_BACK:
        kui_home();
        break;
    default:
        break;
    }
}

void ui_kevent(const void *evp)
{
    const struct kin *ev = evp;
    if (screen == SCR_OFF)
        return;
    if (screen == SCR_GUEST && guest_shown()) {
        guest_kevent(ev);
        return;
    }
    if (ncur == 0 || dirty)
        build();
    switch (ev->type) {
    case KIN_PRESS:
#ifdef SB_HOST
        if (getenv_dbg()) printf("dbg press %d,%d at %llu\n", ev->x, ev->y, (unsigned long long)sb_ms());
#endif
        if (screen == SCR_LIGHT) {
            kui_light(0);
            break;
        }
        key_hold_armed = -1;
        pressed = hit(ev->x, ev->y);
        press_y = ev->y;
        scroll_drag = 0;
        t_press = sb_ms();
        press_done = 0;
        dirty = 1;
        break;
    case KIN_MOVE:
        if (screen == SCR_APP && scroll_max > 0 && press_y >= SCROLL_TOP &&
            (pressed < 0 || cur[pressed].kind != E_KEYS)) {
            int dy = ev->y - press_y;
            if (!scroll_drag && (dy > 40 || dy < -40)) {
                scroll_drag = 1;            /* a drag, not a tap: the page scrolls */
                scroll_from = app_scroll;
                pressed = -1;
            }
            if (scroll_drag) {
                int s = scroll_from - dy;
                s = s < 0 ? 0 : s > scroll_max ? scroll_max : s;
                if (s != app_scroll) {
                    app_scroll = s;
                    full = dirty = 1;
                }
                break;
            }
        }
        if (pressed >= 0) {
            const struct el *e = &cur[pressed];
            if (ev->x < e->x - 60 || ev->x > e->x + e->w + 60 || ev->y < e->y - 60 || ev->y > e->y + e->h + 60) {
                pressed = -1;
                dirty = 1;
            }
        }
        break;
    case KIN_RELEASE:
#ifdef SB_HOST
        if (getenv_dbg()) printf("dbg release %d,%d at %llu pressed %d\n", ev->x, ev->y, (unsigned long long)sb_ms(), pressed);
#endif
        if (scroll_drag) {
            scroll_drag = 0;
            pressed = -1;
            press_y = -1;
            dirty = 1;
            break;
        }
        if (pressed < 0 && press_y >= 0 && press_y < UI_STATUS_H + 20 && ev->y < UI_STATUS_H + 200) {
            press_y = -1;
            kui_control_open();             /* 0.6: a tap on the status bar */
            break;
        }
        if (pressed >= 0 && !press_done) {
            int i = pressed;
            const struct el *e = &cur[i];
            pressed = -1;
            if (e->kind == E_HOLD || e->kind == E_ROUND)
                kui_toast(e->kind == E_ROUND ? "Hold the button" : "Keep holding until the bar is full", 1500);
            else if (ev->x >= e->x && ev->x < e->x + e->w && ev->y >= e->y && ev->y < e->y + e->h)
                activate(i, ev->x, ev->y);
        }
        pressed = -1;
        dirty = 1;
        break;
    case KIN_UP:
    case KIN_DOWN: {
        if (screen == SCR_LIGHT) {
            kui_light(0);
            break;
        }
        int n = interactive_count();
        if (!n)
            break;
        if (focus < 0)
            focus = ev->type == KIN_DOWN ? 0 : n - 1;
        else
            focus = (focus + (ev->type == KIN_DOWN ? 1 : n - 1)) % n;
        focus_follow = 1;                   /* 0.6.2: a long page scrolls to the focused element */
        key_hold_armed = -1;
        dirty = 1;
        break;
    }
    case KIN_SELECT: {
        if (screen == SCR_LIGHT) {
            kui_light(0);
            break;
        }
        int i = focus_el();
        if (i < 0) {
            if (interactive_count()) {
                focus = 0;
                dirty = 1;
            }
            break;
        }
        const struct el *e = &cur[i];
        if (e->kind == E_HOLD || e->kind == E_ROUND) {
            if (key_hold_armed == i && sb_ms() < key_hold_until) {
                key_hold_armed = -1;
                activate(i, -1, -1);
            } else {
                key_hold_armed = i;
                key_hold_until = sb_ms() + 3000;
                kt_set(KT_UI, 5, sb_ms(), 3100, 0);
            }
        } else {
            activate(i, -1, -1);
        }
        dirty = 1;
        break;
    }
    case KIN_BACK:
        back();
        break;
    default:
        break;
    }
}

/* the UI task's timers: 1 hold animation, 2 toast end, 3 light hint, 4 light auto-off, 5 key hold expiry */
void ui_ktimer(unsigned id)
{
#ifdef SB_HOST
    if (getenv_dbg()) printf("dbg ui timer %u at %llu pressed %d\n", id, (unsigned long long)sb_ms(), pressed);
#endif
    if (id == 1 && pressed >= 0) {
        const struct el *e = &cur[pressed];
        if ((e->kind == E_HOLD || e->kind == E_ROUND) && !press_done && sb_ms() - t_press >= e->ms) {
            int i = pressed;
            press_done = 1;
            pressed = -1;
            activate(i, -1, -1);
        }
        dirty = 1;
    } else if (id == 6) {
        panel_idle();
    } else if (id == 7) {
        fled_timer();                       /* 0.6.3: the flash pin low after the pulse */
    } else if (id == 4 && screen == SCR_LIGHT) {
        kui_light(0);
    } else if (id == 9) {
        vib_timer();                        /* 1.2-alpha7: the vibration's pulse or gap ended */
    } else if (id == 8) {
        if (screen == SCR_LIGHT && light_heat)
            dirty = 1;                      /* the strip's numbers */
        else
            kt_cancel(KT_UI, 8);
    } else if (id == 5) {
        if (key_hold_armed >= 0 && sb_ms() >= key_hold_until)
            key_hold_armed = -1;
        dirty = 1;
    } else {
        dirty = 1;
    }
}

/* once a minute and when the battery changes: the clock and status */
void ui_kminute(void)
{
    if (screen == SCR_LOCK || screen == SCR_HOME || screen == SCR_APP || screen == SCR_POWER)
        dirty = 1;
}

int kui_screen(void)
{
    return screen;
}

void ui_kinit(void)
{
    screen = SCR_LOCK;
    full = dirty = 1;
}
