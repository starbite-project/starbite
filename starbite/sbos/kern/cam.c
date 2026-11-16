/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the Galaxy S9's front camera, a Samsung S5K3H1 (Android's camera 1), from its supplies to frames in
 * memory, without the ISP's firmware: the sensor streams RAW10 over MIPI CSI-2 into CSIS1, whose WDMA writes each frame
 * straight to RAM, and cam_isp.c makes pictures of them on the processor. docs/drivers/CAMERA.md.
 *
 * THE WIRING (this phone's device tree, device/dtb/starlte.dts, and Samsung's GPL-2.0 fimc-is2 driver for it;
 * register values and sequences also as an earlier port's XNU driver recorded them working on this phone, read as facts only)
 *   sensor      I2C 0x35 (fimc-is-3h1@35) on hsi2c@108F0000 (USI08_I2C of PERIC1, "hsi2c21", 400 kHz), mode word
 *               usi@1081102C ("i2c"); pins gpp5-2 SCL / gpp5-3 SDA, function 3 (hsi2c21-bus), 0 when off; its
 *               CMU_PERIC1 gates 0x2050 / 0x2098 / 0x20EC / 0x20F0 and Q-channel 0x303C. 16-bit register addresses
 *               (MIPI CCS map): model ID 0x0000, revision 0x0002, frame count 0x0005 (0xFF: not streaming),
 *               mode_select 0x0100, grouped hold 0x0104, coarse integration 0x0202, analog gain 0x0204 (0x20 = 1x),
 *               digital gain 0x020E (0x100 = 1x)
 *   reset       gpg2-5 (PERIC0 0x104300C0), output, high = running. gpg2-4 on the same bank is the torch: never
 *               written here (read-modify-write of bit 5 only)
 *   MCLK        CIS_CLK1 of CMU_TOP (0x1A240000): mux 0x101C [0] (0 = OSCCLK, 26 MHz, what Android asks), divider
 *               0x1818 [4:0], gate 0x201C (bit 20 manual, 21 on), Q-channel 0x3008; out on gpc1-5 (PERIC1 0x10830080)
 *               function 2 (fimc_is_mclk1_fn)
 *   supplies    on the S2MPB02 (0x59 on hsi2c@10920000, USI10: the LED's chip, kern/fled.c): BUCK2 VDD_EXT_1P3_PB02
 *               1.3 V (B2CTRL1 0x0D [7:6] = 3 on, B2CTRL2 0x0E = 0x90: 400 mV + 6.25 mV n), the 1.3 V the low-voltage
 *               LDOs are made from (Samsung's star boards: USE_BUCK2_REGULATOR_CONTROL); LDO5 VDDD_1.05V_VT (0x22,
 *               0x24), LDO6 VDDIO_1.8V_VT (0x23, 0x30), LDO13 VDDA_2.8V_VT (0x2A, 0x58), LDO16 VDDAF_2.8V_VT (0x2D,
 *               0x58): bit 7 on, [6:0] 600 mV + 12.5 mV n (LDO1-5) or 25 mV n (LDO6-18)
 *   ISPPRE      pd-isppre: PMU 0x14064048 [3:0] = 0xF, status 0x1406404C; its parent pd-dpu (0x14064024) is the
 *               display's and is only checked; after power-on the DTZPC restore SMC 0x82000410 (1, 0x16210004, 0),
 *               before power-off MEMORY_ISPPRE_OPTION 0x14064730 [1:0] = 2 and the save SMC (0, 0x16210004, 0).
 *               CMU_TOP CLKCMU_ISPPRE_BUS: mux 0x1094 [1:0] = 1 (SHARED1_DIV4, 466 MHz), divider 0x1890 [3:0] = 1
 *               (233 MHz: the boot's ~50 MHz overflows CSIS1), gate 0x209C. CMU_ISPPRE 0x16200000: BUS_USER +0x100
 *               bit 4, the Q-channels of CSIS0-3, PDP DMA, SysMMU, AXI_D and BTM requested (bit 1)
 *   D-PHY       CSIS1's M0S4S2: isolation PMU 0x14060730 bit 0 (1 = in use), reset SYSREG_ISPPRE 0x16210500 bit 2
 *               (1 = running); registers 0x16180000: settle 36 at 720 Mb/s (fimc-is-hw-settle-10nm-lpp.h)
 *   CSIS1       0x16240000 (fimc-is-hw-csi-v5_1): two lanes, VC0 RAW10 1632 x 1224, interleave VC_DT; its VC0 WDMA
 *               0x162C0000 (DMA common +0x400), the four CSISs' DMA common 0x162F0000. The SysMMU 0x16310000 is never
 *               touched: after the domain's power-on it is off, and passes the WDMA's addresses through
 *   the mode    Samsung's setfile A, 1632 x 1224 RAW10 at 30 frames a second, two lanes at 719 Mb/s
 *               (fimc-is-cis-3h1-setA.h, sensor_3h1_setfile_A_1632x1224_30fps), then 0x3092 = 0; stream on: 0x0104 =
 *               0, 8 ms, 0x6028 = 0x4000, 0x0100 = 1, 0x0216 = 0 (fimc-is-cis-3h1.c)
 *   the frames  32 MiB at 0xD8000000 (cam.h CAM_MEM_PA): a ring of four 4 MiB raw frames given to the WDMA's eight
 *               frame pointers, a fifth slot that stands in for one held for a still, the preview, the still and its
 *               BMP. That range lies in the device tree's gap (no reserved-memory node: the panel's buffers end at
 *               0xCF600000, secure_camera starts at 0xE1900000, the modem and ABOX are above 0xF0000000), libsb maps it
 *               non-cacheable, nothing else in Starbite OS uses it, and an earlier port's DMA wrote there on this phone.
 *               The WDMA writes each RAW10 pixel in 16 bits, little-endian (PACK12 off).
 *
 * WHAT HAS BEEN SEEN: nothing of this file has run on a phone yet (2026-11-12). an earlier port's driver, on this phone, took the
 * same sequence to 30 frames a second (its CHANGELOG). This file's own ways (the step machine, the found-state
 * restore, the slot markers) are only host-tested (kern/host/cam-test.c).
 *
 * Every write goes through wr(), which refuses (and logs) any address outside the blocks above. Every wait is bounded
 * by the clock. Whatever was switched on is switched off in the reverse order, also on a failure, and pins, rails and
 * clocks go back to the state they were found in. Two exceptions, said in the log: the S2MPB02's I2C bus (USI10 and
 * its pins) is left set up, as kern/fled.c leaves it (the torch shares it); a power domain found on is left on. */
#include "cam.h"
#include <stdarg.h>
#ifdef SBOS_KERNEL
#include "sb.h"
#endif

/* ---- addresses ---------------------------------------------------------------------------------------------------- */
#define PMU             0x14060000ULL
#define PMU_DPHY_ISO    (PMU + 0x0730)
#define PMU_DPU_STAT    (PMU + 0x4024)
#define PMU_ISP_CONF    (PMU + 0x4048)
#define PMU_ISP_STAT    (PMU + 0x404C)
#define PMU_ISP_MEM     (PMU + 0x4730)
#define PD_ON           0xFU

#define CMU_TOP         0x1A240000ULL
#define CIS1_MUX        (CMU_TOP + 0x101C)
#define CIS1_DIV        (CMU_TOP + 0x1818)
#define CIS1_GATE       (CMU_TOP + 0x201C)
#define CIS1_QCH        (CMU_TOP + 0x3008)
#define ISPB_MUX        (CMU_TOP + 0x1094)
#define ISPB_DIV        (CMU_TOP + 0x1890)
#define ISPB_GATE       (CMU_TOP + 0x209C)
#define GATE_ON         0x00300000U         /* bit 20 manual, bit 21 on */
#define TOP_BUSY        (1U << 16)

#define CMU_PERIC1      0x10800000ULL
#define GPIO_PERIC0     0x10430000ULL
#define GPIO_PERIC1     0x10830000ULL
#define GPG2            (GPIO_PERIC0 + 0xC0)     /* reset: pin 5 (pin 4: the torch) */
#define GPP5            (GPIO_PERIC1 + 0x20)     /* the sensor's bus: pins 2, 3 */
#define GPP6            (GPIO_PERIC1 + 0x40)     /* the S2MPB02's bus: pins 0, 1 */
#define GPC1            (GPIO_PERIC1 + 0x80)     /* MCLK1: pin 5 */
#define RESET_PIN       5U
#define MCLK_PIN        5U

#define CMU_ISPPRE      0x16200000ULL
#define ISPPRE_USER     (CMU_ISPPRE + 0x100)
#define SYSREG_ISPPRE   0x16210000ULL
#define DPHY_RESET      (SYSREG_ISPPRE + 0x500)
#define DPHY_RESET_CSIS1 (1U << 2)
#define DTZPC_ISPPRE    0x16210004ULL
#define SMC_PD          0x82000410ULL       /* SMC_CMD_PREPARE_PD_ONOFF: a1 1 restore / 0 save, a2 the DTZPC */

#define CSIS            0x16240000ULL
#define CSIS_VERSION    (CSIS + 0x000)
#define CSIS_CMN        (CSIS + 0x004)
#define CSIS_MSK0       (CSIS + 0x010)
#define CSIS_SRC0       (CSIS + 0x014)
#define CSIS_MSK1       (CSIS + 0x018)
#define CSIS_SRC1       (CSIS + 0x01C)
#define CSIS_CFG(n)     (CSIS + 0x040 + 0x10 * (n))
#define CSIS_RES(n)     (CSIS + 0x044 + 0x10 * (n))
#define CSIS_FRMCNT     (CSIS + 0x100)
#define CSIS_PHYSTAT    (CSIS + 0x700)
#define CSIS_PHYCMN     (CSIS + 0x704)
#define CMN_CSI_EN      (1U << 0)
#define CMN_SW_RESET    (1U << 1)
#define CMN_UPDATE_SHADOW (0xFU << 16)
#define CMN_QCH_EN      (1U << 20)
#define CMN_PHY_SEL     (1U << 21)
#define FMT_RAW10       0x2BU
#define FMT_EMBEDDED    0x12U
#define FMT_UNKNOWN     0x3FU

#define DPHY            0x16180000ULL
#define DPHY_SC06       (DPHY + 0x1018)
#define DPHY_SC02       (DPHY + 0x1084)
#define DPHY_SD08(n)    (DPHY + 0x143C + 0x400 * (n))
#define DPHY_SD05(n)    (DPHY + 0x14AC + 0x400 * (n))
#define DPHY_SD06(n)    (DPHY + 0x14B0 + 0x400 * (n))
#define DPHY_SD13(n)    (DPHY + 0x14E0 + 0x400 * (n))
#define SETTLE_720      36U

#define WDMA            0x162C0000ULL
#define DMA_CTRL        (WDMA + 0x000)
#define DMA_FMT         (WDMA + 0x004)
#define DMA_ADDR(n)     (WDMA + 0x010 + 4 * (n))
#define DMA_ACT         (WDMA + 0x030)
#define DMA_RESOL       (WDMA + 0x040)
#define VC_DATA         (WDMA + 0x400)
#define VC_MASK         (WDMA + 0x404)
#define VC_SRC          (WDMA + 0x408)
#define VC_CMN          (WDMA + 0x414)
#define VC_ERR          (WDMA + 0x418)
#define VC_OTF          (WDMA + 0x41C)
#define DMA_ON          (1U << 0)
#define DMA_UPDT        (1U << 1)
#define DMA_PTRS        (7U << 2)
#define VC_IRQ_MASK     0x00F11F00U
#define DMA_FS          (1U << 4)
#define DMA_FE          (1U << 8)
#define DMA_ERROR       (1U << 12)
#define DMA_ABORTED     (1U << 16)

#define CDMA            0x162F0000ULL
#define CDMA_ARB        (CDMA + 0x00)
#define CDMA_CTRL       (CDMA + 0x08)
#define CDMA_SPLIT      (CDMA + 0x0C)
#define CDMA_MATRIX     (CDMA + 0x10)

#define PMIC_ADDR       0x59U
#define PMIC_ID_REG     0x1DU               /* reads 0x90 on this phone's S2MPB02 (kern/fled.c's check) */
#define PMIC_ID         0x90U
#define B2CTRL1         0x0DU
#define B2CTRL2         0x0EU
#define BUCK_ON         0xC0U
#define BUCK2_VSEL      0x90U
#define LDO_ON          0x80U
#define LDO_VSEL        0x7FU

#define SENSOR_ADDR     0x35U
#define MODE_LINES      0x04F2U             /* the mode's frame: 1266 lines of 26.3 us */
#define EXP_MAX         (MODE_LINES - 4U)   /* SENSOR_3H1_COARSE_INTEGRATION_TIME_MAX_MARGIN */
#define CELLS_W         (CAM_FRAME_W / 2U)
#define CELLS_H         (CAM_FRAME_H / 2U)
#define MARK_OFF        ((uint32_t)CAM_FRAME_H * CAM_LINE_BYTES - 8U)
#define FILL64          0xA5A5A5A5A5A5A5A5ULL

