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

#define ENG_VERSION "1.1"
#define ENG_BUILD "EN1-EN3 (v1.1, dry-run; keys-only navigation)"

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
void run_bw(void), run_speed(void), sector_screen(void), peek_screen(void), dump_pmic(void);
void touch_rails(void), sd_page(void), sdboot_page(void), start_page(void), ios_opts_page(void);
void bright_page(void), reboot_page(void), show_pmsg(void), show_records(void), show_cmdline(void);
/* Clear a pending boot-next, reboot normally: S-Boot, the loader, its menu. Asks first. */
void eng_exit_to_menu(void);

/* ---- armed mode (armed.c) -------------------------------------------------------------------- */

enum { TIER0, TIER1, TIER2, TIER_NEVER };
int armed_tier(void);
uint64_t armed_left_ms(void);
/* Arm (hold Power 2 s, then slide; battery >= 30 % for writes). 1 armed. */
int armed_arm(void);
void armed_disarm(const char *why);
/* Second unlock for tier 2: type the partition name on the keypad. */
int armed_tier2(const char *part);
void armed_tick(void);
/* A would-be change: shows the change card, logs the dry run, refuses. Returns SB_W_* or 0. */
struct change {
    const char *what;       /* "Register write", "PMIC rail", "Settings record", "Partition" */
    const char *target;     /* "0x14050000", "LDO35", "UFS block 4194306" */
    const char *old_v;      /* old value or hash */
    const char *new_v;
    const char *backup;     /* where the backup would go */
    int tier;               /* needed tier */
};
int armed_change(const struct change *c);
/* Register poke policy: TIER_NEVER for the compiled-in exclusions, TIER1 for allowlisted
 * blocks, else TIER_NEVER. */
int poke_tier(uint64_t addr, const char **why);
int peek_allowed(uint64_t addr);
void armed_page(void);
void audit_page(void);
void audit_line(const char *s);
