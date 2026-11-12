/* SPDX-License-Identifier: GPL-2.0-only
 * Settings > Startup's kernel calls (ui/shell.h sh_os_*, sh_fw_*, sh_req_*, sh_crash_*, sh_restart_*, sh_fw_info) on
 * libsb's firmware-layer API (libsb/sbfw_api.h; docs/STARBITE-FIRMWARE-LAYER.md section 10):
 *   - the catalog's OS rows (hidden ones only on request), whether each is ready, which one is the default;
 *   - the loader's settings (the home store's record) read and written: default OS, menu timeout, debug level, POST,
 *     watchdog, crash detection and what follows a crash, Starbite OS as the boot menu (shell mode);
 *   - one-shot requests for an OS's next start (verbose, safe mode, single user, previous kernel, XNU single user);
 *   - the crash reports of the home store's "crashlog", the loader's table (sb_fw) for "why this start";
 *   - restarts: an OS once, the plain boot menu once, Starbite Recovery, Download mode.
 * Every restart and every one-shot asks first with the kernel's hold confirmation (kui_confirm: Next draws it in
 * its own look) and switches everything off as for a restart (power_boot_os). A settings write is the user's tap
 * on that row: the store gate is opened for exactly that one record write (tier 1, kind store) and closed again.
 * In a self-test image (sbos_selftest_enabled: starbite-os-selftest*, SBOS_SELFTEST_NEXT's too) every write and every
 * restart is a dry run that logs a line, and neither the settings record nor the crash log is read: the settings live
 * in a RAM copy (defaults first) so the pages still change.
 * Built into every image (the regular UI does not call it).
 */
#include "sb.h"
#include "kern.h"
#include "store.h"
#include "catalog.h"
#include "sbfw_api.h"
#include "../ui/shell.h"

static int dry(void)
{
#ifdef SB_HOST
    return 1;
#else
    return sbos_selftest_enabled;
#endif
}

static void say(const char *what, const char *key)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, dry() ? "startup: would " : "startup: ");
    ub_s(&u, what);
    if (key && key[0]) {
        ub_s(&u, " ");
        ub_s(&u, key);
    }
    if (dry())
        ub_s(&u, " (a dry run: the self test)");
    sb_log(u.b);
}

/* ---- the catalog's rows ------------------------------------------------------------------------------------------ */

static int os_flags;

static int os_shown(const struct cat_ent *c)
{
    return c && c->key[0] && ((os_flags & SH_OS_ALL) || !(c->flags & CF_HIDDEN));
}

/* catalog index of the i-th shown row, or -1 */
static int os_index(int i)
{
    unsigned n = cat_count();
    for (unsigned k = 0; k < n; ++k)
        if (os_shown(cat_get(k)) && i-- == 0)
            return (int)k;
    return -1;
}

static char def_key[24];              /* the default, read once per sh_os_count (not one store read per row) */

int sh_os_count(int flags)
{
    os_flags = flags;
    sh_fw_t f;
    sb_strcpy(def_key, sizeof(def_key), sh_fw_get(&f) >= 0 ? f.default_key : "");
    int n = 0;
    for (unsigned k = 0; k < cat_count(); ++k)
        n += os_shown(cat_get(k));
    return n;
}

static int same(const char *a, const char *b);
static sh_fw_t fw_ram;                /* the dry run's settings (after the first dry write) */
static int fw_ram_on;

