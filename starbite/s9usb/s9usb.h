/* SPDX-License-Identifier: GPL-2.0-only
 * s9usb: the Galaxy S9's (Exynos 9810) USB bring-up as portable C, shared by every place that
 * needs it: the Windows UEFI's USB host module (tools/woa/modules/Star2LteUsbHostDxe), later
 * Windows KMDF drivers (the Starbite Link's device mode, the Type-C driver), and libsb if the
 * loader switches to it. Design and register sources: docs/WIN11-USB.md section 3.
 *
 * No OS here: registers are reached through struct s9usb_plat (mapped bases + a delay), and
 * nothing allocates, prints or sleeps. Results come back in small structs for the caller to log.
 *
 * Where it comes from (all GPL-2.0-only, ours, proven on this phone in device mode):
 *   s9usb_phy.c      starbite/utility/usb_phy.c (Samsung CAL phy_exynos_usb_v3p1_enable) + the
 *                    host column of tools/linux/port/src/usb/phy-exynos9810-usbdrd.c
 *   s9usb_dwc3.c     the core part of starbite/utility/usb_dwc3.c usbd_start (Linux dwc3_core_init)
 *   s9usb_max77705.c starbite/utility/max77705_muic.c (HSI2C + the Type-C MCU's opcodes)
 */
#ifndef S9USB_H
#define S9USB_H

#if defined(S9USB_EDK2)               /* UEFI: edk2's types, no libc headers */
#include <Base.h>
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
typedef UINTN uintptr_t;
#elif defined(S9USB_PLAT_HEADER)
#include S9USB_PLAT_HEADER          /* must provide uint8_t, uint16_t, uint32_t, uint64_t, uintptr_t */
#else
#include <stdint.h>
#endif

/* register blocks; s9usb_phys[] has their physical bases for the platform to map */
enum s9usb_blk {
    S9_DWC,          /* 0x10c00000 DWC3 (xHCI at +0, globals at +0xc100), 64 KiB */
    S9_USBCON,       /* 0x11100000 USB 2.0 PHY + link glue */
    S9_CMU_TOP,      /* 0x1a240000 */
    S9_CMU_FSYS0,    /* 0x11000000 */
    S9_PMU,          /* 0x14060000 */
    S9_HSI2C,        /* 0x14360000 USI_CMGP03 in I2C mode: the MAX77705 */
    S9_CMU_CMGP,     /* 0x14200000 */
    S9_SYSREG_CMGP,  /* 0x14210000 */
    S9_GPIO_CMGP,    /* 0x14220000 */
    S9_NBLK
};

struct s9usb_plat {
    volatile uint8_t *base[S9_NBLK];      /* mapped bases (identity in UEFI and libsb) */
    void (*udelay)(unsigned us);
};

extern const uintptr_t s9usb_phys[S9_NBLK];
extern const uint32_t s9usb_size[S9_NBLK];

enum s9usb_role { S9USB_DEVICE = 0, S9USB_HOST = 1 };

/* ---- clocks, PMU, PHY (s9usb_phy.c) ---------------------------------------------------------- */

/* The three USB gates (MANUAL | CG_VAL) and both PMU isolation bits. Muxes, dividers and
 * Q-channels are never written. Idempotent. */
void s9usb_clocks_on(const struct s9usb_plat *p);

/* The vendor CAL enable sequence, then the role's UTMI/HSP/tune column. 0, or -1 when USBCON did
 * not take the settings (*why). Call s9usb_clocks_on() first. */
int s9usb_phy_init(const struct s9usb_plat *p, enum s9usb_role role, const char **why);

/* Only the role column (UTMI forces/pull-downs, HSP VBUS-valid select, HSP_TUNE), for a role
 * switch after s9usb_phy_init(). */
void s9usb_phy_set_role(const struct s9usb_plat *p, enum s9usb_role role);

/* HSP_TEST LINESTATE: "SE0", "J", "K" or "SE1" */
const char *s9usb_phy_line(const struct s9usb_plat *p);

