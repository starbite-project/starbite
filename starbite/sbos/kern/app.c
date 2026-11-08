/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS apps (docs/STARBITE-OS.md 2.3, 5, 6.1): loading the built-in .sba files into their slots, the
 * per-app event queues, running handler frames at EL0 with the 500 ms budget, and the system calls.
 *
 * On the phone an app's handler is a frame (kapp.h) that hw_app_resume() runs until it leaves EL0; every exit
 * comes back here, so the loop never waits for an app. On the Mac (SB_HOST) the same apps are linked into the
 * preview and a handler is a plain call; the dispatcher is the same. */
#include "sb.h"
#include "kern.h"
#include "kapp.h"

struct kapp apps[APP_MAX];
int napps;
static int cur_app = -1;                    /* whose handler runs (the host's direct calls) */

#define BUDGET_MS 500U
#define SLOT_BASE 0xa8000000ULL
#define SLOT_COUNT 28U

/* the boot self test's apps ("test-..."): not on home, no toasts, started only in interrupt mode */
int app_hidden(int a)
{
    return a >= 0 && a < napps && sb_prefix(apps[a].id, "test-");
}

int app_by_id(const char *id)
{
    for (int a = 0; a < napps; ++a)
        if (sb_streq(apps[a].id, id))
            return a;
    return -1;
}

const char *app_name(int a)
{
    return a >= 0 && a < napps ? apps[a].name : "";
}

const char *app_tile(int a)
{
    return a >= 0 && a < napps ? apps[a].tile : "";
}

uint32_t app_color(int a)
{
    return a >= 0 && a < napps ? apps[a].color : 0;
}

uint32_t app_icon(int a)
{
    return a >= 0 && a < napps ? apps[a].icon : 0;
}

/* State-like events (a timer firing, a service's state, the power state): one of a kind is enough in the
 * queue; a newer one updates it in place. A busy service can therefore never fill an app's queue (0.2 on the
 * phone: hundreds of "event queue full: heat"). Input-driven events (buttons, levels, toggles, holds, back,
 * show / hide) are always queued; the queue keeps room for them: state events take at most half of it. */
static int coalesced(uint32_t type)
{
    return type == EV_APP_TIMER || type == EV_APP_SERVICE || type == EV_APP_POWER;
}

void app_event_send(int a, uint32_t type, uint32_t id, int32_t value)
{
    if (a < 0 || a >= napps || apps[a].dead)
        return;
    struct kapp *p = &apps[a];
    unsigned queued = (p->qh + APP_QLEN - p->qt) % APP_QLEN;
    if (coalesced(type)) {
        for (unsigned i = p->qt; i != p->qh; i = (i + 1) % APP_QLEN)
            if (p->q[i].type == type && p->q[i].id == id) {
                p->q[i].value = value;
                p->q[i].uptime_ms = sb_ms();
                return;
            }
        if (queued >= APP_QLEN / 2) {
            ++p->dropped;
            return;
        }
    }
    unsigned n = (p->qh + 1) % APP_QLEN;
    if (n == p->qt) {
        ++p->dropped;
        uint64_t now = sb_ms();
        if (now - p->drop_logged >= 10000U || !p->drop_logged) {
            p->drop_logged = now ? now : 1;
            sb_logf("sbos: event queue full: ", p->id);
        }
        return;
    }
    struct sbos_event *e = &p->q[p->qh];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->id = id;
    e->value = value;
    e->uptime_ms = sb_ms();
    p->qh = n;
}

static void __attribute__((unused)) app_kill(int a, const char *why)
{
    struct kapp *p = &apps[a];
    p->dead = 1;
    p->running = 0;
    p->qh = p->qt = 0;
    kt_cancel_owner(KT_APP0 + (unsigned)a);
    struct ub u;
    ub_init(&u);
    ub_s(&u, p->name);
    ub_s(&u, why);
    sb_log(u.b);
    if (app_front == a)
        kui_home();
    if (!app_hidden(a))
        kui_toast(u.b, 3000);
}

/* ---- user memory --------------------------------------------------------------------------------- */

