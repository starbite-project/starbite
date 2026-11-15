/* Track J: Settings > Startup's platform on the host (shell.h sh_os_*, sh_fw_*, sh_req_*, sh_crash_*, sh_restart_*,
 * sh_fw_info; FW4: sh_health_os, sh_store_check, sh_logs_save, sh_slots_get, sh_shell_set,
 * sh_req_why, sh_req_label, sh_boot_os_req): a made-up catalog, the loader's settings in RAM, two crash reports, a
 * loader table that says "shell mode, the card", made-up check results and slots. Nothing restarts: a held confirmation prints what the phone would do. */
/* 8w: the phone's catalog as of 2026-11-15 (the main checkout's tools/store/catalog.toml: 32 rows, 19 hidden), so
 * the lock's cards and Startup are tried on the real list; hidden rows only with SH_OS_ALL (as the kernel's) */
typedef struct { const char *key, *name; int cat, method, ready, hidden; } sf_os_t;
static const sf_os_t SF_OS[] = {
    { "ios27", "iOS 27", 0, 1, 1, 0 }, { "mac13", "macOS Ventura", 0, 1, 1, 0 }, { "win11", "Windows 11", 1, 4, 1, 0 },
    { "win11-big", "Windows 11 (big cores)", 1, 4, 1, 0 }, { "android", "One UI 8", 2, 3, 1, 0 },
    { "chromeos", "ChromiumOS", 2, 2, 1, 0 }, { "linux-shell", "Starbite shell", 3, 2, 1, 1 },
    { "linux-ubuntu", "Ubuntu", 3, 2, 1, 0 }, { "linux-ubuntu-gnome", "Ubuntu Desktop", 3, 2, 0, 1 },
    { "linux-ubuntu-lomiri", "Ubuntu Lomiri", 3, 2, 0, 1 }, { "linux", "Ubuntu", 3, 2, 1, 1 },
    { "win11-m3probe", "Windows 11 (big-core probe)", 6, 4, 1, 1 }, { "linux-debian", "Debian", 3, 2, 0, 1 },
    { "linux-pmos", "postmarketOS", 3, 2, 1, 0 }, { "linux-arch", "Arch Linux ARM", 3, 2, 0, 1 },
    { "store2card", "Card store test", 6, 5, 1, 1 }, { "linux-kali", "Kali Linux", 3, 2, 0, 1 },
    { "uefishell", "UEFI Shell", 1, 4, 1, 1 }, { "ios27t", "iOS 27 (log run)", 6, 1, 1, 1 },
    { "engineer", "Engineer OS", 4, 5, 1, 0 }, { "utility", "Utility OS", 4, 5, 1, 0 },
    { "fwlayer", "Firmware layer test", 6, 5, 1, 1 }, { "maint", "Starbite Maintenance", 4, 5, 1, 1 },
    { "maint-rescue", "Starbite Maintenance (rescue)", 4, 5, 1, 1 }, { "win11-gpu", "Windows 11 (GPU test)", 6, 4, 1, 1 },
    { "starbite-os", "Starbite OS", 4, 5, 1, 0 }, { "modemsim", "Modem SIM test (attended)", 6, 5, 1, 1 },
    { "applem1", "Apple M1 test (chained loader)", 6, 5, 0, 1 }, { "starbite-next-gpu", "Starbite OS Next (GPU)", 4, 5, 1, 0 },
    { "starbite-next", "Starbite OS Next", 4, 5, 1, 0 }, { "win11-auto", "Windows 11 (unattended)", 6, 4, 1, 1 },
    { "mac15", "macOS Sequoia", 0, 1, 1, 1 },
};
#define SF_N ((int)(sizeof SF_OS / sizeof SF_OS[0]))
static sh_fw_t SF_FW = { "starbite-next-gpu", 5, 1, 1, 0, 0, 0, 0, 0, 1, "" };
static int sf_all;                                /* the flags sh_os_count was last given (as the kernel's) */
static int sf_find(const char *key)
{
    for (int i = 0; i < SF_N; i++) if (!strcmp(SF_OS[i].key, key)) return i;
    return -1;
}
static int sf_row(int i)                          /* the i-th row as listed (hidden rows only with SH_OS_ALL) */
{
    for (int k = 0; k < SF_N; k++) if (sf_all || !SF_OS[k].hidden) { if (!i) return k; i--; }
    return -1;
}
int sh_os_count(int flags)
{
    sf_all = flags & SH_OS_ALL;
    int n = 0;
    for (int k = 0; k < SF_N; k++) n += sf_all || !SF_OS[k].hidden;
    return n;
}
int sh_os_get(int i, char *key, char *name, int *category, int *method, int *ready, int *is_default)
{
    int k = i >= 0 ? sf_row(i) : -1;
    if (k < 0) return -1;
    snprintf(key, 24, "%s", SF_OS[k].key);
    snprintf(name, 32, "%s", SF_OS[k].name);
    if (category) *category = SF_OS[k].cat;
    if (method) *method = SF_OS[k].method;
    if (ready) *ready = SF_OS[k].ready;
    if (is_default) *is_default = !strcmp(SF_FW.default_key, SF_OS[k].key);
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
/* the loader's sbfw_req_fits (M_XNU 1, M_LINUX 2, M_ANDROID 3, M_UEFI 4, M_BARE 5); WINRE never. FW4: the same answer
 * as sh_req_why, which also has a reason: this made-up store has no previous kernel, a row not ready is not installed */
const char *sh_req_label(int a)
{
    return a == SH_REQ_VERBOSE ? "Verbose" : a == SH_REQ_SAFE ? "Safe Mode" : a == SH_REQ_SINGLE ? "Single User" :
           a == SH_REQ_PREV ? "Previous Kernel" : a == SH_REQ_XNU_SINGLE ? "Single User (XNU)" :
           a == SH_REQ_DRIVER_OFF ? "Drivers Off" : "";
}
int sh_req_why(const char *key, int a, char *why, int cap)
{
    const char *w = 0;
    int i = sf_find(key);
    int m = i < 0 ? 0 : SF_OS[i].method;
    if (i < 0) w = "Not in the catalog";
    else if (!sh_req_label(a)[0]) w = "Not available";
    else if (!(a == SH_REQ_VERBOSE ? m >= 1 && m <= 5 : a == SH_REQ_SAFE ? m == 5 : a == SH_REQ_SINGLE || a == SH_REQ_PREV ? m == 2 :
               a == SH_REQ_XNU_SINGLE ? m == 1 : a == SH_REQ_DRIVER_OFF ? m == 4 : 0))
        w = a == SH_REQ_SAFE ? "Only for Starbite systems" : a == SH_REQ_SINGLE || a == SH_REQ_PREV ? "Only for Linux systems" :
            a == SH_REQ_XNU_SINGLE ? "Only for Apple systems" : a == SH_REQ_DRIVER_OFF ? "Only for Windows" : "Not for this system";
    else if (!SF_OS[i].ready) w = "Not installed";
    else if (a == SH_REQ_PREV) w = "No previous kernel in the store";
    if (why && cap > 0) snprintf(why, (size_t)cap, "%s", w ? w : "");
    return !w;
}
int sh_req_fits(const char *key, int a) { return sh_req_why(key, a, 0, 0); }
int sh_boot_os_req(const char *key, int a)
{
    char why[48];
    if (!sh_req_why(key, a, why, sizeof why)) {
        fprintf(stderr, "startup: one-shot %d for %s refused: %s\n", a, key, why);
        return -3;
    }
    fprintf(stderr, "host: boot %s with %d\n", key, a);
    return -1;                              /* nothing restarts on the host */
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
             a == SH_REQ_PREV ? "with the previous kernel" : a == SH_REQ_DRIVER_OFF ? "with the Starbite drivers off" :
             "in single user mode");
    snprintf(s, sizeof s, "would arm one-shot %d for %s and restart", a, key);
    return sf_ask(t, "For this one start only: the loader applies it and clears it after.", s);
}
int sh_restart_menu(void) { return sf_ask("Restart to the boot menu?", "The loader's own menu, this once.", "would restart to the boot menu"); }
int sh_restart_recovery(void) { return sf_ask("Restart to Starbite Recovery?", "The loader's recovery.", "would restart to Starbite Recovery"); }
int sh_restart_repair(void) { return sf_ask("Restart to Repair and Reset?", "Starbite Recovery's repair page.", "would restart to recovery-repair"); }
int sh_restart_maint(int rescue) { fprintf(stderr, "startup: would restart to %s (asks first)\n", rescue ? "maint-rescue" : "maint"); return 0; }
int sh_restart_fastboot(void) { fprintf(stderr, "startup: would restart to the loader's USB fastboot (asks first)\n"); return 0; }
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
    snprintf(f->why, sizeof f->why, "Starbite OS at power-on");
    snprintf(f->crash_key, sizeof f->crash_key, "linux-pmos");
    snprintf(f->crash_kind, sizeof f->crash_kind, "KERNEL PANIC");
    f->crash_sev = 2;
    return 1;
}

