/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: ABOX bring-up with Samsung's Calliope firmware and the call routes (abox.h has the interface).
 *
 * A port of Samsung's GPL-2.0 ABOX driver for the Exynos 9810 (ref/starlte-kernel, read with git show):
 *   abox.c        abox_enable() (the boot order), abox_download_firmware() / _extra_firmware(), __abox_process_ipc(),
 *                 abox_irq_handler() / abox_system_ipc_handler(), abox_boot_done_work_func() + abox_restore_data(),
 *                 abox_cpu_power() / abox_cpu_enable() / abox_pad_retention() and the CONFIG_SOC_EXYNOS9810 PMU
 *                 offsets, abox_cpu_pm_ipc(), abox_change_cpu_gear() / abox_notify_cpu_gear(), abox_request_dram_on(),
 *                 abox_modem_notifier() (START_VSS), the route enums and the SIFS0 / RECP / SIFM1 flush events;
 *   abox_gic.c    GIC_IS_SECURE_FREE: abox_gic_init_gic(), abox_gic_generate_interrupt(), the IAR / EOI / DIR loop;
 *   abox_if.c     abox_uaif_set_fmt() / _hw_params() / _trigger() / _startup(), abox_if_shutdown(), set_tristate();
 *   abox_rdma.c / abox_wdma.c   the PCM IPC order for a "call" platform (OPEN, SET_BUFFER, HW_PARAMS, PREPARE,
 *                 TRIGGER; TRIGGER 0, HW_FREE, CLOSE) and the cpu gear CALL_KERNEL;
 *   abox_vss.c    the magic word at VSS + magic_offset cleared before the CP starts; the call's INT floor 178 MHz;
 *   abox_log.c    the ABOX_LOG_BUFFER ring (index_writer, index_reader, size, text);
 *   star_madera.c the UAIF formats (I2S, NB_NF, CBS_CFS = ABOX master) and the DPCM trigger order (POST_PRE playback,
 *                 PRE_POST capture);
 *   exynos9810.dtsi / the phone's DTB   SFR / SRAM / GIC addresses, ipc offsets, pm_qos_aud, ext_bin list, pinctrl
 *                 aud-i2s0/1-bus, abox_rmem, modem_if, shmem; exynos-iommu*.h  SysMMU v1-style tables and CTRL values;
 *   cal-if/exynos9810   flexpmu aud_on / aud_off, cmucal-sfr (CMU_AUD offsets), cmucal-node (PLL_AUD M 45 P 1 S 0
 *                 K 24319, PLL_1031X), cmucal-qch (Q-channel ENABLE bit 0 / CLOCK_REQ bit 1), ra.c (PLL and Q-channel
 *                 sequences), pll_spec.c (1031X lock times 150 / 500), exynos-el3_mon.c (the PD SMC arguments).
 * The VENDOR files and the CP's VSS image are read at run time, never shipped.
 *
 * Safety: writes only to the PMU's audio offsets, CMU_AUD, SYSREG_AUD, ABOX SFR / SRAM / GIC, SysMMU_AUD, the AUD
 * pinctrl, abox_rmem and the VSS magic word; reg_ok() refuses any other register before it reaches the HAL, and
 * H.ram() is asked for exactly two things: abox_rmem and the magic word (both from the checked abox_cfg).
 * Every wait has a time-out. A failure stops the ABOX CPU and powers the domain off (teardown()). */
#include "abox.h"

#ifdef SBOS_KERNEL
#include "sb.h"
#else
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
#endif

/* ---- Samsung's IPC numbers (include/sound/samsung/abox_ipc.h) --------------------------------------------------- */
#define IPC_SYSTEM       1U
#define IPC_PCMPLAYBACK  2U
#define IPC_PCMCAPTURE   3U
#define IPC_ERAP         5U
#define IPC_ABOX_CONFIG  0xAU

#define SYS_SUSPEND          0x01U
#define SYS_RESUME           0x02U
#define SYS_BOOT_DONE        0x03U
#define SYS_CHANGE_GEAR      0x04U
#define SYS_END_L2C          0x06U
#define SYS_REQUEST_SYSCLK   0x07U
#define SYS_REQUEST_L2C      0x08U
#define SYS_CHANGED_GEAR     0x09U
#define SYS_REPORT_LOG       0x10U
#define SYS_FLUSH_LOG        0x11U
#define SYS_REPORT_DUMP      0x20U
#define SYS_FLUSH_DUMP       0x22U
#define SYS_SET_MODE         0x50U
#define SYS_SET_TYPE         0x60U
#define SYS_START_VSS        0xA0U
#define SYS_REPORT_COMPONENT 0xC0U
#define SYS_REPORT_FAULT     0xFAU

#define PCM_PLTDAI_OPEN      11U
#define PCM_PLTDAI_CLOSE     12U
#define PCM_PLTDAI_HW_PARAMS 14U
#define PCM_PLTDAI_HW_FREE   15U
#define PCM_PLTDAI_PREPARE   16U
#define PCM_PLTDAI_TRIGGER   17U
#define PCM_PLTDAI_POINTER   18U
#define PCM_SET_BUFFER       20U
#define PCM_PLTDAI_ACK       22U

#define CFG_SET_MIXER_RATE    0x01U         /* .. 0x09 SET_INMUX4_SAMPLE_RATE */
#define CFG_SET_MIXER_FORMAT  0x10U         /* .. 0x18 SET_INMUX4_FORMAT */
#define ERAP_REALTIME_CLOSE   2U
#define ERAP_ECHO_CANCEL      0U
#define ERAP_VI_SENSE         1U

#define MODE_NORMAL   0U
#define MODE_IN_CALL  2U
#define TYPE_VOICE    0U

#define GEAR_MAX      1U
#define GEAR_MIN      12U
#define GEAR_ID_DEFAULT 0xAB0CDEFAU         /* DEFAULT_CPU_GEAR_ID */
#define GEAR_ID_CALL    0xCA12U             /* ABOX_CPU_GEAR_CALL_KERNEL */
#define CALL_INT_KHZ    178000U             /* abox_vss.c E9810_INT_FREQ */

#define IPC_WORDS     (ABOX_IPC_BYTES / 4U) /* 191 */
#define RX_WORDS      32U                   /* what is kept of a received message */
#define RXQ           8U

/* ---- CMU_AUD (cmucal-sfr.c) ----------------------------------------------------------------------------------- */
#define CMU_PLL_LOCKTIME   0x0000U
#define CMU_PLL_CON0       0x0120U          /* ENABLE 31, STABLE 29, MUX_BUSY 7, MUX_SEL 4, M [25:16] P [13:8] S [2:0] */
#define CMU_PLL_CON3       0x018CU          /* K [15:0] */
#define CMU_MUX_CPU        0x1000U          /* SELECT 0: DIV_CLK_AUD_PLL, 1: CLKCMU_AUD_CPU (CMU_TOP) */
#define CMU_MUX_UAIF0      0x1004U
#define CMU_MUX_UAIF1      0x1008U
#define CMU_DIV_AUDIF      0x1800U          /* ratio [8:0] */
#define CMU_DIV_BUS        0x1804U          /* [2:0] */
#define CMU_DIV_BUSP       0x1808U          /* [1:0] */
#define CMU_DIV_CPU_ACLK   0x180CU
#define CMU_DIV_PLL        0x1820U          /* [3:0] */
#define CMU_DIV_UAIF0      0x1824U          /* [8:0] */
#define CMU_DIV_UAIF1      0x1828U
#define CMU_QCH_DUMMY      0x3000U          /* GATE_ABOX_DUMMY: the "cpu" clock abox_enable() turns on */
#define CMU_QCH_BCLK0      0x3028U
#define CMU_QCH_BCLK1      0x302CU
#define CMU_QCH_SYSMMU     0x3060U
#define DIV_BUSY           (1U << 16)
#define PLL_ENABLE         (1U << 31)
#define PLL_STABLE         (1U << 29)
#define PLL_MUX_BUSY       (1U << 7)
#define PLL_MUX_SEL        (1U << 4)
#define PLL_MPS_MASK       0x03FF3F07U
#define PLL_M              45U
#define PLL_P              1U
#define PLL_S              0U
#define PLL_K              24319U           /* 26 MHz x (45 + 24319/65536) = 1179.648 MHz */
#define PLL_FLOCK          500U             /* pll_spec.c gpll1031X flock_time (K != 0): LOCKTIME = P x 500 */
#define QCH_ENABLE         (1U << 0)
#define QCH_CLOCK_REQ      (1U << 1)

/* blk_aud init LUT (cmucal-vclklut.c): BUS 1, BUSP 2; AUDIF 24.576 MHz at boot (AUDIF_RATE_HZ), 73.728 MHz once a
 * UAIF runs (abox_register_bclk_usage, uaif_max_div 512: 384000 x 96 x 2); BCLK 48000 x 2 x 16 = 1.536 MHz */
#define DIV_BUS_VAL        1U
#define DIV_BUSP_VAL       2U
#define DIV_AUDIF_BOOT     47U              /* 1179.648 / 48 = 24.576 */
#define DIV_AUDIF_UAIF     15U              /* / 16 = 73.728 */
#define DIV_UAIF_BCLK      47U              /* 73.728 / 48 = 1.536 */
#define DIV_PLL_IDLE       3U               /* / 4 = 294.912 MHz: Calliope's idle AUD level (pm_qos 295000) */

/* ---- SYSREG_AUD, ABOX SFR (abox.h) ------------------------------------------------------------------------------ */
#define SYSREG_MISC_CON    0x032CU
#define SFR_IP_INDEX       0x0000U
#define SFR_VERSION        0x0004U
#define SFR_SYSPOWER_CTRL  0x0010U
#define SFR_SYSPOWER_STAT  0x0014U
#define SFR_QCH_DISABLE    0x0038U          /* bit 2: BCLK_UAIF0 */
#define SFR_ROUTE_CTRL0    0x0040U          /* UAIFn SPK [4n+3:4n]: 1 SIFS0 */
#define SFR_ROUTE_CTRL1    0x0044U          /* NSRCn [4n+3:4n]: 8 UAIF0 */
#define SFR_ROUTE_CTRL2    0x0048U          /* RSRCn: 1 SIFS0 */
#define SFR_SPUS_CTRL0     0x0200U          /* chain n [4n+3:4n]: IN bit 3 (0 RDMA), OUT [2:1] (1 SIFS0), ASRC bit 0 */
#define SFR_SPUS_CTRL1     0x0204U          /* MIXP_FORMAT [4:0] */
#define SFR_SPUS_CTRL2     0x0208U          /* MIXP_FLUSH bit 0 */
#define SFR_SPUM_CTRL0     0x0300U          /* RSRC ASRC bit 0; NSRCn at 4 + 4n: ASRC +0, OUT (SIFMn: 0 WDMA) +3 */
#define SFR_SPUM_CTRL1     0x0304U          /* RECP_SRC_FORMAT [12:8], RECP_SRC_VALID [1:0] (PIFS0 bit 0) */
#define SFR_SPUM_CTRL2     0x0308U          /* RECP_FLUSH bit 0 */
#define SFR_SPUM_CTRL3     0x030CU          /* SIFMn_FLUSH bit n */
#define SFR_UAIF_CTRL0(n)  (0x0500U + 0x10U * (n))  /* MIC_EN 1, SPK_EN 0, MODE 2 (1 master), IRQ_MODE 3, DATA_MODE 4 */
#define SFR_UAIF_CTRL1(n)  (0x0504U + 0x10U * (n))
#define SFR_UAIF_STATUS(n) (0x050CU + 0x10U * (n))
#define SFR_CPU_PC         0x2C3CU
#define UAIF_SPK_EN        (1U << 0)
#define UAIF_MIC_EN        (1U << 1)
#define UAIF_MODE_MASTER   (1U << 2)
#define UAIF_IRQ_MODE      (1U << 3)
#define UAIF_DATA_MODE     (1U << 4)
#define UAIF_CTRL1_MASK    0x1FFFFFFFU      /* FORMAT [28:24] .. VALID_END [5:0] */
#define FMT_16_2           9U               /* abox_get_format(16, 2) = (2 - 1) | 1 << 3 */
/* FORMAT 9, BCLK_POLARITY 1 (NB_NF), WS_MODE 0 (I2S), WS_POLAR 0, SLOT_MAX 1, SBIT_MAX 15, VALID 0: 0x0984F000 */
#define UAIF_CTRL1_VAL     ((FMT_16_2 << 24) | (1U << 23) | (1U << 18) | (15U << 12))

