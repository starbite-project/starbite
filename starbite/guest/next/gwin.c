/* SPDX-License-Identifier: GPL-2.0-only
 * gwin: Linux apps in Starbite OS Next (docs/STARBITE-LINUX-APPS.md, milestones L2 + L3). The kernel's side of the
 * shell's sh_linux_* calls (ui/shell.h) on kern/guest.c (GUEST_WL: guest-wl.img / .dtb / -rd.img in the store, the
 * surface 720 x 1344 at 2x) and sbghost's contract v2 (sbshm.h): the catalog (the home's Linux page) and the App
 * Store's packages copied out of the shared region under their seqlocks and pushed to the shell when they change;
 * an app opened by its desktop id (LAUNCH + ACTIVATE once the catalog has it, the viewport after); the active window's
 * rows copied (damage per row) into the shell's window strips while the guest says that window is in the surface;
 * touches, typed text (text-input-v3) and keys to the guest. Built into the Next images only (sbos/build.sh).
 *   gwin_now()            kern_next.c's next_kdraw, every loop turn: 1 when the screen changed (draw a frame)
 *   gwin_selftest_step()  uitest.c's SBOS_SELFTEST_NEXT_LINUX variant, every 100 ms: 0 running, 1 passed, -1 failed
 * An L2 guest (no catalog: its sbwl starts foot itself) still works for the Terminal: when no catalog has come 2 s
 * after HELLO, the surface is taken as foot's.
 * Contract v3 (Track E, "apps work fully"; shell.h's v3 hooks, INTEGRATION-NEXT.md "v3"): a card swiped away closes its
 * app (APP_CLOSE), the key bar's keys with KEYMOD around them, the cards keep Linux up (guest_apps), the guest's
 * notifications and clipboard to the shell (v3_poll) and Starbite's clipboard to the guest, and Track A's zero-copy
 * window (sh_linux_front: the front buffer, sbg_surf_reading / sbg_surf_done).
 */
#include "sb.h"
#include "kern.h"
#include "guest.h"
#include "shell.h"
#include "../host/sbghost.h"
#include "../../appstore/sbshm-store3.h"           /* Track F: store3 */

#define SAME(a, b) ((int)(a) == (int)(b))          /* (two enums: gcc's -Wenum-compare) */
_Static_assert(SAME(SH_LAPP_RUNNING, SBSHM_APP_RUNNING) && SAME(SH_LAPP_KEYBOARD, SBSHM_APP_KEYBOARD) &&
               SAME(SH_LAPP_SYSTEM, SBSHM_APP_SYSTEM) && SAME(SH_LAPP_TERMINAL, SBSHM_APP_TERMINAL), "catalog flags");
_Static_assert(SAME(SH_PKG_INSTALLED, SBSHM_PKG_INSTALLED) && SAME(SH_PKG_NO_SPACE, SBSHM_PKG_NO_SPACE) &&
               SAME(SH_PKG_FEATURED, SBSHM_PKG_FEATURED) && SAME(SH_PKG_TOO_BIG, SBSHM_PKG_TOO_BIG), "package flags");
_Static_assert(SAME(SH_PKG_FAILED, SBG_PKG_FAILED) && SAME(SH_PKG_OP_CANCEL, SBG_PKG_OP_CANCEL), "package states");
_Static_assert(SH_LINUX_ICON == SBSHM_ICON_PX && SH_LPKGS == SBSHM_PKGS_MAX && SH_LAPPS <= SBSHM_APPS_MAX, "sizes");
_Static_assert(SH_LINUX_W == GUEST_WL_W && SH_LINUX_H == GUEST_WL_H, "the surface");
_Static_assert(SAME(SH_PKG_UPDATE, SBSHM_PKG_UPDATE) && SAME(SH_PKG_NEEDS_NET, SBSHM_PKG_NEEDS_NET) && SAME(SH_PKG_OP_CHECK, SBG_PKG_OP_CHECK) &&
               SAME(SH_M_SOURCE, SBG_PKG_M_SOURCE) && SAME(SH_PKG_BUILDING, SBG_PKG_BUILDING) &&
               SAME(SH_NET_NO_ANSWER, SBSHM_NET_NO_ANSWER) && SAME(SH_NET_OFFLINE, SBSHM_NET_OFFLINE_MODE), "Track F: store3");
_Static_assert(SAME(SH_LMOD_CTRL, SBSHM_MOD_CTRL) && SAME(SH_LMOD_ALT, SBSHM_MOD_ALT), "key modifiers");

static int last_state = -1;
static unsigned frames;                    /* the window's frames (rows copied) since the start */
static uint32_t *const *win_rows;          /* the shell's window rows at the last copy (the self test reads them) */
static sh_lcat_t cat;                      /* the platform's copies (kept while Linux is off: the home's icons) */
static sh_lstore_t store;
static int cat_have, cat_live, cat_dirty, store_dirty;   /* cat_live: the catalog is this run's */
static uint32_t cat_layout;                /* the catalog's layout_seq (its ids and order) at the last whole copy */
static char pend[40];                      /* an open waiting for the catalog */
static int want = -1;                      /* the catalog index asked into the surface */
static int win_ok;                         /* the surface has want's window (or an L2 guest's foot) */
static int legacy;                         /* an L2 guest: no catalog */
static int vp_rows = SH_LINUX_H, text_on, hold, busy;
static uint64_t t_running;
static int ext_reading;                    /* v3 + Track A: a front buffer handed to the shell's GPU layer */
static const volatile uint32_t *ext_px;    /* that front buffer and its stride (px), for the self test's look */
static unsigned ext_stride;
#define CLIP_BYTES 8192U                   /* the shell's clipboard (shell_linux.inc LX_CLIP) */