/* ---- the sensor's mode -----------------------------------------------------------------------------------------------
 * Samsung's setfile A for the 3H1, 1632 x 1224 at 30 frames a second (fimc-is-cis-3h1-setA.h,
 * sensor_3h1_setfile_A_1632x1224_30fps, GPL-2.0): the sensor's own register values, every one a 16-bit write, in
 * Samsung's order. It leaves the frame at 0x04F2 lines (0x0340) of 0x2970 clocks (0x0342), output 0x0660 x 0x04C8
 * (0x034C / 0x034E), the PLL from a 26 MHz MCLK (0x0136 = 0x1A00). */
static const uint16_t mode_a[][2] = {
    { 0xFCFC, 0x4000 }, { 0x6028, 0x2000 }, { 0x6214, 0x7971 }, { 0x6218, 0x7150 }, { 0x0344, 0x0010 },
    { 0x0348, 0x0CCF }, { 0x0346, 0x0010 }, { 0x034A, 0x099F }, { 0x034C, 0x0660 }, { 0x034E, 0x04C8 },
    { 0x0408, 0x0000 }, { 0x040A, 0x0000 }, { 0x0342, 0x2970 }, { 0x0340, 0x04F2 }, { 0x0900, 0x0112 },
    { 0x0386, 0x0003 }, { 0x0400, 0x0001 }, { 0x0404, 0x0020 }, { 0x379E, 0x0104 }, { 0x31B8, 0x0020 },
    { 0x37FA, 0x0032 }, { 0xF46C, 0x0010 }, { 0xF414, 0x64C6 }, { 0xF416, 0x002F }, { 0xF462, 0x0000 },
    { 0xF464, 0x0000 }, { 0xF476, 0x0082 }, { 0x37C2, 0x0804 }, { 0x37C6, 0x0403 }, { 0x37BE, 0x0909 },
    { 0x37BA, 0x130D }, { 0xF44C, 0x0014 }, { 0xF452, 0x000B }, { 0xF44E, 0x0012 }, { 0x37DE, 0x0000 },
    { 0x37E0, 0x7FE7 }, { 0x37E2, 0x03FF }, { 0x37F0, 0x007F }, { 0x0200, 0x13B8 }, { 0x021C, 0x13B8 },
    { 0x3158, 0x0060 }, { 0x3254, 0x0001 }, { 0x3258, 0x0001 }, { 0x325C, 0x0003 }, { 0x3260, 0x0002 },
    { 0x3298, 0x000A }, { 0x32B8, 0x0002 }, { 0x32BC, 0x0000 }, { 0x31B2, 0x0200 }, { 0x3ACE, 0xFECD },
    { 0x3B00, 0xFEF6 }, { 0x3B32, 0xFEF6 }, { 0x3E0C, 0x0FF7 }, { 0x3DF8, 0x1008 }, { 0x3E02, 0x1008 },
    { 0x3AC4, 0xFF29 }, { 0x3AF6, 0xFF3E }, { 0x3B28, 0xFF3E }, { 0x3E0A, 0x0FF7 }, { 0x3DF6, 0x1000 },
    { 0x3E00, 0x1000 }, { 0x3ABA, 0xFF43 }, { 0x3AEC, 0xFF7B }, { 0x3B1E, 0xFF7B }, { 0x3E08, 0x1000 },
    { 0x3DF4, 0x0FF7 }, { 0x3DFE, 0x0FF7 }, { 0x3DE4, 0x0001 }, { 0x3AA4, 0x0100 }, { 0xF4A2, 0x000C },
    { 0x0136, 0x1A00 }, { 0x1130, 0x43CA }, { 0x1132, 0x0000 }, { 0x0304, 0x0007 }, { 0x0306, 0x00A3 },
    { 0x030C, 0x0003 }, { 0x030E, 0x0053 }, { 0x300A, 0x0001 }, { 0x3092, 0x0000 }, { 0x31AA, 0x0001 },
    { 0xB134, 0x0000 }, { 0x307C, 0x0340 }, { 0x3052, 0x0104 }, { 0x6214, 0x7970 }, { 0x0216, 0x0000 },
};
#define MODE_N (sizeof(mode_a) / sizeof(mode_a[0]))

/* ---- the HAL and the log ------------------------------------------------------------------------------------------ */
static struct cam_hal H;
static int hal_ok;

static void fmt_u(char *b, unsigned *n, unsigned cap, unsigned long long v, unsigned base, unsigned width, char pad)
{
    char t[24];
    unsigned k = 0;
    do {
        unsigned d = (unsigned)(v % base);
        t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v && k < sizeof(t));
    while (width > k && *n + 1 < cap) {
        b[(*n)++] = pad;
        width--;
    }
    while (k && *n + 1 < cap)
        b[(*n)++] = t[--k];
}

/* %s %c %d %u %x with an optional 0 and width, and l / ll */
static void vfmt(char *b, unsigned cap, const char *f, va_list ap)
{
    unsigned n = 0;
    for (; *f && n + 1 < cap; ++f) {
        if (*f != '%') {
            b[n++] = *f;
            continue;
        }
        ++f;
        char pad = ' ';
        unsigned width = 0, l = 0;
        if (*f == '0')
            pad = '0', ++f;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (unsigned)(*f++ - '0');
        while (*f == 'l')
            l++, f++;
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(none)";
            while (*s && n + 1 < cap)
                b[n++] = *s++;
            break;
        }
        case 'c':
            b[n++] = (char)va_arg(ap, int);
            break;
        case 'd': {
            long long v = l >= 2 ? va_arg(ap, long long) : l ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0 && n + 1 < cap)
                b[n++] = '-', v = -v;
            fmt_u(b, &n, cap, (unsigned long long)v, 10, width, pad);
            break;
        }
        case 'u':
        case 'x': {
            unsigned long long v = l >= 2 ? va_arg(ap, unsigned long long) : l ? va_arg(ap, unsigned long)
                                                                                : va_arg(ap, unsigned);
            fmt_u(b, &n, cap, v, *f == 'x' ? 16 : 10, width, pad);
            break;
        }
        case '%':
            b[n++] = '%';
            break;
        default:
            return;
        }
        if (!*f)
            break;
    }
    b[n] = 0;
}

static void say(const char *f, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *f, ...)
{
    char line[400] = "cam: ";
    va_list ap;
    va_start(ap, f);
    vfmt(line + 5, sizeof(line) - 5, f, ap);
    va_end(ap);
    if (hal_ok)
        H.log(H.ctx, line);
}

static void psay(const char *f, ...) __attribute__((format(printf, 1, 2)));
static void psay(const char *f, ...)
{
    char line[400] = "drvprobe: cam: ";
    va_list ap;
    va_start(ap, f);
    vfmt(line + 15, sizeof(line) - 15, f, ap);
    va_end(ap);
    if (hal_ok)
        H.log(H.ctx, line);
}

static uint64_t now_us(void)
{
    return H.now_us(H.ctx);
}

static void spin(uint32_t us)
{
    H.delay_us(H.ctx, us);
}

/* ---- registers ---------------------------------------------------------------------------------------------------- */
static unsigned faults, refused;

static uint32_t rd(uint64_t pa)
{
    uint32_t v = 0;
    if (!H.rd32(H.ctx, pa, &v)) {
        if (faults++ < 8)
            say("a read of 0x%llx faulted", (unsigned long long)pa);
        return 0;
    }
    return v;
}

/* the blocks this driver may write: everything else is refused before it reaches the bus */
static int may_write(uint64_t pa)
{
    static const uint64_t one[] = {
        0x1081102CULL,                                          /* USI08_I2C's mode (usi16) */
        CMU_PERIC1 + 0x2050, CMU_PERIC1 + 0x2098, CMU_PERIC1 + 0x20EC, CMU_PERIC1 + 0x20F0, CMU_PERIC1 + 0x303C,
        0x1081103CULL,                                          /* USI10's mode (the S2MPB02's bus) */
        CIS1_MUX, CIS1_DIV, CIS1_GATE, CIS1_QCH, ISPB_MUX, ISPB_DIV, ISPB_GATE,
        PMU_DPHY_ISO, PMU_ISP_CONF, PMU_ISP_MEM, ISPPRE_USER, DPHY_RESET,
    };
    static const uint64_t span[][2] = {
        { GPG2, 0x10 }, { GPP5, 0x10 }, { GPP6, 0x10 }, { GPC1, 0x10 },
        { CMU_ISPPRE + 0x3010, 0x64 },                          /* the Q-channels 0x3010..0x3070 */
        { CSIS, 0x1000 }, { DPHY, 0x3000 }, { WDMA, 0x1000 }, { CDMA, 0x40 },
    };
    for (unsigned i = 0; i < sizeof(one) / sizeof(one[0]); ++i)
        if (pa == one[i])
            return 1;
    for (unsigned i = 0; i < sizeof(span) / sizeof(span[0]); ++i)
        if (pa >= span[i][0] && pa < span[i][0] + span[i][1])
            return 1;
    return 0;
}

static void wr(uint64_t pa, uint32_t v)
{
    if (!may_write(pa)) {
        if (refused++ < 8)
            say("BUG: a write of 0x%x to 0x%llx refused (not a camera register)", v, (unsigned long long)pa);
        return;
    }
    H.wr32(H.ctx, pa, v);
}

static void upd(uint64_t pa, uint32_t mask, uint32_t val)
{
    uint32_t o = rd(pa), n = (o & ~mask) | (val & mask);
    if (n != o)
        wr(pa, n);
}

static int wait_for(uint64_t pa, uint32_t mask, uint32_t want, uint32_t us)
{
    uint64_t t0 = now_us();
    for (;;) {
        if ((rd(pa) & mask) == want)
            return 1;
        if (now_us() - t0 >= us)
            return (rd(pa) & mask) == want;
        spin(10);
    }
}

/* ---- pins --------------------------------------------------------------------------------------------------------- */
struct pin {
    uint64_t bank;
    unsigned n;
    uint32_t con, dat, pud;                 /* as found: the pin's nibble / bit */
    int saved;
};

static void pin_save(struct pin *p)
{
    p->con = (rd(p->bank) >> (4 * p->n)) & 0xFU;
    p->dat = (rd(p->bank + 4) >> p->n) & 1U;
    p->pud = (rd(p->bank + 8) >> (4 * p->n)) & 0xFU;
    p->saved = 1;
}

static void pin_fn(struct pin *p, unsigned fn)
{
    upd(p->bank, 0xFU << (4 * p->n), fn << (4 * p->n));
}

static void pin_level(struct pin *p, int high)
{
    upd(p->bank + 4, 1U << p->n, high ? 1U << p->n : 0);
}

/* an output at a level: the level first, then the direction */
static void pin_out(struct pin *p, int high)
{
    pin_level(p, high);
    pin_fn(p, 1);
}

static void pin_restore(struct pin *p)
{
    if (!p->saved)
        return;
    pin_level(p, (int)p->dat);
    upd(p->bank, 0xFU << (4 * p->n), p->con << (4 * p->n));
    upd(p->bank + 8, 0xFU << (4 * p->n), p->pud << (4 * p->n));
    p->saved = 0;
}

static struct pin reset_pin = { GPG2, RESET_PIN, 0, 0, 0, 0 };
static struct pin mclk_pin = { GPC1, MCLK_PIN, 0, 0, 0, 0 };

/* ---- the two I2C buses -------------------------------------------------------------------------------------------- */
struct bus {
    const char *name;
    unsigned id;
    uint64_t base, mode, bank;
    uint32_t pinmask, pinwant;
    uint64_t gate[4], qch;                  /* CMU_PERIC1 offsets; qch 0: not touched */
    int may_open, restore;                  /* closed gates opened here; put back at the end */
    /* as found */
    uint32_t f_mode, f_con, f_pud, f_gate[4], f_qch;
    int mine, up;
};

static struct bus pbus = { "the S2MPB02's bus (USI10)", CAM_BUS_PMIC, 0x10920000ULL, 0x1081103CULL, GPP6, 0xFFU, 0x22U,
                           { 0x2048, 0x20AC, 0x2114, 0x2118 }, 0, 0, 0, 0, 0, 0, { 0, 0, 0, 0 }, 0, 0, 0 };
static struct bus sbus = { "the sensor's bus (USI08_I2C)", CAM_BUS_SENSOR, 0x108F0000ULL, 0x1081102CULL, GPP5, 0xFF00U,
                           0x3300U, { 0x2050, 0x2098, 0x20EC, 0x20F0 }, 0x303C, 1, 1, 0, 0, 0, { 0, 0, 0, 0 }, 0, 0, 0 };

static int gate_closed(uint32_t g)
{
    return (g & GATE_ON) == 0x00100000U;    /* manual and off */
}

/* is the bus already set up by someone (the torch driver): mode I2C, pins on, gates open, a timing programmed */
static int bus_ready(struct bus *b)
{
    if (rd(b->mode) != 4U || (rd(b->bank) & b->pinmask) != b->pinwant)
        return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (gate_closed(rd(CMU_PERIC1 + b->gate[i])))
            return 0;
    return ((rd(b->base + 0x68) >> 16) & 0xFFU) != 0;
}

/* the controller's timing: the sensor's bus at 400 kHz from an input clock taken as 400 MHz (a lower real clock only
 * makes the bus slower): FS3 [23:16] the divider 66, FS2 [7:0] 0xE0, FS1 [23:16] 0xF0; the S2MPB02's as fled.c: what
 * is there, else Android's 200 MHz / 400 kHz words */
