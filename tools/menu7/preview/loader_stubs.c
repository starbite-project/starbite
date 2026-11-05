/* SPDX-License-Identifier: GPL-2.0-only
 * The loader under menu v7, on the host (docs/MENU7-PORT.md section 5; owner: menu7).
 *
 * h7.c (the preview harness) links this instead of menu.c, xnu_boot.c, catalog.c, store.c, em.c:
 *   hw_*            a fake clock (hw_ticks in us; hw_wait_us advances it, every read adds 1 us so busy
 *                   loops end), scripted keys and touches on that clock
 *   cat_*           the catalog from an SBCATLG1 file (--catalog; tools/store/sbstore2.py catalog TOML
 *                   OUT.sbc). Default: tools/menu7/preview/concept-catalog.toml (the concept's own
 *                   systems), packed on first use into /tmp/m7-concept.sbc, with the concept's
 *                   readiness and settings unless --ready / --settings say otherwise
 *   store_find      every file the catalog names, with a made-up size (31.4 MB-ish), on UFS
 *   star_entry_ready  --ready KEY,KEY | all | none (default: all); entries with an empty first file
 *                   are "coming" in menu7 whatever this says
 *   star_menu_*     the settings record (--settings default=K,last=K,timeout=N,valid=1), boot-next
 *                   (--next KEY), a failed start (--fail "WHY")
 *   star_font_blob  the BOOT ramdisk (--ramdisk build/brand/menu-v7.sbm), loaded 4 KiB aligned
 *   em_main         returns --lab-pick (default 0)
 *   --tmax MS       exit(0) once the fake clock passes MS (a menu with nothing moving draws no frames)
 *
 * h7.c calls h7_stubs_args(argc, argv) (after its own options, before g7_init): it removes the
 * options above from argv and returns the new argc. Scripts (--keys, --touch) are lists of comma-separated steps on the
 * fake clock: up, down, power, bixby (pressed for 80 ms), hold-power (600 ms), hold-bixby (800 ms),
 * wait:MS; touches: tap:X:Y, long:X:Y, drag:X:Y0:Y1, wait:MS. stub_now_us() is the fake clock.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../shim/starlte/menu.h"
#include "../../../shim/starlte/hw.h"
#include "../../../shim/starlte/catalog.h"
#include "../../../shim/starlte/store.h"
#include "../../../shim/starlte/fbtext.h"

/* ---- options ---- */

static const char *o_catalog;      /* 0: the concept catalog */
static const char *o_ready;
static const char *o_ramdisk = "build/brand/menu-v7.sbm";
static char o_next[24];
static const char *o_fail;
static int o_lab_pick;
static uint64_t o_tick_us;          /* --tick-us N: every hw_ticks read costs N us (a slow phone: the
                                     * renderer reads the clock between rows, so frames cost ~ rows x N) */
double o_slow;                      /* --slow K: every g7_frame (and g7_idle) advances the fake clock by K x
                                     * its host time (the device: K ~ 35), so real-time springs and the
                                     * countdown see the phone's frame times */
static uint64_t o_tmax;             /* --tmax MS: exit when the fake clock passes it (an idle menu) */
static struct star_settings o_set;
static int o_set_valid = 1;
static int fb_muted;

struct step { uint64_t t0, t1; unsigned keys; int kind, x, y, y1; };
static struct step K[256], T[256];
static unsigned nk, nt;