int user_buf(int a, uint64_t p, uint64_t n)
{
#ifdef SB_HOST
    (void)a;
    return p != 0 || n == 0;
#else
    if (a < 0 || a >= napps)
        return 0;
    uint64_t s = apps[a].slot, e = s + APP_SLOT_SIZE;
    return p >= s && p <= e && n <= e - p;
#endif
}

int user_str(int a, uint64_t p, char *out, unsigned cap)
{
    out[0] = 0;
    if (!p)
        return 0;
#ifndef SB_HOST
    if (a < 0 || a >= napps)
        return 0;
    uint64_t e = apps[a].slot + APP_SLOT_SIZE;
    if (p < apps[a].slot || p >= e)
        return 0;
#else
    (void)a;
    uint64_t e = UINT64_MAX;
#endif
    const char *s = (const char *)(uintptr_t)p;
    unsigned i = 0;
    for (; p + i < e && s[i]; ++i)
        if (i + 1 < cap)
            out[i] = s[i];
    if (p + i >= e) {                       /* not terminated inside the slot */
        out[0] = 0;
        return 0;
    }
    out[i + 1 < cap ? i : cap - 1] = 0;
    return 1;
}

int user_out(int a, uint64_t p, const void *src, uint64_t n)
{
    if (!user_buf(a, p, n))
        return 0;
    memcpy((void *)(uintptr_t)p, src, n);
    return 1;
}

int user_in(int a, uint64_t p, void *dst, uint64_t n)
{
    if (!user_buf(a, p, n))
        return 0;
    memcpy(dst, (const void *)(uintptr_t)p, n);
    return 1;
}

/* ---- loading -------------------------------------------------------------------------------------- */

static void app_add(const struct sbapp_header *h, uintptr_t slot, void (*host_entry)(const struct sbos_event *))
{
    if (napps >= APP_MAX)
        return;
    struct kapp *p = &apps[napps];
    memset(p, 0, sizeof(*p));
    p->slot = slot;
    p->hdr = h;
    p->host_entry = host_entry;
    sb_strcpy(p->id, sizeof(p->id), h->id);
    sb_strcpy(p->name, sizeof(p->name), h->name);
    p->perms = h->permissions;
    p->color = h->color;
    p->icon = h->icon;
    p->entry = h->entry;
    if (app_by_id(p->id) >= 0) {            /* built-in by origin: a second app with the same id is refused */
        sb_logf("sbos: app id twice, refused: ", p->id);
        return;
    }
    uint64_t stack = h->stack_size ? h->stack_size : (64U << 10);
    p->heap = slot ? ((slot + h->file_size + h->bss_size + 15U) & ~15ULL) : 0;
    p->heap_end = slot ? slot + APP_SLOT_SIZE - APP_MAILBOX - stack : 0;
    if (p->heap > p->heap_end)
        p->heap = p->heap_end;
    ++napps;
    settings_app_restore(napps - 1);        /* its saved state (the slot), before EV_APP_START reads it */
    if (!app_hidden(napps - 1) || K.irq_mode)
        app_event_send(napps - 1, EV_APP_START, 0, 0);
}

#ifdef SB_HOST
/* the host preview links the apps in (sbos-host-stubs.c fills this table) */
struct host_app {
    const struct sbapp_header *hdr;
    void (*entry)(const struct sbos_event *);
};
extern const struct host_app sbos_host_apps[];
extern const int sbos_host_napps;

void apps_load_builtin(void)
{
    for (int i = 0; i < sbos_host_napps; ++i)
        app_add(sbos_host_apps[i].hdr, 0, sbos_host_apps[i].entry);
}
#else
extern const uint8_t sbos_apps_start[], sbos_apps_end[];

