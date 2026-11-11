/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB DWC3 device controller (sbusb_dwc3.h).
 *
 * Register facts and the order that enumerates on this phone come from our loader's polling driver
 * (shim/starlte/usb_dwc3.c, read as documentation: that file is marked GPL-2.0-only) and from the Synopsys
 * programming model; an earlier port's dwc3.c (APSL-2.0) agrees on the core start. NOTES.md says per item
 * which is verified on the phone and which is inferred. Written new for this driver.
 *
 * Endpoint 0 on a DWC3: a SETUP TRB on physical endpoint 0; a data stage TRB (IN on 1, OUT on 0) started at
 * once; the status TRB only when the core reports XferNotReady(status). SET_ADDRESS goes into DCFG during the
 * setup stage (the core applies it after the status stage).
 */
#include "sbusb_dwc3.h"
#include "sbusb_regs.h"
#include "sbusb_ncm.h"

enum { E0_SETUP = 0, E0_DATA, E0_WAIT_STATUS, E0_STATUS };

#define QCAP            (sizeof(((struct sbusb_dwc3 *)0)->q) / sizeof(struct sbusb_ev))
#define E0_TIMEOUT_US   1000000u
#define STORM_RESETS    12u             /* bus resets without one SETUP before the link is restarted */
#define RESTART_MIN_MS  2000u
#define RESTART_MAX_MS  32000u

static const uint8_t ring_addr[SBUSB_RINGS] = { SBUSB_EP_NOTIFY, SBUSB_EP_IN, SBUSB_EP_OUT };

static uint32_t rd(struct sbusb_dwc3 *d, uint32_t off) { return d->io.rd(d->io.ctx, SBUSB_BLK_DWC3, off); }
static void wr(struct sbusb_dwc3 *d, uint32_t off, uint32_t v) { d->io.wr(d->io.ctx, SBUSB_BLK_DWC3, off, v); }
static void wait_us(struct sbusb_dwc3 *d, unsigned us) { d->io.delay_us(d->io.ctx, us); }

static void
sync(struct sbusb_dwc3 *d, uint32_t off, uint32_t len, int to_dev)
{
	if (d->io.sync != 0 && len != 0) {
		d->io.sync(d->io.ctx, off, len, to_dev);
	}
}

static void
dctl(struct sbusb_dwc3 *d, uint32_t v)
{
	wr(d, D3_DCTL, v & ~D3_DCTL_ULSTCHNGREQ_MASK);
}

static int
wait_for(struct sbusb_dwc3 *d, uint32_t off, uint32_t mask, uint32_t want, unsigned us)
{
	for (unsigned t = 0; t < us; t += 10) {
		if ((rd(d, off) & mask) == want) {
			return 1;
		}
		wait_us(d, 10);
	}
	return (rd(d, off) & mask) == want;
}

static void
push(struct sbusb_dwc3 *d, int type, unsigned ring, unsigned cookie, uint32_t value, uint32_t status)
{
	struct sbusb_ev *e;

	if (d->qn >= QCAP) {
		return;
	}
	e = &d->q[(d->qh + d->qn++) % QCAP];
	e->type = type;
	e->ring = ring;
	e->cookie = cookie;
	e->value = value;
	e->status = status;
}

static int
depcmd(struct sbusb_dwc3 *d, unsigned phys, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	wr(d, D3_DEPCMDPAR2(phys), p2);
	wr(d, D3_DEPCMDPAR1(phys), p1);
	wr(d, D3_DEPCMDPAR0(phys), p0);
	wr(d, D3_DEPCMD(phys), cmd | D3_CMD_ACT);
	for (unsigned i = 0; i < 5000; ++i) {
		uint32_t v = rd(d, D3_DEPCMD(phys));
		if ((v & D3_CMD_ACT) == 0) {
			unsigned status = D3_CMD_STATUS(v);
			if (status != 0) {
				++d->n.cmd_fail;
				d->n.last_cmd_fail = phys << 24 | (cmd & 0xffu) << 16 | (v & 0xffffu);
				return -(int)status;
			}
			return 0;
		}
		wait_us(d, 1);
	}
	++d->n.cmd_fail;
	d->n.last_cmd_fail = 0x80000000u | phys << 24 | (cmd & 0xffu) << 16;
	return -16;
}

static int
ep_config(struct sbusb_dwc3 *d, unsigned phys, unsigned type, unsigned mps, uint32_t action, unsigned interval)
{
	uint32_t p0 = D3_CFG0_TYPE(type) | D3_CFG0_MPS(mps) | action;
	uint32_t p1 = D3_CFG1_XFERCOMPLETE | D3_CFG1_EPNUM(phys);

	if ((phys & 1u) != 0) {
		p0 |= D3_CFG0_FIFO(phys >> 1);          /* TX FIFO number = endpoint number */
	}
	if (phys < 2) {
		p1 |= D3_CFG1_XFERNOTREADY;
	} else if ((d->flags & SBUSB_F_RING) != 0) {
		p1 |= D3_CFG1_XFERINPROGRESS;
	}
	if (interval != 0) {
		p1 |= D3_CFG1_INTERVAL_M1(interval - 1u);
	}
	return depcmd(d, phys, D3_CMD_SETEPCONFIG, p0, p1, 0);
}

