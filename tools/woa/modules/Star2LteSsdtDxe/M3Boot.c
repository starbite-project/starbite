/** @file
  M3Boot: Windows boots on M3 core 1 (docs/WIN11-BIGCORES.md stage B3).

  Part of Star2LteSsdtDxe when built with STARBITE_M3_BOOT=1. Armed during dispatch; acts in an
  ExitBootServices event, when UEFI needs neither its timer nor interrupts any more, on A55 core 0:
    1. the live MADT (as the release's "P" module left it at ReadyToBoot) is logged, and a new
       version prepared in a buffer, not yet in the table:
         - M3 core 1's GICC entry first (UID 5, MPIDR 0x101), enabled, efficiency class 1;
         - A55 1..3 as P left them (their mailboxes, the release's parked cores), class 0;
         - A55 0 and M3 0/2/3 disabled.
       Anything unexpected (no M3 1 entry with MPIDR 0x101, an A55 1..3 entry without a mailbox)
       -> nothing happens, Windows boots on A55 core 0 as today;
    2. M3BootHandoff saves the A55's EL1 state, CPU_ONs M3 core 1 into M3BootStub.S and waits on
       Context.State without its stack: the M3 restores the state, claims (State 0 -> 1) and
       returns 1 from M3BootHandoff on the M3; the A55 then powers itself off (PSCI CPU_OFF). If
       no claim within 1 s, the A55 sets State 2 (the M3 powers off if it gets there later) and
       M3BootHandoff returns 0 on the A55;
    3. on the M3: the prepared MADT is copied into the table (the commit point), logged, and
       ExitBootServices returns into winload on the M3.
  `numproc 4` stays: Windows runs on M3 1 + A55 1..3.

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#ifndef STARBITE_M3_BOOT
#define STARBITE_M3_BOOT 0
#endif

#if STARBITE_M3_BOOT
#include "StarbiteLog.h"
#include "M3BootStub.h"

#define BOOT_UID        5u                  // M3 core 1 (the release MADT: A55 0..3, M3 4..7)
#define BOOT_MPIDR      0x101u
#define BOOT_TARGET     0x101u              // PSCI target of M3 core 1
#define TIMEOUT_TICKS   26000000u           // 1 s of the 26 MHz counter (CNTFRQ is 0 here)
#define MADT_MAX        1024u
#define GICC_TYPE       0x0B
#define GICC_LEN        80u

typedef struct {
  UINT64    X[12];                                   // 0x000 x19..x30
  UINT64    SpEl0, SpEl1, SpSel, Daif, Nzcv, Fpcr, Fpsr;            // 0x060
  UINT64    Sctlr, Tcr, Ttbr0, Ttbr1, Mair, Amair, Vbar, Cpacr;     // 0x098
  UINT64    ContextId, Tpidr0, Tpidr1, TpidrRo, Cntkctl;            // 0x0d8
  UINT32    State, Stage;                                           // 0x100 (Stage: 1 MMU off, 2 MMU on, 3 claimed)
  UINT64    El, Mpidr, Timeout, Rc, Esr, Elr, Far;                  // 0x108
  UINT64    Q[64];                                                  // 0x140 q0..q31
} M3_BOOT_CTX;

STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, SpEl0) == 0x060, "CTX_SP_EL0");
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, Sctlr) == 0x098, "CTX_SCTLR");
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, Cntkctl) == 0x0f8, "CTX_CNTKCTL");
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, State) == 0x100, "CTX_STATE");
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, Stage) == 0x104, "CTX_STAGE");
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, Timeout) == 0x118, "CTX_TIMEOUT");
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, Rc) == 0x120, "CTX_RC");
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, Far) == 0x138, "CTX_FAR");
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, Q) == 0x140, "CTX_Q0");
STATIC_ASSERT (sizeof (M3_BOOT_CTX) == 0x340, "M3_BOOT_CTX size");

STATIC EFI_GUID  mAcpi20Guid = {
  0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 }
};

STATIC M3_BOOT_CTX           *mCtx;
STATIC EFI_PHYSICAL_ADDRESS  mStub;
STATIC EFI_EVENT             mEbsEvent;
STATIC BOOLEAN               mFired;
STATIC UINT8                 *mMadt;
STATIC UINT32                mMadtLen;
STATIC UINT8                 mNewMadt[MADT_MAX];

STATIC CONST struct {
  UINT32    Offset, Mask, Value;
} mBootPmu[] = {
  { 0x1088, 0xFF000000u, 0x8D000000u },
  { 0x2114, 3, 2 }, { 0x2180, 3, 2 }, { 0x21EC, 3, 2 }, { 0x2258, 3, 2 }, { 0x2384, 3, 2 },
};

//
// A55 side. x0 = context, x1 = PSCI target, x2 = entry. Returns 1 on the M3 (via the stub), 0 on
// the A55 when the M3 didn't claim in time or CPU_ON failed (Context.Rc). After a claim the A55
// never returns: it powers off without touching its stack (the M3 is using it).
//
STATIC
__attribute__ ((naked))
UINT64
M3BootHandoff (
  IN M3_BOOT_CTX  *Ctx,
  IN UINT64       Target,
  IN UINT64       Entry
  )
{
  __asm__ __volatile__ (
    "stp   x19, x20, [x0, #0x00]\n"
    "stp   x21, x22, [x0, #0x10]\n"
    "stp   x23, x24, [x0, #0x20]\n"
    "stp   x25, x26, [x0, #0x30]\n"
    "stp   x27, x28, [x0, #0x40]\n"
    "stp   x29, x30, [x0, #0x50]\n"
    "mov   x9, sp\n"
    "mrs   x10, spsel\n"
    "str   x10, [x0, #0x70]\n"
    "cbz   x10, 1f\n"
    "str   x9, [x0, #0x68]\n"            // SPSel 1: sp is SP_EL1
    "mrs   x11, sp_el0\n"
    "str   x11, [x0, #0x60]\n"
    "b     2f\n"
    "1:\n"
    "str   x9, [x0, #0x60]\n"            // SPSel 0: sp is SP_EL0
    "msr   spsel, #1\n"
    "mov   x11, sp\n"
    "msr   spsel, #0\n"
    "str   x11, [x0, #0x68]\n"
    "2:\n"
    "mrs   x9, daif\n            str x9, [x0, #0x78]\n"
    "mrs   x9, nzcv\n            str x9, [x0, #0x80]\n"
    "mrs   x9, fpcr\n            str x9, [x0, #0x88]\n"
    "mrs   x9, fpsr\n            str x9, [x0, #0x90]\n"
    "mrs   x9, sctlr_el1\n       str x9, [x0, #0x98]\n"
    "mrs   x9, tcr_el1\n         str x9, [x0, #0xa0]\n"
    "mrs   x9, ttbr0_el1\n       str x9, [x0, #0xa8]\n"
    "mrs   x9, ttbr1_el1\n       str x9, [x0, #0xb0]\n"
    "mrs   x9, mair_el1\n        str x9, [x0, #0xb8]\n"
    "mrs   x9, amair_el1\n       str x9, [x0, #0xc0]\n"
    "mrs   x9, vbar_el1\n        str x9, [x0, #0xc8]\n"
    "mrs   x9, cpacr_el1\n       str x9, [x0, #0xd0]\n"
    "mrs   x9, contextidr_el1\n  str x9, [x0, #0xd8]\n"
    "mrs   x9, tpidr_el0\n       str x9, [x0, #0xe0]\n"
    "mrs   x9, tpidr_el1\n       str x9, [x0, #0xe8]\n"
    "mrs   x9, tpidrro_el0\n     str x9, [x0, #0xf0]\n"
    "mrs   x9, cntkctl_el1\n     str x9, [x0, #0xf8]\n"
    "add   x9, x0, #0x140\n"
    "stp   q0, q1, [x9, #0x000]\n   stp q2, q3, [x9, #0x020]\n"
    "stp   q4, q5, [x9, #0x040]\n   stp q6, q7, [x9, #0x060]\n"
    "stp   q8, q9, [x9, #0x080]\n   stp q10, q11, [x9, #0x0a0]\n"
    "stp   q12, q13, [x9, #0x0c0]\n stp q14, q15, [x9, #0x0e0]\n"
    "stp   q16, q17, [x9, #0x100]\n stp q18, q19, [x9, #0x120]\n"
    "stp   q20, q21, [x9, #0x140]\n stp q22, q23, [x9, #0x160]\n"
    "stp   q24, q25, [x9, #0x180]\n stp q26, q27, [x9, #0x1a0]\n"
    "stp   q28, q29, [x9, #0x1c0]\n stp q30, q31, [x9, #0x1e0]\n"
    "msr   daifset, #0xf\n"
    "str   wzr, [x0, #0x100]\n"           // State 0
    "mov   x9, x0\n"                      // the context to the point of coherency (the stub reads it MMU off)
    "add   x10, x0, #0x340\n"
    "3:\n"
    "dc    civac, x9\n"
    "add   x9, x9, #64\n"
    "cmp   x9, x10\n"
    "b.lo  3b\n"
    "dsb   sy\n"
    "mov   x19, x0\n"
    "mov   x0, #0x0003\n"                 // PSCI CPU_ON 0xC4000003 (x1 target, x2 entry)
    "movk  x0, #0xc400, lsl #16\n"
    "mov   x3, #0\n"
    "smc   #0\n"                          // x0..x17 clobbered
    "str   x0, [x19, #0x120]\n"
    "cbnz  x0, 7f\n"
    "mrs   x20, cntvct_el0\n"
    "ldr   x21, [x19, #0x118]\n"
    "add   x22, x19, #0x100\n"
    "4:\n"
    "ldr   w9, [x22]\n"
    "cmp   w9, #1\n"
    "b.eq  6f\n"
    "mrs   x9, cntvct_el0\n"
    "sub   x9, x9, x20\n"
    "cmp   x9, x21\n"
    "b.lo  4b\n"
    "5:\n"                                // timeout: State 0 -> 2, unless the M3 just claimed
    "ldaxr w9, [x22]\n"
    "cbnz  w9, 6f\n"
    "mov   w10, #2\n"
    "stlxr w11, w10, [x22]\n"
    "cbnz  w11, 5b\n"
    "b     7f\n"
    "6:\n"                                // claimed: the M3 is the boot CPU now; off, no stack
    "mov   x0, #0x0002\n"
    "movk  x0, #0x8400, lsl #16\n"
    "smc   #0\n"
    "8:\n"
    "wfe\n"
    "b     8b\n"
    "7:\n"                                // stay the boot CPU: undo what this function changed
    "ldp   x20, x21, [x19, #0x08]\n"
    "ldp   x22, x23, [x19, #0x18]\n"
    "ldr   x9, [x19, #0x78]\n"
    "msr   daif, x9\n"
    "mov   x0, #0\n"
    "ldr   x19, [x19, #0x00]\n"
    "ret\n"
    );
}

STATIC
EFI_ACPI_DESCRIPTION_HEADER *
FindMadt (
  VOID
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
    if (CompareGuid (&gST->ConfigurationTable[i].VendorGuid, &mAcpi20Guid)) {
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
    if ((T != NULL) && (T->Signature == SIGNATURE_32 ('A', 'P', 'I', 'C'))) {
      return T;
    }
  }

  return NULL;
}

STATIC UINT32 Rd32 (CONST UINT8 *p) { UINT32 v; CopyMem (&v, p, 4); return v; }
STATIC UINT64 Rd64 (CONST UINT8 *p) { UINT64 v; CopyMem (&v, p, 8); return v; }
STATIC VOID   Wr32 (UINT8 *p, UINT32 v) { CopyMem (p, &v, 4); }
STATIC VOID   Wr64 (UINT8 *p, UINT64 v) { CopyMem (p, &v, 8); }

//
// GICC (ACPI 6.0+, 80 bytes): +4 CPU interface number, +8 UID, +12 flags, +16 parking protocol
// version, +24 parked address, +68 MPIDR, +76 efficiency class.
//
STATIC
BOOLEAN
PrepareMadt (
  VOID
  )
{
  EFI_ACPI_DESCRIPTION_HEADER  *Madt;
  UINT8                        *M;
  UINT8                        *Boot;
  UINT8                        *E;
  UINT8                        *Out;
  UINT32                       Off;
  UINT32                       Uid;
  UINT32                       Mailboxes;
  UINT32                       Pass;

  Madt = FindMadt ();
  if ((Madt == NULL) || (Madt->Length > MADT_MAX) || (Madt->Length < 44)) {
    SbLog ("=M3 boot: no usable MADT");
    return FALSE;
  }

  M         = (UINT8 *)Madt;
  Boot      = NULL;
  Mailboxes = 0;
  for (Off = 44; Off + 2 <= Madt->Length && M[Off + 1] != 0; Off += M[Off + 1]) {
    if ((M[Off] != GICC_TYPE) || (M[Off + 1] != GICC_LEN)) {
      continue;
    }

    E   = M + Off;
    Uid = Rd32 (E + 8);
    SbLog (
      "=M3 boot MADT gicc uid=%d if=%d flags=%x mpidr=%lx park v%d %lx eff=%d",
      Uid,
      Rd32 (E + 4),
      Rd32 (E + 12),
      Rd64 (E + 68),
      Rd32 (E + 16),
      Rd64 (E + 24),
      E[76]
      );
    if ((Uid == BOOT_UID) && ((Rd64 (E + 68) & 0xFF00FFFFFFull) == BOOT_MPIDR)) {
      Boot = E;
    }

    if ((Uid >= 1) && (Uid <= 3) && (Rd32 (E + 12) & 1) && (Rd64 (E + 24) != 0)) {
      Mailboxes++;
    }
  }

  if ((Boot == NULL) || (Mailboxes != 3)) {
    SbLog ("=M3 boot: MADT not as expected (M3 1 entry %a, A55 1..3 mailboxes %d)", Boot ? "found" : "MISSING", Mailboxes);
    return FALSE;
  }

  //
  // The new table: header, the boot CPU's GICC, the other GICCs in their order, then the rest.
  //
  Out = mNewMadt;
  CopyMem (Out, M, 44);
  Out += 44;
  for (Pass = 0; Pass < 3; Pass++) {
    for (Off = 44; Off + 2 <= Madt->Length && M[Off + 1] != 0; Off += M[Off + 1]) {
      E = M + Off;
      if (Pass == 0) {
        if (E != Boot) {
          continue;
        }

        CopyMem (Out, E, GICC_LEN);
        Wr32 (Out + 12, Rd32 (Out + 12) | 1);       // enabled
        Wr32 (Out + 16, 0);                         // the boot CPU needs no mailbox
        Wr64 (Out + 24, 0);
        Out[76] = 1;                                // efficiency class: M3 above A55
      } else if (Pass == 1) {
        if ((E[0] != GICC_TYPE) || (E[1] != GICC_LEN) || (E == Boot)) {
          continue;
        }

        CopyMem (Out, E, GICC_LEN);
        Uid = Rd32 (E + 8);
        if ((Uid >= 1) && (Uid <= 3)) {
          Out[76] = 0;                              // A55 1..3: as P left them, class 0
        } else {
          Wr32 (Out + 12, Rd32 (Out + 12) & ~1u);   // A55 0, M3 0/2/3: disabled
        }
      } else {
        if ((E[0] == GICC_TYPE) && (E[1] == GICC_LEN)) {
          continue;
        }

        CopyMem (Out, E, E[1]);
      }

      Out += E[1];
    }
  }

  if ((UINT32)(Out - mNewMadt) != Madt->Length) {
    SbLog ("=M3 boot: new MADT %d bytes, old %d: not committing", (UINT32)(Out - mNewMadt), Madt->Length);
    return FALSE;
  }

  ((EFI_ACPI_DESCRIPTION_HEADER *)mNewMadt)->Checksum = 0;
  ((EFI_ACPI_DESCRIPTION_HEADER *)mNewMadt)->Checksum = CalculateCheckSum8 (mNewMadt, Madt->Length);
  mMadt    = M;
  mMadtLen = Madt->Length;
  return TRUE;
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  volatile UINT32  *Pmu = (volatile UINT32 *)0x14060000UL;
  UINT64           Ret;
  UINT64           Mpidr;
  UINT32           Before;
  UINT32           After;
  UINTN            i;
  UINTN            A;

  if (mFired) {
    return;
  }

  mFired = TRUE;
  if (!PrepareMadt ()) {
    SbLog ("=M3 boot: staying on A55 core 0 (Windows boots as before)");
    return;
  }

  SbLog ("=M3 boot v1: handing the boot CPU to M3 core 1 at ExitBootServices");
  for (i = 0; i < ARRAY_SIZE (mBootPmu); i++) {
    Before                      = Pmu[mBootPmu[i].Offset / 4];
    Pmu[mBootPmu[i].Offset / 4] = (Before & ~mBootPmu[i].Mask) | mBootPmu[i].Value;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    After = Pmu[mBootPmu[i].Offset / 4];
    SbLog ("=M3 pmu %04x %08x -> %08x%a", mBootPmu[i].Offset, Before, After, ((After & mBootPmu[i].Mask) == mBootPmu[i].Value) ? "" : " NOT SET");
  }

  ZeroMem (mCtx, sizeof (*mCtx));
  mCtx->Timeout = TIMEOUT_TICKS;
  WriteBackDataCache ();
  //
  // The stub page (code + literal) to the point of coherency by address: the M3 fetches it with
  // its MMU off (set/way and PoU cleaning were not enough in B3a v1). M3BootHandoff cleans the
  // context the same way after filling it.
  //
  for (A = (UINTN)mStub; A < (UINTN)mStub + EFI_PAGE_SIZE; A += 64) {
    __asm__ __volatile__ ("dc civac, %0" : : "r" (A) : "memory");
  }

  __asm__ __volatile__ ("dsb sy\n ic iallu\n dsb sy\n isb" ::: "memory");

  Ret = M3BootHandoff (mCtx, BOOT_TARGET, mStub + M3_BOOT_STUB_ENTRY);
  if (Ret == 1) {
    //
    // On M3 core 1 now; A55 core 0 is powering off. The commit point.
    //
    CopyMem (mMadt, mNewMadt, mMadtLen);
    __asm__ __volatile__ ("mrs %0, mpidr_el1" : "=r" (Mpidr));
    SbLog ("=M3 boot: now on mpidr=%lx (el %ld), MADT committed (UID %d first); winload continues here", Mpidr, mCtx->El, BOOT_UID);
    return;
  }

  for (A = (UINTN)mCtx; A < (UINTN)mCtx + 0x140; A += 64) {
    __asm__ __volatile__ ("dc civac, %0" : : "r" (A) : "memory");    // what the M3 wrote MMU off
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
  SbLog (
    "=M3 boot: hand-off not taken (CPU_ON rc=%lx, state=%d, stage=%d, el=%ld, mpidr=%lx, esr=%lx elr=%lx far=%lx); staying on A55 core 0, MADT untouched",
    mCtx->Rc,
    mCtx->State,
    mCtx->Stage,
    mCtx->El,
    mCtx->Mpidr,
    mCtx->Esr,
    mCtx->Elr,
    mCtx->Far
    );
}

VOID
M3BootArm (
  VOID
  )
{
  UINT64                *Lit;
  EFI_PHYSICAL_ADDRESS  CtxPage;
  EFI_STATUS            Status;

  mStub   = 0xFFFFFFFFu;
  CtxPage = 0xFFFFFFFFu;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesCode, 1, &mStub))) {
    SbLog ("=M3 boot: no stub page, not armed");
    return;
  }

  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1, &CtxPage))) {
    SbLog ("=M3 boot: no context page, not armed");
    return;
  }

  mCtx = (M3_BOOT_CTX *)(UINTN)CtxPage;

  CopyMem ((VOID *)(UINTN)mStub, mM3BootStub, sizeof (mM3BootStub));
  Lit  = (UINT64 *)(UINTN)(mStub + M3_BOOT_STUB_LIT);
  *Lit = (UINT64)(UINTN)mCtx;

  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, OnExitBootServices, NULL, &mEbsEvent);
  SbLog ("=M3 boot v1 armed: stub %lx, context %lx, ExitBootServices event %r", mStub, (UINT64)(UINTN)mCtx, Status);
}
#endif
