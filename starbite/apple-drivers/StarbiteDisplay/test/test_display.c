/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Host tests for StarbiteDisplay's hw/ layer against the mock register file:
 *   - probe accepts the boot state and refuses anything else without writing
 *   - a flip writes the right registers in the right order, and is taken at the next frame
 *   - vsync accounting, by interrupt and by polling FRAME_COUNT
 *   - the scaler channel set-up, the letterbox, and the way back
 *   - DSIM packets: the backlight's DCS bytes, short writes, reads, timeouts and the retry
 *   - the panel allow-list
 *   - the swap record in both layouts
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "mock_mmio.h"
#include "../hw/sbd_regs.h"
#include "../hw/sbd_decon.h"
#include "../hw/sbd_dsim.h"
#include "../hw/sbd_panel.h"
#include "../hw/sbd_swap.h"

static int checks, failures;
static const char *current;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { failures++; printf("FAIL %s: %s (line %d)\n", current, #cond, __LINE__); } \
} while (0)
#define CHECK_EQ(a, b) do { \
	unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b); \
	checks++; \
	if (va_ != vb_) { failures++; printf("FAIL %s: %s = %#llx, expected %s = %#llx (line %d)\n", current, #a, va_, #b, vb_, __LINE__); } \
} while (0)

#define FB      0xcc000000ull
#define G0      IDMA_CH(SBD_CH_G0)
#define VGF0    IDMA_CH(SBD_CH_VGF0)

static struct mock m;
static struct sbd_decon d;

static void
fresh(void)
{
	struct sbd_io io;

	mock_init(&m);
	mock_boot_state(&m);
	io = mock_io(&m);
	sbd_decon_init(&d, &io);
}

static void
test_probe(void)
{
	current = "probe";
	fresh();
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	CHECK_EQ(m.nlog, 0);                            /* reads only */
	CHECK_EQ(d.win, 5);
	CHECK_EQ(d.boot_type, SBD_CH_G0);
	CHECK_EQ(d.boot_base, 0xcc000000u);
	CHECK_EQ(d.shown_base, 0xcc000000u);
	CHECK_EQ(d.boot_trig, 0x1281);

	fresh();
	CHECK_EQ(sbd_decon_probe(&d, FB, 1080, 2400, 4320), SBD_ERR_GEOMETRY);
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_ERR_GEOMETRY);         /* a refusal sticks */
	CHECK_EQ(sbd_decon_attach_flip(&d), SBD_ERR_NOT_PROBED);
	CHECK_EQ(sbd_decon_flip(&d, 0x94000000, 5760), SBD_ERR_NOT_PROBED);

	fresh();
	m.reg[SBD_BLK_DECON][DECON_DATA_PATH_0 / 4] |= 1u;                              /* a second window */
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_ERR_WINDOWS);

	fresh();
	m.reg[SBD_BLK_DECON][DECON_DATA_PATH_1 / 4] = 6u << 20;                         /* no IDMA has channel 6 */
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_ERR_CHANNEL);

	fresh();
	m.reg[SBD_BLK_IDMA][(G0 + IDMA_BASE_Y) / 4] = 0xcd000000u;
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_ERR_NOT_BOOT_BUFFER);

	fresh();
	m.reg[SBD_BLK_DECON][DECON_GLOBAL_CONTROL / 4] &= ~DECON_GC_CMD_MODE;
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_ERR_NOT_RUNNING);
	CHECK_EQ(m.nlog, 0);                            /* refused: still nothing written */
}

