/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Host-test register file for StarbiteDisplay: four register blocks, a log of every write in order, and a small
 * model of the hardware behaviour the driver depends on (frames taking shadow updates, write-1-to-clear interrupt
 * registers, the DSIM packet FIFO). It models what NOTES.md states about the hardware, nothing more.
 */
#ifndef MOCK_MMIO_H
#define MOCK_MMIO_H

#include "../hw/sbd_io.h"

#define MOCK_WORDS      (0x10000 / 4)
#define MOCK_LOG        8192
#define MOCK_PACKETS    64

struct mock_write {
	unsigned block;
	uint32_t off, val;
};

struct mock_packet {
	uint8_t  type;
	bool     bta;
	uint32_t len;                   /* long: payload length; short: 1 or 2 */
	uint8_t  data[64];
};

struct mock {
	uint32_t reg[SBD_BLK_COUNT][MOCK_WORDS];
	struct mock_write log[MOCK_LOG];
	unsigned nlog;
	uint64_t slept_us;

	/* DECON model */
	uint32_t active_base[6];        /* what each channel's IDMA really reads (taken at a frame) */
	uint32_t active_path1;
	uint32_t active_win_start, active_win_end;
	unsigned frames;
	bool     auto_frames;           /* a frame every 16 ms of udelay */
	uint64_t frame_clock_us;

	/* DSIM model */
	struct mock_packet pkt[MOCK_PACKETS];
	unsigned npkt;
	uint8_t  payload[64];
	uint32_t payload_len;
	int      tx_delay_polls;        /* INTSRC reads before a header counts as sent; -1 = never */
	int      tx_polls_left;
	bool     fail_next_tx;          /* the next packet never completes (one timeout) */
	uint32_t rx[16];
	unsigned rx_len, rx_pos;
	bool     answer_reads;
};

void mock_init(struct mock *m);
struct sbd_io mock_io(struct mock *m);

/* the state S-Boot and our loader leave: window 5 on channel G0 reading 0xcc000000, command mode, HW trigger on */
void mock_boot_state(struct mock *m);
/* one refresh: frame count, shadow registers taken (if the trigger runs), interrupts raised (if enabled) */
void mock_frame(struct mock *m);
void mock_log_reset(struct mock *m);
/* index of the first logged write to (block, off) at or after `from`, or -1 */
int  mock_find(const struct mock *m, unsigned from, unsigned block, uint32_t off);
void mock_queue_rx(struct mock *m, const uint32_t *words, unsigned n);

#endif
