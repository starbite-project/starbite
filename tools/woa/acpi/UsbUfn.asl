/** @file
  Blob SBUSBUFN: the S9's DWC3 as a USB function (device) controller alone, without the role
  switch (docs/WIN11-USB.md section 5, layout C). A route for the Starbite Link (L1) while URS
  can't be told the role: ufxsynopsys.inf matches ACPI\INT3445 inbox with the same generic install
  section as URS\PNP0CA1&FUNCTION (only Qualcomm's IDs differ). INT3445 is the ID Intel's platforms
  use for their DWC3 device controller. Gated: installed only when Star2LteUsbDxe enabled it
  (device role, layout C). Packed by tools/woa/ssdt-ffs.py --gated.

  DWC3 at 0x10C00000 (64 KiB), GIC SPI 244 = GSIV 276 level high, _CCA 0 (not coherent).

  Revision 2: attach notification. On the phone (L1, 2026-11-07) revision 1 bound ufxsynopsys but
  the controller never connected (no pull-up, the Mac saw nothing, no USBFN child): ufxsynopsys.sys
  carries Intel's xDCI _DSM GUID 732b85d5-b7a7-4a1b-9ba0-4bbd00ffd511 and learns of attach from
  the platform. The function semantics, from Intel's xDCI ACPI in Slim Bootloader
  (Platform/ApollolakeBoardPkg/AcpiTables/Dsdt/ScXdci.asl and TigerlakeBoardPkg .../PchXdci.asl,
  BSD-2-Clause-Patent; read, not copied):
    0  supported-functions bitmap
    1  attach/detach and port detection properties: bit 0 = 1, the platform notifies attach and
       detach; bit 1 = 0, hardware-based charging indication
    3  xDCI status: 1 enabled
    4  set PMU power state (Arg3 = Package {0 = D0 | 3 = D3}): Intel's PMU block; nothing here
    5  attach notification enabled: the function stack is ready, the platform answers with the
       current state: Notify (device, 0x80) attached, Notify (device, 0x81) detached
    7  get PMU power state: 0 = D0
  This node reports "attached" at function 5: Star2LteUsbDxe enables this blob only in device
  role, which the L1 runs boot with the Mac's cable plugged. A later detach is not reported (stage
  H3's Type-C driver will). _UPC 0x09 = Type-C USB2 (Microsoft's function-controller bring-up page).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

DefinitionBlock ("UsbUfn.aml", "SSDT", 2, "STRBTE", "SBUSBUFN", 0x00000002)
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
            Name (_UPC, Package () { One, 0x09, Zero, Zero })   // connectable, Type-C USB2
            Method (_STA, 0, NotSerialized) { Return (0x0F) }

            Method (_DSM, 4, Serialized)
            {
                If (LEqual (Arg0, ToUUID ("732b85d5-b7a7-4a1b-9ba0-4bbd00ffd511")))
                {
                    Switch (ToInteger (Arg2))
                    {
                        Case (0) { Return (Buffer () { 0xBB }) }   // functions 0, 1, 3, 4, 5, 7
                        Case (1) { Return (One) }                  // the platform notifies attach/detach
                        Case (3) { Return (One) }                  // xDCI enabled
                        Case (4) { Return (Zero) }                 // PMU power state: nothing to do
                        Case (5)
                        {
                            Notify (UFN0, 0x80)                    // attached
                            Return (Zero)
                        }
                        Case (7) { Return (Zero) }                 // PMU in D0
                    }
                }
                Return (Buffer () { Zero })
            }
        }
    }
}
