/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteTouch host tests: the HSI2C state machine against the register model, the S6SY761 protocol over it,
 * event decoding, the macOS pointer gestures (with a model of WindowServer's pointer and the display's drawn
 * pointer layer), the iOS digitizer frames, scripted input and the packet self-test.
 */
#include <stdio.h>
#include <string.h>
#include "mock.h"
#include "sb_hsi2c.h"
#include "sb_s6sy761.h"
#include "sb_touch_gesture.h"
#include "sb_touch_script.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; \
	if (_a != _b) { failures++; printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* ---------- HSI2C ---------- */

static void
setup_bus(struct mock *m, struct sb_mmio_ops *ops, struct sb_hsi2c *b)
{
	mock_init(m);
	mock_ops(m, ops);
	sb_hsi2c_setup(b, ops, MOCK_HSI2C, NULL);
}

static void
test_hsi2c_reset(void)
{
	struct mock m; struct sb_mmio_ops ops; struct sb_hsi2c b;
	setup_bus(&m, &ops, &b);
	m.hsi2c[SB_HSI2C_FS1 / 4] = 0x12345678;
	m.hsi2c[SB_HSI2C_USI_CON / 4] = 1;
	m.hsi2c[SB_HSI2C_TIMEOUT / 4] = 0x80000010;
	sb_hsi2c_reset(&b);
	CHECK_EQ(m.sw_resets, 1);
	CHECK_EQ(m.hsi2c[SB_HSI2C_USI_CON / 4], 0);
	CHECK_EQ(m.hsi2c[SB_HSI2C_FS1 / 4], 0x12f05678);         /* only [23:16] changed */
	CHECK_EQ(m.hsi2c[SB_HSI2C_FS2 / 4] & 0xff, 0xe0);
	CHECK_EQ((m.hsi2c[SB_HSI2C_FS3 / 4] >> 16) & 0xff, 0x21);
	CHECK_EQ(m.hsi2c[SB_HSI2C_CTL / 4], SB_HSI2C_CTL_MASTER);
	CHECK_EQ(m.hsi2c[SB_HSI2C_TIMEOUT / 4], 0x10);
	CHECK(m.hsi2c[SB_HSI2C_CONF / 4] & SB_HSI2C_CONF_AUTO);
	CHECK_EQ(m.hsi2c[SB_HSI2C_INT_EN / 4], 0);
}

static void
test_hsi2c_transfers(void)
{
	struct mock m; struct sb_mmio_ops ops; struct sb_hsi2c b;
	setup_bus(&m, &ops, &b);
	uint8_t reg = SB_S6_DEVICE_ID, buf[8] = { 0 };

	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, false, &reg, 1), SB_I2C_OK);
	CHECK_EQ(m.chip.reg, SB_S6_DEVICE_ID);
	CHECK_EQ(m.hsi2c[SB_HSI2C_ADDR / 4], 0x48u << 10);
	CHECK_EQ(m.hsi2c[SB_HSI2C_FIFO_CTL / 4], 3u | (1u << 4) | (1u << 16));
	CHECK_EQ(b.state, SB_HSI2C_IDLE);

	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, true, buf, 3), SB_I2C_OK);
	CHECK(buf[0] == 0xac && buf[1] == 0x37 && buf[2] == 0x61);
	CHECK(m.hsi2c[SB_HSI2C_AUTO_CONF / 4] & SB_HSI2C_AUTO_READ);
	CHECK(m.hsi2c[SB_HSI2C_AUTO_CONF / 4] & SB_HSI2C_AUTO_STOP);
	CHECK_EQ(m.hsi2c[SB_HSI2C_AUTO_CONF / 4] & 0xffff, 3);
	CHECK_EQ(m.hsi2c[SB_HSI2C_CTL / 4], SB_HSI2C_CTL_MASTER | SB_HSI2C_CTL_RXCHON);

	/* a long write that the FIFO takes in pieces */
	uint8_t longw[20];
	for (int i = 0; i < 20; i++) longw[i] = (uint8_t)(0x40 + i);
	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, false, longw, 20), SB_I2C_OK);
	CHECK_EQ(m.txbuf[19], 0x53);
	CHECK_EQ(m.hsi2c[SB_HSI2C_FIFO_CTL / 4], 3u | (8u << 4) | (8u << 16));
	CHECK_EQ(b.xfers, 3);

	/* every transfer starts from a reset bus (samsung,reset-before-trans) */
	CHECK(m.sw_resets >= 3);
}