static void scopy(char *d, const volatile char *s, unsigned cap)
{
    unsigned k = 0;
    while (k + 1 < cap && s[k]) {
        d[k] = s[k];
        k++;
    }
    d[k] = 0;
}
static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static int term_id(const char *id) { return same(id, SH_LINUX_TERMINAL) || same(id, "foot"); }

static int push_state(void)                /* 2: only the start's words changed */
{
    int s = guest_state();
    static const char *last_stage;
    const char *stage = s == GUEST_STARTING ? guest_stage() : "";
    if (s == last_state && stage == last_stage)
        return 0;
    if (s == last_state) {                 /* the start's words changed (cp7 #1: the disk check, a slow start) */
        last_stage = stage;
        sh_linux_state(s, stage);
        return 2;
    }
    last_stage = stage;
    if (s == GUEST_RUNNING) {
        t_running = sb_ms();
        const char *c = sh_clip_get();     /* v3: what Starbite copied before Linux ran, for Linux's paste */
        if (c && c[0])
            sh_linux_clip(c);
    }
    if (s != GUEST_RUNNING) {
        ext_reading = 0;                   /* (the header is gone with the guest) */
        ext_px = 0;
    }
    if (last_state == GUEST_RUNNING && s != GUEST_RUNNING) {
        /* Linux went away: no app runs any more (the icons stay), nothing is in the surface */
        for (int i = 0; i < cat.n; i++)
            cat.app[i].flags &= ~(unsigned)(SH_LAPP_RUNNING | SH_LAPP_WINDOW);
        if (cat_have)
            sh_linux_catalog(&cat);
        win_ok = legacy = cat_live = text_on = 0;
        want = -1;
    }
    last_state = s;
    sh_linux_state(s, s == GUEST_FAILED ? guest_why() : stage);
    return 1;
}

/* ---- the guest's tables (seqlocked; copied whole or not at all): 1 copied, 0 not written yet, -1 again later ---- */
static int read_cat(void)
{
    const volatile struct sbshm_cat *c = sbg_cat();
    uint32_t s = sbshm_seq_begin(&c->seq);
    if (!s || (s & 1U))
        return s ? -1 : 0;                 /* being written: later; never written: the last run's copy stays */
    static sh_lcat_t t;
    uint32_t n = c->n;
    t.seq = s;
    t.n = (int)(n > SH_LAPPS ? SH_LAPPS : n);
    t.mem_total_kb = c->mem_total_kb;
    t.mem_avail_kb = c->mem_avail_kb;
    uint32_t layout = c->layout_seq;       /* LAUNCH carries this one: seq also moves with rss / flags (~2 s) */
    for (int i = 0; i < t.n; i++) {
        const volatile struct sbshm_app *a = &c->app[i];
        scopy(t.app[i].id, a->id, sizeof t.app[i].id);
        scopy(t.app[i].name, a->name, sizeof t.app[i].name);
        t.app[i].flags = a->flags;
        t.app[i].icon = a->icon < SBSHM_ICONS_MAX ? (int)a->icon : -1;
        t.app[i].pkg = a->pkg < SBSHM_PKGS_MAX ? (int)a->pkg : -1;
        t.app[i].rss_kb = a->rss_kb;
    }
    if (!sbshm_seq_ok(&c->seq, s))
        return -1;
    cat = t;
    cat_layout = layout;
    cat_have = cat_live = 1;
    store.mem_total_kb = cat.mem_total_kb;
    return 1;
}
static int read_store(void)
{
    const volatile struct sbshm_store *st = sbg_store();
    uint32_t s = sbshm_seq_begin(&st->seq);
    if (!s || (s & 1U))
        return s ? -1 : 0;
    static sh_lstore_t t;
    uint32_t n = st->n;
    t.seq = s;
    t.n = (int)(n > SH_LPKGS ? SH_LPKGS : n);
    t.repo_ok = (int)st->repo_ok;
    t.overlay_total_kb = st->overlay_total_kb;
    t.overlay_free_kb = st->overlay_free_kb;
    t.mem_total_kb = cat.mem_total_kb;
    for (int i = 0; i < t.n; i++) {
        const volatile struct sbshm_pkg *p = &st->pkg[i];
        sh_lpkg_t *d = &t.pkg[i];
        scopy(d->pkg, p->pkg, sizeof d->pkg);
        scopy(d->name, p->name, sizeof d->name);
        scopy(d->summary, p->summary, sizeof d->summary);
        scopy(d->category, p->category, sizeof d->category);
        scopy(d->err, p->err, sizeof d->err);
        d->dl_kb = p->dl_kb, d->inst_kb = p->inst_kb, d->ram_mb = p->ram_mb;
        d->flags = p->flags, d->state = p->state, d->pct = p->pct;
        d->icon = p->icon < SBSHM_ICONS_MAX ? (int)p->icon : -1;
    }
    /* Track F: store3: the v3 table beside the store (same region layout, same seqlock), the descriptions in the
     * text area; every copy bounded (the guest writes these) */
    const volatile struct sbshm_store3 *s3 =
        (const volatile struct sbshm_store3 *)((uintptr_t)st - SBSHM_STORE_OFF + SBSHM_STORE3_OFF);
    const volatile char *txt = (const volatile char *)((uintptr_t)st - SBSHM_STORE_OFF + SBSHM_STORE_TEXT_OFF);
    t.v3 = s3->magic == SBSHM_STORE3_MAGIC;
    t.net_ok = t.v3 ? s3->net_ok : 0, t.mirror_ok = t.v3 ? s3->mirror_ok : 0, t.checked_at = t.v3 ? s3->checked_at : 0;
    t.net_why = t.v3 ? s3->net_why : 0, t.busy = t.v3 ? s3->busy : 0;
    for (int i = 0; i < t.n; i++) {
        const volatile struct sbshm_pkg3 *q = &s3->pkg[i];
        sh_lpkg_t *d = &t.pkg[i];
        if (!t.v3) {
            d->methods = d->method = d->def_method = d->phase = d->done_kb = d->total_kb = d->eta_s = 0;
            d->build_min = d->touch = d->needs = d->flags3 = d->rel_size_mb = 0;
            d->ver_inst[0] = d->ver_new[0] = d->ver_rel[0] = d->ver_distro[0] = d->ver_src[0] = 0;
            d->licence[0] = d->source[0] = d->desc[0] = d->hint[0] = 0;
            continue;
        }
        d->methods = q->methods, d->method = q->method, d->def_method = q->def_method, d->phase = q->phase;
        d->done_kb = q->done_kb, d->total_kb = q->total_kb, d->eta_s = q->eta_s, d->build_min = q->build_min;
        d->touch = q->touch, d->needs = q->needs, d->flags3 = q->flags3, d->rel_size_mb = q->rel_size_mb;
        scopy(d->ver_inst, q->ver_inst, sizeof d->ver_inst);
        scopy(d->ver_new, q->ver_new, sizeof d->ver_new);
        scopy(d->ver_rel, q->ver_release, sizeof d->ver_rel);
        scopy(d->ver_distro, q->ver_distro, sizeof d->ver_distro);
        scopy(d->ver_src, q->ver_source, sizeof d->ver_src);
        scopy(d->licence, q->licence, sizeof d->licence);
        scopy(d->source, q->source, sizeof d->source);
        uint32_t o = q->desc_off, l = q->desc_len, k = 0;
        if (o < SBSHM_STORE_TEXT_SIZE && l <= SBSHM_STORE_TEXT_SIZE - o)
            for (; k < l && k < sizeof d->desc - 1 && txt[o + k]; k++)
                d->desc[k] = txt[o + k];
        if (k == sizeof d->desc - 1 && k < l)       /* cut at the cap: not half a UTF-8 character */
            while (k > 0 && (txt[o + k] & 0xc0) == 0x80)
                k--;
        d->desc[k] = 0;
        o = q->hint_off, l = q->hint_len, k = 0;    /* the failure's what-to-do sentence, same rules */
        if (l && o < SBSHM_STORE_TEXT_SIZE && l <= SBSHM_STORE_TEXT_SIZE - o)
            for (; k < l && k < sizeof d->hint - 1 && txt[o + k]; k++)
                d->hint[k] = txt[o + k];
        if (k == sizeof d->hint - 1 && k < l)
            while (k > 0 && (txt[o + k] & 0xc0) == 0x80)
                k--;
        d->hint[k] = 0;
    }
    if (!sbshm_seq_ok(&st->seq, s))
        return -1;
    store = t;
    return 1;
}
/* Linux kept up while the App Store is open or a package operation runs (guest.c's idle stop waits: guest_keep) */
static void busy_update(void)
{
    int b = hold || (store.v3 && store.busy);       /* store3's busy: queued jobs and update checks too */
    for (int i = 0; i < store.n; i++)
        b |= store.pkg[i].state == SBG_PKG_QUEUED || store.pkg[i].state == SBG_PKG_INSTALLING ||
             store.pkg[i].state == SBG_PKG_REMOVING;
    if (b != busy) {
        busy = b;
        guest_keep(b);
    }
}
/* ---- cp6 (Track B's ask, the user's cp3 notes): a store job in words for the shell, the phone awake while one runs, a
 * notification when one ends ---------------------------------------------------------------------------------------- */