static void
trb_write(struct sbusb_dwc3 *d, uint32_t off, uint64_t buf_pa, uint32_t size, uint32_t ctl)
{
	uint8_t *t = d->dma.va + off;

	sbusb_put32(t + 0, (uint32_t)buf_pa);
	sbusb_put32(t + 4, (uint32_t)(buf_pa >> 32));
	sbusb_put32(t + 8, size & D3_TRB_SIZE_MASK);
	sbusb_put32(t + 12, ctl);
	sync(d, off, D3_TRB_BYTES, 1);
}

static int
start_transfer(struct sbusb_dwc3 *d, unsigned phys, uint32_t trb_off, uint8_t *res)
{
	uint64_t pa = d->dma.pa + trb_off;
	int r = depcmd(d, phys, D3_CMD_STARTXFER, (uint32_t)(pa >> 32), (uint32_t)pa, 0);

	if (r == 0) {
		*res = (uint8_t)D3_CMD_RESIDX(rd(d, D3_DEPCMD(phys)));
	}
	return r;
}

static void
end_transfer(struct sbusb_dwc3 *d, unsigned phys, unsigned res)
{
	(void)depcmd(d, phys, D3_CMD_ENDXFER | D3_CMD_IOC | D3_CMD_FORCERM | D3_CMD_PARAM(res), 0, 0, 0);
	wait_us(d, 100);
}

/* ---- endpoint 0 ----------------------------------------------------------------------------- */

static int
e0_start(struct sbusb_dwc3 *d, unsigned phys, uint32_t buf, uint32_t len, uint32_t type)
{
	uint32_t ctl = type | D3_TRB_HWO | D3_TRB_LST | D3_TRB_IOC;
	uint32_t trb = SBUSB_DMA_TRB_EP0 + D3_TRB_BYTES * phys;
	int r;

	if (phys == 0) {
		ctl |= D3_TRB_ISP;
	} else {
		sync(d, buf, len, 1);
	}
	trb_write(d, trb, d->dma.pa + buf, len, ctl);
	r = start_transfer(d, phys, trb, &d->e0_res[phys]);
	if (r == 0) {
		d->e0_busy[phys] = 1;
		d->e0_len[phys] = len;
	}
	return r;
}

static void
e0_end(struct sbusb_dwc3 *d, unsigned phys)
{
	if (d->e0_busy[phys]) {
		end_transfer(d, phys, d->e0_res[phys]);
		d->e0_busy[phys] = 0;
	}
}

static void
e0_arm_setup(struct sbusb_dwc3 *d)
{
	d->e0 = E0_SETUP;
	d->e0_pending = 0;
	if (!d->e0_busy[0] && e0_start(d, 0, SBUSB_DMA_SETUP, 8, D3_TRB_TYPE_SETUP) != 0) {
		d->why = "ep0: the SETUP TRB was refused";
	}
}

static void
e0_stall(struct sbusb_dwc3 *d)
{
	e0_end(d, 0);
	e0_end(d, 1);
	(void)depcmd(d, 0, D3_CMD_SETSTALL, 0, 0, 0);
	++d->n.stalls;
	e0_arm_setup(d);
}

static void
e0_status(struct sbusb_dwc3 *d, unsigned phys)
{
	if (e0_start(d, phys, SBUSB_DMA_EP0BUF, 0, d->e0_three ? D3_TRB_TYPE_STATUS3 : D3_TRB_TYPE_STATUS2) != 0) {
		e0_stall(d);
		return;
	}
	d->e0 = E0_STATUS;
}

static void
e0_to_status(struct sbusb_dwc3 *d)
{
	d->e0 = E0_WAIT_STATUS;
	if (d->e0_pending != 0) {               /* its XferNotReady already came */
		unsigned phys = (unsigned)d->e0_pending - 1u;
		d->e0_pending = 0;
		e0_status(d, phys);
	}
}

/* ---- data rings ----------------------------------------------------------------------------- */

static uint32_t
ring_trb(const struct sbusb_ring *r, unsigned i)
{
	return r->off + D3_TRB_BYTES * i;
}

static void
ring_reset(struct sbusb_dwc3 *d, struct sbusb_ring *r)
{
	r->enq = r->deq = r->inflight = r->started = 0;
	for (unsigned i = 0; i < SBUSB_RING_TRBS * D3_TRB_BYTES; ++i) {
		d->dma.va[r->off + i] = 0;
	}
	if ((d->flags & SBUSB_F_RING) != 0) {
		/* the link TRB: back to TRB 0, owned by the controller for good */
		trb_write(d, ring_trb(r, SBUSB_RING_USABLE), d->dma.pa + r->off, 0, D3_TRB_TYPE_LINK | D3_TRB_HWO);
	}
	sync(d, r->off, SBUSB_RING_TRBS * D3_TRB_BYTES, 1);
}