static void
test_hsi2c_errors(void)
{
	struct mock m; struct sb_mmio_ops ops; struct sb_hsi2c b;
	uint8_t buf[8];
	setup_bus(&m, &ops, &b);

	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, true, buf, 0), SB_I2C_EINVAL);
	uint8_t big[40];
	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, false, big, 33), SB_I2C_EINVAL);

	m.chip.present = false;
	unsigned resets = m.sw_resets;
	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, true, buf, 1), SB_I2C_ENOACK);
	CHECK(b.fail_int & SB_HSI2C_INT_NOACK);
	CHECK(m.sw_resets >= resets + 2);       /* the start's reset and the recovery reset */
	CHECK_EQ(m.hsi2c[SB_HSI2C_INT_EN / 4], 0);
	m.chip.present = true;
	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, true, buf, 1), SB_I2C_OK);     /* recovers */

	CHECK_EQ(sb_hsi2c_xfer(&b, 0x49, true, buf, 1), SB_I2C_ENOACK); /* wrong address */

	m.fault_no_done = true;
	m.udelay_us = 0;
	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, true, buf, 2), SB_I2C_ETIMEOUT);
	CHECK_EQ(m.udelay_us, SB_HSI2C_RUN_STEPS - 1);                  /* bounded: 50 ms */
	m.fault_no_done = false;

	m.fault_stuck_busy = true;
	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, true, buf, 2), SB_I2C_EBUSY);
	m.fault_stuck_busy = false;
	CHECK_EQ(sb_hsi2c_xfer(&b, 0x48, true, buf, 2), SB_I2C_OK);
	CHECK(b.failures == 4);

	/* the stepwise interface: the state sequence of one read */
	CHECK_EQ(sb_hsi2c_start(&b, 0x48, true, buf, 8), SB_I2C_OK);
	CHECK_EQ(b.state, SB_HSI2C_RUN);
	int run = 0, wait = 0;
	enum sb_hsi2c_state s;
	while ((s = sb_hsi2c_step(&b)) != SB_HSI2C_DONE && s != SB_HSI2C_ERROR) {
		run += s == SB_HSI2C_RUN;
		wait += s == SB_HSI2C_WAIT_IDLE;
	}
	CHECK_EQ(s, SB_HSI2C_DONE);
	CHECK_EQ(run, 2);       /* 8 bytes at 3 per poll: 3 + 3 (RUN), 2 (then WAIT_IDLE) */
	CHECK(wait >= 1);       /* MASTER_BUSY held for a few polls */
	CHECK_EQ(strcmp(sb_i2c_strerror(SB_I2C_ENOACK), "no ack"), 0);
}

static void
test_platform_init(void)
{
	struct mock m; struct sb_mmio_ops ops;
	mock_init(&m);
	mock_ops(&m, &ops);
	struct sb_usi_i2c_platform p = sb_usi03_touch_bus;
	p.cmu = MOCK_CMU; p.sysreg = MOCK_SYSREG; p.gpio = MOCK_GPIO;
	m.cmu[0x2018 / 4] = 0x1;
	m.gpio[0x20 / 4] = 0x12345678;          /* GPP1 CON */
	m.gpio[0x28 / 4] = 0xffffffff;          /* PUD */
	m.gpio[0x2c / 4] = 0xffffffff;          /* DRV */
	m.gpio[0x24 / 4] = 0x30;                /* DAT: SCL, SDA high */
	sb_hsi2c_platform_init(&ops, &p);
	CHECK_EQ(m.cmu[0x2018 / 4], 0x00300001);
	CHECK_EQ(m.cmu[0x206c / 4], 0x00300000);
	CHECK_EQ(m.cmu[0x20dc / 4], 0x00300000);
	CHECK_EQ(m.cmu[0x20e0 / 4], 0x00300000);
	CHECK_EQ(m.cmu[0x3038 / 4], 0x3);
	CHECK_EQ(m.sysreg[0x1c / 4], 4);
	CHECK_EQ(m.gpio[0x20 / 4], 0x12225678);
	CHECK_EQ(m.gpio[0x28 / 4], 0xff00ffff);
	CHECK_EQ(m.gpio[0x2c / 4], 0xff00ffff);
	CHECK_EQ(sb_hsi2c_pin_levels(&ops, &p), 3);
}

/* ---------- S6SY761 ---------- */

struct contacts {
	int n;
	int finger[256], x[256], y[256];
	bool down[256];
};

static void
record(void *ctx, int finger, bool down, int x, int y)
{
	struct contacts *c = (struct contacts *)ctx;
	if (c->n < 256) {
		c->finger[c->n] = finger; c->down[c->n] = down; c->x[c->n] = x; c->y[c->n] = y;
		c->n++;
	}
}