int sh_os_get(int i, char *key /* [24] */, char *name /* [32] */, int *category, int *method, int *ready, int *is_default)
{
    int k = os_index(i);
    const struct cat_ent *c = k >= 0 ? cat_get((unsigned)k) : 0;
    if (!c)
        return -1;
    sb_strcpy(key, 24, c->key);
    sb_strcpy(name, 32, c->name[0] ? c->name : c->key);
    if (category)
        *category = c->category;
    if (method)
        *method = c->method;
    if (ready)
        *ready = star_entry_ready((unsigned)k, 0);
    if (is_default)
        *is_default = def_key[0] && same(def_key, c->key);
    return 0;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

static int known(const char *key)
{
    return key && key[0] && cat_find(key) >= 0;
}

/* ---- confirmations and restarts ---------------------------------------------------------------------------------- */

enum { RS_ONCE = 1, RS_REQ, RS_MENU, RS_RECOVERY, RS_DOWNLOAD };
static struct { int what; unsigned action; char key[24]; } pend;

static void download_now(void)
{
    torch_set(0);
    kmodem_stop("Restarting");
    wifi_power(0);
    guest_power_off();
    heater_stop_all("Restarting");
    heater_trip_restore_now();
    sb_mdelay(200);
    sb_reboot(SB_INFORM_DOWNLOAD);
}

static int store_gate(const char *what);

static void restart_done(int ok)
{
    int w = pend.what;
    pend.what = 0;
    if (!ok || !w) {
        sb_log("startup: not restarted");
        return;
    }
    switch (w) {
    case RS_ONCE:
        say("restart once into", pend.key);
        if (!dry())
            power_boot_os(pend.key);
        break;
    case RS_REQ: {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "arm the one-shot \"");
        ub_s(&u, sbfw_req_name(pend.action));
        ub_s(&u, "\" and restart into");
        say(u.b, pend.key);
        if (dry())
            break;
        int e = store_gate("startup: a one-shot request");
        if (!e) {
            e = sbfw_request_arm(pend.key, pend.action);
            sb_gate_close();
        }
        if (e) {
            ub_init(&u);
            ub_s(&u, "startup: the request was not written (");
            ub_sdec(&u, e);
            ub_s(&u, "): not restarted");
            sb_log(u.b);
            break;
        }
        power_boot_os(pend.key);
        break;
    }
    case RS_MENU:
        say("restart to the plain boot menu (\"menu\")", 0);
        if (!dry())
            power_boot_os("menu");
        break;
    case RS_RECOVERY:
        say("restart to Starbite Recovery (\"recovery\")", 0);
        if (!dry())
            power_boot_os("recovery");
        break;
    case RS_DOWNLOAD:
        say("restart to Download mode", 0);
        if (!dry())
            download_now();
        break;
    }
}

static int ask(int what, const char *key, unsigned action, const char *title, const char *l1, const char *l2)
{
    if (pend.what)
        return -1;                                /* one question at a time */
    pend.what = what;
    pend.action = action;
    sb_strcpy(pend.key, sizeof(pend.key), key ? key : "");
    kui_confirm(title, l1, l2, dry() ? "A dry run: the self test only logs it." : 0, "Hold to restart", restart_done);
    return 0;
}

int sh_os_start_once(const char *key)
{
    if (!known(key))
        return -1;
    const struct cat_ent *c = cat_get((unsigned)cat_find(key));
    static char t[64];
    struct ub u;
    ub_init(&u);
    ub_s(&u, "Start ");
    ub_s(&u, c->name[0] ? c->name : key);
    ub_s(&u, " once?");
    sb_strcpy(t, sizeof(t), u.b);
    return ask(RS_ONCE, key, 0, t, "Starbite OS closes and the loader starts it once.",
               "The next start after that is as before.");
}

int sh_req_fits(const char *key, int action)
{
    int k = key && key[0] ? cat_find(key) : -1;
    const struct cat_ent *c = k >= 0 ? cat_get((unsigned)k) : 0;
    return c && action > 0 && action != SH_REQ_WINRE && sbfw_req_fits((unsigned)action, c->method);
}

int sh_req_start(const char *key, int action)
{
    if (!sh_req_fits(key, action))
        return -1;
    const struct cat_ent *c = cat_get((unsigned)cat_find(key));
    static char t[64];
    struct ub u;
    ub_init(&u);
    ub_s(&u, "Start ");
    ub_s(&u, c->name[0] ? c->name : key);
    ub_s(&u, action == SH_REQ_VERBOSE ? " verbose?" : action == SH_REQ_SAFE ? " in safe mode?" :
              action == SH_REQ_PREV ? " with the previous kernel?" : " in single user mode?");
    sb_strcpy(t, sizeof(t), u.b);
    return ask(RS_REQ, key, (unsigned)action, t, "For this one start only: the loader applies it and clears it after.",
               "Starbite OS closes now.");
}

