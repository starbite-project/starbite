/* SPDX-License-Identifier: GPL-2.0-only
 * Engineer OS (docs/ENGINEER-OS.md): the application on libsb.
 *
 * Screens keep shim/starlte/em.c's model so its pages port with few changes: a page's fill()
 * calls kv_hdr / kv_add / kv_big / kv_mono (key/value pages), build() calls row_add (lists),
 * tv_line fills a monospace text viewer; info_screen / list_screen / text_screen run the screen
 * on sbui's retained page (only changed rows are redrawn, scrolling moves pixels).
 */
#pragma once
#include "sbui_w.h"
#include "ufs.h"
#include "store.h"
#include "catalog.h"
#include "sd.h"

#define ENG_VERSION "2.1"
#define ENG_BUILD "Starbite Lab (v2.1: tiers 1-4, stress, thermal, battery, GPU, utilities)"

/* em.c's colour names on menu v7's palette */
#define UI_TEXT C_L1
#define UI_TEXT2 C_L2
#define UI_TEXT3 C_L3
#define UI_HOLO C_STAR
#define UI_GREEN C_GREEN
#define UI_RED C_RED
#define UI_ORANGE C_ORANGE
#define UI_DEG U_DEG
#define UI_DOT U_DOT
#define UI_ELLIPSIS U_ELL
#define UI_TIMES U_TIMES
#define UI_PLUSMINUS U_PM
#define UI_LEFT U_LEFT
#define UI_RIGHT U_RIGHT
#define UI_CHECK U_CHECK

/* ---- navigation state ---------------------------------------------------------------------- */

struct eng {
    int home;               /* unwind to the tab's root */
    int tab_req;            /* switch to this tab once unwound (-1 none) */
    int redraw;             /* chrome changed (armed): redraw the whole screen */
    int depth;
    const char *titles[8];  /* the navigation stack, for the back label */
};
extern struct eng E;
#define NTABS 8
extern const char *const tab_label[NTABS];
extern int cur_tab;
const char *tab_title_of(int t);

/* ---- screens (shell.c) ----------------------------------------------------------------------- */

void kv_hdr(const char *t);
void kv_add(const char *k, const char *v);
void kv_addc(const char *k, const char *v, uint32_t c);
void kv_mono(const char *t);
void kv_monoc(const char *t, uint32_t c);
void kv_big(const char *k, const char *v);
void kv_bigc(const char *k, const char *v, uint32_t c);
void kv_note(const char *t);
void kv_gap(int h);
void kv_hex(const char *k, uint64_t v, unsigned digits);
void kv_dec(const char *k, uint64_t v, const char *unit);
void kv_custom(int h, pi_draw_fn draw, uintptr_t arg, uint32_t hash);
void row_add(const char *t, const char *s, uint32_t f);
void row_value(const char *t, const char *v, uint32_t f);
const char *row_title(int i);
void tv_reset(void);
void tv_line(const char *s);
int dialog(const char *title, const char *l1, const char *l2, const char *ok);
int dialog_danger(const char *title, const char *l1, const char *l2, const char *ok);
void info_screen(const char *title, void (*fill)(void), unsigned refresh_ms);
void list_screen(const char *title, void (*build)(void), int (*act)(int));
void text_screen(const char *title);
/* Draw a page once (a test's progress) without waiting. */
void show_now(const char *title, void (*fill)(void));
/* Rebuild p with fill (only what changed is redrawn): for screens with their own loop. */
void eng_build_page(struct page *p, void (*fill)(void));
/* Full redraw of the chrome around a custom screen: status bar, header, tab bar. */
void chrome(const char *title, const char *note, uint32_t note_c);
const char *back_label(void);
/* Common event handling for custom screens: 1 = leave the screen (back, home, tab). */
int nav_event(const struct ui_ev *e);
void status_refresh(int force);
void screen_push(const char *title);
void screen_pop(void);

/* ---- pages ----------------------------------------------------------------------------------- */

void tab_build(int t);
void tab_open(int t, int row);
void dash_fill(void);           /* dash.c: the Dashboard tab root */
void dash_sample(void);
void dash_battery_page(void);
void tests_history(void);       /* tests.c */
void test_record(const char *name, int pass, const char *detail);
/* A tier 3 / 4 action (confirmation or outcome) into the history and the pmsg ring ("SBENG1 W"). */
void test_record_action(int tier, const char *what, const char *detail);
void tests_load_history(void);
void logs_engineer(void);       /* pages_sb.c */
void logs_loader(void);
void sb_pages_catalog(void);
void sb_pages_settings(void);
void sb_pages_gpt(void);
void sb_pages_fence(void);
void sb_pages_coming(const char *what);
void sb_pages_about(void);

