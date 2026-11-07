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
    2. M3BootHandoff saves the A55's EL1 state, CPU_ONs M3 core 1 into M3BootEntry and waits on
       Context.State without its stack: the M3 restores the state, claims (State 0 -> 1) and
       returns 1 from M3BootHandoff on the M3; the A55 then powers itself off (PSCI CPU_OFF). If
       no claim within 1 s, the A55 sets State 2 (the M3 powers off if it gets there later) and
       M3BootHandoff returns 0 on the A55;
    3. on the M3: the prepared MADT is copied into the table (the commit point), logged, and
       ExitBootServices returns into winload on the M3.
  `numproc 4` stays: Windows runs on M3 1 + A55 1..3.

  v3 (after v2 on the phone: CPU_ON rc=0 at ExitBootServices but the M3 never ran an instruction of
  ours): the release's PSCI EL2 shim installs during DXE (=PSCI2 ... hcr=80080002, PSCI_VERSION
  0xFFFFFFFF -> 0x10000) and from then on traps every SMC from EL1; its CPU_ON doesn't bring an M3
  into our code. B1/B2/B3a CPU_ON'd before it and worked. So M3 core 1 is CPU_ON'd at dispatch
  (only while PSCI_VERSION is still 0xFFFFFFFF) into a parking loop in reserved memory (M3ParkGo),
  and at ExitBootServices the A55 just sets Go and SEVs: no SMC. Note: the M3 then has no shim at
  EL2 under it; Windows' PSCI calls from it go to Samsung's EL3 directly (the next thing to watch).

  v2 (after B3 v1 on the phone: CPU_ON rc=0, no claim in 1 s): the M3's entry code and vectors are
  part of this driver's own .text (M3BootEntry below), which is mapped and executable in whatever
  translation tables are active at ExitBootServices, because the A55 is running from it at that moment.
  v1 used a separately allocated page. The breadcrumbs (Stage, EL, MPIDR, ESR/ELR/FAR) have a
  cache line of their own that the A55 never writes after cleaning it (v1's shared the line with
  State/Rc, which the A55 writes, so its write-back could erase them). The A55 also logs its
  TTBR0/TCR/SCTLR/MAIR at arming and at ExitBootServices, and the page-table entries of the entry
  code, the context and the stack.

  v4 (after B3 v3 solo on the phone, 2026-11-07: "now on mpidr=80000101 ... winload continues here", then a hang at the
  Windows logo and a watchdog reset; the woa wrapper's "=EBSOK", which it logs when ExitBootServices returns to
  winload, never came): the M3 hung inside the rest of ExitBootServices, i.e. in UEFI code (the event handlers after
  ours, the DXE core's tail: timer and GIC off). UEFI drives the PHYSICAL generic timer (ArmGenericTimerPhyCounterLib)
  and the M3's EL2 timer controls were set up by EL3, not by S-Boot, so CNTP_* from EL1 may trap there. Two changes:
    - the hand-off now happens AFTER the real ExitBootServices has returned: gBS->ExitBootServices is wrapped at
      dispatch, every handler and the DXE core's tail run on the A55, and only the return path (the woa wrapper's
      "=EBSOK", then winload) runs on the M3. After the real call gBS is gone (edk2 zeroes it): the log writes
      without boot services (mSbAfterEbs) and nothing here calls one;
    - solo builds first probe M3 core 2 (idle there) at dispatch: one CPU_ON into a stub that reads/writes
      CNTVCT, CNTPCT, CNTP_CTL, CNTV_CTL, CNTP_CVAL with a breadcrumb after each ("=M3 tprobe"), then waits in WFE.
      A crumb short of 8 names the access that trapped (to EL2) on an M3.

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Guid/EventGroup.h>

#ifndef STARBITE_M3_BOOT
#define STARBITE_M3_BOOT 0
#endif

#if STARBITE_M3_BOOT
#include "StarbiteLog.h"

//
// STARBITE_M3_BOOT=1: M3 1 + A55 1..3 (4 CPUs); =2 ("solo"): M3 1 alone, every other GICC disabled,
// to split "Windows on an M3" from "an M3 boot CPU with A55 secondaries" if 1 fails.
//
#define BOOT_SOLO       (STARBITE_M3_BOOT == 2)
//
// =3 ("big", docs/WIN11-24H2-A55.md "Windows on the 4 M3s"): M3 1 boots, the M3s Cores8.c parked at dispatch (2, 3,
// and 0 with STARBITE_CORES8=2) are its secondaries through their parking-protocol mailboxes, all four A55s are
// disabled (a mixed A55 + M3 Windows runs LSE atomics on the M3s), every efficiency class 0 (one kind of core).
// The DSDT's _MAT copies are made equal to the new MADT at the commit point (Cores8SyncMat).
//
#define BOOT_BIG        (STARBITE_M3_BOOT == 3)
#define BOOT_UID        5u                  // M3 core 1 (the release MADT: A55 0..3, M3 4..7)
#define BOOT_MPIDR      0x101u
#define BOOT_TARGET     0x101u              // PSCI target of M3 core 1
#define TIMEOUT_TICKS   26000000u           // 1 s of the 26 MHz counter (CNTFRQ is 0 here)
#define MADT_MAX        1024u
#define GICC_TYPE       0x0B
#define GICC_LEN        80u

#if BOOT_BIG
UINT64
Cores8Mailbox (
  IN UINTN  Core
  );

UINT32
Cores8SyncMat (
  IN CONST UINT8  *Madt,
  IN UINT32       MadtLen
  );
#endif

typedef struct {
  UINT64    X[12];                                   // 0x000 x19..x30
  UINT64    SpEl0, SpEl1, SpSel, Daif, Nzcv, Fpcr, Fpsr;            // 0x060
  UINT64    Sctlr, Tcr, Ttbr0, Ttbr1, Mair, Amair, Vbar, Cpacr;     // 0x098
  UINT64    ContextId, Tpidr0, Tpidr1, TpidrRo, Cntkctl;            // 0x0d8
  UINT32    State, Stage;                                           // 0x100 (Stage: 1 MMU off, 2 MMU on, 3 claimed)
  UINT64    El, Mpidr, Timeout, Rc, Esr, Elr, Far;                  // 0x108
  UINT64    Q[64];                                                  // 0x140 q0..q31
  UINT64    Pad[8];                                                 // 0x340
  //
  // 0x380, a cache line of its own: written only by the M3 (MMU off or on); the A55 cleans it
  // before CPU_ON and only reads it (after clean+invalidate) when it gives up.
  //
  UINT64    DStage, DEl, DMpidr, DEsr, DElr, DFar;                  // 0x380
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
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, DStage) == 0x380, "CTX_DSTAGE");
STATIC_ASSERT (OFFSET_OF (M3_BOOT_CTX, DFar) == 0x3a8, "CTX_DFAR");
STATIC_ASSERT (sizeof (M3_BOOT_CTX) <= 0x400, "M3_BOOT_CTX size");

