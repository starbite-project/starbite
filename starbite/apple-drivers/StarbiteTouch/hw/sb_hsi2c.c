/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Exynos 9810 USI HSI2C master (see sb_hsi2c.h). The transfer is an explicit state machine so the same code
 * runs polled (sb_hsi2c_xfer, today) or from the bus interrupt (StarbiteHSI2C later): RUN moves bytes between
 * the caller's buffer and the 64-byte FIFOs and watches INT_STAT; WAIT_IDLE waits for MASTER_BUSY to drop.
 */
#include "sb_hsi2c.h"

#define RD(b, off)      ((b)->ops->rd((b)->ops->ctx, (b)->regs, (off)))
#define WR(b, off, v)   ((b)->ops->wr((b)->ops->ctx, (b)->regs, (off), (v)))

static const uint32_t usi03_gates[] = { 0x2018u, 0x206cu, 0x20dcu, 0x20e0u };

const struct sb_usi_i2c_platform sb_usi03_touch_bus = {
	.cmu = 0, .sysreg = 0, .gpio = 0,       /* filled from the ADT by the caller */
	.gates = usi03_gates,
	.ngates = sizeof(usi03_gates) / sizeof(usi03_gates[0]),
	.qch = 0x3038u,
	.usi_off = 0x1cu, .usi_mode = 4u,
	.pin_bank = 0x20u,                      /* GPP1 */
	.scl_pin = 4, .sda_pin = 5, .pin_func = 2,
};

void
sb_hsi2c_setup(struct sb_hsi2c *b, const struct sb_mmio_ops *ops, uintptr_t regs, const struct sb_hsi2c_timing *t)
{
	static const struct sb_hsi2c_timing def = SB_HSI2C_TIMING_400K;
	unsigned char *p = (unsigned char *)b;
	for (unsigned i = 0; i < sizeof(*b); i++) {
		p[i] = 0;
	}
	b->ops = ops;
	b->regs = regs;
	b->timing = t ? *t : def;
	b->state = SB_HSI2C_IDLE;
}

void
sb_hsi2c_reset(struct sb_hsi2c *b)
{
	WR(b, SB_HSI2C_INT_EN, 0);
	WR(b, SB_HSI2C_USI_CON, 0);                             /* USI_RESET is active high */
	WR(b, SB_HSI2C_CTL, RD(b, SB_HSI2C_CTL) | SB_HSI2C_CTL_SW_RST);
	WR(b, SB_HSI2C_CTL, RD(b, SB_HSI2C_CTL) & ~SB_HSI2C_CTL_SW_RST);
	WR(b, SB_HSI2C_FS3, (RD(b, SB_HSI2C_FS3) & ~0x00ff0000u) | ((uint32_t)b->timing.fs3_div << 16));
	WR(b, SB_HSI2C_FS2, (RD(b, SB_HSI2C_FS2) & ~0x000000ffu) | b->timing.fs2_mask);
	WR(b, SB_HSI2C_FS1, (RD(b, SB_HSI2C_FS1) & ~0x00ff0000u) | ((uint32_t)b->timing.fs1_hold << 16));
	WR(b, SB_HSI2C_CTL, SB_HSI2C_CTL_MASTER);
	WR(b, SB_HSI2C_TRAILING, 0x00ffffffu);
	WR(b, SB_HSI2C_TIMEOUT, RD(b, SB_HSI2C_TIMEOUT) & ~SB_HSI2C_TIMEOUT_EN);
	WR(b, SB_HSI2C_CONF, RD(b, SB_HSI2C_CONF) | SB_HSI2C_CONF_AUTO);
	WR(b, SB_HSI2C_INT_STAT, RD(b, SB_HSI2C_INT_STAT) & 0xfffu);     /* write-1-to-clear */
}

