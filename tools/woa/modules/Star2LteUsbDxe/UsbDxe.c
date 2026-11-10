/** @file
  Star2LteUsbDxe: brings up the S9's USB controller for Windows in the boot role, host or device
  (docs/WIN11-USB.md). Installs no ACPI table itself: the USB blobs are gated, and Star2LteSsdtDxe
  installs one at ReadyToBoot only if this module enabled it (section 4.1). Fails closed: no
  module, or any failed check, and Windows sees no USB controller at all.

  At dispatch: maps the USB, clock and MAX77705 register blocks that the firmware's own page
  tables leave out; reads the Type-C state; picks the role; runs s9usb (clocks, the USB 2.0 PHY's
  column for the role, the DWC3 core with PRTCAPDIR = the role); in host role checks the xHCI
  read-only; routes D+/D- to the AP; sets the Type-C MCU's VBUS policy.

  Enables (a NULL protocol under the gated blob's file GUID, 53534454-5342-4754-<Table ID>),
  only after every check passed:
    layout 0 (A, xHCI)          -> SBUSBXHC, in host role only
    layout 1 (B, URS)           -> SBUSBURS, either role
    layout 2 (C, function only) -> SBUSBUFN, in device role only

  Mapping keeps what is there: a block already mapped as uncached MMIO (the PMU page carries
  EFI_MEMORY_RUNTIME for the release's ResetSystem) is left untouched; only blocks the GCD does
  not know are added, and only the cache-type bits are ever changed.

  Build-time policy (patch 0024's DSC lines, from -D on the build command):
    STARBITE_USB_ROLE  0 host, 1 device, 2 auto: device when the MCU sees the phone as a sink
                       with VBUS present (a computer or charger at boot), otherwise host
    STARBITE_USB_ACPI  0 layout A (SBUSBXHC, PNP0D10), 1 layout B (SBUSBURS, PNP0CA1 + children),
                       2 layout C (SBUSBUFN, INT3445: ufxsynopsys alone, device role)
    STARBITE_USB_VBUS  0 the MCU's auto-VBUS off; 1 on (Samsung's max77705_usbc_enable_auto_vbus:
                       the MCU powers VBUS when it attaches as source), only in host role and
                       only when nothing powers VBUS and the phone isn't a sink at boot; after
                       that the MCU decides alone by its Type-C rules until its next reset

  Log: every line starts with "=USB" (StarbiteLog.h: the pmsg ring, the zone-0 evidence ring that
  survives a Windows session, DEBUG).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "s9usb/s9usb.h"
#include "StarbiteLog.h"

#ifndef STARBITE_USB_ROLE
#define STARBITE_USB_ROLE 0
#endif
#ifndef STARBITE_USB_ACPI
#define STARBITE_USB_ACPI 0
#endif
#ifndef STARBITE_USB_VBUS
#define STARBITE_USB_VBUS 0
#endif
#if STARBITE_USB_ROLE < 0 || STARBITE_USB_ROLE > 2
#error STARBITE_USB_ROLE must be 0 (host), 1 (device) or 2 (auto)
#endif
#if STARBITE_USB_ACPI < 0 || STARBITE_USB_ACPI > 2
#error STARBITE_USB_ACPI must be 0 (xHCI, SBUSBXHC), 1 (URS, SBUSBURS) or 2 (function, SBUSBUFN)
#endif
#if STARBITE_USB_VBUS != 0 && STARBITE_USB_VBUS != 1
#error STARBITE_USB_VBUS must be 0 (off) or 1 (MCU auto-VBUS)
#endif

#define USB_VERSION  4
#define USB_STR2(x)  #x
#define USB_STR(x)   USB_STR2 (x)
// also the marker tools/woa/build-win-usbhost.sh looks for in each variant's module
#define USB_POLICY   "usb-policy role=" USB_STR (STARBITE_USB_ROLE) " acpi=" USB_STR (STARBITE_USB_ACPI) \
                     " vbus=" USB_STR (STARBITE_USB_VBUS)

#define CACHE_TYPE_BITS  (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB | EFI_MEMORY_UCE)

//
// Gated blob file GUIDs = enable protocol GUIDs (tools/woa/ssdt-ffs.py --gated):
// 53534454-5342-4754-<Table ID>
//
STATIC EFI_GUID  mBlobXhc = {
  0x53534454, 0x5342, 0x4754, { 'S', 'B', 'U', 'S', 'B', 'X', 'H', 'C' }
};
STATIC EFI_GUID  mBlobUrs = {
  0x53534454, 0x5342, 0x4754, { 'S', 'B', 'U', 'S', 'B', 'U', 'R', 'S' }
};
STATIC EFI_GUID  mBlobUfn = {
  0x53534454, 0x5342, 0x4754, { 'S', 'B', 'U', 'S', 'B', 'U', 'F', 'N' }
};

STATIC
VOID
UsbDelay (
  IN unsigned  Us
  )
{
  MicroSecondDelay (Us);
}

STATIC
VOID
Enable (
  IN EFI_GUID     *Blob,
  IN CONST CHAR8  *Name
  )
{
  EFI_HANDLE  Handle;
  EFI_STATUS  Status;

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (&Handle, Blob, EFI_NATIVE_INTERFACE, NULL);
  SbLog ("=USB enable %a: %r", Name, Status);
}

//
// Nothing enabled: no USB device in ACPI at all.
//
STATIC
EFI_STATUS
Fail (
  IN CONST CHAR8  *Why
  )
{
  SbLog ("=USB FAIL %a (no USB blob enabled)", Why);
  return EFI_UNSUPPORTED;
}

//
// Make one register block accessible as uncached device memory, walking every GCD descriptor in
// [Base, Base + Size):
//   - not in the GCD: added as MMIO with UC, then set to UC (ArmCpuDxe maps it);
//   - MMIO that is already UC: left exactly as it is (its RUNTIME bit and mapping stay);
//   - MMIO with another cache type: only the cache-type bits change, everything else is kept;
//   - anything else (RAM, reserved): refused, nothing is touched.
//
STATIC
EFI_STATUS
MapMmio (
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  EFI_STATUS                       Status;
  UINT64                           Addr;
  UINT64                           End;
  UINT64                           ChunkEnd;
  UINT64                           Len;

  End = Base + Size;
  for (Addr = Base; Addr < End; Addr = ChunkEnd) {
    Status = gDS->GetMemorySpaceDescriptor (Addr, &Desc);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    ChunkEnd = MIN (End, Desc.BaseAddress + Desc.Length);
    Len      = ChunkEnd - Addr;
    if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Addr, Len, EFI_MEMORY_UC);
      if (!EFI_ERROR (Status)) {
        Status = gDS->SetMemorySpaceAttributes (Addr, Len, EFI_MEMORY_UC);
      }
    } else if (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) {
      Status = EFI_ACCESS_DENIED;
    } else if ((Desc.Attributes & CACHE_TYPE_BITS) == EFI_MEMORY_UC) {
      Status = EFI_SUCCESS;
    } else {
      Status = EFI_SUCCESS;
      if ((Desc.Capabilities & EFI_MEMORY_UC) == 0) {
        Status = gDS->SetMemorySpaceCapabilities (Addr, Len, Desc.Capabilities | EFI_MEMORY_UC);
      }

      if (!EFI_ERROR (Status)) {
        Status = gDS->SetMemorySpaceAttributes (Addr, Len, (Desc.Attributes & ~CACHE_TYPE_BITS) | EFI_MEMORY_UC);
      }
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UsbDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  struct s9usb_plat       P;
  struct s9usb_max        Max;
  struct s9usb_dwc3_info  Dwc;
  struct s9usb_xhci_info  Xhci;
  UINT32                  Snap[S9USB_SNAP_N];
  CHAR8                   Why[120];
  CONST CHAR8             *Reason;
  EFI_STATUS              Status;
  UINTN                   Index;
  int                     MaxUp;
  int                     Sink;
  int                     Routed;
  int                     VbusOn;
  int                     VbusAnswered;
  enum s9usb_role         Role;

  SbLog ("=USB v%d %a", USB_VERSION, USB_POLICY);

  for (Index = 0; Index < S9_NBLK; Index++) {
    Status = MapMmio (s9usb_phys[Index], s9usb_size[Index]);
    if (EFI_ERROR (Status)) {
      AsciiSPrint (Why, sizeof (Why), "map %lx+%x: %r", (UINT64)s9usb_phys[Index], s9usb_size[Index], Status);
      return Fail (Why);
    }

    P.base[Index] = (volatile uint8_t *)(UINTN)s9usb_phys[Index];
  }

  P.udelay = UsbDelay;

  //
  // As found: clocks and PMU (readable with the USB gates shut), then the gates opened and
  // USBCON read before the PHY init touches it.
  //
  s9usb_snapshot (&P, Snap, S9USB_SNAP_CLOCKS);
  SbLog (
    "=USB found top=%08x/%08x/%08x fsys=%08x/%08x/%08x/%08x pmu=%08x/%08x",
    Snap[0], Snap[1], Snap[2], Snap[3], Snap[4], Snap[5], Snap[6], Snap[12], Snap[13]
    );
  SbLog ("=USB found qch=%08x/%08x/%08x/%08x/%08x", Snap[7], Snap[8], Snap[9], Snap[10], Snap[11]);
  s9usb_clocks_on (&P);
  s9usb_snapshot (&P, Snap, S9USB_SNAP_N);
  SbLog (
    "=USB found usbcon ver=%08x link=%08x clkrst=%08x utmi=%08x hsp=%08x tune=%08x test=%08x",
    Snap[14], Snap[15], Snap[16], Snap[17], Snap[18], Snap[19], Snap[20]
    );

  //
  // The Type-C state decides the role in auto mode, and whether VBUS may be offered.
  //
  ZeroMem (&Max, sizeof (Max));
  MaxUp = s9usb_max_init (&P, &Max) && s9usb_max_status (&P, &Max);
  Sink  = 0;
  if (MaxUp) {
    Sink = s9usb_max_vbusdet (&Max) && (s9usb_max_ccstat (&Max) == S9USB_CC_SINK);
    SbLog (
      "=USB max hw=%02x fw=%02x usbc1=%02x bc=%02x cc0=%02x ccstat=%d vbusdet=%d",
      Max.hw, Max.fw, Max.usbc1, Max.bc, Max.cc0, s9usb_max_ccstat (&Max), s9usb_max_vbusdet (&Max)
      );
  } else {
    SbLog ("=USB max: no answer on HSI2C 0x14360000 (bus=%d i2c_errors=%d)", Max.bus, Max.errors);
  }

  if (STARBITE_USB_ROLE == 1) {
    Role   = S9USB_DEVICE;
    Reason = "build";
  } else if (STARBITE_USB_ROLE == 0) {
    Role   = S9USB_HOST;
    Reason = "build";
  } else if (Sink) {
    Role   = S9USB_DEVICE;
    Reason = "auto: a sink with VBUS (computer or charger)";
  } else {
    Role   = S9USB_HOST;
    Reason = MaxUp ? "auto: not a sink with VBUS" : "auto: MCU silent";
  }

  SbLog ("=USB role %a (%a)", Role == S9USB_HOST ? "host" : "device", Reason);

  //
  // Controller: PHY (the role's column), DWC3 core (the role), xHCI checks in host role.
  //
  if (s9usb_phy_init (&P, Role, &Reason) != 0) {
    AsciiSPrint (Why, sizeof (Why), "phy: %a", Reason);
    return Fail (Why);
  }

  s9usb_snapshot (&P, Snap, S9USB_SNAP_N);
  SbLog (
    "=USB phy link=%08x clkrst=%08x utmi=%08x hsp=%08x tune=%08x test=%08x line=%a",
    Snap[15], Snap[16], Snap[17], Snap[18], Snap[19], Snap[20], s9usb_phy_line (&P)
    );

  if (s9usb_dwc3_core_init (&P, Role, &Dwc, &Reason) != 0) {
    AsciiSPrint (Why, sizeof (Why), "dwc3 snpsid=%08x: %a", Dwc.snpsid, Reason);
    return Fail (Why);
  }

  SbLog (
    "=USB dwc3 snpsid=%08x gctl=%08x gusb2=%08x gusb3=%08x guctl=%08x guctl1=%08x bus0=%08x fladj=%08x%a",
    Dwc.snpsid, Dwc.gctl, Dwc.gusb2, Dwc.gusb3, Dwc.guctl, Dwc.guctl1, Dwc.gsbuscfg0, Dwc.gfladj,
    Dwc.was_host ? " (was a host: halted + core reset)" : ""
    );

  if (Role == S9USB_HOST) {
    if (s9usb_xhci_check (&P, &Xhci, &Reason) != 0) {
      AsciiSPrint (
        Why, sizeof (Why), "xhci caplen=%x hciver=%04x hcs1=%08x hcc1=%08x usbsts=%08x: %a",
        Xhci.caplen, Xhci.hciver, Xhci.hcs1, Xhci.hcc1, Xhci.usbsts, Reason
        );
      return Fail (Why);
    }

    SbLog (
      "=USB xhci caplen=%x hciver=%04x ports=%d ac64=%d usbsts=%08x usb2=%d+%d usb3=%d+%d",
      Xhci.caplen, Xhci.hciver, Xhci.ports, Xhci.ac64, Xhci.usbsts,
      Xhci.usb2_first, Xhci.usb2_count, Xhci.usb3_first, Xhci.usb3_count
      );
  }

  //
  // D+/D- to the AP, and VBUS: offered only in host role, and not when, at boot, something already
  // powers VBUS or the MCU sees the phone as a sink. After that the MCU decides alone (Type-C
  // rules) until its next reset.
  //
  if (MaxUp) {
    VbusOn = STARBITE_USB_VBUS && (Role == S9USB_HOST);
    if (VbusOn && (s9usb_max_vbusdet (&Max) || (s9usb_max_ccstat (&Max) == S9USB_CC_SINK))) {
      VbusOn = 0;
      SbLog ("=USB vbus policy 1 -> 0 for this boot: vbusdet=%d ccstat=%d", s9usb_max_vbusdet (&Max), s9usb_max_ccstat (&Max));
    }

    Routed       = s9usb_max_route_usb (&P, &Max);
    VbusAnswered = s9usb_max_auto_vbus (&P, &Max, VbusOn);
    SbLog (
      "=USB muic ctrl1=%02x (%a) op54 data=%02x answer=%02x %02x %02x %02x (%a) i2c_errors=%d",
      (UINT32)(Max.ctrl1 & 0xFF), Routed ? "routed" : "NOT routed", VbusOn ? 2 : 0,
      Max.answer[0], Max.answer[1], Max.answer[2], Max.answer[3],
      VbusAnswered ? "answered" : "no answer", Max.errors
      );
  }

  //
  // Which USB blob Windows gets (the others stay gated off).
  //
  if (STARBITE_USB_ACPI == 1) {
    Enable (&mBlobUrs, "SBUSBURS");
  } else if ((STARBITE_USB_ACPI == 0) && (Role == S9USB_HOST)) {
    Enable (&mBlobXhc, "SBUSBXHC");
  } else if ((STARBITE_USB_ACPI == 2) && (Role == S9USB_DEVICE)) {
    Enable (&mBlobUfn, "SBUSBUFN");
  } else {
    SbLog ("=USB no blob: layout %d has nothing for the %a role", STARBITE_USB_ACPI, Role == S9USB_HOST ? "host" : "device");
  }

  SbLog ("=USB ready, line=%a", s9usb_phy_line (&P));
  return EFI_SUCCESS;
}
