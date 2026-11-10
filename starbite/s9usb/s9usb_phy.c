/* SPDX-License-Identifier: GPL-2.0-only
 * s9usb: USB clocks, PMU isolation and the USB 2.0 PHY (USBCON 0x11100000) of the Exynos 9810.
 *
 * The enable sequence is starbite/utility/usb_phy.c usb_phy_start() step for step (Samsung's CAL
 * phy_exynos_usb_v3p1_enable() for EXYNOS_USBCON_VER_03_0_0, as our Linux driver
 * tools/linux/port/src/usb/phy-exynos9810-usbdrd.c runs it; woa's UsbDebugHid.c UsbEnablePhy,
 * BSD-2-Clause-Patent, does the same steps). The only change is the last part: instead of always
 * the device column, the role's column (docs/WIN11-USB.md 3.2):
 *   device: VBUS/BVALID forced valid (the board has no VBUS pad), external VBUS-valid selected,
 *           stock device tuning
 *   host:   CAL phy_exynos_usb_v3p1_config_host_mode(): the forces off, the 15k DP/DM
 *           pull-downs on; external VBUS-valid off; stock host tuning
 * Tuning: stock exynos9810-starlte_common.dtsi usb_hs_tune, tune_value = <device host>.
 */
#include "s9usb_io.h"

const uintptr_t s9usb_phys[S9_NBLK] = {
    [S9_DWC] = 0x10c00000UL,     [S9_USBCON] = 0x11100000UL,   [S9_CMU_TOP] = 0x1a240000UL,
    [S9_CMU_FSYS0] = 0x11000000UL, [S9_PMU] = 0x14060000UL,    [S9_HSI2C] = 0x14360000UL,
    [S9_CMU_CMGP] = 0x14200000UL, [S9_SYSREG_CMGP] = 0x14210000UL, [S9_GPIO_CMGP] = 0x14220000UL,
};
const uint32_t s9usb_size[S9_NBLK] = {
    [S9_DWC] = 0x10000U,  [S9_USBCON] = 0x1000U, [S9_CMU_TOP] = 0x3000U, [S9_CMU_FSYS0] = 0x4000U,
    [S9_PMU] = 0x1000U,   [S9_HSI2C] = 0x1000U,  [S9_CMU_CMGP] = 0x4000U, [S9_SYSREG_CMGP] = 0x3000U,
    [S9_GPIO_CMGP] = 0x1000U,
};

#define LINK_CTRL     0x04U
#define  LC_PIPE3_FORCE_RX_ELEC_IDLE (1U << 18)
#define  LC_PIPE3_FORCE_PHY_STATUS   (1U << 17)
#define  LC_PIPE3_FORCE_EN           (1U << 16)
#define  LC_DIS_QACT_LINKGATE        (1U << 12)
#define  LC_DIS_QACT_ID0             (1U << 11)
#define  LC_DIS_QACT_VBUS_VALID      (1U << 10)
#define  LC_DIS_QACT_BVALID          (1U << 9)
#define  LC_FORCE_QACT               (1U << 8)
#define  LC_BUS_FILTER_BYPASS        (0xfU << 4)
#define LINK_PORT     0x08U
#define  LP_HUB_PORT_SEL_OCD_U3      (1U << 3)
#define  LP_HUB_PORT_SEL_OCD_U2      (1U << 2)
#define CLKRST        0x20U
#define  CR_PHY_SW_RST               (1U << 3)
#define  CR_PHY_RST_SEL              (1U << 2)
#define  CR_PORT_RST                 (1U << 1)
#define  CR_LINK_SW_RST              (1U << 0)
#define COMBO_PMA     0x48U
#define  PMA_LOW_PWRN                (1U << 4)
#define UTMI          0x50U
#define  UTMI_FORCE_VBUSVALID        (1U << 5)
#define  UTMI_FORCE_BVALID           (1U << 4)
#define  UTMI_DP_PULLDOWN            (1U << 3)
#define  UTMI_DM_PULLDOWN            (1U << 2)
#define  UTMI_FORCE_SUSPEND          (1U << 1)
#define  UTMI_FORCE_SLEEP            (1U << 0)
#define HSP           0x54U
#define  HSP_VBUSVLDEXTSEL           (1U << 13)
#define  HSP_VBUSVLDEXT              (1U << 12)
#define  HSP_EN_UTMISUSPEND          (1U << 9)
#define  HSP_COMMONONN               (1U << 8)
#define HSP_TUNE      0x58U
#define HSP_TEST      0x5cU
#define  HSP_TEST_SIDDQ              (1U << 24)
#define REWA_ENABLE   0x100U
#define HSREWA_CTRL   0x108U
#define HSREWA_REFTO  0x10cU
#define HSREWA_HSTK   0x110U
#define HSREWA_INT1_MASK 0x11cU

/* HSP_TUNE: tx_vref [31:28], tx_rise [25:24], tx_res [22:21], tx_pre_emp [19:18],
 * rx_sqrx [10:8], compdis [2:0] */