static uint64_t parse_steps(const char *s, struct step *a, unsigned *n, int touch)
{
    uint64_t t = 200000;            /* scripts start 200 ms after the first frame */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", s);
    for (char *tok = strtok(buf, ","); tok && *n < 256; tok = strtok(0, ",")) {
        struct step *p = &a[*n];
        memset(p, 0, sizeof(*p));
        if (!strncmp(tok, "wait:", 5)) {
            t += (uint64_t)atoi(tok + 5) * 1000;
            continue;
        }
        p->t0 = t;
        if (!touch) {
            unsigned d = 80000;
            if (!strcmp(tok, "up")) p->keys = K_UP;
            else if (!strcmp(tok, "down")) p->keys = K_DOWN;
            else if (!strcmp(tok, "power")) p->keys = K_POWER;
            else if (!strcmp(tok, "bixby")) p->keys = K_BIXBY;
            else if (!strcmp(tok, "hold-power")) { p->keys = K_POWER; d = 600000; }
            else if (!strcmp(tok, "hold-bixby")) { p->keys = K_BIXBY; d = 800000; }
            else if (!strcmp(tok, "hold-vol")) { p->keys = K_UP | K_DOWN; d = 900000; }
            else { fprintf(stderr, "loader_stubs: key %s?\n", tok); continue; }
            p->t1 = t + d;
            t += d + 120000;
        } else {
            int x = 0, y = 0, y1 = 0;
            if (sscanf(tok, "tap:%d:%d", &x, &y) == 2) { p->kind = 0; p->t1 = t + 80000; }
            else if (sscanf(tok, "long:%d:%d", &x, &y) == 2) { p->kind = 0; p->t1 = t + 700000; }
            else if (sscanf(tok, "drag:%d:%d:%d", &x, &y, &y1) == 3) { p->kind = 1; p->t1 = t + 300000; }
            else { fprintf(stderr, "loader_stubs: touch %s?\n", tok); continue; }
            p->x = x;
            p->y = y;
            p->y1 = y1;
            t = p->t1 + 150000;
        }
        ++*n;
    }
    return t;
}

static void parse_settings(const char *s)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(0, ",")) {
        if (!strncmp(tok, "default=", 8)) snprintf(o_set.default_key, 24, "%s", tok + 8);
        else if (!strncmp(tok, "last=", 5)) snprintf(o_set.last_key, 24, "%s", tok + 5);
        else if (!strncmp(tok, "timeout=", 8)) o_set.timeout_s = (uint32_t)atoi(tok + 8);
        else if (!strncmp(tok, "valid=", 6)) o_set_valid = atoi(tok + 6);
    }
}

/* the repository root, from this file's path (h7 is built with absolute source paths) */
static void root_path(char *out, size_t cap, const char *rel)
{
    const char *f = __FILE__, *e = strstr(f, "tools/menu7/preview/");
    snprintf(out, cap, "%.*s%s", e ? (int)(e - f) : 0, f, rel);
}

static void concept_defaults(void)
{
    static char sbc[512];
    char toml[512], tool[512], cmd[1600];
    snprintf(sbc, sizeof(sbc), "/tmp/m7-concept.sbc");
    root_path(toml, sizeof(toml), "tools/menu7/preview/concept-catalog.toml");
    root_path(tool, sizeof(tool), "tools/store/sbstore2.py");
    snprintf(cmd, sizeof(cmd), "python3 '%s' catalog '%s' '%s'", tool, toml, sbc);
    if (system(cmd))
        fprintf(stderr, "loader_stubs: %s failed\n", cmd);
    o_catalog = sbc;
    if (!o_ready)
        o_ready = "ios26,win11,a17,phosh,display,touch,uart,mem";
}

int h7_stubs_args(int argc, char **argv)
{
    int o = 1, settings = 0;
    o_set.timeout_s = 5;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "--catalog") && v) { o_catalog = v; ++i; }
        else if (!strcmp(a, "--ready") && v) { o_ready = v; ++i; }
        else if (!strcmp(a, "--settings") && v) { parse_settings(v); settings = 1; ++i; }
        else if (!strcmp(a, "--keys") && v) { parse_steps(v, K, &nk, 0); ++i; }
        else if (!strcmp(a, "--touch") && v) { parse_steps(v, T, &nt, 1); ++i; }
        else if (!strcmp(a, "--ramdisk") && v) { o_ramdisk = v; ++i; }
        else if (!strcmp(a, "--next") && v) { snprintf(o_next, sizeof(o_next), "%s", v); ++i; }
        else if (!strcmp(a, "--fail") && v) { o_fail = v; ++i; }
        else if (!strcmp(a, "--lab-pick") && v) { o_lab_pick = atoi(v); ++i; }
        else if (!strcmp(a, "--tmax") && v) { o_tmax = (uint64_t)atoi(v) * 1000; ++i; }
        else if (!strcmp(a, "--tick-us") && v) { o_tick_us = (uint64_t)atoi(v); ++i; }
        else if (!strcmp(a, "--slow") && v) { o_slow = atof(v); ++i; }
        else argv[o++] = argv[i];
    }
    argv[o] = 0;
    if (!o_catalog) {
        concept_defaults();
        if (!settings)
            parse_settings("default=phosh,last=ios26,timeout=5");
    }
    if (!o_ready)
        o_ready = "all";
    return o;
}