static void
test_s6_decode(void)
{
	struct sb_s6_event ev;
	uint8_t e[8];
	for (int tid = 0; tid < 10; tid++) {
		for (unsigned st = 1; st <= 3; st++) {
			sb_s6_encode(e, tid, st, 0xabc, 0x123);
			CHECK_EQ(sb_s6_decode(e, &ev), SB_S6_COORD);
			CHECK_EQ(ev.tid, tid);
			CHECK_EQ(ev.state, st);
			CHECK_EQ(ev.rx, 0xabc);
			CHECK_EQ(ev.ry, 0x123);
		}
	}
	/* a real packet's layout: tid 1 (finger 0) pressed at x 0x2d5, y 0x9b4 */
	uint8_t real[8] = { 0x44, 0x2d, 0x9b, 0x54, 0x0a, 0x08, 0x25, 0x02 };
	CHECK_EQ(sb_s6_decode(real, &ev), SB_S6_COORD);
	CHECK_EQ(ev.tid, 0);
	CHECK_EQ(ev.state, SB_S6_PRESS);
	CHECK_EQ(ev.rx, 0x2d5);
	CHECK_EQ(ev.ry, 0x9b4);
	CHECK_EQ(ev.major, 10);
	CHECK_EQ(ev.minor, 8);
	CHECK_EQ(ev.z, 0x25);
	CHECK_EQ(ev.left, 2);
	uint8_t empty[8] = { 0 };
	CHECK_EQ(sb_s6_decode(empty, &ev), SB_S6_EMPTY);
	uint8_t status[8] = { 0x01, 0x00, 0, 0, 0, 0, 0, 0 };
	CHECK_EQ(sb_s6_decode(status, &ev), SB_S6_STATUS);
	uint8_t tid0[8] = { 0x40, 1, 1, 0, 0, 0, 0, 0 };          /* TID 0 */
	CHECK_EQ(sb_s6_decode(tid0, &ev), SB_S6_BAD);
	uint8_t tid11[8] = { 0x40 | (11 << 2), 1, 1, 0, 0, 0, 0, 0 };
	CHECK_EQ(sb_s6_decode(tid11, &ev), SB_S6_BAD);
	uint8_t state0[8] = { 0x04, 1, 1, 0, 0, 0, 0, 0 };
	CHECK_EQ(sb_s6_decode(state0, &ev), SB_S6_BAD);
}

static void
setup_chip(struct mock *m, struct sb_mmio_ops *ops, struct sb_hsi2c *b, struct sb_s6 *s, struct contacts *c)
{
	struct sb_i2c i2c;
	setup_bus(m, ops, b);
	sb_hsi2c_as_i2c(b, &i2c);
	memset(c, 0, sizeof(*c));
	sb_s6_setup(s, &i2c, 1440, 2960, record, c);
}

static void
test_s6_probe_and_events(void)
{
	struct mock m; struct sb_mmio_ops ops; struct sb_hsi2c b; struct sb_s6 s; struct contacts c;
	setup_chip(&m, &ops, &b, &s, &c);

	m.chip.nack_next = 3;                   /* the chip NACKs while it boots/scans */
	CHECK_EQ(sb_s6_identify(&s), SB_I2C_OK);
	CHECK_EQ(s.boot_status, SB_S6_BOOT_APP);
	CHECK(s.have_id && s.id[0] == 0xac && s.id[1] == 0x37 && s.id[2] == 0x61);
	CHECK(s.have_panel);
	CHECK_EQ(s.max_x, 4095);
	CHECK_EQ(s.max_y, 4095);
	CHECK_EQ(s.chan_x, 16);
	CHECK_EQ(s.chan_y, 32);
	CHECK_EQ(s.retries, 3);
	CHECK_EQ(sb_s6_sense_on(&s), SB_I2C_OK);
	CHECK(m.chip.sensing);
	CHECK_EQ(sb_s6_clear(&s), SB_I2C_OK);
	CHECK_EQ(m.chip.clear_cmds, 1);

	/* a tap and a move through the whole stack: register model -> state machine -> decoding -> pixels */
	fake_push(&m.chip, 0, SB_S6_PRESS, 2047, 4094);
	fake_push(&m.chip, 0, SB_S6_MOVE, 4094, 0);
	fake_push(&m.chip, 0, SB_S6_RELEASE, 4094, 0);
	uint8_t st[8] = { 0x01, 0, 0, 0, 0, 0, 0, 0 };
	fake_push_raw(&m.chip, st);
	CHECK(fake_int_low(&m.chip));
	CHECK_EQ(sb_s6_service(&s, fake_int_low(&m.chip)), 4);
	CHECK(!fake_int_low(&m.chip));
	CHECK_EQ(c.n, 3);
	CHECK(c.finger[0] == 0 && c.down[0] && c.x[0] == 719 && c.y[0] == 2959);
	CHECK(c.down[1] && c.x[1] == 1439 && c.y[1] == 0);
	CHECK(!c.down[2]);
	CHECK_EQ(s.status_events, 1);
	CHECK_EQ(sb_s6_service(&s, false), 0);

	/* chip absent: identify fails after the bounded retries */
	m.chip.present = false;
	CHECK_EQ(sb_s6_identify(&s), SB_I2C_ENOACK);
	CHECK(s.lost >= 1);
	m.chip.present = true;
}

