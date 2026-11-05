/* SPDX-License-Identifier: GPL-2.0-only
 * Engineer OS host preview (docs/ENGINEER-OS.md section 7): the real application and sbui code,
 * built for aarch64 Linux (the Lima VM) with libsb's hardware layer replaced by fakes, driven by
 * a script of key presses and taps on a fake clock, writing the panel to PPM files.
 *
 *   engineer-host OUTDIR CATALOG.sbc cmd...
 *     w:MS          wait MS (fake) milliseconds
 *     k:p|u|d|b     tap Power / Volume Up / Volume Down / Bixby (80 ms)
 *     h:p|u|d|b:MS  hold a key MS ms
 *     t:X,Y         tap the panel at X, Y
 *     g:X,Y,DY,MS   drag from X, Y by DY over MS ms, then lift
 *     s:NAME        write OUTDIR/NAME.ppm; also check that the panel equals the back buffer (every
 *                   drawn pixel was presented: the dirty rectangles cover every change)
 *     q             quit
 *
 * Register blocks, DRAM windows, the framebuffer, pmsg and the loader's log page are anonymous
 * mappings at their real addresses, seeded with plausible values, so the pages' raw register
 * reads run unchanged. The PMIC, fuel gauge, touch, UFS, store and card are fakes below.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <math.h>
#include <signal.h>
#include <ucontext.h>

#include "sbui.h"
#include "ufs.h"
#include "store.h"
#include "sd.h"
#include "catalog.h"
#include "sbui_w.h"

/* ---- fake clock ------------------------------------------------------------------------------ */

static uint64_t now_us = 7ULL * 60 * 1000000 + 123456;   /* "uptime" 7 min */

uint64_t sb_ticks(void) { now_us += 1; return now_us * 26U; }
uint64_t sb_freq(void) { return 26000000U; }
uint64_t sb_us(void) { now_us += 1; return now_us; }
uint64_t sb_ms(void) { return sb_us() / 1000U; }
static void host_step(void);
void sb_udelay(uint64_t us) { now_us += us; host_step(); }
void sb_mdelay(unsigned ms) { sb_udelay((uint64_t)ms * 1000U); }

/* ---- libsb state the image's crt0 / sys.c would own ------------------------------------------- */

struct sb_boot sb_boot;
volatile struct sb_fault sb_fault;
volatile uint64_t sb_probe_depth;
volatile uint64_t sb_core_box[32];
char sb_core_entry[4];
int64_t sb_psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)fn; (void)a2; (void)a3;
    /* A55 cpu1-3 come up and go off again; the M3 cores are "denied" in the preview */
    if (a1 < 4) {
        unsigned slot = (unsigned)a1;
        sb_core_box[slot * 4 + 1] = 0x81000000ULL | (a1 << 8);
        sb_core_box[slot * 4 + 2] = 4;
        sb_core_box[slot * 4] = 1;
        return 0;
    }
    return -3;
}

static uint8_t *arena;
static uint64_t arena_used;
#define ARENA (320ULL << 20)
void *sb_alloc(uint64_t size)
{
    uint64_t p = (arena_used + 63) & ~63ULL;
    if (p + size > ARENA)
        return 0;
    arena_used = p + size;
    return arena + p;
}
uint64_t sb_arena_used(void) { return arena_used; }
uint64_t sb_arena_free(void) { return ARENA - arena_used; }

void sb_mmu_enable(void) {}
int sb_mmu_on(void) { return 1; }
void sb_dcache_clean_inval(const volatile void *a, uint64_t n) { (void)a; (void)n; }
void sb_dcache_inval(const volatile void *a, uint64_t n) { (void)a; (void)n; }
int sb_map_kind(uint64_t pa)
{
    if (pa < 0x80000000ULL)
        return 1;
    if (pa >= 0xaf800000ULL && pa < 0xc0000000ULL)
        return 0;
    if (pa < 0x100000000ULL)
        return (pa >= 0xa0000000ULL && pa < 0xa8000000ULL) || pa >= 0xaf000000ULL ? 3 : 2;
    return pa >= 0x880000000ULL && pa < 0x900000000ULL ? 3 : 0;
}
int sb_probe32(uint64_t addr, uint32_t *v)
{
    if (!sb_map_kind(addr) || (addr >= 0x880000000ULL)) {
        *v = 0;
        sb_fault.count++;
        return 0;
    }
    *v = *(volatile uint32_t *)addr;
    return 1;
}

void sb_crash_show(const char *why, const uint64_t *regs)
{
    (void)regs;
    fprintf(stderr, "CRASH: %s\n", why ? why : "?");
    exit(3);
}

/* ---- keys, reset ------------------------------------------------------------------------------ */

static unsigned keys_now;
unsigned sb_keys(void) { host_step(); return keys_now; }
void sb_keys_release_wait(void) { while (sb_keys()) sb_mdelay(20); }
void sb_reboot(uint32_t inform3) { printf("host: reboot (inform3 %#x)\n", inform3); exit(0); }
void sb_power_off(void) { printf("host: power off\n"); exit(0); }
void sb_boot_next(const char *key) { printf("host: boot-next %s\n", key); exit(0); }
int sb_boot_next_peek(char key[24]) { key[0] = 0; return 0; }