/* ---- GIC-400 (arm-gic.h), SysMMU (exynos-iommu.h) --------------------------------------------------------------- */
#define GICD_CTRL       0x000U
#define GICD_PRI        0x400U
#define GICD_SOFTINT    0xF00U
#define GICC_CTRL       0x000U
#define GICC_PRIMASK    0x004U
#define GICC_IAR        0x00CU
#define GICC_EOI        0x010U
#define GICC_DIR        0x1000U
#define MMU_CTRL        0x000U
#define MMU_CFG         0x004U
#define MMU_PT_BASE     0x00CU
#define MMU_FLUSH       0x010U
#define MMU_INT_STATUS  0x060U
#define MMU_INT_CLEAR   0x064U
#define MMU_FAULT_ADDR  0x070U
#define MMU_FAULT_INFO  0x078U
#define MMU_CTRL_ENABLE 0x5U
#define MMU_CTRL_BLOCK  0x7U
#define MMU_CTRL_BLOCK_DISABLE 0x3U
#define MMU_CFG_MASK    0x301F1F8CU
#define SECT            0x100000U
#define PT_L1_BYTES     0x4000U

/* AUD pinctrl (pinctrl-exynos.c exynos9810_pin_banks1, bank_type_6) */
#define GPB0_CON  0x00U
#define GPB0_PUD  0x08U
#define GPB1_CON  0x20U
#define GPB1_PUD  0x28U
#define UAIF0_PINS 0x000FFFF0U              /* gpb0-1..4 (aud-i2s0-bus): function 2, pull 1 */
#define UAIF1_PINS 0x0000FFFFU              /* gpb1-0..3 (aud-i2s1-bus) */

static const uint32_t pm_qos_aud[5] = { 1180000, 800000, 590000, 394000, 0 };   /* the DT's pm_qos_aud */

/* The phone's DT ext_bin list (all area 1 = DRAM); missing ones are logged and skipped, as Samsung does. */
static const struct { const char *name; uint32_t off; } ext_bins[] = {
    { "dsm.bin", 0x502000 },           { "AP_AUDIO_SLSI.bin", 0x7E0000 },     { "APBargeIn_AUDIO_SLSI.bin", 0x7DC000 },
    { "SoundBoosterParam.bin", 0x4FC000 }, { "APDV_AUDIO_SLSI.bin", 0x4D0000 }, { "APBiBF_AUDIO_SLSI.bin", 0x7DF000 },
    { "dsm_tune.bin", 0x601000 },
};

/* ---- state -------------------------------------------------------------------------------------------------------- */
static struct abox_hal H;
static int have_hal;
static struct abox_cfg C = { 0xFB200000ULL, 0x2800000ULL, 0xF7900000ULL, 0x700000ULL, 0xF9000000ULL, 0x200000ULL,
                             0x600000U, 3000U, 2000U };

static struct {
    int state, err;
    uint8_t *rm;                        /* CPU pointer to abox_rmem */
    int pd_touched, cpu_started, mmu_set, pins_set, call_on, vss_started, boot_done, dump_win;
    unsigned qch_set;                   /* bit i: qch_regs[i] CLOCK_REQ set by us */
    uint32_t version, sram_bytes;
    unsigned ipc_sent, ipc_recv, ipc_timeouts, ipc_fail_row, refused, ipc_lines, fw_lines, fw_lost;
    unsigned pointer_msgs, rx_fallback, rx_fallback_pending, fault;
    uint32_t rxq[RXQ][RX_WORDS];
    unsigned rxn;
    struct { uint32_t id, gear; } greq[8];
    uint32_t gear, aud_khz, int_khz;
    struct { uint32_t id, addr; } logs[4];
    unsigned nlogs;
    char line[128];
    unsigned linen;
    uint32_t pcm_ptr[2][8], pcm_ptr_n[2][8];   /* abox_pcm_pointer(): the last POINTER per direction / channel */
} S;

/* abox_map_extra()'s windows (kept across boots) */
#define XMAPS 2
static struct { uint32_t iova, size; uint64_t pa; } xmap[XMAPS];

static uint32_t tx[IPC_WORDS];
static char why_buf[160] = "ABOX off";
static const uint32_t qch_regs[4] = { CMU_QCH_DUMMY, CMU_QCH_SYSMMU, CMU_QCH_BCLK0, CMU_QCH_BCLK1 };

/* ---- small text helpers (no libc) ---------------------------------------------------------------------------------- */
struct lb {
    char b[192];
    unsigned n;
};
static void lb_init(struct lb *l) { l->n = 0; l->b[0] = 0; }
static void lb_c(struct lb *l, char c)
{
    if (l->n + 1 < sizeof(l->b)) {
        l->b[l->n++] = c;
        l->b[l->n] = 0;
    }
}
static void lb_s(struct lb *l, const char *s) { while (s && *s) lb_c(l, *s++); }
static void lb_u(struct lb *l, uint64_t v)
{
    char t[24];
    unsigned i = 0;
    do {
        t[i++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && i < sizeof(t));
    while (i)
        lb_c(l, t[--i]);
}
static void lb_x(struct lb *l, uint64_t v)
{
    static const char hx[] = "0123456789abcdef";
    int started = 0;
    lb_s(l, "0x");
    for (int sh = 60; sh >= 0; sh -= 4) {
        unsigned d = (unsigned)(v >> sh) & 15U;
        if (d || started || sh == 0) {
            lb_c(l, hx[d]);
            started = 1;
        }
    }
}
static void lb_ver(struct lb *l, uint32_t v)
{
    for (int sh = 24; sh >= 0; sh -= 8) {
        char c = (char)(v >> sh);
        lb_c(l, c >= 0x20 && c < 0x7f ? c : '?');
    }
}

static void say(const char *s)
{
    if (have_hal && H.log)
        H.log(H.ctx, s);
}
static void sayl(struct lb *l) { say(l->b); }
static void set_why(const char *s)
{
    unsigned i = 0;
    for (; s[i] && i + 1 < sizeof(why_buf); ++i)
        why_buf[i] = s[i];
    why_buf[i] = 0;
}
/* the line to the log and as abox_why() */
static void sayw(struct lb *l)
{
    sayl(l);
    set_why(l->b + (l->n > 6 && l->b[0] == 'a' && l->b[4] == ':' ? 6 : 0));
}

static uint32_t ab_crc32(const uint8_t *p, uint64_t n)
{
    uint32_t c = 0xFFFFFFFFU;
    for (uint64_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
    }
    return ~c;
}

/* ---- guarded access ------------------------------------------------------------------------------------------------- */
static int in(uint64_t pa, uint64_t base, uint64_t size) { return pa >= base && pa < base + size; }

/* the audio blocks: the only registers this driver writes */
static int reg_ok(uint64_t pa)
{
    if (pa & 3U)
        return 0;
    if (pa == ABOX_PMU + ABOX_PMU_AUD_CONF || pa == ABOX_PMU + ABOX_PMU_CPU_CONF || pa == ABOX_PMU + ABOX_PMU_CPU_OPT ||
        pa == ABOX_PMU + ABOX_PMU_PAD_RET || pa == ABOX_PMU + ABOX_PMU_GPIO_MODE)
        return 1;
    return in(pa, ABOX_CMU, 0x8000) || in(pa, ABOX_SYSREG, 0x1000) || in(pa, ABOX_SFR, 0x10000) ||
           in(pa, ABOX_GPIO, 0x1000) || in(pa, ABOX_GICD, 0x1000) || in(pa, ABOX_GICC, 0x1004) ||
           in(pa, ABOX_SRAM, ABOX_SRAM_SIZE) || in(pa, ABOX_SYSMMU, 0x10000);
}

static uint32_t r32(uint64_t pa) { return H.rd32(H.ctx, pa); }

static void w32(uint64_t pa, uint32_t v)
{
    if (!reg_ok(pa)) {
        struct lb l;
        if (!S.refused++) {
            lb_init(&l);
            lb_s(&l, "abox: REFUSED a write outside the audio blocks (");
            lb_x(&l, pa);
            lb_s(&l, "): a bug, nothing written");
            sayl(&l);
        }
        return;
    }
    H.wr32(H.ctx, pa, v);
}

static void m32(uint64_t pa, uint32_t mask, uint32_t val) { w32(pa, (r32(pa) & ~mask) | (val & mask)); }

/* wait until (reg & mask) == want, at most timeout_us in step_us steps; 1 met */
static int wait32(uint64_t pa, uint32_t mask, uint32_t want, uint32_t timeout_us, uint32_t step_us, uint32_t *took)
{
    uint32_t t = 0;
    for (;;) {
        if ((r32(pa) & mask) == want) {
            if (took)
                *took = t;
            return 1;
        }
        if (t >= timeout_us)
            break;
        H.delay_us(H.ctx, step_us);
        t += step_us;
    }
    if (took)
        *took = t;
    return 0;
}

static void ram_sync(void)
{
    if (H.sync)
        H.sync(H.ctx);
}

/* a pointer into abox_rmem (offset, length), or 0 */
static uint8_t *rm(uint64_t off, uint64_t len)
{
    if (!S.rm || off > C.rmem_size || len > C.rmem_size - off)
        return 0;
    return S.rm + off;
}

/* an ABOX address (SRAM offset below 0x80000000, else an IOVA of a window backed by abox_rmem) -> rmem offset;
 * 1 found (RAM), 2 SRAM, 0 not readable here (VSS / VPARAM / CHIPID / unmapped) */
static int abox_addr(uint32_t a, uint32_t len, uint64_t *off)
{
    static const struct { uint32_t iova, size, rmoff; } w[] = {
        { ABOX_IOVA_DRAM, ABOX_RM_DRAM_SZ, ABOX_RM_DRAM }, { ABOX_IOVA_IVA, SECT, ABOX_RM_IVA },
        { ABOX_IOVA_RDMA4, SECT, ABOX_RM_RDMA4 },           { ABOX_IOVA_WDMA2, SECT, ABOX_RM_WDMA2 },
        { ABOX_IOVA_DUMP, ABOX_RM_DRAM_SZ, ABOX_RM_DUMP },
    };
    if (a < 0x80000000U) {
        if (a >= ABOX_SRAM_SIZE || len > ABOX_SRAM_SIZE - a)
            return 0;
        *off = a;
        return 2;
    }
    for (unsigned i = 0; i < sizeof(w) / sizeof(w[0]); ++i)
        if (a >= w[i].iova && a - w[i].iova < w[i].size && len <= w[i].size - (a - w[i].iova)) {
            if (w[i].iova == ABOX_IOVA_DUMP && !S.dump_win)
                return 0;
            *off = w[i].rmoff + (a - w[i].iova);
            return 1;
        }
    return 0;
}

/* ---- power, clocks, pins ----------------------------------------------------------------------------------------- */
static int pd_is_on(void) { return (r32(ABOX_PMU + ABOX_PMU_AUD_STAT) & 0xFU) == 0xFU; }

static void qch_on(unsigned i)
{
    uint64_t pa = ABOX_CMU + qch_regs[i];
    uint32_t v = r32(pa);
    /* ra_req_enable_qch(): with ENABLE 0 the hardware does not gate by itself, so ask for the clock (CLOCK_REQ) */
    if (!(v & QCH_ENABLE) && !(v & QCH_CLOCK_REQ)) {
        w32(pa, v | QCH_CLOCK_REQ);
        S.qch_set |= 1U << i;
    }
}

static void qch_release(void)
{
    for (unsigned i = 0; i < 4; ++i)
        if (S.qch_set & (1U << i))
            m32(ABOX_CMU + qch_regs[i], QCH_CLOCK_REQ, 0);
    S.qch_set = 0;
}

/* a CMU divider / mux field, then BUSY clear (1 ms); 1 took the value */
static int cmu_set(uint32_t off, uint32_t mask, uint32_t val)
{
    m32(ABOX_CMU + off, mask, val);
    if (!wait32(ABOX_CMU + off, DIV_BUSY, 0, 1000, 10, 0))
        return 0;
    return (r32(ABOX_CMU + off) & mask) == val;
}

static uint64_t pll_hz(void)
{
    uint32_t c0 = r32(ABOX_CMU + CMU_PLL_CON0);
    uint32_t m = (c0 >> 16) & 0x3FFU, p = (c0 >> 8) & 0x3FU, s = c0 & 7U;
    int64_t k = (int16_t)(r32(ABOX_CMU + CMU_PLL_CON3) & 0xFFFFU);
    if (!(c0 & PLL_MUX_SEL))
        return 26000000ULL;
    if (!(c0 & PLL_ENABLE) || !p)
        return 0;
    return (uint64_t)((26000000LL * (((int64_t)m << 16) + k)) / ((int64_t)p << s) >> 16);
}

/* the CA7's clock as the registers say (kHz), 0 if it comes from CMU_TOP (not known here) */
static uint32_t cpu_khz_now(void)
{
    if (r32(ABOX_CMU + CMU_MUX_CPU) & 1U)
        return 0;
    return (uint32_t)(pll_hz() / ((r32(ABOX_CMU + CMU_DIV_PLL) & 0xFU) + 1U) / 1000U);
}

static void pins(int on)
{
    uint32_t f = on ? 0x22222222U : 0;
    m32(ABOX_GPIO + GPB1_CON, UAIF1_PINS, f);
    m32(ABOX_GPIO + GPB1_PUD, UAIF1_PINS, 0x11111111U);
    m32(ABOX_GPIO + GPB0_CON, UAIF0_PINS, f);
    m32(ABOX_GPIO + GPB0_PUD, UAIF0_PINS, 0x11111111U);
    S.pins_set = on;
}

/* ---- IPC ------------------------------------------------------------------------------------------------------------ */
static void msg_new(uint32_t ipcid)
{
    memset(tx, 0, sizeof(tx));
    tx[0] = ipcid;
}
static void msg_sys(uint32_t type, uint32_t p1, uint32_t p2, uint32_t p3)
{
    msg_new(IPC_SYSTEM);
    tx[2] = type, tx[3] = p1, tx[4] = p2, tx[5] = p3;
}
/* IPC_PCMTASK_MSG: msgtype, channel_id, param (task_id = channel_id) */
static void msg_pcm(uint32_t ipcid, uint32_t id, uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    msg_new(ipcid);
    tx[1] = id, tx[2] = type, tx[3] = id, tx[4] = a, tx[5] = b, tx[6] = c;
}

static void rx_take(void);

/* __abox_process_ipc(): the message to SRAM tx, tx_ack 1, SGI (ipcid & 0xF) to ABOX's CPU interface 0, then tx_ack
 * back to 0 within 10 ms. Samsung re-sends while the ack stays set; here at most 3 tries, then the ack word is cleared
 * (its failsafe) and the send fails. Messages arriving meanwhile are queued, not handled (no nesting). */
static int ipc_send(const char *what)
{
    uint32_t ipcid = tx[0];
    if (S.state != ABOX_ST_BOOTING && S.state != ABOX_ST_READY && S.state != ABOX_ST_CALL)
        return ABOX_ESTATE;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        for (unsigned i = 0; i < IPC_WORDS; ++i)
            w32(ABOX_SRAM + ABOX_IPC_TX + 4U * i, tx[i]);
        w32(ABOX_SRAM + ABOX_IPC_TX_ACK, 1);
        w32(ABOX_GICD + GICD_SOFTINT, (1U << 16) | (ipcid & 0xFU));
        for (uint32_t t = 0; t <= 10000U; t += 10U) {
            if (!r32(ABOX_SRAM + ABOX_IPC_TX_ACK)) {
                S.ipc_sent++;
                S.ipc_fail_row = 0;
                H.delay_us(H.ctx, 20);          /* "giving time to ABOX for processing" */
                return 0;
            }
            if (!(t % 200U))
                rx_take();
            H.delay_us(H.ctx, 10);
        }
    }
    w32(ABOX_SRAM + ABOX_IPC_TX_ACK, 0);
    S.ipc_timeouts++;
    S.ipc_fail_row++;
    struct lb l;
    lb_init(&l);
    lb_s(&l, "abox: IPC ");
    lb_s(&l, what);
    lb_s(&l, " not taken by Calliope (3 x 10 ms)");
    sayw(&l);
    return ABOX_EIPC;
}

