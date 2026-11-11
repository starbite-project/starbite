/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Exynos 9810 USI high-speed I2C (HSI2C) master, polled, AUTO mode, one message with STOP per transfer.
 * Plain C, no IOKit: shared by StarbiteTouch (S6SY761 on USI03), the host tests and, later, StarbiteHSI2C
 * (docs/APPLE-S9-DRIVERS.md Part 3 row 13), which should take these files over unchanged.
 *
 * Register facts are today's working driver (build/xnu-12377 bsd/dev/starlte_touch.c, 0302: st_bus_init,
 * st_message, st_platform_init), cross-checked against an earlier port's APSL s6sy761.c; see ../NOTES.md.
 * Addresses are never compiled in: the caller passes mapped bases that came from the ADT.
 */
#ifndef SB_HSI2C_H
#define SB_HSI2C_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MMIO and delay hooks: kernel = mapped device memory + IODelay, host tests = a register model. */
struct sb_mmio_ops {
	uint32_t (*rd)(void *ctx, uintptr_t base, uint32_t off);
	void     (*wr)(void *ctx, uintptr_t base, uint32_t off, uint32_t v);
	void     (*udelay)(void *ctx, unsigned us);
	void      *ctx;
};

/* HSI2C registers (offsets in the block). */
#define SB_HSI2C_CTL            0x00u
#define SB_HSI2C_FIFO_CTL       0x04u
#define SB_HSI2C_TRAILING       0x08u
#define SB_HSI2C_INT_EN         0x20u
#define SB_HSI2C_INT_STAT       0x24u
#define SB_HSI2C_FIFO_STAT      0x30u
#define SB_HSI2C_TX_DATA        0x34u
#define SB_HSI2C_RX_DATA        0x38u
#define SB_HSI2C_CONF           0x40u
#define SB_HSI2C_AUTO_CONF      0x44u
#define SB_HSI2C_TIMEOUT        0x48u
#define SB_HSI2C_TRANS          0x50u
#define SB_HSI2C_FS1            0x60u
#define SB_HSI2C_FS2            0x64u
#define SB_HSI2C_FS3            0x68u
#define SB_HSI2C_ADDR           0x70u
#define SB_HSI2C_USI_CON        0xc4u

/* Bits. */
#define SB_HSI2C_CTL_SW_RST     0x80000000u
#define SB_HSI2C_CTL_TXCHON     0x00000080u
#define SB_HSI2C_CTL_RXCHON     0x00000040u
#define SB_HSI2C_CTL_MASTER     0x00000008u
#define SB_HSI2C_CONF_AUTO      0x80000000u
#define SB_HSI2C_TIMEOUT_EN     0x80000000u
#define SB_HSI2C_AUTO_RUN       0x80000000u
#define SB_HSI2C_AUTO_STOP      0x00020000u     /* STOP after the transfer */
#define SB_HSI2C_AUTO_READ      0x00010000u
#define SB_HSI2C_INT_DONE       0x00000080u     /* TRANSFER_DONE */
#define SB_HSI2C_INT_ABORT      0x00000100u
#define SB_HSI2C_INT_NOACK      0x00000200u
#define SB_HSI2C_INT_NODEV      0x00000400u
#define SB_HSI2C_INT_TIMEOUT    0x00000800u
#define SB_HSI2C_INT_ERRORS     0x00000f00u
#define SB_HSI2C_FIFO_TX_FULL   0x00000080u
#define SB_HSI2C_FIFO_RX_EMPTY  0x01000000u
#define SB_HSI2C_TRANS_BUSY     0x00020000u     /* MASTER_BUSY */
#define SB_HSI2C_MAX_MSG        32u

/* Polling limits of one message (1 us per step, as starlte_touch.c). */
#define SB_HSI2C_RUN_STEPS      50000u
#define SB_HSI2C_IDLE_STEPS     10000u

enum sb_i2c_err {
	SB_I2C_OK = 0,
	SB_I2C_EINVAL = -1,     /* length 0 or > 32 */
	SB_I2C_ENOACK = -2,     /* INT_STAT NO_DEV_ACK */
	SB_I2C_ENODEV = -3,     /* INT_STAT NO_DEV */
	SB_I2C_ETIMEOUT = -4,   /* INT_STAT TIMEOUT, or no progress in SB_HSI2C_RUN_STEPS */
	SB_I2C_EABORT = -5,     /* INT_STAT TRANS_ABORT */
	SB_I2C_EBUSY = -6,      /* MASTER_BUSY did not clear after the bytes moved */
};

