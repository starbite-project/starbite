/* SPDX-License-Identifier: APSL-2.0 */
/*
 * DECON_F scan-out control (see sbd_decon.h). Register facts and their sources: NOTES.md.
 *
 * How a flip works on this hardware. The window's DMA channel (an IDMA block) holds the buffer's physical
 * address in IN_BASE_ADDR. Writes to it land in a shadow copy. SHADOW_REG_UPDATE_REQ asks the DECON to take
 * the shadow registers at the start of its next frame, which in command mode is the panel's next TE pulse
 * (HW trigger). The DECON clears the request when it has taken them. So: write the address, request the
 * update, and the new buffer is read from the next refresh on, never in the middle of one.
 */
#include "sbd_decon.h"
#include "sbd_regs.h"

#define DECON   SBD_BLK_DECON
#define IDMA    SBD_BLK_IDMA
#define DPP     SBD_BLK_DPP

/* DATA_PATH_CONTROL_1 channel number -> channel block (G0 G1 VG0 VG1 VGF0 VGF1), -1 = none */
static const int sbd_type_of_channel[8] = { SBD_CH_VG0, SBD_CH_VGF0, SBD_CH_VGF1, SBD_CH_G1, SBD_CH_VG1, SBD_CH_G0, -1, -1 };

static uint32_t
idma_rd(const struct sbd_decon *d, unsigned type, uint32_t reg)
{
	return sbd_rd(&d->io, IDMA, IDMA_CH(type) + reg);
}

static void
idma_wr(const struct sbd_decon *d, unsigned type, uint32_t reg, uint32_t val)
{
	sbd_wr(&d->io, IDMA, IDMA_CH(type) + reg, val);
}

static uint32_t
dpp_rd(const struct sbd_decon *d, unsigned type, uint32_t reg)
{
	return sbd_rd(&d->io, DPP, DPP_CH(type) + reg);
}

static void
dpp_wr(const struct sbd_decon *d, unsigned type, uint32_t reg, uint32_t val)
{
	sbd_wr(&d->io, DPP, DPP_CH(type) + reg, val);
}

void
sbd_decon_init(struct sbd_decon *d, const struct sbd_io *io)
{
	unsigned char *p = (unsigned char *)d;

	for (size_t i = 0; i < sizeof(*d); i++) {
		p[i] = 0;
	}
	d->io = *io;
	d->scan_limit = SBD_SCANOUT_LIMIT;
}

const char *
sbd_decon_strerror(int err)
{
	switch (err) {
	case SBD_OK:                  return "ok";
	case SBD_ERR_NOT_PROBED:      return "the DECON was not probed as expected";
	case SBD_ERR_GEOMETRY:        return "not the S9's 1440x2960x4 boot framebuffer";
	case SBD_ERR_WINDOWS:         return "not exactly one window on";
	case SBD_ERR_CHANNEL:         return "the window's channel cannot be used";
	case SBD_ERR_NOT_BOOT_BUFFER: return "the channel does not read the boot framebuffer";
	case SBD_ERR_NOT_RUNNING:     return "the DECON is not running in command mode";
	case SBD_ERR_ADDRESS:         return "buffer address above the limit or misaligned";
	case SBD_ERR_STRIDE:          return "row length not usable";
	case SBD_ERR_MODE:            return "wrong mode for this call";
	case SBD_ERR_BUSY:            return "a shadow update is pending or the channel is busy";
	case SBD_ERR_SIZE:            return "size outside the scaler's limits";
	case SBD_ERR_HW:              return "the hardware reported a configuration error";
	default:                      return "unknown";
	}
}

