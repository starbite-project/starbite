/* SPDX-License-Identifier: GPL-2.0-only
 * The Starbite OS store (store.c, written by tools/store/sbstore2.py).
 *
 * One store per medium, the same layout on each, in 4 KiB blocks from the store's start:
 *   UFS     USERDATA +16 GiB, up to the end of USERDATA (2.75 GiB: Kiosk's old root is free)
 *   microSD MBR partition 2 (type 0xDA) of the card (sd.c); since 2026-11-11 the HOME store (docs/STORE-ON-CARD.md)
 *
 *   block 0  "SBSTORE1": the version 1 view of the entries that fit it (name < 24, below 1 GiB),
 *            for older loaders and the iOS overlay installer (xnu/src/starlte_overlay.c)
 *   block 1  "SBSTORE2" header: u32 version 2, u32 count, u64 capacity, u64 used (blocks),
 *            u32 dir_block, u32 dir_blocks, u32 settings_block, u32 dir CRC32, u32 CRC32 of 0..47
 *            + at bytes 64..91 the home extension: "SBSTHOME", u32 1, u32 flags (bit 0 HOME: the loader's
 *            settings and crash log live in this store), u64 the first data block, u32 CRC32 of 64..87
 *   block 2  settings record "SBSETTNG" (on the home store; the loader writes it, the tools keep it)
 *   block 3  directory: 64-byte entries: char name[32], u64 block, u64 size, u32 CRC32,
 *            u16 type, u16 flags, 8 bytes zero
 *   data     4 KiB aligned
 */
#pragma once
#include <stdint.h>

#define STORE_UFS   0U
#define STORE_SD    1U
#define STORE_MEDIA 2U
#define STORE_MAX   256U       /* directory entries per medium */

/* entry types */
#define ST_DATA    0U
#define ST_IMAGE   1U          /* arm64 Image */
#define ST_DTB     2U
#define ST_INITRD  3U
#define ST_TEXT    4U          /* command line */
#define ST_XNU     5U          /* SWEETXNU payload */
#define ST_UEFI    6U          /* bootshim + FD, an arm64 Image */
#define ST_CATALOG 7U

struct store_ent {
    char name[32];
    uint64_t lba;              /* absolute 4 KiB block on the medium */
    uint64_t size;
    uint32_t crc;
    uint16_t type, flags;
    uint8_t medium;
};

/* Directory of a medium, read once: entry count, 0 without a store. */
int store_open(unsigned medium);
/* 1 or 2 once opened, 0 without a store. */
int store_version(unsigned medium);
int store_get(unsigned medium, unsigned i, struct store_ent *e);
/* By name: "ufs:" or "sd:" picks the medium, "home:" only the home store, else the home store first, then the
 * other (UFS first when there is no home). 1 found. */
int store_find(const char *name, struct store_ent *e);
/* Whole entry to dst (at most cap bytes), CRC checked: 0, -1 read error, -2 CRC, -3 too large. */
int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap);
/* The last store_load since store_last_clear: its result (0, -1, -2, -3; 1 = none since the clear),
 * the entry's name (cut to cap), size, the directory's CRC32 and the one computed (0 unless read).
 * For the menu's error card ("expected 0x... · read 0x..."). */
int store_last_load(char *name, unsigned cap, uint64_t *size, uint32_t *want, uint32_t *got);
void store_last_clear(void);
/* The entry's first 4 KiB block to dst (no CRC): 0 or -1. */
int store_head(const struct store_ent *e, uintptr_t dst);
/* First block, capacity and blocks in use (header + data). */
uint64_t store_base(unsigned medium);
uint64_t store_capacity(unsigned medium);
uint64_t store_used(unsigned medium);

/* An OS's own volatile entry on the home store (flag 1, at least min_bytes, whole blocks): the fence's standing
 * grant for exactly its blocks (e->medium says which medium: write it with sb_write_volatile(e->medium, ...)).
 * 1 granted. */
