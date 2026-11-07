/* SPDX-License-Identifier: GPL-2.0-only
 * s9usb: DWC3 core init common to both roles, and read-only xHCI checks for the host role.
 *
 * Steps 1-9 are starbite/utility/usb_dwc3.c usbd_start() (proven in device mode on this phone):
 * Linux drivers/usb/dwc3/core.c dwc3_core_init order, our Linux DT quirks for usb@10c00000
 * (dis_u2_susphy, dis_u3_susphy, dis-u2-freeclk-exists, frame-length-adjustment 0x20,
 * maximum-speed high-speed), and Samsung's dwc3_core_config bus values (also in woa's
 * UsbDebugHid.c and an earlier port). SOFITPSYNC (SOF/ITP from ref_clk while the PHY is suspended) is the
 * loader's and woa's setting, not Linux's (which sets it only for 2.10a..2.50a); with PHY suspend
 * disabled (dis_u2_susphy) it should not come into play. Host-specific: PRTCAPDIR = host; USBHSTINAUTORETRYEN matters only
 * there; GUCTL1 DEV_FORCE_20_CLK_FOR_30_CLK as Linux sets it for a high-speed-only core from
 * 2.90a (there is no USB3 PIPE clock). Table: docs/WIN11-USB.md 3.3.
 */
#include "s9usb_io.h"

#define GSBUSCFG0    0xc100U
#define  GSBUSCFG0_INCRBRSTEN   (1U << 0)
#define  GSBUSCFG0_INCR16BRSTEN (1U << 3)
#define  GSBUSCFG0_REQINFO      ((2U << 16) | (2U << 20) | (2U << 24) | (2U << 28))
#define GSBUSCFG1    0xc104U
#define GCTL         0xc110U
#define  GCTL_PRTCAP_SHIFT 12
#define  GCTL_PRTCAP_MASK  (3U << 12)
#define  GCTL_PRTCAP_HOST  1U
#define  GCTL_PRTCAP_DEV   2U
#define  GCTL_CORESOFTRESET (1U << 11)
#define  GCTL_SOFITPSYNC   (1U << 10)
#define  GCTL_SCALEDOWN    (3U << 4)
#define  GCTL_DSBLCLKGTNG  (1U << 0)
#define GUCTL1       0xc11cU
#define  GUCTL1_FORCE_20_CLK_FOR_30_CLK (1U << 26)
#define GSNPSID      0xc120U
#define GUCTL        0xc12cU
#define  GUCTL_REFCLKPER_MASK (0x3ffU << 22)
#define  GUCTL_REFCLKPER(x)   ((uint32_t)(x) << 22)
#define  GUCTL_HSTINAUTORETRY (1U << 14)
#define GUSB2PHYCFG  0xc200U
#define  U2_PHYSOFTRST     (1U << 31)
#define  U2_FREECLK_EXISTS (1U << 30)
#define  U2_ENBLSLPM       (1U << 8)
#define  U2_SUSPHY         (1U << 6)
#define GUSB3PIPECTL 0xc2c0U
#define  U3_PHYSOFTRST     (1U << 31)
#define  U3_SUSPHY         (1U << 17)
#define GFLADJ       0xc630U
#define  GFLADJ_30MHZ_SDBND_SEL (1U << 7)
#define  GFLADJ_30MHZ_MASK      0x3fU
#define DCTL         0xc704U
#define  DCTL_RUN_STOP     (1U << 31)
#define  DCTL_CSFTRST      (1U << 30)

/* DWC_usb3 revision compare: snpsid 0x5533XYZa; DWC_usb31 (0x3331....) is newer than all */
static int usb3_at_least(uint32_t id, uint32_t rev)
{
    if ((id >> 16) == 0x3331U)
        return 1;
    return (id & 0xffffU) >= rev;
}

int s9usb_dwc3_core_init(const struct s9usb_plat *p, enum s9usb_role role,
                         struct s9usb_dwc3_info *o, const char **why)
{
    const enum s9usb_blk d = S9_DWC;
    uint32_t id = s9_rd(p, d, GSNPSID), v;

    o->snpsid = id;
    o->was_host = 0;
    if ((id & 0xffff0000U) != 0x55330000U && (id & 0xffff0000U) != 0x33310000U) {
        *why = "no DWC3 at 0x10c00000 (GSNPSID)";
        return -1;
    }

