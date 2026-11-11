/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteDisplay register access: one small back end shared by the kext (real MMIO from the ADT nubs)
 * and the host tests (a mock register file). No IOKit, no libc.
 */
#ifndef SBD_IO_H
#define SBD_IO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register blocks. The kext maps each from a "reg" entry of the decon / dsim nub (INTEGRATION.md). */
enum sbd_block {
	SBD_BLK_DECON = 0,      /* DECON_F, 0x16030000 */
	SBD_BLK_IDMA  = 1,      /* DPU_DMA, 0x16070000: channel block k at 0x1000 * (k + 1) */
	SBD_BLK_DPP   = 2,      /* DPP, 0x16020000: channel block k at 0x1000 * (k + 1) */
	SBD_BLK_DSIM  = 3,      /* MIPI DSIM0, 0x16080000 */
	SBD_BLK_COUNT = 4
};

struct sbd_io {
	uint32_t (*rd)(void *ctx, unsigned block, uint32_t off);
	void     (*wr)(void *ctx, unsigned block, uint32_t off, uint32_t val);
	void     (*udelay)(void *ctx, uint32_t us);     /* may sleep; never called from the interrupt path */
	void     *ctx;
};

static inline uint32_t
sbd_rd(const struct sbd_io *io, unsigned block, uint32_t off)
{
	return io->rd(io->ctx, block, off);
}

static inline void
sbd_wr(const struct sbd_io *io, unsigned block, uint32_t off, uint32_t val)
{
	io->wr(io->ctx, block, off, val);
}

/* read-modify-write: bits in mask become those of val */
static inline void
sbd_wr_mask(const struct sbd_io *io, unsigned block, uint32_t off, uint32_t val, uint32_t mask)
{
	uint32_t old = io->rd(io->ctx, block, off);
	io->wr(io->ctx, block, off, (old & ~mask) | (val & mask));
}

#ifdef __cplusplus
}
#endif
#endif /* SBD_IO_H */
