/** @file
  Star2LteSsdtDxe: installs the Starbite SSDT blobs found in the firmware volumes, during DXE
  dispatch (docs/WIN11-USB.md section 4.1). Shared by the USB and the display/GPU tracks: each
  ships a self-contained .aml, packed by tools/woa/ssdt-ffs.py.

  A blob is an FFS file of type FREEFORM whose name GUID is 53534454-5342-5445-<8 bytes> (plain)
  or 53534454-5342-4754-<8 bytes> (gated), the 8 bytes being the table's OEM Table ID; its RAW
  section is the table. Rules, per blob:
    - skipped unless: signature "SSDT", 36 <= Length <= the section size, OEM ID "STRBTE", the
      OEM Table ID equal to the GUID's last 8 bytes, not a Table ID already seen;
    - plain: installed at this module's entry unless a protocol whose GUID is the blob's file
      GUID is installed (a veto); a veto that comes later uninstalls it again;
    - gated: installed only once that protocol IS installed (its driver checked the hardware
      and enabled it): at entry if it already is, else from a protocol notification the moment
      it appears. A gated blob without its driver never appears: fails closed;
    - the checksum is recomputed, then EFI_ACPI_TABLE_PROTOCOL.InstallAcpiTable;
    - at most 32 blobs.
  So the order of dispatch between this module and the drivers doesn't matter.

  Why during dispatch and not at ReadyToBoot (v2 did ReadyToBoot, TPL_CALLBACK - 1): on the phone
  (2026-11-07, T1) none of v2's tables reached Windows, while the platform module's dispatch-time
  SSDT (RAMRET01, RWOA03E0) always does. Why v2's were lost is not proven (v2's log was
  overwritten): the release modules "P" and the PSCI EL2 shim work on the published tables at
  ReadyToBoot (they reference ReadyToBoot and the ACPI 2.0 configuration table, not the table
  protocol), or ReadyToBoot may not be signalled as expected. Dispatch is the path proven to reach
  Windows either way. The GPIO module's exactly-4-tables check that v2 waited for already fails on
  every boot (the platform installs 5), so dispatch costs nothing.

  Every decision is logged as an "=SSDT" line (StarbiteLog.h: pmsg ring, the zone-0 evidence ring,
  DEBUG).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/FirmwareVolume2.h>

#include "StarbiteLog.h"

#ifndef STARBITE_PPTT_FIX
#define STARBITE_PPTT_FIX 0
#endif

VOID
PpttFix (
  VOID
  );

#define SSDT_VERSION    3
#define SSDT_MAX_BLOBS  32

//
// 53534454-5342-5445-xxxx-xxxxxxxxxxxx: "SSDT", "SB", "TE" (Starbite), then the Table ID
//
#define BLOB_DATA1   0x53534454u
#define BLOB_DATA2   0x5342u
#define BLOB_DATA3   0x5445u      // plain
#define GATED_DATA3  0x4754u      // gated

typedef struct {
  EFI_GUID                       Name;
  EFI_ACPI_DESCRIPTION_HEADER    *Table;       // pool copy, kept for a later install
  BOOLEAN                        Gated;
  BOOLEAN                        Installed;
  UINTN                          Key;
  CHAR8                          Id[9];
  EFI_EVENT                      Event;
  VOID                           *Registration;
} BLOB;

STATIC EFI_ACPI_TABLE_PROTOCOL  *mAcpiTable;
STATIC BLOB                     mBlobs[SSDT_MAX_BLOBS];
STATIC UINTN                    mCount;

STATIC
BOOLEAN
IsBlob (
  IN CONST EFI_GUID  *Name
  )
{
  return (BOOLEAN)(Name->Data1 == BLOB_DATA1 && Name->Data2 == BLOB_DATA2 &&
                   (Name->Data3 == BLOB_DATA3 || Name->Data3 == GATED_DATA3));
}

//
// The Table ID as printable text (unprintable bytes as '.', trailing NULs/spaces dropped)
//
STATIC
VOID
TableIdText (
  IN  CONST UINT8  *Id,
  OUT CHAR8        Text[9]
  )
{
  UINTN  Index;
  UINTN  Len;

  Len = 0;
  for (Index = 0; Index < 8; Index++) {
    Text[Index] = (Id[Index] >= 0x20 && Id[Index] < 0x7F) ? (CHAR8)Id[Index] : '.';
    if (Id[Index] != 0 && Id[Index] != ' ') {
      Len = Index + 1;
    }
  }

  Text[Len] = 0;
}

STATIC
BOOLEAN
Flagged (
  IN BLOB  *Blob
  )
{
  VOID  *Flag;

  return (BOOLEAN)!EFI_ERROR (gBS->LocateProtocol (&Blob->Name, NULL, &Flag));
}

STATIC
VOID
Install (
  IN BLOB         *Blob,
  IN CONST CHAR8  *When
  )
{
  EFI_STATUS  Status;

  if (Blob->Installed) {
    return;
  }

  Blob->Table->Checksum = 0;
  Blob->Table->Checksum = CalculateCheckSum8 ((UINT8 *)Blob->Table, Blob->Table->Length);
  Status                = mAcpiTable->InstallAcpiTable (mAcpiTable, Blob->Table, Blob->Table->Length, &Blob->Key);
  if (EFI_ERROR (Status)) {
    SbLog ("=SSDT FAIL %a: InstallAcpiTable %r", Blob->Id, Status);
    return;
  }

  Blob->Installed = TRUE;
  SbLog (
    "=SSDT installed %a (%d bytes, rev %d%a, %a)",
    Blob->Id,
    Blob->Table->Length,
    Blob->Table->OemRevision,
    Blob->Gated ? ", gated" : "",
    When
    );
}

//
// The blob's protocol appeared: a gated blob is enabled, a plain one vetoed.
//
STATIC
VOID
EFIAPI
OnFlag (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  BLOB        *Blob;
  EFI_STATUS  Status;

  Blob = (BLOB *)Context;
  if (!Flagged (Blob)) {
    return;                         // defensive: RegisterProtocolNotify doesn't signal on its own
  }

  if (Blob->Gated) {
    Install (Blob, "enabled by its driver");
  } else if (Blob->Installed) {
    Status = mAcpiTable->UninstallAcpiTable (mAcpiTable, Blob->Key);
    if (!EFI_ERROR (Status)) {
      Blob->Installed = FALSE;
    }

    SbLog ("=SSDT vetoed %a after install: uninstalled %r", Blob->Id, Status);
  }

  gBS->CloseEvent (Event);
}

STATIC
VOID
Take (
  IN EFI_GUID  *Name,
  IN VOID      *Buffer,
  IN UINTN     BufferSize
  )
{
  EFI_ACPI_DESCRIPTION_HEADER  *Table;
  BLOB                         *Blob;
  CONST CHAR8                  *Skip;
  CHAR8                        Id[9];
  UINTN                        Index;
  EFI_STATUS                   Status;

  TableIdText (Name->Data4, Id);
  Table = (EFI_ACPI_DESCRIPTION_HEADER *)Buffer;
  Skip  = NULL;
  if ((BufferSize < sizeof (*Table)) || (Table->Length < sizeof (*Table)) || (Table->Length > BufferSize)) {
    Skip = "bad length";
  } else if (Table->Signature != EFI_ACPI_6_3_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
    Skip = "not an SSDT";
  } else if (CompareMem (Table->OemId, "STRBTE", 6) != 0) {
    Skip = "OEM ID is not STRBTE";
  } else if (CompareMem (&Table->OemTableId, Name->Data4, 8) != 0) {
    Skip = "Table ID differs from the file GUID";
  } else if (mCount == SSDT_MAX_BLOBS) {
    Skip = "more than 32 blobs";
  } else {
    for (Index = 0; Index < mCount; Index++) {
      if (mBlobs[Index].Table->OemTableId == Table->OemTableId) {
        Skip = "Table ID already seen";
      }
    }
  }

  if (Skip != NULL) {
    SbLog ("=SSDT skip %a: %a", Id, Skip);
    FreePool (Buffer);
    return;
  }

  Blob = &mBlobs[mCount++];
  CopyGuid (&Blob->Name, Name);
  CopyMem (Blob->Id, Id, sizeof (Id));
  Blob->Table     = Table;          // kept: a gated blob may be installed later
  Blob->Gated     = (BOOLEAN)(Name->Data3 == GATED_DATA3);
  Blob->Installed = FALSE;

  if (Blob->Gated ? Flagged (Blob) : !Flagged (Blob)) {
    Install (Blob, "at dispatch");
  } else {
    SbLog ("=SSDT %a %a at dispatch", Id, Blob->Gated ? "waits for its driver's enable" : "vetoed");
  }

  //
  // Gated and not yet installed: wait for the enable. Plain and installed: watch for a veto.
  //
  if ((Blob->Gated && !Blob->Installed) || (!Blob->Gated && Blob->Installed)) {
    Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnFlag, Blob, &Blob->Event);
    if (!EFI_ERROR (Status)) {
      Status = gBS->RegisterProtocolNotify (&Blob->Name, Blob->Event, &Blob->Registration);
    }

    if (EFI_ERROR (Status)) {
      SbLog ("=SSDT %a: no notification (%r)", Id, Status);
    }
  }
}

EFI_STATUS
EFIAPI
SsdtDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE                     *Handles;
  UINTN                          HandleCount;
  UINTN                          FvIndex;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *Fv;
  VOID                           *Key;
  EFI_FV_FILETYPE                FileType;
  EFI_GUID                       Name;
  EFI_FV_FILE_ATTRIBUTES         FileAttributes;
  UINTN                          FileSize;
  VOID                           *Buffer;
  UINTN                          BufferSize;
  UINT32                         AuthStatus;
  EFI_STATUS                     Status;
  UINTN                          Index;
  UINTN                          Installed;

  SbLog ("=SSDT v%d at dispatch, PPTT fix %d", SSDT_VERSION, STARBITE_PPTT_FIX);
  if (STARBITE_PPTT_FIX) {
    PpttFix ();
  }

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&mAcpiTable);
  if (EFI_ERROR (Status)) {
    SbLog ("=SSDT FAIL no ACPI table protocol: %r", Status);
    return Status;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    SbLog ("=SSDT FAIL no firmware volumes: %r", Status);
    return Status;
  }

  for (FvIndex = 0; FvIndex < HandleCount; FvIndex++) {
    Status = gBS->HandleProtocol (Handles[FvIndex], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Key = AllocateZeroPool (Fv->KeySize);
    if (Key == NULL) {
      break;
    }

    for ( ; ;) {
      FileType = EFI_FV_FILETYPE_FREEFORM;
      Status   = Fv->GetNextFile (Fv, Key, &FileType, &Name, &FileAttributes, &FileSize);
      if (EFI_ERROR (Status)) {
        break;
      }

      if (!IsBlob (&Name)) {
        continue;
      }

      Buffer     = NULL;
      BufferSize = 0;
      Status     = Fv->ReadSection (Fv, &Name, EFI_SECTION_RAW, 0, &Buffer, &BufferSize, &AuthStatus);
      if (EFI_ERROR (Status)) {
        SbLog ("=SSDT skip %g: no RAW section (%r)", &Name, Status);
        continue;
      }

      Take (&Name, Buffer, BufferSize);
    }

    FreePool (Key);
  }

  FreePool (Handles);
  Installed = 0;
  for (Index = 0; Index < mCount; Index++) {
    Installed += mBlobs[Index].Installed ? 1 : 0;
  }

  SbLog ("=SSDT dispatch done: %d blobs, %d installed now", (UINT32)mCount, (UINT32)Installed);
  return EFI_SUCCESS;
}