/* em.c pages (pages_em.c) */
void fill_device_info(void), fill_soc(void), fill_memory(void), fill_reset(void), fill_cpu(void);
void fill_thermal(void), fill_clocks(void), fill_gpu(void), fill_panel(void), fill_rtc(void);
void fill_timer(void), measure_timer(void), fill_battery(void), fill_pmic(void), fill_ufs(void);
void fill_partitions(void), fill_ufs_dev(void), fill_sd(void), fill_android(void), fill_ios(void);
void fill_linux(void), fill_windows(void), fill_store(void), fill_regs(void);
void test_display(void), test_touch(void), test_keys(void), run_cores_dialog(void), run_ram_dialog(void);
void run_bw(void), run_speed(void), sector_screen(void), dump_pmic(void);
void touch_rails(void), sd_page(void), sdboot_page(void), start_page(void), ios_opts_page(void);
void bright_page(void), reboot_page(void), show_pmsg(void), show_records(void), show_cmdline(void);
/* Clear a pending boot-next, reboot normally: S-Boot, the loader, its menu. Asks first. */
void eng_exit_to_menu(void);

/* ---- policy (policy.c, pure, unit-tested) ------------------------------------------------------
 * Tiers of confirmation (libsb SB_TIER*): 0 read, 1 warning + held slide, 2 + the target typed,
 * 3 + a second confirmation (+ a verified backup for storage), 4 extreme (red full-screen warning,
 * "<target> I ACCEPT" typed, Power held 5 s, storage: a backup unless explicitly waived). TIER_NO:
 * only what cannot be expressed (docs/ENGINEER-OS.md 8.1). */
enum { TIER0 = SB_TIER0, TIER1 = SB_TIER1, TIER2 = SB_TIER2, TIER3 = SB_TIER3, TIER4 = SB_TIER4, TIER_NO = SB_TIER_NO };
#define GPU_NLEVELS 6
extern const unsigned gpu_levels_khz[GPU_NLEVELS];
int dvfs_tier(unsigned domain, unsigned khz, const char **why);
int poke_tier(uint64_t addr, const char **why);         /* a 32-bit register / memory write */
const char *poke_undo(uint64_t addr);
int range_tier(uint64_t addr, uint64_t len, const char **why);   /* memory fill: the worst page */
int read_tier(uint64_t addr, const char **why);         /* 0 free, 1 asks, TIER_NO */
int peek_allowed(uint64_t addr);                        /* read_tier == 0 */
int pmic_tier(unsigned type, unsigned reg, unsigned val, const char **why);
int max_tier(unsigned addr7, unsigned reg, unsigned val, const char **why);
const char *tier_name(int t);

/* ---- armed mode and confirmations (armed.c) ------------------------------------------------------
 * Armed mode is the session switch for the free-form tools (register poke, memory fill, sector
 * edit, PMIC / MAX77705 writes): hold Power 2 s, then slide; red border; ends after 5 minutes
 * without input, when its page is left, and on reboot. Every write, named or free-form, goes
 * through lab_confirm: the warning screen (what happens, how to undo), then the confirmations its
 * tier needs. */
int armed_tier(void);                   /* 1 while armed, 0 not */
uint64_t armed_left_ms(void);
int armed_arm(void);
void armed_disarm(const char *why);
void armed_tick(void);
struct action {
    const char *title;                  /* "Write EFS", "Register write" */
    const char *target;                 /* what is typed for tier 2+: "EFS", "14220180" */
    const char *happens[4];             /* what happens, one line each (0 ends) */
    const char *undo;                   /* how to undo it */
    const char *old_v, *new_v;
    const char *backup;                 /* where the backup goes ("" none) */
    int tier;
    unsigned space, lu, kind;           /* the authorisation: SB_MED_UFS / SD / SB_SP_* */
    uint64_t first, count;
};
/* Warning screen and the confirmations; on success *auth is filled and complete except, for tier
 * 3 storage, the backup step (lab_backup adds it). 1 confirmed, 0 cancelled or refused. Tier 4:
 * the red full-screen warning, the typed phrase, the slide, Power held 5 s; tier 4 storage also
 * asks "back up first / no backup" on a second red screen. Every tier 3 / 4 confirmation is
 * written to the pmsg ring before anything happens ("SBENG1 W", Test history). */
int lab_confirm(const struct action *a, struct sb_auth *auth);
/* 1 if the confirmed storage action still needs its verified backup (tier 3, or tier 4 without
 * the waiver). */
int lab_backup_needed(const struct sb_auth *auth);
/* After a tier 3 / 4 action: the outcome next to its confirmation in the history and pmsg. */
void lab_major_result(const char *title, const char *target, const char *outcome);
/* Back up memory [addr, addr + len) into the OS store as "bak-<label>-<n>", verified. 0 or < 0. */
int lab_backup_mem(uint64_t addr, uint64_t len, const char *label, char *where);
/* A tier 4 access to an address Engineer OS does not map: mapped for the access, the fault
 * caught. 1 the access completed (*v read), 0 it faulted (*esr, the SError state in *serror). */
int lab_attempt32(uint64_t addr, int write, uint32_t *v, uint64_t *esr, int *serror);
/* Back up blocks [first, first + count) of medium / lu into the OS store (UFS, else the card) as
 * "bak-<label>-<n>", verified (read back, CRC, compared with the source again). On success records
 * SB_STEP_BACKUP in auth (if given) and writes where it went into where (64 bytes). 0 or < 0. */