/* the pack (mk-sba.py pack): "SBAPPACK", u32 count, u32 0, then count x { u32 offset, u32 size } */
void apps_load_builtin(void)
{
    const uint8_t *p = sbos_apps_start;
    uint64_t total = (uint64_t)(sbos_apps_end - sbos_apps_start);
    if (total < 16 || memcmp(p, "SBAPPACK", 8)) {
        sb_log("sbos: no app pack");
        return;
    }
    uint32_t n = *(const uint32_t *)(p + 8);
    for (uint32_t i = 0; i < n && i < APP_MAX && i < SLOT_COUNT; ++i) {
        uint32_t off = *(const uint32_t *)(p + 16 + 8 * i), size = *(const uint32_t *)(p + 20 + 8 * i);
        if ((uint64_t)off + size > total || (off & 7U)) {
            sb_log("sbos: app pack entry out of range");
            continue;
        }
        const uint8_t *f = p + off;
        const char *bad = app_check(f, size, APP_SLOT_SIZE);
        if (bad) {
            sb_logf("sbos: app refused: ", bad);
            continue;
        }
        uintptr_t slot = (uintptr_t)(SLOT_BASE + (uint64_t)napps * APP_SLOT_SIZE);
        app_place(f, size, slot);
        hw_code_sync(slot, size);
        app_add((const struct sbapp_header *)slot, slot, 0);
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "sbos: apps loaded: ");
    ub_dec(&u, (uint64_t)napps);
    sb_log(u.b);
}
#endif

/* ---- running handlers ------------------------------------------------------------------------------ */

static int pop_event(struct kapp *p, struct sbos_event *e)
{
    if (p->qt == p->qh)
        return 0;
    *e = p->q[p->qt];
    p->qt = (p->qt + 1) % APP_QLEN;
    return 1;
}

#ifdef SB_HOST
void apps_service(void)
{
    for (int round = 0; round < 64; ++round) {
        int any = 0;
        for (int a = 0; a < napps; ++a) {
            struct kapp *p = &apps[a];
            struct sbos_event e;
            if (p->dead || !pop_event(p, &e))
                continue;
            any = 1;
            cur_app = a;
            p->running = 1;
            p->host_entry(&e);
            p->running = 0;
            cur_app = -1;
        }
        if (!any)
            break;
    }
}

int64_t sbos_call(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    uint64_t arg[31];
    memset(arg, 0, sizeof(arg));
    arg[0] = a;
    arg[1] = b;
    arg[2] = c;
    arg[3] = d;
    arg[4] = e;
    if (cur_app < 0)
        return SBOS_EPERM;
    return sys_dispatch(cur_app, nr, arg);
}
#else
static void frame_start(struct kapp *p, const struct sbos_event *e)
{
    uintptr_t mbox = p->slot + APP_SLOT_SIZE - APP_MAILBOX;
    memcpy((void *)mbox, e, sizeof(*e));
    memset(&p->fr, 0, sizeof(p->fr));
    p->fr.x[0] = mbox;
    p->fr.sp = mbox;                        /* the stack grows down from the mailbox */
    p->fr.elr = p->slot + p->entry;
    p->fr.spsr = K.irq_mode ? 0 : 0x80;     /* EL0t, DAIF clear (I set in polling mode: the GIC is off) */
    hw_tls_clear();
    p->running = 1;
    p->used_ms = 0;
}

static int exit_req = -1;

static void run_frame(int a)
{
    struct kapp *p = &apps[a];
    hw_app_map(p->slot, 1);
    cur_app = a;
    for (;;) {
        uint64_t now = sb_ms();
        uint64_t left = p->used_ms < BUDGET_MS ? BUDGET_MS - p->used_ms : 0;
        uint64_t until = kt_next();
        if (until > now + left)
            until = now + left;
        hw_arm_until(until);
        uint64_t t0 = sb_ms();
        uint64_t why = hw_app_resume(&p->fr);
        p->used_ms += sb_ms() - t0;
        if (why == 1) {
            unsigned ec = (unsigned)(p->fr.esr >> 26) & 0x3fU;
            if (ec != 0x15) {
                struct ub u;
                ub_init(&u);
                ub_s(&u, ": stopped (a fault, ESR ");
                ub_hex(&u, p->fr.esr, 8);
                ub_s(&u, ")");
                app_kill(a, u.b);
                break;
            }
            uint64_t nr = p->fr.x[8];
            if (nr == SYS_RETURN) {
                p->running = 0;
                break;
            }
            p->fr.x[0] = (uint64_t)sys_dispatch(a, nr, p->fr.x);
            if (exit_req == a) {
                exit_req = -1;
                p->running = 0;
                if (app_front == a)
                    kui_home();
                break;
            }
            if (p->used_ms >= BUDGET_MS) {
                app_kill(a, " stopped responding and was closed");
                break;
            }
            if (kt_next() <= sb_ms())
                break;                      /* kernel work is due: resume the frame next turn */
            continue;
        }
        if (why == 2) {                     /* an interrupt: the loop acknowledges it */
            if (p->used_ms >= BUDGET_MS)
                app_kill(a, " stopped responding and was closed");
            break;
        }
        app_kill(a, why == 4 ? ": stopped (SError)" : ": stopped (an exception)");
        break;
    }
    cur_app = -1;
    hw_app_map(p->slot, 0);
}