static int job_awake;
static struct { char pkg[40]; unsigned state, phase; } JP[SH_LPKGS];   /* the last snapshot's busy packages */
static int jp_n;
static int job_busy(const sh_lpkg_t *p)
{
    return p->state == SH_PKG_QUEUED || p->state == SH_PKG_INSTALLING || p->state == SH_PKG_REMOVING;
}
static void jcat(char *b, int cap, int *n, const char *s)
{
    while (*s && *n < cap - 1)
        b[(*n)++] = *s++;
    b[*n] = 0;
}
static void jnum(char *b, int cap, int *n, unsigned v)
{
    char d[12];
    int k = 0;
    do
        d[k++] = (char)('0' + v % 10);
    while ((v /= 10) && k < 11);
    while (k && *n < cap - 1)
        b[(*n)++] = d[--k];
    b[*n] = 0;
}
static const char *job_step(const sh_lpkg_t *p)
{
    if (p->state == SH_PKG_QUEUED)
        return "Waiting to install";
    if (p->state == SH_PKG_REMOVING)
        return "Removing";
    return p->phase == SH_PKG_DOWNLOADING ? "Downloading" : p->phase == SH_PKG_BUILDING ? "Building" :
           p->phase == SH_PKG_VERIFYING ? "Checking" : "Installing";
}
/* the running job in words ("Building btop from source, 45 %", "+ 2 more" when several): 1, or 0 (b = "") with none.
 * For the shell (the App Store's page, the Linux page, the lock): ask each frame it shows it, it is cheap */
int sh_linux_job(char *b, int cap);
int sh_linux_job(char *b, int cap)
{
    int n = 0, first = -1, more = 0;
    if (!b || cap < 2)
        return 0;
    b[0] = 0;
    for (int i = 0; i < store.n; i++)
        if (job_busy(&store.pkg[i])) {
            if (first < 0)
                first = i;
            else
                more++;
        }
    if (first < 0)
        return 0;
    const sh_lpkg_t *p = &store.pkg[first];
    jcat(b, cap, &n, job_step(p));
    jcat(b, cap, &n, " ");
    jcat(b, cap, &n, p->name[0] ? p->name : p->pkg);
    if (p->state != SH_PKG_REMOVING && p->state != SH_PKG_QUEUED && p->method == SH_M_SOURCE && p->phase == SH_PKG_BUILDING)
        jcat(b, cap, &n, " from source");
    if (p->state != SH_PKG_QUEUED && p->pct > 0 && p->pct < 100) {
        jcat(b, cap, &n, ", ");
        jnum(b, cap, &n, p->pct);
        jcat(b, cap, &n, " %");
    }
    if (more) {
        jcat(b, cap, &n, " + ");
        jnum(b, cap, &n, (unsigned)more);
        jcat(b, cap, &n, " more");
    }
    return 1;
}
/* after each store snapshot: a package that was busy and is not any more ends with a notification (the Centre and a
 * banner: "Neovim Is Installed", "btop Couldn't Be Installed" + the guest's reason); the phone stays awake while any
 * job runs (not merely while the App Store is open) */
