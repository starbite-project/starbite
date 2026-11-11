/* SPDX-License-Identifier: APSL-2.0
 * Register facts for the Exynos 9810 USB DRD (Synopsys DWC3) and its USB 2.0 PHY glue ("USBCON").
 * Sources per group in NOTES.md. Offsets and bit positions are hardware facts; no code is taken from the
 * loader (shim/starlte/usb_*.c, marked GPL-2.0-only) or from Linux.
 */
#ifndef SBUSB_REGS_H
#define SBUSB_REGS_H

/* ---- DWC3 global (block SBUSB_BLK_DWC3) ---- */
#define D3_GSBUSCFG0            0xc100u
#define  D3_GSBUSCFG0_INCRBRST          (1u << 0)
#define  D3_GSBUSCFG0_INCR16            (1u << 3)
#define  D3_GSBUSCFG0_REQINFO_2         ((2u << 16) | (2u << 20) | (2u << 24) | (2u << 28))
#define D3_GSBUSCFG1            0xc104u
#define  D3_GSBUSCFG1_BREQLIMIT_MASK    (0xfu << 8)
#define  D3_GSBUSCFG1_BREQLIMIT_3       (3u << 8)
#define D3_GCTL                 0xc110u
#define  D3_GCTL_DSBLCLKGTNG            (1u << 0)
#define  D3_GCTL_SCALEDOWN_MASK         (3u << 4)
#define  D3_GCTL_SOFITPSYNC             (1u << 10)
#define  D3_GCTL_PRTCAP_MASK            (3u << 12)
#define  D3_GCTL_PRTCAP_DEVICE          (2u << 12)
#define D3_GSTS                 0xc118u
#define D3_GSNPSID              0xc120u
#define  D3_ID_USB3                     0x5533u         /* GSNPSID >> 16 */
#define  D3_ID_USB31                    0x3331u
#define D3_GUCTL                0xc12cu
#define  D3_GUCTL_HSTINAUTORETRY        (1u << 14)
#define  D3_GUCTL_REFCLKPER_MASK        (0x3ffu << 22)
#define  D3_GUCTL_REFCLKPER_0x14        (0x14u << 22)
#define D3_GUSB2PHYCFG          0xc200u
#define  D3_U2_SUSPHY                   (1u << 6)
#define  D3_U2_ENBLSLPM                 (1u << 8)
#define  D3_U2_FREECLK_EXISTS           (1u << 30)
#define  D3_U2_PHYSOFTRST               (1u << 31)
#define D3_GUSB3PIPECTL         0xc2c0u
#define  D3_U3_SUSPHY                   (1u << 17)
#define D3_GEVNTADRLO           0xc400u
#define D3_GEVNTADRHI           0xc404u
#define D3_GEVNTSIZ             0xc408u
#define  D3_GEVNTSIZ_INTMASK            (1u << 31)
#define D3_GEVNTCOUNT           0xc40cu
#define  D3_GEVNTCOUNT_MASK             0xfffcu
#define D3_GFLADJ               0xc630u
#define  D3_GFLADJ_30MHZ_MASK           0x3fu
#define  D3_GFLADJ_30MHZ_SDBND_SEL      (1u << 7)
#define D3_DCFG                 0xc700u
#define  D3_DCFG_SPEED_MASK             7u              /* 0 = high speed */
#define  D3_DCFG_ADDR_MASK              (0x7fu << 3)
#define  D3_DCFG_ADDR(a)                ((uint32_t)(a) << 3)
#define  D3_DCFG_LPM_CAP                (1u << 22)
#define D3_DCTL                 0xc704u
#define  D3_DCTL_ULSTCHNGREQ_MASK       (0xfu << 5)     /* write 0: never request a link state */
#define  D3_DCTL_CSFTRST                (1u << 30)
#define  D3_DCTL_RUN_STOP               (1u << 31)
#define D3_DEVTEN               0xc708u
#define  D3_DEVTEN_DISCONN              (1u << 0)
#define  D3_DEVTEN_USBRST               (1u << 1)
#define  D3_DEVTEN_CONNDONE             (1u << 2)
#define  D3_DEVTEN_ULSTCNG              (1u << 3)
#define D3_DSTS                 0xc70cu
#define  D3_DSTS_SPEED_MASK             7u              /* 0 high, 1 or 3 full */
#define  D3_DSTS_DEVCTRLHLT             (1u << 22)
#define D3_DALEPENA             0xc720u
/* per physical endpoint n (2 x number + 1 for IN): parameters then command */
#define D3_DEPCMDPAR2(n)        (0xc800u + 0x10u * (n))
#define D3_DEPCMDPAR1(n)        (0xc804u + 0x10u * (n))
#define D3_DEPCMDPAR0(n)        (0xc808u + 0x10u * (n))
#define D3_DEPCMD(n)            (0xc80cu + 0x10u * (n))
#define  D3_CMD_SETEPCONFIG             1u
#define  D3_CMD_SETXFERRES              2u
#define  D3_CMD_SETSTALL                4u
#define  D3_CMD_CLEARSTALL              5u
#define  D3_CMD_STARTXFER               6u
#define  D3_CMD_UPDATEXFER              7u
#define  D3_CMD_ENDXFER                 8u
#define  D3_CMD_STARTCFG                9u
#define  D3_CMD_IOC                     (1u << 8)
#define  D3_CMD_ACT                     (1u << 10)
#define  D3_CMD_FORCERM                 (1u << 11)
#define  D3_CMD_STATUS(v)               (((v) >> 12) & 0xfu)
#define  D3_CMD_PARAM(x)                ((uint32_t)(x) << 16)   /* resource index / STARTCFG base */
#define  D3_CMD_RESIDX(v)               (((v) >> 16) & 0x7fu)
/* SETEPCONFIG parameter 0 / 1 */
#define  D3_CFG0_TYPE(t)                ((uint32_t)(t) << 1)    /* 0 control, 2 bulk, 3 interrupt */
#define  D3_CFG0_MPS(m)                 ((uint32_t)(m) << 3)
#define  D3_CFG0_FIFO(f)                ((uint32_t)(f) << 17)
#define  D3_CFG0_ACTION_INIT            (0u << 30)
#define  D3_CFG0_ACTION_MODIFY          (2u << 30)
#define  D3_CFG1_XFERCOMPLETE           (1u << 8)
#define  D3_CFG1_XFERINPROGRESS         (1u << 9)
#define  D3_CFG1_XFERNOTREADY           (1u << 10)
#define  D3_CFG1_INTERVAL_M1(i)         ((uint32_t)(i) << 16)
#define  D3_CFG1_EPNUM(phys)            ((uint32_t)(phys) << 25)