void apps_service(void)
{
    if (!K.irq_mode && napps) {
        /* polling mode: nothing can interrupt an app; built-in apps only (they are), no budget */
    }
    for (int round = 0; round < 32; ++round) {
        int a = -1;
        for (int i = 0; i < napps && a < 0; ++i)
            if (!apps[i].dead && apps[i].running)
                a = i;
        if (a < 0 && app_front >= 0 && !apps[app_front].dead && apps[app_front].qt != apps[app_front].qh)
            a = app_front;
        for (int i = 0; i < napps && a < 0; ++i)
            if (!apps[i].dead && apps[i].qt != apps[i].qh)
                a = i;
        if (a < 0)
            return;
        struct kapp *p = &apps[a];
        if (!p->running) {
            struct sbos_event e;
            if (!pop_event(p, &e))
                return;
            frame_start(p, &e);
        }
        run_frame(a);
        if (p->running || kt_next() <= sb_ms())
            return;                         /* the loop's turn */
    }
}
#endif

/* ---- system calls ----------------------------------------------------------------------------------- */

static unsigned timers_of(int a)
{
    unsigned n = 0;
    for (unsigned id = 0; id < 256; ++id)
        n += kt_pending(KT_APP0 + (unsigned)a, id);
    return n;
}

static void gpu_confirm_done(int ok)
{
    gpu_heat_allowed = ok;
    sb_log(ok ? "settings: GPU in High: on (confirmed)" : "settings: GPU in High: not confirmed");
    int s = app_by_id("settings");
    if (s >= 0)
        app_event_send(s, EV_APP_SHOW, 0, 0);
}

/* SYS_SETTING_SET for the app in front: 0 done */
static int64_t setting_set_k(uint64_t id, uint64_t v)
{
    uint64_t arg[2] = { id, v };
    switch (arg[0]) {
    case SET_PANEL_SAVE:
        panel_save_set(arg[1] != 0);
        sb_log(arg[1] ? "settings: panel refresh only on change: on" : "settings: panel refresh only on change: off");
        return 0;
    case SET_PANEL_OFF:
        panel_off_on = arg[1] != 0;
        sb_log(arg[1] ? "settings: panel off with the screen: on" : "settings: panel off with the screen: off");
        return 0;
    case SET_GPU_HEAT:
        if (!arg[1]) {
            gpu_heat_allowed = 0;
            sb_log("settings: GPU in High: off");
            return 0;
        }
        kui_confirm("Let High use the GPU?",
                    "High also runs the graphics processor: more heat, more battery.",
                    "Its voltage is set to 950 mV (as iOS on this phone) and put back when it stops.",
                    "First test: only with someone watching the phone.",
                    "Hold to allow", gpu_confirm_done);
        return 0;
    case SET_AUTO_OFF_MIN:
        if (arg[1] != 0 && arg[1] != 5 && arg[1] != 15 && arg[1] != 60)
            return SBOS_EINVAL;
        auto_off_min = (uint32_t)arg[1];
        sb_logx("settings: switch off after the screen is off, minutes (0 never): ", arg[1]);
        if (!K.screen_on && auto_off_min)
            kt_set(KT_POWER, 2, sb_ms(), auto_off_min * 60000U, 0);
        return 0;
    case SET_BRIGHTNESS:
        if (arg[1] > 4)
            return SBOS_EINVAL;
        return panel_brightness((int)arg[1]) ? 0 : SBOS_EAGAIN;
    case SET_SOS_LED:
        sos_led_on = arg[1] != 0;
        sb_log(arg[1] ? "settings: LED in SOS: on" : "settings: LED in SOS: off");
        return 0;
    case SET_SCREEN_OFF_S:
        if (arg[1] < 10 || arg[1] > 600)
            return SBOS_EINVAL;
        K.screen_off_s = (uint32_t)arg[1];
        power_input();
        return 0;
    default:
        return SBOS_EINVAL;
    }
}