/* ---- Calliope's log rings (abox_log.c) --------------------------------------------------------------------------- */
static void fw_line_out(void)
{
    struct lb l;
    if (!S.linen)
        return;
    S.line[S.linen] = 0;
    if (S.fw_lines < C.fw_log_max) {
        lb_init(&l);
        lb_s(&l, "abox: fw: ");
        lb_s(&l, S.line);
        sayl(&l);
        if (++S.fw_lines == C.fw_log_max)
            say("abox: fw: (further Calliope log lines are not copied: fw_log_max)");
    }
    S.linen = 0;
}

static void fw_char(char c)
{
    if (c == '\n' || c == '\r' || c == 0) {
        fw_line_out();
        return;
    }
    S.line[S.linen++] = (c >= 0x20 && c < 0x7f) ? c : '.';
    if (S.linen >= sizeof(S.line) - 1)
        fw_line_out();
}

/* a byte of an ABOX address window: RAM pointer or SRAM word */
static uint8_t rd8(int kind, uint64_t off)
{
    if (kind == 1)
        return S.rm[off];
    uint32_t w = r32(ABOX_SRAM + (off & ~3ULL));
    return (uint8_t)(w >> (8U * (unsigned)(off & 3U)));
}
static uint32_t rdw(int kind, uint64_t off)
{
    if (kind == 1) {
        const uint8_t *p = S.rm + off;
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    return r32(ABOX_SRAM + off);
}
static void wrw(int kind, uint64_t off, uint32_t v)
{
    if (kind == 1) {
        uint8_t *p = S.rm + off;
        p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
    } else {
        w32(ABOX_SRAM + off, v);
    }
}

static void fw_log_flush(void)
{
    for (unsigned b = 0; b < S.nlogs; ++b) {
        uint64_t off;
        int kind = abox_addr(S.logs[b].addr, 12, &off);
        if (!kind || (off & 3U))
            continue;
        uint32_t wr = rdw(kind, off), rd = rdw(kind, off + 4), size = rdw(kind, off + 8);
        uint64_t data;
        if (!size || size > 0x100000U || wr >= size || rd >= size || abox_addr(S.logs[b].addr + 12U, size, &data) != kind)
            continue;
        unsigned budget = 8192;                     /* bytes per flush and ring */
        while (rd != wr && budget--) {
            if (S.fw_lines < C.fw_log_max)
                fw_char((char)rd8(kind, data + rd));
            else
                S.fw_lost++;
            rd = rd + 1U == size ? 0 : rd + 1U;
        }
        wrw(kind, off + 4, rd);                     /* index_reader */
    }
}

/* ---- gears (abox_change_cpu_gear on the 9810: pm_qos AUD, then ABOX_CHANGED_GEAR) -------------------------------- */
static uint32_t gear_khz(uint32_t gear) { return gear >= 1 && gear <= 5 ? pm_qos_aud[gear - 1] : 0; }

static void gear_set(uint32_t id, uint32_t gear, int notify)
{
    unsigned i, free = 8;
    struct lb l;
    for (i = 0; i < 8; ++i) {
        if (S.greq[i].id == id)
            break;
        if (!S.greq[i].id && free == 8)
            free = i;
    }
    if (i == 8)
        i = free;
    if (i < 8) {
        S.greq[i].id = id;
        S.greq[i].gear = gear;
    }
    uint32_t g = GEAR_MIN;
    for (i = 0; i < 8; ++i)
        if (S.greq[i].id && S.greq[i].gear < g)
            g = S.greq[i].gear;
    if (g < 1)
        g = 1;
    uint32_t khz = gear_khz(g), report;
    if (H.dvfs_aud) {
        if (khz != S.aud_khz) {
            int r = H.dvfs_aud(H.ctx, khz);
            lb_init(&l);
            lb_s(&l, "abox: gear ");
            lb_u(&l, g);
            lb_s(&l, ": ACPM AUD ");
            lb_u(&l, khz);
            lb_s(&l, " kHz asked, answer ");
            lb_u(&l, (uint64_t)(r < 0 ? 0 : r));
            lb_s(&l, r < 0 ? " (failed)" : "");
            sayl(&l);
            S.aud_khz = khz;
        }
        report = khz;
    } else {
        report = cpu_khz_now();
    }
    S.gear = g;
    if (!notify || (S.state != ABOX_ST_READY && S.state != ABOX_ST_CALL))
        return;
    uint64_t us = H.now_us ? H.now_us(H.ctx) : 0;
    msg_sys(SYS_CHANGED_GEAR, report * 1000U, (uint32_t)(us / 1000000U), (uint32_t)(us % 1000000U) * 1000U);
    (void)ipc_send("CHANGED_GEAR");
}

/* ---- receiving (abox_gic_irq_handler + abox_irq_handler) --------------------------------------------------------- */
static int is_dma_irq(uint32_t id) { return id == 8 || id == 9 || (id >= 0xB && id <= 0xE); }

static void rx_copy(void)
{
    for (unsigned i = 0; i < RX_WORDS; ++i)
        S.rxq[S.rxn][i] = r32(ABOX_SRAM + ABOX_IPC_RX + 4U * i);
    w32(ABOX_SRAM + ABOX_IPC_RX_ACK, 0);
    S.rxn++;
    S.ipc_recv++;
}

/* drain the AP's view of ABOX's GIC into the queue: SGIs below 16 carry a message (DMA ones do not); EOI and DIR */
static void rx_take(void)
{
    for (unsigned n = 0; n < 16 && S.rxn < RXQ; ++n) {
        uint32_t iar = r32(ABOX_GICC + GICC_IAR), id = iar & 0x3FFU;
        if (id >= 1020U) {
            /* nothing pending at the GIC; a message posted without an interrupt reaching us is taken too */
            if (r32(ABOX_SRAM + ABOX_IPC_RX_ACK)) {
                if (!S.rx_fallback++)
                    say("abox: a message was waiting with no interrupt at ABOX's GICC: taken from SRAM (logged once)");
                S.rx_fallback_pending++;
                rx_copy();
                continue;
            }
            return;
        }
        if (id < 16U) {
            if (!is_dma_irq(id)) {
                if (!r32(ABOX_SRAM + ABOX_IPC_RX_ACK) && S.rx_fallback_pending)
                    S.rx_fallback_pending--;    /* its message was already taken above */
                else
                    rx_copy();
            }
            w32(ABOX_GICC + GICC_EOI, iar);
            w32(ABOX_GICC + GICC_DIR, iar);
        } else {
            w32(ABOX_GICC + GICC_EOI, iar);
        }
    }
}

static void ipc_note(const char *what, uint32_t a, uint32_t b, uint32_t c)
{
    struct lb l;
    if (S.ipc_lines >= 200U)
        return;
    if (++S.ipc_lines == 200U) {
        say("abox: (further IPC notes are not logged)");
        return;
    }
    lb_init(&l);
    lb_s(&l, "abox: Calliope: ");
    lb_s(&l, what);
    lb_s(&l, " (");
    lb_x(&l, a);
    lb_s(&l, ", ");
    lb_x(&l, b);
    lb_s(&l, ", ");
    lb_x(&l, c);
    lb_s(&l, ")");
    sayl(&l);
}

static void handle(const uint32_t *m)
{
    uint32_t type = m[2], p1 = m[3], p2 = m[4], p3 = m[5];
    struct lb l;
    if (m[0] == IPC_PCMPLAYBACK || m[0] == IPC_PCMCAPTURE) {
        if (type == PCM_PLTDAI_POINTER) {
            if (p1 < 8U) {
                S.pcm_ptr[m[0] == IPC_PCMCAPTURE][p1] = m[4];
                S.pcm_ptr_n[m[0] == IPC_PCMCAPTURE][p1]++;
            }
            if (!S.pointer_msgs++)
                ipc_note(m[0] == IPC_PCMPLAYBACK ? "first period pointer, playback (channel)"
                                                 : "first period pointer, capture (channel)", p1, 0, 0);
        } else {
            ipc_note(m[0] == IPC_PCMPLAYBACK ? "playback task message" : "capture task message", type, p1, m[4]);
        }
        return;
    }
    if (m[0] != IPC_SYSTEM) {
        ipc_note("message of IPC id", m[0], type, p1);
        return;
    }
    switch (type) {
    case SYS_BOOT_DONE:
        S.version = p3;
        S.boot_done = 1;
        if ((r32(ABOX_SRAM + 0x30054U) >> 24) == 'A')   /* abox_is_calliope_incompatible(): param3 of 0x30040 */
            say("abox: Calliope says it is not compatible with this driver version ('A'): going on, as Samsung does");
        break;
    case SYS_CHANGE_GEAR:
        ipc_note("CHANGE_GEAR (gear, id)", p1, p2, 0);
        gear_set(p2, p1, 1);
        break;
    case SYS_REQUEST_SYSCLK:
        if (p2 == 1 && H.dvfs_int) {
            int r = H.dvfs_int(H.ctx, p1);
            ipc_note(r < 0 ? "REQUEST_SYSCLK INT (kHz, id): the hook failed" : "REQUEST_SYSCLK INT (kHz, id): asked",
                     p1, p3, 0);
        } else {
            ipc_note(p2 == 1 ? "REQUEST_SYSCLK INT (kHz, id): no INT hook, not served"
                             : "REQUEST_SYSCLK MIF (kHz, id): not served", p1, p3, 0);
        }
        break;
    case SYS_REPORT_LOG: {
        uint64_t off;
        int kind = abox_addr(p2, 12, &off);
        lb_init(&l);
        lb_s(&l, "abox: Calliope log ring ");
        lb_u(&l, p1);
        lb_s(&l, " at ABOX ");
        lb_x(&l, p2);
        if (!kind || (off & 3U)) {
            lb_s(&l, ": not in SRAM or a mapped window, not read");
        } else if (S.nlogs < 4) {
            S.logs[S.nlogs].id = p1;
            S.logs[S.nlogs++].addr = p2;
            lb_s(&l, kind == 2 ? " (SRAM): copied to this log as \"abox: fw:\"" : " (DRAM): copied to this log as \"abox: fw:\"");
        }
        sayl(&l);
        break;
    }
    case SYS_FLUSH_LOG:
        fw_log_flush();
        break;
    case SYS_REPORT_DUMP:
        ipc_note("REPORT_DUMP (id, addr, size): no dump reader here", p1, p2, p3);
        break;
    case SYS_FLUSH_DUMP:
        break;
    case SYS_REQUEST_L2C:
    case SYS_END_L2C:
        ipc_note("L2C message (no VTS SRAM sharing on the 9810: nothing to do)", type, p1, p2);
        break;
    case SYS_REPORT_COMPONENT:
        ipc_note("REPORT_COMPONENT (not registered here)", p1, 0, 0);
        break;
    case SYS_REPORT_FAULT: {
        static const char *const kinds[] = { "unknown error", "data abort", "prefetch abort", "os error", "vss error",
                                             "undefined exception" };
        S.fault = 1;
        lb_init(&l);
        lb_s(&l, "abox: Calliope reports a fault: ");
        lb_s(&l, kinds[p1 <= 5 ? p1 : 0]);
        lb_s(&l, " (");
        lb_x(&l, p1);
        lb_s(&l, ", ");
        lb_x(&l, p2);
        lb_s(&l, ", ");
        lb_x(&l, p3);
        lb_s(&l, ")");
        sayw(&l);
        break;
    }
    default:
        ipc_note("other system message (type, p1, p2)", type, p1, p2);
        break;
    }
}

static void rx_process(void)
{
    uint32_t m[RX_WORDS];
    while (S.rxn) {
        memcpy(m, S.rxq[0], sizeof(m));
        for (unsigned i = 1; i < S.rxn; ++i)
            memcpy(S.rxq[i - 1], S.rxq[i], sizeof(S.rxq[0]));
        S.rxn--;
        handle(m);
    }
}

/* ---- stopping ------------------------------------------------------------------------------------------------------- */
static int cpu_stop(void)
{
    m32(ABOX_PMU + ABOX_PMU_CPU_OPT, 1U << 28, 0);      /* abox_cpu_enable(false) */
    m32(ABOX_PMU + ABOX_PMU_CPU_CONF, 1U, 0);           /* abox_cpu_power(false) */
    return wait32(ABOX_PMU + ABOX_PMU_CPU_STAT, 1U, 0, 10000, 10, 0);
}

/* everything back off, whatever was reached; logs one line */
static void teardown(void)
{
    struct lb l;
    int cpu_ok = 1, pd_ok = 1;
    int64_t smc = 0;
    if (S.pd_touched && pd_is_on()) {
        if (S.cpu_started || (r32(ABOX_PMU + ABOX_PMU_CPU_STAT) & 1U))
            cpu_ok = cpu_stop();
        if (S.mmu_set) {
            w32(ABOX_SYSMMU + MMU_CFG, 0);                      /* __sysmmu_disable_nocount() */
            w32(ABOX_SYSMMU + MMU_CTRL, MMU_CTRL_BLOCK_DISABLE);
        }
        m32(ABOX_SFR + SFR_SYSPOWER_CTRL, 1U, 0);
        if (S.pins_set)
            pins(0);                                            /* aud-i2s*-idle: function 0, pull 1 */
        m32(ABOX_PMU + ABOX_PMU_GPIO_MODE, 1U, 1U);             /* abox_pad_retention(true) */
        qch_release();
        if (H.dvfs_aud && S.aud_khz)
            (void)H.dvfs_aud(H.ctx, 0);
        if (H.dvfs_int && S.int_khz)
            (void)H.dvfs_int(H.ctx, 0);
        smc = H.smc(H.ctx, ABOX_SMC_PD, 0, ABOX_DTZPC, 0);     /* exynos_pd_tz_save() */
        m32(ABOX_PMU + ABOX_PMU_AUD_CONF, 0xFU, 0);             /* aud_off */
        pd_ok = wait32(ABOX_PMU + ABOX_PMU_AUD_STAT, 0xFU, 0, 10000, 10, 0);
    }
    lb_init(&l);
    lb_s(&l, "abox: stopped: CPU ");
    lb_s(&l, cpu_ok ? "off" : "DID NOT REPORT OFF");
    lb_s(&l, ", AUD domain ");
    lb_s(&l, !S.pd_touched ? "not touched" : pd_ok ? "off" : "STILL ON");
    if (S.pd_touched) {
        lb_s(&l, " (TZPC save SMC ");
        lb_u(&l, (uint64_t)(smc < 0 ? -smc : smc));
        lb_s(&l, smc < 0 ? " negative)" : ")");
    }
    sayl(&l);
    S.pd_touched = S.cpu_started = S.mmu_set = S.pins_set = S.call_on = S.boot_done = S.vss_started = 0;
    S.aud_khz = S.int_khz = 0;
    S.nlogs = 0;
}

static int fail(int err, struct lb *l)
{
    sayw(l);
    char keep[sizeof(why_buf)];
    memcpy(keep, why_buf, sizeof(keep));
    teardown();
    memcpy(why_buf, keep, sizeof(why_buf));
    S.state = ABOX_ST_FAILED;
    S.err = err;
    return err;
}

/* ---- set-up ----------------------------------------------------------------------------------------------------------- */
void abox_hal_set(const struct abox_hal *h)
{
    if (!h || !h->rd32 || !h->wr32 || !h->smc || !h->delay_us || !h->log || !h->file || !h->ram) {
        have_hal = 0;
        return;
    }
    H = *h;
    have_hal = 1;
}

void abox_cfg_default(struct abox_cfg *c)
{
    c->rmem_base = 0xFB200000ULL, c->rmem_size = 0x2800000ULL;
    c->vss_base = 0xF7900000ULL, c->vss_size = 0x700000ULL;
    c->vparam_base = 0xF9000000ULL, c->vparam_size = 0x200000ULL;
    c->vss_magic_off = 0x600000U, c->boot_timeout_ms = 3000U, c->fw_log_max = 2000U;
}

int abox_cfg_set(const struct abox_cfg *c)
{
    uint64_t top = 1ULL << 32;
    if (!c || (c->rmem_base | c->vss_base | c->vparam_base) & (SECT - 1U) || c->rmem_size < ABOX_RM_NEED ||
        c->vss_size > 0x1000000U || c->vparam_size > 0x1000000U || !c->vss_size || !c->vparam_size ||
        c->rmem_base + c->rmem_size > top || c->vss_base + c->vss_size > top || c->vparam_base + c->vparam_size > top ||
        (c->vss_magic_off & 3U) || c->vss_magic_off + 4U > 0x1000000U || !c->boot_timeout_ms ||
        c->boot_timeout_ms > 60000U || (S.state != ABOX_ST_OFF && S.state != ABOX_ST_FAILED))
        return ABOX_ECFG;
    C = *c;
    return 0;
}

int abox_state(void) { return S.state; }
uint32_t abox_version(void) { return S.version; }
const char *abox_why(void) { return why_buf; }

/* step 1: the VENDOR files into abox_rmem (nothing else touched); 0 or ABOX_EFILE */
static long load(const char *name, uint64_t off, uint32_t cap, int required)
{
    char path[64] = "firmware/";
    unsigned n = 9;
    struct lb l;
    for (unsigned i = 0; name[i] && n + 1 < sizeof(path); ++i)
        path[n++] = name[i];
    path[n] = 0;
    long got = H.file(H.ctx, path, rm(off, cap), cap);
    lb_init(&l);
    lb_s(&l, "abox: 1/13 files: ");
    lb_s(&l, name);
    if (got <= 0) {
        lb_s(&l, required ? ": MISSING or larger than its place: cannot boot" : ": missing or too large, skipped (as Samsung does)");
        if (required)
            sayw(&l);
        else
            sayl(&l);
        return -1;
    }
    lb_s(&l, " ");
    lb_u(&l, (uint64_t)got);
    lb_s(&l, " bytes, crc32 ");
    lb_x(&l, ab_crc32(rm(off, (uint64_t)got), (uint64_t)got));
    sayl(&l);
    return got;
}

static int step_files(void)
{
    long sram, dram, iva;
    S.rm = (uint8_t *)H.ram(H.ctx, C.rmem_base, C.rmem_size);
    if (!S.rm)
        return -1;
    S.dump_win = C.rmem_size >= ABOX_RM_DUMP + ABOX_RM_DRAM_SZ;
    sram = load("calliope_sram.bin", ABOX_RM_RDMA4, ABOX_SRAM_SIZE, 1);   /* scratch until step 9 */
    if (sram < 0)
        return ABOX_EFILE;
    S.sram_bytes = (uint32_t)sram;
    dram = load("calliope_dram.bin", ABOX_RM_DRAM, ABOX_RM_DRAM_SZ, 1);
    if (dram < 0)
        return ABOX_EFILE;
    memset(rm(ABOX_RM_DRAM + (uint64_t)dram, ABOX_RM_DRAM_SZ - (uint64_t)dram), 0, ABOX_RM_DRAM_SZ - (uint64_t)dram);
    iva = load("calliope_iva.bin", ABOX_RM_IVA, 0x80000U, 0);
    if (iva < 0)
        iva = 0;
    memset(rm(ABOX_RM_IVA + (uint64_t)iva, SECT - (uint64_t)iva), 0, SECT - (uint64_t)iva);
    for (unsigned i = 0; i < sizeof(ext_bins) / sizeof(ext_bins[0]); ++i)
        (void)load(ext_bins[i].name, ABOX_RM_DRAM + ext_bins[i].off, ABOX_RM_DRAM_SZ - ext_bins[i].off, 0);
    return 0;
}

/* step 5: our own SysMMU tables (S-Boot leaves this SysMMU translating with tables nobody here knows) */
static void map_sect(uint32_t *l1, uint32_t iova, uint64_t pa, uint64_t size)
{
    for (uint64_t o = 0; o < size; o += SECT)
        l1[(iova + o) >> 20] = (uint32_t)((pa + o) >> 4) | 2U;     /* mk_lv1ent_sect() */
}

static int step_sysmmu(void)
{
    uint32_t *l1 = (uint32_t *)(void *)rm(ABOX_RM_PT, PT_L1_BYTES), *l2 = (uint32_t *)(void *)rm(ABOX_RM_PT_L2, 0x400);
    uint64_t pt = C.rmem_base + ABOX_RM_PT, l2pa = C.rmem_base + ABOX_RM_PT_L2;
    struct lb l;
    if (!l1 || !l2)
        return 0;
    memset(l1, 0, PT_L1_BYTES);
    memset(l2, 0, 0x400);
    map_sect(l1, ABOX_IOVA_DRAM, C.rmem_base + ABOX_RM_DRAM, ABOX_RM_DRAM_SZ);
    map_sect(l1, ABOX_IOVA_IVA, C.rmem_base + ABOX_RM_IVA, SECT);
    map_sect(l1, ABOX_IOVA_RDMA4, C.rmem_base + ABOX_RM_RDMA4, SECT);
    map_sect(l1, ABOX_IOVA_WDMA2, C.rmem_base + ABOX_RM_WDMA2, SECT);
    map_sect(l1, ABOX_IOVA_VSS, C.vss_base, C.vss_size);
    map_sect(l1, ABOX_IOVA_VPARAM, C.vparam_base, C.vparam_size);
    if (S.dump_win)
        map_sect(l1, ABOX_IOVA_DUMP, C.rmem_base + ABOX_RM_DUMP, ABOX_RM_DRAM_SZ);
    l1[ABOX_IOVA_CHIPID >> 20] = (uint32_t)(l2pa >> 4) | 1U;      /* mk_lv1ent_page(): a 1 KiB L2 table */
    l2[0] = (uint32_t)(ABOX_IOVA_CHIPID >> 4) | 2U;               /* mk_lv2ent_spage(0x10000000): 4 KiB, 1:1 */
    for (unsigned i = 0; i < XMAPS; ++i) {                        /* abox_map_extra(): one L2 table each */
        uint32_t *x2 = (uint32_t *)(void *)rm(ABOX_RM_PT_L2 + 0x400U * (i + 1U), 0x400);
        if (!xmap[i].size || !x2)
            continue;
        lb_init(&l);
        lb_s(&l, "abox: 5/13 SysMMU: extra window ");
        lb_x(&l, xmap[i].iova);
        if (l1[xmap[i].iova >> 20] || xmap[i].pa < C.rmem_base + ABOX_RM_PT + 0x10000U ||
            xmap[i].pa + xmap[i].size > C.rmem_base + ABOX_RM_DUMP) {
            lb_s(&l, " clashes with one of ours or is outside abox_rmem's free section: not mapped");
            sayl(&l);
            continue;
        }
        memset(x2, 0, 0x400);
        for (uint32_t o = 0; o < xmap[i].size; o += 0x1000U)
            x2[o >> 12] = (uint32_t)((xmap[i].pa + o) >> 4) | 2U;
        l1[xmap[i].iova >> 20] = (uint32_t)((l2pa + 0x400U * (i + 1U)) >> 4) | 1U;
        lb_s(&l, " -> ");
        lb_x(&l, xmap[i].pa);
        lb_s(&l, " (");
        lb_u(&l, xmap[i].size >> 10);
        lb_s(&l, " KiB, 4 KiB pages)");
        sayl(&l);
    }
    ram_sync();
    uint32_t was = r32(ABOX_SYSMMU + MMU_CTRL);
    /* __sysmmu_enable_nocount(): BLOCK, CFG (keep the bits outside CFG_MASK), PT_BASE_PPN, FLUSH, ENABLE */
    w32(ABOX_SYSMMU + MMU_CTRL, MMU_CTRL_BLOCK);
    w32(ABOX_SYSMMU + MMU_CFG, r32(ABOX_SYSMMU + MMU_CFG) & ~MMU_CFG_MASK);
    w32(ABOX_SYSMMU + MMU_PT_BASE, (uint32_t)(pt >> 12));
    w32(ABOX_SYSMMU + MMU_FLUSH, 1);
    w32(ABOX_SYSMMU + MMU_INT_CLEAR, r32(ABOX_SYSMMU + MMU_INT_STATUS));
    w32(ABOX_SYSMMU + MMU_CTRL, MMU_CTRL_ENABLE);
    S.mmu_set = 1;
    lb_init(&l);
    lb_s(&l, "abox: 5/13 SysMMU: was CTRL ");
    lb_x(&l, was);
    lb_s(&l, "; now our tables (DRAM 18M, IVA, RDMA4, WDMA2, VSS ");
    lb_u(&l, C.vss_size >> 20);
    lb_s(&l, "M, VSS PARAM ");
    lb_u(&l, C.vparam_size >> 20);
    lb_s(&l, S.dump_win ? "M, dump 18M, CHIPID page), CTRL " : "M, CHIPID page), CTRL ");
    lb_x(&l, r32(ABOX_SYSMMU + MMU_CTRL));
    sayl(&l);
    return 1;
}

static int step_clocks(struct lb *l)
{
    uint32_t c0 = r32(ABOX_CMU + CMU_PLL_CON0), took = 0;
    uint32_t want = (PLL_M << 16) | (PLL_P << 8) | PLL_S;
    int cpu_from_pll = !(r32(ABOX_CMU + CMU_MUX_CPU) & 1U);
    w32(ABOX_SYSREG + SYSREG_MISC_CON, 1);                      /* abox_enable(): MISC_CON 1 */
    qch_on(0);                                                  /* clk_enable(clk_cpu): GATE_ABOX_DUMMY */
    if (!H.dvfs_aud && cpu_from_pll && !cmu_set(CMU_DIV_PLL, 0xFU, DIV_PLL_IDLE)) {
        lb_s(l, "abox: 4/13 clocks: DIV_CLK_AUD_PLL did not take the idle ratio");
        return 0;
    }
    /* the dividers first, so that nothing runs faster than meant when the PLL comes up */
    if (!cmu_set(CMU_DIV_BUS, 0x7U, DIV_BUS_VAL) || !cmu_set(CMU_DIV_BUSP, 0x3U, DIV_BUSP_VAL) ||
        !cmu_set(CMU_DIV_AUDIF, 0x1FFU, DIV_AUDIF_BOOT) || !cmu_set(CMU_MUX_UAIF0, 1U, 0) ||
        !cmu_set(CMU_MUX_UAIF1, 1U, 0) || !cmu_set(CMU_DIV_UAIF0, 0x1FFU, 15U) || !cmu_set(CMU_DIV_UAIF1, 0x1FFU, 15U)) {
        lb_s(l, "abox: 4/13 clocks: a CMU_AUD divider or mux stayed busy / did not take its value");
        return 0;
    }
    if ((c0 & (PLL_ENABLE | PLL_STABLE | PLL_MUX_SEL)) == (PLL_ENABLE | PLL_STABLE | PLL_MUX_SEL) &&
        (c0 & PLL_MPS_MASK) == want && (r32(ABOX_CMU + CMU_PLL_CON3) & 0xFFFFU) == PLL_K) {
        say("abox: 4/13 clocks: PLL_AUD already at 1179.648 MHz (M 45 P 1 S 0 K 24319): left so");
    } else {
        /* ra_enable_pll(0) + ra_pll_set_pmsk(): mux to OSCCLK, off, lock time, K, M/P/S with ENABLE and MUX_SEL */
        if (c0 & PLL_MUX_SEL) {
            m32(ABOX_CMU + CMU_PLL_CON0, PLL_MUX_SEL, 0);
            if (!wait32(ABOX_CMU + CMU_PLL_CON0, PLL_MUX_BUSY, 0, 1000, 10, 0)) {
                lb_s(l, "abox: 4/13 clocks: PLL_AUD's mux stayed busy");
                return 0;
            }
        }
        m32(ABOX_CMU + CMU_PLL_CON0, PLL_ENABLE, 0);
        m32(ABOX_CMU + CMU_PLL_LOCKTIME, 0xFFFFFU, PLL_P * PLL_FLOCK);
        m32(ABOX_CMU + CMU_PLL_CON3, 0xFFFFU, PLL_K);
        m32(ABOX_CMU + CMU_PLL_CON0, PLL_MPS_MASK | PLL_MUX_SEL | PLL_ENABLE, want | PLL_MUX_SEL | PLL_ENABLE);
        if (!wait32(ABOX_CMU + CMU_PLL_CON0, PLL_STABLE, PLL_STABLE, 5000, 10, &took)) {
            lb_s(l, "abox: 4/13 clocks: PLL_AUD not stable after 5 ms");
            return 0;
        }
    }
    uint64_t hz = pll_hz();
    if (hz < 1179000000ULL || hz > 1180300000ULL) {
        lb_s(l, "abox: 4/13 clocks: PLL_AUD is not at the 48 kHz family's 1179.648 MHz");
        return 0;
    }
    qch_on(1);                                                  /* the SysMMU's clock (GATE_SMMU_ABOX) */
    qch_on(2);                                                  /* UAIF0 / UAIF1 bclk_gate */
    qch_on(3);
    if (H.dvfs_aud)
        gear_set(GEAR_ID_DEFAULT, GEAR_MAX, 0);                 /* abox_request_cpu_gear_sync(.., MAX) */
    struct lb m;
    lb_init(&m);
    lb_s(&m, "abox: 4/13 clocks: PLL_AUD ");
    lb_u(&m, hz);
    lb_s(&m, " Hz (stable after ");
    lb_u(&m, took);
    lb_s(&m, " us), ACLK / ");
    lb_u(&m, (r32(ABOX_CMU + CMU_DIV_BUS) & 7U) + 1U);
    lb_s(&m, ", AUDIF / ");
    lb_u(&m, (r32(ABOX_CMU + CMU_DIV_AUDIF) & 0x1FFU) + 1U);
    lb_s(&m, ", CA7 ");
    if (!cpu_from_pll)
        lb_s(&m, "from CMU_TOP (rate not known here)");
    else {
        lb_u(&m, cpu_khz_now());
        lb_s(&m, " kHz");
    }
    lb_s(&m, H.dvfs_aud ? " (ACPM AUD asked)" : " (no AUD DVFS hook: idle ratio)");
    sayl(&m);
    return 1;
}

int abox_boot(void)
{
    struct lb l;
    uint32_t took = 0;
    int r;
    if (!have_hal) {
        set_why("no HAL: abox_hal_set() first");
        return ABOX_ESTATE;
    }
    if (S.state == ABOX_ST_READY || S.state == ABOX_ST_CALL) {
        say("abox: boot: Calliope already runs");
        return 0;
    }
    if (S.state == ABOX_ST_BOOTING)
        return ABOX_ESTATE;
    memset(&S, 0, sizeof(S));
    S.state = ABOX_ST_BOOTING;
    S.gear = GEAR_MIN;
    lb_init(&l);

    /* 1: files (no hardware touched) */
    r = step_files();
    if (r) {
        S.state = ABOX_ST_FAILED;
        S.err = r < 0 && r != -1 ? r : ABOX_EMMU;
        if (r == -1) {
            lb_s(&l, "abox: 1/13 files: abox_rmem is not mappable as RAM: nothing touched");
            sayw(&l);
        }
        return S.err;
    }

    /* 2: the AUD power domain (flexpmu aud_on), then the DTZPC restore (exynos_pd_tz_restore) */
    if (pd_is_on()) {
        say("abox: 2/13 power: the AUD domain is already on (S-Boot): no TZPC restore needed");
        S.pd_touched = 1;
    } else {
        S.pd_touched = 1;
        m32(ABOX_PMU + ABOX_PMU_AUD_CONF, 0xFU, 0xFU);
        if (!wait32(ABOX_PMU + ABOX_PMU_AUD_STAT, 0xFU, 0xFU, 10000, 10, &took)) {
            lb_s(&l, "abox: 2/13 power: the AUD domain did not come on in 10 ms");
            return fail(ABOX_EPOWER, &l);
        }
        int64_t smc = H.smc(H.ctx, ABOX_SMC_PD, 1, ABOX_DTZPC, 0);
        if (smc) {
            lb_s(&l, "abox: 2/13 power: the DTZPC restore SMC failed (");
            lb_x(&l, (uint64_t)smc);
            lb_s(&l, ")");
            return fail(ABOX_EPOWER, &l);
        }
        lb_s(&l, "abox: 2/13 power: AUD domain on in ");
        lb_u(&l, took);
        lb_s(&l, " us, DTZPC restored (SMC 0x82000410 answered 0)");
        sayl(&l);
        lb_init(&l);
    }
    uint32_t ip = r32(ABOX_SFR + SFR_IP_INDEX);
    if (ip != 0x41424F58U && ip != 0x584F4241U) {
        lb_s(&l, "abox: 2/13 power: ABOX IP_INDEX reads ");
        lb_x(&l, ip);
        lb_s(&l, ", not \"ABOX\": the block does not answer");
        return fail(ABOX_EPOWER, &l);
    }
    lb_s(&l, "abox: 2/13 power: IP_INDEX \"ABOX\", VERSION ");
    lb_x(&l, r32(ABOX_SFR + SFR_VERSION));
    sayl(&l);
    lb_init(&l);

    /* 3: whatever ABOX's CPU runs from S-Boot is stopped before its memory changes */
    if (r32(ABOX_PMU + ABOX_PMU_CPU_STAT) & 1U) {
        if (!cpu_stop()) {
            lb_s(&l, "abox: 3/13 cpu: ABOX's CPU was on (S-Boot) and did not power down in 10 ms");
            return fail(ABOX_ECPU, &l);
        }
        say("abox: 3/13 cpu: ABOX's CPU was on (left by S-Boot): stopped");
    } else {
        say("abox: 3/13 cpu: ABOX's CPU is off");
    }

    /* 4: clocks */
    if (!step_clocks(&l))
        return fail(ABOX_ECLOCK, &l);

    /* 5: SysMMU */
    if (!step_sysmmu()) {
        lb_s(&l, "abox: 5/13 SysMMU: the page tables do not fit in abox_rmem");
        return fail(ABOX_EMMU, &l);
    }

    /* 6: ABOX's GIC (abox_gic_init_gic, GIC_IS_SECURE_FREE: plain writes) */
    w32(ABOX_GICC + GICC_PRIMASK, 0xFFU);
    w32(ABOX_GICD + GICD_CTRL, 3U);
    for (unsigned i = 0; i < 40; ++i)
        w32(ABOX_GICD + GICD_PRI + 4U * i, 0x10101010U);
    w32(ABOX_GICC + GICC_CTRL, 3U);
    say("abox: 6/13 gic: PRIMASK 0xff, GICD CTRL 3, priorities 0x10, GICC CTRL 3");

    /* 7: pins (the uaif nodes' "default" pinctrl) */
    pins(1);
    say("abox: 7/13 pins: gpb1-0..3 (UAIF1, amplifier) and gpb0-1..4 (UAIF0, codec): function 2, pull 1");

    /* 8: the AP-owned routing registers as Samsung's mixer defaults leave them (regcache_sync before the CPU) */
    m32(ABOX_SFR + SFR_ROUTE_CTRL0, 0xFFFFU, 0);                /* UAIF0..3 SPK = RESERVED */
    m32(ABOX_SFR + SFR_ROUTE_CTRL1, 0xFFFFU, 0);                /* NSRC0..3 = RESERVED */
    m32(ABOX_SFR + SFR_ROUTE_CTRL2, 0xFFU, 0);                  /* RSRC0..1 = RESERVED */
    m32(ABOX_SFR + SFR_SPUS_CTRL0, 0xFU << 16, 0x3U << 16);     /* SPUS IN4 RDMA, ASRC4 On, OUT4 SIFS0 */
    m32(ABOX_SFR + SFR_SPUM_CTRL0, (1U << 0) | (1U << 8) | (1U << 11), (1U << 0) | (1U << 8));  /* ASRC0/2 On, SIFM1 WDMA */
    m32(ABOX_SFR + SFR_SPUM_CTRL1, 0x3U, 0);                    /* RECP PIFS0/1 = 0 */
    say("abox: 8/13 routes: mixer defaults (UAIF SPK / NSRC / RSRC reserved, SPUS chain 4 RDMA4 -> ASRC -> SIFS0)");

    /* 9: firmware into SRAM (word writes: SRAM is device memory), the rest of SRAM zeroed (IPC area included) */
    {
        const uint8_t *p = rm(ABOX_RM_RDMA4, ABOX_SRAM_SIZE);
        uint32_t n = S.sram_bytes, i;
        for (i = 0; i + 4U <= n; i += 4U)
            w32(ABOX_SRAM + i, (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16 |
                                   (uint32_t)p[i + 3] << 24);
        if (i < n) {
            uint32_t v = 0;
            for (uint32_t k = 0; i + k < n; ++k)
                v |= (uint32_t)p[i + k] << (8U * k);
            w32(ABOX_SRAM + i, v);
            i += 4U;
        }
        for (; i < ABOX_SRAM_SIZE; i += 4U)
            w32(ABOX_SRAM + i, 0);
        uint32_t back = r32(ABOX_SRAM), want = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                                                (uint32_t)p[3] << 24;
        if (S.dump_win)
            memset(rm(ABOX_RM_DUMP, ABOX_RM_DRAM_SZ), 0, ABOX_RM_DRAM_SZ);
        if (back != want) {
            lb_s(&l, "abox: 9/13 sram: the first SRAM word does not read back");
            return fail(ABOX_EMMU, &l);
        }
        lb_s(&l, "abox: 9/13 sram: calliope_sram.bin in SRAM, the rest of the ");
        lb_u(&l, ABOX_SRAM_SIZE);
        lb_s(&l, " bytes zeroed; DRAM image, IVA and ext_bins in abox_rmem");
        sayl(&l);
        lb_init(&l);
        ram_sync();
    }

    /* 10: the CPU (abox_request_dram_on(true), abox_cpu_power(true), abox_cpu_enable(true): 1 s) */
    m32(ABOX_SFR + SFR_SYSPOWER_CTRL, 1U, 1U);
    m32(ABOX_PMU + ABOX_PMU_CPU_CONF, 1U, 1U);
    m32(ABOX_PMU + ABOX_PMU_CPU_OPT, 1U << 28, 1U << 28);
    S.cpu_started = 1;
    if (!wait32(ABOX_PMU + ABOX_PMU_CPU_STAT, 1U, 1U, 1000000, 100, &took)) {
        lb_s(&l, "abox: 10/13 cpu: ABOX's CPU did not report on within 1 s");
        return fail(ABOX_ECPU, &l);
    }
    lb_s(&l, "abox: 10/13 cpu: DRAM on, ABOX CPU powered and enabled (status after ");
    lb_u(&l, took);
    lb_s(&l, " us)");
    sayl(&l);
    lb_init(&l);

    /* 11: pads out of retention (abox_pad_retention(false)) */
    m32(ABOX_PMU + ABOX_PMU_PAD_RET, 1U << 28, 1U << 28);
    m32(ABOX_PMU + ABOX_PMU_GPIO_MODE, 1U, 1U);
    say("abox: 11/13 pads: retention released");

    /* 12: BOOT_DONE ("Calliope is ready to sing") */
    uint32_t waited = 0, limit = C.boot_timeout_ms * 1000U;
    while (!S.boot_done) {
        rx_take();
        rx_process();
        fw_log_flush();
        if (S.boot_done || S.fault)
            break;
        if (r32(ABOX_SYSMMU + MMU_INT_STATUS)) {
            lb_s(&l, "abox: 12/13 ready: a SysMMU fault at ABOX address ");
            lb_x(&l, r32(ABOX_SYSMMU + MMU_FAULT_ADDR));
            lb_s(&l, " (info ");
            lb_x(&l, r32(ABOX_SYSMMU + MMU_FAULT_INFO));
            lb_s(&l, ") before BOOT_DONE");
            return fail(ABOX_EFAULT, &l);
        }
        if (waited >= limit)
            break;
        H.delay_us(H.ctx, 100);
        waited += 100;
    }
    fw_line_out();
    if (S.fault) {
        lb_s(&l, "abox: 12/13 ready: Calliope reported a fault while booting");
        return fail(ABOX_EFAULT, &l);
    }
    if (!S.boot_done) {
        lb_s(&l, "abox: 12/13 ready: no BOOT_DONE within ");
        lb_u(&l, C.boot_timeout_ms);
        lb_s(&l, " ms (ABOX PC ");
        lb_x(&l, r32(ABOX_SFR + SFR_CPU_PC));
        lb_s(&l, ", messages ");
        lb_u(&l, S.ipc_recv);
        lb_s(&l, ")");
        return fail(ABOX_ETIME, &l);
    }
    S.state = ABOX_ST_READY;
    lb_s(&l, "abox: 12/13 ready: Calliope is ready to sing (version ");
    lb_ver(&l, S.version);
    lb_s(&l, ") after ");
    lb_u(&l, waited / 1000U);
    lb_s(&l, " ms");
    sayl(&l);
    lb_init(&l);

    /* 13: abox_boot_done_work_func(): RESUME, abox_restore_data(), gear MIN, DRAM off */
    msg_sys(SYS_RESUME, 0, 0, 0);
    r = ipc_send("RESUME");
    for (uint32_t i = 0; !r && i < 9; ++i) {                    /* SET_MIXER..INMUX4_SAMPLE_RATE: 48000 */
        msg_new(IPC_ABOX_CONFIG);
        tx[2] = CFG_SET_MIXER_RATE + i, tx[3] = ABOX_CALL_RATE;
        r = ipc_send("SET_SAMPLE_RATE");
    }
    if (!r) {
        m32(ABOX_SFR + SFR_SPUS_CTRL1, 0x1FU, FMT_16_2);        /* SET_MIXER_FORMAT's register update */
        m32(ABOX_SFR + SFR_SPUM_CTRL1, 0x1FU << 8, FMT_16_2 << 8);      /* SET_RECP_FORMAT's */
    }
    for (uint32_t i = 0; !r && i < 9; ++i) {                    /* SET_MIXER..INMUX4_FORMAT: 16 bit, 2 ch */
        msg_new(IPC_ABOX_CONFIG);
        tx[2] = CFG_SET_MIXER_FORMAT + i, tx[3] = ABOX_CALL_BITS, tx[4] = ABOX_CALL_CH;
        r = ipc_send("SET_FORMAT");
    }
    for (uint32_t t = ERAP_ECHO_CANCEL; !r && t <= ERAP_VI_SENSE; ++t) {
        msg_new(IPC_ERAP);
        tx[2] = ERAP_REALTIME_CLOSE, tx[3] = t;
        r = ipc_send("ERAP close");
    }
    if (!r) {
        msg_sys(SYS_SET_MODE, MODE_NORMAL, 0, 0);
        r = ipc_send("SET_MODE");
    }
    if (!r) {
        msg_sys(SYS_SET_TYPE, TYPE_VOICE, 0, 0);
        r = ipc_send("SET_TYPE");
    }
    if (r) {
        lb_s(&l, "abox: 13/13 restore: Calliope stopped taking messages after BOOT_DONE");
        return fail(ABOX_EIPC, &l);
    }
    rx_take();
    rx_process();
    gear_set(GEAR_ID_DEFAULT, GEAR_MIN, 1);
    m32(ABOX_SFR + SFR_SYSPOWER_CTRL, 1U, 0);                   /* abox_request_dram_on(false) */
    fw_log_flush();
    lb_s(&l, "abox: 13/13 restore: RESUME, rates 48000, formats 16 bit 2 ch, ERAP closed, mode NORMAL, type VOICE; "
             "Calliope ");
    lb_ver(&l, S.version);
    lb_s(&l, " ready, VSS not started");
    sayw(&l);
    return 0;
}

/* ---- VSS ------------------------------------------------------------------------------------------------------------ */
void abox_vss_prepare(void)
{
    struct lb l;
    if (!have_hal)
        return;
    if (S.vss_started) {
        say("abox: vss: VSS already started: the magic word is left alone");
        return;
    }
    volatile uint32_t *magic = (volatile uint32_t *)H.ram(H.ctx, C.vss_base + C.vss_magic_off, 4);
    lb_init(&l);
    if (!magic) {
        lb_s(&l, "abox: vss: the VSS magic word is not mappable: not cleared");
        sayw(&l);
        return;
    }
    uint32_t was = *magic;
    *magic = 0;
    ram_sync();
    lb_s(&l, "abox: vss: magic word at VSS + ");
    lb_x(&l, C.vss_magic_off);
    lb_s(&l, was ? " cleared (it held leftover data)" : " cleared (it was 0)");
    sayw(&l);
}

int abox_vss_start(void)
{
    struct lb l;
    lb_init(&l);
    if (S.state != ABOX_ST_READY && S.state != ABOX_ST_CALL) {
        lb_s(&l, "abox: vss: START_VSS needs Calliope running (abox_boot first)");
        sayw(&l);
        return ABOX_ESTATE;
    }
    msg_sys(SYS_START_VSS, 0, 0, 0);
    int r = ipc_send("START_VSS");
    if (r)
        return r;
    S.vss_started = 1;
    lb_s(&l, "abox: vss: START_VSS sent (Calliope now waits for the CP's magic word; watch for VSS lines in \"abox: fw:\")");
    sayw(&l);
    return 0;
}

/* ---- the call ------------------------------------------------------------------------------------------------------- */
/* the PCM task of a call DMA: OPEN, SET_BUFFER, HW_PARAMS, PREPARE (abox_rdma/wdma open / hw_params / prepare) */
static int pcm_up(uint32_t ipcid, uint32_t id, uint32_t iova)
{
    int r;
    msg_pcm(ipcid, id, PCM_PLTDAI_OPEN, 0, 0, 0);
    if ((r = ipc_send("PCM OPEN")))
        return r;
    msg_pcm(ipcid, id, PCM_SET_BUFFER, iova, ABOX_CALL_PERIOD, ABOX_CALL_PERIODS);
    if ((r = ipc_send("PCM SET_BUFFER")))
        return r;
    msg_pcm(ipcid, id, PCM_PLTDAI_HW_PARAMS, ABOX_CALL_RATE, ABOX_CALL_BITS, ABOX_CALL_CH);
    if ((r = ipc_send("PCM HW_PARAMS")))
        return r;
    msg_pcm(ipcid, id, PCM_PLTDAI_PREPARE, 0, 0, 0);
    return ipc_send("PCM PREPARE");
}

static int pcm_trigger(uint32_t ipcid, uint32_t id, uint32_t on)
{
    msg_pcm(ipcid, id, PCM_PLTDAI_TRIGGER, on, 0, 0);
    return ipc_send(on ? "PCM TRIGGER start" : "PCM TRIGGER stop");
}

static void pcm_down(uint32_t ipcid, uint32_t id)
{
    msg_pcm(ipcid, id, PCM_PLTDAI_HW_FREE, 0, 0, 0);
    (void)ipc_send("PCM HW_FREE");
    msg_pcm(ipcid, id, PCM_PLTDAI_CLOSE, 0, 0, 0);
    (void)ipc_send("PCM CLOSE");
}

/* the call path back to idle, best effort (each step whatever the one before did) */
static void call_down(void)
{
    /* playback stop is PRE (the DMA first), capture stop is POST (the UAIF first) */
    (void)pcm_trigger(IPC_PCMPLAYBACK, 4, 0);
    m32(ABOX_SFR + SFR_UAIF_CTRL0(0), UAIF_MIC_EN, 0);
    (void)pcm_trigger(IPC_PCMCAPTURE, 2, 0);
    m32(ABOX_SFR + SFR_SYSPOWER_CTRL, 1U, 0);                   /* abox_request_dram_on(false) at TRIGGER stop */
    pcm_down(IPC_PCMPLAYBACK, 4);
    pcm_down(IPC_PCMCAPTURE, 2);
    m32(ABOX_SFR + SFR_UAIF_CTRL0(1), UAIF_SPK_EN, 0);          /* abox_uaif_shutdown(): SPK off */
    m32(ABOX_SFR + SFR_QCH_DISABLE, 1U << 2, 0);                /* UAIF0 tristate again */
    /* routes back to the mixer defaults, then the flushes of the widgets that powered down */
    m32(ABOX_SFR + SFR_ROUTE_CTRL0, 0xFU << 4, 0);
    m32(ABOX_SFR + SFR_ROUTE_CTRL2, 0xFU, 0);
    m32(ABOX_SFR + SFR_SPUM_CTRL1, 1U, 0);
    m32(ABOX_SFR + SFR_ROUTE_CTRL1, 0xFU << 4, 0);
    m32(ABOX_SFR + SFR_SPUS_CTRL2, 1U, 1U);                     /* SIFS0 POST_PMD: MIXP flush */
    m32(ABOX_SFR + SFR_SPUM_CTRL2, 1U, 1U);                     /* RECP flush */
    m32(ABOX_SFR + SFR_SPUM_CTRL3, 1U << 1, 1U << 1);           /* SIFM1 flush */
    msg_sys(SYS_SET_MODE, MODE_NORMAL, 0, 0);
    (void)ipc_send("SET_MODE NORMAL");
    gear_set(GEAR_ID_CALL, GEAR_MIN, 1);
    if (H.dvfs_int && S.int_khz) {
        (void)H.dvfs_int(H.ctx, 0);
        S.int_khz = 0;
    }
    S.call_on = 0;
}

int abox_call(int on)
{
    struct lb l;
    int r = 0;
    lb_init(&l);
    if (S.state != ABOX_ST_READY && S.state != ABOX_ST_CALL) {
        lb_s(&l, "abox: call: Calliope is not running");
        sayw(&l);
        return ABOX_ESTATE;
    }
    if (!on) {
        if (!S.call_on) {
            say("abox: call: off (was not on)");
            return 0;
        }
        unsigned before = S.ipc_timeouts;
        call_down();
        S.state = ABOX_ST_READY;
        lb_s(&l, S.ipc_timeouts == before ? "abox: call: off: PCMs stopped and closed, routes back, mode NORMAL"
                                          : "abox: call: off, but Calliope missed messages on the way");
        sayw(&l);
        if (S.ipc_fail_row >= 3) {
            lb_init(&l);
            lb_s(&l, "abox: call: Calliope no longer takes messages: stopping ABOX");
            return fail(ABOX_EIPC, &l);
        }
        return 0;
    }
    if (S.call_on) {
        say("abox: call: already on");
        return 0;
    }
    if (!S.vss_started)
        say("abox: call: VSS was not started (abox_vss_start): the voice codec will not run");
    S.call_on = 1;

    /* the HAL's mode and routes: ABOX Audio Mode IN_CALL, then incall_nb-handset + incall_nb-handset-mic */
    msg_sys(SYS_SET_MODE, MODE_IN_CALL, 0, 0);
    r = ipc_send("SET_MODE IN_CALL");
    if (!r) {
        m32(ABOX_SFR + SFR_ROUTE_CTRL0, 0xFU << 4, 1U << 4);    /* UAIF1 SPK = SIFS0 */
        m32(ABOX_SFR + SFR_ROUTE_CTRL2, 0xFU, 1U);              /* RSRC0 = SIFS0 */
        m32(ABOX_SFR + SFR_SPUM_CTRL1, 1U, 1U);                 /* RECP PIFS0 = 1 */
        m32(ABOX_SFR + SFR_ROUTE_CTRL1, 0xFU << 4, 8U << 4);    /* NSRC1 = UAIF0 */
        m32(ABOX_SFR + SFR_SPUM_CTRL0, 1U << 11, 0);            /* SIFM1 = WDMA */
        msg_sys(SYS_SET_TYPE, TYPE_VOICE, 0, 0);
        r = ipc_send("SET_TYPE VOICE");
    }
    say("abox: call: 1/4 mode IN_CALL, routes SIFS0 -> UAIF1 and RSRC0 (RECP PIFS0), UAIF0 -> NSRC1 -> SIFM1 -> WDMA2, "
        "type VOICE");

    /* the UAIFs (abox_if startup / set_fmt / hw_params; abox_register_bclk_usage: AUDIF 73.728 MHz, BCLK 1.536 MHz) */
    if (!r) {
        if (!cmu_set(CMU_DIV_AUDIF, 0x1FFU, DIV_AUDIF_UAIF) || !cmu_set(CMU_DIV_UAIF1, 0x1FFU, DIV_UAIF_BCLK) ||
            !cmu_set(CMU_DIV_UAIF0, 0x1FFU, DIV_UAIF_BCLK)) {
            lb_s(&l, "abox: call: a UAIF clock divider did not take its value");
            r = ABOX_ECLOCK;
        }
    }
    if (!r) {
        qch_on(2);
        qch_on(3);
        m32(ABOX_SFR + SFR_UAIF_CTRL0(1), UAIF_MODE_MASTER, UAIF_MODE_MASTER);
        m32(ABOX_SFR + SFR_UAIF_CTRL1(1), UAIF_CTRL1_MASK, UAIF_CTRL1_VAL);
        m32(ABOX_SFR + SFR_UAIF_CTRL0(0), UAIF_MODE_MASTER | UAIF_DATA_MODE | UAIF_IRQ_MODE,
            UAIF_MODE_MASTER | UAIF_DATA_MODE);
        m32(ABOX_SFR + SFR_UAIF_CTRL1(0), UAIF_CTRL1_MASK, UAIF_CTRL1_VAL);
        m32(ABOX_SFR + SFR_QCH_DISABLE, 1U << 2, 1U << 2);      /* set_tristate(0): UAIF0's BCLK always runs */
        say("abox: call: 2/4 UAIF1 and UAIF0: I2S masters, 48 kHz 2 x 16 (CTRL1 0x0984f000), BCLK 1.536 MHz");
    }

    /* the PCMs (rdma/wdma open: gear CALL_KERNEL MAX and the INT floor; hw_params; prepare) */
    if (!r) {
        gear_set(GEAR_ID_CALL, GEAR_MAX, 1);
        if (H.dvfs_int) {
            if (H.dvfs_int(H.ctx, CALL_INT_KHZ) >= 0)
                S.int_khz = CALL_INT_KHZ;
        }
        memset(rm(ABOX_RM_RDMA4, SECT), 0, SECT);
        memset(rm(ABOX_RM_WDMA2, SECT), 0, SECT);
        ram_sync();
        r = pcm_up(IPC_PCMPLAYBACK, 4, ABOX_IOVA_RDMA4);
        if (!r)
            r = pcm_up(IPC_PCMCAPTURE, 2, ABOX_IOVA_WDMA2);
        if (!r)
            say("abox: call: 3/4 RDMA4 (RX ring) and WDMA2 (TX ring): open, 1920 x 4 bytes, 48000 / 16 / 2, prepared");
    }

    /* start: SIFS0 PRE_PMU flush, DRAM on; playback POST_PRE (UAIF1 first), capture PRE_POST (WDMA2 first) */
    if (!r) {
        m32(ABOX_SFR + SFR_SPUS_CTRL2, 1U, 1U);
        m32(ABOX_SFR + SFR_SYSPOWER_CTRL, 1U, 1U);
        m32(ABOX_SFR + SFR_UAIF_CTRL0(1), UAIF_SPK_EN, UAIF_SPK_EN);
        r = pcm_trigger(IPC_PCMPLAYBACK, 4, 1);
        if (!r)
            r = pcm_trigger(IPC_PCMCAPTURE, 2, 1);
        if (!r)
            m32(ABOX_SFR + SFR_UAIF_CTRL0(0), UAIF_MIC_EN, UAIF_MIC_EN);
    }
    if (r) {
        if (!l.n)
            lb_s(&l, "abox: call: Calliope did not take the call set-up");
        sayw(&l);
        call_down();
        if (S.ipc_fail_row >= 3) {
            lb_init(&l);
            lb_s(&l, "abox: call: Calliope no longer takes messages: stopping ABOX");
            return fail(ABOX_EIPC, &l);
        }
        return r;
    }
    S.state = ABOX_ST_CALL;
    lb_s(&l, "abox: call: 4/4 on: UAIF1 SPK and UAIF0 MIC enabled, RDMA4 / WDMA2 triggered (UAIF1 status ");
    lb_x(&l, r32(ABOX_SFR + SFR_UAIF_STATUS(1)));
    lb_s(&l, ")");
    sayw(&l);
    return 0;
}

/* ---- media PCMs (abox_media.c): additive hooks, abox.h has the contract --------------------------------------------- */
int abox_map_extra(uint32_t iova, uint64_t pa, uint32_t size)
{
    unsigned i, free = XMAPS;
    if (S.state == ABOX_ST_BOOTING || S.state == ABOX_ST_READY || S.state == ABOX_ST_CALL)
        return ABOX_ESTATE;
    if ((iova & (SECT - 1U)) || (pa & 0xFFFU) || size > 0xF0000U || (size & 0xFFFU))
        return ABOX_ECFG;
    for (i = 0; i < XMAPS; ++i) {
        if (xmap[i].size && xmap[i].iova == iova)
            break;
        if (!xmap[i].size && free == XMAPS)
            free = i;
    }
    if (!size) {
        if (i < XMAPS)
            xmap[i].size = 0;
        return 0;
    }
    if (pa < C.rmem_base + ABOX_RM_PT + 0x10000U || pa + size > C.rmem_base + ABOX_RM_DUMP)
        return ABOX_ECFG;
    if (i == XMAPS)
        i = free;
    if (i == XMAPS)
        return ABOX_ECFG;
    xmap[i].iova = iova, xmap[i].pa = pa, xmap[i].size = size;
    return 0;
}

int abox_pcm_msg(int capture, uint32_t id, uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    static const struct { uint32_t t; const char *s; } nm[] = {
        { PCM_PLTDAI_OPEN, "media PCM OPEN" },         { PCM_PLTDAI_CLOSE, "media PCM CLOSE" },
        { PCM_PLTDAI_HW_PARAMS, "media PCM HW_PARAMS" }, { PCM_PLTDAI_HW_FREE, "media PCM HW_FREE" },
        { PCM_PLTDAI_PREPARE, "media PCM PREPARE" },   { PCM_PLTDAI_TRIGGER, "media PCM TRIGGER" },
        { PCM_SET_BUFFER, "media PCM SET_BUFFER" },
    };
    const char *what = "media PCM message";
    if ((S.state != ABOX_ST_READY && S.state != ABOX_ST_CALL) || id >= 8U || (!capture && id == 4U) ||
        (capture && id == 2U))
        return ABOX_ESTATE;
    for (unsigned i = 0; i < sizeof(nm) / sizeof(nm[0]); ++i)
        if (nm[i].t == type)
            what = nm[i].s;
    msg_pcm(capture ? IPC_PCMCAPTURE : IPC_PCMPLAYBACK, id, type, a, b, c);
    return ipc_send(what);
}

uint32_t abox_pcm_pointer(int capture, uint32_t id, uint32_t *count)
{
    unsigned d = capture ? 1U : 0U;
    if (count)
        *count = id < 8U ? S.pcm_ptr_n[d][id] : 0;
    return id < 8U ? S.pcm_ptr[d][id] : 0;
}

/* ---- the timer's poll ---------------------------------------------------------------------------------------------- */
int abox_poll(void)
{
    struct lb l;
    if (S.state == ABOX_ST_FAILED)
        return S.err ? S.err : ABOX_EFAULT;
    if (S.state != ABOX_ST_READY && S.state != ABOX_ST_CALL)
        return 0;
    lb_init(&l);
    uint32_t st = r32(ABOX_SYSMMU + MMU_INT_STATUS);
    if (st) {
        lb_s(&l, "abox: SysMMU fault (status ");
        lb_x(&l, st);
        lb_s(&l, ") at ABOX address ");
        lb_x(&l, r32(ABOX_SYSMMU + MMU_FAULT_ADDR));
        lb_s(&l, ", info ");
        lb_x(&l, r32(ABOX_SYSMMU + MMU_FAULT_INFO));
        lb_s(&l, ", ABOX PC ");
        lb_x(&l, r32(ABOX_SFR + SFR_CPU_PC));
        fw_log_flush();
        fw_line_out();
        return fail(ABOX_EFAULT, &l);
    }
    if (!(r32(ABOX_PMU + ABOX_PMU_CPU_STAT) & 1U)) {
        lb_s(&l, "abox: ABOX's CPU is no longer powered");
        return fail(ABOX_EFAULT, &l);
    }
    rx_take();
    rx_process();
    fw_log_flush();
    if (S.fault) {
        fw_line_out();
        lb_s(&l, "abox: Calliope reported a fault: ABOX stopped (");
        lb_s(&l, why_buf);
        lb_s(&l, ")");
        return fail(ABOX_EFAULT, &l);
    }
    if (S.ipc_fail_row >= 3) {
        lb_s(&l, "abox: Calliope no longer takes messages: ABOX stopped");
        return fail(ABOX_EIPC, &l);
    }
    return 0;
}

void abox_off(void)
{
    struct lb l;
    if (!have_hal)
        return;
    if (S.state == ABOX_ST_OFF) {
        say("abox: off: already off");
        return;
    }
    if (S.state == ABOX_ST_CALL || S.call_on)
        call_down();
    if (S.state == ABOX_ST_READY || S.state == ABOX_ST_CALL) {
        /* abox_cpu_pm_ipc(false): SUSPEND, then WFI (PMU ABOX_CPU_STANDBY bit 28) */
        msg_sys(SYS_SUSPEND, 0, 0, 0);
        int r = ipc_send("SUSPEND");
        int wfi = !r && wait32(ABOX_PMU + ABOX_PMU_CPU_STANDBY, 1U << 28, 1U << 28, 10000, 10, 0);
        fw_log_flush();
        fw_line_out();
        lb_init(&l);
        lb_s(&l, wfi ? "abox: off: Calliope suspended (WFI)" : "abox: off: Calliope did not reach WFI: stopped anyway");
        sayl(&l);
    }
    teardown();
    S.state = ABOX_ST_OFF;
    S.err = 0;
    set_why("ABOX off");
}

/* ---- libsb glue ------------------------------------------------------------------------------------------------------ */
#ifdef SBOS_KERNEL
static uint32_t lsb_rd32(void *c, uint64_t pa)
{
    (void)c;
    uint32_t v = *(volatile uint32_t *)(uintptr_t)pa;
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}
static void lsb_wr32(void *c, uint64_t pa, uint32_t v)
{
    (void)c;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)pa = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int64_t lsb_smc(void *c, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)c;
    return sb_psci(fn, a1, a2, a3);
}
static void lsb_delay(void *c, uint32_t us)
{
    (void)c;
    sb_udelay(us);
}
static void lsb_log(void *c, const char *s)
{
    (void)c;
    sb_log(s);
}
static uint64_t lsb_now(void *c)
{
    (void)c;
    return sb_us();
}
static void lsb_sync(void *c)
{
    (void)c;
    __asm__ volatile("dsb sy" ::: "memory");
}
/* RAM the CPU writes with plain stores and ABOX reads: only libsb's non-cacheable window (kind 3) */
static void *lsb_ram(void *c, uint64_t pa, uint64_t len)
{
    (void)c;
    if (!len || sb_map_kind(pa) != 3 || sb_map_kind(pa + len - 1U) != 3)
        return 0;
    return (void *)(uintptr_t)pa;
}

void abox_hal_libsb(struct abox_hal *h, long (*file)(void *ctx, const char *path, void *dst, uint32_t cap),
                    void *file_ctx)
{
    memset(h, 0, sizeof(*h));
    h->ctx = file_ctx;
    h->rd32 = lsb_rd32;
    h->wr32 = lsb_wr32;
    h->smc = lsb_smc;
    h->delay_us = lsb_delay;
    h->log = lsb_log;
    h->file = file;
    h->ram = lsb_ram;
    h->now_us = lsb_now;
    h->sync = lsb_sync;
}
#endif