/* 8j (Track A; the user, 8i: "installs still all fail", the reasons only in the overlay's /var/log/sbstore): each
 * finished job in the pmsg, with the store's view of the repo, the network, the mirror and the overlay's room */
static void job_log(const sh_lpkg_t *p, const char *what)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "gwin: store: ");
    ub_s(&u, what);
    ub_s(&u, " (");
    ub_s(&u, p->pkg);
    ub_s(&u, ", method ");
    ub_dec(&u, p->method);
    ub_s(&u, "; repo ");
    ub_dec(&u, (uint64_t)(store.repo_ok != 0));
    ub_s(&u, ", net ");
    ub_dec(&u, store.net_ok);
    ub_s(&u, " why ");
    ub_dec(&u, store.net_why);
    ub_s(&u, ", mirror ");
    ub_dec(&u, store.mirror_ok);
    ub_s(&u, ", overlay free MB ");
    ub_dec(&u, store.overlay_free_kb / 1024U);
    ub_s(&u, ")");
    if (p->state == SH_PKG_FAILED) {
        ub_s(&u, ": ");
        ub_s(&u, p->err[0] ? p->err : "(no reason)");
    }
    sb_log(u.b);
    if (p->state == SH_PKG_FAILED && p->hint[0])
        sb_logf("gwin: store: what to do: ", p->hint);
}
static void jobs_update(void)
{
    int any = 0;
    for (int i = 0; i < store.n; i++) {
        const sh_lpkg_t *p = &store.pkg[i];
        any |= job_busy(p);
        if (job_busy(p))
            continue;
        for (int k = 0; k < jp_n; k++) {
            if (!same(JP[k].pkg, p->pkg))
                continue;
            char t[72];
            int n = 0;
            const char *nm = p->name[0] ? p->name : p->pkg;
            jcat(t, sizeof t, &n, nm);
            if (p->state == SH_PKG_FAILED)
                jcat(t, sizeof t, &n, JP[k].state == SH_PKG_REMOVING ? " Couldn't Be Removed" : " Couldn't Be Installed");
            else
                jcat(t, sizeof t, &n, JP[k].state == SH_PKG_REMOVING ? " Was Removed" :
                                      (p->flags & SH_PKG_INSTALLED) ? " Is Installed" : " Was Not Installed");
            const char *body = p->state == SH_PKG_FAILED ? (p->err[0] ? p->err : "See the App Store for details.") :
                               JP[k].state == SH_PKG_REMOVING ? "" : (p->flags & SH_PKG_INSTALLED) ? "Open it from the Linux page." : "";
            sh_notify("store", t, body, -1);
            job_log(p, t);
            JP[k].pkg[0] = 0;               /* said once */
        }
    }
    jp_n = 0;
    for (int i = 0; i < store.n && jp_n < SH_LPKGS; i++)
        if (job_busy(&store.pkg[i])) {
            scopy(JP[jp_n].pkg, store.pkg[i].pkg, sizeof JP[jp_n].pkg);
            JP[jp_n].state = store.pkg[i].state;
            JP[jp_n].phase = store.pkg[i].phase;
            jp_n++;
        }
    if (any != job_awake) {
        job_awake = any;
        (void)sh_hold_awake("linux-store", any);   /* Track A (shell.h): no Auto-Lock while a job runs */
    }
}

static int cat_find(const char *id)
{
    for (int i = 0; i < cat.n; i++)
        if (same(cat.app[i].id, id))
            return i;
    if (term_id(id))
        for (int i = 0; i < cat.n; i++)
            if (term_id(cat.app[i].id))
                return i;
    return -1;
}
/* the open waiting for the catalog: LAUNCH (started, or raised), ACTIVATE (into the surface), the viewport */
static void try_launch(void)
{
    if (!pend[0] || guest_state() != GUEST_RUNNING)
        return;
    int ci = cat_live ? cat_find(pend) : -1;   /* indices of this run's catalog only */
    if (ci < 0) {
        /* an L2 guest publishes no catalog; its sbwl shows foot by itself */
        if (term_id(pend) && !cat_live && sb_ms() - t_running > 2000) {
            sb_log("gwin: no catalog 2 s after HELLO: an L2 guest, the surface is foot's");
            legacy = win_ok = 1;
            pend[0] = 0;
            guest_full_next();
        }
        return;
    }
    if (!sbg_launch((uint32_t)ci, cat_layout) || !sbg_activate((uint32_t)ci))
        return;                            /* the ring full: again next turn */
    sbg_viewport((uint32_t)vp_rows, SH_LINUX_W);
    sb_logf("gwin: launch + activate ", pend);
    want = ci;
    pend[0] = 0;
    int has = 0;
    win_ok = sbg_window(&has) == (uint32_t)ci && has;
    if (win_ok)
        guest_full_next();
}

