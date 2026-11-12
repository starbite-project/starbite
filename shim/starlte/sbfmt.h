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
 *  16 u32 seq  20 u32 timeout_s  24 u32 flags (store.h: bit 0 card store, bits 4-5 POST level,
 *     bits 8-9 log level)  28 u32 0
 *  32 default_key[24]  56 last_key[24]  80 next_key[24]
 * 104 u8 crash_mode  105 u8 crash_detail  106 u8 crash_action  107 u8 crash_timeout (s / 10)
 * 108 u8 wdt_mode  109 u8 0  110 u16 wdt_s  112..255 zero
 * Records written before 2026-11-06 have zeros from 104 on and flags bits 4-9 clear: every new
 * field's 0 is its default. */
#define SBSET_BYTES 256U
int sbset_check(const uint8_t *blk);                 /* 1 valid, 0 no record (magic), -1 bad CRC/version */
void sbset_defaults(struct star_settings *s);
void sbset_decode(const uint8_t *blk, struct star_settings *s);   /* only call when sbset_check == 1 */
void sbset_encode(uint8_t *blk, const struct star_settings *s);   /* 4096 bytes, seq as given */
/* Effective values, clamped (bad bytes from a future tool read as defaults). */
unsigned sbset_post(const struct star_settings *s);         /* SET_POST_* (3 reads as full) */
unsigned sbset_log(const struct star_settings *s);          /* SET_LOG_* 0..3 */
void sbset_set_post(struct star_settings *s, unsigned v);   /* flags bits 4-5 */
void sbset_set_log(struct star_settings *s, unsigned v);    /* flags bits 8-9 */
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
#define CLF_MAINT        16U   /* Starbite Maintenance (docs/MAINTENANCE.md) */
struct cl_head {
    uint32_t slots, slot_size, next, count, boots, launch_seq, launch_state;
    uint8_t method, wdt_armed;
    uint16_t wdt_s;
    char key[24], name[32];
    uint64_t launch_epoch;
    uint32_t log_level, lflags;
    uint32_t seen_dmesg, seen_console, seen_secmagic, seen_pmsg;
    uint32_t hflags;                /* 136: CLH_* */
};
#define CLH_CONSUMES 1U             /* judged evidence is marked in RAM (sb_consume): present = new */
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
/* Where text byte i of sb_ring_text(zone) lies in the zone's data (zone + 12 + it), or -1. */
int sb_ring_phys(const uint8_t *zone, unsigned zone_size, unsigned i);

/* ---- evidence: new, or judged already (crash.c evaluate / settle) ------------------------------
 * The phone, 2026-11-06: two runs of the same deterministic XNU test panic left byte-identical
 * dumps, and the second was taken for the first (the seen-CRC check): "CLEAN RESTART", no report.
 * So the loader now marks every dmesg record and XNU dump it judged in RAM itself ("consumes" it):
 *   Linux ramoops dmesg record (zone 0)  header "====<sec>.<usec>-D" (or -C) becomes "...-S": Linux
 *                                        still reads the record as before (only 'C' means
 *                                        compressed), the loader no longer takes it for new
 *   XNU's dump (pmsg ring, zone 1)       "STARBITE: XNU panic ..." / "STARBITE: sweetreboot timer"
 *                                        becomes "STARSEEN: ..." (the loader's needles no longer
 *                                        match; the text stays readable)
 * A new crash writes a new header, so even an identical one is found. Old evidence (an
 * unrelated reset, any number of boots later) stays marked, so it is never reported again.
 * Evidence from before the first consuming loader (cl_head.hflags without CLH_CONSUMES) is still
 * checked against the seen CRCs once. */
struct sb_found {
    uint32_t ev;                    /* EV_LX_* / EV_XNU_* found new for the launched method */
    int dmesg;                      /* zone 0 holds a live (unjudged) dmesg record */
    unsigned dflag;                 /* the text offset of its header's flag character */
    int xat;                        /* zone 1 text offset of XNU's live dump header, -1 none */
    int xpanic;                     /* 1: the panic header, 0: the timer's */
    uint32_t c0, cx;                /* CRCs as kept in seen_dmesg / seen_pmsg */
};
/* t0/t1: the zones' text (sb_ring_text of the snapshot); xp/xh: where crash.c's needles for XNU's
 * panic and timer headers matched t1 (sb_find), -1 none; is_linux / is_xnu: the launched OS. */
