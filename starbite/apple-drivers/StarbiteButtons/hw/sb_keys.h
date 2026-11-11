/* SPDX-License-Identifier: APSL-2.0 */
/*
 * The Galaxy S9's keys on ALIVE GPIO: debouncing and the key table. Plain C, no IOKit; host-tested.
 *
 * Each key's pin is pulled up on the board and grounded by the key: it reads 0 while held (active low).
 * Facts from today's a5xelte_keys.c (STARLTE: power GPA2-4, volume up GPA0-3, volume down GPA0-4, Bixby GPA0-6,
 * sampled every 10 ms, a change taken after three equal samples), cross-checked with an earlier port's APSL
 * exynos_keys.c and the stock DTB's gpio_keys (../NOTES.md).
 *
 * Added here: a key already held when the driver starts (someone holding volume down from a Download-mode
 * combo, or power) sends nothing until it has been released once; a pin whose CON nibble is neither input (0)
 * nor wake-up interrupt (0xF) is not a key (another driver owns it).
 */
#ifndef SB_KEYS_H
#define SB_KEYS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_KEYS_MAX             8
#define SB_KEYS_BANKS           4
#define SB_KEYS_POLL_MS         10
#define SB_KEYS_STABLE_SAMPLES  3       /* a new state is taken when seen on this many polls in a row */

/* HID usages (Consumer page 0x0C). */
#define SB_HID_PAGE_CONSUMER    0x0cu
#define SB_HID_CSMR_POWER       0x30u
#define SB_HID_CSMR_MENU        0x40u   /* Home: what Bixby stands in for, as today */
#define SB_HID_CSMR_VOL_UP      0xe9u
#define SB_HID_CSMR_VOL_DOWN    0xeau

struct sb_key_def {
	uint32_t bank;                  /* GPIO bank CON offset in the pin controller (DAT = +4) */
	uint8_t pin;
	uint16_t usage_page, usage;
	char letter;                    /* for sbkeyscript: p, u, d, h */
	const char *name;
};

/* The S9 (starlte) table: power, volume up, volume down, Bixby. */
extern const struct sb_key_def sb_keys_starlte[];
extern const unsigned sb_keys_starlte_count;

struct sb_key_change {
	unsigned key;                   /* index into the table */
	bool down;
};

struct sb_keys {
	const struct sb_key_def *defs;
	unsigned n;
	uint32_t usable;                /* keys whose pin is configured as input / wake-up */
	uint32_t stable, previous;      /* bit per key: 1 = held */
	unsigned repeated;
	uint32_t held_at_start;         /* ignored until released */
	unsigned changes, ignored_at_start;
};

/* Whether a pin is an input (CON nibble 0) or a wake-up interrupt (0xF). */
bool sb_keys_pin_usable(uint32_t con, uint8_t pin);
/* Bit per key, 1 = held, from the banks' DAT values (dat[i] belongs to bank_offsets[i]). */
uint32_t sb_keys_pressed(const struct sb_keys *k, const uint32_t *bank_offsets, const uint32_t *dat, unsigned nbanks);
/* The first sample: keys held now are ignored until they are released. */
void sb_keys_init(struct sb_keys *k, const struct sb_key_def *defs, unsigned n, uint32_t usable, uint32_t pressed);
/* One poll. Returns the number of changes written to out (at most max). */
int  sb_keys_sample(struct sb_keys *k, uint32_t pressed, struct sb_key_change *out, int max);
/* The distinct banks of a table (for reading DAT once per bank). Returns how many. */
unsigned sb_keys_banks(const struct sb_key_def *defs, unsigned n, uint32_t *offsets, unsigned max);

/* sbkeyscript=<s><letter>,... : press a key for 80 ms at <s> seconds (p power, u up, d down, h Bixby/Home). */
#define SB_KEYSCRIPT_MAX        16
#define SB_KEYSCRIPT_HOLD_MS    80
struct sb_keyscript {
	struct { uint64_t at_ms; unsigned key; } steps[SB_KEYSCRIPT_MAX];
	unsigned n, next;
	bool pressed;
	int bad;
};
unsigned sb_keyscript_parse(struct sb_keyscript *s, const struct sb_key_def *defs, unsigned n, const char *text);
/* A press or release due at now_ms: true and *key / *down. *next_ms = when the next one is due, or UINT64_MAX. */
bool sb_keyscript_next(struct sb_keyscript *s, uint64_t now_ms, unsigned *key, bool *down, uint64_t *next_ms);

#ifdef __cplusplus
}
#endif
#endif /* SB_KEYS_H */