static void bus_timing(struct bus *b, uint32_t *fs1, uint32_t *fs2, uint32_t *fs3)
{
    *fs1 = rd(b->base + 0x60), *fs2 = rd(b->base + 0x64), *fs3 = rd(b->base + 0x68);
    if (b->id == CAM_BUS_SENSOR) {
        *fs3 = (*fs3 & ~0x00FF0000U) | (66U << 16);
        *fs2 = (*fs2 & ~0xFFU) | 0xE0U;
        *fs1 = (*fs1 & ~0x00FF0000U) | (0xF0U << 16);
    } else if (!((*fs3 >> 16) & 0xFFU)) {
        *fs1 = 0x01F0FF00U, *fs2 = 0x030003E0U, *fs3 = 0x00210000U;
    }
}

/* the controller up (gates, Q-channel, mode, timing); pins only with pins = 1 */
static int bus_up(struct bus *b, int pins)
{
    if (b->up && !b->restore && !bus_ready(b))
        b->up = 0, b->mine = 0;             /* the shared bus was taken down meanwhile: set it up again */
    if (b->up) {
        if (pins) {
            upd(b->bank, b->pinmask, b->pinwant);
            upd(b->bank + 8, b->pinmask, 0);
        }
        return 1;
    }
    if (!b->mine) {
        b->f_mode = rd(b->mode);
        b->f_con = rd(b->bank) & b->pinmask;
        b->f_pud = rd(b->bank + 8) & b->pinmask;
        for (unsigned i = 0; i < 4; ++i)
            b->f_gate[i] = rd(CMU_PERIC1 + b->gate[i]);
        b->f_qch = b->qch ? rd(CMU_PERIC1 + b->qch) : 0;
    }
    for (unsigned i = 0; i < 4; ++i)
        if (gate_closed(b->f_gate[i]) && !b->may_open) {
            say("%s: CMU_PERIC1 gate 0x%llx is closed (0x%x): not opened from here", b->name,
                (unsigned long long)b->gate[i], b->f_gate[i]);
            return 0;
        }
    b->mine = 1;
    for (unsigned i = 0; i < 4; ++i)
        if (gate_closed(b->f_gate[i]))
            wr(CMU_PERIC1 + b->gate[i], b->f_gate[i] | GATE_ON);
    if (b->qch)
        upd(CMU_PERIC1 + b->qch, 3U, 3U);
    if (b->f_mode != 4U)
        wr(b->mode, 4U);
    if (pins) {
        upd(b->bank, b->pinmask, b->pinwant);
        upd(b->bank + 8, b->pinmask, 0);
    }
    uint32_t fs1, fs2, fs3;
    bus_timing(b, &fs1, &fs2, &fs3);
    H.i2c_init(H.ctx, b->id, fs1, fs2, fs3, 1);
    b->up = 1;
    say("%s up: mode 0x%x (found 0x%x), pins 0x%x (found 0x%x), gates 0x%x 0x%x 0x%x 0x%x, timing 0x%x 0x%x 0x%x",
        b->name, rd(b->mode), b->f_mode, rd(b->bank) & b->pinmask, b->f_con, rd(CMU_PERIC1 + b->gate[0]),
        rd(CMU_PERIC1 + b->gate[1]), rd(CMU_PERIC1 + b->gate[2]), rd(CMU_PERIC1 + b->gate[3]), fs1, fs2, fs3);
    return 1;
}

/* the pins first (inputs again: nothing drives a powered-down part), then the mode, the Q-channel, the gates */
static void bus_down(struct bus *b)
{
    if (!b->mine || !b->restore)
        return;
    upd(b->bank, b->pinmask, b->f_con);
    upd(b->bank + 8, b->pinmask, b->f_pud);
    if (rd(b->mode) != b->f_mode)
        wr(b->mode, b->f_mode);
    if (b->qch)
        upd(CMU_PERIC1 + b->qch, 3U, b->f_qch);
    for (unsigned i = 4; i-- > 0;)
        if (gate_closed(b->f_gate[i]))
            wr(CMU_PERIC1 + b->gate[i], b->f_gate[i]);
    b->mine = 0;
    b->up = 0;
}

/* ---- the S2MPB02 -------------------------------------------------------------------------------------------------- */
static const struct ldo {
    const char *name;
    uint8_t reg, vsel;
} ldos[4] = {
    { "VDDD_1.05V_VT (LDO5)", 0x22, 0x24 },
    { "VDDIO_1.8V_VT (LDO6)", 0x23, 0x30 },
    { "VDDA_2.8V_VT (LDO13)", 0x2A, 0x58 },
    { "VDDAF_2.8V_VT (LDO16)", 0x2D, 0x58 },
};
#define VDDD  0
#define VDDIO 1
#define VDDA  2
#define VDDAF 3

static int pmic_rd(uint8_t reg, uint8_t *v)
{
    for (unsigned t = 0; t < 2; ++t) {
        uint8_t r = reg;
        if (H.i2c(H.ctx, CAM_BUS_PMIC, PMIC_ADDR, 0, &r, 1, t) && H.i2c(H.ctx, CAM_BUS_PMIC, PMIC_ADDR, 1, v, 1, 1))
            return 1;
    }
    return 0;
}

static struct {
    int bad;                                /* a write did not read back: no more writes this run */
    uint8_t f_b1, f_b2, f_ldo[4];           /* as found */
    int b2_mine, ldo_mine[4];
} P;

/* what may be written: the four LDOs at their voltage, on or off, or off as found; BUCK2's voltage 1.3 V or as found;
 * BUCK2's enable bits only */
static int pmic_allowed(uint8_t reg, uint8_t v)
{
    for (unsigned i = 0; i < 4; ++i)
        if (reg == ldos[i].reg)
            return v == ldos[i].vsel || v == (LDO_ON | ldos[i].vsel) || (v == P.f_ldo[i] && !(v & LDO_ON));
    if (reg == B2CTRL2)
        return v == BUCK2_VSEL || v == P.f_b2;
    if (reg == B2CTRL1)
        return (v & ~BUCK_ON) == (P.f_b1 & ~BUCK_ON) && ((v & BUCK_ON) == 0 || (v & BUCK_ON) == BUCK_ON);
    return 0;
}

/* the only way anything reaches the S2MPB02: an allowed register and value, read back */
static int pmic_put(uint8_t reg, uint8_t v, const char *what)
{
    if (P.bad)
        return 0;
    if (!pmic_allowed(reg, v)) {
        say("BUG: S2MPB02 0x%x (%s) = 0x%x refused: not a value this driver writes", reg, what, v);
        P.bad = 1;
        return 0;
    }
    uint8_t m[2] = { reg, v }, back = 0;
    int ok = H.i2c(H.ctx, CAM_BUS_PMIC, PMIC_ADDR, 0, m, 2, 1) && pmic_rd(reg, &back);
    if (ok)
        ok = reg == B2CTRL1 ? (back & BUCK_ON) == (v & BUCK_ON) : back == v;
    if (!ok) {
        say("S2MPB02 0x%x (%s): wrote 0x%x, read back 0x%x: no more writes this run", reg, what, v, back);
        P.bad = 1;
    }
    return ok;
}

/* switching off is always tried, even after a failed write */
static void pmic_off(uint8_t reg, uint8_t v, const char *what, int *mine)
{
    int was = P.bad;
    P.bad = 0;
    if (pmic_put(reg, v, what))
        *mine = 0;
    P.bad = P.bad || was;
}

/* ---- the driver's state ------------------------------------------------------------------------------------------- */
enum {
    PH_OFF, PH_LOOK, PH_POWER, PH_POWER2, PH_ID, PH_ISP, PH_FILL, PH_LINK, PH_MODE, PH_STREAM, PH_RUN, PH_FAILED
};

static struct {
    int phase, err;
    const char *why;
    struct cam_req req;
    uint64_t t_open, t_phase, t_frame, t_fps, t_stream, t_first;
    unsigned fill_k, id_tries;
    volatile uint8_t *mem;
    /* found / mine */
    uint32_t f_cmux, f_cdiv, f_cgate, f_cqch;
    int clk_mine, powered, sensor_bad;
    int dom_mine, dom_up;
    uint32_t f_mem, f_tgate, f_tmux, f_tdiv, f_user, f_q[8];
    int tgate_mine, trate_mine, qch_mine;
    uint32_t f_iso, f_dres, f_cdma[4];
    int phy_mine, link_on, dma_on, cdma_mine, streaming;
    /* the stream */
    uint8_t map[8];                         /* the WDMA's frame pointer -> slot */
    int hold;
    unsigned held;
    unsigned frames, fps_frames, dma_err, ae_wait;
    uint32_t fps10, model, rev, lines, again, dgain;
    uint32_t seq;
    int have_pic, last_slot;
    struct cam_wb wb;
    struct cam_stats last;
    struct cam_pstats ps;                   /* v2: the last frame's measures */
    uint32_t luma, aim;                     /* v2: the metered luma (12-bit linear) and its aim */
    unsigned settled;                       /* v2: frames in a row within 20 % of the aim (or at a range end) */
    uint32_t isp_us, isp_max, isp_n;        /* the preview's processing time over the streaming line's 2 s */
    /* the still */
    int still;                              /* 0 none, 1 asked, 2 rendering, 3 ready */
    unsigned still_row, sw, sh;
    uint32_t bmp_len;
} C;

static const unsigned ring_bytes = CAM_SLOT_BYTES;

static void set_why(int err, const char *why)
{
    C.err = err;
    C.why = why;
}

static void fail(int err, const char *why);

static volatile uint8_t *slot_mem(unsigned k)
{
    return C.mem + (size_t)k * ring_bytes;
}

static uint64_t slot_pa(unsigned k)
{
    return CAM_MEM_PA + (uint64_t)k * ring_bytes;
}

static int slot_done(unsigned k)
{
    return *(const volatile uint64_t *)(slot_mem(k) + MARK_OFF) != FILL64;
}

static void slot_arm(unsigned k)
{
    *(volatile uint64_t *)(slot_mem(k) + MARK_OFF) = FILL64;
}

/* ---- the sensor --------------------------------------------------------------------------------------------------- */
static int s_rd(uint16_t reg, uint8_t *b, unsigned n)
{
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return H.i2c(H.ctx, CAM_BUS_SENSOR, SENSOR_ADDR, 0, a, 2, 0) && H.i2c(H.ctx, CAM_BUS_SENSOR, SENSOR_ADDR, 1, b, n, 1);
}