static void
ring_end(struct sbusb_dwc3 *d, struct sbusb_ring *r)
{
	if (r->started) {
		end_transfer(d, r->phys, r->res);
	}
	ring_reset(d, r);
}

static void
ring_kick(struct sbusb_dwc3 *d, struct sbusb_ring *r)
{
	if (r->inflight == 0) {
		return;
	}
	if (!r->started) {
		if (start_transfer(d, r->phys, ring_trb(r, r->deq), &r->res) == 0) {
			r->started = 1;
		}
	} else if ((d->flags & SBUSB_F_RING) != 0) {
		(void)depcmd(d, r->phys, D3_CMD_UPDATEXFER | D3_CMD_PARAM(r->res), 0, 0, 0);
	}
}

static void
ring_complete_one(struct sbusb_dwc3 *d, struct sbusb_ring *r)
{
	uint32_t off = ring_trb(r, r->deq), size, left, len = r->m[r->deq].len, moved;

	size = sbusb_get32(d->dma.va + off + 8);
	left = size & D3_TRB_SIZE_MASK;
	moved = left <= len ? len - left : 0;
	if ((r->phys & 1u) == 0) {
		sync(d, r->m[r->deq].buf, moved, 0);
	}
	push(d, SBUSB_EV_DONE, (unsigned)(r - d->ring), r->m[r->deq].cookie, moved, D3_TRB_STATUS(size));
	if (++r->deq == SBUSB_RING_USABLE) {
		r->deq = 0;
		++r->wraps;
	}
	--r->inflight;
}

static void
ring_event(struct sbusb_dwc3 *d, struct sbusb_ring *r, unsigned type)
{
	if (!r->on || r->inflight == 0 || !r->started) {
		return;                                 /* ended by us: stale */
	}
	sync(d, r->off, SBUSB_RING_TRBS * D3_TRB_BYTES, 0);
	if ((d->flags & SBUSB_F_RING) == 0) {
		if (type != D3_EVT_XFERCOMPLETE) {
			return;
		}
		r->started = 0;
		ring_complete_one(d, r);
		ring_kick(d, r);                        /* the next TRB, if one waits */
		return;
	}
	if (type != D3_EVT_XFERINPROGRESS && type != D3_EVT_XFERCOMPLETE) {
		return;
	}
	while (r->inflight != 0 && (sbusb_get32(d->dma.va + ring_trb(r, r->deq) + 12) & D3_TRB_HWO) == 0) {
		ring_complete_one(d, r);
	}
	if (type == D3_EVT_XFERCOMPLETE) {              /* not expected without LST: start again */
		r->started = 0;
		ring_kick(d, r);
	}
}

int
sbusb_dwc3_queue(struct sbusb_dwc3 *d, unsigned ring, uint32_t buf, uint32_t len, unsigned cookie)
{
	struct sbusb_ring *r;
	uint32_t ctl = D3_TRB_TYPE_NORMAL | D3_TRB_HWO | D3_TRB_IOC;

	if (ring >= SBUSB_RINGS || !d->started) {
		return -1;
	}
	r = &d->ring[ring];
	if (!r->on || r->inflight >= SBUSB_RING_USABLE || (uint64_t)buf + len > d->dma.size) {
		return -1;
	}
	if ((r->phys & 1u) == 0) {
		ctl |= D3_TRB_ISP;                      /* OUT: an event on a short packet too */
		if ((d->flags & SBUSB_F_RING) != 0) {
			ctl |= D3_TRB_CSP;              /* and go on to the next TRB after one */
		}
	} else {
		sync(d, buf, len, 1);
	}
	if ((d->flags & SBUSB_F_RING) == 0) {
		ctl |= D3_TRB_LST;
	}
	r->m[r->enq].buf = buf;
	r->m[r->enq].len = len;
	r->m[r->enq].cookie = (uint16_t)cookie;
	trb_write(d, ring_trb(r, r->enq), d->dma.pa + buf, len, ctl);
	if (++r->enq == SBUSB_RING_USABLE) {
		r->enq = 0;
	}
	++r->inflight;
	ring_kick(d, r);
	return 0;
}

unsigned
sbusb_dwc3_ring_free(const struct sbusb_dwc3 *d, unsigned ring)
{
	if (ring >= SBUSB_RINGS || !d->ring[ring].on) {
		return 0;
	}
	return SBUSB_RING_USABLE - d->ring[ring].inflight;
}

/* Transfers of the rings from `first` on are ended; completions of theirs still waiting in the class queue
 * go with them, so the class layer never queues again on a ring that was just emptied. */