/* Read-only snapshot of the clocks, PMU and USBCON for logs. */
#define S9USB_SNAP_N 21U
#define S9USB_SNAP_CLOCKS 14U   /* entries 0..13: CMU and PMU only, readable with the USB gates shut */
/* the first n entries (S9USB_SNAP_CLOCKS before s9usb_clocks_on(), S9USB_SNAP_N after) */
void s9usb_snapshot(const struct s9usb_plat *p, uint32_t *out, unsigned n);
const char *s9usb_snap_name(unsigned i);

/* ---- DWC3 core (s9usb_dwc3.c) --------------------------------------------------------------- */

struct s9usb_dwc3_info {
    uint32_t snpsid, gctl, gusb2, gusb3, guctl, guctl1, gsbuscfg0, gfladj;
    int was_host;               /* the core already was a host: halted, then the whole-core reset */
};

/* Soft reset and the global settings common to both roles (docs/WIN11-USB.md 3.3), with
 * GCTL PRTCAPDIR set to the role. Device mode still needs its event buffer, DCFG and endpoints
 * (the caller's own code); host mode needs nothing more: the xHCI is left halted. 0, or a
 * negative step with *why. Needs the PHY running (its clock completes the soft reset). */
int s9usb_dwc3_core_init(const struct s9usb_plat *p, enum s9usb_role role,
                         struct s9usb_dwc3_info *out, const char **why);

struct s9usb_xhci_info {
    uint32_t caplen, hciver, hcs1, hcc1, usbsts;
    unsigned ports;             /* HCSPARAMS1 MaxPorts */
    unsigned ac64;              /* HCCPARAMS1 AC64 */
    /* Supported Protocol capabilities: first port number and count, 0 if absent */
    unsigned usb2_first, usb2_count, usb3_first, usb3_count;
};

/* Read-only checks of the xHCI after s9usb_dwc3_core_init(HOST): sane capability registers,
 * halted, controller ready (waits up to 100 ms for CNR), the port numbering. 0 or -1 (*why). */
int s9usb_xhci_check(const struct s9usb_plat *p, struct s9usb_xhci_info *x, const char **why);

/* ---- MAX77705 Type-C MCU over HSI2C (s9usb_max77705.c) -------------------------------------- */

#define S9USB_CC_NONE   0U
#define S9USB_CC_SINK   1U   /* the phone is the sink: a charger or a computer */
#define S9USB_CC_SOURCE 2U   /* the phone is the source: a device behind an OTG adapter */

struct s9usb_max {
    int bus;                    /* 0 untouched, 1 answering, -1 no answer */
    uint32_t fs[3];             /* HSI2C timing (S-Boot's, or 400 kHz at 200 MHz) */
    uint8_t hw, fw, usbc1, bc, cc0;
    int ctrl1;                  /* CONTROL1 as read back, -1 no answer */
    uint8_t answer[4];          /* the last opcode answer (AP_DATAIN0..3) */
    uint32_t errors;            /* I2C transfers that failed and were retried */
};

/* Bus up (CMGP gates, USI I2C mode, the two pins) and the MCU's revision read. 1 or 0. */
int s9usb_max_init(const struct s9usb_plat *p, struct s9usb_max *m);
/* USBC_STATUS1, BC_STATUS, CC_STATUS0 into *m. 1 or 0. */
int s9usb_max_status(const struct s9usb_plat *p, struct s9usb_max *m);
static inline unsigned s9usb_max_ccstat(const struct s9usb_max *m) { return m->cc0 & 7U; }
static inline unsigned s9usb_max_vbusdet(const struct s9usb_max *m) { return (m->bc >> 7) & 1U; }
/* D+/D- to the AP (CONTROL1 = COM_USB 0x09), read back; four tries. 1 when read back. */
int s9usb_max_route_usb(const struct s9usb_plat *p, struct s9usb_max *m);
/* Opcode 0x54: the MCU's own VBUS control on (data 0x02) or off (0x00). 1 when the MCU
 * answered with the opcode within 100 ms (the answer is in m->answer). */
int s9usb_max_auto_vbus(const struct s9usb_plat *p, struct s9usb_max *m, int on);

#endif