static int s_wr(uint16_t reg, uint16_t v, unsigned n)
{
    uint8_t b[4] = { (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(n == 2 ? v >> 8 : v), (uint8_t)v };
    if (C.sensor_bad)
        return 0;
    if (!H.i2c(H.ctx, CAM_BUS_SENSOR, SENSOR_ADDR, 0, b, 2U + n, 1)) {
        say("the sensor did not take 0x%04x = 0x%x", reg, v);
        C.sensor_bad = 1;
        return 0;
    }
    return 1;
}

static int exposure_put(uint32_t lines, uint32_t again, uint32_t dgain)
{
    int ok = s_wr(0x0104, 1, 1) && s_wr(0x0202, (uint16_t)lines, 2) && s_wr(0x0204, (uint16_t)again, 2) &&
             s_wr(0x020E, (uint16_t)dgain, 2);
    (void)s_wr(0x0104, 0, 1);
    if (ok)
        C.lines = lines, C.again = again, C.dgain = dgain;
    return ok;
}

/* ---- the sensor's power, in the order of Samsung's fimc-is-device-module-3h1.c (star) ------------------------------ */
static int clock_on(void)
{
    C.f_cmux = rd(CIS1_MUX), C.f_cdiv = rd(CIS1_DIV), C.f_cgate = rd(CIS1_GATE), C.f_cqch = rd(CIS1_QCH);
    C.clk_mine = 1;
    if (C.f_cmux & 1U)
        wr(CIS1_MUX, C.f_cmux & ~1U);       /* OSCCLK */
    if (C.f_cdiv & 0x1FU)
        wr(CIS1_DIV, C.f_cdiv & ~0x1FU);    /* undivided: 26 MHz */
    upd(CIS1_QCH, 3U, 3U);
    upd(CIS1_GATE, GATE_ON, GATE_ON);
    say("CIS_CLK1 26 MHz: mux 0x%x -> 0x%x, divider 0x%x -> 0x%x, gate 0x%x -> 0x%x", C.f_cmux, rd(CIS1_MUX), C.f_cdiv,
        rd(CIS1_DIV), C.f_cgate, rd(CIS1_GATE));
    return 1;
}

static void clock_off(void)
{
    if (!C.clk_mine)
        return;
    wr(CIS1_GATE, C.f_cgate);
    wr(CIS1_QCH, C.f_cqch);
    wr(CIS1_DIV, C.f_cdiv);
    wr(CIS1_MUX, C.f_cmux);
    C.clk_mine = 0;
}

static int rails_look(int log_only)
{
    uint8_t id = 0, id0 = 0;
    if (!pmic_rd(PMIC_ID_REG, &id)) {
        set_why(CAM_EPMIC, "The camera's power chip does not answer");
        say("the S2MPB02 does not answer at 0x59");
        return 0;
    }
    (void)pmic_rd(0x00, &id0);
    int ok = pmic_rd(B2CTRL1, &P.f_b1) && pmic_rd(B2CTRL2, &P.f_b2);
    for (unsigned i = 0; ok && i < 4; ++i)
        ok = pmic_rd(ldos[i].reg, &P.f_ldo[i]);
    say("S2MPB02: 0x1D 0x%02x (0x90 expected), 0x00 0x%02x; BUCK2 0x0D 0x%02x 0x0E 0x%02x (%s); %s 0x%02x, %s 0x%02x, "
        "%s 0x%02x, %s 0x%02x", id, id0, P.f_b1, P.f_b2, (P.f_b1 & BUCK_ON) == BUCK_ON ? "on" : "off", ldos[0].name,
        P.f_ldo[0], ldos[1].name, P.f_ldo[1], ldos[2].name, P.f_ldo[2], ldos[3].name, P.f_ldo[3]);
    if (!ok) {
        set_why(CAM_EPMIC, "The camera's power chip does not answer");
        return 0;
    }
    if (id != PMIC_ID) {
        set_why(CAM_EPMIC, "The camera's power chip is not the expected one");
        return 0;
    }
    if (log_only)
        return 1;
    if ((P.f_b1 & BUCK_ON) == BUCK_ON && P.f_b2 != BUCK2_VSEL) {
        set_why(CAM_EPMIC, "A camera supply is in use at another voltage");
        say("BUCK2 is on at 0x%x, not 1.3 V: someone else's; nothing written", P.f_b2);
        return 0;
    }
    for (unsigned i = 0; i < 4; ++i)
        if ((P.f_ldo[i] & LDO_ON) && (P.f_ldo[i] & LDO_VSEL) != ldos[i].vsel) {
            set_why(CAM_EPMIC, "A camera supply is in use at another voltage");
            say("%s is on at 0x%x: someone else's; nothing written", ldos[i].name, P.f_ldo[i]);
            return 0;
        }
    return 1;
}

static int buck2_on(void)
{
    if ((P.f_b1 & BUCK_ON) == BUCK_ON) {
        say("BUCK2 was on already: left as it is");
        return 1;
    }
    if (P.f_b2 != BUCK2_VSEL && !pmic_put(B2CTRL2, BUCK2_VSEL, "BUCK2's voltage"))
        return 0;
    P.b2_mine = 1;
    if (!pmic_put(B2CTRL1, (uint8_t)((P.f_b1 & ~BUCK_ON) | BUCK_ON), "BUCK2"))
        return 0;
    spin(200);
    return 1;
}

static int ldo_on(unsigned i)
{
    if (P.f_ldo[i] & LDO_ON) {
        say("%s was on already: left as it is", ldos[i].name);
        return 1;
    }
    if ((P.f_ldo[i] & LDO_VSEL) != ldos[i].vsel && !pmic_put(ldos[i].reg, ldos[i].vsel, ldos[i].name))
        return 0;
    P.ldo_mine[i] = 1;
    if (!pmic_put(ldos[i].reg, (uint8_t)(LDO_ON | ldos[i].vsel), ldos[i].name))
        return 0;
    spin(200);                              /* an LDO's enable time is 180 us */
    return 1;
}

/* off, and its voltage as found (a supply this run switched on was found off) */
static void ldo_off(unsigned i)
{
    if (P.ldo_mine[i])
        pmic_off(ldos[i].reg, P.f_ldo[i], ldos[i].name, &P.ldo_mine[i]);
}

/* the first half: clock, reset low, the supplies; the bus pins and the reset come 1 ms later (power2) */
static int power_up(void)
{
    C.powered = 1;
    pin_save(&reset_pin);
    pin_save(&mclk_pin);
    if (!clock_on())
        return 0;
    pin_out(&reset_pin, 0);
    if (!buck2_on() || !ldo_on(VDDAF) || !ldo_on(VDDA) || !ldo_on(VDDIO) || !ldo_on(VDDD)) {
        set_why(CAM_EPMIC, "A camera supply did not switch on");
        return 0;
    }
    say("supplies on: BUCK2 1.3 V, VDDAF 2.8 V, VDDA 2.8 V, VDDIO 1.8 V, VDDD 1.05 V; reset low");
    return 1;
}

static int power_up2(void)
{
    if (!bus_up(&sbus, 1)) {
        set_why(CAM_EBUS, "The camera's I2C bus could not be set up");
        return 0;
    }
    spin(10);
    pin_out(&reset_pin, 1);
    pin_fn(&mclk_pin, 2);                   /* MCLK out */
    say("the sensor's bus on, reset high, MCLK out on gpc1-5");
    return 1;
}

/* the module file's order: MCLK and reset low, the bus pins inputs, VDDAF, VDDD, VDDA, VDDIO, BUCK2; then the pins,
 * the clock and the bus as found */
static void power_down(void)
{
    if (!C.powered)
        return;
    if (mclk_pin.saved)
        pin_out(&mclk_pin, 0);
    if (reset_pin.saved)
        pin_out(&reset_pin, 0);
    spin(1000);
    if (sbus.mine)
        upd(sbus.bank, sbus.pinmask, 0);
    ldo_off(VDDAF);
    ldo_off(VDDD);
    ldo_off(VDDA);
    ldo_off(VDDIO);
    if (P.b2_mine) {
        pmic_off(B2CTRL1, (uint8_t)(P.f_b1 & ~BUCK_ON), "BUCK2", &P.b2_mine);
        if (!P.b2_mine && P.f_b2 != BUCK2_VSEL) {
            int one = 1;
            pmic_off(B2CTRL2, P.f_b2, "BUCK2's voltage", &one);     /* as found, now that it is off */
        }
    }
    pin_restore(&mclk_pin);
    pin_restore(&reset_pin);
    clock_off();
    bus_down(&sbus);
    C.powered = 0;
    int left = P.ldo_mine[0] || P.ldo_mine[1] || P.ldo_mine[2] || P.ldo_mine[3] || P.b2_mine;
    say("sensor powered down: %s; pins, CIS_CLK1 and its bus as found", left ? "A SUPPLY DID NOT SWITCH OFF" :
                                                                                 "supplies off as found");
}

/* ---- the ISPPRE domain and its clocks ----------------------------------------------------------------------------- */
static const uint32_t qchs[8] = { 0x3020, 0x3024, 0x3028, 0x302C, 0x3038, 0x305C, 0x306C, 0x3010 };

static int isp_up(void)
{
    uint32_t dpu = rd(PMU_DPU_STAT), st = rd(PMU_ISP_STAT);
    C.f_mem = rd(PMU_ISP_MEM);
    say("PMU: DPU status 0x%x, ISPPRE configuration 0x%x status 0x%x, MEMORY_ISPPRE_OPTION 0x%x", dpu, rd(PMU_ISP_CONF),
        st, C.f_mem);
    if ((dpu & PD_ON) != PD_ON) {
        set_why(CAM_EPOWER, "The camera's power domain cannot come on (the display's is off)");
        return 0;
    }
    if ((st & PD_ON) == PD_ON) {
        say("ISPPRE was on already: used as it is, no SMC, left on");
    } else {
        C.dom_mine = 1;
        upd(PMU_ISP_CONF, PD_ON, PD_ON);
        if (!wait_for(PMU_ISP_STAT, PD_ON, PD_ON, 10000)) {
            say("ISPPRE did not come on (status 0x%x): its configuration back to off", rd(PMU_ISP_STAT));
            upd(PMU_ISP_CONF, PD_ON, 0);
            (void)wait_for(PMU_ISP_STAT, PD_ON, 0, 10000);
            C.dom_mine = 0;
            set_why(CAM_EPOWER, "The camera's power domain did not come on");
            return 0;
        }
        int64_t a = H.smc(H.ctx, SMC_PD, 1, DTZPC_ISPPRE, 0);
        say("ISPPRE on; SMC 0x82000410 (1, 0x16210004): the DTZPC restore returned 0x%llx", (unsigned long long)a);
        if ((uint32_t)a != 0) {
            upd(PMU_ISP_CONF, PD_ON, 0);
            (void)wait_for(PMU_ISP_STAT, PD_ON, 0, 10000);
            C.dom_mine = 0;
            set_why(CAM_EPOWER, "The secure monitor refused the camera's power domain");
            return 0;
        }
    }
    C.dom_up = 1;
    C.f_tgate = rd(ISPB_GATE), C.f_tmux = rd(ISPB_MUX), C.f_tdiv = rd(ISPB_DIV);
    if ((C.f_tgate & GATE_ON) != GATE_ON) {
        C.tgate_mine = 1;
        wr(ISPB_GATE, C.f_tgate | GATE_ON);
    }
    if ((C.f_tmux & 3U) != 1U || (C.f_tdiv & 0xFU) != 1U) {
        C.trate_mine = 1;
        wr(ISPB_MUX, (C.f_tmux & ~3U) | 1U);                    /* SHARED1_DIV4: the mux first, to the lower rate */
        (void)wait_for(ISPB_MUX, TOP_BUSY, 0, 1000);
        wr(ISPB_DIV, (C.f_tdiv & ~0xFU) | 1U);                  /* / 2: 233 MHz */
        (void)wait_for(ISPB_DIV, TOP_BUSY, 0, 1000);
    }
    C.f_user = rd(ISPPRE_USER);
    for (unsigned i = 0; i < 8; ++i)
        C.f_q[i] = rd(CMU_ISPPRE + qchs[i]);
    C.qch_mine = 1;
    if (!(C.f_user & (1U << 4))) {
        wr(ISPPRE_USER, C.f_user | (1U << 4));
        (void)wait_for(ISPPRE_USER, 1U << 7, 0, 1000);
    }
    for (unsigned i = 0; i < 8; ++i)
        upd(CMU_ISPPRE + qchs[i], 2U, 2U);
    uint32_t ver = rd(CSIS_VERSION);
    say("ISPPRE clocks: CLKCMU_ISPPRE_BUS mux 0x%x div 0x%x gate 0x%x, BUS_USER 0x%x; CSIS1 version 0x%x", rd(ISPB_MUX),
        rd(ISPB_DIV), rd(ISPB_GATE), rd(ISPPRE_USER), ver);
    if (!ver || ver == 0xFFFFFFFFU) {
        set_why(CAM_EPOWER, "The camera interface does not answer");
        return 0;
    }
    return 1;
}

static void isp_down(void)
{
    if (C.qch_mine && C.dom_up && !C.dom_mine) {
        for (unsigned i = 0; i < 8; ++i)
            wr(CMU_ISPPRE + qchs[i], C.f_q[i]);              /* a domain found on: its clocks as found */
        wr(ISPPRE_USER, C.f_user);
    }
    C.qch_mine = 0;
    if (C.dom_mine) {
        upd(PMU_ISP_MEM, 3U, 2U);
        int64_t a = H.smc(H.ctx, SMC_PD, 0, DTZPC_ISPPRE, 0);
        if ((uint32_t)a != 0) {
            say("the DTZPC save returned 0x%llx: the domain is LEFT ON", (unsigned long long)a);
        } else {
            C.dom_up = 0;
            upd(PMU_ISP_CONF, PD_ON, 0);
            if (!wait_for(PMU_ISP_STAT, PD_ON, 0, 10000)) {
                say("ISPPRE did not go off (status 0x%x): on again, its protection restored, LEFT ON",
                    rd(PMU_ISP_STAT));
                upd(PMU_ISP_CONF, PD_ON, PD_ON);
                if (wait_for(PMU_ISP_STAT, PD_ON, PD_ON, 10000))
                    (void)H.smc(H.ctx, SMC_PD, 1, DTZPC_ISPPRE, 0);
                C.dom_up = 1;
            }
        }
        if (!C.dom_up)
            upd(PMU_ISP_MEM, 3U, C.f_mem);                  /* the option as found, now that the domain is off */
        C.dom_mine = 0;
    }
    C.dom_up = 0;
    if (C.trate_mine) {
        wr(ISPB_DIV, C.f_tdiv);                             /* the divider first: back through the slower rate */
        (void)wait_for(ISPB_DIV, TOP_BUSY, 0, 1000);
        wr(ISPB_MUX, C.f_tmux);
        (void)wait_for(ISPB_MUX, TOP_BUSY, 0, 1000);
        C.trate_mine = 0;
    }
    if (C.tgate_mine) {
        wr(ISPB_GATE, C.f_tgate);
        C.tgate_mine = 0;
    }
}

/* ---- the D-PHY, CSIS1 and its WDMA -------------------------------------------------------------------------------- */
static void phy_on(void)
{
    C.f_iso = rd(PMU_DPHY_ISO), C.f_dres = rd(DPHY_RESET);
    C.phy_mine = 1;
    upd(PMU_DPHY_ISO, 1U, 1U);
    upd(DPHY_RESET, DPHY_RESET_CSIS1, 0);
    upd(DPHY_RESET, DPHY_RESET_CSIS1, DPHY_RESET_CSIS1);
    /* the timing for 719 Mb/s a lane (csi_hw_s_phy_default_value, csi_hw_s_phy_config) */
    wr(DPHY_SC06, 2U);
    upd(DPHY_SC02, 0xFFU, 1U);
    for (unsigned l = 0; l < 2; ++l) {
        upd(DPHY_SD08(l), 3U << 5, 0);
        upd(DPHY_SD13(l), 1U, 0);
        upd(DPHY_SD05(l), 1U << 2, 1U << 2);
        upd(DPHY_SD06(l), 0xFFU, SETTLE_720);
    }
}

static void phy_off(void)
{
    if (!C.phy_mine)
        return;
    if (C.dom_up && !(C.f_dres & DPHY_RESET_CSIS1))
        upd(DPHY_RESET, DPHY_RESET_CSIS1, 0);
    upd(PMU_DPHY_ISO, 1U, C.f_iso);
    C.phy_mine = 0;
}

static int link_setup(void)
{
    upd(CSIS_CMN, CMN_SW_RESET, CMN_SW_RESET);
    if (!wait_for(CSIS_CMN, CMN_SW_RESET, 0, 1000)) {
        set_why(CAM_EPOWER, "The camera interface did not reset");
        return 0;
    }
    C.link_on = 1;
    wr(CSIS_MSK0, 0x011FFF1FU);
    wr(CSIS_MSK1, 0x0000FFF0U);
    wr(CSIS_SRC0, rd(CSIS_SRC0));
    wr(CSIS_SRC1, rd(CSIS_SRC1));
    upd(CSIS_CMN, (3U << 8) | (3U << 10), (1U << 8) | (3U << 10));       /* two lanes, VC_DT interleave */
    upd(CSIS_PHYCMN, 0xFU << 1, 3U << 1);                                 /* data lanes 0 and 1 */
    upd(CSIS_CFG(0), 0x33FFU, (1U << 12) | (FMT_RAW10 << 2) | 0U);        /* VC0 RAW10, two pixels a clock */
    wr(CSIS_RES(0), (CAM_FRAME_H << 16) | CAM_FRAME_W);
    upd(CSIS_CFG(1), 0x33FFU, (1U << 12) | (FMT_UNKNOWN << 2) | 1U);
    wr(CSIS_RES(1), 0);
    upd(CSIS_CFG(2), 0x33FFU, (1U << 12) | (FMT_EMBEDDED << 2) | 0U);     /* VC0's embedded data: not taken */
    wr(CSIS_RES(2), 0);
    upd(CSIS_CFG(3), 0x33FFU, (1U << 12) | (FMT_UNKNOWN << 2) | 3U);
    wr(CSIS_RES(3), 0);
    /* VC0's WDMA: 2D, RAW10 in 16 bits (PACK12 off), the width; its common: OTF in, RAW10 */
    upd(DMA_FMT, 0x3FU, 0);
    upd(DMA_RESOL, 0x3FFFU, CAM_FRAME_W);
    upd(VC_OTF, 3U, 0);
    upd(VC_DATA, 1U, 0);
    wr(VC_MASK, VC_IRQ_MASK);
    wr(VC_SRC, rd(VC_SRC));
    if (rd(VC_CMN) & 1U)
        upd(VC_CMN, 1U, 0);                                               /* a stale abort request */
    /* the DMA common of the four (csi_hw_s_dma_common), as found kept */
    C.f_cdma[0] = rd(CDMA_ARB), C.f_cdma[1] = rd(CDMA_CTRL), C.f_cdma[2] = rd(CDMA_SPLIT), C.f_cdma[3] = rd(CDMA_MATRIX);
    C.cdma_mine = 1;
    wr(CDMA_ARB, (C.f_cdma[0] & ~0xFU) | (1U << 2) | 2U);
    wr(CDMA_CTRL, C.f_cdma[1] | (1U << 1));
    wr(CDMA_SPLIT, (C.f_cdma[2] & ~0x03FF03FFU) | (0x140U << 16) | 0x140U);
    wr(CDMA_MATRIX, C.f_cdma[3] & ~0x1FU);
    /* the ring: pointer i -> slot i % 4 */
    upd(DMA_CTRL, DMA_PTRS, 0);
    for (unsigned i = 0; i < 8; ++i) {
        C.map[i] = (uint8_t)(i % CAM_RING);
        wr(DMA_ADDR(i), (uint32_t)slot_pa(C.map[i]));
    }
    C.hold = 0;
    wr(VC_SRC, rd(VC_SRC));
    C.dma_on = 1;
    upd(DMA_CTRL, DMA_ON | DMA_UPDT, DMA_ON | DMA_UPDT);
    say("CSIS1 set up: CMN 0x%x PHY 0x%x CH0 0x%x 0x%x; WDMA FMT 0x%x RESOL 0x%x CTRL 0x%x, ring at 0x%llx", rd(CSIS_CMN),
        rd(CSIS_PHYCMN), rd(CSIS_CFG(0)), rd(CSIS_RES(0)), rd(DMA_FMT), rd(DMA_RESOL), rd(DMA_CTRL),
        (unsigned long long)slot_pa(0));
    return 1;
}

static void link_enable(void)
{
    upd(CSIS_CMN, CMN_UPDATE_SHADOW, CMN_UPDATE_SHADOW);
    upd(CSIS_CMN, CMN_PHY_SEL, 0);
    upd(CSIS_PHYCMN, 1U, 1U);                                             /* the clock lane */
    upd(CSIS_CMN, CMN_QCH_EN, 0);
    upd(CSIS_CMN, CMN_CSI_EN, CMN_CSI_EN);
}

static void dma_stop(void)
{
    if (C.dma_on && C.dom_up) {
        upd(DMA_CTRL, DMA_ON | DMA_UPDT, 0);
        /* the frame in flight dropped (DMA_ABORT_REQ, then the request taken back) */
        wr(VC_SRC, DMA_ABORTED);
        upd(VC_CMN, 1U, 1U);
        (void)wait_for(VC_SRC, DMA_ABORTED, DMA_ABORTED, 10000);
        upd(VC_CMN, 1U, 0);
        wr(VC_SRC, DMA_ABORTED);
        if (!wait_for(DMA_ACT, DMA_ON, 0, 20000))
            say("the WDMA still reads active (0x%x)", rd(DMA_ACT));
    }
    C.dma_on = 0;
}

static void link_stop(void)
{
    if (C.link_on && C.dom_up) {
        upd(CSIS_PHYCMN, 0x1FU, 0);
        upd(CSIS_CMN, CMN_CSI_EN | CMN_QCH_EN, CMN_QCH_EN);
        upd(CSIS_CMN, CMN_SW_RESET, CMN_SW_RESET);
        (void)wait_for(CSIS_CMN, CMN_SW_RESET, 0, 1000);
        wr(CSIS_MSK0, 0);
        wr(CSIS_MSK1, 0);
    }
    if (C.cdma_mine && C.dom_up) {
        wr(CDMA_ARB, C.f_cdma[0]);
        wr(CDMA_CTRL, C.f_cdma[1]);
        wr(CDMA_SPLIT, C.f_cdma[2]);
        wr(CDMA_MATRIX, C.f_cdma[3]);
    }
    C.cdma_mine = 0;
    C.link_on = 0;
}

static void stream_stop(void)
{
    if (!C.streaming)
        return;
    int was = C.sensor_bad;
    C.sensor_bad = 0;
    (void)s_wr(0x0104, 0, 1);
    (void)s_wr(0x6028, 0x4000, 2);
    (void)s_wr(0x0100, 0, 1);
    C.sensor_bad = C.sensor_bad || was;
    C.streaming = 0;
    /* the sensor ends its frame (at most 33 ms): wait for the frame count to say 0xFF, 60 ms at the most */
    uint64_t t0 = now_us();
    uint8_t fc = 0;
    while (now_us() - t0 < 60000U) {
        if (s_rd(0x0005, &fc, 1) && fc == 0xFF)
            break;
        spin(2000);
    }
    say("the sensor told to stop streaming; its frame count reads 0x%x", fc);
}

/* everything off in the reverse order; bounded (about 100 ms at the most) */
static void shutdown(void)
{
    int any = C.powered || C.dom_up || C.dom_mine || C.trate_mine || C.tgate_mine || C.phy_mine;
    if (C.dom_up)
        dma_stop();
    stream_stop();
    link_stop();
    phy_off();
    isp_down();
    power_down();
    C.hold = 0;
    if (C.still == 1 || C.still == 2)
        C.still = 0;
    if (any)
        say("everything off: the WDMA, the sensor, the link, the D-PHY, the domain, the supplies, in that order");
}

static void fail(int err, const char *why)
{
    say("FAILED: %s", why);
    set_why(err, why);
    shutdown();
    C.phase = PH_FAILED;
}

/* ---- the stream: frames, the preview, the exposure, the still ----------------------------------------------------- */
static void raw_of(unsigned k, struct cam_raw *r)
{
    r->p = slot_mem(k);
    r->w = CAM_FRAME_W, r->h = CAM_FRAME_H, r->stride = CAM_LINE_BYTES;
    r->fmt = CAM_RAW16, r->bayer = CAM_BAYER_GRBG, r->black = CAM_BLACK;
}

/* the exposure towards 18 % grey on the greens: time first, then analog gain, then digital gain; half steps; every
 * third frame (a change written after frame N shows in frame N + 3) */
static void ae_step(const struct cam_stats *s)
{
    if (!C.req.ae || C.ae_wait) {
        if (C.ae_wait)
            C.ae_wait--;
        return;
    }
    const uint32_t aim = (1023U - CAM_BLACK) * 18U / 100U;
    uint32_t g = (s->mean[1] + s->mean[2]) / 2U;
    g = g > CAM_BLACK ? g - CAM_BLACK + 1U : 1U;
    uint64_t now = (uint64_t)C.lines * C.again * C.dgain / 32U, want;   /* lines at 1x, x 256 */
    int clipped = s->clip * 1000U > 30U * 2U * s->n && g > aim / 2U;
    if (!clipped && g * 100U >= aim * 85U && g * 100U <= aim * 118U)
        return;
    want = clipped ? now / 2U : now * aim / g;
    if (want > now * 8U)
        want = now * 8U;
    if (want < now / 8U)
        want = now / 8U;
    want = (want + now) / 2U;
    uint32_t l = (uint32_t)(want / 256U > EXP_MAX ? EXP_MAX : want / 256U);
    l = l < 2U ? 2U : l;
    uint64_t gl = want * 32U / l;                                       /* again x dgain / 256, in again's units */
    uint32_t a = (uint32_t)(gl / 256U), d;
    a = a < 0x20U ? 0x20U : a > 0x200U ? 0x200U : a;
    d = (uint32_t)(want * 32U / ((uint64_t)l * a));
    d = d < 0x100U ? 0x100U : d > 0x400U ? 0x400U : d;
    if (l == C.lines && a == C.again && d == C.dgain)
        return;                                                         /* at an end of the range */
    (void)exposure_put(l, a, d);
    C.ae_wait = 2;
}

/* ---- v2 (Face Unlock round 2): the pipeline, the metering, white balance from the near-grey pixels ---------------- */
static int v2_off;                          /* cam_set_pipeline(1): v1 */
static struct cam_tune v2_tune;
static int v2_tune_set;
static struct cam_roi v2_roi;
static uint64_t v2_roi_t;
static const struct cam_tune *tune(void)
{
    if (!v2_tune_set)
        cam_tune_default(&v2_tune), v2_tune_set = 1;
    return &v2_tune;
}
void cam_set_pipeline(int v) { v2_off = v == 1; }
void cam_set_roi(const struct cam_roi *roi)
{
    if (roi && roi->valid)
        v2_roi = *roi, v2_roi_t = now_us();
    else
        v2_roi.valid = 0;
}
static const struct cam_roi *roi_now(void)
{
    return v2_roi.valid && now_us() - v2_roi_t < 600000U ? &v2_roi : 0;
}
/* white balance: the near-grey pixels' grey world when there are enough of them (>= 2 %), else every pixel's; the
 * gains within 0.5x..4x and moved a quarter of the way a frame */
static void wb_v2(const struct cam_pstats *p, int first)
{
    const uint64_t *sm = p->gn * 50U >= p->n && p->gn ? p->gsum : p->sum;
    uint32_t r = 256U, b = 256U;
    if (sm[0] > 0 && sm[1] > 0 && sm[2] > 0) {
        uint64_t kr = sm[1] * 256U / sm[0], kb = sm[1] * 256U / sm[2];
        r = (uint32_t)(kr < 128U ? 128U : kr > 1024U ? 1024U : kr);
        b = (uint32_t)(kb < 128U ? 128U : kb > 1024U ? 1024U : kb);
    }
    if (first)
        C.wb.r = r, C.wb.b = b;
    else
        C.wb.r = (3U * C.wb.r + r) / 4U, C.wb.b = (3U * C.wb.b + b) / 4U;
    C.wb.g = 256U;
}
/* the exposure towards the aim: the face's luma at 24 % when the ROI is set (a face was found), else the frame's mean
 * and its middle third's, at 16 %; clipping pulls it down; a whole step when far off, else half a step */
static void ae_v2(const struct cam_pstats *p)
{
    const struct cam_roi *roi = roi_now();
    uint32_t m, aim;
    if (roi && p->rn) {
        m = (uint32_t)(p->rsum / p->rn);
        aim = 983U;                                                     /* 0.24 x 4095 */
    } else {
        uint32_t all = p->n ? (uint32_t)(p->ysum / p->n) : 0, mid = p->cn ? (uint32_t)(p->csum / p->cn) : all;
        m = (all + mid) / 2U;
        aim = 655U;                                                     /* 0.16 x 4095 */
    }
    C.luma = m, C.aim = aim;
    if (!C.req.ae || C.ae_wait) {
        if (C.ae_wait)
            C.ae_wait--;
        return;
    }
    m = m ? m : 1U;
    int clipped = !roi && p->clip * 100U > 3U * p->n && m > aim / 2U;
    if (!clipped && m * 100U >= aim * 80U && m * 100U <= aim * 120U) {
        C.settled++;
        return;
    }
    uint64_t now = (uint64_t)C.lines * C.again * C.dgain / 32U, want;   /* lines at 1x, x 256 */
    want = clipped ? now * 7U / 10U : now * aim / m;
    if (want > now * 8U)
        want = now * 8U;
    if (want < now / 8U)
        want = now / 8U;
    if (!(want > now * 2U || want * 2U < now))
        want = (want + now) / 2U;                                       /* near: half a step (no hunting) */
    uint32_t l = (uint32_t)(want / 256U > EXP_MAX ? EXP_MAX : want / 256U);
    l = l < 2U ? 2U : l;
    uint64_t gl = want * 32U / l;
    uint32_t a = (uint32_t)(gl / 256U), d;
    a = a < 0x20U ? 0x20U : a > 0x200U ? 0x200U : a;
    d = (uint32_t)(want * 32U / ((uint64_t)l * a));
    d = d < 0x100U ? 0x100U : d > 0x400U ? 0x400U : d;
    if (l == C.lines && a == C.again && d == C.dgain) {
        C.settled++;                                                    /* at an end of the range: as good as it gets */
        return;
    }
    C.settled = 0;
    (void)exposure_put(l, a, d);
    C.ae_wait = 2;
}
void cam_ae_info(struct cam_ae_info *i)
{
    i->lines = C.lines, i->again = C.again, i->dgain = C.dgain;
    i->luma = C.luma, i->settled = C.settled >= 2U, i->wb_r = C.wb.r, i->wb_b = C.wb.b, i->frames = C.frames;
}
/* the newest frame's raw pixels (copied within the poll's turn: the slot is armed again for the WDMA, ~4 frames) */
int cam_raw_crop(uint16_t *dst, unsigned x0, unsigned y0, unsigned w, unsigned h)
{
    if (C.phase != PH_RUN || C.last_slot < 0 || !C.frames || (x0 | y0) & 1U || x0 + w > CAM_FRAME_W || y0 + h > CAM_FRAME_H)
        return CAM_ESTATE;
    volatile uint8_t *m = slot_mem((unsigned)C.last_slot);
    for (unsigned y = 0; y < h; ++y) {
        const volatile uint32_t *s = (const volatile uint32_t *)(m + (size_t)(y0 + y) * CAM_LINE_BYTES) + x0 / 2U;
        for (unsigned x = 0; x < w; x += 2U) {
            uint32_t v = s[x / 2U];
            dst[(size_t)y * w + x] = (uint16_t)v;
            if (x + 1U < w)
                dst[(size_t)y * w + x + 1U] = (uint16_t)(v >> 16);
        }
    }
    return 0;
}
int cam_render_again(uint32_t *px, int v1)
{
    if (C.phase != PH_RUN || C.last_slot < 0 || !C.frames || !px)
        return CAM_ESTATE;
    struct cam_raw r;
    raw_of((unsigned)C.last_slot, &r);
    struct cam_out o = { px, C.req.w, C.req.h, C.req.w, C.req.xform };
    if (v1)
        cam_isp_render(&r, &C.wb, &o, 0, C.req.h);
    else
        cam_isp_process(&r, &C.wb, tune(), &o, 0, 0, cam_isp_rows(&o), 0);
    return 0;
}

static void hold_slot(unsigned k)
{
    slot_arm(CAM_RING);
    for (unsigned i = 0; i < 8; ++i)
        if (C.map[i] == k) {
            C.map[i] = (uint8_t)CAM_RING;
            wr(DMA_ADDR(i), (uint32_t)slot_pa(CAM_RING));
        }
    C.hold = 1, C.held = k;
}

static void release_slot(void)
{
    if (!C.hold)
        return;
    slot_arm(C.held);
    if (C.dom_up)
        for (unsigned i = 0; i < 8; ++i)
            if (C.map[i] == CAM_RING) {
                C.map[i] = (uint8_t)C.held;
                wr(DMA_ADDR(i), (uint32_t)slot_pa(C.held));
            }
    C.hold = 0;
}

static int slot_in_ring(unsigned k)
{
    for (unsigned i = 0; i < 8; ++i)
        if (C.map[i] == k)
            return 1;
    return 0;
}

/* the newest finished frame: a slot whose last 8 bytes are no longer the fill (the WDMA writes them last); with more
 * than one, the one right behind the WDMA's current pointer; the others are dropped (re-armed) */
static int newest_slot(void)
{
    int done[CAM_RING + 1], nd = 0, pick = -1;
    for (unsigned k = 0; k <= CAM_RING; ++k) {
        done[k] = 0;
        if ((C.hold && k == C.held) || !slot_in_ring(k))
            continue;
        if (slot_done(k))
            done[k] = 1, nd++, pick = (int)k;
    }
    if (nd > 1) {
        unsigned cur = (rd(DMA_ACT) >> 2) & 7U;
        for (unsigned d = 1; d <= 8; ++d) {
            unsigned k = C.map[(cur + 8U - d) & 7U];
            if (done[k]) {
                pick = (int)k;
                break;
            }
        }
        for (unsigned k = 0; k <= CAM_RING; ++k)
            if (done[k] && (int)k != pick)
                slot_arm(k);
    }
    return pick;
}

static void still_work(void)
{
    struct cam_raw r;
    raw_of(C.held, &r);
    struct cam_out o = { (uint32_t *)(uintptr_t)(C.mem + CAM_STILLPX_OFF), C.sw, C.sh, C.sw, C.req.xform & 3U };
    unsigned rows = v2_off ? C.sh : cam_isp_rows(&o);
    unsigned y1 = C.still_row + (v2_off ? 102U : 64U);
    if (v2_off)
        cam_isp_render(&r, &C.wb, &o, C.still_row, y1);
    else
        cam_isp_process(&r, &C.wb, tune(), &o, 0, C.still_row, y1, 0);
    C.still_row = y1;
    if (C.still_row < rows)
        return;
    C.bmp_len = cam_isp_bmp(o.px, C.sw, C.sh, C.sw, (uint8_t *)(uintptr_t)(C.mem + CAM_BMP_OFF), CAM_PIC_BYTES);
    release_slot();
    C.still = 3;
    say("still ready: %u x %u, a %u-byte BMP at 0x%llx", C.sw, C.sh, C.bmp_len,
        (unsigned long long)(CAM_MEM_PA + CAM_BMP_OFF));
}

static void run_poll(void)
{
    uint64_t t = now_us();
    uint32_t src = rd(VC_SRC);
    if (src & DMA_ERROR) {
        wr(VC_SRC, DMA_ERROR);
        if (C.dma_err++ < 4)
            say("the WDMA reports an error (code 0x%x, CSIS1 0x%x)", rd(VC_ERR), rd(CSIS_SRC0));
    }
    if (src & (DMA_FS | DMA_FE))
        wr(VC_SRC, src & (DMA_FS | DMA_FE));
    if (C.still == 2) {
        still_work();
        return;                                                         /* one heavy job a call */
    }
    int k = newest_slot();
    if (k < 0) {
        if (t - C.t_frame > 1500000U)
            fail(CAM_EFRAME, C.frames ? "The camera stopped sending pictures" : "No picture from the camera");
        return;
    }
    C.t_frame = t;
    if (!C.frames)
        C.t_first = t;
    C.frames++, C.fps_frames++;
    if (t - C.t_fps >= 2000000U) {
        C.fps10 = (uint32_t)((uint64_t)C.fps_frames * 10000000U / (t - C.t_fps));
        C.t_fps = t, C.fps_frames = 0;
        say("streaming: %u.%u frames a second, %u frames, exposure %u lines gain 0x%x digital 0x%x, %u DMA errors; "
            "pipeline v%u %u ms (max %u); luma %u aim %u; wb r %u b %u",
            C.fps10 / 10U, C.fps10 % 10U, C.frames, C.lines, C.again, C.dgain, C.dma_err, v2_off ? 1U : 2U,
            C.isp_n ? C.isp_us / C.isp_n / 1000U : 0, C.isp_max / 1000U, C.luma, C.aim, C.wb.r, C.wb.b);
        C.isp_us = C.isp_max = C.isp_n = 0;
    }
    struct cam_raw r;
    struct cam_stats s;
    raw_of((unsigned)k, &r);
    cam_isp_stats(&r, 16, &s);
    if (v2_off) {
        struct cam_wb w;
        cam_isp_wb_grey(&s, CAM_BLACK, &w);
        if (C.frames == 1)
            C.wb = w;
        else
            C.wb.r = (3U * C.wb.r + w.r) / 4U, C.wb.b = (3U * C.wb.b + w.b) / 4U, C.wb.g = 256U;
    }
    C.last = s;
    C.last_slot = k;
    if (C.still == 1 && C.frames > 3U) {
        C.still = 2, C.still_row = 0;
        C.sw = (C.req.xform & 1U) ? CELLS_H : CELLS_W;
        C.sh = (C.req.xform & 1U) ? CELLS_W : CELLS_H;
        hold_slot((unsigned)k);
        say("still: frame %u held (slot %d), rendering %u x %u", C.frames, k, C.sw, C.sh);
    } else {
        struct cam_out o = { (uint32_t *)(uintptr_t)(C.mem + CAM_PREVIEW_OFF), C.req.w, C.req.h, C.req.w, C.req.xform };
        uint64_t ti = now_us();
        if (v2_off) {
            cam_isp_render(&r, &C.wb, &o, 0, C.req.h);
        } else {
            struct cam_pstats p;
            for (unsigned i = 0; i < sizeof p; ++i)
                ((uint8_t *)&p)[i] = 0;
            cam_isp_process(&r, &C.wb, tune(), &o, roi_now(), 0, cam_isp_rows(&o), &p);
            C.ps = p;
            wb_v2(&p, C.frames == 1);
        }
        uint32_t dt = (uint32_t)(now_us() - ti);
        C.isp_us += dt, C.isp_n++;
        if (dt > C.isp_max)
            C.isp_max = dt;
        C.seq++;
        C.have_pic = 1;
        slot_arm((unsigned)k);
    }
    if (v2_off)
        ae_step(&s);
    else if (C.ps.n)
        ae_v2(&C.ps);
}

/* ---- the steps ---------------------------------------------------------------------------------------------------- */
static int step(unsigned ms)
{
    C.t_phase = now_us();
    return (int)ms;
}

/* one step of the bring-up / the stream: the ms until the next call wants to be made */
static int poll_once(void)
{
    uint64_t t = now_us();
    if (C.phase > PH_LOOK && C.phase < PH_RUN && t - C.t_open > 4000000U) {
        fail(CAM_ESTATE, "The camera took too long to start");
        return 0;
    }
    switch (C.phase) {
    case PH_LOOK:
        C.mem = (volatile uint8_t *)H.ram(H.ctx, CAM_MEM_PA, CAM_MEM_SIZE);
        if (!C.mem) {
            fail(CAM_EMEM, "The camera's memory is not available");
            return 0;
        }
        if ((rd(PMU_DPU_STAT) & PD_ON) != PD_ON) {
            fail(CAM_EPOWER, "The camera's power domain cannot come on (the display's is off)");
            return 0;
        }
        if (!bus_up(&pbus, 1)) {
            fail(CAM_EBUS, "The camera's power chip's bus is not available");
            return 0;
        }
        if (!rails_look(0)) {
            fail(C.err ? C.err : CAM_EPMIC, C.why);
            return 0;
        }
        C.phase = PH_POWER;
        return step(0);
    case PH_POWER:
        if (!power_up()) {
            fail(C.err, C.why);
            return 0;
        }
        C.phase = PH_POWER2;
        return step(1);
    case PH_POWER2:
        if (t - C.t_phase < 1000U)
            return 1;
        if (!power_up2()) {
            fail(C.err, C.why);
            return 0;
        }
        C.phase = PH_ID, C.id_tries = 0;
        return step(10);                    /* MCLK 9 ms before the first access */
    case PH_ID: {
        if (t - C.t_phase < (C.id_tries ? 20000U : 9000U))
            return 1;
        uint8_t id[2] = { 0, 0 }, rev = 0, fc = 0;
        if (!s_rd(0x0000, id, 2) || (!id[0] && !id[1]) || (id[0] == 0xFF && id[1] == 0xFF)) {
            if (++C.id_tries < 2) {
                say("the sensor does not answer at 0x35 yet: 20 ms more");
                return step(20);            /* (the wait above: 20 ms on a retry) */
            }
            fail(CAM_ESENSOR, "The front camera does not answer");
            return 0;
        }
        (void)s_rd(0x0002, &rev, 1);
        (void)s_rd(0x0005, &fc, 1);
        C.model = (uint32_t)id[0] << 8 | id[1], C.rev = rev;
        say("THE SENSOR ANSWERS at 0x35: model ID 0x%04x, revision 0x%02x, frame count 0x%02x%s", C.model, rev, fc,
            id[0] == 0x30 || id[0] == 0x31 ? " (a Samsung S5K3 series ID)" : " (not the usual S5K3 range: logged)");
        C.phase = PH_ISP;
        return step(0);
    }
    case PH_ISP:
        if (!isp_up()) {
            fail(C.err, C.why);
            return 0;
        }
        C.phase = PH_FILL, C.fill_k = 0;
        return step(0);
    case PH_FILL: {
        /* a slot a call: the fill (no RAW10 value), so that the markers and what the WDMA wrote show */
        volatile uint64_t *p = (volatile uint64_t *)slot_mem(C.fill_k);
        for (uint32_t i = 0; i < ring_bytes / 8U; ++i)
            p[i] = FILL64;
        if (++C.fill_k <= CAM_RING)
            return step(0);
        C.phase = PH_LINK;
        return step(0);
    }
    case PH_LINK:
        phy_on();
        if (!link_setup()) {
            fail(C.err, C.why);
            return 0;
        }
        C.phase = PH_MODE;
        return step(0);
    case PH_MODE:
        for (unsigned i = 0; i < MODE_N; ++i)
            if (!s_wr(mode_a[i][0], mode_a[i][1], 2)) {
                fail(CAM_ESENSOR, "The front camera did not take its settings");
                return 0;
            }
        if (!s_wr(0x3092, 0, 2) || !s_wr(0x6028, 0x2000, 2)) {
            fail(CAM_ESENSOR, "The front camera did not take its settings");
            return 0;
        }
        {
            uint32_t l = C.req.exp_lines ? C.req.exp_lines : 1000U, a = C.req.again ? C.req.again : 0x40U;
            l = l < 2U ? 2U : l > EXP_MAX ? EXP_MAX : l;
            a = a < 0x20U ? 0x20U : a > 0x200U ? 0x200U : a;
            if (!exposure_put(l, a, 0x100U)) {
                fail(CAM_ESENSOR, "The front camera did not take its exposure");
                return 0;
            }
        }
        {
            uint8_t w[2] = { 0, 0 }, h[2] = { 0, 0 };
            (void)s_rd(0x034C, w, 2);
            (void)s_rd(0x034E, h, 2);
            say("mode: %u writes of setfile A; the sensor reads back %u x %u; exposure %u lines, gain 0x%x",
                (unsigned)MODE_N, (unsigned)(w[0] << 8 | w[1]), (unsigned)(h[0] << 8 | h[1]), C.lines, C.again);
        }
        link_enable();
        if (!s_wr(0x0104, 0, 1)) {
            fail(CAM_ESENSOR, "The front camera did not start");
            return 0;
        }
        C.phase = PH_STREAM;
        return step(8);
    case PH_STREAM:
        if (t - C.t_phase < 8000U)
            return 1;
        C.streaming = 1;
        if (!s_wr(0x6028, 0x4000, 2) || !s_wr(0x0100, 1, 1) || !s_wr(0x0216, 0, 1)) {
            fail(CAM_ESENSOR, "The front camera did not start");
            return 0;
        }
        say("STREAM ON (0x0100 = 1)");
        C.phase = PH_RUN;
        C.t_frame = C.t_fps = C.t_stream = t;
        set_why(0, "Front camera on");
        return step(5);
    case PH_RUN:
        run_poll();
        return C.phase == PH_RUN ? 5 : 0;
    default:
        return 0;
    }
}

/* ---- the interface ------------------------------------------------------------------------------------------------ */
#ifdef SBOS_KERNEL
static void auto_hal(void)
{
    if (!hal_ok) {
        struct cam_hal h;
        cam_hal_libsb(&h);
        cam_hal_set(&h);
    }
}
#else
static void auto_hal(void)
{
}
#endif

void cam_hal_set(const struct cam_hal *h)
{
    H = *h;
    hal_ok = h->rd32 && h->wr32 && h->i2c_init && h->i2c && h->smc && h->delay_us && h->now_us && h->log && h->ram;
}

static void reset_state(void)
{
    struct cam_req keep = C.req;
    uint8_t *z = (uint8_t *)&C;
    for (size_t i = 0; i < sizeof(C); ++i)
        z[i] = 0;
    C.req = keep;
    z = (uint8_t *)&P;
    for (size_t i = 0; i < sizeof(P); ++i)
        z[i] = 0;
    reset_pin.saved = mclk_pin.saved = 0;
    sbus.mine = sbus.up = 0;
    faults = refused = 0;
    C.last_slot = -1;
}

int cam_open(const struct cam_req *req)
{
    auto_hal();
    if (!hal_ok)
        return CAM_ESTATE;
    if (C.phase != PH_OFF && C.phase != PH_FAILED)
        return CAM_ESTATE;
    if (!req || req->facing != CAM_FRONT || req->w < 8U || req->h < 8U || req->w > 816U || req->h > 816U ||
        req->w * req->h * 4U > CAM_PIC_BYTES || req->xform > 7U) {
        set_why(CAM_EARG, "That camera or picture size is not available");
        return CAM_EARG;
    }
    C.req = *req;
    reset_state();
    C.t_open = C.t_phase = now_us();
    C.phase = PH_LOOK;
    C.lines = 0, C.again = 0x20, C.dgain = 0x100;
    C.wb.r = C.wb.g = C.wb.b = 256U;
    set_why(0, "Starting the front camera");
    say("open: the front camera, preview %u x %u (orientation %u)", req->w, req->h, req->xform);
    return 0;
}

int cam_poll(void)
{
    if (!hal_ok || C.phase == PH_OFF)
        return 0;
    if (C.phase == PH_FAILED)
        return C.err ? C.err : CAM_ESTATE;
    (void)poll_once();
    if (C.phase == PH_FAILED)
        return C.err ? C.err : CAM_ESTATE;
    return 1;
}

/* the ms until the step machine wants its next call (the probe uses it) */
static unsigned poll_ms(void)
{
    int ms = poll_once();
    return ms < 0 ? 0U : (unsigned)ms;
}

int cam_frame(struct cam_pic *p)
{
    if (!C.have_pic || C.phase != PH_RUN || !p)
        return 0;
    p->px = (const uint32_t *)(uintptr_t)(C.mem + CAM_PREVIEW_OFF);
    p->w = C.req.w, p->h = C.req.h, p->stride = C.req.w, p->seq = C.seq;
    return 1;
}

int cam_still(void)
{
    if (C.phase < PH_LOOK || C.phase > PH_RUN || C.still == 1 || C.still == 2)
        return CAM_ESTATE;
    C.still = 1;
    return 0;
}

int cam_still_get(struct cam_shot *s)
{
    if (C.still != 3 || !s || !C.mem)
        return 0;
    s->bmp = (const uint8_t *)(uintptr_t)(C.mem + CAM_BMP_OFF);
    s->bmp_len = C.bmp_len;
    s->px = (const uint32_t *)(uintptr_t)(C.mem + CAM_STILLPX_OFF);
    s->w = C.sw, s->h = C.sh, s->stride = C.sw;
    return 1;
}

void cam_close(void)
{
    if (!hal_ok || C.phase == PH_OFF)
        return;
    if (C.phase != PH_FAILED) {
        say("close");
        shutdown();
    }
    C.phase = PH_OFF;
    C.have_pic = 0;
    if (C.still != 3)
        C.still = 0;
    set_why(0, "Camera off");
}

void cam_abort(void)
{
    if (!hal_ok || C.phase == PH_OFF)
        return;
    if (C.phase != PH_FAILED) {
        say("abort");
        shutdown();
    }
    C.phase = PH_OFF;
    C.have_pic = 0;
    C.still = 0;
    set_why(CAM_EABORT, "Camera stopped");
}

const char *cam_why(void)
{
    return C.why ? C.why : "Camera off";
}

int cam_state(void)
{
    switch (C.phase) {
    case PH_OFF:
        return CAM_ST_OFF;
    case PH_RUN:
        return CAM_ST_RUNNING;
    case PH_FAILED:
        return CAM_ST_FAILED;
    default:
        return CAM_ST_STARTING;
    }
}

uint32_t cam_fps_x10(void)
{
    return C.fps10;
}

uint32_t cam_model_id(void)
{
    return C.model;
}

int cam_flash_available(void)
{
    return 0;
}

/* ---- the probe ---------------------------------------------------------------------------------------------------- */
/* what the camera's run may change, read back before and after (all outside the ISPPRE domain, safe to read off) */
static const struct snapreg {
    const char *name;
    uint64_t pa;
    uint32_t mask;
} snapregs[] = {
    { "reset gpg2-5 CON", GPG2, 0xFU << 20 }, { "reset gpg2-5 DAT", GPG2 + 4, 1U << 5 },
    { "reset gpg2-5 PUD", GPG2 + 8, 0xFU << 20 }, { "MCLK gpc1-5 CON", GPC1, 0xFU << 20 },
    { "MCLK gpc1-5 DAT", GPC1 + 4, 1U << 5 }, { "MCLK gpc1-5 PUD", GPC1 + 8, 0xFU << 20 },
    { "bus gpp5-2/3 CON", GPP5, 0xFF00U }, { "bus gpp5-2/3 PUD", GPP5 + 8, 0xFF00U },
    { "USI08_I2C mode", 0x1081102CULL, 0xFFFFFFFFU },
    { "USI08_I2C gate 0x2050", CMU_PERIC1 + 0x2050, 0xFFFFFFFFU },
    { "USI08_I2C gate 0x2098", CMU_PERIC1 + 0x2098, 0xFFFFFFFFU },
    { "USI08_I2C gate 0x20EC", CMU_PERIC1 + 0x20EC, 0xFFFFFFFFU },
    { "USI08_I2C gate 0x20F0", CMU_PERIC1 + 0x20F0, 0xFFFFFFFFU },
    { "USI08_I2C Q-channel", CMU_PERIC1 + 0x303C, 3U },
    { "CIS_CLK1 mux", CIS1_MUX, 0xFFFFFFFFU }, { "CIS_CLK1 divider", CIS1_DIV, 0xFFFFFFFFU },
    { "CIS_CLK1 gate", CIS1_GATE, 0xFFFFFFFFU }, { "CIS_CLK1 Q-channel", CIS1_QCH, 3U },
    { "ISPPRE_BUS mux", ISPB_MUX, 3U }, { "ISPPRE_BUS divider", ISPB_DIV, 0xFU },
    { "ISPPRE_BUS gate", ISPB_GATE, GATE_ON },
    { "ISPPRE configuration", PMU_ISP_CONF, PD_ON }, { "ISPPRE status", PMU_ISP_STAT, PD_ON },
    { "MEMORY_ISPPRE_OPTION", PMU_ISP_MEM, 3U }, { "D-PHY isolation", PMU_DPHY_ISO, 1U },
};
#define NSNAP (sizeof(snapregs) / sizeof(snapregs[0]))

struct snap {
    uint32_t v[NSNAP];
    int pmic;                               /* the S2MPB02's registers were read */
    uint8_t b1, b2, ldo[4];
};

static void snap_take(struct snap *s, int read_pmic)
{
    for (unsigned i = 0; i < NSNAP; ++i)
        s->v[i] = rd(snapregs[i].pa) & snapregs[i].mask;
    s->pmic = 0;
    if (read_pmic && pmic_rd(B2CTRL1, &s->b1) && pmic_rd(B2CTRL2, &s->b2)) {
        s->pmic = 1;
        for (unsigned i = 0; i < 4; ++i)
            s->pmic = s->pmic && pmic_rd(ldos[i].reg, &s->ldo[i]);
    }
}

/* 0 the same; else logs each difference */
static unsigned snap_diff(const struct snap *a, const struct snap *b)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NSNAP; ++i)
        if (a->v[i] != b->v[i]) {
            n++;
            psay("not put back: %s 0x%x, found 0x%x", snapregs[i].name, b->v[i], a->v[i]);
        }
    if (a->pmic && b->pmic) {
        if (a->b1 != b->b1 || a->b2 != b->b2) {
            n++;
            psay("not put back: BUCK2 0x%02x 0x%02x, found 0x%02x 0x%02x", b->b1, b->b2, a->b1, a->b2);
        }
        for (unsigned i = 0; i < 4; ++i)
            if (a->ldo[i] != b->ldo[i]) {
                n++;
                psay("not put back: %s 0x%02x, found 0x%02x", ldos[i].name, b->ldo[i], a->ldo[i]);
            }
    }
    return n;
}