/* ---- PMIC, RTC, rails ------------------------------------------------------------------------------ */

static uint8_t pmic[3][256];
int sb_pmic_read(uint32_t type, uint32_t reg, uint8_t *val)
{
    if (type > 2 || reg > 255)
        return 0;
    *val = pmic[type][reg];
    return 1;
}
int sb_acpm_build(char out[25]) { strcpy(out, "Mar 29 2018 16:48:12 ac"); return 1; }
void sb_rtc_read(struct sb_rtc *r)
{
    uint64_t s = 14 * 3600 + 32 * 60 + 10 + now_us / 1000000U;
    r->y = 2026; r->mo = 11; r->d = 5;
    r->h = (unsigned)(s / 3600U) % 24U; r->mi = (unsigned)(s / 60U) % 60U; r->s = (unsigned)(s % 60U);
    r->ctrl = 0x02; r->ok = 1;
}
int sb_rtc_epoch(uint64_t *e) { struct sb_rtc r; sb_rtc_read(&r); *e = 1793887200ULL + r.h * 3600U + r.mi * 60U + r.s - 14 * 3600; return 1; }
int sb_touch_rails(uint8_t *l35, uint8_t *l43) { *l35 = pmic[1][0x6c]; *l43 = pmic[1][0x74]; return 1; }
int sb_sd_rails(uint8_t *l31, uint8_t *l2) { *l31 = pmic[1][0x68]; *l2 = pmic[1][0x4a]; return 0; }

/* ---- I2C, MAX77705 ------------------------------------------------------------------------------- */

struct sb_i2c sb_max_bus = { 0x14360000UL, 0x000000e0, 0x00f00000, 0x00210000, 1 };
const char *sb_max_why;
void sb_i2c_bus(struct sb_i2c *b) { (void)b; }
int sb_i2c_msg(struct sb_i2c *b, unsigned a, int r, uint8_t *d, unsigned n, int s) { (void)b; (void)a; (void)r; (void)d; (void)n; (void)s; return 1; }
static int fake_ma(void)
{
    double t = (double)now_us / 1e6;
    return (int)(-310 + 90 * sin(t / 9.0) + 35 * sin(t / 2.3));
}
int sb_fg_word(uint8_t reg, uint16_t *v)
{
    switch (reg) {
    case 0x06: *v = 0x4c00 | 0x60; break;                       /* 76.3 % */
    case 0xff: *v = 0x4b00 | 0x40; break;
    case 0x09: *v = (uint16_t)((3912 + fake_ma() / 40) * 64 / 5); break;
    case 0x19: *v = (uint16_t)(3915 * 64 / 5); break;
    case 0xfb: *v = (uint16_t)(3958 * 64 / 5); break;
    case 0x0a: *v = (uint16_t)(int16_t)(fake_ma() * 16 / 5); break;
    case 0x0b: *v = (uint16_t)(int16_t)(-305 * 16 / 5); break;
    case 0x05: *v = 2252; break;
    case 0x35: *v = 2861; break;
    case 0x18: *v = 0x0b97; break;
    case 0x17: *v = 312; break;
    case 0x08: *v = (uint16_t)(29 * 256 + 128); break;
    case 0x00: *v = 0x0002; break;
    default: *v = 0; break;
    }
    return 1;
}
int sb_chg_read(uint8_t reg, uint8_t *v)
{
    switch (reg) {
    case 0xb2: *v = 0x00; break;
    case 0xb3: *v = 0x00; break;
    case 0xb4: *v = 0x38; break;                                /* battery OK, charger off */
    case 0xb9: *v = 0x3c; break;
    case 0xc0: *v = 0x3f; break;
    case 0xb7: *v = 0x05; break;
    default: *v = 0; break;
    }
    return 1;
}
int sb_usbc_read(uint8_t reg, uint8_t *v) { (void)reg; *v = 0; return 1; }
int sb_i2c_read(struct sb_i2c *b, unsigned addr, uint8_t reg, uint8_t *data, unsigned len)
{
    (void)b;
    if (addr == 0x36 && len == 2) { uint16_t w; sb_fg_word(reg, &w); data[0] = (uint8_t)w; data[1] = (uint8_t)(w >> 8); return 1; }
    if (addr == 0x69 && len == 1) return sb_chg_read(reg, data);
    if (addr == 0x66 && len == 1) { *data = reg == 0 ? 0x05 : 0x09; return 1; }
    return 0;
}
int sb_max_init(void) { return 1; }
int sb_max_id(uint8_t *id, uint8_t *rev) { *id = 0x05; *rev = 0x09; return 1; }
int sb_bat_read(struct sb_bat *b)
{
    uint16_t v;
    b->ok = 1;
    sb_fg_word(0x06, &v);
    b->soc_pct = v >> 8;
    b->soc_x10 = (v >> 8) * 10 + (int)((v & 0xffU) * 10U / 256U);
    sb_fg_word(0x09, &v);
    b->mv = (int)((uint32_t)v * 5U / 64U);
    b->avg_mv = 3915;
    b->ma = fake_ma();
    b->avg_ma = -305;
    b->input = 0;
    b->chg_dtls = 8;
    return 1;
}

