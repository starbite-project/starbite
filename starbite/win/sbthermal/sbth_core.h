/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite thermal core: TMU code to temperature, the sample filter and the watchdog levels.
 * Plain C (no OS calls), shared by sbthermal.sys and the host test (starbite/win/tests).
 * Port of starbite/libsb/tmu.c (itself after shim/starlte/em.c and the Samsung exynos_tmu.c
 * conversion, build/em-research-thermal-clocks.md). docs/WIN11-GPU.md section 8.
 */
#ifndef SBTH_CORE_H
#define SBTH_CORE_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef UINT32 sbth_u32;
typedef UINT64 sbth_u64;
#else
#include <stdint.h>
typedef uint32_t sbth_u32;
typedef uint64_t sbth_u64;
#endif

/* Physical blocks (read only). */
#define SBTH_TMU_TOP_PA 0x10070000ULL
#define SBTH_TMU_SUB_PA 0x10074000ULL
#define SBTH_TMU_SIZE 0x1000U

/* Register offsets inside one TMU block. */
#define SBTH_TRIMINFO(p) (4U * (p))       /* [8:0] te1 (25 C), [17:9] te2 (85 C); TRIMINFO0 [23] two-point */
#define SBTH_CONTROL 0x20U                /* [0] CORE_EN, [12] THERM_TRIP_EN */
#define SBTH_THD_TEMP0 0x50U              /* [24:16] rising trip code */
#define SBTH_EMUL_CON 0x160U              /* [0] emulation on: readings are fake */

/* Probe slots: 0..7 = TOP probes 0..7, 8..10 = SUB probes 0..2. */
#define SBTH_SLOTS 11U
#define SBTH_SLOT_GPU 9U                  /* SUB probe 1 (G3D) */
#define SBTH_SLOT_ISP 10U                 /* SUB probe 2 */

#define SBTH_NO_TEMP (-1000000)           /* m C: no reading */

enum sbth_level {
    SBTH_NORMAL = 0,                      /* < 70 C on the GPU probe */
    SBTH_WARM = 1,                        /* >= 70: GPU capped at 338 MHz */
    SBTH_HOT = 2,                         /* >= 80: GPU at 260 MHz */
    SBTH_STOP = 3,                        /* >= 90, or blind: no new GPU jobs */
    SBTH_CRITICAL = 4,                    /* GPU >= 100 or any probe >= 105: GPU off now */
};

/* Level thresholds in m C (rising); falling uses threshold - SBTH_HYST_MC. */
#define SBTH_WARM_MC 70000
#define SBTH_HOT_MC 80000
#define SBTH_STOP_MC 90000
#define SBTH_CRIT_GPU_MC 100000
#define SBTH_CRIT_ANY_MC 105000
#define SBTH_HYST_MC 5000
#define SBTH_RISE_READINGS 2U             /* 2 x 250 ms above a level before it is entered */
#define SBTH_FALL_READINGS 8U             /* 8 x 250 ms below (with hysteresis) before it is left */
#define SBTH_BLIND_READINGS 4U            /* 4 readings without a GPU temperature = blind */

/* Register access for the core: block 0 = TOP, 1 = SUB. */
struct sbth_io {
    void *ctx;
    sbth_u32 (*rd)(void *ctx, unsigned block, unsigned off);
    void (*delay_us)(void *ctx, unsigned us);
};

/* One probe: 9-bit code -> m C with the probe's trim (te1/te2) and the block's calibration mode. */
int sbth_code_to_mc(int code, int te1, int te2, int two_point);

/* CURRENT_TEMP code of probe p (0..7) in one block. */
int sbth_read_code(const struct sbth_io *io, unsigned block, unsigned p);

/* A filtered reading (median of 3 accepted samples from at most 6; codes 0 and 0x1ff and
 * temperatures outside -30..150 C rejected). SBTH_NO_TEMP when fewer than 3 samples passed.
 * *rejected (if not NULL) gets the number of rejected samples. */
int sbth_probe_mc(const struct sbth_io *io, unsigned block, unsigned p, unsigned *rejected);

/* Is the block on (CONTROL.CORE_EN) and not in emulation mode? */
int sbth_block_live(const struct sbth_io *io, unsigned block);

