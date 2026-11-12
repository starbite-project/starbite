/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: ABOX in "bypass" (AP-only) mode for media sound (abox_pcm.c). ABOX's Cortex-A7 stays off and no
 * firmware runs: the AP powers the AUD domain, clocks PLL_AUD, lets ABOX's SysMMU pass addresses through, and drives
 * the DMA engines itself:
 *
 *   playback  RDMA0 (a ring in abox_rmem) -> SPUS chain 0 (no ASRC) -> SIFS1 -> UAIF1 (I2S master: the MAX98512 pair)
 *                                                                          or UAIF0 (I2S master: the codec's AIF1RX)
 *   capture   UAIF0 (the codec's AIF1TX: the main mic) -> NSRC0 -> SPUM (no ASRC) -> SIFM0 -> WDMA1 (a ring)
 *
 * Sources (facts): Samsung's GPL-2.0 ABOX driver for this SoC (sound/soc/samsung/abox/abox.h register map, RDMA /
 * WDMA layout and STATUS fields, abox_rdma.c pointer and disable barrier, abox.c DAPM mux values, abox_if.c UAIF
 * formats); kern/abox.c (the call path: PMU / CMU_AUD / PLL / pins, same SoC); an earlier port's AP-only driver for this phone
 * (APSL, read for facts only: drivers/audio/abox/exynos_abox.c), which saw on this S9: the speaker path via
 * SIFS1 running at 192000 bytes a second (the SIFS0 mixer route stalls without firmware), the SysMMU at CTRL 4 after
 * boot and RDMA working with CTRL 0, SPUS SBANK_RDMA0 (+0x220) and SPUM SBANK_NSRC0 (+0x328) reading 0 after boot and
 * needing a size (512 bytes, SIZE [29:20] in 16-byte units) before the DMA moves, and main-mic capture via WDMA1.
 *
 * Not seen on this phone with THIS code yet. Inferred, not read anywhere: RDMA's BUF_END = one past the ring's last
 * byte, VOL_FACTOR 0x800000 = unity (Q1.23), RDMA0's CTRL0 FORMAT field (its bit position is not in the 9810's
 * source: CTRL0 is left as it reads after reset apart from ENABLE and BURST_LEN; the probe measures the bytes per
 * second RDMA0 reads, 192000 = 48 kHz 16-bit stereo, and apcm_cfg.rdma_ctrl0_or can add bits if it is not).
 *
 * Single-threaded, no locks, bounded waits, every step logged ("apcm: ..."). Any failure leaves ABOX as found (the
 * AUD domain off again if it was off). Calliope wins: apcm refuses to start while the call driver (abox.c) owns ABOX,
 * and the caller must stop it (snd_suspend_for_call) before abox_boot(). */
#ifndef SBOS_ABOX_PCM_H
#define SBOS_ABOX_PCM_H
#include "abox.h"

/* geometry: 48 kHz, 2 x S16 (4 bytes a frame) both ways */
#define APCM_RATE           48000U
#define APCM_FRAME_BYTES    4U
#define APCM_PERIOD_FRAMES  512U                /* 10.67 ms (an earlier port's working period) */
#define APCM_PERIOD_BYTES   (APCM_PERIOD_FRAMES * APCM_FRAME_BYTES)
#define APCM_PERIODS        32U                 /* 341 ms of ring */
#define APCM_RING_BYTES     (APCM_PERIODS * APCM_PERIOD_BYTES)      /* 64 KiB */
#define APCM_BYTES_PER_S    (APCM_RATE * APCM_FRAME_BYTES)          /* 192000 */
/* where in abox_rmem (offsets): in the 1 MiB section of abox.c's page tables, after them (abox.c uses its first
 * 17 KiB, only in call mode). Each ring is followed by 64 KiB of zeroes: STATUS's offset field covers 128 KiB, so a
 * DMA that takes the ring's end differently reads silence, never other memory. */
#define APCM_RM_PLAY        0x1580000U
#define APCM_RM_CAP         0x15C0000U
#define APCM_RM_WINDOW      0x20000U            /* ring + guard zeroes */

#define APCM_UAIF0          0                   /* the codec (headphones, mic) */
#define APCM_UAIF1          1                   /* the amplifiers */

#define APCM_ST_OFF         0
#define APCM_ST_UP          1                   /* powered and clocked, nothing running */
#define APCM_ST_FAILED      2

