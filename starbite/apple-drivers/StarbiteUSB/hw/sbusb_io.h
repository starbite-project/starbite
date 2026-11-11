/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB hardware layer: the backend the register logic runs on. Plain C, no IOKit. The kext gives
 * mapped nub memory; the host tests give a mock (test/mock.c).
 */
#ifndef SBUSB_IO_H
#define SBUSB_IO_H
#include <stdint.h>
#include <stddef.h>

/* Register blocks. DWC3 and PHY are the "reg" of the usb-drd and usb-phy nubs. The other five are single
 * 32-bit registers (offset 0) that the usb-phy nub must carry as extra "reg" entries 1..5 (INTEGRATION.md);
 * a missing one is skipped and reported, never guessed. */
enum {
	SBUSB_BLK_DWC3 = 0,     /* 0x10c00000 + 0x10000 */
	SBUSB_BLK_PHY,          /* USBCON 0x11100000 + 0x200 */
	SBUSB_BLK_PMU_USB2,     /* 0x1406072c: USB 2.0 PHY isolation, bit 0 = enabled */
	SBUSB_BLK_PMU_USB3DP,   /* 0x14060704: USB3/DP PHY isolation, bit 0 = enabled */
	SBUSB_BLK_GATE_TOP,     /* 0x1a242064: GATE_CLKCMU_FSYS0_USB30DRD */
	SBUSB_BLK_GATE_REF,     /* 0x1100206c: FSYS0 USB30DRD_REF_CLK */
	SBUSB_BLK_GATE_DPPHY,   /* 0x11002008: FSYS0 USBDPPHY_REF_SOC_PLL */
	SBUSB_BLK_COUNT
};

struct sbusb_io {
	void *ctx;
	int (*has)(void *ctx, unsigned blk);                      /* is this block mapped */
	uint32_t (*rd)(void *ctx, unsigned blk, uint32_t off);
	void (*wr)(void *ctx, unsigned blk, uint32_t off, uint32_t v);
	void (*delay_us)(void *ctx, unsigned us);
	uint64_t (*now_us)(void *ctx);
	/* DMA memory coherence around [off, off+len) of the DMA block: to_dev = CPU wrote and the controller
	 * will read; else the controller wrote and the CPU will read. May be NULL (memory mapped uncached). */
	void (*sync)(void *ctx, uint32_t off, uint32_t len, int to_dev);
};

/* One physically contiguous block the controller reaches (layout: sbusb_dwc3.h). */
struct sbusb_dma {
	uint8_t *va;
	uint64_t pa;
	uint32_t size;
};

static inline void sbusb_put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void sbusb_put32(uint8_t *p, uint32_t v) { sbusb_put16(p, v & 0xffffU); sbusb_put16(p + 2, v >> 16); }
static inline unsigned sbusb_get16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static inline uint32_t sbusb_get32(const uint8_t *p) { return (uint32_t)sbusb_get16(p) | (uint32_t)sbusb_get16(p + 2) << 16; }

#endif