int sh_restart_menu(void)
{
    return ask(RS_MENU, 0, 0, "Restart to the boot menu?", "The loader's own menu, this once: pick any OS or setting there.",
               "The next start is as before.");
}

int sh_restart_recovery(void)
{
    return ask(RS_RECOVERY, 0, 0, "Restart to Starbite Recovery?", "The loader's recovery: the store, the settings, the logs.",
               "Restart from there to come back.");
}

int sh_restart_download(void)
{
    return ask(RS_DOWNLOAD, 0, 0, "Restart to Download mode?", "S-Boot's Odin mode, for flashing from a computer.",
               "Hold Volume Down + Power to leave it without flashing.");
}

/* ---- the loader's settings ----------------------------------------------------------------------------------------- */

static int store_gate(const char *what)
{
    int h = store_home();
    if (h < 0)
        return SBFW_E_NOHOME;
    struct sb_auth a;
    sb_auth_init(&a, h == (int)STORE_SD ? SB_MED_SD : SB_MED_UFS, 0, 0, 0, SB_TIER1, SB_KIND_STORE, what);
    sb_auth_step(&a, sb_steps_needed(SB_TIER1, SB_KIND_STORE));   /* the user's own tap on that row */
    return sb_gate_open(&a) ? 0 : SB_W_AUTH;
}

static void fw_from(sh_fw_t *f, const struct sbfw_settings *s)
{
    sb_strcpy(f->default_key, sizeof(f->default_key), s->default_key);
    f->timeout_s = (int)s->timeout_s;
    f->debug_level = s->debug_level;
    f->post = s->post;
    f->wdt_mode = s->wdt_mode;
    f->wdt_s = s->wdt_s;
    f->crash_mode = s->crash_mode;
    f->crash_action = s->crash_action;
    f->crash_detail = s->crash_detail;
    f->shell = s->shell;
    sb_strcpy(f->shell_key, sizeof(f->shell_key), s->shell_key);
}

int sh_fw_get(sh_fw_t *f)
{
    if (fw_ram_on || dry()) {                /* the self test reads no settings record: its RAM copy (defaults first) */
        *f = fw_ram;
        return fw_ram_on;
    }
    struct sbfw_settings s;
    int r = sbfw_settings_read(&s);
    fw_from(f, &s);
    return store_home() < 0 ? -1 : r;
}

int sh_fw_set(const sh_fw_t *f)
{
    if (f->default_key[0] && !known(f->default_key))
        return SBFW_E_NOFIT;
    if (f->shell_key[0] && !known(f->shell_key))
        return SBFW_E_NOFIT;
    struct sbfw_settings s;
    sbfw_settings_read(&s);
    sb_strcpy(s.default_key, sizeof(s.default_key), f->default_key);
    s.timeout_s = f->timeout_s < 0 ? 0 : (uint32_t)f->timeout_s;
    s.debug_level = (uint8_t)f->debug_level;
    s.post = (uint8_t)f->post;
    s.wdt_mode = (uint8_t)f->wdt_mode;
    s.wdt_s = (uint16_t)f->wdt_s;
    s.crash_mode = (uint8_t)f->crash_mode;
    s.crash_action = (uint8_t)f->crash_action;
    s.crash_detail = (uint8_t)f->crash_detail;
    s.shell = (uint8_t)!!f->shell;
    sb_strcpy(s.shell_key, sizeof(s.shell_key), f->shell_key);
    if (f->debug_level < 0 || f->debug_level > 3 || f->post < 0 || f->post > 2 || f->wdt_mode < 0 || f->wdt_mode > 2 ||
        f->crash_mode < 0 || f->crash_mode > 2 || f->crash_action < 0 || f->crash_action > 2 || f->wdt_s < 0 ||
        f->crash_detail < 0 || f->crash_detail > 3 ||
        (f->wdt_mode == SBFW_WDT_FIXED && (f->wdt_s < (int)SBFW_WDT_MIN_S || f->wdt_s > (int)SBFW_WDT_MAX_S)))
        return SBFW_E_INVAL;
    if (dry()) {
        say("write the loader's settings", 0);
        fw_ram = *f;
        fw_ram_on = 1;
        return 0;
    }
    int e = store_gate("startup: the loader's settings");
    if (e)
        return e;
    e = sbfw_settings_write(&s);
    sb_gate_close();
    if (e) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "startup: the settings were not written: ");
        ub_sdec(&u, e);
        sb_log(u.b);
    } else
        sb_log("startup: the loader's settings written (the next start reads them)");
    return e;
}