    /* 2: reset. Normally the core is a device (the loader's USB link) or untouched: the
     * device-side soft reset stops it (Linux dwc3_core_soft_reset). If it already is a host
     * (a warm start after a host role), DCTL's reset does not cover the host side: stop the
     * xHCI (Run/Stop off, up to 20 ms for HCHalted), then the whole-core reset with both PHY
     * interface resets, as Samsung's dwc3_core_soft_reset and woa's UsbInitializeController do. */
    if (((s9_rd(p, d, GCTL) & GCTL_PRTCAP_MASK) >> GCTL_PRTCAP_SHIFT) == GCTL_PRTCAP_HOST) {
        uint32_t cap = s9_rd(p, d, 0x00U) & 0xffU;
        o->was_host = 1;
        if (cap >= 0x20U && cap <= 0x100U && !(cap & 3U)) {
            s9_rmw(p, d, cap + 0x00U, 1U, 0);                    /* USBCMD.R/S = 0 */
            (void)s9_wait(p, d, cap + 0x04U, 1U, 1U, 20000);    /* USBSTS.HCHalted */
        }
        s9_rmw(p, d, GCTL, 0, GCTL_CORESOFTRESET);
        s9_rmw(p, d, GUSB2PHYCFG, 0, U2_PHYSOFTRST);
        s9_rmw(p, d, GUSB3PIPECTL, 0, U3_PHYSOFTRST);
        p->udelay(100);
        s9_rmw(p, d, GUSB2PHYCFG, U2_PHYSOFTRST, 0);
        s9_rmw(p, d, GUSB3PIPECTL, U3_PHYSOFTRST, 0);
        p->udelay(100);
        s9_rmw(p, d, GCTL, GCTL_CORESOFTRESET, 0);
        p->udelay(10000);
    } else {
        s9_wr(p, d, DCTL, (s9_rd(p, d, DCTL) & ~DCTL_RUN_STOP) | DCTL_CSFTRST);
        if (!s9_wait(p, d, DCTL, DCTL_CSFTRST, 0, 200000)) {
            *why = "DWC3 soft reset stuck (no PHY clock?)";
            return -2;
        }
        p->udelay(10000);
    }

    /* 3-4: PHY interfaces: no PHY suspend, no LPM sleep signal, no free-running clock */
    v = s9_rd(p, d, GUSB2PHYCFG);
    v &= ~(U2_SUSPHY | U2_FREECLK_EXISTS | U2_ENBLSLPM | U2_PHYSOFTRST);
    s9_wr(p, d, GUSB2PHYCFG, v);
    s9_rmw(p, d, GUSB3PIPECTL, U3_SUSPHY, 0);

    /* 5: role, SOF/ITP from ref_clk, no scale-down; Linux's 2.10a..2.50a clock-gating workaround */
    v = s9_rd(p, d, GCTL) & ~(GCTL_SCALEDOWN | GCTL_PRTCAP_MASK | GCTL_DSBLCLKGTNG);
    if ((id >> 16) == 0x5533U && (id & 0xffffU) >= 0x210aU && (id & 0xffffU) <= 0x250aU)
        v |= GCTL_DSBLCLKGTNG;
    v |= (role == S9USB_HOST ? GCTL_PRTCAP_HOST : GCTL_PRTCAP_DEV) << GCTL_PRTCAP_SHIFT;
    s9_wr(p, d, GCTL, v | GCTL_SOFITPSYNC);

    /* 6-8: the bus as Samsung's dwc3_core_config sets it */
    v = s9_rd(p, d, GSBUSCFG0) | GSBUSCFG0_INCRBRSTEN | GSBUSCFG0_INCR16BRSTEN;
    if (usb3_at_least(id, 0x220aU))
        v |= GSBUSCFG0_REQINFO;
    s9_wr(p, d, GSBUSCFG0, v);
    s9_rmw(p, d, GSBUSCFG1, 0xfU << 8, 3U << 8);
    s9_rmw(p, d, GUCTL, GUCTL_REFCLKPER_MASK, GUCTL_REFCLKPER(0x14) | GUCTL_HSTINAUTORETRY);

    /* 9: frame length adjustment 0x20 (from 2.50a) */
    if (usb3_at_least(id, 0x250aU)) {
        v = s9_rd(p, d, GFLADJ);
        if ((v & GFLADJ_30MHZ_MASK) != 0x20U)
            s9_wr(p, d, GFLADJ, (v & ~GFLADJ_30MHZ_MASK) | GFLADJ_30MHZ_SDBND_SEL | 0x20U);
    }
    /* 10: high-speed only: the USB 2.0 clock stands in for the missing USB 3 one (DWC_usb3 from
     * 2.90a, as Linux: DWC3_VER_IS_WITHIN(DWC3, 290A, ANY) excludes DWC_usb31) */
    if ((id >> 16) == 0x5533U && (id & 0xffffU) >= 0x290aU)
        s9_rmw(p, d, GUCTL1, 0, GUCTL1_FORCE_20_CLK_FOR_30_CLK);
    S9_DSB();
    p->udelay(1000);

