/* SPDX-License-Identifier: GPL-2.0-only
 * The Starbite OS store: see store.h. The reader is shim/starlte/store.c's. The writer (Engineer OS
 * v2) only appends (data beyond the used end, a directory slot beyond the count, then the header
 * as the single commit) and saves the settings record (one block, as the loader's settings_save);
 * removing or renaming an entry rewrites the directory and is tier 2. Every block goes through the
 * write fence (sb_write_store / sb_write) with the user's confirmation, and is read back.
 * The home store (store_home, the loader's rule: store.h) holds the settings record and the OS's volatile
 * state: the UFS store, or since 2026-11-11 the card's (docs/STORE-ON-CARD.md); the loader's SBFW table
 * (sbfw.c) says which, and "card only" keeps libsb off the UFS store entirely.
 */
#include <stdint.h>
#include "store.h"
#include "sb.h"
#include "ufs.h"
#include "sd.h"

#define UFS_STORE_OFFSET 4194304ULL            /* USERDATA +16 GiB, in 4 KiB blocks */
#define V1_BLOCKS        262144ULL             /* the version 1 store: 1 GiB */
#define SCRATCH          (UFS_BUF + (512U << 10))
#define SETTINGS_MAGIC   0x474e545445534253ULL /* "SBSETTNG" */