int
sb_hsi2c_start(struct sb_hsi2c *b, uint8_t addr, bool rx, uint8_t *buf, unsigned len)
{
	if (len == 0 || len > SB_HSI2C_MAX_MSG || buf == 0) {
		b->state = SB_HSI2C_ERROR;
		b->err = SB_I2C_EINVAL;
		return SB_I2C_EINVAL;
	}
	sb_hsi2c_reset(b);
	b->addr = addr;
	b->rx = rx;
	b->buf = buf;
	b->len = len;
	b->done = 0;
	b->steps = 0;
	b->irq_seen = 0;
	b->xfer_done = false;
	b->err = SB_I2C_OK;
	unsigned trig = len < 8 ? len : 8;
	WR(b, SB_HSI2C_FIFO_CTL, 3u | (trig << 4) | (trig << 16));     /* RX/TX FIFO on, trigger levels */
	WR(b, SB_HSI2C_ADDR, (uint32_t)addr << 10);
	WR(b, SB_HSI2C_CTL, SB_HSI2C_CTL_MASTER | (rx ? SB_HSI2C_CTL_RXCHON : SB_HSI2C_CTL_TXCHON));
	WR(b, SB_HSI2C_INT_EN, SB_HSI2C_INT_DONE);                      /* polled: TRANSFER_DONE only */
	WR(b, SB_HSI2C_AUTO_CONF, len | (rx ? SB_HSI2C_AUTO_READ : 0) | SB_HSI2C_AUTO_STOP);
	WR(b, SB_HSI2C_AUTO_CONF, RD(b, SB_HSI2C_AUTO_CONF) | SB_HSI2C_AUTO_RUN);
	b->state = SB_HSI2C_RUN;
	return SB_I2C_OK;
}

static int
irq_error(uint32_t irq)
{
	if (irq & SB_HSI2C_INT_NOACK) {
		return SB_I2C_ENOACK;
	}
	if (irq & SB_HSI2C_INT_NODEV) {
		return SB_I2C_ENODEV;
	}
	if (irq & SB_HSI2C_INT_TIMEOUT) {
		return SB_I2C_ETIMEOUT;
	}
	return SB_I2C_EABORT;
}

static enum sb_hsi2c_state
fail(struct sb_hsi2c *b, int err)
{
	b->err = err;
	b->failures++;
	b->fail_int = b->irq_seen;
	b->fail_trans = RD(b, SB_HSI2C_TRANS);
	b->fail_fifo = RD(b, SB_HSI2C_FIFO_STAT);
	WR(b, SB_HSI2C_INT_EN, 0);
	sb_hsi2c_reset(b);
	b->state = SB_HSI2C_ERROR;
	return b->state;
}

enum sb_hsi2c_state
sb_hsi2c_step(struct sb_hsi2c *b)
{
	switch (b->state) {
	case SB_HSI2C_RUN: {
		uint32_t irq = RD(b, SB_HSI2C_INT_STAT);
		if (irq) {
			b->irq_seen |= irq;
			WR(b, SB_HSI2C_INT_STAT, irq);
		}
		if (irq & SB_HSI2C_INT_ERRORS) {
			return fail(b, irq_error(irq));
		}
		if (irq & SB_HSI2C_INT_DONE) {
			b->xfer_done = true;
		}
		if (b->rx) {
			while (b->done < b->len && !(RD(b, SB_HSI2C_FIFO_STAT) & SB_HSI2C_FIFO_RX_EMPTY)) {
				b->buf[b->done++] = (uint8_t)RD(b, SB_HSI2C_RX_DATA);
			}
		} else {
			while (b->done < b->len && !(RD(b, SB_HSI2C_FIFO_STAT) & SB_HSI2C_FIFO_TX_FULL)) {
				WR(b, SB_HSI2C_TX_DATA, b->buf[b->done++]);
			}
		}
		/* A read is complete when every byte arrived; a write when the controller says so. */
		if (b->done == b->len && (b->xfer_done || b->rx)) {
			WR(b, SB_HSI2C_INT_EN, 0);
			b->steps = 0;
			b->state = SB_HSI2C_WAIT_IDLE;
		} else if (++b->steps >= SB_HSI2C_RUN_STEPS) {
			return fail(b, SB_I2C_ETIMEOUT);
		}
		return b->state;
	}
	case SB_HSI2C_WAIT_IDLE:
		if (!(RD(b, SB_HSI2C_TRANS) & SB_HSI2C_TRANS_BUSY)) {
			b->xfers++;
			b->state = SB_HSI2C_DONE;
		} else if (++b->steps >= SB_HSI2C_IDLE_STEPS) {
			return fail(b, SB_I2C_EBUSY);
		}
		return b->state;
	default:
		return b->state;
	}
}

