/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB NCM function (sbusb_func.h).
 */
#include "sbusb_func.h"

static void
rx_dg(void *ctx, const uint8_t *frame, uint32_t len)
{
	struct sbusb_func *f = (struct sbusb_func *)ctx;

	++f->n.rx_frames;
	if (f->ops.rx != 0) {
		f->ops.rx(f->ops.ctx, frame, len);
	}
}

static void
tx_reset(struct sbusb_func *f)
{
	for (unsigned i = 0; i < SBUSB_TX_SLOTS; ++i) {
		f->tx_state[i] = 0;
	}
	f->tx_open = -1;
	f->tx_inflight = 0;
}

static void
set_up(struct sbusb_func *f, int up)
{
	if (f->up == up) {
		return;
	}
	f->up = up;
	if (f->ops.link != 0) {
		f->ops.link(f->ops.ctx, up, f->d.st.full_speed);
	}
	if (up && f->tx_was_full && f->ops.tx_ready != 0) {
		f->tx_was_full = 0;
		f->ops.tx_ready(f->ops.ctx);
	}
}

static void
rx_arm(struct sbusb_func *f)
{
	for (unsigned i = 0; i < SBUSB_RX_SLOTS; ++i) {
		(void)sbusb_dwc3_queue(&f->d, SBUSB_RING_OUT, SBUSB_DMA_RX(i), SBUSB_NTB_MAX, i);
	}
}

/* speed first, then "connected": the order hosts expect (NCM 1.0 7.1) */
static void
notify(struct sbusb_func *f)
{
	uint8_t *va = f->d.dma.va;
	unsigned a = sbusb_ncm_notify_speed(va + SBUSB_DMA_NOTIFY(0), f->d.st.full_speed ? 12000000u : 480000000u);
	unsigned b = sbusb_ncm_notify_connection(va + SBUSB_DMA_NOTIFY(1), 1);

	if (sbusb_dwc3_queue(&f->d, SBUSB_RING_NOTIFY, SBUSB_DMA_NOTIFY(0), a, 0) == 0 &&
	    sbusb_dwc3_queue(&f->d, SBUSB_RING_NOTIFY, SBUSB_DMA_NOTIFY(1), b, 1) == 0) {
		f->n.notifies += 2;
	}
}

static void
tx_submit(struct sbusb_func *f)
{
	int slot = f->tx_open;
	uint32_t len;

	if (slot < 0 || f->w.ndg == 0) {
		return;
	}
	len = sbusb_ntb_end(&f->w, sbusb_bulk_mps(f->d.st.full_speed));
	f->tx_open = -1;
	if (sbusb_dwc3_queue(&f->d, SBUSB_RING_IN, SBUSB_DMA_TX((unsigned)slot), len, (unsigned)slot) != 0) {
		f->tx_state[slot] = 0;                  /* the link went away under us: the frames are lost */
		return;
	}
	f->tx_state[slot] = 2;
	++f->tx_inflight;
	++f->n.tx_ntb;
}

static int
tx_open_slot(struct sbusb_func *f)
{
	for (unsigned i = 0; i < SBUSB_TX_SLOTS; ++i) {
		if (f->tx_state[i] == 0) {
			f->tx_state[i] = 1;
			f->tx_open = (int)i;
			sbusb_ntb_begin(&f->w, f->d.dma.va + SBUSB_DMA_TX(i), f->d.st.ntb_in_max, f->seq++);
			return 1;
		}
	}
	return 0;
}

uint8_t *
sbusb_func_tx_begin(struct sbusb_func *f, uint32_t len, int *stall)
{
	uint8_t *p;

	*stall = 0;
	if (!f->up || len < SBUSB_DG_MIN || len > SBUSB_DG_MAX) {
		return 0;
	}
	if (f->tx_open >= 0) {
		p = sbusb_ntb_add(&f->w, len);
		if (p != 0) {
			return p;
		}
		tx_submit(f);                           /* full: send it and take another */
	}
	if (!tx_open_slot(f)) {
		*stall = 1;
		f->tx_was_full = 1;
		++f->n.tx_stalls;
		return 0;
	}
	return sbusb_ntb_add(&f->w, len);
}

