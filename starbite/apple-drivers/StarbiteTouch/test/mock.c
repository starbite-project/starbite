/* SPDX-License-Identifier: APSL-2.0 */
/* Host-test mocks (see mock.h). */
#include <string.h>
#include "mock.h"

static void
chip_write(struct fake_s6 *c, const uint8_t *b, unsigned n)
{
	c->writes++;
	if (n < 1) {
		return;
	}
	c->reg = b[0];
	if (b[0] == SB_S6_SENSE_ON) {
		c->sensing = true;
		c->sense_cmds++;
	} else if (b[0] == SB_S6_CLEAR_EVENT_STACK) {
		c->qhead = c->qtail = 0;
		c->never_empty_but_int = false;
		c->clear_cmds++;
	}
}

static void
chip_read(struct fake_s6 *c, uint8_t *b, unsigned n)
{
	c->reads++;
	memset(b, 0, n);
	switch (c->reg) {
	case SB_S6_BOOT_STATUS:
		b[0] = SB_S6_BOOT_APP;
		break;
	case SB_S6_DEVICE_ID:
		b[0] = 0xac; b[1] = 0x37; if (n > 2) b[2] = 0x61;
		break;
	case SB_S6_PANEL_INFO:
		b[0] = (uint8_t)(c->panel_max_x >> 8); b[1] = (uint8_t)c->panel_max_x;
		b[2] = (uint8_t)(c->panel_max_y >> 8); b[3] = (uint8_t)c->panel_max_y;
		if (n > 9) { b[8] = 16; b[9] = 32; }
		break;
	case SB_S6_READ_ONE_EVENT:
		if (c->qhead != c->qtail) {
			memcpy(b, c->queue[c->qhead % 64], n < 8 ? n : 8);
			c->qhead++;
		}
		break;
	default:
		break;
	}
}

void
fake_push_raw(struct fake_s6 *c, const uint8_t e[8])
{
	memcpy(c->queue[c->qtail % 64], e, 8);
	c->qtail++;
}

void
fake_push(struct fake_s6 *c, int tid, unsigned state, unsigned rx, unsigned ry)
{
	uint8_t e[8];
	sb_s6_encode(e, tid, state, rx, ry);
	fake_push_raw(c, e);
}

bool
fake_int_low(const struct fake_s6 *c)
{
	return c->qhead != c->qtail || c->never_empty_but_int;
}

#define R(m, off) ((m)->hsi2c[(off) / 4])

static void
hsi2c_begin(struct mock *m)
{
	uint32_t ac = R(m, SB_HSI2C_AUTO_CONF);
	m->starts++;
	m->rx = (ac & SB_HSI2C_AUTO_READ) != 0;
	m->len = ac & 0xffffu;
	m->moved = 0;
	m->rx_pos = 0;
	m->pending_rx = 0;
	m->addr = (uint8_t)(R(m, SB_HSI2C_ADDR) >> 10);
	struct fake_s6 *c = &m->chip;
	if (!c->present || m->addr != SB_S6_I2C_ADDR || c->nack_next) {
		if (c->nack_next) {
			c->nack_next--;
		}
		R(m, SB_HSI2C_INT_STAT) |= SB_HSI2C_INT_NOACK;
		m->running = false;
		return;
	}
	m->running = true;
	m->busy_polls = 3;
	R(m, SB_HSI2C_TRANS) |= SB_HSI2C_TRANS_BUSY;
	if (m->rx && !m->fault_no_done) {
		chip_read(c, m->rxbuf, m->len);
	}
}

/* The wire: a few bytes per FIFO_STAT poll. */
static uint32_t
fifo_stat(struct mock *m)
{
	uint32_t v = 0;
	if (!m->running || m->fault_no_done) {
		return m->rx ? SB_HSI2C_FIFO_RX_EMPTY : SB_HSI2C_FIFO_TX_FULL;
	}
	if (m->rx) {
		if (m->pending_rx == 0 && m->moved < m->len && m->wire_credit) {
			m->wire_credit--;
			unsigned k = m->len - m->moved;
			k = k < m->fifo_per_poll ? k : m->fifo_per_poll;
			m->pending_rx = k;
			m->moved += k;
		}
		if (m->pending_rx == 0) {
			v |= SB_HSI2C_FIFO_RX_EMPTY;
		}
		if (m->moved == m->len && m->pending_rx == 0) {
			R(m, SB_HSI2C_INT_STAT) |= SB_HSI2C_INT_DONE;
		}
	} else {
		if (m->pending_rx >= m->fifo_per_poll) {       /* reused as "bytes in the TX FIFO" */
			m->pending_rx = 0;                      /* the wire took them; full this once */
			v |= SB_HSI2C_FIFO_TX_FULL;
		}
	}
	return v;
}