static void
test_flip_sequence(void)
{
	int base_y, base_c, shadow;

	current = "flip sequence";
	fresh();
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	CHECK_EQ(sbd_decon_flip(&d, 0x94000000, 5760), SBD_ERR_MODE);                  /* not attached yet */
	CHECK_EQ(sbd_decon_attach_flip(&d), SBD_OK);
	CHECK_EQ(m.nlog, 0);                            /* attaching writes nothing */

	CHECK_EQ(sbd_decon_flip(&d, 0x94000000, 5760), SBD_OK);
	/* exactly three writes: the address (Y, then C), then the shadow request; nothing else */
	CHECK_EQ(m.nlog, 3);
	base_y = mock_find(&m, 0, SBD_BLK_IDMA, G0 + IDMA_BASE_Y);
	base_c = mock_find(&m, 0, SBD_BLK_IDMA, G0 + IDMA_BASE_C);
	shadow = mock_find(&m, 0, SBD_BLK_DECON, DECON_SHADOW_REQ);
	CHECK(base_y == 0 && base_c == 1 && shadow == 2);
	CHECK_EQ(m.log[0].val, 0x94000000u);
	CHECK_EQ(m.log[1].val, 0x94000000u);
	CHECK_EQ(m.log[2].val, 0x8000003fu);            /* global bit + all six windows */

	/* not shown until a frame takes it */
	CHECK(d.pending);
	CHECK(!sbd_decon_latched(&d));
	CHECK_EQ(m.active_base[SBD_CH_G0], 0xcc000000u);
	CHECK_EQ(d.shown_base, 0xcc000000u);
	mock_frame(&m);
	CHECK_EQ(m.active_base[SBD_CH_G0], 0x94000000u);
	CHECK(sbd_decon_latched(&d));

	/* a different row length: SRC_SIZE first (height kept), then the address, then the request */
	mock_log_reset(&m);
	CHECK_EQ(sbd_decon_flip(&d, 0x95000000, 5824), SBD_OK);
	CHECK_EQ(m.nlog, 4);
	CHECK(m.log[0].block == SBD_BLK_IDMA && m.log[0].off == G0 + IDMA_SRC_SIZE);
	CHECK_EQ(m.log[0].val, (2960u << 16) | 1456u);
	CHECK(m.log[1].off == G0 + IDMA_BASE_Y && m.log[2].off == G0 + IDMA_BASE_C);
	CHECK(m.log[3].block == SBD_BLK_DECON && m.log[3].off == DECON_SHADOW_REQ);

	/* refusals write nothing */
	mock_log_reset(&m);
	CHECK_EQ(sbd_decon_flip(&d, 0x880000000ull, 5760), SBD_ERR_ADDRESS);           /* bank 2: above 4 GiB */
	CHECK_EQ(sbd_decon_flip(&d, 0xdf800000ull, 5760), SBD_ERR_ADDRESS);            /* runs past 0xe0000000 */
	CHECK_EQ(sbd_decon_flip(&d, 0x94000010ull, 5760), SBD_ERR_ADDRESS);            /* misaligned */
	CHECK_EQ(sbd_decon_flip(&d, 0, 5760), SBD_ERR_ADDRESS);
	CHECK_EQ(sbd_decon_flip(&d, 0x94000000, 5762), SBD_ERR_STRIDE);
	CHECK_EQ(sbd_decon_flip(&d, 0x94000000, 4096), SBD_ERR_STRIDE);                /* shorter than the picture */
	CHECK_EQ(m.nlog, 0);
}

static void
test_flip_masked_trigger(void)
{
	current = "flip with the trigger masked";
	fresh();
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	CHECK_EQ(sbd_decon_attach_flip(&d), SBD_OK);
	/* S-Boot's own value: active value, edge polarity, masked, HW trigger off */
	m.reg[SBD_BLK_DECON][DECON_TRIG_CONTROL / 4] = 0x3070;
	mock_frame(&m);
	CHECK_EQ(m.frames, 0);                          /* the model: no frames while masked */
	CHECK_EQ(sbd_decon_flip(&d, 0x94000000, 5760), SBD_OK);
	/* the trigger is unmasked BEFORE the address and the request, or the update is never taken */
	CHECK(m.log[0].block == SBD_BLK_DECON && m.log[0].off == DECON_TRIG_CONTROL);
	CHECK_EQ(m.log[0].val, 0x3061);                 /* EN set, MASK clear, the other bits kept */
	CHECK(mock_find(&m, 0, SBD_BLK_DECON, DECON_SHADOW_REQ) == (int)m.nlog - 1);
	mock_frame(&m);
	CHECK_EQ(m.active_base[SBD_CH_G0], 0x94000000u);

	sbd_decon_trigger(&d, false);
	CHECK_EQ(m.reg[SBD_BLK_DECON][DECON_TRIG_CONTROL / 4], 0x3070);
}