int lab_backup(unsigned medium, unsigned lu, uint64_t first, uint64_t count, const char *label,
               struct sb_auth *auth, char *where);
/* Copy a store entry back to blocks [first, ...) of medium / lu with the gate open on auth, then
 * read back and compare the CRC. 0 or < 0. */
int lab_restore(const struct store_ent *e, unsigned medium, unsigned lu, uint64_t first, const struct sb_auth *auth);
/* Run a confirmed register write / PMIC / MAX77705 write: gate open, write, read back, gate closed.
 * The old value is kept for lab_undo. 1 written. */
int lab_poke32(uint64_t addr, uint32_t val, const struct sb_auth *auth);
int lab_pmic(unsigned type, unsigned reg, unsigned val, const struct sb_auth *auth);
int lab_max(unsigned addr7, unsigned reg, unsigned val, const struct sb_auth *auth);
int lab_undo_available(char *what, unsigned cap);
void lab_undo(void);
/* Power held 0.6 s (3 s if it was already down at the reset) stops a long operation or a test. */
void lab_stop_reset(void);
int lab_stop_check(void);
/* The type-the-name keypad (tier 2+): 1 typed correctly, 0 cancelled. */
int lab_type_name(const char *title, const char *name);
/* Battery guard for writes: 1 ok (>= 20 %, or a charger, or the gauge does not answer and the
 * user accepted that), 0 refused. */
int lab_battery_ok(const char *what);
void armed_page(void);
void audit_page(void);
void audit_line(const char *s);

/* ---- utilities (util.c) ----------------------------------------------------------------------------- */

void util_peek(void);                   /* register peek / poke */
void util_memory(void);                 /* memory dump / fill */
void util_partitions(void);             /* partition read / hexdump / backup / restore (UFS, LUs, card) */
void util_store(void);                  /* OS store inspect / edit */
void util_settings(void);               /* settings editor */
void util_reboot(void);                 /* reboot to any target */
void util_pmic(void);                   /* PMIC and MAX77705 register writes */
void util_sector(unsigned medium, unsigned lu, uint64_t lba);   /* hexdump + edit */
/* Keys-friendly hex entry: Volume changes the digit, Power moves on, hold Power cancels; touch:
 * tap the upper / lower half of a digit. 1 entered. */
int ask_hex(const char *title, const char *hint, uint64_t *v, unsigned digits);
/* Pick one of n labels (a list): index or -1. */
int ask_pick(const char *title, const char *const *labels, int n, int cur);

/* ---- Lab tests (lab.c, kern.c) ------------------------------------------------------------------ */

void lab_cpu_stress(void);
void lab_mem_stress(void);
void lab_display_stress(void);
void lab_touch_grid(void);
void lab_storage_bench(void);
void lab_thermal(void);
void lab_battery(void);                 /* the battery tests' list */
void lab_health(void);                  /* full health check */
void lab_save_report(void);             /* results to the OS store */
void lab_sensors(void);                 /* every thermal sensor, live */
void lab_gpu(void);                     /* gpu.c: the Mali-G72 tests */
/* The temperature-limit option shared by the load tests: 70..95 C or "no limit" (index
 * LIMIT_NONE, chosen only through a warning naming the hardware trip as decoded live). */
#define LIMIT_N 7
#define LIMIT_NONE 6
int lab_limit_c(int idx);               /* C, or 1000 for no limit */
const char *lab_limit_label(int idx);
int lab_limit_next(int idx);            /* the next option; asks before LIMIT_NONE */
/* gpu.c results for the report, and lab.c's live page and charts (0..4) */
void result_line(const char *name, int pass, const char *detail);
void lab_live_begin(const char *title);
int lab_live_poll(void (*fill)(void), unsigned every_ms);   /* 1: the user asked to stop */
void lab_live_end(void);
void lab_chart_reset(unsigned i, const char *title, uint32_t col, unsigned total_samples);
void lab_chart_push(unsigned i, int32_t v);
void lab_chart_kv(unsigned i, int h);
/* Results of this session as text lines (report, store export). */
unsigned lab_report_lines(char (*out)[96], unsigned max);
/* kern.c: the stress kernels (ARMv8.0, run on every core) */
#define KERN_INT 1U
#define KERN_FP 2U
#define KERN_NEON 4U
#define KERN_MEM 8U
struct kern_cfg {
    unsigned mask;
    uint64_t gold[4];                   /* reference results (the boot core computes them) */
    uint64_t *buf[SB_NCORES];           /* KERN_MEM: per-core buffer */
    uint64_t words;                     /* per buffer */
    volatile uint32_t inject;           /* unit test / self test: corrupt one result */
};
void kern_gold(struct kern_cfg *c);
/* One batch of every enabled kernel; returns the mismatches (0 = all results correct). */
unsigned kern_batch(struct kern_cfg *c, unsigned core, uint64_t *iters);
void kern_worker(unsigned w, void *arg);
