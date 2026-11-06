/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite formats and decisions that need no hardware (sbfmt.c), so the host can test them
 * (tools/loader-test): the settings record codec, the crash log (store entry "crashlog"), the
 * reset-reason decision, the watchdog timeout math, ramoops ring text, command line edits, dates.
 * docs/STARBITE-HEALTH.md describes every format here; tools/store/sbstore2.py mirrors them.
 *
 * Plain C on byte arrays: no MMIO, no libc, no pointer tables (the loader Image must have no
 * absolute relocations, so names come from switch statements).
 */
#pragma once
#include <stdint.h>
#include "store.h"

/* zlib's CRC32 (IEEE, reflected), bit by bit: the same value as store.c's star_crc32. */
uint32_t sb_crc32(const void *p, uint64_t n);
uint32_t sb_crc32_more(uint32_t crc, const void *p, uint64_t n);   /* crc of a previous call */

/* ---- settings record: block 2 of the UFS store ------------------------------------------------
 *   0 "SBSETTNG"  8 u32 version 1  12 u32 CRC32 of bytes 16..255
 *  16 u32 seq  20 u32 timeout_s  24 u32 flags  28 u32 0
 *  32 default_key[24]  56 last_key[24]  80 next_key[24]
 * 104 u8 post_level  105 u8 log_level  106 u8 crash_mode  107 u8 crash_detail  108 u8 crash_action
 * 109 u8 crash_timeout  110 u8 wdt_mode  111 u8 0  112 u16 wdt_s  114..255 zero
 * Records written before 2026-11-06 have zeros from 104 on: every new field's 0 is its default. */
#define SBSET_BYTES 256U
int sbset_check(const uint8_t *blk);                 /* 1 valid, 0 no record (magic), -1 bad CRC/version */
void sbset_defaults(struct star_settings *s);
void sbset_decode(const uint8_t *blk, struct star_settings *s);   /* only call when sbset_check == 1 */
void sbset_encode(uint8_t *blk, const struct star_settings *s);   /* 4096 bytes, seq as given */
/* Effective values, clamped (bad bytes from a future tool read as defaults). */
unsigned sbset_post(const struct star_settings *s);         /* SET_POST_* */
unsigned sbset_log(const struct star_settings *s);          /* 0..3 */
unsigned sbset_crash_timeout(const struct star_settings *s);/* seconds, 10..600 */
const char *sbset_post_name(unsigned v);                    /* "FULL" ... */
const char *sbset_log_name(unsigned v);                     /* "QUIET" ... */

/* ---- reset reasons ---------------------------------------------------------------------------
 * PMU 0x14060000: RST_STAT +0x404 (why the SoC reset), AUTOMATIC_WDT_RESET_DISABLE +0x408,
 * MASK_WDT_RESET_REQUEST +0x40C, INFORM2 +0x808, INFORM3 +0x80C. Bit names: the Samsung 4.9
 * kernel's sec_debug_reset_reason.c (EXYNOS 9810). */
#define RST_PINRESET   (1U << 16)
#define RST_WDT_CL1    (1U << 23)   /* CLUSTER1_WDTRESET: the big cluster's watchdog, 0x10060000 */
#define RST_WDT_CL0    (1U << 24)   /* CLUSTER0_WDTRESET: the little cluster's watchdog, 0x10050000 */
#define RST_WRESET     (1U << 28)
#define RST_SWRESET    (1U << 29)
#define RST_WDT_ANY    (RST_WDT_CL0 | RST_WDT_CL1 | (1U << 19) | (1U << 20) | (1U << 22) | (1U << 25) | (1U << 26))
const char *sb_rst_bit_name(unsigned bit);   /* "SWRESET", 0 for a reserved bit */
/* S-Boot's own verdict, passed to every kernel as sec_debug.reset_reason=N (enum
 * sec_debug_reset_reason_t: 1 S .. 10 T); -1 when absent. */
#define RR_S 1
#define RR_W 2
#define RR_D 3
#define RR_K 4
#define RR_M 5
#define RR_P 6
#define RR_R 7
#define RR_B 8
#define RR_N 9
#define RR_T 10
const char *sb_rr_name(int rr);              /* "KPON KERNEL PANIC" */
const char *sb_inform3_name(uint32_t v);     /* "NORMAL", "RECOVERY", ... or 0 */
#define SEC_MAGIC_PANIC 0x66262564U          /* Samsung sec_debug: panic magic at PA 0x80000000 */
#define SEC_CAUSE_PANIC 0x000000C8U          /* ... and INFORM3 = UPLOAD_CAUSE_KERNEL_PANIC */

