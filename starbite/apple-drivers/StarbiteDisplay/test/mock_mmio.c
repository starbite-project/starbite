/* SPDX-License-Identifier: APSL-2.0 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mock_mmio.h"
#include "../hw/sbd_regs.h"

static uint32_t *
R(struct mock *m, unsigned block, uint32_t off)
{
	if (block >= SBD_BLK_COUNT || off >= 0x10000 || (off & 3)) {
		fprintf(stderr, "mock: bad register access block %u offset %#x\n", block, off);
		abort();
	}
	return &m->reg[block][off / 4];
}

static uint32_t
mock_rd(void *ctx, unsigned block, uint32_t off)
{
	struct mock *m = ctx;

	if (block == SBD_BLK_DSIM && off == DSIM_INTSRC && m->tx_polls_left > 0) {
		if (--m->tx_polls_left == 0) {
			*R(m, block, DSIM_INTSRC) |= DSIM_INT_PH_EMPTY;
			*R(m, block, DSIM_FIFOCTRL) = (*R(m, block, DSIM_FIFOCTRL) & ~(0x3fu << 16)) | DSIM_FIFO_PH_EMPTY;
		}
	}
	if (block == SBD_BLK_DSIM && off == DSIM_RXFIFO) {
		uint32_t v = m->rx_pos < m->rx_len ? m->rx[m->rx_pos++] : 0;

		if (m->rx_pos >= m->rx_len) {
			*R(m, block, DSIM_FIFOCTRL) |= DSIM_FIFO_RX_EMPTY;
		}
		return v;
	}
	return *R(m, block, off);
}

static void
dsim_header(struct mock *m, uint32_t val)
{
	struct mock_packet *p;

	if (m->npkt >= MOCK_PACKETS) {
		fprintf(stderr, "mock: too many packets\n");
		abort();
	}
	p = &m->pkt[m->npkt++];
	memset(p, 0, sizeof(*p));
	p->type = (uint8_t)val;
	p->bta = (val & DSIM_PKTHDR_BTA) != 0;
	if (p->type == DSI_DCS_LONG) {
		p->len = (val >> 8) & 0xffff;
		if (p->len > m->payload_len || p->len > sizeof(p->data)) {
			fprintf(stderr, "mock: long packet of %u bytes but %u in the payload FIFO\n", p->len, m->payload_len);
			abort();
		}
		memcpy(p->data, m->payload, p->len);
	} else {
		p->len = 2;
		p->data[0] = (uint8_t)(val >> 8);
		p->data[1] = (uint8_t)(val >> 16);
	}
	m->payload_len = 0;
	/* the header is queued; it leaves after tx_delay_polls reads of INTSRC */
	*R(m, SBD_BLK_DSIM, DSIM_FIFOCTRL) = (*R(m, SBD_BLK_DSIM, DSIM_FIFOCTRL) & ~((0x3fu << 16) | DSIM_FIFO_PH_EMPTY)) | (1u << 16);
	if (m->fail_next_tx) {
		m->fail_next_tx = false;
		m->tx_polls_left = 600;         /* longer than one 50 ms wait (501 polls): one timeout, then it leaves */
	} else if (m->tx_delay_polls < 0) {
		m->tx_polls_left = 0;
	} else {
		m->tx_polls_left = m->tx_delay_polls + 1;
	}
	if (p->type == DSI_DCS_READ && m->answer_reads) {
		*R(m, SBD_BLK_DSIM, DSIM_INTSRC) |= DSIM_INT_RX_DONE;
		if (m->rx_len) {
			*R(m, SBD_BLK_DSIM, DSIM_FIFOCTRL) &= ~DSIM_FIFO_RX_EMPTY;
		}
	}
}

static void
mock_wr(void *ctx, unsigned block, uint32_t off, uint32_t val)
{
	struct mock *m = ctx;

	if (m->nlog >= MOCK_LOG) {
		fprintf(stderr, "mock: write log full\n");
		abort();
	}
	m->log[m->nlog++] = (struct mock_write){ block, off, val };
	if (block == SBD_BLK_DECON && (off == DECON_INT_PENDING || off == DECON_EXTRA_INT_PENDING)) {
		*R(m, block, off) &= ~val;              /* write 1 to clear */
		return;
	}
	if (block == SBD_BLK_DSIM) {
		switch (off) {
		case DSIM_INTSRC:
			*R(m, block, off) &= ~val;
			return;
		case DSIM_PAYLOAD:
			if (m->payload_len + 4 > sizeof(m->payload)) {
				fprintf(stderr, "mock: payload FIFO overflow\n");
				abort();
			}
			memcpy(m->payload + m->payload_len, &val, 4);
			m->payload_len += 4;
			return;
		case DSIM_PKTHDR:
			*R(m, block, off) = val;
			dsim_header(m, val);
			return;
		case DSIM_FIFOCTRL:
			fprintf(stderr, "mock: FIFOCTRL must never be written\n");
			abort();
		default:
			break;
		}
	}
	*R(m, block, off) = val;
}

static void
mock_udelay(void *ctx, uint32_t us)
{
	struct mock *m = ctx;

	m->slept_us += us;
	if (m->auto_frames) {
		m->frame_clock_us += us;
		while (m->frame_clock_us >= 16667) {
			m->frame_clock_us -= 16667;
			mock_frame(m);
		}
	}
}

void
mock_init(struct mock *m)
{
	memset(m, 0, sizeof(*m));
	m->answer_reads = true;
}

struct sbd_io
mock_io(struct mock *m)
{
	struct sbd_io io = { mock_rd, mock_wr, mock_udelay, m };

	return io;
}

