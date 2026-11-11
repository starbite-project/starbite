/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Host-test mocks: an Exynos HSI2C register model (AUTO mode, FIFOs that fill a few bytes per poll, MASTER_BUSY,
 * INT_STAT write-1-to-clear, injected NACK / timeout / stuck busy) with a fake S6SY761 behind it, plus plain
 * register blocks for CMU / SYSREG / GPIO.
 */
#ifndef SB_TOUCH_MOCK_H
#define SB_TOUCH_MOCK_H

#include <stdint.h>
#include <stdbool.h>
#include "sb_hsi2c.h"
#include "sb_s6sy761.h"

/* Block ids used as "bases". */
#define MOCK_HSI2C      1u
#define MOCK_CMU        2u
#define MOCK_SYSREG     3u
#define MOCK_GPIO       4u

struct fake_s6 {
	bool present;
	uint8_t reg;                    /* last register / command written */
	bool sensing;
	unsigned sense_cmds, clear_cmds;
	uint8_t queue[64][8];
	unsigned qhead, qtail;
	unsigned nack_next;             /* NACK this many transfers */
	bool never_empty_but_int;       /* INT low with nothing to read */
	unsigned writes, reads;
	unsigned panel_max_x, panel_max_y;
};

struct mock {
	uint32_t hsi2c[0x100 / 4];
	uint32_t cmu[0x4000 / 4];
	uint32_t sysreg[0x100 / 4];
	uint32_t gpio[0x100 / 4];
	struct fake_s6 chip;
	/* the HSI2C transfer in flight */
	bool running, rx;
	unsigned len, moved, pending_rx, busy_polls;
	uint8_t txbuf[64], rxbuf[64];
	unsigned rx_pos;
	unsigned wire_credit;
	uint8_t addr;
	/* injected faults */
	bool fault_no_done;             /* never TRANSFER_DONE, never data */
	bool fault_stuck_busy;          /* MASTER_BUSY never drops */
	unsigned fifo_per_poll;         /* bytes the "wire" moves per FIFO_STAT read */
	/* observations */
	unsigned sw_resets, starts, udelay_us;
	uint32_t last_fs1, last_fs2, last_fs3;
};

void mock_init(struct mock *m);
void mock_ops(struct mock *m, struct sb_mmio_ops *ops);
void fake_push(struct fake_s6 *c, int tid, unsigned state, unsigned rx, unsigned ry);
void fake_push_raw(struct fake_s6 *c, const uint8_t e[8]);
bool fake_int_low(const struct fake_s6 *c);

#endif