static void
test_s6_recovery(void)
{
	struct mock m; struct sb_mmio_ops ops; struct sb_hsi2c b; struct sb_s6 s; struct contacts c;
	setup_chip(&m, &ops, &b, &s, &c);

	/* INT stays low with an empty stack: after 200 polls every finger is released and the stack cleared */
	fake_push(&m.chip, 3, SB_S6_PRESS, 100, 100);
	CHECK_EQ(sb_s6_service(&s, true), 1);
	m.chip.never_empty_but_int = true;
	for (int i = 0; i < SB_S6_STUCK_POLLS - 1; i++) {
		sb_s6_service(&s, fake_int_low(&m.chip));
	}
	CHECK_EQ(m.chip.clear_cmds, 0);
	sb_s6_service(&s, fake_int_low(&m.chip));
	CHECK_EQ(m.chip.clear_cmds, 1);
	CHECK(!fake_int_low(&m.chip));
	CHECK_EQ(c.n, 2);
	CHECK(c.finger[1] == 3 && !c.down[1]);

	/* a full burst (32 events) also releases and clears */
	memset(&c, 0, sizeof(c));
	for (int i = 0; i < 40; i++) {
		fake_push(&m.chip, 1, i ? SB_S6_MOVE : SB_S6_PRESS, 100u + (unsigned)i, 100);
	}
	CHECK_EQ(sb_s6_service(&s, true), SB_S6_BURST);
	CHECK_EQ(m.chip.clear_cmds, 2);
	CHECK_EQ(c.n, 33);                      /* 32 contacts, then the release */
	CHECK(!c.down[32]);
}

/* ---------- macOS pointer ---------- */

/* WindowServer's pointer (canvas points, clamped) and the display's pointer layer, drawn every tick. */
struct screen {
	int32_t px, py;                         /* the hotspot's position */
	int32_t hot_x, hot_y;                   /* the arrow's hotspot inside its image */
	uint32_t seq;
	uint32_t buttons;
	int clicks1, clicks2, releases, scroll_x, scroll_y, scroll_events;
	int32_t click_x, click_y;
	int32_t abs_x, abs_y;
	bool draws;
};

static void
apply(struct screen *sc, const struct sb_tg_action *a, int n, const struct sb_tg_config *cfg)
{
	for (int i = 0; i < n; i++) {
		if (a[i].kind == SB_TG_SCROLL) {
			sc->scroll_x += a[i].lines_x;
			sc->scroll_y += a[i].lines_y;
			sc->scroll_events++;
			continue;
		}
		if (a[i].kind == SB_TG_REL) {
			sc->px += a[i].dx;
			sc->py += a[i].dy;
			sc->px = sc->px < 0 ? 0 : (sc->px >= cfg->canvas_w ? cfg->canvas_w - 1 : sc->px);
			sc->py = sc->py < 0 ? 0 : (sc->py >= cfg->canvas_h ? cfg->canvas_h - 1 : sc->py);
		} else {
			sc->abs_x = (int32_t)a[i].nx;
			sc->abs_y = (int32_t)a[i].ny;
		}
		uint32_t pressed = a[i].buttons & ~sc->buttons;
		if (pressed & 1u) { sc->clicks1++; sc->click_x = sc->px; sc->click_y = sc->py; }
		if (pressed & 2u) { sc->clicks2++; }
		if (sc->buttons & ~a[i].buttons) { sc->releases++; }
		sc->buttons = a[i].buttons;
	}
}

struct sim {
	struct sb_tg g;
	struct screen sc;
	uint64_t now_ms, next_tick;
};

static void
sim_init(struct sim *s, int mode, bool draws)
{
	struct sb_tg_config cfg;
	memset(s, 0, sizeof(*s));
	sb_tg_default_config(&cfg);
	cfg.mode = mode;
	sb_tg_init(&s->g, &cfg);
	s->sc.px = 400;
	s->sc.py = 900;
	s->sc.hot_x = 3;
	s->sc.hot_y = 5;
	s->sc.draws = draws;
	s->next_tick = (uint64_t)-1;
}