static void
rings_down(struct sbusb_dwc3 *d, unsigned first)
{
	unsigned kept = 0;

	for (unsigned i = first; i < SBUSB_RINGS; ++i) {
		if (d->ring[i].on) {
			ring_end(d, &d->ring[i]);
		}
	}
	for (unsigned i = 0; i < d->qn; ++i) {
		struct sbusb_ev e = d->q[(d->qh + i) % QCAP];

		if (e.type == SBUSB_EV_DONE && e.ring >= first) {
			continue;
		}
		d->q[(d->qh + kept++) % QCAP] = e;
	}
	d->qn = kept;
}

/* ---- configuration -------------------------------------------------------------------------- */

static void
configure(struct sbusb_dwc3 *d, unsigned cfg)
{
	uint32_t enabled = 3;

	rings_down(d, 0);
	for (unsigned i = 0; i < SBUSB_RINGS; ++i) {
		d->ring[i].on = 0;
	}
	wr(d, D3_DALEPENA, enabled);
	if (cfg == 1) {
		(void)depcmd(d, 0, D3_CMD_STARTCFG | D3_CMD_PARAM(2), 0, 0, 0);
		for (unsigned i = 0; i < SBUSB_RINGS; ++i) {
			struct sbusb_ring *r = &d->ring[i];
			int notify = i == SBUSB_RING_NOTIFY;
			unsigned mps = notify ? SBUSB_NOTIFY_MPS : sbusb_bulk_mps(d->st.full_speed);

			if (ep_config(d, r->phys, notify ? 3u : 2u, mps, D3_CFG0_ACTION_INIT, notify ? 9u : 0u) != 0 ||
			    depcmd(d, r->phys, D3_CMD_SETXFERRES, 1, 0, 0) != 0) {
				d->why = "a data endpoint was refused (DEPCMD)";
				continue;
			}
			ring_reset(d, r);
			r->on = 1;
			enabled |= 1u << r->phys;
		}
		wr(d, D3_DALEPENA, enabled);
		d->restart_wait_ms = RESTART_MIN_MS;    /* a host that configures us is a healthy link */
	}
	++d->n.set_configs;
	push(d, SBUSB_EV_CONFIGURED, 0, 0, cfg, 0);
}

static void
set_halt(struct sbusb_dwc3 *d, unsigned addr, int set)
{
	for (unsigned i = 0; i < SBUSB_RINGS; ++i) {
		struct sbusb_ring *r = &d->ring[i];

		if (ring_addr[i] != addr || !r->on) {
			continue;
		}
		ring_end(d, r);                         /* the class layer queues again on SBUSB_EV_REARM */
		for (unsigned k = 0, kept = 0, qn = d->qn; k < qn; ++k) {
			struct sbusb_ev e = d->q[(d->qh + k) % QCAP];

			if (e.type == SBUSB_EV_DONE && e.ring == i) {
				--d->qn;
				continue;
			}
			d->q[(d->qh + kept++) % QCAP] = e;
		}
		(void)depcmd(d, r->phys, set ? D3_CMD_SETSTALL : D3_CMD_CLEARSTALL, 0, 0, 0);
		if (!set) {
			push(d, SBUSB_EV_REARM, i, 0, 0, 0);
		}
	}
}

static void
e0_setup(struct sbusb_dwc3 *d)
{
	struct sbusb_ep0_res *res = &d->res;
	unsigned wlen;

	sync(d, SBUSB_DMA_SETUP, 8, 0);
	for (unsigned i = 0; i < 8; ++i) {
		d->setup[i] = d->dma.va[SBUSB_DMA_SETUP + i];
	}
	++d->n.setups;
	d->resets_no_setup = 0;
	d->e0_t0 = d->io.now_us(d->io.ctx);
	sbusb_ep0_setup(&d->st, d->setup, res);
	if (res->set_address >= 0) {
		wr(d, D3_DCFG, (rd(d, D3_DCFG) & ~D3_DCFG_ADDR_MASK) | D3_DCFG_ADDR(res->set_address));
	}
	if (res->set_config >= 0) {
		configure(d, (unsigned)res->set_config);
	}
	if (res->set_alt >= 0) {
		rings_down(d, SBUSB_RING_IN);           /* the bulk pair; the endpoints stay configured */
		push(d, SBUSB_EV_ALT, 0, 0, (uint32_t)res->set_alt, 0);
	}
	if (res->halt_ep >= 0) {
		set_halt(d, (unsigned)res->halt_ep, res->halt_set);
	}
	wlen = sbusb_get16(d->setup + 6);
	d->e0_pending = 0;
	switch (res->action) {
	case SBUSB_EP0_ACK:
		d->e0_three = 0;
		d->e0 = E0_WAIT_STATUS;
		break;
	case SBUSB_EP0_IN:
		for (unsigned i = 0; i < res->len; ++i) {
			d->dma.va[SBUSB_DMA_EP0BUF + i] = res->data[i];
		}
		d->e0_three = 1;
		d->e0_phys = 1;
		d->e0_zlp = res->len < wlen && res->len % SBUSB_EP0_MPS == 0;
		d->e0 = E0_DATA;
		if (e0_start(d, 1, SBUSB_DMA_EP0BUF, res->len, D3_TRB_TYPE_DATA) != 0) {
			e0_stall(d);
		}
		break;
	case SBUSB_EP0_OUT:
		d->e0_three = 1;
		d->e0_phys = 0;
		d->e0_zlp = 0;
		d->e0 = E0_DATA;
		/* an OUT TRB is a whole number of packets */
		if (e0_start(d, 0, SBUSB_DMA_EP0BUF, (res->len + SBUSB_EP0_MPS - 1u) / SBUSB_EP0_MPS * SBUSB_EP0_MPS,
		    D3_TRB_TYPE_DATA) != 0) {
			e0_stall(d);
		}
		break;
	default:
		e0_stall(d);
	}
}

