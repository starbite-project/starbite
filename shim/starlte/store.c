/* SPDX-License-Identifier: GPL-2.0-only
 * The Starbite OS store: see store.h. Read only, except the home store's settings record and crash
 * log (the UFS store, or the card's once it is home: docs/STORE-ON-CARD.md).
 */
#include <stdint.h>
#include "store.h"
#include "ufs.h"
#include "sd.h"
#include "sbfmt.h"

#define UFS_STORE_OFFSET 4194304ULL            /* USERDATA +16 GiB, in 4 KiB blocks */
#define V1_BLOCKS        262144ULL             /* the version 1 store: 1 GiB */
#define SCRATCH          (UFS_BUF + (512U << 10))
#define READBACK         (SCRATCH + (64U << 10))   /* write read-backs (the directory uses SCRATCH) */

struct medium {
    int state;                 /* 0 not tried, 1 open, -1 none */
    int version;
    int home;                  /* the header's HOME flag (SBSTHOME extension) */
    uint64_t base, capacity, used, settings;
    unsigned count;
    struct store_ent ent[STORE_MAX];
};
static struct medium med[STORE_MEDIA];

static uint64_t le(uintptr_t p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = n; i; --i)
        v = v << 8 | *(volatile uint8_t *)(p + i - 1);
    return v;
}

/* CRC32 (IEEE, reflected, as zlib.crc32) with the ARMv8 CRC32 instructions. */
__attribute__((target("+crc"))) uint32_t star_crc32(uintptr_t p, uint64_t n)
{
    uint32_t c = 0xffffffffU;
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8)
        __asm__("crc32x %w0, %w0, %x1" : "+r"(c) : "r"(*(volatile uint64_t *)(p + i)));
    for (; i < n; ++i)
        __asm__("crc32b %w0, %w0, %w1" : "+r"(c) : "r"((uint32_t)*(volatile uint8_t *)(p + i)));
    return ~c;
}

/* count 4 KiB blocks of a medium to dst (DMA-visible memory). */
static int med_read(unsigned m, uint64_t lba, uint64_t count, uintptr_t dst)
{
    if (m == STORE_UFS)
        return ufs_read_to(lba, count, dst);
    return sd_read4k(lba, count, dst);
}

/* One 4 KiB block from src: UFS (settings / crash log only, by the callers below) or the card
 * (sd.c refuses anything outside the fence store_home opened). */
static int med_write(unsigned m, uint64_t lba, uintptr_t src)
{
    if (m == STORE_UFS)
        return ufs_write_block(lba, src);
    return sd_write4k(lba, 1, src);
}

/* ---- home store state (store.h) -------------------------------------------------------------- */
#ifdef STORE_CARD_ONLY
static int card_only = 1;      /* test loaders chained from the store have no BOOT command line */
#else
static int card_only;
#endif
static int home = -2;          /* -2 not resolved, -3 resolving, -1 none, else the medium */

static void name_copy(char *dst, uintptr_t src, unsigned n)
{
    unsigned k = 0;
    for (; k + 1 < n && *(volatile uint8_t *)(src + k); ++k)
        dst[k] = (char)*(volatile uint8_t *)(src + k);
    for (; k < 32; ++k)
        dst[k] = 0;
}