/* ---- touch: the script's taps and drags ----------------------------------------------------- */

struct sb_touch_info sb_tp;
#define TQ 64
static struct { uint64_t t; int st, x, y; } tq[TQ];
static int tq_h, tq_t;
static void tq_push(uint64_t t, int st, int x, int y)
{
    tq[tq_h].t = t; tq[tq_h].st = st; tq[tq_h].x = x; tq[tq_h].y = y;
    tq_h = (tq_h + 1) % TQ;
}
int sb_touch_start(void)
{
    sb_tp.ok = 1; sb_tp.max_x = 1440; sb_tp.max_y = 2960; sb_tp.tx = 32; sb_tp.rx = 16;
    sb_tp.id[0] = 0x11; sb_tp.id[1] = 0x29; sb_tp.id[2] = 0x07; sb_tp.status = 0x20;
    return 1;
}
int sb_touch_event(int *st, int *x, int *y)
{
    host_step();
    if (tq_t == tq_h || tq[tq_t].t > now_us)
        return 0;
    *st = tq[tq_t].st; *x = tq[tq_t].x; *y = tq[tq_t].y;
    tq_t = (tq_t + 1) % TQ;
    return 1;
}
int sb_touch_events(int *id, int *st, int *x, int *y, int max)
{
    int n = 0;
    while (n < max && sb_touch_event(&st[n], &x[n], &y[n])) {
        id[n] = 0;
        ++n;
    }
    return n;
}
int sb_touch_read(uint8_t reg, uint8_t *data, unsigned len) { (void)reg; memset(data, 0, len); return 1; }
void sb_touch_clear(void) {}

/* ---- UFS, GPT, store, card ----------------------------------------------------------------------- */

int ufs_state = 1, ufs_why;
uint32_t ufs_ocs, ufs_resp, ufs_status, ufs_qresp;
char gpt_name[GPT_MAX][24];
uint64_t gpt_first[GPT_MAX], gpt_last[GPT_MAX];
int gpt_n = -1;

