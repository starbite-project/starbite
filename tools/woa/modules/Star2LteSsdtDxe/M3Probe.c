/** @file
  M3Probe: do the Mongoose M3 cores run when started from UEFI? (docs/WIN11-BIGCORES.md stage B1)

  Part of Star2LteSsdtDxe when built with STARBITE_M3_PROBE=1; runs during DXE dispatch, before the
  release BDS parks the A55 secondaries. Nothing here is visible to Windows: the MADT, the PPTT and
  numproc stay as they are, and every M3 core that arrives powers itself off again.

  What is known (woa's PlatformBootManagerLib.c notes, boot-v20..v36, and our XNU patch 0304):
    - CPU_ON (0xC4000003) for an M3 target returns SUCCESS and FlexPMU marks the core up, but from
      woa's UEFI the core never left a visible mark; a SECOND CPU_ON to the same target never
      returns and the SoC needs a forced reset (the watchdog doesn't help).
    - XNU starts the M3s on this phone after six PMU writes (Samsung's pmucal_lpm_init entries, as
      the Linux S9+ port applies them before every M3 CPU_ON): CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG
      0x1088 [31:24] = 0x8D, MEMORY_CLUSTER1_CPU0..3_OPTION 0x2114/0x2180/0x21EC/0x2258 and
      MEMORY_CLUSTER1_NONCPU_OPTION 0x2384 [1:0] = 2. PSCI targets are 0x100 + n.
  So this probe applies the six writes, then issues exactly ONE CPU_ON per core, for M3 cores 1, 2
  and 3 (core 0 shares its affinity 0x100 with A55 core 1 and is not planned for Windows), and
  stops at the first core that doesn't check in within 200 ms (no second CPU_ON, ever).

  The witness stub (one reserved page below 4 GiB per core, MMU and caches off on entry): stores
  its own MPIDR and "M3OK" into its page, cleans the line to the point of coherency, then calls
  PSCI CPU_OFF (0x84000002); if that returns, it waits in WFE. The poller cleans and invalidates
  the line before each read. Logged as "=M3" lines (StarbiteLog.h).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "StarbiteLog.h"

#define PMU_BASE        0x14060000UL
#define PSCI_CPU_ON     0xC4000003UL
#define M3_OK           0x4D334F4Bu        // "KO3M" in memory order, reads 0x4D334F4B
#define POLL_MS         200u

STATIC CONST struct {
  UINT32    Offset, Mask, Value;
} mM3Pmu[] = {
  { 0x1088, 0xFF000000u, 0x8D000000u },     // CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG
  { 0x2114, 3, 2 },                         // MEMORY_CLUSTER1_CPU0_OPTION
  { 0x2180, 3, 2 },                         // ..._CPU1_OPTION
  { 0x21EC, 3, 2 },                         // ..._CPU2_OPTION
  { 0x2258, 3, 2 },                         // ..._CPU3_OPTION
  { 0x2384, 3, 2 },                         // MEMORY_CLUSTER1_NONCPU_OPTION
};

STATIC
UINT64
Smc (
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

  //
  // PSCI 0.1 firmware (older than SMCCC 1.1) may change x4..x17: clobbered, as shim/starlte/em.c
  // psci_call and XNU's 0300 declare them.
  //
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
CleanInvalidate (
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

//
// The witness stub at Page + 0x800; its page base is baked in (Samsung's EL3 doesn't deliver
// the PSCI context id). Marker at Page + 0x08, MPIDR at Page + 0x10.
//
STATIC
VOID
EmitStub (
  OUT UINT32  *Code,
  IN  UINT64  Page
  )
{
  UINTN  i;

  i         = 0;
  Code[i++] = 0xD2800001u | (((UINT32)(Page >> 0) & 0xFFFFu) << 5);              // MOVZ X1, #p0
  Code[i++] = 0xF2A00001u | (((UINT32)(Page >> 16) & 0xFFFFu) << 5);             // MOVK X1, #p1, LSL #16
  Code[i++] = 0xF2C00001u | (((UINT32)(Page >> 32) & 0xFFFFu) << 5);             // MOVK X1, #p2, LSL #32
  Code[i++] = 0xF2E00001u | (((UINT32)(Page >> 48) & 0xFFFFu) << 5);             // MOVK X1, #p3, LSL #48
  Code[i++] = 0xD53800A2u;                                                       // MRS  X2, MPIDR_EL1
  Code[i++] = 0xF9000822u;                                                       // STR  X2, [X1, #0x10]
  Code[i++] = 0x52800003u | ((M3_OK & 0xFFFFu) << 5);                            // MOVZ W3, #lo
  Code[i++] = 0x72A00003u | ((M3_OK >> 16) << 5);                                // MOVK W3, #hi, LSL #16
  Code[i++] = 0xB9000823u;                                                       // STR  W3, [X1, #0x08]
  Code[i++] = 0xD5033F9Fu;                                                       // DSB  SY
  Code[i++] = 0xD50B7E21u;                                                       // DC   CIVAC, X1
  Code[i++] = 0xD5033F9Fu;                                                       // DSB  SY
  Code[i++] = 0x52800040u;                                                       // MOVZ W0, #0x0002
  Code[i++] = 0x72B08000u;                                                       // MOVK W0, #0x8400, LSL #16
  Code[i++] = 0xD4000003u;                                                       // SMC  #0  (CPU_OFF)
  Code[i++] = 0xD503205Fu;                                                       // WFE
  Code[i++] = 0x17FFFFFFu;                                                       // B    .-4
}

//
// The six writes, before EVERY M3 CPU_ON as XNU 0304 and the Linux port do (the cluster may power
// down after a core's CPU_OFF and its option registers with it). Logged on the first pass only.
//
STATIC
VOID
M3PmuDefaults (
  IN BOOLEAN  Log
  )
{
  volatile UINT32  *Pmu;
  UINT32           Before;
  UINT32           After;
  UINTN            Index;

  Pmu = (volatile UINT32 *)PMU_BASE;
  for (Index = 0; Index < ARRAY_SIZE (mM3Pmu); Index++) {
    Before = Pmu[mM3Pmu[Index].Offset / 4];
    Pmu[mM3Pmu[Index].Offset / 4] = (Before & ~mM3Pmu[Index].Mask) | mM3Pmu[Index].Value;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    After = Pmu[mM3Pmu[Index].Offset / 4];
    if (Log || ((After & mM3Pmu[Index].Mask) != mM3Pmu[Index].Value)) {
      SbLog (
        "=M3 pmu %04x %08x -> %08x%a",
        mM3Pmu[Index].Offset,
        Before,
        After,
        ((After & mM3Pmu[Index].Mask) == mM3Pmu[Index].Value) ? "" : " NOT SET"
        );
    }
  }
}

VOID
M3Probe (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS  Page;
  volatile UINT32       *Marker;
  volatile UINT64       *Mpidr;
  UINT64                Rc;
  UINTN                 Core;
  UINTN                 Ms;
  EFI_STATUS            Status;

  SbLog ("=M3 probe v1: PMU defaults, then one CPU_ON per M3 core 1..3");

  for (Core = 1; Core <= 3; Core++) {
    Page   = 0xFFFFFFFFu;
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 1, &Page);
    if (EFI_ERROR (Status)) {
      SbLog ("=M3 core %d: no page (%r), stop", Core, Status);
      return;
    }

    ZeroMem ((VOID *)(UINTN)Page, EFI_PAGE_SIZE);
    EmitStub ((UINT32 *)(UINTN)(Page + 0x800), Page);
    CleanInvalidate ((UINTN)Page, EFI_PAGE_SIZE);
    __asm__ __volatile__ ("ic iallu\n dsb sy\n isb" ::: "memory");

    Marker = (volatile UINT32 *)(UINTN)(Page + 0x08);
    Mpidr  = (volatile UINT64 *)(UINTN)(Page + 0x10);
    M3PmuDefaults ((BOOLEAN)(Core == 1));
    Rc = Smc (PSCI_CPU_ON, 0x100 + Core, Page + 0x800, 0);
    for (Ms = 0; Ms < POLL_MS; Ms++) {
      CleanInvalidate ((UINTN)Page, 64);
      if (*Marker == M3_OK) {
        break;
      }
      MicroSecondDelay (1000);
    }

    SbLog (
      "=M3 core %d target %x: CPU_ON rc=%lx, %a after %d ms, mpidr=%lx",
      Core,
      0x100 + Core,
      Rc,
      (*Marker == M3_OK) ? "ARRIVED" : "no mark",
      Ms,
      *Mpidr
      );
    if ((Rc != 0) || (*Marker != M3_OK)) {
      SbLog ("=M3 stop: no further CPU_ON this boot");
      return;
    }

    //
    // Let the core finish its CPU_OFF before the next CPU_ON in the same cluster (PSCI 0.1: no
    // AFFINITY_INFO to ask; woa saw a power transaction in flight block a CPU_ON for good).
    //
    MicroSecondDelay (50000);
  }

  SbLog ("=M3 probe done: cores 1..3 ran and powered off again");
}