/* ---- the platform (shell.h) ---- */
int sh_linux_open(const char *id)
{
    int s = guest_state();
    char want_id[40];
    scopy(want_id, id, sizeof want_id);
    if (s == GUEST_RUNNING || s == GUEST_STARTING) {
        if (guest_mode() != GUEST_WL) {    /* another guest (a self test of the old UI): not this window's */
            sh_linux_state(GUEST_FAILED, "Linux is busy with another guest.");
            return -1;
        }
    } else if (s == GUEST_STOPPING) {
        push_state();                      /* "Linux Is Stopping": the next open starts it again */
        return -1;
    } else {
        int r = guest_start_mode(GUEST_WL);
        if (r < 0) {
            last_state = GUEST_FAILED;
            sh_linux_state(GUEST_FAILED, guest_why());
            sb_logf("gwin: the start was refused: ", guest_why());
            return r;
        }
        frames = 0;
        sb_log("gwin: Linux started for an app (the M3 cores)");
    }
    scopy(pend, want_id, sizeof pend);
    want = -1;
    win_ok = text_on = 0;
    if (legacy && term_id(pend)) {         /* the L2 guest's foot again */
        win_ok = 1;
        pend[0] = 0;
    }
    guest_show(1);                         /* MSG_FOCUS 1; the guest's idle stop cancelled */
    if (sh_pref_int("linux.used", 0) != 1) (void)sh_pref_set_int("linux.used", 1);   /* Track A: prewarm only for Linux users */
    guest_full_next();
    push_state();
    try_launch();
    return 0;
}

static void ext_done(void)
{
    if (ext_reading && guest_state() == GUEST_RUNNING)
        sbg_surf_done();                   /* the GPU reads none of the guest's buffers now */
    ext_reading = 0;
}

void sh_linux_hide(void)
{
    ext_done();
    if (want >= 0 && guest_state() == GUEST_RUNNING)
        sbg_activate(SBSHM_NONE);          /* home: no window in the surface; the app keeps running */
    guest_show(0);                         /* MSG_FOCUS 0; guest.c stops Linux after 60 s hidden and idle */
    want = -1;
    win_ok = text_on = 0;
    pend[0] = 0;
    win_rows = 0;
}

int sh_linux_blit(uint32_t *const *rows, int first, int *y0, int *y1)
{
    *y0 = *y1 = -1;
    if (!win_ok)
        return 0;                          /* not the asked app's window yet: the shell keeps "Opening" */
    if (first)
        guest_full_next();
    if (!first && !guest_frame_new())
        return 0;
    ext_done();                            /* (the strips again after the zero-copy layer) */
    int n = guest_blit_rows(rows, y0, y1); /* 2x; alpha forced opaque */
    if (n > 0) {
        frames++;
        win_rows = rows;
    }
    return n;
}

int sh_linux_touch(int phase, int x, int y)
{
    if (!guest_shown() || !win_ok)
        return 0;
    uint32_t p = phase == 0 ? SBG_TOUCH_DOWN : phase == 1 ? SBG_TOUCH_MOVE : SBG_TOUCH_UP;
    return sbg_touch(0, p, x, y);
}

int sh_linux_char(char c)
{
    /* a KEYBOARD app (foot: it binds text-input-v3 too, TEXT_FOCUS with purpose terminal) always gets keys */
    unsigned fl = want >= 0 && want < cat.n ? cat.app[want].flags : SH_LAPP_KEYBOARD;
    if (c != '\n' && c != '\b' && text_on && !(fl & SH_LAPP_KEYBOARD) && guest_shown()) {
        char s[2] = {c, 0};
        return sbg_text(s);                /* text-input-v3: commit_string into the focused field */
    }
    return guest_type(c);                  /* the Linux key, with Shift around it ('\n' Return, '\b' BackSpace) */
}

int sh_linux_viewport(int rows)
{
    vp_rows = rows < 1 ? SH_LINUX_H : rows > SH_LINUX_H ? SH_LINUX_H : rows;
    if (guest_state() == GUEST_RUNNING && want >= 0)
        return sbg_viewport((uint32_t)vp_rows, SH_LINUX_W);
    return 1;                              /* sent with the next ACTIVATE */
}

/* cp7 landscape: the shell's turn for the Linux window (Track A's sh_orientation, while a Linux app is shown).
 * Sent once the guest runs (again to each new guest); the shell turns its own chrome when sh_linux_rot_shown()
 * reaches q (the guest's frame is turned then). A guest without the capability stays portrait (0 forever). */
static int rot_q, rot_rows, rot_dirty, rot_said;
int sh_linux_rotate(int q, int rows)
{
    q = q == 1 || q == 3 ? q : 0;
    rows = !q ? 0 : rows < 1 || rows > SH_LINUX_W ? SH_LINUX_W : rows;
    if (q == rot_q && rows == rot_rows)
        return 1;
    rot_q = q, rot_rows = rows, rot_dirty = 1;
    return 1;
}
int sh_linux_rot_shown(void) { return guest_state() == GUEST_RUNNING ? (int)sbg_rot_shown() : 0; }
/* Track A's sh_orientation while a Linux window is in front (every 200 ms: it keeps the accelerometer alive): the
 * phone's top to the left (SH_ORI_TURNED_LEFT) is turn 1, to the right turn 3; upside down stays portrait. With the
 * keyboard up the window stays portrait (the shell's keyboard is drawn portrait; a landscape keyboard would pass its
 * rows to sh_linux_rotate). Hidden: portrait again (the next open reads the sensor again). */