/* Version 2 at block 1: the header, then its directory into SCRATCH. */
static int open_v2(struct medium *s, unsigned m, uint64_t limit)
{
    if (med_read(m, s->base + 1, 1, SCRATCH) || le(SCRATCH, 8) != 0x3245524f54534253ULL ||  /* "SBSTORE2" */
        le(SCRATCH + 8, 4) != 2 || star_crc32(SCRATCH, 48) != (uint32_t)le(SCRATCH + 48, 4))
        return 0;
    uint32_t n = (uint32_t)le(SCRATCH + 12, 4), dir = (uint32_t)le(SCRATCH + 32, 4),
             dir_blocks = (uint32_t)le(SCRATCH + 36, 4), crc = (uint32_t)le(SCRATCH + 44, 4);
    s->capacity = le(SCRATCH + 16, 8);
    s->used = le(SCRATCH + 24, 8);
    s->settings = le(SCRATCH + 40, 4);
    s->home = le(SCRATCH + 64, 8) == 0x454d4f4854534253ULL &&                       /* "SBSTHOME" */
              le(SCRATCH + 72, 4) == 1 && star_crc32(SCRATCH + 64, 24) == (uint32_t)le(SCRATCH + 88, 4) &&
              (le(SCRATCH + 76, 4) & 1U);
    if (s->capacity > limit || s->used > s->capacity || n > STORE_MAX || !dir_blocks ||
        dir_blocks > STORE_MAX * 64U / 4096U || n * 64U > dir_blocks * 4096U ||
        dir + dir_blocks > s->capacity)
        return 0;
    if (med_read(m, s->base + dir, dir_blocks, SCRATCH) || star_crc32(SCRATCH, n * 64ULL) != crc)
        return 0;
    for (uint32_t i = 0; i < n; ++i) {
        uintptr_t p = SCRATCH + 64U * i;
        struct store_ent *e = &s->ent[s->count];
        uint64_t blk = le(p + 32, 8), size = le(p + 40, 8);
        if (blk > s->capacity || (size + 4095U) / 4096U > s->capacity - blk)
            continue;
        name_copy(e->name, p, 32);
        e->lba = s->base + blk;
        e->size = size;
        e->crc = (uint32_t)le(p + 48, 4);
        e->type = (uint16_t)le(p + 52, 2);
        e->flags = (uint16_t)le(p + 54, 2);
        e->medium = (uint8_t)m;
        ++s->count;
    }
    s->version = 2;
    return 1;
}

/* Version 1 at block 0 (tools/linux/sbstore.py): byte offsets, names of 24, 63 entries. */
static int open_v1(struct medium *s, unsigned m)
{
    if (med_read(m, s->base, 1, SCRATCH) || le(SCRATCH, 8) != 0x3145524f54534253ULL ||   /* "SBSTORE1" */
        le(SCRATCH + 8, 4) != 1)
        return 0;
    uint32_t n = (uint32_t)le(SCRATCH + 12, 4);
    s->capacity = V1_BLOCKS;
    s->used = le(SCRATCH + 16, 8);
    s->settings = 0;
    s->home = 0;
    for (uint32_t i = 0; i < n && i < 63; ++i) {
        uintptr_t p = SCRATCH + 64 + 64U * i;
        struct store_ent *e = &s->ent[s->count];
        uint64_t off = le(p + 24, 8), size = le(p + 32, 8);
        if ((off & 4095U) || off / 4096U + (size + 4095U) / 4096U > V1_BLOCKS)
            continue;
        name_copy(e->name, p, 24);
        e->lba = s->base + off / 4096U;
        e->size = size;
        e->crc = (uint32_t)le(p + 40, 4);
        e->type = ST_DATA;
        e->flags = 0;
        e->medium = (uint8_t)m;
        ++s->count;
    }
    s->version = 1;
    return 1;
}

int store_open(unsigned m)
{
    if (m >= STORE_MEDIA)
        return 0;
    if (home == -2)
        (void)store_home();            /* which media may be touched at all: decided first */
    struct medium *s = &med[m];
    if (s->state)
        return s->state > 0 ? (int)s->count : 0;
    s->state = -1;
    s->count = 0;
    s->home = 0;
    uint64_t first, last, limit;
    if (m == STORE_UFS) {
        /* USERDATA released to iOS (card only), or the card's store is home: the UFS store area
         * is not read, listed or written again (docs/STORE-ON-CARD.md) */
        if (card_only || home == (int)STORE_SD)
            return 0;
        if (!ufs_up() || !gpt_find("USERDATA", &first, &last) || last - first + 1 <= UFS_STORE_OFFSET + 16)
            return 0;
        s->base = first + UFS_STORE_OFFSET;
        limit = last - first + 1 - UFS_STORE_OFFSET;
    } else {
        /* The card hardware is touched only once allowed (sd.h): always in card only mode, else
         * by the UFS settings record (store_home) or EngineerMode's microSD page, which then
         * calls store_reset. */
        if (!sd_allowed())
            return 0;
        if (!sd_store_area(&first, &limit))
            return 0;
        s->base = first;
    }
    if (!open_v2(s, m, limit)) {
        s->count = 0;
        if (m != STORE_UFS || !open_v1(s, m))
            return 0;
    }
    s->state = 1;
    return (int)s->count;
}

void store_reset(unsigned m)
{
    if (m < STORE_MEDIA)
        med[m].state = 0;
}