int
sb_hsi2c_xfer(struct sb_hsi2c *b, uint8_t addr, bool rx, uint8_t *buf, unsigned len)
{
	int r = sb_hsi2c_start(b, addr, rx, buf, len);
	if (r != SB_I2C_OK) {
		return r;
	}
	for (;;) {
		enum sb_hsi2c_state s = sb_hsi2c_step(b);
		if (s == SB_HSI2C_DONE) {
			b->state = SB_HSI2C_IDLE;
			return SB_I2C_OK;
		}
		if (s == SB_HSI2C_ERROR) {
			return b->err;
		}
		b->ops->udelay(b->ops->ctx, 1);
	}
}

const char *
sb_i2c_strerror(int err)
{
	switch (err) {
	case SB_I2C_OK: return "ok";
	case SB_I2C_EINVAL: return "bad length";
	case SB_I2C_ENOACK: return "no ack";
	case SB_I2C_ENODEV: return "no device";
	case SB_I2C_ETIMEOUT: return "timeout";
	case SB_I2C_EABORT: return "aborted";
	case SB_I2C_EBUSY: return "bus stays busy";
	default: return "?";
	}
}

void
sb_hsi2c_platform_init(const struct sb_mmio_ops *ops, const struct sb_usi_i2c_platform *p)
{
	for (unsigned i = 0; i < p->ngates; i++) {
		ops->wr(ops->ctx, p->cmu, p->gates[i], ops->rd(ops->ctx, p->cmu, p->gates[i]) | 0x00300000u);
	}
	ops->wr(ops->ctx, p->cmu, p->qch, ops->rd(ops->ctx, p->cmu, p->qch) | 0x3u);
	ops->wr(ops->ctx, p->sysreg, p->usi_off, p->usi_mode);
	uint32_t mask = (0xfu << (p->scl_pin * 4)) | (0xfu << (p->sda_pin * 4));
	uint32_t func = ((uint32_t)p->pin_func << (p->scl_pin * 4)) | ((uint32_t)p->pin_func << (p->sda_pin * 4));
	uint32_t con = p->pin_bank;
	ops->wr(ops->ctx, p->gpio, con, (ops->rd(ops->ctx, p->gpio, con) & ~mask) | func);
	ops->wr(ops->ctx, p->gpio, con + 8, ops->rd(ops->ctx, p->gpio, con + 8) & ~mask);       /* no pull */
	ops->wr(ops->ctx, p->gpio, con + 0xc, ops->rd(ops->ctx, p->gpio, con + 0xc) & ~mask);   /* lowest drive */
}

unsigned
sb_hsi2c_pin_levels(const struct sb_mmio_ops *ops, const struct sb_usi_i2c_platform *p)
{
	uint32_t dat = ops->rd(ops->ctx, p->gpio, p->pin_bank + 4);
	return ((dat >> p->scl_pin) & 1u) | (((dat >> p->sda_pin) & 1u) << 1);
}

static int
as_xfer(void *ctx, uint8_t addr, bool rx, uint8_t *buf, unsigned len)
{
	return sb_hsi2c_xfer((struct sb_hsi2c *)ctx, addr, rx, buf, len);
}

static void
as_udelay(void *ctx, unsigned us)
{
	struct sb_hsi2c *b = (struct sb_hsi2c *)ctx;
	b->ops->udelay(b->ops->ctx, us);
}

void
sb_hsi2c_as_i2c(struct sb_hsi2c *b, struct sb_i2c *out)
{
	out->xfer = as_xfer;
	out->udelay = as_udelay;
	out->ctx = b;
}
