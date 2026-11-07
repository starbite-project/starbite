/** @file
  Star2LteUsbHostDxe: the S9's DWC3 as a USB host for Windows (docs/WIN11-USB.md).

  At dispatch: maps the USB, clock and MAX77705 register blocks that the firmware's own page
  tables leave out, runs s9usb in host mode (clocks, the USB 2.0 PHY's host column, the DWC3 core
  with PRTCAPDIR = host), checks the xHCI read-only, routes D+/D- to the AP and sets the Type-C
  MCU's VBUS policy. Only when every controller check passed it arms a ReadyToBoot event that
  installs the SSDT describing \_SB.USB0 (PNP0D10) for Windows' inbox xHCI driver. Any failure:
  no SSDT, and Windows sees the same tables as without this module.

  The SSDT waits for ReadyToBoot at TPL_CALLBACK - 1 because the release's
  Star2LteGpioHandoffDxe (98100003-..., touch and buttons) checks at ReadyToBoot, TPL_CALLBACK,
  that the XSDT holds exactly its 4 expected tables and publishes nothing otherwise
  (GpioHandoffDxe.c "if(entries!=4)AUTH_FAIL(6)"). A lower TPL runs after every TPL_CALLBACK
  notification of the group, whatever the creation order.

  Mapping keeps what is there: a block already mapped as uncached MMIO (the PMU page carries
  EFI_MEMORY_RUNTIME for the release's ResetSystem) is left untouched; only blocks the GCD does
  not know are added, and only the cache-type bits are ever changed.

  Log: every line starts with "=USBH" and goes to the pmsg ring at 0xFED14000 (the format of
  tools/woa/patches/0003) and to DEBUG.

  Build-time VBUS policy STARBITE_USBH_VBUS: 0 = the MCU's auto-VBUS off (the phone never powers
  the port), 1 = on (Samsung's max77705_usbc_enable_auto_vbus: the MCU powers VBUS when it
  attaches as source), downgraded to off for this boot when something already powers VBUS or
  the phone is a sink at boot.

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/AcpiTable.h>

#include "s9usb/s9usb.h"
#include "UsbHostSsdtAml.h"

#ifndef STARBITE_USBH_VBUS
#define STARBITE_USBH_VBUS 0
#endif
#if STARBITE_USBH_VBUS != 0 && STARBITE_USBH_VBUS != 1
#error STARBITE_USBH_VBUS must be 0 (off) or 1 (MCU auto-VBUS)
#endif

#define USBH_VERSION  2
#define USBH_STR2(x)  #x
#define USBH_STR(x)   USBH_STR2 (x)
// also the marker tools/woa/build-win-usbhost.sh looks for in each variant's module
#define USBH_POLICY   "usbh-policy vbus=" USBH_STR (STARBITE_USBH_VBUS)

#define CACHE_TYPE_BITS  (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB | EFI_MEMORY_UCE)

//
// The pmsg ring (patch 0003's StarbiteRingWrite): header "DBGC", start, size; data at +12.
//
#define STARBITE_RING_BASE  0xFED14000UL
#define STARBITE_RING_CAP   0x3F00u

STATIC EFI_EVENT  mReadyEvent;

STATIC
VOID
RingWrite (
  IN CONST CHAR8  *Buffer,
  IN UINTN        NumberOfBytes
  )
{
  volatile UINT32  *W = (volatile UINT32 *)STARBITE_RING_BASE;
  volatile UINT8   *B = (volatile UINT8 *)(STARBITE_RING_BASE + 12);
  UINTN            Index;
  UINT32           Pos;

  if ((W[0] != 0x43474244u) || (W[1] >= STARBITE_RING_CAP) || (W[2] > STARBITE_RING_CAP)) {
    W[0] = 0x43474244u;
    W[1] = 0;
    W[2] = 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    if (W[2] < STARBITE_RING_CAP) {
      Pos  = W[2];
      W[2] = Pos + 1;
      W[1] = (Pos + 1) % STARBITE_RING_CAP;
    } else {
      Pos  = W[1];
      W[1] = (Pos + 1) % STARBITE_RING_CAP;
    }

    B[Pos] = (UINT8)Buffer[Index];
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
EFIAPI
Log (
  IN CONST CHAR8  *Format,
  ...
  )
{
  CHAR8    Line[200];
  VA_LIST  Args;
  UINTN    Len;

  VA_START (Args, Format);
  Len = AsciiVSPrint (Line, sizeof (Line) - 1, Format, Args);
  VA_END (Args);
  Line[Len++] = '\n';
  Line[Len]   = 0;
  RingWrite (Line, Len);
  DEBUG ((DEBUG_INFO, "%a", Line));
}

STATIC
VOID
UsbhDelay (
  IN unsigned  Us
  )
{
  MicroSecondDelay (Us);
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

//
// ReadyToBoot, TPL_CALLBACK - 1: after the release's GPIO handoff has checked and extended the
// tables. Once.
//
STATIC
VOID
EFIAPI
InstallSsdt (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTable;
  EFI_STATUS               Status;
  UINTN                    TableKey;

  gBS->CloseEvent (Event);
  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (!EFI_ERROR (Status)) {
    Status = AcpiTable->InstallAcpiTable (AcpiTable, UsbHostSsdtAml, sizeof (UsbHostSsdtAml), &TableKey);
  }

  if (EFI_ERROR (Status)) {
    Log ("=USBH FAIL ssdt at ReadyToBoot: %r", Status);
  } else {
    Log ("=USBH ssdt installed at ReadyToBoot (key %lx, %d bytes)", (UINT64)TableKey, (UINT32)sizeof (UsbHostSsdtAml));
  }
}

EFI_STATUS
EFIAPI
UsbHostDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  struct s9usb_plat       P;
  struct s9usb_max        Max;
  struct s9usb_dwc3_info  Dwc;
  struct s9usb_xhci_info  Xhci;
  UINT32                  Snap[S9USB_SNAP_N];
  CONST CHAR8             *Why;
  EFI_STATUS              Status;
  UINTN                   Index;
  int                     MaxUp;
  int                     Routed;
  int                     VbusOn;
  int                     VbusAnswered;

  Log ("=USBH v%d %a role=host", USBH_VERSION, USBH_POLICY);

  for (Index = 0; Index < S9_NBLK; Index++) {
    Status = MapMmio (s9usb_phys[Index], s9usb_size[Index]);
    if (EFI_ERROR (Status)) {
      Log ("=USBH FAIL map %lx+%x: %r (no SSDT)", (UINT64)s9usb_phys[Index], s9usb_size[Index], Status);
      return EFI_UNSUPPORTED;
    }

    P.base[Index] = (volatile uint8_t *)(UINTN)s9usb_phys[Index];
  }

  P.udelay = UsbhDelay;

  //
  // As found: clocks and PMU (readable with the USB gates shut), then the gates opened and
  // USBCON read before the PHY init touches it.
  //
  s9usb_snapshot (&P, Snap, S9USB_SNAP_CLOCKS);
  Log (
    "=USBH found top=%08x/%08x/%08x fsys=%08x/%08x/%08x/%08x pmu=%08x/%08x",
    Snap[0], Snap[1], Snap[2], Snap[3], Snap[4], Snap[5], Snap[6], Snap[12], Snap[13]
    );
  Log ("=USBH found qch=%08x/%08x/%08x/%08x/%08x", Snap[7], Snap[8], Snap[9], Snap[10], Snap[11]);
  s9usb_clocks_on (&P);
  s9usb_snapshot (&P, Snap, S9USB_SNAP_N);
  Log (
    "=USBH found usbcon ver=%08x link=%08x clkrst=%08x utmi=%08x hsp=%08x tune=%08x test=%08x",
    Snap[14], Snap[15], Snap[16], Snap[17], Snap[18], Snap[19], Snap[20]
    );

  //
  // The Type-C side, read only for now.
  //
  ZeroMem (&Max, sizeof (Max));
  MaxUp = s9usb_max_init (&P, &Max) && s9usb_max_status (&P, &Max);
  if (MaxUp) {
    Log (
      "=USBH max hw=%02x fw=%02x usbc1=%02x bc=%02x cc0=%02x ccstat=%d vbusdet=%d",
      Max.hw, Max.fw, Max.usbc1, Max.bc, Max.cc0, s9usb_max_ccstat (&Max), s9usb_max_vbusdet (&Max)
      );
  } else {
    Log ("=USBH max: no answer on HSI2C 0x14360000 (bus=%d i2c_errors=%d)", Max.bus, Max.errors);
  }

  //
  // Controller: PHY (host column), DWC3 core (host), xHCI checks.
  //
  if (s9usb_phy_init (&P, S9USB_HOST, &Why) != 0) {
    Log ("=USBH FAIL phy: %a (no SSDT)", Why);
    return EFI_UNSUPPORTED;
  }

  s9usb_snapshot (&P, Snap, S9USB_SNAP_N);
  Log (
    "=USBH phy host link=%08x clkrst=%08x utmi=%08x hsp=%08x tune=%08x test=%08x line=%a",
    Snap[15], Snap[16], Snap[17], Snap[18], Snap[19], Snap[20], s9usb_phy_line (&P)
    );

  if (s9usb_dwc3_core_init (&P, S9USB_HOST, &Dwc, &Why) != 0) {
    Log ("=USBH FAIL dwc3 snpsid=%08x: %a (no SSDT)", Dwc.snpsid, Why);
    return EFI_UNSUPPORTED;
  }

  Log (
    "=USBH dwc3 snpsid=%08x gctl=%08x gusb2=%08x gusb3=%08x guctl=%08x guctl1=%08x bus0=%08x fladj=%08x%a",
    Dwc.snpsid, Dwc.gctl, Dwc.gusb2, Dwc.gusb3, Dwc.guctl, Dwc.guctl1, Dwc.gsbuscfg0, Dwc.gfladj,
    Dwc.was_host ? " (was a host: halted + core reset)" : ""
    );

  if (s9usb_xhci_check (&P, &Xhci, &Why) != 0) {
    Log (
      "=USBH FAIL xhci caplen=%x hciver=%04x hcs1=%08x hcc1=%08x usbsts=%08x: %a (no SSDT)",
      Xhci.caplen, Xhci.hciver, Xhci.hcs1, Xhci.hcc1, Xhci.usbsts, Why
      );
    return EFI_UNSUPPORTED;
  }

  Log (
    "=USBH xhci caplen=%x hciver=%04x ports=%d ac64=%d usbsts=%08x usb2=%d+%d usb3=%d+%d",
    Xhci.caplen, Xhci.hciver, Xhci.ports, Xhci.ac64, Xhci.usbsts,
    Xhci.usb2_first, Xhci.usb2_count, Xhci.usb3_first, Xhci.usb3_count
    );

  //
  // D+/D- to the AP, and the VBUS policy. With policy 1, the MCU's auto-VBUS is still switched
  // off for this boot when VBUS is already powered (a charger, a computer, a back-powering hub)
  // or the MCU sees the phone as a sink: never a second source on the line.
  //
  if (MaxUp) {
    VbusOn = STARBITE_USBH_VBUS;
    if (VbusOn && (s9usb_max_vbusdet (&Max) || (s9usb_max_ccstat (&Max) == S9USB_CC_SINK))) {
      VbusOn = 0;
      Log ("=USBH vbus policy 1 -> 0 for this boot: vbusdet=%d ccstat=%d", s9usb_max_vbusdet (&Max), s9usb_max_ccstat (&Max));
    }

    Routed       = s9usb_max_route_usb (&P, &Max);
    VbusAnswered = s9usb_max_auto_vbus (&P, &Max, VbusOn);
    Log (
      "=USBH muic ctrl1=%02x (%a) op54 data=%02x answer=%02x %02x %02x %02x (%a) i2c_errors=%d",
      (UINT32)(Max.ctrl1 & 0xFF), Routed ? "routed" : "NOT routed", VbusOn ? 2 : 0,
      Max.answer[0], Max.answer[1], Max.answer[2], Max.answer[3],
      VbusAnswered ? "answered" : "no answer", Max.errors
      );
  }

  //
  // The SSDT, at ReadyToBoot after the release's GPIO handoff (see the file header).
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK - 1,
                  InstallSsdt,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mReadyEvent
                  );
  if (EFI_ERROR (Status)) {
    Log ("=USBH FAIL ReadyToBoot event: %r (no SSDT)", Status);
    return EFI_UNSUPPORTED;
  }

  Log ("=USBH ready, SSDT armed for ReadyToBoot, line=%a", s9usb_phy_line (&P));
  return EFI_SUCCESS;
}