/* Struct copies would call memcpy: there is no libc. */
static void ent_copy(struct store_ent *d, const struct store_ent *e)
{
    volatile const uint8_t *a = (volatile const uint8_t *)e;
    uint8_t *b = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof(*e); ++i)
        b[i] = a[i];
}

/* 1 if medium m has a store (possibly empty). */
static int opened(unsigned m)
{
    if (m >= STORE_MEDIA)
        return 0;
    (void)store_open(m);
    return med[m].state > 0;
}

int store_version(unsigned m)
{
    return opened(m) ? med[m].version : 0;
}

int store_get(unsigned m, unsigned i, struct store_ent *e)
{
    if (!opened(m) || i >= med[m].count)
        return 0;
    ent_copy(e, &med[m].ent[i]);
    return 1;
}

static int name_is(const char *a, const char *b)
{
    unsigned k = 0;
    while (k < 32 && a[k] && a[k] == b[k])
        ++k;
    return k == 32 || a[k] == b[k];
}

int store_find(const char *name, struct store_ent *e)
{
    unsigned from = 0, to = STORE_MEDIA;
    if (name[0] == 'u' && name[1] == 'f' && name[2] == 's' && name[3] == ':') {
        name += 4;
        to = 1;
    } else if (name[0] == 's' && name[1] == 'd' && name[2] == ':') {
        name += 3;
        from = 1;
    }
    for (unsigned m = from; m < to; ++m) {
        if (!opened(m))
            continue;
        for (unsigned i = 0; i < med[m].count; ++i)
            if (name_is(med[m].ent[i].name, name)) {
                ent_copy(e, &med[m].ent[i]);
                return 1;
            }
    }
    return 0;
}

static struct {
    int res;
    uint32_t want, got;
    uint64_t size;
    char name[32];
} last_ld = { 1, 0, 0, 0, { 0 } };

void store_last_clear(void)
{
    last_ld.res = 1;
    last_ld.name[0] = 0;
}

int store_last_load(char *name, unsigned cap, uint64_t *size, uint32_t *want, uint32_t *got)
{
    unsigned i = 0;
    for (; cap && i + 1 < cap && i < sizeof(last_ld.name) && last_ld.name[i]; ++i)
        name[i] = last_ld.name[i];
    if (cap)
        name[i] = 0;
    *size = last_ld.size;
    *want = last_ld.want;
    *got = last_ld.got;
    return last_ld.res;
}

/* A failed read or CRC to pmsg (flushed by pmsg_put): medium, entry, blocks, the card's sector and
 * driver state, the first bytes read. STC2: find where card reads go wrong without a screen. */
void star_pmsg(const char *s);
static char *dg_s(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}
static char *dg_x(char *p, uint64_t v, unsigned digits)
{
    for (unsigned i = digits; i; --i) {
        unsigned d = (unsigned)(v >> (4 * (i - 1))) & 15U;
        *p++ = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    return p;
}
static char *dg_u(char *p, uint64_t v)
{
    char t[21];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = t[--n];
    return p;
}
static char *dg_i(char *p, int v)
{
    if (v < 0) {
        *p++ = '-';
        return dg_u(p, (uint64_t)(-(int64_t)v));
    }
    return dg_u(p, (uint64_t)v);
}
void store_read_diag(const char *what, const struct store_ent *e, uint64_t lba, uint64_t count, int r, uintptr_t dst)
{
    char b[400];
    char *p = b;
    p = dg_s(p, "STARBITE store: ");
    p = dg_s(p, what);
    p = dg_s(p, e->medium == STORE_SD ? " sd:" : " ufs:");
    for (unsigned i = 0; i < 31 && e->name[i]; ++i)
        *p++ = e->name[i];
    p = dg_s(p, " r=");
    p = dg_i(p, r);
    p = dg_s(p, " lba ");
    p = dg_u(p, lba);
    p = dg_s(p, " +");
    p = dg_u(p, count);
    p = dg_s(p, " blocks");
    if (e->medium == STORE_SD) {
        p = dg_s(p, " (card sector ");
        p = dg_u(p, lba * 8U);
        p = dg_s(p, " +");
        p = dg_u(p, count * 8U);
        p = dg_s(p, ") sd state ");
        p = dg_i(p, sd_rep.state);
        p = dg_s(p, " err ");
        p = dg_i(p, sd_rep.last_error);
        p = dg_s(p, " rintsts ");
        p = dg_x(p, sd_rep.last_rintsts, 8);
        p = dg_s(p, " status ");
        p = dg_x(p, sd_rep.last_status, 8);
        p = dg_s(p, " card ");
        p = dg_i(p, sd_rep.card_state);
        p = dg_s(p, " recov ");
        p = dg_u(p, sd_rep.recoveries);
        p = dg_s(p, " hc ");
        p = dg_u(p, (uint64_t)sd_rep.high_capacity);
    }
    p = dg_s(p, " first ");
    for (unsigned i = 0; i < 16; ++i)
        p = dg_x(p, *(volatile uint8_t *)(dst + i), 2);
    p = dg_s(p, "\n");
    *p = 0;
    star_pmsg(b);
}

int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap)
{
    int r;
    uint32_t got = 0;
    if (e->size > cap) {
        r = -3;
    } else if ((r = med_read(e->medium, e->lba, (e->size + 4095U) / 4096U, dst)) != 0) {
        store_read_diag("read failed", e, e->lba, (e->size + 4095U) / 4096U, r, dst);
        r = -1;
    } else {
        got = star_crc32(dst, e->size);
        r = got == e->crc ? 0 : -2;
        if (r)
            store_read_diag("CRC mismatch", e, e->lba, (e->size + 4095U) / 4096U, (int)(got & 0x7fffffff), dst);
    }
    unsigned i = 0;
    for (; i + 1 < sizeof(last_ld.name) && i < sizeof(e->name) && e->name[i]; ++i)
        last_ld.name[i] = e->name[i];
    last_ld.name[i] = 0;
    last_ld.res = r;
    last_ld.size = e->size;
    last_ld.want = e->crc;
    last_ld.got = got;
    return r;
}

