/* SPDX-License-Identifier: APSL-2.0 */
/*
 * A mock APM for the host tests: the SRAM (with the S9's faulting first pages), the mailbox, a channel table like
 * the phone's, and a firmware loop that answers PMIC requests from an S2MPS18 register model (TOP, PMIC, RTC with
 * its latch). Shared with StarbiteRTC's tests.
 */
#ifndef MOCK_APM_H
#define MOCK_APM_H

#include <stdint.h>
#include <stdbool.h>
#include "../hw/acpm_ipc.h"

#define MOCK_SRAM_SIZE  ACPM_S9_SRAM_SIZE

struct mock_apm {
	uint32_t sram[MOCK_SRAM_SIZE / 4];
	uint32_t intsr1, intmr1, doorbells;
	uint64_t now_us;
	unsigned faults;                /* SRAM accesses below sram_first: must stay 0 */
	unsigned bad_doorbells;
	/* the S2MPS18 */
	uint8_t top[256], pmic[256], rtc[256];
	uint64_t rtc_counter;           /* the RTC's own time, Unix seconds */
	int rudr_pending, wudr_pending; /* polls until the update bit clears (-1: never) */
	unsigned latch_delay_polls, write_delay_polls;
	unsigned pmic_reads, pmic_writes, rtc_writes;
	uint32_t last_w0, last_w1;
	/* behaviour switches */
	unsigned answer_delay_polls;    /* answer after this many delay() calls */
	unsigned drop_answers;          /* swallow the next N requests (no answer at all) */
	unsigned speedy_fail;           /* answer the next N PMIC requests with status 1 */
	bool never_take;                /* never advance in_rear (a stuck APM) */
	unsigned pending_polls;
	bool dvfs_seen;
	uint32_t dvfs_domain, dvfs_khz;
};

extern struct acpm_io mock_apm_io;
extern struct mock_apm *mock;

/* A fresh mock with the S9 layout: channels 0 (DVFS, index 0), 2 (PMIC, queue of len x elem), 5, plus a buffer
 * channel 7. stale: answers left in channel 2's answer queue, as an earlier kernel would. */
void mock_apm_reset(struct mock_apm *m, uint32_t len, uint32_t elem, unsigned stale);

/* RTC registers 0x04..0x0b from the counter, binary or BCD, 24 h */
void mock_rtc_latch(struct mock_apm *m);

#endif