static void
sim_run(struct sim *s, uint64_t until_ms)
{
	for (; s->now_ms < until_ms; s->now_ms++) {
		if (s->next_tick != (uint64_t)-1 && s->now_ms >= s->next_tick) {
			struct sb_tg_action a[4];
			struct sb_tg_seen seen = { s->sc.draws, s->sc.seq, s->sc.px - s->sc.hot_x, s->sc.py - s->sc.hot_y };
			bool again = false;
			int n = sb_tg_tick(&s->g, s->now_ms * 1000000ULL, &seen, a, 4, &again);
			apply(&s->sc, a, n, &s->g.cfg);
			if (s->sc.draws) {
				s->sc.seq++;            /* the display drew a frame with the pointer layer */
			}
			s->next_tick = again ? s->now_ms + 16 : (uint64_t)-1;
		}
	}
}

static void
sim_touch(struct sim *s, int finger, bool down, int x, int y)
{
	struct sb_tg_action a[4];
	bool kick = false;
	int n = sb_tg_contact(&s->g, s->now_ms * 1000000ULL, finger, down, x, y, a, 4, &kick);
	apply(&s->sc, a, n, &s->g.cfg);
	if (kick && s->next_tick == (uint64_t)-1) {
		s->next_tick = s->now_ms + 1;
	}
}

/* panel pixel -> canvas point, as the engine maps it (strip 1367 px centred: bar 36) */
static int32_t cx(int x) { return (int32_t)((int64_t)(x - 36) * 828 / 1367); }
static int32_t cy(int y) { return (int32_t)((int64_t)y * 1792 / 2960); }

static void
test_pointer_tap_click(bool draws)
{
	struct sim s;
	sim_init(&s, SB_TG_DIRECT, draws);
	sim_touch(&s, 0, true, 700, 1800);
	sim_run(&s, 80);
	sim_touch(&s, 0, false, 700, 1800);
	sim_run(&s, 1500);
	CHECK_EQ(s.g.cal, 2);
	if (draws) {
		CHECK_EQ(s.g.hot_x, 3);
		CHECK_EQ(s.g.hot_y, 5);
	}
	CHECK_EQ(s.sc.clicks1, 1);
	CHECK_EQ(s.sc.releases, 1);
	CHECK(s.sc.click_x >= cx(700) - 2 && s.sc.click_x <= cx(700) + 2);
	CHECK(s.sc.click_y >= cy(1800) - 2 && s.sc.click_y <= cy(1800) + 2);
	CHECK_EQ(s.sc.buttons, 0);
	CHECK_EQ(s.next_tick, (uint64_t)-1);    /* steering stops when idle */
	CHECK_EQ(s.g.taps, 1);

	/* a second tap elsewhere: no new calibration, click where the finger is */
	sim_touch(&s, 0, true, 1200, 400);
	sim_run(&s, 1560);
	sim_touch(&s, 0, false, 1200, 400);
	sim_run(&s, 2500);
	CHECK_EQ(s.sc.clicks1, 2);
	CHECK(s.sc.click_x >= cx(1200) - 2 && s.sc.click_x <= cx(1200) + 2);
	CHECK(s.sc.click_y >= cy(400) - 2 && s.sc.click_y <= cy(400) + 2);
}

static void
test_pointer_follow_and_drag(void)
{
	struct sim s;
	sim_init(&s, SB_TG_DIRECT, true);
	/* a quick move (no hold): the pointer follows, no button */
	sim_touch(&s, 0, true, 300, 600);
	sim_run(&s, 100);
	for (int i = 1; i <= 40; i++) {
		sim_touch(&s, 0, true, 300 + 15 * i, 600 + 20 * i);
		sim_run(&s, s.now_ms + 16);
	}
	sim_run(&s, s.now_ms + 700);
	CHECK(s.sc.px >= cx(900) - 2 && s.sc.px <= cx(900) + 2);
	CHECK(s.sc.py >= cy(1400) - 2 && s.sc.py <= cy(1400) + 2);
	CHECK_EQ(s.sc.clicks1, 0);
	sim_touch(&s, 0, false, 900, 1400);
	sim_run(&s, s.now_ms + 100);
	CHECK_EQ(s.sc.clicks1, 0);              /* a long touch with travel is no tap */

	/* touch, hold 400 ms, move, lift: a drag with button 1 */
	sim_touch(&s, 0, true, 500, 500);
	sim_run(&s, s.now_ms + 400);
	CHECK_EQ(s.sc.buttons, 1);
	CHECK_EQ(s.g.drags, 1);
	for (int i = 1; i <= 10; i++) {
		sim_touch(&s, 0, true, 500 + 30 * i, 500);
		sim_run(&s, s.now_ms + 16);
	}
	sim_run(&s, s.now_ms + 200);
	CHECK_EQ(s.sc.buttons, 1);
	CHECK(s.sc.px >= cx(800) - 2 && s.sc.px <= cx(800) + 2);
	sim_touch(&s, 0, false, 800, 500);
	CHECK_EQ(s.sc.buttons, 0);
	CHECK_EQ(s.sc.clicks1, 1);
	CHECK_EQ(s.sc.releases, 1);
}