static void
e0_event(struct sbusb_dwc3 *d, unsigned phys, unsigned type, unsigned status)
{
	if (type == D3_EVT_XFERCOMPLETE) {
		uint32_t size, left, moved;

		if (!d->e0_busy[phys]) {
			return;                         /* ended by us: stale */
		}
		sync(d, SBUSB_DMA_TRB_EP0, 2 * D3_TRB_BYTES, 0);
		size = sbusb_get32(d->dma.va + SBUSB_DMA_TRB_EP0 + D3_TRB_BYTES * phys + 8);
		left = size & D3_TRB_SIZE_MASK;
		moved = left <= d->e0_len[phys] ? d->e0_len[phys] - left : 0;
		d->e0_busy[phys] = 0;
		if (d->e0 == E0_SETUP) {
			if (phys == 0) {
				e0_setup(d);
			}
		} else if (d->e0 == E0_DATA && phys == (unsigned)d->e0_phys) {
			if (D3_TRB_STATUS(size) == D3_TRBSTS_SETUP_PENDING) {
				e0_arm_setup(d);        /* the host gave up and sent a new SETUP */
				return;
			}
			if (phys == 1 && d->e0_zlp) {
				d->e0_zlp = 0;
				if (e0_start(d, 1, SBUSB_DMA_EP0BUF, 0, D3_TRB_TYPE_DATA) != 0) {
					e0_stall(d);
				}
				return;
			}
			if (phys == 0) {
				if (moved > 64) {
					moved = 64;
				}
				sync(d, SBUSB_DMA_EP0BUF, moved, 0);
				if (!sbusb_ep0_out(&d->st, d->setup, d->dma.va + SBUSB_DMA_EP0BUF, moved)) {
					e0_stall(d);
					return;
				}
			}
			e0_to_status(d);
		} else if (d->e0 == E0_STATUS) {
			e0_arm_setup(d);
		}
	} else if (type == D3_EVT_XFERNOTREADY) {
		unsigned phase = status & 3u;

		if (phase == 2u) {                      /* the host is in the status stage */
			if (d->e0 == E0_WAIT_STATUS) {
				e0_status(d, phys);
			} else if (d->e0 == E0_DATA) {
				d->e0_pending = (int)phys + 1;
			}
		} else if (phase == 1u && d->e0 == E0_DATA && phys != (unsigned)d->e0_phys) {
			e0_stall(d);                    /* a data stage in the other direction */
		}
	}
}

/* ---- device events -------------------------------------------------------------------------- */

static void
on_reset(struct sbusb_dwc3 *d)
{
	++d->n.resets;
	++d->resets_no_setup;
	rings_down(d, 0);
	for (unsigned i = 0; i < SBUSB_RINGS; ++i) {
		d->ring[i].on = 0;
	}
	wr(d, D3_DALEPENA, 3);
	wr(d, D3_DCFG, rd(d, D3_DCFG) & ~D3_DCFG_ADDR_MASK);
	sbusb_state_reset(&d->st);
	d->suspended = 0;
	if (d->e0 != E0_SETUP) {
		e0_end(d, 0);
		e0_end(d, 1);
	}
	e0_arm_setup(d);
	push(d, SBUSB_EV_RESET, 0, 0, 0, 0);
}

static void
on_connect_done(struct sbusb_dwc3 *d)
{
	unsigned speed = rd(d, D3_DSTS) & D3_DSTS_SPEED_MASK;

	++d->n.conndone;
	d->st.full_speed = speed == 1u || speed == 3u;
	d->connected = 1;
	/* endpoint 0's max packet for this link: 64 at high and full speed */
	(void)ep_config(d, 0, 0, SBUSB_EP0_MPS, D3_CFG0_ACTION_MODIFY, 0);
	(void)ep_config(d, 1, 0, SBUSB_EP0_MPS, D3_CFG0_ACTION_MODIFY, 0);
	push(d, SBUSB_EV_CONNECT, 0, 0, (uint32_t)d->st.full_speed, 0);
}