//
// The M3's side, in this image's .text (v2). Entered by PSCI CPU_ON: MMU and caches off, DAIF
// masked, SPSel 1. gM3BootCtx (this image's .data, cleaned to PoC before CPU_ON) holds the context.
//   DStage 1: running, MMU off; not EL1 -> CPU_OFF.
//   VBAR = M3BootVectors (any exception before the claim: ESR/ELR/FAR into the context, CPU_OFF);
//   MAIR, TCR, TTBR0/1, AMAIR, CPACR, CONTEXTIDR, CNTKCTL, TPIDRs; SCTLR (MMU and caches on).
//   DStage 2 (+ EL, MPIDR again, cacheable); claim State 0 -> 1 with exclusives (2 = the A55 gave up:
//   CPU_OFF). DStage 3: UEFI's VBAR, FPCR/FPSR, q0-q31, SPs/SPSel, NZCV, x19-x30, DAIF; return 1 to
//   M3BootHandoff's caller.
//
UINT64  gM3BootCtx __attribute__ ((used));

extern UINT8  M3BootVectors[];
extern UINT8  M3BootCatchVectors[];
extern UINT8  M3BootEntry[];
extern UINT8  M3BootEnd[];

__asm__ (
  "  .text\n"
  "  .p2align 11\n"
  "  .globl M3BootVectors\n"
  "M3BootVectors:\n"
  "  .rept 16\n"
  "  b     M3BootVfail\n"
  "  .p2align 7\n"
  "  .endr\n"
  "  .globl M3BootEntry\n"
  "M3BootEntry:\n"
  "  adrp  x1, gM3BootCtx\n"
  "  ldr   x19, [x1, :lo12:gM3BootCtx]\n"
  "  mov   x0, #1\n"
  "  str   x0, [x19, #0x380]\n"          // DStage 1
  "  mrs   x0, CurrentEL\n"
  "  lsr   x0, x0, #2\n"
  "  str   x0, [x19, #0x388]\n"
  "  mrs   x2, mpidr_el1\n"
  "  str   x2, [x19, #0x390]\n"
  "  dsb   sy\n"
  "  cmp   x0, #1\n"
  "  b.ne  M3BootOff\n"
  "  tlbi  vmalle1\n"
  "  dsb   nsh\n"
  "  ic    iallu\n"
  "  dsb   nsh\n"
  "  isb\n"
  "  adr   x0, M3BootVectors\n"
  "  msr   vbar_el1, x0\n"
  "  ldr   x0, [x19, #0xb8]\n  msr mair_el1, x0\n"
  "  ldr   x0, [x19, #0xa0]\n  msr tcr_el1, x0\n"
  "  ldr   x0, [x19, #0xa8]\n  msr ttbr0_el1, x0\n"
  "  ldr   x0, [x19, #0xb0]\n  msr ttbr1_el1, x0\n"
  "  ldr   x0, [x19, #0xc0]\n  msr amair_el1, x0\n"
  "  ldr   x0, [x19, #0xd0]\n  msr cpacr_el1, x0\n"
  "  ldr   x0, [x19, #0xd8]\n  msr contextidr_el1, x0\n"
  "  ldr   x0, [x19, #0xf8]\n  msr cntkctl_el1, x0\n"
  "  ldr   x0, [x19, #0xe0]\n  msr tpidr_el0, x0\n"
  "  ldr   x0, [x19, #0xe8]\n  msr tpidr_el1, x0\n"
  "  ldr   x0, [x19, #0xf0]\n  msr tpidrro_el0, x0\n"
  "  isb\n"
  "  ldr   x0, [x19, #0x98]\n"
  "  msr   sctlr_el1, x0\n"
  "  isb\n"
  "  mov   x0, #2\n"
  "  str   x0, [x19, #0x380]\n"          // DStage 2, EL and MPIDR again (cacheable now)
  "  mrs   x0, CurrentEL\n"
  "  lsr   x0, x0, #2\n"
  "  str   x0, [x19, #0x388]\n"
  "  mrs   x0, mpidr_el1\n"
  "  str   x0, [x19, #0x390]\n"
  "  add   x2, x19, #0x100\n"            // claim: State 0 -> 1
  "1:\n"
  "  ldaxr w0, [x2]\n"
  "  cbnz  w0, M3BootOff\n"
  "  mov   w1, #1\n"
  "  stlxr w3, w1, [x2]\n"
  "  cbnz  w3, 1b\n"
  "  dmb   ish\n"
  "  mov   x0, #3\n"
  "  str   x0, [x19, #0x380]\n"          // DStage 3: claimed
  "  ldr   x0, [x19, #0xc8]\n  msr vbar_el1, x0\n"
  "  ldr   x0, [x19, #0x88]\n  msr fpcr, x0\n"
  "  ldr   x0, [x19, #0x90]\n  msr fpsr, x0\n"
  "  add   x1, x19, #0x140\n"
  "  ldp   q0, q1, [x1, #0x000]\n   ldp q2, q3, [x1, #0x020]\n"
  "  ldp   q4, q5, [x1, #0x040]\n   ldp q6, q7, [x1, #0x060]\n"
  "  ldp   q8, q9, [x1, #0x080]\n   ldp q10, q11, [x1, #0x0a0]\n"
  "  ldp   q12, q13, [x1, #0x0c0]\n ldp q14, q15, [x1, #0x0e0]\n"
  "  ldp   q16, q17, [x1, #0x100]\n ldp q18, q19, [x1, #0x120]\n"
  "  ldp   q20, q21, [x1, #0x140]\n ldp q22, q23, [x1, #0x160]\n"
  "  ldp   q24, q25, [x1, #0x180]\n ldp q26, q27, [x1, #0x1a0]\n"
  "  ldp   q28, q29, [x1, #0x1c0]\n ldp q30, q31, [x1, #0x1e0]\n"
  "  msr   spsel, #1\n"
  "  ldr   x0, [x19, #0x68]\n"
  "  mov   sp, x0\n"
  "  ldr   x0, [x19, #0x60]\n"
  "  msr   sp_el0, x0\n"
  "  ldr   x0, [x19, #0x70]\n"
  "  msr   spsel, x0\n"
  "  isb\n"
  "  ldr   x0, [x19, #0x80]\n  msr nzcv, x0\n"
  "  ldp   x20, x21, [x19, #0x08]\n"
  "  ldp   x22, x23, [x19, #0x18]\n"
  "  ldp   x24, x25, [x19, #0x28]\n"
  "  ldp   x26, x27, [x19, #0x38]\n"
  "  ldp   x28, x29, [x19, #0x48]\n"
  "  ldr   x30, [x19, #0x58]\n"
  "  ldr   x18, [x19, #0x340]\n"
  "  ldr   x0, [x19, #0x78]\n"
  "  msr   daif, x0\n"
  "  mov   x0, #1\n"
  "  ldr   x19, [x19, #0x00]\n"
  "  ret\n"
  "  .p2align 11\n"                        // v5: the M3's vectors after the claim (until winload sets its own)
  "  .globl M3BootCatchVectors\n"
  "M3BootCatchVectors:\n"
  "  .rept 16\n"
  "  b     M3BootVcatch\n"
  "  .p2align 7\n"
  "  .endr\n"
  "M3BootVcatch:\n"                       // record ESR/ELR/FAR + catch flag (Pad[2]) for the A55's watch line, then stop
  "  adrp  x1, gM3BootCtx\n"
  "  ldr   x19, [x1, :lo12:gM3BootCtx]\n"
  "  mrs   x0, esr_el1\n  str x0, [x19, #0x398]\n"
  "  mrs   x0, elr_el1\n  str x0, [x19, #0x3a0]\n"
  "  mrs   x0, far_el1\n  str x0, [x19, #0x3a8]\n"
  "  ldr   x0, [x19, #0x350]\n"
  "  add   x0, x0, #1\n"
  "  str   x0, [x19, #0x350]\n"
  "  dsb   sy\n"
  "9:\n"
  "  wfe\n"
  "  b     9b\n"
  "M3BootVfail:\n"                        // an exception before the claim
  "  adrp  x1, gM3BootCtx\n"
  "  ldr   x19, [x1, :lo12:gM3BootCtx]\n"
  "  mrs   x0, esr_el1\n  str x0, [x19, #0x398]\n"
  "  mrs   x0, elr_el1\n  str x0, [x19, #0x3a0]\n"
  "  mrs   x0, far_el1\n  str x0, [x19, #0x3a8]\n"
  "  dsb   sy\n"
  "M3BootOff:\n"
  "  mov   w0, #0x0002\n"                // PSCI CPU_OFF 0x84000002
  "  movk  w0, #0x8400, lsl #16\n"
  "  smc   #0\n"
  "2:\n"
  "  wfe\n"
  "  b     2b\n"
  "  .globl M3BootEnd\n"
  "M3BootEnd:\n"
  );