int64_t sys_dispatch(int a, uint64_t nr, const uint64_t *arg)
{
    struct kapp *p = &apps[a];
    char s1[128], s2[128];
    int front = a == app_front;
    switch (nr) {
    case SYS_LOG: {
        uint64_t now = sb_ms();
        if (now - p->log_window >= 1000) {
            p->log_window = now;
            p->log_n = 0;
        }
        if (++p->log_n > 20)
            return SBOS_EBUSY;
        if (!user_str(a, arg[0], s1, sizeof(s1)))
            return SBOS_EINVAL;
        struct ub u;
        ub_init(&u);
        ub_s(&u, "app ");
        ub_s(&u, p->id);
        ub_s(&u, ": ");
        ub_s(&u, s1);
        sb_log(u.b);
        return 0;
    }
    case SYS_VERSION:
        return SBOS_API;
    case SYS_ALLOC: {
        uint64_t n = (arg[0] + 15U) & ~15ULL;
#ifdef SB_HOST
        static uint8_t pool[APP_MAX][256 << 10];
        static uint64_t used[APP_MAX];
        if (n > sizeof(pool[0]) - used[a])
            return 0;
        uint64_t r = (uint64_t)(uintptr_t)&pool[a][used[a]];
        used[a] += n;
        return (int64_t)r;
#else
        if (!n || n > p->heap_end - p->heap)
            return 0;
        uint64_t r = p->heap;
        p->heap += n;
        return (int64_t)r;
#endif
    }
    case SYS_EXIT:
#ifndef SB_HOST
        exit_req = a;
#else
        if (front)
            kui_home();
#endif
        return 0;
    /* ---- UI: the app in front only ---- */
    case SYS_UI_BEGIN:
        if (!front)
            return SBOS_EPERM;
        kui_begin(a);
        return 0;
    case SYS_UI_TITLE:
        if (!front || !user_str(a, arg[0], s1, sizeof(s1)))
            return SBOS_EINVAL;
        kui_title(s1);
        return 0;
    case SYS_UI_BUTTON:
        if (!front || !user_str(a, arg[1], s1, sizeof(s1)))
            return SBOS_EINVAL;
        if (arg[2] && !user_str(a, arg[2], s2, sizeof(s2)))
            return SBOS_EINVAL;
        if (!arg[2])
            s2[0] = 0;
        kui_button((uint32_t)arg[0], s1, s2, (uint32_t)arg[3]);
        return 0;
    case SYS_UI_BIG:
        if (!front || !user_str(a, arg[0], s1, sizeof(s1)) || !user_str(a, arg[1], s2, sizeof(s2)))
            return SBOS_EINVAL;
        kui_big(s1, s2, (uint32_t)arg[2]);
        return 0;
    case SYS_UI_TEXT:
        if (!front || !user_str(a, arg[0], s1, sizeof(s1)))
            return SBOS_EINVAL;
        kui_text(s1, (uint32_t)arg[1]);
        return 0;
    case SYS_UI_LEVEL:
        if (!front || !user_str(a, arg[1], s1, sizeof(s1)))
            return SBOS_EINVAL;
        kui_level((uint32_t)arg[0], s1, (int)arg[2]);
        return 0;
    case SYS_UI_TOGGLE:
        if (!front || !user_str(a, arg[1], s1, sizeof(s1)))
            return SBOS_EINVAL;
        kui_toggle((uint32_t)arg[0], s1, (int)arg[2]);
        return 0;
    case SYS_UI_HOLD:
        if (!front || !user_str(a, arg[1], s1, sizeof(s1)))
            return SBOS_EINVAL;
        kui_hold((uint32_t)arg[0], s1, (uint32_t)arg[2], (uint32_t)arg[3]);
        return 0;
    case SYS_UI_END:
        if (!front)
            return SBOS_EPERM;
        kui_end();
        return 0;
    case SYS_UI_TOAST:
        if (!front || !user_str(a, arg[0], s1, sizeof(s1)))
            return SBOS_EINVAL;
        kui_toast(s1, (uint32_t)arg[1]);
        return 0;
    case SYS_UI_LIGHT:
        if (!front)
            return SBOS_EPERM;
        kui_light((uint32_t)arg[0]);
        return 0;
    case SYS_UI_AWAKE:
        if (!front)
            return SBOS_EPERM;
        kui_awake((int)arg[0]);
        return 0;
    case SYS_UI_TILE:
        if (!user_str(a, arg[0], s1, sizeof(s1)))
            return SBOS_EINVAL;
        unsigned cut = sizeof(p->tile) - 1;
        if (sb_strlen(s1) > cut) {
            while (cut > 0 && ((uint8_t)s1[cut] & 0xc0U) == 0x80U)
                --cut;                          /* not inside a UTF-8 character */
            s1[cut] = 0;
        }
        unsigned k = 0;
        while (p->tile[k] && p->tile[k] == s1[k])
            ++k;
        if (p->tile[k] != s1[k]) {
            sb_strcpy(p->tile, sizeof(p->tile), s1);
            ui_mark_dirty();
        }
        return 0;
    case SYS_UI_INPUT: {
        char init[72];
        if (!front || !arg[0] || !user_str(a, arg[1], s1, sizeof(s1)))
            return SBOS_EINVAL;
        if (arg[4] && !user_str(a, arg[4], init, sizeof(init)))
            return SBOS_EINVAL;
        kui_input((uint32_t)arg[0], s1, (uint32_t)arg[2], (int)arg[3], arg[4] ? init : 0);
        memset(init, 0, sizeof(init));
        return 0;
    }
    case SYS_UI_INPUT_GET: {
        char t[65];
        if (!front || !arg[1])
            return SBOS_EINVAL;
        int n = kui_input_get(t, sizeof(t));
        uint64_t cap = arg[1] > sizeof(t) ? sizeof(t) : arg[1];
        if ((uint64_t)n + 1 > cap)
            n = (int)cap - 1;
        t[n] = 0;
        int ok = user_out(a, arg[0], t, (uint64_t)n + 1);
        memset(t, 0, sizeof(t));
        return ok ? n : SBOS_EINVAL;
    }
    case SYS_UI_GAUGE:
        if (!front || !user_str(a, arg[0], s1, sizeof(s1)) || !user_str(a, arg[1], s2, sizeof(s2)))
            return SBOS_EINVAL;
        kui_gauge(s1, s2, (int)arg[2], (uint32_t)arg[3]);
        return 0;
    /* ---- time ---- */
    case SYS_TIME_NOW: {
        struct sbos_time t;
        int ok = clock_now(&t);
        if (!user_out(a, arg[0], &t, sizeof(t)))
            return SBOS_EINVAL;
        return ok ? 0 : SBOS_EAGAIN;
    }
    case SYS_TIMER_SET: {
        uint32_t ms = (uint32_t)arg[1];
        if (arg[0] > 255)
            return SBOS_EINVAL;
        if (ms < 50)
            ms = 50;
        if (!kt_pending(KT_APP0 + (unsigned)a, (unsigned)arg[0]) && timers_of(a) >= 16)
            return SBOS_EBUSY;
        return kt_set(KT_APP0 + (unsigned)a, (unsigned)arg[0], sb_ms(), ms, (int)arg[2]) ? SBOS_EBUSY : 0;
    }
    case SYS_TIMER_CANCEL:
        kt_cancel(KT_APP0 + (unsigned)a, (unsigned)arg[0]);
        return 0;
    case SYS_UPTIME_MS:
        return (int64_t)sb_ms();
    /* ---- power ---- */
    case SYS_BATTERY: {
        struct sbos_battery b;
        battery_get(&b);
        return user_out(a, arg[0], &b, sizeof(b)) ? 0 : SBOS_EINVAL;
    }
    case SYS_ESTIMATE: {
        struct sbos_estimate e;
        battery_estimate(&e);
        return user_out(a, arg[0], &e, sizeof(e)) ? 0 : SBOS_EINVAL;
    }
    case SYS_SYSINFO: {
        struct sbos_sysinfo si;
        memset(&si, 0, sizeof(si));
        sb_strcpy(si.version, sizeof(si.version), "Starbite OS " SBOS_VERSION);
        si.irq_mode = K.irq_mode;
        si.first_frame_ms = (int32_t)K.first_frame_ms;
        si.a55_mhz = (int32_t)(hw_a55_khz() / 1000U);
        si.m3_mhz = (int32_t)(hw_m3_khz() / 1000U);
        si.apps = napps;
        si.uptime_s = (int32_t)(sb_ms() / 1000U);
        si.screen_off_s = (int32_t)K.screen_off_s;
        si.sleep_off_s = (int32_t)K.sleep_off_s;
        return user_out(a, arg[0], &si, sizeof(si)) ? 0 : SBOS_EINVAL;
    }
    case SYS_POWER_REQUEST:
        if (arg[0] == PWR_SCREEN_OFF) {
            if (!front)
                return SBOS_EPERM;
            power_key_short();
            return 0;
        }
        if (arg[0] == PWR_SET_SCREEN_OFF_S) {
            if (arg[1] < 10 || arg[1] > 600)
                return SBOS_EINVAL;
            K.screen_off_s = (uint32_t)arg[1];
            power_input();
            return 0;
        }
        return SBOS_EINVAL;
    /* ---- light ---- */
    case SYS_TORCH: {
        if (!(p->perms & SBOS_P_LIGHT))
            return SBOS_EPERM;
        int64_t lv = (int64_t)arg[0];
        if (lv < 0)
            return torch_ready() ? torch_on() : SBOS_EAGAIN;
        if (lv > 0 && !front)
            return SBOS_EPERM;                  /* switching on needs the app in front */
        return torch_set(lv > 0);
    }
    case SYS_STROBE:
        return (p->perms & SBOS_P_LIGHT) ? SBOS_ENOSYS : SBOS_EPERM;
    case SYS_SETTING_GET:
        switch (arg[0]) {
        case SET_PANEL_SAVE: return panel_save_on;
        case SET_PANEL_OFF: return panel_off_on;
        case SET_SOS_LED: return sos_led_on;
        case SET_GPU_HEAT: return gpu_heat_allowed;
        case SET_SCREEN_OFF_S: return (int64_t)K.screen_off_s;
        case SET_BRIGHTNESS: return panel_bright_level;
        case SET_SAVED: return settings_persistent();
        case SET_AUTO_OFF_MIN: return (int64_t)auto_off_min;
        default: return SBOS_EINVAL;
        }
    case SYS_SETTING_SET: {
        if (!front)
            return SBOS_EPERM;
        int64_t r = setting_set_k(arg[0], arg[1]);
        if (r == 0 && arg[0] != SET_GPU_HEAT)
            settings_changed();             /* saved in 3 s where there is a state slot (the GPU never is) */
        return r;
    }
    case SYS_FILE_READ: {
        if (!(p->perms & SBOS_P_FILES))
            return SBOS_EPERM;
        uint64_t cap = arg[2] > 65536 ? 65536 : arg[2];
        if (!user_str(a, arg[0], s1, sizeof(s1)) || !user_buf(a, arg[1], cap))
            return SBOS_EINVAL;
        return kfile_read(s1, (void *)(uintptr_t)arg[1], (uint32_t)cap);
    }
    /* ---- heat ---- */
    case SYS_HEAT_REQUEST:
        if (!(p->perms & SBOS_P_HEAT))
            return SBOS_EPERM;
        if (arg[0] && !front)
            return SBOS_EPERM;              /* starting needs the app in front (the confirmation) */
        return heater_request((int)arg[0], (int)arg[1]);
    case SYS_HEAT_STATUS: {
        struct sbos_heat h;
        if (!(p->perms & SBOS_P_HEAT))
            return SBOS_EPERM;
        heater_status(&h);
        return user_out(a, arg[0], &h, sizeof(h)) ? 0 : SBOS_EINVAL;
    }
    /* ---- alarm ---- */
    case SYS_SIREN:
        if (!(p->perms & SBOS_P_ALARM))
            return SBOS_EPERM;
        if (arg[0] && !front)
            return SBOS_EPERM;
        return sos_set((int)arg[0]);
    case SYS_SOS_STATUS: {
        struct sbos_sos s;
        sos_status(&s);
        return user_out(a, arg[0], &s, sizeof(s)) ? 0 : SBOS_EINVAL;
    }
    /* ---- Wi-Fi (0.4) ---- */
    case SYS_NET_STATE: {
        struct sbos_wifi ws;
        if (!(p->perms & SBOS_P_NET))
            return SBOS_EPERM;
        wifi_status(&ws);
        return user_out(a, arg[0], &ws, sizeof(ws)) ? 0 : SBOS_EINVAL;
    }
    case SYS_WIFI_POWER:
        if (!(p->perms & SBOS_P_NET) || !front)
            return SBOS_EPERM;
        return wifi_power(arg[0] != 0);
    case SYS_WIFI_SCAN:
        if (!(p->perms & SBOS_P_NET) || !front)
            return SBOS_EPERM;
        return wifi_scan();
    case SYS_WIFI_LIST: {
        static struct sbos_wifi_net wl[SBOS_WIFI_NETS];
        if (!(p->perms & SBOS_P_NET))
            return SBOS_EPERM;
        int max = arg[1] > SBOS_WIFI_NETS ? SBOS_WIFI_NETS : (int)arg[1];
        int n = wifi_list(wl, max);
        return user_out(a, arg[0], wl, sizeof(wl[0]) * (uint64_t)n) ? n : SBOS_EINVAL;
    }
    case SYS_WIFI_JOIN: {
        if (!(p->perms & SBOS_P_NET) || !front)
            return SBOS_EPERM;
        char pw[72];
        pw[0] = 0;
        if (arg[1] && !user_str(a, arg[1], pw, sizeof(pw)))
            return SBOS_EINVAL;
        int r = wifi_join((int)arg[0], arg[1] ? pw : 0);
        memset(pw, 0, sizeof(pw));
        return r;
    }
    case SYS_SENSOR_READ:
        if (!(p->perms & (SBOS_P_SENSORS | SBOS_P_HEAT)))
            return SBOS_EPERM;
        return heater_sensor((int)arg[0]);
    case SYS_VIBRATE:
    case SYS_ALARM_SET:
    case SYS_ALARM_CANCEL:
        return (p->perms & SBOS_P_ALARM) ? SBOS_ENOSYS : SBOS_EPERM;
    /* ---- state (kept in RAM in v0.1; saved from v0.2) ---- */
    case SYS_STATE_PUT:
        if (arg[1] > STATE_REC_MAX || !user_in(a, arg[0], p->state, arg[1]))
            return SBOS_EINVAL;
        p->state_n = (uint32_t)arg[1];
        settings_changed();                 /* 0.5.3: saved with the settings (3 s later) where the slot exists */
        return 0;
    case SYS_STATE_GET: {
        uint64_t n = p->state_n < arg[1] ? p->state_n : arg[1];
        if (!user_out(a, arg[0], p->state, n))
            return SBOS_EINVAL;
        return (int64_t)n;
    }
    default:
        return SBOS_ENOSYS;
    }
}