/* What the loader found after an OS ran (crash.c gathers it). */
#define EV_LX_PANIC  0x01U   /* Linux ramoops dmesg record "Panic#" not seen before */
#define EV_LX_OOPS   0x02U   /* ... "Oops#" */
#define EV_XNU_PANIC 0x04U   /* pmsg: "STARBITE: XNU panic (sweetreboot)" not seen before */
#define EV_XNU_HANG  0x08U   /* pmsg: "STARBITE: sweetreboot timer (no panic)" */
#define EV_AND_PANIC 0x10U   /* Samsung sec_debug panic magic / INFORM3 0xC8 */
#define EV_LX_OTHER  0x20U   /* a dmesg record with another reason (Emergency, BUG...) */
struct sb_evidence {
    uint32_t rst_stat, inform3;
    int32_t sboot_rr, sboot_pwron;   /* -1 absent */
    uint32_t ev;                     /* EV_* */
    uint8_t launched;                /* the crash log's launch record: an OS was started, not seen back */
    uint8_t method, wdt_armed;
};
#define CK_NONE      0U   /* no OS was running */
#define CK_CLEAN     1U   /* the OS rebooted itself */
#define CK_POWER     2U   /* cold start: the phone was off, or lost power */
#define CK_PANIC     3U
#define CK_WATCHDOG  4U   /* hardware watchdog reset: the OS hung */
#define CK_SWHANG    5U   /* the OS's own hang timer fired (XNU sweetreboot) */
#define CK_FORCED    6U   /* restart forced with the keys (Volume Down + Power): usually a hang */
#define CK_FAULT     7U   /* S-Boot's other crash-like codes */
#define CK_UNKNOWN   8U   /* a reset nobody explains */
#define CK_LOADERWDT 9U   /* a watchdog reset while no OS ran */
#define CK_OOPS      10U  /* Linux oops, the system went on */
#define SEV_INFO  0U      /* logged only */
#define SEV_WARN  1U      /* report saved, crash screen */
#define SEV_CRASH 2U
unsigned sb_classify(const struct sb_evidence *e, unsigned *severity);
const char *sb_kind_name(unsigned kind);     /* "KERNEL PANIC" */

/* ---- the crash log: store entry "crashlog" (flag SF_VOLATILE), 16 blocks of 4 KiB -------------
 * Block 0, the header (CRC32 of bytes 16..1023 at 12):
 *   0 "SBCRASH1"  8 u32 version 1  16 u32 slots (15)  20 u32 slot size (4096)  24 u32 next slot
 *  28 u32 reports written  32 u32 launches evaluated  36 u32 launch_seq  40 u32 launch_state
 *  44 u8 method  45 u8 watchdog armed  46 u16 watchdog s  48 key[24]  72 name[32]
 * 104 u64 launch epoch (UTC s, 0 unknown)  112 u32 log level  116 u32 launch flags (CLF_*)
 * 120 u32 seen CRC: dmesg record  124 console ring  128 sec_debug panic string  132 XNU pmsg block
 * Blocks 1..15, the ring of reports (CRC32 of bytes 16..4095 at 12):
 *   0 "SBCRREP1"  8 u32 version 1  16 u32 report number  20 u8 kind, severity, method, source
 *  24 u64 detected  32 u64 launched  40 u64 event (from the OS's log)   (UTC s, 0 unknown)
 *  48 key[24]  72 name[32]  104 u32 reg[16] (CR_*)  168 reason[96]  264 u16 text bytes
 * 266 u16 text lines  268 u32 0  272 text[3824]: the last log lines, oldest first, '\n' ended */