int store_volatile_grant(const char *name, uint64_t min_bytes, struct store_ent *e);

/* Forget what store_open found, so the next call reads the medium again. */
void store_reset(unsigned medium);

/* ---- the home store (docs/STORE-ON-CARD.md, docs/STARBITE-FIRMWARE-LAYER.md 10.2), the loader's rule ----------
 * The store that holds the settings record (block 2), the crash log and the OS's volatile state:
 *   - with the loader's SBFW table (sb_fw.valid): the medium it names (store_medium), if that store opens as
 *     version 2 at the base the table gives; SBFW_MED_NONE or a mismatch: none. The table's "card only" flag
 *     (USERDATA released to iOS) closes the UFS store for good: never opened, read or written.
 *   - without one (older loaders): the card's store if the card may be touched (the UFS settings' SET_SD_STORE)
 *     and its header carries the HOME flag; else the UFS store, as before 2026-11-11.
 * When the card is home the UFS store is closed for this boot (never written, its entries not listed), as the
 * loader does. Returns STORE_UFS, STORE_SD or -1 (none). Decided once. */
int store_home(void);
/* 1 if medium m's version 2 header carries the HOME flag. */
int store_marked_home(unsigned medium);
/* 1 if libsb runs card only (the table's flag): the UFS store is never touched. */
int store_card_only(void);

/* Settings record (the home store's block 2). Keys are catalog keys ("" = none). */
#define SET_SD_STORE 1U        /* flags: read the microSD's store at boot (sd.h) */
struct star_settings {
    uint32_t seq, timeout_s, flags;
    char default_key[24], last_key[24], next_key[24];
};
/* POST level and log level, shared with the loader (shim/starlte/store.h; docs/STARBITE-HEALTH.md). 0 is the
 * default in both, so a record written before these existed means full POST and quiet logs. */
#define SET_POST_SHIFT 4U      /* 0 full (default), 1 quick, 2 off */
#define SET_POST_MASK  (3U << SET_POST_SHIFT)
#define SET_LOG_SHIFT  8U      /* 0 quiet (default), 1 normal, 2 verbose, 3 debug */
#define SET_LOG_MASK   (3U << SET_LOG_SHIFT)
/* 1 if a valid record was read; otherwise *s = defaults (zeros) and 0. */
int settings_load(struct star_settings *s);
/* Write the record (seq + 1), read back: 0, or < 0 / SB_W_*. Needs an open authorisation of kind
 * SB_KIND_STORE (the fence's gate). Keeps bytes 104..255 of the current record (the loader's crash / watchdog
 * fields and the SBFW part). */
int settings_save(struct star_settings *s);

/* ---- the SBFW part of the settings record (bytes 112..255; docs/STARBITE-FIRMWARE-LAYER.md 10.1), the
 * loader's codec byte for byte (shim/starlte/sbfmt.h) ---- */