static void
device_event(struct sbusb_dwc3 *d, uint32_t ev)
{
	switch (D3_EV_DEV_TYPE(ev)) {
	case 0:
		++d->n.disconnects;
		d->connected = 0;
		sbusb_state_reset(&d->st);
		push(d, SBUSB_EV_DISCONNECT, 0, 0, 0, 0);
		break;
	case 1:
		on_reset(d);
		break;
	case 2:
		on_connect_done(d);
		break;
	case 3: {
		unsigned link = D3_EV_DEV_INFO(ev) & 0xfu;
		if (link == D3_LINK_U3 && !d->suspended) {
			d->suspended = 1;
			++d->n.suspends;
			push(d, SBUSB_EV_SUSPEND, 0, 0, 0, 0);
		} else if (link == D3_LINK_U0 && d->suspended) {
			d->suspended = 0;
			push(d, SBUSB_EV_RESUME, 0, 0, 0, 0);
		}
		break;
	}
	default:
		++d->n.unknown_events;
	}
}

static void
hw_events(struct sbusb_dwc3 *d)
{
	uint32_t count = rd(d, D3_GEVNTCOUNT) & D3_GEVNTCOUNT_MASK;

	if (count == 0) {
		return;
	}
	sync(d, SBUSB_DMA_EVT, SBUSB_EVT_BYTES, 0);
	/* one hardware event makes at most a ring's worth of class events */
	while (count >= 4 && d->qn + SBUSB_RING_TRBS <= QCAP) {
		uint32_t ev = sbusb_get32(d->dma.va + SBUSB_DMA_EVT + d->evt_pos);

		d->evt_pos = (d->evt_pos + 4u) & (SBUSB_EVT_BYTES - 1u);
		count -= 4;
		wr(d, D3_GEVNTCOUNT, 4);
		++d->n.events;
		if (D3_EV_IS_DEV(ev)) {
			if (D3_EV_DEV_KIND(ev) == 0) {
				device_event(d, ev);
			}
		} else {
			unsigned phys = D3_EV_EP_PHYS(ev), type = D3_EV_EP_TYPE(ev);

			if (phys < 2) {
				e0_event(d, phys, type, D3_EV_EP_STATUS(ev));
			} else {
				for (unsigned i = 0; i < SBUSB_RINGS; ++i) {
					if (d->ring[i].phys == phys) {
						ring_event(d, &d->ring[i], type);
					}
				}
			}
		}
		if (!d->started) {
			return;
		}
	}
}

/* ---- start, stop, poll ---------------------------------------------------------------------- */

void
sbusb_dwc3_init(struct sbusb_dwc3 *d, const struct sbusb_io *io, const struct sbusb_dma *dma, const char *serial,
    unsigned flags)
{
	uint8_t *p = (uint8_t *)d;

	for (size_t i = 0; i < sizeof(*d); ++i) {
		p[i] = 0;
	}
	d->io = *io;
	d->dma = *dma;
	d->flags = flags;
	d->restart_wait_ms = RESTART_MIN_MS;
	sbusb_state_init(&d->st, serial);
	for (unsigned i = 0; i < SBUSB_RINGS; ++i) {
		d->ring[i].off = SBUSB_DMA_RING(i);
		d->ring[i].phys = (uint8_t)SBUSB_PHYS(ring_addr[i]);
	}
}

static void
halt_core(struct sbusb_dwc3 *d)
{
	dctl(d, rd(d, D3_DCTL) & ~D3_DCTL_RUN_STOP);
	/* the core halts once its event buffer is drained */
	for (unsigned i = 0; i < 1000 && (rd(d, D3_DSTS) & D3_DSTS_DEVCTRLHLT) == 0; ++i) {
		uint32_t c = rd(d, D3_GEVNTCOUNT) & D3_GEVNTCOUNT_MASK;
		if (c != 0) {
			wr(d, D3_GEVNTCOUNT, c);
		}
		wait_us(d, 100);
	}
	wr(d, D3_DEVTEN, 0);
	wr(d, D3_GEVNTSIZ, D3_GEVNTSIZ_INTMASK | (rd(d, D3_GEVNTSIZ) & 0xffffu));
}

static int
fail(struct sbusb_dwc3 *d, int stage, const char *why)
{
	d->why = why;
	return stage;
}

