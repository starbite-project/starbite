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
       E_FIELD, E_TOAST, E_CTEXT };
enum { SCR_LOCK = 1, SCR_HOME, SCR_APP, SCR_POWER, SCR_CONFIRM, SCR_LIGHT, SCR_SOS, SCR_OFF };

struct el {
    uint8_t kind, interactive;
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
static int key_hold_armed = -1;             /* a hold element selected once with Bixby */
static uint64_t key_hold_until;
static int dirty = 1, full = 1;
static int light_return = SCR_LOCK;
static uint32_t light_argb;
static uint64_t light_hint_until, light_off_at;
static char toast[64];
static uint64_t toast_until;
static int app_awake;
static struct ui_status st_shown;
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
                     kind == E_HOLD || kind == E_TILE || kind == E_ROUND;
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

void kui_end(void)
{
    app_building = 0;
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
        return;
    }
    light_on(argb, SCR_APP, 10);
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
    if (app_front >= 0)
        app_event_send(app_front, EV_APP_HIDE, 0, 0);
    app_front = -1;
    app_awake = 0;
    screen = SCR_HOME;
    focus = -1;
    full = dirty = 1;
}

void kui_lock(void)
{
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
        default: e->h = 120; break;
        }
        e->y = y;
        y += e->h + (e->kind == E_TEXT ? 4 : 40);
    }
}

static void build_status(void)
{
    struct ui_status s;
    memset(&s, 0, sizeof(s));
    struct sbos_time t;
    if (clock_now(&t)) {
        struct ub u;
        ub_init(&u);
        ub_dec2(&u, (unsigned)t.h);
        ub_c(&u, ':');
        ub_dec2(&u, (unsigned)t.mi);
        scopy(s.left, sizeof(s.left), u.b);
    }
    struct sbos_battery b;
    battery_get(&b);
    s.bat = b.ok ? b.pct : -1;
    s.charging = b.charger != 0;
    if (heater_running_ui())
        scopy(s.mid, sizeof(s.mid), "HEATING");
    s.mid_c = C_ORANGE;
    if (b.ok) {
        struct ub u;
        ub_init(&u);
        ub_dec(&u, (uint64_t)(b.pct < 0 ? 0 : b.pct));
        ub_s(&u, " %");
        scopy(s.right, sizeof(s.right), u.b);
    }
    ui_status_draw(&s, status_forced);
    status_forced = 0;
    st_shown = s;
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
    scopy(e->b, sizeof(e->b), "Hold a button");
    e->value = F_SMALL;
    e->color = C_L3;
    e->x = 0, e->y = 2660, e->w = UI_W, e->h = 80;
}