#define SBSET_BYTES         256U  /* the record's bytes under its CRC (16..255) */
#define SBFW_FLAG_SHELL     1U    /* fw_flags: start the shell entry instead of drawing the menu */
#define SBFW_FLAG_CON_NEVER 2U
#define SBFW_FLAG_CON_ALWAYS 4U
#define SBFW_REQ_NONE       0U
#define SBFW_REQ_VERBOSE    1U
#define SBFW_REQ_SAFE       2U
#define SBFW_REQ_SINGLE     3U
#define SBFW_REQ_PREV       4U
#define SBFW_REQ_XNU_SINGLE 5U
#define SBFW_REQ_WINRE      6U
#define SBFW_REQ_DRIVER_OFF 7U
#define SBFW_REQ_NO_SHELL   8U
#define SBFW_REQ_MAX        8U
#define SBFW_ST_NONE        0U    /* req_state */
#define SBFW_ST_ARMED       1U    /* the OS asked (written by the OS) */
#define SBFW_ST_APPLIED     2U    /* its start had it */
#define SBFW_ST_DONE        3U    /* cleared (undone) at the next loader start */
#define SBFW_ST_REFUSED     4U    /* not possible for that entry, or it expired unused */
#define SBFW_ST_SEEN        5U    /* armed, and one loader start went by (the next one expires it) */
struct sbfw_rec {
    uint8_t fw_flags, req_action, req_state;
    uint16_t req_seq;
    char req_key[24], shell_key[24];
    uint8_t undo[64];
};
/* bytes 112..255 of a settings block: all zero when they do not start with "FW" version 1 */
void sbfw_rec_decode(const uint8_t *blk, struct sbfw_rec *r);
/* into bytes 112..255 of blk (the caller recomputes the record's CRC: sbset_crc_fix) */
void sbfw_rec_encode(uint8_t *blk, const struct sbfw_rec *r);
void sbset_crc_fix(uint8_t *blk);
/* The home record's SBFW part (all zero without a record or without "FW"): 1 if a valid record was read. */
int settings_fw_load(struct sbfw_rec *r);
/* s's fields plus r as the SBFW part (the loader's crash / watchdog bytes 104..111 kept), written and read back:
 * 0, or < 0 / SB_W_* as settings_save (the same authorisation). */
int settings_fw_save(struct star_settings *s, const struct sbfw_rec *r);
/* The loader's crash / watchdog bytes 104..111 of the home record (shim/starlte/store.h: 104 crash_mode, 105
 * crash_detail, 106 crash_action, 107 crash_timeout, 108 wdt_mode, 109 0, 110 u16 wdt_s), zeros without a record:
 * 1 if a valid record was read. */
int settings_ldr_load(uint8_t ldr[8]);
/* settings_fw_save with those 8 bytes replaced too (sbfw_api.c's sbfw_settings_write): 0, or < 0 / SB_W_*. */
int settings_all_save(struct star_settings *s, const struct sbfw_rec *r, const uint8_t ldr[8]);
/* Block k of entry e read (no CRC check: for volatile entries such as "crashlog") into dst (plain RAM, 4096 bytes):
 * 0, -1 read error, -3 past the entry. */
int store_read_block(const struct store_ent *e, uint64_t k, void *dst);

/* ---- writer (Engineer OS v2): append only, every block through the write fence, read back ---- */
struct store_writer {
    unsigned m, type;
    int err;
    uint64_t at, size, done;
    uint32_t crc;
    char name[32];
};
/* Free 4 KiB blocks for new entries (after the tail guard). */
uint64_t store_free(unsigned m);
/* Streaming append: begin (checks name, room), chunks (each a multiple of 4096 bytes except the
 * last; written and read back), commit (the whole entry read back, then the directory slot, then
 * the header: the entry exists from that one write on). 0 or < 0: -2 bad name, -3 exists,
 * -4 directory full, -5 no room, -6 sizes, -7 the store changed, -8 not found after the commit,
 * SB_W_* from the fence. Before the commit nothing a reader sees has changed. */
int store_wr_begin(struct store_writer *w, unsigned m, const char *name, unsigned type, uint64_t size);
int store_wr_chunk(struct store_writer *w, const void *src, uint64_t bytes);
int store_wr_commit(struct store_writer *w, struct store_ent *out);
int store_append(unsigned m, const char *name, unsigned type, const void *src, uint64_t size, struct store_ent *out);
/* Tier 2 directory rewrites (see store.c): remove / rename the entry at directory index idx. */
int store_remove(unsigned m, unsigned idx);
int store_rename(unsigned m, unsigned idx, const char *newname);
int store_dir_range(unsigned m, uint64_t *first, uint64_t *count);
/* Entry i read back and its CRC computed: 0 match, 1 mismatch, < 0 read error. */
int store_verify(unsigned m, unsigned i, uint32_t *got);
uint32_t store_crc32(const void *p, uint64_t n);