int
sbusb_dwc3_start(struct sbusb_dwc3 *d)
{
	uint32_t v, rev;
	uint64_t evt_pa = d->dma.pa + SBUSB_DMA_EVT;

	d->why = 0;
	d->want_up = 1;
	d->started = d->connected = d->suspended = 0;
	if (d->dma.va == 0 || d->dma.size < SBUSB_DMA_CLASS) {
		return fail(d, -6, "the DMA block is too small");
	}
	for (uint32_t i = 0; i < SBUSB_DMA_CLASS; ++i) {
		d->dma.va[i] = 0;
	}
	sync(d, 0, SBUSB_DMA_CLASS, 1);
	d->e0_busy[0] = d->e0_busy[1] = 0;
	d->qh = d->qn = 0;
	d->evt_pos = 0;
	d->resets_no_setup = 0;
	sbusb_state_reset(&d->st);
	for (unsigned i = 0; i < SBUSB_RINGS; ++i) {
		d->ring[i].on = d->ring[i].started = d->ring[i].inflight = d->ring[i].enq = d->ring[i].deq = 0;
	}

	/* Whatever ran before: the loader stops and resets the core before every OS (fbbg_off), S-Boot may
	 * not have. A core left running is halted before its PHY is reset under it. */
	if ((rd(d, D3_DCTL) & D3_DCTL_RUN_STOP) != 0) {
		halt_core(d);
	}
	if (sbusb_phy_start(&d->io, &d->phy) != 0) {
		return fail(d, -1, "USBCON did not take the PHY settings");
	}
	d->id = rd(d, D3_GSNPSID);
	if ((d->id >> 16) != D3_ID_USB3 && (d->id >> 16) != D3_ID_USB31) {
		return fail(d, -2, "no DWC3 behind the usb-drd nub (GSNPSID)");
	}
	/* core soft reset: completes only with the PHY's clock running */
	dctl(d, D3_DCTL_CSFTRST);
	if (!wait_for(d, D3_DCTL, D3_DCTL_CSFTRST, 0, 200000)) {
		return fail(d, -3, "DWC3 soft reset stuck (no PHY clock?)");
	}
	wait_us(d, 10000);

	/* USB 2.0 PHY interface: no PHY suspend, no LPM sleep signal, no free-running clock, PHY out of reset */
	wr(d, D3_GUSB2PHYCFG,
	    rd(d, D3_GUSB2PHYCFG) & ~(D3_U2_SUSPHY | D3_U2_FREECLK_EXISTS | D3_U2_ENBLSLPM | D3_U2_PHYSOFTRST));
	wr(d, D3_GUSB3PIPECTL, rd(d, D3_GUSB3PIPECTL) & ~D3_U3_SUSPHY);

	rev = d->id & 0xffffu;
	v = rd(d, D3_GCTL) & ~(D3_GCTL_SCALEDOWN_MASK | D3_GCTL_PRTCAP_MASK | D3_GCTL_DSBLCLKGTNG);
	if ((d->id >> 16) == D3_ID_USB3 && rev >= 0x210au && rev <= 0x250au) {
		v |= D3_GCTL_DSBLCLKGTNG;               /* clock gating is unreliable on 2.10a..2.50a */
	}
	wr(d, D3_GCTL, v | D3_GCTL_PRTCAP_DEVICE | D3_GCTL_SOFITPSYNC);
	/* the AXI master as Samsung's BSP sets it: INCR bursts up to 16, cache hints, burst request limit 3 */
	v = rd(d, D3_GSBUSCFG0) | D3_GSBUSCFG0_INCRBRST | D3_GSBUSCFG0_INCR16;
	if ((d->id >> 16) == D3_ID_USB31 || d->id >= 0x5533220au) {
		v |= D3_GSBUSCFG0_REQINFO_2;
	}
	wr(d, D3_GSBUSCFG0, v);
	wr(d, D3_GSBUSCFG1, (rd(d, D3_GSBUSCFG1) & ~D3_GSBUSCFG1_BREQLIMIT_MASK) | D3_GSBUSCFG1_BREQLIMIT_3);
	wr(d, D3_GUCTL, (rd(d, D3_GUCTL) & ~D3_GUCTL_REFCLKPER_MASK) | D3_GUCTL_REFCLKPER_0x14 | D3_GUCTL_HSTINAUTORETRY);
	if ((d->id >> 16) != D3_ID_USB3 || rev >= 0x250au) {
		v = rd(d, D3_GFLADJ);                   /* frame length adjustment 0x20 (the dts quirk) */
		if ((v & D3_GFLADJ_30MHZ_MASK) != 0x20u) {
			wr(d, D3_GFLADJ, (v & ~D3_GFLADJ_30MHZ_MASK) | D3_GFLADJ_30MHZ_SDBND_SEL | 0x20u);
		}
	}
	/* high speed, address 0, no LPM */
	wr(d, D3_DCFG, rd(d, D3_DCFG) & ~(D3_DCFG_SPEED_MASK | D3_DCFG_ADDR_MASK | D3_DCFG_LPM_CAP));

	/* one event buffer; its interrupt masked unless the caller takes the nub's interrupt */
	wr(d, D3_GEVNTADRLO, (uint32_t)evt_pa);
	wr(d, D3_GEVNTADRHI, (uint32_t)(evt_pa >> 32));
	wr(d, D3_GEVNTSIZ, ((d->flags & SBUSB_F_IRQ) != 0 ? 0 : D3_GEVNTSIZ_INTMASK) | SBUSB_EVT_BYTES);
	wr(d, D3_GEVNTCOUNT, rd(d, D3_GEVNTCOUNT) & D3_GEVNTCOUNT_MASK);
	wr(d, D3_DEVTEN, D3_DEVTEN_DISCONN | D3_DEVTEN_USBRST | D3_DEVTEN_CONNDONE | D3_DEVTEN_ULSTCNG);

	/* endpoint 0: transfer resources from 0, both directions at 512 until Connect Done says 64 */
	if (depcmd(d, 0, D3_CMD_STARTCFG | D3_CMD_PARAM(0), 0, 0, 0) != 0 ||
	    ep_config(d, 0, 0, 512, D3_CFG0_ACTION_INIT, 0) != 0 || depcmd(d, 0, D3_CMD_SETXFERRES, 1, 0, 0) != 0 ||
	    ep_config(d, 1, 0, 512, D3_CFG0_ACTION_INIT, 0) != 0 || depcmd(d, 1, D3_CMD_SETXFERRES, 1, 0, 0) != 0) {
		return fail(d, -4, "ep0 configuration refused (DEPCMD)");
	}
	wr(d, D3_DALEPENA, 3);
	d->started = 1;
	e0_arm_setup(d);
	if (!d->e0_busy[0]) {
		d->started = 0;
		return fail(d, -4, "ep0: the SETUP TRB was refused");
	}
	d->e0_t0 = d->io.now_us(d->io.ctx);
	dctl(d, rd(d, D3_DCTL) | D3_DCTL_RUN_STOP);     /* the pull-up: the host sees a device from here */
	if (!wait_for(d, D3_DSTS, D3_DSTS_DEVCTRLHLT, 0, 100000)) {
		d->started = 0;
		return fail(d, -5, "DWC3 did not leave halt (run/stop)");
	}
	return 0;
}

