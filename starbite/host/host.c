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
 *     tour          (alone) the keys-only tour: generates a script that opens every row of every
 *                   tab with Volume and Power only (other tabs through the Dashboard's TABS rows),
 *                   checks each page opened (the panel changed), that every pixel drawn was
 *                   presented, and that holding Power always leads back to the Dashboard. No files
 *                   are written; exit status 0 = every page passed.
 *
 * Register blocks, DRAM windows, the framebuffer, pmsg and the loader's log page are anonymous
 * mappings at their real addresses, seeded with plausible values, so the pages' raw register
 * reads run unchanged. The PMIC, fuel gauge, touch, UFS, store and card are fakes below.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <math.h>
#include <signal.h>
#include <ucontext.h>
#include <time.h>

#include "sbui.h"
#include "ufs.h"
#include "store.h"
#include "sd.h"
#include "catalog.h"
#include "sbui_w.h"

/* ---- fake clock ------------------------------------------------------------------------------ */

static uint64_t now_us = 7ULL * 60 * 1000000 + 123456;   /* "uptime" 7 min */

/* The fake clock also follows real time while the CPU is busy (stress loops, full-screen frames),
 * so a busy loop's minutes are minutes of fake time too; idle waits (sb_udelay) still jump ahead. */
static uint64_t real_last;
static void clock_follow(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t r = (uint64_t)ts.tv_sec * 1000000U + (uint64_t)ts.tv_nsec / 1000U;
    if (real_last && r > real_last)
        now_us += r - real_last;
    real_last = r;
}
uint64_t sb_ticks(void) { clock_follow(); now_us += 1; return now_us * 26U; }
uint64_t sb_freq(void) { return 26000000U; }
uint64_t sb_us(void) { clock_follow(); now_us += 1; return now_us; }
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
void sb_boot_next_clear(void) { printf("host: boot-next cleared\n"); }

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
struct sb_fg_stat sb_fg_stat = { 0, 0, 0, 0, -1 };
int sb_fg_word_checked(uint8_t reg, uint16_t *v, unsigned lo, unsigned hi)
{
    ++sb_fg_stat.reads;
    if (!sb_fg_word(reg, v)) {
        ++sb_fg_stat.failed;
        return 0;
    }
    if (lo < hi && (*v < lo || *v > hi)) {
        ++sb_fg_stat.rejected;
        return 0;
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

/* ---- fake block device: written blocks kept, the rest generated -------------------------------- */

#define WB_MAX 8192
static struct wblk {
    unsigned med, lu;
    uint64_t lba;
    uint8_t *data;
} wb[WB_MAX];
static int wb_n;

static struct wblk *wb_find(unsigned med, unsigned lu, uint64_t lba)
{
    for (int i = 0; i < wb_n; ++i)
        if (wb[i].med == med && wb[i].lu == lu && wb[i].lba == lba)
            return &wb[i];
    return 0;
}

int sb_blk_read(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, void *dst)
{
    uint8_t *d = dst;
    sb_udelay(n * 40U);                 /* about 100 MB/s of fake time */
    for (uint64_t k = 0; k < n; ++k, d += 4096) {
        struct wblk *w = wb_find(medium, lu, lba + k);
        if (w) {
            memcpy(d, w->data, 4096);
        } else if (medium == SB_MED_UFS && !lu) {
            ufs_read(lba + k, 1);
            memcpy(d, (void *)UFS_BUF, 4096);
        } else {
            for (int i = 0; i < 4096; i += 8)
                *(uint64_t *)(d + i) = (lba + k) * 0x9e3779b97f4a7c15ULL ^ (uint64_t)i ^ ((uint64_t)lu << 56);
        }
    }
    return 0;
}

int sb_blk_write(unsigned medium, unsigned lu, uint64_t lba, uint64_t n, const void *src)
{
    if (!sb_fence_active())
        return -6;                      /* as the drivers: only from inside sb_write */
    const uint8_t *s = src;
    for (uint64_t k = 0; k < n; ++k, s += 4096) {
        struct wblk *w = wb_find(medium, lu, lba + k);
        if (!w) {
            if (wb_n >= WB_MAX)
                return -1;
            w = &wb[wb_n++];
            w->med = medium;
            w->lu = lu;
            w->lba = lba + k;
            w->data = malloc(4096);
        }
        memcpy(w->data, s, 4096);
    }
    return 0;
}

/* ---- fake OS store: the real catalog, fixed entries, and what the store writer appends --------- */

static const char *store_names[] = {
    "catalog", "ios16.uefi", "mac13-v33.xnu", "ios26-sd.xnu", "windows.uefi", "android.kernel",
    "android.ramdisk", "android.cmdline", "linux.image", "linux.dtb", "linux.initrd", "engineer.img",
    "uefishell.uefi",
};
#define NFIX (sizeof(store_names) / sizeof(store_names[0]))
static uint8_t *catalog_blob;
static uint64_t catalog_size;
static struct store_ent added[64];
static unsigned nadded, removed_mask;
static uint64_t st_used = 412931;
#define ST_BASE 16903808ULL
#define ST_CAP 721024ULL

int store_open(unsigned m)
{
    if (m != STORE_UFS)
        return 0;
    sb_fence_store(STORE_UFS, ST_BASE, st_used, ST_CAP);
    unsigned n = 0;
    struct store_ent e;
    while (store_get(m, n, &e))
        ++n;
    return (int)n;
}
int store_version(unsigned m) { return m == STORE_UFS ? 2 : 0; }
static int raw_get(unsigned i, struct store_ent *e)
{
    if (i < NFIX) {
        memset(e, 0, sizeof(*e));
        sb_strcpy(e->name, 32, store_names[i]);
        e->lba = ST_BASE + 7 + i * 4096U;
        e->size = i == 0 ? catalog_size : (uint64_t)(1 + i * 7) * 1048576U + i * 4321U;
        e->crc = 0x1234abcdU * (i + 1);
        e->type = i == 0 ? ST_CATALOG : ST_DATA;
        return 1;
    }
    if (i < NFIX + nadded) {
        *e = added[i - NFIX];
        return 1;
    }
    return 0;
}
int store_get(unsigned m, unsigned i, struct store_ent *e)
{
    if (m != STORE_UFS)
        return 0;
    for (unsigned r = 0, k = 0; raw_get(r, e); ++r) {
        if (r < 32 && (removed_mask & (1U << r)))
            continue;
        if (k++ == i)
            return 1;
    }
    return 0;
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
uint64_t store_base(unsigned m) { return m ? 0 : ST_BASE; }
uint64_t store_capacity(unsigned m) { return m ? 0 : ST_CAP; }
uint64_t store_used(unsigned m) { return m ? 0 : st_used; }
void store_reset(unsigned m) { (void)m; }
uint64_t store_free(unsigned m) { return m ? 0 : ST_CAP - st_used - 1033; }
uint32_t store_crc32(const void *p, uint64_t n) { return sb_crc32(p, n); }

/* The writer goes through the real fence (sb_write_store): the preview checks the gate. */
int store_wr_begin(struct store_writer *w, unsigned m, const char *name, unsigned type, uint64_t size)
{
    struct store_ent e;
    memset(w, 0, sizeof(*w));
    w->m = m;
    w->type = type;
    w->size = size;
    sb_strcpy(w->name, sizeof(w->name), name);
    if (m != STORE_UFS)
        return w->err = -1;
    if (store_find(name, &e))
        return w->err = -3;
    if ((size + 4095) / 4096 > store_free(m))
        return w->err = -5;
    w->at = ST_BASE + st_used;
    w->crc = 0;
    return 0;
}
int store_wr_chunk(struct store_writer *w, const void *src, uint64_t bytes)
{
    if (w->err)
        return w->err;
    static uint8_t pad[4096];
    uint64_t full = bytes / 4096, lba = w->at + w->done / 4096;
    int r = full ? sb_write_store(w->m, lba, full, src) : 0;
    if (!r && (bytes & 4095)) {
        memset(pad, 0, sizeof(pad));
        memcpy(pad, (const uint8_t *)src + full * 4096, bytes & 4095);
        r = sb_write_store(w->m, lba + full, 1, pad);
    }
    if (r)
        return w->err = r;
    w->done += bytes;
    return 0;
}
int store_wr_commit(struct store_writer *w, struct store_ent *out)
{
    static uint8_t hdr[4096];
    if (w->err)
        return w->err;
    int r = sb_write_store(w->m, ST_BASE + 1, 1, hdr);           /* the header commit */
    if (r)
        return w->err = r;
    struct store_ent *e = &added[nadded < 63 ? nadded++ : 63];
    memset(e, 0, sizeof(*e));
    sb_strcpy(e->name, 32, w->name);
    e->lba = w->at;
    e->size = w->size;
    e->type = (uint16_t)w->type;
    e->crc = 0x5eed0000U + nadded;
    st_used += (w->size + 4095) / 4096;
    sb_fence_store(STORE_UFS, ST_BASE, st_used, ST_CAP);
    if (out)
        *out = *e;
    printf("host: store append %s (%llu bytes)\n", w->name, (unsigned long long)w->size);
    return 0;
}
int store_append(unsigned m, const char *name, unsigned type, const void *src, uint64_t size, struct store_ent *out)
{
    struct store_writer w;
    int r = store_wr_begin(&w, m, name, type, size);
    if (!r)
        r = store_wr_chunk(&w, src, size);
    if (!r)
        r = store_wr_commit(&w, out);
    return r;
}
int store_dir_range(unsigned m, uint64_t *first, uint64_t *count)
{
    if (m)
        return 0;
    *first = ST_BASE + 1;
    *count = 6;
    return 1;
}
static int dir_edit(unsigned m, unsigned idx, const char *nn)
{
    static uint8_t blk[6 * 4096];
    struct store_ent e;
    if (m || !store_get(m, idx, &e))
        return -2;
    int r = sb_write(SB_MED_UFS, 0, ST_BASE + 3, 4, blk);
    if (!r)
        r = sb_write(SB_MED_UFS, 0, ST_BASE + 1, 1, blk);
    if (r)
        return r;
    for (unsigned raw = 0, k = 0; raw_get(raw, &e); ++raw) {
        if (raw < 32 && (removed_mask & (1U << raw)))
            continue;
        if (k++ != idx)
            continue;
        if (nn && raw >= NFIX)
            sb_strcpy(added[raw - NFIX].name, 32, nn);
        else if (!nn && raw < 32)
            removed_mask |= 1U << raw;
        break;
    }
    return 0;
}
int store_remove(unsigned m, unsigned idx) { return dir_edit(m, idx, 0); }
int store_rename(unsigned m, unsigned idx, const char *n) { return dir_edit(m, idx, n); }
int store_verify(unsigned m, unsigned i, uint32_t *got)
{
    struct store_ent e;
    if (!store_get(m, i, &e))
        return -1;
    *got = e.crc;
    return 0;
}

static struct star_settings host_settings = { 41, 0, 0, "mac13", "mac13", "" };
int settings_load(struct star_settings *s)
{
    *s = host_settings;
    return 1;
}
int settings_save(struct star_settings *s)
{
    static uint8_t blk[4096];
    int r = sb_write_store(STORE_UFS, ST_BASE + 2, 1, blk);
    if (r)
        return r;
    s->seq++;
    host_settings = *s;
    printf("host: settings saved (default %s, timeout %u, flags %#x)\n", s->default_key, s->timeout_s, s->flags);
    return 0;
}

/* ---- PMIC / MAX77705 writes through the gate --------------------------------------------------- */

int sb_pmic_write(uint32_t type, uint32_t reg, uint8_t val, int tier)
{
    const char *why;
    if (type > 2 || reg > 255 || sb_gate_check(SB_SP_PMIC, 0, (uint64_t)type << 8 | reg, 1, tier < 1 ? 1 : tier, &why))
        return 0;
    pmic[type][reg] = val;
    return 1;
}
static void w32(uint64_t a, uint32_t v);
static uint32_t pll(unsigned m, unsigned p, unsigned s);
/* ACPM DVFS: domain 4 sets the fake PLL_G3D (p 4, s 2: 26 MHz x m / 16) */
int sb_acpm_dvfs(uint32_t domain, uint32_t khz, int tier)
{
    const char *why;
    if (sb_gate_check(SB_SP_DVFS, 0, domain, 1, tier < 3 ? 3 : tier, &why))
        return 0;
    if (domain == 4)
        w32(0x17400140, pll(khz * 16U / 26000U, 4, 2));
    sb_log(domain == 4 ? "acpm dvfs domain 4 (host)" : "acpm dvfs (host)");
    return 1;
}
/* tier 4 attempts: the preview cannot map anything (the fake probe faults unmapped addresses) */
int sb_mmu_map_temp(uint64_t pa) { (void)pa; return -1; }
void sb_mmu_unmap_temp(void) {}

/* ---- the Mali-G72 (libsb mali.c calls host_gpu_write after every register write) ----------------
 * Registers are memory at 0x17500000; this fake reacts to the writes that start something: a soft
 * reset, the power-on / -off masks, the IRQ clears, a job slot's START. A job chain is walked
 * through the real AS0 page tables mali.c wrote (Mali-LPAE: tables | 3, level-3 pages | 1), so a
 * wrong table or VA faults here as on the GPU; WRITE_VALUE and clear-only FRAGMENT jobs run. */
#define HG(o) (*(volatile uint32_t *)(0x17500000UL + (o)))
static int hg_cycles_on;
static uint64_t hg_cyc_t0;
static uint64_t hg_fault_va;

static void *hg_va(uint64_t va)
{
    uint64_t t = ((uint64_t)HG(0x2404) << 32 | HG(0x2400)) & 0xfffffffff000ULL;
    if (!t)
        return 0;
    for (int lvl = 0; lvl < 4; ++lvl) {
        unsigned idx = (unsigned)(va >> (39 - 9 * lvl)) & 0x1ffU;
        uint64_t e = ((volatile uint64_t *)(uintptr_t)t)[idx];
        if (lvl < 3) {
            if ((e & 3U) != 3U)
                return 0;
            t = e & 0xfffffffff000ULL;
        } else {
            if ((e & 3U) != 1U)
                return 0;
            return (void *)(uintptr_t)((e & 0xfffffffff000ULL) | (va & 0xfffU));
        }
    }
    return 0;
}

static int hg_fill(uint64_t base, uint32_t stride, unsigned w, unsigned h, uint32_t c)
{
    for (unsigned y = 0; y < h; ++y) {
        uint64_t va = base + (uint64_t)y * stride, end = va + (uint64_t)w * 4U;
        while (va < end) {
            uint32_t *p = hg_va(va);
            if (!p) {
                hg_fault_va = va;
                return 0;
            }
            uint64_t n = 0x1000U - (va & 0xfffU);
            if (n > end - va)
                n = end - va;
            for (uint64_t i = 0; i < n / 4U; ++i)
                p[i] = c;
            va += n;
        }
    }
    return 1;
}

static void hg_run(unsigned js)
{
    unsigned s = 0x1800U + js * 0x80U;
    uint64_t va = (uint64_t)HG(s + 0x44) << 32 | HG(s + 0x40);
    int fault = 0;
    for (unsigned n = 0; va && n < 100000U && !fault; ++n) {
        volatile uint32_t *j = hg_va(va);
        if (!j) {
            hg_fault_va = va;
            fault = 1;
            break;
        }
        unsigned type = (j[4] >> 1) & 0x7fU;
        if (type == 2) {
            uint64_t a = j[8] | (uint64_t)j[9] << 32, imm = j[12] | (uint64_t)j[13] << 32;
            uint8_t *d = hg_va(a);
            if (!d) {
                hg_fault_va = a;
                fault = 1;
                break;
            }
            uint64_t val = j[10] == 1 ? (now_us - hg_cyc_t0) * 260U : j[10] == 2 ? sb_ticks() : j[10] == 3 ? 0 : imm;
            unsigned bytes = j[10] == 4 ? 1 : j[10] == 5 ? 2 : j[10] == 6 ? 4 : 8;
            memcpy(d, &val, bytes);
        } else if (type == 9) {
            uint64_t fbd = (j[10] | (uint64_t)j[11] << 32) & ~63ULL;
            volatile uint32_t *f = hg_va(fbd);
            if (!f) {
                hg_fault_va = fbd;
                fault = 1;
                break;
            }
            unsigned w = (f[8] & 0xffffU) + 1U, h = (f[8] >> 16) + 1U;
            unsigned tw = ((j[9] & 0xfffU) + 1U) * 16U, th = (((j[9] >> 16) & 0xfffU) + 1U) * 16U;
            if (!hg_fill(f[40] | (uint64_t)f[41] << 32, f[42], w < tw ? w : tw, h < th ? h : th, f[44])) {
                fault = 1;
                break;
            }
        } else if (type != 1) {
            HG(s + 0x24) = 0x40;                        /* JOB_CONFIG_FAULT */
            HG(0x1000) |= 1U << (js + 16);
            return;
        }
        j[0] = 1;                                       /* exception status: DONE */
        va = j[6] | (uint64_t)j[7] << 32;
    }
    if (fault) {                                        /* an MMU translation fault stalls the slot */
        HG(0x2000) |= 1U;
        HG(0x241c) = 0xc2U;
        HG(0x2420) = (uint32_t)hg_fault_va;
        HG(0x2424) = (uint32_t)(hg_fault_va >> 32);
        HG(s + 0x24) = 0x08;
        return;
    }
    HG(s + 0x24) = 1;
    HG(0x1000) |= 1U << js;
}

void host_gpu_write(unsigned off, uint32_t v);
void host_gpu_write(unsigned off, uint32_t v)
{
    switch (off) {
    case 0x024: HG(0x020) &= ~v; break;
    case 0x030:
        if (v == 1) {
            HG(0x020) |= 0x100U;
            HG(0x140) = HG(0x150) = HG(0x160) = 0;
            HG(0x1000) = HG(0x2000) = 0;
        } else if (v == 5) {
            hg_cycles_on = 1;
            hg_cyc_t0 = now_us;
        } else if (v == 6) {
            hg_cycles_on = 0;
        }
        break;
    case 0x180: HG(0x140) |= v; break;
    case 0x190: HG(0x150) |= v; break;
    case 0x1a0: HG(0x160) |= v; break;
    case 0x1c0: HG(0x140) &= ~v; break;
    case 0x1d0: HG(0x150) &= ~v; break;
    case 0x1e0: HG(0x160) &= ~v; break;
    case 0x1004: HG(0x1000) &= ~v; break;
    case 0x2004: HG(0x2000) &= ~v; break;
    default:
        if (off >= 0x1800 && off < 0x2000 && (off & 0x7fU) == 0x60 && v == 1)
            hg_run((off - 0x1800U) / 0x80U);
        break;
    }
    /* the job interrupt line at the GIC: GICD_ISPENDR9 bit 26 (SPI 282) while masked-in and raised */
    volatile uint32_t *pend = (volatile uint32_t *)0x10101224UL;
    *pend = (*pend & ~(1U << 26)) | ((HG(0x1000) & HG(0x1008)) ? 1U << 26 : 0);
}

static void host_gpu_tick(void)
{
    uint64_t c = hg_cycles_on ? (now_us - hg_cyc_t0) * 260U : 0, ts = now_us * 26U;
    HG(0x90) = (uint32_t)c;
    HG(0x94) = (uint32_t)(c >> 32);
    HG(0x98) = (uint32_t)ts;
    HG(0x9c) = (uint32_t)(ts >> 32);
}

static uint8_t chg_regs[256];
int sb_max_write(unsigned addr7, uint8_t reg, uint8_t val, int tier)
{
    const char *why;
    if (sb_gate_check(SB_SP_MAX, 0, (uint64_t)addr7 << 8 | reg, 1, tier < 1 ? 1 : tier, &why))
        return 0;
    if (addr7 == 0x69)
        chg_regs[reg] = val;
    return 1;
}

/* ---- the CMGP ADC (libsb adc.c asks host_adc under SB_HOST) ---------------------------------- */

static int host_load;                   /* worker cores running: the fake temperatures rise */
/* The phone's v2.0 glitch reproduced: every 5th conversion reads raw 0 (a conversion read before
 * it finished). The filter must reject it: unfiltered, the battery thermistor would read 90 C and
 * stop the Lab tour's battery tests at the 45 C guard. */
static unsigned host_adc_n;
int host_adc(unsigned ch)
{
    if (++host_adc_n % 5U == 0)
        return 0;
    switch (ch) {
    case 4: return 1950 - host_load * 25;   /* battery ~26 C, warmer under load */
    case 0: return 1700 - host_load * 40;
    case 1: return 1900;
    case 2: return 1800;
    case 10: return 0x800;
    case 3: return 1880;
    default: return -1;
    }
}

/* ---- the other cores: threads ------------------------------------------------------------------- */

#include <pthread.h>
#include <sched.h>
static struct sb_wslot hslot[SB_NCORES];
static volatile int hstop;
static pthread_t hth[SB_NCORES];
static int hth_on[SB_NCORES];
static sb_work_fn hfn[SB_NCORES];
static void *harg[SB_NCORES];
sb_m3_prep_fn sb_smp_m3_prep;

static void *hrun(void *p)
{
    unsigned w = (unsigned)(uintptr_t)p;
    hslot[w].mpidr = 0x81000000ULL | (uint64_t)w << 8;
    hslot[w].state = SB_WK_RUNNING;
    hfn[w](w, harg[w]);
    hslot[w].state = SB_WK_DONE;
    return 0;
}
uint64_t sb_smp_target(unsigned w) { return w < 4 ? w : 0x100U + (w - 4U); }
const struct sb_wslot *sb_smp_slot(unsigned w) { return w < SB_NCORES ? &hslot[w] : 0; }
struct sb_wslot *sb_smp_own_slot(unsigned w) { return w < SB_NCORES ? &hslot[w] : 0; }
int64_t sb_smp_start(unsigned w, sb_work_fn fn, void *arg)
{
    if (w == 0 || w >= SB_NCORES || !fn)
        return -2;
    if (w >= 4) {
        if (sb_smp_m3_prep)
            sb_smp_m3_prep();
        return -3;                      /* the preview has no M3 cores: PSCI DENIED */
    }
    if (hslot[w].state == SB_WK_RUNNING)
        return -4;
    if (hth_on[w])
        pthread_join(hth[w], 0);
    memset(&hslot[w], 0, sizeof(hslot[w]));
    hfn[w] = fn;
    harg[w] = arg;
    hstop = 0;
    hth_on[w] = !pthread_create(&hth[w], 0, hrun, (void *)(uintptr_t)w);
    while (hth_on[w] && hslot[w].state == SB_WK_IDLE)
        sched_yield();
    ++host_load;
    return hth_on[w] ? 0 : -100;
}
unsigned sb_smp_running(void)
{
    unsigned n = 0;
    for (unsigned w = 1; w < SB_NCORES; ++w)
        n += hslot[w].state == SB_WK_RUNNING;
    return n;
}
unsigned sb_smp_stop(unsigned ms)
{
    (void)ms;
    hstop = 1;
    for (unsigned w = 1; w < SB_NCORES; ++w)
        if (hth_on[w]) {
            pthread_join(hth[w], 0);
            hth_on[w] = 0;
        }
    host_load = 0;
    return 0;
}
int sb_worker_stop(void) { return hstop; }
void sb_worker_publish(struct sb_wslot *s) { s->beats++; }

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
int sd_write(uint64_t s, uint32_t c, uintptr_t d) { (void)s; (void)c; (void)d; return sb_fence_active() ? -1 : -9; }
int sd_write4k(uint64_t l, uint64_t c, uintptr_t d) { (void)l; (void)c; (void)d; return sb_fence_active() ? -1 : -9; }
uint64_t sd_sectors_hint(void) { return 249999360ULL; }

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

/* ---- the keys-only tour ---------------------------------------------------------------------- */

#include "eng.h"

static int tour_mode, tour_ready, tour_pages, tour_fail, tour_tab, lab_mode;
static uint64_t tour_root_hash;

static uint64_t panel_hash(void)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < (size_t)UI_W * UI_H; i += 7)
        h = (h ^ (panel[i] & 0xffffffU)) * 1099511628211ULL;
    return h;
}

static uint64_t panel_vs_back(void)
{
    uint64_t diff = 0;
    const uint32_t *bb = ui_back();
    for (size_t i = 0; bb && i < (size_t)UI_W * UI_H; ++i)
        diff += (panel[i] & 0xffffffU) != (bb[i] & 0xffffffU);
    return diff;
}

static int scratch_tab;
static void scratch_build(void) { tab_build(scratch_tab); }

/* Rows per tab, counted on a scratch page (the back buffer is saved and restored around it). */
static int tab_rows(int t)
{
    extern struct page *host_last_page;
    struct page *keep_last = host_last_page;
    size_t n = (size_t)UI_W * UI_H;
    uint32_t *save = malloc(n * 4);
    memcpy(save, ui_back(), n * 4);
    struct ui_rect clip = ui_get_clip();
    struct page p;
    page_init(&p, UI_VIEW_Y0, UI_VIEW_Y1);
    scratch_tab = t;
    eng_build_page(&p, scratch_build);
    int rows = p.nrows;
    page_free(&p);
    ui_set_clip(clip);
    memcpy(ui_back(), save, n * 4);
    free(save);
    host_last_page = keep_last;
    return rows;
}

static char **tl;
static int tn, tcap;
static void tadd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tadd(const char *fmt, ...)
{
    char b[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (tn == tcap) {
        tcap = tcap ? 2 * tcap : 1024;
        tl = realloc(tl, sizeof(*tl) * (size_t)tcap);
    }
    tl[tn++] = strdup(b);
}

#define TOUR_HUB 5          /* Dashboard row of tab 1 (tabs.c: rows 5..11 = tabs 1..7) */
#define TOUR_BACK "h:p:1700"
#define TOUR_TOP "h:u:2200"         /* 1 + (2200 - 420) / 90 = 20 rows up */

static void tour_generate(void)
{
    tadd("w:800");
    for (int t = 0; t < NTABS; ++t) {
        int rows = tab_rows(t);
        printf("tour: tab %d %s: %d rows\n", t, tab_title_of(t), rows);
        for (int r = 0; r < rows; ++r) {
            if (t == 0 && r >= TOUR_HUB && r < TOUR_HUB + NTABS - 1)
                continue;                               /* the hub rows: the tour's own path */
            /* Volume Up held (auto-repeat) puts the focus on the first row of any root, wherever
             * it was; then Volume Down counts rows: the Dashboard's TABS row, the tab's row. */
            if (t) {
                tadd(TOUR_TOP);
                for (int k = 0; k < TOUR_HUB + t - 1; ++k)
                    tadd("k:d");
                tadd("k:p");
                tadd("w:500");
            }
            tadd("R:%d", t);
            tadd(TOUR_TOP);
            for (int k = 0; k < r; ++k)
                tadd("k:d");
            tadd("k:p");
            tadd("w:1500");
            tadd("x:%d.%d", t, r);
            for (int k = 0; k < 4; ++k)
                tadd(TOUR_BACK);
            tadd("w:300");
            tadd("H");
        }
    }
    tadd("Q");
    cmds = tl;
    ncmd = tn;
    ci = 0;
}

/* ---- the Lab tour: every Lab test started with keys and stopped with Power held -------------------
 * Each test is opened from the Tests tab, its options set, started; after a while Power is held
 * and the result page must show (the test stopped); then Power held leads back to the Dashboard.
 * Also: a tier 1 write end to end (Save report: warning, Continue, slide), the M3 offer and its PMU
 * warning cancelled inside the health check, an unconfirmed write refused by the fence, the "no
 * limit" warning cancelled, the GPU tests (power-up, identify, job manager, fill, stress, power
 * down) on the fake Mali, and a tier 4 write up to the red screen, cancelled. */
static void lt_tab(int t)
{
    tadd(TOUR_TOP);
    for (int k = 0; k < TOUR_HUB + t - 1; ++k)
        tadd("k:d");
    tadd("k:p");
    tadd("w:600");
}

static void lt_row(int r)
{
    tadd(TOUR_TOP);
    for (int k = 0; k < r; ++k)
        tadd("k:d");
    tadd("k:p");
    tadd("w:900");
}

static void lt_home(int holds)
{
    for (int k = 0; k < holds; ++k)
        tadd(TOUR_BACK);
    tadd("w:300");
    tadd("H");
}

/* start (row), run ms, stop with Power, expect the result title */
static void lt_run(const char *name, int start_row, unsigned ms, const char *result_title, int holds)
{
    lt_row(start_row);
    tadd("w:%u", ms);
    tadd("D:2");
    tadd("T:%s", name);
    tadd("x:lab.%s", name);
    tadd(TOUR_BACK);
    tadd("w:1500");
    tadd("T:%s", result_title);
    tadd("x:lab.result");
    lt_home(holds);
}

static void labtour_generate(void)
{
    tadd("w:800");
    /* CPU stress on the A55 cores (row 0 cycles the cores option once: all 8 -> the 4 A55) */
    lt_tab(1);
    lt_row(1);
    tadd("T:CPU stress");
    lt_row(0);
    lt_run("CPU stress", 4, 6000, "CPU stress result", 3);
    tadd("L:test CPU stress: PASS");
    /* memory stress, passes until stopped (row 1 three times: 1 -> 3 -> 10 -> until stopped) */
    lt_tab(1);
    lt_row(2);
    lt_row(1);
    lt_row(1);
    lt_row(1);
    lt_run("Memory stress", 2, 3000, "Memory stress result", 3);
    tadd("L:test Memory stress: PASS");
    /* thermal test: 10 s baseline, then the load; stopped during the load */
    lt_tab(1);
    lt_row(3);
    lt_run("Thermal test", 5, 16000, "Thermal test result", 3);
    tadd("L:test Thermal test");
    /* battery: discharge (all A55 cores + white screen), then the sag test */
    lt_tab(1);
    lt_row(4);
    lt_run("Discharge test", 5, 5000, "Discharge result", 3);
    lt_tab(1);
    lt_row(4);
    lt_run("Sag test", 6, 25000, "Sag result", 3);
    tadd("L:test Battery sag");
    /* storage benchmark and verify */
    lt_tab(1);
    lt_row(5);
    lt_row(1);
    lt_row(1);
    lt_run("Storage test", 3, 1500, "Storage test result", 3);
    tadd("L:test Storage test (UFS): PASS");
    /* display stress and the touch grid: full-screen, stopped by Power held, back at the Tests root */
    lt_tab(1);
    lt_row(6);
    tadd("w:2500");
    tadd(TOUR_BACK);
    tadd("w:800");
    tadd("D:0");
    tadd("L:test Display stress: FAIL");
    lt_home(2);
    lt_tab(1);
    lt_row(7);
    tadd("w:1500");
    tadd(TOUR_BACK);
    tadd("w:800");
    tadd("D:0");
    tadd("L:test Touch grid: FAIL");
    lt_home(2);
    /* the full health check: it offers the M3 cores first; "enable" leads to the tier 3 PMU warning,
     * cancelled with Power held: the check runs on the A55 cores */
    lt_tab(1);
    lt_row(0);
    lt_row(0);
    tadd("w:600");
    tadd("T:Use the M3 cores?");
    tadd("x:lab.offer");
    lt_row(0);
    tadd("w:600");
    tadd("T:Before you write");
    tadd(TOUR_BACK);
    tadd("w:22000");
    tadd("T:Health check result");
    tadd("x:lab.health");
    tadd("L:audit: cancelled at the warning: Start the M3 cores");
    tadd("L:test Full health check");
    lt_home(3);
    /* a tier 1 write end to end: Save report -> warning -> Continue -> slide (hold Volume Up) */
    lt_tab(1);
    lt_row(19);
    tadd("T:Before you write");
    tadd("x:lab.warning");
    lt_row(0);
    tadd("h:u:1900");
    tadd("w:2500");
    tadd("L:report saved: ufs:lab-");
    tadd("D:0");
    lt_home(1);
    /* an unconfirmed write: the fence refuses it */
    lt_tab(5);
    lt_row(5);
    lt_row(0);
    tadd("w:1500");
    tadd("L:fence: refused");
    lt_home(2);
    /* "no limit": CPU stress, Stop at cycled 85 -> 90 -> 95 -> the warning, cancelled (back to 70) */
    lt_tab(1);
    lt_row(1);
    lt_row(3);
    lt_row(3);
    lt_row(3);
    tadd("T:Before you write");
    tadd("x:lab.nolimit");
    tadd(TOUR_BACK);
    tadd("w:600");
    tadd("L:audit: cancelled at the warning: No temperature limit");
    lt_home(3);
    /* the GPU tests: power up (tier 1: warning, Continue, slide), identify, the job manager, the fill
     * pattern (on the screen, dismissed with a key), the stress stopped with Power, power down */
    lt_tab(1);
    lt_row(8);
    tadd("T:GPU tests");
    lt_row(0);
    tadd("T:Before you write");
    lt_row(0);
    tadd("h:u:1900");
    tadd("w:1500");
    tadd("L:gpu: session opened");
    tadd("L:test GPU power-up: PASS");
    lt_row(1);
    tadd("w:600");
    tadd("T:GPU identification");
    tadd("x:lab.gpuid");
    tadd("L:test GPU identification: PASS");
    tadd(TOUR_BACK);
    tadd("w:600");
    lt_row(2);
    tadd("w:2500");
    tadd("T:GPU job manager");
    tadd("x:lab.gpujm");
    tadd("L:test GPU job manager: PASS");
    tadd(TOUR_BACK);
    tadd("w:600");
    lt_row(3);
    tadd("w:3000");
    tadd("k:d");
    tadd("w:1500");
    tadd("T:GPU fill pattern");
    tadd("x:lab.gpufill");
    tadd("L:test GPU fill pattern: PASS");
    tadd(TOUR_BACK);
    tadd("w:600");
    lt_row(4);
    tadd("T:GPU stress");
    lt_row(3);
    tadd("w:5000");
    tadd("D:3");
    tadd("T:GPU stress");
    tadd("x:lab.gpustress");
    tadd(TOUR_BACK);
    tadd("w:1500");
    tadd("T:GPU stress result");
    tadd("x:lab.result");
    tadd("L:test GPU stress: PASS");
    tadd(TOUR_BACK);
    tadd("w:600");
    tadd(TOUR_BACK);
    tadd("w:600");
    tadd(TOUR_BACK);
    tadd("w:900");
    tadd("L:GPU powered down, vdd_g3d back");
    lt_home(1);
    /* tier 4 up to the red screen: Register peek / poke at S-Boot's ECT (0x0A0000000): the address
     * (from 0x014060000: digit 1 up 9 to A, digit 2 down 4, digit 4 down 6), Write a word, the value,
     * Arm (hold Power 2 s, slide), then the red warning: Power on its default (Cancel) */
    lt_tab(7);
    lt_row(1);
    tadd("T:Register peek");
    lt_row(0);
    tadd("k:p");
    for (int k = 0; k < 9; ++k)
        tadd("k:u");
    tadd("k:p");
    for (int k = 0; k < 4; ++k)
        tadd("k:d");
    tadd("k:p");
    tadd("k:p");
    for (int k = 0; k < 6; ++k)
        tadd("k:d");
    for (int k = 0; k < 6; ++k)
        tadd("k:p");
    tadd("w:600");
    lt_row(3);
    for (int k = 0; k < 10; ++k)
        tadd("k:p");
    tadd("w:300");
    for (int k = 0; k < 9; ++k)
        tadd("k:p");
    tadd("w:600");
    tadd("k:p");
    tadd("w:600");
    tadd("h:p:2300");
    tadd("w:900");
    tadd("h:u:1900");
    tadd("w:1500");
    tadd("L:audit: armed");
    tadd("L:audit: tier 4 warning shown: Register write A0000000");
    tadd("k:p");
    tadd("w:900");
    tadd("L:audit: cancelled at the red warning: Register write");
    tadd("x:lab.tier4");
    lt_home(3);
    tadd("Q");
    cmds = tl;
    ncmd = tn;
    ci = 0;
}

static unsigned key_of(char c)
{
    return c == 'p' ? SB_K_POWER : c == 'u' ? SB_K_UP : c == 'd' ? SB_K_DOWN : SB_K_BIXBY;
}

static uint64_t key_up_at;
static unsigned key_held;

/* A thermal model for the preview: the TMU codes follow the worker cores' load (one-point trim 50:
 * code = temperature + 25). */
static double host_temp = 41.0;
static uint64_t host_temp_t;
static void w32(uint64_t a, uint32_t v);

static void host_thermal(void)
{
    if (now_us - host_temp_t < 200000)
        return;
    double dt = (double)(now_us - host_temp_t) / 1e6;
    host_temp_t = now_us;
    double target = 41.0 + 9.0 * host_load, k = dt / 25.0;
    host_temp += (target - host_temp) * (k > 1 ? 1 : k);
    int t = (int)host_temp;
    int top[8] = { t - 4, t + 3, t + 2, t + 4, t + 1, t - 2, t, t - 3 }, sub[3] = { t - 5, t - 3, t - 7 };
    w32(0x10070040, (uint32_t)(top[0] + 25) | (uint32_t)(top[1] + 25) << 9);
    w32(0x10070044, (uint32_t)(top[2] + 25) | (uint32_t)(top[3] + 25) << 9 | (uint32_t)(top[4] + 25) << 18);
    w32(0x10070048, (uint32_t)(top[5] + 25) | (uint32_t)(top[6] + 25) << 9 | (uint32_t)(top[7] + 25) << 18);
    w32(0x10074040, (uint32_t)(sub[0] + 25) | (uint32_t)(sub[1] + 25) << 9);
    w32(0x10074044, (uint32_t)(sub[2] + 25));
}

static void host_step(void)
{
    if (in_step)
        return;
    in_step = 1;
    host_thermal();
    host_gpu_tick();
    if (key_held && now_us >= key_up_at) {
        keys_now &= ~key_held;
        key_held = 0;
    }
    if (tour_mode && !tour_ready) {
        if (ui_back() && ui_stats.frames >= 2 && E.tab_req < 0) {
            tour_ready = 1;
            if (lab_mode)
                labtour_generate();
            else
                tour_generate();
        } else {
            in_step = 0;
            return;
        }
    }
    while (ci < ncmd && now_us >= next_t && !key_held) {
        const char *c = cmds[ci++];
        if (c[0] == 'T' || c[0] == 'D' || c[0] == 'L') {
            const char *title = E.depth > 0 && E.depth <= 8 ? E.titles[E.depth - 1] : tab_title_of(cur_tab);
            int ok = 1;
            if (c[0] == 'T')
                ok = strstr(title, c + 2) != 0;
            else if (c[0] == 'D')
                ok = E.depth == atoi(c + 2);
            else {
                ok = 0;
                for (unsigned i = 0; i < sb_log_count() && !ok; ++i)
                    ok = strstr(sb_log_line(i), c + 2) != 0;
            }
            printf("check %-44s %s  (at \"%s\", depth %d)\n", c, ok ? "ok" : "FAIL", title, E.depth);
            tour_fail += !ok;
            continue;
        }
        if (c[0] == 'R') {
            tour_tab = atoi(c + 2);
            tour_root_hash = panel_hash();
            if (cur_tab != tour_tab || E.depth != 0) {
                printf("tour FAIL: wanted the root of tab %d, at tab %d depth %d\n", tour_tab, cur_tab, E.depth);
                ++tour_fail;
            }
            continue;
        }
        if (c[0] == 'x') {
            uint64_t d = panel_vs_back(), h = panel_hash();
            const char *title = E.depth > 0 && E.depth <= 8 ? E.titles[E.depth - 1] : "(full screen)";
            int bad = d != 0 || h == tour_root_hash;
            printf("tour %-6s %-4s depth %d  %-34s panel vs back buffer %llu px%s\n", c + 2, bad ? "FAIL" : "ok",
                   E.depth, title, (unsigned long long)d, h == tour_root_hash ? "  (nothing opened)" : "");
            tour_fail += bad;
            ++tour_pages;
            continue;
        }
        if (c[0] == 'H') {
            if (cur_tab != 0 || E.depth != 0) {
                printf("tour FAIL: holding Power did not lead back to the Dashboard (tab %d depth %d)\n", cur_tab,
                       E.depth);
                ++tour_fail;
            }
            continue;
        }
        if (c[0] == 'Q') {
            printf("tour: %d pages opened with Volume and Power only, %d failures\n", tour_pages, tour_fail);
            exit(tour_fail ? 4 : 0);
        }
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
    w32(0x10070050, 140U << 16);           /* THD_TEMP0 rise: 115 C, as S-Boot arms it */
    int top[8] = { 37, 44, 43, 45, 42, 39, 41, 38 }, sub[3] = { 36, 38, 34 };
    w32(0x10070040, (uint32_t)(top[0] + 25) | (uint32_t)(top[1] + 25) << 9);
    w32(0x10070044, (uint32_t)(top[2] + 25) | (uint32_t)(top[3] + 25) << 9 | (uint32_t)(top[4] + 25) << 18);
    w32(0x10070048, (uint32_t)(top[5] + 25) | (uint32_t)(top[6] + 25) << 9 | (uint32_t)(top[7] + 25) << 18);
    w32(0x10074040, (uint32_t)(sub[0] + 25) | (uint32_t)(sub[1] + 25) << 9);
    w32(0x10074044, (uint32_t)(sub[2] + 25));
    /* clocks: A55 on its PLL (1794 MHz), shared PLLs, MIF, G3D */
    w32(0x1d000140, pll(138, 2, 0));
    w32(0x17400140, pll(160, 4, 2));        /* PLL_G3D: S-Boot's 260 MHz */
    w32(0x1b800120, pll(276, 2, 0));
    w32(0x1a240120, pll(123, 2, 0));
    w32(0x1a2401a0, pll(144, 2, 1));
    w32(0x1a240220, pll(103, 2, 0));
    w32(0x1a2402a0, pll(92, 3, 0));
    w32(0x1a240320, pll(76, 2, 0));
    /* Mali-G72 MP18 as the phone reported it (NOTES.md 2026-10-28); job slot features assumed */
    w32(0x17500000, 0x62210010U);
    w32(0x17500004, 0x07120206U);
    w32(0x1750000c, 0x00000809U);
    w32(0x17500010, 0x00000001U);
    w32(0x17500014, 0x00002830U);
    w32(0x17500018, 0x000000ffU);
    w32(0x1750001c, 0x00000007U);
    w32(0x175000a0, 384U);
    w32(0x175000c0, 0x0000020eU);
    w32(0x175000c4, 0x000001feU);
    w32(0x175000c8, 0x0000007eU);
    w32(0x17500100, 0x05771777U);
    w32(0x17500110, 1U);
    w32(0x17500120, 1U);
    w32(0x17500020, 0x100U);
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
    pmic[1][0x2b] = 0x37;                   /* vdd_g3d as S-Boot leaves it: 643.75 mV */
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
    setvbuf(stdout, 0, _IOLBF, 0);
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
    lab_mode = ncmd == 1 && !strcmp(cmds[0], "labtour");
    tour_mode = ncmd == 1 && (!strcmp(cmds[0], "tour") || lab_mode);
    if (tour_mode)
        ncmd = 0;
    map(0x02000000ULL, 0x00100000ULL);
    map(0x10000000ULL, 0x0e000000ULL);
    map(0x80000000ULL, 0x80000000ULL);
    map(SB_GPU_MEM, SB_GPU_MEM_SIZE);       /* the GPU tests' memory in bank 2 */
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