static void ori_tick(void)
{
    static uint64_t t;
    uint64_t now = sb_ms();
    if (!guest_shown() || want < 0) {
        if (rot_q)
            sh_linux_rotate(0, 0);
        return;
    }
    if (now - t < 200)
        return;
    t = now;
    int o = sh_orientation();
    int q = o == SH_ORI_TURNED_LEFT ? 1 : o == SH_ORI_TURNED_RIGHT ? 3 : 0;
    if (vp_rows < SH_LINUX_H)
        q = 0;
    sh_linux_rotate(q, 0);
}
static void rot_tick(void)
{
    if (!rot_dirty || guest_state() != GUEST_RUNNING || (want < 0 && rot_q))
        return;                            /* (portrait goes at once, a turn only with a window in front) */
    int r = sbg_rotate((uint32_t)rot_q, (uint32_t)rot_rows);
    if (r > 0) {
        rot_dirty = 0;
        sb_logf("gwin: rotate ", rot_q == 1 ? "1 (the top to the left)" : rot_q == 3 ? "3 (the top to the right)" : "0");
    } else if (r < 0) {
        rot_dirty = 0;                     /* an older guest: portrait */
        if (!rot_said++)
            sb_log("gwin: this guest cannot turn its window (no SBSHM_CAP_ROTATE): portrait");
    }
}

int sh_linux_icon(int icon, uint32_t *px)
{
    const volatile uint32_t *s = icon >= 0 ? sbg_icon((uint32_t)icon) : 0;
    if (!s)
        return 0;
    /* the catalog's icons under its seq, the store's under the store's: a torn copy is tried again */
    const volatile uint32_t *seq = (uint32_t)icon < SBSHM_ICONS_CAT ? &sbg_cat()->seq : &sbg_store()->seq;
    for (int t = 0; t < 3; t++) {
        uint32_t b = sbshm_seq_begin(seq);
        if (b & 1U)
            continue;
        for (unsigned i = 0; i < SBSHM_ICON_PX * SBSHM_ICON_PX; i++)
            px[i] = s[i];
        if (sbshm_seq_ok(seq, b))
            return 1;
    }
    return 0;
}

int sh_linux_hold(int on)
{
    hold = !!on;
    int r = 0, s = guest_state();
    if (hold && (s == GUEST_OFF || s == GUEST_FAILED)) {
        r = guest_start_mode(GUEST_WL);
        if (r < 0) {
            last_state = GUEST_FAILED;
            sh_linux_state(GUEST_FAILED, guest_why());
        } else {
            frames = 0;
            sb_log("gwin: Linux started for the App Store");
        }
    }
    busy_update();
    push_state();
    return r;
}

int sh_linux_pkg(int idx, int op)
{
    if (guest_state() != GUEST_RUNNING || ((idx < 0 || idx >= store.n) && idx != SH_PKG_ALL))
        return -1;
    /* Track F: store3: op is the op word (op | method << 8), passed on unchanged; SH_PKG_ALL: SBSHM_NONE (CHECK) */
    return sbg_pkg(idx == SH_PKG_ALL ? SBSHM_NONE : (uint32_t)idx, (uint32_t)op, store.seq) ? 0 : -1;
}

/* ---- contract v3 (Track E, "apps work fully"; shell.h's v3 hooks) ---- */
/* a card swiped away: APP_CLOSE for its catalog index (this run's catalog) */
int sh_linux_close(const char *id)
{
    int ci = guest_state() == GUEST_RUNNING && cat_live ? cat_find(id) : -1;
    if (ci < 0)
        return 0;
    sb_logf("gwin: close ", id);
    return sbg_app_close((uint32_t)ci);
}
/* the key bar: KEYMOD (Ctrl / Alt held), the key down and up, KEYMOD 0 */
int sh_linux_key(int code, unsigned mods)
{
    if (!guest_shown() || code <= 0)
        return 0;
    mods &= SBSHM_MOD_CTRL | SBSHM_MOD_ALT;
    if (mods)
        sbg_keymod(mods);
    int r = guest_key((unsigned)code, 1);
    r &= guest_key((unsigned)code, 0);
    if (mods)
        sbg_keymod(0);
    return r;
}
/* a typed character with Ctrl / Alt latched: always keys (Ctrl+C is a key, not text) */
int sh_linux_char_mods(char c, unsigned mods)
{
    if (!guest_shown())
        return 0;
    mods &= SBSHM_MOD_CTRL | SBSHM_MOD_ALT;
    if (mods)
        sbg_keymod(mods);
    int r = guest_type(c);
    if (mods)
        sbg_keymod(0);
    return r;
}
/* Linux apps with a switcher card: guest.c keeps Linux up while hidden (guest_apps) */
void sh_linux_cards(int n) { guest_apps(n); }
int sh_linux_notif_act(unsigned id, int action)
{
    if (guest_state() != GUEST_RUNNING)
        return 0;
    return sbg_notif_act(id, action < 0 ? SBSHM_NONE : (uint32_t)action);
}
int sh_linux_clip(const char *text)
{
    unsigned n = 0;
    while (text[n] && n < CLIP_BYTES)
        n++;
    return guest_state() == GUEST_RUNNING && sbg_clip_set("text/plain;charset=utf-8", (const uint8_t *)text, n);
}
/* Track A's zero-copy path: the front buffer for the shell's external layer, marked as read by the GPU */
const uint32_t *sh_linux_front(int *stride_px)
{
    int idx = 0;
    unsigned stride = 0;
    const volatile uint32_t *px = win_ok && guest_shown() ? guest_front(&idx, &stride) : 0;
    *stride_px = 0;
    if (!px)
        return 0;
    /* mark it read FIRST, then check it is still the front: if the guest flipped in between, its next back could be
     * this very buffer, so take the newer front instead (the guest never renders into the front) */
    for (int tries = 0; tries < 4; tries++) {
        sbg_surf_reading(idx);             /* the guest won't render into it until the next front / the hide */
        int now = 0;
        unsigned st2 = 0;
        const volatile uint32_t *p2 = guest_front(&now, &st2);
        if (!p2)
            break;
        if (now == idx)
            break;
        idx = now;
        px = p2;
        stride = st2;
    }
    ext_reading = 1;
    frames++;
    *stride_px = (int)stride;
    ext_px = px;                           /* (the self test reads the window from here on the zero-copy path) */
    ext_stride = stride;
    return (const uint32_t *)(uintptr_t)px;
}
static int is_text(const char *mime)
{
    static const char t[] = "text/plain";
    for (unsigned i = 0; t[i]; i++)
        if (mime[i] != t[i])
            return 0;
    return 1;
}
/* the guest's clipboard and notifications, after sbg_changes: 1 the screen may change */
static int v3_poll(unsigned ch)
{
    int changed = 0;
    if (ch & SBG_CH_CLIP) {
        static char cb[CLIP_BYTES];
        char mime[40];
        unsigned n = sbg_clip_get(mime, (uint8_t *)cb, CLIP_BYTES - 1);
        if (n && is_text(mime)) {          /* (0: empty, or too big for the shell's: the old one stays) */
            cb[n] = 0;
            sh_linux_clip_in(cb);
        }
    }
    if (ch & SBG_CH_NOTIF) {
        struct sbshm_notif n;
        int k;
        while ((k = sbg_notif_next(&n)) != 0) {
            if (k == 2) {
                sh_linux_notify_close(n.id);
            } else {
                struct sbos_time t;
                int hm = clock_now(&t) ? t.h * 100 + t.mi : -1;
                const char *app = cat_live && n.app < (uint32_t)cat.n ? cat.app[n.app].id : "";
                sh_linux_notify(n.id, app, n.app_name, n.summary, n.body, hm);
            }
            changed = 1;
        }
    }
    return changed;
}