static uint32_t
mock_rd(void *ctx, uintptr_t base, uint32_t off)
{
	struct mock *m = (struct mock *)ctx;
	switch (base) {
	case MOCK_HSI2C:
		if (off == SB_HSI2C_FIFO_STAT) {
			return fifo_stat(m);
		}
		if (off == SB_HSI2C_INT_STAT) {
			m->wire_credit = 1;     /* the wire moves between two polls of the state machine */
			return R(m, off);
		}
		if (off == SB_HSI2C_RX_DATA) {
			if (m->pending_rx) {
				m->pending_rx--;
			}
			return m->rx_pos < sizeof(m->rxbuf) ? m->rxbuf[m->rx_pos++] : 0;
		}
		if (off == SB_HSI2C_TRANS) {
			uint32_t v = R(m, off);
			if (m->fault_stuck_busy) {
				return v | SB_HSI2C_TRANS_BUSY;
			}
			if ((v & SB_HSI2C_TRANS_BUSY) && m->busy_polls && --m->busy_polls == 0) {
				R(m, off) &= ~SB_HSI2C_TRANS_BUSY;
			}
			return v;
		}
		return R(m, off);
	case MOCK_CMU: return m->cmu[off / 4];
	case MOCK_SYSREG: return m->sysreg[off / 4];
	case MOCK_GPIO: return m->gpio[off / 4];
	default: return 0xdeadbeef;
	}
}

static void
mock_wr(void *ctx, uintptr_t base, uint32_t off, uint32_t v)
{
	struct mock *m = (struct mock *)ctx;
	switch (base) {
	case MOCK_HSI2C:
		if (off == SB_HSI2C_INT_STAT) {
			R(m, off) &= ~v;
			return;
		}
		if (off == SB_HSI2C_CTL && (v & SB_HSI2C_CTL_SW_RST)) {
			m->sw_resets++;
			m->running = false;
			R(m, SB_HSI2C_TRANS) &= ~SB_HSI2C_TRANS_BUSY;
		}
		if (off == SB_HSI2C_TX_DATA) {
			if (m->running && m->moved < m->len) {
				m->txbuf[m->moved++] = (uint8_t)v;
				m->pending_rx++;
				if (m->moved == m->len && !m->fault_no_done) {
					chip_write(&m->chip, m->txbuf, m->len);
					R(m, SB_HSI2C_INT_STAT) |= SB_HSI2C_INT_DONE;
				}
			}
			return;
		}
		R(m, off) = v;
		if (off == SB_HSI2C_FS1) m->last_fs1 = v;
		if (off == SB_HSI2C_FS2) m->last_fs2 = v;
		if (off == SB_HSI2C_FS3) m->last_fs3 = v;
		if (off == SB_HSI2C_AUTO_CONF && (v & SB_HSI2C_AUTO_RUN)) {
			hsi2c_begin(m);
		}
		return;
	case MOCK_CMU: m->cmu[off / 4] = v; return;
	case MOCK_SYSREG: m->sysreg[off / 4] = v; return;
	case MOCK_GPIO: m->gpio[off / 4] = v; return;
	default: return;
	}
}

static void
mock_udelay(void *ctx, unsigned us)
{
	((struct mock *)ctx)->udelay_us += us;
}

void
mock_init(struct mock *m)
{
	memset(m, 0, sizeof(*m));
	m->chip.present = true;
	m->chip.panel_max_x = 4095;
	m->chip.panel_max_y = 4095;
	m->fifo_per_poll = 3;
}

void
mock_ops(struct mock *m, struct sb_mmio_ops *ops)
{
	ops->rd = mock_rd;
	ops->wr = mock_wr;
	ops->udelay = mock_udelay;
	ops->ctx = m;
}
