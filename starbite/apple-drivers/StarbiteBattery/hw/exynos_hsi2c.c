/* SPDX-License-Identifier: APSL-2.0 */
/* Polled read-only Exynos USI HSI2C master (see exynos_hsi2c.h). */
#include "exynos_hsi2c.h"

static uint32_t
R(struct hsi2c *h, uint32_t off)
{
	return h->io->rd(h->io->ctx, off);
}

static void
W(struct hsi2c *h, uint32_t off, uint32_t v)
{
	h->io->wr(h->io->ctx, off, v);
}

void
hsi2c_init(struct hsi2c *h, const struct hsi2c_io *io)
{
	h->io = io;
	h->errors = h->resets = h->fallbacks = 0;
	if (!h->saved) {
		h->fs1 = R(h, HSI2C_TIMING_FS1);
		h->fs2 = R(h, HSI2C_TIMING_FS2);
		h->fs3 = R(h, HSI2C_TIMING_FS3);
		if (((h->fs3 >> 16) & 0xffu) == 0) {
			/* nothing from S-Boot: 400 kHz from a 200 MHz IPCLK, as the touch bus (exynos9810-woa) */
			h->fs3 = (h->fs3 & ~0x00ff0000u) | (0x21u << 16);
			h->fs2 = (h->fs2 & ~0x000000ffu) | 0xe0u;
			h->fs1 = (h->fs1 & ~0x00ff0000u) | (0xf0u << 16);
		}
		h->saved = true;
	}
	hsi2c_reset(h);
}

void
hsi2c_reset(struct hsi2c *h)
{
	h->resets++;
	W(h, HSI2C_INT_EN, 0);
	W(h, HSI2C_USI_CON, 0);                 /* USI reset released */
	W(h, HSI2C_CTL, R(h, HSI2C_CTL) | 0x80000000u);
	W(h, HSI2C_CTL, R(h, HSI2C_CTL) & ~0x80000000u);
	W(h, HSI2C_TIMING_FS3, h->fs3);
	W(h, HSI2C_TIMING_FS2, h->fs2);
	W(h, HSI2C_TIMING_FS1, h->fs1);
	W(h, HSI2C_CTL, 0x8u);                  /* master */
	W(h, HSI2C_TRAILING_CTL, 0x00ffffffu);
	W(h, HSI2C_TIMEOUT, R(h, HSI2C_TIMEOUT) & ~0x80000000u);
	W(h, HSI2C_CONF, R(h, HSI2C_CONF) | 0x80000000u);      /* AUTO mode */
	W(h, HSI2C_INT_STAT, R(h, HSI2C_INT_STAT) & 0xfffu);
}

int
hsi2c_msg(struct hsi2c *h, uint8_t addr7, bool receive, uint8_t *buf, unsigned count, bool stop)
{
	uint32_t last = 0;
	unsigned done = 0, tx_idle = 0;
	bool xfer_done = false;

	if (count == 0 || count > 32 || addr7 > 0x7fu) {
		return -1;
	}
	unsigned trig = count < 8 ? count : 8;
	W(h, HSI2C_FIFO_CTL, 3u | (trig << 4) | (trig << 16));
	W(h, HSI2C_ADDR, (uint32_t)addr7 << 10);
	W(h, HSI2C_CTL, 0x8u | (receive ? 0x40u : 0x80u));
	W(h, HSI2C_INT_EN, HSI2C_INT_XFER_DONE);
	W(h, HSI2C_AUTO_CONF, count | (receive ? 0x10000u : 0) | (stop ? 0x20000u : 0));
	W(h, HSI2C_AUTO_CONF, R(h, HSI2C_AUTO_CONF) | 0x80000000u);
	for (unsigned us = 0; us < HSI2C_POLLS; us++) {
		uint32_t irq = R(h, HSI2C_INT_STAT);
		if (irq) {
			last |= irq;
			W(h, HSI2C_INT_STAT, irq);
		}
		if (irq & HSI2C_INT_ERRORS) {
			break;
		}
		if (irq & HSI2C_INT_XFER_DONE) {
			xfer_done = true;
		}
		if (receive) {
			while (done < count && !(R(h, HSI2C_FIFO_STAT) & HSI2C_FIFO_RX_EMPTY)) {
				buf[done++] = (uint8_t)R(h, HSI2C_RX_DATA);
			}
		} else {
			while (done < count && !(R(h, HSI2C_FIFO_STAT) & HSI2C_FIFO_TX_FULL)) {
				W(h, HSI2C_TX_DATA, buf[done++]);
			}
			if (!stop && done == count && (R(h, HSI2C_FIFO_STAT) & HSI2C_FIFO_TX_EMPTY)) {
				tx_idle++;
			}
		}
		if (done == count && (xfer_done || (receive && stop) || tx_idle > HSI2C_TX_IDLE_POLLS)) {
			W(h, HSI2C_INT_EN, 0);
			if (!stop) {
				return 0;
			}
			for (unsigned n = 0; n < 10000; n++) {
				if (!(R(h, HSI2C_TRANS_STATUS) & (1u << 17))) {
					return 0;
				}
				h->io->delay_us(h->io->ctx, 1);
			}
			break;
		}
		h->io->delay_us(h->io->ctx, 1);
	}
	h->errors++;
	h->last_int = last;
	h->last_fifo = R(h, HSI2C_FIFO_STAT);
	h->last_trans = R(h, HSI2C_TRANS_STATUS);
	W(h, HSI2C_INT_EN, 0);
	hsi2c_reset(h);
	return -1;
}

int
hsi2c_read_reg(struct hsi2c *h, uint8_t addr7, uint8_t reg, uint8_t *buf, unsigned len)
{
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		if (hsi2c_msg(h, addr7, false, &reg, 1, false) == 0 && hsi2c_msg(h, addr7, true, buf, len, true) == 0) {
			return 0;
		}
		hsi2c_reset(h);
		h->fallbacks++;
		if (hsi2c_msg(h, addr7, false, &reg, 1, true) == 0 && hsi2c_msg(h, addr7, true, buf, len, true) == 0) {
			return 0;
		}
		hsi2c_reset(h);
		h->io->delay_us(h->io->ctx, 300);
	}
	return -1;
}

int
hsi2c_write_reg(struct hsi2c *h, uint8_t addr7, uint8_t reg, uint8_t value)
{
	uint8_t m[2] = { reg, value };
	if (addr7 != HSI2C_WRITE_ADDR || reg != HSI2C_WRITE_REG) {
		return -1;
	}
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		if (hsi2c_msg(h, addr7, false, m, 2, true) == 0) {
			return 0;
		}
		hsi2c_reset(h);
	}
	return -1;
}
