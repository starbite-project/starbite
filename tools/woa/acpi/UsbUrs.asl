/** @file
  Blob SBUSBURS: the S9's DWC3 as a dual-role controller under Windows' USB Role Switch driver
  (docs/WIN11-USB.md section 5, layout B). Gated: Star2LteSsdtDxe installs it only when
  Star2LteUsbDxe enabled it. Packed by tools/woa/ssdt-ffs.py --gated.

  Layout from Microsoft's "USB dual role driver stack architecture" sample: the URS device holds
  the register space; child _ADR 0 = the host stack, _ADR 1 = the function stack; each child has
  the controller interrupt, Shared (only one stack is ever loaded, but nothing then depends on it)
  and its own _CCA 0 (the children do the DMA; ACPI says _CCA is inherited, stated anyway). IDs (our image's DriverDatabase):
  ACPI\PNP0CA1 -> urssynopsys.inf; the URS driver names the children URS\PNP0CA1&HOST and
  URS\PNP0CA1&FUNCTION; the function child matches ufxsynopsys.inf inbox (compatible ID), the
  host child gets the compatible IDs PCI\CC_0C0330 + ACPI\PNP0D15 from urscx01000.sys, which
  usbxhci.inf matches inbox (docs/WIN11-USB.md section 5). No ID-pin GpioInt: a Type-C port, whose
  role comes from a UCM connector driver (stage H3/H4). Until that exists URS picks its own role,
  which need not be the firmware's boot role (docs/WIN11-USB.md section 5).

  DWC3 at 0x10C00000 (64 KiB), GIC SPI 244 = GSIV 276 level high, _CCA 0 (not coherent).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

DefinitionBlock ("UsbUrs.aml", "SSDT", 2, "STRBTE", "SBUSBURS", 0x00000001)
{
    Scope (\_SB)
    {
        Device (URS0)
        {
            Name (_HID, "PNP0CA1")
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x10C00000, 0x00010000)
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }

            Device (USB0)                   // host stack
            {
                Name (_ADR, Zero)
                Name (_CCA, Zero)
                Name (_CRS, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 276 }
                })
            }

            Device (UFN0)                   // function stack
            {
                Name (_ADR, One)
                Name (_CCA, Zero)
                Name (_CRS, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Shared) { 276 }
                })
            }
        }
    }
}