static void
core_stop(struct sbusb_dwc3 *d)
{
	if (!d->started) {
		return;
	}
	rings_down(d, 0);
	for (unsigned i = 0; i < SBUSB_RINGS; ++i) {
		d->ring[i].on = 0;
	}
	e0_end(d, 0);
	e0_end(d, 1);
	halt_core(d);
	wr(d, D3_DALEPENA, 0);
	dctl(d, D3_DCTL_CSFTRST);
	(void)wait_for(d, D3_DCTL, D3_DCTL_CSFTRST, 0, 100000);
	wr(d, D3_GEVNTADRLO, 0);
	wr(d, D3_GEVNTADRHI, 0);
	wr(d, D3_GEVNTSIZ, D3_GEVNTSIZ_INTMASK);
	wr(d, D3_GEVNTCOUNT, 0);
	d->started = d->connected = d->suspended = 0;
	sbusb_state_reset(&d->st);
}

void
sbusb_dwc3_stop(struct sbusb_dwc3 *d)
{
	d->want_up = 0;
	core_stop(d);
}

/* A replug cannot be seen by the controller (VBUS is forced valid in the PHY), and a link left as it was
 * has been seen to take the host's resets but never a SETUP again (the loader's and an earlier port's notes). So a
 * run of resets without one SETUP restarts the link from the PHY up, with a growing pause between tries. */
static void
link_watch(struct sbusb_dwc3 *d, uint64_t now)
{
	/* also: a restart that failed is tried again after the pause */
	if (!d->want_up || now < d->restart_after || (d->started && d->resets_no_setup < STORM_RESETS)) {
		return;
	}
	core_stop(d);
	++d->n.restarts;
	d->restart_after = now + (uint64_t)d->restart_wait_ms * 1000u;
	if (d->restart_wait_ms < RESTART_MAX_MS) {
		d->restart_wait_ms *= 2;
	}
	(void)sbusb_dwc3_start(d);                      /* a failure leaves it stopped, with d->why */
	d->want_up = 1;
	push(d, SBUSB_EV_RESTART, 0, 0, (uint32_t)d->started, 0);
}

int
sbusb_dwc3_poll(struct sbusb_dwc3 *d, struct sbusb_ev *ev)
{
	if (d->qn == 0 && (d->started || d->want_up)) {
		uint64_t now;

		if (d->started) {
			hw_events(d);
		}
		now = d->io.now_us(d->io.ctx);
		/* a control transfer the host abandoned, or a status stage we missed: start over */
		if (d->started && d->e0 != E0_SETUP && now - d->e0_t0 > E0_TIMEOUT_US) {
			++d->n.e0_timeouts;
			e0_stall(d);
		}
		link_watch(d, now);
	}
	if (d->qn == 0) {
		return 0;
	}
	*ev = d->q[d->qh];
	d->qh = (d->qh + 1) % QCAP;
	--d->qn;
	return 1;
}