/* Hardware trip (S-Boot arms it at 115 C): armed? and its temperature in m C (from probe 0's trim). */
int sbth_trip_armed(const struct sbth_io *io, unsigned block);
int sbth_trip_mc(const struct sbth_io *io, unsigned block);

/* All probes of one reading. mc[slot] = SBTH_NO_TEMP where a block is off or a probe failed. */
struct sbth_reading {
    int mc[SBTH_SLOTS];
    int gpu_mc;                           /* = mc[SBTH_SLOT_GPU] */
    int hottest_mc;                       /* over the probes the watchdog uses (TOP 1..6, SUB 1..2) */
    unsigned rejected;                    /* samples rejected in this reading */
};

void sbth_read_all(const struct sbth_io *io, struct sbth_reading *r);

/* The level machine. */
struct sbth_levels {
    enum sbth_level level;
    unsigned above;                       /* consecutive readings asking for a higher level */
    unsigned below;                       /* consecutive readings allowing a lower level */
    unsigned blind;                       /* consecutive readings without a GPU temperature */
    enum sbth_level want_up;              /* lowest level the current rising run asked for */
    enum sbth_level want_down;            /* highest level seen in the current falling run */
    int max_gpu_mc;
    int max_any_mc;
};

void sbth_levels_init(struct sbth_levels *s);

/* The level a reading asks for without hysteresis (rising thresholds). */
enum sbth_level sbth_level_for(int gpu_mc, int hottest_mc);

/* The highest level a reading still holds when falling (thresholds - hysteresis). */
enum sbth_level sbth_level_hold(int gpu_mc, int hottest_mc);

/* Feed one reading; returns 1 when the level changed. */
int sbth_levels_step(struct sbth_levels *s, int gpu_mc, int hottest_mc);

/* The state other drivers and tools see (also the IOCTL and the callback's query answer). */
#define SBTH_STATE_VERSION 2U
struct sbth_state {
    sbth_u32 version;                     /* SBTH_STATE_VERSION; 0 after a query = no watchdog */
    sbth_u32 seq;                         /* +1 per reading: a consumer treats a value that stops
                                           * moving, or time_100ns older than 1 s, as Blind (Stop) */
    sbth_u64 time_100ns;                  /* KeQueryInterruptTime() of the last reading */
    sbth_u32 level;                       /* enum sbth_level */
    int gpu_mc, hottest_mc;
    int max_gpu_mc, max_any_mc;
    int mc[SBTH_SLOTS];
    sbth_u32 readings;                    /* readings taken since start */
    sbth_u32 rejected;                    /* samples rejected since start */
    sbth_u32 blind_events;                /* times the watchdog went blind */
    sbth_u32 trip_armed;                  /* bit 0 TOP, bit 1 SUB */
    int trip_top_mc, trip_sub_mc;
    sbth_u32 blocks_live;                 /* bit 0 TOP, bit 1 SUB */
    sbth_u64 changes;                     /* level changes since start */
};

/* The callback object other drivers use: \Callback\StarbiteThermal.
 * Open it with ExCreateCallback(..., Create = FALSE, ...): a consumer that creates it would make an
 * empty object that proves nothing.
 *  ExNotifyCallback(obj, SBTH_CB_QUERY, struct sbth_state *)   fills the state (zero it first;
 *      version 0 afterwards = no watchdog). Every registered callback sees the query: consumers'
 *      callbacks must ignore Argument1 values other than SBTH_CB_LEVEL.
 *  registered callbacks get (SBTH_CB_LEVEL, const struct sbth_state *) on every level change
 * Callbacks run in the watchdog's thread at PASSIVE_LEVEL and must return quickly. The watchdog
 * cannot be unloaded (no DriverUnload). */
#define SBTH_CALLBACK_NAME L"\\Callback\\StarbiteThermal"
#define SBTH_CB_QUERY ((void *)(sbth_u64)0x53425451U)   /* 'SBTQ' */
#define SBTH_CB_LEVEL ((void *)(sbth_u64)0x5342544cU)   /* 'SBTL' */

/* \\.\StarbiteThermal: METHOD_BUFFERED, FILE_READ_ACCESS, returns struct sbth_state. */
#define SBTH_DEVICE_TYPE 0x8253U
#define SBTH_IOCTL_GET_STATE (((sbth_u32)SBTH_DEVICE_TYPE << 16) | (1U << 14) | (0x801U << 2) | 0U)

#endif
