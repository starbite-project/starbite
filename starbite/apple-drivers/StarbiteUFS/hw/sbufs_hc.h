/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteUFS host controller core: takes the UFSHCI over as S-Boot left it (link up, HS gear, UniPro/M-PHY
 * configured: never reset here), owns the transfer request list, runs the doorbell/completion state machine.
 * Plain C; the kext gives it MMIO, time and DMA memory through sbufs_hw_ops, the host tests a mock controller.
 *
 * Every command goes: build (sbufs_build) -> doorbell audit (sbufs_gate, against the whitelist and the fence)
 * -> vendor nexus bit -> barrier -> doorbell. A command the audit refuses never reaches the doorbell.
 */
#ifndef SBUFS_HC_H
#define SBUFS_HC_H

#include "sbufs_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SBUFS_MAX_SLOTS         8u

/* the DMA area (physically contiguous, below 4 GiB unless dma64): */
#define SBUFS_DMA_UTRL          0x00000u                        /* 32 x 32 bytes, 1 KiB aligned */
#define SBUFS_DMA_UCD           0x01000u                        /* SBUFS_MAX_SLOTS x SBUFS_UCD_SIZE */
#define SBUFS_DMA_SCRATCH       (SBUFS_DMA_UCD + SBUFS_MAX_SLOTS * SBUFS_UCD_SIZE)
#define SBUFS_SCRATCH_SIZE      0x20000u                        /* 2 x 64 KiB: probes and the multi-block self test */
#define SBUFS_DMA_BOUNCE        (SBUFS_DMA_SCRATCH + SBUFS_SCRATCH_SIZE)
#define SBUFS_BOUNCE_SIZE       0x10000u                        /* per slot: 16 blocks */
#define SBUFS_DMA_SIZE(nslots)  (SBUFS_DMA_BOUNCE + (nslots) * SBUFS_BOUNCE_SIZE)
#define SBUFS_MULTI_BLOCKS      16u                             /* blocks per command once multi-entry is proven */

struct sbufs_hw_ops {
	void *ctx;
	uint32_t (*rd)(void *ctx, unsigned bank, uint32_t off);
	void (*wr)(void *ctx, unsigned bank, uint32_t off, uint32_t v);
	void (*barrier)(void *ctx);                             /* dsb sy */
	uint64_t (*now_us)(void *ctx);
	void (*delay_us)(void *ctx, uint32_t us);
	void (*log)(void *ctx, const char *line);               /* one line, no newline */
	/* cache maintenance of the DMA area, by offset; NULL when the area is mapped non-cacheable */
	void (*dma_clean)(void *ctx, uint32_t off, uint32_t len);
	void (*dma_inval)(void *ctx, uint32_t off, uint32_t len);
};

struct sbufs_cfg {
	unsigned nslots;                /* slots used, 1..SBUFS_MAX_SLOTS (and <= the host's NUTRS) */
	bool clr_one;                   /* Exynos: UTRLCLR takes a 1 to clear a slot (BROKEN_REQ_LIST_CLR) */
	bool nexus;                     /* Exynos: vendor UTRL_NEXUS_TYPE bit per SCSI slot */
	bool dma64;                     /* host may address >= 4 GiB (needs CAP.64AS too) */
	bool irq;                       /* set the I bit and IE; else polled */
	uint32_t timeout_us;            /* per command */
};

struct sbufs_slot {
	bool busy;
	bool bounce;                    /* data in this slot's bounce buffer */
	uint64_t deadline;
	struct sbufs_cmd cmd;
	void *cookie;
};

typedef void (*sbufs_done_fn)(void *ctx, unsigned slot, void *cookie, const struct sbufs_result *r);

struct sbufs_hc {
	struct sbufs_hw_ops ops;
	struct sbufs_cfg cfg;
	struct sbufs_layout L;
	uint8_t *dma;                   /* VA of the DMA area */
	uint64_t dma_pa;
	size_t dma_size;
	struct sbufs_fence *fence;
	struct sbufs_slot slot[SBUFS_MAX_SLOTS];
	uint32_t inflight;              /* slots rung and not completed */
	bool ready, dead;
	uint32_t cap, ver, hcs, hce;
	uint32_t vs_saved[4];           /* TX/RX PRDT size, reorder, AXI DMA as S-Boot left them */
	/* counters */
	uint64_t ncmds, nerr, ntimeout, ngate, nfatal, nue, ncompl_poll, ncompl_irq;
	uint64_t foreign_touched;       /* data commands rung outside the fence (re-decoded after the doorbell): 0 */
	uint32_t last_is;
	char line[200];
};

int sbufs_hc_init(struct sbufs_hc *hc, const struct sbufs_hw_ops *ops, const struct sbufs_cfg *cfg, uint8_t *dma_va,
    uint64_t dma_pa, size_t dma_size, struct sbufs_fence *fence);

/* Adopt the host: HCS says device present and list ready (HCE may read 0 on the 9810), nothing in flight, our
 * list taken (read back). IE 0, AHIT 0. Never resets anything. */
int sbufs_hc_take_over(struct sbufs_hc *hc);

/* After the take-over: enable the interrupts this core handles (only if cfg.irq). */
void sbufs_hc_enable_irq(struct sbufs_hc *hc, bool on);

/* Slot management for the asynchronous path. */
int sbufs_hc_alloc_slot(struct sbufs_hc *hc);                   /* slot or SBUFS_E_BUSY / SBUFS_E_NODEV */
void sbufs_hc_free_slot(struct sbufs_hc *hc, unsigned slot);    /* a slot allocated and never issued */
uint8_t *sbufs_hc_bounce_va(struct sbufs_hc *hc, unsigned slot);
uint64_t sbufs_hc_bounce_pa(struct sbufs_hc *hc, unsigned slot);
uint32_t sbufs_hc_max_xfer(const struct sbufs_hc *hc);          /* bytes per command */

/* Issue cmd in slot. segs == NULL with data: the slot's bounce buffer (the caller fills it for a write).
 * On error the slot is freed and nothing was rung. */
int sbufs_hc_issue(struct sbufs_hc *hc, unsigned slot, const struct sbufs_cmd *cmd, const struct sbufs_seg *segs,
    unsigned nsegs, void *cookie);

/* Reap: acknowledge IS, complete every slot whose doorbell bit cleared, time out the late ones, fail everything
 * on a fatal error. from_irq only picks the counter. Returns the number completed. */
unsigned sbufs_hc_poll(struct sbufs_hc *hc, bool from_irq, sbufs_done_fn done, void *ctx);

/* Synchronous: one command, polled to completion (start-up, probes, the self test). */
int sbufs_hc_run(struct sbufs_hc *hc, const struct sbufs_cmd *cmd, uint8_t *data_out, uint32_t data_len,
    struct sbufs_result *r);

/* Start-up steps. */
int sbufs_hc_wake(struct sbufs_hc *hc);                         /* device W-LUN: TUR (UA), SSU ACTIVE */
int sbufs_hc_probe_lu(struct sbufs_hc *hc, uint8_t lun, uint64_t *blocks, uint32_t *blksz);
int sbufs_hc_inquiry(struct sbufs_hc *hc, uint8_t lun, char vendor[9], char product[17], char rev[5]);
/* Multi-entry PRDTs (16 x 4 KiB per command): program the vendor registers, prove it with reads of 16 blocks at
 * lun/lba (which must be inside the fence) against single-block reads; restore and stay at 1 on any mismatch. */
int sbufs_hc_multi_selftest(struct sbufs_hc *hc, uint8_t lun, uint64_t lba);

#ifdef __cplusplus
}
#endif

#endif /* SBUFS_HC_H */