/* level 0: reads only; the S2MPB02 only if its bus is already up (the torch driver's) */
static int look(char *summary_ok)
{
    unsigned f0 = faults;
    uint32_t gc = rd(GPG2), gd = rd(GPG2 + 4), gp = rd(GPG2 + 8), mc = rd(GPC1), md = rd(GPC1 + 4), mp = rd(GPC1 + 8);
    psay("level 0: reset gpg2-5 CON %x DAT %u PUD %x; MCLK gpc1-5 CON %x DAT %u PUD %x; sensor bus gpp5-2/3 CON %02x "
         "PUD %02x", (gc >> 20) & 0xFU, (gd >> 5) & 1U, (gp >> 20) & 0xFU, (mc >> 20) & 0xFU, (md >> 5) & 1U,
         (mp >> 20) & 0xFU, (rd(GPP5) >> 8) & 0xFFU, (rd(GPP5 + 8) >> 8) & 0xFFU);
    psay("level 0: USI08_I2C mode 0x%x, gates 0x%x 0x%x 0x%x 0x%x, Q-channel 0x%x, divider 0x%x; USI10 mode 0x%x, "
         "gpp6 CON %02x", rd(0x1081102CULL), rd(CMU_PERIC1 + 0x2050), rd(CMU_PERIC1 + 0x2098), rd(CMU_PERIC1 + 0x20EC),
         rd(CMU_PERIC1 + 0x20F0), rd(CMU_PERIC1 + 0x303C), rd(CMU_PERIC1 + 0x1830), rd(0x1081103CULL),
         rd(GPP6) & 0xFFU);
    psay("level 0: CMU_TOP CIS_CLK1 mux 0x%x div 0x%x gate 0x%x qch 0x%x; CLKCMU_ISPPRE_BUS mux 0x%x div 0x%x gate 0x%x",
         rd(CIS1_MUX), rd(CIS1_DIV), rd(CIS1_GATE), rd(CIS1_QCH), rd(ISPB_MUX), rd(ISPB_DIV), rd(ISPB_GATE));
    uint32_t dpu = rd(PMU_DPU_STAT), st = rd(PMU_ISP_STAT);
    psay("level 0: PMU DPU status 0x%x (the parent: %s), ISPPRE configuration 0x%x status 0x%x (%s), "
         "MEMORY_ISPPRE_OPTION 0x%x, D-PHY isolation 0x%x", dpu, (dpu & PD_ON) == PD_ON ? "on" : "OFF",
         rd(PMU_ISP_CONF), st, (st & PD_ON) == PD_ON ? "on" : "off", rd(PMU_ISP_MEM), rd(PMU_DPHY_ISO));
    if ((st & PD_ON) == PD_ON)
        psay("level 0: ISPPRE is on: CSIS1 version 0x%x", rd(CSIS_VERSION));
    else
        psay("level 0: ISPPRE is off: CSIS1 not read (its block is not powered; nothing switched on here)");
    int pm = 0;
    if (bus_ready(&pbus)) {
        uint32_t fs1 = rd(pbus.base + 0x60), fs2 = rd(pbus.base + 0x64), fs3 = rd(pbus.base + 0x68);
        H.i2c_init(H.ctx, CAM_BUS_PMIC, fs1, fs2, fs3, 0);   /* only remembered: nothing written */
        pm = rails_look(1);
    } else {
        psay("level 0: the S2MPB02's bus is not set up yet (USI10 mode 0x%x): its LDO registers are read at level 1",
             rd(0x1081103CULL));
    }
    const char *w = (dpu & PD_ON) != PD_ON ? "the display's domain is OFF (the camera cannot start)" :
                    (st & PD_ON) == PD_ON  ? "ISPPRE on (unexpected), DPU on" :
                    pm                     ? "ISPPRE off, DPU on, the camera's supplies read" :
                                             "ISPPRE off, DPU on, supplies not read (bus not up)";
    unsigned i = 0;
    while (w[i] && i < 99) {
        summary_ok[i] = w[i];
        i++;
    }
    summary_ok[i] = 0;
    return faults == f0;
}

