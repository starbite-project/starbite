/** @file
  Blob SBUSBUFN: the S9's DWC3 as a USB function (device) controller alone, without the role
  switch (docs/WIN11-USB.md section 5, layout C). A fallback for the Starbite Link (L1) while URS
  can't be told the role: ufxsynopsys.inf matches ACPI\INT3445 inbox (our image's DriverDatabase).
  INT3445 is the ID Intel's platforms use for their DWC3 device controller; ufxsynopsys.inf installs
  it with the same generic section as URS\PNP0CA1&FUNCTION (only Qualcomm's IDs differ); whether
  ufxsynopsys.sys behaves differently by ID is unverified. Gated: installed only when
  Star2LteUsbDxe enabled it (device role, layout C). Packed by tools/woa/ssdt-ffs.py --gated.

  DWC3 at 0x10C00000 (64 KiB), GIC SPI 244 = GSIV 276 level high, _CCA 0 (not coherent).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

DefinitionBlock ("UsbUfn.aml", "SSDT", 2, "STRBTE", "SBUSBUFN", 0x00000001)
{
    Scope (\_SB)
    {
        Device (UFN0)
        {
            Name (_HID, "INT3445")
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
