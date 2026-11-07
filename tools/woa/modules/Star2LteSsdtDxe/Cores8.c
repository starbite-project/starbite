/** @file
  Cores8: the Mongoose M3 cores for Windows next to the four A55s (docs/WIN11-24H2-A55.md, "All 8 cores").

  Part of Star2LteSsdtDxe when built with STARBITE_CORES8=1 (M3 cores 1..3: 7 CPUs) or =2 (M3 cores 1..3, then
  core 0: 8 CPUs). Builds on the big-core stages B1/B2/B2b (docs/WIN11-BIGCORES.md):

  1. During DXE dispatch, before the release's PSCI EL2 shim installs (B3 v2 showed a CPU_ON issued after it never
     reaches our code), each M3 core gets one reserved page below 4 GiB as its ACPI parking-protocol mailbox, the
     parking stub (Cores8ParkStub.S) in the mailbox's firmware half, then XNU 0304's six PMU writes and exactly ONE
     PSCI CPU_ON (Samsung target 0x100 + n, entry = the stub). The first core that doesn't check in within 200 ms
     stops the sequence (never a second CPU_ON). Order 1, 2, 3, then 0: core 0 has never been started on this phone
     (woa: "CPU_ON 0x100 never returns"; B1 skipped it), so it goes last and only in the 8-core build.
  2. At ExitBootServices (after the release's "P" module published its MADT at ReadyToBoot), the live MADT is
     rewritten in place, same length: every parked M3's GICC entry (UID 4 + n, CPU interface 4 + n, MPIDR 0x100 + n
     as P wrote it) gets Enabled, parking protocol version 1 and its mailbox address; efficiency classes become
     A55 = 0, M3 = 1 (P has them the other way round; Windows' rule: a higher class is a faster core). The DSDT's
     processor devices CPU4..CPU7 carry a copy of their GICC entry in _MAT (woa's DSDT: disabled-era values, parking
     version 0): each parked M3's copy is replaced by the new entry, so the namespace and the MADT agree. Both
     checksums are recomputed. Nothing else changes; unparked M3s stay disabled.

  MPIDRs: P's MADT lists A55 n as 0x00n and M3 n as 0x10n. These are not the cores' real affinities (A55 n =
  0x81000n00, M3 n = 0x8000010n), but they are unique, and Windows already runs the A55s with P's values (it starts
  parked cores through the mailbox, by UID, and sends IPIs by GIC CPU interface number). So the A55 1 / M3 0
  collision of the real affinities (both 0x100) never reaches the table.

  Windows' own limit: the BCD's `numproc 4` keeps Windows on the first four GICC entries (the A55s). Until it is
  removed, the parked M3s just sleep through the session as in B2b.

  Logged as "=C8" lines in the zone-0 ring; each parked core has a line whose last digit its stub sets: 1 parked,
  2 taken by Windows.

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <Guid/Acpi.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "StarbiteLog.h"

#ifndef STARBITE_CORES8
#define STARBITE_CORES8 0
#endif
#ifndef STARBITE_M3_BOOT
#define STARBITE_M3_BOOT 0
#endif

//
// Big-core mode (STARBITE_M3_BOOT=3, docs/WIN11-24H2-A55.md "Windows on the 4 M3s"): M3Boot.c parks M3 core 1 and
// hands Windows' boot CPU to it at ExitBootServices; Cores8 parks only the other M3s (2, 3, and 0 with
// STARBITE_CORES8=2) and leaves the MADT to M3Boot, which reads the mailboxes through Cores8Mailbox().
//
#define C8_BIG  (STARBITE_M3_BOOT == 3)

#if STARBITE_CORES8
#include "Cores8ParkStub.h"

#define C8_PMU_BASE     0x14060000UL
#define C8_PSCI_CPU_ON  0xC4000003UL
#define C8_POLL_MS      200u
#define C8_UID_M3       4u                // the release MADT's GICC UIDs: A55 0..3, M3 4..7
#define C8_GICC_TYPE    0x0B
#define C8_GICC_LEN     80u

STATIC CONST struct {
  UINT32    Offset, Mask, Value;
} mC8Pmu[] = {
  { 0x1088, 0xFF000000u, 0x8D000000u },   // CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG
  { 0x2114, 3, 2 },                       // MEMORY_CLUSTER1_CPU0_OPTION
  { 0x2180, 3, 2 },                       // ..._CPU1_OPTION
  { 0x21EC, 3, 2 },                       // ..._CPU2_OPTION
  { 0x2258, 3, 2 },                       // ..._CPU3_OPTION
  { 0x2384, 3, 2 },                       // MEMORY_CLUSTER1_NONCPU_OPTION
};

STATIC UINT64     mMailbox[4];            // per M3 core: its mailbox page, 0 = not parked
#if !C8_BIG
STATIC EFI_EVENT  mC8Ebs;
#endif
STATIC EFI_GUID   mC8Acpi20 = EFI_ACPI_20_TABLE_GUID;

STATIC
UINT64
C8Smc (
  IN UINT64  Fn,
  IN UINT64  A1,
  IN UINT64  A2,
  IN UINT64  A3
  )
{
  register UINT64  x0 __asm__ ("x0") = Fn;
  register UINT64  x1 __asm__ ("x1") = A1;
  register UINT64  x2 __asm__ ("x2") = A2;
  register UINT64  x3 __asm__ ("x3") = A3;

  // pre-SMCCC-1.1 firmware may change x4..x17 (as M3Probe.c, shim/starlte/em.c)
  __asm__ __volatile__ (
    "smc #0"
    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
    :
    : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory"
    );
  return x0;
}

STATIC
VOID
C8CleanInvalidate (
  IN UINTN  Address,
  IN UINTN  Length
  )
{
  UINTN  A;

  for (A = Address & ~(UINTN)63; A < Address + Length; A += 64) {
    __asm__ __volatile__ ("dc civac, %0" : : "r" (A) : "memory");
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
C8PmuDefaults (
  IN BOOLEAN  Log
  )
{
  volatile UINT32  *Pmu;
  UINT32           Before;
  UINT32           After;
  UINTN            Index;

  Pmu = (volatile UINT32 *)C8_PMU_BASE;
  for (Index = 0; Index < ARRAY_SIZE (mC8Pmu); Index++) {
    Before                         = Pmu[mC8Pmu[Index].Offset / 4];
    Pmu[mC8Pmu[Index].Offset / 4] = (Before & ~mC8Pmu[Index].Mask) | mC8Pmu[Index].Value;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    After = Pmu[mC8Pmu[Index].Offset / 4];
    if (Log || ((After & mC8Pmu[Index].Mask) != mC8Pmu[Index].Value)) {
      SbLog (
        "=C8 pmu %04x %08x -> %08x%a",
        mC8Pmu[Index].Offset,
        Before,
        After,
        ((After & mC8Pmu[Index].Mask) == mC8Pmu[Index].Value) ? "" : " NOT SET"
        );
    }
  }
}

//
// The address of Needle's first character in the zone-0 ring (StarbiteLog.h: data at SB_EVLOG_BASE + 12, W[2]
// bytes, never wrapped), or 0.
//
STATIC
UINTN
C8RingFind (
  IN CONST CHAR8  *Needle
  )
{
  volatile UINT32  *W = (volatile UINT32 *)SB_EVLOG_BASE;
  volatile UINT8   *B = (volatile UINT8 *)(SB_EVLOG_BASE + 12);
  UINTN            Len;
  UINTN            Pos;
  UINTN            i;

  Len = AsciiStrLen (Needle);
  if ((W[0] != SB_DBGC) || (W[2] > SB_EVLOG_CAP) || (W[2] < Len)) {
    return 0;
  }

  for (Pos = W[2] - Len + 1; Pos-- > 0;) {
    for (i = 0; (i < Len) && (B[Pos + i] == (UINT8)Needle[i]); i++) {
    }

    if (i == Len) {
      return (UINTN)(SB_EVLOG_BASE + 12 + Pos);
    }
  }

  return 0;
}

STATIC
BOOLEAN
C8ParkOne (
  IN UINTN    Core,
  IN BOOLEAN  FirstPmuLog
  )
{
  EFI_PHYSICAL_ADDRESS  Page;
  volatile UINT64       *Arrived;
  UINT64                *Lit;
  UINT64                Rc;
  UINTN                 Ms;
  UINTN                 Base;
  EFI_STATUS            Status;
  CHAR8                 Line[96];

  Page   = 0xFFFFFFFFu;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 1, &Page);
  if (EFI_ERROR (Status)) {
    SbLog ("=C8 core %d: no page (%r)", Core, Status);
    return FALSE;
  }

  AsciiSPrint (Line, sizeof (Line), "=C8 park core %d uid %d mbox %08lx state 0", Core, C8_UID_M3 + Core, Page);
  SbLog ("%a", Line);
  Base = C8RingFind (Line);

  ZeroMem ((VOID *)(UINTN)Page, EFI_PAGE_SIZE);
  CopyMem ((VOID *)(UINTN)(Page + 0x800), mCores8ParkStub, sizeof (mCores8ParkStub));
  Lit    = (UINT64 *)(UINTN)(Page + 0x800 + CORES8_PARK_STUB_LIT);
  Lit[0] = Page;
  Lit[1] = C8_UID_M3 + Core;
  Lit[2] = (Base != 0) ? Base + AsciiStrLen (Line) - 1 : 0;      // the last character: the state digit
  C8CleanInvalidate ((UINTN)Page, EFI_PAGE_SIZE);
  __asm__ __volatile__ ("ic iallu\n dsb sy\n isb" ::: "memory");

  Arrived = (volatile UINT64 *)(UINTN)(Page + 0xFF8);
  C8PmuDefaults (FirstPmuLog);
  Rc = C8Smc (C8_PSCI_CPU_ON, 0x100 + Core, Page + 0x800, 0);
  for (Ms = 0; Ms < C8_POLL_MS; Ms++) {
    C8CleanInvalidate ((UINTN)(Page + 0xFF8), 8);
    if (*Arrived != 0) {
      break;
    }

    MicroSecondDelay (1000);
  }

  SbLog (
    "=C8 core %d target %x: CPU_ON rc=%lx, %a after %d ms, mpidr=%lx, digit %a",
    Core,
    0x100 + Core,
    Rc,
    (*Arrived != 0) ? "PARKED" : "no mark",
    Ms,
    *Arrived,
    (Base != 0) ? "found" : "NOT found"
    );
  if ((Rc != 0) || (*Arrived == 0)) {
    return FALSE;
  }

  mMailbox[Core] = Page;
  return TRUE;
}

STATIC
EFI_ACPI_DESCRIPTION_HEADER *
C8FindTable (
  IN UINT32  Signature
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER                   *Xsdt;
  EFI_ACPI_DESCRIPTION_HEADER                   *T;
  UINT64                                        Entry;
  UINTN                                         i;
  UINTN                                         n;

  Rsdp = NULL;
  for (i = 0; i < gST->NumberOfTableEntries; i++) {
    if (CompareGuid (&gST->ConfigurationTable[i].VendorGuid, &mC8Acpi20)) {
      Rsdp = gST->ConfigurationTable[i].VendorTable;
    }
  }

  if ((Rsdp == NULL) || (Rsdp->XsdtAddress == 0)) {
    return NULL;
  }

  Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  n    = (Xsdt->Length - sizeof (*Xsdt)) / 8;
  for (i = 0; i < n; i++) {
    CopyMem (&Entry, (UINT8 *)(Xsdt + 1) + 8 * i, 8);
    T = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Entry;
    if ((T != NULL) && (T->Signature == Signature)) {
      return T;
    }
  }

  return NULL;
}

STATIC UINT32 C8Rd32 (CONST UINT8 *p) { UINT32 v; CopyMem (&v, p, 4); return v; }
STATIC UINT64 C8Rd64 (CONST UINT8 *p) { UINT64 v; CopyMem (&v, p, 8); return v; }
STATIC VOID   C8Wr32 (UINT8 *p, UINT32 v) { CopyMem (p, &v, 4); }
#if !C8_BIG
STATIC VOID   C8Wr64 (UINT8 *p, UINT64 v) { CopyMem (p, &v, 8); }
#endif

STATIC
VOID
C8Checksum (
  IN OUT EFI_ACPI_DESCRIPTION_HEADER  *T
  )
{
  T->Checksum = 0;
  T->Checksum = CalculateCheckSum8 ((UINT8 *)T, T->Length);
}

//
// The DSDT's processor devices CPU0..CPU7 carry a copy of their GICC entry in _MAT (Buffer (0x50) { GICC }, woa's
// DSDT: located by the first 12 bytes, type 0x0B, length 0x50, interface = UID). Make every copy equal to the GICC
// with that UID in Madt (the table about to be or just published), then fix the DSDT checksum. Returns the number
// of copies written.
//
UINT32
Cores8SyncMat (
  IN CONST UINT8  *Madt,
  IN UINT32       MadtLen
  )
{
  EFI_ACPI_DESCRIPTION_HEADER  *Fadt;
  EFI_ACPI_DESCRIPTION_HEADER  *Dsdt;
  UINT8                        *D;
  CONST UINT8                  *E;
  UINT32                       Off;
  UINT32                       Uid;
  UINT32                       Pos;
  UINT32                       Mats;
  UINT8                        Key[12];

  Fadt = C8FindTable (SIGNATURE_32 ('F', 'A', 'C', 'P'));
  Dsdt = NULL;
  if (Fadt != NULL) {
    if ((Fadt->Length >= 148) && (C8Rd64 ((UINT8 *)Fadt + 140) != 0)) {
      Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)C8Rd64 ((UINT8 *)Fadt + 140);
    } else if (Fadt->Length >= 44) {
      Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)C8Rd32 ((UINT8 *)Fadt + 40);
    }
  }

  if ((Dsdt == NULL) || (Dsdt->Signature != SIGNATURE_32 ('D', 'S', 'D', 'T'))) {
    return 0;
  }

  D    = (UINT8 *)Dsdt;
  Mats = 0;
  for (Off = 44; Off + 2 <= MadtLen && Madt[Off + 1] != 0; Off += Madt[Off + 1]) {
    E = Madt + Off;
    if ((E[0] != C8_GICC_TYPE) || (E[1] != C8_GICC_LEN)) {
      continue;
    }

    Uid    = C8Rd32 (E + 8);
    Key[0] = C8_GICC_TYPE;
    Key[1] = C8_GICC_LEN;
    Key[2] = 0;
    Key[3] = 0;
    C8Wr32 (Key + 4, Uid);
    C8Wr32 (Key + 8, Uid);
    if ((Uid >= 8) || (C8Rd32 (E + 4) != Uid)) {
      continue;
    }

    for (Pos = sizeof (EFI_ACPI_DESCRIPTION_HEADER); Pos + C8_GICC_LEN <= Dsdt->Length; Pos++) {
      if (CompareMem (D + Pos, Key, sizeof (Key)) == 0) {
        CopyMem (D + Pos, E, C8_GICC_LEN);
        Mats++;
        break;
      }
    }
  }

  C8Checksum (Dsdt);
  return Mats;
}

//
// GICC (ACPI 6.0+, 80 bytes): +4 CPU interface number, +8 UID, +12 flags, +16 parking protocol version,
// +24 parked address, +68 MPIDR, +76 efficiency class.
//
#if !C8_BIG
STATIC
VOID
EFIAPI
C8OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_ACPI_DESCRIPTION_HEADER  *Madt;
  EFI_ACPI_DESCRIPTION_HEADER  *Fadt;
  EFI_ACPI_DESCRIPTION_HEADER  *Dsdt;
  UINT8                        *M;
  UINT8                        *E;
  UINT8                        *D;
  UINT8                        *Gicc[8];
  UINT32                       Off;
  UINT32                       Uid;
  UINT32                       Core;
  UINT32                       Enabled;
  UINT32                       Mats;
  UINT32                       Pos;
  UINT8                        Key[12];

  Madt = C8FindTable (SIGNATURE_32 ('A', 'P', 'I', 'C'));
  if ((Madt == NULL) || (Madt->Length < 44)) {
    SbLog ("=C8 EBS: no MADT, nothing changed");
    return;
  }

  ZeroMem (Gicc, sizeof (Gicc));
  M = (UINT8 *)Madt;
  for (Off = 44; Off + 2 <= Madt->Length && M[Off + 1] != 0; Off += M[Off + 1]) {
    E = M + Off;
    if ((E[0] == C8_GICC_TYPE) && (E[1] == C8_GICC_LEN)) {
      Uid = C8Rd32 (E + 8);
      if ((Uid < 8) && (C8Rd32 (E + 4) == Uid)) {
        Gicc[Uid] = E;
      }
    }
  }

  for (Uid = 0; Uid < 8; Uid++) {
    if (Gicc[Uid] == NULL) {
      SbLog ("=C8 EBS: MADT has no GICC with UID %d = interface %d: nothing changed", Uid, Uid);
      return;
    }
  }

  //
  // The M3s' entries: only the parked cores, and only if P left them as expected (disabled, MPIDR 0x100 + n).
  //
  Enabled = 0;
  for (Core = 0; Core < 4; Core++) {
    E = Gicc[C8_UID_M3 + Core];
    if (mMailbox[Core] == 0) {
      continue;
    }

    if (((C8Rd32 (E + 12) & 1) != 0) || (C8Rd64 (E + 68) != 0x100 + Core)) {
      SbLog ("=C8 EBS: M3 %d entry not as expected (flags %x mpidr %lx): left alone", Core, C8Rd32 (E + 12), C8Rd64 (E + 68));
      continue;
    }

    C8Wr32 (E + 12, C8Rd32 (E + 12) | 1);                         // Enabled
    C8Wr32 (E + 16, 1);                                           // parking protocol version 1
    C8Wr64 (E + 24, mMailbox[Core]);                              // parked address
    E[76] = 1;                                                    // efficiency class: faster
    Enabled++;
  }

  if (Enabled == 0) {
    SbLog ("=C8 EBS: no M3 parked, MADT untouched");
    return;
  }

  for (Uid = 0; Uid < 4; Uid++) {
    Gicc[Uid][76] = 0;                                            // A55: efficiency class 0
  }

  C8Checksum (Madt);
  for (Uid = 0; Uid < 8; Uid++) {
    E = Gicc[Uid];
    SbLog (
      "=C8 MADT uid=%d if=%d flags=%x mpidr=%lx park v%d %lx eff=%d",
      Uid,
      C8Rd32 (E + 4),
      C8Rd32 (E + 12),
      C8Rd64 (E + 68),
      C8Rd32 (E + 16),
      C8Rd64 (E + 24),
      E[76]
      );
  }

  //
  // The DSDT's _MAT copies (Buffer (0x50) { GICC }) for the enabled M3s: located by their first 12 bytes as woa's
  // DSDT has them (type 0x0B, length 0x50, interface = UID).
  //
  Mats = 0;
  Fadt = C8FindTable (SIGNATURE_32 ('F', 'A', 'C', 'P'));
  Dsdt = NULL;
  if (Fadt != NULL) {
    if ((Fadt->Length >= 148) && (C8Rd64 ((UINT8 *)Fadt + 140) != 0)) {
      Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)C8Rd64 ((UINT8 *)Fadt + 140);
    } else if (Fadt->Length >= 44) {
      Dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)C8Rd32 ((UINT8 *)Fadt + 40);
    }
  }

  if ((Dsdt == NULL) || (Dsdt->Signature != SIGNATURE_32 ('D', 'S', 'D', 'T'))) {
    SbLog ("=C8 EBS: no DSDT, _MAT not updated");
  } else {
    D = (UINT8 *)Dsdt;
    for (Core = 0; Core < 4; Core++) {
      if ((mMailbox[Core] == 0) || ((C8Rd32 (Gicc[C8_UID_M3 + Core] + 12) & 1) == 0)) {
        continue;
      }

      Uid    = C8_UID_M3 + Core;
      Key[0] = C8_GICC_TYPE;
      Key[1] = C8_GICC_LEN;
      Key[2] = 0;
      Key[3] = 0;
      C8Wr32 (Key + 4, Uid);
      C8Wr32 (Key + 8, Uid);
      for (Pos = sizeof (EFI_ACPI_DESCRIPTION_HEADER); Pos + C8_GICC_LEN <= Dsdt->Length; Pos++) {
        if (CompareMem (D + Pos, Key, sizeof (Key)) == 0) {
          CopyMem (D + Pos, Gicc[Uid], C8_GICC_LEN);
          Mats++;
          break;
        }
      }
    }

    C8Checksum (Dsdt);
  }

  SbLog ("=C8 EBS: %d M3 entries enabled (A55 eff 0, M3 eff 1), %d _MAT copies updated", Enabled, Mats);
}
#endif

UINT64
Cores8Mailbox (
  IN UINTN  Core
  )
{
  return (Core < 4) ? mMailbox[Core] : 0;
}

VOID
Cores8Arm (
  VOID
  )
{
 #if C8_BIG
  UINTN       Order[3] = { 2, 3, 0 };       // M3 core 1 is M3Boot's (the boot CPU)
 #else
  UINTN       Order[4] = { 1, 2, 3, 0 };
 #endif
  UINTN       Count;
  UINTN       Index;
  UINTN       Parked;
  EFI_STATUS  Status;

 #if C8_BIG
  Count = (STARBITE_CORES8 >= 2) ? 3 : 2;
  SbLog ("=C8 v1 big: M3 cores %a into parking stubs for M3Boot's MADT", (Count == 3) ? "2,3,0" : "2,3");
 #else
  Count = (STARBITE_CORES8 >= 2) ? 4 : 3;
  SbLog ("=C8 v1: M3 cores %a into parking stubs, MADT at ExitBootServices", (Count == 4) ? "1,2,3,0" : "1,2,3");
 #endif
  Parked = 0;
  for (Index = 0; Index < Count; Index++) {
    if (!C8ParkOne (Order[Index], (BOOLEAN)(Index == 0))) {
      SbLog ("=C8 stop: no further CPU_ON this boot");
      break;
    }

    Parked++;
    MicroSecondDelay (50000);
  }

  SbLog ("=C8 parked %d of %d M3 cores", Parked, Count);
  if (C8_BIG || (Parked == 0)) {
    return;
  }

#if !C8_BIG
  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, C8OnExitBootServices, NULL, &mC8Ebs);
  if (EFI_ERROR (Status)) {
    SbLog ("=C8 no ExitBootServices event (%r): MADT stays 4 CPUs", Status);
  }
#else
  Status = EFI_SUCCESS;
  (VOID)Status;
#endif
}
#endif