/* the engine's tunables (defaults: apcm_cfg_default) */
struct apcm_cfg {
    uint64_t rmem_base, rmem_size;              /* abox_rmem (0xFB200000, 40 MiB; the phone's DT) */
    uint32_t sbank_bytes;                       /* SPUS / SPUM sample bank given when it reads 0: 512 */
    uint32_t burst;                             /* RDMA0 / WDMA1 CTRL0 BURST_LEN [22:19] set when it reads 0: 8 */
    uint32_t rdma_ctrl0_or;                     /* extra CTRL0 bits for RDMA0 and WDMA1 (format trials): 0 */
};
void apcm_cfg_default(struct apcm_cfg *c);
int apcm_cfg_set(const struct apcm_cfg *c);     /* only while OFF; 0 or ABOX_ECFG */

/* The HAL is abox.h's (rd32, wr32, smc, delay_us, log and ram are required; file is not used). owns(): nonzero while
 * the call driver (Calliope) has ABOX (the kernel: abox_state() not OFF / FAILED); NULL = never. */
int apcm_hal_set(const struct abox_hal *h, int (*owns)(void));

int apcm_up(void);                      /* power, CPU off, clocks, SysMMU pass-through, pins, UAIF formats; 0 or <0 */
int apcm_play_start(int uaif);          /* ring zeroed, route to UAIF0/1, RDMA0 started after the UAIF; 0 or <0 */
int apcm_play_route(int uaif);          /* move the running stream to the other UAIF; 0 or <0 */
uint32_t apcm_play_pos(uint32_t *status);   /* byte RDMA0 reads now (0 .. ring-1); APCM_NOPOS not running/outside */
uint8_t *apcm_play_buf(void);           /* the ring (CPU view, non-cacheable); NULL when not up */
void apcm_play_stop(void);              /* RDMA0 first (40 ms barrier), 1 ms FIFO flush, then the UAIF */
int apcm_cap_start(void);               /* UAIF0 MIC -> WDMA1 into the capture ring; 0 or <0 */
uint32_t apcm_cap_pos(uint32_t *status);
const uint8_t *apcm_cap_buf(void);
void apcm_cap_stop(void);
void apcm_down(void);                   /* everything stopped and put back as found; safe at any point */
void apcm_forget(void);                 /* Calliope took ABOX under us (a missed snd_suspend_for_call): our state is
                                         * dropped WITHOUT touching the hardware, which is the call driver's now */
uint32_t apcm_uaif_status(int uaif);    /* UAIFn STATUS (ERROR_OF_MIC bit 1, ERROR_OF_SPK bit 0); 0 when not up */
void apcm_sync(void);                   /* after CPU stores into the ring (dsb) */

int apcm_state(void);
int apcm_playing(void);                 /* -1 none, else the UAIF */
int apcm_capturing(void);
const char *apcm_why(void);
#define APCM_NOPOS 0xFFFFFFFFU

/* read-only looks for the probe's level 0 (the PMU is always on; CMU_AUD / SFR are read only if the domain is on) */
struct apcm_look {
    uint32_t aud_stat, cpu_stat;
    int aud_on;
    uint32_t pll_con0, pll_con3, mmu_ctrl, ip_index;   /* valid when aud_on */
    uint64_t pll_hz;
};
void apcm_look(struct apcm_look *l);

/* ---- registers this file adds to abox.h's map (Samsung's abox.h unless said) ---------------------------------- */
#define APCM_RDMA(n)            (0x1000U + 0x100U * (n))
#define APCM_WDMA(n)            (0x2000U + 0x100U * (n))
#define APCM_DMA_CTRL0          0x00U           /* ENABLE bit 0; BURST_LEN [22:19] (an earlier port, from the S10's source) */
#define APCM_DMA_BUF_STR        0x08U
#define APCM_DMA_BUF_END        0x0CU
#define APCM_DMA_BUF_OFFSET     0x10U
#define APCM_DMA_STR_POINT      0x14U
#define APCM_DMA_VOL_FACTOR     0x18U           /* [23:0] */
#define APCM_DMA_STATUS         0x30U           /* PROGRESS 31, RBUF_OFFSET [28:16] (16-byte units), RBUF_CNT [12:0] */
#define APCM_SBANK_RDMA0        0x0220U         /* SPUS sample bank of RDMA0 (an earlier port: SIZE [29:20] x 16 bytes) */
#define APCM_SBANK_NSRC0        0x0328U         /* SPUM sample bank of NSRC0 (an earlier port, the S10's abox_soc.h) */
#define APCM_SFR_QCH_DIS        0x0038U         /* QCHANNEL_DISABLE: 1 ACLK, 2 BCLK_UAIF0, 3 BCLK_UAIF1 */
#define APCM_SFR_SYSPOWER       0x0010U

#endif