enum sb_hsi2c_state {
	SB_HSI2C_IDLE = 0,
	SB_HSI2C_RUN,           /* bytes moving: FIFO filled / drained, INT_STAT watched */
	SB_HSI2C_WAIT_IDLE,     /* all bytes moved: wait for MASTER_BUSY to drop */
	SB_HSI2C_DONE,
	SB_HSI2C_ERROR,
};

/* Bus timing for the 9810's 200 MHz IP clock: FS1[23:16] start hold, FS2[7:0] SCL-high mask, FS3[23:16]
 * divider. 400 kHz: 0xf0 / 0xe0 / 0x21 (starlte_touch.c, from exynos9810-woa). */
struct sb_hsi2c_timing {
	uint8_t fs1_hold, fs2_mask, fs3_div;
};
#define SB_HSI2C_TIMING_400K { 0xf0, 0xe0, 0x21 }

struct sb_hsi2c {
	const struct sb_mmio_ops *ops;
	uintptr_t regs;
	struct sb_hsi2c_timing timing;
	/* the transfer in flight */
	enum sb_hsi2c_state state;
	bool rx, xfer_done;
	uint8_t addr;
	uint8_t *buf;
	unsigned len, done, steps;
	uint32_t irq_seen;
	int err;
	/* statistics and the last failure's registers (for the log) */
	unsigned xfers, failures;
	uint32_t fail_int, fail_trans, fail_fifo;
};

void sb_hsi2c_setup(struct sb_hsi2c *b, const struct sb_mmio_ops *ops, uintptr_t regs,
    const struct sb_hsi2c_timing *t);
/* USI reset release, soft reset, timing, master, AUTO mode, interrupts off (st_bus_init). */
void sb_hsi2c_reset(struct sb_hsi2c *b);
/* Starts one message: reset (the DTB's samsung,reset-before-trans), FIFO, address, AUTO_CONF, RUN. */
int  sb_hsi2c_start(struct sb_hsi2c *b, uint8_t addr, bool rx, uint8_t *buf, unsigned len);
/* One poll of the state machine; no delay inside. Returns the new state. */
enum sb_hsi2c_state sb_hsi2c_step(struct sb_hsi2c *b);
/* start + step + 1 us delays, bounded. 0 or an sb_i2c_err. */
int  sb_hsi2c_xfer(struct sb_hsi2c *b, uint8_t addr, bool rx, uint8_t *buf, unsigned len);
const char *sb_i2c_strerror(int err);

/*
 * The USI I2C bus's platform side, before any HSI2C access: CMU gates (bit 20 manual, bit 21 on) and the
 * Q-channel, the USI block's mode in SYSREG, the two pins' I2C function with no pull and lowest drive.
 * Each base is a mapped ADT block; offsets are the block's register facts.
 */
struct sb_usi_i2c_platform {
	uintptr_t cmu, sysreg, gpio;
	const uint32_t *gates;
	unsigned ngates;
	uint32_t qch;                   /* CMU Q-channel register, bits 1:0 set */
	uint32_t usi_off, usi_mode;     /* SYSREG: USI mode register, 4 = I2C */
	uint32_t pin_bank;              /* GPIO bank CON offset; DAT +4, PUD +8, DRV +0xc */
	uint8_t scl_pin, sda_pin, pin_func;
};
/* The S9's touch bus (USI03 of PERIC0, GPP1-4/5), starlte_touch.c st_platform_init. */
extern const struct sb_usi_i2c_platform sb_usi03_touch_bus;
void sb_hsi2c_platform_init(const struct sb_mmio_ops *ops, const struct sb_usi_i2c_platform *p);
/* SCL/SDA levels (bit 0 SCL, bit 1 SDA) from the bank's DAT: both low = the bus has no pull-up power. */
unsigned sb_hsi2c_pin_levels(const struct sb_mmio_ops *ops, const struct sb_usi_i2c_platform *p);

/* A byte-message bus as the chip drivers see it (HSI2C here, a fake chip in the tests). */
struct sb_i2c {
	int  (*xfer)(void *ctx, uint8_t addr, bool rx, uint8_t *buf, unsigned len);
	void (*udelay)(void *ctx, unsigned us);
	void *ctx;
};
/* An sb_i2c that runs on this HSI2C master. */
void sb_hsi2c_as_i2c(struct sb_hsi2c *b, struct sb_i2c *out);

#ifdef __cplusplus
}
#endif
#endif /* SB_HSI2C_H */