void
sbusb_func_tx_commit(struct sbusb_func *f)
{
	++f->n.tx_frames;
	/* nothing on the wire: send now. Otherwise keep filling; the next completion sends it. */
	if (f->tx_inflight == 0) {
		tx_submit(f);
	}
}

static void
on_done(struct sbusb_func *f, const struct sbusb_ev *e)
{
	if (e->ring == SBUSB_RING_OUT && e->cookie < SBUSB_RX_SLOTS) {
		if (e->value != 0) {
			int r = sbusb_ntb_parse(f->d.dma.va + SBUSB_DMA_RX(e->cookie), e->value, rx_dg, f);
			++f->n.rx_ntb;
			if (r < 0) {
				++f->n.rx_bad;
				f->n.rx_last_error = (uint32_t)-r;
			}
		}
		if (f->alt1) {
			(void)sbusb_dwc3_queue(&f->d, SBUSB_RING_OUT, SBUSB_DMA_RX(e->cookie), SBUSB_NTB_MAX, e->cookie);
		}
	} else if (e->ring == SBUSB_RING_IN && e->cookie < SBUSB_TX_SLOTS && f->tx_state[e->cookie] == 2) {
		f->tx_state[e->cookie] = 0;
		--f->tx_inflight;
		tx_submit(f);
		if (f->tx_was_full && f->ops.tx_ready != 0) {
			f->tx_was_full = 0;
			f->ops.tx_ready(f->ops.ctx);
		}
	}
}

unsigned
sbusb_func_poll(struct sbusb_func *f)
{
	struct sbusb_ev e;
	unsigned n = 0;

	while (n < 64 && sbusb_dwc3_poll(&f->d, &e)) {
		++n;
		switch (e.type) {
		case SBUSB_EV_DONE:
			on_done(f, &e);
			break;
		case SBUSB_EV_ALT:
			tx_reset(f);
			f->alt1 = e.value == 1;
			if (f->alt1) {
				rx_arm(f);
				notify(f);
			}
			set_up(f, f->alt1);
			break;
		case SBUSB_EV_REARM:
			if (e.ring == SBUSB_RING_OUT && f->alt1) {
				rx_arm(f);
			} else if (e.ring == SBUSB_RING_IN) {
				tx_reset(f);
			}
			break;
		case SBUSB_EV_SUSPEND:
			set_up(f, 0);
			break;
		case SBUSB_EV_RESUME:
			set_up(f, f->alt1);
			break;
		case SBUSB_EV_RESET:
		case SBUSB_EV_DISCONNECT:
		case SBUSB_EV_CONFIGURED:
		case SBUSB_EV_RESTART:
			tx_reset(f);
			f->alt1 = 0;
			set_up(f, 0);
			break;
		default:
			break;
		}
	}
	return n;
}

void
sbusb_func_init(struct sbusb_func *f, const struct sbusb_io *io, const struct sbusb_dma *dma, const char *serial,
    unsigned flags, const struct sbusb_func_ops *ops)
{
	sbusb_dwc3_init(&f->d, io, dma, serial, flags);
	f->ops = *ops;
	f->up = f->alt1 = f->tx_was_full = 0;
	f->seq = 0;
	f->n = (struct sbusb_func_stats){ 0 };
	tx_reset(f);
}

int
sbusb_func_start(struct sbusb_func *f)
{
	if (f->d.dma.size < SBUSB_DMA_BYTES) {
		f->d.why = "the DMA block is too small";
		return -6;
	}
	return sbusb_dwc3_start(&f->d);
}

void
sbusb_func_stop(struct sbusb_func *f)
{
	sbusb_dwc3_stop(&f->d);
	tx_reset(f);
	f->alt1 = 0;
	set_up(f, 0);
}