STATIC EFI_GUID  mAcpi20Guid = {
  0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 }
};

//
// v3: the parking loop M3 core 1 runs from dispatch until ExitBootServices. CPU_ON'd during
// dispatch, BEFORE the release's PSCI EL2 shim installs (PSCI_VERSION still unsupported then; after
// it, every SMC from EL1 traps into the shim and its CPU_ON never brought an M3 into our code: B3
// v1/v2). Copied into a reserved page at +0x800 with two literals (the page, M3BootEntry). MMU off:
//   +0x80 Crumb = 1 and +0x88 MPIDR when parked; the event stream on (WFE wakes ~400/s, as B2b);
//   WFE until +0x40 Go == 1, then Crumb = 2 and a jump to M3BootEntry (this image's .text, still
//   there at ExitBootServices). If Go never comes the core stays parked here (reserved memory, as B2).
//
extern UINT8  M3ParkGo[];
extern UINT8  M3ParkGoLit[];
extern UINT8  M3ParkGoEnd[];

__asm__ (
  "  .text\n"
  "  .p2align 3\n"
  "  .globl M3ParkGo\n"
  "M3ParkGo:\n"
  "  adr   x20, M3ParkGoLit\n"
  "  ldr   x19, [x20]\n"
  "  ldr   x21, [x20, #8]\n"
  "  mrs   x0, mpidr_el1\n"
  "  str   x0, [x19, #0x88]\n"
  "  mov   x0, #1\n"
  "  str   x0, [x19, #0x80]\n"
  "  dsb   sy\n"
  "  mrs   x0, cntkctl_el1\n"
  "  bic   x0, x0, #0xfc\n"
  "  orr   x0, x0, #0xf0\n"
  "  orr   x0, x0, #0x4\n"
  "  msr   cntkctl_el1, x0\n"
  "  isb\n"
  "1:\n"
  "  wfe\n"
  "  ldr   w0, [x19, #0x40]\n"
  "  cmp   w0, #1\n"
  "  b.ne  1b\n"
  "  mov   x0, #2\n"
  "  str   x0, [x19, #0x80]\n"
  "  dsb   sy\n"
  "  br    x21\n"
  "  .p2align 3\n"
  "  .globl M3ParkGoLit\n"
  "M3ParkGoLit:\n"
  "  .quad 0\n"
  "  .quad 0\n"
  "  .globl M3ParkGoEnd\n"
  "M3ParkGoEnd:\n"
  );

//
// v4 timer probe (solo builds): M3 core 2, MMU off, at dispatch. Page layout: +0x80 crumb, +0x88 CurrentEL,
// +0x90 CNTVCT, +0x98 CNTPCT, +0xA0 CNTP_CTL, +0xA8 CNTV_CTL, +0xB0 CNTP_CVAL, +0xB8 CNTKCTL. Crumb n is written after
// step n succeeded; then WFE forever (the page is reserved memory, as B2).
//
extern UINT8  M3TProbe[];
extern UINT8  M3TProbeLit[];
extern UINT8  M3TProbeEnd[];

__asm__ (
  "  .text\n"
  "  .p2align 3\n"
  "  .globl M3TProbe\n"
  "M3TProbe:\n"
  "  adr   x20, M3TProbeLit\n"
  "  ldr   x19, [x20]\n"
  "  mrs   x0, CurrentEL\n  str x0, [x19, #0x88]\n"
  "  mov   x0, #1\n  str x0, [x19, #0x80]\n  dsb sy\n"
  "  mrs   x0, cntvct_el0\n  str x0, [x19, #0x90]\n"
  "  mov   x0, #2\n  str x0, [x19, #0x80]\n  dsb sy\n"
  "  mrs   x0, cntkctl_el1\n  str x0, [x19, #0xb8]\n"
  "  mrs   x0, cntpct_el0\n  str x0, [x19, #0x98]\n"
  "  mov   x0, #3\n  str x0, [x19, #0x80]\n  dsb sy\n"
  "  mrs   x0, cntp_ctl_el0\n  str x0, [x19, #0xa0]\n"
  "  mov   x0, #4\n  str x0, [x19, #0x80]\n  dsb sy\n"
  "  msr   cntp_ctl_el0, xzr\n  isb\n"
  "  mov   x0, #5\n  str x0, [x19, #0x80]\n  dsb sy\n"
  "  mrs   x0, cntv_ctl_el0\n  str x0, [x19, #0xa8]\n"
  "  mov   x0, #6\n  str x0, [x19, #0x80]\n  dsb sy\n"
  "  msr   cntv_ctl_el0, xzr\n  isb\n"
  "  mov   x0, #7\n  str x0, [x19, #0x80]\n  dsb sy\n"
  "  mrs   x0, cntp_cval_el0\n  str x0, [x19, #0xb0]\n"
  "  mov   x0, #8\n  str x0, [x19, #0x80]\n  dsb sy\n"
  "1:\n"
  "  wfe\n"
  "  b     1b\n"
  "  .p2align 3\n"
  "  .globl M3TProbeLit\n"
  "M3TProbeLit:\n"
  "  .quad 0\n"
  "  .globl M3TProbeEnd\n"
  "M3TProbeEnd:\n"
  );