/* TRB: four little-endian words: buffer low, buffer high, size | status << 28, control */
#define D3_TRB_BYTES            16u
#define  D3_TRB_SIZE_MASK               0x00ffffffu
#define  D3_TRB_STATUS(sz)              ((sz) >> 28)
#define  D3_TRBSTS_SETUP_PENDING        2u
#define  D3_TRB_HWO                     (1u << 0)
#define  D3_TRB_LST                     (1u << 1)
#define  D3_TRB_CHN                     (1u << 2)
#define  D3_TRB_CSP                     (1u << 3)
#define  D3_TRB_TYPE_NORMAL             (1u << 4)
#define  D3_TRB_TYPE_SETUP              (2u << 4)
#define  D3_TRB_TYPE_STATUS2            (3u << 4)
#define  D3_TRB_TYPE_STATUS3            (4u << 4)
#define  D3_TRB_TYPE_DATA               (5u << 4)
#define  D3_TRB_TYPE_LINK               (8u << 4)
#define  D3_TRB_ISP                     (1u << 10)
#define  D3_TRB_IOC                     (1u << 11)

/* event word */
#define D3_EV_IS_DEV(e)         ((e) & 1u)
#define D3_EV_DEV_KIND(e)       (((e) >> 1) & 0x7fu)    /* 0 = device event */
#define D3_EV_DEV_TYPE(e)       (((e) >> 8) & 0xfu)     /* 0 disconnect, 1 reset, 2 connect done, 3 link state */
#define D3_EV_DEV_INFO(e)       (((e) >> 16) & 0x1ffu)  /* link state in bits 3:0 for type 3 */
#define D3_EV_EP_PHYS(e)        (((e) >> 1) & 0x1fu)
#define D3_EV_EP_TYPE(e)        (((e) >> 6) & 0xfu)     /* 1 complete, 2 in progress, 3 not ready */
#define D3_EV_EP_STATUS(e)      (((e) >> 12) & 0xfu)    /* not ready: bits 1:0 = 1 data, 2 status */
#define  D3_EVT_XFERCOMPLETE            1u
#define  D3_EVT_XFERINPROGRESS          2u
#define  D3_EVT_XFERNOTREADY            3u
#define  D3_LINK_U0                     0u
#define  D3_LINK_U3                     3u              /* USB 2.0 suspend */

