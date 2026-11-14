/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the Galaxy S9's front camera (Samsung S5K3H1) from its supplies to a picture (cam.c), and the small
 * image pipeline that makes its raw frames into pixels (cam_isp.c). docs/drivers/CAMERA.md has the whole story.
 *
 * Use from the kernel (single-threaded, no locks; nothing here blocks for more than ~100 ms):
 *   cam_hal_set(&hal);                  once (the kernel: cam_hal_libsb() fills one from libsb)
 *   cam_open(&req);                     start the front camera's preview: 0 started (frames follow), < 0 refused
 *   cam_poll();                         every ~10 ms while cam_state() is not CAM_ST_OFF / CAM_ST_FAILED: it walks
 *                                       the power-up, then picks up each new frame (preview, exposure, still)
 *   cam_frame(&f);                      the newest preview picture (XRGB8888), 0 = none yet
 *   cam_still();                        ask for a still (the next good frame, 816 x 612 before rotation)
 *   cam_still_get(&s);                  1 when it is ready: a 24-bit BMP in RAM (pointer + length) and its XRGB
 *   cam_close();                        everything off in reverse order, pins and rails back as found
 *   cam_abort();                        the same at once (any state; the watchdog path); bounded spins only
 *   cam_why();                          the last refusal / failure / state in plain words, for the UI
 *
 * The probe (drvprobe.h): drv_cam_probe(level, &wait_ms), drv_cam_abort().
 */
#ifndef SBOS_CAM_H
#define SBOS_CAM_H
#include <stdint.h>
#include <stddef.h>

/* ---- the hardware access the driver is given ---------------------------------------------------------------------
 * Physical addresses. rd32 returns 0 when the read faulted (libsb's sb_probe32: an abort is logged, never taken);
 * wr32 completes before returning. i2c_init sets up one HSI2C controller (bus CAM_BUS_PMIC or CAM_BUS_SENSOR) with the
 * three timing words given (reset = 1: the controller's soft reset and the timing written, libsb sb_i2c_bus;
 * reset = 0: only remember the timing the controller already runs with, nothing written). i2c is one message
 * (libsb sb_i2c_msg): 1 done, 0 failed (no ack, or the controller never ran). ram() gives a CPU pointer to physical
 * memory that the DMA also sees (non-cacheable in libsb's map) or NULL. */