/* FW4: made-up check results (the kernel: kern/sbfw_health.c on the loader's sbfwfs.c checkers) */
static void sf_h(sh_health_t *h, int checked, const char *fs, const char *where)
{
    memset(h, 0, sizeof *h);
    h->checked = checked;
    snprintf(h->fs, sizeof h->fs, "%s", fs);
    snprintf(h->where, sizeof h->where, "%s", where);
}
static void sf_l(sh_health_t *h, const char *s)
{
    for (int i = 0; i < SH_HEALTH_LINES; i++)
        if (!h->line[i][0]) { snprintf(h->line[i], sizeof h->line[i], "%s", s); return; }
}
int sh_health_os(const char *key, sh_health_t *h)
{
    int i = sf_find(key);
    if (i < 0) {
        memset(h, 0, sizeof *h);
        snprintf(h->line[0], sizeof h->line[0], "No such operating system in the catalog");
        return -1;
    }
    int m = SF_OS[i].method;
    if (m == 4) {
        sf_h(h, 1, "NTFS", "UFS WINESP + WINDOWS");
        sf_l(h, "FAT32: 260 MiB, 4 KiB clusters, FSInfo valid");
        sf_l(h, "FAT32: both FAT copies agree, clean shutdown");
        sf_l(h, "BCD: header clean, default {current} = Windows Boot Manager entry");
        sf_l(h, "BCD: recovery sequence present (Windows Recovery Environment)");
        sf_l(h, "NTFS: version 3.1, clean (no dirty flag), $MFT and $MFTMirr agree");
        sf_l(h, "No problems found");
    } else if (m == 2) {
        sf_h(h, 1, "ext4", "card sectors 4196352+16777216");
        sf_l(h, "Root: microSD card, card sectors 4196352+16777216");
        sf_l(h, "file system: ext2/3/4");
        sf_l(h, "PROBLEM: needs journal recovery (not unmounted cleanly)");
        sf_l(h, "ext4: label \"pmos_root\", journal, 64bit, metadata_csum: checksum OK");
        sf_l(h, "ext4: last mounted 2026-11-11 22:14 UTC, 41 mounts");
        sf_l(h, "Problems found: see above");
        h->bad = 1;
    } else if (m == 1) {
        sf_h(h, 1, "APFS", "UFS SYSTEM");
        sf_l(h, "Root: UFS partition SYSTEM");
        sf_l(h, "file system: APFS");
        sf_l(h, "APFS: container superblock checksum OK, 2 volumes");
        sf_l(h, "No problems found");
    } else if (m == 3) {
        sf_h(h, 1, "ext4", "UFS SYSTEM");
        sf_l(h, "Root: UFS partition SYSTEM");
        sf_l(h, "file system: ext2/3/4");
        sf_l(h, "ext4: clean, no errors recorded");
        sf_l(h, "No problems found");
    } else {
        sf_h(h, 0, "-", "none (RAM only)");
        sf_l(h, "Nothing to check: the root is none (RAM only)");
        sf_l(h, "Nothing was checked");
    }
    return 0;
}
int sh_store_check(sh_health_t *h)
{
    sf_h(h, 1, "-", "the microSD card");
    sf_l(h, "Home store: the microSD card");
    sf_l(h, "Header: OK, 23 entries, 3 GiB of 4 GiB used");
    sf_l(h, "Entries: all inside the store");
    sf_l(h, "Settings record: OK");
    sf_l(h, "Crash log: OK, 2 reports");
    sf_l(h, "Head backup: OK, the same as the store's head");
    sf_l(h, "No problems found");
    return 0;
}
int sh_logs_save(void)
{
    fprintf(stderr, "startup: would save the logs to \"loader-logs\"\n");
    return 0;
}
int sh_slots_get(sh_slots_t *s)
{
    memset(s, 0, sizeof *s);
    s->active = 1;
    s->trying = 0;
    s->state[0] = 1;
    s->state[1] = 1;
    snprintf(s->version[1], sizeof s->version[1], "1.0 cp6");
    s->state[2] = 0;
    return 0;
}
int sh_shell_set(int on)
{
    SF_FW.shell = !!on;
    fprintf(stderr, "startup: Starbite OS at power-on: %s (the user's choice)\n", on ? "on" : "off");
    return 0;
}