static void
test_vsync_irq(void)
{
	uint32_t ev;

	current = "vsync by interrupt";
	fresh();
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	CHECK_EQ(sbd_decon_attach_flip(&d), SBD_OK);
	m.reg[SBD_BLK_DECON][DECON_INT_PENDING / 4] = DECON_INT_FRAME_START;            /* stale, from before */
	sbd_decon_irq_enable(&d, true);
	CHECK_EQ(m.reg[SBD_BLK_DECON][DECON_INT_PENDING / 4], 0);                       /* cleared first */
	CHECK_EQ(m.reg[SBD_BLK_DECON][DECON_INT_ENABLE / 4], 0x3011);
	CHECK_EQ(m.reg[SBD_BLK_DECON][DECON_EXTRA_INT_ENABLE / 4], 0x110);
	CHECK_EQ(sbd_decon_irq(&d), 0);                 /* a spurious call counts nothing */
	CHECK_EQ(d.vsync_irqs, 0);

	for (int i = 0; i < 60; i++) {
		mock_frame(&m);
		ev = sbd_decon_irq(&d);
		CHECK(ev & SBD_EV_VSYNC);
		CHECK(ev & SBD_EV_FRAME_DONE);
		CHECK_EQ(m.reg[SBD_BLK_DECON][DECON_INT_PENDING / 4], 0);               /* both bits cleared */
	}
	CHECK_EQ(d.vsync_irqs, 60);
	CHECK_EQ(d.frame_done_irqs, 60);
	CHECK_EQ(d.flips_latched, 0);

	/* a flip is counted as taken at the first frame interrupt after the DECON took it, once */
	CHECK_EQ(sbd_decon_flip(&d, 0x94000000, 5760), SBD_OK);
	CHECK_EQ(sbd_decon_irq(&d), 0);                 /* no frame yet: not latched */
	CHECK_EQ(d.flips_latched, 0);
	mock_frame(&m);
	ev = sbd_decon_irq(&d);
	CHECK(ev & SBD_EV_LATCHED);
	CHECK_EQ(d.flips_latched, 1);
	CHECK_EQ(d.shown_base, 0x94000000u);
	mock_frame(&m);
	ev = sbd_decon_irq(&d);
	CHECK(!(ev & SBD_EV_LATCHED));
	CHECK_EQ(d.flips_latched, 1);

	/* two flips inside one refresh: the second replaces the first, one latch */
	CHECK_EQ(sbd_decon_flip(&d, 0x95000000, 5760), SBD_OK);
	CHECK_EQ(sbd_decon_flip(&d, 0x96000000, 5760), SBD_OK);
	mock_frame(&m);
	(void)sbd_decon_irq(&d);
	CHECK_EQ(d.flips_requested, 3);
	CHECK_EQ(d.flips_replaced, 1);
	CHECK_EQ(d.flips_latched, 2);
	CHECK_EQ(d.shown_base, 0x96000000u);
	CHECK_EQ(m.active_base[SBD_CH_G0], 0x96000000u);

	/* extra interrupts: each cause cleared in its own register and counted */
	m.reg[SBD_BLK_DECON][DECON_INT_PENDING / 4] = DECON_INT_EXTRA;
	m.reg[SBD_BLK_DECON][DECON_EXTRA_INT_PENDING / 4] = DECON_XINT_TIMEOUT | DECON_XINT_CONFLICT;
	ev = sbd_decon_irq(&d);
	CHECK(ev == (SBD_EV_TIMEOUT | SBD_EV_CONFLICT));
	CHECK_EQ(d.timeout_irqs, 1);
	CHECK_EQ(d.conflict_irqs, 1);
	CHECK_EQ(m.reg[SBD_BLK_DECON][DECON_INT_PENDING / 4], 0);
	CHECK_EQ(m.reg[SBD_BLK_DECON][DECON_EXTRA_INT_PENDING / 4], 0);

	sbd_decon_irq_enable(&d, false);
	CHECK_EQ(m.reg[SBD_BLK_DECON][DECON_INT_ENABLE / 4] & (DECON_INT_EN | DECON_INT_EXTRA), 0);
}

static void
test_vsync_poll(void)
{
	uint32_t n = 99, ev;

	current = "vsync by polling";
	fresh();
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	CHECK_EQ(sbd_decon_attach_flip(&d), SBD_OK);
	m.reg[SBD_BLK_DECON][DECON_FRAME_COUNT / 4] = 0xfffffffeu;      /* about to wrap */
	ev = sbd_decon_poll(&d, &n);
	CHECK_EQ(n, 0);                                 /* the first call only takes the starting count */
	CHECK_EQ(ev, 0);
	mock_frame(&m);
	mock_frame(&m);
	mock_frame(&m);
	ev = sbd_decon_poll(&d, &n);
	CHECK_EQ(n, 3);                                 /* across the wrap */
	CHECK(ev & SBD_EV_VSYNC);
	CHECK_EQ(d.refreshes_polled, 3);
	CHECK_EQ(sbd_decon_flip(&d, 0x94000000, 5760), SBD_OK);
	ev = sbd_decon_poll(&d, &n);
	CHECK_EQ(n, 0);
	CHECK(!(ev & SBD_EV_LATCHED));
	mock_frame(&m);
	ev = sbd_decon_poll(&d, &n);
	CHECK_EQ(n, 1);
	CHECK(ev & SBD_EV_LATCHED);
	CHECK_EQ(d.flips_latched, 1);
	CHECK_EQ(d.vsync_irqs, 0);                      /* nothing claimed as an interrupt */
}

static void
test_detach(void)
{
	current = "detach";
	fresh();
	m.auto_frames = true;
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	CHECK_EQ(sbd_decon_attach_flip(&d), SBD_OK);
	CHECK_EQ(sbd_decon_flip(&d, 0x94000000, 5824), SBD_OK);
	mock_frame(&m);
	sbd_decon_detach(&d);
	CHECK_EQ(d.mode, SBD_MODE_BOOT);
	CHECK_EQ(m.active_base[SBD_CH_G0], 0xcc000000u);
	CHECK_EQ(m.reg[SBD_BLK_IDMA][(G0 + IDMA_SRC_SIZE) / 4], (2960u << 16) | 1440u);
	CHECK_EQ(d.shown_base, 0xcc000000u);
	CHECK(!d.pending);
}