void
mock_boot_state(struct mock *m)
{
	uint32_t g0 = IDMA_CH(SBD_CH_G0);

	*R(m, SBD_BLK_DECON, DECON_GLOBAL_CONTROL) = DECON_GC_CMD_MODE | DECON_GC_RUN | 3;
	*R(m, SBD_BLK_DECON, DECON_TRIG_CONTROL) = 0x1281;              /* our loader's value (shim fbtext.c) */
	*R(m, SBD_BLK_DECON, DECON_DATA_PATH_0) = 1u << (4 * 5);        /* window 5 on (dts default_win) */
	*R(m, SBD_BLK_DECON, DECON_DATA_PATH_1) = 5u << (4 * 5);        /* channel number 5 = G0 */
	*R(m, SBD_BLK_DECON, DECON_WIN_START(5)) = 0;
	*R(m, SBD_BLK_DECON, DECON_WIN_END(5)) = (2959u << 16) | 1439u;
	*R(m, SBD_BLK_IDMA, g0 + IDMA_IN_CON) = (0x10u << 19) | (IDMA_FMT_ARGB8888 << IDMA_IN_FORMAT_SHIFT);
	*R(m, SBD_BLK_IDMA, g0 + IDMA_SRC_SIZE) = (2960u << 16) | 1440u;
	*R(m, SBD_BLK_IDMA, g0 + IDMA_IMG_SIZE) = (2960u << 16) | 1440u;
	*R(m, SBD_BLK_IDMA, g0 + IDMA_BASE_Y) = 0xcc000000u;
	*R(m, SBD_BLK_IDMA, g0 + IDMA_BASE_C) = 0xcc000000u;
	*R(m, SBD_BLK_IDMA, g0 + IDMA_OUT_CON) = 0x11;
	*R(m, SBD_BLK_IDMA, g0 + IDMA_DEADLOCK_NUM) = 0x7fffffff;
	*R(m, SBD_BLK_IDMA, g0 + IDMA_BUS_CON) = 0x22;
	*R(m, SBD_BLK_IDMA, g0 + IDMA_DYN_GATING) = 0x33;
	*R(m, SBD_BLK_IDMA, g0 + IDMA_REQ_DEST) = 0x44;
	*R(m, SBD_BLK_DPP, DPP_CH(SBD_CH_G0) + DPP_IN_CON) = 1u << 3;
	*R(m, SBD_BLK_DPP, DPP_CH(SBD_CH_G0) + DPP_DYN_GATING) = 0x55;
	m->active_base[SBD_CH_G0] = 0xcc000000u;
	m->active_path1 = 5u << (4 * 5);
	m->active_win_end = (2959u << 16) | 1439u;
	/* DSIM: HS clock requested, command mode, FIFOs idle (S-Boot's dumped FIFOCTRL 0x00011007 minus the queued header) */
	*R(m, SBD_BLK_DSIM, DSIM_CLK_CTRL) = DSIM_TX_REQUEST_HSCLK;
	*R(m, SBD_BLK_DSIM, DSIM_CONFIG) = 1u << 7;
	*R(m, SBD_BLK_DSIM, DSIM_FIFOCTRL) = DSIM_FIFO_RX_EMPTY | DSIM_FIFO_PH_EMPTY | (1u << 8) | 7;
	*R(m, SBD_BLK_DSIM, DSIM_PKTHDR) = 0x06000000;                  /* upper bits the driver must keep */
}

void
mock_frame(struct mock *m)
{
	uint32_t trig = *R(m, SBD_BLK_DECON, DECON_TRIG_CONTROL), en = *R(m, SBD_BLK_DECON, DECON_INT_ENABLE);

	if (!(trig & DECON_HW_TRIG_EN) || (trig & DECON_HW_TRIG_MASK)) {
		return;                 /* no TE-triggered frame: nothing is taken, nothing counts */
	}
	m->frames++;
	*R(m, SBD_BLK_DECON, DECON_FRAME_COUNT) += 1;
	if (*R(m, SBD_BLK_DECON, DECON_SHADOW_REQ) & DECON_SHADOW_ALL) {
		for (unsigned t = 0; t < 6; t++) {
			m->active_base[t] = *R(m, SBD_BLK_IDMA, IDMA_CH(t) + IDMA_BASE_Y);
		}
		m->active_path1 = *R(m, SBD_BLK_DECON, DECON_DATA_PATH_1);
		for (unsigned w = 0; w < 6; w++) {
			if (*R(m, SBD_BLK_DECON, DECON_DATA_PATH_0) & (1u << (4 * w))) {
				m->active_win_start = *R(m, SBD_BLK_DECON, DECON_WIN_START(w));
				m->active_win_end = *R(m, SBD_BLK_DECON, DECON_WIN_END(w));
			}
		}
		*R(m, SBD_BLK_DECON, DECON_SHADOW_REQ) = 0;
	}
	if (en & DECON_INT_EN) {
		*R(m, SBD_BLK_DECON, DECON_INT_PENDING) |= en & (DECON_INT_FRAME_START | DECON_INT_FRAME_DONE);
	}
}

void
mock_log_reset(struct mock *m)
{
	m->nlog = 0;
}

int
mock_find(const struct mock *m, unsigned from, unsigned block, uint32_t off)
{
	for (unsigned i = from; i < m->nlog; i++) {
		if (m->log[i].block == block && m->log[i].off == off) {
			return (int)i;
		}
	}
	return -1;
}

void
mock_queue_rx(struct mock *m, const uint32_t *words, unsigned n)
{
	memcpy(m->rx, words, n * sizeof(uint32_t));
	m->rx_len = n;
	m->rx_pos = 0;
}