static void build_home(void)
{
    int heat = app_by_id("heat"), sos_a = app_by_id("sos");
    for (int i = 0, t = 0; i < napps && t < 6; ++i) {
        if (app_hidden(i))
            continue;
        struct el *e = add(cur, &ncur, E_TILE);
        e->id = (uint32_t)i;
        scopy(e->a, sizeof(e->a), app_name(i));
        e->icon = app_icon(i);
        e->color = app_color(i) ? app_color(i) : C_STAR;
        e->w = 625;
        e->h = 600;
        e->x = MX + (t % 2) * (625 + 50);
        e->y = 300 + (t / 2) * (600 + 50);
        ++t;
        if (i == heat && heater_running_ui())
            scopy(e->b, sizeof(e->b), "Heating");
        if (i == sos_a && sos_active())
            scopy(e->b, sizeof(e->b), "On");
    }
    struct el *e = add(cur, &ncur, E_CTEXT);
    scopy(e->b, sizeof(e->b), "Bixby: open  " U_DOT "  Power: screen off  " U_DOT "  hold Power: menu");
    e->value = F_SMALL;
    e->color = C_L3;
    e->x = 0, e->y = 2290, e->w = UI_W, e->h = 80;
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

static void build_light(void)
{
    struct el *e = add(cur, &ncur, E_FIELD);
    e->x = 0, e->y = 0, e->w = UI_W, e->h = UI_H;
    e->color = light_argb;
    if (sb_ms() < light_hint_until)
        scopy(e->b, sizeof(e->b), "Tap or press a key to turn it off");
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

static void build(void)
{
    ncur = 0;
    switch (screen) {
    case SCR_LOCK: build_lock(); break;
    case SCR_HOME: build_home(); break;
    case SCR_APP: build_app(); break;
    case SCR_POWER: build_power(); break;
    case SCR_CONFIRM: build_confirm(); break;
    case SCR_LIGHT: build_light(); break;
    case SCR_SOS: build_sos(); break;
    default: break;
    }
    if (toast[0] && sb_ms() < toast_until && screen != SCR_LIGHT && screen != SCR_SOS && screen != SCR_OFF) {
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

static void draw_el(const struct el *e, int idx)
{
    int f = is_focused(idx), p = idx == pressed;
    uint32_t card = p ? C_CARD3 : f ? C_CARD2 : C_CARD;
    switch (e->kind) {
    case E_TITLE:
        ui_text_cut(F_TITLE, e->x, e->y + 100, e->a, e->w, C_L1);
        break;
    case E_BACK:
        if (f)
            ui_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 83, C_STAR);
        ui_round(e->x, e->y, e->w, e->h, 75, card);
        ui_icon(ICON_BACK, e->x + 50, e->y + (e->h - 64) / 2, C_STAR);
        ui_text(F_BTN, e->x + 140, e->y + 96, e->a, C_STAR);
        break;
    case E_BUTTON: {
        uint32_t lc = (e->flags & UI_RED) ? C_RED : (e->flags & UI_ACCENT) ? C_STAR : (e->flags & UI_DIM) ? C_L3 : C_L1;
        if (f)
            ui_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 68, C_STAR);
        ui_round(e->x, e->y, e->w, e->h, 60, (e->flags & UI_RED) && !p ? 0xff2a0f0eU : card);
        int ty = e->b[0] ? e->y + 118 : e->y + e->h / 2 + 26;
        ui_text_cut(F_TITLE_S, e->x + 70, ty, e->a, e->w - 140, lc);
        if (e->b[0])
            ui_text_cut(F_DETAIL, e->x + 72, e->y + 200, e->b, e->w - 140, C_L2);
        break;
    }
    case E_HOLD: {
        uint32_t ac = (e->flags & UI_RED) ? C_RED : C_STAR;
        if (f || key_hold_armed == idx)
            ui_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 68, key_hold_armed == idx ? C_ORANGE : C_STAR);
        ui_round(e->x, e->y, e->w, e->h, 60, C_CARD);
        unsigned pw = hold_prog(e, idx, (unsigned)e->w);
        if (pw >= 120)
            ui_round(e->x, e->y, (int)pw, e->h, 60, ui_mix(ac, C_CARD, 110));
        else if (pw)
            ui_round(e->x, e->y, 120, e->h, 60, ui_mix(ac, C_CARD, (unsigned)(110 * pw / 120)));
        ui_text_c(F_TITLE_S, e->x + e->w / 2, e->y + e->h / 2 + 26, e->a, ac);
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
            ui_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 68, C_STAR);
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
            ui_round(x, e->y, sw, e->h, 56, bg);
            ui_text_c(F_TITLE_S, x + sw / 2, e->y + e->h / 2 + 26, lab, on && i ? 0xff1a1000U : C_L1);
        }
        break;
    }
    case E_TOGGLE:
        if (f)
            ui_round(e->x - 8, e->y - 8, e->w + 16, e->h + 16, 68, C_STAR);
        ui_round(e->x, e->y, e->w, e->h, 60, card);
        ui_text_cut(F_TITLE_S, e->x + 70, e->y + e->h / 2 + 26, e->a, e->w - 420, C_L1);
        ui_round(e->x + e->w - 300, e->y + (e->h - 130) / 2, 230, 130, 65, e->value ? C_GREEN : C_CARD3);
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
            ui_round(e->x - 10, e->y - 10, e->w + 20, e->h + 20, 82, C_STAR);
        ui_round(e->x, e->y, e->w, e->h, 72, card);
        ui_disc(e->x + 150, e->y + 160, 100, ui_mix(e->color, C_CARD, 60));
        if (e->icon)
            ui_icon(e->icon, e->x + 75, e->y + 85, e->color);
        ui_text_cut(F_TITLE, e->x + 70, e->y + e->h - 110, e->a, e->w - 120, C_L1);
        if (e->b[0])
            ui_text(F_DETAIL, e->x + 72, e->y + e->h - 40, e->b, C_ORANGE);
        break;
    }
    case E_ROUND: {
        int cx = e->x + e->w / 2, cy = e->y + 180;
        if (f)
            ui_ring(cx, cy, 168, 10, C_STAR);
        ui_disc(cx, cy, 150, p ? C_CARD3 : C_CARD);
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
        if (screen == SCR_LIGHT && e->b[0])
            ui_text_c(F_ROW, UI_W / 2, UI_H - 200, e->b, ui_mix(0xff000000U, e->color, 90));
        if (screen == SCR_SOS) {
            uint32_t tc = (e->color & 0xffffffU) == 0xffffffU ? C_RED : 0xffffffffU;
            ui_text_c(ui_has_font(F_VALUE) ? F_VALUE : F_BIG, UI_W / 2, 1100, "SOS", tc);
            ui_text_c(F_TITLE, UI_W / 2, 1350, e->b, tc);
        }
        break;
    }
    case E_TOAST:
        ui_round(e->x, e->y, e->w, e->h, 75, C_CARD3);
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
    if (y < UI_STATUS_H && screen != SCR_LIGHT && screen != SCR_SOS)
        y = UI_STATUS_H, h -= UI_STATUS_H - (e->y - m);
    if (h > 0)
        ui_fill(x, y, w, h, C_BG);
}

