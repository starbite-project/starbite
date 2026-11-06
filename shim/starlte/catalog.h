/* SPDX-License-Identifier: GPL-2.0-only
 * The boot catalog (catalog.c): every bootable entry, from the OS store's "catalog" entry
 * (tools/store/sbstore2.py from tools/store/catalog.toml), or built in when the store has none.
 *
 * "SBCATLG1", u32 version 1, u32 count, u32 entry size (64), u32 string pool offset, u32 total
 * size, u32 0; then count x 64 bytes: char key[24], u8 category, u8 method, u8 tag, u8 icon,
 * u16 flags, u16 order, u32 string offsets (from the catalog start, 0 = empty) of name, version,
 * file[4], cmdline, root. Strings are NUL terminated.
 *
 * A file field names store entries ("ufs:" / "sd:" pick the medium), with alternatives separated
 * by '|'; "@boot" is the SWEETXNU payload in BOOT, "@bundle" the "SBUEFI!!" bundle in BOOT.
 * Files by method: xnu payload; linux Image, DTB, initrd (optional); android kernel, ramdisk,
 * cmdline file (optional), DTB or DTBH (optional: replaces S-Boot's FDT, dtmerge.h); uefi image;
 * bare Image. cmdline is appended to the OS's own.
 *
 * Extension (2026-11-06; older loaders ignore it): the header's last u32 (+28, 0 before) is the
 * offset of "SBCATEX1", u32 version 1, u32 count (= the entry count), u32 entry size (32), u32 0,
 * then count x 32 bytes in entry order: u32 string offsets of the log-level fragments 0..3 (the
 * text appended to the command line for the global log level, docs/STARBITE-HEALTH.md), u16
 * watchdog seconds (0 = never armed), u8 feeder (CW_*), u8 flags (CE_*), 12 bytes zero.
 */
#pragma once
#include <stdint.h>

#define CAT_MAX 32U

#define CAT_APPLE     0U
#define CAT_MICROSOFT 1U
#define CAT_ANDROID   2U
#define CAT_LINUX     3U
#define CAT_STARBITE  4U
#define CAT_OTHER     5U
#define CAT_LAB       6U

#define M_XNU     1U
#define M_LINUX   2U
#define M_ANDROID 3U
#define M_UEFI    4U
#define M_BARE    5U   /* arm64 Image entered with x0 = S-Boot's FDT, the screen as it is */

#define TAG_NONE         0U
#define TAG_DAILY        1U
#define TAG_BETA         2U
#define TAG_EXPERIMENTAL 3U
#define TAG_LAB          4U
#define TAG_STABLE       5U

#define CF_HIDDEN 1U

/* Who keeps the hardware watchdog fed once the OS runs (catalog extension, u8 feeder). */
#define CW_NONE      0U   /* nobody: never arm it for this entry */
#define CW_KERNEL    1U   /* Linux: s3c2410_wdt.tmr_atboot=1 tmr_margin=T, the watchdog core pings it */
#define CW_USERSPACE 2U   /* Linux: as CW_KERNEL plus sb.wdt=T (the initrd hands it to systemd/busybox) */
#define CW_OWN       3U   /* the OS's own driver takes it over (Samsung's Android kernel) */
#define CW_XNU       4U   /* XNU with a feeder patch: sbwdt=T appended (docs/STARBITE-HEALTH.md 5.4) */
/* Extension flags (u8). */
#define CE_LINUX_INFORM3 1U   /* patch the Linux DTB's INFORM3 = recovery to normal, so a panic
                               * or watchdog reset comes back to the loader instead of RECOVERY */
#define CE_NO_LAUNCH     2U   /* no launch record (crash detection) for this entry */
#define CAT_LOG_MAX 128U

struct cat_ent {
    char key[24];
    char name[32];
    char version[48];
    char file[4][64];
    char cmdline[512];
    char root[64];
    char log[4][CAT_LOG_MAX];   /* command line fragment per log level 0..3 ("" = none) */
    uint8_t category, method, tag, icon;
    uint16_t flags, order;
    uint16_t wdt_s;             /* watchdog seconds, 0 = never */
    uint8_t wdt_feed;           /* CW_* */
    uint8_t cflags;             /* CE_* */
};

/* Read the catalog (once): the entry count. */
int cat_load(void);
unsigned cat_count(void);
const struct cat_ent *cat_get(unsigned i);
/* Index of the entry with this key, or -1. */
int cat_find(const char *key);
/* 1 when the store had no catalog and the built-in one is in use. */
int cat_builtin(void);
/* Alternative k of a file field ("a|b") into out (64 bytes): 1 if there is one. */
int cat_alt(const char *field, unsigned k, char *out);

/* xnu_boot.c: 1 if every file the entry needs is there; *why says what is missing. */
int star_entry_ready(unsigned i, const char **why);