/* ---- the fake clock, keys, touch ---- */

static uint64_t clk;

uint64_t stub_now_us(void) { return clk; }
void stub_advance_us(uint64_t us) { clk += us; }
uint64_t hw_ticks(void) { return clk += 1 + o_tick_us; }
uint64_t hw_freq(void) { return 1000000; }
void hw_wait_us(uint64_t us)
{
    clk += us;
    if (o_tmax && clk > o_tmax) {
        printf("loader_stubs: --tmax reached at %llu ms\n", (unsigned long long)(clk / 1000));
        exit(0);
    }
}
void hw_wait_ms(unsigned ms) { clk += (uint64_t)ms * 1000; }

unsigned hw_keys(void)
{
    unsigned k = 0;
    for (unsigned i = 0; i < nk; ++i)
        if (clk >= K[i].t0 && clk < K[i].t1)
            k |= K[i].keys;
    return k;
}

static unsigned t_cur, t_phase;     /* 0 not down, 1 down sent, 2 moves, 3 up sent */
static int t_lastx, t_lasty;

int hw_tp_event(int *st, int *x, int *y)
{
    while (t_cur < nt) {
        struct step *p = &T[t_cur];
        if (clk < p->t0)
            return 0;
        if (t_phase == 0) {
            t_phase = 1;
            *st = 1;
            *x = t_lastx = p->x;
            *y = t_lasty = p->y;
            return 1;
        }
        if (p->kind == 1 && clk < p->t1) {
            int ny = p->y + (int)((int64_t)(p->y1 - p->y) * (int64_t)(clk - p->t0) / (int64_t)(p->t1 - p->t0));
            if (ny == t_lasty)
                return 0;
            *st = 2;
            *x = p->x;
            *y = t_lasty = ny;
            return 1;
        }
        if (clk < p->t1)
            return 0;
        *st = 3;
        *x = p->x;
        *y = p->kind == 1 ? p->y1 : p->y;
        ++t_cur;
        t_phase = 0;
        return 1;
    }
    return 0;
}

void hw_tp_start(void) {}
void hw_tp_clear(void) {}
int hw_tp_ok(void) { return 1; }
void hw_reboot(uint32_t inform3) { printf("loader_stubs: hw_reboot(0x%08x)\n", inform3); }
void hw_power_off(void) { printf("loader_stubs: hw_power_off (stays on)\n"); clk += 3000000; }
void star_fb_mute(int on) { fb_muted = on; }
int stub_fb_muted(void) { return fb_muted; }

/* ---- the catalog (catalog.c's format) ---- */

static struct cat_ent cat[CAT_MAX];
static unsigned cat_n;
static int cat_done;

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = n; i; --i)
        v = v << 8 | p[i - 1];
    return v;
}

static void str_at(char *d, unsigned cap, const uint8_t *b, uint32_t off, uint32_t total)
{
    unsigned k = 0;
    if (off)
        for (; off + k < total && k + 1 < cap && b[off + k]; ++k)
            d[k] = (char)b[off + k];
    for (; k < cap; ++k)
        d[k] = 0;
}