static void
test_pointer_two_fingers(void)
{
	struct sim s;
	sim_init(&s, SB_TG_DIRECT, true);
	/* two-finger scroll: the fingers move down 240 px = 10 lines; natural: the wheel goes the other way */
	sim_touch(&s, 0, true, 700, 1000);
	sim_run(&s, 20);
	sim_touch(&s, 1, true, 800, 1000);
	for (int i = 1; i <= 10; i++) {
		sim_touch(&s, 0, true, 700, 1000 + 24 * i);
		sim_touch(&s, 1, true, 800, 1000 + 24 * i);
		sim_run(&s, s.now_ms + 16);
	}
	CHECK_EQ(s.sc.scroll_y, -10);
	CHECK_EQ(s.sc.scroll_x, 0);
	CHECK_EQ(s.sc.scroll_events, 10);
	sim_touch(&s, 1, false, 800, 1240);
	sim_touch(&s, 0, false, 700, 1240);
	sim_run(&s, s.now_ms + 600);
	CHECK_EQ(s.sc.clicks1, 0);
	CHECK_EQ(s.sc.clicks2, 0);
	CHECK_EQ(s.sc.buttons, 0);

	/* a two-finger tap: secondary click */
	sim_touch(&s, 0, true, 700, 1000);
	sim_run(&s, s.now_ms + 10);
	sim_touch(&s, 1, true, 800, 1000);
	sim_run(&s, s.now_ms + 80);
	sim_touch(&s, 1, false, 800, 1000);
	sim_touch(&s, 0, false, 700, 1000);
	sim_run(&s, s.now_ms + 600);
	CHECK_EQ(s.sc.clicks2, 1);
	CHECK_EQ(s.sc.clicks1, 0);
	CHECK_EQ(s.sc.buttons, 0);

	/* scroll_step < 0 flips the direction */
	struct sb_tg_config cfg;
	sb_tg_default_config(&cfg);
	cfg.scroll_step = -24;
	sb_tg_init(&s.g, &cfg);
	memset(&s.sc, 0, sizeof(s.sc));
	sim_touch(&s, 0, true, 700, 1000);
	sim_touch(&s, 1, true, 800, 1000);
	sim_touch(&s, 0, true, 700, 1048);
	CHECK_EQ(s.sc.scroll_y, 2);

	/* the primary finger lifts first: the other one takes over */
	sb_tg_default_config(&cfg);
	sb_tg_init(&s.g, &cfg);
	memset(&s.sc, 0, sizeof(s.sc));
	sim_touch(&s, 4, true, 700, 1000);
	sim_touch(&s, 7, true, 800, 1000);
	sim_touch(&s, 4, false, 700, 1000);
	CHECK_EQ(s.g.primary, 7);
	sim_touch(&s, 7, false, 800, 1000);
	CHECK_EQ(s.g.primary, -1);
	CHECK_EQ(s.g.down_mask, 0);
}

static void
test_pointer_absolute(void)
{
	struct sim s;
	sim_init(&s, SB_TG_ABSOLUTE, false);
	sim_touch(&s, 0, true, 36, 0);          /* the strip's top-left corner */
	CHECK_EQ(s.sc.abs_x, 0);
	CHECK_EQ(s.sc.abs_y, 0);
	CHECK_EQ(s.sc.buttons, 1);
	sim_touch(&s, 0, true, 1439, 2959);     /* beyond the strip: clamped */
	CHECK_EQ(s.sc.abs_x, 0xffff);
	CHECK_EQ(s.sc.abs_y, (2959 << 16) / 2960);
	sim_touch(&s, 0, true, 1439, 2959);     /* no change, no event */
	CHECK_EQ(s.g.moves, 2);
	sim_touch(&s, 1, true, 600, 600);       /* second finger: scroll mode, button released */
	CHECK_EQ(s.sc.buttons, 0);
	sim_touch(&s, 0, true, 1439, 2959 - 48);
	CHECK_EQ(s.sc.scroll_y, 2);             /* fingers up: content up, natural */
	sim_touch(&s, 0, false, 1439, 2911);
	sim_touch(&s, 1, false, 600, 600);
	CHECK(!s.g.a_scroll);
	CHECK_EQ(s.g.a_primary, -1);
	CHECK_EQ(s.sc.clicks1, 1);
	CHECK_EQ(s.sc.releases, 1);
	/* tick does nothing in this mode */
	struct sb_tg_action a[4];
	bool again = true;
	CHECK_EQ(sb_tg_tick(&s.g, 0, NULL, a, 4, &again), 0);
	CHECK(!again);
}

