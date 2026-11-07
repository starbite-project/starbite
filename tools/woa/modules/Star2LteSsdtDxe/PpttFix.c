/** @file
  PpttFix: give the PPTT the CPU's caches (docs/WIN11-USB.md section 10). Part of
  Star2LteSsdtDxe (called first in its entry when built with STARBITE_PPTT_FIX=1), so the fix
  costs no extra module in the full DXE volume.

  The release's PPTT (the platform module's, woa Pptt.aslc) describes the package, the two
  clusters and the eight cores, but no caches. Windows then reports no cache descriptors, and code
  that divides by a cache line or size gets zero (suspected for the 0xC0000094 that x86
  OneDriveSetup and WerFault hit at every start since the install). This module, during DXE
  dispatch, before the blobs:
    1. reads the boot CPU's caches from the CPU itself: CLIDR_EL1 (which levels exist), then
       CSSELR_EL1/CCSIDR_EL1 per cache (line size, ways, sets; both CCSIDR formats, chosen by
       ID_AA64MMFR2_EL1.CCIDX). The boot CPU is an A55, the only kind Windows runs on here;
    2. finds the installed PPTT (EFI_ACPI_SDT_PROTOCOL), builds a copy with cache structures for
       every core of the boot CPU's cluster (PpttBuild.c: level 1 and 2 per core, level 3 and up
       shared, chained "next level of cache"; the M3 cores' nodes unchanged);
    3. uninstalls the old table and installs the new one; if that install fails the old table is
       installed again.
  Anything unexpected (no PPTT yet, a PPTT that already has caches, implausible geometry) leaves
  the table as it is. Every step is logged as an "=PPTT" line (StarbiteLog.h).

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
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/AcpiTable.h>

#include "StarbiteLog.h"
#include "PpttBuild.h"

#define PPTT_VERSION  1
#define PPTT_OUT_MAX  4096

STATIC
UINT64
ReadClidr (
  VOID
  )
{
  UINT64  V;

  __asm__ __volatile__ ("mrs %0, clidr_el1" : "=r" (V));
  return V;
}

STATIC
UINT64
ReadMmfr2 (
  VOID
  )
{
  UINT64  V;

  __asm__ __volatile__ ("mrs %0, id_aa64mmfr2_el1" : "=r" (V));
  return V;
}

STATIC
UINT64
ReadCcsidr (
  IN UINT64  Select
  )
{
  UINT64  Old;
  UINT64  V;

  __asm__ __volatile__ ("mrs %0, csselr_el1" : "=r" (Old));
  __asm__ __volatile__ ("msr csselr_el1, %1\n isb\n mrs %0, ccsidr_el1" : "=r" (V) : "r" (Select));
  __asm__ __volatile__ ("msr csselr_el1, %0\n isb" : : "r" (Old));
  return V;
}

//
// One cache's geometry from CCSIDR_EL1 (Select = (Level - 1) << 1 | InD).
//
STATIC
VOID
Geometry (
  IN  UINT32    Level,
  IN  UINT32    Kind,
  IN  BOOLEAN   Ccidx,
  OUT PB_CACHE  *C
  )
{
  UINT64  V;

  V           = ReadCcsidr (((UINT64)(Level - 1) << 1) | (Kind == PB_INSTRUCTION ? 1u : 0u));
  C->Level    = (pb_u8)Level;
  C->Kind     = (pb_u8)Kind;
  C->LineSize = (pb_u16)(1u << ((V & 7u) + 4));
  if (Ccidx) {
    C->Ways = (pb_u32)(((V >> 3) & 0x1FFFFFu) + 1);
    C->Sets = (pb_u32)(((V >> 32) & 0xFFFFFFu) + 1);
  } else {
    C->Ways = (pb_u32)(((V >> 3) & 0x3FFu) + 1);
    C->Sets = (pb_u32)(((V >> 13) & 0x7FFFu) + 1);
  }
}

VOID
PpttFix (
  VOID
  )
{
  PB_CACHE                 Caches[PB_MAX_CACHES];
  UINT32                   NumCaches;
  UINT64                   Clidr;
  BOOLEAN                  Ccidx;
  UINT32                   Level;
  UINT32                   Ctype;
  UINTN                    Index;
  EFI_ACPI_SDT_PROTOCOL    *Sdt;
  EFI_ACPI_TABLE_PROTOCOL  *AcpiTable;
  EFI_ACPI_SDT_HEADER      *Table;
  EFI_ACPI_TABLE_VERSION   Version;
  UINTN                    Key;
  UINTN                    NewKey;
  UINT8                    *Old;
  UINT8                    *New;
  UINT32                   OldLen;
  UINT32                   NewLen;
  CONST CHAR8              *Why;
  EFI_STATUS               Status;

  Clidr = ReadClidr ();
  Ccidx = (BOOLEAN)(((ReadMmfr2 () >> 20) & 0xFu) != 0);
  SbLog ("=PPTT v%d clidr=%lx ccidx=%d", PPTT_VERSION, Clidr, Ccidx);

  //
  // CLIDR_EL1 Ctype<n>, n = 1..7: 0 none, 1 instruction, 2 data, 3 separate, 4 unified.
  //
  NumCaches = 0;
  for (Level = 1; Level <= 7 && NumCaches + 2 <= PB_MAX_CACHES; Level++) {
    Ctype = (UINT32)((Clidr >> (3 * (Level - 1))) & 7u);
    if (Ctype == 0 || Ctype > 4) {
      break;
    }
    if (Ctype == 2 || Ctype == 3) {
      Geometry (Level, PB_DATA, Ccidx, &Caches[NumCaches++]);
    }
    if (Ctype == 1 || Ctype == 3) {
      Geometry (Level, PB_INSTRUCTION, Ccidx, &Caches[NumCaches++]);
    }
    if (Ctype == 4) {
      Geometry (Level, PB_UNIFIED, Ccidx, &Caches[NumCaches++]);
    }
  }

  for (Index = 0; Index < NumCaches; Index++) {
    SbLog (
      "=PPTT L%d %a line %d ways %d sets %d = %d KiB",
      Caches[Index].Level,
      Caches[Index].Kind == PB_DATA ? "data" : Caches[Index].Kind == PB_INSTRUCTION ? "instruction" : "unified",
      Caches[Index].LineSize,
      Caches[Index].Ways,
      Caches[Index].Sets,
      (UINT32)((UINT64)Caches[Index].LineSize * Caches[Index].Ways * Caches[Index].Sets / 1024)
      );
  }

  Status = gBS->LocateProtocol (&gEfiAcpiSdtProtocolGuid, NULL, (VOID **)&Sdt);
  if (!EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTable);
  }
  if (EFI_ERROR (Status)) {
    SbLog ("=PPTT skip: no ACPI protocols (%r)", Status);
    return;
  }

  Table = NULL;
  for (Index = 0; ; Index++) {
    Status = Sdt->GetAcpiTable (Index, &Table, &Version, &Key);
    if (EFI_ERROR (Status)) {
      Table = NULL;
      break;
    }
    if (Table->Signature == SIGNATURE_32 ('P', 'P', 'T', 'T')) {
      break;
    }
  }
  if (Table == NULL) {
    SbLog ("=PPTT skip: no PPTT installed yet (unchanged)");
    return;
  }

  OldLen = Table->Length;
  Old    = AllocateCopyPool (OldLen, Table);
  New    = AllocateZeroPool (PPTT_OUT_MAX);
  if ((Old == NULL) || (New == NULL)) {
    SbLog ("=PPTT skip: out of memory");
    return;
  }

  NewLen = PpttAddCaches (Old, OldLen, Caches, NumCaches, New, PPTT_OUT_MAX, &Why);
  if (NewLen == 0) {
    SbLog ("=PPTT skip: %a (unchanged, %d bytes)", Why, OldLen);
    return;
  }

  Status = AcpiTable->UninstallAcpiTable (AcpiTable, Key);
  if (EFI_ERROR (Status)) {
    SbLog ("=PPTT skip: uninstall %r (unchanged)", Status);
    return;
  }

  Status = AcpiTable->InstallAcpiTable (AcpiTable, New, NewLen, &NewKey);
  if (EFI_ERROR (Status)) {
    Status = AcpiTable->InstallAcpiTable (AcpiTable, Old, OldLen, &NewKey);
    SbLog ("=PPTT FAIL install of the new table; old one back: %r", Status);
    return;
  }

  SbLog ("=PPTT replaced: %d -> %d bytes (%d cache kinds: levels 1-2 per core, 3+ shared)", OldLen, NewLen, NumCaches);
}
