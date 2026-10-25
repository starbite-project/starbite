/* SPDX-License-Identifier: GPL-2.0-only
 * Project Starbite Engineer mode: shared primitives, used by menu.c (where
 * they are defined) and the low-level hardware pages in eng_hw.c. The Image
 * must have no absolute relocations, so there are no pointer tables here and
 * eng_hw.c reaches menu.c's helpers through plain (PC-relative) calls.
 */
#pragma once
#include <stdint.h>
#include "fbtext.h"
#include "menu.h"

/* Keys: S9 ALIVE GPIOs, active low. Volume moves, Power/Bixby selects. */
#define K_UP 1U
#define K_DOWN 2U
#define K_BIXBY 4U
#define K_POWER 8U
#define K_SELECT (K_BIXBY | K_POWER)

#define C_WHITE 0xffffffffU
#define C_BLACK 0xff000000U
#define C_BLUE 0xff2a5bf0U
#define C_MUTED 0xffb1bad3U
#define C_GOLD 0xffffd43bU
#define C_RED 0xffff4d4dU
#define C_GREEN 0xff3ddc84U

/* Engineer page layout (text rows). */
#define ROW_TITLE 3U
#define ROW_BODY 5U
#define ROW_HINT 43U
#define HINT_MENU "VOL: MOVE   POWER: SELECT"
#define HINT_BACK "PRESS POWER TO GO BACK"

/* ACPM / S2MPS18 PMIC (as bsd/dev/starlte_touch.c). */
#define ACPM_MBOX 0x14100000UL
#define ACPM_SRAM 0x02039000UL
#define ACPM_SRAM_SIZE 0x30000U
#define ACPM_INITDATA 0x7f00U
#define ACPM_PMIC_CH 2U

/* A one-line text builder bounded by the panel width. */
struct sb { char b[STAR_FB_COLS + 2]; unsigned n; };

/* ---- primitives defined in menu.c ------------------------------------ */
uint64_t ticks(void);
uint64_t freq(void);
void wait_us(uint64_t us);
void wait_ms(unsigned ms);
unsigned keys(void);
unsigned key_wait(void);

void sb_init(struct sb *s);
void sb_s(struct sb *s, const char *t);
void sb_c(struct sb *s, char c);
void sb_hex(struct sb *s, uint64_t v, unsigned digits);
void sb_dec(struct sb *s, uint64_t v);

void row_hex(unsigned row, const char *label, uint64_t v, unsigned digits);
void row_dec(unsigned row, const char *label, uint64_t v, const char *unit);
void row_ldo(unsigned row, const char *name, int ok, uint8_t v, unsigned base_mv);

void item(unsigned i, const char *s, uint32_t fg);
void label(unsigned i, const char *name, const char *value, uint32_t fg);
unsigned choose(unsigned row0, unsigned n, unsigned sel);
void page(const char *title, const char *hint);
void wait_select(void);
int confirm(const char *what);

uint32_t ard(uint32_t off);
void awr(uint32_t off, uint32_t v);
uint32_t acpm_find(uint32_t id);
int acpm_send(uint32_t c, uint32_t id, uint32_t cmd[4]);
int pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
int pmic_write(uint32_t type, uint32_t reg, uint8_t val);

unsigned slen(const char *s);
const char *arg_find(const char *c, const char *key, unsigned *len);
void arg_del(char *c, const char *key);
void arg_set(char *c, unsigned len, const char *key, const char *val);
unsigned arg_num(const char *c, const char *key, unsigned dflt);
void opt_load(const char *c);

/* ---- the low-level hardware mode (eng_hw.c) -------------------------- */
void eng_hardware(const struct star_info *in, char *cmdline, unsigned len);