/* ---------- iOS digitizer ---------- */

static void
test_digitizer(void)
{
	struct sb_dig d;
	struct sb_dig_frame f;
	/* 828 x 1792 drawn full height: 1367 px wide strip, bars of 36 px */
	struct sb_dig_config cfg = { 1440, 2960, 1367, 2960 };
	sb_dig_init(&d, &cfg);
	CHECK_EQ(d.off_x, -36);

	CHECK(sb_dig_contact(&d, 0, true, 720, 1480, &f));
	CHECK_EQ(f.count, 1);
	CHECK_EQ(f.c[0].index, 0);
	CHECK_EQ(f.c[0].identity, 2);
	CHECK(f.c[0].touch);
	CHECK_EQ(f.c[0].mask, SB_DIG_TOUCH | SB_DIG_RANGE | SB_DIG_POSITION | SB_DIG_IDENTITY);
	CHECK_EQ(f.nx, (uint32_t)(((720 - 36) << 16) / 1367));
	CHECK_EQ(f.ny, (uint32_t)((1480 << 16) / 2960));
	CHECK(f.any);
	CHECK(f.changed_touch);

	CHECK(!sb_dig_contact(&d, 0, true, 720, 1480, &f));      /* same place: no frame */

	CHECK(sb_dig_contact(&d, 1, true, 1430, 1000, &f));      /* on the right bar: an edge contact */
	CHECK_EQ(f.count, 2);
	CHECK_EQ(f.c[0].mask, 0);                               /* finger 0 rides along, unchanged */
	CHECK(f.c[1].mask & SB_DIG_FROM_EDGE_TIP);
	CHECK_EQ(f.c[1].nx, 0xffff);

	CHECK(sb_dig_contact(&d, 1, true, 1000, 1000, &f));      /* moves: keeps FromEdgeTip for life */
	CHECK_EQ(f.c[1].mask, SB_DIG_FROM_EDGE_TIP | SB_DIG_POSITION);

	CHECK(sb_dig_contact(&d, 0, false, 720, 1480, &f));
	CHECK_EQ(f.count, 2);
	CHECK(!f.c[0].touch);
	CHECK_EQ(f.c[0].mask, SB_DIG_TOUCH | SB_DIG_RANGE);
	CHECK(f.any);                                           /* finger 1 still down */
	CHECK(sb_dig_contact(&d, 1, false, 1000, 1000, &f));
	CHECK_EQ(f.count, 1);
	CHECK(!f.any);
	CHECK(!sb_dig_contact(&d, 1, false, 1000, 1000, &f));    /* already up */

	/* bottom edge on a full-panel display */
	struct sb_dig_config full = { 1440, 2960, 1440, 2960 };
	sb_dig_init(&d, &full);
	CHECK(sb_dig_contact(&d, 2, true, 720, 2955, &f));
	CHECK(f.mask & SB_DIG_FROM_EDGE_TIP);
	CHECK(sb_dig_contact(&d, 3, true, 720, 1000, &f));
	CHECK(!(f.c[1].mask & SB_DIG_FROM_EDGE_TIP));
	/* a display larger than the panel (1125 x 2436 logical in a 1440-wide panel would be offset > 0) */
	struct sb_dig_config big = { 1440, 2960, 1500, 3000 };
	sb_dig_init(&d, &big);
	CHECK(sb_dig_contact(&d, 0, true, 0, 0, &f));
	CHECK_EQ(f.nx, (uint32_t)((30 << 16) / 1500));
	CHECK_EQ(f.ny, (uint32_t)((20 << 16) / 3000));
	CHECK(!sb_dig_contact(&d, 11, true, 0, 0, &f));
}

/* ---------- script and self-test ---------- */

