/** @file
  Star2LteSsdtDxe: installs every Starbite SSDT blob found in the firmware volumes, at ReadyToBoot
  (docs/WIN11-USB.md section 4.1). Shared by the USB and the display/GPU tracks: each ships a
  self-contained .aml, packed by tools/woa/ssdt-ffs.py.

  A blob is an FFS file of type FREEFORM whose name GUID is 53534454-5342-5445-<8 bytes> (plain)
  or 53534454-5342-4754-<8 bytes> (gated), the 8 bytes being the table's OEM Table ID; its RAW
  section is the table. Rules, per blob:
    - skipped unless: signature "SSDT", 36 <= Length <= the section size, OEM ID "STRBTE", the
      OEM Table ID equal to the GUID's last 8 bytes, not a Table ID already installed;
    - plain: vetoed (skipped) when a protocol whose GUID is the blob's file GUID is installed (a
      driver that found its hardware dead installs a NULL interface under that GUID);
    - gated: installed only when that protocol IS installed (its driver checked the hardware and
      enabled it), so a gated blob without its driver never appears: fails closed;
    - the checksum is recomputed, then EFI_ACPI_TABLE_PROTOCOL.InstallAcpiTable;
    - at most 32 blobs.
  Every decision is logged as an "=SSDT" line to the pmsg ring at 0xFED14000 (patch 0003's format)
  and to DEBUG.

  Why ReadyToBoot at TPL_CALLBACK - 1: the release's Star2LteGpioHandoffDxe (touch) checks at
  ReadyToBoot, TPL_CALLBACK, that the XSDT holds exactly its 4 expected tables and publishes
  nothing otherwise (GpioHandoffDxe.c "if(entries!=4)AUTH_FAIL(6)"). A lower TPL runs after every
  TPL_CALLBACK notification of the group, so our tables come after its own.

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/FirmwareVolume2.h>

#define SSDT_MAX_BLOBS  32

//
// 53534454-5342-5445-xxxx-xxxxxxxxxxxx: "SSDT", "SB", "TE" (Starbite), then the Table ID
//
#define BLOB_DATA1  0x53534454u
#define BLOB_DATA2  0x5342u
#define BLOB_DATA3  0x5445u      // plain
#define GATED_DATA3 0x4754u      // gated

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
  CHAR8    Line[160];
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
VOID
EFIAPI
InstallBlobs (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_ACPI_TABLE_PROTOCOL        *AcpiTable;
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
  EFI_ACPI_DESCRIPTION_HEADER    *Table;
  UINT64                         Installed[SSDT_MAX_BLOBS];
  UINTN                          InstalledCount;
  UINTN                          Seen;
  UINTN                          Index;
  UINTN                          TableKey;
  VOID                           *Flag;
  BOOLEAN                        Gated;
  BOOLEAN                        Flagged;
  CHAR8                          Id[9];
  CONST CHAR8                    *Skip;
  EFI_STATUS                     Status;

  gBS->CloseEvent (Event);
  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  if (EFI_ERROR (Status)) {
    Log ("=SSDT FAIL no ACPI table protocol: %r", Status);
    return;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    Log ("=SSDT FAIL no firmware volumes: %r", Status);
    return;
  }

  InstalledCount = 0;
  Seen           = 0;
  for (FvIndex = 0; FvIndex < HandleCount && Seen < SSDT_MAX_BLOBS; FvIndex++) {
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

      if (++Seen > SSDT_MAX_BLOBS) {
        Log ("=SSDT more than %d blobs, the rest ignored", SSDT_MAX_BLOBS);
        break;
      }

      TableIdText (Name.Data4, Id);
      Buffer     = NULL;
      BufferSize = 0;
      Status     = Fv->ReadSection (Fv, &Name, EFI_SECTION_RAW, 0, &Buffer, &BufferSize, &AuthStatus);
      if (EFI_ERROR (Status)) {
        Log ("=SSDT skip %a: no RAW section (%r)", Id, Status);
        continue;
      }

      Table   = (EFI_ACPI_DESCRIPTION_HEADER *)Buffer;
      Skip    = NULL;
      Gated   = (BOOLEAN)(Name.Data3 == GATED_DATA3);
      Flagged = (BOOLEAN)!EFI_ERROR (gBS->LocateProtocol (&Name, NULL, &Flag));
      if ((BufferSize < sizeof (*Table)) || (Table->Length < sizeof (*Table)) || (Table->Length > BufferSize)) {
        Skip = "bad length";
      } else if (Table->Signature != EFI_ACPI_6_3_SECONDARY_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
        Skip = "not an SSDT";
      } else if (CompareMem (Table->OemId, "STRBTE", 6) != 0) {
        Skip = "OEM ID is not STRBTE";
      } else if (CompareMem (&Table->OemTableId, Name.Data4, 8) != 0) {
        Skip = "Table ID differs from the file GUID";
      } else if (!Gated && Flagged) {
        Skip = "vetoed by its driver";
      } else if (Gated && !Flagged) {
        Skip = "gated, not enabled by its driver";
      } else {
        for (Index = 0; Index < InstalledCount; Index++) {
          if (Installed[Index] == Table->OemTableId) {
            Skip = "Table ID already installed";
          }
        }
      }

      if (Skip == NULL) {
        Table->Checksum = 0;
        Table->Checksum = CalculateCheckSum8 ((UINT8 *)Table, Table->Length);
        Status          = AcpiTable->InstallAcpiTable (AcpiTable, Table, Table->Length, &TableKey);
        if (EFI_ERROR (Status)) {
          Log ("=SSDT FAIL %a: InstallAcpiTable %r", Id, Status);
        } else {
          Installed[InstalledCount++] = Table->OemTableId;
          Log ("=SSDT installed %a (%d bytes, rev %d%a)", Id, Table->Length, Table->OemRevision, Gated ? ", gated" : "");
        }
      } else {
        Log ("=SSDT skip %a: %a", Id, Skip);
      }

      FreePool (Buffer);
    }

    FreePool (Key);
  }

  FreePool (Handles);
  Log ("=SSDT done: %d installed of %d blobs", (UINT32)InstalledCount, (UINT32)MIN (Seen, SSDT_MAX_BLOBS));
}

EFI_STATUS
EFIAPI
SsdtDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK - 1,
                  InstallBlobs,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mReadyEvent
                  );
  Log ("=SSDT v2 armed for ReadyToBoot: %r", Status);
  return Status;
}