struct cam_hal {
    void *ctx;
    int (*rd32)(void *ctx, uint64_t pa, uint32_t *v);
    void (*wr32)(void *ctx, uint64_t pa, uint32_t v);
    void (*i2c_init)(void *ctx, unsigned bus, uint32_t fs1, uint32_t fs2, uint32_t fs3, int reset);
    int (*i2c)(void *ctx, unsigned bus, unsigned addr, int receive, uint8_t *bytes, unsigned count, int stop);
    int64_t (*smc)(void *ctx, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
    void (*delay_us)(void *ctx, uint32_t us);                       /* a spin: only short waits (<= 10 ms) */
    uint64_t (*now_us)(void *ctx);
    void (*log)(void *ctx, const char *line);                       /* one line, no newline */
    void *(*ram)(void *ctx, uint64_t pa, uint64_t len);
    /* optional: level 2's picture on the screen (NULL: not shown). XRGB8888, stride in pixels */
    void (*show)(void *ctx, const uint32_t *px, unsigned w, unsigned h, unsigned stride);
};
void cam_hal_set(const struct cam_hal *h);
#ifdef SBOS_KERNEL
void cam_hal_libsb(struct cam_hal *h);     /* sb_probe32, barriers, sb_i2c_*, sb_psci, sb_udelay, sb_us, sb_log;
                                            * show draws into S-Boot's scanout (SB_FB) at x 120, y 400 */
#endif

#define CAM_BUS_PMIC   0U                  /* hsi2c@10920000 (USI10, PERIC1): the S2MPB02 at 0x59 (also the LED's) */
#define CAM_BUS_SENSOR 1U                  /* hsi2c@108F0000 (USI08_I2C, PERIC1): the S5K3H1 at 0x35 */

/* ---- the interface ------------------------------------------------------------------------------------------------ */
#define CAM_FRONT 1U                       /* the only camera so far (the rear S5K2L3: not yet) */

/* the picture's orientation on the screen: applied to the sensor's landscape image (816 x 612 cells) */
#define CAM_ROT0   0U
#define CAM_ROT90  1U                      /* clockwise */
#define CAM_ROT180 2U
#define CAM_ROT270 3U
#define CAM_MIRROR 4U                      /* left-right, after the rotation (a selfie preview) */
/* the S9's front sensor sits at 270 degrees (Android's SENSOR_ORIENTATION for camera 1): a portrait selfie preview
 * is CAM_ROT270 | CAM_MIRROR (not yet seen on the phone: if it comes out upside down, CAM_ROT90 | CAM_MIRROR) */
#define CAM_PORTRAIT_SELFIE (CAM_ROT270 | CAM_MIRROR)

struct cam_req {
    unsigned facing;                       /* CAM_FRONT */
    unsigned w, h;                         /* the preview's size after the rotation; each at most 816, w x h x 4 at
                                            * most 2 MiB (e.g. 360 x 480 for a portrait 3:4 preview) */
    unsigned xform;                        /* CAM_ROT* | CAM_MIRROR */
    unsigned ae;                           /* 1 auto-exposure (default); 0 the fixed exposure below */
    unsigned exp_lines, again;             /* the first exposure: lines of 26.3 us (0: 1000) and the analog gain
                                            * (0x20 = 1x; 0: 0x40) */
};

struct cam_pic {
    const uint32_t *px;                    /* XRGB8888 (0xFFRRGGBB) */
    unsigned w, h, stride;                 /* stride in pixels */
    uint32_t seq;                          /* counts up with every new picture */
};

struct cam_shot {
    const uint8_t *bmp;                    /* a Windows BMP, 24 bits, bottom-up: the file's bytes as they are */
    uint32_t bmp_len;
    const uint32_t *px;                    /* the same picture, XRGB8888 */
    unsigned w, h, stride;
};

int cam_open(const struct cam_req *req);
int cam_poll(void);                        /* 1 running (or getting there), 0 off, < 0 failed (cam_why) */
int cam_frame(struct cam_pic *p);          /* 1 a picture, 0 none yet */
int cam_still(void);                       /* 0 asked, < 0 not running / one already pending */
int cam_still_get(struct cam_shot *s);     /* 1 ready (until the next cam_still / cam_close), 0 not (yet) */
void cam_close(void);
void cam_abort(void);
const char *cam_why(void);
int cam_state(void);                       /* CAM_ST_* */
uint32_t cam_fps_x10(void);                /* frames a second x 10 over the last ~2 s of the stream */
uint32_t cam_model_id(void);               /* the sensor's model ID as read (0: not read) */
int cam_flash_available(void);             /* 0: the front camera has no LED (use a white screen); see CAMERA.md */

#define CAM_ST_OFF      0
#define CAM_ST_STARTING 1                  /* powering up, ID, the ISP side, the mode */
#define CAM_ST_RUNNING  2                  /* streaming */
#define CAM_ST_STOPPING 3
#define CAM_ST_FAILED   4                  /* everything off again; cam_why() says why; cam_open() may try again */

#define CAM_ESTATE  -1                     /* wrong state (no HAL, already open, not running) */
#define CAM_EARG    -2                     /* the request: facing, size */
#define CAM_EBUS    -3                     /* an I2C bus could not be set up / no answer */
#define CAM_EPMIC   -4                     /* the S2MPB02: not the chip, a rail in someone else's use, a write */
#define CAM_ESENSOR -5                     /* the sensor did not answer / did not take a register */
#define CAM_EPOWER  -6                     /* the ISPPRE domain, its parent, the SMC */
#define CAM_EFRAME  -7                     /* no frame in time / DMA errors */
#define CAM_EMEM    -8                     /* the frames' memory is not mappable */
#define CAM_EABORT  -9

/* ---- the ISPPRE side lent to another CSIS port (kern/iris.c: CSIS3) ---------------------------------------------------
 * The same domain, clocks, D-PHY / CSIS / WDMA set-up and the same reverse-order way down as the front camera, for a
 * port other than CSIS1 (never while the camera is open; cam_open refuses while a port is up). A port's CSIS is not read
 * by cam_port_up (its owner reads it fault-safe first). Every write still goes through cam.c's allow-list, which then
 * names the port's CSIS / D-PHY / WDMA blocks instead of CSIS1's. */
struct cam_port {
    const char *name;
    uint64_t csis, wdma, dphy;             /* the CSIS, its VC0 WDMA (its DMA common at +0x400), its D-PHY's registers */
    uint32_t dphy_reset;                   /* its D-PHY's reset bit in SYSREG_ISPPRE 0x500 */
    uint32_t settle;                       /* HSSETTLECTL (fimc-is-hw-settle-10nm-lpp.h) */
    uint32_t skew;                         /* 0: below 1.5 Gb/s (HS_MODE_SEL 1, no skew calibration); 1..3 SKEW_DLYSEL
                                            * with SKEWCAL_EN 1, HS_MODE_SEL 0 (csi_hw_s_phy_config) */
    uint32_t dt;                           /* VC0's CSI-2 data type: 0x2B RAW10 (16 bits a pixel), 0x2A RAW8 (packet) */
    unsigned w, h;
};
int cam_port_up(const struct cam_port *p);  /* 1: ISPPRE (+ the DTZPC restore SMC) and its clocks up for p; 0: refused
                                             * or failed, everything put back (cam_why) */
int cam_port_link(uint32_t buf_pa);         /* 1: the port's D-PHY, CSIS and VC0 WDMA set up, every frame pointer at
                                             * buf_pa (32-bit, the DMA's view); not yet enabled */
void cam_port_go(void);                     /* the link enabled (the sensor's stream may start 8 ms later) */
uint32_t cam_port_dma_src(void);            /* the WDMA's interrupt source, read and cleared: bit 4 frame start, bit 8
                                             * frame end, bit 12 error */
void cam_port_diag(uint32_t d[6]);          /* CSIS INT_SRC0, INT_SRC1, FRM_CNT_CH0, PHY_STATUS; WDMA ERR, ACT_CTRL */
void cam_port_dma_off(void);                /* the WDMA stopped (the frame in flight dropped) */
void cam_port_down(void);                   /* the WDMA, the link, the D-PHY, the domain: off in that order; the port
                                             * back to CSIS1 */

/* ---- the probe ---------------------------------------------------------------------------------------------------- */
int drv_cam_probe(int level, unsigned *wait_ms);
void drv_cam_abort(void);
#define CAM_PROBE_BUDGET_MS 20000U         /* level 2's 10 s preview + the still + the way down; level 0 ~0.2 s,
                                            * level 1 ~2 s on a good run (each level also stops itself in time) */

/* ---- the image pipeline (cam_isp.c, pure C) ---------------------------------------------------------------------- */
#define CAM_RAW16  0U                      /* RAW10 in 16 bits a pixel, little-endian, [9:0] (CSIS1's WDMA, PACK12 off) */
#define CAM_RAW10P 1U                      /* MIPI RAW10 packed: 4 pixels in 5 bytes */
#define CAM_BAYER_RGGB 0U
#define CAM_BAYER_GRBG 1U                  /* the 3H1's (Samsung's fimc-is-cis-3h1.c: GR_BG) */
#define CAM_BAYER_GBRG 2U
#define CAM_BAYER_BGGR 3U

struct cam_raw {
    const volatile uint8_t *p;
    unsigned w, h, stride;                 /* pixels, pixels, bytes a line */
    unsigned fmt, bayer, black;            /* CAM_RAW16 / CAM_RAW10P, CAM_BAYER_*, the black level (10-bit) */
};
struct cam_stats {
    uint32_t mean[4];                      /* R, Gr, Gb, B (10-bit, black level NOT taken off) */
    uint32_t min, max;                     /* over every sample read */
    uint32_t over;                         /* samples above 10 bits (16-bit format only: not a RAW10 value) */
    uint32_t clip;                         /* green samples at or above 1000 */
    uint32_t sd_g;                         /* the greens' standard deviation (10-bit) */
    uint32_t n;                            /* 2 x 2 cells read */
};
struct cam_wb {
    uint32_t r, g, b;                      /* gains, 256 = 1x */
};
struct cam_out {
    uint32_t *px;                          /* XRGB8888 */
    unsigned w, h, stride;                 /* stride in pixels */
    unsigned xform;                        /* CAM_ROT* | CAM_MIRROR */
};

/* every step-th 2 x 2 cell across and down (step >= 1) */
void cam_isp_stats(const struct cam_raw *r, unsigned step, struct cam_stats *s);
/* grey world from the stats: R and B gains to make their means the greens', within 0.5x..4x; G 1x */
void cam_isp_wb_grey(const struct cam_stats *s, unsigned black, struct cam_wb *wb);
/* rows [y0, y1) of the output: each output pixel is one 2 x 2 Bayer cell (R, the greens' mean, B), the cell grid
 * scaled to the output's size after the rotation (nearest), black level off, white balance, a gamma of 2 (square
 * root), clipped to 8 bits */
void cam_isp_render(const struct cam_raw *r, const struct cam_wb *wb, const struct cam_out *o, unsigned y0, unsigned y1);
/* the BMP file of an XRGB picture: its length, or 0 if cap is too small */
uint32_t cam_isp_bmp(const uint32_t *px, unsigned w, unsigned h, unsigned stride, uint8_t *dst, uint32_t cap);
uint32_t cam_isp_bmp_size(unsigned w, unsigned h);
/* one sample (10-bit; 16-bit format: the whole 16-bit word, so a fill shows as > 1023) */
uint32_t cam_isp_px(const struct cam_raw *r, unsigned x, unsigned y);

/* ---- where things are (physical; this phone's device tree and Samsung's GPL fimc-is2 driver, see cam.c) ---------- */
/* the frames' memory: 32 MiB in the lower bank's gap between the panel's buffers and secure_camera (0xE1900000), below
 * 4 GiB (the WDMA's addresses are 32 bits), in libsb's non-cacheable window, used by nothing else in Starbite OS */
#define CAM_MEM_PA      0xD8000000ULL
#define CAM_MEM_SIZE    0x2000000ULL
#define CAM_SLOT_BYTES  0x400000U           /* a raw frame: 1632 x 1224 x 2 = 3995136 bytes */
#define CAM_RING        4U                  /* slots 0..3 the WDMA's ring, slot 4 the stand-in while one is held */
#define CAM_PREVIEW_OFF 0x1400000U          /* 2 MiB: the preview picture (XRGB) */
#define CAM_STILLPX_OFF 0x1600000U          /* 2 MiB: the still's picture (XRGB) */
#define CAM_BMP_OFF     0x1800000U          /* 2 MiB: the still's BMP */
#define CAM_PIC_BYTES   0x200000U
#define CAM_FRAME_W     1632U
#define CAM_FRAME_H     1224U
#define CAM_LINE_BYTES  (CAM_FRAME_W * 2U)
#define CAM_BLACK       64U                 /* the 3H1's black level in 10 bits */
#define CAM_FILL        0xA5U               /* what the slots are filled with: 0xA5A5 is no RAW10 value */

#endif