static void
test_script(void)
{
	struct sb_script s;
	CHECK_EQ(sb_script_parse(&s, "20t556:1406,30s720:2300:720:900"), 2);
	CHECK_EQ(s.bad, -1);
	CHECK_EQ(s.steps[0].at_ms, 20000);
	CHECK(s.steps[0].kind == 't' && s.steps[0].x0 == 556 && s.steps[0].y0 == 1406);
	CHECK(s.steps[1].kind == 's' && s.steps[1].x1 == 720 && s.steps[1].y1 == 900);
	CHECK_EQ(sb_script_parse(&s, "5t1:2,6h,7t3:4"), 1);    /* Home is a key: the parse stops there */
	CHECK_EQ(s.bad, 1);
	CHECK_EQ(sb_script_parse(&s, ""), 0);

	sb_script_parse(&s, "1t100:200,2s0:0:1000:2000");
	int f, x, y; bool down; uint64_t next;
	CHECK(!sb_script_next(&s, 999, &f, &down, &x, &y, &next));
	CHECK_EQ(next, 1000);
	CHECK(sb_script_next(&s, 1000, &f, &down, &x, &y, &next));
	CHECK(f == SB_SCRIPT_FINGER && down && x == 100 && y == 200);
	CHECK_EQ(next, 1000 + SB_SCRIPT_TAP_MS);
	CHECK(sb_script_next(&s, 1080, &f, &down, &x, &y, &next));
	CHECK(!down);
	CHECK_EQ(next, 2000);
	int samples = 0, ups = 0, lastx = -1, lasty = -1;
	bool monotonic = true;
	for (uint64_t t = 2000; t < 3000; t++) {
		while (sb_script_next(&s, t, &f, &down, &x, &y, &next)) {
			samples++;
			if (!down) { ups++; }
			if (x < lastx || y < lasty) { monotonic = false; }
			lastx = x; lasty = y;
		}
	}
	CHECK_EQ(samples, SB_SCRIPT_SWIPE_SAMPLES + 2);
	CHECK_EQ(ups, 1);
	CHECK(monotonic);
	CHECK(lastx == 1000 && lasty == 2000);
	CHECK_EQ(next, (uint64_t)-1);
}

static void
test_selftest(void)
{
	struct sb_selftest_result r;
	struct sb_tg_config tg;
	struct sb_dig_config dig = { 1440, 2960, 1367, 2960 };
	sb_tg_default_config(&tg);

	sb_touch_selftest(true, &tg, &dig, 1440, 2960, &r);
	printf("  selftest direct: packets %u contacts %u ignored %u rel %u press1 %u press2 %u scroll %u taps %u drags %u secondary %u\n",
	    r.packets, r.contacts, r.ignored, r.rel_moves, r.presses1, r.presses2, r.scrolls, r.taps, r.drags, r.secondary);
	CHECK(r.pass);
	CHECK_EQ(r.packets, 41);
	CHECK_EQ(r.contacts, 39);
	CHECK_EQ(r.ignored, 2);
	CHECK_EQ(r.taps, 1);
	CHECK_EQ(r.drags, 1);
	CHECK_EQ(r.secondary, 1);
	CHECK_EQ(r.presses1, 2);
	CHECK_EQ(r.presses2, 1);
	CHECK_EQ(r.scrolls, 10);
	CHECK(r.all_up);

	tg.mode = SB_TG_ABSOLUTE;
	sb_touch_selftest(true, &tg, &dig, 1440, 2960, &r);
	printf("  selftest absolute: abs %u press1 %u scroll %u\n", r.abs_events, r.presses1, r.scrolls);
	CHECK(r.pass);
	CHECK_EQ(r.presses1, 5);
	CHECK_EQ(r.scrolls, 10);

	sb_touch_selftest(false, &tg, &dig, 1440, 2960, &r);
	printf("  selftest digitizer: frames %u edge %u touch changes %u\n", r.frames, r.edge_frames, r.touch_changes);
	CHECK(r.pass);
	CHECK_EQ(r.touch_changes, 14);
	CHECK_EQ(r.edge_frames, 7);             /* the edge swipe's press, 5 moves, release */
}

int
main(void)
{
	struct { const char *name; void (*fn)(void); } tests[] = {
		{ "hsi2c reset", test_hsi2c_reset },
		{ "hsi2c transfers", test_hsi2c_transfers },
		{ "hsi2c errors and recovery", test_hsi2c_errors },
		{ "usi i2c platform init", test_platform_init },
		{ "s6sy761 decode", test_s6_decode },
		{ "s6sy761 probe and events over hsi2c", test_s6_probe_and_events },
		{ "s6sy761 stuck stack and burst", test_s6_recovery },
		{ "pointer tap = click (display draws)", NULL },
		{ "pointer tap = click (dead reckoning)", NULL },
		{ "pointer follow and drag", test_pointer_follow_and_drag },
		{ "pointer two fingers", test_pointer_two_fingers },
		{ "pointer absolute", test_pointer_absolute },
		{ "digitizer frames", test_digitizer },
		{ "script", test_script },
		{ "packet self-test", test_selftest },
	};
	for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int before = failures;
		if (i == 7) {
			test_pointer_tap_click(true);
		} else if (i == 8) {
			test_pointer_tap_click(false);
		} else {
			tests[i].fn();
		}
		printf("%s: %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
	}
	printf("StarbiteTouch host tests: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