/* ---- USBCON, the PHY glue (block SBUSB_BLK_PHY) ---- */
#define UC_LINK_CTRL            0x04u
#define  UC_LC_FORCE_QACT               (1u << 8)
#define  UC_LC_DIS_QACT_BVALID          (1u << 9)
#define  UC_LC_DIS_QACT_VBUS_VALID      (1u << 10)
#define  UC_LC_DIS_QACT_ID0             (1u << 11)
#define  UC_LC_DIS_QACT_LINKGATE        (1u << 12)
#define  UC_LC_BUS_FILTER_BYPASS        (0xfu << 4)
#define  UC_LC_PIPE3_FORCE_EN           (1u << 16)
#define  UC_LC_PIPE3_FORCE_PHY_STATUS   (1u << 17)
#define  UC_LC_PIPE3_FORCE_RX_ELEC_IDLE (1u << 18)
#define UC_LINK_PORT            0x08u
#define  UC_LP_HUB_PORT_SEL_OCD_U2      (1u << 2)
#define  UC_LP_HUB_PORT_SEL_OCD_U3      (1u << 3)
#define UC_CLKRST               0x20u
#define  UC_CR_LINK_SW_RST              (1u << 0)
#define  UC_CR_PORT_RST                 (1u << 1)
#define  UC_CR_PHY_RST_SEL              (1u << 2)
#define  UC_CR_PHY_SW_RST               (1u << 3)
#define UC_COMBO_PMA            0x48u
#define  UC_PMA_LOW_PWRN                (1u << 4)
#define UC_UTMI                 0x50u
#define  UC_UTMI_FORCE_SLEEP            (1u << 0)
#define  UC_UTMI_FORCE_SUSPEND          (1u << 1)
#define  UC_UTMI_DM_PULLDOWN            (1u << 2)
#define  UC_UTMI_DP_PULLDOWN            (1u << 3)
#define  UC_UTMI_FORCE_BVALID           (1u << 4)
#define  UC_UTMI_FORCE_VBUSVALID        (1u << 5)
#define UC_HSP                  0x54u
#define  UC_HSP_COMMONONN               (1u << 8)
#define  UC_HSP_EN_UTMISUSPEND          (1u << 9)
#define  UC_HSP_VBUSVLDEXT              (1u << 12)
#define  UC_HSP_VBUSVLDEXTSEL           (1u << 13)
#define UC_HSP_TUNE             0x58u
/* stock star2lte device-mode high-speed tuning: tx_vref 0xd, tx_rise 1, tx_res 3, tx_pre_emp 3, rx_sqrx 4, compdis 3 */
#define  UC_TUNE_MASK   ((0xfu << 28) | (0x3u << 24) | (0x3u << 21) | (0x3u << 18) | (0x7u << 8) | 0x7u)
#define  UC_TUNE_VALUE  ((0xdu << 28) | (0x1u << 24) | (0x3u << 21) | (0x3u << 18) | (0x4u << 8) | 0x3u)
#define UC_HSP_TEST             0x5cu
#define  UC_HSP_TEST_LINESTATE(v)       (((v) >> 20) & 3u)      /* 0 SE0, 1 J, 2 K, 3 SE1 */
#define  UC_HSP_TEST_SIDDQ              (1u << 24)
#define UC_REWA_ENABLE          0x100u
#define UC_HSREWA_CTRL          0x108u
#define UC_HSREWA_REFTO         0x10cu
#define UC_HSREWA_HSTK          0x110u
#define UC_HSREWA_INT1_MASK     0x11cu

/* single-register blocks */
#define PMU_PHY_ENABLE          (1u << 0)
#define GATE_MANUAL_CGVAL       ((1u << 20) | (1u << 21))

#endif