#define TUNE(vref, rise, res, pre, sqrx, comp) \
    (((vref) << 28) | ((rise) << 24) | ((res) << 21) | ((pre) << 18) | ((sqrx) << 8) | (comp))
#define TUNE_MASK   TUNE(0xfU, 0x3U, 0x3U, 0x3U, 0x7U, 0x7U)
#define TUNE_DEVICE TUNE(0xdU, 1U, 3U, 3U, 4U, 3U)
#define TUNE_HOST   TUNE(0x7U, 1U, 1U, 3U, 6U, 7U)

/* the role column (docs/WIN11-USB.md 3.2) */
#define UTMI_ROLE_BITS (UTMI_FORCE_VBUSVALID | UTMI_FORCE_BVALID | UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN)
#define UTMI_DEVICE    (UTMI_FORCE_VBUSVALID | UTMI_FORCE_BVALID)
#define UTMI_HOST      (UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN)
#define HSP_ROLE_BITS  (HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT)

/* CMU_TOP / CMU_FSYS0 / PMU offsets */
#define TOP_MUX_USB30DRD   0x1060U
#define TOP_DIV_USB30DRD   0x1858U
#define TOP_GATE_USB30DRD  0x2064U
#define FSYS0_MUX          0x01e0U
#define FSYS0_MUX_AUTOGATE 0x0228U
#define FSYS0_GATE_REF     0x206cU
#define FSYS0_GATE_DPPHY   0x2008U
#define FSYS0_QCH_CMU      0x3000U
#define PMU_USB20_PHY      0x072cU
#define PMU_USB3DP_PHY     0x0704U
#define GATE_MANUAL_CGVAL  ((1U << 20) | (1U << 21))

void s9usb_clocks_on(const struct s9usb_plat *p)
{
    s9_rmw(p, S9_CMU_TOP, TOP_GATE_USB30DRD, 0, GATE_MANUAL_CGVAL);
    s9_rmw(p, S9_CMU_FSYS0, FSYS0_GATE_REF, 0, GATE_MANUAL_CGVAL);
    s9_rmw(p, S9_CMU_FSYS0, FSYS0_GATE_DPPHY, 0, GATE_MANUAL_CGVAL);
    s9_rmw(p, S9_PMU, PMU_USB20_PHY, 0, 1U);
    s9_rmw(p, S9_PMU, PMU_USB3DP_PHY, 0, 1U);
    S9_DSB();
    p->udelay(10);
}

/* exynos_cal_usbphy_q_ch(enable): the Q-channel forced active, with the vendor's delays */
static void q_ch_on(const struct s9usb_plat *p)
{
    uint32_t v = s9_rd(p, S9_USBCON, LINK_CTRL);
    v |= LC_DIS_QACT_ID0 | LC_DIS_QACT_VBUS_VALID | LC_DIS_QACT_BVALID | LC_DIS_QACT_LINKGATE;
    v &= ~LC_FORCE_QACT;
    p->udelay(500);
    s9_wr(p, S9_USBCON, LINK_CTRL, v);
    p->udelay(500);
    v = s9_rd(p, S9_USBCON, LINK_CTRL) | LC_FORCE_QACT;
    p->udelay(500);
    s9_wr(p, S9_USBCON, LINK_CTRL, v);
}

void s9usb_phy_set_role(const struct s9usb_plat *p, enum s9usb_role role)
{
    if (role == S9USB_HOST) {
        s9_rmw(p, S9_USBCON, UTMI, UTMI_ROLE_BITS, UTMI_HOST);
        s9_rmw(p, S9_USBCON, HSP, HSP_ROLE_BITS, 0);
        s9_rmw(p, S9_USBCON, HSP_TUNE, TUNE_MASK, TUNE_HOST);
    } else {
        s9_rmw(p, S9_USBCON, UTMI, UTMI_ROLE_BITS, UTMI_DEVICE);
        s9_rmw(p, S9_USBCON, HSP, HSP_ROLE_BITS, HSP_ROLE_BITS);
        s9_rmw(p, S9_USBCON, HSP_TUNE, TUNE_MASK, TUNE_DEVICE);
    }
    S9_DSB();
}