int
sbd_decon_probe(struct sbd_decon *d, uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes)
{
	uint32_t gc, path0, path1, on = 0;
	int type;

	if (d->probed != 0) {
		return d->probed > 0 ? SBD_OK : d->probe_err;
	}
	d->probed = -1;
	if (fb_phys == 0 || (fb_phys >> 32) != 0 || width != 1440 || height != 2960 || rowbytes != width * 4) {
		return d->probe_err = SBD_ERR_GEOMETRY;
	}
	gc = sbd_rd(&d->io, DECON, DECON_GLOBAL_CONTROL);
	path0 = sbd_rd(&d->io, DECON, DECON_DATA_PATH_0);
	path1 = sbd_rd(&d->io, DECON, DECON_DATA_PATH_1);
	for (unsigned w = 0; w < DECON_WINDOWS; w++) {
		if (path0 & (1u << (4 * w))) {
			on++;
			d->win = w;
		}
	}
	if (on != 1) {
		return d->probe_err = SBD_ERR_WINDOWS;
	}
	type = sbd_type_of_channel[(path1 >> (4 * d->win)) & 7];
	if (type < 0) {
		return d->probe_err = SBD_ERR_CHANNEL;
	}
	d->boot_type = (unsigned)type;
	d->boot_base = idma_rd(d, d->boot_type, IDMA_BASE_Y);
	d->boot_img_size = idma_rd(d, d->boot_type, IDMA_IMG_SIZE);
	d->boot_src_size = idma_rd(d, d->boot_type, IDMA_SRC_SIZE);
	d->boot_in_con = idma_rd(d, d->boot_type, IDMA_IN_CON);
	if (d->boot_base != (uint32_t)fb_phys || d->boot_img_size != ((height << 16) | width) ||
	    (d->boot_src_size & 0xffffu) * 4 != rowbytes) {
		return d->probe_err = SBD_ERR_NOT_BOOT_BUFFER;
	}
	if (!(gc & DECON_GC_CMD_MODE) || !(gc & DECON_GC_RUN)) {
		return d->probe_err = SBD_ERR_NOT_RUNNING;
	}
	d->boot_path1 = path1;
	d->boot_win_start = sbd_rd(&d->io, DECON, DECON_WIN_START(d->win));
	d->boot_win_end = sbd_rd(&d->io, DECON, DECON_WIN_END(d->win));
	d->boot_trig = sbd_rd(&d->io, DECON, DECON_TRIG_CONTROL);
	d->panel_w = width;
	d->panel_h = height;
	d->chan_type = d->boot_type;
	d->stride_px = d->boot_src_size & 0xffffu;
	d->src_w = width;
	d->src_h = height;
	d->shown_base = d->boot_base;
	d->mode = SBD_MODE_BOOT;
	d->probed = 1;
	d->probe_err = SBD_OK;
	return SBD_OK;
}

int
sbd_decon_buffer_ok(const struct sbd_decon *d, uint64_t phys, uint64_t bytes, uint32_t rowbytes)
{
	if (phys == 0 || (phys >> 32) != 0 || (phys & 0xffu) != 0 || bytes == 0 || phys + bytes > d->scan_limit) {
		return SBD_ERR_ADDRESS;
	}
	if ((rowbytes & 3u) != 0 || rowbytes / 4 < d->src_w || rowbytes / 4 > 0xffffu) {
		return SBD_ERR_STRIDE;
	}
	return SBD_OK;
}

int
sbd_decon_attach_flip(struct sbd_decon *d)
{
	if (d->probed <= 0) {
		return SBD_ERR_NOT_PROBED;
	}
	if (d->mode != SBD_MODE_BOOT) {
		return SBD_ERR_MODE;
	}
	d->mode = SBD_MODE_FLIP;
	d->chan_type = d->boot_type;
	d->src_w = d->panel_w;
	d->src_h = d->panel_h;
	return SBD_OK;
}

void
sbd_decon_trigger(struct sbd_decon *d, bool on)
{
	/* HW trigger: enable = EN set and MASK clear; disable = MASK set and EN clear (the vendor's pairing) */
	sbd_wr_mask(&d->io, DECON, DECON_TRIG_CONTROL, on ? DECON_HW_TRIG_EN : DECON_HW_TRIG_MASK,
	    DECON_HW_TRIG_EN | DECON_HW_TRIG_MASK);
}