static void
test_fit(void)
{
	struct sbd_rect r;

	current = "letterbox";
	sbd_decon_fit(828, 1792, 1440, 2960, &r);       /* iPhone 11 canvas: narrower than the panel */
	CHECK_EQ(r.w, 1366);
	CHECK_EQ(r.h, 2960);
	CHECK_EQ(r.x, 36);
	CHECK_EQ(r.y, 0);
	sbd_decon_fit(1440, 2960, 1440, 2960, &r);
	CHECK(r.x == 0 && r.y == 0 && r.w == 1440 && r.h == 2960);
	sbd_decon_fit(2960, 1440, 1440, 2960, &r);      /* a landscape canvas: bars above and below */
	CHECK_EQ(r.w, 1440);
	CHECK_EQ(r.h, 700);
	CHECK_EQ(r.y, 1130);
	CHECK(r.x + r.w <= 1440 && r.y + r.h <= 2960);
}

static void
test_scaler(void)
{
	int16_t c[9][4];
	struct sbd_rect dst;
	int idx_force, idx_path, idx_start, idx_end, idx_shadow, idx_base;

	current = "scaler coefficients";
	sbd_decon_scaler_coefficients(c);
	for (int t = 0; t < 9; t++) {
		CHECK_EQ(c[t][0] + c[t][1] + c[t][2] + c[t][3], 512);
	}
	CHECK(c[0][0] == 0 && c[0][1] == 512 && c[0][2] == 0 && c[0][3] == 0);         /* phase 0: the pixel itself */
	CHECK(c[8][0] == c[8][3] && c[8][1] == c[8][2]);                               /* half way: symmetric */
	CHECK(c[8][0] < 0);                                                            /* Catmull-Rom's negative lobe */

	current = "scaler attach";
	fresh();
	m.auto_frames = true;
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	sbd_decon_fit(828, 1792, 1440, 2960, &dst);
	CHECK_EQ(sbd_decon_attach_scaler(&d, 0x94000000, 828, 1792, 3328, &dst), SBD_OK);
	CHECK_EQ(d.mode, SBD_MODE_SCALER);
	CHECK_EQ(d.chan_type, SBD_CH_VGF0);
	/* VGF0's IDMA: the boot channel's format and IC_MAX only, this buffer's sizes and address */
	CHECK_EQ(m.reg[SBD_BLK_IDMA][(VGF0 + IDMA_IN_CON) / 4], 0x10u << 19);
	CHECK_EQ(m.reg[SBD_BLK_IDMA][(VGF0 + IDMA_SRC_SIZE) / 4], (1792u << 16) | 832u);
	CHECK_EQ(m.reg[SBD_BLK_IDMA][(VGF0 + IDMA_IMG_SIZE) / 4], (1792u << 16) | 828u);
	CHECK_EQ(m.reg[SBD_BLK_IDMA][(VGF0 + IDMA_BASE_Y) / 4], 0x94000000u);
	CHECK_EQ(m.reg[SBD_BLK_IDMA][(VGF0 + IDMA_BUS_CON) / 4], 0x22);
	CHECK_EQ(m.reg[SBD_BLK_IDMA][(VGF0 + IDMA_REQ_DEST) / 4], 0x44);
	CHECK(m.reg[SBD_BLK_IDMA][(VGF0 + IDMA_ENABLE) / 4] & IDMA_SFR_UPDATE_FORCE);
	CHECK(m.reg[SBD_BLK_DPP][(DPP_CH(SBD_CH_VGF0) + DPP_ENABLE) / 4] & DPP_SFR_UPDATE_FORCE);
	CHECK_EQ(m.reg[SBD_BLK_DPP][(DPP_CH(SBD_CH_VGF0) + DPP_IMG_SIZE) / 4], (1792u << 16) | 828u);
	CHECK_EQ(m.reg[SBD_BLK_DPP][(DPP_CH(SBD_CH_VGF0) + DPP_SCALED_SIZE) / 4], (2960u << 16) | 1366u);
	CHECK_EQ(m.reg[SBD_BLK_DPP][(DPP_CH(SBD_CH_VGF0) + DPP_H_RATIO) / 4], (uint32_t)(((uint64_t)828 << 20) / 1366));
	CHECK_EQ(m.reg[SBD_BLK_DPP][(DPP_CH(SBD_CH_VGF0) + DPP_V_RATIO) / 4], (uint32_t)(((uint64_t)1792 << 20) / 2960));
	CHECK_EQ(m.reg[SBD_BLK_DPP][(DPP_CH(SBD_CH_VGF0) + DPP_IN_CON) / 4] & DPP_IN_KEEP, 1u << 3);
	CHECK_EQ(m.reg[SBD_BLK_DPP][(DPP_CH(SBD_CH_VGF0) + DPP_H_COEF(0, 3, 0)) / 4], 512);    /* phase 0, centre tap */
	CHECK_EQ(m.reg[SBD_BLK_DPP][(DPP_CH(SBD_CH_VGF0) + DPP_V_COEF(0, 1, 1)) / 4], 512);
	/* the window: the letterbox rectangle, channel VGF0, taken by a frame */
	CHECK_EQ(m.active_win_start, 36u);
	CHECK_EQ(m.active_win_end, (2959u << 16) | (36u + 1366u - 1u));
	CHECK_EQ((m.active_path1 >> 20) & 7, SBD_PATH_CH_VGF0);
	/* order: the channel is fully set up and forced in before the window moves; one shadow request after */
	idx_force = mock_find(&m, 0, SBD_BLK_DPP, DPP_CH(SBD_CH_VGF0) + DPP_ENABLE);
	idx_base = mock_find(&m, 0, SBD_BLK_IDMA, VGF0 + IDMA_BASE_Y);
	idx_start = mock_find(&m, 0, SBD_BLK_DECON, DECON_WIN_START(5));
	idx_end = mock_find(&m, 0, SBD_BLK_DECON, DECON_WIN_END(5));
	idx_path = mock_find(&m, 0, SBD_BLK_DECON, DECON_DATA_PATH_1);
	idx_shadow = mock_find(&m, 0, SBD_BLK_DECON, DECON_SHADOW_REQ);
	CHECK(idx_base >= 0 && idx_base < idx_force);
	CHECK(idx_force < idx_start && idx_start < idx_end && idx_end < idx_path && idx_path < idx_shadow);
	/* the boot channel was not touched */
	CHECK(mock_find(&m, 0, SBD_BLK_IDMA, G0 + IDMA_BASE_Y) < 0);

	current = "scaler flip";
	mock_log_reset(&m);
	CHECK_EQ(sbd_decon_flip(&d, 0x95000000, 3328), SBD_OK);
	CHECK_EQ(m.nlog, 3);
	CHECK(m.log[0].block == SBD_BLK_IDMA && m.log[0].off == VGF0 + IDMA_BASE_Y);
	CHECK(m.log[2].block == SBD_BLK_DECON && m.log[2].off == DECON_SHADOW_REQ);
	CHECK_EQ(sbd_decon_flip(&d, 0x95000000, 3200), SBD_ERR_STRIDE);                /* 800 px < 828 */

	current = "scaler detach";
	sbd_decon_detach(&d);
	CHECK_EQ(m.active_win_start, 0);
	CHECK_EQ(m.active_win_end, (2959u << 16) | 1439u);
	CHECK_EQ((m.active_path1 >> 20) & 7, 5);
	CHECK_EQ(m.active_base[SBD_CH_G0], 0xcc000000u);

	current = "scaler refusals";
	fresh();
	m.auto_frames = true;
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	sbd_decon_fit(828, 1792, 1440, 2960, &dst);
	CHECK_EQ(sbd_decon_attach_scaler(&d, 0x880000000ull, 828, 1792, 3328, &dst), SBD_ERR_ADDRESS);
	CHECK_EQ(sbd_decon_attach_scaler(&d, 0x94000000, 4000, 1792, 16000, &dst), SBD_ERR_SIZE);     /* > 2x down */
	CHECK_EQ(sbd_decon_attach_scaler(&d, 0x94000000, 8, 8, 32, &dst), SBD_ERR_SIZE);
	CHECK_EQ(m.nlog, 0);
	m.reg[SBD_BLK_IDMA][(VGF0 + IDMA_ENABLE) / 4] = IDMA_OP_BUSY;
	CHECK_EQ(sbd_decon_attach_scaler(&d, 0x94000000, 828, 1792, 3328, &dst), SBD_ERR_BUSY);
	CHECK_EQ(m.nlog, 0);

	current = "scaler hardware error";
	fresh();
	m.auto_frames = true;
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	m.reg[SBD_BLK_IDMA][(VGF0 + IDMA_CFG_ERR) / 4] = 1u << 5;       /* the IDMA dislikes the width */
	CHECK_EQ(sbd_decon_attach_scaler(&d, 0x94000000, 828, 1792, 3328, &dst), SBD_ERR_HW);
	CHECK_EQ(d.mode, SBD_MODE_BOOT);                /* and the boot window is back */
	CHECK_EQ((m.active_path1 >> 20) & 7, 5);
	CHECK_EQ(m.active_win_end, (2959u << 16) | 1439u);
	CHECK_EQ(m.active_base[SBD_CH_G0], 0xcc000000u);

	current = "scaler without frames";
	fresh();                                        /* no auto frames: the shadow update is never taken */
	CHECK_EQ(sbd_decon_probe(&d, FB, 1440, 2960, 5760), SBD_OK);
	m.reg[SBD_BLK_DECON][DECON_TRIG_CONTROL / 4] = 0x1281;
	CHECK_EQ(sbd_decon_attach_scaler(&d, 0x94000000, 828, 1792, 3328, &dst), SBD_ERR_BUSY);
	CHECK_EQ(d.mode, SBD_MODE_BOOT);
	CHECK_EQ(m.reg[SBD_BLK_DECON][DECON_DATA_PATH_1 / 4], 5u << 20);                /* put back */
}