STATIC
UINT64
PsciVersion (
  VOID
  )
{
  register UINT64  x0 __asm__ ("x0") = 0x84000000u;
  register UINT64  x1 __asm__ ("x1") = 0;
  register UINT64  x2 __asm__ ("x2") = 0;
  register UINT64  x3 __asm__ ("x3") = 0;

  __asm__ __volatile__ (
    "smc #0"
    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
    :
    : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory"
    );
  return x0 & 0xFFFFFFFFu;
}

STATIC
UINT64
CpuOn (
  IN UINT64  Target,
  IN UINT64  Entry
  )
{
  register UINT64  x0 __asm__ ("x0") = 0xC4000003u;
  register UINT64  x1 __asm__ ("x1") = Target;
  register UINT64  x2 __asm__ ("x2") = Entry;
  register UINT64  x3 __asm__ ("x3") = 0;

  __asm__ __volatile__ (
    "smc #0"
    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
    :
    : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory"
    );
  return x0;
}

STATIC EFI_PHYSICAL_ADDRESS  mParkPage;
STATIC M3_BOOT_CTX           *mCtx;
STATIC EFI_PHYSICAL_ADDRESS  mWatchStack;      // v5: the A55 watcher's stack page (reserved)
STATIC UINT64                mArmTtbr0, mArmTcr, mArmSctlr, mArmMair;
STATIC EFI_EXIT_BOOT_SERVICES  mOrigEbs;
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
// v5 (after solo4: "=M3 state" printed on the M3, then nothing, not even the woa wrapper's =EBSOK): the A55 no longer
// powers off right after the claim. It watches the M3 for 15 s, rewriting the hex fields of one zone-0 line
// (=M3 watch, found when it was logged before the hand-off) about five times a second:
//   beat   the A55's own count (alive)
//   prog   the M3's progress: 1 leaving M3BootAfterEbs, 2 leaving M3BootEbsWrap (back into the woa wrapper)
//   catch  exceptions the M3 took after the claim (M3BootCatchVectors), with the first's ESR / ELR / FAR
//   pmsg   the pmsg ring's write index (woa's =EBSOK and anything else written there moves it)
// Then CPU_OFF as before (whether an immediate CPU_OFF of core 0 hurt the M3 is one of the questions).
//
STATIC UINTN  mWatchDigits[7];          // beat prog catch esr elr far pmsg: addresses of the first digit, 0 = none

STATIC
VOID
WatchHex (
  IN UINTN   At,
  IN UINT64  V,
  IN UINTN   Digits
  )
{
  UINTN  i;
  UINT8  d;

  if (At == 0) {
    return;
  }

  for (i = Digits; i-- > 0;) {
    d                                = (UINT8)(V & 0xF);
    *(volatile UINT8 *)(At + i)      = (UINT8)((d < 10) ? ('0' + d) : ('A' + d - 10));
    V                              >>= 4;
  }
}

//
// The address of Needle's first character in the zone-0 ring (StarbiteLog.h: data at SB_EVLOG_BASE + 12), or 0.
//
STATIC
UINTN
WatchRingFind (
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

VOID
M3BootA55Watch (
  M3_BOOT_CTX  *Ctx
  ) __attribute__ ((used, noreturn));

VOID
M3BootA55Watch (
  M3_BOOT_CTX  *Ctx
  )
{
  UINT64  T0;
  UINT64  Now;
  UINT64  Last;
  UINT32  Beat;
  UINTN   A;

  __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (T0));
  Last = T0;
  for (Beat = 0; ; ) {
    __asm__ __volatile__ ("isb\n mrs %0, cntvct_el0" : "=r" (Now));
    if (Now - Last < 5200000u) {        // 0.2 s at 26 MHz
      continue;
    }

    Last = Now;
    Beat++;
    for (A = (UINTN)&Ctx->Pad[0]; A < (UINTN)Ctx + 0x400; A += 64) {
      __asm__ __volatile__ ("dc civac, %0" : : "r" (A) : "memory");
    }

    __asm__ __volatile__ ("dsb sy" ::: "memory");
    WatchHex (mWatchDigits[0], Beat, 8);
    WatchHex (mWatchDigits[1], Ctx->Pad[1], 2);
    WatchHex (mWatchDigits[2], Ctx->Pad[2], 2);
    WatchHex (mWatchDigits[3], Ctx->DEsr, 8);
    WatchHex (mWatchDigits[4], Ctx->DElr, 16);
    WatchHex (mWatchDigits[5], Ctx->DFar, 16);
    __asm__ __volatile__ ("dc civac, %0\n dsb sy" : : "r" ((UINTN)0xFED14000UL) : "memory");
    WatchHex (mWatchDigits[6], *(volatile UINT32 *)0xFED14008UL, 8);
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    if (Now - T0 > 26000000ull * 15) {
      break;
    }
  }

  //
  // v7: no CPU_OFF at all (woa's PlatformBootManagerLib notes Samsung's EL3 hanging the platform after a CPU_OFF;
  // a WFI park was stable). This code lives in boot-services memory Windows will reuse, so the A55 parks in a
  // WFI loop in its reserved stack page (written at arming: WFI; B .-4 at offset 0), with its MMU and caches off:
  // UEFI maps memory 1:1, so the branch lands on the same address with the MMU off.
  //
  {
    UINT64  Sctlr;

    __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r" (Sctlr));
    Sctlr &= ~(UINT64)((1u << 0) | (1u << 2) | (1u << 12));     // M, C, I off
    __asm__ __volatile__ (
      "dsb sy\n"
      "msr sctlr_el1, %0\n"
      "isb\n"
      "ic iallu\n"
      "dsb sy\n"
      "isb\n"
      "br  %1\n"
      :
      : "r" (Sctlr), "r" ((UINT64)mWatchStack)
      : "memory"
      );
  }

  for ( ; ; ) {
    __asm__ __volatile__ ("wfi");
  }
}

STATIC
VOID
M3Progress (
  IN UINT64  P
  )
{
  UINT64  Mpidr;

  __asm__ __volatile__ ("mrs %0, mpidr_el1" : "=r" (Mpidr));
  if ((mCtx == NULL) || ((Mpidr & 0xFFFFFF) != BOOT_MPIDR)) {
    return;                             // not on the M3 (no hand-off)
  }

  mCtx->Pad[1] = P;
  __asm__ __volatile__ ("dc civac, %0\n dsb sy" : : "r" ((UINTN)&mCtx->Pad[1]) : "memory");
}

