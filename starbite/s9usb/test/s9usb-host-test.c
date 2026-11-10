/* SPDX-License-Identifier: GPL-2.0-only
 * Host test for s9usb (starbite/s9usb/test/run.sh): runs the PHY and DWC3 code against fake
 * register blocks and checks the final register values for both roles against
 * docs/WIN11-USB.md 3.1-3.3, plus the xHCI checks against a fake capability layout. The fake
 * hardware: DCTL CSFTRST clears on the next delay, everything else is plain memory.
 * The MAX77705/HSI2C code is not simulated (it is the loader's proven code, re-based).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../s9usb.h"

static uint8_t *mem[S9_NBLK];
static unsigned fails, checks;
static unsigned long long delayed_us;

static uint32_t *R(enum s9usb_blk b, uint32_t off) { return (uint32_t *)(mem[b] + off); }

static void fake_udelay(unsigned us)
{
    delayed_us += us;
    *R(S9_DWC, 0xc704) &= ~(1U << 30);          /* soft reset completes */
}

static void check(const char *what, uint32_t got, uint32_t want)
{
    ++checks;
    if (got != want) {
        ++fails;
        printf("FAIL %-28s got %08x want %08x\n", what, got, want);
    }
}

static void reset_blocks(void)
{
    for (unsigned i = 0; i < S9_NBLK; ++i)
        memset(mem[i], 0, s9usb_size[i]);
    /* PHY as S-Boot might leave it: SIDDQ on, UTMI forced to sleep */
    *R(S9_USBCON, 0x5c) = 1U << 24;
    *R(S9_USBCON, 0x50) = 0x3;
    /* DWC3 3.00a in device role, xHCI capability layout of a 1+1 port DWC3 */
    *R(S9_DWC, 0xc120) = 0x5533300aU;
    *R(S9_DWC, 0xc110) = 2U << 12;
    *R(S9_DWC, 0x00) = 0x01100020U;              /* HCIVERSION 1.10, CAPLENGTH 0x20 */
    *R(S9_DWC, 0x04) = 0x02000140U;              /* MaxPorts 2 */
    *R(S9_DWC, 0x10) = (0x220U << 16) | 1U;      /* xECP at 0x880, AC64 */
    *R(S9_DWC, 0x24) = 1U;                       /* USBSTS: HCHalted */
    *R(S9_DWC, 0x880) = 0x02000802U;             /* protocol USB 2, next +32 bytes */
    *R(S9_DWC, 0x888) = 0x00000101U;             /* ports 1..1 */
    *R(S9_DWC, 0x8a0) = 0x03000002U;             /* protocol USB 3, last */
    *R(S9_DWC, 0x8a8) = 0x00000102U;             /* ports 2..2 */
}