struct medium {
    int state;                 /* 0 not tried, 1 open, -1 none */
    int version;
    int home;                  /* the header's HOME flag (SBSTHOME extension) */
    uint64_t base, capacity, used, settings;
    uint32_t n_hdr, dir, dir_blocks;    /* the header's count and directory (v2) */
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
static __attribute__((target("+crc"))) uint32_t star_crc32(uintptr_t p, uint64_t n)
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
    s->n_hdr = n;
    s->dir = dir;
    s->dir_blocks = dir_blocks;
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

/* ---- home store state (store.h) ------------------------------------------------------------------- */
static int home = -2;          /* -2 not resolved, -3 resolving, -1 none, else the medium */
static int probed_card_only;   /* 1.0 (Track A): an older loader without a table, on a card-only phone (sbstore=card, or
                                * no UFS store at all): the card's store is home and UFS is not touched again */

int store_card_only(void)
{
    return (sb_fw.valid && (sb_fw.flags & SBFW_TF_CARD_ONLY)) || probed_card_only;
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
        /* USERDATA released to iOS (card only), or the card's store is home: the UFS store area is not
         * read, listed or written again (docs/STORE-ON-CARD.md) */
        if (store_card_only() || home == (int)STORE_SD)
            return 0;
        if (!ufs_up() || !gpt_find("USERDATA", &first, &last) || last - first + 1 <= UFS_STORE_OFFSET + 16)
            return 0;
        s->base = first + UFS_STORE_OFFSET;
        limit = last - first + 1 - UFS_STORE_OFFSET;
    } else {
        /* The card hardware is touched only once allowed (sd.h): by the loader's table (the card is home),
         * the UFS settings record (store_home) or EngineerMode's microSD page, which then calls
         * store_reset. */
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
    if (s->version == 2)
        sb_fence_store(m, s->base, s->used, s->capacity);
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
    int h = store_home();
    unsigned order[STORE_MEDIA] = { STORE_UFS, STORE_SD }, n = STORE_MEDIA;
    if (h == (int)STORE_SD) {          /* the home store first */
        order[0] = STORE_SD;
        order[1] = STORE_UFS;
    }
    if (name[0] == 'u' && name[1] == 'f' && name[2] == 's' && name[3] == ':') {
        name += 4;
        order[0] = STORE_UFS;
        n = 1;
    } else if (name[0] == 's' && name[1] == 'd' && name[2] == ':') {
        name += 3;
        order[0] = STORE_SD;
        n = 1;
    } else if (name[0] == 'h' && name[1] == 'o' && name[2] == 'm' && name[3] == 'e' && name[4] == ':') {
        name += 5;
        if (h < 0)
            return 0;
        order[0] = (unsigned)h;
        n = 1;
    }
    for (unsigned k = 0; k < n; ++k) {
        unsigned m = order[k];
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

int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap)
{
    int r;
    uint32_t got = 0;
    if (e->size > cap) {
        r = -3;
    } else if (med_read(e->medium, e->lba, (e->size + 4095U) / 4096U, dst)) {
        r = -1;
    } else {
        got = star_crc32(dst, e->size);
        /* flag 1 = volatile entry (the loader's crash log ring, docs/STARBITE-HEALTH.md): rewritten in
         * place, so its directory CRC is not kept up to date and is not checked */
        r = (e->flags & 1U) || got == e->crc ? 0 : -2;
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
    return med_read(e->medium, e->lba, 1, dst) ? -1 : 0;
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
 * Block 2 of the home store: "SBSETTNG", u32 version 1, u32 CRC32 of bytes 16..255, u32 seq,
 * u32 timeout_s, u32 flags, u32 0, char default_key[24], last_key[24], next_key[24], the loader's
 * crash / watchdog bytes 104..111, the SBFW part 112..255 (store.h, sbfw.c).
 */
/* Medium m's settings block (version 2, block 2), or 0. */
static uint64_t settings_lba_of(unsigned m)
{
    struct medium *s = &med[m];
    if (!opened(m) || s->version != 2 || s->settings != 2)
        return 0;
    return s->base + s->settings;
}

/* The home store's settings block and its medium, or 0. */
static uint64_t settings_lba(unsigned *m)
{
    int h = store_home();
    if (h < 0)
        return 0;
    *m = (unsigned)h;
    return settings_lba_of((unsigned)h);
}

/* The record at lba of medium m into UFS_BUF: 1 if it is a valid one (magic, version 1, CRC). */
static int settings_read(unsigned m, uint64_t lba)
{
    if (m == STORE_UFS ? ufs_read(lba, 1) != 0 : sd_read4k(lba, 1, UFS_BUF) != 0)
        return 0;
    return le(UFS_BUF, 8) == SETTINGS_MAGIC && le(UFS_BUF + 8, 4) == 1 &&
           star_crc32(UFS_BUF + 16, 240) == (uint32_t)le(UFS_BUF + 12, 4);
}

static void key_get(char *dst, uintptr_t src)
{
    for (unsigned k = 0; k < 24; ++k)
        dst[k] = (char)*(volatile uint8_t *)(src + k);
    dst[23] = 0;
}

int settings_load(struct star_settings *st)
{
    uint8_t *z = (uint8_t *)st;
    for (unsigned i = 0; i < sizeof(*st); ++i)
        z[i] = 0;
    unsigned m = 0;
    uint64_t lba = settings_lba(&m);
    if (!lba || !settings_read(m, lba))
        return 0;
    st->seq = (uint32_t)le(UFS_BUF + 16, 4);
    st->timeout_s = (uint32_t)le(UFS_BUF + 20, 4);
    st->flags = (uint32_t)le(UFS_BUF + 24, 4);
    key_get(st->default_key, UFS_BUF + 32);
    key_get(st->last_key, UFS_BUF + 56);
    key_get(st->next_key, UFS_BUF + 80);
    return 1;
}

/* ---- writer (Engineer OS v2) ----------------------------------------------------------------
 * Append: data from the used end, read back chunk by chunk, CRC32 of the whole read back again at
 * the end; the new directory entry goes into slot n, which the header's directory CRC (over n
 * entries) does not cover yet, so writing that block changes nothing a reader sees; then block 1
 * with n + 1, the new used end and both CRCs: the one write that makes the entry exist. A failure
 * before it leaves the store exactly as it was. Tail guards: UFS keeps the last 1025 blocks of
 * USERDATA free (the iOS 26 kernel's herolog ring), the card the last 16384 (XNU's microSD root
 * test range); sbstore2.py refuses the same. */

static __attribute__((target("+crc"))) uint32_t crc_upd(uint32_t c, uintptr_t p, uint64_t n)
{
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8)
        __asm__("crc32x %w0, %w0, %x1" : "+r"(c) : "r"(*(volatile uint64_t *)(p + i)));
    for (; i < n; ++i)
        __asm__("crc32b %w0, %w0, %w1" : "+r"(c) : "r"((uint32_t)*(volatile uint8_t *)(p + i)));
    return c;
}

uint32_t store_crc32(const void *p, uint64_t n)
{
    return ~crc_upd(0xffffffffU, (uintptr_t)p, n);
}

static void put(uintptr_t p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        *(volatile uint8_t *)(p + i) = (uint8_t)(v >> (8 * i));
}

static void zero(uintptr_t p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i += 8)
        *(volatile uint64_t *)(p + i) = 0;
}

static int same(uintptr_t a, uintptr_t b, uint64_t n)
{
    for (uint64_t i = 0; i < n; i += 8)
        if (*(volatile uint64_t *)(a + i) != *(volatile uint64_t *)(b + i))
            return 0;
    return 1;
}

static uint64_t tail_guard(unsigned m)
{
    return m == STORE_UFS ? 1025U : 16384U;
}

uint64_t store_free(unsigned m)
{
    if (!opened(m) || med[m].version != 2)
        return 0;
    uint64_t g = tail_guard(m) + 8U;
    return med[m].used + g < med[m].capacity ? med[m].capacity - med[m].used - g : 0;
}

/* One block through the store writer, then read back and compared. */
static int wr_block(unsigned m, uint64_t lba, uintptr_t src)
{
    int r = sb_write_store(m, lba, 1, (const void *)src);
    if (r)
        return r;
    if (med_read(m, lba, 1, SB_STORE_IO + 0x80000))
        return SB_W_IO;
    return same(src, SB_STORE_IO + 0x80000, 4096) ? 0 : SB_W_IO;
}

int store_wr_begin(struct store_writer *w, unsigned m, const char *name, unsigned type, uint64_t size)
{
    w->err = -1;
    w->m = m;
    w->size = size;
    w->done = 0;
    w->crc = 0xffffffffU;
    w->type = type;
    unsigned k = 0;
    for (; name[k] && k < 31; ++k)
        w->name[k] = name[k];
    if (name[k] || !k)
        return w->err = -2;                     /* empty or longer than 31 */
    for (; k < 32; ++k)
        w->name[k] = 0;
    if (!opened(m) || med[m].version != 2)
        return w->err = -1;
    struct medium *s = &med[m];
    for (unsigned i = 0; i < s->count; ++i)
        if (name_is(s->ent[i].name, w->name))
            return w->err = -3;                 /* the name exists */
    if (s->n_hdr >= STORE_MAX || (s->n_hdr + 1U) * 64U > s->dir_blocks * 4096U)
        return w->err = -4;                     /* directory full */
    uint64_t blocks = (size + 4095U) / 4096U;
    if (!blocks || blocks > store_free(m))
        return w->err = -5;                     /* no room */
    w->at = s->base + s->used;
    w->err = 0;
    return 0;
}

/* bytes: a multiple of 4096 except in the last chunk; src any RAM. */
int store_wr_chunk(struct store_writer *w, const void *src, uint64_t bytes)
{
    if (w->err)
        return w->err;
    if (!bytes || w->done + bytes > w->size || (w->done & 4095U))
        return w->err = -6;
    uintptr_t p = (uintptr_t)src;
    uint64_t lba = w->at + w->done / 4096U;
    w->crc = crc_upd(w->crc, p, bytes);
    uint64_t full = bytes / 4096U;
    for (uint64_t i = 0; i < full; i += 64) {
        uint64_t n = full - i < 64 ? full - i : 64;
        int r = sb_write_store(w->m, lba + i, n, (const void *)(p + i * 4096U));
        if (!r && med_read(w->m, lba + i, n, SB_STORE_IO))
            r = SB_W_IO;
        if (!r && !same(p + i * 4096U, SB_STORE_IO, n * 4096U))
            r = SB_W_IO;
        if (r)
            return w->err = r;
    }
    if (bytes & 4095U) {
        uintptr_t b = SB_STORE_IO + 0x40000;
        zero(b, 4096);
        for (uint64_t i = 0; i < (bytes & 4095U); ++i)
            *(volatile uint8_t *)(b + i) = *(const volatile uint8_t *)(p + full * 4096U + i);
        int r = wr_block(w->m, lba + full, b);
        if (r)
            return w->err = r;
    }
    w->done += bytes;
    return 0;
}

static int find_on(unsigned m, const char *name, struct store_ent *e)
{
    if (!opened(m))
        return 0;
    for (unsigned i = 0; i < med[m].count; ++i)
        if (name_is(med[m].ent[i].name, name)) {
            ent_copy(e, &med[m].ent[i]);
            return 1;
        }
    return 0;
}

int store_wr_commit(struct store_writer *w, struct store_ent *out)
{
    if (w->err)
        return w->err;
    if (w->done != w->size)
        return w->err = -6;
    struct medium *s = &med[w->m];
    uint32_t crc = ~w->crc;
    uint64_t blocks = (w->size + 4095U) / 4096U;
    uint32_t c = 0xffffffffU;                           /* the whole entry read back once more */
    for (uint64_t i = 0; i < blocks; i += 64) {
        uint64_t n = blocks - i < 64 ? blocks - i : 64;
        if (med_read(w->m, w->at + i, n, SB_STORE_IO))
            return w->err = SB_W_IO;
        uint64_t bytes = (i + n) * 4096U > w->size ? w->size - i * 4096U : n * 4096U;
        c = crc_upd(c, SB_STORE_IO, bytes);
    }
    if (~c != crc)
        return w->err = SB_W_IO;
    uint32_t n = s->n_hdr;                              /* directory slot n: not covered yet */
    uintptr_t dir = SB_STORE_IO + 0x40000;              /* the whole directory (16 KiB) */
    if (med_read(w->m, s->base + s->dir, s->dir_blocks, dir))
        return w->err = SB_W_IO;
    uintptr_t e = dir + 64U * n;
    zero(e, 64);
    for (unsigned k = 0; k < 32; ++k)
        *(volatile uint8_t *)(e + k) = (uint8_t)w->name[k];
    put(e + 32, w->at - s->base, 8);
    put(e + 40, w->size, 8);
    put(e + 48, crc, 4);
    put(e + 52, w->type, 2);
    uint32_t blk = n * 64U / 4096U;
    int r = wr_block(w->m, s->base + s->dir + blk, dir + blk * 4096U);
    if (r)
        return w->err = r;
    uintptr_t h = SB_STORE_IO + 0x50000;                /* the header: the commit */
    if (med_read(w->m, s->base + 1, 1, h) || le(h, 8) != 0x3245524f54534253ULL ||
        (uint32_t)le(h + 12, 4) != n || le(h + 24, 8) != s->used)
        return w->err = -7;                             /* the store changed under us */
    put(h + 12, n + 1U, 4);
    put(h + 24, s->used + blocks, 8);
    put(h + 44, store_crc32((const void *)dir, (uint64_t)(n + 1U) * 64U), 4);
    put(h + 48, store_crc32((const void *)h, 48), 4);
    r = wr_block(w->m, s->base + 1, h);
    if (r)
        return w->err = r;
    store_reset(w->m);
    struct store_ent got;
    if (!find_on(w->m, w->name, &got) || got.crc != crc)
        return w->err = -8;                             /* committed, but not found again */
    if (out)
        ent_copy(out, &got);
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

/* The settings record (the home store's block 2): one block, as the loader's settings_save, read back.
 * fw: a new SBFW part (bytes 112..255), or 0 to keep the current one; ldr: new loader bytes 104..111 (the crash /
 * watchdog settings, docs/STARBITE-HEALTH.md), or 0 to keep them. */
static int settings_write(struct star_settings *st, const struct sbfw_rec *fw, const uint8_t *ldr)
{
    unsigned m = 0;
    uint64_t lba = settings_lba(&m);
    if (!lba)
        return -1;
    uintptr_t b = SB_STORE_IO + 0x60000;
    zero(b, 4096);
    /* Keep everything this record does not own: the loader's crash/watchdog settings at bytes
     * 104..111 (docs/STARBITE-HEALTH.md), the SBFW part 112..255 (docs/STARBITE-FIRMWARE-LAYER.md 10.1)
     * and anything a later loader adds. Only a valid current record is carried over; otherwise the
     * block starts from zeros (the loader's defaults). */
    if (settings_read(m, lba))
        for (unsigned i = 104; i < 256; ++i)
            *(volatile uint8_t *)(b + i) = *(volatile uint8_t *)(UFS_BUF + i);
    if (ldr)
        for (unsigned i = 0; i < 8; ++i)
            *(volatile uint8_t *)(b + 104 + i) = ldr[i];
    if (fw)
        sbfw_rec_encode((uint8_t *)b, fw);
    st->seq++;
    put(b, SETTINGS_MAGIC, 8);
    put(b + 8, 1, 4);
    put(b + 16, st->seq, 4);
    put(b + 20, st->timeout_s, 4);
    put(b + 24, st->flags, 4);
    for (unsigned k = 0; k < 24; ++k) {
        *(volatile uint8_t *)(b + 32 + k) = (uint8_t)(k < 23 ? st->default_key[k] : 0);
        *(volatile uint8_t *)(b + 56 + k) = (uint8_t)(k < 23 ? st->last_key[k] : 0);
        *(volatile uint8_t *)(b + 80 + k) = (uint8_t)(k < 23 ? st->next_key[k] : 0);
    }
    put(b + 12, store_crc32((const void *)(b + 16), 240), 4);
    int r = wr_block(m, lba, b);
    if (r)
        st->seq--;
    return r;
}

int settings_save(struct star_settings *st)
{
    return settings_write(st, 0, 0);
}

int settings_fw_save(struct star_settings *st, const struct sbfw_rec *r)
{
    return settings_write(st, r, 0);
}

int settings_all_save(struct star_settings *st, const struct sbfw_rec *r, const uint8_t ldr[8])
{
    return settings_write(st, r, ldr);
}

int settings_ldr_load(uint8_t ldr[8])
{
    unsigned m = 0;
    uint64_t lba = settings_lba(&m);
    int ok = lba && settings_read(m, lba);
    for (unsigned i = 0; i < 8; ++i)
        ldr[i] = ok ? *(volatile uint8_t *)(UFS_BUF + 104 + i) : 0;
    return ok;
}

/* Block k of entry e (no CRC: a volatile entry's blocks change under its directory CRC) into dst (plain RAM), via
 * the store's DMA scratch. */
int store_read_block(const struct store_ent *e, uint64_t k, void *dst)
{
    if (k >= (e->size + 4095U) / 4096U)
        return -3;
    uintptr_t b = SB_STORE_IO + 0x60000;
    if (med_read(e->medium, e->lba + k, 1, b))
        return -1;
    uint8_t *d = (uint8_t *)dst;
    for (unsigned i = 0; i < 4096; ++i)
        d[i] = *(volatile uint8_t *)(b + i);
    return 0;
}

int settings_fw_load(struct sbfw_rec *r)
{
    unsigned m = 0;
    uint64_t lba = settings_lba(&m);
    if (lba && settings_read(m, lba)) {
        uint8_t blk[SBSET_BYTES];
        for (unsigned i = 0; i < SBSET_BYTES; ++i)
            blk[i] = *(volatile uint8_t *)(UFS_BUF + i);
        sbfw_rec_decode(blk, r);
        return 1;
    }
    uint8_t *q = (uint8_t *)r;                 /* no record: all zero */
    for (unsigned i = 0; i < sizeof(*r); ++i)
        q[i] = 0;
    return 0;
}

/* Remove entry idx (directory index) or rename it: the directory is rewritten, then the header.
 * Between the two writes (milliseconds) the directory CRC does not match, and a power loss there
 * leaves the loader on the version 1 view. Tier 2 (sb_write, the live store): the caller's
 * authorisation covers store_dir_range. The data of a removed entry stays where it is (the used
 * end does not move, so the version 1 view stays valid for older readers); a repack on the Mac
 * reclaims it. If the header write fails, the old directory is written back. */
static int dir_rewrite(unsigned m, unsigned idx, const char *newname)
{
    if (!opened(m) || med[m].version != 2)
        return -1;
    struct medium *s = &med[m];
    uint32_t n = s->n_hdr;
    if (idx >= n || s->count != n)
        return -2;
    uintptr_t old = SB_STORE_IO, dir = SB_STORE_IO + 0x40000, h = SB_STORE_IO + 0x50000;
    if (med_read(m, s->base + s->dir, s->dir_blocks, old) || med_read(m, s->base + s->dir, s->dir_blocks, dir) ||
        med_read(m, s->base + 1, 1, h))
        return SB_W_IO;
    uint32_t nn = n;
    if (newname) {
        uintptr_t e = dir + 64U * idx;
        unsigned k = 0;
        for (; newname[k] && k < 31; ++k)
            *(volatile uint8_t *)(e + k) = (uint8_t)newname[k];
        if (!k || newname[k])
            return -3;
        for (; k < 32; ++k)
            *(volatile uint8_t *)(e + k) = 0;
    } else {
        for (uint32_t i = idx; i + 1 < n; ++i)
            for (unsigned b = 0; b < 64; b += 8)
                *(volatile uint64_t *)(dir + 64U * i + b) = *(volatile uint64_t *)(dir + 64U * (i + 1) + b);
        zero(dir + 64U * (n - 1), 64);
        nn = n - 1;
    }
    put(h + 12, nn, 4);
    put(h + 44, store_crc32((const void *)dir, (uint64_t)nn * 64U), 4);
    put(h + 48, store_crc32((const void *)h, 48), 4);
    uint32_t first = idx * 64U / 4096U, last = (n * 64U + 4095U) / 4096U;
    if (last > s->dir_blocks)
        last = s->dir_blocks;
    int r = sb_write(m, 0, s->base + s->dir + first, last - first, (const void *)(dir + first * 4096U));
    if (r)
        return r;
    r = sb_write(m, 0, s->base + 1, 1, (const void *)h);
    if (r) {                                            /* put the old directory back */
        (void)sb_write(m, 0, s->base + s->dir + first, last - first, (const void *)(old + first * 4096U));
        return r;
    }
    store_reset(m);
    return store_open(m) >= 0 ? 0 : -4;
}

int store_remove(unsigned m, unsigned idx)
{
    return dir_rewrite(m, idx, 0);
}

int store_rename(unsigned m, unsigned idx, const char *newname)
{
    return dir_rewrite(m, idx, newname);
}

/* Blocks 1 .. the directory's end (what store_remove / store_rename write). */
int store_dir_range(unsigned m, uint64_t *first, uint64_t *count)
{
    if (!opened(m) || med[m].version != 2)
        return 0;
    *first = med[m].base + 1;
    *count = (uint64_t)med[m].dir + med[m].dir_blocks - 1U;
    return 1;
}

/* Entry i's data read back and its CRC32 computed (*got): 0 match, 1 mismatch, < 0 read error. */
int store_verify(unsigned m, unsigned i, uint32_t *got)
{
    if (!opened(m) || i >= med[m].count)
        return -1;
    const struct store_ent *e = &med[m].ent[i];
    if (e->flags & 1U) {                    /* volatile (crashlog, sbos-state): written in place, no CRC to check */
        *got = e->crc;
        return 0;
    }
    uint64_t blocks = (e->size + 4095U) / 4096U;
    uint32_t c = 0xffffffffU;
    for (uint64_t b = 0; b < blocks; b += 64) {
        uint64_t n = blocks - b < 64 ? blocks - b : 64;
        if (med_read(m, e->lba + b, n, SB_STORE_IO))
            return -2;
        uint64_t bytes = (b + n) * 4096U > e->size ? e->size - b * 4096U : n * 4096U;
        c = crc_upd(c, SB_STORE_IO, bytes);
    }
    *got = ~c;
    return *got == e->crc ? 0 : 1;
}

/* An OS's own volatile entry (docs/STARBITE-OS.md 6.5): found on the home store (UFS, or the card's once it is
 * home), with the volatile directory flag (sbstore2.py --volatile), at least min_bytes; the fence then lets
 * sb_write_volatile write exactly its blocks. 1 granted (*e filled), 0 no such entry (or not volatile, too
 * small: nothing granted). */
int store_volatile_grant(const char *name, uint64_t min_bytes, struct store_ent *e)
{
    int h = store_home();
    if (h < 0)
        return 0;
    char full[40];
    unsigned p = h == (int)STORE_SD ? 3U : 4U;
    sb_strcpy(full, sizeof(full), h == (int)STORE_SD ? "sd:" : "ufs:");
    sb_strcpy(full + p, sizeof(full) - p, name);
    if (!store_open((unsigned)h) || !store_find(full, e) || e->medium != (unsigned)h || !(e->flags & 1U) ||
        e->size < min_bytes || (e->size & 4095U))
        return 0;
    /* the layout as this store's header says it, not as assumed: never blocks 0..1 (the v1 view, the header),
     * the settings block or the directory, and no other entry's blocks (a directory whose CRC is right can
     * still list overlapping entries) */
    const struct medium *s = &med[h];
    uint64_t a = e->lba - s->base, n = e->size / 4096U;
    if (s->version != 2 || e->lba < s->base || a < 2 || (s->settings >= a && s->settings < a + n) ||
        (a < (uint64_t)s->dir + s->dir_blocks && a + n > s->dir)) {
        sb_log("store: the volatile entry overlaps the store's header, settings or directory: not granted");
        return 0;
    }
    for (unsigned i = 0; i < s->count; ++i) {
        const struct store_ent *o = &s->ent[i];
        uint64_t on = (o->size + 4095U) / 4096U;
        if (o->lba == e->lba && name_is(o->name, e->name))
            continue;
        if (on && o->lba < e->lba + n && o->lba + on > e->lba) {
            sb_logf("store: the volatile entry overlaps another entry, not granted: ", o->name);
            return 0;
        }
    }
    return sb_volatile_grant(h == (int)STORE_SD ? SB_MED_SD : SB_MED_UFS, e->lba, n, e->name);
}

/* ---- the home store (store.h) ---------------------------------------------------------------------- */

int store_marked_home(unsigned m)
{
    return opened(m) && med[m].version == 2 && med[m].home;
}

/* The UFS store's settings flags (its own block 2, whoever is home), 0 without a valid record. */
static uint32_t ufs_settings_flags(void)
{
    uint64_t lba = med[STORE_UFS].state > 0 ? settings_lba_of(STORE_UFS) : 0;
    if (!lba || !settings_read(STORE_UFS, lba))
        return 0;
    return (uint32_t)le(UFS_BUF + 24, 4);
}

static void home_log(int h, const char *why)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "store: home ");
    ub_s(&u, h == (int)STORE_SD ? "card" : h == (int)STORE_UFS ? "ufs" : "none");
    if (h >= 0) {
        ub_s(&u, " at ");
        ub_dec(&u, med[h].base);
    }
    ub_s(&u, " (");
    ub_s(&u, why);
    ub_s(&u, store_card_only() ? ", card only)" : ")");
    sb_log(u.b);
}

int store_home(void)
{
    if (home >= -1)
        return home;
    if (home == -3)
        return -1;                     /* asked while resolving (store_open of a medium): none yet */
    home = -3;
    int h = -1;
    const char *why;
    if (sb_fw.valid) {
        /* the loader decided: its home store, at the base it gives, or none (no probing, no UFS guess) */
        unsigned m = sb_fw.store_medium;
        why = "the loader's table";
        if (m == STORE_SD || store_card_only())
            sd_allow();
        if (m == STORE_SD || (m == STORE_UFS && !store_card_only())) {
            (void)store_open(m);
            if (med[m].state > 0 && med[m].version == 2 && med[m].base == sb_fw.store_base)
                h = (int)m;
            else
                why = med[m].state > 0 ? "the table's store is not this version 2 store: nothing written"
                                       : "the table's store does not open: nothing written";
        }
        /* the UFS home's record may still let the card's store be read, as before */
        if (h == (int)STORE_UFS && !sd_allowed() && (ufs_settings_flags() & SET_SD_STORE))
            sd_allow();
    } else {
        /* an older loader: the card when its store says HOME (and the card may be touched), else UFS */
        why = "no table: probed";
        /* 1.0 (Track A; wifi-core's card-only finding): S-Boot's sbstore=card says USERDATA is not ours (the UFS
         * store area is not read at all); a UFS area without a store says the same. Then the card's version 2
         * store is home whether or not it carries the HOME mark (a card made before SBSTHOME) */
        int card_arg = 0;
#ifndef SB_HOST_FAKE_MEM
        {
            char v[8];
            card_arg = sb_bootarg("sbstore", v, sizeof v) && v[0] == 'c' && v[1] == 'a' && v[2] == 'r' && v[3] == 'd' && !v[4];
        }
#endif
        if (card_arg)
            probed_card_only = 1;
        else
            (void)store_open(STORE_UFS);
        int no_ufs = card_arg || med[STORE_UFS].state <= 0;
        if (!sd_allowed() && (no_ufs || (ufs_settings_flags() & SET_SD_STORE)))
            sd_allow();
        if (sd_allowed() && (store_open(STORE_SD), med[STORE_SD].state > 0) && med[STORE_SD].version == 2 &&
            (med[STORE_SD].home || no_ufs)) {
            h = (int)STORE_SD;
            if (no_ufs) {
                probed_card_only = 1;
                why = card_arg ? "no table: sbstore=card" : "no table: no UFS store, the card's";
            }
        } else if (med[STORE_UFS].state > 0)
            h = (int)STORE_UFS;
    }
    if (h == (int)STORE_SD) {
        med[STORE_UFS].state = -1;     /* the card is home: the UFS store is closed for this boot */
        med[STORE_UFS].count = 0;
    }
    home = h;
    home_log(h, why);
    return home;
}