static int overlaps(const struct el *a, const struct el *b)
{
    int m = 16;
    return a->x - m < b->x + b->w + m && b->x - m < a->x + a->w + m && a->y - m < b->y + b->h + m &&
           b->y - m < a->y + a->h + m;
}

void ui_kdraw(void)
{
    if (screen == SCR_OFF)
        return;
    if (!dirty && pressed < 0)
        return;
    dirty = 0;
    build();
    int fullscreen = screen == SCR_LIGHT || screen == SCR_SOS;
    if (full || screen != shown_screen) {
        ui_noclip();
        ui_fill(0, 0, UI_W, UI_H, C_BG);
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
    uint64_t px = ui_present();
    if (px && !K.first_frame_ms)
        K.first_frame_ms = sb_ms() ? sb_ms() : 1;
    if (pressed >= 0 && !kt_pending(KT_UI, 1))
        kt_set(KT_UI, 1, sb_ms(), 40, 0);   /* animate the hold (not re-armed while pending: it would never fire) */
}

/* ---- events --------------------------------------------------------------------------------------- */

static int hit(int x, int y)
{
    for (int i = ncur - 1; i >= 0; --i) {
        const struct el *e = &cur[i];
        if (e->interactive && x >= e->x && x < e->x + e->w && y >= e->y && y < e->y + e->h)
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

static void activate(int i, int x)
{
    const struct el *e = &cur[i];
    switch (screen) {
    case SCR_LOCK:
        lock_pick((int)e->id);
        break;
    case SCR_HOME:
        kui_open_app((int)e->id);
        break;
    case SCR_POWER:
        power_menu_pick((int)e->id);
        break;
    case SCR_CONFIRM: {
        void (*d)(int) = cf_done;
        cf_done = 0;
        screen = cf_return;
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
            kui_home();
        } else if (e->kind == E_BUTTON) {
            app_event_send(app_front, EV_APP_BUTTON, e->id, 0);
        } else if (e->kind == E_HOLD) {
            app_event_send(app_front, EV_APP_HOLD, e->id, 0);
        } else if (e->kind == E_TOGGLE) {
            app_event_send(app_front, EV_APP_TOGGLE, e->id, !e->value);
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
    case SCR_APP:
        if (app_front >= 0)
            app_event_send(app_front, EV_APP_BACK, 0, 0);
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
    default:
        break;
    }
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
            ui_noclip();
            ui_fill(0, 0, UI_W, UI_H, 0xff000000U);   /* v0.1: a black screen (OLED: those pixels are off) */
            ui_dirty_all();
            ui_present();
            nshown = 0;
        }
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
    full = dirty = 1;
}

static void lock_pick(int id)
{
    if (id == 0)
        light_on(SBOS_COLOR_WHITE, SCR_LOCK, 10);
    else if (id == 1)
        sos_set(1);
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

void ui_kevent(const void *evp)
{
    const struct kin *ev = evp;
    if (screen == SCR_OFF)
        return;
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
        t_press = sb_ms();
        press_done = 0;
        dirty = 1;
        break;
    case KIN_MOVE:
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
        if (pressed >= 0 && !press_done) {
            int i = pressed;
            const struct el *e = &cur[i];
            pressed = -1;
            if (e->kind == E_HOLD || e->kind == E_ROUND)
                kui_toast(e->kind == E_ROUND ? "Hold the button" : "Keep holding until the bar is full", 1500);
            else if (ev->x >= e->x && ev->x < e->x + e->w && ev->y >= e->y && ev->y < e->y + e->h)
                activate(i, ev->x);
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
                activate(i, -1);
            } else {
                key_hold_armed = i;
                key_hold_until = sb_ms() + 3000;
                kt_set(KT_UI, 5, sb_ms(), 3100, 0);
            }
        } else {
            activate(i, -1);
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
            activate(i, -1);
        }
        dirty = 1;
    } else if (id == 4 && screen == SCR_LIGHT) {
        kui_light(0);
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
