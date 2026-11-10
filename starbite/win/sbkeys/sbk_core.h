/* SPDX-License-Identifier: GPL-2.0-only
 * sbkeys' portable core: the Galaxy S9's four keys from the always-on GPIO DAT registers, and what a
 * press pattern means (docs/WIN11-GPU.md 6.40). Host-tested (starbite/win/tests/keys_test.c).
 *
 * Registers (shim/starlte/menu.c, the loader reads the same): the ALIVE GPIO page 0x14050000, bank
 * GPA0 at 0x20 (CON +0, DAT +4), GPA2 at 0x60. Keys are active low (pressed = 0):
 *   GPA0 DAT bit 3 Volume Up, bit 4 Volume Down, bit 6 Bixby; GPA2 DAT bit 4 Power.
 * sbkeys only ever reads (CON and DAT); CON/PUD belong to the release's touch driver, which owns the page.
 *
 * Patterns:
 *   - Bixby alone held HoldMs (default 3000)          -> SBK_RESTART (a clean restart)
 *   - Volume Up + Power together, all released within  -> SBK_SAS (only with Sas on: sblinkd sends
 *     SasMaxMs (default 1500), no other key involved       Ctrl+Alt+Del)
 *   - Volume Down + Power: never used (held ~7 s it is the hardware force restart)
 * Nothing counts until all four keys have been seen released for ArmMs (default 2000) in a row: a pin
 * that reads "pressed" from the start (not an input, wrong pull) can never restart Windows. A gap
 * between two polls longer than 500 ms (the system slept, the thread starved) drops any hold in progress.
 */
#ifndef SBK_CORE_H
#define SBK_CORE_H

typedef unsigned int sbk_u32;

#define SBK_GPIO_PA 0x14050000ULL
#define SBK_GPA0_CON 0x20U
#define SBK_GPA0_DAT 0x24U
#define SBK_GPA2_CON 0x60U
#define SBK_GPA2_DAT 0x64U

#define SBK_UP 1U
#define SBK_DOWN 2U
#define SBK_BIXBY 4U
#define SBK_POWER 8U

enum sbk_action { SBK_NONE = 0, SBK_RESTART = 1, SBK_SAS = 2 };

struct sbk_cfg {
    sbk_u32 hold_ms;    /* Bixby alone for this long: restart (1000..10000) */
    sbk_u32 arm_ms;     /* all keys released this long before anything counts */
    sbk_u32 sas;        /* 1: Volume Up + Power brief press -> SBK_SAS */
    sbk_u32 sas_max_ms; /* the whole Volume Up + Power press, first key down to last key up */
};

struct sbk_state {
    sbk_u32 armed;      /* all keys seen released for arm_ms */
    sbk_u32 up_ms;      /* released time so far (until armed) */
    sbk_u32 bixby_ms;   /* Bixby-alone hold so far */
    sbk_u32 fired;      /* the restart fired for this hold (cleared on release) */
    sbk_u32 sess_ms;    /* the current press session: from the first key down */
    sbk_u32 sess_union; /* keys seen in the session */
    sbk_u32 sess_both;  /* Volume Up and Power were down at the same time */
    sbk_u32 prev;       /* keys at the last step */
};

/* Keys (SBK_* bits, 1 = pressed) from the two DAT words. */
sbk_u32 sbk_keys(sbk_u32 gpa0_dat, sbk_u32 gpa2_dat);
/* 1 when every key's pin is an input (CON nibble 0x0) or an external interrupt (0xF). */
int sbk_pins_ok(sbk_u32 gpa0_con, sbk_u32 gpa2_con);
/* Defaults and limits applied to a configuration read from the registry. */
void sbk_cfg_fix(struct sbk_cfg *c);
void sbk_init(struct sbk_state *s);
/* One poll: keys now, dt_ms since the previous poll. */
enum sbk_action sbk_step(const struct sbk_cfg *c, struct sbk_state *s, sbk_u32 keys, sbk_u32 dt_ms);

#endif