static struct {
    int level, phase, open_done, timeout;
    uint64_t t0, t1;
    struct snap a, b;
    uint32_t last_seq;
    unsigned shown;
} Q;

static unsigned count_written(unsigned k)
{
    const volatile uint64_t *p = (const volatile uint64_t *)slot_mem(k);
    unsigned n = 0, words = (unsigned)((uint32_t)CAM_FRAME_H * CAM_LINE_BYTES / 8U);
    for (unsigned i = 0; i < words; ++i)
        n += p[i] != FILL64;
    return (unsigned)((uint64_t)n * 1000U / words);       /* per mille */
}

static int probe_end(int pass, const char *what)
{
    if (pass)
        psay("PASS %s", what);
    else
        psay("FAIL %s", what);
    Q.phase = 0;
    return 1;
}

static void stats_line(const char *what, const struct cam_stats *s)
{
    psay("%s: R %u Gr %u Gb %u B %u (10-bit, black 64), min %u max %u, green sd %u, %u above 10 bits, %u cells", what,
         s->mean[0], s->mean[1], s->mean[2], s->mean[3], s->min, s->max, s->sd_g, s->over, s->n);
}

int drv_cam_probe(int level, unsigned *wait_ms)
{
    static char sum[100];
    auto_hal();
    *wait_ms = 0;
    if (!hal_ok) {
        return 1;
    }
    if (Q.phase == 0 || Q.level != level) {
        Q.level = level, Q.phase = 1, Q.t0 = now_us(), Q.open_done = 0, Q.shown = 0, Q.last_seq = 0, Q.timeout = 0;
    }
    uint64_t t = now_us();
    if (level <= 0) {
        int ok = look(sum);
        char w[160];
        unsigned n = 0;
        const char *a = ok ? "read-only look: " : "a read faulted in the read-only look: ";
        while (*a && n < 60)
            w[n++] = *a++;
        for (const char *b = sum; *b && n < 158; ++b)
            w[n++] = *b;
        w[n] = 0;
        return probe_end(ok, w);
    }
    unsigned limit = level == 1 ? 6000000U : 19000000U;
    if (Q.phase == 2 && t - Q.t0 > limit) {
        psay("level %d: out of time (%u s): everything switched off", level, limit / 1000000U);
        cam_abort();
        Q.timeout = 1, Q.phase = 3;
    }
    switch (Q.phase) {
    case 1: {                               /* the state before, then open */
        if (C.phase != PH_OFF && C.phase != PH_FAILED) {
            return probe_end(0, "the camera is in use: not probed");
        }
        int pm = bus_up(&pbus, 1);
        snap_take(&Q.a, pm);
        psay("level %d: the state before: %u registers%s", level, (unsigned)NSNAP,
             Q.a.pmic ? " and the S2MPB02's six" : " (the S2MPB02 not readable)");
        struct cam_req r = { CAM_FRONT, level == 1 ? 64U : 360U, level == 1 ? 48U : 480U,
                             level == 1 ? CAM_ROT0 : (unsigned)CAM_PORTRAIT_SELFIE, 1, 0, 0 };
        if (cam_open(&r) != 0)
            return probe_end(0, cam_why());
        Q.phase = 2, Q.t1 = t;
        return 0;
    }
    case 2: {                               /* streaming: level 1 waits for 4 frames, level 2 shows 10 s */
        unsigned ms = poll_ms();
        if (C.phase == PH_FAILED) {
            Q.phase = 3;
            *wait_ms = 0;
            return 0;
        }
        if (C.phase == PH_RUN && level >= 2) {
            struct cam_pic p;
            if (cam_frame(&p) && p.seq != Q.last_seq) {
                Q.last_seq = p.seq;
                Q.shown++;
                if (H.show)
                    H.show(H.ctx, p.px, p.w, p.h, p.stride);
            }
            if (!Q.open_done && C.frames) {
                Q.open_done = 1, Q.t1 = t;
                psay("level 2: the preview runs (360 x 480, a selfie mirror): 10 s on the screen");
            }
            if (Q.open_done && t - Q.t1 >= 10000000U && C.still == 0) {
                (void)cam_still();
                psay("level 2: a still asked after %u pictures shown", Q.shown);
            }
            if (C.still == 3)
                Q.phase = 3;
        } else if (C.phase == PH_RUN && C.frames >= 4U) {
            Q.phase = 3;
        }
        *wait_ms = ms > 10U ? 10U : ms;
        return 0;
    }
    case 3: {                               /* the result, everything off, the state after */
        int ok = C.phase != PH_FAILED && !Q.timeout;
        const char *why = Q.timeout ? (level == 1 ? "no frame within 6 s" : "the preview did not finish in time") :
                                      cam_why();
        unsigned frames = C.frames, fps10 = C.fps10, errs = C.dma_err, slot = (unsigned)C.last_slot;
        if (!fps10 && frames > 1U && C.t_frame > C.t_first)  /* no 2 s window yet: over the frames seen */
            fps10 = (unsigned)((uint64_t)(frames - 1U) * 10000000U / (C.t_frame - C.t_first));
        uint32_t model = C.model;
        struct cam_stats s;
        struct cam_shot sh;
        int have_still = level >= 2 && cam_still_get(&sh);
        uint32_t still_mean[3] = { 0, 0, 0 };
        if (have_still) {
            uint64_t m[3] = { 0, 0, 0 };
            for (unsigned y = 0; y < sh.h; y += 4)
                for (unsigned x = 0; x < sh.w; x += 4) {
                    uint32_t p = sh.px[(size_t)y * sh.stride + x];
                    m[0] += (p >> 16) & 0xFFU, m[1] += (p >> 8) & 0xFFU, m[2] += p & 0xFFU;
                }
            uint32_t n = ((sh.h + 3U) / 4U) * ((sh.w + 3U) / 4U);
            for (unsigned i = 0; i < 3; ++i)
                still_mean[i] = (uint32_t)(m[i] / n);
        }
        cam_close();
        snap_take(&Q.b, Q.a.pmic);
        unsigned diff = snap_diff(&Q.a, &Q.b);
        if (refused) {
            psay("%u writes outside the camera's blocks were refused (a bug)", refused);
            ok = 0;
        }
        if (model)
            psay("the sensor's model ID 0x%04x", model);
        if (ok && slot <= CAM_RING) {
            struct cam_raw r;
            raw_of(slot, &r);
            cam_isp_stats(&r, 4, &s);
            unsigned wpm = count_written(slot);
            stats_line("the last frame (slot statistics, every 4th cell)", &s);
            psay("the last frame: %u.%u%% of it written by the DMA; %u frames, %u.%u frames a second, %u DMA errors",
                 wpm / 10U, wpm % 10U, frames, fps10 / 10U, fps10 % 10U, errs);
            if (wpm < 950U)
                ok = 0, why = "the frame was not written whole";
            else if (s.over > s.n / 1000U)
                ok = 0, why = "the frame holds values that are not RAW10";
            else if (s.max == s.min)
                ok = 0, why = "the frame is one flat value";
        } else if (ok) {
            ok = 0, why = "no frame was kept";
        }
        if (have_still)
            psay("the still: %u x %u, BMP %u bytes, mean R %u G %u B %u (of 255)", sh.w, sh.h, sh.bmp_len,
                 still_mean[0], still_mean[1], still_mean[2]);
        else if (level >= 2 && ok)
            ok = 0, why = "no still";
        if (diff)
            ok = 0, why = "something was not put back as found";
        if (ok)
            return probe_end(1, level == 1 ? "front camera: ID read, frames reached memory, everything put back" :
                                             "front camera: preview shown 10 s, still taken, everything put back");
        return probe_end(0, why);
    }
    default:
        return probe_end(0, "lost its place");
    }
}

