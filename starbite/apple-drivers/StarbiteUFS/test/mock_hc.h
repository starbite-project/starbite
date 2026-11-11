/* SPDX-License-Identifier: APSL-2.0 */
/*
 * A mock Exynos 9810 UFS host for the host tests: registers in two banks, the DMA area in host memory at a made-up
 * physical address, general LUs backed by RAM, and a device that executes what the doorbell rings, decoding the
 * descriptors ITSELF (independently of sbufs_proto.c) and recording every block it touches.
 */
#ifndef MOCK_HC_H
#define MOCK_HC_H

#include "../hw/sbufs_hc.h"

#define MOCK_DMA_PA     0x9a000000ull           /* below 4 GiB, like the phone's low RAM window */
#define MOCK_MAX_MAP    8

struct mock_lu {
	uint64_t blocks;                        /* 0 = LU not present */
	uint8_t *data;                          /* blocks x 4096 */
	uint8_t *touched;                       /* per block: 1 read, 2 written */
	bool ua_pending;                        /* next command reports UNIT ATTENTION */
};

struct mock_map {
	uint64_t pa;
	uint8_t *va;
	size_t len;
};

struct mock {
	uint32_t hci[0x200 / 4], vs[0x200 / 4];
	uint8_t *dma;                           /* the DMA area */
	size_t dma_size;
	struct mock_map map[MOCK_MAX_MAP];      /* client buffers for direct DMA */
	unsigned nmap;
	struct mock_lu lu[8];
	bool device_asleep;                     /* W-LUN TUR: OCS 7 + NOT READY until SSU ACTIVE */
	bool deferred;                          /* complete on mock_tick, not at the doorbell */
	uint32_t pending;                       /* rung, not yet executed (deferred mode) */
	bool multi_needs_vs;                    /* >1 PRDT entry returns garbage unless TX/RX PRDT are programmed */
	bool multi_broken;                      /* >1 PRDT entry always returns garbage */
	/* injections: count down per data/any command */
	unsigned inject_ocs;                    /* next N commands: OCS 0x7 */
	unsigned inject_check;                  /* next N commands: CHECK CONDITION, sense 3/11/00 (medium error) */
	unsigned inject_hang;                   /* next N commands never complete; UTRLCLR (1 = clear) frees them */
	bool inject_fatal;                      /* next doorbell: IS HCFES, nothing completes */
	bool host_off;                          /* HCS without DP (set before mock_init's values are read) */
	bool rc10_ffff;                         /* READ CAPACITY(10) answers 0xffffffff: the driver must ask (16) */
	bool refuse_list;                       /* UTRLBA reads back wrong */
	uint32_t hung;                          /* slots hung */
	uint64_t now;                           /* microseconds */
	uint64_t ncmds, nexus_missing, bad_prdt;
	uint64_t last_lba[8];
	struct sbufs_hw_ops ops;
	char lastlog[256];
	unsigned nlog;
	bool echo;                              /* print the core's log lines */
};

void mock_init(struct mock *m, size_t dma_size);
void mock_free(struct mock *m);
void mock_add_lu(struct mock *m, unsigned lun, uint64_t blocks);
void mock_map_buffer(struct mock *m, uint64_t pa, uint8_t *va, size_t len);
void mock_tick(struct mock *m);                         /* deferred mode: execute what is pending */
void mock_tick_mask(struct mock *m, uint32_t mask);     /* only these slots (out-of-order completion) */
uint64_t mock_count_touched(const struct mock *m, unsigned lun, uint64_t first, uint64_t count, int how);
uint64_t mock_count_touched_outside(const struct mock *m, unsigned lun, uint64_t first, uint64_t count);

#endif
