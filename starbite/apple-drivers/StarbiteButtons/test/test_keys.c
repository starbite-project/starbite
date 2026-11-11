/* SPDX-License-Identifier: APSL-2.0 */
/* StarbiteButtons host tests: the key table against the pin registers, debouncing, keys held at start,
 * pin ownership, and the key script. */
#include <stdio.h>
#include <string.h>
#include "sb_keys.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
	if (_a != _b) { failures++; printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* The ALIVE block's DAT registers: every key pin high (released) unless held. */
static uint32_t gpa0 = 0xff, gpa2 = 0xff;
static const uint32_t banks[2] = { 0x20, 0x60 };

static void
hold(unsigned key, bool down)
{
	const struct sb_key_def *d = &sb_keys_starlte[key];
	uint32_t *dat = d->bank == 0x20 ? &gpa0 : &gpa2;
	if (down) {
		*dat &= ~(1u << d->pin);
	} else {
		*dat |= 1u << d->pin;
	}
}

static uint32_t
pressed(const struct sb_keys *k)
{
	uint32_t dat[2] = { gpa0, gpa2 };
	return sb_keys_pressed(k, banks, dat, 2);
}

static int
poll(struct sb_keys *k, struct sb_key_change *out)
{
	return sb_keys_sample(k, pressed(k), out, 8);
}

static void
test_table(void)
{
	CHECK_EQ(sb_keys_starlte_count, 4);
	CHECK(sb_keys_starlte[0].bank == 0x60 && sb_keys_starlte[0].pin == 4 && sb_keys_starlte[0].usage == 0x30);
	CHECK(sb_keys_starlte[1].bank == 0x20 && sb_keys_starlte[1].pin == 3 && sb_keys_starlte[1].usage == 0xe9);
	CHECK(sb_keys_starlte[2].bank == 0x20 && sb_keys_starlte[2].pin == 4 && sb_keys_starlte[2].usage == 0xea);
	CHECK(sb_keys_starlte[3].bank == 0x20 && sb_keys_starlte[3].pin == 6 && sb_keys_starlte[3].usage == 0x40);
	uint32_t off[4];
	CHECK_EQ(sb_keys_banks(sb_keys_starlte, sb_keys_starlte_count, off, 4), 2);
	CHECK(off[0] == 0x60 && off[1] == 0x20);
	/* CON nibbles: 0 input, 0xF wake-up interrupt are keys; 1 output or 2 a function are not */
	CHECK(sb_keys_pin_usable(0x00000000, 3));
	CHECK(sb_keys_pin_usable(0x0000f000, 3));
	CHECK(!sb_keys_pin_usable(0x00001000, 3));
	CHECK(!sb_keys_pin_usable(0x00002000, 3));
	CHECK(sb_keys_pin_usable(0x00002000, 4));
}

static void
test_debounce(void)
{
	struct sb_keys k;
	struct sb_key_change ch[8];
	gpa0 = gpa2 = 0xff;
	sb_keys_init(&k, sb_keys_starlte, sb_keys_starlte_count, 0xf, 0);
	CHECK_EQ(poll(&k, ch), 0);

	/* a 10 ms bounce is ignored */
	hold(1, true);
	CHECK_EQ(poll(&k, ch), 0);
	hold(1, false);
	CHECK_EQ(poll(&k, ch), 0);
	CHECK_EQ(poll(&k, ch), 0);
	CHECK_EQ(poll(&k, ch), 0);

	/* held: taken on the third equal sample (20 ms) */
	hold(1, true);
	CHECK_EQ(poll(&k, ch), 0);
	CHECK_EQ(poll(&k, ch), 0);
	CHECK_EQ(poll(&k, ch), 1);
	CHECK(ch[0].key == 1 && ch[0].down);
	CHECK_EQ(poll(&k, ch), 0);              /* no repeat while held */
	CHECK_EQ(poll(&k, ch), 0);
	hold(1, false);
	CHECK_EQ(poll(&k, ch), 0);
	CHECK_EQ(poll(&k, ch), 0);
	CHECK_EQ(poll(&k, ch), 1);
	CHECK(ch[0].key == 1 && !ch[0].down);

	/* two keys together (a chord: power + volume down) in one sample */
	hold(0, true);
	hold(2, true);
	poll(&k, ch); poll(&k, ch);
	int n = poll(&k, ch);
	CHECK_EQ(n, 2);
	CHECK(ch[0].key == 0 && ch[0].down && ch[1].key == 2 && ch[1].down);
	hold(0, false);
	hold(2, false);
	poll(&k, ch); poll(&k, ch);
	CHECK_EQ(poll(&k, ch), 2);
	CHECK_EQ(k.changes, 6);

	/* Bixby and power on different banks */
	hold(3, true);
	poll(&k, ch); poll(&k, ch);
	CHECK_EQ(poll(&k, ch), 1);
	CHECK(ch[0].key == 3 && ch[0].down);
	hold(3, false);
	poll(&k, ch); poll(&k, ch); poll(&k, ch);
}

static void
test_held_at_start_and_unusable(void)
{
	struct sb_keys k;
	struct sb_key_change ch[8];
	gpa0 = gpa2 = 0xff;
	hold(2, true);                          /* volume down held through boot */
	sb_keys_init(&k, sb_keys_starlte, sb_keys_starlte_count, 0xf, pressed(&k));
	k.defs = sb_keys_starlte;
	for (int i = 0; i < 5; i++) {
		CHECK_EQ(poll(&k, ch), 0);
	}
	hold(2, false);                         /* its release says nothing */
	for (int i = 0; i < 5; i++) {
		CHECK_EQ(poll(&k, ch), 0);
	}
	CHECK_EQ(k.ignored_at_start, 1);
	hold(2, true);                          /* the next press counts */
	poll(&k, ch); poll(&k, ch);
	CHECK_EQ(poll(&k, ch), 1);
	CHECK(ch[0].key == 2 && ch[0].down);
	hold(2, false);
	poll(&k, ch); poll(&k, ch); poll(&k, ch);

	/* a key whose pin another driver owns never reports */
	sb_keys_init(&k, sb_keys_starlte, sb_keys_starlte_count, 0xf & ~(1u << 1), 0);
	hold(1, true);
	for (int i = 0; i < 6; i++) {
		CHECK_EQ(poll(&k, ch), 0);
	}
	hold(1, false);
}

static void
test_keyscript(void)
{
	struct sb_keyscript s;
	unsigned key; bool down; uint64_t next;
	CHECK_EQ(sb_keyscript_parse(&s, sb_keys_starlte, sb_keys_starlte_count, "20h,25u,30d,31p"), 4);
	CHECK_EQ(s.bad, -1);
	CHECK(!sb_keyscript_next(&s, 19999, &key, &down, &next));
	CHECK_EQ(next, 20000);
	CHECK(sb_keyscript_next(&s, 20000, &key, &down, &next));
	CHECK(key == 3 && down);
	CHECK_EQ(next, 20000 + SB_KEYSCRIPT_HOLD_MS);
	CHECK(sb_keyscript_next(&s, 20080, &key, &down, &next));
	CHECK(key == 3 && !down);
	CHECK_EQ(next, 25000);
	int events = 0;
	for (uint64_t t = 25000; t < 40000; t += 10) {
		while (sb_keyscript_next(&s, t, &key, &down, &next)) {
			events++;
		}
	}
	CHECK_EQ(events, 6);
	CHECK_EQ(next, (uint64_t)-1);
	CHECK_EQ(sb_keyscript_parse(&s, sb_keys_starlte, sb_keys_starlte_count, "5h,6x,7u"), 1);
	CHECK_EQ(s.bad, 1);
	CHECK_EQ(sb_keyscript_parse(&s, sb_keys_starlte, sb_keys_starlte_count, "h"), 0);
}

int
main(void)
{
	struct { const char *name; void (*fn)(void); } tests[] = {
		{ "key table and pin ownership", test_table },
		{ "debounce", test_debounce },
		{ "held at start, pins owned elsewhere", test_held_at_start_and_unusable },
		{ "key script", test_keyscript },
	};
	for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int before = failures;
		tests[i].fn();
		printf("%s: %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
	}
	printf("StarbiteButtons host tests: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
