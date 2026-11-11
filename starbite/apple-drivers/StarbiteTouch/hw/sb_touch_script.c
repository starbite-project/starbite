/* SPDX-License-Identifier: APSL-2.0 */
/* Scripted input and the packet self-test (see sb_touch_script.h). */
#include "sb_touch_script.h"
#include "sb_s6sy761.h"

#define NO_TIME ((uint64_t)-1)

static void
zero(void *p, unsigned n)
{
	unsigned char *c = (unsigned char *)p;
	for (unsigned i = 0; i < n; i++) {
		c[i] = 0;
	}
}

static unsigned
num(const char **pp)
{
	unsigned v = 0;
	while (**pp >= '0' && **pp <= '9' && v < 100000u) {
		v = v * 10 + (unsigned)(**pp - '0');
		(*pp)++;
	}
	return v;
}

static void
colon(const char **pp)
{
	if (**pp == ':') {
		(*pp)++;
	}
}

unsigned
sb_script_parse(struct sb_script *s, const char *p)
{
	zero(s, sizeof(*s));
	s->bad = -1;
	while (p && *p && s->n < SB_SCRIPT_MAX) {
		struct sb_script_step *st = &s->steps[s->n];
		if (*p < '0' || *p > '9') {
			s->bad = (int)s->n;
			break;
		}
		st->at_ms = (uint64_t)num(&p) * 1000u;
		char kind = *p ? *p++ : 0;
		if (kind != 't' && kind != 's') {
			s->bad = (int)s->n;
			break;
		}
		st->kind = kind;
		st->x0 = (int)num(&p);
		colon(&p);
		st->y0 = (int)num(&p);
		st->x1 = st->x0;
		st->y1 = st->y0;
		if (kind == 's') {
			colon(&p);
			st->x1 = (int)num(&p);
			colon(&p);
			st->y1 = (int)num(&p);
		}
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

/* Samples of a step: a tap is down + up 80 ms later; a swipe 51 eased samples 6 ms apart, then up. */
static unsigned
samples(const struct sb_script_step *st)
{
	return st->kind == 't' ? 2u : SB_SCRIPT_SWIPE_SAMPLES + 2u;
}

static uint64_t
sample_at(const struct sb_script_step *st, unsigned k)
{
	if (st->kind == 't') {
		return st->at_ms + (k ? SB_SCRIPT_TAP_MS : 0);
	}
	return st->at_ms + (uint64_t)k * SB_SCRIPT_SAMPLE_MS;
}

bool
sb_script_next(struct sb_script *s, uint64_t now_ms, int *finger, bool *down, int *x, int *y, uint64_t *next_ms)
{
	*next_ms = NO_TIME;
	if (s->step >= s->n) {
		return false;
	}
	const struct sb_script_step *st = &s->steps[s->step];
	unsigned k = s->sample;
	uint64_t at = sample_at(st, k);
	if (now_ms < at) {
		*next_ms = at;
		return false;
	}
	unsigned last = samples(st) - 1;
	*finger = SB_SCRIPT_FINGER;
	*down = k < last;
	if (st->kind == 't' || k >= last) {
		*x = st->kind == 't' ? st->x0 : st->x1;
		*y = st->kind == 't' ? st->y0 : st->y1;
	} else {
		int t = (int)(k * 1000u / SB_SCRIPT_SWIPE_SAMPLES);        /* 0..1000 */
		int e = t * t / 1000 * (3000 - 2 * t) / 1000;              /* smoothstep, 0..1000 */
		*x = st->x0 + (st->x1 - st->x0) * e / 1000;
		*y = st->y0 + (st->y1 - st->y0) * e / 1000;
	}
	if (k >= last) {
		s->step++;
		s->sample = 0;
	} else {
		s->sample++;
	}
	if (s->step < s->n) {
		*next_ms = sample_at(&s->steps[s->step], s->sample);
	}
	return true;
}

/* ---- self-test ---- */

struct pkt {
	uint16_t at_ms;
	int8_t tid;                     /* -1: a status event, -2: an empty read */
	uint8_t state;
	uint16_t rx, ry;
};

#define P SB_S6_PRESS
#define M SB_S6_MOVE
#define R SB_S6_RELEASE
static const struct pkt selftest_packets[] = {
	/* a tap in the middle */
	{ 0, 0, P, 2048, 2048 }, { 80, 0, R, 2048, 2048 },
	/* touch, hold still 450 ms, then move: a drag */
	{ 1000, 0, P, 1000, 1000 },
	{ 1466, 0, M, 1100, 1000 }, { 1482, 0, M, 1200, 1000 }, { 1498, 0, M, 1300, 1000 }, { 1514, 0, M, 1400, 1000 },
	{ 1530, 0, M, 1500, 1000 }, { 1546, 0, M, 1600, 1000 }, { 1562, 0, M, 1700, 1000 }, { 1578, 0, M, 1800, 1000 },
	{ 1594, 0, M, 1900, 1000 }, { 1610, 0, M, 2000, 1000 }, { 1650, 0, R, 2000, 1000 },
	/* two fingers, the first moves down: scroll */
	{ 3000, 0, P, 2048, 1000 }, { 3020, 1, P, 2300, 1000 },
	{ 3036, 0, M, 2048, 1200 }, { 3052, 0, M, 2048, 1400 }, { 3068, 0, M, 2048, 1600 }, { 3084, 0, M, 2048, 1800 },
	{ 3100, 0, M, 2048, 2000 }, { 3116, 0, M, 2048, 2200 }, { 3132, 0, M, 2048, 2400 }, { 3148, 0, M, 2048, 2600 },
	{ 3164, 0, M, 2048, 2800 }, { 3180, 0, M, 2048, 3000 }, { 3200, 1, R, 2300, 1000 }, { 3210, 0, R, 2048, 3000 },
	/* a two-finger tap */
	{ 5000, 0, P, 1500, 2500 }, { 5010, 1, P, 1700, 2500 }, { 5100, 1, R, 1700, 2500 }, { 5110, 0, R, 1500, 2500 },
	/* a swipe up from the bottom edge */
	{ 6000, 2, P, 2048, 4090 }, { 6016, 2, M, 2048, 3790 }, { 6032, 2, M, 2048, 3490 }, { 6048, 2, M, 2048, 3190 },
	{ 6064, 2, M, 2048, 2890 }, { 6080, 2, M, 2048, 2590 }, { 6100, 2, R, 2048, 2590 },
	/* a status event and an empty read: ignored */
	{ 7000, -1, 0, 0, 0 }, { 7010, -2, 0, 0, 0 },
};
#undef P
#undef M
#undef R
#define SELFTEST_END_MS 7500u

struct st_ctx {
	bool pointer;
	struct sb_tg tg;
	struct sb_dig dig;
	struct sb_selftest_result *r;
	uint64_t now_ms, next_tick;
	uint32_t buttons;
	bool any_down;
};

static void
count_actions(struct st_ctx *c, const struct sb_tg_action *a, int n)
{
	struct sb_selftest_result *r = c->r;
	for (int i = 0; i < n; i++) {
		if (a[i].kind == SB_TG_SCROLL) {
			r->scrolls++;
			continue;
		}
		if (a[i].kind == SB_TG_ABS) {
			r->abs_events++;
		} else if (a[i].dx || a[i].dy) {
			r->rel_moves++;
		}
		uint32_t pressed = a[i].buttons & ~c->buttons;
		if (pressed & 1u) {
			r->presses1++;
		}
		if (pressed & 2u) {
			r->presses2++;
		}
		if (c->buttons & ~a[i].buttons) {
			r->releases++;
		}
		c->buttons = a[i].buttons;
	}
}

static void
st_contact(void *ctx, int finger, bool down, int x, int y)
{
	struct st_ctx *c = (struct st_ctx *)ctx;
	c->r->contacts++;
	if (c->pointer) {
		struct sb_tg_action a[4];
		bool kick = false;
		int n = sb_tg_contact(&c->tg, c->now_ms * 1000000ULL, finger, down, x, y, a, 4, &kick);
		count_actions(c, a, n);
		if (kick && c->next_tick == NO_TIME) {
			c->next_tick = c->now_ms + 1;
		}
	} else {
		struct sb_dig_frame f;
		if (sb_dig_contact(&c->dig, finger, down, x, y, &f)) {
			c->r->frames++;
			if (f.mask & SB_DIG_FROM_EDGE_TIP) {
				c->r->edge_frames++;
			}
			if (f.changed_touch) {
				c->r->touch_changes++;
			}
			c->any_down = f.any;
		}
	}
}

static int
no_xfer(void *ctx, uint8_t addr, bool rx, uint8_t *buf, unsigned len)
{
	(void)ctx; (void)addr; (void)rx; (void)buf; (void)len;
	return SB_I2C_ENODEV;
}

static void
no_delay(void *ctx, unsigned us)
{
	(void)ctx; (void)us;
}

void
sb_touch_selftest(bool pointer, const struct sb_tg_config *tg, const struct sb_dig_config *dig,
    unsigned panel_w, unsigned panel_h, struct sb_selftest_result *r)
{
	static struct st_ctx c;         /* large; not on a kernel stack */
	struct sb_s6 s6;
	struct sb_i2c none = { no_xfer, no_delay, 0 };

	zero(r, sizeof(*r));
	zero(&c, sizeof(c));
	c.pointer = pointer;
	c.r = r;
	c.next_tick = NO_TIME;
	if (pointer) {
		sb_tg_init(&c.tg, tg);
	} else {
		sb_dig_init(&c.dig, dig);
	}
	sb_s6_setup(&s6, &none, panel_w, panel_h, st_contact, &c);

	unsigned next = 0, count = sizeof(selftest_packets) / sizeof(selftest_packets[0]);
	for (c.now_ms = 0; c.now_ms <= SELFTEST_END_MS; c.now_ms++) {
		while (next < count && selftest_packets[next].at_ms == c.now_ms) {
			const struct pkt *p = &selftest_packets[next++];
			uint8_t e[SB_S6_EVENT_LEN] = { 0 };
			if (p->tid >= 0) {
				sb_s6_encode(e, p->tid, p->state, p->rx, p->ry);
			} else if (p->tid == -1) {
				e[0] = 0x01;            /* EID 1: status */
			}
			r->packets++;
			if (sb_s6_feed(&s6, e) != SB_S6_COORD) {
				r->ignored++;
			}
		}
		if (pointer && c.next_tick != NO_TIME && c.now_ms >= c.next_tick) {
			struct sb_tg_action a[4];
			bool again = false;
			int n = sb_tg_tick(&c.tg, c.now_ms * 1000000ULL, 0, a, 4, &again);
			count_actions(&c, a, n);
			c.next_tick = again ? c.now_ms + 16 : NO_TIME;
		}
	}
	if (pointer) {
		r->taps = c.tg.taps;
		r->secondary = c.tg.secondary;
		r->drags = c.tg.drags;
		r->all_up = c.tg.down_mask == 0 && c.buttons == 0;
		for (int i = 0; i < SB_TG_FINGERS; i++) {
			if (c.tg.a_down[i]) {
				r->all_up = false;
			}
		}
		if (tg->mode == SB_TG_ABSOLUTE) {
			r->pass = r->abs_events > 0 && r->scrolls > 0 && r->presses1 >= 4 && r->all_up;
		} else {
			r->pass = r->taps == 1 && r->drags == 1 && r->secondary == 1 && r->scrolls > 0 &&
			    r->presses1 == 2 && r->presses2 == 1 && r->rel_moves > 0 && r->all_up;
		}
	} else {
		r->all_up = !c.any_down;
		r->pass = r->frames > 0 && r->edge_frames > 0 && r->touch_changes == 14 && r->all_up;
	}
}