static const struct { const char *n; uint64_t first, blocks; } parts[] = {
    { "BOTA0", 6, 1024 }, { "BOTA1", 1030, 1024 }, { "EFS", 2054, 5120 }, { "PARAM", 7174, 2048 },
    { "BOTA2", 9222, 2048 }, { "UP_PARAM", 11270, 3072 }, { "BOOT", 14342, 14336 },
    { "RECOVERY", 28678, 16384 }, { "RADIO", 45062, 12800 }, { "KEYSTORAGE", 57862, 256 },
    { "PERSISTENT", 58118, 512 }, { "STEADY", 58630, 256 }, { "DTB", 58886, 2048 }, { "DTBO", 60934, 2048 },
    { "CPEFS", 62982, 5120 }, { "SYSTEM", 68736, 1153024 }, { "VENDOR", 1221760, 179200 },
    { "ODM", 1400960, 10240 }, { "CACHE", 1411200, 76800 }, { "OMR", 1488000, 12800 },
    { "HIDDEN", 1500800, 107520 }, { "CP_DEBUG", 1608320, 25600 }, { "A16DATA", 1633920, 2621440 },
    { "WINESP", 4255360, 65536 }, { "WINDOWS", 4320896, 8388608 }, { "USERDATA", 12709504, 4915200 },
};
int gpt_load(void)
{
    if (gpt_n >= 0)
        return gpt_n;
    gpt_n = 0;
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        sb_strcpy(gpt_name[gpt_n], 24, parts[i].n);
        gpt_first[gpt_n] = parts[i].first;
        gpt_last[gpt_n] = parts[i].first + parts[i].blocks - 1;
        ++gpt_n;
    }
    return gpt_n;
}
int gpt_find(const char *name, uint64_t *first, uint64_t *last)
{
    gpt_load();
    for (int i = 0; i < gpt_n; ++i)
        if (sb_streq(gpt_name[i], name)) {
            *first = gpt_first[i];
            *last = gpt_last[i];
            return 1;
        }
    return 0;
}
int gpt_header(uint64_t lba, struct gpt_head *h)
{
    memset(h, 0, sizeof(*h));
    h->my_lba = lba;
    h->alt_lba = lba == 1 ? 17624703 : 1;
    h->first_usable = 6;
    h->last_usable = 17624698;
    h->entries_lba = lba == 1 ? 2 : 17624699;
    h->num = 128;
    h->esz = 128;
    h->hdr_crc = h->hdr_crc_calc = 0x52b1c3a7U ^ (uint32_t)lba;
    h->ent_crc = h->ent_crc_calc = 0x9a01f2e4U;
    for (int i = 0; i < 16; ++i)
        h->guid[i] = (uint8_t)(0x52 + 17 * i);
    return 1;
}
int ufs_up(void) { return 1; }
int ufs_read(uint64_t lba, uint32_t count)
{
    uint8_t *b = (uint8_t *)UFS_BUF;
    memset(b, 0, (size_t)count * 4096U);
    for (uint32_t k = 0; k < count; ++k) {
        uint8_t *p = b + k * 4096U;
        uint64_t l = lba + k;
        if (l == 1)
            memcpy(p, "EFI PART\0\0\1\0\x5c\0\0\0", 16);
        for (int i = 0; i < gpt_n; ++i)
            if (l == gpt_first[i]) {
                const char *n = gpt_name[i];
                if (sb_streq(n, "SYSTEM") || sb_streq(n, "VENDOR") || sb_streq(n, "ODM") || sb_streq(n, "EFS")) {
                    p[1080] = 0x53; p[1081] = 0xef;
                    sb_strcpy((char *)p + 1144, 16, sb_streq(n, "SYSTEM") ? "/" : n);
                } else if (sb_streq(n, "A16DATA") || sb_streq(n, "CACHE")) {
                    p[1024] = 0x10; p[1025] = 0x20; p[1026] = 0xf5; p[1027] = 0xf2;
                } else if (sb_streq(n, "WINDOWS")) {
                    memcpy(p + 3, "NTFS", 4);
                } else if (sb_streq(n, "WINESP")) {
                    memcpy(p + 82, "FAT32", 5);
                } else if (sb_streq(n, "USERDATA")) {
                    p[1024] = 'H'; p[1025] = 'X';
                } else if (sb_streq(n, "BOOT") || sb_streq(n, "RECOVERY")) {
                    memcpy(p, "ANDROID!", 8);
                }
            }
        if (l > 1 && l < 6)
            for (int i = 0; i < 4096; i += 7)
                p[i] = (uint8_t)(i * 13 + l);
    }
    return 0;
}
int ufs_read_to(uint64_t lba, uint64_t count, uintptr_t dst) { (void)lba; memset((void *)dst, 0, count * 4096U); return 0; }
int ufs_scsi(const uint8_t *cdb, unsigned n, uint32_t len)
{
    (void)n;
    memset((void *)UFS_BUF, ' ', len);
    if (cdb[0] == 0x12) {
        memcpy((void *)(UFS_BUF + 8), "SAMSUNG ", 8);
        memcpy((void *)(UFS_BUF + 16), "KLUCG4J1ED-B0C1 ", 16);
        memcpy((void *)(UFS_BUF + 32), "0300", 4);
    }
    return 0;
}
int ufs_desc(uint8_t idn, uint8_t index, uint8_t *out, unsigned cap)
{
    memset(out, 0, cap);
    if (idn == 0x00) {
        out[0x06] = 8; out[0x10] = 2; out[0x11] = 0x10; out[0x12] = 0x07; out[0x13] = 0x17;
        out[0x15] = 1; out[0x16] = 2; out[0x18] = 0x01; out[0x19] = 0xce; out[0x21] = 32;
        out[0x22] = 0x02; out[0x23] = 0x10; out[0x2a] = 3;
        return 0x40;
    }
    if (idn == 0x05) {
        const char *s = index == 1 ? "KLUCG4J1ED-B0C1" : index == 2 ? "2C7A1B39F0E1" : "0300";
        unsigned n = (unsigned)strlen(s);
        out[0] = (uint8_t)(2 + 2 * n);
        for (unsigned i = 0; i < n; ++i)
            out[2 + 2 * i + 1] = (uint8_t)s[i];
        return out[0];
    }
    if (idn == 0x07) {
        uint64_t raw = 0x7740000ULL * 8;
        for (int i = 0; i < 8; ++i)
            out[4 + i] = (uint8_t)(raw >> (56 - 8 * i));
        return 0x48;
    }
    if (idn == 0x02) {
        uint64_t n = 17624704;
        out[0x0a] = 12;
        for (int i = 0; i < 8; ++i)
            out[0x0b + i] = (uint8_t)(n >> (56 - 8 * i));
        return 0x23;
    }
    if (idn == 0x09) {
        out[2] = 1; out[3] = 0x02; out[4] = 0x01;
        return 0x25;
    }
    return -3;
}

