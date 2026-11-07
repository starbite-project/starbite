/** @file
  Blob SBUSBXHC: the S9's DWC3 as a plain xHCI host (docs/WIN11-USB.md section 4, layout A).
  Gated: Star2LteSsdtDxe installs it only when Star2LteUsbDxe enabled it (host role, layout A,
  every controller check passed). Packed by tools/woa/ssdt-ffs.py --gated.

  \_SB.USB0: the xHCI inside the DWC3 at 0x10C00000 (xHCI at +0, DWC3 globals at +0xC100; the
  whole 64 KiB window), GIC SPI 244 (INTREQ__USB30DRD_Interrupt_00, Samsung exynos9810.dtsi) =
  GSIV 276, level high. _HID PNP0D10: Windows 22631's usbxhci.inf matches ACPI\PNP0D10 (our
  image's DriverDatabase). _CCA 0: not cache-coherent (Linux has no dma-coherent for it).
  No _S0W/_PRW (no wake), no root-hub port objects yet (stage H2).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

DefinitionBlock ("UsbXhc.aml", "SSDT", 2, "STRBTE", "SBUSBXHC", 0x00000001)
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