static struct sbd_panel panel;

static void
fresh_panel(void)
{
	struct sbd_io io;

	mock_init(&m);
	mock_boot_state(&m);
	io = mock_io(&m);
	sbd_panel_init(&panel, &io);
}

static bool
packet_is(unsigned i, uint8_t type, const uint8_t *bytes, uint32_t n)
{
	return i < m.npkt && m.pkt[i].type == type && m.pkt[i].len == n && memcmp(m.pkt[i].data, bytes, n) == 0;
}

static void
test_backlight(void)
{
	static const uint8_t key_on[] = { 0xf0, 0x5a, 0x5a }, commit[] = { 0xf7, 0x03 }, key_off[] = { 0xf0, 0xa5, 0xa5 };
	static const uint8_t aor_440[] = { 0xb1, 0x04, 0x40 }, aor_min[] = { 0xb1, 0x00, 0x0c }, aor_dim[] = { 0xb1, 0x0b, 0x76 };
	static const uint8_t aor_cap[] = { 0xb1, 0x0b, 0xa3 };
	int payload, header;

	current = "backlight DCS bytes";
	fresh_panel();
	CHECK_EQ(sbd_panel_set_aor(&panel, 0x0440), SBD_DSIM_OK);       /* S-Boot's normal level */
	CHECK_EQ(m.npkt, 4);
	CHECK(packet_is(0, DSI_DCS_LONG, key_on, 3));
	CHECK(packet_is(1, DSI_DCS_LONG, aor_440, 3));
	CHECK(packet_is(2, DSI_DCS_LONG, commit, 2));                   /* two bytes still go as a long write */
	CHECK(packet_is(3, DSI_DCS_LONG, key_off, 3));
	/* the register order of one packet: payload word, then the header with the word count */
	payload = mock_find(&m, 0, SBD_BLK_DSIM, DSIM_PAYLOAD);
	header = mock_find(&m, 0, SBD_BLK_DSIM, DSIM_PKTHDR);
	CHECK(payload >= 0 && header > payload);
	CHECK_EQ(m.log[payload].val, 0x005a5af0u);                      /* byte 0 lowest, zero padded */
	CHECK_EQ(m.log[header].val, 0x06000339u);                       /* 0x39, count 3, the upper bits kept */
	CHECK_EQ(panel.aor, 0x0440);

	current = "backlight levels";
	fresh_panel();
	CHECK_EQ(sbd_panel_set_brightness(&panel, 255), SBD_DSIM_OK);
	CHECK(packet_is(1, DSI_DCS_LONG, aor_min, 3));
	CHECK_EQ(sbd_panel_set_brightness(&panel, 0), SBD_DSIM_OK);
	CHECK(packet_is(5, DSI_DCS_LONG, aor_dim, 3));
	CHECK_EQ(panel.level, 0);
	CHECK_EQ(sbd_panel_set_aor(&panel, 0xffff), SBD_DSIM_OK);       /* clamped to S-Boot's darkest */
	CHECK(packet_is(9, DSI_DCS_LONG, aor_cap, 3));
	CHECK_EQ(sbd_panel_set_aor(&panel, 0), SBD_DSIM_OK);            /* and to the brightest */
	CHECK(packet_is(13, DSI_DCS_LONG, aor_min, 3));
	CHECK_EQ(sbd_panel_aor_for_level(-5), 0x0b76);
	CHECK_EQ(sbd_panel_aor_for_level(999), 0x000c);
	for (int l = 1; l <= 255; l++) {
		uint32_t a = sbd_panel_aor_for_level(l);

		CHECK(a <= sbd_panel_aor_for_level(l - 1));             /* brighter never dims */
		CHECK(a >= SBD_PANEL_AOR_MIN && a <= SBD_PANEL_AOR_DIMMEST);
	}

	current = "backlight when a frame holds the packet back";
	fresh_panel();
	m.tx_delay_polls = 95;                          /* ~9.5 ms: one frame on the link */
	CHECK_EQ(sbd_panel_set_aor(&panel, 0x0440), SBD_DSIM_OK);
	CHECK_EQ(m.npkt, 4);
	CHECK_EQ(panel.stats.timeouts, 0);

	current = "backlight retry";
	fresh_panel();
	m.fail_next_tx = true;                          /* the key packet times out once (S-Boot's log shows this) */
	CHECK_EQ(sbd_panel_set_aor(&panel, 0x0440), SBD_DSIM_OK);
	CHECK_EQ(panel.stats.timeouts, 1);
	/* first attempt: key on (timed out), key off anyway; second: the whole sequence */
	CHECK_EQ(m.npkt, 6);
	CHECK(packet_is(0, DSI_DCS_LONG, key_on, 3));
	CHECK(packet_is(1, DSI_DCS_LONG, key_off, 3));
	CHECK(packet_is(2, DSI_DCS_LONG, key_on, 3));
	CHECK(packet_is(3, DSI_DCS_LONG, aor_440, 3));
	CHECK(packet_is(4, DSI_DCS_LONG, commit, 2));
	CHECK(packet_is(5, DSI_DCS_LONG, key_off, 3));

	current = "backlight on a dead link";
	fresh_panel();
	m.tx_delay_polls = -1;                          /* nothing ever leaves */
	CHECK_EQ(sbd_panel_set_aor(&panel, 0x0440), SBD_DSIM_TIMEOUT);
	CHECK_EQ(panel.level, -1);
	CHECK(mock_find(&m, 0, SBD_BLK_DSIM, 0x04) < 0);                /* no software reset of the DSIM */

	fresh_panel();
	m.reg[SBD_BLK_DSIM][DSIM_CLK_CTRL / 4] = 0;     /* the link is not in HS: nothing is sent */
	CHECK_EQ(sbd_panel_set_aor(&panel, 0x0440), SBD_DSIM_NOT_READY);
	CHECK_EQ(m.nlog, 0);
	fresh_panel();
	m.reg[SBD_BLK_DSIM][DSIM_CONFIG / 4] |= DSIM_VIDEO_MODE;
	CHECK_EQ(sbd_panel_set_brightness(&panel, 128), SBD_DSIM_NOT_READY);
	CHECK_EQ(m.nlog, 0);
}