//
// A55 side. x0 = context, x1 = the parked M3's Go word (v3; v1/v2 did a PSCI CPU_ON here), x2
// unused. Returns 1 on the M3 (via M3BootEntry), 0 on the A55 when the M3 didn't claim in time
// (Go is reset to 0 then). After a claim the A55 never returns: it powers off (PSCI CPU_OFF, through
// the shim like any A55 call) without touching its stack (the M3 is using it).
//
STATIC
__attribute__ ((naked))
UINT64
M3BootHandoff (
  IN M3_BOOT_CTX  *Ctx,
  IN UINT64       GoAddr,
  IN UINT64       Unused
  )
{
  __asm__ __volatile__ (
    "stp   x19, x20, [x0, #0x00]\n"
    "stp   x21, x22, [x0, #0x10]\n"
    "stp   x23, x24, [x0, #0x20]\n"
    "stp   x25, x26, [x0, #0x30]\n"
    "stp   x27, x28, [x0, #0x40]\n"
    "stp   x29, x30, [x0, #0x50]\n"
    "str   x18, [x0, #0x340]\n"          // v4: the platform register too (Pad[0])
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
    "add   x10, x0, #0x400\n"
    "3:\n"
    "dc    civac, x9\n"
    "add   x9, x9, #64\n"
    "cmp   x9, x10\n"
    "b.lo  3b\n"
    "dsb   sy\n"
    "mov   x19, x0\n"
    "mov   w9, #1\n"                      // v3: Go = 1 for the M3 parked since dispatch (no SMC:
    "str   w9, [x1]\n"                    // after the release's PSCI shim, CPU_ON doesn't reach an M3)
    "dc    civac, x1\n"
    "dsb   sy\n"
    "sev\n"
    "str   xzr, [x19, #0x120]\n"          // Rc 0
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
    "6:\n"                                // claimed: the M3 is the boot CPU now. v5: no CPU_OFF; the
    "ldr   x9, [x19, #0x358]\n"          // A55 becomes the M3's watcher on a stack of its own (Pad[3])
    "mov   sp, x9\n"
    "mov   x0, x19\n"
    "bl    M3BootA55Watch\n"
    "8:\n"
    "wfe\n"
    "b     8b\n"
    "7:\n"                                // stay the boot CPU: undo what this function changed
    "str   wzr, [x1]\n"                   // Go = 0: a parked M3 that hasn't looked yet stays parked
    "dc    civac, x1\n"
    "dsb   sy\n"
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

  if ((Boot == NULL) || ((Mailboxes != 3) && !BOOT_BIG)) {
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
        Out[76] = BOOT_BIG ? 0 : 1;                 // efficiency class: M3 above A55 (big: all M3, all 0)
      } else if (Pass == 1) {
        if ((E[0] != GICC_TYPE) || (E[1] != GICC_LEN) || (E == Boot)) {
          continue;
        }

        CopyMem (Out, E, GICC_LEN);
        Uid = Rd32 (E + 8);
 #if BOOT_BIG
        Out[76] = 0;
        Wr32 (Out + 12, Rd32 (Out + 12) & ~1u);     // A55 0..3 and unparked M3s: disabled
        if ((Uid >= 4) && (Uid <= 7) && (Rd32 (E + 4) == Uid) && (Rd64 (E + 68) == 0x100u + (Uid - 4)) &&
            (Cores8Mailbox (Uid - 4) != 0))
        {
          Wr32 (Out + 12, Rd32 (Out + 12) | 1);     // a parked M3: enabled, parking protocol v1, its mailbox
          Wr32 (Out + 16, 1);
          Wr64 (Out + 24, Cores8Mailbox (Uid - 4));
        }
 #else
        if ((Uid >= 1) && (Uid <= 3) && !BOOT_SOLO) {
          Out[76] = 0;                              // A55 1..3: as P left them, class 0
        } else {
          Wr32 (Out + 12, Rd32 (Out + 12) & ~1u);   // A55 0, M3 0/2/3: disabled
        }
 #endif
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

//
// The stage-1 descriptor that maps Va in the current TTBR0_EL1 tables (4 KiB granule), for the
// log: 0 when unmapped. *Level gets the level of the leaf.
//
STATIC
UINT64
WalkVa (
  IN  UINT64  Va,
  OUT UINTN   *Level
  )
{
  UINT64  Ttbr0;
  UINT64  Tcr;
  UINT64  *Table;
  UINT64  Desc;
  UINTN   T0sz;
  UINTN   Lvl;

  __asm__ __volatile__ ("mrs %0, ttbr0_el1" : "=r" (Ttbr0));
  __asm__ __volatile__ ("mrs %0, tcr_el1" : "=r" (Tcr));
  T0sz   = (UINTN)(Tcr & 0x3F);
  Lvl    = (T0sz <= 24) ? 0 : ((T0sz <= 33) ? 1 : 2);
  Table  = (UINT64 *)(UINTN)(Ttbr0 & 0x0000FFFFFFFFF000ull);
  *Level = Lvl;
  for ( ; Lvl <= 3; Lvl++) {
    Desc   = Table[(Va >> (39 - 9 * Lvl)) & 0x1FF];
    *Level = Lvl;
    if ((Desc & 1) == 0) {
      return 0;
    }

    if ((Lvl == 3) || ((Desc & 2) == 0)) {
      return Desc;                        // page (level 3) or block
    }

    Table = (UINT64 *)(UINTN)(Desc & 0x0000FFFFFFFFF000ull);
  }

  return 0;
}

STATIC
VOID
LogMapping (
  IN CONST CHAR8  *What,
  IN UINT64       Va
  )
{
  UINT64  Desc;
  UINTN   Level;

  Desc = WalkVa (Va, &Level);
  SbLog (
    "=M3 boot map %a %lx: L%d desc=%lx attridx=%d sh=%d af=%d pxn=%d",
    What,
    Va,
    Level,
    Desc,
    (UINT32)((Desc >> 2) & 7),
    (UINT32)((Desc >> 8) & 3),
    (UINT32)((Desc >> 10) & 1),
    (UINT32)((Desc >> 53) & 1)
    );
}

STATIC
VOID
CleanToPoc (
  IN UINTN  Start,
  IN UINTN  End
  )
{
  UINTN  A;

  for (A = Start & ~(UINTN)63; A < End; A += 64) {
    __asm__ __volatile__ ("dc civac, %0" : : "r" (A) : "memory");
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
M3BootAfterEbs (
  VOID
  )
{
  UINT64           Ret;
  UINT64           Mpidr;
  UINT64           Ttbr0, Tcr, Sctlr, Mair;

  if (mFired) {
    return;
  }

  mFired = TRUE;
  __asm__ __volatile__ ("mrs %0, ttbr0_el1" : "=r" (Ttbr0));
  __asm__ __volatile__ ("mrs %0, tcr_el1" : "=r" (Tcr));
  __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r" (Sctlr));
  __asm__ __volatile__ ("mrs %0, mair_el1" : "=r" (Mair));
  SbLog (
    "=M3 boot at ExitBootServices: ttbr0=%lx tcr=%lx sctlr=%lx mair=%lx (armed with %lx %lx %lx %lx)",
    Ttbr0,
    Tcr,
    Sctlr,
    Mair,
    mArmTtbr0,
    mArmTcr,
    mArmSctlr,
    mArmMair
    );
  LogMapping ("entry", (UINT64)(UINTN)M3BootEntry);
  LogMapping ("context", (UINT64)(UINTN)mCtx);
  LogMapping ("stack", (UINT64)(UINTN)&Ret);
  CleanToPoc ((UINTN)mParkPage + 0x80, (UINTN)mParkPage + 0x90);
  SbLog (
    "=M3 boot: PSCI_VERSION now %lx (the shim's 10000 expected), parked M3 crumb=%ld mpidr=%lx",
    PsciVersion (),
    *(volatile UINT64 *)(UINTN)(mParkPage + 0x80),
    *(volatile UINT64 *)(UINTN)(mParkPage + 0x88)
    );
  if (*(volatile UINT64 *)(UINTN)(mParkPage + 0x80) != 1) {
    SbLog ("=M3 boot: M3 core 1 is not parked; staying on A55 core 0 (Windows boots as before)");
    return;
  }

  if (!PrepareMadt ()) {
    SbLog ("=M3 boot: staying on A55 core 0 (Windows boots as before)");
    return;
  }

  SbLog ("=M3 boot v4: handing the boot CPU to the parked M3 core 1 after ExitBootServices (Go, no SMC)");
  ZeroMem (mCtx, sizeof (*mCtx));
  mCtx->Timeout = TIMEOUT_TICKS;
  mCtx->Pad[3]  = (UINT64)(mWatchStack + EFI_PAGE_SIZE - 16);
  {
    STATIC CONST CHAR8  *Fields[] = { " beat ", " prog ", " catch ", " esr ", " elr ", " far ", " pmsg " };
    STATIC CONST CHAR8  Line[]    = "=M3 watch beat 00000000 prog 00 catch 00 esr 00000000 elr 0000000000000000 far 0000000000000000 pmsg 00000000";
    UINTN               Base;
    UINTN               k;
    CHAR8               *Hit;

    SbLog ("%a", Line);
    Base = WatchRingFind (Line);
    for (k = 0; k < ARRAY_SIZE (Fields); k++) {
      Hit             = AsciiStrStr (Line, Fields[k]);
      mWatchDigits[k] = ((Base != 0) && (Hit != NULL)) ? Base + (UINTN)(Hit - Line) + AsciiStrLen (Fields[k]) : 0;
    }
  }
  gM3BootCtx    = (UINT64)(UINTN)mCtx;
  // (no WriteBackDataCache: edk2's ARM version is ASSERT(FALSE) and does nothing; what the M3 reads with its MMU
  // off is cleaned to PoC by address below, and everything else is coherent between the clusters, B3a v2)
  //
  // What the M3 reads with its MMU off, to the point of coherency by address: its code (this
  // image's .text, M3BootVectors..M3BootEnd) and gM3BootCtx. M3BootHandoff cleans the context
  // (0x400 bytes, the breadcrumb line included) after filling it.
  //
  CleanToPoc ((UINTN)M3BootVectors, (UINTN)M3BootEnd);
  CleanToPoc ((UINTN)&gM3BootCtx, (UINTN)&gM3BootCtx + sizeof (gM3BootCtx));
  __asm__ __volatile__ ("ic iallu\n dsb sy\n isb" ::: "memory");

  Ret = M3BootHandoff (mCtx, (UINT64)(UINTN)(mParkPage + 0x40), 0);
  if (Ret == 1) {
    //
    // On M3 core 1 now; A55 core 0 is powering off. The commit point.
    //
    CopyMem (mMadt, mNewMadt, mMadtLen);
    __asm__ __volatile__ ("mrs %0, mpidr_el1" : "=r" (Mpidr));
    SbLog ("=M3 boot: now on mpidr=%lx (el %ld), MADT committed (UID %d first); winload continues here", Mpidr, mCtx->DEl, BOOT_UID);
    {
      UINT64  Daif, Isr, Vct;

      __asm__ __volatile__ ("mrs %0, daif" : "=r" (Daif));
      __asm__ __volatile__ ("mrs %0, isr_el1" : "=r" (Isr));
      __asm__ __volatile__ ("isb\n mrs %0, cntvct_el0" : "=r" (Vct));
      SbLog (
        "=M3 state: daif=%lx isr=%lx vct=%lx gicc ctlr=%x pmr=%x hppir=%x",
        Daif,
        Isr,
        Vct,
        *(volatile UINT32 *)0x10102000UL,
        *(volatile UINT32 *)0x10102004UL,
        *(volatile UINT32 *)0x10102018UL
        );
    }

    __asm__ __volatile__ ("msr vbar_el1, %0\n isb" : : "r" ((UINT64)(UINTN)M3BootCatchVectors) : "memory");
    //
    // v7: Windows calls PSCI early; from the M3 an SMC goes straight to Samsung's EL3 (no woa shim at EL2 there).
    // One PSCI_VERSION from here shows whether EL3 answers the M3 at all.
    //
    SbLog ("=M3 psci: PSCI_VERSION from the M3 ...");
    SbLog ("=M3 psci: returned %lx", PsciVersion ());
    M3Progress (1);
 #if BOOT_BIG
    {
      UINT32  Off;
      UINT8   *E;

      for (Off = 44; Off + 2 <= mMadtLen && mMadt[Off + 1] != 0; Off += mMadt[Off + 1]) {
        E = mMadt + Off;
        if ((E[0] == GICC_TYPE) && (E[1] == GICC_LEN)) {
          SbLog ("=M3 boot big MADT uid=%d if=%d flags=%x mpidr=%lx park v%d %lx eff=%d", Rd32 (E + 8), Rd32 (E + 4),
            Rd32 (E + 12), Rd64 (E + 68), Rd32 (E + 16), Rd64 (E + 24), E[76]);
        }
      }

      SbLog ("=M3 boot big: %d DSDT _MAT copies synced to the new MADT", Cores8SyncMat (mMadt, mMadtLen));
    }
 #endif
    return;
  }

  CleanToPoc ((UINTN)&mCtx->DStage, (UINTN)&mCtx->DFar + 8);     // the M3's own line
  CleanToPoc ((UINTN)mParkPage + 0x80, (UINTN)mParkPage + 0x90);
  SbLog (
    "=M3 boot: hand-off not taken (park crumb=%ld, state=%d, stage=%ld, el=%ld, mpidr=%lx, esr=%lx elr=%lx far=%lx); staying on A55 core 0, MADT untouched",
    *(volatile UINT64 *)(UINTN)(mParkPage + 0x80),
    mCtx->State,
    mCtx->DStage,
    mCtx->DEl,
    mCtx->DMpidr,
    mCtx->DEsr,
    mCtx->DElr,
    mCtx->DFar
    );
}

//
// v4, solo builds: the timer probe on M3 core 2 (see M3TProbe). Never hangs the A55: one CPU_ON, then 200 ms of
// polling; the core stays wherever it stopped (reserved page, or an EL2 trap with nobody to answer).
//
STATIC
VOID
M3TimerProbe (
  VOID
  )
{
  volatile UINT32       *Pmu = (volatile UINT32 *)0x14060000UL;
  EFI_PHYSICAL_ADDRESS  Page;
  volatile UINT64       *P;
  UINT64                *Lit;
  UINT64                Rc;
  UINT64                Vct;
  UINT64                Pct;
  UINTN                 i;
  UINTN                 Ms;

  Page = 0xFFFFFFFFu;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 1, &Page))) {
    SbLog ("=M3 tprobe: no page");
    return;
  }

  ZeroMem ((VOID *)(UINTN)Page, EFI_PAGE_SIZE);
  CopyMem ((VOID *)(UINTN)(Page + 0x800), M3TProbe, (UINTN)(M3TProbeEnd - M3TProbe));
  Lit    = (UINT64 *)(UINTN)(Page + 0x800 + (UINTN)(M3TProbeLit - M3TProbe));
  Lit[0] = Page;
  CleanToPoc ((UINTN)Page, (UINTN)Page + EFI_PAGE_SIZE);
  __asm__ __volatile__ ("ic iallu\n dsb sy\n isb" ::: "memory");
  for (i = 0; i < ARRAY_SIZE (mBootPmu); i++) {
    Pmu[mBootPmu[i].Offset / 4] = (Pmu[mBootPmu[i].Offset / 4] & ~mBootPmu[i].Mask) | mBootPmu[i].Value;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
  }

  P  = (volatile UINT64 *)(UINTN)Page;
  Rc = CpuOn (0x102, Page + 0x800);
  for (Ms = 0; Ms < 200; Ms++) {
    CleanToPoc ((UINTN)Page + 0x80, (UINTN)Page + 0xC0);
    if (P[0x80 / 8] == 8) {
      break;
    }

    MicroSecondDelay (1000);
  }

  __asm__ __volatile__ ("isb\n mrs %0, cntvct_el0" : "=r" (Vct));
  __asm__ __volatile__ ("isb\n mrs %0, cntpct_el0" : "=r" (Pct));
  SbLog (
    "=M3 tprobe core 2: CPU_ON rc=%lx crumb=%ld (8 = all) el=%lx vct=%lx pct=%lx p_ctl=%lx v_ctl=%lx p_cval=%lx kctl=%lx; A55 now vct=%lx pct=%lx",
    Rc,
    P[0x80 / 8],
    P[0x88 / 8] >> 2,
    P[0x90 / 8],
    P[0x98 / 8],
    P[0xA0 / 8],
    P[0xA8 / 8],
    P[0xB0 / 8],
    P[0xB8 / 8],
    Vct,
    Pct
    );
  MicroSecondDelay (50000);
}

//
// v4: MSVC's outline-atomics helpers (one copy per image) choose LSE once, from ID_AA64ISAR0_EL1 of the CPU that
// runs their first call, and keep the choice in a word at the start of a data page: bit 31 initialised, bit 0 LSE.
// winload.efi (md5 116bff41: init stub at RVA 0x3B10, latch at RVA 0x210000) latches on the A55 before
// ExitBootServices, so after the hand-off its interlocked helpers would run LSE on the M3 (no LSE: undefined
// instruction). ntoskrnl and the drivers latch on the M3 themselves (their first call runs there). So before the
// real ExitBootServices every EfiLoaderCode image (winload, bootmgr) gets its latch set to 0x80000000: initialised,
// no LSE; LL/SC works on both cores. Found by the init stub's code: MOVZ X16,#0x8000,LSL #16; ADRP X17,latch;
// STR W16,[X17]; MOVZ X16,#0; MRS X16,CurrentEL.
//
STATIC
VOID
M3BootLseGuard (
  VOID
  )
{
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  UINTN                      i;
  EFI_LOADED_IMAGE_PROTOCOL  *Li;
  UINT8                      *Base;
  UINT64                     Size;
  UINT64                     Off;
  UINT32                     W[5];
  UINT64                     Pc;
  INT64                      Imm;
  UINT64                     Latch;
  UINT32                     Old;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (i = 0; i < Count; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiLoadedImageProtocolGuid, (VOID **)&Li)) ||
        (Li->ImageCodeType != EfiLoaderCode) || (Li->ImageBase == NULL) || (Li->ImageSize < 0x1000))
    {
      continue;
    }

    Base = (UINT8 *)Li->ImageBase;
    Size = Li->ImageSize;
    for (Off = 0; Off + 20 <= Size; Off += 4) {
      CopyMem (W, Base + Off, sizeof (W));
      if ((W[0] != 0xD2B00010u) || ((W[1] & 0x9F00001Fu) != 0x90000011u) || (W[2] != 0xB9000230u) ||
          (W[3] != 0xD2800010u) || (W[4] != 0xD5384250u))
      {
        continue;
      }

      Pc  = (UINT64)(UINTN)(Base + Off + 4);
      Imm = (INT64)(((W[1] >> 29) & 3) | (((W[1] >> 5) & 0x7FFFF) << 2));
      if (Imm & (1 << 20)) {
        Imm -= (1 << 21);
      }

      Latch = (Pc & ~0xFFFull) + (UINT64)(Imm << 12);
      if ((Latch < (UINT64)(UINTN)Base) || (Latch + 4 > (UINT64)(UINTN)Base + Size)) {
        SbLog ("=M3 boot lse: image %lx stub +%lx: latch %lx outside the image, left", (UINT64)(UINTN)Base, Off, Latch);
        continue;
      }

      Old                                   = *(volatile UINT32 *)(UINTN)Latch;
      *(volatile UINT32 *)(UINTN)Latch = 0x80000000u;
      CleanToPoc ((UINTN)Latch, (UINTN)Latch + 4);
      SbLog ("=M3 boot lse: image %lx size %lx stub +%lx latch +%lx: %08x -> 80000000", (UINT64)(UINTN)Base, Size, Off, Latch - (UINT64)(UINTN)Base, Old);
    }
  }

  FreePool (Handles);
}