int cat_load(void)
{
    if (cat_done)
        return (int)cat_n;
    cat_done = 1;
    FILE *f = fopen(o_catalog, "rb");
    static uint8_t b[65536];
    size_t size = f ? fread(b, 1, sizeof(b), f) : 0;
    if (f)
        fclose(f);
    if (size < 32 || memcmp(b, "SBCATLG1", 8)) {
        fprintf(stderr, "loader_stubs: no catalog in %s\n", o_catalog);
        return 0;
    }
    uint32_t n = (uint32_t)le(b + 12, 4), total = (uint32_t)le(b + 24, 4);
    for (uint32_t i = 0; i < n && cat_n < CAT_MAX; ++i) {
        const uint8_t *p = b + 32 + 64 * i;
        struct cat_ent *e = &cat[cat_n];
        memcpy(e->key, p, 24);
        e->key[23] = 0;
        if (!e->key[0])
            continue;
        e->category = p[24];
        e->method = p[25];
        e->tag = p[26];
        e->icon = p[27];
        e->flags = (uint16_t)le(p + 28, 2);
        e->order = (uint16_t)le(p + 30, 2);
        str_at(e->name, sizeof(e->name), b, (uint32_t)le(p + 32, 4), total);
        str_at(e->version, sizeof(e->version), b, (uint32_t)le(p + 36, 4), total);
        for (unsigned k = 0; k < 4; ++k)
            str_at(e->file[k], sizeof(e->file[k]), b, (uint32_t)le(p + 40 + 4 * k, 4), total);
        str_at(e->cmdline, sizeof(e->cmdline), b, (uint32_t)le(p + 56, 4), total);
        str_at(e->root, sizeof(e->root), b, (uint32_t)le(p + 60, 4), total);
        ++cat_n;
    }
    return (int)cat_n;
}

unsigned cat_count(void) { return (unsigned)cat_load(); }
const struct cat_ent *cat_get(unsigned i) { return i < cat_count() ? &cat[i] : 0; }
int cat_builtin(void) { return 0; }

int cat_find(const char *key)
{
    for (unsigned i = 0; i < cat_count(); ++i)
        if (!strncmp(cat[i].key, key, 23))
            return (int)i;
    return -1;
}

int cat_alt(const char *field, unsigned k, char *out)
{
    unsigned i = 0;
    while (k && field[i])
        if (field[i++] == '|')
            --k;
    if (k || !field[i])
        return 0;
    unsigned n = 0;
    while (field[i] && field[i] != '|' && n < 63)
        out[n++] = field[i++];
    out[n] = 0;
    return n > 0;
}

static int listed(const char *list, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = list; (p = strstr(p, key)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || !p[n]))
            return 1;
    return 0;
}

int star_entry_ready(unsigned i, const char **why)
{
    const struct cat_ent *e = cat_get(i);
    int ok = e && (!strcmp(o_ready, "all") || listed(o_ready, e->key)) && strcmp(o_ready, "none");
    if (why)
        *why = ok ? 0 : e && e->method == M_XNU ? "NO XNU PAYLOAD" : e && e->method == M_UEFI ? "NO UEFI IMAGE" : "NO KERNEL";
    return ok;
}

/* ---- the store: every named file, sizes made up from the name ---- */

int store_find(const char *name, struct store_ent *e)
{
    memset(e, 0, sizeof(*e));
    const char *n = name;
    if (!strncmp(n, "ufs:", 4))
        n += 4;
    else if (!strncmp(n, "sd:", 3)) {
        n += 3;
        e->medium = STORE_SD;
    }
    snprintf(e->name, sizeof(e->name), "%s", n);
    uint32_t h = 2166136261U;
    for (const char *p = n; *p; ++p)
        h = (h ^ (uint8_t)*p) * 16777619U;
    e->size = (uint64_t)(4 + h % 60) << 20 | (h & 0xfffff);
    e->crc = h;
    return 1;
}

int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap) { (void)e; (void)dst; (void)cap; return 0; }

