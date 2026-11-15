/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the iris camera (Samsung S5K5F1 on CSIS3), probe P1 (docs/drivers/IRIS.md 6): attended only, run from
 * irisprobe.c's level 2. Nothing here ever writes the IR LED: the HAL below has no way to (its registers are read
 * before and after, and must not change).
 *
 *   P1a  the sensor up in the stock order (sensor_5f1_power_setpin), its reset and clock words, its ID (0x0000) and
 *        version (0x0002: 0xA1 on a final module), the OTP page tried without streaming; everything off in reverse.
 *        Settles the bus: USI07's second HSI2C (0x108D0000) first, a bit-banged bus on gpp4-6/7 if that is silent.
 *   P1b  ISPPRE on (cam.c's port helpers), CSIS3 read fault-safe (a fault = the secure world holds it: stop); the
 *        sensor up again, the OTP the stock way (streaming 4 ms, the link not yet enabled), the setfile, CSIS3 + its
 *        VC0 WDMA for 2400 x 2400 RAW8 over two lanes at 2041 Mb/s (settle 10), ONE frame streamed without the LED
 *        (at most 0.5 s of streaming) into IRIS_MEM_PA, stream off (+40 ms), everything off; the frame's statistics.
 *
 * Every wait is bounded; the whole of P1 stops itself at IRIS_P1_LIMIT_US; any failure switches off whatever was
 * switched on, in reverse order. The pure parts (the LDO codes, the I2C message layout, the frame statistics) and the
 * whole sequence against a fake phone are host-tested (kern/host/iris-test.c). */
#ifndef SBOS_IRIS_H
#define SBOS_IRIS_H
#include <stdint.h>
#include "cam.h"

#define IRIS_W           2400U
#define IRIS_H           2400U
#define IRIS_FRAME_BYTES (IRIS_W * IRIS_H)          /* RAW8 in packet storage: a byte a pixel, 5 760 000 bytes */
/* the frame's memory: the front camera's documented window (cam.h CAM_MEM_PA: 32 MiB at 0xD8000000, in the device
 * tree's gap below secure_camera, libsb's non-cacheable map, below 4 GiB for the WDMA's 32-bit pointers). The two never
 * run at once: cam_port_up refuses while the camera is open, and cam_open while the port is up. */
#define IRIS_MEM_PA      CAM_MEM_PA
#define IRIS_FILL        0xA5U                      /* the buffer before the capture (counted afterwards) */
#define IRIS_ADDR        0x10U                      /* fimc-is-secure@10 */
#define IRIS_P1_LIMIT_US 9000000U                   /* the whole of P1 (the probe's level-2 budget is 10 s) */
#define IRIS_STREAM_MAX_US 500000U                  /* streaming at most 0.5 s */

/* ---- the supplies: S2MPS18 LDO39-41 (s2mps18-private.h: CTRL [7:6] enable, [5:0] the code) ----------------------- */
struct iris_ldo_spec {
    const char *name;
    uint8_t reg;
    uint32_t base_uv, step_uv;             /* the LDO's range: base + code x step */
    uint32_t dt_min_uv, dt_max_uv;         /* this phone's device tree (build/linux-bundle/lts.dts) */
    uint32_t want_uv;                      /* what sensor_5f1_power_setpin asks for */
};
extern const struct iris_ldo_spec iris_ldos[3];     /* in the power-on order: VDDIO (L41), VDDA (L40), VDDD (L39) */
int iris_ldo_code(const struct iris_ldo_spec *l, uint32_t uv);   /* the code; -1 off the range / grid / outside the DT */
uint32_t iris_ldo_uv(const struct iris_ldo_spec *l, unsigned code);
#define IRIS_LDO_ON 0xC0U

/* ---- the sensor's I2C messages: 16-bit register addresses, big-endian, then the data ----------------------------- */
/* the bytes a write puts on the wire after START: addr << 1 (write), reg [15:8], reg [7:0], data...; the length */
unsigned iris_i2c_wire(uint8_t addr7, uint16_t reg, const uint8_t *data, unsigned n, uint8_t *out, unsigned cap);

/* ---- the frame ---------------------------------------------------------------------------------------------------- */
struct iris_fstats {
    uint32_t min, max, mean_x10;           /* over the samples read (RAW8) */
    uint32_t hist[16];                     /* 16 bins of 16 values */
    uint32_t n;                            /* samples */
    uint32_t fill_words, words;            /* 8-byte words still all IRIS_FILL / looked at (aligned, the rows read) */
};
/* every row_step-th row, every pixel of it */
void iris_frame_stats(const volatile uint8_t *p, unsigned w, unsigned h, unsigned stride, unsigned row_step,
                      struct iris_fstats *s);

/* ---- the hardware access P1 is given ----------------------------------------------------------------------------- */
struct iris_hal {
    void *ctx;
    int (*rd32)(void *ctx, uint64_t pa, uint32_t *v);           /* 1 read, 0 faulted (libsb sb_probe32) */
    void (*wr32)(void *ctx, uint64_t pa, uint32_t v);
    int (*pmic_rd)(void *ctx, uint8_t reg, uint8_t *v);         /* the S2MPS18's PMIC block (ACPM): 1 read */
    int (*pmic_wr)(void *ctx, uint8_t reg, uint8_t v);          /* the gated tier-1 write (libsb sb_pmic_write): 1 */
    void (*hs_init)(void *ctx, uint32_t fs1, uint32_t fs2, uint32_t fs3);   /* hsi2c@108D0000: reset + timing */
    int (*hs_msg)(void *ctx, unsigned addr, int rx, uint8_t *b, unsigned n, int stop);   /* 1 done */
    int (*led_rd)(void *ctx, uint8_t r[7]);                     /* the S2MPB02's 0x13, 0x16..0x1B: 1 read */
    void (*delay_us)(void *ctx, uint32_t us);                   /* a spin, <= 10 ms */
    uint64_t (*now_us)(void *ctx);
    void (*log)(void *ctx, const char *line);
    void *(*ram)(void *ctx, uint64_t pa, uint64_t len);         /* a CPU pointer the DMA also sees, or NULL */
    /* the ISPPRE side: cam.c's port helpers (cam.h) */
    int (*isp_up)(void *ctx, const struct cam_port *p);
    int (*link_up)(void *ctx, uint32_t buf_pa);
    void (*link_go)(void *ctx);
    uint32_t (*dma_src)(void *ctx);
    void (*diag)(void *ctx, uint32_t d[6]);
    void (*dma_off)(void *ctx);
    void (*isp_down)(void *ctx);
    /* optional (P1c): an SMC to the secure monitor (libsb sb_psci); NULL: P1 never calls PREPARE / UNPREPARE */
    int64_t (*smc)(void *ctx, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
};
/* P1c (run 17: the iris pins and CSIS3 are the secure world's from boot): 1 = call MC_SECURE_CAMERA_PREPARE
 * (0x83000043, as the stock kernel does before it powers the sensor) first, and UNPREPARE (0x83000044) on every way
 * out. 0 (the default) = P1 as before, no SMC. */
extern int iris_use_prepare;
#define IRIS_SMC_PREPARE   0x83000043ULL
#define IRIS_SMC_UNPREPARE 0x83000044ULL
void iris_hal_set(const struct iris_hal *h);
#ifdef SBOS_KERNEL
void iris_hal_libsb(struct iris_hal *h);
#endif
extern const struct cam_port iris_port;     /* CSIS3 */

/* ---- P1 ----------------------------------------------------------------------------------------------------------- */
void iris_p1_begin(void);
int iris_p1_step(unsigned *wait_ms);        /* 0: call again after *wait_ms ms; 1: finished, the PASS / FAIL line logged */
void iris_p1_abort(void);                   /* everything off in reverse order, at once */
int iris_p1_passed(void);
/* what the last P1 saw (also for the host test) */
struct iris_p1_result {
    int sensor;                            /* the sensor answered */
    int bus;                               /* 0 none, 1 HSI2C, 2 bit-banged */
    uint8_t ver, otp[6], page;
    uint16_t model;
    int csis3;                             /* -1 not reached, 0 FAULT, 1 readable */
    int frame_end, frame;                  /* a frame end seen; a frame written into memory */
    uint32_t stream_us, powered_us;        /* the longest stream; the longest time the sensor was powered */
    struct iris_fstats st;
    int led_same;                          /* the IR LED's registers read the same after as before */
};
const struct iris_p1_result *iris_p1_last(void);

#ifdef IRIS_HOST
extern int iris_test_fail_step;             /* the host test: power-on step n fails (-1 none) */
#endif

#endif