//
// v4: gBS->ExitBootServices while armed. The real call first (all of UEFI's teardown on the A55); on success the
// hand-off, so the M3 returns from here into the caller (the woa wrapper, then winload). Failures (a stale map
// key: winload just retries) pass through untouched.
//
STATIC
EFI_STATUS
EFIAPI
M3BootEbsWrap (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  EFI_STATUS  Status;

  if (!mFired) {
    SbLog ("=M3 boot v6: ExitBootServices wrapper called from %lx (winload = outermost, as intended)", (UINT64)(UINTN)__builtin_return_address (0));
    M3BootLseGuard ();                  // while boot services still exist; idempotent if winload retries
  }

  Status = mOrigEbs (ImageHandle, MapKey);
  if (EFI_ERROR (Status) || mFired) {
    return Status;
  }

  mSbAfterEbs = TRUE;                   // gBS is zeroed now: the log must not use it
  SbLog ("=M3 boot v4: ExitBootServices returned %r on the A55; hand-off now", Status);
  M3BootAfterEbs ();
  M3Progress (2);                       // on the M3 only: about to return into the woa wrapper
  return Status;
}

//
// v6: wrap gBS->ExitBootServices at ReadyToBoot, after the release BdsDxe installed its own hook (BdsDxe +0x428C,
// early in BDS), so ours is the OUTERMOST: winload -> ours -> woa's -> the real one. The woa hook's post-EBS code
// (=EBSOK, its latches) then runs on the A55 before the hand-off, and the M3 returns straight into winload. (v4/v5
// wrapped at dispatch: ours was inside woa's, the M3 returned into woa's hook and never printed =EBSOK.)
//
STATIC EFI_EVENT  mReadyToBoot;