int store_head(const struct store_ent *e, uintptr_t dst)
{
    int r = med_read(e->medium, e->lba, 1, dst);
    if (r)
        store_read_diag("head read failed", e, e->lba, 1, r, dst);
    return r ? -1 : 0;
}

uint64_t store_base(unsigned m)
{
    return opened(m) ? med[m].base : 0;
}

uint64_t store_capacity(unsigned m)
{
    return opened(m) ? med[m].capacity : 0;
}

uint64_t store_used(unsigned m)
{
    return opened(m) ? med[m].used : 0;
}

/* ---- settings record --------------------------------------------------------
 * Block 2 of the UFS store, the codec and layout in sbfmt.c / sbfmt.h ("SBSETTNG", version 1, CRC32
 * of bytes 16..255). settings_save keeps bytes it does not know (112..255 of the record it read).
 */
/* The home store's settings block (and its medium in *m), or 0. */
static uint64_t settings_lba_on(unsigned *m)
{
    int h = store_home();
    if (h < 0)
        return 0;
    struct medium *s = &med[h];
    if (s->state <= 0 || s->version != 2 || s->settings != 2)
        return 0;
    *m = (unsigned)h;
    return s->base + s->settings;
}

static int set_status = -2;
static uint8_t set_tail[SBSET_BYTES];   /* the last record read, for the bytes after the known fields */

int settings_status(void)
{
    return set_status;
}

int settings_load(struct star_settings *st)
{
    sbset_defaults(st);
    unsigned m = 0;
    uint64_t lba = settings_lba_on(&m);
    if (!lba) {
        set_status = -2;
        return 0;
    }
    if (med_read(m, lba, 1, READBACK)) {
        set_status = -1;
        return 0;
    }
    uint8_t b[SBSET_BYTES];
    for (unsigned i = 0; i < SBSET_BYTES; ++i)
        b[i] = *(volatile uint8_t *)(READBACK + i);
    set_status = sbset_check(b);
    if (set_status != 1)
        return 0;
    sbset_decode(b, st);
    for (unsigned i = 0; i < SBSET_BYTES; ++i)
        set_tail[i] = b[i];
    return 1;
}

static uint8_t set_blk[CL_BLOCK];
static int fw_dirty;     /* settings_fw_save put a new SBFW part into set_tail */

int settings_fw(struct sbfw_rec *r)
{
    struct star_settings st;
    int ok = settings_load(&st);
    if (ok) {
        sbfw_rec_decode(set_tail, r);
    } else {
        uint8_t *q = (uint8_t *)r;             /* no record yet: all zero */
        for (unsigned i = 0; i < sizeof(*r); ++i)
            q[i] = 0;
    }
    return ok;
}

