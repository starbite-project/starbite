/* SPDX-License-Identifier: APSL-2.0 */
/* Galaxy S9 keys: debouncing and the key table (see sb_keys.h). */
#include "sb_keys.h"

/* ALIVE pin controller banks (CON offsets): GPA0 0x20, GPA2 0x60 (a5xelte_keys.c STARLTE). */
const struct sb_key_def sb_keys_starlte[] = {
	{ 0x60u, 4, SB_HID_PAGE_CONSUMER, SB_HID_CSMR_POWER,    'p', "Power" },
	{ 0x20u, 3, SB_HID_PAGE_CONSUMER, SB_HID_CSMR_VOL_UP,   'u', "Volume Up" },
	{ 0x20u, 4, SB_HID_PAGE_CONSUMER, SB_HID_CSMR_VOL_DOWN, 'd', "Volume Down" },
	{ 0x20u, 6, SB_HID_PAGE_CONSUMER, SB_HID_CSMR_MENU,     'h', "Bixby (Home)" },
};
const unsigned sb_keys_starlte_count = sizeof(sb_keys_starlte) / sizeof(sb_keys_starlte[0]);

bool
sb_keys_pin_usable(uint32_t con, uint8_t pin)
{
	uint32_t f = (con >> (pin * 4u)) & 0xfu;
	return f == 0x0u || f == 0xfu;
}

uint32_t
sb_keys_pressed(const struct sb_keys *k, const uint32_t *bank_offsets, const uint32_t *dat, unsigned nbanks)
{
	uint32_t held = 0;
	for (unsigned i = 0; i < k->n; i++) {
		for (unsigned b = 0; b < nbanks; b++) {
			if (bank_offsets[b] == k->defs[i].bank) {
				if (!((dat[b] >> k->defs[i].pin) & 1u)) {
					held |= 1u << i;
				}
				break;
			}
		}
	}
	return held;
}

void
sb_keys_init(struct sb_keys *k, const struct sb_key_def *defs, unsigned n, uint32_t usable, uint32_t pressed)
{
	k->defs = defs;
	k->n = n > SB_KEYS_MAX ? SB_KEYS_MAX : n;
	k->usable = usable;
	k->stable = k->previous = pressed & usable;
	k->held_at_start = k->stable;
	k->repeated = 0;
	k->changes = 0;
	k->ignored_at_start = 0;
}

int
sb_keys_sample(struct sb_keys *k, uint32_t pressed, struct sb_key_change *out, int max)
{
	int n = 0;
	pressed &= k->usable;
	if (pressed == k->previous) {
		if (k->repeated < SB_KEYS_STABLE_SAMPLES - 1) {
			k->repeated++;
		}
	} else {
		k->previous = pressed;
		k->repeated = 0;
	}
	if (k->repeated != SB_KEYS_STABLE_SAMPLES - 1 || k->stable == pressed) {
		return 0;
	}
	uint32_t diff = k->stable ^ pressed;
	for (unsigned i = 0; i < k->n; i++) {
		uint32_t bit = 1u << i;
		if (!(diff & bit)) {
			continue;
		}
		bool down = (pressed & bit) != 0;
		if (k->held_at_start & bit) {
			/* held since before we started: say nothing, arm it once released */
			if (!down) {
				k->held_at_start &= ~bit;
			}
			k->ignored_at_start++;
			continue;
		}
		if (n < max) {
			out[n].key = i;
			out[n].down = down;
			n++;
		}
		k->changes++;
	}
	k->stable = pressed;
	return n;
}

unsigned
sb_keys_banks(const struct sb_key_def *defs, unsigned n, uint32_t *offsets, unsigned max)
{
	unsigned count = 0;
	for (unsigned i = 0; i < n; i++) {
		bool seen = false;
		for (unsigned b = 0; b < count; b++) {
			seen |= offsets[b] == defs[i].bank;
		}
		if (!seen && count < max) {
			offsets[count++] = defs[i].bank;
		}
	}
	return count;
}

unsigned
sb_keyscript_parse(struct sb_keyscript *s, const struct sb_key_def *defs, unsigned n, const char *p)
{
	s->n = s->next = 0;
	s->pressed = false;
	s->bad = -1;
	while (p && *p && s->n < SB_KEYSCRIPT_MAX) {
		unsigned v = 0;
		bool digits = false;
		while (*p >= '0' && *p <= '9' && v < 100000u) {
			v = v * 10 + (unsigned)(*p++ - '0');
			digits = true;
		}
		unsigned key = n;
		for (unsigned i = 0; i < n && *p; i++) {
			if (defs[i].letter == *p) {
				key = i;
			}
		}
		if (!digits || key == n) {
			s->bad = (int)s->n;
			break;
		}
		p++;
		s->steps[s->n].at_ms = (uint64_t)v * 1000u;
		s->steps[s->n].key = key;
		s->n++;
		while (*p && *p != ',') {
			p++;
		}
		if (*p == ',') {
			p++;
		}
	}
	return s->n;
}

bool
sb_keyscript_next(struct sb_keyscript *s, uint64_t now_ms, unsigned *key, bool *down, uint64_t *next_ms)
{
	*next_ms = (uint64_t)-1;
	if (s->next >= s->n) {
		return false;
	}
	uint64_t at = s->steps[s->next].at_ms + (s->pressed ? SB_KEYSCRIPT_HOLD_MS : 0);
	if (now_ms < at) {
		*next_ms = at;
		return false;
	}
	*key = s->steps[s->next].key;
	*down = !s->pressed;
	if (s->pressed) {
		s->pressed = false;
		s->next++;
	} else {
		s->pressed = true;
	}
	if (s->next < s->n) {
		*next_ms = s->steps[s->next].at_ms + (s->pressed ? SB_KEYSCRIPT_HOLD_MS : 0);
	}
	return true;
}