STATIC
VOID
EFIAPI
M3BootOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  if (gBS->ExitBootServices == M3BootEbsWrap) {
    return;                             // ReadyToBoot again (another boot option): already wrapped
  }

  mOrigEbs              = gBS->ExitBootServices;
  gBS->ExitBootServices = M3BootEbsWrap;
  gBS->Hdr.CRC32        = 0;
  Status                = gBS->CalculateCrc32 ((UINT8 *)gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  SbLog ("=M3 boot v6: ReadyToBoot: ExitBootServices %lx wrapped by ours %lx (crc %r)", (UINT64)(UINTN)mOrigEbs, (UINT64)(UINTN)M3BootEbsWrap, Status);
}

VOID
M3BootArm (
  VOID
  )
{
  volatile UINT32       *Pmu = (volatile UINT32 *)0x14060000UL;
  EFI_PHYSICAL_ADDRESS  CtxPage;
  EFI_STATUS            Status;
  UINT64                Version;
  UINT64                Rc;
  UINT64                *Lit;
  UINT32                Before;
  UINT32                After;
  UINTN                 i;
  UINTN                 Ms;

  if (((UINTN)M3BootVectors & 0x7FF) != 0) {
    SbLog ("=M3 boot: vectors at %lx are not 2 KiB aligned, not armed", (UINT64)(UINTN)M3BootVectors);
    return;
  }

  CtxPage = 0xFFFFFFFFu;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 1, &CtxPage))) {   // v5: the M3 writes it after EBS
    SbLog ("=M3 boot: no context page, not armed");
    return;
  }

  mCtx = (M3_BOOT_CTX *)(UINTN)CtxPage;
  mWatchStack = 0xFFFFFFFFu;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 1, &mWatchStack))) {
    SbLog ("=M3 boot: no watch stack page, not armed");
    return;
  }

  ZeroMem ((VOID *)(UINTN)mWatchStack, EFI_PAGE_SIZE);
  ((UINT32 *)(UINTN)mWatchStack)[0] = 0xD503207Fu;           // WFI
  ((UINT32 *)(UINTN)mWatchStack)[1] = 0x17FFFFFFu;           // B .-4
  CleanToPoc ((UINTN)mWatchStack, (UINTN)mWatchStack + 64);
  __asm__ __volatile__ ("ic iallu\n dsb sy\n isb" ::: "memory");

  //
  // v3: park M3 core 1 now, while PSCI calls still go straight to Samsung's EL3 (PSCI_VERSION
  // unsupported = 0xFFFFFFFF; the release's shim, once installed, answers 0x10000 and its CPU_ON
  // never reached our code on an M3).
  //
  Version = PsciVersion ();
  if (Version != 0xFFFFFFFFu) {
    SbLog ("=M3 boot: PSCI_VERSION %lx at dispatch: the shim is already installed, not armed", Version);
    return;
  }

  mParkPage = 0xFFFFFFFFu;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 1, &mParkPage))) {
    SbLog ("=M3 boot: no park page, not armed");
    return;
  }

  ZeroMem ((VOID *)(UINTN)mParkPage, EFI_PAGE_SIZE);
  CopyMem ((VOID *)(UINTN)(mParkPage + 0x800), M3ParkGo, (UINTN)(M3ParkGoEnd - M3ParkGo));
  Lit    = (UINT64 *)(UINTN)(mParkPage + 0x800 + (UINTN)(M3ParkGoLit - M3ParkGo));
  Lit[0] = mParkPage;
  Lit[1] = (UINT64)(UINTN)M3BootEntry;
  CleanToPoc ((UINTN)mParkPage, (UINTN)mParkPage + EFI_PAGE_SIZE);
  __asm__ __volatile__ ("ic iallu\n dsb sy\n isb" ::: "memory");
  for (i = 0; i < ARRAY_SIZE (mBootPmu); i++) {
    Before                      = Pmu[mBootPmu[i].Offset / 4];
    Pmu[mBootPmu[i].Offset / 4] = (Before & ~mBootPmu[i].Mask) | mBootPmu[i].Value;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    After = Pmu[mBootPmu[i].Offset / 4];
    SbLog ("=M3 pmu %04x %08x -> %08x%a", mBootPmu[i].Offset, Before, After, ((After & mBootPmu[i].Mask) == mBootPmu[i].Value) ? "" : " NOT SET");
  }

  if (BOOT_SOLO) {
    M3TimerProbe ();                    // v4: M3 core 2, before M3 core 1 parks
  }

  Rc = CpuOn (BOOT_TARGET, mParkPage + 0x800);
  for (Ms = 0; Ms < 200; Ms++) {
    CleanToPoc ((UINTN)mParkPage + 0x80, (UINTN)mParkPage + 0x90);
    if (*(volatile UINT64 *)(UINTN)(mParkPage + 0x80) == 1) {
      break;
    }

    MicroSecondDelay (1000);
  }

  SbLog (
    "=M3 boot: M3 core 1 parked at dispatch: PSCI_VERSION %lx, CPU_ON rc=%lx, crumb=%ld after %d ms, mpidr=%lx, park page %lx",
    Version,
    Rc,
    *(volatile UINT64 *)(UINTN)(mParkPage + 0x80),
    Ms,
    *(volatile UINT64 *)(UINTN)(mParkPage + 0x88),
    mParkPage
    );
  if ((Rc != 0) || (*(volatile UINT64 *)(UINTN)(mParkPage + 0x80) != 1)) {
    SbLog ("=M3 boot: M3 core 1 didn't park, not armed");
    return;
  }

  __asm__ __volatile__ ("mrs %0, ttbr0_el1" : "=r" (mArmTtbr0));
  __asm__ __volatile__ ("mrs %0, tcr_el1" : "=r" (mArmTcr));
  __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r" (mArmSctlr));
  __asm__ __volatile__ ("mrs %0, mair_el1" : "=r" (mArmMair));

  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, M3BootOnReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &mReadyToBoot);
  SbLog (
    "=M3 boot v7 armed (%a): entry %lx (vectors %lx..%lx in this image), context %lx, ExitBootServices wrapped at ReadyToBoot (now %lx, event %r)",
    BOOT_SOLO ? "solo: M3 1 only" : (BOOT_BIG ? "big: M3 1 + parked M3s, no A55" : "M3 1 + A55 1..3"),
    (UINT64)(UINTN)M3BootEntry,
    (UINT64)(UINTN)M3BootVectors,
    (UINT64)(UINTN)M3BootEnd,
    (UINT64)(UINTN)mCtx,
    (UINT64)(UINTN)gBS->ExitBootServices,
    Status
    );
}
#endif
