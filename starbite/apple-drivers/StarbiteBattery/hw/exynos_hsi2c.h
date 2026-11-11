/* SPDX-License-Identifier: APSL-2.0 */
/*
 * A polled, read-only Exynos 9810 USI HSI2C master in AUTO mode, for the MAX77705 on HSI2C 0x14360000 (USI_CMGP03).
 * Re-implemented from our own build/xnu-12377/bsd/dev/starlte_touch.c (st_bus_init / st_message) and
 * herolte_battery.c (fg_message, repeated start). Facts: ../NOTES.md.
 *
 * Read-only towards the devices: the only bytes ever sent to a slave are one-byte register pointers before a
 * read. There is no write API. Clear-on-read registers are refused (max77705.h lists them).
 * When StarbiteHSI2C (row 13) exists this file is replaced by its provider API (INTEGRATION.md).
 */
#ifndef STARBITE_EXYNOS_HSI2C_H
#define STARBITE_EXYNOS_HSI2C_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HSI2C_CTL               0x00u   /* [31] soft reset, [7] TXCHON, [6] RXCHON, [3] master */
#define HSI2C_FIFO_CTL          0x04u   /* [0] RX FIFO en, [1] TX FIFO en, [10:4] RX trigger, [22:16] TX trigger */
#define HSI2C_TRAILING_CTL      0x08u
#define HSI2C_INT_EN            0x20u
#define HSI2C_INT_STAT          0x24u   /* [7] TRANSFER_DONE (auto), [11:8] errors (NACKs, timeout) */
#define HSI2C_FIFO_STAT         0x30u   /* [7] TX full, [8] TX empty, [22:16] RX level, [24] RX empty */
#define HSI2C_TX_DATA           0x34u
#define HSI2C_RX_DATA           0x38u
#define HSI2C_CONF              0x40u   /* [31] AUTO mode */
#define HSI2C_AUTO_CONF         0x44u   /* [15:0] count, [16] read, [17] STOP after, [31] MASTER_RUN */
#define HSI2C_TIMEOUT           0x48u   /* [31] timeout enable */
#define HSI2C_TRANS_STATUS      0x50u   /* [17] master busy */
#define HSI2C_TIMING_FS1        0x60u
#define HSI2C_TIMING_FS2        0x64u
#define HSI2C_TIMING_FS3        0x68u
#define HSI2C_ADDR              0x70u   /* [16:10] slave address (master) */
#define HSI2C_USI_CON           0xc4u   /* [0] USI reset, active high */

#define HSI2C_INT_XFER_DONE     0x80u
#define HSI2C_INT_ERRORS        0xf00u
#define HSI2C_FIFO_TX_FULL      (1u << 7)
#define HSI2C_FIFO_TX_EMPTY     (1u << 8)
#define HSI2C_FIFO_RX_EMPTY     (1u << 24)

#define HSI2C_POLLS             50000u  /* x 1 us per message */
#define HSI2C_TX_IDLE_POLLS      200u    /* TX empty this long without TRANSFER_DONE: a no-STOP write is out */

struct hsi2c_io {
	void *ctx;
	uint32_t (*rd)(void *ctx, uint32_t off);
	void (*wr)(void *ctx, uint32_t off, uint32_t v);
	void (*delay_us)(void *ctx, uint32_t us);
};

struct hsi2c {
	const struct hsi2c_io *io;
	uint32_t fs1, fs2, fs3;         /* S-Boot's timing, saved before the first reset */
	bool saved;
	uint32_t errors, resets, fallbacks;
	uint32_t last_int, last_fifo, last_trans;
};

/* Save S-Boot's timing (0x21 / 0xe0 / 0xf0 for 400 kHz from 200 MHz if it left none), then reset the controller. */
void hsi2c_init(struct hsi2c *h, const struct hsi2c_io *io);
void hsi2c_reset(struct hsi2c *h);

/* One AUTO-mode message; stop = false leaves the bus for a repeated START. 1..32 bytes. 0 = ok. */
int hsi2c_msg(struct hsi2c *h, uint8_t addr7, bool receive, uint8_t *buf, unsigned count, bool stop);

/* Register read: pointer byte without STOP, then a read with STOP (repeated start); if that fails, the same
 * with a STOP between the halves. 0 = ok. */
int hsi2c_read_reg(struct hsi2c *h, uint8_t addr7, uint8_t reg, uint8_t *buf, unsigned len);

#ifdef __cplusplus
}
#endif
#endif