static void
test_panel_power(void)
{
	static const uint8_t k1_on[] = { 0x9f, 0xa5, 0xa5 }, k1_off[] = { 0x9f, 0x5a, 0x5a };
	uint8_t one[2];

	current = "display off / on";
	fresh_panel();
	CHECK_EQ(sbd_panel_display(&panel, false), SBD_DSIM_OK);
	CHECK_EQ(m.npkt, 3);
	CHECK(packet_is(0, DSI_DCS_LONG, k1_on, 3));
	one[0] = 0x28;
	one[1] = 0;
	CHECK(packet_is(1, DSI_DCS_SHORT, one, 2));     /* a short write: the command, no parameter */
	CHECK(packet_is(2, DSI_DCS_LONG, k1_off, 3));
	CHECK(panel.display_off);
	CHECK_EQ(sbd_panel_display(&panel, true), SBD_DSIM_OK);
	one[0] = 0x29;
	CHECK(packet_is(4, DSI_DCS_SHORT, one, 2));
	CHECK(!panel.display_off);

	current = "sleep in / out";
	fresh_panel();
	CHECK_EQ(sbd_panel_set_brightness(&panel, 200), SBD_DSIM_OK);
	m.npkt = 0;
	m.slept_us = 0;
	CHECK_EQ(sbd_panel_sleep(&panel, true), SBD_DSIM_OK);
	CHECK_EQ(m.npkt, 6);
	one[0] = 0x28;
	CHECK(packet_is(1, DSI_DCS_SHORT, one, 2));     /* display off before sleep in */
	one[0] = 0x10;
	CHECK(packet_is(4, DSI_DCS_SHORT, one, 2));
	CHECK(m.slept_us >= 120000);                    /* 20 ms + the vendor's 100 ms */
	CHECK(panel.asleep);
	m.npkt = 0;
	CHECK_EQ(sbd_panel_sleep(&panel, false), SBD_DSIM_OK);
	one[0] = 0x11;
	CHECK(packet_is(1, DSI_DCS_SHORT, one, 2));     /* sleep out first */
	CHECK_EQ(m.pkt[4].data[0], 0xb1);               /* then the brightness again */
	one[0] = 0x29;
	CHECK(packet_is(8, DSI_DCS_SHORT, one, 2));     /* display on last */
	CHECK(!panel.asleep && !panel.display_off);

	current = "panel allow-list";
	{
		static const uint8_t never[] = { 0xc0, 0xc1, 0xc8, 0xca, 0xb3, 0xb5, 0xf4, 0xfc, 0x01, 0x34, 0x53, 0xb0, 0xa1, 0x51 };
		static const uint8_t allowed[] = { 0xf0, 0x9f, 0xb1, 0xf7, 0x28, 0x29, 0x10, 0x11 };
		unsigned count = 0;

		for (unsigned i = 0; i < sizeof(never); i++) {
			CHECK(!sbd_panel_cmd_allowed(never[i]));
		}
		for (unsigned i = 0; i < sizeof(allowed); i++) {
			CHECK(sbd_panel_cmd_allowed(allowed[i]));
		}
		for (unsigned c = 0; c < 256; c++) {
			count += sbd_panel_cmd_allowed((uint8_t)c);
		}
		CHECK_EQ(count, sizeof(allowed));               /* nothing else slips through */
	}
}

