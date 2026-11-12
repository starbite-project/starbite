/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS's side of the firmware layer (docs/STARBITE-FIRMWARE-LAYER.md section 10): the loader's settings as
 * one struct, the one-shot requests, the boot menu / Starbite Recovery once, the crash reports of the home store's
 * "crashlog" (sbfw_api.c). Every write goes to the home store's settings record (block 2) and is read back; like
 * settings_save it needs an open authorisation of kind SB_KIND_STORE (the caller's confirmed action).
 */
#pragma once
#include <stdint.h>
#include "store.h"

/* the loader's values (shim/starlte/store.h SET_*; docs/STARBITE-HEALTH.md) */
#define SBFW_DEBUG_OFF     0U      /* debug_level: flags bits 8-9 */
#define SBFW_DEBUG_NORMAL  1U
#define SBFW_DEBUG_HIGH    2U
#define SBFW_DEBUG_HIGHEST 3U
#define SBFW_POST_FULL     0U      /* post: flags bits 4-5 */
#define SBFW_POST_QUICK    1U
#define SBFW_POST_OFF      2U
#define SBFW_WDT_ENTRY     0U      /* wdt_mode: each catalog entry's own timeout */
#define SBFW_WDT_OFF       1U
#define SBFW_WDT_FIXED     2U      /*   wdt_s for every entry whose catalog timeout is not 0 */
#define SBFW_WDT_MIN_S     10U
#define SBFW_WDT_MAX_S     82U     /* the 16-bit WTCNT at 793 Hz */
#define SBFW_CRASH_ON      0U      /* crash_mode: detect, save, the crash screen */
#define SBFW_CRASH_SAVE    1U      /*   detect and save, no screen */
#define SBFW_CRASH_OFF     2U
#define SBFW_AFTER_MENU    0U      /* crash_action */
#define SBFW_AFTER_RECOVERY 1U
#define SBFW_AFTER_RETRY   2U      /*   the same entry again, once */

/* errors (besides store.h / SB_W_* from the write) */
#define SBFW_E_NOHOME  -1          /* no home store (no settings record to write) */
#define SBFW_E_INVAL   -2          /* a value out of range, an unknown or plan-only request */
#define SBFW_E_NOFIT   -3          /* no such catalog key, or the request is not for its kind of OS */
#define SBFW_E_BUSY    -4          /* the last request is applied and its undo still pending (the loader's) */
#define SBFW_E_NOLOG   -5          /* no crash log on the home store, or not a valid one */

struct sbfw_settings {
    char default_key[24];          /* "" none: the menu waits */
    uint32_t timeout_s;            /* the menu's timeout before the default starts (0 = wait) */
    uint8_t debug_level;           /* SBFW_DEBUG_* */
    uint8_t post;                  /* SBFW_POST_* */
    uint8_t wdt_mode;              /* SBFW_WDT_* */
    uint8_t crash_mode;            /* SBFW_CRASH_* */
    uint8_t crash_action;          /* SBFW_AFTER_* */
    uint8_t crash_detail;          /* the crash screen's first level, 0..3 */
    uint8_t shell;                 /* 1: start shell_key instead of drawing the menu (fw_flags bit 0) */
    uint16_t wdt_s;                /* SBFW_WDT_FIXED: 10..82 */
    char shell_key[24];            /* "" = "starbite-os" */
};
/* The home record's settings (defaults, all zero, without one): 1 if a valid record was read, 0 defaults. */
int sbfw_settings_read(struct sbfw_settings *s);
/* Load the record, change these fields, write it back (every other field and the request fields kept), read back:
 * 0, SBFW_E_NOHOME, SBFW_E_INVAL, or < 0 / SB_W_* from the write. */
int sbfw_settings_write(const struct sbfw_settings *s);

/* The loader's mailbox keys (10.3): the plain boot menu once / Starbite Recovery (never return). */
void sb_boot_menu_next(void) __attribute__((noreturn));
void sb_boot_recovery_next(void) __attribute__((noreturn));

/* 1 if request req (SBFW_REQ_*) can apply to an entry of catalog method m (catalog.h M_*): the loader's
 * sbfw_req_fits byte for byte (WINRE plan only, NO_SHELL not a start's request: 0). */
int sbfw_req_fits(unsigned req, unsigned method);
/* "verbose", "safe" ... or 0 for an unknown one (the loader's names) */
const char *sbfw_req_name(unsigned req);
/* Arm a one-shot for key's next start: req_action, req_key, req_state = ARMED, req_seq + 1, written and read back;
 * nothing restarts. 0, SBFW_E_INVAL (unknown, WINRE, NO_SHELL), SBFW_E_NOFIT, SBFW_E_BUSY, SBFW_E_NOHOME or the
 * write's error. */
int sbfw_request_arm(const char *key, unsigned action);
/* sbfw_request_arm, then sb_boot_next(key): returns only when refused (< 0). */
int sbfw_request(const char *key, unsigned action);
/* The last request: *action, *state (SBFW_ST_*), key (24 bytes); returns its req_seq (0: none ever). */
int sbfw_request_status(unsigned *action, unsigned *state, char key[24]);

/* Crash reports (the home store's "crashlog": block 0 the head, blocks 1..15 the ring of reports, as the loader's
 * shim/starlte/sbfmt.h cl_head / cl_report). */
struct sbfw_crash {
    char key[24], os[32];          /* the entry's catalog key and name */
    char kind[24];                 /* "KERNEL PANIC" (the loader's sb_kind_name) */
    char line[96];                 /* the first message line (the report's reason, else its first text line) */
    unsigned kind_id, severity, method, seq;   /* severity: 0 info, 1 warning, 2 crash */
    uint64_t epoch;                /* UTC s: when it happened (the OS's log), else detected, else launched; 0 unknown */
};
/* Reports there (0..15), or SBFW_E_NOLOG. */
int sbfw_crash_count(void);
/* Report i (0 = the newest): 0, SBFW_E_NOLOG, SBFW_E_INVAL (no such one), -1 read error, -6 a bad report. */
int sbfw_crash_get(unsigned i, struct sbfw_crash *c);
/* the loader's crash kind names (sbfmt.c sb_kind_name) */
const char *sbfw_kind_name(unsigned kind);

/* ---- update slots (docs/STARBITE-FIRMWARE-LAYER.md 10.5) ----------------------------------------------------
 * Two volatile store entries "sbos-slot-a" (1) / "sbos-slot-b" (2): block 0 the header (sbslot_build), the image from
 * +4096; a row whose files are "@slot|<normal file>" starts slot_try during an update's trial start, else
 * slot_active, else its normal file. The OS stages into the inactive slot (store_volatile_grant, 4 MiB chunks,
 * the header last), sbfw_update_arm, sb_boot_next(key); once the new image is up it calls sbfw_update_commit.
 * Without the commit the loader's next start rolls back (slot_try cleared, req_state REFUSED). */
int sbfw_slot_get(unsigned *active, unsigned *try_, unsigned *tries);      /* 1 if a record was read */
int sbfw_slot_set(unsigned active, unsigned try_, unsigned tries);         /* 0, SBFW_E_INVAL / _NOHOME / SB_W_* */
int sbfw_update_arm(const char *key, unsigned slot);   /* slot_try = slot, REQ_UPDATE armed for key: 0 or SBFW_E_* */
int sbfw_update_commit(void);                          /* this start's trial becomes slot_active: 0 or SBFW_E_* */
struct sbslot_info { uint32_t version; uint64_t size; uint32_t crc; char vstr[32], key[24]; };
/* header block: "SBSLOT01", u32 1, u32 CRC32(16..4095), u64 image size, u32 image CRC32 (zlib), u32 0,
 * char version[32] at 32, char key[24] at 64 (the catalog row) */
int sbslot_check(const uint8_t *hdr, uint64_t entry_bytes, const char *key, struct sbslot_info *si);
void sbslot_build(uint8_t *hdr4k, const struct sbslot_info *si);