void sb_evidence(const char *t0, unsigned n0, const char *t1, unsigned n1, int xp, int xh, int is_linux, int is_xnu,
                 const struct cl_head *h, struct sb_found *f);
/* Mark what f found as judged in the live zones (live0/live1: the RAM the snapshot snap0/snap1
 * was copied from; bytes are changed only where they still hold what the snapshot had: the
 * loader's own pmsg lines may have overwritten XNU's header since), update h (seen CRCs,
 * CLH_CONSUMES). Returns 1 dmesg marked | 2 XNU header marked. */
unsigned sb_consume(volatile uint8_t *live0, volatile uint8_t *live1, const uint8_t *snap0, const uint8_t *snap1,
                    unsigned zone_size, const struct sb_found *f, struct cl_head *h);

/* ---- Starbite Maintenance: the SBMAINT1 container (docs/MAINTENANCE.md 3 and 4.2 R4) ------------
 * One slot: a 4096-byte header, then the sections. Header: "SBMAINT1", u32 version 1, u32 header
 * size 4096, u32 seq (>= 1), u32 flags, u64 total, u64 build time, 24 reserved, at 64 four
 * sections {u64 offset, u64 size, u32 crc32, u32 kind, u64 0} (kernel 1, dtb 2, initrd 3,
 * dtb-rescue 4), at 192 cmdline[256], at 448 version[64], at 512 SHA-256s, at 4092 the CRC32 of
 * bytes 0..4091. tools/maint/sbmaint.py is the reference (parse_header, choose). */
#define SBM_HDR        4096U
#define SBM_STAGE_MAX  0x04FF0000U   /* the loader's download buffer: a slot is staged whole */
#define SBM_KERNEL     0U
#define SBM_DTB        1U
#define SBM_INITRD     2U
#define SBM_DTB_RESCUE 3U
struct sbm_info {
    uint32_t seq;
    uint64_t total;
    uint64_t off[4], size[4];
    uint32_t crc[4];
    char cmdline[256], version[64];
};
/* The checks the loader makes on a slot's header before it reads the body (limit: the slot's size,
 * never more than SBM_STAGE_MAX): 1 valid, 0 with *why. */
int sbm_check(const uint8_t *h, uint64_t limit, struct sbm_info *m, const char **why);
/* Slot B's byte offset in a partition of part_bytes: half, rounded down to 1 MiB (slot A: 0). */
uint64_t sbm_slot_b(uint64_t part_bytes);
/* The order to try the slots (0 A, 1 B): valid ones only, higher seq first, A before B on a tie;
 * prev ("maint-prev") reverses it. Returns how many (0..2). */
unsigned sbm_order(const int valid[2], const uint32_t seq[2], int prev, unsigned out[2]);

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
/* The last "key=value" of a command line (Linux's module parameters and XNU's PE_parse_boot_argn
 * take the last one): 1 and the value, else 0. */
int sb_arg_last(const char *args, unsigned len, const char *key, char *out, unsigned cap);

/* ---- the watchdog's kernel arguments (xnu_boot.c launch_prepare / boot_xnu) -------------------
 * sb_line_edit drops tokens that do not fit (XNU's boot-args hold 607 bytes), so the loader checks
 * the finished command line before it arms the watchdog: 1 if line carries what makes the OS feed
 * a watchdog of wdt seconds, 0 if not (then nothing is armed). */
#define SB_WDT_XNU   1U     /* sbwdt=N (xnu/series-12377/0399) */
#define SB_WDT_LINUX 2U     /* s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=N */
int sb_wdt_args_ok(const char *line, unsigned len, unsigned how, unsigned wdt);

/* ---- POST presentation (post.c, docs/STARBITE-HEALTH.md 2.2) ------------------------------------
 * The full POST shows the Starbite logo (PV_LOGO). A Volume Up press that STARTS during POST opens
 * the live log (PV_LOG) for the rest of the run. Volume Up already held when POST starts is the
 * loader's fastboot gesture (fastboot_dev.c fastboot_wanted, which runs after POST): it is ignored
 * until the key has been seen released. Volume Down held skips the rest of POST. Times in ms. */