static int run(enum s9usb_role role)
{
    struct s9usb_plat p;
    struct s9usb_dwc3_info d;
    const char *why = 0;
    for (unsigned i = 0; i < S9_NBLK; ++i)
        p.base[i] = mem[i];
    p.udelay = fake_udelay;
    reset_blocks();
    s9usb_clocks_on(&p);
    if (s9usb_phy_init(&p, role, &why)) {
        printf("FAIL phy_init: %s\n", why);
        ++fails;
        return -1;
    }
    if (s9usb_dwc3_core_init(&p, role, &d, &why)) {
        printf("FAIL dwc3_core_init: %s\n", why);
        ++fails;
        return -1;
    }
    const int host = role == S9USB_HOST;
    const char *r = host ? "host" : "device";
    printf("%s: utmi %08x hsp %08x tune %08x link %08x clkrst %08x | gctl %08x gusb2 %08x guctl %08x guctl1 %08x\n",
           r, *R(S9_USBCON, 0x50), *R(S9_USBCON, 0x54), *R(S9_USBCON, 0x58), *R(S9_USBCON, 0x04),
           *R(S9_USBCON, 0x20), d.gctl, d.gusb2, d.guctl, d.guctl1);

    check("top gate", *R(S9_CMU_TOP, 0x2064), 0x00300000U);
    check("fsys0 ref gate", *R(S9_CMU_FSYS0, 0x206c), 0x00300000U);
    check("fsys0 dpphy gate", *R(S9_CMU_FSYS0, 0x2008), 0x00300000U);
    check("pmu usb20", *R(S9_PMU, 0x72c), 1U);
    check("pmu usb3dp", *R(S9_PMU, 0x704), 1U);
    check("usbcon hsp_test (SIDDQ off)", *R(S9_USBCON, 0x5c), 0);
    check("usbcon clkrst", *R(S9_USBCON, 0x20), 1U << 2);
    check("usbcon link_ctrl", *R(S9_USBCON, 0x04), 0x00051ff0U);
    check("usbcon link_port", *R(S9_USBCON, 0x08), 0xcU);
    check("usbcon pma", *R(S9_USBCON, 0x48), 0x10U);
    check("usbcon utmi", *R(S9_USBCON, 0x50), host ? 0x0cU : 0x30U);
    check("usbcon hsp", *R(S9_USBCON, 0x54), host ? 0x0300U : 0x3300U);
    check("usbcon hsp_tune", *R(S9_USBCON, 0x58),
          host ? 0x712c0607U /* 7,1,1,3,6,7 */ : 0xd16c0403U /* d,1,3,3,4,3 */);
    check("dwc gctl", d.gctl, (host ? 1U : 2U) << 12 | (1U << 10));
    check("dwc gusb2phycfg", d.gusb2, 0);
    check("dwc gsbuscfg0", d.gsbuscfg0, 0x22220009U);
    check("dwc gsbuscfg1", *R(S9_DWC, 0xc104), 0x300U);
    check("dwc guctl", d.guctl, (0x14U << 22) | (1U << 14));
    check("dwc guctl1", d.guctl1, 1U << 26);
    check("dwc gfladj", d.gfladj, 0xa0U);
    check("dwc: was not a host", (uint32_t)d.was_host, 0);

    if (host) {
        struct s9usb_xhci_info x;
        if (s9usb_xhci_check(&p, &x, &why)) {
            printf("FAIL xhci_check: %s\n", why);
            ++fails;
            return -1;
        }
        printf("xhci: caplen %x hciver %04x ports %u ac64 %u usb2 %u+%u usb3 %u+%u\n", x.caplen,
               x.hciver, x.ports, x.ac64, x.usb2_first, x.usb2_count, x.usb3_first, x.usb3_count);
        check("xhci usb2 port", x.usb2_first << 8 | x.usb2_count, 0x101U);
        check("xhci usb3 port", x.usb3_first << 8 | x.usb3_count, 0x201U);
        /* a running controller must be refused */
        *R(S9_DWC, 0x24) = 0;
        ++checks;
        if (!s9usb_xhci_check(&p, &x, &why)) {
            ++fails;
            printf("FAIL xhci_check accepted a running controller\n");
        }
        /* a second init with the core already a running host: halted, whole-core reset */
        *R(S9_DWC, 0x24) = 1;
        *R(S9_DWC, 0x20) = 1;                    /* USBCMD.R/S */
        if (s9usb_dwc3_core_init(&p, role, &d, &why))
            printf("FAIL second init: %s\n", why), ++fails;
        check("second init: was a host", (uint32_t)d.was_host, 1);
        check("second init: R/S cleared", *R(S9_DWC, 0x20), 0);
        check("second init: core reset released", d.gctl & (1U << 11), 0);
        check("second init: PHY resets released", (*R(S9_DWC, 0xc200) | *R(S9_DWC, 0xc2c0)) >> 31, 0);
    }
    /* role switch of the PHY alone */
    s9usb_phy_set_role(&p, host ? S9USB_DEVICE : S9USB_HOST);
    check("set_role utmi", *R(S9_USBCON, 0x50), host ? 0x30U : 0x0cU);
    return 0;
}

int main(void)
{
    for (unsigned i = 0; i < S9_NBLK; ++i)
        mem[i] = calloc(1, s9usb_size[i]);
    run(S9USB_DEVICE);
    run(S9USB_HOST);
    /* a non-DWC3 must be refused */
    {
        struct s9usb_plat p;
        struct s9usb_dwc3_info d;
        const char *why;
        for (unsigned i = 0; i < S9_NBLK; ++i)
            p.base[i] = mem[i];
        p.udelay = fake_udelay;
        reset_blocks();
        *R(S9_DWC, 0xc120) = 0;
        ++checks;
        if (s9usb_dwc3_core_init(&p, S9USB_HOST, &d, &why) != -1)
            ++fails, printf("FAIL GSNPSID 0 accepted\n");
    }
    printf("%u checks, %u failures, %llu us of delays per two runs\n", checks, fails, delayed_us);
    return fails ? 1 : 0;
}
