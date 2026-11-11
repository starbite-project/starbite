/* SPDX-License-Identifier: APSL-2.0 */
/*
 * DECON_F scan-out control for the Galaxy S9 (Exynos 9810): address flips on the window S-Boot left on,
 * an optional move of that window to the VGF0 channel (hardware scaler), the frame interrupt model and
 * refresh accounting. Plain C, no IOKit: shared by StarbiteDisplay.cpp and test/.
 *
 * The flip and scaler sequences follow what an earlier port's public driver does on this phone
 * (refport-kernel drivers/display/dpu/exynos_dpu.c, APSL-2.0, an earlier port; credited in NOTES.md).
 * This file is written anew around a testable register back end.
 */
#ifndef SBD_DECON_H
#define SBD_DECON_H

#include "sbd_io.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sbd_decon_mode {
	SBD_MODE_BOOT   = 0,    /* S-Boot's window and buffer: nothing of ours written */
	SBD_MODE_FLIP   = 1,    /* address flips on S-Boot's channel, buffers of the panel's size */
	SBD_MODE_SCALER = 2     /* window on VGF0: smaller buffers scaled to a rectangle of the panel */
};

/* why a call was refused (negative returns) */
enum sbd_decon_err {
	SBD_OK                  = 0,
	SBD_ERR_NOT_PROBED      = -1,
	SBD_ERR_GEOMETRY        = -2,   /* not the 1440 x 2960 x 4 boot buffer we know */
	SBD_ERR_WINDOWS         = -3,   /* not exactly one window on */
	SBD_ERR_CHANNEL         = -4,   /* the window's channel is no IDMA's, or VGF0 is taken */
	SBD_ERR_NOT_BOOT_BUFFER = -5,   /* the channel does not read the boot framebuffer */
	SBD_ERR_NOT_RUNNING     = -6,   /* not command mode, or not running */
	SBD_ERR_ADDRESS         = -7,   /* above 4 GiB, above the scan-out limit, or misaligned */
	SBD_ERR_STRIDE          = -8,
	SBD_ERR_MODE            = -9,
	SBD_ERR_BUSY            = -10,  /* a shadow update is still pending, or the channel is busy */
	SBD_ERR_SIZE            = -11,  /* outside the scaler's limits */
	SBD_ERR_HW              = -12   /* the hardware reported a configuration error */
};

/* events returned by sbd_decon_irq() / sbd_decon_poll() */
#define SBD_EV_VSYNC            (1u << 0)       /* a frame started (one refresh) */
#define SBD_EV_FRAME_DONE       (1u << 1)
#define SBD_EV_LATCHED          (1u << 2)       /* the pending flip was taken: shown_base changed */
#define SBD_EV_TIMEOUT          (1u << 3)
#define SBD_EV_CONFLICT         (1u << 4)

/* an earlier port's observation on this phone: nothing drawn from 0xE0000000 up reaches the panel ([S], NOTES.md). */
#define SBD_SCANOUT_LIMIT       0xe0000000ull

struct sbd_rect {
	uint32_t x, y, w, h;
};

struct sbd_decon {
	struct sbd_io io;
	int      probed;                /* 0 not yet, 1 as expected, -1 refused (nothing was written) */
	int      probe_err;
	enum sbd_decon_mode mode;
	uint32_t panel_w, panel_h;
	uint64_t scan_limit;

	/* what S-Boot left, read at probe and put back by sbd_decon_detach() */
	unsigned win;                   /* the one window that is on */
	unsigned boot_type;             /* its channel block */
	uint32_t boot_base, boot_path1, boot_src_size, boot_img_size, boot_in_con;
	uint32_t boot_win_start, boot_win_end, boot_trig;

	/* the channel flips go to now */
	unsigned chan_type;
	uint32_t stride_px;             /* what IDMA_SRC_SIZE holds */
	uint32_t src_w, src_h;          /* the buffers' size in this mode */

	/* flips */
	bool     pending;               /* a base was written and its shadow update not yet seen taken */
	uint32_t pending_base;
	uint32_t shown_base;            /* what the DECON reads now */
	uint64_t flips_requested;
	uint64_t flips_latched;         /* "frame flips by address" */
	uint64_t flips_replaced;        /* a newer flip overwrote one the DECON had not taken */

	/* refresh accounting */
	uint64_t vsync_irqs;            /* frame-start interrupts: "vsync IRQ counted" */
	uint64_t frame_done_irqs;
	uint64_t timeout_irqs, conflict_irqs;
	uint64_t refreshes_polled;      /* from FRAME_COUNT, the timer's path */
	uint32_t last_frame_count;
	bool     have_frame_count;
};

void sbd_decon_init(struct sbd_decon *d, const struct sbd_io *io);

/* Reads only. 0 when the DECON is in the state S-Boot and our loader leave it in. */
int  sbd_decon_probe(struct sbd_decon *d, uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes);

/* Is this buffer one the IDMA can read? (physical, contiguous is the caller's check) */
int  sbd_decon_buffer_ok(const struct sbd_decon *d, uint64_t phys, uint64_t bytes, uint32_t rowbytes);

/* Mode 1: flips on S-Boot's channel. Writes nothing by itself. */
int  sbd_decon_attach_flip(struct sbd_decon *d);

/*
 * Mode 2: the window moves to VGF0, which reads src_w x src_h (rowbytes a row) at phys and scales it to
 * `dst` on the panel. Sleeps (io.udelay). On any error the boot window is put back.
 */
int  sbd_decon_attach_scaler(struct sbd_decon *d, uint64_t phys, uint32_t src_w, uint32_t src_h,
    uint32_t rowbytes, const struct sbd_rect *dst);

/* Show `phys` from the next refresh on. May be called again before the last flip was taken. */
int  sbd_decon_flip(struct sbd_decon *d, uint64_t phys, uint32_t rowbytes);

/* Has every requested shadow update been taken? */
bool sbd_decon_latched(struct sbd_decon *d);

/* Back to S-Boot's channel, window and buffer (the kernel console and a panic draw there). */
void sbd_decon_detach(struct sbd_decon *d);

/* The TE trigger: on = a frame at every TE; off = masked, the panel shows its own memory. */
void sbd_decon_trigger(struct sbd_decon *d, bool on);

/* Frame-start, frame-done and extra interrupts on or off (pending ones are cleared first). */
void sbd_decon_irq_enable(struct sbd_decon *d, bool on);

/* The interrupt handler's register work: read, clear, count. Returns SBD_EV_*. No sleeping. */
uint32_t sbd_decon_irq(struct sbd_decon *d);

/* Without interrupts: refreshes since the last call from FRAME_COUNT (`*refreshes`), and SBD_EV_LATCHED. */
uint32_t sbd_decon_poll(struct sbd_decon *d, uint32_t *refreshes);

/* The letterbox: src scaled to the largest rectangle of the panel with its aspect, centred, even sizes. */
void sbd_decon_fit(uint32_t src_w, uint32_t src_h, uint32_t panel_w, uint32_t panel_h, struct sbd_rect *out);

/* Catmull-Rom weight table for the scaler (nine phases, four taps, units of 1/512; each row sums to 512). */
void sbd_decon_scaler_coefficients(int16_t table[9][4]);

const char *sbd_decon_strerror(int err);

#ifdef __cplusplus
}
#endif
#endif /* SBD_DECON_H */