#define PV_LOGO 0U
#define PV_LOG  1U
#define PV_NONE 2U              /* nothing drawn: quick POST over S-Boot's splash */
#define PV_DEBOUNCE_MS 40U      /* Volume Up counts once down (or released) this long */
#define PV_SKIP_MS     20U      /* Volume Down counts once down this long */
#define PV_READ_MS     2000U    /* the log view stays this long after the run ... */
#define PV_HOLD_MAX_MS 60000U   /* ... or while Volume Up (pressed during POST) is held, at most this */
#define PV_FAIL_WAIT_S 30U      /* the failure screen continues on its own after this */
struct pv_keys {
    uint8_t view;               /* PV_* now */
    uint8_t up_blocked;         /* Volume Up was down at the start and not yet seen released */
    uint8_t up_used;            /* a Volume Up press during POST (the log was asked for) */
    uint8_t skip;               /* Volume Down: the rest is skipped */
    uint64_t up_since, up_free_since, down_since;   /* ms + 1 when first seen, 0 = not */
};
#define PV_EV_LOG  1U           /* pv_poll: switch to the log view now */
#define PV_EV_SKIP 2U           /* pv_poll: skip the rest of POST now */
void pv_init(struct pv_keys *k, unsigned view, int up_at_start, uint64_t now_ms);
unsigned pv_poll(struct pv_keys *k, int up, int down, uint64_t now_ms);
/* What follows the run: the failure screen if a check failed (red), else the log view's pause to
 * read (the log view only), else straight on. Warnings (gold) never stop the logo. */
#define PV_END_GO   0U
#define PV_END_READ 1U
#define PV_END_FAIL 2U
unsigned pv_end(const struct pv_keys *k, unsigned fails);
/* PV_END_READ, t ms after the run ended: 1 = keep the log on the panel. Power or Volume Down
 * (cut) ends it; Volume Up held keeps it (only a press made during POST, at most PV_HOLD_MAX_MS). */
int pv_hold(const struct pv_keys *k, int up, int cut, uint64_t t_ms);
/* The failed checks for the failure screen, in display order: the ids in order[0..n) whose
 * state[id] is fail_state, at most max into out. Returns how many failed (all of them). */
unsigned pv_failures(const uint8_t *state, const uint8_t *order, unsigned n, unsigned fail_state,
                     uint8_t *out, unsigned max);

/* ---- keys from USB (`fastboot oem key`, fastboot_dev.c) ----------------------------------------
 * Presses queue up; each is held for its time, then released for KQ_GAP_MS, then the next. The
 * menus read them together with the real keys (menu.c keys(), recovery.c r_keys()), so each
 * menu's own debounce, repeat and hold rules apply unchanged. KQ_PRESS_MS is a plain press for
 * every menu: longer than their debounce (menu v7 20 ms, recovery 30, crash report 20) and
 * shorter than any hold or repeat (v7 Power hold 520 ms, Volume repeat 450, crash report repeat
 * 300, Bixby hold 700). A longer time (`oem key bixby 900`) makes a hold. */
#define KQ_N 8U
#define KQ_PRESS_MS 150U
#define KQ_GAP_MS 120U
#define KQ_MAX_MS 5000U
struct kq {
    uint16_t bits[KQ_N], ms[KQ_N];
    unsigned h, n;
    uint64_t t0;                    /* ms + 1 when the head press began, 0 = not begun */
};
void kq_init(struct kq *k);
/* Queue a press of key bits (K_* of hw.h) for ms (0: KQ_PRESS_MS, at most KQ_MAX_MS); 0 if full. */
int kq_push(struct kq *k, unsigned bits, unsigned ms);
/* The injected keys at now_ms (0 when idle: the fast path). */
unsigned kq_bits(struct kq *k, uint64_t now_ms);
/* "up" "down" "bixby" "power" (also "volup" "voldown" "vol+" "vol-"): the K_* bit, 0 if unknown. */
unsigned kq_key_bit(const char *name);

/* ---- the menus' background USB link: when to start, serve, re-link (fastboot_dev.c fbbg_*) -----
 * One owner for the whole boot: the crash report, the boot menu and Starbite Recovery poll the
 * same link. The phone, health5: after the crash report the menu's intro and wallpaper (about
 * 3 s) polled nothing; the Mac's command in that time made it reset the device, nobody answered
 * the new enumeration, macOS gave up on the port and `fastboot devices` stayed empty at the menu.
 * So a link not served for BGL_GAP_MS is restarted (soft disconnect, PHY and core again: the
 * host sees a fresh attach) before it is served again; a start that failed is retried after
 * BGL_RETRY_MS, doubling to BGL_RETRY_MAX_MS; a link that VBUS says is plugged in but no host has
 * configured for BGL_UNCONF_MS (doubling to the same cap) is restarted too. Times in ms. */