static void
request_shadow_update(struct sbd_decon *d)
{
	sbd_wr(&d->io, DECON, DECON_SHADOW_REQ, sbd_rd(&d->io, DECON, DECON_SHADOW_REQ) | DECON_SHADOW_ALL);
}

int
sbd_decon_flip(struct sbd_decon *d, uint64_t phys, uint32_t rowbytes)
{
	uint32_t trig;
	int err;

	if (d->probed <= 0) {
		return SBD_ERR_NOT_PROBED;
	}
	if (d->mode == SBD_MODE_BOOT) {
		return SBD_ERR_MODE;
	}
	err = sbd_decon_buffer_ok(d, phys, (uint64_t)rowbytes * d->src_h, rowbytes);
	if (err != SBD_OK) {
		return err;
	}
	/* a masked or disabled trigger would never take the update: frames must be running first */
	trig = sbd_rd(&d->io, DECON, DECON_TRIG_CONTROL);
	if ((trig & DECON_HW_TRIG_MASK) || !(trig & DECON_HW_TRIG_EN)) {
		sbd_decon_trigger(d, true);
	}
	if (rowbytes / 4 != d->stride_px) {
		idma_wr(d, d->chan_type, IDMA_SRC_SIZE,
		    (idma_rd(d, d->chan_type, IDMA_SRC_SIZE) & 0xffff0000u) | (rowbytes / 4));
		d->stride_px = rowbytes / 4;
	}
	idma_wr(d, d->chan_type, IDMA_BASE_Y, (uint32_t)phys);
	idma_wr(d, d->chan_type, IDMA_BASE_C, (uint32_t)phys);
	request_shadow_update(d);
	if (d->pending) {
		d->flips_replaced++;
	}
	d->pending = true;
	d->pending_base = (uint32_t)phys;
	d->flips_requested++;
	return SBD_OK;
}

bool
sbd_decon_latched(struct sbd_decon *d)
{
	return (sbd_rd(&d->io, DECON, DECON_SHADOW_REQ) & DECON_SHADOW_ALL) == 0;
}

/* the pending flip, if the DECON has taken it */
static uint32_t
account_latch(struct sbd_decon *d)
{
	if (d->pending && sbd_decon_latched(d)) {
		d->pending = false;
		d->shown_base = d->pending_base;
		d->flips_latched++;
		return SBD_EV_LATCHED;
	}
	return 0;
}

static int
wait_latched(struct sbd_decon *d, unsigned ms)
{
	for (unsigned i = 0; i <= ms; i++) {
		if (sbd_decon_latched(d)) {
			return 0;
		}
		d->io.udelay(d->io.ctx, 1000);
	}
	return -1;
}

void
sbd_decon_irq_enable(struct sbd_decon *d, bool on)
{
	/* clear what is pending, so the first interrupt is a new frame's */
	sbd_wr(&d->io, DECON, DECON_INT_PENDING, DECON_INT_FRAME_DONE | DECON_INT_FRAME_START | DECON_INT_EXTRA);
	sbd_wr(&d->io, DECON, DECON_EXTRA_INT_PENDING, DECON_XINT_CONFLICT | DECON_XINT_TIMEOUT);
	if (on) {
		sbd_wr_mask(&d->io, DECON, DECON_INT_ENABLE,
		    DECON_INT_FRAME_DONE | DECON_INT_FRAME_START | DECON_INT_EXTRA | DECON_INT_EN, DECON_INT_ENABLE_MASK);
		sbd_wr(&d->io, DECON, DECON_EXTRA_INT_ENABLE, DECON_XINT_CONFLICT | DECON_XINT_TIMEOUT);
	} else {
		sbd_wr_mask(&d->io, DECON, DECON_INT_ENABLE, 0, DECON_INT_EXTRA | DECON_INT_EN);
	}
}

