/** @file
  M3Run: run a C function on an M3 core in the boot CPU's UEFI context (docs/WIN11-BIGCORES.md B3a).

  Part of Star2LteSsdtDxe when built with STARBITE_M3_RUN=1; during DXE dispatch. The step before
  B3 (Windows' boot CPU = an M3): prove that an M3 can take over the boot CPU's EL1 context (MMU on
  with UEFI's page tables, caches on, UEFI's vectors) and work coherently with the A55s, and read
  what B3/B4's MADT needs. Nothing persists: each M3 runs one test function, then PSCI CPU_OFF;
  the MADT, PPTT and Windows are untouched.

  Per M3 core 1, 2, 3 (one CPU_ON each, PMU defaults first, as B1):
    - a context (M3_RUN_CTX) with the A55's SCTLR, TCR, TTBR0, MAIR, CPACR, a 16 KiB stack of
      its own and the function; the stub (M3RunStub.S) in a boot-services *code* page (it keeps
      running there once the MMU is on, so the page must not be execute-never), with its own
      vector table: any exception on the M3 is recorded (ESR, ELR, FAR; Done = 3) and the core
      powers itself off, never left inside UEFI's handlers in memory Windows reuses;
    - the A55 raises to TPL_HIGH_LEVEL (no timer interrupt runs DXE code while the M3 shares the
      UEFI context; the M3 itself calls no boot services) and writes back its whole data cache
      (WriteBackDataCache: page tables, context, stack and stub reach memory even if the M3 were
      not coherent);
    - then a 4 KiB pattern written AFTER the write-back (dirty in the A55's cache only): the M3
      reads it, so a correct sum means the clusters are coherent A55 -> M3; the M3 writes the
      inverted pattern elsewhere and the A55 checks it (M3 -> A55, weaker: CPU_OFF also flushes);
    - the M3 reads its ID registers, CTR_EL0, CLIDR, SCTLR (MMU/caches really on), CNTFRQ, and
      GICD_ITARGETSR0 byte 0 (its own GIC CPU interface bit: the MADT's "CPU Interface Number");
    - Done = 1 (2: not entered at EL1; 3: an exception), CPU_OFF. The A55 polls Done for 1 s, then logs it all
      as "=M3 run" lines. The first core that fails stops the series.

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "StarbiteLog.h"

#ifndef STARBITE_M3_RUN
#define STARBITE_M3_RUN 0
#endif

#if STARBITE_M3_RUN
#include "M3RunStub.h"

#define PMU_BASE      0x14060000UL
#define GICD_BASE     0x10101000UL          // the MADT's GIC distributor (GICv2)
#define PSCI_CPU_ON   0xC4000003UL
#define PATTERN_WORDS 512u                  // 4 KiB
#define STACK_PAGES   4u

typedef struct {
  UINT64    Sctlr, Tcr, Ttbr0, Mair, Vbar, Cpacr, Sp, Fn;   // 0x00: set by the A55
  UINT64    El, Mpidr;                                     // 0x40: by the stub
  UINT32    Done, Pad;                                     // 0x50: by the stub
  UINT64    Esr, Elr, Far;                                 // 0x58: by its vectors (Done 3)
  //
  // By M3RunTest, on the M3:
  //
  UINT64    Midr, Ctr, Isar0, Isar1, Pfr0, Mmfr0, Mmfr1, Mmfr2, Clidr, SctlrNow, Cntfrq;
  UINT32    Itargets, Pad2;
  UINT64    *Pattern, *Back;
  UINT64    Sum;
} M3_RUN_CTX;

STATIC_ASSERT (OFFSET_OF (M3_RUN_CTX, Fn) == 0x38, "M3RunStub.S CTX_FN");
STATIC_ASSERT (OFFSET_OF (M3_RUN_CTX, El) == 0x40, "M3RunStub.S CTX_EL");
STATIC_ASSERT (OFFSET_OF (M3_RUN_CTX, Mpidr) == 0x48, "M3RunStub.S CTX_MPIDR");
STATIC_ASSERT (OFFSET_OF (M3_RUN_CTX, Done) == 0x50, "M3RunStub.S CTX_DONE");
STATIC_ASSERT (OFFSET_OF (M3_RUN_CTX, Far) == 0x68, "M3RunStub.S CTX_FAR");

STATIC CONST struct {
  UINT32    Offset, Mask, Value;
} mRunPmu[] = {
  { 0x1088, 0xFF000000u, 0x8D000000u },
  { 0x2114, 3, 2 }, { 0x2180, 3, 2 }, { 0x21EC, 3, 2 }, { 0x2258, 3, 2 }, { 0x2384, 3, 2 },
};

#define MRS(Reg)  ({ UINT64 _v; __asm__ __volatile__ ("mrs %0, " #Reg : "=r" (_v)); _v; })

STATIC
UINT64
RunSmc (
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
RunCivac (
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
// On the M3, MMU and caches on, the A55 waiting. No boot services, no logging, no globals written.
//
STATIC
VOID
M3RunTest (
  IN M3_RUN_CTX  *C
  )
{
  UINTN   i;
  UINT64  Sum;

  C->Midr     = MRS (midr_el1);
  C->Ctr      = MRS (ctr_el0);
  C->Isar0    = MRS (id_aa64isar0_el1);
  C->Isar1    = MRS (id_aa64isar1_el1);
  C->Pfr0     = MRS (id_aa64pfr0_el1);
  C->Mmfr0    = MRS (id_aa64mmfr0_el1);
  C->Mmfr1    = MRS (id_aa64mmfr1_el1);
  C->Mmfr2    = MRS (s3_0_c0_c7_2);          // ID_AA64MMFR2_EL1
  C->Clidr    = MRS (clidr_el1);
  C->SctlrNow = MRS (sctlr_el1);
  C->Cntfrq   = MRS (cntfrq_el0);
  C->Itargets = *(volatile UINT32 *)(GICD_BASE + 0x800) & 0xFF;

  for (Sum = 0, i = 0; i < PATTERN_WORDS; i++) {
    Sum        += C->Pattern[i];
    C->Back[i]  = ~C->Pattern[i];
  }

  C->Sum = Sum;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
RunPmuDefaults (
  VOID
  )
{
  volatile UINT32  *Pmu = (volatile UINT32 *)PMU_BASE;
  UINTN            i;

  for (i = 0; i < ARRAY_SIZE (mRunPmu); i++) {
    Pmu[mRunPmu[i].Offset / 4] = (Pmu[mRunPmu[i].Offset / 4] & ~mRunPmu[i].Mask) | mRunPmu[i].Value;
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

VOID
M3Run (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS  StubPage;
  EFI_PHYSICAL_ADDRESS  CtxPage;
  EFI_PHYSICAL_ADDRESS  Stack;
  M3_RUN_CTX            *C;
  UINT64                *Lit;
  UINT64                Rc;
  UINT64                Expect;
  UINTN                 Core;
  UINTN                 Ms;
  UINTN                 i;
  UINTN                 BackOk;
  EFI_TPL               OldTpl;

  SbLog (
    "=M3 run v1 (B3a): A55 ctr=%lx isar0=%lx isar1=%lx pfr0=%lx mmfr0=%lx mmfr1=%lx sctlr=%lx",
    MRS (ctr_el0),
    MRS (id_aa64isar0_el1),
    MRS (id_aa64isar1_el1),
    MRS (id_aa64pfr0_el1),
    MRS (id_aa64mmfr0_el1),
    MRS (id_aa64mmfr1_el1),
    MRS (sctlr_el1)
    );
  SbLog (
    "=M3 run A55 tcr=%lx mair=%lx ttbr0=%lx vbar=%lx cpacr=%lx gic itargets=%02x",
    MRS (tcr_el1),
    MRS (mair_el1),
    MRS (ttbr0_el1),
    MRS (vbar_el1),
    MRS (cpacr_el1),
    *(volatile UINT32 *)(GICD_BASE + 0x800) & 0xFF
    );

  StubPage = 0xFFFFFFFFu;
  CtxPage  = 0xFFFFFFFFu;
  Stack    = 0xFFFFFFFFu;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesCode, 1, &StubPage)) ||
      EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 3, &CtxPage)) ||
      EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, STACK_PAGES, &Stack)))
  {
    SbLog ("=M3 run: no memory, skip");
    return;
  }

  CopyMem ((VOID *)(UINTN)StubPage, mM3RunStub, sizeof (mM3RunStub));
  Lit = (UINT64 *)(UINTN)(StubPage + M3_RUN_STUB_LIT);
  C   = (M3_RUN_CTX *)(UINTN)CtxPage;
  *Lit = (UINT64)(UINTN)C;

  for (Core = 1; Core <= 3; Core++) {
    ZeroMem (C, 3 * EFI_PAGE_SIZE);
    C->Pattern = (UINT64 *)(UINTN)(CtxPage + EFI_PAGE_SIZE);
    C->Back    = (UINT64 *)(UINTN)(CtxPage + 2 * EFI_PAGE_SIZE);
    C->Sctlr   = MRS (sctlr_el1);
    C->Tcr     = MRS (tcr_el1);
    C->Ttbr0   = MRS (ttbr0_el1);
    C->Mair    = MRS (mair_el1);
    C->Vbar    = MRS (vbar_el1);
    C->Cpacr   = MRS (cpacr_el1);
    C->Sp      = (Stack + STACK_PAGES * EFI_PAGE_SIZE) & ~(UINT64)15;
    C->Fn      = (UINT64)(UINTN)M3RunTest;

    OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
    WriteBackDataCache ();
    InvalidateInstructionCacheRange ((VOID *)(UINTN)StubPage, EFI_PAGE_SIZE);
    for (Expect = 0, i = 0; i < PATTERN_WORDS; i++) {
      C->Pattern[i] = 0x5A5A000000000000ull | ((UINT64)Core << 32) | (i * 0x9E3779B1u);   // dirty in the A55 only
      Expect       += C->Pattern[i];
    }

    RunPmuDefaults ();
    Rc = RunSmc (PSCI_CPU_ON, 0x100 + Core, StubPage + M3_RUN_STUB_ENTRY, 0);
    for (Ms = 0; Ms < 1000; Ms++) {
      RunCivac ((UINTN)&C->Done, sizeof (C->Done));
      if (C->Done != 0) {
        break;
      }

      MicroSecondDelay (1000);
    }

    gBS->RestoreTPL (OldTpl);

    for (BackOk = 1, i = 0; i < PATTERN_WORDS; i++) {
      if (C->Back[i] != ~C->Pattern[i]) {
        BackOk = 0;
        break;
      }
    }

    SbLog (
      "=M3 run core %d target %x: CPU_ON rc=%lx, done=%d after %d ms, el=%ld, mpidr=%lx",
      Core,
      0x100 + Core,
      Rc,
      C->Done,
      Ms,
      C->El,
      C->Mpidr
      );
    if (C->Done == 3) {
      SbLog ("=M3 run core %d EXCEPTION esr=%lx elr=%lx far=%lx (powered itself off)", Core, C->Esr, C->Elr, C->Far);
    }

    if (C->Done != 1) {
      SbLog ("=M3 run stop: core %d didn't finish in the UEFI context; no further CPU_ON", Core);
      break;
    }

    SbLog (
      "=M3 run core %d ids: midr=%lx ctr=%lx isar0=%lx isar1=%lx pfr0=%lx mmfr0=%lx mmfr1=%lx mmfr2=%lx clidr=%lx",
      Core,
      C->Midr,
      C->Ctr,
      C->Isar0,
      C->Isar1,
      C->Pfr0,
      C->Mmfr0,
      C->Mmfr1,
      C->Mmfr2,
      C->Clidr
      );
    SbLog (
      "=M3 run core %d sctlr=%lx (M %d C %d I %d) cntfrq=%lx gic itargets=%02x, coherency A55->M3 %a, M3->A55 %a",
      Core,
      C->SctlrNow,
      (UINT32)(C->SctlrNow & 1),
      (UINT32)((C->SctlrNow >> 2) & 1),
      (UINT32)((C->SctlrNow >> 12) & 1),
      C->Cntfrq,
      C->Itargets,
      (C->Sum == Expect) ? "ok" : "STALE",
      BackOk ? "ok" : "STALE"
      );
    MicroSecondDelay (50000);
  }

  SbLog ("=M3 run done");
}
#endif
