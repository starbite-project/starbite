/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite display core: DECON scanout flips and S6E3HA8 brightness over DSIM, without any OS
 * calls; shared by sbdisplay.sys and the host test. docs/WIN11-GPU.md section 7.
 *
 * Sources (all proven on this phone): the DECON flip handshake and the live-window identification
 * of our Linux driver tools/linux/port/src/gpu/drm/s9p/s9p-decon.c (GPL-2.0), the AOR brightness
 * sequence of shim/starlte/em.c panel_set_aor (S-Boot's own sequence, build/em-research-brightness.md).
 *
 * What may be written, and nothing else:
 *   DECON  0x060 SHADOW_UPDATE_REQ (the live window's bit only), 0x070 HW_SW_TRIG_CONTROL
 *          (read-modify-write of bit 0 HW_TRIG_EN and bit 4 HW_TRIG_MASK_DECON),
 *          0x040 INTERRUPT_ENABLE (RMW of bits 0 and 12), 0x04c INTERRUPT_PENDING (W1C)
 *   DMA    IDMA channel 0 IN_BASE_ADDR_Y (+0x40) and the word after it (+0x44), as s9p-decon
 *   DSIM   0x050 INTSRC (W1C of bits 28, 18), 0x058 PKTHDR, 0x05c PAYLOAD
 * Only IDMA channel 0 is ever read: a read of a clock-gated channel wedges the SoC (measured).
 */
#ifndef SBDC_CORE_H
#define SBDC_CORE_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef UINT32 sbdc_u32;
typedef UINT8 sbdc_u8;
#else
#include <stdint.h>
typedef uint32_t sbdc_u32;
typedef uint8_t sbdc_u8;
#endif

/* Blocks (physical bases as in the ACPI device, docs/WIN11-GPU.md 4.2). */
enum { SBDC_DECON = 0, SBDC_DMA = 1, SBDC_DSIM = 2, SBDC_BLOCKS = 3 };
#define SBDC_DECON_PA 0x16030000ULL
#define SBDC_DECON_SIZE 0x10000U
#define SBDC_DMA_PA 0x16070000ULL
#define SBDC_DMA_SIZE 0x7000U
#define SBDC_DSIM_PA 0x16080000ULL
#define SBDC_DSIM_SIZE 0x100U

/* DECON0 */
#define SBDC_INT_EN 0x040U
#define SBDC_INT_EN_ON (1U << 0)
#define SBDC_INT_EN_FRAME_START (1U << 12)
#define SBDC_INT_PEND 0x04cU
#define SBDC_INT_PEND_FRAME_START (1U << 12)
#define SBDC_INT_PEND_FRAME_DONE (1U << 13)
#define SBDC_INT_PEND_EXTRA (1U << 4)
#define SBDC_SHADOW_REQ 0x060U
#define SBDC_TRIG 0x070U
#define SBDC_TRIG_HW_EN (1U << 0)
#define SBDC_TRIG_HW_MASK (1U << 4)
#define SBDC_PATH0 0x214U                 /* window enables, bit 4 * win */
#define SBDC_PATH1 0x218U                 /* channel map nibbles (a DMA type, not the channel) */
#define SBDC_WIN_START(w) (0x1008U + (w) * 0x30U)
#define SBDC_WIN_END(w) (0x100cU + (w) * 0x30U)
#define SBDC_MAX_WIN 6U

/* DPU_DMA: IDMA channel 0 at +0x1000; the shadow (active) bank at +0x800 inside it. */
#define SBDC_CH0 0x1000U
#define SBDC_IDMA_IMG_SIZE 0x018U         /* height << 16 | width */
#define SBDC_IDMA_BASE_Y 0x040U
#define SBDC_IDMA_SHADOW 0x800U

/* DSIM0 */
#define SBDC_DSIM_CLK_CTRL 0x020U         /* [20] TX_REQUEST_HSCLK: link in HS (S-Boot left it running) */
#define SBDC_DSIM_CONFIG 0x04cU           /* [18] VIDEO_MODE: must be 0 (command mode) */
#define SBDC_DSIM_INTSRC 0x050U
#define SBDC_DSIM_PH_EMPTY (1U << 28)
#define SBDC_DSIM_PKTHDR 0x058U
#define SBDC_DSIM_PAYLOAD 0x05cU
#define SBDC_DSIM_FIFOCTRL 0x068U         /* read only here: [21:16] headers queued, [10] header FIFO empty */

/* The panel. */
#define SBDC_PANEL_W 1440U
#define SBDC_PANEL_H 2960U
#define SBDC_AOR_MIN 0x000cU              /* brightest the vendor uses */
#define SBDC_AOR_MAX 0x0ba3U              /* S-Boot's darkest */

struct sbdc_io {
    void *ctx;
    sbdc_u32 (*rd)(void *ctx, unsigned block, unsigned off);
    void (*wr)(void *ctx, unsigned block, unsigned off, sbdc_u32 v);   /* ordered: barrier inside */
    void (*delay_us)(void *ctx, unsigned us);
};

struct sbdc {
    const struct sbdc_io *io;
    unsigned win;                         /* the window S-Boot scans out through */
    unsigned w, h;
    sbdc_u32 fb_pa;                       /* the firmware framebuffer (S-Boot's 0xCC000000) */
    sbdc_u32 trig_inherited;              /* HW_SW_TRIG_CONTROL as found */
    sbdc_u32 cur_pa;                      /* what channel 0 was last pointed at */
    unsigned flips, latch_timeouts;
};

/* Identify S-Boot's live pipeline: an enabled full-screen window and IDMA channel 0 scanning fb_pa
 * with the panel's size. 0 = found; < 0 = not as expected (then nothing may be written). */
int sbdc_identify(struct sbdc *d, const struct sbdc_io *io, sbdc_u32 fb_pa, unsigned w, unsigned h);

/* 1 while a requested flip has not been latched by a TE edge yet. */
int sbdc_flip_pending(const struct sbdc *d);

/* Wait (polling every step_us) until no flip is pending; 0 = idle, -1 = timeout. */
int sbdc_wait_latched(struct sbdc *d, unsigned timeout_us, unsigned step_us);

/* Point channel 0 at pa (32-bit, page aligned) and request the shadow update for the next TE.
 * -1 if a previous flip is still pending (call sbdc_wait_latched first) or pa is invalid. */
int sbdc_flip(struct sbdc *d, sbdc_u32 pa);

/* Frame-start interrupt enable (read-modify-write) and acknowledge; the ISR's view of a latch. */
void sbdc_frame_irq(struct sbdc *d, int on);
sbdc_u32 sbdc_irq_ack(struct sbdc *d);

/* ---- dirty rectangles -------------------------------------------------------------------- */

struct sbdc_rect {
    int l, t, r, b;                       /* r, b exclusive */
};

#define SBDC_MAX_RECTS 32U

struct sbdc_rects {
    unsigned n;
    struct sbdc_rect r[SBDC_MAX_RECTS];
};

void sbdc_rects_clear(struct sbdc_rects *s);
void sbdc_rects_full(struct sbdc_rects *s, unsigned w, unsigned h);
/* Add a rectangle clipped to w x h; beyond SBDC_MAX_RECTS the set becomes its bounding box. */
void sbdc_rects_add(struct sbdc_rects *s, struct sbdc_rect r, unsigned w, unsigned h);
unsigned long long sbdc_rects_area(const struct sbdc_rects *s);

/* Double buffering: buffer 0 = the firmware framebuffer, buffer 1 = ours. stale[i] = what changed
 * since buffer i was last brought up to date. A present adds its rectangles to both sets, copies
 * stale[back] into the back buffer, then flips to it. */
struct sbdc_swap {
    sbdc_u32 pa[2];
    int front;                            /* the buffer DECON scans (or will at the next TE) */
    struct sbdc_rects stale[2];
};

void sbdc_swap_init(struct sbdc_swap *s, sbdc_u32 fb_pa, sbdc_u32 second_pa, unsigned w, unsigned h);
/* Record one present's rectangles (dirty and move destinations). */
void sbdc_swap_damage(struct sbdc_swap *s, const struct sbdc_rect *r, unsigned n, unsigned w, unsigned h);
/* The buffer to draw into next, and the rectangles it needs. */
int sbdc_swap_back(const struct sbdc_swap *s);
/* After the back buffer was brought up to date and flipped: it is the front now. */
void sbdc_swap_flipped(struct sbdc_swap *s);

/* ---- scaled copies (WG1 in software: Windows draws w/scale x h/scale, the panel shows it doubled) -------- */

/* Copy rectangle r (in SOURCE pixels, already clipped to the source) of a 32-bit image into dst, each
 * source pixel becoming a scale x scale block (scale 1 or 2). Never reads dst (write-combined memory is
 * slow to read). */
void sbdc_blit(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
               unsigned scale);

/* ---- modes, rotation and non-integer scales (T9) -------------------------------------------------------- */

/* Rotation as D3DKMDT_VIDPN_PRESENT_PATH_ROTATION's first values, so the driver passes the path's value through. */
#define SBDC_ROT_0 1U
#define SBDC_ROT_90 2U
#define SBDC_ROT_180 3U
#define SBDC_ROT_270 4U

/* How Windows' image reaches the panel. The mode (lw x lh) is in the panel's orientation; Windows presents
 * its image rotated (rot 90/270: lh x lw, as Microsoft's KMDOD sample). Scale: panel pixel X shows mode pixel
 * (X * den) >> shift, i.e. panel/mode = 2^shift/den: (1,0) native, (3,2) 4/3, (1,1) 2. Nearest neighbour. */
struct sbdc_xf {
    unsigned lw, lh;
    unsigned pw, ph;
    unsigned rot;
    unsigned den, shift;
    /* Optional, for rotated copies (T10b): a cached CPU buffer of SBDC_SCRATCH_ROWS x pw pixels. Rows are
     * gathered there (reads stay in a few source cache lines) and then written to the panel sequentially,
     * which write-combined memory needs. NULL = the tiled path. */
    sbdc_u32 *scratch;
};
#define SBDC_SCRATCH_ROWS 8U

/* The size of the image Windows presents for this transform. */
void sbdc_xf_src_size(const struct sbdc_xf *x, unsigned *w, unsigned *h);

/* Copy rectangle r (in the presented image's pixels; clipped here) into the panel buffer, rotated and scaled.
 * Writes exactly the panel pixels whose mode pixel comes from inside r; never reads dst. */
void sbdc_blit_xf(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                  const struct sbdc_xf *x);

/* The present's copy: identity 1x/2x through sbdc_blit (rows limited to the panel), everything else through
 * sbdc_blit_xf. _c: the same dispatch onto the plain-C versions (what T7/T9 ran; benchmark baseline). */
void sbdc_present_blit(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                       const struct sbdc_xf *x);
void sbdc_present_blit_c(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                         const struct sbdc_xf *x);

/* The plain-C versions (T7/T9 as run on the phone): the reference for the fast ones above (NEON on ARM64,
 * cache tiles when rotated), which must produce identical pixels. Used by the host test and the benchmark. */
void sbdc_blit_c(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                 unsigned scale);
void sbdc_blit_xf_c(sbdc_u8 *dst, unsigned dst_pitch, const sbdc_u8 *src, unsigned src_pitch, struct sbdc_rect r,
                    const struct sbdc_xf *x);

/* ---- brightness (S6E3HA8 AOR) ------------------------------------------------------------ */

/* Windows level 0..100 -> AOR (higher AOR = darker), through S-Boot's and the vendor's points. */
sbdc_u32 sbdc_level_to_aor(unsigned level);

/* The link is up in command mode (S-Boot left it so); else no command is sent. */
int sbdc_dsim_ready(const struct sbdc *d);

/* One DCS long write (type 0x39): payload words, then the header; waits up to 50 ms. 0 = sent. */
int sbdc_dcs_long(struct sbdc *d, const sbdc_u8 *bytes, unsigned n);

/* S-Boot's sequence: F0 5A 5A, B1 hi lo, F7 03, F0 A5 A5 (the key always locked again), one retry
 * of the whole sequence 20 ms later. AOR clamped to SBDC_AOR_MIN..SBDC_AOR_MAX. 0 = done. */
int sbdc_set_aor(struct sbdc *d, sbdc_u32 aor);

#endif