uint32_t
sbd_decon_irq(struct sbd_decon *d)
{
	uint32_t pend = sbd_rd(&d->io, DECON, DECON_INT_PENDING), ev = 0;

	if (pend & DECON_INT_FRAME_START) {
		sbd_wr(&d->io, DECON, DECON_INT_PENDING, DECON_INT_FRAME_START);
		d->vsync_irqs++;
		ev |= SBD_EV_VSYNC | account_latch(d);
	}
	if (pend & DECON_INT_FRAME_DONE) {
		sbd_wr(&d->io, DECON, DECON_INT_PENDING, DECON_INT_FRAME_DONE);
		d->frame_done_irqs++;
		ev |= SBD_EV_FRAME_DONE | account_latch(d);
	}
	if (pend & DECON_INT_EXTRA) {
		uint32_t extra;

		sbd_wr(&d->io, DECON, DECON_INT_PENDING, DECON_INT_EXTRA);
		extra = sbd_rd(&d->io, DECON, DECON_EXTRA_INT_PENDING);
		if (extra & DECON_XINT_CONFLICT) {
			sbd_wr(&d->io, DECON, DECON_EXTRA_INT_PENDING, DECON_XINT_CONFLICT);
			d->conflict_irqs++;
			ev |= SBD_EV_CONFLICT;
		}
		if (extra & DECON_XINT_TIMEOUT) {
			sbd_wr(&d->io, DECON, DECON_EXTRA_INT_PENDING, DECON_XINT_TIMEOUT);
			d->timeout_irqs++;
			ev |= SBD_EV_TIMEOUT;
		}
	}
	return ev;
}

uint32_t
sbd_decon_poll(struct sbd_decon *d, uint32_t *refreshes)
{
	uint32_t count = sbd_rd(&d->io, DECON, DECON_FRAME_COUNT), delta = 0;

	if (d->have_frame_count) {
		delta = count - d->last_frame_count;    /* wraps with the register */
	}
	d->have_frame_count = true;
	d->last_frame_count = count;
	d->refreshes_polled += delta;
	if (refreshes != NULL) {
		*refreshes = delta;
	}
	return account_latch(d) | (delta ? SBD_EV_VSYNC : 0);
}

void
sbd_decon_fit(uint32_t src_w, uint32_t src_h, uint32_t panel_w, uint32_t panel_h, struct sbd_rect *out)
{
	uint32_t w, h;

	if (src_w == 0 || src_h == 0) {
		out->x = out->y = 0;
		out->w = panel_w;
		out->h = panel_h;
		return;
	}
	w = (uint32_t)(((uint64_t)src_w * panel_h) / src_h);
	h = panel_h;
	if (w > panel_w) {
		w = panel_w;
		h = (uint32_t)(((uint64_t)src_h * panel_w) / src_w);
	}
	w &= ~1u;
	h &= ~1u;
	out->w = w;
	out->h = h;
	out->x = ((panel_w - w) / 2) & ~1u;
	out->y = ((panel_h - h) / 2) & ~1u;
}

/* Catmull-Rom weight of pixel k (-1 .. 2) for a point t/16 past pixel 0, in 1/512 */
static int
cubic_weight(int k, int t)
{
	int x = k * 16 - t, a = x < 0 ? -x : x;
	long long w;

	/* in 1/(2 * 16^3): |x| <= 1: 3|x|^3 - 5|x|^2 + 2;  1 < |x| < 2: -|x|^3 + 5|x|^2 - 8|x| + 4 */
	if (a <= 16) {
		w = 3LL * a * a * a - 5LL * 16 * a * a + 2LL * 16 * 16 * 16;
	} else if (a < 32) {
		w = -1LL * a * a * a + 5LL * 16 * a * a - 8LL * 16 * 16 * a + 4LL * 16 * 16 * 16;
	} else {
		w = 0;
	}
	return (int)((w * 512 + (w >= 0 ? 4096 : -4096)) / 8192);
}