static void
test_dsim_read(void)
{
	static const uint32_t long_answer[] = { 0x0000031c, 0x00440491 };      /* long read response, 3 bytes: 91 04 44 */
	static const uint32_t short_answer[] = { 0x00003421 }, error_answer[] = { 0x00800002 };
	uint8_t id[3] = { 0, 0, 0 }, buf[4];
	struct sbd_dsim_stats st;
	struct sbd_io io;

	current = "panel ID read";
	fresh_panel();
	mock_queue_rx(&m, long_answer, 2);
	CHECK_EQ(sbd_panel_read_id(&panel, id), SBD_DSIM_OK);
	CHECK(id[0] == 0x91 && id[1] == 0x04 && id[2] == 0x44);
	CHECK_EQ(m.npkt, 2);
	CHECK(m.pkt[0].type == DSI_SET_MAX_RETURN && m.pkt[0].data[0] == 3 && !m.pkt[0].bta);
	CHECK(m.pkt[1].type == DSI_DCS_READ && m.pkt[1].data[0] == 0x04 && m.pkt[1].bta);      /* bus turn-around */

	current = "dsim read variants";
	fresh_panel();
	io = mock_io(&m);
	memset(&st, 0, sizeof(st));
	mock_queue_rx(&m, short_answer, 1);
	CHECK_EQ(sbd_dsim_dcs_read(&io, 0x0a, buf, 1, &st), 1);
	CHECK_EQ(buf[0], 0x34);
	mock_queue_rx(&m, error_answer, 1);
	CHECK_EQ(sbd_dsim_dcs_read(&io, 0x0a, buf, 1, &st), SBD_DSIM_RX_ERROR);
	CHECK_EQ(st.rx_errors, 1);
	m.answer_reads = false;                         /* the panel stays silent */
	CHECK_EQ(sbd_dsim_dcs_read(&io, 0x0a, buf, 1, &st), SBD_DSIM_TIMEOUT);
	CHECK_EQ(sbd_dsim_dcs_read(&io, 0x0a, buf, 0, &st), SBD_DSIM_BAD_ARG);
	CHECK_EQ(sbd_dsim_dcs_write(&io, buf, 0, &st), SBD_DSIM_BAD_ARG);
}