int sh_os_make_default(const char *key)
{
    if (!known(key))
        return -1;
    sh_fw_t f;
    if (sh_fw_get(&f) < 0)
        return SBFW_E_NOHOME;
    sb_strcpy(f.default_key, sizeof(f.default_key), key);
    return sh_fw_set(&f);
}

/* ---- crash reports and the loader's table ------------------------------------------------------------------------- */

int sh_crash_count(void)
{
    return dry() ? SBFW_E_NOLOG : sbfw_crash_count();   /* the self test reads no crash log */
}

int sh_crash_get(int i, sh_crash_t *o)
{
    struct sbfw_crash c;
    if (i < 0 || dry())
        return -1;
    int r = sbfw_crash_get((unsigned)i, &c);
    if (r)
        return r;
    sb_strcpy(o->key, sizeof(o->key), c.key);
    sb_strcpy(o->os, sizeof(o->os), c.os[0] ? c.os : c.key);
    sb_strcpy(o->kind, sizeof(o->kind), c.kind);
    sb_strcpy(o->line, sizeof(o->line), c.line);
    o->severity = (int)c.severity;
    o->utc = (long long)c.epoch;
    return 0;
}

static const char *why_text(unsigned w)
{
    switch (w) {
    case SBFW_WHY_MENU:     return "Picked in the boot menu";
    case SBFW_WHY_AUTO:     return "The default, started by itself";
    case SBFW_WHY_NEXT:     return "Started once (boot next)";
    case SBFW_WHY_SHELL:    return "Starbite OS as the boot menu";
    case SBFW_WHY_RETRY:    return "The retry after a crash";
    case SBFW_WHY_FASTBOOT: return "fastboot boot";
    default:                return "Unknown";
    }
}

int sh_fw_info(sh_fwinfo_t *o)
{
    uint8_t *z = (uint8_t *)o;
    for (unsigned i = 0; i < sizeof(*o); ++i)
        z[i] = 0;
    o->valid = sb_fw.valid;
    int h = store_home();
    o->medium = sb_fw.valid ? (sb_fw.store_medium == STORE_SD ? 1 : sb_fw.store_medium == STORE_UFS ? 0 : -1)
                            : (h == (int)STORE_SD ? 1 : h == (int)STORE_UFS ? 0 : -1);
    o->card_only = store_card_only();
    if (!sb_fw.valid) {
        sb_strcpy(o->why, sizeof(o->why), "An older loader (no table)");
        return 0;
    }
    o->loader_version = (int)sb_fw.loader_version;
    sb_strcpy(o->why, sizeof(o->why), why_text(sb_fw.boot_reason));
    o->shell_mode = !!(sb_fw.flags & SBFW_TF_SHELL);
    o->req_action = (int)sb_fw.req_action;
    o->post_fails = (int)sb_fw.post_fails;
    o->post_warns = (int)sb_fw.post_warns;
    if (sb_fw.crash_kind) {
        sb_strcpy(o->crash_key, sizeof(o->crash_key), sb_fw.crash_key);
        sb_strcpy(o->crash_kind, sizeof(o->crash_kind), sbfw_kind_name(sb_fw.crash_kind));
        o->crash_sev = (int)sb_fw.crash_sev;
    }
    return 1;
}