void drv_cam_abort(void)
{
    cam_abort();
    Q.phase = 0;
}

/* ---- libsb glue --------------------------------------------------------------------------------------------------- */
#ifdef SBOS_KERNEL
static struct sb_i2c kbus[2] = { { 0x10920000UL, 0, 0, 0, 0 }, { 0x108F0000UL, 0, 0, 0, 0 } };

static int k_rd32(void *c, uint64_t pa, uint32_t *v)
{
    (void)c;
    return sb_probe32(pa, v);
}

static void k_wr32(void *c, uint64_t pa, uint32_t v)
{
    (void)c;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)pa = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void k_i2c_init(void *c, unsigned bus, uint32_t fs1, uint32_t fs2, uint32_t fs3, int reset)
{
    (void)c;
    if (bus > 1)
        return;
    kbus[bus].fs1 = fs1, kbus[bus].fs2 = fs2, kbus[bus].fs3 = fs3;
    if (reset)
        sb_i2c_bus(&kbus[bus]);
}

static int k_i2c(void *c, unsigned bus, unsigned addr, int rx, uint8_t *b, unsigned n, int stop)
{
    (void)c;
    return bus <= 1 && sb_i2c_msg(&kbus[bus], addr, rx, b, n, stop);
}

static int64_t k_smc(void *c, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)c;
    return sb_psci(fn, a1, a2, a3);
}