void
sbd_decon_scaler_coefficients(int16_t table[9][4])
{
	for (int t = 0; t < 9; t++) {
		int sum = 0;

		for (int k = 0; k < 4; k++) {
			table[t][k] = (int16_t)cubic_weight(k - 1, t);
			sum += table[t][k];
		}
		/* the rounding remainder goes to the nearest pixel's tap */
		table[t][t < 8 ? 1 : 2] = (int16_t)(table[t][t < 8 ? 1 : 2] + (512 - sum));
	}
}

static void
setup_scaler(struct sbd_decon *d, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
	int16_t c[9][4];
	uint32_t hpos, vpos;

	sbd_decon_scaler_coefficients(c);
	for (unsigned bank = 0; bank < 2; bank++) {     /* luma and chroma banks, both written */
		for (unsigned phase = 0; phase < 9; phase++) {
			/* 8 taps across: the four used are taps 2..5; 4 taps down */
			for (unsigned tap = 0; tap < 8; tap++) {
				int v = (tap >= 2 && tap <= 5) ? c[phase][tap - 2] : 0;

				dpp_wr(d, SBD_CH_VGF0, DPP_H_COEF(phase, tap, bank), (uint32_t)v & 0x7ffu);
			}
			for (unsigned tap = 0; tap < 4; tap++) {
				dpp_wr(d, SBD_CH_VGF0, DPP_V_COEF(phase, tap, bank), (uint32_t)c[phase][tap] & 0x7ffu);
			}
		}
	}
	dpp_wr(d, SBD_CH_VGF0, DPP_H_RATIO, (uint32_t)(((uint64_t)sw << 20) / dw));
	dpp_wr(d, SBD_CH_VGF0, DPP_V_RATIO, (uint32_t)(((uint64_t)sh << 20) / dh));
	/* start positions: (source - destination) / (2 destination), 12.20, two's complement */
	hpos = (uint32_t)(int32_t)((((long long)sw - (long long)dw) * (1LL << 20)) / (2 * (long long)dw));
	vpos = (uint32_t)(int32_t)((((long long)sh - (long long)dh) * (1LL << 20)) / (2 * (long long)dh));
	dpp_wr(d, SBD_CH_VGF0, DPP_YH_POS, hpos);
	dpp_wr(d, SBD_CH_VGF0, DPP_YV_POS, vpos);
	dpp_wr(d, SBD_CH_VGF0, DPP_CH_POS, hpos);
	dpp_wr(d, SBD_CH_VGF0, DPP_CV_POS, vpos);
}

void
sbd_decon_detach(struct sbd_decon *d)
{
	if (d->probed <= 0 || d->mode == SBD_MODE_BOOT) {
		return;
	}
	if (d->mode == SBD_MODE_SCALER) {
		sbd_wr(&d->io, DECON, DECON_DATA_PATH_1, d->boot_path1);
		sbd_wr(&d->io, DECON, DECON_WIN_START(d->win), d->boot_win_start);
		sbd_wr(&d->io, DECON, DECON_WIN_END(d->win), d->boot_win_end);
	}
	idma_wr(d, d->boot_type, IDMA_SRC_SIZE, d->boot_src_size);
	idma_wr(d, d->boot_type, IDMA_BASE_Y, d->boot_base);
	idma_wr(d, d->boot_type, IDMA_BASE_C, d->boot_base);
	sbd_decon_trigger(d, true);
	request_shadow_update(d);
	(void)wait_latched(d, 100);
	d->mode = SBD_MODE_BOOT;
	d->chan_type = d->boot_type;
	d->stride_px = d->boot_src_size & 0xffffu;
	d->src_w = d->panel_w;
	d->src_h = d->panel_h;
	d->pending = false;
	d->shown_base = d->boot_base;
}

