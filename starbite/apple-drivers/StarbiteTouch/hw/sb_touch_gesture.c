/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Touch -> pointer (macOS) and touch -> digitizer frames (iOS); see sb_touch_gesture.h. Ported from today's
 * SweetHID.cpp (patches 0323, 0071, 0373, 0378, 0394, 0397: our code) without its IOHIDEvent calls, so the
 * logic is unchanged and testable on the Mac.
 */
#include "sb_touch_gesture.h"

static void
zero(void *p, unsigned n)
{
	unsigned char *c = (unsigned char *)p;
	for (unsigned i = 0; i < n; i++) {
		c[i] = 0;
	}
}

static int
iabs(int v)
{
	return v < 0 ? -v : v;
}

static int32_t
clampi(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

void
sb_tg_default_config(struct sb_tg_config *c)
{
	zero(c, sizeof(*c));
	c->mode = SB_TG_DIRECT;
	c->panel_w = 1440;
	c->panel_h = 2960;
	c->strip_w = 1367;              /* 828 x 1792 drawn full height: 828 * 2960 / 1792 */
	c->canvas_w = 828;
	c->canvas_h = 1792;
	c->scroll_step = 24;
	c->tap_ms = 300;
	c->tap_travel = 40;
	c->hold_ms = 350;
	c->hold_travel = 20;
	c->click_wait_ticks = 20;
}

void
sb_tg_init(struct sb_tg *g, const struct sb_tg_config *c)
{
	zero(g, sizeof(*g));
	g->cfg = *c;
	if (g->cfg.strip_w <= 0 || g->cfg.strip_w > g->cfg.panel_w) {
		g->cfg.strip_w = g->cfg.panel_w;
	}
	if (g->cfg.scroll_step == 0) {
		g->cfg.scroll_step = 24;
	}
	g->primary = -1;
	g->a_primary = -1;
}

static int
emit(struct sb_tg_action *out, int n, int max, enum sb_tg_kind kind, int32_t a, int32_t b, uint32_t buttons)
{
	if (n >= max) {
		return n;
	}
	struct sb_tg_action *x = &out[n];
	zero(x, sizeof(*x));
	x->kind = kind;
	x->buttons = buttons;
	if (kind == SB_TG_REL) {
		x->dx = a;
		x->dy = b;
	} else if (kind == SB_TG_ABS) {
		x->nx = (uint32_t)a;
		x->ny = (uint32_t)b;
	} else {
		x->lines_x = a;
		x->lines_y = b;
	}
	return n + 1;
}

/* Scroll lines from accumulated panel pixels; the remainder stays accumulated. */
static int
scroll_lines(const struct sb_tg_config *c, int *acc_x, int *acc_y, int32_t *lx, int32_t *ly)
{
	int step = iabs(c->scroll_step);
	int sx = *acc_x / step, sy = *acc_y / step;
	if (!sx && !sy) {
		return 0;
	}
	*acc_x -= sx * step;
	*acc_y -= sy * step;
	/* natural (> 0): the content follows the fingers, so the wheel turns the other way */
	*lx = c->scroll_step > 0 ? -sx : sx;
	*ly = c->scroll_step > 0 ? -sy : sy;
	return 1;
}

static int
direct_contact(struct sb_tg *g, uint64_t now, int finger, bool down, int x, int y,
    struct sb_tg_action *out, int max, bool *kick)
{
	const struct sb_tg_config *c = &g->cfg;
	int n = 0;
	int bar = (c->panel_w - c->strip_w) / 2;
	int32_t lx = (int32_t)((int64_t)(x - bar) * c->canvas_w / c->strip_w);
	int32_t ly = (int32_t)((int64_t)y * c->canvas_h / c->panel_h);
	lx = clampi(lx, 0, c->canvas_w - 1);
	ly = clampi(ly, 0, c->canvas_h - 1);

	uint32_t bit = 1u << finger;
	bool was = (g->down_mask & bit) != 0;
	if (down) {
		g->down_mask |= bit;
	} else {
		g->down_mask &= ~bit;
	}
	if (down && !was && g->primary < 0) {
		g->primary = finger;
		g->last_x = x;
		g->last_y = y;
		g->travel = 0;
		g->two = false;
		g->dragging = false;
		g->down_at = now;
		g->tgt_x = lx;
		g->tgt_y = ly;
		g->click_button = 0;
		*kick = true;
		g->note = "down";
	} else if (down && !was && finger != g->primary && g->primary >= 0 && !g->two) {
		g->two = true;
		g->acc_x = g->acc_y = 0;
		g->note = "two fingers";
	} else if (down && finger == g->primary) {
		int dx = x - g->last_x, dy = y - g->last_y;
		g->last_x = x;
		g->last_y = y;
		g->travel += iabs(dx) + iabs(dy);
		if (g->two) {
			int32_t sx, sy;
			g->acc_x += dx;
			g->acc_y += dy;
			if (scroll_lines(c, &g->acc_x, &g->acc_y, &sx, &sy)) {
				n = emit(out, n, max, SB_TG_SCROLL, sx, sy, 0);
				g->scroll_events++;
			}
		} else {
			g->tgt_x = lx;
			g->tgt_y = ly;
			g->moves++;
			*kick = !g->active;
		}
	} else if (!down && was && g->down_mask == 0 && g->primary >= 0) {
		uint64_t held = now - g->down_at;
		bool tap = held < (uint64_t)c->tap_ms * 1000000ULL && g->travel < (int)c->tap_travel;
		if (g->dragging) {
			n = emit(out, n, max, SB_TG_REL, 0, 0, 0);
			g->buttons = 0;
			g->dragging = false;
			g->note = "drag end";
		} else if (tap && g->two) {
			n = emit(out, n, max, SB_TG_REL, 0, 0, 2);
			n = emit(out, n, max, SB_TG_REL, 0, 0, 0);
			g->secondary++;
			g->note = "two-finger tap = secondary click";
		} else if (tap) {
			g->click_button = 1;    /* once the pointer is under the finger */
			g->click_ticks = 0;
			*kick = !g->active;
			g->taps++;
			g->note = "tap = click";
		} else {
			g->note = g->two ? "scroll end" : "up";
		}
		g->primary = -1;
		g->two = false;
	} else if (!down && was && finger == g->primary) {
		for (int i = 0; i < SB_TG_FINGERS; i++) {
			if (g->down_mask & (1u << i)) {
				g->primary = i;
				break;
			}
		}
	}
	if (*kick) {
		g->active = true;
	}
	return n;
}

static int
absolute_contact(struct sb_tg *g, int finger, bool down, int x, int y, struct sb_tg_action *out, int max)
{
	const struct sb_tg_config *c = &g->cfg;
	int n = 0;
	int bar = (c->panel_w - c->strip_w) / 2;
	uint32_t nx = (uint32_t)clampi((int32_t)(((int64_t)(x - bar) << 16) / c->strip_w), 0, 0xffff);
	uint32_t ny = (uint32_t)clampi((int32_t)(((int64_t)y << 16) / c->panel_h), 0, 0xffff);
	bool was = g->a_down[finger];
	bool up_now = false;

	if (was == down && (!down || (nx == g->a_x[finger] && ny == g->a_y[finger]))) {
		return 0;
	}
	g->a_down[finger] = down;
	if (down) {
		g->a_x[finger] = nx;
		g->a_y[finger] = ny;
	}
	int count = 0;
	for (int i = 0; i < SB_TG_FINGERS; i++) {
		count += g->a_down[i] ? 1 : 0;
	}
	uint32_t old = g->a_buttons;
	if (!was && down) {
		if (g->a_primary < 0 && !g->a_scroll) {
			g->a_primary = finger;
			g->a_px = x;
			g->a_py = y;
			g->a_buttons = 1;
			g->note = "down";
		} else if (g->a_primary >= 0 && !g->a_scroll) {
			g->a_scroll = true;
			g->a_acc_x = g->a_acc_y = 0;
			g->a_buttons = 0;
			g->note = "scroll start";
		}
	} else if (was && !down) {
		if (finger == g->a_primary) {
			g->a_primary = -1;
			if (g->a_buttons) {
				g->a_buttons = 0;
				up_now = true;
				g->note = "up";
			}
		}
		if (count == 0) {
			if (g->a_scroll) {
				g->note = "scroll end";
			}
			g->a_scroll = false;
			g->a_primary = -1;
		}
	} else if (g->a_scroll) {
		if (finger == g->a_primary) {
			int32_t sx, sy;
			g->a_acc_x += x - g->a_px;
			g->a_acc_y += y - g->a_py;
			g->a_px = x;
			g->a_py = y;
			if (scroll_lines(c, &g->a_acc_x, &g->a_acc_y, &sx, &sy)) {
				n = emit(out, n, max, SB_TG_SCROLL, sx, sy, 0);
				g->scroll_events++;
			}
		}
		return n;
	} else if (finger == g->a_primary) {
		g->a_px = x;
		g->a_py = y;
	}
	/* the pointer stays where the primary finger is (or was) */
	bool moves = finger == g->a_primary || up_now;
	if (moves || old != g->a_buttons) {
		int at = g->a_primary >= 0 ? g->a_primary : finger;
		uint32_t px = moves ? nx : g->a_x[at];
		uint32_t py = moves ? ny : g->a_y[at];
		n = emit(out, n, max, SB_TG_ABS, (int32_t)px, (int32_t)py, g->a_buttons);
		g->moves++;
	}
	return n;
}

int
sb_tg_contact(struct sb_tg *g, uint64_t now_ns, int finger, bool down, int x, int y,
    struct sb_tg_action *out, int max, bool *kick)
{
	*kick = false;
	g->note = 0;
	if (finger < 0 || finger >= SB_TG_FINGERS) {
		return 0;
	}
	x = clampi(x, 0, g->cfg.panel_w - 1);
	y = clampi(y, 0, g->cfg.panel_h - 1);
	if (g->cfg.mode == SB_TG_ABSOLUTE) {
		return absolute_contact(g, finger, down, x, y, out, max);
	}
	return direct_contact(g, now_ns, finger, down, x, y, out, max, kick);
}

int
sb_tg_tick(struct sb_tg *g, uint64_t now_ns, const struct sb_tg_seen *seen,
    struct sb_tg_action *out, int max, bool *again)
{
	const struct sb_tg_config *c = &g->cfg;
	int n = 0;
	bool clicked = false;
	*again = false;
	g->note = 0;
	if (c->mode != SB_TG_DIRECT) {
		return 0;
	}
	bool have = seen && seen->valid;
	uint32_t seq = have ? seen->seq : 0;
	int32_t sx = have ? seen->x : 0, sy = have ? seen->y : 0;

	if (g->cal == 0) {
		/* into the top-left corner: the pointer is then at 0,0 */
		n = emit(out, n, max, SB_TG_REL, -4000, -4000, g->buttons);
		g->cal = 1;
		g->cal_ticks = 0;
		g->sent_seq = seq;
		g->est_x = g->est_y = 0;
		g->est_valid = true;
	} else if (g->cal == 1) {
		if (seq != g->sent_seq && ++g->cal_ticks >= 3) {
			g->hot_x = -sx;
			g->hot_y = -sy;
			g->cal = 2;
			g->note = "pointer hotspot measured";
		} else if (g->cal_ticks++ > 30) {
			g->hot_x = g->hot_y = 0;
			g->cal = 2;
			g->note = "no pointer layer seen, steering by dead reckoning";
		}
	} else if (seq != g->sent_seq || g->sent_seq == 0 || ++g->stale >= 3) {
		/* one correction per drawn frame; without a redraw for 3 ticks, by dead reckoning */
		int32_t px, py;
		if (seq != g->sent_seq || !g->est_valid) {
			px = sx + g->hot_x;
			py = sy + g->hot_y;
		} else {
			px = g->est_x;
			py = g->est_y;
		}
		g->stale = 0;
		int32_t ex = g->tgt_x - px, ey = g->tgt_y - py;
		bool there = iabs(ex) <= 2 && iabs(ey) <= 2;
		if (!there && !g->two) {
			n = emit(out, n, max, SB_TG_REL, ex, ey, g->buttons);
			g->sent_seq = seq;
			g->est_x = clampi(g->tgt_x, 0, c->canvas_w - 1);
			g->est_y = clampi(g->tgt_y, 0, c->canvas_h - 1);
			g->est_valid = true;
		}
		if (g->click_button && (there || ++g->click_ticks > c->click_wait_ticks)) {
			n = emit(out, n, max, SB_TG_REL, 0, 0, g->click_button);
			n = emit(out, n, max, SB_TG_REL, 0, 0, g->buttons);
			g->click_button = 0;
			clicked = true;
			g->note = "click";
		}
	}
	/* touch, hold still, then move: drag */
	if (g->primary >= 0 && !g->two && !g->dragging && g->travel < (int)c->hold_travel &&
	    now_ns - g->down_at > (uint64_t)c->hold_ms * 1000000ULL) {
		g->dragging = true;
		g->buttons = 1;
		g->drags++;
		if (!clicked) {
			n = emit(out, n, max, SB_TG_REL, 0, 0, 1);
		}
		g->note = "drag start";
	}
	*again = g->primary >= 0 || g->click_button != 0 || g->cal == 1;
	g->active = *again;
	return n;
}

/* ---- iOS digitizer ---- */

void
sb_dig_init(struct sb_dig *d, const struct sb_dig_config *c)
{
	zero(d, sizeof(*d));
	d->cfg = *c;
	if (d->cfg.logical_w <= 0) {
		d->cfg.logical_w = d->cfg.panel_w;
	}
	if (d->cfg.logical_h <= 0) {
		d->cfg.logical_h = d->cfg.panel_h;
	}
	d->off_x = (d->cfg.logical_w - d->cfg.panel_w) / 2;
	d->off_y = (d->cfg.logical_h - d->cfg.panel_h) / 2;
}

bool
sb_dig_contact(struct sb_dig *d, int finger, bool down, int x, int y, struct sb_dig_frame *f)
{
	const struct sb_dig_config *c = &d->cfg;
	if (finger < 0 || finger >= SB_TG_FINGERS) {
		return false;
	}
	x = clampi(x, 0, c->panel_w - 1);
	y = clampi(y, 0, c->panel_h - 1);
	uint32_t nx = (uint32_t)clampi((int32_t)(((int64_t)(x + d->off_x) << 16) / c->logical_w), 0, 0xffff);
	uint32_t ny = (uint32_t)clampi((int32_t)(((int64_t)(y + d->off_y) << 16) / c->logical_h), 0, 0xffff);
	bool was = d->down[finger];
	bool moved = down && (nx != d->x[finger] || ny != d->y[finger]);
	if (was == down && !moved) {
		return false;
	}
	if (!was && down) {
		/* edges of what is shown: the panel's, or the bars' inner sides */
		int left = d->off_x < 0 ? -d->off_x : 0, right = d->off_x < 0 ? c->panel_w + d->off_x : c->panel_w;
		d->edge[finger] = x < left + SB_DIG_EDGE_PX || x > right - 1 - SB_DIG_EDGE_PX ||
		    y < SB_DIG_EDGE_PX || y > c->panel_h - 1 - SB_DIG_EDGE_PX;
	}
	zero(f, sizeof(*f));
	for (int i = 0; i < SB_TG_FINGERS; i++) {
		bool on = (i == finger) ? down : d->down[i];
		if (i != finger && !on) {
			continue;
		}
		uint32_t mask = d->edge[i] ? SB_DIG_FROM_EDGE_TIP : 0;
		if (i == finger) {
			if (was != down) {
				mask |= SB_DIG_TOUCH | SB_DIG_RANGE;
			}
			if (moved) {
				mask |= SB_DIG_POSITION;
			}
			if (!was && down) {
				mask |= SB_DIG_IDENTITY;
			}
		}
		struct sb_dig_contact *k = &f->c[f->count++];
		k->index = i;
		k->identity = (uint32_t)i + 2;
		k->touch = on;
		k->nx = (i == finger && down) ? nx : d->x[i];
		k->ny = (i == finger && down) ? ny : d->y[i];
		k->mask = mask;
		f->mask |= mask;
		f->any |= on;
	}
	d->down[finger] = down;
	if (down) {
		d->x[finger] = nx;
		d->y[finger] = ny;
	}
	f->nx = nx;
	f->ny = ny;
	f->finger = finger;
	f->changed_touch = was != down;
	d->frames++;
	return true;
}