/* every loop turn (next_kdraw), at most every 16 ms: the state, the tables, the window, its frames */
int gwin_now(void)
{
    static uint64_t t;
    uint64_t now = sb_ms();
    if (now - t < 16)
        return 0;
    t = now;
    int was = last_state, changed = 0;
    int words = push_state() == 2;
    changed = was != last_state || words;
    if (was != last_state && last_state != GUEST_RUNNING && rot_q)
        rot_dirty = 1;                     /* the next guest gets the turn again */
    ori_tick();
    rot_tick();
    if (guest_state() == GUEST_RUNNING) {
        unsigned ch = sbg_changes();
        if (ch & SBG_CH_CATALOG)
            cat_dirty = 1;
        if (ch & (SBG_CH_STORE | SBG_CH_PKG))
            store_dirty = 1;
        int r;
        if (cat_dirty && (r = read_cat()) >= 0) {
            cat_dirty = 0;
            if (r) {
                sh_linux_catalog(&cat);
                changed = 1;
            }
        }
        if (store_dirty && (r = read_store()) >= 0) {
            store_dirty = 0;
            if (r) {
                busy_update();
                jobs_update();              /* cp6: a job's end said, the phone kept awake meanwhile */
                sh_linux_store(&store);
                changed = 1;
            }
        }
        if (ch & SBG_CH_WINDOW) {
            int has = 0;
            uint32_t w = sbg_window(&has);
            int ok = want >= 0 && w == (uint32_t)want && has;
            if (ok && !win_ok)
                guest_full_next();         /* the app's window: every row of it */
            if (!legacy)
                win_ok = ok;
        }
        if (ch & SBG_CH_TEXT) {
            int on = sbg_text_focus(0);
            if (on != text_on) {
                text_on = on;
                if (guest_shown()) {
                    sh_linux_textfocus(on);
                    changed = 1;
                }
            }
        }
        try_launch();
        if (v3_poll(ch))
            changed = 1;
    }
    if (win_ok && guest_frame_new() && sh_linux_frame())
        changed = 1;
    return changed;
}

/* ---- the unattended check (starbite-next-linuxtest.img: uitest.c -DSBOS_SELFTEST_NEXT_LINUX) -------------------
 * The Terminal opened through the shell (sh_set "app"), Linux started from the store, foot launched from the catalog
 * (or an L2 guest's foot), its first frame in the window; a command typed through the shell's keyboard path (four
 * characters a step); its kmsg marker in the guest's log and text in the window's bitmap; a tap in the window; the
 * Terminal closed, Linux stopped, the M3 cluster off. Never a storage write (no package operation). The caller
 * restarts into recovery (pmsg) after the result. */
static int gs_phase, gs_typed;
static uint64_t gs_t, gs_t0;
static unsigned gs_frames;

static void gs_log(const char *what, uint64_t v, int with_v)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "selftest: next-linux: ");
    ub_s(&u, what);
    if (with_v)
        ub_dec(&u, v);
    sb_log(u.b);
}
static int gs_failed(const char *why)
{
    gs_log("FAILED: ", 0, 0);
    sb_logf("selftest: next-linux: why: ", why);
    if (guest_state() == GUEST_RUNNING || guest_state() == GUEST_STARTING)
        guest_stop();                      /* the cluster off before the restart (guest_power_off waits too) */
    gs_phase = 99;
    return -1;
}
/* rows of the window above the keyboard with anything other than its first pixel (the terminal's background) */
static unsigned lit_rows(void)
{
    if (!win_rows && ext_px && ext_stride) {   /* 8d: the zero-copy path (no strips): the guest's front buffer, 1x */
        uint32_t bg = ext_px[0] & 0xffffffU;
        unsigned n = 0;
        for (int y = 0; y < SH_LINUX_HKB; y += 2) {
            const volatile uint32_t *r = ext_px + (size_t)y * ext_stride;
            for (int x = 0; x < SH_LINUX_W; x++)
                if ((r[x] & 0xffffffU) != bg) {
                    n++;
                    break;
                }
        }
        return n;
    }
    if (!win_rows)
        return 0;
    uint32_t bg = win_rows[0][0];
    unsigned n = 0;
    for (int y = 0; y < 2 * SH_LINUX_HKB; y += 4) {
        const uint32_t *r = win_rows[y];
        for (int x = 0; x < 2 * SH_LINUX_W; x += 2)
            if (r[x] != bg) {
                n++;
                break;
            }
    }
    return n;
}