static void k_delay(void *c, uint32_t us)
{
    (void)c;
    sb_udelay(us);
}

static uint64_t k_now(void *c)
{
    (void)c;
    return sb_us();
}

static void k_log(void *c, const char *s)
{
    (void)c;
    sb_log(s);
}

static void *k_ram(void *c, uint64_t pa, uint64_t len)
{
    (void)c;
    if (!len || sb_map_kind(pa) != 3 || sb_map_kind(pa + len - 1U) != 3)
        return 0;
    return (void *)(uintptr_t)pa;
}

static void k_show(void *c, const uint32_t *px, unsigned w, unsigned h, unsigned stride)
{
    (void)c;
    volatile uint32_t *fb = (volatile uint32_t *)SB_FB;
    const unsigned x0 = 120, y0 = 400;
    for (unsigned y = 0; y < h && y0 + y < SB_FB_H; ++y)
        for (unsigned x = 0; x < w && x0 + x < SB_FB_W; ++x)
            fb[(size_t)(y0 + y) * SB_FB_W + x0 + x] = px[(size_t)y * stride + x];
}

void cam_hal_libsb(struct cam_hal *h)
{
    h->ctx = 0;
    h->rd32 = k_rd32;
    h->wr32 = k_wr32;
    h->i2c_init = k_i2c_init;
    h->i2c = k_i2c;
    h->smc = k_smc;
    h->delay_us = k_delay;
    h->now_us = k_now;
    h->log = k_log;
    h->ram = k_ram;
    h->show = k_show;
}
#endif