/* the error state's evidence: the concept's CRCs (state=error, "expected ... · read ...") */
int store_last_load(char *name, unsigned cap, uint64_t *size, uint32_t *want, uint32_t *got)
{
    snprintf(name, cap, "linux.image");
    *size = 31400000;
    *want = 0x9A3F1C07U;
    *got = 0x9A3F0E11U;
    return -2;
}
void store_last_clear(void) {}

/* ---- menu.c's API ---- */

void star_menu_begin(void) {}
struct star_settings *star_menu_settings(int *valid)
{
    if (valid)
        *valid = o_set_valid;
    return &o_set;
}
int star_menu_settings_save(void)
{
    o_set_valid = 1;
    printf("loader_stubs: settings saved default=%s timeout=%u\n", o_set.default_key, o_set.timeout_s);
    return 0;
}
int star_menu_ready_key(const char *key)
{
    int i = cat_find(key);
    return i >= 0 && star_entry_ready((unsigned)i, 0) ? i : -1;
}
int star_menu_next(void)
{
    int i = o_next[0] ? star_menu_ready_key(o_next) : -1;
    o_next[0] = 0;
    return i;
}
void star_menu_remember(int idx)
{
    const struct cat_ent *e = cat_get((unsigned)idx);
    if (e)
        snprintf(o_set.last_key, 24, "%s", e->key);
}
static int failed_once;
int star_menu_failure(const char **name, const char **why)
{
    if (!o_fail || failed_once)
        return 0;
    failed_once = 1;
    *name = "";
    *why = o_fail;
    return 1;
}
void star_menu_failed(const char *name, const char *why) { (void)name; o_fail = why; failed_once = 0; }

int em_main(const struct star_info *in, char *cmdline, unsigned len)
{
    (void)in;
    (void)cmdline;
    (void)len;
    printf("loader_stubs: em_main -> %d\n", o_lab_pick);
    return o_lab_pick;
}

/* ---- the BOOT ramdisk ---- */

static uint8_t *rd;
static size_t rdn;

int star_font_blob(uint64_t *start, uint64_t *end)
{
    if (!rd) {
        FILE *f = fopen(o_ramdisk, "rb");
        if (!f)
            return 0;
        fseek(f, 0, SEEK_END);
        rdn = (size_t)ftell(f);
        rewind(f);
        void *p = 0;
        if (posix_memalign(&p, 4096, rdn + 4096))
            return 0;
        rd = p;
        if (fread(rd, 1, rdn, f) != rdn)
            rdn = 0;
        fclose(f);
    }
    for (size_t i = 0; i + 8 < rdn; i += 4)
        if (!memcmp(rd + i, "SBFONT01", 8)) {
            *start = (uintptr_t)(rd + i);
            *end = (uintptr_t)(rd + rdn);
            return 1;
        }
    return 0;
}

/* ---- the battery (batt.c is the phone's: MMIO) ----
 * H7_BATT: "87" (default), "12" (low), "64,c" (cable in), "none" (no answer: the outline only). */

#include "../../../shim/starlte/batt.h"

int batt_init(void)
{
    const char *e = getenv("H7_BATT");
    return !(e && !strcmp(e, "none"));
}

int batt_read(struct batt_info *b)
{
    const char *e = getenv("H7_BATT");
    memset(b, 0, sizeof(*b));
    if (!batt_init())
        return 0;
    int p = e ? atoi(e) : 87;
    b->valid = 1;
    b->pct = (uint8_t)(p < 0 ? 0 : p > 100 ? 100 : p);
    b->plugged = b->charging = e && strchr(e, 'c') ? 1 : 0;
    b->mv = (uint16_t)(3500 + 7 * b->pct);
    b->ma = (int16_t)(b->plugged ? 900 : -250);
    return 1;
}

int batt_percent(void)
{
    struct batt_info b;
    return batt_read(&b) ? b.pct : -1;
}

int batt_mv(void)
{
    struct batt_info b;
    return batt_read(&b) ? b.mv : -1;
}

int batt_plugged(void)
{
    struct batt_info b;
    return batt_read(&b) ? b.plugged : -1;
}