int settings_fw_save(struct star_settings *st, const struct sbfw_rec *r)
{
    if (set_status != 1) {          /* no record yet: start from this one's bytes */
        sbset_encode(set_blk, st);  /* 4 KiB: the tail keeps its first 256 */
        for (unsigned i = 0; i < SBSET_BYTES; ++i)
            set_tail[i] = set_blk[i];
    }
    sbfw_rec_encode(set_tail, r);
    fw_dirty = 1;
    return settings_save(st);
}

int settings_save(struct star_settings *st)
{
    unsigned m = 0;
    uint64_t lba = settings_lba_on(&m);
    if (!lba)
        return -1;
    ++st->seq;
    sbset_encode(set_blk, st);
    /* fields a newer tool wrote after ours: kept as they were (the CRC covers them again) */
    if ((set_status == 1 && set_tail[0] == 'S') || fw_dirty) {
        for (unsigned i = 112; i < SBSET_BYTES; ++i)
            set_blk[i] = set_tail[i];
        uint32_t c = sb_crc32(set_blk + 16, SBSET_BYTES - 16);
        for (unsigned i = 0; i < 4; ++i)
            set_blk[12 + i] = (uint8_t)(c >> (8 * i));
    }
    uintptr_t b = SCRATCH;
    const uint8_t *src = set_blk;
    __asm__("" : "+r"(src));    /* no folded "set_blk - SCRATCH" constant: that is an ABS64 relocation */
    for (unsigned i = 0; i < CL_BLOCK; ++i)
        *(volatile uint8_t *)(b + i) = src[i];
    if (med_write(m, lba, b) || med_read(m, lba, 1, READBACK))
        return -2;
    for (unsigned i = 0; i < 256; i += 8)
        if (*(volatile uint64_t *)(READBACK + i) != *(volatile uint64_t *)(b + i))
            return -3;
    for (unsigned i = 0; i < SBSET_BYTES; ++i)
        set_tail[i] = set_blk[i];
    set_status = 1;
    fw_dirty = 0;
    return 0;
}

/* ---- the crash log ("crashlog", SF_VOLATILE, 16 blocks: sbfmt.h) ----------------------------- */

int store_crashlog(struct store_ent *e)
{
    int h = store_home();
    if (h < 0)
        return 0;
    return store_find(h == (int)STORE_SD ? "sd:crashlog" : "ufs:crashlog", e) && (e->flags & SF_VOLATILE) &&
           e->size >= CL_BYTES && e->medium == (unsigned)h;
}

int store_crashlog_write(const struct store_ent *e, unsigned index, uintptr_t src)
{
    struct store_ent chk;
    if (index >= CL_BYTES / CL_BLOCK || !store_crashlog(&chk) || chk.lba != e->lba || chk.medium != e->medium)
        return -1;
    uint64_t lba = e->lba + index;
    if (med_write(e->medium, lba, src) || med_read(e->medium, lba, 1, READBACK))
        return -2;
    for (unsigned i = 0; i < CL_BLOCK; i += 8)
        if (*(volatile uint64_t *)(READBACK + i) != *(volatile uint64_t *)(src + i))
            return -3;
    return 0;
}

int store_volatile_write(const char *name, uint64_t index, uint64_t count, uintptr_t src)
{
    int h = store_home();
    struct store_ent e;
    char full[40];
    const char *pre = h == (int)STORE_SD ? "sd:" : "ufs:";
    unsigned k = 0;
    for (; pre[k]; ++k)
        full[k] = pre[k];
    for (unsigned j = 0; name[j] && k + 1 < sizeof(full); ++j)
        full[k++] = name[j];
    full[k] = 0;
    if (h < 0 || !store_find(full, &e) || !(e.flags & SF_VOLATILE) || e.medium != (unsigned)h)
        return -1;
    uint64_t blocks = (e.size + 4095U) / 4096U;
    if (!count || index > blocks || count > blocks - index)
        return -1;
    int r = 0;
    if (h == (int)STORE_SD)
        sd_fence(2, e.lba + index, count);
    for (uint64_t i = 0; i < count && !r; ++i) {
        uintptr_t s = src + (uintptr_t)i * 4096U;
        if (med_write(e.medium, e.lba + index + i, s) || med_read(e.medium, e.lba + index + i, 1, READBACK)) {
            r = -2;
            break;
        }
        for (unsigned j = 0; j < 4096U; j += 8)
            if (*(volatile uint64_t *)(READBACK + j) != *(volatile uint64_t *)(s + j)) {
                r = -3;
                break;
            }
    }
    if (h == (int)STORE_SD)
        sd_fence(2, 0, 0);
    return r;
}

