/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: ABOX (the Exynos 9810 audio DSP) with Samsung's Calliope firmware, for circuit-switched call audio
 * (abox.c). Ported from Samsung's GPL-2.0 kernel (ref/starlte-kernel: sound/soc/samsung/abox/abox.c, abox_gic.c,
 * abox_if.c, abox_rdma.c, abox_wdma.c, abox_vss.c, include/sound/samsung/abox_ipc.h, the 9810 DT, cal-if/exynos9810);
 * a few register values were checked against measurements made on this phone (facts only).
 *
 * The call path never passes through the AP's memory: CP <-> VSS (runs on ABOX's Cortex-A7 inside Calliope) <->
 * RDMA4 / WDMA2 <-> SIFS0 / NSRC1 <-> UAIF1 (the amplifier) / UAIF0 (the codec). The AP boots Calliope, sets the
 * routes, sends a few IPC messages and answers Calliope's requests. The amplifier and the codec have their own
 * drivers.
 *
 * Use (single-threaded, no locks; every call logs one "abox: ..." line per step and fails cleanly):
 *   abox_hal_set(&hal);             once (the kernel: abox_hal_libsb() fills one from libsb)
 *   abox_cfg_set(&cfg);             optional: the carve-outs from the FDT / the modem's memory map
 *   abox_vss_prepare();             before the CP starts (the modem's own loader also clears the word)
 *   abox_boot();                    0 = Calliope said BOOT_DONE; < 0 = ABOX left stopped and powered off
 *   abox_vss_start();               after the CP is online (or right after abox_boot() if the CP already is)
 *   abox_call(1) ... abox_call(0);  per call; the amplifier / codec drivers are started around it
 *   abox_poll();                    from the kernel's timer, every 10 ms or less while ABOX runs
 *   abox_off();                     best effort: CPU stopped, clocks released, AUD domain off
 */
#ifndef SBOS_ABOX_H
#define SBOS_ABOX_H
#include <stdint.h>
#include <stddef.h>

/* ---- the hardware access the driver is given ------------------------------------------------------------------
 * Every register access goes through rd32 / wr32 (physical addresses; the kernel's identity map has the SoC as
 * Device-nGnRnE). abox.c refuses (and logs) any write outside the audio blocks before it reaches wr32. ram() gives a
 * CPU pointer to normal RAM (abox_rmem and the VSS magic word only; both non-cacheable in libsb's map), so the 18 MiB
 * firmware image and the page tables are written with plain stores. file() reads a VENDOR file (path relative to
 * VENDOR's root, e.g. "firmware/calliope_sram.bin") into dst: its size, or < 0 if missing, unreadable or larger than
 * cap (nothing promised about dst then). Optional hooks may be NULL. */
struct abox_hal {
    void *ctx;                                                      /* passed to every callback */
    uint32_t (*rd32)(void *ctx, uint64_t pa);
    void (*wr32)(void *ctx, uint64_t pa, uint32_t v);               /* completes before returning (dsb) */
    int64_t (*smc)(void *ctx, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
    void (*delay_us)(void *ctx, uint32_t us);
    void (*log)(void *ctx, const char *line);                       /* one line, no newline */
    long (*file)(void *ctx, const char *path, void *dst, uint32_t cap);
    void *(*ram)(void *ctx, uint64_t pa, uint64_t len);             /* NULL: not mappable */
    /* optional */
    uint64_t (*now_us)(void *ctx);                                  /* monotonic; for CHANGED_GEAR's time stamp */
    void (*sync)(void *ctx);                                        /* dsb sy: RAM stores visible to ABOX */
    int (*dvfs_aud)(void *ctx, uint32_t khz);                       /* ACPM AUD domain (9) level; 0 = release */
    int (*dvfs_int)(void *ctx, uint32_t khz);                       /* ACPM INT floor; 0 = release */
};
void abox_hal_set(const struct abox_hal *h);
#ifdef SBOS_KERNEL
/* rd32/wr32 with barriers, sb_psci for the SMC, sb_udelay, sb_us, sb_log, ram() = identity for non-cacheable RAM
 * (sb_map_kind 3). file and ctx come from the caller (the VENDOR reader, e.g. EROFS erofs_lookup/erofs_read_all into
 * dst after checking in.size <= cap). dvfs_aud / dvfs_int are left NULL (Starbite's rule: no INT/MIF DVFS, AUD not
 * in the listed domains): the driver then clocks the CA7 at the idle level, see abox.c step 4. */
void abox_hal_libsb(struct abox_hal *h, long (*file)(void *ctx, const char *path, void *dst, uint32_t cap),
                    void *file_ctx);
#endif

/* ---- where things are ----------------------------------------------------------------------------------------
 * Defaults are this phone's (device/dtb/starlte.dts: abox_rmem, modem_if, shmem; starbite/modem sbm_toc.c MEM0 map:
 * VSS 0x7900000 + 0x700000, VPA 0x9000000 + 0x200000 inside modem_if 0xF0000000). All 1 MiB aligned, below 4 GiB. */
struct abox_cfg {
    uint64_t rmem_base, rmem_size;          /* abox_rmem: 0xFB200000, 40 MiB (the DRAM image, IVA, buffers, tables) */
    uint64_t vss_base, vss_size;            /* the VSS region of the modem carve-out: 0xF7900000, 7 MiB */
    uint64_t vparam_base, vparam_size;      /* VSS PARAM: 0xF9000000, 2 MiB */
    uint32_t vss_magic_off;                 /* DT abox_vss magic_offset: 0x600000 */
    uint32_t boot_timeout_ms;               /* BOOT_DONE wait (Android logs ~64 ms; Samsung allows 10 s): 3000 */
    uint32_t fw_log_max;                    /* Calliope log lines copied to the log per boot (0: none): 2000 */
};
void abox_cfg_default(struct abox_cfg *c);
int abox_cfg_set(const struct abox_cfg *c);     /* 0, or ABOX_ECFG (kept the previous one) */

/* ---- the interface -------------------------------------------------------------------------------------------- */
int abox_boot(void);
void abox_vss_prepare(void);
int abox_vss_start(void);
int abox_call(int on);
int abox_poll(void);
void abox_off(void);
const char *abox_why(void);
int abox_state(void);                       /* ABOX_ST_* */
uint32_t abox_version(void);                /* Calliope's version word from BOOT_DONE ("CRL0" = 0x43524C30), 0 none */

/* ---- media through Calliope (abox_media.c): additive hooks ----------------------------------------------------
 * abox_map_extra(): one more ABOX window in our SysMMU tables (4 KiB pages through its own L2 table), set BEFORE
 *   abox_boot() and kept until removed with size 0 (2 slots, found by iova). iova: 1 MiB aligned, not one of the
 *   windows above (abox_boot() logs and skips a clash); pa: 4 KiB aligned, inside abox_rmem's page-table section
 *   past its first 64 KiB (0x1510000 .. 0x1600000), so ABOX never sees anything but abox_rmem; size <= 960 KiB.
 *   0 or ABOX_ECFG (ABOX_ESTATE while Calliope runs: the tables are written at boot).
 * abox_pcm_msg(): one PCM task message (Samsung's IPC_PCMTASK_MSG: msgtype, channel_id, three param words) on
 *   IPC_PCMPLAYBACK (capture 0) or IPC_PCMCAPTURE (1); the call path's own channels (playback 4, capture 2) and
 *   ids >= 8 are refused. 0, ABOX_ESTATE (not READY / CALL, refused id) or ABOX_EIPC.
 * abox_pcm_pointer(): the last PCM_PLTDAI_POINTER Calliope sent for that channel (an ABOX IOVA, 0 none yet) and,
 *   in *count, how many it sent since abox_boot(). */
#define ABOX_IOVA_RDMA(n)    (0x91000000U + 0x100000U * (uint32_t)(n))     /* IOVA_RDMA_BUFFER(n) */
#define ABOX_PCM_OPEN        11U
#define ABOX_PCM_CLOSE       12U
#define ABOX_PCM_HW_PARAMS   14U            /* rate, bit depth, channels */
#define ABOX_PCM_HW_FREE     15U
#define ABOX_PCM_PREPARE     16U
#define ABOX_PCM_TRIGGER     17U            /* 1 start, 0 stop */
#define ABOX_PCM_POINTER     18U
#define ABOX_PCM_SET_BUFFER  20U            /* IOVA, period bytes, periods */
int abox_map_extra(uint32_t iova, uint64_t pa, uint32_t size);
int abox_pcm_msg(int capture, uint32_t id, uint32_t type, uint32_t a, uint32_t b, uint32_t c);
uint32_t abox_pcm_pointer(int capture, uint32_t id, uint32_t *count);
/* diagnostics: log the next n messages from Calliope raw ("abox: rx:" + 8 words) before handling them; copy n more
 * of Calliope's own log lines ("abox: fw:") from now on, whatever fw_log_max allowed so far */
void abox_trace_rx(unsigned n);
void abox_fw_log_more(unsigned n);

#define ABOX_ST_OFF     0
#define ABOX_ST_BOOTING 1
#define ABOX_ST_READY   2                   /* Calliope said BOOT_DONE */
#define ABOX_ST_CALL    3                   /* READY with the call path on */
#define ABOX_ST_FAILED  4                   /* stopped after a failure; abox_boot() may try again */

#define ABOX_ESTATE  -1                     /* wrong state (no HAL, not booted, ...) */
#define ABOX_EFILE   -2                     /* a required VENDOR file is missing or too large */
#define ABOX_EPOWER  -3                     /* AUD domain / TZPC SMC / IP_INDEX */
#define ABOX_ECLOCK  -4                     /* PLL_AUD or a divider */
#define ABOX_EMMU    -5                     /* memory / SysMMU set-up */
#define ABOX_ECPU    -6                     /* ABOX CPU did not power up / down */
#define ABOX_ETIME   -7                     /* no BOOT_DONE in time */
#define ABOX_EIPC    -8                     /* Calliope did not take an IPC message (10 ms, 3 tries) */
#define ABOX_EFAULT  -9                     /* SysMMU fault, Calliope REPORT_FAULT, CPU went down */
#define ABOX_ERANGE  -10                    /* a write outside the audio blocks was refused (a bug) */
#define ABOX_ECFG    -11

/* ---- register map (physical; Samsung's 9810 DT and abox.h) ---------------------------------------------------- */
#define ABOX_PMU        0x14060000ULL
#define ABOX_PMU_AUD_CONF       0x4000U     /* [3:0] 0xF on (flexpmu aud_on) */
#define ABOX_PMU_AUD_STAT       0x4004U
#define ABOX_PMU_CPU_CONF       0x415CU     /* ABOX_CPU_CONFIGURATION, bit 0 LOCAL_PWR_CFG */
#define ABOX_PMU_CPU_STAT       0x4160U     /* bit 0 */
#define ABOX_PMU_CPU_OPT        0x4164U     /* bit 28 ENABLE_CPU */
#define ABOX_PMU_CPU_STANDBY    0x3804U     /* bit 28 WFI */
#define ABOX_PMU_PAD_RET        0x4170U     /* PAD_RETENTION_ABOX_OPTION, bit 28 release */
#define ABOX_PMU_GPIO_MODE      0x1424U     /* GPIO_MODE_ABOX_SYS_PWR_REG, bit 0 */
#define ABOX_CMU        0x17C00000ULL       /* CMU_AUD, 0x8000 */
#define ABOX_SYSREG     0x17C10000ULL       /* SYSREG_AUD; 0x17C10204 is the DTZPC argument of the PD SMC */
#define ABOX_SFR        0x17C50000ULL       /* 0x10000 */
#define ABOX_GPIO       0x17C60000ULL       /* AUD pinctrl: gpb0 +0x00, gpb1 +0x20 (bank_type_6: CON +0, PUD +8) */
#define ABOX_GICD       0x17CF1000ULL
#define ABOX_GICC       0x17CF2000ULL       /* 0x1004: DIR at +0x1000 */
#define ABOX_SRAM       0x17D00000ULL
#define ABOX_SRAM_SIZE  0x31000U
#define ABOX_SYSMMU     0x17E00000ULL
#define ABOX_SMC_PD     0x82000410ULL       /* SMC_CMD_PREAPRE_PD_ONOFF (sic): a1 1 restore / 0 save, a2 the DTZPC */
#define ABOX_DTZPC      0x17C10204ULL

/* SRAM IPC (DT ipc_*_offset); ABOX_IPC_MSG is 0x2FC bytes */
#define ABOX_IPC_TX      0x30000U
#define ABOX_IPC_TX_ACK  0x302FCU
#define ABOX_IPC_RX      0x30300U
#define ABOX_IPC_RX_ACK  0x305FCU
#define ABOX_IPC_BYTES   0x2FCU

/* IOVAs (abox.h) */
#define ABOX_IOVA_DRAM   0x80000000U        /* 18 MiB */
#define ABOX_IOVA_IVA    0x90000000U        /* 512 KiB */
#define ABOX_IOVA_RDMA4  0x91400000U        /* IOVA_RDMA_BUFFER(4) */
#define ABOX_IOVA_WDMA2  0x92200000U        /* IOVA_WDMA_BUFFER(2) */
#define ABOX_IOVA_VSS    0xA0000000U
#define ABOX_IOVA_VPARAM 0xA1000000U
#define ABOX_IOVA_DUMP   0xD0000000U
#define ABOX_IOVA_CHIPID 0x10000000U        /* one page, 1:1 */

/* abox_rmem layout (offsets): every piece on a 1 MiB boundary so the SysMMU maps it with sections */
#define ABOX_RM_DRAM     0x0000000U         /* 18 MiB: calliope_dram.bin + the ext_bins */
#define ABOX_RM_DRAM_SZ  0x1200000U
#define ABOX_RM_IVA      0x1200000U         /* 1 MiB section, calliope_iva.bin (512 KiB used) */
#define ABOX_RM_RDMA4    0x1300000U         /* 1 MiB: the call RX ring (7680 bytes used); calliope_sram.bin's
                                             * scratch during abox_boot() */
#define ABOX_RM_WDMA2    0x1400000U         /* 1 MiB: the call TX ring */
#define ABOX_RM_PT       0x1500000U         /* the L1 table (16 KiB) and one L2 table (1 KiB) */
#define ABOX_RM_PT_L2    0x1504000U
#define ABOX_RM_DUMP     0x1600000U         /* 18 MiB to the end: IOVA 0xD0000000 (Samsung maps abox_rmem there);
                                             */
#define ABOX_RM_NEED     0x1600000U         /* smallest abox_rmem that works (no dump window then) */

/* the call PCMs (libaudioproxy.so pcm_config_voicerx_playback / voicetx_capture: 2 ch, 48 kHz, 480 frames x 4, S16) */
#define ABOX_CALL_RATE    48000U
#define ABOX_CALL_BITS    16U
#define ABOX_CALL_CH      2U
#define ABOX_CALL_PERIOD  1920U             /* bytes */
#define ABOX_CALL_PERIODS 4U

#endif