static void
put32(uint8_t *rec, uint32_t off, uint32_t v)
{
	memcpy(rec + off, &v, 4);
}

static void
test_swap(void)
{
	uint8_t rec[SBD_SWAP_REC_MAX];
	struct sbd_swap s;

	current = "swap record, iOS 16 layout";
	memset(rec, 0, sizeof(rec));
	put32(rec, 0x50, 77);
	put32(rec, 0x54, 12);
	put32(rec, 0x58, 13);
	put32(rec, 0xc4 + 16, 100);                     /* layer 1 destination */
	put32(rec, 0xc4 + 20, 200);
	put32(rec, 0xc4 + 24, 64);
	put32(rec, 0xc4 + 28, 64);
	put32(rec, 0x104, 3);
	CHECK(sbd_swap_parse(rec, 0x46c, &s));
	CHECK_EQ(s.shift, 0);
	CHECK_EQ(s.swap_id, 77);
	CHECK_EQ(s.enabled, 2);
	CHECK(s.layer[0].enabled && s.layer[0].surface == 12);
	CHECK(s.layer[1].enabled && s.layer[1].dx == 100 && s.layer[1].dy == 200 && s.layer[1].dw == 64);
	CHECK(!sbd_swap_single_full_layer(&s, 828, 1792, 828, 1792));  /* two layers: not for one window */

	current = "swap record, iOS 26 / macOS 26.5 layout";
	memset(rec, 0, sizeof(rec));
	put32(rec, 0x98, 78);
	put32(rec, 0x9c, 21);
	put32(rec, 0x14c, 1);
	put32(rec, 0x10c + 8, 828);                     /* rect b of layer 0, 0x48 further in */
	put32(rec, 0x10c + 12, 1792);
	CHECK(sbd_swap_parse(rec, 0x588, &s));
	CHECK_EQ(s.shift, 0x48);
	CHECK_EQ(s.swap_id, 78);
	CHECK_EQ(s.enabled, 1);
	CHECK_EQ(s.layer[0].surface, 21);
	CHECK(sbd_swap_single_full_layer(&s, 828, 1792, 828, 1792));
	CHECK(!sbd_swap_single_full_layer(&s, 828, 1792, 1440, 2960)); /* the surface is not the canvas */
	put32(rec, 0x10c, 10);                          /* moved: not full */
	CHECK(sbd_swap_parse(rec, 0x588, &s));
	CHECK(!sbd_swap_single_full_layer(&s, 828, 1792, 828, 1792));

	current = "swap record, edge cases";
	memset(rec, 0, sizeof(rec));
	put32(rec, 0x50, 5);
	put32(rec, 0x54, 9);
	put32(rec, 0x104, 1);
	CHECK(sbd_swap_parse(rec, 0x468, &s));          /* empty rectangles: the whole surface */
	CHECK(sbd_swap_single_full_layer(&s, 1440, 2960, 1440, 2960));
	put32(rec, 0x104, 2);                           /* the mask names a layer with no surface */
	CHECK(sbd_swap_parse(rec, 0x468, &s));
	CHECK_EQ(s.enabled, 0);
	CHECK(!sbd_swap_parse(rec, 0x3ff, &s));         /* too short */
}

int
main(void)
{
	test_probe();
	test_flip_sequence();
	test_flip_masked_trigger();
	test_vsync_irq();
	test_vsync_poll();
	test_detach();
	test_fit();
	test_scaler();
	test_backlight();
	test_panel_power();
	test_dsim_read();
	test_swap();
	printf("StarbiteDisplay host tests: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