#define BGL_GAP_MS       750U
#define BGL_RETRY_MS     5000U
#define BGL_UNCONF_MS    10000U
#define BGL_RETRY_MAX_MS 60000U
#define BGL_IDLE   0U               /* nothing to do: not wanted, or a failed start waits for its retry */
#define BGL_START  1U               /* bring the link up now (bgl.routed 0: route D+/D- first) */
#define BGL_RELINK 2U               /* stop it and bring it up again first */
#define BGL_SERVE  3U               /* serve it */
struct bgl {
    uint8_t wanted, up, failed, routed;
    uint64_t last;                  /* ms + 1 of the last poll that served it, 0 = never */
    uint64_t retry_at, unconf_since;    /* ms + 1, 0 = none */
    uint32_t retry_ms, unconf_ms;
    uint32_t starts, relinks, fails;    /* for `oem usb` and the host test */
};
void bgl_init(struct bgl *b);
void bgl_enable(struct bgl *b, int on);         /* wanted (fbbg_enable / fbbg_off) */
/* A poll at now_ms: what to do first. configured: a host has configured the link; vbus: a cable. */
unsigned bgl_poll(struct bgl *b, uint64_t now_ms, int configured, int vbus);
/* After BGL_START / BGL_RELINK: ok or not. A success also counts as routed and served. */
void bgl_result(struct bgl *b, unsigned what, int ok, uint64_t now_ms);
void bgl_served(struct bgl *b, uint64_t now_ms);    /* the poll served it (BGL_SERVE, after a start) */
void bgl_stopped(struct bgl *b);                    /* the loader stopped the link (fbbg_stop) */

/* ---- what a crashed OS left: the crash screen's parser (crash.c) -------------------------------
 * Text from the ramoops dmesg record or the console ring (Linux: lines "<N>[ secs.usecs] text",
 * the prefix optional) or XNU's panic dump in the pmsg ring (docs/STARBITE-HEALTH.md 5.4). Each
 * line gets a kind; the header fields come from the lines. */
#define CT_LOG   0U             /* any other line */
#define CT_MSG   1U             /* the panic / oops message: "Kernel panic - not syncing:", "Internal
                                 * error:", "Unable to handle kernel", "BUG:", the abort info block,
                                 * XNU "panic(cpu ..." */
#define CT_INFO  2U             /* who and what: "CPU: n ... PID: ... Comm:", "Hardware name:", "Modules
                                 * linked in:", "Linux version"; XNU "Kernel
                                 * version:", "Panicked task", the STARBITE marker lines */
#define CT_REGS  3U             /* CPU registers: "pc :", "lr :", "sp :", "pstate:", "xNN:", "Code:";
                                 * XNU "x0:", "pc:", "cpsr:", "CORE n: PC=" */
#define CT_TRACE 4U             /* the call trace: "Call trace:" and its frames; XNU "Panicked
                                 * thread: ..., backtrace:" and its "lr: ... fp: ..." lines */
struct sb_crashinfo {
    char version[64];           /* kernel release ("7.1.0-starbite-23527494", "xnu-12377...") */
    char task[64];              /* "CPU 4 PID 1234 sh", "CPU 2 PID 0 kernel_task" */
    char message[160];          /* the panic message line, without its prefix */
    uint64_t uptime_ms;         /* at the panic (its timestamp, XNU's "uptime N s"), 0 unknown */
    unsigned lines, msg, info, regs, trace;   /* lines in all and of each kind */
};
/* Parse text[0..n): fills *ci; kinds[i] = the kind of line i for i < cap (kinds may be 0).
 * Returns the number of lines. */
unsigned sb_crash_parse(const char *text, unsigned n, struct sb_crashinfo *ci, uint8_t *kinds, unsigned cap);
/* Bytes of a line's "<N>" syslog prefix (0 if none): the screen and the report leave it out. */
unsigned sb_crash_prefix(const char *line, unsigned n);
/* Bytes of the "[ secs.usecs] " time after it (0 if none): the screen's extracted sections leave
 * it out (the log keeps it). */
unsigned sb_crash_stamp(const char *line, unsigned n);
/* The saved report's text (cl_report.text): every message, info, register and trace line, then as
 * many of the last lines as fit, in their original order, without "<N>" prefixes; skipped lines
 * become one "[...]" line. Like cl_tail otherwise (printable ASCII, '\n' ended). */