int s9usb_phy_init(const struct s9usb_plat *p, enum s9usb_role role, const char **why)
{
    const enum s9usb_blk r = S9_USBCON;

    /* phy_exynos_usb_v3p1_enable(), VER_03_0_0 */
    q_ch_on(p);
    s9_rmw(p, r, CLKRST, 0, CR_LINK_SW_RST);                          /* link reset */
    p->udelay(10);
    s9_rmw(p, r, CLKRST, CR_LINK_SW_RST, 0);
    s9_rmw(p, r, CLKRST, 0, CR_PHY_SW_RST | CR_PHY_RST_SEL);          /* PHY reset high */
    s9_rmw(p, r, HSP_TEST, HSP_TEST_SIDDQ, 0);                        /* PHY power: SIDDQ off */
    s9_rmw(p, r, UTMI, UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP | UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN, 0);
    s9_rmw(p, r, HSP, 0, HSP_EN_UTMISUSPEND | HSP_COMMONONN);         /* common_block_disable = 1 */
    p->udelay(100);
    s9_rmw(p, r, CLKRST, CR_PHY_SW_RST | CR_PORT_RST, CR_PHY_RST_SEL); /* PHY reset low */
    s9_rmw(p, r, LINK_CTRL, 0, LC_BUS_FILTER_BYPASS);                 /* no VBUS pad on this board */
    s9_rmw(p, r, LINK_PORT, 0, LP_HUB_PORT_SEL_OCD_U3 | LP_HUB_PORT_SEL_OCD_U2);
    /* phy_exynos_usb3p1_rewa_ready(): ReWA parked */
    s9_rmw(p, r, REWA_ENABLE, 1U, 0);
    s9_rmw(p, r, HSREWA_CTRL, 1U << 24, 1U << 28);
    s9_wr(p, r, HSREWA_HSTK, 0x1U);
    s9_wr(p, r, HSREWA_REFTO, 0xff00U);
    s9_rmw(p, r, HSREWA_INT1_MASK, 0, (1U << 18) | (1U << 17) | (1U << 16) | 7U);
    /* phy_exynos_usb_v3p1_pipe_ovrd(): the USB3 side forced idle, its PMA powered down */
    s9_rmw(p, r, LINK_CTRL, LC_PIPE3_FORCE_PHY_STATUS, LC_PIPE3_FORCE_EN | LC_PIPE3_FORCE_RX_ELEC_IDLE);
    s9_rmw(p, r, COMBO_PMA, 0, PMA_LOW_PWRN);
    /* the role: VBUS sensing, pull-downs, tuning */
    s9usb_phy_set_role(p, role);
    p->udelay(100);

    if (s9_rd(p, r, HSP_TEST) & HSP_TEST_SIDDQ) {
        *why = "USBCON: the PHY stayed in SIDDQ";
        return -1;
    }
    if ((s9_rd(p, r, UTMI) & UTMI_ROLE_BITS) != (role == S9USB_HOST ? UTMI_HOST : UTMI_DEVICE)) {
        *why = "USBCON: the UTMI role bits did not stick";
        return -1;
    }
    *why = 0;
    return 0;
}

const char *s9usb_phy_line(const struct s9usb_plat *p)
{
    unsigned ls = (s9_rd(p, S9_USBCON, HSP_TEST) >> 20) & 3U;   /* HSP_TEST LINESTATE */
    return ls == 0 ? "SE0" : ls == 1 ? "J" : ls == 2 ? "K" : "SE1";
}

static const struct { enum s9usb_blk b; uint16_t off; const char *name; } snap[S9USB_SNAP_N] = {
    { S9_CMU_TOP, TOP_MUX_USB30DRD, "top.mux" },
    { S9_CMU_TOP, TOP_DIV_USB30DRD, "top.div" },
    { S9_CMU_TOP, TOP_GATE_USB30DRD, "top.gate" },
    { S9_CMU_FSYS0, FSYS0_MUX, "fsys0.mux" },
    { S9_CMU_FSYS0, FSYS0_MUX_AUTOGATE, "fsys0.mux_ag" },
    { S9_CMU_FSYS0, FSYS0_GATE_REF, "fsys0.ref_gate" },
    { S9_CMU_FSYS0, FSYS0_GATE_DPPHY, "fsys0.dpphy_gate" },
    { S9_CMU_FSYS0, FSYS0_QCH_CMU, "qch.cmu" },
    { S9_CMU_FSYS0, 0x304cU, "qch.ctrl" },
    { S9_CMU_FSYS0, 0x3050U, "qch.link" },
    { S9_CMU_FSYS0, 0x3054U, "qch.phy" },
    { S9_CMU_FSYS0, 0x3058U, "qch.pcs" },
    { S9_PMU, PMU_USB20_PHY, "pmu.usb20" },
    { S9_PMU, PMU_USB3DP_PHY, "pmu.usb3dp" },
    { S9_USBCON, 0x00U, "usbcon.ver" },
    { S9_USBCON, LINK_CTRL, "usbcon.link" },
    { S9_USBCON, CLKRST, "usbcon.clkrst" },
    { S9_USBCON, UTMI, "usbcon.utmi" },
    { S9_USBCON, HSP, "usbcon.hsp" },
    { S9_USBCON, HSP_TUNE, "usbcon.hsp_tune" },
    { S9_USBCON, HSP_TEST, "usbcon.hsp_test" },
};

void s9usb_snapshot(const struct s9usb_plat *p, uint32_t *out, unsigned n)
{
    for (unsigned i = 0; i < n && i < S9USB_SNAP_N; ++i)
        out[i] = s9_rd(p, snap[i].b, snap[i].off);
}

const char *s9usb_snap_name(unsigned i)
{
    return i < S9USB_SNAP_N ? snap[i].name : "?";
}