static const char *store_names[] = {
    "catalog", "ios16.uefi", "mac13-v33.xnu", "ios26-sd.xnu", "windows.uefi", "android.kernel",
    "android.ramdisk", "android.cmdline", "linux.image", "linux.dtb", "linux.initrd", "engineer.img",
    "uefishell.uefi",
};
static uint8_t *catalog_blob;
static uint64_t catalog_size;
int store_open(unsigned m) { return m == STORE_UFS ? (int)(sizeof(store_names) / sizeof(store_names[0])) : 0; }
int store_version(unsigned m) { return m == STORE_UFS ? 2 : 0; }
int store_get(unsigned m, unsigned i, struct store_ent *e)
{
    if (m != STORE_UFS || i >= sizeof(store_names) / sizeof(store_names[0]))
        return 0;
    memset(e, 0, sizeof(*e));
    sb_strcpy(e->name, 32, store_names[i]);
    e->lba = 16903808 + i * 4096U;
    e->size = i == 0 ? catalog_size : (uint64_t)(1 + i * 7) * 1048576U + i * 4321U;
    e->crc = 0x1234abcdU * (i + 1);
    e->medium = 0;
    return 1;
}
int store_find(const char *name, struct store_ent *e)
{
    if (name[0] == 's' && name[1] == 'd' && name[2] == ':')
        return 0;
    if (name[0] == 'u' && name[1] == 'f' && name[2] == 's' && name[3] == ':')
        name += 4;
    for (unsigned i = 0; store_get(0, i, e); ++i)
        if (sb_streq(e->name, name))
            return 1;
    return 0;
}
int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap)
{
    if (sb_streq(e->name, "catalog") && catalog_blob && catalog_size <= cap) {
        memcpy((void *)dst, catalog_blob, catalog_size);
        return 0;
    }
    return -1;
}
int store_head(const struct store_ent *e, uintptr_t dst) { (void)e; memset((void *)dst, 0, 4096); return 0; }
uint64_t store_base(unsigned m) { return m ? 0 : 16903808; }
uint64_t store_capacity(unsigned m) { return m ? 0 : 721024; }
uint64_t store_used(unsigned m) { return m ? 0 : 412931; }
void store_reset(unsigned m) { (void)m; }
int settings_load(struct star_settings *s)
{
    memset(s, 0, sizeof(*s));
    s->seq = 41;
    s->timeout_s = 0;
    s->flags = 0;
    strcpy(s->last_key, "mac13");
    strcpy(s->default_key, "mac13");
    return 1;
}

struct sd_report sd_rep;
static int sd_ok;
void sd_allow(void) { sd_ok = 1; }
int sd_allowed(void) { return sd_ok; }
int sd_up(void)
{
    sd_rep.present = 1;
    sd_rep.rails = 0;
    sd_rep.gate[0] = 0x00200000; sd_rep.gate[1] = 0x00200000; sd_rep.gate[2] = 0x00200000;
    sd_rep.state = -1;
    sd_rep.why = "card rails off (Tier 1: Storage > microSD)";
    return 0;
}
int sd_read(uint64_t s, uint32_t c, uintptr_t d) { (void)s; (void)c; (void)d; return -1; }
int sd_store_area(uint64_t *f, uint64_t *b) { (void)f; (void)b; return 0; }
int sd_read4k(uint64_t l, uint64_t c, uintptr_t d) { (void)l; (void)c; (void)d; return -1; }

/* ---- present: the panel the host sees ------------------------------------------------------- */

static uint32_t *panel;
void host_present_rect(const uint32_t *bb, int x, int y, int w, int h)
{
    for (int yy = y; yy < y + h; ++yy)
        memcpy(panel + (size_t)yy * UI_W + x, bb + (size_t)yy * UI_W + x, (size_t)w * 4);
}