    o->gctl = s9_rd(p, d, GCTL);
    o->gusb2 = s9_rd(p, d, GUSB2PHYCFG);
    o->gusb3 = s9_rd(p, d, GUSB3PIPECTL);
    o->guctl = s9_rd(p, d, GUCTL);
    o->guctl1 = s9_rd(p, d, GUCTL1);
    o->gsbuscfg0 = s9_rd(p, d, GSBUSCFG0);
    o->gfladj = s9_rd(p, d, GFLADJ);
    if (((o->gctl & GCTL_PRTCAP_MASK) >> GCTL_PRTCAP_SHIFT) !=
        (role == S9USB_HOST ? GCTL_PRTCAP_HOST : GCTL_PRTCAP_DEV)) {
        *why = "GCTL did not take the role";
        return -3;
    }
    *why = 0;
    return 0;
}

/* ---- xHCI (host role) ----------------------------------------------------------------------- */

#define XHCI_END      0x8000U          /* the xHCI's share of the 64 KiB window */
#define CAPLENGTH     0x00U            /* [7:0] CAPLENGTH, [31:16] HCIVERSION */
#define HCSPARAMS1    0x04U
#define HCCPARAMS1    0x10U
#define OP_USBSTS     0x04U
#define  USBSTS_HCH   (1U << 0)
#define  USBSTS_CNR   (1U << 11)
#define XCAP_PROTOCOL 2U

int s9usb_xhci_check(const struct s9usb_plat *p, struct s9usb_xhci_info *x, const char **why)
{
    const enum s9usb_blk d = S9_DWC;
    uint32_t c = s9_rd(p, d, CAPLENGTH);

    x->caplen = c & 0xffU;
    x->hciver = c >> 16;
    x->hcs1 = s9_rd(p, d, HCSPARAMS1);
    x->hcc1 = s9_rd(p, d, HCCPARAMS1);
    x->ports = x->hcs1 >> 24;
    x->ac64 = x->hcc1 & 1U;
    x->usb2_first = x->usb2_count = x->usb3_first = x->usb3_count = 0;
    x->usbsts = 0;
    if (x->caplen < 0x20U || x->caplen > 0x100U || (x->caplen & 3U)) {
        *why = "xHCI: implausible CAPLENGTH";
        return -1;
    }
    if (x->hciver != 0x0100U && x->hciver != 0x0110U && x->hciver != 0x0120U) {
        *why = "xHCI: unknown HCIVERSION";
        return -1;
    }
    if (x->ports < 1U || x->ports > 4U) {
        *why = "xHCI: implausible MaxPorts";
        return -1;
    }
    /* the controller ready (CNR clear) within 100 ms, and halted */
    (void)s9_wait(p, d, x->caplen + OP_USBSTS, USBSTS_CNR, 0, 100000);
    x->usbsts = s9_rd(p, d, x->caplen + OP_USBSTS);
    if (x->usbsts & USBSTS_CNR) {
        *why = "xHCI: controller not ready (CNR)";
        return -1;
    }
    if (!(x->usbsts & USBSTS_HCH)) {
        *why = "xHCI: running, expected halted";
        return -1;
    }
    /* extended capabilities: the Supported Protocol entries give the port numbering */
    uint32_t off = (x->hcc1 >> 16) << 2;
    for (unsigned n = 0; off && off + 12U <= XHCI_END && n < 32U; ++n) {
        uint32_t w0 = s9_rd(p, d, off);
        if ((w0 & 0xffU) == XCAP_PROTOCOL) {
            uint32_t w2 = s9_rd(p, d, off + 8U);
            unsigned major = w0 >> 24;
            if (major == 2U) {
                x->usb2_first = w2 & 0xffU;
                x->usb2_count = (w2 >> 8) & 0xffU;
            } else if (major == 3U) {
                x->usb3_first = w2 & 0xffU;
                x->usb3_count = (w2 >> 8) & 0xffU;
            }
        }
        uint32_t next = (w0 >> 8) & 0xffU;
        off = next ? off + (next << 2) : 0;
    }
    if (!x->usb2_count) {
        *why = "xHCI: no USB 2.0 port in the protocol capabilities";
        return -1;
    }
    *why = 0;
    return 0;
}