int gwin_selftest_step(void)
{
    uint64_t now = sb_ms(), dt = now - gs_t;
    int st = guest_state();
    switch (gs_phase) {
    case 0:
        gs_t0 = now;
        if (!guest_available_mode(GUEST_WL))
            return gs_failed("the Linux apps' files are not in the store (guest-wl.img / .dtb / -rd.img)");
        sh_set("app", "terminal", 1);      /* as a tap on the icon: app_open -> sh_linux_open(foot) */
        if (guest_state() != GUEST_STARTING && guest_state() != GUEST_RUNNING)
            return gs_failed(guest_why()[0] ? guest_why() : "the Terminal did not start Linux");
        gs_log("the Terminal opened; Linux starting (guest-wl from the store)", 0, 0);
        gs_t = now;
        gs_phase = 1;
        return 0;
    case 1:                                /* Linux up, foot launched, its first frame in the window */
        if (st == GUEST_FAILED)
            return gs_failed(guest_why());
        if (st == GUEST_RUNNING && frames > 0 && (win_rows || ext_px)) {   /* (8d: the zero-copy path has no strips) */
            gs_log("Linux running, the window's first frame after ms ", dt, 1);
            gs_log(legacy ? "an L2 guest (no catalog): foot by itself" : "foot from the catalog; apps in it: ",
                   (uint64_t)cat.n, !legacy);
            if (store.n || store.repo_ok)
                gs_log("App Store packages in the index: ", (uint64_t)store.n, 1);
            gs_t = now;
            gs_phase = 2;
        } else if (dt > 40000) {
            return gs_failed(st != GUEST_RUNNING ? "Linux did not come up within 40 s" :
                             !win_ok ? "Linux runs but foot's window never came into the surface" :
                                       "Linux runs but the window got no frame within 40 s");
        }
        return 0;
    case 2:                                /* the shell's prompt first */
        if (st != GUEST_RUNNING)
            return gs_failed("Linux stopped by itself");
        if (dt >= 2000) {
            gs_typed = 0;
            gs_phase = 3;
        }
        return 0;
    case 3: {                              /* the command through the keyboard's path, four characters a step */
        static const char cmd[] = "uname -a; echo STARBITE-TERM-OK > /dev/kmsg\n";
        for (int i = 0; i < 4 && cmd[gs_typed]; ++i, ++gs_typed)
            if (!sh_linux_char(cmd[gs_typed]))
                return gs_failed("a character could not be typed (no key, or the ring full)");
        if (!cmd[gs_typed]) {
            gs_log("typed: uname -a; echo STARBITE-TERM-OK > /dev/kmsg", 0, 0);
            gs_t = now;
            gs_frames = frames;
            gs_phase = 4;
        }
        return 0;
    }
    case 4:                                /* the marker in the guest's log, the window redrawn, text in it */
        if (st != GUEST_RUNNING)
            return gs_failed("Linux stopped by itself");
        /* the window redrew since the typing and has nothing pending (a quiet terminal publishes only on change),
         * or kept redrawing a guest that keeps publishing */
        if (guest_term_ok() && frames != gs_frames && dt >= 500 && (!guest_frame_new() || frames - gs_frames >= 3)) {
            unsigned lit = lit_rows();
            gs_log("the command ran (STARBITE-TERM-OK in the guest's log) after ms ", dt, 1);
            gs_log("the window's frames since the typing: ", frames - gs_frames, 1);
            gs_log("rows of the window with text (1 in 4): ", lit, 1);
            if (lit < 4)
                return gs_failed("the window shows no text");
            uint32_t sent = sbg_stats()->touches_sent;
            sh_linux_touch(0, SH_LINUX_W / 2, SH_LINUX_HKB / 2);
            sh_linux_touch(2, SH_LINUX_W / 2, SH_LINUX_HKB / 2);
            gs_log("a tap in the window: touch messages sent ", sbg_stats()->touches_sent - sent, 1);
            if (sbg_stats()->touches_sent - sent < 2)
                return gs_failed("the tap did not reach the guest's ring");
            gs_t = now;
            gs_phase = 5;
        } else if (dt > 15000) {
            gs_log("the marker: ", (uint64_t)guest_term_ok(), 1);
            gs_log("window frames since the typing: ", frames - gs_frames, 1);
            return gs_failed(guest_term_ok() ? "the window did not redraw after the command within 15 s" :
                                               "STARBITE-TERM-OK did not reach the guest's log within 15 s");
        }
        return 0;
    case 5:                                /* the tap's events delivered, then the Terminal closed, Linux stopped */
        if (dt < 1000)
            return 0;
        sh_set("app", "", 0);              /* app_finish_close -> sh_linux_hide */
        if (guest_shown())
            return gs_failed("the window is still shown after the Terminal closed");
        guest_stop();
        gs_t = now;
        gs_phase = 6;
        return 0;
    case 6:
        if (st == GUEST_OFF) {
            gs_log("off; M3 cluster powered: ", (uint64_t)hw_m3_on(), 1);
            if (hw_m3_on() || guest_owns_m3())
                return gs_failed("the M3 cluster still powered after the stop");
            gs_log("passed: the Terminal's window with foot, a typed command, a tap, the cluster off; seconds ",
                   (now - gs_t0) / 1000U, 1);
            gs_phase = 98;
            return 1;
        }
        if (st == GUEST_FAILED)
            return gs_failed(guest_why());
        if (dt > 8000)
            return gs_failed("Linux did not power off within 8 s of MSG_STOP");
        return 0;
    case 98:
        return 1;
    default:
        return -1;
    }
}