int
sbd_decon_attach_scaler(struct sbd_decon *d, uint64_t phys, uint32_t src_w, uint32_t src_h, uint32_t rowbytes,
    const struct sbd_rect *dst)
{
	uint32_t path1, frames, dpp_in;

	if (d->probed <= 0) {
		return SBD_ERR_NOT_PROBED;
	}
	if (d->mode != SBD_MODE_BOOT) {
		return SBD_ERR_MODE;
	}
	path1 = d->boot_path1;
	dpp_in = dpp_rd(d, d->boot_type, DPP_IN_CON);
	for (unsigned w = 0; w < DECON_WINDOWS; w++) {
		if (((path1 >> (4 * w)) & 7) == SBD_PATH_CH_VGF0 &&
		    (sbd_rd(&d->io, DECON, DECON_DATA_PATH_0) & (1u << (4 * w)))) {
			return SBD_ERR_CHANNEL;
		}
	}
	if (d->boot_type == SBD_CH_VGF0 ||
	    (d->boot_in_con & (IDMA_IN_ROT_FLIP_MASK | IDMA_IN_AFBC_MASK | IDMA_IN_BLOCK_EN)) || (dpp_in & 7u) != 0) {
		return SBD_ERR_CHANNEL;
	}
	/* the boot window must be the whole panel, with inclusive end coordinates (what the rectangle maths assumes) */
	if (d->boot_win_start != 0 || d->boot_win_end != (((d->panel_h - 1) << 16) | (d->panel_w - 1))) {
		return SBD_ERR_CHANNEL;
	}
	if (dst->w < 16 || dst->h < 16 || src_w < 16 || src_h < 16 || src_w > 4096 || src_h > 4096 ||
	    dst->x + dst->w > d->panel_w || dst->y + dst->h > d->panel_h ||
	    src_w > 2 * dst->w || src_h > 2 * dst->h || dst->w > 8 * src_w || dst->h > 8 * src_h) {
		return SBD_ERR_SIZE;
	}
	if ((phys >> 32) != 0 || (phys & 0xffu) != 0 || phys == 0 || phys + (uint64_t)rowbytes * src_h > d->scan_limit) {
		return SBD_ERR_ADDRESS;
	}
	if ((rowbytes & 3u) != 0 || rowbytes / 4 < src_w || rowbytes / 4 > 0xffffu) {
		return SBD_ERR_STRIDE;
	}
	if (idma_rd(d, SBD_CH_VGF0, IDMA_ENABLE) & IDMA_OP_BUSY) {
		return SBD_ERR_BUSY;
	}

	/* VGF0's IDMA: the boot channel's format and bus settings, this buffer and its sizes */
	idma_wr(d, SBD_CH_VGF0, IDMA_IN_CON, d->boot_in_con & (IDMA_IN_IC_MAX_MASK | IDMA_IN_FORMAT_MASK));
	idma_wr(d, SBD_CH_VGF0, IDMA_OUT_CON, idma_rd(d, d->boot_type, IDMA_OUT_CON));
	idma_wr(d, SBD_CH_VGF0, IDMA_SRC_SIZE, (src_h << 16) | (rowbytes / 4));
	idma_wr(d, SBD_CH_VGF0, IDMA_SRC_OFFSET, 0);
	idma_wr(d, SBD_CH_VGF0, IDMA_IMG_SIZE, (src_h << 16) | src_w);
	idma_wr(d, SBD_CH_VGF0, IDMA_BASE_Y, (uint32_t)phys);
	idma_wr(d, SBD_CH_VGF0, IDMA_BASE_C, (uint32_t)phys);
	idma_wr(d, SBD_CH_VGF0, IDMA_DEADLOCK_NUM, idma_rd(d, d->boot_type, IDMA_DEADLOCK_NUM));
	idma_wr(d, SBD_CH_VGF0, IDMA_BUS_CON, idma_rd(d, d->boot_type, IDMA_BUS_CON));
	idma_wr(d, SBD_CH_VGF0, IDMA_DYN_GATING, idma_rd(d, d->boot_type, IDMA_DYN_GATING));
	idma_wr(d, SBD_CH_VGF0, IDMA_REQ_DEST, idma_rd(d, d->boot_type, IDMA_REQ_DEST));
	/* VGF0's DPP: the boot channel's input format, the sizes, the scaler */
	dpp_wr(d, SBD_CH_VGF0, DPP_IN_CON, (dpp_rd(d, SBD_CH_VGF0, DPP_IN_CON) & ~DPP_IN_KEEP) | (dpp_in & DPP_IN_KEEP));
	dpp_wr(d, SBD_CH_VGF0, DPP_IMG_SIZE, (src_h << 16) | src_w);
	dpp_wr(d, SBD_CH_VGF0, DPP_SCALED_SIZE, (dst->h << 16) | dst->w);
	setup_scaler(d, src_w, src_h, dst->w, dst->h);
	dpp_wr(d, SBD_CH_VGF0, DPP_DYN_GATING, dpp_rd(d, d->boot_type, DPP_DYN_GATING));
	/* VGF0 is idle, so its registers are forced in now rather than at a frame of its own */
	idma_wr(d, SBD_CH_VGF0, IDMA_ENABLE, idma_rd(d, SBD_CH_VGF0, IDMA_ENABLE) | IDMA_SFR_UPDATE_FORCE);
	dpp_wr(d, SBD_CH_VGF0, DPP_ENABLE, dpp_rd(d, SBD_CH_VGF0, DPP_ENABLE) | DPP_SFR_UPDATE_FORCE);
	if (wait_latched(d, 100) != 0) {
		return SBD_ERR_BUSY;            /* nothing of the window was changed yet */
	}

	/* move the window: its rectangle, then its channel, taken together at the next frame */
	frames = sbd_rd(&d->io, DECON, DECON_FRAME_COUNT);
	d->mode = SBD_MODE_SCALER;
	d->chan_type = SBD_CH_VGF0;
	d->stride_px = rowbytes / 4;
	d->src_w = src_w;
	d->src_h = src_h;
	sbd_decon_trigger(d, true);
	sbd_wr(&d->io, DECON, DECON_WIN_START(d->win), (dst->y << 16) | dst->x);
	sbd_wr(&d->io, DECON, DECON_WIN_END(d->win), ((dst->y + dst->h - 1) << 16) | (dst->x + dst->w - 1));
	sbd_wr(&d->io, DECON, DECON_DATA_PATH_1,
	    (path1 & ~(7u << (4 * d->win))) | ((uint32_t)SBD_PATH_CH_VGF0 << (4 * d->win)));
	request_shadow_update(d);
	if (wait_latched(d, 100) != 0) {
		sbd_decon_detach(d);
		return SBD_ERR_BUSY;
	}
	/* three refreshes on the new channel, then its error state */
	for (unsigned ms = 0; ms < 100 && sbd_rd(&d->io, DECON, DECON_FRAME_COUNT) - frames < 3; ms++) {
		d->io.udelay(d->io.ctx, 1000);
	}
	if ((idma_rd(d, SBD_CH_VGF0, IDMA_CFG_ERR) & 0x7ffu) || (dpp_rd(d, SBD_CH_VGF0, DPP_CFG_ERR) & 0x1fu) ||
	    (idma_rd(d, SBD_CH_VGF0, IDMA_IRQ) & IDMA_IRQ_ERRORS) || (dpp_rd(d, SBD_CH_VGF0, DPP_IRQ) & DPP_IRQ_CONFIG_ERROR)) {
		sbd_decon_detach(d);
		return SBD_ERR_HW;
	}
	d->shown_base = (uint32_t)phys;
	return SBD_OK;
}