static const char *outdir;
static void shot(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
    static uint8_t row[UI_W * 3];
    for (int y = 0; y < UI_H; ++y) {
        for (int x = 0; x < UI_W; ++x) {
            uint32_t p = panel[(size_t)y * UI_W + x];
            row[3 * x] = (uint8_t)(p >> 16);
            row[3 * x + 1] = (uint8_t)(p >> 8);
            row[3 * x + 2] = (uint8_t)p;
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
    uint64_t diff = 0;
    const uint32_t *bb = ui_back();
    for (size_t i = 0; bb && i < (size_t)UI_W * UI_H; ++i)
        diff += (panel[i] & 0xffffffU) != (bb[i] & 0xffffffU);
    printf("shot %-22s t=%7.2f s  frames %llu  last copy %llu px  panel vs back buffer: %llu px differ\n",
           name, (double)now_us / 1e6, (unsigned long long)ui_stats.frames,
           (unsigned long long)ui_stats.px_last, (unsigned long long)diff);
}

/* ---- the script ----------------------------------------------------------------------------- */

static char **cmds;
static int ncmd, ci;
static uint64_t next_t;
static int in_step;

static unsigned key_of(char c)
{
    return c == 'p' ? SB_K_POWER : c == 'u' ? SB_K_UP : c == 'd' ? SB_K_DOWN : SB_K_BIXBY;
}

static uint64_t key_up_at;
static unsigned key_held;

static void host_step(void)
{
    if (in_step)
        return;
    in_step = 1;
    if (key_held && now_us >= key_up_at) {
        keys_now &= ~key_held;
        key_held = 0;
    }
    while (ci < ncmd && now_us >= next_t && !key_held) {
        const char *c = cmds[ci++];
        if (c[0] == 'w') {
            next_t = now_us + (uint64_t)atoi(c + 2) * 1000U;
        } else if (c[0] == 'k' || c[0] == 'h') {
            key_held = key_of(c[2]);
            keys_now |= key_held;
            uint64_t ms = c[0] == 'k' ? 80 : (uint64_t)atoi(c + 4);
            key_up_at = now_us + ms * 1000U;
            next_t = key_up_at + 250000;
        } else if (c[0] == 't') {
            int x, y;
            sscanf(c + 2, "%d,%d", &x, &y);
            tq_push(now_us, 1, x, y);
            tq_push(now_us + 60000, 3, x, y);
            next_t = now_us + 400000;
        } else if (c[0] == 'g') {
            int x, y, dy, ms;
            sscanf(c + 2, "%d,%d,%d,%d", &x, &y, &dy, &ms);
            tq_push(now_us, 1, x, y);
            for (int k = 1; k <= 8; ++k)
                tq_push(now_us + (uint64_t)ms * 1000U * k / 8, 2, x, y + dy * k / 8);
            tq_push(now_us + (uint64_t)ms * 1000U + 1000, 3, x, y + dy);
            next_t = now_us + (uint64_t)ms * 1000U + 600000;
        } else if (c[0] == 's') {
            shot(c + 2);
            next_t = now_us;
        } else if (c[0] == 'S') {
            /* after scrolling: the incrementally scrolled viewport must equal a full redraw */
            extern struct page *host_last_page;
            shot(c + 2);
            if (host_last_page) {
                size_t n = (size_t)UI_W * UI_H;
                uint32_t *save = malloc(n * 4), *bb = ui_back();
                memcpy(save, bb, n * 4);
                page_redraw(host_last_page);
                uint64_t d = 0;
                for (int y = host_last_page->vy0; y < host_last_page->vy1; ++y)
                    for (int x = 0; x < UI_W; ++x)
                        d += (save[(size_t)y * UI_W + x] & 0xffffffU) != (bb[(size_t)y * UI_W + x] & 0xffffffU);
                printf("scroll check %-14s scroll %d: incremental vs full redraw: %llu px differ\n", c + 2,
                       host_last_page->scroll, (unsigned long long)d);
                memcpy(bb, save, n * 4);
                free(save);
            }
            next_t = now_us;
        } else if (c[0] == 'q') {
            printf("host: done at %.2f s, %llu frames\n", (double)now_us / 1e6, (unsigned long long)ui_stats.frames);
            exit(0);
        }
    }
    if (ci >= ncmd && !key_held && now_us >= next_t) {
        printf("host: script ended\n");
        exit(0);
    }
    in_step = 0;
}

/* ---- fake memory map and seeded registers ----------------------------------------------------------- */

static void map(uint64_t a, uint64_t n)
{
    void *p = mmap((void *)a, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != (void *)a) {
        fprintf(stderr, "mmap %#llx: failed\n", (unsigned long long)a);
        exit(2);
    }
}

static void w32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint32_t pll(unsigned m, unsigned p, unsigned s) { return 0xa0000010U | m << 16 | p << 8 | s; }

/* a tiny FDT: /chosen/bootargs, two memory banks, /reserved-memory */
static uint8_t fdt[8192], fstr[1024];
static uint32_t fn, fsn;
static void be(uint32_t v) { fdt[fn] = (uint8_t)(v >> 24); fdt[fn + 1] = (uint8_t)(v >> 16); fdt[fn + 2] = (uint8_t)(v >> 8); fdt[fn + 3] = (uint8_t)v; fn += 4; }
static void fbegin(const char *n) { be(1); size_t l = strlen(n) + 1; memcpy(fdt + fn, n, l); fn += (uint32_t)((l + 3) & ~3U); }
static void fend(void) { be(2); }
static uint32_t sname(const char *n)
{
    for (uint32_t i = 0; i < fsn; i += (uint32_t)strlen((char *)fstr + i) + 1)
        if (!strcmp((char *)fstr + i, n))
            return i;
    uint32_t o = fsn;
    strcpy((char *)fstr + fsn, n);
    fsn += (uint32_t)strlen(n) + 1;
    return o;
}
static void fprop(const char *n, const void *v, uint32_t l) { be(3); be(l); be(sname(n)); memcpy(fdt + fn, v, l); fn += (l + 3) & ~3U; }
static void fcells(const char *n, const uint32_t *c, unsigned k)
{
    uint8_t b[64];
    for (unsigned i = 0; i < k; ++i) { b[4 * i] = (uint8_t)(c[i] >> 24); b[4 * i + 1] = (uint8_t)(c[i] >> 16); b[4 * i + 2] = (uint8_t)(c[i] >> 8); b[4 * i + 3] = (uint8_t)c[i]; }
    fprop(n, b, 4 * k);
}
static void make_fdt(uint64_t at)
{
    static const char args[] = "console=ram loglevel=4 androidboot.em.model=SM-G960F androidboot.revision=26 "
        "androidboot.serialno=R58K30ABCDE androidboot.ap_serial=0x0A1B2C3D4E5F sales_code=XEF "
        "androidboot.carrierid=XEF region1=EUR androidboot.bootloader=G960FXXUHFVG4 androidboot.warranty_bit=1 "
        "androidboot.verifiedbootstate=orange androidboot.fmm_lock=0 androidboot.odin_download=1 "
        "androidboot.hmac_mismatch=0 bootmode=0 charging_mode=0x0 androidboot.dram_info=01,06,00,4G "
        "sec_debug.lpddr4_size=4 lcdtype=37383 pmic_info=33 ccic_info=1 sec_debug.reset_reason=7 "
        "sec_debug.pwronsrc=0x40 sec_debug.pwroffsrc=0x0 sec_debug.rst_stat=0x20000000";
    fn = 40;
    fbegin("");
    uint32_t two = 2, one = 1;
    fcells("#address-cells", &two, 1);
    fcells("#size-cells", &one, 1);
    fbegin("chosen");
    fprop("bootargs", args, sizeof(args));
    fend();
    fbegin("memory@80000000");
    uint32_t m0[3] = { 0, 0x80000000U, 0x7c000000U };
    fcells("reg", m0, 3);
    fend();
    fbegin("memory@880000000");
    uint32_t m1[3] = { 8, 0x80000000U, 0x80000000U };
    fcells("reg", m1, 3);
    fend();
    fbegin("reserved-memory");
    uint32_t c2 = 2;
    fcells("#address-cells", &c2, 1);
    fcells("#size-cells", &c2, 1);
    static const struct { const char *n; uint32_t a, s; } rm[] = {
        { "rkp_region", 0xaf800000U, 0x2000000U }, { "tima_region", 0xb8000000U, 0x200000U },
        { "seclog_mem", 0xc0000000U, 0x80000U }, { "fb_handover", 0xcc000000U, 0x1050000U },
        { "secure_camera", 0xe1900000U, 0x1e00000U }, { "modem_if", 0xf0000000U, 0x9200000U },
        { "cp_log", 0xf9200000U, 0x2000000U }, { "abox_rmem", 0xfb200000U, 0x2800000U },
        { "debug_snapshot", 0xfda00000U, 0x1c00000U }, { "camera_fw", 0xff800000U, 0x800000U },
    };
    for (unsigned i = 0; i < sizeof(rm) / sizeof(rm[0]); ++i) {
        char nm[48];
        snprintf(nm, sizeof(nm), "%s@%x", rm[i].n, rm[i].a);
        fbegin(nm);
        uint32_t r[4] = { 0, rm[i].a, 0, rm[i].s };
        fcells("reg", r, 4);
        fend();
    }
    fend();
    fend();
    be(9);
    uint32_t off_struct = 40, size_struct = fn - 40, off_str = fn;
    memcpy(fdt + fn, fstr, fsn);
    uint32_t total = fn + fsn;
    fn = 0;
    be(0xd00dfeed); be(total); be(off_struct); be(off_str); be(0); be(17); be(16); be(0); be(fsn); be(size_struct);
    memcpy((void *)at, fdt, total);
}

static void seed(void)
{
    /* CHIPID, PMU, keys, touch INT */
    w32(0x10000000, 0x81100010U);
    w32(0x10000014, 0x3a51c0deU);
    w32(0x10000018, 0x00004b21U);
    w32(0x14060404, 0x20000000U);
    w32(0x14060808, 0x12345678U);
    w32(0x1406080c, 0x12345670U);
    w32(0x14050024, 0xffffffffU);
    w32(0x14050044, 0xffffffffU);
    w32(0x14050064, 0xffffffffU);
    w32(0x14064044, 0x0000000fU);           /* G3D on */
    w32(0x14062044, 0x00000000U);           /* CPUCL1 (M3) off */
    w32(0x14202014, 0x00300000U);
    /* TMU: running, one-point trim 50, codes = temperature + 25 */
    for (int b = 0; b < 2; ++b) {
        uint64_t base = b ? 0x10074000UL : 0x10070000UL;
        w32(base + 0x20, 0x00001001U);
        for (int p = 0; p < 8; ++p)
            w32(base + 4U * (unsigned)p, 50);
    }
    int top[8] = { 37, 44, 43, 45, 42, 39, 41, 38 }, sub[3] = { 36, 38, 34 };
    w32(0x10070040, (uint32_t)(top[0] + 25) | (uint32_t)(top[1] + 25) << 9);
    w32(0x10070044, (uint32_t)(top[2] + 25) | (uint32_t)(top[3] + 25) << 9 | (uint32_t)(top[4] + 25) << 18);
    w32(0x10070048, (uint32_t)(top[5] + 25) | (uint32_t)(top[6] + 25) << 9 | (uint32_t)(top[7] + 25) << 18);
    w32(0x10074040, (uint32_t)(sub[0] + 25) | (uint32_t)(sub[1] + 25) << 9);
    w32(0x10074044, (uint32_t)(sub[2] + 25));
    /* clocks: A55 on its PLL (1794 MHz), shared PLLs, MIF, G3D */
    w32(0x1d000140, pll(138, 2, 0));
    w32(0x17400140, pll(35, 2, 0));
    w32(0x1b800120, pll(276, 2, 0));
    w32(0x1a240120, pll(123, 2, 0));
    w32(0x1a2401a0, pll(144, 2, 1));
    w32(0x1a240220, pll(103, 2, 0));
    w32(0x1a2402a0, pll(92, 3, 0));
    w32(0x1a240320, pll(76, 2, 0));
    /* Mali */
    w32(0x17500000, 0x62210010U);
    w32(0x17500100, 0x0003ffffU);
    /* UFS host */
    w32(0x11120000, 0x0187001fU);
    w32(0x11120008, 0x00000210U);
    w32(0x11120030, 0x0000000fU);
    w32(0x11121100, 0x8000000cU);
    /* DSIM: command mode, ready */
    w32(0x16080020, 1U << 20);
    w32(0x16030070, 0x1281U);
    /* PMIC: TOP id, LDO controls on, touch rails on, card rails off, BUCK6 */
    pmic[0][0x00] = 0x11;
    for (int n = 1; n <= 45; ++n)
        pmic[1][n == 1 ? 0x49 : n == 2 ? 0x4a : 0x4c + n - 3] = (uint8_t)((n % 3 ? 0xc0 : 0x00) | ((n * 7) & 0x3f));
    pmic[1][0x6c] = 0xec;
    pmic[1][0x74] = 0xf0;
    pmic[1][0x68] = 0x28;
    pmic[1][0x4a] = 0x3c;
    pmic[1][0x2b] = 0x68;
    pmic[1][0x10] = 0x40;
    pmic[1][0x0e] = 0x01;
    /* the loader's log page */
    volatile uint64_t *l = (volatile uint64_t *)0x91fff000UL;
    l[0] = 0x31304554524c5453ULL;
    l[1] = 4;
    l[2] = 0x30d00800;
    l[3] = 0x8a000000;
    l[4] = 0x80080000;
    l[10] = 0x600;
    l[15] = 0x92080000;
    l[17] = 0x8a000000;
    l[77] = 0x1281;
    l[119] = 0x300000;
    /* pmsg: an earlier session's test lines */
    const char *pm = "eng: libsb up\nSBENG1 B 1\nSBENG1 T PASS Memory test | 16 MiB, no errors, 806 ms\n"
                     "SBENG1 T PASS Keys | all four keys seen\nSBENG1 T FAIL Core cpu4 M3 | CPU_ON DENIED\n";
    volatile uint32_t *h = (volatile uint32_t *)SB_PMSG;
    h[0] = 0x43474244U;
    h[1] = (uint32_t)strlen(pm);
    h[2] = (uint32_t)strlen(pm);
    memcpy((void *)(SB_PMSG + 12), pm, strlen(pm));
}

void sb_main(void);

/* EL1 system registers the pages read (CurrentEL, CLIDR, CSSELR / CCSIDR ...) trap at EL0: emulate
 * an MRS as a read of a plausible value and an MSR as nothing, as a probe would on the phone. */
static void on_sigill(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;
    (void)si;
    ucontext_t *uc = ctx;
    uint32_t insn = *(uint32_t *)uc->uc_mcontext.pc;
    if ((insn & 0xfff00000U) == 0xd5300000U) {             /* MRS */
        unsigned rt = insn & 0x1fU, sysreg = (insn >> 5) & 0x7fffU;
        uint64_t v = 0;
        if (sysreg == 0x4212)                               /* CurrentEL */
            v = 4;
        else if (sysreg == 0x4801)                          /* CLIDR_EL1 */
            v = 0x0a200023ULL;
        else if (sysreg == 0x4800)                          /* CCSIDR_EL1 */
            v = 0x701fe01aULL;
        if (rt != 31)
            uc->uc_mcontext.regs[rt] = v;
    }
    uc->uc_mcontext.pc += 4;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_sigill;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: engineer-host OUTDIR CATALOG.sbc cmd...\n");
        return 2;
    }
    outdir = argv[1];
    FILE *f = fopen(argv[2], "rb");
    if (f) {
        catalog_blob = malloc(65536);
        catalog_size = fread(catalog_blob, 1, 65536, f);
        fclose(f);
    }
    cmds = argv + 3;
    ncmd = argc - 3;
    map(0x02000000ULL, 0x00100000ULL);
    map(0x10000000ULL, 0x0e000000ULL);
    map(0x80000000ULL, 0x80000000ULL);
    arena = mmap(0, ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    panel = calloc((size_t)UI_W * UI_H, 4);
    seed();
    make_fdt(0x8a000000ULL);
    sb_boot.fdt = 0x8a000000ULL;
    sb_boot.base = 0x92080000ULL;
    sb_boot.image_size = 0x3f9000;
    sb_boot.relocs = 361;
    sb_boot.el = 4;
    sb_boot.sctlr_in = 0x30d00800;
    sb_boot.cpacr_in = 0x300000;
    sb_boot.loader_tag = 0x31304554524c5453ULL;
    sb_boot.loader_entry = 0x92080000ULL;
    sb_log("libsb up (host preview)");
    sb_main();
    return 0;
}