#define CL_BLOCK    4096U
#define CL_SLOTS    15U
#define CL_BYTES    (CL_BLOCK * (1U + CL_SLOTS))
#define CL_HEAD_CRC 1024U
#define CL_IDLE     0U
#define CL_RUNNING  1U
#define CLF_LINUX_INFORM3 1U   /* the Linux DTB's INFORM3 = recovery was patched to normal */
#define CLF_FASTBOOT      2U   /* started by `fastboot boot` */
#define CLF_RESCUE        4U   /* Starbite Rescue */
#define CLF_RETRY         8U   /* the automatic retry after a crash (no second retry) */
struct cl_head {
    uint32_t slots, slot_size, next, count, boots, launch_seq, launch_state;
    uint8_t method, wdt_armed;
    uint16_t wdt_s;
    char key[24], name[32];
    uint64_t launch_epoch;
    uint32_t log_level, lflags;
    uint32_t seen_dmesg, seen_console, seen_secmagic, seen_pmsg;
};
#define CR_TEXT_MAX 3824U
#define CR_RST_STAT  0
#define CR_INFORM2   1
#define CR_INFORM3   2
#define CR_WDT_DIS   3
#define CR_WDT_MASK  4
#define CR_SB_RR     5    /* int32, -1 absent */
#define CR_SB_RST    6
#define CR_SB_PWRON  7
#define CR_SB_PWROFF 8
#define CR_SEC_MAGIC 9
#define CR_WDT1_CON  10
#define CR_WDT1_DAT  11
#define CR_WDT1_CNT  12
#define CR_WDT0_CON  13
#define CR_LAUNCH_WDT 14
#define CR_FLAGS     15   /* launch flags (CLF_*) | evidence (EV_*) << 16 */
#define RS_NONE    0U
#define RS_DMESG   1U    /* Linux ramoops dmesg record (0xFED10000) */
#define RS_CONSOLE 2U    /* a console ring (Linux console 0xFED14000, Android's 0xFED10000) */
#define RS_SECDBG  3U    /* Samsung sec_debug panic string (0x80000004) */
#define RS_PMSG    4U    /* the pmsg ring 0xFED14000 (XNU's panic dump, the woa UEFI, the loader) */
#define RS_SBOOT   5U    /* only S-Boot's reset code */
struct cl_report {
    uint32_t seq;
    uint8_t kind, severity, method, source;
    uint64_t epoch_detect, epoch_launch, epoch_event;
    char key[24], name[32];
    uint32_t reg[16];
    char reason[96];
    uint16_t text_len, text_lines;
    char text[CR_TEXT_MAX];
};
void cl_head_init(struct cl_head *h);
int cl_head_decode(const uint8_t *blk, struct cl_head *h);     /* 1 valid, 0 not a header, -1 bad CRC */
void cl_head_encode(uint8_t *blk, const struct cl_head *h);    /* 4096 bytes */
int cl_report_decode(const uint8_t *blk, struct cl_report *r); /* 1 valid, 0 empty, -1 bad CRC */
void cl_report_encode(uint8_t *blk, const struct cl_report *r);/* 4096 bytes */
/* Slot of the k-th newest report (k = 0 newest), or -1. */
int cl_newest(const struct cl_head *h, unsigned k);
/* The report's text = the last lines of src (len bytes) that fit, cut at a line start; *lines. */
unsigned cl_tail(char *dst, unsigned cap, const char *src, unsigned len, uint16_t *lines);
const char *sb_source_name(unsigned src);

/* ---- the hardware watchdog (S3C2410 type, OSCCLK 26 MHz) --------------------------------------
 * WTCON = prescaler 255 << 8 | DIV128 (3 << 3) | ENABLE (1 << 5) | RSTEN (1 << 0) = 0xFF39:
 * 26 MHz / 256 / 128 = 793.457 Hz, a 16-bit WTCNT: at most 65535 counts = 82.6 s. */
#define WDT_CLK_HZ  26000000U
#define WDT_WTCON   0xFF39U
#define WDT_MIN_S   10U
#define WDT_MAX_S   82U
/* Counts for secs (clamped to WDT_MIN_S..WDT_MAX_S); *ms = the real timeout. 0 for secs 0. */
uint32_t sb_wdt_count(unsigned secs, unsigned *ms);

/* ---- ramoops rings ---------------------------------------------------------------------------
 * persistent_ram zone: u32 "DBGC" (0x43474244), u32 start (next write), u32 size (bytes held),
 * data. Written by Linux (data = zone - 12), the loader, the woa UEFI and XNU (data 0x3F00).
 * The text oldest first into out (cap bytes, not NUL-terminated); returns its length, 0 if the
 * zone holds no valid ring. */
#define RING_SIG  0x43474244U
#define RING_PMSG 0x3F00U
unsigned sb_ring_text(const uint8_t *zone, unsigned zone_size, char *out, unsigned cap);

/* ---- text helpers ------------------------------------------------------------------------------ */
/* Offset of needle in hay[0..n), or -1. */
int sb_find(const char *hay, unsigned n, const char *needle);
/* The rest of the line after needle (first match), cut to cap - 1: 1 found. */
int sb_line_after(const char *hay, unsigned n, const char *needle, char *out, unsigned cap);
/* The line containing offset at, to out: its length. */
unsigned sb_line_at(const char *hay, unsigned n, unsigned at, char *out, unsigned cap);
/* key=value of a kernel command line (args NUL- or len-terminated): 1 and the value, else 0. */
int sb_arg(const char *args, unsigned len, const char *key, char *out, unsigned cap);
/* Decimal or 0x hex: 1 if s was a number. */
int sb_num(const char *s, int64_t *v);
/* Edit a command line: each space-separated token of frag is appended; with allow_remove a token
 * "!name" first removes every "name" and "name=..." token of line. Returns the new length (stops
 * appending when cap would be exceeded; line stays NUL-terminated). */
unsigned sb_line_edit(char *line, unsigned cap, const char *frag, int allow_remove);
/* "2026-11-06 14:03" (UTC) for a Unix time; "UNKNOWN TIME" for 0. out: 20 bytes. */
void sb_date(uint64_t epoch, char *out);
/* Linux ramoops record header "====<sec>.<usec>-<C|D>": the seconds, or 0. */
uint64_t sb_dmesg_time(const char *text, unsigned n);