void store_rehome(void)
{
    home = -2;
    for (unsigned m = 0; m < STORE_MEDIA; ++m)
        med[m].state = 0;
    for (unsigned i = 0; i < SD_FENCES; ++i)
        sd_fence(i, 0, 0);
    set_status = -2;
}

int store_read(const struct store_ent *e, uint64_t index, uint64_t count, uintptr_t dst)
{
    uint64_t blocks = (e->size + 4095U) / 4096U;
    if (!count || index > blocks || count > blocks - index)
        return -1;
    return med_read(e->medium, e->lba + index, count, dst) ? -1 : 0;
}

/* ---- the home store ---------------------------------------------------------------------------- */

void store_cmdline(const char *c)
{
    static const char want[] = "sbstore=card";
    for (unsigned i = 0; c && c[i]; ++i) {
        if (i && c[i - 1] != ' ')
            continue;
        unsigned k = 0;
        while (want[k] && c[i + k] == want[k])
            ++k;
        if (!want[k] && (c[i + k] == 0 || c[i + k] == ' '))
            card_only = 1;
    }
}

int store_card_only(void)
{
    return card_only;
}

int store_marked_home(unsigned m)
{
    return opened(m) && med[m].version == 2 && med[m].home;
}

/* The UFS store's settings flags (raw: the record of the UFS store's block 2), 0 without one. */
static uint32_t ufs_settings_flags(void)
{
    struct medium *s = &med[STORE_UFS];
    if (s->state <= 0 || s->version != 2 || s->settings != 2 || med_read(STORE_UFS, s->base + 2, 1, READBACK))
        return 0;
    uint8_t b[SBSET_BYTES];
    for (unsigned i = 0; i < SBSET_BYTES; ++i)
        b[i] = *(volatile uint8_t *)(READBACK + i);
    if (sbset_check(b) != 1)
        return 0;
    struct star_settings st;
    sbset_decode(b, &st);
    return st.flags;
}

int store_home(void)
{
    if (home >= -1)
        return home;
    if (home == -3)
        return -1;                     /* asked while resolving (store_open of a medium): none yet */
    home = -3;
    int h = -1;
    if (card_only) {
        sd_allow();
        if (store_open(STORE_SD) >= 0 && med[STORE_SD].state > 0 && med[STORE_SD].version == 2)
            h = (int)STORE_SD;
    } else {
        (void)store_open(STORE_UFS);
        if (!sd_allowed() && (ufs_settings_flags() & SET_SD_STORE))
            sd_allow();
        if (sd_allowed() && (store_open(STORE_SD), med[STORE_SD].state > 0) && med[STORE_SD].version == 2 &&
            med[STORE_SD].home) {
            h = (int)STORE_SD;
            med[STORE_UFS].state = -1;     /* the card is home: the UFS store is closed for this boot */
            med[STORE_UFS].count = 0;
        } else if (med[STORE_UFS].state > 0) {
            h = (int)STORE_UFS;
        }
    }
    home = h;
    if (h == (int)STORE_SD) {
        /* the only card blocks the loader may write: settings (block 2) and the crash log */
        struct medium *s = &med[STORE_SD];
        if (s->settings == 2)
            sd_fence(0, s->base + 2, 1);
        for (unsigned i = 0; i < s->count; ++i)
            if (name_is(s->ent[i].name, "crashlog") && (s->ent[i].flags & SF_VOLATILE) && s->ent[i].size >= CL_BYTES)
                sd_fence(1, s->ent[i].lba, CL_BYTES / CL_BLOCK);
    }
    return home;
}

const char *store_home_why(void)
{
    if (store_home() >= 0)
        return 0;
    if (!card_only)
        return "NO OS STORE";
    if (!sd_rep.present)
        return "NO OS STORE: NO MICROSD CARD";
    if (sd_rep.state < 0)
        return "NO OS STORE: THE MICROSD CARD DID NOT START";
    return "NO OS STORE ON THE CARD";
}
