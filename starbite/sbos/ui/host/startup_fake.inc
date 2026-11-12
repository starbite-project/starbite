/* Track J: Settings > Startup's platform on the host (shell.h sh_os_*, sh_fw_*, sh_req_*, sh_crash_*, sh_restart_*,
 * sh_fw_info): a made-up catalog, the loader's settings in RAM, two crash reports, a loader table that says "shell
 * mode, the card". Nothing restarts: a held confirmation prints what the phone would do. */
typedef struct { const char *key, *name; int cat, method, ready; } sf_os_t;
static const sf_os_t SF_OS[] = {
    { "starbite-os", "Starbite OS", 4, 5, 1 }, { "ios26", "iOS 26", 0, 1, 1 }, { "macos-sequoia", "macOS Sequoia", 0, 1, 0 },
    { "win11", "Windows 11", 1, 4, 1 }, { "android", "One UI 8", 2, 3, 1 }, { "linux-pmos", "postmarketOS", 3, 2, 1 },
    { "linux-ubuntu", "Ubuntu", 3, 2, 1 }, { "engineer", "Engineer OS", 4, 5, 1 }, { "utility", "Utility OS", 4, 5, 1 },
};
#define SF_N ((int)(sizeof SF_OS / sizeof SF_OS[0]))
static sh_fw_t SF_FW = { "starbite-os", 5, 1, 1, 0, 0, 0, 0, 0, 1, "" };
static int sf_find(const char *key)
{
    for (int i = 0; i < SF_N; i++) if (!strcmp(SF_OS[i].key, key)) return i;
    return -1;
}
int sh_os_count(int flags) { (void)flags; return SF_N; }
int sh_os_get(int i, char *key, char *name, int *category, int *method, int *ready, int *is_default)
{
    if (i < 0 || i >= SF_N) return -1;
    snprintf(key, 24, "%s", SF_OS[i].key);
    snprintf(name, 32, "%s", SF_OS[i].name);
    if (category) *category = SF_OS[i].cat;
    if (method) *method = SF_OS[i].method;
    if (ready) *ready = SF_OS[i].ready;
    if (is_default) *is_default = !strcmp(SF_FW.default_key, SF_OS[i].key);
    return 0;
}
int sh_fw_get(sh_fw_t *f) { *f = SF_FW; return 1; }
int sh_fw_set(const sh_fw_t *f)
{
    SF_FW = *f;
    fprintf(stderr, "startup: settings written: default %s, timeout %d s, debug %d, POST %d, watchdog %d/%d s, crash %d, after %d, shell %d\n",
            f->default_key, f->timeout_s, f->debug_level, f->post, f->wdt_mode, f->wdt_s, f->crash_mode, f->crash_action, f->shell);
    return 0;
}
int sh_os_make_default(const char *key)
{
    if (sf_find(key) < 0) return -1;
    snprintf(SF_FW.default_key, sizeof SF_FW.default_key, "%s", key);
    return sh_fw_set(&SF_FW);
}
/* the loader's sbfw_req_fits (M_XNU 1, M_LINUX 2, M_ANDROID 3, M_UEFI 4, M_BARE 5); WINRE never */
int sh_req_fits(const char *key, int a)
{
    int i = sf_find(key);
    if (i < 0) return 0;
    int m = SF_OS[i].method;
    return a == SH_REQ_VERBOSE ? m >= 1 && m <= 5 : a == SH_REQ_SAFE ? m == 5 : a == SH_REQ_SINGLE || a == SH_REQ_PREV ? m == 2 :
           a == SH_REQ_XNU_SINGLE ? m == 1 : 0;
}
static char sf_said[128];
static int sf_pending;
static int sf_ask(const char *title, const char *l1, const char *said)
{
    snprintf(sf_said, sizeof sf_said, "%s", said);
    sf_pending = 1;
    sh_confirm(title, l1, "Nothing else changes.", 0, "Hold to restart");
    return 0;
}
/* sh_confirmed's share: 1 if the question was ours */
static int startup_fake_confirmed(int ok)
{
    if (!sf_pending) return 0;
    sf_pending = 0;
    fprintf(stderr, "startup: %s\n", ok ? sf_said : "not restarted");
    return 1;
}
int sh_os_start_once(const char *key)
{
    int i = sf_find(key);
    if (i < 0) return -1;
    char t[64], s[96];
    snprintf(t, sizeof t, "Start %s once?", SF_OS[i].name);
    snprintf(s, sizeof s, "would restart into %s once", key);
    return sf_ask(t, "Starbite OS closes and the loader starts it once.", s);
}
int sh_req_start(const char *key, int a)
{
    if (!sh_req_fits(key, a)) return -1;
    char t[64], s[96];
    snprintf(t, sizeof t, "Start %s %s?", SF_OS[sf_find(key)].name, a == SH_REQ_VERBOSE ? "verbose" : a == SH_REQ_SAFE ? "in safe mode" :
             a == SH_REQ_PREV ? "with the previous kernel" : "in single user mode");
    snprintf(s, sizeof s, "would arm one-shot %d for %s and restart", a, key);
    return sf_ask(t, "For this one start only: the loader applies it and clears it after.", s);
}
int sh_restart_menu(void) { return sf_ask("Restart to the boot menu?", "The loader's own menu, this once.", "would restart to the boot menu"); }
int sh_restart_recovery(void) { return sf_ask("Restart to Starbite Recovery?", "The loader's recovery.", "would restart to Starbite Recovery"); }
int sh_restart_download(void) { return sf_ask("Restart to Download mode?", "S-Boot's Odin mode.", "would restart to Download mode"); }
int sh_crash_count(void) { return 2; }
int sh_crash_get(int i, sh_crash_t *c)
{
    memset(c, 0, sizeof *c);
    if (i == 0) {
        snprintf(c->key, sizeof c->key, "linux-pmos");
        snprintf(c->os, sizeof c->os, "postmarketOS");
        snprintf(c->kind, sizeof c->kind, "KERNEL PANIC");
        snprintf(c->line, sizeof c->line, "panic: Attempted to kill init! exitcode=0x0000000b");
        c->severity = 2;
        c->utc = 1794470123LL;
        return 0;
    }
    if (i == 1) {
        snprintf(c->key, sizeof c->key, "ios26");
        snprintf(c->os, sizeof c->os, "iOS 26");
        snprintf(c->kind, sizeof c->kind, "HANG (OS TIMER)");
        snprintf(c->line, sizeof c->line, "STARBITE: sweetreboot timer (no panic)");
        c->severity = 2;
        c->utc = 1794400100LL;
        return 0;
    }
    return -2;
}
int sh_fw_info(sh_fwinfo_t *f)
{
    memset(f, 0, sizeof *f);
    f->valid = 1;
    f->loader_version = 10;
    f->medium = 1;
    f->card_only = 1;
    f->shell_mode = 1;
    snprintf(f->why, sizeof f->why, "Starbite OS as the boot menu");
    snprintf(f->crash_key, sizeof f->crash_key, "linux-pmos");
    snprintf(f->crash_kind, sizeof f->crash_kind, "KERNEL PANIC");
    f->crash_sev = 2;
    return 1;
}
