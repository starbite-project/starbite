/* SPDX-License-Identifier: APSL-2.0 */
/* Samsung S6SY761: decoding and protocol (see sb_s6sy761.h). */
#include "sb_s6sy761.h"

enum sb_s6_kind
sb_s6_decode(const uint8_t e[SB_S6_EVENT_LEN], struct sb_s6_event *ev)
{
	ev->tid = -1;
	ev->state = 0;
	ev->rx = ev->ry = 0;
	ev->major = ev->minor = ev->z = ev->left = 0;
	if (e[0] == 0) {
		ev->kind = SB_S6_EMPTY;
		return ev->kind;
	}
	ev->left = e[7] & 0x3fu;
	if ((e[0] & 0x3u) != 0) {
		ev->kind = SB_S6_STATUS;
		return ev->kind;
	}
	ev->tid = (int)((e[0] >> 2) & 0xfu) - 1;
	ev->state = e[0] >> 6;
	ev->rx = (unsigned)e[1] << 4 | e[3] >> 4;
	ev->ry = (unsigned)e[2] << 4 | (e[3] & 0xfu);
	ev->major = e[4];
	ev->minor = e[5];
	ev->z = e[6] & 0x3fu;
	if (ev->tid < 0 || ev->tid >= SB_S6_FINGERS || ev->state == 0) {
		ev->kind = SB_S6_BAD;
		return ev->kind;
	}
	ev->kind = SB_S6_COORD;
	return ev->kind;
}

void
sb_s6_encode(uint8_t e[SB_S6_EVENT_LEN], int tid, unsigned state, unsigned rx, unsigned ry)
{
	e[0] = (uint8_t)(((state & 3u) << 6) | ((((unsigned)tid + 1u) & 0xfu) << 2));
	e[1] = (uint8_t)(rx >> 4);
	e[2] = (uint8_t)(ry >> 4);
	e[3] = (uint8_t)(((rx & 0xfu) << 4) | (ry & 0xfu));
	e[4] = 8;
	e[5] = 6;
	e[6] = 20;
	e[7] = 0;
}

void
sb_s6_setup(struct sb_s6 *s, const struct sb_i2c *bus, unsigned panel_w, unsigned panel_h,
    sb_s6_contact_fn contact, void *ctx)
{
	unsigned char *p = (unsigned char *)s;
	for (unsigned i = 0; i < sizeof(*s); i++) {
		p[i] = 0;
	}
	s->bus = *bus;
	s->addr = SB_S6_I2C_ADDR;
	s->panel_w = panel_w;
	s->panel_h = panel_h;
	s->max_x = s->max_y = 4096;
	s->contact = contact;
	s->contact_ctx = ctx;
}

int
sb_s6_cmd(struct sb_s6 *s, uint8_t reg)
{
	int r = SB_I2C_EINVAL;
	for (unsigned attempt = 0; attempt < SB_S6_ATTEMPTS; attempt++) {
		if (attempt) {
			s->retries++;
			s->bus.udelay(s->bus.ctx, 200);
		}
		r = s->bus.xfer(s->bus.ctx, s->addr, false, &reg, 1);
		if (r == SB_I2C_OK) {
			return r;
		}
	}
	s->last_err = r;
	return r;
}

int
sb_s6_read(struct sb_s6 *s, uint8_t reg, uint8_t *buf, unsigned len)
{
	int r = SB_I2C_EINVAL;
	for (unsigned attempt = 0; attempt < SB_S6_ATTEMPTS; attempt++) {
		if (attempt) {
			s->retries++;
			s->bus.udelay(s->bus.ctx, 200);
		}
		uint8_t cmd = reg;
		r = s->bus.xfer(s->bus.ctx, s->addr, false, &cmd, 1);
		if (r == SB_I2C_OK) {
			r = s->bus.xfer(s->bus.ctx, s->addr, true, buf, len);
		}
		if (r == SB_I2C_OK) {
			return r;
		}
	}
	s->lost++;
	s->last_err = r;
	return r;
}

int
sb_s6_identify(struct sb_s6 *s)
{
	uint8_t b[11];
	int r = sb_s6_read(s, SB_S6_BOOT_STATUS, b, 1);
	if (r != SB_I2C_OK) {
		return r;
	}
	s->boot_status = b[0];
	if (sb_s6_read(s, SB_S6_DEVICE_ID, b, 3) == SB_I2C_OK) {
		s->id[0] = b[0];
		s->id[1] = b[1];
		s->id[2] = b[2];
		s->have_id = true;
	}
	if (sb_s6_read(s, SB_S6_PANEL_INFO, b, 11) == SB_I2C_OK) {
		unsigned mx = (unsigned)b[0] << 8 | b[1], my = (unsigned)b[2] << 8 | b[3];
		s->chan_x = b[8];
		s->chan_y = b[9];
		s->have_panel = true;
		/* a range below 1000 is not a panel's: keep the default rather than divide by nonsense */
		if (mx >= 1000 && my >= 1000) {
			s->max_x = mx;
			s->max_y = my;
		}
	}
	return SB_I2C_OK;
}

int
sb_s6_sense_on(struct sb_s6 *s)
{
	return sb_s6_cmd(s, SB_S6_SENSE_ON);
}

int
sb_s6_clear(struct sb_s6 *s)
{
	s->clears++;
	return sb_s6_cmd(s, SB_S6_CLEAR_EVENT_STACK);
}

void
sb_s6_release_all(struct sb_s6 *s)
{
	for (int f = 0; f < SB_S6_FINGERS; f++) {
		if (s->down[f]) {
			s->down[f] = false;
			if (s->contact) {
				s->contact(s->contact_ctx, f, false, s->x[f], s->y[f]);
			}
		}
	}
}

enum sb_s6_kind
sb_s6_feed(struct sb_s6 *s, const uint8_t e[SB_S6_EVENT_LEN])
{
	struct sb_s6_event ev;
	enum sb_s6_kind k = sb_s6_decode(e, &ev);
	if (k == SB_S6_EMPTY) {
		return k;
	}
	s->events++;
	if (k == SB_S6_STATUS) {
		s->status_events++;
		return k;
	}
	if (k == SB_S6_BAD) {
		s->bad_events++;
		return k;
	}
	unsigned rx = ev.rx < s->max_x ? ev.rx : s->max_x - 1;
	unsigned ry = ev.ry < s->max_y ? ev.ry : s->max_y - 1;
	int x = (int)((uint64_t)rx * s->panel_w / s->max_x);
	int y = (int)((uint64_t)ry * s->panel_h / s->max_y);
	s->x[ev.tid] = x;
	s->y[ev.tid] = y;
	bool down = ev.state != SB_S6_RELEASE;
	s->down[ev.tid] = down;
	if (s->contact) {
		s->contact(s->contact_ctx, ev.tid, down, x, y);
	}
	return k;
}

unsigned
sb_s6_service(struct sb_s6 *s, bool int_low)
{
	if (!int_low) {
		s->stuck = 0;
		return 0;
	}
	unsigned n = 0;
	uint8_t e[SB_S6_EVENT_LEN];
	while (n < SB_S6_BURST) {
		if (sb_s6_read(s, SB_S6_READ_ONE_EVENT, e, sizeof(e)) != SB_I2C_OK || e[0] == 0) {
			break;
		}
		(void)sb_s6_feed(s, e);
		n++;
	}
	if (n == SB_S6_BURST || (n == 0 && ++s->stuck >= SB_S6_STUCK_POLLS)) {
		sb_s6_release_all(s);
		(void)sb_s6_clear(s);
		s->stuck = 0;
	} else if (n) {
		s->stuck = 0;
	}
	return n;
}
