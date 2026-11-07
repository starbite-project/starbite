/** @file
  SSDT for the S9's USB host (docs/WIN11-USB.md section 4), installed by Star2LteUsbHostDxe only
  after the controller is in host role and its xHCI answered.

  \_SB.USB0: the xHCI inside the DWC3 at 0x10C00000 (xHCI at +0, DWC3 globals at +0xC100; the
  whole 64 KiB window, as other DWC3 ACPI descriptions give it), GIC SPI 244
  (INTREQ__USB30DRD_Interrupt_00, Samsung exynos9810.dtsi) = GSIV 276, level high. _HID PNP0D10
  is the generic xHCI ID Windows' inbox usbxhci.sys binds to (the Raspberry Pi edk2 Xhci.asl
  pattern). _CCA 0: not cache-coherent (Linux has no dma-coherent for this controller).
  No _S0W/_PRW (no wake), no root-hub port objects yet (stage H2).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

DefinitionBlock ("UsbHostSsdt.aml", "SSDT", 2, "STRBTE", "STRBUSBH", 0x00000001)
{
    Scope (\_SB)
    {
        Device (USB0)
        {
            Name (_HID, "PNP0D10")
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x10C00000, 0x00010000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 276 }
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
    }
}