unsigned cl_excerpt(char *dst, unsigned cap, const char *src, unsigned len, uint16_t *lines);

/* ---- SBFW: the firmware layer's part of the settings record (bytes 112..255) and the table it
 * hands to every OS at 0x91FFE000 (docs/STARBITE-FIRMWARE-LAYER.md section 10) ---------------- */
#define SBFW_FLAG_SHELL     1U   /* start the shell entry instead of drawing the menu */
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
#define SBFW_ST_NONE        0U   /* req_state */
#define SBFW_ST_ARMED       1U   /* the OS asked (written by the OS) */
#define SBFW_ST_APPLIED     2U   /* its start had it */
#define SBFW_ST_DONE        3U   /* cleared (undone) at the next loader start */
#define SBFW_ST_REFUSED     4U   /* not possible for that entry, or it expired unused */
#define SBFW_ST_SEEN        5U   /* armed, and one loader start went by (the next one expires it) */
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
const char *sbfw_req_name(unsigned req);      /* "verbose", "safe" ... 0 for an unknown one */
/* What a request means for an entry of method m (catalog.h M_*): 1 can apply, 0 not for that kind */
int sbfw_req_fits(unsigned req, unsigned method);

#define SBFW_TABLE_PA   0x91FFE000UL
#define SBFW_TABLE_SIZE 4096U
#define SBFW_WHY_MENU     0U
#define SBFW_WHY_AUTO     1U
#define SBFW_WHY_NEXT     2U
#define SBFW_WHY_SHELL    3U
#define SBFW_WHY_RETRY    4U
#define SBFW_WHY_FASTBOOT 5U
#define SBFW_TF_CARD_ONLY 1U
#define SBFW_TF_SHELL     2U
#define SBFW_LINES        24U
#define SBFW_LINE         120U
struct sbfw_table_in {
    uint32_t debug_level, req_action;
    const char *key;
    uint32_t console_y0, console_y1;
    uint64_t fb_base;
    uint32_t stride, fb_fmt, loader_version, boot_reason;
    uint32_t store_medium, store_flags;
    uint64_t store_base, store_capacity;
    uint32_t crash_kind, crash_sev;
    const char *crash_key;
    uint32_t post_fails, post_warns, settings_status, flags;
};
/* The 4 KiB table into page (header, fields, CRC); lines (newest last, '\n' separated text of n
 * bytes) fill the last-lines area with its last SBFW_LINES lines. */
void sbfw_table_build(uint8_t *page, const struct sbfw_table_in *in, const char *lines, unsigned n);
/* 1 if page holds a valid table (magic, version, CRC): what libsb checks */
int sbfw_table_check(const uint8_t *page);
/* The request's state at a loader start (armed -> seen, seen -> refused (expired), applied -> done);
 * the same state if nothing changes. */
unsigned sbfw_age(unsigned state);
/* Shell mode's decision (section 10.1), as plain inputs. Returns SBFW_SH_START or why not. */
#define SBFW_SH_START     0U
#define SBFW_SH_OFF       1U
#define SBFW_SH_SKIP      2U   /* Volume Down or the "menu" key */
#define SBFW_SH_NO_SHELL  3U   /* the no-shell one-shot */
#define SBFW_SH_CRASH     4U   /* the last start crashed: the menu shows it */
#define SBFW_SH_NEXT      5U   /* a boot-next key or a one-shot next entry */
#define SBFW_SH_MISSING   6U   /* no shell entry in the catalog */
#define SBFW_SH_NOT_READY 7U
unsigned sbfw_shell_decide(unsigned fw_flags, int skip, unsigned req_state, unsigned req_action, int crash_hold,
                           int next_pending, int shell_index, int shell_ready);
const char *sbfw_shell_why(unsigned d);

/* A store's head (blocks 0..6, 28 KiB: v1 view, SBSTORE2 header, settings, directory) checked as
 * the loader opens it: header magic/version/CRC, the directory layout and CRC, the home extension's
 * CRC when present, capacity == cap (0: any). 1 valid (fills what it can), 0 not. */
struct sbhead_info { uint32_t count; uint64_t capacity, used, data_block; int home; };
int sbstore_head_check(const uint8_t *head, uint64_t cap, struct sbhead_info *hi);
